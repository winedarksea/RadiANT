/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_ble.h - the BLE half: FTMS (0x1826) and RSC (0x1814) beside two
 * ANT+ masters on one radio.
 *
 * The FTMS service is radiant's shared core (<radiant/radiant_ble_ftms.h>);
 * treadmill_ble.c plugs the treadmill into it. This file is what main.c calls
 * and what it implements in return.
 */

#ifndef TREADMILL_BLE_H_
#define TREADMILL_BLE_H_

#include <stdbool.h>
#include <stdint.h>

#include "treadmill_caps.h"
#include "treadmill_session.h"

struct treadmill_state;

/* Bring the controller up, publish both services, set the Device Information
 * Service from radiant_product and the unit's 32-bit `serial`, and start
 * advertising through radiant_ble_adv (which keeps it running across
 * connections). With CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX the name is
 * "<name> <devnum>". Called last from main(), after both ANT+ channels are
 * open, so a failure to advertise cannot be mistaken for a failure to
 * transmit. `caps` (treadmill_caps.c) is what FTMS's Feature characteristic
 * declares and which target op codes are honoured. */
int treadmill_ble_start(uint16_t devnum, uint32_t serial, const struct treadmill_caps *caps);

/* From the node tick: the advertising watchdog. */
void treadmill_ble_tick(void);

/* True while at least one central is connected. Exposed for the log and for a
 * future policy; NOTHING in this application suppresses ANT+ on it, and that
 * is a decision rather than an omission - see main.c's header.
 *
 * IT IS ALSO NOT THE ARBITRATION INPUT, and using it as one would be the
 * natural mistake. Whether a phone is CONNECTED and whether it OWNS THE DECK
 * are different facts: a phone can be connected for a whole session reading
 * Treadmill Data and never once run Request Control, and gating an FE-C
 * controller on this would lock it out for exactly that phone.
 * radiant_ctrl_owner() is the question the control paths ask. */
bool treadmill_ble_connected(void);

/*
 * The state changed. Called from the workqueue on every node tick and on every
 * source report; this module decides for itself how often to actually notify,
 * because FTMS fixes the Treadmill Data rate at "typically once per second"
 * and the client cannot configure it.
 */
void treadmill_ble_state_changed(const struct treadmill_state *s);

/*
 * The session moved (main.c, on the workqueue): Training Status and the
 * machine status it owes go to every FTMS subscriber. `revoke` (a safety-key
 * pull) also takes FTMS control away, with 0xFF to the client that had it.
 */
void treadmill_ble_session_changed(enum treadmill_run prev, enum treadmill_run next,
				   bool revoke);

/* The target incline changed through FE-C page 51 or the grade provider:
 * FTMS subscribers get 0x06 Target Incline Changed, as for an FTMS change. */
void treadmill_ble_target_incline_changed(int32_t centi_pct);

/*
 * THE ZWIFT SEAM: specified, and deliberately not built.
 *
 * An optional hook a future module can register to supply grade from somewhere
 * other than the FTMS control point. If one is registered it is asked on every
 * notify tick for a grade in 0.01 % - FE-C's unit, the finer of the two - and
 * anything other than INT32_MIN replaces whatever the control point last set.
 * INT32_MIN means "no opinion this tick", which a provider that only has a
 * grade sometimes needs and which 0 could not express (0 is flat).
 *
 * WHY IT IS EMPTY. Zwift's native treadmill incline does not travel over FTMS
 * at all: it sends route grade to a KICKR RUN over a *proprietary Wahoo
 * service*, not over `Set Target Inclination` and not over FE-C. The protocol
 * is undocumented, and implementing it would need its own ADR in the shape of
 * docs/decisions/0002-clean-room-policy.md - which is the same ruling
 * docs/ble-running-dynamics-notes.md already carries for the Garmin BLE path.
 * So the seam is named, the shape is fixed, and nothing ships behind it.
 *
 * Returns 0, or -EEXIST if one is already registered (same rule as the source
 * seam: exactly one wins, loudly).
 */
typedef int32_t (*treadmill_ble_grade_provider_t)(void *user);

int treadmill_ble_grade_provider_register(treadmill_ble_grade_provider_t fn,
					  void *user);

/* ---------------------------------------------------------------------------
 * Implemented by main.c.
 * ---------------------------------------------------------------------------
 *
 * The actuating calls that used to be here (treadmill_node_set_*()) are gone:
 * every path that moves the machine is in treadmill_cmd.c, behind the motion
 * rule, and the FTMS core reaches it through the hooks treadmill_ble.c
 * registers.
 */

/* The live state, for the encoders. Read-only, system workqueue. */
const struct treadmill_state *treadmill_node_state(void);

/* The session, for Training Status. Read-only, system workqueue. */
const struct treadmill_session *treadmill_node_session(void);

/* An FTMS Reset succeeded: return the session to IDLE and bump the epoch,
 * without a second FTMS status (the core has notified Reset). Workqueue. */
void treadmill_node_session_reset(void);

/* The session epoch, from ANY thread (an atomic mirror): FTMS records it when
 * a Control Point write is received, on the BT RX thread. */
uint32_t treadmill_node_epoch(void);

#endif /* TREADMILL_BLE_H_ */
