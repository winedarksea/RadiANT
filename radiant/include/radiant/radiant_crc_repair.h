/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_crc_repair.h - turn a frame that failed its CRC by one bit into a
 * frame that did not.
 *
 * Provenance: clean-room. Arithmetic is the standard CRC-over-GF(2) linearity
 * property; polynomial and frame geometry come from radiant_frame.h.
 *
 * At the sensitivity knee, essentially every CRC failure is one flipped bit
 * (~1e-4 bit error rate at 1% PER over 136 covered bits puts two-bit failures
 * two orders of magnitude below one-bit ones), so recovering single-bit cases
 * recovers nearly all failures for the cost of a small table - roughly 1-3 dB
 * of effective sensitivity.
 *
 * TRACKED WINDOWS ONLY, not negotiable from the call site. A frame with more
 * than one bit error can land on a valid single-bit syndrome by chance and
 * get "repaired" into something still wrong. Two-bit errors are excluded
 * outright by the polynomial (x^16+x^12+x^5+1 is divisible by x+1, so
 * syndrome parity matches error weight parity - no two-bit error can collide
 * with a one-bit entry). What remains is 3+-bit odd-weight errors: for a
 * standard 10-byte body, 96 reachable entries (80 body bits + 16 CRC bits;
 * address-bit positions are refused since a flipped address bit wouldn't
 * match the filter) against 32768 odd-popcount syndromes, ~1 in 341.
 *
 * On a TRACKED window that's acceptable - the hardware already matched a
 * full 5-byte address, so a mis-repair produces one bad payload in an
 * otherwise-cumulative ANT+ stream. On a SEARCH window the same odds would
 * promote noise-triggered 3-byte address matches (19-27 per 15s,
 * radiant_search.c) into fabricated sensors, so RADIANT_FRAME_CFG_SEARCH is
 * refused rather than served.
 *
 * Residual false accepts are undetectable here but not elsewhere: radiant_api.c
 * refutes repaired frames (only) against two structural checks - the control
 * byte (11 of 256 values ever transmitted, already enforced by
 * radiant_frame_decode()) and the transmission type (known from the channel
 * when not a wildcard) - covering 16 of the 80 body bits
 * (api_stats.crc_repair_refuted). The other 64 bits are payload, outside this
 * layer's business; radiant_profile_sanity.c is the third refutation, a
 * separate profile-layer plausibility check (bounded power/heart-rate ranges)
 * applied to repaired frames only - never to clean data, since real athletes
 * do exceed "typical" ranges - counted in api_stats.crc_repair_implausible.
 * That's also why crc_repaired is counted apart from a clean receive: the
 * label is what makes the profile-level check safe to apply at all.
 */

#ifndef RADIANT_CRC_REPAIR_H_
#define RADIANT_CRC_REPAIR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant/radiant_frame.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Return codes
 * ---------------------------------------------------------------------------
 */

/* A bit in the body was flipped back. The body has been modified in place and
 * now produces the CRC that was received. */
#define RADIANT_CRC_REPAIR_BODY      0

/* The flipped bit was in the CRC field itself, so the body is untouched.
 * Counted as a repair, not a pass - same 1-in-585 gamble, and the caller
 * must be able to tell the two apart. */
#define RADIANT_CRC_REPAIR_IN_CRC    1

/* No single flipped bit explains the difference. Two or more bits, or a burst.
 * The frame is unrecoverable and must be discarded, exactly as it would have
 * been without this module. */
#define RADIANT_CRC_REPAIR_ENONE   (-1)

/* A null pointer, an over-long body, or RADIANT_FRAME_CFG_SEARCH. */
#define RADIANT_CRC_REPAIR_EINVAL  (-2)

/* The syndrome table was never built, or was built and found to collide.
 * Never transient - this build can't repair anything, ever. */
#define RADIANT_CRC_REPAIR_ESTATE  (-3)

/* The received CRC and the computed one agree - nothing to repair. A
 * CRC_FAIL event producing this means the backend disagrees with
 * radiant_crc16(), a configuration fault distinct from an unrepairable frame. */
#define RADIANT_CRC_REPAIR_EAGREE  (-4)

/* ---------------------------------------------------------------------------
 * The table
 * ---------------------------------------------------------------------------
 */

/*
 * Entries: every bit of the largest body, plus every bit of the CRC. The
 * address is deliberately absent - a matched address never reaches RAM, and
 * a frame with a flipped address bit doesn't match in the first place, so
 * there's no event to repair and nothing to correct.
 */
#define RADIANT_CRC_REPAIR_ENTRIES \
	(((size_t)RADIANT_FRAME_BODY_MAX * 8u) + (RADIANT_FRAME_CRC_BYTES * 8u))

/*
 * Build the syndrome table and check it. Idempotent.
 *
 * Returns true if usable. False means two single-bit errors produced the
 * same syndrome (a "repair" would be a coin toss, so this module refuses to
 * do any) - shouldn't happen for CRC-16/CCITT over a frame this short, so a
 * false here means the polynomial or geometry changed. Checked, not assumed.
 */
bool radiant_crc_repair_init(void);

/* Whether radiant_crc_repair() can do anything at all in this build. */
bool radiant_crc_repair_ready(void);

/*
 * Attempt one single-bit correction.
 *
 * `addr`/`body` are the frame as struct radiant_frame_wire holds it (address
 * regenerated, since hardware never hands it over; body as DMA'd). `crc_rx`
 * is the CRC as received (radiant_rx_event.crc_rx) - not recomputed here,
 * which would make the syndrome identically zero.
 *
 * Body modified in place only on RADIANT_CRC_REPAIR_BODY.
 * `cfg` must be RADIANT_FRAME_CFG_TRACKING - see the header comment.
 */
int radiant_crc_repair(enum radiant_frame_cfg cfg, const uint8_t *addr,
		       uint8_t addr_len, uint8_t *body, uint8_t body_len,
		       uint32_t crc_rx);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_CRC_REPAIR_H_ */
