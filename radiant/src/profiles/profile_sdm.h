/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_sdm.h - ANT+ Stride Based Speed and Distance Monitor (SDM),
 * device type 0x7C.
 * ---------------------------------------------------------------------------
 * WHY A TREADMILL EMITS THIS AT ALL, BESIDE FE-C
 * ---------------------------------------------------------------------------
 * FE-C is what a control-capable head unit pairs with. SDM is what everything
 * that only knows foot pods pairs with, which includes Zwift Run and most
 * watches. They are different device types with different periods and no
 * shared page space, so this is a second, independent master - two
 * profile_sched instances, not a new scheduler concept.
 *
 * ---------------------------------------------------------------------------
 * FOUR THINGS ABOUT THIS PROFILE THAT ARE THE OPPOSITE OF EVERY OTHER ONE HERE
 * ---------------------------------------------------------------------------
 * 1. THE PERIOD IS 8134, NOT 8192 AND NOT 8070. It is close enough to both to
 *    look like a typo in a diff and far enough that a receiver told 8070 never
 *    finds the sensor. A wrong channel period does not fail loudly - the
 *    channel simply never opens and nothing anywhere names the period as the
 *    reason. This project has already lost bench sessions to exactly that.
 *
 * 2. THERE IS NO 0xFF INVALID CONVENTION. Every other profile in this
 *    directory marks an absent field with all ones. Here an unused field is
 *    ZERO and validity is out of band, in page 22's capability bits. So 0xFF
 *    in a distance byte is 255 metres, 0xFF in a cadence byte is 255
 *    strides/min, and a decoder that reached for the familiar sentinel would
 *    delete real readings. Nothing in this file writes
 *    PROFILE_COMMON_INVALID_U8 and that is not an oversight.
 *
 * 3. "PAGE 16" IS 0x10 AND "PAGE 22" IS 0x16, AND THE DIGITS ARE THE SAME
 *    TWO. The document names pages in decimal and the wire carries hex, so the
 *    summary page (16 decimal, 0x10) and the capabilities page (22 decimal,
 *    0x16) swap places for anybody who reads one number in the other base.
 *    Both spellings appear on every constant below for that reason.
 *
 * 4. STRIDES ARE NOT STEPS. One stride is two footfalls, so a runner at 170
 *    steps per minute is at 85 strides per minute here. FE-C page 19 also
 *    counts strides; BLE RSC counts STEPS and every real client expects steps
 *    there. That is one conversion in one direction, and the whole reason
 *    apps/treadmill has a single helper for it with its own unit test - see
 *    treadmill_rsc_cadence_from_strides().
 */

#ifndef RADIANT_PROFILE_SDM_H_
#define RADIANT_PROFILE_SDM_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_common.h"
#include "profile_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_SDM_DEVICE_TYPE 0x7Cu /* 124 decimal */

/* Counts of 1/32768 s. 8134 is ~4.03 Hz - see trap 1 in the header. */
#define PROFILE_SDM_PERIOD 8134u

/* Part of the channel id a receiver matches on, and the same value
 * apps/hrm_ble, apps/sim and tools/ant_sim.py use for their masters. */
#define PROFILE_SDM_TRANS_TYPE 5u

#define PROFILE_SDM_PAGE_DEFAULT      0x01u /* page 1 */
#define PROFILE_SDM_PAGE_BASE         0x02u /* page 2 */
#define PROFILE_SDM_PAGE_CALORIES     0x03u /* page 3 */
#define PROFILE_SDM_PAGE_SUMMARY      0x10u /* page 16 decimal - see trap 3 */
#define PROFILE_SDM_PAGE_CAPABILITIES 0x16u /* page 22 decimal - see trap 3 */

/* ---------------------------------------------------------------------------
 * The status byte, shared by pages 2 and 3 - byte [7]
 * ---------------------------------------------------------------------------
 *   bits 7..6  SDM location
 *   bits 5..4  battery status
 *   bits 3..2  SDM health
 *   bits 1..0  use state
 */
#define PROFILE_SDM_LOC_LACES   0u
#define PROFILE_SDM_LOC_MIDSOLE 1u
#define PROFILE_SDM_LOC_OTHER   2u /* a treadmill is not on anybody's shoe */
#define PROFILE_SDM_LOC_ANKLE   3u

#define PROFILE_SDM_BATTERY_NEW  0u
#define PROFILE_SDM_BATTERY_GOOD 1u
#define PROFILE_SDM_BATTERY_OK   2u
#define PROFILE_SDM_BATTERY_LOW  3u

#define PROFILE_SDM_HEALTH_OK      0u
#define PROFILE_SDM_HEALTH_ERROR   1u
#define PROFILE_SDM_HEALTH_WARNING 2u

#define PROFILE_SDM_USE_INACTIVE 0u
#define PROFILE_SDM_USE_ACTIVE   1u

/* Pack and unpack byte [7]. Two functions rather than open-coded shifts at
 * five call sites, because the four fields are two bits each and a shift that
 * is two out still produces a legal-looking status. */
uint8_t profile_sdm_status(uint8_t location, uint8_t battery, uint8_t health,
			   uint8_t use_state);
void profile_sdm_status_split(uint8_t status, uint8_t *location,
			      uint8_t *battery, uint8_t *health,
			      uint8_t *use_state);

/* ---------------------------------------------------------------------------
 * The split scalars
 * ---------------------------------------------------------------------------
 * Speed is an integer m/s in a NIBBLE plus a 1/256 m/s byte, so the whole
 * field tops out at 15.996 m/s (~57.6 km/h) - far above any treadmill and
 * still a real ceiling worth refusing rather than wrapping.
 *
 * Distance is an integer-metre accumulator plus a 1/16 m nibble; cadence is an
 * integer strides/min plus a 1/16 nibble. Three different fractional
 * denominators on one profile (200, 256, 16), which is why each helper names
 * its own.
 */
#define PROFILE_SDM_SPEED_MAX_MM_S 15996u
#define PROFILE_SDM_SPEED_FRAC_DEN 256u
#define PROFILE_SDM_DIST_FRAC_DEN  16u
#define PROFILE_SDM_CAD_FRAC_DEN   16u
#define PROFILE_SDM_TIME_FRAC_DEN  200u
#define PROFILE_SDM_LATENCY_DEN    32u

/* Returns 0, or -EINVAL above PROFILE_SDM_SPEED_MAX_MM_S. */
int profile_sdm_speed_split(uint32_t mm_s, uint8_t *int_mps,
			    uint8_t *frac_256);
uint32_t profile_sdm_speed_mm_s(uint8_t int_mps, uint8_t frac_256);

/* ---------------------------------------------------------------------------
 * Page 1 (0x01) Default Data
 * ---------------------------------------------------------------------------
 *   [0] page number, 0x01
 *   [1] time, fractional, 1/200 s
 *   [2] time, integer seconds - accumulator, wraps at 256 s
 *   [3] distance, integer metres - accumulator, wraps at 256 m
 *   [4] bits 7..4 distance fractional (1/16 m); bits 3..0 speed integer (m/s)
 *   [5] speed fractional, 1/256 m/s
 *   [6] STRIDE COUNT - accumulator, wraps at 256, +1 per TWO footfalls
 *   [7] update latency, 1/32 s
 *
 * The stride count is required rather than optional: it is the field that
 * survives a lost packet, exactly as the beat count is on heart rate, and a
 * receiver differences it rather than integrating the cadence.
 */
struct profile_sdm_default {
	uint8_t time_frac_200;
	uint8_t time_s;
	uint8_t distance_m;
	uint8_t distance_frac_16; /* 0..15 */
	uint8_t speed_int_mps;    /* 0..15 */
	uint8_t speed_frac_256;
	uint8_t strides;
	uint8_t latency_32;
};

int profile_sdm_encode_default(const struct profile_sdm_default *in,
			       uint8_t *out);
int profile_sdm_decode_default(const uint8_t *body,
			       struct profile_sdm_default *out);

/* ---------------------------------------------------------------------------
 * Pages 2 (0x02) Base and 3 (0x03) Calories - ONE template, two page numbers
 * ---------------------------------------------------------------------------
 *   [0] page number
 *   [1] reserved, 0x00 (NOT 0xFF - see trap 2)
 *   [2] reserved, 0x00
 *   [3] cadence, integer strides/min
 *   [4] bits 7..4 cadence fractional (1/16); bits 3..0 speed integer (m/s)
 *   [5] speed fractional, 1/256 m/s
 *   [6] page 3: accumulated calories (kcal, accumulator). Page 2: reserved 0x00
 *   [7] status byte
 *
 * One struct and one encoder taking the page number, rather than two of each,
 * because the only difference is byte [6] - and two copies of a shared layout
 * is how a fractional denominator ends up different on one of them.
 */
struct profile_sdm_supplementary {
	uint8_t cadence_strides_min;
	uint8_t cadence_frac_16; /* 0..15 */
	uint8_t speed_int_mps;   /* 0..15 */
	uint8_t speed_frac_256;
	uint8_t calories;        /* page 3 only; ignored when encoding page 2 */
	uint8_t status;          /* profile_sdm_status() */
};

int profile_sdm_encode_supplementary(uint8_t page,
				     const struct profile_sdm_supplementary *in,
				     uint8_t *out);
int profile_sdm_decode_supplementary(const uint8_t *body,
				     struct profile_sdm_supplementary *out);

/* ---------------------------------------------------------------------------
 * Page 16 decimal (0x10) Distance and Strides Summary - request-only
 * ---------------------------------------------------------------------------
 *   [0]    page number, 0x10
 *   [1..3] cumulative strides, 24-bit LE
 *   [4..7] cumulative distance, 32-bit LE, 1/256 m
 *
 * Wider than the accumulators on page 1 and answering a different question:
 * page 1's roll every 256 units and are for differencing, this one is the
 * session total and is for displaying. A receiver that differenced this would
 * be right and slow; one that displayed page 1's would be wrong every 256 m.
 */
struct profile_sdm_summary {
	uint32_t strides;      /* 24-bit */
	uint32_t distance_256; /* 1/256 m */
};

int profile_sdm_encode_summary(const struct profile_sdm_summary *in,
			       uint8_t *out);
int profile_sdm_decode_summary(const uint8_t *body,
			       struct profile_sdm_summary *out);

/* ---------------------------------------------------------------------------
 * Page 22 decimal (0x16) Capabilities - REQUIRED on request in Rev 1.6
 * ---------------------------------------------------------------------------
 *   [0]    page number, 0x16
 *   [1]    capability flags
 *   [2..7] reserved, 0x00
 *
 * This page IS the validity convention this profile has instead of sentinels
 * (trap 2). A sensor that never answers page 22 is a sensor whose zeros a
 * receiver cannot distinguish from readings.
 */
#define PROFILE_SDM_CAP_TIME     (1u << 0)
#define PROFILE_SDM_CAP_DISTANCE (1u << 1)
#define PROFILE_SDM_CAP_SPEED    (1u << 2)
#define PROFILE_SDM_CAP_LATENCY  (1u << 3)
#define PROFILE_SDM_CAP_CADENCE  (1u << 4)
#define PROFILE_SDM_CAP_CALORIES (1u << 5)

struct profile_sdm_capabilities {
	uint8_t flags;
};

int profile_sdm_encode_capabilities(const struct profile_sdm_capabilities *in,
				    uint8_t *out);
int profile_sdm_decode_capabilities(const uint8_t *body,
				    struct profile_sdm_capabilities *out);

/* ---------------------------------------------------------------------------
 * The master
 * ---------------------------------------------------------------------------
 * The interleave: 1, 1, X, X repeating for 64 messages, then the common pair
 * twice consecutively - a 66-message cycle. X alternates between page 2 and
 * page 3 group by group, so each appears in eight of the sixteen groups.
 *
 * 66 IS NOT 121, so - exactly as in profile_fec_tx.c, and for exactly the same
 * reason - the common pair cannot come out of profile_sched.c's built-in
 * 119/120-of-121 placement and rides the client seam instead.
 * profile_sdm_next() reports it back as PROFILE_SLOT_COMMON_80/81.
 */
#define PROFILE_SDM_GROUP_SLOTS     4u
#define PROFILE_SDM_DATA_MESSAGES   64u
#define PROFILE_SDM_CYCLE           (PROFILE_SDM_DATA_MESSAGES + 2u)

struct profile_sdm_cfg {
	struct profile_common_id id;

	/* The status byte's three standing fields. A treadmill is
	 * PROFILE_SDM_LOC_OTHER and PROFILE_SDM_BATTERY_NEW: it is not on a
	 * shoe and it is not on a cell. */
	uint8_t location;
	uint8_t battery;
	uint8_t health;

	/* Page 22's answer. Set the bits whose fields this machine really
	 * produces; on this profile that IS the validity signal. */
	struct profile_sdm_capabilities caps;
};

struct profile_sdm {
	struct profile_sdm_cfg cfg;
	struct profile_sched   sched;

	struct profile_sdm_default       page1;
	struct profile_sdm_supplementary supp;
	struct profile_sdm_summary       summary;

	uint32_t messages;
	uint32_t group;
	uint8_t  group_slot;

	/* Which page the X slots of this group carry: page 2 or page 3. */
	bool group_is_calories;

	uint8_t on_request_page;
	uint8_t on_request_left;

	uint8_t rotation[1];
};

int profile_sdm_init(struct profile_sdm *sdm, const struct profile_sdm_cfg *cfg);

/*
 * What the node reports. `use_state` is folded into the status byte here so a
 * caller never has to pack it: a treadmill is ACTIVE while the belt moves and
 * INACTIVE otherwise, and that is the whole of the state machine.
 */
void profile_sdm_set_motion(struct profile_sdm *sdm, uint32_t speed_mm_s,
			    uint8_t cadence_strides_min,
			    uint8_t cadence_frac_16, bool active);

/* Page 1's time, distance and stride accumulators, and the update latency. */
void profile_sdm_set_accumulators(struct profile_sdm *sdm, uint32_t elapsed_ms,
				  uint64_t distance_mm, uint32_t strides,
				  uint8_t latency_32);

/* Page 3's accumulated calories. */
void profile_sdm_set_calories(struct profile_sdm *sdm, uint8_t kcal);

/* A request arrived. 0, or -ENOTSUP for a page this master cannot produce on
 * request (supported: 16 decimal, 22 decimal, 80, 81). */
int profile_sdm_request(struct profile_sdm *sdm, uint8_t page, uint8_t count);

enum profile_slot_kind profile_sdm_next(struct profile_sdm *sdm, uint8_t *body);

uint32_t profile_sdm_messages(const struct profile_sdm *sdm);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_SDM_H_ */
