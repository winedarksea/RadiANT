/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_burst.c - the originator's stop-and-wait state machine, and the block
 * ownership that goes with it.
 *
 * Provenance: clean-room. Written from docs/spike-b-part2-results.md (the
 * one-bit sequence, the strict data/acknowledgement alternation, the 1567/1559
 * and 1546/1550 us intervals, and the fragmentation of a 24-byte host block
 * into three eight-byte on-air packets), from the buffer-ownership obligations
 * B1-B5 in src/ant_radio.h, and from radiant_core/include/radiant_core/radiant_radio_hal.h. Nothing
 * here derives from sdk-ant, from libant.a, from disassembly of any binary, or
 * from any ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * This file includes no Zephyr header and nothing from the application. The
 * gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I radiant_core/include -I radiant_core/tests -fsyntax-only radiant_core/src/radiant_burst.c
 *
 * The loop:
 *   submit(block)          -> arm packet 0                      TX_DATA
 *   tx event OK            -> arm the reply window, +1560 -/+250 WAIT_ACK
 *   the acknowledgement    -> was it the last packet?
 *                               yes: TX_COMPLETED, release the block   IDLE
 *                               no:  more packets in this block?
 *                                      yes: arm the next, +1550        TX_DATA
 *                                      no:  NEXT_BLOCK              WAIT_BLOCK
 *   window closed empty    -> TX_FAILED / NO_ACK. NOT retried.         IDLE
 *
 * Every arm after the first happens inside a completion callback, in radio
 * interrupt context - the only path that meets these intervals, because the
 * operation slot frees before the terminal callback runs so the next
 * operation can be armed from inside it.
 *
 * "Arm" means POST TO radiant_sched.c, not call the HAL. radiant_sched.c is the
 * single arming authority and the only place in radiant_core that calls
 * radiant_radio_tx()/rx(); see "One radio, one arming authority" in
 * radiant_transfer.h.
 *
 * B1-B5 (buffer ownership, see src/ant_radio.h) hold because: a block is
 * recorded as accepted only after the radio has taken its first packet (B2,
 * B5), and it is released exactly once, by whichever of release_next() or
 * finish() reaches it first, gated by t->block_released (B3, B4). Getting
 * this wrong doesn't crash - the bridge's k_sem_take() times out after
 * 1000 ms and answers ANTW_TRANSFER_IN_PROGRESS, so a two-second transfer
 * takes ten minutes with nothing in the log to explain why. Not catchable by
 * inspection, so radiant_core/tests/src/test_transfer.c counts released blocks
 * against accepted ones on every path (success, mid-transfer failure, abort,
 * rejected block).
 */

#include <string.h>

#include <radiant_core/radiant_transfer.h>

/* ---------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------------
 */

static void reset_transfer(struct radiant_transfer *t)
{
	t->state = RADIANT_TRANSFER_STATE_IDLE;
	t->op = 0u;
	t->block = NULL;
	t->block_len = 0u;
	t->block_seg = 0u;
	t->n_pkt_in_block = 0u;
	t->pkt_in_block = 0u;
	t->block_released = false;
	t->own_block = false;
	t->pkt_index = 0u;
	t->pkt_acked = 0u;
	t->cur_ctrl = 0u;
	t->next_t_sync = RADIANT_TIME_NEVER;
}

/* Hand the block back and ask for the next one. State moves to WAIT_BLOCK
 * *before* done() runs, because done() is where the bridge frees its
 * semaphore and a submit call can arrive re-entrantly from inside it. */
static void release_next(struct radiant_transfer *t)
{
	struct radiant_transfer_result r;
	const struct radiant_transfer_ops *ops = t->cfg.ops;
	void *ctx = t->cfg.ctx;

	if (!t->own_block || t->block_released || t->block == NULL) {
		return;
	}

	memset(&r, 0, sizeof(r));
	r.ev = RADIANT_TRANSFER_EV_NEXT_BLOCK;
	r.block = t->block;
	r.block_len = t->block_len;
	r.fail = RADIANT_TRANSFER_FAIL_NONE;
	r.packets = t->pkt_acked;

	t->block_released = true;
	/* Null it too, not just flag it: B1 says the buffer isn't valid after
	 * the releasing event, so nothing here should be readable by accident. */
	t->block = NULL;
	t->stats.blocks_released++;

	if (ops != NULL && ops->done != NULL) {
		ops->done(ctx, &r);
	}
}

/* End the transfer, exactly once, releasing whatever is still held. */
static void finish(struct radiant_transfer *t, enum radiant_transfer_event ev,
		   enum radiant_transfer_fail fail)
{
	struct radiant_transfer_result r;
	const struct radiant_transfer_ops *ops = t->cfg.ops;
	void *ctx = t->cfg.ctx;

	memset(&r, 0, sizeof(r));
	r.ev = ev;
	r.fail = fail;
	r.packets = t->pkt_acked;

	if (t->own_block && !t->block_released && t->block != NULL) {
		r.block = t->block;
		r.block_len = t->block_len;
		t->block_released = true;
		t->stats.blocks_released++;
	}

	if (ev == RADIANT_TRANSFER_EV_TX_COMPLETED) {
		t->stats.transfers_ok++;
	} else {
		t->stats.transfers_failed++;
	}

	/* Reset before the callback, for the same reason release_next() does. */
	reset_transfer(t);

	if (ops != NULL && ops->done != NULL) {
		ops->done(ctx, &r);
	}
}

/* ---------------------------------------------------------------------------
 * Arming
 * ---------------------------------------------------------------------------
 */

int radiant_transfer_arm_tx(struct radiant_transfer *t, uint8_t ctrl,
			const uint8_t *payload, uint8_t payload_len,
			radiant_time_t t_sync_at)
{
	struct radiant_sched_tx req;
	int n;
	int rc;

	if (t == NULL || payload == NULL || t->fmt == NULL) {
		return RADIANT_TRANSFER_EINVAL;
	}
	if (t_sync_at == RADIANT_TIME_NEVER) {
		return RADIANT_TRANSFER_EINVAL;
	}

	n = radiant_transfer_build_body(&t->cfg, ctrl, payload, payload_len,
				    t->tx_body, sizeof(t->tx_body));
	if (n < 0) {
		return n;
	}
	t->tx_body_len = (uint8_t)n;

	memset(&req, 0, sizeof(req));
	req.fmt = t->fmt;
	req.rf_index = t->cfg.rf_index;
	req.power = t->cfg.power;
	/* The same five bytes the reply window filters on, not a second
	 * derivation: radiant_transfer_init() already resolved them into
	 * t->filter, and recomputing via radiant_frame_addr() here would risk
	 * drift from that copy. */
	memcpy(req.addr, t->filter.addr, sizeof(req.addr));
	req.addr_len = t->filter.addr_len;
	/* t->tx_body, not a local: backends DMA out of it until the completion
	 * callback, and the mock's deep-copy at arm time wouldn't catch a stack
	 * buffer here - this has to be right by construction. */
	req.body = t->tx_body;
	req.body_len = t->tx_body_len;
	req.t_sync_at = t_sync_at;

	/* The scheduler, not the radio: this posts an intent against the
	 * engine's channel slot; the commit happens on the way out of the
	 * current radio callback or at the caller's next radiant_sched_tick(). */
	rc = radiant_sched_request_tx(t->cfg.channel, &req);
	if (rc != RADIANT_RADIO_OK_RC) {
		/* The scheduler's codes are the HAL's, deliberately - neither
		 * EINVAL (malformed) nor ESTATE (posted before init) is
		 * recoverable, so both become EIO and the transfer fails loudly. */
		return RADIANT_TRANSFER_EIO;
	}

	/* No HAL operation id to record: the scheduler merges several channels'
	 * requests into one hardware operation and routes each channel only its
	 * own events, so the sentinel below is what's true instead. */
	t->op = RADIANT_TRANSFER_OP_EXTERNAL;
	return RADIANT_TRANSFER_OK;
}

/* Open the window the acknowledgement is expected in, centred on the
 * measured 1560 us with a guard sized on the observed range rather than the
 * standard deviation. STOP_ON_FIRST because exactly one frame is expected
 * and it frees the operation slot immediately, letting the next data packet
 * arm from inside the same callback. */
static int arm_ack_window(struct radiant_transfer *t, radiant_time_t t_data_sync)
{
	struct radiant_sched_rx req;
	int rc;

	memset(&req, 0, sizeof(req));
	req.fmt = t->fmt;
	req.rf_index = t->cfg.rf_index;
	/* &t->filter, not a local: the HAL says the filter array must stay
	 * valid until the terminal event, and the scheduler copies it into the
	 * merged array only at the moment it arms. */
	/* &t->filter, not a local: must stay valid until the terminal event,
	 * since the scheduler copies it into the merged array only when it
	 * arms. */
	req.filters = &t->filter;
	req.n_filters = 1u;
	req.t_open = t_data_sync + (radiant_time_t)RADIANT_TRANSFER_REPLY_US -
		     (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US;
	req.t_close = t_data_sync + (radiant_time_t)RADIANT_TRANSFER_REPLY_US +
		      (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US;
	/* A request, not a guarantee: the HAL forbids STOP_ON_FIRST on a merged
	 * window, so a reply window sharing hardware with a tracked channel
	 * won't stop early. Costs receive current but doesn't change behaviour -
	 * the ack is recognised by its control byte, not by being alone. */
	req.stop_on_first = true;

	rc = radiant_sched_request_rx(t->cfg.channel, &req);
	if (rc != RADIANT_RADIO_OK_RC) {
		return RADIANT_TRANSFER_EIO;
	}

	t->op = RADIANT_TRANSFER_OP_EXTERNAL;
	return RADIANT_TRANSFER_OK;
}

/* Build and arm one data packet. Leaves the engine untouched on failure, so a
 * caller can decide whether that is a rejected submit or a dead transfer. */
static int try_send(struct radiant_transfer *t, uint32_t index, bool last,
		    const uint8_t *payload, radiant_time_t at)
{
	uint8_t ctrl;
	int rc;

	rc = radiant_transfer_ctrl(index, last, t->cfg.slot_opener, &ctrl);
	if (rc != RADIANT_TRANSFER_OK) {
		return rc;
	}
	rc = radiant_transfer_arm_tx(t, ctrl, payload, RADIANT_TRANSFER_PKT_BYTES, at);
	if (rc != RADIANT_TRANSFER_OK) {
		return rc;
	}

	t->cur_ctrl = ctrl;
	t->state = RADIANT_TRANSFER_STATE_TX_DATA;
	return RADIANT_TRANSFER_OK;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

int radiant_transfer_init(struct radiant_transfer *t, const struct radiant_transfer_cfg *cfg)
{
	const struct radiant_radio_caps *caps;
	int n;

	if (t == NULL || cfg == NULL || cfg->ops == NULL ||
	    cfg->ops->done == NULL) {
		return RADIANT_TRANSFER_EINVAL;
	}
	if (cfg->rf_index > RADIANT_RF_INDEX_MAX) {
		return RADIANT_TRANSFER_EINVAL;
	}
	/* The engine posts against this slot for the life of the channel, and
	 * radiant_sched_request_*() would answer RADIANT_RADIO_EINVAL for an
	 * out-of-range one on every packet rather than once, here. */
	if (cfg->channel >= (uint8_t)RADIANT_SCHED_MAX_CHANNELS) {
		return RADIANT_TRANSFER_EINVAL;
	}

	memset(t, 0, sizeof(*t));
	t->cfg = *cfg;

	t->fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	if (t->fmt == NULL) {
		return RADIANT_TRANSFER_EINVAL;
	}

	/* The peer answers on the channel's own address, so the window filters
	 * on exactly those five bytes; an on-air address match IS a channel-ID
	 * match, so no software comparison follows. */
	n = radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING, t->cfg.net_addr, &t->cfg.id,
			   t->filter.addr, sizeof(t->filter.addr));
	if (n < 0) {
		return RADIANT_TRANSFER_EINVAL;
	}
	t->filter.addr_len = (uint8_t)n;

	caps = radiant_radio_caps_get();
	if (caps == NULL) {
		return RADIANT_TRANSFER_ESTATE;
	}
	/* Refuse a backend that cannot acknowledge, loudly, at configuration
	 * time: the reply window must arm REPLY_US-GUARD ahead of the data
	 * packet's address, and the reply frame REPLY_US ahead of itself.
	 * Acknowledged data is how a trainer's resistance gets set, so failing
	 * here beats discovering "ERG mode does not work" months downstream.
	 *
	 * min_arm_lead_in_grant_us, NOT min_arm_lead_us: this path arms from
	 * inside the data packet's own completion callback, with the reply's
	 * air already reserved (follow_on_us), so what must fit in REPLY_US is
	 * just the peripheral programming time. min_arm_lead_us is the wrong
	 * number once a backend has to ask an arbiter for air - on MPSL it's
	 * ~2500 us against this check's 1310 us ceiling, so using it would
	 * refuse acknowledged data (ERG mode) on every arbitrated build. 0 means
	 * "same as min_arm_lead_us", the value every backend reported before
	 * this field existed. */
	{
		uint32_t lead = caps->min_arm_lead_in_grant_us != 0u
					? caps->min_arm_lead_in_grant_us
					: caps->min_arm_lead_us;

		if (lead + (uint32_t)RADIANT_TRANSFER_ACK_GUARD_US >
		    (uint32_t)RADIANT_TRANSFER_REPLY_US) {
			return RADIANT_TRANSFER_ENOTSUP;
		}
		if ((uint32_t)caps->rx_to_tx_us >
		    (uint32_t)RADIANT_TRANSFER_REPLY_US) {
			return RADIANT_TRANSFER_ENOTSUP;
		}
		t->min_lead_us = (uint16_t)lead;
	}

	reset_transfer(t);
	t->ready = true;
	return RADIANT_TRANSFER_OK;
}

/* ---------------------------------------------------------------------------
 * Submitting a block
 * ---------------------------------------------------------------------------
 */

int radiant_transfer_submit(struct radiant_transfer *t, uint8_t *block, uint8_t len,
			uint8_t segment, bool owned, radiant_time_t t_sync_at)
{
	uint8_t n_pkts;
	uint32_t index;
	bool last;
	radiant_time_t when;
	bool starting;
	int rc;

	if (t == NULL || block == NULL) {
		return RADIANT_TRANSFER_EINVAL;
	}
	if (!t->ready) {
		return RADIANT_TRANSFER_ESTATE;
	}
	if ((segment & (uint8_t)~RADIANT_TRANSFER_SEG_MASK) != 0u) {
		return RADIANT_TRANSFER_EINVAL;
	}

	n_pkts = radiant_transfer_packets_in_block(len);
	if (n_pkts == 0u) {
		/* Not 8, 16 or 24. A dongle that accepts a block size it can
		 * never fragment cleanly is worse than one that refuses it. */
		return RADIANT_TRANSFER_EINVAL;
	}

	if (t->state == RADIANT_TRANSFER_STATE_IDLE) {
		if ((segment & RADIANT_TRANSFER_SEG_START) == 0u) {
			return RADIANT_TRANSFER_ESEQ;
		}
		starting = true;
		index = 0u;
	} else if (t->state == RADIANT_TRANSFER_STATE_WAIT_BLOCK) {
		if ((segment & RADIANT_TRANSFER_SEG_START) != 0u) {
			return RADIANT_TRANSFER_ESEQ;
		}
		if (owned != t->own_block) {
			/* A burst changing its mind about ownership mid-transfer
			 * is a caller bug, not a state to model. */
			return RADIANT_TRANSFER_EINVAL;
		}
		starting = false;
		index = t->pkt_index;
	} else {
		/* Mid-packet. The host must wait for NEXT_BLOCK; this is the
		 * ANTW_TRANSFER_IN_PROGRESS the bridge already expects. */
		return RADIANT_TRANSFER_EBUSY;
	}

	/* Only the final packet of an END block is the last of the transfer. */
	last = ((segment & RADIANT_TRANSFER_SEG_END) != 0u) && (n_pkts == 1u);

	if (t_sync_at != RADIANT_TIME_NEVER) {
		when = t_sync_at;
	} else if (!starting && t->next_t_sync != RADIANT_TIME_NEVER) {
		/* The continuation instant the acknowledgement already fixed:
		 * ack t_sync + 1550 us. */
		when = t->next_t_sync;
	} else {
		when = radiant_radio_now() + (radiant_time_t)t->min_lead_us +
		       (radiant_time_t)RADIANT_TRANSFER_ARM_SLACK_US;
	}
	/* A continuation whose scheduled instant already passed (host was
	 * slower than one turnaround) is sent as soon as possible rather than
	 * failed. Unmeasured: the spike only saw the *receiver* retransmit,
	 * never a late host block, so this choice (vs. failing the transfer) is
	 * unevidenced either way. */
	if (!starting) {
		radiant_time_t earliest = radiant_radio_now() +
				      (radiant_time_t)t->min_lead_us +
				      (radiant_time_t)RADIANT_TRANSFER_ARM_SLACK_US;

		if (when < earliest) {
			when = earliest;
		}
	}

	/* Arm BEFORE recording the block as accepted (B2: on failure the backend
	 * owns nothing and raises no events). Doing it the other way - install,
	 * arm, roll back on failure - is how B5 gets violated: a released block
	 * frees a semaphore the bridge still holds for a different one, and the
	 * next host packet overwrites a buffer the radio is transmitting from. */
	rc = try_send(t, index, last, block, when);
	if (rc != RADIANT_TRANSFER_OK) {
		return rc;
	}

	if (starting) {
		t->pkt_index = 0u;
		t->pkt_acked = 0u;
		t->own_block = owned;
	}
	t->block = block;
	t->block_len = len;
	t->block_seg = segment;
	t->n_pkt_in_block = n_pkts;
	t->pkt_in_block = 0u;
	t->block_released = false;
	t->stats.blocks_accepted++;

	return RADIANT_TRANSFER_OK;
}

int radiant_transfer_abort(struct radiant_transfer *t)
{
	if (t == NULL) {
		return RADIANT_TRANSFER_EINVAL;
	}
	if (!t->ready) {
		return RADIANT_TRANSFER_ESTATE;
	}

	switch (t->state) {
	case RADIANT_TRANSFER_STATE_IDLE:
		return RADIANT_TRANSFER_OK;
	case RADIANT_TRANSFER_STATE_WAIT_BLOCK:
		/* Nothing is armed; there is no terminal event to wait for. */
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_ABORTED);
		return RADIANT_TRANSFER_OK;
	case RADIANT_TRANSFER_STATE_ABORTING:
		return RADIANT_TRANSFER_OK;
	default:
		break;
	}

	/* The cancelled operation's terminal event still arrives (HAL guarantee,
	 * confirmed by the mock with no fault injection), so this marks the
	 * engine ABORTING rather than finishing now; the event handler below
	 * finishes it when the terminal lands. From thread context
	 * radiant_radio_abort() delivers it before returning; from inside a
	 * callback it's deferred until the current one returns.
	 *
	 * radiant_radio_abort(), NOT radiant_sched_cancel(): the latter is
	 * documented SILENT for the cancelled channel, so no completion would
	 * ever end the ABORTING wait. The cost: if the aborted operation was a
	 * merged receive window, other channels sharing it lose the rest of
	 * their window (RADIANT_SCHED_DONE_ABORTED) - loud and recoverable,
	 * unlike an engine stuck in ABORTING. A proper fix needs a "cancel and
	 * tell me" entry point in radiant_sched.h that this change didn't add. */
	t->state = RADIANT_TRANSFER_STATE_ABORTING;
	(void)radiant_radio_abort();

	return RADIANT_TRANSFER_OK;
}

/* ---------------------------------------------------------------------------
 * Radio events
 * ---------------------------------------------------------------------------
 */

/* Rebuild the received frame. The matched address bytes never reach RAM (a
 * hardware address match means exactly that), so the address half is
 * regenerated from the channel ID this window filtered on.
 *
 * RADIANT_FRAME_TRUSTED_CRC is right for every delivered event, not just
 * crc_in_hw backends: status OK means the CRC was verified, in hardware or
 * software, and the received CRC bytes aren't available here either way. */
static bool decode_rx(const struct radiant_transfer *t,
		      const struct radiant_rx_event *e, struct radiant_frame *out)
{
	struct radiant_frame_wire w;
	int n;

	if (e->body == NULL || e->body_len == 0u ||
	    (size_t)e->body_len > sizeof(w.body)) {
		return false;
	}

	memset(&w, 0, sizeof(w));
	n = radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING, t->cfg.net_addr, &t->cfg.id,
			   w.addr, sizeof(w.addr));
	if (n < 0) {
		return false;
	}
	w.addr_len = (uint8_t)n;
	memcpy(w.body, e->body, e->body_len);
	w.body_len = e->body_len;

	return radiant_frame_decode(RADIANT_FRAME_CFG_TRACKING, &w,
				RADIANT_FRAME_TRUSTED_CRC, out) == RADIANT_FRAME_OK;
}

/* The acknowledgement arrived and matched. Move on. */
static void advance(struct radiant_transfer *t, radiant_time_t ack_t_sync)
{
	int rc;
	bool last;

	t->pkt_acked++;
	t->stats.packets_sent++;
	t->pkt_index++;
	t->pkt_in_block++;

	if (t->pkt_in_block >= t->n_pkt_in_block) {
		/* The block is spent. State first, then release: done() frees
		 * the bridge's buffer and a re-entrant submit can follow. */
		t->state = RADIANT_TRANSFER_STATE_WAIT_BLOCK;
		t->next_t_sync = ack_t_sync +
				 (radiant_time_t)RADIANT_TRANSFER_NEXT_PACKET_US;
		release_next(t);
		return;
	}

	/* Another packet from a fragmented 16- or 24-byte advanced-burst block.
	 * The sequence bit counts on-air packets, not host blocks. */
	last = ((t->block_seg & RADIANT_TRANSFER_SEG_END) != 0u) &&
	       (t->pkt_in_block + 1u == t->n_pkt_in_block);

	rc = try_send(t, t->pkt_index, last,
		      &t->block[(size_t)t->pkt_in_block * RADIANT_TRANSFER_PKT_BYTES],
		      ack_t_sync + (radiant_time_t)RADIANT_TRANSFER_NEXT_PACKET_US);
	if (rc != RADIANT_TRANSFER_OK) {
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_RADIO);
	}
}

void radiant_transfer_on_tx_event(struct radiant_transfer *t,
			      const struct radiant_tx_event *e)
{
	if (t == NULL || e == NULL || !t->ready) {
		return;
	}
	/* RADIANT_TRANSFER_OP_EXTERNAL means the scheduler has already routed
	 * this event to its owner and there's no HAL id to check; any other
	 * non-zero value is a real id, still checked against a stale or
	 * someone-else's operation. */
	if (t->op == 0u ||
	    (t->op != RADIANT_TRANSFER_OP_EXTERNAL && e->op != t->op)) {
		t->stats.late_events++;
		return;
	}

	if (t->state == RADIANT_TRANSFER_STATE_ABORTING) {
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_ABORTED);
		return;
	}

	if (t->state == RADIANT_TRANSFER_STATE_TX_REPLY) {
		/* Our acknowledgement of somebody else's data packet. Nothing
		 * follows it. A retransmission timer, if wired, belongs in
		 * api_sched_done(), not radiant_sched.c (which owns no timer -
		 * see gap 1 in radiant_transfer.h).
		 *
		 * reply_ctrl/reply_payload/reply_attempts deliberately survive
		 * this transition since retransmission is defined on an idle
		 * engine - nothing clears them here because this path skips
		 * finish()/reset_transfer(). That means a stale ack could go out
		 * on the next idle window; the staleness check in
		 * radiant_transfer_reply_retransmit() (against the t_sync stamped
		 * below) is what prevents it. */
		t->op = 0u;
		t->state = RADIANT_TRANSFER_STATE_IDLE;
		t->reply_t_sync = e->t_sync;
		if (e->status == RADIANT_RADIO_STATUS_OK) {
			t->stats.acks_sent++;
		}
		return;
	}

	if (t->state != RADIANT_TRANSFER_STATE_TX_DATA) {
		t->stats.late_events++;
		return;
	}

	if (e->status != RADIANT_RADIO_STATUS_OK) {
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_RADIO);
		return;
	}

	/* Anchored on the t_sync the backend actually achieved, not the one
	 * requested - on a backend that can't schedule exactly, anchoring on
	 * the request would put the window after an instant that never
	 * happened. */
	if (arm_ack_window(t, e->t_sync) != RADIANT_TRANSFER_OK) {
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_RADIO);
		return;
	}
	t->state = RADIANT_TRANSFER_STATE_WAIT_ACK;
}

void radiant_transfer_on_rx_event(struct radiant_transfer *t,
			      const struct radiant_rx_event *e)
{
	struct radiant_frame f;

	if (t == NULL || e == NULL || !t->ready) {
		return;
	}
	/* See the note on the same test in radiant_transfer_on_tx_event(). */
	if (t->op == 0u ||
	    (t->op != RADIANT_TRANSFER_OP_EXTERNAL && e->op != t->op)) {
		t->stats.late_events++;
		return;
	}

	if (t->state == RADIANT_TRANSFER_STATE_ABORTING) {
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_ABORTED);
		return;
	}
	if (t->state != RADIANT_TRANSFER_STATE_WAIT_ACK) {
		t->stats.late_events++;
		return;
	}

	switch (e->status) {
	case RADIANT_RADIO_STATUS_OK:
		break;
	case RADIANT_RADIO_STATUS_CRC_FAIL:
		/* Non-terminal, and only delivered to a window that asked for
		 * it - which this one does not. The window is still open, so
		 * there is nothing to do but wait. */
		return;
	case RADIANT_RADIO_STATUS_TIMEOUT:
		/* The reply window closed empty. NOT RETRIED: what a sender does
		 * when an ack goes missing was never measured (untested item 5,
		 * docs/spike-b-part2-results.md) - only the receiver's retry
		 * behaviour was, and that's implemented in radiant_ack.c.
		 * Inventing a sender-side retry here would be unevidenced. */
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_NO_ACK);
		return;
	default:
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_RADIO);
		return;
	}

	/* RADIANT_RX_STOP_ON_FIRST makes this event terminal, so the operation slot
	 * is already free and the next packet can be armed from right here. */
	if (!decode_rx(t, e, &f)) {
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_BAD_ACK);
		return;
	}
	if (!radiant_transfer_ack_matches(t->cur_ctrl, f.ctrl_byte)) {
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_BAD_ACK);
		return;
	}

	if (radiant_ctrl_is_last(t->cur_ctrl)) {
		/* 0xE2/0xF2: the transfer is complete. The END block is
		 * released by this event and no other (B4). */
		t->pkt_acked++;
		t->stats.packets_sent++;
		finish(t, RADIANT_TRANSFER_EV_TX_COMPLETED, RADIANT_TRANSFER_FAIL_NONE);
		return;
	}

	advance(t, e->t_sync);
}

/* ---------------------------------------------------------------------------
 * Inspection
 * ---------------------------------------------------------------------------
 */

enum radiant_transfer_state radiant_transfer_state(const struct radiant_transfer *t)
{
	return (t == NULL) ? RADIANT_TRANSFER_STATE_IDLE : t->state;
}

uint32_t radiant_transfer_op(const struct radiant_transfer *t)
{
	return (t == NULL) ? 0u : t->op;
}

bool radiant_transfer_is_idle(const struct radiant_transfer *t)
{
	return (t == NULL) || (t->state == RADIANT_TRANSFER_STATE_IDLE);
}

const struct radiant_transfer_stats *radiant_transfer_stats(const struct radiant_transfer *t)
{
	static const struct radiant_transfer_stats none;

	return (t == NULL) ? &none : &t->stats;
}
