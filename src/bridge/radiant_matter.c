/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * P7 - the Matter plane. See radiant_matter.h for what this is and is not.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "radiant_matter.h"

/* A fixed level and no Kconfig symbol, matching radiant_rules.c and
 * radiant_binding.c beside it: nothing in src/bridge carries configuration of
 * its own, and a table that is the same on every build has nothing to
 * configure. */
LOG_MODULE_REGISTER(radiant_matter, LOG_LEVEL_INF);

/* ---------------------------------------------------------------------------
 * The table - docs/radiant-bridge.md section 8.1, one row per line of it
 * ---------------------------------------------------------------------------
 */

static const struct radiant_matter_type_map type_map[] = {
	{
		/* Occupancy. Section 8.2 calls this "a stretch" and it is, but
		 * it is the row that makes the four derived booleans of
		 * section 6 appear in every controller with nothing installed. */
		.field_type = RADIANT_FIELD_OCCUPANCY,
		.device_type = MATTER_DEVTYPE_OCCUPANCY_SENSOR,
		.cluster = MATTER_CLUSTER_OCCUPANCY_SENSING,
		.attribute = MATTER_ATTR_OCCUPANCY,
		.mul = 1, .div = 1, .offset = 0,
	},
	{
		/*
		 * THE ONLY ROW WITH AN OFFSET, and section 8.1a says so in as
		 * many words. The vocabulary's canonical unit is KELVIN and
		 * Matter wants 0.01 degrees Celsius: K x 100 - 27315, a scale
		 * AND a shift. Every other row in this table is a pure decimal
		 * scale, which is why `exp` alone was enough everywhere else
		 * and is not enough here.
		 */
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
		 * device_type 0 - "cluster only, on the binding's own
		 * endpoint". A battery is not a device; it is a property of
		 * one, and giving it an endpoint of its own would put a
		 * phantom entity in every controller.
		 */
		.field_type = RADIANT_FIELD_BATTERY_SOC,
		.device_type = 0u,
		.cluster = MATTER_CLUSTER_POWER_SOURCE,
		.attribute = MATTER_ATTR_BAT_PERCENT_REMAINING,
		.mul = 2, .div = 1, .offset = 0,
	},
	/*
	 * AND THAT IS THE WHOLE TABLE. RADIANT_FIELD_HEART_RATE (0x26) is
	 * ABSENT ON PURPOSE - section 8.1's table spells the entry out as
	 * "none, and section 1 is the whole reason". Anyone adding it here
	 * should read section 8.3 and 8.3b first: both candidates are outside
	 * tier 1, one is a deliberately mislabelled Flow Measurement and the
	 * other is invisible until somebody else's controller learns an MEI
	 * cluster. Neither belongs in a table whose promise is "appears
	 * correctly named on a QR scan with nothing installed".
	 */
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

bool radiant_matter_convert(const struct radiant_sample *s, int64_t *out)
{
	const struct radiant_matter_type_map *row;
	int64_t num;
	int64_t den;
	int64_t v;
	int e;

	if (s == NULL || out == NULL) {
		return false;
	}
	row = radiant_matter_row(s->field_type);
	if (row == NULL || row->div == 0) {
		return false;
	}

	/*
	 * ONE DIVISION, AT THE END, AND THAT IS THE WHOLE REASON THIS IS NOT
	 * THREE LINES.
	 *
	 * value_SI = raw * 10^exp, and the cluster value is
	 * value_SI * mul / div + offset. Doing that as written loses the
	 * fraction twice for a negative exponent - once turning raw into
	 * value_SI and again applying mul/div - and a sample carrying
	 * 29815 x 10^-2 K (298.15 K, a perfectly ordinary room) would land two
	 * hundredths of a degree out before the offset is even applied.
	 *
	 * So the negative exponent is folded into the DENOMINATOR and the
	 * division happens once.
	 */
	num = s->raw;
	den = row->div;

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

	if (!mul_ok(num, row->mul)) {
		return false;
	}
	num *= row->mul;

	/*
	 * ROUND HALF AWAY FROM ZERO, not truncate. C integer division rounds
	 * toward zero, which biases every converted value toward 0 degrees and
	 * makes the bias asymmetric across the freezing point - the same
	 * reading would round down above it and up below it. Half a hundredth
	 * of a degree is not a lot; a discontinuity at one particular
	 * temperature is the kind of thing that gets found much later.
	 */
	if ((num >= 0) == (den > 0)) {
		v = (num + den / 2) / den;
	} else {
		v = (num - den / 2) / den;
	}

	/*
	 * The offset guard has to branch on the SIGN of the offset, and the
	 * version that did not is why every temperature sample was refused.
	 *
	 * `v > INT64_MAX - offset` reads like a bounds check and is one only for
	 * a positive offset. Temperature's offset is -27315, so the subtrahend is
	 * negative and INT64_MAX - (-27315) overflows before the comparison ever
	 * happens - undefined, and in practice a large negative number that every
	 * v is greater than. The single row in this table with an offset was
	 * therefore the single row the guard rejected outright, which is the most
	 * unhelpful possible distribution of the bug: the three pure-scale rows
	 * all passed and the arithmetic they exercise is the arithmetic that
	 * cannot go wrong.
	 *
	 * Only one of the two bounds can be at risk for a given sign, so each is
	 * tested in the branch where it is the reachable one.
	 */
	if (row->offset > 0) {
		if (v > INT64_MAX - row->offset) {
			return false;
		}
	} else if (row->offset < 0) {
		if (v < INT64_MIN - row->offset) {
			return false;
		}
	}
	*out = v + row->offset;
	return true;
}

/* ---------------------------------------------------------------------------
 * The endpoint table
 * ---------------------------------------------------------------------------
 */

/*
 * Endpoint 0 is the Matter root node and can never be ours, so assignment
 * starts at 1. Section 8.1's worked example - "a household with one strap and
 * one trainer shows four entities" - is the size this is scaled for, with room
 * for the section 8.1a temperature and humidity rows on top.
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
 * Instantiate on first sight of a (source, field_id) whose type has a row.
 *
 * KEYED ON field_id AND NOT ON field_type, which is the entire point of
 * section 8.1's rewrite. The four derived booleans of section 6 - worn, bike in
 * use, at rest, training - are ALL type 0x02. Keyed on type they would collapse
 * into one endpoint that four different rules fought over; keyed on field_id
 * they are four instances of one table row, which is what "an endpoint is
 * instantiated per announced field, not per binding kind" means.
 */
static const struct radiant_matter_endpoint *endpoint_get_or_add(
	const struct radiant_sample *s, const struct radiant_matter_type_map *row)
{
	const struct radiant_matter_endpoint *found;
	struct radiant_matter_endpoint *e;

	found = radiant_matter_endpoint_for(s->source, s->field_id);
	if (found != NULL) {
		return found;
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
	/*
	 * First refusal, and it is a real filter rather than a formality: the
	 * bus carries every field of every binding, and the Matter plane has
	 * rows for four of them. Declining here keeps heart rate, power,
	 * cadence and the whole accumulating half of the vocabulary out of
	 * publish() entirely - which is section 8.1's "a field the Matter
	 * plane declines and the MQTT plane carries", enforced at the one
	 * place that can enforce it.
	 */
	return radiant_matter_row(s->field_type) != NULL;
}

static void matter_publish(const struct radiant_sample *s)
{
	const struct radiant_matter_type_map *row;
	const struct radiant_matter_endpoint *e;
	int64_t value;

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

	/*
	 * A STALE SAMPLE IS STILL WRITTEN, and that is deliberate. Matter has
	 * no "this value is old" on these clusters - the alternative to writing
	 * it is leaving the last good value in place, which is
	 * indistinguishable from a live sensor reporting the same thing. The
	 * staleness belongs in reachability, which is a node-level property and
	 * not this table's business.
	 */
	radiant_matter_attr_write(e->endpoint_id, row->cluster, row->attribute,
				  value);
}

RADIANT_SINK_DEFINE(radiant_matter_sink, matter_want, matter_publish, NULL);
