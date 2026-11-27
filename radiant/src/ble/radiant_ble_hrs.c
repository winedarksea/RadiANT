/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_hrs.c - the GATT half of the Heart Rate Service
 * (CONFIG_RADIANT_BLE_HRS). See <radiant/radiant_ble_hrs.h> for what it does
 * differently from Zephyr's BT_HRS and why; the measurement bytes come from
 * the pure encoder in radiant_ble_hrs_encode.c.
 *
 * Two characteristics and no third:
 *   - Heart Rate Measurement: notify only (no read, no write), with its CCC;
 *   - Body Sensor Location: read, CONFIG_RADIANT_BLE_HRS_BODY_SENSOR_LOCATION.
 * No Heart Rate Control Point: it exists only to reset Energy Expended, which
 * this service does not support.
 *
 * Open permissions, as HRS specifies none: a heart-rate sensor is read by any
 * collector without pairing.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <radiant/radiant_ble_hrs.h>

LOG_MODULE_DECLARE(radiant_ble_adv, CONFIG_RADIANT_BLE_LOG_LEVEL);

BUILD_ASSERT(CONFIG_RADIANT_BLE_HRS_BODY_SENSOR_LOCATION <= RADIANT_BLE_HRS_BSL_FOOT,
	     "Body Sensor Location values above 6 are reserved");

static const uint8_t body_sensor_location = CONFIG_RADIANT_BLE_HRS_BODY_SENSOR_LOCATION;

static ssize_t read_bsl(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			void *buf, uint16_t len, uint16_t offset)
{
	return bt_gatt_attr_read(conn, attr, buf, len, offset,
				 &body_sensor_location, sizeof(body_sensor_location));
}

static void meas_ccc_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	LOG_INF("HRS notifications %s", (value == BT_GATT_CCC_NOTIFY) ? "on" : "off");
}

BT_GATT_SERVICE_DEFINE(radiant_hrs_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_HRS),
	BT_GATT_CHARACTERISTIC(BT_UUID_HRS_MEASUREMENT, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(meas_ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
	BT_GATT_CHARACTERISTIC(BT_UUID_HRS_BODY_SENSOR, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_bsl, NULL, NULL),
);

/* The Measurement characteristic's declaration: bt_gatt_notify() and
 * bt_gatt_is_subscribed() both accept it and find the value and CCC. */
#define MEAS_ATTR (&radiant_hrs_svc.attrs[1])

/* Flags + UINT16 rate + 30 RR-Intervals. A larger MTU than this only means
 * more room than the application ever has intervals for. */
#define MEAS_BUF_MAX (RADIANT_BLE_HRS_MEAS_MIN + 2u * 30u)

struct notify_ctx {
	const struct radiant_ble_hrs_meas *m;
	int sent;
	int err;
	size_t rr_sent;
	bool probe_only;
};

static bool conn_subscribed(struct bt_conn *conn)
{
	struct bt_conn_info info;

	if (bt_conn_get_info(conn, &info) != 0 || info.state != BT_CONN_STATE_CONNECTED) {
		return false;
	}
	return bt_gatt_is_subscribed(conn, MEAS_ATTR, BT_GATT_CCC_NOTIFY);
}

static void notify_one(struct bt_conn *conn, void *user_data)
{
	struct notify_ctx *ctx = user_data;
	uint8_t buf[MEAS_BUF_MAX];
	uint16_t mtu;
	size_t cap;
	size_t rr_written;
	int len;
	int err;

	if (!conn_subscribed(conn)) {
		return;
	}
	if (ctx->probe_only) {
		ctx->sent++;
		return;
	}

	/* Each connection at its own MTU: the payload is ATT MTU - 3. */
	mtu = bt_gatt_get_mtu(conn);
	cap = (mtu > 3u) ? (size_t)(mtu - 3u) : 0u;
	if (cap > sizeof(buf)) {
		cap = sizeof(buf);
	}

	len = radiant_ble_hrs_encode(ctx->m, buf, cap, &rr_written);
	if (len < 0) {
		ctx->err = len;
		return;
	}

	err = bt_gatt_notify(conn, MEAS_ATTR, buf, (uint16_t)len);
	if (err == 0) {
		ctx->sent++;
		/* Only an accepted notification consumes intervals. */
		if (rr_written > ctx->rr_sent) {
			ctx->rr_sent = rr_written;
		}
	} else {
		LOG_DBG("HRS notify: %d", err);
	}
}

bool radiant_ble_hrs_subscribed(void)
{
	struct notify_ctx ctx = { .probe_only = true };

	bt_conn_foreach(BT_CONN_TYPE_LE, notify_one, &ctx);
	return ctx.sent > 0;
}

int radiant_ble_hrs_notify(const struct radiant_ble_hrs_meas *m, size_t *rr_sent)
{
	struct notify_ctx ctx = { .m = m };

	if (rr_sent != NULL) {
		*rr_sent = 0u;
	}
	if (m == NULL) {
		return -EINVAL;
	}
	bt_conn_foreach(BT_CONN_TYPE_LE, notify_one, &ctx);
	if (rr_sent != NULL) {
		*rr_sent = ctx.rr_sent;
	}
	return (ctx.err != 0) ? ctx.err : ctx.sent;
}
