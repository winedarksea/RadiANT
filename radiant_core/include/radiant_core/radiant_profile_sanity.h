/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_profile_sanity.h - reject a repaired frame whose payload is not a
 * physically possible reading, for the handful of device types where "not
 * physically possible" can be decided from one frame with no accumulator
 * history.
 *
 * Provenance: clean-room. The two fields read here - bicycle power meter
 * instantaneous power at payload[6..7] LE, heart rate at payload[7] - are
 * published ANT+ device profile page layouts, reused from tools/ant_pages.py
 * rather than re-derived. See docs/decisions/0002-clean-room-policy.md.
 *
 * radiant_crc_repair.h documents two structural refutations for a
 * mis-repair (an unseen control byte, a wrong transmission type). This is
 * the third and different kind: profile knowledge (a power meter can't
 * report 4000 W, a heart rate monitor can't report 300 bpm), which is
 * exactly what doesn't belong at the frame-codec layer - hence its own
 * file, its own Kconfig symbol, and applied by the caller to REPAIRED
 * FRAMES ONLY (radiant_api.c, inside `if (crc_failed)`). Not negotiable:
 * real athletes do hit 2000 W or 200 bpm, so the same check against a
 * clean frame would silently delete the most interesting moments of a ride.
 *
 * Thresholds: 1000 W / 200 bpm were the original suggestion but both are
 * inside real (if extreme) athlete output, so either would misclassify
 * real data even under the repaired-only guard. 3000 W reuses
 * tools/ant_verify.py's existing "not a bicycle" line; 240 bpm sits a
 * comparable margin above the highest documented human values (low 220s).
 *
 * Only these two fields/device types, because everything else this project
 * decodes (torque, speed, cadence) is an ACCUMULATOR needing the previous
 * frame's reading to mean anything - which this module never gets and
 * radiant_core has no business retaining just to police a repair. Power
 * and heart rate are the only instantaneous single-frame values available.
 */

#ifndef RADIANT_PROFILE_SANITY_H_
#define RADIANT_PROFILE_SANITY_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ANT+ device type assignments. Public numbers, already in use for the power
 * one in tools/ant_pages.py (BPWR_DEVICE_TYPE); reused rather than redefined
 * under a different name. */
#define RADIANT_PROFILE_SANITY_DEVTYPE_BPWR 0x0Bu /* bicycle power */
#define RADIANT_PROFILE_SANITY_DEVTYPE_HR   0x78u /* heart rate monitor */

/* Standard Power Only page. Matches tools/ant_pages.py's PAGE_POWER_STANDARD. */
#define RADIANT_PROFILE_SANITY_PAGE_POWER_STD 0x10u

/* Above this, an instantaneous power reading is not a cyclist - a
 * mis-repair. See the header comment for the figure's origin. */
#define RADIANT_PROFILE_SANITY_BPWR_MAX_WATTS 3000u

/* Above this, a computed heart rate is not a human at effort - a
 * mis-repair. See the header comment for the figure's origin. */
#define RADIANT_PROFILE_SANITY_HR_MAX_BPM 240u

/*
 * True if this payload is a value this module judges and that value is out
 * of range for the claimed device type. False for everything else
 * (unopinionated device type, unchecked page, "not reported" sentinel,
 * in-range value) - false means "no objection", not "confirmed plausible",
 * since most payloads are never examined at all.
 *
 * `device_type`'s pairing bit is masked internally (RADIANT_DEVICE_TYPE_MASK)
 * so callers may pass it either way. `payload` is as radiant_frame_decode()
 * produced it (payload[0] the ANT+ page number); shorter than the checked
 * field is treated as "nothing to judge", never read past its declared length.
 */
bool radiant_profile_sanity_implausible(uint8_t device_type,
					const uint8_t *payload,
					uint8_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_SANITY_H_ */
