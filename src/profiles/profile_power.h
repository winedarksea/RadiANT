/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_power.h - ANT+ Bicycle Power, device type 0x0B.
 *
 * Provenance: docs/device-profiles.md section "Bicycle Power, device type 0x0B"
 * (formerly docs/ant-plus-profiles.md, which that document absorbed; the
 * reference is by SECTION rather than by line number because line numbers in a
 * living document silently stop pointing at what they cited) - the byte tables
 * for pages 0x10 Standard Power Only, 0x11 Wheel
 * Torque, 0x12 Crank Torque and 0x20 Crank Torque Frequency, together with the
 * two power formulae. That document is this project's OWN prior clean-room
 * derivation, and its statement of provenance ("Facts and tables only. Every
 * layout here is derived from the encoders and decoders in tools/ant_pages.py
 * ... No prose is taken from any ANT+ device profile document") is the
 * derivation this file inherits. The byte-for-byte reference is
 * tools/ant_pages.py's encode_power_std(), encode_power_torque() and
 * encode_power_torque_freq(), checked against this file by
 * tools/test_compat_capture.py.
 *
 * ANT+ device profiles are open spec and may be implemented anywhere in this
 * library (fact 5 of docs/decisions/0008, amending
 * docs/decisions/0002-clean-room-policy.md). No sdk-ant source was consulted
 * and nothing here derives from libant.a.
 *
 * ---------------------------------------------------------------------------
 * THIS FILE CONTAINS NO CRYPTOGRAPHY AND CALLS radiant_sec NOWHERE
 * ---------------------------------------------------------------------------
 * Same boundary as profile_hr.c and for the same reason: everything a legacy
 * receiver sees is built here, everything RadiANT adds is built in
 * profile_compat.c and arrives through profile_sched.c's client seam.
 *
 * ---------------------------------------------------------------------------
 * Three traps this profile sets, all of them in the tables above
 * ---------------------------------------------------------------------------
 *   PAGE 0x20 IS BIG-ENDIAN. Every other multi-byte field in every other page
 *   of every other profile here is little-endian.
 *
 *   PAGES 0x11 AND 0x12 ARE INDEPENDENT ACCUMULATOR SERIES THAT SHARE A
 *   CHANNEL. A sensor emitting both advances two unrelated sets, so this
 *   module keeps one struct per series rather than one per node - differencing
 *   one against the other produces a plausible number rather than an error.
 *
 *   EVERY ACCUMULATOR IS MEANT TO WRAP. Widening one before subtracting is
 *   correct for hours and then reports a single absurd sample, which is easy to
 *   dismiss as radio noise.
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

/*
 * The permitted channel period, in counts of 1/32768 s: 8182, ~4.005 Hz. One
 * rate, so a compat power node has no choice to get wrong - and the constant
 * exists so that "one" is a recorded fact rather than an omission. No
 * reduced-rate variant is registered for 0x0B because this project has not
 * verified one against its own code, and the rule is that a number nobody has
 * verified does not get written down.
 */
#define PROFILE_POWER_PERIOD 8182u

#define PROFILE_POWER_PAGE_STANDARD     0x10u
#define PROFILE_POWER_PAGE_WHEEL_TORQUE 0x11u
#define PROFILE_POWER_PAGE_CRANK_TORQUE 0x12u
#define PROFILE_POWER_PAGE_TORQUE_FREQ  0x20u

/* Up to four data pages in the rotation, which is every page this profile
 * defines. A node emitting all four at once is not a sensor anybody ships; the
 * capacity is here so the array is not the limit. */
#define PROFILE_POWER_MAX_PAGES 4u

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
 * The master
 * ---------------------------------------------------------------------------
 */

int profile_power_init(struct profile_power *pw,
		       const struct profile_power_cfg *cfg);

/* Turn the RadiANT compat layer on. NULL turns it off, and off is the default.
 * Device type 0x0B has no page-change toggle, so this installs none - byte [0]
 * of every page here is a whole page number. */
int profile_power_set_compat(struct profile_power *pw,
			     struct profile_compat *compat);

/* One power event, for page 0x10: the event count advances, the accumulator
 * takes the instantaneous value, and both are meant to wrap. */
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
