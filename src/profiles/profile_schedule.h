/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_schedule.h - the descriptor schedule block: where, when and how well
 * a device type 0x60 node transmits.
 *
 * Provenance: docs/radiant-telemetry.md section 6 (frame 1 byte [3]'s
 * informational-flag extension space, the schedule frame's layout, the
 * clock-accuracy nibble and the downlink window's relative-phase rule) and
 * section 12 (the clock-accuracy ladder this file REUSES rather than
 * restates). This project's own written specification. No ANT+
 * device profile document was read for this file, no sdk-ant source was
 * consulted, and nothing here derives from libant.a. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this is
 * ---------------------------------------------------------------------------
 * A node already transmits a retained descriptor, so extending it costs
 * nothing extra. Three things go in it, only one a feature:
 *
 *   1. CLOCK ACCURACY, a bug fix. radiant_channel_guard_us() sizes a receive
 *      window from ANT's +/-50 ppm bound; a coin-cell RC oscillator is at
 *      250-500 ppm, which at a 2 s heartbeat is over a millisecond of
 *      per-period disagreement against a 400 us ceiling - every slot lands
 *      outside every window and the channel is lost with no error code
 *      anywhere. Three bits let the receiver size the first window correctly.
 *   2. A DOWNLINK WINDOW, announced and not yet built. Today a command
 *      reaches a node only near its own transmit slot, bounding actuator
 *      latency by the heartbeat (up to 60 s). 500 us of listening every 2 s
 *      is 0.025% duty and fixes it - this phase puts it on the wire and
 *      decodes it; the listen slot itself belongs to the response-slot phase.
 *   3. CODING RATE AND ANNOUNCED TX POWER, vocabulary the long-range PHY
 *      phase needs and must not choose twice. Defined now, one rate
 *      implemented and one reserved, so that phase isn't a format break.
 *
 * ---------------------------------------------------------------------------
 * The clock-accuracy ladder is the sync-handoff page's, deliberately
 * ---------------------------------------------------------------------------
 * enum profile_handoff_clk, unchanged, including code 0 - one ladder in this
 * project, no synonym here.
 *
 * The one distinction this block adds that the handoff page didn't need: a
 * handoff is only ever sent by a receiver that already knows something, so
 * its code 0 unambiguously means "worst case, 500 ppm". A descriptor is sent
 * by every node, including ones built before this block existed and which
 * transmit these bits as zero. So the fourth bit of the nibble -
 * PROFILE_SCHED_CLK_STATED - says whether the three below it are a ladder
 * code at all:
 *
 *   stated = 0: nothing announced; receiver uses its compiled-in bound
 *               (the default, and what every pre-existing node transmits).
 *   stated = 1: the three bits are enum profile_handoff_clk, code 0 meaning
 *               the RC node's honest 500 ppm rather than silence.
 *
 * ---------------------------------------------------------------------------
 * Why the block is a frame and the clock is not
 * ---------------------------------------------------------------------------
 * The clock nibble lives in frame 1, which every node already sends, so a
 * sparse asset tag announces its RC oscillator without adding a frame. The
 * other two items need 42 bits frame 1 doesn't have, so they get a frame of
 * their own, emitted only if there's something to say. A node announcing
 * neither is byte-for-byte the node it was before this phase.
 *
 * ---------------------------------------------------------------------------
 * The phase is RELATIVE, for the same reason it is in the handoff page
 * ---------------------------------------------------------------------------
 * The downlink phase is measured from the t_sync of the carrying frame, never
 * an absolute instant, since two clocks share no time base. It's in counts of
 * 1/32768 s rather than a fraction of the interval: a fraction of a 2 s
 * interval quantises at 244 us, half the 500 us dwell it's supposed to land
 * inside.
 *
 * Because the phase is relative to the carrying frame, a node must recompute
 * it for each transmission of that frame - but profile_sched.c encodes the
 * descriptor set once at init and retransmits the bytes, so it couldn't
 * announce a live downlink window and had to leave the interval at zero.
 *
 * The response-slot phase closes that with two functions at the end of this
 * header and one hook in profile_sched.h. The descriptor set is still encoded
 * once; what changes is that the six bytes of the schedule frame are
 * RE-PHASED in place on their way out - profile_sched_phase_for() computes
 * the count, profile_sched_rephase() writes that one field and touches no
 * other - so a live window costs a 16-bit store per set, not a re-encode.
 * Every other field, including the interval and dwell the phase must agree
 * with, is still the one the encoder validated at init.
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
 * The downlink interval is in units of 1/32 s: 12 bits reaches 128 s, past
 * the 60 s heartbeat that motivates the field. Not the 1/32768 s counts the
 * period uses, because 16 bits of those stop at 2 s, which is the example
 * duty, not the ceiling.
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
 * A ladder rather than a microsecond count since three bits was what was
 * left, and the useful range spans two orders of magnitude - 250 us is a
 * receiver's turnaround, 32 ms is a node that's decided battery isn't its
 * problem. Code 0 is cheapest, so a defaulted block announces the smallest
 * window.
 */
#define PROFILE_SCHED_DWELL_CODE_MAX 7u
uint32_t profile_sched_dwell_us(uint8_t dwell_code);

/*
 * CODING RATE - the new vocabulary, defined here so the long-range PHY phase
 * inherits it rather than inventing it.
 *
 * The registry's `LR PHY` column carries no/yes/per-node today, but "yes"
 * alone doesn't let a consumer budget a window: at eight bytes an S=8 frame
 * is ~1.23 ms against ~150 us at 1 M. So the rate is named on the wire, and a
 * later second rate is a value in this enum rather than a format break.
 *
 * S=2 IS DEFINED AND NOT IMPLEMENTED: the long-range phase builds S=8 only,
 * since FEC block 1 is always S=8-coded, so S=2 is only 2.1x cheaper rather
 * than 4x. The encoder refuses to announce a rate this build cannot
 * transmit; the decoder accepts any code, since a receiver meeting a rate it
 * doesn't implement should decline the channel, not reject the node.
 */
enum profile_sched_coding {
	PROFILE_SCHED_CODING_NONE = 0, /* uncoded; the 1 M GFSK case */
	PROFILE_SCHED_CODING_S8   = 1, /* LE Coded S=8, 125 kbit/s */
	PROFILE_SCHED_CODING_S2   = 2, /* LE Coded S=2, 500 kbit/s - reserved */
	PROFILE_SCHED_CODING_COUNT
};

/* Air rate in kbit/s for a code, or 0 for one outside the vocabulary - the
 * number a consumer budgets a window with. */
uint16_t profile_sched_coding_kbps(uint8_t coding);

/* Whether THIS BUILD can transmit or receive at that rate. False for S=2 and
 * every reserved code; a receiver that gets false must not open the channel. */
bool profile_sched_coding_implemented(uint8_t coding);

/* ---------------------------------------------------------------------------
 * The duty bound - ADR 0007
 *
 * The long-range PHY makes frames longer AND slower at once. At S=8 every
 * body byte is 64 us; a 40-byte body is ~3.2 ms of radio-on time against
 * ~150 us for an eight-byte 1 M frame. A node announcing a 4 Hz period and
 * emitting a full-length coded frame spends 1.3% of its life transmitting -
 * for a coin cell, the difference between a year and a season.
 *
 * ONE RULE, NO SECOND CONSTANT: a node's frame must stay under 25% of its
 * channel period at its announced rate. Self-enforcing, since both halves are
 * already on the wire (period in descriptor frame 0, rate in the schedule
 * block), so a receiver can check the claim as easily as the node makes it.
 *
 * It binds only where both factors are present. A 0.5 Hz asset tag with a
 * 40-byte frame is at 0.16% and never sees this; an 8-byte S=8 frame never
 * sees it either. What it refuses is a fast node with a long frame, the one
 * expensive combination a caller reaches by accident.
 *
 * Refused at the encoder, following the dwell-versus-clock-drift rule above -
 * a runtime check or warning would be a rule enforced only where somebody
 * remembered to look.
 * ---------------------------------------------------------------------------
 */

/* The 25 %, as a ratio rather than a percentage, so the arithmetic below stays
 * in integers. */
#define PROFILE_SCHED_DUTY_NUM 1u
#define PROFILE_SCHED_DUTY_DEN 4u

/*
 * Airtime of one frame with `body_len` body bytes at the announced coding
 * rate, in microseconds. 0 for a rate outside the vocabulary or a body the
 * matching format cannot carry. Delegates to radiant_frame_airtime_us()
 * rather than restating the FEC arithmetic.
 */
uint32_t profile_sched_frame_us(uint8_t coding, uint8_t body_len);

/*
 * Refuse a frame that does not fit the duty bound.
 *
 * `period_counts` is the descriptor's period, in counts of 1/32768 s. Zero
 * means asynchronous (a sparse node with no period), and the bound is
 * skipped rather than treated as an infinitely fast period - a rule about a
 * period cannot be applied to a node that hasn't got one. The sparse node's
 * cost is governed by its heartbeat instead.
 *
 * Returns 0, or -EINVAL when the frame exceeds the bound, or when the rate and
 * the body length do not describe a frame this project can put on the air.
 */
int profile_sched_duty_check(uint8_t coding, uint16_t period_counts,
			     uint8_t body_len);

/*
 * ANNOUNCED TX POWER, int8 dBm EIRP.
 *
 * Distinguishes a distant node from a desensed one, and doubles unintentionally
 * as iBeacon's "measured power": announced power minus received RSSI is path
 * loss, room-level presence done in host arithmetic with no firmware feature
 * behind it. Nothing here promises metres.
 *
 * On the wire it is BIASED so zero means silence: 0 dBm is a real transmit
 * power, so a raw int8 would make an all-zero block announce one. Biased by
 * 100, legal range 60..120, byte 0 free for the sentinel - which is what lets
 * an all-defaulted schedule frame be 48 zero bits.
 */
#define PROFILE_SCHED_TX_POWER_UNSTATED ((int8_t)-128)
#define PROFILE_SCHED_TX_POWER_MIN      ((int8_t)-40)
#define PROFILE_SCHED_TX_POWER_MAX      ((int8_t)20)
#define PROFILE_SCHED_POWER_BIAS        100

/* The block, decoded. Every field defaults to the value meaning "this node
 * announces nothing", so an all-default block costs nothing. */
struct profile_schedule {
	uint16_t dl_interval;  /* units of 1/32 s; 0 = no downlink window */
	uint16_t dl_phase;     /* counts of 1/32768 s from the carrying frame */
	uint8_t  dl_dwell;     /* code, 0..7; meaningless when interval is 0 */
	uint8_t  coding;       /* enum profile_sched_coding */
	int8_t   tx_power_dbm; /* PROFILE_SCHED_TX_POWER_UNSTATED when unstated */
};

/* The block that announces nothing: 48 zero bits on the wire. A memset() to
 * zero is NOT this value - it announces 0 dBm - hence this exists. */
#define PROFILE_SCHED_INIT_DEFAULT                                         \
	{                                                                  \
		0u, 0u, 0u, PROFILE_SCHED_CODING_NONE,                     \
			PROFILE_SCHED_TX_POWER_UNSTATED                    \
	}

/* The interval in 1/32768 s counts, or 0 when there is no downlink window. */
uint32_t profile_sched_interval_counts(const struct profile_schedule *s);

/*
 * Refuse a block that describes something a node cannot do or a receiver
 * cannot use. `lr_phy` is frame 0's long-range bit and `clk_ppm` the ceiling
 * the clock nibble announced, or 0 for nothing announced.
 *
 * Returns 0, -EINVAL for a field outside its range or two fields that
 * contradict each other, or -ENOTSUP for a coding rate this build won't
 * announce.
 *
 * Two cross-field checks are the point:
 *   - the long-range bit and the coding rate must agree.
 *   - the dwell must cover the clock error accumulated over the phase (a
 *     500 ppm node pointing 2 s ahead has drifted a millisecond by the time
 *     the window opens); checked only when the clock was stated.
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
 * This is the whole of the clock-accuracy feature. `nibble` is frame 1 byte
 * [3]'s low four bits as they arrived; the ladder lookup and the "stated"
 * rule both live here. A nibble with the stated bit clear clears the
 * channel's announcement, so a node that stops announcing behaves like one
 * that never did.
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
 * The inverse of profile_sched_listen_at(), and the reason the interval-0
 * restriction above is no longer a restriction.
 *
 * A node opens its downlink window on a grid of its own: `t_anchor` plus a
 * whole number of intervals, forever. `t_carrier` is the t_sync of the frame
 * about to carry the schedule block. This returns the phase that frame must
 * announce, in counts of 1/32768 s, so a receiver applying
 * profile_sched_listen_at() to it lands on the same absolute instant the
 * node will open - asserted across two frames sent at different times by
 * radiant_core/tests/src/test_command.c.
 *
 * The grid is unbounded in both directions - a caller never has to advance
 * the anchor itself. The result is always in 0..interval_counts-1, exactly
 * the range profile_sched_check() accepts.
 *
 * Returns the phase, or -1 when the block announces no window (the case a
 * caller must handle first, as listen_at() states it). Negative rather than
 * zero, since phase 0 is a legal announcement meaning "opens at this frame's
 * t_sync".
 */
int32_t profile_sched_phase_for(const struct profile_schedule *s,
				radiant_time_t t_anchor, radiant_time_t t_carrier);

/*
 * Rewrite the phase field of an ALREADY PACKED schedule block, in place.
 *
 * `body` is the six bytes profile_sched_pack() wrote. Touches bits 15..30 and
 * nothing else: interval, dwell, coding rate, power and reservation keep the
 * values the encoder validated, so a re-phase cannot turn a checked block
 * into an unchecked one. The interval it validates the new phase against is
 * read back out of the body, not taken from a caller - one interval, the one
 * on the wire.
 *
 * Returns 0, -EINVAL for a null body or a phase at or past the interval, or
 * -ENOENT when the packed block announces no window at all - not a silent
 * success, since re-phasing a windowless block is a bug one layer up.
 *
 * One obligation this function cannot discharge: profile_sched_check()'s
 * dwell-versus-drift rule bounds the PHASE (a 500 ppm node pointing further
 * ahead has drifted further by the time its window opens), so a block
 * validated at one phase isn't thereby validated at every phase. A caller
 * that intends to re-phase must validate its block once at the worst case,
 * phase = interval_counts - 1; profile_cmd_window_set() does exactly that.
 */
int profile_sched_rephase(uint8_t *body, uint16_t phase);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_SCHEDULE_H_ */
