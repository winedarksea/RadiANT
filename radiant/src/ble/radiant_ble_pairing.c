/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_pairing.c - the pairing window and bond service calls. See
 * <radiant/radiant_ble_pairing.h> for the policy. The window decision is
 * radiant_ble_pairing_window_allows() in radiant_ble_policy.c.
 *
 * pairing_accept runs on the Bluetooth RX thread; the window is opened from a
 * console driver's context. Two words under a spinlock.
 */

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>

#include <radiant/radiant_ble_pairing.h>

LOG_MODULE_DECLARE(radiant_ble_adv, CONFIG_RADIANT_BLE_LOG_LEVEL);

static struct k_spinlock lock;
static uint32_t win_opened_ms;
static uint32_t win_len_ms; /* 0 = closed */

static bool window_allows_now(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool ok = radiant_ble_pairing_window_allows(win_opened_ms, win_len_ms,
						    k_uptime_get_32());

	if (!ok) {
		win_len_ms = 0u; /* expired: closed for good, no wrap-around */
	}
	k_spin_unlock(&lock, key);
	return ok;
}

static enum bt_security_err pairing_accept(struct bt_conn *conn,
					   const struct bt_conn_pairing_feat *const feat)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(feat);

	if (window_allows_now()) {
		LOG_INF("pairing accepted: the pairing window is open");
		return BT_SECURITY_ERR_SUCCESS;
	}
	LOG_WRN("pairing refused: no pairing window is open (bonded peers "
		"still connect; open one from the console to pair)");
	return BT_SECURITY_ERR_PAIR_NOT_ALLOWED;
}

static struct bt_conn_auth_cb auth_cb = {
	.pairing_accept = pairing_accept,
};

int radiant_ble_pairing_init(void)
{
	int err = bt_conn_auth_cb_register(&auth_cb);

	if (err != 0) {
		LOG_ERR("bt_conn_auth_cb_register: %d - the pairing window is "
			"not enforced", err);
	}
	return err;
}

int radiant_ble_pairing_window_open(uint32_t seconds)
{
	if (seconds == 0u || seconds > RADIANT_BLE_PAIRING_WINDOW_MAX_S) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	win_opened_ms = k_uptime_get_32();
	win_len_ms = seconds * 1000u;
	k_spin_unlock(&lock, key);
	LOG_INF("pairing window open for %u s", seconds);
	return 0;
}

void radiant_ble_pairing_window_close(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	win_len_ms = 0u;
	k_spin_unlock(&lock, key);
}

bool radiant_ble_pairing_window_is_open(void)
{
	return window_allows_now();
}

int radiant_ble_bonds_forget_all(void)
{
	int err = bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);

	if (err != 0) {
		LOG_ERR("bt_unpair (all): %d", err);
	} else {
		LOG_INF("every bond deleted");
	}
	return err;
}
