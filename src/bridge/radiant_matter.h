/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * P7 - the Matter plane's endpoint model and type map.
 * docs/radiant-bridge.md section 8.1, transcribed rather than redesigned.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IS, AND WHAT IT DELIBERATELY IS NOT
 * ---------------------------------------------------------------------------
 *
 * This is the DATA MODEL: the table keyed on the section 7 vocabulary type, the
 * unit conversion into each cluster's unit, and the rule that instantiates one
 * endpoint per announced field. It is a `radiant_sink` like any other, so it
 * reaches the samples through P5's bus and learns about bindings through P6's
 * `binding_changed` - no new seam, and no switch statement per profile.
 *
 * It is NOT the Matter stack. There is no CHIP, no commissioning, no fabric and
 * no QR code here, and the reason is not that they are hard: it is that
 * section 8.1's content - which field becomes which cluster, and what the
 * number is once it gets there - is completely independent of them, and is the
 * part that can be wrong in ways a demo would hide. `matter_attr_write()` below
 * is the seam a real stack plugs into. The tests drive it directly.
 *
 * ---------------------------------------------------------------------------
 * THE ONE ROW THAT IS NOT THERE
 * ---------------------------------------------------------------------------
 *
 * `0x26` heart rate has NO ROW, and that is section 8.1's own table saying
 * "none, and section 1 is the whole reason". Matter has no heart-rate cluster;
 * the two candidates are a deliberately mislabelled Flow Measurement (section
 * 8.3b) and an MEI cluster nothing else understands (section 8.3), and both are
 * explicitly outside tier 1. A missing row is NOT an error - section 8.1 again:
 * "A type with no row is not an error - it is a field the Matter plane declines
 * and the MQTT plane carries."
 *
 * So `radiant_matter_row()` returning NULL is a normal, expected answer, and a
 * caller that treats it as a failure has misread the design.
 */

#ifndef RADIANT_MATTER_H_
#define RADIANT_MATTER_H_

#include <stdbool.h>
#include <stdint.h>

#include "radiant_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Section 8.1's struct, verbatim.
 *
 * mul/div/offset take a value in the vocabulary's CANONICAL SI unit and produce
 * the cluster's unit. Section 8.1a: "temperature is the only row with an
 * offset" - kelvin to 0.01 degrees Celsius is a scale AND a shift, `K x 100 -
 * 27315`, where every other row is a pure decimal scale. That asymmetry is the
 * reason `offset` exists at all, and the reason it is applied AFTER the scale.
 */
struct radiant_matter_type_map {
	uint8_t  field_type;   /* the section 7 vocabulary */
	uint16_t device_type;  /* Matter Device Library, 0 = cluster only */
	uint32_t cluster;
	uint32_t attribute;
	int32_t  mul;
	int32_t  div;
	int32_t  offset;
};

/* Matter cluster and device-type identifiers used by the table. Named rather
 * than left as bare hex in the rows, because a wrong constant here is invisible
 * until a controller shows the wrong icon. */
#define MATTER_DEVTYPE_OCCUPANCY_SENSOR   0x0107u
#define MATTER_DEVTYPE_TEMPERATURE_SENSOR 0x0302u
#define MATTER_DEVTYPE_HUMIDITY_SENSOR    0x0307u

#define MATTER_CLUSTER_TEMP_MEASUREMENT     0x0402u
#define MATTER_CLUSTER_REL_HUMIDITY         0x0405u
#define MATTER_CLUSTER_OCCUPANCY_SENSING    0x0406u
#define MATTER_CLUSTER_POWER_SOURCE         0x002Fu

#define MATTER_ATTR_MEASURED_VALUE          0x0000u
#define MATTER_ATTR_OCCUPANCY               0x0000u
#define MATTER_ATTR_BAT_PERCENT_REMAINING   0x000Cu

/*
 * The row for a vocabulary type, or NULL if the Matter plane declines it.
 * NULL is a normal answer - see the header comment.
 */
const struct radiant_matter_type_map *radiant_matter_row(uint8_t field_type);

/*
 * Convert a sample into its cluster's unit.
 *
 * Returns false when the type has no row, or when the arithmetic would not fit
 * the attribute - a saturating cast would put a plausible wrong number in front
 * of a user, which is worse than an absent one.
 */
bool radiant_matter_convert(const struct radiant_sample *s, int64_t *out);

/*
 * How many endpoints are currently instantiated, and what is on each.
 *
 * Section 8.1: "An endpoint is instantiated per announced field, not per
 * binding kind" - so a power meter binding instantiates one, a heart-rate
 * binding three, and the four derived booleans of section 6 are four INSTANCES
 * OF ONE ROW (all type 0x02, distinguished by field_id) rather than four cases
 * in a switch.
 */
/*
 * Section 8.1's worked example is "a household with one strap and one trainer
 * shows four entities", plus the section 8.1a temperature and humidity rows and
 * a battery row per binding. 16 is that with room, and it is a compile-time
 * constant rather than a Kconfig symbol because nothing else in src/bridge
 * carries configuration either. Overrunning it is a logged warning and a sensor
 * that does not appear - never a silent drop; see endpoint_get_or_add().
 */
#define RADIANT_MATTER_MAX_ENDPOINTS 16u

struct radiant_matter_endpoint {
	uint16_t endpoint_id;
	uint32_t source;      /* binding index */
	uint8_t  field_id;    /* stable within source */
	uint8_t  field_type;
	uint16_t device_type;
	bool     in_use;
};

uint16_t radiant_matter_endpoint_count(void);
const struct radiant_matter_endpoint *radiant_matter_endpoint_at(uint16_t i);

/*
 * Find the endpoint carrying one (source, field_id), or NULL. Endpoint numbers
 * are ASSIGNED, not derived: section 8.1's earlier draft used "endpoint numbers
 * as fixed identities" and that is exactly the per-binding-kind hardcoding it
 * replaced.
 */
const struct radiant_matter_endpoint *radiant_matter_endpoint_for(uint32_t source,
								  uint8_t field_id);

/*
 * THE SEAM A REAL MATTER STACK PLUGS INTO.
 *
 * Weak, so the tests and a stack-free build get a no-op that still exercises
 * every line above it, and an image with CHIP in it overrides this with the
 * real attribute write. Deliberately narrow: an endpoint, a cluster, an
 * attribute and an integer is the entire vocabulary the type map produces.
 */
void radiant_matter_attr_write(uint16_t endpoint_id, uint32_t cluster,
			       uint32_t attribute, int64_t value);

/* Drop every endpoint. Tests, and a factory reset. */
void radiant_matter_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_MATTER_H_ */
