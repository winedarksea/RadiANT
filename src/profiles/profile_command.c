/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_command.c - response slots and the reliable-command state machine.
 *
 * Provenance: docs/radiant-telemetry.md section 9 and section 6's schedule
 * block, this project's own written specification, authored in advance of any
 * code. No adopter-gated ANT+ device profile document was read for this file,
 * no sdk-ant source was consulted, and nothing here derives from libant.a. See
 * docs/decisions/0002-clean-room-policy.md. The argument for every decision is
 * in profile_command.h.
 *
 * The order of on_command() is section 9's order and must stay that way: tag,
 * then duplicate, then window, then execute. Every rearrangement of those four
 * is a different protocol - checking the window before the tag, for instance,
 * turns the accept window into a free oracle for which sequence numbers are
 * live.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "profile_command.h"

/* ---------------------------------------------------------------------------
 * The control-channel convention
 * ---------------------------------------------------------------------------
 */

uint8_t profile_cmd_tag_bytes(enum radiant_frame_cfg cfg)
{
	return (cfg == RADIANT_FRAME_CFG_LR) ? (uint8_t)PROFILE_TLM_CMD_TAG_LR
					     : (uint8_t)PROFILE_TLM_CMD_TAG_STD;
}

int profile_cmd_channel_check(const struct profile_schedule *s,
			      enum radiant_frame_cfg cfg)
{
	if (s == NULL) {
		return -EINVAL;
	}

	/*
	 * The coding rate the node ANNOUNCES and the configuration the channel
	 * RUNS are two statements about one PHY. profile_sched_check() already
	 * requires the announcement to agree with frame 0's long-range bit; this
	 * is the third leg, against what the radio was actually handed, and it
	 * is the one no encoder can make on its own.
	 */
	if (cfg == RADIANT_FRAME_CFG_LR) {
		if (s->coding != PROFILE_CMD_CTRL_CODING) {
			return -EINVAL;
		}
	} else if (s->coding != PROFILE_SCHED_CODING_NONE) {
		return -EINVAL;
	}

	if (s->dl_interval == 0u) {
		return -ENOENT;
	}

	return 0;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

static bool tag_len_ok(uint8_t tag_len)
{
	return tag_len == (uint8_t)PROFILE_TLM_CMD_TAG_STD ||
	       tag_len == (uint8_t)PROFILE_TLM_CMD_TAG_LR;
}

int profile_cmd_init(struct profile_cmd_node *n, const struct profile_cmd_cfg *cfg)
{
	if (n == NULL || cfg == NULL) {
		return -EINVAL;
	}
	if (!tag_len_ok(cfg->tag_len) || cfg->execute == NULL) {
		return -EINVAL;
	}

	memset(n, 0, sizeof(*n));
	n->cfg = *cfg;
	return 0;
}

int profile_cmd_set_epoch(struct profile_cmd_node *n, uint32_t epoch)
{
	if (n == NULL) {
		return -EINVAL;
	}

	n->cfg.epoch = epoch;
	/* See the header: a sequence number is meaningless outside the epoch
	 * whose key authenticated it. */
	n->seq_known = false;
	n->stored_valid = false;
	n->last_seq = 0u;
	return 0;
}

const struct profile_cmd_stats *profile_cmd_stats(const struct profile_cmd_node *n)
{
	return (n == NULL) ? NULL : &n->stats;
}

/* ---------------------------------------------------------------------------
 * The tag
 * ---------------------------------------------------------------------------
 */

int profile_cmd_tag(const struct profile_cmd_node *n, const uint8_t *body,
		    uint8_t *tag)
{
#if defined(CONFIG_RADIANT_SEC)
	struct radiant_sec_cmac_ctx ctx;
	struct radiant_sec_key k_cmd;
	uint8_t nonce[RADIANT_SEC_BLOCK_BYTES];
	uint8_t full[RADIANT_SEC_BLOCK_BYTES];
	int rc;

	if (n == NULL || body == NULL || tag == NULL) {
		return -EINVAL;
	}
	if (n->cfg.k_root == NULL) {
		return -EACCES;
	}

	/*
	 * K_cmd is derived per call and destroyed before returning, which is
	 * what the compat layer's command tag does and for the same reason: a
	 * cached subkey is a key that outlives the epoch it was bound to, and
	 * the epoch is the entire replay defence here.
	 */
	rc = radiant_sec_kdf(n->cfg.k_root, RADIANT_SEC_LABEL_CMD, n->cfg.epoch,
			     n->cfg.devnum, &k_cmd);
	if (rc != RADIANT_SEC_OK) {
		return -EIO;
	}

	/* The sequence number rides the nonce's counter field as well as the
	 * covered bytes. It costs nothing and it means two pages differing only
	 * in their sequence never share a MAC block. */
	radiant_sec_nonce_block(nonce, n->cfg.epoch, n->cfg.devnum,
				(uint16_t)body[1], RADIANT_SEC_DOM_TLM_CMD);

	rc = radiant_sec_cmac_init(&ctx, &k_cmd);
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&ctx, nonce, sizeof(nonce));
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_update(&ctx, body, PROFILE_TLM_CMD_COVERED);
	}
	if (rc == RADIANT_SEC_OK) {
		rc = radiant_sec_cmac_final(&ctx, full);
	}
	radiant_sec_key_destroy(&k_cmd);
	if (rc != RADIANT_SEC_OK) {
		return -EIO;
	}

	/* Truncation is a prefix of the tag, matching every other truncated tag
	 * in this project. */
	memcpy(tag, full, n->cfg.tag_len);
	memset(full, 0, sizeof(full));
	return 0;
#else
	(void)n;
	(void)body;
	(void)tag;
	/* Never OK with an untouched buffer. See the header. */
	return -ENOTSUP;
#endif
}

bool profile_cmd_tag_ok(const struct profile_cmd_node *n, const uint8_t *body,
			const uint8_t *tag)
{
	uint8_t want[PROFILE_TLM_CMD_TAG_LR];
	uint8_t diff = 0u;
	uint8_t i;

	if (n == NULL || body == NULL || tag == NULL) {
		return false;
	}
	if (profile_cmd_tag(n, body, want) != 0) {
		return false;
	}

	/* Constant time over the configured width: an early return on the first
	 * differing byte leaks how much of a forged tag was right, which for a
	 * two-byte tag is most of the work. */
	for (i = 0u; i < n->cfg.tag_len; i++) {
		diff |= (uint8_t)(want[i] ^ tag[i]);
	}
	memset(want, 0, sizeof(want));
	return diff == 0u;
}

/* ---------------------------------------------------------------------------
 * The backoff
 * ---------------------------------------------------------------------------
 */

static void backoff_note_failure(struct profile_cmd_node *n, radiant_time_t now)
{
	uint32_t shift;

	if (n->fails < 0xFFu) {
		n->fails++;
	}
	shift = (uint32_t)(n->fails - 1u);
	if (shift > PROFILE_CMD_BACKOFF_SHIFT_MAX) {
		shift = PROFILE_CMD_BACKOFF_SHIFT_MAX;
	}
	n->blocked_until = now + (radiant_time_t)((uint64_t)PROFILE_CMD_BACKOFF_BASE_US
						  << shift);
}

static void backoff_clear(struct profile_cmd_node *n)
{
	n->fails = 0u;
	n->blocked_until = 0u;
}

/* ---------------------------------------------------------------------------
 * The state machine - section 9's four rules, in section 9's order
 * ---------------------------------------------------------------------------
 */

/* "delta_u8(seq, last_seq)" - unsigned difference on a counter that wraps at
 * 256, which is the only arithmetic under which a window near the wrap behaves
 * the same as a window anywhere else. */
static uint8_t delta_u8(uint8_t seq, uint8_t last)
{
	return (uint8_t)(seq - last);
}

static int emit_ack(struct profile_cmd_node *n,
		    const struct profile_command_ack *a, uint8_t *ack,
		    size_t ack_cap)
{
	uint8_t body[PROFILE_TLM_CMD_LEN_LR];
	uint8_t tag[PROFILE_TLM_CMD_TAG_LR];
	int rc;

	/* Covered bytes first, then the tag over them. The zero tag handed to
	 * the encoder here never reaches the air: it is overwritten below, and
	 * the six bytes it sits behind are the six the MAC covers. */
	memset(tag, 0, sizeof(tag));
	rc = profile_command_ack_encode_tag(a, tag, n->cfg.tag_len, body,
					    sizeof(body));
	if (rc < 0) {
		return rc;
	}

	if (profile_cmd_tag(n, body, tag) != 0) {
		return -EACCES;
	}
	rc = profile_command_ack_encode_tag(a, tag, n->cfg.tag_len, body,
					    sizeof(body));
	if (rc < 0) {
		return rc;
	}
	if (ack_cap < (size_t)rc) {
		return -EINVAL;
	}
	memcpy(ack, body, (size_t)rc);
	return rc;
}

int profile_cmd_on_command(struct profile_cmd_node *n, const uint8_t *body,
			   uint8_t len, radiant_time_t now, uint8_t *ack,
			   size_t ack_cap)
{
	struct profile_command c;
	struct profile_command_ack a;
	uint8_t tag[PROFILE_TLM_CMD_TAG_LR];
	uint8_t result;
	uint16_t value = 0u;
	int rc;

	if (n == NULL || body == NULL || ack == NULL) {
		return -EINVAL;
	}
	if (len != (uint8_t)(PROFILE_TLM_CMD_COVERED + n->cfg.tag_len)) {
		/* A page at the other width is not a malformed page, it is a
		 * page from a channel this node is not running. Refusing it
		 * here rather than verifying a truncated tag is what keeps the
		 * width a property of the channel. */
		return -EINVAL;
	}

	rc = profile_command_decode_tag(body, len, &c, tag, sizeof(tag));
	if (rc < 0) {
		return rc;
	}

	/* Counted before any verdict, because the question it answers is "did a
	 * response slot catch this", not "was it legitimate". windows minus
	 * window_hits is the miss rate, and a miss costs a full interval - which
	 * is the number the gate asks for. */
	if (n->armed_close != 0u && now >= n->armed_open && now <= n->armed_close) {
		n->stats.window_hits++;
	}

	/*
	 * The throttle sits in front of everything, including the verification
	 * it exists to limit. A node that verified first and then declined to
	 * answer would still be spending a CMAC per forged packet, which on a
	 * coin cell is the cost, not the airtime.
	 */
	if (n->fails != 0u && now < n->blocked_until) {
		n->stats.throttled++;
		return 0;
	}

	/* --- Rule 1: verify the tag first. ---------------------------------
	 * A failure changes no protocol state - not last_seq, not the stored
	 * acknowledgement. The backoff counter is not protocol state; it is the
	 * mitigation section 9 requires, and it is the one thing a failure is
	 * allowed to move.
	 */
	if (!profile_cmd_tag_ok(n, body, tag)) {
		bool first = (n->fails == 0u);

		n->stats.bad_tag++;
		backoff_note_failure(n, now);
		if (!first) {
			return 0;
		}

		memset(&a, 0, sizeof(a));
		a.seq = c.seq;
		a.result = PROFILE_TLM_RESULT_BAD_TAG;
		a.cmd = c.cmd;
		a.value = 0u;
		return emit_ack(n, &a, ack, ack_cap);
	}

	backoff_clear(n);

	/* --- Rule 2: a duplicate executes nothing. -------------------------
	 * Section 9 says "re-send the stored acknowledgement and do not execute
	 * again". This re-sends the stored CONTENT - the same command, the same
	 * resulting value - but reports result 0x01 where the stored result was
	 * 0x00.
	 *
	 * THAT IS A JUDGMENT CALL AND HERE IS THE ARGUMENT. The result table
	 * defines 0x01 as "accepted; already executed (idempotent repeat)", and
	 * a byte-identical resend would make 0x01 unreachable - a code in a
	 * normative table that no implementation can ever emit is a defect in
	 * one of the two. A REJECTION, by contrast, is repeated verbatim: a
	 * duplicate of a command that was refused for a bad argument is still a
	 * bad argument, and answering "already executed" would be a lie about an
	 * effect that never ran.
	 *
	 * The tag is recomputed either way, because the tag covers the result.
	 */
	if (n->seq_known && c.seq == n->last_seq && n->stored_valid) {
		a = n->stored;
		if (a.result == PROFILE_TLM_RESULT_OK) {
			a.result = PROFILE_TLM_RESULT_ALREADY;
		}
		n->stats.repeats++;
		profile_cmd_expect(n, PROFILE_CMD_RETRY_WINDOWS);
		return emit_ack(n, &a, ack, ack_cap);
	}

	/* --- Rules 3 and 4: the accept window, and adoption from nothing. --- */
	if (!n->seq_known) {
		/* Rule 4. The tag check above is what makes this safe: a command
		 * captured in a previous epoch does not reach here at all. */
		n->stats.adopted++;
	} else {
		uint8_t d = delta_u8(c.seq, n->last_seq);

		if (d == 0u || d > PROFILE_CMD_ACCEPT_WINDOW) {
			/* d == 0 with no stored acknowledgement: the sequence
			 * is current but the answer is gone, which is a node
			 * that rebooted mid-exchange. Outside the window is
			 * outside the window either way. */
			n->stats.bad_seq++;
			memset(&a, 0, sizeof(a));
			a.seq = c.seq;
			a.result = PROFILE_TLM_RESULT_BAD_SEQ;
			a.cmd = c.cmd;
			profile_cmd_expect(n, PROFILE_CMD_RETRY_WINDOWS);
			return emit_ack(n, &a, ack, ack_cap);
		}
	}

	result = n->cfg.execute(c.cmd, c.target, c.arg, &value, n->cfg.user);
	if (result == PROFILE_TLM_RESULT_OK) {
		n->stats.executed++;
	} else {
		n->stats.rejected++;
	}

	memset(&a, 0, sizeof(a));
	a.seq = c.seq;
	a.result = result;
	a.cmd = c.cmd;
	a.value = value;

	/*
	 * The sequence advances on any ANSWER, accepted or refused, because the
	 * answer is what the commanding receiver will retry for. Storing only
	 * successes would make a refused command retry forever against a node
	 * that has already decided.
	 */
	n->last_seq = c.seq;
	n->seq_known = true;
	n->stored = a;
	n->stored_valid = true;

	profile_cmd_expect(n, PROFILE_CMD_RETRY_WINDOWS);
	return emit_ack(n, &a, ack, ack_cap);
}

/* ---------------------------------------------------------------------------
 * The response slot
 * ---------------------------------------------------------------------------
 */

int profile_cmd_window_set(struct profile_cmd_node *n,
			   const struct profile_schedule *s,
			   radiant_time_t anchor, bool lr_phy, uint16_t clk_ppm)
{
	struct profile_schedule worst;
	uint32_t counts;
	int rc;

	if (n == NULL || s == NULL) {
		return -EINVAL;
	}
	counts = profile_sched_interval_counts(s);
	if (counts == 0u) {
		return -ENOENT;
	}

	/*
	 * THE WORST-CASE CHECK profile_sched_rephase() cannot make. The dwell
	 * must cover twice the drift accumulated over the phase, and a
	 * re-phasing node reaches every phase in the interval, so the block is
	 * checked at the largest of them. A node that passes here can be
	 * re-phased to anything; a node that fails here would have worked at
	 * the phase it was encoded with and failed later, at a time nobody was
	 * watching.
	 */
	worst = *s;
	worst.dl_phase = (uint16_t)(counts - 1u);
	rc = profile_sched_check(&worst, lr_phy, clk_ppm);
	if (rc != 0) {
		return rc;
	}

	n->sched = *s;
	n->anchor = anchor;
	n->have_window = true;
	return 0;
}

void profile_cmd_expect(struct profile_cmd_node *n, uint32_t windows)
{
	if (n == NULL || windows == 0u) {
		return;
	}
	if (n->expect_left > (UINT32_MAX - windows)) {
		n->expect_left = UINT32_MAX;
	} else {
		n->expect_left += windows;
	}
}

radiant_time_t profile_cmd_next_window(const struct profile_cmd_node *n,
				       radiant_time_t now)
{
	struct profile_schedule at;
	int32_t phase;

	if (n == NULL || !n->have_window) {
		return RADIANT_TIME_NEVER;
	}

	/*
	 * Reusing the announcing arithmetic rather than repeating it: the phase
	 * from `now` IS the offset to the next window, because that is what the
	 * announcement means. One implementation of "where is the grid" is one
	 * more than the number that can be right.
	 */
	phase = profile_sched_phase_for(&n->sched, n->anchor, now);
	if (phase < 0) {
		return RADIANT_TIME_NEVER;
	}

	/* The announcement, applied to itself: a block carrying that phase,
	 * heard at `now`, opens its first window exactly where this node will.
	 * Going through listen_at() rather than converting counts here is what
	 * keeps the node's grid and the receiver's the same arithmetic. */
	at = n->sched;
	at.dl_phase = (uint16_t)phase;
	return profile_sched_listen_at(&at, now, 0u);
}

int profile_cmd_arm_listen(struct profile_cmd_node *n, radiant_time_t now)
{
	struct radiant_sched_rx rx;
	radiant_time_t open_at;
	uint32_t dwell;
	int rc;

	if (n == NULL) {
		return -EINVAL;
	}
	if (!n->have_window) {
		return -ENOENT;
	}
	if (n->expect_left == 0u) {
		/* The ordinary case: nothing is expected, so nothing is armed
		 * and the radio stays off. */
		return -EAGAIN;
	}
	if (n->cfg.slot.fmt == NULL || n->cfg.slot.filters == NULL ||
	    n->cfg.slot.n_filters == 0u) {
		return -EINVAL;
	}

	open_at = profile_cmd_next_window(n, now);
	if (open_at == RADIANT_TIME_NEVER) {
		return -ENOENT;
	}
	dwell = profile_sched_dwell_us(n->sched.dl_dwell);
	if (dwell == 0u) {
		return -EINVAL;
	}

	memset(&rx, 0, sizeof(rx));
	rx.fmt = n->cfg.slot.fmt;
	rx.rf_index = n->cfg.slot.rf_index;
	rx.filters = n->cfg.slot.filters;
	rx.n_filters = n->cfg.slot.n_filters;
	rx.t_open = open_at;
	rx.t_close = open_at + (radiant_time_t)dwell;
	/* One command per window. A second frame inside the same dwell is
	 * either a duplicate, which rule 2 handles on the next window, or
	 * somebody else's traffic. */
	rx.stop_on_first = true;

	rc = radiant_sched_request_rx(n->cfg.slot.ch, &rx);
	if (rc != 0) {
		return rc;
	}

	n->expect_left--;
	n->armed_open = rx.t_open;
	n->armed_close = rx.t_close;
	n->stats.windows++;
	return 0;
}

int32_t profile_cmd_sched_phase(radiant_time_t t_sync, void *user)
{
	struct profile_cmd_node *n = (struct profile_cmd_node *)user;

	if (n == NULL || !n->have_window) {
		return -1;
	}
	return profile_sched_phase_for(&n->sched, n->anchor, t_sync);
}

void profile_cmd_downlink(struct profile_cmd_node *n,
			  struct profile_sched_downlink *out)
{
	if (out == NULL) {
		return;
	}
	out->phase = profile_cmd_sched_phase;
	out->user = n;
}
