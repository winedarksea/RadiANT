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

	/* Instantaneous bpm (byte 7), posted unconditionally: 0 is a valid
	 * RADIANT_FIELD_HEART_RATE value here (unlike PROFILE_HR_INVALID_BPM),
	 * so a sink needing liveness must use the accumulators instead
	 * (section 6.1's rule). */
	post(source, RADIANT_HR_FIELD_COMPUTED_BPM, RADIANT_FIELD_HEART_RATE,
	     0u, 0, (int64_t)computed_hr, t_us);
	n++;

	if (!a->have_prev) {
		a->have_prev = true;
		a->prev_event_time = event_time;
		a->prev_beat_count = beat_count;
		return n;
	}

	/* Difference in the field's own wire width first (section 3.2/5):
	 * unsigned subtraction at that width wraps correctly by construction,
	 * so 8/16-bit wraparound needs no special case. Converting before
	 * differencing is the trap; neither line here does that.
	 *
	 * What is PUBLISHED is the running total, not that delta - the same
	 * construction as acc_1024 below, and now the same as
	 * radiant_power_adapter.c's acc_events. This line used to post the
	 * per-message delta, which meant a sink obeying radiant_bridge.h's
	 * RADIANT_SAMPLE_ACCUMULATING contract ("raw is a monotone counter, not
	 * an instant") was differencing an already-differenced value: at a
	 * steady heart rate consecutive deltas are equal, so the second
	 * difference is zero and the series read as never advancing. */
	a->acc_beats += (uint64_t)(uint8_t)(beat_count - a->prev_beat_count);
	post(source, RADIANT_HR_FIELD_BEAT_COUNT, RADIANT_FIELD_EVENT_COUNT,
	     RADIANT_SAMPLE_ACCUMULATING, 0, (int64_t)a->acc_beats, t_us);
	n++;

	/* The 1/1024s trap (section 3.2): acc_1024 accumulates the exact raw
	 * delta forever and is only converted to ms at publication. Converting
	 * each reading to ms before differencing would drift unboundedly;
	 * this stays bounded to <1ms total error, forever. */
	a->acc_1024 += (uint64_t)(uint16_t)(event_time - a->prev_event_time);
	post(source, RADIANT_HR_FIELD_BEAT_TIME_MS, RADIANT_FIELD_DURATION,
	     RADIANT_SAMPLE_ACCUMULATING, -3,
	     (int64_t)((a->acc_1024 * 1000u) / 1024u), t_us);
	n++;

	a->prev_event_time = event_time;
	a->prev_beat_count = beat_count;

	return n;
}
