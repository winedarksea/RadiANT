/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_pairing.c - the pairing window, the bond LRU and the bond
 * service calls. See <radiant/radiant_ble_pairing.h> for the policy; the
 * decisions (window expiry, which bond to evict, the record codec) are in
 * radiant_ble_policy.c and tested on native_sim.
 *
 * CONTEXTS. pairing_accept, connected and security_changed run on the
 * Bluetooth RX thread; the window is opened from a console driver's context
 * or at boot. Every bond deletion and settings write happens on the system
 * work queue (room_work, lru_work), never inside a BT callback.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/settings/settings.h>

#include <radiant/radiant_ble_pairing.h>

LOG_MODULE_DECLARE(radiant_ble_adv, CONFIG_RADIANT_BLE_LOG_LEVEL);

BUILD_ASSERT(sizeof(bt_addr_le_t) == RADIANT_BLE_ADDR_BYTES,
	     "the LRU record copies bt_addr_le_t byte-wise");
BUILD_ASSERT(CONFIG_RADIANT_BLE_PAIRING_WINDOW_AT_BOOT_S <=
	     RADIANT_BLE_PAIRING_WINDOW_MAX_S,
	     "the boot pairing window is longer than the 600 s cap");

#define LRU_N   CONFIG_BT_MAX_PAIRED
#define LRU_KEY "rble/lru"

/*
 * THE SPARE SLOT. One key slot is always kept free, so the bonds a user can
 * hold are CONFIG_BT_MAX_PAIRED - 1. Without BT_KEYS_OVERWRITE_OLDEST a
 * Pairing Request that finds no free key slot is dropped inside
 * smp_pairing_req() before SMP_FLAG_PAIRING is set - no pairing_failed, no
 * security_changed, nothing this layer could react to - so the room has to
 * exist before the request arrives. Outside the window pairing_accept refuses
 * after SMP_FLAG_PAIRING is set, and smp_pairing_complete() releases the
 * stranger's half-made keys, so the spare comes back and no bond is lost.
 * Inside the window the new bond takes the spare; pairing_complete then
 * evicts the least recently used bond to restore it.
 */
#define BOND_CAPACITY (CONFIG_BT_MAX_PAIRED - 1)
BUILD_ASSERT(CONFIG_BT_MAX_PAIRED >= 2,
	     "CONFIG_RADIANT_BLE_PAIRING_WINDOW keeps one key slot spare: "
	     "CONFIG_BT_MAX_PAIRED must be at least 2 (capacity MAX_PAIRED - 1)");

static struct k_spinlock lock;
static uint64_t win_opened_ms;
static uint32_t win_len_ms; /* 0 = closed */

/* Guarded by `lock`; written to settings from lru_work. */
static struct radiant_ble_lru_entry lru[LRU_N];
static uint32_t lru_next;
/* The peer whose bond just completed: never the eviction victim. */
static uint8_t just_paired[RADIANT_BLE_ADDR_BYTES];

static void room_work_fn(struct k_work *w);
static K_WORK_DEFINE(room_work, room_work_fn);
static void lru_work_fn(struct k_work *w);
static K_WORK_DEFINE(lru_work, lru_work_fn);

static bool window_allows_now(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);
	bool ok = radiant_ble_pairing_window_allows(win_opened_ms, win_len_ms,
						    (uint64_t)k_uptime_get());

	if (!ok) {
		win_len_ms = 0u;
	}
	k_spin_unlock(&lock, key);
	return ok;
}

/* ── The LRU record ───────────────────────────────────────────────────────── */

static void lru_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
#if defined(CONFIG_BT_SETTINGS)
	uint8_t buf[1u + LRU_N * RADIANT_BLE_LRU_ENTRY_BYTES];
	k_spinlock_key_t key = k_spin_lock(&lock);
	int len = radiant_ble_lru_encode(lru, LRU_N, buf, sizeof(buf));

	k_spin_unlock(&lock, key);
	if (len > 0) {
		int err = settings_save_one(LRU_KEY, buf, (size_t)len);

		if (err != 0) {
			LOG_WRN("bond LRU not saved: %d (eviction order falls "
				"back to the last saved one)", err);
		}
	}
#endif
}

#if defined(CONFIG_BT_SETTINGS)
static int lru_set(const char *name, size_t len, settings_read_cb read_cb,
		   void *cb_arg)
{
	uint8_t buf[1u + LRU_N * RADIANT_BLE_LRU_ENTRY_BYTES * 2u];
	ssize_t got;

	if (!settings_name_steq(name, "lru", NULL)) {
		return -ENOENT;
	}
	if (len > sizeof(buf)) {
		return -EINVAL;
	}
	got = read_cb(cb_arg, buf, len);
	if (got < 0) {
		return (int)got;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);
	uint32_t hi = 0u;

	if (radiant_ble_lru_decode(buf, (size_t)got, lru, LRU_N, &hi) < 0) {
		memset(lru, 0, sizeof(lru));
		hi = 0u;
	}
	lru_next = hi;
	k_spin_unlock(&lock, key);
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(radiant_ble_lru, "rble", NULL, lru_set, NULL, NULL);
#endif

/* ── Making room, only inside an open window ──────────────────────────────── */

struct bond_list {
	uint8_t addr[LRU_N][RADIANT_BLE_ADDR_BYTES];
	size_t  n;
};

static void collect_bond(const struct bt_bond_info *info, void *user)
{
	struct bond_list *b = user;

	if (b->n < LRU_N) {
		memcpy(b->addr[b->n], &info->addr, RADIANT_BLE_ADDR_BYTES);
	}
	b->n++;
}

/*
 * BT_ID_DEFAULT only. With CONFIG_BT_ID_MAX > 1 a bond on another identity
 * shares the key pool but is not counted here, so a full pool can look
 * not-full and no slot is freed - which fails closed (the pairing is refused,
 * no bond is lost). Every application here has one identity.
 */
static bool is_bonded(const bt_addr_le_t *addr)
{
	struct bond_list bonds = { .n = 0u };

	bt_foreach_bond(BT_ID_DEFAULT, collect_bond, &bonds);
	for (size_t i = 0; i < MIN(bonds.n, (size_t)LRU_N); i++) {
		if (memcmp(bonds.addr[i], addr, RADIANT_BLE_ADDR_BYTES) == 0) {
			return true;
		}
	}
	return false;
}

/* Runs only after a bond completed (on_pairing_complete). Deletes least
 * recently used bonds - never the one just made, never a connected peer's -
 * until the store is back at BOND_CAPACITY, which frees the spare slot. */
static void room_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	/* Bounded: one deletion per pass, and never more passes than slots. */
	for (int pass = 0; pass < LRU_N; pass++) {
		struct bond_list bonds = { .n = 0u };
		bool connected[LRU_N];
		uint8_t keep[RADIANT_BLE_ADDR_BYTES];
		int victim;

		bt_foreach_bond(BT_ID_DEFAULT, collect_bond, &bonds);
		if (bonds.n > LRU_N) {
			bonds.n = LRU_N;
		}
		for (size_t i = 0; i < bonds.n; i++) {
			struct bt_conn *c = bt_conn_lookup_addr_le(
				BT_ID_DEFAULT, (const bt_addr_le_t *)bonds.addr[i]);

			connected[i] = c != NULL;
			if (c != NULL) {
				bt_conn_unref(c);
			}
		}

		k_spinlock_key_t key = k_spin_lock(&lock);

		memcpy(keep, just_paired, sizeof(keep));
		victim = radiant_ble_lru_over_capacity_victim(
			lru, LRU_N, (const uint8_t (*)[RADIANT_BLE_ADDR_BYTES])bonds.addr,
			connected, bonds.n, BOND_CAPACITY, keep);
		if (victim >= 0) {
			radiant_ble_lru_forget(lru, LRU_N, bonds.addr[victim]);
		}
		k_spin_unlock(&lock, key);

		if (victim == -1) {
			return; /* at or under capacity: the spare slot is free */
		}
		if (victim < 0) {
			LOG_WRN("bond store over capacity but every other bonded peer is "
				"connected: the spare slot stays used until one leaves");
			return;
		}

		int err = bt_unpair(BT_ID_DEFAULT,
				    (const bt_addr_le_t *)bonds.addr[victim]);

		if (err != 0) {
			LOG_ERR("evicting the least recently used bond: bt_unpair %d",
				err);
			return;
		}
		LOG_INF("new bond made inside the pairing window: least recently "
			"used bond deleted to keep a key slot spare");
		(void)k_work_submit(&lru_work);
	}
}

/* ── Keeping the LRU order ────────────────────────────────────────────────── */

static void lru_touch(const bt_addr_le_t *addr)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	radiant_ble_lru_touch(lru, LRU_N, (const uint8_t *)addr, &lru_next);
	k_spin_unlock(&lock, key);
	(void)k_work_submit(&lru_work);
}

/*
 * A BONDED peer reconnecting and encrypting: the host resolved its RPA from
 * the stored IRK at connect, so dst is already the identity address and is
 * in the bond list. On a FIRST pairing this event comes from the HCI
 * Encryption Change, before key distribution - dst is still the RPA and the
 * peer is not yet bonded - so it is skipped here and pairing_complete()
 * records it under the identity instead.
 */
static void on_security_changed(struct bt_conn *conn, bt_security_t level,
				enum bt_security_err err)
{
	const bt_addr_le_t *dst = bt_conn_get_dst(conn);

	if (err != BT_SECURITY_ERR_SUCCESS || level < BT_SECURITY_L2 ||
	    !is_bonded(dst)) {
		return;
	}
	lru_touch(dst);
}

/* Belt and braces: anything recorded under an RPA moves to the identity. */
static void on_identity_resolved(struct bt_conn *conn, const bt_addr_le_t *rpa,
				 const bt_addr_le_t *identity)
{
	ARG_UNUSED(conn);

	k_spinlock_key_t key = k_spin_lock(&lock);
	bool moved = radiant_ble_lru_rekey(lru, LRU_N, (const uint8_t *)rpa,
					   (const uint8_t *)identity);

	k_spin_unlock(&lock, key);
	if (moved) {
		(void)k_work_submit(&lru_work);
	}
}

BT_CONN_CB_DEFINE(radiant_ble_pairing_conn_cb) = {
	.security_changed = on_security_changed,
	.identity_resolved = on_identity_resolved,
};

/* After key distribution and identity resolution: dst is the identity. Only
 * a bond gets an entry - a non-bonded pairing leaves nothing to evict. The
 * new bond is recorded first (most recent, so never the victim), then the
 * spare slot it took is restored by evicting the least recently used. */
static void on_pairing_complete(struct bt_conn *conn, bool bonded)
{
	const bt_addr_le_t *dst = bt_conn_get_dst(conn);

	if (!bonded) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	memcpy(just_paired, dst, sizeof(just_paired));
	k_spin_unlock(&lock, key);
	lru_touch(dst);
	(void)k_work_submit(&room_work);
}

static void on_bond_deleted(uint8_t id, const bt_addr_le_t *peer)
{
	if (id != BT_ID_DEFAULT) {
		return;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	radiant_ble_lru_forget(lru, LRU_N, (const uint8_t *)peer);
	k_spin_unlock(&lock, key);
	(void)k_work_submit(&lru_work);
}

static struct bt_conn_auth_info_cb auth_info_cb = {
	.pairing_complete = on_pairing_complete,
	.bond_deleted = on_bond_deleted,
};

/* ── Pairing acceptance ───────────────────────────────────────────────────── */

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

/* Registered at boot, not by the application: a window that is only
 * enforced when the application remembers to call an init function fails
 * open. Registration only stores a pointer, so it needs no bt_enable(). */
static int pairing_sys_init(void)
{
	int err = bt_conn_auth_cb_register(&auth_cb);

	if (err != 0) {
		LOG_ERR("bt_conn_auth_cb_register: %d - the pairing window is "
			"NOT enforced", err);
	}
	/* The bond LRU's events (pairing_complete, bond_deleted). A list in Zephyr, so it cannot collide with an
	 * application's own. */
	int ierr = bt_conn_auth_info_cb_register(&auth_info_cb);

	if (ierr != 0) {
		LOG_ERR("bt_conn_auth_info_cb_register: %d - bond LRU order "
			"is not kept", ierr);
		if (err == 0) {
			err = ierr;
		}
	}
#if CONFIG_RADIANT_BLE_PAIRING_WINDOW_AT_BOOT_S > 0
	/* Opening evicts nothing: the spare key slot is what lets a pairing
	 * in this window succeed, and a bond is evicted only after the new
	 * one is made (on_pairing_complete). */
	win_opened_ms = (uint64_t)k_uptime_get();
	win_len_ms = CONFIG_RADIANT_BLE_PAIRING_WINDOW_AT_BOOT_S * 1000u;
#endif
	return err;
}

SYS_INIT(pairing_sys_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* ── Public calls ─────────────────────────────────────────────────────────── */

int radiant_ble_pairing_window_open(uint32_t seconds)
{
	if (seconds == 0u || seconds > RADIANT_BLE_PAIRING_WINDOW_MAX_S) {
		return -EINVAL;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	win_opened_ms = (uint64_t)k_uptime_get();
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
		return err;
	}

	k_spinlock_key_t key = k_spin_lock(&lock);

	memset(lru, 0, sizeof(lru));
	lru_next = 0u;
	k_spin_unlock(&lock, key);
	(void)k_work_submit(&lru_work);
	LOG_INF("every bond deleted");
	return 0;
}
