/* SPDX-License-Identifier: Apache-2.0 */
#ifndef STRAP_BLE_H_
#define STRAP_BLE_H_

/*
 * P8b: the SIG Heart Rate Service beside the ANT+ 0x78 master.
 *
 * Enables the controller and starts advertising. Everything after that is
 * driven by the beat generator in main.c, which notifies the HRS at the same
 * instants it advances the ANT+ accumulators - one heart, two radios.
 */
int strap_ble_start(void);

/*
 * True while a phone is connected.
 *
 * P9's suppression rule reads this: a connected phone is receiving the same
 * heart rate over the SIG service, so continuing to broadcast ANT+ spends air
 * on a duplicate. It is exposed rather than acted on internally because
 * stopping an ANT+ channel is the ANT+ side's business, and a BLE module that
 * reached into radiant_core would be the layering this project keeps out.
 */
bool strap_ble_connected(void);

/* Push a heart rate to the SIG service. No-op when nothing is connected. */
void strap_ble_notify_hr(uint8_t bpm);

#endif /* STRAP_BLE_H_ */
