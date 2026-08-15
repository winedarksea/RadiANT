/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HRM_BLE_H_
#define HRM_BLE_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * P8b: the SIG Heart Rate Service beside the ANT+ 0x78 master.
 *
 * Enables the controller and starts advertising; the beat work handler in
 * main.c notifies HRS at the same instants it advances the ANT+ accumulators,
 * so the two radios cannot disagree about the rate.
 *
 * Not Garmin-protocol-compatible and no running dynamics - this is the standard
 * SIG service only (not Garmin's proprietary Multi-Link/GFDI). See
 * docs/ble-running-dynamics-notes.md.
 */
int hrm_ble_start(void);

/*
 * True while a phone is connected. CONFIG_HRM_SUPPRESS_ANT_WHEN_CONNECTED reads
 * this to suppress the ANT+ duplicate broadcast. Exposed rather than acted on
 * internally, since stopping an ANT+ channel is the ANT+ side's business.
 */
bool hrm_ble_connected(void);

/* Push a heart rate to the SIG service. No-op when nothing is connected. */
void hrm_ble_notify_hr(uint8_t bpm);

#endif /* HRM_BLE_H_ */
