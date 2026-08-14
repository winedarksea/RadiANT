/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_profile_rd.c - ANT+ Running Dynamics 0x1E: the page tables, the master
 * rotation, and the sample-bus adapter.
 *
 * Vectors are HAND-PLACED BIT PATTERNS, not encoder output fed back into the
 * decoder - a round-trip-only suite would pass even with a split field read
 * backwards on both sides. Shared with tools/test_ant_pages.py so both
 * implementations are checked against the document, not against each other.
 */

#include <zephyr/ztest.h>

#include <string.h>

#include "profile_rd.h"
#include "radiant_bridge.h"
#include "radiant_rd_adapter.h"

/* Cadence 180.25 strides/min, VO 96.5 mm, GCT 261 ms, stance 35.75 %, 100
 * steps - the same numbers tools/test_ant_pages.py uses. */
#define RD_CADENCE_32 5768u
#define RD_VO_QUARTER 386u
#define RD_GCT_MS     261u
#define RD_STANCE_Q   143u
#define RD_STEPS      100u

/* Balance 49.5 %, vertical ratio 7.75 %, step length 1200 mm. */
#define RD_BALANCE_32   1584u
#define RD_VERT_RATIO   248u
#define RD_STEP_LEN_MM  1200u

static struct profile_rd_metrics sample_metrics(void)
{
	struct profile_rd_metrics m;

	memset(&m, 0, sizeof(m));
	m.cadence_32 = RD_CADENCE_32;
	m.vert_osc_quarter_mm = RD_VO_QUARTER;
	m.gct_ms = RD_GCT_MS;
	m.stance_quarter = RD_STANCE_Q;
	m.step_count = RD_STEPS;
	m.balance_32 = RD_BALANCE_32;
	m.vert_ratio_32 = RD_VERT_RATIO;
	m.step_length_mm = RD_STEP_LEN_MM;
	return m;
}

/* ── Page 0x00 ──────────────────────────────────────────────────────────── */

ZTEST(profile_rd, test_page_a_bit_placement)
{
	struct profile_rd_metrics m = sample_metrics();
	uint8_t body[8];

	zassert_equal(profile_rd_encode_a(&m, false, body), 0);

	zassert_equal(body[0], PROFILE_RD_PAGE_A);
	zassert_equal(body[1], 180, "integer strides per minute");
	zassert_equal(body[2] & 0x1Fu, 8, "8/32 = 0.25 strides/min");
	zassert_equal(body[2] & 0x80u, 0, "byte 2 bit 7 is reserved, set to 0");

	zassert_equal(body[3], 96, "vertical oscillation, whole millimetres");
	zassert_equal(body[4] & 0x07u, 0, "96 needs none of the three high bits");
	zassert_equal((body[4] >> 3) & 0x03u, 2, "0.5 mm = 2 quarters");

	/* Ground contact time keeps its THREE LOW bits in byte [4] and its
	 * eight high bits in byte [5] - the opposite of every other split
	 * field on this profile. */
	zassert_equal((body[4] >> 5) & 0x07u, RD_GCT_MS & 0x07u);
	zassert_equal(body[5], RD_GCT_MS >> 3);

	/* Stance 143 quarters = 35 % remainder 3 (0b11), split across the byte
	 * boundary LOW BIT FIRST. */
	zassert_equal(body[6] & 0x7Fu, 35);
	zassert_equal((body[6] >> 7) & 0x01u, 1, "fraction bit 0");
	zassert_equal(body[7] & 0x01u, 1, "fraction bit 1");
	zassert_equal((body[7] >> 1) & 0x7Fu, RD_STEPS);
}

ZTEST(profile_rd, test_page_a_round_trip)
{
	struct profile_rd_metrics m = sample_metrics();
	struct profile_rd_metrics got;
	uint8_t body[8];
	bool    bidirectional = false;

	m.walking = true;
	zassert_equal(profile_rd_encode_a(&m, true, body), 0);

	memset(&got, 0, sizeof(got));
	zassert_equal(profile_rd_decode_a(body, &got, &bidirectional), 0);
	zassert_equal(got.cadence_32, RD_CADENCE_32);
	zassert_equal(got.vert_osc_quarter_mm, RD_VO_QUARTER);
	zassert_equal(got.gct_ms, RD_GCT_MS);
	zassert_equal(got.stance_quarter, RD_STANCE_Q);
	zassert_equal(got.step_count, RD_STEPS);
	zassert_true(got.walking);
	zassert_true(bidirectional);
}

ZTEST(profile_rd, test_page_a_gct_uses_all_eleven_bits)
{
	struct profile_rd_metrics m;
	struct profile_rd_metrics got;
	uint8_t body[8];

	memset(&m, 0, sizeof(m));
	m.gct_ms = PROFILE_RD_GCT_MAX;
	zassert_equal(profile_rd_encode_a(&m, false, body), 0);

	memset(&got, 0, sizeof(got));
	zassert_equal(profile_rd_decode_a(body, &got, NULL), 0);
	zassert_equal(got.gct_ms, PROFILE_RD_GCT_MAX);
}

ZTEST(profile_rd, test_invalid_is_zero_and_never_ff)
{
	struct profile_rd_metrics m;
	struct profile_rd_metrics got;
	uint8_t body[8];
	int     i;

	memset(&m, 0, sizeof(m));
	zassert_equal(profile_rd_encode_a(&m, false, body), 0);

	/* This is the one implemented profile where the common 0xFF sentinel
	 * does not apply to any measurement. An all-invalid page 0x00 contains
	 * no 0xFF at all. */
	for (i = 1; i < 8; i++) {
		zassert_not_equal(body[i], 0xFFu,
				  "byte %d carries the wrong sentinel", i);
	}

	memset(&got, 0, sizeof(got));
	zassert_equal(profile_rd_decode_a(body, &got, NULL), 0);
	zassert_equal(got.cadence_32, PROFILE_RD_INVALID_CADENCE);
	zassert_equal(got.vert_osc_quarter_mm, PROFILE_RD_INVALID_VERTICAL_OSC);
	zassert_equal(got.stance_quarter, PROFILE_RD_INVALID_STANCE);
}

/* ── Page 0x01 ──────────────────────────────────────────────────────────── */

ZTEST(profile_rd, test_page_b_bit_placement)
{
	struct profile_rd_metrics m = sample_metrics();
	uint8_t body[8];

	zassert_equal(profile_rd_encode_b(&m, 0x1234u, body), 0);

	zassert_equal(body[0], PROFILE_RD_PAGE_B);
	zassert_equal(body[1] & 0x7Fu, 49, "whole percent of balance");
	/* 1584 = 49*32 + 16, so the 5-bit fraction is 0b10000: bit 0 clear,
	 * high four bits 0b1000. */
	zassert_equal((body[1] >> 7) & 0x01u, 0);
	zassert_equal(body[2] & 0x0Fu, 8);

	zassert_equal((body[2] >> 4) & 0x0Fu, 7u & 0x0Fu, "ratio low nibble");
	zassert_equal(body[3] & 0x07u, 7u >> 4, "ratio high bits");
	zassert_equal((body[3] >> 3) & 0x1Fu, RD_VERT_RATIO % 32u);

	zassert_equal(body[4], RD_STEP_LEN_MM & 0xFFu);
	zassert_equal(body[5] & 0x1Fu, RD_STEP_LEN_MM >> 8);
	/* Page 0x01's two reserved bits are 0b11, where page 0x00's single
	 * reserved bit is 0. Both transcribed; neither a typo. */
	zassert_equal(body[5] & 0xC0u, 0xC0u);
	zassert_equal(body[5] & 0x20u, 0, "right side up");

	zassert_equal(body[6], 0x34u);
	zassert_equal(body[7], 0x12u);
}

ZTEST(profile_rd, test_page_b_round_trip)
{
	struct profile_rd_metrics m = sample_metrics();
	struct profile_rd_metrics got;
	uint8_t  body[8];
	uint16_t leader = 0u;

	m.upside_down = true;
	zassert_equal(profile_rd_encode_b(&m, 0x1234u, body), 0);

	memset(&got, 0, sizeof(got));
	zassert_equal(profile_rd_decode_b(body, &got, &leader), 0);
	zassert_equal(got.balance_32, RD_BALANCE_32);
	zassert_equal(got.vert_ratio_32, RD_VERT_RATIO);
	zassert_equal(got.step_length_mm, RD_STEP_LEN_MM);
	zassert_true(got.upside_down);
	zassert_equal(leader, 0x1234u);
}

ZTEST(profile_rd, test_balance_fraction_is_split_low_bit_first)
{
	struct profile_rd_metrics m;
	struct profile_rd_metrics got;
	uint8_t body[8];

	/* 32 % + 1/32. The fraction is 1, so its LOW bit is set and the four
	 * high bits are clear - which is the case a reversed reading gets
	 * wrong, and the reason a round-trip-only test would not catch it. */
	memset(&m, 0, sizeof(m));
	m.balance_32 = 32u * 32u + 1u;
	zassert_equal(profile_rd_encode_b(&m, PROFILE_RD_LEADER_NONE, body), 0);
	zassert_equal((body[1] >> 7) & 0x01u, 1);
	zassert_equal(body[2] & 0x0Fu, 0);

	memset(&got, 0, sizeof(got));
	zassert_equal(profile_rd_decode_b(body, &got, NULL), 0);
	zassert_equal(got.balance_32, 32u * 32u + 1u);
}

ZTEST(profile_rd, test_percentage_reserved_range_is_refused)
{
	struct profile_rd_metrics m;
	uint8_t body[8];

	/* 101..127 % is reserved on every percentage field, so a sensor
	 * clamping a computed percentage must clamp to 100 rather than to the
	 * field maximum. Encoding refuses rather than transmitting it. */
	memset(&m, 0, sizeof(m));
	m.stance_quarter = 101u * PROFILE_RD_STANCE_PER_PERCENT;
	zassert_equal(profile_rd_encode_a(&m, false, body), -EINVAL);

	memset(&m, 0, sizeof(m));
	m.balance_32 = 101u * PROFILE_RD_BALANCE_PER_PERCENT;
	zassert_equal(profile_rd_encode_b(&m, PROFILE_RD_LEADER_NONE, body),
		      -EINVAL);
}

ZTEST(profile_rd, test_oversized_values_are_refused_not_masked)
{
	struct profile_rd_metrics m;
	uint8_t body[8];

	memset(&m, 0, sizeof(m));
	m.gct_ms = PROFILE_RD_GCT_MAX + 1u;
	zassert_equal(profile_rd_encode_a(&m, false, body), -EINVAL,
		      "a masked measurement is a plausible wrong number");
}

/* ── Pages 0x10, 0x20, 0x4A ─────────────────────────────────────────────── */

ZTEST(profile_rd, test_speed_page_and_its_two_sentinels)
{
	uint8_t  body[8];
	uint16_t speed = 0u;

	zassert_equal(profile_rd_encode_speed(3u * 256u + 128u, body), 0);
	zassert_equal(profile_rd_decode_speed(body, &speed), 0);
	zassert_equal(speed, 3u * 256u + 128u);
	zassert_equal(body[3], 0xFFu);
	zassert_equal(body[7], 0xFFu);

	zassert_equal(profile_rd_encode_speed(PROFILE_RD_SPEED_INVALID, body), 0);
	zassert_equal(body[1], 0xFFu);
	zassert_equal(body[2], 0xFFu);
	zassert_equal(profile_rd_decode_speed(body, &speed), 0);
	zassert_equal(speed, PROFILE_RD_SPEED_INVALID);

	/* Either sentinel alone invalidates the reading: they are different
	 * values in different widths and the document gives each its own. */
	zassert_equal(profile_rd_encode_speed(3u * 256u + 128u, body), 0);
	body[1] = (uint8_t)((body[1] & 0xF0u) | PROFILE_RD_SPEED_INVALID_INT);
	zassert_equal(profile_rd_decode_speed(body, &speed), 0);
	zassert_equal(speed, PROFILE_RD_SPEED_INVALID);
}

ZTEST(profile_rd, test_speed_stops_below_fifteen_mps)
{
	uint8_t body[8];

	/* 0x0F is the integer field's sentinel, so 15 m/s is not expressible
	 * and the range stops at 14.996 m/s. */
	zassert_equal(profile_rd_encode_speed(15u * 256u, body), -EINVAL);
	zassert_equal(profile_rd_encode_speed(14u * 256u + 255u, body), 0);
}

ZTEST(profile_rd, test_leader_request)
{
	uint8_t  body[8];
	uint16_t id = 0u;

	zassert_equal(profile_rd_encode_leader_request(0x2A2Bu, body), 0);
	zassert_equal(profile_rd_decode_leader_request(body, &id), 0);
	zassert_equal(id, 0x2A2Bu);

	zassert_equal(profile_rd_encode_leader_request(PROFILE_RD_LEADER_NONE,
						       body), -EINVAL);
}

ZTEST(profile_rd, test_open_channel_page)
{
	uint8_t  body[8];
	uint32_t leader = 0u;
	uint8_t  rf = 0u;
	uint16_t period = 0u;

	zassert_equal(profile_rd_encode_open_channel(0xAABBCCu, PROFILE_RD_RF_2461,
						     PROFILE_RD_PERIOD_HR_RD,
						     body), 0);
	zassert_equal(body[4], PROFILE_RD_DEVICE_TYPE);
	zassert_equal(profile_rd_decode_open_channel(body, &leader, &rf, &period),
		      0);
	zassert_equal(leader, 0xAABBCCu);
	zassert_equal(rf, PROFILE_RD_RF_2461);
	zassert_equal(period, PROFILE_RD_PERIOD_HR_RD);

	/* The ANT+ public frequency is NOT a legal RD channel for an HR-RD
	 * strap, and the standalone pod's 8 Hz period is not this channel's. */
	zassert_equal(profile_rd_encode_open_channel(1u, PROFILE_RD_RF_FREQ,
						     PROFILE_RD_PERIOD_HR_RD,
						     body), -EINVAL);
	zassert_equal(profile_rd_encode_open_channel(1u, PROFILE_RD_RF_2461,
						     PROFILE_RD_PERIOD, body),
		      -EINVAL);
}

ZTEST(profile_rd, test_rf_enumeration_is_not_sorted)
{
	/* Code 3 is 2475 MHz and code 4 is 2461 MHz. The document states this
	 * twice and both statements agree, so it is a table and not a slip. */
	zassert_equal(profile_rd_rf_enum_index(PROFILE_RD_RF_ENUM_2403),
		      PROFILE_RD_RF_2403);
	zassert_equal(profile_rd_rf_enum_index(PROFILE_RD_RF_ENUM_2439),
		      PROFILE_RD_RF_2439);
	zassert_equal(profile_rd_rf_enum_index(3u), PROFILE_RD_RF_2475);
	zassert_equal(profile_rd_rf_enum_index(4u), PROFILE_RD_RF_2461);

	zassert_equal(profile_rd_rf_index_enum(PROFILE_RD_RF_2475), 3u);
	zassert_equal(profile_rd_rf_index_enum(PROFILE_RD_RF_2461), 4u);
}

ZTEST(profile_rd, test_rf_enumeration_refuses_everything_else)
{
	/* 0 is "RD channel not open"; every other unlisted value is
	 * manufacturer-specific data on a strap WITHOUT running dynamics, and
	 * the profile forbids interpreting it. Both answer 0 so that "do not
	 * interpret" is the default rather than something a caller remembers. */
	zassert_equal(profile_rd_rf_enum_index(PROFILE_RD_RF_ENUM_INVALID), 0u);
	zassert_equal(profile_rd_rf_enum_index(0xFFu), 0u);
	zassert_equal(profile_rd_rf_enum_index(PROFILE_RD_RF_FREQ), 0u);
	zassert_equal(profile_rd_rf_index_enum(PROFILE_RD_RF_FREQ),
		      PROFILE_RD_RF_ENUM_INVALID);
}

ZTEST(profile_rd, test_page_numbers_carry_no_toggle)
{
	struct profile_rd_metrics m = sample_metrics();
	uint8_t body[8];

	/* Heart rate reserves byte [0] bit 7 as the page-change toggle; this
	 * device type does not, and byte [0] is the whole page number. */
	zassert_equal(profile_rd_encode_a(&m, false, body), 0);
	zassert_equal(body[0], PROFILE_RD_PAGE_A);
	zassert_equal(profile_rd_encode_b(&m, PROFILE_RD_LEADER_NONE, body), 0);
	zassert_equal(body[0], PROFILE_RD_PAGE_B);
}

/* ── The master ─────────────────────────────────────────────────────────── */

static void rd_init_default(struct profile_rd *rd, bool hr_rd)
{
	struct profile_rd_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id.manufacturer_id = 0x00FFu;
	cfg.id.model_number = 0x1234u;
	cfg.id.hw_revision = 1u;
	cfg.id.sw_revision_main = 2u;
	cfg.id.sw_revision_supplemental = PROFILE_COMMON_INVALID_U8;
	cfg.id.serial_number = PROFILE_COMMON_INVALID_U32;
	cfg.hr_rd = hr_rd;

	zassert_equal(profile_rd_init(rd, &cfg), 0);
}

ZTEST(profile_rd, test_rotation_alternates_the_two_main_pages)
{
	struct profile_rd rd;
	struct profile_rd_metrics m = sample_metrics();
	uint8_t body[8];
	int     i;
	int     a = 0;
	int     b = 0;
	int     common = 0;

	rd_init_default(&rd, false);
	profile_rd_set_metrics(&rd, &m);

	/* One full 121-message cycle: pages 0x00 and 0x01 alternate in the
	 * data slots, which is the document's "~2 Hz each" at 8 Hz, and the
	 * common pages arrive on the engine's 119/120-of-121 cadence - inside
	 * this profile's "at least once every 260 messages". */
	for (i = 0; i < 121; i++) {
		enum profile_slot_kind kind = profile_rd_next(&rd, body);

		if (kind == PROFILE_SLOT_DATA) {
			if (body[0] == PROFILE_RD_PAGE_A) {
				a++;
			} else if (body[0] == PROFILE_RD_PAGE_B) {
				b++;
			} else {
				zassert_unreachable("page 0x%02X in a data slot",
						    body[0]);
			}
		} else if (kind == PROFILE_SLOT_COMMON_80 ||
			   kind == PROFILE_SLOT_COMMON_81) {
			common++;
		}
	}

	zassert_equal(common, 2, "pages 80 and 81, once each per cycle");
	zassert_equal(a + b, 119);
	zassert_within(a, b, 1, "the two main pages alternate");
}

ZTEST(profile_rd, test_hr_rd_strap_reports_the_invalid_leader)
{
	struct profile_rd rd;
	struct profile_rd_metrics m = sample_metrics();
	uint8_t  body[8];
	uint16_t leader = 0u;
	int      i;

	rd_init_default(&rd, true);
	profile_rd_set_metrics(&rd, &m);

	for (i = 0; i < 8; i++) {
		if (profile_rd_next(&rd, body) == PROFILE_SLOT_DATA &&
		    body[0] == PROFILE_RD_PAGE_B) {
			zassert_equal(profile_rd_decode_b(body, &m, &leader), 0);
			zassert_equal(leader, PROFILE_RD_LEADER_HR_RD);
			break;
		}
	}
	zassert_equal(leader, PROFILE_RD_LEADER_HR_RD, "no page 0x01 in 8 slots");

	/* Session leadership on an HR-RD strap is settled by page 74 on the
	 * heart-rate channel, so page 0x20 is refused here. */
	zassert_equal(profile_rd_claim_leader(&rd, 0x2A2Bu), -EINVAL);
}

ZTEST(profile_rd, test_session_leader_is_first_come)
{
	struct profile_rd rd;

	rd_init_default(&rd, false);
	zassert_equal(profile_rd_claim_leader(&rd, PROFILE_RD_LEADER_NONE),
		      -EINVAL);
	zassert_equal(profile_rd_claim_leader(&rd, 0x2A2Bu), 0);
	zassert_equal(profile_rd_claim_leader(&rd, 0x2A2Bu), 0, "idempotent");
	zassert_equal(profile_rd_claim_leader(&rd, 0x0102u), -EBUSY,
		      "a sensor with a leader does not change leader on request");
}

ZTEST(profile_rd, test_step_count_wraps_at_128)
{
	struct profile_rd rd;

	rd_init_default(&rd, false);
	profile_rd_add_steps(&rd, 120u);
	zassert_equal(rd.m.step_count, 120u);
	profile_rd_add_steps(&rd, 10u);
	zassert_equal(rd.m.step_count, 2u, "7 bits, and it is MEANT to wrap");
}

ZTEST(profile_rd, test_back_channel_speed_is_applied)
{
	struct profile_rd rd;
	uint8_t body[8];

	rd_init_default(&rd, false);
	zassert_false(rd.have_leader_speed);

	zassert_equal(profile_rd_encode_speed(4u * 256u, body), 0);
	zassert_equal(profile_rd_apply_speed(&rd, body), 0);
	zassert_true(rd.have_leader_speed);
	zassert_equal(rd.leader_speed_256, 4u * 256u);

	zassert_equal(profile_rd_encode_speed(PROFILE_RD_SPEED_INVALID, body), 0);
	zassert_equal(profile_rd_apply_speed(&rd, body), 0);
	zassert_false(rd.have_leader_speed);
}

/* ── The sample-bus adapter ─────────────────────────────────────────────── */

ZTEST(profile_rd, test_adapter_publishes_the_six_mapped_metrics)
{
	struct radiant_rd_adapter a;
	struct profile_rd_metrics m = sample_metrics();
	uint8_t  body[8];
	uint32_t n;

	radiant_bridge_reset();
	radiant_rd_adapter_init(&a);

	/* The first page 0x00 establishes the step-count baseline and posts no
	 * step delta - the same first-call rule as the heart-rate adapter. */
	zassert_equal(profile_rd_encode_a(&m, false, body), 0);
	n = radiant_rd_adapter_decode(&a, 1u, body, 1000u);
	zassert_equal(n, 2u, "vertical oscillation and stance time");

	m.step_count = (uint8_t)(RD_STEPS + 5u);
	zassert_equal(profile_rd_encode_a(&m, false, body), 0);
	n = radiant_rd_adapter_decode(&a, 1u, body, 2000u);
	zassert_equal(n, 3u, "and now a step delta");

	zassert_equal(profile_rd_encode_b(&m, PROFILE_RD_LEADER_NONE, body), 0);
	n = radiant_rd_adapter_decode(&a, 1u, body, 3000u);
	zassert_equal(n, 3u, "balance, vertical ratio, step length");

	/* The two halves of the sensor's picture meet in the adapter, because
	 * the two pages share no field at all. */
	zassert_true(a.seen_a);
	zassert_true(a.seen_b);
	zassert_equal(a.m.gct_ms, RD_GCT_MS,
		      "ground contact time is decoded even though it is not "
		      "published - there is no vocabulary type for it yet");
	zassert_equal(a.m.cadence_32, RD_CADENCE_32);
}

ZTEST(profile_rd, test_adapter_does_not_publish_invalid_as_zero)
{
	struct radiant_rd_adapter a;
	struct profile_rd_metrics m;
	uint8_t body[8];

	radiant_bridge_reset();
	radiant_rd_adapter_init(&a);

	/* "No motion detected" and "zero millimetres" are different statements
	 * and a sink cannot tell them apart after the fact, so an invalid
	 * field is not posted at all. */
	memset(&m, 0, sizeof(m));
	zassert_equal(profile_rd_encode_a(&m, false, body), 0);
	zassert_equal(radiant_rd_adapter_decode(&a, 1u, body, 1000u), 0u);
}

ZTEST(profile_rd, test_adapter_ignores_the_displays_pages)
{
	struct radiant_rd_adapter a;
	uint8_t body[8];

	radiant_bridge_reset();
	radiant_rd_adapter_init(&a);

	/* 0x10, 0x20 and 0x4A are the display's half of the conversation
	 * rather than measurements, and none of them belongs on the bus. */
	zassert_equal(profile_rd_encode_speed(1024u, body), 0);
	zassert_equal(radiant_rd_adapter_decode(&a, 1u, body, 1000u), 0u);
	zassert_equal(profile_rd_encode_leader_request(7u, body), 0);
	zassert_equal(radiant_rd_adapter_decode(&a, 1u, body, 1000u), 0u);
	zassert_equal(profile_rd_encode_open_channel(1u, PROFILE_RD_RF_2439,
						     PROFILE_RD_PERIOD_HR_RD,
						     body), 0);
	zassert_equal(radiant_rd_adapter_decode(&a, 1u, body, 1000u), 0u);
}

ZTEST(profile_rd, test_adapter_step_delta_wraps_in_seven_bits)
{
	struct radiant_rd_adapter a;
	struct profile_rd_metrics m;
	uint8_t body[8];

	radiant_bridge_reset();
	radiant_rd_adapter_init(&a);

	memset(&m, 0, sizeof(m));
	m.vert_osc_quarter_mm = RD_VO_QUARTER; /* so something is publishable */
	m.step_count = 126u;
	zassert_equal(profile_rd_encode_a(&m, false, body), 0);
	zassert_equal(radiant_rd_adapter_decode(&a, 1u, body, 1000u), 1u);

	/* 126 -> 3 across the 7-bit rollover is five steps, not 133 and not a
	 * negative. A u8 subtraction alone would report 128 extra. */
	m.step_count = 3u;
	zassert_equal(profile_rd_encode_a(&m, false, body), 0);
	zassert_equal(radiant_rd_adapter_decode(&a, 1u, body, 2000u), 2u);
	zassert_equal(a.prev_step_count, 3u);
}

ZTEST_SUITE(profile_rd, NULL, NULL, NULL, NULL, NULL);
