/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_search.c - wildcard search: the sweep, the seen cache, and scan mode.
 *
 * Provenance: clean-room. Written from radiant/include/radiant/radiant_search.h (which
 * carries the reasoning), docs/ant-radio-link.md, docs/spike-a-results.md and
 * docs/spike-b-results.md for the measured search configuration, the noise
 * floor of a three-byte matcher and the RXMATCH recovery of devnum_lo, and from
 * the free ANT Message Protocol and Usage Rev 5.1 (D00000652) for the wildcard
 * channel-ID convention and the 25 s default search timeout. Nothing here
 * derives from sdk-ant, from libant.a, from disassembly of any binary, or from
 * any ANT+ device profile document. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * The header carries the design argument. This file is deliberately dull;
 * the three places where it is not are build_set() (the only place a filter
 * address is constructed, via radiant_frame_addr()), handle_ok() (devnum_lo
 * comes from filter_lo[filter_index] and nowhere else), and
 * radiant_search_on_rx() (CRC_FAIL is counted and dropped).
 *
 * NO REGISTER ARITHMETIC ANYWHERE: the BALEN < 4 packing rule is a backend's
 * problem, solved once in radiant_frame.h's radiant_addr_pack_leading() /
 * radiant_addr_pack_trailing(). This module speaks on-air byte order only.
 */

#include <string.h>

#include <radiant/radiant_search.h>

#include <radiant/radiant_frame.h>
#include <radiant/radiant_radio_hal.h>

/* ---------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------------
 */

static bool inst_ok(const struct radiant_search *s)
{
	return s != NULL && s->inited;
}

/* ANT's wildcard convention: zero is "any" [rev5.1]. Device type is compared
 * with the pairing bit masked off - a sensor in pairing mode sets that bit,
 * and a search that stopped matching the moment the user pressed pairing
 * would be a memorable bug. The raw byte still reaches the caller. */
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
	 * "now - t_seen < lifetime" so a now BEFORE t_seen (a test can produce
	 * this, a real timebase cannot) keeps the entry instead of wrapping the
	 * subtraction into a huge positive number. */
	return now <= e->t_seen + (radiant_time_t)s->cfg.seen_lifetime_us;
}

/* ---------------------------------------------------------------------------
 * The sweep
 * ---------------------------------------------------------------------------
 */

/* Set k holds the n_filters consecutive devnum_lo values starting at
 * k * n_filters, consecutive rather than interleaved so "which set catches
 * device X" is a division - what the seen cache and a named-device search
 * need to jump the queue. The last set is short when n_filters doesn't
 * divide 256 (fine: a short window is legal, a duplicate filter is not). */

/*
 * The devnum_lo carried by filter j of set k. ORDINARILY consecutive, but a
 * backend may need two simultaneously-armed filters to differ by more than
 * one bit before its matcher can separate them (caps.min_filter_hamming_bits;
 * on a CC26x2 a one-bit gap receives nothing) - consecutive integers hand
 * such a part the worst pair every window.
 *
 * For the two-filter case: COMPLEMENT PAIRING, set k holds k and k ^ 0xFF -
 * eight bits apart, an involution so the 128 sets still partition all 256
 * values with no duplicate, and "which set catches X" stays arithmetic
 * (min(X, X ^ 0xFF)).
 *
 * Above two filters the constraint doesn't bind the same way (no involution
 * fixes a whole set at once) and no such backend exists, so this keeps the
 * consecutive layout and lets the backend's own arm_rx() reject what it
 * can't do, loudly rather than silently.
 */
static uint32_t set_filter_lo(const struct radiant_search *s, uint16_t set, uint8_t j)
{
	if (s->spread_pairs) {
		return (j == 0u) ? (uint32_t)set
				 : ((uint32_t)set ^ 0xFFu);
	}
	return (uint32_t)set * (uint32_t)s->n_filters + (uint32_t)j;
}

static void build_set(struct radiant_search *s, uint16_t set)
{
	uint8_t n = 0;

	for (uint8_t j = 0; j < s->n_filters; j++) {
		struct radiant_channel_id id;
		uint8_t addr[RADIANT_FRAME_ADDR_MAX];
		uint32_t lo = set_filter_lo(s, set, j);
		int rc;

		if (lo >= RADIANT_SEARCH_LO_VALUES) {
			break;
		}

		memset(&id, 0, sizeof(id));
		id.device_number = (uint16_t)lo;

		rc = radiant_frame_addr(RADIANT_FRAME_CFG_SEARCH, s->cfg.net_addr, &id,
				    addr, sizeof(addr));
		if (rc != (int)RADIANT_FRAME_ADDR_LEN_SEARCH) {
			/* Unreachable: cfg is a compile-time constant and the
			 * buffer is max-sized. Skipping, not asserting, keeps a
			 * would-be silent duplicate filter out of the window. */
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
 * channels re-acquiring the same sensor shouldn't make the sweep listen to
 * that set twice while every other set waits. */
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

	/*
	 * DISCOVERY IS RADIANT_FRAME_CFG_SEARCH AND NOTHING ELSE, PERMANENTLY
	 * (ADR 0007): a long-range node is found by its 1 M descriptor or the
	 * sync handoff, never by sweeping the coded PHY.
	 *
	 * Reason: this module covers all 256 devnum_lo values in n_sets windows
	 * and is CERTAIN to find any transmitting device within one sweep
	 * (seven tests in test_search.c defend this). A second PHY would not
	 * add windows to that sweep, it would MULTIPLY it - every set listened
	 * to twice, doubling sweep time - and a frequency axis would multiply
	 * it again. So extension axes are announced in a descriptor heard on
	 * 1 M instead, and ADR 0005's merged RX window survives both without
	 * either touching discovery.
	 */
	fmt = radiant_frame_format(RADIANT_FRAME_CFG_SEARCH);
	if (fmt == NULL) {
		return RADIANT_SEARCH_EINVAL;
	}

	caps = radiant_radio_caps_get();
	if (caps == NULL || caps->max_filters == 0u) {
		return RADIANT_SEARCH_EINVAL;
	}
	/* The flag says the backend can match "any device number" with one
	 * filter, collapsing the sweep to a single window - but there's no
	 * field in struct radiant_rx_filter to ask for that, so nothing here
	 * could arm it. No planned backend sets it. */
	if (caps->filter_wildcard_dev) {
		return RADIANT_SEARCH_ENOTSUP;
	}
	/* A backend that cannot carry the search body cannot search: twelve
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
	/* Complement pairing only where the backend asks for separation AND the
	 * window holds exactly two filters, the only width the involution
	 * covers. See set_filter_lo(). */
	s->spread_pairs = (caps->min_filter_hamming_bits > 1u) && (nf == 2u);
	/* Coverage arithmetic, and must never become a constant: 8 filters ->
	 * 32 sets on nRF, 2 filters -> 128 sets on EFR32/RAIL, same expression. */
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

/* Dwell still owed to the selected set, or 0 if finished - the single
 * definition both radiant_search_set_complete() and
 * radiant_search_dwell_remaining() answer from. They MUST agree: the caller
 * keeps the request armed while set_complete() is false and sizes the next
 * chunk from dwell_remaining(), so disagreeing would arm chunks too short to
 * hear anything forever. See RADIANT_SEARCH_DWELL_EPSILON_US. */
static uint32_t dwell_owed(const struct radiant_search *s)
{
	uint32_t owed;

	if (s->set_dwell_us >= s->cfg.dwell_us) {
		return 0u;
	}
	owed = s->cfg.dwell_us - s->set_dwell_us;

	/* A residue too small to be worth arming is not owed at all: the set
	 * has had its coverage and the next one is waiting. */
	return (owed < RADIANT_SEARCH_DWELL_EPSILON_US) ? 0u : owed;
}

bool radiant_search_set_complete(const struct radiant_search *s)
{
	if (!inst_ok(s)) {
		return true;
	}
	/* No set selected yet is "finished" for this purpose: nothing is worth
	 * keeping armed, and radiant_search_window() must run to choose one. */
	if (!s->set_valid) {
		return true;
	}
	return dwell_owed(s) == 0u;
}

uint32_t radiant_search_dwell_remaining(const struct radiant_search *s)
{
	if (!inst_ok(s) || !s->set_valid) {
		return 0u;
	}
	return dwell_owed(s);
}

uint16_t radiant_search_set_for_devnum(const struct radiant_search *s, uint16_t devnum)
{
	if (!inst_ok(s)) {
		return RADIANT_SEARCH_SETS_NONE;
	}
	if (s->spread_pairs) {
		uint8_t lo = (uint8_t)(devnum & 0xFFu);

		/* Complement pairing: set k holds k and k ^ 0xFF, so the set is
		 * whichever of the two is the lower. See set_filter_lo(). */
		return (uint16_t)((lo < 0x80u) ? lo : (uint8_t)(lo ^ 0xFFu));
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

	/* Jump the queue where we can - the whole reason re-acquisition is
	 * fast: either the channel named a device number (devnum_lo known
	 * outright) or the seen cache holds a matching device. Either way this
	 * only reorders the sweep; a wrong guess costs one dwell, which the
	 * round robin covers anyway. */
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

	/* Advance only when the dwell is fully credited. A window the
	 * scheduler cut short to service a tracked channel leaves the same set
	 * selected for the remainder, so "certain within one sweep" survives
	 * fragmentation instead of degrading into "likely". */
	if (!s->set_valid || dwell_owed(s) == 0u) {
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
	/* RADIANT_RX_STOP_ON_FIRST is never set: more than one master may
	 * transmit inside one search window, and stopping at the first would
	 * cost every other searching channel the rest of the dwell. */
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

	/* FACT ONE: filter_index indexes the filter array WE supplied, so
	 * devnum_lo comes from looking up our own table, never parsed from the
	 * body - the low byte was consumed by the hardware matcher. Spike B
	 * confirmed this by putting the real device in slot 5 and decoys in
	 * the other seven; RXMATCH read 5 on all 750 CRC-valid frames. */
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

	/* FACT TWO: RADIANT_FRAME_TRUSTED_CRC is not a shortcut. STATUS_OK
	 * means the CRC was verified (hardware or software, same semantics)
	 * and the received CRC bytes never reach the core, so w.crc has
	 * nothing to check against. */
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
	 * in software to all of them, which is why eight simultaneous searches
	 * take one sweep, not eight.
	 *
	 * Offering to every match is right for SCAN but wrong for ACQUIRE:
	 * several wildcard ACQUIRE channels would otherwise all claim the same
	 * device off one frame. So one frame claims at most one ACQUIRE
	 * channel (first match in channel order) and every matching SCAN
	 * channel, as before.
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

/* Does this event belong to the window we have in flight?
 * RADIANT_SEARCH_OP_EXTERNAL means the scheduler merged our request with
 * others' and already routed the event to us, so there's no op id to
 * compare. Everything else is the plain HAL case. */
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
		/* THE MOST IMPORTANT THREE LINES IN THIS MODULE: a three-byte
		 * address matcher fires on noise several times a second
		 * (Spike B measured 19-27 per 15 s window, transmitter off,
		 * real signal ~70 dB above them). Counted and dropped - it's
		 * evidence about the noise floor and nothing else; ranking on
		 * match count instead would acquire a device number out of
		 * thermal noise within seconds. */
		s->stats.crc_fail++;
		break;

	case RADIANT_RADIO_STATUS_TIMEOUT:
		/* The window closed on schedule: credit the dwell. */
		radiant_search_on_done(s, true, true, evt->t_sync);
		break;

	case RADIANT_RADIO_STATUS_ABORTED:
	case RADIANT_RADIO_STATUS_FAILED:
	case RADIANT_RADIO_STATUS_DENIED:
	default:
		/* Credit nothing, deliberately. DENIED needs no arm of its own:
		 * a window the arbiter refused didn't listen for a
		 * microsecond, so zero is exact, not conservative
		 * (radiant_api.c's scheduler path reaches the same conclusion
		 * via ran=false, credited=false). This is the direct-HAL
		 * search - nothing competes for the radio here, so the
		 * starvation that makes partial credit necessary can't arise
		 * without a scheduler taking the radio away. */
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
		/* The race the op id exists for: a frame already in the pipeline
		 * when the scheduler aborted the window arrives after we moved
		 * on. Counted, never acted on. */
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

	/* Credit the time actually spent listening on this set. A window that
	 * never opened credits nothing (crediting it would skip a set); a
	 * window cut short credits what it really listened to, clamped into
	 * the window it was given so a caller reporting the wrong instant
	 * can't over-credit and skip coverage. Crediting the cut-short case
	 * keeps the sweep moving while a tracked channel owns most of the
	 * radio; see the contract in the header. */
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
#if defined(CONFIG_RADIANT_SEARCH_DENIAL_ESCAPE)
			/* THE ONLY PLACE REAL LISTENING IS CREDITED, so the
			 * only place the denial run can honestly be reset. A
			 * sweep that is merely slow - denied often but still
			 * getting air in between - therefore never escapes,
			 * which is the point: the escape is for the set that
			 * gets nothing, not for the set that gets less. */
			s->denied_run = 0u;
#endif
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

#if defined(CONFIG_RADIANT_SEARCH_DENIAL_ESCAPE)
/*
 * THE BOUNDED-DENIAL ESCAPE. Called once per refused window, from
 * radiant_search_note_denied() and nowhere else.
 *
 * WHY IT CANNOT SPIN, in full, because "bounded" is the entire claim:
 *
 *  1. Nothing here arms, re-arms or requests anything. It is a pure state
 *     update on a report of something that has already happened. The rate of
 *     denials is set by the arbiter and the arming authority, not by this
 *     function, so it cannot feed itself.
 *  2. denied_run only reaches RUN once per RUN calls (it is zeroed on every
 *     escape), so at most one quantum is credited per RUN denials.
 *  3. Every escape adds a STRICTLY POSITIVE quantum to set_dwell_us - the
 *     floor of 1 below is what makes that true for every legal dwell_us, not
 *     just the sane ones - and nothing but build_set() ever lowers
 *     set_dwell_us. So the sequence of set_dwell_us values across escapes on
 *     one set is strictly increasing.
 *  4. dwell_owed() returns 0 once set_dwell_us > cfg.dwell_us - EPSILON, and
 *     by (3) that threshold is reached in at most
 *     ceil(cfg.dwell_us / quantum) escapes: 8 at the shift of 3, hence 8*RUN
 *     = 64 denials at the default run length. (For cfg.dwell_us below
 *     EPSILON, dwell_owed() is already 0 at zero credit and no escape is ever
 *     needed - see dwell_owed().)
 *  5. At that point radiant_search_set_complete() is true, so the arming
 *     authority drops the slot and the pump calls radiant_search_window(),
 *     which calls select_next_set() -> build_set(), which zeroes
 *     set_dwell_us. A DIFFERENT set is now selected, and the argument starts
 *     over on it. There is no state in which the same set can be escaped
 *     from twice without the sweep having moved.
 *
 * Note (5) is why this credits a quantum instead of advancing the cursor: the
 * advance is done by the code that already knows about the steer queue and
 * the seen cache. Doing it here would work and would silently cost fast
 * re-acquisition exactly when the arbiter is busy, which is when it matters.
 */
static void denial_escape(struct radiant_search *s)
{
	uint32_t quantum;
	uint64_t total;

	/* No set selected: radiant_search_window() has not run, there is
	 * nothing to credit, and the run is not advanced either - a denial
	 * with no set selected cost the sweep no coverage. */
	if (!s->set_valid) {
		return;
	}

	if (s->denied_run < UINT8_MAX) {
		s->denied_run++;
	}
	if (s->denied_run < RADIANT_SEARCH_DENIAL_ESCAPE_RUN) {
		return;
	}

	quantum = s->cfg.dwell_us >> RADIANT_SEARCH_DENIAL_ESCAPE_SHIFT;
	if (quantum == 0u) {
		/* Only reachable for a dwell under eight microseconds, which
		 * dwell_owed() already calls finished. Kept anyway: point (3)
		 * of the argument above is "strictly positive", and a
		 * zero-quantum escape would be an infinite loop that no test
		 * with a realistic dwell could ever reach. */
		quantum = 1u;
	}

	total = (uint64_t)s->set_dwell_us + (uint64_t)quantum;
	s->set_dwell_us = (total > (uint64_t)UINT32_MAX) ? UINT32_MAX
							 : (uint32_t)total;
	s->denied_run = 0u;
	s->stats.denial_escapes++;
}
#endif /* CONFIG_RADIANT_SEARCH_DENIAL_ESCAPE */

void radiant_search_note_denied(struct radiant_search *s, uint32_t window_us)
{
	if (!inst_ok(s) || window_us == 0u) {
		return;
	}

#if defined(CONFIG_RADIANT_SEARCH_DENIAL_ESCAPE)
	denial_escape(s);
#endif

	for (uint8_t c = 0u; c < (uint8_t)RADIANT_SEARCH_MAX_CHANNELS; c++) {
		struct radiant_search_chan *ch = &s->chans[c];

		if (!ch->active || ch->deadline == RADIANT_TIME_NEVER) {
			continue;
		}
		/* Saturate rather than wrap: an overflowed deadline would
		 * expire the channel on the very next tick, the opposite of
		 * what the extension is for. */
		if ((radiant_time_t)(ch->deadline + window_us) < ch->deadline) {
			ch->deadline = RADIANT_TIME_NEVER - 1u;
		} else {
			ch->deadline += (radiant_time_t)window_us;
		}
	}
}

/* ---------------------------------------------------------------------------
 * The seen cache: sixteen entries, sixty seconds. Re-acquiring a sensor that
 * dropped behind a rider's body in ~250 ms instead of ~4 s is a difference a
 * rider notices.
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

	/* Refresh in place if already known, so a sensor transmitting four
	 * times a second doesn't evict the fifteen others in four seconds. */
	for (uint8_t i = 0; i < (uint8_t)RADIANT_SEARCH_SEEN_ENTRIES; i++) {
		if (s->seen[i].valid && same_device(&s->seen[i].id, id)) {
			s->seen[i].id = *id;
			s->seen[i].t_seen = now;
			return;
		}
	}

	/* Otherwise the ring. Oldest-by-insertion, not oldest-by-use: the
	 * refresh above already keeps a live sensor alive, and a true LRU
	 * would cost a scan for no measurable benefit. */
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
