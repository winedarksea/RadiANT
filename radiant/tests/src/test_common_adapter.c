/*
 * SPDX-License-Identifier: Apache-2.0
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "profile_common.h"
#include "radiant_binding.h"
#include "radiant_bridge.h"
#include "radiant_common_adapter.h"

/* ---------------------------------------------------------------------------
 * Capture
 * ---------------------------------------------------------------------------
 */

#define COMMON_CAP_MAX 16

static struct radiant_sample ccap_buf[COMMON_CAP_MAX];
static uint32_t              ccap_n;

static bool ccap_want(const struct radiant_sample *s)
{
	ARG_UNUSED(s);
	return true;
}

static void ccap_publish(const struct radiant_sample *s)
{
	if (ccap_n < COMMON_CAP_MAX) {
		ccap_buf[ccap_n++] = *s;
	}
}

RADIANT_SINK_DEFINE(test_common_sink, ccap_want, ccap_publish, NULL);

/* The captured sample for a field_id, or NULL. Looked up rather than indexed:
 * this application links several sinks and several suites, and asserting on
 * "the third sample drained" would couple this suite to the order the OTHER
 * producers happen to run in. */
static const struct radiant_sample *ccap_find(uint8_t field_id)
{
	uint32_t i;

	for (i = 0u; i < ccap_n; i++) {
		if (ccap_buf[i].field_id == field_id) {
			return &ccap_buf[i];
		}
	}
	return NULL;
}

/*
 * Look up a required sample, or fail the test and STOP.
 *
 * The early return is the point, and it is not defensive padding: a zassert
 * does not unwind its enclosing function when CONFIG_MULTITHREADING=y - the
 * `return;` inside ztest_assert.h's _zassert_base is compiled out in that
 * configuration, which is every configuration this suite runs in, because the
 * runtime is a real 32-bit ARM board. So `zassert_not_null(s); ... s->raw`
 * turns a legible one-line assertion failure into a null dereference and a
 * crash dump. The macro hides a `return`, which is normally objectionable; it
 * is spelled in capitals for exactly that reason.
 */
#define CCAP_REQUIRE(_s, _field_id, _why)                                     \
	do {                                                                  \
		(_s) = ccap_find(_field_id);                                  \
		zassert_not_null((_s), _why);                                 \
		if ((_s) == NULL) {                                           \
			return;                                               \
		}                                                             \
	} while (0)

static void common_reset(void *unused)
{
	ARG_UNUSED(unused);
	radiant_bridge_reset();
	radiant_binding_init();
	memset(ccap_buf, 0, sizeof(ccap_buf));
	ccap_n = 0u;
}

ZTEST_SUITE(radiant_common_adapter, NULL, NULL, common_reset, NULL, NULL);

/* Builds one page-84 body. Bytes [4..5] and [6..7] are written little-endian
 * here rather than taken as bytes, so a test reads as the NUMBER the spec
 * table states and the endianness lives in exactly one place. */
static void page84(uint8_t *body, uint8_t sub1, uint16_t v1, uint8_t sub2,
		   uint16_t v2)
{
	body[0] = PROFILE_COMMON_PAGE_84;
	body[1] = 0xFFu; /* reserved */
	body[2] = sub1;
	body[3] = sub2;
	body[4] = (uint8_t)(v1 & 0xFFu);
	body[5] = (uint8_t)(v1 >> 8);
	body[6] = (uint8_t)(v2 & 0xFFu);
	body[7] = (uint8_t)(v2 >> 8);
}

/* ---------------------------------------------------------------------------
 * The codec: section 6.13.1's worked example
 * ---------------------------------------------------------------------------
 */

/* ANT Message Payload = [54][FF][01][03][6B][0A][EA][19] - Figure 6-8, copied
 * byte for byte from the document rather than assembled by page84(), because
 * the point of a golden vector is that it is the document's bytes. */
static const uint8_t golden[8] = {
	0x54u, 0xFFu, 0x01u, 0x03u, 0x6Bu, 0x0Au, 0xEAu, 0x19u,
};

ZTEST(radiant_common_adapter, test_golden_vector_decodes_to_its_two_subfields)
{
	struct profile_common_84 d;

	zassert_equal(profile_common_decode_84(golden, &d), 0, NULL);

	zassert_equal(d.slot[0].subpage, PROFILE_COMMON_SUBPAGE_TEMPERATURE,
		      "section 6.13.1: the first subfield is temperature");
	zassert_equal(d.slot[0].raw, PROFILE_COMMON_84_GOLDEN_TEMP_CENTI,
		      "0x0A6B = 2667 = 26.67 degC, little-endian");

	zassert_equal(d.slot[1].subpage, PROFILE_COMMON_SUBPAGE_HUMIDITY,
		      "section 6.13.1: the second subfield is humidity");
	zassert_equal(d.slot[1].raw, PROFILE_COMMON_84_GOLDEN_HUMID_CENTI,
		      "0x19EA = 6634 = 66.34 percent, little-endian");
}

ZTEST(radiant_common_adapter, test_decoder_rejects_a_body_that_is_not_page_84)
{
	struct profile_common_84 d;
	uint8_t body[8];

	memcpy(body, golden, sizeof(body));
	body[0] = PROFILE_COMMON_PAGE_80;

	/* -EINVAL, not "decode it anyway": the ADAPTER filters the common
	 * range, and a decoder that quietly accepted page 80 would make the
	 * absence of that filter invisible. */
	zassert_equal(profile_common_decode_84(body, &d), -EINVAL, NULL);
	zassert_equal(profile_common_decode_84(NULL, &d), -EINVAL, NULL);
	zassert_equal(profile_common_decode_84(golden, NULL), -EINVAL, NULL);
}

ZTEST(radiant_common_adapter, test_golden_vector_end_to_end_on_the_bus)
{
	struct radiant_common_adapter a;
	const struct radiant_sample *s;

	radiant_common_adapter_init(&a);
	zassert_equal(radiant_common_adapter_decode(&a, 3u, golden, 1234u), 2u,
		      "both slots of the worked example are understood");
	radiant_bridge_drain();

	/* Temperature: EXACT. 26.67 degC is 299.82 K, and the vocabulary's
	 * unit is kelvin, so raw = 2667 + 27315 at exp -2 with no rounding
	 * anywhere. An assertion on 29982 is an assertion that nothing in the
	 * path did floating point. */
	CCAP_REQUIRE(s, RADIANT_COMMON_FIELD_TEMPERATURE,
		     "subpage 1 posts a temperature");
	zassert_equal(s->field_type, RADIANT_FIELD_TEMPERATURE, NULL);
	zassert_equal(s->exp, -2, NULL);
	zassert_equal(s->raw, 29982, "26.67 degC = 299.82 K, exactly");
	zassert_equal(s->source, 3u, NULL);
	zassert_equal(s->t_us, 1234u, NULL);
	zassert_equal(s->flags, 0u, "instantaneous, not an accumulator");

	/* Humidity: EXACT. 0.01 % is exp -2 on the wire value itself. */
	CCAP_REQUIRE(s, RADIANT_COMMON_FIELD_HUMIDITY,
		     "subpage 3 posts a humidity");
	zassert_equal(s->field_type, RADIANT_FIELD_HUMIDITY, NULL);
	zassert_equal(s->exp, -2, NULL);
	zassert_equal(s->raw, 6634, "66.34 %%, exactly");
	zassert_equal(s->flags, 0u, NULL);
}

/* ---------------------------------------------------------------------------
 * THE ARCHITECTURAL TEST - the point of the whole package
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_common_adapter, test_page_84_on_a_power_meter_binding_gives_humidity)
{
	struct radiant_common_adapter a;
	const struct radiant_binding *b;
	const struct radiant_sample *s;
	uint32_t source = RADIANT_BINDING_NONE;
	uint8_t body[8];

	/*
	 * Device type 0x0B is BICYCLE POWER. It is not an environment sensor,
	 * it has no environmental page of its own, and before this package a
	 * page-84 broadcast on its channel was dropped by rx_tap.c's
	 * `if (b->devtype == 0x78)` before any decoder saw it.
	 *
	 * Table 4-1 says the 0x40-0x5D range is keyed on the TRANSMISSION TYPE
	 * of the channel, not the device type, so a power meter carrying
	 * weather data is a conforming sensor and this is the specified case
	 * rather than an exotic one.
	 */
	zassert_equal(radiant_binding_bind(0x1234u, 0x0Bu, 0x05u, "power",
					   &source), 0, NULL);
	b = radiant_binding_get(source);
	zassert_not_null(b, NULL);
	zassert_equal(b->devtype, 0x0Bu,
		      "the binding really is a power meter, not an env sensor");

	radiant_common_adapter_init(&a);

	/* Slot 1 humidity, slot 2 empty. Nothing about this call mentions the
	 * device type - that is the property under test. */
	page84(body, PROFILE_COMMON_SUBPAGE_HUMIDITY, 5000u,
	       PROFILE_COMMON_SUBPAGE_INVALID, 0u);
	zassert_equal(radiant_common_adapter_decode(&a, source, body, 77u), 1u,
		      NULL);
	radiant_bridge_drain();

	CCAP_REQUIRE(s, RADIANT_COMMON_FIELD_HUMIDITY,
		     "a power meter's page 84 must still produce humidity");
	zassert_equal(s->field_type, RADIANT_FIELD_HUMIDITY, NULL);
	zassert_equal(s->raw, 5000, "50.00 percent");
	zassert_equal(s->source, source, NULL);

	/* And the field_id came out of the reserved common block, so it cannot
	 * collide with whatever the power adapter writes onto the same source -
	 * the two-producers-one-source case radiant_bridge.h describes. */
	zassert_true(s->field_id >= RADIANT_FIELD_ID_COMMON_BASE &&
		     s->field_id <= RADIANT_FIELD_ID_COMMON_MAX,
		     "common fields live in 0x20-0x3F, clear of every profile");
}

/* ---------------------------------------------------------------------------
 * Declining is a normal answer
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_common_adapter, test_invalid_sentinel_in_slot_one_leaves_slot_two)
{
	struct radiant_common_adapter a;
	uint8_t body[8];

	radiant_common_adapter_init(&a);
	page84(body, PROFILE_COMMON_SUBPAGE_INVALID, 0xDEADu,
	       PROFILE_COMMON_SUBPAGE_HUMIDITY, 6634u);

	zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 1u), 1u,
		      "one slot declined, the other still decodes");
	radiant_bridge_drain();
	zassert_not_null(ccap_find(RADIANT_COMMON_FIELD_HUMIDITY), NULL);
	zassert_equal(a.declined, 1u, NULL);
	zassert_false(a.have_last[PROFILE_COMMON_SUBPAGE_TEMPERATURE],
		      "a declined slot records nothing, whatever its data bytes");
}

ZTEST(radiant_common_adapter, test_invalid_sentinel_in_slot_two_leaves_slot_one)
{
	struct radiant_common_adapter a;
	uint8_t body[8];

	radiant_common_adapter_init(&a);
	page84(body, PROFILE_COMMON_SUBPAGE_HUMIDITY, 6634u,
	       PROFILE_COMMON_SUBPAGE_INVALID, 0xDEADu);

	zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 1u), 1u, NULL);
	radiant_bridge_drain();
	zassert_not_null(ccap_find(RADIANT_COMMON_FIELD_HUMIDITY), NULL);
	zassert_equal(a.declined, 1u, NULL);
}

ZTEST(radiant_common_adapter, test_reserved_subpage_is_declined_without_error)
{
	struct radiant_common_adapter a;
	uint8_t body[8];

	/* Subpage 9 is the first of Table 6-17's "Reserved for future use"
	 * range. A sensor sending it is behaving, so this is a decline and a
	 * counter bump - not a rejected page, and not a log line at 4 Hz. */
	radiant_common_adapter_init(&a);
	page84(body, PROFILE_COMMON_SUBPAGE_RESERVED_FIRST, 0x1234u,
	       PROFILE_COMMON_SUBPAGE_TEMPERATURE, 2667u);

	zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 1u), 1u,
		      "the reserved slot declines, the temperature still posts");
	radiant_bridge_drain();
	zassert_not_null(ccap_find(RADIANT_COMMON_FIELD_TEMPERATURE), NULL);
	zassert_equal(a.declined, 1u, NULL);

	/* And the top of the reserved range, 254, which is the last legal
	 * subpage VALUE and still not one this decoder posts for. */
	radiant_common_adapter_init(&a);
	page84(body, PROFILE_COMMON_SUBPAGE_RESERVED_LAST, 0u,
	       PROFILE_COMMON_SUBPAGE_RESERVED_LAST, 0u);
	zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 1u), 0u, NULL);
	zassert_equal(a.declined, 2u, NULL);
}

ZTEST(radiant_common_adapter, test_other_common_pages_post_nothing)
{
	struct radiant_common_adapter a;
	uint8_t body[8];
	uint8_t page;

	/* Every common page except 84 is identity, battery, time, memory,
	 * pairing, errors or the request/command surface - none of them a
	 * measurement. Posting nothing is the answer, not a gap. Swept across
	 * the whole range so a future page number added to the middle of it
	 * cannot start decoding as page 84 by accident. */
	radiant_common_adapter_init(&a);
	for (page = PROFILE_COMMON_PAGE_RANGE_FIRST;
	     page <= PROFILE_COMMON_PAGE_RANGE_LAST; page++) {
		if (page == PROFILE_COMMON_PAGE_84) {
			continue;
		}
		memset(body, 0xFFu, sizeof(body));
		body[0] = page;
		zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 1u),
			      0u, "page 0x%02x posted a sample", page);
	}

	zassert_equal(radiant_common_adapter_decode(&a, 0u, NULL, 1u), 0u, NULL);
	zassert_equal(radiant_common_adapter_decode(NULL, 0u, golden, 1u), 0u,
		      NULL);
}

/* ---------------------------------------------------------------------------
 * The range test, at all four boundaries
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_common_adapter, test_common_page_range_boundaries)
{
	/* Table 4-1: 0x00-0x3F is device-type specific, 0x40-0x5D is the common
	 * range, 0x5E-0x6F is reserved. Both ends are inclusive and both
	 * off-by-ones are silent: one page low and every profile's page 0x3F
	 * stops reaching its own decoder; one page high and rx_tap.c hands a
	 * reserved page to the common path on every channel. */
	zassert_false(radiant_common_adapter_is_common_page(0x3Fu),
		      "0x3F is the last ANT+ device-type-specific page");
	zassert_true(radiant_common_adapter_is_common_page(0x40u),
		     "0x40 (64) is the first common page");
	zassert_true(radiant_common_adapter_is_common_page(0x5Du),
		     "0x5D (93) is the last common page");
	zassert_false(radiant_common_adapter_is_common_page(0x5Eu),
		      "0x5E (94) is the first reserved page");

	/* Spot checks well outside, so a broken comparison direction cannot
	 * pass on the four boundaries alone. */
	zassert_false(radiant_common_adapter_is_common_page(0x00u), NULL);
	zassert_true(radiant_common_adapter_is_common_page(0x54u), NULL);
	zassert_false(radiant_common_adapter_is_common_page(0xFFu), NULL);
}

/* ---------------------------------------------------------------------------
 * The exact conversions
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_common_adapter, test_temperature_is_twos_complement_into_kelvin)
{
	struct radiant_common_adapter a;
	const struct radiant_sample *s;
	uint8_t body[8];

	/* -5.00 degC on the wire is 0xFE0C. Read unsigned it is 65036, which
	 * would publish as +377.51 K - a number that looks like a plausible
	 * oven and not like a bug. */
	radiant_common_adapter_init(&a);
	page84(body, PROFILE_COMMON_SUBPAGE_TEMPERATURE, 0xFE0Cu,
	       PROFILE_COMMON_SUBPAGE_INVALID, 0u);
	zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 1u), 1u, NULL);
	radiant_bridge_drain();

	CCAP_REQUIRE(s, RADIANT_COMMON_FIELD_TEMPERATURE, NULL);
	zassert_equal(s->raw, 26815, "-5.00 degC = 268.15 K");
}

ZTEST(radiant_common_adapter, test_min_and_max_operating_temperature_ids)
{
	struct radiant_common_adapter a;
	const struct radiant_sample *s;
	uint8_t body[8];

	/*
	 * Subpages 7 and 8, on their NAMES not their descriptions - Table 6-17
	 * transposes the two description cells (see profile_common.h defect
	 * (a)). Pinned here so a reader who meets the descriptions first cannot
	 * quietly swap the two field ids back, which would silently relabel
	 * every stored series a sink already holds.
	 */
	radiant_common_adapter_init(&a);
	page84(body, PROFILE_COMMON_SUBPAGE_TEMP_MIN, 0xF830u /* -20.00 degC */,
	       PROFILE_COMMON_SUBPAGE_TEMP_MAX, 6000u /* +60.00 degC */);
	zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 1u), 2u, NULL);
	radiant_bridge_drain();

	CCAP_REQUIRE(s, RADIANT_COMMON_FIELD_TEMP_MIN, NULL);
	zassert_equal(s->field_type, RADIANT_FIELD_TEMPERATURE, NULL);
	zassert_equal(s->raw, 25315, "-20.00 degC = 253.15 K");

	CCAP_REQUIRE(s, RADIANT_COMMON_FIELD_TEMP_MAX, NULL);
	zassert_equal(s->raw, 33315, "+60.00 degC = 333.15 K");
}

ZTEST(radiant_common_adapter, test_pressure_is_exact_at_exp_plus_one)
{
	struct radiant_common_adapter a;
	const struct radiant_sample *s;
	uint8_t body[8];

	/* 101.32 kPa arrives as 10132 in 0.01 kPa units. 0.01 kPa is exactly
	 * 10 Pa, so the vocabulary value is 10132 x 10^1 Pa = 101320 Pa with no
	 * multiply and nothing to round. */
	radiant_common_adapter_init(&a);
	page84(body, PROFILE_COMMON_SUBPAGE_PRESSURE, 10132u,
	       PROFILE_COMMON_SUBPAGE_INVALID, 0u);
	zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 1u), 1u, NULL);
	radiant_bridge_drain();

	CCAP_REQUIRE(s, RADIANT_COMMON_FIELD_PRESSURE, NULL);
	zassert_equal(s->field_type, RADIANT_FIELD_PRESSURE, NULL);
	zassert_equal(s->exp, 1, "0.01 kPa = 10 Pa is an exponent, not a multiply");
	zassert_equal(s->raw, 10132, NULL);

	/* Table 6-17's stated maximum, 655.35 kPa, at the full u16. */
	radiant_common_adapter_init(&a);
	radiant_bridge_reset();
	ccap_n = 0u;
	page84(body, PROFILE_COMMON_SUBPAGE_PRESSURE, 0xFFFFu,
	       PROFILE_COMMON_SUBPAGE_INVALID, 0u);
	(void)radiant_common_adapter_decode(&a, 0u, body, 1u);
	radiant_bridge_drain();
	CCAP_REQUIRE(s, RADIANT_COMMON_FIELD_PRESSURE, NULL);
	zassert_equal(s->raw, 65535, "655.35 kPa = 655350 Pa at exp 1");
}

/* ---------------------------------------------------------------------------
 * The two lossy conversions - hand-computed expected raws
 * ---------------------------------------------------------------------------
 */

/*
 * Decodes one wind subpage and checks the published raw against a
 * hand-computed expectation.
 *
 * Void with the expectation passed in, rather than returning the raw for the
 * caller to assert on, because a zassert does NOT unwind the enclosing
 * function under CONFIG_MULTITHREADING=y (ztest_assert.h's _zassert_base
 * compiles the `return;` out) - so a helper that asserted non-null and then
 * dereferenced would carry on and fault on the failure path, turning a legible
 * assertion into a crash dump.
 */
static void wind_check(uint8_t subpage, uint8_t field_id, uint8_t field_type,
		       uint16_t wire, int64_t expect)
{
	struct radiant_common_adapter a;
	const struct radiant_sample *s;
	uint8_t body[8];

	radiant_bridge_reset();
	ccap_n = 0u;
	radiant_common_adapter_init(&a);
	page84(body, subpage, wire, PROFILE_COMMON_SUBPAGE_INVALID, 0u);
	(void)radiant_common_adapter_decode(&a, 0u, body, 1u);
	radiant_bridge_drain();

	s = ccap_find(field_id);
	zassert_not_null(s, "subpage %u must publish", subpage);
	if (s == NULL) {
		return;
	}
	zassert_equal(s->field_type, field_type, NULL);
	zassert_equal(s->exp, -6, NULL);
	zassert_equal(s->flags, 0u,
		      "instantaneous - the error does not compound, so section "
		      "3.2's accumulator rule does not apply here");
	zassert_equal(s->raw, expect, "wire %u expected raw %lld, got %lld",
		      wire, (long long)expect, (long long)s->raw);
}

static void wind_speed_check(uint16_t wire, int64_t expect)
{
	wind_check(PROFILE_COMMON_SUBPAGE_WIND_SPEED,
		   RADIANT_COMMON_FIELD_WIND_SPEED, RADIANT_FIELD_SPEED, wire,
		   expect);
}

static void wind_dir_check(uint16_t wire, int64_t expect)
{
	wind_check(PROFILE_COMMON_SUBPAGE_WIND_DIRECTION,
		   RADIANT_COMMON_FIELD_WIND_DIRECTION, RADIANT_FIELD_ANGLE,
		   wire, expect);
}

ZTEST(radiant_common_adapter, test_wind_speed_is_rounded_half_up_at_exp_minus_six)
{
	/*
	 * 0.01 km/h = 10/3600 m/s = 1/360 m/s. 10^k/360 is never an integer
	 * (360 has a factor of 3, 10^k has none), so the conversion is lossy at
	 * every exponent and the implementation rounds half up:
	 *     raw = (v * 1000000 + 180) / 360
	 *
	 * Every expected value below is the exact quotient, hand-computed:
	 *   v=1     1000000/360     = 2777.777...  -> 2778 (up)
	 *   v=2     2000000/360     = 5555.555...  -> 5556 (up)   nearest above 1/2
	 *   v=7     7000000/360     = 19444.444... -> 19444 (down) nearest below 1/2
	 *   v=9     9000000/360     = 25000        -> 25000 (exact; 9/360 = 1/40)
	 *   v=65535 65535000000/360 = 182041666.66 -> 182041667 (up), the full u16
	 *
	 * v=2 and v=7 are as close to a half-step as this expression can get.
	 * An EXACT half is unreachable: it needs v*10^6 = 180 (mod 360), and
	 * 10^6 = 280 (mod 360) with gcd(280,360) = 40, which does not divide
	 * 180. So the pair either side is the strongest statement available -
	 * a test asserting an exact .5 case would be asserting about an input
	 * that cannot occur.
	 */
	wind_speed_check(0u, 0);       /* zero wind is zero, not a rounding */
	wind_speed_check(1u, 2778);
	wind_speed_check(2u, 5556);    /* fraction 0.5555, rounds up */
	wind_speed_check(7u, 19444);   /* fraction 0.4444, rounds down */
	wind_speed_check(9u, 25000);   /* 9/360 = 1/40, exact */
	wind_speed_check(65535u, 182041667); /* 655.35 km/h = 182.041667 m/s */
}

ZTEST(radiant_common_adapter, test_wind_direction_is_rounded_half_up_at_exp_minus_six)
{
	/*
	 * 0.05 deg = 0.05 * pi/180 rad = 872.66462...e-6 rad. Irrational, so no
	 * (raw, exp) pair is exact; the implementation uses the 6-digit
	 * truncation 872665e-9 and rounds half up:
	 *     raw = (v * 872665 + 500000) / 1000000
	 *
	 * Hand-computed, exact products over 10^6:
	 *   v=1     872665/10^6     = 0.872665    -> 1
	 *   v=1017  887500305/10^6  = 887.500305  -> 888  (just above 1/2, up)
	 *   v=1606  1401499990/10^6 = 1401.499990 -> 1401 (just below 1/2, down)
	 *   v=7199  6282315335/10^6 = 6282.315335 -> 6282, Table 6-17's stated
	 *                                            maximum (359.95 degrees)
	 *
	 * v=1017 and v=1606 are the two values in the whole legal range whose
	 * fractional part lies closest to a half - 0.500305 and 0.499990 - so
	 * they are what pins the rounding direction. An exact half-step is
	 * again unreachable: the smallest solution of v*872665 = 500000
	 * (mod 10^6) is v = 100000, which does not fit the u16 the wire
	 * carries.
	 */
	wind_dir_check(0u, 0);       /* north is zero, not a rounding */
	wind_dir_check(1u, 1);
	wind_dir_check(1017u, 888);  /* 887.500305, rounds up */
	wind_dir_check(1606u, 1401); /* 1401.499990, rounds down */
	wind_dir_check(7199u, 6282); /* 359.95 deg = 6.282315 rad, the maximum */
}

/* ---------------------------------------------------------------------------
 * Subpage 6: the one accumulator on this page
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_common_adapter, test_charging_cycles_difference_at_u16_wire_width)
{
	struct radiant_common_adapter a;
	const struct radiant_sample *s;
	uint8_t body[8];

	radiant_common_adapter_init(&a);

	/* First sighting establishes prev and posts nothing: the wire carries a
	 * cumulative TOTAL, so posting it would put one enormous fake delta at
	 * the head of every sink's series. */
	page84(body, PROFILE_COMMON_SUBPAGE_CHARGE_CYCLES, 65530u,
	       PROFILE_COMMON_SUBPAGE_INVALID, 0u);
	zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 1u), 0u,
		      "nothing to difference against yet");
	radiant_bridge_drain();
	zassert_is_null(ccap_find(RADIANT_COMMON_FIELD_CHARGE_CYCLES), NULL);

	/* An ordinary step. */
	page84(body, PROFILE_COMMON_SUBPAGE_CHARGE_CYCLES, 65533u,
	       PROFILE_COMMON_SUBPAGE_INVALID, 0u);
	zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 2u), 1u, NULL);
	radiant_bridge_drain();
	CCAP_REQUIRE(s, RADIANT_COMMON_FIELD_CHARGE_CYCLES, NULL);
	zassert_equal(s->field_type, RADIANT_FIELD_EVENT_COUNT, NULL);
	zassert_equal(s->flags, RADIANT_SAMPLE_ACCUMULATING, NULL);
	zassert_equal(s->exp, 0, NULL);
	zassert_equal(s->raw, 3, NULL);

	/*
	 * And across the u16 rollover: 65533 -> 4 is 7 more charges, not 65529
	 * fewer. Differenced at the field's own wire width, where the unsigned
	 * subtraction wraps correctly by construction (section 3.2/5) - the
	 * same rule radiant_hr_adapter.c applies to its beat count, and the
	 * only part of section 3.2 that DOES apply to page 84.
	 *
	 * WHAT IS PUBLISHED IS THE RUNNING TOTAL, NOT THE STEP, so the wrap is
	 * asserted as a step OF the total rather than as the posted value: 3
	 * above, then 3 + 7 = 10 here. RADIANT_SAMPLE_ACCUMULATING is defined
	 * in radiant_bridge.h as "raw is a monotone counter, not an instant"
	 * and radiant_rules.c differences what it receives, so an adapter that
	 * posted the step would be handing it an already-differenced value.
	 * Checking the difference of the two totals is the stronger test
	 * anyway: it fails both if the wrap is taken at the wrong width AND if
	 * the accumulator is reset or double-counted between messages.
	 */
	ccap_n = 0u;
	page84(body, PROFILE_COMMON_SUBPAGE_CHARGE_CYCLES, 4u,
	       PROFILE_COMMON_SUBPAGE_INVALID, 0u);
	zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 3u), 1u, NULL);
	radiant_bridge_drain();
	CCAP_REQUIRE(s, RADIANT_COMMON_FIELD_CHARGE_CYCLES, NULL);
	zassert_equal(s->raw, 10,
		      "65533 -> 4 wraps to +7 on top of the running total of 3, "
		      "not -65529 and not a bare 7");
}

/* ---------------------------------------------------------------------------
 * Adapter state
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_common_adapter, test_last_seen_is_recorded_but_never_suppresses)
{
	struct radiant_common_adapter a;
	uint8_t body[8];

	/*
	 * The adapter keeps last-seen values for the sink-side deadband and
	 * does NOT deadband on them itself. Suppressing an unchanged sample
	 * here would suppress the ARRIVAL, and radiant_liveness.c decides a
	 * source has gone quiet from arrivals - a thermometer in a stable room
	 * would be declared STALE for being accurate.
	 */
	radiant_common_adapter_init(&a);
	zassert_false(a.have_last[PROFILE_COMMON_SUBPAGE_TEMPERATURE], NULL);

	page84(body, PROFILE_COMMON_SUBPAGE_TEMPERATURE, 2667u,
	       PROFILE_COMMON_SUBPAGE_INVALID, 0u);
	zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 1u), 1u, NULL);
	zassert_true(a.have_last[PROFILE_COMMON_SUBPAGE_TEMPERATURE], NULL);
	zassert_equal(a.last_raw[PROFILE_COMMON_SUBPAGE_TEMPERATURE], 2667u,
		      NULL);

	zassert_equal(radiant_common_adapter_decode(&a, 0u, body, 2u), 1u,
		      "an identical reading posts again - the deadband lives in "
		      "the Matter plane, not here");
}

ZTEST(radiant_common_adapter, test_init_clears_every_field)
{
	struct radiant_common_adapter a;
	uint8_t body[8];

	memset(&a, 0xAA, sizeof(a));
	radiant_common_adapter_init(&a);
	zassert_false(a.have_prev_cycles, NULL);
	zassert_equal(a.declined, 0u, NULL);
	zassert_false(a.have_last[PROFILE_COMMON_SUBPAGE_HUMIDITY], NULL);

	/* The state belongs to the SENSOR, not the channel (rx_tap.c), so
	 * re-init is what a re-bind does and it must forget the accumulator -
	 * otherwise a re-paired device's first delta is measured against a
	 * total from before it left. */
	page84(body, PROFILE_COMMON_SUBPAGE_CHARGE_CYCLES, 100u,
	       PROFILE_COMMON_SUBPAGE_INVALID, 0u);
	(void)radiant_common_adapter_decode(&a, 0u, body, 1u);
	zassert_true(a.have_prev_cycles, NULL);
	radiant_common_adapter_init(&a);
	zassert_false(a.have_prev_cycles, NULL);

	radiant_common_adapter_init(NULL); /* must not fault */
}
