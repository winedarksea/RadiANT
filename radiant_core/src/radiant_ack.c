/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ack.c - the control byte of an acknowledged exchange, in both directions.
 *
 * Provenance: clean-room. Written from docs/spike-b-part2-results.md - the
 * six-field control byte, its eleven measured values, the four measured
 * data/acknowledgement pairs and the 1.55 ms turnaround - together with
 * docs/ant-radio-link.md, radiant_core/include/radiant_core/radiant_frame.h and
 * radiant_core/include/radiant_core/radiant_radio_hal.h. Nothing here derives from sdk-ant, from
 * libant.a, from disassembly of any binary, or from any adopter-gated ANT+
 * device profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * This file includes no Zephyr header and nothing from the application, on
 * purpose - same reasoning as radiant_frame.c. The gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I radiant_core/include -I radiant_core/tests -fsyntax-only radiant_core/src/radiant_ack.c
 *
 * ---------------------------------------------------------------------------
 * What is in this file and what is in radiant_burst.c
 * ---------------------------------------------------------------------------
 * The split is by direction, not by message type, because message type is not a
 * thing an ANT frame has: acknowledged data and a one-packet burst are the same
 * bytes on the air.
 *
 *   here          the control byte itself - how a data packet's five flags are
 *                 chosen, what an acknowledgement of it looks like, and how to
 *                 recognise one; and the ACKNOWLEDGER's path, which receives a
 *                 data packet and has 1.55 ms to put a reply on the air.
 *   radiant_burst.c   the ORIGINATOR's state machine - packets, the one-bit
 *                 sequence, host blocks and who owns them.
 *
 * radiant_transfer_ack_data() lives here and is four lines, three of which are a
 * memcpy. It calls radiant_transfer_submit(). That is the point.
 */

#include <string.h>

#include <radiant_core/radiant_transfer.h>

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
	/*
	 * b4. ONE BIT. There is no wider form and there is nowhere in the frame
	 * for one: a 17-packet and a 51-packet burst were captured end to end
	 * with the sniffer's ring-drop counter at zero and this bit alternated
	 * across every on-air packet while bits 7:6 held still. (7:6, not 7:5 -
	 * bit 5 goes to 1 on the final packet of every burst, which is b5 "last".)
	 */
	out->seq = ((index & 1u) != 0u);
	/*
	 * b3. Only the first packet of a transfer opens the slot. `[inferred]`:
	 * see the gap list in radiant_transfer.h.
	 */
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

	/*
	 * Six bytes are reachable from here - 0x82, 0x92, 0xA2, 0xB2 in slot,
	 * 0x8A and 0xAA opening one - and all six are in the measured eleven,
	 * so this can never fire today. It is the assertion that a future edit
	 * to the field rules above did not quietly invent a seventh.
	 */
	if (!radiant_ctrl_observed(ctrl)) {
		return RADIANT_TRANSFER_EINVAL;
	}

	*out = ctrl;
	return RADIANT_TRANSFER_OK;
}

uint8_t radiant_transfer_reply_ctrl(uint8_t data_ctrl)
{
	/*
	 * radiant_frame.c holds the four measured pairs as a table and returns 0 -
	 * never a legal control byte - for anything else, including the slot
	 * openers. Reproducing the arithmetic here instead of calling it would
	 * be a second implementation of the one relation the whole exchange
	 * turns on, and the second one would be the one that forgets that bit 4
	 * is complemented rather than echoed.
	 */
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
	/* b5 echoed. 0xE2/0xF2 answer the final packet, 0xC2/0xD2 answer a
	 * non-final one, and confusing the two is how a transfer reports
	 * completion one packet early. */
	if (radiant_ctrl_is_last(data_ctrl) != radiant_ctrl_is_last(ack_ctrl)) {
		return false;
	}
	/* b4 complemented: "the sequence bit I expect next". */
	if (radiant_ctrl_seq(data_ctrl) == radiant_ctrl_seq(ack_ctrl)) {
		return false;
	}

	/*
	 * b3 is NOT compared. All four measured pairs are in-slot frames
	 * answered by in-slot frames; no acknowledgement of a slot opener has
	 * ever been captured, so a check on that bit would be a guess capable
	 * of rejecting real traffic. See gap 2 in radiant_transfer.h.
	 */
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

	/*
	 * Byte -> fields -> radiant_frame_make() -> bytes, rather than handing the
	 * byte to radiant_frame_encode() directly.
	 *
	 * The round trip is not ceremony. radiant_frame_make() is the one place
	 * that refuses a control byte nothing has ever transmitted;
	 * radiant_frame_encode() deliberately carries a hand-set byte untouched
	 * because a bench experiment needs to put a candidate encoding on the
	 * air without editing the frame module. Taking the second path from
	 * production code disables the only check standing between a field
	 * model that can name 256 bytes and the eleven that exist.
	 */
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

	/*
	 * Copied, because src/ant_radio.h says antr_acknowledge_message_tx()
	 * copies before returning and transfers no ownership. The copy then
	 * runs through the ordinary burst path as a single START|END block, so
	 * the engine has one sequencer rather than two and 0xAA cannot drift
	 * away from being 0xA2 with the slot bit set.
	 */
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

	/*
	 * Dispatch on the control byte, which cannot distinguish acknowledged
	 * data from a one-packet burst and must not try. BURST_LAST is both.
	 */
	mt = radiant_frame_msg_type(f->ctrl_byte);
	if (mt != RADIANT_MSG_BURST_DATA && mt != RADIANT_MSG_BURST_LAST) {
		return RADIANT_TRANSFER_EINVAL;
	}

	reply = radiant_transfer_reply_ctrl(f->ctrl_byte);
	if (reply == 0u) {
		/*
		 * A slot-opening data packet, 0x8A or 0xAA. No acknowledgement
		 * of one has ever been captured, so bit 3 of the reply has no
		 * measured value and there is nothing honest to send. Counted,
		 * not guessed - gap 2 in radiant_transfer.h. The experiment that
		 * closes it is a master-originated burst to a real slave.
		 */
		t->stats.unackable_openers++;
		return RADIANT_TRANSFER_ENOTSUP;
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
		return RADIANT_TRANSFER_ESTATE;
	}

	/*
	 * Arm FIRST. The measured budget between this packet's t_sync and the
	 * reply's is 1567/1559 us, it is the tightest deadline in the link
	 * layer, and anything spent in the caller's payload handling before the
	 * transmit is armed comes straight out of it. Missing it produces no
	 * error code anywhere - the peer simply retransmits its own frame and
	 * eventually abandons the transfer.
	 */
	rc = radiant_transfer_arm_tx(t, reply, payload, RADIANT_TRANSFER_PKT_BYTES,
				 t_sync + (radiant_time_t)RADIANT_TRANSFER_REPLY_US);
	if (rc != RADIANT_TRANSFER_OK) {
		return rc;
	}

	t->state = RADIANT_TRANSFER_STATE_TX_REPLY;
	t->reply_ctrl = reply;
	/* This frame is attempt one of at most RADIANT_TRANSFER_REPLY_ATTEMPTS_MAX. */
	t->reply_attempts = 1u;
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
	/*
	 * 21 attempts - the original plus twenty repeats - measured twice in
	 * run 0's two 16-block attempts, at 3143 us. `[measured, n=2]` and on
	 * one stack, which is why the limit is a named constant rather than a
	 * literal: it is the number to change when a second stack is measured.
	 */
	if (t->reply_attempts >= (uint8_t)RADIANT_TRANSFER_REPLY_ATTEMPTS_MAX) {
		return RADIANT_TRANSFER_ESTATE;
	}
	/*
	 * AND it must not be ancient. Nothing clears the saved reply when an
	 * exchange ends - TX_REPLY returns to IDLE without going through
	 * finish() - so the attempt counter on its own would authorise repeating
	 * an acknowledgement from minutes ago as readily as one from 3 ms ago.
	 * See RADIANT_TRANSFER_REPLY_VALID_US. Refusing is not enough on its own
	 * either: the saved reply is dropped here, so this cannot be the answer
	 * to a bug that keeps asking.
	 */
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
