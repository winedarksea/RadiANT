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
 * written specification. No ANT+ device profile document was read
 * for this file, no sdk-ant source was consulted, and nothing here derives from
 * libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this is, and what it is worth
 * ---------------------------------------------------------------------------
 * A command reaches a node only when it's listening - near its own transmit
 * slot today, so latency is bounded by the channel period, or by the
 * heartbeat (up to 60 s) for a sparse node.
 *
 * Not available to Zwift/FE-C: FE-C is a certified ANT+ profile and its
 * resistance command waits for the next FE-C slot regardless. This file is
 * for RadiANT-native control device types only.
 *
 * ---------------------------------------------------------------------------
 * RESPONSE SLOTS, NOT A SHORTER PERIOD
 * ---------------------------------------------------------------------------
 * Running the control channel faster would multiply duty across the 99% of
 * slots nothing is commanded in, on the shared RF 57 window. Instead the
 * period stays fixed and the node opens a downlink sub-slot at a FIXED
 * OFFSET inside it (the schedule block's phase/interval/dwell) - extra
 * receive current only, no extra airtime, worst case one interval instead of
 * one heartbeat.
 *
 * The interval is not the period: a node at a 2 s heartbeat can announce a
 * 1 s (or 1/32 s) downlink interval without touching what it transmits.
 * Latency and duty become independent knobs.
 *
 * ---------------------------------------------------------------------------
 * WHY THERE IS NO NEW SCHEDULER SLOT KIND
 * ---------------------------------------------------------------------------
 * radiant_sched.c has SLOT_RX, SLOT_TX, SLOT_ED. A response slot is not a
 * fourth kind: unlike SLOT_ED (no format/filter/open/close, sweeps a range
 * of RF indices), a response slot has a format, filter, rf_index, open and
 * close - it IS an ordinary bounded RX window, describable exactly through
 * radiant_sched_request_rx().
 *
 * A dedicated kind would touch ~20 sites (enum, arm function, preemption
 * rules, leader scan, expiry sweep, HAL callback, Kconfig, tests) and, worse,
 * would merge with nothing: radiant_sched.c's merge rules key off
 * `kind == SLOT_RX`, so posting a response slot as ordinary RX lets a
 * receiver commanding three overlapping nodes get one armed window instead
 * of three. So radiant_sched.c is unmodified - asserted by
 * radiant/tests/src/test_command.c (a node with no downlink window arms
 * the identical sequence it armed before this file existed).
 *
 * ---------------------------------------------------------------------------
 * THE NODE LISTENS ONLY WHEN IT EXPECTS A COMMAND
 * ---------------------------------------------------------------------------
 * An always-open window would run the receiver at full announced duty
 * forever. So arming is explicit and consumable: profile_cmd_expect() buys
 * windows, profile_cmd_arm_listen() spends one, and a node expecting nothing
 * arms nothing. A received command silently buys PROFILE_CMD_RETRY_WINDOWS
 * more, so a lost-ack retry lands in a window that's still open.
 *
 * ---------------------------------------------------------------------------
 * THE TAG, AND THE ONE THING THE LONG-RANGE PHY CHANGED
 * ---------------------------------------------------------------------------
 * Section 9 fixed the inline tag at 16 bits pending a measured way to put a
 * longer frame on the air; the LR PHY's RadiANT-authored length field is
 * that way, since nobody else can hear it. So width is a function of frame
 * config, not a node preference: profile_cmd_tag_bytes() returns 8 for
 * RADIANT_FRAME_CFG_LR, 2 otherwise. At 16 bits forgery succeeds with
 * probability 2^-16 and the backoff below is load-bearing; at 64 bits it's
 * 2^-64 and the backoff is belt and braces.
 *
 * ---------------------------------------------------------------------------
 * A CONTROL CHANNEL IS CODED S=8, AND THAT IS A CONFIGURATION RULE
 * ---------------------------------------------------------------------------
 * Not a latency fix (an S=8 frame is ~1.23 ms against a period in hundreds
 * of ms) - it buys link budget for range and, via the length field it
 * unlocks, the 64-bit tag. The two arrive together, which is why
 * profile_cmd_channel_check() checks them as one condition.
 */

#ifndef RADIANT_PROFILE_COMMAND_H_
#define RADIANT_PROFILE_COMMAND_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant/radiant_frame.h>
#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_sched.h>
#include <radiant/radiant_sec.h>

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
 * Refuse a control channel that is not one. `s` is the node's announced
 * schedule block, `cfg` the frame config the channel actually runs. Returns
 * 0, -EINVAL when they disagree, or -ENOENT when the block announces no
 * downlink window. A non-control channel never calls this - ordinary 1M
 * telemetry channels with a 16-bit tag remain legal per section 9; this is
 * an opt-in stricter setup, not a floor.
 */
int profile_cmd_channel_check(const struct profile_schedule *s,
			      enum radiant_frame_cfg cfg);

/* ---------------------------------------------------------------------------
 * The idempotency rule's constants - section 9, stated as numbers
 * ---------------------------------------------------------------------------
 */

/* "Accept if delta_u8(seq, last_seq) is in 1..64": wide enough to survive a
 * run of loss, narrow enough that a stale command can't walk back into range.
 */
#define PROFILE_CMD_ACCEPT_WINDOW 64u

/*
 * Backoff on failed verifications - the whole mitigation for a 16-bit tag.
 * The n-th consecutive failure blocks for 100 ms << min(n-1, 13): the first
 * 14 failures cost 1638 s total, every one after costs 819.2 s, capping a
 * flooder at ~105 attempts/day. Against a 2^-16 tag that's ~625 days to one
 * forged acceptance, vs. ~4.5 hours on an unthrottled 4 Hz channel
 * (~345,600 attempts/day).
 *
 * Accepted commands are not rate-limited: a brute-force attacker produces
 * 65535 rejections per acceptance, so throttling acceptances would throttle
 * 0.002% of the traffic. Throttle the failures instead.
 */
#define PROFILE_CMD_BACKOFF_BASE_US   100000u
#define PROFILE_CMD_BACKOFF_SHIFT_MAX 13u

/* Windows a received command buys, so a retry lands unasked. Two, not one,
 * because the lost ack may be lost again. */
#define PROFILE_CMD_RETRY_WINDOWS 2u

/* ---------------------------------------------------------------------------
 * The node
 * ---------------------------------------------------------------------------
 */

/* Counters: the "log rejections" half of section 9's mitigation - a forging
 * attacker should be a detectable flood, not a quiet success. */
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
 * The actuator, supplied by the device type. Returns a section 9 result
 * code: OK when the effect ran, or UNKNOWN_CMD / BAD_ARG / BUSY. `*value`
 * receives the target field's resulting value, carried in the ack.
 *
 * Called at most once per sequence number - that's this file's contract; the
 * implementation itself need not be idempotent.
 */
typedef uint8_t (*profile_cmd_exec_fn)(uint8_t cmd, uint8_t target, uint16_t arg,
				       uint16_t *value, void *user);

/* Where a response slot is placed on the air - the node's own channel in
 * every field; a downlink window must not become a second channel. */
struct profile_cmd_slot {
	uint8_t                          ch;
	const struct radiant_pkt_format *fmt;
	uint8_t                          rf_index;
	const struct radiant_rx_filter  *filters;
	uint8_t                          n_filters;
};

struct profile_cmd_cfg {
	/* Identity the tag is bound to. The epoch is why a command captured
	 * yesterday and replayed after a reboot fails verification instead
	 * of being adopted as a fresh sequence (rule 4). */
	uint32_t epoch;
	uint16_t devnum;

	/* K_cmd is DERIVED from this per epoch, never stored. NULL builds a
	 * node that verifies (rule 1) and therefore accepts nothing. */
	const struct radiant_sec_key *k_root;

	/* PROFILE_TLM_CMD_TAG_STD or _LR. Take it from profile_cmd_tag_bytes()
	 * rather than choosing it. */
	uint8_t tag_len;

	profile_cmd_exec_fn execute;
	void               *user;

	struct profile_cmd_slot slot;
};

struct profile_cmd_node {
	struct profile_cmd_cfg cfg;

	/* Section 9's idempotency state: last_seq and the result produced. */
	uint8_t                    last_seq;
	bool                       seq_known;
	struct profile_command_ack stored;
	bool                       stored_valid;

	/* The backoff. `fails` counts CONSECUTIVE failures, cleared by any
	 * success, so an unlucky-but-legitimate receiver never climbs it. */
	uint8_t        fails;
	radiant_time_t blocked_until;

	/* The response slot. `anchor` is any instant a window opens at; the
	 * grid is anchor + n * interval, so it never needs advancing. */
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

/* Move to a new epoch. Clears last_seq and the stored ack: a sequence number
 * is only meaningful inside the epoch whose key authenticated it. */
int profile_cmd_set_epoch(struct profile_cmd_node *n, uint32_t epoch);

const struct profile_cmd_stats *profile_cmd_stats(const struct profile_cmd_node *n);

/* ---------------------------------------------------------------------------
 * The tag
 * ---------------------------------------------------------------------------
 */

/*
 * trunc(MAC(K_cmd, nonce || body[0..5])) at the configured width. `body` is
 * a packed page 0x10 or 0x11; the covered six bytes include the page number,
 * which is what separates a command's tag from an acknowledgement's without
 * a second domain byte.
 *
 * `tag` receives cfg.tag_len bytes. Returns 0, -EACCES with no root key, or
 * -ENOTSUP without CONFIG_RADIANT_SEC - never OK with an untouched buffer.
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
 * One received page 0x10, through section 9's four rules in order.
 *
 * `len` is the received body length (must match the configured width).
 * `now` is the frame's t_sync, what the backoff is measured on.
 *
 * Returns ack bytes written, 0 when the node deliberately says nothing, or a
 * negative errno for malformed input. The zero return is deliberate: a node
 * that answered every failed verification would be a tag oracle, and one
 * that answered a flood would spend its battery on it. Only the FIRST
 * failure after a success gets result 0x03 (telling a legitimate receiver
 * once that its key/epoch went stale); failures during backoff are silent.
 */
int profile_cmd_on_command(struct profile_cmd_node *n, const uint8_t *body,
			   uint8_t len, radiant_time_t now, uint8_t *ack,
			   size_t ack_cap);

/* ---------------------------------------------------------------------------
 * The response slot
 * ---------------------------------------------------------------------------
 */

/*
 * Adopt a downlink window and the grid it recurs on. `anchor` is any instant
 * the node opens one at; every other window is anchor + n * interval.
 * `lr_phy`/`clk_ppm` let this function validate the block at the WORST-CASE
 * phase (interval_counts - 1) rather than the phase it happens to carry, so
 * every later re-phase is safe and a too-small dwell is refused here instead
 * of silently never working.
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
 * Arm the next response slot as an ordinary bounded receive window:
 * [open, open + dwell] on the NODE's clock, no guard added - the dwell was
 * already sized to cover twice the accumulated clock drift, and a guard
 * here would make the announced dwell a number nobody keeps.
 *
 * Returns 0 when armed, -EAGAIN when the node expects nothing (ordinary,
 * not an error), -ENOENT with no window, or the scheduler's return code.
 */
int profile_cmd_arm_listen(struct profile_cmd_node *n, radiant_time_t now);

/*
 * The downlink hook, in profile_sched.h's shape. Install with:
 *
 *     struct profile_sched_downlink dl;
 *     profile_cmd_downlink(&node, &dl);
 *     profile_sched_set_downlink(&ps, &dl);
 *
 * From then on every schedule frame announces a phase measured from that
 * frame's own t_sync.
 */
int32_t profile_cmd_sched_phase(radiant_time_t t_sync, void *user);
void    profile_cmd_downlink(struct profile_cmd_node *n,
			     struct profile_sched_downlink *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_COMMAND_H_ */
