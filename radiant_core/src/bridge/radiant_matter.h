/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * P7 - the Matter plane's endpoint model and type map.
 * docs/radiant-bridge.md section 8.1.
 *
 * This is the data model: the table keyed on the section 7 vocabulary type,
 * unit conversion into each cluster's unit, and the rule that instantiates
 * one endpoint per announced field. It's a `radiant_sink` like any other -
 * samples via P5's bus, bindings via P6's `binding_changed`.
 *
 * Not the Matter stack: no CHIP, commissioning, fabric or QR code here.
 * `radiant_matter_attr_write()` is the seam a real stack plugs into; tests
 * drive it directly.
 *
 * 0x26 heart rate has no row, on purpose (section 8.1): both Matter
 * candidates - a mislabelled Flow Measurement (8.3b), or an MEI cluster most
 * controllers won't understand (8.3) - are outside tier 1. So
 * `radiant_matter_row()` returning NULL is a normal answer, not a failure.
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
 * mul/div/offset take a value in the vocabulary's canonical SI unit and
 * produce the cluster's unit, offset applied after the scale. Temperature
 * (kelvin to 0.01degC, `K*100-27315`) is the only row needing a shift as
 * well as a scale (section 8.1a); every other row is a pure decimal scale.
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

/* Matter cluster and device-type identifiers, named rather than left as bare
 * hex - a wrong constant here is invisible until a controller shows the
 * wrong icon. */
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

/* The row for a vocabulary type, or NULL if the Matter plane declines it
 * (a normal answer - see header comment). */
const struct radiant_matter_type_map *radiant_matter_row(uint8_t field_type);

/* Converts a sample into its cluster's unit. Returns false when the type
 * has no row or the arithmetic doesn't fit the attribute - a saturating
 * cast would put a plausible wrong number in front of a user. */
bool radiant_matter_convert(const struct radiant_sample *s, int64_t *out);

/*
 * How many endpoints are currently instantiated, and what is on each. An
 * endpoint is instantiated per announced field, not per binding kind
 * (section 8.1): the four derived booleans of section 6 (all type 0x02)
 * are four instances of one row, distinguished by field_id.
 */
/* 16: room for section 8.1's worked example plus temperature/humidity/
 * battery rows. Compile-time constant, not Kconfig, like the rest of
 * src/bridge. Overrunning it logs a warning; the sensor just doesn't
 * appear (see endpoint_get_or_add()) - never a silent drop. */
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

/* Finds the endpoint carrying one (source, field_id), or NULL. Endpoint
 * numbers are assigned, not derived from any fixed identity. */
const struct radiant_matter_endpoint *radiant_matter_endpoint_for(uint32_t source,
								  uint8_t field_id);

/* The seam a real Matter stack plugs into. Weak, so tests and a stack-free
 * build get a no-op that still exercises everything above it; an image with
 * CHIP overrides this with the real attribute write. */
void radiant_matter_attr_write(uint16_t endpoint_id, uint32_t cluster,
			       uint32_t attribute, int64_t value);

/* Drop every endpoint. Tests, and a factory reset. */
void radiant_matter_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_MATTER_H_ */
