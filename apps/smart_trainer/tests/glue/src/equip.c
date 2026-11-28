/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_equip: the session, the accumulators, the FE-C pages and echoes,
 * and the FTMS Indoor Bike Data record with its Reset baseline.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include "trainer_equip.h"

static struct trainer_report riding(uint16_t w, uint8_t rpm, uint32_t mm_s)
{
	struct trainer_report r;

	memset(&r, 0, sizeof(r));
	r.power_w = w;
	r.power_valid = true;
	r.cadence_rpm = rpm;
	r.cadence_valid = true;
	r.speed_mm_s = mm_s;
	r.speed_valid = true;
	r.speed_virtual = true;
	r.resistance_n = 500u;
	r.resistance_valid = true;
	r.pedalling = (rpm != 0u);
	return r;
}

ZTEST(trainer_equip, test_pedalling_starts_a_session_from_idle_only)
{
	struct trainer_equip e;
	struct trainer_report r = riding(200u, 90u, 9000u);
	struct trainer_report still = riding(0u, 0u, 0u);

	trainer_equip_init(&e, 2000u);
	zassert_equal(e.run, TRAINER_RUN_IDLE);
	zassert_equal(trainer_equip_report(&e, &still), TRAINER_RUN_IDLE);
	zassert_equal(e.run, TRAINER_RUN_IDLE, "no pedalling, no session");
	zassert_equal(trainer_equip_report(&e, &r), TRAINER_RUN_IDLE);
	zassert_equal(e.run, TRAINER_RUN_RUNNING);

	/* PAUSED and STOPPED are left only by an explicit start or a console
	 * action, never by pedalling (the treadmill's rule). */
	(void)trainer_equip_event(&e, TRAINER_EVT_PAUSED);
	zassert_equal(e.run, TRAINER_RUN_PAUSED);
	(void)trainer_equip_report(&e, &r);
	zassert_equal(e.run, TRAINER_RUN_PAUSED, "pedalling does not resume a Pause");
	(void)trainer_equip_event(&e, TRAINER_EVT_STARTED);
	zassert_equal(e.run, TRAINER_RUN_RUNNING);

	(void)trainer_equip_event(&e, TRAINER_EVT_STOPPED);
	zassert_equal(e.run, TRAINER_RUN_STOPPED);
	(void)trainer_equip_report(&e, &r);
	zassert_equal(e.run, TRAINER_RUN_STOPPED, "pedalling does not undo a Stop");
	(void)trainer_equip_event(&e, TRAINER_EVT_STARTED);
	zassert_equal(e.run, TRAINER_RUN_RUNNING);

	/* A console reset returns to IDLE, from which pedalling starts. */
	(void)trainer_equip_event(&e, TRAINER_EVT_CONSOLE_STOP);
	zassert_equal(e.run, TRAINER_RUN_STOPPED);
	(void)trainer_equip_event(&e, TRAINER_EVT_CONSOLE_RESET);
	zassert_equal(e.run, TRAINER_RUN_IDLE);
	(void)trainer_equip_report(&e, &still);
	zassert_equal(e.run, TRAINER_RUN_IDLE);
	(void)trainer_equip_report(&e, &r);
	zassert_equal(e.run, TRAINER_RUN_RUNNING);
}

ZTEST(trainer_equip, test_only_local_stop_and_reset_move_the_epoch)
{
	struct trainer_equip e;
	struct trainer_report r = riding(200u, 90u, 9000u);
	uint32_t epoch;

	trainer_equip_init(&e, 2000u);
	(void)trainer_equip_report(&e, &r);
	epoch = e.epoch;

	(void)trainer_equip_event(&e, TRAINER_EVT_PAUSED);
	(void)trainer_equip_event(&e, TRAINER_EVT_STARTED);
	(void)trainer_equip_event(&e, TRAINER_EVT_STOPPED);
	zassert_equal(e.epoch, epoch, "state reports are not local stops");

	(void)trainer_equip_event(&e, TRAINER_EVT_STARTED);
	(void)trainer_equip_event(&e, TRAINER_EVT_CONSOLE_STOP);
	zassert_equal(e.epoch, epoch + 1u);
	zassert_equal(e.run, TRAINER_RUN_STOPPED);

	/* The equipment state's half of a console reset: the targets are the
	 * brake release's to change (trainer_cmd_release_brake()). */
	e.mode = TRAINER_EQUIP_ERG;
	e.target_power_w = 250u;
	(void)trainer_equip_event(&e, TRAINER_EVT_CONSOLE_RESET);
	zassert_equal(e.epoch, epoch + 2u);
	zassert_equal(e.run, TRAINER_RUN_IDLE);
	zassert_equal(e.mode, TRAINER_EQUIP_ERG, "not changed until the brake is released");

	/* FTMS Reset moves the epoch too (the treadmill's REMOTE_RESET). */
	(void)trainer_equip_report(&e, &r);
	zassert_equal(trainer_equip_ftms_reset(&e), TRAINER_RUN_RUNNING);
	zassert_equal(e.epoch, epoch + 3u);
	zassert_equal(e.run, TRAINER_RUN_IDLE);
}

ZTEST(trainer_equip, test_tick_accumulates_only_while_running)
{
	struct trainer_equip e;
	struct trainer_report r = riding(200u, 90u, 10000u); /* 10 m/s */

	trainer_equip_init(&e, 2000u);
	e.rep = r; /* a report, but the session is still IDLE */
	trainer_equip_tick(&e, 250u);
	zassert_equal(e.elapsed_ms, 0u);
	zassert_equal(e.distance_um, 0u);
	zassert_equal(e.event_count, 1u, "page 25 events count regardless");
	zassert_equal(e.acc_power_w, 200u);

	(void)trainer_equip_report(&e, &r);
	for (int i = 0; i < 40; i++) {
		trainer_equip_tick(&e, 250u);
	}
	zassert_equal(e.elapsed_ms, 10000u);
	zassert_equal(e.distance_um, 100000000ull, "100 m in 10 s at 10 m/s");
	zassert_equal(e.event_count, 41u);
	zassert_equal(e.acc_power_w, (uint16_t)(41u * 200u));
}

ZTEST(trainer_equip, test_pages_16_17_25_from_the_state)
{
	struct trainer_equip e;
	struct trainer_report r = riding(321u, 85u, 8333u);
	struct radiant_profile_fec_general g;
	struct radiant_profile_fec_settings s;
	struct radiant_profile_fec_trainer t;

	trainer_equip_init(&e, 2000u);
	(void)trainer_equip_report(&e, &r);
	e.sim.grade_centi_pct = 450;
	trainer_equip_tick(&e, 250u);
	trainer_equip_pages(&e, &g, &s, &t);

	zassert_equal(g.equipment_type, RADIANT_PROFILE_FEC_TYPE_TRAINER);
	zassert_equal(g.speed_mm_s, 8333u);
	zassert_true(g.speed_valid && g.speed_virtual && g.distance_valid);
	zassert_equal(g.elapsed_time_qs, 1u);
	zassert_equal(g.state, RADIANT_PROFILE_FEC_STATE_IN_USE);

	zassert_equal(s.incline_centi_pct, 450, "SS8.8.4.1: incline = grade");
	zassert_true(s.resistance_valid);
	zassert_equal(s.resistance_half_pct, 50u, "500 N of 2000 N = 25 % = 50");

	zassert_equal(t.inst_power_w, 321u);
	zassert_equal(t.cadence_rpm, 85u);
	zassert_equal(t.event_count, 1u);
	zassert_equal(t.flags, RADIANT_PROFILE_FEC_TARGET_POWER_OK);

	/* BASIC reports the commanded share; ERG reports the speed flag. */
	e.mode = TRAINER_EQUIP_BASIC;
	e.resistance_permille = 375u;
	e.rep.erg_speed_low = true;
	trainer_equip_pages(&e, &g, &s, &t);
	zassert_equal(s.resistance_half_pct, 75u);
	zassert_equal(t.flags, RADIANT_PROFILE_FEC_TARGET_POWER_OK, "flag is ERG-only");
	e.mode = TRAINER_EQUIP_ERG;
	trainer_equip_pages(&e, &g, &s, &t);
	zassert_equal(t.flags, RADIANT_PROFILE_FEC_TARGET_POWER_SPEED_LOW);

	/* No force and no maximum: page 17's resistance is invalid. */
	e.mode = TRAINER_EQUIP_FREE;
	e.max_resistance_n = 0u;
	trainer_equip_pages(&e, &g, &s, &t);
	zassert_false(s.resistance_valid);

	(void)trainer_equip_event(&e, TRAINER_EVT_PAUSED);
	trainer_equip_pages(&e, &g, &s, &t);
	zassert_equal(g.state, RADIANT_PROFILE_FEC_STATE_FINISHED);
}

ZTEST(trainer_equip, test_echo_pages_carry_the_settings_in_use)
{
	struct trainer_equip e;
	struct radiant_profile_fec_cmd_wind_resistance w;
	struct radiant_profile_fec_cmd_track_resistance tr;
	struct radiant_profile_fec_cmd_user_config u;

	trainer_equip_init(&e, 2000u);
	e.sim.cw_centi = 60u;
	e.sim.wind_mm_s = -2778; /* -10 km/h */
	e.sim.drafting_centi = 80u;
	e.sim.grade_centi_pct = -250;
	e.sim.crr_e5 = 500u; /* 0.005 */
	e.user.user_weight_10g = 8000u;
	e.user.bike_weight_50g = 180u;
	e.user.wheel_circ_deci_mm = 21991u; /* 700 mm diameter */
	trainer_equip_echo(&e, &w, &tr, &u);

	zassert_equal(w.coefficient_centi, 60u);
	zassert_equal(w.wind_speed_kph, -10);
	zassert_equal(w.drafting_centi, 80u);
	zassert_equal(tr.grade_centi_pct, -250);
	zassert_equal(tr.rolling_resistance, 100u, "0.005 in 5e-5 units");
	zassert_equal(u.user_weight_10g, 8000u);
	zassert_equal(u.bike_weight_50g, 180u);
	zassert_equal(u.wheel_diameter_cm, 70u);
	zassert_equal(u.wheel_offset_mm, 0u);
	zassert_false(u.gear_ratio_valid);
}

/* Parse an Indoor Bike Data record the way a client does: by the flags. */
ZTEST(trainer_equip, test_indoor_bike_data_layout_and_reset_baseline)
{
	struct trainer_equip e;
	struct trainer_report r = riding(250u, 92u, 10000u);
	uint8_t b[TRAINER_IBD_LEN];
	uint16_t flags;
	uint32_t dist;

	trainer_equip_init(&e, 2000u);
	(void)trainer_equip_report(&e, &r);
	for (int i = 0; i < 80; i++) {
		trainer_equip_tick(&e, 250u); /* 20 s, 200 m */
	}
	e.mode = TRAINER_EQUIP_BASIC;
	e.resistance_permille = 500u;

	zassert_equal(trainer_equip_ibd_encode(&e, b, sizeof(b)), (int)TRAINER_IBD_LEN);
	flags = (uint16_t)(b[0] | (b[1] << 8));
	zassert_equal(flags, 0x0874u, "speed, cadence, distance, resistance, power, elapsed");
	zassert_equal(b[2] | (b[3] << 8), 3600, "10 m/s = 36.00 km/h");
	zassert_equal(b[4] | (b[5] << 8), 184, "92 rpm in 0.5 units");
	dist = (uint32_t)(b[6] | (b[7] << 8) | (b[8] << 16));
	zassert_equal(dist, 200u);
	zassert_equal((int16_t)(b[9] | (b[10] << 8)), 10, "50 % = level 10.0, resolution 1");
	zassert_equal((int16_t)(b[11] | (b[12] << 8)), 250);
	zassert_equal(b[13] | (b[14] << 8), 20);

	/* FTMS Reset zeros the FTMS view only. */
	zassert_equal(trainer_equip_ftms_reset(&e), TRAINER_RUN_RUNNING);
	zassert_equal(e.run, TRAINER_RUN_IDLE);
	zassert_equal(trainer_equip_ibd_encode(&e, b, sizeof(b)), (int)TRAINER_IBD_LEN);
	zassert_equal(b[6] | (b[7] << 8) | (b[8] << 16), 0);
	zassert_equal(b[13] | (b[14] << 8), 0);
	zassert_equal(e.elapsed_ms, 20000u, "the ANT+ accumulators do not move");

	(void)trainer_equip_report(&e, &r);
	for (int i = 0; i < 8; i++) {
		trainer_equip_tick(&e, 250u);
	}
	zassert_equal(trainer_equip_ibd_encode(&e, b, sizeof(b)), (int)TRAINER_IBD_LEN);
	zassert_equal(b[13] | (b[14] << 8), 2);
	zassert_equal(b[6] | (b[7] << 8) | (b[8] << 16), 20);

	zassert_equal(trainer_equip_ibd_encode(&e, b, 10u), -ENOSPC);
	zassert_equal(trainer_equip_ibd_encode(NULL, b, sizeof(b)), -EINVAL);
}

ZTEST_SUITE(trainer_equip, NULL, NULL, NULL, NULL, NULL);
