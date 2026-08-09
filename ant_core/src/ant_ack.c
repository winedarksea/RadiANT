/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ant_ack.c - the control byte of an acknowledged exchange, in both directions.
 *
 * Provenance: clean-room. Written from docs/spike-b-part2-results.md - the
 * six-field control byte, its eleven measured values, the four measured
 * data/acknowledgement pairs and the 1.55 ms turnaround - together with
 * docs/ant-radio-link.md, ant_core/include/ant_frame.h and
 * ant_core/include/ant_radio_hal.h. Nothing here derives from sdk-ant, from
 * libant.a, from disassembly of any binary, or from any adopter-gated ANT+
 * device profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * This file includes no Zephyr header and nothing from the application, on
 * purpose - same reasoning as ant_frame.c. The gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I ant_core/include -I ant_core/tests -fsyntax-only ant_core/src/ant_ack.c
 *
 * ---------------------------------------------------------------------------
 * What is in this file and what is in ant_burst.c
 * ---------------------------------------------------------------------------
 * The split is by direction, not by message type, because message type is not a
 * thing an ANT frame has: acknowledged data and a one-packet burst are the same
 * bytes on the air.
 *
 *   here          the control byte itself - how a data packet's five flags are
 *                 chosen, what an acknowledgement of it looks like, and how to
 *                 recognise one; and the ACKNOWLEDGER's path, which receives a
 *                 data packet and has 1.55 ms to put a reply on the air.
 *   ant_burst.c   the ORIGINATOR's state machine - packets, the one-bit
 *                 sequence, host blocks and who owns them.
 *
 * ant_transfer_ack_data() lives here and is four lines, three of which are a
 * memcpy. It calls ant_transfer_submit(). That is the point.
 */

#include <string.h>

#include "ant_transfer.h"

/* ---------------------------------------------------------------------------
 * The encoder
 * ---------------------------------------------------------------------------
 */

int ant_transfer_ctrl_fields(uint32_t index, bool last, bool slot_opener,
			     struct ant_ctrl_fields *out)
{
	if (out == NULL) {
		return ANT_TRANSFER_EINVAL;
	}

	out->exchange = true;   /* b7: acknowledged data and burst alike */
	out->ack = false;       /* b6: this is the data packet, not the reply */
	out->last = last;       /* b5: echoed by the acknowledgement */
	/*
	 * b4. ONE BIT. There is no wider form and there is nowhere in the frame
	 * for one: a 17-packet and a 51-packet burst were captured end to end
	 * with the sniffer's ring-drop counter at zero and this bit alternated
	 * across every on-air packet while bits 7:5 held still.
	 */
	out->seq = ((index & 1u) != 0u);
	/*
	 * b3. Only the first packet of a transfer opens the slot. `[inferred]`:
	 * see the gap list in ant_transfer.h.
	 */
	out->slot_opener = slot_opener && (index == 0u);

	return ANT_TRANSFER_OK;
}

int ant_transfer_ctrl(uint32_t index, bool last, bool slot_opener, uint8_t *out)
{
	struct ant_ctrl_fields f;
	uint8_t ctrl;
	int rc;

	if (out == NULL) {
		return ANT_TRANSFER_EINVAL;
	}

	rc = ant_transfer_ctrl_fields(index, last, slot_opener, &f);
	if (rc != ANT_TRANSFER_OK) {
		return rc;
	}

	ctrl = ant_ctrl_encode(&f);

	/*
	 * Six bytes are reachable from here - 0x82, 0x92, 0xA2, 0xB2 in slot,
	 * 0x8A and 0xAA opening one - and all six are in the measured eleven,
	 * so this can never fire today. It is the assertion that a future edit
	 * to the field rules above did not quietly invent a seventh.
	 */
	if (!ant_ctrl_observed(ctrl)) {
		return ANT_TRANSFER_EINVAL;
	}

	*out = ctrl;
	return ANT_TRANSFER_OK;
}

uint8_t ant_transfer_reply_ctrl(uint8_t data_ctrl)
{
	/*
	 * ant_frame.c holds the four measured pairs as a table and returns 0 -
	 * never a legal control byte - for anything else, including the slot
	 * openers. Reproducing the arithmetic here instead of calling it would
	 * be a second implementation of the one relation the whole exchange
	 * turns on, and the second one would be the one that forgets that bit 4
	 * is complemented rather than echoed.
	 */
	return ant_ctrl_reply_for(data_ctrl);
}

bool ant_transfer_ack_matches(uint8_t data_ctrl, uint8_t ack_ctrl)
{
	if (!ant_ctrl_low_ok(data_ctrl) || !ant_ctrl_low_ok(ack_ctrl)) {
		return false;
	}
	if (!ant_ctrl_is_exchange(data_ctrl) || !ant_ctrl_is_exchange(ack_ctrl)) {
		return false;
	}
	if (ant_ctrl_is_ack(data_ctrl) || !ant_ctrl_is_ack(ack_ctrl)) {
		return false;
	}
	/* b5 echoed. 0xE2/0xF2 answer the final packet, 0xC2/0xD2 answer a
	 * non-final one, and confusing the two is how a transfer reports
	 * completion one packet early. */
	if (ant_ctrl_is_last(data_ctrl) != ant_ctrl_is_last(ack_ctrl)) {
		return false;
	}
	/* b4 complemented: "the sequence bit I expect next". */
	if (ant_ctrl_seq(data_ctrl) == ant_ctrl_seq(ack_ctrl)) {
		return false;
	}

	/*
	 * b3 is NOT compared. All four measured pairs are in-slot frames
	 * answered by in-slot frames; no acknowledgement of a slot opener has
	 * ever been captured, so a check on that bit would be a guess capable
	 * of rejecting real traffic. See gap 2 in ant_transfer.h.
	 */
	return true;
}

uint8_t ant_transfer_packets_in_block(uint8_t block_len)
{
	if (block_len == 0u || block_len > ANT_TRANSFER_BLOCK_MAX) {
		return 0u;
	}
	if ((block_len % ANT_TRANSFER_PKT_BYTES) != 0u) {
		return 0u;
	}
	return (uint8_t)(block_len / ANT_TRANSFER_PKT_BYTES);
}

int ant_transfer_build_body(const struct ant_transfer_cfg *cfg, uint8_t ctrl,
			    const uint8_t *payload, uint8_t payload_len,
			    uint8_t *body, size_t body_cap)
{
	struct ant_ctrl_fields fields;
	struct ant_frame f;
	struct ant_frame_wire w;

	if (cfg == NULL || payload == NULL || body == NULL) {
		return ANT_TRANSFER_EINVAL;
	}
	if (payload_len != ANT_TRANSFER_PKT_BYTES) {
		return ANT_TRANSFER_EINVAL;
	}

	/*
	 * Byte -> fields -> ant_frame_make() -> bytes, rather than handing the
	 * byte to ant_frame_encode() directly.
	 *
	 * The round trip is not ceremony. ant_frame_make() is the one place
	 * that refuses a control byte nothing has ever transmitted;
	 * ant_frame_encode() deliberately carries a hand-set byte untouched
	 * because a bench experiment needs to put a candidate encoding on the
	 * air without editing the frame module. Taking the second path from
	 * production code disables the only check standing between a field
	 * model that can name 256 bytes and the eleven that exist.
	 */
	if (ant_ctrl_decode(ctrl, &fields) != ANT_FRAME_OK) {
		return ANT_TRANSFER_EINVAL;
	}
	if (ant_frame_make(&f, &cfg->id, &fields, payload, payload_len) !=
	    ANT_FRAME_OK) {
		return ANT_TRANSFER_EINVAL;
	}
	if (ant_frame_encode(ANT_FRAME_CFG_TRACKING, cfg->net_addr, &f, &w) !=
	    ANT_FRAME_OK) {
		return ANT_TRANSFER_EINVAL;
	}
	if (body_cap < (size_t)w.body_len) {
		return ANT_TRANSFER_EINVAL;
	}

	memcpy(body, w.body, w.body_len);
	return (int)w.body_len;
}

/* ---------------------------------------------------------------------------
 * Acknowledged data - a one-packet burst, and nothing more than that
 * ---------------------------------------------------------------------------
 */

int ant_transfer_ack_data(struct ant_transfer *t, const uint8_t *payload,
			  uint8_t len, ant_time_t t_sync_at)
{
	if (t == NULL || payload == NULL) {
		return ANT_TRANSFER_EINVAL;
	}
	if (len != ANT_TRANSFER_PKT_BYTES) {
		return ANT_TRANSFER_EINVAL;
	}

	/*
	 * Copied, because src/ant_radio.h says antr_acknowledge_message_tx()
	 * copies before returning and transfers no ownership. The copy then
	 * runs through the ordinary burst path as a single START|END block, so
	 * the engine has one sequencer rather than two and 0xAA cannot drift
	 * away from being 0xA2 with the slot bit set.
	 */
	memcpy(t->ack_payload, payload, ANT_TRANSFER_PKT_BYTES);

	return ant_transfer_submit(t, t->ack_payload, ANT_TRANSFER_PKT_BYTES,
				   ANT_TRANSFER_SEG_START | ANT_TRANSFER_SEG_END,
				   false, t_sync_at);
}

/* ---------------------------------------------------------------------------
 * The acknowledger - the path that has 1.55 ms to transmit
 * ---------------------------------------------------------------------------
 */

int ant_transfer_on_data(struct ant_transfer *t, const struct ant_frame *f,
			 ant_time_t t_sync)
{
	enum ant_msg_type mt;
	uint8_t reply;
	uint8_t payload[ANT_TRANSFER_PKT_BYTES];
	int rc;

	if (t == NULL || f == NULL) {
		return ANT_TRANSFER_EINVAL;
	}
	if (!t->ready) {
		return ANT_TRANSFER_ESTATE;
	}
	if (t->state != ANT_TRANSFER_STATE_IDLE) {
		/* Either we are mid-transfer as the originator, or the previous
		 * acknowledgement has not left yet. Neither can be answered
		 * with the radio we do not have. */
		return ANT_TRANSFER_ESTATE;
	}
	if (f->payload_len != ANT_TRANSFER_PKT_BYTES) {
		return ANT_TRANSFER_EINVAL;
	}

	/*
	 * Dispatch on the control byte, which cannot distinguish acknowledged
	 * data from a one-packet burst and must not try. BURST_LAST is both.
	 */
	mt = ant_frame_msg_type(f->ctrl_byte);
	if (mt != ANT_MSG_BURST_DATA && mt != ANT_MSG_BURST_LAST) {
		return ANT_TRANSFER_EINVAL;
	}

	reply = ant_transfer_reply_ctrl(f->ctrl_byte);
	if (reply == 0u) {
		/*
		 * A slot-opening data packet, 0x8A or 0xAA. No acknowledgement
		 * of one has ever been captured, so bit 3 of the reply has no
		 * measured value and there is nothing honest to send. Counted,
		 * not guessed - gap 2 in ant_transfer.h. The experiment that
		 * closes it is a master-originated burst to a real slave.
		 */
		t->stats.unackable_openers++;
		return ANT_TRANSFER_ENOTSUP;
	}

	/*
	 * Every acknowledgement ever observed carried the acknowledger's own
	 * broadcast buffer - in each capture the master's next bicycle-power
	 * page, which then went out unchanged as the next scheduled broadcast
	 * one channel period later. Whether an acknowledgement may carry
	 * something else is untested item 4, so a channel with nothing to
	 * broadcast does not get a substitute here.
	 */
	if (t->cfg.ops == NULL || t->cfg.ops->broadcast_payload == NULL ||
	    !t->cfg.ops->broadcast_payload(t->cfg.ctx, payload)) {
		t->stats.no_broadcast++;
		return ANT_TRANSFER_ESTATE;
	}

	/*
	 * Arm FIRST. The measured budget between this packet's t_sync and the
	 * reply's is 1567/1559 us, it is the tightest deadline in the link
	 * layer, and anything spent in the caller's payload handling before the
	 * transmit is armed comes straight out of it. Missing it produces no
	 * error code anywhere - the peer simply retransmits its own frame and
	 * eventually abandons the transfer.
	 */
	rc = ant_transfer_arm_tx(t, reply, payload, ANT_TRANSFER_PKT_BYTES,
				 t_sync + (ant_time_t)ANT_TRANSFER_REPLY_US);
	if (rc != ANT_TRANSFER_OK) {
		return rc;
	}

	t->state = ANT_TRANSFER_STATE_TX_REPLY;
	t->reply_ctrl = reply;
	/* This frame is attempt one of at most ANT_TRANSFER_REPLY_ATTEMPTS_MAX. */
	t->reply_attempts = 1u;
	memcpy(t->reply_payload, payload, ANT_TRANSFER_PKT_BYTES);

	if (t->cfg.ops->rx_data != NULL) {
		t->cfg.ops->rx_data(t->cfg.ctx, f->payload, f->payload_len,
				    ant_ctrl_is_last(f->ctrl_byte));
	}

	return ANT_TRANSFER_OK;
}

int ant_transfer_reply_retransmit(struct ant_transfer *t, ant_time_t t_sync_at)
{
	int rc;

	if (t == NULL) {
		return ANT_TRANSFER_EINVAL;
	}
	if (!t->ready || t->state != ANT_TRANSFER_STATE_IDLE) {
		return ANT_TRANSFER_ESTATE;
	}
	if (t->reply_ctrl == 0u) {
		return ANT_TRANSFER_ESTATE;
	}
	/*
	 * 21 attempts - the original plus twenty repeats - measured twice in
	 * run 0's two 16-block attempts, at 3143 us. `[measured, n=2]` and on
	 * one stack, which is why the limit is a named constant rather than a
	 * literal: it is the number to change when a second stack is measured.
	 */
	if (t->reply_attempts >= (uint8_t)ANT_TRANSFER_REPLY_ATTEMPTS_MAX) {
		return ANT_TRANSFER_ESTATE;
	}

	rc = ant_transfer_arm_tx(t, t->reply_ctrl, t->reply_payload,
				 ANT_TRANSFER_PKT_BYTES, t_sync_at);
	if (rc != ANT_TRANSFER_OK) {
		return rc;
	}

	t->reply_attempts++;
	t->stats.ack_retransmits++;
	t->state = ANT_TRANSFER_STATE_TX_REPLY;
	return ANT_TRANSFER_OK;
}
