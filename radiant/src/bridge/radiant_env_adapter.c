/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_env_adapter.c - see radiant_env_adapter.h.
 */

#include <string.h>

#include "radiant_env_adapter.h"

void radiant_env_adapter_init(struct radiant_env_adapter *a)
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

uint32_t radiant_env_adapter_decode(struct radiant_env_adapter *a, uint32_t source,
				    const uint8_t body8[8], uint64_t t_us)
{
	struct profile_env_temperature t;
	uint32_t n = 0u;

	if (a == NULL || body8 == NULL) {
		return 0u;
	}

	/* Page 0x00 first, because it is the cheaper test and because it is
	 * the one that returns without touching the accumulator. */
	if (profile_env_decode_capabilities(body8, &a->caps) == 0) {
		a->have_caps = true;
		return 0u;
	}

	if (profile_env_decode_temperature(body8, &t) != 0) {
		/* Reserved pages 0x02..0x3F and the common data pages (which
		 * have their own adapter, running on this same channel) both
		 * land here. Declining is a normal answer, not an error. */
		return 0u;
	}

	a->t = t;
	a->have_temperature = true;

	/*
	 * degC -> K, and the whole of it: 273.15 K is a whole number of
	 * hundredths, so adding 27315 at exp -2 is exact. Nothing rounds and
	 * nothing drifts. See the header.
	 *
	 * Posted only when the sensor said the reading is real. An invalid
	 * field must not be published as a zero here - unlike profile_rd.c,
	 * where zero IS the profile's sentinel, 0.00 degC is an ordinary
	 * winter temperature and 273.15 K is what a sink would render.
	 */
	if (t.current_valid) {
		post(source, RADIANT_ENV_FIELD_TEMPERATURE,
		     RADIANT_FIELD_TEMPERATURE, 0u, -2,
		     (int64_t)t.current_centi + RADIANT_ENV_KELVIN_OFFSET_CENTI,
		     t_us);
		n++;
	}

	/*
	 * The 24-hour low and high are decoded (they are in a->t, for a caller
	 * that wants them) and DELIBERATELY NOT POSTED. They are statistics
	 * over a day, and every consumer downstream reads a temperature record
	 * as a live measurement - two extra Matter Temperature Sensor
	 * endpoints per sensor, reading like current values. The header argues
	 * this at length; the reserved field_ids above hold their places.
	 */

	if (!a->have_prev) {
		a->have_prev = true;
		a->prev_event_count = t.event_count;
		return n;
	}

	/*
	 * Difference in the field's own wire width first (section 3.2/5):
	 * unsigned subtraction at u8 wraps correctly by construction, so the
	 * 255 -> 0 rollover needs no special case. At 0.5 Hz this counter
	 * takes over eight minutes to wrap, so a receiver that misses a whole
	 * wrap has been away far longer than any staleness threshold.
	 *
	 * THE DELTA IS ACCUMULATED AND THE RUNNING TOTAL IS PUBLISHED, not the
	 * delta itself. RADIANT_SAMPLE_ACCUMULATING is defined in
	 * radiant_bridge.h as "raw is a monotone counter, not an instant", and
	 * radiant_rules.c differences what it is given. Publishing the delta
	 * would hand it an already-differenced value, and at a steady rate
	 * consecutive deltas are EQUAL, so the second difference is zero and
	 * the source reads as permanently idle. This adapter is the worst case
	 * of that: a thermometer's event count advances by exactly 1 every
	 * message, forever, so the delta form is the constant 1 and nothing
	 * downstream could ever see it move.
	 */
	a->acc_events += (uint64_t)(uint8_t)(t.event_count - a->prev_event_count);
	post(source, RADIANT_ENV_FIELD_EVENT_COUNT, RADIANT_FIELD_EVENT_COUNT,
	     RADIANT_SAMPLE_ACCUMULATING, 0, (int64_t)a->acc_events, t_us);
	n++;

	a->prev_event_count = t.event_count;

	return n;
}
