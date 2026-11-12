/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_search_rf.c - the sweep's frequency axis (release review 1b).
 *
 * A slave on another RF index used to be unable to acquire anything, because
 * the one shared sweep sat on RF 57 whatever its searchers asked for. The
 * sweep now follows the frequency its channels declare via
 * radiant_search_begin_rf(), keeps a whole pass per frequency when they
 * disagree, and offers a channel only devices heard on its own frequency.
 *
 * A separate file from test_search.c on purpose: that suite is the contract
 * for a search in which nobody declares a frequency, and the first test here
 * pins that this still holds with the axis present.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "../fake_radio.h"

#include <radiant/radiant_frame.h>
#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_search.h>

static struct radiant_search g_s;
static struct radiant_search_window g_w;
static uint32_t g_op;
static radiant_time_t g_close; /* the arm log is bounded; keep our own */

#define N_CAP 8
static uint8_t g_acq_chan[N_CAP];
static uint8_t g_acq_rf[N_CAP];
static uint32_t g_n_acq;

static void on_acquired(uint8_t channel, const struct radiant_search_result *r, void *user)
{
	ARG_UNUSED(user);
	ARG_UNUSED(r);
	if (g_n_acq < N_CAP) {
		g_acq_chan[g_n_acq] = channel;
		g_acq_rf[g_n_acq] = radiant_search_rf_get(&g_s);
	}
	g_n_acq++;
}

static const struct radiant_search_cbs cbs = { .acquired = on_acquired };

static void rx_cb(const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(user);
	radiant_search_on_rx(&g_s, evt);
}

static const struct radiant_radio_cbs radio_cbs = { .rx = rx_cb, .tx = NULL };

static const uint8_t payload8[8] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17 };
static const struct radiant_search_id_filter want_any = { 0u, 0u, 0u };

#define RF_A 57u
#define RF_B 66u

static void setup(void *f)
{
	ARG_UNUSED(f);
	fake_radio_reset();
}

ZTEST_SUITE(radiant_search_rf, NULL, NULL, setup, NULL, NULL);

static void bring_up(void)
{
	struct radiant_search_cfg cfg;

	memset(&g_s, 0, sizeof(g_s));
	g_n_acq = 0u;
	radiant_search_cfg_default(&cfg);
	zassert_ok(radiant_search_init(&g_s, &cfg, &cbs, NULL));
	zassert_ok(radiant_radio_init(&radio_cbs, NULL));
	zassert_ok(radiant_radio_enable());
}

static void begin(uint8_t ch, uint8_t rf)
{
	zassert_ok(radiant_search_begin_rf(&g_s, ch, RADIANT_SEARCH_MODE_ACQUIRE,
					   &want_any, radiant_radio_now(),
					   RADIANT_SEARCH_TIMEOUT_NONE, rf));
}

static int win_open(void)
{
	struct radiant_rx_req req;
	radiant_time_t earliest =
		radiant_radio_now() + radiant_radio_caps_get()->min_arm_lead_us + 100u;
	int rc = radiant_search_window(&g_s, earliest, &g_w);

	if (rc != RADIANT_SEARCH_OK) {
		return rc;
	}
	memset(&req, 0, sizeof(req));
	req.fmt = g_w.fmt;
	req.rf_index = g_w.rf_index;
	req.filters = g_w.filters;
	req.n_filters = g_w.n_filters;
	req.t_open = g_w.t_open;
	req.t_close = g_w.t_close;
	req.flags = g_w.flags;
	zassert_ok(radiant_radio_rx(&req, &g_op));
	radiant_search_armed(&g_s, g_op, req.t_open, req.t_close);
	g_close = req.t_close;
	return RADIANT_SEARCH_OK;
}

static void win_run(void)
{
	fake_radio_advance_to(g_close + 1u);
	radiant_search_tick(&g_s, radiant_radio_now());
}

/* One whole round-robin pass, returning every window's frequency in `rf`. */
#if defined(CONFIG_RADIANT_SEARCH_COVERAGE)
/*
 * A RETUNE RESTARTS THE CURSOR ON A NEW FREQUENCY, SO THE MASK GOES WITH IT.
 *
 * The bug this catches is phases heard on one frequency counting as covered
 * on another. Two sets, one on each frequency, each covering only one half of
 * the period: neither set heard every phase on its own frequency, so both are
 * counted uncovered. A mask that was not reset would make the second set's
 * mask full and count it as covered.
 */
#define COV_P RADIANT_SEARCH_COVERAGE_PERIOD_US

static void chunk_at(radiant_time_t open, uint32_t len_us)
{
	struct radiant_search_window w;

	zassert_ok(radiant_search_window(&g_s, open, &w));
	radiant_search_armed(&g_s, RADIANT_SEARCH_OP_EXTERNAL, open,
			     open + (radiant_time_t)len_us);
	radiant_search_on_done(&g_s, true, true, open + (radiant_time_t)len_us);
}

ZTEST(radiant_search_rf, test_coverage_mask_resets_with_a_retune)
{
	const radiant_time_t t0 = (radiant_time_t)COV_P * 8u;
	struct radiant_search_window w;

	bring_up();
	begin(0u, 57u);

	/* First set: the first half of the period, three times over for 375 ms
	 * of credit, so it completes on one half of the phases only. */
	for (uint32_t k = 0u; k < 3u; k++) {
		chunk_at(t0 + (radiant_time_t)k * COV_P, 125000u);
	}
	zassert_true(radiant_search_set_complete(&g_s));

	/* The channel moves to another frequency: the next window retunes and
	 * selects a new set. The first set is counted, the mask is reset. */
	zassert_ok(radiant_search_end(&g_s, 0u));
	begin(1u, 66u);
	zassert_ok(radiant_search_window(&g_s, t0 + 8u * COV_P, &w));
	zassert_equal(66u, w.rf_index, "the sweep did not retune");
	zassert_equal(0u, g_s.phase_mask, "the mask did not reset on the retune");
	zassert_equal(1u, radiant_search_get_stats(&g_s)->sets_advanced_uncovered);
	radiant_search_armed(&g_s, RADIANT_SEARCH_OP_EXTERNAL, t0 + 8u * COV_P + 125000u,
			     t0 + 8u * COV_P + 125000u + 121000u);
	radiant_search_on_done(&g_s, true, true, t0 + 8u * COV_P + 125000u + 121000u);

	/* Second set: only the other half of the period, on the new frequency.
	 * Two more chunks to make up the dwell. */
	for (uint32_t k = 1u; k < 3u; k++) {
		chunk_at(t0 + (radiant_time_t)(k + 8u) * COV_P + 125000u, 121000u);
	}
	zassert_true(radiant_search_set_complete(&g_s));
	zassert_ok(radiant_search_window(&g_s, t0 + 16u * COV_P, &w));
	zassert_equal(2u, radiant_search_get_stats(&g_s)->sets_advanced_uncovered,
		      "phases heard on 57 counted as covered on 66");
	radiant_search_on_done(&g_s, false, false, 0u);
}
#endif /* CONFIG_RADIANT_SEARCH_COVERAGE */

static void run_pass(uint8_t *rf, uint16_t n_sets)
{
	for (uint16_t i = 0u; i < n_sets; i++) {
		zassert_equal(RADIANT_SEARCH_OK, win_open());
		rf[i] = g_w.rf_index;
		win_run();
	}
}

static void expect_pass_on(const uint8_t *rf, uint16_t n, uint8_t want, const char *what)
{
	for (uint16_t i = 0u; i < n; i++) {
		zassert_equal(want, rf[i], "%s: window %u armed on RF %u, want %u", what,
			      (unsigned int)i, (unsigned int)rf[i], (unsigned int)want);
	}
}

/* No channel declared a frequency, or every one declared 57: the sweep never
 * leaves the configured RF, in any window. */
ZTEST(radiant_search_rf, test_undeclared_and_all_57_sessions_never_retune)
{
	uint8_t rf[64];
	uint16_t n;

	bring_up();
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
					radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	n = radiant_search_sets(&g_s);
	run_pass(rf, n);
	run_pass(rf, n);
	expect_pass_on(rf, n, RF_A, "undeclared");
	zassert_ok(radiant_search_end(&g_s, 0u));

	begin(0u, RF_A);
	begin(1u, RF_A);
	run_pass(rf, n);
	run_pass(rf, n);
	expect_pass_on(rf, n, RF_A, "all 57");
	zassert_equal(RF_A, radiant_search_rf_get(&g_s));
	zassert_ok(radiant_search_end(&g_s, 0u));
	zassert_ok(radiant_search_end(&g_s, 1u));
}

/* Every searcher is on 66: the sweep goes to 66 for its very first window
 * (a pass on 57 would be heard by nobody) and stays. */
ZTEST(radiant_search_rf, test_sweep_follows_a_frequency_all_searchers_agree_on)
{
	uint8_t rf[64];
	uint16_t n;

	bring_up();
	begin(0u, RF_B);
	begin(1u, RF_B);
	n = radiant_search_sets(&g_s);
	run_pass(rf, n);
	expect_pass_on(rf, n, RF_B, "agree, pass 1");
	run_pass(rf, n);
	expect_pass_on(rf, n, RF_B, "agree, pass 2");
	zassert_ok(radiant_search_end(&g_s, 0u));
	zassert_ok(radiant_search_end(&g_s, 1u));
}

/* Mixed: whole passes alternate, and the sweep never changes frequency in the
 * middle of one, so each frequency keeps "certain within one pass". */
ZTEST(radiant_search_rf, test_mixed_frequencies_alternate_whole_passes)
{
	uint8_t rf[64];
	uint16_t n;

	bring_up();
	begin(0u, RF_B);
	begin(1u, RF_A);
	n = radiant_search_sets(&g_s);

	run_pass(rf, n);
	expect_pass_on(rf, n, RF_A, "mixed, pass 1");
	run_pass(rf, n);
	expect_pass_on(rf, n, RF_B, "mixed, pass 2");
	run_pass(rf, n);
	expect_pass_on(rf, n, RF_A, "mixed, pass 3");
	zassert_ok(radiant_search_end(&g_s, 0u));
	zassert_ok(radiant_search_end(&g_s, 1u));
}

/*
 * A host cycling keyed frequencies faster than a pass: the only searcher ends
 * and a new one begins on the other frequency every half pass. Each frequency
 * must still visit every set within its first n_sets windows on it. When a
 * retune restarted the cursor, each frequency repeated the first half of its
 * pass forever and the second half was never heard.
 */
ZTEST(radiant_search_rf, test_a_frequency_resumes_its_own_cursor_after_a_retune)
{
	bool seen[2][256];
	uint16_t windows_on[2] = { 0u, 0u };
	uint16_t n;
	uint16_t half;

	bring_up();
	memset(seen, 0, sizeof(seen));
	n = radiant_search_sets(&g_s);
	zassert_true(n <= 256u);
	half = (n > 1u) ? (uint16_t)(n / 2u) : 1u;

	for (uint16_t round = 0u; round < 4u; round++) {
		uint8_t rf = (round % 2u == 0u) ? RF_A : RF_B;
		uint8_t k = (round % 2u == 0u) ? 0u : 1u;

		begin(0u, rf);
		for (uint16_t i = 0u; i < half; i++) {
			zassert_equal(RADIANT_SEARCH_OK, win_open());
			zassert_equal(rf, g_w.rf_index, "round %u window %u on RF %u",
				      (unsigned int)round, (unsigned int)i,
				      (unsigned int)g_w.rf_index);
			if (windows_on[k] < n) {
				seen[k][g_w.set_index] = true;
			}
			windows_on[k]++;
			win_run();
		}
		zassert_ok(radiant_search_end(&g_s, 0u));
	}

	for (uint8_t k = 0u; k < 2u; k++) {
		for (uint16_t set = 0u; set < n; set++) {
			zassert_true(seen[k][set],
				     "RF %u: set %u not visited in its first %u windows",
				     (unsigned int)(k == 0u ? RF_A : RF_B),
				     (unsigned int)set, (unsigned int)n);
		}
	}
}

/* More frequencies than RADIANT_SEARCH_RF_CURSORS: the oldest parked cursor
 * of a frequency nobody declares is dropped (that frequency restarts at set
 * 0), and the others still resume. */
ZTEST(radiant_search_rf, test_a_full_cursor_table_drops_the_oldest_undeclared)
{
	const uint8_t first = 40u;

	bring_up();
	if (radiant_search_sets(&g_s) < 4u) {
		ztest_test_skip(); /* one set: nothing to resume */
	}
	/* RADIANT_SEARCH_RF_CURSORS + 2 frequencies, two windows each: every
	 * one is left at set 2. */
	for (uint8_t f = 0u; f < (uint8_t)(RADIANT_SEARCH_RF_CURSORS + 2u); f++) {
		begin(0u, (uint8_t)(first + f));
		for (uint8_t i = 0u; i < 2u; i++) {
			zassert_equal(RADIANT_SEARCH_OK, win_open());
			zassert_equal(i, g_w.set_index);
			win_run();
		}
		zassert_ok(radiant_search_end(&g_s, 0u));
	}

	begin(0u, (uint8_t)(first + 1u));
	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(2u, g_w.set_index, "a parked cursor was not resumed");
	win_run();
	zassert_ok(radiant_search_end(&g_s, 0u));

	begin(0u, first);
	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(0u, g_w.set_index, "the oldest cursor should have been dropped");
	win_run();
	zassert_ok(radiant_search_end(&g_s, 0u));
}

/*
 * A steer queued for the other frequency waits for it. Channel 0 is on 66 and
 * names a device; channel 1 on 57 searches for anything, so the sweep starts
 * with a whole pass on 57. That pass must be pure round robin: taking the
 * steer there spends a dwell on a set nobody on 57 asked for, and ticking
 * channel 0's revisits there doubles its backoff for visits that never
 * happened. After the retune, the first 66 window is the steered set.
 */
ZTEST(radiant_search_rf, test_a_steer_for_the_other_frequency_waits_for_it)
{
	const uint16_t devnum = 0x0A09u;
	const struct radiant_search_id_filter want_dev = { devnum, 0u, 0u };
	uint16_t n;
	uint16_t steer_set;

	bring_up();
	zassert_ok(radiant_search_begin_rf(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE,
					   &want_dev, radiant_radio_now(),
					   RADIANT_SEARCH_TIMEOUT_NONE, RF_B));
	begin(1u, RF_A);
	n = radiant_search_sets(&g_s);
	steer_set = radiant_search_set_for_devnum(&g_s, devnum);
	zassert_equal(1u, g_s.n_steer, "the named device was not queued");

	for (uint16_t i = 0u; i < n; i++) {
		zassert_equal(RADIANT_SEARCH_OK, win_open());
		zassert_equal(RF_A, g_w.rf_index);
		zassert_equal(i, g_w.set_index,
			      "57 window %u took set %u: a 66 steer was spent on 57",
			      (unsigned int)i, (unsigned int)g_w.set_index);
		zassert_false(g_w.from_cache);
		win_run();
	}
	zassert_equal(0u, radiant_search_get_stats(&g_s)->cache_steers,
		      "a steer was taken during the 57 pass");
	zassert_equal(RADIANT_SEARCH_REVISIT_FIRST, g_s.chans[0].revisit_gap,
		      "channel 0's backoff grew to %u while the sweep was on 57",
		      (unsigned int)g_s.chans[0].revisit_gap);
	zassert_equal(1u, g_s.n_steer, "the 66 steer was lost");

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(RF_B, g_w.rf_index, "the pass on 57 did not hand over to 66");
	zassert_equal(steer_set, g_w.set_index, "the first 66 window was not the steer");
	zassert_true(g_w.from_cache);
	zassert_equal(1u, radiant_search_get_stats(&g_s)->cache_steers);
	win_run();

	zassert_ok(radiant_search_end(&g_s, 0u));
	zassert_ok(radiant_search_end(&g_s, 1u));
}

/* A steer for a frequency nobody declares any more is dropped at the next
 * boundary, so the queue is bounded by what is still wanted. */
ZTEST(radiant_search_rf, test_a_steer_for_an_abandoned_frequency_is_dropped)
{
	const struct radiant_search_id_filter want_dev = { 0x0A09u, 0u, 0u };

	bring_up();
	begin(1u, RF_A);
	zassert_ok(radiant_search_begin_rf(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE,
					   &want_dev, radiant_radio_now(),
					   RADIANT_SEARCH_TIMEOUT_NONE, RF_B));
	zassert_equal(1u, g_s.n_steer);
	zassert_ok(radiant_search_end(&g_s, 0u));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(0u, g_s.n_steer, "a steer for RF %u outlived its channel", RF_B);
	zassert_false(g_w.from_cache);
	win_run();
	zassert_ok(radiant_search_end(&g_s, 1u));
}

/* When the last searcher on the current frequency leaves, the sweep moves on
 * at once rather than finishing a pass nobody can hear. */
ZTEST(radiant_search_rf, test_the_sweep_leaves_a_frequency_nobody_searches)
{
	uint8_t rf[4];

	bring_up();
	begin(0u, RF_B);
	begin(1u, RF_A);
	run_pass(rf, 2u);
	expect_pass_on(rf, 2u, RF_A, "before");
	zassert_ok(radiant_search_end(&g_s, 1u));
	run_pass(rf, 2u);
	expect_pass_on(rf, 2u, RF_B, "after the 57 searcher left");
	zassert_ok(radiant_search_end(&g_s, 0u));
}

/* Review 2026-11-09: a keyless scan on channel 1 and a keyed search on 66 on
 * channel 0. When the keyed search ends, nobody declares a frequency any more,
 * and the sweep must go back to 57 for the scan. It used to stay on 66 until
 * reset, deaf to every 57 device the scan was looking for. */
ZTEST(radiant_search_rf, test_the_sweep_returns_home_when_the_last_declared_search_ends)
{
	uint8_t rf[64];
	uint16_t n;

	bring_up();
	zassert_ok(radiant_search_begin(&g_s, 1u, RADIANT_SEARCH_MODE_SCAN, &want_any,
					radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	begin(0u, RF_B);
	n = radiant_search_sets(&g_s);
	run_pass(rf, n);
	expect_pass_on(rf, n, RF_B, "while the 66 search runs");
	zassert_ok(radiant_search_end(&g_s, 0u));
	run_pass(rf, n);
	expect_pass_on(rf, n, RF_A, "after the 66 search ended");
	zassert_equal(RF_A, radiant_search_rf_get(&g_s));
	zassert_ok(radiant_search_end(&g_s, 1u));
}

/* Offer filtering, live. Channel 0 (first in channel order, so it would take
 * the frame) is on 66; channel 1 on 57. The sweep is on 57, so a device heard
 * there is channel 1's and must not be offered to channel 0. */
ZTEST(radiant_search_rf, test_a_channel_is_offered_only_devices_heard_on_its_frequency)
{
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;

	bring_up();
	begin(0u, RF_B);
	begin(1u, RF_A);

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(RF_A, g_w.rf_index);
	len = fake_radio_build_ant_frame(frame, 0x0703u, 0x78u, 1u, payload8);
	zassert_ok(fake_radio_air_frame(g_w.t_open + 1000u, frame, len));
	win_run();

	zassert_equal(1u, g_n_acq, "the device was not acquired at all");
	zassert_equal(1u, g_acq_chan[0],
		      "channel %u took a device heard on RF %u; it listens on %u",
		      (unsigned int)g_acq_chan[0], RF_A, RF_B);
	zassert_true(radiant_search_is_searching(&g_s, 0u));
	zassert_false(radiant_search_is_searching(&g_s, 1u));
	zassert_ok(radiant_search_end(&g_s, 0u));
}

/* Offer filtering, cached. A device heard on 57 is in the seen cache; a channel
 * that then begins searching on 66 for anything must not be steered to it. */
ZTEST(radiant_search_rf, test_the_seen_cache_does_not_steer_across_frequencies)
{
	struct radiant_channel_id id;
	uint16_t steered;

	bring_up();
	memset(&id, 0, sizeof(id));
	id.device_number = 0x0A09u; /* set 0x09 / 8 = 1 on the nRF geometry */
	id.device_type = 0x0Bu;
	id.trans_type = 5u;
	radiant_search_seen_note_rf(&g_s, &id, radiant_radio_now(), RF_A);

	/* Same frequency: steered (control, so the filter is what is tested). */
	zassert_ok(radiant_search_begin_rf(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE,
					   &want_any, radiant_radio_now(),
					   RADIANT_SEARCH_TIMEOUT_NONE, RF_A));
	zassert_equal(1u, g_s.n_steer, "control: a same-frequency cache hit must steer");
	steered = g_s.steer[0];
	zassert_equal(radiant_search_set_for_devnum(&g_s, 0x0A09u), steered);
	zassert_ok(radiant_search_end(&g_s, 0u));
	g_s.n_steer = 0u;

	/* Other frequency: not steered. */
	zassert_ok(radiant_search_begin_rf(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE,
					   &want_any, radiant_radio_now(),
					   RADIANT_SEARCH_TIMEOUT_NONE, RF_B));
	zassert_equal(0u, g_s.n_steer,
		      "a device heard on RF %u steered a channel on RF %u", RF_A, RF_B);
	zassert_ok(radiant_search_end(&g_s, 0u));
}

/*
 * A pass that ends while nobody (or nobody with a declared frequency) is
 * searching must not leave its "pass done" flag behind: the next mixed session
 * would otherwise leave its first frequency at the very first boundary instead
 * of after a whole pass on it.
 */
/* `windows` is what is left of the pass the cursor is part-way through. */
static void expect_mixed_session_stays_for(uint16_t windows)
{
	uint8_t rf[64];

	begin(0u, RF_B);
	begin(1u, RF_A);
	run_pass(rf, windows);
	expect_pass_on(rf, windows, RF_A, "first mixed pass after a stale wrap");
	zassert_ok(radiant_search_end(&g_s, 0u));
	zassert_ok(radiant_search_end(&g_s, 1u));
}

/* No radiant_search_window() between the end and the next session: the API
 * asks for no window while nobody searches, so the ENOENT path that used to
 * clear the flag is never taken there. */
ZTEST(radiant_search_rf, test_a_pass_that_ended_with_no_searchers_does_not_leak)
{
	uint8_t rf[64];
	uint16_t n;

	bring_up();
	begin(0u, RF_A);
	n = radiant_search_sets(&g_s);
	run_pass(rf, n); /* selecting the last set wraps the cursor */
	zassert_ok(radiant_search_end(&g_s, 0u));

	expect_mixed_session_stays_for(n);
}

/* The same wrap, but the session ends by ACQUISITION in the last set of the
 * pass rather than by radiant_search_end(). */
ZTEST(radiant_search_rf, test_a_pass_that_ended_by_acquisition_does_not_leak)
{
	uint8_t rf[64];
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;
	uint16_t n;
	uint16_t lo = 0u;

	bring_up();
	begin(0u, RF_A);
	n = radiant_search_sets(&g_s);
	/* A device in the pass's last set, whatever the geometry. */
	while (lo < 0x100u &&
	       radiant_search_set_for_devnum(&g_s, (uint16_t)(0x0700u | lo)) != n - 1u) {
		lo++;
	}
	zassert_true(lo < 0x100u, "no devnum_lo lands in set %u", (unsigned int)(n - 1u));

	run_pass(rf, n - 1u);
	zassert_equal(RADIANT_SEARCH_OK, win_open()); /* the last set: the cursor wraps */
	zassert_equal(n - 1u, g_w.set_index);
	len = fake_radio_build_ant_frame(frame, (uint16_t)(0x0700u | lo), 0x78u, 1u, payload8);
	zassert_ok(fake_radio_air_frame(g_w.t_open + 1000u, frame, len));
	win_run();
	zassert_equal(1u, g_n_acq, "the device in the last set was not acquired");
	zassert_equal(0u, radiant_search_n_searching(&g_s));

	expect_mixed_session_stays_for(n);
}

ZTEST(radiant_search_rf, test_a_pass_that_ended_undeclared_does_not_leak)
{
	uint8_t rf[64];
	uint16_t n;

	bring_up();
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
					radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	n = radiant_search_sets(&g_s);
	run_pass(rf, n + 1u); /* one window past the wrap: the cursor is at set 1 */
	zassert_ok(radiant_search_end(&g_s, 0u));

	expect_mixed_session_stays_for(n - 1u);
}
