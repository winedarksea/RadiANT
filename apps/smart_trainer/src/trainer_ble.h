/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_ble.h - FTMS (Indoor Bike) over BLE beside the ANT+ FE-C master.
 * The service itself is radiant_ble_ftms (shared with apps/treadmill); this is
 * the trainer plugged into it. All calls on the system workqueue.
 */
#ifndef TRAINER_BLE_H_
#define TRAINER_BLE_H_

#include <stddef.h>
#include <stdint.h>

#include <radiant/radiant_ble_ftms.h>

#include "trainer_equip.h"

/* Register the machine, bring up the controller, DIS and advertising. 0, or
 * the errno of a fatal step (the node is then ANT+ only). */
int trainer_ble_start(uint16_t devnum, uint32_t serial,
		      const struct radiant_ble_ftms_caps *caps);

/* Called every node tick: paces the Indoor Bike Data notification and runs
 * the advertising watchdog. */
void trainer_ble_tick(void);

/* The session moved: Training Status and the machine status. */
void trainer_ble_session_changed(enum trainer_run prev, enum trainer_run next);

/* A target changed through FE-C: tell FTMS subscribers (0x07/0x08/0x12/0x13). */
void trainer_ble_notify_status(const uint8_t *msg, size_t len);

#endif /* TRAINER_BLE_H_ */
