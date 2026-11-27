/* SPDX-License-Identifier: Apache-2.0 */
#ifndef HRM_BLE_H_
#define HRM_BLE_H_

#include <stdbool.h>
#include <stdint.h>

#include <radiant/radiant_ble_hrs.h>

/*
 * P8b: the SIG Heart Rate Service beside the ANT+ 0x78 master.
 *
 * Enables the controller, sets the Device Information Service from
 * radiant_product and the unit's 32-bit `serial`, and starts advertising
 * through radiant_ble_adv (which keeps it running across connections). With
 * CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX the name is "<name> <devnum>".
 * The service itself is radiant_ble_hrs (CONFIG_RADIANT_BLE_HRS): contact
 * support kept apart from status, RR-Intervals, no Control Point. main.c
 * notifies it at 1 Hz from the same snapshot that feeds ANT+ byte 7
 * (src/hrm_publish.h), so the two radios cannot disagree about the rate.
 *
 * Not Garmin-protocol-compatible and no running dynamics - this is the standard
 * SIG service only (not Garmin's proprietary Multi-Link/GFDI). See
 * docs/ble-running-dynamics-notes.md.
 */
int hrm_ble_start(uint16_t devnum, uint32_t serial);

/* From the node tick: the advertising watchdog. */
void hrm_ble_tick(void);

/*
 * True while a phone is connected. CONFIG_HRM_SUPPRESS_ANT_WHEN_CONNECTED reads
 * this to suppress the ANT+ duplicate broadcast. Exposed rather than acted on
 * internally, since stopping an ANT+ channel is the ANT+ side's business.
 */
bool hrm_ble_connected(void);

/* True while a connected collector has Heart Rate Measurement notifications
 * enabled. */
bool hrm_ble_subscribed(void);

/* Notify one measurement to every subscribed collector. Returns the number
 * notified, or a negative errno. Thread context (the node tick). */
int hrm_ble_notify(const struct radiant_ble_hrs_meas *m);

/* True once after any link dropped since the last call: the node tick clears
 * pending RR-Intervals on it. */
bool hrm_ble_take_disconnect(void);

#endif /* HRM_BLE_H_ */
