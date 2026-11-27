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
 *     CONFIG_RADIANT_BLE_PAIRING_WINDOW_AT_BOOT_S opens one for N seconds
 *     after power-on, the usual product pattern for "power-cycle to pair".
 *   - Outside the window, a pairing request is refused with "pairing not
 *     allowed" (BT_SMP_APP_PAIRING_ACCEPT); bonded peers still connect and
 *     encrypt with their stored keys, which involves no pairing.
 *   - BOND STORAGE FULL (CONFIG_BT_MAX_PAIRED). Zephyr's own answer,
 *     BT_KEYS_OVERWRITE_OLDEST, is NOT used and must not be: it evicts the
 *     oldest bond inside smp_pairing_req() BEFORE pairing_accept is asked,
 *     so with the window closed every refused stranger would still erase one
 *     bond. Instead this layer keeps its own least-recently-used order (a
 *     settings record, "rble/lru", touched each time a peer reaches an
 *     encrypted link) and frees ONE slot itself, only while the window is
 *     open: when the window opens and the store is full, and again when a
 *     new (non-bonded) peer connects inside an open window. A connected peer
 *     is never evicted. Without a free slot SMP refuses the pairing before
 *     pairing_accept runs, which is why the room is made first.
 *   - radiant_ble_bonds_forget_all() for service.
 *
 * The layer registers the auth callbacks itself at boot (SYS_INIT); an
 * application using this option does not register its own. Pairing stays
 * Just Works, as before - the window, not a passkey, is what gates it.
 */
#ifndef RADIANT_BLE_PAIRING_H_
#define RADIANT_BLE_PAIRING_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Longest window radiant_ble_pairing_window_open() accepts. */
#define RADIANT_BLE_PAIRING_WINDOW_MAX_S 600u

/* ── Kernel-free ──────────────────────────────────────────────────────────── */

/*
 * Whether a pairing request at `now_ms` is inside a window that was opened at
 * `opened_ms` for `len_ms` (0 = closed). 64-bit uptime: a 32-bit millisecond
 * clock wraps after 49.7 days and would re-open an expired window.
 */
bool radiant_ble_pairing_window_allows(uint64_t opened_ms, uint32_t len_ms,
				       uint64_t now_ms);

/* A bonded peer's identity address: type byte, then the six address bytes
 * (bt_addr_le_t's layout, copied byte-wise so this stays kernel-free). */
#define RADIANT_BLE_ADDR_BYTES 7u

struct radiant_ble_lru_entry {
	uint8_t  addr[RADIANT_BLE_ADDR_BYTES];
	uint32_t seq; /* 0 = empty slot; higher = used more recently */
};

/* One encoded entry in the "rble/lru" record: 7 address bytes, seq LE32. */
#define RADIANT_BLE_LRU_ENTRY_BYTES 11u
/* The record's first byte. A new key, so v1 by definition. */
#define RADIANT_BLE_LRU_FORMAT_V1 1u

/*
 * Record that `addr` just used its bond. Updates its entry, or takes an
 * empty slot, or replaces the least recent entry. `next_seq` is the counter
 * (the highest seq in the table); it is advanced.
 */
void radiant_ble_lru_touch(struct radiant_ble_lru_entry *t, size_t n,
			   const uint8_t addr[RADIANT_BLE_ADDR_BYTES],
			   uint32_t *next_seq);

/* Forget `addr` (its bond was deleted). */
void radiant_ble_lru_forget(struct radiant_ble_lru_entry *t, size_t n,
			    const uint8_t addr[RADIANT_BLE_ADDR_BYTES]);

/*
 * Which bond to evict to make room: among `bonds[0..nb)` whose `connected`
 * flag is false, the one whose LRU seq is lowest. A bond with no LRU entry
 * counts as least recent of all (seq 0). Ties go to the lower index. Returns
 * the index into `bonds`, or -1 when every bond is connected (or nb is 0).
 */
int radiant_ble_lru_pick_victim(const struct radiant_ble_lru_entry *t, size_t n,
				const uint8_t (*bonds)[RADIANT_BLE_ADDR_BYTES],
				const bool *connected, size_t nb);

/* The record codec. encode returns the bytes written (1 + 11 * used
 * entries) or -1 if `cap` is too small; decode fills `t` (zeroing the rest)
 * and returns the entry count, or -1 for a malformed record. */
int radiant_ble_lru_encode(const struct radiant_ble_lru_entry *t, size_t n,
			   uint8_t *out, size_t cap);
int radiant_ble_lru_decode(const uint8_t *in, size_t len,
			   struct radiant_ble_lru_entry *t, size_t n,
			   uint32_t *max_seq);

/* ── Zephyr glue ──────────────────────────────────────────────────────────── */

/* Open (or re-open, restarting the clock) a window of 1..600 s. Returns 0 or
 * -EINVAL. Frees one bond slot if the store is full (see above). */
int radiant_ble_pairing_window_open(uint32_t seconds);

void radiant_ble_pairing_window_close(void);

bool radiant_ble_pairing_window_is_open(void);

/* Delete every bond on the default identity. Returns bt_unpair()'s result. */
int radiant_ble_bonds_forget_all(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_BLE_PAIRING_H_ */
