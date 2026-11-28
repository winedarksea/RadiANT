/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_cmd.c - every motion path, each behind treadmill_motion_permitted().
 * See treadmill_cmd.h. Kernel-free.
 *
 * THE INVARIANT THIS FILE EXISTS FOR: every call into treadmill_source_ctrl.h
 * is inside drive_speed(), drive_incline(), drive_start() or drive_stop(), and
 * each of those calls gate() on its first line and returns before the source
 * if gate() refuses. A new path adds a call to one of these, never a new call
 * into the source.
 */

#include <errno.h>
#include <stddef.h>

#include <radiant/radiant_profile_fec_tx.h>

#include "treadmill_cmd.h"
#include "treadmill_source_ctrl.h"

static struct treadmill_cmd_cfg cfg;

void treadmill_cmd_bind(const struct treadmill_cmd_cfg *c)
{
	cfg = *c;
}

/* -ENOTSUP from a source is "nothing to move here", not a refusal: a Stop or
 * Reset on a machine with a fixed deck or a self-paced belt succeeds. */
static int accepted_or_rc(int rc)
{
	return (rc == -ENOTSUP) ? 0 : rc;
}

static int gate(enum treadmill_motion_kind kind, int32_t value, uint32_t epoch, bool token)
{
	struct treadmill_motion_cmd c = {
		.kind = kind, .value = value, .epoch = epoch, .token = token,
	};

	return treadmill_motion_permitted(&c, cfg.session);
}

/* ── The four doors to the source ───────────────────────────────────────── */

static int drive_speed(enum treadmill_motion_kind kind, uint32_t mm_s, uint32_t epoch,
		       bool token)
{
	int rc = gate(kind, (int32_t)mm_s, epoch, token);

	if (rc != 0) {
		return rc;
	}
	return treadmill_source_set_speed(mm_s);
}

static int drive_incline(enum treadmill_motion_kind kind, int32_t centi, uint32_t epoch,
			 bool token)
{
	int rc = gate(kind, centi, epoch, token);

	if (rc != 0) {
		return rc;
	}
	return treadmill_source_set_incline(centi);
}

static int drive_start(bool resume, uint32_t epoch, bool token)
{
	int rc = gate(resume ? TREADMILL_MOTION_RESUME : TREADMILL_MOTION_START, 0, epoch,
		      token);

	if (rc != 0) {
		return rc;
	}
	return resume ? treadmill_source_workout_resume() : treadmill_source_workout_start();
}

/* Stop and Pause: the gate always answers 0 for a STOP; it is called anyway
 * so that this door looks like the other three. */
static int drive_stop(bool pause)
{
	int rc = gate(TREADMILL_MOTION_STOP, 0, cfg.session->epoch, false);

	if (rc != 0) {
		return rc;
	}
	return pause ? treadmill_source_workout_pause() : treadmill_source_workout_stop();
}

/* ── FE-C ───────────────────────────────────────────────────────────────── */

static uint8_t fec_status_for(int rc)
{
	if (rc == 0) {
		return RADIANT_PROFILE_FEC_CMD_STATUS_PASS;
	}
	if (rc == -ENOTSUP) {
		return RADIANT_PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED;
	}
	if (rc == -EACCES || rc == -ECANCELED) {
		return RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED;
	}
	return RADIANT_PROFILE_FEC_CMD_STATUS_FAIL;
}

uint8_t treadmill_cmd_fec_track_resistance(int32_t grade_centi, uint32_t rx_epoch,
					   bool token)
{
	struct treadmill_motion_cmd probe = {
		.kind = TREADMILL_MOTION_SIM_GRADE, .value = grade_centi,
		.epoch = rx_epoch, .token = token,
	};
	int32_t adopted;
	int rc;

	if (!cfg.incline_cmd) {
		/* Page 54 does not declare Simulation mode: the answer the
		 * declaration implies. */
		return RADIANT_PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED;
	}
	/* Authorise BEFORE the target moves: a refused page must leave the
	 * target where it was. drive_incline() asks again. */
	rc = treadmill_motion_permitted(&probe, cfg.session);
	if (rc != 0) {
		return fec_status_for(rc);
	}
	/* A simulation grade: CLAMPED to the machine range and answered PASS -
	 * the road may be steeper than the deck. */
	adopted = treadmill_state_request_incline(cfg.state, grade_centi);
	rc = drive_incline(TREADMILL_MOTION_SIM_GRADE, adopted, rx_epoch, token);
	return fec_status_for(rc);
}

uint8_t treadmill_cmd_fec_wind(uint32_t rx_epoch, bool token)
{
	if (!token || rx_epoch != cfg.session->epoch) {
		return RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED;
	}
	return RADIANT_PROFILE_FEC_CMD_STATUS_PASS;
}

/* ── The grade provider ─────────────────────────────────────────────────── */

int treadmill_cmd_grade_provider(int32_t grade_centi, bool token)
{
	struct treadmill_motion_cmd probe = {
		.kind = TREADMILL_MOTION_SIM_GRADE, .value = grade_centi,
		.epoch = cfg.session->epoch, .token = token,
	};
	int32_t adopted;
	int rc;

	if (!cfg.incline_cmd) {
		return -ENOTSUP;
	}
	rc = treadmill_motion_permitted(&probe, cfg.session);
	if (rc != 0) {
		return rc; /* dropped, target untouched */
	}
	adopted = treadmill_state_request_incline(cfg.state, grade_centi);
	return drive_incline(TREADMILL_MOTION_SIM_GRADE, adopted, cfg.session->epoch, token);
}

/* ── FTMS ───────────────────────────────────────────────────────────────── */

static bool ftms_token(void)
{
	return (cfg.ftms_token == NULL) || cfg.ftms_token();
}

int treadmill_cmd_ftms_set_target(const struct radiant_ble_ftms_target *t, void *ctx)
{
	uint32_t epoch = cfg.session->epoch;
	int rc;

	(void)ctx;

	switch (t->op) {
	case RADIANT_BLE_FTMS_OP_SET_SPEED: {
		uint32_t mm_s = treadmill_mm_s_from_ftms_speed(t->v.speed_centi_kph);
		uint32_t adopted = 0u;

		if (!cfg.speed_cmd) {
			return -ENOTSUP;
		}
		rc = gate(TREADMILL_MOTION_SPEED, (int32_t)mm_s, epoch, ftms_token());
		if (rc != 0) {
			return rc;
		}
		/* EXPLICIT: refused out of range, target unchanged. */
		rc = treadmill_state_command_speed(cfg.state, mm_s, &adopted);
		if (rc != 0) {
			return rc;
		}
		return drive_speed(TREADMILL_MOTION_SPEED, adopted, epoch, ftms_token());
	}
	case RADIANT_BLE_FTMS_OP_SET_INCLINE: {
		int32_t centi = treadmill_fec_incline_from_ftms(t->v.incline_deci);
		int32_t adopted = 0;

		if (!cfg.incline_cmd) {
			return -ENOTSUP;
		}
		rc = gate(TREADMILL_MOTION_INCLINE, centi, epoch, ftms_token());
		if (rc != 0) {
			return rc;
		}
		rc = treadmill_state_command_incline(cfg.state, centi, &adopted);
		if (rc != 0) {
			return rc;
		}
		return drive_incline(TREADMILL_MOTION_INCLINE, adopted, epoch, ftms_token());
	}
	default:
		return -ENOTSUP;
	}
}

int treadmill_cmd_ftms_workout(enum radiant_ble_ftms_workout w, void *ctx)
{
	uint32_t adopted;

	(void)ctx;

	switch (w) {
	case RADIANT_BLE_FTMS_WORKOUT_START:
		return drive_start(false, cfg.session->epoch, ftms_token());
	case RADIANT_BLE_FTMS_WORKOUT_RESUME:
		return drive_start(true, cfg.session->epoch, ftms_token());
	case RADIANT_BLE_FTMS_WORKOUT_PAUSE:
		return drive_stop(true);
	case RADIANT_BLE_FTMS_WORKOUT_STOP:
		/* The commanded target follows the stop, so a later resume of
		 * a canned workout does not restore an old setpoint. */
		(void)treadmill_state_command_speed(cfg.state, 0u, &adopted);
		return drive_stop(false);
	default:
		return -ENOTSUP;
	}
}

int treadmill_cmd_ftms_reset(bool motion_ok, void *ctx)
{
	uint32_t adopted_speed;
	int32_t adopted_incline;
	int incline_rc = 0;
	int speed_rc;

	(void)ctx;

	/*
	 * §4.16.2: target speed and inclination to 0. The incline half MOVES THE
	 * DECK and is motion (RESET_INCLINE); the speed half is a stop and
	 * always runs, so a Reset whose incline half is refused still stops the
	 * belt (and the core answers 0x04).
	 */
	if (motion_ok && cfg.incline_cmd) {
		incline_rc = drive_incline(TREADMILL_MOTION_RESET_INCLINE, 0,
					   cfg.session->epoch, ftms_token());
		if (incline_rc == 0) {
			(void)treadmill_state_command_incline(cfg.state, 0, &adopted_incline);
		}
		incline_rc = accepted_or_rc(incline_rc);
	}

	(void)treadmill_state_command_speed(cfg.state, 0u, &adopted_speed);
	speed_rc = accepted_or_rc(drive_speed(TREADMILL_MOTION_STOP, 0u, cfg.session->epoch,
					      false));

	return (incline_rc != 0) ? incline_rc : speed_rc;
}

enum radiant_ble_ftms_verdict treadmill_cmd_ftms_auth(const struct radiant_ble_ftms_motion *m,
						      const struct radiant_ble_ftms_session *s,
						      void *ctx)
{
	struct treadmill_motion_cmd c = { .token = ftms_token() };
	int rc;

	(void)s; /* the bound treadmill session is the same session, in full */
	(void)ctx;

	/* The core reports staleness as a flag; the gate compares epochs. */
	c.epoch = m->stale ? cfg.session->epoch + 1u : cfg.session->epoch;

	switch (m->op) {
	case RADIANT_BLE_FTMS_OP_SET_SPEED:
		c.kind = TREADMILL_MOTION_SPEED;
		c.value = (int32_t)treadmill_mm_s_from_ftms_speed(m->target.v.speed_centi_kph);
		break;
	case RADIANT_BLE_FTMS_OP_SET_INCLINE:
		c.kind = TREADMILL_MOTION_INCLINE;
		c.value = treadmill_fec_incline_from_ftms(m->target.v.incline_deci);
		break;
	case RADIANT_BLE_FTMS_OP_START_RESUME:
		c.kind = m->resume ? TREADMILL_MOTION_RESUME : TREADMILL_MOTION_START;
		break;
	case RADIANT_BLE_FTMS_OP_RESET:
		c.kind = TREADMILL_MOTION_RESET_INCLINE;
		break;
	default:
		/* Nothing else moves a treadmill; anything new is refused until
		 * it is mapped here deliberately. */
		return RADIANT_BLE_FTMS_DENY;
	}

	rc = treadmill_motion_permitted(&c, cfg.session);
	if (rc == 0) {
		return RADIANT_BLE_FTMS_PERMIT;
	}
	return (rc == -ECANCELED) ? RADIANT_BLE_FTMS_STALE : RADIANT_BLE_FTMS_DENY;
}
