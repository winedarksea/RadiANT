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

/*
 * Every write, not just the last one. A row may now write more than one
 * attribute per sample (the pressure row's ScaledValue beside its mandatory
 * MeasuredValue) and more than once per endpoint lifetime (occupancy's
 * constant OccupancySensorType at creation), and "the last write" cannot see
 * either. 32 is more than any test here produces.
 */
static struct {
	uint16_t endpoint_id;
	uint32_t cluster;
	uint32_t attribute;
	int64_t  value;
} writes[32];
static uint32_t n_writes;

void radiant_matter_attr_write(uint16_t endpoint_id, uint32_t cluster,
			       uint32_t attribute, int64_t value)
{
	last.endpoint_id = endpoint_id;
	last.cluster = cluster;
	last.attribute = attribute;
	last.value = value;
	last.writes++;

	if (n_writes < ARRAY_SIZE(writes)) {
		writes[n_writes].endpoint_id = endpoint_id;
		writes[n_writes].cluster = cluster;
		writes[n_writes].attribute = attribute;
		writes[n_writes].value = value;
		n_writes++;
	}
}

/* The one write of `attribute` on `cluster`, or NULL. Fails the caller's
 * assertion rather than this one if there are two. */
static int find_write(uint32_t cluster, uint32_t attribute, int64_t *value_out,
		      uint16_t *endpoint_out)
{
	uint32_t i;
	int found = 0;

	for (i = 0u; i < n_writes; i++) {
		if (writes[i].cluster == cluster &&
		    writes[i].attribute == attribute) {
			if (value_out != NULL) {
				*value_out = writes[i].value;
			}
			if (endpoint_out != NULL) {
				*endpoint_out = writes[i].endpoint_id;
			}
			found++;
		}
	}
	return found;
}

static void matter_before(void *f)
{
	ARG_UNUSED(f);
	memset(&last, 0, sizeof(last));
	memset(writes, 0, sizeof(writes));
	n_writes = 0u;
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
	 * Section 8.1a: temperature is the only row with an offset - the
	 * conversion is K x 100 - 27315, a scale and a shift. 273.15 K is
	 * 0.00 C = 0 in the cluster's units, which catches a missing offset or
	 * a wrong sign on it.
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

	/* 300.123 K -> 26.973 C -> 2697.3 hundredths -> 2697. An implementation
	 * that truncates to an integer kelvin first gets 300 K and answers
	 * 2685 - twelve hundredths of a degree out and easy to miss. */
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

	/* A saturating cast would put a plausible wrong number in front of a
	 * user (327.67 C reads as a real fault, not a broken decoder).
	 * Refusing leaves the previous value and reachability to tell the story. */
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
	 * The four derived booleans of section 6 - worn, bike in use, at rest,
	 * training - are all field type 0x02. Keyed on TYPE they'd collapse
	 * into one endpoint four rules fight over; keyed on FIELD_ID they're
	 * four instances of one table row (an endpoint is instantiated per
	 * announced field, not per binding kind).
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

	/* Heart rate goes on the bus like everything else (MQTT carries it)
	 * and must not produce a Matter write or endpoint - this is the sink's
	 * `want` doing its job, not publish() checking again. A `want` that
	 * returned true for everything would still look correct in the write
	 * count, so the endpoint count matters too. */
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
	 * Section 8.1's worked outcome: a power meter binding instantiates one
	 * endpoint, a heart-rate binding three, so one strap plus one trainer
	 * shows four entities rather than sixteen. The strap announces raw bpm
	 * (declined) plus three derived booleans: worn, at rest, training. The
	 * trainer announces power (declined) plus
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

	/* Matter has no "this value is old" on these clusters. Not writing
	 * leaves the last good value in place, indistinguishable from a live
	 * sensor - worse than writing the value we have. Staleness belongs in
	 * reachability. */
	s = mk(5u, 0u, RADIANT_FIELD_HUMIDITY, 400, -1);
	s.flags = RADIANT_SAMPLE_STALE;
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_equal(last.writes, 1u, "a stale sample still writes");
	zassert_equal(last.value, 4000, NULL);
}

/* ---------------------------------------------------------------------------
 * Package D - the pressure row, and the two bugs fixed before CHIP amplifies
 * them
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_matter, test_pressure_serves_both_the_mandatory_and_the_useful_attribute)
{
	struct radiant_sample s;
	int64_t v = 0;
	uint16_t ep_measured = 0;
	uint16_t ep_scaled = 0;

	/*
	 * Standard atmosphere, 101325 Pa.
	 *
	 * MeasuredValue is an int16s in WHOLE kPa, so it can only say 101 -
	 * that is the cluster's definition, not a rounding bug, and it is why
	 * this row does not stop there. ScaledValue at Scale 2 says 10132.5 ->
	 * 10133, which is 10 Pa of resolution. Both are written, from one
	 * sample, onto one endpoint.
	 */
	s = mk(9u, 0u, RADIANT_FIELD_PRESSURE, 101325, 0);
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_equal(radiant_matter_endpoint_count(), 1u,
		      "one pressure field, one endpoint");

	zassert_equal(find_write(MATTER_CLUSTER_PRESSURE_MEASUREMENT,
				 MATTER_ATTR_MEASURED_VALUE, &v, &ep_measured),
		      1, "MeasuredValue is mandatory and must be written");
	zassert_equal(v, 101, "101325 Pa is 101 whole kPa, got %lld",
		      (long long)v);

	zassert_equal(find_write(MATTER_CLUSTER_PRESSURE_MEASUREMENT,
				 MATTER_ATTR_SCALED_VALUE, &v, &ep_scaled),
		      1, "ScaledValue carries the resolution MeasuredValue "
			 "cannot");
	zassert_equal(v, 10133,
		      "101325 Pa at Scale 2 is 10132.5 -> 10133 (round half "
		      "away from zero), got %lld", (long long)v);

	/* Scale is a constant and must be written, exactly once, or
	 * ScaledValue is unreadable - it reads back as 0, i.e. whole kPa,
	 * i.e. a barometer reporting 10133 kPa. */
	zassert_equal(find_write(MATTER_CLUSTER_PRESSURE_MEASUREMENT,
				 MATTER_ATTR_SCALE, &v, NULL),
		      1, "Scale is written exactly once");
	zassert_equal(v, MATTER_PRESSURE_SCALE, NULL);

	zassert_equal(ep_measured, ep_scaled,
		      "all three are attributes of ONE endpoint");
}

ZTEST(radiant_matter, test_the_pressure_constant_is_written_once_not_per_sample)
{
	struct radiant_sample s;
	int i;

	for (i = 0; i < 5; i++) {
		s = mk(9u, 0u, RADIANT_FIELD_PRESSURE, 101325 + i, 0);
		radiant_bridge_post(&s);
		radiant_bridge_drain();
	}

	/* Five samples: five MeasuredValue, five ScaledValue, ONE Scale. A
	 * constant re-written per sample would dirty a subscription path 4 Hz
	 * for a value that never changes - the exact waste finding 7's
	 * deadband argument is about. */
	zassert_equal(find_write(MATTER_CLUSTER_PRESSURE_MEASUREMENT,
				 MATTER_ATTR_SCALE, NULL, NULL),
		      1, "Scale is a creation-time constant");
	zassert_equal(find_write(MATTER_CLUSTER_PRESSURE_MEASUREMENT,
				 MATTER_ATTR_SCALED_VALUE, NULL, NULL),
		      5, NULL);
}

ZTEST(radiant_matter, test_occupancy_declares_itself_as_not_a_pir)
{
	struct radiant_sample s;
	int64_t v = 99;

	/*
	 * Trap 16. Zero-initialised, OccupancySensorType means PIR, and
	 * home-assistant/core#164839 proposes remapping PIR occupancy
	 * endpoints to `device_class: motion`. "Strap is worn" is not motion.
	 * On a dynamic endpoint every attribute is external storage, so a
	 * constant nobody writes reads back as zero - there is no default to
	 * fall back on and this write is the only thing standing between these
	 * four booleans and a future controller relabelling all of them.
	 */
	s = mk(6u, 0u, RADIANT_FIELD_OCCUPANCY, 1, 0);
	s.flags = RADIANT_SAMPLE_DERIVED;
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_equal(find_write(MATTER_CLUSTER_OCCUPANCY_SENSING,
				 MATTER_ATTR_OCCUPANCY_SENSOR_TYPE, &v, NULL),
		      1, NULL);
	zassert_equal(v, MATTER_OCCUPANCY_TYPE_PHYSICAL_CONTACT,
		      "OccupancySensorType must NOT be left at 0 (PIR)");
	zassert_not_equal(v, 0, "0 is PIR - see trap 16");

	zassert_equal(find_write(MATTER_CLUSTER_OCCUPANCY_SENSING,
				 MATTER_ATTR_OCCUPANCY_SENSOR_BITMAP, &v, NULL),
		      1, NULL);
	zassert_equal(v, MATTER_OCCUPANCY_BITMAP_PHYSICAL_CONTACT,
		      "the bitmap must agree with the enum - a controller may "
		      "read either");
}

ZTEST(radiant_matter, test_a_battery_does_not_become_its_own_phantom_entity)
{
	struct radiant_sample s;
	const struct radiant_matter_endpoint *primary;
	int64_t v = 0;
	uint16_t ep = 0xFFFFu;

	/*
	 * radiant_matter.h has documented `device_type 0` as "cluster only, on
	 * the binding's own endpoint" since P7, and matter_publish() called
	 * endpoint_get_or_add() unconditionally, so a battery field allocated
	 * an endpoint of its own with no device type. In Matter that is a
	 * bridged node with no device type and one Power Source cluster: a
	 * phantom entity sitting beside the real sensor in every controller.
	 */
	s = mk(2u, 0u, RADIANT_FIELD_OCCUPANCY, 1, 0);
	s.flags = RADIANT_SAMPLE_DERIVED;
	radiant_bridge_post(&s);
	radiant_bridge_drain();
	zassert_equal(radiant_matter_endpoint_count(), 1u, NULL);
	primary = radiant_matter_endpoint_for(2u, 0u);
	zassert_not_null(primary, NULL);

	s = mk(2u, 1u, RADIANT_FIELD_BATTERY_SOC, 80, 0);
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_equal(radiant_matter_endpoint_count(), 1u,
		      "a battery adds NO endpoint - got %u",
		      (unsigned int)radiant_matter_endpoint_count());
	zassert_equal(find_write(MATTER_CLUSTER_POWER_SOURCE,
				 MATTER_ATTR_BAT_PERCENT_REMAINING, &v, &ep),
		      1, "but it is still published");
	zassert_equal(v, 160, "80 %% at 0.5 %% per step is 160", NULL);
	zassert_equal(ep, primary->endpoint_id,
		      "onto the source's OWN endpoint, not a new one");
}

ZTEST(radiant_matter, test_a_battery_with_no_primary_yet_is_deferred_not_dropped)
{
	struct radiant_sample s;

	/* Common page 82 can lead a sensor's first data page. Deferring is the
	 * right answer, and the next battery sample after a primary exists
	 * must land - "deferred" must not mean "declined for good". */
	s = mk(3u, 1u, RADIANT_FIELD_BATTERY_SOC, 80, 0);
	radiant_bridge_post(&s);
	radiant_bridge_drain();
	zassert_equal(radiant_matter_endpoint_count(), 0u,
		      "no endpoint, and in particular no phantom one");
	zassert_equal(last.writes, 0u, NULL);

	s = mk(3u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2);
	radiant_bridge_post(&s);
	s = mk(3u, 1u, RADIANT_FIELD_BATTERY_SOC, 80, 0);
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_equal(radiant_matter_endpoint_count(), 1u, NULL);
	zassert_equal(find_write(MATTER_CLUSTER_POWER_SOURCE,
				 MATTER_ATTR_BAT_PERCENT_REMAINING, NULL, NULL),
		      1, "the battery lands once the primary exists");
}

ZTEST(radiant_matter, test_wind_has_no_row_and_that_is_the_design)
{
	/*
	 * Finding 7: the chip/ data model has flow, pressure, humidity,
	 * temperature, occupancy and air-quality clusters and NO wind
	 * anything. Wind speed and direction take the MQTT plane only - the
	 * same rule as heart rate, for the same reason, and the second
	 * instance of it. If either of these starts passing a row, somebody has
	 * mapped wind onto a cluster that means something else.
	 */
	zassert_is_null(radiant_matter_row(RADIANT_FIELD_SPEED),
			"there is no wind speed cluster in Matter");
	zassert_is_null(radiant_matter_row(RADIANT_FIELD_ANGLE),
			"there is no wind direction cluster in Matter");
}

ZTEST(radiant_matter, test_the_endpoint_ceiling_announces_itself)
{
	struct radiant_sample s;
	uint8_t f;

	/*
	 * The budget does not close in the worst case (see
	 * RADIANT_MATTER_MAX_ENDPOINTS' comment). What must hold is that
	 * overrunning it is bounded and loud rather than corrupting: the table
	 * stops at its size, the endpoints already assigned keep their ids,
	 * and nothing writes past the array.
	 */
	for (f = 0u; f < RADIANT_MATTER_MAX_ENDPOINTS + 4u; f++) {
		s = mk(1u, f, RADIANT_FIELD_OCCUPANCY, 1, 0);
		s.flags = RADIANT_SAMPLE_DERIVED;
		radiant_bridge_post(&s);
		radiant_bridge_drain();
	}

	zassert_equal(radiant_matter_endpoint_count(),
		      RADIANT_MATTER_MAX_ENDPOINTS,
		      "the table stops at its size");
	zassert_not_null(radiant_matter_endpoint_for(1u, 0u),
			 "and the first endpoint is undisturbed");
	zassert_is_null(radiant_matter_endpoint_for(
				1u, (uint8_t)RADIANT_MATTER_MAX_ENDPOINTS),
			"the one past the ceiling simply does not appear");
}

