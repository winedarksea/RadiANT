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

#include "strap_ble.h"

LOG_MODULE_DECLARE(strap, CONFIG_STRAP_LOG_LEVEL);

static atomic_t connected;

/* 0.625 ms units, which is what the advertising interval is expressed in. */
#define ADV_UNITS(ms) ((uint32_t)(((uint32_t)(ms) * 1000u) / 625u))

/*
 * The name goes in the advertising payload, not just the GAP Device Name
 * characteristic, so an unconnected scanner sees a name rather than a bare
 * MAC. Budget vs the 31-byte legacy adv limit: flags 3 + UUID16 4 + name
 * (2 + strlen) = 19 for "RadiANT HR"; longer than ~24 chars silently fails
 * bt_le_adv_start() with -EINVAL.
 */
static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HRS_VAL)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1u),
};

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0u) {
		LOG_WRN("BLE connect failed (0x%02x)", err);
		return;
	}
	atomic_set(&connected, 1);
	LOG_INF("BLE connected");
	/* Nothing is done to ANT+ from here - P9's suppression rule lives
	 * with the ANT+ side; a BLE callback reaching into radiant
	 * (from controller context, no less) would be the layering violation
	 * the gate seam exists to avoid. */
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	atomic_set(&connected, 0);
	LOG_INF("BLE disconnected (0x%02x)", reason);
}

/*
 * Refuse a short connection interval rather than accept it quietly: a phone
 * (iOS especially) will ask for tens of ms, handing the controller a
 * recurring high-priority event MPSL can't make ANT+ outrank. Reject and
 * hold at the advertised preference, logging it so a degraded strap is
 * traceable to a phone connection.
 */
static bool on_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	if (param->interval_max < CONFIG_STRAP_BLE_MIN_CONN_INTERVAL_UNITS) {
		LOG_INF("refusing %u-unit connection interval, holding at %u "
			"- see finding 1: this is the arbitration policy",
			param->interval_max,
			(unsigned)CONFIG_STRAP_BLE_MIN_CONN_INTERVAL_UNITS);
		param->interval_min = CONFIG_STRAP_BLE_MIN_CONN_INTERVAL_UNITS;
		param->interval_max = CONFIG_STRAP_BLE_MIN_CONN_INTERVAL_UNITS;
	}
	return true;
}

BT_CONN_CB_DEFINE(strap_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.le_param_req = on_param_req,
};

bool strap_ble_connected(void)
{
	return atomic_get(&connected) != 0;
}

void strap_ble_notify_hr(uint8_t bpm)
{
	if (atomic_get(&connected) == 0) {
		return;
	}
	(void)bt_hrs_notify(bpm);
}

int strap_ble_start(void)
{
	struct bt_le_adv_param param = *BT_LE_ADV_CONN_FAST_1;
	int err;

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("bt_enable: %d", err);
		return err;
	}

	param.interval_min = ADV_UNITS(CONFIG_STRAP_BLE_ADV_INTERVAL_MS);
	param.interval_max = ADV_UNITS(CONFIG_STRAP_BLE_ADV_INTERVAL_MS) + 16u;

	err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);
	if (err != 0) {
		LOG_ERR("bt_le_adv_start: %d", err);
		return err;
	}

	LOG_INF("BLE: SIG HRS advertising every %u ms",
		(unsigned)CONFIG_STRAP_BLE_ADV_INTERVAL_MS);
	return 0;
}
