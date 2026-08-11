/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sec_compat - the two attestation tags a RadiANT node puts on an ANT+
 * device type, and nothing else.
 *
 * docs/radiant-security.md section 11.4 is normative;
 * docs/decisions/0008-antplus-additive-pages-and-compat-security.md pins the
 * bytes and records why there are two tiers rather than one.
 *
 * ── This file names no page and no field, deliberately ─────────────────────
 *
 * A compat channel exists to be read by a Garmin head unit, so its data pages
 * are ANT+ heart rate or ANT+ power, byte for byte. THAT IS NOT THIS MODULE'S
 * BUSINESS. What is here takes an epoch, a device number, a counter and some
 * opaque eight-byte messages, and returns a truncated CMAC. It contains no page
 * number and no field name, which is what makes it
 *
 *   - testable against synthetic payloads with no profile in the loop,
 *   - usable unchanged by a profile nobody has written yet, and
 *   - unable to grow an opinion about heart rate that a power meter then has to
 *     work around.
 *
 * Same discipline as "radiant_sec.c contains no call to radiant_sec_aes_ecb()",
 * and the `zero-cost` CI job greps for it in the same spirit. The page numbers,
 * the subtype nibble's position IN THE PAGE, the beacon and the SWITCH/RETURN
 * frames all live above this layer.
 *
 * ── Why two tiers, in one paragraph ────────────────────────────────────────
 *
 * Authenticating an identity and authenticating a data stream are two different
 * things with two different costs. Tier I answers "is this stream from the
 * holder of K_auth, now?" and covers NO PAYLOAD, so it is verifiable on receipt
 * and its verification rate equals its page delivery rate no matter how rarely
 * it is sent - which is what lets it be on by default at ~1.2% of slots. Tier II
 * answers "were these exact bytes sent by that holder?", costs one page in N,
 * and one lost packet unverifies the whole window - which is why it is off by
 * default. Neither is a degraded version of the other.
 *
 * ── Zero cost when off ─────────────────────────────────────────────────────
 *
 * CONFIG_RADIANT_SEC_COMPAT is default n and depends on CONFIG_RADIANT_SEC, and
 * the discipline is radiant_sec.h's, inherited unchanged: no ops struct, no
 * structs in hot-path signatures, no init call, and no-op static inlines below
 * when the symbol is off. The tag entry points refuse with
 * RADIANT_SEC_ENOTSUP rather than returning OK, because a disabled build that
 * answered OK would hand its caller a tag of stack garbage.
 */

#ifndef RADIANT_CORE_RADIANT_SEC_COMPAT_H_
#define RADIANT_CORE_RADIANT_SEC_COMPAT_H_

#include <stdint.h>
#include <stddef.h>

#include <radiant_core/radiant_sec.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The subtype, at nonce_block[9]. RADIANT_SEC_DOM_COMPAT_MAC (0x04) is in
 * radiant_sec.h beside the other three domain bytes, because that is the byte a
 * reader compares against them.
 *
 * THE SUBTYPE IS INSIDE THE MAC'D BLOCK AND NOT ONLY IN THE PAGE. Both tiers
 * ride one page number - the 7-bit page namespace is why - so the subtype is
 * the only thing separating a Tier I tag from a Tier II tag over the same
 * counter value, and a separator an attacker rewrites in a page byte separates
 * nothing.
 *
 * 0x03 is the SWITCH/RETURN announcement's, pinned here rather than left free
 * so the value cannot be spent twice before the layer that emits it exists.
 * Nothing in this module computes that tag today.
 */
#define RADIANT_SEC_COMPAT_SUBTYPE_TIER_I    0x01
#define RADIANT_SEC_COMPAT_SUBTYPE_TIER_II   0x02
#define RADIANT_SEC_COMPAT_SUBTYPE_ANNOUNCE  0x03

/*
 * 40 bits for Tier I and 48 for Tier II, and the difference is arithmetic
 * rather than a security judgement: Tier I carries a two-byte counter in its
 * page because no window index is implied, and 8 - 1 - 2 = 5 bytes are what is
 * left. 2^-40 per attempt, against a mechanism rate-limited to one attempt per
 * T by construction, is not the weak link in this system.
 */
#define RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES   5
#define RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES  6

/*
 * The Tier II window: N consecutive TRANSMITTED messages, N in {4, 8, 16, 32}.
 * Enumerated rather than ranged because N is simultaneously the airtime cost,
 * the verification latency and the DoS amplification factor, so an unenumerated
 * N is three surprises at once.
 */
#define RADIANT_SEC_COMPAT_N_MIN      4
#define RADIANT_SEC_COMPAT_N_MAX      32
#define RADIANT_SEC_COMPAT_N_DEFAULT  8

/* One transmitted ANT message is eight payload bytes, all of which Tier II
 * covers - including byte [0]. Unlike the spread tag, nothing rides in byte
 * [7] here: the compat tag has a page of its own. */
#define RADIANT_SEC_COMPAT_MSG_BYTES  8

#if defined(CONFIG_RADIANT_SEC_COMPAT)

/*
 * nonce_block = epoch[4 LE] || devnum[2 LE] || counter[2 LE]
 *               || RADIANT_SEC_DOM_COMPAT_MAC || sub || 0x00 x6
 *
 * Section 3.3's block EXTENDED at position 9, not duplicated: the domain byte
 * stays where every other block in this protocol has it and positions 10..15
 * stay zero, so a reader comparing this against radiant_sec_nonce_block() sees
 * one added byte rather than a second layout to keep in step.
 *
 * `counter` is the attestation counter for Tier I and the window index for
 * Tier II. Both are 16 bits here and both are carried truncated in the page -
 * two bytes for Tier I, one for Tier II - so a receiver reconstructs the high
 * bits from time exactly as it already does for the data counter.
 *
 * `sub` is a nibble in 1..15; out is left untouched for anything else, which is
 * the only refusal a void function can make and is why the tag entry points
 * below do not take a subtype at all.
 */
void radiant_sec_compat_nonce_block(uint8_t out[RADIANT_SEC_BLOCK_BYTES],
				    uint32_t epoch, uint16_t devnum,
				    uint16_t counter, uint8_t sub);

/*
 * Tier I - the identity attestation. trunc40( CMAC(K_auth, nonce_block) ).
 *
 * IT COVERS NO PAYLOAD, AND THAT IS THE POINT rather than a limitation. The
 * tag proves "this stream comes from the holder of K_auth, now, and is not a
 * replay" and nothing else, so it is verifiable the moment it arrives and a
 * packet lost anywhere else costs nothing. Replay is closed by `att_counter`,
 * which is monotone and derivable from elapsed time.
 *
 * Returns RADIANT_SEC_OK, RADIANT_SEC_EINVAL on a null argument, or whatever
 * the CMAC refused with (RADIANT_SEC_ENOKEY for an empty key handle).
 */
int radiant_sec_compat_tier1_tag(const struct radiant_sec_key *k_auth,
				 uint32_t epoch, uint16_t devnum,
				 uint16_t att_counter,
				 uint8_t out[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES]);

/*
 * Tier II - the data attestation.
 *
 *   trunc48( CMAC(K_auth, nonce_block || p_1 || p_2 || ... || p_{N-1}) )
 *
 * `msgs` is (n - 1) * 8 bytes: the N-1 preceding TRANSMITTED messages, in
 * transmission order, each the full eight payload bytes with the page number
 * included. Transmitted, not data - so the common pages, the beacon and any
 * Tier I page inside the window are covered too, and a receiver counts messages
 * rather than trying to decide which ones were "data".
 *
 * A flat byte array and two scalars rather than an array of structs, per
 * radiant_sec.h's rule 2. The caller owns the buffer; nothing is retained.
 *
 * Returns RADIANT_SEC_EINVAL for a null argument or an `n` outside
 * {4, 8, 16, 32}.
 */
int radiant_sec_compat_tier2_tag(const struct radiant_sec_key *k_auth,
				 uint32_t epoch, uint16_t devnum,
				 uint16_t window_index, const uint8_t *msgs,
				 uint8_t n,
				 uint8_t out[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES]);

#else  /* !CONFIG_RADIANT_SEC_COMPAT */

/*
 * The disabled shape. Scalars and uint8_t * only, so each collapses to nothing
 * without the compiler having to prove a struct copy dead, and there is no ops
 * table anywhere for the linker to fail to garbage-collect.
 */
static inline void radiant_sec_compat_nonce_block(uint8_t *out, uint32_t epoch,
						  uint16_t devnum,
						  uint16_t counter, uint8_t sub)
{
	(void)out; (void)epoch; (void)devnum; (void)counter; (void)sub;
}

static inline int radiant_sec_compat_tier1_tag(const struct radiant_sec_key *k,
					       uint32_t epoch, uint16_t devnum,
					       uint16_t att_counter,
					       uint8_t *out)
{
	(void)k; (void)epoch; (void)devnum; (void)att_counter; (void)out;
	return RADIANT_SEC_ENOTSUP;
}

static inline int radiant_sec_compat_tier2_tag(const struct radiant_sec_key *k,
					       uint32_t epoch, uint16_t devnum,
					       uint16_t window_index,
					       const uint8_t *msgs, uint8_t n,
					       uint8_t *out)
{
	(void)k; (void)epoch; (void)devnum; (void)window_index; (void)msgs;
	(void)n; (void)out;
	return RADIANT_SEC_ENOTSUP;
}

#endif /* CONFIG_RADIANT_SEC_COMPAT */

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_CORE_RADIANT_SEC_COMPAT_H_ */
