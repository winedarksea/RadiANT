/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original work. Every expected value in this file comes from
 * docs/ant-radio-link.md, from docs/spike-a-results.md, from
 * docs/spike-b-results.md or from docs/spike-b-part2-results.md, and from the
 * capture logs those cite
 * (archive/captures/radio/2026-08-09-nrf54l15-run*.log,
 * archive/captures/radio/2026-08-09-spike-b-*.log and
 * archive/captures/radio/2026-08-09-spike-b2-*.log) - not from running
 * ant_frame.c and writing down what it did. Nothing here derives from
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
 *     silently repaired CRC, a control byte whose bits 2:0 are not 010, a
 *     cross-check on bits 4:0 that rejects every valid slave frame, a
 *     length-from-body format that would drop every acknowledged frame -
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

/*
 * The pair of frames from docs/spike-b-results.md that refuted the length
 * reading, transcribed from the capture rather than reconstructed:
 *
 *   B3=AA pl=A0005AA500112233   acknowledged
 *   B3=0A pl=A0005AA500112233   the same eight bytes, one slot later
 *
 * An ANT master leaves its last payload in the broadcast buffer, so the two
 * frames differ in exactly byte 3 and in nothing else. A length field cannot
 * do that. That pair recurs 22 times across the six runs.
 */
static const uint8_t spike_b_payload[8] = {
	0xA0, 0x00, 0x5A, 0xA5, 0x00, 0x11, 0x22, 0x33,
};

/* ---------------------------------------------------------------------------
 * The eleven control bytes, from docs/spike-b-part2-results.md's table and
 * from the analyser's own decode of
 * archive/captures/radio/2026-08-09-spike-b2-run{0,A,B,C}.log.
 *
 * Written out as fields AND as the hex byte the capture shows, deliberately.
 * The two columns were derived independently - the fields from the part 2 bit
 * table, the hex from the logs' B3= column - so the table asserts that the
 * six-field model reproduces the air rather than that the encoder is
 * self-consistent.
 * ---------------------------------------------------------------------------
 */
struct ctrl_case {
	uint8_t                on_air;
	struct ant_ctrl_fields f;
	enum ant_msg_type      type;
	const char            *what;
};

static const struct ctrl_case ctrl_cases[] = {
	/* Slot openers - part 1's three values, and run C's control. */
	{ 0x0Au, { false, false, false, false, true },
	  ANT_MSG_BROADCAST,    "broadcast" },
	{ 0x8Au, { true, false, false, false, true },
	  ANT_MSG_BURST_DATA,   "slot-opening burst packet, seq 0" },
	{ 0xAAu, { true, false, true, false, true },
	  ANT_MSG_BURST_LAST,   "slot-opening acknowledged data == one-packet burst" },
	/* In-slot data packets. */
	{ 0x82u, { true, false, false, false, false },
	  ANT_MSG_BURST_DATA,   "burst packet, seq 0" },
	{ 0x92u, { true, false, false, true, false },
	  ANT_MSG_BURST_DATA,   "burst packet, seq 1" },
	{ 0xA2u, { true, false, true, false, false },
	  ANT_MSG_BURST_LAST,   "burst last packet, seq 0" },
	{ 0xB2u, { true, false, true, true, false },
	  ANT_MSG_BURST_LAST,   "burst last packet, seq 1" },
	/* In-slot acknowledgements. */
	{ 0xC2u, { true, true, false, false, false },
	  ANT_MSG_BURST_ACK,    "ack of a non-final packet, next seq 0" },
	{ 0xD2u, { true, true, false, true, false },
	  ANT_MSG_BURST_ACK,    "ack of a non-final packet, next seq 1" },
	{ 0xE2u, { true, true, true, false, false },
	  ANT_MSG_TRANSFER_ACK, "ack of the final packet, seq 0" },
	{ 0xF2u, { true, true, true, true, false },
	  ANT_MSG_TRANSFER_ACK, "ack of the final packet, seq 1" },
};

/* The field sets the rest of this file builds frames with. A master's three. */
static const struct ant_ctrl_fields ctrl_broadcast = {
	.slot_opener = true,
};
static const struct ant_ctrl_fields ctrl_ack_data_open = {
	.exchange = true, .last = true, .slot_opener = true,
};
static const struct ant_ctrl_fields ctrl_burst_open_seq0 = {
	.exchange = true, .slot_opener = true,
};
/* And a slave's, so the tests exercise a frame with bit 3 clear - the exact
 * case the bits-4:0 cross-check used to reject. */
static const struct ant_ctrl_fields ctrl_burst_last_seq0 = {
	.exchange = true, .last = true,
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
 * The two HAL packet formats. The interesting assertion is now the first pair:
 * BOTH are ANT_LEN_FIXED, and tracking is fixed because of Spike B rather than
 * because of the hardware.
 *
 * ANT_LEN_FROM_BODY is what a backend maps onto nRF PCNF0.LFLEN=8, and that
 * register reads byte 3 as a length. Byte 3 is a control byte: an acknowledged
 * frame carries 0xAA there, which parses as LENGTH=170, overruns MAXLEN and is
 * discarded as a CRC error. Such a receiver hears every broadcast perfectly
 * and drops every acknowledged and burst frame in silence - which is "ERG mode
 * does not work", found months later. This assertion is the regression test
 * for that, and it is worth more than anything else in this file.
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
	zassert_equal(t->len_mode, ANT_LEN_FIXED,
		      "tracking must be static-length: a length-from-body "
		      "format becomes PCNF0.LFLEN=8, which reads an "
		      "acknowledged frame's 0xAA as a 170-byte length and "
		      "silently drops every frame above broadcast");
	zassert_equal(t->body_len, 10, "trans type, control byte, 8 payload");
	zassert_equal(t->max_body_len, 10,
		      "a longer frame is not receivable and must not be "
		      "advertised as if it were");

	zassert_equal(s->addr_len, 3, NULL);
	zassert_equal(s->len_mode, ANT_LEN_FIXED,
		      "search has three bytes ahead of the control byte, so no "
		      "S0|LENGTH|S1 layout can place a length field there "
		      "either - it must be static");
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

	zassert_equal(ant_frame_make(&f, &spike_id, &ctrl_broadcast,
				     spike_payload, sizeof(spike_payload)),
		      ANT_FRAME_OK, NULL);
	zassert_equal(f.ctrl_byte, ANT_CTRL_BROADCAST,
		      "a broadcast of 8 payload bytes is 0x0A on air");

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
	zassert_equal(w.body[1], ANT_CTRL_BROADCAST, NULL);
	zassert_equal(w.crc, SPIKE_CRC, NULL);

	n = ant_frame_to_bytes(&w, flat, sizeof(flat));
	zassert_equal(n, 17, "17 bytes follow the preamble");
	zassert_mem_equal(flat, spike15, sizeof(spike15),
			  "the encoded frame differs from the one Spike A "
			  "captured off a real power meter");
	zassert_equal(flat[15], 0x19u, "CRC goes out most significant byte first");
	zassert_equal(flat[16], 0x9Au, NULL);

	/* The fixed-length relation a backend will actually program. */
	zassert_equal(ant_frame_format(ANT_FRAME_CFG_TRACKING)->body_len,
		      w.body_len,
		      "a STATLEN-programmed receiver must expect exactly the "
		      "body this encoder produces");
}

ZTEST(ant_frame, test_encode_search_bytes)
{
	struct ant_frame f;
	struct ant_frame_wire w;
	uint8_t flat[32];

	zassert_equal(ant_frame_make(&f, &spike_id, &ctrl_broadcast,
				     spike_payload, sizeof(spike_payload)),
		      ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_SEARCH,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_OK, NULL);

	zassert_equal(w.addr_len, 3, NULL);
	zassert_equal(w.body_len, 12, NULL);
	/* Exactly the buffer both spikes read out of RAM, and the banner
	 * Spike B printed at boot:
	 * [devnum_hi][dtype][ttype][control byte][d0..d7]. */
	zassert_equal(w.body[0], 0x3Au, NULL);
	zassert_equal(w.body[1], SPIKE_DTYPE, NULL);
	zassert_equal(w.body[2], SPIKE_TTYPE, NULL);
	zassert_equal(w.body[3], ANT_CTRL_BROADCAST, NULL);
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

	zassert_equal(ant_frame_make(&f, &id, &ctrl_broadcast, payload,
				     sizeof(payload)),
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
		      const struct ant_ctrl_fields *ctrl, const uint8_t *payload,
		      uint8_t payload_len)
{
	struct ant_frame in, out;
	struct ant_frame_wire w;

	zassert_equal(ant_frame_make(&in, id, ctrl, payload, payload_len),
		      ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_encode(cfg, ant_net_addr_ant_plus, &in, &w),
		      ANT_FRAME_OK, NULL);
	zassert_true(ant_frame_crc_ok(&w), "encode produced a bad CRC");
	zassert_equal(ant_frame_decode(cfg, &w, 0, &out), ANT_FRAME_OK, NULL);

	zassert_equal(out.id.device_number, id->device_number, NULL);
	zassert_equal(out.id.device_type, id->device_type, NULL);
	zassert_equal(out.id.trans_type, id->trans_type, NULL);
	zassert_equal(out.ctrl_byte, in.ctrl_byte, NULL);
	/* Every field must survive the round trip, or the control byte is being
	 * carried without being understood. */
	zassert_equal(ant_ctrl_is_exchange(out.ctrl_byte), ctrl->exchange, NULL);
	zassert_equal(ant_ctrl_is_ack(out.ctrl_byte), ctrl->ack, NULL);
	zassert_equal(ant_ctrl_is_last(out.ctrl_byte), ctrl->last, NULL);
	zassert_equal(ant_ctrl_seq(out.ctrl_byte) != 0u, ctrl->seq, NULL);
	zassert_equal(ant_ctrl_is_slot_opener(out.ctrl_byte), ctrl->slot_opener,
		      NULL);
	zassert_true(ant_ctrl_low_ok(out.ctrl_byte), NULL);
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

	/* ALL ELEVEN measured control bytes, through both configurations. Part 1
	 * could reach three of these; the eight with bit 3 clear are the ones
	 * the bits-4:0 cross-check used to reject outright. */
	for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
		for (size_t c = 0; c < ARRAY_SIZE(ctrl_cases); c++) {
			roundtrip(ANT_FRAME_CFG_TRACKING, &ids[i],
				  &ctrl_cases[c].f, payload, sizeof(payload));
			roundtrip(ANT_FRAME_CFG_SEARCH, &ids[i],
				  &ctrl_cases[c].f, payload, sizeof(payload));
		}
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

	zassert_equal(ant_frame_make(&f, &id, &ctrl_broadcast, spike_payload,
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
		zassert_equal(f.ctrl_byte, ANT_CTRL_BROADCAST, NULL);
		zassert_equal(ant_frame_msg_type(f.ctrl_byte), ANT_MSG_BROADCAST,
			      NULL);
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
 * BITS 2:0 == 010 is the whole invariant, on encode and on decode. Nothing else
 * about the byte is ever an error.
 *
 * The wrong answer this test exists to exclude is named, because it shipped:
 * the version of ant_frame.c written against Spike B part 1 cross-checked bits
 * 4:0 against payload_len + 2, which rejects EVERY valid in-slot frame. 0xA2 is
 * an eight-byte slave packet whose low five bits read 2, and part 1's check
 * wanted 10. That is the PCNF0.LFLEN=8 defect reintroduced in software.
 */
ZTEST(ant_frame, test_control_byte_low_bits_are_the_only_invariant)
{
	struct ant_frame f, out;
	struct ant_frame_wire w;

	zassert_equal(ant_frame_make(&f, &spike_id, &ctrl_broadcast,
				     spike_payload, sizeof(spike_payload)),
		      ANT_FRAME_OK, NULL);

	/* Every measured control byte must ENCODE against an eight-byte
	 * payload, including all eight whose low five bits are not 10. */
	for (size_t c = 0; c < ARRAY_SIZE(ctrl_cases); c++) {
		f.ctrl_byte = ctrl_cases[c].on_air;
		zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
					       ant_net_addr_ant_plus, &f, &w),
			      ANT_FRAME_OK,
			      "encode rejected the measured control byte 0x%02X "
			      "(%s); a check on bits 4:0 rejects eight of the "
			      "eleven",
			      ctrl_cases[c].on_air, ctrl_cases[c].what);
		zassert_equal(w.body[1], ctrl_cases[c].on_air, NULL);

		/* And DECODE, off the air, unchanged. */
		zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w, 0,
					       &out),
			      ANT_FRAME_OK,
			      "decode rejected the measured control byte 0x%02X",
			      ctrl_cases[c].on_air);
		zassert_equal(out.ctrl_byte, ctrl_cases[c].on_air, NULL);
		zassert_equal(out.payload_len, 8u, NULL);
	}

	/* Now the rejections. Bits 2:0 other than 010, under several different
	 * flag combinations, because a parser that masked the wrong way round
	 * would pass one and fail another. */
	static const uint8_t bad_low[] = {
		0x00u, 0x01u, 0x03u, 0x04u, 0x05u, 0x06u, 0x07u,
		0x08u, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu,
		0xA8u, 0xAFu, 0xF0u, 0x81u,
	};

	for (size_t i = 0; i < ARRAY_SIZE(bad_low); i++) {
		f.ctrl_byte = bad_low[i];
		zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
					       ant_net_addr_ant_plus, &f, &w),
			      ANT_FRAME_ECTRL,
			      "encode accepted 0x%02X, whose bits 2:0 are not "
			      "010", bad_low[i]);
	}

	/* On the receive side the byte comes off the air, so this is a
	 * malformed frame rather than a caller bug. The CRC is recomputed first
	 * so the test fails on the control check and not incidentally on the
	 * CRC. */
	f.ctrl_byte = ANT_CTRL_BROADCAST;
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_OK, NULL);
	w.body[1] = 0x0Cu;
	w.crc = ant_frame_crc(&w);
	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w, 0, &out),
		      ANT_FRAME_ECTRL,
		      "decode accepted a control byte whose bits 2:0 are 100");
	zassert_equal(ant_frame_msg_type(0x0Cu), ANT_MSG_UNKNOWN,
		      "bits 2:0 != 010 cannot be named");
}

/*
 * The other direction, and the one both parts of Spike B bought: a control byte
 * whose FLAGS this module has never measured must still ENCODE and DECODE,
 * because refusing it is how the next unmeasured thing becomes unmeasurable.
 * The eleven measured values are a table, not a whitelist; only
 * ant_frame_make() refuses to invent one.
 */
ZTEST(ant_frame, test_unmeasured_flags_are_carried_not_rejected)
{
	struct ant_frame f, out;
	struct ant_frame_wire w;
	struct ant_ctrl_fields bad = { .ack = true, .last = true };

	zassert_equal(ant_frame_make(&f, &spike_id, &ctrl_broadcast,
				     spike_payload, sizeof(spike_payload)),
		      ANT_FRAME_OK, NULL);

	/* 0b011 in bits 7:5 - b7 clear with b6 and b5 set. Bits 7:5 never took
	 * 001, 010 or 011 in either part of Spike B, so this is a combination
	 * nothing has ever transmitted, with legal bits 2:0. */
	f.ctrl_byte = (uint8_t)(0x60u | ANT_CTRL_SLOT | ANT_CTRL_LOW_VALUE);
	zassert_equal(f.ctrl_byte, 0x6Au, NULL);
	zassert_false(ant_ctrl_observed(f.ctrl_byte),
		      "0x6A has never been on the air and the table must say so");

	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_OK,
		      "a bench experiment must be able to put a candidate "
		      "encoding on the air without editing ant_frame.c");
	zassert_equal(w.body[1], 0x6Au, "the flags must reach the air exactly "
					"as the caller set them");

	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w, 0, &out),
		      ANT_FRAME_OK,
		      "decode dispatches on the control byte; it must not "
		      "validate it against the eleven known values");
	zassert_equal(out.ctrl_byte, 0x6Au, NULL);
	zassert_equal(ant_frame_msg_type(out.ctrl_byte), ANT_MSG_UNKNOWN,
		      "an unmeasured encoding must be named unknown, not "
		      "guessed at");
	zassert_equal(out.payload_len, 8, NULL);
	zassert_mem_equal(out.payload, spike_payload, sizeof(spike_payload),
			  NULL);

	/* But ant_frame_make() must refuse to originate it. That is the line
	 * between "the model can express it" and "the air has carried it". */
	zassert_equal(ant_frame_make(&f, &spike_id, &bad, spike_payload,
				     sizeof(spike_payload)),
		      ANT_FRAME_EINVAL,
		      "ant_frame_make invented a control byte nothing has "
		      "transmitted");
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
	 * before the control byte itself. */
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

	zassert_equal(ant_frame_make(&f, &spike_id, &ctrl_broadcast,
				     spike_payload, sizeof(spike_payload)),
		      ANT_FRAME_OK, NULL);
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

	zassert_equal(ant_frame_make(&f, &spike_id, &ctrl_broadcast,
				     spike_payload, sizeof(spike_payload)),
		      ANT_FRAME_OK, NULL);
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

	zassert_equal(ant_frame_make(NULL, &spike_id, &ctrl_broadcast,
				     spike_payload, 8),
		      ANT_FRAME_EINVAL, NULL);
	zassert_equal(ant_frame_make(&f, NULL, &ctrl_broadcast,
				     spike_payload, 8),
		      ANT_FRAME_EINVAL, NULL);
	zassert_equal(ant_frame_make(&f, &spike_id, &ctrl_broadcast, NULL, 8),
		      ANT_FRAME_EINVAL, NULL);
	zassert_equal(ant_frame_make(&f, &spike_id, &ctrl_broadcast,
				     spike_payload,
				     (uint8_t)(ANT_FRAME_PAYLOAD_MAX + 1u)),
		      ANT_FRAME_EINVAL, "an over-long payload must be refused");

	/* No field set, and no safe default to fall back on: a caller who meant
	 * acknowledged data and silently got a broadcast would set no trainer
	 * resistance and report no error. */
	zassert_equal(ant_frame_make(&f, &spike_id, NULL, spike_payload, 8),
		      ANT_FRAME_EINVAL,
		      "ant_frame_make must refuse to invent a control byte");

	zassert_equal(ant_frame_make(&f, &spike_id, &ctrl_broadcast,
				     spike_payload, 8),
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
 * The flat parser takes the frame geometry from the configuration and the
 * control byte's bits 2:0 from the air, and nothing else. Every combination of
 * the five flags above legal bits 2:0 must parse as a 17-byte frame; every
 * value with bits 2:0 != 010 must be ANT_FRAME_ECTRL.
 *
 * The wrong answer this excludes by name: taking the length from bits 4:0. It
 * gives the right answer for 0x0A - 8 + 2 = 10 - and the wrong one for all
 * eight in-slot values, which is why part 1's reading survived 750 frames.
 */
ZTEST(ant_frame, test_flat_parse_ignores_everything_but_bits_2_0)
{
	uint8_t buf[64];
	struct ant_frame_wire w;
	struct ant_frame f;

	/* All 32 flag combinations over legal low bits. */
	for (unsigned int flags = 0; flags < 32u; flags++) {
		uint16_t crc;

		memcpy(buf, spike15, sizeof(spike15));
		memset(&buf[15], 0, sizeof(buf) - 15u);
		buf[6] = (uint8_t)((flags << 3) | ANT_CTRL_LOW_VALUE);
		crc = ant_crc16(ANT_CRC_INIT, buf, 15u);
		buf[15] = (uint8_t)(crc >> 8);
		buf[16] = (uint8_t)(crc & 0xFFu);

		zassert_equal(ant_frame_from_bytes(ANT_FRAME_CFG_TRACKING, buf,
						   sizeof(buf), &w),
			      17,
			      "control byte 0x%02X was not parsed as a 17-byte "
			      "frame; reading bits 4:0 as a length gets this "
			      "right only for 0x0A", buf[6]);
		zassert_equal(w.body_len, 10u, NULL);
		zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w, 0, &f),
			      ANT_FRAME_OK, NULL);
		zassert_equal(f.payload_len, 8u, NULL);
		zassert_equal(f.ctrl_byte, buf[6], NULL);
		zassert_mem_equal(f.payload, spike_payload, sizeof(spike_payload),
				  NULL);
	}

	/* And the seven illegal low-bit values, under three different flag
	 * sets, because a parser that masked the wrong way round would pass one
	 * and fail another. */
	static const uint8_t flag_sets[] = { 0x00u, 0xA0u, 0x80u };

	for (size_t t = 0; t < ARRAY_SIZE(flag_sets); t++) {
		for (unsigned int low = 0; low < 8u; low++) {
			if (low == ANT_CTRL_LOW_VALUE) {
				continue;
			}
			memcpy(buf, spike15, sizeof(spike15));
			memset(&buf[15], 0, sizeof(buf) - 15u);
			buf[6] = (uint8_t)(flag_sets[t] | low);
			zassert_equal(ant_frame_from_bytes(ANT_FRAME_CFG_TRACKING,
							   buf, sizeof(buf), &w),
				      ANT_FRAME_ECTRL,
				      "control byte 0x%02X has bits 2:0 != 010 "
				      "and was accepted", buf[6]);
		}
	}
}

/* ---------------------------------------------------------------------------
 * The control byte - Spike B parts 1 and 2
 * ---------------------------------------------------------------------------
 */

/*
 * THE table test. All eleven values that have been on the air, against the
 * six-field decode.
 *
 * The two columns of ctrl_cases[] were derived independently - the hex from the
 * logs' B3= column, the fields from part 2's bit table - so this asserts that
 * the field model reproduces the air, not that the encoder agrees with itself.
 */
ZTEST(ant_frame, test_eleven_measured_control_bytes)
{
	zassert_equal(ARRAY_SIZE(ctrl_cases), 11u,
		      "eleven values were measured; a twelfth needs a capture "
		      "behind it");

	for (size_t i = 0; i < ARRAY_SIZE(ctrl_cases); i++) {
		const struct ctrl_case *c = &ctrl_cases[i];
		struct ant_ctrl_fields got;

		/* Fields -> byte. */
		zassert_equal(ant_ctrl_encode(&c->f), c->on_air,
			      "the field model does not produce 0x%02X (%s)",
			      c->on_air, c->what);

		/* Byte -> fields, and it must be a genuine inverse rather than
		 * a second table that happens to agree today. */
		zassert_equal(ant_ctrl_decode(c->on_air, &got), ANT_FRAME_OK,
			      NULL);
		zassert_equal(got.exchange, c->f.exchange, "b7 of 0x%02X",
			      c->on_air);
		zassert_equal(got.ack, c->f.ack, "b6 of 0x%02X", c->on_air);
		zassert_equal(got.last, c->f.last, "b5 of 0x%02X", c->on_air);
		zassert_equal(got.seq, c->f.seq, "b4 of 0x%02X", c->on_air);
		zassert_equal(got.slot_opener, c->f.slot_opener, "b3 of 0x%02X",
			      c->on_air);

		zassert_equal(ant_frame_msg_type(c->on_air), c->type,
			      "0x%02X (%s) decoded as the wrong message",
			      c->on_air, c->what);
		zassert_true(ant_ctrl_observed(c->on_air), NULL);

		/* Measured on all 3,104 CRC-valid frames of both parts. */
		zassert_true(ant_ctrl_low_ok(c->on_air),
			     "0x%02X does not carry 010 in bits 2:0", c->on_air);
	}

	/* Measured: bits 7:5 never took 001, 010 or 011, so b7 = 0 occurred
	 * only ever as 0x0A. There is one broadcast encoding. */
	zassert_equal(ant_frame_msg_type(0x2Au), ANT_MSG_UNKNOWN, NULL);
	zassert_equal(ant_frame_msg_type(0x4Au), ANT_MSG_UNKNOWN, NULL);
	zassert_equal(ant_frame_msg_type(0x6Au), ANT_MSG_UNKNOWN, NULL);
	zassert_false(ant_ctrl_observed(0x2Au), NULL);
	zassert_false(ant_ctrl_observed(0x4Au), NULL);
	zassert_false(ant_ctrl_observed(0x6Au), NULL);

	/* And the twelve slot-opener variants of the in-slot values, which the
	 * field model can express and nothing has transmitted. 0x8A and 0xAA
	 * are the two that HAVE been, so they are excluded. */
	zassert_false(ant_ctrl_observed(0xC2u | ANT_CTRL_SLOT), NULL); /* 0xCA */
	zassert_false(ant_ctrl_observed(0xD2u | ANT_CTRL_SLOT), NULL); /* 0xDA */
	zassert_false(ant_ctrl_observed(0xE2u | ANT_CTRL_SLOT), NULL); /* 0xEA */
	zassert_false(ant_ctrl_observed(0xF2u | ANT_CTRL_SLOT), NULL); /* 0xFA */
	zassert_false(ant_ctrl_observed(0x92u | ANT_CTRL_SLOT), NULL); /* 0x9A */
	zassert_false(ant_ctrl_observed(0xB2u | ANT_CTRL_SLOT), NULL); /* 0xBA */
}

/*
 * 0xAA and 0xA2 are the same frame apart from the slot bit, and that is the
 * single most load-bearing sentence in part 2.
 *
 * Part 1 saw 0xAA four times from a one-block burst and wrote down "do not read
 * that as burst-last". Part 2's run C is the control that settles it: the same
 * Feather, the same stack, the same driver script, the same payload bytes, with
 * only the channel role changed from slave to master - 0xA2 became 0xAA and
 * 0x82 became 0x8A. One bit moved. Acknowledged data IS a one-packet burst, on
 * air and byte for byte, which is why ant_ack.c and ant_burst.c share one
 * encoder and why a receiver dispatching on this byte cannot tell them apart.
 */
ZTEST(ant_frame, test_ack_data_is_a_one_packet_burst)
{
	struct ant_ctrl_fields open, in_slot;

	zassert_equal(ant_ctrl_decode(0xAAu, &open), ANT_FRAME_OK, NULL);
	zassert_equal(ant_ctrl_decode(0xA2u, &in_slot), ANT_FRAME_OK, NULL);

	zassert_equal(open.exchange, in_slot.exchange, NULL);
	zassert_equal(open.ack, in_slot.ack, NULL);
	zassert_equal(open.last, in_slot.last, NULL);
	zassert_equal(open.seq, in_slot.seq, NULL);
	zassert_not_equal(open.slot_opener, in_slot.slot_opener,
			  "the slot bit is the ONLY thing separating 0xAA from "
			  "0xA2; if it is not, run C's control proved nothing");

	/* Same message either way, and it is burst-last rather than a type of
	 * its own. */
	zassert_equal(ant_frame_msg_type(0xAAu), ANT_MSG_BURST_LAST, NULL);
	zassert_equal(ant_frame_msg_type(0xA2u), ANT_MSG_BURST_LAST, NULL);
	zassert_equal(ant_frame_msg_type(0xAAu), ant_frame_msg_type(0xA2u), NULL);

	/* And the same one bit apart for the burst-packet-0 pair. */
	zassert_equal((uint8_t)(0x82u | ANT_CTRL_SLOT), 0x8Au, NULL);
	zassert_equal(ant_frame_msg_type(0x8Au), ANT_MSG_BURST_DATA, NULL);
	zassert_equal(ant_frame_msg_type(0x82u), ANT_MSG_BURST_DATA, NULL);

	/* There is no ANT_MSG_ACKNOWLEDGED, and that absence is the finding. */
	zassert_not_equal(ant_frame_msg_type(0xA2u), ANT_MSG_BURST_ACK,
			  "acknowledged DATA is not an acknowledgeMENT; b6 is "
			  "what separates them");
}

/*
 * The reply frame - the row part 1 listed as never observed.
 *
 * Bit 6 set, bit 5 echoed, bit 4 complemented: 82 -> D2, 92 -> C2, A2 -> F2,
 * B2 -> E2, on every one of the 168 adjacent CRC-valid data/acknowledgement
 * pairs in runs 0, A and B. Read as "the sequence bit I expect next" that is
 * exactly right, and read as "an echo of what I just received" it is exactly
 * wrong - which is why the complement is asserted rather than the echo.
 */
ZTEST(ant_frame, test_reply_frame_relationship)
{
	static const struct {
		uint8_t data;
		uint8_t reply;
	} pairs[] = {
		{ 0x82u, 0xD2u },
		{ 0x92u, 0xC2u },
		{ 0xA2u, 0xF2u },
		{ 0xB2u, 0xE2u },
	};

	for (size_t i = 0; i < ARRAY_SIZE(pairs); i++) {
		uint8_t d = pairs[i].data;
		uint8_t r = ant_ctrl_reply_for(d);

		zassert_equal(r, pairs[i].reply,
			      "0x%02X must be acknowledged with 0x%02X", d,
			      pairs[i].reply);

		/* Stated as the three field relations as well as the byte, so a
		 * table typo cannot pass both. */
		zassert_true(ant_ctrl_is_ack(r), "bit 6 must be set");
		zassert_false(ant_ctrl_is_ack(d), "a data packet has bit 6 clear");
		zassert_equal(ant_ctrl_is_last(r), ant_ctrl_is_last(d),
			      "bit 5 is echoed, not complemented");
		zassert_not_equal(ant_ctrl_seq(r), ant_ctrl_seq(d),
				  "bit 4 is complemented, not echoed - an ack "
				  "carries the sequence bit it expects NEXT");
		zassert_equal(ant_ctrl_is_exchange(r), ant_ctrl_is_exchange(d),
			      NULL);
		zassert_equal(ant_ctrl_is_slot_opener(r),
			      ant_ctrl_is_slot_opener(d), NULL);
		zassert_true(ant_ctrl_observed(r), NULL);
	}

	/*
	 * And what was NOT measured. Every one of the 168 pairs is an in-slot
	 * frame answered by an in-slot frame; no acknowledgement of a
	 * slot-opening data packet was ever captured, so there is no measured
	 * answer for what bit 3 of one would be. Returning 0 - never a legal
	 * control byte - is how that stays visible.
	 */
	zassert_equal(ant_ctrl_reply_for(0x8Au), 0u,
		      "there is no measured acknowledgement of a slot-opening "
		      "burst packet, and inventing one is how bit 3 gets fixed "
		      "by guess");
	zassert_equal(ant_ctrl_reply_for(0xAAu), 0u, NULL);
	zassert_equal(ant_ctrl_reply_for(ANT_CTRL_BROADCAST), 0u,
		      "a broadcast is not acknowledged");
	zassert_equal(ant_ctrl_reply_for(0xC2u), 0u,
		      "an acknowledgement is not itself acknowledged");
}

/*
 * THE test. Byte 3 is not a length, and the evidence is a pair of frames one
 * slot apart carrying the same eight payload bytes with byte 3 changing
 * 0xAA -> 0x0A. Encoding that pair and getting two frames that differ in
 * exactly one byte is what a length field cannot do, so this test fails on any
 * implementation that derives byte 3 from payload_len.
 */
ZTEST(ant_frame, test_same_payload_two_control_bytes)
{
	struct ant_frame ack, bcast, out;
	struct ant_frame_wire w_ack, w_bcast;
	uint8_t flat_ack[32], flat_bcast[32];
	unsigned int differing = 0;

	zassert_equal(ant_frame_make(&ack, &spike_id, &ctrl_ack_data_open,
				     spike_b_payload, sizeof(spike_b_payload)),
		      ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_make(&bcast, &spike_id, &ctrl_broadcast,
				     spike_b_payload, sizeof(spike_b_payload)),
		      ANT_FRAME_OK, NULL);

	zassert_equal(ack.payload_len, bcast.payload_len,
		      "the two frames must carry the same number of payload "
		      "bytes or this test proves nothing");
	zassert_equal(ack.ctrl_byte, 0xAAu, NULL);
	zassert_equal(bcast.ctrl_byte, 0x0Au, NULL);

	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &ack, &w_ack),
		      ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &bcast, &w_bcast),
		      ANT_FRAME_OK, NULL);

	zassert_equal(w_ack.body_len, w_bcast.body_len, NULL);
	zassert_equal(ant_frame_to_bytes(&w_ack, flat_ack, sizeof(flat_ack)),
		      17, NULL);
	zassert_equal(ant_frame_to_bytes(&w_bcast, flat_bcast,
					 sizeof(flat_bcast)), 17, NULL);

	/* Byte 6 of the flat frame is byte 3 of the body: 5 address bytes then
	 * the transmission type. The CRC differs too, of course. */
	for (size_t i = 0; i < 15u; i++) {
		if (flat_ack[i] != flat_bcast[i]) {
			differing++;
			zassert_equal(i, 6u,
				      "the two frames differ at byte %zu, but "
				      "the capture shows them differing only "
				      "in the control byte", i);
		}
	}
	zassert_equal(differing, 1u,
		      "an acknowledged frame and a broadcast of the same "
		      "payload must differ in exactly one covered byte");
	zassert_not_equal(w_ack.crc, w_bcast.crc, NULL);

	/* And the receive side sorts them apart, which is the whole point:
	 * both decode, both yield the same payload, and the message type is
	 * what distinguishes them. */
	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w_ack, 0, &out),
		      ANT_FRAME_OK,
		      "an acknowledged frame must decode - a receiver that "
		      "drops these cannot set trainer resistance");
	zassert_equal(ant_frame_msg_type(out.ctrl_byte), ANT_MSG_BURST_LAST,
		      "acknowledged data is burst-last on air; part 1 refused "
		      "to say so and part 2's run C settled it");
	zassert_mem_equal(out.payload, spike_b_payload,
			  sizeof(spike_b_payload), NULL);

	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w_bcast, 0,
				       &out), ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_msg_type(out.ctrl_byte), ANT_MSG_BROADCAST,
		      NULL);
	zassert_mem_equal(out.payload, spike_b_payload,
			  sizeof(spike_b_payload), NULL);
}

/*
 * THE REGRESSION TEST FOR THE LIVE BUG.
 *
 * A slave's in-slot frame - bit 3 clear - must make, encode, transmit-shape and
 * decode. The version of ant_frame.c written against Spike B part 1 rejected
 * every one of these with ANT_FRAME_ELEN, because 0xA2's low five bits are
 * 00010 = 2 and the check wanted payload_len + 2 = 10. Nothing on the receive
 * side would have got through: every frame a slave sends has bit 3 clear.
 */
ZTEST(ant_frame, test_a_slave_frame_is_not_rejected)
{
	struct ant_frame f, out;
	struct ant_frame_wire w;
	uint8_t flat[32];

	zassert_equal(ant_frame_make(&f, &spike_id, &ctrl_burst_last_seq0,
				     spike_b_payload, sizeof(spike_b_payload)),
		      ANT_FRAME_OK,
		      "an in-slot acknowledged-data frame was refused");
	zassert_equal(f.ctrl_byte, 0xA2u, NULL);
	zassert_false(ant_ctrl_is_slot_opener(f.ctrl_byte), NULL);
	zassert_equal((uint8_t)(f.ctrl_byte & 0x1Fu), 2u,
		      "0xA2's low five bits read 2, and that is the number a "
		      "length check would have wanted to be 10");

	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &w),
		      ANT_FRAME_OK, "encode rejected a valid slave frame");
	zassert_equal(w.body[1], 0xA2u, NULL);
	zassert_equal(ant_frame_to_bytes(&w, flat, sizeof(flat)), 17, NULL);

	zassert_equal(ant_frame_from_bytes(ANT_FRAME_CFG_TRACKING, flat,
					   sizeof(flat), &w),
		      17, "the flat parser rejected a valid slave frame");
	zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w, 0, &out),
		      ANT_FRAME_OK, "decode rejected a valid slave frame");
	zassert_equal(out.ctrl_byte, 0xA2u, NULL);
	zassert_equal(out.payload_len, 8u, NULL);
	zassert_mem_equal(out.payload, spike_b_payload, sizeof(spike_b_payload),
			  NULL);

	/* And the acknowledgement the master answers it with, which is a full
	 * eight-byte frame of the acknowledger's own broadcast buffer. */
	zassert_equal(ant_ctrl_reply_for(out.ctrl_byte), 0xF2u, NULL);
}

/*
 * Every measured control byte must parse out of flat bytes as a 17-byte frame -
 * all eleven, not the three part 1 knew. The eight with bit 3 clear are exactly
 * the ones a length reading of bits 4:0 rejects, and they are also every frame
 * a slave sends. This test names the wrong answer so it cannot pass by
 * accident.
 */
ZTEST(ant_frame, test_flat_parse_accepts_every_measured_control_byte)
{
	uint8_t buf[64];

	for (size_t i = 0; i < ARRAY_SIZE(ctrl_cases); i++) {
		struct ant_frame_wire w;
		struct ant_frame f;
		uint16_t crc;

		memcpy(buf, spike15, sizeof(spike15));
		buf[6] = ctrl_cases[i].on_air;
		crc = ant_crc16(ANT_CRC_INIT, buf, 15u);
		buf[15] = (uint8_t)(crc >> 8);
		buf[16] = (uint8_t)(crc & 0xFFu);

		zassert_equal(ant_frame_from_bytes(ANT_FRAME_CFG_TRACKING, buf,
						   sizeof(buf), &w),
			      17,
			      "control byte 0x%02X (%s) was not parsed as a "
			      "17-byte frame; its low five bits read %u, and a "
			      "parser expecting 10 drops it",
			      ctrl_cases[i].on_air, ctrl_cases[i].what,
			      (unsigned int)(ctrl_cases[i].on_air & 0x1Fu));
		zassert_equal(w.body_len, 10u, NULL);
		zassert_equal(ant_frame_decode(ANT_FRAME_CFG_TRACKING, &w, 0,
					       &f), ANT_FRAME_OK, NULL);
		zassert_equal(f.payload_len, 8u, NULL);
		zassert_equal(f.ctrl_byte, ctrl_cases[i].on_air, NULL);
		zassert_mem_equal(f.payload, spike_payload,
				  sizeof(spike_payload), NULL);
	}
}

/* ---------------------------------------------------------------------------
 * Boundary: a 24-byte payload, and the prediction that is withdrawn
 * ---------------------------------------------------------------------------
 */

/*
 * THE ADVANCED-BURST CONTROL-BYTE PREDICTION IS WITHDRAWN, NOT RESTATED.
 *
 * docs/ant-radio-link.md once predicted 0x1A for a 24-byte advanced-burst
 * frame. A later pass corrected that to 0x9A, on the reading that bits 4:0
 * carry payload + 2 with the burst type bits above them. Spike B part 2
 * falsified the premise: 0x0A's low five bits read 10 and 0xA2's read 2, both
 * with an eight-byte payload, so bits 4:0 are not a length and there is nothing
 * to compute a prediction from. Neither 0x1A nor 0x9A is asserted as a control
 * byte anywhere in this module or this file, and the test that pinned 0x9A was
 * deleted rather than adjusted - a test asserting a withdrawn prediction is
 * worse than no test, because it makes the prediction look checked.
 *
 * (For the record: the only 0x1A this bench has ever seen appeared twice, in
 * runs B and C, as a CRC-FAILED frame one bit away from 0x0A. It is a bit
 * error.)
 *
 * What this test asserts is only what is not speculative: the control byte does
 * not move when the payload length does - which is the positive form of "there
 * is no length in it" - the module's arithmetic holds at the top of its range,
 * the coverage invariant still binds the two configurations at 31 bytes, and
 * NEITHER hardware format claims it can carry the frame.
 */
ZTEST(ant_frame, test_long_payload_arithmetic_and_the_format_limit)
{
	struct ant_frame f, f8;
	struct ant_frame_wire wt, ws;
	uint8_t payload[ANT_FRAME_PAYLOAD_MAX];
	uint8_t flat_t[64], flat_s[64];

	for (uint8_t i = 0; i < sizeof(payload); i++) {
		payload[i] = (uint8_t)(0x20u + i);
	}

	zassert_equal(ant_frame_make(&f, &spike_id, &ctrl_burst_open_seq0,
				     payload, sizeof(payload)),
		      ANT_FRAME_OK, NULL);
	zassert_equal(ant_frame_make(&f8, &spike_id, &ctrl_burst_open_seq0,
				     payload, 8u),
		      ANT_FRAME_OK, NULL);

	/*
	 * THE assertion. Tripling the payload changes nothing in the control
	 * byte, because no bit of it depends on the payload length. Any
	 * implementation that still derives a length here fails on this line.
	 */
	zassert_equal(f.ctrl_byte, f8.ctrl_byte,
		      "the control byte moved when the payload length did; "
		      "bits 4:0 are the slot bit, the sequence bit and 010, not "
		      "a length");
	zassert_equal(f.ctrl_byte, 0x8Au,
		      "a slot-opening burst packet is 0x8A whatever it carries");
	zassert_equal(ant_frame_msg_type(f.ctrl_byte), ANT_MSG_BURST_DATA, NULL);

	zassert_equal(ant_frame_encode(ANT_FRAME_CFG_TRACKING,
				       ant_net_addr_ant_plus, &f, &wt),
		      ANT_FRAME_OK, NULL);
	zassert_equal(wt.body_len, 26, NULL);
	zassert_equal(wt.body[1], 0x8Au, NULL);

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

	/* Round-trips as software, in both configurations - the frame layer is
	 * arithmetic and has no opinion about what a radio can do. */
	roundtrip(ANT_FRAME_CFG_TRACKING, &spike_id, &ctrl_burst_open_seq0,
		  payload, sizeof(payload));
	roundtrip(ANT_FRAME_CFG_SEARCH, &spike_id, &ctrl_burst_open_seq0,
		  payload, sizeof(payload));

	/* But neither hardware format may claim to carry it. Both are
	 * static-length at the only payload size ever measured, and a format
	 * that advertised more would have a backend arming a window for a
	 * frame nothing transmits. */
	zassert_equal(ant_frame_format(ANT_FRAME_CFG_TRACKING)->max_body_len, 10,
		      "tracking is STATLEN=10; a longer frame needs its own "
		      "format and a measurement to justify it");
	zassert_equal(ant_frame_format(ANT_FRAME_CFG_SEARCH)->max_body_len, 12,
		      "search cannot receive a longer-than-standard frame, and "
		      "the format must keep saying so");
	zassert_true(wt.body_len >
			     ant_frame_format(ANT_FRAME_CFG_TRACKING)->max_body_len,
		     "this test only means something while the encoder can "
		     "produce a body the hardware format refuses");
}
