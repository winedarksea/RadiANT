/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ant_frame.c - the ANT on-air frame: CRC, address packing, encode, decode.
 *
 * Provenance: clean-room. Written from docs/ant-radio-link.md and
 * docs/spike-a-results.md, from ant_core/include/ant_radio_hal.h, and from
 * public nRF52840/nRF54L15 product specifications for the reasoning about the
 * two packet configurations. The CRC is a textbook bitwise CRC-16/CCITT-FALSE
 * with the parameters recorded in docs/ant-radio-link.md; the only other
 * implementation in this repository is ant_core/spike/rx_raw/src/main.c, which
 * is our own Apache-2.0 code. Nothing here derives from sdk-ant, from
 * libant.a, from disassembly of any binary, from rtl_433's expression, or from
 * any adopter-gated ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * This file includes no Zephyr header and nothing from the application, on
 * purpose. It has to compile as a freestanding translation unit - the gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I ant_core/include -fsyntax-only ant_core/src/ant_frame.c
 *
 * - so that a compile error here is never confused with a Zephyr or a board
 * problem, and so that a future host-side consumer (a capture reader, a
 * conformance tool) can build it with nothing but a C compiler.
 */

#include <string.h>

#include "ant_frame.h"

/*
 * _Static_assert rather than Zephyr's BUILD_ASSERT, because this file
 * deliberately includes no Zephyr header. C11 gives it to us directly.
 */

/* The frame must fit inside the HAL's advisory buffer ceilings, or a backend
 * sized from those would silently truncate the largest frame this module can
 * produce. */
_Static_assert(ANT_FRAME_BODY_MAX <= ANT_RADIO_BODY_MAX,
	       "ant_frame body exceeds the HAL's advisory body ceiling");
_Static_assert(ANT_FRAME_ADDR_MAX <= ANT_RADIO_ADDR_MAX,
	       "ant_frame address exceeds the HAL's address ceiling");

/*
 * The 15-byte coverage invariant, at compile time as well as in the tests.
 * Tracking is 5 address + 2 header, search is 3 address + 4 header; they are
 * both 7, so both configurations cover the same 15 bytes for a standard frame
 * and produce the same CRC over them. If someone changes one of the four
 * numbers this stops the build, which is a better place to find out than the
 * air.
 */
_Static_assert(ANT_FRAME_ADDR_LEN_TRACKING + ANT_FRAME_HDR_LEN_TRACKING ==
		       ANT_FRAME_ADDR_LEN_SEARCH + ANT_FRAME_HDR_LEN_SEARCH,
	       "the two packet configurations no longer cover the same bytes");
_Static_assert(ANT_FRAME_ADDR_LEN_TRACKING + ANT_FRAME_HDR_LEN_TRACKING +
		       ANT_FRAME_PAYLOAD_STD == 15u,
	       "a standard ANT frame no longer has 15 CRC-covered bytes");

const uint8_t ant_net_addr_ant_plus[ANT_NET_ADDR_LEN] = {
	ANT_NET_ADDR_ANT_PLUS_0,
	ANT_NET_ADDR_ANT_PLUS_1,
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
uint16_t ant_crc16(uint16_t seed, const uint8_t *data, size_t len)
{
	uint16_t crc = seed;

	if (data == NULL) {
		return crc;
	}

	for (size_t i = 0; i < len; i++) {
		crc ^= (uint16_t)((uint16_t)data[i] << 8);
		for (unsigned int bit = 0; bit < 8u; bit++) {
			if (crc & 0x8000u) {
				crc = (uint16_t)((uint16_t)(crc << 1) ^ ANT_CRC_POLY);
			} else {
				crc = (uint16_t)(crc << 1);
			}
		}
	}

	return crc;
}

/* ---------------------------------------------------------------------------
 * Bit order and address packing
 * ---------------------------------------------------------------------------
 */

uint8_t ant_rev8(uint8_t b)
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
	return addr_len >= 2u && addr_len <= ANT_FRAME_ADDR_MAX;
}

uint32_t ant_addr_pack_leading(const uint8_t *addr, uint8_t addr_len)
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
		word |= (uint32_t)ant_rev8(addr[i]) << (8u * i);
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

uint8_t ant_addr_pack_trailing(const uint8_t *addr, uint8_t addr_len)
{
	if (addr == NULL || !addr_len_ok(addr_len)) {
		return 0;
	}

	return ant_rev8(addr[addr_len - 1u]);
}

/* ---------------------------------------------------------------------------
 * Geometry
 * ---------------------------------------------------------------------------
 */

int ant_frame_addr_len(enum ant_frame_cfg cfg)
{
	switch (cfg) {
	case ANT_FRAME_CFG_TRACKING:
		return (int)ANT_FRAME_ADDR_LEN_TRACKING;
	case ANT_FRAME_CFG_SEARCH:
		return (int)ANT_FRAME_ADDR_LEN_SEARCH;
	default:
		return ANT_FRAME_EINVAL;
	}
}

int ant_frame_hdr_len(enum ant_frame_cfg cfg)
{
	switch (cfg) {
	case ANT_FRAME_CFG_TRACKING:
		return (int)ANT_FRAME_HDR_LEN_TRACKING;
	case ANT_FRAME_CFG_SEARCH:
		return (int)ANT_FRAME_HDR_LEN_SEARCH;
	default:
		return ANT_FRAME_EINVAL;
	}
}

int ant_frame_body_len(enum ant_frame_cfg cfg, uint8_t payload_len)
{
	int hdr = ant_frame_hdr_len(cfg);

	if (hdr < 0) {
		return hdr;
	}
	if (payload_len > ANT_FRAME_PAYLOAD_MAX) {
		return ANT_FRAME_EINVAL;
	}

	return hdr + (int)payload_len;
}

int ant_frame_covered_len(enum ant_frame_cfg cfg, uint8_t payload_len)
{
	int addr = ant_frame_addr_len(cfg);
	int body = ant_frame_body_len(cfg, payload_len);

	if (addr < 0) {
		return addr;
	}
	if (body < 0) {
		return body;
	}

	return addr + body;
}

/*
 * The two formats a HAL arm call takes.
 *
 * Tracking is length-from-body with a bias of zero, and that zero is not a
 * coincidence: the two header bytes ahead of the payload and the two CRC bytes
 * the length counts cancel exactly, so the on-air length byte *is* the body
 * length for every payload size. Search cannot use the same trick - four
 * header bytes against two CRC bytes do not cancel - and in any case the
 * hardware has no length-field slot with three bytes ahead of it, so it is
 * fixed at the standard 12. A longer search frame is therefore not receivable
 * and would need a format of its own; saying so here is cheaper than
 * discovering it when the first extension frame goes out.
 */
static const struct ant_pkt_format fmt_tracking = {
	.phy = ANT_PHY_1M_GFSK,
	.addr_len = (uint8_t)ANT_FRAME_ADDR_LEN_TRACKING,
	.len_mode = ANT_LEN_FROM_BODY,
	.body_len = 0,
	.len_offset = (uint8_t)(ANT_FRAME_HDR_LEN_TRACKING - 1u),
	.len_bias = 0,
	.max_body_len = (uint8_t)(ANT_FRAME_HDR_LEN_TRACKING + ANT_FRAME_PAYLOAD_MAX),
	.crc = {
		.width_bits = 16,
		.poly = ANT_CRC_POLY,
		.init = ANT_CRC_INIT,
		.xor_out = 0,
		.reflect_in = false,
		.reflect_out = false,
		.cover_addr = true,
	},
};

static const struct ant_pkt_format fmt_search = {
	.phy = ANT_PHY_1M_GFSK,
	.addr_len = (uint8_t)ANT_FRAME_ADDR_LEN_SEARCH,
	.len_mode = ANT_LEN_FIXED,
	.body_len = (uint8_t)(ANT_FRAME_HDR_LEN_SEARCH + ANT_FRAME_PAYLOAD_STD),
	.len_offset = 0,
	.len_bias = 0,
	.max_body_len = (uint8_t)(ANT_FRAME_HDR_LEN_SEARCH + ANT_FRAME_PAYLOAD_STD),
	.crc = {
		.width_bits = 16,
		.poly = ANT_CRC_POLY,
		.init = ANT_CRC_INIT,
		.xor_out = 0,
		.reflect_in = false,
		.reflect_out = false,
		.cover_addr = true,
	},
};

const struct ant_pkt_format *ant_frame_format(enum ant_frame_cfg cfg)
{
	switch (cfg) {
	case ANT_FRAME_CFG_TRACKING:
		return &fmt_tracking;
	case ANT_FRAME_CFG_SEARCH:
		return &fmt_search;
	default:
		return NULL;
	}
}

/* ---------------------------------------------------------------------------
 * Encode and decode
 * ---------------------------------------------------------------------------
 */

int ant_frame_make(struct ant_frame *f, const struct ant_channel_id *id,
		   const uint8_t *payload, uint8_t payload_len)
{
	if (f == NULL || id == NULL) {
		return ANT_FRAME_EINVAL;
	}
	if (payload_len > ANT_FRAME_PAYLOAD_MAX) {
		return ANT_FRAME_EINVAL;
	}
	if (payload == NULL && payload_len > 0u) {
		return ANT_FRAME_EINVAL;
	}

	memset(f, 0, sizeof(*f));
	f->id = *id;
	f->payload_len = payload_len;
	f->len_byte = ant_frame_len_byte(payload_len);
	if (payload_len > 0u) {
		memcpy(f->payload, payload, payload_len);
	}

	return ANT_FRAME_OK;
}

int ant_frame_addr(enum ant_frame_cfg cfg, const uint8_t net_addr[ANT_NET_ADDR_LEN],
		   const struct ant_channel_id *id, uint8_t *out, size_t out_len)
{
	int addr_len = ant_frame_addr_len(cfg);

	if (net_addr == NULL || id == NULL || out == NULL) {
		return ANT_FRAME_EINVAL;
	}
	if (addr_len < 0) {
		return addr_len;
	}
	if (out_len < (size_t)addr_len) {
		return ANT_FRAME_ETRUNC;
	}

	out[0] = net_addr[0];
	out[1] = net_addr[1];
	/* Device number goes out low byte first. */
	out[2] = (uint8_t)(id->device_number & 0xFFu);

	if (cfg == ANT_FRAME_CFG_TRACKING) {
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
static int body_write(enum ant_frame_cfg cfg, const struct ant_frame *in,
		      uint8_t *body, size_t body_cap)
{
	int hdr = ant_frame_hdr_len(cfg);
	int body_len = ant_frame_body_len(cfg, in->payload_len);

	if (hdr < 0) {
		return hdr;
	}
	if (body_len < 0) {
		return body_len;
	}
	if (body_cap < (size_t)body_len) {
		return ANT_FRAME_ETRUNC;
	}

	if (cfg == ANT_FRAME_CFG_SEARCH) {
		body[0] = (uint8_t)((in->id.device_number >> 8) & 0xFFu);
		body[1] = in->id.device_type;
		body[2] = in->id.trans_type;
		body[3] = in->len_byte;
	} else {
		body[0] = in->id.trans_type;
		body[1] = in->len_byte;
	}

	if (in->payload_len > 0u) {
		memcpy(&body[hdr], in->payload, in->payload_len);
	}

	return body_len;
}

int ant_frame_encode(enum ant_frame_cfg cfg, const uint8_t net_addr[ANT_NET_ADDR_LEN],
		     const struct ant_frame *in, struct ant_frame_wire *out)
{
	struct ant_frame_wire w;
	int addr_len;
	int body_len;

	if (net_addr == NULL || in == NULL || out == NULL) {
		return ANT_FRAME_EINVAL;
	}
	if (in->payload_len > ANT_FRAME_PAYLOAD_MAX) {
		return ANT_FRAME_EINVAL;
	}
	/* Cross-check rather than derive - see struct ant_frame. */
	if (in->len_byte != ant_frame_len_byte(in->payload_len)) {
		return ANT_FRAME_ELEN;
	}

	memset(&w, 0, sizeof(w));

	addr_len = ant_frame_addr(cfg, net_addr, &in->id, w.addr, sizeof(w.addr));
	if (addr_len < 0) {
		return addr_len;
	}
	w.addr_len = (uint8_t)addr_len;

	body_len = body_write(cfg, in, w.body, sizeof(w.body));
	if (body_len < 0) {
		return body_len;
	}
	w.body_len = (uint8_t)body_len;

	w.crc = ant_frame_crc(&w);

	*out = w;
	return ANT_FRAME_OK;
}

/* Well formed enough that its bytes can be walked at all. Says nothing about
 * whether the length byte agrees - that is decode's job, and it is a different
 * error. */
static bool wire_shape_ok(const struct ant_frame_wire *w)
{
	return w != NULL && addr_len_ok(w->addr_len) &&
	       w->body_len <= ANT_FRAME_BODY_MAX;
}

uint16_t ant_frame_crc(const struct ant_frame_wire *w)
{
	uint16_t crc;

	if (!wire_shape_ok(w)) {
		return 0;
	}

	crc = ant_crc16(ANT_CRC_INIT, w->addr, w->addr_len);
	crc = ant_crc16(crc, w->body, w->body_len);

	return crc;
}

bool ant_frame_crc_ok(const struct ant_frame_wire *w)
{
	if (!wire_shape_ok(w)) {
		return false;
	}

	return ant_frame_crc(w) == w->crc;
}

int ant_frame_decode(enum ant_frame_cfg cfg, const struct ant_frame_wire *in,
		     uint32_t flags, struct ant_frame *out)
{
	struct ant_frame f;
	int addr_len;
	int hdr;
	uint8_t payload_len;
	uint8_t len_byte;

	if (in == NULL || out == NULL) {
		return ANT_FRAME_EINVAL;
	}
	if (!wire_shape_ok(in)) {
		return ANT_FRAME_EINVAL;
	}

	addr_len = ant_frame_addr_len(cfg);
	hdr = ant_frame_hdr_len(cfg);
	if (addr_len < 0 || hdr < 0) {
		return ANT_FRAME_EINVAL;
	}
	if (in->addr_len != (uint8_t)addr_len) {
		return ANT_FRAME_EINVAL;
	}
	if (in->body_len < (uint8_t)hdr) {
		return ANT_FRAME_ETRUNC;
	}

	payload_len = (uint8_t)(in->body_len - (uint8_t)hdr);
	if (payload_len > ANT_FRAME_PAYLOAD_MAX) {
		return ANT_FRAME_EINVAL;
	}

	len_byte = in->body[hdr - 1];
	if (len_byte != ant_frame_len_byte(payload_len)) {
		return ANT_FRAME_ELEN;
	}

	/* Last, and never optional unless the caller says something else
	 * already did it. A frame that fails here is reported and discarded;
	 * nothing in this module attempts to repair one. */
	if ((flags & ANT_FRAME_TRUSTED_CRC) == 0u) {
		if (ant_frame_crc(in) != in->crc) {
			return ANT_FRAME_ECRC;
		}
	}

	memset(&f, 0, sizeof(f));
	f.id.device_number = (uint16_t)in->addr[2];
	if (cfg == ANT_FRAME_CFG_TRACKING) {
		f.id.device_number |= (uint16_t)((uint16_t)in->addr[3] << 8);
		f.id.device_type = in->addr[4];
		f.id.trans_type = in->body[0];
	} else {
		f.id.device_number |= (uint16_t)((uint16_t)in->body[0] << 8);
		f.id.device_type = in->body[1];
		f.id.trans_type = in->body[2];
	}
	f.len_byte = len_byte;
	f.payload_len = payload_len;
	if (payload_len > 0u) {
		memcpy(f.payload, &in->body[hdr], payload_len);
	}

	*out = f;
	return ANT_FRAME_OK;
}

/* ---------------------------------------------------------------------------
 * The flat form
 * ---------------------------------------------------------------------------
 */

int ant_frame_to_bytes(const struct ant_frame_wire *w, uint8_t *out, size_t out_len)
{
	size_t total;

	if (out == NULL || !wire_shape_ok(w)) {
		return ANT_FRAME_EINVAL;
	}

	total = (size_t)w->addr_len + (size_t)w->body_len + ANT_FRAME_CRC_BYTES;
	if (out_len < total) {
		return ANT_FRAME_ETRUNC;
	}

	memcpy(out, w->addr, w->addr_len);
	memcpy(&out[w->addr_len], w->body, w->body_len);
	/* Most significant byte first, which is the order it is on air. */
	out[w->addr_len + w->body_len] = (uint8_t)(w->crc >> 8);
	out[w->addr_len + w->body_len + 1u] = (uint8_t)(w->crc & 0xFFu);

	return (int)total;
}

int ant_frame_from_bytes(enum ant_frame_cfg cfg, const uint8_t *buf, size_t len,
			 struct ant_frame_wire *out)
{
	struct ant_frame_wire w;
	int addr_len = ant_frame_addr_len(cfg);
	int hdr = ant_frame_hdr_len(cfg);
	size_t min_len;
	size_t total;
	uint8_t len_byte;
	uint8_t payload_len;

	if (buf == NULL || out == NULL) {
		return ANT_FRAME_EINVAL;
	}
	if (addr_len < 0 || hdr < 0) {
		return ANT_FRAME_EINVAL;
	}

	/* Enough to reach the length byte, plus the CRC a zero-payload frame
	 * would still carry. Anything shorter cannot even be interrogated. */
	min_len = (size_t)addr_len + (size_t)hdr + ANT_FRAME_CRC_BYTES;
	if (len < min_len) {
		return ANT_FRAME_ETRUNC;
	}

	len_byte = buf[(size_t)addr_len + (size_t)hdr - 1u];
	if (len_byte < ANT_FRAME_CRC_BYTES) {
		return ANT_FRAME_ELEN;
	}
	payload_len = (uint8_t)(len_byte - ANT_FRAME_CRC_BYTES);
	if (payload_len > ANT_FRAME_PAYLOAD_MAX) {
		return ANT_FRAME_ELEN;
	}

	total = (size_t)addr_len + (size_t)hdr + (size_t)payload_len +
		ANT_FRAME_CRC_BYTES;
	if (len < total) {
		return ANT_FRAME_ETRUNC;
	}

	memset(&w, 0, sizeof(w));
	w.addr_len = (uint8_t)addr_len;
	memcpy(w.addr, buf, w.addr_len);
	w.body_len = (uint8_t)((size_t)hdr + (size_t)payload_len);
	memcpy(w.body, &buf[w.addr_len], w.body_len);
	w.crc = (uint16_t)(((uint16_t)buf[total - 2u] << 8) | buf[total - 1u]);

	*out = w;
	return (int)total;
}
