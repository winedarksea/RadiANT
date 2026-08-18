/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_power_adapter.c - see radiant_power_adapter.h, which carries the
 * 0x30 energy decision, the field_id allocation and the reserved-gap rationale.
 */

#include <string.h>

#include "profile_fec.h"
#include "profile_power.h"
#include "radiant_power_adapter.h"

void radiant_power_adapter_init(struct radiant_power_adapter *a)
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

/*
 * The half both device types share: instantaneous watts, the event-count
 * accumulator and the energy integral. `inst_w` and `event_count` have already
 * been pulled out of whichever page carried them, and the caller has already
 * checked that the sensor considers them valid.
 *
 * Returns samples posted: 1 on the call that establishes the baseline, 3
 * afterwards.
 */
static uint32_t post_power(struct radiant_power_adapter *a, uint32_t source,
			   uint16_t inst_w, uint8_t event_count, uint64_t t_us,
			   uint8_t page_flags)
{
	uint32_t n = 0u;

	/* Instantaneous watts, posted unconditionally. 0 W is a real reading
	 * (coasting), so a sink needing liveness must use the accumulators
	 * instead - docs/radiant-bridge.md section 6.1's rule, the same one
	 * radiant_hr_adapter.c states for a computed bpm of 0. */
	post(source, RADIANT_POWER_FIELD_INST_POWER, RADIANT_FIELD_ACTIVE_POWER,
	     page_flags, 0, (int64_t)inst_w, t_us);
	n++;

	if (!a->have_prev) {
		a->have_prev = true;
		a->prev_event_count = event_count;
		a->prev_t_us = t_us;
		return n;
	}

	/* Difference in the field's own wire width first (section 3.2/5): both
	 * profiles roll this counter at 256, and unsigned subtraction at that
	 * width wraps correctly by construction. Widening event_count before
	 * subtracting is the trap - it turns one wrap into a delta of -255. */
	a->acc_events += (uint64_t)(uint8_t)(event_count - a->prev_event_count);
	a->prev_event_count = event_count;
	post(source, RADIANT_POWER_FIELD_EVENT_COUNT, RADIANT_FIELD_EVENT_COUNT,
	     (uint8_t)(RADIANT_SAMPLE_ACCUMULATING | page_flags), 0,
	     (int64_t)a->acc_events, t_us);
	n++;

	/*
	 * The energy integral (see the header at length). 1 W x 1 us == 1 uJ
	 * exactly, so this is a unit identity and nothing rounds; the division
	 * by 10^6 happens in the consumer, via exp = -6.
	 *
	 * A non-advancing or backwards t_us contributes nothing rather than a
	 * negative or wrapped interval. The bus's t_us is a monotone t_sync, so
	 * this should not happen - but "should not" is how an unbounded uint64
	 * accumulator gets a garbage term it can never lose.
	 */
	if (t_us > a->prev_t_us) {
		a->acc_uj += (uint64_t)inst_w * (t_us - a->prev_t_us);
	}
	a->prev_t_us = t_us;
	post(source, RADIANT_POWER_FIELD_ENERGY, RADIANT_FIELD_ENERGY,
	     (uint8_t)(RADIANT_SAMPLE_ACCUMULATING | page_flags), -6,
	     (int64_t)a->acc_uj, t_us);
	n++;

	return n;
}

uint32_t radiant_power_adapter_decode(struct radiant_power_adapter *a,
				      uint32_t source, const uint8_t body8[8],
				      uint64_t t_us)
{
	struct profile_power_std p;

	if (a == NULL || body8 == NULL) {
		return 0u;
	}

	/* Every page other than 0x10 declines here, in the decoder, rather than
	 * being filtered by a page test in this file: one implementation of
	 * "is this that page" is one place for it to be wrong. */
	if (profile_power_decode_std(body8, &p) != 0) {
		return 0u;
	}

	/* Page 0x10 has no validity flag on either power field - Table 8-1
	 * gives them no invalid value, unlike FE-C's 0xFFF. */
	/* Page 0x10 IS the bicycle-power profile's every-message page, so these
	 * three series repeat at the channel period and an expiry computed from
	 * that period is right for them. No RADIANT_SAMPLE_ROTATED. */
	return post_power(a, source, p.inst_power_w, p.event_count, t_us, 0u);
}

static uint32_t decode_fec_general(uint32_t source,
				   const struct profile_fec_general *g,
				   uint64_t t_us)
{
	uint32_t n = 0u;

	/* THE EQUIPMENT TYPE FIRST, and the order is the point: a sink that
	 * creates its endpoint on this page has the subtype in hand before the
	 * state sample gets there, so the endpoint is named "Treadmill 51234 In
	 * Use" the first time rather than being renamed a second later. See
	 * radiant_power_adapter.h's RADIANT_POWER_FIELD_FEC_TYPE block. */
	post(source, RADIANT_POWER_FIELD_FEC_TYPE, RADIANT_FIELD_ENUM_GENERIC,
	     RADIANT_SAMPLE_ROTATED, 0, (int64_t)g->equipment_type, t_us);
	n++;

	/* The equipment state, as an enum rather than a boolean. Turning
	 * IN_USE into "bike in use" here would put a rule in an adapter;
	 * radiant_rules.c owns derived booleans and marks them DERIVED, and
	 * this sample is a decoded measurement, so it carries no such flag. */
	post(source, RADIANT_POWER_FIELD_FEC_STATE, RADIANT_FIELD_ENUM_GENERIC,
	     RADIANT_SAMPLE_ROTATED, 0, (int64_t)g->state, t_us);
	n++;

	/* 0.001 m/s is exactly 10^-3 m/s, so the wire value IS the raw at
	 * exp -3 and nothing is converted. Virtual speed (capabilities bit 3)
	 * is still posted: it is what the equipment reports and the flag that
	 * says so has no home in the vocabulary, so suppressing it would be a
	 * silent hole rather than a caveat. */
	if (g->speed_valid) {
		post(source, RADIANT_POWER_FIELD_FEC_SPEED, RADIANT_FIELD_SPEED,
		     RADIANT_SAMPLE_ROTATED, -3, (int64_t)g->speed_mm_s, t_us);
		n++;
	}

	return n;
}

uint32_t radiant_power_adapter_decode_fec(struct radiant_power_adapter *a,
					  uint32_t source,
					  const uint8_t body8[8], uint64_t t_us)
{
	struct profile_fec_general g;
	struct profile_fec_trainer tr;

	if (a == NULL || body8 == NULL) {
		return 0u;
	}

	if (profile_fec_decode_general(body8, &g) == 0) {
		return decode_fec_general(source, &g, t_us);
	}

	if (profile_fec_decode_trainer(body8, &tr) == 0) {
		if (!tr.power_valid) {
			/*
			 * Table 8-25's 0xFFF condemns the accumulator in bytes
			 * 3-4 as well as the instantaneous field, so there is
			 * no wattage here to publish or to integrate.
			 *
			 * The BASELINE IS STILL ADVANCED, which is the part
			 * that is easy to get backwards. Leaving prev_t_us
			 * behind would make the next valid message charge the
			 * entire unknown stretch at its own wattage - a rider
			 * who stopped for a minute would be billed a minute of
			 * their restart power. Advancing it charges the
			 * unknown stretch nothing, which is the honest answer
			 * for "the equipment declined to say".
			 *
			 * The event count is real regardless (section 8.6.7.1:
			 * no invalid values), so it keeps accumulating; it is
			 * simply not published until the next message, since
			 * the three series are posted together.
			 */
			if (a->have_prev) {
				a->acc_events += (uint64_t)(uint8_t)
					(tr.event_count - a->prev_event_count);
			} else {
				a->have_prev = true;
			}
			a->prev_event_count = tr.event_count;
			a->prev_t_us = t_us;
			return 0u;
		}
		/* FE-C page 25, one page of an interleaved rotation - unlike
		 * page 0x10 above, which is why the flag is a parameter. */
		return post_power(a, source, tr.inst_power_w, tr.event_count,
				  t_us, RADIANT_SAMPLE_ROTATED);
	}

	return 0u;
}
