/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * radiant_profile_hr_beats(): coalesced beats on ANT+ page 4.
 *
 * A node whose beat ring overflowed knows that n beats happened and only the
 * newest instant. Replaying radiant_profile_hr_beat() n times with that instant
 * set page 4's previous event time equal to the current one - a 0 ms R-R
 * interval on the air. radiant_profile_hr_beats() advances the count by n and
 * keeps prev at the last beat really delivered.
 */

#include <zephyr/ztest.h>
#include <string.h>

#include <radiant/radiant_profile_common.h>
#include <radiant/radiant_profile_hr.h>

static void cfg_fill(struct radiant_profile_hr_cfg *cfg)
{
	memset(cfg, 0, sizeof(*cfg));
	cfg->id.manufacturer_id = 0x00FFu;
	cfg->id.model_number = 1u;
	cfg->id.sw_revision_supplemental = RADIANT_PROFILE_COMMON_INVALID_U8;
	cfg->id.serial_number = RADIANT_PROFILE_COMMON_INVALID_U32;
}

ZTEST_SUITE(radiant_profile_hr_beats, NULL, NULL, NULL, NULL, NULL);

ZTEST(radiant_profile_hr_beats, test_coalesced_beats_keep_prev_at_last_delivered)
{
	struct radiant_profile_hr hr;
	struct radiant_profile_hr_cfg cfg;
	uint8_t page[8];

	cfg_fill(&cfg);
	zassert_equal(0, radiant_profile_hr_init(&hr, &cfg));

	radiant_profile_hr_beat(&hr, 1000u);
	radiant_profile_hr_beat(&hr, 1850u);
	zassert_equal(hr.beats, 2);

	/* Three beats, the newest at 4400: count +3, cur 4400, prev 1850. */
	radiant_profile_hr_beats(&hr, 3u, 4400u);
	zassert_equal(hr.beats, 5);
	zassert_equal(hr.event_time, 4400u);
	zassert_equal(hr.previous_event_time, 1850u, "prev is the last delivered beat");

	zassert_equal(0, radiant_profile_hr_encode_previous_beat(&hr, false, page));
	zassert_equal(page[2] | (page[3] << 8), 1850u);
	zassert_equal(page[4] | (page[5] << 8), 4400u);
	zassert_equal(page[6], 5u);

	/* n == 1 is one ordinary beat; n == 0 is nothing. */
	radiant_profile_hr_beats(&hr, 1u, 5250u);
	zassert_equal(hr.previous_event_time, 4400u);
	zassert_equal(hr.beats, 6);
	radiant_profile_hr_beats(&hr, 0u, 9999u);
	zassert_equal(hr.event_time, 5250u);
	zassert_equal(hr.beats, 6);

	/* The count wraps at 256 as the page's byte does. */
	radiant_profile_hr_beats(&hr, 300u, 6000u);
	zassert_equal(hr.beats, (uint8_t)(6u + 300u));
}
