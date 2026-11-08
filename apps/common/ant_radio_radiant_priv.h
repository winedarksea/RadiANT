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

#endif /* ANT_RADIO_RADIANT_PRIV_H_ */
