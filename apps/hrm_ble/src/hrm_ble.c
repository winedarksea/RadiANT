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

#include "hrm_ble.h"

LOG_MODULE_DECLARE(hrm, CONFIG_HRM_LOG_LEVEL);

static atomic_t connected;

/* 0.625 ms units, which is what the advertising interval is expressed in. */
#define ADV_UNITS(ms) ((uint32_t)(((uint32_t)(ms) * 1000u) / 625u))

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
 * "RadiANT HR" is 10, so the default has 12 characters of headroom. A product
 * name that overruns is not a warning: bt_le_adv_start() returns -EINVAL, and
 * main.c's call site is `(void)hrm_ble_start()`, so an over-long name ships a
 * node that transmits ANT+ perfectly and never advertises, with the only
 * evidence a log line on a board that has no console attached. Hence a
 * BUILD_ASSERT rather than a runtime check.
 */
BUILD_ASSERT(sizeof(CONFIG_BT_DEVICE_NAME) - 1u <= 22u,
	     "CONFIG_BT_DEVICE_NAME does not fit the 31-byte legacy advertising "
	     "payload beside the flags and the HRS UUID: the budget is 22 "
	     "characters (3 + 4 + 2 + N <= 31). Over it, bt_le_adv_start() "
	     "returns -EINVAL and the node silently never advertises.");

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_HRS_VAL)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1u),
};

/*
 * ONE definition of what "advertising" means for this node, used both at
 * start-up and when restarting after a disconnect. Kept as a single function
 * on purpose: two copies of the parameter setup would let the post-disconnect
 * advertiser drift to a different interval from the boot-time one, and the
 * interval here is not cosmetic - it IS this application's share of the MPSL
 * arbitration against the ANT+ master, per the header comment above.
 */
static int adv_start(void)
{
	struct bt_le_adv_param param = *BT_LE_ADV_CONN_FAST_1;

	param.interval_min = ADV_UNITS(CONFIG_HRM_BLE_ADV_INTERVAL_MS);
	param.interval_max = ADV_UNITS(CONFIG_HRM_BLE_ADV_INTERVAL_MS) + 16u;

	return bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), NULL, 0);
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
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

/*
 * ADVERTISING DOES NOT COME BACK BY ITSELF, and this is the whole reason this
 * work item exists. Zephyr stops a connectable advertiser when a connection is
 * established and does NOT resume it on disconnect: the old auto-resume
 * behaviour behind BT_LE_ADV_OPT_CONNECTABLE is gone, so restarting is the
 * application's job.
 *
 * Observed on real hardware, 2026-08-16, and it is exactly as bad as it
 * sounds: a phone found "RadiANT HR", connected, read a heart rate, and after
 * disconnecting the node was invisible to every scanner until it was power
 * cycled. A heart-rate strap that can be paired once per boot is not a strap.
 * There is no log evidence either, because the node goes on transmitting ANT+
 * perfectly - which is precisely the silent-failure shape the BUILD_ASSERT on
 * the name length above was written to prevent, arrived at from a different
 * direction.
 *
 * Restarted from a work item rather than inline: this callback runs in the
 * Bluetooth RX thread's context, and bt_le_adv_start() there re-enters the
 * host from its own callback. The system work queue is the ordinary Zephyr
 * idiom for handing work out of a BT callback, and it costs nothing here.
 */
static void adv_restart_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	int err = adv_start();

	if (err == -EALREADY) {
		/* Something already restarted it; not an error. */
		return;
	}
	if (err != 0) {
		LOG_ERR("bt_le_adv_start after disconnect: %d", err);
		return;
	}
	LOG_INF("BLE: advertising again after disconnect");
}

static K_WORK_DEFINE(adv_restart_work, adv_restart_fn);

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	atomic_set(&connected, 0);
	LOG_INF("BLE disconnected (0x%02x)", reason);
	k_work_submit(&adv_restart_work);
}

/*
 * Rewrite a non-compliant connection-interval request to policy rather than
 * reject it: this function has its textual twin in
 * apps/treadmill/src/treadmill_ble.c's on_param_req() - keep the two
 * parallel and edit both together.
 *
 * WHY REWRITE AND NOT REJECT. A reject (returning false) leaves the link at
 * whatever short interval it connected at - Zephyr does not fall back to the
 * PPCP preference on a rejected request, it just leaves the current
 * parameters alone. A phone that asks for 30-50 ms and gets rejected stays
 * at 30-50 ms indefinitely, with a controller event every few ms, which is
 * the exact air-time loss this policy exists to prevent. Rewriting the
 * request in place and returning true is the only branch that actually
 * moves the link to policy.
 *
 * THE BUG THIS REPLACES. The previous version rewrote interval_min/max only
 * and left latency and timeout as the peer asked - a peer that requested a
 * short interval paired with a short timeout (tuned for that short
 * interval) produced, after the interval was widened to 800 units (1 s), a
 * combination that fails Zephyr's own validity check
 * (bt_le_conn_params_valid(), hci_core.c: timeout*4 > (1+latency)*
 * interval_max, both sides raw units). An invalid rewrite is silently
 * neg-replied by the controller (BT_HCI_ERR_INVALID_LL_PARAM) and the link
 * stays at the peer's original fast interval while this function's own log
 * line claims it was held to policy. Forcing latency to 0 and raising (never
 * lowering) timeout to at least CONFIG_BT_PERIPHERAL_PREF_TIMEOUT keeps the
 * rewritten triple inside the bound for every latency<=0 case reachable
 * here, since CONFIG_BT_PERIPHERAL_PREF_TIMEOUT was itself chosen to clear
 * that bound at interval_max=800, latency=0 (see apps/hrm_ble/Kconfig's
 * block on BT_PERIPHERAL_PREF_TIMEOUT).
 */
static bool on_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	ARG_UNUSED(conn);

	/*
	 * The accept-untouched branch must ALSO clear the same validity bound
	 * the rewrite is careful about, or it reintroduces the bug one step
	 * further out: a peer asking for an interval_max of 1600 units (2 s)
	 * with latency 0 and timeout 400 satisfies every policy test above and
	 * still fails bt_le_conn_params_valid() (4*400 = 1600, not > 1600), so
	 * accepting it verbatim produces the same silent neg-reply while the
	 * log claims the ask was honoured. Checking the bound here means every
	 * `return true` out of this function is a triple the controller can
	 * actually apply.
	 */
	bool valid = (uint32_t)param->timeout * 4u >
		     ((uint32_t)param->latency + 1u) * (uint32_t)param->interval_max;

	bool compliant = param->interval_min >= CONFIG_HRM_BLE_MIN_CONN_INTERVAL_UNITS &&
			  param->interval_max >= CONFIG_HRM_BLE_MIN_CONN_INTERVAL_UNITS &&
			  param->latency == 0 &&
			  param->timeout >= CONFIG_BT_PERIPHERAL_PREF_TIMEOUT;

	if (compliant && valid) {
		return true;
	}

	LOG_INF("rewriting non-compliant request (interval %u-%u latency %u "
		"timeout %u) to policy %u/%u/0/%u - see finding 1: this is "
		"the arbitration policy",
		param->interval_min, param->interval_max, param->latency,
		param->timeout,
		(unsigned)CONFIG_HRM_BLE_MIN_CONN_INTERVAL_UNITS,
		(unsigned)CONFIG_HRM_BLE_MIN_CONN_INTERVAL_UNITS,
		(unsigned)CONFIG_BT_PERIPHERAL_PREF_TIMEOUT);

	param->interval_min = CONFIG_HRM_BLE_MIN_CONN_INTERVAL_UNITS;
	param->interval_max = CONFIG_HRM_BLE_MIN_CONN_INTERVAL_UNITS;
	param->latency = 0;
	if (param->timeout < CONFIG_BT_PERIPHERAL_PREF_TIMEOUT) {
		param->timeout = CONFIG_BT_PERIPHERAL_PREF_TIMEOUT;
	}

	return true;
}

/*
 * The observability that would have caught the previous silent failure: the
 * rewrite above only edits what a peer PROPOSES, and the controller can
 * still negotiate something else. This logs what parameters actually landed,
 * so a degraded link (short interval, short timeout) is visible in the log
 * rather than inferred from an absence of complaints.
 */
static void on_param_updated(struct bt_conn *conn, uint16_t interval,
			      uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);

	LOG_INF("connection params updated: interval=%u (%u ms) latency=%u "
		"timeout=%u (%u ms)",
		interval, (interval * 5u) / 4u, latency, timeout, timeout * 10u);
}

BT_CONN_CB_DEFINE(hrm_ble_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.le_param_req = on_param_req,
	.le_param_updated = on_param_updated,
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

int hrm_ble_start(void)
{
	int err;

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("bt_enable: %d", err);
		return err;
	}

	err = adv_start();
	if (err != 0) {
		LOG_ERR("bt_le_adv_start: %d", err);
		return err;
	}

	LOG_INF("BLE: SIG HRS advertising every %u ms",
		(unsigned)CONFIG_HRM_BLE_ADV_INTERVAL_MS);
	return 0;
}
