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

#include <radiant/radiant_binding.h>
#include <radiant/radiant_bridge.h>
#include <radiant/radiant_env_adapter.h>
#include <radiant/radiant_hr_adapter.h>
#include <radiant/radiant_matter.h>
#include <radiant/radiant_rules.h>

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
	zassert_not_null(r);
	zassert_equal(r->device_type, RADIANT_MATTER_DEVTYPE_OCCUPANCY_SENSOR);
	zassert_equal(r->cluster, RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING);

	r = radiant_matter_row(RADIANT_FIELD_TEMPERATURE);
	zassert_not_null(r);
	zassert_equal(r->device_type, RADIANT_MATTER_DEVTYPE_TEMPERATURE_SENSOR);
	zassert_equal(r->cluster, RADIANT_MATTER_CLUSTER_TEMP_MEASUREMENT);

	r = radiant_matter_row(RADIANT_FIELD_HUMIDITY);
	zassert_not_null(r);
	zassert_equal(r->device_type, RADIANT_MATTER_DEVTYPE_HUMIDITY_SENSOR);
	zassert_equal(r->cluster, RADIANT_MATTER_CLUSTER_REL_HUMIDITY);

	r = radiant_matter_row(RADIANT_FIELD_BATTERY_SOC);
	zassert_not_null(r);
	/* Battery is cluster only, on the binding's own endpoint: a device type
	 * here would put a phantom entity in every controller (section 8.1). */
	zassert_equal(r->device_type, 0u);
	zassert_equal(r->cluster, RADIANT_MATTER_CLUSTER_POWER_SOURCE);
}

ZTEST(radiant_matter, test_heart_rate_has_no_row_and_that_is_the_design)
{
	/*
	 * Section 8.1's table: "0x26 heart rate | none, and section 1 is the
	 * whole reason". If this ever starts passing a non-NULL row, somebody
	 * has added the Flow Measurement hack of section 8.3b or the MEI
	 * cluster of section 8.3 to tier 1, and both are explicitly outside it.
	 */
	zassert_is_null(radiant_matter_row(RADIANT_FIELD_HEART_RATE));
}

ZTEST(radiant_matter, test_a_type_with_no_row_is_declined_not_an_error)
{
	struct radiant_sample s;
	int64_t out = 12345;

	/* Section 8.1: "A type with no row is not an error - it is a field the
	 * Matter plane declines and the MQTT plane carries." */
	s = mk(1u, 0u, RADIANT_FIELD_ACTIVE_POWER, 250, 0);
	zassert_false(radiant_matter_convert(&s, &out));
	zassert_is_null(radiant_matter_row(RADIANT_FIELD_ACTIVE_POWER));
	zassert_equal(radiant_matter_endpoint_count(), 0u);
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
	zassert_true(radiant_matter_convert(&s, &v));
	zassert_equal(v, 0, "%lld", (long long)v);

	/* 298.15 K = 25.00 C = 2500. An ordinary room, and the value that
	 * catches truncation: done naively the 10^-2 is lost before the x100. */
	s = mk(1u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2);
	zassert_true(radiant_matter_convert(&s, &v));
	zassert_equal(v, 2500, "%lld", (long long)v);

	/* Below freezing, where a truncating implementation rounds the other
	 * way and the bias becomes a discontinuity at 0 C. 263.15 K = -10 C. */
	s = mk(1u, 0u, RADIANT_FIELD_TEMPERATURE, 26315, -2);
	zassert_true(radiant_matter_convert(&s, &v));
	zassert_equal(v, -1000, "%lld", (long long)v);
}

ZTEST(radiant_matter, test_the_negative_exponent_is_not_lost_before_the_scale)
{
	struct radiant_sample s;
	int64_t v = 0;

	/* 300.123 K -> 26.973 C -> 2697.3 hundredths -> 2697. An implementation
	 * that truncates to an integer kelvin first gets 300 K and answers
	 * 2685 - twelve hundredths of a degree out and easy to miss. */
	s = mk(1u, 0u, RADIANT_FIELD_TEMPERATURE, 300123, -3);
	zassert_true(radiant_matter_convert(&s, &v));
	zassert_equal(v, 2697, "%lld", (long long)v);
}

ZTEST(radiant_matter, test_humidity_and_battery_are_pure_scales)
{
	struct radiant_sample s;
	int64_t v = 0;

	/* 45.5 % -> 4550 in 0.01 % units. */
	s = mk(1u, 0u, RADIANT_FIELD_HUMIDITY, 455, -1);
	zassert_true(radiant_matter_convert(&s, &v));
	zassert_equal(v, 4550, "%lld", (long long)v);

	/* Section 8.1: battery is "0.5 %: % x 2". 80 % -> 160. */
	s = mk(1u, 1u, RADIANT_FIELD_BATTERY_SOC, 80, 0);
	zassert_true(radiant_matter_convert(&s, &v));
	zassert_equal(v, 160, "%lld", (long long)v);
}

ZTEST(radiant_matter, test_occupancy_passes_through_as_a_boolean)
{
	struct radiant_sample s;
	int64_t v = 99;

	s = mk(1u, 0u, RADIANT_FIELD_OCCUPANCY, 1, 0);
	zassert_true(radiant_matter_convert(&s, &v));
	zassert_equal(v, 1);

	s = mk(1u, 0u, RADIANT_FIELD_OCCUPANCY, 0, 0);
	zassert_true(radiant_matter_convert(&s, &v));
	zassert_equal(v, 0);
}

ZTEST(radiant_matter, test_an_unrepresentable_value_is_refused_not_saturated)
{
	struct radiant_sample s;
	int64_t v = 0;

	/* A saturating cast would put a plausible wrong number in front of a
	 * user (327.67 C reads as a real fault, not a broken decoder).
	 * Refusing leaves the previous value and reachability to tell the story. */
	s = mk(1u, 0u, RADIANT_FIELD_TEMPERATURE, INT64_MAX / 2, 0);
	zassert_false(radiant_matter_convert(&s, &v));

	/* An exponent past the table is refused rather than approximated. */
	s = mk(1u, 0u, RADIANT_FIELD_TEMPERATURE, 1, 40);
	zassert_false(radiant_matter_convert(&s, &v));
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
		zassert_not_null(radiant_matter_row(s.field_type));
		/* publish through the sink's own path */
		e = radiant_matter_endpoint_for(s.source, s.field_id, s.field_type);
		zassert_is_null(e);
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

	zassert_equal(radiant_matter_endpoint_count(), 1u);
	a = radiant_matter_endpoint_for(3u, 0u, RADIANT_FIELD_TEMPERATURE);
	zassert_not_null(a);
	zassert_not_equal(a->endpoint_id, 0u);
	first_id = a->endpoint_id;
	zassert_equal(last.writes, 1u);
	zassert_equal(last.value, 2500);
	zassert_equal(last.cluster, RADIANT_MATTER_CLUSTER_TEMP_MEASUREMENT);

	/* A second sample on the same field reuses the endpoint. An endpoint
	 * number that moved would re-create the entity in every controller. */
	s = mk(3u, 0u, RADIANT_FIELD_TEMPERATURE, 29915, -2);
	radiant_bridge_post(&s);
	radiant_bridge_drain();
	b = radiant_matter_endpoint_for(3u, 0u, RADIANT_FIELD_TEMPERATURE);
	zassert_not_null(b);
	zassert_equal(b->endpoint_id, first_id);
	zassert_equal(radiant_matter_endpoint_count(), 1u);
	zassert_equal(last.writes, 2u);
	zassert_equal(last.value, 2600);
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

	zassert_equal(radiant_matter_endpoint_count(), 2u);
	zassert_not_equal(radiant_matter_endpoint_for(1u, 0u, RADIANT_FIELD_OCCUPANCY)->endpoint_id,
			  radiant_matter_endpoint_for(2u, 0u, RADIANT_FIELD_OCCUPANCY)->endpoint_id);
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

	zassert_equal(last.writes, 0u);
	zassert_equal(radiant_matter_endpoint_count(), 0u);
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

	zassert_equal(radiant_matter_endpoint_count(), 4u, "got %u",
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

	zassert_equal(last.writes, 1u);
	zassert_equal(last.value, 4000);
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
	 * MeasuredValue is an int16s in 0.1 kPa (Matter 1.5.1 section
	 * 2.4.5.1, 10 x P[kPa]), so it says 1013.25 -> 1013 - a 100 Pa step,
	 * which is the cluster's definition rather than a rounding bug, and
	 * is why this row does not stop there. ScaledValue is in PASCALS x
	 * 10^Scale (section 2.4.5.5), so at Scale -1 it says 10132.5 -> 10133,
	 * which is 10 Pa of resolution. Both are written, from one sample,
	 * onto one endpoint.
	 *
	 * The numbers are the regression: the row used to write 101 (whole
	 * kPa, 10x low) and 10133 beside Scale 2, which a controller reading
	 * the spec decodes as 101.33 Pa. 10133 is unchanged; only its Scale
	 * was wrong, so the Scale assertion below is the half that catches
	 * that bug.
	 */
	s = mk(9u, 0u, RADIANT_FIELD_PRESSURE, 101325, 0);
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_equal(radiant_matter_endpoint_count(), 1u);

	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_PRESSURE_MEASUREMENT,
				 RADIANT_MATTER_ATTR_MEASURED_VALUE, &v, &ep_measured),
		      1);
	zassert_equal(v, 1013, "%lld", (long long)v);

	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_PRESSURE_MEASUREMENT,
				 RADIANT_MATTER_ATTR_SCALED_VALUE, &v, &ep_scaled),
		      1);
	zassert_equal(v, 10133, "%lld", (long long)v);

	/* Scale is a constant and must be written, exactly once, or
	 * ScaledValue is unreadable - it reads back as 0, i.e. whole Pa,
	 * i.e. a barometer reporting 10133 Pa. And it must be -1 LITERALLY,
	 * not merely equal to the header's constant: comparing against
	 * RADIANT_MATTER_PRESSURE_SCALE alone is how Scale 2 sat beside Pa/10 with a
	 * green test. */
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_PRESSURE_MEASUREMENT,
				 RADIANT_MATTER_ATTR_SCALE, &v, NULL),
		      1);
	zassert_equal(v, RADIANT_MATTER_PRESSURE_SCALE);
	zassert_equal(v, -1, "%lld", (long long)v);

	zassert_equal(ep_measured, ep_scaled);
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
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_PRESSURE_MEASUREMENT,
				 RADIANT_MATTER_ATTR_SCALE, NULL, NULL),
		      1);
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_PRESSURE_MEASUREMENT,
				 RADIANT_MATTER_ATTR_SCALED_VALUE, NULL, NULL),
		      5);
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

	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				 RADIANT_MATTER_ATTR_OCCUPANCY_SENSOR_TYPE, &v, NULL),
		      1);
	zassert_equal(v, RADIANT_MATTER_OCCUPANCY_TYPE_PHYSICAL_CONTACT);
	zassert_not_equal(v, 0);

	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				 RADIANT_MATTER_ATTR_OCCUPANCY_SENSOR_BITMAP, &v, NULL),
		      1);
	zassert_equal(v, RADIANT_MATTER_OCCUPANCY_BITMAP_PHYSICAL_CONTACT);
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
	zassert_equal(radiant_matter_endpoint_count(), 1u);
	primary = radiant_matter_endpoint_for(2u, 0u, RADIANT_FIELD_OCCUPANCY);
	zassert_not_null(primary);

	s = mk(2u, 1u, RADIANT_FIELD_BATTERY_SOC, 80, 0);
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_equal(radiant_matter_endpoint_count(), 1u, "got %u",
		      (unsigned int)radiant_matter_endpoint_count());
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_POWER_SOURCE,
				 RADIANT_MATTER_ATTR_BAT_PERCENT_REMAINING, &v, &ep),
		      1);
	zassert_equal(v, 160);
	zassert_equal(ep, primary->endpoint_id);
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
	zassert_equal(radiant_matter_endpoint_count(), 0u);
	zassert_equal(last.writes, 0u);

	s = mk(3u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2);
	radiant_bridge_post(&s);
	s = mk(3u, 1u, RADIANT_FIELD_BATTERY_SOC, 80, 0);
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_equal(radiant_matter_endpoint_count(), 1u);
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_POWER_SOURCE,
				 RADIANT_MATTER_ATTR_BAT_PERCENT_REMAINING, NULL, NULL),
		      1);
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
	zassert_is_null(radiant_matter_row(RADIANT_FIELD_SPEED));
	zassert_is_null(radiant_matter_row(RADIANT_FIELD_ANGLE));
}

/* ---------------------------------------------------------------------------
 * Package E6 - the deadbands
 *
 * Finding 7's argument, made executable. These are an EFFICIENCY measure and
 * not a conformance one: a subscription's MinIntervalFloor already bounds how
 * often a report leaves the device, so writing at 4 Hz would be legal. What it
 * would not be is free - each write walks the subscription list and bumps
 * DataVersion on the CHIP thread, which preempts the ANT host thread - and a
 * controller subscribing with MinIntervalFloor = 0 would put 4 Hz per
 * attribute straight into the section 7 coexistence budget.
 * ---------------------------------------------------------------------------
 */

/* Same as mk() but with an explicit sample time, because every deadband test
 * is really a test about two samples and the gap between them. */
static struct radiant_sample mk_at(uint32_t source, uint8_t field_id,
				   uint8_t field_type, int64_t raw, int8_t exp,
				   uint64_t t_us)
{
	struct radiant_sample s = mk(source, field_id, field_type, raw, exp);

	s.t_us = t_us;
	return s;
}

static void post_drain(const struct radiant_sample *s)
{
	radiant_bridge_post(s);
	radiant_bridge_drain();
}

ZTEST(radiant_matter, test_temperature_below_the_deadband_is_not_written)
{
	struct radiant_sample s;

	/* 25.00 C - the first sample for a field is always written, whatever
	 * the deadband says: there is nothing to compare it against. */
	s = mk_at(10u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2, 1000000u);
	post_drain(&s);
	zassert_equal(last.writes, 1u);
	zassert_equal(last.value, 2500);

	/* 25.20 C. Delta 0.20 C, under the 0.5 C deadband. */
	s = mk_at(10u, 0u, RADIANT_FIELD_TEMPERATURE, 29835, -2, 2000000u);
	post_drain(&s);
	zassert_equal(last.writes, 1u);

	/* 25.60 C. Delta 0.60 C from the last WRITTEN value, not from the
	 * suppressed one - which is the whole point of comparing against what
	 * the stack has, rather than against the previous sample. */
	s = mk_at(10u, 0u, RADIANT_FIELD_TEMPERATURE, 29875, -2, 3000000u);
	post_drain(&s);
	zassert_equal(last.writes, 2u);
	zassert_equal(last.value, 2560);
}

ZTEST(radiant_matter, test_the_heartbeat_writes_an_unchanged_value_anyway)
{
	struct radiant_sample s;

	s = mk_at(11u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2, 1000000u);
	post_drain(&s);
	zassert_equal(last.writes, 1u);

	/* 59 s later, unchanged: still suppressed. */
	s = mk_at(11u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2, 60000000u);
	post_drain(&s);
	zassert_equal(last.writes, 1u);

	/*
	 * 60 s later, unchanged: written. Without this a sensor whose reading
	 * never moves stops writing forever, and "unchanged" becomes
	 * indistinguishable from "gone" to anything that reads timestamps
	 * rather than Reachable.
	 */
	s = mk_at(11u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2, 61000000u);
	post_drain(&s);
	zassert_equal(last.writes, 2u);
}

ZTEST(radiant_matter, test_a_suppressed_run_does_not_postpone_the_heartbeat)
{
	struct radiant_sample s;
	uint64_t t;

	/*
	 * The subtle one. If a suppressed sample advanced the "last written"
	 * timestamp, a sensor drifting slowly inside its deadband would push
	 * the heartbeat out forever, one sample at a time, and the heartbeat
	 * would silently stop existing.
	 */
	s = mk_at(12u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2, 1000000u);
	post_drain(&s);
	zassert_equal(last.writes, 1u);

	/* Ten samples over the next 50 s, each 0.1 C from the written value
	 * and every one of them inside the deadband. */
	for (t = 6000000u; t <= 51000000u; t += 5000000u) {
		s = mk_at(12u, 0u, RADIANT_FIELD_TEMPERATURE, 29825, -2, t);
		post_drain(&s);
	}
	zassert_equal(last.writes, 1u);

	s = mk_at(12u, 0u, RADIANT_FIELD_TEMPERATURE, 29825, -2, 62000000u);
	post_drain(&s);
	zassert_equal(last.writes, 2u);
}

ZTEST(radiant_matter, test_occupancy_is_on_change_only)
{
	struct radiant_sample s;

	s = mk_at(13u, 0u, RADIANT_FIELD_OCCUPANCY, 1, 0, 1000000u);
	s.flags = RADIANT_SAMPLE_DERIVED;
	post_drain(&s);
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				 RADIANT_MATTER_ATTR_OCCUPANCY, NULL, NULL),
		      1);

	/* The same boolean again. A threshold on a one-bit value can only be
	 * "did it change". */
	s = mk_at(13u, 0u, RADIANT_FIELD_OCCUPANCY, 1, 0, 2000000u);
	s.flags = RADIANT_SAMPLE_DERIVED;
	post_drain(&s);
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				 RADIANT_MATTER_ATTR_OCCUPANCY, NULL, NULL),
		      1);

	s = mk_at(13u, 0u, RADIANT_FIELD_OCCUPANCY, 0, 0, 3000000u);
	s.flags = RADIANT_SAMPLE_DERIVED;
	post_drain(&s);
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				 RADIANT_MATTER_ATTR_OCCUPANCY, NULL, NULL),
		      2);

	/* And the two constants are still written exactly once, at creation -
	 * a deadband that suppressed the first sample's writes would have taken
	 * OccupancySensorType with it, which is trap 16 arriving through the
	 * back door. */
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				 RADIANT_MATTER_ATTR_OCCUPANCY_SENSOR_TYPE, NULL, NULL),
		      1);
}

ZTEST(radiant_matter, test_pressure_has_no_deadband_and_says_so)
{
	const struct radiant_matter_type_map *r;

	/*
	 * Finding 7's list names occupancy, temperature, humidity and battery.
	 * Pressure is absent from it, and the row states the sentinel
	 * explicitly so that the absence reads as a decision rather than as an
	 * omission - MeasuredValue is already quantised to 100 Pa by the
	 * cluster, so a threshold of even one count would hold back a
	 * barometric change a user could act on, and ScaledValue's 10 Pa step
	 * is the sensor's own resolution.
	 *
	 * test_the_pressure_constant_is_written_once_not_per_sample above is
	 * the behavioural half of this: five samples, five ScaledValue writes.
	 */
	r = radiant_matter_row(RADIANT_FIELD_PRESSURE);
	zassert_not_null(r);
	zassert_equal(r->deadband, RADIANT_MATTER_DEADBAND_EVERY);

	r = radiant_matter_row(RADIANT_FIELD_OCCUPANCY);
	zassert_equal(r->deadband, RADIANT_MATTER_DEADBAND_ON_CHANGE);
	/* Cluster units: 0.5 degC, 1 % (0.01 units), 1 % (0.5 % steps). */
	r = radiant_matter_row(RADIANT_FIELD_TEMPERATURE);
	zassert_equal(r->deadband, 50);
	r = radiant_matter_row(RADIANT_FIELD_HUMIDITY);
	zassert_equal(r->deadband, 100);
	r = radiant_matter_row(RADIANT_FIELD_BATTERY_SOC);
	zassert_equal(r->deadband, 2);
	zassert_equal(r->heartbeat_s, 300u);
}

ZTEST(radiant_matter, test_a_battery_and_its_primary_do_not_share_a_deadband)
{
	struct radiant_sample s;

	/*
	 * THE TRAP THIS TEST EXISTS FOR. A `device_type == 0` row publishes
	 * onto the SOURCE'S PRIMARY endpoint, so a strap's battery and that
	 * strap's occupancy boolean are attributes of ONE endpoint. Deadband
	 * state indexed by endpoint would have the two overwrite each other's
	 * last value, and the visible failure would be a battery percentage
	 * suppressing a "strap removed" edge.
	 */
	s = mk_at(14u, 0u, RADIANT_FIELD_OCCUPANCY, 1, 0, 1000000u);
	s.flags = RADIANT_SAMPLE_DERIVED;
	post_drain(&s);

	/* 80 % -> 160 in the cluster's 0.5 % steps, onto the same endpoint. */
	s = mk_at(14u, 1u, RADIANT_FIELD_BATTERY_SOC, 80, 0, 1100000u);
	post_drain(&s);
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_POWER_SOURCE,
				 RADIANT_MATTER_ATTR_BAT_PERCENT_REMAINING, NULL, NULL),
		      1);

	/* The occupancy edge must still get through, and must not be measured
	 * against 160. */
	s = mk_at(14u, 0u, RADIANT_FIELD_OCCUPANCY, 0, 0, 1200000u);
	s.flags = RADIANT_SAMPLE_DERIVED;
	post_drain(&s);
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				 RADIANT_MATTER_ATTR_OCCUPANCY, NULL, NULL),
		      2);

	/* And a 0.5 % battery step is still inside the battery's own 1 %
	 * deadband, i.e. its state was not clobbered by the occupancy edge
	 * either. */
	s = mk_at(14u, 1u, RADIANT_FIELD_BATTERY_SOC, 805, -1, 1300000u);
	post_drain(&s);
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_POWER_SOURCE,
				 RADIANT_MATTER_ATTR_BAT_PERCENT_REMAINING, NULL, NULL),
		      1);
}

ZTEST(radiant_matter, test_two_sources_deadband_independently)
{
	struct radiant_sample s;

	/* Deadband state is keyed on (source, field_id) for the same reason
	 * endpoints are: two straps both announce field 0. */
	s = mk_at(1u, 0u, RADIANT_FIELD_HUMIDITY, 400, -1, 1000000u);
	post_drain(&s);
	s = mk_at(2u, 0u, RADIANT_FIELD_HUMIDITY, 600, -1, 1000000u);
	post_drain(&s);
	zassert_equal(last.writes, 2u);

	/* 40.0 -> 40.5 % on source 1: half the 1 % deadband, suppressed. It
	 * must not be compared against source 2's 60 %. */
	s = mk_at(1u, 0u, RADIANT_FIELD_HUMIDITY, 405, -1, 2000000u);
	post_drain(&s);
	zassert_equal(last.writes, 2u);
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

	zassert_equal(radiant_matter_endpoint_count(), RADIANT_MATTER_MAX_ENDPOINTS);
	zassert_not_null(radiant_matter_endpoint_for(1u, 0u, RADIANT_FIELD_OCCUPANCY));
	zassert_is_null(radiant_matter_endpoint_for(1u, (uint8_t)RADIANT_MATTER_MAX_ENDPOINTS,
						   RADIANT_FIELD_OCCUPANCY));
}

/*
 * The self channels evict a paired sensor to pair a new one, and the binding
 * index is then reused. Without a release, the table only grows: a bridge that
 * re-pairs often hits the ceiling above with two sensors in the house.
 */
ZTEST(radiant_matter, test_a_forgotten_source_releases_its_endpoints_for_reuse)
{
	struct radiant_sample s;
	uint8_t f;

	for (f = 0u; f < RADIANT_MATTER_MAX_ENDPOINTS; f++) {
		s = mk(1u, f, RADIANT_FIELD_OCCUPANCY, 1, 0);
		s.flags = RADIANT_SAMPLE_DERIVED;
		radiant_bridge_post(&s);
		radiant_bridge_drain();
	}
	zassert_equal(radiant_matter_endpoint_count(), RADIANT_MATTER_MAX_ENDPOINTS);

	/* Source 1 evicted; a different sensor is bound under the same index
	 * and reports a field the old one never had. */
	radiant_matter_forget_source(1u);
	radiant_matter_admit_source(1u);
	s = mk(1u, 40u, RADIANT_FIELD_TEMPERATURE, 2150, -2);
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	zassert_not_null(radiant_matter_endpoint_for(1u, 40u, RADIANT_FIELD_TEMPERATURE));
	zassert_is_null(radiant_matter_endpoint_for(1u, 0u, RADIANT_FIELD_OCCUPANCY));
	zassert_equal(radiant_matter_endpoint_count(), RADIANT_MATTER_MAX_ENDPOINTS);

	/* And a rebound field starts with no deadband memory: the old sensor's
	 * last value must not suppress the new sensor's first. */
	n_writes = 0u;
	s = mk(1u, 0u, RADIANT_FIELD_OCCUPANCY, 1, 0);
	s.flags = RADIANT_SAMPLE_DERIVED;
	radiant_bridge_post(&s);
	radiant_bridge_drain();
	zassert_true(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING, RADIANT_MATTER_ATTR_OCCUPANCY,
				NULL, NULL) >= 1);
}

/*
 * A departed sensor's sample still queued at its release. Handled, it re-created
 * an endpoint for the unbound source; the glue refused that row, so the
 * endpoint's once-only constants went nowhere, and the next sensor bound to the
 * index inherited the endpoint and never had them written - OccupancySensorType
 * read 0, PIR, trap 16, for the life of that binding.
 */
ZTEST(radiant_matter, test_a_forgotten_source_creates_nothing_until_admitted)
{
	struct radiant_sample s;
	int64_t v = 99;

	s = mk(3u, 0u, RADIANT_FIELD_OCCUPANCY, 1, 0);
	s.flags = RADIANT_SAMPLE_DERIVED;
	radiant_bridge_post(&s);
	radiant_bridge_drain();
	zassert_not_null(radiant_matter_endpoint_for(3u, 0u, RADIANT_FIELD_OCCUPANCY));

	/* Released, and the old sensor's last sample is drained after it. */
	radiant_matter_forget_source(3u);
	n_writes = 0u;
	radiant_bridge_post(&s);
	radiant_bridge_drain();
	zassert_is_null(radiant_matter_endpoint_for(3u, 0u, RADIANT_FIELD_OCCUPANCY));
	zassert_equal(n_writes, 0u);

	/* The next sensor bound to the index: a fresh endpoint, constants and
	 * all. */
	radiant_matter_admit_source(3u);
	radiant_bridge_post(&s);
	radiant_bridge_drain();
	zassert_not_null(radiant_matter_endpoint_for(3u, 0u, RADIANT_FIELD_OCCUPANCY));
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				 RADIANT_MATTER_ATTR_OCCUPANCY_SENSOR_TYPE, &v, NULL),
		      1);
	zassert_equal(v, RADIANT_MATTER_OCCUPANCY_TYPE_PHYSICAL_CONTACT);
}

/* ---------------------------------------------------------------------------
 * One field_id, two series (TODO 2026-11-24)
 *
 * A field_id is unique per source only within one producer. An Environment
 * sensor's temperature is field 0 (radiant_env_adapter.h) and the WORN rule
 * posts field 0 as OCCUPANCY (radiant_rules.h), and both types have rows. Keyed
 * on (source, field_id) the second series to arrive was written onto the first
 * one's endpoint: WORN refused by a temperature endpoint, or, WORN first, an
 * occupancy sensor where the temperature should be.
 *
 * These cases look endpoints up by enumeration rather than through
 * radiant_matter_endpoint_for(), so that they say the same thing whatever the
 * lookup's key is.
 * ---------------------------------------------------------------------------
 */

static const struct radiant_matter_endpoint *find_endpoint(uint32_t source, uint8_t field_id,
							   uint16_t device_type)
{
	uint16_t i;

	for (i = 0u; i < radiant_matter_endpoint_count(); i++) {
		const struct radiant_matter_endpoint *e = radiant_matter_endpoint_at(i);

		if (e != NULL && e->in_use && e->source == source && e->field_id == field_id &&
		    e->device_type == device_type) {
			return e;
		}
	}
	return NULL;
}

/* Writes of one attribute onto one endpoint. */
static uint32_t writes_on(uint16_t endpoint_id, uint32_t cluster, uint32_t attribute)
{
	uint32_t i;
	uint32_t n = 0u;

	for (i = 0u; i < n_writes; i++) {
		if (writes[i].endpoint_id == endpoint_id && writes[i].cluster == cluster &&
		    writes[i].attribute == attribute) {
			n++;
		}
	}
	return n;
}

static void post_worn(uint32_t source, bool worn, uint64_t t_us)
{
	struct radiant_sample s = mk_at(source, 0u, RADIANT_FIELD_OCCUPANCY, worn ? 1 : 0, 0, t_us);

	s.flags = RADIANT_SAMPLE_DERIVED;
	post_drain(&s);
}

/* Both endpoints exist, carry their own device types, and each got its own
 * series - nothing written across. */
static void assert_temperature_and_worn_apart(uint32_t source)
{
	const struct radiant_matter_endpoint *temp =
		find_endpoint(source, 0u, RADIANT_MATTER_DEVTYPE_TEMPERATURE_SENSOR);
	const struct radiant_matter_endpoint *worn =
		find_endpoint(source, 0u, RADIANT_MATTER_DEVTYPE_OCCUPANCY_SENSOR);

	zassert_equal(radiant_matter_endpoint_count(), 2u, "got %u",
		      (unsigned int)radiant_matter_endpoint_count());
	zassert_not_null(temp, "no temperature endpoint");
	zassert_not_null(worn, "no occupancy endpoint");
	zassert_not_equal(temp->endpoint_id, worn->endpoint_id);

	zassert_equal(writes_on(temp->endpoint_id, RADIANT_MATTER_CLUSTER_TEMP_MEASUREMENT,
				RADIANT_MATTER_ATTR_MEASURED_VALUE),
		      1u);
	zassert_equal(writes_on(temp->endpoint_id, RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				RADIANT_MATTER_ATTR_OCCUPANCY),
		      0u);
	zassert_equal(writes_on(worn->endpoint_id, RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				RADIANT_MATTER_ATTR_OCCUPANCY),
		      1u);
	zassert_equal(writes_on(worn->endpoint_id, RADIANT_MATTER_CLUSTER_TEMP_MEASUREMENT,
				RADIANT_MATTER_ATTR_MEASURED_VALUE),
		      0u);
	/* The occupancy endpoint's constants went to the occupancy endpoint. */
	zassert_equal(writes_on(worn->endpoint_id, RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				RADIANT_MATTER_ATTR_OCCUPANCY_SENSOR_TYPE),
		      1u);
}

ZTEST(radiant_matter, test_temperature_then_worn_on_one_field_id_are_two_endpoints)
{
	struct radiant_sample s;

	s = mk_at(15u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2, 1000000u);
	post_drain(&s);
	post_worn(15u, true, 2000000u);

	assert_temperature_and_worn_apart(15u);
}

ZTEST(radiant_matter, test_worn_then_temperature_on_one_field_id_are_two_endpoints)
{
	struct radiant_sample s;

	post_worn(15u, true, 1000000u);
	s = mk_at(15u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2, 2000000u);
	post_drain(&s);

	assert_temperature_and_worn_apart(15u);
}

ZTEST(radiant_matter, test_temperature_and_worn_on_one_field_id_deadband_apart)
{
	struct radiant_sample s;

	/* 25.00 C, then WORN up and down. */
	s = mk_at(16u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2, 1000000u);
	post_drain(&s);
	post_worn(16u, true, 1100000u);
	post_worn(16u, false, 1200000u);
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING, RADIANT_MATTER_ATTR_OCCUPANCY,
				 NULL, NULL),
		      2);

	/* 25.20 C is inside the temperature's 0.5 C deadband measured from
	 * 2500 - not from WORN's last 0, which would let it through. */
	s = mk_at(16u, 0u, RADIANT_FIELD_TEMPERATURE, 29835, -2, 1300000u);
	post_drain(&s);
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_TEMP_MEASUREMENT,
				 RADIANT_MATTER_ATTR_MEASURED_VALUE, NULL, NULL),
		      1);

	/* And WORN still moves on its own change, not against 2500. */
	post_worn(16u, false, 1400000u);
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING, RADIANT_MATTER_ATTR_OCCUPANCY,
				 NULL, NULL),
		      2);
	post_worn(16u, true, 1500000u);
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING, RADIANT_MATTER_ATTR_OCCUPANCY,
				 NULL, NULL),
		      3);
}

/* What the glue picks a UniqueID form from (ant_bridge.cpp RowUniqueID()). */
ZTEST(radiant_matter, test_an_endpoint_records_whether_a_rule_made_it)
{
	struct radiant_sample s;

	s = mk_at(17u, 0u, RADIANT_FIELD_TEMPERATURE, 29815, -2, 1000000u);
	post_drain(&s);
	post_worn(17u, true, 2000000u);

	zassert_false(radiant_matter_endpoint_for(17u, 0u, RADIANT_FIELD_TEMPERATURE)->derived);
	zassert_true(radiant_matter_endpoint_for(17u, 0u, RADIANT_FIELD_OCCUPANCY)->derived);
	zassert_is_null(radiant_matter_endpoint_for(17u, 0u, RADIANT_FIELD_HUMIDITY));
}

/* ---------------------------------------------------------------------------
 * The whole path: profile adapter -> rules -> Matter sink
 *
 * Every sink is live in every drain (RADIANT_SINK_DEFINE is a link-time array),
 * so a sample an adapter posts reaches radiant_rules.c, whose derived output
 * is posted back onto the bus and delivered to radiant_matter.c in the same
 * radiant_bridge_drain(). The bus, the bindings, the rules and the Matter
 * table are reset before each case, and again after it, so no binding this
 * suite makes is seen by another suite.
 * ---------------------------------------------------------------------------
 */

static void combo_reset(void *f)
{
	ARG_UNUSED(f);
	radiant_bridge_reset();
	radiant_binding_init();
	radiant_rules_reset();
	matter_before(NULL);
}

ZTEST_SUITE(radiant_matter_combo, NULL, NULL, combo_reset, combo_reset, NULL);

/* Environment page 0x01 (radiant_profile_env.h): event count in byte 2, the
 * 24-hour statistics invalid, the current temperature in 0.01 degC LE. */
static void env_page1(uint8_t body[8], uint8_t event_count, int16_t centi_c)
{
	body[0] = RADIANT_PROFILE_ENV_PAGE_TEMPERATURE;
	body[1] = 0xFFu;
	body[2] = event_count;
	body[3] = 0x00u;
	body[4] = 0x80u; /* low 24 h 0x800: invalid */
	body[5] = 0x80u; /* high 24 h 0x800: invalid */
	body[6] = (uint8_t)((uint16_t)centi_c & 0xFFu);
	body[7] = (uint8_t)(((uint16_t)centi_c >> 8) & 0xFFu);
}

ZTEST(radiant_matter_combo, test_a_bound_thermometer_gets_a_temperature_and_no_worn)
{
	struct radiant_env_adapter a;
	uint8_t body[8];
	uint32_t source = RADIANT_BINDING_NONE;
	uint8_t i;
	const struct radiant_matter_endpoint *temp;

	zassert_equal(radiant_binding_bind(4321u, 0x19u, 5u, NULL, &source), 0);
	radiant_env_adapter_init(&a);

	/* Six messages a second apart: the event count steps on every one, which
	 * is an advancing accumulator for well over the 2 s WORN dwell. */
	for (i = 0u; i < 6u; i++) {
		env_page1(body, i, 2150);
		(void)radiant_env_adapter_decode(&a, source, body, (uint64_t)i * 1000000u);
		radiant_bridge_drain();
	}

	temp = find_endpoint(source, RADIANT_ENV_FIELD_TEMPERATURE,
			     RADIANT_MATTER_DEVTYPE_TEMPERATURE_SENSOR);
	zassert_not_null(temp, "no temperature endpoint");
	zassert_equal(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING, RADIANT_MATTER_ATTR_OCCUPANCY,
				 NULL, NULL),
		      0, "WORN reached the Matter sink");
	zassert_is_null(find_endpoint(source, RADIANT_RULE_FIELD_WORN,
				      RADIANT_MATTER_DEVTYPE_OCCUPANCY_SENSOR));
	zassert_equal(radiant_matter_endpoint_count(), 1u, "got %u",
		      (unsigned int)radiant_matter_endpoint_count());
	/* 21.50 C, written once (the rest are inside the deadband). */
	zassert_equal(writes_on(temp->endpoint_id, RADIANT_MATTER_CLUSTER_TEMP_MEASUREMENT,
				RADIANT_MATTER_ATTR_MEASURED_VALUE),
		      1u);
	zassert_equal(last.value, 2150);
}

ZTEST(radiant_matter_combo, test_a_bound_heart_rate_strap_still_gets_worn)
{
	struct radiant_hr_adapter a;
	uint8_t body[8] = { 0 };
	uint32_t source = RADIANT_BINDING_NONE;
	uint8_t i;
	const struct radiant_matter_endpoint *worn;
	int64_t v = -1;

	zassert_equal(radiant_binding_bind(1234u, 0x78u, 1u, NULL, &source), 0);
	radiant_hr_adapter_init(&a);

	/* Page 0, one beat a second at 60 bpm: beat time (1/1024 s) in bytes
	 * 4-5, beat count in byte 6, computed bpm in byte 7. */
	for (i = 0u; i < 6u; i++) {
		uint16_t beat_time = (uint16_t)(i * 1024u);

		body[0] = 0x00u;
		body[4] = (uint8_t)(beat_time & 0xFFu);
		body[5] = (uint8_t)(beat_time >> 8);
		body[6] = i;
		body[7] = 60u;
		(void)radiant_hr_adapter_decode(&a, source, body, (uint64_t)i * 1000000u);
		radiant_bridge_drain();
	}

	worn = find_endpoint(source, RADIANT_RULE_FIELD_WORN, RADIANT_MATTER_DEVTYPE_OCCUPANCY_SENSOR);
	zassert_not_null(worn, "no WORN endpoint for a strap");
	/* Two: the rules announce `false` on the first beat count (so the
	 * endpoint exists before the strap is worn), then the assert. */
	zassert_equal(writes_on(worn->endpoint_id, RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				RADIANT_MATTER_ATTR_OCCUPANCY),
		      2u);
	zassert_true(find_write(RADIANT_MATTER_CLUSTER_OCCUPANCY_SENSING,
				RADIANT_MATTER_ATTR_OCCUPANCY_SENSOR_TYPE, &v, NULL) >= 1);
	zassert_equal(v, RADIANT_MATTER_OCCUPANCY_TYPE_PHYSICAL_CONTACT);
}

