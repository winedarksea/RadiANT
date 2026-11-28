/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_cmd.c - every motion path, each behind treadmill_motion_permitted().
 * See treadmill_cmd.h. Kernel-free.
 *
 * THE INVARIANT THIS FILE EXISTS FOR: every call into treadmill_source_ctrl.h
 * is inside one of the four treadmill_cmd_drive_*() doors, and each of those
 * calls gate() on its first line and returns before the source if gate()
 * refuses. A new path adds a call to one of these, never a new call into the
 * source.
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

bool treadmill_cmd_ftms_token(void)
{
	return (cfg.ctrl_owner == NULL) || (cfg.ctrl_owner() == RADIANT_CTRL_BLE);
}

bool treadmill_cmd_grade_token(void)
{
	/* The same token FTMS uses: the grade provider is a BLE-side command
	 * source and may not act while the BLE side does not own the machine. */
	return treadmill_cmd_ftms_token();
}

/* ── The four doors to the source ───────────────────────────────────────── */

int treadmill_cmd_drive_speed(enum treadmill_motion_kind kind, uint32_t mm_s, uint32_t epoch,
			      bool token)
{
	int rc = gate(kind, (int32_t)mm_s, epoch, token);

	if (rc != 0) {
		return rc;
	}
	return treadmill_source_set_speed(mm_s);
}

int treadmill_cmd_drive_incline(enum treadmill_motion_kind kind, int32_t centi, uint32_t epoch,
				bool token)
{
	int rc = gate(kind, centi, epoch, token);

	if (rc != 0) {
		return rc;
	}
	return treadmill_source_set_incline(centi);
}

int treadmill_cmd_drive_start(bool resume, uint32_t epoch, bool token)
{
	int rc = gate(resume ? TREADMILL_MOTION_RESUME : TREADMILL_MOTION_START, 0, epoch,
		      token);

	if (rc != 0) {
		return rc;
	}
	return resume ? treadmill_source_workout_resume() : treadmill_source_workout_start();
}

/* Stop and Pause: the gate always answers 0 for a STOP; it is called anyway
 * so that this door looks like the other three. A source without the hook
 * falls back to set_speed(0) inside treadmill_source.c. */
int treadmill_cmd_drive_stop(bool pause)
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

/* The shared front half of pages 50 and 51: capability, epoch, the motion
 * rule, then the claim - in that order, so an undeclared, stale or refused
 * page never touches the token (a refused page that took it would lock FTMS
 * and the grade provider out until the ANT timeout). Page 50 moves nothing but
 * is gated with page 51, so it is asked the same rule. 0xFF means "go on". */
static uint8_t fec_front(int32_t grade_centi, uint32_t rx_epoch)
{
	struct treadmill_motion_cmd probe = {
		.kind = TREADMILL_MOTION_SIM_GRADE, .value = grade_centi,
		.epoch = rx_epoch, .token = true, /* (d) is the claim, below */
	};
	int rc;

	if (!cfg.incline_cmd) {
		/* Page 54 does not declare Simulation mode: the answer the
		 * declaration implies, and the page is ungated. */
		return RADIANT_PROFILE_FEC_CMD_STATUS_NOT_SUPPORTED;
	}
	if (rx_epoch != cfg.session->epoch) {
		/* Received before a local stop or a safety-key event. Refused
		 * BEFORE the claim: a stale page must not re-take a token the
		 * key-out revoked. */
		return RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED;
	}
	rc = treadmill_motion_permitted(&probe, cfg.session);
	if (rc != 0) {
		return fec_status_for(rc);
	}
	if (cfg.fec_claim != NULL && !cfg.fec_claim()) {
		/* An FTMS client ran Request Control: explicit beats implicit. */
		return RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED;
	}
	return 0xFFu;
}

uint8_t treadmill_cmd_fec_track_resistance(int32_t grade_centi, uint32_t rx_epoch)
{
	uint8_t front = fec_front(grade_centi, rx_epoch);
	int32_t adopted;
	int rc;

	/* Authorised (and claimed) BEFORE the target moves: a refused page
	 * leaves the target where it was. The door asks again. */
	if (front != 0xFFu) {
		return front;
	}
	/* A simulation grade: CLAMPED to the machine range and answered PASS -
	 * the road may be steeper than the deck. */
	adopted = treadmill_state_request_incline(cfg.state, grade_centi);
	rc = treadmill_cmd_drive_incline(TREADMILL_MOTION_SIM_GRADE, adopted, rx_epoch, true);
	return fec_status_for(rc);
}

uint8_t treadmill_cmd_fec_wind(uint32_t rx_epoch)
{
	uint8_t front = fec_front(0, rx_epoch);

	return (front != 0xFFu) ? front : RADIANT_PROFILE_FEC_CMD_STATUS_PASS;
}

/* ── The grade provider ─────────────────────────────────────────────────── */

int treadmill_cmd_grade_provider(int32_t grade_centi)
{
	bool token = treadmill_cmd_grade_token();
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
	return treadmill_cmd_drive_incline(TREADMILL_MOTION_SIM_GRADE, adopted,
					   cfg.session->epoch, token);
}

/* ── FTMS ───────────────────────────────────────────────────────────────── */

int treadmill_cmd_ftms_set_target(struct radiant_ble_ftms_target *t, uint32_t epoch,
				  void *ctx)
{
	bool token = treadmill_cmd_ftms_token();
	int rc;

	(void)ctx;

	switch (t->op) {
	case RADIANT_BLE_FTMS_OP_SET_SPEED: {
		uint32_t mm_s = treadmill_mm_s_from_ftms_speed(t->v.speed_centi_kph);
		uint32_t adopted = 0u;

		if (!cfg.speed_cmd) {
			return -ENOTSUP;
		}
		rc = gate(TREADMILL_MOTION_SPEED, (int32_t)mm_s, epoch, token);
		if (rc != 0) {
			return rc;
		}
		/* EXPLICIT: refused out of range, target unchanged. */
		rc = treadmill_state_command_speed(cfg.state, mm_s, &adopted);
		if (rc != 0) {
			return rc;
		}
		rc = treadmill_cmd_drive_speed(TREADMILL_MOTION_SPEED, adopted, epoch, token);
		if (adopted == 0u) {
			/* A requested stop counts whatever the driver said. */
			treadmill_session_note_stop(cfg.session);
		} else if (rc == 0) {
			treadmill_session_note_start(cfg.session);
		}
		return rc;
	}
	case RADIANT_BLE_FTMS_OP_SET_INCLINE: {
		int32_t centi = treadmill_fec_incline_from_ftms(t->v.incline_deci);
		int32_t adopted = 0;

		if (!cfg.incline_cmd) {
			return -ENOTSUP;
		}
		rc = gate(TREADMILL_MOTION_INCLINE, centi, epoch, token);
		if (rc != 0) {
			return rc;
		}
		rc = treadmill_state_command_incline(cfg.state, centi, &adopted);
		if (rc != 0) {
			return rc;
		}
		return treadmill_cmd_drive_incline(TREADMILL_MOTION_INCLINE, adopted, epoch, token);
	}
	default:
		return -ENOTSUP;
	}
}

int treadmill_cmd_ftms_workout(enum radiant_ble_ftms_workout w, uint32_t epoch, void *ctx)
{
	uint32_t adopted;
	int rc;

	(void)ctx;

	switch (w) {
	case RADIANT_BLE_FTMS_WORKOUT_START:
	case RADIANT_BLE_FTMS_WORKOUT_RESUME:
		rc = treadmill_cmd_drive_start(w == RADIANT_BLE_FTMS_WORKOUT_RESUME, epoch,
					       treadmill_cmd_ftms_token());
		if (rc == 0) {
			treadmill_session_note_start(cfg.session);
		}
		return rc;
	case RADIANT_BLE_FTMS_WORKOUT_PAUSE:
	case RADIANT_BLE_FTMS_WORKOUT_STOP:
		if (w == RADIANT_BLE_FTMS_WORKOUT_STOP) {
			/* The commanded target follows the stop, so a later
			 * resume does not restore an old setpoint. */
			(void)treadmill_state_command_speed(cfg.state, 0u, &adopted);
		}
		rc = treadmill_cmd_drive_stop(w == RADIANT_BLE_FTMS_WORKOUT_PAUSE);
		/* The belt stops counting as moving NOW, before the machine
		 * reports it, and WHATEVER THE DRIVER ANSWERED: a stop it
		 * refused must not leave a remote speed-up or a queued page 51
		 * free to pass condition (c). */
		treadmill_session_note_stop(cfg.session);
		return rc;
	default:
		return -ENOTSUP;
	}
}

/* Reset has an incline half only if there is a deck to level that is not
 * already level. */
static bool reset_needs_incline(void)
{
	return cfg.incline_cmd && (cfg.state->target_incline_centi_pct != 0);
}

int treadmill_cmd_ftms_reset(bool motion_ok, uint32_t epoch, void *ctx)
{
	uint32_t adopted_speed;
	int32_t adopted_incline;
	int incline_rc = 0;
	int speed_rc;

	(void)ctx;

	/*
	 * §4.16.2: target speed and inclination to 0. THE BELT STOPS FIRST: the
	 * speed half is a stop and always runs. Then the incline half, which
	 * moves the deck and is motion (RESET_INCLINE) - authorised by the core
	 * before execute, and asked again at its door with the receipt epoch.
	 *
	 * The session is told about the stop only AFTER both halves: the
	 * incline door must judge the machine as it was when the Reset was
	 * authorised, not as the speed half has just left it.
	 */
	(void)treadmill_state_command_speed(cfg.state, 0u, &adopted_speed);
	speed_rc = accepted_or_rc(treadmill_cmd_drive_speed(TREADMILL_MOTION_STOP, 0u, epoch,
							    false));

	if (motion_ok && reset_needs_incline()) {
		incline_rc = treadmill_cmd_drive_incline(TREADMILL_MOTION_RESET_INCLINE, 0, epoch,
							 treadmill_cmd_ftms_token());
		if (incline_rc == 0) {
			(void)treadmill_state_command_incline(cfg.state, 0, &adopted_incline);
		}
		incline_rc = accepted_or_rc(incline_rc);
	}

	/* A requested stop counts whatever the driver said. */
	treadmill_session_note_stop(cfg.session);
	return (speed_rc != 0) ? speed_rc : incline_rc;
}

enum radiant_ble_ftms_verdict treadmill_cmd_ftms_auth(const struct radiant_ble_ftms_motion *m,
						      const struct radiant_ble_ftms_session *s,
						      void *ctx)
{
	struct treadmill_motion_cmd c = { .token = treadmill_cmd_ftms_token() };
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
		if (cfg.session->run == TREADMILL_RUN_SAFETY_STOP) {
			/* Latched: the stop half still runs (a stop always
			 * passes), but the procedure is refused - 0x04, no
			 * Training Status Idle, no session change - so FTMS and
			 * page 16 keep agreeing that the machine is stopped by
			 * its safety key. */
			return RADIANT_BLE_FTMS_DENY;
		}
		if (!reset_needs_incline()) {
			/* No deck to move: Reset is all stop, and a stop
			 * always passes. */
			return RADIANT_BLE_FTMS_PERMIT;
		}
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
