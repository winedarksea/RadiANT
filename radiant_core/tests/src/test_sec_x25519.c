/* SPDX-License-Identifier: Apache-2.0 */
/*
 * X25519 against RFC 7748's own published vectors.
 *
 * Every vector here is copied from the RFC and from nothing else, which is the
 * only kind of vector worth having for a primitive written from that same RFC:
 * a test whose expected values came out of the implementation under test proves
 * that the code is self-consistent and nothing more.
 *
 * The whole file is behind CONFIG_RADIANT_SEC_PAIRING_X25519 because the
 * parent application globs src/*.c and the symbol is default n.
 */

#include <zephyr/kernel.h>

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant_core/radiant_sec.h>

/* RFC 7748 section 5.2, first vector. */
static const uint8_t v1_scalar[32] = {
	0xa5, 0x46, 0xe3, 0x6b, 0xf0, 0x52, 0x7c, 0x9d,
	0x3b, 0x16, 0x15, 0x4b, 0x82, 0x46, 0x5e, 0xdd,
	0x62, 0x14, 0x4c, 0x0a, 0xc1, 0xfc, 0x5a, 0x18,
	0x50, 0x6a, 0x22, 0x44, 0xba, 0x44, 0x9a, 0xc4,
};
static const uint8_t v1_point[32] = {
	0xe6, 0xdb, 0x68, 0x67, 0x58, 0x30, 0x30, 0xdb,
	0x35, 0x94, 0xc1, 0xa4, 0x24, 0xb1, 0x5f, 0x7c,
	0x72, 0x66, 0x24, 0xec, 0x26, 0xb3, 0x35, 0x3b,
	0x10, 0xa9, 0x03, 0xa6, 0xd0, 0xab, 0x1c, 0x4c,
};
static const uint8_t v1_want[32] = {
	0xc3, 0xda, 0x55, 0x37, 0x9d, 0xe9, 0xc6, 0x90,
	0x8e, 0x94, 0xea, 0x4d, 0xf2, 0x8d, 0x08, 0x4f,
	0x32, 0xec, 0xcf, 0x03, 0x49, 0x1c, 0x71, 0xf7,
	0x54, 0xb4, 0x07, 0x55, 0x77, 0xa2, 0x85, 0x52,
};

/* RFC 7748 section 5.2, second vector. */
static const uint8_t v2_scalar[32] = {
	0x4b, 0x66, 0xe9, 0xd4, 0xd1, 0xb4, 0x67, 0x3c,
	0x5a, 0xd2, 0x26, 0x91, 0x95, 0x7d, 0x6a, 0xf5,
	0xc1, 0x1b, 0x64, 0x21, 0xe0, 0xea, 0x01, 0xd4,
	0x2c, 0xa4, 0x16, 0x9e, 0x79, 0x18, 0xba, 0x0d,
};
static const uint8_t v2_point[32] = {
	0xe5, 0x21, 0x0f, 0x12, 0x78, 0x68, 0x11, 0xd3,
	0xf4, 0xb7, 0x95, 0x9d, 0x05, 0x38, 0xae, 0x2c,
	0x31, 0xdb, 0xe7, 0x10, 0x6f, 0xc0, 0x3c, 0x3e,
	0xfc, 0x4c, 0xd5, 0x49, 0xc7, 0x15, 0xa4, 0x93,
};
static const uint8_t v2_want[32] = {
	0x95, 0xcb, 0xde, 0x94, 0x76, 0xe8, 0x90, 0x7d,
	0x7a, 0xad, 0xe4, 0x5c, 0xb4, 0xb8, 0x73, 0xf8,
	0x8b, 0x59, 0x5a, 0x68, 0x79, 0x9f, 0xa1, 0x52,
	0xe6, 0xf8, 0xf7, 0x64, 0x7a, 0xac, 0x79, 0x57,
};

/* RFC 7748 section 6.1, the Diffie-Hellman worked example. */
static const uint8_t alice_sk[32] = {
	0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
	0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
	0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
	0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
};
static const uint8_t alice_pk[32] = {
	0x85, 0x20, 0xf0, 0x09, 0x89, 0x30, 0xa7, 0x54,
	0x74, 0x8b, 0x7d, 0xdc, 0xb4, 0x3e, 0xf7, 0x5a,
	0x0d, 0xbf, 0x3a, 0x0d, 0x26, 0x38, 0x1a, 0xf4,
	0xeb, 0xa4, 0xa9, 0x8e, 0xaa, 0x9b, 0x4e, 0x6a,
};
static const uint8_t bob_sk[32] = {
	0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b,
	0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
	0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
	0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb,
};
static const uint8_t bob_pk[32] = {
	0xde, 0x9e, 0xdb, 0x7d, 0x7b, 0x7d, 0xc1, 0xb4,
	0xd3, 0x5b, 0x61, 0xc2, 0xec, 0xe4, 0x35, 0x37,
	0x3f, 0x83, 0x43, 0xc8, 0x5b, 0x78, 0x67, 0x4d,
	0xad, 0xfc, 0x7e, 0x14, 0x6f, 0x88, 0x2b, 0x4f,
};
static const uint8_t shared[32] = {
	0x4a, 0x5d, 0x9d, 0x5b, 0xa4, 0xce, 0x2d, 0xe1,
	0x72, 0x8e, 0x3b, 0xf4, 0x80, 0x35, 0x0f, 0x25,
	0xe0, 0x7e, 0x21, 0xc9, 0x47, 0xd1, 0x9e, 0x33,
	0x76, 0xf0, 0x9b, 0x3c, 0x1e, 0x16, 0x17, 0x42,
};

ZTEST_SUITE(sec_x25519, NULL, NULL, NULL, NULL, NULL);

ZTEST(sec_x25519, test_rfc7748_vector_one)
{
	uint8_t got[32];

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_x25519(got, v1_scalar, v1_point));
	zassert_mem_equal(got, v1_want, 32, "RFC 7748 5.2 vector 1");
}

ZTEST(sec_x25519, test_rfc7748_vector_two)
{
	uint8_t got[32];

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_x25519(got, v2_scalar, v2_point));
	zassert_mem_equal(got, v2_want, 32, "RFC 7748 5.2 vector 2");
}

ZTEST(sec_x25519, test_the_base_point_produces_the_published_public_keys)
{
	uint8_t got[32];

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_x25519(got, alice_sk,
					 radiant_sec_x25519_basepoint));
	zassert_mem_equal(got, alice_pk, 32, "Alice's public key");

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_x25519(got, bob_sk,
					 radiant_sec_x25519_basepoint));
	zassert_mem_equal(got, bob_pk, 32, "Bob's public key");
}

ZTEST(sec_x25519, test_both_sides_reach_the_same_secret)
{
	uint8_t mine[32];
	uint8_t theirs[32];

	/*
	 * The property the whole pairing rests on, and the published value it
	 * has to equal. Agreeing with each other but not with the RFC would
	 * mean two RadiANT nodes pair happily and nothing else in the world
	 * can ever join them.
	 */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_x25519(mine, alice_sk, bob_pk));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_x25519(theirs, bob_sk, alice_pk));

	zassert_mem_equal(mine, theirs, 32, "the two sides disagree");
	zassert_mem_equal(mine, shared, 32, "RFC 7748 6.1 shared secret");
}

ZTEST(sec_x25519, test_the_caller_s_scalar_is_not_rewritten)
{
	uint8_t scalar[32];
	uint8_t out[32];

	/*
	 * Clamping happens on a copy. A function that clamped in place would
	 * corrupt a host's stored private key on first use, and the damage
	 * would only show up as a peer that could no longer agree.
	 */
	memcpy(scalar, alice_sk, sizeof(scalar));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_x25519(out, scalar,
					 radiant_sec_x25519_basepoint));
	zassert_mem_equal(scalar, alice_sk, 32,
			  "the scalar was clamped in place");
}

ZTEST(sec_x25519, test_the_high_bit_of_a_u_coordinate_is_ignored)
{
	uint8_t point[32];
	uint8_t clean[32];
	uint8_t dirty[32];

	/*
	 * RFC 7748 section 5: decodeUCoordinate MASKS the most significant bit
	 * rather than rejecting the input. Implementations exist that set it,
	 * and a receiver treating the value as 256 bits would compute a
	 * different secret from its peer and fail to agree - silently, which is
	 * the worst way for a pairing to fail.
	 */
	memcpy(point, bob_pk, sizeof(point));
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_x25519(clean, alice_sk, point));

	point[31] |= 0x80u;
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_x25519(dirty, alice_sk, point));

	zassert_mem_equal(clean, dirty, 32,
			  "bit 255 of the u-coordinate changed the answer");
}

ZTEST(sec_x25519, test_a_small_order_point_is_caught)
{
	uint8_t zero_point[32] = { 0 };
	uint8_t one_point[32] = { 1 };
	uint8_t out[32];

	/*
	 * u = 0 and u = 1 are small-order points: the shared secret comes out
	 * all zeros whatever the scalar was. RFC 7748 leaves rejecting this
	 * optional for Diffie-Hellman; here it is mandatory, because the result
	 * is fed to a KDF and installed as a root key. Accepting it would let
	 * anyone able to inject one packet fix the group key to a value they
	 * already know.
	 */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_x25519(out, alice_sk, zero_point));
	zassert_true(radiant_sec_x25519_is_degenerate(out),
		     "u=0 was not recognised as degenerate");

	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_x25519(out, alice_sk, one_point));
	zassert_true(radiant_sec_x25519_is_degenerate(out),
		     "u=1 was not recognised as degenerate");

	/* And an honest secret is not flagged. */
	zassert_equal(RADIANT_SEC_OK,
		      radiant_sec_x25519(out, alice_sk, bob_pk));
	zassert_false(radiant_sec_x25519_is_degenerate(out));
}

ZTEST(sec_x25519, test_null_arguments_are_refused)
{
	uint8_t out[32];

	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_x25519(NULL, alice_sk, bob_pk));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_x25519(out, NULL, bob_pk));
	zassert_equal(RADIANT_SEC_EINVAL,
		      radiant_sec_x25519(out, alice_sk, NULL));
	zassert_true(radiant_sec_x25519_is_degenerate(NULL));
}

ZTEST(sec_x25519, test_one_ladder_costs_what_the_spec_says_it_costs)
{
	uint8_t  out[32];
	uint32_t start;
	uint32_t ms;
	int      i;

	/*
	 * Not a performance test - a truth test on a number in the
	 * documentation. docs/radiant-security.md section 8 quoted "roughly
	 * 4 ms on a Cortex-M4", which is what an OPTIMISED implementation
	 * costs; this one is 16-bit limbs and schoolbook multiplication, which
	 * is far simpler to audit and an order of magnitude slower.
	 *
	 * The bound is deliberately loose. A pairing happens once, by hand,
	 * with a human waiting - so a few hundred milliseconds is free and the
	 * only thing worth catching is a change that makes it seconds. What the
	 * assertion really protects is the DOCUMENTED figure: if this trips,
	 * the number in the spec is wrong again and should be re-measured
	 * rather than the bound raised.
	 */
	start = k_uptime_get_32();
	for (i = 0; i < 4; i++) {
		zassert_equal(RADIANT_SEC_OK,
			      radiant_sec_x25519(out, alice_sk, bob_pk));
	}
	ms = (k_uptime_get_32() - start) / 4u;

	printk("x25519: %u ms per exchange\n", (unsigned)ms);
	zassert_true(ms < 1000u,
		     "one exchange took %u ms; the spec's stated cost and this "
		     "implementation have diverged", (unsigned)ms);
}

ZTEST(sec_x25519, test_the_caps_bit_says_it_is_here)
{
	/* A caller must test the cap rather than the Kconfig symbol: "not
	 * built" and "this backend cannot" are the same condition from above,
	 * and only the cap expresses both. */
	zassert_true(radiant_sec_caps()->has_x25519,
		     "X25519 is compiled in but the capability says otherwise");
}

#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */
