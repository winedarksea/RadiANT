/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_schedule.h - the descriptor schedule block: where, when and how well
 * a device type 0x60 node transmits.
 *
 * Provenance: docs/radiant-telemetry.md section 6 (frame 1 byte [3]'s
 * informational-flag extension space, the schedule frame's layout, the
 * clock-accuracy nibble and the downlink window's relative-phase rule) and
 * section 12 (the clock-accuracy ladder this file REUSES rather than restates).
 * That document is this project's own written specification. No adopter-gated
 * ANT+ device profile document was read for this file, no sdk-ant source was
 * consulted, and nothing here derives from libant.a. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this is
 * ---------------------------------------------------------------------------
 * A node already transmits a retained descriptor, so the descriptor is a
 * standing pointer and costs nothing extra to extend. Three things go in it,
 * and only one of them is a feature:
 *
 *   1. CLOCK ACCURACY, which is a bug fix. radiant_channel_guard_us() sizes a
 *      receive window from ANT's +/-50 ppm bound. A coin-cell node on an RC
 *      oscillator is at 250-500 ppm, and at a 2 s heartbeat that is over a
 *      millisecond of per-period disagreement against a 400 us ceiling - so
 *      every slot lands outside every window, the estimator is never given a
 *      clean slot to lock onto, and the channel is lost with no error code
 *      anywhere. Three bits let the receiver size the first window correctly.
 *   2. A DOWNLINK WINDOW, announced and not yet built. Today a command reaches
 *      a node only near the node's own transmit slot, so actuator latency is
 *      bounded by the heartbeat - up to 60 s to turn on the air conditioning.
 *      500 us of listening every 2 s is 0.025 % duty and fixes it. THIS PHASE
 *      PUTS IT ON THE WIRE AND DECODES IT; the listen slot itself belongs to
 *      the phase that generalises response slots, and building the machinery
 *      early would put a scheduler kind on the air before anything asked for
 *      one.
 *   3. CODING RATE AND ANNOUNCED TX POWER, which are vocabulary the long-range
 *      PHY phase needs and must not get to choose twice. Defining them now,
 *      with one rate implemented and one reserved, is what stops that phase
 *      from being a format break.
 *
 * ---------------------------------------------------------------------------
 * The clock-accuracy ladder is the sync-handoff page's, deliberately
 * ---------------------------------------------------------------------------
 * enum profile_handoff_clk, unchanged, including its code 0. That page put a
 * three-bit ladder on the wire and did not consume it, precisely so that this
 * block would inherit a vocabulary instead of picking a second one meaning the
 * same thing differently. There is exactly one clock-accuracy ladder in this
 * project and it is that one; this header adds no synonym for it.
 *
 * WHAT THIS BLOCK ADDS IS THE ONE DISTINCTION THE HANDOFF PAGE DID NOT NEED.
 * A handoff is only ever sent by a receiver that already knows something, so
 * its code 0 can mean "worst case, 500 ppm" with no ambiguity. A descriptor is
 * sent by every node, including every node built before this block existed,
 * and those nodes transmit these bits as zeros. So the fourth bit of the
 * nibble - PROFILE_SCHED_CLK_STATED - says whether the three below it are a
 * ladder code at all:
 *
 *   stated = 0: nothing is announced. The receiver uses its compiled-in bound
 *               and behaves exactly as it did before this phase. This is what
 *               every pre-existing node transmits, and it is the default.
 *   stated = 1: the three bits are enum profile_handoff_clk, and code 0 is the
 *               RC node's honest 500 ppm rather than a silence.
 *
 * ---------------------------------------------------------------------------
 * Why the block is a frame and the clock is not
 * ---------------------------------------------------------------------------
 * The clock nibble lives in frame 1, which every node already sends, so a
 * sparse asset tag with no fields announces its RC oscillator without adding a
 * single frame to its descriptor set - and the tag is exactly the node this
 * field was written for. The other two items need 42 bits, which frame 1 does
 * not have, so they are a frame of their own that a node emits only if it has
 * something to say. A node that announces neither is byte-for-byte the node it
 * was before this phase.
 *
 * ---------------------------------------------------------------------------
 * The phase is RELATIVE, for the same reason it is in the handoff page
 * ---------------------------------------------------------------------------
 * The downlink phase is measured from the t_sync of the frame that carries it
 * and never from an absolute instant, because two clocks share no time base.
 * It is in counts of 1/32768 s rather than a fraction of the interval: a
 * fraction of a 2 s interval quantises at 244 us, which is half of the 500 us
 * dwell it is supposed to land inside, and a phase whose error is comparable to
 * the window it points at is not a pointer.
 *
 * ONE RESTRICTION WAS RECORDED HERE RATHER THAN SOLVED, AND IS NOW SOLVED.
 * Because the phase is relative to the carrying frame, a node must recompute it
 * for each transmission of that frame. profile_sched.c encodes the descriptor
 * set ONCE at init and retransmits the bytes, so a node driven by it could not
 * announce a live downlink window and had to leave the interval at zero.
 *
 * The response-slot phase closes that with two functions at the end of this
 * header and one hook in profile_sched.h, and the shape is worth stating
 * because it is smaller than the obvious fix. The descriptor set is still
 * encoded once. What changes is that the six bytes of the schedule frame are
 * RE-PHASED in place on their way out - profile_sched_phase_for() computes the
 * count, profile_sched_rephase() writes that one field and touches no other -
 * so the cost of a live window is a 16-bit store per descriptor set rather than
 * a re-encode of the whole set per slot. Every other field, including the
 * interval and the dwell the phase must agree with, is still the one the
 * encoder validated at init.
 */

#ifndef RADIANT_PROFILE_SCHEDULE_H_
#define RADIANT_PROFILE_SCHEDULE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* For the clock-accuracy ladder, which is not restated here. */
#include "profile_handoff.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Frame 1 byte [3] bits 3..0 - the informational-flag extension space
 * ---------------------------------------------------------------------------
 */

#define PROFILE_SCHED_CLK_STATED  0x08u /* bit 3: the three below are a code */
#define PROFILE_SCHED_CLK_MASK    0x07u /* bits 2..0: enum profile_handoff_clk */

/* ---------------------------------------------------------------------------
 * The schedule frame's 48-bit body, MSB-first per profile_bits.h
 *
 *   bits  0..2   downlink dwell code
 *   bits  3..14  downlink interval, units of 1/32 s; 0 = no downlink window
 *   bits 15..30  downlink phase, counts of 1/32768 s from the carrying frame
 *   bits 31..33  coding rate
 *   bits 34..41  announced TX power, int8 dBm EIRP
 *   bits 42..47  reserved, must be zero
 * ---------------------------------------------------------------------------
 */

#define PROFILE_SCHED_AREA_BITS 48u

#define PROFILE_SCHED_DWELL_OFF    0u
#define PROFILE_SCHED_DWELL_W      3u
#define PROFILE_SCHED_INTERVAL_OFF 3u
#define PROFILE_SCHED_INTERVAL_W   12u
#define PROFILE_SCHED_PHASE_OFF    15u
#define PROFILE_SCHED_PHASE_W      16u
#define PROFILE_SCHED_CODING_OFF   31u
#define PROFILE_SCHED_CODING_W     3u
#define PROFILE_SCHED_POWER_OFF    34u
#define PROFILE_SCHED_POWER_W      8u
#define PROFILE_SCHED_RSVD_OFF     42u
#define PROFILE_SCHED_RSVD_W       6u

/*
 * The downlink interval is in units of 1/32 s, which is 12 bits reaching 128 s
 * - past the 60 s heartbeat that motivates the whole field - at a resolution
 * far finer than any listen cadence needs. It is NOT in the 1/32768 s counts
 * the period uses, because 16 bits of those stop at 2 s and 2 s is the
 * EXAMPLE duty, not the ceiling.
 */
#define PROFILE_SCHED_INTERVAL_HZ    32u
#define PROFILE_SCHED_INTERVAL_MAX   ((1u << PROFILE_SCHED_INTERVAL_W) - 1u)
#define PROFILE_SCHED_PHASE_MAX      ((1u << PROFILE_SCHED_PHASE_W) - 1u)
/* 1/32 s expressed in the 1/32768 s counts everything else in this protocol is
 * measured in. */
#define PROFILE_SCHED_INTERVAL_COUNTS (32768u / PROFILE_SCHED_INTERVAL_HZ)

/*
 * The dwell ladder: how long the node listens once it opens the window.
 *
 * A ladder rather than a count of microseconds because three bits is what was
 * left, and because the useful range spans two orders of magnitude - 250 us is
 * a receiver's turnaround and 32 ms is a node that has decided battery is not
 * its problem. Code 0 is the cheapest, so a defaulted block announces the
 * smallest window rather than the largest.
 */
#define PROFILE_SCHED_DWELL_CODE_MAX 7u
uint32_t profile_sched_dwell_us(uint8_t dwell_code);

/*
 * CODING RATE - the new vocabulary, defined here so the long-range PHY phase
 * inherits it rather than inventing it.
 *
 * The registry's `LR PHY` column (docs/profile-registry.md) carries no/yes/
 * per-node today, and "yes" alone does not let a consumer budget a window: at
 * eight bytes an S=8 frame is ~1.23 ms against ~150 us at 1 M, which is the
 * difference between a slot that fits and one that does not. So the rate is
 * named on the wire from the first block that can carry it, and a second rate
 * arriving later is a value in this enum rather than a format break.
 *
 * S=2 IS DEFINED AND NOT IMPLEMENTED, deliberately: the long-range phase builds
 * S=8 only, because FEC block 1 is always S=8-coded and so S=2 is only 2.1x
 * cheaper rather than 4x - it gives up ~3 dB to save a quarter of a percent of
 * duty at 4 Hz. The encoder here refuses to ANNOUNCE a rate this build cannot
 * transmit; the decoder accepts any code, because a receiver's job on meeting a
 * rate it does not implement is to decline the channel, not to reject the node.
 */
enum profile_sched_coding {
	PROFILE_SCHED_CODING_NONE = 0, /* uncoded; the 1 M GFSK case */
	PROFILE_SCHED_CODING_S8   = 1, /* LE Coded S=8, 125 kbit/s */
	PROFILE_SCHED_CODING_S2   = 2, /* LE Coded S=2, 500 kbit/s - reserved */
	PROFILE_SCHED_CODING_COUNT
};

/* Air rate in kbit/s for a code, or 0 for one outside the vocabulary. The
 * number a consumer budgets a window with, which is the reason the field
 * exists at all. */
uint16_t profile_sched_coding_kbps(uint8_t coding);

/* Whether THIS BUILD can transmit or receive at that rate. False for S=2 and
 * for every reserved code, and a receiver that gets false must not open the
 * channel. */
bool profile_sched_coding_implemented(uint8_t coding);

/* ---------------------------------------------------------------------------
 * The duty bound - ADR 0007
 *
 * The long-range PHY makes frames longer AND slower at the same time, and the
 * two multiply. At S=8 every body byte is 64 us; a 40-byte body is about
 * 3.2 ms of radio-on time against ~150 us for an eight-byte 1 M frame. A node
 * that announces a 4 Hz period and then emits a full-length coded frame is
 * spending 1.3 % of its life transmitting, which for a coin cell is the
 * difference between a year and a season.
 *
 * ONE RULE, NO SECOND CONSTANT: a node's frame must stay under 25 % of its
 * channel period at its announced rate. It is self-enforcing, because both
 * halves are already on the wire - the period is in descriptor frame 0 and the
 * rate is in the schedule block - so a receiver can check the claim as easily
 * as the node can make it, and a node cannot announce a schedule it does not
 * keep.
 *
 * IT BINDS ONLY WHERE BOTH FACTORS ARE PRESENT, which is the point. A 0.5 Hz
 * asset tag with a 40-byte frame is at 0.16 % and never sees this. An 8-byte
 * frame at S=8 is 1.3 ms against any real period and never sees it either. What
 * it refuses is the combination - a fast node with a long frame - which is the
 * one shape that is expensive and the one a caller reaches by accident rather
 * than by decision.
 *
 * REFUSED AT THE ENCODER, following the dwell-versus-clock-drift rule above.
 * The alternative - a warning, or a runtime check - is a rule that is enforced
 * only where somebody remembered to look, on a cost that shows up months later
 * as a battery complaint with no reproducer.
 * ---------------------------------------------------------------------------
 */

/* The 25 %, as a ratio rather than a percentage, so the arithmetic below stays
 * in integers. */
#define PROFILE_SCHED_DUTY_NUM 1u
#define PROFILE_SCHED_DUTY_DEN 4u

/*
 * Airtime of one frame with `body_len` body bytes at the announced coding rate,
 * in microseconds. 0 for a rate outside the vocabulary or a body the matching
 * format cannot carry.
 *
 * It delegates to radiant_frame_airtime_us() rather than restating the FEC
 * arithmetic: one implementation of "how long is this frame" is one more than
 * the number that can be right, and the profile layer's coding code and the
 * frame layer's configuration are two names for the same choice.
 */
uint32_t profile_sched_frame_us(uint8_t coding, uint8_t body_len);

/*
 * Refuse a frame that does not fit the duty bound.
 *
 * `period_counts` is the descriptor's period, in counts of 1/32768 s. ZERO
 * MEANS ASYNCHRONOUS - a sparse node with no period at all - and the bound is
 * skipped rather than treated as an infinitely fast period, on the same terms
 * profile_sched_check() skips the drift bound when no clock was announced: a
 * rule about a period cannot be applied to a node that has not got one. The
 * sparse node's cost is governed by its heartbeat, which is a different
 * announcement.
 *
 * Returns 0, or -EINVAL when the frame exceeds the bound, or when the rate and
 * the body length do not describe a frame this project can put on the air.
 */
int profile_sched_duty_check(uint8_t coding, uint16_t period_counts,
			     uint8_t body_len);

/*
 * ANNOUNCED TX POWER, int8 dBm EIRP.
 *
 * It distinguishes a distant node from a desensed one, and it is the target a
 * future power-control loop would servo against. It is also, unintentionally,
 * iBeacon's "measured power": announced power minus received RSSI is path
 * loss, which is room-level presence done entirely in host arithmetic with no
 * firmware feature behind it. Nothing here promises metres.
 *
 * ON THE WIRE IT IS BIASED, AND THE BIAS EXISTS TO MAKE ZERO MEAN SILENCE.
 * 0 dBm is a real and common transmit power, so it cannot double as "not
 * stated"; a raw int8 would therefore make an all-zero block announce a
 * power. Biased by 100, the legal range is 60..120 and the byte 0 is free for
 * the sentinel - which is what lets an all-defaulted schedule frame be 48 zero
 * bits, and lets the idle-cost A/B be a statement about bytes rather than about
 * intentions.
 */
#define PROFILE_SCHED_TX_POWER_UNSTATED ((int8_t)-128)
#define PROFILE_SCHED_TX_POWER_MIN      ((int8_t)-40)
#define PROFILE_SCHED_TX_POWER_MAX      ((int8_t)20)
#define PROFILE_SCHED_POWER_BIAS        100

/*
 * The block, decoded. Every field is defaulted to the value that means "this
 * node announces nothing", which is what makes an all-default block a block
 * that costs nothing.
 */
struct profile_schedule {
	uint16_t dl_interval;  /* units of 1/32 s; 0 = no downlink window */
	uint16_t dl_phase;     /* counts of 1/32768 s from the carrying frame */
	uint8_t  dl_dwell;     /* code, 0..7; meaningless when interval is 0 */
	uint8_t  coding;       /* enum profile_sched_coding */
	int8_t   tx_power_dbm; /* PROFILE_SCHED_TX_POWER_UNSTATED when unstated */
};

/*
 * The block that announces nothing, which is the block the idle-cost A/B is
 * run with: 48 zero bits on the wire. Note that a memset() to zero is NOT this
 * value - it announces 0 dBm - and that is why this exists.
 */
#define PROFILE_SCHED_INIT_DEFAULT                                         \
	{                                                                  \
		0u, 0u, 0u, PROFILE_SCHED_CODING_NONE,                     \
			PROFILE_SCHED_TX_POWER_UNSTATED                    \
	}

/* The interval in 1/32768 s counts, or 0 when there is no downlink window. */
uint32_t profile_sched_interval_counts(const struct profile_schedule *s);

/*
 * Refuse a block that describes something a node cannot do or a receiver cannot
 * use. `lr_phy` is frame 0's long-range bit and `clk_ppm` the ceiling the
 * clock nibble announced, or 0 when it announced nothing.
 *
 * Returns 0, -EINVAL for a field outside its range or two fields that contradict
 * each other, or -ENOTSUP for a coding rate this build will not announce.
 *
 * THE TWO CROSS-FIELD CHECKS ARE THE POINT:
 *
 *   - the long-range bit and the coding rate must agree. An LR channel with no
 *     coding rate does not tell a consumer how long a frame takes, and a coding
 *     rate on a 1 M channel is a node describing a PHY it is not using.
 *   - the dwell must cover the clock error accumulated over the phase. A 500
 *     ppm node pointing 2 s ahead has drifted a millisecond by the time the
 *     window opens, so a 500 us dwell is a window the commanding receiver
 *     cannot hit - the two announcements are not independent, and a node that
 *     gets this wrong produces a downlink that silently never works. Checked
 *     only when the clock was stated; an unstated clock leaves the receiver to
 *     assume the worst, which it should.
 */
int profile_sched_check(const struct profile_schedule *s, bool lr_phy,
			uint16_t clk_ppm);

/* Pack into the 6 body bytes of the schedule frame. Returns 0 or a negative
 * errno from profile_sched_check(). */
int profile_sched_pack(const struct profile_schedule *s, bool lr_phy,
		       uint16_t clk_ppm, uint8_t *body);

/* The inverse. -EPROTO when a reserved bit is not zero, -EINVAL for a field
 * that cannot be used as decoded. */
int profile_sched_unpack(const uint8_t *body, bool lr_phy, uint16_t clk_ppm,
			 struct profile_schedule *out);

/* ---------------------------------------------------------------------------
 * The consumers
 * ---------------------------------------------------------------------------
 */

/*
 * Push an announced clock accuracy into a channel's window estimator.
 *
 * THIS IS THE WHOLE OF THE CLOCK-ACCURACY FEATURE. `nibble` is frame 1 byte
 * [3]'s low four bits exactly as they arrived; the ladder lookup and the
 * "stated" rule both live here so that no caller gets to write either one
 * twice. A nibble with the stated bit clear clears the channel's announcement,
 * which is what makes a node that stops announcing behave like a node that
 * never did.
 *
 * Returns the ppm now in force on that channel, or 0 for "nothing announced".
 */
uint16_t profile_sched_apply_clock(uint8_t channel, uint8_t nibble);

/* The ppm a nibble announces, or 0 when it announces nothing. The pure half of
 * the above, for a caller that wants the number without a channel. */
uint16_t profile_sched_clk_ppm(uint8_t nibble);

/* Build the nibble. `code` is an enum profile_handoff_clk; anything outside the
 * ladder returns 0, which is "nothing stated" rather than a wrong claim. */
uint8_t profile_sched_clk_nibble(uint8_t code, bool stated);

/*
 * The k-th listen window after the frame that announced it, on the RECIPIENT's
 * clock. `t_carrier` is the t_sync at which this receiver heard the schedule
 * frame; k=0 is the first window.
 *
 * RADIANT_TIME_NEVER when there is no downlink window, which is the case a
 * caller must handle before it handles any other.
 */
radiant_time_t profile_sched_listen_at(const struct profile_schedule *s,
				       radiant_time_t t_carrier, uint32_t k);

/* ---------------------------------------------------------------------------
 * The node side of the same arithmetic - the response-slot phase
 * ---------------------------------------------------------------------------
 */

/*
 * THE INVERSE OF profile_sched_listen_at(), and the reason the interval-0
 * restriction above is no longer a restriction.
 *
 * A node opens its downlink window on a grid of its own: `t_anchor` plus a
 * whole number of intervals, forever. `t_carrier` is the t_sync of the frame
 * about to carry the schedule block. This returns the phase that frame must
 * announce, in counts of 1/32768 s, so that a receiver applying
 * profile_sched_listen_at() to it lands on the SAME absolute instant the node
 * will open - which is the property the two functions exist to have, and the
 * one radiant_core/tests/src/test_command.c asserts across two frames sent at
 * different times.
 *
 * The grid is unbounded in both directions: an anchor in the past is reduced
 * forward and an anchor more than an interval ahead is reduced back, so a
 * caller never has to advance the anchor itself. The result is always in
 * 0 .. interval_counts-1, which is exactly the range profile_sched_check()
 * accepts.
 *
 * Returns the phase, or -1 when the block announces no window - the case a
 * caller must handle before any other, stated the same way listen_at() states
 * it. Negative rather than a zero phase, because phase 0 is a legal
 * announcement meaning "the window opens at this frame's t_sync".
 */
int32_t profile_sched_phase_for(const struct profile_schedule *s,
				radiant_time_t t_anchor, radiant_time_t t_carrier);

/*
 * Rewrite the phase field of an ALREADY PACKED schedule block, in place.
 *
 * `body` is the six bytes profile_sched_pack() wrote. This touches bits 15..30
 * and nothing else: the interval, the dwell, the coding rate, the announced
 * power and the reservation all keep the values the encoder validated, so a
 * re-phase cannot turn a checked block into an unchecked one. The interval it
 * validates the new phase against is read back out of the body rather than
 * taken from a caller, for the same reason - there is one interval and it is
 * the one on the wire.
 *
 * Returns 0, -EINVAL for a null body or a phase at or past the interval, or
 * -ENOENT when the packed block announces no window at all. -ENOENT rather than
 * a silent success, because a caller re-phasing a block with no window has a
 * bug one layer up and a quiet no-op is how it survives to the bench.
 *
 * ONE OBLIGATION THIS FUNCTION CANNOT DISCHARGE, so it is stated rather than
 * checked here. profile_sched_check()'s dwell-versus-drift rule is a bound on
 * the PHASE - a 500 ppm node pointing further ahead has drifted further by the
 * time its window opens - so a block validated at one phase is not thereby
 * validated at every phase, and this function has neither the clock accuracy
 * nor a reason to be told it. A caller that intends to re-phase must therefore
 * validate its block ONCE AT THE WORST CASE, phase = interval_counts - 1, and
 * then every phase this function can write is covered. profile_cmd_window_set()
 * is the caller in this tree and does exactly that; the alternative - checking
 * per re-phase - would put a multiply in a path whose whole point is that it is
 * a single store.
 */
int profile_sched_rephase(uint8_t *body, uint16_t phase);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_SCHEDULE_H_ */
