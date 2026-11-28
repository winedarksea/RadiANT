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
	/* UNKNOWN until the source reports it: no motion until then. */
	s->key = TREADMILL_KEY_UNKNOWN;
	s->belt_mm_s = 0u;
	s->stop_commanded = false;
	s->policy = policy;
}

void treadmill_session_set_belt(struct treadmill_session *s, uint32_t mm_s)
{
	s->belt_mm_s = mm_s;
}

void treadmill_session_note_stop(struct treadmill_session *s)
{
	s->stop_commanded = true;
}

void treadmill_session_note_start(struct treadmill_session *s)
{
	s->stop_commanded = false;
}

bool treadmill_session_moving(const struct treadmill_session *s)
{
	return (s->run == TREADMILL_RUN_RUNNING) && (s->belt_mm_s > 0u) &&
	       !s->stop_commanded;
}

/* The latched state: only key-in plus a console action leaves it. */
static enum treadmill_run safety_next(struct treadmill_session *s,
				      enum treadmill_session_input in)
{
	bool key_in = (s->key == TREADMILL_KEY_IN);

	switch (in) {
	case TREADMILL_SESSION_KEY_IN:
		s->key = TREADMILL_KEY_IN;
		return TREADMILL_RUN_SAFETY_STOP;
	case TREADMILL_SESSION_CONSOLE_START:
		return key_in ? TREADMILL_RUN_RUNNING : TREADMILL_RUN_SAFETY_STOP;
	case TREADMILL_SESSION_CONSOLE_STOP:
		return key_in ? TREADMILL_RUN_IDLE : TREADMILL_RUN_SAFETY_STOP;
	default:
		/* State reports, remote pause/resume echoes, console pause, a
		 * remote Reset: none is the console action the latch waits for. */
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
		s->key = TREADMILL_KEY_OUT;
		s->epoch++;
		s->run = TREADMILL_RUN_SAFETY_STOP;
		c.next = s->run;
		c.epoch_bumped = true;
		c.revoke_control = true;
		return c;
	}

	/* Local stops and a remote Reset bump the epoch whatever state they
	 * find: a queued remote start must not land after either. */
	if (in == TREADMILL_SESSION_CONSOLE_STOP || in == TREADMILL_SESSION_CONSOLE_PAUSE ||
	    in == TREADMILL_SESSION_REMOTE_RESET) {
		s->epoch++;
		c.epoch_bumped = true;
	}

	if (r == TREADMILL_RUN_SAFETY_STOP) {
		s->run = safety_next(s, in);
	} else {
		switch (in) {
		case TREADMILL_SESSION_FE_READY:
			s->run = TREADMILL_RUN_IDLE;
			break;
		case TREADMILL_SESSION_FE_IN_USE:
			/* A paused machine may still call itself IN_USE; only
			 * an explicit resume or a console Start leaves PAUSED. */
			if (r == TREADMILL_RUN_IDLE || r == TREADMILL_RUN_STOPPED) {
				s->run = TREADMILL_RUN_RUNNING;
			}
			break;
		case TREADMILL_SESSION_FE_FINISHED:
			/* FE state 4 is "FINISHED (PAUSED)": while PAUSED it
			 * means the pause, not the end of the session. */
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
			s->key = TREADMILL_KEY_IN;
			break;
		case TREADMILL_SESSION_REMOTE_RESET:
			/* FTMS §4.16.2: Training Status goes to Idle. */
			s->run = TREADMILL_RUN_IDLE;
			break;
		default:
			break;
		}
	}

	/* A session that (re)enters RUNNING, or a console Start, is a fresh
	 * intent to move: a remote stop commanded before it no longer counts. */
	if ((s->run == TREADMILL_RUN_RUNNING && r != TREADMILL_RUN_RUNNING) ||
	    in == TREADMILL_SESSION_CONSOLE_START) {
		s->stop_commanded = false;
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

static bool moves_the_deck(const struct treadmill_motion_cmd *cmd)
{
	return (cmd->kind == TREADMILL_MOTION_INCLINE) ||
	       (cmd->kind == TREADMILL_MOTION_RESET_INCLINE) ||
	       (cmd->kind == TREADMILL_MOTION_SIM_GRADE);
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

	/* (b) the safety latch, and a key the machine has not yet reported */
	if (s->run == TREADMILL_RUN_SAFETY_STOP || s->key != TREADMILL_KEY_IN) {
		return -EACCES;
	}

	/* (a) the machine says remote motion is allowed: key present, remote
	 * control enabled on the console. No answer is a no. */
	if (s->policy == NULL || s->policy->remote_motion_allowed == NULL ||
	    !s->policy->remote_motion_allowed()) {
		return -EACCES;
	}

	/* (c) starting the belt, or moving the deck, on a machine whose belt
	 * is not moving: RUNNING, belt speed reported above 0, and no stop
	 * accepted since. */
	if ((starts_the_belt(cmd) || moves_the_deck(cmd)) && !treadmill_session_moving(s) &&
	    !s->policy->remote_start) {
		return -EACCES;
	}

	return 0;
}
