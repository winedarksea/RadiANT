/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ack.c - the control byte of an acknowledged exchange, in both directions.
 *
 * Provenance: clean-room. Written from docs/spike-b-part2-results.md - the
 * six-field control byte, its eleven measured values, the four measured
 * data/acknowledgement pairs and the 1.55 ms turnaround - together with
 * docs/ant-radio-link.md, radiant/include/radiant/radiant_frame.h and
 * radiant/include/radiant/radiant_radio_hal.h. Nothing here derives from sdk-ant, from
 * libant.a, from disassembly of any binary, or from any ANT+
 * device profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * This file includes no Zephyr header and nothing from the application, on
 * purpose - same reasoning as radiant_frame.c. The gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I radiant/include -I radiant/tests -fsyntax-only radiant/src/radiant_ack.c
 *
 * Split by direction, not message type (acknowledged data and a one-packet
 * burst are the same bytes on the air): this file holds the control byte
 * itself and the ACKNOWLEDGER's path (1.55 ms to reply); radiant_burst.c holds
 * the ORIGINATOR's state machine.
 */

#include <string.h>

#include <radiant/radiant_transfer.h>

/* ---------------------------------------------------------------------------
 * The encoder
 * ---------------------------------------------------------------------------
 */

int radiant_transfer_ctrl_fields(uint32_t index, bool last, bool slot_opener,
			     struct radiant_ctrl_fields *out)
{
	if (out == NULL) {
		return RADIANT_TRANSFER_EINVAL;
	}

	out->exchange = true;   /* b7: acknowledged data and burst alike */
	out->ack = false;       /* b6: this is the data packet, not the reply */
	out->last = last;       /* b5: echoed by the acknowledgement */
	/* b4: one bit, alternates per on-air packet (measured on 17- and
	 * 51-packet bursts; bits 7:6 hold still while this toggles). */
	out->seq = ((index & 1u) != 0u);
	/* b3: only the first packet of a transfer opens the slot. `[inferred]`
	 * - see the gap list in radiant_transfer.h. */
	out->slot_opener = slot_opener && (index == 0u);

	return RADIANT_TRANSFER_OK;
}

int radiant_transfer_ctrl(uint32_t index, bool last, bool slot_opener, uint8_t *out)
{
	struct radiant_ctrl_fields f;
	uint8_t ctrl;
	int rc;

	if (out == NULL) {
		return RADIANT_TRANSFER_EINVAL;
	}

	rc = radiant_transfer_ctrl_fields(index, last, slot_opener, &f);
	if (rc != RADIANT_TRANSFER_OK) {
		return rc;
	}

	ctrl = radiant_ctrl_encode(&f);

	/* All six reachable bytes are in the measured eleven, so this can never
	 * fire today; it guards against a future edit inventing a seventh. */
	if (!radiant_ctrl_observed(ctrl)) {
		return RADIANT_TRANSFER_EINVAL;
	}

	*out = ctrl;
	return RADIANT_TRANSFER_OK;
}

uint8_t radiant_transfer_reply_ctrl(uint8_t data_ctrl)
{
	/* radiant_frame.c holds the four measured pairs as a table and returns 0
	 * (never a legal control byte) for anything else, including slot
	 * openers; reproducing that arithmetic here would risk a second,
	 * disagreeing implementation. */
	return radiant_ctrl_reply_for(data_ctrl);
}

bool radiant_transfer_ack_matches(uint8_t data_ctrl, uint8_t ack_ctrl)
{
	if (!radiant_ctrl_low_ok(data_ctrl) || !radiant_ctrl_low_ok(ack_ctrl)) {
		return false;
	}
	if (!radiant_ctrl_is_exchange(data_ctrl) || !radiant_ctrl_is_exchange(ack_ctrl)) {
		return false;
	}
	if (radiant_ctrl_is_ack(data_ctrl) || !radiant_ctrl_is_ack(ack_ctrl)) {
		return false;
	}
	/* b5 echoed. Confusing final vs non-final acks reports completion one
	 * packet early. */
	if (radiant_ctrl_is_last(data_ctrl) != radiant_ctrl_is_last(ack_ctrl)) {
		return false;
	}
	/* b4 complemented: "the sequence bit I expect next". */
	if (radiant_ctrl_seq(data_ctrl) == radiant_ctrl_seq(ack_ctrl)) {
		return false;
	}

	/* b3 is NOT compared: no acknowledgement of a slot opener has ever been
	 * captured, so checking it would risk rejecting real traffic. See gap 2
	 * in radiant_transfer.h. */
	return true;
}

uint8_t radiant_transfer_packets_in_block(uint8_t block_len)
{
	if (block_len == 0u || block_len > RADIANT_TRANSFER_BLOCK_MAX) {
		return 0u;
	}
	if ((block_len % RADIANT_TRANSFER_PKT_BYTES) != 0u) {
		return 0u;
	}
	return (uint8_t)(block_len / RADIANT_TRANSFER_PKT_BYTES);
}

int radiant_transfer_build_body(const struct radiant_transfer_cfg *cfg, uint8_t ctrl,
			    const uint8_t *payload, uint8_t payload_len,
			    uint8_t *body, size_t body_cap)
{
	struct radiant_ctrl_fields fields;
	struct radiant_frame f;
	struct radiant_frame_wire w;

	if (cfg == NULL || payload == NULL || body == NULL) {
		return RADIANT_TRANSFER_EINVAL;
	}
	if (payload_len != RADIANT_TRANSFER_PKT_BYTES) {
		return RADIANT_TRANSFER_EINVAL;
	}

	/* Byte -> fields -> radiant_frame_make() -> bytes, not radiant_frame_encode()
	 * directly: radiant_frame_make() refuses a control byte nothing has ever
	 * transmitted, while radiant_frame_encode() deliberately lets a bench
	 * experiment carry a hand-set byte untouched. Taking that second path
	 * from production code would disable the only check against the 245
	 * control bytes that don't exist. */
	if (radiant_ctrl_decode(ctrl, &fields) != RADIANT_FRAME_OK) {
		return RADIANT_TRANSFER_EINVAL;
	}
	if (radiant_frame_make(&f, &cfg->id, &fields, payload, payload_len) !=
	    RADIANT_FRAME_OK) {
		return RADIANT_TRANSFER_EINVAL;
	}
	if (radiant_frame_encode(RADIANT_FRAME_CFG_TRACKING, cfg->net_addr, &f, &w) !=
	    RADIANT_FRAME_OK) {
		return RADIANT_TRANSFER_EINVAL;
	}
	if (body_cap < (size_t)w.body_len) {
		return RADIANT_TRANSFER_EINVAL;
	}

	memcpy(body, w.body, w.body_len);
	return (int)w.body_len;
}

/* ---------------------------------------------------------------------------
 * Acknowledged data - a one-packet burst, and nothing more than that
 * ---------------------------------------------------------------------------
 */

int radiant_transfer_ack_data(struct radiant_transfer *t, const uint8_t *payload,
			  uint8_t len, radiant_time_t t_sync_at)
{
	if (t == NULL || payload == NULL) {
		return RADIANT_TRANSFER_EINVAL;
	}
	if (len != RADIANT_TRANSFER_PKT_BYTES) {
		return RADIANT_TRANSFER_EINVAL;
	}

	/* Copied per src/ant_radio.h's antr_acknowledge_message_tx() contract (no
	 * ownership transfer), then run through the ordinary burst path as one
	 * START|END block so there's a single sequencer, not two. */
	memcpy(t->ack_payload, payload, RADIANT_TRANSFER_PKT_BYTES);

	return radiant_transfer_submit(t, t->ack_payload, RADIANT_TRANSFER_PKT_BYTES,
				   RADIANT_TRANSFER_SEG_START | RADIANT_TRANSFER_SEG_END,
				   false, t_sync_at);
}

/* ---------------------------------------------------------------------------
 * The acknowledger - the path that has 1.55 ms to transmit
 * ---------------------------------------------------------------------------
 */

int radiant_transfer_on_data(struct radiant_transfer *t, const struct radiant_frame *f,
			 radiant_time_t t_sync)
{
	enum radiant_msg_type mt;
	uint8_t reply;
	uint8_t payload[RADIANT_TRANSFER_PKT_BYTES];
	int rc;

	if (t == NULL || f == NULL) {
		return RADIANT_TRANSFER_EINVAL;
	}
	if (!t->ready) {
		return RADIANT_TRANSFER_ESTATE;
	}
	if (t->state != RADIANT_TRANSFER_STATE_IDLE) {
		/* Either we are mid-transfer as the originator, or the previous
		 * acknowledgement has not left yet. Neither can be answered
		 * with the radio we do not have. */
		return RADIANT_TRANSFER_ESTATE;
	}
	if (f->payload_len != RADIANT_TRANSFER_PKT_BYTES) {
		return RADIANT_TRANSFER_EINVAL;
	}

	/* The control byte cannot distinguish acknowledged data from a
	 * one-packet burst; BURST_LAST is both. */
	mt = radiant_frame_msg_type(f->ctrl_byte);
	if (mt != RADIANT_MSG_BURST_DATA && mt != RADIANT_MSG_BURST_LAST) {
		return RADIANT_TRANSFER_EINVAL;
	}

	reply = radiant_transfer_reply_ctrl(f->ctrl_byte);
	if (reply == 0u) {
		/* A slot-opening data packet (0x8A/0xAA): no acknowledgement of
		 * one has ever been captured, so there's nothing honest to send.
		 * See gap 2 in radiant_transfer.h. */
		t->stats.unackable_openers++;
		return RADIANT_TRANSFER_ENOTSUP;
	}

	/* Every observed acknowledgement carried the acknowledger's own
	 * broadcast buffer, unchanged; whether it may carry something else is
	 * untested item 4, so no substitute is invented here. */
	if (t->cfg.ops == NULL || t->cfg.ops->broadcast_payload == NULL ||
	    !t->cfg.ops->broadcast_payload(t->cfg.ctx, payload)) {
		t->stats.no_broadcast++;
		return RADIANT_TRANSFER_ESTATE;
	}

	/* Arm FIRST: the measured budget is 1567/1559 us, the tightest deadline
	 * in the link layer. Missing it raises no error - the peer just
	 * retransmits and eventually abandons the transfer. */
	rc = radiant_transfer_arm_tx(t, reply, payload, RADIANT_TRANSFER_PKT_BYTES,
				 t_sync + (radiant_time_t)RADIANT_TRANSFER_REPLY_US);
	if (rc != RADIANT_TRANSFER_OK) {
		return rc;
	}

	t->state = RADIANT_TRANSFER_STATE_TX_REPLY;
	t->reply_ctrl = reply;
	t->reply_attempts = 1u; /* attempt one of at most REPLY_ATTEMPTS_MAX */
	memcpy(t->reply_payload, payload, RADIANT_TRANSFER_PKT_BYTES);

	if (t->cfg.ops->rx_data != NULL) {
		t->cfg.ops->rx_data(t->cfg.ctx, f->payload, f->payload_len,
				    radiant_ctrl_is_last(f->ctrl_byte));
	}

	return RADIANT_TRANSFER_OK;
}

int radiant_transfer_reply_retransmit(struct radiant_transfer *t, radiant_time_t t_sync_at)
{
	int rc;

	if (t == NULL) {
		return RADIANT_TRANSFER_EINVAL;
	}
	if (!t->ready || t->state != RADIANT_TRANSFER_STATE_IDLE) {
		return RADIANT_TRANSFER_ESTATE;
	}
	if (t->reply_ctrl == 0u) {
		return RADIANT_TRANSFER_ESTATE;
	}
	/* 21 attempts (original + 20 repeats), measured twice at 3143 us
	 * `[measured, n=2]` on one stack - hence a named constant to update
	 * when a second stack is measured. */
	if (t->reply_attempts >= (uint8_t)RADIANT_TRANSFER_REPLY_ATTEMPTS_MAX) {
		return RADIANT_TRANSFER_ESTATE;
	}
	/* AND it must not be ancient: nothing clears the saved reply when an
	 * exchange ends (TX_REPLY -> IDLE skips finish()), so without this
	 * staleness check the attempt counter alone would authorise repeating
	 * an ack from minutes ago. The saved reply is dropped below too, so a
	 * caller that keeps asking can't keep retrying it. */
	if (t_sync_at < t->reply_t_sync ||
	    (t_sync_at - t->reply_t_sync) >
		    (radiant_time_t)RADIANT_TRANSFER_REPLY_VALID_US) {
		t->reply_ctrl = 0u;
		t->reply_attempts = 0u;
		t->stats.stale_replies++;
		return RADIANT_TRANSFER_ESTATE;
	}

	rc = radiant_transfer_arm_tx(t, t->reply_ctrl, t->reply_payload,
				 RADIANT_TRANSFER_PKT_BYTES, t_sync_at);
	if (rc != RADIANT_TRANSFER_OK) {
		return rc;
	}

	t->reply_attempts++;
	t->stats.ack_retransmits++;
	t->state = RADIANT_TRANSFER_STATE_TX_REPLY;
	return RADIANT_TRANSFER_OK;
}
