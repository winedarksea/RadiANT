/* SPDX-License-Identifier: Apache-2.0 */
/*
 * RTC1's own cases (power plan pass B5): the nRF52840's LF counter under
 * CONFIG_RADIANT_NRF_LOW_POWER, in the radiant.backend.lp_rtc scenario only
 * (backend_lp_under_test.c with LPT_RTC). Every shared case in src_lp/ runs
 * here as well; these cover what only RTC1 and the nRF52 TIMER have:
 *
 *   - a driver that refuses to schedule more than 2^23 ticks (256 s) ahead,
 *     and extended counts that run through 2^24 (RTC1's 24-bit wrap, every
 *     512 s); the fake driver returns a perfect 64-bit count, so these test
 *     the backend's arithmetic across the wrap, not the driver's overflow
 *     handling;
 *   - the driver's 3-tick minimum for an exact compare (the product
 *     specification's 2-tick guarantee is not enough for nrf_rtc_timer);
 *   - TIMER SHUTDOWN after STOP (nRF52 anomaly 78), which zeroes the counter
 *     - and which this model also lets overwrite CC5, so a release that read
 *     its capture after the halt would show;
 *   - 30.5 us ticks, so a software TIMER start must wait for the counter's
 *     step to keep its error below a microsecond.
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_lp_probe.h"

#define ARM_LEAD   230u
#define FAR_US     100000u
#define NEAR_US    3000u
#define RTC_WRAP   (1ull << 24)

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	lpt_reset();
}

ZTEST_SUITE(radiant_backend_lp_rtc, NULL, NULL, before, NULL, NULL);

static uint64_t t_start_world(uint64_t t_sync)
{
	return LPT_W0 + t_sync - lpt_tx_lead();
}

/* Release with nothing armed: a far arm, aborted, then the release edge. */
static void sleep_idle(void)
{
	zassert_equal(lpt_tx_at(lpt_radio_now() + FAR_US), RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	lpt_run(500u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
}

/* Run to `world` in steps, checking the timebase against world time. */
static void run_checked(uint64_t world, uint64_t step_us)
{
	uint64_t last = lpt_radio_now();

	while (lpt_world() + step_us < world) {
		uint64_t t;

		lpt_run(step_us);
		t = lpt_radio_now();
		zassert_true(t >= last, "time ran backwards: %llu < %llu",
			     (unsigned long long)t, (unsigned long long)last);
		zassert_within(t, lpt_world() - LPT_W0, 1u + LPT_ASLEEP_SLACK_US,
			       "radio %llu world %llu", (unsigned long long)t,
			       (unsigned long long)(lpt_world() - LPT_W0));
		last = t;
	}
	lpt_run_to(world);
}

/*
 * Risk: the extended count crosses 2^24 (where RTC1's COUNTER wraps) in the
 * middle of a sleep. The backend's timebase must run straight through it - no
 * step, no range error, no 24- or 32-bit truncation - and the wake must land
 * on its microsecond, then the next period's release take the LF image as
 * usual. (The driver's own wrap handling is not modelled.)
 */
ZTEST(radiant_backend_lp_rtc, test_rtc_sleep_across_24bit_wrap)
{
	struct lpt_stats st;
	uint64_t t_sync;
	uint64_t start;

	/* The wrap falls ~50 ms after boot, inside the 100 ms sleep below. */
	lpt_reset_lf_offset(RTC_WRAP - 1700u);
	t_sync = lpt_radio_now() + FAR_US;
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	lpt_run(200u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
	zassert_true(lpt_lf_ticks() < RTC_WRAP, "asleep before the wrap");

	run_checked(t_start_world(t_sync) - ARM_LEAD + 1u, 997u);
	zassert_true(lpt_lf_ticks() > RTC_WRAP, "and awake after it");
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1, "TXEN %llu wanted %llu",
		       (unsigned long long)start,
		       (unsigned long long)t_start_world(t_sync));
	lpt_tx_end();
	zassert_equal(lpt_probe()->last_tx_status, RADIANT_RADIO_STATUS_OK);

	lpt_run(500u);
	lpt_stats(&st);
	zassert_equal(st.sleeps, 2);
	zassert_equal(st.lf_taken, 2, "both releases took the LF image");
	zassert_equal(st.disagreements, 0);
	zassert_equal(st.range_errors, 0);
	zassert_equal(st.late_anchors, 0);
	zassert_equal(st.late_wakes, 0);
}

/*
 * Risk: an idle sleep (nothing armed) longer than a whole RTC1 period
 * (512 s). Asleep readings convert the driver's extended count, so the
 * timebase stays exact across both wraps with no HFXO at all, and the next
 * operation after it is on time.
 */
ZTEST(radiant_backend_lp_rtc, test_rtc_idle_beyond_counter_period)
{
	struct lpt_stats st;
	uint64_t lf0;
	uint64_t t_sync;
	uint64_t start;
	uint32_t req0;

	sleep_idle();
	lf0 = lpt_lf_ticks();
	req0 = lpt_hfxo_requests();
	run_checked(lpt_world() + 600000000ull, 50000000ull);   /* 600 s */
	zassert_equal(lpt_state(), LPT_ASLEEP);
	zassert_true(lpt_lf_ticks() - lf0 > RTC_WRAP, "more than a counter period");
	zassert_equal(lpt_hfxo_requests(), req0, "no HFXO for nothing");

	t_sync = lpt_radio_now() + FAR_US;
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	lpt_run_to(t_start_world(t_sync) - ARM_LEAD + 1u);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	lpt_stats(&st);
	zassert_equal(st.range_errors, 0);
}

/*
 * Risk: an operation beyond the driver's schedule span (2^23 ticks, 256 s).
 * Every compare the backend writes must stay inside it - a refused one would
 * be a late anchor, or a wake that never comes - so the wake is reached by
 * HFXO-less rebases at most 128 s apart, and is on time.
 */
ZTEST(radiant_backend_lp_rtc, test_rtc_wake_beyond_schedule_span)
{
	struct lpt_hw hw;
	struct lpt_stats st;
	uint64_t t_sync = lpt_radio_now() + 300000000ull;   /* 300 s */
	uint64_t start;
	uint32_t req0 = lpt_hfxo_requests();

	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	lpt_run(200u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
	run_checked(t_start_world(t_sync) - ARM_LEAD + 1u, 10000000ull);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	lpt_hw(&hw);
	zassert_equal(hw.span_refusals, 0, "every compare inside the driver's span");
	zassert_true(hw.hf_sets >= 3u, "two rebases and EDGE 1 (%u)", hw.hf_sets);
	zassert_equal(lpt_hfxo_requests(), req0 + 1u, "the HFXO for the wake only");
	lpt_stats(&st);
	zassert_equal(st.wakes, 1);
	zassert_equal(st.late_anchors, 0);
}

/*
 * Risk: the minimum lead. The backend writes its edges 4 ticks ahead of its
 * own counter read so that a step between that read and the driver's (the
 * common case: a few us of code at 30.5 us a tick) still leaves the driver's
 * 3. Two steps - a real preemption - leave 2: inside the product
 * specification's guarantee, but not the driver's, and exact_set() refuses.
 * That is a late anchor at a point where nothing has stopped, so the backend
 * stays awake, programmes the operation on the running TIMER, and the refused
 * compare (written and routed by the driver before it refused) is withdrawn
 * and never fires.
 */
ZTEST(radiant_backend_lp_rtc, test_rtc_exact_compare_two_ticks_ahead_refused)
{
	struct lpt_hw hw0, hw;
	struct lpt_stats st;
	uint64_t t_sync;
	uint64_t start;

	lpt_run_to_next_lf_tick(1u);
	lpt_hw(&hw0);
	lpt_delay_next_anchor_set(2u * LPT_TICK_US);   /* two steps pass: 2 ticks ahead */
	t_sync = lpt_radio_now() + FAR_US;
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_false(lpt_staged(), "granted at once");
	zassert_equal(lpt_state(), LPT_AWAKE);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	lpt_hw(&hw);
	zassert_equal(hw.exact_refusals, hw0.exact_refusals + 1u);
	lpt_stats(&st);
	zassert_equal(st.late_anchors, 1);
	zassert_equal(st.sleeps, 0);
	lpt_run(500u);
	zassert_equal(lpt_state(), LPT_AWAKE, "the refused compare was withdrawn");
	zassert_true(lpt_timer_running());
	lpt_stats(&st);
	zassert_equal(st.spurious, 0);
}

/*
 * The case the extra tick is for: one step between the backend's read and
 * the driver's leaves 3 ticks, and the release is taken. (With a 3-tick write
 * lead this would be refused - about one release in ten on hardware.)
 */
ZTEST(radiant_backend_lp_rtc, test_rtc_step_between_reads_still_releases)
{
	struct lpt_hw hw0, hw;
	struct lpt_stats st;
	uint64_t t_sync;
	uint64_t start;

	lpt_hw(&hw0);
	zassert_equal(hw0.min_exact_ahead, 4u, "edges are written 4 ticks ahead");
	lpt_run_to_next_lf_tick(1u);
	lpt_delay_next_anchor_set(LPT_TICK_US);   /* one step passes: 3 ticks ahead */
	t_sync = lpt_radio_now() + FAR_US;
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_true(lpt_staged());
	zassert_equal(lpt_state(), LPT_RELEASING);
	lpt_run(200u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
	lpt_hw(&hw);
	zassert_equal(hw.exact_refusals, hw0.exact_refusals);
	zassert_equal(hw.min_exact_ahead, 3u, "taken at the driver's minimum");
	lpt_run_to(t_start_world(t_sync) - ARM_LEAD + 1u);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	lpt_hw(&hw);
	zassert_true(hw.min_exact_ahead >= 3u);
	zassert_equal(hw.exact_refusals, hw0.exact_refusals, "no anchoring write refused");
	lpt_stats(&st);
	zassert_equal(st.late_anchors, 0);
}

/*
 * Risk: nRF52 anomaly 78. Every release stops the TIMER and then SHUTS IT
 * DOWN, which zeroes its counter; the release must have read its CC5
 * capture before that (this model overwrites CC5 at SHUTDOWN, so a late read
 * would refuse the sleep or take a garbage anchor), EDGE 2 must start the
 * TIMER again from zero, and the timebase must come out exact, period after
 * period.
 */
ZTEST(radiant_backend_lp_rtc, test_rtc_release_shuts_timer_down)
{
	struct lpt_hw hw;
	struct lpt_stats st;
	struct lpt_probe *p = lpt_probe();
	uint64_t start;

	p->period_us = 246277u;   /* 8070/32768 s */
	p->next_sync = lpt_radio_now() + FAR_US;
	zassert_equal(lpt_tx_at(p->next_sync), RADIANT_RADIO_OK_RC);
	lpt_hw(&hw);
	zassert_equal(hw.shutdowns, 0);
	lpt_run(200u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
	lpt_hw(&hw);
	zassert_equal(hw.shutdowns, 1);
	zassert_equal(hw.stop_then_shutdown, 1, "SHUTDOWN straight after STOP");
	zassert_false(lpt_timer_running());
	zassert_equal(hw.timer_count, 0u, "SHUTDOWN zeroed the counter");
	zassert_equal(hw.cc5, 0xFFFFFFFFu, "CC5 primed for EDGE 2 after the halt");
	lpt_stats(&st);
	zassert_equal(st.lf_taken, 1, "the capture was read before SHUTDOWN");

	for (uint32_t i = 0u; i < 3u; i++) {
		uint64_t want = t_start_world(p->next_sync);

		lpt_run_to(want - ARM_LEAD + 1u);
		zassert_true(lpt_tx_programmed(&start), "period %u", i);
		zassert_within(start, want, 1, "period %u: TXEN %llu wanted %llu", i,
			       (unsigned long long)start, (unsigned long long)want);
		lpt_tx_end();
		lpt_run(500u);
		zassert_equal(lpt_state(), LPT_ASLEEP, "period %u", i);
	}
	lpt_hw(&hw);
	zassert_equal(hw.shutdowns, 4);
	zassert_equal(hw.stop_then_shutdown, 4);
	lpt_stats(&st);
	zassert_equal(st.lf_taken, 4);
	zassert_equal(st.disagreements, 0);
	zassert_equal(st.late_anchors, 0);
	zassert_equal(p->tx_events, 3);
}

/*
 * Risk: the other halt. A wake cancelled after EDGE 2's PPI already started
 * the TIMER (interrupt held off) must put it back shut down, not merely
 * stopped - anomaly 78 applies to every stop that precedes a sleep.
 */
ZTEST(radiant_backend_lp_rtc, test_rtc_cancelled_wake_shuts_timer_down)
{
	struct lpt_hw hw0, hw;
	uint64_t t_sync = lpt_radio_now() + FAR_US;
	uint64_t e2;

	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	lpt_run(200u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
	zassert_true(lpt_anchor_world(&e2), "EDGE 2 armed");
	lpt_hold_isr_until(e2 + 50u);
	lpt_run_to(e2 + 10u);
	zassert_true(lpt_timer_running(), "the PPI started it on the tick");
	lpt_hw(&hw0);
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	lpt_hw(&hw);
	zassert_false(lpt_timer_running());
	zassert_equal(hw.shutdowns, hw0.shutdowns + 1u, "shut down, not just stopped");
	zassert_equal(hw.timer_count, 0u);
	lpt_run(2000u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
	zassert_false(lpt_hfxo_on());
}

/*
 * Risk: a software start on a coarse counter. After a missed EDGE 2 the
 * TIMER is started in software from an LF reading; read mid-tick, that
 * reading lags the start by up to 30.5 us, an error the next release would
 * keep for good. The start must wait for the counter's step, so the TIMER's
 * segment begins exactly on a tick, and must carry that tick's
 * sub-microsecond part (dropped, the timebase ends up to 2 us behind at some
 * phases), so the operation after the recovery is on its microsecond.
 * Swept over 16 phases between RTC1 ticks and microseconds: the dropped
 * fraction passes at some of them by luck.
 */
ZTEST(radiant_backend_lp_rtc, test_rtc_software_start_waits_for_the_step)
{
	for (uint32_t k = 0u; k < 16u; k++) {
		struct lpt_hw hw0, hw;
		struct lpt_stats st;
		uint64_t t_sync;
		uint64_t start;

		lpt_reset();
		lpt_run(k * 37u);
		t_sync = lpt_radio_now() + FAR_US + k * 13u;
		zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
		lpt_run(200u);
		zassert_equal(lpt_state(), LPT_ASLEEP);
		lpt_hw(&hw0);
		lpt_break_next_dppi();
		lpt_run_to(t_start_world(t_sync) - ARM_LEAD + 1u);
		lpt_stats(&st);
		zassert_equal(st.late_anchors, 1, "phase %u", k);
		zassert_equal(lpt_probe()->failed_events, 1, "phase %u", k);
		lpt_hw(&hw);
		zassert_equal(hw.wait_edges, hw0.wait_edges + 1u,
			      "phase %u: the software start waited", k);
		zassert_equal(hw.timer_t0_world_q % 15625u, 0u,
			      "phase %u: the TIMER started on an RTC1 step", k);

		/* Nothing armed: the release takes the TIMER's reading. */
		lpt_run(500u);
		zassert_equal(lpt_state(), LPT_ASLEEP, "phase %u", k);
		zassert_within(lpt_radio_now(), lpt_world() - LPT_W0,
			       1u + LPT_ASLEEP_SLACK_US, "phase %u", k);
		t_sync = lpt_radio_now() + FAR_US;
		zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
		lpt_run_to(t_start_world(t_sync) - ARM_LEAD + 1u);
		zassert_true(lpt_tx_programmed(&start), "phase %u", k);
		zassert_within(start, t_start_world(t_sync), 1,
			       "phase %u: TXEN %llu wanted %llu", k,
			       (unsigned long long)start,
			       (unsigned long long)t_start_world(t_sync));
	}
}
