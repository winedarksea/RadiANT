/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * P8b: BLE beside RadiANT on one node.
 *
 * The SoftDevice Controller advertising the SIG Heart Rate Service while
 * radiant_core keeps an ANT+ 0x78 master channel running. One strap, two
 * radios' worth of demand on one RADIO peripheral, arbitrated by the MPSL gate
 * from P3.
 *
 * ⚠ THE INTERVALS ARE THE PRIORITY MECHANISM. THIS IS THE WHOLE PHASE.
 *
 * Finding 1 of the plan, from nrfxlib's own timeslot.rst: applications should
 * always request PRIORITY_NORMAL, and other MPSL users - the SoftDevice
 * Controller - have priority levels above anything an application can request.
 * The two application-visible levels rank us against OURSELVES. There is no
 * setting anywhere that makes ANT+ outrank BLE.
 *
 * So the stated product priority - RadiANT owns the radio - is delivered by
 * shaping the other stack's demand instead, and the in-tree precedent is
 * explicit about it: nrf/samples/esb/esb_ptx_ble/prj.conf runs BLE beside a
 * timeslot user with BT_PERIPHERAL_PREF_MIN_INT = MAX_INT = 800, which is a
 * one-second connection interval. A long advertising interval and a long
 * connection interval are not power tuning here. They are the arbitration
 * policy, expressed in the only place the hardware will honour it.
 *
 * The measured shape of this on the bench, from P3's coexistence work: a
 * TRACKED ANT+ window coexists with a 100 ms advertiser perfectly - every
 * window granted, nothing blocked. It is ACQUISITION, which is all sweep, that
 * a busy neighbour starves. A strap is a master and never sweeps, so it is on
 * the good side of that line by construction - which is exactly why the plan
 * puts the BLE branch on a node rather than on the dongle.
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
 * The name goes in the advertising payload, not only in the GAP Device Name
 * characteristic. A scanner that has not connected yet can only read what is
 * broadcast, so a name left out here shows up as a bare MAC address in every
 * scan list - which is how a simulated heart rate monitor gets mistaken for
 * an unrelated peripheral on a bench with several boards powered up.
 *
 * Budget check, against the 31-byte legacy advertising limit: flags 3 +
 * UUID16 list 4 + name (2 + strlen). CONFIG_BT_DEVICE_NAME is "RadiANT HR",
 * so 3 + 4 + 12 = 19. Renaming to something longer than 24 characters would
 * silently fail bt_le_adv_start() with -EINVAL, so keep it short.
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
	/*
	 * NOTHING IS DONE TO ANT+ FROM HERE. P9's suppression rule is policy
	 * above the link layer and lives with the ANT+ side; this module only
	 * reports the fact. A BLE callback that closed a radiant_core channel
	 * would be the layering violation the whole gate seam exists to avoid,
	 * and it would do it from the controller's own callback context.
	 */
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	atomic_set(&connected, 0);
	LOG_INF("BLE disconnected (0x%02x)", reason);
}

/*
 * REFUSE A SHORT CONNECTION INTERVAL RATHER THAN ACCEPT IT QUIETLY.
 *
 * A phone will ask for one - iOS in particular asks for tens of milliseconds -
 * and accepting it hands the controller a recurring high-priority event every
 * few milliseconds for as long as the connection lasts. Under MPSL that is not
 * something ANT+ can outrank; it is simply air that has gone. Rejecting the
 * update keeps the interval the peripheral advertised its preference for, and
 * says so in the log, because "the strap got worse when a phone connected" is
 * otherwise an unattributable field report.
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
