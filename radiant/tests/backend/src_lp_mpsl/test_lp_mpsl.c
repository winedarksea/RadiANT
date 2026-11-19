/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The nRF backend with CONFIG_RADIANT_NRF_LOW_POWER on the MPSL gate's build
 * (power plan pass B6, the LP_MPSL section of src/radiant_radio_nrf_lp.c),
 * against a simulated GRTC, HFXO and TIMER (backend_lp_mpsl_under_test.c).
 * The gate's side - that every START and every timeslot end reaches the hooks,
 * in the right order - is radiant/tests/gate's gate.lp scenario. Each case
 * names the risk it covers.
 *
 * With hf_ppm 0 the TIMER and the GRTC agree, so radio time stays exactly
 * world - LMT_W0 through every transition, and any error a transition makes
 * shows as a difference. Cases that set hf_ppm use spike B1's measured offsets
 * (-30 ppm nRF54L15 DK, +-58 ppm the worst crystal pair seen) or HFINT's
 * order of 1 %.
 */

#include <zephyr/ztest.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_lp_mpsl_probe.h"

#define PERIOD_US 246338u   /* 8070/32768 s, the HR period */
#define SLOT_US   3000u

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	lmt_reset();
}

ZTEST_SUITE(radiant_backend_lp_mpsl, NULL, NULL, before, NULL, NULL);

static int64_t radio_err(void)
{
	return (int64_t)lmt_radio_now() - (int64_t)(lmt_world() - LMT_W0);
}

/*
 * Risk: the option leaves the boot HFXO request held (the 100 % duty Gate GA
 * measured) or the TIMER free-running between timeslots on whatever clock MPSL
 * leaves it - the two things the pass exists to stop - or loses the epoch.
 */
ZTEST(radiant_backend_lp_mpsl, test_init_stops_timer_and_drops_hfxo)
{
	struct lmt_stats st;

	lmt_stats(&st);
	zassert_equal(st.size, 0x74u, "the MPSL build appends its six fields");
	zassert_equal(st.refused, 0u);
	zassert_equal(lmt_state(), LMT_ASLEEP);
	zassert_false(lmt_timer_running(), "TIMER stopped until the first START");
	zassert_false(lmt_hfxo_on(), "the boot HFXO request is dropped");
	zassert_equal(lmt_hfxo_requests(), 1u);
	zassert_equal(lmt_hfxo_releases(), 1u);
	zassert_equal(st.sleeps, 0u, "the epoch release is not a timeslot");
	zassert_equal(st.wakes, 0u);
	zassert_equal(radio_err(), 0, "the epoch continues the boot TIMER");
	lmt_run(123457u);
	zassert_equal(radio_err(), 0, "between timeslots time is the GRTC's");
}

/*
 * Risk: the START re-anchor starts the TIMER from a stale count or loads the
 * fold from anything but the tick it started on, so every compare the grant
 * then programmes is off by the difference.
 */
ZTEST(radiant_backend_lp_mpsl, test_slot_start_restarts_timer_on_the_tick)
{
	struct lmt_stats st;
	uint32_t starts = lmt_timer_starts();

	lmt_run(10000u);
	lmt_slot_start(0u);
	lmt_stats(&st);
	zassert_true(lmt_timer_running());
	zassert_equal(lmt_timer_starts(), starts + 1u);
	zassert_equal(lmt_timer_count(), 0u, "started from zero");
	zassert_true(lmt_timer_at_anchor(), "a START is an exact anchor");
	zassert_equal(lmt_state(), LMT_AWAKE);
	zassert_equal(st.wakes, 1u);
	zassert_equal(radio_err(), 0, "no step at the START");
	zassert_false(lmt_hfxo_on(), "the crystal is MPSL's: no request of ours");
	lmt_run(2500u);
	zassert_equal(radio_err(), 0, "the awake segment counts from the tick");
}

/*
 * Risk: the release takes the TIMER's reading every time, so each timeslot's
 * quantisation and HF-vs-LF offset is kept and the timebase random-walks
 * away from MPSL's grid; or the TIMER is left running between timeslots.
 */
ZTEST(radiant_backend_lp_mpsl, test_slot_end_takes_lf_image_and_stops_timer)
{
	struct lmt_stats st;

	lmt_set_hf_ppm(200);   /* +1 us over a 5 ms timeslot */
	lmt_run(10000u);
	lmt_slot_start(0u);
	lmt_run(5000u);
	zassert_equal(radio_err(), 1, "awake, the TIMER's rate");
	lmt_slot_end(false);
	lmt_stats(&st);
	zassert_false(lmt_timer_running(), "TIMER stopped at the timeslot's end");
	zassert_equal(lmt_state(), LMT_ASLEEP);
	zassert_equal(st.sleeps, 1u);
	zassert_equal(st.lf_taken, 1u, "within tolerance: the LF image");
	zassert_equal(st.timer_taken, 0u);
	zassert_equal(st.max_disagree_us, 1u);
	zassert_equal(st.missed_releases, 0u);
	zassert_equal(st.slot_awake_us, 5000u);
	zassert_equal(radio_err(), 1, "held by the clamp, never backwards...");
	lmt_run(1u);
	zassert_equal(radio_err(), 0, "...then exactly the GRTC again");
	lmt_run(PERIOD_US);
	zassert_equal(radio_err(), 0);
}

/*
 * Risk: a long timeslot (an elastic scan grows to ~100 ms and more) whose HF
 * time has genuinely run apart from the LF clock is snapped to the LF image,
 * stepping the timebase by more than the tolerance; or time runs backwards.
 */
ZTEST(radiant_backend_lp_mpsl, test_long_slot_keeps_hf_time_monotonic)
{
	struct lmt_stats st;
	uint64_t at_end;
	uint64_t prev;

	lmt_set_hf_ppm(-30);
	lmt_run(10000u);
	lmt_slot_start(0u);
	lmt_run(200000u);
	prev = lmt_radio_now();
	zassert_equal(radio_err(), -6, "-30 ppm over 200 ms");
	lmt_slot_end(false);
	lmt_stats(&st);
	zassert_equal(st.timer_taken, 1u, "beyond tolerance: the TIMER's reading");
	zassert_equal(st.disagreements, 1u);
	zassert_equal(st.max_disagree_us, 6u);
	at_end = lmt_radio_now();
	zassert_true(at_end >= prev, "monotonic");
	zassert_true(at_end - prev <= 1u, "no step at the release");
	lmt_run(50000u);
	zassert_equal(lmt_radio_now() - at_end, 50000u, "asleep, the LF rate");
}

/*
 * Risk: error carried from one timeslot into the next - the failure the
 * per-START re-anchor exists to rule out. Four thousand HR periods (16 min)
 * with the crystals 30 and 58 ppm apart: radio time must still be world time.
 */
ZTEST(radiant_backend_lp_mpsl, test_many_slots_no_accumulation)
{
	static const int32_t ppm[] = { -30, 58, -58 };

	for (size_t k = 0; k < ARRAY_SIZE(ppm); k++) {
		struct lmt_stats st;
		uint64_t prev = 0u;

		lmt_reset();
		lmt_set_hf_ppm(ppm[k]);
		for (uint32_t i = 0u; i < 4000u; i++) {
			lmt_run(PERIOD_US - SLOT_US);
			lmt_slot_start(0u);
			zassert_true(lmt_radio_now() >= prev, "monotonic at a START");
			lmt_run(SLOT_US);
			prev = lmt_radio_now();
			lmt_slot_end(false);
			zassert_true(lmt_radio_now() >= prev, "monotonic at a release");
		}
		lmt_stats(&st);
		zassert_equal(st.sleeps, 4000u);
		zassert_equal(st.wakes, 4000u);
		zassert_equal(st.lf_taken, 4000u, "every short timeslot takes the LF image");
		zassert_equal(st.missed_releases, 0u);
		zassert_equal(st.slot_awake_us, 4000u * SLOT_US);
		zassert_true(radio_err() >= -1 && radio_err() <= 1,
			     "%d ppm: radio time %lld us off after 4000 periods",
			     ppm[k], (long long)radio_err());
	}
}

/*
 * Risk: the TIMER restarted at a START maps radio time to compare values
 * wrongly, so the first operation of every timeslot goes out at the wrong
 * instant - exactly what the on-air gate would see as a period shift or loss.
 */
ZTEST(radiant_backend_lp_mpsl, test_tx_in_slot_lands_where_asked)
{
	uint64_t start;
	uint64_t t_sync;

	lmt_set_hf_ppm(-30);
	for (uint32_t i = 0u; i < 50u; i++) {
		lmt_run(PERIOD_US - SLOT_US);
		lmt_slot_start(0u);
		lmt_run(SLOT_US);
		lmt_slot_end(false);
	}
	lmt_run(PERIOD_US - SLOT_US);
	lmt_slot_start(0u);
	lmt_run(100u);
	t_sync = lmt_radio_now() + 1500u;
	zassert_equal(lmt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
	zassert_true(lmt_tx_programmed(&start), "programmed in the timeslot");
	/* Within the 1 us the -30 ppm HFXO itself drifts over the 1.6 ms from
	 * the START: none of it is carried in from the 50 timeslots before. */
	zassert_true(start >= LMT_W0 + t_sync - lmt_tx_lead() &&
		     start <= LMT_W0 + t_sync - lmt_tx_lead() + 1u,
		     "TXEN at world %llu, wanted %llu", (unsigned long long)start,
		     (unsigned long long)(LMT_W0 + t_sync - lmt_tx_lead()));
	lmt_retire_op();
	lmt_slot_end(false);
}

/*
 * Risk: the START cross-check (Gate G4's "anchor error within 0..17 us")
 * records the wrong sign, the wrong reference, or counts untimed timeslots.
 */
ZTEST(radiant_backend_lp_mpsl, test_timed_start_cross_check)
{
	struct lmt_stats st;
	uint64_t now;

	lmt_run(20000u);
	now = lmt_radio_now();
	lmt_slot_start(now - 7u);   /* MPSL started it 7 us late */
	lmt_run(1000u);
	lmt_slot_end(false);
	lmt_run(20000u);
	now = lmt_radio_now();
	lmt_slot_start(now + 3u);   /* 3 us early */
	lmt_slot_end(false);
	lmt_run(20000u);
	lmt_slot_start(0u);         /* the bootstrap or the elastic class */
	lmt_slot_end(false);
	lmt_stats(&st);
	zassert_equal(st.start_err_n, 2u, "only timed STARTs are cross-checked");
	zassert_equal(st.start_err_min_us, -3);
	zassert_equal(st.start_err_max_us, 7);
	zassert_equal(st.start_err_last_us, -3);
	zassert_equal(st.wakes, 3u);
}

/*
 * Risk: a timeslot that ended without passing the ACTION_END funnel leaves the
 * TIMER running after MPSL may have dropped the crystal; the next START takes
 * that TIMER's (HFINT) reading into the timebase for good, or runs time
 * backwards to undo it.
 */
ZTEST(radiant_backend_lp_mpsl, test_missed_release_takes_lf_image)
{
	struct lmt_stats st;
	uint64_t ahead;

	lmt_run(10000u);
	lmt_set_hf_ppm(10000);   /* HFINT's order of 1 % */
	lmt_slot_start(0u);
	lmt_run(10000u);
	ahead = lmt_radio_now();
	zassert_equal(radio_err(), 100);
	lmt_slot_start(0u);      /* no end in between */
	/* The new segment counts from zero here, so the rate can change now
	 * without rewriting what the TIMER already counted. */
	lmt_set_hf_ppm(0);
	lmt_stats(&st);
	zassert_equal(st.missed_releases, 1u);
	zassert_equal(st.last_disagree_q, -100, "it saw the HFINT reading...");
	zassert_equal(st.lf_taken, 1u, "...and took the LF image all the same");
	zassert_equal(st.sleeps, 1u);
	zassert_equal(st.wakes, 2u);
	zassert_true(lmt_radio_now() >= ahead, "monotonic: held, not stepped back");
	lmt_run(101u);
	zassert_equal(radio_err(), 0, "back on the GRTC once the clamp is passed");
}

/*
 * Risk: the low-priority backstops (an expiry-ended timeslot, a closing
 * session, gate_shutdown()) double-count, or their idempotent call releases
 * twice and stops a TIMER some later START started.
 */
ZTEST(radiant_backend_lp_mpsl, test_backstop_end_counts_once)
{
	struct lmt_stats st;

	lmt_run(10000u);
	lmt_slot_start(0u);
	lmt_run(1000u);
	lmt_slot_end(true);
	lmt_slot_end(true);
	lmt_slot_end(false);
	lmt_stats(&st);
	zassert_equal(st.missed_releases, 1u);
	zassert_equal(st.sleeps, 1u);
	zassert_false(lmt_timer_running());
	zassert_equal(radio_err(), 0);
}

/* Risk: an end with nothing awake (every backstop after a normal END) moves
 * the timebase or touches the TIMER. */
ZTEST(radiant_backend_lp_mpsl, test_end_while_asleep_is_noop)
{
	struct lmt_stats st;
	uint32_t starts = lmt_timer_starts();

	lmt_run(5000u);
	lmt_slot_end(false);
	lmt_slot_end(true);
	lmt_stats(&st);
	zassert_equal(st.sleeps, 0u);
	zassert_equal(st.missed_releases, 0u);
	zassert_equal(lmt_timer_starts(), starts);
	zassert_equal(radio_err(), 0);
}

/*
 * Risk: radio_disable_now() spins on a stopped TIMER (its backstop is that
 * TIMER), or disables a RADIO that between timeslots is the other stack's.
 * Inside a timeslot it must still disable.
 */
ZTEST(radiant_backend_lp_mpsl, test_radio_disable_now_only_when_awake)
{
	lmt_radio_disable_now();
	zassert_equal(lmt_disable_tasks(), 0u, "asleep: the RADIO is not ours");
	lmt_slot_start(0u);
	lmt_radio_disable_now();
	zassert_equal(lmt_disable_tasks(), 1u, "awake: disabled as before");
	lmt_slot_end(false);
}

/*
 * Risk: a compare left armed from one timeslot fires early in the next, after
 * the TIMER restarts from zero, inside that timeslot's operation. The
 * TIMER-driven window connections go at every release; the ADDRESS capture
 * and RXREADY->RSSISTART (not TIMER-driven) stay.
 */
ZTEST(radiant_backend_lp_mpsl, test_release_disables_timer_connections)
{
	lmt_slot_start(0u);
	lmt_conn_enable_all();
	lmt_run(1000u);
	lmt_slot_end(false);
	zassert_equal(lmt_conn_on() & lmt_conn_bit_timer(), 0u,
		      "TIMER-driven connections disabled with the TIMER");
	zassert_equal(lmt_conn_on() & lmt_conn_bits_other(), lmt_conn_bits_other(),
		      "the others are not the release's to touch");
}

/*
 * Risk: with the option refused (no LF crystal) the hooks still stop the TIMER
 * - which then has no LF timebase behind it - or the boot HFXO request is
 * dropped. Refused must be exactly the build without the option.
 */
ZTEST(radiant_backend_lp_mpsl, test_refused_without_lf_crystal)
{
	struct lmt_stats st;
	uint32_t starts;

	lmt_reset_lf_rc();
	lmt_stats(&st);
	zassert_equal(st.refused, 1u);
	zassert_equal(lmt_state(), LMT_OFF);
	zassert_true(lmt_hfxo_on(), "the boot request stays");
	zassert_true(lmt_timer_running(), "the TIMER free-runs as without the option");
	starts = lmt_timer_starts();
	lmt_run(10000u);
	lmt_slot_start(12345u);
	lmt_run(1000u);
	lmt_slot_end(false);
	lmt_slot_end(true);
	lmt_stats(&st);
	zassert_true(lmt_timer_running());
	zassert_equal(lmt_timer_starts(), starts);
	zassert_equal(st.wakes, 0u);
	zassert_equal(st.sleeps, 0u);
	zassert_equal(st.start_err_n, 0u);
	zassert_equal(radio_err(), 0);
}

/* Risk: the duty proxy (Gate G4's) counts the gaps, or drops a timeslot. */
ZTEST(radiant_backend_lp_mpsl, test_awake_us_accumulates)
{
	struct lmt_stats st;

	lmt_run(10000u);
	lmt_slot_start(0u);
	lmt_run(3000u);
	lmt_slot_end(false);
	lmt_run(200000u);
	lmt_slot_start(0u);
	lmt_run(1500u);
	lmt_slot_end(false);
	lmt_run(200000u);
	lmt_stats(&st);
	zassert_equal(st.slot_awake_us, 4500u);
}
