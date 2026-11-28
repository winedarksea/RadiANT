/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_cmd.h - EVERY PATH THAT CAN MOVE THE MACHINE, in one file.
 *
 * This is the only module that includes treadmill_source_ctrl.h, and every one
 * of its calls into the source's actuating entry points sits in one of four
 * helpers (drive_speed, drive_incline, drive_start, drive_stop) that call
 * treadmill_motion_permitted() first. The paths:
 *
 *   FE-C page 51                 treadmill_cmd_fec_track_resistance()
 *   FE-C page 50                 treadmill_cmd_fec_wind()   (moves nothing)
 *   the BLE grade provider       treadmill_cmd_grade_provider()
 *   FTMS Set Target Speed        treadmill_cmd_ftms_set_target()  (0x02)
 *   FTMS Set Target Inclination  treadmill_cmd_ftms_set_target()  (0x03)
 *   FTMS Start/Resume, Stop/Pause treadmill_cmd_ftms_workout()
 *   FTMS Reset                   treadmill_cmd_ftms_reset()
 *   FTMS motion authorisation    treadmill_cmd_ftms_auth()  (the core asks
 *                                before it executes; the drive_* helpers ask
 *                                again, so no FTMS hook can be reached around
 *                                the gate even by a future caller)
 *
 * Kernel-free: no Zephyr header, no lock, no clock. System workqueue only,
 * like the state and the session it is bound to. apps/treadmill/tests/pages
 * links it against a fake source and drives a negative case per path.
 */

#ifndef TREADMILL_CMD_H_
#define TREADMILL_CMD_H_

#include <stdbool.h>
#include <stdint.h>

#include <radiant/radiant_ble_ftms.h>

#include "treadmill_session.h"
#include "treadmill_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The node's state and session, and how an FTMS path learns whether it holds
 * the cross-transport token (NULL: always - TREADMILL_CTRL_POLICY_NONE).
 * `incline_cmd` and `speed_cmd` are the DERIVED capabilities
 * (treadmill_caps.c): a capability the machine does not advertise is refused
 * as not supported before anything else, matching what it declared.
 */
struct treadmill_cmd_cfg {
	struct treadmill_state *state;
	struct treadmill_session *session;
	bool (*ftms_token)(void);
	bool incline_cmd;
	bool speed_cmd;
};

void treadmill_cmd_bind(const struct treadmill_cmd_cfg *cfg);

/*
 * FE-C page 51, a simulation grade in 0.01 % (the 0xFFFF "assume flat"
 * sentinel already decoded to 0). `rx_epoch` is the session epoch when the
 * page was received; `token` is the result of the arbiter's implicit claim.
 * Returns the page 71 status: PASS, NOT_SUPPORTED (no incline capability),
 * REJECTED (no token, not authorised, or cancelled by a local stop), FAIL.
 * The grade CLAMPS to the machine range; refused, the target is untouched.
 */
uint8_t treadmill_cmd_fec_track_resistance(int32_t grade_centi, uint32_t rx_epoch,
					   bool token);

/* FE-C page 50: moves nothing on a treadmill and is acknowledged PASS, but it
 * is half of the simulation parameter set and is refused (REJECTED) exactly
 * when page 51 would be for the token or the epoch. */
uint8_t treadmill_cmd_fec_wind(uint32_t rx_epoch, bool token);

/* The BLE grade provider's value, 0.01 %, a simulation grade like page 51.
 * 0 when applied, -EACCES/-ECANCELED when the rule refuses it (the value is
 * DROPPED), -ENOTSUP without the incline capability, else the source's rc. */
int treadmill_cmd_grade_provider(int32_t grade_centi, bool token);

/* The FTMS machine hooks (struct radiant_ble_ftms_ops / _auth). `ctx` is
 * unused. */
int treadmill_cmd_ftms_set_target(const struct radiant_ble_ftms_target *t, void *ctx);
int treadmill_cmd_ftms_workout(enum radiant_ble_ftms_workout w, void *ctx);
int treadmill_cmd_ftms_reset(bool motion_ok, void *ctx);
enum radiant_ble_ftms_verdict treadmill_cmd_ftms_auth(const struct radiant_ble_ftms_motion *m,
						      const struct radiant_ble_ftms_session *s,
						      void *ctx);

#ifdef __cplusplus
}
#endif

#endif /* TREADMILL_CMD_H_ */
