/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Every motion path (treadmill_cmd.c), the FTMS core driven through the
 * treadmill's hooks, the real FE-C/FTMS arbiter, the capability derivation and
 * the Reset baseline.
 *
 * THE SAFETY CASES. One negative case per motion path with authorisation
 * denied - FTMS Start, Set Target Speed > 0, Set Target Inclination, Reset's
 * incline half, FE-C page 51, the grade provider, and the Set-Speed auto-start
 * - each asserting that the fake source saw NO motion call. A belt that is
 * RUNNING but not moving (after a remote stop; a console start at 0) refuses
 * remote speed and incline. Queued FTMS requests and FE-C pages are refused
 * after a local Stop and a safety-key event. Stop/Pause pass with every
 * authorisation condition failed. Each of the four doors refuses on its own.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_ble_ftms.h>
#include <radiant/radiant_ctrl_arbiter.h>
#include <radiant/radiant_profile_fec_tx.h>

#include "fake_source.h"
#include "treadmill_caps.h"
#include "treadmill_cmd.h"
#include "treadmill_ftms_data.h"
#include "treadmill_session.h"
#include "treadmill_state.h"

static struct treadmill_state st;
static struct treadmill_session ses;
static bool remote_ok;
static uint32_t now_ms;
static struct treadmill_motion_policy pol;

static bool fake_remote(void)
{
	return remote_ok;
}

/* The FE-C implicit claim, through the REAL arbiter, as main.c makes it. */
static bool real_fec_claim(void)
{
	return radiant_ctrl_claim(RADIANT_CTRL_ANT, false, now_ms) == 0;
}

static struct radiant_ble_ftms_caps ftms_caps = {
	.targets = RADIANT_BLE_FTMS_TSF_SPEED | RADIANT_BLE_FTMS_TSF_INCLINATION,
	.speed = { .min = 0, .max = 2000, .step = 10, .valid = true },
	.incline = { .min = -30, .max = 150, .step = 5, .valid = true },
};

static const struct radiant_ble_ftms_ops ops = {
	.set_target = treadmill_cmd_ftms_set_target,
	.workout = treadmill_cmd_ftms_workout,
	.reset = treadmill_cmd_ftms_reset,
};

static const struct radiant_ble_ftms_auth auth = { .check = treadmill_cmd_ftms_auth };

static void bind(bool incline_cmd, bool speed_cmd)
{
	struct treadmill_cmd_cfg cfg = {
		.state = &st, .session = &ses,
		.ctrl_owner = radiant_ctrl_owner, .fec_claim = real_fec_claim,
		.incline_cmd = incline_cmd, .speed_cmd = speed_cmd,
	};

	treadmill_cmd_bind(&cfg);
}

/* RUNNING with the belt reported moving. */
static void go_moving(void)
{
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_FE_IN_USE);
	treadmill_session_set_belt(&ses, 3000u);
	zassert_true(treadmill_session_moving(&ses), NULL);
}

static void cmd_reset(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_source_reset();
	remote_ok = true;
	now_ms = 1000u;
	pol.remote_motion_allowed = fake_remote;
	pol.remote_start = false;
	treadmill_state_init(&st);
	(void)treadmill_state_set_limits(&st, -300, 1500, 5555);
	treadmill_session_init(&ses, &pol);
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_KEY_IN);
	radiant_ctrl_init(30000u, RADIANT_CTRL_GATE_SIMULATION);
	/* An FTMS client holds control by default: the token for FTMS paths. */
	(void)radiant_ctrl_claim(RADIANT_CTRL_BLE, true, now_ms);
	bind(true, true);
	go_moving();
}

static enum radiant_ble_ftms_run run_of(enum treadmill_run r)
{
	static const enum radiant_ble_ftms_run map[] = {
		RADIANT_BLE_FTMS_RUN_IDLE, RADIANT_BLE_FTMS_RUN_RUNNING,
		RADIANT_BLE_FTMS_RUN_PAUSED, RADIANT_BLE_FTMS_RUN_STOPPED,
		RADIANT_BLE_FTMS_RUN_SAFETY_STOP,
	};

	return map[r];
}

/* One Control Point procedure from a client with relation `owner`, received
 * under `rx_epoch`, through the real core and the treadmill's hooks. */
static struct radiant_ble_ftms_cp_outcome cp_as(const uint8_t *data, uint16_t len,
						uint32_t rx_epoch,
						enum radiant_ble_ftms_owner owner)
{
	struct radiant_ble_ftms_cp_request req = { .data = data, .len = len, .epoch = rx_epoch };
	struct radiant_ble_ftms_session s = { .run = run_of(ses.run), .epoch = ses.epoch };
	struct radiant_ble_ftms_cp_decision d =
		radiant_ble_ftms_cp_decide(&req, &s, &ftms_caps, owner, &auth);

	return radiant_ble_ftms_cp_execute(&d, &ops);
}

static struct radiant_ble_ftms_cp_outcome cp_at(const uint8_t *data, uint16_t len,
						uint32_t rx_epoch)
{
	return cp_as(data, len, rx_epoch, RADIANT_BLE_FTMS_OWNER_SELF);
}

static struct radiant_ble_ftms_cp_outcome cp(const uint8_t *data, uint16_t len)
{
	return cp_at(data, len, ses.epoch);
}

static const uint8_t op_start[] = { 0x07 };
static const uint8_t op_stop[] = { 0x08, 0x01 };
static const uint8_t op_pause[] = { 0x08, 0x02 };
static const uint8_t op_reset[] = { 0x01 };
static const uint8_t op_speed_10kph[] = { 0x02, 0xE8, 0x03 };
static const uint8_t op_speed_0[] = { 0x02, 0x00, 0x00 };
static const uint8_t op_incline_5pct[] = { 0x03, 0x32, 0x00 };

/* ── The positive baseline ──────────────────────────────────────────────── */

ZTEST(treadmill_cmd, test_every_path_reaches_the_source_when_permitted)
{
	zassert_equal(cp(op_speed_10kph, 3).result, 0x01, NULL);
	zassert_equal(fake.speed_calls, 1, NULL);
	zassert_equal(fake.last_speed, 2778u, "10.00 km/h");
	zassert_equal(st.target_speed_mm_s, 2778u, NULL);

	zassert_equal(cp(op_incline_5pct, 3).result, 0x01, NULL);
	zassert_equal(fake.last_incline, 500, NULL);

	zassert_equal(treadmill_cmd_grade_provider(-5000), 0, NULL);
	zassert_equal(fake.last_incline, -300, "a simulation grade clamps");

	/* FE-C needs the token: the FTMS client gives it back first. */
	radiant_ctrl_release(RADIANT_CTRL_BLE);
	zassert_equal(treadmill_cmd_fec_track_resistance(250, ses.epoch),
		      RADIANT_PROFILE_FEC_CMD_STATUS_PASS, NULL);
	zassert_equal(fake.last_incline, 250, NULL);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_ANT, "the page claimed implicitly");
}

ZTEST(treadmill_cmd, test_resume_needs_remote_start)
{
	/* A resume restarts the belt from PAUSED: condition (c). */
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_PAUSED);
	zassert_equal(cp(op_start, 1).result, 0x05, NULL);
	zassert_equal(fake.resume_calls, 0, NULL);
	pol.remote_start = true;
	zassert_equal(cp(op_start, 1).result, 0x01, NULL);
	zassert_equal(fake.resume_calls, 1, "Start while paused resumes");
}

/* ── One negative case per motion path ──────────────────────────────────── */

ZTEST(treadmill_cmd, test_denied_ftms_start)
{
	/* (c) satisfied, so only (a) refuses. */
	pol.remote_start = true;
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_FE_READY);
	remote_ok = false;
	zassert_equal(cp(op_start, 1).result, 0x05, NULL);
	zassert_equal(fake_source_motion_calls(), 0, NULL);
}

ZTEST(treadmill_cmd, test_denied_speed_above_zero)
{
	remote_ok = false;
	zassert_equal(cp(op_speed_10kph, 3).result, 0x05, NULL);
	zassert_equal(fake.speed_calls, 0, NULL);
	zassert_equal(st.target_speed_mm_s, 0u, "a refused command leaves the target");
}

ZTEST(treadmill_cmd, test_denied_incline)
{
	remote_ok = false;
	zassert_equal(cp(op_incline_5pct, 3).result, 0x05, NULL);
	zassert_equal(fake.incline_calls, 0, NULL);
	zassert_equal(st.target_incline_centi_pct, 0, NULL);
}

ZTEST(treadmill_cmd, test_denied_reset_incline_still_stops_the_belt)
{
	struct radiant_ble_ftms_cp_outcome o;

	st.target_incline_centi_pct = 400; /* a deck to level */
	remote_ok = false;
	o = cp(op_reset, 1);
	zassert_equal(o.result, 0x04, "a Reset whose incline half is denied answers 0x04");
	zassert_equal(fake.incline_calls, 0, "the deck did not move");
	zassert_equal(fake.speed_calls, 1, "the belt was stopped");
	zassert_equal(fake.last_speed, 0u, NULL);
	zassert_false(o.release, NULL);
	zassert_false(o.reset_view, NULL);
}

ZTEST(treadmill_cmd, test_reset_with_no_incline_half_succeeds)
{
	struct radiant_ble_ftms_cp_outcome o;

	/* The deck is already level: there is no incline half to deny. */
	remote_ok = false;
	o = cp(op_reset, 1);
	zassert_equal(o.result, 0x01, NULL);
	zassert_true(o.release, NULL);
	zassert_equal(fake.incline_calls, 0, NULL);

	/* No incline capability: the same. */
	fake_source_reset();
	bind(false, true);
	st.target_incline_centi_pct = 400;
	o = cp(op_reset, 1);
	zassert_equal(o.result, 0x01, NULL);
	zassert_equal(fake.incline_calls, 0, NULL);
	zassert_equal(fake.speed_calls, 1, NULL);
}

ZTEST(treadmill_cmd, test_denied_fec_page_51)
{
	radiant_ctrl_release(RADIANT_CTRL_BLE);
	remote_ok = false;
	zassert_equal(treadmill_cmd_fec_track_resistance(400, ses.epoch),
		      RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED, NULL);
	zassert_equal(fake.incline_calls, 0, NULL);
	zassert_equal(st.target_incline_centi_pct, 0, NULL);
}

ZTEST(treadmill_cmd, test_denied_grade_provider_value_is_dropped)
{
	remote_ok = false;
	zassert_equal(treadmill_cmd_grade_provider(600), -EACCES, NULL);
	zassert_equal(fake.incline_calls, 0, NULL);
	zassert_equal(st.target_incline_centi_pct, 0, NULL);

	/* In SAFETY_STOP with everything else fine. */
	remote_ok = true;
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_KEY_OUT);
	zassert_equal(treadmill_cmd_grade_provider(600), -EACCES, NULL);
	zassert_equal(fake.incline_calls, 0, NULL);
}

ZTEST(treadmill_cmd, test_set_speed_auto_start_needs_remote_start)
{
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_FE_READY);
	treadmill_session_set_belt(&ses, 0u);
	zassert_equal(ses.run, TREADMILL_RUN_IDLE, NULL);

	zassert_equal(cp(op_speed_10kph, 3).result, 0x05,
		      "Set Speed while Idle does not start the belt by default");
	zassert_equal(fake.speed_calls, 0, NULL);
	zassert_equal(cp(op_start, 1).result, 0x05, NULL);
	zassert_equal(fake.start_calls, 0, NULL);

	pol.remote_start = true;
	zassert_equal(cp(op_speed_10kph, 3).result, 0x01, NULL);
	zassert_equal(fake.speed_calls, 1, NULL);
}

/* ── Moving is not RUNNING (S1) and the deck at rest (S2) ───────────────── */

ZTEST(treadmill_cmd, test_remote_stop_then_speed_is_refused)
{
	/* Remote Stop accepted; the source has not yet reported FINISHED, so
	 * the session is still RUNNING and the belt still reports speed. */
	zassert_equal(cp(op_stop, 2).result, 0x01, NULL);
	zassert_equal(ses.run, TREADMILL_RUN_RUNNING, NULL);
	zassert_equal(cp(op_speed_10kph, 3).result, 0x05,
		      "no restart from rest without REMOTE_START");
	zassert_equal(fake.speed_calls, 0, NULL);

	/* The same through Set Target Speed 0. */
	cmd_reset(NULL);
	zassert_equal(cp(op_speed_0, 3).result, 0x01, NULL);
	zassert_equal(cp(op_speed_10kph, 3).result, 0x05, NULL);
	zassert_equal(fake.speed_calls, 1, "only the speed-0 call reached the source");
}

ZTEST(treadmill_cmd, test_console_start_at_zero_then_remote_speed_is_refused)
{
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_FE_READY);
	treadmill_session_set_belt(&ses, 0u);
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_CONSOLE_START);
	zassert_equal(ses.run, TREADMILL_RUN_RUNNING, NULL);
	zassert_equal(cp(op_speed_10kph, 3).result, 0x05, NULL);
	zassert_equal(fake.speed_calls, 0, NULL);
}

ZTEST(treadmill_cmd, test_no_remote_incline_after_a_console_stop)
{
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_CONSOLE_STOP);
	treadmill_session_set_belt(&ses, 0u);

	/* A FRESH page (current epoch) and the grade provider: both refused. */
	radiant_ctrl_release(RADIANT_CTRL_BLE);
	zassert_equal(treadmill_cmd_fec_track_resistance(300, ses.epoch),
		      RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED, NULL);
	(void)radiant_ctrl_claim(RADIANT_CTRL_BLE, true, now_ms);
	zassert_equal(treadmill_cmd_grade_provider(300), -EACCES, NULL);
	zassert_equal(cp(op_incline_5pct, 3).result, 0x05, NULL);
	zassert_equal(fake.incline_calls, 0, NULL);
	zassert_equal(st.target_incline_centi_pct, 0, NULL);

	/* With REMOTE_START the machine allows it. */
	pol.remote_start = true;
	zassert_equal(treadmill_cmd_grade_provider(300), 0, NULL);
}

/* ── The four doors refuse on their own ─────────────────────────────────── */

ZTEST(treadmill_cmd, test_each_door_gates_by_itself)
{
	remote_ok = false;
	zassert_equal(treadmill_cmd_drive_speed(TREADMILL_MOTION_SPEED, 2000u, ses.epoch, true),
		      -EACCES, NULL);
	zassert_equal(treadmill_cmd_drive_incline(TREADMILL_MOTION_INCLINE, 200, ses.epoch, true),
		      -EACCES, NULL);
	zassert_equal(treadmill_cmd_drive_start(false, ses.epoch, true), -EACCES, NULL);
	zassert_equal(treadmill_cmd_drive_start(true, ses.epoch, true), -EACCES, NULL);
	zassert_equal(fake_source_motion_calls(), 0, NULL);

	/* A stale epoch at a door, everything else fine. */
	remote_ok = true;
	zassert_equal(treadmill_cmd_drive_speed(TREADMILL_MOTION_SPEED, 2000u, ses.epoch + 1u,
						true), -ECANCELED, NULL);
	zassert_equal(treadmill_cmd_drive_incline(TREADMILL_MOTION_SIM_GRADE, 200,
						  ses.epoch + 1u, true), -ECANCELED, NULL);
	zassert_equal(fake_source_motion_calls(), 0, NULL);

	/* The stop door opens in the worst state there is. */
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_KEY_OUT);
	remote_ok = false;
	zassert_equal(treadmill_cmd_drive_stop(false), 0, NULL);
	zassert_equal(treadmill_cmd_drive_stop(true), 0, NULL);
	zassert_equal(fake.stop_calls, 1, NULL);
	zassert_equal(fake.pause_calls, 1, NULL);
}

ZTEST(treadmill_cmd, test_ftms_hooks_use_the_receipt_epoch)
{
	struct radiant_ble_ftms_target speed = { .op = 0x02, .v.speed_centi_kph = 1000 };
	uint32_t rx = ses.epoch;

	/* A hook called with an epoch the session has left is refused at its
	 * door, even though the core's decision would have been made earlier. */
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_CONSOLE_STOP);
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_CONSOLE_START);
	zassert_equal(treadmill_cmd_ftms_set_target(&speed, rx, NULL), -ECANCELED, NULL);
	zassert_equal(treadmill_cmd_ftms_workout(RADIANT_BLE_FTMS_WORKOUT_START, rx, NULL),
		      -ECANCELED, NULL);
	zassert_equal(fake_source_motion_calls(), 0, NULL);
}

/* ── Tokens ─────────────────────────────────────────────────────────────── */

ZTEST(treadmill_cmd, test_ftms_and_grade_tokens_need_a_ble_owner)
{
	zassert_true(treadmill_cmd_ftms_token(), "BLE holds it");
	zassert_true(treadmill_cmd_grade_token(), NULL);

	radiant_ctrl_release(RADIANT_CTRL_BLE);
	zassert_false(treadmill_cmd_ftms_token(), "nobody holds it");
	zassert_false(treadmill_cmd_grade_token(), "the grade provider is no weaker");
	zassert_equal(treadmill_cmd_grade_provider(300), -EACCES, NULL);

	(void)radiant_ctrl_claim(RADIANT_CTRL_ANT, false, now_ms);
	zassert_false(treadmill_cmd_ftms_token(), NULL);
	zassert_false(treadmill_cmd_grade_token(), NULL);
	zassert_equal(cp(op_speed_10kph, 3).result, 0x05, NULL);
	zassert_equal(fake_source_motion_calls(), 0, NULL);

	/* Without arbitration (POLICY_NONE) every token holds. */
	{
		struct treadmill_cmd_cfg cfg = {
			.state = &st, .session = &ses, .incline_cmd = true, .speed_cmd = true,
		};

		treadmill_cmd_bind(&cfg);
		zassert_true(treadmill_cmd_ftms_token(), NULL);
		zassert_true(treadmill_cmd_grade_token(), NULL);
	}
}

ZTEST(treadmill_cmd, test_fec_claim_goes_through_the_real_arbiter)
{
	/* An FTMS client holds control (explicit): the head unit is refused
	 * and does not take the token. */
	zassert_equal(treadmill_cmd_fec_track_resistance(300, ses.epoch),
		      RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED, NULL);
	zassert_equal(treadmill_cmd_fec_wind(ses.epoch), RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED,
		      NULL);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_BLE, NULL);
	zassert_equal(fake.incline_calls, 0, NULL);

	radiant_ctrl_release(RADIANT_CTRL_BLE);
	zassert_equal(treadmill_cmd_fec_track_resistance(300, ses.epoch),
		      RADIANT_PROFILE_FEC_CMD_STATUS_PASS, NULL);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_ANT, NULL);
}

ZTEST(treadmill_cmd, test_a_stale_page_cannot_reclaim_after_a_key_pull)
{
	uint32_t rx;

	radiant_ctrl_release(RADIANT_CTRL_BLE);
	rx = ses.epoch;
	/* Key out: the session revokes (main.c calls radiant_ctrl_revoke()). */
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_KEY_OUT);
	(void)radiant_ctrl_revoke();

	zassert_equal(treadmill_cmd_fec_track_resistance(300, rx),
		      RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED, NULL);
	zassert_equal(treadmill_cmd_fec_wind(rx), RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED, NULL);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_NONE,
		      "the stale page did not take the token back");
}

ZTEST(treadmill_cmd, test_without_simulation_pages_50_51_are_not_supported)
{
	bind(false, true);
	radiant_ctrl_init(30000u, 0u); /* ungated, as main.c does then */
	zassert_equal(treadmill_cmd_fec_track_resistance(200, ses.epoch),
		      RADIANT_PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED, NULL);
	zassert_equal(treadmill_cmd_fec_wind(ses.epoch),
		      RADIANT_PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED, NULL);
	zassert_false(radiant_ctrl_fec_page_is_gated(0x33u), NULL);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_NONE, "no claim was made");
}

ZTEST(treadmill_cmd, test_stop_from_a_non_owner_is_refused)
{
	/* §4.16.2: Stop is a procedure and needs control (S4). The console
	 * Stop and the safety key are always available locally. */
	zassert_equal(cp_as(op_stop, 2, ses.epoch, RADIANT_BLE_FTMS_OWNER_NONE).result, 0x05,
		      NULL);
	zassert_equal(cp_as(op_stop, 2, ses.epoch, RADIANT_BLE_FTMS_OWNER_OTHER).result, 0x05,
		      NULL);
	zassert_equal(cp_as(op_speed_0, 3, ses.epoch, RADIANT_BLE_FTMS_OWNER_NONE).result, 0x05,
		      NULL);
	zassert_equal(fake.stop_calls + fake.speed_calls, 0, NULL);
}

/* ── Queued requests cannot outlive a local stop ────────────────────────── */

ZTEST(treadmill_cmd, test_queued_ftms_request_refused_after_a_local_stop)
{
	uint32_t rx = ses.epoch; /* received... */

	(void)treadmill_session_input(&ses, TREADMILL_SESSION_CONSOLE_STOP);
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_CONSOLE_START);
	/* ...dequeued after a Stop and a restart: everything else passes. */
	zassert_true(treadmill_session_moving(&ses), NULL);
	zassert_equal(cp_at(op_speed_10kph, 3, rx).result, 0x04, NULL);
	zassert_equal(cp_at(op_incline_5pct, 3, rx).result, 0x04, NULL);
	zassert_equal(fake_source_motion_calls(), 0, NULL);

	/* A queued Stop still lands. */
	zassert_equal(cp_at(op_stop, 2, rx).result, 0x01, NULL);
	zassert_equal(fake.stop_calls, 1, NULL);
}

ZTEST(treadmill_cmd, test_queued_ftms_request_refused_after_a_safety_key_event)
{
	uint32_t rx = ses.epoch;

	(void)treadmill_session_input(&ses, TREADMILL_SESSION_KEY_OUT);
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_KEY_IN);
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_CONSOLE_START);
	zassert_equal(ses.run, TREADMILL_RUN_RUNNING, "latch cleared locally");
	zassert_equal(cp_at(op_speed_10kph, 3, rx).result, 0x04, NULL);
	zassert_equal(fake_source_motion_calls(), 0, NULL);
}

ZTEST(treadmill_cmd, test_queued_fec_page_51_refused_after_a_local_stop)
{
	uint32_t rx;

	radiant_ctrl_release(RADIANT_CTRL_BLE);
	rx = ses.epoch;
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_CONSOLE_STOP);
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_CONSOLE_START);
	zassert_equal(treadmill_cmd_fec_track_resistance(300, rx),
		      RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED, NULL);
	zassert_equal(treadmill_cmd_fec_wind(rx), RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED,
		      "page 50 goes with page 51");
	zassert_equal(fake.incline_calls, 0, NULL);
	zassert_equal(st.target_incline_centi_pct, 0, NULL);

	zassert_equal(treadmill_cmd_fec_track_resistance(300, ses.epoch),
		      RADIANT_PROFILE_FEC_CMD_STATUS_PASS, "a fresh page is applied");
}

ZTEST(treadmill_cmd, test_queued_fec_page_51_refused_after_a_safety_key_event)
{
	uint32_t rx;

	radiant_ctrl_release(RADIANT_CTRL_BLE);
	rx = ses.epoch;
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_KEY_OUT);
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_KEY_IN);
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_CONSOLE_START);
	zassert_equal(treadmill_cmd_fec_track_resistance(300, rx),
		      RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED, NULL);
	zassert_equal(fake.incline_calls, 0, NULL);
}

/* ── Stopping always passes authorisation ───────────────────────────────── */

ZTEST(treadmill_cmd, test_stop_and_pause_always_pass)
{
	uint32_t rx = ses.epoch;

	/* Every authorisation condition failed at once: latched, key out,
	 * remote disabled, no token, and a stale epoch on top. (FTMS control
	 * itself is still needed, S4.) */
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_KEY_OUT);
	(void)radiant_ctrl_revoke();
	remote_ok = false;

	zassert_equal(cp_at(op_stop, 2, rx).result, 0x01, NULL);
	zassert_equal(fake.stop_calls, 1, NULL);
	zassert_equal(cp_at(op_pause, 2, rx).result, 0x01, NULL);
	zassert_equal(fake.pause_calls, 1, NULL);
	zassert_equal(cp_at(op_speed_0, 3, rx).result, 0x01, "speed 0 is a stop");
	zassert_equal(fake.speed_calls, 1, NULL);
	zassert_equal(fake.last_speed, 0u, NULL);

	/* Reset in the same state with a deck to level: the belt half runs,
	 * the answer is 0x04. */
	st.target_incline_centi_pct = 400;
	zassert_equal(cp_at(op_reset, 1, rx).result, 0x04, NULL);
	zassert_equal(fake.speed_calls, 2, NULL);
	zassert_equal(fake.incline_calls, 0, NULL);
	zassert_equal(fake_source_motion_calls(), fake.speed_calls,
		      "nothing but speed-0 calls reached the source");
}

/* ── Reset ──────────────────────────────────────────────────────────────── */

ZTEST(treadmill_cmd, test_reset_success_stops_the_belt_first)
{
	struct radiant_ble_ftms_cp_outcome o;

	st.target_incline_centi_pct = 400;
	o = cp(op_reset, 1);
	zassert_equal(o.result, 0x01, NULL);
	zassert_true(o.release, NULL);
	zassert_true(o.reset_view, NULL);
	zassert_equal(fake.first, 2, "speed first, then incline");
	zassert_equal(fake.incline_calls, 1, NULL);
	zassert_equal(fake.last_incline, 0, NULL);
	zassert_equal(fake.speed_calls, 1, NULL);
	zassert_equal(st.target_incline_centi_pct, 0, NULL);
	zassert_false(treadmill_session_moving(&ses), "the stop counts at once");
}

ZTEST(treadmill_cmd, test_reset_with_a_refusing_actuator)
{
	struct radiant_ble_ftms_cp_outcome o;

	st.target_incline_centi_pct = 400;
	fake.incline_rc = -EIO;
	o = cp(op_reset, 1);
	zassert_equal(o.result, 0x04, NULL);
	zassert_false(o.release, NULL);
	zassert_equal(fake.speed_calls, 1, "the belt was stopped regardless");

	cmd_reset(NULL);
	st.target_incline_centi_pct = 400;
	fake.speed_rc = -EIO;
	o = cp(op_reset, 1);
	zassert_equal(o.result, 0x04, NULL);
	zassert_false(o.release, NULL);

	/* -ENOTSUP is "nothing to move": not a failure. */
	cmd_reset(NULL);
	st.target_incline_centi_pct = 400;
	fake.incline_rc = -ENOTSUP;
	fake.speed_rc = -ENOTSUP;
	zassert_equal(cp(op_reset, 1).result, 0x01, NULL);
}

ZTEST(treadmill_cmd, test_success_is_acceptance_not_state)
{
	/* A source that accepts Start and never starts: success, and the
	 * session (driven only by reports) stays where it was. */
	pol.remote_start = true;
	(void)treadmill_session_input(&ses, TREADMILL_SESSION_FE_READY);
	zassert_equal(cp(op_start, 1).result, 0x01, NULL);
	zassert_equal(fake.start_calls, 1, NULL);
	zassert_equal(ses.run, TREADMILL_RUN_IDLE, NULL);

	fake.start_rc = -EIO;
	zassert_equal(cp(op_start, 1).result, 0x04, NULL);
	fake.start_rc = -ENOTSUP;
	zassert_equal(cp(op_start, 1).result, 0x04,
		      "Start is mandatory: a missing hook is a failure, not 0x02");
}

/* ── Capabilities ───────────────────────────────────────────────────────── */

ZTEST(treadmill_cmd, test_capabilities_follow_the_source)
{
	struct treadmill_source_caps src = { .set_speed = true, .set_incline = true };
	struct treadmill_caps_cfg cfg = { .allow_speed = true, .allow_incline = true };
	struct treadmill_caps c;

	treadmill_caps_derive(&src, &cfg, &c);
	zassert_equal(c.ftms_targets,
		      RADIANT_BLE_FTMS_TSF_SPEED | RADIANT_BLE_FTMS_TSF_INCLINATION, NULL);
	zassert_true(c.fec_simulation, NULL);
	zassert_equal(c.ftms_features & RADIANT_BLE_FTMS_FMF_HEART_RATE, 0u,
		      "no HR bit without an ANT+ strap receiver");

	/* A fixed deck: no incline anywhere. */
	src.set_incline = false;
	treadmill_caps_derive(&src, &cfg, &c);
	zassert_equal(c.ftms_targets, RADIANT_BLE_FTMS_TSF_SPEED, NULL);
	zassert_false(c.fec_simulation, NULL);
	zassert_false(c.incline_cmd, NULL);

	/* Kconfig takes a capability away, never adds one. */
	src.set_incline = true;
	cfg.allow_speed = false;
	treadmill_caps_derive(&src, &cfg, &c);
	zassert_equal(c.ftms_targets, RADIANT_BLE_FTMS_TSF_INCLINATION, NULL);
	src.set_speed = false;
	cfg.allow_speed = true;
	treadmill_caps_derive(&src, &cfg, &c);
	zassert_false(c.speed_cmd, NULL);

	cfg.heart_rate = true;
	treadmill_caps_derive(&src, &cfg, &c);
	zassert_not_equal(c.ftms_features & RADIANT_BLE_FTMS_FMF_HEART_RATE, 0u, NULL);
}

ZTEST(treadmill_cmd, test_undeclared_capabilities_are_not_supported)
{
	struct radiant_ble_ftms_target incline = { .op = 0x03, .v.incline_deci = 10 };

	bind(false, true);
	zassert_equal(treadmill_cmd_grade_provider(200), -ENOTSUP, NULL);
	zassert_equal(treadmill_cmd_ftms_set_target(&incline, ses.epoch, NULL), -ENOTSUP, NULL);
	zassert_equal(fake.incline_calls, 0, NULL);
}

/* ── Reset baselines ────────────────────────────────────────────────────── */

ZTEST(treadmill_cmd, test_reset_zeroes_the_ftms_view_and_not_ant)
{
	struct treadmill_ftms_baseline b = { 0 };
	uint8_t rec[TREADMILL_FTMS_DATA_MAX];
	int len;

	treadmill_state_set_state(&st, TREADMILL_STATE_IN_USE);
	treadmill_state_set_speed(&st, 3000u);
	treadmill_state_set_incline(&st, 200);
	for (int i = 0; i < 600; i++) {
		treadmill_state_tick(&st, 100u); /* a minute */
	}
	len = treadmill_ftms_data_encode(&st, &b, false, rec, sizeof(rec));
	zassert_equal(len, 17, NULL);
	zassert_equal(rec[4] | (rec[5] << 8) | (rec[6] << 16), 180, "180 m before Reset");
	zassert_equal(rec[15] | (rec[16] << 8), 60, "60 s before Reset");

	/* FTMS Reset. */
	treadmill_ftms_baseline_capture(&b, &st);
	(void)treadmill_ftms_data_encode(&st, &b, false, rec, sizeof(rec));
	zassert_equal(rec[4] | rec[5] | rec[6], 0, "distance zeroed");
	zassert_equal(rec[11] | rec[12], 0, "elevation zeroed");
	zassert_equal(rec[15] | rec[16], 0, "elapsed time zeroed");
	zassert_equal(st.elapsed_ms, 60000u, "the ANT+ accumulators did not move");
	zassert_equal(st.distance_mm / 1000u, 180u, NULL);

	for (int i = 0; i < 100; i++) {
		treadmill_state_tick(&st, 100u);
	}
	(void)treadmill_ftms_data_encode(&st, &b, false, rec, sizeof(rec));
	zassert_equal(rec[4], 30, "30 m since Reset");
	zassert_equal(rec[15], 10, "10 s since Reset");

	/* The session restarted underneath: re-base to zero, no wrap. */
	treadmill_state_init(&st);
	(void)treadmill_ftms_data_encode(&st, &b, false, rec, sizeof(rec));
	zassert_equal(rec[4] | rec[5] | rec[6], 0, NULL);
	zassert_equal(rec[15] | rec[16], 0, NULL);
	zassert_equal(b.distance_mm, 0u, NULL);
}

ZTEST(treadmill_cmd, test_heart_rate_field_follows_the_capability)
{
	struct treadmill_ftms_baseline b = { 0 };
	uint8_t rec[TREADMILL_FTMS_DATA_MAX];

	treadmill_state_set_heart_rate(&st, 150u, true);
	zassert_equal(treadmill_ftms_data_encode(&st, &b, true, rec, sizeof(rec)), 18, NULL);
	zassert_not_equal(rec[1] & 0x01, 0, "flag bit 8");
	zassert_equal(rec[15], 150, NULL);
	zassert_equal(treadmill_ftms_data_encode(&st, &b, false, rec, sizeof(rec)), 17, NULL);
	zassert_equal(rec[1] & 0x01, 0, NULL);
	zassert_equal(rec[0] & 0x01, 0, "bit 0 clear: speed present");
	zassert_equal(treadmill_ftms_data_encode(&st, &b, true, rec, 17), -ENOMEM, NULL);
}

/* ── The lap event (P7) ─────────────────────────────────────────────────── */

ZTEST(treadmill_cmd, test_derived_finished_is_not_a_lap)
{
	bool lap = st.lap_toggle;

	/* PAUSED / SAFETY_STOP read FINISHED on page 16 but are not laps. */
	treadmill_state_set_derived_state(&st, TREADMILL_STATE_FINISHED);
	zassert_equal(st.lap_toggle, lap, NULL);
	zassert_equal(st.state, TREADMILL_STATE_FINISHED, NULL);
	treadmill_state_lap(&st);
	zassert_not_equal(st.lap_toggle, lap, "the source's own FINISHED is");
}

/* ── Round 3 review cases ───────────────────────────────────────────────── */

ZTEST(treadmill_cmd, test_a_refused_stop_still_ends_moving)
{
	/* The driver refuses the Stop: answered 0x04, but the belt no longer
	 * counts as moving, so nothing can speed it up or tilt it remotely. */
	fake.stop_rc = -EIO;
	zassert_equal(cp(op_stop, 2).result, 0x04, NULL);
	zassert_false(treadmill_session_moving(&ses), NULL);
	zassert_equal(cp(op_speed_10kph, 3).result, 0x05, NULL);
	radiant_ctrl_release(RADIANT_CTRL_BLE);
	zassert_equal(treadmill_cmd_fec_track_resistance(300, ses.epoch),
		      RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED, NULL);
	zassert_equal(fake.speed_calls + fake.incline_calls, 0, NULL);

	/* The same for a refused Pause and a refused Set Target Speed 0. */
	cmd_reset(NULL);
	fake.pause_rc = -EIO;
	(void)cp(op_pause, 2);
	zassert_false(treadmill_session_moving(&ses), NULL);
	cmd_reset(NULL);
	fake.speed_rc = -EIO;
	(void)cp(op_speed_0, 3);
	zassert_false(treadmill_session_moving(&ses), NULL);
}

ZTEST(treadmill_cmd, test_reset_while_latched_stops_and_answers_0x04)
{
	struct radiant_ble_ftms_cp_outcome o;

	(void)treadmill_session_input(&ses, TREADMILL_SESSION_KEY_OUT);
	o = cp(op_reset, 1);
	zassert_equal(o.result, 0x04, NULL);
	zassert_equal(fake.speed_calls, 1, "the belt-stop half still ran");
	zassert_equal(fake.last_speed, 0u, NULL);
	zassert_false(o.reset_view, "no Training Status Idle while latched");
	zassert_false(o.release, NULL);
	zassert_equal(ses.run, TREADMILL_RUN_SAFETY_STOP, NULL);
}

ZTEST(treadmill_cmd, test_a_refused_page_does_not_take_the_token)
{
	radiant_ctrl_release(RADIANT_CTRL_BLE);
	remote_ok = false;
	zassert_equal(treadmill_cmd_fec_track_resistance(300, ses.epoch),
		      RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED, NULL);
	zassert_equal(treadmill_cmd_fec_wind(ses.epoch), RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED,
		      NULL);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_NONE,
		      "a refused page must not lock FTMS and the grade provider out");
}

ZTEST_SUITE(treadmill_cmd, NULL, NULL, cmd_reset, NULL, NULL);
