/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fake_rng.h - the control surface for the test image's entropy backend.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md (the caps.has_rng
 * seam) and docs/decisions/0020-k-dev-is-generated-on-first-boot.md (its first
 * caller, node_ident_boot()'s self-provisioning).
 *
 * fake_rng.c is to radiant_sec_rng() what fake_nvm.c is to node_nvm.h: the one
 * implementation of the seam in this image, compiled when
 * CONFIG_RADIANT_TEST_FAKE_RNG selects RADIANT_SEC_HAS_RNG. It is
 * deterministic, so a suite can name the K_dev a first boot will draw, and it
 * can be told to fail - the two things a real TRNG cannot be asked to do.
 */

#ifndef FAKE_RNG_H_
#define FAKE_RNG_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Back to the first draw, no failures armed, the draw count at zero. */
void fake_rng_reset(void);

/* The bytes draw number `draw` (0-based, counted since fake_rng_reset())
 * would produce, so a suite can say which key a first boot stored. Never all
 * zero. */
void fake_rng_expected(uint32_t draw, uint8_t *out, size_t len);

/* Make the next `n` draws fail as radiant_sec_rng_entropy.c does: zero-filled
 * buffer, RADIANT_SEC_ENOTSUP. */
void fake_rng_fail_next(uint32_t n);

/* The next draws return these keys, in order, before the deterministic
 * pattern resumes. For a suite that needs a specific K_dev (up to 16). Each is
 * `len` bytes, copied. fake_rng_reset() clears them. */
void fake_rng_force_next(const uint8_t *key, size_t len);

/* Report success with an all-zero buffer: a backend that lies. The caller's
 * all-zero refusal is what has to catch it. */
void fake_rng_return_zeros(bool on);

/* Every call, failed or not, since fake_rng_reset(). */
uint32_t fake_rng_draw_count(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_RNG_H_ */
