/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_command.c - response slots and the reliable-command state machine.
 *
 * Provenance: docs/radiant-telemetry.md section 9 and section 6's schedule
 * block, this project's own written specification, authored in advance of any
 * code. No ANT+ device profile document was read for this file,
 * no sdk-ant source was consulted, and nothing here derives from libant.a. See
 * docs/decisions/0002-clean-room-policy.md. The argument for every decision is
 * in profile_command.h.
 *
 * The order of on_command() is section 9's order and must stay that way: tag,
 * then duplicate, then window, then execute. Checking the window before the
 * tag, for instance, turns the accept window into a free oracle for which
 * sequence numbers are live.
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

	/* Third leg (besides profile_sched_check()'s announcement check):
	 * announced coding vs. what the radio was actually handed. */
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

	/* K_cmd derived per call, destroyed before returning: a cached subkey
	 * would outlive the epoch it was bound to, and the epoch is the whole
	 * replay defence here. */
	rc = radiant_sec_kdf(n->cfg.k_root, RADIANT_SEC_LABEL_CMD, n->cfg.epoch,
			     n->cfg.devnum, &k_cmd);
	if (rc != RADIANT_SEC_OK) {
		return -EIO;
	}

	/* Sequence number rides the nonce too, so two pages differing only in
	 * sequence never share a MAC block. */
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

	/* Truncation is a prefix, matching every other truncated tag here. */
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

	/* Constant time over the configured width: an early return would leak
	 * how much of a forged tag was right. */
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

/* Unsigned difference on a counter that wraps at 256, so a window near the
 * wrap behaves the same as one anywhere else. */
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

	/* Zero tag lays out covered bytes first; overwritten below and never
	 * reaches the air. */
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
		/* Wrong width = wrong channel, not malformed; refuse rather
		 * than verify a truncated tag. */
		return -EINVAL;
	}

	rc = profile_command_decode_tag(body, len, &c, tag, sizeof(tag));
	if (rc < 0) {
		return rc;
	}

	/* Counted before any verdict: this answers "did a response slot catch
	 * this", not "was it legitimate". */
	if (n->armed_close != 0u && now >= n->armed_open && now <= n->armed_close) {
		n->stats.window_hits++;
	}

	/* Throttle sits in front of verification itself - verifying first and
	 * declining to answer would still spend a CMAC per forged packet; on
	 * a coin cell that's the cost, not airtime. */
	if (n->fails != 0u && now < n->blocked_until) {
		n->stats.throttled++;
		return 0;
	}

	/* --- Rule 1: verify the tag first. ---------------------------------
	 * A failure changes no protocol state (not last_seq, not the stored
	 * ack) - only the backoff counter moves.
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
	 * Re-sends the stored content but reports 0x01 (already executed)
	 * where the stored result was 0x00 (accepted) - otherwise 0x01 would
	 * be unreachable in the result table. A stored REJECTION is repeated
	 * verbatim instead: reporting "already executed" for an effect that
	 * never ran would be a lie. Tag is recomputed either way since it
	 * covers the result.
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
		/* Rule 4. Safe because the tag check above already excludes a
		 * command captured in a previous epoch. */
		n->stats.adopted++;
	} else {
		uint8_t d = delta_u8(c.seq, n->last_seq);

		if (d == 0u || d > PROFILE_CMD_ACCEPT_WINDOW) {
			/* d == 0 with no stored ack: node rebooted mid-
			 * exchange. Still outside the window either way. */
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

	/* Sequence advances on any answer, accepted or refused - storing only
	 * successes would make a refused command retry forever. */
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

	/* Worst-case check profile_sched_rephase() cannot make: dwell must
	 * cover twice the drift accumulated at the largest phase in the
	 * interval, so a node that passes here can be re-phased to anything. */
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

	/* Reuses the announcing arithmetic rather than repeating it: the phase
	 * from `now` IS the offset to the next window. */
	phase = profile_sched_phase_for(&n->sched, n->anchor, now);
	if (phase < 0) {
		return RADIANT_TIME_NEVER;
	}

	/* Going through listen_at() rather than converting counts here keeps
	 * the node's grid and the receiver's the same arithmetic. */
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
		/* Ordinary case: nothing expected, radio stays off. */
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
	/* One command per window; a second frame in the same dwell is either
	 * a duplicate (rule 2 handles it next window) or other traffic. */
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
