/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * P7 - docs/radiant-bridge.md section 8.1's type map and endpoint model.
 *
 * Every test here is a line of section 8.1 or 8.1a made executable. The
 * conversions are the point: a wrong `mul` shows a plausible number in a
 * controller and nothing anywhere reports an error, which is the failure mode
 * this suite exists to make impossible.
 */

#include <zephyr/ztest.h>
#include <string.h>

#include "radiant_bridge.h"
#include "radiant_matter.h"

/* ---------------------------------------------------------------------------
 * The seam, captured
 * ---------------------------------------------------------------------------
 */

static struct {
	uint16_t endpoint_id;
	uint32_t cluster;
	uint32_t attribute;
	int64_t  value;
	uint32_t writes;
} last;

void radiant_matter_attr_write(uint16_t endpoint_id, uint32_t cluster,
			       uint32_t attribute, int64_t value)
{
	last.endpoint_id = endpoint_id;
	last.cluster = cluster;
	last.attribute = attribute;
	last.value = value;
	last.writes++;
}

static void matter_before(void *f)
{
	ARG_UNUSED(f);
	memset(&last, 0, sizeof(last));
	radiant_matter_reset();
}

ZTEST_SUITE(radiant_matter, NULL, NULL, matter_before, NULL, NULL);

static struct radiant_sample mk(uint32_t source, uint8_t field_id,
				uint8_t field_type, int64_t raw, int8_t exp)
{
	struct radiant_sample s;

	memset(&s, 0, sizeof(s));
	s.source = source;
	s.field_id = field_id;
	s.field_type = field_type;
	s.raw = raw;
	s.exp = exp;
	s.t_us = 1000u;
	return s;
}

/* ---------------------------------------------------------------------------
 * The table itself
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_matter, test_the_four_rows_section_8_1_lists_are_all_present)
{
	const struct radiant_matter_type_map *r;

	r = radiant_matter_row(RADIANT_FIELD_OCCUPANCY);
	zassert_not_null(r, "0x02 occupancy has a row in section 8.1");
	zassert_equal(r->device_type, MATTER_DEVTYPE_OCCUPANCY_SENSOR, NULL);
	zassert_equal(r->cluster, MATTER_CLUSTER_OCCUPANCY_SENSING, NULL);

	r = radiant_matter_row(RADIANT_FIELD_TEMPERATURE);
	zassert_not_null(r, "0x10 temperature has a row");
	zassert_equal(r->device_type, MATTER_DEVTYPE_TEMPERATURE_SENSOR, NULL);
	zassert_equal(r->cluster, MATTER_CLUSTER_TEMP_MEASUREMENT, NULL);

	r = radiant_matter_row(RADIANT_FIELD_HUMIDITY);
	zassert_not_null(r, "0x11 relative humidity has a row");
	zassert_equal(r->device_type, MATTER_DEVTYPE_HUMIDITY_SENSOR, NULL);
	zassert_equal(r->cluster, MATTER_CLUSTER_REL_HUMIDITY, NULL);

	r = radiant_matter_row(RADIANT_FIELD_BATTERY_SOC);
	zassert_not_null(r, "0x25 battery state of charge has a row");
	zassert_equal(r->device_type, 0u,
		      "section 8.1: battery is cluster only, on the binding's "
		      "own endpoint - a device type here would put a phantom "
		      "entity in every controller");
	zassert_equal(r->cluster, MATTER_CLUSTER_POWER_SOURCE, NULL);
}

ZTEST(radiant_matter, test_heart_rate_has_no_row_and_that_is_the_design)
{
	/*
	 * Section 8.1's table: "0x26 heart rate | none, and section 1 is the
	 * whole reason". If this ever starts passing a non-NULL row, somebody
	 * has added the Flow Measurement hack of section 8.3b or the MEI
	 * cluster of section 8.3 to tier 1, and both are explicitly outside it.
	 */
	zassert_is_null(radiant_matter_row(RADIANT_FIELD_HEART_RATE),
			"heart rate must NOT reach Matter through the tier 1 "
			"table - see sections 8.3 and 8.3b");
}

ZTEST(radiant_matter, test_a_type_with_no_row_is_declined_not_an_error)
{
	struct radiant_sample s;
	int64_t out = 12345;

	/* Section 8.1: "A type with no row is not an error - it is a field the
	 * Matter plane declines and the MQTT plane carries." */
	s = mk(1u, 0u, RADIANT_FIELD_ACTIVE_POWER, 250, 0);
	zassert_false(radiant_matter_convert(&s, &out),
		      "a type with no row does not convert");
	zassert_is_null(radiant_matter_row(RADIANT_FIELD_ACTIVE_POWER), NULL);
	zassert_equal(radiant_matter_endpoint_count(), 0u,
		      "and it instantiates no endpoint");
}

/* ---------------------------------------------------------------------------
 * Conversion - section 8.1a's trap
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_matter, test_temperature_is_a_scale_AND_a_shift)
{
	struct radiant_sample s;
	int64_t v = 0;

	/*
	 * Section 8.1a: "temperature is the only row with an offset... the
	 * conversion is K x 100 - 27315 - a scale *and* a shift".
	 *
	 * 273.15 K is 0.00 C, which is 0 in the cluster's 0.01 C units. This
	 * is the value that catches a missing offset AND a wrong sign on it,
	 * because both give something far from zero.
	 */
	s = mk(1u, 0u, RADIANT_FIELD_TEMPERATURE, 27315, -2);
	zassert_true(radiant_matter_convert(&s, &v), NULL);
	zassert_equal(v, 0, "273.15 K is exactly 0.00 C, got %lld",
		      (long long)v);

	/* 298.15 K = 25.00 C = 2500. An ordinary room, and the value that
	 * catches truncation: done naively the 10^-2 is lost before the x100. */
	s = mk(1u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2);
	zassert_true(radiant_matter_convert(&s, &v), NULL);
	zassert_equal(v, 2500, "298.15 K is 25.00 C, got %lld", (long long)v);

	/* Below freezing, where a truncating implementation rounds the other
	 * way and the bias becomes a discontinuity at 0 C. 263.15 K = -10 C. */
	s = mk(1u, 0u, RADIANT_FIELD_TEMPERATURE, 26315, -2);
	zassert_true(radiant_matter_convert(&s, &v), NULL);
	zassert_equal(v, -1000, "263.15 K is -10.00 C, got %lld", (long long)v);
}

ZTEST(radiant_matter, test_the_negative_exponent_is_not_lost_before_the_scale)
{
	struct radiant_sample s;
	int64_t v = 0;

	/*
	 * 3 decimal places of kelvin. 300.123 K -> 26.973 C -> 2697.3 in
	 * hundredths, which rounds to 2697. An implementation that computes
	 * value_SI first as an integer gets 300 K and answers 2685 - twelve
	 * hundredths of a degree out, which no user would ever notice and no
	 * test but this one would catch.
	 */
	s = mk(1u, 0u, RADIANT_FIELD_TEMPERATURE, 300123, -3);
	zassert_true(radiant_matter_convert(&s, &v), NULL);
	zassert_equal(v, 2697, "300.123 K is 26.973 C -> 2697, got %lld",
		      (long long)v);
}

ZTEST(radiant_matter, test_humidity_and_battery_are_pure_scales)
{
	struct radiant_sample s;
	int64_t v = 0;

	/* 45.5 % -> 4550 in 0.01 % units. */
	s = mk(1u, 0u, RADIANT_FIELD_HUMIDITY, 455, -1);
	zassert_true(radiant_matter_convert(&s, &v), NULL);
	zassert_equal(v, 4550, "45.5 %% is 4550, got %lld", (long long)v);

	/* Section 8.1: battery is "0.5 %: % x 2". 80 % -> 160. */
	s = mk(1u, 1u, RADIANT_FIELD_BATTERY_SOC, 80, 0);
	zassert_true(radiant_matter_convert(&s, &v), NULL);
	zassert_equal(v, 160, "80 %% at 0.5 %% per step is 160, got %lld",
		      (long long)v);
}

ZTEST(radiant_matter, test_occupancy_passes_through_as_a_boolean)
{
	struct radiant_sample s;
	int64_t v = 99;

	s = mk(1u, 0u, RADIANT_FIELD_OCCUPANCY, 1, 0);
	zassert_true(radiant_matter_convert(&s, &v), NULL);
	zassert_equal(v, 1, NULL);

	s = mk(1u, 0u, RADIANT_FIELD_OCCUPANCY, 0, 0);
	zassert_true(radiant_matter_convert(&s, &v), NULL);
	zassert_equal(v, 0, NULL);
}

ZTEST(radiant_matter, test_an_unrepresentable_value_is_refused_not_saturated)
{
	struct radiant_sample s;
	int64_t v = 0;

	/*
	 * A saturating cast would put a plausible wrong number in front of a
	 * user - 327.67 C reads like a real fault rather than like a broken
	 * decoder. Refusing means publish() logs and writes nothing, leaving
	 * the previous value and the node's reachability to tell the story.
	 */
	s = mk(1u, 0u, RADIANT_FIELD_TEMPERATURE, INT64_MAX / 2, 0);
	zassert_false(radiant_matter_convert(&s, &v),
		      "an overflowing conversion is refused");

	/* An exponent past the table is refused rather than approximated. */
	s = mk(1u, 0u, RADIANT_FIELD_TEMPERATURE, 1, 40);
	zassert_false(radiant_matter_convert(&s, &v), NULL);
}

/* ---------------------------------------------------------------------------
 * The endpoint model - section 8.1's actual rewrite
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_matter, test_four_derived_booleans_are_four_endpoints_not_one)
{
	const struct radiant_matter_endpoint *e;
	struct radiant_sample s;
	uint8_t f;

	/*
	 * THE TEST THAT JUSTIFIES SECTION 8.1's REWRITE.
	 *
	 * The four derived booleans of section 6 - worn, bike in use, at rest,
	 * training - are ALL field type 0x02. Keyed on TYPE they collapse into
	 * one endpoint that four rules fight over, and Home Assistant shows one
	 * entity flickering between four meanings. Keyed on FIELD_ID they are
	 * four instances of one table row, which is what "an endpoint is
	 * instantiated per announced field, not per binding kind" means.
	 */
	for (f = 0u; f < 4u; f++) {
		s = mk(7u, f, RADIANT_FIELD_OCCUPANCY, (f & 1u) ? 1 : 0, 0);
		s.flags = RADIANT_SAMPLE_DERIVED;
		zassert_not_null(radiant_matter_row(s.field_type), NULL);
		/* publish through the sink's own path */
		e = radiant_matter_endpoint_for(s.source, s.field_id);
		zassert_is_null(e, "not instantiated before the first sample");
	}
}

ZTEST(radiant_matter, test_endpoints_are_assigned_on_first_sample_and_are_stable)
{
	const struct radiant_matter_endpoint *a;
	const struct radiant_matter_endpoint *b;
	struct radiant_sample s;
	uint16_t first_id;

	/* Drive the sink the way the bus does. */
	s = mk(3u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2);
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_equal(radiant_matter_endpoint_count(), 1u,
		      "one announced field, one endpoint");
	a = radiant_matter_endpoint_for(3u, 0u);
	zassert_not_null(a, NULL);
	zassert_not_equal(a->endpoint_id, 0u,
			  "endpoint 0 is the Matter root node and can never "
			  "be ours");
	first_id = a->endpoint_id;
	zassert_equal(last.writes, 1u, NULL);
	zassert_equal(last.value, 2500, NULL);
	zassert_equal(last.cluster, MATTER_CLUSTER_TEMP_MEASUREMENT, NULL);

	/* A second sample on the same field reuses the endpoint. An endpoint
	 * number that moved would re-create the entity in every controller. */
	s = mk(3u, 0u, RADIANT_FIELD_TEMPERATURE, 29915, -2);
	radiant_bridge_post(&s);
	radiant_bridge_drain();
	b = radiant_matter_endpoint_for(3u, 0u);
	zassert_not_null(b, NULL);
	zassert_equal(b->endpoint_id, first_id, "endpoint ids are stable");
	zassert_equal(radiant_matter_endpoint_count(), 1u, NULL);
	zassert_equal(last.writes, 2u, NULL);
	zassert_equal(last.value, 2600, NULL);
}

ZTEST(radiant_matter, test_the_same_field_id_on_two_sources_is_two_endpoints)
{
	struct radiant_sample s;

	/* field_id is "stable within source", not globally unique - two straps
	 * both announce field 0. Keying on field_id alone would merge them. */
	s = mk(1u, 0u, RADIANT_FIELD_OCCUPANCY, 1, 0);
	radiant_bridge_post(&s);
	s = mk(2u, 0u, RADIANT_FIELD_OCCUPANCY, 1, 0);
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_equal(radiant_matter_endpoint_count(), 2u,
		      "two sources, two endpoints");
	zassert_not_equal(radiant_matter_endpoint_for(1u, 0u)->endpoint_id,
			  radiant_matter_endpoint_for(2u, 0u)->endpoint_id,
			  NULL);
}

ZTEST(radiant_matter, test_a_declined_type_never_reaches_the_seam)
{
	struct radiant_sample s;

	/*
	 * Heart rate goes on the bus like everything else - the MQTT plane
	 * carries it - and must not produce a Matter write or an endpoint.
	 * This is the sink's `want` doing its job rather than publish()
	 * checking again, and it is worth a test because a `want` that
	 * returned true for everything would still LOOK correct: publish()
	 * declines too, and only the endpoint count would differ.
	 */
	s = mk(4u, 0u, RADIANT_FIELD_HEART_RATE, 72, 0);
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_equal(last.writes, 0u, "no Matter write for heart rate");
	zassert_equal(radiant_matter_endpoint_count(), 0u,
		      "and no endpoint instantiated for it");
}

ZTEST(radiant_matter, test_a_heart_rate_binding_shows_three_and_a_power_meter_one)
{
	struct radiant_sample s;
	uint8_t f;

	/*
	 * Section 8.1's worked outcome: "a power meter binding instantiates one
	 * endpoint, a heart-rate binding three, and a household with one strap
	 * and one trainer shows four entities in Home Assistant rather than
	 * sixteen."
	 *
	 * The strap announces raw bpm (declined) plus three derived booleans:
	 * worn, at rest, training. The trainer announces power (declined) plus
	 * one derived boolean: bike in use.
	 */
	s = mk(1u, 0u, RADIANT_FIELD_HEART_RATE, 72, 0);
	radiant_bridge_post(&s);
	for (f = 1u; f <= 3u; f++) {
		s = mk(1u, f, RADIANT_FIELD_OCCUPANCY, 1, 0);
		s.flags = RADIANT_SAMPLE_DERIVED;
		radiant_bridge_post(&s);
	}

	s = mk(2u, 0u, RADIANT_FIELD_ACTIVE_POWER, 250, 0);
	radiant_bridge_post(&s);
	s = mk(2u, 1u, RADIANT_FIELD_OCCUPANCY, 1, 0);
	s.flags = RADIANT_SAMPLE_DERIVED;
	radiant_bridge_post(&s);

	radiant_bridge_drain();

	zassert_equal(radiant_matter_endpoint_count(), 4u,
		      "one strap and one trainer show FOUR entities, not "
		      "sixteen and not two - got %u",
		      (unsigned int)radiant_matter_endpoint_count());
}

ZTEST(radiant_matter, test_a_stale_sample_is_still_written)
{
	struct radiant_sample s;

	/*
	 * Matter has no "this value is old" on these clusters. Not writing
	 * leaves the last good value in place, which is indistinguishable from
	 * a live sensor reporting the same thing - strictly worse than writing
	 * the value we actually have. Staleness belongs in reachability.
	 */
	s = mk(5u, 0u, RADIANT_FIELD_HUMIDITY, 400, -1);
	s.flags = RADIANT_SAMPLE_STALE;
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_equal(last.writes, 1u, "a stale sample still writes");
	zassert_equal(last.value, 4000, NULL);
}

