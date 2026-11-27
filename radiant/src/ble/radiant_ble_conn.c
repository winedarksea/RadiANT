/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_conn.c - the connection-parameter policy's Zephyr glue: the
 * le_param_req / le_param_updated pair that apps/hrm_ble and apps/treadmill
 * used to carry as textual twins. The rule is radiant_ble_conn_param_apply()
 * in radiant_ble_policy.c; <radiant/radiant_ble_conn.h> has the reasoning.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include <radiant/radiant_ble_conn.h>

LOG_MODULE_DECLARE(radiant_ble_adv, CONFIG_RADIANT_BLE_LOG_LEVEL);

#define POLICY_INTERVAL ((uint16_t)CONFIG_RADIANT_BLE_CONN_MIN_INTERVAL_UNITS)
#define POLICY_TIMEOUT  ((uint16_t)CONFIG_BT_PERIPHERAL_PREF_TIMEOUT)

/* The rewrite is valid only if the policy triple itself is: timeout * 4 >
 * interval at latency 0. Otherwise every rewrite would be neg-replied. */
BUILD_ASSERT((uint32_t)CONFIG_BT_PERIPHERAL_PREF_TIMEOUT * 4u >
	     (uint32_t)CONFIG_RADIANT_BLE_CONN_MIN_INTERVAL_UNITS,
	     "CONFIG_BT_PERIPHERAL_PREF_TIMEOUT is too short for "
	     "CONFIG_RADIANT_BLE_CONN_MIN_INTERVAL_UNITS: every rewritten "
	     "request would fail bt_le_conn_params_valid() and be silently "
	     "neg-replied by the controller.");

static bool on_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	struct radiant_ble_conn_param p = {
		.interval_min = param->interval_min,
		.interval_max = param->interval_max,
		.latency = param->latency,
		.timeout = param->timeout,
	};

	ARG_UNUSED(conn);

	if (!radiant_ble_conn_param_apply(&p, POLICY_INTERVAL, POLICY_TIMEOUT)) {
		return true;
	}

	LOG_INF("rewriting non-compliant request (interval %u-%u latency %u "
		"timeout %u) to policy %u/%u/0/%u - the long interval is the "
		"MPSL arbitration policy",
		param->interval_min, param->interval_max, param->latency,
		param->timeout, p.interval_min, p.interval_max, p.timeout);

	param->interval_min = p.interval_min;
	param->interval_max = p.interval_max;
	param->latency = p.latency;
	param->timeout = p.timeout;
	return true;
}

/* What actually landed: the rewrite above only edits what a peer proposes,
 * and the controller can still negotiate something else. */
static void on_param_updated(struct bt_conn *conn, uint16_t interval,
			     uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(conn);

	LOG_INF("connection params updated: interval=%u (%u ms) latency=%u "
		"timeout=%u (%u ms)",
		interval, (interval * 5u) / 4u, latency, timeout, timeout * 10u);
}

BT_CONN_CB_DEFINE(radiant_ble_conn_cb) = {
	.le_param_req = on_param_req,
	.le_param_updated = on_param_updated,
};
