/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work against radiant_naming.h/.c.
 *
 * What earns a case here: the two base-name tables (device type and FE-C
 * subtype, including both defaults), the five occupancy suffixes, the label
 * override, and - the one that matters most - that the SUFFIX SURVIVES at the
 * worst case. A heart-rate strap is four endpoints and a treadmill is one more,
 * and every one of them is Matter device type 0x0107 Occupancy Sensor: the tail
 * ("Worn", "At Rest", "In Use") is the only thing distinguishing them, and
 * snprintf truncates from the end. A name that loses its tail is not a cosmetic
 * failure, it is four identical entities again.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "radiant_bridge.h"
#include "radiant_naming.h"
#include "radiant_rules.h"

/* The device types, spelled here rather than included: this suite tests the
 * formatter's table, and reading the constant out of the same header the table
 * uses would let both move together unnoticed. */
#define DEVTYPE_HR    0x78u
#define DEVTYPE_POWER 0x0Bu
#define DEVTYPE_FEC   0x11u
#define DEVTYPE_ENV   0x19u

/* Table 8-8 (radiant/src/profiles/profile_fec.h). */
#define FEC_TREADMILL  19u
#define FEC_ELLIPTICAL 20u
#define FEC_ROWER      22u
#define FEC_CLIMBER    23u
#define FEC_SKI        24u
#define FEC_TRAINER    25u

static char buf[RADIANT_NAMING_MAX];

static const char *name_of(uint8_t devtype, uint16_t devnum, uint8_t subtype,
			   const char *label, uint8_t field_type, uint8_t field_id)
{
	int n = radiant_naming_format(buf, sizeof(buf), devtype, devnum, subtype,
				      label, field_type, field_id);

	zassert_true(n >= 0, "formatter refused a valid buffer");
	zassert_equal((size_t)n, strlen(buf),
		      "the return value must be the length actually written");
	return buf;
}

/* Every occupancy endpoint of one heart-rate strap, which is the case the
 * whole change exists for: four entities, one device type, four names. */
static const char *occupancy_name(uint8_t devtype, uint16_t devnum, uint8_t subtype,
				  uint8_t rule_field)
{
	return name_of(devtype, devnum, subtype, NULL, RADIANT_FIELD_OCCUPANCY,
		       rule_field);
}

/* ── The device-type table ──────────────────────────────────────────────── */

ZTEST(radiant_naming, test_base_names_come_from_the_device_type)
{
	zassert_str_equal(name_of(DEVTYPE_HR, 8842u, 0u, NULL, RADIANT_FIELD_HEART_RATE, 0u),
			  "Heart Rate 8842", NULL);
	zassert_str_equal(name_of(DEVTYPE_POWER, 17u, 0u, NULL, RADIANT_FIELD_ACTIVE_POWER, 0u),
			  "Power 17", NULL);
	zassert_str_equal(name_of(DEVTYPE_ENV, 4211u, 0u, NULL, RADIANT_FIELD_TEMPERATURE, 0u),
			  "Environment 4211", NULL);
}

ZTEST(radiant_naming, test_an_unknown_device_type_still_gets_a_usable_name)
{
	/* Not the hex device type: a user cannot act on "0x5C". The device
	 * number is what tells two of them apart, and it is printed on the
	 * sensor. */
	zassert_str_equal(name_of(0x5Cu, 900u, 0u, NULL, RADIANT_FIELD_TEMPERATURE, 0u),
			  "Sensor 900", NULL);
}

ZTEST(radiant_naming, test_the_primary_reading_carries_no_suffix)
{
	/* Field id 0 is the profile's headline series - bpm on a strap, watts on
	 * a power meter, degrees on an environment sensor - and it composes to
	 * the bare name a user expects for the thing the sensor is FOR.
	 *
	 * THIS USED TO ASSERT SOMETHING BROADER AND WRONG: that no field except
	 * the occupancy booleans gets a suffix, on the argument that the field
	 * type already says what the reading is. A bench run with a real strap
	 * and a real trainer measured four collisions from exactly that. See
	 * radiant_naming.c's suffix_names[] header. */
	zassert_str_equal(name_of(DEVTYPE_HR, 8842u, 0u, NULL, RADIANT_FIELD_HEART_RATE, 0u),
			  "Heart Rate 8842", NULL);
	zassert_str_equal(name_of(DEVTYPE_ENV, 4211u, 0u, NULL, RADIANT_FIELD_TEMPERATURE, 0u),
			  "Environment 4211", NULL);
}

ZTEST(radiant_naming, test_a_field_with_no_row_is_still_given_a_unique_tail)
{
	/* The backstop. A series the table has no row for must not fall back to
	 * the bare name, because that is the primary's name - two identical
	 * entities, which is the defect this file exists to prevent. "#10" is
	 * deliberately ugly: it is a legible name and a visible request to add a
	 * row. */
	zassert_str_equal(name_of(DEVTYPE_HR, 8842u, 0u, NULL, RADIANT_FIELD_TEMPERATURE, 10u),
			  "Heart Rate 8842 #10", NULL);
	zassert_str_equal(name_of(DEVTYPE_HR, 8842u, 0u, NULL, RADIANT_FIELD_VOLTAGE, 31u),
			  "Heart Rate 8842 #31", NULL);

	/* And it must differ from the primary, which is the whole point. */
	zassert_true(strcmp(name_of(DEVTYPE_HR, 8842u, 0u, NULL, RADIANT_FIELD_TEMPERATURE, 10u),
			    "Heart Rate 8842") != 0,
		     "an unrowed field must never collide with the primary reading");
}

ZTEST(radiant_naming, test_the_series_a_real_sensor_carries_all_get_their_own_name)
{
	/* THE REGRESSION FOR THE MEASURED DEFECT. Four collisions were observed
	 * on real hardware with a heart-rate strap (#8265) and an FE-C trainer
	 * (#52233) on air, all of them invisible to a suite that checks names one
	 * at a time:
	 *
	 *   "Power 52233"       = event count (0x36/0x01) and energy (0x30/0x02)
	 *   "Indoor Bike 52233" = equipment type (0x40/0x0D) and event count
	 *   "Indoor Bike 52233" = equipment type (0x40/0x0D) and FE state (0x40/0x08)
	 *   "Heart Rate 8265"   = beat count (0x36/0x01) and beat time (0x37/0x02)
	 *
	 * So this case asserts the PAIRWISE property over every series one
	 * binding can actually carry, rather than the value of any one name. */
	struct field {
		uint8_t type;
		uint8_t id;
	};

	/* The common-page block rides on top of whatever profile owns the
	 * channel (radiant_common_adapter.h), so it is appended to every
	 * profile below rather than being a device type of its own. */
	static const struct field common[] = {
		{ RADIANT_FIELD_TEMPERATURE, 0x21u }, { RADIANT_FIELD_PRESSURE, 0x22u },
		{ RADIANT_FIELD_HUMIDITY, 0x23u },    { RADIANT_FIELD_SPEED, 0x24u },
		{ RADIANT_FIELD_ANGLE, 0x25u },       { RADIANT_FIELD_EVENT_COUNT, 0x26u },
		{ RADIANT_FIELD_TEMPERATURE, 0x27u }, { RADIANT_FIELD_TEMPERATURE, 0x28u },
	};
	static const struct field hr[] = {
		{ RADIANT_FIELD_HEART_RATE, 0x00u }, { RADIANT_FIELD_EVENT_COUNT, 0x01u },
		{ RADIANT_FIELD_DURATION, 0x02u },
		{ RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_WORN },
		{ RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_ACTIVE },
		{ RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_AT_REST },
		{ RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_ZONE2 },
	};
	static const struct field power[] = {
		{ RADIANT_FIELD_ACTIVE_POWER, 0x00u }, { RADIANT_FIELD_EVENT_COUNT, 0x01u },
		{ RADIANT_FIELD_ENERGY, 0x02u },
		{ RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_ACTIVE },
	};
	static const struct field fec[] = {
		{ RADIANT_FIELD_ACTIVE_POWER, 0x00u }, { RADIANT_FIELD_EVENT_COUNT, 0x01u },
		{ RADIANT_FIELD_ENERGY, 0x02u },       { RADIANT_FIELD_ENUM_GENERIC, 0x08u },
		{ RADIANT_FIELD_SPEED, 0x09u },        { RADIANT_FIELD_ENUM_GENERIC, 0x0Du },
		{ RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_EQUIPMENT_IN_USE },
	};
	static const struct field env[] = {
		{ RADIANT_FIELD_TEMPERATURE, 0x00u }, { RADIANT_FIELD_EVENT_COUNT, 0x01u },
	};
	/* radiant_rd_adapter.c. Not linked into the dongle image, but compiled
	 * and tested - and its id 0 is vertical oscillation rather than a
	 * headline reading, which is why it has an explicit row. */
	static const struct field rd[] = {
		{ RADIANT_FIELD_LENGTH, 0x00u },     { RADIANT_FIELD_PERCENTAGE, 0x02u },
		{ RADIANT_FIELD_EVENT_COUNT, 0x04u }, { RADIANT_FIELD_PERCENTAGE, 0x05u },
		{ RADIANT_FIELD_PERCENTAGE, 0x06u },  { RADIANT_FIELD_LENGTH, 0x07u },
	};

	const struct {
		const char         *what;
		uint8_t             devtype;
		uint8_t             subtype;
		const struct field *fields;
		size_t              n;
	} sensors[] = {
		{ "heart rate", DEVTYPE_HR, 0u, hr, ARRAY_SIZE(hr) },
		{ "power meter", DEVTYPE_POWER, 0u, power, ARRAY_SIZE(power) },
		{ "trainer", DEVTYPE_FEC, FEC_TRAINER, fec, ARRAY_SIZE(fec) },
		{ "treadmill", DEVTYPE_FEC, FEC_TREADMILL, fec, ARRAY_SIZE(fec) },
		/* Before any page 16 - the state every FE-C binding is in for its
		 * first fraction of a second. */
		{ "unlearned equipment", DEVTYPE_FEC, 0u, fec, ARRAY_SIZE(fec) },
		{ "environment", DEVTYPE_ENV, 0u, env, ARRAY_SIZE(env) },
		{ "running dynamics", 0x1Eu, 0u, rd, ARRAY_SIZE(rd) },
		{ "an unknown device type", 0x5Cu, 0u, hr, ARRAY_SIZE(hr) },
	};

	for (size_t s = 0u; s < ARRAY_SIZE(sensors); s++) {
		char   names[24][RADIANT_NAMING_MAX];
		size_t total = sensors[s].n + ARRAY_SIZE(common);
		size_t i;

		zassert_true(total <= ARRAY_SIZE(names), "widen names[]");

		for (i = 0u; i < total; i++) {
			const struct field *f = (i < sensors[s].n)
							? &sensors[s].fields[i]
							: &common[i - sensors[s].n];
			int n = radiant_naming_format(names[i], RADIANT_NAMING_MAX,
						      sensors[s].devtype, 4242u,
						      sensors[s].subtype, NULL,
						      f->type, f->id);

			zassert_true(n > 0, "%s: field 0x%02x/0x%02x produced no name",
				     sensors[s].what, f->type, f->id);
		}

		for (i = 0u; i < total; i++) {
			for (size_t j = i + 1u; j < total; j++) {
				zassert_true(strcmp(names[i], names[j]) != 0,
					     "%s: two series both compose to \"%s\"",
					     sensors[s].what, names[i]);
			}
		}
	}
}

/* ── The FE-C subtype table ─────────────────────────────────────────────── */

ZTEST(radiant_naming, test_every_fec_subtype_has_its_own_name)
{
	zassert_str_equal(name_of(DEVTYPE_FEC, 51234u, FEC_TREADMILL, NULL, 0u, 0u),
			  "Treadmill 51234", NULL);
	zassert_str_equal(name_of(DEVTYPE_FEC, 1u, FEC_ELLIPTICAL, NULL, 0u, 0u),
			  "Elliptical 1", NULL);
	zassert_str_equal(name_of(DEVTYPE_FEC, 2u, FEC_ROWER, NULL, 0u, 0u), "Rower 2", NULL);
	zassert_str_equal(name_of(DEVTYPE_FEC, 3u, FEC_CLIMBER, NULL, 0u, 0u), "Climber 3", NULL);
	zassert_str_equal(name_of(DEVTYPE_FEC, 4u, FEC_SKI, NULL, 0u, 0u), "Ski Erg 4", NULL);
	zassert_str_equal(name_of(DEVTYPE_FEC, 5u, FEC_TRAINER, NULL, 0u, 0u),
			  "Indoor Bike 5", NULL);
}

ZTEST(radiant_naming, test_an_unlearned_or_unknown_subtype_is_generic)
{
	/* 0 is "no page 16 has been heard yet" - the state every FE-C endpoint
	 * is in for its first fraction of a second, and the reason the Matter
	 * bridge carries a late-rename path at all. It must still produce a
	 * usable name rather than an empty or hex one. */
	zassert_str_equal(name_of(DEVTYPE_FEC, 51234u, 0u, NULL, 0u, 0u),
			  "Fitness Equip 51234", NULL);
	/* 21 and 26 are not in Table 8-8. A future allocation must not be
	 * guessed at. */
	zassert_str_equal(name_of(DEVTYPE_FEC, 51234u, 21u, NULL, 0u, 0u),
			  "Fitness Equip 51234", NULL);
	zassert_str_equal(name_of(DEVTYPE_FEC, 51234u, 26u, NULL, 0u, 0u),
			  "Fitness Equip 51234", NULL);
}

ZTEST(radiant_naming, test_the_subtype_is_ignored_off_fec)
{
	/* Field ids and subtypes are per device type. A heart-rate binding can
	 * never carry an equipment type, and if one somehow arrived it must not
	 * rename the strap to "Treadmill". */
	zassert_str_equal(name_of(DEVTYPE_HR, 8842u, FEC_TREADMILL, NULL, 0u, 0u),
			  "Heart Rate 8842", NULL);
}

/* ── The suffixes ───────────────────────────────────────────────────────── */

ZTEST(radiant_naming, test_the_five_occupancy_booleans_are_distinguishable)
{
	/* THE CASE THE WHOLE CHANGE EXISTS FOR. Before it, all four of these
	 * were "3f2a91c40e77b21205"-style hex, differing only in the last two
	 * characters, on four endpoints of one identical Matter device type. */
	zassert_str_equal(occupancy_name(DEVTYPE_HR, 8842u, 0u, RADIANT_RULE_FIELD_WORN),
			  "Heart Rate 8842 Worn", NULL);
	zassert_str_equal(occupancy_name(DEVTYPE_HR, 8842u, 0u, RADIANT_RULE_FIELD_ACTIVE),
			  "Heart Rate 8842 Active", NULL);
	zassert_str_equal(occupancy_name(DEVTYPE_HR, 8842u, 0u, RADIANT_RULE_FIELD_AT_REST),
			  "Heart Rate 8842 At Rest", NULL);
	zassert_str_equal(occupancy_name(DEVTYPE_HR, 8842u, 0u, RADIANT_RULE_FIELD_ZONE2),
			  "Heart Rate 8842 Zone 2", NULL);
	zassert_str_equal(occupancy_name(DEVTYPE_FEC, 51234u, FEC_TREADMILL,
					 RADIANT_RULE_FIELD_EQUIPMENT_IN_USE),
			  "Treadmill 51234 In Use", NULL);
}

ZTEST(radiant_naming, test_a_suffix_needs_both_halves_of_the_key)
{
	/* field_id 0 is RADIANT_RULE_FIELD_WORN on an OCCUPANCY sample and
	 * instantaneous power on a power adapter's. Keying the suffix on the id
	 * alone would name a wattage series "Worn". */
	zassert_str_equal(name_of(DEVTYPE_POWER, 17u, 0u, NULL, RADIANT_FIELD_ACTIVE_POWER,
				  RADIANT_RULE_FIELD_WORN),
			  "Power 17", NULL);
}

/* ── The label override ─────────────────────────────────────────────────── */

ZTEST(radiant_naming, test_a_label_replaces_the_base_name_and_the_number)
{
	/* struct radiant_binding::label is "what a human calls it". Repeating
	 * the device number they named it to avoid is not an improvement - but
	 * the suffix stays, because a labelled strap is still five endpoints. */
	zassert_str_equal(name_of(DEVTYPE_HR, 8842u, 0u, "Chest Strap",
				  RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_ZONE2),
			  "Chest Strap Zone 2", NULL);
	zassert_str_equal(name_of(DEVTYPE_HR, 8842u, 0u, "Chest Strap",
				  RADIANT_FIELD_HEART_RATE, 0u),
			  "Chest Strap", NULL);
}

ZTEST(radiant_naming, test_an_empty_label_is_not_an_override)
{
	/* Which is the production case: the only bind site passes NULL, so
	 * every binding in the tree has label[0] == '\0'. */
	zassert_str_equal(name_of(DEVTYPE_HR, 8842u, 0u, "", RADIANT_FIELD_OCCUPANCY,
				  RADIANT_RULE_FIELD_WORN),
			  "Heart Rate 8842 Worn", NULL);
	zassert_str_equal(name_of(DEVTYPE_HR, 8842u, 0u, NULL, RADIANT_FIELD_OCCUPANCY,
				  RADIANT_RULE_FIELD_WORN),
			  "Heart Rate 8842 Worn", NULL);
}

/* ── The budget ─────────────────────────────────────────────────────────── */

ZTEST(radiant_naming, test_the_worst_case_fits_with_its_suffix_intact)
{
	/* Longest base ("Fitness Equip"), widest device number (65535) and
	 * longest suffix ("At Rest") together. radiant_naming.c has a
	 * BUILD_ASSERT for this, which is the real guard - this case is what
	 * says what the assertion is protecting, and fails loudly if someone
	 * relaxes it. */
	const char *n = name_of(DEVTYPE_FEC, 65535u, 0u, NULL, RADIANT_FIELD_OCCUPANCY,
				RADIANT_RULE_FIELD_AT_REST);

	zassert_str_equal(n, "Fitness Equip 65535 At Rest", NULL);
	zassert_true(strlen(n) < RADIANT_NAMING_MAX,
		     "the composed name must fit Matter's kNodeLabelSize with its NUL");
	zassert_not_null(strstr(n, "At Rest"),
			 "the suffix is the half that disambiguates - it must never be the half truncated");

	/* The longest suffix in the table is no longer "At Rest". Pair it with
	 * the longest base and the widest device number - a combination no real
	 * sensor produces, which is exactly why only the BUILD_ASSERT and this
	 * case are watching it. */
	n = name_of(DEVTYPE_FEC, 65535u, 0u, NULL, RADIANT_FIELD_PERCENTAGE, 0x06u);
	zassert_str_equal(n, "Fitness Equip 65535 Vert Ratio", NULL);
	zassert_true(strlen(n) < RADIANT_NAMING_MAX,
		     "the widest base/suffix pair must still fit kNodeLabelSize");
}

ZTEST(radiant_naming, test_a_labelled_sensor_keeps_its_suffix_at_the_worst_case)
{
	/* struct radiant_binding::label is char[16], so 15 characters plus the
	 * longest suffix is the label path's own worst case. It has a
	 * BUILD_ASSERT of its own now; this is what says what that assertion is
	 * protecting. */
	const char *n = name_of(DEVTYPE_HR, 8842u, 0u, "Fifteen Chars!!",
				RADIANT_FIELD_PERCENTAGE, 0x06u);

	zassert_str_equal(n, "Fifteen Chars!! Vert Ratio", NULL);
	zassert_true(strlen(n) < RADIANT_NAMING_MAX, NULL);
}

ZTEST(radiant_naming, test_a_short_buffer_truncates_safely)
{
	char small[8];
	int  n = radiant_naming_format(small, sizeof(small), DEVTYPE_HR, 8842u, 0u, NULL,
				       RADIANT_FIELD_OCCUPANCY, RADIANT_RULE_FIELD_WORN);

	/* The reported length must be what IS there, not what snprintf would
	 * have written: Matter's SetNodeLabel() memcpy()s the length it is
	 * given, so an over-reported one reads past the end of this buffer. */
	zassert_equal(n, (int)(sizeof(small) - 1u), NULL);
	zassert_equal(strlen(small), sizeof(small) - 1u, NULL);
}

ZTEST(radiant_naming, test_a_null_buffer_is_refused_rather_than_written)
{
	char one[1];

	zassert_true(radiant_naming_format(NULL, 32u, DEVTYPE_HR, 1u, 0u, NULL, 0u, 0u) < 0, NULL);
	zassert_true(radiant_naming_format(one, 0u, DEVTYPE_HR, 1u, 0u, NULL, 0u, 0u) < 0, NULL);
}

ZTEST_SUITE(radiant_naming, NULL, NULL, NULL, NULL, NULL);
