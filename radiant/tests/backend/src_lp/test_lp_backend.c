/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The nRF backend with CONFIG_RADIANT_NRF_LOW_POWER (power plan passes B3 and
 * B5, src/radiant_radio_nrf_lp.c), against a simulated LF counter, HFXO and
 * TIMER (backend_lp_under_test.c). Each case names the risk it covers. Every
 * case here runs in both scenarios: radiant.backend.lp (the nRF54L15 GRTC)
 * and radiant.backend.lp_rtc (the nRF52840 RTC1); src_lp_rtc/ adds RTC1's
 * own. What differs between them comes from backend_lp_probe.h (LPT_*).
 *
 * The numbers: the wake lead is LP_ARM_LEAD_US (ARM_SETUP_US 80 + 150 = 230)
 * after EDGE 2 and LPT_HFXO_LEAD from EDGE 1 to EDGE 2 (800 + 100 = 900 on
 * the GRTC, 500 + 100 = 600 on RTC1). On the GRTC the backend sleeps when the
 * next operation is more than 1132 (wake lead) + 217 (LF overhead) + 3000
 * (keep-on) = 4349 us away; on RTC1, 892 + 275 + 3000 = 4167 us. The
 * simulated HFXO starts in LPT_HFXO_STARTUP (spike B1's maximum on each
 * board: 680, 418 us) unless a case says otherwise. RTC1's wake edges fall on
 * 30.5 us ticks, up to one tick early, so times "before an edge" allow
 * LPT_TICK_US per edge.
 */

#include <errno.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_lp_probe.h"

#define ARM_LEAD   230u
#define HFXO_LEAD  LPT_HFXO_LEAD
#define FAR_US     100000u
#define NEAR_US    3000u

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	lpt_reset();
}

ZTEST_SUITE(radiant_backend_lp, NULL, NULL, before, NULL, NULL);

static uint64_t t_start_world(uint64_t t_sync)
{
	return LPT_W0 + t_sync - lpt_tx_lead();
}

/* Arm a transmit far ahead and run until the backend is asleep on it. */
static uint64_t arm_far_and_sleep(void)
{
	uint64_t t_sync = lpt_radio_now() + FAR_US;

	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_true(lpt_staged(), "a far arm is staged, not programmed");
	zassert_equal(lpt_state(), LPT_RELEASING);
	lpt_run(200u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
	return t_sync;
}

/*
 * Risk: the core path - a far arm sleeps, the TIMER stops, the HFXO and
 * CONSTLAT are released, and the two-edge wake brings the TIMER back on an
 * exact anchor in time to programme the operation where it was asked for.
 */
ZTEST(radiant_backend_lp, test_far_arm_sleeps_and_fires_on_time)
{
	struct lpt_stats st;
	uint64_t start;
	uint64_t t_sync = arm_far_and_sleep();
	uint64_t ts = t_start_world(t_sync);

	zassert_false(lpt_hfxo_on(), "HFXO released while asleep");
	zassert_false(lpt_timer_running(), "TIMER stopped while asleep");
	zassert_equal(lpt_constlat_refs(), 0, "CONSTLAT freed between slots");
	zassert_false(lpt_tx_programmed(&start));

	lpt_run_to(ts - ARM_LEAD - HFXO_LEAD - 2u * LPT_TICK_US - 2u);
	zassert_equal(lpt_state(), LPT_ASLEEP, "EDGE 1 not before its time");
	lpt_run_to(ts - ARM_LEAD - LPT_TICK_US - 1u);
	zassert_equal(lpt_state(), LPT_WAKING, "EDGE 1 requested the HFXO");
	zassert_true(lpt_hfxo_on());
	zassert_equal(lpt_constlat_refs(), LPT_CONSTLAT_ON,
		      "CONSTLAT back before the radio runs");
	lpt_run_to(ts - ARM_LEAD + 1u);
	zassert_equal(lpt_state(), LPT_AWAKE);
	zassert_true(lpt_tx_programmed(&start), "EDGE 2 granted the staged transmit");
	zassert_within(start, ts, 1, "TXEN at %llu, wanted %llu",
		       (unsigned long long)start, (unsigned long long)ts);
	zassert_true(lpt_timer_at_anchor(), "the TIMER started on the edge");

	lpt_stats(&st);
	zassert_equal(st.sleeps, 1);
	zassert_equal(st.wakes, 1);
	zassert_equal(st.late_anchors, 0);
	zassert_equal(st.late_wakes, 0);
	zassert_equal(st.max_wake_late_us, 0);
	zassert_equal(st.lf_taken, 1, "the release took the LF image");
	zassert_equal(st.max_disagree_us, 0);

	lpt_tx_end();
	zassert_equal(lpt_probe()->tx_events, 1);
	zassert_equal(lpt_probe()->last_tx_status, RADIANT_RADIO_STATUS_OK);
	zassert_equal(lpt_state(), LPT_RELEASING,
		      "nothing armed after the terminal: sleep again");
}

/* Risk: a gap below the threshold must cost nothing - same as today. */
ZTEST(radiant_backend_lp, test_near_arm_stays_awake)
{
	struct lpt_stats st;
	uint64_t start;
	uint64_t t_sync = lpt_radio_now() + NEAR_US;

	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_false(lpt_staged());
	zassert_equal(lpt_state(), LPT_AWAKE);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	zassert_true(lpt_hfxo_on());
	lpt_stats(&st);
	zassert_equal(st.sleeps, 0);
}

/*
 * Risk: the timebase across many sleeps - radiant_radio_now() never runs
 * backwards, never drifts from the LF image (the rates are equal here, so any
 * transition error shows), every period's transmit lands on its microsecond,
 * and the HFXO is held for a small fraction of the time (Gate G1's < 3 %).
 */
ZTEST(radiant_backend_lp, test_periodic_master_monotonic_and_on_time)
{
	struct lpt_probe *p = lpt_probe();
	struct lpt_stats st;
	uint64_t last = 0u;
	uint64_t w_begin;
	uint64_t on_begin;
	const uint32_t periods = 20u;

	p->period_us = 246277u;   /* 8070/32768 s */
	p->next_sync = lpt_radio_now() + NEAR_US;
	zassert_equal(lpt_tx_at(p->next_sync), RADIANT_RADIO_OK_RC);
	w_begin = lpt_world();
	on_begin = lpt_hfxo_on_us();

	for (uint32_t i = 0u; i < periods; i++) {
		uint64_t start;
		uint64_t want = t_start_world(p->next_sync);
		uint32_t guard = 0u;

		while (!lpt_tx_programmed(&start)) {
			uint64_t t;

			lpt_run(997u);
			t = lpt_radio_now();
			zassert_true(t >= last, "time ran backwards: %llu < %llu",
				     (unsigned long long)t, (unsigned long long)last);
			zassert_within(t, lpt_world() - LPT_W0, 3u + LPT_ASLEEP_SLACK_US,
				       "radio %llu world %llu", (unsigned long long)t,
				       (unsigned long long)(lpt_world() - LPT_W0));
			last = t;
			zassert_true(++guard < 400u, "period %u never armed", i);
		}
		zassert_within(start, want, 1, "period %u: TXEN %llu wanted %llu", i,
			       (unsigned long long)start, (unsigned long long)want);
		lpt_tx_end();
		zassert_equal(p->nested_rc, RADIANT_RADIO_OK_RC);
	}
	lpt_stats(&st);
	zassert_equal(p->tx_events, periods);
	zassert_true(st.sleeps >= periods - 1u, "sleeps %u", st.sleeps);
	zassert_equal(st.late_anchors, 0);
	zassert_equal(st.late_wakes, 0);
	zassert_equal(st.range_errors, 0);
	zassert_equal(st.disagreements, 0);
	zassert_true(st.max_disagree_us <= 1u);
	zassert_true(st.max_wake_slack_us <= ARM_LEAD + LPT_TICK_US, "slack %u",
		     st.max_wake_slack_us);
	{
		uint64_t on = lpt_hfxo_on_us() - on_begin;
		uint64_t span = lpt_world() - w_begin;

		zassert_true(on * 100u < span * 3u, "HFXO duty %llu/%llu us",
			     (unsigned long long)on, (unsigned long long)span);
	}
}

/*
 * Risk: EDGE 2 fires but does not start the TIMER (a missed anchor). The
 * staged operation must fail exactly once through FAILED - never be sent on
 * an untrusted clock - and the backend must recover: a software start, then
 * (nothing armed) the next release, which takes the TIMER's reading because
 * the software-started segment has no exact anchor, then the next sleep and
 * wake on time.
 */
ZTEST(radiant_backend_lp, test_late_anchor_fails_once_then_recovers)
{
	struct lpt_probe *p = lpt_probe();
	struct lpt_stats st0, st;
	uint64_t t_sync;
	uint64_t start;

	t_sync = arm_far_and_sleep();
	lpt_stats(&st0);
	lpt_break_next_dppi();
	lpt_run_to(t_start_world(t_sync) - ARM_LEAD + 1u);

	zassert_equal(p->tx_events, 1);
	zassert_equal(p->failed_events, 1, "exactly one FAILED");
	zassert_true(lpt_op_idle());
	zassert_true(lpt_timer_running(), "software start");
	zassert_false(lpt_timer_at_anchor());
	lpt_stats(&st);
	zassert_equal(st.late_anchors, 1);
	zassert_equal(st.late_wakes, 1);
	zassert_equal(st.wakes, 1);
	zassert_equal(lpt_state(), LPT_RELEASING, "nothing armed: sleep again");
	lpt_run(400u);
	lpt_stats(&st);
	zassert_equal(st.timer_taken, st0.timer_taken + 1u,
		      "no exact anchor: the release took the TIMER");
	zassert_within(lpt_radio_now(), lpt_world() - LPT_W0, 1u + LPT_ASLEEP_SLACK_US);

	/* And the next one sleeps and wakes normally. */
	t_sync = lpt_radio_now() + FAR_US;
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	lpt_run_to(t_start_world(t_sync) - ARM_LEAD + 1u);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	zassert_equal(p->failed_events, 1, "no second FAILED");
	lpt_stats(&st);
	zassert_equal(st.late_anchors, 1);
}

/*
 * Risk: the FAILED callback arms the next operation from inside the wake's own
 * completion path (the core re-arms from every terminal). The nested arm must
 * be accepted and programmed, the wake path must not then undo it, and the
 * software-started TIMER is re-anchored while it runs.
 */
ZTEST(radiant_backend_lp, test_nested_arm_from_failed_callback)
{
	struct lpt_probe *p = lpt_probe();
	struct lpt_stats st0, st;
	uint64_t t_sync = arm_far_and_sleep();
	uint64_t start;

	lpt_stats(&st0);
	p->on_failed_arm_us = NEAR_US;
	lpt_break_next_dppi();
	lpt_run_to(t_start_world(t_sync) - ARM_LEAD + 1u);

	zassert_equal(p->failed_events, 1);
	zassert_equal(p->nested_arms, 1);
	zassert_equal(p->nested_rc, RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_state(), LPT_AWAKE);
	zassert_true(lpt_tx_programmed(&start), "the nested arm is programmed");
	lpt_run(200u);
	lpt_stats(&st);
	zassert_equal(st.reanchors, st0.reanchors + 1u, "re-anchored after the software start");
	zassert_true(lpt_timer_at_anchor());
	lpt_tx_end();
	zassert_equal(p->tx_events, 2);
	zassert_equal(p->last_tx_status, RADIANT_RADIO_STATUS_OK);
}

/*
 * Risk: the HFXO starts later than the budget, so EDGE 2 starts the TIMER on
 * HFINT. Nothing may be programmed until the crystal is up; the segment is
 * marked inexact and re-anchored; an operation the late crystal still makes is
 * sent on time.
 */
ZTEST(radiant_backend_lp, test_hfxo_late_at_edge2_reanchors)
{
	struct lpt_stats st0, st;
	uint64_t t_sync;
	uint64_t ts;
	uint64_t start;

	lpt_stats(&st0);
	lpt_set_hfxo_startup(HFXO_LEAD + 100u);   /* ready ~100 us after EDGE 2 */
	t_sync = arm_far_and_sleep();
	ts = t_start_world(t_sync);
	lpt_run_to(ts - ARM_LEAD + 1u);
	zassert_equal(lpt_state(), LPT_AWAKE);
	zassert_false(lpt_tx_programmed(&start), "nothing programmed on HFINT");
	zassert_false(lpt_timer_at_anchor());
	lpt_stats(&st);
	zassert_equal(st.hfxo_late_edge2, 1);

	lpt_run_to(ts - ARM_LEAD + 100u + 1u);
	zassert_true(lpt_tx_programmed(&start), "granted once the crystal is up");
	zassert_within(start, ts, 1);
	lpt_run(200u);
	lpt_stats(&st);
	zassert_equal(st.reanchors, st0.reanchors + 1u);
	zassert_true(lpt_timer_at_anchor());
	zassert_equal(st.late_wakes, 0);
}

/* Risk: a crystal so late the operation cannot be met: FAILED, once, not late air. */
ZTEST(radiant_backend_lp, test_hfxo_too_late_fails_once)
{
	struct lpt_probe *p = lpt_probe();
	struct lpt_stats st;
	uint64_t t_sync;

	lpt_set_hfxo_startup(HFXO_LEAD + 400u);   /* ready over 100 us after TXEN */
	t_sync = arm_far_and_sleep();
	lpt_run_to(t_start_world(t_sync) + 500u);
	zassert_equal(p->failed_events, 1);
	zassert_equal(p->tx_events, 1);
	lpt_stats(&st);
	zassert_equal(st.late_wakes, 1);
	zassert_true(st.max_wake_late_us > 0u);
}

/*
 * Risk: an abort of a staged operation while asleep must deliver its terminal
 * and cancel the wake (no HFXO start for nothing), and the TIMER-timed
 * DISABLE backstop must not run on a stopped TIMER.
 */
ZTEST(radiant_backend_lp, test_abort_while_staged_asleep)
{
	struct lpt_probe *p = lpt_probe();
	struct lpt_stats st;
	uint64_t t_sync = arm_far_and_sleep();
	uint32_t req0 = lpt_hfxo_requests();

	lpt_set_disable_hangs(true);
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	zassert_equal(p->tx_events, 1);
	zassert_equal(p->last_tx_status, RADIANT_RADIO_STATUS_ABORTED);
	zassert_false(lpt_staged());
	zassert_true(lpt_op_idle());
	lpt_run_to(t_start_world(t_sync) + 1000u);
	zassert_equal(lpt_state(), LPT_ASLEEP, "the wake was cancelled");
	zassert_equal(lpt_hfxo_requests(), req0);
	lpt_stats(&st);
	zassert_equal(st.wakes, 0);
}

/* Risk: an abort between the commit and the release edge. */
ZTEST(radiant_backend_lp, test_abort_while_releasing)
{
	struct lpt_probe *p = lpt_probe();
	uint64_t t_sync = lpt_radio_now() + FAR_US;
	uint32_t req0 = lpt_hfxo_requests();

	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_state(), LPT_RELEASING);
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	zassert_equal(p->last_tx_status, RADIANT_RADIO_STATUS_ABORTED);
	lpt_run(200u);
	zassert_equal(lpt_state(), LPT_ASLEEP, "the release still runs, with nothing wanted");
	lpt_run_to(t_start_world(t_sync) + 1000u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
	zassert_equal(lpt_hfxo_requests(), req0);
}

/* Risk: a nearer arm after a committed release is called off, not failed. */
ZTEST(radiant_backend_lp, test_release_cancelled_by_near_arm)
{
	struct lpt_stats st;
	uint64_t start;
	uint64_t t_sync;

	zassert_equal(lpt_tx_at(lpt_radio_now() + FAR_US), RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_state(), LPT_RELEASING);
	t_sync = lpt_radio_now() + NEAR_US;
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_state(), LPT_AWAKE);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	lpt_stats(&st);
	zassert_equal(st.release_cancels, 1);
	lpt_run(500u);
	zassert_equal(lpt_state(), LPT_AWAKE, "the cancelled edge does not fire");
}

/*
 * Risk: radiant_radio_disable() while asleep hangs in radio_disable_now(),
 * whose backstop is timed on the stopped TIMER. It must return at once, both
 * through disable and when called directly.
 */
ZTEST(radiant_backend_lp, test_disable_while_asleep_returns)
{
	struct lpt_probe *p = lpt_probe();

	(void)arm_far_and_sleep();
	lpt_set_disable_hangs(true);   /* a backstop on a stopped TIMER would spin forever */
	zassert_equal(lpt_disable(), RADIANT_RADIO_OK_RC);
	zassert_equal(p->last_tx_status, RADIANT_RADIO_STATUS_ABORTED);
	zassert_equal(lpt_disable_tasks(), 0u, "nothing to disable: never programmed");
	lpt_radio_disable_now();
	zassert_equal(lpt_disable_tasks(), 0u, "returns before touching the RADIO");
	zassert_equal(lpt_state(), LPT_ASLEEP);

	/* Enable from asleep wakes at once, so what follows is not late. */
	zassert_equal(lpt_enable(), RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_state(), LPT_WAKING);
	lpt_run(HFXO_LEAD + 200u);
	zassert_equal(lpt_state(), LPT_AWAKE, "lingers after enable");
}

/* Risk: an acquisition or the first seconds after enable must not sleep. */
ZTEST(radiant_backend_lp, test_linger_after_enable)
{
	struct lpt_probe *p = lpt_probe();
	uint64_t start;
	uint64_t t_sync;

	zassert_equal(lpt_enable(), RADIANT_RADIO_OK_RC);
	t_sync = lpt_radio_now() + FAR_US;
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_false(lpt_staged(), "lingering: granted at once");
	zassert_true(lpt_tx_programmed(&start));
	lpt_tx_end();
	zassert_equal(p->tx_events, 1);
	zassert_equal(lpt_state(), LPT_AWAKE, "still lingering");

	/* Finding 5: the linger's end is itself a decision point. */
	lpt_run(1000000u);
	zassert_equal(lpt_state(), LPT_ASLEEP, "linger over: slept with nothing armed");
	zassert_false(lpt_hfxo_on());
	zassert_equal(lpt_constlat_refs(), 0);
	zassert_equal(lpt_tx_at(lpt_radio_now() + FAR_US), RADIANT_RADIO_OK_RC);
	zassert_true(lpt_staged(), "and the next far arm is staged behind a wake");
}

ZTEST(radiant_backend_lp, test_linger_after_prio_normal_acquire)
{
	/* A search window (RADIANT_GATE_PRIO_NORMAL) keeps it awake for the linger. */
	zassert_equal(lpt_acquire(lpt_radio_now() + FAR_US, true), 0, "GATE_GRANTED");
	zassert_equal(lpt_tx_at(lpt_radio_now() + FAR_US), RADIANT_RADIO_OK_RC);
	zassert_false(lpt_staged());
	zassert_equal(lpt_state(), LPT_AWAKE);
}

/*
 * Risk: an arm made while asleep with less notice than the wake lead. It wakes
 * at once in software and, too late for the operation, fails it - counted -
 * rather than sending it late or hanging.
 */
ZTEST(radiant_backend_lp, test_near_arm_while_asleep_wakes_asap)
{
	struct lpt_probe *p = lpt_probe();
	struct lpt_stats st;
	uint64_t t_sync;

	(void)arm_far_and_sleep();
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	/* Starts before an ASAP wake's EDGE 2 (one HFXO lead from now). */
	t_sync = lpt_radio_now() + lpt_tx_lead() + HFXO_LEAD - 100u;
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_state(), LPT_WAKING, "HFXO requested at once");
	lpt_run(HFXO_LEAD + 300u);
	lpt_stats(&st);
	zassert_equal(st.asap_wakes, 1);
	zassert_equal(p->failed_events, 1);
	zassert_equal(st.late_wakes, 1);
	zassert_equal(lpt_state(), LPT_ASLEEP, "and back to sleep");
}

/* Risk: an LF clock that is not the crystal must leave the option off. */
ZTEST(radiant_backend_lp, test_refuses_without_lf_crystal)
{
	struct lpt_stats st;
	uint64_t start;

	lpt_reset_lf_rc();
	lpt_stats(&st);
	zassert_equal(st.refused, 1);
	zassert_equal(lpt_state(), LPT_OFF);
	zassert_equal(lpt_tx_at(lpt_radio_now() + FAR_US), RADIANT_RADIO_OK_RC);
	zassert_false(lpt_staged());
	zassert_true(lpt_tx_programmed(&start));
	zassert_true(lpt_hfxo_on());
	lpt_tx_end();
	zassert_true(lpt_hfxo_on(), "always on");
	zassert_equal(lpt_state(), LPT_OFF);
}

/* Risk: the receive path's staging (lp_staged() hook, staged filter copies). */
ZTEST(radiant_backend_lp, test_far_receive_window_on_time)
{
	uint64_t t_open = lpt_radio_now() + FAR_US;
	uint64_t start;

	zassert_equal(lpt_rx_at(t_open, 1000u), RADIANT_RADIO_OK_RC);
	zassert_true(lpt_staged());
	lpt_run(200u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
	lpt_run_to(LPT_W0 + t_open - lpt_rx_lead() - ARM_LEAD + 1u);
	zassert_true(lpt_rx_programmed(&start));
	zassert_within(start, LPT_W0 + t_open - lpt_rx_lead(), 1);
}

/*
 * Risk: the wake overtakes an arm still copying its request (a thread arm
 * preempted between gate_acquire() and staging). The grant is owed and paid
 * when the staging completes, not lost.
 */
ZTEST(radiant_backend_lp, test_wake_overtakes_staging)
{
	uint64_t t_sync;
	uint64_t start;

	(void)arm_far_and_sleep();
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	t_sync = lpt_radio_now() + FAR_US;
	zassert_equal(lpt_acquire(t_sync - lpt_tx_lead(), false), 1, "GATE_PENDING");
	lpt_run_to(t_start_world(t_sync) - ARM_LEAD + 1u);
	zassert_equal(lpt_state(), LPT_AWAKE);
	zassert_false(lpt_tx_programmed(&start));
	lpt_stage_tx_late(t_sync);
	zassert_true(lpt_tx_programmed(&start), "granted by lp_staged()");
	zassert_within(start, t_start_world(t_sync), 1);
}

/*
 * Risk: the release edge fires without its capture (the DPPI did nothing), so
 * the TIMER's relation to the edge is unknown. Nothing may be stopped: stay
 * awake, count it, and commit the release again.
 */
ZTEST(radiant_backend_lp, test_release_without_capture_stays_awake)
{
	struct lpt_stats st;
	uint64_t t_sync = lpt_radio_now() + FAR_US;
	uint64_t start;

	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	lpt_break_next_dppi();
	lpt_run_past_anchor();
	lpt_stats(&st);
	zassert_equal(st.late_anchors, 1);
	zassert_equal(st.sleeps, 0);
	zassert_true(lpt_timer_running(), "nothing stopped on a missing capture");
	zassert_equal(lpt_state(), LPT_RELEASING, "the release is committed again");
	lpt_run(200u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
	lpt_run_to(t_start_world(t_sync) - ARM_LEAD + 1u);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
}

/*
 * Risk: the release compare is written late (the arm was preempted between
 * choosing the edge and writing it), so the edge may already have passed.
 * That is a late anchor at a point where nothing has stopped: stay awake and
 * programme the operation now.
 */
ZTEST(radiant_backend_lp, test_late_release_write_stays_awake)
{
	struct lpt_stats st;
	uint64_t t_sync = lpt_radio_now() + FAR_US;
	uint64_t start;

	lpt_delay_next_anchor_set(200u);
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_false(lpt_staged(), "granted at once");
	zassert_equal(lpt_state(), LPT_AWAKE);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	lpt_stats(&st);
	zassert_equal(st.late_anchors, 1);
	lpt_run(500u);
	zassert_equal(lpt_state(), LPT_AWAKE, "the stale edge was withdrawn");
}

/*
 * Risk: an operation further out than the longest planned wake (600 s on the
 * GRTC, 128 s on RTC1 - whose driver refuses anything past 256 s, and whose
 * 24-bit counter wraps every 512 s): the backend wakes without the HFXO at the
 * clamp, rebases, plans again, and the operation is still on its microsecond.
 */
ZTEST(radiant_backend_lp, test_very_far_arm_rebases)
{
	struct lpt_stats st;
	uint64_t t_sync = lpt_radio_now() + 700000000ull;   /* 700 s */
	uint64_t start;
	uint32_t req0 = lpt_hfxo_requests();

	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	lpt_run(200u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
	lpt_run(650000000ull);
	zassert_equal(lpt_state(), LPT_ASLEEP, "the clamp wake needs no HFXO");
	zassert_equal(lpt_hfxo_requests(), req0);
	zassert_within(lpt_radio_now(), lpt_world() - LPT_W0, 1u + LPT_ASLEEP_SLACK_US);
	lpt_run_to(t_start_world(t_sync) - ARM_LEAD + 1u);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	lpt_stats(&st);
	zassert_equal(st.wakes, 1);
	zassert_equal(st.range_errors, 0);
}

/* ---- Review 2026-11-18 findings ---- */

/*
 * Finding 1: a release committed at an abort's terminal races the arm the
 * same scheduler pass makes next. Re-armed after the cancel window has gone
 * by, the near operation is programmed at once and the release edge, seeing
 * it, stops nothing - no sleep, no ASAP wake, no FAILED.
 */
ZTEST(radiant_backend_lp, test_abort_then_near_arm_past_cancel_window)
{
	struct lpt_probe *p = lpt_probe();
	struct lpt_stats st;
	uint64_t start;
	uint64_t t_sync;

	zassert_equal(lpt_tx_at(lpt_radio_now() + NEAR_US), RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_state(), LPT_RELEASING, "the abort's terminal committed a release");
	/* Past the cancel window (LP_POSTWRITE_LEAD before the edge: 85 us on
	 * the GRTC, two ticks on RTC1), before the edge. */
	lpt_run_to_anchor_minus(10u);
	t_sync = lpt_radio_now() + NEAR_US;
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_false(lpt_staged(), "programmed at once on the running TIMER");
	lpt_run(200u);
	zassert_equal(lpt_state(), LPT_AWAKE, "the edge saw the operation and stayed awake");
	zassert_true(lpt_timer_running());
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	lpt_stats(&st);
	zassert_equal(st.sleeps, 0);
	zassert_equal(st.release_cancels, 1);
	zassert_equal(st.asap_wakes, 0);
	zassert_equal(p->failed_events, 0);
	lpt_tx_end();
	zassert_equal(p->last_tx_status, RADIANT_RADIO_STATUS_OK);
}

/* Finding 1, the arm-committed release (108-tick lead): same outcome. */
ZTEST(radiant_backend_lp, test_far_then_near_arm_races_release_edge)
{
	struct lpt_probe *p = lpt_probe();
	uint64_t start;
	uint64_t t_sync;

	zassert_equal(lpt_tx_at(lpt_radio_now() + FAR_US), RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	lpt_run_to_anchor_minus(10u);    /* past the cancel window, before the edge */
	t_sync = lpt_radio_now() + NEAR_US;
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	lpt_run(200u);
	zassert_equal(lpt_state(), LPT_AWAKE);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	zassert_equal(p->failed_events, 0);
}

/*
 * Finding 2: the LF interrupt held off past both wake edges. Where the
 * anchor channel (EDGE 2) is the lower index (this fake's GRTC), the driver
 * services it first, while still ASLEEP: the handler must do EDGE 1's work
 * and take the edge, not drop it and leave the backend waking forever on
 * HFINT with the HFXO held. On RTC1 the HF channel is the lower and EDGE 1
 * runs first; the outcome must be the same either way.
 */
ZTEST(radiant_backend_lp, test_edge2_serviced_before_edge1)
{
	struct lpt_probe *p = lpt_probe();
	struct lpt_stats st;
	uint64_t t_sync = arm_far_and_sleep();
	uint64_t e2 = t_start_world(t_sync) - ARM_LEAD;

	lpt_hold_isr_until(e2 + 50u);
	lpt_run_to(e2 + 50u + 1000u);
	lpt_stats(&st);
	zassert_equal(st.edge_order, LPT_ANCHOR_SERVICED_FIRST, "EDGE 2 handled first");
	zassert_equal(st.hfxo_late_edge2, 1);
	zassert_equal(p->failed_events, 1, "the operation was missed, and said so");
	lpt_run(1000u);
	zassert_equal(lpt_state(), LPT_ASLEEP, "not wedged: back to sleep");
	zassert_false(lpt_hfxo_on());
	zassert_equal(lpt_constlat_refs(), 0);
	zassert_within(lpt_radio_now(), lpt_world() - LPT_W0, 1u + LPT_ASLEEP_SLACK_US,
		       "the TIMER started on the edge: the timebase is exact");
}

/*
 * Finding 3: a wake moved earlier inside gate_acquire() whose EDGE 2 write is
 * late, on a ready HFXO, recovers synchronously (software start). The arm
 * must then be granted - programmed now, its own ETIME check deciding -
 * rather than left PENDING with nothing to settle it.
 */
ZTEST(radiant_backend_lp, test_late_wake_rewrite_inside_arm_grants)
{
	struct lpt_probe *p = lpt_probe();
	struct lpt_stats st0, st;
	uint64_t t_sync = arm_far_and_sleep();
	uint64_t e1 = t_start_world(t_sync) - ARM_LEAD - HFXO_LEAD;
	uint64_t start;

	lpt_run_to(e1 + LPT_HFXO_STARTUP + 20u);   /* HFXO up, EDGE 2 not yet */
	zassert_equal(lpt_state(), LPT_WAKING);
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	lpt_stats(&st0);
	lpt_delay_next_anchor_set(200u);
	/* Leaves the arming lead after the 200 us preemption and RTC1's wait for
	 * its step before the software start. */
	t_sync = lpt_radio_now() + lpt_tx_lead() + 300u + LPT_TICK_US;
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_false(lpt_staged(), "granted, not stranded PENDING");
	zassert_equal(lpt_state(), LPT_AWAKE);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	lpt_stats(&st);
	zassert_equal(st.late_anchors, st0.late_anchors + 1u);
	zassert_equal(p->failed_events, 0);
	lpt_tx_end();
	zassert_equal(p->last_tx_status, RADIANT_RADIO_STATUS_OK);
}

/*
 * Finding 4: the HFXO start fails (the callback reports an error). The
 * staged operation fails once and the option drops to always-on: TIMER
 * running, crystal requested through the fallback, CONSTLAT held, and later
 * arms programmed at once.
 */
ZTEST(radiant_backend_lp, test_hfxo_start_error_drops_to_always_on)
{
	struct lpt_probe *p = lpt_probe();
	struct lpt_stats st;
	uint64_t t_sync = arm_far_and_sleep();
	uint64_t start;

	lpt_hfxo_errors(false, true);
	lpt_run_to(t_start_world(t_sync) - ARM_LEAD - HFXO_LEAD + LPT_HFXO_STARTUP + 20u);
	lpt_stats(&st);
	zassert_equal(st.hfxo_errors, 1);
	zassert_equal(st.refused, 2);
	zassert_equal(lpt_state(), LPT_OFF);
	zassert_equal(p->failed_events, 1);
	zassert_true(lpt_timer_running());
	zassert_true(lpt_hf_fallback());
	zassert_equal(lpt_constlat_refs(), LPT_CONSTLAT_ON);
	zassert_within(lpt_radio_now(), lpt_world() - LPT_W0, 1);
	lpt_run(FAR_US);
	t_sync = lpt_radio_now() + FAR_US;
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
}

/* Finding 4: the request itself fails, inside an arm call: always-on, and
 * the arm is granted rather than failed from inside it. */
ZTEST(radiant_backend_lp, test_hfxo_request_error_inside_arm)
{
	struct lpt_probe *p = lpt_probe();
	struct lpt_stats st;
	uint64_t t_sync;
	uint64_t start;

	(void)arm_far_and_sleep();
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	lpt_hfxo_errors(true, false);
	t_sync = lpt_radio_now() + lpt_tx_lead() + HFXO_LEAD - 100u;   /* an ASAP wake */
	zassert_equal(lpt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_false(lpt_staged());
	zassert_equal(lpt_state(), LPT_OFF);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	lpt_stats(&st);
	zassert_equal(st.refused, 2);
	zassert_equal(p->failed_events, 0, "the aborted one ended ABORTED, this one runs");
}

/* A request that completes synchronously (another client holds the HFXO)
 * leaves its work to the caller, and the wake runs normally. */
ZTEST(radiant_backend_lp, test_hfxo_shared_sync_callback)
{
	struct lpt_stats st;
	uint64_t t_sync = arm_far_and_sleep();
	uint64_t start;

	lpt_hfxo_shared(true);
	lpt_run_to(t_start_world(t_sync) - ARM_LEAD + 1u);
	zassert_true(lpt_tx_programmed(&start));
	zassert_within(start, t_start_world(t_sync), 1);
	lpt_stats(&st);
	zassert_equal(st.hfxo_late_edge2, 0);
	zassert_equal(st.late_wakes, 0);
}

/* Finding 5: enable while a release is committed lingers instead of
 * sleeping, and the linger's end sleeps. */
ZTEST(radiant_backend_lp, test_enable_while_releasing_lingers)
{
	struct lpt_stats st;

	zassert_equal(lpt_tx_at(lpt_radio_now() + FAR_US), RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	zassert_equal(lpt_state(), LPT_RELEASING);
	zassert_equal(lpt_enable(), RADIANT_RADIO_OK_RC);
	lpt_run(500u);
	zassert_equal(lpt_state(), LPT_AWAKE, "lingering: the edge stopped nothing");
	lpt_stats(&st);
	zassert_equal(st.sleeps, 0);
	lpt_run(1000000u);
	zassert_equal(lpt_state(), LPT_ASLEEP, "and slept at the linger's end");
}

/*
 * Finding 6: an arm whose staging a wake overtook, granted from lp_staged()
 * inside the arm call, fails ETIME there. The FAILED must not be delivered
 * inside the arm call (the scheduler has not recorded the operation yet); it
 * comes from the work queue, and not at all if the operation ended first.
 */
ZTEST(radiant_backend_lp, test_staging_failure_is_deferred)
{
	struct lpt_probe *p = lpt_probe();
	uint64_t t_sync;
	uint32_t ev0;

	(void)arm_far_and_sleep();
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	ev0 = p->tx_events;
	t_sync = lpt_radio_now() + FAR_US;
	zassert_equal(lpt_acquire(t_sync - lpt_tx_lead(), false), 1, "GATE_PENDING");
	lpt_run_to(t_start_world(t_sync) + 100u);   /* woke; the op's time has passed */
	lpt_stage_tx_late(t_sync);
	zassert_equal(p->tx_events, ev0, "nothing delivered inside the arm call");
	lpt_run_work();
	zassert_equal(p->tx_events, ev0 + 1u);
	zassert_equal(p->last_tx_status, RADIANT_RADIO_STATUS_FAILED);
}

ZTEST(radiant_backend_lp, test_deferred_failure_dropped_after_abort)
{
	struct lpt_probe *p = lpt_probe();
	uint64_t t_sync;

	(void)arm_far_and_sleep();
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	t_sync = lpt_radio_now() + FAR_US;
	zassert_equal(lpt_acquire(t_sync - lpt_tx_lead(), false), 1);
	lpt_run_to(t_start_world(t_sync) + 100u);
	lpt_stage_tx_late(t_sync);
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	zassert_equal(p->last_tx_status, RADIANT_RADIO_STATUS_ABORTED);
	lpt_run_work();
	zassert_equal(p->last_tx_status, RADIANT_RADIO_STATUS_ABORTED, "no FAILED after it");
	zassert_equal(p->failed_events, 0);
}

/* Nit: a boot HFXO request that times out is taken back off the manager's
 * queue, so a retried init does not queue the same client twice. */
ZTEST(radiant_backend_lp, test_boot_hfxo_timeout_dequeues)
{
	bool queued = true;

	zassert_equal(lpt_boot_hfxo_timeout(&queued), -ETIMEDOUT);
	zassert_false(queued);
}

/* Finding 2, the reverse: an abort while both wake edges are latched behind
 * a held interrupt. EDGE 2's (D)PPI has already started the TIMER; cancelling
 * the wake must stop it again, and the late handlers - where the driver
 * still delivers them after the abort (the GRTC's) - change nothing. */
ZTEST(radiant_backend_lp, test_abort_with_wake_edges_latched)
{
	struct lpt_stats st;
	uint64_t t_sync = arm_far_and_sleep();
	uint64_t e2 = t_start_world(t_sync) - ARM_LEAD;

	lpt_hold_isr_until(e2 + 50u);
	lpt_run_to(e2 + 10u);
	zassert_true(lpt_timer_running(), "the DPPI started it on the tick");
	zassert_equal(lpt_abort(), RADIANT_RADIO_OK_RC);
	zassert_false(lpt_timer_running(), "stopped again with the cancelled wake");
	lpt_run(2000u);
	zassert_equal(lpt_state(), LPT_ASLEEP);
	zassert_false(lpt_timer_running());
	zassert_false(lpt_hfxo_on());
	lpt_stats(&st);
	zassert_equal(st.spurious, LPT_ABORT_KEEPS_LATCHED ? 2u : 0u,
		      "both latched handlers ran and were ignored");
	zassert_equal(st.wakes, 0);
}

/* Nits N2/N4: a refused always-on request is counted (and logged), and the
 * drop to always-on happens once however many errors follow. */
ZTEST(radiant_backend_lp, test_hfxo_fallback_refused_counted_once)
{
	struct lpt_stats st;
	uint64_t t_sync = arm_far_and_sleep();

	lpt_set_fallback_rc(-EIO);
	lpt_hfxo_errors(false, true);
	lpt_run_to(t_start_world(t_sync) - ARM_LEAD - HFXO_LEAD + LPT_HFXO_STARTUP + 20u);
	lpt_stats(&st);
	zassert_equal(lpt_state(), LPT_OFF);
	zassert_equal(st.refused, 2);
	zassert_equal(st.hfxo_errors, 2, "the start error and the refused fallback");
	lpt_run(FAR_US);
	lpt_stats(&st);
	zassert_equal(st.hfxo_errors, 2, "nothing degrades twice");
}
