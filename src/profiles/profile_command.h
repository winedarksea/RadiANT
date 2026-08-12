/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_command.h - the reliable-command path: response slots, the
 * idempotency state machine, and the inline tag at both of its widths.
 *
 * Provenance: docs/radiant-telemetry.md section 9 in full (the page 0x10 and
 * 0x11 layouts, the result-code table, the command vocabulary, the idempotency
 * rule stated as four numbered steps, and the honest-limit paragraph on a
 * 16-bit inline tag together with its 2026-08-11 amendment for the long-range
 * PHY) and section 6's schedule block (the downlink interval, phase and dwell
 * this file finally opens a window for). That document is this project's own
 * written specification. No adopter-gated ANT+ device profile document was read
 * for this file, no sdk-ant source was consulted, and nothing here derives from
 * libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this is, and what it is worth
 * ---------------------------------------------------------------------------
 * A command reaches a node only when the node is listening. Today that is near
 * the node's own transmit slot, so actuator latency is bounded by the channel
 * period - or, for a sparse node, by the heartbeat, which is up to 60 s for the
 * air-conditioning case section 1 of the envelope document is written around.
 *
 * NONE OF THIS IS AVAILABLE TO ZWIFT AND THAT IS NOT A DEFECT. FE-C is an ANT+
 * compatibility profile; its resistance command waits for the next FE-C slot
 * and no amount of work here changes that, because changing it would mean
 * changing somebody else's certified profile. Everything in this file is
 * available only to RadiANT-native control device types. It is written now so
 * that the first such type inherits a mechanism instead of inventing one.
 *
 * ---------------------------------------------------------------------------
 * RESPONSE SLOTS, NOT A SHORTER PERIOD
 * ---------------------------------------------------------------------------
 * The obvious fix - run a control channel faster - is brute force. It multiplies
 * duty across the 99 % of slots in which nothing is commanded, and it does so on
 * the merged RF 57 window every other channel shares, which is the load-bearing
 * property of this whole link layer.
 *
 * So the period does not change. The node keeps its telemetry cadence and opens
 * a downlink sub-slot at a FIXED OFFSET inside it - the schedule block's phase,
 * recurring at the schedule block's interval, for the schedule block's dwell.
 * The cost is node receive current during that dwell and nothing else: no extra
 * transmission, no extra airtime, no change to anybody else's window. A command
 * therefore lands on a predictable phase, and the worst case is one interval
 * rather than one heartbeat.
 *
 * THE INTERVAL IS NOT THE PERIOD, and that separation is the point of the
 * design. A node at a 2 s heartbeat may announce a 1 s downlink interval, or a
 * 1/32 s one, without touching what it transmits. Latency and duty are then two
 * independent knobs instead of one, which is what "response slots, not a shorter
 * period" means in code.
 *
 * ---------------------------------------------------------------------------
 * WHY THERE IS NO NEW SCHEDULER SLOT KIND, WHICH WAS THE OPEN QUESTION
 * ---------------------------------------------------------------------------
 * radiant_sched.c has SLOT_RX, SLOT_TX and SLOT_ED, and the energy-detect phase
 * added the third. The natural assumption is that a response slot is a fourth.
 * It is not, and the reason is worth writing down because the assumption is
 * cheap to act on and expensive to undo.
 *
 * SLOT_ED needed its own kind because it is not a receive: it has no packet
 * format, no address filter, no open and no close, and it sweeps a RANGE of RF
 * indices while every other operation names one. Not a single one of those is
 * true of a response slot. A response slot has a format (the node's own), a
 * filter (the node's own address), an rf_index (the node's own), an open (the
 * announced phase) and a close (the announced dwell). It IS a bounded receive
 * window - the same shape radiant_burst.c's acknowledgement window already
 * posts - and a request through radiant_sched_request_rx() describes it
 * exactly.
 *
 * Two consequences follow, and the second is the one that decides it:
 *
 *   - A dedicated kind would have to be touched into the enum, the arm
 *     function, the preemption rules in both directions, the leader scan, the
 *     expiry sweep, the terminal keep-rule, a HAL callback, a request entry
 *     point, a Kconfig symbol and a test suite. That is the energy-detect
 *     phase's checklist, and it is twenty-odd sites.
 *   - A dedicated kind MERGES WITH NOTHING. radiant_sched.c's membership test
 *     is `kind == SLOT_RX` and its five merge rules follow from that. Posting a
 *     response slot as an ordinary bounded RX means a receiver commanding three
 *     nodes on RF 57, with windows that overlap, gets ONE armed window covering
 *     all three for free. A fourth kind would get three.
 *
 * So the RF content of this phase is slot placement alone, and radiant_sched.c
 * is unmodified. That is a claim rather than a hope:
 * radiant_core/tests/src/test_command.c asserts that a node with no downlink
 * window arms the identical sequence of windows it armed before this file
 * existed.
 *
 * ---------------------------------------------------------------------------
 * THE NODE LISTENS ONLY WHEN IT EXPECTS A COMMAND
 * ---------------------------------------------------------------------------
 * An always-open window at the announced cadence is a receiver running at the
 * announced duty forever, which for a coin cell is the cost this design exists
 * to avoid. So arming is explicit and consumable: profile_cmd_expect() buys a
 * number of windows, profile_cmd_arm_listen() spends one, and a node that
 * expects nothing arms nothing and spends nothing.
 *
 * A received command silently buys PROFILE_CMD_RETRY_WINDOWS more. That is what
 * makes retry free in the sense section 9 means it: a commanding receiver whose
 * acknowledgement was lost retries into a window that is still open, rather than
 * waiting for the application to ask for one.
 *
 * ---------------------------------------------------------------------------
 * THE TAG, AND THE ONE THING THE LONG-RANGE PHY CHANGED
 * ---------------------------------------------------------------------------
 * Section 9 fixed the inline tag at 16 bits and said plainly that two bytes was
 * the limit "until some measured way of putting a longer frame on the air
 * exists". The long-range phase built one: a RadiANT-authored length field on
 * the LR PHY, permitted there and only there because nobody else can hear it.
 *
 * So the width is now a function of the frame configuration:
 * profile_cmd_tag_bytes() returns 8 for RADIANT_FRAME_CFG_LR and 2 for
 * everything else. It is deliberately NOT a node preference and not a
 * negotiation. A 64-bit tag on an 8-byte ANT frame does not fit, and a node
 * that could choose 16 bits on a channel that fits 64 would be choosing to be
 * forgeable for no saving.
 *
 * The difference is the difference between a defensible actuator path and a
 * rate-limit-mitigated one. At 16 bits a forgery attempt succeeds with
 * probability 2^-16 and the backoff below is load-bearing; at 64 bits it is
 * 2^-64 and the backoff is belt and braces.
 *
 * ---------------------------------------------------------------------------
 * A CONTROL CHANNEL IS CODED S=8, AND THAT IS A CONFIGURATION RULE
 * ---------------------------------------------------------------------------
 * No radio work is implied by it. At eight bytes an S=8 frame is about 1.23 ms
 * against a period measured in hundreds of milliseconds, so airtime is not this
 * phase's latency problem and never was - slot placement is. What S=8 buys is
 * the link budget an actuator at range needs and, through the length field it
 * unlocks, the 64-bit tag above. The two arrive together or neither does, which
 * is why profile_cmd_channel_check() checks them as one condition rather than
 * two.
 */

#ifndef RADIANT_PROFILE_COMMAND_H_
#define RADIANT_PROFILE_COMMAND_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_radio_hal.h>
#include <radiant_core/radiant_sched.h>
#include <radiant_core/radiant_sec.h>

#include "profile_sched.h"
#include "profile_schedule.h"
#include "profile_telemetry.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * The control-channel convention
 * ---------------------------------------------------------------------------
 */

/* What a RadiANT-native control device type is configured with. Named rather
 * than described, so that the first such type cites a constant instead of
 * repeating a sentence out of a plan. */
#define PROFILE_CMD_CTRL_CODING    PROFILE_SCHED_CODING_S8
#define PROFILE_CMD_CTRL_FRAME_CFG RADIANT_FRAME_CFG_LR

/* PROFILE_TLM_CMD_TAG_LR on the long-range configuration, PROFILE_TLM_CMD_TAG_STD
 * on every other. The whole of the width decision, in one function, so no
 * caller gets to make it twice. */
uint8_t profile_cmd_tag_bytes(enum radiant_frame_cfg cfg);

/*
 * Refuse a control channel that is not one.
 *
 * `s` is the node's announced schedule block and `cfg` the frame configuration
 * the channel actually runs. Returns 0, or -EINVAL when the two disagree, or
 * -ENOENT when the block announces no downlink window - a control channel with
 * no window to command it in is the configuration mistake this exists to catch,
 * and it is silent otherwise.
 *
 * A NON-CONTROL CHANNEL IS NOT REFUSED BY THIS FUNCTION BECAUSE IT NEVER CALLS
 * IT. Commands on an ordinary 1 M telemetry channel are legal, carry a 16-bit
 * tag and are exactly what section 9 specified; this is the stricter setup a
 * type may opt into, not a floor imposed on the page.
 */
int profile_cmd_channel_check(const struct profile_schedule *s,
			      enum radiant_frame_cfg cfg);

/* ---------------------------------------------------------------------------
 * The idempotency rule's constants - section 9, stated as numbers
 * ---------------------------------------------------------------------------
 */

/*
 * "Accept if delta_u8(seq, last_seq) is in 1..64". Wide enough to survive a run
 * of loss, narrow enough that a stale command cannot walk back into range.
 */
#define PROFILE_CMD_ACCEPT_WINDOW 64u

/*
 * THE BACKOFF ON FAILED VERIFICATIONS, which section 9 requires and which is
 * the whole mitigation for a 16-bit tag.
 *
 * The arithmetic, because a backoff ladder that is not costed is a number
 * somebody will relax:
 *
 *   - The n-th consecutive failure blocks for 100 ms << min(n-1, 13), so the
 *     first fourteen failures cost 1638 s in total and every failure after that
 *     costs 819.2 s.
 *   - That caps a flooder at about 105 attempts per day. Against a 2^-16 tag
 *     the expected time to one forged acceptance is therefore on the order of
 *     625 days.
 *   - Without the backoff, a 4 Hz channel admits roughly 345,600 attempts per
 *     day and the same expectation is about four and a half hours. The ladder
 *     is the difference between those two numbers and nothing else.
 *
 * Rate-limiting ACCEPTED commands, which an earlier draft of section 9 asked
 * for, is not done here and the document explains why: a brute-force attacker
 * produces 65535 rejections per acceptance, so throttling acceptances throttles
 * 0.002 % of the traffic. Throttle the failures.
 */
#define PROFILE_CMD_BACKOFF_BASE_US   100000u
#define PROFILE_CMD_BACKOFF_SHIFT_MAX 13u

/* Windows a received command buys, so a retry lands without the application
 * asking for one. Two rather than one because the acknowledgement that was lost
 * may be lost again. */
#define PROFILE_CMD_RETRY_WINDOWS 2u

/* ---------------------------------------------------------------------------
 * The node
 * ---------------------------------------------------------------------------
 */

/*
 * Counters, and they are the "log rejections" half of section 9's mitigation.
 * A forging attacker is meant to be a detectable flood rather than a quiet
 * success, and a flood is only detectable if somebody counted it.
 */
struct profile_cmd_stats {
	uint32_t executed;    /* commands whose effect actually ran */
	uint32_t repeats;     /* duplicates answered from the stored ack */
	uint32_t bad_tag;     /* failed verifications */
	uint32_t bad_seq;     /* outside the accept window */
	uint32_t rejected;    /* the actuator said no: unknown, bad arg, busy */
	uint32_t throttled;   /* dropped unverified while backed off */
	uint32_t adopted;     /* rule 4 fired: a sequence adopted from nothing */
	uint32_t windows;     /* response slots actually armed */
	uint32_t window_hits; /* response slots a command arrived in */
};

/*
 * The actuator, supplied by the device type.
 *
 * Returns a section 9 result code: PROFILE_TLM_RESULT_OK when the effect ran,
 * or UNKNOWN_CMD / BAD_ARG / BUSY. `*value` receives the resulting value of the
 * target field, which the acknowledgement carries so that a commanding receiver
 * learns the outcome rather than only that its packet arrived.
 *
 * IT IS CALLED AT MOST ONCE PER SEQUENCE NUMBER, and that is the contract the
 * whole file exists to provide. An implementation does not have to be
 * idempotent itself.
 */
typedef uint8_t (*profile_cmd_exec_fn)(uint8_t cmd, uint8_t target, uint16_t arg,
				       uint16_t *value, void *user);

/* Where a response slot is placed on the air. The node's own channel, in every
 * field - a downlink window is not a second channel and must not become one. */
struct profile_cmd_slot {
	uint8_t                          ch;
	const struct radiant_pkt_format *fmt;
	uint8_t                          rf_index;
	const struct radiant_rx_filter  *filters;
	uint8_t                          n_filters;
};

struct profile_cmd_cfg {
	/* Identity the tag is bound to. The epoch is what makes a command
	 * captured yesterday and replayed after a reboot fail verification
	 * instead of being adopted as a fresh sequence, which is section 9 rule
	 * 4's whole justification for the epoch's four bytes. */
	uint32_t epoch;
	uint16_t devnum;

	/* K_cmd is DERIVED from this per epoch, never stored. NULL builds a node
	 * that verifies nothing and therefore accepts nothing: section 9 rule 1
	 * is "verify the tag first", and a node with no key cannot. */
	const struct radiant_sec_key *k_root;

	/* PROFILE_TLM_CMD_TAG_STD or PROFILE_TLM_CMD_TAG_LR. Take it from
	 * profile_cmd_tag_bytes() rather than choosing it. */
	uint8_t tag_len;

	profile_cmd_exec_fn execute;
	void               *user;

	struct profile_cmd_slot slot;
};

struct profile_cmd_node {
	struct profile_cmd_cfg cfg;

	/* Section 9's idempotency state, and it is exactly the two things the
	 * document names: "The node keeps last_seq and the result it
	 * produced." */
	uint8_t                    last_seq;
	bool                       seq_known;
	struct profile_command_ack stored;
	bool                       stored_valid;

	/* The backoff. `fails` counts CONSECUTIVE failures and is cleared by any
	 * successful verification, so a legitimate receiver that is merely
	 * unlucky never climbs the ladder. */
	uint8_t        fails;
	radiant_time_t blocked_until;

	/* The response slot. `anchor` is any instant at which a window opens;
	 * the grid is anchor + n * interval in both directions, so it never has
	 * to be advanced. */
	struct profile_schedule sched;
	radiant_time_t          anchor;
	bool                    have_window;
	uint32_t                expect_left;
	radiant_time_t          armed_open;
	radiant_time_t          armed_close;

	struct profile_cmd_stats stats;
};

/* Latch the configuration. -EINVAL for a tag width that is neither of the two,
 * a missing actuator, or a null argument. */
int profile_cmd_init(struct profile_cmd_node *n, const struct profile_cmd_cfg *cfg);

/*
 * Move to a new epoch.
 *
 * This CLEARS last_seq and the stored acknowledgement, which is section 9 rule
 * 4 read forwards: a sequence number is only meaningful inside the epoch whose
 * key authenticated it, so carrying one across would be exactly the replay the
 * epoch exists to stop.
 */
int profile_cmd_set_epoch(struct profile_cmd_node *n, uint32_t epoch);

const struct profile_cmd_stats *profile_cmd_stats(const struct profile_cmd_node *n);

/* ---------------------------------------------------------------------------
 * The tag
 * ---------------------------------------------------------------------------
 */

/*
 * trunc(MAC(K_cmd, nonce || body[0..5])) at the configured width.
 *
 * `body` is a packed page 0x10 or 0x11 - the first six bytes are what is
 * covered, which is the page number, the sequence, the command or result, the
 * target, and the argument or value. The page number being INSIDE the covered
 * bytes is what separates a command's tag from an acknowledgement's without
 * spending a second domain byte on it.
 *
 * `tag` receives cfg.tag_len bytes. Returns 0, -EACCES when no root key was configured, or -ENOTSUP in a build without CONFIG_RADIANT_SEC - never OK with
 * an untouched buffer, because a caller that got OK would put stack contents on
 * the air and call it authentication.
 */
int profile_cmd_tag(const struct profile_cmd_node *n, const uint8_t *body,
		    uint8_t *tag);

/* Constant-time verify of a tag already on the wire. true only for a match. */
bool profile_cmd_tag_ok(const struct profile_cmd_node *n, const uint8_t *body,
			const uint8_t *tag);

/* ---------------------------------------------------------------------------
 * The state machine
 * ---------------------------------------------------------------------------
 */

/*
 * One received page 0x10, through section 9's four rules in section 9's order.
 *
 * `len` is the received body length - PROFILE_TLM_CMD_LEN_STD or _LEN_LR, and
 * it must match the configured width. `now` is the frame's t_sync, and it is
 * what the backoff is measured on.
 *
 * Returns the number of acknowledgement bytes written to `ack`, or 0 when the
 * node deliberately says nothing, or a negative errno for a malformed input.
 *
 * THE ZERO RETURN IS A DESIGN DECISION AND NOT AN OMISSION. A node that
 * answered every failed verification would be a tag oracle, and one that
 * answered a flood would spend its battery on the flood. So the FIRST failure
 * after any success is answered with result 0x03 - a legitimate receiver whose
 * key or epoch has gone stale needs to be told, and told once - and every
 * failure while the backoff is running is dropped in silence. Section 9 defines
 * the code; it does not require it to be emitted 65535 times.
 */
int profile_cmd_on_command(struct profile_cmd_node *n, const uint8_t *body,
			   uint8_t len, radiant_time_t now, uint8_t *ack,
			   size_t ack_cap);

/* ---------------------------------------------------------------------------
 * The response slot
 * ---------------------------------------------------------------------------
 */

/*
 * Adopt a downlink window and the grid it recurs on.
 *
 * `anchor` is any absolute instant at which the node will open one; every other
 * window is anchor + n * interval. `lr_phy` and `clk_ppm` are the descriptor's,
 * and are here because this function performs the check that
 * profile_sched_rephase() cannot: THE BLOCK IS VALIDATED AT THE WORST-CASE
 * PHASE, interval_counts - 1, not at the phase it happens to carry. That is
 * what makes every later re-phase safe, and it is why a node whose dwell is too
 * small for its clock is refused here, at configuration time, rather than
 * producing a downlink that silently never works.
 *
 * Returns 0, -ENOENT when the block announces no window, or whatever
 * profile_sched_check() refused it for.
 */
int profile_cmd_window_set(struct profile_cmd_node *n,
			   const struct profile_schedule *s,
			   radiant_time_t anchor, bool lr_phy, uint16_t clk_ppm);

/* Buy `windows` response slots. Additive; saturates rather than wraps. */
void profile_cmd_expect(struct profile_cmd_node *n, uint32_t windows);

/* The first window opening at or after `now`, or RADIANT_TIME_NEVER when the
 * node has no window. Independent of whether one is expected - it is where the
 * grid is, not whether the node will listen. */
radiant_time_t profile_cmd_next_window(const struct profile_cmd_node *n,
				       radiant_time_t now);

/*
 * Arm the next response slot as an ordinary bounded receive window.
 *
 * The window is [open, open + dwell] on the NODE's clock, with no guard added.
 * That is deliberate and it is the schedule block's own rule: the dwell was
 * already required to cover twice the clock drift accumulated over the phase,
 * and the commanding receiver is the party that aims at the middle. A guard
 * here would widen a window that was sized on the wire and make the announced
 * dwell a number nobody keeps.
 *
 * Returns 0 when a slot was armed, -EAGAIN when the node expects nothing (the
 * ordinary case, and not an error), -ENOENT when it has no window, or the
 * scheduler's own return code.
 */
int profile_cmd_arm_listen(struct profile_cmd_node *n, radiant_time_t now);

/*
 * The downlink hook, in profile_sched.h's shape.
 *
 * profile_cmd_sched_phase() is the callback and a struct profile_cmd_node * is
 * its user pointer, so installing it is:
 *
 *     struct profile_sched_downlink dl;
 *     profile_cmd_downlink(&node, &dl);
 *     profile_sched_set_downlink(&ps, &dl);
 *
 * and from then on every schedule frame the page scheduler hands out announces
 * a phase measured from that frame's own t_sync. That is the whole of the
 * closure of the interval-0 restriction profile_schedule.h recorded.
 */
int32_t profile_cmd_sched_phase(radiant_time_t t_sync, void *user);
void    profile_cmd_downlink(struct profile_cmd_node *n,
			     struct profile_sched_downlink *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_COMMAND_H_ */
