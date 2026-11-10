/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ble_load.c - BENCH ONLY (ADR 0018 Phase 1). A connectable BLE advertiser
 * whose only job is to want the radio, so the MPSL gate's held grant can be
 * measured beside a second stack with events on the schedule. Started from a
 * SYS_INIT after a delay; touches nothing else. Replaces the P3/P4
 * ble_coex_load.c retired in fc04371. Never in a shipping image.
 */

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(ble_load, LOG_LEVEL_INF);

#define ADV_UNITS ((uint16_t)((CONFIG_ANT_DONGLE_BLE_LOAD_INTERVAL_MS * 1000) / 625))

static const struct bt_data adv_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
};

static void start_fn(struct k_work *w)
{
	static const struct bt_le_adv_param param =
		BT_LE_ADV_PARAM_INIT(BT_LE_ADV_OPT_CONN, ADV_UNITS, ADV_UNITS, NULL);
	int rc;

	ARG_UNUSED(w);
	rc = bt_enable(NULL);
	if (rc != 0 && rc != -EALREADY) {
		LOG_ERR("BLE load: bt_enable %d", rc);
		return;
	}
	rc = bt_le_adv_start(&param, adv_data, ARRAY_SIZE(adv_data), NULL, 0);
	LOG_INF("BLE load: advertising every %d ms (rc %d)",
		CONFIG_ANT_DONGLE_BLE_LOAD_INTERVAL_MS, rc);
}

static K_WORK_DELAYABLE_DEFINE(start_work, start_fn);

static int ble_load_init(void)
{
	(void)k_work_schedule(&start_work, K_SECONDS(3));
	return 0;
}

SYS_INIT(ble_load_init, APPLICATION, 99);
