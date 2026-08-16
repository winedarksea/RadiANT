/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_ble.h - the BLE half: FTMS (0x1826) and RSC (0x1814) beside two
 * ANT+ masters on one radio.
 *
 * The reasoning, the spec citations and the traps are in treadmill_ble.c's
 * header block, which is where a reader will be when they need them. This file
 * is the four functions main.c calls and the two it implements.
 */

#ifndef TREADMILL_BLE_H_
#define TREADMILL_BLE_H_

#include <stdbool.h>
#include <stdint.h>

struct treadmill_state;

/* Bring the controller up, publish both services and start advertising.
 * Called last from main(), after both ANT+ channels are open, so a failure to
 * advertise cannot be mistaken for a failure to transmit. */
int treadmill_ble_start(void);

/* True while at least one central is connected. Exposed for the log and for a
 * future policy; NOTHING in this application suppresses ANT+ on it, and that
 * is a decision rather than an omission - see main.c's header. */
bool treadmill_ble_connected(void);

/*
 * The state changed. Called from the workqueue on every node tick and on every
 * source report; this module decides for itself how often to actually notify,
 * because FTMS fixes the Treadmill Data rate at "typically once per second"
 * and the client cannot configure it.
 */
void treadmill_ble_state_changed(const struct treadmill_state *s);

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
 * Implemented by main.c, called from this module on the workqueue or from a
 * GATT write callback.
 * ---------------------------------------------------------------------------
 *
 * These are the reason the FTMS control point and FE-C page 51 cannot
 * disagree: both end up in treadmill_state_request_incline() and then in
 * treadmill_source_set_incline(), and neither radio knows the other exists.
 *
 * The return value is the SOURCE's, unchanged - 0, -ENOTSUP or another
 * negative errno - because FTMS's three result codes and FE-C's three command
 * statuses are the same three answers and mapping them belongs at each radio's
 * own edge, not here.
 */
int treadmill_node_set_incline_deci(int16_t deci_pct, int32_t *adopted_centi);
int treadmill_node_set_speed_mm_s(uint32_t mm_s, uint32_t *adopted_mm_s);

/* The live state, for the encoders. Read-only and read on the workqueue. */
const struct treadmill_state *treadmill_node_state(void);

#endif /* TREADMILL_BLE_H_ */
