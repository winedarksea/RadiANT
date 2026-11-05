/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The trainer simulator's physics and its ACK fault model.
 *
 * THE PHYSICS AGAINST HAND-COMPUTED NUMBERS, not against itself. Every
 * expected force below is worked out in the comment beside it from FE-C
 * SS8.8.4's three equations with the profile's defaults (75 kg rider, 10 kg
 * bike, Crr 0.004, wind coefficient 0.51 kg/m, drafting 1.00, g = 9.81), so a
 * transcription error in trainer_model.c disagrees with arithmetic rather
 * than with a second copy of the same transcription.
 *
 * THE FAULT MODEL STATISTICALLY, over ten thousand draws from a fixed seed.
 * The tolerances are several binomial standard deviations wide, so they do
 * not depend on the particular seed - and the reproducibility case is what
 * pins the seed's own determinism.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "profile_fec.h"
#include "profile_fec_tx.h"
#include "trainer_ackfault.h"
#include "trainer_model.h"

#define NEWTON_TOL 0.01f

static float fabs_f(float x)
{
	return x < 0.0f ? -x : x;
}

/* ── The physics ────────────────────────────────────────────────────────── */

ZTEST(trainer_model, test_flat_road_at_ten_metres_per_second)
{
	struct trainer_model m;

	trainer_model_init(&m);

	/* rolling = 85 x 9.81 x 0.004       =  3.3354 N
	 * wind    = 0.5 x 0.51 x 10^2 x 1.0 = 25.5    N
	 * total                             = 28.8354 N, so 288.354 W at 10 m/s */
	zassert_true(fabs_f(trainer_model_resistance_n(&m, 10.0f) - 28.8354f) <
			     NEWTON_TOL,
		     "flat at 10 m/s: %d mN",
		     (int)(trainer_model_resistance_n(&m, 10.0f) * 1000.0f));
	zassert_true(fabs_f(trainer_model_solve_speed(&m, 288.354f) - 10.0f) <
			     0.01f,
		     "the solver must invert the same equation");
	zassert_equal(trainer_model_solve_speed(&m, 0.0f), 0.0f,
		      "no power, no speed");
}

ZTEST(trainer_model, test_five_percent_climb)
{
	struct trainer_model                    m;
	struct profile_fec_cmd_track_resistance t;

	trainer_model_init(&m);
	memset(&t, 0, sizeof(t));
	t.grade_centi_pct = 500;
	t.grade_valid = true;
	t.rolling_resistance_valid = false; /* -> Crr 0.004 */
	trainer_model_track(&m, &t);
	zassert_equal(m.mode, TRAINER_MODE_SIM, NULL);

	/* gravity = 85 x 9.81 x 0.05 = 41.6925 N
	 * rolling                    =  3.3354 N
	 * wind    = 0.5 x 0.51 x 25  =  6.375  N
	 * total                      = 51.4029 N at 5 m/s, 257.01 W */
	zassert_true(fabs_f(trainer_model_resistance_n(&m, 5.0f) - 51.4029f) <
			     NEWTON_TOL, NULL);
	zassert_true(fabs_f(trainer_model_solve_speed(&m, 257.0145f) - 5.0f) <
			     0.01f, NULL);
}

ZTEST(trainer_model, test_headwind_adds_to_the_rider_speed)
{
	struct trainer_model                   m;
	struct profile_fec_cmd_wind_resistance w;

	trainer_model_init(&m);
	memset(&w, 0, sizeof(w));
	w.coefficient_valid = false; /* -> 0.51 */
	w.wind_speed_kph = 18;       /* 5 m/s headwind */
	w.wind_speed_valid = true;
	w.drafting_valid = false;    /* -> 1.00 */
	trainer_model_wind(&m, &w);

	/* wind = 0.5 x 0.51 x (10 + 5)^2 = 57.375 N, + 3.3354 rolling */
	zassert_true(fabs_f(trainer_model_resistance_n(&m, 10.0f) - 60.7104f) <
			     NEWTON_TOL, NULL);

	/* Half the drafting factor halves the wind term only. */
	w.drafting_centi = 50u;
	w.drafting_valid = true;
	trainer_model_wind(&m, &w);
	zassert_true(fabs_f(trainer_model_resistance_n(&m, 10.0f) -
			    (3.3354f + 28.6875f)) < NEWTON_TOL, NULL);
}

ZTEST(trainer_model, test_steep_descent_clamps_at_zero_newtons)
{
	struct trainer_model                    m;
	struct profile_fec_cmd_track_resistance t;
	float                                   flat;

	trainer_model_init(&m);
	flat = trainer_model_solve_speed(&m, 200.0f);

	memset(&t, 0, sizeof(t));
	t.grade_centi_pct = -1000; /* -10 % */
	t.grade_valid = true;
	trainer_model_track(&m, &t);

	/* gravity = -83.385 N against 3.3354 + 6.375 at 5 m/s: -73.67 N, which
	 * a brake cannot apply. */
	zassert_equal(trainer_model_resistance_n(&m, 5.0f), 0.0f,
		      "a trainer cannot push the rider downhill");
	zassert_true(trainer_model_solve_speed(&m, 200.0f) > flat,
		     "and the descent is still faster than the flat");
}

ZTEST(trainer_model, test_invalid_fields_fall_back_to_the_defaults)
{
	struct trainer_model                    m;
	struct profile_fec_cmd_user_config      u;
	struct profile_fec_cmd_track_resistance t;

	trainer_model_init(&m);

	/* A page 55 of nothing but sentinels, as the decoder reports one: every
	 * _valid false, every value garbage that must not be used. (The decoder
	 * itself is radiant/tests' business - test_profile_fec_tx.c covers its
	 * sentinels - and is not linked into this image.) */
	memset(&u, 0xA5, sizeof(u));
	u.user_weight_valid = false;
	u.bike_weight_valid = false;
	u.wheel_offset_valid = false;
	u.wheel_diameter_valid = false;
	u.gear_ratio_valid = false;
	trainer_model_user(&m, &u);
	zassert_equal(m.user_weight_10g, TRAINER_DEFAULT_USER_WEIGHT_10G, NULL);
	zassert_equal(m.bike_weight_50g, TRAINER_DEFAULT_BIKE_WEIGHT_50G, NULL);
	zassert_equal(m.wheel_diameter_cm, TRAINER_DEFAULT_WHEEL_DIAM_CM, NULL);

	/* At standstill only rolling is left: 85 x 9.81 x 0.004. */
	zassert_true(fabs_f(trainer_model_resistance_n(&m, 0.0f) - 3.3354f) <
			     NEWTON_TOL, NULL);

	/* A real page 55: 90 kg + 8 kg -> 98 x 9.81 x 0.004 = 3.84552 N. */
	memset(&u, 0, sizeof(u));
	u.user_weight_10g = 9000u;
	u.user_weight_valid = true;
	u.bike_weight_50g = 160u;
	u.bike_weight_valid = true;
	trainer_model_user(&m, &u);
	zassert_true(fabs_f(trainer_model_resistance_n(&m, 0.0f) - 3.84552f) <
			     NEWTON_TOL, NULL);

	/* Page 51's flat sentinel and the default Crr, as the decoder reports
	 * an all-0xFF page: grade 0 and not valid, Crr not valid. */
	memset(&t, 0, sizeof(t));
	t.grade_centi_pct = 0;
	t.grade_valid = false;
	t.rolling_resistance = 0xFFu;
	t.rolling_resistance_valid = false;
	m.grade_centi_pct = 700;
	m.crr_5e5 = 200u;
	trainer_model_track(&m, &t);
	zassert_equal(m.grade_centi_pct, 0, "0xFFFF means assume flat");
	zassert_equal(m.crr_5e5, TRAINER_DEFAULT_CRR_5E5, NULL);
}

ZTEST(trainer_model, test_erg_reports_the_target_and_basic_brakes)
{
	struct trainer_model       m;
	struct profile_fec_general g;
	struct profile_fec_settings s;
	struct profile_fec_trainer t;

	trainer_model_init(&m);
	trainer_model_target_power(&m, 1000u); /* 250 W */
	trainer_model_tick(&m, 250u);
	zassert_equal(m.power_w, 250u, "ERG reports the target, not the rider");
	trainer_model_pages(&m, &g, &s, &t);
	zassert_equal(t.inst_power_w, 250u, NULL);
	zassert_equal(t.flags, PROFILE_FEC_TARGET_POWER_OK, NULL);

	/* Stop pedalling: no power, and the textbook "speed too low". */
	trainer_model_set_rider(&m, 200u, 0u);
	trainer_model_tick(&m, 250u);
	trainer_model_pages(&m, &g, &s, &t);
	zassert_equal(t.inst_power_w, 0u, NULL);
	zassert_equal(t.flags, PROFILE_FEC_TARGET_POWER_SPEED_LOW, NULL);
	zassert_equal(g.state, PROFILE_FEC_STATE_READY, NULL);

	/* BASIC 10 %: a 200 N brake on a flat road, plus rolling. */
	trainer_model_set_rider(&m, 200u, 90u);
	trainer_model_basic_resistance(&m, 20u);
	zassert_true(fabs_f(trainer_model_resistance_n(&m, 0.0f) -
			    (200.0f + 3.3354f)) < NEWTON_TOL, NULL);
	trainer_model_tick(&m, 250u);
	trainer_model_pages(&m, &g, &s, &t);
	zassert_equal(s.resistance_half_pct, 20u,
		      "page 17 reports the commanded resistance in BASIC");
}

ZTEST(trainer_model, test_pages_carry_type_25_virtual_speed_and_the_grade)
{
	struct trainer_model                    m;
	struct profile_fec_cmd_track_resistance tr;
	struct profile_fec_general              g;
	struct profile_fec_settings             s;
	struct profile_fec_trainer              t;
	uint8_t                                 acc_before;

	trainer_model_init(&m);
	memset(&tr, 0, sizeof(tr));
	tr.grade_centi_pct = 300;
	tr.grade_valid = true;
	trainer_model_track(&m, &tr);

	trainer_model_tick(&m, 250u);
	acc_before = (uint8_t)m.acc_power_w;
	trainer_model_tick(&m, 250u);
	trainer_model_pages(&m, &g, &s, &t);

	zassert_equal(g.equipment_type, PROFILE_FEC_TYPE_TRAINER, NULL);
	zassert_true(g.speed_virtual, "no wheel turns: the speed is virtual");
	zassert_true(g.speed_valid, NULL);
	zassert_equal(g.elapsed_time_qs, 2u, "two quarter-seconds");
	zassert_equal(s.incline_centi_pct, 300,
		      "SS8.8.4.1: the incline matches the commanded grade");
	zassert_true(s.resistance_valid, "mandatory on a trainer");
	zassert_equal(t.event_count, 2u, NULL);
	zassert_equal((uint8_t)(t.acc_power_w - acc_before), 200u,
		      "one tick adds one update's power");
	zassert_equal(t.inst_power_w, 200u, NULL);
	zassert_equal(t.cadence_rpm, 90u, NULL);
	zassert_equal(g.state, PROFILE_FEC_STATE_IN_USE, NULL);
}

/* ── The fault model ────────────────────────────────────────────────────── */

#define DRAWS 10000u

struct tally {
	uint32_t action[3];
	uint32_t cause[5];
	int32_t  off_min, off_max;
	int64_t  off_sum;
	uint32_t n_ack;
};

static void run_draws(const struct trainer_ackfault_params *p, uint32_t n,
		      uint64_t step_us, struct tally *t)
{
	struct trainer_ackfault          af;
	struct trainer_ackfault_decision d;

	memset(t, 0, sizeof(*t));
	t->off_min = INT32_MAX;
	t->off_max = INT32_MIN;
	trainer_ackfault_init(&af, p);

	for (uint32_t i = 0; i < n; i++) {
		trainer_ackfault_decide(&af, (uint64_t)i * step_us, &d);
		t->action[d.action]++;
		t->cause[d.cause]++;
		if (d.action == TRAINER_ACTION_ACK) {
			t->n_ack++;
			t->off_sum += d.offset_us;
			if (d.offset_us < t->off_min) {
				t->off_min = d.offset_us;
			}
			if (d.offset_us > t->off_max) {
				t->off_max = d.offset_us;
			}
		}
	}
	zassert_equal(af.n, n, NULL);
}

static bool within(uint32_t got, uint32_t want, uint32_t tol)
{
	return got + tol >= want && got <= want + tol;
}

ZTEST(trainer_model, test_ideal_acknowledges_everything_on_time)
{
	struct trainer_ackfault_params p;
	struct tally                   t;

	trainer_ackfault_params_ideal(&p);
	run_draws(&p, DRAWS, 1000000u, &t);
	zassert_equal(t.action[TRAINER_ACTION_ACK], DRAWS, NULL);
	zassert_equal(t.off_min, 0, NULL);
	zassert_equal(t.off_max, 0, NULL);
}

ZTEST(trainer_model, test_mute_and_deaf_rates_over_ten_thousand_draws)
{
	struct trainer_ackfault_params p;
	struct tally                   t;

	/* lossy25: 2500 expected, binomial sd 43. */
	zassert_equal(trainer_ackfault_preset("lossy25", &p), 0, NULL);
	run_draws(&p, DRAWS, 1000000u, &t);
	zassert_true(within(t.action[TRAINER_ACTION_MUTE], 2500u, 200u),
		     "mute %u of 10000", t.action[TRAINER_ACTION_MUTE]);
	zassert_equal(t.action[TRAINER_ACTION_DEAF], 0u, NULL);

	/* DEAF is drawn first, MUTE from what is left: 10 % and 0.9 x 20 %. */
	trainer_ackfault_params_ideal(&p);
	p.deaf_pm = 100u;
	p.mute_pm = 200u;
	run_draws(&p, DRAWS, 1000000u, &t);
	zassert_true(within(t.action[TRAINER_ACTION_DEAF], 1000u, 150u),
		     "deaf %u", t.action[TRAINER_ACTION_DEAF]);
	zassert_true(within(t.action[TRAINER_ACTION_MUTE], 1800u, 200u),
		     "mute %u", t.action[TRAINER_ACTION_MUTE]);
	zassert_equal(t.cause[TRAINER_CAUSE_DEAF],
		      t.action[TRAINER_ACTION_DEAF], NULL);
}

ZTEST(trainer_model, test_offset_and_jitter_stay_inside_their_span)
{
	struct trainer_ackfault_params p;
	struct tally                   t;
	int32_t                        mean;

	trainer_ackfault_params_ideal(&p);
	p.offset_us = 100;
	p.jitter_us = 200u;
	run_draws(&p, DRAWS, 1000000u, &t);

	zassert_equal(t.n_ack, DRAWS, NULL);
	zassert_true(t.off_min >= -100 && t.off_max <= 300,
		     "uniform +-200 about +100: %d..%d", t.off_min, t.off_max);
	/* Ten thousand draws reach within a few microseconds of both ends. */
	zassert_true(t.off_min <= -95 && t.off_max >= 295, NULL);
	/* sd of the mean is ~1.2 us. */
	mean = (int32_t)(t.off_sum / (int64_t)t.n_ack);
	zassert_true(mean >= 90 && mean <= 110, "mean %d", mean);
}

ZTEST(trainer_model, test_a_seed_reproduces_its_run_and_another_does_not)
{
	struct trainer_ackfault_params   p;
	struct trainer_ackfault          a, b;
	struct trainer_ackfault_decision da, db;
	uint32_t                         differ = 0u;

	trainer_ackfault_params_ideal(&p);
	p.mute_pm = 300u;
	p.jitter_us = 100u;
	trainer_ackfault_init(&a, &p);
	trainer_ackfault_init(&b, &p);
	for (uint32_t i = 0; i < 1000u; i++) {
		trainer_ackfault_decide(&a, i, &da);
		trainer_ackfault_decide(&b, i, &db);
		zassert_equal(da.action, db.action, NULL);
		zassert_equal(da.offset_us, db.offset_us, NULL);
	}

	/* reset() goes back to the start of the same stream. */
	trainer_ackfault_reset(&a);
	trainer_ackfault_init(&b, &p);
	trainer_ackfault_decide(&a, 0u, &da);
	trainer_ackfault_decide(&b, 0u, &db);
	zassert_equal(da.offset_us, db.offset_us, NULL);

	p.seed = 12345u;
	trainer_ackfault_init(&b, &p);
	trainer_ackfault_reset(&a);
	for (uint32_t i = 0; i < 1000u; i++) {
		trainer_ackfault_decide(&a, i, &da);
		trainer_ackfault_decide(&b, i, &db);
		differ += (da.action != db.action || da.offset_us != db.offset_us);
	}
	zassert_true(differ > 100u, "a different seed is a different run");
}

ZTEST(trainer_model, test_busy_window_mutes_without_extending_itself)
{
	struct trainer_ackfault_params   p;
	struct trainer_ackfault          af;
	struct trainer_ackfault_decision d;

	trainer_ackfault_params_ideal(&p);
	p.busy_ms = 1500u;
	trainer_ackfault_init(&af, &p);

	/* Commands at 1 Hz against a 1.5 s busy window: ACK, then the next is
	 * busy, then free again - because a busy MUTE does not restart the
	 * window. If it did, everything after the first would be MUTE. */
	for (uint32_t i = 0; i < 20u; i++) {
		trainer_ackfault_decide(&af, (uint64_t)i * 1000000u, &d);
		if ((i % 2u) == 0u) {
			zassert_equal(d.action, TRAINER_ACTION_ACK, "i=%u", i);
		} else {
			zassert_equal(d.action, TRAINER_ACTION_MUTE, "i=%u", i);
			zassert_equal(d.cause, TRAINER_CAUSE_BUSY, NULL);
		}
	}

	/* A command never heard does not make the trainer busy. */
	p.deaf_pm = 1000u;
	trainer_ackfault_init(&af, &p);
	for (uint32_t i = 0; i < 10u; i++) {
		trainer_ackfault_decide(&af, (uint64_t)i * 100000u, &d);
		zassert_equal(d.action, TRAINER_ACTION_DEAF, NULL);
	}
	zassert_equal(af.n_cause[TRAINER_CAUSE_BUSY], 0u, NULL);

	/* But an applied MUTE does. */
	p.deaf_pm = 0u;
	p.mute_pm = 1000u;
	trainer_ackfault_init(&af, &p);
	trainer_ackfault_decide(&af, 0u, &d);
	zassert_equal(d.cause, TRAINER_CAUSE_MUTE, NULL);
	trainer_ackfault_decide(&af, 1000000u, &d);
	zassert_equal(d.cause, TRAINER_CAUSE_BUSY, NULL);
}

ZTEST(trainer_model, test_gilbert_chain_states_and_burst_length)
{
	struct trainer_ackfault_params   p;
	struct trainer_ackfault          af;
	struct trainer_ackfault_decision d;
	struct tally                     t;
	uint32_t                         bursts = 0u, in_burst = 0u;
	bool                             prev_bad = false;

	/* Never enters BAD. */
	trainer_ackfault_params_ideal(&p);
	p.p_good_pm = 500u;
	run_draws(&p, DRAWS, 1000u, &t);
	zassert_equal(t.cause[TRAINER_CAUSE_BURST], 0u, NULL);

	/* Enters at once and never leaves; the bad action is the one asked. */
	p.p_bad_pm = 1000u;
	p.p_good_pm = 0u;
	p.bad_action = TRAINER_ACTION_DEAF;
	run_draws(&p, 1000u, 1000u, &t);
	zassert_equal(t.action[TRAINER_ACTION_DEAF], 1000u, NULL);
	zassert_equal(t.cause[TRAINER_CAUSE_BURST], 1000u, NULL);

	/* The stationary share is p_bad / (p_bad + p_good) = 50/300 = 16.7 %,
	 * and a burst lasts 1000/p_good = 4 packets on average. */
	p.p_bad_pm = 50u;
	p.p_good_pm = 250u;
	p.bad_action = TRAINER_ACTION_MUTE;
	trainer_ackfault_init(&af, &p);
	for (uint32_t i = 0; i < DRAWS; i++) {
		bool bad;

		trainer_ackfault_decide(&af, (uint64_t)i * 1000u, &d);
		bad = (d.cause == TRAINER_CAUSE_BURST);
		if (bad) {
			zassert_equal(d.action, TRAINER_ACTION_MUTE, NULL);
			in_burst++;
			if (!prev_bad) {
				bursts++;
			}
		}
		prev_bad = bad;
	}
	zassert_true(within(in_burst, 1667u, 300u), "bad share %u", in_burst);
	zassert_true(bursts > 0u, NULL);
	zassert_true(in_burst * 10u >= bursts * 34u &&
			     in_burst * 10u <= bursts * 46u,
		     "mean burst %u/%u, want ~4", in_burst, bursts);
}

ZTEST(trainer_model, test_shell_parameters_and_presets)
{
	struct trainer_ackfault_params p, before;

	trainer_ackfault_params_ideal(&p);
	zassert_equal(trainer_ackfault_set(&p, "offset_us", "-400"), 0, NULL);
	zassert_equal(p.offset_us, -400, NULL);
	zassert_equal(trainer_ackfault_set(&p, "bad_action", "deaf"), 0, NULL);
	zassert_equal(p.bad_action, TRAINER_ACTION_DEAF, NULL);
	zassert_equal(trainer_ackfault_set(&p, "seed", "0xdeadbeef"), 0, NULL);
	zassert_equal(p.seed, 0xDEADBEEFu, NULL);

	before = p;
	zassert_equal(trainer_ackfault_set(&p, "mute_pm", "1001"), -1, NULL);
	zassert_equal(trainer_ackfault_set(&p, "offset_us", "12x"), -1, NULL);
	zassert_equal(trainer_ackfault_set(&p, "nonsense", "1"), -1, NULL);
	zassert_equal(trainer_ackfault_set(&p, "bad_action", "ack"), -1, NULL);
	zassert_mem_equal(&p, &before, sizeof(p), "a refusal changes nothing");

	zassert_equal(trainer_ackfault_preset("late300", &p), 0, NULL);
	zassert_equal(p.offset_us, 300, NULL);
	zassert_equal(trainer_ackfault_preset("late-edge", &p), 0, NULL);
	zassert_equal(p.offset_us, 250, "the dongle window's late edge");
	zassert_equal(trainer_ackfault_preset("jitter", &p), 0, NULL);
	zassert_equal(p.jitter_us, 200u, NULL);
	zassert_equal(trainer_ackfault_preset("wahoo", &p), 0, NULL);
	before = p;
	zassert_equal(trainer_ackfault_preset("nope", &p), -1, NULL);
	zassert_mem_equal(&p, &before, sizeof(p), NULL);
}

ZTEST_SUITE(trainer_model, NULL, NULL, NULL, NULL, NULL);
