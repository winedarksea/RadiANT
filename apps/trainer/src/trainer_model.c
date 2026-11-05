/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_model.c - see trainer_model.h. No Zephyr header, on purpose: the
 * include list is the check that tests/model can compile this on its own.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "profile_fec.h"
#include "profile_fec_tx.h"
#include "trainer_model.h"

#define G_MS2 9.81f

void trainer_model_init(struct trainer_model *m)
{
	memset(m, 0, sizeof(*m));
	m->rider_power_w = TRAINER_DEFAULT_RIDER_POWER_W;
	m->cadence_rpm = TRAINER_DEFAULT_RIDER_CADENCE_RPM;
	m->mode = TRAINER_MODE_SIM;
	m->wind_coeff_centi = TRAINER_DEFAULT_WIND_COEFF_CENTI;
	m->wind_speed_kph = 0;
	m->drafting_centi = TRAINER_DEFAULT_DRAFTING_CENTI;
	m->grade_centi_pct = 0;
	m->crr_5e5 = TRAINER_DEFAULT_CRR_5E5;
	m->user_weight_10g = TRAINER_DEFAULT_USER_WEIGHT_10G;
	m->bike_weight_50g = TRAINER_DEFAULT_BIKE_WEIGHT_50G;
	m->wheel_diameter_cm = TRAINER_DEFAULT_WHEEL_DIAM_CM;
}

void trainer_model_set_rider(struct trainer_model *m, uint16_t power_w,
			     uint8_t cadence_rpm)
{
	m->rider_power_w = power_w;
	m->cadence_rpm = cadence_rpm;
}

void trainer_model_basic_resistance(struct trainer_model *m,
				    uint8_t resistance_half_pct)
{
	m->mode = TRAINER_MODE_BASIC;
	/* 200 half-percent is 100 %; anything above is not a resistance. */
	m->resistance_half_pct = (resistance_half_pct > 200u)
					 ? 200u
					 : resistance_half_pct;
}

void trainer_model_target_power(struct trainer_model *m,
				uint16_t power_quarter_w)
{
	m->mode = TRAINER_MODE_ERG;
	m->target_power_qw = power_quarter_w;
}

void trainer_model_wind(struct trainer_model *m,
			const struct profile_fec_cmd_wind_resistance *w)
{
	m->mode = TRAINER_MODE_SIM;
	m->wind_coeff_centi = w->coefficient_valid
				      ? w->coefficient_centi
				      : TRAINER_DEFAULT_WIND_COEFF_CENTI;
	m->wind_speed_kph = w->wind_speed_valid ? w->wind_speed_kph : 0;
	m->drafting_centi = w->drafting_valid ? w->drafting_centi
					      : TRAINER_DEFAULT_DRAFTING_CENTI;
}

void trainer_model_track(struct trainer_model *m,
			 const struct profile_fec_cmd_track_resistance *t)
{
	m->mode = TRAINER_MODE_SIM;
	/* The decoder already turned the 0xFFFF sentinel into 0: SS8.8.4.1
	 * says an FE told nothing assumes flat ground. */
	m->grade_centi_pct = t->grade_centi_pct;
	m->crr_5e5 = t->rolling_resistance_valid ? t->rolling_resistance
						 : TRAINER_DEFAULT_CRR_5E5;
}

void trainer_model_user(struct trainer_model *m,
			const struct profile_fec_cmd_user_config *u)
{
	m->user_weight_10g = u->user_weight_valid
				     ? u->user_weight_10g
				     : TRAINER_DEFAULT_USER_WEIGHT_10G;
	m->bike_weight_50g = u->bike_weight_valid
				     ? u->bike_weight_50g
				     : TRAINER_DEFAULT_BIKE_WEIGHT_50G;
	m->wheel_diameter_cm = u->wheel_diameter_valid
				       ? u->wheel_diameter_cm
				       : TRAINER_DEFAULT_WHEEL_DIAM_CM;
}

static float total_mass_kg(const struct trainer_model *m)
{
	return (float)m->user_weight_10g * 0.01f +
	       (float)m->bike_weight_50g * 0.05f;
}

float trainer_model_resistance_n(const struct trainer_model *m, float speed_ms)
{
	float mass = total_mass_kg(m);
	float grade = 0.0f;
	float rel;
	float total;

	/* BASIC is a flat road plus the brake; the grade is the simulation's
	 * and does not apply to it. */
	if (m->mode != TRAINER_MODE_BASIC) {
		grade = (float)m->grade_centi_pct / 10000.0f;
	}

	/* Equation 8-13, gravitational. Negative downhill. */
	total = mass * G_MS2 * grade;
	/* Equation 8-14, rolling. */
	total += mass * G_MS2 * ((float)m->crr_5e5 * 5e-5f);
	/* Equation 8-15, wind. Signed: a tailwind faster than the rider
	 * pushes, which is the one way this term goes negative. */
	rel = speed_ms + (float)m->wind_speed_kph / 3.6f;
	total += 0.5f * ((float)m->wind_coeff_centi * 0.01f) * rel *
		 (rel < 0.0f ? -rel : rel) * ((float)m->drafting_centi * 0.01f);

	if (m->mode == TRAINER_MODE_BASIC) {
		total += (float)TRAINER_MAX_RESISTANCE_N *
			 ((float)m->resistance_half_pct / 200.0f);
	}

	/* A brake cannot push the rider downhill. */
	return (total < 0.0f) ? 0.0f : total;
}

float trainer_model_solve_speed(const struct trainer_model *m, float power_w)
{
	float lo = 0.0f;
	float hi = TRAINER_SPEED_MAX_MS;

	if (power_w <= 0.0f) {
		return 0.0f;
	}
	if (trainer_model_resistance_n(m, hi) * hi < power_w) {
		return hi;
	}

	/* resistance(v) x v rises with v wherever it is positive, which is
	 * what bisection needs; where the clamp holds it at 0 (a steep
	 * descent), the crossing is simply further up. */
	for (uint32_t i = 0; i < TRAINER_SOLVE_ITERATIONS; i++) {
		float mid = 0.5f * (lo + hi);

		if (trainer_model_resistance_n(m, mid) * mid < power_w) {
			lo = mid;
		} else {
			hi = mid;
		}
	}
	return hi;
}

void trainer_model_tick(struct trainer_model *m, uint32_t dt_ms)
{
	float power;
	float speed;

	m->erg_speed_low = false;

	if (m->cadence_rpm == 0u) {
		/* Not pedalling is no power in every mode, and in ERG it is
		 * the textbook "speed too low to reach the target". */
		power = 0.0f;
		m->erg_speed_low = (m->mode == TRAINER_MODE_ERG);
	} else if (m->mode == TRAINER_MODE_ERG) {
		power = (float)m->target_power_qw * 0.25f;
	} else {
		power = (float)m->rider_power_w;
	}

	speed = trainer_model_solve_speed(m, power);
	m->speed_mm_s = (uint32_t)(speed * 1000.0f + 0.5f);
	m->resistance_n = trainer_model_resistance_n(m, speed);
	m->power_w = (uint16_t)(power + 0.5f);

	/* One page 25 update per tick: the event count and the accumulator
	 * advance together, which is what makes the accumulator's delta over
	 * the count's delta an average power. */
	m->event_count++;
	m->acc_power_w = (uint16_t)(m->acc_power_w + m->power_w);
	m->elapsed_ms += dt_ms;
	m->distance_um += (uint64_t)m->speed_mm_s * dt_ms;
}

void trainer_model_pages(const struct trainer_model *m,
			 struct profile_fec_general *g,
			 struct profile_fec_settings *s,
			 struct profile_fec_trainer *t)
{
	uint8_t state = (m->cadence_rpm != 0u) ? PROFILE_FEC_STATE_IN_USE
					       : PROFILE_FEC_STATE_READY;

	memset(g, 0, sizeof(*g));
	g->equipment_type = PROFILE_FEC_TYPE_TRAINER;
	g->elapsed_time_qs = (uint8_t)((m->elapsed_ms / 250u) & 0xFFu);
	g->distance_m = (uint8_t)((m->distance_um / 1000000u) & 0xFFu);
	g->distance_valid = true;
	g->speed_mm_s = (m->speed_mm_s > 0xFFFEu) ? 0xFFFEu
						  : (uint16_t)m->speed_mm_s;
	g->speed_valid = true;
	/* Capabilities bit 3: a real number for an unreal quantity. */
	g->speed_virtual = true;
	g->heart_rate_valid = false;
	g->hr_source = PROFILE_FEC_HR_SRC_INVALID;
	g->state = state;

	memset(s, 0, sizeof(*s));
	s->cycle_length_valid = false;
	/* SS8.8.4.1: the reported incline matches the commanded grade. The
	 * setter clamps it to page 17's +-100 %. */
	s->incline_centi_pct = m->grade_centi_pct;
	s->incline_valid = true;
	if (m->mode == TRAINER_MODE_BASIC) {
		s->resistance_half_pct = m->resistance_half_pct;
	} else {
		float share = m->resistance_n / (float)TRAINER_MAX_RESISTANCE_N;

		s->resistance_half_pct =
			(share >= 1.0f) ? 200u
					: (uint8_t)(share * 200.0f + 0.5f);
	}
	/* SS10.1.2.6: mandatory on a trainer. */
	s->resistance_valid = true;
	s->state = state;

	memset(t, 0, sizeof(*t));
	t->event_count = m->event_count;
	t->cadence_rpm = m->cadence_rpm;
	t->cadence_valid = true;
	t->acc_power_w = m->acc_power_w;
	t->inst_power_w = m->power_w;
	t->power_valid = true;
	/* No calibration and no user configuration required: the defaults
	 * stand in for a page 55 that never came, and SS10.1.2.6 allows the
	 * calibration bits to stay clear. */
	t->trainer_status = 0u;
	t->flags = m->erg_speed_low ? PROFILE_FEC_TARGET_POWER_SPEED_LOW
				    : PROFILE_FEC_TARGET_POWER_OK;
	t->state = state;
}

void trainer_model_echo(const struct trainer_model *m,
			struct profile_fec_cmd_wind_resistance *w,
			struct profile_fec_cmd_track_resistance *t)
{
	memset(w, 0, sizeof(*w));
	w->coefficient_centi = m->wind_coeff_centi;
	w->coefficient_valid = true;
	w->wind_speed_kph = m->wind_speed_kph;
	w->wind_speed_valid = true;
	w->drafting_centi = m->drafting_centi;
	w->drafting_valid = true;

	memset(t, 0, sizeof(*t));
	t->grade_centi_pct = m->grade_centi_pct;
	t->grade_valid = true;
	t->rolling_resistance = m->crr_5e5;
	t->rolling_resistance_valid = true;
}
