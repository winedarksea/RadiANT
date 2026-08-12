/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ble_coex_load.h - start a BLE advertiser that competes for the radio.
 *
 * The bench instrument for the multiprotocol plan's P3 coexistence gate. See
 * ble_coex_load.c for why a linked-in-but-idle controller is not a second stack
 * and what this is deliberately not (the heart-rate strap).
 */

#ifndef ANT_DONGLE_BLE_COEX_LOAD_H_
#define ANT_DONGLE_BLE_COEX_LOAD_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring the controller up and start advertising. Blocks until the controller is
 * ready. Returns 0, or a negative errno from bt_enable()/bt_le_adv_start().
 *
 * Call AFTER the radio backend is initialised: the backend takes an explicit
 * HFXO request that the P0 spike showed is load-bearing, and it has to be held
 * before the controller starts cycling the clock at timeslot edges.
 */
int ble_coex_load_start(void);

#ifdef __cplusplus
}
#endif

#endif /* ANT_DONGLE_BLE_COEX_LOAD_H_ */
