/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_common_adapter.c - see radiant_common_adapter.h.
 */

#include <string.h>

#include "profile_common.h"
#include "radiant_common_adapter.h"

/* ---------------------------------------------------------------------------
 * The conversion constants. Exact ones first, so the two lossy ones below
 * stand out rather than blending in - the header explains at length why the
 * distinction matters and why section 3.2 is not the rule that governs them.
 * ---------------------------------------------------------------------------
 */

/* EXACT. degC -> K is +273.15, and the wire is already in 0.01 degC, so the
 * whole conversion is an integer add at the wire's own scale. */
#define COMMON_KELVIN_OFFSET_CENTI 27315

/* EXACT. 0.01 kPa = 10 Pa, so raw is the wire value unchanged at exp = +1.
 * Spelled out rather than left implicit: the temptation is to "normalise" this
 * to exp = 0 by multiplying by 10, which is the same number with one more
 * digit and one more chance to overflow. */
#define COMMON_PRESSURE_EXP 1

/* LOSSY (bounded, non-compounding - see the header). 0.01 km/h = 1/360 m/s and
 * 10^k/360 is never an integer, because 360 has a factor of 3 and 10^k has
 * none. Round half up at exp = -6. */
#define COMMON_WIND_SPEED_NUM   1000000
#define COMMON_WIND_SPEED_DEN   360
#define COMMON_WIND_SPEED_HALF  (COMMON_WIND_SPEED_DEN / 2)

/* LOSSY (bounded, non-compounding - see the header). 0.05 deg = 872.66462... microrad,
 * irrational, truncated to 872665e-9 and rounded half up at exp = -6. */
#define COMMON_WIND_DIR_URAD_PER_STEP 872665
#define COMMON_WIND_DIR_DEN           1000000
#define COMMON_WIND_DIR_HALF          (COMMON_WIND_DIR_DEN / 2)

void radiant_common_adapter_init(struct radiant_common_adapter *a)
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

/* Temperature, subpages 1, 7 and 8. The wire field is two's complement
 * (Table 6-17), so the cast through int16_t is load-bearing: without it a
 * reading of -5.00 degC arrives as 0xFE0C = 65036 and publishes as +377 K. */
static void post_temperature(uint32_t source, uint8_t field_id, uint16_t raw,
			     uint64_t t_us)
{
	int32_t centi = (int32_t)(int16_t)raw;

	post(source, field_id, RADIANT_FIELD_TEMPERATURE, 0u, -2,
	     (int64_t)centi + COMMON_KELVIN_OFFSET_CENTI, t_us);
}

/*
 * One slot of one page-84 message. Returns 1 if it posted, 0 if it declined -
 * and declining is a normal answer for a reserved subpage (9-254), for the
 * 0xFF "slot carries nothing" sentinel, and for the first charging-cycles
 * sighting, which has nothing to difference against yet.
 */
static uint32_t post_subfield(struct radiant_common_adapter *a, uint32_t source,
			      const struct profile_common_subfield *f,
			      uint64_t t_us)
{
	uint16_t delta;

	if (f->subpage == PROFILE_COMMON_SUBPAGE_INVALID ||
	    f->subpage > RADIANT_COMMON_SUBPAGE_MAX ||
	    f->subpage == 0u) {
		a->declined++;
		return 0u;
	}

	/* Recorded before the switch so it is recorded for every subpage the
	 * adapter understands, including the accumulating one, and so a future
	 * `continue`-shaped edit inside the switch cannot skip it. It is not a
	 * deadband - the header says why the adapter does not suppress. */
	a->have_last[f->subpage] = true;
	a->last_raw[f->subpage] = f->raw;

	switch (f->subpage) {
	case PROFILE_COMMON_SUBPAGE_TEMPERATURE:
		post_temperature(source, RADIANT_COMMON_FIELD_TEMPERATURE,
				 f->raw, t_us);
		return 1u;

	case PROFILE_COMMON_SUBPAGE_PRESSURE:
		/* EXACT: 0.01 kPa is 10 Pa, which is exp = +1 on the wire
		 * value itself. Unsigned per Table 6-17 (0 - 655.35 kPa). */
		post(source, RADIANT_COMMON_FIELD_PRESSURE,
		     RADIANT_FIELD_PRESSURE, 0u, COMMON_PRESSURE_EXP,
		     (int64_t)f->raw, t_us);
		return 1u;

	case PROFILE_COMMON_SUBPAGE_HUMIDITY:
		/* EXACT: the vocabulary's humidity unit is % and the wire is
		 * 0.01 %, so raw passes through at exp = -2. */
		post(source, RADIANT_COMMON_FIELD_HUMIDITY,
		     RADIANT_FIELD_HUMIDITY, 0u, -2, (int64_t)f->raw, t_us);
		return 1u;

	case PROFILE_COMMON_SUBPAGE_WIND_SPEED:
		/* LOSSY, instantaneous, non-compounding. */
		post(source, RADIANT_COMMON_FIELD_WIND_SPEED,
		     RADIANT_FIELD_SPEED, 0u, -6,
		     ((int64_t)f->raw * COMMON_WIND_SPEED_NUM +
		      COMMON_WIND_SPEED_HALF) / COMMON_WIND_SPEED_DEN,
		     t_us);
		return 1u;

	case PROFILE_COMMON_SUBPAGE_WIND_DIRECTION:
		/* LOSSY, instantaneous, non-compounding. */
		post(source, RADIANT_COMMON_FIELD_WIND_DIRECTION,
		     RADIANT_FIELD_ANGLE, 0u, -6,
		     ((int64_t)f->raw * COMMON_WIND_DIR_URAD_PER_STEP +
		      COMMON_WIND_DIR_HALF) / COMMON_WIND_DIR_DEN,
		     t_us);
		return 1u;

	case PROFILE_COMMON_SUBPAGE_CHARGE_CYCLES:
		/* The one accumulating quantity on this page. Differenced at
		 * its own u16 wire width, where the subtraction wraps
		 * correctly by construction (section 3.2/5's rule); the first
		 * sighting only establishes prev. */
		if (!a->have_prev_cycles) {
			a->have_prev_cycles = true;
			a->prev_cycles = f->raw;
			return 0u;
		}
		delta = (uint16_t)(f->raw - a->prev_cycles);
		a->prev_cycles = f->raw;
		/* Accumulated, and the ACCUMULATION is what is published -
		 * RADIANT_SAMPLE_ACCUMULATING means "raw is a monotone
		 * counter, not an instant" (radiant_bridge.h), and
		 * radiant_rules.c differences what it is handed. Publishing
		 * `delta` would be differencing an already-differenced value.
		 * Same discipline as radiant_hr_adapter.h's acc_1024. */
		a->acc_cycles += (uint64_t)delta;
		post(source, RADIANT_COMMON_FIELD_CHARGE_CYCLES,
		     RADIANT_FIELD_EVENT_COUNT, RADIANT_SAMPLE_ACCUMULATING, 0,
		     (int64_t)a->acc_cycles, t_us);
		return 1u;

	case PROFILE_COMMON_SUBPAGE_TEMP_MIN:
		/* Table 6-17 NAMES this the minimum and DESCRIBES it as the
		 * maximum; subpage 8 is the mirror image. The names are
		 * authoritative - profile_common.h defect (a). */
		post_temperature(source, RADIANT_COMMON_FIELD_TEMP_MIN, f->raw,
				 t_us);
		return 1u;

	case PROFILE_COMMON_SUBPAGE_TEMP_MAX:
		post_temperature(source, RADIANT_COMMON_FIELD_TEMP_MAX, f->raw,
				 t_us);
		return 1u;

	default:
		/* Unreachable: the range test above already covers 0, 9-254
		 * and 255. Present so a future subpage added to the catalogue
		 * without a case here declines rather than falls through. */
		a->declined++;
		return 0u;
	}
}

bool radiant_common_adapter_is_common_page(uint8_t page)
{
	return page >= PROFILE_COMMON_PAGE_RANGE_FIRST &&
	       page <= PROFILE_COMMON_PAGE_RANGE_LAST;
}

uint32_t radiant_common_adapter_decode(struct radiant_common_adapter *a, uint32_t source,
				       const uint8_t body8[8], uint64_t t_us)
{
	struct profile_common_84 d;
	uint32_t n = 0u;
	uint32_t i;

	if (a == NULL || body8 == NULL) {
		return 0u;
	}

	/* Page 84 is the only common page carrying measurements; 80/81/82/83
	 * and 85/86/87 are identity, battery, time, memory, pairing and error
	 * reporting, and the sample bus carries measurements. Returning 0 for
	 * those is the answer, not a gap - see the header. */
	if (body8[0] != PROFILE_COMMON_PAGE_84) {
		return 0u;
	}

	if (profile_common_decode_84(body8, &d) != 0) {
		return 0u;
	}

	for (i = 0u; i < PROFILE_COMMON_84_SLOTS; i++) {
		n += post_subfield(a, source, &d.slot[i], t_us);
	}

	return n;
}
