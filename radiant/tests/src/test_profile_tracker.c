/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_profile_tracker.c - ANT+ Tracker 0x29: page tables and the master
 * rotation. Vectors mirror tools/test_ant_pages.py's TestTrackerPages.
 */

#include <zephyr/ztest.h>

#include <string.h>

#include "profile_tracker.h"

ZTEST(profile_tracker, test_location_1_bit_placement)
{
	struct profile_tracker_location loc;
	uint8_t body[8];
	uint8_t idx = 0xFFu;
	struct profile_tracker_location got;

	memset(&loc, 0, sizeof(loc));
	loc.distance_m = 1500u;
	loc.bearing_brad = 128u;
	loc.status = PROFILE_TRACKER_STATUS_LOW_BATTERY |
		     PROFILE_TRACKER_SITUATION_DOG_MOVING;
	loc.latitude_semi = (int32_t)0x1A6CFB08;

	zassert_equal(profile_tracker_encode_location_1(3u, &loc, body), 0);
	zassert_equal(body[0], PROFILE_TRACKER_PAGE_LOCATION_1);
	zassert_equal(body[1] & 0x1Fu, 3u);
	zassert_equal(body[1] & 0xE0u, 0xE0u, "reserved top 3 bits are 0x7");
	zassert_equal((uint16_t)(body[2] | (body[3] << 8)), 1500u);
	zassert_equal(body[4], 128u);
	zassert_equal(body[5], loc.status);
	zassert_equal((uint16_t)(body[6] | (body[7] << 8)), 0xFB08u,
		      "low 16 bits of latitude only");

	memset(&got, 0, sizeof(got));
	zassert_equal(profile_tracker_decode_location_1(body, &idx, &got), 0);
	zassert_equal(idx, 3u);
	zassert_equal(got.distance_m, 1500u);
	zassert_true(got.connected);
}

ZTEST(profile_tracker, test_location_split_across_two_pages)
{
	struct profile_tracker_location loc;
	uint8_t p1[8];
	uint8_t p2[8];
	struct profile_tracker_location got;

	memset(&loc, 0, sizeof(loc));
	loc.latitude_semi = (int32_t)0x1A6CFB08;
	loc.longitude_semi = -1424586524;

	zassert_equal(profile_tracker_encode_location_1(0u, &loc, p1), 0);
	zassert_equal(profile_tracker_encode_location_2(0u, &loc, p2), 0);

	/* Page 1 alone is a QUARTER of a position report: it names none of
	 * longitude and only the low half of latitude. */
	memset(&got, 0, sizeof(got));
	zassert_equal(profile_tracker_decode_location_1(p1, NULL, &got), 0);
	zassert_equal(got.longitude_semi, 0, "longitude not carried by page 1");

	zassert_equal(profile_tracker_decode_location_2(p2, NULL, &got), 0);
	zassert_equal(got.latitude_semi, (int32_t)0x1A6CFB08);
	zassert_equal(got.longitude_semi, -1424586524);
}

ZTEST(profile_tracker, test_no_assets_page)
{
	uint8_t body[8];
	int     i;

	zassert_equal(profile_tracker_encode_no_assets(body), 0);
	zassert_equal(body[0], PROFILE_TRACKER_PAGE_NO_ASSETS);
	for (i = 1; i < 8; i++) {
		zassert_equal(body[i], 0xFFu);
	}
}

ZTEST(profile_tracker, test_identity_pages_split_the_name)
{
	struct profile_tracker_ident id;
	uint8_t body1[8];
	uint8_t body2[8];
	struct profile_tracker_ident got;

	memset(&id, 0, sizeof(id));
	memcpy(id.name, "BuddyMax", 8);
	id.asset_type = PROFILE_TRACKER_ASSET_TYPE_DOG;
	id.colour = 0xE0u;

	zassert_equal(profile_tracker_encode_ident_1(5u, &id, body1), 0);
	zassert_equal(profile_tracker_encode_ident_2(5u, &id, body2), 0);
	zassert_equal(body1[0], PROFILE_TRACKER_PAGE_IDENT_1);
	zassert_equal(body2[0], PROFILE_TRACKER_PAGE_IDENT_2);
	zassert_mem_equal(&body1[3], "Buddy", 5);
	zassert_mem_equal(&body2[3], "Max\0\0", 5);

	memset(&got, 0, sizeof(got));
	zassert_equal(profile_tracker_decode_ident_1(body1, NULL, &got), 0);
	zassert_equal(profile_tracker_decode_ident_2(body2, NULL, &got), 0);
	zassert_mem_equal(got.name, "BuddyMax\0\0", 10);
	zassert_equal(got.asset_type, PROFILE_TRACKER_ASSET_TYPE_DOG);
}

ZTEST(profile_tracker, test_asset_index_out_of_range_is_refused)
{
	struct profile_tracker_location loc;
	uint8_t body[8];

	memset(&loc, 0, sizeof(loc));
	zassert_equal(profile_tracker_encode_location_1(32u, &loc, body),
		      -EINVAL);
}

ZTEST(profile_tracker, test_disconnect_page)
{
	uint8_t body[8];
	int     i;

	zassert_equal(profile_tracker_encode_disconnect(body), 0);
	zassert_equal(body[0], PROFILE_TRACKER_PAGE_DISCONNECT);
	for (i = 1; i < 8; i++) {
		zassert_equal(body[i], 0xFFu);
	}
}

ZTEST(profile_tracker, test_bradian_round_trip_at_field_resolution)
{
	uint8_t code = profile_tracker_degrees_to_bradians(60.0);

	/* The document's own equation 5 example rounds 42.67 up to 43. */
	zassert_equal(code, 0x2B);
	zassert_within(profile_tracker_bradians_to_degrees(code), 60.0,
		       360.0 / 256.0);
}

static void tracker_init_default(struct profile_tracker *t)
{
	struct profile_tracker_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id.manufacturer_id = 0x00FFu;
	cfg.id.model_number = 1u;
	cfg.id.sw_revision_supplemental = PROFILE_COMMON_INVALID_U8;
	cfg.id.serial_number = PROFILE_COMMON_INVALID_U32;

	zassert_equal(profile_tracker_init(t, &cfg), 0);
}

ZTEST(profile_tracker, test_no_assets_until_one_connects)
{
	struct profile_tracker t;
	uint8_t body[8];

	tracker_init_default(&t);
	zassert_equal(profile_tracker_next(&t, body), PROFILE_SLOT_DATA);
	zassert_equal(body[0], PROFILE_TRACKER_PAGE_NO_ASSETS);
}

ZTEST(profile_tracker, test_connect_queues_identity_then_location)
{
	struct profile_tracker          t;
	struct profile_tracker_location loc;
	struct profile_tracker_ident    id;
	uint8_t body[8];

	tracker_init_default(&t);
	memset(&loc, 0, sizeof(loc));
	memset(&id, 0, sizeof(id));

	zassert_equal(profile_tracker_connect(&t, 0u, &loc, &id), 0);

	/* Identity goes out first (16 then 17), then the ordinary location
	 * pair (1 then 2) - the document's "sent when an asset is added"
	 * rule, ahead of the periodic rotation. */
	zassert_equal(profile_tracker_next(&t, body), PROFILE_SLOT_DATA);
	zassert_equal(body[0], PROFILE_TRACKER_PAGE_IDENT_1);
	zassert_equal(profile_tracker_next(&t, body), PROFILE_SLOT_DATA);
	zassert_equal(body[0], PROFILE_TRACKER_PAGE_IDENT_2);
	zassert_equal(profile_tracker_next(&t, body), PROFILE_SLOT_DATA);
	zassert_equal(body[0], PROFILE_TRACKER_PAGE_LOCATION_1);
	zassert_equal(profile_tracker_next(&t, body), PROFILE_SLOT_DATA);
	zassert_equal(body[0], PROFILE_TRACKER_PAGE_LOCATION_2);
}

ZTEST(profile_tracker, test_disconnect_burst_is_five_messages)
{
	struct profile_tracker t;
	uint8_t body[8];
	int     i;

	tracker_init_default(&t);
	profile_tracker_begin_disconnect(&t);

	for (i = 0; i < 5; i++) {
		zassert_equal(profile_tracker_next(&t, body), PROFILE_SLOT_DATA);
		zassert_equal(body[0], PROFILE_TRACKER_PAGE_DISCONNECT);
	}
	/* The sixth slot is back to the ordinary rotation (no assets). */
	zassert_equal(profile_tracker_next(&t, body), PROFILE_SLOT_DATA);
	zassert_equal(body[0], PROFILE_TRACKER_PAGE_NO_ASSETS);
}

ZTEST_SUITE(profile_tracker, NULL, NULL, NULL, NULL, NULL);
