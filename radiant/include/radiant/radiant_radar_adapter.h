/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radar_adapter.h - ANT+ Bike Radar (device type 0x28) onto the
 * sample bus.
 *
 * SAFETY: telemetry, not a certified radar display. A sink that shows these
 * values to a rider is outside what this project claims.
 *
 * What is published, per radar source:
 *   AVAILABLE     boolean: 1 while target information is current. Goes 0
 *                 when no page 48/49 has arrived for 2 s (spec 6.4.1, a
 *                 TIME rule, so radiant_radar_adapter_tick() must be called
 *                 from a timer, not only from received frames), on page 87
 *                 (6.4.2), on device state Shutdown Requested/Forced (6.7.1)
 *                 and on the clear-targets bit (6.7.2). Starts at 0: a
 *                 display shows "unavailable" until the first target page
 *                 (6.4.1). Shutdown Requested/Forced LATCHES it at 0: in
 *                 that pattern page 1 alternates with page 48 (6.3.3), and
 *                 without the latch every page 48 would flip it back to 1
 *                 at 8 Hz. The latch releases on a page 1 with state 0 or
 *                 2, or after 2 s with no page 1.
 *   TARGET_COUNT  enum 0..8, targets with threat > 0
 *   THREAT        enum 0..3, the highest threat of any target
 *   NEAREST_RANGE length (m, exp -3), the closest target - computed here,
 *                 because Rev 2.1 says targets are NOT sorted
 *   NEAREST_SPEED speed (m/s, exp -3), that target's closing speed
 *   NEAREST_SIDE  enum 0 behind / 1 right / 2 left
 *   DEVICE_STATE  enum, page 1's state, whenever page 1 arrives
 *   ERROR_CODE    enum, page 87's profile error code (0 saturated, 1 skew)
 *
 * A target page with zero targets posts AVAILABLE=1, TARGET_COUNT=0,
 * THREAT=0 and nothing for the nearest-target fields (there is no nearest
 * target; a zero range would claim a vehicle at the wheel). Target pages
 * arrive at ~8 Hz; values are posted when they change, and all of them
 * every RADIANT_RADAR_REPOST pages so a late subscriber catches up.
 */

#ifndef RADIANT_RADAR_ADAPTER_H_
#define RADIANT_RADAR_ADAPTER_H_

#include <stdbool.h>
#include <stdint.h>

#include <radiant/radiant_bridge.h>
#include <radiant/radiant_profile_radar.h>

#ifdef __cplusplus
extern "C" {
#endif

/* field_ids. The suffix table in radiant_naming.c is keyed on (field type,
 * field id) across EVERY adapter, so these avoid the pairs other profiles
 * already name: THREAT is id 0 (the headline - composes to the bare
 * "Radar 25738"), the rest sit at 0x10-0x16, which no profile uses. */
#define RADIANT_RADAR_FIELD_THREAT        0x00u /* 0x40 enum 0..3 */
#define RADIANT_RADAR_FIELD_AVAILABLE     0x10u /* 0x01 boolean */
#define RADIANT_RADAR_FIELD_TARGET_COUNT  0x11u /* 0x40 enum 0..8 */
#define RADIANT_RADAR_FIELD_NEAREST_RANGE 0x12u /* 0x14 length, exp -3 */
#define RADIANT_RADAR_FIELD_NEAREST_SPEED 0x13u /* 0x16 speed, exp -3 */
#define RADIANT_RADAR_FIELD_NEAREST_SIDE  0x14u /* 0x40 enum */
#define RADIANT_RADAR_FIELD_DEVICE_STATE  0x15u /* 0x40 enum */
#define RADIANT_RADAR_FIELD_ERROR_CODE    0x16u /* 0x40 enum */

/* Spec 6.4.1: 2 s without a target page = unavailable. */
#define RADIANT_RADAR_TIMEOUT_US 2000000u
/* Re-post everything every N target pages (~8 s at 8 Hz). */
#define RADIANT_RADAR_REPOST     64u
/* Targets 5-8 are dropped when no page 49 has arrived for this long. A radar
 * reporting > 4 targets alternates 48/49, so page 49 comes every 250 ms;
 * 750 ms rides out one lost page 49 without the count flickering. */
#define RADIANT_RADAR_PAGE_B_EXPIRY_US 750000u

struct radiant_radar_adapter {
	struct radiant_profile_radar_target targets[RADIANT_PROFILE_RADAR_MAX_TARGETS];
	uint64_t last_target_us; /* t of the last page 48/49 */
	uint64_t last_b_us;      /* t of the last page 49 */
	uint64_t last_status_us; /* t of the last page 1 */
	bool     have_targets;   /* at least one target page since unavailable */
	bool     shutdown_latch; /* page 1 said Shutdown Requested/Forced */
	bool     available;
	bool     posted_available;
	uint16_t pages_since_repost;
	/* Last posted values, for post-on-change; -1 = never posted. */
	int32_t  last_count;
	int32_t  last_threat;
	int32_t  last_range_code;
	int32_t  last_speed_code;
	int32_t  last_side;
	int32_t  last_state;
	int32_t  last_error;
};

void radiant_radar_adapter_init(struct radiant_radar_adapter *a);

/* One received 8-byte payload from the radar's channel. Pages 48, 49, 1 and
 * 87 are decoded; others (common 80-82, a display's page 2/70) return 0.
 * Returns samples posted. */
uint32_t radiant_radar_adapter_decode(struct radiant_radar_adapter *a, uint32_t source,
				      const uint8_t body8[8], uint64_t t_us);

/* Call from a timer (any period <= 500 ms). Posts AVAILABLE=0 once when
 * 2 s pass with no target page. Returns samples posted. */
uint32_t radiant_radar_adapter_tick(struct radiant_radar_adapter *a, uint32_t source,
				    uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RADAR_ADAPTER_H_ */
