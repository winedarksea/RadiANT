/* SPDX-License-Identifier: Apache-2.0 */
/*
 * X25519, from RFC 7748.
 *
 * Provenance: clean-room. Written from RFC 7748 sections 4.1 and 5 - the
 * curve parameters, the scalar clamping, the Montgomery ladder step and the
 * decodeUCoordinate rule - and verified against the test vectors in RFC 7748
 * section 5.2 and the Diffie-Hellman vector in section 6.1. No third-party
 * implementation was consulted or copied.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS WRITTEN RATHER THAN VENDORED
 * ---------------------------------------------------------------------------
 * The plan for this phase said "vendored public-domain X25519 with a
 * Provenance: line". Vendoring is normally the right call for a primitive this
 * well-studied - fewer eyes on new crypto code is strictly worse - and the
 * intent behind that instruction is preserved here: one entry point, one file,
 * displaceable wholesale by a PKA backend.
 *
 * What is NOT preserved is the word "vendored", and the difference matters
 * enough to write down. A vendored file's value is its provenance: it is the
 * bytes that thousands of deployments audited, and the Provenance: line is a
 * verifiable claim about where they came from. A file reconstructed from
 * memory and labelled "vendored public-domain" would carry a provenance claim
 * nobody can check, in a repository whose entire clean-room posture (ADR 0002)
 * rests on those claims being accurate. That is a worse outcome than an honest
 * clean-room implementation, so this is the honest one.
 *
 * If a real vendored implementation is wanted later, it drops in behind
 * radiant_sec_x25519() and nothing above this file changes. That is the point
 * of the seam.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS AND IS NOT CONSTANT TIME
 * ---------------------------------------------------------------------------
 * The ladder is constant time in the secret scalar: every bit runs the same
 * sequence of field operations, and the conditional swap is arithmetic rather
 * than branching. Field multiplication is schoolbook with no data-dependent
 * branches or table lookups.
 *
 * NOT claimed: resistance to power or electromagnetic analysis, which is a
 * different threat model and needs hardware this does not have. A node whose
 * physical security matters should use a PKA backend, and the caps bit exists
 * so it can.
 *
 * ---------------------------------------------------------------------------
 * REPRESENTATION
 * ---------------------------------------------------------------------------
 * A field element is 16 signed 64-bit limbs of 16 bits each, little-endian,
 * value = sum(limb[i] * 2^(16i)). Limbs are signed and allowed to run wide
 * between carries, which is what makes subtraction free of borrows. 16-bit
 * limbs in 64-bit accumulators leave enormous headroom: a schoolbook product
 * accumulates at most 16 terms of 2^32, then folds with a factor of 38, which
 * reaches about 2^41 - twenty-two bits below overflow.
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
 * Swap a and b when `swap` is 1, leave them alone when it is 0, in the same
 * instructions either way.
 *
 * The mask is built by negating the flag, so 1 becomes all-ones and 0 becomes
 * all-zeros; the XOR dance then either exchanges the limbs or exchanges them
 * with zero. Nothing branches on `swap`, which is the whole requirement - the
 * ladder calls this once per bit of the secret scalar, so a branch here would
 * leak the scalar through timing.
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
 * One carry pass, bringing every limb back to 16 bits.
 *
 * The shift is arithmetic and therefore floors, which is exactly right for a
 * signed limb: a negative limb borrows from the next one up, and the borrow is
 * the floored quotient. The top limb's overflow wraps into limb 0 multiplied by
 * 38, because 2^255 = 19 (mod p) and therefore 2^256 = 38.
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
 * Modular inverse by Fermat: a^(p-2) mod p.
 *
 * p - 2 = 2^255 - 21, whose binary expansion is all ones except bits 2 and 4.
 * So: square 255 times, multiplying in `a` at every step except those two.
 * 254 squarings and 252 multiplications, and it is constant time because the
 * exponent is a constant.
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
 * decodeUCoordinate, RFC 7748 section 5: little-endian, and the most
 * significant bit of the last byte is MASKED OFF rather than rejected.
 *
 * That masking is normative and easy to skip. Curve25519 u-coordinates are
 * 255 bits, so bit 255 carries no information - but implementations exist that
 * set it, and a receiver that treated the value as 256 bits would compute a
 * different shared secret from its peer and fail to agree, silently.
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
 * Freeze into the canonical range [0, p) and serialise little-endian.
 *
 * After carrying, a value can still be anywhere in [0, 2p): the limbs are in
 * range but the number may be at or above p. Subtracting p conditionally twice
 * settles it. The subtraction is computed unconditionally into a scratch
 * element and selected with the same constant-time swap the ladder uses, so
 * nothing branches on the value.
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
	 * makes the scalar a multiple of the cofactor, which is what kills
	 * small-subgroup contributions; setting bit 254 and clearing bit 255
	 * fixes the ladder's length so the loop below is constant time.
	 *
	 * Done on a copy: the caller's scalar is theirs, and a function that
	 * silently rewrote it would corrupt a host's stored key.
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
	 * The Montgomery ladder, RFC 7748 section 5. Bit 255 is always 0 after
	 * clamping, so the loop starts at 254.
	 *
	 * The swap is deferred rather than performed twice per bit: `swap`
	 * holds whether the two points are currently exchanged, and the XOR
	 * makes each iteration swap only when the bit differs from the last
	 * one. One conditional swap per bit instead of two, with the same
	 * result and the same timing.
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

	/*
	 * Wipe the clamped scalar and every intermediate. The ladder's working
	 * state is as sensitive as the scalar itself - x2/z2 at any point in
	 * the loop plus the remaining bits would reconstruct the secret.
	 */
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
 * secret carries no secrecy at all. RFC 7748 section 6.1 leaves rejecting it
 * optional for Diffie-Hellman; here it is mandatory, because the result is fed
 * straight into a KDF and installed as a root key. Accepting it would let any
 * passive attacker who can inject one packet fix the group key to a value they
 * know.
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
