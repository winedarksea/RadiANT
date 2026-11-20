/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The TI backend's arm path against the HAL contract (TODO.md "Backend
 * drift: TI vs nRF", item 1 and two points of item 5):
 *
 *  - rf_index is honoured: an arm on another channel chains a CMD_FS onto
 *    radiant_rf_index_to_mhz(rf_index) ahead of the RX/TX in the same posted
 *    command, the operation itself still starting at its absolute time; the
 *    synthesiser is credited only once that FS ran; an index above
 *    RADIANT_RF_INDEX_MAX is EINVAL with nothing posted.
 *  - a retune needs its settling time: inside the floor plus FS time, ETIME.
 *  - a transmit never goes out late: pastTrig = 0 on the TX start trigger,
 *    and ETIME when its start is already inside the arm floor.
 *  - operation ids skip 0 on wrap, and every failed arm leaves *op == 0.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <ti/drivers/rf/RF.h>
#include <driverlib/rf_prop_mailbox.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_cc26xx_probe.h"

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	cc_reset(true);
}

ZTEST_SUITE(radiant_backend_cc26xx_arm, NULL, NULL, before, NULL, NULL);

static const uint8_t body8[RADIANT_RADIO_BODY_MAX] = {
	0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88, 0x99, 0xAA,
};

#define RF_OTHER 66u   /* 2466 MHz */

static int arm_rx(uint8_t rf, radiant_time_t t_open, uint32_t *op)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter filt;

	cc_rx_req(&req, &filt, t_open, 1000u);
	req.rf_index = rf;
	return radiant_radio_rx(&req, op);
}

static int arm_tx(uint8_t rf, radiant_time_t t_sync, uint32_t *op)
{
	struct radiant_tx_req req;

	cc_tx_req(&req, body8, t_sync);
	req.rf_index = rf;
	return radiant_radio_tx(&req, op);
}

/* Run the operation now armed to its normal end. */
static void finish_last(void)
{
	struct fake_rf_cmd *c = fake_rf_last();
	const bool rx = cc_kind() == 1;

	zassert_not_null(c);
	fake_rf_start(c->cmd.ch);
	fake_rf_complete(c->cmd.ch, rx ? PROP_DONE_RXTIMEOUT : PROP_DONE_OK,
			 RF_EventLastCmdDone);
	zassert_equal(cc_kind(), 0);
}

/* The CMD_FS chained ahead of `main_no` in the newest posted command: it must
 * be the chain's head, the main op right behind it. NULL if no FS. */
static const struct fake_rf_op_snap *chained_fs(uint16_t main_no)
{
	const struct fake_rf_cmd *c = fake_rf_last();
	const struct fake_rf_op_snap *fs;

	zassert_not_null(c);
	fs = fake_rf_find_op(c, CMD_FS);
	if (fs == NULL) {
		zassert_equal(c->n_chain, 1u, "no FS: the main op alone");
		zassert_equal(c->chain[0].command_no, main_no);
		return NULL;
	}
	zassert_equal(c->n_chain, 2u, "FS then the main op");
	zassert_equal_ptr(fs, &c->chain[0], "the FS heads the chain");
	zassert_equal(c->chain[1].command_no, main_no);
	zassert_not_equal(fs->cond_rule, COND_NEVER);
	return fs;
}

/* An arm on rf 66 posts one command: CMD_FS at 2466 MHz (the test build's
 * trim is 0) at an absolute time ahead of the RX, and the RX starting exactly
 * where it would without the retune. */
ZTEST(radiant_backend_cc26xx_arm, test_rx_retune_chains_fs)
{
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	const struct fake_rf_op_snap *fs;
	const struct fake_rf_op_snap *rx;
	uint32_t base_start;
	uint32_t op = 0u;

	zassert_equal(arm_rx(RADIANT_RF_INDEX_ANT_PLUS, t_open, &op), RADIANT_RADIO_OK_RC);
	zassert_is_null(chained_fs(CMD_PROP_RX_ADV), "rf 57 is already tuned");
	base_start = fake_rf_find_op(fake_rf_last(), CMD_PROP_RX_ADV)->start_time;

	cc_reset(true);
	zassert_equal(arm_rx(RF_OTHER, t_open, &op), RADIANT_RADIO_OK_RC);
	fs = chained_fs(CMD_PROP_RX_ADV);
	zassert_not_null(fs, "rf 66 must retune");
	zassert_equal(fs->fs_frequency, radiant_rf_index_to_mhz(RF_OTHER));
	zassert_equal(fs->fs_fract_freq, 0u);
	zassert_equal(fs->trigger_type, TRIG_ABSTIME);
	rx = fake_rf_find_op(fake_rf_last(), CMD_PROP_RX_ADV);
	zassert_equal(rx->trigger_type, TRIG_ABSTIME);
	zassert_equal(rx->start_time, base_start, "the RX keeps its absolute start");
	zassert_true((int32_t)(rx->start_time - fs->start_time) > 0,
		     "the FS starts ahead of the RX");
#if defined(BACKEND_COEX)
	zassert_equal(fake_rf_last()->sch.startTime, fs->start_time,
		      "the scheduler books the air from the FS on");
#endif
}

/* Tuned once, a second arm on the same channel posts no FS; going back to
 * the default retunes to 2457. */
ZTEST(radiant_backend_cc26xx_arm, test_rx_retune_once_and_back)
{
	const struct fake_rf_op_snap *fs;
	uint32_t op = 0u;

	zassert_equal(arm_rx(RF_OTHER, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_not_null(chained_fs(CMD_PROP_RX_ADV));
	finish_last();

	zassert_equal(arm_rx(RF_OTHER, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_is_null(chained_fs(CMD_PROP_RX_ADV), "already on rf 66");
	finish_last();

	zassert_equal(arm_rx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	fs = chained_fs(CMD_PROP_RX_ADV);
	zassert_not_null(fs, "back to rf 57 retunes");
	zassert_equal(fs->fs_frequency, 2457u);
}

/* The same for transmits. */
ZTEST(radiant_backend_cc26xx_arm, test_tx_retune)
{
	const struct fake_rf_op_snap *fs;
	const struct fake_rf_op_snap *tx;
	uint32_t op = 0u;

	zassert_equal(arm_tx(RF_OTHER, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	fs = chained_fs(CMD_PROP_TX_ADV);
	zassert_not_null(fs, "rf 66 must retune");
	zassert_equal(fs->fs_frequency, radiant_rf_index_to_mhz(RF_OTHER));
	tx = fake_rf_find_op(fake_rf_last(), CMD_PROP_TX_ADV);
	zassert_true((int32_t)(tx->start_time - fs->start_time) > 0,
		     "the FS starts ahead of the TX");
	finish_last();
	zassert_equal(cc_probe()->tx[0].status, RADIANT_RADIO_STATUS_OK);

	zassert_equal(arm_tx(RF_OTHER, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_is_null(chained_fs(CMD_PROP_TX_ADV), "already on rf 66");
	finish_last();

	zassert_equal(arm_tx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	fs = chained_fs(CMD_PROP_TX_ADV);
	zassert_not_null(fs, "back to rf 57 retunes");
	zassert_equal(fs->fs_frequency, 2457u);
}

/* rf_index above RADIANT_RF_INDEX_MAX: EINVAL, nothing posted, *op cleared,
 * the slot left free - receive and transmit. */
ZTEST(radiant_backend_cc26xx_arm, test_rf_index_above_max_einval)
{
	const uint32_t posted = fake_rf.n_cmds;
	uint32_t op = 0xDEADBEEFu;

	zassert_equal(arm_rx(RADIANT_RF_INDEX_MAX + 1u, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_EINVAL);
	zassert_equal(op, 0u);
	zassert_equal(fake_rf.n_cmds, posted, "nothing posted");
	zassert_equal(cc_kind(), 0);

	op = 0xDEADBEEFu;
	zassert_equal(arm_tx(RADIANT_RF_INDEX_MAX + 1u, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_EINVAL);
	zassert_equal(op, 0u);
	zassert_equal(fake_rf.n_cmds, posted, "nothing posted");
	zassert_equal(cc_kind(), 0);

	/* The top of the range is a channel like any other. */
	zassert_equal(arm_rx(RADIANT_RF_INDEX_MAX, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_equal(chained_fs(CMD_PROP_RX_ADV)->fs_frequency,
		      radiant_rf_index_to_mhz(RADIANT_RF_INDEX_MAX));
}

/* The synthesiser is credited with rf 66 only once the chained FS ran: an
 * arm aborted before it started, or one whose FS failed, leaves the next arm
 * on rf 66 retuning again. */
ZTEST(radiant_backend_cc26xx_arm, test_retune_credited_only_when_fs_ran)
{
	struct fake_rf_cmd *c;
	uint32_t op = 0u;

	/* Aborted before the RF core reached it: the FS never ran. */
	zassert_equal(arm_rx(RF_OTHER, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_not_null(chained_fs(CMD_PROP_RX_ADV));
	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	(void)fake_rf_deliver_pending();

	zassert_equal(arm_rx(RF_OTHER, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_not_null(chained_fs(CMD_PROP_RX_ADV), "the FS never ran: retune");

	/* This time the chain ended, but the FS reported a synth error. */
	c = fake_rf_last();
	fake_rf_complete(c->cmd.ch, PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);
	c->chain[0].op->status = ERROR_SYNTH_PROG;

	zassert_equal(arm_rx(RF_OTHER, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_not_null(chained_fs(CMD_PROP_RX_ADV), "the FS failed: retune");
	finish_last();

	zassert_equal(arm_rx(RF_OTHER, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_is_null(chained_fs(CMD_PROP_RX_ADV), "that one ran");
}

/* disable/enable starts over on the default channel: enable's blocking CMD_FS
 * is again a lone FS at 2457 (no chain left over from a retune), and rf 66
 * needs a retune again. */
ZTEST(radiant_backend_cc26xx_arm, test_retune_reset_by_reenable)
{
	const struct fake_rf_cmd *run;
	uint32_t op = 0u;

	zassert_equal(arm_rx(RF_OTHER, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_not_null(chained_fs(CMD_PROP_RX_ADV));
	finish_last();

	zassert_equal(radiant_radio_disable(), RADIANT_RADIO_OK_RC);
	zassert_equal(radiant_radio_enable(), RADIANT_RADIO_OK_RC);
	run = fake_rf_nth(fake_rf.n_cmds - 1u);
	zassert_not_null(run);
	zassert_equal(run->via, FAKE_RF_RUN);
	zassert_equal(run->n_chain, 1u, "a lone FS");
	zassert_equal(run->chain[0].command_no, CMD_FS);
	zassert_equal(run->chain[0].fs_frequency, 2457u);
	zassert_equal(run->chain[0].trigger_type, TRIG_NOW);

	zassert_equal(arm_rx(RF_OTHER, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_not_null(chained_fs(CMD_PROP_RX_ADV), "re-enable is back on rf 57");
}

/* A retune owes its settling time: an arm that is fine on the tuned channel
 * is ETIME on another when the FS could not finish first. */
ZTEST(radiant_backend_cc26xx_arm, test_retune_too_close_etime)
{
	const uint32_t posted = fake_rf.n_cmds;
	uint32_t op = 0xDEADBEEFu;

	zassert_equal(arm_rx(RF_OTHER, radiant_radio_now() + 300u, &op),
		      RADIANT_RADIO_ETIME);
	zassert_equal(op, 0u);
	zassert_equal(fake_rf.n_cmds, posted, "nothing posted");
	zassert_equal(cc_kind(), 0);
	zassert_equal(arm_rx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 300u, &op),
		      RADIANT_RADIO_OK_RC, "no retune, no settling owed");
	finish_last();

	op = 0xDEADBEEFu;
	zassert_equal(arm_tx(RF_OTHER, radiant_radio_now() + 450u, &op),
		      RADIANT_RADIO_ETIME);
	zassert_equal(op, 0u);
	zassert_equal(cc_kind(), 0);
	zassert_equal(arm_tx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 450u, &op),
		      RADIANT_RADIO_OK_RC, "no retune, no settling owed");
}

/* A transmit's start trigger has pastTrig = 0: a start the RF core reaches
 * late ends the command (ERROR_PAST_START) instead of sending late. */
ZTEST(radiant_backend_cc26xx_arm, test_tx_past_trig_off)
{
	uint32_t op = 0u;

	zassert_equal(arm_tx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_false(fake_rf_find_op(fake_rf_last(), CMD_PROP_TX_ADV)->past_trig);

	/* With a retune too: the FS may start late, the TX may not. */
	finish_last();
	zassert_equal(arm_tx(RF_OTHER, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_false(fake_rf_find_op(fake_rf_last(), CMD_PROP_TX_ADV)->past_trig);
}

/* A transmit whose start (t_sync less preamble, address and ramp-up) is
 * already inside the arm floor is ETIME, never posted to go out late with
 * an exact t_sync. t_sync itself is past the old t_sync-only floor. */
ZTEST(radiant_backend_cc26xx_arm, test_tx_start_inside_floor_etime)
{
	const uint32_t posted = fake_rf.n_cmds;
	uint32_t op = 0xDEADBEEFu;

	zassert_equal(arm_tx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 200u, &op),
		      RADIANT_RADIO_ETIME);
	zassert_equal(op, 0u);
	zassert_equal(fake_rf.n_cmds, posted, "nothing posted");
	zassert_equal(cc_probe()->tx_events, 0u);
	zassert_equal(cc_kind(), 0);
}

/* Operation ids skip 0 on wrap: 0 means "no operation". */
ZTEST(radiant_backend_cc26xx_arm, test_op_id_skips_zero)
{
	uint32_t op = 0u;

	cc_set_next_op_id(UINT32_MAX);
	zassert_equal(arm_rx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_equal(op, UINT32_MAX);
	finish_last();

	zassert_equal(arm_tx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_not_equal(op, 0u, "the wrap must skip 0");
	zassert_equal(op, cc_op_id());
	finish_last();
	zassert_equal(cc_probe()->tx[0].op, op);
	zassert_not_equal(cc_probe()->tx[0].op, 0u);
}

/* Every failed arm leaves *op == 0, whatever the refusal. */
ZTEST(radiant_backend_cc26xx_arm, test_failed_arm_clears_op)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter filt;
	uint32_t op;

	op = 0xDEADBEEFu;   /* ETIME */
	zassert_equal(arm_tx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 10u, &op),
		      RADIANT_RADIO_ETIME);
	zassert_equal(op, 0u);

	op = 0xDEADBEEFu;   /* EINVAL (window backwards) */
	cc_rx_req(&req, &filt, radiant_radio_now() + 2000u, 1000u);
	req.t_close = req.t_open - 1u;
	zassert_equal(radiant_radio_rx(&req, &op), RADIANT_RADIO_EINVAL);
	zassert_equal(op, 0u);

	op = 0xDEADBEEFu;   /* EDENIED (driver refusal) */
	fake_rf.refuse_posts = 1u;
	zassert_equal(arm_rx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_EDENIED);
	zassert_equal(op, 0u);
	op = 0xDEADBEEFu;
	fake_rf.refuse_posts = 1u;
	zassert_equal(arm_tx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_EDENIED);
	zassert_equal(op, 0u);

	op = 0xDEADBEEFu;   /* EBUSY */
	zassert_equal(arm_rx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	op = 0xDEADBEEFu;
	zassert_equal(arm_tx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 4000u, &op),
		      RADIANT_RADIO_EBUSY);
	zassert_equal(op, 0u);
	op = 0xDEADBEEFu;
	zassert_equal(arm_rx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 4000u, &op),
		      RADIANT_RADIO_EBUSY);
	zassert_equal(op, 0u);
	finish_last();

	op = 0xDEADBEEFu;   /* ESTATE */
	zassert_equal(radiant_radio_disable(), RADIANT_RADIO_OK_RC);
	zassert_equal(arm_tx(RADIANT_RF_INDEX_ANT_PLUS, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_ESTATE);
	zassert_equal(op, 0u);
}

/* The CMD_FS is one static struct shared by every chain. A chain aborted while
 * its FS ran can have that FS finish afterwards and write DONE_OK over the
 * status of a later chain's FS that never ran (that chain was cancelled before
 * the RF core reached it). The later channel must not be credited from it:
 * the synthesiser is on the earlier one. (Review 2026-11-19, finding 2.) */
ZTEST(radiant_backend_cc26xx_arm, test_retune_not_credited_from_stale_fs_status)
{
	struct fake_rf_cmd *c;
	rfc_radioOp_t *fs;
	uint32_t op = 0u;

	/* A: a retune to 66, aborted. */
	zassert_equal(arm_rx(RF_OTHER, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_not_null(chained_fs(CMD_PROP_RX_ADV));
	c = fake_rf_last();
	fs = c->chain[0].op;
	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	(void)fake_rf_deliver_pending();

	/* B: a retune to 70, cancelled before the RF core reached it. */
	zassert_equal(arm_rx(70u, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_not_null(chained_fs(CMD_PROP_RX_ADV));
	zassert_equal_ptr(fake_rf_last()->chain[0].op, fs, "one shared fs_cmd");
	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	(void)fake_rf_deliver_pending();

	/* A's FS completes late, into the shared struct. */
	fs->status = DONE_OK;

	zassert_equal(arm_rx(70u, radiant_radio_now() + 2000u, &op),
		      RADIANT_RADIO_OK_RC);
	zassert_not_null(chained_fs(CMD_PROP_RX_ADV),
			 "B's operation never ran, so neither did its FS: retune");
}
