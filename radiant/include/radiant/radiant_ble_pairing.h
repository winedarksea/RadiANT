/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_pairing.h - the bonding policy for equipment.
 *
 * A treadmill or a trainer in a gym or a living room must not accept a new
 * pairing from whoever walks past, but must always let its owner recover
 * after replacing a phone. So (CONFIG_RADIANT_BLE_PAIRING_WINDOW):
 *
 *   - New pairings are accepted only while a pairing window is open.
 *     radiant_ble_pairing_window_open(seconds) is called from a local console
 *     button by the source driver - physical presence is the authorisation.
 *   - Outside the window, a pairing request is refused with "pairing not
 *     allowed" (BT_SMP_APP_PAIRING_ACCEPT); bonded peers still connect and
 *     encrypt with their stored keys, which involves no pairing.
 *   - Bond storage full (CONFIG_BT_MAX_PAIRED): a pairing inside the window
 *     replaces the least recently used bond (BT_KEYS_OVERWRITE_OLDEST; the
 *     aging counter advances on every encrypted connection and is saved with
 *     BT_SETTINGS), so a replaced phone always recovers through the window.
 *   - radiant_ble_bonds_forget_all() for service.
 *
 * The layer registers the auth callbacks (bt_conn_auth_cb_register()) in
 * radiant_ble_pairing_init(); an application using this option does not
 * register its own. Pairing stays Just Works, as before - the window, not a
 * passkey, is what gates it.
 */
#ifndef RADIANT_BLE_PAIRING_H_
#define RADIANT_BLE_PAIRING_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest window radiant_ble_pairing_window_open() accepts. */
#define RADIANT_BLE_PAIRING_WINDOW_MAX_S 600u

/* ── Kernel-free ──────────────────────────────────────────────────────────── */

/*
 * Whether a pairing request at `now_ms` is inside a window that was opened at
 * `opened_ms` for `len_ms` (0 = closed). Wrap-safe for windows shorter than
 * 2^31 ms.
 */
bool radiant_ble_pairing_window_allows(uint32_t opened_ms, uint32_t len_ms,
				       uint32_t now_ms);

/* ── Zephyr glue ──────────────────────────────────────────────────────────── */

/* Register the auth callbacks. Call once, before advertising. */
int radiant_ble_pairing_init(void);

/* Open (or re-open, restarting the clock) a window of 1..600 s. Returns 0 or
 * -EINVAL. */
int radiant_ble_pairing_window_open(uint32_t seconds);

void radiant_ble_pairing_window_close(void);

bool radiant_ble_pairing_window_is_open(void);

/* Delete every bond on the default identity. Returns bt_unpair()'s result. */
int radiant_ble_bonds_forget_all(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_BLE_PAIRING_H_ */
