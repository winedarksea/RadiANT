/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_hr.h - ANT+ Heart Rate, device type 0x78.
 *
 * Provenance: the ANT+ Heart Rate Monitor device profile, as published in the
 * HRM sample documentation on thisisant.com - publicly retrievable, cited by
 * Colin on 2026-08-11 as the source for this device type. The layout facts it
 * fixes are: byte [0] bits 6..0 are the page number and bit 7 is the
 * page-change toggle; bytes [4..7] are the same heartbeat event time, beat
 * count and computed heart rate on EVERY page; pages 0x00 default, 0x01
 * cumulative operating time, 0x02 manufacturer information, 0x03 product
 * information and 0x04 previous heartbeat time; and the main-page /
 * background-page split.
 *
 * Those facts are already written down in this repository twice, and this file
 * was derived from those rather than re-derived from the document:
 * docs/ant-plus-profiles.md section "Heart Rate, device type 0x78" (lines
 * 150-206), and tools/ant_pages.py's encode_hr_default(),
 * encode_hr_cumulative_time(), encode_hr_manufacturer(), encode_hr_product()
 * and encode_hr_previous_beat(), which are the byte-for-byte reference this
 * file's output is cross-checked against by tools/test_compat_capture.py.
 *
 * ANT+ device profiles are open spec and may be implemented anywhere in this
 * library (Colin's ruling of 2026-08-10, recorded as fact 5 of
 * docs/decisions/0008 and amending docs/decisions/0002-clean-room-policy.md).
 * No sdk-ant source was consulted and nothing here derives from libant.a.
 *
 * ---------------------------------------------------------------------------
 * THIS FILE CONTAINS NO CRYPTOGRAPHY AND CALLS radiant_sec NOWHERE
 * ---------------------------------------------------------------------------
 * That is the compatibility claim, expressed as a file boundary rather than as
 * a promise. Everything a legacy receiver sees is built here; everything
 * RadiANT adds is built in profile_compat.c and arrives through
 * profile_sched.c's client seam. A node turns the second one on by handing
 * this module a `struct profile_compat *` and off by not doing so, and the
 * stream in the second case is a stock ANT+ heart-rate strap - not a degraded
 * mode, but the configuration most straps should ship in.
 *
 * The include list is the check: nothing here reaches a radiant_core header,
 * even transitively.
 *
 * ---------------------------------------------------------------------------
 * Two rules of this profile that are easy to get subtly wrong
 * ---------------------------------------------------------------------------
 *   THE TOGGLE COUNTS TRANSMITTED MESSAGES, NOT DATA PAGES. It flips every
 *   four messages, and the common pages and the compat pages are messages. A
 *   toggle driven off the data-page count would drift out of step with the
 *   sensor's own message sequence every time a common page went out, and the
 *   toggle sequence in the presence of inserted pages is precisely the
 *   question docs/decisions/0008 sends to the bench.
 *
 *   PAGE 0x04 IS THE MAIN PAGE AND 0x00 IS NOT. Two event times on one page is
 *   what lets a receiver compute an R-R interval from a single packet instead
 *   of differencing two, which is why a modern strap sends 0x04 every message
 *   and rotates 0x00..0x03 through the background slot.
 */

#ifndef RADIANT_PROFILE_HR_H_
#define RADIANT_PROFILE_HR_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_common.h"
#include "profile_compat.h"
#include "profile_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_HR_DEVICE_TYPE 0x78u

/*
 * The three permitted channel periods, in counts of 1/32768 s. Unlike a page
 * number a period is not something a receiver can skip: it has to match or the
 * channel does not open at all, and nothing on either side reports the period
 * as the reason. That asymmetry is why the set is enumerated here and checked
 * against docs/profile-registry.md rather than left as a number somewhere.
 *
 *   8070   ~4.06 Hz  standard, and the default
 *   16140  ~2.03 Hz  half rate
 *   32280  ~1.02 Hz  quarter rate
 *
 * Tier I's interval T is in SECONDS, so a slower rate spends proportionally
 * LESS of its slots on RadiANT, not more.
 */
#define PROFILE_HR_PERIOD         8070u
#define PROFILE_HR_PERIOD_HALF    16140u
#define PROFILE_HR_PERIOD_QUARTER 32280u

#define PROFILE_HR_PAGE_DEFAULT        0x00u
#define PROFILE_HR_PAGE_CUMULATIVE     0x01u
#define PROFILE_HR_PAGE_MANUFACTURER   0x02u
#define PROFILE_HR_PAGE_PRODUCT        0x03u
#define PROFILE_HR_PAGE_PREVIOUS_BEAT  0x04u

#define PROFILE_HR_PAGE_TOGGLE 0x80u
#define PROFILE_HR_PAGE_MASK   0x7Fu

/* Event times are 1/1024 s here - not the 1/2048 s of the power torque pages
 * and not the 1/2000 s of power page 0x20. Three profiles, three time bases. */
#define PROFILE_HR_EVENT_TIME_HZ 1024u

/* Cumulative operating time counts two-second units, so page 0x01's 24-bit
 * field covers about 388 days. */
#define PROFILE_HR_OPERATING_UNIT_S 2u

/*
 * "No reading" is 0 on this byte and NOT 0xFF, which is the one place in this
 * profile where the common invalid-value sentinel does not apply: 0xFF here is
 * a real 255 bpm, and a decoder that reached for the sentinel reports it.
 */
#define PROFILE_HR_INVALID_BPM 0u

/* The toggle flips every four messages, and one background page is sent every
 * 64 data pages. Both are the cadence tools/ant_sim.py's HeartRateSensor
 * emits, so a capture from this module and a capture from the simulator are
 * the same stream. */
#define PROFILE_HR_TOGGLE_INTERVAL     4u
#define PROFILE_HR_BACKGROUND_INTERVAL 64u
#define PROFILE_HR_BACKGROUND_PAGES    4u

struct profile_hr_cfg {
	/* Common pages 80 and 81, and page 82 when the node has a battery. */
	struct profile_common_id id;

	/*
	 * Page 0x02's manufacturer id is EIGHT bits where common page 80's is
	 * sixteen, and page 0x03's model number is EIGHT where page 80's is
	 * sixteen. They are different fields in different documents and this
	 * struct keeps them apart, because a node that truncated page 80's
	 * value into them would do it silently.
	 */
	uint8_t  manufacturer_id_8;
	uint16_t serial_lsb;
	uint8_t  model_number_8;
	uint8_t  hw_version;
	uint8_t  sw_version;

	/* Page 82 every N data slots; 0 means never, and never is a perfectly
	 * good answer for a strap with a coin cell it cannot measure. */
	uint16_t common_82_every;
	struct profile_common_battery battery;
};

struct profile_hr {
	struct profile_hr_cfg cfg;
	struct profile_sched  sched;

	/* NULL is a plain ANT+ strap, and that is the default. */
	struct profile_compat *compat;

	/* Transmitted messages, which is what the toggle counts. */
	uint32_t messages;

	/* Data pages, which is what the background rotation counts. Two
	 * counters because they count two different things, and one counter
	 * doing both is the bug this comment exists to prevent. */
	uint32_t data_pages;
	uint8_t  background_next;

	/* The physiology, as the node reports it. */
	uint16_t event_time;
	uint16_t previous_event_time;
	uint8_t  beats;
	uint8_t  computed_hr;
	uint32_t operating_time_2s;

	uint8_t  rotation[1];
};

/* ---------------------------------------------------------------------------
 * The page encoders
 *
 * Pure, and public, because tools/test_ant_pages.py's C-side mirror is a
 * per-page comparison rather than a whole-stream one: a test that could only
 * compare streams would report "the seventh byte of the fortieth message
 * differs" where this reports "page 0x02 puts the serial in the wrong order".
 *
 * Each writes eight bytes and returns 0, or -EINVAL.
 * ---------------------------------------------------------------------------
 */

/* Bytes [0] and [4..7]: the page number with its toggle, and the tail every
 * page of this device type carries unchanged. */
int profile_hr_encode(uint8_t page, const uint8_t body3[3], uint16_t event_time,
		      uint8_t beats, uint8_t computed_hr, bool toggle,
		      uint8_t *out);

int profile_hr_encode_default(const struct profile_hr *hr, bool toggle,
			      uint8_t *out);
int profile_hr_encode_cumulative(const struct profile_hr *hr, bool toggle,
				 uint8_t *out);
int profile_hr_encode_manufacturer(const struct profile_hr *hr, bool toggle,
				   uint8_t *out);
int profile_hr_encode_product(const struct profile_hr *hr, bool toggle,
			      uint8_t *out);
int profile_hr_encode_previous_beat(const struct profile_hr *hr, bool toggle,
				    uint8_t *out);

/* ---------------------------------------------------------------------------
 * The master
 * ---------------------------------------------------------------------------
 */

int profile_hr_init(struct profile_hr *hr, const struct profile_hr_cfg *cfg);

/*
 * Turn the RadiANT compat layer on. NULL turns it off, and off is the default.
 *
 * The instance is the node's, not this module's: it holds a key, and a struct
 * holding a key has no business inside the file whose claim is that it holds
 * none. What this call does is register it as the scheduler's client and lend
 * it this profile's toggle, which is the only thing about heart rate the
 * compat layer needs to know.
 */
int profile_hr_set_compat(struct profile_hr *hr, struct profile_compat *compat);

/* The toggle for the message about to go out. Exposed because the compat layer
 * needs it and because a test can then assert the sequence directly. */
bool profile_hr_toggle(const struct profile_hr *hr);

/* One beat. `event_time` is the beat's absolute time in 1/1024 s, wrapping at
 * 65536 - the node's clock, not this module's, for the same reason
 * radiant_sec_compat.h takes every instant as an argument. */
void profile_hr_beat(struct profile_hr *hr, uint16_t event_time);

/* The sensor's own bpm answer, a convenience beside the accumulator pair that
 * is what actually survives a lost packet. 0 means no reading. */
void profile_hr_set_computed(struct profile_hr *hr, uint8_t bpm);

/* Seconds since the battery went in, for page 0x01. Converted to the page's
 * two-second units here so a caller never has to remember which of the three
 * operating-time fields in this project uses which unit. */
void profile_hr_set_operating_time(struct profile_hr *hr, uint32_t seconds);

/*
 * Fill `body` with the next message and say what it was.
 *
 * `now_us` is the instant this message goes on the air, and it reaches the
 * attestation layer unchanged - this module never reads a clock.
 */
enum profile_slot_kind profile_hr_next(struct profile_hr *hr, uint64_t now_us,
				       uint8_t *body);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_HR_H_ */
