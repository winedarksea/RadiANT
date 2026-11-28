/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_ble.c - FTMS Indoor Bike (0x1826 / 0x2AD2) beside the ANT+ FE-C
 * master, through the shared core radiant/src/ble/radiant_ble_ftms.c.
 *
 * This file is the trainer plugged into that core: the capabilities and
 * ranges (trainer_cmd.c), the Indoor Bike Data record and its Reset baseline
 * (trainer_equip.c), the actuators (trainer_cmd.c's FTMS hooks), the session
 * and epoch (main.c), DIS and advertising through radiant_ble_adv with the
 * FTMS Service Data element (machine type Indoor Bike).
 *
 * The advertising payload is flags 3 + a one-UUID16 list 4 + the FTMS Service
 * Data element 7 = 14 bytes. The name goes in the scan response, as in
 * apps/treadmill, so it may be 29 characters.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <radiant/radiant_ble_adv.h>
#include <radiant/radiant_ble_dis.h>
#include <radiant/radiant_ble_ftms.h>

#include "trainer_ble.h"
#include "trainer_ble_svc.h"
#include "trainer_cmd.h"
#include "trainer_equip.h"
#include "trainer_node.h"

LOG_MODULE_DECLARE(smart_trainer, CONFIG_SMART_TRAINER_LOG_LEVEL);

#define ADV_UNITS(ms) ((uint32_t)(((uint32_t)(ms) * 1000u) / 625u))

BUILD_ASSERT(TRAINER_IBD_LEN <= 20u,
	     "the Indoor Bike Data record must fit a default 23-byte ATT_MTU");

static atomic_t connections;
static uint32_t last_notify_ms;

static enum radiant_ble_ftms_run ftms_run(enum trainer_run run)
{
	switch (run) {
	case TRAINER_RUN_RUNNING:
		return RADIANT_BLE_FTMS_RUN_RUNNING;
	case TRAINER_RUN_PAUSED:
		return RADIANT_BLE_FTMS_RUN_PAUSED;
	case TRAINER_RUN_STOPPED:
		return RADIANT_BLE_FTMS_RUN_STOPPED;
	default:
		return RADIANT_BLE_FTMS_RUN_IDLE;
	}
}

static void machine_session(struct radiant_ble_ftms_session *out, void *ctx)
{
	const struct trainer_equip *e = trainer_node_equip();

	ARG_UNUSED(ctx);
	out->run = ftms_run(e->run);
	out->epoch = e->epoch;
}

static uint32_t machine_epoch(void *ctx)
{
	ARG_UNUSED(ctx);
	return trainer_node_epoch(); /* atomic: the BT RX thread calls this */
}

static int machine_encode(uint8_t *buf, size_t cap, void *ctx)
{
	ARG_UNUSED(ctx);
	return trainer_equip_ibd_encode(trainer_node_equip(), buf, cap);
}

static void machine_reset_done(void *ctx)
{
	ARG_UNUSED(ctx);
	/* SS4.16.2: the FTMS view's elapsed time and distance to zero (ANT+
	 * untouched), the session to Idle, the epoch moved - QUIETLY: the core
	 * sends Reset (0x01) and Idle itself (trainer_logic.h). */
	trainer_node_ftms_reset_done();
}

static struct radiant_ble_ftms_machine ftms_machine = {
	.machine_type = RADIANT_BLE_FTMS_TYPE_INDOOR_BIKE,
	.ops = {
		.set_target = trainer_cmd_ftms_set_target,
		.workout = trainer_cmd_ftms_workout,
		.reset = trainer_cmd_ftms_reset,
	},
	/* A trainer is passive: no motion rule. A NULL check makes a stale
	 * request STALE (0x04) and permits everything else. */
	.auth = { .check = NULL },
	.encode_data = machine_encode,
	.session = machine_session,
	.epoch = machine_epoch,
	.reset_done = machine_reset_done,
};

void trainer_ble_session_changed(enum trainer_run prev, enum trainer_run next)
{
	radiant_ble_ftms_session_changed(ftms_run(prev), ftms_run(next));
}

void trainer_ble_notify_status(const uint8_t *msg, size_t len)
{
	radiant_ble_ftms_notify_status(msg, len);
}

void trainer_ble_tick(void)
{
	uint32_t now;

	radiant_ble_adv_watchdog();
	if (atomic_get(&connections) == 0) {
		return;
	}
	/* FTMS: "typically once per second". Every notification is radio time
	 * against the ANT+ master through the MPSL gate. */
	now = (uint32_t)k_uptime_get_32();
	if ((now - last_notify_ms) < CONFIG_SMART_TRAINER_BLE_NOTIFY_INTERVAL_MS) {
		return;
	}
	last_notify_ms = now;
	radiant_ble_ftms_notify_data();
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);
	if (err != 0u) {
		LOG_WRN("BLE connect failed (0x%02x)", err);
		return;
	}
	atomic_inc(&connections);
	LOG_INF("BLE connected");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	atomic_dec(&connections);
	LOG_INF("BLE disconnected (0x%02x)", reason);
	/* radiant_ble_adv restarts advertising from `recycled`. */
}

BT_CONN_CB_DEFINE(trainer_ble_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

/* Table 3.1's Service Data, initialised from trainer_ble_svc.h (which the
 * glue suite checks). trainer_ble_start() compares it with the core's bytes
 * before advertising and, should they ever differ, advertises the core's. */
static uint8_t ftms_svc_data[RADIANT_BLE_FTMS_SVC_DATA_LEN] = TRAINER_FTMS_SVC_DATA_INIT;

static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL, BT_UUID_16_ENCODE(BT_UUID_FMS_VAL)),
	/* FTMP's Service Data: Available + machine type Indoor Bike. It is how
	 * FTMS clients filter. */
	BT_DATA(BT_DATA_SVC_DATA16, ftms_svc_data, sizeof(ftms_svc_data)),
};

#define NAME_SUFFIX_BUDGET \
	(IS_ENABLED(CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX) ? RADIANT_BLE_NAME_SUFFIX_MAX : 0u)

BUILD_ASSERT(sizeof(CONFIG_BT_DEVICE_NAME) - 1u + NAME_SUFFIX_BUDGET <= 29u,
	     "CONFIG_BT_DEVICE_NAME (plus the devnum suffix when on) does not fit "
	     "the 31-byte scan response: 29 characters at most");
BUILD_ASSERT((3u + 4u + 7u) <= 31u, "the advertising payload no longer fits");

#if defined(CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX)
BUILD_ASSERT(sizeof(CONFIG_BT_DEVICE_NAME) - 1u + RADIANT_BLE_NAME_SUFFIX_MAX <=
	     CONFIG_BT_DEVICE_NAME_MAX,
	     "CONFIG_BT_DEVICE_NAME plus the devnum suffix exceeds CONFIG_BT_DEVICE_NAME_MAX");
#endif

static char adv_name[sizeof(CONFIG_BT_DEVICE_NAME) + RADIANT_BLE_NAME_SUFFIX_MAX] =
	CONFIG_BT_DEVICE_NAME;

static struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, adv_name, sizeof(CONFIG_BT_DEVICE_NAME) - 1u),
};

static struct bt_le_adv_param adv_param;

int trainer_ble_start(uint16_t devnum, uint32_t serial,
		      const struct radiant_ble_ftms_caps *caps)
{
	int err;

	ftms_machine.caps = *caps;
	err = radiant_ble_ftms_register(&ftms_machine);
	if (err != 0) {
		LOG_ERR("radiant_ble_ftms_register: %d", err);
		return err;
	}
	{
		uint8_t want[RADIANT_BLE_FTMS_SVC_DATA_LEN];

		if (radiant_ble_ftms_service_data(want, sizeof(want), true,
						  RADIANT_BLE_FTMS_TYPE_INDOOR_BIKE) == sizeof(want) &&
		    memcmp(want, ftms_svc_data, sizeof(want)) != 0) {
			LOG_ERR("FTMS Service Data constant disagrees with the core; "
				"advertising the core's bytes");
			memcpy(ftms_svc_data, want, sizeof(want));
		}
	}

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("bt_enable: %d", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		/* Bonds, and (with BT_SETTINGS) the identity address that
		 * bt_enable() defers to the settings commit. Not fatal; see
		 * apps/treadmill/src/treadmill_ble.c for the full reasoning. */
		err = settings_load();
		if (err != 0) {
			LOG_ERR("settings_load: %d - bonds will not persist", err);
		}
	}

	/* DIS from radiant_product and the unit's serial, after settings_load()
	 * (which would otherwise replace them with stored values). */
	(void)radiant_ble_dis_apply(serial);

#if defined(CONFIG_RADIANT_BLE_NAME_DEVNUM_SUFFIX)
	{
		int n = radiant_ble_name_with_devnum(adv_name, sizeof(adv_name),
						     CONFIG_BT_DEVICE_NAME, devnum);

		if (n > 0) {
			sd[0].data_len = (uint8_t)n;
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
	adv_param.interval_min = ADV_UNITS(CONFIG_SMART_TRAINER_BLE_ADV_INTERVAL_MS);
	adv_param.interval_max = ADV_UNITS(CONFIG_SMART_TRAINER_BLE_ADV_INTERVAL_MS) + 16u;

	err = radiant_ble_adv_init(&adv_param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err != 0) {
		LOG_ERR("radiant_ble_adv_init: %d", err);
		return err;
	}
	err = radiant_ble_adv_ensure();
	if (err != 0) {
		LOG_WRN("BLE advertising not started yet (%d); retrying in the background",
			err);
		return 0;
	}

	LOG_INF("BLE: FTMS Indoor Bike (targets 0x%08x) advertising every %u ms as '%s'",
		(unsigned)caps->targets, (unsigned)CONFIG_SMART_TRAINER_BLE_ADV_INTERVAL_MS,
		adv_name);
	return 0;
}
