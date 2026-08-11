/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Published vectors for the crypto primitives, and the two blocks
 * docs/radiant-security.md pins to the byte.
 *
 * Four published sets and one cross-check:
 *
 *   FIPS-197 C.1 and appendix B    AES-128 block encryption
 *   RFC 4493 section 4             AES-CMAC, subkeys and the empty message
 *   SP 800-38A F.5.1               AES-128-CTR
 *   tools/radiant_crypto.py        the SP 800-108 KDF outputs, as literals
 *
 * That last one is the one worth explaining. The KDF is CMAC over a block this
 * project invented, so no standards body publishes a vector for it - but the
 * host tools have to compute the same value the node does or a two-dongle 1:N
 * proof cannot work. The literals below also appear in
 * tools/test_radiant_crypto.py. Neither implementation is authoritative;
 * agreeing is the assertion, and the published sets above are what keep both
 * of them honest about AES itself.
 *
 * This suite needs no radio, no HAL and no fake_radio - src/radiant_sec_aes.c
 * includes nothing but <stdint.h>, <string.h> and the seam header, which is
 * exactly the property that lets a suite hammer it.
 *
 * It runs twice: once against the portable mode layer, and once with
 * CONFIG_RADIANT_SEC_BACKEND_MODES=y, where src/sec_modes_backend.c supplies
 * AES-CTR and AES-CMAC itself. The second run is what proves the two-level
 * seam is live rather than decorative.
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant_core/radiant_sec.h>

/* RFC 4493's key and SP 800-38A's plaintext, which are the same four blocks. */
static const uint8_t nist_key[16] = {
	0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
	0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
};

static const uint8_t nist_message[64] = {
	0x6b, 0xc1, 0xbe, 0xe2, 0x2e, 0x40, 0x9f, 0x96,
	0xe9, 0x3d, 0x7e, 0x11, 0x73, 0x93, 0x17, 0x2a,
	0xae, 0x2d, 0x8a, 0x57, 0x1e, 0x03, 0xac, 0x9c,
	0x9e, 0xb7, 0x6f, 0xac, 0x45, 0xaf, 0x8e, 0x51,
	0x30, 0xc8, 0x1c, 0x46, 0xa3, 0x5c, 0xe4, 0x11,
	0xe5, 0xfb, 0xc1, 0x19, 0x1a, 0x0a, 0x52, 0xef,
	0xf6, 0x9f, 0x24, 0x45, 0xdf, 0x4f, 0x9b, 0x17,
	0xad, 0x2b, 0x41, 0x7b, 0xe6, 0x6c, 0x37, 0x10,
};

static void import_or_fail(struct radiant_sec_key *k, const uint8_t *raw)
{
	zassert_equal(radiant_sec_key_import(k, raw, 128), RADIANT_SEC_OK,
		      "a 128-bit import must succeed on every backend");
}

/* ── FIPS-197 ───────────────────────────────────────────────────────────── */

ZTEST(sec_aes, test_fips197_appendix_c1)
{
	static const uint8_t key[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	};
	static const uint8_t plain[16] = {
		0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
		0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
	};
	static const uint8_t want[16] = {
		0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
		0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
	};
	struct radiant_sec_key k;
	uint8_t got[16];

	import_or_fail(&k, key);
	zassert_equal(radiant_sec_aes_ecb(&k, plain, got), RADIANT_SEC_OK, NULL);
	zassert_mem_equal(got, want, 16, "FIPS-197 C.1");
	radiant_sec_key_destroy(&k);
}

ZTEST(sec_aes, test_fips197_appendix_b)
{
	static const uint8_t plain[16] = {
		0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a, 0x30, 0x8d,
		0x31, 0x31, 0x98, 0xa2, 0xe0, 0x37, 0x07, 0x34,
	};
	static const uint8_t want[16] = {
		0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc, 0x09, 0xfb,
		0xdc, 0x11, 0x85, 0x97, 0x19, 0x6a, 0x0b, 0x32,
	};
	struct radiant_sec_key k;
	uint8_t got[16];

	import_or_fail(&k, nist_key);
	zassert_equal(radiant_sec_aes_ecb(&k, plain, got), RADIANT_SEC_OK, NULL);
	zassert_mem_equal(got, want, 16, "FIPS-197 appendix B");
	radiant_sec_key_destroy(&k);
}

/* ── RFC 4493 ───────────────────────────────────────────────────────────── */

ZTEST(sec_aes, test_rfc4493_vectors)
{
	static const struct {
		size_t len;
		uint8_t tag[16];
	} cases[] = {
		{ 0, { 0xbb, 0x1d, 0x69, 0x29, 0xe9, 0x59, 0x37, 0x28,
		       0x7f, 0xa3, 0x7d, 0x12, 0x9b, 0x75, 0x67, 0x46 } },
		{ 16, { 0x07, 0x0a, 0x16, 0xb4, 0x6b, 0x4d, 0x41, 0x44,
			0xf7, 0x9b, 0xdd, 0x9d, 0xd0, 0x4a, 0x28, 0x7c } },
		{ 40, { 0xdf, 0xa6, 0x67, 0x47, 0xde, 0x9a, 0xe6, 0x30,
			0x30, 0xca, 0x32, 0x61, 0x14, 0x97, 0xc8, 0x27 } },
		{ 64, { 0x51, 0xf0, 0xbe, 0xbf, 0x7e, 0x3b, 0x9d, 0x92,
			0xfc, 0x49, 0x74, 0x17, 0x79, 0x36, 0x3c, 0xfe } },
	};
	struct radiant_sec_key k;
	unsigned i;

	import_or_fail(&k, nist_key);
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		uint8_t got[16];

		zassert_equal(radiant_sec_cmac(&k, nist_message, cases[i].len,
					       got),
			      RADIANT_SEC_OK, NULL);
		zassert_mem_equal(got, cases[i].tag, 16,
				  "RFC 4493 vector, message length %u",
				  (unsigned)cases[i].len);
	}
	radiant_sec_key_destroy(&k);
}

ZTEST(sec_aes, test_cmac_incremental_matches_one_shot)
{
	/*
	 * The incremental path is what the spread-MAC window uses, and it is
	 * where a full block held back one update too early or too late stops
	 * mattering only for messages whose length is a multiple of 16 - which
	 * is most of the cases a casual test would try. So: every split of
	 * every length, against the one-shot answer.
	 */
	struct radiant_sec_key k;
	size_t len;

	import_or_fail(&k, nist_key);
	for (len = 0; len <= 64; len++) {
		uint8_t want[16];
		size_t split;

		zassert_equal(radiant_sec_cmac(&k, nist_message, len, want),
			      RADIANT_SEC_OK, NULL);

		for (split = 0; split <= len; split++) {
			struct radiant_sec_cmac_ctx ctx;
			uint8_t got[16];

			zassert_equal(radiant_sec_cmac_init(&ctx, &k),
				      RADIANT_SEC_OK, NULL);
			zassert_equal(radiant_sec_cmac_update(&ctx,
							      nist_message,
							      split),
				      RADIANT_SEC_OK, NULL);
			zassert_equal(radiant_sec_cmac_update(&ctx,
							      nist_message +
							      split,
							      len - split),
				      RADIANT_SEC_OK, NULL);
			zassert_equal(radiant_sec_cmac_final(&ctx, got),
				      RADIANT_SEC_OK, NULL);
			zassert_mem_equal(got, want, 16,
					  "length %u split at %u",
					  (unsigned)len, (unsigned)split);
		}
	}
	radiant_sec_key_destroy(&k);
}

ZTEST(sec_aes, test_cmac_byte_at_a_time)
{
	/* The pathological split, and the one the window actually makes: seven
	 * bytes per packet, arriving one packet at a time. */
	struct radiant_sec_key k;
	struct radiant_sec_cmac_ctx ctx;
	uint8_t want[16];
	uint8_t got[16];
	size_t i;

	import_or_fail(&k, nist_key);
	zassert_equal(radiant_sec_cmac(&k, nist_message, 64, want),
		      RADIANT_SEC_OK, NULL);

	zassert_equal(radiant_sec_cmac_init(&ctx, &k), RADIANT_SEC_OK, NULL);
	for (i = 0; i < 64; i++) {
		zassert_equal(radiant_sec_cmac_update(&ctx, nist_message + i, 1),
			      RADIANT_SEC_OK, NULL);
	}
	zassert_equal(radiant_sec_cmac_final(&ctx, got), RADIANT_SEC_OK, NULL);
	zassert_mem_equal(got, want, 16, "byte-at-a-time CMAC");
	radiant_sec_key_destroy(&k);
}

/* ── SP 800-38A ─────────────────────────────────────────────────────────── */

static const uint8_t ctr_iv[16] = {
	0xf0, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7,
	0xf8, 0xf9, 0xfa, 0xfb, 0xfc, 0xfd, 0xfe, 0xff,
};

static const uint8_t ctr_cipher[64] = {
	0x87, 0x4d, 0x61, 0x91, 0xb6, 0x20, 0xe3, 0x26,
	0x1b, 0xef, 0x68, 0x64, 0x99, 0x0d, 0xb6, 0xce,
	0x98, 0x06, 0xf6, 0x6b, 0x79, 0x70, 0xfd, 0xff,
	0x86, 0x17, 0x18, 0x7b, 0xb9, 0xff, 0xfd, 0xff,
	0x5a, 0xe4, 0xdf, 0x3e, 0xdb, 0xd5, 0xd3, 0x5e,
	0x5b, 0x4f, 0x09, 0x02, 0x0d, 0xb0, 0x3e, 0xab,
	0x1e, 0x03, 0x1d, 0xda, 0x2f, 0xbe, 0x03, 0xd1,
	0x79, 0x21, 0x70, 0xa0, 0xf3, 0x00, 0x9c, 0xee,
};

ZTEST(sec_aes, test_sp800_38a_ctr)
{
	struct radiant_sec_key k;
	uint8_t buf[64];

	import_or_fail(&k, nist_key);
	memcpy(buf, nist_message, sizeof(buf));
	zassert_equal(radiant_sec_ctr_xor(&k, ctr_iv, buf, sizeof(buf)),
		      RADIANT_SEC_OK, NULL);
	zassert_mem_equal(buf, ctr_cipher, sizeof(buf), "SP 800-38A F.5.1");

	/* CTR is its own inverse, which is what makes one transform function
	 * serve both directions in radiant_sec.c. */
	zassert_equal(radiant_sec_ctr_xor(&k, ctr_iv, buf, sizeof(buf)),
		      RADIANT_SEC_OK, NULL);
	zassert_mem_equal(buf, nist_message, sizeof(buf), "CTR round trip");
	radiant_sec_key_destroy(&k);
}

ZTEST(sec_aes, test_ctr_five_bytes)
{
	/* Five bytes is the length X_CONF actually uses - bytes [2..6] of a
	 * page - so it gets a vector rather than an assumption that truncating
	 * a 16-byte result is the same thing. */
	struct radiant_sec_key k;
	uint8_t buf[5];

	import_or_fail(&k, nist_key);
	memcpy(buf, nist_message, sizeof(buf));
	zassert_equal(radiant_sec_ctr_xor(&k, ctr_iv, buf, sizeof(buf)),
		      RADIANT_SEC_OK, NULL);
	zassert_mem_equal(buf, ctr_cipher, sizeof(buf), "partial CTR block");
	radiant_sec_key_destroy(&k);
}

ZTEST(sec_aes, test_ctr_counter_carries_across_the_whole_block)
{
	/*
	 * A counter block of all-ones must carry all the way into byte 0
	 * rather than wrapping the last byte in place. RadiANT never reaches
	 * this - its nonce block ends in seven zeroes - but two
	 * implementations disagreeing here would diverge silently on the first
	 * long message, so it is pinned rather than left to be discovered.
	 */
	struct radiant_sec_key k;
	uint8_t ones[16];
	uint8_t zeros[16];
	uint8_t block_a[16];
	uint8_t block_b[16];
	uint8_t stream[32];

	import_or_fail(&k, nist_key);
	memset(ones, 0xff, sizeof(ones));
	memset(zeros, 0, sizeof(zeros));
	zassert_equal(radiant_sec_aes_ecb(&k, ones, block_a), RADIANT_SEC_OK,
		      NULL);
	zassert_equal(radiant_sec_aes_ecb(&k, zeros, block_b), RADIANT_SEC_OK,
		      NULL);

	memset(stream, 0, sizeof(stream));
	zassert_equal(radiant_sec_ctr_xor(&k, ones, stream, sizeof(stream)),
		      RADIANT_SEC_OK, NULL);
	zassert_mem_equal(stream, block_a, 16, "first keystream block");
	zassert_mem_equal(stream + 16, block_b, 16,
			  "the counter must carry into byte 0");
	radiant_sec_key_destroy(&k);
}

/* ── The pinned blocks ──────────────────────────────────────────────────── */

ZTEST(sec_aes, test_nonce_block_layout)
{
	static const uint8_t want[16] = {
		0x44, 0x33, 0x22, 0x11,   /* epoch  0x11223344, LE */
		0xef, 0xbe,               /* devnum 0xBEEF, LE */
		0x02, 0x01,               /* counter 0x0102, LE */
		0x01,                     /* domain: CTR keystream */
		0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	};
	uint8_t got[16];

	radiant_sec_nonce_block(got, 0x11223344u, 0xBEEFu, 0x0102u,
				RADIANT_SEC_DOM_CTR);
	zassert_mem_equal(got, want, 16, "docs/radiant-security.md 3.3");
}

ZTEST(sec_aes, test_domain_byte_separates_keystream_from_mac)
{
	/*
	 * The field the first draft omitted, and the omission that produces a
	 * scheme passing every test and leaking the tag key: without it, a
	 * keystream block and a MAC block coincide under one
	 * (epoch, devnum, counter).
	 */
	uint8_t ctr[16];
	uint8_t mac[16];
	uint8_t desc[16];

	radiant_sec_nonce_block(ctr, 7, 0x1234, 9, RADIANT_SEC_DOM_CTR);
	radiant_sec_nonce_block(mac, 7, 0x1234, 9, RADIANT_SEC_DOM_SPREAD_MAC);
	radiant_sec_nonce_block(desc, 7, 0x1234, 9, RADIANT_SEC_DOM_DESC_MAC);

	zassert_true(memcmp(ctr, mac, 16) != 0, NULL);
	zassert_true(memcmp(ctr, desc, 16) != 0, NULL);
	zassert_true(memcmp(mac, desc, 16) != 0, NULL);
	zassert_equal(ctr[8], 0x01, NULL);
	zassert_equal(mac[8], 0x02, NULL);
	zassert_equal(desc[8], 0x03, NULL);
}

ZTEST(sec_aes, test_kdf_vectors_shared_with_the_python_mirror)
{
	static const uint8_t root_raw[16] = {
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
		0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
	};
	static const struct {
		const char *label;
		uint32_t epoch;
		uint8_t want[16];
	} cases[] = {
		{ RADIANT_SEC_LABEL_ENC, 0x11223344u,
		  { 0x58, 0xd0, 0x15, 0x7f, 0x06, 0xca, 0x49, 0xc1,
		    0x41, 0x44, 0xc2, 0xdb, 0x93, 0xe9, 0x1b, 0x40 } },
		{ RADIANT_SEC_LABEL_AUTH, 0x11223344u,
		  { 0x60, 0x70, 0x64, 0x5b, 0x3a, 0x9f, 0x3e, 0xac,
		    0x4a, 0x9e, 0xaf, 0xa0, 0x5b, 0x77, 0x71, 0x12 } },
		/* The "id" label is epoch-less and uses epoch = 0. Pinned so
		 * two implementations cannot differ. */
		{ RADIANT_SEC_LABEL_ID, 0u,
		  { 0x5a, 0x6a, 0x14, 0xa0, 0x28, 0x97, 0x35, 0x71,
		    0x2b, 0xf7, 0xd1, 0x9a, 0x02, 0x30, 0x88, 0x7f } },
		{ RADIANT_SEC_LABEL_CMD, 0x11223344u,
		  { 0x4b, 0xa0, 0xc7, 0xa1, 0x90, 0xd3, 0xdd, 0x0e,
		    0x45, 0x24, 0x3a, 0x1d, 0xee, 0x28, 0x3b, 0x29 } },
	};
	struct radiant_sec_key root;
	unsigned i;

	import_or_fail(&root, root_raw);
	for (i = 0; i < ARRAY_SIZE(cases); i++) {
		struct radiant_sec_key derived;

		zassert_equal(radiant_sec_kdf(&root, cases[i].label,
					      cases[i].epoch, 0xBEEFu,
					      &derived),
			      RADIANT_SEC_OK, NULL);
		zassert_mem_equal(derived.b, cases[i].want, 16,
				  "KDF label %s disagrees with "
				  "tools/radiant_crypto.py", cases[i].label);
		radiant_sec_key_destroy(&derived);
	}
	radiant_sec_key_destroy(&root);
}

ZTEST(sec_aes, test_every_derived_key_differs)
{
	static const uint8_t root_raw[16] = { 0xa5 };
	struct radiant_sec_key root;
	struct radiant_sec_key derived[4];
	static const char *const labels[4] = {
		RADIANT_SEC_LABEL_ENC, RADIANT_SEC_LABEL_AUTH,
		RADIANT_SEC_LABEL_ID, RADIANT_SEC_LABEL_CMD,
	};
	unsigned i;
	unsigned j;

	import_or_fail(&root, root_raw);
	for (i = 0; i < 4; i++) {
		uint32_t epoch = (labels[i] == RADIANT_SEC_LABEL_ID) ? 0u : 99u;

		zassert_equal(radiant_sec_kdf(&root, labels[i], epoch, 0x0101u,
					      &derived[i]),
			      RADIANT_SEC_OK, NULL);
	}
	for (i = 0; i < 4; i++) {
		for (j = i + 1; j < 4; j++) {
			zassert_true(memcmp(derived[i].b, derived[j].b, 16) != 0,
				     "labels %s and %s derive the same key",
				     labels[i], labels[j]);
		}
	}
	for (i = 0; i < 4; i++) {
		radiant_sec_key_destroy(&derived[i]);
	}
	radiant_sec_key_destroy(&root);
}

ZTEST(sec_aes, test_base_devnum_separates_two_sensors_under_one_root)
{
	/* A household sharing one root across two same-type sensors: binding
	 * base_devnum into the KDF is what gives each its own keys. */
	static const uint8_t root_raw[16] = { 0x11, 0x22, 0x33 };
	struct radiant_sec_key root;
	struct radiant_sec_key a;
	struct radiant_sec_key b;

	import_or_fail(&root, root_raw);
	zassert_equal(radiant_sec_kdf(&root, RADIANT_SEC_LABEL_ENC, 5, 1, &a),
		      RADIANT_SEC_OK, NULL);
	zassert_equal(radiant_sec_kdf(&root, RADIANT_SEC_LABEL_ENC, 5, 2, &b),
		      RADIANT_SEC_OK, NULL);
	zassert_true(memcmp(a.b, b.b, 16) != 0, NULL);

	radiant_sec_key_destroy(&a);
	radiant_sec_key_destroy(&b);
	radiant_sec_key_destroy(&root);
}

ZTEST(sec_aes, test_epoch_rotates_the_derived_key)
{
	static const uint8_t root_raw[16] = { 0x5a };
	struct radiant_sec_key root;
	struct radiant_sec_key a;
	struct radiant_sec_key b;

	import_or_fail(&root, root_raw);
	zassert_equal(radiant_sec_kdf(&root, RADIANT_SEC_LABEL_ENC, 1, 7, &a),
		      RADIANT_SEC_OK, NULL);
	zassert_equal(radiant_sec_kdf(&root, RADIANT_SEC_LABEL_ENC, 2, 7, &b),
		      RADIANT_SEC_OK, NULL);
	zassert_true(memcmp(a.b, b.b, 16) != 0,
		     "the epoch is the key-rotation clock; if it does not "
		     "rotate the key, a reboot's fresh epoch buys nothing");

	radiant_sec_key_destroy(&a);
	radiant_sec_key_destroy(&b);
	radiant_sec_key_destroy(&root);
}

/* ── The seam ───────────────────────────────────────────────────────────── */

ZTEST(sec_aes, test_caps_are_self_consistent)
{
	const struct radiant_sec_caps *caps = radiant_sec_caps();

	zassert_not_null(caps, NULL);
	zassert_not_null(caps->name, NULL);
	zassert_true((caps->key_bits_mask & RADIANT_SEC_KEY_BITS_128) != 0,
		     "every backend must do AES-128; the protocol is AES-128");

#if defined(CONFIG_RADIANT_SEC_BACKEND_MODES)
	zassert_true(caps->has_ctr,
		     "a mode-level backend advertises the modes it provides, "
		     "and radiant_sec.c decides what it can do from that "
		     "rather than from a backend's name");
	zassert_true(caps->has_cmac, NULL);
#else
	zassert_false(caps->has_ctr,
		      "the software backend implements the block level; the "
		      "portable layer provides the modes over it");
	zassert_false(caps->has_cmac, NULL);
#endif
}

ZTEST(sec_aes, test_key_import_refuses_what_the_backend_cannot_do)
{
	struct radiant_sec_key k;
	uint8_t raw[32];

	memset(raw, 0x5a, sizeof(raw));

	/* The seam expresses 256 so a future backend can offer it. The
	 * software one refuses loudly rather than quietly truncating a key a
	 * caller meant. */
	if ((radiant_sec_caps()->key_bits_mask & RADIANT_SEC_KEY_BITS_256) == 0) {
		zassert_equal(radiant_sec_key_import(&k, raw, 256),
			      RADIANT_SEC_ENOTSUP, NULL);
	}
	zassert_equal(radiant_sec_key_import(&k, raw, 7), RADIANT_SEC_ENOTSUP,
		      NULL);
	zassert_equal(radiant_sec_key_import(NULL, raw, 128), RADIANT_SEC_EINVAL,
		      NULL);
}

ZTEST(sec_aes, test_an_empty_or_destroyed_key_is_refused)
{
	/* Nothing here should ever encrypt under a zeroed key and call it a
	 * result. */
	struct radiant_sec_key k;
	uint8_t out[16];
	uint8_t in[16] = { 0 };

	memset(&k, 0, sizeof(k));
	zassert_equal(radiant_sec_aes_ecb(&k, in, out), RADIANT_SEC_ENOKEY, NULL);
	zassert_equal(radiant_sec_cmac(&k, in, sizeof(in), out),
		      RADIANT_SEC_ENOKEY, NULL);
	zassert_equal(radiant_sec_ctr_xor(&k, in, out, sizeof(out)),
		      RADIANT_SEC_ENOKEY, NULL);

	import_or_fail(&k, nist_key);
	radiant_sec_key_destroy(&k);
	zassert_equal(radiant_sec_aes_ecb(&k, in, out), RADIANT_SEC_ENOKEY,
		      "a destroyed key must not still encrypt");
}

ZTEST(sec_aes, test_two_keys_do_not_alias_through_the_shared_schedule)
{
	/*
	 * The software backend keeps ONE key schedule for the whole system and
	 * re-expands it when the key changes, because 176 bytes per key would
	 * be 4.2 KB against a ~960 B budget. The cache tag is an import
	 * serial, not an address: destroy a key and import another at the same
	 * address and an address-keyed cache would silently encrypt under the
	 * old one. This is that test, and it is written to interleave rather
	 * than to run one key and then the other, because the interleave is
	 * what the spread-MAC window does.
	 */
	static const uint8_t other_raw[16] = {
		0xff, 0xee, 0xdd, 0xcc, 0xbb, 0xaa, 0x99, 0x88,
		0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11, 0x00,
	};
	struct radiant_sec_key a;
	struct radiant_sec_key b;
	uint8_t block[16];
	uint8_t want_a[16];
	uint8_t want_b[16];
	uint8_t got[16];
	unsigned i;

	memset(block, 0x42, sizeof(block));
	import_or_fail(&a, nist_key);
	import_or_fail(&b, other_raw);

	zassert_equal(radiant_sec_aes_ecb(&a, block, want_a), RADIANT_SEC_OK,
		      NULL);
	zassert_equal(radiant_sec_aes_ecb(&b, block, want_b), RADIANT_SEC_OK,
		      NULL);
	zassert_true(memcmp(want_a, want_b, 16) != 0, NULL);

	for (i = 0; i < 8; i++) {
		zassert_equal(radiant_sec_aes_ecb(&a, block, got),
			      RADIANT_SEC_OK, NULL);
		zassert_mem_equal(got, want_a, 16, "key A after %u swaps", i);
		zassert_equal(radiant_sec_aes_ecb(&b, block, got),
			      RADIANT_SEC_OK, NULL);
		zassert_mem_equal(got, want_b, 16, "key B after %u swaps", i);
	}

	/* And the recycled-address case the serial exists for. */
	radiant_sec_key_destroy(&a);
	import_or_fail(&a, other_raw);
	zassert_equal(radiant_sec_aes_ecb(&a, block, got), RADIANT_SEC_OK, NULL);
	zassert_mem_equal(got, want_b, 16,
			  "a re-imported handle must use its new key");

	radiant_sec_key_destroy(&a);
	radiant_sec_key_destroy(&b);
}

ZTEST_SUITE(sec_aes, NULL, NULL, NULL, NULL, NULL);
