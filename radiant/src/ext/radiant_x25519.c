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
 * sequence and multiplication is schoolbook with no data-dependent
 * branches/lookups. The conditional swap is the one place C semantics do NOT
 * hold the line: masked-XOR source is no promise, and GCC 14.3 at -Os and -O2
 * turned the old version back into a branch on the swap flag (security review
 * 2026-11-20, item 1). fe_cswap() now hides its mask behind a compiler value
 * barrier. scripts/check_x25519_ct.py disassembles the result - on the
 * release pairing image (the production check), and compiled standalone over
 * a compiler, -O level and core matrix on every push and locally - and shows
 * two things only: fe_cswap is branch-free apart from a loop edge whose exit
 * test does not visibly read the flag, and the ladder swaps only through it.
 * Everything else - the field arithmetic, memory access, the loop's trip
 * count, multiplier timing - rests on review of straight-line code, and the
 * claim covers Cortex-M4/M33 only (M3's umull exits early). NOT claimed:
 * resistance to power/EM analysis - a node needing that should use a PKA
 * backend.
 *
 * Representation: a field element is 16 signed 64-bit limbs of 16 bits each,
 * little-endian, value = sum(limb[i] * 2^(16i)). Limbs are signed and run
 * wide between carries (borrow-free subtraction); a schoolbook product
 * accumulates at most 16 terms of 2^32, folds by 38, reaching ~2^41 - 22 bits
 * below overflow in the 64-bit accumulator.
 */

#include <stdint.h>
#include <string.h>

#include <radiant/radiant_sec.h>

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
 * Wipe limbs the optimiser may not delete, as radiant_sec_wipe() does for
 * bytes. Limb-wide rather than byte-wide because fe_mul() calls it on every
 * product, about 2,800 times per exchange: eight times fewer stores than the
 * byte loop for the same 248 bytes.
 */
static void fe_wipe(int64_t *p, int n)
{
	volatile int64_t *v = p;

	while (n-- > 0) {
		*v++ = 0;
	}
}

/*
 * Swap a and b when `swap` is 1, in the same instructions either way. Mask
 * is the negated flag (all-ones or all-zeros); nothing may branch on `swap`,
 * because the ladder calls this once per scalar bit and a branch would leak
 * the scalar through timing.
 *
 * The C alone does not guarantee that. Every caller passes 0 or 1, GCC's
 * interprocedural value ranges carry that into this body, and once the mask
 * is provably 0 or -1 the masked XOR is just a select: GCC 14.3 at -Os
 * emitted `cmp ip,#0 / beq` inside the limb loop (security review
 * 2026-11-20, item 1). The empty asm below takes the mask in a register and
 * hands back a value the compiler must treat as unknown, so there is no
 * range left to turn into a select. The volatile round trip is the fallback
 * for a compiler without GNU asm; a volatile read is equally opaque, just one
 * load and store dearer.
 *
 * noinline keeps fe_cswap a symbol of its own in the ELF, which is what
 * scripts/check_x25519_ct.py disassembles (`objdump --disassemble=fe_cswap`)
 * to prove no branch came back.
 */
#if defined(__GNUC__) || defined(__clang__)
__attribute__((noinline))
#endif
static void fe_cswap(fe a, fe b, int64_t swap)
{
	uint32_t m = 0u - (uint32_t)swap;
	int64_t  mask;
	int64_t  t;
	int      i;

#if defined(__GNUC__) || defined(__clang__)
	__asm__("" : "+r"(m));
#else
	{
		volatile uint32_t vm = m;

		m = vm;
	}
#endif
	/* Sign-extend: 0 -> 0, 0xFFFFFFFF -> all 64 bits set. */
	mask = (int64_t)(int32_t)m;

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
 *
 * What stays in the limb is its low 16 bits, taken with a mask. It used to
 * be `o[i] -= carry << 16`, and a borrow makes `carry` negative: a left
 * shift of a negative value is undefined in ISO C (review 2026-11-14,
 * item 4). The mask is the same value - the floored shift leaves exactly
 * the residue mod 2^16 behind - and it is defined: int64_t is two's
 * complement by definition (C11 7.20.1.1). It is also what GCC already
 * compiled the shift to; `o[i] -= carry * 65536` was measured at 3.6 %
 * more instructions per exchange on Cortex-M4 at -Os, since GCC does not
 * fold the multiply into the mask. The right shift is implementation-
 * defined, not undefined; GCC and Clang both define it as arithmetic.
 */
static void fe_carry(fe o)
{
	int64_t carry;
	int     i;

	for (i = 0; i < FE_LIMBS - 1; i++) {
		carry = o[i] >> 16;
		o[i] &= 0xFFFF;
		o[i + 1] += carry;
	}
	carry = o[FE_LIMBS - 1] >> 16;
	o[FE_LIMBS - 1] &= 0xFFFF;
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
	/* The product of two secrets, in the frame every later multiply
	 * reuses but no caller can reach (review 2026-11-14, item 4). */
	fe_wipe(t, 2 * FE_LIMBS - 1);
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
 *
 * Works in `o` directly, so `o` MUST NOT be `a`: `a` is read on every step.
 * (It had a scratch element of its own; the caller's spare one is cheaper
 * than 128 B more stack - security review 2026-11-17, item 1.)
 */
static void fe_invert(fe o, const fe a)
{
	int i;

	fe_copy(o, a);
	for (i = 253; i >= 0; i--) {
		fe_sq(o, o);
		if (i != 2 && i != 4) {
			fe_mul(o, o, a);
		}
	}
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
 *
 * Freezes `t` in place and uses the caller's `m` as scratch (both were
 * locals here until security review 2026-11-17, item 1); the caller wipes
 * both, since `t` is the shared secret itself and `m` it or it minus p.
 */
static void fe_pack(uint8_t *out, fe t, fe m)
{
	int64_t borrow;
	int     i, pass;

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

/*
 * The ladder's working state: seven field elements, 896 B, in .bss rather
 * than on the caller's stack. On the stack they and the step's ten
 * temporaries made a 2,248 B frame, which overran the dongle's 2,048 B serial
 * bridge thread that runs 0xF5, and on nRF52840 stepped clean over the 32 B
 * MPU stack guard instead of tripping it (security review 2026-11-17,
 * item 1). The frame is now under 100 B; fe_mul()'s 248 B product is the
 * deepest thing left.
 *
 * NOT RE-ENTRANT - radiant serialises every call with api_lock, and the entry
 * point asserts it (RADIANT_SEC_ASSERT_LOCKED) in a lock-checked build, as
 * radiant_sec_aes.c does for the shared key schedule. Only built with
 * CONFIG_RADIANT_SEC_PAIRING_X25519, so no other image pays for the RAM.
 */
static struct {
	fe x1, x2, z2, x3, z3;
	fe t0, t1;
} ws;

int radiant_sec_x25519(uint8_t *out, const uint8_t *scalar, const uint8_t *point)
{
	RADIANT_SEC_ASSERT_LOCKED();
	uint8_t k[RADIANT_SEC_X25519_BYTES];
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

	fe_unpack(ws.x1, point);
	fe_one(ws.x2);
	fe_zero(ws.z2);
	fe_copy(ws.x3, ws.x1);
	fe_one(ws.z3);

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
		fe_cswap(ws.x2, ws.x3, swap);
		fe_cswap(ws.z2, ws.z3, swap);
		swap = bit;

		/*
		 * RFC 7748's step, A = x2 + z2 through z2 = E * (AA + a24 * E),
		 * in two temporaries instead of ten: each of x2, z2, x3, z3 is
		 * overwritten once its last reader has run. Same field
		 * operations on the same operands in the same order as the
		 * RFC's list, which the section 5.2 and 6.1 vectors in
		 * test_sec_x25519.c pin. Aliasing is safe because fe_add() and
		 * fe_sub() are limb-wise and fe_mul() writes its output only
		 * after reading both inputs. The name each element holds is on
		 * its right.
		 */
		fe_add(ws.t0, ws.x2, ws.z2);         /* t0 = A  = x2 + z2      */
		fe_sub(ws.t1, ws.x2, ws.z2);         /* t1 = B  = x2 - z2      */
		fe_add(ws.x2, ws.x3, ws.z3);         /* x2 = C  = x3 + z3      */
		fe_sub(ws.z2, ws.x3, ws.z3);         /* z2 = D  = x3 - z3      */
		fe_mul(ws.z2, ws.z2, ws.t0);         /* z2 = DA = D * A        */
		fe_mul(ws.x2, ws.x2, ws.t1);         /* x2 = CB = C * B        */
		fe_sq(ws.t0, ws.t0);                 /* t0 = AA = A^2          */
		fe_sq(ws.t1, ws.t1);                 /* t1 = BB = B^2          */

		fe_add(ws.x3, ws.z2, ws.x2);         /* x3 = DA + CB           */
		fe_sq(ws.x3, ws.x3);                 /* x3 = (DA + CB)^2       */
		fe_sub(ws.z3, ws.z2, ws.x2);         /* z3 = DA - CB           */
		fe_sq(ws.z3, ws.z3);                 /* z3 = (DA - CB)^2       */
		fe_mul(ws.z3, ws.x1, ws.z3);         /* z3 = x1 * (DA - CB)^2  */

		fe_sub(ws.z2, ws.t0, ws.t1);         /* z2 = E  = AA - BB      */
		fe_mul(ws.x2, ws.t0, ws.t1);         /* x2 = AA * BB           */
		fe_mul_small(ws.t1, ws.z2, A24);     /* t1 = a24 * E           */
		fe_add(ws.t1, ws.t0, ws.t1);         /* t1 = AA + a24 * E      */
		fe_mul(ws.z2, ws.z2, ws.t1);         /* z2 = E * (AA + a24 * E) */
	}
	fe_cswap(ws.x2, ws.x3, swap);
	fe_cswap(ws.z2, ws.z3, swap);

	fe_invert(ws.t0, ws.z2);                     /* t0 = 1 / z2 */
	fe_mul(ws.x2, ws.x2, ws.t0);
	fe_pack(out, ws.x2, ws.t1);

	/* Wipe the clamped scalar and the whole workspace: the ladder's
	 * working state is as sensitive as the scalar itself, and after
	 * fe_pack() x2 is the shared secret and t1 it or it minus p.
	 * fe_mul() still wipes its own product scratch (review 2026-11-14,
	 * item 4). Not covered: scalar locals (the swap flag, a carry), which
	 * live in registers or spill slots C cannot name. radiant_sec_wipe(),
	 * not memset(): `k` is a dead local, and a plain memset() on a dead
	 * local is a store GCC deletes at -Os. */
	radiant_sec_wipe(k, sizeof(k));
	radiant_sec_wipe(&ws, sizeof(ws));

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
