/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_naming.c - see radiant_naming.h.
 *
 * Three static const tables and one formatter over them. No state, no locks,
 * no allocation, and nothing here reads the binding table or the sample bus -
 * every input arrives as a scalar from a caller that already holds whatever
 * lock protects it.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>

#include "radiant_bridge.h"
#include "radiant_naming.h"
#include "radiant_rules.h"

/*
 * The two table widths, NUL INCLUDED, and they are `char name[N]` rather than
 * `const char *` on purpose: an over-long entry is then a compile error at the
 * initialiser instead of a name that silently truncates at run time. The
 * BUILD_ASSERT below turns those two widths into the one guarantee that
 * matters - see "WHY THE SUFFIX IS THE HALF THAT MUST SURVIVE".
 */
#define NAMING_BASE_SZ   14u /* "Fitness Equip" + NUL */
#define NAMING_SUFFIX_SZ 11u /* "Vert Ratio" + NUL */

/* struct radiant_binding::label is char[16]. Named here rather than included,
 * so this file still has no dependency on the binding table. */
#define NAMING_LABEL_MAX 15u

/*
 * WHY THE SUFFIX IS THE HALF THAT MUST SURVIVE.
 *
 * A heart-rate strap is FOUR endpoints and every one of them is Matter device
 * type 0x0107 Occupancy Sensor, so "Heart Rate 8842" x4 is no more usable than
 * the hex string this file replaces - it is the "Worn"/"Zone 2" tail that
 * distinguishes them. snprintf truncates from the END, so a base name that
 * grew past the budget would eat exactly the half that carries the
 * information, and it would do it silently.
 *
 * So the worst case is asserted rather than tested: longest base + ' ' +
 * five digits of device number + ' ' + longest suffix + NUL, against
 * RADIANT_NAMING_MAX (Matter's kNodeLabelSize). Today that is
 * "Fitness Equip 65535 At Rest" = 27 + NUL. A future base name long enough to
 * push it over fails the build here rather than shipping truncated names.
 */
BUILD_ASSERT((NAMING_BASE_SZ - 1u) + 1u + 5u + 1u + (NAMING_SUFFIX_SZ - 1u) + 1u <= RADIANT_NAMING_MAX,
	     "a base or suffix name grew past the NodeLabel budget - the suffix would be truncated away");

/* The label path has its own worst case and it was never asserted: a 15-char
 * label plus the longest suffix. Shorter than the base path today, but the two
 * budgets move independently. */
BUILD_ASSERT(NAMING_LABEL_MAX + 1u + (NAMING_SUFFIX_SZ - 1u) + 1u <= RADIANT_NAMING_MAX,
	     "a labelled sensor's longest name no longer fits - the suffix would be truncated away");

struct base_row {
	uint8_t devtype;
	char    name[NAMING_BASE_SZ];
};

/*
 * ANT device type -> base name. Seeded from src/self_channels.c's own
 * self_profiles[].name strings (the ones the search log already prints) and
 * docs/profile-registry.md, title-cased for a user-facing label.
 *
 * 0x11 IS ABSENT FROM THIS TABLE, deliberately: fitness equipment resolves
 * through the subtype table below, and a row here would be a second answer for
 * it. Generic nouns only, per ADR 0003 - no vendor word and no "ANT+" prefix.
 */
static const struct base_row base_names[] = {
	{ 0x78u, "Heart Rate" },
	{ 0x0Bu, "Power" },
	{ 0x19u, "Environment" },
	/* Not linked in the production dongle image (see apps/dongle_thread's
	 * CMakeLists.txt on radiant_rd_adapter.c) but compiled and tested, so
	 * the name is here rather than appearing as "Sensor" the day it is. */
	{ 0x1Eu, "Running" },
	{ 0x7Cu, "Stride" },
};

/*
 * FE-C equipment type -> base name. The codes are Table 8-8's, spelled with
 * the PROFILE_FEC_TYPE_* names in the comments rather than by including
 * src/profiles/profile_fec.h: this file sits above the profile decoders, the
 * same line radiant_rules.c draws for FEC_STATE_IN_USE.
 *
 * 0 IS NOT A ROW. Table 8-8 has no type 0, and the subtype is 0 for exactly as
 * long as no page 16 has been heard - so an unknown code and a not-yet-learned
 * one both land on the default below, which is the honest answer for both.
 */
static const struct base_row fec_names[] = {
	{ 19u, "Treadmill" },    /* PROFILE_FEC_TYPE_TREADMILL */
	{ 20u, "Elliptical" },   /* PROFILE_FEC_TYPE_ELLIPTICAL */
	{ 22u, "Rower" },        /* PROFILE_FEC_TYPE_ROWER */
	{ 23u, "Climber" },      /* PROFILE_FEC_TYPE_CLIMBER */
	{ 24u, "Ski Erg" },      /* PROFILE_FEC_TYPE_NORDIC_SKIER */
	{ 25u, "Indoor Bike" },  /* PROFILE_FEC_TYPE_TRAINER */
};

#define FEC_DEVTYPE 0x11u

/* The one place the "we have not learned it yet / we do not know this code"
 * answer is spelled. Still a usable name, which is the point: an endpoint
 * created before the first page 16 is "Fitness Equip 51234 In Use" rather than
 * nameless, and improves to "Treadmill ..." when the page arrives. */
static const char fec_default[] = "Fitness Equip";

struct suffix_row {
	uint8_t field_type;
	uint8_t field_id;
	char    name[NAMING_SUFFIX_SZ];
};

/*
 * (field_type, field_id) -> suffix. BOTH halves are the key, because field_id
 * alone means nothing across producers - id 0 is the WORN boolean here and the
 * instantaneous-power series in radiant_power_adapter.h.
 *
 * EVERY SERIES A SOURCE CAN CARRY NEEDS A ROW, NOT JUST THE AMBIGUOUS-LOOKING
 * ONES, and that is the correction this table records.
 *
 * The first version of this file suffixed only the five derived occupancy
 * booleans, on the argument that a temperature endpoint is already a Matter
 * Temperature Sensor and an HA entity with device_class temperature, so
 * appending "Temperature" would repeat what the type already says. That
 * argument is wrong twice over, and a bench run with a real heart-rate strap
 * and a real trainer on air measured it (four distinct collisions):
 *
 *   - THE TYPE OFTEN DOES NOT DISAMBIGUATE. A heart-rate strap posts a beat
 *     count (0x36) and a beat time (0x37) beside its bpm; a trainer posts an
 *     event count, an energy total, an FE state and now an equipment type. All
 *     of them composed to the bare "Heart Rate 8265" / "Indoor Bike 52233".
 *   - AND SOMETIMES THE TYPE IS THE SAME TYPE. Common pages 0x21, 0x27 and
 *     0x28 are all RADIANT_FIELD_TEMPERATURE on one source, so those three
 *     Matter endpoints shared a name AND a device type - exactly the case the
 *     occupancy five were suffixed to avoid.
 *
 * The pre-existing MQTT names ("RadiANT 1", "RadiANT 2") were ugly but unique,
 * so shipping the table in its first form would have traded a readability bug
 * for a correctness one.
 *
 * ONE ROW SERVES EVERY PRODUCER OF A GIVEN (type, id). A heart-rate strap's
 * beat count, a power meter's event count and an environment sensor's event
 * count are all (0x36, 0x01) and all read "Events" - the vocabulary's own
 * word. Keying on the device type as well would let three tables disagree
 * about one series for no gain.
 *
 * ID 0 IS THE PROFILE'S PRIMARY READING and deliberately has no row, so it
 * composes to the bare "Heart Rate 8265" / "Power 52233" - the name a user
 * expects for the thing the sensor is for. radiant_rd_adapter.c is the
 * exception that proves it: its id 0 is vertical oscillation rather than a
 * headline reading, so it has an explicit row and that row wins.
 */
static const struct suffix_row suffix_names[] = {
	/* The derived booleans (radiant_rules.c). All five are
	 * RADIANT_FIELD_OCCUPANCY, which is why they needed this first. */
	{ RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_WORN, "Worn" },
	{ RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_ACTIVE, "Active" },
	{ RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_AT_REST, "At Rest" },
	{ RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_ZONE2, "Zone 2" },
	{ RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_EQUIPMENT_IN_USE, "In Use" },

	/* The profile block, ids 0x00-0x1F (radiant_bridge.h). Shared across
	 * radiant_hr_adapter.c, radiant_power_adapter.c and
	 * radiant_env_adapter.c - see the "one row serves every producer" note. */
	{ RADIANT_FIELD_EVENT_COUNT, 0x01u, "Events" },    /* beats / power events / env events */
	{ RADIANT_FIELD_DURATION, 0x02u, "Beat Time" },    /* radiant_hr_adapter.h */
	{ RADIANT_FIELD_ENERGY, 0x02u, "Energy" },         /* radiant_power_adapter.h */
	{ RADIANT_FIELD_ENUM_GENERIC, 0x08u, "State" },    /* FE-C state */
	{ RADIANT_FIELD_SPEED, 0x09u, "Speed" },           /* FE-C speed */
	{ RADIANT_FIELD_ENUM_GENERIC, 0x0Du, "Type" },     /* FE-C equipment type */

	/* The common-page block, ids 0x20-0x3F (radiant_common_adapter.h).
	 * These ride on top of WHATEVER profile owns the channel, so they must
	 * not collide with the profile rows above or with each other. */
	{ RADIANT_FIELD_TEMPERATURE, 0x21u, "Temp" },
	{ RADIANT_FIELD_PRESSURE, 0x22u, "Pressure" },
	{ RADIANT_FIELD_HUMIDITY, 0x23u, "Humidity" },
	{ RADIANT_FIELD_SPEED, 0x24u, "Wind" },
	{ RADIANT_FIELD_ANGLE, 0x25u, "Wind Dir" },
	{ RADIANT_FIELD_EVENT_COUNT, 0x26u, "Charge" },
	{ RADIANT_FIELD_TEMPERATURE, 0x27u, "Temp Min" },
	{ RADIANT_FIELD_TEMPERATURE, 0x28u, "Temp Max" },

	/* radiant_rd_adapter.c. Not linked into the production dongle image
	 * (see apps/dongle_thread/CMakeLists.txt) but compiled and tested, so
	 * its rows are here for the same reason "Running" is in base_names[]. */
	{ RADIANT_FIELD_LENGTH, 0x00u, "Vert Osc" },
	{ RADIANT_FIELD_PERCENTAGE, 0x02u, "Stance" },
	{ RADIANT_FIELD_EVENT_COUNT, 0x04u, "Steps" },
	{ RADIANT_FIELD_PERCENTAGE, 0x05u, "Balance" },
	{ RADIANT_FIELD_PERCENTAGE, 0x06u, "Vert Ratio" },
	{ RADIANT_FIELD_LENGTH, 0x07u, "Step Len" },
};

static const char *base_for(uint8_t devtype, uint8_t subtype, size_t *cap)
{
	size_t i;

	if (devtype == FEC_DEVTYPE) {
		for (i = 0u; i < ARRAY_SIZE(fec_names); i++) {
			if (fec_names[i].devtype == subtype) {
				*cap = sizeof(fec_names[i].name);
				return fec_names[i].name;
			}
		}
		*cap = sizeof(fec_default);
		return fec_default;
	}

	for (i = 0u; i < ARRAY_SIZE(base_names); i++) {
		if (base_names[i].devtype == devtype) {
			*cap = sizeof(base_names[i].name);
			return base_names[i].name;
		}
	}

	/* An unknown device type still gets a name with the device number in
	 * it, which is enough to tell two of them apart. "Sensor" rather than
	 * the hex device type: a user cannot act on 0x5C. */
	*cap = sizeof("Sensor");
	return "Sensor";
}

/* NULL means "no row", which is not the same as "no suffix" - see the caller. */
static const char *suffix_for(uint8_t field_type, uint8_t field_id, size_t *cap)
{
	size_t i;

	for (i = 0u; i < ARRAY_SIZE(suffix_names); i++) {
		if (suffix_names[i].field_type == field_type &&
		    suffix_names[i].field_id == field_id) {
			*cap = sizeof(suffix_names[i].name);
			return suffix_names[i].name;
		}
	}

	return NULL;
}

/*
 * THE BACKSTOP, and it is the half of this fix that survives the next adapter.
 *
 * A table is only correct until somebody posts a series it has no row for, and
 * the failure mode is silent: the new field composes to the same bare name as
 * the profile's primary reading and a user sees two identical entities. That
 * is precisely how the first version of this file shipped.
 *
 * So an unmatched, non-zero field id falls back to "#<id>" rather than to
 * nothing. It is deliberately ugly - "Heart Rate 8265 #10" is a legible name
 * AND a visible request to add a row - and it cannot collide: a table suffix
 * never begins with '#', the primary (id 0) carries no suffix at all, and ids
 * are unique within each of the three allocation blocks radiant_bridge.h
 * defines. test_naming.c asserts the whole property rather than trusting this
 * paragraph.
 */
static const char *suffix_fallback(uint8_t field_id, char *buf, size_t buf_sz,
				   size_t *cap)
{
	if (field_id == 0u) {
		*cap = 1u;
		return ""; /* the profile's primary reading */
	}

	(void)snprintf(buf, buf_sz, "#%u", (unsigned int)field_id);
	*cap = buf_sz;
	return buf;
}

int radiant_naming_format(char *out, size_t n,
			  uint8_t devtype, uint16_t devnum, uint8_t subtype,
			  const char *label,
			  uint8_t field_type, uint8_t field_id)
{
	const char *base;
	const char *suffix;
	char        fallback[8]; /* "#255" + NUL, comfortably */
	size_t      base_cap = 0u;
	size_t      suffix_cap = 0u;
	bool        overridden = (label != NULL && label[0] != '\0');
	int         written;

	if (out == NULL || n == 0u) {
		return -EINVAL;
	}

	suffix = suffix_for(field_type, field_id, &suffix_cap);
	if (suffix == NULL) {
		suffix = suffix_fallback(field_id, fallback, sizeof(fallback),
					 &suffix_cap);
	}

	/*
	 * The %.*s precisions are not belt-and-braces. `name` is a fixed-width
	 * char array, and C lets an initialiser exactly as long as the array
	 * drop the NUL silently - so a future 13-character base name would
	 * otherwise print whatever follows it in .rodata. The precision bounds
	 * every one of these reads at the array's own width.
	 */
	if (overridden) {
		/* The label REPLACES the base name and the device number - it is
		 * what a human chose to call this sensor, and repeating the
		 * number they named it to avoid is not an improvement. The
		 * suffix still goes on: the five occupancy booleans of one
		 * labelled strap are still five endpoints. */
		if (suffix[0] != '\0') {
			written = snprintf(out, n, "%s %.*s", label,
					   (int)suffix_cap, suffix);
		} else {
			written = snprintf(out, n, "%s", label);
		}
	} else {
		base = base_for(devtype, subtype, &base_cap);
		if (suffix[0] != '\0') {
			written = snprintf(out, n, "%.*s %u %.*s",
					   (int)base_cap, base, (unsigned int)devnum,
					   (int)suffix_cap, suffix);
		} else {
			written = snprintf(out, n, "%.*s %u",
					   (int)base_cap, base, (unsigned int)devnum);
		}
	}

	if (written < 0) {
		out[0] = '\0';
		return -EINVAL;
	}
	if ((size_t)written >= n) {
		/* snprintf reports what it WOULD have written. Report what is
		 * there, so a caller passing the result to a memcpy-based API
		 * (Matter's SetNodeLabel does exactly that) cannot be handed a
		 * length past the end of its own buffer. */
		return (int)(n - 1u);
	}
	return written;
}


