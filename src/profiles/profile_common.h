/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_common.h - ANT+ common pages 80, 81 and 82.
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
 * Heart rate 0x78 and bicycle power 0x0B both owe common pages 80 and 81 at
 * the 119/120 of 121 cadence, and they owe exactly the same bytes: the page
 * layouts belong to the common-page definition, not to either device type.
 * One implementation shared by two device types is one implementation; two
 * copies is a place for a manufacturer id to be little-endian in one profile
 * and big-endian in the other, which is the kind of bug a receiver reports as
 * "sensor works, name is garbage".
 *
 * Device type 0x60 does NOT come through here, and that is deliberate rather
 * than an oversight: profile_sched.c takes its common pages as node-supplied
 * callbacks precisely so the envelope has no opinion about what a
 * manufacturer id is. These functions are what an ANT+ compatibility profile
 * hands it.
 *
 * ---------------------------------------------------------------------------
 * The serial number is a privacy decision and this file does not make it
 * ---------------------------------------------------------------------------
 * PROFILE_COMMON_INVALID_U32 in `serial_number` emits page 81's not-supplied
 * sentinel, and that is the whole of the page 81 privacy rule
 * (docs/radiant-security.md section 5.4): a 32-bit globally unique serial
 * broadcast in the clear every 30 seconds is STRICTLY MORE IDENTIFYING than
 * the 16-bit device number, it survives any device-number re-roll, and a node
 * that re-rolls its device number while broadcasting its serial has not
 * changed identity - it has added a field.
 *
 * The sentinel is not the default here because a struct has no defaults; it is
 * what profile_hr_init() and profile_power_init() install when the node says
 * nothing, which is where a default belongs.
 */

#ifndef RADIANT_PROFILE_COMMON_H_
#define RADIANT_PROFILE_COMMON_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The three page numbers. Named 80/81/82 everywhere in the ANT+ documents and
 * 0x50/0x51/0x52 on the air, which is a trap worth one line of header: they
 * are the same pages and the decimal names are not page numbers. */
#define PROFILE_COMMON_PAGE_80 0x50u
#define PROFILE_COMMON_PAGE_81 0x51u
#define PROFILE_COMMON_PAGE_82 0x52u

/* The invalid-value sentinels these pages use. Heart rate's computed-bpm byte
 * does NOT use them - 0 means "no reading" there and 0xFF is a real 255 bpm -
 * which is why that sentinel lives in profile_hr.h and not here. */
#define PROFILE_COMMON_INVALID_U8  0xFFu
#define PROFILE_COMMON_INVALID_U16 0xFFFFu
#define PROFILE_COMMON_INVALID_U32 0xFFFFFFFFu

/* Page 82's battery status nibble, byte [7] bits 6..4. */
#define PROFILE_COMMON_BATTERY_NEW      1u
#define PROFILE_COMMON_BATTERY_GOOD     2u
#define PROFILE_COMMON_BATTERY_OK       3u
#define PROFILE_COMMON_BATTERY_LOW      4u
#define PROFILE_COMMON_BATTERY_CRITICAL 5u

/*
 * What pages 80 and 81 say about the node.
 *
 * Page 80's manufacturer id is SIXTEEN bits and heart rate page 0x02's is
 * EIGHT. They are different fields of different widths in different documents,
 * and a node that shares one struct member between them truncates silently -
 * which is why profile_hr.h carries its own 8-bit id rather than reaching in
 * here for this one.
 */
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

/*
 * Each writes eight bytes and returns 0, or -EINVAL for a null argument.
 *
 * Every reserved byte is written, not skipped: a caller handing these an
 * uninitialised body would otherwise put stack contents on the air in the
 * bytes the layout reserves, and 0xFF is what the reservation says.
 */
int profile_common_80(const struct profile_common_id *id, uint8_t *body);
int profile_common_81(const struct profile_common_id *id, uint8_t *body);
int profile_common_82(const struct profile_common_battery *b, uint8_t *body);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_COMMON_H_ */
