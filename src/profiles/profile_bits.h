/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_bits.h - MSB-first bit packing for the RadiANT telemetry field area.
 *
 * Provenance: docs/radiant-telemetry.md section 6, "Bit packing is MSB-first",
 * and the width-code table in the same section. That document is this
 * project's own written specification, authored before any code existed; no
 * adopter-gated ANT+ device profile document, no sdk-ant source and no
 * libant.a-derived knowledge was consulted for this file. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * Why this is its own file, and why it has a twin in Python
 * ---------------------------------------------------------------------------
 * The envelope's field area is the one place in this project where a value is
 * NOT byte-aligned, and docs/radiant-telemetry.md section 6 says why that
 * matters:
 *
 *   > This is the opposite of the little-endian byte order every ANT+ page in
 *   > tools/ant_pages.py uses, and the difference is deliberate: those fields
 *   > are byte-aligned, where little-endian is unambiguous; these are not,
 *   > where little-endian bit order is a reliable source of two
 *   > implementations that each work alone.
 *
 * "Two implementations that each work alone" is the failure this file is
 * shaped against. So there is exactly one pack and one unpack here, exactly
 * one pair in tools/ant_pages.py, and tools/test_ant_pages.py checks both
 * against the same vectors - the vectors, not each other's output.
 *
 * The convention, stated once so neither side has to infer it:
 *
 *   bit offset 0 is the MOST SIGNIFICANT BIT OF BYTE [0] of the area, which is
 *   payload byte [2] of a data page. Offsets increase towards the least
 *   significant bit of the last byte. Within its width a value is stored
 *   most-significant-bit first, so a 12-bit value at offset 4 occupies the low
 *   nibble of byte 0 and the whole of byte 1, high nibble first.
 *
 * There is no alignment requirement of any kind: a 6-bit field may start at
 * offset 7. The descriptor carries an explicit bit offset per field precisely
 * so that a node may pack tightly and a receiver never has to guess.
 */

#ifndef RADIANT_PROFILE_BITS_H_
#define RADIANT_PROFILE_BITS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Width codes 0..12 are defined; 13..15 are reserved and are rejected rather
 * than clamped, because a receiver that guesses a width decodes every
 * subsequent field in the page at the wrong offset.
 */
#define PROFILE_BITS_WIDTH_CODE_MAX 12u
#define PROFILE_BITS_WIDTH_MAX      48u

/* Bits for a width code, or -EINVAL for a reserved one. */
int profile_bits_width(uint8_t width_code);

/* The inverse: a code for a width in bits, or -EINVAL if no code names it. */
int profile_bits_width_code(uint8_t bits);

/*
 * Write `width` bits of `value` at `bit_off` in an area `area_bits` wide.
 *
 * Bits of `value` above `width` are ignored rather than rejected: a node
 * publishing an accumulating field is expected to hand over a counter that has
 * already wrapped in some wider type, and refusing that would push the
 * masking - and therefore the wrap semantics - out to every caller. See
 * docs/radiant-telemetry.md section 5: the transmitted value is a running
 * total in the field's declared width and it is MEANT to wrap.
 *
 * Returns 0, or -EINVAL for a zero or oversized width, or -ERANGE when the
 * field would run past the end of the area.
 */
int profile_bits_pack(uint8_t *area, uint16_t area_bits, uint16_t bit_off,
		      uint8_t width, uint64_t value);

/* The exact inverse. *out is zero-extended; see profile_bits_sign_extend(). */
int profile_bits_unpack(const uint8_t *area, uint16_t area_bits,
			uint16_t bit_off, uint8_t width, uint64_t *out);

/*
 * Sign-extend a raw value of `width` bits.
 *
 * Separate from the unpack because the descriptor's `signed` bit is a property
 * of the field, not of the packing, and because a receiver that differences
 * two ACCUMULATING readings must do so in the field's own width - sign
 * extension before subtraction is precisely the "one absurd sample per wrap"
 * bug section 5 of the envelope document warns about.
 */
int64_t profile_bits_sign_extend(uint64_t raw, uint8_t width);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_BITS_H_ */
