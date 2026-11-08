/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sec_rng_entropy.c - radiant_sec_rng() over Zephyr's entropy driver.
 *
 * Per docs/decisions/0009-hostless-node-identity.md (the caps.has_rng seam)
 * and docs/decisions/0020-k-dev-is-generated-on-first-boot.md (its first
 * caller): the node applications draw K_dev from this once, at first boot.
 *
 * The second backend behind the seam, beside radiant_sec_rng_psa.c, and the
 * one the node applications use. It reads the board's `zephyr,entropy` chosen
 * node, so the part decides the source and this file does not: psa_rng
 * (CRACEN's TRNG) on nRF54L, and the RNG peripheral on an nRF52840 whose
 * overlay chooses it. No caller anywhere names the driver - policy reads
 * radiant_sec_caps()->has_rng, which RADIANT_SEC_HAS_RNG drives.
 *
 * Same failure contract as radiant_sec_rng_psa.c: a failed draw zero-fills the
 * buffer. The caller is about to treat these bytes as a private key, and a
 * partial draw that looks usable is the one outcome worse than none.
 * node_ident_provision() refuses an all-zero key, so a zeroed failure that
 * somebody ignored still cannot become an identity.
 */

#include <stddef.h>
#include <stdint.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/entropy.h>

#include <radiant/radiant_sec.h>

#if !DT_HAS_CHOSEN(zephyr_entropy)
#error "CONFIG_RADIANT_SEC_RNG_ENTROPY needs a zephyr,entropy chosen node"
#endif

static const struct device *const entropy_dev =
	DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));

static void zero(uint8_t *out, size_t len)
{
	for (size_t i = 0u; i < len; i++) {
		out[i] = 0u;
	}
}

int radiant_sec_rng(uint8_t *out, size_t len)
{
	size_t done = 0u;

	if (out == NULL && len != 0u) {
		return RADIANT_SEC_EINVAL;
	}
	if (len == 0u) {
		return RADIANT_SEC_OK;
	}
	if (!device_is_ready(entropy_dev)) {
		zero(out, len);
		return RADIANT_SEC_ENOTSUP;
	}

	/* entropy_get_entropy() takes a uint16_t length; chunk rather than
	 * truncate, although the one caller today asks for 16 bytes. */
	while (done < len) {
		size_t   left  = len - done;
		uint16_t chunk = (left > UINT16_MAX) ? UINT16_MAX : (uint16_t)left;

		if (entropy_get_entropy(entropy_dev, out + done, chunk) != 0) {
			zero(out, len);
			return RADIANT_SEC_ENOTSUP;
		}
		done += chunk;
	}
	return RADIANT_SEC_OK;
}
