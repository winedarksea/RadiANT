/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_cmd.h - EVERY COMMAND PATH INTO THE TRAINER, for both radios.
 *
 * FE-C control pages 48/49/50/51 and page 55, and the FTMS Control Point's
 * hooks (radiant_ble_ftms_ops), land here, are checked against the session
 * epoch and the FE-C/FTMS control arbiter, reach the driver's hook, and on
 * acceptance update the one equipment state (trainer_equip.h). Kernel-free:
 * tests/glue drives it against the real arbiter and the FTMS core.
 *
 * ─── The mapping ───────────────────────────────────────────────────────────
 *
 *   FTMS op                       hook               FTMS status on 0x01
 *   0x00 Request Control          (arbiter claim)    -
 *   0x01 Reset                    set_sim(flat) or   0x01 Reset
 *                                 set_resistance(0)
 *   0x04 Set Target Resistance    set_resistance     0x07 Target Resistance
 *   0x05 Set Target Power         set_target_power   0x08 Target Power
 *   0x07 Start/Resume             workout            (session report: 0x04)
 *   0x08 Stop/Pause               workout            (session report: 0x02)
 *   0x11 Indoor Bike Simulation   set_sim            0x12 Bike Simulation
 *   0x12 Wheel Circumference      set_user           0x13 Wheel Circumference
 *
 *   FE-C page                     hook               page 71
 *   48 Basic Resistance           set_resistance     PASS / FAIL / REJECTED
 *   49 Target Power               set_target_power   PASS / FAIL / REJECTED
 *   50 Wind Resistance            set_sim            PASS / FAIL / REJECTED
 *   51 Track Resistance           set_sim            PASS / FAIL / REJECTED
 *   55 User Configuration         set_user           none (not a control page)
 *
 * An FE-C page whose capability the trainer lacks is IGNORED - no page 71 and
 * no sequence number (FE-C SS8.12.3.1-2; status 2 NOT_SUPPORTED is one the FE
 * "shall not use"). An FE-C change also owes FTMS subscribers the matching
 * status (0x07/0x08/0x12/0x13), returned in the answer for the caller to send.
 *
 * ─── Resistance scale ──────────────────────────────────────────────────────
 *
 * FTMS 0x04 is a UINT8 at 0.1 resolution (0..25.5). The trainer advertises a
 * Supported Resistance Level Range of 0.0..20.0 in 0.1 steps and maps it
 * linearly onto 0..100 % of the brake: one 0.1 step = 0.5 % = 5 per mille,
 * which is exactly FE-C page 48's step, so the two radios share one scale.
 */
#ifndef TRAINER_CMD_H_
#define TRAINER_CMD_H_

#include <stdbool.h>
#include <stdint.h>

#include <radiant/radiant_ble_ftms.h>
#include <radiant/radiant_ctrl_arbiter.h>

#include "trainer_equip.h"
#include "trainer_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRAINER_RESISTANCE_LEVEL_MAX_DECI 200 /* 20.0 */
#define TRAINER_PERMILLE_PER_LEVEL_DECI   5u

struct trainer_cmd_cfg {
	struct trainer_equip *equip;
	const struct trainer_source_ops *src; /* NULL: nothing is supported */

	/* Kconfig switches that can only take a capability away. */
	bool allow_erg;
	bool allow_resistance;
	bool allow_sim;

	/* The power range commands are clamped/checked against. */
	uint16_t min_power_w;
	uint16_t max_power_w;

	/* The arbiter, or NULL in a build without one (ANT+ only). */
	enum radiant_ctrl_src (*ctrl_owner)(void);
	int (*ctrl_claim)(enum radiant_ctrl_src src, bool explicit_req, uint32_t now_ms);
	bool (*ctrl_gated)(uint8_t fec_page);
};

void trainer_cmd_bind(const struct trainer_cmd_cfg *cfg);

/* What this trainer can do: hook present AND allowed. */
struct trainer_caps {
	bool erg;
	bool resistance;
	bool sim;
	bool user;
};

void trainer_cmd_caps(struct trainer_caps *out);

/* FE-C page 54's capability bits. */
uint8_t trainer_cmd_fec_capabilities(void);

/* The arbiter's gated page mask (RADIANT_CTRL_GATE bits): every control page
 * this trainer implements. */
uint8_t trainer_cmd_ctrl_gates(void);

/* FTMS features, targets and ranges. */
void trainer_cmd_ftms_caps(struct radiant_ble_ftms_caps *out);

/* ── FE-C ──────────────────────────────────────────────────────────────── */

struct trainer_fec_answer {
	bool    answer; /* a page 71 is owed */
	uint8_t status; /* RADIANT_PROFILE_FEC_CMD_STATUS_* */
	/* An FTMS Fitness Machine Status the change owes BLE subscribers. */
	uint8_t ftms_status[RADIANT_BLE_FTMS_STATUS_MAX];
	uint8_t ftms_status_len;
};

/* One FE-C control page (48/49/50/51/55), received under `rx_epoch`. Page 70
 * is not handled here (radiant_profile_fec_tx_handle_request()). */
struct trainer_fec_answer trainer_cmd_fec(const uint8_t *body, uint32_t rx_epoch,
					  uint32_t now_ms);

/* ── FTMS hooks (struct radiant_ble_ftms_ops) ─────────────────────────────
 *
 * A request received before the epoch moved, or while the BLE side does not
 * hold the arbiter token, is -ECANCELED / -EACCES (0x04).
 */
int trainer_cmd_ftms_set_target(const struct radiant_ble_ftms_target *t, uint32_t epoch,
				void *ctx);
int trainer_cmd_ftms_workout(enum radiant_ble_ftms_workout w, uint32_t epoch, void *ctx);
int trainer_cmd_ftms_reset(bool motion_ok, uint32_t epoch, void *ctx);

/* ── Conversions, exposed for the tests ─────────────────────────────────── */
int32_t  trainer_wind_mm_s_from_kph(int32_t kph);
uint16_t trainer_wheel_deci_mm_from_diameter(uint8_t diam_cm, uint8_t offset_mm,
					     bool offset_valid);

#ifdef __cplusplus
}
#endif

#endif /* TRAINER_CMD_H_ */
