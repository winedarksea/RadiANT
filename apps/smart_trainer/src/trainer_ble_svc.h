/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_ble_svc.h - the FTMS advertising Service Data the trainer puts in
 * every advertisement (FTMP Table 3.1): UUID 0x1826, Flags = Fitness Machine
 * Available, Fitness Machine Type = Indoor Bike (bit 5).
 *
 * In a header, not inline in trainer_ble.c, so tests/glue checks THIS
 * initialiser against radiant_ble_ftms_service_data() rather than a copy of
 * it. trainer_ble.c also checks it at boot and advertises the core's bytes if
 * the two ever disagree.
 */
#ifndef TRAINER_BLE_SVC_H_
#define TRAINER_BLE_SVC_H_

#include <stdint.h>

#include <radiant/radiant_ble_ftms.h>

#define TRAINER_FTMS_SVC_DATA_INIT                                          \
	{                                                                   \
		0x26u, 0x18u, RADIANT_BLE_FTMS_SD_FLAG_AVAILABLE,           \
		(uint8_t)(RADIANT_BLE_FTMS_TYPE_INDOOR_BIKE & 0xFFu),       \
		(uint8_t)(RADIANT_BLE_FTMS_TYPE_INDOOR_BIKE >> 8),          \
	}

#endif /* TRAINER_BLE_SVC_H_ */
