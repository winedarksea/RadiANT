/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_env.c - ANT+ Environment, device type 0x19. See profile_env.h.
 *
 * Decode only. There is no encoder here and that is deliberate: this project
 * receives environment sensors, it does not pretend to be one, and an unused
 * encoder is an unused second opinion about the bit packing that nothing keeps
 * honest. profile_rd.c pairs each encoder with its decoder because ant_sim.py
 * and tools/test_ant_pages.py exercise both halves; nothing does that here.
 *
 * profile_bits.c is deliberately NOT used, for the same reason profile_rd.c
 * states: it packs MSB-first into the RadiANT envelope's field area, which is
 * a different convention from this document's numbered bit positions within a
 * byte.
 *
 * All bit arithmetic for the two nibble-split fields lives in the two helpers
 * below, so there is exactly one place to be wrong about the packing.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "profile_env.h"

/* ── Sign extension ──────────────────────────────────────────────────────────
 *
 * §6.4's two 24-hour fields are twelve-bit SIGNED integers. Nothing about
 * reading them out of the bytes makes them signed - a 12-bit field lifted into
 * a uint16_t is a positive number between 0 and 4095, and -5.3 degC arrives as
 * 4043. The extension from bit 11 is a separate, explicit step, and skipping it
 * is a bug that only shows below freezing: every test vector taken in a warm
 * room passes.
 *
 * Written as an arithmetic subtraction rather than as `(int16_t)(v | 0xF000)`
 * because converting an out-of-range unsigned value to a signed type is
 * implementation-defined before C23. This form is fully defined by the
 * standard and compiles to the same instruction.
 */
static int16_t sign_extend_12(uint16_t v)
{
	int32_t x = (int32_t)(v & 0x0FFFu);

	if ((v & 0x0800u) != 0u) {
		x -= 4096;
	}
	return (int16_t)x;
}

/* The current-temperature field is a full sixteen bits wide, so there is no
 * extension to do - but the uint16_t -> int16_t conversion above 0x7FFF is the
 * same implementation-defined corner, and 0x8000 (the invalid sentinel) is
 * exactly the value that hits it. Same arithmetic form, same reason. */
static int16_t as_int16(uint16_t v)
{
	int32_t x = (int32_t)v;

	if ((v & 0x8000u) != 0u) {
		x -= 65536;
	}
	return (int16_t)x;
}

/*
 * The 24-hour low: byte [3] is the whole LSB (value bits 0..7) and byte [4]
 * bits 7:4 are the MSN (value bits 8..11). Table 6-4 calls them "24 Hour Low
 * LSB" and "24 Hour Low MSN".
 */
static uint16_t pack_low_24h(const uint8_t *body)
{
	return (uint16_t)((uint16_t)body[3] |
			  (uint16_t)(((uint16_t)(body[4] >> 4) & 0x0Fu) << 8));
}

/*
 * The 24-hour high is the MIRROR IMAGE and this is the trap in the page: byte
 * [4] bits 3:0 are the LSN (value bits 0..3) and byte [5] is the MSB (value
 * bits 4..11). The two fields share byte [4] and are packed in opposite
 * directions, so a decoder that reuses the low field's shape for the high one
 * reads a plausible temperature that is wrong by a factor of sixteen plus a
 * nibble - and stays plausible, because both fields are temperatures in the
 * same range.
 */
static uint16_t pack_high_24h(const uint8_t *body)
{
	return (uint16_t)(((uint16_t)body[4] & 0x0Fu) |
			  (uint16_t)((uint16_t)body[5] << 4));
}

/* ── Page 0x00, general information (§6.3) ───────────────────────────────── */

int profile_env_decode_capabilities(const uint8_t *body,
				    struct profile_env_capabilities *c)
{
	if (body == NULL || c == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_ENV_PAGE_CAPABILITIES) {
		return -EINVAL;
	}

	/*
	 * Table 6-3's bit order is stated from the top down (6:7 reserved, 4:5
	 * local, 2:3 UTC, 0:1 rate), which reads as if the rate were the high
	 * field. It is the low one. Each two-bit field is kept RAW rather than
	 * mapped to an enum: 0b11 is "Reserved" in all three, and a decoder
	 * that folded reserved into "not supported" would hide a sensor that a
	 * future revision of the profile made meaningful.
	 */
	c->default_rate = (uint8_t)(body[3] & 0x03u);
	c->utc_time     = (uint8_t)((body[3] >> 2) & 0x03u);
	c->local_time   = (uint8_t)((body[3] >> 4) & 0x03u);

	/* Bytes [4..7], u32 little-endian, bit position == page number
	 * (§6.3.2). Bit 0 is always set; a temperature sensor also sets bit 1;
	 * bits 2..31 are "shall be set to 0" today, which is why this is
	 * returned whole rather than reduced to two booleans - the reserved
	 * bits are the only place a future page would announce itself. */
	c->supported_pages = (uint32_t)body[4] |
			     ((uint32_t)body[5] << 8) |
			     ((uint32_t)body[6] << 16) |
			     ((uint32_t)body[7] << 24);

	return 0;
}

uint16_t profile_env_period_counts(uint8_t default_rate)
{
	switch (default_rate) {
	case PROFILE_ENV_RATE_0P5_HZ:
		return PROFILE_ENV_PERIOD_0P5_HZ;
	case PROFILE_ENV_RATE_4_HZ:
		return PROFILE_ENV_PERIOD_4_HZ;
	default:
		/* 0b10 and 0b11 are reserved. Returning 0 rather than guessing
		 * 8192 is the same abstention radiant_liveness.c makes for a
		 * binding with no period: a wrong period is a wrong staleness
		 * threshold, which is worse than none. */
		return 0u;
	}
}

/* ── Page 0x01, temperature (§6.4) ───────────────────────────────────────── */

int profile_env_decode_temperature(const uint8_t *body,
				   struct profile_env_temperature *t)
{
	uint16_t low_raw;
	uint16_t high_raw;
	uint16_t cur_raw;

	if (body == NULL || t == NULL) {
		return -EINVAL;
	}
	if (body[0] != PROFILE_ENV_PAGE_TEMPERATURE) {
		return -EINVAL;
	}

	t->event_count = body[2];

	low_raw  = pack_low_24h(body);
	high_raw = pack_high_24h(body);
	cur_raw  = (uint16_t)((uint16_t)body[6] | ((uint16_t)body[7] << 8));

	/*
	 * The sentinel test happens on the RAW field, before sign extension.
	 * It reads the same either way here and that is not luck: 0x800 and
	 * 0x8000 are each the most negative value their width can hold, and
	 * §6.4's stated ranges (-204.7..204.7 and -327.67..327.67) stop one
	 * count short of it on the negative side precisely so the sentinel is
	 * not also a legal reading. Checking the raw pattern is still the
	 * form to keep, because it is what the document says.
	 */
	t->low_24h_valid  = (low_raw != PROFILE_ENV_INVALID_12);
	t->high_24h_valid = (high_raw != PROFILE_ENV_INVALID_12);
	t->current_valid  = (cur_raw != PROFILE_ENV_INVALID_16);

	t->low_24h_deci   = sign_extend_12(low_raw);
	t->high_24h_deci  = sign_extend_12(high_raw);
	t->current_centi  = as_int16(cur_raw);

	/*
	 * The values are filled in even when invalid, and a caller must read
	 * the *_valid flag rather than compare against a magic number. Zeroing
	 * an invalid field would be worse than either: 0.00 degC is a
	 * perfectly ordinary reading and no consumer could tell the two apart
	 * afterwards. (profile_rd.c can zero its invalids because zero is that
	 * profile's own documented sentinel; here it is a temperature.)
	 */
	return 0;
}
