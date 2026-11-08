/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fake_rng.c - radiant_sec_rng() for the test image: deterministic, and
 * breakable on demand.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md and
 * docs/decisions/0020-k-dev-is-generated-on-first-boot.md. See fake_rng.h.
 *
 * Compiled only with CONFIG_RADIANT_TEST_FAKE_RNG, which selects
 * RADIANT_SEC_HAS_RNG and so suppresses the refusing definition in
 * src/radiant_sec.c - the same one-definition arrangement fake_radio.c has
 * with CONFIG_RADIANT_BACKEND_NONE.
 *
 * NOT RANDOM, and that is the point: draw n's bytes are a fixed function of n,
 * so a suite can assert which key a first boot stored. Nothing outside this
 * test application can link it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant/radiant_sec.h>

#include "fake_rng.h"

static uint32_t draws;
static uint32_t fail_remaining;
static bool     zeros;

#define FORCED_MAX 16u
#define FORCED_LEN 32u
static uint8_t  forced[FORCED_MAX][FORCED_LEN];
static size_t   forced_len[FORCED_MAX];
static uint32_t forced_n;
static uint32_t forced_next;

void fake_rng_reset(void)
{
	draws = 0u;
	fail_remaining = 0u;
	zeros = false;
	forced_n = 0u;
	forced_next = 0u;
}

void fake_rng_force_next(const uint8_t *key, size_t len)
{
	if (forced_n < FORCED_MAX && len <= FORCED_LEN) {
		for (size_t i = 0u; i < len; i++) {
			forced[forced_n][i] = key[i];
		}
		forced_len[forced_n] = len;
		forced_n++;
	}
}

void fake_rng_expected(uint32_t draw, uint8_t *out, size_t len)
{
	/* A different pattern per draw: byte i of draw n is 0x5A + 0x11*n + i
	 * (mod 256). Consecutive values, so at most one byte in 256 is zero
	 * and the buffer is never all zero. */
	for (size_t i = 0u; i < len; i++) {
		out[i] = (uint8_t)(0x5Au + (0x11u * draw) + (uint32_t)i);
	}
}

void fake_rng_fail_next(uint32_t n)
{
	fail_remaining = n;
}

void fake_rng_return_zeros(bool on)
{
	zeros = on;
}

uint32_t fake_rng_draw_count(void)
{
	return draws;
}

int radiant_sec_rng(uint8_t *out, size_t len)
{
	uint32_t n = draws;

	if (out == NULL && len != 0u) {
		return RADIANT_SEC_EINVAL;
	}
	draws++;
	if (len == 0u) {
		return RADIANT_SEC_OK;
	}
	if (fail_remaining > 0u) {
		fail_remaining--;
		for (size_t i = 0u; i < len; i++) {
			out[i] = 0u;
		}
		return RADIANT_SEC_ENOTSUP;
	}
	if (zeros) {
		for (size_t i = 0u; i < len; i++) {
			out[i] = 0u;
		}
		return RADIANT_SEC_OK;
	}
	if (forced_next < forced_n && forced_len[forced_next] == len) {
		for (size_t i = 0u; i < len; i++) {
			out[i] = forced[forced_next][i];
		}
		forced_next++;
		return RADIANT_SEC_OK;
	}
	fake_rng_expected(n, out, len);
	return RADIANT_SEC_OK;
}
