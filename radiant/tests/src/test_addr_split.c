/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, written against
 * radiant/src/radiant_addr_split.h.
 *
 * The nRF backend can match a TRACKING frame's first N address bytes in
 * hardware and the rest here (ADR 0019). Every way this goes wrong is silent on
 * the air: a compare that accepts a foreign frame attributes another sensor's
 * data to this channel; one that rejects our own frame is packet loss; a strip
 * off by one byte hands the core a shifted body whose CRC already passed; a
 * t_sync not corrected moves every slot, every ACK reply window and every
 * 0xE0 timestamp by the bytes' airtime.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_frame.h>
#include <radiant/radiant_radio_hal.h>
#include "../../src/radiant_addr_split.h"

ZTEST_SUITE(radiant_addr_split, NULL, NULL, NULL, NULL, NULL);

/* [A6 C5 dnl dnh dtype], the on-air TRACKING address. */
static void mk(struct radiant_rx_filter *f, uint16_t dn, uint8_t dtype)
{
	f->addr[0] = 0xA6;
	f->addr[1] = 0xC5;
	f->addr[2] = (uint8_t)(dn & 0xFFu);
	f->addr[3] = (uint8_t)(dn >> 8);
	f->addr[4] = dtype;
	f->addr_len = 5u;
	f->wild_last = false;
}

ZTEST(radiant_addr_split, test_hw_len_only_shortens_tracking_on_1m)
{
	/* The tracking address on 1 M takes the configured length. */
	zassert_equal(radiant_addr_split_hw_len(5u, true, 2u), 2u);
	zassert_equal(radiant_addr_split_hw_len(5u, true, 3u), 3u);
	zassert_equal(radiant_addr_split_hw_len(5u, true, 4u), 4u);
	/* 5 is today's full match. */
	zassert_equal(radiant_addr_split_hw_len(5u, true, 5u), 5u);
	/* Search's 3-byte format is never touched, nor is the coded PHY. */
	zassert_equal(radiant_addr_split_hw_len(3u, true, 2u), 3u);
	zassert_equal(radiant_addr_split_hw_len(4u, false, 2u), 4u);
	zassert_equal(radiant_addr_split_hw_len(5u, false, 2u), 5u);
	/* Out-of-range configuration falls back to the full match. */
	zassert_equal(radiant_addr_split_hw_len(5u, true, 1u), 5u);
	zassert_equal(radiant_addr_split_hw_len(5u, true, 9u), 5u);
}

ZTEST(radiant_addr_split, test_own_frame_matches_and_strips)
{
	struct radiant_rx_filter f[1];
	/* What the RADIO delivers with a 2-byte hardware match: dnl dnh dtype,
	 * then the 9-byte body. */
	const uint8_t payload[] = { 0x54, 0xD3, 0x0B,
				    0x10, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
				    0x07, 0x08 };

	mk(&f[0], 54100u, 0x0B);
	zassert_equal(radiant_addr_split_match(f, 1u, 2u, 3u, 0u, payload), 0);
	/* The core sees exactly the body it sees with a full hardware match. */
	zassert_equal_ptr(radiant_addr_split_body(payload, 3u), &payload[3]);
	zassert_equal(radiant_addr_split_body(payload, 3u)[0], 0x10);
	/* sw_len 0 (full hardware match) strips nothing. */
	zassert_equal_ptr(radiant_addr_split_body(payload, 0u), payload);
}

ZTEST(radiant_addr_split, test_foreign_frame_is_rejected)
{
	struct radiant_rx_filter f[1];
	/* Same [A6 C5], a different device number: the short hardware match
	 * hears it, the software compare must not. */
	const uint8_t other_dn[] = { 0x74, 0xD3, 0x0B };
	/* Same device number, different device type. */
	const uint8_t other_type[] = { 0x54, 0xD3, 0x78 };
	/* One bit off in the high byte. */
	const uint8_t one_bit[] = { 0x54, 0xD2, 0x0B };

	mk(&f[0], 54100u, 0x0B);
	zassert_equal(radiant_addr_split_match(f, 1u, 2u, 3u, 0u, other_dn), -1);
	zassert_equal(radiant_addr_split_match(f, 1u, 2u, 3u, 0u, other_type), -1);
	zassert_equal(radiant_addr_split_match(f, 1u, 2u, 3u, 0u, one_bit), -1);
}

ZTEST(radiant_addr_split, test_three_byte_hardware_match)
{
	struct radiant_rx_filter f[1];
	const uint8_t ok[] = { 0xD3, 0x0B };
	const uint8_t bad[] = { 0xD3, 0x0C };

	mk(&f[0], 54100u, 0x0B);
	zassert_equal(radiant_addr_split_match(f, 1u, 3u, 2u, 0u, ok), 0);
	zassert_equal(radiant_addr_split_match(f, 1u, 3u, 2u, 0u, bad), -1);
}

ZTEST(radiant_addr_split, test_demux_across_filters_sharing_a_short_address)
{
	struct radiant_rx_filter f[4];
	const uint8_t to2[] = { 0x2A, 0xD3, 0x78 };
	const uint8_t to0[] = { 0x54, 0xD3, 0x0B };
	const uint8_t none[] = { 0x55, 0xD3, 0x0B };

	mk(&f[0], 54100u, 0x0B);
	mk(&f[1], 43634u, 0x11);
	mk(&f[2], 54058u, 0x78);
	mk(&f[3], 21845u, 0x0B);

	/* With a 2-byte hardware match every filter shares [A6 C5], so whichever
	 * filter RXMATCH names, the frame goes to the one whose device number it
	 * carries - and the answer does not depend on the hint. */
	for (uint8_t hint = 0u; hint < 4u; hint++) {
		zassert_equal(radiant_addr_split_match(f, 4u, 2u, 3u, hint, to2), 2,
			      "hint %u", hint);
		zassert_equal(radiant_addr_split_match(f, 4u, 2u, 3u, hint, to0), 0,
			      "hint %u", hint);
		zassert_equal(radiant_addr_split_match(f, 4u, 2u, 3u, hint, none), -1,
			      "hint %u", hint);
	}
}

ZTEST(radiant_addr_split, test_demux_respects_the_hardware_part)
{
	struct radiant_rx_filter f[2];
	const uint8_t head[] = { 0xD3, 0x0B };

	/* 3-byte hardware match: the two filters differ in dnl, which the
	 * hardware matched. A frame the hardware matched on filter 1's address
	 * must not be handed to filter 0 even though their software bytes
	 * (dnh, dtype) agree. */
	mk(&f[0], 54100u, 0x0B);   /* dnl 0x54 */
	mk(&f[1], 54101u, 0x0B);   /* dnl 0x55 */
	zassert_equal(radiant_addr_split_match(f, 2u, 3u, 2u, 1u, head), 1);
	zassert_equal(radiant_addr_split_match(f, 2u, 3u, 2u, 0u, head), 0);
}

ZTEST(radiant_addr_split, test_refuses_nonsense)
{
	struct radiant_rx_filter f[1];
	const uint8_t head[] = { 0x54, 0xD3, 0x0B };

	mk(&f[0], 54100u, 0x0B);
	zassert_equal(radiant_addr_split_match(NULL, 1u, 2u, 3u, 0u, head), -1);
	zassert_equal(radiant_addr_split_match(f, 1u, 2u, 3u, 0u, NULL), -1);
	zassert_equal(radiant_addr_split_match(f, 0u, 2u, 3u, 0u, head), -1);
	/* hint out of range */
	zassert_equal(radiant_addr_split_match(f, 1u, 2u, 3u, 1u, head), -1);
	/* lengths that do not add up to the filter's */
	zassert_equal(radiant_addr_split_match(f, 1u, 2u, 2u, 0u, head), -1);
	zassert_equal(radiant_addr_split_match(f, 1u, 3u, 3u, 0u, head), -1);
}

ZTEST(radiant_addr_split, test_alt_run_counts_on_air_order)
{
	struct radiant_rx_filter f;

	/* The sweep's numbers (power, dtype 0x0B) and the run the sweep
	 * measured for each: C5's trailing 0101 joins dnl when dnl starts 010. */
	static const struct { uint16_t dn; uint8_t run; } v[] = {
		{ 21845u, 21u }, { 54101u, 12u }, { 54100u, 11u }, { 43634u, 11u },
		{ 54102u, 10u }, { 54096u, 9u },  { 54186u, 9u },  { 54058u, 8u },
		{ 54080u, 7u },  { 54132u, 6u },  { 54900u, 6u },
	};

	for (size_t i = 0; i < ARRAY_SIZE(v); i++) {
		mk(&f, v[i].dn, 0x0B);
		zassert_equal(radiant_addr_split_alt_run(f.addr, 5u), v[i].run,
			      "dn %u", v[i].dn);
	}
	/* Across a byte boundary, and the degenerate cases. */
	/* 0000[1010 1010]0000: bits 3..11 alternate - 9, across the boundary. */
	zassert_equal(radiant_addr_split_alt_run((const uint8_t[]){ 0x0A, 0xA0 },
						 2u), 9u);
	zassert_equal(radiant_addr_split_alt_run((const uint8_t[]){ 0x00 }, 1u),
		      1u);
	zassert_equal(radiant_addr_split_alt_run(NULL, 5u), 0u);
	zassert_equal(radiant_addr_split_alt_run(f.addr, 0u), 0u);
}

ZTEST(radiant_addr_split, test_guard_shortens_only_at_risk_windows)
{
	struct radiant_rx_filter f[2];

	/* A clean address keeps today's full match - the noise cost of a
	 * short one is not paid where nothing is bought back. */
	mk(&f[0], 54132u, 0x0B);
	zassert_equal(radiant_addr_split_rx_hw_len(f, 1u, 5u, true, 5u, 9u), 5u);
	/* At-risk addresses match only [A6 C5]. */
	mk(&f[0], 54100u, 0x0B);
	zassert_equal(radiant_addr_split_rx_hw_len(f, 1u, 5u, true, 5u, 9u), 2u);
	mk(&f[0], 54096u, 0x0B);   /* exactly at the threshold */
	zassert_equal(radiant_addr_split_rx_hw_len(f, 1u, 5u, true, 5u, 9u), 2u);
	mk(&f[0], 54058u, 0x0B);   /* one below */
	zassert_equal(radiant_addr_split_rx_hw_len(f, 1u, 5u, true, 5u, 9u), 5u);
	/* BALEN is per window: one at-risk filter shortens it for all. */
	mk(&f[0], 54132u, 0x0B);
	mk(&f[1], 43634u, 0x78);
	zassert_equal(radiant_addr_split_rx_hw_len(f, 2u, 5u, true, 5u, 9u), 2u);
	zassert_equal(radiant_addr_split_rx_hw_len(f, 1u, 5u, true, 5u, 9u), 5u);
	/* Guard off, or nothing to shorten: the fixed rule alone. */
	mk(&f[0], 54100u, 0x0B);
	zassert_equal(radiant_addr_split_rx_hw_len(f, 1u, 5u, true, 5u, 0u), 5u);
	zassert_equal(radiant_addr_split_rx_hw_len(f, 1u, 5u, true, 3u, 0u), 3u);
	zassert_equal(radiant_addr_split_rx_hw_len(f, 1u, 5u, false, 5u, 9u), 5u);
	zassert_equal(radiant_addr_split_rx_hw_len(f, 1u, 3u, true, 5u, 9u), 3u);
	zassert_equal(radiant_addr_split_rx_hw_len(NULL, 1u, 5u, true, 5u, 9u),
		      5u);
}

/* A frame captured on the nRF54L15 DK (2026-11-07, #43634, 3-byte hardware
 * match, p4_n3b_dn43634): the RADIO dropped one bit of dnh's 0xAA. */
static const uint8_t cap43634[] = { 0xa4, 0x16, 0x0a, 0x14, 0x20, 0x71,
				    0xfe, 0xa1, 0xd2, 0x2a, 0xca, 0x01 };
static const uint32_t cap43634_rxcrc = 0xe6edu;
/* What was sent, from the repair done offline. */
static const uint8_t sent43634_body[] = { 0x05, 0x0a, 0x10, 0x38, 0xff,
					  0x50, 0xe9, 0x15, 0x65, 0x00 };

ZTEST(radiant_addr_split, test_repair_recovers_a_captured_slip)
{
	struct radiant_rx_filter f[1];
	uint8_t body[10];

	mk(&f[0], 43634u, 0x0B);
	/* The plain compare rejects it - that is the loss. */
	zassert_equal(radiant_addr_split_match(f, 1u, 3u, 2u, 0u, cap43634), -1);
	memset(body, 0xEE, sizeof(body));
	zassert_equal(radiant_addr_split_repair(f, 1u, 3u, 2u, 0u, cap43634,
						10u, cap43634_rxcrc,
						RADIANT_CRC_INIT, body), 0);
	zassert_mem_equal(body, sent43634_body, sizeof(body));
}

/*
 * Build what the RADIO stores when the bit at `del` of the software part is
 * dropped: the original software bits (sw address, body, CRC) one place early
 * from there on, then `tail` (whatever followed the frame) last.
 */
static void slip(const uint8_t *orig, uint32_t nbits, uint32_t del,
		 uint8_t tail, uint8_t *out_payload, uint32_t payload_len,
		 uint32_t *out_rxcrc)
{
	uint8_t obs[RADIANT_RADIO_ADDR_MAX + RADIANT_RADIO_BODY_MAX + 2];

	memset(obs, 0, sizeof(obs));
	for (uint32_t i = 0; i < nbits; i++) {
		uint32_t src = (i < del) ? i : i + 1u;
		uint8_t b = (src < nbits)
			? (uint8_t)((orig[src >> 3] >> (7u - (src & 7u))) & 1u)
			: tail;
		obs[i >> 3] |= (uint8_t)(b << (7u - (i & 7u)));
	}
	memcpy(out_payload, obs, payload_len);
	*out_rxcrc = ((uint32_t)obs[payload_len] << 8) | obs[payload_len + 1u];
}

/* The software part of a sent 54100 frame: dnl dnh dtype, body, CRC. */
static void sent54100(uint8_t sw[15], const uint8_t body[10])
{
	const uint8_t addr[] = { 0xA6, 0xC5, 0x54, 0xD3, 0x0B };
	uint16_t crc;

	memcpy(sw, &addr[2], 3);
	memcpy(&sw[3], body, 10);
	crc = radiant_crc16(radiant_crc16(RADIANT_CRC_INIT, addr, 5u), body,
			    10u);
	sw[13] = (uint8_t)(crc >> 8);
	sw[14] = (uint8_t)crc;
}

ZTEST(radiant_addr_split, test_repair_any_deletion_in_the_address_bytes)
{
	struct radiant_rx_filter f[1];
	const uint8_t sent_body[10] = { 0x05, 0x0a, 0x10, 0x66, 0xff,
					0x4d, 0xca, 0x27, 0x66, 0x00 };
	uint8_t sw[15], payload[13], body[10];
	uint32_t rxcrc;
	uint32_t repaired = 0, changed = 0;

	mk(&f[0], 54100u, 0x0B);
	sent54100(sw, sent_body);
	/* Every deletion position in dnl dnh dtype, either trailing bit. A
	 * deletion that leaves the address bits unchanged (inside a run of
	 * equal bits that continues to the end) is not a slip the compare
	 * sees, so it is not counted. */
	for (uint32_t del = 0; del < 24u; del++) {
		for (uint8_t tail = 0; tail < 2u; tail++) {
			slip(sw, 120u, del, tail, payload, sizeof(payload), &rxcrc);
			if (radiant_addr_split_match(f, 1u, 2u, 3u, 0u, payload) == 0) {
				continue;
			}
			changed++;
			memset(body, 0, sizeof(body));
			zassert_equal(radiant_addr_split_repair(
					      f, 1u, 2u, 3u, 0u, payload, 10u, rxcrc,
					      RADIANT_CRC_INIT, body), 0,
				      "del %u tail %u", del, tail);
			zassert_mem_equal(body, sent_body, sizeof(body),
					  "del %u", del);
			repaired++;
		}
	}
	zassert_true(changed >= 40u, "only %u slips changed the address", changed);
	zassert_equal(repaired, changed);
}

ZTEST(radiant_addr_split, test_repair_refuses_what_is_not_one_slip)
{
	struct radiant_rx_filter f[2];
	const uint8_t sent_body[10] = { 0x05, 0x0a, 0x10, 0x66, 0xff,
					0x4d, 0xca, 0x27, 0x66, 0x00 };
	uint8_t sw[15], payload[13], body[10];
	uint32_t rxcrc;

	mk(&f[0], 54100u, 0x0B);
	sent54100(sw, sent_body);
	slip(sw, 120u, 5u, 0u, payload, sizeof(payload), &rxcrc);

	/* A good slip, then the same with one body bit flipped: the CRC
	 * stops it. */
	zassert_equal(radiant_addr_split_repair(f, 1u, 2u, 3u, 0u, payload, 10u,
						rxcrc, RADIANT_CRC_INIT, body), 0);
	payload[8] ^= 0x10u;
	zassert_equal(radiant_addr_split_repair(f, 1u, 2u, 3u, 0u, payload, 10u,
						rxcrc, RADIANT_CRC_INIT, body), -1);
	payload[8] ^= 0x10u;
	/* Wrong CRC seed: refused. */
	zassert_equal(radiant_addr_split_repair(f, 1u, 2u, 3u, 0u, payload, 10u,
						rxcrc, 0x0000u, body), -1);

	/* An unslipped frame from another sensor is not one deletion away. */
	mk(&f[1], 54132u, 0x0B);
	{
		const uint8_t foreign[13] = { 0x74, 0xD3, 0x0B, 0x05, 0x0a, 0x10,
					      0x66, 0xff, 0x4d, 0xca, 0x27, 0x66,
					      0x00 };

		zassert_equal(radiant_addr_split_repair(f, 1u, 2u, 3u, 0u, foreign,
							10u, 0x1234u,
							RADIANT_CRC_INIT, body),
			      -1);
	}
	/* Our own frame received correctly is not a slip either (the plain
	 * compare handles it; a CRC failure there is not repaired here). */
	zassert_equal(radiant_addr_split_repair(f, 1u, 2u, 3u, 0u, sw, 10u,
						((uint32_t)sw[13] << 8) | sw[14],
						RADIANT_CRC_INIT, body), -1);

	/* The slip is attributed to the filter it repairs to, whatever the
	 * hint: f[1] is 54132, f[0] 54100. */
	zassert_equal(radiant_addr_split_repair(f, 2u, 2u, 3u, 1u, payload, 10u,
						rxcrc, RADIANT_CRC_INIT, body), 0);

	/* Nonsense in, -1 out. */
	zassert_equal(radiant_addr_split_repair(NULL, 1u, 2u, 3u, 0u, payload,
						10u, rxcrc, RADIANT_CRC_INIT,
						body), -1);
	zassert_equal(radiant_addr_split_repair(f, 1u, 2u, 0u, 0u, payload, 10u,
						rxcrc, RADIANT_CRC_INIT, body),
		      -1);
	zassert_equal(radiant_addr_split_repair(f, 1u, 2u, 3u, 0u, payload, 0u,
						rxcrc, RADIANT_CRC_INIT, body),
		      -1);
	zassert_equal(radiant_addr_split_repair(f, 1u, 3u, 3u, 0u, payload, 10u,
						rxcrc, RADIANT_CRC_INIT, body),
		      -1);
}

ZTEST(radiant_addr_split, test_t_sync_offset_is_the_bytes_airtime)
{
	/* ADDRESS fires after the hardware's last byte; t_sync is the end of
	 * the fifth. 8 us per byte at 1 Mbit/s. */
	zassert_equal(radiant_addr_split_t_sync_us(0u), 0u);
	zassert_equal(radiant_addr_split_t_sync_us(1u), 8u);
	zassert_equal(radiant_addr_split_t_sync_us(2u), 16u);
	zassert_equal(radiant_addr_split_t_sync_us(3u), 24u);
}

/* ---------------------------------------------------------------------------
 * Wildcard search (ADR 0022): [A6 C5] in hardware, devnum_lo as data
 * ---------------------------------------------------------------------------
 */

/* [A6 C5 0x00], the 3-byte search address, wild in its last byte. */
static void mk_wild(struct radiant_rx_filter *f)
{
	memset(f, 0, sizeof(*f));
	f->addr[0] = 0xA6;
	f->addr[1] = 0xC5;
	f->addr_len = 3u;
	f->wild_last = true;
}

ZTEST(radiant_addr_split, test_wild_window_leaves_the_last_byte_to_software)
{
	struct radiant_rx_filter w;
	struct radiant_rx_filter plain[1];

	mk_wild(&w);
	/* 3-byte search, 1 M: two bytes in hardware whatever the slip-guard
	 * settings, so devnum_lo always arrives as data. */
	zassert_equal(radiant_addr_split_rx_hw_len(&w, 1u, 3u, true, 5u, 0u), 2u);
	zassert_equal(radiant_addr_split_rx_hw_len(&w, 1u, 3u, true, 5u, 9u), 2u);
	/* Not on the coded PHY, which has no movable match boundary. */
	zassert_equal(radiant_addr_split_rx_hw_len(&w, 1u, 3u, false, 5u, 0u), 3u);
	/* A plain search filter keeps the full 3-byte hardware match. */
	memset(plain, 0, sizeof(plain));
	plain[0].addr[0] = 0xA6;
	plain[0].addr[1] = 0xC5;
	plain[0].addr[2] = 0x09;
	plain[0].addr_len = 3u;
	zassert_equal(radiant_addr_split_rx_hw_len(plain, 1u, 3u, true, 5u, 9u),
		      3u);
}

ZTEST(radiant_addr_split, test_wild_filter_accepts_every_devnum_lo)
{
	struct radiant_rx_filter w[1];

	mk_wild(&w[0]);
	for (unsigned v = 0; v < 256u; v++) {
		const uint8_t head[] = { (uint8_t)v, 0x33 };

		zassert_equal(radiant_addr_split_match(w, 1u, 2u, 1u, 0u, head), 0,
			      "devnum_lo 0x%02x", v);
		zassert_equal(radiant_addr_split_wild_value(head, 1u), (uint8_t)v);
	}
	/* Nothing left to software means nothing to report, and no match. */
	static const uint8_t one55[] = { 0x55 };
	zassert_equal(radiant_addr_split_wild_value(one55, 0u), 0u);
	zassert_equal(radiant_addr_split_match(w, 1u, 3u, 0u, 0u,
					       one55), -1);
}

ZTEST(radiant_addr_split, test_wild_filter_is_never_repaired)
{
	struct radiant_rx_filter w[1];
	uint8_t payload[13] = { 0x09, 0xCC, 0x0B, 0x05 };
	uint8_t body[12];

	mk_wild(&w[0]);
	/* Repair needs the expected address bits; a wild byte has none. */
	zassert_equal(radiant_addr_split_repair(w, 1u, 2u, 1u, 0u, payload, 12u,
						0x1234u, RADIANT_CRC_INIT, body),
		      -1);
}
