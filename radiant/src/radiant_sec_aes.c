/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sec_aes.c - the software crypto backend, and the portable helpers
 * every backend shares.
 *
 * AES-128 is the single primitive in v1: CTR for X_CONF, CMAC (RFC 4493) for
 * every MAC, SP 800-108 counter-mode KDF with CMAC as the PRF for derived
 * keys. No SHA linked anywhere (saves ~1.4 KB), test surface is four
 * published vector sets - FIPS-197, RFC 4493, SP 800-38A, SP 800-108 - run
 * by tests/src/test_sec_aes.c.
 *
 * WHAT THIS FILE MAY INCLUDE: <stdint.h> and <string.h>, nothing else - no
 * Zephyr header, no HAL, no CMSIS. Lets ztest hammer this file on native_sim
 * with no radio and no board; protect this when someone wants "just one"
 * logging call in here. The one CONFIG_ symbol below uses a plain #ifdef
 * (Zephyr force-includes autoconf.h, no include needed).
 *
 * NO UNALIGNED WORD ACCESS, EVER. Current targets are ARM (tolerates it); an
 * RV32 core may trap or emulate it slowly. Everything goes through memcpy
 * into aligned locals or explicit byte shifts.
 *
 * NO CMSIS IN THE PORTABLE PATH. No __REV, no __RBIT - ARM specialisation
 * goes behind a guard, not inline.
 *
 * WHY NOT A T-TABLE AES: the classic 4x256-word T-table is faster but costs
 * 4 KB of rodata - the entire .text budget the zero-cost job asserts for
 * this feature. So: a 256-byte S-box plus MixColumns on 32-bit columns via
 * the xtime trick (word-oriented, no tables), packed by explicit shifts so
 * it's byte-order independent by construction. ~3-4x a naive byte loop,
 * ~12 us/block on an M4 at 64 MHz - noise against a 500 us radio window. Do
 * not hand-optimise this; the effort belongs in the seam, where accelerators
 * actually attach.
 */

#include <stdint.h>
#include <string.h>

#include <radiant/radiant_sec.h>

/* ── AES-128, encrypt only ──────────────────────────────────────────────── */

static const uint8_t sbox[256] = {
	0x63, 0x7c, 0x77, 0x7b, 0xf2, 0x6b, 0x6f, 0xc5,
	0x30, 0x01, 0x67, 0x2b, 0xfe, 0xd7, 0xab, 0x76,
	0xca, 0x82, 0xc9, 0x7d, 0xfa, 0x59, 0x47, 0xf0,
	0xad, 0xd4, 0xa2, 0xaf, 0x9c, 0xa4, 0x72, 0xc0,
	0xb7, 0xfd, 0x93, 0x26, 0x36, 0x3f, 0xf7, 0xcc,
	0x34, 0xa5, 0xe5, 0xf1, 0x71, 0xd8, 0x31, 0x15,
	0x04, 0xc7, 0x23, 0xc3, 0x18, 0x96, 0x05, 0x9a,
	0x07, 0x12, 0x80, 0xe2, 0xeb, 0x27, 0xb2, 0x75,
	0x09, 0x83, 0x2c, 0x1a, 0x1b, 0x6e, 0x5a, 0xa0,
	0x52, 0x3b, 0xd6, 0xb3, 0x29, 0xe3, 0x2f, 0x84,
	0x53, 0xd1, 0x00, 0xed, 0x20, 0xfc, 0xb1, 0x5b,
	0x6a, 0xcb, 0xbe, 0x39, 0x4a, 0x4c, 0x58, 0xcf,
	0xd0, 0xef, 0xaa, 0xfb, 0x43, 0x4d, 0x33, 0x85,
	0x45, 0xf9, 0x02, 0x7f, 0x50, 0x3c, 0x9f, 0xa8,
	0x51, 0xa3, 0x40, 0x8f, 0x92, 0x9d, 0x38, 0xf5,
	0xbc, 0xb6, 0xda, 0x21, 0x10, 0xff, 0xf3, 0xd2,
	0xcd, 0x0c, 0x13, 0xec, 0x5f, 0x97, 0x44, 0x17,
	0xc4, 0xa7, 0x7e, 0x3d, 0x64, 0x5d, 0x19, 0x73,
	0x60, 0x81, 0x4f, 0xdc, 0x22, 0x2a, 0x90, 0x88,
	0x46, 0xee, 0xb8, 0x14, 0xde, 0x5e, 0x0b, 0xdb,
	0xe0, 0x32, 0x3a, 0x0a, 0x49, 0x06, 0x24, 0x5c,
	0xc2, 0xd3, 0xac, 0x62, 0x91, 0x95, 0xe4, 0x79,
	0xe7, 0xc8, 0x37, 0x6d, 0x8d, 0xd5, 0x4e, 0xa9,
	0x6c, 0x56, 0xf4, 0xea, 0x65, 0x7a, 0xae, 0x08,
	0xba, 0x78, 0x25, 0x2e, 0x1c, 0xa6, 0xb4, 0xc6,
	0xe8, 0xdd, 0x74, 0x1f, 0x4b, 0xbd, 0x8b, 0x8a,
	0x70, 0x3e, 0xb5, 0x66, 0x48, 0x03, 0xf6, 0x0e,
	0x61, 0x35, 0x57, 0xb9, 0x86, 0xc1, 0x1d, 0x9e,
	0xe1, 0xf8, 0x98, 0x11, 0x69, 0xd9, 0x8e, 0x94,
	0x9b, 0x1e, 0x87, 0xe9, 0xce, 0x55, 0x28, 0xdf,
	0x8c, 0xa1, 0x89, 0x0d, 0xbf, 0xe6, 0x42, 0x68,
	0x41, 0x99, 0x2d, 0x0f, 0xb0, 0x54, 0xbb, 0x16,
};

#define AES128_ROUNDS 10
#define AES128_WORDS  ((AES128_ROUNDS + 1) * 4)   /* 44 */

static uint32_t rotl32(uint32_t x, unsigned n)
{
	return (x << n) | (x >> (32u - n));
}

static uint32_t sub_word(uint32_t w)
{
	return ((uint32_t)sbox[(w >> 24) & 0xffu] << 24) |
	       ((uint32_t)sbox[(w >> 16) & 0xffu] << 16) |
	       ((uint32_t)sbox[(w >>  8) & 0xffu] <<  8) |
	       ((uint32_t)sbox[ w        & 0xffu]);
}

/* Multiply every byte of the word by x in GF(2^8), branchlessly. */
static uint32_t xtime_word(uint32_t x)
{
	uint32_t high = x & 0x80808080u;

	return ((x & 0x7f7f7f7fu) << 1) ^ ((high >> 7) * 0x1bu);
}

/*
 * MixColumns on one packed column:
 *   d = 2*c ^ 3*r1 ^ r2 ^ r3
 *     = xtime(c ^ r1) ^ r1 ^ r2 ^ r3      where rN = rotl32(c, 8*N)
 * the identity that removes the tables.
 */
static uint32_t mix_column(uint32_t c)
{
	uint32_t r1 = rotl32(c, 8);
	uint32_t r2 = rotl32(c, 16);
	uint32_t r3 = rotl32(c, 24);

	return xtime_word(c ^ r1) ^ r1 ^ r2 ^ r3;
}

/*
 * The shared key schedule: ONE for the whole system, re-expanded when the key
 * changes. Eight secured channels x three derived keys each would be 4.2 KB
 * of schedule against a ~960 B budget; expansion is ~1/10th of a block
 * encryption and the access pattern is repetitive (one key per CMAC/CTR run),
 * so the cache hits essentially always.
 *
 * Cache tag is the key's import serial, NOT its address: an address-keyed
 * cache would silently encrypt under the old key if one were destroyed and
 * another imported at the same address.
 *
 * NOT RE-ENTRANT - radiant serialises every call with api_lock, and the
 * no-crypto-in-ISR rule makes that sufficient.
 */
static uint32_t sched[AES128_WORDS];
static uint32_t sched_serial;   /* 0 = nothing cached */
static uint32_t import_serial;  /* monotone; 32 bits so it cannot wrap into a
				 * serial that is still live */

static void key_expand(const uint8_t key[16])
{
	static const uint8_t rcon[AES128_ROUNDS] = {
		0x01, 0x02, 0x04, 0x08, 0x10, 0x20, 0x40, 0x80, 0x1b, 0x36,
	};
	unsigned i;

	for (i = 0; i < 4; i++) {
		sched[i] = ((uint32_t)key[4 * i] << 24) |
			   ((uint32_t)key[4 * i + 1] << 16) |
			   ((uint32_t)key[4 * i + 2] << 8) |
			   ((uint32_t)key[4 * i + 3]);
	}
	for (i = 4; i < AES128_WORDS; i++) {
		uint32_t t = sched[i - 1];

		if ((i & 3u) == 0u) {
			t = sub_word(rotl32(t, 8)) ^
			    ((uint32_t)rcon[(i / 4u) - 1u] << 24);
		}
		sched[i] = sched[i - 4] ^ t;
	}
}

static int select_key(const struct radiant_sec_key *k)
{
	if (k == NULL || k->bits != 128u || k->serial == 0u) {
		return RADIANT_SEC_ENOKEY;
	}
	if (sched_serial != k->serial) {
		key_expand(k->b);
		sched_serial = k->serial;
	}
	return RADIANT_SEC_OK;
}

/* One block, under whatever key select_key() last installed. */
static void encrypt_block(const uint8_t in[16], uint8_t out[16])
{
	uint32_t c[4];
	uint32_t n[4];
	unsigned round;
	unsigned j;

	for (j = 0; j < 4; j++) {
		c[j] = ((uint32_t)in[4 * j] << 24) |
		       ((uint32_t)in[4 * j + 1] << 16) |
		       ((uint32_t)in[4 * j + 2] << 8) |
		       ((uint32_t)in[4 * j + 3]);
		c[j] ^= sched[j];
	}

	for (round = 1; round <= AES128_ROUNDS; round++) {
		for (j = 0; j < 4; j++) {
			c[j] = sub_word(c[j]);
		}

		/* ShiftRows. Row r of the state is the byte at shift 24-8r of
		 * every column word, and row r rotates left by r - so column j
		 * takes its row-r byte from column (j+r) mod 4. */
		for (j = 0; j < 4; j++) {
			n[j] = (c[j] & 0xff000000u) |
			       (c[(j + 1) & 3u] & 0x00ff0000u) |
			       (c[(j + 2) & 3u] & 0x0000ff00u) |
			       (c[(j + 3) & 3u] & 0x000000ffu);
		}

		if (round != AES128_ROUNDS) {
			for (j = 0; j < 4; j++) {
				c[j] = mix_column(n[j]) ^ sched[4 * round + j];
			}
		} else {
			for (j = 0; j < 4; j++) {
				c[j] = n[j] ^ sched[4 * round + j];
			}
		}
	}

	for (j = 0; j < 4; j++) {
		out[4 * j]     = (uint8_t)(c[j] >> 24);
		out[4 * j + 1] = (uint8_t)(c[j] >> 16);
		out[4 * j + 2] = (uint8_t)(c[j] >> 8);
		out[4 * j + 3] = (uint8_t)c[j];
	}
}

/* ── Key handling ───────────────────────────────────────────────────────── */

int radiant_sec_key_import(struct radiant_sec_key *k, const uint8_t *raw,
			   size_t bits)
{
	if (k == NULL || raw == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	if (bits != 128u) {
		/* The seam expresses 256; this backend and the protocol are
		 * both AES-128. Refuse loudly rather than truncate silently. */
		return RADIANT_SEC_ENOTSUP;
	}

	memset(k, 0, sizeof(*k));
	memcpy(k->b, raw, 16);
	k->bits = 128u;

	import_serial++;
	if (import_serial == 0u) {
		/* Serial 0 means "empty"; a wrap that reused it would make an
		 * empty handle look valid. */
		import_serial = 1u;
		sched_serial = 0u;
	}
	k->serial = import_serial;
	return RADIANT_SEC_OK;
}

void radiant_sec_key_destroy(struct radiant_sec_key *k)
{
	if (k == NULL) {
		return;
	}
	if (sched_serial == k->serial) {
		/* Do not leave a live schedule for a destroyed key. */
		memset(sched, 0, sizeof(sched));
		sched_serial = 0u;
	}
	memset(k, 0, sizeof(*k));
}

/* ── Level 1: the block primitive. For backends, not for policy. ─────────── */

int radiant_sec_aes_ecb(const struct radiant_sec_key *k, const uint8_t in[16],
			uint8_t out[16])
{
	int rc = select_key(k);

	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	if (in == NULL || out == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	encrypt_block(in, out);
	return RADIANT_SEC_OK;
}

/*
 * Level 2: the portable mode layer. Compiled only when the selected backend
 * doesn't implement the modes itself - a mode-level backend (CRACEN via PSA,
 * CC310) defines these and its own radiant_sec_caps(), and this section
 * disappears.
 */
#ifndef CONFIG_RADIANT_SEC_BACKEND_MODES

static const struct radiant_sec_caps sw_caps = {
	.name          = "software",
	.has_ctr       = false,   /* the portable layer provides it over ECB */
	.has_cmac      = false,
	/* Tracks the symbol for src/ext/radiant_x25519.c (a separate file),
	 * not this backend: "not built" and "this backend cannot" are the
	 * same condition to a caller. */
#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
	.has_x25519    = true,
#else
	.has_x25519    = false,
#endif
	.key_is_opaque = false,
	/* Same reasoning as has_x25519, tracking the entropy backend's
	 * symbol. Plain #ifdef since this file includes no Zephyr header
	 * (IS_ENABLED() lives in one). */
#if defined(CONFIG_RADIANT_SEC_HAS_RNG)
	.has_rng       = true,
#else
	.has_rng       = false,
#endif
	.key_bits_mask = RADIANT_SEC_KEY_BITS_128,
	.block_us      = 12,      /* M4 at 64 MHz, estimated, not measured */
};

const struct radiant_sec_caps *radiant_sec_caps(void)
{
	return &sw_caps;
}

int radiant_sec_ctr_xor(const struct radiant_sec_key *k, const uint8_t iv[16],
			uint8_t *buf, size_t len)
{
	uint8_t counter[16];
	uint8_t stream[16];
	int rc;

	if (iv == NULL || (buf == NULL && len != 0u)) {
		return RADIANT_SEC_EINVAL;
	}
	rc = select_key(k);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}

	memcpy(counter, iv, 16);

	while (len != 0u) {
		size_t chunk = (len < 16u) ? len : 16u;
		size_t i;

		encrypt_block(counter, stream);
		for (i = 0; i < chunk; i++) {
			buf[i] ^= stream[i];
		}
		buf += chunk;
		len -= chunk;

		if (len != 0u) {
			/* 128-bit big-endian increment, SP 800-38A F.5. */
			for (i = 16; i-- > 0;) {
				counter[i]++;
				if (counter[i] != 0u) {
					break;
				}
			}
		}
	}

	memset(stream, 0, sizeof(stream));
	return RADIANT_SEC_OK;
}

/* RFC 4493 dbl(): left shift by one, and xor 0x87 back in on carry. */
static void cmac_dbl(uint8_t v[16])
{
	uint8_t carry = (uint8_t)(v[0] >> 7);
	unsigned i;

	for (i = 0; i < 15; i++) {
		v[i] = (uint8_t)((v[i] << 1) | (v[i + 1] >> 7));
	}
	v[15] = (uint8_t)(v[15] << 1);
	v[15] ^= (uint8_t)(0x87u * carry);
}

static void cmac_subkeys(uint8_t k1[16], uint8_t k2[16])
{
	static const uint8_t zero[16] = { 0 };

	encrypt_block(zero, k1);
	cmac_dbl(k1);
	memcpy(k2, k1, 16);
	cmac_dbl(k2);
}

int radiant_sec_cmac_init(struct radiant_sec_cmac_ctx *c,
			  const struct radiant_sec_key *k)
{
	if (c == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	if (k == NULL || k->bits != 128u || k->serial == 0u) {
		return RADIANT_SEC_ENOKEY;
	}
	memset(c, 0, sizeof(*c));
	c->key = k;
	c->started = 1u;
	return RADIANT_SEC_OK;
}

int radiant_sec_cmac_update(struct radiant_sec_cmac_ctx *c, const uint8_t *msg,
			    size_t len)
{
	int rc;

	if (c == NULL || c->started == 0u) {
		return RADIANT_SEC_EINVAL;
	}
	if (len == 0u) {
		return RADIANT_SEC_OK;
	}
	if (msg == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	rc = select_key(c->key);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}

	while (len != 0u) {
		size_t room = 16u - c->partial_len;
		size_t take = (len < room) ? len : room;
		unsigned i;

		memcpy(c->partial + c->partial_len, msg, take);
		c->partial_len = (uint8_t)(c->partial_len + take);
		msg += take;
		len -= take;

		/* A full block is not processed until more data arrives:
		 * CMAC's last block takes a different subkey, and whether
		 * this is the last block isn't known until the next update or
		 * final. Processing eagerly is the classic incremental-CMAC
		 * bug - wrong only for messages whose length is a multiple
		 * of 16. */
		if (c->partial_len == 16u && len != 0u) {
			for (i = 0; i < 16; i++) {
				c->chain[i] ^= c->partial[i];
			}
			encrypt_block(c->chain, c->chain);
			c->partial_len = 0u;
		}
	}
	return RADIANT_SEC_OK;
}

int radiant_sec_cmac_final(struct radiant_sec_cmac_ctx *c, uint8_t tag[16])
{
	uint8_t k1[16];
	uint8_t k2[16];
	uint8_t last[16];
	unsigned i;
	int rc;

	if (c == NULL || c->started == 0u || tag == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	rc = select_key(c->key);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	cmac_subkeys(k1, k2);

	memcpy(last, c->partial, c->partial_len);
	if (c->partial_len == 16u) {
		for (i = 0; i < 16; i++) {
			last[i] ^= k1[i];
		}
	} else {
		last[c->partial_len] = 0x80u;
		for (i = (unsigned)c->partial_len + 1u; i < 16u; i++) {
			last[i] = 0u;
		}
		for (i = 0; i < 16; i++) {
			last[i] ^= k2[i];
		}
	}

	for (i = 0; i < 16; i++) {
		last[i] ^= c->chain[i];
	}
	encrypt_block(last, tag);

	memset(c, 0, sizeof(*c));
	memset(k1, 0, sizeof(k1));
	memset(k2, 0, sizeof(k2));
	memset(last, 0, sizeof(last));
	return RADIANT_SEC_OK;
}

int radiant_sec_cmac(const struct radiant_sec_key *k, const uint8_t *msg,
		     size_t len, uint8_t tag[16])
{
	struct radiant_sec_cmac_ctx c;
	int rc = radiant_sec_cmac_init(&c, k);

	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	rc = radiant_sec_cmac_update(&c, msg, len);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}
	return radiant_sec_cmac_final(&c, tag);
}

#endif /* !CONFIG_RADIANT_SEC_BACKEND_MODES */

/*
 * Portable helpers on top of the seam. Compiled for every backend - two
 * backends disagreeing about a block this protocol pins to the byte is
 * exactly the failure the pinning exists to prevent. Call Level 2, never
 * Level 1.
 */

void radiant_sec_nonce_block(uint8_t out[16], uint32_t epoch, uint16_t devnum,
			     uint16_t counter, uint8_t dom)
{
	if (out == NULL) {
		return;
	}
	out[0] = (uint8_t)epoch;
	out[1] = (uint8_t)(epoch >> 8);
	out[2] = (uint8_t)(epoch >> 16);
	out[3] = (uint8_t)(epoch >> 24);
	out[4] = (uint8_t)devnum;
	out[5] = (uint8_t)(devnum >> 8);
	out[6] = (uint8_t)counter;
	out[7] = (uint8_t)(counter >> 8);
	out[8] = dom;
	memset(out + 9, 0, 7);
}

int radiant_sec_kdf(const struct radiant_sec_key *root, const char *label,
		    uint32_t epoch, uint16_t base_devnum,
		    struct radiant_sec_key *out)
{
	/* 1 counter + up to 4 label + 1 separator + 4 epoch + 2 devnum
	 * + 2 length. */
	uint8_t block[16];
	uint8_t tag[16];
	size_t label_len;
	size_t n = 0;
	int rc;

	if (label == NULL || out == NULL) {
		return RADIANT_SEC_EINVAL;
	}
	label_len = strlen(label);
	if (label_len == 0u || label_len > 4u) {
		return RADIANT_SEC_EINVAL;
	}

	block[n++] = 0x01u;                       /* SP 800-108 counter i = 1 */
	memcpy(block + n, label, label_len);
	n += label_len;
	block[n++] = 0x00u;                       /* the separator */
	block[n++] = (uint8_t)epoch;
	block[n++] = (uint8_t)(epoch >> 8);
	block[n++] = (uint8_t)(epoch >> 16);
	block[n++] = (uint8_t)(epoch >> 24);
	block[n++] = (uint8_t)base_devnum;
	block[n++] = (uint8_t)(base_devnum >> 8);
	block[n++] = 0x00u;                       /* [L]_2 = 128 bits, BE */
	block[n++] = 0x80u;

	rc = radiant_sec_cmac(root, block, n, tag);
	if (rc != RADIANT_SEC_OK) {
		return rc;
	}

	rc = radiant_sec_key_import(out, tag, 128u);
	memset(tag, 0, sizeof(tag));
	memset(block, 0, sizeof(block));
	return rc;
}
