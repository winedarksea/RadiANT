/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Fake <ti/drivers/rf/RF.h> for radiant/tests/backend_cc26xx: TI's RF driver
 * API as radiant_radio_cc26xx.c and radiant_radio_cc26xx_arb.c use it, with a
 * recording, test-driven implementation in fake_rf.c. The command structures
 * (rfc_CMD_*, dataQueue_t, rfc_dataEntryGeneral_t ...) are hal_ti's real
 * driverlib headers; only the driver above them is fake. Names, enum values
 * and event bits are copied from hal_ti's RFCC26X2.h so a test's event masks
 * mean what they mean on hardware.
 *
 * ---------------------------------------------------------------------------
 * HOOK API (what a test may use; everything is reset by fake_rf_reset())
 * ---------------------------------------------------------------------------
 *
 * Every RF_runCmd/RF_postCmd/RF_scheduleCmd is RECORDED as a struct
 * fake_rf_cmd (newest FAKE_RF_MAX_CMDS kept):
 *   fake_rf_last()            newest posted/scheduled command (not RF_runCmd)
 *   fake_rf_cmd(ch)           newest record with handle ch, or NULL
 *   fake_rf_nth(i)            i-th record ever made (0 = first), or NULL if
 *                             it has been overwritten; fake_rf.n_cmds counts
 *   fake_rf_find_op(c, no)    the snapshot of command number `no` (CMD_FS,
 *                             CMD_PROP_RX_ADV ...) in c's chain, or NULL
 * A record holds: via (run/post/schedule), priority, the RF_ScheduleCmdParams
 * copy (schedule only), callback + subscribed mask, and chain[0..n_chain-1]:
 * a SNAPSHOT taken at post time of each op in the chain (pNextOp followed
 * while condition.rule != COND_NEVER): commandNo, startTime, startTrigger
 * type/pastTrig, and per type CMD_FS frequency/fractFreq, PROP_RX endTime/
 * endTrigger/syncWord0/1/maxPktLen, PROP_TX syncWord/pktLen. Snapshots matter
 * because the backend reuses one static command struct per kind. c->cmd is
 * the RF_Cmd the driver's scheduler hooks would see (ch, pOp, pClient,
 * pastifg, startTime, endTime, endType, activityInfo).
 *
 * Driving time: the RAT is fake_rf.rat (4 MHz ticks, 32-bit, wraps).
 *   fake_rf_set_rat(ticks), fake_rf_advance_us(us); fake_rf.rat_step adds
 *   that many ticks after every RF_getCurrentTime() read (0 = frozen).
 *
 * Driving the RF core:
 *   fake_rf_rx_entry(ch, payload, len, rssi, rat, sync_id)
 *       writes one received frame into the data queue of ch's RX op as the
 *       RF core would: payload (the bytes after the sync word), then RSSI,
 *       4-byte RAT timestamp and status byte (rfc_propRxStatus_t, syncWordId
 *       = sync_id) - each only if that op's rxConf.bAppend* is set - then
 *       DATA_ENTRY_FINISHED and pCurrEntry advanced. No callback: follow
 *       with fake_rf_fire(ch, RF_EventRxEntryDone). Returns 0, or -1 if ch
 *       has no RX op / queue, -2 if the current entry is not PENDING
 *       (buffer full) or too short.
 *   fake_rf_set_status(ch, status)
 *       writes the status word of the LAST op in ch's chain (live struct,
 *       which the backend reads in its callback, e.g. PROP_DONE_RXTIMEOUT,
 *       PROP_ERROR_PAR, ERROR_PAST_START).
 *   fake_rf_start(ch)        marks ch as started (running on the core).
 *   fake_rf_fire(ch, events)
 *       calls ch's callback NOW, synchronously, with exactly `events` (no
 *       subscription masking), as the RF SWI would. Any termination bit
 *       (LastCmdDone/Aborted/Stopped/Cancelled) marks ch ended. NB the real
 *       driver never puts RF_EventLastCmdDone beside an abort bit: when
 *       Cancelled/Aborted/Stopped/Preempted is set it masks the terminal
 *       callback down to those bits (RF_radioOpDoneCb).
 *   fake_rf_complete(ch, status, events)  set_status, then fire.
 *
 * Asynchronous cancellation (as on hardware; see commit e82a0ea):
 *   RF_cancelCmd/RF_flushCmd never call back synchronously. On a live
 *   command they store the termination into c->cmd.pastifg (as RF_abortCmd
 *   does, which arb.c's own_retired() relies on) and QUEUE one callback:
 *   started ? (mode ? RF_EventCmdStopped : RF_EventCmdAborted)
 *           : RF_EventCmdCancelled.
 *   RF_flushCmd also queues one for each later live command of the client.
 *   On an ended or unknown command: RF_StatCmdEnded / RF_StatInvalidParams-
 *   Error and nothing queued.
 *     fake_rf_pending()        callbacks queued, not yet delivered
 *     fake_rf_deliver_one()    delivers the oldest; false if none
 *     fake_rf_deliver_pending() delivers until empty (including ones queued
 *                              by callbacks meanwhile); returns the count
 *     fake_rf_queue_cb(ch, ev) queue any callback yourself (e.g. a
 *                              preemption the driver would deliver later)
 *
 * Knobs in struct fake_rf_state fake_rf (set after fake_rf_reset()):
 *   open_fails       RF_open() returns NULL
 *   run_result       RF_runCmd() return mask (default RF_EventLastCmdDone)
 *   refuse_posts     the next n RF_postCmd/RF_scheduleCmd calls return
 *                    refuse_rc (default RF_SCHEDULE_CMD_ERROR), not recorded
 *                    as commands; fake_rf.refusals counts them
 *   radio_active     RF_getInfo(RF_GET_RADIO_STATE) (default true)
 *   set_tx_power_rc  RF_setTxPower() return (default RF_StatSuccess)
 *   last_ch          the handle most recently issued; the next is
 *                    (last_ch + 1) & FAKE_RF_CH_MASK, as the driver's
 *                    nSeqPost (N_CMD_MODMASK 0xFFF). Set it to test a wrap.
 * Counters: opens, closes, n_cmds, cancel_calls, flush_calls,
 *   set_tx_power_calls, last_tx_power, params (last RF_open), mode, setup.
 *
 * Scheduler-policy hooks (coex scenario): RF_scheduleCmd does NOT run the
 * submit hook; a test calls radiant_cc26xx_submit()/_execute() directly with
 * fake_rf_cmd records' c->cmd. RF_defaultSubmitPolicy/RF_defaultExecutionPolicy
 * are recorded stubs returning fake_rf.default_submit_rc (default
 * RF_ScheduleStatusTail) / fake_rf.default_execute_rc (default
 * RF_ExecuteActionNone); default_submit_calls / default_execute_calls count.
 */
#ifndef FAKE_TI_DRIVERS_RF_RF_H_
#define FAKE_TI_DRIVERS_RF_RF_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <driverlib/rf_mailbox.h>
#include <driverlib/rf_common_cmd.h>
#include <driverlib/rf_prop_cmd.h>
#include <driverlib/rf_data_entry.h>

#include <ti/drivers/utils/List.h>

/* ---- Driver types (shapes from hal_ti RFCC26X2.h) ---- */

typedef uint64_t RF_EventMask;
typedef int16_t RF_CmdHandle;
typedef rfc_radioOp_t RF_Op;

#define RF_EventCmdDone             (1 << 0)
#define RF_EventLastCmdDone         (1 << 1)
#define RF_EventFGCmdDone           (1 << 2)
#define RF_EventLastFGCmdDone       (1 << 3)
#define RF_EventTxDone              (1 << 4)
#define RF_EventTxEntryDone         (1 << 10)
#define RF_EventRxOk                (1 << 16)
#define RF_EventRxNOk               (1 << 17)
#define RF_EventRxIgnored           (1 << 18)
#define RF_EventRxEmpty             (1 << 19)
#define RF_EventRxBufFull           (1 << 22)
#define RF_EventRxEntryDone         (1 << 23)
#define RF_EventRxAborted           (1 << 26)
#define RF_EventInternalError       (uint32_t)(1u << 31)
#define RF_EventCmdCancelled        0x1000000000000000ull
#define RF_EventCmdAborted          0x2000000000000000ull
#define RF_EventCmdStopped          0x4000000000000000ull
#define RF_EventError               0x0200000000000000ull
#define RF_EventCmdPreempted        0x0100000000000000ull

#define RF_ALLOC_ERROR              (-2)
#define RF_SCHEDULE_CMD_ERROR       (-3)
#define RF_CMDHANDLE_FLUSH_ALL      (-1)

typedef enum {
	RF_StatBusyError,
	RF_StatRadioInactiveError,
	RF_StatCmdDoneError,
	RF_StatInvalidParamsError,
	RF_StatCmdEnded,
	RF_StatError = 0x80,
	RF_StatCmdDoneSuccess,
	RF_StatCmdSch,
	RF_StatSuccess
} RF_Stat;

typedef enum {
	RF_PriorityHighest = 2,
	RF_PriorityHigh    = 1,
	RF_PriorityNormal  = 0,
} RF_Priority;

typedef enum {
	RF_PriorityCoexDefault = 0,
	RF_PriorityCoexLow     = 1,
	RF_PriorityCoexHigh    = 2,
} RF_PriorityCoex;

typedef enum {
	RF_RequestCoexDefault       = 0,
	RF_RequestCoexAssertRx      = 1,
	RF_RequestCoexNoAssertRx    = 2,
} RF_RequestCoex;

typedef enum {
	RF_StartNotSpecified = 0,
	RF_StartAbs          = 1,
} RF_StartType;

typedef enum {
	RF_EndNotSpecified = 0,
	RF_EndAbs          = 1,
	RF_EndRel          = 2,
	RF_EndInfinit      = 3,
} RF_EndType;

typedef enum {
	RF_AllowDelayNone = 0,
	RF_AllowDelayAny  = UINT32_MAX
} RF_AllowDelay;

typedef struct {
	uint8_t rfMode;
	void (*cpePatchFxn)(void);
	void (*mcePatchFxn)(void);
	void (*rfePatchFxn)(void);
} RF_Mode;

typedef union {
	rfc_CMD_PROP_RADIO_DIV_SETUP_t    prop_div;
	rfc_CMD_PROP_RADIO_DIV_SETUP_PA_t prop_div_pa;
} RF_RadioSetup;

/* Opaque to clients; the fake keeps nothing in it but a marker. */
typedef struct RF_ObjectMultiMode {
	uint32_t fake_magic;
	uint32_t nID;
} RF_Object;
typedef RF_Object *RF_Handle;

typedef void (*RF_Callback)(RF_Handle h, RF_CmdHandle ch, RF_EventMask e);

typedef struct {
	uint32_t nInactivityTimeout;
	uint32_t nPowerUpDuration;
	void    *pPowerCb;
	void    *pErrCb;
	uint16_t nPowerUpDurationMargin;
	uint16_t nPhySwitchingDurationMargin;
	void    *pClientEventCb;
	uint32_t nClientEventMask;
	uint32_t nID;
} RF_Params;

typedef struct {
	uint32_t        startTime;
	RF_StartType    startType;
	uint32_t        allowDelay;
	uint32_t        endTime;
	RF_EndType      endType;
	uint32_t        duration;
	uint32_t        activityInfo;
	RF_PriorityCoex coexPriority;
	RF_RequestCoex  coexRequest;
} RF_ScheduleCmdParams;

typedef enum {
	RF_GET_CURR_CMD,
	RF_GET_AVAIL_RAT_CH,
	RF_GET_RADIO_STATE,
	RF_GET_SCHEDULE_MAP,
	RF_GET_CLIENT_LIST,
	RF_GET_CLIENT_SWITCHING_TIME,
} RF_InfoType;

typedef union {
	RF_CmdHandle ch;
	uint16_t     availRatCh;
	bool         bRadioState;
	RF_Handle    pClientList[2];
	uint32_t     phySwitchingTimeInUs[2];
	void        *pScheduleMap;
} RF_InfoVal;

/* ---- TX power table ---- */

typedef struct {
	uint32_t rawValue:22;
	uint32_t __dummy:9;
	uint32_t paType:1;
} RF_TxPowerTable_Value;

typedef struct {
	int8_t                power;
	RF_TxPowerTable_Value value;
} RF_TxPowerTable_Entry;

typedef enum {
	RF_TxPowerTable_DefaultPA = 0,
	RF_TxPowerTable_HighPA = 1,
} RF_TxPowerTable_PAType;

#define RF_TxPowerTable_MIN_DBM       (-128)
#define RF_TxPowerTable_MAX_DBM       126
#define RF_TxPowerTable_INVALID_DBM   127
#define RF_TxPowerTable_INVALID_VALUE 0x3fffff
#define RF_TxPowerTable_TERMINATION_ENTRY                                          \
	{ .power = RF_TxPowerTable_INVALID_DBM,                                     \
	  .value = { .rawValue = RF_TxPowerTable_INVALID_VALUE,                     \
		     .paType = RF_TxPowerTable_DefaultPA } }
#define RF_TxPowerTable_DEFAULT_PA_ENTRY(bias, gain, boost, coefficient)           \
	{ .rawValue = ((bias) << 0) | ((gain) << 6) | ((boost) << 8) |              \
		      ((coefficient) << 9),                                         \
	  .paType = RF_TxPowerTable_DefaultPA }

/* ---- The driver's command record and the scheduler-policy hooks ---- */

typedef struct RF_Cmd_s RF_Cmd;
struct RF_Cmd_s {
	List_Elem            _elem;
	RF_Callback volatile pCb;
	RF_Op               *pOp;
	RF_Object           *pClient;
	RF_EventMask         bmEvent;
	RF_EventMask         pastifg;
	RF_EventMask         rfifg;
	RF_CmdHandle         ch;
	RF_Priority          ePri;
	uint8_t volatile     flags;
	uint32_t             startTime;
	RF_StartType         startType;
	uint32_t             allowDelay;
	uint32_t             endTime;
	RF_EndType           endType;
	uint32_t             duration;
	uint32_t             activityInfo;
	RF_PriorityCoex      coexPriority;
	RF_RequestCoex       coexRequest;
};

typedef enum {
	RF_ExecuteActionNone           = 0,
	RF_ExecuteActionRejectIncoming = 1,
	RF_ExecuteActionAbortOngoing   = 2,
} RF_ExecuteAction;

typedef enum {
	RF_ScheduleStatusError   = -3,
	RF_ScheduleStatusNone    = 0,
	RF_ScheduleStatusTop     = 1,
	RF_ScheduleStatusMiddle  = 2,
	RF_ScheduleStatusTail    = 4,
	RF_ScheduleStatusPreempt = 8
} RF_ScheduleStatus;

typedef RF_ScheduleStatus (*RF_SubmitHook)(RF_Cmd *pCmdNew, RF_Cmd *pCmdBg,
					   RF_Cmd *pCmdFg, List_List *pPendQueue,
					   List_List *pDoneQueue);
typedef RF_ExecuteAction (*RF_ExecuteHook)(RF_Cmd *pCmdBg, RF_Cmd *pCmdFg,
					   List_List *pPendQueue,
					   List_List *pDoneQueue, bool bConflict,
					   RF_Cmd *conflictCmd);

typedef struct {
	RF_SubmitHook  submitHook;
	RF_ExecuteHook executeHook;
} RFCC26XX_SchedulerPolicy;

/* ---- Driver API ---- */

void         RF_Params_init(RF_Params *params);
void         RF_ScheduleCmdParams_init(RF_ScheduleCmdParams *params);
RF_Handle    RF_open(RF_Object *pObj, RF_Mode *pRfMode, RF_RadioSetup *pOpSetup,
		     RF_Params *params);
void         RF_close(RF_Handle h);
uint32_t     RF_getCurrentTime(void);
RF_EventMask RF_runCmd(RF_Handle h, RF_Op *pOp, RF_Priority ePri,
		       RF_Callback pCb, RF_EventMask bmEvent);
RF_CmdHandle RF_postCmd(RF_Handle h, RF_Op *pOp, RF_Priority ePri,
			RF_Callback pCb, RF_EventMask bmEvent);
RF_CmdHandle RF_scheduleCmd(RF_Handle h, RF_Op *pOp,
			    RF_ScheduleCmdParams *pSchParams, RF_Callback pCb,
			    RF_EventMask bmEvent);
RF_Stat      RF_cancelCmd(RF_Handle h, RF_CmdHandle ch, uint8_t mode);
RF_Stat      RF_flushCmd(RF_Handle h, RF_CmdHandle ch, uint8_t mode);
RF_Stat      RF_getInfo(RF_Handle h, RF_InfoType type, RF_InfoVal *pValue);
RF_Stat      RF_setTxPower(RF_Handle h, RF_TxPowerTable_Value value);
RF_TxPowerTable_Value RF_TxPowerTable_findValue(RF_TxPowerTable_Entry table[],
						int8_t powerLevel);

RF_ScheduleStatus RF_defaultSubmitPolicy(RF_Cmd *pCmdNew, RF_Cmd *pCmdBg,
					 RF_Cmd *pCmdFg, List_List *pPendQueue,
					 List_List *pDoneQueue);
RF_ExecuteAction  RF_defaultExecutionPolicy(RF_Cmd *pCmdBg, RF_Cmd *pCmdFg,
					    List_List *pPendQueue,
					    List_List *pDoneQueue, bool bConflict,
					    RF_Cmd *conflictCmd);

/* ---- Test hooks (see the header comment) ---- */

#define FAKE_RF_MAX_CMDS     64
#define FAKE_RF_CHAIN_MAX    4
#define FAKE_RF_PENDING_MAX  16
#define FAKE_RF_CH_MASK      0xFFF

enum fake_rf_via { FAKE_RF_RUN, FAKE_RF_POST, FAKE_RF_SCHEDULE };

struct fake_rf_op_snap {
	RF_Op   *op;            /* the live struct (the backend reuses it) */
	uint16_t command_no;
	uint32_t start_time;
	uint8_t  trigger_type;
	bool     past_trig;
	uint8_t  cond_rule;
	/* CMD_FS */
	uint16_t fs_frequency;
	uint16_t fs_fract_freq;
	bool     fs_tx_mode;
	/* CMD_PROP_RX / CMD_PROP_RX_ADV */
	uint32_t end_time;
	uint8_t  end_trigger_type;
	bool     end_past_trig;
	uint32_t sync_word0;
	uint32_t sync_word1;
	uint16_t max_pkt_len;
	/* CMD_PROP_TX / CMD_PROP_TX_ADV */
	uint32_t sync_word;
	uint16_t pkt_len;
};

struct fake_rf_cmd {
	RF_Cmd               cmd;       /* the scheduler hooks' view */
	enum fake_rf_via     via;
	RF_Handle            h;
	RF_Priority          pri;
	RF_ScheduleCmdParams sch;       /* FAKE_RF_SCHEDULE only */
	RF_Callback          cb;
	RF_EventMask         subscribed;
	uint8_t              n_chain;
	struct fake_rf_op_snap chain[FAKE_RF_CHAIN_MAX];
	bool                 started;
	bool                 ended;
	uint32_t             cancel_calls;  /* cancel/flush calls naming it */
	bool                 cancel_flush;  /* the last was RF_flushCmd */
	uint8_t              cancel_mode;
	RF_EventMask         fired;     /* OR of every event delivered to it */
	uint32_t             seq;       /* index in fake_rf_nth() */
};

struct fake_rf_state {
	/* knobs */
	bool              open_fails;
	RF_EventMask      run_result;
	uint32_t          refuse_posts;
	RF_CmdHandle      refuse_rc;
	bool              radio_active;
	RF_Stat           set_tx_power_rc;
	uint32_t          rat_step;
	RF_ScheduleStatus default_submit_rc;
	RF_ExecuteAction  default_execute_rc;
	/* state and counters */
	uint32_t          rat;
	bool              open;
	RF_Handle         handle;
	uint32_t          opens;
	uint32_t          closes;
	RF_Mode          *mode;
	RF_RadioSetup    *setup;
	RF_Params         params;
	RF_CmdHandle      last_ch;
	uint32_t          n_cmds;
	uint32_t          refusals;
	uint32_t          cancel_calls;
	uint32_t          flush_calls;
	uint32_t          set_tx_power_calls;
	RF_TxPowerTable_Value last_tx_power;
	uint32_t          default_submit_calls;
	uint32_t          default_execute_calls;
};

extern struct fake_rf_state fake_rf;

void                fake_rf_reset(void);
struct fake_rf_cmd *fake_rf_last(void);
struct fake_rf_cmd *fake_rf_cmd(RF_CmdHandle ch);
struct fake_rf_cmd *fake_rf_nth(uint32_t i);
const struct fake_rf_op_snap *fake_rf_find_op(const struct fake_rf_cmd *c,
					      uint16_t command_no);
void                fake_rf_set_rat(uint32_t ticks);
void                fake_rf_advance_us(uint32_t us);
int                 fake_rf_rx_entry(RF_CmdHandle ch, const uint8_t *payload,
				     uint8_t len, int8_t rssi, uint32_t rat,
				     uint8_t sync_id);
void                fake_rf_set_status(RF_CmdHandle ch, uint16_t status);
void                fake_rf_start(RF_CmdHandle ch);
void                fake_rf_fire(RF_CmdHandle ch, RF_EventMask events);
void                fake_rf_complete(RF_CmdHandle ch, uint16_t status,
				     RF_EventMask events);
void                fake_rf_queue_cb(RF_CmdHandle ch, RF_EventMask events);
uint32_t            fake_rf_pending(void);
bool                fake_rf_deliver_one(void);
uint32_t            fake_rf_deliver_pending(void);

#endif /* FAKE_TI_DRIVERS_RF_RF_H_ */
