/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Every command path: FE-C pages through trainer_cmd_fec(), FTMS procedures
 * through the real core (radiant_ble_ftms_cp_decide/execute) and the
 * trainer's hooks, both against the real arbiter; and the page-70/71 round
 * trips through the real FE-C master.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_ble_ftms.h>
#include <radiant/radiant_ctrl_arbiter.h>
#include <radiant/radiant_profile_fec_tx.h>

#include "fake.h"
#include "trainer_cmd.h"
#include "trainer_equip.h"

static struct trainer_equip equip;
static struct radiant_ble_ftms_caps ftms_caps;

static const struct radiant_ble_ftms_ops ops = {
	.set_target = trainer_cmd_ftms_set_target,
	.workout = trainer_cmd_ftms_workout,
	.reset = trainer_cmd_ftms_reset,
};

static void bind(const struct trainer_source_ops *src, bool arbiter)
{
	struct trainer_cmd_cfg c;

	fake_reset();
	trainer_equip_init(&equip, (src != NULL) ? src->max_resistance_n : 0u);
	memset(&c, 0, sizeof(c));
	c.equip = &equip;
	c.src = src;
	c.allow_erg = true;
	c.allow_resistance = true;
	c.allow_sim = true;
	c.min_power_w = 0u;
	c.max_power_w = 2000u;
	if (arbiter) {
		c.ctrl_owner = radiant_ctrl_owner;
		c.ctrl_claim = radiant_ctrl_claim;
		c.ctrl_gated = radiant_ctrl_fec_page_is_gated;
	}
	trainer_cmd_bind(&c);
	radiant_ctrl_init(30000u, trainer_cmd_ctrl_gates());
	trainer_cmd_ftms_caps(&ftms_caps);
}

static void page(uint8_t *b, uint8_t p)
{
	memset(b, 0xFF, 8);
	b[0] = p;
}

/* One FTMS procedure, as radiant_ble_ftms.c's cp_work runs it, including the
 * glue's token claim on a grant. */
static struct radiant_ble_ftms_cp_outcome cp(const uint8_t *data, uint16_t len,
					     enum radiant_ble_ftms_owner owner,
					     uint32_t rx_epoch)
{
	struct radiant_ble_ftms_cp_request req = { .data = data, .len = len, .epoch = rx_epoch };
	struct radiant_ble_ftms_session s = { .epoch = equip.epoch };
	struct radiant_ble_ftms_auth auth = { .check = NULL }; /* passive */
	struct radiant_ble_ftms_cp_decision d;
	struct radiant_ble_ftms_cp_outcome o;

	s.run = (equip.run == TRAINER_RUN_RUNNING)   ? RADIANT_BLE_FTMS_RUN_RUNNING
		: (equip.run == TRAINER_RUN_PAUSED)  ? RADIANT_BLE_FTMS_RUN_PAUSED
		: (equip.run == TRAINER_RUN_STOPPED) ? RADIANT_BLE_FTMS_RUN_STOPPED
						     : RADIANT_BLE_FTMS_RUN_IDLE;
	d = radiant_ble_ftms_cp_decide(&req, &s, &ftms_caps, owner, &auth);
	o = radiant_ble_ftms_cp_execute(&d, &ops);
	if (o.grant) {
		(void)radiant_ctrl_claim(RADIANT_CTRL_BLE, true, 0u);
	}
	if (o.release) {
		radiant_ctrl_release(RADIANT_CTRL_BLE);
	}
	if (o.reset_view) {
		(void)trainer_equip_ftms_reset(&equip);
	}
	return o;
}

#define SELF RADIANT_BLE_FTMS_OWNER_SELF

/* ── Capabilities ───────────────────────────────────────────────────────── */

ZTEST(trainer_cmd, test_capabilities_follow_the_hooks)
{
	bind(&fake_full, true);
	zassert_equal(trainer_cmd_fec_capabilities(),
		      RADIANT_PROFILE_FEC_CAP_BASIC_RESISTANCE | RADIANT_PROFILE_FEC_CAP_TARGET_POWER |
			      RADIANT_PROFILE_FEC_CAP_SIMULATION);
	zassert_equal(ftms_caps.targets,
		      RADIANT_BLE_FTMS_TSF_POWER | RADIANT_BLE_FTMS_TSF_RESISTANCE |
			      RADIANT_BLE_FTMS_TSF_BIKE_SIM | RADIANT_BLE_FTMS_TSF_WHEEL_CIRC);
	zassert_true(ftms_caps.power.valid && ftms_caps.resistance.valid);
	zassert_equal(ftms_caps.resistance.max, TRAINER_RESISTANCE_LEVEL_MAX_DECI);
	zassert_equal(ftms_caps.power.max, 2000);
	zassert_equal(trainer_cmd_ctrl_gates(), 0x0Fu, "all four control pages gated");
	/* The data fields the record sends, no more. */
	zassert_equal(ftms_caps.features,
		      RADIANT_BLE_FTMS_FMF_CADENCE | RADIANT_BLE_FTMS_FMF_TOTAL_DISTANCE |
			      RADIANT_BLE_FTMS_FMF_RESISTANCE_LEVEL | RADIANT_BLE_FTMS_FMF_POWER |
			      RADIANT_BLE_FTMS_FMF_ELAPSED_TIME);

	bind(&fake_erg_only, true);
	zassert_equal(trainer_cmd_fec_capabilities(), RADIANT_PROFILE_FEC_CAP_TARGET_POWER);
	zassert_equal(ftms_caps.targets, RADIANT_BLE_FTMS_TSF_POWER);
	zassert_equal(trainer_cmd_ctrl_gates(), RADIANT_CTRL_GATE(RADIANT_CTRL_PAGE_TARGET_POWER));

	bind(NULL, true);
	zassert_equal(trainer_cmd_fec_capabilities(), 0u);
	zassert_equal(ftms_caps.targets, 0u);
}

/* ── FE-C ──────────────────────────────────────────────────────────────── */

ZTEST(trainer_cmd, test_fec_48_49_50_51_reach_the_hooks)
{
	uint8_t b[8];
	struct trainer_fec_answer a;

	bind(&fake_full, true);

	page(b, RADIANT_PROFILE_FEC_PAGE_BASIC_RESISTANCE);
	b[7] = 100u; /* 50 % */
	a = trainer_cmd_fec(b, equip.epoch, 0u);
	zassert_true(a.answer);
	zassert_equal(a.status, RADIANT_PROFILE_FEC_CMD_STATUS_PASS);
	zassert_equal(fake.permille, 500u);
	zassert_equal(equip.mode, TRAINER_EQUIP_BASIC);
	zassert_equal(a.ftms_status_len, 2u);
	zassert_equal(a.ftms_status[0], RADIANT_BLE_FTMS_STATUS_TARGET_RESISTANCE);
	zassert_equal(a.ftms_status[1], 100u, "level 10.0");

	page(b, RADIANT_PROFILE_FEC_PAGE_TARGET_POWER);
	b[6] = 0xE8u; /* 1000 x 0.25 W = 250 W */
	b[7] = 0x03u;
	a = trainer_cmd_fec(b, equip.epoch, 0u);
	zassert_equal(a.status, RADIANT_PROFILE_FEC_CMD_STATUS_PASS);
	zassert_equal(fake.power_w, 250u);
	zassert_equal(equip.mode, TRAINER_EQUIP_ERG);
	zassert_equal(a.ftms_status[0], RADIANT_BLE_FTMS_STATUS_TARGET_POWER);
	zassert_equal(a.ftms_status[1] | (a.ftms_status[2] << 8), 250);

	page(b, RADIANT_PROFILE_FEC_PAGE_WIND_RESISTANCE);
	b[5] = 60u;  /* 0.60 kg/m */
	b[6] = 137u; /* +10 km/h */
	b[7] = 90u;
	a = trainer_cmd_fec(b, equip.epoch, 0u);
	zassert_equal(a.status, RADIANT_PROFILE_FEC_CMD_STATUS_PASS);
	zassert_equal(fake.sim.cw_centi, 60u);
	zassert_equal(fake.sim.wind_mm_s, 2778);
	zassert_equal(fake.sim.drafting_centi, 90u);
	zassert_equal(equip.mode, TRAINER_EQUIP_SIM);

	page(b, RADIANT_PROFILE_FEC_PAGE_TRACK_RESISTANCE);
	b[5] = 0x4Cu; /* 20300 -> +3.00 % */
	b[6] = 0x4Fu;
	b[7] = 80u;   /* 0.004 */
	a = trainer_cmd_fec(b, equip.epoch, 0u);
	zassert_equal(a.status, RADIANT_PROFILE_FEC_CMD_STATUS_PASS);
	zassert_equal(fake.sim.grade_centi_pct, 300);
	zassert_equal(fake.sim.crr_e5, 400u);
	zassert_equal(fake.sim.cw_centi, 60u, "page 51 keeps page 50's settings");
	zassert_equal(a.ftms_status[0], RADIANT_BLE_FTMS_STATUS_BIKE_SIM);
	zassert_equal(a.ftms_status_len, 7u);

	/* Grade sentinel = flat; crr sentinel = default. */
	page(b, RADIANT_PROFILE_FEC_PAGE_TRACK_RESISTANCE);
	a = trainer_cmd_fec(b, equip.epoch, 0u);
	zassert_equal(fake.sim.grade_centi_pct, 0);
	zassert_equal(fake.sim.crr_e5, TRAINER_SIM_DEFAULT_CRR_E5);

	/* A hook failure is FAIL and changes nothing. */
	fake.rc = -EIO;
	page(b, RADIANT_PROFILE_FEC_PAGE_TARGET_POWER);
	b[6] = 0x20u;
	b[7] = 0x03u;
	a = trainer_cmd_fec(b, equip.epoch, 0u);
	zassert_true(a.answer);
	zassert_equal(a.status, RADIANT_PROFILE_FEC_CMD_STATUS_FAIL);
	zassert_equal(equip.mode, TRAINER_EQUIP_SIM);
	zassert_equal(a.ftms_status_len, 0u);
}

ZTEST(trainer_cmd, test_fec_power_is_clamped_to_the_range)
{
	uint8_t b[8];

	bind(&fake_full, true);
	page(b, RADIANT_PROFILE_FEC_PAGE_TARGET_POWER);
	b[6] = 0xFFu; /* 16383 W */
	b[7] = 0xFFu;
	(void)trainer_cmd_fec(b, equip.epoch, 0u);
	zassert_equal(fake.power_w, 2000u);
}

ZTEST(trainer_cmd, test_fec_unsupported_page_is_ignored_not_answered)
{
	uint8_t b[8];
	struct trainer_fec_answer a;

	bind(&fake_erg_only, true);
	page(b, RADIANT_PROFILE_FEC_PAGE_TRACK_RESISTANCE);
	a = trainer_cmd_fec(b, equip.epoch, 0u);
	zassert_false(a.answer, "SS8.12.3.1: ignored, no page 71");
	page(b, RADIANT_PROFILE_FEC_PAGE_BASIC_RESISTANCE);
	a = trainer_cmd_fec(b, equip.epoch, 0u);
	zassert_false(a.answer);
	zassert_equal(fake.n_resistance + fake.n_sim, 0);
	page(b, 0x40u);
	zassert_false(trainer_cmd_fec(b, equip.epoch, 0u).answer);
}

ZTEST(trainer_cmd, test_fec_queued_before_a_local_stop_is_rejected)
{
	uint8_t b[8];
	struct trainer_fec_answer a;
	uint32_t rx = equip.epoch;

	bind(&fake_full, true);
	rx = equip.epoch;
	(void)trainer_equip_event(&equip, TRAINER_EVT_CONSOLE_STOP);

	page(b, RADIANT_PROFILE_FEC_PAGE_TARGET_POWER);
	b[6] = 0x20u;
	b[7] = 0x03u;
	a = trainer_cmd_fec(b, rx, 0u);
	zassert_true(a.answer);
	zassert_equal(a.status, RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED);
	zassert_equal(fake.n_power, 0, "never reached the brake");
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_NONE, "and took no token");

	/* After a console reset too. */
	rx = equip.epoch;
	(void)trainer_equip_event(&equip, TRAINER_EVT_CONSOLE_RESET);
	page(b, RADIANT_PROFILE_FEC_PAGE_TRACK_RESISTANCE);
	a = trainer_cmd_fec(b, rx, 0u);
	zassert_equal(a.status, RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED);
	zassert_equal(fake.n_sim, 0);
}

ZTEST(trainer_cmd, test_fec_is_rejected_while_ftms_holds_control)
{
	uint8_t b[8];
	static const uint8_t req_ctrl[] = { RADIANT_BLE_FTMS_OP_REQUEST_CONTROL };
	struct trainer_fec_answer a;

	bind(&fake_full, true);
	/* An FE-C page claims implicitly... */
	page(b, RADIANT_PROFILE_FEC_PAGE_TARGET_POWER);
	b[6] = 0x20u;
	b[7] = 0x03u;
	a = trainer_cmd_fec(b, equip.epoch, 0u);
	zassert_equal(a.status, RADIANT_PROFILE_FEC_CMD_STATUS_PASS);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_ANT);

	/* ...an FTMS Request Control preempts it (explicit beats implicit)... */
	zassert_equal(cp(req_ctrl, 1u, RADIANT_BLE_FTMS_OWNER_NONE, equip.epoch).result,
		      RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_BLE);

	/* ...and the next FE-C command is REJECTED, without reaching the brake. */
	a = trainer_cmd_fec(b, equip.epoch, 100u);
	zassert_equal(a.status, RADIANT_PROFILE_FEC_CMD_STATUS_REJECTED);
	zassert_equal(fake.n_power, 1);

	/* Page 55 is not gated: still applied. */
	page(b, RADIANT_PROFILE_FEC_PAGE_USER_CONFIG);
	b[1] = 0x58u; /* 7000 = 70.00 kg */
	b[2] = 0x1Bu;
	a = trainer_cmd_fec(b, equip.epoch, 100u);
	zassert_false(a.answer, "page 55 is not a control page");
	zassert_equal(fake.n_user, 1);
	zassert_equal(equip.user.user_weight_10g, 7000u);
}

ZTEST(trainer_cmd, test_fec_55_applies_with_defaults_for_sentinels)
{
	uint8_t b[8];
	struct trainer_fec_answer a;

	bind(&fake_full, true);
	page(b, RADIANT_PROFILE_FEC_PAGE_USER_CONFIG);
	b[1] = 0x40u; /* 0x1F40 = 80.00 kg */
	b[2] = 0x1Fu;
	b[4] = 0x45u; /* offset 5 mm, bike weight LSN 4 */
	b[5] = 0x0Bu; /* bike weight 0x0B4 = 180 x 0.05 = 9 kg */
	b[6] = 66u;   /* 0.66 m + 5 mm = 665 mm */
	b[7] = 0x00u; /* gear ratio invalid */
	a = trainer_cmd_fec(b, equip.epoch, 0u);
	zassert_false(a.answer);
	zassert_equal(fake.user.user_weight_10g, 8000u);
	zassert_equal(fake.user.bike_weight_50g, 180u);
	zassert_equal(fake.user.wheel_circ_deci_mm, 20892u, "665 mm x pi = 2089.2 mm");
	zassert_equal(a.ftms_status[0], RADIANT_BLE_FTMS_STATUS_WHEEL_CIRC);

	page(b, RADIANT_PROFILE_FEC_PAGE_USER_CONFIG);
	b[7] = 0x00u;
	(void)trainer_cmd_fec(b, equip.epoch, 0u);
	zassert_equal(fake.user.user_weight_10g, TRAINER_USER_DEFAULT_WEIGHT_10G);
	zassert_equal(fake.user.bike_weight_50g, TRAINER_USER_DEFAULT_BIKE_50G);
	zassert_equal(fake.user.wheel_circ_deci_mm, TRAINER_USER_DEFAULT_WHEEL_DECI_MM);
}

/* ── FTMS: the CP opcode -> hook -> status table ────────────────────────── */

ZTEST(trainer_cmd, test_ftms_targets_map_to_hooks_and_statuses)
{
	static const uint8_t req_ctrl[] = { RADIANT_BLE_FTMS_OP_REQUEST_CONTROL };
	static const uint8_t power[] = { RADIANT_BLE_FTMS_OP_SET_POWER, 0xFA, 0x00 };
	static const uint8_t resist[] = { RADIANT_BLE_FTMS_OP_SET_RESISTANCE, 40 };
	static const uint8_t sim[] = { RADIANT_BLE_FTMS_OP_SET_BIKE_SIM,
				       0xE8, 0x03, /* wind 1.000 m/s */
				       0x2C, 0x01, /* grade 3.00 % */
				       40,         /* crr 0.0040 */
				       51 };       /* cw 0.51 */
	static const uint8_t wheel[] = { RADIANT_BLE_FTMS_OP_SET_WHEEL_CIRC, 0x3A, 0x52 };
	struct radiant_ble_ftms_cp_outcome o;

	bind(&fake_full, true);
	zassert_equal(cp(req_ctrl, 1u, RADIANT_BLE_FTMS_OWNER_NONE, 0u).result,
		      RADIANT_BLE_FTMS_RESULT_SUCCESS);

	o = cp(power, sizeof(power), SELF, equip.epoch);
	zassert_equal(o.result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.power_w, 250u);
	zassert_equal(o.status[0], RADIANT_BLE_FTMS_STATUS_TARGET_POWER);
	zassert_equal(equip.mode, TRAINER_EQUIP_ERG);

	o = cp(resist, sizeof(resist), SELF, equip.epoch);
	zassert_equal(o.result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.permille, 200u, "4.0 = 20 %");
	zassert_equal(o.status[0], RADIANT_BLE_FTMS_STATUS_TARGET_RESISTANCE);
	zassert_equal(equip.mode, TRAINER_EQUIP_BASIC);

	o = cp(sim, sizeof(sim), SELF, equip.epoch);
	zassert_equal(o.result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.sim.wind_mm_s, 1000);
	zassert_equal(fake.sim.grade_centi_pct, 300);
	zassert_equal(fake.sim.crr_e5, 400u);
	zassert_equal(fake.sim.cw_centi, 51u);
	zassert_equal(o.status[0], RADIANT_BLE_FTMS_STATUS_BIKE_SIM);
	zassert_equal(o.status_len, 7u);
	zassert_equal(equip.mode, TRAINER_EQUIP_SIM);

	o = cp(wheel, sizeof(wheel), SELF, equip.epoch);
	zassert_equal(o.result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.user.wheel_circ_deci_mm, 21050u);
	zassert_equal(o.status[0], RADIANT_BLE_FTMS_STATUS_WHEEL_CIRC);
}

ZTEST(trainer_cmd, test_ftms_refusals)
{
	static const uint8_t req_ctrl[] = { RADIANT_BLE_FTMS_OP_REQUEST_CONTROL };
	static const uint8_t too_much[] = { RADIANT_BLE_FTMS_OP_SET_POWER, 0xD1, 0x07 };
	static const uint8_t level_high[] = { RADIANT_BLE_FTMS_OP_SET_RESISTANCE, 201 };
	static const uint8_t power[] = { RADIANT_BLE_FTMS_OP_SET_POWER, 100, 0 };
	static const uint8_t no_wheel[] = { RADIANT_BLE_FTMS_OP_SET_WHEEL_CIRC, 0, 0 };
	static const uint8_t sim[] = { RADIANT_BLE_FTMS_OP_SET_BIKE_SIM, 0, 0, 0, 0, 0, 0 };
	struct radiant_ble_ftms_cp_outcome o;
	uint32_t rx;

	bind(&fake_full, true);
	/* Without control: 0x05. */
	zassert_equal(cp(power, sizeof(power), RADIANT_BLE_FTMS_OWNER_NONE, 0u).result,
		      RADIANT_BLE_FTMS_RESULT_CONTROL_NOT_PERMITTED);
	(void)cp(req_ctrl, 1u, RADIANT_BLE_FTMS_OWNER_NONE, 0u);

	/* Outside the Supported Ranges: 0x03, nothing reaches the brake. */
	zassert_equal(cp(too_much, sizeof(too_much), SELF, equip.epoch).result,
		      RADIANT_BLE_FTMS_RESULT_INVALID_PARAM);
	zassert_equal(cp(level_high, sizeof(level_high), SELF, equip.epoch).result,
		      RADIANT_BLE_FTMS_RESULT_INVALID_PARAM);
	zassert_equal(cp(no_wheel, sizeof(no_wheel), SELF, equip.epoch).result,
		      RADIANT_BLE_FTMS_RESULT_INVALID_PARAM);
	zassert_equal(fake.n_power + fake.n_resistance + fake.n_user, 0);

	/* A hook failure: 0x04 and no status. */
	fake.rc = -EIO;
	o = cp(power, sizeof(power), SELF, equip.epoch);
	zassert_equal(o.result, RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED);
	zassert_equal(o.status_len, 0u);
	fake.rc = 0;

	/* Queued before a local stop: 0x04 without reaching the brake. */
	rx = equip.epoch;
	(void)trainer_equip_event(&equip, TRAINER_EVT_CONSOLE_STOP);
	fake.n_power = 0;
	fake.n_sim = 0;
	zassert_equal(cp(power, sizeof(power), SELF, rx).result,
		      RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED);
	zassert_equal(cp(sim, sizeof(sim), SELF, rx).result,
		      RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED);
	zassert_equal(fake.n_power + fake.n_sim, 0);

	/* An op code the trainer lacks: 0x02. */
	bind(&fake_erg_only, true);
	(void)cp(req_ctrl, 1u, RADIANT_BLE_FTMS_OWNER_NONE, 0u);
	zassert_equal(cp(sim, sizeof(sim), SELF, equip.epoch).result,
		      RADIANT_BLE_FTMS_RESULT_OP_NOT_SUPPORTED);
}

ZTEST(trainer_cmd, test_ftms_workout_and_reset)
{
	static const uint8_t req_ctrl[] = { RADIANT_BLE_FTMS_OP_REQUEST_CONTROL };
	static const uint8_t start[] = { RADIANT_BLE_FTMS_OP_START_RESUME };
	static const uint8_t stop[] = { RADIANT_BLE_FTMS_OP_STOP_PAUSE, RADIANT_BLE_FTMS_PARAM_STOP };
	static const uint8_t pause[] = { RADIANT_BLE_FTMS_OP_STOP_PAUSE, RADIANT_BLE_FTMS_PARAM_PAUSE };
	static const uint8_t reset[] = { RADIANT_BLE_FTMS_OP_RESET };
	static const uint8_t power[] = { RADIANT_BLE_FTMS_OP_SET_POWER, 100, 0 };
	struct radiant_ble_ftms_cp_outcome o;
	uint32_t rx;

	bind(&fake_full, true);
	(void)cp(req_ctrl, 1u, RADIANT_BLE_FTMS_OWNER_NONE, 0u);

	zassert_equal(cp(start, 1u, SELF, equip.epoch).result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.workout, TRAINER_WORKOUT_START);
	/* Success means accepted: the session follows the driver's report. */
	zassert_equal(equip.run, TRAINER_RUN_IDLE);
	zassert_equal(cp(pause, 2u, SELF, equip.epoch).result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.workout, TRAINER_WORKOUT_PAUSE);

	/* Stop passes even when queued before a local stop. */
	rx = equip.epoch;
	(void)trainer_equip_event(&equip, TRAINER_EVT_CONSOLE_STOP);
	zassert_equal(cp(stop, 2u, SELF, rx).result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.workout, TRAINER_WORKOUT_STOP);
	zassert_equal(cp(start, 1u, SELF, rx).result, RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED,
		      "a stale Start is refused");

	/* Reset: targets to default (flat road), view zeroed, control released. */
	(void)cp(power, sizeof(power), SELF, equip.epoch);
	zassert_equal(equip.mode, TRAINER_EQUIP_ERG);
	o = cp(reset, 1u, SELF, equip.epoch);
	zassert_equal(o.result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(o.status[0], RADIANT_BLE_FTMS_STATUS_RESET);
	zassert_true(o.release && o.reset_view);
	zassert_equal(equip.mode, TRAINER_EQUIP_FREE);
	zassert_equal(fake.sim.grade_centi_pct, 0);
	zassert_equal(radiant_ctrl_owner(), RADIANT_CTRL_NONE);
	zassert_equal(cp(power, sizeof(power), RADIANT_BLE_FTMS_OWNER_NONE, equip.epoch).result,
		      RADIANT_BLE_FTMS_RESULT_CONTROL_NOT_PERMITTED);

	/* No workout hook: the mandatory op codes answer 0x04, never 0x02. */
	bind(&fake_erg_only, true);
	(void)cp(req_ctrl, 1u, RADIANT_BLE_FTMS_OWNER_NONE, 0u);
	zassert_equal(cp(start, 1u, SELF, equip.epoch).result,
		      RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED);
	zassert_equal(cp(stop, 2u, SELF, equip.epoch).result,
		      RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED);
}

ZTEST(trainer_cmd, test_ftms_hook_needs_the_ble_token)
{
	static const uint8_t power[] = { RADIANT_BLE_FTMS_OP_SET_POWER, 100, 0 };

	bind(&fake_full, true);
	/* A client the core thinks has control, but the arbiter was revoked
	 * underneath it: the hook refuses (0x04), the brake is untouched. */
	(void)radiant_ctrl_claim(RADIANT_CTRL_ANT, false, 0u);
	zassert_equal(cp(power, sizeof(power), SELF, equip.epoch).result,
		      RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED);
	zassert_equal(fake.n_power, 0);

	/* Without an arbiter (ANT+-only build), no token check. */
	bind(&fake_full, false);
	zassert_equal(cp(power, sizeof(power), SELF, equip.epoch).result,
		      RADIANT_BLE_FTMS_RESULT_SUCCESS);
}

/* ── Reset: the brake release chosen from the declared capabilities (R5) ── */

static int reset_via_ftms(void)
{
	static const uint8_t req_ctrl[] = { RADIANT_BLE_FTMS_OP_REQUEST_CONTROL };
	static const uint8_t reset[] = { RADIANT_BLE_FTMS_OP_RESET };

	(void)cp(req_ctrl, 1u, RADIANT_BLE_FTMS_OWNER_NONE, 0u);
	return cp(reset, 1u, SELF, equip.epoch).result;
}

static int f2_power(uint16_t w)
{
	fake.n_power++;
	fake.power_w = w;
	return fake.rc;
}

static int f2_resistance(uint16_t pm)
{
	fake.n_resistance++;
	fake.permille = pm;
	return fake.rc;
}

static int f2_sim(const struct trainer_sim_params *p)
{
	fake.n_sim++;
	fake.sim = *p;
	return fake.rc;
}

ZTEST(trainer_cmd, test_reset_on_each_source_shape)
{
	static const struct trainer_source_ops resist_erg = {
		.set_target_power = f2_power, .set_resistance = f2_resistance,
	};
	static const struct trainer_source_ops sim_hook = {
		.set_target_power = f2_power, .set_sim = f2_sim,
	};
	struct trainer_cmd_cfg c;

	/* Full: simulation declared -> flat road, free ride. */
	bind(&fake_full, true);
	equip.mode = TRAINER_EQUIP_ERG;
	zassert_equal(reset_via_ftms(), RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.n_sim, 1);
	zassert_equal(fake.sim.grade_centi_pct, 0);
	zassert_equal(fake.n_resistance + fake.n_power, 0);
	zassert_equal(equip.mode, TRAINER_EQUIP_FREE);

	/* Resistance + ERG: resistance 0. */
	bind(&resist_erg, true);
	equip.mode = TRAINER_EQUIP_ERG;
	zassert_equal(reset_via_ftms(), RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.n_resistance, 1);
	zassert_equal(fake.permille, 0u);
	zassert_equal(fake.n_power, 0);
	zassert_equal(equip.mode, TRAINER_EQUIP_FREE);

	/* ERG only: the minimum target, and the state says ERG at it. */
	bind(&fake_erg_only, true);
	equip.mode = TRAINER_EQUIP_ERG;
	equip.target_power_w = 250u;
	zassert_equal(reset_via_ftms(), RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.n_power, 1);
	zassert_equal(fake.power_w, 0u);
	zassert_equal(equip.mode, TRAINER_EQUIP_ERG);
	zassert_equal(equip.target_power_w, 0u);

	/* A set_sim hook with simulation NOT declared (CAP_SIM=n): never used;
	 * ERG at the minimum instead. */
	fake_reset();
	trainer_equip_init(&equip, 0u);
	memset(&c, 0, sizeof(c));
	c.equip = &equip;
	c.src = &sim_hook;
	c.allow_erg = true;
	c.allow_resistance = true;
	c.allow_sim = false;
	c.min_power_w = 50u;
	c.max_power_w = 2000u;
	c.ctrl_owner = radiant_ctrl_owner;
	c.ctrl_claim = radiant_ctrl_claim;
	c.ctrl_gated = radiant_ctrl_fec_page_is_gated;
	trainer_cmd_bind(&c);
	radiant_ctrl_init(30000u, trainer_cmd_ctrl_gates());
	trainer_cmd_ftms_caps(&ftms_caps);
	zassert_equal(reset_via_ftms(), RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.n_sim, 0, "an undeclared mode is never entered");
	zassert_equal(fake.power_w, 50u);

	/* Nothing declared can release the brake: 0x04, mode kept. */
	bind(NULL, true);
	equip.mode = TRAINER_EQUIP_BASIC;
	zassert_equal(reset_via_ftms(), RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED);
	zassert_equal(equip.mode, TRAINER_EQUIP_BASIC);

	/* A release the brake refuses: 0x04, mode kept. */
	bind(&fake_full, true);
	equip.mode = TRAINER_EQUIP_ERG;
	fake.rc = -EIO;
	zassert_equal(reset_via_ftms(), RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED);
	zassert_equal(equip.mode, TRAINER_EQUIP_ERG);
}

ZTEST(trainer_cmd, test_bike_sim_status_carries_the_clamped_grade)
{
	static const uint8_t req_ctrl[] = { RADIANT_BLE_FTMS_OP_REQUEST_CONTROL };
	/* grade +300.00 % (0x7530), beyond FE-C's +200.00 % */
	static const uint8_t sim[] = { RADIANT_BLE_FTMS_OP_SET_BIKE_SIM, 0, 0, 0x30, 0x75, 40, 51 };
	struct radiant_ble_ftms_cp_outcome o;

	bind(&fake_full, true);
	(void)cp(req_ctrl, 1u, RADIANT_BLE_FTMS_OWNER_NONE, 0u);
	o = cp(sim, sizeof(sim), SELF, equip.epoch);
	zassert_equal(o.result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.sim.grade_centi_pct, 20000);
	zassert_equal((int16_t)(o.status[3] | (o.status[4] << 8)), 20000,
		      "0x12 reports the applied grade, not the requested one");
}

ZTEST(trainer_cmd, test_start_while_pedalling_is_a_no_op)
{
	static const uint8_t req_ctrl[] = { RADIANT_BLE_FTMS_OP_REQUEST_CONTROL };
	static const uint8_t start[] = { RADIANT_BLE_FTMS_OP_START_RESUME };
	struct trainer_report r;

	bind(&fake_full, true);
	zassert_true(ftms_caps.passive);
	memset(&r, 0, sizeof(r));
	r.pedalling = true;
	(void)trainer_equip_report(&equip, &r);
	zassert_equal(equip.run, TRAINER_RUN_RUNNING);
	(void)cp(req_ctrl, 1u, RADIANT_BLE_FTMS_OWNER_NONE, 0u);
	zassert_equal(cp(start, 1u, SELF, equip.epoch).result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(fake.n_workout, 0, "already running: no hook");
}

ZTEST_SUITE(trainer_cmd, NULL, NULL, NULL, NULL, NULL);
