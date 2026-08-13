/* SPDX-License-Identifier: Apache-2.0 */
#ifndef STRAP_BLE_H_
#define STRAP_BLE_H_

/*
 * P8b: the SIG Heart Rate Service beside the ANT+ 0x78 master.
 *
 * Enables the controller and starts advertising; the beat generator in
 * main.c notifies HRS at the same instants it advances the ANT+
 * accumulators.
 *
 * Not Garmin-protocol-compatible and no running dynamics - this is a
 * coexistence load generator only (SIG HRS, not Garmin's proprietary
 * Multi-Link/GFDI). See docs/ble-running-dynamics-notes.md.
 */
int strap_ble_start(void);

/*
 * True while a phone is connected. P9 reads this to suppress the ANT+
 * duplicate broadcast. Exposed rather than acted on internally, since
 * stopping an ANT+ channel is the ANT+ side's business.
 */
bool strap_ble_connected(void);

/* Push a heart rate to the SIG service. No-op when nothing is connected. */
void strap_ble_notify_hr(uint8_t bpm);

#endif /* STRAP_BLE_H_ */
