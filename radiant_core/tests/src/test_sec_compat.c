/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The compat attestation primitives: docs/radiant-security.md section 11.4 and
 * docs/decisions/0008, as assertions.
 *
 * Vectors are literals, also pinned in tools/test_radiant_crypto.py, rather
 * than a round trip - two implementations checked only against each other can
 * agree on a shared mistake and pass. AES itself is covered by test_sec_aes.c;
 * this file asserts only the layer this project invented.
 *
 * No page and no channel here: subtype separation is asserted by constructing
 * the nonce inputs directly, not by sending a page with a subtype nibble - a
 * page-only test would pass even if the subtype never reached the MAC, the
 * failure ADR 0008 pins the position to prevent.
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant_core/radiant_sec_compat.h>

/* K_auth as a literal, not a KDF output - a KDF change would otherwise move
 * every tag here and misattribute the failure. */
static const uint8_t k_auth_raw[16] = {
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
};

#define VEC_EPOCH   0x11223344u
#define VEC_DEVNUM  0xBEEFu
#define VEC_COUNTER 0x0501u

/* nonce_block for the two tiers at one counter value. Identical but for
 * position 9, which is the whole claim. */
static const uint8_t nonce_tier_i[16] = {
	0x44, 0x33, 0x22, 0x11, 0xef, 0xbe, 0x01, 0x05,
	0x04, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};
static const uint8_t nonce_tier_ii[16] = {
	0x44, 0x33, 0x22, 0x11, 0xef, 0xbe, 0x01, 0x05,
	0x04, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* Untruncated CMAC over each block, so the separation is visible as sixteen
 * bytes and the truncation below is demonstrably a prefix of this. */
static const uint8_t full_tag_tier_i[16] = {
	0x86, 0xa6, 0x3d, 0x51, 0x14, 0x99, 0x83, 0xaa,
	0x92, 0x94, 0xad, 0x0d, 0x7d, 0x00, 0x7a, 0xd6,
};
static const uint8_t full_tag_tier_ii[16] = {
	0x18, 0xd0, 0xa9, 0x1f, 0xa6, 0x75, 0x58, 0x47,
	0x9b, 0x5e, 0x34, 0x15, 0xfe, 0x72, 0x85, 0x4e,
};

/* trunc40 of the above. */
static const uint8_t tier1_tag[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES] = {
	0x86, 0xa6, 0x3d, 0x51, 0x14,
};

/* Seven synthetic transmitted messages (N=8 means p_1..p_7). Byte [0] differs
 * from the rest so a Tier II implementation that skips byte [0] (legitimate
 * for the spread tag) produces a different tag and is caught. */
static const uint8_t window_msgs[7][RADIANT_SEC_COMPAT_MSG_BYTES] = {
	{ 0x10, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00 },
	{ 0x11, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 },
	{ 0x12, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02, 0x02 },
	{ 0x13, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03, 0x03 },
	{ 0x14, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 },
	{ 0x15, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05, 0x05 },
	{ 0x16, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06, 0x06 },
};

/* N = 8 over all seven, at window index 0x0501. */
static const uint8_t tier2_tag_n8[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES] = {
	0xb6, 0x1a, 0xa9, 0x87, 0x8f, 0x10,
};

/* N = 4 over the first three, at window index 0x0007 - a second window size,
 * because an implementation that hard-coded 8 would pass everything above. */
static const uint8_t tier2_tag_n4[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES] = {
	0xdc, 0x94, 0x1a, 0x16, 0xd2, 0x3e,
};

/* The N = 8 window with one bit flipped in the LAST covered message. */
static const uint8_t tier2_tag_n8_flipped[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES] = {
	0x22, 0x1f, 0x4a, 0x94, 0xdc, 0x76,
};

static void import_or_fail(struct radiant_sec_key *k, const uint8_t *raw)
{
	zassert_equal(radiant_sec_key_import(k, raw, 128), RADIANT_SEC_OK,
		      "a 128-bit import must succeed on every backend");
}

/* ── The block ──────────────────────────────────────────────────────────── */

ZTEST(sec_compat, test_nonce_block_is_section_3_3_extended_at_position_9)
{
	uint8_t got[16];

	radiant_sec_compat_nonce_block(got, VEC_EPOCH, VEC_DEVNUM, VEC_COUNTER,
				       RADIANT_SEC_COMPAT_SUBTYPE_TIER_I);
	zassert_mem_equal(got, nonce_tier_i, 16,
			  "the compat nonce disagrees with "
			  "tools/radiant_crypto.py and with ADR 0008");

	/* The three facts the layout has to keep, spelled separately so a
	 * failure says which one moved. */
	zassert_equal(got[8], RADIANT_SEC_DOM_COMPAT_MAC,
		      "the domain byte stays at position 8, where every other "
		      "block in this protocol has it");
	zassert_equal(got[9], RADIANT_SEC_COMPAT_SUBTYPE_TIER_I, NULL);
	for (unsigned i = 10; i < 16; i++) {
		zassert_equal(got[i], 0x00, "position %u must stay zero", i);
	}

	radiant_sec_compat_nonce_block(got, VEC_EPOCH, VEC_DEVNUM, VEC_COUNTER,
				       RADIANT_SEC_COMPAT_SUBTYPE_TIER_II);
	zassert_mem_equal(got, nonce_tier_ii, 16, NULL);
}

ZTEST(sec_compat, test_the_compat_domain_byte_is_its_own)
{
	uint8_t compat[16];
	uint8_t spread[16];

	/* Without its own domain byte, a compat tag and a spread tag over the
	 * same (epoch, devnum, counter) would be the same block. */
	radiant_sec_compat_nonce_block(compat, VEC_EPOCH, VEC_DEVNUM,
				       VEC_COUNTER,
				       RADIANT_SEC_COMPAT_SUBTYPE_TIER_I);
	radiant_sec_nonce_block(spread, VEC_EPOCH, VEC_DEVNUM, VEC_COUNTER,
				RADIANT_SEC_DOM_SPREAD_MAC);
	zassert_true(memcmp(compat, spread, 16) != 0, NULL);
	zassert_equal(RADIANT_SEC_DOM_COMPAT_MAC, 0x04,
		      "ADR 0008 pins the value, not merely the name");
}

ZTEST(sec_compat, test_the_subtype_reaches_the_mac_and_not_only_the_page)
{
	/* Built directly rather than via a page: a subtype written only into a
	 * page byte is chosen by the sender, so the tags would be interchangeable
	 * and a Tier II tag replayable as a Tier I one. */
	struct radiant_sec_key k;
	uint8_t block[16];
	uint8_t got_i[16];
	uint8_t got_ii[16];
	uint8_t announce[16];

	import_or_fail(&k, k_auth_raw);

	radiant_sec_compat_nonce_block(block, VEC_EPOCH, VEC_DEVNUM,
				       VEC_COUNTER,
				       RADIANT_SEC_COMPAT_SUBTYPE_TIER_I);
	zassert_equal(radiant_sec_cmac(&k, block, 16, got_i), RADIANT_SEC_OK,
		      NULL);
	radiant_sec_compat_nonce_block(block, VEC_EPOCH, VEC_DEVNUM,
				       VEC_COUNTER,
				       RADIANT_SEC_COMPAT_SUBTYPE_TIER_II);
	zassert_equal(radiant_sec_cmac(&k, block, 16, got_ii), RADIANT_SEC_OK,
		      NULL);

	zassert_mem_equal(got_i, full_tag_tier_i, 16, NULL);
	zassert_mem_equal(got_ii, full_tag_tier_ii, 16, NULL);
	zassert_true(memcmp(got_i, got_ii, 16) != 0,
		     "one counter value, two subtypes, one tag - the subtype "
		     "is not inside the MAC'd block");

	/* And the third subtype, which nothing computes yet, is a third
	 * block - pinned now so the value cannot be spent twice. */
	radiant_sec_compat_nonce_block(announce, VEC_EPOCH, VEC_DEVNUM,
				       VEC_COUNTER,
				       RADIANT_SEC_COMPAT_SUBTYPE_ANNOUNCE);
	zassert_equal(announce[9], 0x03, NULL);
	zassert_true(memcmp(announce, nonce_tier_i, 16) != 0, NULL);
	zassert_true(memcmp(announce, nonce_tier_ii, 16) != 0, NULL);

	radiant_sec_key_destroy(&k);
}

ZTEST(sec_compat, test_an_illegal_subtype_writes_nothing)
{
	uint8_t got[16];
	static const uint8_t untouched[16] = {
		0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
		0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
	};

	/* Subtype is a nibble (fits in a page byte beside other data); 0 or 0x10
	 * is a caller bug. */
	memset(got, 0xa5, sizeof(got));
	radiant_sec_compat_nonce_block(got, VEC_EPOCH, VEC_DEVNUM, VEC_COUNTER,
				       0x00);
	zassert_mem_equal(got, untouched, 16, NULL);

	radiant_sec_compat_nonce_block(got, VEC_EPOCH, VEC_DEVNUM, VEC_COUNTER,
				       0x10);
	zassert_mem_equal(got, untouched, 16, NULL);
}

/* ── Tier I ─────────────────────────────────────────────────────────────── */

ZTEST(sec_compat, test_tier1_vector_shared_with_the_python_mirror)
{
	struct radiant_sec_key k;
	uint8_t got[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES];

	import_or_fail(&k, k_auth_raw);
	zassert_equal(radiant_sec_compat_tier1_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER, got),
		      RADIANT_SEC_OK, NULL);
	zassert_mem_equal(got, tier1_tag, sizeof(got),
			  "Tier I disagrees with tools/radiant_crypto.py");

	/* trunc40 is the first five bytes - the end matters, two implementations
	 * could easily disagree here. */
	zassert_mem_equal(got, full_tag_tier_i,
			  RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES, NULL);
	zassert_equal(RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES * 8, 40,
		      "40 bits, not 48: the counter needs two in-page bytes");

	radiant_sec_key_destroy(&k);
}

ZTEST(sec_compat, test_tier1_covers_the_counter_the_devnum_and_the_epoch)
{
	/* Covers no payload, so counter/devnum/epoch are the whole claim, and
	 * each has to reach the tag: counter closes replay, devnum/epoch bind it
	 * to this node's stream and this boot. */
	struct radiant_sec_key k;
	uint8_t base[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES];
	uint8_t other[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES];

	import_or_fail(&k, k_auth_raw);
	zassert_equal(radiant_sec_compat_tier1_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER, base),
		      RADIANT_SEC_OK, NULL);

	zassert_equal(radiant_sec_compat_tier1_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER + 1u, other),
		      RADIANT_SEC_OK, NULL);
	zassert_true(memcmp(base, other, sizeof(base)) != 0,
		     "a replayed page with a fresh counter would verify");

	zassert_equal(radiant_sec_compat_tier1_tag(&k, VEC_EPOCH,
						   VEC_DEVNUM ^ 1u,
						   VEC_COUNTER, other),
		      RADIANT_SEC_OK, NULL);
	zassert_true(memcmp(base, other, sizeof(base)) != 0,
		     "a colliding sensor is the mainstream threat; the device "
		     "number has to be inside the tag");

	zassert_equal(radiant_sec_compat_tier1_tag(&k, VEC_EPOCH + 1u,
						   VEC_DEVNUM, VEC_COUNTER,
						   other),
		      RADIANT_SEC_OK, NULL);
	zassert_true(memcmp(base, other, sizeof(base)) != 0, NULL);

	radiant_sec_key_destroy(&k);
}

ZTEST(sec_compat, test_tier1_is_key_bound)
{
	static const uint8_t other_raw[16] = {
		0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
		0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
	};
	struct radiant_sec_key a;
	struct radiant_sec_key b;
	uint8_t tag_a[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES];
	uint8_t tag_b[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES];

	import_or_fail(&a, k_auth_raw);
	import_or_fail(&b, other_raw);
	zassert_equal(radiant_sec_compat_tier1_tag(&a, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER, tag_a),
		      RADIANT_SEC_OK, NULL);
	zassert_equal(radiant_sec_compat_tier1_tag(&b, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER, tag_b),
		      RADIANT_SEC_OK, NULL);
	zassert_true(memcmp(tag_a, tag_b, sizeof(tag_a)) != 0,
		     "the question the tier answers is 'from the holder of "
		     "K_auth?'; two keys must not answer it the same way");

	radiant_sec_key_destroy(&a);
	radiant_sec_key_destroy(&b);
}

/* ── Tier II ────────────────────────────────────────────────────────────── */

ZTEST(sec_compat, test_tier2_vectors_shared_with_the_python_mirror)
{
	struct radiant_sec_key k;
	uint8_t got[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES];

	import_or_fail(&k, k_auth_raw);

	zassert_equal(radiant_sec_compat_tier2_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER,
						   &window_msgs[0][0], 8, got),
		      RADIANT_SEC_OK, NULL);
	zassert_mem_equal(got, tier2_tag_n8, sizeof(got),
			  "Tier II at N=8 disagrees with "
			  "tools/radiant_crypto.py");

	zassert_equal(radiant_sec_compat_tier2_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   0x0007u,
						   &window_msgs[0][0], 4, got),
		      RADIANT_SEC_OK, NULL);
	zassert_mem_equal(got, tier2_tag_n4, sizeof(got),
			  "Tier II at N=4 disagrees with "
			  "tools/radiant_crypto.py");

	zassert_equal(RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES * 8, 48, NULL);
	radiant_sec_key_destroy(&k);
}

ZTEST(sec_compat, test_tier2_covers_every_byte_of_every_covered_message)
{
	/* One flipped bit in the last covered message; the tag must move (and
	 * the moved value is pinned, so it can't pass by going constant). */
	struct radiant_sec_key k;
	uint8_t msgs[7][RADIANT_SEC_COMPAT_MSG_BYTES];
	uint8_t got[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES];

	memcpy(msgs, window_msgs, sizeof(msgs));
	msgs[6][1] ^= 0x01;

	import_or_fail(&k, k_auth_raw);
	zassert_equal(radiant_sec_compat_tier2_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER, &msgs[0][0], 8,
						   got),
		      RADIANT_SEC_OK, NULL);
	zassert_mem_equal(got, tier2_tag_n8_flipped, sizeof(got), NULL);
	zassert_true(memcmp(got, tier2_tag_n8, sizeof(got)) != 0, NULL);
	radiant_sec_key_destroy(&k);
}

ZTEST(sec_compat, test_tier2_covers_byte_zero_unlike_the_spread_tag)
{
	/* The spread tag excludes byte [7] (where it rides); a compat tag has
	 * its own page, so all eight bytes are inside the MAC. Byte [0] matters
	 * most - flip it unauthenticated and an attacker reinterprets the bits
	 * against a different schema. */
	struct radiant_sec_key k;
	uint8_t msgs[7][RADIANT_SEC_COMPAT_MSG_BYTES];
	uint8_t first[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES];
	uint8_t last[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES];

	import_or_fail(&k, k_auth_raw);

	memcpy(msgs, window_msgs, sizeof(msgs));
	msgs[3][0] ^= 0x80;
	zassert_equal(radiant_sec_compat_tier2_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER, &msgs[0][0], 8,
						   first),
		      RADIANT_SEC_OK, NULL);
	zassert_true(memcmp(first, tier2_tag_n8, sizeof(first)) != 0,
		     "byte [0] of a covered message is outside the MAC");

	memcpy(msgs, window_msgs, sizeof(msgs));
	msgs[3][7] ^= 0x80;
	zassert_equal(radiant_sec_compat_tier2_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER, &msgs[0][0], 8,
						   last),
		      RADIANT_SEC_OK, NULL);
	zassert_true(memcmp(last, tier2_tag_n8, sizeof(last)) != 0,
		     "byte [7] of a covered message is outside the MAC");

	radiant_sec_key_destroy(&k);
}

ZTEST(sec_compat, test_tier2_message_order_is_transmission_order)
{
	struct radiant_sec_key k;
	uint8_t msgs[7][RADIANT_SEC_COMPAT_MSG_BYTES];
	uint8_t got[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES];

	/* Two messages swapped. A MAC over a SET rather than a SEQUENCE would
	 * let an attacker reorder a window's history and keep it verifiable. */
	memcpy(msgs, window_msgs, sizeof(msgs));
	memcpy(msgs[0], window_msgs[6], RADIANT_SEC_COMPAT_MSG_BYTES);
	memcpy(msgs[6], window_msgs[0], RADIANT_SEC_COMPAT_MSG_BYTES);

	import_or_fail(&k, k_auth_raw);
	zassert_equal(radiant_sec_compat_tier2_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER, &msgs[0][0], 8,
						   got),
		      RADIANT_SEC_OK, NULL);
	zassert_true(memcmp(got, tier2_tag_n8, sizeof(got)) != 0, NULL);
	radiant_sec_key_destroy(&k);
}

ZTEST(sec_compat, test_tier2_window_index_is_inside_the_tag)
{
	struct radiant_sec_key k;
	uint8_t a[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES];
	uint8_t b[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES];

	import_or_fail(&k, k_auth_raw);
	zassert_equal(radiant_sec_compat_tier2_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER,
						   &window_msgs[0][0], 8, a),
		      RADIANT_SEC_OK, NULL);
	zassert_equal(radiant_sec_compat_tier2_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER + 1u,
						   &window_msgs[0][0], 8, b),
		      RADIANT_SEC_OK, NULL);
	zassert_true(memcmp(a, b, sizeof(a)) != 0,
		     "identical windows at two indices must not share a tag, "
		     "or a whole window replays");
	radiant_sec_key_destroy(&k);
}

ZTEST(sec_compat, test_tier2_refuses_an_unenumerated_window)
{
	struct radiant_sec_key k;
	uint8_t got[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES];
	static const uint8_t illegal[] = { 0, 1, 2, 3, 5, 6, 7, 9, 12, 24, 31,
					   33, 64, 128, 255 };
	unsigned i;

	import_or_fail(&k, k_auth_raw);
	for (i = 0; i < ARRAY_SIZE(illegal); i++) {
		zassert_equal(radiant_sec_compat_tier2_tag(&k, VEC_EPOCH,
							   VEC_DEVNUM,
							   VEC_COUNTER,
							   &window_msgs[0][0],
							   illegal[i], got),
			      RADIANT_SEC_EINVAL,
			      "N = %u was accepted; N is the airtime cost, the "
			      "latency and the amplification factor at once",
			      illegal[i]);
	}

	/* And every legal one is accepted, so the check above cannot pass by
	 * refusing everything. The 16 and 32 cases read past window_msgs, so
	 * they get a buffer of their own. */
	{
		uint8_t big[31][RADIANT_SEC_COMPAT_MSG_BYTES];
		static const uint8_t legal[] = { 4, 8, 16, 32 };

		memset(big, 0x5a, sizeof(big));
		for (i = 0; i < ARRAY_SIZE(legal); i++) {
			zassert_equal(radiant_sec_compat_tier2_tag(
					      &k, VEC_EPOCH, VEC_DEVNUM,
					      VEC_COUNTER, &big[0][0],
					      legal[i], got),
				      RADIANT_SEC_OK, "N = %u", legal[i]);
		}
	}
	radiant_sec_key_destroy(&k);
}

/* ── Refusals ───────────────────────────────────────────────────────────── */

ZTEST(sec_compat, test_a_missing_argument_or_key_is_refused)
{
	struct radiant_sec_key k;
	uint8_t got[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES];

	zassert_equal(radiant_sec_compat_tier1_tag(NULL, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER, got),
		      RADIANT_SEC_EINVAL, NULL);
	zassert_equal(radiant_sec_compat_tier2_tag(NULL, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER,
						   &window_msgs[0][0], 8, got),
		      RADIANT_SEC_EINVAL, NULL);

	import_or_fail(&k, k_auth_raw);
	zassert_equal(radiant_sec_compat_tier1_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER, NULL),
		      RADIANT_SEC_EINVAL, NULL);
	zassert_equal(radiant_sec_compat_tier2_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER, NULL, 8, got),
		      RADIANT_SEC_EINVAL, NULL);

	/* An empty handle must not produce a tag under a zeroed key and call
	 * it a result. */
	radiant_sec_key_destroy(&k);
	zassert_equal(radiant_sec_compat_tier1_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER, got),
		      RADIANT_SEC_ENOKEY, NULL);
	zassert_equal(radiant_sec_compat_tier2_tag(&k, VEC_EPOCH, VEC_DEVNUM,
						   VEC_COUNTER,
						   &window_msgs[0][0], 8, got),
		      RADIANT_SEC_ENOKEY, NULL);
}

ZTEST_SUITE(sec_compat, NULL, NULL, NULL, NULL, NULL);
