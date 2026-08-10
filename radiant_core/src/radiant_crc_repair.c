/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_crc_repair.c - single-bit CRC repair for tracked frames.
 *
 * Provenance: clean-room. See radiant_crc_repair.h for the argument and for the
 * tracked-windows-only rule that bounds the false-accept rate.
 *
 * This file includes no Zephyr header and no HAL entry point, on the same terms
 * as radiant_channel.c and radiant_frame.c. Its gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I radiant_core/include -fsyntax-only \
 *       radiant_core/src/radiant_crc_repair.c
 *
 * ---------------------------------------------------------------------------
 * The arithmetic, in full, because it is short and it is the whole file
 * ---------------------------------------------------------------------------
 * A CRC is linear over GF(2). For a fixed length, CRC_I(d) = L(d) + c(I), where
 * L is linear and c depends only on the initial value. So for any error vector
 * e of the same length:
 *
 *     CRC_I(d ^ e) ^ CRC_I(d) = L(e) = CRC_0(e)
 *
 * The initial value cancels. That is why RADIANT_CRC_INIT appears nowhere below
 * and why the table is built with a zero seed: the syndrome of an error is a
 * property of the error alone.
 *
 * Received frame: data d ^ e_d, CRC field crc(d) ^ e_c. Compute the CRC over
 * what arrived and subtract what arrived:
 *
 *     syndrome = crc_rx ^ crc_computed
 *              = (crc(d) ^ e_c) ^ (crc(d) ^ CRC_0(e_d))
 *              = e_c ^ CRC_0(e_d)
 *
 * For a single bit in the data at position p that is CRC_0(unit vector at p);
 * for a single bit q in the CRC field it is 1 << q. Both go in one table,
 * because both are equally valid explanations of the same syndrome and the
 * caller has to be told which one it got.
 *
 * CRC_0 of a unit vector depends only on how many bytes FOLLOW the flipped bit,
 * because a zero-seeded register fed zeros stays zero. That is what lets one
 * table serve every body length: positions are counted from the END.
 */

#include <string.h>

#include <radiant_core/radiant_crc_repair.h>

/*
 * One entry per correctable bit. Sorted is not worth it: 112 entries at 4 Hz
 * per channel is a few microseconds of Cortex-M4 time on the rare frame that
 * failed its CRC at all, and a linear scan needs no ordering invariant to go
 * wrong. The same argument radiant_frame.c makes for its bitwise CRC.
 */
struct repair_entry {
	uint16_t syndrome;
	/* Bit index counted from the END of the CRC-covered data, so entry i
	 * describes byte (i / 8) from the end and bit (i % 8) within it. The
	 * CRC field's own bits sit above the data's, at CRC_BIT_BASE. */
	uint16_t position;
};

#define CRC_BITS      ((uint16_t)(RADIANT_FRAME_CRC_BYTES * 8u))
#define DATA_BITS     ((uint16_t)((uint16_t)RADIANT_FRAME_BODY_MAX * 8u))
#define CRC_BIT_BASE  DATA_BITS

static struct repair_entry repair_table[RADIANT_CRC_REPAIR_ENTRIES];
static bool repair_built;
static bool repair_usable;

/*
 * CRC_0 of the unit vector `bit` bits from the end of the data.
 *
 * Built by running the real radiant_crc16() over an error vector rather than by
 * a shortcut, so the table cannot disagree with the CRC the rest of the stack
 * computes. If radiant_frame.c's polynomial or bit order ever changes, this
 * changes with it and the duplicate check below is what notices if the change
 * broke something.
 */
static uint16_t syndrome_of_data_bit(uint16_t bit)
{
	uint8_t vector[RADIANT_FRAME_BODY_MAX];
	uint16_t byte_from_end = (uint16_t)(bit / 8u);
	uint16_t bit_in_byte = (uint16_t)(bit % 8u);
	size_t len = (size_t)byte_from_end + 1u;

	memset(vector, 0, sizeof(vector));
	/* Bit 0 of the position is the LEAST significant bit of the last byte,
	 * which is the bit that arrives last. Nothing here reverses anything:
	 * radiant_crc16() consumes bytes most-significant-bit first, so the
	 * numbering only has to be consistent with itself and with the flip
	 * below. */
	vector[0] = (uint8_t)(1u << bit_in_byte);

	return radiant_crc16(0u, vector, len);
}

bool radiant_crc_repair_init(void)
{
	size_t i;
	size_t j;

	if (repair_built) {
		return repair_usable;
	}

	for (i = 0u; i < DATA_BITS; i++) {
		repair_table[i].syndrome = syndrome_of_data_bit((uint16_t)i);
		repair_table[i].position = (uint16_t)i;
	}
	for (i = 0u; i < CRC_BITS; i++) {
		repair_table[DATA_BITS + i].syndrome = (uint16_t)(1u << i);
		repair_table[DATA_BITS + i].position =
			(uint16_t)(CRC_BIT_BASE + i);
	}

	/*
	 * No duplicates, and no zero.
	 *
	 * A duplicate means two different single-bit errors are
	 * indistinguishable, so a "repair" would be a coin toss between them; a
	 * zero syndrome means some single-bit error is invisible to the CRC
	 * entirely, which would be a broken polynomial. Neither can happen for
	 * CRC-16/CCITT over 136 bits - it detects every single-bit error in
	 * blocks up to 32767 - so this loop exists to catch a change to the
	 * polynomial or the frame geometry, and the cost of it is once per boot.
	 */
	repair_usable = true;
	for (i = 0u; i < RADIANT_CRC_REPAIR_ENTRIES && repair_usable; i++) {
		if (repair_table[i].syndrome == 0u) {
			repair_usable = false;
			break;
		}
		for (j = i + 1u; j < RADIANT_CRC_REPAIR_ENTRIES; j++) {
			if (repair_table[i].syndrome == repair_table[j].syndrome) {
				repair_usable = false;
				break;
			}
		}
	}

	repair_built = true;
	return repair_usable;
}

bool radiant_crc_repair_ready(void)
{
	return repair_built && repair_usable;
}

static int position_of(uint16_t syndrome, uint16_t *out)
{
	size_t i;

	for (i = 0u; i < RADIANT_CRC_REPAIR_ENTRIES; i++) {
		if (repair_table[i].syndrome == syndrome) {
			*out = repair_table[i].position;
			return 0;
		}
	}
	return -1;
}

int radiant_crc_repair(enum radiant_frame_cfg cfg, const uint8_t *addr,
		       uint8_t addr_len, uint8_t *body, uint8_t body_len,
		       uint32_t crc_rx)
{
	uint16_t computed;
	uint16_t syndrome;
	uint16_t position = 0u;
	uint16_t byte_from_end;
	uint16_t bit_in_byte;

	/*
	 * TRACKING ONLY, decided here and not by the caller. The 1-in-585
	 * false-accept rate is tolerable against a population that already
	 * matched a full 5-byte address and intolerable against noise-triggered
	 * 3-byte matches - see the header. A call site can forget which window
	 * it is on; this cannot.
	 */
	if (cfg != RADIANT_FRAME_CFG_TRACKING) {
		return RADIANT_CRC_REPAIR_EINVAL;
	}
	if (addr == NULL || body == NULL || body_len == 0u ||
	    body_len > RADIANT_FRAME_BODY_MAX ||
	    addr_len > RADIANT_FRAME_ADDR_MAX) {
		return RADIANT_CRC_REPAIR_EINVAL;
	}
	if (!radiant_crc_repair_ready()) {
		return RADIANT_CRC_REPAIR_ESTATE;
	}

	/* The CRC covers the address and then the body, chained, exactly as
	 * radiant_frame.c computes it for transmission. */
	computed = radiant_crc16(RADIANT_CRC_INIT, addr, addr_len);
	computed = radiant_crc16(computed, body, body_len);

	syndrome = (uint16_t)((uint16_t)crc_rx ^ computed);
	if (syndrome == 0u) {
		/* The backend called this a CRC failure and the software CRC
		 * over the same bytes disagrees. That is a configuration fault
		 * - a different polynomial, a different coverage - and it must
		 * not be reported as an unrepairable frame, because the two
		 * have entirely different fixes. */
		return RADIANT_CRC_REPAIR_EAGREE;
	}

	if (position_of(syndrome, &position) != 0) {
		return RADIANT_CRC_REPAIR_ENONE;
	}

	if (position >= CRC_BIT_BASE) {
		/* The damage was in the CRC field. The body is already what was
		 * transmitted, so there is nothing to flip. */
		return RADIANT_CRC_REPAIR_IN_CRC;
	}

	byte_from_end = (uint16_t)(position / 8u);
	bit_in_byte = (uint16_t)(position % 8u);
	if (byte_from_end >= body_len) {
		/*
		 * The syndrome names a bit that would lie in the ADDRESS, in
		 * front of this body. That cannot have happened: a flipped
		 * address bit does not match the filter and produces no event.
		 * So the syndrome is a coincidence from a multi-bit error, and
		 * "unrepairable" is the honest answer rather than a write past
		 * the front of the buffer.
		 */
		return RADIANT_CRC_REPAIR_ENONE;
	}

	body[body_len - 1u - byte_from_end] ^= (uint8_t)(1u << bit_in_byte);
	return RADIANT_CRC_REPAIR_BODY;
}
