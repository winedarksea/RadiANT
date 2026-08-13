/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_frame.c - the ANT on-air frame: CRC, address packing, encode, decode.
 *
 * Provenance: clean-room. Written from docs/ant-radio-link.md,
 * docs/spike-a-results.md, docs/spike-b-results.md and
 * docs/spike-b-part2-results.md (the control byte, its eleven measured values
 * and the six-field structure behind them; part 2 supersedes part 1 wherever
 * they differ), from radiant_core/include/radiant_core/radiant_radio_hal.h, and from
 * public nRF52840/nRF54L15 product specifications for the reasoning about the
 * two packet configurations. The CRC is a textbook bitwise CRC-16/CCITT-FALSE
 * with the parameters recorded in docs/ant-radio-link.md; the only other
 * implementation in this repository is radiant_core/spike/rx_raw/src/main.c, which
 * is our own Apache-2.0 code. Nothing here derives from sdk-ant, from
 * libant.a, from disassembly of any binary, from rtl_433's expression, or from
 * any ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * This file includes no Zephyr header and nothing from the application, on
 * purpose. It has to compile as a freestanding translation unit - the gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I radiant_core/include -fsyntax-only radiant_core/src/radiant_frame.c
 *
 * - so that a compile error here is never confused with a Zephyr or a board
 * problem, and so that a future host-side consumer (a capture reader, a
 * conformance tool) can build it with nothing but a C compiler.
 */

#include <string.h>

#include <radiant_core/radiant_frame.h>

/* _Static_assert rather than Zephyr's BUILD_ASSERT, since this file
 * deliberately includes no Zephyr header. */

/* The frame must fit inside the HAL's advisory buffer ceilings, or a backend
 * sized from those would silently truncate the largest frame this module can
 * produce. */
_Static_assert(RADIANT_FRAME_BODY_MAX <= RADIANT_RADIO_BODY_MAX,
	       "radiant_frame body exceeds the HAL's advisory body ceiling");
_Static_assert(RADIANT_FRAME_ADDR_MAX <= RADIANT_RADIO_ADDR_MAX,
	       "radiant_frame address exceeds the HAL's address ceiling");
/* The long-range body is why that ceiling is 40 rather than 32; if someone
 * lowers one without the other, DMA buffers become too small for the longest
 * frame, and the receive-side truncation the CRC turns into looks exactly
 * like poor sensitivity. */
_Static_assert(RADIANT_FRAME_LR_BODY_MAX <= RADIANT_RADIO_BODY_MAX,
	       "the long-range body exceeds the HAL's advisory body ceiling");
_Static_assert(RADIANT_FRAME_ADDR_LEN_LR <= RADIANT_FRAME_ADDR_MAX,
	       "the long-range address exceeds the address ceiling");
/* The length byte must be able to express the longest body (body[0] =
 * body_len - 1), so the bound is on body_len - 1, not body_len. */
_Static_assert(RADIANT_FRAME_LR_BODY_MAX - RADIANT_FRAME_LR_LEN_BIAS <= 255,
	       "the long-range length byte cannot express its own longest body");

/* The 15-byte coverage invariant, at compile time too: tracking is 5 addr +
 * 2 hdr, search is 3 addr + 4 hdr, both 7, so both cover the same 15 bytes
 * and produce the same CRC. Catches a change to any of the four numbers at
 * build time rather than on the air. */
_Static_assert(RADIANT_FRAME_ADDR_LEN_TRACKING + RADIANT_FRAME_HDR_LEN_TRACKING ==
		       RADIANT_FRAME_ADDR_LEN_SEARCH + RADIANT_FRAME_HDR_LEN_SEARCH,
	       "the two packet configurations no longer cover the same bytes");
_Static_assert(RADIANT_FRAME_ADDR_LEN_TRACKING + RADIANT_FRAME_HDR_LEN_TRACKING +
		       RADIANT_FRAME_PAYLOAD_STD == 15u,
	       "a standard ANT frame no longer has 15 CRC-covered bytes");

const uint8_t radiant_net_addr_ant_plus[RADIANT_NET_ADDR_LEN] = {
	RADIANT_NET_ADDR_ANT_PLUS_0,
	RADIANT_NET_ADDR_ANT_PLUS_1,
};

/* ---------------------------------------------------------------------------
 * CRC
 * ---------------------------------------------------------------------------
 */

/* Bitwise, not table-driven: a 512-byte table would buy ~8x on a calculation
 * that costs ~2 us of Cortex-M4 time per frame even at 32 channels, spending
 * flash where flash is scarce and the CRC runs in hardware on the backend
 * that matters. If a software-CRC backend ever makes this hot, the table
 * goes behind a Kconfig symbol. */
uint16_t radiant_crc16(uint16_t seed, const uint8_t *data, size_t len)
{
	uint16_t crc = seed;

	if (data == NULL) {
		return crc;
	}

	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)((uint16_t)data[i] << 8);
		for (unsigned int bit = 0; bit < 8u; bit++) {
			if (crc & 0x8000u) {
				crc = (uint16_t)((uint16_t)(crc << 1) ^ RADIANT_CRC_POLY);
			} else {
				crc = (uint16_t)(crc << 1);
			}
		}
	}

	return crc;
}

/* ---------------------------------------------------------------------------
 * The control byte: encoded/decoded as six fields, but the field model can
 * express 256 bytes and only eleven have ever been transmitted, so it's
 * paired with a table of the measured eleven that every on-air byte choice is
 * checked against. Both halves are needed - shift-and-mask alone invents the
 * other 245, a table alone missed bit 4 for six runs of part 1.
 * ---------------------------------------------------------------------------
 */

/* The eleven values measured on air, in ascending order
 * (archive/captures/radio/2026-08-09-spike-b2-run{0,A,B,C}.log). Nothing may
 * be added here that has not been captured. */
static const uint8_t ctrl_observed[] = {
	RADIANT_CTRL_BROADCAST,       /* 0x0A */
	RADIANT_CTRL_BURST_SEQ0,      /* 0x82 */
	RADIANT_CTRL_BURST_OPEN_SEQ0, /* 0x8A */
	RADIANT_CTRL_BURST_SEQ1,      /* 0x92 */
	RADIANT_CTRL_BURST_LAST_SEQ0, /* 0xA2 */
	RADIANT_CTRL_ACK_DATA_OPEN,   /* 0xAA */
	RADIANT_CTRL_BURST_LAST_SEQ1, /* 0xB2 */
	RADIANT_CTRL_ACK_SEQ0,        /* 0xC2 */
	RADIANT_CTRL_ACK_SEQ1,        /* 0xD2 */
	RADIANT_CTRL_ACK_LAST_SEQ0,   /* 0xE2 */
	RADIANT_CTRL_ACK_LAST_SEQ1,   /* 0xF2 */
};

/* Every measured value has 010 in bits 2:0 - 0 exceptions in 2,354 CRC-valid
 * frames in part 2 and 750 in part 1. A row that breaks this changes an
 * invariant the whole decode path rests on, so the build should catch it. */
_Static_assert((RADIANT_CTRL_BROADCAST & RADIANT_CTRL_LOW_MASK) == RADIANT_CTRL_LOW_VALUE &&
		       (RADIANT_CTRL_BURST_SEQ0 & RADIANT_CTRL_LOW_MASK) == RADIANT_CTRL_LOW_VALUE &&
		       (RADIANT_CTRL_BURST_OPEN_SEQ0 & RADIANT_CTRL_LOW_MASK) == RADIANT_CTRL_LOW_VALUE &&
		       (RADIANT_CTRL_BURST_SEQ1 & RADIANT_CTRL_LOW_MASK) == RADIANT_CTRL_LOW_VALUE &&
		       (RADIANT_CTRL_BURST_LAST_SEQ0 & RADIANT_CTRL_LOW_MASK) == RADIANT_CTRL_LOW_VALUE &&
		       (RADIANT_CTRL_ACK_DATA_OPEN & RADIANT_CTRL_LOW_MASK) == RADIANT_CTRL_LOW_VALUE &&
		       (RADIANT_CTRL_BURST_LAST_SEQ1 & RADIANT_CTRL_LOW_MASK) == RADIANT_CTRL_LOW_VALUE &&
		       (RADIANT_CTRL_ACK_SEQ0 & RADIANT_CTRL_LOW_MASK) == RADIANT_CTRL_LOW_VALUE &&
		       (RADIANT_CTRL_ACK_SEQ1 & RADIANT_CTRL_LOW_MASK) == RADIANT_CTRL_LOW_VALUE &&
		       (RADIANT_CTRL_ACK_LAST_SEQ0 & RADIANT_CTRL_LOW_MASK) == RADIANT_CTRL_LOW_VALUE &&
		       (RADIANT_CTRL_ACK_LAST_SEQ1 & RADIANT_CTRL_LOW_MASK) == RADIANT_CTRL_LOW_VALUE,
	       "a control byte in the observed table does not carry 010 in bits 2:0");

/* The four measured data -> acknowledgement pairs, and no fifth: every one
 * of the 168 adjacent CRC-valid pairs in runs 0, A and B is one of these
 * (bit 6 set, bit 5 echoed, bit 4 complemented, bits 7/3 unchanged). A slot
 * opener is deliberately absent - what bit 3 of an ack to 0x8A/0xAA would be
 * is not measured, and this table does not invent it. */
static const struct {
	uint8_t data;
	uint8_t reply;
} ctrl_replies[] = {
	{ RADIANT_CTRL_BURST_SEQ0,      RADIANT_CTRL_ACK_SEQ1 },      /* 82 -> D2 */
	{ RADIANT_CTRL_BURST_SEQ1,      RADIANT_CTRL_ACK_SEQ0 },      /* 92 -> C2 */
	{ RADIANT_CTRL_BURST_LAST_SEQ0, RADIANT_CTRL_ACK_LAST_SEQ1 }, /* A2 -> F2 */
	{ RADIANT_CTRL_BURST_LAST_SEQ1, RADIANT_CTRL_ACK_LAST_SEQ0 }, /* B2 -> E2 */
};

uint8_t radiant_ctrl_encode(const struct radiant_ctrl_fields *f)
{
	uint8_t ctrl = RADIANT_CTRL_LOW_VALUE;

	if (f == NULL) {
		return 0;
	}

	if (f->exchange) {
		ctrl |= RADIANT_CTRL_EXCHANGE;
	}
	if (f->ack) {
		ctrl |= RADIANT_CTRL_ACK;
	}
	if (f->last) {
		ctrl |= RADIANT_CTRL_LAST;
	}
	if (f->seq) {
		ctrl |= RADIANT_CTRL_SEQ;
	}
	if (f->slot_opener) {
		ctrl |= RADIANT_CTRL_SLOT;
	}

	return ctrl;
}

int radiant_ctrl_decode(uint8_t ctrl, struct radiant_ctrl_fields *out)
{
	struct radiant_ctrl_fields f;

	if (out == NULL) {
		return RADIANT_FRAME_EINVAL;
	}
	if (!radiant_ctrl_low_ok(ctrl)) {
		return RADIANT_FRAME_ECTRL;
	}

	f.exchange = radiant_ctrl_is_exchange(ctrl);
	f.ack = radiant_ctrl_is_ack(ctrl);
	f.last = radiant_ctrl_is_last(ctrl);
	f.seq = (radiant_ctrl_seq(ctrl) != 0u);
	f.slot_opener = radiant_ctrl_is_slot_opener(ctrl);

	*out = f;
	return RADIANT_FRAME_OK;
}

bool radiant_ctrl_observed(uint8_t ctrl)
{
	for (size_t i = 0; i < sizeof(ctrl_observed) / sizeof(ctrl_observed[0]); i++) {
		if (ctrl_observed[i] == ctrl) {
			return true;
		}
	}

	return false;
}

uint8_t radiant_ctrl_reply_for(uint8_t data_ctrl)
{
	for (size_t i = 0; i < sizeof(ctrl_replies) / sizeof(ctrl_replies[0]); i++) {
		if (ctrl_replies[i].data == data_ctrl) {
			return ctrl_replies[i].reply;
		}
	}

	/* Not a data packet this bench has ever seen acknowledged. 0 is not a
	 * legal control byte, so an ignoring caller puts an unacceptable frame
	 * on the air rather than a plausible wrong one. */
	return 0;
}

enum radiant_msg_type radiant_frame_msg_type(uint8_t ctrl_byte)
{
	if (!radiant_ctrl_low_ok(ctrl_byte)) {
		return RADIANT_MSG_UNKNOWN;
	}

	if (!radiant_ctrl_is_exchange(ctrl_byte)) {
		/* b7 = 0 occurred only with b6 = b5 = 0 across all 3,104
		 * CRC-valid frames: the only broadcast encoding is 0x0A, and
		 * anything else with b7 clear is untransmitted. */
		if (radiant_ctrl_is_ack(ctrl_byte) || radiant_ctrl_is_last(ctrl_byte)) {
			return RADIANT_MSG_UNKNOWN;
		}
		return RADIANT_MSG_BROADCAST;
	}

	if (radiant_ctrl_is_ack(ctrl_byte)) {
		return radiant_ctrl_is_last(ctrl_byte) ? RADIANT_MSG_TRANSFER_ACK
						   : RADIANT_MSG_BURST_ACK;
	}

	/* b6 = 0: a data packet. "Last" is what acknowledged data and a
	 * one-packet burst both are, and they are the same bytes on air. */
	return radiant_ctrl_is_last(ctrl_byte) ? RADIANT_MSG_BURST_LAST
					   : RADIANT_MSG_BURST_DATA;
}

/* ---------------------------------------------------------------------------
 * Bit order and address packing
 * ---------------------------------------------------------------------------
 */

uint8_t radiant_rev8(uint8_t b)
{
	b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
	b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
	b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
	return b;
}

/* An address needs at least one leading byte and one trailing byte, and no
 * planned matcher takes more than five bytes in total. */
static bool addr_len_ok(uint8_t addr_len)
{
	return addr_len >= 2u && addr_len <= RADIANT_FRAME_ADDR_MAX;
}

uint32_t radiant_addr_pack_leading(const uint8_t *addr, uint8_t addr_len)
{
	uint32_t word = 0;
	uint8_t n_leading;

	if (addr == NULL || !addr_len_ok(addr_len)) {
		return 0;
	}

	n_leading = (uint8_t)(addr_len - 1u);

	/* First byte on air in the lowest occupied byte, every byte
	 * bit-reversed. */
	for (uint8_t i = 0; i < n_leading; i++) {
		word |= (uint32_t)radiant_rev8(addr[i]) << (8u * i);
	}

	/*
	 * Then lift the used bytes to the top of the word. At four leading
	 * bytes the shift is zero and this whole line disappears, which is the
	 * reason it is written as a shift: one expression covers both
	 * configurations, and the alternative - a short address packed into the
	 * low bytes - is a receiver that fires on noise and validates nothing.
	 */
	word <<= 8u * (4u - n_leading);

	return word;
}

uint8_t radiant_addr_pack_trailing(const uint8_t *addr, uint8_t addr_len)
{
	if (addr == NULL || !addr_len_ok(addr_len)) {
		return 0;
	}

	return radiant_rev8(addr[addr_len - 1u]);
}

/* ---------------------------------------------------------------------------
 * Geometry
 * ---------------------------------------------------------------------------
 */

int radiant_frame_addr_len(enum radiant_frame_cfg cfg)
{
	switch (cfg) {
	case RADIANT_FRAME_CFG_TRACKING:
		return (int)RADIANT_FRAME_ADDR_LEN_TRACKING;
	case RADIANT_FRAME_CFG_SEARCH:
		return (int)RADIANT_FRAME_ADDR_LEN_SEARCH;
	case RADIANT_FRAME_CFG_LR:
		return (int)RADIANT_FRAME_ADDR_LEN_LR;
	default:
		return RADIANT_FRAME_EINVAL;
	}
}

int radiant_frame_hdr_len(enum radiant_frame_cfg cfg)
{
	switch (cfg) {
	case RADIANT_FRAME_CFG_TRACKING:
		return (int)RADIANT_FRAME_HDR_LEN_TRACKING;
	case RADIANT_FRAME_CFG_SEARCH:
		return (int)RADIANT_FRAME_HDR_LEN_SEARCH;
	case RADIANT_FRAME_CFG_LR:
		return (int)RADIANT_FRAME_HDR_LEN_LR;
	default:
		return RADIANT_FRAME_EINVAL;
	}
}

/*
 * The largest payload a configuration may carry.
 *
 * PER CONFIGURATION RATHER THAN ONE CONSTANT, because the two ANT formats and
 * the long-range one disagree by a factor of one and a half and the disagreement
 * is the whole point of the third format. The ANT bound stays exactly where it
 * was - 24, an advanced-burst buffer ceiling that no captured frame has ever
 * approached - so nothing about the ANT path moves.
 */
static int payload_max(enum radiant_frame_cfg cfg)
{
	switch (cfg) {
	case RADIANT_FRAME_CFG_TRACKING:
	case RADIANT_FRAME_CFG_SEARCH:
		return (int)RADIANT_FRAME_PAYLOAD_MAX;
	case RADIANT_FRAME_CFG_LR:
		return (int)RADIANT_FRAME_PAYLOAD_LR_MAX;
	default:
		return RADIANT_FRAME_EINVAL;
	}
}

int radiant_frame_body_len(enum radiant_frame_cfg cfg, uint8_t payload_len)
{
	int hdr = radiant_frame_hdr_len(cfg);
	int pmax = payload_max(cfg);

	if (hdr < 0) {
		return hdr;
	}
	if (pmax < 0) {
		return pmax;
	}
	if ((int)payload_len > pmax) {
		return RADIANT_FRAME_EINVAL;
	}

	return hdr + (int)payload_len;
}

int radiant_frame_covered_len(enum radiant_frame_cfg cfg, uint8_t payload_len)
{
	int addr = radiant_frame_addr_len(cfg);
	int body = radiant_frame_body_len(cfg, payload_len);

	if (addr < 0) {
		return addr;
	}
	if (body < 0) {
		return body;
	}

	return addr + body;
}

/*
 * Both ANT formats below (tracking, search) are static-length. Tracking used
 * to be RADIANT_LEN_FROM_BODY assuming byte 3 was a length byte - it's a
 * control byte. Mapping that mode onto nRF PCNF0.LFLEN=8 reads an
 * acknowledged frame's 0xAA as LENGTH=170, overrunning MAXLEN and discarding
 * it as a CRC error: every broadcast works, every acknowledged/burst frame
 * silently drops. Acknowledged data sets a trainer's resistance, so this
 * bug reads as "ERG mode does not work" months downstream.
 *
 * Spike B part 2 makes this permanent: byte 3 reads 0x0A on a broadcast (low
 * five bits 10) and 0xA2 on a slave frame (low five bits 2) for the same
 * eight-byte payload - no hardware length field can parse both.
 *
 * Fixed length (PCNF0=0/STATLEN) is byte-identical on air to CRCINC (40/40
 * CRC-valid in Spike A; 750 + 2,354 frames captured this way) and puts byte 3
 * in RAM for software to read/choose.
 *
 * Cost: a payload other than 8 bytes needs its own format, same as search
 * already carries. Nothing has ever needed one - both parts of Spike B sent
 * 24-byte advanced-burst blocks fragmented into 8-byte on-air packets.
 */
static const struct radiant_pkt_format fmt_tracking = {
	.phy = RADIANT_PHY_1M_GFSK,
	.addr_len = (uint8_t)RADIANT_FRAME_ADDR_LEN_TRACKING,
	.len_mode = RADIANT_LEN_FIXED,
	.body_len = (uint8_t)(RADIANT_FRAME_HDR_LEN_TRACKING + RADIANT_FRAME_PAYLOAD_STD),
	.len_offset = 0,
	.len_bias = 0,
	.max_body_len = (uint8_t)(RADIANT_FRAME_HDR_LEN_TRACKING + RADIANT_FRAME_PAYLOAD_STD),
	.crc = {
		.width_bits = 16,
		.poly = RADIANT_CRC_POLY,
		.init = RADIANT_CRC_INIT,
		.xor_out = 0,
		.reflect_in = false,
		.reflect_out = false,
		.cover_addr = true,
	},
};

static const struct radiant_pkt_format fmt_search = {
	.phy = RADIANT_PHY_1M_GFSK,
	.addr_len = (uint8_t)RADIANT_FRAME_ADDR_LEN_SEARCH,
	.len_mode = RADIANT_LEN_FIXED,
	.body_len = (uint8_t)(RADIANT_FRAME_HDR_LEN_SEARCH + RADIANT_FRAME_PAYLOAD_STD),
	.len_offset = 0,
	.len_bias = 0,
	.max_body_len = (uint8_t)(RADIANT_FRAME_HDR_LEN_SEARCH + RADIANT_FRAME_PAYLOAD_STD),
	.crc = {
		.width_bits = 16,
		.poly = RADIANT_CRC_POLY,
		.init = RADIANT_CRC_INIT,
		.xor_out = 0,
		.reflect_in = false,
		.reflect_out = false,
		.cover_addr = true,
	},
};

/*
 * The third format, and the only one that is not ANT.
 *
 * RADIANT_LEN_FROM_BODY is correct here for the reason it's wrong above: the
 * byte at that offset is genuinely a length, because this module put one
 * there - no pre-existing control byte being reinterpreted.
 *
 * The CRC stays 16-bit (not BLE coded PHY's 24-bit) to avoid a second CRC
 * implementation and the loss of single-bit repair, for a residual-error
 * improvement that FEC (S=8) already makes the smaller term. CRC-24 is the
 * obvious next change if long frames become the common case.
 *
 * cover_addr stays true, matching both ANT formats, so one software CRC
 * covers every format. A wrong CRC configuration here fails loudly (zero
 * valid frames) rather than silently, unlike a wrong t_sync constant.
 */
static const struct radiant_pkt_format fmt_lr = {
	.phy = RADIANT_PHY_LR_CODED,
	.addr_len = (uint8_t)RADIANT_FRAME_ADDR_LEN_LR,
	.len_mode = RADIANT_LEN_FROM_BODY,
	/* Unused in this mode, and zero rather than a plausible number: a
	 * backend that read it anyway would get an obviously wrong answer
	 * instead of a subtly wrong one. */
	.body_len = 0,
	.len_offset = (uint8_t)RADIANT_FRAME_LR_LEN_OFFSET,
	.len_bias = (int8_t)RADIANT_FRAME_LR_LEN_BIAS,
	.max_body_len = (uint8_t)RADIANT_FRAME_LR_BODY_MAX,
	.crc = {
		.width_bits = 16,
		.poly = RADIANT_CRC_POLY,
		.init = RADIANT_CRC_INIT,
		.xor_out = 0,
		.reflect_in = false,
		.reflect_out = false,
		.cover_addr = true,
	},
};

const struct radiant_pkt_format *radiant_frame_format(enum radiant_frame_cfg cfg)
{
	switch (cfg) {
	case RADIANT_FRAME_CFG_TRACKING:
		return &fmt_tracking;
	case RADIANT_FRAME_CFG_SEARCH:
		return &fmt_search;
	case RADIANT_FRAME_CFG_LR:
		return &fmt_lr;
	default:
		return NULL;
	}
}

/* ---------------------------------------------------------------------------
 * Airtime. Two PHYs, two shapes of arithmetic.
 *
 * 1 M GFSK is linear: preamble, address, body and CRC at 8 us per byte.
 *
 * LE Coded is not: FEC BLOCK 1 IS ALWAYS CODED AT S=8 WHATEVER THE DATA RATE
 * (hardware, not a choice), carrying preamble (80 us) + 32-bit access
 * address (256 us) + coding indicator (16 us) + TERM1 (24 us) = 376 us fixed.
 * FEC block 2 carries body + CRC + TERM2 at the selected rate (64 us/byte
 * at S=8).
 *
 * Consequence (why ADR 0007 builds one rate): at these frame sizes the fixed
 * 376 us dominates, so S=2 would only be ~2.1x cheaper than S=8, not 4x - not
 * worth the ~3 dB link margin it costs.
 *
 * This function returns 1.30 ms for an eight-byte frame at S=8, vs. the
 * plan's reference ~1.23 ms; the 64 us difference is one byte, because the
 * reference assumed a 3-byte header where the format carries 4 (the length
 * byte the length extension needs).
 * ---------------------------------------------------------------------------
 */

/* 1 M GFSK: one byte is 8 us exactly. */
#define AIR_1M_US_PER_BYTE 8u
/* The preamble is a PHY property and not this module's to choose, but its
 * airtime is part of a frame's cost. One byte at 1 M (PCNF0.PLEN = 8bit,
 * confirmed by Spike A); on LE Coded it is inside the 376 us below. */
#define AIR_1M_PREAMBLE_BYTES 1u

#define AIR_LR_FEC1_US       376u
#define AIR_LR_US_PER_BYTE   64u   /* 8 bits at 8 us/bit */
#define AIR_LR_TERM2_US      24u   /* 3 bits at 8 us/bit */

uint32_t radiant_frame_airtime_us(enum radiant_frame_cfg cfg, uint8_t payload_len)
{
	int body = radiant_frame_body_len(cfg, payload_len);
	int addr = radiant_frame_addr_len(cfg);

	if (body < 0 || addr < 0) {
		return 0u;
	}

	if (cfg == RADIANT_FRAME_CFG_LR) {
		/* The address is inside FEC block 1's fixed 376 us (it IS the
		 * coded PHY's 32-bit access address), so it's deliberately not
		 * added again - the most plausible mistake in this function. */
		return AIR_LR_FEC1_US +
		       ((uint32_t)body + RADIANT_FRAME_CRC_BYTES) * AIR_LR_US_PER_BYTE +
		       AIR_LR_TERM2_US;
	}

	return (AIR_1M_PREAMBLE_BYTES + (uint32_t)addr + (uint32_t)body +
		RADIANT_FRAME_CRC_BYTES) *
	       AIR_1M_US_PER_BYTE;
}

/* ---------------------------------------------------------------------------
 * The long-range body
 * ---------------------------------------------------------------------------
 */

int radiant_frame_lr_body(const struct radiant_channel_id *id, uint8_t ctrl_byte,
			  const uint8_t *payload, uint8_t payload_len,
			  uint8_t *out, size_t out_len)
{
	int body_len;

	if (id == NULL || out == NULL) {
		return RADIANT_FRAME_EINVAL;
	}
	if (payload == NULL && payload_len > 0u) {
		return RADIANT_FRAME_EINVAL;
	}
	if (!radiant_ctrl_low_ok(ctrl_byte)) {
		return RADIANT_FRAME_ECTRL;
	}

	body_len = radiant_frame_body_len(RADIANT_FRAME_CFG_LR, payload_len);
	if (body_len < 0) {
		return body_len;
	}
	if (out_len < (size_t)body_len) {
		return RADIANT_FRAME_ETRUNC;
	}

	/* The length counts everything after itself - the radio's own
	 * convention, needing no translation in a backend. */
	out[0] = (uint8_t)(body_len - RADIANT_FRAME_LR_LEN_BIAS);
	out[1] = id->device_type;
	out[2] = id->trans_type;
	out[3] = ctrl_byte;
	if (payload_len > 0u) {
		memcpy(&out[RADIANT_FRAME_HDR_LEN_LR], payload, payload_len);
	}

	return body_len;
}

int radiant_frame_lr_parse(const uint8_t *body, uint8_t body_len,
			   struct radiant_channel_id *id, uint8_t *ctrl_byte,
			   const uint8_t **payload, uint8_t *payload_len)
{
	uint8_t declared;

	if (body == NULL) {
		return RADIANT_FRAME_EINVAL;
	}
	if (body_len < RADIANT_FRAME_HDR_LEN_LR ||
	    body_len > RADIANT_FRAME_LR_BODY_MAX) {
		return RADIANT_FRAME_ETRUNC;
	}

	/* The length byte is CHECKED, not trusted - this keeps a
	 * length-from-body format from being a buffer overrun waiting for a
	 * bit error. The CRC normally catches a corrupted length before it
	 * reaches here, but the residual case is a read past a DMA buffer, so
	 * the delivered length is authoritative and the declared one must
	 * agree. */
	declared = (uint8_t)(body_len - RADIANT_FRAME_LR_LEN_BIAS);
	if (body[RADIANT_FRAME_LR_LEN_OFFSET] != declared) {
		return RADIANT_FRAME_ETRUNC;
	}

	if (!radiant_ctrl_low_ok(body[3])) {
		return RADIANT_FRAME_ECTRL;
	}

	if (id != NULL) {
		/* The device number is NOT here: it's in the on-air address the
		 * matcher consumed. A caller fills it from the matched filter,
		 * as search does with devnum_lo; leaving it alone here beats
		 * writing a zero that would look like an answer. */
		id->device_type = body[1];
		id->trans_type = body[2];
	}
	if (ctrl_byte != NULL) {
		*ctrl_byte = body[3];
	}
	if (payload != NULL) {
		*payload = &body[RADIANT_FRAME_HDR_LEN_LR];
	}
	if (payload_len != NULL) {
		*payload_len = (uint8_t)(body_len - RADIANT_FRAME_HDR_LEN_LR);
	}

	return RADIANT_FRAME_OK;
}

/* ---------------------------------------------------------------------------
 * Encode and decode
 * ---------------------------------------------------------------------------
 */

int radiant_frame_make(struct radiant_frame *f, const struct radiant_channel_id *id,
		   const struct radiant_ctrl_fields *ctrl_fields,
		   const uint8_t *payload, uint8_t payload_len)
{
	uint8_t ctrl;

	if (f == NULL || id == NULL || ctrl_fields == NULL) {
		return RADIANT_FRAME_EINVAL;
	}
	if (payload_len > RADIANT_FRAME_PAYLOAD_MAX) {
		return RADIANT_FRAME_EINVAL;
	}
	if (payload == NULL && payload_len > 0u) {
		return RADIANT_FRAME_EINVAL;
	}

	/* The field model can pack any of 32 flag combinations; eleven have
	 * been on the air. Enforced here, not in radiant_frame_encode(), so a
	 * bench experiment can still set ctrl_byte directly. */
	ctrl = radiant_ctrl_encode(ctrl_fields);
	if (!radiant_ctrl_observed(ctrl)) {
		return RADIANT_FRAME_EINVAL;
	}

	memset(f, 0, sizeof(*f));
	f->id = *id;
	f->payload_len = payload_len;
	f->ctrl_byte = ctrl;
	if (payload_len > 0u) {
		memcpy(f->payload, payload, payload_len);
	}

	return RADIANT_FRAME_OK;
}

int radiant_frame_addr(enum radiant_frame_cfg cfg, const uint8_t net_addr[RADIANT_NET_ADDR_LEN],
		   const struct radiant_channel_id *id, uint8_t *out, size_t out_len)
{
	int addr_len = radiant_frame_addr_len(cfg);

	if (net_addr == NULL || id == NULL || out == NULL) {
		return RADIANT_FRAME_EINVAL;
	}
	if (addr_len < 0) {
		return addr_len;
	}
	if (out_len < (size_t)addr_len) {
		return RADIANT_FRAME_ETRUNC;
	}

	out[0] = net_addr[0];
	out[1] = net_addr[1];
	/* Device number goes out low byte first. */
	out[2] = (uint8_t)(id->device_number & 0xFFu);

	/*
	 * Both longer configurations carry the high byte of the device number;
	 * only tracking has a fifth byte for the device type.
	 *
	 * THE LONG-RANGE ADDRESS IS A FULL DEVICE NUMBER AND NOTHING ELSE: a
	 * long-range channel is never discovered by sweeping (ADR 0007 keeps
	 * discovery on 1 M/RF 57), so its address is always known in full when
	 * armed, and the fourth byte buys eight more bits against false
	 * triggers for free.
	 */
	if (cfg == RADIANT_FRAME_CFG_TRACKING || cfg == RADIANT_FRAME_CFG_LR) {
		out[3] = (uint8_t)((id->device_number >> 8) & 0xFFu);
	}
	if (cfg == RADIANT_FRAME_CFG_TRACKING) {
		out[4] = id->device_type;
	}

	return addr_len;
}

/*
 * Lay out the body. Tracking puts only what the address did not already carry;
 * search carries the rest of the channel ID because its address stopped after
 * devnum_lo.
 */
static int body_write(enum radiant_frame_cfg cfg, const struct radiant_frame *in,
		      uint8_t *body, size_t body_cap)
{
	int hdr = radiant_frame_hdr_len(cfg);
	int body_len = radiant_frame_body_len(cfg, in->payload_len);

	if (hdr < 0) {
		return hdr;
	}
	if (body_len < 0) {
		return body_len;
	}
	if (body_cap < (size_t)body_len) {
		return RADIANT_FRAME_ETRUNC;
	}

	if (cfg == RADIANT_FRAME_CFG_SEARCH) {
		body[0] = (uint8_t)((in->id.device_number >> 8) & 0xFFu);
		body[1] = in->id.device_type;
		body[2] = in->id.trans_type;
		body[3] = in->ctrl_byte;
	} else {
		body[0] = in->id.trans_type;
		body[1] = in->ctrl_byte;
	}

	if (in->payload_len > 0u) {
		memcpy(&body[hdr], in->payload, in->payload_len);
	}

	return body_len;
}

int radiant_frame_encode(enum radiant_frame_cfg cfg, const uint8_t net_addr[RADIANT_NET_ADDR_LEN],
		     const struct radiant_frame *in, struct radiant_frame_wire *out)
{
	struct radiant_frame_wire w;
	int addr_len;
	int body_len;

	if (net_addr == NULL || in == NULL || out == NULL) {
		return RADIANT_FRAME_EINVAL;
	}
	/* REFUSED RATHER THAN APPROXIMATED, explicitly: struct
	 * radiant_frame_wire's body buffer is sized for ANT, and body_write()
	 * below would lay out a long-range frame as a tracking frame (wrong
	 * header, silent truncation). radiant_frame_lr_body() is this
	 * configuration's encoder; there is no shared path. */
	if (cfg == RADIANT_FRAME_CFG_LR) {
		return RADIANT_FRAME_EINVAL;
	}
	if (in->payload_len > RADIANT_FRAME_PAYLOAD_MAX) {
		return RADIANT_FRAME_EINVAL;
	}
	/* Bits 2:0 only. This used to cross-check bits 4:0 against
	 * payload_len + 2, which rejected every valid in-slot frame (0xA2's low
	 * five bits are 2, not the expected 10) - bits 4/3 are sequence/slot
	 * flags, unrelated to payload length.
	 *
	 * The five flags go out untouched, including unmeasured combinations:
	 * radiant_frame_make() refuses to fabricate a control byte, but a
	 * caller setting ctrl_byte directly (e.g. a bench experiment) is not
	 * second-guessed here. */
	if (!radiant_ctrl_low_ok(in->ctrl_byte)) {
		return RADIANT_FRAME_ECTRL;
	}

	memset(&w, 0, sizeof(w));

	addr_len = radiant_frame_addr(cfg, net_addr, &in->id, w.addr, sizeof(w.addr));
	if (addr_len < 0) {
		return addr_len;
	}
	w.addr_len = (uint8_t)addr_len;

	body_len = body_write(cfg, in, w.body, sizeof(w.body));
	if (body_len < 0) {
		return body_len;
	}
	w.body_len = (uint8_t)body_len;

	w.crc = radiant_frame_crc(&w);

	*out = w;
	return RADIANT_FRAME_OK;
}

/* Well formed enough that its bytes can be walked at all. Says nothing about
 * the control byte - that is decode's job, and it is a different error. */
static bool wire_shape_ok(const struct radiant_frame_wire *w)
{
	return w != NULL && addr_len_ok(w->addr_len) &&
	       w->body_len <= RADIANT_FRAME_BODY_MAX;
}

uint16_t radiant_frame_crc(const struct radiant_frame_wire *w)
{
	uint16_t crc;

	if (!wire_shape_ok(w)) {
		return 0;
	}

	crc = radiant_crc16(RADIANT_CRC_INIT, w->addr, w->addr_len);
	crc = radiant_crc16(crc, w->body, w->body_len);

	return crc;
}

bool radiant_frame_crc_ok(const struct radiant_frame_wire *w)
{
	if (!wire_shape_ok(w)) {
		return false;
	}

	return radiant_frame_crc(w) == w->crc;
}

int radiant_frame_decode(enum radiant_frame_cfg cfg, const struct radiant_frame_wire *in,
		     uint32_t flags, struct radiant_frame *out)
{
	struct radiant_frame f;
	int addr_len;
	int hdr;
	uint8_t payload_len;
	uint8_t ctrl_byte;

	if (in == NULL || out == NULL) {
		return RADIANT_FRAME_EINVAL;
	}
	/* Counterpart of the refusal in radiant_frame_encode(): body[0] here is
	 * a transmission type, but on a long-range frame it's the length.
	 * radiant_frame_lr_parse() is that configuration's decoder. */
	if (cfg == RADIANT_FRAME_CFG_LR) {
		return RADIANT_FRAME_EINVAL;
	}
	if (!wire_shape_ok(in)) {
		return RADIANT_FRAME_EINVAL;
	}

	addr_len = radiant_frame_addr_len(cfg);
	hdr = radiant_frame_hdr_len(cfg);
	if (addr_len < 0 || hdr < 0) {
		return RADIANT_FRAME_EINVAL;
	}
	if (in->addr_len != (uint8_t)addr_len) {
		return RADIANT_FRAME_EINVAL;
	}
	if (in->body_len < (uint8_t)hdr) {
		return RADIANT_FRAME_ETRUNC;
	}

	payload_len = (uint8_t)(in->body_len - (uint8_t)hdr);
	if (payload_len > RADIANT_FRAME_PAYLOAD_MAX) {
		return RADIANT_FRAME_EINVAL;
	}

	/* Dispatch, not validation: the byte is carried out to the caller and
	 * radiant_frame_msg_type() names it or returns RADIANT_MSG_UNKNOWN.
	 * Bits 2:0 are the only invariant thing (010 on all 3,104 CRC-valid
	 * Spike B frames) - checking bits 4:0 against payload_len + 2 instead
	 * (as an earlier version did) rejected every valid slave frame. */
	ctrl_byte = in->body[hdr - 1];
	if (!radiant_ctrl_low_ok(ctrl_byte)) {
		return RADIANT_FRAME_ECTRL;
	}

	/* Last, and never optional unless the caller says something else
	 * already did it. A frame that fails here is reported and discarded;
	 * nothing in this module attempts to repair one. */
	if ((flags & RADIANT_FRAME_TRUSTED_CRC) == 0u) {
		if (radiant_frame_crc(in) != in->crc) {
			return RADIANT_FRAME_ECRC;
		}
	}

	memset(&f, 0, sizeof(f));
	f.id.device_number = (uint16_t)in->addr[2];
	if (cfg == RADIANT_FRAME_CFG_TRACKING) {
		f.id.device_number |= (uint16_t)((uint16_t)in->addr[3] << 8);
		f.id.device_type = in->addr[4];
		f.id.trans_type = in->body[0];
	} else {
		f.id.device_number |= (uint16_t)((uint16_t)in->body[0] << 8);
		f.id.device_type = in->body[1];
		f.id.trans_type = in->body[2];
	}
	f.ctrl_byte = ctrl_byte;
	f.payload_len = payload_len;
	if (payload_len > 0u) {
		memcpy(f.payload, &in->body[hdr], payload_len);
	}

	*out = f;
	return RADIANT_FRAME_OK;
}

/* ---------------------------------------------------------------------------
 * The flat form
 * ---------------------------------------------------------------------------
 */

int radiant_frame_to_bytes(const struct radiant_frame_wire *w, uint8_t *out, size_t out_len)
{
	size_t total;

	if (out == NULL || !wire_shape_ok(w)) {
		return RADIANT_FRAME_EINVAL;
	}

	total = (size_t)w->addr_len + (size_t)w->body_len + RADIANT_FRAME_CRC_BYTES;
	if (out_len < total) {
		return RADIANT_FRAME_ETRUNC;
	}

	memcpy(out, w->addr, w->addr_len);
	memcpy(&out[w->addr_len], w->body, w->body_len);
	/* Most significant byte first, which is the order it is on air. */
	out[w->addr_len + w->body_len] = (uint8_t)(w->crc >> 8);
	out[w->addr_len + w->body_len + 1u] = (uint8_t)(w->crc & 0xFFu);

	return (int)total;
}

int radiant_frame_from_bytes(enum radiant_frame_cfg cfg, const uint8_t *buf, size_t len,
			 struct radiant_frame_wire *out)
{
	struct radiant_frame_wire w;
	int addr_len = radiant_frame_addr_len(cfg);
	int hdr = radiant_frame_hdr_len(cfg);
	size_t total;
	uint8_t ctrl_byte;

	if (buf == NULL || out == NULL) {
		return RADIANT_FRAME_EINVAL;
	}
	if (addr_len < 0 || hdr < 0) {
		return RADIANT_FRAME_EINVAL;
	}
	/* The flat form is 17 fixed bytes and this function's whole contract is
	 * that the geometry comes from cfg. A long-range frame's length comes
	 * from its own body, so it has no place here at all. */
	if (cfg == RADIANT_FRAME_CFG_LR) {
		return RADIANT_FRAME_EINVAL;
	}

	/* The geometry comes from cfg, not a length byte: there is no length
	 * anywhere in an ANT frame (the PCNF0.LFLEN=8 defect, moved into
	 * software, is what taking it from bits 4:0 would reproduce). Both
	 * configurations are static at the standard payload, so a frame is 17
	 * bytes: 5+2+8+2 tracking, 3+4+8+2 search. */
	total = (size_t)addr_len + (size_t)hdr + RADIANT_FRAME_PAYLOAD_STD +
		RADIANT_FRAME_CRC_BYTES;
	if (len < total) {
		return RADIANT_FRAME_ETRUNC;
	}

	/* The one thing the byte itself has to satisfy. */
	ctrl_byte = buf[(size_t)addr_len + (size_t)hdr - 1u];
	if (!radiant_ctrl_low_ok(ctrl_byte)) {
		return RADIANT_FRAME_ECTRL;
	}

	memset(&w, 0, sizeof(w));
	w.addr_len = (uint8_t)addr_len;
	memcpy(w.addr, buf, w.addr_len);
	w.body_len = (uint8_t)((size_t)hdr + RADIANT_FRAME_PAYLOAD_STD);
	memcpy(w.body, &buf[w.addr_len], w.body_len);
	w.crc = (uint16_t)(((uint16_t)buf[total - 2u] << 8) | buf[total - 1u]);

	*out = w;
	return (int)total;
}
