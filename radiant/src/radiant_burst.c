/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_burst.c - the originator's stop-and-wait state machine, and the block
 * ownership that goes with it.
 *
 * Provenance: clean-room. Written from docs/spike-b-results.md (the
 * one-bit sequence, the strict data/acknowledgement alternation, the 1567/1559
 * and 1546/1550 us intervals, and the fragmentation of a 24-byte host block
 * into three eight-byte on-air packets), from the buffer-ownership obligations
 * B1-B5 in src/ant_radio.h, and from radiant/include/radiant/radiant_radio_hal.h. Nothing
 * here derives from sdk-ant, from libant.a, from disassembly of any binary, or
 * from any ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * This file includes no Zephyr header and nothing from the application. The
 * gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I radiant/include -I radiant/tests -fsyntax-only radiant/src/radiant_burst.c
 *
 * The loop:
 *   submit(block)          -> arm packet 0                      TX_DATA
 *   tx event OK            -> arm the reply window, +1560 -/+250 WAIT_ACK
 *   the acknowledgement    -> was it the last packet?
 *                               yes: TX_COMPLETED, release the block   IDLE
 *                               no:  more packets in this block?
 *                                      yes: arm the next, +1550        TX_DATA
 *                                      no:  NEXT_BLOCK              WAIT_BLOCK
 *   window closed empty    -> multi-packet burst, retries left:
 *                               re-arm the same packet, same ctrl   TX_DATA
 *                             otherwise TX_FAILED / NO_ACK          IDLE
 *
 * And the acknowledger's half, for an inbound burst (radiant_ack.c arms the
 * acknowledgement; this file follows the burst):
 *   on_data(non-LAST)      -> arm the ACK at +1560                  TX_REPLY
 *   ACK sent, burst open   -> window at ack + 1550 -/+250           WAIT_NEXT
 *   next packet            -> on_data(): deliver, ACK               TX_REPLY
 *   same sequence bit      -> on_data(): re-ACK, don't deliver      TX_REPLY
 *   window closed empty    -> after LAST: done                      IDLE
 *                             else repeat the ACK at +3143          TX_REPLY
 *                             (21 attempts, then rx_failed)         IDLE
 *   broadcast in window    -> rx_failed unless LAST was acked; the
 *                             frame goes back to the owner          IDLE
 * Acknowledged data (LAST with no burst open) goes TX_REPLY -> IDLE, as it
 * always did.
 *
 * Every arm after the first happens inside a completion callback, in radio
 * interrupt context - the only path that meets these intervals, because the
 * operation slot frees before the terminal callback runs so the next
 * operation can be armed from inside it.
 *
 * "Arm" means POST TO radiant_sched.c, not call the HAL. radiant_sched.c is the
 * single arming authority and the only place in radiant that calls
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
 * inspection, so radiant/tests/src/test_transfer.c counts released blocks
 * against accepted ones on every path (success, mid-transfer failure, abort,
 * rejected block).
 */

#include <string.h>

#include <radiant/radiant_transfer.h>
#include <radiant/radiant_event.h>
#include <radiant/radiant_diag.h>

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
	t->staged = NULL;
	t->staged_len = 0u;
	t->staged_seg = 0u;
	t->pkt_index = 0u;
	t->pkt_acked = 0u;
	t->cur_ctrl = 0u;
	t->next_t_sync = RADIANT_TIME_NEVER;
	t->data_t_sync = RADIANT_TIME_NEVER;
	t->retries = 0u;
}

/* Hand the block back and ask for the next one. State moves to WAIT_BLOCK
 * *before* done() runs, because done() is where the bridge frees its
 * semaphore and a submit call can arrive re-entrantly from inside it. */
static void release_block(struct radiant_transfer *t, uint8_t *block,
			  uint8_t len)
{
	struct radiant_transfer_result r;
	const struct radiant_transfer_ops *ops = t->cfg.ops;
	void *ctx = t->cfg.ctx;

	memset(&r, 0, sizeof(r));
	r.ev = RADIANT_TRANSFER_EV_NEXT_BLOCK;
	r.block = block;
	r.block_len = len;
	r.fail = RADIANT_TRANSFER_FAIL_NONE;
	r.packets = t->pkt_acked;
	t->stats.blocks_released++;

	if (ops != NULL && ops->done != NULL) {
		ops->done(ctx, &r);
	}
}

static void release_next(struct radiant_transfer *t)
{
	uint8_t *block = t->block;

	if (!t->own_block || t->block_released || block == NULL) {
		return;
	}
	t->block_released = true;
	/* Null it too, not just flag it: B1 says the buffer isn't valid after
	 * the releasing event, so nothing here should be readable by accident. */
	t->block = NULL;
	release_block(t, block, t->block_len);
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
	/* A block staged behind the one in flight goes back with the same
	 * terminal: one event ends the transfer, and each pointer is still
	 * carried by exactly one done() call. */
	if (t->staged != NULL) {
		r.staged_block = t->staged;
		r.staged_len = t->staged_len;
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

/*
 * Does this transmit's acknowledgement come on the slave's SLOT-reply
 * turnaround (~2190 us) rather than the burst one (~1560)? Only the first
 * attempt of a slot opener. A repeat is answered at the burst turnaround by a
 * receiver that heard the first attempt and joined the burst: every repeated
 * 0x8A in the 2026-11-11 A/B (A1 and A2, eleven) was acknowledged at
 * +1534..+1584 us, none near 2190. A receiver that missed the first attempt
 * is still tracking and hears no repeat 3.7 ms later at all, so the wide
 * window bought nothing on a retry but ~630 us of held air.
 */
static bool answered_on_slot_turnaround(uint8_t ctrl, bool repeat)
{
	return radiant_ctrl_is_slot_opener(ctrl) && !repeat;
}

static int arm_tx(struct radiant_transfer *t, uint8_t ctrl,
		  const uint8_t *payload, uint8_t payload_len,
		  radiant_time_t t_sync_at, bool repeat)
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
	/* An originated data packet must retain air for its ACK receive window.
	 * The reserve also permits in-grant arming on arbitrated backends. An
	 * acknowledgement (ours, or a retransmission of it) ends the exchange
	 * and completes to idle, so it reserves nothing: an arbiter would
	 * otherwise hold ~2 ms of the other stack's air per reply for a window
	 * that is never opened. The control byte decides, since it is what
	 * says on the air whether anything answers. */
	if (!radiant_ctrl_is_ack(ctrl)) {
		/* A slot opener's first window runs out to the slave's
		 * slot-reply turnaround (arm_ack_window()), so its reserve does
		 * too; a repeat's does not. */
		req.follow_on_us = (answered_on_slot_turnaround(ctrl, repeat) ?
				    RADIANT_TRANSFER_OPENER_REPLY_US :
				    RADIANT_TRANSFER_REPLY_US) +
			RADIANT_TRANSFER_ACK_GUARD_US + radiant_frame_tail_us(t->fmt);
	}

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

int radiant_transfer_arm_tx(struct radiant_transfer *t, uint8_t ctrl,
			const uint8_t *payload, uint8_t payload_len,
			radiant_time_t t_sync_at)
{
	/* Every caller but retry_packet() sends something for the first time. */
	return arm_tx(t, ctrl, payload, payload_len, t_sync_at, false);
}

/* What every window the engine opens shares: this channel's format, RF and
 * address filter, and STOP_ON_FIRST, because exactly one frame is expected and
 * stopping frees the operation slot so the answer can be armed from inside
 * the same callback. A request, not a guarantee: the HAL forbids
 * STOP_ON_FIRST on a merged window, so a window sharing hardware with a
 * tracked channel won't stop early. That costs receive current but doesn't
 * change behaviour - the frame is recognised by its control byte, not by
 * being alone. */
static void window_req_init(const struct radiant_transfer *t,
			    struct radiant_sched_rx *req)
{
	memset(req, 0, sizeof(*req));
	req->fmt = t->fmt;
	req->rf_index = t->cfg.rf_index;
	/* &t->filter, not a local: the HAL says the filter array must stay
	 * valid until the terminal event, and the scheduler copies it into the
	 * merged array only at the moment it arms. */
	req->filters = &t->filter;
	req->n_filters = 1u;
	req->stop_on_first = true;
}

/* Open the window the acknowledgement is expected in, centred on the
 * measured 1560 us with a guard sized on the observed range rather than the
 * standard deviation. */
static int arm_ack_window(struct radiant_transfer *t, radiant_time_t t_data_sync)
{
	struct radiant_sched_rx req;
	int rc;

	window_req_init(t, &req);
	req.t_open = t_data_sync + (radiant_time_t)RADIANT_TRANSFER_REPLY_US -
		     (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US;
	/* A slot-opening packet is answered on the slave's slot-reply
	 * turnaround, ~2190 us, the first time; a burst receiver answers at
	 * ~1560. That first window spans both (RADIANT_TRANSFER_OPENER_REPLY_US).
	 * Until 2026-11-11 it closed at +1810 and a Garmin's +2209 answer to
	 * a master's 0x8A was never heard: no master-originated burst or
	 * acknowledged message had ever completed on air. A repeat's window is
	 * the burst one (answered_on_slot_turnaround()); t->retries already
	 * counts this attempt, since retry_packet() bumps it before the TX
	 * event this runs from. */
	req.t_close = t_data_sync +
		      (radiant_time_t)(answered_on_slot_turnaround(t->cur_ctrl,
								    t->retries != 0u) ?
				       RADIANT_TRANSFER_OPENER_REPLY_US :
				       RADIANT_TRANSFER_REPLY_US) +
		      (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US;
	/* The reserve runs on to where whatever follows this window goes: the
	 * next packet at ack t_sync + NEXT_PACKET_US, or, if the window closes
	 * empty, the retry at the same instant (retry_packet(): data t_sync +
	 * reply + NEXT_PACKET_US, i.e. t_close - ACK_GUARD_US + NEXT_PACKET_US).
	 * Without it both were armed at a fresh reservation's lead, and on a
	 * backend whose window ends late (the CC26x2 holds 508 us past t_close,
	 * caps.rx_close_hold_us) the retry missed that lead by ~20 us and the
	 * transfer failed (archive/benchmarks/2026-11-12-ti-findings/). */
	req.follow_on_us = RADIANT_TRANSFER_NEXT_PACKET_US;

	rc = radiant_sched_request_rx(t->cfg.channel, &req);
	if (rc != RADIANT_RADIO_OK_RC) {
		return RADIANT_TRANSFER_EIO;
	}

	t->op = RADIANT_TRANSFER_OP_EXTERNAL;
	return RADIANT_TRANSFER_OK;
}

/*
 * The acknowledger's twin of arm_ack_window(): inside an inbound burst, open
 * the window the peer's next packet arrives in, ack t_sync + NEXT_PACKET_US
 * (1546/1550 measured), guarded like the acknowledgement window. The same
 * window catches the peer's retry of the packet just acknowledged, since a
 * retry goes where the next packet would have gone. The reserve runs on to
 * the acknowledgement that packet will need: REPLY_US after the window's late
 * edge at the most, plus the frame.
 */
static int arm_next_window(struct radiant_transfer *t, radiant_time_t ack_t_sync)
{
	struct radiant_sched_rx req;
	int rc;

	window_req_init(t, &req);
	req.t_open = ack_t_sync + (radiant_time_t)RADIANT_TRANSFER_NEXT_PACKET_US -
		     (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US;
	req.t_close = ack_t_sync + (radiant_time_t)RADIANT_TRANSFER_NEXT_PACKET_US +
		      (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US;
	req.follow_on_us = (uint16_t)(RADIANT_TRANSFER_REPLY_US +
				      RADIANT_TRANSFER_ACK_GUARD_US +
				      radiant_frame_tail_us(t->fmt));

	rc = radiant_sched_request_rx(t->cfg.channel, &req);
	if (rc != RADIANT_RADIO_OK_RC) {
		return RADIANT_TRANSFER_EIO;
	}

	t->op = RADIANT_TRANSFER_OP_EXTERNAL;
	return RADIANT_TRANSFER_OK;
}

/* Forget an inbound burst. Returns whether it ended before its LAST packet
 * was acknowledged, which is what the owner reports as RX_FAILED. */
static bool clear_inbound(struct radiant_transfer *t)
{
	bool unfinished = t->in_burst && !t->in_last_acked;

	t->in_burst = false;
	t->in_last_acked = false;
	t->in_data_ctrl = 0u;
	/* Nothing may repeat an acknowledgement of a burst that is over. */
	t->reply_ctrl = 0u;
	t->reply_attempts = 0u;
	return unfinished;
}

static void report_rx_failed(struct radiant_transfer *t)
{
	t->stats.rx_failed++;
	if (t->cfg.ops != NULL && t->cfg.ops->rx_failed != NULL) {
		t->cfg.ops->rx_failed(t->cfg.ctx);
	}
}

/* An inbound burst is over, completed or not: back to IDLE, RX_FAILED if it
 * was unfinished, and let the owner post its tracked work again. State first,
 * so an owner that posts from inbound_ended() finds the engine idle. */
static void end_inbound(struct radiant_transfer *t)
{
	bool failed = clear_inbound(t);

	t->op = 0u;
	t->state = RADIANT_TRANSFER_STATE_IDLE;
	if (failed) {
		report_rx_failed(t);
	}
	if (t->cfg.ops != NULL && t->cfg.ops->inbound_ended != NULL) {
		t->cfg.ops->inbound_ended(t->cfg.ctx);
	}
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
	/* A new packet: its retries start from zero. A retry does not come
	 * through here (retry_packet() re-arms directly), so this is exactly
	 * "per packet", not per attempt. */
	t->retries = 0u;
	t->data_t_sync = RADIANT_TIME_NEVER;
	return RADIANT_TRANSFER_OK;
}

/*
 * The acknowledgement window closed empty. Re-arm the same packet if Rev 5.1
 * sec 5.4.3 / 9.5.5.3 allows a retry here; see RADIANT_TRANSFER_TX_RETRIES_MAX
 * for the rules. Returns false when the caller must fail the transfer.
 */
static bool retry_packet(struct radiant_transfer *t)
{
	radiant_time_t at;

	if (!t->own_block || t->block == NULL ||
	    t->pkt_in_block >= t->n_pkt_in_block ||
	    t->retries >= (uint8_t)RADIANT_TRANSFER_TX_RETRIES_MAX ||
	    t->data_t_sync == RADIANT_TIME_NEVER) {
		return false;
	}
	/* A single-packet burst: first packet and last packet at once. */
	if (t->pkt_index == 0u && radiant_ctrl_is_last(t->cur_ctrl)) {
		return false;
	}

	if (!t->cfg.slot_opener && t->pkt_index == 0u) {
		/* A slave's packet 0 rides the master's reply window, which
		 * only opens again on the master's next slot. */
		if (t->cfg.ops == NULL || t->cfg.ops->retry_t_sync == NULL) {
			return false;
		}
		at = t->cfg.ops->retry_t_sync(t->cfg.ctx);
		if (at == RADIANT_TIME_NEVER) {
			return false;
		}
	} else {
		/* Where the next packet would have gone had the acknowledgement
		 * arrived: its expected instant + 1550 us. A first-attempt slot
		 * opener is answered at the slave's slot turnaround; a repeat,
		 * by a receiver already in the burst, at the burst turnaround. */
		radiant_time_t reply =
			answered_on_slot_turnaround(t->cur_ctrl, t->retries != 0u) ?
				(radiant_time_t)RADIANT_TRANSFER_OPENER_REPLY_US :
				(radiant_time_t)RADIANT_TRANSFER_REPLY_US;

		at = t->data_t_sync + reply +
		     (radiant_time_t)RADIANT_TRANSFER_NEXT_PACKET_US;
	}

	/* cur_ctrl, not a fresh radiant_transfer_ctrl(): the sequence bit
	 * must not move, or the receiver takes the repeat for the next
	 * packet. Not try_send(), which would zero the retry count. A repeat,
	 * so its reserve is the burst turnaround's. */
	if (arm_tx(t, t->cur_ctrl,
		   &t->block[(size_t)t->pkt_in_block * RADIANT_TRANSFER_PKT_BYTES],
		   RADIANT_TRANSFER_PKT_BYTES, at, true) != RADIANT_TRANSFER_OK) {
		return false;
	}

	t->retries++;
	t->stats.retries++;
	t->data_t_sync = RADIANT_TIME_NEVER;
	t->state = RADIANT_TRANSFER_STATE_TX_DATA;
	return true;
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
	} else if ((t->state == RADIANT_TRANSFER_STATE_TX_DATA ||
		    t->state == RADIANT_TRANSFER_STATE_WAIT_ACK) &&
		   t->own_block && owned && t->staged == NULL &&
		   (t->block_seg & RADIANT_TRANSFER_SEG_END) == 0u &&
		   (segment & RADIANT_TRANSFER_SEG_START) == 0u) {
		/*
		 * PRE-STAGED CONTINUATION. A burst block that arrives while the
		 * one before it is still on the air is held here, and advance()
		 * sends its first packet from the acknowledgement's own callback
		 * at ack t_sync + NEXT_PACKET_US. Without it every block boundary
		 * waited for NEXT_BLOCK to travel radio callback -> event thread
		 * -> bridge -> this call, which on the cc26xx (600 us arm lead)
		 * did not fit inside 1550 us and lost every chained packet
		 * (2026-11-12 bench). Never behind an END block: that block is
		 * the transfer's last, so nothing may follow it.
		 *
		 * Nothing is armed here, so nothing can fail: the block is the
		 * engine's from this return until it is released by NEXT_BLOCK
		 * or by the transfer's terminal event (B1-B5).
		 */
		t->staged = block;
		t->staged_len = len;
		t->staged_seg = segment;
		t->stats.blocks_accepted++;
		t->stats.blocks_staged++;
		RADIANT_DIAG_INC(cont_submits);
		return RADIANT_TRANSFER_OK;
	} else {
		/* Mid-packet with a block already staged, or behind the END
		 * block. The host must wait for NEXT_BLOCK; this is the
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

		RADIANT_DIAG_INC(cont_submits);
		/* Host turnaround: the acknowledgement that asked for this
		 * block (next_t_sync - NEXT_PACKET_US) to its arrival here. */
		if (t->next_t_sync != RADIANT_TIME_NEVER) {
			RADIANT_DIAG_TURN(radiant_radio_now() -
					  (t->next_t_sync -
					   (radiant_time_t)RADIANT_TRANSFER_NEXT_PACKET_US));
		}
		if (when < earliest) {
			RADIANT_DIAG_INC(cont_late);
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
	case RADIANT_TRANSFER_STATE_TX_REPLY:
	case RADIANT_TRANSFER_STATE_WAIT_NEXT: {
		/* WAIT_NEXT has the same shape: the armed window is the
		 * caller's to cancel with its scheduler slot. An inbound burst
		 * closed before its LAST was acknowledged is the one thing
		 * this reports, as rx_failed - never a TX event. */
		bool unfinished = clear_inbound(t);

		/* Our acknowledgement of a peer's packet, not a transfer the
		 * host started: no terminal event is owed (radiant_transfer.h,
		 * "follows an accepted call"), and raising TX_FAILED here told
		 * the host a send it never made had failed - and released the
		 * bridge's shared burst-block semaphore with it (review
		 * 2026-10-31, E2-5.3). The armed reply is the caller's to take
		 * off the air: antr_channel_close() cancels the channel's
		 * scheduler slot next, antr_stack_reset() has already reset the
		 * scheduler. A terminal that lands anyway meets t->op == 0 and
		 * is counted late. The saved reply goes too: re-sending an ACK
		 * on a closed channel is never right. */
		t->op = 0u;
		t->state = RADIANT_TRANSFER_STATE_IDLE;
		if (unfinished) {
			report_rx_failed(t);
		}
		return RADIANT_TRANSFER_OK;
	}
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

	if (t->pkt_in_block >= t->n_pkt_in_block && t->staged != NULL) {
		/* The block is spent and the next one is already here: chain it
		 * from this callback, then hand the spent one back. Arm first,
		 * so a re-entrant submit from done() finds TX_DATA and stages
		 * behind the new block rather than racing it. */
		uint8_t *spent = t->block;
		uint8_t spent_len = t->block_len;

		t->block = t->staged;
		t->block_len = t->staged_len;
		t->block_seg = t->staged_seg;
		t->n_pkt_in_block = radiant_transfer_packets_in_block(t->staged_len);
		t->pkt_in_block = 0u;
		t->block_released = false;
		t->staged = NULL;
		t->staged_len = 0u;
		t->staged_seg = 0u;
		t->next_t_sync = ack_t_sync +
				 (radiant_time_t)RADIANT_TRANSFER_NEXT_PACKET_US;

		last = ((t->block_seg & RADIANT_TRANSFER_SEG_END) != 0u) &&
		       (t->n_pkt_in_block == 1u);
		rc = try_send(t, t->pkt_index, last, t->block, t->next_t_sync);
		release_block(t, spent, spent_len);
		if (rc != RADIANT_TRANSFER_OK) {
			finish(t, RADIANT_TRANSFER_EV_TX_FAILED,
			       RADIANT_TRANSFER_FAIL_RADIO);
		}
		return;
	}

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

	if (t->state == RADIANT_TRANSFER_STATE_TX_REPLY && t->in_burst) {
		/* Our acknowledgement of a packet inside an inbound burst. The
		 * peer's next packet - or its retry of this one, if our
		 * acknowledgement was lost - is due NEXT_PACKET_US after it, so
		 * the window for it is armed from right here. After the LAST
		 * packet too, once, for the peer's retry of LAST.
		 *
		 * Anchored on the achieved t_sync when the reply went out, and
		 * on the instant it was armed for when it did not (missed or
		 * denied): the peer retries relative to where our reply should
		 * have been, not to when we noticed it wasn't. */
		radiant_time_t anchor = (e->status == RADIANT_RADIO_STATUS_OK) ?
						e->t_sync : t->in_ack_at;

		t->op = 0u;
		t->reply_t_sync = anchor;
		if (e->status == RADIANT_RADIO_STATUS_OK) {
			t->stats.acks_sent++;
		}
		if (arm_next_window(t, anchor) != RADIANT_TRANSFER_OK) {
			end_inbound(t);
			return;
		}
		t->state = RADIANT_TRANSFER_STATE_WAIT_NEXT;
		return;
	}

	if (t->state == RADIANT_TRANSFER_STATE_TX_REPLY) {
		/* Our acknowledgement of acknowledged data - a LAST packet with
		 * no inbound burst open. Nothing follows it, and this path is
		 * byte for byte what it was before inbound bursts existed.
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
	 * happened. Kept for a retry, which is placed from the same instant. */
	t->data_t_sync = e->t_sync;
	if (arm_ack_window(t, e->t_sync) != RADIANT_TRANSFER_OK) {
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_RADIO);
		return;
	}
	t->state = RADIANT_TRANSFER_STATE_WAIT_ACK;
}

/*
 * The inbound burst's continuation window. Returns false only for a broadcast,
 * which ends the burst and is handed back to the owner as a tracked frame.
 */
static bool on_next_window(struct radiant_transfer *t,
			   const struct radiant_rx_event *e)
{
	struct radiant_frame f;
	enum radiant_msg_type mt;

	switch (e->status) {
	case RADIANT_RADIO_STATUS_OK:
		break;
	case RADIANT_RADIO_STATUS_CRC_FAIL:
		/* Not asked for, as in WAIT_ACK; the window is still open. */
		return true;
	case RADIANT_RADIO_STATUS_TIMEOUT:
		/* Nothing came. After LAST that is the burst completing: our
		 * final acknowledgement was heard, so the peer has nothing to
		 * retry. Otherwise the peer's packet was lost, or our
		 * acknowledgement was and its retry was too: repeat the saved
		 * acknowledgement one measured retry interval after the last
		 * (3143 us, run 0). The peer reads it as stale and retries into
		 * the window that follows it. The repeat limits are
		 * radiant_transfer_reply_retransmit()'s; when they run out the
		 * burst has failed. */
		if (t->in_last_acked) {
			end_inbound(t);
			return true;
		}
		if (radiant_transfer_reply_repeat(
			    t, t->reply_t_sync +
				       (radiant_time_t)RADIANT_TRANSFER_REPLY_RETRY_US) !=
		    RADIANT_TRANSFER_OK) {
			end_inbound(t);
		}
		return true;
	default:
		/* FAILED, ABORTED: the window never ran or was taken away (a
		 * denial on an arbitrated build lands here). The burst ends;
		 * no worse than before inbound bursts were followed at all. */
		end_inbound(t);
		return true;
	}

	if (!decode_rx(t, e, &f)) {
		/* Undecodable: as good as an empty window, which its terminal
		 * will report next. */
		return true;
	}

	mt = radiant_frame_msg_type(f.ctrl_byte);
	if (mt == RADIANT_MSG_BURST_DATA || mt == RADIANT_MSG_BURST_LAST) {
		/* The next packet, or a duplicate of the last: on_data() tells
		 * them apart, arms the acknowledgement and moves to TX_REPLY. */
		if (radiant_transfer_on_data(t, &f, e->t_sync) !=
		    RADIANT_TRANSFER_OK) {
			end_inbound(t);
		}
		return true;
	}
	if (mt == RADIANT_MSG_BROADCAST) {
		/* The peer went back to broadcasting: it abandoned the burst
		 * (or finished it, if LAST was acknowledged). The frame itself
		 * is an ordinary slot frame, and the owner's to handle. */
		end_inbound(t);
		return false;
	}
	/* An acknowledgement or an unmeasured byte: nothing of ours. */
	return true;
}

bool radiant_transfer_on_rx_event(struct radiant_transfer *t,
			      const struct radiant_rx_event *e)
{
	struct radiant_frame f;

	if (t == NULL || e == NULL || !t->ready) {
		return true;
	}
	/* See the note on the same test in radiant_transfer_on_tx_event(). */
	if (t->op == 0u ||
	    (t->op != RADIANT_TRANSFER_OP_EXTERNAL && e->op != t->op)) {
		t->stats.late_events++;
		return true;
	}

	if (t->state == RADIANT_TRANSFER_STATE_ABORTING) {
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_ABORTED);
		return true;
	}
	if (t->state == RADIANT_TRANSFER_STATE_WAIT_NEXT) {
		return on_next_window(t, e);
	}
	if (t->state != RADIANT_TRANSFER_STATE_WAIT_ACK) {
		t->stats.late_events++;
		return true;
	}

	switch (e->status) {
	case RADIANT_RADIO_STATUS_OK:
		break;
	case RADIANT_RADIO_STATUS_CRC_FAIL:
		/* Non-terminal, and only delivered to a window that asked for
		 * it - which this one does not. The window is still open, so
		 * there is nothing to do but wait. */
		return true;
	case RADIANT_RADIO_STATUS_TIMEOUT:
		/* The reply window closed empty. Never captured from the
		 * sender's side (docs/spike-b-results.md, untested item 5), so
		 * the rule is the public spec's: a multi-packet burst retries
		 * the packet up to five times (Rev 5.1 sec 5.4.3, 9.5.5.3),
		 * then fails. See retry_packet(). */
		if (!retry_packet(t)) {
			finish(t, RADIANT_TRANSFER_EV_TX_FAILED,
			       RADIANT_TRANSFER_FAIL_NO_ACK);
		}
		return true;
	default:
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_RADIO);
		return true;
	}

	/* RADIANT_RX_STOP_ON_FIRST makes this event terminal, so the operation slot
	 * is already free and the next packet can be armed from right here. */
	if (!decode_rx(t, e, &f)) {
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_BAD_ACK);
		return true;
	}
	if (!radiant_transfer_ack_matches(t->cur_ctrl, f.ctrl_byte)) {
		/* The receiver repeating its answer to the previous packet: it
		 * missed this one. A miss, not a protocol error - retried like
		 * an empty window, and BAD_ACK only when no retry is allowed.
		 * Ending the burst here was both of the 2026-11-11 A2 failures:
		 * the Garmin kept repeating its stale answer, waiting. */
		if (radiant_transfer_ack_is_stale(t->cur_ctrl, f.ctrl_byte)) {
			t->stats.stale_acks++;
			if (retry_packet(t)) {
				return true;
			}
		}
		finish(t, RADIANT_TRANSFER_EV_TX_FAILED, RADIANT_TRANSFER_FAIL_BAD_ACK);
		return true;
	}

	if (radiant_ctrl_is_last(t->cur_ctrl)) {
		/* 0xE2/0xF2: the transfer is complete. The END block is
		 * released by this event and no other (B4). */
		t->pkt_acked++;
		t->stats.packets_sent++;
		finish(t, RADIANT_TRANSFER_EV_TX_COMPLETED, RADIANT_TRANSFER_FAIL_NONE);
		return true;
	}

	advance(t, e->t_sync);
	return true;
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

int radiant_transfer_set_power(struct radiant_transfer *t,
			       struct radiant_tx_power power)
{
	unsigned int key;

	if (t == NULL) {
		return RADIANT_TRANSFER_EINVAL;
	}
	/* arm_tx() reads cfg.power from the radio interrupt mid-transfer; the
	 * struct is wider than one store, so a torn read could pair a new dbm
	 * with an old use_raw. */
	key = radiant_event_crit_enter();
	t->cfg.power = power;
	radiant_event_crit_exit(key);

	return RADIANT_TRANSFER_OK;
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

const uint8_t *radiant_transfer_cur_payload(const struct radiant_transfer *t)
{
	if (t == NULL || t->state != RADIANT_TRANSFER_STATE_TX_DATA ||
	    t->block == NULL || t->pkt_in_block >= t->n_pkt_in_block) {
		return NULL;
	}

	return &t->block[(size_t)t->pkt_in_block * RADIANT_TRANSFER_PKT_BYTES];
}

bool radiant_transfer_cur_is_slot(const struct radiant_transfer *t)
{
	return t != NULL && t->state == RADIANT_TRANSFER_STATE_TX_DATA &&
	       t->retries == 0u && radiant_ctrl_is_slot_opener(t->cur_ctrl);
}

bool radiant_transfer_cur_starts_burst(const struct radiant_transfer *t)
{
	/* own_block rules out acknowledged data; !last on packet 0 rules out a
	 * one-packet burst; retries == 0 makes it the first attempt only. */
	return t != NULL && t->state == RADIANT_TRANSFER_STATE_TX_DATA &&
	       t->pkt_index == 0u && t->retries == 0u && t->own_block &&
	       !radiant_ctrl_is_last(t->cur_ctrl);
}
