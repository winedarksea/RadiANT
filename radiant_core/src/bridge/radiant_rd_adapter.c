/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_rd_adapter.c - see radiant_rd_adapter.h.
 */

#include <string.h>

#include "radiant_rd_adapter.h"

/* Exact wire-scale to SI-decimal (raw, exp) conversions, no rounding:
 * 1/4mm = 250um, 1/4% = 25*10^-2%, 1/32% = 3125*10^-5%. */
#define RD_UM_PER_QUARTER_MM   250
#define RD_QUARTER_PCT_SCALE   25    /* with exp -2 */
#define RD_THIRTYSECOND_SCALE  3125  /* with exp -5 */

void radiant_rd_adapter_init(struct radiant_rd_adapter *a)
{
	if (a == NULL) {
		return;
	}
	memset(a, 0, sizeof(*a));
	a->m.step_count = 0u;
}

static void post(uint32_t source, uint8_t field_id, uint8_t field_type,
		 uint8_t flags, int8_t exp, int64_t raw, uint64_t t_us)
{
	struct radiant_sample s = {
		.source = source,
		.field_id = field_id,
		.field_type = field_type,
		.flags = flags,
		.exp = exp,
		.raw = raw,
		.t_us = t_us,
	};

	radiant_bridge_post(&s);
}

static uint32_t decode_a(struct radiant_rd_adapter *a, uint32_t source,
			 const uint8_t *body8, uint64_t t_us)
{
	uint32_t n = 0u;
	uint8_t  steps;

	if (profile_rd_decode_a(body8, &a->m, NULL) != 0) {
		return 0u;
	}
	a->seen_a = true;

	/* An invalid field is not published as a zero: "no motion detected"
	 * and "zero mm of vertical oscillation" are different statements a
	 * sink can't tell apart afterward. */
	if (a->m.vert_osc_quarter_mm != PROFILE_RD_INVALID_VERTICAL_OSC) {
		post(source, RADIANT_RD_FIELD_VERTICAL_OSC,
		     RADIANT_FIELD_LENGTH, 0u, -6,
		     (int64_t)a->m.vert_osc_quarter_mm * RD_UM_PER_QUARTER_MM,
		     t_us);
		n++;
	}

	if (a->m.stance_quarter != PROFILE_RD_INVALID_STANCE) {
		post(source, RADIANT_RD_FIELD_STANCE_TIME,
		     RADIANT_FIELD_PERCENTAGE, 0u, -2,
		     (int64_t)a->m.stance_quarter * RD_QUARTER_PCT_SCALE, t_us);
		n++;
	}

	/* Cadence and ground contact time are decoded into a->m above but
	 * deliberately not posted - see the header. */

	if (!a->have_prev_steps) {
		a->have_prev_steps = true;
		a->prev_step_count = a->m.step_count;
		return n;
	}

	/* Differenced in the field's own 7-bit width: masking after the
	 * subtraction makes the wrap exact; a plain u8 subtraction would
	 * report 128 extra steps once per rollover. */
	steps = (uint8_t)((a->m.step_count - a->prev_step_count) & 0x7Fu);
	a->prev_step_count = a->m.step_count;
	if (steps != 0u) {
		post(source, RADIANT_RD_FIELD_STEP_COUNT,
		     RADIANT_FIELD_EVENT_COUNT, RADIANT_SAMPLE_ACCUMULATING, 0,
		     (int64_t)steps, t_us);
		n++;
	}
	return n;
}

static uint32_t decode_b(struct radiant_rd_adapter *a, uint32_t source,
			 const uint8_t *body8, uint64_t t_us)
{
	uint32_t n = 0u;

	if (profile_rd_decode_b(body8, &a->m, NULL) != 0) {
		return 0u;
	}
	a->seen_b = true;

	if (a->m.balance_32 != PROFILE_RD_INVALID_BALANCE) {
		post(source, RADIANT_RD_FIELD_BALANCE, RADIANT_FIELD_PERCENTAGE,
		     0u, -5,
		     (int64_t)a->m.balance_32 * RD_THIRTYSECOND_SCALE, t_us);
		n++;
	}

	/* Vertical ratio has no stated invalid value, so it's published
	 * unconditionally, including a genuine zero. */
	post(source, RADIANT_RD_FIELD_VERT_RATIO, RADIANT_FIELD_PERCENTAGE, 0u,
	     -5, (int64_t)a->m.vert_ratio_32 * RD_THIRTYSECOND_SCALE, t_us);
	n++;

	post(source, RADIANT_RD_FIELD_STEP_LENGTH, RADIANT_FIELD_LENGTH, 0u, -3,
	     (int64_t)a->m.step_length_mm, t_us);
	n++;
	return n;
}

uint32_t radiant_rd_adapter_decode(struct radiant_rd_adapter *a, uint32_t source,
				   const uint8_t body8[8], uint64_t t_us)
{
	if (a == NULL || body8 == NULL) {
		return 0u;
	}

	switch (body8[0]) {
	case PROFILE_RD_PAGE_A:
		return decode_a(a, source, body8, t_us);
	case PROFILE_RD_PAGE_B:
		return decode_b(a, source, body8, t_us);
	default:
		/* 0x10, 0x20, 0x4A are the display's half of the conversation
		 * / identity pages, not measurements - don't belong on the bus. */
		return 0u;
	}
}
