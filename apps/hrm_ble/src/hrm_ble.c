/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * P8b: BLE beside RadiANT on one node. SoftDevice Controller advertises the
 * SIG Heart Rate Service while radiant keeps an ANT+ 0x78 master
 * running - one RADIO peripheral, arbitrated by the MPSL gate (P3).
 *
 * The intervals ARE the priority mechanism. Per nrfxlib's timeslot.rst,
 * applications only get PRIORITY_NORMAL and other MPSL users (the SDC) rank
 * above anything an app can request - there is no setting that makes ANT+
 * outrank BLE. So RadiANT's priority is delivered by shaping BLE's demand
 * instead: long advertising/connection intervals (cf.
 * nrf/samples/esb/esb_ptx_ble/prj.conf, MIN_INT = MAX_INT = 800, a 1 s
 * interval) are the arbitration policy, not power tuning.
 *
 * Bench data (P3): a tracked ANT+ window coexists fine with a 100 ms
 * advertiser - it's acquisition/sweep that a busy neighbour starves. A
 * strap is a master and never sweeps, hence the BLE branch lives on a node
 * rather than the dongle.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/services/hrs.h>

#include <radiant/radiant_ble_adv.h>
#include <radiant/radiant_ble_dis.h>

#include "hrm_ble.h"

LOG_MODULE_DECLARE(hrm, CONFIG_HRM_LOG_LEVEL);

static atomic_t connected;

/* 0.625 ms units, which is what the advertising interval is expressed in. */
#define ADV_UNITS(ms) ((uint32_t)(((uint32_t)(ms) * 1000u) / 625u))

/* What CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX can add: " 65535". */
#define NAME_SUFFIX_BUDGET \
	(IS_ENABLED(CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX) ? RADIANT_BLE_NAME_SUFFIX_MAX : 0u)

/*
 * The name goes in the advertising payload, not just the GAP Device Name
 * characteristic, so an unconnected scanner sees a name rather than a bare MAC.
 *
 * THE BUDGET IS 22 CHARACTERS, and it is worth the arithmetic because the file
 * this replaced said "~24" from memory and was wrong. Against the 31-byte
 * legacy advertising payload, each AD structure costs a length byte and a type
 * byte on top of its data:
 *
 *   flags        1 + 1 + 1 =  3
 *   UUID16 list  1 + 1 + 2 =  4
 *   name         1 + 1 + N =  2 + N
 *                            ------
 *                            9 + N  <=  31   ->   N <= 22
 *
 * "RadiANT HR" is 10, so the default has 12 characters of headroom - and with
 * CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX the " <devnum>" suffix takes up to 6
 * of them. A product name that overruns is not a warning: bt_le_adv_start()
 * returns -EINVAL on every attempt, and the node transmits ANT+ perfectly and
 * never advertises, with the only evidence a log line on a board that has no
 * console attached. Hence a BUILD_ASSERT rather than a runtime check.
 */
BUILD_ASSERT(sizeof(CONFIG_BT_DEVICE_NAME) - 1u + NAME_SUFFIX_BUDGET <= 22u,
	     "CONFIG_BT_DEVICE_NAME (plus the 6-character devnum suffix when "
	     "CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX is on) does not fit the "
	     "31-byte legacy advertising payload beside the flags and the HRS "
	     "UUID: the budget is 22 characters (3 + 4 + 2 + N <= 31). Over it, "
	     "bt_le_adv_start() returns -EINVAL and the node silently never "
	     "advertises.");

/* The advertised name: CONFIG_BT_DEVICE_NAME, or "<name> <devnum>" once
 * hrm_ble_start() has written it. */
static char adv_name[sizeof(CONFIG_BT_DEVICE_NAME) + RADIANT_BLE_NAME_SUFFIX_MAX] =
	CONFIG_BT_DEVICE_NAME;

#if defined(CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX)
/* bt_set_name() refuses a name longer than its store with -ENOMEM. */
BUILD_ASSERT(sizeof(CONFIG_BT_DEVICE_NAME) - 1u + RADIANT_BLE_NAME_SUFFIX_MAX <=
	     CONFIG_BT_DEVICE_NAME_MAX,
	     "CONFIG_BT_DEVICE_NAME plus the devnum suffix exceeds "
	     "CONFIG_BT_DEVICE_NAME_MAX");
#endif

#define AD_NAME 2

static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HRS_VAL)),
	[AD_NAME] = BT_DATA(BT_DATA_NAME_COMPLETE, adv_name,
			    sizeof(CONFIG_BT_DEVICE_NAME) - 1u),
};

/*
 * ONE definition of what "advertising" means for this node, registered with
 * radiant_ble_adv at start-up and used by it for every restart. The interval
 * is not cosmetic - it IS this application's share of the MPSL arbitration
 * against the ANT+ master, per the header comment above - so a restart must
 * never drift to a different one.
 *
 * Restarting is the shared layer's job now (radiant/src/ble/radiant_ble_adv.c):
 * Zephyr stops a connectable advertiser on connection and never resumes it,
 * and the restart this file used to make once, from `disconnected`, with no
 * retry, was found on real hardware (2026-08-16) to leave a strap invisible
 * until power-cycled. The layer restarts from `recycled`, retries with a
 * backoff until it succeeds, and hrm_ble_tick() runs its watchdog.
 *
 * The connection-parameter policy that used to be here (le_param_req rewrite,
 * le_param_updated log) is radiant/src/ble/radiant_ble_conn.c, configured from
 * HRM_BLE_MIN_CONN_INTERVAL_UNITS via RADIANT_BLE_CONN_MIN_INTERVAL_UNITS.
 */
static struct bt_le_adv_param adv_param;

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);

	if (err != 0u) {
		LOG_WRN("BLE connect failed (0x%02x)", err);
		return;
	}
	atomic_set(&connected, 1);
	LOG_INF("BLE connected");
	/* Nothing is done to ANT+ from here - the suppression rule lives
	 * with the ANT+ side; a BLE callback reaching into radiant
	 * (from controller context, no less) would be the layering violation
	 * the gate seam exists to avoid. */
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	atomic_set(&connected, 0);
	LOG_INF("BLE disconnected (0x%02x)", reason);
	/* No restart here: radiant_ble_adv restarts from `recycled`. */
}

BT_CONN_CB_DEFINE(hrm_ble_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

bool hrm_ble_connected(void)
{
	return atomic_get(&connected) != 0;
}

void hrm_ble_notify_hr(uint8_t bpm)
{
	if (atomic_get(&connected) == 0) {
		return;
	}
	(void)bt_hrs_notify(bpm);
}

void hrm_ble_tick(void)
{
	radiant_ble_adv_watchdog();
}

int hrm_ble_start(uint16_t devnum, uint32_t serial)
{
	int err;

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("bt_enable: %d", err);
		return err;
	}

	/* DIS from radiant_product and the unit's serial. A failure keeps the
	 * Kconfig strings and is not a reason to stay invisible. */
	(void)radiant_ble_dis_apply(serial);

#if defined(CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX)
	{
		int n = radiant_ble_name_with_devnum(adv_name, sizeof(adv_name),
						     CONFIG_BT_DEVICE_NAME, devnum);

		if (n > 0) {
			ad[AD_NAME].data_len = (uint8_t)n;
			err = bt_set_name(adv_name);
			if (err != 0) {
				LOG_WRN("bt_set_name(%s): %d", adv_name, err);
			}
		}
	}
#else
	ARG_UNUSED(devnum);
#endif

	adv_param = *BT_LE_ADV_CONN_FAST_1;
	adv_param.interval_min = ADV_UNITS(CONFIG_HRM_BLE_ADV_INTERVAL_MS);
	adv_param.interval_max = ADV_UNITS(CONFIG_HRM_BLE_ADV_INTERVAL_MS) + 16u;

	err = radiant_ble_adv_init(&adv_param, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err != 0) {
		LOG_ERR("radiant_ble_adv_init: %d", err);
		return err;
	}

	/* A failure here is not BLE being dead: the layer has already
	 * scheduled a retry and keeps retrying until advertising starts. So
	 * it is a warning and this returns 0 - only a fatal step above (the
	 * controller not coming up) may make the caller report "ANT+ only". */
	err = radiant_ble_adv_ensure();
	if (err != 0) {
		LOG_WRN("BLE advertising not started yet (%d); retrying in the "
			"background", err);
		return 0;
	}

	LOG_INF("BLE: SIG HRS advertising every %u ms as '%s'",
		(unsigned)CONFIG_HRM_BLE_ADV_INTERVAL_MS, adv_name);
	return 0;
}
