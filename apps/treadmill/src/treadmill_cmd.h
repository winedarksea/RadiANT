/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_cmd.h - EVERY PATH THAT CAN MOVE THE MACHINE, in one file.
 *
 * This is the only module that includes treadmill_source_ctrl.h, and every one
 * of its calls into the source's actuating entry points sits in one of four
 * DOORS - treadmill_cmd_drive_speed/_incline/_start/_stop - each of which calls
 * treadmill_motion_permitted() on its first line and returns before the source
 * if it refuses. The paths:
 *
 *   FE-C page 51                 treadmill_cmd_fec_track_resistance()
 *   FE-C page 50                 treadmill_cmd_fec_wind()   (moves nothing)
 *   the BLE grade provider       treadmill_cmd_grade_provider()
 *   FTMS Set Target Speed        treadmill_cmd_ftms_set_target()  (0x02)
 *   FTMS Set Target Inclination  treadmill_cmd_ftms_set_target()  (0x03)
 *   FTMS Start/Resume, Stop/Pause treadmill_cmd_ftms_workout()
 *   FTMS Reset                   treadmill_cmd_ftms_reset()
 *   FTMS motion authorisation    treadmill_cmd_ftms_auth()  (the core asks
 *                                before it executes; the doors ask again with
 *                                the epoch recorded at receipt)
 *
 * Kernel-free: no Zephyr header, no lock, no clock. System workqueue only,
 * like the state and the session it is bound to. apps/treadmill/tests/pages
 * links it against a fake source and the real arbiter.
 */

#ifndef TREADMILL_CMD_H_
#define TREADMILL_CMD_H_

#include <stdbool.h>
#include <stdint.h>

#include <radiant/radiant_ble_ftms.h>
#include <radiant/radiant_ctrl_arbiter.h>

#include "treadmill_session.h"
#include "treadmill_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The node's state and session, and the arbiter:
 *
 *   ctrl_owner  radiant_ctrl_owner under exclusive arbitration; NULL means no
 *               arbitration (TREADMILL_CTRL_POLICY_NONE) and every token holds.
 *   fec_claim   the FE-C implicit claim (radiant_ctrl_claim(ANT, false, now)
 *               == 0); NULL means no arbitration. Called only AFTER a page has
 *               passed the epoch check, so a stale page cannot re-claim.
 *
 * `incline_cmd` and `speed_cmd` are the DERIVED capabilities (treadmill_caps.c):
 * a capability the machine does not advertise is refused as not supported
 * before anything else.
 */
struct treadmill_cmd_cfg {
	struct treadmill_state *state;
	struct treadmill_session *session;
	enum radiant_ctrl_src (*ctrl_owner)(void);
	bool (*fec_claim)(void);
	bool incline_cmd;
	bool speed_cmd;
};

void treadmill_cmd_bind(const struct treadmill_cmd_cfg *cfg);

/* (d) for an FTMS path and for the grade provider: the arbiter's owner is BLE
 * (the FTMS core has separately checked this client holds FTMS control). The
 * grade provider uses the same token as FTMS, never a weaker one. */
bool treadmill_cmd_ftms_token(void);
bool treadmill_cmd_grade_token(void);

/*
 * FE-C page 51, a simulation grade in 0.01 % (the 0xFFFF "assume flat"
 * sentinel already decoded to 0), received under `rx_epoch`. In order:
 * NOT_SUPPORTED without the incline capability (the page is then ungated);
 * REJECTED if the epoch moved since receipt (before any claim); REJECTED if
 * the implicit claim fails; REJECTED if the motion rule refuses; then the
 * clamped grade goes to the source (PASS / NOT_SUPPORTED / FAIL).
 */
uint8_t treadmill_cmd_fec_track_resistance(int32_t grade_centi, uint32_t rx_epoch);

/* FE-C page 50: moves nothing, but it is half of the simulation parameter set
 * and follows page 51's capability, epoch and claim answers. */
uint8_t treadmill_cmd_fec_wind(uint32_t rx_epoch);

/* The BLE grade provider's value, 0.01 %, a simulation grade like page 51.
 * 0 when applied, -EACCES/-ECANCELED when the rule refuses it (the value is
 * DROPPED), -ENOTSUP without the incline capability, else the source's rc. */
int treadmill_cmd_grade_provider(int32_t grade_centi);

/* The FTMS machine hooks (struct radiant_ble_ftms_ops / _auth). `epoch` is the
 * session epoch at receipt; `ctx` is unused. */
int treadmill_cmd_ftms_set_target(const struct radiant_ble_ftms_target *t, uint32_t epoch,
				  void *ctx);
int treadmill_cmd_ftms_workout(enum radiant_ble_ftms_workout w, uint32_t epoch, void *ctx);
int treadmill_cmd_ftms_reset(bool motion_ok, uint32_t epoch, void *ctx);
enum radiant_ble_ftms_verdict treadmill_cmd_ftms_auth(const struct radiant_ble_ftms_motion *m,
						      const struct radiant_ble_ftms_session *s,
						      void *ctx);

/*
 * THE FOUR DOORS to the source. Public so their own gate can be tested
 * directly; nothing outside this module calls them. Each returns the gate's
 * refusal, or the source's answer.
 */
int treadmill_cmd_drive_speed(enum treadmill_motion_kind kind, uint32_t mm_s, uint32_t epoch,
			      bool token);
int treadmill_cmd_drive_incline(enum treadmill_motion_kind kind, int32_t centi, uint32_t epoch,
				bool token);
int treadmill_cmd_drive_start(bool resume, uint32_t epoch, bool token);
int treadmill_cmd_drive_stop(bool pause);

#ifdef __cplusplus
}
#endif

#endif /* TREADMILL_CMD_H_ */
