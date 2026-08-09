/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original work. Every expected value in this file comes from
 * docs/ant-radio-link.md or from docs/spike-a-results.md and the capture logs
 * it cites (archive/captures/radio/2026-08-09-nrf54l15-run*.log) - not from
 * running ant_frame.c and writing down what it did. Nothing here derives from
 * sdk-ant, from libant.a, from disassembly of any binary, or from any
 * adopter-gated ANT+ device profile document.
 *
 * ---------------------------------------------------------------------------
 * The suite for ant_core/src/ant_frame.c
 * ---------------------------------------------------------------------------
 * ant_frame.c is pure logic with no radio in it, so there is no excuse for
 * thin coverage here and no hardware needed to get it. Two things this file
 * tries hard to be:
 *
 *   - *Independent*. The constants below were derived from the documents and
 *     cross-checked with a separate implementation before ant_frame.c existed
 *     in this form. A test whose expected value came out of the code under
 *     test asserts only that the code is deterministic.
 *
 *   - *Capable of going red*. Several tests here name the specific wrong
 *     answer they exist to exclude - the naive short-address truncation, a
 *     silently repaired CRC, a length byte that disagrees with the payload -
 *     rather than merely asserting that the right one comes out. Those were
 *     real documented errors or real design hazards, and an assertion that
 *     cannot distinguish the two answers is not a test of anything.
 *
 * These run only in CI on Linux: native_sim does not build on Windows, so no
 * amount of local work executes them. See ant_core/tests/testcase.yaml.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include "ant_frame.h"

ZTEST_SUITE(ant_frame, NULL, NULL, NULL, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Ground truth
 * ---------------------------------------------------------------------------
 */

/*
 * The synthetic golden vector from docs/ant-radio-link.md - computed, not
 * captured, and re-derived by Spike A's own boot self-test before its radio
 * was touched.
 */
static const uint8_t golden15[] = {
	0xA6, 0xC5, 0x34, 0x12, 0x78, 0x01, 0x0A,
	0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
};
#define GOLDEN_CRC 0x1B12u

/*
 * A frame Spike A actually pulled off the air, from docs/spike-a-results.md
 * ("The frame, byte for byte"), with the assumed preamble byte dropped. This
 * is the strongest vector in the file: a real ANT+ power meter, decoded twice
 * on real silicon, with an independently established channel ID.
 */
static const uint8_t spike15[] = {
	0xA6, 0xC5, 0x17, 0x3A, 0x0B, 0x05, 0x0A,
	0x10, 0xBD, 0xFF, 0x50, 0xDE, 0x11, 0x64, 0x00,
};
#define SPIKE_CRC     0x199Au
#define SPIKE_DEVNUM  14871u   /* 0x3A17 */
#define SPIKE_DTYPE   0x0Bu
#define SPIKE_TTYPE   5u

static const struct ant_channel_id spike_id = {
	.device_number = SPIKE_DEVNUM,
	.device_type = SPIKE_DTYPE,
	.trans_type = SPIKE_TTYPE,
};

static const uint8_t spike_payload[8] = {
	0x10, 0xBD, 0xFF, 0x50, 0xDE, 0x11, 0x64, 0x00,
};

/* ---------------------------------------------------------------------------
 * CRC
 * ---------------------------------------------------------------------------
 */

ZTEST(ant_frame, test_crc_golden_vector)
{
	zassert_equal(ant_crc16(ANT_CRC_INIT, golden15, sizeof(golden15)),
		      GOLDEN_CRC,
		      "CRC-16/CCITT-FALSE over the documented golden vector");

	/* And on the real frame, whose CRC the hardware also computed. */
	zassert_equal(ant_crc16(ANT_CRC_INIT, spike15, sizeof(spike15)),
		      SPIKE_CRC,
		      "software CRC disagrees with the one RADIO->RXCRC reported "
		      "for the captured frame");
}

/*
 * The cheap, decisive form: CCITT-FALSE has no final XOR and no reflection, so
 * appending a correct CRC drives the register to zero. Both forms held on
 * every one of Spike A's 2,164 CRC-valid frames, and they catch different
 * mistakes - a wrong seed shows up in the first, a wrong byte order of the
 * appended CRC only in the second.
 */
ZTEST(ant_frame, test_crc_over_message_plus_crc_is_zero)
{
	uint8_t buf[sizeof(golden15) + 2];

	memcpy(buf, golden15, sizeof(golden15));
	buf[sizeof(golden15)] = (uint8_t)(GOLDEN_CRC >> 8);
	buf[sizeof(golden15) + 1] = (uint8_t)(GOLDEN_CRC & 0xFFu);

	zassert_equal(ant_crc16(ANT_CRC_INIT, buf, sizeof(buf)), 0u,
		      "message + its own CRC did not drive the register to zero");

	/* The negative half, so the test can tell the two answers apart: one
	 * flipped bit anywhere must stop it being zero. */
	buf[7] ^= 0x01u;
	zassert_not_equal(ant_crc16(ANT_CRC_INIT, buf, sizeof(buf)), 0u,
			  "a corrupted frame still recomputed to zero, so this "
			  "check cannot detect corruption at all");
}

/*
 * 0x233E is the register state after A6 C5 from init 0xFFFF. It matters well
 * beyond arithmetic: it is what lets a radio whose CRC engine starts after its
 * sync word - RAIL on EFR32 - fold the constant into CRCINIT and still emit
 * ANT's CRC from hardware. If this ever stops holding, a whole backend loses
 * its hardware CRC, so it is asserted here rather than left in prose.
 */
ZTEST(ant_frame, test_crc_chaining_constant)
{
	static const uint8_t net[2] = { 0xA6, 0xC5 };

	zassert_equal(ant_crc16(ANT_CRC_INIT, net, sizeof(net)),
		      ANT_CRC_AFTER_ANT_PLUS_NET,
		      "the ANT+ network address no longer chains to 0x233E");

	zassert_equal(ant_crc16(ANT_CRC_AFTER_ANT_PLUS_NET, &golden15[2],
				sizeof(golden15) - 2u),
		      GOLDEN_CRC,
		      "chaining from 0x233E over the remaining 13 bytes did not "
		      "reproduce the full-frame CRC");

	zassert_equal(ant_crc16(ANT_CRC_AFTER_ANT_PLUS_NET, &spike15[2],
				sizeof(spike15) - 2u),
		      SPIKE_CRC, "chaining failed on the captured frame");
}

ZTEST(ant_frame, test_crc_seed_and_empty_input)
{
	zassert_equal(ant_crc16(ANT_CRC_INIT, golden15, 0), ANT_CRC_INIT,
		      "zero bytes must leave the seed alone");
	zassert_equal(ant_crc16(0x1234u, NULL, 8), 0x1234u,
		      "a null buffer must not be walked");
}

/* ---------------------------------------------------------------------------
 * Bit order and address packing
 * ---------------------------------------------------------------------------
 */

ZTEST(ant_frame, test_rev8)
{
	/* The four in docs/ant-radio-link.md and docs/spike-a-results.md. */
	zassert_equal(ant_rev8(0xA6u), 0x65u, NULL);
	zassert_equal(ant_rev8(0xC5u), 0xA3u, NULL);
	zassert_equal(ant_rev8(0x0Bu), 0xD0u, NULL);
	zassert_equal(ant_rev8(0x17u), 0xE8u, NULL);
	zassert_equal(ant_rev8(0x3Au), 0x5Cu, NULL);

	/* Boundaries, where an off-by-one in the mask ladder shows up. */
	zassert_equal(ant_rev8(0x00u), 0x00u, NULL);
	zassert_equal(ant_rev8(0xFFu), 0xFFu, NULL);
	zassert_equal(ant_rev8(0x01u), 0x80u, NULL);
	zassert_equal(ant_rev8(0x80u), 0x01u, NULL);

	/* An involution over the whole domain. A wrong-but-plausible bit
	 * permutation - a nibble swap, a rotate - passes several of the cases
	 * above and fails this. */
	for (unsigned int b = 0; b < 256u; b++) {
		zassert_equal(ant_rev8(ant_rev8((uint8_t)b)), (uint8_t)b,
			      "rev8 is not its own inverse at 0x%02X", b);
	}
}

/*
 * The five-byte tracking address for the measured device, whose exact packing
 * received 240-241 frames per minute in six separate windows while all seven
 * other permutations heard literally nothing.
 */
ZTEST(ant_frame, test_addr_pack_tracking)
{
	static const uint8_t addr[5] = { 0xA6, 0xC5, 0x17, 0x3A, 0x0B };

	zassert_equal(ant_addr_pack_leading(addr, sizeof(addr)), 0x5CE8A365u,
		      "leading address word for A6 C5 17 3A 0B");
	zassert_equal(ant_addr_pack_trailing(addr, sizeof(addr)), 0xD0u,
		      "trailing address byte is rev8(device type)");
}

/*
 * The rule the documentation originally got wrong, and the one that costs a
 * bench day. With fewer than four leading bytes the used bytes are the HIGH
 * ones: a short address is truncated from its least significant byte, and
 * under the byte order the rest of the frame establishes those are exactly the
 * bytes that must survive.
 *
 * The naive answer is asserted against by name, because it does not fail
 * quietly - Spike A measured it producing 9 to 20 address matches per window
 * with zero CRC-valid frames at -54 to -101 dBm. That is a matcher firing on
 * noise, and it looks like a working receiver until you look at the CRC column.
 */
ZTEST(ant_frame, test_addr_pack_search_is_shifted_not_truncated)
{
	static const uint8_t addr[3] = { 0xA6, 0xC5, 0x17 };
	uint32_t lead = ant_addr_pack_leading(addr, sizeof(addr));

	zassert_equal(lead, 0xA3650000u,
		      "the two surviving base bytes must sit in the high half");
	zassert_not_equal(lead, 0x0000A365u,
			  "this is the naive low-bytes truncation: it produces a "
			  "receiver that fires on noise and validates nothing");

	zassert_equal(ant_addr_pack_trailing(addr, sizeof(addr)), 0xE8u,
		      "trailing address byte is rev8(devnum_lo) in search");

	/* The same leading bytes, so the same relative order either way. */
	zassert_equal(lead >> 16, 0xA365u,
		      "shifting must not reorder the surviving bytes");
}

/*
 * Written as one shift, the packing collapses to the four-byte case at full
 * length. This is what lets a backend hold one expression rather than two, so
 * it is worth an assertion that the shift really is a no-op there - a backend
 * that special-cased BALEN would be the thing this design exists to avoid.
 */
ZTEST(ant_frame, test_addr_pack_shift_collapses_at_full_length)
{
	static const uint8_t addr[5] = { 0x11, 0x22, 0x33, 0x44, 0x55 };
	uint32_t expect = ((uint32_t)ant_rev8(0x44u) << 24) |
			  ((uint32_t)ant_rev8(0x33u) << 16) |
			  ((uint32_t)ant_rev8(0x22u) << 8) |
			  (uint32_t)ant_rev8(0x11u);

	zassert_equal(ant_addr_pack_leading(addr, sizeof(addr)), expect,
		      "at four leading bytes the shift must vanish");

	/* Three leading bytes: one byte of shift, nothing else moves. */
	zassert_equal(ant_addr_pack_leading(addr, 4u),
		      (((uint32_t)ant_rev8(0x33u) << 16) |
		       ((uint32_t)ant_rev8(0x22u) << 8) |
		       (uint32_t)ant_rev8(0x11u)) << 8,
		      "a four-byte address shifts by exactly one byte");
}

ZTEST(ant_frame, test_addr_pack_rejects_bad_length)
{
	static const uint8_t addr[5] = { 0xA6, 0xC5, 0x17, 0x3A, 0x0B };

	/* One byte cannot be split into leading bytes plus a trailing one, and
	 * nothing planned matches more than five. */
	zassert_equal(ant_addr_pack_leading(addr, 0u), 0u, NULL);
	zassert_equal(ant_addr_pack_leading(addr, 1u), 0u, NULL);
	zassert_equal(ant_addr_pack_leading(addr, 6u), 0u, NULL);
	zassert_equal(ant_addr_pack_leading(NULL, 5u), 0u, NULL);
	zassert_equal(ant_addr_pack_trailing(addr, 1u), 0u, NULL);
	zassert_equal(ant_addr_pack_trailing(NULL, 5u), 0u, NULL);
}

/* ---------------------------------------------------------------------------
 * Geometry - the 15-byte coverage invariant
 * ---------------------------------------------------------------------------
 */

/*
 * Tracking is 5 address + 10 body, search is 3 address + 12 body, and both are
 * 15. This is genuinely useful rather than decorative, because the two
 * configurations put different bytes on the hardware matcher: the equality is
 * what makes the same CRC validate a frame received either way, and a future
 * format that breaks it announces itself here instead of on the air.
 */
ZTEST(ant_frame, test_crc_coverage_is_15_in_both_configs)
{
	zassert_equal(ant_frame_addr_len(ANT_FRAME_CFG_TRACKING), 5, NULL);
	zassert_equal(ant_frame_addr_len(ANT_FRAME_CFG_SEARCH), 3, NULL);
	zassert_equal(ant_frame_body_len(ANT_FRAME_CFG_TRACKING, 8), 10, NULL);
	zassert_equal(ant_frame_body_len(ANT_FRAME_CFG_SEARCH, 8), 12, NULL);

	zassert_equal(ant_frame_covered_len(ANT_FRAME_CFG_TRACKING, 8), 15,
		      "tracking no longer covers 15 bytes");
	zassert_equal(ant_frame_covered_len(ANT_FRAME_CFG_SEARCH, 8), 15,
		      "search no longer covers 15 bytes");

	/* The general form: 7 fixed bytes ahead of the payload, however the
	 * split falls. Checked across every payload length so that the
	 * equality is a property and not a coincidence at 8. */
	for (uint8_t p = 0; p <= ANT_FRAME_PAYLOAD_MAX; p++) {
		zassert_equal(ant_frame_covered_len(ANT_FRAME_CFG_TRACKING, p),
			      ant_frame_covered_len(ANT_FRAME_CFG_SEARCH, p),
			      "coverage diverged at payload length %u", p);
		zassert_equal(ant_frame_covered_len(ANT_FRAME_CFG_TRACKING, p),
			      7 + (int)p, NULL);
	}

	zassert_equal(ant_frame_covered_len(ANT_FRAME_CFG_TRACKING,
					    ANT_FRAME_PAYLOAD_MAX + 1u),
		      ANT_FRAME_EINVAL, "an unrepresentable payload must fail");
	zassert_equal(ant_frame_addr_len((enum ant_frame_cfg)99),
		      ANT_FRAME_EINVAL, NULL);
}

/*
 * The two HAL packet formats. The interesting assertion is the last one: in
 * tracking, len_bias is exactly zero because the two header bytes ahead of the
 * payload and the two CRC bytes the length counts cancel. A backend that
 * derives body length from a field in the body therefore needs no correction,
 * and if that ever stops being true this is where it surfaces.
 */
ZTEST(ant_frame, test_pkt_formats)
{
	const struct ant_pkt_format *t = ant_frame_format(ANT_FRAME_CFG_TRACKING);
	const struct ant_pkt_format *s = ant_frame_format(ANT_FRAME_CFG_SEARCH);

	zassert_not_null(t, NULL);
	zassert_not_null(s, NULL);
	zassert_is_null(ant_frame_format((enum ant_frame_cfg)99), NULL);

	zassert_equal(t->phy, ANT_PHY_1M_GFSK,
		      "an ANT+ compatibility channel may only use the 1M PHY");
	zassert_equal(s->phy, ANT_PHY_1M_GFSK, NULL);

	zassert_equal(t->addr_len, 5, NULL);
	zassert_equal(t->len_mode, ANT_LEN_FROM_BODY, NULL);
	zassert_equal(t->len_offset, 1, "the length byte follows the trans type");
	zassert_equal(t->len_bias, 0,
		      "two header bytes against two CRC bytes must cancel");

	zassert_equal(s->addr_len, 3, NULL);
	zassert_equal(s->len_mode, ANT_LEN_FIXED,
		      "search has three bytes ahead of the length field, so no "
		      "S0|LENGTH|S1 layout can place it - it must be static");
	zassert_equal(s->body_len, 12, NULL);

	/* Both describe the same CRC, which is the other half of the coverage
	 * invariant: same polynomial, same seed, and covering the address. */
	zassert_equal(t->crc.width_bits, 16, NULL);
	zassert_equal(t->crc.poly, ANT_CRC_POLY,
		      "normal form 0x1021, not the 0x11021 one register wants");
	zassert_equal(t->crc.init, ANT_CRC_INIT, NULL);
	zassert_equal(t->crc.xor_out, 0u, NULL);
	zassert_false(t->crc.reflect_in, NULL);
	zassert_false(t->crc.reflect_out, NULL);
	zassert_true(t->crc.cover_addr,
		     "ANT's CRC covers the on-air address; a format that says "
		     "otherwise makes every backend compute the wrong 13 bytes");
	/* Field by field rather than memcmp: struct padding is unspecified, and
	 * a test that can fail on a compiler's choice of filler is worse than
	 * no test. */
	zassert_equal(s->crc.width_bits, t->crc.width_bits, NULL);
	zassert_equal(s->crc.poly, t->crc.poly, NULL);
	zassert_equal(s->crc.init, t->crc.init, NULL);
	zassert_equal(s->crc.xor_out, t->crc.xor_out, NULL);
	zassert_equal(s->crc.reflect_in, t->crc.reflect_in, NULL);
	zassert_equal(s->crc.reflect_out, t->crc.reflect_out, NULL);
	zassert_equal(s->crc.cover_addr, t->crc.cover_addr,
		      "the two configurations must agree on the CRC");
}

/* ---------------------------------------------------------------------------
 * Encode and decode
 * ---------------------------------------------------------------------------
 */

ZTEST(ant_frame, test_encode_tracking_bytes)
{
	struct ant_frame f;
	struct ant_frame_wire w;
	uint8_t flat[32];
	int n;

	zassert_equal(ant_frame_make(&f, &spike_id, spike_payload,
				     sizeof(spike_payload)), ANT_FRAME_OK, NULL);
	zassert_equal(f.len_byte, ANT_FRAME_LEN_BROADCAST,
		      "8 payload + 2 CRC is 10, not 8");

	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_OK, NULL);

	zassert_equal(w.addr_len, 5, NULL);
	zassert_equal(w.body_len, 10, NULL);
	/* Device number low byte first on air. */
	zassert_equal(w.addr[2], 0x17u, NULL);
	zassert_equal(w.addr[3], 0x3Au, NULL);
	zassert_equal(w.addr[4], SPIKE_DTYPE, NULL);
	zassert_equal(w.body[0], SPIKE_TTYPE, NULL);
	zassert_equal(w.body[1], ANT_FRAME_LEN_BROADCAST, NULL);
	zassert_equal(w.crc, SPIKE_CRC, NULL);

	n = ant_frame_to_bytes(&w, flat, sizeof(flat));
	zassert_equal(n, 17, "17 bytes follow the preamble");
	zassert_mem_equal(flat, spike15, sizeof(spike15),
			  "the encoded frame differs from the one Spike A "
			  "captured off a real power meter");
	zassert_equal(flat[15], 0x19u, "CRC goes out most significant byte first");
	zassert_equal(flat[16], 0x9Au, NULL);

	/* The length-from-body relation a backend will actually evaluate. */
	zassert_equal(w.body[ant_frame_format(ANT_FRAME_CFG_TRACKING)->len_offset] +
			      ant_frame_format(ANT_FRAME_CFG_TRACKING)->len_bias,
		      w.body_len, NULL);
}

ZTEST(ant_frame, test_encode_search_bytes)
{
	struct ant_frame f;
	struct ant_frame_wire w;
	uint8_t flat[32];

	zassert_equal(ant_frame_make(&f, &spike_id, spike_payload,
				     sizeof(spike_payload)), ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_SEARCH,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_OK, NULL);

	zassert_equal(w.addr_len, 3, NULL);
	zassert_equal(w.body_len, 12, NULL);
	/* Exactly the buffer Spike A read out of RAM:
	 * [devnum_hi][dtype][ttype][0x0A][d0..d7]. */
	zassert_equal(w.body[0], 0x3Au, NULL);
	zassert_equal(w.body[1], SPIKE_DTYPE, NULL);
	zassert_equal(w.body[2], SPIKE_TTYPE, NULL);
	zassert_equal(w.body[3], ANT_FRAME_LEN_BROADCAST, NULL);
	zassert_mem_equal(&w.body[4], spike_payload, sizeof(spike_payload), NULL);

	zassert_equal(ant_frame_to_bytes(&w, flat, sizeof(flat)), 17, NULL);
	zassert_mem_equal(flat, spike15, sizeof(spike15), NULL);
}

/*
 * The same logical frame, encoded both ways, is byte-identical on air. That is
 * the coverage invariant expressed as bytes rather than as a length, and it is
 * the reason a frame received in search validates against the same CRC as one
 * received while tracking.
 */
ZTEST(ant_frame, test_both_configs_put_the_same_bytes_on_air)
{
	struct ant_frame f;
	struct ant_frame_wire wt, ws;
	uint8_t flat_t[32], flat_s[32];
	static const uint8_t payload[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
	const struct ant_channel_id id = {
		.device_number = 0x1234u,
		.device_type = 0x78u,
		.trans_type = 0x01u,
	};

	zassert_equal(ant_frame_make(&f, &id, payload, sizeof(payload)),
		      ANT_FRAME_OK, NULL);

	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &wt),
		      ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_SEARCH,
				       ant_net_addr_ant_plus, &f, &ws),
		      ANT_FRAME_OK, NULL);

	zassert_not_equal(wt.addr_len, ws.addr_len,
			  "the two configurations must split the frame "
			  "differently or this test proves nothing");
	zassert_equal(wt.crc, ws.crc, "same 15 bytes, therefore same CRC");

	zassert_equal(ant_frame_to_bytes(&wt, flat_t, sizeof(flat_t)), 17, NULL);
	zassert_equal(ant_frame_to_bytes(&ws, flat_s, sizeof(flat_s)), 17, NULL);
	zassert_mem_equal(flat_t, flat_s, 17,
			  "tracking and search produced different on-air bytes");

	/* And this particular frame is the documented golden vector, so the
	 * CRC is pinned to a number from the document rather than to itself. */
	zassert_mem_equal(flat_t, golden15, sizeof(golden15), NULL);
	zassert_equal(wt.crc, GOLDEN_CRC, NULL);
}

static void roundtrip(enum ant_frame_cfg cfg, const struct ant_channel_id *id,
		      const uint8_t *payload, uint8_t payload_len)
{
	struct ant_frame in, out;
	struct ant_frame_wire w;

	zassert_equal(ant_frame_make(&in, id, payload, payload_len),
		      ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_encode(cfg, ant_net_addr_ant_plus, &in, &w),
		      ANT_FRAME_OK, NULL);
	zassert_true(ant_frame_crc_ok(&w), "encode produced a bad CRC");
	zassert_equal(ant_frame_decode(cfg, &w, 0, &out), ANT_FRAME_OK, NULL);

	zassert_equal(out.id.device_number, id->device_number, NULL);
	zassert_equal(out.id.device_type, id->device_type, NULL);
	zassert_equal(out.id.trans_type, id->trans_type, NULL);
	zassert_equal(out.len_byte, in.len_byte, NULL);
	zassert_equal(out.payload_len, payload_len, NULL);
	zassert_mem_equal(out.payload, payload, payload_len, NULL);
}

ZTEST(ant_frame, test_roundtrip_both_configs)
{
	/* Including the pairing bit set, a 0xFF transmission type, and device
	 * numbers whose two bytes differ - the case a byte-order slip in
	 * either direction would survive if they were equal. */
	static const struct ant_channel_id ids[] = {
		{ .device_number = SPIKE_DEVNUM, .device_type = SPIKE_DTYPE,
		  .trans_type = SPIKE_TTYPE },
		{ .device_number = 0x0001u, .device_type = 0x01u,
		  .trans_type = 0x00u },
		{ .device_number = 0xFFFFu, .device_type = 0xFFu,
		  .trans_type = 0xFFu },
		{ .device_number = 0xFF00u, .device_type = 0x80u | 0x0Bu,
		  .trans_type = 0x05u },
		{ .device_number = 0x00FFu, .device_type = 0x78u,
		  .trans_type = 0x01u },
	};
	static const uint8_t payload[8] = {
		0xDE, 0xAD, 0xBE, 0xEF, 0x00, 0xFF, 0x55, 0xAA,
	};

	for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
		roundtrip(ANT_FRAME_CFG_TRACKING, &ids[i], payload,
			  sizeof(payload));
		roundtrip(ANT_FRAME_CFG_SEARCH, &ids[i], payload,
			  sizeof(payload));
	}
}

ZTEST(ant_frame, test_pairing_bit_is_carried_not_interpreted)
{
	struct ant_frame f, out;
	struct ant_frame_wire w;
	const struct ant_channel_id id = {
		.device_number = SPIKE_DEVNUM,
		.device_type = (uint8_t)(ANT_DEVICE_TYPE_PAIRING_BIT | SPIKE_DTYPE),
		.trans_type = SPIKE_TTYPE,
	};

	zassert_equal(ant_frame_make(&f, &id, spike_payload,
				     sizeof(spike_payload)), ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_OK, NULL);

	/* The pairing bit is part of the on-air address byte, so it changes
	 * which frames the hardware matches. The frame layer must not mask it
	 * away, and must not act on it either. */
	zassert_equal(w.addr[4], id.device_type, NULL);
	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w, 0, &out),
		      ANT_FRAME_OK, NULL);
	zassert_equal(out.id.device_type, id.device_type, NULL);
	zassert_equal(out.id.device_type & ANT_DEVICE_TYPE_MASK, SPIKE_DTYPE, NULL);
}

/*
 * Parse the real captured frame from its flat bytes, in both configurations,
 * and check the decoded channel ID against the ground truth tools/ant_scan.py
 * established independently before Spike A ran.
 */
ZTEST(ant_frame, test_decode_captured_frame)
{
	uint8_t buf[sizeof(spike15) + 2];
	enum ant_frame_cfg cfgs[] = { ANT_FRAME_CFG_TRACKING, ANT_FRAME_CFG_SEARCH };

	memcpy(buf, spike15, sizeof(spike15));
	buf[15] = (uint8_t)(SPIKE_CRC >> 8);
	buf[16] = (uint8_t)(SPIKE_CRC & 0xFFu);

	for (size_t i = 0; i < ARRAY_SIZE(cfgs); i++) {
		struct ant_frame_wire w;
		struct ant_frame f;

		zassert_equal(ant_frame_from_bytes(cfgs[i], buf, sizeof(buf), &w),
			      17, "the whole frame should have been consumed");
		zassert_equal(w.crc, SPIKE_CRC, NULL);
		zassert_true(ant_frame_crc_ok(&w), NULL);

		zassert_equal(ant_frame_decode(cfgs[i], &w, 0, &f),
			      ANT_FRAME_OK, NULL);
		zassert_equal(f.id.device_number, SPIKE_DEVNUM,
			      "decoded device number does not match the ground "
			      "truth tools/ant_scan.py reported");
		zassert_equal(f.id.device_type, SPIKE_DTYPE, NULL);
		zassert_equal(f.id.trans_type, SPIKE_TTYPE, NULL);
		zassert_equal(f.len_byte, ANT_FRAME_LEN_BROADCAST, NULL);
		zassert_equal(f.payload_len, 8, NULL);
		zassert_mem_equal(f.payload, spike_payload, sizeof(spike_payload),
				  NULL);
	}
}

/* ---------------------------------------------------------------------------
 * Malformed input
 * ---------------------------------------------------------------------------
 */

/*
 * The length byte is stated separately from the payload length and
 * cross-checked, rather than derived, because Spike B may yet show it is a
 * control byte. Until then the two must agree, in both directions.
 */
ZTEST(ant_frame, test_wrong_length_byte_is_rejected)
{
	struct ant_frame f, out;
	struct ant_frame_wire w;

	zassert_equal(ant_frame_make(&f, &spike_id, spike_payload,
				     sizeof(spike_payload)), ANT_FRAME_OK, NULL);

	/* 8 would be the natural mistake: a payload length rather than a
	 * ShockBurst length that counts the CRC bytes. */
	f.len_byte = 8u;
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_ELEN,
		      "encode accepted a length byte of 8 for an 8-byte payload");

	f.len_byte = ANT_FRAME_LEN_BROADCAST;
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_OK, NULL);

	/* On the receive side the length byte comes off the air, so a frame
	 * whose body does not match it is malformed rather than a caller bug.
	 * The CRC is repaired first, so that this test fails on the length
	 * check and not incidentally on the CRC. */
	w.body[1] = 0x0Cu;
	w.crc = ant_frame_crc(&w);
	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w, 0, &out),
		      ANT_FRAME_ELEN,
		      "decode accepted a length byte disagreeing with the body");
}

ZTEST(ant_frame, test_truncated_input_is_rejected)
{
	uint8_t buf[sizeof(spike15) + 2];
	struct ant_frame_wire w;
	struct ant_frame f;
	uint8_t small[8];

	memcpy(buf, spike15, sizeof(spike15));
	buf[15] = (uint8_t)(SPIKE_CRC >> 8);
	buf[16] = (uint8_t)(SPIKE_CRC & 0xFFu);

	/* Every length short of the full frame, including the ones that stop
	 * before the length byte itself. */
	for (size_t n = 0; n < 17u; n++) {
		zassert_equal(ant_frame_from_bytes(ANT_FRAME_CFG_TRACKING, buf,
						   n, &w),
			      ANT_FRAME_ETRUNC,
			      "a %zu-byte buffer was accepted as a 17-byte frame",
			      n);
	}
	zassert_equal(ant_frame_from_bytes(ANT_FRAME_CFG_TRACKING, buf, 17u, &w),
		      17, NULL);

	/* Trailing bytes are ignored and the consumed count says so. */
	zassert_equal(ant_frame_from_bytes(ANT_FRAME_CFG_TRACKING, buf,
					   sizeof(buf), &w),
		      17, NULL);

	/* A body with no room for its own header. */
	memset(&w, 0, sizeof(w));
	w.addr_len = 5;
	w.body_len = 1;
	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w, 0, &f),
		      ANT_FRAME_ETRUNC, NULL);

	/* And the output side: a buffer too small must fail rather than write
	 * as much as fits. */
	zassert_equal(ant_frame_from_bytes(ANT_FRAME_CFG_TRACKING, buf, 17u, &w),
		      17, NULL);
	zassert_equal(ant_frame_to_bytes(&w, small, sizeof(small)),
		      ANT_FRAME_ETRUNC, NULL);
}

/*
 * The one that matters most. A CRC failure is reported, never repaired, never
 * downgraded to a flag on an otherwise-populated result - so the output must
 * be untouched, because a caller that ignores a return code is the reason the
 * distinction exists at all.
 */
ZTEST(ant_frame, test_crc_failure_is_reported_and_never_repaired)
{
	struct ant_frame f, out, sentinel;
	struct ant_frame_wire w;

	zassert_equal(ant_frame_make(&f, &spike_id, spike_payload,
				     sizeof(spike_payload)), ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_OK, NULL);

	/* One flipped payload bit, everything else intact. */
	w.body[4] ^= 0x01u;
	zassert_false(ant_frame_crc_ok(&w), NULL);

	/* memcpy rather than a struct assignment, so that every byte including
	 * any padding is known before the call and the comparison afterwards
	 * cannot fail on filler the compiler was free not to copy. */
	memset(&sentinel, 0x5A, sizeof(sentinel));
	memcpy(&out, &sentinel, sizeof(out));
	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w, 0, &out),
		      ANT_FRAME_ECRC,
		      "a corrupted frame decoded successfully");
	zassert_mem_equal(&out, &sentinel, sizeof(out),
			  "decode wrote a partial result for a frame it "
			  "rejected; a caller ignoring the return code would "
			  "act on corrupt data");

	/* A wrong CRC value with an intact body is the same failure. */
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_OK, NULL);
	w.crc = (uint16_t)(w.crc ^ 0x0001u);
	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w, 0, &out),
		      ANT_FRAME_ECRC, NULL);
}

/*
 * ANT_FRAME_TRUSTED_CRC is opt-out rather than opt-in, so that forgetting it
 * produces a loud rejection of a good frame rather than silent acceptance of a
 * bad one. Both halves are asserted, because the value of the choice is
 * entirely in which mistake it makes.
 */
ZTEST(ant_frame, test_trusted_crc_is_opt_out)
{
	struct ant_frame f, out;
	struct ant_frame_wire w;

	zassert_equal(ant_frame_make(&f, &spike_id, spike_payload,
				     sizeof(spike_payload)), ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_SEARCH,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_OK, NULL);

	/* What a hardware CRC engine leaves behind: a verified frame whose CRC
	 * bytes never reached the core, so w.crc holds nothing. */
	w.crc = 0u;
	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_SEARCH, &w, 0, &out),
		      ANT_FRAME_ECRC,
		      "the default must verify, so a caller who forgets the "
		      "flag fails safely");
	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_SEARCH, &w,
				       ANT_FRAME_TRUSTED_CRC, &out),
		      ANT_FRAME_OK, NULL);
	zassert_equal(out.id.device_number, SPIKE_DEVNUM, NULL);
}

ZTEST(ant_frame, test_null_and_unknown_configuration_are_rejected)
{
	struct ant_frame f, out;
	struct ant_frame_wire w;
	uint8_t buf[32];

	zassert_equal(ant_frame_make(NULL, &spike_id, spike_payload, 8),
		      ANT_FRAME_EINVAL, NULL);
	zassert_equal(ant_frame_make(&f, NULL, spike_payload, 8),
		      ANT_FRAME_EINVAL, NULL);
	zassert_equal(ant_frame_make(&f, &spike_id, NULL, 8),
		      ANT_FRAME_EINVAL, NULL);
	zassert_equal(ant_frame_make(&f, &spike_id, spike_payload,
				     (uint8_t)(ANT_FRAME_PAYLOAD_MAX + 1u)),
		      ANT_FRAME_EINVAL, "an over-long payload must be refused");

	zassert_equal(ant_frame_make(&f, &spike_id, spike_payload, 8),
		      ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_encode((enum ant_frame_cfg)99,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_EINVAL, NULL);
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING, NULL, &f, &w),
		      ANT_FRAME_EINVAL, NULL);
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, NULL, &w),
		      ANT_FRAME_EINVAL, NULL);
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, NULL),
		      ANT_FRAME_EINVAL, NULL);

	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_OK, NULL);

	/* An address length the configuration does not use is not a truncation
	 * or a CRC problem - it means the caller mixed the two up, which is
	 * the mistake most likely to look like it worked. */
	w.addr_len = 3;
	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w, 0, &out),
		      ANT_FRAME_EINVAL, NULL);

	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, NULL, 0, &out),
		      ANT_FRAME_EINVAL, NULL);
	zassert_equal(ant_frame_from_bytes(ANT_FRAME_CFG_TRACKING, NULL,
					   sizeof(buf), &w),
		      ANT_FRAME_EINVAL, NULL);
	zassert_equal(ant_frame_to_bytes(NULL, buf, sizeof(buf)),
		      ANT_FRAME_EINVAL, NULL);
}

/*
 * A length byte a frame cannot possibly have. Below 2 it does not even cover
 * its own CRC bytes; above 26 it exceeds anything this module can hold. Both
 * must be an explicit rejection rather than an underflowed payload length -
 * the second is how a parser turns a corrupt byte into a buffer overrun.
 */
ZTEST(ant_frame, test_impossible_length_byte_is_rejected)
{
	uint8_t buf[64];
	struct ant_frame_wire w;

	memcpy(buf, spike15, sizeof(spike15));
	memset(&buf[15], 0, sizeof(buf) - 15u);

	for (unsigned int lenb = 0; lenb < 2u; lenb++) {
		buf[6] = (uint8_t)lenb;
		zassert_equal(ant_frame_from_bytes(ANT_FRAME_CFG_TRACKING, buf,
						   sizeof(buf), &w),
			      ANT_FRAME_ELEN,
			      "length byte %u does not even cover the CRC", lenb);
	}

	buf[6] = (uint8_t)(ANT_FRAME_PAYLOAD_MAX + ANT_FRAME_CRC_BYTES + 1u);
	zassert_equal(ant_frame_from_bytes(ANT_FRAME_CFG_TRACKING, buf,
					   sizeof(buf), &w),
		      ANT_FRAME_ELEN, "an over-long length byte was accepted");
}

/* ---------------------------------------------------------------------------
 * Boundary: the advanced-burst prediction
 * ---------------------------------------------------------------------------
 */

/*
 * UNTESTED ON AIR. docs/ant-radio-link.md predicts that an advanced-burst
 * frame carrying 24 payload bytes shows 0x1A in the length byte, on the
 * reading that the byte counts payload plus the two CRC bytes. No such frame
 * has ever been captured - Spike A heard only sensor broadcasts, where the
 * byte is constant at 0x0A, and Spike B has not run. If Spike B shows
 * something else, that byte is ANT's control byte rather than a length, and
 * this test is the one that should change: it is written so that the
 * prediction is falsifiable in code rather than only in prose.
 *
 * What it does test today, and this part is not speculative: the module's
 * arithmetic holds at the top of its range, tracking's zero length-bias
 * survives a payload three times the standard size, and the coverage
 * invariant still binds the two configurations together at 31 bytes.
 */
ZTEST(ant_frame, test_advanced_burst_length_prediction)
{
	struct ant_frame f;
	struct ant_frame_wire wt, ws;
	uint8_t payload[ANT_FRAME_PAYLOAD_MAX];
	uint8_t flat_t[64], flat_s[64];

	for (uint8_t i = 0; i < sizeof(payload); i++) {
		payload[i] = (uint8_t)(0x20u + i);
	}

	zassert_equal(ant_frame_make(&f, &spike_id, payload, sizeof(payload)),
		      ANT_FRAME_OK, NULL);
	zassert_equal(f.len_byte, ANT_FRAME_LEN_ADV_BURST,
		      "24 payload + 2 CRC should read 0x1A on air");

	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &wt),
		      ANT_FRAME_OK, NULL);
	zassert_equal(wt.body_len, 26, NULL);
	zassert_equal(wt.body[1], ANT_FRAME_LEN_ADV_BURST, NULL);
	/* The bias is still zero: body_len equals the length byte. */
	zassert_equal(wt.body[1], wt.body_len,
		      "tracking's length-from-body bias only cancels by "
		      "accident if it stops holding at 24 payload bytes");
	zassert_true(wt.body_len <=
			     ant_frame_format(ANT_FRAME_CFG_TRACKING)->max_body_len,
		     "the tracking format cannot express the frame it predicts");

	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_SEARCH,
				       ant_net_addr_ant_plus, &f, &ws),
		      ANT_FRAME_OK, NULL);
	zassert_equal(ws.body_len, 28, NULL);
	zassert_equal(ant_frame_covered_len(ANT_FRAME_CFG_TRACKING, 24), 31, NULL);
	zassert_equal(ant_frame_covered_len(ANT_FRAME_CFG_SEARCH, 24), 31, NULL);
	zassert_equal(wt.crc, ws.crc, NULL);

	zassert_equal(ant_frame_to_bytes(&wt, flat_t, sizeof(flat_t)), 33, NULL);
	zassert_equal(ant_frame_to_bytes(&ws, flat_s, sizeof(flat_s)), 33, NULL);
	zassert_mem_equal(flat_t, flat_s, 33, NULL);

	/* Round-trips in the configuration that can actually carry it. Search
	 * is fixed at 12 body bytes in hardware, so a long frame is encodable
	 * for a software consumer but is not receivable in a search window -
	 * which is what the search format's ANT_LEN_FIXED says. */
	roundtrip(ANT_FRAME_CFG_TRACKING, &spike_id, payload, sizeof(payload));
	zassert_equal(ant_frame_format(ANT_FRAME_CFG_SEARCH)->max_body_len, 12,
		      "search cannot receive a longer-than-standard frame, and "
		      "the format must keep saying so");
}
