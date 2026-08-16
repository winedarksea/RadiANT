/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_power.h - ANT+ Bicycle Power, device type 0x0B.
 *
 * Provenance: docs/device-profiles.md, "Bicycle Power, device type 0x0B" - byte
 * tables for pages 0x10 Standard Power Only, 0x11 Wheel Torque, 0x12 Crank
 * Torque and 0x20 Crank Torque Frequency, and the two power formulae. This
 * project's own clean-room derivation; byte-for-byte reference is
 * tools/ant_pages.py's encode_power_std(), encode_power_torque() and
 * encode_power_torque_freq(), checked against this file by
 * tools/test_compat_capture.py. Permitted under docs/decisions/0002 (fact 5 of
 * 0008). No sdk-ant source was consulted and nothing here derives from libant.a.
 *
 * No cryptography here and no call to radiant_sec, same boundary as
 * profile_hr.c: everything a legacy receiver sees is built here; RadiANT's
 * additions are built in profile_compat.c and arrive through profile_sched.c's
 * client seam.
 *
 * Three traps, all in the tables above:
 *   - PAGE 0x20 IS BIG-ENDIAN; every other multi-byte field in this repo is
 *     little-endian.
 *   - PAGES 0x11 AND 0x12 ARE INDEPENDENT ACCUMULATOR SERIES sharing a
 *     channel, so this module keeps one struct per series, not per node.
 *   - EVERY ACCUMULATOR IS MEANT TO WRAP.
 *
 * ---------------------------------------------------------------------------
 * The page 0x10 DECODER (added later than the encoders above)
 * ---------------------------------------------------------------------------
 */

#ifndef RADIANT_PROFILE_POWER_H_
#define RADIANT_PROFILE_POWER_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_common.h"
#include "profile_compat.h"
#include "profile_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_POWER_DEVICE_TYPE 0x0Bu

/* The permitted channel period, in counts of 1/32768 s: 8182, ~4.005 Hz. The
 * only rate; no reduced-rate variant is registered because none is verified. */
#define PROFILE_POWER_PERIOD 8182u

#define PROFILE_POWER_PAGE_STANDARD     0x10u
#define PROFILE_POWER_PAGE_WHEEL_TORQUE 0x11u
#define PROFILE_POWER_PAGE_CRANK_TORQUE 0x12u
#define PROFILE_POWER_PAGE_TORQUE_FREQ  0x20u

/* Up to four data pages in the rotation - every page this profile defines. */
#define PROFILE_POWER_MAX_PAGES 4u

/*
 * Page 0x10's two optional-field sentinels and byte 2's bit split (Table 8-1,
 * sections 8.2.1 and 8.3). Both sentinels are 0xFF, which is also
 * PROFILE_COMMON_INVALID_U8 - spelled out here anyway, because "the pedal
 * power byte is invalid" and "the cadence byte is invalid" are two independent
 * facts and a reader should not have to infer that they share a value.
 *
 * ZERO IS NOT A SENTINEL FOR EITHER. 0 rpm is a real reading (rider stopped)
 * and 0 % is a real pedal balance; profile_power_init() sets both to 0xFF for
 * this reason and a decoder must apply the same rule in reverse.
 */
#define PROFILE_POWER_INVALID_PEDAL_POWER 0xFFu
#define PROFILE_POWER_INVALID_CADENCE     0xFFu
#define PROFILE_POWER_PEDAL_RIGHT_BIT     0x80u /* section 8.2.1, bit 7 */
#define PROFILE_POWER_PEDAL_PERCENT_MASK  0x7Fu /* section 8.2.2, bits 0-6 */

/* The torque series, one per page. `ticks` counts wheel or crank ticks; the
 * other three are the accumulators the two power formulae difference. */
struct profile_power_torque {
	uint8_t  event_count;
	uint8_t  ticks;
	uint16_t acc_period; /* 1/2048 s */
	uint16_t acc_torque; /* 1/32 N.m */
};

struct profile_power_cfg {
	struct profile_common_id id;

	/* The data pages this node emits, in rotation order. */
	uint8_t pages[PROFILE_POWER_MAX_PAGES];
	uint8_t n_pages;

	/* Page 0x20's slope, 1/10 N.m/Hz. Ignored unless that page is in the
	 * rotation. */
	uint16_t slope_tenth_nm_hz;

	uint16_t common_82_every;
	struct profile_common_battery battery;
};

struct profile_power {
	struct profile_power_cfg cfg;
	struct profile_sched     sched;

	/* NULL is a plain ANT+ power meter, and that is the default. */
	struct profile_compat *compat;

	/* Page 0x10. */
	uint8_t  event_count;
	uint16_t acc_power;
	uint16_t inst_power;
	uint8_t  cadence;      /* PROFILE_COMMON_INVALID_U8 when not reported */
	uint8_t  pedal_power;  /* PROFILE_COMMON_INVALID_U8 when not reported */

	/* Pages 0x11 and 0x12, kept apart on purpose. */
	struct profile_power_torque wheel;
	struct profile_power_torque crank;

	/* Page 0x20. */
	uint8_t  freq_event_count;
	uint16_t time_stamp;   /* 1/2000 s */
	uint16_t torque_ticks;
};

/* ---------------------------------------------------------------------------
 * The page encoders. Pure, public, and each writes eight bytes.
 * ---------------------------------------------------------------------------
 */

int profile_power_encode_std(uint8_t event_count, uint8_t pedal_power,
			     uint8_t cadence, uint16_t acc_power,
			     uint16_t inst_power, uint8_t *out);

int profile_power_encode_torque(uint8_t page,
				const struct profile_power_torque *t,
				uint8_t cadence, uint8_t *out);

int profile_power_encode_torque_freq(uint8_t event_count,
				     uint16_t slope_tenth_nm_hz,
				     uint16_t time_stamp, uint16_t torque_ticks,
				     uint8_t *out);

/* ---------------------------------------------------------------------------
 * The page 0x10 decoder. Pure: no state, no radiant header, out-param only.
 * ---------------------------------------------------------------------------
 */

/*
 * One decoded page 0x10 (Rev 5.1 Table 8-1), every value still in its wire
 * scale. The two power fields are 1-watt resolution already, so there is no
 * conversion to get wrong; the two optional bytes carry an explicit valid flag
 * rather than a magic value, so a caller cannot forget which sentinel this
 * page uses.
 */
struct profile_power_std {
	uint8_t  event_count;   /* byte 1, rolls at 256 (Table 8-1) */
	uint16_t acc_power_w;   /* bytes 4-5 LE, running sum of watts, rolls at 65536 */
	uint16_t inst_power_w;  /* bytes 6-7 LE, watts */

	uint8_t  cadence_rpm;   /* byte 3; meaningless unless cadence_valid */
	bool     cadence_valid; /* byte 3 != 0xFF (section 8.3) */

	/* Byte 2, both as read and split. `pedal_percent` is the RIGHT pedal's
	 * share when pedal_right_known, and an undifferentiated single-pedal
	 * share when not (section 8.2.1) - the bit does not merely label the
	 * number, it changes what the number means. */
	uint8_t  pedal_percent;
	bool     pedal_right_known;
	bool     pedal_valid;   /* byte 2 != 0xFF */
};

/*
 * Decodes an 8-byte page 0x10 body into *out. Returns 0, or -EINVAL for a NULL
 * argument or a byte [0] that is not PROFILE_POWER_PAGE_STANDARD.
 *
 * *out is fully written on success (no field is left at whatever the caller
 * had there), so a caller may reuse one struct across a stream without
 * clearing it between messages.
 */
int profile_power_decode_std(const uint8_t *body, struct profile_power_std *out);

/* ---------------------------------------------------------------------------
 * The master
 * ---------------------------------------------------------------------------
 */

int profile_power_init(struct profile_power *pw,
		       const struct profile_power_cfg *cfg);

/* Turn the RadiANT compat layer on. NULL turns it off (the default). Device
 * type 0x0B has no page-change toggle, so this installs none. */
int profile_power_set_compat(struct profile_power *pw,
			     struct profile_compat *compat);

/* One power event, for page 0x10: event count and accumulator both wrap. */
void profile_power_event(struct profile_power *pw, uint16_t watts,
			 uint8_t cadence);

/* One revolution, for page 0x11 or 0x12. `crank` picks the series. */
void profile_power_torque_event(struct profile_power *pw, bool crank,
				uint16_t period_2048, uint16_t torque_32nm);

/* One event for page 0x20, in that page's own units. */
void profile_power_freq_event(struct profile_power *pw, uint16_t d_time_2000,
			      uint16_t d_ticks);

void profile_power_set_pedal_balance(struct profile_power *pw, uint8_t percent);

enum profile_slot_kind profile_power_next(struct profile_power *pw,
					  uint64_t now_us, uint8_t *body);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_POWER_H_ */
