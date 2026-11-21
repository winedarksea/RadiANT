/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Smoke cases for the harness: the backend's ordinary paths against the fake
 * RF driver, green on the backend as it stands. They pin what the harness
 * itself can see (what was posted, what the core was told) so later suites
 * can build red-then-green cases on it.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <ti/drivers/rf/RF.h>
#include <driverlib/rf_prop_mailbox.h>
#include <rf_patches/rf_patch_cpe_prop.h>
#include <rf_patches/rf_patch_cpe_multi_protocol.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_cc26xx_probe.h"

#if defined(BACKEND_COEX)
#define CC_VIA FAKE_RF_SCHEDULE
#else
#define CC_VIA FAKE_RF_POST
#endif

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	cc_reset(true);
}

ZTEST_SUITE(radiant_backend_cc26xx_smoke, NULL, NULL, before, NULL, NULL);

static const uint8_t body8[RADIANT_RADIO_BODY_MAX] = {
	0x10, 0x20, 0x30, 0x40, 0x50, 0x60, 0x70, 0x80, 0x90, 0xA0,
};

/* init + enable: one RF_open with the setup command and the patch the build
 * names, one blocking CMD_FS onto the default channel, nothing posted. */
ZTEST(radiant_backend_cc26xx_smoke, test_init_enable)
{
	const struct fake_rf_cmd *fs = fake_rf_nth(0);

	zassert_equal(fake_rf.opens, 1u);
	zassert_true(fake_rf.open);
	zassert_not_null(fake_rf.setup);
	zassert_equal(fake_rf.setup->prop_div_pa.commandNo, CMD_PROP_RADIO_DIV_SETUP);
	zassert_equal(fake_rf.params.nInactivityTimeout, UINT32_MAX);
#if defined(BACKEND_COEX)
	zassert_equal_ptr(fake_rf.mode->cpePatchFxn, rf_patch_cpe_multi_protocol);
#else
	zassert_equal_ptr(fake_rf.mode->cpePatchFxn, rf_patch_cpe_prop);
#endif

	zassert_equal(fake_rf.n_cmds, 1u, "only the CMD_FS ran");
	zassert_not_null(fs);
	zassert_equal(fs->via, FAKE_RF_RUN);
	zassert_equal(fs->chain[0].command_no, CMD_FS);
	zassert_equal(fs->chain[0].fs_frequency, 2457u);
	zassert_is_null(fake_rf_last(), "nothing posted");

	zassert_equal(cc_kind(), 0);
	zassert_equal(strcmp(radiant_radio_caps_get()->name, "cc26xx"), 0);
	/* now() reads the fake RAT: 4 ticks per microsecond. */
	zassert_equal(radiant_radio_now(), CC_RAT_START / 4u);
}

/* A receive: posted as one PROP_RX_ADV with absolute, pastTrig triggers
 * around the window; a frame in the window reaches the core with its body,
 * filter and t_sync; LastCmdDone ends it with TIMEOUT. */
ZTEST(radiant_backend_cc26xx_smoke, test_rx_frame_then_timeout)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter filt;
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	const radiant_time_t t_frame = t_open + 300u;
	struct fake_rf_cmd *c;
	const struct fake_rf_op_snap *rx;
	uint8_t pl[RADIANT_RADIO_ADDR_MAX + RADIANT_RADIO_BODY_MAX + 2];
	uint8_t n;
	uint32_t op = 0u;

	cc_rx_req(&req, &filt, t_open, 1000u);
	zassert_equal(radiant_radio_rx(&req, &op), RADIANT_RADIO_OK_RC);
	zassert_equal(op, cc_op_id());
	zassert_equal(cc_kind(), 1);

	c = fake_rf_last();
	zassert_not_null(c);
	zassert_equal(c->via, CC_VIA);
	zassert_equal(c->cmd.ch, cc_cmd_handle());
	rx = fake_rf_find_op(c, CMD_PROP_RX_ADV);
	zassert_not_null(rx);
	zassert_equal(rx->trigger_type, TRIG_ABSTIME);
	zassert_true(rx->past_trig);
	zassert_true(rx->start_time < (uint32_t)(t_open * 4u), "listening before t_open");
	zassert_equal(rx->end_trigger_type, TRIG_ABSTIME);
	zassert_true(rx->end_time > (uint32_t)(req.t_close * 4u), "still listening at t_close");
	zassert_equal(rx->sync_word0,
		      ((uint32_t)cc_addr[0] << 16) | ((uint32_t)cc_addr[1] << 8) | cc_addr[2]);
	zassert_equal(rx->sync_word1, 0u, "one group: second correlator off");

	n = cc_payload(cc_addr, cc_hw_addr_len(), body8, false, pl);
	zassert_equal(fake_rf_rx_entry(c->cmd.ch, pl, n, -55, cc_rat_for_t_sync(t_frame), 0u), 0);
	fake_rf_advance_us(2300u);
	fake_rf_fire(c->cmd.ch, RF_EventRxEntryDone);

	zassert_equal(cc_probe()->rx_events, 1u);
	zassert_equal(cc_probe()->rx[0].status, RADIANT_RADIO_STATUS_OK);
	zassert_equal(cc_probe()->rx[0].op, op);
	zassert_equal(cc_probe()->rx[0].filter_index, 0u);
	zassert_equal(cc_probe()->rx[0].t_sync, t_frame);
	zassert_true(cc_probe()->rx[0].t_sync_exact);
	zassert_equal(cc_probe()->rx[0].body_len, cc_fmt()->body_len);
	zassert_mem_equal(cc_probe()->rx[0].body, body8, cc_fmt()->body_len);
	zassert_equal(cc_probe()->rx[0].rssi_dbm, -55);
	zassert_equal(cc_kind(), 1, "a frame does not end the window");

	fake_rf_complete(c->cmd.ch, PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);

	zassert_equal(cc_probe()->rx_events, 2u);
	zassert_equal(cc_probe()->rx[1].status, RADIANT_RADIO_STATUS_TIMEOUT);
	zassert_equal(cc_probe()->rx[1].op, op);
	zassert_equal(cc_kind(), 0);
	zassert_equal(strcmp(cc_probe()->order, "RR"), 0);
}

/* A frame whose CRC fails is dropped (no REPORT_CRC_FAIL), and one whose
 * software-compared address bytes differ is counted, not delivered. */
ZTEST(radiant_backend_cc26xx_smoke, test_rx_bad_frames_dropped)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter filt;
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	uint8_t other[RADIANT_RADIO_ADDR_MAX];
	uint8_t pl[RADIANT_RADIO_ADDR_MAX + RADIANT_RADIO_BODY_MAX + 2];
	uint8_t n;
	uint32_t op;
	RF_CmdHandle ch;

	cc_rx_req(&req, &filt, t_open, 1000u);
	zassert_equal(radiant_radio_rx(&req, &op), RADIANT_RADIO_OK_RC);
	ch = fake_rf_last()->cmd.ch;

	n = cc_payload(cc_addr, cc_hw_addr_len(), body8, true, pl);
	zassert_equal(fake_rf_rx_entry(ch, pl, n, -60, cc_rat_for_t_sync(t_open + 100u), 0u), 0);
	memcpy(other, cc_addr, sizeof(other));
	other[RADIANT_RADIO_ADDR_MAX - 1] ^= 0xFFu;
	n = cc_payload(other, cc_hw_addr_len(), body8, false, pl);
	zassert_equal(fake_rf_rx_entry(ch, pl, n, -60, cc_rat_for_t_sync(t_open + 200u), 0u), 0);
	fake_rf_fire(ch, RF_EventRxEntryDone);

	zassert_equal(cc_probe()->rx_events, 0u);
	zassert_equal(cc_sw_addr_mismatch(), 1u);

	fake_rf_complete(ch, PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);
	zassert_equal(cc_probe()->rx_events, 1u);
	zassert_equal(cc_probe()->rx[0].status, RADIANT_RADIO_STATUS_TIMEOUT);
}

/* A transmit: one PROP_TX_ADV carrying the software address bytes, body and
 * CRC; LastCmdDone reports OK with the requested t_sync, exact. */
ZTEST(radiant_backend_cc26xx_smoke, test_tx_ok)
{
	struct radiant_tx_req req;
	const radiant_time_t t_sync = radiant_radio_now() + 2000u;
	struct fake_rf_cmd *c;
	const struct fake_rf_op_snap *tx;
	uint32_t op = 0u;

	cc_tx_req(&req, body8, t_sync);
	zassert_equal(radiant_radio_tx(&req, &op), RADIANT_RADIO_OK_RC);
	zassert_equal(cc_kind(), 2);

	c = fake_rf_last();
	zassert_not_null(c);
	zassert_equal(c->via, CC_VIA);
	tx = fake_rf_find_op(c, CMD_PROP_TX_ADV);
	zassert_not_null(tx);
	zassert_equal(tx->trigger_type, TRIG_ABSTIME);
	zassert_false(tx->past_trig, "a transmit never starts late");
	zassert_true(tx->start_time < (uint32_t)(t_sync * 4u), "starts before t_sync");
	zassert_equal(tx->pkt_len, (cc_fmt()->addr_len - cc_hw_addr_len()) +
				   cc_fmt()->body_len + 2u);
	zassert_equal(tx->sync_word,
		      ((uint32_t)cc_addr[0] << 16) | ((uint32_t)cc_addr[1] << 8) | cc_addr[2]);

	fake_rf_complete(c->cmd.ch, PROP_DONE_OK, RF_EventLastCmdDone);

	zassert_equal(cc_probe()->tx_events, 1u);
	zassert_equal(cc_probe()->tx[0].status, RADIANT_RADIO_STATUS_OK);
	zassert_equal(cc_probe()->tx[0].op, op);
	zassert_equal(cc_probe()->tx[0].t_sync, t_sync);
	zassert_true(cc_probe()->tx[0].t_sync_exact);
	zassert_equal(cc_kind(), 0);
}

/* abort(): ABORTED delivered synchronously, the flush recorded but its
 * callback still owed; delivering it later reaches no one, and the next arm
 * is taken and ends normally. */
ZTEST(radiant_backend_cc26xx_smoke, test_abort_rx)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter filt;
	struct fake_rf_cmd *c;
	RF_CmdHandle ch;
	uint32_t op = 0u;
	uint32_t op2 = 0u;

	cc_rx_req(&req, &filt, radiant_radio_now() + 2000u, 1000u);
	zassert_equal(radiant_radio_rx(&req, &op), RADIANT_RADIO_OK_RC);
	ch = fake_rf_last()->cmd.ch;
	fake_rf_start(ch);

	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	zassert_equal(cc_probe()->rx_events, 1u);
	zassert_equal(cc_probe()->rx[0].status, RADIANT_RADIO_STATUS_ABORTED);
	zassert_equal(cc_probe()->rx[0].op, op);
	zassert_equal(cc_kind(), 0);
	zassert_equal(fake_rf.flush_calls, 1u);
	zassert_equal(fake_rf_pending(), 1u, "the cancel callback is asynchronous");

	/* The next operation is armed before the old callback arrives. */
	cc_rx_req(&req, &filt, radiant_radio_now() + 2000u, 1000u);
	zassert_equal(radiant_radio_rx(&req, &op2), RADIANT_RADIO_OK_RC);
	zassert_not_equal(op2, op);

	zassert_equal(fake_rf_deliver_pending(), 1u);
	c = fake_rf_cmd(ch);
	zassert_true((c->fired & RF_EventCmdAborted) != 0u);
	zassert_equal(cc_probe()->rx_events, 1u, "the late cancel reached no one");
	zassert_equal(cc_kind(), 1, "the new window is still live");

	fake_rf_complete(fake_rf_last()->cmd.ch, PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);
	zassert_equal(cc_probe()->rx_events, 2u);
	zassert_equal(cc_probe()->rx[1].status, RADIANT_RADIO_STATUS_TIMEOUT);
	zassert_equal(cc_probe()->rx[1].op, op2);
}

ZTEST(radiant_backend_cc26xx_smoke, test_abort_tx)
{
	struct radiant_tx_req req;
	uint32_t op = 0u;

	cc_tx_req(&req, body8, radiant_radio_now() + 2000u);
	zassert_equal(radiant_radio_tx(&req, &op), RADIANT_RADIO_OK_RC);

	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	zassert_equal(cc_probe()->tx_events, 1u);
	zassert_equal(cc_probe()->tx[0].status, RADIANT_RADIO_STATUS_ABORTED);
	zassert_equal(cc_probe()->tx[0].op, op);
	zassert_false(cc_probe()->tx[0].t_sync_exact);
	zassert_equal(fake_rf_pending(), 1u);

	zassert_equal(fake_rf_deliver_pending(), 1u);
	zassert_equal(cc_probe()->tx_events, 1u);
	zassert_equal(cc_kind(), 0);
}

/* The arm refusals the harness can drive: too late (ETIME, nothing posted)
 * and a driver refusal (EDENIED, the slot left free). */
ZTEST(radiant_backend_cc26xx_smoke, test_arm_refusals)
{
	struct radiant_tx_req req;
	uint32_t op;
	const uint32_t posted = fake_rf.n_cmds;

	cc_tx_req(&req, body8, radiant_radio_now() + 10u);
	zassert_equal(radiant_radio_tx(&req, &op), RADIANT_RADIO_ETIME);
	zassert_equal(fake_rf.n_cmds, posted);

	fake_rf.refuse_posts = 1u;
	cc_tx_req(&req, body8, radiant_radio_now() + 2000u);
	zassert_equal(radiant_radio_tx(&req, &op), RADIANT_RADIO_EDENIED);
	zassert_equal(fake_rf.refusals, 1u);
	zassert_equal(cc_kind(), 0);

	zassert_equal(radiant_radio_tx(&req, &op), RADIANT_RADIO_OK_RC);
}

/* ------------------------------------------------------------------------
 * HAL item 5: window edges, lifecycle and request error codes
 * ------------------------------------------------------------------------
 */

/* t_sync always lies in [t_open, t_close]: the receiver runs early and late,
 * so a frame one microsecond before t_open is dropped in software (and
 * counted where the 0xF7 counters are built); one at exactly t_close is
 * delivered. */
ZTEST(radiant_backend_cc26xx_smoke, test_rx_window_edges)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter filt;
	const radiant_time_t t_open = radiant_radio_now() + 2000u;
	uint8_t pl[RADIANT_RADIO_ADDR_MAX + RADIANT_RADIO_BODY_MAX + 2];
	const uint32_t drops0 = cc_window_drops();
	uint32_t op = 0u;
	uint8_t n;
	RF_CmdHandle ch;

	cc_rx_req(&req, &filt, t_open, 1000u);
	zassert_equal(radiant_radio_rx(&req, &op), RADIANT_RADIO_OK_RC);
	ch = fake_rf_last()->cmd.ch;

	n = cc_payload(cc_addr, cc_hw_addr_len(), body8, false, pl);
	zassert_equal(fake_rf_rx_entry(ch, pl, n, -55, cc_rat_for_t_sync(t_open - 1u), 0u), 0);
	zassert_equal(fake_rf_rx_entry(ch, pl, n, -55, cc_rat_for_t_sync(req.t_close), 0u), 0);
	fake_rf_fire(ch, RF_EventRxEntryDone);

	zassert_equal(cc_probe()->rx_events, 1u, "only the frame at t_close");
	zassert_equal(cc_probe()->rx[0].status, RADIANT_RADIO_STATUS_OK);
	zassert_equal(cc_probe()->rx[0].t_sync, req.t_close);
	zassert_true(cc_probe()->rx[0].t_sync_exact);
	if (cc_diag_built()) {
		zassert_equal(cc_window_drops() - drops0, 1u);
	}
	zassert_equal(cc_kind(), 1);
}

/* init: a missing table or callback is EINVAL; a second init without a
 * fresh boot is ESTATE and leaves the open radio and the live operation
 * alone (it used to memset the state, losing the RF handle). abort: ESTATE
 * before init, OK after init with nothing armed. */
ZTEST(radiant_backend_cc26xx_smoke, test_init_and_abort_lifecycle_codes)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter filt;
	uint32_t op = 0u;

	cc_rx_req(&req, &filt, radiant_radio_now() + 2000u, 1000u);
	zassert_equal(radiant_radio_rx(&req, &op), RADIANT_RADIO_OK_RC);
	zassert_equal(cc_init_call(3), RADIANT_RADIO_ESTATE);
	zassert_true(cc_rf_open(), "a second init dropped the RF handle");
	zassert_equal(cc_kind(), 1, "a second init dropped the live window");
	zassert_equal(cc_op_id(), op);

	cc_forget_init();
	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_ESTATE);
	zassert_equal(cc_init_call(0), RADIANT_RADIO_EINVAL);
	zassert_equal(cc_init_call(1), RADIANT_RADIO_EINVAL);
	zassert_equal(cc_init_call(2), RADIANT_RADIO_EINVAL);
	zassert_equal(cc_init_call(3), RADIANT_RADIO_OK_RC);
	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
}

/* A NULL format or a zero body is malformed (EINVAL), not a format this
 * backend cannot express (ENOTSUP), on both arm calls. */
ZTEST(radiant_backend_cc26xx_smoke, test_malformed_format_is_einval)
{
	struct radiant_rx_req rx;
	struct radiant_rx_filter filt;
	struct radiant_tx_req tx;
	struct radiant_pkt_format empty = *cc_fmt();
	uint32_t op = 1u;

	empty.body_len = 0u;

	cc_rx_req(&rx, &filt, radiant_radio_now() + 2000u, 1000u);
	rx.fmt = NULL;
	zassert_equal(radiant_radio_rx(&rx, &op), RADIANT_RADIO_EINVAL);
	zassert_equal(op, 0u);
	rx.fmt = &empty;
	zassert_equal(radiant_radio_rx(&rx, &op), RADIANT_RADIO_EINVAL);

	cc_tx_req(&tx, body8, radiant_radio_now() + 2000u);
	tx.fmt = NULL;
	zassert_equal(radiant_radio_tx(&tx, &op), RADIANT_RADIO_EINVAL);
	tx.fmt = &empty;
	tx.body_len = 0u;
	zassert_equal(radiant_radio_tx(&tx, &op), RADIANT_RADIO_EINVAL);
	zassert_equal(cc_kind(), 0);
	zassert_is_null(fake_rf_last(), "nothing posted");
}

/* A window longer than a non-zero caps.max_window_us is ENOTSUP, refused
 * before anything reaches the RF driver's scheduler. Only the coex build
 * advertises a ceiling; the plain build takes the same window. */
ZTEST(radiant_backend_cc26xx_smoke, test_window_over_the_ceiling)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter filt;
	const uint32_t max = radiant_radio_caps_get()->max_window_us;
	const uint32_t posted = fake_rf.n_cmds;
	uint32_t op = 1u;

#if defined(BACKEND_COEX)
	zassert_equal(max, 20000u);
	cc_rx_req(&req, &filt, radiant_radio_now() + 3000u, max + 1u);
	zassert_equal(radiant_radio_rx(&req, &op), RADIANT_RADIO_ENOTSUP);
	zassert_equal(op, 0u);
	zassert_equal(fake_rf.n_cmds, posted, "the over-long window was posted");
	zassert_equal(cc_kind(), 0);
	cc_rx_req(&req, &filt, radiant_radio_now() + 3000u, max);
	zassert_equal(radiant_radio_rx(&req, &op), RADIANT_RADIO_OK_RC);
#else
	zassert_equal(max, 0u);
	ARG_UNUSED(posted);
	cc_rx_req(&req, &filt, radiant_radio_now() + 3000u, 50000u);
	zassert_equal(radiant_radio_rx(&req, &op), RADIANT_RADIO_OK_RC);
#endif
}
