/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_bits.c - MSB-first bit packing for the RadiANT telemetry field area.
 *
 * Provenance: docs/radiant-telemetry.md section 6 (bit-packing convention and
 * the width-code table). That document is this project's own written
 * specification. No ANT+ device profile document, no sdk-ant
 * source and nothing derived from libant.a informed this file. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * Bit-at-a-time rather than shift-and-mask over a uint64_t straddle: a 48-bit
 * field at offset 0 in a 48-bit area would shift a uint64_t by 64, which is
 * undefined.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profile_bits.h"

/* docs/radiant-telemetry.md section 6. Gaps are deliberate: 1/2/4/6-bit codes
 * serve the boolean/small-enum vocabulary; no 64-bit code because the field
 * area itself is only 48 bits. */
static const uint8_t width_table[PROFILE_BITS_WIDTH_CODE_MAX + 1u] = {
	1u, 2u, 4u, 6u, 8u, 10u, 12u, 16u, 20u, 24u, 32u, 40u, 48u
};

int profile_bits_width(uint8_t width_code)
{
	if (width_code > PROFILE_BITS_WIDTH_CODE_MAX) {
		return -EINVAL;
	}
	return (int)width_table[width_code];
}

int profile_bits_width_code(uint8_t bits)
{
	uint8_t code;

	for (code = 0u; code <= PROFILE_BITS_WIDTH_CODE_MAX; code++) {
		if (width_table[code] == bits) {
			return (int)code;
		}
	}
	return -EINVAL;
}

/* Shared bounds check, so pack and unpack cannot disagree about the edge. */
static int check(uint16_t area_bits, uint16_t bit_off, uint8_t width)
{
	if (width == 0u || width > PROFILE_BITS_WIDTH_MAX) {
		return -EINVAL;
	}
	if ((uint32_t)bit_off + (uint32_t)width > (uint32_t)area_bits) {
		return -ERANGE;
	}
	return 0;
}

int profile_bits_pack(uint8_t *area, uint16_t area_bits, uint16_t bit_off,
		      uint8_t width, uint64_t value)
{
	int rc;
	uint8_t i;

	if (area == NULL) {
		return -EINVAL;
	}
	rc = check(area_bits, bit_off, width);
	if (rc != 0) {
		return rc;
	}

	for (i = 0u; i < width; i++) {
		/* i counts from the value's most significant bit, so the bit
		 * of `value` wanted here is (width - 1 - i). */
		uint32_t bit = (uint32_t)bit_off + i;
		uint8_t  byte = (uint8_t)(bit >> 3);
		uint8_t  mask = (uint8_t)(0x80u >> (bit & 7u));
		bool     set = ((value >> (width - 1u - i)) & 1u) != 0u;

		if (set) {
			area[byte] |= mask;
		} else {
			area[byte] &= (uint8_t)~mask;
		}
	}

	return 0;
}

int profile_bits_unpack(const uint8_t *area, uint16_t area_bits,
			uint16_t bit_off, uint8_t width, uint64_t *out)
{
	int rc;
	uint8_t i;
	uint64_t value = 0u;

	if (area == NULL || out == NULL) {
		return -EINVAL;
	}
	rc = check(area_bits, bit_off, width);
	if (rc != 0) {
		return rc;
	}

	for (i = 0u; i < width; i++) {
		uint32_t bit = (uint32_t)bit_off + i;
		uint8_t  byte = (uint8_t)(bit >> 3);
		uint8_t  mask = (uint8_t)(0x80u >> (bit & 7u));

		value <<= 1;
		if ((area[byte] & mask) != 0u) {
			value |= 1u;
		}
	}

	*out = value;
	return 0;
}

int64_t profile_bits_sign_extend(uint64_t raw, uint8_t width)
{
	uint64_t sign;

	if (width == 0u || width >= 64u) {
		return (int64_t)raw;
	}

	sign = (uint64_t)1u << (width - 1u);
	if ((raw & sign) == 0u) {
		return (int64_t)raw;
	}
	/* Widened two's complement via subtraction, not a right shift of a
	 * negative value (implementation-defined). */
	return (int64_t)raw - (int64_t)(sign << 1);
}
