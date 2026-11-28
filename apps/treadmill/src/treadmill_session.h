/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_session.h - THE SESSION, AND THE ONE RULE THAT MAY MOVE THE BELT.
 *
 * ─── The session ───────────────────────────────────────────────────────────
 *
 * One state machine, IDLE / RUNNING / PAUSED / STOPPED plus a latched
 * SAFETY_STOP, fed by what the MACHINE says: the source's FE state reports and
 * its events (console start/stop/pause, a remote pause or resume it carried
 * out, safety key out/in). FTMS Training Status, the FTMS machine-status
 * notifications (0x02/0x03/0x04) and FE-C page 16's state are all derived from
 * it, so the two radios cannot disagree about whether the machine is running.
 *
 * COMMANDS DO NOT MOVE THE SESSION. A command result says the driver ACCEPTED
 * a request (its setter or workout hook returned 0); the session moves when
 * the machine reports that it actually did something. A machine that accepts
 * Start and never starts the belt stays IDLE, and is visible as such.
 *
 * THE EPOCH. Every local stop (console Stop or Pause) and every safety-key
 * event increments it. A request queued before that - FTMS cp_pending, the
 * FE-C control-page ring - carries the epoch it was received under, and a
 * mismatch at dequeue refuses it without reaching the driver. A remote "go
 * faster" that was in flight when the runner hit Stop must not land after the
 * Stop.
 *
 * THE SAFETY LATCH. A safety-key pull enters SAFETY_STOP from any state and
 * revokes every remote controller. Only a local key-in PLUS a console action
 * (Start or Stop) clears it; no remote command, state report or key-in alone
 * does. The key starts UNKNOWN and nothing may move remotely until the source
 * has reported it (treadmill_source.h requires a report at start).
 *
 * MOVING IS NOT RUNNING. RUNNING is a session; the belt can sit at 0 inside
 * one (a console session started at 0, or a remote Stop/speed 0 that the
 * machine has not yet turned into a FINISHED report). "Moving", which the
 * motion rule's condition (c) uses, is RUNNING AND a reported belt speed above
 * 0 AND no remote stop accepted since (treadmill_session_note_stop()).
 *
 * A REMOTE RESET (FTMS §4.16.2) returns the session to IDLE and bumps the
 * epoch, unless it is latched.
 *
 * ─── The motion rule ───────────────────────────────────────────────────────
 *
 * A MOTION COMMAND is anything that can start the belt, raise its speed or
 * move the deck. Every path that issues one - FTMS Start/Resume, Set Target
 * Speed, Set Target Inclination, the incline half of FTMS Reset, FE-C page 51,
 * the BLE grade provider, any future setter - calls treadmill_motion_permitted()
 * and nothing else decides. treadmill_cmd.c is the only caller of the source's
 * actuating entry points, and every one of its paths goes through this gate.
 *
 * STOPPING ALWAYS PASSES: speed 0, FTMS Stop and Pause, and the speed half of
 * Reset never need authorization, a stale epoch, a token or anything else.
 *
 * ─── No kernel ─────────────────────────────────────────────────────────────
 *
 * Like treadmill_state.c: no Zephyr header, no lock, no clock. Everything that
 * touches a session runs on the system workqueue (treadmill_source.h rule 1);
 * the one exception is the epoch, which the receive paths read on their own
 * threads through main.c's atomic mirror, never from this struct.
 */

#ifndef TREADMILL_SESSION_H_
#define TREADMILL_SESSION_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum treadmill_run {
	TREADMILL_RUN_IDLE = 0,
	TREADMILL_RUN_RUNNING,
	TREADMILL_RUN_PAUSED,
	TREADMILL_RUN_STOPPED,
	TREADMILL_RUN_SAFETY_STOP,
};

enum treadmill_session_input {
	/* The source's FE state reports (treadmill_source_report_state()). */
	TREADMILL_SESSION_FE_READY = 0,
	TREADMILL_SESSION_FE_IN_USE,
	TREADMILL_SESSION_FE_FINISHED,
	/* The source's events (treadmill_source_report_event()). LOCAL means at
	 * the machine's own console. */
	TREADMILL_SESSION_CONSOLE_START,
	TREADMILL_SESSION_CONSOLE_STOP,
	TREADMILL_SESSION_CONSOLE_PAUSE,
	TREADMILL_SESSION_PAUSED,  /* paused, after an accepted remote Pause */
	TREADMILL_SESSION_RESUMED, /* running again, after a remote Resume */
	TREADMILL_SESSION_KEY_OUT,
	TREADMILL_SESSION_KEY_IN,
	/* An FTMS Reset the machine accepted. */
	TREADMILL_SESSION_REMOTE_RESET,
};

enum treadmill_key {
	TREADMILL_KEY_UNKNOWN = 0, /* not yet reported: no remote motion */
	TREADMILL_KEY_IN,
	TREADMILL_KEY_OUT,
};

/*
 * What the gate needs from outside the session.
 *
 *   remote_motion_allowed  (a) the source: key present and remote control
 *                          enabled on the console. NULL means NEVER - a
 *                          source that cannot say is a source nobody may move
 *                          remotely.
 *   remote_start           (c) CONFIG_TREADMILL_REMOTE_START: may a remote
 *                          command start the belt from a non-RUNNING state.
 */
struct treadmill_motion_policy {
	bool (*remote_motion_allowed)(void);
	bool remote_start;
};

struct treadmill_session {
	enum treadmill_run run;
	uint32_t epoch;
	enum treadmill_key key;
	/* The source's last reported belt speed (treadmill_session_set_belt()). */
	uint32_t belt_mm_s;
	/* A remote stop, speed 0 or pause was accepted and the session has not
	 * re-entered RUNNING since: the belt does not count as moving. */
	bool stop_commanded;
	const struct treadmill_motion_policy *policy;
};

struct treadmill_session_change {
	enum treadmill_run prev;
	enum treadmill_run next;
	/* The epoch moved: queued requests from before this are now stale. */
	bool epoch_bumped;
	/* A safety-key pull: revoke every remote controller (the control token,
	 * and FTMS 0xFF Control Permission Lost). */
	bool revoke_control;
};

void treadmill_session_init(struct treadmill_session *s,
			    const struct treadmill_motion_policy *policy);

struct treadmill_session_change
treadmill_session_input(struct treadmill_session *s,
			enum treadmill_session_input in);

/* The source reported this belt speed. */
void treadmill_session_set_belt(struct treadmill_session *s, uint32_t mm_s);

/* A remote stop (speed 0, Stop, Pause, Reset) was accepted by the driver: the
 * belt stops counting as moving now, before the machine reports anything. */
void treadmill_session_note_stop(struct treadmill_session *s);

/* A remote start or speed above 0 was accepted (it passed the rule). */
void treadmill_session_note_start(struct treadmill_session *s);

/* RUNNING, belt reported above 0, no remote stop since. */
bool treadmill_session_moving(const struct treadmill_session *s);

/* FE-C page 16's state for a run state: IDLE READY, RUNNING IN_USE, and
 * FINISHED for PAUSED, STOPPED and SAFETY_STOP - FE state 4 is the profile's
 * "FINISHED (PAUSED)". */
uint8_t treadmill_session_fe_state(enum treadmill_run run);

/* ── The motion rule ────────────────────────────────────────────────────── */

enum treadmill_motion_kind {
	/* Speed 0, FTMS Stop or Pause, Reset's speed half. Always passes. */
	TREADMILL_MOTION_STOP = 0,
	TREADMILL_MOTION_START,         /* FTMS Start */
	TREADMILL_MOTION_RESUME,        /* FTMS Resume, from PAUSED */
	TREADMILL_MOTION_SPEED,         /* a speed setpoint; 0 is a STOP */
	TREADMILL_MOTION_INCLINE,       /* FTMS Set Target Inclination */
	TREADMILL_MOTION_RESET_INCLINE, /* the incline-to-0 half of FTMS Reset */
	TREADMILL_MOTION_SIM_GRADE,     /* FE-C page 51, the BLE grade provider */
};

struct treadmill_motion_cmd {
	enum treadmill_motion_kind kind;
	/* mm/s for SPEED, 0.01 % for the incline kinds; unused otherwise. */
	int32_t value;
	/* The session epoch the request was RECEIVED under. A path with no
	 * queue (the grade provider, run on the workqueue as it is asked)
	 * passes the current epoch. */
	uint32_t epoch;
	/* (d) the path holds the control token - for FTMS, this client has
	 * control and the arbiter's owner is BLE; for FE-C, the implicit claim
	 * succeeded. True under TREADMILL_CTRL_POLICY_NONE. */
	bool token;
};

/*
 * THE GATE. 0 if `cmd` may reach the driver; otherwise:
 *
 *   -ECANCELED the epoch moved since the request was received (a local Stop or
 *            a safety-key event in between). FTMS 0x04 Operation Failed,
 *            FE-C REJECTED.
 *   -EACCES  not authorized: FTMS 0x05 Control Not Permitted, FE-C REJECTED,
 *            a grade-provider value dropped.
 *
 * Authorization is ALL of:
 *   (a) policy->remote_motion_allowed() is true,
 *   (b) the session is not SAFETY_STOP and the key is reported IN,
 *   (c) for START, RESUME, SPEED > 0, or ANY incline change (INCLINE,
 *       RESET_INCLINE, SIM_GRADE) while the belt is not MOVING (see above):
 *       policy->remote_start (CONFIG_TREADMILL_REMOTE_START, default n) - the
 *       Set-Speed auto-start is this same rule, not a separate one,
 *   (d) cmd->token.
 *
 * A STOP (or SPEED 0) returns 0 before any of it is consulted.
 */
int treadmill_motion_permitted(const struct treadmill_motion_cmd *cmd,
			       const struct treadmill_session *s);

#ifdef __cplusplus
}
#endif

#endif /* TREADMILL_SESSION_H_ */
