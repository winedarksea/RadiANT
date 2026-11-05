/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_ackfault.c - see trainer_ackfault.h. No Zephyr header: the include
 * list is the check that tests/model can compile this file on its own.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "trainer_ackfault.h"

/* The dongle's ACK window half-width, RADIANT_TRANSFER_ACK_GUARD_US. Spelled
 * here rather than included so this file stays free of radiant; `late-edge`
 * is defined as "exactly on the window's late edge", and if the window
 * changes this preset should move with it. */
#define DONGLE_ACK_GUARD_US 250

static uint32_t xorshift32(uint32_t *s)
{
	uint32_t x = *s;

	x ^= x << 13;
	x ^= x >> 17;
	x ^= x << 5;
	*s = x;
	return x;
}

static bool draw_pm(struct trainer_ackfault *af, uint16_t pm)
{
	if (pm == 0u) {
		return false; /* no draw: a zero rate must not move the stream */
	}
	return (xorshift32(&af->rng) % 1000u) < (uint32_t)pm;
}

void trainer_ackfault_params_ideal(struct trainer_ackfault_params *p)
{
	memset(p, 0, sizeof(*p));
	p->bad_action = TRAINER_ACTION_MUTE;
	p->seed = TRAINER_ACKFAULT_SEED_DEFAULT;
}

int trainer_ackfault_preset(const char *name, struct trainer_ackfault_params *p)
{
	struct trainer_ackfault_params q;

	if (name == NULL || p == NULL) {
		return -1;
	}
	trainer_ackfault_params_ideal(&q);

	if (strcmp(name, "ideal") == 0) {
		/* nothing */
	} else if (strcmp(name, "late300") == 0) {
		q.offset_us = 300;
	} else if (strcmp(name, "late-edge") == 0) {
		q.offset_us = DONGLE_ACK_GUARD_US;
	} else if (strcmp(name, "jitter") == 0) {
		q.jitter_us = 200u;
	} else if (strcmp(name, "lossy25") == 0) {
		q.mute_pm = 250u;
	} else if (strcmp(name, "wahoo") == 0) {
		/* PLACEHOLDER. Equal to ideal until the sniffer calibration
		 * (plan Phase 6) measures the real trainer's data->ACK dt and
		 * ACK-presence rate. Anything here before then would be a
		 * guess presented as a model. */
	} else {
		return -1;
	}

	*p = q;
	return 0;
}

void trainer_ackfault_reset(struct trainer_ackfault *af)
{
	struct trainer_ackfault_params p = af->p;

	memset(af, 0, sizeof(*af));
	af->p = p;
	af->rng = (p.seed != 0u) ? p.seed : TRAINER_ACKFAULT_SEED_DEFAULT;
}

void trainer_ackfault_init(struct trainer_ackfault *af,
			   const struct trainer_ackfault_params *p)
{
	af->p = *p;
	trainer_ackfault_reset(af);
}

void trainer_ackfault_decide(struct trainer_ackfault *af, uint64_t t_us,
			     struct trainer_ackfault_decision *out)
{
	const struct trainer_ackfault_params *p = &af->p;
	bool busy = af->busy_valid && t_us < af->busy_until_us;

	out->action = TRAINER_ACTION_ACK;
	out->cause = TRAINER_CAUSE_NONE;
	out->offset_us = 0;

	/* The chain steps on every packet, busy or not, so a busy window does
	 * not freeze a burst in progress. */
	if (p->p_bad_pm != 0u || af->burst_bad) {
		if (af->burst_bad) {
			if (draw_pm(af, p->p_good_pm)) {
				af->burst_bad = false;
			}
		} else if (draw_pm(af, p->p_bad_pm)) {
			af->burst_bad = true;
		}
	}

	if (busy) {
		out->action = TRAINER_ACTION_MUTE;
		out->cause = TRAINER_CAUSE_BUSY;
	} else if (af->burst_bad) {
		out->action = (p->bad_action == TRAINER_ACTION_DEAF)
				      ? TRAINER_ACTION_DEAF
				      : TRAINER_ACTION_MUTE;
		out->cause = TRAINER_CAUSE_BURST;
	} else if (draw_pm(af, p->deaf_pm)) {
		out->action = TRAINER_ACTION_DEAF;
		out->cause = TRAINER_CAUSE_DEAF;
	} else if (draw_pm(af, p->mute_pm)) {
		out->action = TRAINER_ACTION_MUTE;
		out->cause = TRAINER_CAUSE_MUTE;
	} else {
		int32_t j = 0;

		if (p->jitter_us != 0u) {
			uint32_t span = 2u * p->jitter_us + 1u;

			j = (int32_t)(xorshift32(&af->rng) % span) -
			    (int32_t)p->jitter_us;
		}
		out->offset_us = p->offset_us + j;
	}

	/* Applied and not busy: the trainer is now busy for busy_ms. */
	if (p->busy_ms != 0u && !busy && out->action != TRAINER_ACTION_DEAF) {
		af->busy_valid = true;
		af->busy_until_us = t_us + (uint64_t)p->busy_ms * 1000u;
	}

	af->n++;
	af->n_action[out->action]++;
	af->n_cause[out->cause]++;
}

static int parse_i32(const char *s, int32_t lo, int32_t hi, int32_t *out)
{
	char *end = NULL;
	long  v;

	if (s == NULL || *s == '\0') {
		return -1;
	}
	v = strtol(s, &end, 0);
	if (end == NULL || *end != '\0' || v < (long)lo || v > (long)hi) {
		return -1;
	}
	*out = (int32_t)v;
	return 0;
}

int trainer_ackfault_set(struct trainer_ackfault_params *p, const char *key,
			 const char *value)
{
	int32_t v;

	if (p == NULL || key == NULL || value == NULL) {
		return -1;
	}

	if (strcmp(key, "bad_action") == 0) {
		if (strcmp(value, "mute") == 0) {
			p->bad_action = TRAINER_ACTION_MUTE;
		} else if (strcmp(value, "deaf") == 0) {
			p->bad_action = TRAINER_ACTION_DEAF;
		} else {
			return -1;
		}
		return 0;
	}
	if (strcmp(key, "seed") == 0) {
		char         *end = NULL;
		unsigned long s = strtoul(value, &end, 0);

		if (*value == '\0' || end == NULL || *end != '\0') {
			return -1;
		}
		p->seed = (uint32_t)s;
		return 0;
	}

	/* Offsets beyond +-10 ms are not an ACK timing any more, they are a
	 * different slot; per-mille stops at 1000. */
	if (strcmp(key, "offset_us") == 0) {
		if (parse_i32(value, -10000, 10000, &v) != 0) {
			return -1;
		}
		p->offset_us = v;
	} else if (strcmp(key, "jitter_us") == 0) {
		if (parse_i32(value, 0, 10000, &v) != 0) {
			return -1;
		}
		p->jitter_us = (uint32_t)v;
	} else if (strcmp(key, "mute_pm") == 0) {
		if (parse_i32(value, 0, 1000, &v) != 0) {
			return -1;
		}
		p->mute_pm = (uint16_t)v;
	} else if (strcmp(key, "deaf_pm") == 0) {
		if (parse_i32(value, 0, 1000, &v) != 0) {
			return -1;
		}
		p->deaf_pm = (uint16_t)v;
	} else if (strcmp(key, "p_bad") == 0) {
		if (parse_i32(value, 0, 1000, &v) != 0) {
			return -1;
		}
		p->p_bad_pm = (uint16_t)v;
	} else if (strcmp(key, "p_good") == 0) {
		if (parse_i32(value, 0, 1000, &v) != 0) {
			return -1;
		}
		p->p_good_pm = (uint16_t)v;
	} else if (strcmp(key, "busy_ms") == 0) {
		if (parse_i32(value, 0, 600000, &v) != 0) {
			return -1;
		}
		p->busy_ms = (uint32_t)v;
	} else {
		return -1;
	}
	return 0;
}

const char *trainer_ackfault_action_name(uint8_t action)
{
	switch (action) {
	case TRAINER_ACTION_ACK:  return "ack";
	case TRAINER_ACTION_MUTE: return "mute";
	case TRAINER_ACTION_DEAF: return "deaf";
	default:                  return "?";
	}
}

const char *trainer_ackfault_cause_name(uint8_t cause)
{
	switch (cause) {
	case TRAINER_CAUSE_NONE:  return "none";
	case TRAINER_CAUSE_BUSY:  return "busy";
	case TRAINER_CAUSE_BURST: return "burst";
	case TRAINER_CAUSE_DEAF:  return "deaf";
	case TRAINER_CAUSE_MUTE:  return "mute";
	default:                  return "?";
	}
}
