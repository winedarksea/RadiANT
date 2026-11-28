/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The session state machine and the motion rule (treadmill_session.c).
 *
 * The gate is a safety property, so each of its four conditions has a case
 * that holds the other three true and fails only that one, and STOP has a case
 * in every state with every condition failed at once.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "treadmill_session.h"
#include "treadmill_state.h"

static bool remote_ok;

static bool fake_remote_allowed(void)
{
	return remote_ok;
}

static struct treadmill_motion_policy policy;
static struct treadmill_session s;

static void session_reset(void *fixture)
{
	ARG_UNUSED(fixture);
	remote_ok = true;
	policy.remote_motion_allowed = fake_remote_allowed;
	policy.remote_start = false;
	treadmill_session_init(&s, &policy);
	/* The seam requires a key report at start; the default fixture has it. */
	(void)treadmill_session_input(&s, TREADMILL_SESSION_KEY_IN);
}

static struct treadmill_motion_cmd cmd(enum treadmill_motion_kind kind, int32_t value)
{
	struct treadmill_motion_cmd c = {
		.kind = kind, .value = value, .epoch = s.epoch, .token = true,
	};

	return c;
}

/* RUNNING with the belt reported moving. */
static void go_running(void)
{
	(void)treadmill_session_input(&s, TREADMILL_SESSION_FE_IN_USE);
	treadmill_session_set_belt(&s, 3000u);
	zassert_equal(s.run, TREADMILL_RUN_RUNNING, NULL);
	zassert_true(treadmill_session_moving(&s), NULL);
}

/* ── Transitions ────────────────────────────────────────────────────────── */

ZTEST(treadmill_session, test_state_reports_drive_the_session)
{
	struct treadmill_session_change c;

	zassert_equal(s.run, TREADMILL_RUN_IDLE, NULL);
	c = treadmill_session_input(&s, TREADMILL_SESSION_FE_IN_USE);
	zassert_equal(c.prev, TREADMILL_RUN_IDLE, NULL);
	zassert_equal(c.next, TREADMILL_RUN_RUNNING, NULL);
	zassert_false(c.epoch_bumped, "a state report is not a local stop");

	c = treadmill_session_input(&s, TREADMILL_SESSION_FE_FINISHED);
	zassert_equal(c.next, TREADMILL_RUN_STOPPED, NULL);
	c = treadmill_session_input(&s, TREADMILL_SESSION_FE_IN_USE);
	zassert_equal(c.next, TREADMILL_RUN_RUNNING, "a new session from STOPPED");
	c = treadmill_session_input(&s, TREADMILL_SESSION_FE_READY);
	zassert_equal(c.next, TREADMILL_RUN_IDLE, NULL);
	zassert_equal(s.epoch, 0u, NULL);
}

ZTEST(treadmill_session, test_pause_and_resume)
{
	go_running();

	/* A remote pause the machine carried out: no epoch bump. */
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_PAUSED).next,
		      TREADMILL_RUN_PAUSED, NULL);
	zassert_equal(s.epoch, 0u, NULL);

	/* FE state 4 is FINISHED (PAUSED): while paused it is the pause, and
	 * IN_USE is not a resume either. */
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_FE_FINISHED).next,
		      TREADMILL_RUN_PAUSED, NULL);
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_FE_IN_USE).next,
		      TREADMILL_RUN_PAUSED, NULL);

	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_RESUMED).next,
		      TREADMILL_RUN_RUNNING, NULL);

	/* A console pause is local: the epoch moves. */
	zassert_true(treadmill_session_input(&s, TREADMILL_SESSION_CONSOLE_PAUSE).epoch_bumped,
		     NULL);
	zassert_equal(s.run, TREADMILL_RUN_PAUSED, NULL);
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_CONSOLE_START).next,
		      TREADMILL_RUN_RUNNING, "console Start resumes");
}

ZTEST(treadmill_session, test_every_local_stop_bumps_the_epoch)
{
	uint32_t e0 = s.epoch;
	struct treadmill_session_change c;

	/* Even on an idle machine: a queued remote start must not land after
	 * a Stop pressed before the belt ever moved. */
	c = treadmill_session_input(&s, TREADMILL_SESSION_CONSOLE_STOP);
	zassert_true(c.epoch_bumped, NULL);
	zassert_equal(s.epoch, e0 + 1u, NULL);
	zassert_equal(s.run, TREADMILL_RUN_IDLE, NULL);

	go_running();
	c = treadmill_session_input(&s, TREADMILL_SESSION_CONSOLE_STOP);
	zassert_true(c.epoch_bumped, NULL);
	zassert_equal(c.next, TREADMILL_RUN_STOPPED, NULL);
	zassert_equal(s.epoch, e0 + 2u, NULL);

	/* A console Start does not. */
	zassert_false(treadmill_session_input(&s, TREADMILL_SESSION_CONSOLE_START).epoch_bumped,
		      NULL);
}

ZTEST(treadmill_session, test_safety_key_latches_from_every_state)
{
	static const enum treadmill_session_input to[] = {
		TREADMILL_SESSION_FE_READY,      /* IDLE */
		TREADMILL_SESSION_FE_IN_USE,     /* RUNNING */
		TREADMILL_SESSION_PAUSED,        /* PAUSED */
		TREADMILL_SESSION_FE_FINISHED,   /* STOPPED (from RUNNING) */
	};

	for (size_t i = 0; i < ARRAY_SIZE(to); i++) {
		struct treadmill_session_change c;
		uint32_t e;

		session_reset(NULL);
		if (i >= 1u) {
			go_running();
		}
		(void)treadmill_session_input(&s, to[i]);
		e = s.epoch;

		c = treadmill_session_input(&s, TREADMILL_SESSION_KEY_OUT);
		zassert_equal(c.next, TREADMILL_RUN_SAFETY_STOP, "case %u", (unsigned)i);
		zassert_true(c.epoch_bumped, NULL);
		zassert_true(c.revoke_control, "a key pull revokes control");
		zassert_equal(s.epoch, e + 1u, NULL);
		zassert_equal(s.key, TREADMILL_KEY_OUT, NULL);
	}
}

ZTEST(treadmill_session, test_the_latch_needs_key_in_and_a_console_action)
{
	go_running();
	(void)treadmill_session_input(&s, TREADMILL_SESSION_KEY_OUT);

	/* Nothing the machine reports clears it, and neither does a console
	 * action with the key still out. */
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_FE_IN_USE).next,
		      TREADMILL_RUN_SAFETY_STOP, NULL);
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_FE_READY).next,
		      TREADMILL_RUN_SAFETY_STOP, NULL);
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_RESUMED).next,
		      TREADMILL_RUN_SAFETY_STOP, NULL);
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_CONSOLE_START).next,
		      TREADMILL_RUN_SAFETY_STOP, "key still out");

	/* Key in alone: still latched. */
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_KEY_IN).next,
		      TREADMILL_RUN_SAFETY_STOP, NULL);
	zassert_equal(s.key, TREADMILL_KEY_IN, NULL);
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_CONSOLE_PAUSE).next,
		      TREADMILL_RUN_SAFETY_STOP, "pause is not the action the latch waits for");

	/* Key in plus console Stop acknowledges it. */
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_CONSOLE_STOP).next,
		      TREADMILL_RUN_IDLE, NULL);

	/* And key in plus console Start clears it straight into a run. */
	(void)treadmill_session_input(&s, TREADMILL_SESSION_KEY_OUT);
	(void)treadmill_session_input(&s, TREADMILL_SESSION_KEY_IN);
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_CONSOLE_START).next,
		      TREADMILL_RUN_RUNNING, NULL);
}

ZTEST(treadmill_session, test_page16_state_is_derived_from_the_session)
{
	zassert_equal(treadmill_session_fe_state(TREADMILL_RUN_IDLE),
		      TREADMILL_STATE_READY, NULL);
	zassert_equal(treadmill_session_fe_state(TREADMILL_RUN_RUNNING),
		      TREADMILL_STATE_IN_USE, NULL);
	zassert_equal(treadmill_session_fe_state(TREADMILL_RUN_PAUSED),
		      TREADMILL_STATE_FINISHED, "FINISHED (PAUSED)");
	zassert_equal(treadmill_session_fe_state(TREADMILL_RUN_STOPPED),
		      TREADMILL_STATE_FINISHED, NULL);
	zassert_equal(treadmill_session_fe_state(TREADMILL_RUN_SAFETY_STOP),
		      TREADMILL_STATE_FINISHED, NULL);
}

/* ── The gate ───────────────────────────────────────────────────────────── */

ZTEST(treadmill_session, test_stopping_always_passes)
{
	static const enum treadmill_session_input to[] = {
		TREADMILL_SESSION_FE_READY, TREADMILL_SESSION_FE_IN_USE,
		TREADMILL_SESSION_PAUSED, TREADMILL_SESSION_FE_FINISHED,
		TREADMILL_SESSION_KEY_OUT,
	};

	for (size_t i = 0; i < ARRAY_SIZE(to); i++) {
		struct treadmill_motion_cmd stop, speed0;

		session_reset(NULL);
		if (i >= 1u) {
			go_running();
		}
		(void)treadmill_session_input(&s, to[i]);

		/* EVERY condition failed at once: no token, remote disabled,
		 * a stale epoch, remote start off, and (last case) the latch. */
		remote_ok = false;
		stop = cmd(TREADMILL_MOTION_STOP, 0);
		stop.token = false;
		stop.epoch = s.epoch + 7u;
		speed0 = cmd(TREADMILL_MOTION_SPEED, 0);
		speed0.token = false;
		speed0.epoch = s.epoch + 7u;

		zassert_equal(treadmill_motion_permitted(&stop, &s), 0, "case %u", (unsigned)i);
		zassert_equal(treadmill_motion_permitted(&speed0, &s), 0,
			      "speed 0 is a stop, case %u", (unsigned)i);
	}
}

ZTEST(treadmill_session, test_all_four_conditions_pass)
{
	struct treadmill_motion_cmd c;

	go_running();
	c = cmd(TREADMILL_MOTION_SPEED, 3000);
	zassert_equal(treadmill_motion_permitted(&c, &s), 0, NULL);
	c = cmd(TREADMILL_MOTION_INCLINE, 500);
	zassert_equal(treadmill_motion_permitted(&c, &s), 0, NULL);
	c = cmd(TREADMILL_MOTION_SIM_GRADE, -200);
	zassert_equal(treadmill_motion_permitted(&c, &s), 0, NULL);
	c = cmd(TREADMILL_MOTION_RESET_INCLINE, 0);
	zassert_equal(treadmill_motion_permitted(&c, &s), 0, NULL);
}

ZTEST(treadmill_session, test_a_without_remote_motion_allowed)
{
	struct treadmill_motion_cmd c;

	go_running();
	remote_ok = false;
	c = cmd(TREADMILL_MOTION_SPEED, 3000);
	zassert_equal(treadmill_motion_permitted(&c, &s), -EACCES, NULL);
	c = cmd(TREADMILL_MOTION_INCLINE, 100);
	zassert_equal(treadmill_motion_permitted(&c, &s), -EACCES, NULL);

	/* A source that cannot answer is a no. */
	remote_ok = true;
	policy.remote_motion_allowed = NULL;
	zassert_equal(treadmill_motion_permitted(&c, &s), -EACCES, NULL);
}

ZTEST(treadmill_session, test_b_in_safety_stop)
{
	struct treadmill_motion_cmd c;

	go_running();
	policy.remote_start = true;
	(void)treadmill_session_input(&s, TREADMILL_SESSION_KEY_OUT);
	/* Even with the key back in and remote enabled: latched. */
	(void)treadmill_session_input(&s, TREADMILL_SESSION_KEY_IN);
	c = cmd(TREADMILL_MOTION_START, 0);
	zassert_equal(treadmill_motion_permitted(&c, &s), -EACCES, NULL);
	c = cmd(TREADMILL_MOTION_INCLINE, 100);
	zassert_equal(treadmill_motion_permitted(&c, &s), -EACCES, NULL);
	c = cmd(TREADMILL_MOTION_SIM_GRADE, 100);
	zassert_equal(treadmill_motion_permitted(&c, &s), -EACCES, NULL);
}

ZTEST(treadmill_session, test_c_starting_from_rest_needs_remote_start)
{
	static const enum treadmill_run rest[] = {
		TREADMILL_RUN_IDLE, TREADMILL_RUN_PAUSED, TREADMILL_RUN_STOPPED,
	};

	for (size_t i = 0; i < ARRAY_SIZE(rest); i++) {
		struct treadmill_motion_cmd start, resume, speed, incline;

		s.run = rest[i];
		start = cmd(TREADMILL_MOTION_START, 0);
		resume = cmd(TREADMILL_MOTION_RESUME, 0);
		speed = cmd(TREADMILL_MOTION_SPEED, 1000);
		incline = cmd(TREADMILL_MOTION_INCLINE, 100);

		policy.remote_start = false;
		zassert_equal(treadmill_motion_permitted(&start, &s), -EACCES, NULL);
		zassert_equal(treadmill_motion_permitted(&resume, &s), -EACCES, NULL);
		zassert_equal(treadmill_motion_permitted(&speed, &s), -EACCES,
			      "the Set-Speed auto-start is the same rule");
		/* The deck does not move remotely at rest either (S2). */
		zassert_equal(treadmill_motion_permitted(&incline, &s), -EACCES, NULL);

		policy.remote_start = true;
		zassert_equal(treadmill_motion_permitted(&start, &s), 0, NULL);
		zassert_equal(treadmill_motion_permitted(&speed, &s), 0, NULL);
		zassert_equal(treadmill_motion_permitted(&incline, &s), 0, NULL);
	}

	/* Already moving: a speed change does not start anything. */
	policy.remote_start = false;
	s.run = TREADMILL_RUN_RUNNING;
	treadmill_session_set_belt(&s, 3000u);
	{
		struct treadmill_motion_cmd speed = cmd(TREADMILL_MOTION_SPEED, 4000);

		zassert_equal(treadmill_motion_permitted(&speed, &s), 0, NULL);
	}
}

ZTEST(treadmill_session, test_d_without_the_token)
{
	struct treadmill_motion_cmd c;

	go_running();
	c = cmd(TREADMILL_MOTION_SPEED, 3000);
	c.token = false;
	zassert_equal(treadmill_motion_permitted(&c, &s), -EACCES, NULL);
	c = cmd(TREADMILL_MOTION_SIM_GRADE, 300);
	c.token = false;
	zassert_equal(treadmill_motion_permitted(&c, &s), -EACCES, NULL);
}

ZTEST(treadmill_session, test_a_stale_epoch_is_cancelled_not_denied)
{
	struct treadmill_motion_cmd c;

	go_running();
	c = cmd(TREADMILL_MOTION_SPEED, 3000);
	(void)treadmill_session_input(&s, TREADMILL_SESSION_CONSOLE_STOP);
	(void)treadmill_session_input(&s, TREADMILL_SESSION_CONSOLE_START);
	/* The belt is running again and everything else passes - but the
	 * request was received before the Stop. */
	zassert_equal(s.run, TREADMILL_RUN_RUNNING, NULL);
	zassert_equal(treadmill_motion_permitted(&c, &s), -ECANCELED, NULL);

	c.epoch = s.epoch;
	zassert_equal(treadmill_motion_permitted(&c, &s), 0, NULL);
}

/* ── Moving is not running (S1) ─────────────────────────────────────────── */

ZTEST(treadmill_session, test_running_at_zero_is_not_moving)
{
	struct treadmill_motion_cmd speed, incline;

	/* A console session started at 0: RUNNING, belt reported 0. */
	(void)treadmill_session_input(&s, TREADMILL_SESSION_CONSOLE_START);
	zassert_equal(s.run, TREADMILL_RUN_RUNNING, NULL);
	zassert_false(treadmill_session_moving(&s), NULL);
	speed = cmd(TREADMILL_MOTION_SPEED, 1000);
	incline = cmd(TREADMILL_MOTION_INCLINE, 100);
	zassert_equal(treadmill_motion_permitted(&speed, &s), -EACCES,
		      "a remote speed may not start a belt at rest");
	zassert_equal(treadmill_motion_permitted(&incline, &s), -EACCES, NULL);

	/* Once the belt reports moving, both are fine. */
	treadmill_session_set_belt(&s, 800u);
	zassert_equal(treadmill_motion_permitted(&speed, &s), 0, NULL);
	zassert_equal(treadmill_motion_permitted(&incline, &s), 0, NULL);
}

ZTEST(treadmill_session, test_an_accepted_remote_stop_ends_moving_at_once)
{
	struct treadmill_motion_cmd speed;

	go_running();
	treadmill_session_note_stop(&s);
	/* Still RUNNING, the belt still reports speed: but a stop was accepted. */
	zassert_equal(s.run, TREADMILL_RUN_RUNNING, NULL);
	zassert_false(treadmill_session_moving(&s), NULL);
	speed = cmd(TREADMILL_MOTION_SPEED, 3000);
	zassert_equal(treadmill_motion_permitted(&speed, &s), -EACCES, NULL);

	/* A new session clears it. */
	(void)treadmill_session_input(&s, TREADMILL_SESSION_FE_FINISHED);
	(void)treadmill_session_input(&s, TREADMILL_SESSION_FE_IN_USE);
	zassert_true(treadmill_session_moving(&s), NULL);
}

ZTEST(treadmill_session, test_an_unknown_key_permits_nothing)
{
	struct treadmill_motion_cmd speed, stop;

	treadmill_session_init(&s, &policy);
	zassert_equal(s.key, TREADMILL_KEY_UNKNOWN, NULL);
	(void)treadmill_session_input(&s, TREADMILL_SESSION_FE_IN_USE);
	treadmill_session_set_belt(&s, 3000u);
	speed = cmd(TREADMILL_MOTION_SPEED, 3000);
	zassert_equal(treadmill_motion_permitted(&speed, &s), -EACCES, NULL);
	stop = cmd(TREADMILL_MOTION_STOP, 0);
	zassert_equal(treadmill_motion_permitted(&stop, &s), 0, "stopping still passes");

	(void)treadmill_session_input(&s, TREADMILL_SESSION_KEY_IN);
	zassert_equal(treadmill_motion_permitted(&speed, &s), 0, NULL);
}

ZTEST(treadmill_session, test_remote_reset_returns_to_idle)
{
	struct treadmill_session_change c;

	go_running();
	c = treadmill_session_input(&s, TREADMILL_SESSION_REMOTE_RESET);
	zassert_equal(c.next, TREADMILL_RUN_IDLE, NULL);
	zassert_true(c.epoch_bumped, NULL);

	/* It cannot clear the latch. */
	(void)treadmill_session_input(&s, TREADMILL_SESSION_KEY_OUT);
	zassert_equal(treadmill_session_input(&s, TREADMILL_SESSION_REMOTE_RESET).next,
		      TREADMILL_RUN_SAFETY_STOP, NULL);
}

ZTEST_SUITE(treadmill_session, NULL, NULL, session_reset, NULL, NULL);
