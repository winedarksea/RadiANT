/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fake_rf.c - the recording RF driver behind fakes/ti/drivers/rf/RF.h. The
 * hook API is described in that header; this file is only its implementation.
 */

#include <string.h>

#include <zephyr/sys/util.h>

#include <ti/drivers/rf/RF.h>
#include <driverlib/rf_prop_mailbox.h>
#include <rf_patches/rf_patch_cpe_prop.h>
#include <rf_patches/rf_patch_cpe_multi_protocol.h>

/* The patch entry points the backend names (rf_mode.cpePatchFxn). */
void rf_patch_cpe_prop(void) {}
void rf_patch_cpe_multi_protocol(void) {}

struct fake_rf_state fake_rf;

static struct fake_rf_cmd cmds[FAKE_RF_MAX_CMDS];

static struct {
	RF_CmdHandle ch;
	RF_EventMask events;
} pend[FAKE_RF_PENDING_MAX];
static uint32_t pend_head;
static uint32_t pend_n;

#define TERM_EVENTS (RF_EventLastCmdDone | RF_EventLastFGCmdDone | \
		     RF_EventCmdAborted | RF_EventCmdStopped |      \
		     RF_EventCmdCancelled)

void fake_rf_reset(void)
{
	memset(&fake_rf, 0, sizeof(fake_rf));
	memset(cmds, 0, sizeof(cmds));
	memset(pend, 0, sizeof(pend));
	pend_head = 0u;
	pend_n = 0u;
	fake_rf.run_result = RF_EventLastCmdDone;
	fake_rf.refuse_rc = RF_SCHEDULE_CMD_ERROR;
	fake_rf.radio_active = true;
	fake_rf.set_tx_power_rc = RF_StatSuccess;
	fake_rf.default_submit_rc = RF_ScheduleStatusTail;
	fake_rf.default_execute_rc = RF_ExecuteActionNone;
}

/* ---- Records ---- */

struct fake_rf_cmd *fake_rf_nth(uint32_t i)
{
	if (i >= fake_rf.n_cmds || fake_rf.n_cmds - i > FAKE_RF_MAX_CMDS) {
		return NULL;
	}
	return &cmds[i % FAKE_RF_MAX_CMDS];
}

struct fake_rf_cmd *fake_rf_cmd(RF_CmdHandle ch)
{
	for (uint32_t k = 0u; k < MIN(fake_rf.n_cmds, (uint32_t)FAKE_RF_MAX_CMDS); k++) {
		struct fake_rf_cmd *c = fake_rf_nth(fake_rf.n_cmds - 1u - k);

		if (c != NULL && c->via != FAKE_RF_RUN && c->cmd.ch == ch) {
			return c;
		}
	}
	return NULL;
}

struct fake_rf_cmd *fake_rf_last(void)
{
	for (uint32_t k = 0u; k < MIN(fake_rf.n_cmds, (uint32_t)FAKE_RF_MAX_CMDS); k++) {
		struct fake_rf_cmd *c = fake_rf_nth(fake_rf.n_cmds - 1u - k);

		if (c != NULL && c->via != FAKE_RF_RUN) {
			return c;
		}
	}
	return NULL;
}

const struct fake_rf_op_snap *fake_rf_find_op(const struct fake_rf_cmd *c,
					      uint16_t command_no)
{
	if (c == NULL) {
		return NULL;
	}
	for (uint8_t i = 0u; i < c->n_chain; i++) {
		if (c->chain[i].command_no == command_no) {
			return &c->chain[i];
		}
	}
	return NULL;
}

static void snap_op(struct fake_rf_op_snap *s, RF_Op *op)
{
	memset(s, 0, sizeof(*s));
	s->op = op;
	s->command_no = op->commandNo;
	s->start_time = op->startTime;
	s->trigger_type = op->startTrigger.triggerType;
	s->past_trig = op->startTrigger.pastTrig;
	s->cond_rule = op->condition.rule;

	switch (op->commandNo) {
	case CMD_FS: {
		const rfc_CMD_FS_t *fs = (const rfc_CMD_FS_t *)op;

		s->fs_frequency = fs->frequency;
		s->fs_fract_freq = fs->fractFreq;
		s->fs_tx_mode = fs->synthConf.bTxMode;
		break;
	}
	case CMD_PROP_RX_ADV: {
		const rfc_CMD_PROP_RX_ADV_t *rx = (const rfc_CMD_PROP_RX_ADV_t *)op;

		s->end_time = rx->endTime;
		s->end_trigger_type = rx->endTrigger.triggerType;
		s->end_past_trig = rx->endTrigger.pastTrig;
		s->sync_word0 = rx->syncWord0;
		s->sync_word1 = rx->syncWord1;
		s->max_pkt_len = rx->maxPktLen;
		break;
	}
	case CMD_PROP_RX: {
		const rfc_CMD_PROP_RX_t *rx = (const rfc_CMD_PROP_RX_t *)op;

		s->end_time = rx->endTime;
		s->end_trigger_type = rx->endTrigger.triggerType;
		s->end_past_trig = rx->endTrigger.pastTrig;
		s->sync_word0 = rx->syncWord;
		s->max_pkt_len = rx->maxPktLen;
		break;
	}
	case CMD_PROP_TX_ADV: {
		const rfc_CMD_PROP_TX_ADV_t *tx = (const rfc_CMD_PROP_TX_ADV_t *)op;

		s->sync_word = tx->syncWord;
		s->pkt_len = tx->pktLen;
		break;
	}
	case CMD_PROP_TX: {
		const rfc_CMD_PROP_TX_t *tx = (const rfc_CMD_PROP_TX_t *)op;

		s->sync_word = tx->syncWord;
		s->pkt_len = tx->pktLen;
		break;
	}
	default:
		break;
	}
}

static struct fake_rf_cmd *record(enum fake_rf_via via, RF_Handle h, RF_Op *op,
				  RF_Priority pri, RF_Callback cb,
				  RF_EventMask events)
{
	struct fake_rf_cmd *c = &cmds[fake_rf.n_cmds % FAKE_RF_MAX_CMDS];
	RF_Op *it = op;

	memset(c, 0, sizeof(*c));
	c->seq = fake_rf.n_cmds++;
	c->via = via;
	c->h = h;
	c->pri = pri;
	c->cb = cb;
	c->subscribed = events;
	while (it != NULL && c->n_chain < FAKE_RF_CHAIN_MAX) {
		snap_op(&c->chain[c->n_chain++], it);
		if (it->condition.rule == COND_NEVER) {
			break;
		}
		it = (RF_Op *)it->pNextOp;
	}

	c->cmd.pCb = cb;
	c->cmd.pOp = op;
	c->cmd.pClient = h;
	c->cmd.bmEvent = events;
	c->cmd.ePri = pri;
	c->cmd.startTime = op->startTime;
	c->cmd.startType = (op->startTrigger.triggerType == TRIG_ABSTIME)
				   ? RF_StartAbs : RF_StartNotSpecified;
	c->cmd.endType = RF_EndNotSpecified;
	if (via != FAKE_RF_RUN) {
		fake_rf.last_ch = (RF_CmdHandle)((fake_rf.last_ch + 1) & FAKE_RF_CH_MASK);
		c->cmd.ch = fake_rf.last_ch;
	} else {
		c->cmd.ch = -1;
	}
	return c;
}

/* ---- Driver API ---- */

void RF_Params_init(RF_Params *params)
{
	memset(params, 0, sizeof(*params));
	params->nInactivityTimeout = UINT32_MAX;
}

void RF_ScheduleCmdParams_init(RF_ScheduleCmdParams *params)
{
	memset(params, 0, sizeof(*params));
	params->startType = RF_StartNotSpecified;
	params->allowDelay = RF_AllowDelayAny;
	params->endType = RF_EndNotSpecified;
}

RF_Handle RF_open(RF_Object *pObj, RF_Mode *pRfMode, RF_RadioSetup *pOpSetup,
		  RF_Params *params)
{
	fake_rf.opens++;
	fake_rf.mode = pRfMode;
	fake_rf.setup = pOpSetup;
	if (params != NULL) {
		fake_rf.params = *params;
	}
	if (fake_rf.open_fails) {
		return NULL;
	}
	pObj->fake_magic = 0xFA4Eu;
	fake_rf.open = true;
	fake_rf.handle = pObj;
	return pObj;
}

void RF_close(RF_Handle h)
{
	ARG_UNUSED(h);
	fake_rf.closes++;
	fake_rf.open = false;
	fake_rf.handle = NULL;
}

uint32_t RF_getCurrentTime(void)
{
	uint32_t now = fake_rf.rat;

	fake_rf.rat += fake_rf.rat_step;
	return now;
}

RF_EventMask RF_runCmd(RF_Handle h, RF_Op *pOp, RF_Priority ePri,
		       RF_Callback pCb, RF_EventMask bmEvent)
{
	struct fake_rf_cmd *c = record(FAKE_RF_RUN, h, pOp, ePri, pCb, bmEvent);

	c->started = true;
	c->ended = true;
	c->fired = fake_rf.run_result;
	pOp->status = (fake_rf.run_result & RF_EventLastCmdDone) ? DONE_OK : ERROR_PAST_START;
	return fake_rf.run_result;
}

RF_CmdHandle RF_postCmd(RF_Handle h, RF_Op *pOp, RF_Priority ePri,
			RF_Callback pCb, RF_EventMask bmEvent)
{
	struct fake_rf_cmd *c;

	if (fake_rf.refuse_posts > 0u) {
		fake_rf.refuse_posts--;
		fake_rf.refusals++;
		return fake_rf.refuse_rc;
	}
	c = record(FAKE_RF_POST, h, pOp, ePri, pCb, bmEvent);
	pOp->status = PENDING;
	return c->cmd.ch;
}

RF_CmdHandle RF_scheduleCmd(RF_Handle h, RF_Op *pOp,
			    RF_ScheduleCmdParams *pSchParams, RF_Callback pCb,
			    RF_EventMask bmEvent)
{
	struct fake_rf_cmd *c;

	if (fake_rf.refuse_posts > 0u) {
		fake_rf.refuse_posts--;
		fake_rf.refusals++;
		return fake_rf.refuse_rc;
	}
	c = record(FAKE_RF_SCHEDULE, h, pOp, RF_PriorityNormal, pCb, bmEvent);
	c->sch = *pSchParams;
	c->cmd.startTime = pSchParams->startTime;
	c->cmd.startType = pSchParams->startType;
	c->cmd.allowDelay = pSchParams->allowDelay;
	c->cmd.endTime = pSchParams->endTime;
	c->cmd.endType = pSchParams->endType;
	c->cmd.duration = pSchParams->duration;
	c->cmd.activityInfo = pSchParams->activityInfo;
	c->cmd.coexPriority = pSchParams->coexPriority;
	c->cmd.coexRequest = pSchParams->coexRequest;
	pOp->status = PENDING;
	return c->cmd.ch;
}

static bool cmd_started(const struct fake_rf_cmd *c)
{
	return c->started ||
	       (c->cmd.startType == RF_StartAbs &&
		(int32_t)(fake_rf.rat - c->cmd.startTime) >= 0);
}

void fake_rf_queue_cb(RF_CmdHandle ch, RF_EventMask events)
{
	if (pend_n >= FAKE_RF_PENDING_MAX) {
		return;  /* a test that queues 16 callbacks has a bug; drop */
	}
	pend[(pend_head + pend_n) % FAKE_RF_PENDING_MAX].ch = ch;
	pend[(pend_head + pend_n) % FAKE_RF_PENDING_MAX].events = events;
	pend_n++;
}

/* RF_abortCmd's effect on one live command: the termination stored, the
 * callback owed. */
static void abort_one(struct fake_rf_cmd *c, uint8_t mode, bool flush)
{
	const RF_EventMask ev = cmd_started(c)
		? ((mode != 0u) ? RF_EventCmdStopped : RF_EventCmdAborted)
		: RF_EventCmdCancelled;

	c->cancel_calls++;
	c->cancel_flush = flush;
	c->cancel_mode = mode;
	if ((c->cmd.pastifg & TERM_EVENTS) == 0u) {
		fake_rf_queue_cb(c->cmd.ch, ev);
	}
	c->cmd.pastifg |= ev;
}

static RF_Stat cancel_common(RF_Handle h, RF_CmdHandle ch, uint8_t mode, bool flush)
{
	struct fake_rf_cmd *c = fake_rf_cmd(ch);

	if (c == NULL || c->h != h) {
		return RF_StatInvalidParamsError;
	}
	if (c->ended) {
		return RF_StatCmdEnded;
	}
	abort_one(c, mode, flush);
	if (flush) {
		/* Every later live command of this client, in post order. */
		for (uint32_t i = c->seq + 1u; i < fake_rf.n_cmds; i++) {
			struct fake_rf_cmd *n = fake_rf_nth(i);

			if (n != NULL && n->via != FAKE_RF_RUN && n->h == h && !n->ended) {
				abort_one(n, mode, flush);
			}
		}
	}
	return RF_StatSuccess;
}

RF_Stat RF_cancelCmd(RF_Handle h, RF_CmdHandle ch, uint8_t mode)
{
	fake_rf.cancel_calls++;
	return cancel_common(h, ch, mode, false);
}

RF_Stat RF_flushCmd(RF_Handle h, RF_CmdHandle ch, uint8_t mode)
{
	fake_rf.flush_calls++;
	return cancel_common(h, ch, mode, true);
}

RF_Stat RF_getInfo(RF_Handle h, RF_InfoType type, RF_InfoVal *pValue)
{
	ARG_UNUSED(h);
	switch (type) {
	case RF_GET_RADIO_STATE:
		pValue->bRadioState = fake_rf.radio_active;
		return RF_StatSuccess;
	case RF_GET_CURR_CMD: {
		struct fake_rf_cmd *c = fake_rf_last();

		pValue->ch = (c != NULL) ? c->cmd.ch : -1;
		return RF_StatSuccess;
	}
	default:
		return RF_StatInvalidParamsError;
	}
}

RF_Stat RF_setTxPower(RF_Handle h, RF_TxPowerTable_Value value)
{
	ARG_UNUSED(h);
	fake_rf.set_tx_power_calls++;
	fake_rf.last_tx_power = value;
	return fake_rf.set_tx_power_rc;
}

/* TI's own rule (RFCC26X2_multiMode.c): the highest entry not above the
 * request; below the first entry, invalid. */
RF_TxPowerTable_Value RF_TxPowerTable_findValue(RF_TxPowerTable_Entry table[],
						int8_t powerLevel)
{
	RF_TxPowerTable_Value invalid = { .rawValue = RF_TxPowerTable_INVALID_VALUE,
					  .paType = RF_TxPowerTable_DefaultPA };
	uint32_t i;

	if (powerLevel == RF_TxPowerTable_MIN_DBM) {
		return table[0].value;
	}
	for (i = 0u; table[i].power != RF_TxPowerTable_INVALID_DBM &&
		     table[i].value.rawValue != RF_TxPowerTable_INVALID_VALUE; i++) {
		if (table[i].power > powerLevel) {
			break;
		}
	}
	return (i == 0u) ? invalid : table[i - 1u].value;
}

RF_ScheduleStatus RF_defaultSubmitPolicy(RF_Cmd *pCmdNew, RF_Cmd *pCmdBg,
					 RF_Cmd *pCmdFg, List_List *pPendQueue,
					 List_List *pDoneQueue)
{
	ARG_UNUSED(pCmdNew); ARG_UNUSED(pCmdBg); ARG_UNUSED(pCmdFg);
	ARG_UNUSED(pPendQueue); ARG_UNUSED(pDoneQueue);
	fake_rf.default_submit_calls++;
	return fake_rf.default_submit_rc;
}

RF_ExecuteAction RF_defaultExecutionPolicy(RF_Cmd *pCmdBg, RF_Cmd *pCmdFg,
					   List_List *pPendQueue,
					   List_List *pDoneQueue, bool bConflict,
					   RF_Cmd *conflictCmd)
{
	ARG_UNUSED(pCmdBg); ARG_UNUSED(pCmdFg); ARG_UNUSED(pPendQueue);
	ARG_UNUSED(pDoneQueue); ARG_UNUSED(bConflict); ARG_UNUSED(conflictCmd);
	fake_rf.default_execute_calls++;
	return fake_rf.default_execute_rc;
}

/* ---- Test hooks ---- */

void fake_rf_set_rat(uint32_t ticks)
{
	fake_rf.rat = ticks;
}

void fake_rf_advance_us(uint32_t us)
{
	fake_rf.rat += us * 4u;
}

void fake_rf_start(RF_CmdHandle ch)
{
	struct fake_rf_cmd *c = fake_rf_cmd(ch);

	if (c != NULL) {
		c->started = true;
		if (c->n_chain > 0u) {
			c->chain[c->n_chain - 1u].op->status = ACTIVE;
		}
	}
}

void fake_rf_set_status(RF_CmdHandle ch, uint16_t status)
{
	struct fake_rf_cmd *c = fake_rf_cmd(ch);

	if (c != NULL && c->n_chain > 0u) {
		c->chain[c->n_chain - 1u].op->status = status;
	}
}

static void deliver(struct fake_rf_cmd *c, RF_CmdHandle ch, RF_EventMask events)
{
	RF_Callback cb = NULL;
	RF_Handle h = NULL;

	if (c != NULL) {
		c->fired |= events;
		if ((events & TERM_EVENTS) != 0u) {
			c->ended = true;
			c->started = true;
		}
		cb = c->cb;
		h = c->h;
	}
	if (cb != NULL) {
		cb(h, ch, events);
	}
}

void fake_rf_fire(RF_CmdHandle ch, RF_EventMask events)
{
	deliver(fake_rf_cmd(ch), ch, events);
}

void fake_rf_complete(RF_CmdHandle ch, uint16_t status, RF_EventMask events)
{
	fake_rf_set_status(ch, status);
	fake_rf_fire(ch, events);
}

uint32_t fake_rf_pending(void)
{
	return pend_n;
}

bool fake_rf_deliver_one(void)
{
	RF_CmdHandle ch;
	RF_EventMask ev;

	if (pend_n == 0u) {
		return false;
	}
	ch = pend[pend_head].ch;
	ev = pend[pend_head].events;
	pend_head = (pend_head + 1u) % FAKE_RF_PENDING_MAX;
	pend_n--;
	deliver(fake_rf_cmd(ch), ch, ev);
	return true;
}

uint32_t fake_rf_deliver_pending(void)
{
	uint32_t n = 0u;

	while (n < 4u * FAKE_RF_PENDING_MAX && fake_rf_deliver_one()) {
		n++;
	}
	return n;
}

int fake_rf_rx_entry(RF_CmdHandle ch, const uint8_t *payload, uint8_t len,
		     int8_t rssi, uint32_t rat, uint8_t sync_id)
{
	const struct fake_rf_cmd *c = fake_rf_cmd(ch);
	dataQueue_t *q = NULL;
	bool app_rssi = false, app_ts = false, app_status = false;
	rfc_dataEntryGeneral_t *e;
	uint8_t *p;
	size_t need;
	size_t lsz;

	if (c == NULL) {
		return -1;
	}
	for (uint8_t i = 0u; i < c->n_chain && q == NULL; i++) {
		RF_Op *op = c->chain[i].op;

		if (op->commandNo == CMD_PROP_RX_ADV) {
			rfc_CMD_PROP_RX_ADV_t *rx = (rfc_CMD_PROP_RX_ADV_t *)op;

			q = rx->pQueue;
			app_rssi = rx->rxConf.bAppendRssi;
			app_ts = rx->rxConf.bAppendTimestamp;
			app_status = rx->rxConf.bAppendStatus;
		} else if (op->commandNo == CMD_PROP_RX) {
			rfc_CMD_PROP_RX_t *rx = (rfc_CMD_PROP_RX_t *)op;

			q = rx->pQueue;
			app_rssi = rx->rxConf.bAppendRssi;
			app_ts = rx->rxConf.bAppendTimestamp;
			app_status = rx->rxConf.bAppendStatus;
		}
	}
	if (q == NULL || q->pCurrEntry == NULL) {
		return -1;
	}
	e = (rfc_dataEntryGeneral_t *)q->pCurrEntry;
	lsz = e->config.lenSz;
	need = lsz + len + (app_rssi ? 1u : 0u) + (app_ts ? 4u : 0u) +
	       (app_status ? 1u : 0u);
	if (e->status != DATA_ENTRY_PENDING || need > e->length) {
		return -2;
	}

	p = &e->data;
	if (lsz == 1u) {
		*p++ = (uint8_t)(need - lsz);
	} else if (lsz == 2u) {
		const uint16_t l = (uint16_t)(need - lsz);

		memcpy(p, &l, 2u);
		p += 2;
	}
	memcpy(p, payload, len);
	p += len;
	if (app_rssi) {
		*p++ = (uint8_t)rssi;
	}
	if (app_ts) {
		memcpy(p, &rat, 4u);
		p += 4;
	}
	if (app_status) {
		rfc_propRxStatus_t st;

		memset(&st, 0, sizeof(st));
		st.status.syncWordId = sync_id & 1u;
		memcpy(p, &st, 1u);
	}
	e->status = DATA_ENTRY_FINISHED;
	q->pCurrEntry = e->pNextEntry;
	return 0;
}
