/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sec_rng_psa.c - the optional entropy backend.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md, "The entropy
 * backend, and the seam it must not break", and docs/radiant-security.md
 * section 7.4, whose pinned "no entropy driver, no PSA, no CRACEN, on any
 * target" this file is the opt-in exception to rather than the reversal of.
 *
 * One function over one PSA call. It is a whole file because the seam is the
 * point: radiant_sec_caps().has_rng is what a caller reads, CONFIG_RADIANT_SEC_
 * HAS_RNG is what sets it, and no caller anywhere names PSA or CRACEN. Attach a
 * different entropy source later and this file is what gets replaced.
 *
 * psa_generate_random() reaches CRACEN's TRNG on nRF54L and the CryptoCell RNG
 * on nRF53/nRF91, chosen by the nrf_security configuration rather than by
 * anything here - which is why this file has no part number in it.
 *
 * WHAT THIS IS NOT: it is not a precondition for pairing on a hostless node.
 * ADR 0009 makes KDF(K_dev, "pair" || pair_counter) the primary path exactly so
 * that a part with no entropy source is a supported configuration. This is the
 * better source where it exists, and the counter advances identically either
 * way.
 *
 * psa_crypto_init() is idempotent and cheap after the first call, and it is
 * called here rather than assumed: this may be the only PSA user in an image,
 * and a caller that gets PSA_ERROR_BAD_STATE from an uninitialised stack would
 * read it as "no entropy" and silently fall back to the deterministic path -
 * a downgrade with no symptom.
 */

#include <stddef.h>
#include <stdint.h>

#include <psa/crypto.h>

#include <radiant_core/radiant_sec.h>

int radiant_sec_rng(uint8_t *out, size_t len)
{
	psa_status_t st;

	if (out == NULL && len != 0u) {
		return RADIANT_SEC_EINVAL;
	}
	if (len == 0u) {
		return RADIANT_SEC_OK;
	}

	st = psa_crypto_init();
	if (st != PSA_SUCCESS) {
		return RADIANT_SEC_ENOTSUP;
	}

	st = psa_generate_random(out, len);
	if (st != PSA_SUCCESS) {
		/*
		 * A failed draw must not leave a partial one looking usable.
		 * The caller is about to treat these bytes as a private key.
		 */
		for (size_t i = 0u; i < len; i++) {
			out[i] = 0u;
		}
		return RADIANT_SEC_ENOTSUP;
	}
	return RADIANT_SEC_OK;
}
