/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_handoff.h - page 0x12, the sync handoff. BLE's PAST, for ANT.
 *
 * Provenance: docs/radiant-telemetry.md section 12 (the page layout, the
 * two-frame arithmetic, the relative-phase rule, the clock-accuracy ladder and
 * the no-epoch constraint) and section 4 (the two positional invariants that
 * fix bytes [0], [1] and [7]). That document is this project's own written
 * specification. No adopter-gated ANT+ device profile document was read for
 * this file, no sdk-ant source was consulted, and nothing here derives from
 * libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this is
 * ---------------------------------------------------------------------------
 * The one page in the envelope document that a NODE does not send. A receiver
 * that already tracks a node broadcasts it so that a SECOND receiver can
 * configure a channel and go straight to tracking - and so that the same
 * receiver, having persisted it across a reboot, re-acquires in one channel
 * period instead of one sweep.
 *
 * A wildcard search derives its address filter from the low byte of the device
 * number, so it walks a set of filters, one dwell each, and is only certain to
 * have found every node in range after a full sweep - 32 windows on the nRF
 * backend, and the dominant term in discovery latency. None of that work is
 * necessary when somebody already knows the answer. THE SWEEP IS SKIPPED, NOT
 * SHORTENED.
 *
 * ---------------------------------------------------------------------------
 * Two frames, and the arithmetic that forced it
 * ---------------------------------------------------------------------------
 * Byte [0] is the page number and byte [1] is the counter, both fixed by
 * section 4; byte [7] is tag space by the second of the same invariants, since
 * no RadiANT page may put a field where an authentication tag has to go. That
 * leaves 32 bits per frame.
 *
 *   device number 16 + device type 7 + transmission type 8
 *     + period 16 + slot phase 13 + clock accuracy 3  =  63 bits
 *
 * Identity and period alone are 47 bits, so a single eight-byte frame would
 * have had one bit spare and NO ROOM FOR A PHASE FIELD AT ALL - and the phase
 * is the entire point of the page. So this is a set of consecutive frames in
 * exactly the sense section 6 already defines for the descriptor, for exactly
 * the same reason, with the same (index << 4) | (count - 1) encoding moved to
 * byte [2] so that byte [1] can stay a counter.
 *
 * ---------------------------------------------------------------------------
 * The phase is RELATIVE, and that is not a detail
 * ---------------------------------------------------------------------------
 * The slot phase is measured from the t_sync of the frame that carries it -
 * frame 1 - and never from an absolute instant. Two receivers share no time
 * base, so an absolute slot time is not merely imprecise, it is
 * uninterpretable on the receiver that has to consume it.
 *
 * Two consequences, both free:
 *
 *   - frame 0 is timeless and frame 1 is self-dating, so the two need not be
 *     adjacent and need not arrive in order;
 *   - the phase is a FRACTION of the node's own period (period/8192 units)
 *     rather than a count, which is self-scaling: round-trip error is at most
 *     period/16384 counts, 15 us at the 4 Hz ANT+ period and 122 us at the
 *     longest period the field can express. Both are inside
 *     RADIANT_CHANNEL_GUARD_MAX_US, so the first window after a handoff is no
 *     wider than the first window after a sweep acquisition.
 *
 * ---------------------------------------------------------------------------
 * What this deliberately does NOT do
 * ---------------------------------------------------------------------------
 *   - NO EPOCH, EVER. It is the obvious field to add - a receiver handing over
 *     everything else it knows would naturally include it, and it would save a
 *     keyed recipient a forward search - and it must not be added. For a
 *     hostless node the epoch IS the boot counter, and a slowly incrementing
 *     number broadcast in the clear fingerprints the device across sessions and
 *     defeats per-boot device-number rotation on its own. That is why the
 *     compatibility beacon dropped it, and this page is broadcast in the clear
 *     too. A keyed recipient recovers it by forward search against the
 *     key-group hint; an unkeyed one never needed it. The bit budget above is
 *     the enforcement: every one of the 64 bits is assigned.
 *   - NO GUARD WIDENING. The clock accuracy is carried and exposed and is not
 *     yet consumed. Widening radiant_channel_guard_us() toward an announced
 *     ceiling belongs with the descriptor schedule block that announces it in
 *     the first place; a handoff that widened the guard while a descriptor
 *     could not would make one node behave differently depending on how it was
 *     found.
 *   - NO SECURITY. Byte [7] is written as zero and asserted to be zero on the
 *     way back in. A forged handoff costs a receiver one search timeout on a
 *     node that is not there, which is why this page can ship before the key
 *     material does.
 */

#ifndef RADIANT_PROFILE_HANDOFF_H_
#define RADIANT_PROFILE_HANDOFF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* radiant_channel.h brings radiant_time_t and struct radiant_channel_id with
 * it. This is the only file in src/profiles/ that includes anything from
 * radiant_core: the codec below is pure, but profile_handoff_apply() drives a
 * channel, and a handoff that could not do that would be a format with no
 * consumer. The dependency runs application -> core, never the other way. */
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
 * Clock accuracy: the CEILING on the node's clock error, in ppm. BLE's ladder,
 * adopted rather than invented - it already spans exactly the range that
 * matters here, from a coin-cell node running its RC oscillator with no 32 kHz
 * crystal at the top to a crystal-referenced sensor at the bottom, and a second
 * ladder meaning the same thing differently would be a pure interoperability
 * cost.
 *
 * Code 0 is the worst case AND the value to send when the accuracy is not
 * known, because on no evidence the worst case is the only safe answer. A
 * consumer must round OUTWARD from these: narrowing on no evidence is the one
 * thing a window estimator must never do.
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
 * THE SENDER SIDE: fill `out` from a channel this receiver is already tracking,
 * for a handoff whose frame 1 will go on the air at `t_carrier`.
 *
 * Only a TRACKING channel may be handed over, and that is a precondition rather
 * than a nicety: a searching channel has no slot phase to give, and a channel
 * that has merely been configured knows only what somebody typed into it.
 *
 * The phase is reduced modulo the period, so a `t_carrier` on either side of
 * the channel's next predicted slot gives the same answer - which matters
 * because a scheduler decides when the frame actually goes out.
 *
 * Returns 0, -EINVAL for a bad argument, or -ENOTCONN when the channel is not
 * tracking.
 */
int profile_handoff_from_channel(uint8_t channel, radiant_time_t t_carrier,
				 uint8_t counter, uint8_t clock_accuracy,
				 struct profile_handoff *out);

/*
 * Apply a handoff to an assigned, closed slave channel: configure it, open it,
 * and land it in TRACKING with its next slot where the handoff says, having
 * armed no window and heard nothing.
 *
 * This is the whole feature. It goes through radiant_channel_on_acquired() -
 * the same entry point a sweep acquisition goes through, and deliberately not a
 * second path into TRACKING - so a handed-off channel is in the same state a
 * swept one would be, field for field, including a guard estimator that has
 * been reset because nothing has been measured about this master yet.
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
