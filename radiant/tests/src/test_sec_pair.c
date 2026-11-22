/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Pairing, driven from both ends inside one process (channel 0 the initiator
 * A, channel 1 the responder B, as in test_sec.c). A man in the middle is a
 * third scalar on two more channels, and the assertions are the security
 * argument for showing six digits to a human: an honest pair matches, and a
 * man in the middle - even one that grinds - does not get to choose.
 *
 * The commitment and fingerprint literals are shared with
 * tools/test_radiant_crypto.py (TestPairCommitReveal), so the C asserts values
 * an independent implementation produced.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant/radiant_sec.h>
#include <radiant/radiant_sec_compat.h>
#include <radiant/radiant_channel.h>
#include <radiant/radiant_radio_hal.h>

#include "fake_radio.h"

#define A_CH  0u
#define B_CH  1u
#define M1_CH 2u   /* the man in the middle, facing A */
#define M2_CH 3u   /* the man in the middle, facing B */

#define A_DEVNUM 0x3A17u
#define B_DEVNUM 0x51C4u
#define M_DEVNUM 0x7788u

#define INIT RADIANT_SEC_PAIR_INITIATOR
#define RESP RADIANT_SEC_PAIR_RESPONDER

/* RFC 7748 section 6.1's Alice and Bob, so the public keys are published
 * values the Python side can use without an X25519 of its own. */
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

/* Na and Nb, as in TestPairCommitReveal. */
static const uint8_t nonce_a[16] = {
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
static const uint8_t nonce_b[16] = {
	0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
};

/* Cb = CMAC_Nb(PKb || PKa || 0x00), and g2(PKa, PKb, Na, Nb), from
 * tools/radiant_crypto.py. */
static const uint8_t VEC_CB[16] = {
	0x87, 0xec, 0x01, 0xf6, 0x17, 0xe1, 0xf4, 0xd9,
	0x83, 0xc7, 0xfd, 0x3e, 0x6a, 0xd6, 0x85, 0x9b,
};
#define VEC_FP             362706u
/* Over-air enrolment's path for the same two keys: no nonces, keys by
 * value. */
#define VEC_FP_UNCOMMITTED 172078u

/* The roots tools/radiant_crypto.py derives for the same exchange (pair_root
 * and pair_uncommitted). Not readable out of the C, so assert_root_is() keys a
 * reference channel with each and compares what the two channels transmit. */
static const uint8_t VEC_ROOT[16] = {
	0x9a, 0x69, 0x57, 0x17, 0x7a, 0x6a, 0xd2, 0x42,
	0xf7, 0xc3, 0xc1, 0x44, 0xa0, 0xbc, 0x9c, 0x08,
};
static const uint8_t VEC_ROOT_UNCOMMITTED[16] = {
	0x95, 0x46, 0xbe, 0xb5, 0xea, 0xd0, 0x02, 0x9c,
	0x86, 0x91, 0xd9, 0x45, 0x34, 0x73, 0x88, 0x8b,
};

#define REF_CH 6u   /* keyed directly with a Python-derived root */

/* The wrapped root out of an enrolment whose output the test ignores. */
static uint8_t w[RADIANT_SEC_PAIR_WRAPPED_BYTES];

static void pair_before(void *f)
{
	ARG_UNUSED(f);
	radiant_sec_pair_reset();
	radiant_sec_reset();
	/* A sensor delivers a compat root in preference to a secured one, so a
	 * compat context another suite left on these channels would change
	 * what an enrolment here sends. */
	radiant_sec_compat_reset();

	/* A pairing installs against the channel's own device number, so the
	 * channel module has to be live even though nothing here touches the
	 * radio. */
	fake_radio_reset();
	radiant_channel_init();
}

ZTEST_SUITE(sec_pair, NULL, NULL, pair_before, NULL, NULL);

/* A pairing installs the key against the channel's own device number, so a
 * channel needs an ID before it can finish. */
static void give_id(uint8_t ch, uint16_t devnum)
{
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_assign(ch, 0x10u, 0u, 0u));
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(ch, devnum, 0x0Bu, 0x05u));
}

/* One end up to its public key. */
static void key_up(uint8_t ch, const uint8_t *scalar, uint8_t *pub)
{
	zassert_ok(radiant_sec_pair_enter(ch, 60u));
	zassert_ok(radiant_sec_pair_set_scalar(ch, scalar));
	zassert_ok(radiant_sec_pair_local_pubkey(ch, pub));
}

/* Two ends up and their keys crossed: the DH half on both sides. */
static void keys_cross(uint8_t a, const uint8_t *sa, uint8_t b,
		       const uint8_t *sb)
{
	uint8_t pub_a[32];
	uint8_t pub_b[32];

	key_up(a, sa, pub_a);
	key_up(b, sb, pub_b);
	zassert_ok(radiant_sec_pair_peer(a, pub_b));
	zassert_ok(radiant_sec_pair_peer(b, pub_a));
}

/* The honest commit/reveal, in the order docs/ant-serial-protocol.md gives a
 * host: B commits, A takes the commitment before revealing Na, B finishes on
 * Na before revealing Nb, A checks Nb against the commitment. */
static void commit_reveal(uint8_t a, const uint8_t *na, uint8_t b,
			  const uint8_t *nb, uint32_t *fp_a, uint32_t *fp_b)
{
	uint8_t cb[16];

	zassert_ok(radiant_sec_pair_set_nonce(b, RESP, nb, cb));
	zassert_ok(radiant_sec_pair_set_nonce(a, INIT, na, NULL));
	zassert_ok(radiant_sec_pair_peer_commit(a, cb));
	zassert_ok(radiant_sec_pair_peer_nonce(b, na, fp_b));
	zassert_ok(radiant_sec_pair_peer_nonce(a, nb, fp_a));
}

/* No key on the channel: configure refuses with ENOKEY. */
static bool has_key(uint8_t ch)
{
	return radiant_sec_configure(ch, RADIANT_SEC_SW_AUTH, 2u,
				     RADIANT_SEC_PAGE_LO_DEFAULT,
				     RADIANT_SEC_PAGE_HI_DEFAULT) !=
	       RADIANT_SEC_ENOKEY;
}

/*
 * The paired channel holds exactly `root`: a reference channel keyed with it
 * directly, under the same device number, epoch and slot, transmits the same
 * ciphertext for the same page. X_CONF runs the whole hierarchy (KDF from the
 * root, then CTR), so one differing root byte changes every output byte.
 */
static void assert_root_is(uint8_t ch, uint16_t devnum, const uint8_t *root)
{
	uint8_t  plain[8] = { RADIANT_SEC_PAGE_LO_DEFAULT, 0u, 0x11u, 0x22u,
			      0x33u, 0x44u, 0x55u, 0x66u };
	uint8_t  got[8];
	uint8_t  want[8];
	uint64_t t;

	give_id(REF_CH, devnum);
	zassert_ok(radiant_sec_set_key(REF_CH, root, 128u, devnum));
	zassert_ok(radiant_sec_set_devnum(REF_CH, devnum));
	zassert_ok(radiant_sec_configure(ch, RADIANT_SEC_SW_CONF, 2u,
					 RADIANT_SEC_PAGE_LO_DEFAULT,
					 RADIANT_SEC_PAGE_HI_DEFAULT));
	zassert_ok(radiant_sec_configure(REF_CH, RADIANT_SEC_SW_CONF, 2u,
					 RADIANT_SEC_PAGE_LO_DEFAULT,
					 RADIANT_SEC_PAGE_HI_DEFAULT));
	zassert_ok(radiant_sec_set_epoch(ch, 7u, 0u, 8182u));
	zassert_ok(radiant_sec_set_epoch(REF_CH, 7u, 0u, 8182u));
	t = (uint64_t)radiant_radio_now() + 3000000u;

	memcpy(got, plain, sizeof(got));
	memcpy(want, plain, sizeof(want));
	zassert_ok(radiant_sec_tx_transform(ch, got, 8u, t));
	zassert_ok(radiant_sec_tx_transform(REF_CH, want, 8u, t));
	zassert_false(memcmp(want, plain, sizeof(want)) == 0);
	zassert_mem_equal(want, got, sizeof(got));
	radiant_sec_channel_release(REF_CH);
	(void)radiant_channel_unassign(REF_CH);
}

static uint32_t enrolments(uint8_t ch)
{
	struct radiant_sec_stats s;

	radiant_sec_get_stats(ch, &s);
	return s.enrolments;
}

ZTEST(sec_pair, test_two_ends_reach_the_same_fingerprint)
{
	uint8_t  cb[16];
	uint32_t fp_a = 0;
	uint32_t fp_b = 1;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	keys_cross(A_CH, scalar_a, B_CH, scalar_b);

	/* Nothing is installed by the key exchange alone any more. */
	zassert_false(has_key(A_CH));
	zassert_false(has_key(B_CH));

	zassert_ok(radiant_sec_pair_set_nonce(B_CH, RESP, nonce_b, cb));
	zassert_mem_equal(VEC_CB, cb, sizeof(cb));
	zassert_ok(radiant_sec_pair_set_nonce(A_CH, INIT, nonce_a, NULL));
	zassert_ok(radiant_sec_pair_peer_commit(A_CH, cb));
	zassert_ok(radiant_sec_pair_peer_nonce(B_CH, nonce_a, &fp_b));
	zassert_ok(radiant_sec_pair_peer_nonce(A_CH, nonce_b, &fp_a));

	zassert_equal(fp_a, fp_b, "%u vs %u", fp_a, fp_b);
	zassert_equal(VEC_FP, fp_a, "%u", fp_a);
	zassert_true(has_key(A_CH) && has_key(B_CH));
	zassert_equal(1u, enrolments(A_CH));
	zassert_equal(1u, enrolments(B_CH));

	/* And the key both installed is the one Python derives. */
	assert_root_is(A_CH, A_DEVNUM, VEC_ROOT);
	assert_root_is(B_CH, B_DEVNUM, VEC_ROOT);
}

ZTEST(sec_pair, test_a_nonce_that_does_not_open_the_commitment_is_refused)
{
	uint8_t  cb[16];
	uint8_t  other[16];
	uint32_t fp = 0u;
	uint32_t fp_b = 0u;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	keys_cross(A_CH, scalar_a, B_CH, scalar_b);

	zassert_ok(radiant_sec_pair_set_nonce(B_CH, RESP, nonce_b, cb));
	zassert_ok(radiant_sec_pair_set_nonce(A_CH, INIT, nonce_a, NULL));
	zassert_ok(radiant_sec_pair_peer_commit(A_CH, cb));
	zassert_ok(radiant_sec_pair_peer_nonce(B_CH, nonce_a, &fp_b));

	/* B (or whoever answers as B) reveals a nonce it did not commit to. */
	memcpy(other, nonce_b, sizeof(other));
	other[15] ^= 0x01u;
	zassert_equal(RADIANT_SEC_EBADMAC,
		      radiant_sec_pair_peer_nonce(A_CH, other, &fp));

	/* Nothing installed, nothing counted, and the exchange is gone: there
	 * is no second guess, not even with the right nonce. */
	zassert_false(has_key(A_CH));
	zassert_equal(0u, enrolments(A_CH));
	zassert_false(radiant_sec_pair_is_open(A_CH));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_fingerprint(A_CH, &fp));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_nonce(A_CH, nonce_b, &fp));
}

ZTEST(sec_pair, test_changing_either_nonce_changes_the_fingerprint)
{
	uint8_t  na2[16];
	uint8_t  nb2[16];
	uint32_t fp[3][2];

	memcpy(na2, nonce_a, sizeof(na2));
	na2[0] ^= 0x80u;
	memcpy(nb2, nonce_b, sizeof(nb2));
	nb2[7] ^= 0x04u;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);

	keys_cross(A_CH, scalar_a, B_CH, scalar_b);
	commit_reveal(A_CH, nonce_a, B_CH, nonce_b, &fp[0][0], &fp[0][1]);

	radiant_sec_pair_reset();
	keys_cross(A_CH, scalar_a, B_CH, scalar_b);
	commit_reveal(A_CH, na2, B_CH, nonce_b, &fp[1][0], &fp[1][1]);

	radiant_sec_pair_reset();
	keys_cross(A_CH, scalar_a, B_CH, scalar_b);
	commit_reveal(A_CH, nonce_a, B_CH, nb2, &fp[2][0], &fp[2][1]);

	for (int i = 0; i < 3; i++) {
		zassert_equal(fp[i][0], fp[i][1], "run %d", i);
	}
	zassert_not_equal(fp[0][0], fp[1][0]);
	zassert_not_equal(fp[0][0], fp[2][0]);
}

ZTEST(sec_pair, test_every_step_is_refused_out_of_order)
{
	uint8_t  cb[16];
	uint8_t  pub_a[32];
	uint8_t  pub_b[32];
	uint32_t fp = 0u;
	uint32_t fp_a = 0u;
	uint32_t fp_b = 1u;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	key_up(A_CH, scalar_a, pub_a);
	key_up(B_CH, scalar_b, pub_b);

	/* Before EXCHANGE: no nonce (the commitment covers the peer's key),
	 * no commitment, no reveal. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_nonce(B_CH, RESP, nonce_b, cb));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_commit(A_CH, cb));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_nonce(A_CH, nonce_b, &fp));

	zassert_ok(radiant_sec_pair_peer(A_CH, pub_b));
	zassert_ok(radiant_sec_pair_peer(B_CH, pub_a));

	/* A role that is neither, and a responder with nowhere to put Cb. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_nonce(B_CH, 2u, nonce_b, cb));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_nonce(B_CH, RESP, nonce_b, NULL));
	/* No commitment before this end's own nonce. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_commit(A_CH, cb));

	zassert_ok(radiant_sec_pair_set_nonce(B_CH, RESP, nonce_b, cb));
	zassert_ok(radiant_sec_pair_set_nonce(A_CH, INIT, nonce_a, NULL));

	/* One nonce per end: a second would be a second commitment. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_nonce(B_CH, RESP, nonce_a, cb));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_nonce(A_CH, INIT, nonce_b, NULL));
	/* A responder takes no commitment. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_commit(B_CH, cb));
	/* The initiator takes no reveal before the commitment: a commitment
	 * accepted after the reveal commits to nothing. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_nonce(A_CH, nonce_b, &fp));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_fingerprint(A_CH, &fp));
	zassert_false(has_key(A_CH));

	/* None of those refusals disturbed anything: the honest sequence
	 * still completes. */
	zassert_ok(radiant_sec_pair_peer_commit(A_CH, cb));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_commit(A_CH, cb));
	zassert_ok(radiant_sec_pair_peer_nonce(B_CH, nonce_a, &fp_b));
	zassert_ok(radiant_sec_pair_peer_nonce(A_CH, nonce_b, &fp_a));
	zassert_equal(fp_a, fp_b);

	/* And it is over: nothing re-runs on a finished exchange. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_nonce(A_CH, nonce_b, &fp));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_nonce(B_CH, nonce_a, &fp));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_nonce(A_CH, INIT, nonce_a, NULL));
	zassert_equal(1u, enrolments(A_CH));
}

/*
 * The grinding man in the middle (security review 2026-11-13, item 4).
 *
 * M sits between A and B with one key pair, facing A as the responder (M1)
 * and B as the initiator (M2). Under the old two-move protocol the
 * fingerprint was a function of the two public keys only, so M could grind
 * key pairs offline until A's six digits equalled B's - about 10^6 X25519,
 * seconds on a laptop, inside one 60 s window.
 *
 * With the commitment, M's best order is to finish with B first (so it
 * knows B's fingerprint), then commit to A and hope. It needs
 * g2(PKa, PKm, Na, Nm) to equal B's digits, and Na is unknown when Nm is
 * committed. The numbers below are real: GRIND_NM is the nonce a laptop
 * found in 31 s (3 792 251 CMACs, tools/radiant_crypto.py's g2 with the
 * keys and nonces of this test) that makes A show exactly B's digits once
 * Na is known. The test proves the nonce does match - and that A refuses it,
 * because M committed to something else before Na existed.
 */
ZTEST(sec_pair, test_a_man_in_the_middle_cannot_match_both_fingerprints)
{
	/* M's nonce towards B (as initiator), and the one it commits to A. */
	static const uint8_t nm_to_b[16] = {
		0x60, 0x61, 0x62, 0x63, 0x64, 0x65, 0x66, 0x67,
		0x68, 0x69, 0x6a, 0x6b, 0x6c, 0x6d, 0x6e, 0x6f,
	};
	static const uint8_t nm_to_a[16] = {
		0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57,
		0x58, 0x59, 0x5a, 0x5b, 0x5c, 0x5d, 0x5e, 0x5f,
	};
	/* Ground after seeing Na: g2(PKa, PKm, Na, GRIND_NM) == B's digits. */
	static const uint8_t grind_nm[16] = {
		0x7b, 0xdd, 0x39, 0x00, 0x00, 0x00, 0x00, 0x00,
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	uint8_t  cm[16];
	uint32_t fp_b = 0u;
	uint32_t fp_mb = 1u;
	uint32_t fp_a = 2u;
	uint32_t fp_ma = 3u;
	uint32_t fp = 0u;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	give_id(M1_CH, M_DEVNUM);
	give_id(M2_CH, M_DEVNUM);

	/* ── M and B, M as initiator: an honest-looking exchange ──────────── */
	keys_cross(M2_CH, scalar_m, B_CH, scalar_b);
	commit_reveal(M2_CH, nm_to_b, B_CH, nonce_b, &fp_mb, &fp_b);
	zassert_equal(fp_b, fp_mb);

	/* ── A and M, M as responder: committed before Na exists ──────────── */
	keys_cross(A_CH, scalar_a, M1_CH, scalar_m);
	zassert_ok(radiant_sec_pair_set_nonce(M1_CH, RESP, nm_to_a, cm));
	zassert_ok(radiant_sec_pair_set_nonce(A_CH, INIT, nonce_a, NULL));
	zassert_ok(radiant_sec_pair_peer_commit(A_CH, cm));
	/* A reveals Na; M learns it and grinds. */
	zassert_ok(radiant_sec_pair_peer_nonce(M1_CH, nonce_a, &fp_ma));

	/* The ground nonce really would have done it: committed up front (an
	 * attacker who could choose Nm after Na), A shows B's digits. Run on
	 * a fresh A-M exchange with the same keys and the same Na. */
	{
		uint32_t fp_a_ground = 0u;
		uint32_t fp_m_ground = 1u;

		give_id(4u, A_DEVNUM);
		give_id(5u, M_DEVNUM);
		keys_cross(4u, scalar_a, 5u, scalar_m);
		commit_reveal(4u, nonce_a, 5u, grind_nm, &fp_a_ground,
			      &fp_m_ground);
		zassert_equal(fp_b, fp_a_ground, "%u vs %u", fp_b, fp_a_ground);
	}

	/* But M committed to nm_to_a before Na existed, and A holds it to
	 * that: the ground nonce does not open the commitment. */
	zassert_equal(RADIANT_SEC_EBADMAC,
		      radiant_sec_pair_peer_nonce(A_CH, grind_nm, &fp));
	zassert_false(has_key(A_CH));
	zassert_false(radiant_sec_pair_is_open(A_CH));

	/* Revealing what it committed to is all M can do, and then the two
	 * screens disagree. */
	radiant_sec_pair_leave(A_CH);
	radiant_sec_pair_leave(M1_CH);
	keys_cross(A_CH, scalar_a, M1_CH, scalar_m);
	commit_reveal(A_CH, nonce_a, M1_CH, nm_to_a, &fp_a, &fp_ma);
	zassert_equal(fp_a, fp_ma);
	zassert_not_equal(fp_a, fp_b);
}

ZTEST(sec_pair, test_a_substituted_key_does_not_match)
{
	uint8_t  pub_a[32];
	uint8_t  pub_b[32];
	uint8_t  pub_m[32];
	uint8_t  cb[16];
	uint32_t fp_a = 0u;
	uint32_t fp_b = 1u;

	/* The plainest MITM: the key A receives is M's, while A and B run the
	 * rest honestly (M relays the nonces). Both finish, and the digits
	 * differ, because the public keys are in g2. */
	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	give_id(M1_CH, M_DEVNUM);
	key_up(A_CH, scalar_a, pub_a);
	key_up(B_CH, scalar_b, pub_b);
	key_up(M1_CH, scalar_m, pub_m);
	zassert_ok(radiant_sec_pair_peer(A_CH, pub_m));
	zassert_ok(radiant_sec_pair_peer(B_CH, pub_a));

	zassert_ok(radiant_sec_pair_set_nonce(B_CH, RESP, nonce_b, cb));
	zassert_ok(radiant_sec_pair_set_nonce(A_CH, INIT, nonce_a, NULL));
	/* B's commitment covers PKb, not PKm, so relaying it fails at A. */
	zassert_ok(radiant_sec_pair_peer_commit(A_CH, cb));
	zassert_ok(radiant_sec_pair_peer_nonce(B_CH, nonce_a, &fp_b));
	zassert_equal(RADIANT_SEC_EBADMAC,
		      radiant_sec_pair_peer_nonce(A_CH, nonce_b, &fp_a));
	zassert_false(has_key(A_CH));
}

/* Armed (keyed and an epoch set): the transforms run. */
static bool armed(uint8_t ch)
{
	struct radiant_sec_state st;

	radiant_sec_get_state(ch, &st);
	return st.secured;
}

/* Keyed but not armed: a secured-range page is withheld, not sent clear. */
static void assert_pending(uint8_t ch)
{
	uint8_t pay[8] = { RADIANT_SEC_PAGE_LO_DEFAULT, 0u, 1u, 2u, 3u, 4u,
			   5u, 6u };

	zassert_true(has_key(ch));
	zassert_false(armed(ch));
	zassert_equal(RADIANT_SEC_EUNARMED,
		      radiant_sec_tx_transform(ch, pay, 8u,
					       radiant_radio_now()));
}

/* Over-air enrolment's two halves, sensor A and receiver B: A runs the DH and
 * wraps its root into `wrapped`, B runs the DH and holds the wrap keys. */
static void enrol(uint8_t *wrapped, uint32_t *fp_a, uint32_t *fp_b)
{
	uint8_t pub_a[32];
	uint8_t pub_b[32];

	key_up(A_CH, scalar_a, pub_a);
	key_up(B_CH, scalar_b, pub_b);
	zassert_ok(radiant_sec_pair_peer_uncommitted(A_CH, pub_b, wrapped,
						     fp_a));
	zassert_ok(radiant_sec_pair_join(B_CH, pub_a, fp_b));
}

ZTEST(sec_pair, test_enrolment_pairs_through_the_uncommitted_path)
{
	uint8_t  wrapped[RADIANT_SEC_PAIR_WRAPPED_BYTES];
	uint32_t fp_a = 0u;
	uint32_t fp_b = 1u;
	uint32_t fp = 2u;

	/* radiant_profile_enrol.c's path on a sensor that holds no root yet: there is
	 * nothing to deliver and nobody to lock out, so the exchange's
	 * enrolment root becomes A's first, and B receives that one wrapped.
	 * Both ends agree, and the values are the ones radiant_crypto.py
	 * computes for these keys - documented as grindable, kept consistent. */
	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	enrol(wrapped, &fp_a, &fp_b);
	zassert_equal(fp_a, fp_b);
	zassert_equal(VEC_FP_UNCOMMITTED, fp_a, "%u", fp_a);
	zassert_ok(radiant_sec_pair_fingerprint(B_CH, &fp));
	zassert_equal(fp_a, fp);

	/* The join installs nothing; the delivered root does. Both ends'
	 * roots are new, so both are pending until an epoch arrives. */
	zassert_false(has_key(B_CH));
	zassert_ok(radiant_sec_pair_accept_root(B_CH, wrapped,
						RADIANT_SEC_PAIR_INTO_SEC));
	assert_pending(A_CH);
	assert_pending(B_CH);
	assert_root_is(A_CH, A_DEVNUM, VEC_ROOT_UNCOMMITTED);
	assert_root_is(B_CH, B_DEVNUM, VEC_ROOT_UNCOMMITTED);
	zassert_equal(1u, enrolments(A_CH));
	zassert_equal(1u, enrolments(B_CH));
}

/* The root A holds in the delivery tests, and the 24 bytes radiant_crypto.py
 * wraps it to for these two keys (TestEnrolRootDelivery.WRAPPED). */
static const uint8_t live_root[16] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};
static const uint8_t VEC_WRAPPED[RADIANT_SEC_PAIR_WRAPPED_BYTES] = {
	0xd1, 0x43, 0xed, 0x95, 0x37, 0xda, 0x98, 0x82,
	0xca, 0xf0, 0xbd, 0xd2, 0xb6, 0x90, 0xf3, 0xc0,
	0x61, 0x39, 0x17, 0x5e, 0x49, 0xf3, 0xd3, 0xff,
};

/* A secured, armed channel holding live_root at epoch 3. */
static void live_up(void)
{
	give_id(A_CH, A_DEVNUM);
	zassert_ok(radiant_sec_set_key(A_CH, live_root, 128u, A_DEVNUM));
	zassert_ok(radiant_sec_configure(A_CH, RADIANT_SEC_SW_AUTH, 2u,
					 RADIANT_SEC_PAGE_LO_DEFAULT,
					 RADIANT_SEC_PAGE_HI_DEFAULT));
	zassert_ok(radiant_sec_set_epoch(A_CH, 3u, 0u, 8182u));
	zassert_true(armed(A_CH));
}

/*
 * Security review 2026-11-13, item 6: enrolment used to install the DH key as
 * the sensor's root, locking out every receiver it already had. Now the live
 * root is what goes out, sealed, and the channel never stops transmitting
 * under it: still armed (no set_key ran, so its epoch gate was not touched),
 * still the same root, and the receiver ends up holding that root.
 */
ZTEST(sec_pair, test_enrolment_delivers_the_live_root_and_leaves_it_running)
{
	uint8_t  wrapped[RADIANT_SEC_PAIR_WRAPPED_BYTES];
	uint32_t fp_a = 0u;
	uint32_t fp_b = 1u;

	live_up();
	give_id(B_CH, B_DEVNUM);
	enrol(wrapped, &fp_a, &fp_b);
	zassert_equal(fp_a, fp_b);
	zassert_mem_equal(VEC_WRAPPED, wrapped, sizeof(wrapped));

	/* Still armed: nothing reset the sensor's epoch gate. */
	zassert_true(armed(A_CH));
	zassert_equal(1u, enrolments(A_CH));

	zassert_ok(radiant_sec_pair_accept_root(B_CH, wrapped,
						RADIANT_SEC_PAIR_INTO_SEC));
	zassert_equal(1u, enrolments(B_CH));
	/* The receiver's new root fails closed until it has an epoch. */
	assert_pending(B_CH);
	/* assert_root_is() sets epoch 7 on both, explicitly: the receiver's
	 * fresh root is unarmed until an epoch arrives, whatever set_key's
	 * same-root rule. */
	assert_root_is(B_CH, B_DEVNUM, live_root);
	assert_root_is(A_CH, A_DEVNUM, live_root);
}

ZTEST(sec_pair, test_a_tampered_wrapped_root_installs_nothing)
{
	uint8_t  wrapped[RADIANT_SEC_PAIR_WRAPPED_BYTES];
	uint8_t  bad[RADIANT_SEC_PAIR_WRAPPED_BYTES];
	uint32_t fp_a = 0u;
	uint32_t fp_b = 0u;
	uint8_t  i;

	/* Encrypt-then-MAC: one flipped bit anywhere, ciphertext or tag, and
	 * nothing is decrypted or installed. The join survives each refusal,
	 * so a forged copy does not cost the receiver the genuine one. */
	live_up();
	give_id(B_CH, B_DEVNUM);
	enrol(wrapped, &fp_a, &fp_b);

	for (i = 0u; i < sizeof(wrapped); i++) {
		memcpy(bad, wrapped, sizeof(bad));
		bad[i] ^= 0x01u;
		zassert_equal(RADIANT_SEC_EBADMAC,
			      radiant_sec_pair_accept_root(
				      B_CH, bad, RADIANT_SEC_PAIR_INTO_SEC),
			      "byte %u", i);
		zassert_false(has_key(B_CH), "byte %u", i);
	}
	zassert_equal(0u, enrolments(B_CH));

	zassert_ok(radiant_sec_pair_accept_root(B_CH, wrapped,
						RADIANT_SEC_PAIR_INTO_SEC));
	assert_root_is(B_CH, B_DEVNUM, live_root);

	/* And the join is spent once a root is in. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_accept_root(B_CH, wrapped,
						   RADIANT_SEC_PAIR_INTO_SEC));
}

ZTEST(sec_pair, test_a_man_in_the_middle_cannot_unwrap_the_delivered_root)
{
	uint8_t  wrapped[RADIANT_SEC_PAIR_WRAPPED_BYTES];
	uint8_t  pub_a[32];
	uint8_t  pub_m[32];
	uint32_t fp_a = 0u;
	uint32_t fp_b = 0u;
	uint32_t fp_m = 0u;

	/* A listener that joined against A's public key with its own scalar -
	 * a second receiver, or a man in the middle who lost the race - holds
	 * a different DH, so different wrap keys: the root A delivered to B
	 * is opaque to it. */
	live_up();
	give_id(B_CH, B_DEVNUM);
	give_id(M1_CH, M_DEVNUM);
	enrol(wrapped, &fp_a, &fp_b);
	zassert_ok(radiant_sec_pair_local_pubkey(A_CH, pub_a));
	key_up(M1_CH, scalar_m, pub_m);
	zassert_ok(radiant_sec_pair_join(M1_CH, pub_a, &fp_m));
	zassert_not_equal(fp_a, fp_m);

	zassert_equal(RADIANT_SEC_EBADMAC,
		      radiant_sec_pair_accept_root(M1_CH, wrapped,
						   RADIANT_SEC_PAIR_INTO_SEC));
	zassert_false(has_key(M1_CH));
	zassert_ok(radiant_sec_pair_accept_root(B_CH, wrapped,
						RADIANT_SEC_PAIR_INTO_SEC));
}

ZTEST(sec_pair, test_a_root_is_accepted_only_after_a_join)
{
	uint8_t  pub_a[32];
	uint8_t  pub_b[32];
	uint32_t fp = 0u;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);

	/* No pairing context at all. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_accept_root(B_CH, VEC_WRAPPED,
						   RADIANT_SEC_PAIR_INTO_SEC));

	/* A 0xF5-style exchange is not a join: its PRK must never open a
	 * wrapped root, nor a join finish a commit/reveal. */
	keys_cross(A_CH, scalar_a, B_CH, scalar_b);
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_accept_root(B_CH, VEC_WRAPPED,
						   RADIANT_SEC_PAIR_INTO_SEC));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_accept_root(B_CH, NULL,
						   RADIANT_SEC_PAIR_INTO_SEC));

	/* A join, then a target that does not exist. */
	radiant_sec_pair_reset();
	key_up(A_CH, scalar_a, pub_a);
	key_up(B_CH, scalar_b, pub_b);
	zassert_ok(radiant_sec_pair_join(B_CH, pub_a, &fp));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_accept_root(B_CH, VEC_WRAPPED, 2u));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_nonce(B_CH, RESP, nonce_b, pub_b));
	zassert_false(has_key(B_CH));
}

ZTEST(sec_pair, test_a_join_needs_a_device_number_before_it_spends_the_scalar)
{
	uint8_t  pub_a[32];
	uint8_t  pub_b[32];
	uint32_t fp = 0u;

	give_id(A_CH, A_DEVNUM);
	key_up(A_CH, scalar_a, pub_a);
	zassert_ok(radiant_sec_pair_enter(B_CH, 60u));
	zassert_ok(radiant_sec_pair_set_scalar(B_CH, scalar_b));
	zassert_ok(radiant_sec_pair_local_pubkey(B_CH, pub_b));

	/* B's channel has no ID: refused with the scalar still there. */
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_pair_join(B_CH, pub_a, &fp));
	give_id(B_CH, B_DEVNUM);
	zassert_ok(radiant_sec_pair_join(B_CH, pub_a, &fp));
}

ZTEST(sec_pair, test_a_small_order_peer_key_is_refused)
{
	uint8_t  pub_a[32];
	uint8_t  pub_b[32];
	uint8_t  zero_point[32] = { 0 };
	uint8_t  one_point[32] = { 1 };
	uint32_t fp = 0;

	/* u=0 and u=1 make the shared secret all zeros regardless of scalar.
	 * Refused rather than accepted-with-a-weak-key, since this becomes a
	 * root key and accepting it lets an injector fix a known group key. */
	give_id(A_CH, A_DEVNUM);
	key_up(A_CH, scalar_a, pub_a);

	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer(A_CH, zero_point));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer(A_CH, one_point));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_uncommitted(A_CH, zero_point, w, &fp));

	/* Nothing was installed by the attempts, and the scalar survived
	 * them: one injected key does not burn the window. */
	zassert_false(radiant_sec_channel_is_secured(A_CH));
	zassert_false(has_key(A_CH));
	give_id(B_CH, B_DEVNUM);
	key_up(B_CH, scalar_b, pub_b);
	zassert_ok(radiant_sec_pair_peer(A_CH, pub_b));
}

ZTEST(sec_pair, test_pairing_installs_a_key_both_ends_can_use)
{
	uint32_t fp_a = 0u;
	uint32_t fp_b = 0u;

	/* The point of the exchange: both channels now hold a root key.
	 * Asserted through the public surface since the key can't be read back
	 * by design. */
	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	keys_cross(A_CH, scalar_a, B_CH, scalar_b);
	commit_reveal(A_CH, nonce_a, B_CH, nonce_b, &fp_a, &fp_b);

	/* A key is installed, so configure and epoch now succeed - which they
	 * would refuse with RADIANT_SEC_ENOKEY on an unpaired channel. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_configure(A_CH, RADIANT_SEC_SW_AUTH, 2u,
					    RADIANT_SEC_PAGE_LO_DEFAULT,
					    RADIANT_SEC_PAGE_HI_DEFAULT));
	/* Keyed, no epoch yet: pending, and that fails closed (item 5). */
	{
		uint8_t pay[8] = { 0x05u, 0xAAu, 1, 2, 3, 4, 5, 6 };

		zassert_true(radiant_sec_channel_is_secured(A_CH));
		zassert_equal(RADIANT_SEC_EUNARMED,
			      radiant_sec_tx_transform(A_CH, pay, 8u, 0u));
	}
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_set_epoch(A_CH, 1u, 0u, 8182u));
	zassert_true(radiant_sec_channel_is_secured(A_CH));
}

ZTEST(sec_pair, test_nothing_works_before_pairing_mode_is_entered)
{
	uint8_t  pub[32];
	uint8_t  cb[16];
	uint32_t fp = 0;

	give_id(A_CH, A_DEVNUM);

	/* No enter, so there is no context and no scalar to spend. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_scalar(A_CH, scalar_a));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_local_pubkey(A_CH, pub));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer(A_CH, scalar_b));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_nonce(A_CH, RESP, nonce_b, cb));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_commit(A_CH, cb));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_nonce(A_CH, nonce_a, &fp));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_uncommitted(A_CH, scalar_b, w, &fp));
	zassert_false(radiant_sec_pair_is_open(A_CH));
}

ZTEST(sec_pair, test_an_exchange_needs_a_scalar_first)
{
	give_id(A_CH, A_DEVNUM);
	zassert_equal(RADIANT_SEC_OK, radiant_sec_pair_enter(A_CH, 60u));

	/* Open, but with no local key there is nothing to complete. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer(A_CH, scalar_b));
}

ZTEST(sec_pair, test_leaving_wipes_the_state)
{
	uint8_t  pub[32];
	uint8_t  cb[16];
	uint32_t fp = 0u;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	keys_cross(A_CH, scalar_a, B_CH, scalar_b);
	zassert_ok(radiant_sec_pair_set_nonce(B_CH, RESP, nonce_b, cb));
	zassert_true(radiant_sec_pair_is_open(B_CH));

	/* Mid commit/reveal: leaving takes the nonce and the DH result with
	 * it, so nothing can finish afterwards. */
	radiant_sec_pair_leave(B_CH);
	zassert_false(radiant_sec_pair_is_open(B_CH));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_local_pubkey(B_CH, pub));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_nonce(B_CH, nonce_a, &fp));
	zassert_false(has_key(B_CH));
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

	/* Loss of tracking comes from the radio callback, which only marks the
	 * channel; the port releases it under its lock (housekeeping). */
	radiant_sec_pair_on_search(A_CH);
	radiant_sec_pair_service();

	zassert_false(radiant_sec_pair_is_open(A_CH));
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
	zassert_true(radiant_sec_pair_is_open(A_CH));
}

ZTEST(sec_pair, test_the_fingerprint_is_readable_only_after_the_reveal)
{
	uint32_t fp_a = 0u;
	uint32_t fp_b = 0u;
	uint32_t read_back = 0u;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	zassert_ok(radiant_sec_pair_enter(A_CH, 60u));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_fingerprint(A_CH, &read_back));
	radiant_sec_pair_leave(A_CH);

	/* Not after the key exchange either: that is the grindable point. */
	keys_cross(A_CH, scalar_a, B_CH, scalar_b);
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_fingerprint(A_CH, &read_back));

	commit_reveal(A_CH, nonce_a, B_CH, nonce_b, &fp_a, &fp_b);
	zassert_ok(radiant_sec_pair_fingerprint(A_CH, &read_back));
	zassert_equal(fp_a, read_back);
}

/*
 * One scalar, one exchange (review 2026-10-31, D1-2). The scalar is spent by
 * the exchange, and an all-zero scalar clamps to the public constant 2^254 -
 * so a host retrying EXCHANGE (a lost USB reply, say) used to install a root
 * key anyone who saw peer_pub could compute.
 */
ZTEST(sec_pair, test_an_exchange_spends_the_scalar)
{
	uint8_t  pub_a[32];
	uint8_t  pub_b[32];
	uint32_t fp_a = 0u;
	uint32_t fp_b = 0u;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	key_up(A_CH, scalar_a, pub_a);
	key_up(B_CH, scalar_b, pub_b);
	zassert_ok(radiant_sec_pair_peer(A_CH, pub_b));

	/* A host retry of EXCHANGE with the same peer key. */
	zassert_equal(RADIANT_SEC_EINVAL, radiant_sec_pair_peer(A_CH, pub_b));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_uncommitted(A_CH, pub_b, w, &fp_a));

	/* The first exchange is intact and still completes. */
	zassert_ok(radiant_sec_pair_peer(B_CH, pub_a));
	commit_reveal(A_CH, nonce_a, B_CH, nonce_b, &fp_a, &fp_b);
	zassert_equal(fp_a, fp_b);
	zassert_ok(radiant_sec_configure(A_CH, RADIANT_SEC_SW_AUTH, 2u,
					 RADIANT_SEC_PAGE_LO_DEFAULT,
					 RADIANT_SEC_PAGE_HI_DEFAULT));
	zassert_ok(radiant_sec_set_epoch(A_CH, 1u, 0u, 8182u));
	zassert_true(radiant_sec_channel_is_secured(A_CH));

	/* And nothing re-keys the finished channel. */
	zassert_equal(RADIANT_SEC_EINVAL, radiant_sec_pair_peer(A_CH, pub_b));
	zassert_true(radiant_sec_channel_is_secured(A_CH));
}

/*
 * Review 2026-11-13 item 3: past the commitment nothing may fail retryably. A
 * responder left at its nonce step after a failed install would take a second
 * Na - an attacker's, if B's host had already leaked Nb - and show a chosen
 * fingerprint.
 */
ZTEST(sec_pair, test_a_channel_without_a_device_number_cannot_commit)
{
	uint8_t cb[16];

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, 0u);
	keys_cross(A_CH, scalar_a, B_CH, scalar_b);
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_pair_set_nonce(B_CH, RESP, nonce_b, cb));
}

ZTEST(sec_pair, test_a_failed_install_after_the_commitment_wipes_the_exchange)
{
	uint8_t  cb[16];
	uint8_t  na2[16];
	uint32_t fp = 0u;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	keys_cross(A_CH, scalar_a, B_CH, scalar_b);
	zassert_ok(radiant_sec_pair_set_nonce(B_CH, RESP, nonce_b, cb));

	/* The install fails after the commitment (here: the ID changed). */
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(B_CH, 0u, 0x0Bu, 0x05u));
	zassert_equal(RADIANT_SEC_ENOKEY,
		      radiant_sec_pair_peer_nonce(B_CH, nonce_a, &fp));

	/* No second Na, not even after the ID is put back. */
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(B_CH, B_DEVNUM, 0x0Bu, 0x05u));
	memcpy(na2, nonce_a, sizeof(na2));
	na2[3] ^= 0x10u;
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_nonce(B_CH, na2, &fp));
	zassert_false(has_key(B_CH));
}

/* Review item 5: all zeros is enrolment's constant, never a committed nonce. */
ZTEST(sec_pair, test_an_all_zero_nonce_is_refused)
{
	static const uint8_t zero[16];
	uint8_t              cb[16];
	uint32_t             fp_a = 0u;
	uint32_t             fp_b = 1u;
	uint32_t             fp = 0u;

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	keys_cross(A_CH, scalar_a, B_CH, scalar_b);
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_nonce(B_CH, RESP, zero, cb));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_nonce(A_CH, INIT, zero, NULL));

	zassert_ok(radiant_sec_pair_set_nonce(B_CH, RESP, nonce_b, cb));
	zassert_ok(radiant_sec_pair_set_nonce(A_CH, INIT, nonce_a, NULL));
	zassert_ok(radiant_sec_pair_peer_commit(A_CH, cb));
	/* A zero reveal from the peer is refused without disturbing the step. */
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_peer_nonce(B_CH, zero, &fp));
	zassert_ok(radiant_sec_pair_peer_nonce(B_CH, nonce_a, &fp_b));
	zassert_ok(radiant_sec_pair_peer_nonce(A_CH, nonce_b, &fp_a));
	zassert_equal(fp_a, fp_b);
}

/* Review item 4: ENTER on a channel mid-exchange starts from nothing. */
ZTEST(sec_pair, test_re_entering_wipes_the_old_exchange)
{
	uint8_t pub[32];
	uint8_t cb[16];

	give_id(A_CH, A_DEVNUM);
	give_id(B_CH, B_DEVNUM);
	keys_cross(A_CH, scalar_a, B_CH, scalar_b);

	zassert_ok(radiant_sec_pair_enter(B_CH, 60u));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_local_pubkey(B_CH, pub));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_set_nonce(B_CH, RESP, nonce_b, cb));
}

/* Review item 4: an abandoned window is freed by housekeeping, not only by
 * the next pairing call. */
ZTEST(sec_pair, test_housekeeping_frees_an_expired_window)
{
	uint8_t pub[32];

	give_id(A_CH, A_DEVNUM);
	zassert_ok(radiant_sec_pair_enter(A_CH, 1u));
	zassert_ok(radiant_sec_pair_set_scalar(A_CH, scalar_a));
	k_sleep(K_MSEC(1100));

	/* local_pubkey does no lazy expiry, so it sees whether the context
	 * (and the scalar in it) is still in RAM. */
	zassert_ok(radiant_sec_pair_local_pubkey(A_CH, pub));
	radiant_sec_pair_service();
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_pair_local_pubkey(A_CH, pub));
}

/*
 * A loss of tracking is recorded on every RX_FAIL_GO_TO_SEARCH, pairing or
 * not, and waits for the next housekeeping pass. It must not release a pairing
 * entered after it.
 */
ZTEST(sec_pair, test_a_loss_recorded_before_enter_does_not_release_the_pairing)
{
	give_id(A_CH, A_DEVNUM);
	radiant_sec_pair_on_search(A_CH);
	zassert_ok(radiant_sec_pair_enter(A_CH, 60u));
	radiant_sec_pair_service();
	zassert_true(radiant_sec_pair_is_open(A_CH));
}

#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */
