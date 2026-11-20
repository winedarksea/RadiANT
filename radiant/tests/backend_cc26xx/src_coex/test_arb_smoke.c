/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Smoke cases for radiant_radio_cc26xx_arb.c, driven directly with the RF_Cmd
 * records the fake driver keeps (fake_rf_cmd()->cmd) and hand-built foreign
 * commands, as TI's driver would call the hooks. Green on the hooks as they
 * stand.
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <ti/drivers/rf/RF.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_cc26xx_probe.h"
#include "radiant_radio_cc26xx_arb.h"

extern RFCC26XX_SchedulerPolicy RFCC26XX_schedulerPolicy;

RF_ScheduleStatus radiant_cc26xx_submit(RF_Cmd *newCmd, RF_Cmd *pCmdBg,
					 RF_Cmd *pCmdFg, List_List *pPendQueue,
					 List_List *pDoneQueue);
RF_ExecuteAction radiant_cc26xx_execute(RF_Cmd *pCmdBg, RF_Cmd *pCmdFg,
					 List_List *pPendQueue,
					 List_List *pDoneQueue, bool bConflict,
					 RF_Cmd *conflictCmd);

static RF_Object neighbour;
static RF_Cmd foreign_rx;
static List_List pend_q;
static List_List done_q;

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	cc_reset(true);
	/* 802.15.4's background receive: posted, so no end time. */
	memset(&foreign_rx, 0, sizeof(foreign_rx));
	foreign_rx.pClient = &neighbour;
	foreign_rx.startTime = CC_RAT_START;
	foreign_rx.endType = RF_EndNotSpecified;
	List_clearList(&pend_q);
	List_clearList(&done_q);
}

ZTEST_SUITE(radiant_backend_cc26xx_arb, NULL, NULL, before, NULL, NULL);

static struct fake_rf_cmd *arm_rx(uint32_t len_us)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter filt;
	uint32_t op;

	cc_rx_req(&req, &filt, radiant_radio_now() + 2000u, len_us);
	zassert_equal(radiant_radio_rx(&req, &op), RADIANT_RADIO_OK_RC);
	return fake_rf_last();
}

ZTEST(radiant_backend_cc26xx_arb, test_policy_installed)
{
	zassert_equal_ptr(RFCC26XX_schedulerPolicy.submitHook, radiant_cc26xx_submit);
	zassert_equal_ptr(RFCC26XX_schedulerPolicy.executeHook, radiant_cc26xx_execute);
}

/* Under coex every arm is scheduled with an absolute window and a priority
 * from its length: a tracked window HIGH, a scan chunk NORMAL. */
ZTEST(radiant_backend_cc26xx_arb, test_schedule_params)
{
	struct fake_rf_cmd *c = arm_rx(1000u);

	zassert_equal(c->via, FAKE_RF_SCHEDULE);
	zassert_equal(c->sch.startType, RF_StartAbs);
	zassert_equal(c->sch.endType, RF_EndAbs);
	zassert_equal(c->sch.allowDelay, RF_AllowDelayNone);
	zassert_equal(c->cmd.activityInfo, RADIANT_CC26XX_PRIO_HIGH);
	zassert_equal_ptr(c->cmd.pClient, radiant_cc26xx_client());

	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	c = arm_rx(15000u);
	zassert_equal(c->cmd.activityInfo, RADIANT_CC26XX_PRIO_NORMAL);
}

/* Nothing foreign in the way: delegated to the default policy. */
ZTEST(radiant_backend_cc26xx_arb, test_submit_clear_delegates)
{
	struct fake_rf_cmd *c = arm_rx(1000u);

	zassert_equal(radiant_cc26xx_submit(&c->cmd, NULL, NULL, &pend_q, &done_q),
		      RF_ScheduleStatusTail);
	zassert_equal(fake_rf.default_submit_calls, 1u);
}

/* Over the neighbour's endless receive: a tracked window is head-inserted
 * and the execute hook aborts the neighbour for it; a scan chunk is refused. */
ZTEST(radiant_backend_cc26xx_arb, test_submit_over_foreign)
{
	struct fake_rf_cmd *c = arm_rx(1000u);

	zassert_equal(radiant_cc26xx_submit(&c->cmd, &foreign_rx, NULL, &pend_q, &done_q),
		      RF_ScheduleStatusTop);
	zassert_equal_ptr(List_head(&pend_q), (List_Elem *)&c->cmd);
	zassert_equal(fake_rf.default_submit_calls, 0u);
	zassert_equal(radiant_cc26xx_execute(&foreign_rx, NULL, &pend_q, &done_q, true, NULL),
		      RF_ExecuteActionAbortOngoing);

	List_clearList(&pend_q);
	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	c = arm_rx(15000u);
	zassert_equal(radiant_cc26xx_submit(&c->cmd, &foreign_rx, NULL, &pend_q, &done_q),
		      RF_ScheduleStatusError);
	zassert_true(List_empty(&pend_q));
}

/* Our own aborted window (termination stored by the flush, callback still
 * owed) is no obstacle to the next arm, and the execute hook waits for it. */
ZTEST(radiant_backend_cc26xx_arb, test_own_retired_window_ignored)
{
	struct fake_rf_cmd *old = arm_rx(1000u);
	struct fake_rf_cmd *next;

	fake_rf_start(old->cmd.ch);
	zassert_equal(radiant_radio_abort(), RADIANT_RADIO_OK_RC);
	zassert_true((old->cmd.pastifg & RF_EventCmdAborted) != 0u);

	next = arm_rx(1000u);
	zassert_equal(radiant_cc26xx_submit(&next->cmd, &old->cmd, NULL, &pend_q, &done_q),
		      RF_ScheduleStatusTail);
	zassert_equal(fake_rf.default_submit_calls, 1u);

	List_put(&pend_q, (List_Elem *)&next->cmd);
	zassert_equal(radiant_cc26xx_execute(&old->cmd, NULL, &pend_q, &done_q, true, NULL),
		      RF_ExecuteActionNone);
}
