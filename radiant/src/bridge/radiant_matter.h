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

/*
 * One attribute write. `mul == 0` means CONSTANT: `offset` is written
 * verbatim, once, when the endpoint is created, and no sample is consulted.
 * Otherwise it is an ordinary conversion of the same sample the primary row
 * converts, into a different attribute's units.
 *
 * Two things forced this from "one attribute per row" into a list, and both
 * are bugs that only appear once a real CHIP stack is behind the seam:
 *
 *  - PRESSURE. Pressure Measurement's mandatory `MeasuredValue` is an int16s
 *    in whole kPa (pressure-measurement-cluster.xml:39), so 101325 Pa becomes
 *    101 and every barometer in the world reports six distinguishable values.
 *    `ScaledValue`/`Scale` is the cluster's own answer, but it is optional and
 *    `MeasuredValue` is not - a bridged endpoint that serves only the useful
 *    one is non-conformant, and one that serves only the mandatory one is
 *    useless. So the row writes all three.
 *
 *  - OCCUPANCY SENSOR TYPE (trap 16). `OccupancySensorType` left at its
 *    zero-initialised value means PIR, and home-assistant/core#164839 proposes
 *    remapping PIR endpoints from `device_class: occupancy` to `motion`. These
 *    booleans are derived from a chest strap and a crank, not from motion; a
 *    future controller release would silently relabel every one of them. There
 *    is nowhere else to state a constant attribute, because on a dynamic
 *    endpoint EVERY attribute is external storage and the stack stores
 *    nothing on our behalf.
 */
struct radiant_matter_attr_conv {
	uint32_t attribute;
	int32_t  mul;    /* 0 = constant; write `offset`, once, at creation */
	int32_t  div;
	int32_t  offset;
};

/* Two is what the pressure row needs (ScaledValue + Scale) and what occupancy
 * needs (type + bitmap). Raising it is free; it is fixed so a row is a
 * literal in a table rather than a thing with a lifetime. */
#define RADIANT_MATTER_MAX_EXTRA 2u

/*
 * DEADBANDS - the two `deadband` sentinels, named because 0 and -1 both mean
 * something and neither reads as a number.
 *
 * A deadband is an EFFICIENCY measure, not a conformance one, and the
 * distinction decides where it may and may not be applied. Min/max interval
 * enforcement lives in the Matter server's ReadHandler: a subscription with
 * MinIntervalFloor = 5 emits at most one report every 5 s no matter how often
 * a path is dirtied, so writing at 4 Hz is LEGAL. It is still wasteful - each
 * write walks the subscription list and bumps DataVersion on the CHIP thread,
 * which runs at K_PRIO_PREEMPT(1), above the ANT host thread - and a
 * controller that subscribes with MinIntervalFloor = 0 would put 4 Hz of
 * Thread traffic per attribute straight into the coexistence budget of
 * docs/radiant-bridge.md section 7. That is the whole argument; there is no
 * correctness claim here and a controller that wants every sample is not
 * being denied anything it is entitled to.
 *
 * The heartbeat is the other half and it is not optional: a deadband with no
 * heartbeat means a sensor whose reading never moves stops writing forever,
 * and "unchanged" then becomes indistinguishable from "gone" to anything
 * reading timestamps rather than Reachable.
 */
#define RADIANT_MATTER_DEADBAND_EVERY     (-1) /* no deadband: write every sample */
#define RADIANT_MATTER_DEADBAND_ON_CHANGE (0)  /* write only when the value differs */

struct radiant_matter_type_map {
	uint8_t  field_type;   /* the section 7 vocabulary */
	uint16_t device_type;  /* Matter Device Library, 0 = cluster only */
	uint32_t cluster;
	uint32_t attribute;
	int32_t  mul;
	int32_t  div;
	int32_t  offset;

	/*
	 * Suppression threshold IN THE CLUSTER'S OWN UNITS, not in the
	 * vocabulary's - it is compared against the converted value, so a
	 * temperature deadband of 0.5 degC is 50 and not 0.5. Comparing before
	 * conversion would make the threshold depend on whichever exponent a
	 * decoder happened to choose for that sample.
	 *
	 * RADIANT_MATTER_DEADBAND_EVERY / _ON_CHANGE above are the two
	 * sentinels. A row that states neither gets _ON_CHANGE, because it is
	 * the value a zeroed struct has and because "identical value, written
	 * again" is the one suppression that can never lose information.
	 */
	int32_t  deadband;
	/* Write anyway if this long has passed since the last write for this
	 * endpoint, whatever the deadband says. 0 = never (only legitimate
	 * beside DEADBAND_EVERY, where nothing is ever suppressed). */
	uint32_t heartbeat_s;

	/* Written in addition to the primary attribute above, into the same
	 * cluster. See struct radiant_matter_attr_conv. */
	uint8_t  n_extra;
	struct radiant_matter_attr_conv extra[RADIANT_MATTER_MAX_EXTRA];
};

/* Matter cluster and device-type identifiers, named rather than left as bare
 * hex - a wrong constant here is invisible until a controller shows the
 * wrong icon. */
#define MATTER_DEVTYPE_OCCUPANCY_SENSOR   0x0107u
#define MATTER_DEVTYPE_TEMPERATURE_SENSOR 0x0302u
#define MATTER_DEVTYPE_HUMIDITY_SENSOR    0x0307u
#define MATTER_DEVTYPE_PRESSURE_SENSOR    0x0305u

#define MATTER_CLUSTER_TEMP_MEASUREMENT     0x0402u
#define MATTER_CLUSTER_PRESSURE_MEASUREMENT 0x0403u
#define MATTER_CLUSTER_REL_HUMIDITY         0x0405u
#define MATTER_CLUSTER_OCCUPANCY_SENSING    0x0406u
#define MATTER_CLUSTER_POWER_SOURCE         0x002Fu

#define MATTER_ATTR_MEASURED_VALUE          0x0000u
#define MATTER_ATTR_OCCUPANCY               0x0000u
#define MATTER_ATTR_BAT_PERCENT_REMAINING   0x000Cu

/* Occupancy Sensing. `OccupancySensorType` is an enum and
 * `OccupancySensorTypeBitmap` a bitmap over the same four kinds, so the two
 * constants below must agree - a controller may read either. */
#define MATTER_ATTR_OCCUPANCY_SENSOR_TYPE   0x0001u
#define MATTER_ATTR_OCCUPANCY_SENSOR_BITMAP 0x0002u
#define MATTER_OCCUPANCY_TYPE_PHYSICAL_CONTACT     3    /* enum: 0 PIR, 1 ultrasonic, 2 both, 3 contact */
#define MATTER_OCCUPANCY_BITMAP_PHYSICAL_CONTACT   0x04 /* bit 0 PIR, bit 1 ultrasonic, bit 2 contact */

/* Pressure Measurement's optional high-resolution pair.
 * ScaledValue = 10^Scale x pressure in kPa. */
#define MATTER_ATTR_SCALED_VALUE            0x0010u
#define MATTER_ATTR_SCALE                   0x0014u
#define MATTER_PRESSURE_SCALE               2 /* 0.01 kPa = 10 Pa per step */

/* The row for a vocabulary type, or NULL if the Matter plane declines it
 * (a normal answer - see header comment). */
const struct radiant_matter_type_map *radiant_matter_row(uint8_t field_type);

/* Converts a sample into its cluster's unit. Returns false when the type
 * has no row or the arithmetic doesn't fit the attribute - a saturating
 * cast would put a plausible wrong number in front of a user. */
bool radiant_matter_convert(const struct radiant_sample *s, int64_t *out);

/* The same arithmetic against an arbitrary conversion rather than the row's
 * primary one - what a row's `extra[]` entries are converted with. Exposed
 * because the extras are as easy to get wrong as the primaries and a test
 * that cannot reach them does not cover them. `c->mul == 0` (a constant) is
 * refused here: a constant is not a conversion of anything. */
bool radiant_matter_convert_with(const struct radiant_sample *s,
				 const struct radiant_matter_attr_conv *c,
				 int64_t *out);

/*
 * How many endpoints are currently instantiated, and what is on each. An
 * endpoint is instantiated per announced field, not per binding kind
 * (section 8.1): the four derived booleans of section 6 (all type 0x02)
 * are four instances of one row, distinguished by field_id.
 */
/*
 * 16: room for section 8.1's worked example plus temperature/humidity/
 * pressure/battery rows. Compile-time constant, not Kconfig, like the rest of
 * src/bridge. Overrunning it logs a warning; the sensor just doesn't
 * appear (see endpoint_get_or_add()) - never a silent drop.
 *
 * THE BUDGET DOES NOT CLOSE IN THE WORST CASE, AND THAT IS ACCEPTED RATHER
 * THAN UNNOTICED. radiant_binding.h's RADIANT_BINDING_MAX is 8, and one
 * heart-rate binding alone announces four derived booleans; eight full
 * bindings cannot fit in sixteen endpoints and the ninth-onwards field logs
 * the warning and does not appear. Three reasons not to "fix" it by raising
 * this number:
 *
 *  - 16 is not ours to pick unilaterally. It must equal the CHIP side's
 *    BRIDGE_MAX_DYNAMIC_ENDPOINTS_NUMBER and
 *    CHIP_DEVICE_CONFIG_DYNAMIC_ENDPOINT_COUNT (trap 15: the latter sizes the
 *    Descriptor cluster's real server array, and too small means the Nth
 *    bridged endpoint enumerates with no cluster list at all - not a warning,
 *    not an error, just a broken entity). The glue static_asserts the three
 *    together; changing one alone is the failure this comment exists to stop.
 *  - Each slot is not free: a Descriptor instance, a DataVersion[] array and
 *    a shadow row per endpoint.
 *  - A household running eight simultaneous ANT+ sensors through one dongle
 *    is outside the radio budget of docs/radiant-bridge.md section 7 well
 *    before it is outside this table.
 *
 * So the ceiling is real, documented, and announced by the warning rather
 * than discovered as a missing entity.
 */
#define RADIANT_MATTER_MAX_ENDPOINTS 16u

/*
 * `endpoint_id` IS AN OPAQUE KEY, NOT A CHIP ENDPOINT NUMBER (trap 6).
 *
 * Assignment starts at 1 here, and on a real bridge CHIP endpoint 1 is the
 * Aggregator (nrf/applications/matter_bridge's
 * BRIDGE_AGGREGATOR_ENDPOINT_ID). The collision is resolved in the glue, by
 * mapping this id onto the first dynamic endpoint above the fixed ones - NOT
 * by renumbering here, because this numbering is what every test in
 * radiant/tests/src/test_matter.c asserts and because a data model that
 * knows a specific stack's fixed-endpoint layout has stopped being a data
 * model. Treat the value as "the Nth endpoint radiant asked for", and note
 * that trap 8 requires the glue to persist its own uuid -> CHIP id mapping
 * anyway, since Matter forbids reusing an id for a different device and this
 * table is RAM-backed and assigned in bind order.
 */
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
