/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_hr_adapter.c - see radiant_hr_adapter.h.
 */

#include <string.h>

#include "radiant_hr_adapter.h"

void radiant_hr_adapter_init(struct radiant_hr_adapter *a)
{
	if (a == NULL) {
		return;
	}
	memset(a, 0, sizeof(*a));
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

uint32_t radiant_hr_adapter_decode(struct radiant_hr_adapter *a, uint32_t source,
				const uint8_t body8[8], uint64_t t_us)
{
	uint16_t event_time;
	uint8_t  beat_count;
	uint8_t  computed_hr;
	uint32_t n = 0u;

	if (a == NULL || body8 == NULL) {
		return 0u;
	}

	event_time = (uint16_t)body8[4] | ((uint16_t)body8[5] << 8);
	beat_count = body8[6];
	computed_hr = body8[7];

	/*
	 * Instantaneous: the sensor's own bpm answer, byte 7 - unconditional,
	 * unlike the two accumulators below. profile_hr.h names PROFILE_HR_-
	 * INVALID_BPM 0u as "no reading, and NOT the common 0xFF sentinel";
	 * this adapter does not special-case it, because 0 is itself a valid
	 * RADIANT_FIELD_HEART_RATE value on this bus and a sink that cares
	 * about the distinction (section 6.1's liveness rule, for instance)
	 * has the accumulators to test instead - "use the accumulator, never
	 * the instantaneous value" is section 6.1's own rule, stated for
	 * exactly this reason.
	 */
	post(source, RADIANT_HR_FIELD_COMPUTED_BPM, RADIANT_FIELD_HEART_RATE,
	     0u, 0, (int64_t)computed_hr, t_us);
	n++;

	if (!a->have_prev) {
		a->have_prev = true;
		a->prev_event_time = event_time;
		a->prev_beat_count = beat_count;
		return n;
	}

	/*
	 * Both accumulators DIFFERENCE IN THE FIELD'S OWN WIRE WIDTH FIRST,
	 * per section 3.2 and section 5 of the envelope: unsigned subtraction
	 * at the field's own width wraps correctly by construction (C's
	 * modular unsigned arithmetic IS the delta_u8/delta_u16 the envelope
	 * asks for), so a strap's own 8-bit and 16-bit wraparounds are exact
	 * with no special-cased branch - the trap the section warns about is
	 * converting BEFORE differencing, and neither line here does that.
	 */
	post(source, RADIANT_HR_FIELD_BEAT_COUNT, RADIANT_FIELD_EVENT_COUNT,
	     RADIANT_SAMPLE_ACCUMULATING, 0,
	     (int64_t)(uint8_t)(beat_count - a->prev_beat_count), t_us);
	n++;

	/*
	 * THE 1/1024 S TRAP (section 3.2). acc_1024 accumulates the exact
	 * wire-width delta forever, u64, never itself converted; only the
	 * PUBLISHED value is computed fresh from it on every message. The
	 * wrong construction - converting each reading to ms and then
	 * differencing the converted values - drifts without bound; this one
	 * is bounded at one millisecond total, forever, per the section's own
	 * proof.
	 */
	a->acc_1024 += (uint64_t)(uint16_t)(event_time - a->prev_event_time);
	post(source, RADIANT_HR_FIELD_BEAT_TIME_MS, RADIANT_FIELD_DURATION,
	     RADIANT_SAMPLE_ACCUMULATING, -3,
	     (int64_t)((a->acc_1024 * 1000u) / 1024u), t_us);
	n++;

	a->prev_event_time = event_time;
	a->prev_beat_count = beat_count;

	return n;
}
