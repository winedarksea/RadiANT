/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The seam <-> model glue (trainer_sim_model.c): every unit conversion
 * between trainer_source.h and apps/trainer's trainer_model.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "trainer_sim_model.h"

ZTEST(trainer_sim_glue, test_targets_reach_the_model_in_its_units)
{
	struct trainer_sim_model s;
	struct trainer_sim_params p;
	struct trainer_user u;

	trainer_sim_model_init(&s);
	zassert_equal(s.model.mode, TRAINER_MODE_SIM);

	zassert_equal(trainer_sim_model_set_target_power(&s, 250u), 0);
	zassert_equal(s.model.mode, TRAINER_MODE_ERG);
	zassert_equal(s.model.target_power_qw, 1000u, "250 W in 0.25 W units");

	zassert_equal(trainer_sim_model_set_resistance(&s, 375u), 0);
	zassert_equal(s.model.mode, TRAINER_MODE_BASIC);
	zassert_equal(s.model.resistance_half_pct, 75u, "37.5 % in 0.5 % units");
	zassert_equal(trainer_sim_model_set_resistance(&s, 1001u), -ERANGE);

	memset(&p, 0, sizeof(p));
	p.grade_centi_pct = 520;
	p.crr_e5 = 400u;
	p.cw_centi = 51u;
	p.wind_mm_s = 5556; /* 20 km/h headwind */
	p.drafting_centi = 70u;
	zassert_equal(trainer_sim_model_set_sim(&s, &p), 0);
	zassert_equal(s.model.mode, TRAINER_MODE_SIM);
	zassert_equal(s.model.grade_centi_pct, 520);
	zassert_equal(s.model.crr_5e5, 80u, "0.004 in 5e-5 units");
	zassert_equal(s.model.wind_coeff_centi, 51u);
	zassert_equal(s.model.wind_speed_kph, 20);
	zassert_equal(s.model.drafting_centi, 70u);

	u.user_weight_10g = 6800u;
	u.bike_weight_50g = 160u;
	u.wheel_circ_deci_mm = 21991u; /* 0.70 m diameter */
	zassert_equal(trainer_sim_model_set_user(&s, &u), 0);
	zassert_equal(s.model.user_weight_10g, 6800u);
	zassert_equal(s.model.bike_weight_50g, 160u);
	zassert_equal(s.model.wheel_diameter_cm, 70u);
	zassert_equal(trainer_sim_model_set_sim(&s, NULL), -EINVAL);
	zassert_equal(trainer_sim_model_set_user(&s, NULL), -EINVAL);
}

ZTEST(trainer_sim_glue, test_report_comes_from_the_model)
{
	struct trainer_sim_model s;
	struct trainer_report r;

	trainer_sim_model_init(&s);
	trainer_sim_model_tick(&s, 250u, &r);
	zassert_true(r.power_valid && r.cadence_valid && r.speed_valid && r.resistance_valid);
	zassert_true(r.speed_virtual, "no wheel turns on a DK");
	zassert_true(r.pedalling);
	zassert_equal(r.power_w, TRAINER_DEFAULT_RIDER_POWER_W);
	zassert_equal(r.cadence_rpm, TRAINER_DEFAULT_RIDER_CADENCE_RPM);
	zassert_true(r.speed_mm_s > 5000u && r.speed_mm_s < 15000u, "%u", r.speed_mm_s);
	/* At steady state, force x speed is the rider's power. */
	zassert_within((uint32_t)r.resistance_n * r.speed_mm_s / 1000u, 200u, 5u);

	/* ERG: the reported power is the target. */
	(void)trainer_sim_model_set_target_power(&s, 300u);
	trainer_sim_model_tick(&s, 250u, &r);
	zassert_equal(r.power_w, 300u);
	zassert_false(r.erg_speed_low);
}

ZTEST(trainer_sim_glue, test_the_simulated_rider_follows_the_workout)
{
	struct trainer_sim_model s;
	struct trainer_report r;
	enum trainer_source_event evt;

	trainer_sim_model_init(&s);
	zassert_equal(trainer_sim_model_workout(&s, TRAINER_WORKOUT_STOP, &evt), 0);
	zassert_equal(evt, TRAINER_EVT_STOPPED);
	trainer_sim_model_tick(&s, 250u, &r);
	zassert_false(r.pedalling);
	zassert_equal(r.power_w, 0u);

	zassert_equal(trainer_sim_model_workout(&s, TRAINER_WORKOUT_START, &evt), 0);
	zassert_equal(evt, TRAINER_EVT_STARTED);
	trainer_sim_model_tick(&s, 250u, &r);
	zassert_true(r.pedalling);
	zassert_equal(r.cadence_rpm, TRAINER_DEFAULT_RIDER_CADENCE_RPM);

	zassert_equal(trainer_sim_model_workout(&s, TRAINER_WORKOUT_PAUSE, &evt), 0);
	zassert_equal(evt, TRAINER_EVT_PAUSED);
	zassert_equal(trainer_sim_model_workout(&s, TRAINER_WORKOUT_RESUME, &evt), 0);
	zassert_equal(evt, TRAINER_EVT_STARTED);

	/* ERG at no cadence is the textbook "speed too low". */
	(void)trainer_sim_model_set_target_power(&s, 200u);
	trainer_sim_model_set_rider(&s, 200u, 0u);
	trainer_sim_model_tick(&s, 250u, &r);
	zassert_true(r.erg_speed_low);
	zassert_false(r.pedalling);
}

ZTEST_SUITE(trainer_sim_glue, NULL, NULL, NULL, NULL, NULL);
