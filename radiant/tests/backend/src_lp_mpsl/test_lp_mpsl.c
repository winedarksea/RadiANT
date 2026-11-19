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
 * TIMER running after MPSL may have dropped the crystal. The next START must
 * count it, never step time by more than the tolerance, and start the new
 * segment on the clock radiant_radio_now() already reports: if it froze at the
 * high-water mark while the TIMER's base sat below it, the gate's s->anchor
 * and every compare of the timeslot would disagree by the HFINT error.
 */
ZTEST(radiant_backend_lp_mpsl, test_missed_release_keeps_one_clock)
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
	zassert_equal(st.last_disagree_q, -100, "it saw the HFINT reading");
	zassert_equal(st.timer_taken, 1u, "beyond tolerance: no 100 us step back");
	zassert_equal(st.sleeps, 1u);
	zassert_equal(st.wakes, 2u);
	zassert_equal(lmt_radio_now(), ahead, "no step at all");
	lmt_run(10u);
	zassert_equal(lmt_radio_now(), ahead + 10u,
		      "the segment runs from that value at once, not frozen");
}

/*
 * Risk: the ordinary case of the same fault - a release that took the LF image
 * a microsecond or two behind the TIMER, and a START inside those
 * microseconds. The new segment must run from the high-water mark, not from
 * the LF image below it with every reading clamped.
 */
ZTEST(radiant_backend_lp_mpsl, test_wake_below_high_water_mark_starts_there)
{
	struct lmt_stats st;
	uint64_t hwm;

	lmt_run(10000u);
	lmt_set_hf_ppm(400);     /* +2 us over 5 ms */
	lmt_slot_start(0u);
	lmt_run(5000u);
	hwm = lmt_radio_now();
	lmt_slot_end(false);
	lmt_stats(&st);
	zassert_equal(st.lf_taken, 1u);
	lmt_slot_start(0u);      /* at once */
	lmt_set_hf_ppm(0);
	zassert_equal(lmt_radio_now(), hwm);
	lmt_run(10u);
	zassert_equal(lmt_radio_now(), hwm + 10u, "not frozen for 2 us");
	lmt_slot_end(false);
	lmt_run(1000u);
	zassert_equal(radio_err(), 2, "the 2 us is kept, not stepped back");
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

/* Risk: LFCLK on the crystal while the GRTC counts the LFLPRC passes the
 * LFCLK check and times the gaps between timeslots on an RC. */
ZTEST(radiant_backend_lp_mpsl, test_refused_grtc_on_lflprc)
{
	struct lmt_stats st;

	lmt_reset_grtc_lflprc();
	lmt_stats(&st);
	zassert_equal(st.refused, 1u);
	zassert_equal(lmt_state(), LMT_OFF);
	zassert_true(lmt_hfxo_on(), "the boot request stays");
	zassert_true(lmt_timer_running(), "the TIMER free-runs as without the option");
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

/*
 * Risk (test fidelity): every case above starts the TIMER and reads the GRTC
 * on the same whole microsecond, the best case. Here the START lands on a
 * drifting sub-microsecond phase, each GRTC read costs 40..900 ns before the
 * TIMER starts or is captured, and the TIMER's prescaler phase varies. The
 * anchor must still hold to the tick: every short timeslot's release agrees
 * with the GRTC to 2 us, one tick at each end (inside the 3 us tolerance), time does not
 * accumulate error over 2000 periods at -30 ppm, the START cross-check reads
 * the dispatch lateness to the tick, and a transmit lands where asked.
 */
ZTEST(radiant_backend_lp_mpsl, test_sub_tick_phases_and_read_latency)
{
	static const uint32_t lat[] = { 40u, 300u, 900u };
	static const uint32_t phase[] = { 0u, 500u, 999u };

	for (size_t a = 0; a < ARRAY_SIZE(lat); a++) {
		for (size_t b = 0; b < ARRAY_SIZE(phase); b++) {
			struct lmt_stats st;
			uint64_t start;
			uint64_t t_sync;
			uint64_t want;

			lmt_reset();
			lmt_set_hf_ppm(-30);
			lmt_set_read_latency_ns(lat[a]);
			lmt_set_timer_phase_ns(phase[b]);
			for (uint32_t i = 0u; i < 2000u; i++) {
				uint64_t r;

				lmt_run_ns((uint64_t)(PERIOD_US - SLOT_US) * 1000u + 137u);
				/* MPSL dispatched this START 5 us after the
				 * start the gate asked for. */
				r = lmt_world() - LMT_W0 - 5u;
				lmt_slot_start(r);
				lmt_run(SLOT_US);
				lmt_slot_end(false);
			}
			lmt_stats(&st);
			zassert_equal(st.lf_taken, 2000u, "lat %u phase %u", lat[a], phase[b]);
			/* Two 1 us quantisations (the wake's tick and the
			 * release's) plus 0.09 us of drift: at most 2, inside
			 * the 3 us tolerance with room to spare. */
			zassert_true(st.max_disagree_us <= 2u, "lat %u phase %u: %u us",
				     lat[a], phase[b], st.max_disagree_us);
			zassert_true(radio_err() >= -2 && radio_err() <= 2,
				     "lat %u phase %u: %lld us off", lat[a], phase[b],
				     (long long)radio_err());
			/* 5, give or take the tick, the radio-vs-world error and
			 * a high-water-mark start. */
			zassert_true(st.start_err_min_us >= 3 && st.start_err_max_us <= 7,
				     "lat %u phase %u: START error %d..%d, dispatched 5 late",
				     lat[a], phase[b], st.start_err_min_us, st.start_err_max_us);

			lmt_run_ns((uint64_t)(PERIOD_US - SLOT_US) * 1000u + 371u);
			lmt_slot_start(0u);
			lmt_run(100u);
			t_sync = lmt_radio_now() + 1500u;
			zassert_equal(lmt_tx_at(t_sync), RADIANT_RADIO_OK_RC);
			zassert_true(lmt_tx_programmed(&start));
			/* Radio time is world - W0 + radio_err(). */
			want = (uint64_t)((int64_t)(LMT_W0 + t_sync - lmt_tx_lead()) - radio_err());
			zassert_true(start + 2u >= want && start <= want + 2u,
				     "lat %u phase %u: TXEN at %llu, wanted %llu", lat[a],
				     phase[b], (unsigned long long)start,
				     (unsigned long long)want);
			lmt_retire_op();
			lmt_slot_end(false);
		}
	}
}

/*
 * Risk: an in-grant arm (a completion's follow-on, from the software
 * interrupt) preempted by the zero-latency ACTION_END, which hands the RADIO
 * back to the other stack and stops the TIMER. Resumed, the arm wrote MODE,
 * FREQUENCY, PACKETPTR and SHORTS into the other stack's live RADIO, folded a
 * stopped TIMER, and re-enabled a window connection that then fired in the
 * next timeslot; and when the end also aborted (EXTEND_FAILED, the extension
 * skip) the operation got ETIME and ABORTED both. Swept over every point of a
 * transmit, a receive and an energy-detect arm (the signal lands at the end
 * of whichever critical section holds it off, as PRIMASK makes it), with the
 * end alone, the end and the next START, and an aborting end. Always: no
 * RADIO write after the hand-back, no window connection left on, exactly one
 * outcome for the operation. The late variant arms too near to make it (the
 * span's own late check fails), so the aborting end can land after that
 * failure, while the arm is still unwinding: still ETIME alone.
 */
enum { ARM_TX, ARM_RX, ARM_ED };

static int arm_kind(int kind, uint64_t t)
{
	switch (kind) {
	case ARM_TX:
		return lmt_tx_at(t);
	case ARM_RX:
		return lmt_rx_at(t, 1000u);
	default:
		return lmt_ed_at(t);
	}
}

ZTEST(radiant_backend_lp_mpsl, test_arm_preempted_by_slot_end)
{
	static const char *const kn[] = { "tx", "rx", "ed" };
	static const char *const mn[] = { "end", "end+start", "abort+end" };

	for (int late = 0; late <= 1; late++)
	for (int kind = ARM_TX; kind <= ARM_ED; kind++) {
		for (int mode = 0; mode <= 2; mode++) {
			uint32_t etimes = 0u;

			for (uint32_t n = 1u; n <= 12u; n++) {
				uint64_t t;
				uint32_t term;
				int rc;

				lmt_reset();
				lmt_run(10000u);
				lmt_slot_start(0u);
				lmt_run(100u);
				t = lmt_radio_now() + (late ? 10u : 1500u);
				lmt_preempt_mode(n, mode == 1, mode == 2);
				rc = arm_kind(kind, t);
				lmt_preempt_after_capture(0u, false);
				if (!lmt_preempt_fired()) {
					/* The arm made fewer captures: it finished first. */
					zassert_equal(rc, late ? RADIANT_RADIO_ETIME : RADIANT_RADIO_OK_RC,
						      "%s late %d: rc %d", kn[kind], late, rc);
					break;
				}
				term = lmt_terminals();
				zassert_false(lmt_radio_written_after_handback(),
					      "%s %s n %u: RADIO written after the hand-back",
					      kn[kind], mn[mode], n);
				zassert_false(lmt_window_conns_on(),
					      "%s %s n %u: a window connection outlived the release",
					      kn[kind], mn[mode], n);
				zassert_true(!late || rc == RADIANT_RADIO_ETIME,
					     "%s %s n %u: a late arm not refused", kn[kind], mn[mode], n);
				if (rc == RADIANT_RADIO_OK_RC) {
					/* The end landed after the arm's span: an operation
					 * armed and then cut, as if the timeslot ended just
					 * after the arm returned. Live for the gate's DENIED,
					 * or aborted by the signal - never both, never ETIME. */
					zassert_true((!lmt_op_idle() && term == 0u) ||
						     (mode == 2 && lmt_op_idle() && term == 1u),
						     "%s %s n %u: OK, idle %d, %u terminals", kn[kind],
						     mn[mode], n, lmt_op_idle(), term);
				} else {
					zassert_equal(rc, RADIANT_RADIO_ETIME, "%s %s n %u: rc %d",
						      kn[kind], mn[mode], n, rc);
					zassert_true(lmt_op_idle(), "%s %s n %u: slot not freed",
						     kn[kind], mn[mode], n);
					zassert_equal(term, 0u,
						      "%s %s n %u: ETIME and a terminal too",
						      kn[kind], mn[mode], n);
					etimes++;
				}
				if (mode != 1) {
					zassert_false(lmt_timer_running());
					lmt_run(1000u);
					lmt_slot_start(0u);
					zassert_false(lmt_window_conns_on(),
						      "%s n %u: on at the next START", kn[kind], n);
					lmt_slot_end(false);
				}
				zassert_true(radio_err() >= -1 && radio_err() <= 1,
					     "%s %s n %u: the timebase moved", kn[kind], mn[mode], n);
			}
			zassert_true(etimes > 0u, "%s %s: no preemption point failed the arm",
				     kn[kind], mn[mode]);
		}
	}
}
