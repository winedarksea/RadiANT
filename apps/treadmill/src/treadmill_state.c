/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_state.c - see treadmill_state.h for the units, the ownership rule
 * and the cadence trap.
 *
 * NO ZEPHYR HEADER, NO LOCK, NO CLOCK. That is what lets
 * apps/treadmill/tests/pages drive a whole simulated run in a ztest with no
 * radio and no kernel timing, the way apps/hrm_ble/tests/sim_sched drives the
 * beat scheduler.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "treadmill_state.h"

/* The reportable span of FE-C page 17's incline field, which is narrower than
 * page 51's commandable ±200.00 % grade. Clamping to the REPORTABLE range is
 * the right choice: a machine that accepted a +150 % command it could never
 * report back would answer every page 17 with a number that disagrees with the
 * command it just acknowledged. */
#define INCLINE_MIN_CENTI_PCT (-10000)
#define INCLINE_MAX_CENTI_PCT (10000)

/* No treadmill goes faster than this and the SDM speed field cannot express
 * it anyway (its integer part is a nibble). Clamping here means the SDM
 * encoder's own refusal is unreachable in practice rather than load-bearing. */
#define SPEED_MAX_MM_S 15996u

static int32_t clamp_incline(int32_t centi_pct)
{
	if (centi_pct < INCLINE_MIN_CENTI_PCT) {
		return INCLINE_MIN_CENTI_PCT;
	}
	if (centi_pct > INCLINE_MAX_CENTI_PCT) {
		return INCLINE_MAX_CENTI_PCT;
	}
	return centi_pct;
}

static uint32_t clamp_speed(uint32_t mm_s)
{
	return (mm_s > SPEED_MAX_MM_S) ? SPEED_MAX_MM_S : mm_s;
}

void treadmill_state_init(struct treadmill_state *s)
{
	if (s == NULL) {
		return;
	}
	memset(s, 0, sizeof(*s));
	/* READY, not ASLEEP and not IN_USE: the machine is powered and nobody
	 * is on it. This is the value phase 6's occupancy rule reads as
	 * "unoccupied", so starting anywhere else would publish a treadmill in
	 * use at power-up. */
	s->state = TREADMILL_STATE_READY;
	s->user_mass_10g = 0u; /* not supplied; see TREADMILL_DEFAULT_MASS_10G */
}

void treadmill_state_set_speed(struct treadmill_state *s, uint32_t mm_s)
{
	if (s != NULL) {
		s->belt_speed_mm_s = clamp_speed(mm_s);
	}
}

void treadmill_state_set_incline(struct treadmill_state *s, int32_t centi_pct)
{
	if (s != NULL) {
		s->incline_centi_pct = clamp_incline(centi_pct);
	}
}

int32_t treadmill_state_request_incline(struct treadmill_state *s,
					int32_t centi_pct)
{
	if (s == NULL) {
		return 0;
	}
	s->target_incline_centi_pct = clamp_incline(centi_pct);
	return s->target_incline_centi_pct;
}

uint32_t treadmill_state_request_speed(struct treadmill_state *s, uint32_t mm_s)
{
	if (s == NULL) {
		return 0u;
	}
	s->target_speed_mm_s = clamp_speed(mm_s);
	return s->target_speed_mm_s;
}

void treadmill_state_add_strides(struct treadmill_state *s, uint32_t n)
{
	if (s != NULL) {
		s->strides += n;
	}
}

void treadmill_state_set_cadence_16(struct treadmill_state *s,
				    uint32_t strides_min_16)
{
	if (s != NULL) {
		s->cadence_strides_min_16 = strides_min_16;
	}
}

void treadmill_state_set_heart_rate(struct treadmill_state *s, uint8_t bpm,
				    bool from_ant)
{
	if (s == NULL) {
		return;
	}
	s->heart_rate_bpm = bpm;
	/* Sticky once an ANT+ strap has been the source, because FE-C page
	 * 16's capabilities nibble reports WHERE the heart rate came from and
	 * flapping that field every time a strap drops a packet would have a
	 * head unit redraw its source icon four times a second. Cleared only
	 * when the reading itself goes away. */
	if (bpm == TREADMILL_HR_NONE) {
		s->hr_from_ant = false;
	} else if (from_ant) {
		s->hr_from_ant = true;
	}
}

void treadmill_state_set_state(struct treadmill_state *s, uint8_t fe_state)
{
	if (s == NULL || s->state == fe_state) {
		return;
	}

	/* §8.5.2.7.1: the lap event is a CHANGE of the toggle bit, not a value
	 * of it. A finished session is the one transition this machine treats
	 * as a lap; a real console's LAP button would call the same line. */
	if (fe_state == TREADMILL_STATE_FINISHED) {
		s->lap_toggle = !s->lap_toggle;
	}
	s->state = fe_state;
}

void treadmill_state_set_user_mass(struct treadmill_state *s, uint16_t mass_10g)
{
	if (s != NULL) {
		s->user_mass_10g = mass_10g;
	}
}

/*
 * The integrator.
 *
 * Every accumulator keeps a micro-unit carry, because a 100 ms tick at
 * 3000 mm/s is 300 mm exactly but at 3333 mm/s is 333.3 mm, and dropping the
 * tenth loses 12 metres per hour - which is a whole percent of a 20 minute run
 * and is exactly the kind of error that looks like a calibration problem.
 */
void treadmill_state_tick(struct treadmill_state *s, uint32_t dt_ms)
{
	uint64_t distance_um;
	uint64_t tick_mm;
	int64_t  vertical_scaled; /* mm x 0.01 %, i.e. 1/10000 mm */

	if (s == NULL || dt_ms == 0u) {
		return;
	}

	/* FE-C §8.5.2.2: the elapsed-time accumulator advances only in the
	 * IN_USE state. Session time, not wall time. */
	if (s->state == TREADMILL_STATE_IN_USE) {
		s->elapsed_ms += dt_ms;
	}

	/* mm/s * ms = micrometres. 15996 * 4294967295 overflows 32 bits, so
	 * this is deliberately 64-bit even though dt_ms is small in practice. */
	distance_um = (uint64_t)s->belt_speed_mm_s * (uint64_t)dt_ms;
	distance_um += s->distance_carry_um;
	tick_mm = distance_um / 1000u;
	s->distance_mm += tick_mm;
	s->distance_carry_um = (uint32_t)(distance_um % 1000u);

	/*
	 * Vertical distance is grade x horizontal distance, and the grade is a
	 * PERCENT IN HUNDREDTHS - so the divisor is 10000 (100 for the percent,
	 * 100 for the hundredths) and not 100. Getting that wrong makes a 1 %
	 * incline climb like a 100 % one, and nothing downstream would question
	 * it: the field is an accumulator with no plausible range.
	 *
	 * The small-angle approximation (rise = run x grade, rather than
	 * run x sin(atan(grade))) is what every treadmill console does and what
	 * FE-C's own field means; at 15 % it is 1.1 % optimistic, well inside
	 * the resolution of a 0.1 m field.
	 */
	vertical_scaled = (int64_t)tick_mm * (int64_t)s->incline_centi_pct;
	if (vertical_scaled >= 0) {
		uint64_t up = (uint64_t)vertical_scaled + s->vertical_carry;

		s->pos_vertical_mm += (uint32_t)(up / 10000u);
		s->vertical_carry = (uint32_t)(up % 10000u);
	} else {
		uint64_t down = (uint64_t)(-vertical_scaled) + s->vertical_carry;

		/* The DESCENT accumulator is a MAGNITUDE and counts up while
		 * the deck goes down - FE-C page 19 byte 5 is unsigned. A
		 * signed accumulator here is the mistake that makes a downhill
		 * interval subtract from the climb total. */
		s->neg_vertical_mm += (uint32_t)(down / 10000u);
		s->vertical_carry = (uint32_t)(down % 10000u);
	}

	/*
	 * Energy, and it is an ESTIMATE that this file does not pretend
	 * otherwise about. The ACSM walking/running equation in the form every
	 * treadmill console uses:
	 *
	 *   VO2 (ml/kg/min) = 0.2 * v + 0.9 * v * grade + 3.5,  v in m/min
	 *   kcal/min        = VO2 * mass_kg / 200
	 *
	 * Rearranged into integers with the units this struct carries. The
	 * result feeds FE-C page 18, which is OPTIONAL, and nothing else - no
	 * control decision anywhere depends on it, which is the only reason a
	 * population-average equation is acceptable at all.
	 */
	{
		uint32_t mass_10g = (s->user_mass_10g != 0u)
					    ? s->user_mass_10g
					    : TREADMILL_DEFAULT_MASS_10G;
		/* v in metres per minute, from mm/s: * 60 / 1000. Kept in
		 * milli-m/min to preserve the fraction. */
		uint64_t v_mmm = (uint64_t)s->belt_speed_mm_s * 60u;
		/* 0.2 * v + 3.5, in milli-ml/kg/min. */
		int64_t vo2_mmlkgmin = (int64_t)((v_mmm * 2u) / 10u) + 3500;
		/* 0.9 * v * grade, where grade is a FRACTION - so the 0.01 %
		 * unit divides by 10000, not by 100. Getting that wrong makes
		 * a 1 % incline burn as much as a 100 % one. */
		vo2_mmlkgmin += ((int64_t)v_mmm * 9 *
				 (int64_t)s->incline_centi_pct) /
				(10 * 10000);
		if (vo2_mmlkgmin < 0) {
			vo2_mmlkgmin = 0; /* a steep enough descent is not
					   * negative work, it is just cheap */
		}

		{
			/*
			 * kcal/min = VO2 (ml/kg/min) * mass_kg / 200, then
			 * scaled by dt. Every scale factor folded into one
			 * divisor, derived rather than tuned:
			 *
			 *   vo2_mmlkgmin  is 1e-3 ml/kg/min
			 *   mass_10g      is 1e-2 kg
			 *   / 200         ml -> kcal
			 *   * dt_ms/60000 per-minute -> per-tick
			 *   * 1000        kcal -> milli-kcal
			 *
			 *   1e-3 * 1e-2 / 200 / 60000 * 1000 = 1 / 1.2e9
			 *
			 * One divide, done last, so nothing is rounded twice.
			 * The product peaks around 5e12 for an implausible
			 * runner and a 1 s tick - four orders inside uint64.
			 */
			uint64_t num = (uint64_t)vo2_mmlkgmin *
				       (uint64_t)mass_10g * (uint64_t)dt_ms;

			num += s->energy_carry;
			s->energy_mkcal += num / 1200000000ull;
			s->energy_carry = (uint32_t)(num % 1200000000ull);
		}
	}
}

/* ── The conversions ────────────────────────────────────────────────────── */

uint8_t treadmill_cadence_strides_min(uint32_t strides_min_16)
{
	uint32_t whole = (strides_min_16 + 8u) / 16u; /* round to nearest */

	return (whole > 255u) ? 255u : (uint8_t)whole;
}

uint8_t treadmill_rsc_cadence_from_strides(uint32_t strides_min_16)
{
	/*
	 * TIMES TWO, and this is the line the whole cadence trap comes down to.
	 * One stride is two footfalls. FE-C page 19 and ANT+ SDM both count
	 * strides; BLE RSC counts STEPS, whatever Nordic's rscs.h comment says
	 * about its own field - its sample simulates 150-180, which is steps
	 * for a runner and would be an impossible stride rate.
	 *
	 * The doubling happens BEFORE the rounding so that 85.5 strides/min is
	 * 171 steps rather than 172.
	 */
	uint32_t steps = (strides_min_16 * 2u + 8u) / 16u;

	return (steps > 255u) ? 255u : (uint8_t)steps;
}

int16_t treadmill_ftms_incline_from_fec(int32_t centi_pct)
{
	/* Round to nearest, away from zero, so a small negative grade keeps
	 * its sign instead of becoming a positive zero. */
	if (centi_pct >= 0) {
		return (int16_t)((centi_pct + 5) / 10);
	}
	return (int16_t)(-((-centi_pct + 5) / 10));
}

int32_t treadmill_fec_incline_from_ftms(int16_t deci_pct)
{
	return (int32_t)deci_pct * 10;
}

uint16_t treadmill_ftms_speed_from_mm_s(uint32_t mm_s)
{
	/* 1 mm/s = 0.0036 km/h = 0.36 units of 0.01 km/h. Multiply first: at
	 * the 15996 mm/s ceiling this is 575856, comfortably inside 32 bits,
	 * and the field's own maximum is 65535 (655.35 km/h). */
	uint32_t centi_kph = (mm_s * 36u + 50u) / 100u;

	return (centi_kph > 0xFFFFu) ? 0xFFFFu : (uint16_t)centi_kph;
}

uint32_t treadmill_mm_s_from_ftms_speed(uint16_t centi_kph)
{
	return ((uint32_t)centi_kph * 100u + 18u) / 36u;
}

uint16_t treadmill_rsc_speed_from_mm_s(uint32_t mm_s)
{
	uint32_t units = (mm_s * 256u + 500u) / 1000u;

	return (units > 0xFFFFu) ? 0xFFFFu : (uint16_t)units;
}

uint8_t treadmill_stride_length_cm(uint64_t distance_mm, uint32_t strides)
{
	uint64_t cm;

	if (strides == 0u) {
		return 0u;
	}
	/* mm per stride -> 0.01 m per stride is a divide by 10. The field is a
	 * byte, so 2.55 m is the ceiling; a stride longer than that is a
	 * measurement fault, and reporting the ceiling is less wrong than
	 * reporting the low byte of it. */
	cm = (distance_mm * 10u) / ((uint64_t)strides * 100u);
	return (cm > 255u) ? 255u : (uint8_t)cm;
}
