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
 * (single-word reads). See CONFIG_RADIANT_SEC_LOCK_CHECK.
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant/radiant_channel.h>
#include <radiant/radiant_sec.h>

#include "fake_radio.h"

#if defined(CONFIG_RADIANT_SEC_LOCK_CHECK)

static const char *last_who;
static unsigned    calls;

/* Every announcement during one call, since an entry point may call others
 * (radiant_sec_pair_peer() installs a key). */
#define SEEN_MAX 12u
static const char *seen[SEEN_MAX];
static unsigned    nseen;

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
/* When set, the hook plays the radio callback: the first time
 * radiant_sec_set_key() announces itself (inside radiant_sec_pair_peer(), after
 * its X25519 and before it is done with its own state) it reports loss of
 * tracking on this channel. -1 = off. */
static int lose_peer_on;
#endif

void radiant_sec_port_lock_check(const char *who)
{
	last_who = who;
	calls++;
	if (nseen < SEEN_MAX) {
		seen[nseen++] = who;
	}
#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
	if (lose_peer_on >= 0 && strcmp(who, "radiant_sec_set_key") == 0) {
		uint8_t ch = (uint8_t)lose_peer_on;

		lose_peer_on = -1;
		radiant_sec_pair_on_search(ch);
	}
#endif
}

static void before(void *f)
{
	ARG_UNUSED(f);
#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
	lose_peer_on = -1;
#endif
	last_who = NULL;
	calls = 0u;
}

static bool was_seen(const char *name)
{
	for (unsigned i = 0u; i < nseen; i++) {
		if (strcmp(seen[i], name) == 0) {
			return true;
		}
	}
	return false;
}

#define EXPECT_CHECKED(name, call)                                        \
	do {                                                              \
		last_who = NULL;                                          \
		nseen = 0u;                                               \
		(void)(call);                                             \
		zassert_true(was_seen(#name),                             \
			     #name " never called the lock check");       \
	} while (0)

ZTEST(sec_lock_hook, test_every_entry_point_asks_the_port_for_its_lock)
{
	uint8_t                  key[16] = {0};
	uint8_t                  buf[8] = {0};
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
	/* Only an ARMED channel's transform is held to the lock (an unsecured
	 * channel is posted from the scheduler callback and touches nothing). */
	buf[0] = 0x05u;
	zassert_equal(0, radiant_sec_set_key(0u, key, 128u, 0x1234u));
	zassert_equal(0, radiant_sec_configure(0u, RADIANT_SEC_SW_CONF, 2u, 1u,
					       15u));
	zassert_equal(0, radiant_sec_set_epoch(0u, 1u, 0u, 8182u));
	EXPECT_CHECKED(radiant_sec_tx_transform,
		       radiant_sec_tx_transform(0u, buf, 8u, 0u));
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
	EXPECT_CHECKED(radiant_sec_pair_service, radiant_sec_pair_service());
	EXPECT_CHECKED(radiant_sec_pair_reset, radiant_sec_pair_reset());
}
#endif

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
/*
 * TODO security item 8 follow-up: loss of tracking arrives from the radio
 * callback. It used to free the pairing context on the spot, so a peer lost
 * during the 375 ms X25519 zeroed local_pub and the scalar under
 * radiant_sec_pair_peer(), which then derived and installed a root from zeros
 * and reported fingerprint 0. Now the callback only marks the channel and the
 * free happens under the lock (radiant_sec_pair_service()).
 */
ZTEST(sec_lock_hook, test_a_peer_lost_inside_the_exchange_does_not_corrupt_it)
{
	static const uint8_t sa[32] = {
		0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
		0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
		0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
		0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
	};
	static const uint8_t sb[32] = {
		0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b,
		0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
		0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
		0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb,
	};
	uint8_t  pub_a[32];
	uint8_t  pub_b[32];
	uint32_t fp_a = 0;
	uint32_t fp_b = 0;

	radiant_sec_pair_reset();
	radiant_sec_reset();
	fake_radio_reset();
	radiant_channel_init();
	zassert_equal(RADIANT_CH_OK, radiant_channel_assign(0u, 0x10u, 0u, 0u));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(0u, 0x3A17u, 0x0Bu, 0x05u));
	zassert_equal(RADIANT_CH_OK, radiant_channel_assign(1u, 0x10u, 0u, 0u));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(1u, 0x51C4u, 0x0Bu, 0x05u));
	zassert_equal(0, radiant_sec_pair_enter(0u, 60u));
	zassert_equal(0, radiant_sec_pair_enter(1u, 60u));
	zassert_equal(0, radiant_sec_pair_set_scalar(0u, sa));
	zassert_equal(0, radiant_sec_pair_set_scalar(1u, sb));
	zassert_equal(0, radiant_sec_pair_local_pubkey(0u, pub_a));
	zassert_equal(0, radiant_sec_pair_local_pubkey(1u, pub_b));

	/* B finishes normally; A's peer is lost in the middle of its own. */
	zassert_equal(0, radiant_sec_pair_peer(1u, pub_a, &fp_b));
	lose_peer_on = 0;
	zassert_equal(0, radiant_sec_pair_peer(0u, pub_b, &fp_a));
	zassert_equal(-1, lose_peer_on, "the loss was never injected");
	zassert_equal(fp_b, fp_a,
		      "a peer lost inside the exchange corrupted it: fingerprint "
		      "%u against %u", fp_a, fp_b);

	/* The release itself is not lost, only moved under the lock. */
	zassert_true(radiant_sec_pair_is_open(0u), "released in the callback");
	radiant_sec_pair_service();
	zassert_false(radiant_sec_pair_is_open(0u),
		      "the lost peer's pairing state was never released");
	zassert_true(radiant_sec_pair_is_open(1u), "released the wrong channel");

	radiant_sec_pair_reset();
	radiant_sec_reset();
}
#endif

ZTEST_SUITE(sec_lock_hook, NULL, NULL, before, NULL, NULL);

#endif /* CONFIG_RADIANT_SEC_LOCK_CHECK */
