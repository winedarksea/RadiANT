/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The treadmill's state module and its conversions.
 *
 * WHAT EARNS A CASE HERE is a property that cannot be seen from a receiver.
 * Anything visible on the air belongs in radiant/tests/src/test_profile_fec_tx.c
 * or test_profile_sdm.c, where the page bytes are; what is left, and what this
 * file is for, is the arithmetic between one physical fact and five different
 * wire encodings of it:
 *
 *   1. STRIDES TO STEPS. Three outputs, two units, one factor of two. FE-C
 *      page 19 and ANT+ SDM count strides; BLE RSC counts steps. If exactly one
 *      of the three is ever wrong, it is this, and nothing on any single radio
 *      would look odd.
 *   2. THE INCLINE SCALING, both directions. FE-C is 0.01 % and FTMS is 0.1 %,
 *      so a missing factor of ten is a 3 % grade commanded as 30 % - which the
 *      machine would happily report back, consistently, forever.
 *   3. THE ACCUMULATOR CARRIES. A tick's worth of dropped rounding is nothing
 *      and an hour's worth is metres. That is the shape of error a bench run
 *      reads as a calibration problem rather than as a bug.
 *   4. THE VERTICAL DIVISOR. Grade is a percent in HUNDREDTHS, so the divisor
 *      is 10000 and not 100; two missing zeroes make a 1 % incline climb like a
 *      100 % one, and the field is an accumulator with no plausible range for
 *      anything downstream to question.
 *   5. WHO IS ALLOWED TO COMMAND. The control-owner token, which is policy
 *      rather than arithmetic but belongs here for the same reason: it is
 *      invisible from any single receiver, and it is the only part of the
 *      arbitration that anything can test at all. NOTHING tests
 *      src/treadmill_ble.c - no ztest exists for the GATT service, write_cp()
 *      or the advertising payload - so the policy was put in a kernel-free
 *      module precisely so these cases could reach it.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "treadmill_control.h"
#include "treadmill_state.h"

/* ── The conversions ────────────────────────────────────────────────────── */

ZTEST(treadmill_pages, test_strides_become_steps_and_the_factor_is_two)
{
	/* 85 strides/min in 1/16 units. A runner at this cadence takes 170
	 * steps a minute, which is what a BLE RSC client expects to see. */
	uint32_t cadence_16 = 85u * 16u;

	zassert_equal(treadmill_cadence_strides_min(cadence_16), 85u,
		      "FE-C page 19 counts STRIDES");
	zassert_equal(treadmill_rsc_cadence_from_strides(cadence_16), 170u,
		      "BLE RSC counts STEPS - one stride is two footfalls, and "
		      "rscs.h's 'stride/minute' comment disagrees with "
		      "Nordic's own sample, which simulates 150-180");

	/* Half a stride. The doubling happens BEFORE the rounding, so 85.5
	 * strides is 171 steps and not 172. */
	cadence_16 = 85u * 16u + 8u;
	zassert_equal(treadmill_rsc_cadence_from_strides(cadence_16), 171u,
		      NULL);

	/* Zero is a real answer - a standing runner - and not "unknown". */
	zassert_equal(treadmill_rsc_cadence_from_strides(0u), 0u, NULL);

	/* Clamped, not wrapped: a wrapped 256 steps/min would read as
	 * standing still. */
	zassert_equal(treadmill_rsc_cadence_from_strides(200u * 16u), 255u,
		      NULL);
}

ZTEST(treadmill_pages, test_incline_scales_between_fec_and_ftms)
{
	/* +3.00 % is 300 in FE-C's 0.01 % and 30 in FTMS's 0.1 %. A missing
	 * factor of ten here commands 30 % instead of 3 %. */
	zassert_equal(treadmill_ftms_incline_from_fec(300), 30, NULL);
	zassert_equal(treadmill_fec_incline_from_ftms(30), 300, NULL);

	/* Negative, which is where a rounding rule that truncates towards zero
	 * loses the sign. */
	zassert_equal(treadmill_ftms_incline_from_fec(-250), -25, NULL);
	zassert_equal(treadmill_fec_incline_from_ftms(-25), -250, NULL);

	/* Rounding away from zero, so a small descent stays a descent. */
	zassert_equal(treadmill_ftms_incline_from_fec(-5), -1, NULL);
	zassert_equal(treadmill_ftms_incline_from_fec(5), 1, NULL);
	zassert_equal(treadmill_ftms_incline_from_fec(0), 0, NULL);

	/* The FTMS -> FE-C direction is EXACT, which is why the state struct
	 * keeps the finer unit: nothing is lost on the way in. */
	for (int16_t deci = -300; deci <= 300; deci += 5) {
		int32_t centi = treadmill_fec_incline_from_ftms(deci);

		zassert_equal(treadmill_ftms_incline_from_fec(centi), deci,
			      NULL);
	}
}

ZTEST(treadmill_pages, test_speed_conversions)
{
	/* 1 m/s is 3.6 km/h, which is 360 units of 0.01 km/h. */
	zassert_equal(treadmill_ftms_speed_from_mm_s(1000u), 360u, NULL);
	zassert_equal(treadmill_ftms_speed_from_mm_s(3000u), 1080u, NULL);
	zassert_equal(treadmill_ftms_speed_from_mm_s(0u), 0u, NULL);

	/* And back, to within the round trip's own resolution. */
	zassert_equal(treadmill_mm_s_from_ftms_speed(360u), 1000u, NULL);
	for (uint32_t mm = 0; mm <= 6000u; mm += 173u) {
		uint32_t back = treadmill_mm_s_from_ftms_speed(
			treadmill_ftms_speed_from_mm_s(mm));
		uint32_t diff = (back > mm) ? (back - mm) : (mm - back);

		zassert_true(diff <= 3u,
			     "0.01 km/h is ~2.8 mm/s of resolution");
	}

	/* BLE RSC is 1/256 m/s, a different denominator again. */
	zassert_equal(treadmill_rsc_speed_from_mm_s(1000u), 256u, NULL);
	zassert_equal(treadmill_rsc_speed_from_mm_s(3000u), 768u, NULL);
}

ZTEST(treadmill_pages, test_stride_length)
{
	/* 100 strides over 120 m is 1.20 m per stride, which is 120 in the
	 * 0.01 m unit BOTH FE-C page 17 and BLE RSC use for it. */
	zassert_equal(treadmill_stride_length_cm(120000u, 100u), 120u, NULL);
	/* No strides yet: 0, not a divide by zero. */
	zassert_equal(treadmill_stride_length_cm(1000u, 0u), 0u, NULL);
	/* Clamped at the byte's ceiling rather than wrapped - reporting 2.55 m
	 * is less wrong than reporting the low byte of an implausible one. */
	zassert_equal(treadmill_stride_length_cm(1000000u, 100u), 255u, NULL);
}

/* ── The integrator ─────────────────────────────────────────────────────── */

/* Run `seconds` of simulated time at NODE-TICK granularity. */
static void run(struct treadmill_state *s, uint32_t seconds)
{
	for (uint32_t i = 0; i < seconds * 10u; i++) {
		treadmill_state_tick(s, 100u);
	}
}

ZTEST(treadmill_pages, test_elapsed_time_is_session_time_not_wall_time)
{
	struct treadmill_state s;

	treadmill_state_init(&s);
	zassert_equal(s.state, TREADMILL_STATE_READY,
		      "a powered machine with nobody on it must not report "
		      "IN_USE - that value is a gym's occupancy signal");

	run(&s, 10u);
	zassert_equal(s.elapsed_ms, 0u,
		      "FE-C 8.5.2.2: the accumulator advances only while "
		      "IN_USE");

	treadmill_state_set_state(&s, TREADMILL_STATE_IN_USE);
	run(&s, 10u);
	zassert_equal(s.elapsed_ms, 10000u, NULL);

	/* A pause, which is what makes differencing this field across one
	 * understate the interval - correct for a workout timer, wrong as a
	 * basis for integrating anything over time. */
	treadmill_state_set_state(&s, TREADMILL_STATE_READY);
	run(&s, 30u);
	zassert_equal(s.elapsed_ms, 10000u, NULL);
}

ZTEST(treadmill_pages, test_distance_carry_survives_an_hour)
{
	struct treadmill_state s;

	treadmill_state_init(&s);
	treadmill_state_set_state(&s, TREADMILL_STATE_IN_USE);
	/* 3333 mm/s: 333.3 mm per 100 ms tick, so the tenth of a millimetre
	 * that a carry-free integrator drops is 12 metres per hour - about a
	 * percent of a 20-minute run, and exactly the kind of error that reads
	 * as a calibration problem rather than as a bug. */
	treadmill_state_set_speed(&s, 3333u);

	run(&s, 3600u);

	/* 3333 mm/s x 3600 s = 11 998 800 mm, and the carry means the answer
	 * is exact rather than short. */
	zassert_equal(s.distance_mm, 11998800ull, NULL);
}

ZTEST(treadmill_pages, test_vertical_divisor_is_ten_thousand)
{
	struct treadmill_state s;

	treadmill_state_init(&s);
	treadmill_state_set_state(&s, TREADMILL_STATE_IN_USE);
	treadmill_state_set_speed(&s, 1000u);   /* 1 m/s */
	treadmill_state_set_incline(&s, 100);   /* 1.00 %, i.e. 100 in 0.01 % */

	run(&s, 1000u); /* 1000 m of belt */

	/* 1 % of 1000 m is 10 m. A divisor of 100 instead of 10000 would give
	 * 1000 m of climb and nothing downstream would question it. */
	zassert_equal(s.pos_vertical_mm, 10000u, NULL);
	zassert_equal(s.neg_vertical_mm, 0u, NULL);
}

ZTEST(treadmill_pages, test_descent_counts_up_in_its_own_accumulator)
{
	struct treadmill_state s;

	treadmill_state_init(&s);
	treadmill_state_set_state(&s, TREADMILL_STATE_IN_USE);
	treadmill_state_set_speed(&s, 1000u);

	treadmill_state_set_incline(&s, 200); /* +2.00 % for 500 m */
	run(&s, 500u);
	treadmill_state_set_incline(&s, -200); /* -2.00 % for 500 m */
	run(&s, 500u);

	/* BOTH ARE MAGNITUDES AND BOTH COUNT UP. FE-C page 19 byte 5 is
	 * unsigned, so a signed accumulator here would have the descent
	 * subtract from the climb and report a flat run. */
	zassert_equal(s.pos_vertical_mm, 10000u, NULL);
	zassert_equal(s.neg_vertical_mm, 10000u, NULL);
}

ZTEST(treadmill_pages, test_energy_is_in_the_right_order_of_magnitude)
{
	struct treadmill_state s;
	uint32_t               kcal;

	treadmill_state_init(&s);
	treadmill_state_set_state(&s, TREADMILL_STATE_IN_USE);
	treadmill_state_set_user_mass(&s, 7500u); /* 75.00 kg */
	treadmill_state_set_speed(&s, 3000u);     /* 3 m/s = 10.8 km/h */
	treadmill_state_set_incline(&s, 0);

	run(&s, 3600u);
	kcal = (uint32_t)(s.energy_mkcal / 1000u);

	/*
	 * ACSM at 180 m/min: VO2 = 0.2 x 180 + 3.5 = 39.5 ml/kg/min, so
	 * 39.5 x 75 / 200 = 14.8 kcal/min = 889 kcal/h.
	 *
	 * Asserted as a BAND and not a number, deliberately. The equation is a
	 * population average with no measurement in it and the exact figure is
	 * not a property worth pinning; what IS worth pinning is that the
	 * scale factors are right, because a wrong one lands three orders out
	 * and the milli-kcal carry is what stops it landing an order low.
	 */
	zassert_true(kcal > 800u && kcal < 980u,
		     "expected ~889 kcal/h, got %u - check the 1.2e9 divisor "
		     "in treadmill_state_tick()", kcal);

	/* An incline costs more, and the grade term's divisor is 10000 there
	 * too - a 100x error would be visible as a wildly larger number. */
	{
		struct treadmill_state up;
		uint32_t               up_kcal;

		treadmill_state_init(&up);
		treadmill_state_set_state(&up, TREADMILL_STATE_IN_USE);
		treadmill_state_set_user_mass(&up, 7500u);
		treadmill_state_set_speed(&up, 3000u);
		treadmill_state_set_incline(&up, 500); /* 5.00 % */
		run(&up, 3600u);
		up_kcal = (uint32_t)(up.energy_mkcal / 1000u);

		zassert_true(up_kcal > kcal, NULL);
		zassert_true(up_kcal < kcal * 2u,
			     "a 5 %% grade adds about 20 %%, not 100x - if it "
			     "did, the grade term's divisor is 100 short");
	}
}

ZTEST(treadmill_pages, test_both_control_paths_land_on_one_target)
{
	struct treadmill_state s;

	treadmill_state_init(&s);

	/* FE-C page 51 says +3.00 % in 0.01 % units. */
	zassert_equal(treadmill_state_request_incline(&s, 300), 300, NULL);
	zassert_equal(s.target_incline_centi_pct, 300, NULL);

	/* An FTMS client then reads it back through the 0.1 % conversion and
	 * sees the same grade. This is the symmetry the whole application is
	 * built around, and it is one struct member rather than two. */
	zassert_equal(treadmill_ftms_incline_from_fec(s.target_incline_centi_pct),
		      30, NULL);

	/* And an FTMS Set Target Inclination of -2.5 % is visible to FE-C. */
	zassert_equal(treadmill_state_request_incline(
			      &s, treadmill_fec_incline_from_ftms(-25)),
		      -250, NULL);
	zassert_equal(s.target_incline_centi_pct, -250, NULL);

	/* Clamped to what page 17 can REPORT (+-100.00 %), not to what page 51
	 * can command (+-200.00 %): a machine that accepted a grade it could
	 * never report back would answer every page 17 with a number that
	 * disagrees with the command it just acknowledged. */
	zassert_equal(treadmill_state_request_incline(&s, 20000), 10000, NULL);
	zassert_equal(treadmill_state_request_incline(&s, -20000), -10000, NULL);
}

ZTEST(treadmill_pages, test_lap_toggle_flips_on_a_finished_session)
{
	struct treadmill_state s;
	bool                   before;

	treadmill_state_init(&s);
	before = s.lap_toggle;

	treadmill_state_set_state(&s, TREADMILL_STATE_IN_USE);
	zassert_equal(s.lap_toggle, before,
		      "starting a session is not a lap event");

	treadmill_state_set_state(&s, TREADMILL_STATE_FINISHED);
	zassert_not_equal(s.lap_toggle, before,
			  "FE-C 8.5.2.7.1: the lap event is a CHANGE of the "
			  "bit, not a value of it");
}

ZTEST(treadmill_pages, test_heart_rate_source_does_not_flap)
{
	struct treadmill_state s;

	treadmill_state_init(&s);
	zassert_equal(s.heart_rate_bpm, TREADMILL_HR_NONE, NULL);
	zassert_false(s.hr_from_ant, NULL);

	treadmill_state_set_heart_rate(&s, 150u, true);
	zassert_true(s.hr_from_ant, NULL);

	/* A reading that arrives without the ANT+ flag - a console's hand
	 * sensors, say - must not clear the source nibble while the strap is
	 * still the source, or a head unit redraws its icon four times a
	 * second. */
	treadmill_state_set_heart_rate(&s, 151u, false);
	zassert_true(s.hr_from_ant, NULL);

	/* Losing the reading altogether does clear it. */
	treadmill_state_set_heart_rate(&s, TREADMILL_HR_NONE, false);
	zassert_false(s.hr_from_ant, NULL);
}

/* ── The control-owner token ────────────────────────────────────────────────
 *
 * WHY THESE ARE HERE AND NOT IN A BLE TEST: there is no ztest for
 * src/treadmill_ble.c at all. The GATT service, write_cp() and the advertising
 * payload are covered only by BUILD_ASSERTs and a bench procedure, so
 * arbitration living inside that file would be arbitration nothing can reach.
 * treadmill_control.c is kernel-free for exactly this reason - no Zephyr
 * header, no lock, no clock, TOLD what time it is - and these cases drive the
 * whole policy with no radio and no stack.
 *
 * WHAT THEY DO NOT COVER, stated so a pass is not over-read: they prove the
 * POLICY, not the plumbing. That both paths marshal onto the system workqueue
 * before they reach the token (main.c's fec_cmd item, treadmill_ble.c's
 * cp_work item) is what makes the lock-freedom sound, and it is not visible
 * from here - the same limit test_both_control_paths_land_on_one_target has,
 * which drives both paths sequentially on one thread and therefore proves the
 * conversions rather than the concurrency.
 */

#define CTRL_TIMEOUT_MS 30000u

ZTEST(treadmill_pages, test_an_explicit_ble_claim_preempts_an_ant_owner)
{
	treadmill_control_init(CTRL_TIMEOUT_MS);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_NONE, NULL);

	/* An FE-C page 51 arrives first and claims implicitly - there is no
	 * FE-C acquire handshake, so commanding IS the claim. */
	zassert_equal(treadmill_control_claim(TREADMILL_CTRL_ANT, false, 1000u),
		      0, NULL);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_ANT, NULL);

	/* Then a phone runs FTMS Request Control, which IS an explicit
	 * acquire, and takes the machine. This asymmetry is the whole policy
	 * and it is spec-native: FTMS 4.16.2 has the handshake, FE-C has
	 * none. */
	zassert_equal(treadmill_control_claim(TREADMILL_CTRL_BLE, true, 2000u),
		      0, NULL);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_BLE, NULL);
}

ZTEST(treadmill_pages, test_ant_never_preempts_a_ble_owner)
{
	treadmill_control_init(CTRL_TIMEOUT_MS);

	zassert_equal(treadmill_control_claim(TREADMILL_CTRL_BLE, true, 1000u),
		      0, NULL);

	/* The direction that must NOT work. An FE-C page 51 now gets -EACCES,
	 * which main.c turns into page 71 status
	 * PROFILE_FEC_CMD_STATUS_REJECTED (3) - the profile's own constant for
	 * this, and the only signal ANT+ has, since there is no connection to
	 * drop and no status characteristic to notify. */
	zassert_equal(treadmill_control_claim(TREADMILL_CTRL_ANT, false, 2000u),
		      -EACCES, NULL);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_BLE,
		      "a refused claim must not disturb the owner");

	/* Refusal is not a one-off sulk: it holds for as long as the BLE
	 * client has it, including past the ANT idle timeout, which does not
	 * apply to a BLE owner at all. */
	treadmill_control_tick(1000u + CTRL_TIMEOUT_MS * 3u);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_BLE,
		      "the idle timeout is for ANT+ only - a phone that has "
		      "taken control is still there whether or not it has "
		      "commanded anything this minute");
	zassert_equal(treadmill_control_claim(TREADMILL_CTRL_ANT, false,
					      1000u + CTRL_TIMEOUT_MS * 3u),
		      -EACCES, NULL);
}

ZTEST(treadmill_pages, test_an_idle_ant_claim_expires_and_a_command_refreshes_it)
{
	treadmill_control_init(CTRL_TIMEOUT_MS);

	zassert_equal(treadmill_control_claim(TREADMILL_CTRL_ANT, false, 1000u),
		      0, NULL);

	/* Just short of the timeout: still owned. */
	treadmill_control_tick(1000u + CTRL_TIMEOUT_MS - 1u);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_ANT, NULL);

	/* Every command pushes the deadline out, so a controller that keeps
	 * commanding keeps the machine. */
	zassert_equal(treadmill_control_claim(TREADMILL_CTRL_ANT, false,
					      1000u + CTRL_TIMEOUT_MS - 1u),
		      0, NULL);
	treadmill_control_tick(1000u + CTRL_TIMEOUT_MS + 1u);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_ANT,
		      "a refresh must move the deadline, not just succeed");

	/* Stop commanding and it lapses. THIS IS THE ONLY RELEASE AN ANT
	 * CLAIM HAS: ANT+ is connectionless, so a head unit that walks away
	 * generates no event whatsoever. Without this, one page 51 early in a
	 * session would lock a phone out until power cycle. */
	treadmill_control_tick(1000u + CTRL_TIMEOUT_MS * 2u + 1u);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_NONE, NULL);

	/* And a phone can now take it with no explicit acquire needed,
	 * because nobody holds it. */
	zassert_equal(treadmill_control_claim(TREADMILL_CTRL_BLE, false,
					      1000u + CTRL_TIMEOUT_MS * 2u + 2u),
		      0, NULL);
}

ZTEST(treadmill_pages, test_release_hands_the_machine_back_to_ant)
{
	treadmill_control_init(CTRL_TIMEOUT_MS);

	zassert_equal(treadmill_control_claim(TREADMILL_CTRL_BLE, true, 1000u),
		      0, NULL);
	zassert_equal(treadmill_control_claim(TREADMILL_CTRL_ANT, false, 1100u),
		      -EACCES, NULL);

	/* An FTMS Reset (0x01) or a disconnect. Both reach here. */
	treadmill_control_release(TREADMILL_CTRL_BLE);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_NONE, NULL);

	/* The head unit's very next page 51 is applied rather than REJECTED,
	 * with no handshake of its own - which is the point, since FE-C has
	 * none to offer. */
	zassert_equal(treadmill_control_claim(TREADMILL_CTRL_ANT, false, 1200u),
		      0, NULL);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_ANT, NULL);

	/* A release by somebody who does not hold it is a no-op and not an
	 * error - a disconnect callback should not have to ask first. */
	treadmill_control_release(TREADMILL_CTRL_BLE);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_ANT, NULL);
}

ZTEST(treadmill_pages, test_the_ant_timeout_survives_an_uptime_wrap)
{
	/* k_uptime_get_32() wraps every 49.7 days and a treadmill in a gym is
	 * powered for months. Signed arithmetic here would make the claim
	 * expire instantly at the wrap; unsigned subtraction yields the true
	 * interval across it. */
	const uint32_t before_wrap = 0xFFFFF000u;

	treadmill_control_init(CTRL_TIMEOUT_MS);
	zassert_equal(treadmill_control_claim(TREADMILL_CTRL_ANT, false,
					      before_wrap),
		      0, NULL);

	/* 0x2000 counts past the claim, having wrapped through zero. That is
	 * 8192 ms, well inside the 30 s timeout, so the claim must stand. */
	treadmill_control_tick(before_wrap + 0x2000u);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_ANT,
		      "an uptime wrap is not an idle timeout");

	/* And past the real deadline it still expires. */
	treadmill_control_tick(before_wrap + CTRL_TIMEOUT_MS + 1u);
	zassert_equal(treadmill_control_owner(), TREADMILL_CTRL_NONE, NULL);
}

ZTEST(treadmill_pages, test_reads_are_never_gated_by_ownership)
{
	/* The deck-moving pages, and the reason the token exists. */
	zassert_true(treadmill_control_page_is_gated(0x33u),
		     "page 51, Track Resistance - the grade command");
	zassert_true(treadmill_control_page_is_gated(0x32u),
		     "page 50, Wind Resistance - the other half of the "
		     "simulation parameter set, gated with page 51 because "
		     "honouring one and refusing the other is how a controller "
		     "decides simulation mode is broken");

	/*
	 * AND THE ONES THAT MUST NOT BE. Refusing a read would make the
	 * machine look broken to a head unit that is merely observing it,
	 * which is the opposite of what the token is for: nothing here is
	 * allowed to degrade what a listener sees.
	 */
	zassert_false(treadmill_control_page_is_gated(0x46u),
		      "page 70, Request Data Page - a READ, and refusing it "
		      "would make an observing head unit report the machine as "
		      "faulty");
	zassert_false(treadmill_control_page_is_gated(0x37u),
		      "page 55, User Configuration");

	/* 48 and 49 answer NOT_SUPPORTED to everybody. Gating them would
	 * report a missing capability as a missing permission, which tells a
	 * controller to retry something that will never work. */
	zassert_false(treadmill_control_page_is_gated(0x30u), "page 48");
	zassert_false(treadmill_control_page_is_gated(0x31u), "page 49");

	/* The data pages this machine transmits are not commands at all and
	 * must never appear in the gated set. */
	zassert_false(treadmill_control_page_is_gated(16u), NULL);
	zassert_false(treadmill_control_page_is_gated(17u), NULL);
	zassert_false(treadmill_control_page_is_gated(19u), NULL);
	zassert_false(treadmill_control_page_is_gated(0x50u), NULL);
	zassert_false(treadmill_control_page_is_gated(0x51u), NULL);
}

ZTEST_SUITE(treadmill_pages, NULL, NULL, NULL, NULL, NULL);
