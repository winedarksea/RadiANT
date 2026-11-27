/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_adv.c - Zephyr glue for the advertising lifecycle. Every
 * decision is radiant_ble_policy.c's; this file feeds it the connection
 * events and calls bt_le_adv_start() when it says so. See
 * <radiant/radiant_ble_adv.h> for why restart is from `recycled` and why it
 * retries.
 *
 * CONTEXTS. The connection callbacks run on the Bluetooth RX thread, the
 * watchdog on the application's tick (the system work queue in both apps),
 * radiant_ble_adv_ensure() on whichever thread starts BLE. bt_le_adv_start()
 * is never called from a BT callback - it would re-enter the host from its
 * own callback - only from adv_work on the system work queue or from
 * ensure(). The state is under a spinlock that is never held across a call
 * into the stack.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include <radiant/radiant_ble_adv.h>

LOG_MODULE_REGISTER(radiant_ble_adv, CONFIG_RADIANT_BLE_LOG_LEVEL);

BUILD_ASSERT(CONFIG_RADIANT_BLE_ADV_BACKOFF_MIN_MS <=
	     CONFIG_RADIANT_BLE_ADV_BACKOFF_MAX_MS,
	     "the first advertising retry delay is above the cap");

static struct k_spinlock lock;
static struct radiant_ble_adv_state st;
static bool initialised;

static const struct bt_le_adv_param *adv_param;
static const struct bt_data *adv_ad;
static size_t adv_ad_len;
static const struct bt_data *adv_sd;
static size_t adv_sd_len;

static void adv_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(adv_work, adv_work_fn);

/* One attempt. Returns the bt_le_adv_start() result; schedules the retry the
 * policy asks for. */
static int attempt(const char *why)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool go = initialised && radiant_ble_adv_begin_attempt(&st);
	uint32_t failures;
	uint32_t delay;
	int err;

	k_spin_unlock(&lock, key);
	if (!go) {
		return 0;
	}

	err = bt_le_adv_start(adv_param, adv_ad, adv_ad_len, adv_sd, adv_sd_len);

	key = k_spin_lock(&lock);
	delay = radiant_ble_adv_note_result(&st, err);
	failures = st.failures;
	k_spin_unlock(&lock, key);

	if (delay != 0u) {
		/* Once per failure run at error level, then quietly: a stuck
		 * stack must not flood the log at the 5 s cap forever. */
		if (failures == 1u) {
			LOG_ERR("bt_le_adv_start (%s): %d - retrying in %u ms "
				"until it succeeds", why, err, delay);
		} else {
			LOG_DBG("bt_le_adv_start retry %u: %d, next in %u ms",
				failures, err, delay);
		}
		(void)k_work_reschedule(&adv_work, K_MSEC(delay));
	} else if (err == 0) {
		LOG_INF("BLE advertising (%s)", why);
	}
	return err == -EALREADY ? 0 : err;
}

static void adv_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	/* st.backoff_ms stays set while this retry is made: note_result()
	 * doubles from it on failure and clears it on success. */
	(void)attempt("restart");
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	ARG_UNUSED(conn);

	k_spinlock_key_t key = k_spin_lock(&lock);

	radiant_ble_adv_note_connected(&st, err == 0u);
	k_spin_unlock(&lock, key);
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(reason);

	k_spinlock_key_t key = k_spin_lock(&lock);

	radiant_ble_adv_note_disconnected(&st);
	k_spin_unlock(&lock, key);
}

static void on_recycled(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool go = radiant_ble_adv_note_recycled(&st);

	k_spin_unlock(&lock, key);
	if (go) {
		(void)k_work_reschedule(&adv_work, K_NO_WAIT);
	}
}

BT_CONN_CB_DEFINE(radiant_ble_adv_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.recycled = on_recycled,
};

int radiant_ble_adv_init(const struct bt_le_adv_param *param,
			 const struct bt_data *ad, size_t ad_len,
			 const struct bt_data *sd, size_t sd_len)
{
	if (param == NULL || (ad == NULL && ad_len != 0u) ||
	    (sd == NULL && sd_len != 0u)) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	adv_param = param;
	adv_ad = ad;
	adv_ad_len = ad_len;
	adv_sd = sd;
	adv_sd_len = sd_len;
	radiant_ble_adv_state_init(&st, CONFIG_BT_MAX_CONN,
				   CONFIG_RADIANT_BLE_ADV_BACKOFF_MIN_MS,
				   CONFIG_RADIANT_BLE_ADV_BACKOFF_MAX_MS,
				   CONFIG_RADIANT_BLE_ADV_WATCHDOG_MS);
	st.last_probe_ms = k_uptime_get_32();
	initialised = true;
	k_spin_unlock(&lock, key);
	return 0;
}

int radiant_ble_adv_ensure(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	if (!initialised) {
		k_spin_unlock(&lock, key);
		return -EINVAL;
	}
	st.want = true;
	k_spin_unlock(&lock, key);

	return attempt("start");
}

void radiant_ble_adv_watchdog(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool due = initialised &&
		   radiant_ble_adv_watchdog_due(&st, k_uptime_get_32());

	k_spin_unlock(&lock, key);
	if (due) {
		(void)k_work_reschedule(&adv_work, K_NO_WAIT);
	}
}

bool radiant_ble_adv_is_active(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool active = st.active;

	k_spin_unlock(&lock, key);
	return active;
}
