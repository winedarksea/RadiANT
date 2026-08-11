/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sec_compat - the two attestation tags of docs/radiant-security.md
 * section 11.4, as bytes in and bytes out.
 *
 * Provenance: clean-room, and in this file's case the phrase is nearly
 * redundant - it is derived entirely from this project's own specifications,
 * docs/decisions/0008-antplus-additive-pages-and-compat-security.md and
 * docs/radiant-security.md section 11.4, on top of the seam in
 * radiant_core/include/radiant_core/radiant_sec.h. NOTHING HERE DERIVES FROM
 * ANY ANT+ DEVICE PROFILE DOCUMENT, gated or otherwise, and it could not: this
 * file names no page and no field, so there is nothing in it an external
 * profile specification could have said. It derives nothing from sdk-ant, from
 * libant.a, or from disassembly of any binary. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ── What is deliberately absent ────────────────────────────────────────────
 *
 * There is no emit hook, no receive hook, no scheduler client and no state of
 * any kind. Every function here is pure: arguments in, tag out, nothing
 * retained between calls. Wiring these into transmission and reception is a
 * later phase's job, and keeping the two apart is what lets this one be
 * verified against pinned vectors rather than against a running node.
 *
 * radiant_core/include/radiant_core/radiant_sec_compat.h carries the reasoning;
 * this file carries the bytes.
 */

#include <stdint.h>
#include <string.h>

#include <radiant_core/radiant_sec_compat.h>

#if defined(CONFIG_RADIANT_SEC_COMPAT)

/*
 * N in {4, 8, 16, 32} - inside the bounds with a single bit set, which is that
 * set spelled as an arithmetic property rather than as a table. Enumerating it
 * matters because N is simultaneously the airtime cost, the verification
 * latency and the amplification factor, and a caller reaching this with 5 has
 * one of three bugs rather than a rounding error.
 */
static bool window_is_legal(uint8_t n)
{
	return n >= RADIANT_SEC_COMPAT_N_MIN &&
	       n <= RADIANT_SEC_COMPAT_N_MAX &&
	       (n & (uint8_t)(n - 1u)) == 0u;
}

void radiant_sec_compat_nonce_block(uint8_t out[RADIANT_SEC_BLOCK_BYTES],
				    uint32_t epoch, uint16_t devnum,
				    uint16_t counter, uint8_t sub)
{
	if (out == NULL || sub == 0u || sub > 0x0Fu) {
		return;
	}

	/* Built by the shared helper and then extended, rather than laid out
	 * again here. Two byte layouts that must agree are two byte layouts
	 * that eventually do not, and the domain byte and the six trailing
	 * zeros are exactly the part a second copy would drift on. */
	radiant_sec_nonce_block(out, epoch, devnum, counter,
				RADIANT_SEC_DOM_COMPAT_MAC);
	out[9] = sub;
}

int radiant_sec_compat_tier1_tag(const struct radiant_sec_key *k_auth,
				 uint32_t epoch, uint16_t devnum,
				 uint16_t att_counter,
				 uint8_t out[RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES])
{
	uint8_t block[RADIANT_SEC_BLOCK_BYTES];
	uint8_t tag[RADIANT_SEC_BLOCK_BYTES];
	int rc;

	if (k_auth == NULL || out == NULL) {
		return RADIANT_SEC_EINVAL;
	}

	radiant_sec_compat_nonce_block(block, epoch, devnum, att_counter,
				       RADIANT_SEC_COMPAT_SUBTYPE_TIER_I);

	/* The message IS the nonce and there is nothing after it. That single
	 * fact is the tier: with no payload covered, a delivered tag verifies
	 * whatever else was lost. */
	rc = radiant_sec_cmac(k_auth, block, sizeof(block), tag);
	if (rc == RADIANT_SEC_OK) {
		memcpy(out, tag, RADIANT_SEC_COMPAT_TIER_I_TAG_BYTES);
	}

	/* The eleven bytes not sent are as secret as the key that made them:
	 * an attacker who learns them learns the untruncated tag for a counter
	 * value that has not been used yet. */
	memset(tag, 0, sizeof(tag));
	memset(block, 0, sizeof(block));
	return rc;
}

int radiant_sec_compat_tier2_tag(const struct radiant_sec_key *k_auth,
				 uint32_t epoch, uint16_t devnum,
				 uint16_t window_index, const uint8_t *msgs,
				 uint8_t n,
				 uint8_t out[RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES])
{
	struct radiant_sec_cmac_ctx ctx;
	uint8_t block[RADIANT_SEC_BLOCK_BYTES];
	uint8_t tag[RADIANT_SEC_BLOCK_BYTES];
	int rc;

	if (k_auth == NULL || msgs == NULL || out == NULL ||
	    !window_is_legal(n)) {
		return RADIANT_SEC_EINVAL;
	}

	radiant_sec_compat_nonce_block(block, epoch, devnum, window_index,
				       RADIANT_SEC_COMPAT_SUBTYPE_TIER_II);

	/* Absorbed incrementally, which is why there is no buffer here for the
	 * covered messages at all: at N = 32 a one-shot call would want 248
	 * bytes of stack for something CMAC consumes sixteen at a time. The
	 * same reason the spread MAC's window needs no payload buffer. */
	rc = radiant_sec_cmac_init(&ctx, k_auth);
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&ctx, block, sizeof(block));
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&ctx, msgs,
					     (size_t)(n - 1u) *
					     RADIANT_SEC_COMPAT_MSG_BYTES);
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_final(&ctx, tag);
	}
	if (rc == RADIANT_SEC_OK) {
		memcpy(out, tag, RADIANT_SEC_COMPAT_TIER_II_TAG_BYTES);
	}

	memset(&ctx, 0, sizeof(ctx));
	memset(tag, 0, sizeof(tag));
	memset(block, 0, sizeof(block));
	return rc;
}

#endif /* CONFIG_RADIANT_SEC_COMPAT */
