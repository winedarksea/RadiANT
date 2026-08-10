/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: clean-room. The page layouts asserted against are the two
 * public ANT+ facts radiant_profile_sanity.h cites: bicycle power's
 * instantaneous power at payload[6..7] LE (tools/ant_pages.py's
 * decode_power_std, reused rather than re-derived) and a heart rate
 * monitor's computed heart rate at payload[7]. Nothing here derives from
 * sdk-ant, from libant.a, or from any adopter-gated ANT+ device profile
 * document.
 *
 * ---------------------------------------------------------------------------
 * The suite for radiant_core/src/radiant_profile_sanity.c
 * ---------------------------------------------------------------------------
 * The one property this module must never have is a false "implausible" on
 * data that could really come off a sensor: a track sprint's watts, a hard
 * effort's heart rate, a "not reported" sentinel, or any device type or page
 * this module has no business judging. Getting that wrong drops good data
 * silently, on live channels, and radiant_api.c only ever calls this on
 * REPAIRED frames - so a false positive here reads as an unrelated receiver
 * fault rather than as this feature. Every test below is really a boundary
 * test for exactly that reason: the values right at the edge of "still a
 * bicycle", "still a heart", or "still reported at all" matter far more than
 * the values deep inside "obviously wrong".
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_profile_sanity.h>

ZTEST_SUITE(radiant_profile_sanity, NULL, NULL, NULL, NULL, NULL);

#define BPWR RADIANT_PROFILE_SANITY_DEVTYPE_BPWR
#define HR   RADIANT_PROFILE_SANITY_DEVTYPE_HR

/* [0] page 0x10 [1] event [2] pedal power [3] cadence
 * [4..5] acc power LE [6..7] inst power LE. Only bytes 0, 6 and 7 are
 * asserted by the module under test; the rest are filler. */
static void power_body(uint8_t *body, uint16_t inst_watts)
{
	memset(body, 0, 8u);
	body[0] = RADIANT_PROFILE_SANITY_PAGE_POWER_STD;
	body[6] = (uint8_t)(inst_watts & 0xFFu);
	body[7] = (uint8_t)(inst_watts >> 8);
}

static void hr_body(uint8_t *body, uint8_t computed_hr, uint8_t page)
{
	memset(body, 0, 8u);
	body[0] = page;
	body[7] = computed_hr;
}

/* ---------------------------------------------------------------------------
 * Bicycle power
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_profile_sanity, test_a_realistic_power_reading_is_plausible)
{
	uint8_t body[8];

	power_body(body, 250u);
	zassert_false(radiant_profile_sanity_implausible(BPWR, body, 8u),
		      "250 W is an ordinary cyclist and must never be dropped");
}

ZTEST(radiant_profile_sanity, test_a_track_sprint_is_still_plausible)
{
	uint8_t body[8];

	/* Real elite sprint efforts exceed 2000 W. This is the whole reason
	 * the threshold is 3000 W and not the originally proposed 1000 W:
	 * a tighter threshold would drop exactly this reading. */
	power_body(body, 2000u);
	zassert_false(radiant_profile_sanity_implausible(BPWR, body, 8u),
		      "a 2000 W sprint is real and must survive");
}

ZTEST(radiant_profile_sanity, test_the_threshold_itself_is_still_plausible)
{
	uint8_t body[8];

	power_body(body, RADIANT_PROFILE_SANITY_BPWR_MAX_WATTS);
	zassert_false(radiant_profile_sanity_implausible(BPWR, body, 8u),
		      "the boundary value itself must not be rejected - only "
		      "values ABOVE it");
}

ZTEST(radiant_profile_sanity, test_one_watt_over_the_threshold_is_implausible)
{
	uint8_t body[8];

	power_body(body, RADIANT_PROFILE_SANITY_BPWR_MAX_WATTS + 1u);
	zassert_true(radiant_profile_sanity_implausible(BPWR, body, 8u),
		     "3001 W is not a bicycle");
}

ZTEST(radiant_profile_sanity, test_the_not_reported_power_sentinel_is_plausible)
{
	uint8_t body[8];

	power_body(body, 0xFFFFu);
	zassert_false(radiant_profile_sanity_implausible(BPWR, body, 8u),
		      "0xFFFF means \"not reported\", not 65535 W");
}

ZTEST(radiant_profile_sanity, test_a_power_page_this_module_does_not_check_is_plausible)
{
	uint8_t body[8];

	/* Torque pages are accumulators; a per-frame check cannot recover a
	 * watt figure from one without the previous frame. */
	memset(body, 0, sizeof(body));
	body[0] = 0x11u; /* wheel torque */
	body[6] = 0xFFu;
	body[7] = 0xFFu;
	zassert_false(radiant_profile_sanity_implausible(BPWR, body, 8u),
		      "this module has no opinion on a page it does not decode");
}

/* ---------------------------------------------------------------------------
 * Heart rate
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_profile_sanity, test_a_realistic_heart_rate_is_plausible)
{
	uint8_t body[8];

	hr_body(body, 150u, 0u);
	zassert_false(radiant_profile_sanity_implausible(HR, body, 8u),
		      "150 bpm is an ordinary hard effort");
}

ZTEST(radiant_profile_sanity, test_a_young_athletes_max_heart_rate_is_still_plausible)
{
	uint8_t body[8];

	/* Documented human maxima reach the low 220s. This is why the
	 * threshold is 240 bpm and not the originally proposed 200: a
	 * tighter threshold would drop exactly this reading. */
	hr_body(body, 220u, 0u);
	zassert_false(radiant_profile_sanity_implausible(HR, body, 8u),
		      "220 bpm is a real, if extreme, human heart rate");
}

ZTEST(radiant_profile_sanity, test_the_hr_threshold_itself_is_still_plausible)
{
	uint8_t body[8];

	hr_body(body, (uint8_t)RADIANT_PROFILE_SANITY_HR_MAX_BPM, 0u);
	zassert_false(radiant_profile_sanity_implausible(HR, body, 8u),
		      "the boundary value itself must not be rejected");
}

ZTEST(radiant_profile_sanity, test_one_bpm_over_the_threshold_is_implausible)
{
	uint8_t body[8];

	hr_body(body, (uint8_t)(RADIANT_PROFILE_SANITY_HR_MAX_BPM + 1u), 0u);
	zassert_true(radiant_profile_sanity_implausible(HR, body, 8u),
		     "241 bpm belongs to no heart");
}

ZTEST(radiant_profile_sanity, test_computed_heart_rate_is_checked_on_every_page_number)
{
	uint8_t body[8];

	/* Byte 7 is the computed heart rate on every HRM data page, not just
	 * page 0 - unlike the power check, there is no page-number gate. */
	hr_body(body, 250u, 4u);
	zassert_true(radiant_profile_sanity_implausible(HR, body, 8u),
		     "byte 7 is the computed heart rate regardless of page "
		     "number");
}

ZTEST(radiant_profile_sanity, test_the_not_reported_hr_sentinel_is_plausible)
{
	uint8_t body[8];

	hr_body(body, 0xFFu, 0u);
	zassert_false(radiant_profile_sanity_implausible(HR, body, 8u),
		      "0xFF means \"not reported\", not 255 bpm");
}

/* ---------------------------------------------------------------------------
 * Device types and payloads this module has no opinion on
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_profile_sanity, test_an_unknown_device_type_is_never_judged)
{
	uint8_t body[8];

	power_body(body, 0xFFFEu); /* absurd, but not a device type checked */
	zassert_false(radiant_profile_sanity_implausible(0x7Fu, body, 8u),
		      "a device type this module does not know must pass "
		      "through unexamined, not be judged by the wrong rule");
}

ZTEST(radiant_profile_sanity, test_the_pairing_bit_does_not_hide_the_device_type)
{
	uint8_t body[8];

	power_body(body, RADIANT_PROFILE_SANITY_BPWR_MAX_WATTS + 1u);
	zassert_true(radiant_profile_sanity_implausible(
			     BPWR | RADIANT_DEVICE_TYPE_PAIRING_BIT, body, 8u),
		     "the pairing bit must be masked off before the device "
		     "type is matched");
}

ZTEST(radiant_profile_sanity, test_a_short_payload_is_never_judged)
{
	uint8_t body[8];

	power_body(body, RADIANT_PROFILE_SANITY_BPWR_MAX_WATTS + 1u);
	zassert_false(radiant_profile_sanity_implausible(BPWR, body, 6u),
		      "a payload too short to hold the field being checked "
		      "must not be read out of bounds or judged on garbage");
	zassert_false(radiant_profile_sanity_implausible(HR, body, 6u),
		      "same, for heart rate's byte 7");
}

ZTEST(radiant_profile_sanity, test_a_null_payload_is_never_judged)
{
	zassert_false(radiant_profile_sanity_implausible(BPWR, NULL, 8u),
		      "a null payload must not be dereferenced");
}

ZTEST(radiant_profile_sanity, test_a_zero_length_payload_is_never_judged)
{
	uint8_t body[8] = { 0 };

	zassert_false(radiant_profile_sanity_implausible(BPWR, body, 0u),
		      "zero declared length means nothing to judge");
}
