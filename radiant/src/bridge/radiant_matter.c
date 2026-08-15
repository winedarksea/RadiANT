/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * P7 - the Matter plane. See radiant_matter.h for what this is and is not.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "radiant_matter.h"

/* Fixed level, no Kconfig symbol - nothing in src/bridge carries its own
 * configuration. */
LOG_MODULE_REGISTER(radiant_matter, LOG_LEVEL_INF);

/* ---------------------------------------------------------------------------
 * The table - docs/radiant-bridge.md section 8.1, one row per line of it
 * ---------------------------------------------------------------------------
 */

static const struct radiant_matter_type_map type_map[] = {
	{
		/* Occupancy: a stretch (section 8.2), but it's the row that
		 * surfaces the four derived booleans of section 6. */
		.field_type = RADIANT_FIELD_OCCUPANCY,
		.device_type = MATTER_DEVTYPE_OCCUPANCY_SENSOR,
		.cluster = MATTER_CLUSTER_OCCUPANCY_SENSING,
		.attribute = MATTER_ATTR_OCCUPANCY,
		.mul = 1, .div = 1, .offset = 0,
		/*
		 * NOT PIR. Zero-initialised, OccupancySensorType means PIR, and
		 * home-assistant/core#164839 proposes remapping PIR endpoints
		 * from `device_class: occupancy` to `motion`. "Strap is worn"
		 * and "bike in use" are not motion; a controller release could
		 * relabel all four booleans without anything here changing.
		 * Physical contact is the honest one of the four the enum
		 * offers - the signal is that a thing is in contact with a
		 * person or being driven by one.
		 */
		.n_extra = 2u,
		.extra = {
			{ .attribute = MATTER_ATTR_OCCUPANCY_SENSOR_TYPE,
			  .mul = 0, .div = 1,
			  .offset = MATTER_OCCUPANCY_TYPE_PHYSICAL_CONTACT },
			{ .attribute = MATTER_ATTR_OCCUPANCY_SENSOR_BITMAP,
			  .mul = 0, .div = 1,
			  .offset = MATTER_OCCUPANCY_BITMAP_PHYSICAL_CONTACT },
		},
	},
	{
		/* The only row with an offset (section 8.1a): canonical unit
		 * is kelvin, Matter wants 0.01degC, so K*100-27315 is a scale
		 * and a shift, not a pure decimal scale. */
		.field_type = RADIANT_FIELD_TEMPERATURE,
		.device_type = MATTER_DEVTYPE_TEMPERATURE_SENSOR,
		.cluster = MATTER_CLUSTER_TEMP_MEASUREMENT,
		.attribute = MATTER_ATTR_MEASURED_VALUE,
		.mul = 100, .div = 1, .offset = -27315,
	},
	{
		/* Percent to 0.01 percent. */
		.field_type = RADIANT_FIELD_HUMIDITY,
		.device_type = MATTER_DEVTYPE_HUMIDITY_SENSOR,
		.cluster = MATTER_CLUSTER_REL_HUMIDITY,
		.attribute = MATTER_ATTR_MEASURED_VALUE,
		.mul = 100, .div = 1, .offset = 0,
	},
	{
		/*
		 * Barometric pressure, from common page 84 subpage 2 (package
		 * C). THREE ATTRIBUTES, and the reason is a resolution trap
		 * rather than thoroughness:
		 *
		 * Pressure Measurement's mandatory `MeasuredValue` is an int16s
		 * in WHOLE kPa (pressure-measurement-cluster.xml:39). Sea-level
		 * pressure runs about 98-105 kPa, so a barometer reported that
		 * way has roughly seven distinguishable states and every
		 * weather trend a user might care about - a 0.3 kPa front
		 * passing through - is quantised out of existence.
		 *
		 * `ScaledValue` with `Scale` is the cluster's own answer, at
		 * 10^Scale x kPa. Scale 2 gives 10 Pa per step and tops out at
		 * 327.67 kPa, which covers every terrestrial pressure with room
		 * to spare. But it is OPTIONAL and `MeasuredValue` is not, so
		 * serving only the useful one is a non-conformant endpoint and
		 * serving only the mandatory one is a useless entity. Both, and
		 * the constant that makes the second readable.
		 *
		 * Pa -> kPa is /1000; Pa -> 0.01 kPa is /10.
		 */
		.field_type = RADIANT_FIELD_PRESSURE,
		.device_type = MATTER_DEVTYPE_PRESSURE_SENSOR,
		.cluster = MATTER_CLUSTER_PRESSURE_MEASUREMENT,
		.attribute = MATTER_ATTR_MEASURED_VALUE,
		.mul = 1, .div = 1000, .offset = 0,
		.n_extra = 2u,
		.extra = {
			{ .attribute = MATTER_ATTR_SCALED_VALUE,
			  .mul = 1, .div = 10, .offset = 0 },
			{ .attribute = MATTER_ATTR_SCALE,
			  .mul = 0, .div = 1,
			  .offset = MATTER_PRESSURE_SCALE },
		},
	},
	{
		/* device_type 0: cluster only, on the binding's own endpoint.
		 * A battery is a property, not a device - its own endpoint
		 * would be a phantom entity. endpoint_get_or_add() enforces
		 * that; it used to only say it. */
		.field_type = RADIANT_FIELD_BATTERY_SOC,
		.device_type = 0u,
		.cluster = MATTER_CLUSTER_POWER_SOURCE,
		.attribute = MATTER_ATTR_BAT_PERCENT_REMAINING,
		.mul = 2, .div = 1, .offset = 0,
	},
	/* RADIANT_FIELD_HEART_RATE (0x26) is absent on purpose (section 8.1):
	 * both Matter candidates - a mislabelled Flow Measurement, or an MEI
	 * cluster invisible to most controllers - are outside tier 1. See
	 * section 8.3/8.3b before adding it. */
};

const struct radiant_matter_type_map *radiant_matter_row(uint8_t field_type)
{
	size_t i;

	for (i = 0u; i < ARRAY_SIZE(type_map); i++) {
		if (type_map[i].field_type == field_type) {
			return &type_map[i];
		}
	}
	return NULL;
}

/* ---------------------------------------------------------------------------
 * Conversion
 * ---------------------------------------------------------------------------
 */

/* 10^n for the exponents a sample can carry. The vocabulary's `exp` is an
 * int8_t, but a value needing more than 10^18 does not fit int64 anyway, so
 * anything past this table is refused rather than approximated. */
static const int64_t pow10_tab[] = {
	1LL, 10LL, 100LL, 1000LL, 10000LL, 100000LL, 1000000LL,
	10000000LL, 100000000LL, 1000000000LL, 10000000000LL,
	100000000000LL, 1000000000000LL, 10000000000000LL,
	100000000000000LL, 1000000000000000LL, 10000000000000000LL,
	100000000000000000LL, 1000000000000000000LL,
};

static bool mul_ok(int64_t a, int64_t b)
{
	if (a == 0 || b == 0) {
		return true;
	}
	if (a > 0) {
		return (b > 0) ? (a <= INT64_MAX / b) : (b >= INT64_MIN / a);
	}
	return (b > 0) ? (a >= INT64_MIN / b) : (a >= INT64_MAX / b);
}

/*
 * The arithmetic, against an explicit (mul, div, offset) rather than against
 * a row. Split out when the pressure row grew a second scaled attribute: the
 * primary and the extras must round identically, and two copies of a
 * round-half-away-from-zero would not stay identical.
 */
static bool convert_scaled(const struct radiant_sample *s, int32_t mul,
			   int32_t div, int32_t offset, int64_t *out)
{
	int64_t num;
	int64_t den;
	int64_t v;
	int e;

	if (s == NULL || out == NULL || div == 0) {
		return false;
	}

	/*
	 * One division, at the end. value_SI = raw * 10^exp, cluster value =
	 * value_SI * mul / div + offset; doing that literally loses the
	 * fraction twice for a negative exponent. So a negative exponent is
	 * folded into the denominator instead, and division happens once.
	 */
	num = s->raw;
	den = div;

	e = (int)s->exp;
	if (e >= 0) {
		if ((size_t)e >= ARRAY_SIZE(pow10_tab)) {
			return false;
		}
		if (!mul_ok(num, pow10_tab[e])) {
			return false;
		}
		num *= pow10_tab[e];
	} else {
		if ((size_t)(-e) >= ARRAY_SIZE(pow10_tab)) {
			return false;
		}
		if (!mul_ok(den, pow10_tab[-e])) {
			return false;
		}
		den *= pow10_tab[-e];
	}

	if (!mul_ok(num, mul)) {
		return false;
	}
	num *= mul;

	/* Round half away from zero, not truncate: C's toward-zero integer
	 * division would bias asymmetrically around 0 (e.g. the freezing
	 * point for temperature). */
	if ((num >= 0) == (den > 0)) {
		v = (num + den / 2) / den;
	} else {
		v = (num - den / 2) / den;
	}

	/* Must branch on offset's sign: `v > INT64_MAX - offset` is only a
	 * valid bounds check for offset > 0 - for temperature's -27315,
	 * INT64_MAX - (-27315) overflows before the comparison runs. Each
	 * bound is tested only in the branch where it's the reachable one. */
	if (offset > 0) {
		if (v > INT64_MAX - offset) {
			return false;
		}
	} else if (offset < 0) {
		if (v < INT64_MIN - offset) {
			return false;
		}
	}
	*out = v + offset;
	return true;
}

bool radiant_matter_convert(const struct radiant_sample *s, int64_t *out)
{
	const struct radiant_matter_type_map *row;

	if (s == NULL) {
		return false;
	}
	row = radiant_matter_row(s->field_type);
	if (row == NULL) {
		return false;
	}
	return convert_scaled(s, row->mul, row->div, row->offset, out);
}

bool radiant_matter_convert_with(const struct radiant_sample *s,
				 const struct radiant_matter_attr_conv *c,
				 int64_t *out)
{
	if (c == NULL || c->mul == 0) {
		/* A constant is not a conversion of anything - answering with
		 * its value here would make `convert_with` mean two things. */
		return false;
	}
	return convert_scaled(s, c->mul, c->div, c->offset, out);
}

/* ---------------------------------------------------------------------------
 * The endpoint table
 * ---------------------------------------------------------------------------
 */

/*
 * Endpoint 0 is the Matter root node, so assignment starts at 1.
 *
 * On a real bridge CHIP endpoint 1 is the Aggregator, so this 1 and that 1
 * are not the same 1 (trap 6). Resolved in the glue by mapping onto the first
 * dynamic endpoint above the fixed ones, deliberately NOT by renumbering
 * here - see the note on struct radiant_matter_endpoint in the header for
 * why a data model that knows one stack's fixed-endpoint layout has stopped
 * being a data model.
 */
#define MATTER_ENDPOINT_FIRST 1u

static struct radiant_matter_endpoint endpoints[RADIANT_MATTER_MAX_ENDPOINTS];
static uint16_t endpoint_used;

void radiant_matter_reset(void)
{
	memset(endpoints, 0, sizeof(endpoints));
	endpoint_used = 0u;
}

uint16_t radiant_matter_endpoint_count(void)
{
	return endpoint_used;
}

const struct radiant_matter_endpoint *radiant_matter_endpoint_at(uint16_t i)
{
	if (i >= endpoint_used) {
		return NULL;
	}
	return &endpoints[i];
}

const struct radiant_matter_endpoint *radiant_matter_endpoint_for(uint32_t source,
								  uint8_t field_id)
{
	uint16_t i;

	for (i = 0u; i < endpoint_used; i++) {
		if (endpoints[i].in_use && endpoints[i].source == source &&
		    endpoints[i].field_id == field_id) {
			return &endpoints[i];
		}
	}
	return NULL;
}

/*
 * The source's PRIMARY endpoint: the first one instantiated for it that
 * carries a device type of its own. What a `device_type == 0` row attaches
 * its cluster to. NULL if the source has no device-typed endpoint yet.
 */
static const struct radiant_matter_endpoint *endpoint_primary_for(uint32_t source)
{
	uint16_t i;

	for (i = 0u; i < endpoint_used; i++) {
		if (endpoints[i].in_use && endpoints[i].source == source &&
		    endpoints[i].device_type != 0u) {
			return &endpoints[i];
		}
	}
	return NULL;
}

/*
 * Instantiates on first sight of a (source, field_id) whose type has a row.
 * Keyed on field_id, not field_type (section 8.1): the four derived booleans
 * of section 6 are all type 0x02, and keying on type would collapse them
 * into one contested endpoint instead of four instances of one row.
 */
static const struct radiant_matter_endpoint *endpoint_get_or_add(
	const struct radiant_sample *s, const struct radiant_matter_type_map *row)
{
	const struct radiant_matter_endpoint *found;
	struct radiant_matter_endpoint *e;
	uint8_t i;

	found = radiant_matter_endpoint_for(s->source, s->field_id);
	if (found != NULL) {
		return found;
	}

	/*
	 * A CLUSTER-ONLY ROW GETS NO ENDPOINT OF ITS OWN, which is what the
	 * battery row's comment and radiant_matter.h have claimed since P7 and
	 * what this function did not do: it called through unconditionally, so
	 * a battery field allocated its own endpoint with device_type 0. In
	 * Matter that is a bridged node with no device type and a single Power
	 * Source cluster - a phantom entity in every controller, sitting next
	 * to the real sensor and reporting its battery as though it were a
	 * separate thing in the house. Harmless in a table nobody rendered;
	 * not harmless once a CHIP stack is behind the seam.
	 *
	 * Declining when there is no primary yet is deliberate and is not a
	 * drop worth warning about at WRN: it means the battery page arrived
	 * before any measurement did, which is ordinary for a sensor whose
	 * common page 82 leads its first data page. The next battery sample
	 * after the primary exists lands on it.
	 */
	if (row->device_type == 0u) {
		const struct radiant_matter_endpoint *primary =
			endpoint_primary_for(s->source);

		if (primary == NULL) {
			LOG_DBG("source %u field %u: cluster-only row with no "
				"primary endpoint yet - deferred, not dropped",
				(unsigned int)s->source,
				(unsigned int)s->field_id);
		}
		return primary;
	}

	if (endpoint_used >= ARRAY_SIZE(endpoints)) {
		/* Counted as a log line rather than silently dropped: a
		 * household that outgrows the table shows FEWER entities than
		 * it should, which looks like a decoder fault and is not one. */
		LOG_WRN("no endpoint left for source %u field %u (max %u) - "
			"this sensor will not appear in Matter",
			(unsigned int)s->source, (unsigned int)s->field_id,
			(unsigned int)ARRAY_SIZE(endpoints));
		return NULL;
	}

	e = &endpoints[endpoint_used];
	e->endpoint_id = (uint16_t)(MATTER_ENDPOINT_FIRST + endpoint_used);
	e->source = s->source;
	e->field_id = s->field_id;
	e->field_type = s->field_type;
	e->device_type = row->device_type;
	e->in_use = true;
	endpoint_used++;

	LOG_INF("endpoint %u: source %u field %u type 0x%02x -> devtype 0x%04x "
		"cluster 0x%04x", e->endpoint_id, (unsigned int)e->source,
		(unsigned int)e->field_id, e->field_type, e->device_type,
		(unsigned int)row->cluster);

	/*
	 * The row's CONSTANT attributes, written exactly once, here, at
	 * creation. There is no other moment they could be written: on a
	 * dynamic endpoint every attribute is external storage
	 * (attribute-storage.h:73-77 ORs EXTERNAL_STORAGE into
	 * DECLARE_DYNAMIC_ATTRIBUTE unconditionally), so the application owns
	 * every value and a constant nobody writes reads back as zero - which
	 * for OccupancySensorType means PIR, which is trap 16.
	 */
	for (i = 0u; i < row->n_extra; i++) {
		if (row->extra[i].mul != 0) {
			continue; /* a conversion; written per sample below */
		}
		radiant_matter_attr_write(e->endpoint_id, row->cluster,
					  row->extra[i].attribute,
					  (int64_t)row->extra[i].offset);
	}
	return e;
}

/* ---------------------------------------------------------------------------
 * The sink
 * ---------------------------------------------------------------------------
 */

__weak void radiant_matter_attr_write(uint16_t endpoint_id, uint32_t cluster,
				      uint32_t attribute, int64_t value)
{
	ARG_UNUSED(endpoint_id);
	ARG_UNUSED(cluster);
	ARG_UNUSED(attribute);
	ARG_UNUSED(value);
}

static bool matter_want(const struct radiant_sample *s)
{
	/* Real filter: the bus carries every field, the Matter plane has
	 * rows for four of them. Declining here keeps heart rate, power,
	 * cadence etc. out of publish() (section 8.1). */
	return radiant_matter_row(s->field_type) != NULL;
}

static void matter_publish(const struct radiant_sample *s)
{
	const struct radiant_matter_type_map *row;
	const struct radiant_matter_endpoint *e;
	int64_t value;
	uint8_t i;

	row = radiant_matter_row(s->field_type);
	if (row == NULL) {
		return;
	}
	if (!radiant_matter_convert(s, &value)) {
		LOG_WRN("source %u field %u: %lld x 10^%d does not fit "
			"cluster 0x%04x", (unsigned int)s->source,
			(unsigned int)s->field_id, (long long)s->raw,
			(int)s->exp, (unsigned int)row->cluster);
		return;
	}
	e = endpoint_get_or_add(s, row);
	if (e == NULL) {
		return;
	}

	/* A stale sample is still written, deliberately: these clusters have
	 * no "old value" flag, and staleness is a node-level (reachability)
	 * concern, not this table's. */
	radiant_matter_attr_write(e->endpoint_id, row->cluster, row->attribute,
				  value);

	/*
	 * The row's non-constant extras, from the same sample. Only pressure
	 * has one today (ScaledValue beside the mandatory whole-kPa
	 * MeasuredValue). A refused extra is skipped and does NOT suppress the
	 * primary: the primary's narrower range is the one that fits, so an
	 * extra that overflows is the more-precise attribute declining, not
	 * the reading failing.
	 */
	for (i = 0u; i < row->n_extra; i++) {
		int64_t xv;

		if (row->extra[i].mul == 0) {
			continue; /* constant; written once at creation */
		}
		if (!radiant_matter_convert_with(s, &row->extra[i], &xv)) {
			continue;
		}
		radiant_matter_attr_write(e->endpoint_id, row->cluster,
					  row->extra[i].attribute, xv);
	}
}

RADIANT_SINK_DEFINE(radiant_matter_sink, matter_want, matter_publish, NULL);
