/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ant_radio_radiant_priv.h - what ant_radio_radiant.c shares with the other
 * files of the same adapter (ant_sec_host.c) and with its test image.
 *
 * Private to apps/common and radiant/tests/api: nothing else may take
 * api_lock, and nothing in radiant may name it (radiant is freestanding; the
 * lock reaches it only through radiant_sec_port_lock_check()).
 */

#ifndef ANT_RADIO_RADIANT_PRIV_H_
#define ANT_RADIO_RADIANT_PRIV_H_

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>

/*
 * The one lock every thread-context antr_* entry point holds. It serialises
 * the scheduler, the channel tables and, since radiant_sec is not re-entrant,
 * every radiant_sec call (the software AES has one shared key schedule). The
 * 0xF1-0xF5 host commands in ant_sec_host.c take it for the whole call; the
 * pairing exchange holds it across an X25519 operation.
 */
extern struct k_mutex api_lock;

#if defined(CONFIG_RADIANT_SEC_LOCK_CHECK)
/* True when the calling thread holds api_lock. */
bool antr_api_lock_held(void);

/* radiant_sec entry points reached without api_lock since boot. In a build
 * with CONFIG_ASSERT each one also asserts; the counter lets a test that
 * cannot take the assertion see the same fact. */
extern uint32_t antr_api_lock_violations;
#endif

#if defined(CONFIG_RADIANT_SEC_HOST_MESSAGES)
/* True when `ch` is a master with host acked data or a burst block in
 * flight; 0xF2 refuses to key such a channel. Caller holds api_lock. */
bool antr_master_xfer_busy(uint8_t ch);

/* 0xF1: radiant_sec_configure(), and on a master whose policy changes, the
 * staged slot and ack payload taken back before it returns; refused with
 * RADIANT_SEC_ESTATE (nothing changed) while that master's transfer engine is
 * busy. RADIANT_SEC_* codes. Caller holds api_lock. Review 2026-11-17 item 2. */
int antr_sec_policy_set(uint8_t ch, uint8_t switches, uint8_t w,
			uint8_t page_lo, uint8_t page_hi);

/* 0xF2: radiant_sec_set_key(), and on a secured master whose root really
 * changes, the staged slot and ack payload (old-key ciphertext) taken back
 * before it returns. RADIANT_SEC_* codes. Caller holds api_lock and has made
 * the busy check. Review 2026-11-20 item C. */
int antr_sec_key_replace(uint8_t ch, const uint8_t *root, size_t bits,
			 uint16_t base_devnum);
#endif

#if defined(CONFIG_ZTEST)
/* Test seams, ztest images only (review 2026-11-14, item 3). The first is
 * weak, for a test to override; the second is the acknowledgement payload
 * path, 8 bytes into `out`, false when the channel would not acknowledge. */
void antr_test_master_tx_seam(uint8_t ch);
bool antr_test_ack_payload(uint8_t ch, uint8_t out[8]);
/* Weak: after the 0xF1/0xF2 take-back, before the pump that follows it. */
void antr_test_take_back_seam(uint8_t ch);
/* The channel's ID list as stored: entry 0 (the 4 bytes 0x59 wrote, as a
 * little-endian word), the size and the exclude sense. */
void antr_test_id_list_get(uint8_t ch, uint32_t *entry0, uint8_t *size,
			   bool *exclude);
#endif

#endif /* ANT_RADIO_RADIANT_PRIV_H_ */
