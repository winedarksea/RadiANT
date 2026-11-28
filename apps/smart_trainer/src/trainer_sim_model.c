/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_sim_model.c - see trainer_sim_model.h. No Zephyr header.
 */

#include <errno.h>
#include <string.h>

#include "trainer_sim_model.h"

void trainer_sim_model_init(struct trainer_sim_model *s)
{
	memset(s, 0, sizeof(*s));
	trainer_model_init(&s->model);
	s->rider_cadence_rpm = s->model.cadence_rpm;
	s->rider_on = true;
}

int trainer_sim_model_set_target_power(struct trainer_sim_model *s, uint16_t watts)
{
	/* The model takes FE-C page 49's 0.25 W unit. */
	trainer_model_target_power(&s->model, (uint16_t)(watts * 4u));
	return 0;
}

int trainer_sim_model_set_resistance(struct trainer_sim_model *s, uint16_t permille)
{
	if (permille > 1000u) {
		return -ERANGE;
	}
	/* Page 48's 0.5 % unit: 5 per mille. */
	trainer_model_basic_resistance(&s->model, (uint8_t)((permille + 2u) / 5u));
	return 0;
}

int trainer_sim_model_set_sim(struct trainer_sim_model *s, const struct trainer_sim_params *p)
{
	struct radiant_profile_fec_cmd_wind_resistance w;
	struct radiant_profile_fec_cmd_track_resistance t;
	int32_t kph;
	uint32_t crr5;

	if (p == NULL) {
		return -EINVAL;
	}
	memset(&w, 0, sizeof(w));
	w.coefficient_centi = p->cw_centi;
	w.coefficient_valid = true;
	/* mm/s -> km/h, rounded, into the model's int16. */
	kph = (p->wind_mm_s >= 0) ? ((p->wind_mm_s * 36 + 5000) / 10000)
				  : -((-p->wind_mm_s * 36 + 5000) / 10000);
	w.wind_speed_kph = (int16_t)((kph > 127) ? 127 : (kph < -127) ? -127 : kph);
	w.wind_speed_valid = true;
	w.drafting_centi = p->drafting_centi;
	w.drafting_valid = true;
	trainer_model_wind(&s->model, &w);

	memset(&t, 0, sizeof(t));
	t.grade_centi_pct = p->grade_centi_pct;
	t.grade_valid = true;
	/* 1e-5 -> the model's 5e-5. */
	crr5 = ((uint32_t)p->crr_e5 + 2u) / 5u;
	t.rolling_resistance = (uint8_t)((crr5 > 0xFEu) ? 0xFEu : crr5);
	t.rolling_resistance_valid = true;
	trainer_model_track(&s->model, &t);
	return 0;
}

int trainer_sim_model_set_user(struct trainer_sim_model *s, const struct trainer_user *u)
{
	struct radiant_profile_fec_cmd_user_config c;
	uint32_t diam_mm;

	if (u == NULL) {
		return -EINVAL;
	}
	memset(&c, 0, sizeof(c));
	c.user_weight_10g = u->user_weight_10g;
	c.user_weight_valid = true;
	c.bike_weight_50g = u->bike_weight_50g;
	c.bike_weight_valid = (u->bike_weight_50g < RADIANT_PROFILE_FEC_BIKE_WEIGHT_INVALID);
	/* The model keeps the wheel as a diameter in cm (it uses it for
	 * nothing but the record), so 0.1 mm circumference -> cm. */
	diam_mm = ((uint32_t)u->wheel_circ_deci_mm * 1000u + 15708u) / 31416u;
	c.wheel_diameter_cm = (uint8_t)((diam_mm / 10u > 0xFEu) ? 0xFEu : diam_mm / 10u);
	c.wheel_diameter_valid = true;
	trainer_model_user(&s->model, &c);
	return 0;
}

int trainer_sim_model_workout(struct trainer_sim_model *s, enum trainer_workout w,
			      enum trainer_source_event *evt)
{
	switch (w) {
	case TRAINER_WORKOUT_START:
	case TRAINER_WORKOUT_RESUME:
		s->rider_on = true;
		*evt = TRAINER_EVT_STARTED;
		break;
	case TRAINER_WORKOUT_PAUSE:
		s->rider_on = false;
		*evt = TRAINER_EVT_PAUSED;
		break;
	case TRAINER_WORKOUT_STOP:
		s->rider_on = false;
		*evt = TRAINER_EVT_STOPPED;
		break;
	default:
		return -EINVAL;
	}
	trainer_model_set_rider(&s->model, s->model.rider_power_w,
				s->rider_on ? s->rider_cadence_rpm : 0u);
	return 0;
}

void trainer_sim_model_set_rider(struct trainer_sim_model *s, uint16_t power_w,
				 uint8_t cadence_rpm)
{
	s->rider_cadence_rpm = cadence_rpm;
	s->rider_on = (cadence_rpm != 0u);
	trainer_model_set_rider(&s->model, power_w, cadence_rpm);
}

void trainer_sim_model_tick(struct trainer_sim_model *s, uint32_t dt_ms,
			    struct trainer_report *out)
{
	const struct trainer_model *m = &s->model;
	float f;

	trainer_model_tick(&s->model, dt_ms);

	memset(out, 0, sizeof(*out));
	out->power_w = m->power_w;
	out->power_valid = true;
	out->cadence_rpm = m->cadence_rpm;
	out->cadence_valid = true;
	out->speed_mm_s = m->speed_mm_s;
	out->speed_valid = true;
	/* No wheel turns on a DK: the speed is the model's (FE-C page 16
	 * capabilities bit 3). */
	out->speed_virtual = true;
	f = m->resistance_n;
	out->resistance_n = (f <= 0.0f) ? 0u : (f >= 65535.0f) ? 65535u : (uint16_t)(f + 0.5f);
	out->resistance_valid = true;
	out->erg_speed_low = m->erg_speed_low;
	out->pedalling = (m->cadence_rpm != 0u);
}
