/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_cmd.c - see trainer_cmd.h. Kernel-free: no Zephyr header.
 */

#include <errno.h>
#include <string.h>

#include <radiant/radiant_profile_fec_tx.h>

#include "trainer_cmd.h"

static struct trainer_cmd_cfg cfg;

void trainer_cmd_bind(const struct trainer_cmd_cfg *c)
{
	cfg = *c;
}

void trainer_cmd_caps(struct trainer_caps *out)
{
	const struct trainer_source_ops *s = cfg.src;

	memset(out, 0, sizeof(*out));
	if (s == NULL) {
		return;
	}
	out->erg = cfg.allow_erg && s->set_target_power != NULL;
	out->resistance = cfg.allow_resistance && s->set_resistance != NULL;
	out->sim = cfg.allow_sim && s->set_sim != NULL;
	out->user = s->set_user != NULL;
}

uint8_t trainer_cmd_fec_capabilities(void)
{
	struct trainer_caps c;
	uint8_t bits = 0u;

	trainer_cmd_caps(&c);
	if (c.resistance) {
		bits |= RADIANT_PROFILE_FEC_CAP_BASIC_RESISTANCE;
	}
	if (c.erg) {
		bits |= RADIANT_PROFILE_FEC_CAP_TARGET_POWER;
	}
	if (c.sim) {
		bits |= RADIANT_PROFILE_FEC_CAP_SIMULATION;
	}
	return bits;
}

uint8_t trainer_cmd_ctrl_gates(void)
{
	struct trainer_caps c;
	uint8_t g = 0u;

	trainer_cmd_caps(&c);
	if (c.resistance) {
		g |= RADIANT_CTRL_GATE(RADIANT_CTRL_PAGE_BASIC_RESISTANCE);
	}
	if (c.erg) {
		g |= RADIANT_CTRL_GATE(RADIANT_CTRL_PAGE_TARGET_POWER);
	}
	if (c.sim) {
		g |= RADIANT_CTRL_GATE_SIMULATION;
	}
	return g;
}

void trainer_cmd_ftms_caps(struct radiant_ble_ftms_caps *out)
{
	struct trainer_caps c;

	trainer_cmd_caps(&c);
	memset(out, 0, sizeof(*out));
	/* Exactly the Indoor Bike Data fields trainer_equip_ibd_encode() sends
	 * (FTMS Table 4.3 against Table 4.9). */
	out->features = RADIANT_BLE_FTMS_FMF_CADENCE | RADIANT_BLE_FTMS_FMF_TOTAL_DISTANCE |
			RADIANT_BLE_FTMS_FMF_RESISTANCE_LEVEL | RADIANT_BLE_FTMS_FMF_POWER |
			RADIANT_BLE_FTMS_FMF_ELAPSED_TIME;
	if (c.erg) {
		out->targets |= RADIANT_BLE_FTMS_TSF_POWER;
	}
	if (c.resistance) {
		out->targets |= RADIANT_BLE_FTMS_TSF_RESISTANCE;
	}
	if (c.sim) {
		out->targets |= RADIANT_BLE_FTMS_TSF_BIKE_SIM;
	}
	if (c.user) {
		out->targets |= RADIANT_BLE_FTMS_TSF_WHEEL_CIRC;
	}
	/* The two Supported Ranges an indoor bike exposes (SS4.13, SS4.14).
	 * Present whatever the targets, so a client reads a sane value. */
	out->resistance = (struct radiant_ble_ftms_range){
		.min = 0, .max = TRAINER_RESISTANCE_LEVEL_MAX_DECI, .step = 1u, .valid = true,
	};
	out->power = (struct radiant_ble_ftms_range){
		.min = cfg.min_power_w, .max = cfg.max_power_w, .step = 1u, .valid = true,
	};
	/* A trainer moves nothing by itself; its session starts when the rider
	 * pedals, so FTMS Start while RUNNING is a 0x01 no-op. */
	out->passive = true;
}

int32_t trainer_wind_mm_s_from_kph(int32_t kph)
{
	/* 1 km/h = 1000/3.6 mm/s; rounded half away from zero. */
	return (kph >= 0) ? ((kph * 10000 + 18) / 36) : -((-kph * 10000 + 18) / 36);
}

uint16_t trainer_wheel_deci_mm_from_diameter(uint8_t diam_cm, uint8_t offset_mm,
					     bool offset_valid)
{
	uint32_t d_mm = (uint32_t)diam_cm * 10u + (offset_valid ? offset_mm : 0u);
	/* circumference in 0.1 mm = d_mm x pi x 10. */
	uint32_t c = (d_mm * 31416u + 500u) / 1000u;

	return (uint16_t)((c > 0xFFFFu) ? 0xFFFFu : c);
}

static uint16_t clamp_power(uint32_t w)
{
	if (w < cfg.min_power_w) {
		return cfg.min_power_w;
	}
	if (w > cfg.max_power_w) {
		return cfg.max_power_w;
	}
	return (uint16_t)w;
}

static int32_t clamp_grade(int32_t g)
{
	if (g < RADIANT_PROFILE_FEC_GRADE_MIN_CENTI_PCT) {
		return RADIANT_PROFILE_FEC_GRADE_MIN_CENTI_PCT;
	}
	if (g > RADIANT_PROFILE_FEC_GRADE_MAX_CENTI_PCT) {
		return RADIANT_PROFILE_FEC_GRADE_MAX_CENTI_PCT;
	}
	return g;
}

static void status_resistance(uint8_t *out, uint8_t *len, uint16_t permille)
{
	out[0] = RADIANT_BLE_FTMS_STATUS_TARGET_RESISTANCE;
	out[1] = (uint8_t)(permille / TRAINER_PERMILLE_PER_LEVEL_DECI);
	*len = 2u;
}

static void status_power(uint8_t *out, uint8_t *len, uint16_t w)
{
	out[0] = RADIANT_BLE_FTMS_STATUS_TARGET_POWER;
	out[1] = (uint8_t)(w & 0xFFu);
	out[2] = (uint8_t)(w >> 8);
	*len = 3u;
}

static void status_sim(uint8_t *out, uint8_t *len, const struct trainer_sim_params *p)
{
	int32_t wind = p->wind_mm_s;
	uint32_t crr = (p->crr_e5 + 5u) / 10u;

	wind = (wind > 32767) ? 32767 : (wind < -32768) ? -32768 : wind;
	out[0] = RADIANT_BLE_FTMS_STATUS_BIKE_SIM;
	out[1] = (uint8_t)((uint16_t)(int16_t)wind & 0xFFu);
	out[2] = (uint8_t)((uint16_t)(int16_t)wind >> 8);
	out[3] = (uint8_t)((uint16_t)(int16_t)p->grade_centi_pct & 0xFFu);
	out[4] = (uint8_t)((uint16_t)(int16_t)p->grade_centi_pct >> 8);
	out[5] = (uint8_t)((crr > 0xFFu) ? 0xFFu : crr);
	out[6] = p->cw_centi;
	*len = 7u;
}

static void status_wheel(uint8_t *out, uint8_t *len, uint16_t deci_mm)
{
	out[0] = RADIANT_BLE_FTMS_STATUS_WHEEL_CIRC;
	out[1] = (uint8_t)(deci_mm & 0xFFu);
	out[2] = (uint8_t)(deci_mm >> 8);
	*len = 3u;
}

/* The FE-C half: stale, then the token, then the hook. */
static uint8_t fec_gate(uint8_t page, uint32_t rx_epoch, uint32_t now_ms)
{
	/* Epoch first: a page queued before a local stop must not re-take a
	 * token, nor reach the brake. */
	if (rx_epoch != cfg.equip->epoch) {
		return RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED;
	}
	if (cfg.ctrl_claim != NULL && cfg.ctrl_gated != NULL && cfg.ctrl_gated(page) &&
	    cfg.ctrl_claim(RADIANT_CTRL_ANT, false, now_ms) != 0) {
		/* An FTMS client holds control (explicit beats implicit). */
		return RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED;
	}
	return RADIANT_PROFILE_FEC_CMD_STATUS_PASS;
}

static uint8_t fec_status_for(int rc)
{
	return (rc == 0) ? RADIANT_PROFILE_FEC_CMD_STATUS_PASS
			 : RADIANT_PROFILE_FEC_CMD_STATUS_FAIL;
}

static void apply_user_config(const struct radiant_profile_fec_cmd_user_config *u)
{
	struct trainer_equip *e = cfg.equip;
	struct trainer_user next;

	/* SS8.8.4: a field's sentinel selects its default. */
	next.user_weight_10g = u->user_weight_valid ? u->user_weight_10g
						    : TRAINER_USER_DEFAULT_WEIGHT_10G;
	next.bike_weight_50g = u->bike_weight_valid ? u->bike_weight_50g
						    : TRAINER_USER_DEFAULT_BIKE_50G;
	next.wheel_circ_deci_mm =
		u->wheel_diameter_valid
			? trainer_wheel_deci_mm_from_diameter(u->wheel_diameter_cm,
							      u->wheel_offset_mm,
							      u->wheel_offset_valid)
			: TRAINER_USER_DEFAULT_WHEEL_DECI_MM;

	/* SS10.1.3.1: a simulation FE "shall receive, decode, and apply" page
	 * 55. Stored even without a set_user hook, so page 55's echo reports
	 * what the controller said rather than a stale default. */
	if (cfg.src == NULL || cfg.src->set_user == NULL || cfg.src->set_user(&next) == 0) {
		e->user = next;
	}
}

struct trainer_fec_answer trainer_cmd_fec(const uint8_t *body, uint32_t rx_epoch,
					  uint32_t now_ms)
{
	struct trainer_fec_answer a;
	struct trainer_equip *e = cfg.equip;
	struct trainer_caps c;
	int rc;

	memset(&a, 0, sizeof(a));
	if (body == NULL || e == NULL) {
		return a;
	}
	trainer_cmd_caps(&c);

	switch (body[0]) {
	case RADIANT_PROFILE_FEC_PAGE_BASIC_RESISTANCE: {
		struct radiant_profile_fec_cmd_basic_resistance cmd;
		uint16_t pm;

		if (!c.resistance ||
		    radiant_profile_fec_decode_basic_resistance(body, &cmd) != 0) {
			return a; /* ignored: SS8.12.3.1 */
		}
		a.answer = true;
		a.status = fec_gate(body[0], rx_epoch, now_ms);
		if (a.status != RADIANT_PROFILE_FEC_CMD_STATUS_PASS) {
			return a;
		}
		pm = (uint16_t)(((cmd.resistance_half_pct > 200u) ? 200u
								  : cmd.resistance_half_pct) *
				5u);
		rc = cfg.src->set_resistance(pm);
		a.status = fec_status_for(rc);
		if (rc == 0) {
			e->mode = TRAINER_EQUIP_BASIC;
			e->resistance_permille = pm;
			status_resistance(a.ftms_status, &a.ftms_status_len, pm);
		}
		return a;
	}

	case RADIANT_PROFILE_FEC_PAGE_TARGET_POWER: {
		struct radiant_profile_fec_cmd_target_power cmd;
		uint16_t w;

		if (!c.erg || radiant_profile_fec_decode_target_power(body, &cmd) != 0) {
			return a;
		}
		a.answer = true;
		a.status = fec_gate(body[0], rx_epoch, now_ms);
		if (a.status != RADIANT_PROFILE_FEC_CMD_STATUS_PASS) {
			return a;
		}
		/* 0.25 W units, rounded, then into the trainer's range: FE-C
		 * has no "invalid parameter" status, and FTMS clients are told
		 * the same range. */
		w = clamp_power(((uint32_t)cmd.power_quarter_w + 2u) / 4u);
		rc = cfg.src->set_target_power(w);
		a.status = fec_status_for(rc);
		if (rc == 0) {
			e->mode = TRAINER_EQUIP_ERG;
			e->target_power_w = w;
			status_power(a.ftms_status, &a.ftms_status_len, w);
		}
		return a;
	}

	case RADIANT_PROFILE_FEC_PAGE_WIND_RESISTANCE:
	case RADIANT_PROFILE_FEC_PAGE_TRACK_RESISTANCE: {
		struct trainer_sim_params next = e->sim;

		if (!c.sim) {
			return a;
		}
		if (body[0] == RADIANT_PROFILE_FEC_PAGE_WIND_RESISTANCE) {
			struct radiant_profile_fec_cmd_wind_resistance cmd;

			if (radiant_profile_fec_decode_wind_resistance(body, &cmd) != 0) {
				return a;
			}
			next.cw_centi = cmd.coefficient_valid ? cmd.coefficient_centi
							      : TRAINER_SIM_DEFAULT_CW_CENTI;
			next.wind_mm_s = cmd.wind_speed_valid
						 ? trainer_wind_mm_s_from_kph(cmd.wind_speed_kph)
						 : 0;
			next.drafting_centi = cmd.drafting_valid
						      ? cmd.drafting_centi
						      : TRAINER_SIM_DEFAULT_DRAFTING_CENTI;
		} else {
			struct radiant_profile_fec_cmd_track_resistance cmd;

			if (radiant_profile_fec_decode_track_resistance(body, &cmd) != 0) {
				return a;
			}
			/* The decoder already made 0xFFFF "flat" (SS8.8.4.1). */
			next.grade_centi_pct = clamp_grade(cmd.grade_centi_pct);
			next.crr_e5 = cmd.rolling_resistance_valid
					      ? (uint16_t)(cmd.rolling_resistance * 5u)
					      : TRAINER_SIM_DEFAULT_CRR_E5;
		}
		a.answer = true;
		a.status = fec_gate(body[0], rx_epoch, now_ms);
		if (a.status != RADIANT_PROFILE_FEC_CMD_STATUS_PASS) {
			return a;
		}
		rc = cfg.src->set_sim(&next);
		a.status = fec_status_for(rc);
		if (rc == 0) {
			e->mode = TRAINER_EQUIP_SIM;
			e->sim = next;
			status_sim(a.ftms_status, &a.ftms_status_len, &next);
		}
		return a;
	}

	case RADIANT_PROFILE_FEC_PAGE_USER_CONFIG: {
		struct radiant_profile_fec_cmd_user_config cmd;
		uint16_t before = e->user.wheel_circ_deci_mm;

		/* Not a control page (SS8.12.3.1 lists 48-51): applied, never
		 * gated, never answered with a page 71. */
		if (radiant_profile_fec_decode_user_config(body, &cmd) == 0) {
			apply_user_config(&cmd);
			if (e->user.wheel_circ_deci_mm != before) {
				status_wheel(a.ftms_status, &a.ftms_status_len,
					     e->user.wheel_circ_deci_mm);
			}
		}
		return a;
	}

	default:
		/* Anything else is not an FE-C control page this trainer
		 * knows: ignored, as SS8.12.3.1 requires. */
		return a;
	}
}

/* ── FTMS ──────────────────────────────────────────────────────────────── */

static int ftms_gate(uint32_t epoch)
{
	if (cfg.equip == NULL) {
		return -ENODEV;
	}
	if (epoch != cfg.equip->epoch) {
		return -ECANCELED;
	}
	if (cfg.ctrl_owner != NULL && cfg.ctrl_owner() != RADIANT_CTRL_BLE) {
		return -EACCES;
	}
	return 0;
}

int trainer_cmd_ftms_set_target(struct radiant_ble_ftms_target *t, uint32_t epoch,
				void *ctx)
{
	struct trainer_equip *e = cfg.equip;
	const struct trainer_source_ops *s = cfg.src;
	int rc;

	(void)ctx;
	rc = ftms_gate(epoch);
	if (rc != 0) {
		return rc;
	}
	if (s == NULL) {
		return -ENOTSUP;
	}

	switch (t->op) {
	case RADIANT_BLE_FTMS_OP_SET_POWER:
		if (s->set_target_power == NULL) {
			return -ENOTSUP;
		}
		if (t->v.power_w < (int32_t)cfg.min_power_w ||
		    t->v.power_w > (int32_t)cfg.max_power_w) {
			return -ERANGE; /* the core range-checks too */
		}
		rc = s->set_target_power((uint16_t)t->v.power_w);
		if (rc == 0) {
			e->mode = TRAINER_EQUIP_ERG;
			e->target_power_w = (uint16_t)t->v.power_w;
		}
		return rc;

	case RADIANT_BLE_FTMS_OP_SET_RESISTANCE: {
		uint16_t pm;

		if (s->set_resistance == NULL) {
			return -ENOTSUP;
		}
		if (t->v.resistance_deci > TRAINER_RESISTANCE_LEVEL_MAX_DECI) {
			return -ERANGE;
		}
		pm = (uint16_t)(t->v.resistance_deci * TRAINER_PERMILLE_PER_LEVEL_DECI);
		rc = s->set_resistance(pm);
		if (rc == 0) {
			e->mode = TRAINER_EQUIP_BASIC;
			e->resistance_permille = pm;
		}
		return rc;
	}

	case RADIANT_BLE_FTMS_OP_SET_BIKE_SIM: {
		struct trainer_sim_params next = e->sim;

		if (s->set_sim == NULL) {
			return -ENOTSUP;
		}
		/* Same unit as FE-C page 51; clamped to FE-C's span like any
		 * simulation grade, rather than refused mid-ride. Drafting
		 * has no FTMS field and keeps its value. */
		next.grade_centi_pct = clamp_grade(t->v.sim.grade_centi);
		next.wind_mm_s = t->v.sim.wind_mm_s;
		next.crr_e5 = (uint16_t)(t->v.sim.crr_e4 * 10u);
		next.cw_centi = t->v.sim.cw_centi;
		rc = s->set_sim(&next);
		if (rc == 0) {
			e->mode = TRAINER_EQUIP_SIM;
			e->sim = next;
			/* The 0x12 status carries what was applied (R9). */
			t->v.sim.grade_centi = (int16_t)next.grade_centi_pct;
		}
		return rc;
	}

	case RADIANT_BLE_FTMS_OP_SET_WHEEL_CIRC: {
		struct trainer_user next = e->user;

		if (s->set_user == NULL) {
			return -ENOTSUP;
		}
		if (t->v.wheel_deci_mm == 0u) {
			return -EINVAL; /* 0x03: no wheel has no circumference */
		}
		next.wheel_circ_deci_mm = t->v.wheel_deci_mm;
		rc = s->set_user(&next);
		if (rc == 0) {
			e->user = next;
		}
		return rc;
	}

	default:
		return -ENOTSUP;
	}
}

int trainer_cmd_ftms_workout(enum radiant_ble_ftms_workout w, uint32_t epoch, void *ctx)
{
	static const enum trainer_workout map[] = {
		[RADIANT_BLE_FTMS_WORKOUT_START] = TRAINER_WORKOUT_START,
		[RADIANT_BLE_FTMS_WORKOUT_RESUME] = TRAINER_WORKOUT_RESUME,
		[RADIANT_BLE_FTMS_WORKOUT_PAUSE] = TRAINER_WORKOUT_PAUSE,
		[RADIANT_BLE_FTMS_WORKOUT_STOP] = TRAINER_WORKOUT_STOP,
	};
	bool stopping = (w == RADIANT_BLE_FTMS_WORKOUT_PAUSE ||
			 w == RADIANT_BLE_FTMS_WORKOUT_STOP);
	int rc;

	(void)ctx;
	/* Stopping is never refused for the epoch (the core's rule too). */
	rc = stopping ? 0 : ftms_gate(epoch);
	if (rc != 0) {
		return rc;
	}
	if (cfg.src == NULL || cfg.src->workout == NULL) {
		return -ENOTSUP; /* the core answers 0x04: mandatory op codes */
	}
	return cfg.src->workout(map[w]);
}

int trainer_cmd_ftms_reset(bool motion_ok, uint32_t epoch, void *ctx)
{
	struct trainer_equip *e = cfg.equip;
	const struct trainer_source_ops *s = cfg.src;
	struct trainer_sim_params flat;
	int rc = 0;

	(void)ctx;
	/* A trainer has no "stop half" to run: it moves nothing. A refused
	 * Reset (stale) changes nothing, and the core answers 0x04. */
	if (!motion_ok) {
		return -ECANCELED;
	}
	rc = ftms_gate(epoch);
	if (rc != 0) {
		return rc;
	}

	/* SS4.16.2: back to the default state - free ride on a flat road. */
	if (s != NULL && s->set_sim != NULL) {
		flat = e->sim;
		flat.grade_centi_pct = 0;
		flat.crr_e5 = TRAINER_SIM_DEFAULT_CRR_E5;
		flat.cw_centi = TRAINER_SIM_DEFAULT_CW_CENTI;
		flat.wind_mm_s = 0;
		flat.drafting_centi = TRAINER_SIM_DEFAULT_DRAFTING_CENTI;
		rc = s->set_sim(&flat);
	} else if (s != NULL && s->set_resistance != NULL) {
		rc = s->set_resistance(0u);
	}
	if (rc == 0) {
		trainer_equip_reset_targets(e);
	}
	return rc;
}
