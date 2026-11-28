/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_session.c - see treadmill_session.h. Kernel-free.
 */

#include <errno.h>
#include <stddef.h>

#include "treadmill_session.h"
#include "treadmill_state.h"

void treadmill_session_init(struct treadmill_session *s,
			    const struct treadmill_motion_policy *policy)
{
	s->run = TREADMILL_RUN_IDLE;
	s->epoch = 0u;
	s->key_in = true;
	s->policy = policy;
}

/* The latched state: only key-in plus a console action leaves it. */
static enum treadmill_run safety_next(struct treadmill_session *s,
				      enum treadmill_session_input in)
{
	switch (in) {
	case TREADMILL_SESSION_KEY_IN:
		s->key_in = true;
		return TREADMILL_RUN_SAFETY_STOP;
	case TREADMILL_SESSION_CONSOLE_START:
		return s->key_in ? TREADMILL_RUN_RUNNING : TREADMILL_RUN_SAFETY_STOP;
	case TREADMILL_SESSION_CONSOLE_STOP:
		return s->key_in ? TREADMILL_RUN_IDLE : TREADMILL_RUN_SAFETY_STOP;
	default:
		/* State reports, remote pause/resume echoes, console pause: none
		 * of them is the console action the latch waits for. */
		return TREADMILL_RUN_SAFETY_STOP;
	}
}

struct treadmill_session_change
treadmill_session_input(struct treadmill_session *s,
			enum treadmill_session_input in)
{
	struct treadmill_session_change c = { .prev = s->run, .next = s->run };
	enum treadmill_run r = s->run;
	bool running_or_paused =
		(r == TREADMILL_RUN_RUNNING) || (r == TREADMILL_RUN_PAUSED);

	if (in == TREADMILL_SESSION_KEY_OUT) {
		/* From ANY state, the latched one included: a second pull is
		 * still an event a queued request must not survive. */
		s->key_in = false;
		s->epoch++;
		s->run = TREADMILL_RUN_SAFETY_STOP;
		c.next = s->run;
		c.epoch_bumped = true;
		c.revoke_control = true;
		return c;
	}

	/* Local stops bump the epoch whatever state they find: a queued remote
	 * start must not land after a Stop pressed on an idle machine. */
	if (in == TREADMILL_SESSION_CONSOLE_STOP || in == TREADMILL_SESSION_CONSOLE_PAUSE) {
		s->epoch++;
		c.epoch_bumped = true;
	}

	if (r == TREADMILL_RUN_SAFETY_STOP) {
		s->run = safety_next(s, in);
		c.next = s->run;
		return c;
	}

	switch (in) {
	case TREADMILL_SESSION_FE_READY:
		s->run = TREADMILL_RUN_IDLE;
		break;
	case TREADMILL_SESSION_FE_IN_USE:
		/* A paused machine may still call itself IN_USE; only an
		 * explicit resume or a console Start leaves PAUSED. */
		if (r == TREADMILL_RUN_IDLE || r == TREADMILL_RUN_STOPPED) {
			s->run = TREADMILL_RUN_RUNNING;
		}
		break;
	case TREADMILL_SESSION_FE_FINISHED:
		/* FE state 4 is "FINISHED (PAUSED)": while PAUSED it means the
		 * pause, not the end of the session. */
		if (r == TREADMILL_RUN_RUNNING) {
			s->run = TREADMILL_RUN_STOPPED;
		}
		break;
	case TREADMILL_SESSION_CONSOLE_START:
		s->run = TREADMILL_RUN_RUNNING;
		break;
	case TREADMILL_SESSION_CONSOLE_STOP:
		if (running_or_paused) {
			s->run = TREADMILL_RUN_STOPPED;
		}
		break;
	case TREADMILL_SESSION_CONSOLE_PAUSE:
	case TREADMILL_SESSION_PAUSED:
		if (r == TREADMILL_RUN_RUNNING) {
			s->run = TREADMILL_RUN_PAUSED;
		}
		break;
	case TREADMILL_SESSION_RESUMED:
		if (r == TREADMILL_RUN_PAUSED) {
			s->run = TREADMILL_RUN_RUNNING;
		}
		break;
	case TREADMILL_SESSION_KEY_IN:
		s->key_in = true;
		break;
	default:
		break;
	}

	c.next = s->run;
	return c;
}

uint8_t treadmill_session_fe_state(enum treadmill_run run)
{
	switch (run) {
	case TREADMILL_RUN_IDLE:
		return TREADMILL_STATE_READY;
	case TREADMILL_RUN_RUNNING:
		return TREADMILL_STATE_IN_USE;
	default:
		return TREADMILL_STATE_FINISHED;
	}
}

static bool is_stop(const struct treadmill_motion_cmd *cmd)
{
	return (cmd->kind == TREADMILL_MOTION_STOP) ||
	       (cmd->kind == TREADMILL_MOTION_SPEED && cmd->value <= 0);
}

static bool starts_the_belt(const struct treadmill_motion_cmd *cmd)
{
	return (cmd->kind == TREADMILL_MOTION_START) ||
	       (cmd->kind == TREADMILL_MOTION_RESUME) ||
	       (cmd->kind == TREADMILL_MOTION_SPEED && cmd->value > 0);
}

int treadmill_motion_permitted(const struct treadmill_motion_cmd *cmd,
			       const struct treadmill_session *s)
{
	if (cmd == NULL || s == NULL) {
		return -EACCES;
	}

	/* STOPPING ALWAYS PASSES - before every other check, so that nothing
	 * added below can ever stand between a runner and a stopped belt. */
	if (is_stop(cmd)) {
		return 0;
	}

	if (cmd->epoch != s->epoch) {
		return -ECANCELED;
	}

	/* (d) the control token */
	if (!cmd->token) {
		return -EACCES;
	}

	/* (b) the safety latch */
	if (s->run == TREADMILL_RUN_SAFETY_STOP) {
		return -EACCES;
	}

	/* (a) the machine says remote motion is allowed: key present, remote
	 * control enabled on the console. No answer is a no. */
	if (s->policy == NULL || s->policy->remote_motion_allowed == NULL ||
	    !s->policy->remote_motion_allowed()) {
		return -EACCES;
	}

	/* (c) starting the belt from anything but RUNNING */
	if (starts_the_belt(cmd) && s->run != TREADMILL_RUN_RUNNING &&
	    !s->policy->remote_start) {
		return -EACCES;
	}

	return 0;
}
