/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Review 2026-11-19 (second pass) findings 1, 3 and 4:
 *  - a refused arm that chained a retune leaves the shared CMD_FS on the new
 *    channel, which the driver may cache and replay at power-up, so the
 *    backend must stop trusting its tuned channel;
 *  - an operation claimed (abort) while its arm is still inside the post call
 *    has no handle yet: the backend must never pass NO_HANDLE (-1, the
 *    driver's RF_CMDHANDLE_FLUSH_ALL) to a cancel, and must cancel the real
 *    handle once known;
 *  - an operation whose terminal arrived inside the post call must not leave
 *    its handle behind in the slot.
 */
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

ZTEST_SUITE(radiant_backend_cc26xx_review2, NULL, NULL, before, NULL, NULL);

static int arm_rx(uint8_t rf, uint32_t *op)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter filt;

	cc_rx_req(&req, &filt, radiant_radio_now() + 2000u, 1000u);
	req.rf_index = rf;
	return radiant_radio_rx(&req, op);
}

/* Finding 1: refused retune to 66, then an arm back on 57 must chain an FS
 * (the CMD_FS struct the driver may have cached now says 2466). */
ZTEST(radiant_backend_cc26xx_review2, test_refused_retune_forces_fs_next_arm)
{
	const struct fake_rf_op_snap *fs;
	uint32_t op = 0u;

	fake_rf.refuse_posts = 1u;
	zassert_equal(arm_rx(66u, &op), RADIANT_RADIO_EDENIED);
	zassert_equal(cc_kind(), 0);

	zassert_equal(arm_rx(RADIANT_RF_INDEX_ANT_PLUS, &op), RADIANT_RADIO_OK_RC);
	fs = fake_rf_find_op(fake_rf_last(), CMD_FS);
	zassert_not_null(fs, "the refused retune left CMD_FS on 2466: re-tune");
	zassert_equal(fs->fs_frequency,
		      radiant_rf_index_to_mhz(RADIANT_RF_INDEX_ANT_PLUS));
}

static void abort_in_post(RF_CmdHandle ch)
{
	ARG_UNUSED(ch);
	fake_rf.on_post = NULL;
	(void)radiant_radio_abort();
}

/* Finding 3: abort() lands while the arm is inside RF_postCmd. */
ZTEST(radiant_backend_cc26xx_review2, test_abort_mid_post_cancels_real_handle)
{
	struct fake_rf_cmd *c;
	uint32_t op = 0u;

	fake_rf.on_post = abort_in_post;
	zassert_equal(arm_rx(RADIANT_RF_INDEX_ANT_PLUS, &op), RADIANT_RADIO_OK_RC);
	c = fake_rf_last();
	zassert_not_null(c);

	zassert_equal(fake_rf.flush_all_calls, 0u, "never cancel handle -1");
	zassert_equal(cc_probe()->rx_events, 1u);
	zassert_equal(cc_probe()->rx[0].status, RADIANT_RADIO_STATUS_ABORTED);
	zassert_equal(cc_probe()->rx[0].op, op);
	zassert_true(c->cancel_calls > 0u, "the posted command is cancelled");
	zassert_equal(cc_kind(), 0);
	zassert_equal(cc_cmd_handle(), -1, "no handle left in the free slot");

	/* Its own cancel callback is retired: no second terminal. */
	fake_rf_deliver_pending();
	zassert_equal(cc_probe()->rx_events, 1u);

	/* The slot works afterwards. */
	zassert_equal(arm_rx(RADIANT_RF_INDEX_ANT_PLUS, &op), RADIANT_RADIO_OK_RC);
	c = fake_rf_last();
	fake_rf_start(c->cmd.ch);
	fake_rf_complete(c->cmd.ch, PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);
	zassert_equal(cc_probe()->rx_events, 2u);
	zassert_equal(cc_probe()->rx[1].status, RADIANT_RADIO_STATUS_TIMEOUT);
}

static void end_in_post(RF_CmdHandle ch)
{
	fake_rf_start(ch);
	fake_rf_complete(ch, PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);
}

/* Finding 4: the command's own terminal arrives inside RF_postCmd, twice in
 * a row. The first must not leave its handle in the slot, or the second's
 * early terminal looks foreign and is lost. */
ZTEST(radiant_backend_cc26xx_review2, test_terminal_mid_post_no_stale_handle)
{
	uint32_t op = 0u;

	fake_rf.on_post = end_in_post;
	zassert_equal(arm_rx(RADIANT_RF_INDEX_ANT_PLUS, &op), RADIANT_RADIO_OK_RC);
	zassert_equal(cc_probe()->rx_events, 1u);
	zassert_equal(cc_kind(), 0);
	zassert_equal(cc_cmd_handle(), -1, "no handle left in the free slot");

	zassert_equal(arm_rx(RADIANT_RF_INDEX_ANT_PLUS, &op), RADIANT_RADIO_OK_RC);
	zassert_equal(cc_probe()->rx_events, 2u, "second early terminal delivered");
	zassert_equal(cc_probe()->rx[1].status, RADIANT_RADIO_STATUS_TIMEOUT);
	zassert_equal(cc_probe()->rx[1].op, op);
	zassert_equal(cc_kind(), 0);
	fake_rf.on_post = NULL;
}
