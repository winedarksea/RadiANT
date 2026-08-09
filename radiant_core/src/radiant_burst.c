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
 * from any adopter-gated ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * This file includes no Zephyr header and nothing from the application. The
 * gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I radiant_core/include -I radiant_core/tests -fsyntax-only radiant_core/src/radiant_burst.c
 *
 * ---------------------------------------------------------------------------
 * The loop, once, in full
 * ---------------------------------------------------------------------------
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
 * interrupt context. That is the supported low-jitter path and at these
 * intervals it is the only one that works: the operation slot is freed before
 * the terminal callback runs precisely so that the next operation can be armed
 * from inside it.
 *
 * "Arm" means POST TO radiant_sched.c, not call the HAL. radiant_sched.c is the single
 * arming authority and the only place in radiant_core that calls radiant_radio_tx() or
 * radiant_radio_rx(); this file used to call both, and the two sites - the shared
 * radiant_transfer_arm_tx() and the static arm_ack_window() below - were redirected
 * when the two modules first met. See "One radio, one arming authority" in
 * radiant_transfer.h for what that costs and what it buys.
 *
 * ---------------------------------------------------------------------------
 * Where every event goes, so that B1-B5 hold by construction
 * ---------------------------------------------------------------------------
 * There are exactly three places a done() call is made - release_next() and
 * finish(), and finish() is reached from six branches - and both of them are
 * guarded by t->block_released. The invariant is a conjunction of two things
 * that are each cheap to see:
 *
 *   a block is recorded as accepted only after the radio has taken its first
 *   packet (so a rejected submit raises nothing, B2 and B5); and
 *
 *   a block is released once, by whichever of release_next() or finish()
 *   reaches it first, and the flag stops the other (B3 and B4).
 *
 * The failure mode if this is wrong is not a crash. The bridge's k_sem_take()
 * waits its full 1000 ms and then answers the host with a plausible-looking
 * ANTW_TRANSFER_IN_PROGRESS, so an ANT-FS transfer that should take two seconds
 * takes ten minutes and nothing in the build, the log or the host library says
 * why. It cannot be caught by inspection, which is why
 * radiant_core/tests/src/test_transfer.c counts released blocks against accepted
 * ones on the success path, the mid-transfer-failure path, the abort path and
 * the rejected-block path.
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

/*
 * Hand the block back and ask for the next one.
 *
 * The engine's state is moved to WAIT_BLOCK *before* done() runs, because
 * done() is where the bridge gives its semaphore back and the very next thing
 * that can happen is a submit call - possibly re-entrantly, from inside this
 * callback. A state machine that updated itself afterwards would overwrite it.
 */
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
	/* Null it as well as flagging it: B1 says the backend must not assume
	 * the buffer is still valid after the releasing event, so there must be
	 * nothing left here to read by accident. */
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
	/*
	 * The same five bytes the reply window filters on, and not a second
	 * derivation of them. radiant_transfer_init() already resolved the
	 * channel ID into t->filter because "the peer answers on the channel's
	 * own address" - which is the same statement as "this is the address the
	 * channel transmits with", read in the other direction. Calling
	 * radiant_frame_addr() again here would be a second copy of that fact,
	 * free to drift from the first.
	 */
	memcpy(req.addr, t->filter.addr, sizeof(req.addr));
	req.addr_len = t->filter.addr_len;
	/* t->tx_body, not a local: the HAL says the body must stay valid and
	 * unmodified until the completion callback because backends DMA out of
	 * it. The mock deep-copies at the arm call and would not catch a stack
	 * buffer here, so this has to be right by construction rather than by
	 * test. */
	req.body = t->tx_body;
	req.body_len = t->tx_body_len;
	req.t_sync_at = t_sync_at;

	/*
	 * THE SCHEDULER, NOT THE RADIO. radiant_sched.c is the single arming
	 * authority - see "One radio, one arming authority" in radiant_transfer.h.
	 * This posts an intent against the engine's channel slot; the commit
	 * happens on the way out of the current radio callback, or at the
	 * radiant_sched_tick() the thread-context caller owes.
	 */
	rc = radiant_sched_request_tx(t->cfg.channel, &req);
	if (rc != RADIANT_RADIO_OK_RC) {
		/*
		 * The scheduler's codes are the HAL's, deliberately. A
		 * malformed request is RADIANT_RADIO_EINVAL and posting before
		 * radiant_sched_init() is RADIANT_RADIO_ESTATE; neither is something a
		 * transfer can recover from, so both become EIO and the
		 * transfer fails loudly rather than waiting for a frame that
		 * was never going to go out.
		 */
		return RADIANT_TRANSFER_EIO;
	}

	/*
	 * There is no HAL operation id to record: the scheduler merges several
	 * channels' requests into one hardware operation and hands each channel
	 * back only its own events, so the id it would report is not this
	 * engine's. The sentinel says "the events routed to me are mine", which
	 * is true precisely because the router is the thing that routed them.
	 */
	t->op = RADIANT_TRANSFER_OP_EXTERNAL;
	return RADIANT_TRANSFER_OK;
}

/*
 * Open the window the acknowledgement is expected in.
 *
 * Centred on the measured 1560 us with a guard sized on the observed range
 * rather than the standard deviation, and RADIANT_RX_STOP_ON_FIRST because exactly
 * one frame is expected: the peer's reply. STOP_ON_FIRST also frees the
 * operation slot on that frame, which is what lets the next data packet be
 * armed from inside the same callback.
 */
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
	req.filters = &t->filter;
	req.n_filters = 1u;
	req.t_open = t_data_sync + (radiant_time_t)RADIANT_TRANSFER_REPLY_US -
		     (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US;
	req.t_close = t_data_sync + (radiant_time_t)RADIANT_TRANSFER_REPLY_US +
		      (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US;
	/*
	 * A request rather than a guarantee now that the scheduler owns the
	 * arm: the HAL forbids STOP_ON_FIRST on a merged window, so a reply
	 * window that ends up sharing a hardware window with a tracked channel
	 * will not stop early. That costs receive current and delays the free
	 * of the operation slot; it does not change what this engine does,
	 * because the acknowledgement is recognised by its control byte and not
	 * by being the only frame in the window.
	 */
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

	/*
	 * The peer answers on the channel's own address, so the window filters
	 * on exactly the five bytes the channel transmits with. An on-air
	 * address match IS a channel-ID match, which is why no software
	 * comparison of the reply's channel ID follows.
	 */
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
	/*
	 * Refuse a backend that cannot acknowledge, loudly and at
	 * configuration time.
	 *
	 * The reply window has to be armed REPLY_US - GUARD ahead of itself and
	 * the reply frame REPLY_US ahead of itself, both measured from the
	 * instant the data packet's address was at the antenna. A backend whose
	 * minimum arm lead exceeds the first, or whose receive-to-transmit
	 * turnaround exceeds the second, cannot do acknowledged data at all -
	 * and acknowledged data is how a trainer's resistance gets set, so the
	 * alternative to failing here is discovering it as "ERG mode does not
	 * work" some months downstream.
	 */
	if ((uint32_t)caps->min_arm_lead_us + (uint32_t)RADIANT_TRANSFER_ACK_GUARD_US >
	    (uint32_t)RADIANT_TRANSFER_REPLY_US) {
		return RADIANT_TRANSFER_ENOTSUP;
	}
	if ((uint32_t)caps->rx_to_tx_us > (uint32_t)RADIANT_TRANSFER_REPLY_US) {
		return RADIANT_TRANSFER_ENOTSUP;
	}
	t->min_lead_us = caps->min_arm_lead_us;

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
			/* An acknowledged-data transfer is one block by
			 * construction and can never reach here; a burst that
			 * changed its mind about ownership mid-transfer is a
			 * caller bug, not a state to model. */
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
	/*
	 * A continuation whose scheduled instant has already gone by - the host
	 * took longer than one turnaround to produce the block - is sent as
	 * soon as the backend can instead of being failed. WHAT A REAL SENDER
	 * DOES HERE IS NOT MEASURED: the spike measured the *receiver*
	 * retransmitting, never a sender rescheduling, because no host block
	 * was ever late in a completed transfer. Slipping the packet is the
	 * choice that keeps a slow host working; failing the transfer would be
	 * equally defensible and equally unevidenced.
	 */
	if (!starting) {
		radiant_time_t earliest = radiant_radio_now() +
				      (radiant_time_t)t->min_lead_us +
				      (radiant_time_t)RADIANT_TRANSFER_ARM_SLACK_US;

		if (when < earliest) {
			when = earliest;
		}
	}

	/*
	 * Arm BEFORE recording the block as accepted.
	 *
	 * B2: on a non-zero return the backend owns nothing, must not touch the
	 * buffer and must not raise any of the three events for it. If the
	 * radio refuses, nothing below has run, nothing is released, and the
	 * bridge's synchronous release on a non-zero return is the only thing
	 * that happens to this block. Doing it the other way round - install,
	 * arm, roll back on failure - is how B5 gets violated: a released block
	 * hands back a semaphore the bridge still holds for a different one,
	 * and the next host packet overwrites a buffer the radio is
	 * transmitting from.
	 */
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

	/*
	 * The cancelled operation's terminal event still arrives - the HAL
	 * promises it, and K5 saw the mock deliver one with no fault injection
	 * at all. So this does not finish the transfer; it marks the engine as
	 * waiting for a terminal it is going to get either way, and the event
	 * handler below finishes it. From thread context radiant_radio_abort()
	 * delivers that terminal before it returns, so the transfer is over by
	 * the time this function does; from inside a callback the terminal is
	 * deferred until the current one returns, and so is the done() call.
	 */
	/*
	 * radiant_radio_abort(), NOT radiant_sched_cancel(), and the asymmetry with the
	 * two arm sites is deliberate rather than an oversight.
	 *
	 * radiant_sched_cancel() is documented as SILENT for the channel it
	 * cancels - the caller already knows - so no completion would arrive
	 * and the ABORTING state, which exists only to wait for one, would
	 * never end. Aborting the radio directly still produces the terminal
	 * event the HAL guarantees, the scheduler recognises it as its own
	 * armed operation and cleans up its members correctly, and this engine
	 * finishes on it exactly as before.
	 *
	 * The cost is real and is worth naming: if the operation being aborted
	 * was a MERGED receive window, the other channels sharing it lose the
	 * rest of their window and are told RADIANT_SCHED_DONE_ABORTED. That is
	 * loud and recoverable, whereas an engine stuck in ABORTING is neither.
	 * Closing it properly needs a "cancel and tell me" entry point in
	 * radiant_sched.h, which this change was not authorised to add.
	 */
	t->state = RADIANT_TRANSFER_STATE_ABORTING;
	(void)radiant_radio_abort();

	return RADIANT_TRANSFER_OK;
}

/* ---------------------------------------------------------------------------
 * Radio events
 * ---------------------------------------------------------------------------
 */

/*
 * Rebuild the received frame.
 *
 * The matched address bytes never reach RAM - that is what a hardware address
 * match means - so the address half of the wire frame is regenerated from the
 * channel ID this window filtered on, which is exactly the identity the match
 * proved.
 *
 * RADIANT_FRAME_TRUSTED_CRC is right for every delivered event, not only on a
 * backend with crc_in_hw: the HAL says a packet delivered with status OK has a
 * verified CRC, and a backend without a usable CRC engine verifies in software
 * before delivering. The received CRC bytes are not available here either way.
 */
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
		/*
		 * The block is spent. State first, then the release, because
		 * done() is where the bridge frees its buffer and the submit
		 * for the next block can arrive re-entrantly from inside it.
		 */
		t->state = RADIANT_TRANSFER_STATE_WAIT_BLOCK;
		t->next_t_sync = ack_t_sync +
				 (radiant_time_t)RADIANT_TRANSFER_NEXT_PACKET_US;
		release_next(t);
		return;
	}

	/* Another packet out of the block already in hand - a 16- or 24-byte
	 * advanced-burst block, fragmented. The sequence bit alternates across
	 * these too: it counts on-air packets, not host blocks. */
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
	/*
	 * RADIANT_TRANSFER_OP_EXTERNAL means the arming authority has no HAL
	 * operation id to compare against and has already routed this event to
	 * the channel that owns it - see radiant_transfer.h. Any other non-zero
	 * value is a real id and is still checked: somebody else's operation,
	 * or the late terminal of one this engine already replaced, is
	 * recognisable rather than merely surprising.
	 */
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
		 * follows it: the peer either sends the next packet or does
		 * not, and radiant_sched.c owns the retransmission timer. */
		t->op = 0u;
		t->state = RADIANT_TRANSFER_STATE_IDLE;
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

	/*
	 * The window is placed on the t_sync the backend actually achieved, not
	 * on the one that was asked for. They are the same on any backend that
	 * can schedule exactly; on one that cannot, anchoring on the request
	 * would put the window 1560 us after an instant that never happened.
	 */
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
		/*
		 * The reply window closed with nothing in it.
		 *
		 * NOT RETRIED, and that is the honest answer rather than a
		 * missing feature. What a data sender does when an
		 * acknowledgement goes missing was never captured - untested
		 * item 5 of docs/spike-b-part2-results.md - because no
		 * acknowledgement went missing inside a completed transfer. The
		 * receiver's behaviour IS measured (21 retransmissions at
		 * 3143 us) and is implemented, in radiant_ack.c, for the direction
		 * it was measured in. Inventing a sender-side retry here would
		 * put an unmeasured frame on the air at an unmeasured instant
		 * and, worse, would look like evidence to whoever read it next.
		 */
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
		/* Bit 5 was set on the packet and echoed by the reply, so this
		 * is 0xE2 or 0xF2 - the transfer is complete. The END block is
		 * released by this event and by no other (B4). */
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
