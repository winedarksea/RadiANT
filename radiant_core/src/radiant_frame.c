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
 * any adopter-gated ANT+ device profile document.
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

/*
 * _Static_assert rather than Zephyr's BUILD_ASSERT, because this file
 * deliberately includes no Zephyr header. C11 gives it to us directly.
 */

/* The frame must fit inside the HAL's advisory buffer ceilings, or a backend
 * sized from those would silently truncate the largest frame this module can
 * produce. */
_Static_assert(RADIANT_FRAME_BODY_MAX <= RADIANT_RADIO_BODY_MAX,
	       "radiant_frame body exceeds the HAL's advisory body ceiling");
_Static_assert(RADIANT_FRAME_ADDR_MAX <= RADIANT_RADIO_ADDR_MAX,
	       "radiant_frame address exceeds the HAL's address ceiling");

/*
 * The 15-byte coverage invariant, at compile time as well as in the tests.
 * Tracking is 5 address + 2 header, search is 3 address + 4 header; they are
 * both 7, so both configurations cover the same 15 bytes for a standard frame
 * and produce the same CRC over them. If someone changes one of the four
 * numbers this stops the build, which is a better place to find out than the
 * air.
 */
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

/*
 * Bitwise, not table-driven. A 512-byte table would buy about 8x on a
 * calculation that runs over 15 bytes at 4 Hz per channel - roughly 2 us of
 * Cortex-M4 time per frame even at 32 channels - and would spend flash on a
 * part where flash is the scarce resource and where the CRC is done in
 * hardware anyway on the backend that matters. If a software-CRC backend ever
 * makes this hot, the table goes behind a Kconfig symbol and this comment is
 * the record of why it was not there from the start.
 */
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
 * The control byte
 *
 * Encoded and decoded as six fields, because that is what it is. But the field
 * model can express 256 bytes and only eleven have ever been transmitted, so
 * the model is paired with a table of the measured eleven and every place that
 * chooses a byte for the air is checked against it. Encoding by shift-and-mask
 * with nothing behind it is how the other 245 get invented; a table with no
 * arithmetic is how bit 4 stayed invisible through six runs of part 1. Both
 * halves are needed.
 * ---------------------------------------------------------------------------
 */

/*
 * The eleven values measured on air, in ascending order.
 * archive/captures/radio/2026-08-09-spike-b2-run{0,A,B,C}.log; the first three
 * also appear in all six runs of part 1. Nothing may be added here that has not
 * been captured.
 */
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

/*
 * Every measured value has 010 in bits 2:0 - 0 exceptions in 2,354 CRC-valid
 * frames in part 2 and 750 in part 1. If a row is ever added that does not,
 * the invariant the whole decode path rests on has changed and the build should
 * stop rather than the air find out.
 */
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

/*
 * The four measured data -> acknowledgement pairs, and no fifth. Every one of
 * the 168 adjacent CRC-valid pairs in runs 0, A and B is one of these: bit 6
 * set, bit 5 echoed, bit 4 complemented, bits 7 and 3 unchanged.
 *
 * A slot opener is deliberately absent. All four measured pairs are in-slot
 * frames answered by in-slot frames, so what bit 3 of an acknowledgement to
 * 0x8A or 0xAA would be is not measured, and this table does not invent it.
 */
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
	 * legal control byte, so a caller that ignores it puts a frame on the
	 * air no receiver accepts rather than a plausible wrong one. */
	return 0;
}

enum radiant_msg_type radiant_frame_msg_type(uint8_t ctrl_byte)
{
	if (!radiant_ctrl_low_ok(ctrl_byte)) {
		return RADIANT_MSG_UNKNOWN;
	}

	if (!radiant_ctrl_is_exchange(ctrl_byte)) {
		/*
		 * b7 = 0 occurred only ever with b6 = b5 = 0, across all 3,104
		 * CRC-valid frames of both parts: bits 7:5 never took 001, 010
		 * or 011. So the only broadcast encoding is 0x0A, and anything
		 * else with b7 clear is a combination nothing has transmitted.
		 */
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
	default:
		return RADIANT_FRAME_EINVAL;
	}
}

int radiant_frame_body_len(enum radiant_frame_cfg cfg, uint8_t payload_len)
{
	int hdr = radiant_frame_hdr_len(cfg);

	if (hdr < 0) {
		return hdr;
	}
	if (payload_len > RADIANT_FRAME_PAYLOAD_MAX) {
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
 * The two formats a HAL arm call takes. BOTH are static-length, and tracking
 * is static-length because of Spike B rather than because of the hardware.
 *
 * Tracking used to be RADIANT_LEN_FROM_BODY with a bias of zero, on the reading
 * that byte 3 was a length that happened to count the CRC bytes. It is a
 * control byte. A backend that maps RADIANT_LEN_FROM_BODY onto the obvious
 * register - nRF PCNF0.LFLEN=8 - reads an acknowledged frame's 0xAA as
 * LENGTH=170, overruns MAXLEN and discards it as a CRC error, so it receives
 * every broadcast perfectly and drops every acknowledged and burst frame
 * without a symptom. On transmit the same field would try to send 170 bytes.
 * Acknowledged data is how a trainer's resistance gets set, so that bug
 * surfaces as "ERG mode does not work" months downstream of the line that
 * caused it. It is not worth one register bit.
 *
 * Spike B part 2 makes that permanent rather than merely well-evidenced. Byte 3
 * reads 0x0A on a broadcast - low five bits 01010 = 10 - and 0xA2 on the same
 * bench's slave frames - low five bits 00010 = 2 - and BOTH carry exactly eight
 * payload bytes. A hardware length field cannot parse a byte that says 10 and 2
 * for the same payload. There is nothing left to configure a length field
 * with, on this part or any other.
 *
 * Fixed length is the PCNF0=0 / STATLEN form, measured byte-identical on air
 * to the CRCINC form (40/40 CRC-valid both ways in Spike A; 750 frames of
 * part 1 and 2,354 of part 2 captured in exactly this configuration). It puts
 * byte 3 in RAM where software reads it, and on transmit lets software choose
 * it.
 *
 * The cost, and it is the same one search already carried: a payload other
 * than 8 bytes is not receivable under either format and would need one of its
 * own. Nothing has ever put such a frame on the air - both parts of Spike B
 * enabled advanced burst, sent 24-byte blocks, and watched the stack fragment
 * them into 8-byte on-air packets, with the sequence bit alternating per packet
 * rather than per block.
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

const struct radiant_pkt_format *radiant_frame_format(enum radiant_frame_cfg cfg)
{
	switch (cfg) {
	case RADIANT_FRAME_CFG_TRACKING:
		return &fmt_tracking;
	case RADIANT_FRAME_CFG_SEARCH:
		return &fmt_search;
	default:
		return NULL;
	}
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

	/*
	 * The field model would happily pack any of 32 flag combinations.
	 * Eleven have been on the air. This is the one place that difference is
	 * enforced, and it is enforced here rather than in radiant_frame_encode()
	 * so that a bench experiment can still put a candidate encoding on the
	 * air by setting ctrl_byte directly.
	 */
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

	if (cfg == RADIANT_FRAME_CFG_TRACKING) {
		out[3] = (uint8_t)((id->device_number >> 8) & 0xFFu);
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
	if (in->payload_len > RADIANT_FRAME_PAYLOAD_MAX) {
		return RADIANT_FRAME_EINVAL;
	}
	/*
	 * Bits 2:0, and nothing else. This used to cross-check bits 4:0 against
	 * payload_len + 2, which rejects every valid in-slot frame: 0xA2's low
	 * five bits are 00010 = 2 and no check expecting 10 accepts it. Bits 4
	 * and 3 are the sequence and slot flags and have nothing to do with the
	 * payload.
	 *
	 * The five flags go out untouched, including combinations nothing has
	 * measured: this module refuses to fabricate a control byte in
	 * radiant_frame_make(), but a caller who has evidence for one and sets
	 * ctrl_byte directly is not second-guessed here. Validating the flags
	 * on the transmit path would only mean that the module has to be edited
	 * before a bench experiment can put a candidate encoding on the air,
	 * which is backwards.
	 */
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

	/*
	 * Dispatch, not validation. A tracking channel sees eleven values and
	 * one day may see more; the byte is carried out to the caller and
	 * radiant_frame_msg_type() names it or returns RADIANT_MSG_UNKNOWN.
	 *
	 * Bits 2:0 are the only thing checked, because they are the only thing
	 * that is invariant - 010 on every one of the 3,104 CRC-valid frames of
	 * both parts of Spike B. The version of this line written against
	 * part 1 cross-checked bits 4:0 against payload_len + 2 and therefore
	 * rejected EVERY valid slave frame, which is a broadcast-only receiver
	 * reintroduced in software one layer above the register that used to
	 * hold it.
	 */
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

	/*
	 * The geometry comes from cfg. It has to: there is no length anywhere
	 * in an ANT frame, and the byte that used to supply one here reads 10
	 * on a broadcast and 2 on a slave's in-slot frame for the same eight
	 * payload bytes. Taking the length from bits 4:0 parsed broadcasts
	 * correctly and rejected everything else - the PCNF0.LFLEN=8 defect,
	 * moved into software.
	 *
	 * Both configurations are static at the standard payload, so a frame is
	 * 17 bytes: 5 + 2 + 8 + 2 tracking, 3 + 4 + 8 + 2 search.
	 */
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
