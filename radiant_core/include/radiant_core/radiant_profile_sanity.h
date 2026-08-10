/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_profile_sanity.h - reject a repaired frame whose payload is not a
 * physically possible reading, for the handful of device types where "not
 * physically possible" can be decided from one frame with no accumulator
 * history.
 *
 * Provenance: clean-room. The two fields read here - a bicycle power meter's
 * instantaneous power at payload[6..7] LE, a heart rate monitor's computed
 * heart rate at payload[7] - are published ANT+ device profile page layouts,
 * the same two facts tools/ant_pages.py already encodes for the power case
 * and reuses rather than re-derives. Nothing here derives from sdk-ant, from
 * libant.a, or from any adopter-gated ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * Why this exists, and why it is not part of radiant_crc_repair.c
 * ---------------------------------------------------------------------------
 * radiant_crc_repair.h documents two refutations that catch a mis-repair
 * before it reaches a host: a control byte that has never been on the air,
 * and a transmission type that is not this channel's. Both are structural -
 * they know nothing about what a payload MEANS. This module is the third
 * refutation, and it is a different kind: it knows that a bicycle power meter
 * cannot report 4000 W and a heart rate monitor cannot report 300 bpm,
 * because those numbers describe a human body rather than a radio.
 *
 * That is profile knowledge, which is exactly what radiant_crc_repair.h says
 * does not belong at the frame-codec layer - so it lives here, in its own
 * file, behind its own Kconfig symbol, and is applied by the caller to
 * REPAIRED FRAMES ONLY. See radiant_crc_repair.h's "Refuting a bad repair
 * with evidence outside the CRC" section for the argument this module
 * completes.
 *
 * THE CONDITION IS NOT NEGOTIABLE. A track sprinter really does put out
 * 2000 W and an athlete's heart rate really does pass 200 bpm; the same
 * check against a CLEAN frame would silently delete the most interesting
 * moments of a ride and would look like a receiver fault rather than a
 * safety net. This module has no way to enforce that from inside itself -
 * it is only ever handed a payload, never told how the frame arrived - so
 * the caller in radiant_api.c is the one place the rule is actually kept,
 * and it is kept by construction: the call sits inside `if (crc_failed)`.
 *
 * ---------------------------------------------------------------------------
 * Why these thresholds and not the ones first proposed
 * ---------------------------------------------------------------------------
 * 1000 W and 200 bpm were the original suggestion. Both are comfortably
 * inside what a real, uncorrupted athlete produces: track sprint efforts
 * exceed 2000 W and young athletes' heart rates exceed 200 bpm, so either
 * threshold would misclassify real data even with the "repaired frames
 * only" guard in place. tools/ant_verify.py already draws the power line at
 * 3000 W ("not a bicycle") for the same reason on a different data path;
 * this module reuses that figure rather than inventing a second one. The
 * heart rate ceiling is set a comparable distance above the highest
 * documented human values (low 220s) rather than at a round number close to
 * them.
 *
 * ---------------------------------------------------------------------------
 * Why only these two fields, and only these two device types
 * ---------------------------------------------------------------------------
 * Every other value this project decodes (tools/ant_pages.py's torque pages,
 * speed, cadence) is an ACCUMULATOR: recovering a rate from it needs the
 * previous frame's reading, which this module is never handed and which
 * radiant_core has no business retaining just to police a repair. The two
 * fields checked here are instantaneous, single-frame values by
 * construction, which is what makes a per-frame plausibility check possible
 * at all instead of merely a good idea.
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

/*
 * Above this, an instantaneous power reading is not a cyclist - it is a
 * mis-repair. See the header comment for why this figure and not a tighter
 * one.
 */
#define RADIANT_PROFILE_SANITY_BPWR_MAX_WATTS 3000u

/*
 * Above this, a computed heart rate is not a human being at effort - it is a
 * mis-repair. See the header comment for why this figure and not a tighter
 * one.
 */
#define RADIANT_PROFILE_SANITY_HR_MAX_BPM 240u

/*
 * True if this payload is a value this module can and does judge, and that
 * value is out of range for the device type it claims to be. False for
 * every other outcome: a device type this module has no opinion on, a page
 * this module does not check, a "not reported" sentinel (0xFFFF power,
 * 0xFF heart rate), or a value inside range. False therefore means "no
 * objection", not "confirmed plausible" - most payloads pass through this
 * module having never been examined at all.
 *
 * `device_type` is taken with the pairing bit already irrelevant to the
 * check; callers may pass it masked or unmasked; this function masks it
 * itself with RADIANT_DEVICE_TYPE_MASK's value so a caller cannot forget to.
 *
 * `payload` is the frame's payload as radiant_frame_decode() produced it -
 * payload[0] is the ANT+ page number, matching every profile's own layout.
 * A payload shorter than the field being checked is treated as "nothing to
 * judge" rather than as an error; there is no unsafe way to read past its
 * declared length here.
 */
bool radiant_profile_sanity_implausible(uint8_t device_type,
					const uint8_t *payload,
					uint8_t payload_len);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_SANITY_H_ */
