/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Every thread-context radiant_sec entry point calls the port's lock check
 * (radiant_sec_port_lock_check()) before touching shared state. radiant is
 * freestanding, so this suite plays the port: it overrides the hook to record
 * which entry point announced itself. apps/common's real override asserts that
 * api_lock is held, and radiant/tests/api proves it is for the 0xF1-0xF5
 * commands; what only this suite can prove is that no entry point forgot to
 * ask. A new entry point that forgets fails nothing else.
 *
 * Deliberately NOT in the list: radiant_sec_rx_ingest() (ISR context, no lock
 * is possible), radiant_sec_channel_is_secured() and radiant_sec_last_verdict()
 * (single-word reads), and radiant_sec_tx_transform(), which the port calls
 * from the scheduler's terminal callback without api_lock (see the comment in
 * radiant_sec.c). See CONFIG_RADIANT_SEC_LOCK_CHECK.
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant/radiant_sec.h>

#if defined(CONFIG_RADIANT_SEC_LOCK_CHECK)

static const char *last_who;
static unsigned    calls;

void radiant_sec_port_lock_check(const char *who)
{
	last_who = who;
	calls++;
}

static void before(void *f)
{
	ARG_UNUSED(f);
	last_who = NULL;
	calls = 0u;
}

#define EXPECT_CHECKED(name, call)                                            \
	do {                                                                  \
		last_who = NULL;                                              \
		(void)(call);                                                 \
		zassert_not_null(last_who, #name " never called the lock check"); \
		zassert_equal(0, strcmp(last_who, #name),                     \
			      #name " announced itself as %s", last_who);     \
	} while (0)

ZTEST(sec_lock_hook, test_every_entry_point_asks_the_port_for_its_lock)
{
	uint8_t                  key[16] = {0};
	struct radiant_sec_stats stats;
	struct radiant_sec_state state;

	EXPECT_CHECKED(radiant_sec_set_key,
		       radiant_sec_set_key(0u, key, 128u, 0x1234u));
	EXPECT_CHECKED(radiant_sec_configure,
		       radiant_sec_configure(0u, 0u, 8u, 1u, 15u));
	EXPECT_CHECKED(radiant_sec_set_epoch,
		       radiant_sec_set_epoch(0u, 1u, 0u, 8182u));
	EXPECT_CHECKED(radiant_sec_set_devnum,
		       radiant_sec_set_devnum(0u, 0x1234u));
	EXPECT_CHECKED(radiant_sec_try_devnum,
		       radiant_sec_try_devnum(0u, 0x1234u));
	EXPECT_CHECKED(radiant_sec_pump, radiant_sec_pump());
	EXPECT_CHECKED(radiant_sec_get_stats,
		       radiant_sec_get_stats(0u, &stats));
	EXPECT_CHECKED(radiant_sec_get_state,
		       radiant_sec_get_state(0u, &state));
	EXPECT_CHECKED(radiant_sec_stat_enrolment,
		       radiant_sec_stat_enrolment(0u));
	EXPECT_CHECKED(radiant_sec_channel_release,
		       radiant_sec_channel_release(0u));
	EXPECT_CHECKED(radiant_sec_reset, radiant_sec_reset());
}

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
ZTEST(sec_lock_hook, test_every_pairing_entry_point_asks_the_port_for_its_lock)
{
	uint8_t  scalar[RADIANT_SEC_X25519_BYTES] = {1};
	uint8_t  pub[RADIANT_SEC_X25519_BYTES];
	uint32_t fp;

	EXPECT_CHECKED(radiant_sec_pair_enter, radiant_sec_pair_enter(0u, 5u));
	EXPECT_CHECKED(radiant_sec_pair_set_scalar,
		       radiant_sec_pair_set_scalar(0u, scalar));
	EXPECT_CHECKED(radiant_sec_pair_local_pubkey,
		       radiant_sec_pair_local_pubkey(0u, pub));
	EXPECT_CHECKED(radiant_sec_pair_peer,
		       radiant_sec_pair_peer(0u, pub, &fp));
	EXPECT_CHECKED(radiant_sec_pair_fingerprint,
		       radiant_sec_pair_fingerprint(0u, &fp));
	EXPECT_CHECKED(radiant_sec_pair_leave, radiant_sec_pair_leave(0u));
	EXPECT_CHECKED(radiant_sec_pair_reset, radiant_sec_pair_reset());
}
#endif

ZTEST_SUITE(sec_lock_hook, NULL, NULL, before, NULL, NULL);

#endif /* CONFIG_RADIANT_SEC_LOCK_CHECK */
