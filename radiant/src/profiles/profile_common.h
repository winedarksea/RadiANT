/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_common.h - ANT+ common pages 80, 81 and 82 (encode) and 84 (decode).
 *
 * Provenance: docs/device-profiles.md section "Common pages" (formerly
 * docs/ant-plus-profiles.md, absorbed into it) - the byte
 * tables for pages 0x50, 0x51 and 0x52) and its mirror in tools/ant_pages.py -
 * encode_common_80(), encode_common_81(), encode_common_82(). Both are this
 * project's own prior derivation from the ANT+ common-page definitions, which
 * are open spec and implementable anywhere in this library
 * (docs/decisions/0008, fact 5, amending docs/decisions/0002-clean-room-policy.md).
 * No sdk-ant source was consulted and nothing here derives from libant.a.
 *
 * ---------------------------------------------------------------------------
 * Why these are here and not in each profile
 * ---------------------------------------------------------------------------
 * Heart rate 0x78 and bicycle power 0x0B both owe common pages 80/81 at the
 * 119/120-of-121 cadence, with identical bytes - the layout belongs to the
 * common-page definition, not the device type. Two copies would risk a
 * manufacturer id little-endian in one profile and big-endian in the other.
 *
 * Device type 0x60 does NOT come through here (deliberate): profile_sched.c
 * takes its common pages as node-supplied callbacks so the envelope has no
 * opinion about the manufacturer id. These functions are what an ANT+
 * compatibility profile hands it.
 *
 * ---------------------------------------------------------------------------
 * The serial number is a privacy decision and this file does not make it
 * ---------------------------------------------------------------------------
 */

#ifndef RADIANT_PROFILE_COMMON_H_
#define RADIANT_PROFILE_COMMON_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Named 80/81/82 in ANT+ documents, 0x50/0x51/0x52 on the air - same pages,
 * decimal names are not page numbers. */
#define PROFILE_COMMON_PAGE_80 0x50u
#define PROFILE_COMMON_PAGE_81 0x51u
#define PROFILE_COMMON_PAGE_82 0x52u
#define PROFILE_COMMON_PAGE_84 0x54u

/* Table 4-1's Common Data Page range, inclusive at both ends. 0x5D is 93, and
 * the table's own decimal column says "(64 - 93)" - so 0x5E is NOT a common
 * page, it is the first byte of the "Reserved for future use" row. Off-by-one
 * here would hand a reserved page to the common decoder on every channel. */
#define PROFILE_COMMON_PAGE_RANGE_FIRST 0x40u
#define PROFILE_COMMON_PAGE_RANGE_LAST  0x5Du

/* Invalid-value sentinels these pages use. Heart rate's computed-bpm byte
 * does NOT use them (0 = "no reading", 0xFF = real 255 bpm there), so that
 * sentinel lives in profile_hr.h instead. */
#define PROFILE_COMMON_INVALID_U8  0xFFu
#define PROFILE_COMMON_INVALID_U16 0xFFFFu
#define PROFILE_COMMON_INVALID_U32 0xFFFFFFFFu

/* Page 82's battery status nibble, byte [7] bits 6..4. */
#define PROFILE_COMMON_BATTERY_NEW      1u
#define PROFILE_COMMON_BATTERY_GOOD     2u
#define PROFILE_COMMON_BATTERY_OK       3u
#define PROFILE_COMMON_BATTERY_LOW      4u
#define PROFILE_COMMON_BATTERY_CRITICAL 5u

/* What pages 80 and 81 say about the node. Page 80's manufacturer id is 16
 * bits; heart rate page 0x02's is a separate 8-bit field - sharing one
 * struct member between them would truncate silently, so profile_hr.h
 * carries its own. */
struct profile_common_id {
	uint8_t  hw_revision;
	uint16_t manufacturer_id;
	uint16_t model_number;
	uint8_t  sw_revision_main;
	uint8_t  sw_revision_supplemental; /* PROFILE_COMMON_INVALID_U8 = unused */
	uint32_t serial_number;            /* PROFILE_COMMON_INVALID_U32 = not supplied */
};

struct profile_common_battery {
	uint32_t operating_time;     /* u24; units per `resolution_16s` */
	uint8_t  fractional_voltage; /* 1/256 V */
	uint8_t  coarse_voltage;     /* volts, low nibble */
	uint8_t  status;             /* PROFILE_COMMON_BATTERY_*, 3 bits */
	bool     resolution_16s;     /* false = 2 s units, which is bit 7 SET */
	uint8_t  battery_id;         /* 0x00 when there is only one battery */
};

/* Each writes eight bytes and returns 0, or -EINVAL for a null argument.
 * Reserved bytes are written too (0xFF), not skipped, so an uninitialised
 * body never puts stack contents on the air. */
int profile_common_80(const struct profile_common_id *id, uint8_t *body);
int profile_common_81(const struct profile_common_id *id, uint8_t *body);
int profile_common_82(const struct profile_common_battery *b, uint8_t *body);

/* ---------------------------------------------------------------------------
 * Page 84, Subfield Data - the decoder. See the header comment above for the
 * provenance, the transmission-type rule and the two table defects.
 * ---------------------------------------------------------------------------
 */

/* Table 6-17's subpage catalogue. Values 1-254 are page values for the two
 * data slots; 9-254 are "Reserved for future use" and this decoder passes them
 * through untouched rather than rejecting the whole page, because the OTHER
 * slot may still carry something we understand. */
#define PROFILE_COMMON_SUBPAGE_TEMPERATURE    1u  /* signed, 0.01 degC */
#define PROFILE_COMMON_SUBPAGE_PRESSURE       2u  /* unsigned, 0.01 kPa */
#define PROFILE_COMMON_SUBPAGE_HUMIDITY       3u  /* unsigned, 0.01 % */
#define PROFILE_COMMON_SUBPAGE_WIND_SPEED     4u  /* unsigned, 0.01 km/h */
#define PROFILE_COMMON_SUBPAGE_WIND_DIRECTION 5u  /* unsigned, 0.05 deg */
#define PROFILE_COMMON_SUBPAGE_CHARGE_CYCLES  6u  /* unsigned count, cumulative */
#define PROFILE_COMMON_SUBPAGE_TEMP_MIN       7u  /* signed, 0.01 degC - see defect (a) */
#define PROFILE_COMMON_SUBPAGE_TEMP_MAX       8u  /* signed, 0.01 degC - see defect (a) */
#define PROFILE_COMMON_SUBPAGE_RESERVED_FIRST 9u
#define PROFILE_COMMON_SUBPAGE_RESERVED_LAST  254u

/* Table 6-16, bytes [2] and [3]: "Invalid = 255 (0xFF)". Reuses the same value
 * as PROFILE_COMMON_INVALID_U8 and is spelled separately anyway - this one is
 * "the slot carries nothing", not "the field is not supplied", and a reader
 * following the name to the wrong table learns the wrong rule. */
#define PROFILE_COMMON_SUBPAGE_INVALID 0xFFu

/* Number of data slots in one page-84 message. Two, and the specification
 * offers no extension mechanism, so this is a constant rather than a limit. */
#define PROFILE_COMMON_84_SLOTS 2u

/* The section 6.13.1 worked example, kept here beside the layout it exercises
 * so the C test, the Python test and the header cannot drift apart:
 * payload 54 FF 01 03 6B 0A EA 19 is temperature 26.67 degC (subpage 1,
 * 0x0A6B = 2667) and humidity 66.34 % (subpage 3, 0x19EA = 6634). */
#define PROFILE_COMMON_84_GOLDEN_TEMP_CENTI   2667
#define PROFILE_COMMON_84_GOLDEN_HUMID_CENTI  6634

struct profile_common_subfield {
	/* The page value for this slot, exactly as byte [2]/[3] carried it.
	 * PROFILE_COMMON_SUBPAGE_INVALID, or anything in 9..254, means the
	 * caller should ignore `raw` - which is a normal answer, not an error. */
	uint8_t  subpage;

	/* The two data bytes as a little-endian u16, uninterpreted. Sign is the
	 * subpage's business, not this struct's: subpages 1, 7 and 8 are two's
	 * complement and the rest are unsigned, so widening here would need a
	 * per-subpage decision this layer deliberately does not make. Cast to
	 * int16_t at the point of use. */
	uint16_t raw;
};

struct profile_common_84 {
	struct profile_common_subfield slot[PROFILE_COMMON_84_SLOTS];
};

/*
 * Decodes an 8-byte page 84 body into BOTH (subpage, raw) pairs. Returns 0, or
 * -EINVAL for a null argument or a body whose byte [0] is not 0x54 - the same
 * convention profile_rd_decode_a() uses, and for the same reason: handing a
 * decoder the wrong page is a caller bug, whereas a subpage it does not know
 * is the sensor exercising a range the specification reserves for exactly that.
 *
 * Both slots are always written, including the invalid sentinel, so an
 * uninitialised struct never survives a successful call. Pure: no state, no
 * allocation, no logging - safe from a radio ISR.
 */
int profile_common_decode_84(const uint8_t *body, struct profile_common_84 *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_COMMON_H_ */
