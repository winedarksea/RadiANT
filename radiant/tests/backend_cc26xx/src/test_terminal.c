/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The terminal / abort / callback path of radiant_radio_cc26xx.c (TODO.md
 * "Backend drift: TI vs nRF" items 3, 4 and the status-word part of 5):
 *   - exactly one terminal per accepted arm, attributed to the right op, when
 *     an abort and the RF SWI's own completion meet, and when a cancelled
 *     command's late callback arrives behind several later operations;
 *   - RF_EventCmdPreempted is DENIED only for a window that never opened,
 *     ABORTED once it had listened (or a transmit had started);
 *   - a generic 0x08xx command error is counted in status_err.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <ti/drivers/rf/RF.h>
#include <driverlib/rf_mailbox.h>
#include <driverlib/rf_prop_mailbox.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_cc26xx_probe.h"

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	cc_reset(true);
}

ZTEST_SUITE(radiant_backend_cc26xx_terminal, NULL, NULL, before, NULL, NULL);

static const uint8_t body8[RADIANT_RADIO_BODY_MAX] = {
	0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA,
};

static struct radiant_rx_req rx_reqs[4];
static struct radiant_rx_filter rx_filts[4];
static unsigned int rx_req_next;

/* A tracking receive [t_open, t_open + len]; returns its RF handle. Usable
 * from inside a probe hook (a chained arm). */
static RF_CmdHandle arm_rx(radiant_time_t t_open, uint32_t len, uint32_t flags,
			   uint32_t *op)
{
	struct radiant_rx_req *req = &rx_reqs[rx_req_next % ARRAY_SIZE(rx_reqs)];
	struct radiant_rx_filter *filt = &rx_filts[rx_req_next % ARRAY_SIZE(rx_filts)];

	rx_req_next++;
	cc_rx_req(req, filt, t_open, len);
	req->flags = flags;
	zassert_equal(radiant_radio_rx(req, op), RADIANT_RADIO_OK_RC);
	return cc_cmd_handle();
}

static bool is_terminal(int status)
{
	return status != RADIANT_RADIO_STATUS_OK &&
	       status != RADIANT_RADIO_STATUS_CRC_FAIL;
}

/* Terminal rx events the core saw for `op`. */
static uint32_t rx_terminals(uint32_t op)
{
	uint32_t n = 0u;

	for (uint32_t i = 0u; i < MIN(cc_probe()->rx_events, (uint32_t)CC_PROBE_LOG); i++) {
		if (cc_probe()->rx[i].op == op && is_terminal(cc_probe()->rx[i].status)) {
			n++;
		}
	}
	return n;
}

static int rx_terminal_status(uint32_t op)
{
	for (uint32_t i = 0u; i < MIN(cc_probe()->rx_events, (uint32_t)CC_PROBE_LOG); i++) {
		if (cc_probe()->rx[i].op == op && is_terminal(cc_probe()->rx[i].status)) {
			return cc_probe()->rx[i].status;
		}
	}
	return -1;
}

/* One in-window frame into ch's queue and its entry-done callback. */
static void hear_frame(RF_CmdHandle ch, radiant_time_t t_sync)
{
	uint8_t pl[RADIANT_RADIO_ADDR_MAX + RADIANT_RADIO_BODY_MAX + 2];
	uint8_t n = cc_payload(cc_addr, cc_hw_addr_len(), body8, false, pl);

	zassert_equal(fake_rf_rx_entry(ch, pl, n, -50, cc_rat_for_t_sync(t_sync), 0u), 0);
	fake_rf_fire(ch, RF_EventRxEntryDone);
}

/* ------------------------------------------------------------------------
 * Item 3: one terminal, the right one
 * ------------------------------------------------------------------------
 */

static RF_CmdHandle race_ch;
static uint32_t race_op2;

/* The RF SWI completing the window at the instant abort() flushes it. */
static void swi_completes_at_flush(RF_CmdHandle ch)
{
	ARG_UNUSED(ch);
	fake_rf.on_cancel_entry = NULL;
	fake_rf_complete(race_ch, PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);
}

/* The core re-arming from its terminal callback, as a scheduler pass does. */
static void rearm_on_terminal(const struct radiant_rx_event *evt)
{
	if (is_terminal(evt->status) && race_op2 == 0u) {
		(void)arm_rx(radiant_radio_now() + 3000u, 1000u, 0u, &race_op2);
	}
}

/* abort() against the window's own LastCmdDone landing at the flush: op 1
 * gets exactly one terminal; op 2, armed from that terminal, is untouched. */
ZTEST(radiant_backend_cc26xx_terminal, test_abort_vs_swi_completion_one_terminal)
{
	uint32_t op1 = 0u;

	race_op2 = 0u;
	race_ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op1);
	fake_rf_start(race_ch);
	fake_rf.on_cancel_entry = swi_completes_at_flush;
	cc_probe()->on_rx = rearm_on_terminal;

	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	cc_probe()->on_rx = NULL;
	(void)fake_rf_deliver_pending();

	zassert_equal(rx_terminals(op1), 1u, "op 1: exactly one terminal");
	zassert_not_equal(race_op2, 0u, "the core re-armed from the terminal");
	zassert_equal(rx_terminals(race_op2), 0u, "op 2 was not ended by op 1's abort");
	zassert_equal(cc_kind(), 1, "op 2 is live");
	zassert_equal(cc_op_id(), race_op2);
}

/* abort() from inside a frame callback whose RF callback also carries the
 * window's LastCmdDone (the driver coalesces them): one terminal for op 1,
 * op 2 armed after it is live and gets nothing. */
static uint32_t coal_op2;

static void abort_and_rearm_on_frame(const struct radiant_rx_event *evt)
{
	if (evt->status == RADIANT_RADIO_STATUS_OK && coal_op2 == 0u) {
		zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
		(void)arm_rx(radiant_radio_now() + 3000u, 1000u, 0u, &coal_op2);
	}
}

ZTEST(radiant_backend_cc26xx_terminal, test_abort_in_coalesced_frame_callback)
{
	uint32_t op1 = 0u;
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	RF_CmdHandle ch = arm_rx(t_open, 1000u, 0u, &op1);
	uint8_t pl[RADIANT_RADIO_ADDR_MAX + RADIANT_RADIO_BODY_MAX + 2];
	uint8_t n = cc_payload(cc_addr, cc_hw_addr_len(), body8, false, pl);

	coal_op2 = 0u;
	zassert_equal(fake_rf_rx_entry(ch, pl, n, -50, cc_rat_for_t_sync(t_open + 100u), 0u), 0);
	fake_rf_advance_us(3200u);
	fake_rf_set_status(ch, PROP_DONE_RXTIMEOUT);
	cc_probe()->on_rx = abort_and_rearm_on_frame;
	/* Entry done and the window's end in one driver callback. */
	fake_rf_fire(ch, RF_EventRxEntryDone | RF_EventLastCmdDone);
	cc_probe()->on_rx = NULL;
	(void)fake_rf_deliver_pending();

	zassert_equal(rx_terminals(op1), 1u);
	zassert_equal(rx_terminal_status(op1), RADIANT_RADIO_STATUS_ABORTED);
	zassert_not_equal(coal_op2, 0u);
	zassert_equal(rx_terminals(coal_op2), 0u, "op 1's end did not end op 2");
	zassert_equal(cc_kind(), 1);
	zassert_equal(cc_op_id(), coal_op2);
}

static uint32_t nest_op_b;
static uint32_t nest_op_c;

/*
 * From op A's STOP_ON_FIRST frame: arm B, abort B, and from B's ABORTED arm
 * C - a reply armed and then displaced inside one callback.
 */
static void nest_on_rx(const struct radiant_rx_event *evt)
{
	if (evt->status == RADIANT_RADIO_STATUS_OK && nest_op_b == 0u) {
		(void)arm_rx(radiant_radio_now() + 3000u, 1000u, 0u, &nest_op_b);
		zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	} else if (evt->op == nest_op_b && is_terminal(evt->status) && nest_op_c == 0u) {
		(void)arm_rx(radiant_radio_now() + 6000u, 1000u, 0u, &nest_op_c);
	}
}

/* The late cancel callbacks of A (retired by STOP_ON_FIRST) and B (aborted)
 * both arrive while C is live: neither may end C. */
ZTEST(radiant_backend_cc26xx_terminal, test_stale_cancels_after_nested_abort)
{
	uint32_t op_a = 0u;
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	RF_CmdHandle ch_a;

	nest_op_b = 0u;
	nest_op_c = 0u;
	ch_a = arm_rx(t_open, 1000u, RADIANT_RX_STOP_ON_FIRST, &op_a);
	fake_rf_start(ch_a);
	cc_probe()->on_rx = nest_on_rx;
	fake_rf_advance_us(2200u);
	hear_frame(ch_a, t_open + 100u);
	cc_probe()->on_rx = NULL;

	zassert_not_equal(nest_op_b, 0u);
	zassert_not_equal(nest_op_c, 0u);
	zassert_equal(rx_terminal_status(nest_op_b), RADIANT_RADIO_STATUS_ABORTED);
	zassert_equal(cc_op_id(), nest_op_c);

	zassert_true(fake_rf_pending() >= 2u, "A's and B's cancels are owed");
	(void)fake_rf_deliver_pending();

	zassert_equal(rx_terminals(nest_op_b), 1u);
	zassert_equal(rx_terminals(nest_op_c), 0u, "a stale cancel ended C");
	zassert_equal(cc_kind(), 1, "C is still live");

	fake_rf_complete(cc_cmd_handle(), PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);
	zassert_equal(rx_terminals(nest_op_c), 1u);
	zassert_equal(rx_terminal_status(nest_op_c), RADIANT_RADIO_STATUS_TIMEOUT);
}

static uint32_t body_op_a;
static uint32_t body_op_c;
static bool body_seen;
static uint8_t body_after_abort[RADIANT_RADIO_BODY_MAX];

/*
 * From a frame of op A (no STOP_ON_FIRST, so the drain does not retire A
 * first): abort A, and from A's nested ABORTED arm C, whose rx_queue_init()
 * resets the queue A's frame was read from. Then read the outer evt->body.
 */
static void body_on_rx(const struct radiant_rx_event *evt)
{
	if (evt->op == body_op_a && evt->status == RADIANT_RADIO_STATUS_OK &&
	    !body_seen) {
		body_seen = true;
		zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
		zassert_not_equal(body_op_c, 0u, "C armed from the nested terminal");
		memcpy(body_after_abort, evt->body, evt->body_len);
	} else if (evt->op == body_op_a && is_terminal(evt->status) &&
		   body_op_c == 0u) {
		(void)arm_rx(radiant_radio_now() + 6000u, 1000u, 0u, &body_op_c);
	}
}

/* HAL: the outer event's body stays valid until that callback returns, even
 * when the nested terminal re-arms the receiver. */
ZTEST(radiant_backend_cc26xx_terminal, test_body_survives_nested_rearm)
{
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	RF_CmdHandle ch_a;

	body_op_a = 0u;
	body_op_c = 0u;
	body_seen = false;
	memset(body_after_abort, 0, sizeof(body_after_abort));
	ch_a = arm_rx(t_open, 1000u, 0u, &body_op_a);
	fake_rf_start(ch_a);
	cc_probe()->on_rx = body_on_rx;
	fake_rf_advance_us(2200u);
	hear_frame(ch_a, t_open + 100u);
	cc_probe()->on_rx = NULL;

	zassert_true(body_seen);
	zassert_equal(rx_terminal_status(body_op_a), RADIANT_RADIO_STATUS_ABORTED);
	zassert_equal(cc_op_id(), body_op_c);
	zassert_mem_equal(body_after_abort, body8, cc_probe()->rx[0].body_len,
			  "the re-arm zeroed the frame under the callback");
}

/* abort() of a command the driver has already ended (flush: CmdEnded) whose
 * LastCmdDone is still queued in the SWI: that callback reaches no one. */
ZTEST(radiant_backend_cc26xx_terminal, test_abort_ended_command_late_end_dropped)
{
	uint32_t op1 = 0u;
	uint32_t op2 = 0u;
	RF_CmdHandle ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op1);

	fake_rf_cmd(ch)->ended = true;
	fake_rf_set_status(ch, PROP_DONE_RXTIMEOUT);
	fake_rf_queue_cb(ch, RF_EventLastCmdDone);

	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	zassert_equal(rx_terminal_status(op1), RADIANT_RADIO_STATUS_ABORTED);
	(void)arm_rx(radiant_radio_now() + 3000u, 1000u, 0u, &op2);

	zassert_equal(fake_rf_deliver_pending(), 1u);
	zassert_equal(rx_terminals(op1), 1u);
	zassert_equal(rx_terminals(op2), 0u);
	zassert_equal(cc_kind(), 1);
}

/* ------------------------------------------------------------------------
 * Item 4: preempted -> DENIED only if the window never opened
 * ------------------------------------------------------------------------
 */

/* Cancelled in the driver's pending queue, before its start: DENIED. */
ZTEST(radiant_backend_cc26xx_terminal, test_rx_preempted_before_start_denied)
{
	uint32_t op = 0u;
	RF_CmdHandle ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);

	fake_rf_fire(ch, RF_EventCmdCancelled | RF_EventCmdPreempted);
	zassert_equal(rx_terminal_status(op), RADIANT_RADIO_STATUS_DENIED);
	zassert_equal(cc_kind(), 0);
}

/* Dispatched to the RF core but aborted before its trigger: DENIED. */
ZTEST(radiant_backend_cc26xx_terminal, test_rx_preempted_dispatched_unopened_denied)
{
	uint32_t op = 0u;
	RF_CmdHandle ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);

	fake_rf_start(ch);
	fake_rf_advance_us(500u);
	fake_rf_fire(ch, RF_EventCmdAborted | RF_EventCmdPreempted);
	zassert_equal(rx_terminal_status(op), RADIANT_RADIO_STATUS_DENIED);
}

/* Never left the pending queue, though its start has passed (the other
 * client held the radio): nothing was heard, DENIED. */
ZTEST(radiant_backend_cc26xx_terminal, test_rx_preempted_pending_past_open_denied)
{
	uint32_t op = 0u;
	RF_CmdHandle ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);

	fake_rf_advance_us(2500u);
	fake_rf_fire(ch, RF_EventCmdCancelled | RF_EventCmdPreempted);
	zassert_equal(rx_terminal_status(op), RADIANT_RADIO_STATUS_DENIED);
}

/* Preempted after it heard a frame: it listened, ABORTED. */
ZTEST(radiant_backend_cc26xx_terminal, test_rx_preempted_after_frame_aborted)
{
	uint32_t op = 0u;
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	RF_CmdHandle ch = arm_rx(t_open, 1000u, 0u, &op);

	fake_rf_start(ch);
	fake_rf_advance_us(2200u);
	hear_frame(ch, t_open + 100u);
	zassert_equal(cc_probe()->rx_events, 1u);

	fake_rf_fire(ch, RF_EventCmdAborted | RF_EventCmdPreempted);
	zassert_equal(rx_terminals(op), 1u);
	zassert_equal(rx_terminal_status(op), RADIANT_RADIO_STATUS_ABORTED);
}

/* Preempted mid-window with nothing heard: the window was open, ABORTED. */
ZTEST(radiant_backend_cc26xx_terminal, test_rx_preempted_after_open_aborted)
{
	uint32_t op = 0u;
	RF_CmdHandle ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);

	fake_rf_start(ch);
	fake_rf_advance_us(2500u);
	fake_rf_fire(ch, RF_EventCmdAborted | RF_EventCmdPreempted);
	zassert_equal(rx_terminal_status(op), RADIANT_RADIO_STATUS_ABORTED);
}

static uint32_t arm_tx(radiant_time_t t_sync, RF_CmdHandle *ch)
{
	struct radiant_tx_req req;
	uint32_t op = 0u;

	cc_tx_req(&req, body8, t_sync);
	zassert_equal(radiant_radio_tx(&req, &op), RADIANT_RADIO_OK_RC);
	*ch = cc_cmd_handle();
	return op;
}

ZTEST(radiant_backend_cc26xx_terminal, test_tx_preempted_before_start_denied)
{
	RF_CmdHandle ch;
	const uint32_t op = arm_tx(radiant_radio_now() + 2000u, &ch);

	fake_rf_fire(ch, RF_EventCmdCancelled | RF_EventCmdPreempted);
	zassert_equal(cc_probe()->tx_events, 1u);
	zassert_equal(cc_probe()->tx[0].op, op);
	zassert_equal(cc_probe()->tx[0].status, RADIANT_RADIO_STATUS_DENIED);
	zassert_false(cc_probe()->tx[0].t_sync_exact);
}

/* Preempted after the preamble began: part of a frame went out, ABORTED. */
ZTEST(radiant_backend_cc26xx_terminal, test_tx_preempted_after_start_aborted)
{
	RF_CmdHandle ch;
	const uint32_t op = arm_tx(radiant_radio_now() + 2000u, &ch);

	fake_rf_start(ch);
	fake_rf_advance_us(2000u);
	fake_rf_fire(ch, RF_EventCmdAborted | RF_EventCmdPreempted);
	zassert_equal(cc_probe()->tx_events, 1u);
	zassert_equal(cc_probe()->tx[0].op, op);
	zassert_equal(cc_probe()->tx[0].status, RADIANT_RADIO_STATUS_ABORTED);
	zassert_false(cc_probe()->tx[0].t_sync_exact);
}

/* ------------------------------------------------------------------------
 * Item 5: command-end status words
 * ------------------------------------------------------------------------
 */

/* A normal empty end stays TIMEOUT and counts nothing. */
ZTEST(radiant_backend_cc26xx_terminal, test_rx_timeout_still_timeout)
{
	uint32_t op = 0u;
	RF_CmdHandle ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);
	const uint32_t err0 = cc_status_err();

	fake_rf_complete(ch, PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);
	zassert_equal(rx_terminal_status(op), RADIANT_RADIO_STATUS_TIMEOUT);
	zassert_equal(cc_status_err(), err0);
}

/* A generic 0x08xx error ending a command is counted, as 0x38xx already is;
 * each command end once, not once per callback. */
ZTEST(radiant_backend_cc26xx_terminal, test_status_err_counts_generic_errors)
{
	uint32_t op = 0u;
	RF_CmdHandle ch;
	uint32_t err0;

	if (!cc_diag_built()) {
		ztest_test_skip();
	}

	err0 = cc_status_err();  /* the counters outlive cc_reset() */
	(void)arm_tx(radiant_radio_now() + 2000u, &ch);
	fake_rf_complete(ch, ERROR_PAST_START, RF_EventLastCmdDone);
	zassert_equal(cc_status_err() - err0, 1u, "ERROR_PAST_START counted");

	ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);
	fake_rf_complete(ch, ERROR_SYNTH_PROG, RF_EventLastCmdDone);
	zassert_equal(cc_status_err() - err0, 2u, "ERROR_SYNTH_PROG counted");

	ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);
	fake_rf_complete(ch, PROP_ERROR_RXBUF, RF_EventLastCmdDone);
	zassert_equal(cc_status_err() - err0, 3u, "PROP_ERROR_RXBUF still counted");

	ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);
	fake_rf_complete(ch, PROP_DONE_OK, RF_EventLastCmdDone);
	zassert_equal(cc_status_err() - err0, 3u, "a normal end is not an error");
}

/* ------------------------------------------------------------------------
 * Item 3 (drain): an arm made from a frame callback owns the queue
 * ------------------------------------------------------------------------
 */

static uint32_t rearm_op2;

static void abort_rearm_once(const struct radiant_rx_event *evt)
{
	if (evt->status == RADIANT_RADIO_STATUS_OK && rearm_op2 == 0u) {
		zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
		(void)arm_rx(radiant_radio_now() + 3000u, 1000u, 0u, &rearm_op2);
	}
}

/* The core aborts op 1 and arms op 2 from op 1's frame callback (a reply
 * displacing the window). rx_arm() re-initialised the receive queue for op
 * 2, so the drain of op 1 must not then advance the read cursor over it:
 * op 2's first frame must be heard. */
ZTEST(radiant_backend_cc26xx_terminal, test_rearm_from_frame_callback_hears_next_frame)
{
	uint32_t op1 = 0u;
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	RF_CmdHandle ch = arm_rx(t_open, 1000u, 0u, &op1);
	RF_CmdHandle ch2;
	radiant_time_t t_open2;

	rearm_op2 = 0u;
	fake_rf_start(ch);
	fake_rf_advance_us(2200u);
	cc_probe()->on_rx = abort_rearm_once;
	hear_frame(ch, t_open + 100u);
	cc_probe()->on_rx = NULL;
	(void)fake_rf_deliver_pending();

	zassert_not_equal(rearm_op2, 0u);
	zassert_equal(cc_op_id(), rearm_op2);
	ch2 = cc_cmd_handle();
	t_open2 = radiant_radio_now() + 3000u;  /* as abort_rearm_once() armed it */
	fake_rf_start(ch2);
	fake_rf_advance_us(3200u);
	hear_frame(ch2, t_open2 + 100u);

	zassert_equal(cc_probe()->rx_events, 3u, "frame, ABORTED, op 2's frame");
	zassert_equal(cc_probe()->rx[2].op, rearm_op2);
	zassert_equal(cc_probe()->rx[2].status, RADIANT_RADIO_STATUS_OK);
}

/* ------------------------------------------------------------------------
 * Item 5: a command that ended in error, or never ran, is FAILED
 * ------------------------------------------------------------------------
 */

/* A late transmit start (pastTrig = 0): nothing went out. */
ZTEST(radiant_backend_cc26xx_terminal, test_tx_past_start_failed)
{
	RF_CmdHandle ch;
	const uint32_t op = arm_tx(radiant_radio_now() + 2000u, &ch);

	fake_rf_complete(ch, ERROR_PAST_START, RF_EventLastCmdDone);
	zassert_equal(cc_probe()->tx_events, 1u);
	zassert_equal(cc_probe()->tx[0].op, op);
	zassert_equal(cc_probe()->tx[0].status, RADIANT_RADIO_STATUS_FAILED);
	zassert_false(cc_probe()->tx[0].t_sync_exact);
	zassert_equal(cc_kind(), 0);
}

/* The chain ended (LastCmdDone) but the transmit op itself never ran, e.g.
 * a CMD_FS chained ahead of it failed: its status is still IDLE. */
ZTEST(radiant_backend_cc26xx_terminal, test_tx_never_ran_failed)
{
	RF_CmdHandle ch;

	(void)arm_tx(radiant_radio_now() + 2000u, &ch);
	fake_rf_complete(ch, IDLE, RF_EventLastCmdDone);
	zassert_equal(cc_probe()->tx[0].status, RADIANT_RADIO_STATUS_FAILED);
	zassert_false(cc_probe()->tx[0].t_sync_exact);
}

/* A previous window's normal end must not pass for this one's: a chain
 * whose receive op never ran reports FAILED, not the stale TIMEOUT left in
 * the reused command struct. */
ZTEST(radiant_backend_cc26xx_terminal, test_rx_never_ran_failed_not_stale_timeout)
{
	uint32_t op = 0u;
	RF_CmdHandle ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);
	struct fake_rf_cmd *c;

	fake_rf_complete(ch, PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);
	zassert_equal(rx_terminal_status(op), RADIANT_RADIO_STATUS_TIMEOUT);

	ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);
	/* The driver never touches the op's status; undo the fake's PENDING. */
	c = fake_rf_cmd(ch);
	zassert_not_null(c);
	zassert_equal(c->chain[c->n_chain - 1u].op->status, PENDING);
	fake_rf_fire(ch, RF_EventLastCmdDone);
	zassert_equal(rx_terminal_status(op), RADIANT_RADIO_STATUS_FAILED);
}

ZTEST(radiant_backend_cc26xx_terminal, test_rx_prop_error_failed)
{
	uint32_t op = 0u;
	RF_CmdHandle ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);

	fake_rf_complete(ch, PROP_ERROR_PAR, RF_EventLastCmdDone);
	zassert_equal(rx_terminal_status(op), RADIANT_RADIO_STATUS_FAILED);
}

/* Out of receive buffers (the drain fell behind a merged window): the window
 * listened, so it ends TIMEOUT, not FAILED - FAILED would credit a search no
 * dwell and end any transfer riding on it. (Review 2026-11-19, finding 3.) */
ZTEST(radiant_backend_cc26xx_terminal, test_rx_buffer_full_timeout)
{
	static const uint16_t ends[] = { PROP_ERROR_RXBUF, PROP_ERROR_RXFULL };

	for (size_t i = 0u; i < ARRAY_SIZE(ends); i++) {
		uint32_t op = 0u;
		RF_CmdHandle ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);

		fake_rf_complete(ch, ends[i], RF_EventLastCmdDone);
		zassert_equal(rx_terminal_status(op), RADIANT_RADIO_STATUS_TIMEOUT,
			      "end 0x%04x", ends[i]);
	}
}

ZTEST(radiant_backend_cc26xx_terminal, test_rx_generic_error_failed)
{
	uint32_t op = 0u;
	RF_CmdHandle ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);

	fake_rf_complete(ch, ERROR_SYNTH_PROG, RF_EventLastCmdDone);
	zassert_equal(rx_terminal_status(op), RADIANT_RADIO_STATUS_FAILED);
}

/* Every normal receive end keeps TIMEOUT. */
ZTEST(radiant_backend_cc26xx_terminal, test_rx_normal_ends_timeout)
{
	static const uint16_t ends[] = {
		PROP_DONE_OK, PROP_DONE_RXTIMEOUT, PROP_DONE_BREAK,
		PROP_DONE_ENDED, PROP_DONE_RXERR, DONE_OK,
	};

	for (size_t i = 0u; i < ARRAY_SIZE(ends); i++) {
		uint32_t op = 0u;
		RF_CmdHandle ch = arm_rx(radiant_radio_now() + 2000u, 1000u, 0u, &op);

		fake_rf_complete(ch, ends[i], RF_EventLastCmdDone);
		zassert_equal(rx_terminal_status(op), RADIANT_RADIO_STATUS_TIMEOUT,
			      "end 0x%04x", ends[i]);
	}
}

/* A transmit that ended DONE_OK (generic) or PROP_DONE_OK went out. */
ZTEST(radiant_backend_cc26xx_terminal, test_tx_normal_ends_ok)
{
	RF_CmdHandle ch;

	(void)arm_tx(radiant_radio_now() + 2000u, &ch);
	fake_rf_complete(ch, DONE_OK, RF_EventLastCmdDone);
	zassert_equal(cc_probe()->tx[0].status, RADIANT_RADIO_STATUS_OK);
	zassert_true(cc_probe()->tx[0].t_sync_exact);
}

/* ------------------------------------------------------------------------
 * Item 5: a transmit that did not go out reports its requested t_sync
 * ------------------------------------------------------------------------
 * HAL: t_sync is meaningful only on OK; otherwise it is the t_sync_at the
 * transmit was armed for, with t_sync_exact false.
 */

ZTEST(radiant_backend_cc26xx_terminal, test_tx_failed_reports_requested_t_sync)
{
	RF_CmdHandle ch;
	const radiant_time_t t = radiant_radio_now() + 2000u;

	(void)arm_tx(t, &ch);
	fake_rf_complete(ch, ERROR_PAST_START, RF_EventLastCmdDone);
	zassert_equal(cc_probe()->tx[0].status, RADIANT_RADIO_STATUS_FAILED);
	zassert_equal(cc_probe()->tx[0].t_sync, t);
	zassert_false(cc_probe()->tx[0].t_sync_exact);
}

ZTEST(radiant_backend_cc26xx_terminal, test_tx_denied_reports_requested_t_sync)
{
	RF_CmdHandle ch;
	const radiant_time_t t = radiant_radio_now() + 2000u;

	(void)arm_tx(t, &ch);
	fake_rf_fire(ch, RF_EventCmdCancelled | RF_EventCmdPreempted);
	zassert_equal(cc_probe()->tx[0].status, RADIANT_RADIO_STATUS_DENIED);
	zassert_equal(cc_probe()->tx[0].t_sync, t);
	zassert_false(cc_probe()->tx[0].t_sync_exact);
}

ZTEST(radiant_backend_cc26xx_terminal, test_tx_aborted_reports_requested_t_sync)
{
	RF_CmdHandle ch;
	const radiant_time_t t = radiant_radio_now() + 2000u;

	(void)arm_tx(t, &ch);
	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	zassert_equal(cc_probe()->tx_events, 1u);
	zassert_equal(cc_probe()->tx[0].status, RADIANT_RADIO_STATUS_ABORTED);
	zassert_equal(cc_probe()->tx[0].t_sync, t);
	zassert_false(cc_probe()->tx[0].t_sync_exact);
	(void)fake_rf_deliver_pending();
	zassert_equal(cc_probe()->tx_events, 1u);
}

/* ------------------------------------------------------------------------
 * Item 5: an RX terminal reports its requested t_close
 * ------------------------------------------------------------------------
 * HAL: on TIMEOUT/ABORTED/DENIED/FAILED an rx event's t_sync is the window's
 * requested t_close with t_sync_exact false, the mirror of the TX rule. This
 * backend takes no noise sample (RF_getRssi works mid-window only), so
 * has_noise stays false on every terminal, an empty TIMEOUT included.
 */

/* The last rx event, which must be the terminal for `op`. */
static void expect_rx_terminal(uint32_t op, int status, radiant_time_t t_close)
{
	const uint32_t n = cc_probe()->rx_events;
	const struct cc_rx_rec *r;

	zassert_true(n >= 1u && n <= CC_PROBE_LOG);
	r = &cc_probe()->rx[n - 1u];
	zassert_equal(r->op, op);
	zassert_equal(r->status, status);
	zassert_equal(r->t_sync, t_close, "terminal %d reported t_sync %llu, t_close %llu",
		      status, (unsigned long long)r->t_sync, (unsigned long long)t_close);
	zassert_false(r->t_sync_exact);
	zassert_false(r->has_noise, "this backend has no noise sample");
}

ZTEST(radiant_backend_cc26xx_terminal, test_rx_empty_timeout_reports_t_close_no_noise)
{
	uint32_t op = 0u;
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	RF_CmdHandle ch = arm_rx(t_open, 1000u, 0u, &op);

	fake_rf_complete(ch, PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);
	expect_rx_terminal(op, RADIANT_RADIO_STATUS_TIMEOUT, t_open + 1000u);
}

ZTEST(radiant_backend_cc26xx_terminal, test_rx_timeout_after_frame_reports_t_close)
{
	uint32_t op = 0u;
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	RF_CmdHandle ch = arm_rx(t_open, 1000u, 0u, &op);

	fake_rf_start(ch);
	hear_frame(ch, t_open + 100u);
	fake_rf_complete(ch, PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);
	zassert_equal(cc_probe()->rx_events, 2u);
	expect_rx_terminal(op, RADIANT_RADIO_STATUS_TIMEOUT, t_open + 1000u);
}

ZTEST(radiant_backend_cc26xx_terminal, test_rx_aborted_reports_t_close)
{
	uint32_t op = 0u;
	const radiant_time_t t_open = radiant_radio_now() + 2000u;

	(void)arm_rx(t_open, 1000u, 0u, &op);
	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	expect_rx_terminal(op, RADIANT_RADIO_STATUS_ABORTED, t_open + 1000u);
	(void)fake_rf_deliver_pending();
}

ZTEST(radiant_backend_cc26xx_terminal, test_rx_denied_reports_t_close)
{
	uint32_t op = 0u;
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	RF_CmdHandle ch = arm_rx(t_open, 1000u, 0u, &op);

	fake_rf_fire(ch, RF_EventCmdCancelled | RF_EventCmdPreempted);
	expect_rx_terminal(op, RADIANT_RADIO_STATUS_DENIED, t_open + 1000u);
}

ZTEST(radiant_backend_cc26xx_terminal, test_rx_failed_reports_t_close)
{
	uint32_t op = 0u;
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	RF_CmdHandle ch = arm_rx(t_open, 1000u, 0u, &op);

	fake_rf_complete(ch, PROP_ERROR_PAR, RF_EventLastCmdDone);
	expect_rx_terminal(op, RADIANT_RADIO_STATUS_FAILED, t_open + 1000u);
}
