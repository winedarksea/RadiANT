/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_handoff.h - page 0x12, the sync handoff. BLE's PAST, for ANT.
 *
 * Provenance: docs/radiant-telemetry.md section 12 (the page layout, the
 * two-frame arithmetic, the relative-phase rule, the clock-accuracy ladder and
 * the no-epoch constraint) and section 4 (the two positional invariants that
 * fix bytes [0], [1] and [7]). That document is this project's own written
 * specification. No ANT+ device profile document was read for
 * this file, no sdk-ant source was consulted, and nothing here derives from
 * libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * WHAT THIS IS: the one page in the envelope document a NODE does not send.
 * A receiver already tracking a node broadcasts it so a second receiver can
 * configure a channel and go straight to tracking, or so the same receiver
 * re-acquires in one channel period after a reboot instead of a sweep. A
 * wildcard search walks address filters one dwell each and is only certain
 * after a full sweep (32 windows on the nRF backend, the dominant term in
 * discovery latency); THE SWEEP IS SKIPPED, NOT SHORTENED, when someone
 * already knows the answer.
 *
 * TWO FRAMES, AND THE ARITHMETIC THAT FORCED IT: byte [0] page number, byte
 * [1] counter (section 4), byte [7] tag space, leaving 32 bits per frame.
 * device number 16 + device type 7 + trans type 8 + period 16 + slot phase 13
 * + clock accuracy 3 = 63 bits - identity and period alone are 47, so one
 * eight-byte frame would leave no room for a phase field at all. Hence a
 * two-frame set, same (index << 4) | (count - 1) convention as the
 * descriptor, moved to byte [2] so byte [1] stays a counter.
 *
 * THE PHASE IS RELATIVE: measured from frame 1's own t_sync, never an
 * absolute instant, since two receivers share no time base. Consequences:
 * frame 0 is timeless and frame 1 self-dating, so they need not be adjacent
 * or in order; and the phase is a FRACTION of the node's period (period/8192
 * units), self-scaling to a round-trip error of at most period/16384 counts
 * (15 us at 4 Hz, 122 us at the longest expressible period) - both inside
 * RADIANT_CHANNEL_GUARD_MAX_US.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO:
 *   - NO EPOCH, EVER. For a hostless node the epoch IS the boot counter, and
 *     broadcasting it in the clear would fingerprint the device across
 *     sessions and defeat per-boot device-number rotation. A keyed recipient
 *     recovers it by forward search against the key-group hint instead. Every
 *     one of the 64 bits above is assigned, enforcing the absence.
 *   - GUARD WIDENING, matching the descriptor: profile_handoff_apply()
 *     consumes clock accuracy through the same ladder and core call the
 *     descriptor uses, so a node isn't treated differently depending on how
 *     it was found. Only ever WIDENS - narrowing on somebody else's evidence
 *     is the one thing the estimator must not do.
 *   - NO SECURITY. Byte [7] is zero and asserted zero on the way back in. A
 *     forged handoff costs one search timeout on a node that isn't there,
 *     which is why this page can ship before key material does.
 */

#ifndef RADIANT_PROFILE_HANDOFF_H_
#define RADIANT_PROFILE_HANDOFF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* radiant_channel.h brings radiant_time_t and struct radiant_channel_id. The
 * codec below is pure, but profile_handoff_apply() drives a channel, and a
 * handoff that couldn't would be a format with no consumer. Dependency runs
 * application -> core, never the other way. */
#include <radiant_core/radiant_channel.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_HANDOFF_PAGE       0x12u
#define PROFILE_HANDOFF_FRAMES     2u
#define PROFILE_HANDOFF_FRAME_LEN  8u
/* Bytes [3..6]. Byte [7] is tag space and is not part of it. */
#define PROFILE_HANDOFF_AREA_BITS  32u

/* Slot phase is in period/8192 units. See the header comment for why a
 * fraction rather than a count of 1/32768 s. */
#define PROFILE_HANDOFF_PHASE_DEN  8192u
#define PROFILE_HANDOFF_PHASE_MAX  (PROFILE_HANDOFF_PHASE_DEN - 1u)

/*
 * Clock accuracy: the CEILING on the node's clock error, in ppm. BLE's
 * ladder, adopted rather than invented - it already spans the range that
 * matters, from an RC-oscillator coin-cell node to a crystal-referenced
 * sensor.
 *
 * Code 0 is the worst case AND the value to send when accuracy is unknown -
 * on no evidence, the worst case is the only safe answer. A consumer must
 * round OUTWARD: narrowing on no evidence is the one thing a window
 * estimator must never do.
 */
enum profile_handoff_clk {
	PROFILE_HANDOFF_CLK_UNKNOWN = 0, /* 251..500 ppm, and "not stated" */
	PROFILE_HANDOFF_CLK_250PPM  = 1,
	PROFILE_HANDOFF_CLK_150PPM  = 2,
	PROFILE_HANDOFF_CLK_100PPM  = 3,
	PROFILE_HANDOFF_CLK_75PPM   = 4,
	PROFILE_HANDOFF_CLK_50PPM   = 5,
	PROFILE_HANDOFF_CLK_30PPM   = 6, /* a 32 kHz crystal */
	PROFILE_HANDOFF_CLK_20PPM   = 7,
	PROFILE_HANDOFF_CLK_COUNT
};

/* The ppm ceiling a code promises, or 0 for a code outside the ladder. */
uint16_t profile_handoff_clk_ppm(uint8_t code);

/*
 * Everything a second receiver needs to open a tracked channel, and nothing
 * else. The absent field is the epoch, and its absence is a decision.
 */
struct profile_handoff {
	uint16_t device_number;
	uint16_t period;         /* counts of 1/32768 s; 0 is refused */
	uint16_t phase;          /* 0..8191, units of period/8192, from frame 1 */
	uint8_t  device_type;    /* 1..127; the pairing bit is not handed over */
	uint8_t  trans_type;
	uint8_t  clock_accuracy; /* enum profile_handoff_clk */
	uint8_t  counter;        /* byte [1] of both frames */
};

/*
 * Encode the whole set into `out`, which is PROFILE_HANDOFF_FRAMES frames of
 * PROFILE_HANDOFF_FRAME_LEN bytes, laid out consecutively.
 *
 * Returns 0, or -EINVAL for a handoff that does not describe a channel a
 * receiver could open.
 */
int profile_handoff_encode(const struct profile_handoff *h, uint8_t *out);

/*
 * Decode a complete set. `frames` is PROFILE_HANDOFF_FRAMES consecutive frames
 * in any order.
 *
 * Returns 0, -EINVAL for a frame that is not this page or a field outside its
 * range, -EPROTO for a reserved bit or tag byte that is not zero, and -EBADMSG
 * when the set is incomplete or its two frames carry different counters and are
 * therefore two different handoffs.
 */
int profile_handoff_decode(const uint8_t *frames, struct profile_handoff *out);

/* (index, count) of one frame. Returns -EINVAL if it is not a handoff frame. */
int profile_handoff_frame_index(const uint8_t *frame, uint8_t *index,
				uint8_t *count);

/*
 * The receiver-side accumulator, for the ordinary case where the two frames
 * arrive one slot apart among other pages. Zero it before first use.
 *
 * profile_handoff_rx_frame() returns 1 when the set is complete and `out` has
 * been filled, 0 when it has taken the frame and wants more, and a negative
 * errno for a frame it refuses. A frame carrying a new counter abandons a
 * partial set rather than merging with it, because two counters are two
 * handoffs and merging them would describe a channel neither receiver meant.
 */
struct profile_handoff_rx {
	uint8_t frames[PROFILE_HANDOFF_FRAMES][PROFILE_HANDOFF_FRAME_LEN];
	uint8_t have;    /* bitmask of frame indices held */
	uint8_t counter;
	bool    started;
};

int profile_handoff_rx_frame(struct profile_handoff_rx *rx,
			     const uint8_t *frame, struct profile_handoff *out);

/* 32768 Hz counts from the carrying frame's t_sync to the node's next slot. */
uint32_t profile_handoff_phase_counts(const struct profile_handoff *h);

/* The inverse. `phase_counts` is reduced modulo the period: it is a phase, and
 * a caller that measured across a slot boundary is one period out rather than
 * wrong. Returns 0..8191, or 0 for a zero period. */
uint16_t profile_handoff_phase_encode(uint32_t phase_counts, uint16_t period);

/*
 * The node's next slot on the RECIPIENT's clock. `t_carrier` is the t_sync at
 * which this receiver heard FRAME 1. Nothing absolute crosses the air.
 */
radiant_time_t profile_handoff_next_slot(const struct profile_handoff *h,
					 radiant_time_t t_carrier);

/* Microseconds to 32768 Hz counts, rounded to nearest. The inverse of
 * radiant_channel_counts_to_us(), and the sender needs it because it measures
 * in microseconds and the wire is in counts. */
uint32_t profile_handoff_us_to_counts(radiant_time_t us);

/*
 * THE SENDER SIDE: fill `out` from a channel this receiver is already
 * tracking, for a handoff whose frame 1 will go on the air at `t_carrier`.
 *
 * Only a TRACKING channel may be handed over: a searching channel has no
 * slot phase, and a merely configured one knows only what was typed into it.
 *
 * The phase is reduced modulo the period, so a `t_carrier` on either side of
 * the channel's next predicted slot gives the same answer, since a
 * scheduler decides when the frame actually goes out.
 *
 * Returns 0, -EINVAL for a bad argument, or -ENOTCONN when the channel is not
 * tracking.
 */
int profile_handoff_from_channel(uint8_t channel, radiant_time_t t_carrier,
				 uint8_t counter, uint8_t clock_accuracy,
				 struct profile_handoff *out);

/*
 * Apply a handoff to an assigned, closed slave channel: configure it, open
 * it, and land it in TRACKING with its next slot where the handoff says,
 * having armed no window and heard nothing.
 *
 * Goes through radiant_channel_on_acquired(), the same entry point a sweep
 * acquisition uses (deliberately not a second path into TRACKING), so a
 * handed-off channel ends field-for-field in the same state a swept one
 * would, including a guard estimator reset because nothing has been
 * measured about this master yet.
 *
 * `now` is the current instant and `t_carrier` the t_sync of frame 1. The
 * channel must already be assigned as a slave and closed.
 *
 * Returns RADIANT_CH_OK, or the wire error byte of the first call that refused.
 */
radiant_channel_err_t profile_handoff_apply(uint8_t channel,
					    const struct profile_handoff *h,
					    radiant_time_t t_carrier,
					    radiant_time_t now);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_HANDOFF_H_ */
