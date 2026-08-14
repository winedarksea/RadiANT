/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_transfer.h - acknowledged data and burst: the one engine both of them are.
 *
 * Provenance: clean-room, from docs/spike-b-part2-results.md (control byte,
 * the four measured data/ack pairs, every us figure below),
 * docs/spike-b-results.md, docs/ant-radio-link.md, radiant_frame.h,
 * radiant_radio_hal.h, and the buffer-ownership obligations B1-B5 in
 * src/ant_radio.h. Nothing here derives from sdk-ant, libant.a, or any
 * ANT+ device profile document. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * Why one module, not two
 * ---------------------------------------------------------------------------
 * Acknowledged data is a one-packet burst byte for byte: 0xAA decomposes as
 * exchange + not-ack + LAST + sequence 0 + slot-opener, exactly what "last
 * packet, sequence 0" means. A receiver dispatching on the control byte
 * cannot and must not tell the two apart - the distinction is serial-layer
 * only. So radiant_ack.c and radiant_burst.c share one encoder and one state
 * machine; radiant_transfer_ack_data() is radiant_transfer_submit() with a
 * copied eight-byte block carrying START|END.
 *
 * ---------------------------------------------------------------------------
 * The sequence is ONE BIT, so this is stop-and-wait
 * ---------------------------------------------------------------------------
 * No three-bit burst sequence exists in an ANT frame. Bit 4 alternates per
 * on-air packet and nothing else moves until the final packet sets bit 5
 * (docs/spike-b-part2-results.md: 19 bursts, sequence bit never disagreed
 * with packet index). A one-bit sequence cannot express a window, so the
 * protocol is stop-and-wait; a future design assuming a sliding window
 * (including the serial protocol's own two-bit burst-header sequence) is a
 * serial-side concept to translate here, not forward.
 *
 * THE SEQUENCE BIT ALTERNATES PER ON-AIR PACKET, NOT PER HOST BLOCK: a
 * 24-byte advanced-burst block fragments into three 8-byte packets, each
 * flipping the bit. A block of 8/16/24 bytes is 1/2/3 packets for the same
 * reason.
 *
 * ---------------------------------------------------------------------------
 * The acknowledgement is a full frame, and its bit 4 is a COMPLEMENT
 * ---------------------------------------------------------------------------
 * Not a short ack: same channel ID and eight payload bytes, carrying whatever
 * is in the acknowledger's broadcast buffer at that moment.
 *
 * Relative to the packet it answers: bit 6 set, bit 5 echoed, bit 4
 * COMPLEMENTED (165 adjacent CRC-valid pairs, no counterexamples:
 * 82->D2, 92->C2, A2->F2, B2->E2). Read as "the sequence bit I expect next"
 * this is exactly right for stop-and-wait; read as "an echo of what I just
 * received" it is exactly wrong.
 *
 * ---------------------------------------------------------------------------
 * The receive path must TRANSMIT - the tightest deadline in the link
 * ---------------------------------------------------------------------------
 * A channel that hears acknowledged data or a burst packet has ~1.55 ms
 * (RADIANT_TRANSFER_REPLY_US) to reply, which is why
 * radiant_transfer_on_data() arms the transmit BEFORE handing the payload to
 * its caller.
 *
 * EVERY MICROSECOND FIGURE BELOW IS A SOFTWARE TIMESTAMP (ADDRESS-interrupt
 * to ADDRESS-interrupt; the constant offset cancels but jitter doesn't).
 * Hardware ((D)PPI) timestamps are unmeasured - treat these as accurate to
 * tens of microseconds and no better; the guard band below is sized on that
 * basis, not on the sample standard deviations.
 *
 * ---------------------------------------------------------------------------
 * Buffer ownership - the expensive part
 * ---------------------------------------------------------------------------
 * src/ant_radio.h states obligations B1-B5. Short form: the bridge holds ONE
 * 24-byte block behind a binary semaphore, released only on
 * EVENT_TRANSFER_NEXT_DATA_BLOCK/_TX_COMPLETED/_TX_FAILED. Miss the first
 * once and a host burst stalls 1000 ms per packet before a plausible-looking
 * error.
 *
 * This module expresses those three events as radiant_transfer_event and
 * guarantees: EXACTLY ONE radiant_transfer_ops::done() CALL CARRIES ANY GIVEN
 * BLOCK POINTER, AND EXACTLY ONE TERMINAL EVENT ENDS ANY TRANSFER.
 *
 * A block is released with RADIANT_TRANSFER_EV_NEXT_BLOCK when its final
 * packet's acknowledgement arrives - the last moment the old block could
 * still be needed. Releasing earlier (at copy-into-frame-buffer time) would
 * buy one turnaround of pipelining but let the engine hold two blocks at
 * once; deliberately not taken, since B1 forbids re-reading a released
 * buffer for any reason and a single block in flight mirrors the bridge's
 * own single buffer.
 *
 * A failed submit call releases nothing and raises no event (B2) - why
 * radiant_transfer_submit() arms the radio before recording the block as
 * accepted, not after.
 *
 * ---------------------------------------------------------------------------
 * What is NOT here, because it was not measured
 * ---------------------------------------------------------------------------
 * Marked again at the code that would have used each:
 *   1. SENDER-SIDE RETRY. Never captured, so an empty reply window fails the
 *      transfer with RADIANT_TRANSFER_FAIL_NO_ACK and does not retransmit.
 *   2. ACKNOWLEDGING A SLOT OPENER. No such acknowledgement has ever been
 *      captured, so radiant_transfer_on_data() refuses (ENOTSUP), counted in
 *      stats.unackable_openers, rather than guessing bit 3's value.
 *   3. AN ACKNOWLEDGEMENT CARRYING ANYTHING BUT THE BROADCAST BUFFER. Every
 *      one observed carried it, so ops->broadcast_payload is required.
 *   4. BIT 3 AS "SLOT OPENER" IS `[inferred]` - measured only that it isn't
 *      master-vs-slave. cfg.slot_opener follows the reading that fits every
 *      captured frame; a master-originated multi-packet burst to a real
 *      slave would settle it.
 *   5. ANY PAYLOAD OTHER THAN EIGHT BYTES. Never observed; a 16/24-byte
 *      block fragments into 8-byte packets instead, and
 *      RADIANT_TRANSFER_PKT_BYTES is not a tunable.
 */

#ifndef RADIANT_TRANSFER_H_
#define RADIANT_TRANSFER_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant/radiant_frame.h>
#include <radiant/radiant_radio_hal.h>
/*
 * The arming authority. This module posts intents; it does not call
 * radiant_radio_tx() or radiant_radio_rx(). See "One radio, one arming authority"
 * below.
 */
#include <radiant/radiant_sched.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * One radio, one arming authority
 * ---------------------------------------------------------------------------
 * This module was written before radiant_sched.c existed and armed the radio
 * directly, at two sites: radiant_transfer_arm_tx() called radiant_radio_tx() and
 * arm_ack_window() in radiant_burst.c called radiant_radio_rx(). Both now post to
 * radiant_sched.c instead.
 *
 * That is not tidiness. The HAL permits exactly one operation in flight and
 * radiant_sched.h says in its first paragraph that it "is the only place in
 * radiant that calls radiant_radio_tx() or radiant_radio_rx()". Two schedulers racing
 * for one operation slot is a real bug with a specific shape: the transfer
 * engine's arm would take the slot from a merged receive window that several
 * tracked channels were sharing, or lose to it with RADIANT_RADIO_EBUSY and fail a
 * transfer for a reason the scheduler could have avoided by preempting the
 * window it owns. Nothing above either module could see it happen.
 *
 * Two consequences are visible in this header:
 *
 *   - struct radiant_transfer_cfg carries a CHANNEL NUMBER. The scheduler's slots
 *     are per channel and the engine has to name its own.
 *   - the engine no longer holds a HAL operation id, because radiant_sched.c does
 *     not expose one: it merges several channels' requests into one hardware
 *     operation and hands each channel back only its own events. So t->op holds
 *     RADIANT_TRANSFER_OP_EXTERNAL and the engine trusts its caller's routing,
 *     which is correct precisely because the caller is the thing that did the
 *     routing. radiant_search.h reached the same conclusion first and spells it
 *     RADIANT_SEARCH_OP_EXTERNAL.
 *
 * radiant_transfer_abort() still calls radiant_radio_abort() directly and is
 * deliberately left doing so - see the note there.
 */

/*
 * "The events routed to me are mine." Stored in t->op by an arm that went
 * through an authority with no HAL operation id to report; with this token
 * radiant_transfer_on_tx/rx_event() stop filtering by op id. Every other
 * value keeps the original filtering behaviour.
 *
 * 0xFFFFFFFF rather than a small number because the HAL's ids are
 * monotonically increasing and non-zero; a small sentinel could collide with
 * a real one on a long-running image.
 */
#define RADIANT_TRANSFER_OP_EXTERNAL ((uint32_t)0xFFFFFFFFu)

/* ---------------------------------------------------------------------------
 * Return codes
 *
 * This module's own, negative, and distinct from both the HAL's and the frame
 * layer's - the three are mixed freely inside one call and a shared numbering
 * would make a propagated error unattributable.
 *
 * radiant_api.c maps them onto the serial protocol's wire codes, which is where the
 * ANTW_* names belong. The intended mapping, stated here because it is the
 * reason these particular distinctions exist and nowhere else records it:
 *
 *   RADIANT_TRANSFER_EINVAL  -> ANTW_INVALID_MESSAGE
 *   RADIANT_TRANSFER_EBUSY   -> ANTW_TRANSFER_IN_PROGRESS
 *   RADIANT_TRANSFER_ESTATE  -> ANTW_CHANNEL_IN_WRONG_STATE
 *   RADIANT_TRANSFER_ESEQ    -> ANTW_TRANSFER_SEQUENCE_NUMBER_ERROR
 *   RADIANT_TRANSFER_ENOTSUP -> ANTW_INVALID_PARAMETER_PROVIDED
 *   RADIANT_TRANSFER_EIO     -> ANTW_TRANSFER_IN_ERROR
 * ---------------------------------------------------------------------------
 */
#define RADIANT_TRANSFER_OK        0
#define RADIANT_TRANSFER_EINVAL  (-1)  /* null pointer, bad block size, bad flags */
#define RADIANT_TRANSFER_EBUSY   (-2)  /* a transfer already owns this engine */
#define RADIANT_TRANSFER_ESTATE  (-3)  /* not initialised, or wrong state for this */
#define RADIANT_TRANSFER_ESEQ    (-4)  /* a segment flag that does not follow */
#define RADIANT_TRANSFER_ENOTSUP (-5)  /* well formed, but not measured / not
				    * reachable on this backend's timings */
#define RADIANT_TRANSFER_EIO     (-6)  /* the radio refused the operation */

/* ---------------------------------------------------------------------------
 * Geometry
 * ---------------------------------------------------------------------------
 */

/* Eight, and not a tunable. Every ANT frame across three spikes and 5,268
 * CRC-valid frames carried exactly eight payload bytes; advanced burst was
 * enabled twice and the stack fragmented rather than lengthening the frame. */
#define RADIANT_TRANSFER_PKT_BYTES  RADIANT_FRAME_PAYLOAD_STD

/* The largest host block. Plain burst carries 8; advanced burst carries 8, 16
 * or 24, which are 1, 2 and 3 on-air packets. Anything else is refused - a
 * dongle that accepts a block size it can never fragment cleanly is worse than
 * one that says no. */
#define RADIANT_TRANSFER_BLOCK_MAX  24u
#define RADIANT_TRANSFER_PKTS_MAX   (RADIANT_TRANSFER_BLOCK_MAX / RADIANT_TRANSFER_PKT_BYTES)

/*
 * Segment flags, marking the first and last block of a transfer. Chosen to
 * equal src/ant_radio.h's ANTR_BURST_SEGMENT_CONTINUE / _START / _END so that
 * radiant_api.c forwards the byte untouched instead of remapping it.
 *
 * Two constants for one concept is how the END bit ends up wrong - which
 * already happened once, in src/ant_radio.h, where END was 0x04 until a shim
 * checked it. radiant_api.c must therefore carry a BUILD_ASSERT that the three
 * pairs agree, exactly as the sdk-ant shim does; this header cannot carry it
 * because it must compile with no application header in scope.
 */
#define RADIANT_TRANSFER_SEG_CONTINUE 0x00u
#define RADIANT_TRANSFER_SEG_START    0x01u
#define RADIANT_TRANSFER_SEG_END      0x02u
#define RADIANT_TRANSFER_SEG_MASK     (RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END)

/* ---------------------------------------------------------------------------
 * Timing - all `[measured]`, all software timestamps
 *
 * From docs/spike-b-part2-results.md, runs A and B, ADDRESS interrupt to
 * ADDRESS interrupt:
 *
 *   master broadcast -> slave's reply      n=7  2186 us sd 8   / n=12 2191 sd 8
 *   data packet -> its acknowledgement     n=33 1567 us sd 21  / n=128 1559 sd 19
 *   acknowledgement -> next data packet    n=26 1546 us sd 26  / n=116 1550 sd 21
 *
 * The first packet of a burst costs the full slot turnaround; every packet
 * after it costs the shorter one. They are visibly different populations - sd 8
 * against sd 21 - and averaging them, as an earlier draft of the analyser did,
 * produces a number that describes neither.
 *
 * Read the header note above before arming a hardware timer from these.
 * ---------------------------------------------------------------------------
 */

/* Data packet's t_sync -> its acknowledgement's t_sync. The rounded midpoint of
 * 1567 and 1559. This is the deadline the receive path has to hit. */
#define RADIANT_TRANSFER_REPLY_US        1560u

/* Acknowledgement's t_sync -> the next data packet's t_sync. 1546 / 1550. */
#define RADIANT_TRANSFER_NEXT_PACKET_US  1550u

/* Master's broadcast t_sync -> a slave's reply t_sync. 2186 / 2191. Exported
 * because radiant_sched.c needs it to place the first packet of a slave-originated
 * exchange, which this module does not choose for itself. */
#define RADIANT_TRANSFER_SLOT_REPLY_US   2190u

/* One burst packet, end to end: 1560 + 1550 = 3110, against the 3113 us the
 * analyser reports for a running burst. Eight payload bytes per 3.11 ms is
 * about 20.6 kbit/s, which is what an ANT burst is worth on this link. */
#define RADIANT_TRANSFER_PACKET_US \
	(RADIANT_TRANSFER_REPLY_US + RADIANT_TRANSFER_NEXT_PACKET_US)

/*
 * Half-width of the receive window opened for an acknowledgement.
 *
 * Sized on the *observed range*, not on the standard deviation: data -> ack ran
 * 1515..1599 across 161 intervals, so 1560 +/- 250 covers it with about 155 us
 * of margin at each edge. Widening it further is not free - the window has to
 * open before its lower edge, and every microsecond of it is receive current
 * and another chance for an unrelated frame to close a STOP_ON_FIRST window.
 */
#define RADIANT_TRANSFER_ACK_GUARD_US    250u

/*
 * What a receiver does when a burst stops, `[measured, n=2]`: the master
 * answered 0xD2 then retransmitted that identical frame 20 more times at
 * 3143 us, spanning 66 ms, before giving up (21 attempts, twice). 3143 vs.
 * the 3113 a running burst takes is close enough to be the same timer and
 * far enough apart (1%) to record as two numbers.
 *
 * Implemented as radiant_transfer_reply_retransmit(), limit enforced here,
 * but this module does NOT own the timer - that would be a second scheduler.
 * radiant_sched.c owns no timer either (it arms absolute instants and lets
 * the backend hold them), so recovery is meant to be driven off a
 * housekeeping wake in api_sched_done().
 *
 * NOT WIRED YET, deliberately: there's no production caller. Our own receive
 * window closes ~2.9 ms before a peer retry can arrive, so an
 * acknowledger-side retransmission today would put replies on air into a
 * link where the peer's own retries are already unhearable - run 0 measured
 * a NON-FINAL 0xD2 repeat, never a final acknowledgement. Wiring it needs a
 * WAIT_NEXT state and a window at ack t_sync + 1550 +/- 250 (inbound-burst
 * work).
 */
#define RADIANT_TRANSFER_REPLY_RETRY_US  3143u
/* ATTEMPTS, not retransmissions: the measured 21 is one original plus twenty
 * repeats, and counting it the other way is how a limit ends up one off. */
#define RADIANT_TRANSFER_REPLY_ATTEMPTS_MAX 21u

/*
 * How long after the original acknowledgement a retransmission may still be
 * armed - a SECOND limit, not a restatement of the first. The attempt
 * counter alone isn't enough: TX_REPLY returns to IDLE without going through
 * finish(), so reply_ctrl stays loaded for the channel's life, and an
 * attempt count under 21 would say "repeat this" just as readily an hour
 * later as 3 ms later.
 *
 * 21 x 3143 us: the 21st attempt lands at 20 x 3143 = 62.9 ms, so this is
 * that plus one retry interval of slack, derived from the same two measured
 * numbers rather than picked.
 */
#define RADIANT_TRANSFER_REPLY_VALID_US \
	((uint32_t)RADIANT_TRANSFER_REPLY_ATTEMPTS_MAX * \
	 (uint32_t)RADIANT_TRANSFER_REPLY_RETRY_US)

/* Added to caps.min_arm_lead_us when a caller says "as soon as possible"
 * (RADIANT_TIME_NEVER). Small, because the point of the arm-lead figure is that it
 * is already the whole cost of getting a frame out. */
#define RADIANT_TRANSFER_ARM_SLACK_US    50u

/* ---------------------------------------------------------------------------
 * The pure encoder - the shared half of radiant_ack.c and radiant_burst.c
 * ---------------------------------------------------------------------------
 */

/*
 * The five control-byte fields for on-air packet `index` of a transfer.
 *
 *   exchange    always true - an acknowledged exchange either way
 *   ack         always false - these are data packets, not replies
 *   last        the caller's `last` (final packet of the final block)
 *   seq         index & 1, no wider form
 *   slot_opener slot_opener_role && index == 0
 *
 * The slot rule is the `[inferred]` one (gap 4): changing only channel role
 * moved 0x82->0x8A and 0xA2->0xAA, so the bit follows the frame that opens
 * the slot rather than the master transmitter - packet 0 of a master's burst
 * carries it, packets 1..N don't.
 *
 * Returns RADIANT_TRANSFER_OK, or RADIANT_TRANSFER_EINVAL for a null argument.
 */
int radiant_transfer_ctrl_fields(uint32_t index, bool last, bool slot_opener,
			     struct radiant_ctrl_fields *out);

/*
 * The same thing as the on-air byte, checked against the eleven values that
 * have actually been transmitted.
 *
 * Over every (index, last, slot_opener) this can produce exactly six bytes -
 * 0x82, 0x92, 0xA2, 0xB2 in slot, and 0x8A, 0xAA opening one - and all six are
 * measured, so the radiant_ctrl_observed() check can never fire. It is here anyway:
 * it is the assertion that a future edit to the field rules did not invent a
 * seventh.
 *
 * Returns RADIANT_TRANSFER_OK, or RADIANT_TRANSFER_EINVAL.
 */
int radiant_transfer_ctrl(uint32_t index, bool last, bool slot_opener, uint8_t *out);

/*
 * The acknowledgement this module puts on the air for a received data packet.
 *
 * Thin over radiant_ctrl_reply_for(), and named here so that the transfer layer's
 * two directions read symmetrically. Returns 0 - never a legal control byte -
 * for anything that is not one of the four measured pairs, INCLUDING the slot
 * openers 0x8A and 0xAA. See gap 2.
 */
uint8_t radiant_transfer_reply_ctrl(uint8_t data_ctrl);

/*
 * Is `ack_ctrl` an acknowledgement of `data_ctrl`?
 *
 * Bit 7 set on both, bit 6 clear on the data and set on the acknowledgement,
 * bit 5 equal, bit 4 complemented, bits 2:0 legal on both.
 *
 * BIT 3 IS DELIBERATELY NOT COMPARED. All four measured pairs are in-slot
 * frames answered by in-slot frames; what bit 3 of an acknowledgement to a slot
 * opener would be has never been captured, so a check on it would be a guess
 * that rejects real traffic. Everything else is checked, and for the four
 * measured pairs this predicate is exactly radiant_ctrl_reply_for().
 */
bool radiant_transfer_ack_matches(uint8_t data_ctrl, uint8_t ack_ctrl);

/* On-air packets in a host block: 1, 2 or 3. Returns 0 for any length that is
 * not 8, 16 or 24. */
uint8_t radiant_transfer_packets_in_block(uint8_t block_len);

/*
 * Build the tracking-geometry body [ttype][ctrl][d0..d7] for one exchange
 * frame.
 *
 * Goes through radiant_frame_make(), which refuses any field combination outside
 * the measured eleven, and then radiant_frame_encode(). It does NOT hand-set
 * ctrl_byte and pass it to radiant_frame_encode() directly: that path exists so a
 * bench experiment can put a candidate encoding on the air without editing the
 * frame module, and using it from production code would silently disable the
 * only check that stands between a field model and the 245 bytes nothing has
 * ever transmitted.
 *
 * Returns the body length written, or RADIANT_TRANSFER_EINVAL.
 */
struct radiant_transfer_cfg;
int radiant_transfer_build_body(const struct radiant_transfer_cfg *cfg, uint8_t ctrl,
			    const uint8_t *payload, uint8_t payload_len,
			    uint8_t *body, size_t body_cap);

/* ---------------------------------------------------------------------------
 * What the engine tells its owner
 * ---------------------------------------------------------------------------
 */

/*
 * The three events that carry buffer-ownership meaning, in the same order and
 * with the same meaning as the ANTW_EVENT_TRANSFER_* codes radiant_api.c turns them
 * into. There is no fourth: an event that does not release something has no
 * business on this path.
 */
enum radiant_transfer_event {
	/* This block is finished with and the engine wants the next one. Raised
	 * exactly once per accepted block that is not the last of the transfer,
	 * and never for a block that was not accepted (B3, B5). */
	RADIANT_TRANSFER_EV_NEXT_BLOCK = 0,
	/* The whole transfer succeeded. Releases the END block. */
	RADIANT_TRANSFER_EV_TX_COMPLETED,
	/* The whole transfer failed. Releases whatever block the engine still
	 * held, which may be none. */
	RADIANT_TRANSFER_EV_TX_FAILED
};

enum radiant_transfer_fail {
	RADIANT_TRANSFER_FAIL_NONE = 0,
	/* The reply window closed with nothing in it. NOT retried - see gap 1
	 * in the header comment. */
	RADIANT_TRANSFER_FAIL_NO_ACK,
	/* Something CRC-valid answered inside the window and it was not an
	 * acknowledgement of the packet we sent. */
	RADIANT_TRANSFER_FAIL_BAD_ACK,
	/* The radio refused an arm call or reported a failed operation. */
	RADIANT_TRANSFER_FAIL_RADIO,
	/* radiant_transfer_abort(). */
	RADIANT_TRANSFER_FAIL_ABORTED
};

struct radiant_transfer_result {
	enum radiant_transfer_event ev;
	/*
	 * The block being released, or NULL.
	 *
	 * NULL on every event of an acknowledged-data transfer, because
	 * antr_acknowledge_message_tx() copies its payload and transfers no
	 * ownership; NULL on a TX_FAILED raised while the engine held nothing.
	 * Never NULL on RADIANT_TRANSFER_EV_NEXT_BLOCK.
	 *
	 * Exactly one done() call in the whole transfer carries any given
	 * pointer. That single sentence is obligations B1 through B5.
	 */
	uint8_t *block;
	uint8_t  block_len;
	enum radiant_transfer_fail fail;   /* NONE unless ev is TX_FAILED */
	uint32_t packets;              /* on-air packets acknowledged */
};

/*
 * The engine's calls back into its owner.
 *
 * ALL THREE MAY RUN IN RADIO INTERRUPT CONTEXT. The HAL delivers completion
 * callbacks at the backend's highest interrupt priority, this module re-arms
 * from inside them because at a 1.55 ms turnaround that is the only path that
 * meets the deadline, and these callbacks are reached from there. So: no
 * blocking, no mutex, no lifecycle call, and no work proportional to anything.
 * Signalling an ISR-safe object - which is exactly what the bridge's
 * k_sem_give() on the burst block is - is the intended shape.
 */
struct radiant_transfer_ops {
	/* Required. See struct radiant_transfer_result. */
	void (*done)(void *ctx, const struct radiant_transfer_result *r);

	/* Optional. One received data packet of an inbound acknowledged
	 * message or burst, already acknowledged on the air by the time this
	 * runs. `last` is bit 5, which is what tells an inbound burst apart
	 * from acknowledged data at the layer that cares. */
	void (*rx_data)(void *ctx, const uint8_t *payload, uint8_t len,
			bool last);

	/*
	 * Required to acknowledge anything. The channel's current broadcast
	 * payload, eight bytes, which is what every acknowledgement ever
	 * captured carried. Returning false means the channel has nothing to
	 * broadcast and the engine refuses to acknowledge rather than sending
	 * something no measurement supports - see gap 3.
	 */
	bool (*broadcast_payload)(void *ctx,
				  uint8_t out[RADIANT_TRANSFER_PKT_BYTES]);
};

/* ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------
 */
struct radiant_transfer_cfg {
	struct radiant_channel_id id;
	uint8_t               net_addr[RADIANT_NET_ADDR_LEN];
	uint8_t               rf_index;   /* 57 for ANT+ */
	struct radiant_tx_power   power;

	/*
	 * The ANT channel this engine belongs to,
	 * 0 .. RADIANT_SCHED_MAX_CHANNELS-1. Needed because radiant_sched.c's
	 * slots are per channel; no default - zero is a real channel, and
	 * silently defaulting to it would cancel channel 0's window on every
	 * other channel's acknowledged message.
	 */
	uint8_t               channel;

	/*
	 * True if this end's frames open the channel slot (the `[inferred]`
	 * reading of bit 3 - the channel master). Only the FIRST packet of a
	 * transfer carries it. No safe default: getting it wrong either
	 * misdirects 0x82 where 0x8A belongs or claims a slot not owned, and
	 * neither is visible without a sniffer.
	 */
	bool slot_opener;

	const struct radiant_transfer_ops *ops;
	void                          *ctx;
};

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------
 */
enum radiant_transfer_state {
	RADIANT_TRANSFER_STATE_IDLE = 0,
	/* A data packet of an outbound transfer is armed or on the air. */
	RADIANT_TRANSFER_STATE_TX_DATA,
	/* The reply window is open. */
	RADIANT_TRANSFER_STATE_WAIT_ACK,
	/* The last packet of a block was acknowledged and the transfer
	 * continues, but the next host block has not been submitted yet. The
	 * radio is idle in this state, deliberately: the engine has nothing to
	 * send and holding a window open for a block that may never come is how
	 * a stalled host turns into wasted receive current. */
	RADIANT_TRANSFER_STATE_WAIT_BLOCK,
	/* We are the acknowledger and our reply is armed or on the air. */
	RADIANT_TRANSFER_STATE_TX_REPLY,
	/* radiant_transfer_abort() has been called and the terminal event of the
	 * cancelled operation has not arrived yet. It always arrives - the HAL
	 * guarantees it, and K5 measured the mock doing it without any fault
	 * injection - so this state always ends. */
	RADIANT_TRANSFER_STATE_ABORTING
};

struct radiant_transfer_stats {
	uint32_t packets_sent;       /* data packets acknowledged */
	uint32_t blocks_accepted;    /* submit calls that returned OK */
	uint32_t blocks_released;    /* done() calls carrying a block pointer */
	uint32_t transfers_ok;
	uint32_t transfers_failed;
	uint32_t acks_sent;          /* acknowledgements put on the air */
	uint32_t ack_retransmits;
	/* Events whose op id was not the engine's: a late terminal from a
	 * cancelled operation, or an event for somebody else's window. Counted
	 * rather than ignored silently, because "the frame was already in the
	 * receiver's pipeline when abort() ran" is a real race and a scheduler
	 * trace with no number for it is very hard to read. */
	uint32_t late_events;
	/* Inbound slot-opening data packets that could not be acknowledged,
	 * because no acknowledgement of one has ever been captured. Gap 2. */
	uint32_t unackable_openers;
	/* Inbound data packets refused because the channel had no broadcast
	 * payload to answer with. Gap 3. */
	uint32_t no_broadcast;
	/* Retransmissions refused because the saved acknowledgement was older
	 * than RADIANT_TRANSFER_REPLY_VALID_US. Non-zero means somebody is
	 * driving the retransmission off a timer that is not the measured one. */
	uint32_t stale_replies;
};

/*
 * One channel's transfer engine. Embed one per channel in radiant_channel.c; there
 * is no allocation anywhere in radiant.
 *
 * THE FIELDS ARE PRIVATE. They are in the header because the struct has to be
 * embeddable by value, not because anything outside radiant_ack.c and radiant_burst.c
 * may read them. Use radiant_transfer_state(), radiant_transfer_op(),
 * radiant_transfer_is_idle() and radiant_transfer_stats().
 */
struct radiant_transfer {
	struct radiant_transfer_cfg cfg;
	const struct radiant_pkt_format *fmt;
	bool     ready;
	uint16_t min_lead_us;

	enum radiant_transfer_state state;
	uint32_t                op;      /* the HAL operation in flight, or 0 */

	/* The block in hand. */
	uint8_t *block;
	uint8_t  block_len;
	uint8_t  block_seg;
	uint8_t  n_pkt_in_block;
	uint8_t  pkt_in_block;
	bool     block_released;
	bool     own_block;   /* false for acknowledged data, which is copied */

	/* Acknowledged data lives here, so that "a one-packet burst" is
	 * literally what the engine runs and not a second code path. */
	uint8_t ack_payload[RADIANT_TRANSFER_PKT_BYTES];

	uint32_t   pkt_index;      /* on-air packet index; bit 0 is the sequence */
	uint32_t   pkt_acked;      /* packets of THIS transfer acknowledged */
	uint8_t    cur_ctrl;       /* control byte of the packet in flight */
	radiant_time_t next_t_sync;    /* when the next packet is due */

	/* The acknowledger's side. */
	uint8_t reply_ctrl;
	uint8_t reply_payload[RADIANT_TRANSFER_PKT_BYTES];
	uint8_t reply_attempts;   /* including the original; see _ATTEMPTS_MAX */
	/* When the last acknowledgement of this exchange left, so that a
	 * retransmission can be refused for being ancient rather than merely
	 * over the attempt limit. See RADIANT_TRANSFER_REPLY_VALID_US. */
	radiant_time_t reply_t_sync;

	/* The frame currently on the air. The HAL DMAs out of this, so it must
	 * outlive the arm call and must not be a stack buffer. */
	uint8_t tx_body[RADIANT_FRAME_BODY_MAX];
	uint8_t tx_body_len;

	/* Likewise: the HAL says the filter array must stay valid until the
	 * terminal event. */
	struct radiant_rx_filter filter;

	struct radiant_transfer_stats stats;
};

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

/*
 * Wipe and configure one engine. Must be called with the radio initialised,
 * because it reads radiant_radio_caps_get() and refuses a backend that cannot meet
 * the turnaround:
 *
 *   caps.min_arm_lead_us + RADIANT_TRANSFER_ACK_GUARD_US must be <=
 *   RADIANT_TRANSFER_REPLY_US, or the reply window cannot be armed in time;
 *   caps.rx_to_tx_us must be <= RADIANT_TRANSFER_REPLY_US, which is the check the
 *   HAL's own comment on that field asks for.
 *
 * Returns RADIANT_TRANSFER_OK, RADIANT_TRANSFER_EINVAL, or RADIANT_TRANSFER_ENOTSUP for a
 * backend too slow to acknowledge. Failing loudly here is the point: a backend
 * that cannot turn a frame round in 1.55 ms cannot do acknowledged data at all,
 * and discovering that as "ERG mode does not work" is months too late.
 */
int radiant_transfer_init(struct radiant_transfer *t, const struct radiant_transfer_cfg *cfg);

/* ---------------------------------------------------------------------------
 * Sending
 * ---------------------------------------------------------------------------
 */

/*
 * Submit one block. The shared entry point: acknowledged data and burst are the
 * same call with different arguments, which is the whole finding of Spike B
 * part 2 expressed as code.
 *
 * @param block      8, 16 or 24 bytes. Fragmented into 1, 2 or 3 on-air packets.
 * @param segment    RADIANT_TRANSFER_SEG_* flags. A single-block transfer carries
 *                   START|END.
 * @param owned      true if the caller is transferring ownership under B1-B5
 *                   and expects the block back through done(); false if the
 *                   caller's buffer is already a copy this engine may keep for
 *                   the life of the transfer (which is what acknowledged data
 *                   is, and nothing else should pass false).
 * @param t_sync_at  Absolute t_sync for the first packet, or RADIANT_TIME_NEVER for
 *                   "as soon as the backend can". A slave answering its master
 *                   passes master_t_sync + RADIANT_TRANSFER_SLOT_REPLY_US; a master
 *                   passes its own slot instant. Ignored on a continuation
 *                   block, which is already scheduled at
 *                   ack_t_sync + RADIANT_TRANSFER_NEXT_PACKET_US.
 *
 * ON ANY NON-ZERO RETURN THE ENGINE OWNS NOTHING AND RAISES NO EVENT FOR THIS
 * BLOCK (B2). That is why the radio is armed before the block is recorded as
 * accepted, and it is the difference between a rejected block and a released
 * one: releasing a block the bridge still holds hands back a semaphore that
 * belongs to a different block, and the next host packet overwrites a buffer
 * the radio is transmitting from (B5).
 *
 * Returns RADIANT_TRANSFER_OK, or:
 *   EINVAL   null, a block length that is not 8/16/24, unknown segment bits
 *   ESTATE   radiant_transfer_init() has not run
 *   EBUSY    a transfer is mid-packet; the host must wait for NEXT_BLOCK
 *   ESEQ     START on a continuation, or a continuation with no transfer open
 *   EIO      the radio refused the transmit
 */
int radiant_transfer_submit(struct radiant_transfer *t, uint8_t *block, uint8_t len,
			uint8_t segment, bool owned, radiant_time_t t_sync_at);

/* Burst: ownership transfers, per B1-B5. */
static inline int radiant_transfer_burst_block(struct radiant_transfer *t,
					   uint8_t *block, uint8_t len,
					   uint8_t segment,
					   radiant_time_t t_sync_at)
{
	return radiant_transfer_submit(t, block, len, segment, true, t_sync_at);
}

/*
 * Acknowledged data: one packet, START|END, payload copied.
 *
 * On air this is byte for byte a one-packet burst - 0xAA opening a slot, 0xA2
 * inside one - and it runs through radiant_transfer_submit() precisely so that
 * nothing can drift between the two. `len` must be RADIANT_TRANSFER_PKT_BYTES.
 *
 * Exactly one of TX_COMPLETED or TX_FAILED follows an accepted call, with a
 * NULL block. This is how Zwift learns a trainer took a resistance change, so a
 * missing event is a stuck ERG session rather than a cosmetic gap.
 */
int radiant_transfer_ack_data(struct radiant_transfer *t, const uint8_t *payload,
			  uint8_t len, radiant_time_t t_sync_at);

/*
 * Cancel whatever is in flight. The cancelled operation's terminal event
 * STILL ARRIVES (the HAL guarantees it), so this may complete synchronously
 * from thread context or on the deferred terminal from inside a callback.
 * Either way exactly one TX_FAILED with RADIANT_TRANSFER_FAIL_ABORTED ends
 * the transfer, releasing any block still held.
 *
 * Returns RADIANT_TRANSFER_OK, including when there was nothing to cancel.
 */
int radiant_transfer_abort(struct radiant_transfer *t);

/* ---------------------------------------------------------------------------
 * Receiving - the path that has to transmit
 * ---------------------------------------------------------------------------
 */

/*
 * One inbound acknowledged-data or burst packet, already decoded by
 * radiant_frame_decode() and delivered from the channel's tracked window.
 *
 * Arms the acknowledgement first and calls ops->rx_data afterwards. That order
 * is the whole point: there are about 1.55 ms between this packet's t_sync and
 * the instant the peer expects a reply, and spending any of them in the
 * caller's payload handling is the one way to miss a deadline that has no error
 * code attached to it.
 *
 * Returns RADIANT_TRANSFER_OK, or:
 *   EINVAL   null, or a control byte that is not a data packet
 *   ESTATE   not initialised, engine busy, or no broadcast payload (gap 3)
 *   ENOTSUP  the packet opened a slot, and no acknowledgement of one has ever
 *            been captured, so there is no measured reply to send (gap 2)
 *   EIO      the radio refused the transmit - which at this deadline usually
 *            means the frame arrived later than the scheduler predicted
 */
int radiant_transfer_on_data(struct radiant_transfer *t, const struct radiant_frame *f,
			 radiant_time_t t_sync);

/*
 * Send the last acknowledgement again, per RADIANT_TRANSFER_REPLY_RETRY_US
 * and RADIANT_TRANSFER_REPLY_ATTEMPTS_MAX (see those constants for the
 * measurement); this function enforces the limit.
 *
 * The TIMER IS NOT HERE - radiant_sched.c decides when a channel can spend a
 * slot on a repeat; a timer inside this engine would be a second scheduler.
 *
 * Returns RADIANT_TRANSFER_OK, RADIANT_TRANSFER_ESTATE (nothing to repeat,
 * engine busy, or limit reached) or RADIANT_TRANSFER_EIO.
 */
int radiant_transfer_reply_retransmit(struct radiant_transfer *t, radiant_time_t t_sync_at);

/* ---------------------------------------------------------------------------
 * Radio events
 *
 * The HAL has one global callback pair for the whole image; radiant_sched.c
 * owns it and routes each event to the CHANNEL that posted the request.
 *
 * ROUTING MUST BE EXACT: a conservative fan-out (hand every event to every
 * engine, let each filter by op id) stopped being safe once the arming
 * authority moved to the scheduler, which has no HAL op id to report - the
 * engine stores RADIANT_TRANSFER_OP_EXTERNAL and stops filtering, so handing
 * engine B's completion to engine A would make A act on it.
 *
 * Both functions still ignore an event whose op is a real id that isn't
 * theirs (counted in stats.late_events) - the path that catches an event for
 * an operation already retired (after an abort, t->op is 0).
 *
 * Both run in radio interrupt context and may re-arm the radio before
 * returning - the only path that meets a 1.55 ms deadline.
 * ---------------------------------------------------------------------------
 */
void radiant_transfer_on_tx_event(struct radiant_transfer *t,
			      const struct radiant_tx_event *e);
void radiant_transfer_on_rx_event(struct radiant_transfer *t,
			      const struct radiant_rx_event *e);

/* ---------------------------------------------------------------------------
 * Shared between radiant_ack.c and radiant_burst.c
 *
 * Not part of the module's outward API and not to be called from anywhere else.
 * It is declared here rather than in a private header because there are exactly
 * two translation units, one function, and the alternative - a third file whose
 * only job is to let two files see one prototype - costs more than it explains.
 * ---------------------------------------------------------------------------
 */

/*
 * Build one exchange frame and post it to the scheduler.
 *
 * Writes t->tx_body, which is where the backend DMAs from, and sets t->op to
 * RADIANT_TRANSFER_OP_EXTERNAL. Does NOT touch t->state: the caller knows whether
 * it just armed a data packet or an acknowledgement and this function does not
 * guess.
 *
 * POSTING IS NOT COMMITTING. From inside a radio callback - which is where
 * every arm after the first happens, because at a 1.55 ms turnaround it is the
 * only path that works - radiant_sched.c runs its commit on the way out and the
 * frame is armed with no thread wakeup in between. From thread context the
 * caller owes one radiant_sched_tick(); radiant_api.c does that at the end of the
 * antr_* call that provoked the submit.
 *
 * t_sync_at is taken literally. A caller that means "as soon as possible"
 * resolves that itself before calling, because a backend never silently
 * transmits late - a late frame lands in the next slot and is worse than no
 * frame - and quietly sliding a reply past its 1.55 ms deadline in here would
 * turn a loud RADIANT_RADIO_ETIME into a silent one.
 *
 * Returns RADIANT_TRANSFER_OK, RADIANT_TRANSFER_EINVAL or RADIANT_TRANSFER_EIO.
 */
int radiant_transfer_arm_tx(struct radiant_transfer *t, uint8_t ctrl,
			const uint8_t *payload, uint8_t payload_len,
			radiant_time_t t_sync_at);

/* ---------------------------------------------------------------------------
 * Inspection
 * ---------------------------------------------------------------------------
 */
enum radiant_transfer_state radiant_transfer_state(const struct radiant_transfer *t);
uint32_t                radiant_transfer_op(const struct radiant_transfer *t);
bool                    radiant_transfer_is_idle(const struct radiant_transfer *t);
/* Never NULL for a non-NULL engine. */
const struct radiant_transfer_stats *radiant_transfer_stats(const struct radiant_transfer *t);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_TRANSFER_H_ */
