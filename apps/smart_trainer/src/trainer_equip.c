/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_equip.c - see trainer_equip.h. No Zephyr header, on purpose: the
 * include list is the check that tests/glue can compile this on its own.
 */

#include <errno.h>
#include <string.h>

#include "trainer_equip.h"

static void put_le16(uint8_t *out, uint16_t v)
{
	out[0] = (uint8_t)(v & 0xFFu);
	out[1] = (uint8_t)(v >> 8);
}

static void default_sim(struct trainer_sim_params *p)
{
	memset(p, 0, sizeof(*p));
	p->grade_centi_pct = 0;
	p->crr_e5 = TRAINER_SIM_DEFAULT_CRR_E5;
	p->cw_centi = TRAINER_SIM_DEFAULT_CW_CENTI;
	p->wind_mm_s = 0;
	p->drafting_centi = TRAINER_SIM_DEFAULT_DRAFTING_CENTI;
}

void trainer_equip_init(struct trainer_equip *e, uint16_t max_resistance_n)
{
	memset(e, 0, sizeof(*e));
	e->max_resistance_n = max_resistance_n;
	e->user.user_weight_10g = TRAINER_USER_DEFAULT_WEIGHT_10G;
	e->user.bike_weight_50g = TRAINER_USER_DEFAULT_BIKE_50G;
	e->user.wheel_circ_deci_mm = TRAINER_USER_DEFAULT_WHEEL_DECI_MM;
	trainer_equip_reset_targets(e);
	e->run = TRAINER_RUN_IDLE;
}

void trainer_equip_reset_targets(struct trainer_equip *e)
{
	e->mode = TRAINER_EQUIP_FREE;
	e->resistance_permille = 0u;
	e->target_power_w = 0u;
	default_sim(&e->sim);
}

enum trainer_run trainer_equip_report(struct trainer_equip *e,
				      const struct trainer_report *r)
{
	enum trainer_run prev = e->run;

	e->rep = *r;
	/* The rider is the trainer's start button: a passive machine is "in
	 * use" when somebody pedals it. */
	if (e->run == TRAINER_RUN_IDLE && r->pedalling) {
		e->run = TRAINER_RUN_RUNNING;
	} else if (e->run == TRAINER_RUN_STOPPED) {
		/* A FRESH start only: the rider must have been seen still since
		 * the Stop, so legs still turning through it do not restart the
		 * session at once - and an ANT+-only rider, who has no command
		 * to restart with, is not left FINISHED forever. */
		if (!r->pedalling) {
			e->still_since_stop = true;
		} else if (e->still_since_stop) {
			e->run = TRAINER_RUN_RUNNING;
		}
	}
	return prev;
}

enum trainer_run trainer_equip_event(struct trainer_equip *e,
				     enum trainer_source_event evt)
{
	enum trainer_run prev = e->run;

	switch (evt) {
	case TRAINER_EVT_STARTED:
		e->run = TRAINER_RUN_RUNNING;
		break;
	case TRAINER_EVT_PAUSED:
		if (e->run == TRAINER_RUN_RUNNING) {
			e->run = TRAINER_RUN_PAUSED;
		}
		break;
	case TRAINER_EVT_STOPPED:
		if (e->run == TRAINER_RUN_RUNNING || e->run == TRAINER_RUN_PAUSED) {
			e->run = TRAINER_RUN_STOPPED;
			e->still_since_stop = false;
		}
		break;
	case TRAINER_EVT_CONSOLE_STOP:
		e->epoch++;
		if (e->run == TRAINER_RUN_RUNNING || e->run == TRAINER_RUN_PAUSED) {
			e->run = TRAINER_RUN_STOPPED;
			e->still_since_stop = false;
		}
		break;
	case TRAINER_EVT_CONSOLE_RESET:
		e->epoch++;
		/* A local reset starts a new session for every view that has a
		 * baseline; the ANT+ accumulators are rollover counters a
		 * receiver differences, so they keep counting. */
		e->ftms_elapsed_base_ms = e->elapsed_ms;
		e->ftms_distance_base_um = e->distance_um;
		e->run = TRAINER_RUN_IDLE;
		break;
	default:
		break;
	}
	return prev;
}

enum trainer_run trainer_equip_ftms_reset(struct trainer_equip *e)
{
	enum trainer_run prev = e->run;

	e->epoch++;
	e->ftms_elapsed_base_ms = e->elapsed_ms;
	e->ftms_distance_base_um = e->distance_um;
	e->run = TRAINER_RUN_IDLE;
	return prev;
}

void trainer_equip_tick(struct trainer_equip *e, uint32_t dt_ms)
{
	uint16_t p = e->rep.power_valid ? e->rep.power_w : 0u;

	e->event_count++;
	e->acc_power_w = (uint16_t)(e->acc_power_w + p);
	if (e->run == TRAINER_RUN_RUNNING) {
		e->elapsed_ms += dt_ms;
		if (e->rep.speed_valid) {
			e->distance_um += (uint64_t)e->rep.speed_mm_s * dt_ms;
		}
	}
}

uint8_t trainer_equip_fe_state(enum trainer_run run)
{
	switch (run) {
	case TRAINER_RUN_RUNNING:
		return RADIANT_PROFILE_FEC_STATE_IN_USE;
	case TRAINER_RUN_PAUSED:
	case TRAINER_RUN_STOPPED:
		return RADIANT_PROFILE_FEC_STATE_FINISHED;
	default:
		return RADIANT_PROFILE_FEC_STATE_READY;
	}
}

int32_t trainer_equip_resistance_permille(const struct trainer_equip *e)
{
	uint32_t pm;

	if (e->mode == TRAINER_EQUIP_BASIC) {
		return (int32_t)e->resistance_permille;
	}
	if (!e->rep.resistance_valid || e->max_resistance_n == 0u) {
		return -1;
	}
	pm = ((uint32_t)e->rep.resistance_n * 1000u + e->max_resistance_n / 2u) /
	     e->max_resistance_n;
	return (int32_t)((pm > 1000u) ? 1000u : pm);
}

void trainer_equip_pages(const struct trainer_equip *e,
			 struct radiant_profile_fec_general *g,
			 struct radiant_profile_fec_settings *s,
			 struct radiant_profile_fec_trainer *t)
{
	uint8_t state = trainer_equip_fe_state(e->run);
	int32_t pm = trainer_equip_resistance_permille(e);

	memset(g, 0, sizeof(*g));
	g->equipment_type = RADIANT_PROFILE_FEC_TYPE_TRAINER;
	g->elapsed_time_qs = (uint8_t)((e->elapsed_ms / 250u) & 0xFFu);
	g->distance_m = (uint8_t)((e->distance_um / 1000000u) & 0xFFu);
	/* SS10.1.2.6: distance and speed are mandatory on a trainer. */
	g->distance_valid = true;
	g->speed_valid = e->rep.speed_valid;
	g->speed_mm_s = (e->rep.speed_mm_s > 0xFFFEu) ? 0xFFFEu
						      : (uint16_t)e->rep.speed_mm_s;
	g->speed_virtual = e->rep.speed_virtual;
	g->heart_rate_valid = false;
	g->hr_source = RADIANT_PROFILE_FEC_HR_SRC_INVALID;
	g->state = state;

	memset(s, 0, sizeof(*s));
	s->cycle_length_valid = false;
	/* SS8.8.4.1: the reported incline is the commanded grade. The setter
	 * clamps it to page 17's +-100 %. */
	s->incline_centi_pct = e->sim.grade_centi_pct;
	s->incline_valid = true;
	/* SS10.1.2.6: the resistance field is mandatory on a trainer; 0xFF
	 * only when the driver gives no force and no maximum. */
	s->resistance_valid = (pm >= 0);
	s->resistance_half_pct = (pm >= 0) ? (uint8_t)((pm + 2) / 5) : 0xFFu;
	s->state = state;

	memset(t, 0, sizeof(*t));
	t->event_count = e->event_count;
	t->cadence_rpm = e->rep.cadence_rpm;
	t->cadence_valid = e->rep.cadence_valid;
	t->acc_power_w = e->acc_power_w;
	t->inst_power_w = e->rep.power_w;
	t->power_valid = e->rep.power_valid;
	t->trainer_status = 0u;
	/* Table 8-28's target-power limits mean something only in ERG. */
	t->flags = (e->mode == TRAINER_EQUIP_ERG && e->rep.erg_speed_low)
			   ? RADIANT_PROFILE_FEC_TARGET_POWER_SPEED_LOW
			   : RADIANT_PROFILE_FEC_TARGET_POWER_OK;
	t->state = state;
}

void trainer_equip_echo(const struct trainer_equip *e,
			struct radiant_profile_fec_cmd_wind_resistance *w,
			struct radiant_profile_fec_cmd_track_resistance *tr,
			struct radiant_profile_fec_cmd_user_config *u)
{
	int32_t kph;
	uint32_t crr5;
	uint32_t diam_mm;

	memset(w, 0, sizeof(*w));
	w->coefficient_centi = e->sim.cw_centi;
	w->coefficient_valid = true;
	/* mm/s -> km/h, rounded half away from zero. */
	kph = (e->sim.wind_mm_s >= 0) ? ((e->sim.wind_mm_s * 36 + 5000) / 10000)
				      : -((-e->sim.wind_mm_s * 36 + 5000) / 10000);
	w->wind_speed_kph = (int16_t)((kph > 127) ? 127 : (kph < -127) ? -127 : kph);
	w->wind_speed_valid = true;
	w->drafting_centi = e->sim.drafting_centi;
	w->drafting_valid = true;

	memset(tr, 0, sizeof(*tr));
	tr->grade_centi_pct = e->sim.grade_centi_pct;
	tr->grade_valid = true;
	crr5 = ((uint32_t)e->sim.crr_e5 + 2u) / 5u;
	tr->rolling_resistance = (uint8_t)((crr5 > 0xFEu) ? 0xFEu : crr5);
	tr->rolling_resistance_valid = true;

	memset(u, 0, sizeof(*u));
	u->user_weight_10g = e->user.user_weight_10g;
	u->user_weight_valid = true;
	u->bike_weight_50g = e->user.bike_weight_50g;
	u->bike_weight_valid = true;
	/* Circumference (0.1 mm) -> diameter (mm) = c / (10 pi), then page
	 * 55's 0.01 m byte plus its 1 mm offset nibble. */
	diam_mm = ((uint32_t)e->user.wheel_circ_deci_mm * 1000u + 15708u) / 31416u;
	if (diam_mm / 10u < 0xFFu) {
		u->wheel_diameter_cm = (uint8_t)(diam_mm / 10u);
		u->wheel_diameter_valid = true;
		u->wheel_offset_mm = (uint8_t)(diam_mm % 10u);
		u->wheel_offset_valid = true;
	}
	u->gear_ratio_valid = false;
}

int trainer_equip_ibd_encode(const struct trainer_equip *e, uint8_t *buf, size_t cap)
{
	uint16_t flags = TRAINER_IBD_FLAG_CADENCE | TRAINER_IBD_FLAG_DISTANCE |
			 TRAINER_IBD_FLAG_RESISTANCE | TRAINER_IBD_FLAG_POWER |
			 TRAINER_IBD_FLAG_ELAPSED;
	uint32_t speed;
	uint32_t dist_m;
	uint32_t elapsed_s;
	int32_t pm;
	size_t n = 0u;

	if (e == NULL || buf == NULL) {
		return -EINVAL;
	}
	if (cap < TRAINER_IBD_LEN) {
		return -ENOSPC;
	}

	put_le16(&buf[n], flags);
	n += 2u;

	/* 0.01 km/h = mm/s x 0.36. No sentinel in FTMS: an unknown speed is
	 * sent as 0. */
	speed = e->rep.speed_valid ? (e->rep.speed_mm_s * 36u + 50u) / 100u : 0u;
	put_le16(&buf[n], (uint16_t)((speed > 0xFFFFu) ? 0xFFFFu : speed));
	n += 2u;

	/* 0.5 /min units. */
	put_le16(&buf[n], e->rep.cadence_valid ? (uint16_t)(e->rep.cadence_rpm * 2u) : 0u);
	n += 2u;

	dist_m = (uint32_t)((e->distance_um - e->ftms_distance_base_um) / 1000000u);
	if (dist_m > 0xFFFFFFu) {
		dist_m = 0xFFFFFFu;
	}
	buf[n++] = (uint8_t)(dist_m & 0xFFu);
	buf[n++] = (uint8_t)((dist_m >> 8) & 0xFFu);
	buf[n++] = (uint8_t)((dist_m >> 16) & 0xFFu);

	/* Resolution 1 (FTMS 1.0 Table 4.9), in the level scale of the
	 * Supported Resistance Level Range: level = per mille / 50. */
	pm = trainer_equip_resistance_permille(e);
	put_le16(&buf[n], (uint16_t)(int16_t)((pm < 0) ? 0 : (pm + 25) / 50));
	n += 2u;

	put_le16(&buf[n], (uint16_t)(int16_t)(e->rep.power_valid ? e->rep.power_w : 0));
	n += 2u;

	elapsed_s = (e->elapsed_ms - e->ftms_elapsed_base_ms) / 1000u;
	put_le16(&buf[n], (uint16_t)((elapsed_s > 0xFFFFu) ? 0xFFFFu : elapsed_s));
	n += 2u;

	return (int)n;
}
