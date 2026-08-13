/* SPDX-License-Identifier: Apache-2.0 */
/*
 * X25519, from RFC 7748.
 *
 * Clean-room: written from RFC 7748 sections 4.1 and 5 (curve parameters,
 * scalar clamping, Montgomery ladder step, decodeUCoordinate), verified
 * against the test vectors in sections 5.2 and 6.1. No third-party
 * implementation consulted.
 *
 * Written rather than vendored: a reconstructed-from-memory file labelled
 * "vendored public-domain" would carry a provenance claim nobody can check,
 * which this repo's clean-room posture (ADR 0002) can't rest on. One entry
 * point, one file - a real vendored implementation or a PKA backend can drop
 * in behind radiant_sec_x25519() later without anything above it changing.
 *
 * Constant time in the secret scalar: every bit runs the same field-op
 * sequence, conditional swap is arithmetic not branching, multiplication is
 * schoolbook with no data-dependent branches/lookups. NOT claimed: resistance
 * to power/EM analysis - a node needing that should use a PKA backend.
 *
 * Representation: a field element is 16 signed 64-bit limbs of 16 bits each,
 * little-endian, value = sum(limb[i] * 2^(16i)). Limbs are signed and run
 * wide between carries (borrow-free subtraction); a schoolbook product
 * accumulates at most 16 terms of 2^32, folds by 38, reaching ~2^41 - 22 bits
 * below overflow in the 64-bit accumulator.
 */

#include <stdint.h>
#include <string.h>

#include <radiant_core/radiant_sec.h>

#define FE_LIMBS 16

typedef int64_t fe[FE_LIMBS];

/* 2^255 - 19, low limb, for the freeze step. */
#define P_LOW  0xFFEDu
#define P_MID  0xFFFFu
#define P_HIGH 0x7FFFu

/* (486662 - 2) / 4, the ladder constant RFC 7748 calls a24. */
#define A24 121665

const uint8_t radiant_sec_x25519_basepoint[RADIANT_SEC_X25519_BYTES] = {
	9, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
	0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0,
};

static void fe_zero(fe o)
{
	int i;

	for (i = 0; i < FE_LIMBS; i++) {
		o[i] = 0;
	}
}

static void fe_one(fe o)
{
	fe_zero(o);
	o[0] = 1;
}

static void fe_copy(fe o, const fe a)
{
	int i;

	for (i = 0; i < FE_LIMBS; i++) {
		o[i] = a[i];
	}
}

/*
 * Swap a and b when `swap` is 1, in the same instructions either way. Mask
 * is the negated flag (all-ones or all-zeros); nothing branches on `swap`,
 * because the ladder calls this once per scalar bit and a branch would leak
 * the scalar through timing.
 */
static void fe_cswap(fe a, fe b, int64_t swap)
{
	int64_t mask = -swap;
	int64_t t;
	int     i;

	for (i = 0; i < FE_LIMBS; i++) {
		t = mask & (a[i] ^ b[i]);
		a[i] ^= t;
		b[i] ^= t;
	}
}

static void fe_add(fe o, const fe a, const fe b)
{
	int i;

	for (i = 0; i < FE_LIMBS; i++) {
		o[i] = a[i] + b[i];
	}
}

static void fe_sub(fe o, const fe a, const fe b)
{
	int i;

	for (i = 0; i < FE_LIMBS; i++) {
		o[i] = a[i] - b[i];
	}
}

/*
 * One carry pass, bringing every limb back to 16 bits. The shift is
 * arithmetic (floors), which is correct for a signed limb's borrow. Top
 * limb's overflow wraps into limb 0 * 38, since 2^255 = 19 (mod p) so
 * 2^256 = 38.
 */
static void fe_carry(fe o)
{
	int64_t carry;
	int     i;

	for (i = 0; i < FE_LIMBS - 1; i++) {
		carry = o[i] >> 16;
		o[i] -= carry << 16;
		o[i + 1] += carry;
	}
	carry = o[FE_LIMBS - 1] >> 16;
	o[FE_LIMBS - 1] -= carry << 16;
	o[0] += 38 * carry;
}

static void fe_mul(fe o, const fe a, const fe b)
{
	int64_t t[2 * FE_LIMBS - 1];
	int     i, j;

	for (i = 0; i < 2 * FE_LIMBS - 1; i++) {
		t[i] = 0;
	}
	for (i = 0; i < FE_LIMBS; i++) {
		for (j = 0; j < FE_LIMBS; j++) {
			t[i + j] += a[i] * b[j];
		}
	}
	/* Fold the top half down: limb i+16 carries weight 2^(256+16i), and
	 * 2^256 = 38 (mod p). */
	for (i = 0; i < FE_LIMBS - 1; i++) {
		t[i] += 38 * t[i + FE_LIMBS];
	}
	for (i = 0; i < FE_LIMBS; i++) {
		o[i] = t[i];
	}
	/* Twice: the first pass can leave limb 0 wide after the 38x fold. */
	fe_carry(o);
	fe_carry(o);
}

static void fe_sq(fe o, const fe a)
{
	fe_mul(o, a, a);
}

static void fe_mul_small(fe o, const fe a, int64_t n)
{
	int i;

	for (i = 0; i < FE_LIMBS; i++) {
		o[i] = a[i] * n;
	}
	fe_carry(o);
	fe_carry(o);
}

/*
 * Modular inverse by Fermat: a^(p-2) mod p. p - 2 = 2^255 - 21, all-ones
 * except bits 2 and 4, so: square 255 times, multiply in `a` except at those
 * two steps. Constant time since the exponent is a constant.
 */
static void fe_invert(fe o, const fe a)
{
	fe  c;
	int i;

	fe_copy(c, a);
	for (i = 253; i >= 0; i--) {
		fe_sq(c, c);
		if (i != 2 && i != 4) {
			fe_mul(c, c, a);
		}
	}
	fe_copy(o, c);
}

/*
 * decodeUCoordinate, RFC 7748 section 5: little-endian, most significant
 * bit of the last byte MASKED OFF rather than rejected (normative). Some
 * peers set that bit; treating it as 256 bits would silently disagree with
 * such a peer on the shared secret.
 */
static void fe_unpack(fe o, const uint8_t *in)
{
	int i;

	for (i = 0; i < FE_LIMBS; i++) {
		o[i] = (int64_t)in[2 * i] | ((int64_t)in[2 * i + 1] << 8);
	}
	o[FE_LIMBS - 1] &= 0x7FFF;
}

/*
 * Freeze into [0, p) and serialise little-endian. After carrying the value
 * can still be anywhere in [0, 2p), so subtract p conditionally, twice.
 * Subtraction is computed unconditionally into scratch and selected with the
 * same constant-time swap the ladder uses.
 */
static void fe_pack(uint8_t *out, const fe n)
{
	fe      t;
	fe      m;
	int64_t borrow;
	int     i, pass;

	fe_copy(t, n);
	fe_carry(t);
	fe_carry(t);
	fe_carry(t);

	for (pass = 0; pass < 2; pass++) {
		m[0] = t[0] - P_LOW;
		for (i = 1; i < FE_LIMBS - 1; i++) {
			m[i] = t[i] - P_MID - ((m[i - 1] >> 16) & 1);
			m[i - 1] &= 0xFFFF;
		}
		m[FE_LIMBS - 1] = t[FE_LIMBS - 1] - P_HIGH -
				  ((m[FE_LIMBS - 2] >> 16) & 1);
		borrow = (m[FE_LIMBS - 1] >> 16) & 1;
		m[FE_LIMBS - 2] &= 0xFFFF;
		/* borrow == 1 means t < p, so keep t; otherwise take t - p. */
		fe_cswap(t, m, 1 - borrow);
	}

	for (i = 0; i < FE_LIMBS; i++) {
		out[2 * i] = (uint8_t)(t[i] & 0xFF);
		out[2 * i + 1] = (uint8_t)((t[i] >> 8) & 0xFF);
	}
}

int radiant_sec_x25519(uint8_t *out, const uint8_t *scalar, const uint8_t *point)
{
	uint8_t k[RADIANT_SEC_X25519_BYTES];
	fe      x1, x2, z2, x3, z3;
	fe      a, aa, b, bb, e, c, d, da, cb, t;
	int64_t swap = 0;
	int64_t bit;
	int     i;

	if (out == NULL || scalar == NULL || point == NULL) {
		return RADIANT_SEC_EINVAL;
	}

	/*
	 * decodeScalar25519, RFC 7748 section 5. Clearing the low three bits
	 * kills small-subgroup contributions (cofactor multiple); setting bit
	 * 254 and clearing bit 255 fixes the ladder length for constant time.
	 * Done on a copy so the caller's stored key isn't rewritten.
	 */
	memcpy(k, scalar, RADIANT_SEC_X25519_BYTES);
	k[0] &= 248u;
	k[31] &= 127u;
	k[31] |= 64u;

	fe_unpack(x1, point);
	fe_one(x2);
	fe_zero(z2);
	fe_copy(x3, x1);
	fe_one(z3);

	/*
	 * The Montgomery ladder, RFC 7748 section 5; loop starts at bit 254
	 * (bit 255 is always 0 after clamping). Swap is deferred rather than
	 * done twice per bit: `swap` tracks whether the points are currently
	 * exchanged, XORed with each bit so a swap only happens when the bit
	 * changes - one conditional swap per bit instead of two.
	 */
	for (i = 254; i >= 0; i--) {
		bit = (int64_t)((k[i >> 3] >> (i & 7)) & 1u);
		swap ^= bit;
		fe_cswap(x2, x3, swap);
		fe_cswap(z2, z3, swap);
		swap = bit;

		fe_add(a, x2, z2);
		fe_sq(aa, a);
		fe_sub(b, x2, z2);
		fe_sq(bb, b);
		fe_sub(e, aa, bb);
		fe_add(c, x3, z3);
		fe_sub(d, x3, z3);
		fe_mul(da, d, a);
		fe_mul(cb, c, b);

		fe_add(t, da, cb);
		fe_sq(x3, t);
		fe_sub(t, da, cb);
		fe_sq(t, t);
		fe_mul(z3, x1, t);

		fe_mul(x2, aa, bb);
		fe_mul_small(t, e, A24);
		fe_add(t, aa, t);
		fe_mul(z2, e, t);
	}
	fe_cswap(x2, x3, swap);
	fe_cswap(z2, z3, swap);

	fe_invert(z2, z2);
	fe_mul(x2, x2, z2);
	fe_pack(out, x2);

	/* Wipe the clamped scalar and every intermediate: the ladder's working
	 * state is as sensitive as the scalar itself. */
	memset(k, 0, sizeof(k));
	memset(x1, 0, sizeof(x1));
	memset(x2, 0, sizeof(x2));
	memset(z2, 0, sizeof(z2));
	memset(x3, 0, sizeof(x3));
	memset(z3, 0, sizeof(z3));
	memset(a, 0, sizeof(a));
	memset(aa, 0, sizeof(aa));
	memset(b, 0, sizeof(b));
	memset(bb, 0, sizeof(bb));
	memset(e, 0, sizeof(e));
	memset(c, 0, sizeof(c));
	memset(d, 0, sizeof(d));
	memset(da, 0, sizeof(da));
	memset(cb, 0, sizeof(cb));
	memset(t, 0, sizeof(t));

	return RADIANT_SEC_OK;
}

/*
 * An all-zero output means the peer sent a small-order point and the shared
 * secret carries no secrecy. RFC 7748 6.1 leaves rejecting it optional;
 * mandatory here since the result becomes a root key.
 */
bool radiant_sec_x25519_is_degenerate(const uint8_t *shared)
{
	uint8_t acc = 0;
	int     i;

	if (shared == NULL) {
		return true;
	}
	for (i = 0; i < RADIANT_SEC_X25519_BYTES; i++) {
		acc |= shared[i];
	}
	return acc == 0u;
}
