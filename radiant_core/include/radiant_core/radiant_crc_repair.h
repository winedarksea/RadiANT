/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_crc_repair.h - turn a frame that failed its CRC by one bit into a
 * frame that did not.
 *
 * Provenance: clean-room. The arithmetic is the standard linearity property of
 * a CRC over GF(2), which is textbook and older than this project; the
 * polynomial and the frame geometry come from radiant_frame.h, which was
 * recovered on this bench. Nothing here derives from sdk-ant, from libant.a, or
 * from any adopter-gated ANT+ device profile document.
 *
 * ---------------------------------------------------------------------------
 * Why this is worth doing
 * ---------------------------------------------------------------------------
 * At the sensitivity knee essentially every CRC failure is one flipped bit.
 * A tracked ANT frame has 15 CRC-covered bytes plus a 2-byte CRC, so around
 * 136 bits are exposed; at a 1 % packet error rate the implied bit error rate
 * is about 1e-4, which puts two-bit failures two orders of magnitude below
 * one-bit ones. Recovering the single-bit cases therefore recovers nearly all
 * of the failures, and it costs a small table and no change on the air at all.
 * Roughly 1-3 dB of effective sensitivity, for arithmetic.
 *
 * ---------------------------------------------------------------------------
 * Why it runs on tracked windows only
 * ---------------------------------------------------------------------------
 * THIS IS THE DECISION THAT MAKES THE FALSE-ACCEPT RATE ACCEPTABLE, AND IT IS
 * NOT NEGOTIABLE FROM THE CALL SITE.
 *
 * A frame with more bit errors than one can have its syndrome land on a valid
 * single-bit entry by chance, in which case this module "repairs" it into a
 * frame that is still wrong and now claims to be right.
 *
 * TWO-BIT ERRORS ARE EXCLUDED OUTRIGHT, and by the polynomial rather than by
 * luck. x^16 + x^12 + x^5 + 1 evaluates to 0 at x = 1 and is therefore
 * divisible by (x + 1); for e = q*p + r that gives e(1) = r(1), and e(1) is the
 * parity of the error's weight. So an odd-weight error's syndrome always has
 * odd popcount, an even-weight error's always has even popcount, and no
 * two-bit error can ever collide with a one-bit entry. Since two-bit errors are
 * the commonest multi-bit case at the knee, that removes most of the exposure
 * before the argument below even starts.
 *
 * What remains is odd-weight errors of three bits and up, and the count that
 * matters there is the number of entries a given frame can actually reach - not
 * the size of the table.
 *
 * The table is built for RADIANT_FRAME_BODY_MAX so that one table serves every
 * body length, but a repair whose position lies further from the end than the
 * body is long would name a bit inside the ADDRESS, and those are refused: a
 * flipped address bit does not match the filter and produces no event, so such
 * a syndrome is a coincidence rather than an explanation. For a standard
 * tracked frame - 10 body bytes - that leaves 80 body bits plus 16 CRC bits,
 * so 96 reachable entries against 32768 odd-popcount syndromes: about 1 in 341,
 * on a population that is itself another order of magnitude rarer than the
 * two-bit one.
 *
 * On a TRACKED window that residual is acceptable, because the population is
 * already known to be a frame from the right sensor: the hardware matched a
 * full 5-byte address, so what is being repaired is a real frame from a known
 * master that arrived damaged. A mis-repair there produces one bad payload,
 * which every ANT+ profile survives because its values are cumulative.
 *
 * On a SEARCH window it would be applied instead to the 19-27 noise-triggered
 * address matches per 15 seconds that radiant_search.c counts and discards -
 * matches on 3 bytes, from nothing at all - and one in 341 of those would be
 * promoted into a device number manufactured out of thermal noise. A host would
 * then be told a sensor exists that does not.
 *
 * So the configuration is an argument to every call, and RADIANT_FRAME_CFG_SEARCH
 * is refused rather than served.
 *
 * ---------------------------------------------------------------------------
 * Refuting a bad repair with evidence outside the CRC
 * ---------------------------------------------------------------------------
 * The residual false accepts are not undetectable, they are merely undetectable
 * FROM HERE. A mis-repair flips a uniformly random one of the reachable
 * positions, and a receiver usually knows something about what the result
 * should look like. radiant_api.c applies the two such checks that exist at the
 * link layer, to repaired frames only:
 *
 *   - the CONTROL BYTE, of whose 256 values eleven have ever been transmitted.
 *     radiant_frame_decode() already enforces that for other reasons, so a
 *     mis-repair into those 8 bits is refuted about 96 % of the time for free;
 *   - the TRANSMISSION TYPE, which is body byte 0 and which the channel knows
 *     whenever it is not a wildcard. The address match cannot prove it, because
 *     trans_type sits in the body rather than in the matched address.
 *
 * Between them that covers 16 of the 80 body bits, and api_stats.crc_repair_refuted
 * counts what they catch.
 *
 * THE OTHER 64 BITS ARE PAYLOAD, and this layer has no business knowing what a
 * payload means: the frame codec knows frames, the profile layer knows watts
 * and beats per minute, and the clean-room boundary sits between them.
 *
 * radiant_profile_sanity.c IS that profile-layer check, in its own file behind
 * its own Kconfig symbol (CONFIG_RADIANT_CORE_PROFILE_SANITY) rather than in
 * this one: a power reading is an integer in a bounded range, a heart rate is
 * an integer in a much tighter one, and an implausible value in a REPAIRED
 * frame is far more likely a mis-repair than a real reading. api_tracked_frame()
 * applies it as the third refutation, counted in api_stats.crc_repair_implausible.
 *
 * THE CONDITION IS THAT SUCH A CHECK APPLIES TO REPAIRED FRAMES AND NOTHING
 * ELSE, and that is not a technicality. A track sprinter really does put out
 * 2000 W and a young athlete's heart rate really does pass 200 bpm; the same
 * range check applied to clean data would silently delete the most interesting
 * moments of a ride and would look like a receiver fault. tools/ant_verify.py
 * already draws that line at 3000 W ("not a bicycle"), which is the figure
 * radiant_profile_sanity.h reuses rather than a tighter guess.
 *
 * That separation is the whole reason api_stats.crc_repaired is counted apart
 * from a clean receive: the label is what makes the profile-level check safe.
 * Everything else ANT+ reports here - torque pages, speed, cadence - is an
 * accumulator that needs the previous frame's reading to mean anything, which
 * is why only power and heart rate get this treatment: they are the two
 * instantaneous, single-frame values the profile layer has.
 */

#ifndef RADIANT_CRC_REPAIR_H_
#define RADIANT_CRC_REPAIR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant_core/radiant_frame.h>

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

/*
 * The flipped bit was in the CRC field itself, so the body was already correct
 * and has not been touched. Counted as a repair rather than as a pass, because
 * it is the same 1-in-585 gamble and the caller must be able to tell the two
 * apart.
 */
#define RADIANT_CRC_REPAIR_IN_CRC    1

/* No single flipped bit explains the difference. Two or more bits, or a burst.
 * The frame is unrecoverable and must be discarded, exactly as it would have
 * been without this module. */
#define RADIANT_CRC_REPAIR_ENONE   (-1)

/* A null pointer, an over-long body, or RADIANT_FRAME_CFG_SEARCH. */
#define RADIANT_CRC_REPAIR_EINVAL  (-2)

/*
 * The syndrome table has not been built, or was built and found to contain a
 * collision. Never a transient condition: it means this build cannot repair
 * anything and the caller should stop asking.
 */
#define RADIANT_CRC_REPAIR_ESTATE  (-3)

/*
 * The received CRC and the computed one agree, so there is nothing to repair.
 * A CRC_FAIL event that produces this is a backend disagreeing with
 * radiant_crc16() about the same bytes, which is a configuration fault worth
 * separating from an unrepairable frame.
 */
#define RADIANT_CRC_REPAIR_EAGREE  (-4)

/* ---------------------------------------------------------------------------
 * The table
 * ---------------------------------------------------------------------------
 */

/*
 * Entries: every bit of the largest body, plus every bit of the CRC.
 *
 * The address is deliberately absent, and its absence is a fact about the
 * radio rather than an economy. A matched address never reaches RAM - that is
 * what a hardware address match means - and a frame with a flipped address bit
 * does not match in the first place, so it produces no event to repair. There
 * is nothing there to correct and no way to correct it.
 */
#define RADIANT_CRC_REPAIR_ENTRIES \
	(((size_t)RADIANT_FRAME_BODY_MAX * 8u) + (RADIANT_FRAME_CRC_BYTES * 8u))

/*
 * Build the syndrome table and check it. Idempotent; safe to call again.
 *
 * Returns true if the table is usable. False means two different single-bit
 * errors produce the same syndrome, in which case a "repair" would be a coin
 * toss between two positions and this module refuses to do any. That cannot
 * happen for CRC-16/CCITT over a frame this short - the polynomial detects
 * every single-bit error in blocks up to 32767 bits - so a false here means the
 * polynomial or the geometry changed, and it is checked rather than asserted in
 * a comment for exactly that reason.
 */
bool radiant_crc_repair_init(void);

/* Whether radiant_crc_repair() can do anything at all in this build. */
bool radiant_crc_repair_ready(void);

/*
 * Attempt one single-bit correction.
 *
 * `addr` and `body` are the frame as struct radiant_frame_wire holds it: the
 * address the window filtered on (regenerated, since the hardware never handed
 * it over) and the body the radio DMA'd. `crc_rx` is the CRC as received, from
 * radiant_rx_event.crc_rx - not one recomputed here, which would make the
 * syndrome identically zero and the whole exercise circular.
 *
 * On RADIANT_CRC_REPAIR_BODY the body has been modified in place. On every
 * other return it is untouched.
 *
 * `cfg` must be RADIANT_FRAME_CFG_TRACKING. See the header comment for why
 * search is refused here rather than at the call site.
 */
int radiant_crc_repair(enum radiant_frame_cfg cfg, const uint8_t *addr,
		       uint8_t addr_len, uint8_t *body, uint8_t body_len,
		       uint32_t crc_rx);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_CRC_REPAIR_H_ */
