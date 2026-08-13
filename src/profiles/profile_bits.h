/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_bits.h - MSB-first bit packing for the RadiANT telemetry field area.
 *
 * Provenance: docs/radiant-telemetry.md section 6, "Bit packing is MSB-first",
 * and the width-code table in the same section. That document is this
 * project's own written specification, authored before any code existed; no
 * ANT+ device profile document, no sdk-ant source and no
 * libant.a-derived knowledge was consulted for this file. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * The envelope's field area is the one place in this project where a value is
 * NOT byte-aligned (unlike the little-endian byte-aligned ANT+ pages in
 * tools/ant_pages.py). There is exactly one pack/unpack here and one pair in
 * tools/ant_pages.py; tools/test_ant_pages.py checks both against the same
 * vectors, to avoid two implementations that each work alone.
 *
 * Convention: bit offset 0 is the MSB of byte [0] of the area (payload byte
 * [2] of a data page). Offsets increase toward the LSB of the last byte.
 * Within its width a value is stored MSB-first, so a 12-bit value at offset 4
 * occupies the low nibble of byte 0 and all of byte 1, high nibble first.
 *
 * No alignment requirement: a 6-bit field may start at offset 7. The
 * descriptor's explicit per-field bit offset is what makes that safe.
 */

#ifndef RADIANT_PROFILE_BITS_H_
#define RADIANT_PROFILE_BITS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Width codes 0..12 are defined; 13..15 are reserved and rejected rather than
 * clamped - a wrong guess decodes every later field at the wrong offset. */
#define PROFILE_BITS_WIDTH_CODE_MAX 12u
#define PROFILE_BITS_WIDTH_MAX      48u

/* Bits for a width code, or -EINVAL for a reserved one. */
int profile_bits_width(uint8_t width_code);

/* The inverse: a code for a width in bits, or -EINVAL if no code names it. */
int profile_bits_width_code(uint8_t bits);

/*
 * Write `width` bits of `value` at `bit_off` in an area `area_bits` wide.
 *
 * Bits of `value` above `width` are ignored rather than rejected: an
 * accumulating field's value is meant to wrap at its declared width (see
 * docs/radiant-telemetry.md section 5), so masking here saves every caller
 * from doing it themselves.
 *
 * Returns 0, or -EINVAL for a zero or oversized width, or -ERANGE when the
 * field would run past the end of the area.
 */
int profile_bits_pack(uint8_t *area, uint16_t area_bits, uint16_t bit_off,
		      uint8_t width, uint64_t value);

/* The exact inverse. *out is zero-extended; see profile_bits_sign_extend(). */
int profile_bits_unpack(const uint8_t *area, uint16_t area_bits,
			uint16_t bit_off, uint8_t width, uint64_t *out);

/* Sign-extend a raw value of `width` bits. Kept separate from unpack because
 * `signed` is a property of the field, not of the packing; a receiver
 * differencing two accumulating readings must sign-extend before subtracting
 * to avoid the "one absurd sample per wrap" bug (section 5). */
int64_t profile_bits_sign_extend(uint64_t raw, uint8_t width);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_BITS_H_ */
