/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_profile_command.h - the reliable-command path: response slots, the
 * idempotency state machine, and the inline tag at both of its widths.
 *
 * Provenance: docs/radiant-telemetry.md section 9 in full (the page 0x10 and
 * 0x11 layouts, the result-code table, the command vocabulary, the idempotency
 * rule stated as five numbered steps, and the honest-limit paragraph on a
 * 16-bit inline tag together with its 2026-08-11 amendment for the long-range
 * PHY) and section 6's schedule block (the downlink interval, phase and dwell
 * this file finally opens a window for). That document is this project's own
 * written specification. No ANT+ device profile document was read
 * for this file, no sdk-ant source was consulted, and nothing here derives from
 * libant.a. See docs/decisions/0002-clean-room-policy.md.
 *
 * What this is, and what it is worth: a command reaches a node only when
 * it's listening - near its own transmit slot today, so latency is bounded
 * by the channel period, or by the heartbeat (up to 60 s) for a sparse node.
 *
 * Not available to Zwift/FE-C: FE-C is a certified ANT+ profile and its
 * resistance command waits for the next FE-C slot regardless. This file is
 * for RadiANT-native control device types only.
 *
 * RESPONSE SLOTS, NOT A SHORTER PERIOD. Running the control channel faster
 * would multiply duty across the 99% of slots nothing is commanded in, on
 * the shared RF 57 window. Instead the period stays fixed and the node opens
 * a downlink sub-slot at a FIXED OFFSET inside it (the schedule block's
 * phase/interval/dwell) - extra receive current only, no extra airtime,
 * worst case one interval instead of one heartbeat.
 *
 * The interval is not the period: a node at a 2 s heartbeat can announce a
 * 1 s (or 1/32 s) downlink interval without touching what it transmits.
 * Latency and duty become independent knobs.
 *
 * WHY THERE IS NO NEW SCHEDULER SLOT KIND. radiant_sched.c has SLOT_RX,
 * SLOT_TX, SLOT_ED. A response slot is not a fourth kind: unlike SLOT_ED (no
 * format/filter/open/close, sweeps a range of RF indices), a response slot
 * has a format, filter, rf_index, open and close - it IS an ordinary bounded
 * RX window, describable exactly through radiant_sched_request_rx().
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
 * THE NODE LISTENS ONLY WHEN IT EXPECTS A COMMAND. An always-open window
 * would run the receiver at full announced duty forever. So arming is
 * explicit and consumable: radiant_profile_cmd_expect() buys windows,
 * radiant_profile_cmd_arm_listen() spends one, and a node expecting nothing arms
 * nothing. Any authenticated answer (accepted, duplicate or 0x02) leaves at
 * least RADIANT_PROFILE_CMD_RETRY_WINDOWS windows open, so a lost-ack retry lands in
 * a window that's still open.
 *
 * THE TAG, AND THE ONE THING THE LONG-RANGE PHY CHANGED. Section 9 fixed the
 * inline tag at 16 bits pending a measured way to put a longer frame on the
 * air; the LR PHY's RadiANT-authored length field is that way, since nobody
 * else can hear it. So width is a function of frame config, not a node
 * preference: radiant_profile_cmd_tag_bytes() returns 8 for RADIANT_FRAME_CFG_LR, 2
 * otherwise. At 16 bits forgery succeeds with probability 2^-16 and the
 * backoff below is load-bearing; at 64 bits it's 2^-64 and the backoff is
 * belt and braces.
 *
 * A CONTROL CHANNEL IS CODED S=8, AND THAT IS A CONFIGURATION RULE. Not a
 * latency fix (an S=8 frame is ~1.23 ms against a period in hundreds of ms)
 * - it buys link budget for range and, via the length field it unlocks, the
 * 64-bit tag. The two arrive together, which is why
 * radiant_profile_cmd_channel_check() checks them as one condition.
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

#include <radiant/radiant_profile_sched.h>
#include <radiant/radiant_profile_schedule.h>
#include <radiant/radiant_profile_telemetry.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The control-channel convention. */

/* What a RadiANT-native control device type is configured with. Named rather
 * than described, so that the first such type cites a constant instead of
 * repeating a sentence out of a plan. */
#define RADIANT_PROFILE_CMD_CTRL_CODING    RADIANT_PROFILE_SCHED_CODING_S8
#define RADIANT_PROFILE_CMD_CTRL_FRAME_CFG RADIANT_FRAME_CFG_LR

/* RADIANT_PROFILE_TLM_CMD_TAG_LR on the long-range configuration, RADIANT_PROFILE_TLM_CMD_TAG_STD
 * on every other. The whole of the width decision, in one function, so no
 * caller gets to make it twice. */
uint8_t radiant_profile_cmd_tag_bytes(enum radiant_frame_cfg cfg);

/*
 * Refuse a control channel that is not one. `s` is the node's announced
 * schedule block, `cfg` the frame config the channel actually runs. Returns
 * 0, -EINVAL when they disagree, or -ENOENT when the block announces no
 * downlink window. A non-control channel never calls this - ordinary 1M
 * telemetry channels with a 16-bit tag remain legal per section 9; this is
 * an opt-in stricter setup, not a floor.
 */
int radiant_profile_cmd_channel_check(const struct radiant_profile_schedule *s,
			      enum radiant_frame_cfg cfg);

/* The idempotency rule's constants - section 9, stated as numbers. */

/* "Accept if ext - last_ext is in 1..127", in extended-sequence space: as
 * far ahead as the eight-bit wire can say unambiguously, so a sender that
 * lost a long run of acks is still accepted. The window no longer stops a
 * replay - the tag does, because it binds the extended sequence and a command
 * from an earlier lap is rebuilt with the wrong one (RADIANT_PROFILE_CMD_EXT_LAST).
 */
#define RADIANT_PROFILE_CMD_ACCEPT_WINDOW 127u

/*
 * The extended sequence (security review 2026-11-20, item 3). The wire carries
 * eight bits; the tag binds sixteen. The receiver keeps last_ext and infers the
 * high byte from d = delta_u8(seq, last), split symmetrically:
 *
 *     d in 1..127     ahead:     ext = last_ext + d
 *     d == 0          duplicate: ext = last_ext
 *     d in 128..255   behind:    ext = last_ext - (256 - d), 1..128 behind
 *
 * except that a behind that would go below zero (last_ext < 256 - d) is read
 * as last_ext + d. That only happens on the first lap (256 - d <= 128), where
 * last_ext + d < 256 is the seq itself. So for every ext < 256 the tag is
 * byte-identical to the eight-bit tag it replaced.
 *
 * Outcomes: ahead is accepted; a duplicate answers the stored ack; behind
 * verifies only for a command that really was sent at that ext and is answered
 * 0x02 (authenticated, so the sender learns it is stale); anything else - an
 * earlier lap, or a sender 128 or more ahead - is rebuilt with the wrong ext
 * and fails rule 1 (0x03, or silence under backoff). Without this, a command
 * 192..255 behind the stream had its low byte back in the window and its
 * original tag still valid.
 *
 * RADIANT_PROFILE_CMD_EXT_LAST is the highest ext a node accepts: 0xFFFF - 127 =
 * 65408. Accepting it leaves last_ext + RADIANT_PROFILE_CMD_ACCEPT_WINDOW = 0xFFFF, so
 * the ahead inference never wraps the 16-bit counter and no ext is ever
 * reachable twice. Anything above (anything within 127 of 65536) is refused
 * with 0x02 and counted in seq_exhausted; the sender must advance the epoch.
 * That is 65409 accepted sequences per epoch (ext 0..65408), about 4.5 hours
 * of a command every 250 ms.
 */
#define RADIANT_PROFILE_CMD_EXT_LAST (0xFFFFu - RADIANT_PROFILE_CMD_ACCEPT_WINDOW)

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
#define RADIANT_PROFILE_CMD_BACKOFF_BASE_US   100000u
#define RADIANT_PROFILE_CMD_BACKOFF_SHIFT_MAX 13u

/* Windows left open after any authenticated answer, so a retry lands unasked.
 * Two, not one, because the lost ack may be lost again. The answer raises
 * expect_left to this count and never adds to it, so replays cannot bank
 * windows. */
#define RADIANT_PROFILE_CMD_RETRY_WINDOWS 2u

/* The node. */

/* Counters: the "log rejections" half of section 9's mitigation - a forging
 * attacker should be a detectable flood, not a quiet success. */
struct radiant_profile_cmd_stats {
	uint32_t executed;    /* commands whose effect actually ran */
	uint32_t repeats;     /* duplicates answered from the stored ack */
	uint32_t bad_tag;     /* failed verifications */
	uint32_t bad_seq;     /* outside the accept window */
	uint32_t rejected;    /* the actuator said no: unknown, bad arg, busy */
	uint32_t throttled;   /* dropped unverified while backed off */
	uint32_t adopted;     /* rule 4 fired: a sequence adopted from nothing */
	uint32_t windows;     /* response slots actually armed */
	uint32_t window_hits; /* response slots a command arrived in */
	uint32_t seq_exhausted; /* refused past RADIANT_PROFILE_CMD_EXT_LAST: epoch due */
};

/*
 * The actuator, supplied by the device type. Returns a section 9 result
 * code: OK when the effect ran, or UNKNOWN_CMD / BAD_ARG / BUSY. `*value`
 * receives the target field's resulting value, carried in the ack.
 *
 * Called at most once per sequence number - that's this file's contract; the
 * implementation itself need not be idempotent.
 */
typedef uint8_t (*radiant_profile_cmd_exec_fn)(uint8_t cmd, uint8_t target, uint16_t arg,
				       uint16_t *value, void *user);

/* Where a response slot is placed on the air - the node's own channel in
 * every field; a downlink window must not become a second channel. */
struct radiant_profile_cmd_slot {
	uint8_t                          ch;
	const struct radiant_pkt_format *fmt;
	uint8_t                          rf_index;
	const struct radiant_rx_filter  *filters;
	uint8_t                          n_filters;
};

struct radiant_profile_cmd_cfg {
	/* Identity the tag is bound to. The epoch is why a command captured
	 * yesterday and replayed after a reboot fails verification instead
	 * of being adopted as a fresh sequence (rule 4). Inside one epoch
	 * the extended sequence does that job (RADIANT_PROFILE_CMD_EXT_LAST). */
	uint32_t epoch;
	uint16_t devnum;

	/* K_cmd is DERIVED from this per epoch, never stored. NULL builds a
	 * node that verifies (rule 1) and therefore accepts nothing. */
	const struct radiant_sec_key *k_root;

	/* RADIANT_PROFILE_TLM_CMD_TAG_STD or _LR. Take it from radiant_profile_cmd_tag_bytes()
	 * rather than choosing it. */
	uint8_t tag_len;

	radiant_profile_cmd_exec_fn execute;
	void               *user;

	struct radiant_profile_cmd_slot slot;
};

struct radiant_profile_cmd_node {
	struct radiant_profile_cmd_cfg cfg;

	/* Section 9's idempotency state: the last sequence, EXTENDED to 16
	 * bits (its low byte is the wire's last_seq), and the result produced. */
	uint16_t                   last_ext;
	bool                       seq_known;
	struct radiant_profile_command_ack stored;
	bool                       stored_valid;

	/* The backoff. `fails` counts CONSECUTIVE failures, cleared by an
	 * accepted fresh command, so an unlucky-but-legitimate receiver never
	 * climbs it. A duplicate or a 0x02 verifies but does not clear it:
	 * both are replays anyone can record. */
	uint8_t        fails;
	radiant_time_t blocked_until;

	/* The response slot. `anchor` is any instant a window opens at; the
	 * grid is anchor + n * interval, so it never needs advancing. */
	struct radiant_profile_schedule sched;
	radiant_time_t          anchor;
	bool                    have_window;
	uint32_t                expect_left;
	radiant_time_t          armed_open;
	radiant_time_t          armed_close;

	struct radiant_profile_cmd_stats stats;
};

/* Latch the configuration. -EINVAL for a tag width that is neither of the two,
 * a missing actuator, or a null argument. */
int radiant_profile_cmd_init(struct radiant_profile_cmd_node *n, const struct radiant_profile_cmd_cfg *cfg);

/*
 * Move to a new epoch. A real advance clears last_ext and the stored ack: a
 * sequence number is only meaningful inside the epoch whose key authenticated
 * it. The current epoch again is a no-op that KEEPS the replay state, so a
 * retried setter cannot re-arm a command that already ran; an older epoch is
 * -EINVAL and changes nothing (no rollback). The epoch is a u32 that never
 * wraps (docs/radiant-security.md 3.5), so "older" is a plain comparison.
 *
 * After an advance, rule 4 adopts the first verified command with ext = seq:
 * a sender starts every epoch with the high byte at 0. A node reboot advances
 * the epoch (ADR 0009), so adoption only ever happens at an epoch's start.
 * A node that misses the whole first lap (no command with ext < 256 reaches
 * it) can never adopt: every later command is rebuilt with ext = seq and
 * fails the tag. The remedy is an epoch advance, after which the sender
 * starts again at 0.
 */
int radiant_profile_cmd_set_epoch(struct radiant_profile_cmd_node *n, uint32_t epoch);

const struct radiant_profile_cmd_stats *radiant_profile_cmd_stats(const struct radiant_profile_cmd_node *n);

/* The tag. */

/*
 * trunc(MAC(K_cmd, nonce || body[0..5])) at the configured width, where the
 * nonce's counter is `ext`, the EXTENDED sequence (RADIANT_PROFILE_CMD_EXT_LAST).
 * `body` is a packed page 0x10 or 0x11; the covered six bytes include the
 * page number, which is what separates a command's tag from an
 * acknowledgement's without a second domain byte.
 *
 * A sender keeps its own 16-bit counter per epoch, starting at 0, puts its low
 * byte in body[1] and tags with it here; it verifies the acknowledgement with
 * the same ext, so an acknowledgement from an earlier lap cannot answer it.
 *
 * `tag` receives cfg.tag_len bytes. Returns 0, -EINVAL when ext's low byte is
 * not body[1], -EACCES with no root key, or -ENOTSUP without
 * CONFIG_RADIANT_SEC - never OK with an untouched buffer.
 */
int radiant_profile_cmd_tag_ext(const struct radiant_profile_cmd_node *n, const uint8_t *body,
			uint16_t ext, uint8_t *tag);

/* Constant-time verify of a tag already on the wire, under `ext`. true only
 * for a match. */
bool radiant_profile_cmd_tag_ok_ext(const struct radiant_profile_cmd_node *n,
			    const uint8_t *body, uint16_t ext,
			    const uint8_t *tag);

/* The first lap only: ext = body[1]. Correct for an epoch's first 256
 * sequences, wrong (fails verification) for any later one. */
int radiant_profile_cmd_tag(const struct radiant_profile_cmd_node *n, const uint8_t *body,
		    uint8_t *tag);
bool radiant_profile_cmd_tag_ok(const struct radiant_profile_cmd_node *n, const uint8_t *body,
			const uint8_t *tag);

/* The state machine. */

/*
 * One received page 0x10, through section 9's five rules in order.
 *
 * `len` is the received body length (must match the configured width).
 * `now` is the frame's t_sync, what the backoff is measured on.
 *
 * Returns ack bytes written, 0 when the node deliberately says nothing, or a
 * negative errno for malformed input. The zero return is deliberate: a node
 * that answered every failed verification would be a tag oracle, and one
 * that answered a flood would spend its battery on it. Only the FIRST
 * failure after an accepted fresh command gets result 0x03 (telling a
 * legitimate receiver once that its key/epoch went stale); failures during
 * backoff are silent.
 */
int radiant_profile_cmd_on_command(struct radiant_profile_cmd_node *n, const uint8_t *body,
			   uint8_t len, radiant_time_t now, uint8_t *ack,
			   size_t ack_cap);

/* The response slot. */

/*
 * Adopt a downlink window and the grid it recurs on. `anchor` is any instant
 * the node opens one at; every other window is anchor + n * interval.
 * `lr_phy`/`clk_ppm` let this function validate the block at the WORST-CASE
 * phase (interval_counts - 1) rather than the phase it happens to carry, so
 * every later re-phase is safe and a too-small dwell is refused here instead
 * of silently never working.
 *
 * Returns 0, -ENOENT when the block announces no window, or whatever
 * radiant_profile_sched_check() refused it for.
 */
int radiant_profile_cmd_window_set(struct radiant_profile_cmd_node *n,
			   const struct radiant_profile_schedule *s,
			   radiant_time_t anchor, bool lr_phy, uint16_t clk_ppm);

/* Buy `windows` response slots. Additive; saturates rather than wraps. */
void radiant_profile_cmd_expect(struct radiant_profile_cmd_node *n, uint32_t windows);

/* The first window opening at or after `now`, or RADIANT_TIME_NEVER when the
 * node has no window. Independent of whether one is expected - it is where the
 * grid is, not whether the node will listen. */
radiant_time_t radiant_profile_cmd_next_window(const struct radiant_profile_cmd_node *n,
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
int radiant_profile_cmd_arm_listen(struct radiant_profile_cmd_node *n, radiant_time_t now);

/*
 * The downlink hook, in radiant_profile_sched.h's shape. Install with:
 *
 *     struct radiant_profile_sched_downlink dl;
 *     radiant_profile_cmd_downlink(&node, &dl);
 *     radiant_profile_sched_set_downlink(&ps, &dl);
 *
 * From then on every schedule frame announces a phase measured from that
 * frame's own t_sync.
 */
int32_t radiant_profile_cmd_sched_phase(radiant_time_t t_sync, void *user);
void    radiant_profile_cmd_downlink(struct radiant_profile_cmd_node *n,
			     struct radiant_profile_sched_downlink *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_COMMAND_H_ */
