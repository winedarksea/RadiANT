/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Pairing, driven from both ends inside one process (channel 0 sensor, channel
 * 1 receiver, as in test_sec.c). A man in the middle is just a third scalar,
 * and the assertion is that the two fingerprints DIFFER - the entire security
 * argument for showing them to a human.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant/radiant_sec.h>
#include <radiant/radiant_channel.h>

#include "fake_radio.h"

#define A_CH 0u
#define B_CH 1u
#define M_CH 2u   /* the man in the middle */

#define A_DEVNUM 0x3A17u
#define B_DEVNUM 0x51C4u
#define M_DEVNUM 0x7788u

/* Three private scalars, fixed rather than random for a stated expectation. */
static const uint8_t scalar_a[32] = {
	0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
	0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
	0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
	0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
};
static const uint8_t scalar_b[32] = {
	0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b,
	0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
	0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
	0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb,
};
static const uint8_t scalar_m[32] = {
	0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
	0xfe, 0xdc, 0xba, 0x98, 0x76, 0x54, 0x32, 0x10,
	0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
	0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0,
};

static void pair_before(void *f)
{
	ARG_UNUSED(f);
	radiant_sec_pair_reset();
	radiant_sec_reset();

	/* radiant_sec_pair_peer() installs against the channel's own device
	 * number, so the channel module has to be live even though nothing here
	 * touches the radio. */
	fake_radio_reset();
	radiant_channel_init();
}

ZTEST_SUITE(sec_pair, NULL, NULL, pair_before, NULL, NULL);

/* radiant_sec_pair_peer() installs the key against the channel's own device
 * number, so a channel needs an ID before it can finish an exchange. */
static void give_id(uint8_t ch, uint16_t devnum)
{
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_assign(ch, 0x10u, 0u, 0u));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(ch, devnum, 0x0Bu, 0x05u));
}

ZTEST(sec_pair, test_two_ends_reach_the_same_fingerprint)
{
	uint8_t  pub_a[32];
	uint8_t  pub_b[32];
	uint32_t fp_a = 0;
	uint32_t fp_b = 0;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);

	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(A_CH, 60u));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(B_CH, 60u));

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(A_CH, scalar_a));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(B_CH, scalar_b));

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_local_pubkey(A_CH, pub_a));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_local_pubkey(B_CH, pub_b));

	/* The public keys cross on the air. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_peer(A_CH, pub_b, &fp_a));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_peer(B_CH, pub_a, &fp_b));

	/* The fingerprint is order-independent: both ends bind the two public
	 * keys ordered by value, not role, since master/slave don't agree on
	 * who initiated. */
	zassert_equal(fp_a, fp_b, "the two ends computed different "
		      "fingerprints: %u vs %u", fp_a, fp_b);
	zassert_true(fp_a < 1000000u, "the fingerprint is not six digits");
}

ZTEST(sec_pair, test_a_man_in_the_middle_cannot_match_both_fingerprints)
{
	uint8_t  pub_a[32];
	uint8_t  pub_b[32];
	uint8_t  pub_m[32];
	uint32_t fp_a = 0;
	uint32_t fp_b = 0;
	uint32_t fp_ma = 0;
	uint32_t fp_mb = 0;

	/* Section 7.4's argument, as an assertion: M sits between A and B,
	 * completing one exchange with each. Both halves succeed - the
	 * cryptography can't tell - but M cannot make A's fingerprint equal B's,
	 * since they're functions of two different shared secrets. The attack is
	 * undetectable to the protocol, detectable to a human comparing devices
	 * (the same bargain BLE numeric comparison makes). */
	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	give_id(M_CH, M_DEVNUM);

	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(A_CH, 60u));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(B_CH, 60u));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(M_CH, 60u));

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(A_CH, scalar_a));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(B_CH, scalar_b));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(M_CH, scalar_m));

	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_local_pubkey(A_CH, pub_a));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_local_pubkey(B_CH, pub_b));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_local_pubkey(M_CH, pub_m));

	/* A talks to M believing it is B. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_peer(A_CH, pub_m, &fp_a));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_peer(M_CH, pub_a, &fp_ma));
	zassert_equal(fp_a, fp_ma, "A and M did not agree with each other");

	/* M then talks to B believing... whatever M wants B to believe. A
	 * fresh trial is needed because M's scalar was spent above. */
	radiant_sec_pair_leave(M_CH);
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(M_CH, 60u));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(M_CH, scalar_m));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_peer(B_CH, pub_m, &fp_b));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_peer(M_CH, pub_b, &fp_mb));
	zassert_equal(fp_b, fp_mb, "B and M did not agree with each other");

	/* Both halves succeeded, and the two ends the user is comparing do not
	 * match. */
	zassert_not_equal(fp_a, fp_b,
			  "A and B computed the SAME fingerprint through a "
			  "man in the middle; the comparison would show "
			  "nothing and the whole mitigation is void");
}

ZTEST(sec_pair, test_a_small_order_peer_key_is_refused)
{
	uint8_t  pub_a[32];
	uint8_t  zero_point[32] = { 0 };
	uint8_t  one_point[32] = { 1 };
	uint32_t fp = 0;

	/* u=0 and u=1 make the shared secret all zeros regardless of scalar.
	 * Refused rather than accepted-with-a-weak-key, since this becomes a
	 * root key and accepting it lets an injector fix a known group key. */
	give_id(A_CH, A_DEVNUM);
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(A_CH, 60u));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(A_CH, scalar_a));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_local_pubkey(A_CH, pub_a));

	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer(A_CH, zero_point, &fp));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer(A_CH, one_point, &fp));

	/* And nothing was installed by the attempt. */
	zassert_false(radiant_sec_channel_is_secured(A_CH));
}

ZTEST(sec_pair, test_pairing_installs_a_key_both_ends_can_use)
{
	uint8_t  pub_a[32];
	uint8_t  pub_b[32];
	uint32_t fp = 0;

	/* The point of the exchange: both channels now hold a root key.
	 * Asserted through the public surface since the key can't be read back
	 * by design. */
	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(A_CH, 60u));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(B_CH, 60u));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(A_CH, scalar_a));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(B_CH, scalar_b));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_local_pubkey(A_CH, pub_a));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_local_pubkey(B_CH, pub_b));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_peer(A_CH, pub_b, &fp));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_peer(B_CH, pub_a, &fp));

	/* A key is installed, so configure and epoch now succeed - which they
	 * would refuse with RADIANT_SEC_ENOKEY on an unpaired channel. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_configure(A_CH, RADIANT_SEC_SW_AUTH, 2u,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(A_CH, 1u, 0u, 8182u));
	zassert_true(radiant_sec_channel_is_secured(A_CH));
}

ZTEST(sec_pair, test_nothing_works_before_pairing_mode_is_entered)
{
	uint8_t  pub[32];
	uint32_t fp = 0;

	give_id(A_CH, A_DEVNUM);

	/* No enter, so there is no context and no scalar to spend. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_scalar(A_CH, scalar_a));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_local_pubkey(A_CH, pub));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer(A_CH, scalar_b, &fp));
	zassert_false(radiant_sec_pair_is_open(A_CH));
}

ZTEST(sec_pair, test_an_exchange_needs_a_scalar_first)
{
	uint32_t fp = 0;

	give_id(A_CH, A_DEVNUM);
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(A_CH, 60u));

	/* Open, but with no local key there is nothing to complete. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer(A_CH, scalar_b, &fp));
}

ZTEST(sec_pair, test_leaving_wipes_the_state)
{
	uint8_t pub[32];

	give_id(A_CH, A_DEVNUM);
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(A_CH, 60u));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(A_CH, scalar_a));
	zassert_true(radiant_sec_pair_is_open(A_CH));

	radiant_sec_pair_leave(A_CH);
	zassert_false(radiant_sec_pair_is_open(A_CH));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_local_pubkey(A_CH, pub));
}

ZTEST(sec_pair, test_loss_of_tracking_releases_the_exchange)
{
	uint8_t pub[32];

	/* D8.1: a channel that loses its peer mid-pairing gets no completion or
	 * failure event, so RX_FAIL_GO_TO_SEARCH is the only transition that can
	 * free the state. Kept live now (cheap) rather than added later, when
	 * forgetting it would deadlock negotiation permanently and silently. */
	give_id(A_CH, A_DEVNUM);
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(A_CH, 60u));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(A_CH, scalar_a));
	zassert_true(radiant_sec_pair_is_open(A_CH));

	radiant_sec_pair_on_search(A_CH);

	zassert_false(radiant_sec_pair_is_open(A_CH),
		      "pairing survived loss of tracking; there is no other "
		      "event that would ever free it");
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_local_pubkey(A_CH, pub));
}

ZTEST(sec_pair, test_a_zero_timeout_means_the_default_not_forever)
{
	/* A node in pairing mode accepts a key from whoever asks; 0 meaning
	 * "no timeout" would turn one forgotten host command into a permanently
	 * open node, so 0 means the 60-second default instead. */
	give_id(A_CH, A_DEVNUM);
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(A_CH, 0u));
	zassert_true(radiant_sec_pair_is_open(A_CH),
		     "a zero timeout closed the window immediately");
}

ZTEST(sec_pair, test_the_fingerprint_is_readable_only_after_an_exchange)
{
	uint8_t  pub_a[32];
	uint8_t  pub_b[32];
	uint32_t fp = 0;
	uint32_t read_back = 0;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(A_CH, 60u));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(B_CH, 60u));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(A_CH, scalar_a));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_set_scalar(B_CH, scalar_b));

	/* Nothing to attest to yet. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_fingerprint(A_CH, &read_back));

	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_local_pubkey(A_CH, pub_a));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_local_pubkey(B_CH, pub_b));
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_peer(A_CH, pub_b, &fp));

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_pair_fingerprint(A_CH, &read_back));
	zassert_equal(fp, read_back);
}

#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */
