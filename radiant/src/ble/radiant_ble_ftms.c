/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_ftms.c - the GATT half of the FTMS core (CONFIG_RADIANT_BLE_FTMS).
 * See <radiant/radiant_ble_ftms.h>. Every decision is made by the pure
 * functions in radiant_ble_ftms_core.c; this file moves bytes, holds the one
 * pending procedure and runs outcomes.
 *
 * Traps carried over from apps/treadmill/src/treadmill_ble.c, where this
 * service was first written:
 *
 * 1. UUID MACRO NAMES. BT_UUID_FMS is the FTMS *service* 0x1826;
 *    BT_UUID_GATT_FMS is the Fitness Machine *Status characteristic* 0x2ADA.
 *    One `GATT_` apart, and swapping them compiles.
 *
 * 2. THE CONTROL POINT REQUIRES ENCRYPTION (Table 4.1): the attribute carries
 *    BT_GATT_PERM_WRITE_ENCRYPT, which is why this option depends on BT_SMP.
 *
 * 3. THE THREAD HOP. write_cp() runs on the BT RX thread; the machine's state
 *    and the control arbiter are workqueue-only and lock-free. So write_cp()
 *    keeps only the ATT-level checks (§4.16.3 wants those two failures as ATT
 *    errors) and copies the request, with the machine's epoch at receipt, into
 *    a one-deep slot; EVERYTHING from the decision onward runs in cp_work on
 *    the system workqueue. Answering late is free: the response is a GATT
 *    indication, never this function's return value. The connection is
 *    referenced across the hop, and re-checked before anything is applied.
 *
 * 4. NO PROCEDURE TIMEOUT. §4.16.4 is titled "Procedure Timeout" and gives no
 *    number; none is invented here.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <radiant/radiant_ble_ftms.h>
#if defined(CONFIG_RADIANT_CTRL_ARBITER)
#include <radiant/radiant_ctrl_arbiter.h>
#endif

LOG_MODULE_DECLARE(radiant_ble_adv, CONFIG_RADIANT_BLE_LOG_LEVEL);

BUILD_ASSERT(IS_ENABLED(CONFIG_RADIANT_BLE_FTMS_TREADMILL) +
		     IS_ENABLED(CONFIG_RADIANT_BLE_FTMS_INDOOR_BIKE) == 1,
	     "select exactly one FTMS machine type: RADIANT_BLE_FTMS_TREADMILL "
	     "or RADIANT_BLE_FTMS_INDOOR_BIKE");

/* The largest data record a notification carries; a larger ATT MTU only
 * means more room than any record here uses. */
#define DATA_BUF_MAX 64u

static const struct radiant_ble_ftms_machine *machine;

/* Training Status, as last notified. Workqueue-owned. */
static uint8_t training_status = RADIANT_BLE_FTMS_TRAINING_IDLE;

/*
 * §4.16.2: one client holds control at a time, tracked by connection rather
 * than by a bool because "who has it" is what the decision and the 0xFF
 * notification need. FTMS-internal: whether the BLE side owns the machine AT
 * ALL against an FE-C controller is the arbiter's question. Workqueue-owned:
 * written only from cp_work_fn() and the deferred disconnect handler.
 */
static struct bt_conn *controller;

static struct bt_gatt_indicate_params cp_ind_params;
static atomic_t cp_indicating;
static uint8_t cp_ind_data[3];

static uint32_t now_ms(void)
{
	return (uint32_t)k_uptime_get_32();
}

/* ── Read handlers ──────────────────────────────────────────────────────── */

static ssize_t read_feature(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    void *buf, uint16_t len, uint16_t offset)
{
	uint8_t value[8] = { 0 };

	if (machine != NULL) {
		(void)radiant_ble_ftms_feature_encode(&machine->caps, value);
	}
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static ssize_t read_range(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			  void *buf, uint16_t len, uint16_t offset,
			  const struct radiant_ble_ftms_range *r)
{
	uint8_t value[6];

	(void)radiant_ble_ftms_range_encode(r, value);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static const struct radiant_ble_ftms_range no_range;

#if defined(CONFIG_RADIANT_BLE_FTMS_TREADMILL)
static ssize_t read_speed_range(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				void *buf, uint16_t len, uint16_t offset)
{
	return read_range(conn, attr, buf, len, offset,
			  machine ? &machine->caps.speed : &no_range);
}

static ssize_t read_incline_range(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				  void *buf, uint16_t len, uint16_t offset)
{
	return read_range(conn, attr, buf, len, offset,
			  machine ? &machine->caps.incline : &no_range);
}
#endif

#if defined(CONFIG_RADIANT_BLE_FTMS_INDOOR_BIKE)
static ssize_t read_resistance_range(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				     void *buf, uint16_t len, uint16_t offset)
{
	return read_range(conn, attr, buf, len, offset,
			  machine ? &machine->caps.resistance : &no_range);
}

static ssize_t read_power_range(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				void *buf, uint16_t len, uint16_t offset)
{
	return read_range(conn, attr, buf, len, offset,
			  machine ? &machine->caps.power : &no_range);
}
#endif

static ssize_t read_training_status(struct bt_conn *conn, const struct bt_gatt_attr *attr,
				    void *buf, uint16_t len, uint16_t offset)
{
	/* Table 4.13: flags (no status string) then the status. */
	uint8_t value[2] = { 0x00u, training_status };

	return bt_gatt_attr_read(conn, attr, buf, len, offset, value, sizeof(value));
}

static void ccc_changed_noop(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(value);
}

static ssize_t write_cp(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			const void *buf, uint16_t len, uint16_t offset, uint8_t flags);

/* ── The service ────────────────────────────────────────────────────────── */

#if defined(CONFIG_RADIANT_BLE_FTMS_TREADMILL)
#define FTMS_DATA_UUID BT_UUID_GATT_TD
#else
#define FTMS_DATA_UUID BT_UUID_GATT_IBD
#endif

BT_GATT_SERVICE_DEFINE(
	radiant_ftms_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_FMS),

	/* Table 4.1: the one unconditionally mandatory characteristic. */
	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_FMF, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_feature, NULL, NULL),

	/* The machine's data record. */
	BT_GATT_CHARACTERISTIC(FTMS_DATA_UUID, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed_noop, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_TRSTAT, BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_training_status, NULL, NULL),
	BT_GATT_CCC(ccc_changed_noop, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* §4.11-§4.14: a range is exposed because the matching Target Setting
	 * feature may be declared. Present whenever the machine type can have
	 * it; the values come from the registered caps. */
#if defined(CONFIG_RADIANT_BLE_FTMS_TREADMILL)
	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_SSR, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_speed_range, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_SIR, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_incline_range, NULL, NULL),
#endif
#if defined(CONFIG_RADIANT_BLE_FTMS_INDOOR_BIKE)
	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_SRLR, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_resistance_range, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_SPR, BT_GATT_CHRC_READ, BT_GATT_PERM_READ,
			       read_power_range, NULL, NULL),
#endif

	/* WRITE + INDICATE, and "Encryption" (Table 4.1). */
	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_FMCP, BT_GATT_CHRC_WRITE | BT_GATT_CHRC_INDICATE,
			       BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_cp, NULL),
	BT_GATT_CCC(ccc_changed_noop, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* C.6: mandatory because the control point is supported. */
	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_FMS, BT_GATT_CHRC_NOTIFY, BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed_noop, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/* Resolved by UUID, not by attrs[] index, so inserting a characteristic above
 * cannot repoint a notification at the wrong attribute. */
static const struct bt_gatt_attr *find_attr(const struct bt_uuid *uuid)
{
	return bt_gatt_find_by_uuid(radiant_ftms_svc.attrs, radiant_ftms_svc.attr_count, uuid);
}

static const struct bt_gatt_attr *cp_attr(void)
{
	static const struct bt_gatt_attr *cached;

	if (cached == NULL) {
		cached = find_attr(BT_UUID_GATT_FMCP);
	}
	return cached;
}

static const struct bt_gatt_attr *status_attr(void)
{
	static const struct bt_gatt_attr *cached;

	if (cached == NULL) {
		cached = find_attr(BT_UUID_GATT_FMS);
	}
	return cached;
}

static const struct bt_gatt_attr *trstat_attr(void)
{
	static const struct bt_gatt_attr *cached;

	if (cached == NULL) {
		cached = find_attr(BT_UUID_GATT_TRSTAT);
	}
	return cached;
}

static const struct bt_gatt_attr *data_attr(void)
{
	static const struct bt_gatt_attr *cached;

	if (cached == NULL) {
		cached = find_attr(FTMS_DATA_UUID);
	}
	return cached;
}

/* ── Notifications ──────────────────────────────────────────────────────── */

/* The Fitness Machine Status, to EVERY subscribed client - the one whose
 * procedure caused it included. NULL means "every subscriber". */
static void notify_status(const uint8_t *msg, uint16_t len)
{
	const struct bt_gatt_attr *attr = status_attr();

	if (attr != NULL && len != 0u) {
		(void)bt_gatt_notify(NULL, attr, msg, len);
	}
}

static void set_training_status(uint8_t ts)
{
	uint8_t value[2] = { 0x00u, ts };
	const struct bt_gatt_attr *attr;

	if (training_status == ts) {
		return;
	}
	training_status = ts;
	attr = trstat_attr();
	if (attr != NULL) {
		(void)bt_gatt_notify(NULL, attr, value, sizeof(value));
	}
}

void radiant_ble_ftms_session_changed(enum radiant_ble_ftms_run prev,
				      enum radiant_ble_ftms_run next)
{
	uint8_t msg[2];
	size_t n = radiant_ble_ftms_session_status(prev, next, msg);

	set_training_status(radiant_ble_ftms_training_status(next));
	notify_status(msg, (uint16_t)n);
}

static void notify_data_one(struct bt_conn *conn, void *user_data)
{
	const struct bt_gatt_attr *attr = user_data;
	struct bt_conn_info info;
	uint8_t buf[DATA_BUF_MAX];
	uint16_t mtu;
	size_t cap;
	int len;

	if (bt_conn_get_info(conn, &info) != 0 || info.state != BT_CONN_STATE_CONNECTED ||
	    !bt_gatt_is_subscribed(conn, attr, BT_GATT_CCC_NOTIFY)) {
		return;
	}
	mtu = bt_gatt_get_mtu(conn);
	cap = (mtu > 3u) ? (size_t)(mtu - 3u) : 0u;
	if (cap > sizeof(buf)) {
		cap = sizeof(buf);
	}
	len = machine->encode_data(buf, cap, machine->ctx);
	if (len > 0) {
		(void)bt_gatt_notify(conn, attr, buf, (uint16_t)len);
	}
}

void radiant_ble_ftms_notify_data(void)
{
	const struct bt_gatt_attr *attr = data_attr();

	if (machine == NULL || machine->encode_data == NULL || attr == NULL) {
		return;
	}
	bt_conn_foreach(BT_CONN_TYPE_LE, notify_data_one, (void *)attr);
}

/* ── Control ────────────────────────────────────────────────────────────── */

static void release_control(void)
{
	controller = NULL;
#if defined(CONFIG_RADIANT_CTRL_ARBITER)
	/* Hands the machine back, so an FE-C controller's next command is
	 * applied rather than REJECTED. */
	radiant_ctrl_release(RADIANT_CTRL_BLE);
#endif
}

void radiant_ble_ftms_revoke_control(void)
{
	static const uint8_t lost[1] = { RADIANT_BLE_FTMS_STATUS_CONTROL_LOST };
	const struct bt_gatt_attr *attr = status_attr();

	if (controller == NULL) {
		return;
	}
	/* §4.17: Control Permission Lost goes to the client that lost it. */
	if (attr != NULL) {
		(void)bt_gatt_notify(controller, attr, lost, sizeof(lost));
	}
	LOG_INF("FTMS: control revoked");
	release_control();
}

bool radiant_ble_ftms_has_controller(void)
{
	return controller != NULL;
}

/* ── The control point ──────────────────────────────────────────────────── */

static void cp_indicate_destroy(struct bt_gatt_indicate_params *params)
{
	ARG_UNUSED(params);
	atomic_set(&cp_indicating, 0);
}

/* Table 4.23's Response Code: 0x80, the request op code, the result. */
static void cp_respond(struct bt_conn *conn, uint8_t request_op, uint8_t result)
{
	const struct bt_gatt_attr *attr = cp_attr();

	if (attr == NULL) {
		return;
	}
	if (!atomic_cas(&cp_indicating, 0, 1)) {
		/* ATT allows one outstanding indication per connection, and
		 * write_cp() refuses a second procedure while one is out. */
		LOG_WRN("FTMS: an indication is already outstanding");
		return;
	}

	cp_ind_data[0] = RADIANT_BLE_FTMS_OP_RESPONSE_CODE;
	cp_ind_data[1] = request_op;
	cp_ind_data[2] = result;

	memset(&cp_ind_params, 0, sizeof(cp_ind_params));
	cp_ind_params.attr = attr;
	cp_ind_params.data = cp_ind_data;
	cp_ind_params.len = sizeof(cp_ind_data);
	cp_ind_params.destroy = cp_indicate_destroy;

	if (bt_gatt_indicate(conn, &cp_ind_params) != 0) {
		atomic_set(&cp_indicating, 0);
	}
}

/* One request in flight: write_cp() refuses a second with ATT error 0xFE. */
static struct {
	struct bt_conn *conn; /* referenced while pending */
	uint8_t data[8];
	uint16_t len;
	uint32_t epoch; /* the machine's epoch at receipt */
} cp_pending;

static atomic_t cp_pending_valid;

static void cp_work_fn(struct k_work *w)
{
	struct bt_conn *conn = cp_pending.conn;
	struct radiant_ble_ftms_cp_request req = {
		.data = cp_pending.data,
		.len = cp_pending.len,
		.epoch = cp_pending.epoch,
	};
	struct radiant_ble_ftms_session session = { 0 };
	struct radiant_ble_ftms_cp_decision d;
	struct radiant_ble_ftms_cp_outcome o;
	enum radiant_ble_ftms_owner owner;
	struct bt_conn_info info;

	ARG_UNUSED(w);

	if (conn == NULL) {
		atomic_set(&cp_pending_valid, 0);
		return;
	}

	/* The client may have gone between the write and this running.
	 * Applying a command from a client that is no longer there is wrong,
	 * and there is nobody to answer. */
	if (machine == NULL || bt_conn_get_info(conn, &info) != 0 ||
	    info.state != BT_CONN_STATE_CONNECTED) {
		LOG_WRN("FTMS: op 0x%02x dropped - the client disconnected before "
			"its procedure ran", cp_pending.data[0]);
		goto done;
	}

	if (machine->session != NULL) {
		machine->session(&session, machine->ctx);
	}
	owner = (controller == NULL)   ? RADIANT_BLE_FTMS_OWNER_NONE
		: (controller == conn) ? RADIANT_BLE_FTMS_OWNER_SELF
				       : RADIANT_BLE_FTMS_OWNER_OTHER;

	d = radiant_ble_ftms_cp_decide(&req, &session, &machine->caps, owner,
				       &machine->auth);
	o = radiant_ble_ftms_cp_execute(&d, &machine->ops);

	if (o.grant) {
#if defined(CONFIG_RADIANT_CTRL_ARBITER)
		/* AND THE CROSS-TRANSPORT TOKEN, which this may preempt: an
		 * FTMS Request Control is explicit and an FE-C command page is
		 * not. Explicit beats implicit, so the claim cannot fail; the
		 * ANT+ controller learns through REJECTED on its next command. */
		(void)radiant_ctrl_claim(RADIANT_CTRL_BLE, true, now_ms());
#endif
		controller = conn;
	}
	if (o.release) {
		release_control();
	}
	if (o.reset_view && machine->reset_view != NULL) {
		machine->reset_view(machine->ctx);
	}

	cp_respond(conn, o.op, o.result);
	notify_status(o.status, o.status_len);

done:
	bt_conn_unref(conn);
	cp_pending.conn = NULL;
	/* Cleared LAST: write_cp() will not overwrite the slot until it sees
	 * this, which keeps the copy stable for the whole of this function. */
	atomic_set(&cp_pending_valid, 0);
}

static K_WORK_DEFINE(cp_work, cp_work_fn);

static ssize_t write_cp(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0u) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1u || len > sizeof(cp_pending.data)) {
		/* The buffer's bound, not the spec's: the longest procedure
		 * here is seven octets (0x11). */
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	/* §4.16.3: a procedure is in progress from the moment it is queued
	 * until its indication is confirmed. ATT error 0xFE (Procedure Already In Progress, CSS),
	 * not a Response Code. */
	if (atomic_get(&cp_indicating) != 0 || atomic_get(&cp_pending_valid) != 0) {
		return BT_GATT_ERR(BT_ATT_ERR_PROCEDURE_IN_PROGRESS);
	}
	/* The CCC must be configured for INDICATION: a client that subscribed
	 * for notification would never receive its answer. 0xFD. */
	if (!bt_gatt_is_subscribed(conn, cp_attr(), BT_GATT_CCC_INDICATE)) {
		return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
	}

	memcpy(cp_pending.data, buf, len);
	cp_pending.len = len;
	/* The epoch NOW, on this thread: a local stop between here and the
	 * work item running cancels an actuating request. */
	cp_pending.epoch = (machine != NULL && machine->epoch != NULL)
				   ? machine->epoch(machine->ctx)
				   : 0u;
	cp_pending.conn = bt_conn_ref(conn);
	atomic_set(&cp_pending_valid, 1);
	(void)k_work_submit(&cp_work);

	return len;
}

/* ── Disconnects ────────────────────────────────────────────────────────── */

/*
 * Losing control on disconnect is §4.16.2, and it happens on the workqueue
 * like every other write to `controller` and the token. THE CONNECTION IS
 * REFERENCED ACROSS THE HOP: the work item compares pointers, and a freed
 * bt_conn can be reallocated at the same address for the next connection -
 * which would revoke the permission of a client that had just been granted it.
 */
static struct bt_conn *disc_conn;

static void disc_work_fn(struct k_work *w)
{
	struct bt_conn *conn = disc_conn;

	ARG_UNUSED(w);

	if (conn == NULL) {
		return;
	}
	disc_conn = NULL;
	if (controller == conn) {
		/* Nothing else changes: a dropped link must not stop the belt
		 * under a runner, and Training Status keeps following the
		 * machine. */
		release_control();
	}
	bt_conn_unref(conn);
}

static K_WORK_DEFINE(disc_work, disc_work_fn);

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(reason);

	/* One slot: with CONFIG_BT_MAX_CONN=1 there cannot be a second
	 * disconnect in flight. If that rises, this becomes a list. */
	if (disc_conn == NULL) {
		disc_conn = bt_conn_ref(conn);
		(void)k_work_submit(&disc_work);
	} else {
		LOG_WRN("FTMS: a disconnect is already being processed");
	}
}

BT_CONN_CB_DEFINE(radiant_ble_ftms_conn_cb) = {
	.disconnected = on_disconnected,
};

/* ── Registration ───────────────────────────────────────────────────────── */

int radiant_ble_ftms_register(const struct radiant_ble_ftms_machine *m)
{
	struct radiant_ble_ftms_session s = { 0 };

	if (m == NULL) {
		return -EINVAL;
	}
	if (machine != NULL) {
		return -EALREADY;
	}
	machine = m;
	if (m->session != NULL) {
		m->session(&s, m->ctx);
	}
	training_status = radiant_ble_ftms_training_status(s.run);
	return 0;
}
