/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sec_rng_psa.c - the optional entropy backend.
 *
 * Per docs/decisions/0009-hostless-node-identity.md and
 * docs/radiant-security.md section 7.4: this is the opt-in exception to the
 * pinned "no entropy driver, no PSA, no CRACEN" default, not a reversal of
 * it.
 *
 * A whole file for one PSA call because the seam is the point:
 * radiant_sec_caps().has_rng is what callers read, and no caller anywhere
 * names PSA or CRACEN, so a different entropy source later replaces only
 * this file. psa_generate_random() reaches CRACEN's TRNG (nRF54L) or the
 * CryptoCell RNG (nRF53/nRF91), chosen by nrf_security config.
 *
 * Not a precondition for pairing on a hostless node: ADR 0009's primary path
 * is KDF(K_dev, "pair" || pair_counter) precisely so a part with no entropy
 * source is supported; this is just the better source where available.
 *
 * psa_crypto_init() is called here (not assumed) because this may be the
 * only PSA user in the image - an uninitialised stack returning
 * PSA_ERROR_BAD_STATE would otherwise read as "no entropy" and silently
 * downgrade with no symptom.
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
