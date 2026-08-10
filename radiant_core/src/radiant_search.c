/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_search.c - wildcard search: the sweep, the seen cache, and scan mode.
 *
 * Provenance: clean-room. Written from radiant_core/include/radiant_core/radiant_search.h (which
 * carries the reasoning), docs/ant-radio-link.md, docs/spike-a-results.md and
 * docs/spike-b-results.md for the measured search configuration, the noise
 * floor of a three-byte matcher and the RXMATCH recovery of devnum_lo, and from
 * the free ANT Message Protocol and Usage Rev 5.1 (D00000652) for the wildcard
 * channel-ID convention and the 25 s default search timeout. Nothing here
 * derives from sdk-ant, from libant.a, from disassembly of any binary, or from
 * any adopter-gated ANT+ device profile document. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * Reading order
 * ---------------------------------------------------------------------------
 * The header is where the design argument lives. This file is deliberately
 * dull, and the three places where it is not are marked:
 *
 *   build_set()       - the only place a filter address is constructed, and it
 *                       goes through radiant_frame_addr() rather than laying out
 *                       three bytes by hand;
 *   handle_ok()       - devnum_lo comes from filter_lo[filter_index] and from
 *                       nowhere else;
 *   radiant_search_on_rx()- RADIANT_RADIO_STATUS_CRC_FAIL is counted and dropped.
 *
 * NO REGISTER ARITHMETIC ANYWHERE. The BALEN < 4 packing rule - BASE0 =
 * (lsb-first packing) << (8 * (4 - BALEN)), which the docs originally got wrong
 * and which cost a bench run - is a backend's problem, and radiant_frame.h's
 * radiant_addr_pack_leading() / radiant_addr_pack_trailing() are where it is solved
 * once. This module speaks on-air byte order, exactly as struct radiant_rx_filter
 * does, and a second implementation of that arithmetic here would be one more
 * than the number that can be right.
 */

#include <string.h>

#include <radiant_core/radiant_search.h>

#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_radio_hal.h>

/* ---------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------------
 */

static bool inst_ok(const struct radiant_search *s)
{
	return s != NULL && s->inited;
}

/*
 * ANT's wildcard convention: zero is "any" [rev5.1].
 *
 * The device type is compared with the pairing bit masked off. A sensor in
 * pairing mode sets that bit, and a search that stopped matching at exactly the
 * moment the user pressed the pairing button would be a memorable bug. The raw
 * byte still reaches the caller in the result.
 */
static bool id_match(const struct radiant_search_id_filter *want,
		     const struct radiant_channel_id *id)
{
	if (want->device_number != 0u && want->device_number != id->device_number) {
		return false;
	}
	if (want->device_type != 0u &&
	    (uint8_t)(want->device_type & RADIANT_DEVICE_TYPE_MASK) !=
		    (uint8_t)(id->device_type & RADIANT_DEVICE_TYPE_MASK)) {
		return false;
	}
	if (want->trans_type != 0u && want->trans_type != id->trans_type) {
		return false;
	}
	return true;
}

static bool seen_live(const struct radiant_search *s, const struct radiant_search_seen *e,
		      radiant_time_t now)
{
	if (!e->valid) {
		return false;
	}
	/* Written as "now is not yet past the expiry" rather than
	 * "now - t_seen < lifetime" so that a now BEFORE t_seen - which a test
	 * can produce and a real timebase cannot - keeps the entry rather than
	 * wrapping the subtraction into a very large positive number. */
	return now <= e->t_seen + (radiant_time_t)s->cfg.seen_lifetime_us;
}

/* ---------------------------------------------------------------------------
 * The sweep
 * ---------------------------------------------------------------------------
 */

/*
 * Set k holds the n_filters consecutive devnum_lo values starting at
 * k * n_filters. Consecutive rather than interleaved for one reason that
 * matters: it makes "which set would catch device X" a division, which is what
 * both the seen cache and a named-device search need in order to jump the
 * queue. Any other assignment would need a table.
 *
 * The last set is short whenever n_filters does not divide 256. Nothing in
 * view has such a capability - 8 and 2 both divide it - but a window with
 * fewer filters is legal and free, whereas a window with a duplicate filter
 * would make filter_index ambiguous, which is the one thing this module cannot
 * tolerate.
 */
static void build_set(struct radiant_search *s, uint16_t set)
{
	uint32_t base = (uint32_t)set * (uint32_t)s->n_filters;
	uint8_t n = 0;

	for (uint8_t j = 0; j < s->n_filters; j++) {
		struct radiant_channel_id id;
		uint8_t addr[RADIANT_FRAME_ADDR_MAX];
		uint32_t lo = base + (uint32_t)j;
		int rc;

		if (lo >= RADIANT_SEARCH_LO_VALUES) {
			break;
		}

		memset(&id, 0, sizeof(id));
		id.device_number = (uint16_t)lo;

		rc = radiant_frame_addr(RADIANT_FRAME_CFG_SEARCH, s->cfg.net_addr, &id,
				    addr, sizeof(addr));
		if (rc != (int)RADIANT_FRAME_ADDR_LEN_SEARCH) {
			/* Unreachable: the configuration is a compile-time
			 * constant and the buffer is the maximum size. Skipping
			 * rather than asserting keeps a would-be silent
			 * duplicate filter out of the window. */
			continue;
		}

		memset(&s->filters[n], 0, sizeof(s->filters[n]));
		memcpy(s->filters[n].addr, addr, (size_t)rc);
		s->filters[n].addr_len = (uint8_t)rc;
		s->filter_lo[n] = (uint8_t)lo;
		n++;
	}

	s->cur_set = set;
	s->cur_n_filters = n;
	s->set_valid = (n > 0u);
	s->set_dwell_us = 0u;
}

/* Queue a set ahead of the round-robin cursor. Duplicates are dropped: two
 * channels re-acquiring the same sensor should not make the sweep listen to its
 * set twice in a row while every other set waits. */
static void push_steer(struct radiant_search *s, uint16_t set)
{
	if (set >= s->n_sets) {
		return;
	}
	for (uint8_t i = 0; i < s->n_steer; i++) {
		if (s->steer[i] == set) {
			return;
		}
	}
	if (s->n_steer >= (uint8_t)RADIANT_SEARCH_SEEN_ENTRIES) {
		return;
	}
	s->steer[s->n_steer] = set;
	s->n_steer++;
}

static void select_next_set(struct radiant_search *s)
{
	uint16_t set;
	bool steered = false;

	if (s->n_steer > 0u) {
		set = s->steer[0];
		for (uint8_t i = 1; i < s->n_steer; i++) {
			s->steer[i - 1u] = s->steer[i];
		}
		s->n_steer--;
		steered = true;
		s->stats.cache_steers++;
	} else {
		set = s->next_set;
		s->next_set = (uint16_t)(s->next_set + 1u);
		if (s->next_set >= s->n_sets) {
			s->next_set = 0u;
			s->stats.sweeps++;
		}
	}

	build_set(s, set);
	s->cur_from_cache = steered;
	s->stats.sets_advanced++;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

void radiant_search_cfg_default(struct radiant_search_cfg *cfg)
{
	if (cfg == NULL) {
		return;
	}
	memset(cfg, 0, sizeof(*cfg));
	cfg->net_addr[0] = RADIANT_NET_ADDR_ANT_PLUS_0;
	cfg->net_addr[1] = RADIANT_NET_ADDR_ANT_PLUS_1;
	cfg->rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	cfg->dwell_us = RADIANT_SEARCH_DWELL_DEFAULT_US;
	cfg->seen_lifetime_us = RADIANT_SEARCH_SEEN_LIFETIME_US;
	cfg->min_rssi_dbm = 0;
	cfg->report_crc_fail = false;
}

int radiant_search_init(struct radiant_search *s, const struct radiant_search_cfg *cfg,
		    const struct radiant_search_cbs *cbs, void *user)
{
	const struct radiant_radio_caps *caps;
	const struct radiant_pkt_format *fmt;
	uint8_t nf;

	if (s == NULL || cfg == NULL) {
		return RADIANT_SEARCH_EINVAL;
	}
	if (cfg->rf_index > RADIANT_RF_INDEX_MAX || cfg->dwell_us == 0u ||
	    cfg->seen_lifetime_us == 0u) {
		return RADIANT_SEARCH_EINVAL;
	}

	fmt = radiant_frame_format(RADIANT_FRAME_CFG_SEARCH);
	if (fmt == NULL) {
		return RADIANT_SEARCH_EINVAL;
	}

	caps = radiant_radio_caps_get();
	if (caps == NULL || caps->max_filters == 0u) {
		return RADIANT_SEARCH_EINVAL;
	}
	/*
	 * The flag says the backend can match "any device number" with one
	 * filter, which would collapse the sweep to a single window. There is
	 * no field in struct radiant_rx_filter in which to ask for that, so there
	 * is nothing this module could arm, and quietly sweeping anyway would
	 * hide the HAL change the flag needs. No planned backend sets it.
	 */
	if (caps->filter_wildcard_dev) {
		return RADIANT_SEARCH_ENOTSUP;
	}
	/* A backend that cannot carry the search body cannot search. Twelve
	 * bytes [devnum_hi][dtype][ttype][ctrl][d0..d7] is the whole of it. */
	if (caps->max_body_len < fmt->body_len) {
		return RADIANT_SEARCH_ENOTSUP;
	}

	nf = caps->max_filters;
	if (nf > (uint8_t)RADIANT_SEARCH_MAX_FILTERS) {
		nf = (uint8_t)RADIANT_SEARCH_MAX_FILTERS;
	}

	memset(s, 0, sizeof(*s));
	s->cfg = *cfg;
	if (cbs != NULL) {
		s->cbs = *cbs;
	}
	s->user = user;
	s->fmt = fmt;
	s->n_filters = nf;
	/*
	 * Coverage arithmetic, and the one line in this file that must never
	 * become a constant: 8 filters -> 32 sets on nRF, 2 filters -> 128 sets
	 * on EFR32/RAIL, from the same expression.
	 */
	s->n_sets = (uint16_t)((RADIANT_SEARCH_LO_VALUES + nf - 1u) / nf);
	s->inited = true;

	return RADIANT_SEARCH_OK;
}

uint8_t radiant_search_filters_per_window(const struct radiant_search *s)
{
	return inst_ok(s) ? s->n_filters : 0u;
}

uint16_t radiant_search_sets(const struct radiant_search *s)
{
	return inst_ok(s) ? s->n_sets : 0u;
}

uint64_t radiant_search_sweep_us(const struct radiant_search *s)
{
	if (!inst_ok(s)) {
		return 0u;
	}
	return (uint64_t)s->n_sets * (uint64_t)s->cfg.dwell_us;
}

uint16_t radiant_search_set_for_devnum(const struct radiant_search *s, uint16_t devnum)
{
	if (!inst_ok(s)) {
		return RADIANT_SEARCH_SETS_NONE;
	}
	return (uint16_t)((devnum & 0xFFu) / s->n_filters);
}

const struct radiant_search_stats *radiant_search_get_stats(const struct radiant_search *s)
{
	return (s == NULL) ? NULL : &s->stats;
}

/* ---------------------------------------------------------------------------
 * Channels joining and leaving
 * ---------------------------------------------------------------------------
 */

int radiant_search_begin(struct radiant_search *s, uint8_t channel,
		     enum radiant_search_mode mode,
		     const struct radiant_search_id_filter *want, radiant_time_t now,
		     uint32_t timeout_us)
{
	struct radiant_search_chan *ch;

	if (!inst_ok(s) || want == NULL) {
		return RADIANT_SEARCH_EINVAL;
	}
	if (channel >= (uint8_t)RADIANT_SEARCH_MAX_CHANNELS) {
		return RADIANT_SEARCH_ENOSPC;
	}
	if (mode != RADIANT_SEARCH_MODE_ACQUIRE && mode != RADIANT_SEARCH_MODE_SCAN) {
		return RADIANT_SEARCH_EINVAL;
	}

	ch = &s->chans[channel];
	ch->active = true;
	ch->mode = mode;
	ch->want = *want;
	ch->deadline = (timeout_us == RADIANT_SEARCH_TIMEOUT_NONE)
			       ? RADIANT_TIME_NEVER
			       : now + (radiant_time_t)timeout_us;

	/*
	 * Jump the queue where we can. Two cases, and they are the whole reason
	 * re-acquisition is fast:
	 *
	 *   - the channel named a device number, so devnum_lo is known outright
	 *     and exactly one set can ever catch it;
	 *   - the seen cache holds a device matching the filter, so the same is
	 *     true until the entry expires.
	 *
	 * Either way this only reorders the sweep. If the guess is wrong the
	 * round robin covers everything anyway, one dwell later than it would
	 * have - which is the correct price for a guess that is usually right.
	 */
	if (want->device_number != 0u) {
		push_steer(s, radiant_search_set_for_devnum(s, want->device_number));
	} else {
		struct radiant_channel_id hit;

		if (radiant_search_seen_lookup(s, want, now, &hit)) {
			push_steer(s, radiant_search_set_for_devnum(s, hit.device_number));
		}
	}

	return RADIANT_SEARCH_OK;
}

int radiant_search_end(struct radiant_search *s, uint8_t channel)
{
	if (!inst_ok(s)) {
		return RADIANT_SEARCH_EINVAL;
	}
	if (channel >= (uint8_t)RADIANT_SEARCH_MAX_CHANNELS) {
		return RADIANT_SEARCH_EINVAL;
	}
	if (!s->chans[channel].active) {
		return RADIANT_SEARCH_ENOENT;
	}
	memset(&s->chans[channel], 0, sizeof(s->chans[channel]));
	return RADIANT_SEARCH_OK;
}

bool radiant_search_is_searching(const struct radiant_search *s, uint8_t channel)
{
	if (!inst_ok(s) || channel >= (uint8_t)RADIANT_SEARCH_MAX_CHANNELS) {
		return false;
	}
	return s->chans[channel].active;
}

uint8_t radiant_search_n_searching(const struct radiant_search *s)
{
	uint8_t n = 0;

	if (!inst_ok(s)) {
		return 0u;
	}
	for (uint8_t c = 0; c < (uint8_t)RADIANT_SEARCH_MAX_CHANNELS; c++) {
		if (s->chans[c].active) {
			n++;
		}
	}
	return n;
}

enum radiant_search_mode radiant_search_chan_mode(const struct radiant_search *s,
						 uint8_t channel)
{
	if (!inst_ok(s) || channel >= (uint8_t)RADIANT_SEARCH_MAX_CHANNELS ||
	    !s->chans[channel].active) {
		return RADIANT_SEARCH_MODE_ACQUIRE;
	}
	return s->chans[channel].mode;
}

/* ---------------------------------------------------------------------------
 * The pump
 * ---------------------------------------------------------------------------
 */

int radiant_search_window(struct radiant_search *s, radiant_time_t t_earliest,
		      struct radiant_search_window *out)
{
	uint32_t remaining;

	if (!inst_ok(s) || out == NULL) {
		return RADIANT_SEARCH_EINVAL;
	}
	if (s->op != 0u) {
		return RADIANT_SEARCH_ESTATE;
	}
	if (radiant_search_n_searching(s) == 0u) {
		return RADIANT_SEARCH_ENOENT;
	}

	/*
	 * Advance only when the dwell is fully credited. A window the scheduler
	 * cut short to service a tracked channel leaves the same set selected
	 * for the remainder, so the "certain within one sweep" guarantee
	 * survives fragmentation instead of degrading silently into "likely".
	 */
	if (!s->set_valid || s->set_dwell_us >= s->cfg.dwell_us) {
		select_next_set(s);
	}
	if (!s->set_valid) {
		return RADIANT_SEARCH_EINVAL;
	}

	remaining = s->cfg.dwell_us - s->set_dwell_us;

	memset(out, 0, sizeof(*out));
	out->fmt = s->fmt;
	out->rf_index = s->cfg.rf_index;
	out->filters = s->filters;
	out->n_filters = s->cur_n_filters;
	out->t_open = t_earliest;
	out->t_close = t_earliest + (radiant_time_t)remaining;
	/*
	 * RADIANT_RX_STOP_ON_FIRST is never set, and the HAL forbids it here. More
	 * than one master may transmit inside one search window, and stopping
	 * at the first would hand one channel an acquisition while costing
	 * every other searching channel the rest of the dwell - which is the
	 * opposite of "one sweep serves every searching channel".
	 */
	out->flags = s->cfg.report_crc_fail ? RADIANT_RX_REPORT_CRC_FAIL : 0u;
	out->set_index = s->cur_set;
	out->from_cache = s->cur_from_cache;

	s->stats.windows++;
	return RADIANT_SEARCH_OK;
}

void radiant_search_armed(struct radiant_search *s, uint32_t op, radiant_time_t t_open,
		      radiant_time_t t_close)
{
	if (!inst_ok(s)) {
		return;
	}
	s->op = op;
	s->t_open = t_open;
	s->t_close = t_close;
}

void radiant_search_arm_failed(struct radiant_search *s)
{
	if (!inst_ok(s)) {
		return;
	}
	s->op = 0u;
}

bool radiant_search_owns_op(const struct radiant_search *s, uint32_t op)
{
	return inst_ok(s) && op != 0u && s->op == op;
}

/*
 * A CRC-valid frame. The two facts this function exists to get right are both
 * about where a byte came from.
 */
static void handle_ok(struct radiant_search *s, const struct radiant_rx_event *evt,
		      uint8_t filter_index)
{
	struct radiant_frame_wire w;
	struct radiant_frame f;
	struct radiant_channel_id lo_id;
	struct radiant_search_result r;
	int rc;
	bool acquire_claimed = false;

	/*
	 * FACT ONE. filter_index indexes the filter array WE supplied, so
	 * devnum_lo is recovered by looking up our own table. It is never
	 * parsed out of the body, because it is not in the body: a search
	 * frame's body is [devnum_hi][dtype][ttype][ctrl][d0..d7] and the low
	 * byte was consumed by the hardware matcher. Spike B proved the index
	 * itself by putting the real device in slot 5 and decoys in the other
	 * seven; RXMATCH read 5 on all 750 CRC-valid frames.
	 */
	if (filter_index >= s->cur_n_filters) {
		s->stats.bad_filter_index++;
		return;
	}
	if (s->cfg.min_rssi_dbm != 0 && evt->has_rssi &&
	    evt->rssi_dbm < s->cfg.min_rssi_dbm) {
		s->stats.rssi_rejected++;
		return;
	}
	if (evt->body == NULL || evt->body_len > (uint8_t)RADIANT_FRAME_BODY_MAX) {
		s->stats.decode_failed++;
		return;
	}

	memset(&lo_id, 0, sizeof(lo_id));
	lo_id.device_number = s->filter_lo[filter_index];

	memset(&w, 0, sizeof(w));
	rc = radiant_frame_addr(RADIANT_FRAME_CFG_SEARCH, s->cfg.net_addr, &lo_id, w.addr,
			    sizeof(w.addr));
	if (rc != (int)RADIANT_FRAME_ADDR_LEN_SEARCH) {
		s->stats.decode_failed++;
		return;
	}
	w.addr_len = (uint8_t)rc;
	memcpy(w.body, evt->body, evt->body_len);
	w.body_len = evt->body_len;
	w.crc = 0u;

	/*
	 * FACT TWO. RADIANT_FRAME_TRUSTED_CRC, and it is not a shortcut. An
	 * RADIANT_RADIO_STATUS_OK event means the CRC was verified - by the
	 * hardware where caps.crc_in_hw is true and by the backend in software
	 * where it is false, identical semantics either way - and the received
	 * CRC bytes never reach the core, so w.crc holds nothing to check
	 * against. The flag is opt-out for the reason radiant_frame.h gives: a
	 * caller who forgets it gets a loud rejection of a good frame, and only
	 * one of the two polarities has a discoverable failure.
	 */
	rc = radiant_frame_decode(RADIANT_FRAME_CFG_SEARCH, &w, RADIANT_FRAME_TRUSTED_CRC, &f);
	if (rc != RADIANT_FRAME_OK) {
		s->stats.decode_failed++;
		return;
	}

	s->stats.frames_ok++;
	radiant_search_seen_note(s, &f.id, evt->t_sync);

	memset(&r, 0, sizeof(r));
	r.id = f.id;
	r.frame = f;
	r.t_sync = evt->t_sync;
	r.t_sync_exact = evt->t_sync_exact;
	r.has_rssi = evt->has_rssi;
	r.rssi_dbm = evt->rssi_dbm;
	r.set_index = s->cur_set;
	r.filter_index = filter_index;
	r.from_cache = s->cur_from_cache;

	/*
	 * One sweep serves every searching channel: the channel ID is offered
	 * in software to all of them. This loop is the whole of that mechanism,
	 * and it is why eight simultaneous searches take one sweep rather than
	 * eight.
	 *
	 * Offering to every match is right for SCAN, which never leaves and
	 * wants to hear everything, but wrong for ACQUIRE: several wildcard
	 * ACQUIRE channels (want all zero, or otherwise all matching the same
	 * frame) would otherwise all claim the SAME device off one frame, each
	 * one leaving the search satisfied and none of them left listening for
	 * anything else. One frame therefore claims at most one ACQUIRE
	 * channel - the first match in channel order, which is deterministic -
	 * and every SCAN channel that matches, same as before.
	 */
	for (uint8_t c = 0; c < (uint8_t)RADIANT_SEARCH_MAX_CHANNELS; c++) {
		struct radiant_search_chan *ch = &s->chans[c];

		if (!ch->active || !id_match(&ch->want, &f.id)) {
			continue;
		}
		if (ch->mode == RADIANT_SEARCH_MODE_ACQUIRE) {
			if (acquire_claimed) {
				continue;
			}
			acquire_claimed = true;
		}
		if (s->cbs.acquired != NULL) {
			s->cbs.acquired(c, &r, s->user);
		}
		if (ch->mode == RADIANT_SEARCH_MODE_ACQUIRE) {
			memset(ch, 0, sizeof(*ch));
			s->stats.acquired++;
			if (r.from_cache) {
				s->stats.cache_acquires++;
			}
		} else {
			s->stats.scan_reports++;
		}
	}
}

/*
 * Does this event belong to the window we have in flight?
 *
 * RADIANT_SEARCH_OP_EXTERNAL means the arming authority merged our request with
 * other channels' and routed the event to us itself, so there is no op id to
 * compare and the routing has already happened. Everything else is the plain
 * HAL case.
 */
static bool op_matches(const struct radiant_search *s, uint32_t op)
{
	if (s->op == 0u) {
		return false;
	}
	if (s->op == RADIANT_SEARCH_OP_EXTERNAL) {
		return true;
	}
	return op == s->op;
}

static void on_event(struct radiant_search *s, const struct radiant_rx_event *evt,
		     uint8_t filter_index)
{
	switch (evt->status) {
	case RADIANT_RADIO_STATUS_OK:
		handle_ok(s, evt, filter_index);
		break;

	case RADIANT_RADIO_STATUS_CRC_FAIL:
		/*
		 * THE MOST IMPORTANT THREE LINES IN THIS MODULE.
		 *
		 * A three-byte address matcher fires on noise several times a
		 * second on a quiet bench, and with eight filters armed Spike B
		 * measured 19, 22 and 27 of these per 15 s window with the
		 * transmitter switched off. None of them ever decoded to a
		 * plausible channel ID and the real transmitter sat about 70 dB
		 * above them. So this is counted and dropped: it is evidence
		 * about the noise floor and about nothing else, and a search
		 * that ranked on match count instead would acquire a device
		 * number out of thermal noise within seconds.
		 */
		s->stats.crc_fail++;
		break;

	case RADIANT_RADIO_STATUS_TIMEOUT:
		/* The window closed on schedule: credit the dwell. */
		radiant_search_on_done(s, true, true, evt->t_sync);
		break;

	case RADIANT_RADIO_STATUS_ABORTED:
	case RADIANT_RADIO_STATUS_FAILED:
	default:
		/*
		 * Credit nothing, deliberately, and unlike the scheduler path.
		 * This is the direct-HAL search - the module owns the radio
		 * outright - so nothing is competing for it and a window is not
		 * being cut short every period. There is no trustworthy end
		 * instant in an aborted event here, and the starvation that
		 * makes partial credit necessary cannot arise without a
		 * scheduler taking the radio away.
		 */
		radiant_search_on_done(s, false, false, 0u);
		break;
	}
}

void radiant_search_on_rx(struct radiant_search *s, const struct radiant_rx_event *evt)
{
	if (!inst_ok(s) || evt == NULL) {
		return;
	}
	if (!op_matches(s, evt->op)) {
		/* The race the op id exists for: a frame already in the
		 * receiver's pipeline when the scheduler aborted the window
		 * arrives after we moved on. Counted, never acted on. */
		s->stats.late_events++;
		return;
	}
	on_event(s, evt, evt->filter_index);
}

void radiant_search_on_rx_indexed(struct radiant_search *s, const struct radiant_rx_event *evt,
			      uint8_t filter_index)
{
	if (!inst_ok(s) || evt == NULL) {
		return;
	}
	if (!op_matches(s, evt->op)) {
		s->stats.late_events++;
		return;
	}
	on_event(s, evt, filter_index);
}

void radiant_search_on_done(struct radiant_search *s, bool ran_to_close,
			bool opened, radiant_time_t ended_at)
{
	radiant_time_t end;

	if (!inst_ok(s) || s->op == 0u) {
		return;
	}

	/*
	 * Credit the time actually spent listening on this set.
	 *
	 * A window that never opened credits nothing - it listened for no time,
	 * and crediting it would skip a set. A window that opened and was cut
	 * short credits what it really listened to, clamped into the window it
	 * was given so that a clock read a moment after the abort, or a caller
	 * that reports the wrong instant, cannot over-credit and skip coverage.
	 *
	 * Crediting the cut-short case is what keeps the sweep moving while a
	 * tracked channel owns most of the radio; see the contract in the header.
	 */
	if (s->t_close > s->t_open) {
		if (ran_to_close) {
			end = s->t_close;
		} else if (opened) {
			end = (ended_at > s->t_close) ? s->t_close : ended_at;
		} else {
			end = s->t_open;
		}

		if (end > s->t_open) {
			uint64_t listened = (uint64_t)(end - s->t_open);
			uint64_t total = (uint64_t)s->set_dwell_us + listened;

			s->set_dwell_us = (total > (uint64_t)UINT32_MAX)
						  ? UINT32_MAX
						  : (uint32_t)total;
		}
	}

	s->op = 0u;
}

void radiant_search_tick(struct radiant_search *s, radiant_time_t now)
{
	if (!inst_ok(s)) {
		return;
	}

	for (uint8_t i = 0; i < (uint8_t)RADIANT_SEARCH_SEEN_ENTRIES; i++) {
		if (s->seen[i].valid && !seen_live(s, &s->seen[i], now)) {
			memset(&s->seen[i], 0, sizeof(s->seen[i]));
		}
	}

	for (uint8_t c = 0; c < (uint8_t)RADIANT_SEARCH_MAX_CHANNELS; c++) {
		struct radiant_search_chan *ch = &s->chans[c];

		if (!ch->active || ch->deadline == RADIANT_TIME_NEVER) {
			continue;
		}
		if (now < ch->deadline) {
			continue;
		}
		memset(ch, 0, sizeof(*ch));
		s->stats.timeouts++;
		if (s->cbs.timeout != NULL) {
			s->cbs.timeout(c, s->user);
		}
	}
}

/* ---------------------------------------------------------------------------
 * The seen cache
 *
 * Sixteen entries and sixty seconds. It exists because re-acquiring a sensor
 * that dropped behind a rider's body in ~250 ms instead of ~4 s is the
 * difference a rider notices, and because it is thirty lines.
 * ---------------------------------------------------------------------------
 */

static bool same_device(const struct radiant_channel_id *a, const struct radiant_channel_id *b)
{
	return a->device_number == b->device_number &&
	       (uint8_t)(a->device_type & RADIANT_DEVICE_TYPE_MASK) ==
		       (uint8_t)(b->device_type & RADIANT_DEVICE_TYPE_MASK) &&
	       a->trans_type == b->trans_type;
}

void radiant_search_seen_note(struct radiant_search *s, const struct radiant_channel_id *id,
			  radiant_time_t now)
{
	if (!inst_ok(s) || id == NULL) {
		return;
	}

	/* Refresh in place if we already know it, so a sensor transmitting four
	 * times a second does not evict the fifteen others in four seconds. */
	for (uint8_t i = 0; i < (uint8_t)RADIANT_SEARCH_SEEN_ENTRIES; i++) {
		if (s->seen[i].valid && same_device(&s->seen[i].id, id)) {
			s->seen[i].id = *id;
			s->seen[i].t_seen = now;
			return;
		}
	}

	/* Otherwise the ring. Oldest-by-insertion rather than oldest-by-use:
	 * the refresh above already keeps a live sensor alive, and a true LRU
	 * would cost a scan for no behaviour anyone could measure. */
	s->seen[s->seen_next].valid = true;
	s->seen[s->seen_next].id = *id;
	s->seen[s->seen_next].t_seen = now;
	s->seen_next = (uint8_t)((s->seen_next + 1u) % (uint8_t)RADIANT_SEARCH_SEEN_ENTRIES);
}

bool radiant_search_seen_lookup(const struct radiant_search *s,
			    const struct radiant_search_id_filter *want,
			    radiant_time_t now, struct radiant_channel_id *out)
{
	const struct radiant_search_seen *best = NULL;

	if (!inst_ok(s) || want == NULL) {
		return false;
	}

	for (uint8_t i = 0; i < (uint8_t)RADIANT_SEARCH_SEEN_ENTRIES; i++) {
		const struct radiant_search_seen *e = &s->seen[i];

		if (!seen_live(s, e, now) || !id_match(want, &e->id)) {
			continue;
		}
		if (best == NULL || e->t_seen > best->t_seen) {
			best = e;
		}
	}

	if (best == NULL) {
		return false;
	}
	if (out != NULL) {
		*out = best->id;
	}
	return true;
}

uint8_t radiant_search_seen_count(const struct radiant_search *s, radiant_time_t now)
{
	uint8_t n = 0;

	if (!inst_ok(s)) {
		return 0u;
	}
	for (uint8_t i = 0; i < (uint8_t)RADIANT_SEARCH_SEEN_ENTRIES; i++) {
		if (seen_live(s, &s->seen[i], now)) {
			n++;
		}
	}
	return n;
}
