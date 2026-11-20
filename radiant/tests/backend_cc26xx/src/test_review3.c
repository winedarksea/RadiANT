/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Review 2026-11-20 (third pass, commits ded8169..14c43e8):
 *  1. an operation claimed (abort) while its arm is still inside the post
 *     call is cancelled once its handle is known. That cancel must touch that
 *     one command only: the core may already have armed a replacement from
 *     the ABORTED terminal, and RF_flushCmd would cancel it too;
 *  2. a calibration above the preamble, address and ramp-up airtime (up to
 *     +300 against 198 us for a five-byte address) starts the transmit after
 *     t_sync. Clipping it to t_sync sends the frame early;
 *  3. the advertised arm leads must cover the transmit's own arm floor at
 *     every calibration Kconfig allows, or the scheduler arms at the lead
 *     and gets ETIME.
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

ZTEST_SUITE(radiant_backend_cc26xx_review3, NULL, NULL, before, NULL, NULL);

static int arm_rx(radiant_time_t t_open, uint32_t *op)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter filt;

	cc_rx_req(&req, &filt, t_open, 1000u);
	return radiant_radio_rx(&req, op);
}

static uint32_t rearm_op;
static int rearm_rc;

static void abort_in_post(RF_CmdHandle ch)
{
	ARG_UNUSED(ch);
	fake_rf.on_post = NULL;
	(void)radiant_radio_abort();
}

/* The core re-arming from the ABORTED terminal, as a scheduler pass does. */
static void rearm_on_abort(const struct radiant_rx_event *evt)
{
	if (evt->status == RADIANT_RADIO_STATUS_ABORTED && rearm_op == 0u) {
		rearm_rc = arm_rx(radiant_radio_now() + 3000u, &rearm_op);
	}
}

/* Finding 1: abort() lands inside RF_postCmd and the core arms the next
 * window from its terminal before the first arm learns its handle. */
ZTEST(radiant_backend_cc26xx_review3, test_mid_post_cancel_spares_replacement)
{
	struct fake_rf_cmd *first;
	struct fake_rf_cmd *second;
	uint32_t op1 = 0u;
	const uint32_t n0 = fake_rf.n_cmds;  /* enable() ran its setup and FS */

	rearm_op = 0u;
	rearm_rc = -1;
	fake_rf.on_post = abort_in_post;
	cc_probe()->on_rx = rearm_on_abort;
	zassert_equal(arm_rx(radiant_radio_now() + 2000u, &op1),
		      RADIANT_RADIO_OK_RC);
	cc_probe()->on_rx = NULL;

	zassert_equal(rearm_rc, RADIANT_RADIO_OK_RC, "replacement armed");
	zassert_not_equal(rearm_op, 0u);
	zassert_equal(fake_rf.n_cmds, n0 + 2u);
	first = fake_rf_nth(n0);
	second = fake_rf_nth(n0 + 1u);
	zassert_true(first->cancel_calls > 0u, "the orphan is cancelled");
	zassert_false(first->cancel_flush, "cancel, not flush: later commands live");
	zassert_equal(second->cancel_calls, 0u, "the replacement is untouched");
	zassert_equal(cc_kind(), 1, "the replacement holds the slot");
	zassert_equal(cc_op_id(), rearm_op);
	zassert_equal(cc_cmd_handle(), second->cmd.ch);

	/* The orphan's Cancelled callback is retired, not the replacement's. */
	(void)fake_rf_deliver_pending();
	zassert_equal(cc_probe()->rx_events, 1u);
	zassert_equal(cc_kind(), 1);

	fake_rf_start(second->cmd.ch);
	fake_rf_complete(second->cmd.ch, PROP_DONE_RXTIMEOUT, RF_EventLastCmdDone);
	zassert_equal(cc_probe()->rx_events, 2u);
	zassert_equal(cc_probe()->rx[1].op, rearm_op);
	zassert_equal(cc_probe()->rx[1].status, RADIANT_RADIO_STATUS_TIMEOUT);
}

/* Every calibration the Kconfig range allows, its ends and the clamp edges
 * for a five-byte address (198 us of preamble, address and ramp-up). */
static const int32_t cals[] = { -300, -200, -1, 0, 1, 100, 197, 198, 199, 300 };

/* Finding 2: the start time keeps the sign of its offset. */
ZTEST(radiant_backend_cc26xx_review3, test_tx_start_keeps_positive_cal)
{
	const radiant_time_t t = 5000000u;

	zassert_equal(cc_tx_start_us(t, 5u, 100), t - 98u);
	zassert_equal(cc_tx_start_us(t, 5u, 198), t);
	zassert_equal(cc_tx_start_us(t, 5u, 300), t + 102u,
		      "a +300 calibration starts the command after t_sync");
	zassert_equal(cc_tx_start_us(t, 1u, 300), t + 134u);
	zassert_equal(cc_tx_start_us(t, 5u, -300), t - 498u);
}

/* Finding 3: no transmit the backend accepts needs more lead than it
 * advertises. A retune is covered by min_arm_lead_us; the in-grant lead
 * covers a same-channel follow-on (the reply, the next burst packet). */
ZTEST(radiant_backend_cc26xx_review3, test_tx_floor_within_advertised_lead)
{
	for (size_t i = 0u; i < ARRAY_SIZE(cals); i++) {
		for (uint8_t len = 1u; len <= RADIANT_RADIO_ADDR_MAX; len++) {
			const int32_t cal = cals[i];

			zassert_true(cc_tx_arm_floor_us(len, true, cal) <=
					     cc_min_arm_lead_us(cal),
				     "cal %d len %u retune: floor %u > lead %u",
				     cal, len, cc_tx_arm_floor_us(len, true, cal),
				     cc_min_arm_lead_us(cal));
			zassert_true(cc_tx_arm_floor_us(len, false, cal) <=
					     cc_in_grant_lead_us(cal),
				     "cal %d len %u: floor %u > in-grant %u",
				     cal, len, cc_tx_arm_floor_us(len, false, cal),
				     cc_in_grant_lead_us(cal));
		}
	}
	/* The floor is measured from t_sync, so a start after it costs nothing. */
	zassert_equal(cc_tx_arm_floor_us(5u, false, 300), 150u);
}

/* The leads the caps advertise are the ones the check above covered, and
 * the shipped calibration (+100) keeps the measured 600/400 (coex 1414/600). */
ZTEST(radiant_backend_cc26xx_review3, test_caps_leads_match_build)
{
	const struct radiant_radio_caps *caps = radiant_radio_caps_get();

	zassert_equal(caps->min_arm_lead_us, cc_min_arm_lead_us(cc_tx_cal_us()));
	zassert_equal(caps->min_arm_lead_in_grant_us,
		      cc_in_grant_lead_us(cc_tx_cal_us()));
#if defined(BACKEND_COEX)
	zassert_equal(cc_min_arm_lead_us(100), 1414u);
	zassert_equal(cc_in_grant_lead_us(100), 600u);
#else
	zassert_equal(cc_min_arm_lead_us(100), 600u);
	zassert_equal(cc_in_grant_lead_us(100), 400u);
#endif
	/* A -300 calibration needs 898 us ahead of a retuning transmit. */
	zassert_true(cc_min_arm_lead_us(-300) >= 898u);
}
