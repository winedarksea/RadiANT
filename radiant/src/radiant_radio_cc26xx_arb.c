/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_cc26xx_arb.c - RFCC26XX_schedulerPolicy: the submit hook and
 * execute hook that make arbitration with a second RF_postCmd() client
 * possible at all on this part.
 *
 * Provenance: clean-room, written against TI's public RF driver headers
 * (ti/drivers/rf/RFCC26X2.h) and against RF_defaultSubmitPolicy /
 * RF_defaultExecutionPolicy's OWN behaviour (RFCC26X2_multiMode.c, part of
 * hal_ti, TI's own driver - not sdk-ant, not libant.a). See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * WHY THIS IS A SEPARATE FILE, NOT A HAL SEAM. radiant_radio_nrf_gate.h's
 * pattern (radiant_radio_nrf_gate_direct.c / _gate_mpsl.c behind one HAL
 * backend) does not fit here: RFCC26XX_schedulerPolicy is code the RF
 * DRIVER calls, above BOTH RF core clients, not a HAL entry point below one
 * of them. It must not include radiant_radio_hal.h - its only interface to
 * the backend is radiant_cc26xx_client() (an opaque RF_Handle) and the
 * activityInfo priority tag radiant_radio_cc26xx_arb.h defines, which the
 * backend sets on its own RF_scheduleCmd() calls.
 *
 * ON THE DEFAULT POLICY, ARBITRATION IS NOT WEAK, IT IS IMPOSSIBLE.
 * RF_howToSchedule's step 1 calls RF_verifyGap(newCmd, pCmdBg, pHead), which
 * refuses to place anything ahead of a pCmdBg whose endType ==
 * RF_EndNotSpecified - exactly what the 802.15.4 driver's background
 * CMD_IEEE_RX is (posted via RF_postCmd, which hardcodes that shape). Step 2
 * re-runs the same test down the pending queue and fails identically for the
 * same reason; step 3 needs allowDelay, which our own commands never set
 * (RF_AllowDelayNone - see the backend's post_op()). So the DEFAULT policy
 * rejects every ANT window for as long as 802.15.4 is receiving, which is
 * effectively always (its background RX has no end trigger). This file is
 * what makes an ANT window able to preempt that neighbour.
 *
 * STEP 1 ALSO REFUSES US AGAINST OUR OWN CANCELLED WINDOW. RF_cancelCmd and
 * RF_flushCmd (RF_abortCmd, RFCC26X2_multiMode.c) only store
 * RF_EventCmdAborted/Stopped into the running command's pastifg and ring
 * CMD_ABORT/CMD_STOP; the command stays pCurrCmdBg until RF_hwiCpe0Active
 * takes its LAST_COMMAND_DONE. rx_drain()'s STOP_ON_FIRST early stop cancels
 * and then calls the core synchronously, from the RF SWI, and the core arms
 * the ACK reply / next burst packet right there - and the CPE0 HWI runs at
 * the same NVIC level as the SWI (INT_PRI_LEVEL7, kernel/zephyr/dpl/
 * config.c), so it cannot retire the old window first. RF_verifyGap then
 * measures the new start against the cancelled window's endTime (end +
 * follow_on_us, the very reservation made FOR this reply), fails, and with
 * RF_AllowDelayNone the arm is refused: EDENIED on every chained op. So an
 * own command whose pastifg already holds a termination is passed to both
 * the overlap test and RF_defaultSubmitPolicy as absent - see
 * own_retired(). A foreign one never is.
 */

#include <stdbool.h>
#include <stdint.h>

#include <ti/drivers/utils/List.h>

#include "radiant_radio_cc26xx_arb.h"

static bool is_ours(const RF_Cmd *cmd)
{
	return cmd != NULL && (void *)cmd->pClient == radiant_cc26xx_client();
}

/*
 * One of OUR commands that is still pCmdBg/pCmdFg but has already been told
 * to end: RF_abortCmd stores the termination event into pastifg
 * synchronously, under its own critical section, before ringing the
 * doorbell (RF_cmdStoreEvents), so this is true from the instant
 * RF_cancelCmd/RF_flushCmd returns until the done interrupt retires it.
 *
 * Safe to schedule past: the backend holds at most one live op
 * (arm_common() refuses EBUSY otherwise), so the only own command a new arm
 * can meet here is one already retired; the new op's absolute start trigger
 * has pastTrig = 1 (radiant_radio_rx()/_tx()), so if the abort is still
 * completing at its start time it opens microseconds late instead of
 * failing. A live own command (no termination stored) keeps the full test.
 */
#define OWN_RETIRED_EVENTS (RF_EventCmdAborted | RF_EventCmdStopped | \
			    RF_EventCmdCancelled)

static bool own_retired(const RF_Cmd *cmd)
{
	return is_ours(cmd) && (cmd->pastifg & OWN_RETIRED_EVENTS) != 0;
}

/*
 * Wrap-safe overlap of [a_start, a_end) against a command b's own active
 * span. The RAT is a free-running 32-bit counter (radiant_radio_cc26xx.c's
 * own header explains the same wrap the backend folds to 64 bits for), so
 * every comparison here is a SIGNED DISTANCE, never a raw >/< on the raw
 * ticks - identical reasoning to rx_t_sync()'s fold in the backend.
 *
 * b->endType == RF_EndNotSpecified (802.15.4's background CMD_IEEE_RX, and
 * the only shape RF_postCmd produces) has no meaningful endTime: such a
 * command is active from its own start until something preempts it, so it
 * overlaps any window that has not already finished before that start.
 */
static bool overlaps(uint32_t a_start, uint32_t a_end, const RF_Cmd *b)
{
	if (b->endType == RF_EndNotSpecified) {
		return (int32_t)(a_end - b->startTime) > 0;
	}

	{
		int32_t gap_before = (int32_t)(b->startTime - a_end);
		int32_t gap_after  = (int32_t)(a_start - b->endTime);

		return !(gap_before >= 0 || gap_after >= 0);
	}
}

static bool any_foreign_overlap(const RF_Cmd *newCmd, RF_Cmd *pCmdBg,
				 RF_Cmd *pCmdFg, List_List *pendQueue)
{
	RF_Cmd *it;

	if (pCmdBg != NULL && !is_ours(pCmdBg) &&
	    overlaps(newCmd->startTime, newCmd->endTime, pCmdBg)) {
		return true;
	}
	if (pCmdFg != NULL && !is_ours(pCmdFg) &&
	    overlaps(newCmd->startTime, newCmd->endTime, pCmdFg)) {
		return true;
	}
	for (it = (RF_Cmd *)List_head(pendQueue); it != NULL;
	     it = (RF_Cmd *)List_next((List_Elem *)it)) {
		if (!is_ours(it) &&
		    overlaps(newCmd->startTime, newCmd->endTime, it)) {
			return true;
		}
	}

	return false;
}

/*
 * newCmd->pClient != our handle: DELEGATE VERBATIM to
 * RF_defaultSubmitPolicy. Not "improve" the neighbour's scheduling - the
 * whole point of coexistence here is that the 802.15.4 driver is unmodified
 * except for the re-post fix in coex154/ (a different problem: its command
 * never surviving preemption, not how it gets scheduled).
 *
 * Ours: no overlap with anything foreign -> the ordinary case, delegated to
 * RF_defaultSubmitPolicy so it inserts in time order and returns Top/Middle
 * exactly as the non-coex build's RF_postCmd path effectively would.
 * Overlap -> our own priority rule, not RF_verifyGap's: PRIO_NORMAL (sweep,
 * scan, ED - the elastic consumer, ADR 0013) is refused; PRIO_HIGH (tracked
 * slot, transmit) is head-inserted so the execute hook gets a chance to
 * preempt what is running.
 */
RF_ScheduleStatus radiant_cc26xx_submit(RF_Cmd *newCmd, RF_Cmd *pCmdBg,
					 RF_Cmd *pCmdFg, List_List *pPendQueue,
					 List_List *pDoneQueue)
{
	if (!is_ours(newCmd)) {
		return RF_defaultSubmitPolicy(newCmd, pCmdBg, pCmdFg,
					       pPendQueue, pDoneQueue);
	}

	/* See the header: our own cancelled window is not an obstacle. NULL is
	 * what RF_howToSchedule/RF_verifyGap already see with nothing running
	 * (verifyGap treats a NULL prevCmd as "nothing before"; pCmdFg is not
	 * read by the default policy at all). */
	if (own_retired(pCmdBg)) {
		pCmdBg = NULL;
	}
	if (own_retired(pCmdFg)) {
		pCmdFg = NULL;
	}

	if (any_foreign_overlap(newCmd, pCmdBg, pCmdFg, pPendQueue)) {
		if (newCmd->activityInfo == RADIANT_CC26XX_PRIO_NORMAL) {
			return RF_ScheduleStatusError;
		}
		List_putHead(pPendQueue, (List_Elem *)newCmd);
		return RF_ScheduleStatusTop;
	}

	return RF_defaultSubmitPolicy(newCmd, pCmdBg, pCmdFg, pPendQueue,
				       pDoneQueue);
}

/*
 * RF_dispatchNextCmd only ever displaces a running command through this
 * hook returning AbortOngoing - RF_defaultExecutionPolicy returns
 * ExecuteActionNone unconditionally, so head-of-queue plus the default
 * execute hook would mean waiting behind an infinite RX forever. This is
 * the other half of what the submit hook's head-insert needs to matter.
 *
 * Only abort for OUR OWN head-of-queue PRIO_HIGH command - never abort the
 * neighbour's command to make room for one of OUR PRIO_NORMAL requests
 * (the submit hook already refused those on overlap) and never abort on the
 * neighbour's behalf, which would be "improving" its scheduling again.
 *
 * And only when what is running is FOREIGN. With the submit hook now letting
 * an arm queue behind our own cancelled window (header), the dispatcher can
 * call this with that window still pCmdBg and our new op at the head. Its
 * AbortOngoing is RF_abortCmd(pCurrCmdBg->pClient, ch, false, flush = true,
 * preempt = true): with pClient == us the flush walks the pend queue and
 * discards OUR head - the op just armed - with Cancelled|Preempted, which
 * the backend reports as an async DENIED. The window is already aborting;
 * waiting (ExecuteActionNone) lets its done interrupt dispatch the new op.
 * A NULL pCmdBg is also excluded: the dispatcher dereferences it.
 */
RF_ExecuteAction radiant_cc26xx_execute(RF_Cmd *pCmdBg, RF_Cmd *pCmdFg,
					 List_List *pPendQueue,
					 List_List *pDoneQueue, bool bConflict,
					 RF_Cmd *conflictCmd)
{
	RF_Cmd *pHead = (RF_Cmd *)List_head(pPendQueue);

	if (bConflict && pCmdBg != NULL && !is_ours(pCmdBg) &&
	    pHead != NULL && is_ours(pHead) &&
	    pHead->activityInfo == RADIANT_CC26XX_PRIO_HIGH) {
		return RF_ExecuteActionAbortOngoing;
	}

	return RF_defaultExecutionPolicy(pCmdBg, pCmdFg, pPendQueue,
					  pDoneQueue, bConflict, conflictCmd);
}

/*
 * Installed by definition alone - the driver's own RFCC26XX_schedulerPolicy
 * is __attribute__((weak)), so this strong definition replaces it with no
 * Kconfig glue beyond compiling this file in (CMakeLists.txt, under
 * CONFIG_RADIANT_BACKEND_CC26XX_COEX).
 */
RFCC26XX_SchedulerPolicy RFCC26XX_schedulerPolicy = {
	.submitHook  = radiant_cc26xx_submit,
	.executeHook = radiant_cc26xx_execute,
};
