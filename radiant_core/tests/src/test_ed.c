/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_ed.c - energy-detect scan: the HAL operation, the channel-quality map,
 * and the scheduler slot kind that pays for it.
 *
 * Provenance: original clean-room work. Written against
 * radiant_core/include/radiant_core/radiant_radio_hal.h, radiant_sched.h and
 * radiant_chanmap.h, and driven through radiant_core/tests/fake_radio.c.
 * Nothing here derives from sdk-ant, from libant.a, or from any
 * ANT+ device profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * The test that matters is sched_ed_leaves_tracked_schedule_identical. The
 * phase's gate is "loss_exact unchanged with ED scanning on under full
 * tracked load", an on-air figure this suite (no radio) cannot measure - so
 * instead it asserts the mechanism more sharply than a loss figure could:
 * under full tracked load with an ED request measuring throughout, EVERY
 * receive window is armed at the same instant, same members, same order, as
 * with no ED request at all. Not "within tolerance" - identical, entry for
 * entry. A packet is lost to a scheduler when its window is late, short, or
 * never armed; if no window moved, no packet was lost to this feature. A
 * 300-second loss run can only say "smaller than the bench's ~0.3 pp
 * run-to-run spread"; this says zero, and why (see arm_ed_chunk() on
 * s.cursor and s.replan).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include <radiant_core/radiant_chanmap.h>
#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_noise.h>
#include <radiant_core/radiant_sched.h>

#include "../fake_radio.h"

/* ---------------------------------------------------------------------------
 * Fixtures
 * ---------------------------------------------------------------------------
 */

#define ED_DEVNUM_BASE 0x3A00u
#define ED_DEV_TYPE    0x0Bu
#define ED_TRANS_TYPE  0x05u

/* The measured mean ANT+ slot period (docs/ant-radio-link.md), so the gate runs
 * at the rate the bench actually sees. */
#define ED_PERIOD_US 249696u
#define ED_WINDOW_US 400u

/* Tracked channels in the load. Eight, against caps.max_addr_groups == 2 on the
 * nRF preset, so the load is four merged windows per period rather than one -
 * which is what makes "the same members in the same order" a real assertion
 * instead of a statement about a single window. */
#define ED_TRACKED 8u

/* The ED channel. Above the tracked ones so that the leader search reaches it
 * last, which is the arrangement most likely to expose a cursor perturbation if
 * one existed. */
#define ED_CH 20u

/*
 * The ED range and dwell used by the scheduler tests. Five indices at 40 ms
 * is deliberately unlike a product setting, chosen against the mock (which
 * spends the WHOLE dwell on every index, unlike the real backend): a
 * realistic 400 us dwell would overflow the mock's 128-entry event log
 * inside the first period, whereas 40 ms holds one full sweep and part of
 * another per gap, exercising chunking, wrap and resumption readably.
 */
#define ED_RF_LO     2u
#define ED_RF_HI     6u
#define ED_DWELL_US  40000u
#define ED_CHUNK_US  250000u

static struct radiant_rx_filter track_filter[RADIANT_SCHED_MAX_CHANNELS];

#define ED_LOG_MAX 128u

static struct {
	/* Every receive window arm, as its members saw it. This is the
	 * sequence the gate compares. */
	uint32_t n_armed;
	struct {
		uint8_t    ch;
		radiant_time_t t_open;
		radiant_time_t t_close;
	} armed[ED_LOG_MAX];

	uint32_t n_dwell;
	struct {
		uint8_t  ch;
		uint8_t  rf_index;
		int8_t   min_dbm;
		int8_t   mean_dbm;
		uint16_t samples;
	} dwell[ED_LOG_MAX];

	uint32_t done_ok[RADIANT_SCHED_MAX_CHANNELS];
	uint32_t done_missed[RADIANT_SCHED_MAX_CHANNELS];
	uint32_t done_aborted[RADIANT_SCHED_MAX_CHANNELS];
	uint32_t done_failed[RADIANT_SCHED_MAX_CHANNELS];
	uint32_t done_denied[RADIANT_SCHED_MAX_CHANNELS];

	bool       repost;
	radiant_time_t next_open[RADIANT_SCHED_MAX_CHANNELS];
} log;

static void on_rx(uint8_t ch, uint8_t filter_index,
		  const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(ch);
	ARG_UNUSED(filter_index);
	ARG_UNUSED(evt);
	ARG_UNUSED(user);
}

static void on_tx(uint8_t ch, const struct radiant_tx_event *evt, void *user)
{
	ARG_UNUSED(ch);
	ARG_UNUSED(evt);
	ARG_UNUSED(user);
}

static void on_ed(uint8_t ch, const struct radiant_ed_event *evt, void *user)
{
	ARG_UNUSED(user);

	if (log.n_dwell < ED_LOG_MAX) {
		log.dwell[log.n_dwell].ch = ch;
		log.dwell[log.n_dwell].rf_index = evt->rf_index;
		log.dwell[log.n_dwell].min_dbm = evt->min_dbm;
		log.dwell[log.n_dwell].mean_dbm = evt->mean_dbm;
		log.dwell[log.n_dwell].samples = evt->samples;
	}
	log.n_dwell++;
}

static void post_track(uint8_t ch, radiant_time_t t_open);

static void on_done(uint8_t ch, enum radiant_sched_done why, void *user)
{
	ARG_UNUSED(user);

	switch (why) {
	case RADIANT_SCHED_DONE_OK:
		log.done_ok[ch]++;
		break;
	case RADIANT_SCHED_DONE_MISSED:
		log.done_missed[ch]++;
		break;
	case RADIANT_SCHED_DONE_ABORTED:
		log.done_aborted[ch]++;
		break;
	case RADIANT_SCHED_DONE_DENIED:
		/* Its own bucket, not folded into `failed`: the whole claim of
		 * the denial signal is that these two are different facts. */
		log.done_denied[ch]++;
		break;
	default:
		log.done_failed[ch]++;
		break;
	}

	/* An ED request is never consumed, so it must not be re-posted; doing so
	 * would reset its cursor every chunk and the sweep would never leave its
	 * first index. */
	if (log.repost && ch < (uint8_t)ED_TRACKED) {
		log.next_open[ch] += (radiant_time_t)ED_PERIOD_US;
		post_track(ch, log.next_open[ch]);
	}
}

static void on_armed(uint8_t ch, radiant_time_t t_open, radiant_time_t t_close,
		     void *user)
{
	ARG_UNUSED(user);

	if (log.n_armed < ED_LOG_MAX) {
		log.armed[log.n_armed].ch = ch;
		log.armed[log.n_armed].t_open = t_open;
		log.armed[log.n_armed].t_close = t_close;
	}
	log.n_armed++;
}

static const struct radiant_sched_cbs ed_cbs = {
	.rx = on_rx,
	.tx = on_tx,
	.ed = on_ed,
	.armed = on_armed,
	.done = on_done,
};

static void post_track(uint8_t ch, radiant_time_t t_open)
{
	struct radiant_sched_rx r;

	memset(&r, 0, sizeof(r));
	r.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	r.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	r.filters = &track_filter[ch];
	r.n_filters = 1u;
	r.t_open = t_open;
	r.t_close = t_open + (radiant_time_t)ED_WINDOW_US;
	zassert_ok(radiant_sched_request_rx(ch, &r), "channel %u refused", ch);
}

static void post_ed(uint8_t ch)
{
	struct radiant_sched_ed e;

	memset(&e, 0, sizeof(e));
	e.rf_index_lo = ED_RF_LO;
	e.rf_index_hi = ED_RF_HI;
	e.dwell_us = ED_DWELL_US;
	e.chunk_us = ED_CHUNK_US;
	zassert_ok(radiant_sched_request_ed(ch, &e), "ED request refused");
}

static void build_filters(void)
{
	uint32_t i;

	for (i = 0u; i < RADIANT_SCHED_MAX_CHANNELS; i++) {
		struct radiant_channel_id id = {
			.device_number = (uint16_t)(ED_DEVNUM_BASE + i),
			.device_type = ED_DEV_TYPE,
			.trans_type = ED_TRANS_TYPE,
		};

		zassert_equal(5, radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING,
						    radiant_net_addr_ant_plus, &id,
						    track_filter[i].addr,
						    sizeof(track_filter[i].addr)),
			      "tracking address length changed");
		track_filter[i].addr_len = 5u;
	}
}

static void bring_up(void)
{
	memset(&log, 0, sizeof(log));
	fake_radio_reset();
	fake_radio_clear_ed();
	build_filters();
	zassert_ok(radiant_sched_init(&ed_cbs, NULL));
	zassert_ok(radiant_radio_init(radiant_sched_radio_cbs(), NULL));
	zassert_ok(radiant_radio_enable());
}

/* ---------------------------------------------------------------------------
 * The HAL operation, driven directly
 * ---------------------------------------------------------------------------
 */

#define RAW_LOG_MAX 64u

static struct {
	uint32_t n;
	struct {
		enum radiant_radio_status status;
		uint8_t  rf_index;
		int8_t   min_dbm;
		int8_t   mean_dbm;
		uint16_t samples;
	} ev[RAW_LOG_MAX];
} raw;

static void raw_ed(const struct radiant_ed_event *evt, void *user)
{
	ARG_UNUSED(user);

	if (raw.n < RAW_LOG_MAX) {
		raw.ev[raw.n].status = evt->status;
		raw.ev[raw.n].rf_index = evt->rf_index;
		raw.ev[raw.n].min_dbm = evt->min_dbm;
		raw.ev[raw.n].mean_dbm = evt->mean_dbm;
		raw.ev[raw.n].samples = evt->samples;
	}
	raw.n++;
}

static const struct radiant_radio_cbs raw_cbs = {
	.rx = NULL,
	.tx = NULL,
	.ed = raw_ed,
};

static void raw_bring_up(void)
{
	memset(&raw, 0, sizeof(raw));
	fake_radio_reset();
	fake_radio_clear_ed();
	zassert_ok(radiant_radio_init(&raw_cbs, NULL));
	zassert_ok(radiant_radio_enable());
}

static void raw_req(struct radiant_ed_req *r, uint8_t lo, uint8_t hi,
		    uint32_t dwell_us)
{
	memset(r, 0, sizeof(*r));
	r->rf_index_lo = lo;
	r->rf_index_hi = hi;
	r->dwell_us = dwell_us;
	r->t_start = radiant_radio_now() +
		     (radiant_time_t)radiant_radio_caps_get()->min_arm_lead_us;
}

/*
 * A sweep delivers one OK event per index, in ascending order, and then exactly
 * one terminal event - which is the receive window's contract applied to a
 * different operation, and is asserted here rather than assumed because every
 * consumer above is written against it.
 */
ZTEST(ed, test_hal_sweep_shape)
{
	struct radiant_ed_req r;
	uint32_t op = 0u;
	uint32_t i;

	raw_bring_up();
	/* A band with one loud index in it, so "which index did this event
	 * describe" is checkable rather than a matter of trusting a counter. */
	fake_radio_set_ed(4u, -70, -64);

	raw_req(&r, 2u, 6u, 1000u);
	zassert_ok(radiant_radio_ed(&r, &op), "ED arm refused");
	zassert_not_equal(0u, op, "an accepted arm must return a non-zero op");

	fake_radio_advance(10u * 1000u);

	zassert_equal(6u, raw.n, "five indices plus one terminal event");
	for (i = 0u; i < 5u; i++) {
		zassert_equal(RADIANT_RADIO_STATUS_OK, raw.ev[i].status,
			      "event %u should be a measurement", i);
		zassert_equal(2u + i, raw.ev[i].rf_index,
			      "indices must be swept in ascending order");
		zassert_equal(FAKE_RADIO_ED_SAMPLES, raw.ev[i].samples,
			      "a measurement must report its sample count");
	}
	zassert_equal(-70, raw.ev[2].min_dbm, "RF 4's min did not reach the event");
	zassert_equal(-64, raw.ev[2].mean_dbm, "RF 4's mean did not reach the event");
	zassert_equal(FAKE_RADIO_ED_MIN_DBM, raw.ev[0].min_dbm,
		      "an unset index should report the default");

	zassert_equal(RADIANT_RADIO_STATUS_TIMEOUT, raw.ev[5].status,
		      "a completed sweep ends with TIMEOUT, as a window does");
	zassert_equal(0u, raw.ev[5].samples,
		      "a terminal event carries no measurement");
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
	zassert_equal(0u, fake_radio_viol_count(), "%s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
}

/* The one-operation-in-flight rule, and the range checks. */
ZTEST(ed, test_hal_refusals)
{
	struct radiant_ed_req r;
	uint32_t op = 0u;
	uint32_t op2 = 0u;

	raw_bring_up();

	raw_req(&r, 6u, 2u, 1000u);
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_ed(&r, &op),
		      "a range running backwards must be refused");
	raw_req(&r, 2u, RADIANT_RF_INDEX_MAX + 1u, 1000u);
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_ed(&r, &op),
		      "an index above the HAL's ceiling must be refused");
	raw_req(&r, 2u, 6u, 0u);
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_ed(&r, &op),
		      "a zero dwell must be refused");

	raw_req(&r, 2u, 6u, 1000u);
	r.t_start = radiant_radio_now();
	zassert_equal(RADIANT_RADIO_ETIME, radiant_radio_ed(&r, &op),
		      "an unreachable start must be refused, never run late");

	raw_req(&r, 2u, 6u, 1000u);
	zassert_ok(radiant_radio_ed(&r, &op));
	raw_req(&r, 2u, 6u, 1000u);
	zassert_equal(RADIANT_RADIO_EBUSY, radiant_radio_ed(&r, &op2),
		      "a second operation while one is in flight is EBUSY");
	zassert_equal(0u, op2, "a refused arm must not hand out an op id");

	zassert_ok(radiant_radio_abort());
	zassert_equal(1u, raw.n, "an aborted sweep still gets its terminal event");
	zassert_equal(RADIANT_RADIO_STATUS_ABORTED, raw.ev[0].status);
}

/*
 * "Backends without it return RADIANT_RADIO_ENOTSUP (existing HAL rule)."
 *
 * Both halves: the backend refuses, and core policy reads the capability rather
 * than finding out by trying.
 */
ZTEST(ed, test_hal_notsup_without_capability)
{
	struct radiant_ed_req r;
	struct radiant_sched_ed e;
	uint32_t op = 0u;

	raw_bring_up();
	fake_radio_caps_preset_rail();
	zassert_false(radiant_radio_caps_get()->has_ed_scan,
		      "the RAIL preset is the backend that does not have it");

	raw_req(&r, 2u, 6u, 1000u);
	zassert_equal(RADIANT_RADIO_ENOTSUP, radiant_radio_ed(&r, &op),
		      "a backend without the capability must refuse ENOTSUP");
	zassert_equal(0u, raw.n, "a refused arm produces no event at all");

	/* And the scheduler refuses at the door rather than parking a request no
	 * pass could ever serve. */
	memset(&log, 0, sizeof(log));
	zassert_ok(radiant_sched_init(&ed_cbs, NULL));
	memset(&e, 0, sizeof(e));
	e.rf_index_lo = ED_RF_LO;
	e.rf_index_hi = ED_RF_HI;
	zassert_equal(RADIANT_RADIO_ENOTSUP,
		      radiant_sched_request_ed(ED_CH, &e),
		      "the scheduler must read caps.has_ed_scan, not try");
	zassert_false(radiant_sched_pending(ED_CH),
		      "a refused ED request must not occupy a slot");
}

/* ---------------------------------------------------------------------------
 * The channel-quality map
 * ---------------------------------------------------------------------------
 */

/*
 * "Aggregate into a channel-quality map using the same 1 dB binning as
 * radiant_noise.c, so opportunistic and deliberate measurements are on one
 * scale."
 *
 * Asserted as an identity against radiant_noise's own binning rather than
 * against literal dBm values, because a literal would still pass if both sides
 * moved together and the point is that they cannot move apart.
 */
ZTEST(ed, test_chanmap_shares_the_noise_binning)
{
	struct radiant_chanmap_report rep;
	uint32_t i;

	radiant_chanmap_reset();

	for (i = 0u; i < RADIANT_CHANMAP_MIN_DWELLS; i++) {
		radiant_chanmap_note(57u, -91, -88, FAKE_RADIO_ED_SAMPLES);
	}
	zassert_true(radiant_chanmap_get(57u, &rep), "RF 57 should be readable");
	zassert_equal(radiant_noise_bin_dbm(radiant_noise_bin(-91)), rep.floor_dbm,
		      "the floor is not on the noise histogram's scale");
	zassert_equal(radiant_noise_bin_dbm(radiant_noise_bin(-88)), rep.busy_dbm,
		      "the busy figure is not on the noise histogram's scale");
	zassert_equal(radiant_noise_bin_dbm(radiant_noise_bin(-88)), rep.mean_dbm,
		      "the mean of one value repeated is that value");

	/* Both walls, and both are the histogram's walls rather than this
	 * module's own. A sample below the range clamps to the quiet end and one
	 * above it clamps to the loud end - the same rule radiant_noise_note()
	 * applies, which is what keeps a piled-up distribution comparable
	 * between the two. */
	radiant_chanmap_clear(57u);
	for (i = 0u; i < RADIANT_CHANMAP_MIN_DWELLS; i++) {
		radiant_chanmap_note(57u, -128, 0, FAKE_RADIO_ED_SAMPLES);
	}
	zassert_true(radiant_chanmap_get(57u, &rep));
	zassert_equal(RADIANT_NOISE_DBM_MIN, rep.floor_dbm,
		      "a sample below the range must clamp to the range's floor");
	zassert_equal(RADIANT_NOISE_DBM_MAX, rep.busy_dbm,
		      "a sample above the range must clamp to the range's top");
}

/* floor is the minimum of the minima, busy is the maximum of the means, and the
 * mean is the mean - three different statistics over the same dwells, which is
 * the only way one set of eight bytes can answer "how quiet does it get" and
 * "how bad does it get" separately. */
ZTEST(ed, test_chanmap_floor_busy_and_mean)
{
	struct radiant_chanmap_report rep;

	radiant_chanmap_reset();

	radiant_chanmap_note(26u, -100, -96, 16u);
	radiant_chanmap_note(26u, -60, -56, 16u);
	radiant_chanmap_note(26u, -90, -86, 16u);
	radiant_chanmap_note(26u, -94, -90, 16u);

	zassert_true(radiant_chanmap_get(26u, &rep));
	zassert_equal(4u, rep.dwells, "every dwell must be counted");
	zassert_equal(-100, rep.floor_dbm, "the floor is the quietest minimum");
	zassert_equal(-56, rep.busy_dbm, "the busy figure is the loudest mean");
	/* (-96 + -56 + -86 + -90) / 4 = -82, computed in bins. */
	zassert_equal(-82, rep.mean_dbm, "the mean of the four dwell means");

	/* Below the evidence threshold an index reports nothing at all, which is
	 * a different answer from "measured and quiet" and has to be. */
	radiant_chanmap_reset();
	radiant_chanmap_note(26u, -100, -96, 16u);
	zassert_false(radiant_chanmap_get(26u, &rep),
		      "one dwell is not enough evidence to report");

	/* A dwell that measured nothing is dropped and counted, never folded in
	 * as a quiet index - which is the direction this map must not lie in. */
	radiant_chanmap_reset();
	radiant_chanmap_note(26u, -100, -96, 0u);
	zassert_false(radiant_chanmap_get(26u, &rep));
	zassert_equal(1u, radiant_chanmap_dropped(),
		      "a zero-sample dwell must be dropped and counted");
	radiant_chanmap_note(RADIANT_RF_INDEX_MAX + 1u, -100, -96, 16u);
	zassert_equal(2u, radiant_chanmap_dropped(),
		      "an out-of-range index must be dropped and counted");
}

/* ---------------------------------------------------------------------------
 * The scheduler slot
 * ---------------------------------------------------------------------------
 */

/* Slot phase for a tracked channel: paired, not spread, and not all on one
 * instant either. Channels 2k and 2k+1 share an instant so they merge
 * (caps.max_addr_groups is 2 on the nRF preset - a pair per window, four
 * merged windows per period). The pairs are then spaced, since eight
 * channels on the SAME window would be contention, not load. */
static radiant_time_t ch_phase(uint8_t ch)
{
	return (radiant_time_t)((uint32_t)(ch / 2u) * 3000u);
}

/* Drive a tracked load for `periods` channel periods, optionally with an ED
 * request running underneath it. */
static void run_load(bool with_ed, uint32_t periods)
{
	radiant_time_t t0;
	uint8_t        ch;

	bring_up();
	t0 = radiant_radio_now() +
	     (radiant_time_t)radiant_radio_caps_get()->min_arm_lead_us + 1000u;

	log.repost = true;
	for (ch = 0u; ch < (uint8_t)ED_TRACKED; ch++) {
		log.next_open[ch] = t0 + ch_phase(ch);
		post_track(ch, log.next_open[ch]);
	}
	if (with_ed) {
		post_ed(ED_CH);
	}
	zassert_ok(radiant_sched_tick());

	fake_radio_advance_to(t0 + (radiant_time_t)periods *
					 (radiant_time_t)ED_PERIOD_US);
	log.repost = false;
}

/*
 * The gate, in its deterministic form: run the same eight-channel tracked
 * load twice, once with an ED request measuring underneath it and once
 * without, and assert the sequence of receive-window arms is identical
 * entry for entry - same channel, t_open, t_close, order. Every way a
 * scheduler can cost a packet (late, truncated, never armed, dropped from a
 * merge) moves an entry in that sequence. None move.
 */
ZTEST(ed, test_sched_ed_leaves_tracked_schedule_identical)
{
	static struct {
		uint32_t n;
		struct {
			uint8_t    ch;
			radiant_time_t t_open;
			radiant_time_t t_close;
		} armed[ED_LOG_MAX];
		uint32_t ok[RADIANT_SCHED_MAX_CHANNELS];
		uint32_t missed[RADIANT_SCHED_MAX_CHANNELS];
	} baseline;
	uint32_t i;

	run_load(false, 6u);
	zassert_true(log.n_armed > 0u, "the baseline load armed no windows");
	zassert_true(log.n_armed <= ED_LOG_MAX,
		     "the arm log overflowed; shorten the run");
	baseline.n = log.n_armed;
	memcpy(baseline.armed, log.armed, sizeof(baseline.armed));
	memcpy(baseline.ok, log.done_ok, sizeof(baseline.ok));
	memcpy(baseline.missed, log.done_missed, sizeof(baseline.missed));

	run_load(true, 6u);

	/* The feature was actually doing something during the second run.
	 * Without this the test passes trivially on a build where ED never
	 * armed, which is the one way an "unchanged" assertion can lie. */
	zassert_true(log.n_dwell > 0u,
		     "no ED dwell ran; the comparison would be vacuous");
	zassert_true(radiant_sched_stats_get()->ed_chunks > 0u,
		     "no ED chunk was armed");

	zassert_equal(baseline.n, log.n_armed,
		      "%u windows armed with ED, %u without", log.n_armed,
		      baseline.n);
	for (i = 0u; i < baseline.n; i++) {
		zassert_equal(baseline.armed[i].ch, log.armed[i].ch,
			      "arm %u went to channel %u with ED and %u without",
			      i, log.armed[i].ch, baseline.armed[i].ch);
		zassert_equal(baseline.armed[i].t_open, log.armed[i].t_open,
			      "arm %u opened %lld us from where it did without ED",
			      i,
			      (long long)((int64_t)log.armed[i].t_open -
					  (int64_t)baseline.armed[i].t_open));
		zassert_equal(baseline.armed[i].t_close, log.armed[i].t_close,
			      "arm %u closed at a different instant", i);
	}
	for (i = 0u; i < ED_TRACKED; i++) {
		zassert_equal(baseline.ok[i], log.done_ok[i],
			      "channel %u completed a different number of windows",
			      i);
		zassert_equal(baseline.missed[i], log.done_missed[i],
			      "channel %u missed a different number of windows",
			      i);
		zassert_equal(0u, log.done_missed[i],
			      "channel %u missed a window at all", i);
	}
}

/* An ED request measures in the gaps, and what it measures reaches the map. The
 * other half of the gate: unchanged AND not inert. */
ZTEST(ed, test_sched_ed_fills_gaps_and_the_map)
{
	struct radiant_chanmap_report rep;
	uint32_t i;
	uint32_t loud = 0u;

	bring_up();
	/* A quiet band with one loud index, which is the shape the map exists to
	 * find. */
	for (i = ED_RF_LO; i <= ED_RF_HI; i++) {
		fake_radio_set_ed((uint8_t)i, -100, -96);
	}
	fake_radio_set_ed(4u, -70, -62);

	{
		radiant_time_t t0 = radiant_radio_now() +
				    (radiant_time_t)radiant_radio_caps_get()
					    ->min_arm_lead_us + 1000u;
		uint8_t ch;

		log.repost = true;
		for (ch = 0u; ch < (uint8_t)ED_TRACKED; ch++) {
			log.next_open[ch] = t0 + ch_phase(ch);
			post_track(ch, log.next_open[ch]);
		}
		post_ed(ED_CH);
		zassert_ok(radiant_sched_tick());
		fake_radio_advance_to(t0 + 6u * (radiant_time_t)ED_PERIOD_US);
		log.repost = false;
	}

	zassert_true(log.n_dwell >= RADIANT_CHANMAP_MIN_DWELLS,
		     "only %u dwells ran in six periods of idle gap",
		     log.n_dwell);
	for (i = 0u; i < log.n_dwell && i < ED_LOG_MAX; i++) {
		zassert_equal(ED_CH, log.dwell[i].ch,
			      "a dwell was routed to the wrong channel");
		zassert_true(log.dwell[i].rf_index >= ED_RF_LO &&
			     log.dwell[i].rf_index <= ED_RF_HI,
			     "RF %u is outside the requested range",
			     log.dwell[i].rf_index);
		if (log.dwell[i].rf_index == 4u) {
			loud++;
		}
	}
	zassert_true(loud > 0u, "the sweep never reached RF 4");

	/* The sweep wrapped rather than stopping at the top of the range. */
	zassert_true(log.n_dwell > (ED_RF_HI - ED_RF_LO + 1u),
		     "the sweep did not wrap back to the bottom of its range");

	zassert_true(radiant_chanmap_get(4u, &rep),
		     "RF 4 was measured but is not in the map");
	zassert_equal(radiant_noise_bin_dbm(radiant_noise_bin(-70)), rep.floor_dbm,
		      "the map did not record what the dwell reported");
	zassert_equal(radiant_noise_bin_dbm(radiant_noise_bin(-62)), rep.busy_dbm);

	if (radiant_chanmap_get(2u, &rep)) {
		zassert_true(rep.busy_dbm < -62,
			     "a quiet index reads as loud as the loud one");
	}

	zassert_equal(radiant_sched_stats_get()->ed_dwells, log.n_dwell,
		      "the scheduler counted a different number of dwells");
}

/*
 * "lowest priority, never preempts, merges with nothing, takes gaps only."
 *
 * Merging is asserted structurally: an ED channel never appears in an armed()
 * notification, because armed() is fired per member of a receive window and an
 * ED slot cannot be a member of one.
 */
ZTEST(ed, test_sched_ed_merges_with_nothing_and_never_preempts)
{
	radiant_time_t t0;
	uint32_t i;

	bring_up();
	t0 = radiant_radio_now() +
	     (radiant_time_t)radiant_radio_caps_get()->min_arm_lead_us + 1000u;

	log.repost = true;
	log.next_open[0] = t0;
	post_track(0u, t0);
	post_ed(ED_CH);
	zassert_ok(radiant_sched_tick());
	fake_radio_advance_to(t0 + 6u * (radiant_time_t)ED_PERIOD_US);
	log.repost = false;

	for (i = 0u; i < log.n_armed && i < ED_LOG_MAX; i++) {
		zassert_not_equal(ED_CH, log.armed[i].ch,
				  "an ED slot joined a receive window");
	}
	zassert_equal(0u, log.done_missed[0],
		      "the tracked channel missed a window to an ED chunk");
	zassert_equal(0u, log.done_failed[ED_CH],
		      "the ED request failed rather than yielding");
	/* The ED request outlives its chunks, exactly as a background scan
	 * outlives its own. */
	zassert_true(radiant_sched_pending(ED_CH),
		     "the ED request was consumed by a chunk");
	zassert_true(log.done_ok[ED_CH] > 0u, "no ED chunk completed");
}

/*
 * Lowest priority means lower than the background scan, which is a stronger
 * statement than "lower than tracked work" and is the one most likely to be got
 * wrong: a scan slot and an ED slot are both gap fillers, and the scan wins.
 */
ZTEST(ed, test_sched_ed_yields_to_the_background_scan)
{
	struct radiant_sched_rx scan;
	static struct radiant_rx_filter scan_filter[RADIANT_SCHED_MAX_FILTERS];
	radiant_time_t t0;
	uint32_t i;

	bring_up();
	for (i = 0u; i < RADIANT_SCHED_MAX_FILTERS; i++) {
		struct radiant_channel_id id = {
			.device_number = (uint16_t)(ED_DEVNUM_BASE + i),
			.device_type = ED_DEV_TYPE,
			.trans_type = ED_TRANS_TYPE,
		};

		zassert_equal(3, radiant_frame_addr(RADIANT_FRAME_CFG_SEARCH,
						    radiant_net_addr_ant_plus, &id,
						    scan_filter[i].addr,
						    sizeof(scan_filter[i].addr)));
		scan_filter[i].addr_len = 3u;
	}

	t0 = radiant_radio_now() +
	     (radiant_time_t)radiant_radio_caps_get()->min_arm_lead_us + 1000u;

	memset(&scan, 0, sizeof(scan));
	scan.fmt = radiant_frame_format(RADIANT_FRAME_CFG_SEARCH);
	scan.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	scan.filters = scan_filter;
	scan.n_filters = RADIANT_SCHED_MAX_FILTERS;
	scan.t_open = t0;
	scan.t_close = RADIANT_TIME_NEVER;
	zassert_ok(radiant_sched_request_rx(1u, &scan));
	post_ed(ED_CH);
	zassert_ok(radiant_sched_tick());

	fake_radio_advance_to(t0 + 2u * (radiant_time_t)ED_PERIOD_US);

	zassert_equal(0u, log.n_dwell,
		      "an ED chunk ran while a background scan wanted the radio");
	zassert_equal(0u, radiant_sched_stats_get()->ed_chunks,
		      "an ED chunk was armed under a scan");
	zassert_true(radiant_sched_stats_get()->scan_chunks > 0u,
		     "the scan did not run either; the test proves nothing");
	zassert_true(radiant_sched_pending(ED_CH),
		     "a starved ED request must survive rather than expire");
	zassert_equal(0u, log.done_missed[ED_CH],
		      "a starved ED request must not be reported as missed");
}

/* An ED request has no deadline, so the expiry sweep must never touch it - the
 * failure this guards is a slot deleted on the first pass after it was posted,
 * with nothing on the air to show for it. */
ZTEST(ed, test_sched_ed_never_expires)
{
	bring_up();
	post_ed(ED_CH);
	zassert_ok(radiant_sched_tick());

	/* No other work at all, and a long time with the clock moving. */
	fake_radio_advance(5u * (uint64_t)ED_PERIOD_US);

	zassert_true(radiant_sched_pending(ED_CH), "the ED request was expired");
	zassert_equal(0u, log.done_missed[ED_CH],
		      "the ED request was reported MISSED");
	zassert_true(log.n_dwell > 0u, "the ED request never measured anything");
}

/*
 * The one way an arbiter can silently end energy-detect scanning for ever:
 * nothing re-posts a struct radiant_sched_ed (unlike a scan or a tracked
 * window). It survives on end_armed() choosing not to consume it, keyed on
 * the done reason - a reason not in that list drops the request silently, no
 * counter, no event, the map just stops updating. Both denial routes are
 * exercised: refused arm calls (no op installed) and accepted-then-DENIED
 * operations.
 */
ZTEST(ed, test_sched_ed_survives_a_denial)
{
	uint32_t dwells_before;

	bring_up();
	post_ed(ED_CH);
	zassert_ok(radiant_sched_tick());
	fake_radio_advance((uint64_t)ED_PERIOD_US);
	dwells_before = log.n_dwell;
	zassert_true(dwells_before > 0u, "the ED request never got going");

	/* Accepted, then never granted. */
	fake_radio_force_terminal_repeat(RADIANT_RADIO_STATUS_DENIED, 4u);
	fake_radio_advance(4u * (uint64_t)ED_PERIOD_US);

	zassert_true(radiant_sched_pending(ED_CH),
		     "a denied chunk consumed the standing ED request");
	zassert_equal(0u, log.done_missed[ED_CH], NULL);
	zassert_equal(0u, log.done_failed[ED_CH],
		      "a denial was reported as a backend fault");
	zassert_true(log.done_denied[ED_CH] > 0u,
		     "the denial was not reported to the owner at all");

	/* Refused at the arm call - the synchronous EDENIED path. */
	fake_radio_force_arm_repeat(RADIANT_RADIO_EDENIED, 4u);
	fake_radio_advance(4u * (uint64_t)ED_PERIOD_US);

	zassert_true(radiant_sched_pending(ED_CH),
		     "a refused arm consumed the standing ED request");

	/* And when the air comes back, so does the sweep. Asserting the request
	 * is still pending is not enough: it has to be armable again, which is
	 * the property a cleared rf_cursor or a stuck in_flight flag would
	 * break without touching the pending bit. */
	fake_radio_advance(4u * (uint64_t)ED_PERIOD_US);
	zassert_true(log.n_dwell > dwells_before,
		     "the ED sweep never resumed after the arbiter let go");
	zassert_equal(0u, fake_radio_viol_count(), "%s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
}

static void ed_before(void *fixture)
{
	ARG_UNUSED(fixture);
	memset(&log, 0, sizeof(log));
	memset(&raw, 0, sizeof(raw));
	radiant_chanmap_reset();
}

static void ed_after(void *fixture)
{
	ARG_UNUSED(fixture);
	radiant_sched_reset();
	(void)radiant_radio_disable();
	fake_radio_reset();
}

ZTEST_SUITE(ed, NULL, NULL, ed_before, ed_after, NULL);
