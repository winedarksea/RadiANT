/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_hrs.h - the Bluetooth SIG Heart Rate Service (HRS 1.0), for a
 * heart-rate sensor that also broadcasts ANT+ 0x78 (Heart Rate Profile 1.0's
 * Sensor role).
 *
 * WHY NOT ZEPHYR'S BT_HRS. subsys/bluetooth/services/hrs.c always sets the
 * Measurement flags to 0x06 (contact supported AND detected, whatever the
 * electrodes say), always exposes a Heart Rate Control Point although it
 * supports no Energy Expended (HRS 1.0: the Control Point is mandatory
 * only when Energy Expended is supported, and excluded otherwise), carries no
 * RR-Intervals and takes one byte of rate. This service:
 *
 *   - separates contact SUPPORT (a property of the driver, constant for the
 *     life of a connection) from contact STATUS (reported live);
 *   - carries RR-Intervals in 1/1024 s, the oldest first; those the ATT MTU
 *     cannot hold go in the next notification (HRS 1.0 3.1.1.5);
 *   - has no Control Point and never sets the Energy Expended flag;
 *   - takes Body Sensor Location from CONFIG_RADIANT_BLE_HRS_BODY_SENSOR_LOCATION;
 *   - uses the UINT16 rate format only when the rate needs it (> 255).
 *
 * HRP 1.0 also requires the Device Information Service (Manufacturer Name
 * at least): CONFIG_RADIANT_BLE selects it (radiant_ble_dis.h). The appearance
 * is the application's (CONFIG_BT_DEVICE_APPEARANCE, 833 = Heart Rate Belt) -
 * good practice, not something HRP requires.
 *
 * The encoder is a pure function (radiant_ble_hrs_encode.c, no kernel, no
 * Bluetooth host) and is compiled unmodified by apps/hrm_ble/tests/publish on
 * native_sim. radiant_ble_hrs.c is the GATT glue.
 */
#ifndef RADIANT_BLE_HRS_H_
#define RADIANT_BLE_HRS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Heart Rate Measurement flags (HRS 1.0). */
#define RADIANT_BLE_HRS_FLAG_UINT16        0x01u /* bit 0: rate is UINT16 */
#define RADIANT_BLE_HRS_FLAG_CONTACT       0x02u /* bit 1: contact detected */
#define RADIANT_BLE_HRS_FLAG_CONTACT_SUPP  0x04u /* bit 2: contact supported */
#define RADIANT_BLE_HRS_FLAG_ENERGY        0x08u /* bit 3: never set here */
#define RADIANT_BLE_HRS_FLAG_RR            0x10u /* bit 4: RR-Intervals present */

/* Body Sensor Location values (HRS 1.0). */
#define RADIANT_BLE_HRS_BSL_OTHER     0u
#define RADIANT_BLE_HRS_BSL_CHEST     1u
#define RADIANT_BLE_HRS_BSL_WRIST     2u
#define RADIANT_BLE_HRS_BSL_FINGER    3u
#define RADIANT_BLE_HRS_BSL_HAND      4u
#define RADIANT_BLE_HRS_BSL_EAR_LOBE  5u
#define RADIANT_BLE_HRS_BSL_FOOT      6u

/* The ATT payload of a notification at the default 23-byte ATT MTU. */
#define RADIANT_BLE_HRS_DEFAULT_PAYLOAD 20u

/* Flags + UINT16 rate: the shortest buffer every measurement fits. */
#define RADIANT_BLE_HRS_MEAS_MIN 3u

/* One measurement, as the application's snapshot states it. */
struct radiant_ble_hrs_meas {
	/* Beats per minute; 0 when the sensor has no valid reading (stale,
	 * no contact). Values above 255 select the UINT16 format. */
	uint16_t bpm;
	/* The driver can detect skin contact (flag bit 2). A capability:
	 * constant for the life of a connection. */
	bool contact_supported;
	/* Contact detected now (flag bit 1). Ignored, and sent as 0, when
	 * contact_supported is false (HRS 1.0). */
	bool contact_detected;
	/* RR-Intervals since the last notification, OLDEST FIRST, in
	 * 1/1024 s. NULL or rr_count 0 for none. */
	const uint16_t *rr;
	size_t rr_count;
};

/*
 * Encode one Heart Rate Measurement into buf[0..cap). Pure.
 *
 * `cap` is the notification payload available (ATT MTU - 3). When not every
 * RR-Interval fits, the OLDEST that fit are written (HRS 1.0 3.1.1.5: the
 * rest go in the next notification). The RR flag is set only if at least one
 * interval was written. *rr_written (may be NULL) receives how many were -
 * rr[0..*rr_written) - which the caller removes from its pending list once
 * the notification has gone.
 *
 * Returns the encoded length, -EINVAL for a NULL argument or rr == NULL with
 * rr_count > 0, or -ENOMEM when cap cannot hold the flags and the rate.
 */
int radiant_ble_hrs_encode(const struct radiant_ble_hrs_meas *m, uint8_t *buf,
			   size_t cap, size_t *rr_written);

/* ── GATT glue (radiant_ble_hrs.c, CONFIG_RADIANT_BLE_HRS) ──────────────── */

/*
 * True while at least one connected peer has notifications enabled on the
 * Heart Rate Measurement characteristic.
 */
bool radiant_ble_hrs_subscribed(void);

/*
 * Notify `m` to every subscribed connection, each encoded for its own ATT
 * MTU. Returns the number of connections bt_gatt_notify() accepted - a
 * connection whose encode failed is skipped - or, when none accepted, 0 (none
 * subscribed, or every send failed) or the encoder's negative errno. *rr_sent (may be NULL) receives how many of m->rr, from the oldest,
 * went out: the most any accepted notification carried, 0 when none was
 * accepted. The caller removes exactly those and keeps the rest pending; with
 * several collectors at different MTUs, one with a smaller MTU misses the
 * difference (a missing interval is legal; a repeated one would not be).
 * Thread context only.
 */
int radiant_ble_hrs_notify(const struct radiant_ble_hrs_meas *m, size_t *rr_sent);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_BLE_HRS_H_ */
