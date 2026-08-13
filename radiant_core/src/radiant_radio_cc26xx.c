/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_cc26xx.c - radiant_radio_hal.h on TI's CC13x2/CC26x2 RF core.
 *
 * Provenance: clean-room. Written against radiant_radio_hal.h, against
 * docs/ant-radio-link.md, against TI's public CC13x2/CC26x2 Technical
 * Reference Manual and driverlib headers, against the public SimpleLink RF
 * driver API, and against the PHY parameters measured by
 * radiant_core/spike/ti_phy on 2026-08-13. Nothing here derives from sdk-ant,
 * from libant.a, or from an ANT+ device profile document. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * Written against the HAL contract and the spike's measurements, deliberately
 * NOT against radiant_radio_nrf.c - the two radios disagree on most of what
 * matters here, so a port-by-analogy would inherit the wrong shape:
 *
 *   - The nRF RADIO is a register peripheral driven by PPI and an ISR. This
 *     is a COMMAND PROCESSOR: software posts an operation to a doorbell and a
 *     separate CPU in the RF core executes it, trigger programmed before the
 *     command is posted.
 *   - The nRF matcher has one shared BASE and eight prefixes. This has two
 *     fully independent sync words and nothing else, so max_filters and
 *     max_addr_groups are both 2.
 *   - The nRF matches all five bytes of a tracking address. This matches at
 *     most four (formatConf.nSwBits is 8..32), so the fifth arrives as a
 *     payload byte and the match finishes in software - anticipated by the
 *     HAL's caps.addr_len_hw_max and filter_index contract.
 *
 * THE NUMBERS THIS FILE RESTS ON:
 *
 *   MEASURED (spike/ti_phy on a LAUNCHXL-CC26X2R1, 2026-08-13):
 *     rxBw = 98 (1567.2 kHz), aaFilter = 0xB   40 of 40 frames CRC-valid,
 *                                              nRxNok = 0, ~-67 dBm
 *     RAT = 4.000 MHz                          4 000 244 ticks in 1 000 ms
 *     RAT free-runs across RF_close/RF_open    200 337 850 ticks over 5 cycles
 *     max_filters = 2, max_addr_groups = 2
 *     the matcher reaches 4 bytes    but this backend uses 3 - see
 *                                    setup_cmd.formatConf for why
 *
 *   SEEDED, NOT MEASURED (P3 bench items): T_SYNC_CAL_US, TX_RAMP_UP_US,
 *   RX_RAMP_UP_US, MIN_ARM_LEAD_US, RX_TO_TX_US, TX_TO_RX_US - deliberately
 *   pessimistic where pessimism is safe (too large costs schedulable gap;
 *   too small looks like poor sensitivity). Only T_SYNC_CAL_US has no safe
 *   direction - see its own comment.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <driverlib/rf_mailbox.h>
#include <driverlib/rf_common_cmd.h>
#include <driverlib/rf_prop_cmd.h>
#include <driverlib/rf_prop_mailbox.h>
#include <driverlib/rf_data_entry.h>
#include <rf_patches/rf_patch_cpe_prop.h>

#include <ti/drivers/rf/RF.h>

#include <radiant_core/radiant_radio_hal.h>

LOG_MODULE_REGISTER(radiant_cc26xx, CONFIG_RADIANT_CORE_LOG_LEVEL);

/* ---------------------------------------------------------------------------
 * The PHY - every value measured by the spike or taken from SmartRF's own
 * settings database; nothing here is a guess.
 * ---------------------------------------------------------------------------
 */

#define ANT_RF_INDEX_DEFAULT   57u
#define RAT_HZ                 4000000u
#define RAT_TICKS_PER_US       (RAT_HZ / 1000000u)   /* 4 */

/* 1 Mbps: one on-air byte is 8 us. Used for every airtime calculation below,
 * and the reason none of them is a magic number. */
#define BYTE_US                8u

/* ANT's preamble is one byte and runs continuously into the first address bit
 * (docs/ant-radio-link.md). preamMode 2 - "same first bit in preamble and sync
 * word" - is what reproduces that; SmartRF's own settings use four bytes and
 * preamMode 0, which is right for their PHYs and silent on this one. */
#define PREAMBLE_BYTES         1u

/*
 * Anti-aliasing filter bandwidth, left at TI's tc901 value. Swept and found
 * not to matter once the settings that DO matter (AGC_REF_VALUE, deviation)
 * are right: 0x5 and 0xB scored 96%/93% of frames on the same run, within
 * run-to-run spread.
 */
#define AA_FILTER_OVERRIDE_INDEX 2
#define AA_FILTER_VALUE          0x5

/*
 * AGC reference level - tc901's default (0x22) is wrong for this PHY. It's
 * SmartRF's value for a 250 kbps / 530 kHz PHY, copied in without checking it
 * against a PHY 4x the rate and 3x the bandwidth. Measured against
 * tools/ant_sim.py at 4.0049 Hz / -47 dBm (% of frames sent that were
 * received):
 *
 *   0x22   37%, 47%, 47%
 *   0x2E   87%, 90%, 95%, 97%      <- TI's own default
 *   0x34   91%, 93%, 96%, 96%, 97%, 100%
 *   0x3A   87%, 93%
 *
 * 0x34 taken over TI's 0x2E because it led every round.
 */
#define AGC_REF_OVERRIDE_INDEX   3
#define AGC_REF_VALUE            0x0034

/*
 * RX filter bandwidth code. 98 = 1567.2 kHz. Earlier sweeps favored 96
 * (1092.5 kHz) while compensating for a wrong deviation (see
 * DEVIATION_UNITS); at 250 kHz deviation Carson's rule wants
 * 2*(250+500) = 1500 kHz, and 98 is the code above that. With deviation and
 * AGC fixed, 98 beats 96 (96% vs 93%) as the arithmetic predicts.
 *
 * Percentages here are fractions of frames a known 4.0049 Hz transmitter
 * actually sent, not frames already detected - the latter denominator hides
 * a PHY that drops more than half of everything.
 */
#define RX_BW_CODE               98

/*
 * Deviation, in units of 250 Hz. docs/ant-radio-link.md gives ~170 kHz;
 * measured, 1000 units (250 kHz) is at least as good at every AGC level
 * tried, and matches modulation index 0.5 (250 kHz dev at 1 Mbps), same as a
 * BLE-shaped 1 Mbps GFSK link.
 *
 * This is the deviation the DEMODULATOR expects, a receiver-side setting -
 * not a claim about what ANT transmits. If a future measurement pins ANT's
 * real deviation at 170 kHz, that argues for splitting the RX/TX fields
 * rather than reverting this one, since only the RX number is measured.
 */
#define DEVIATION_UNITS          1000

static uint32_t phy_overrides[] = {
	/* override_prop_common.json: "DC/DC regulator: In Tx, use
	 * DCDCCTL5[3:0]=0x3 (DITHER_EN=0 and IPEAK=3)." */
	(uint32_t)0x00F388D3,
	/* override_tc901.json: "Tx: Configure PA ramp time, PACTL2.RC=0x1" */
	ADI_2HALFREG_OVERRIDE(0, 16, 0x8, 0x8, 17, 0x1, 0x0),
	/* override_tc901.json: anti-aliasing filter bandwidth. See
	 * AA_FILTER_VALUE. */
	ADI_HALFREG_OVERRIDE(0, 61, 0xF, AA_FILTER_VALUE),
	/* override_tc901.json: AGC reference level, overridden. See
	 * AGC_REF_VALUE. */
	HW_REG_OVERRIDE(0x609C, AGC_REF_VALUE),
	/* override_hposc.json */
	HPOSC_OVERRIDE(0),
	(uint32_t)0xFFFFFFFF
};

static volatile rfc_CMD_PROP_RADIO_DIV_SETUP_t setup_cmd = {
	.commandNo = CMD_PROP_RADIO_DIV_SETUP,
	.condition.rule = COND_NEVER,
	.modulation = {
		.modType = 1,          /* 2-GFSK */
		.deviation = DEVIATION_UNITS,
		.deviationStepSz = 0,
	},
	.symbolRate = {
		.preScale = 15,
		.rateWord = 655360,    /* 1 Mbps, TRM 25.10.5.2 */
		.decimMode = 0,
	},
	.rxBw = RX_BW_CODE,
	.preamConf = {
		.nPreamBytes = PREAMBLE_BYTES,
		.preamMode = 2,
	},
	.formatConf = {
		/*
		 * FIXED AT 24 BITS FOR EVERY OPERATION.
		 *
		 * nSwBits lives in the SETUP command, consumed by RF_open() - it
		 * cannot be rewritten under a live handle; assigning it between
		 * operations changes RAM and nothing in the radio (measured: a
		 * first draft did exactly that and silently heard nothing).
		 *
		 * ANT's two geometries need different match lengths: search
		 * matches [A6 C5 devnum_lo], tracking matches
		 * [A6 C5 dnl dnh dtype]. Re-running CMD_PROP_RADIO_DIV_SETUP per
		 * length would need a setup->CMD_FS->op chain inside
		 * min_arm_lead_us, from callback context where nothing may
		 * block - not viable. Fixing nSwBits at 32 is impossible (the
		 * search address is only 3 bytes on air). So it is fixed at 24,
		 * the 3 bytes both geometries share, with the rest finished in
		 * software - costing two extra compared bytes and some spurious
		 * wake-ups on shared devnum_lo, for a backend with no
		 * reconfiguration path in arm calls. caps.addr_len_hw_max
		 * reports 3 accordingly (the HAL treats it as informational).
		 */
		.nSwBits = 24,
		.bBitReversal = 0,
		/*
		 * bMsbFirst = 1. docs/ant-radio-link.md's bit-reversal note is
		 * specific to the nRF's address serialiser (LSBit-first address,
		 * MSBit-first payload under ENDIAN=Big); the real invariant is
		 * on-air order, and ANT is MSBit-first throughout. A radio that
		 * serialises sync word and payload the same way needs no
		 * reversal. Wrong here means silence, not a weak link - measured
		 * correct by the spike.
		 */
		.bMsbFirst = 1,
		.fecMode = 0,
		.whitenMode = 0,       /* ANT does not whiten */
	},
	.config = {
		.frontEndMode = 0,     /* differential; the LaunchXL's balun */
		.biasMode = 0,
		.analogCfgMode = 0,
		.bNoFsPowerUp = 0,
	},
	.txPower = 0x7217,             /* SmartRF's 2.4 GHz value, tc901 */
	.pRegOverride = phy_overrides,
	.centerFreq = 2400u + ANT_RF_INDEX_DEFAULT,
	.intFreq = 0x0800,
	.loDivider = 0,                /* 0 = 2.4 GHz band; "DIV" is not a divider here */
};

static volatile rfc_CMD_FS_t fs_cmd = {
	.commandNo = CMD_FS,
	.condition.rule = COND_NEVER,
	.frequency = 2400u + ANT_RF_INDEX_DEFAULT,
	.fractFreq = 0,
	.synthConf.bTxMode = 0,
	.synthConf.refFreq = 0,
};

/*
 * RF_MODE_PROPRIETARY_2_4 + rf_patch_cpe_prop is SmartRF's own 2.4 GHz
 * proprietary setting for this part, proven by the spike. RF_MODE_MULTIPLE
 * would be needed for ANT-beside-BLE; not chosen here to avoid answering two
 * questions at once.
 *
 * The RF core keeps a pointer to this, so it may not live on a stack.
 */
static RF_Mode rf_mode = {
	.rfMode = RF_MODE_PROPRIETARY_2_4,
	.cpePatchFxn = &rf_patch_cpe_prop,
};

/* ---------------------------------------------------------------------------
 * Timing constants that are SEEDED and must be measured in P3
 * ---------------------------------------------------------------------------
 */

/*
 * T_SYNC_CAL_US - correction from the hardware's captured timestamp to the
 * HAL's t_sync ("last bit of the on-air address at the antenna"). rx_t_sync()
 * adds airtime for the address bytes not in the sync word, then this
 * constant, which absorbs demodulator group delay, filter latency and
 * capture offset.
 *
 * ZERO IS A PLACEHOLDER, NOT A MEASUREMENT, AND HAS NO SAFE DIRECTION. A
 * constant error here cancels out of the period estimate - the drift PLL
 * still locks and nothing looks wrong - while every RX window silently
 * shifts and eats its guard on one side, changing yield by a few tenths of a
 * percent (near the bench's ~0.4% collision floor). Measuring it needs a
 * wired two-board trigger (one pulses GPIO on its own address-sent event,
 * the other captures the pulse on the timer it timestamps t_sync with).
 * Until done, the A/B `timing` gate against the nRF backend only bounds the
 * error rather than measuring it.
 */
#define T_SYNC_CAL_US          0

/*
 * Ramp-up and turnaround. Seeded from the CC26x2 datasheet's TX/RX turnaround
 * figures plus margin for the RF core's command dispatch, and deliberately
 * generous: too large costs schedulable gap and is visible as a scheduler
 * statistic, too small produces late windows that read as poor sensitivity.
 */
#define TX_RAMP_UP_US          150
#define RX_RAMP_UP_US          150

/*
 * EXTRA RECEIVE TIME AT THE LATE EDGE OF EVERY WINDOW.
 *
 * The HAL's window is [t_open, t_close] in t_sync terms; ending exactly at
 * t_close + one byte is correct arithmetic but measured 79% loss:
 *
 *   end = t_close + BYTE_US            79.0% loss   (126 of 601 packets)
 *   end = t_close + BYTE_US +  500 us   8.3% loss   (551 of 601)
 *   end = t_close + BYTE_US + 2000 us   9.0% loss   (547 of 601)
 *
 * Frames weren't arriving late (543 of 554 landed 0-200us before t_close
 * once slop was added). The problem: endTrigger stops SYNC SEARCH, not just
 * reception. endType = 1 lets an already-started packet finish, but gives
 * the correlator no run-up to start one - and a tracked window is only
 * ~400us wide, so a to-the-microsecond end trigger leaves almost no armed
 * time at the edge where the frame is. Missing it there loses the ANCHOR,
 * not just one packet: the channel free-runs and misses several more after.
 *
 * 500 and 2000 measure the same, so 500 is taken as the bounded cost - extra
 * receive current in exchange for a correlator that's running before the
 * frame arrives.
 */
#define RX_END_SLOP_US         500
#define RX_TO_TX_US            250
#define TX_TO_RX_US            250

/*
 * MIN_ARM_LEAD_US - between radiant_radio_now() and the earliest instant an
 * arm call can still honour. Larger than the nRF backend's because an arm
 * here posts a command through a driver to a separate processor's doorbell,
 * not a register write - covers building the command, RF_postCmd's own
 * work, and the RF core waking its command processor.
 */
#define MIN_ARM_LEAD_US        600

/*
 * ARM_FLOOR_US - the lead below which an arm is REFUSED, distinct from the
 * lead the scheduler budgets for (min_arm_lead_us). Needed as two numbers:
 * radiant_sched.c aims at now + min_arm_lead_us, and the microseconds elapsed
 * between its decision and this function's clock read eat into that margin.
 * With one number, real arms landed ~55us short every time (measured:
 * "rx refused rc=-4 ... open=2573983 now=2573438", 545us of lead against a
 * 600us test) - clean empty scans with no counter moving.
 *
 * So min_arm_lead_us stays the honest budgeting figure, and ARM_FLOOR_US is
 * the true hardware limit: below it the trigger would already be past, and
 * pastTrig = 1 makes that start the window late rather than erroring. A
 * window opened between the floor and the advertised lead loses part of its
 * head, and the frames missed as a result are counted as ordinary misses.
 */
#define ARM_FLOOR_US           150

/* ---------------------------------------------------------------------------
 * Receive buffers
 *
 * Sized for the worst case the HAL permits, not for ANT: RADIANT_RADIO_BODY_MAX
 * plus the one address byte the hardware matcher can't reach plus two CRC
 * bytes plus the appended RSSI/timestamp/status - costs static RAM only.
 *
 * FOUR ENTRIES: a merged window may deliver several frames back to back, and
 * the draining callback runs at RF driver priority, not the radio's. Two
 * would cover ANT's slot geometry; four is cheap insurance against a burst.
 * ---------------------------------------------------------------------------
 */
#define RX_CRC_BYTES      2u
#define RX_SW_ADDR_MAX    (RADIANT_RADIO_ADDR_MAX - 4u)   /* 1 */
#define RX_PAYLOAD_MAX    (RX_SW_ADDR_MAX + RADIANT_RADIO_BODY_MAX + RX_CRC_BYTES)
#define RX_APPEND_BYTES   (1u /* RSSI */ + 4u /* RAT */ + 1u /* status */)
#define RX_BUF_BYTES      (RX_PAYLOAD_MAX + RX_APPEND_BYTES)
#define RX_ENTRIES        4

/*
 * EVERY ENTRY 4-BYTE ALIGNED INDIVIDUALLY - __aligned(4) ON THE ARRAY ONLY
 * ALIGNS ROW 0.
 *
 * The RF core requires each rfc_dataEntryGeneral_t to be word aligned. Later
 * rows start at base + i*rowsize, so they're aligned only if rowsize is a
 * multiple of four; the natural row here is 33 bytes (rows 1..3 land on odd
 * addresses). Measured as total receive failure with no error: the RF core
 * filled entry 0 (nRxOk = 1, FINISHED), advanced to the misaligned entry 1,
 * and entry 0's frame was never delivered.
 *
 * ROUND_UP on the row fixes it, at three bytes per entry.
 */
#define RX_ROW_BYTES  ROUND_UP(sizeof(rfc_dataEntryGeneral_t) + RX_BUF_BYTES, 4)

static uint8_t rx_data[RX_ENTRIES][RX_ROW_BYTES] __aligned(4);
static dataQueue_t rx_queue;
static rfc_propRxOutput_t rx_output;

/*
 * The read cursor is software's, not rx_queue.pCurrEntry: that field is
 * where the RF core will WRITE next and advances as it fills entries.
 * Draining from it reads the entry the radio is about to use (PENDING), so
 * the loop breaks immediately and the finished entry behind it is missed.
 * TI's own rfQueue helper keeps a separate read pointer for this reason.
 */
static uint8_t rx_read;

static volatile rfc_CMD_PROP_RX_ADV_t rx_cmd = {
	.commandNo = CMD_PROP_RX_ADV,
	.condition.rule = COND_NEVER,
	.pktConf = {
		.bFsOff = 0,
		.bRepeatOk = 1,     /* a merged window may catch several frames */
		.bRepeatNok = 1,
		/*
		 * bUseCrc = 0: the CRC is verified in SOFTWARE, and that is a
		 * capability statement rather than a shortcut - caps.crc_in_hw
		 * is false and caps.has_rx_crc is true because of this line.
		 *
		 * The RF core's CRC engine cannot express ANT's: ANT's CRC
		 * covers the on-air ADDRESS bytes as well as the body
		 * (radiant_crc_cfg.cover_addr), and the sync-word matcher
		 * consumes those bytes before the CRC engine ever sees them.
		 * That is the usual sticking point the HAL's CRC section names,
		 * and it is real here.
		 *
		 * The cost is CPU per frame and no hardware suppression of
		 * noise-triggered matches. The benefit, which is not nothing,
		 * is that crc_rx is available on every failure - so
		 * radiant_crc_repair.c's single-bit repair works on this
		 * backend, where a pass/fail bit would have disabled it.
		 */
		.bUseCrc = 0,
		.bCrcIncSw = 0,
		.bCrcIncHdr = 0,
		/*
		 * endType = 1: an end trigger that fires while a packet is
		 * being received lets that packet finish. With 0 the frame
		 * would be truncated, and a frame truncated by the window edge
		 * is indistinguishable from a frame corrupted on the air - it
		 * fails its CRC and is counted as loss. That would have put a
		 * floor under the A/B loss_exact gate that no amount of RF work
		 * could lift.
		 */
		.endType = 1,
		.filterOp = 1,      /* mark, do not silently drop */
	},
	.rxConf = {
		.bAutoFlushIgnored = 1,
		.bAutoFlushCrcErr = 0,
		.bIncludeHdr = 0,
		.bIncludeCrc = 0,
		.bAppendRssi = 1,
		.bAppendTimestamp = 1,
		.bAppendStatus = 1,
	},
	.syncWord0 = 0,
	.syncWord1 = 0,
	.maxPktLen = RX_PAYLOAD_MAX,
	.hdrConf = {
		.numHdrBits = 0,    /* ANT has no length field anywhere */
		.lenPos = 0,
		.numLenBits = 0,
	},
	.startTrigger.triggerType = TRIG_ABSTIME,
	.endTrigger.triggerType = TRIG_ABSTIME,
	.pQueue = &rx_queue,
	.pOutput = (uint8_t *)&rx_output,
};

/* Address bytes the matcher could not reach, then body, then CRC. */
static uint8_t tx_buf[RX_SW_ADDR_MAX + RADIANT_RADIO_BODY_MAX + RX_CRC_BYTES];

static volatile rfc_CMD_PROP_TX_ADV_t tx_cmd = {
	.commandNo = CMD_PROP_TX_ADV,
	.condition.rule = COND_NEVER,
	.pktConf = {
		.bFsOff = 0,
		.bUseCrc = 0,       /* software CRC, appended into the body */
		.bCrcIncSw = 0,
		.bCrcIncHdr = 0,
	},
	.numHdrBits = 0,
	.pktLen = 0,
	.startTrigger.triggerType = TRIG_ABSTIME,
	.syncWord = 0,
	.pPkt = tx_buf,
};

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------
 */

enum op_kind {
	OP_NONE = 0,
	OP_RX,
	OP_TX,
};

static struct {
	const struct radiant_radio_cbs *cbs;
	void                       *user;

	RF_Object   rf_object;
	RF_Handle   rf_handle;
	RF_CmdHandle cmd_handle;

	bool        inited;
	bool        enabled;

	enum op_kind kind;
	uint32_t     op_id;
	uint32_t     next_op_id;

	/* The armed request, copied because the HAL only guarantees the
	 * filters array outlives the operation - not the request struct. */
	struct radiant_pkt_format fmt;
	struct radiant_rx_filter  filters[8];
	uint8_t                   n_filters;
	uint8_t                   hw_addr_len;
	uint8_t                   sw_addr_len;
	radiant_time_t            t_open;
	radiant_time_t            t_close;
	uint32_t                  flags;
	radiant_time_t            tx_t_sync;

	/* Which filter each hardware sync word stands for, as an index into
	 * filters[]; -1 when that sync word is unused. Only the first
	 * hw_addr_len bytes are implied - software finishes the rest. */
	int8_t                    sync_group[2];

	/* 64-bit fold of the 32-bit RAT. See rat_now(). */
	uint32_t                  rat_last;
	uint64_t                  rat_hi;
	int64_t                   rat_uptime_ref;

	bool                      delivered_terminal;
} st;

static struct k_timer rat_keepalive;

/* ---------------------------------------------------------------------------
 * Time: folding a 32-bit 4 MHz counter into 64-bit microseconds
 *
 * The RAT wraps every 2^32 / 4e6 = 1073s (~18 min). The HAL needs 64-bit
 * absolute microseconds the core can subtract freely, so a dongle left
 * plugged in over a weekend must not develop a scheduling bug at minute 18.
 *
 * The fold (detect a step backwards, add 2^32) is correct only if sampled
 * more often than the wrap period. radiant_radio_now() is called constantly
 * in practice, but a k_timer also forces a sample every 60s regardless, to
 * cover a disabled radio, an idle dongle, or a long USB stall.
 *
 * The spike measured the RAT free-running across RF_close()/RF_open()
 * (200,337,850 ticks over five power cycles, matching elapsed wall time),
 * so there's no epoch to re-establish on enable - disable()/enable() doesn't
 * perturb the timebase.
 * ---------------------------------------------------------------------------
 */

static uint64_t rat_fold(uint32_t now32)
{
	if (now32 < st.rat_last) {
		st.rat_hi += (uint64_t)1 << 32;
	}
	st.rat_last = now32;

	return st.rat_hi | now32;
}

static uint32_t rat_raw(void)
{
	if (st.rf_handle != NULL) {
		return RF_getCurrentTime();
	}

	/* No RAT to read before RF_open / after RF_close, but the HAL still
	 * needs a monotonic answer. A k_uptime-derived one is good enough here:
	 * the only callers in that state are deciding whether it's worth
	 * arming anything, not scheduling against it. */
	return (uint32_t)((uint64_t)k_uptime_get() * 1000u * RAT_TICKS_PER_US);
}

static radiant_time_t now_us(void)
{
	unsigned int key = irq_lock();
	uint64_t ticks = rat_fold(rat_raw());

	irq_unlock(key);

	return (radiant_time_t)(ticks / RAT_TICKS_PER_US);
}

static void rat_keepalive_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	(void)now_us();
}

/* The RAT tick a given absolute microsecond corresponds to, truncated to the
 * 32 bits a command's trigger field holds. */
static uint32_t us_to_rat(radiant_time_t t)
{
	return (uint32_t)((uint64_t)t * RAT_TICKS_PER_US);
}

/* ---------------------------------------------------------------------------
 * CRC - generic over struct radiant_crc_cfg rather than hardcoded to
 * CCITT-FALSE, since a hardcoded backend would silently transmit the wrong
 * CRC the moment a different format (e.g. the long-range format from ADR
 * 0007) arrived.
 * ---------------------------------------------------------------------------
 */

static uint32_t reflect(uint32_t v, uint8_t bits)
{
	uint32_t r = 0;

	for (uint8_t i = 0; i < bits; i++) {
		if (v & ((uint32_t)1 << i)) {
			r |= (uint32_t)1 << (bits - 1u - i);
		}
	}

	return r;
}

static uint32_t crc_compute(const struct radiant_crc_cfg *cfg,
			    const uint8_t *p, size_t n)
{
	const uint8_t width = cfg->width_bits;
	const uint32_t top = (uint32_t)1 << (width - 1u);
	const uint32_t mask = (width == 32u) ? 0xFFFFFFFFu
					     : (((uint32_t)1 << width) - 1u);
	uint32_t crc = cfg->init & mask;

	for (size_t i = 0; i < n; i++) {
		uint32_t b = p[i];

		if (cfg->reflect_in) {
			b = reflect(b, 8);
		}

		crc ^= b << (width - 8u);
		for (int k = 0; k < 8; k++) {
			crc = (crc & top) ? ((crc << 1) ^ cfg->poly) : (crc << 1);
			crc &= mask;
		}
	}

	if (cfg->reflect_out) {
		crc = reflect(crc, width);
	}

	return (crc ^ cfg->xor_out) & mask;
}

/* ---------------------------------------------------------------------------
 * Capabilities
 * ---------------------------------------------------------------------------
 */

static const enum radiant_phy phys[] = { RADIANT_PHY_1M_GFSK };

static const struct radiant_radio_caps caps = {
	.name = "cc26xx",

	/*
	 * TWO - A KNOWN PRODUCT REGRESSION. CMD_PROP_RX_ADV has only
	 * syncWord0/syncWord1 vs the nRF's eight. radiant_search.c sweeps
	 * max_filters addresses per window to cover all 256 devnum_lo values:
	 * 32 sets at eight filters, 128 sets at two - breaking
	 * ab_gates.toml's max_absolute_s = 5.0 by construction.
	 *
	 * RECORD the measured discovery time and STATE the regression rather
	 * than tuning the gate to fit; decide separately whether the sweep
	 * needs a different strategy at low filter counts.
	 */
	.max_filters = 2,

	/*
	 * Equal to max_filters, unlike the nRF. Each sync word is a fully
	 * independent 32-bit value with no shared-BASE constraint, so two
	 * tracked channels with different device numbers CAN share a window -
	 * the nRF's max_addr_groups = 2 comes from register layout, this one
	 * just from there being two sync words. Same number, unrelated reason,
	 * and tracking isn't the extra penalty here that it is on the nRF.
	 */
	.max_addr_groups = 2,

	/*
	 * FOUR BITS - the most expensive difference from the nRF. The two sync
	 * words are matched by a correlator, not a comparator, so it can't
	 * separate close templates. Measured at -47 dBm, 4.0049 fps, syncWord1
	 * on the real address and syncWord0 a controlled distance away (out of
	 * ~30 receivable frames per point):
	 *
	 *     1 bit apart    0 frames      4 bits apart   18 frames
	 *     2 bits apart   3 frames      8 bits apart   20 frames
	 *
	 * A single sync word scores 21-24 on the same run, so 4 bits is where
	 * the penalty stops mattering. Declared rather than worked around
	 * because the backend doesn't choose the addresses it's handed - a
	 * search sweep using consecutive devnum_lo values (1 bit apart, free
	 * on the nRF) made this backend receive nothing all sweep with no
	 * error reported.
	 */
	.min_filter_hamming_bits = 4,

	.filter_wildcard_dev = false,

	/*
	 * THREE - less than the silicon can do, deliberately. formatConf.nSwBits
	 * goes to 32 bits (4 bytes), but it lives in the setup command RF_open()
	 * consumed, so serving ANT's two address lengths from one open handle
	 * means fixing it at the 3 bytes they share (see setup_cmd.formatConf).
	 * The 4th/5th on-air bytes on a tracked channel arrive as leading
	 * payload bytes and are matched in software (rx_match()) - more
	 * spurious wakeups and receive current, identical delivered frames.
	 */
	.addr_len_hw_max = 3,

	.max_body_len = RADIANT_RADIO_BODY_MAX,

	.phys = phys,
	.n_phys = ARRAY_SIZE(phys),
	.phy_switch_us = 0,

	.ramp_up_us = TX_RAMP_UP_US,
	.rx_to_tx_us = RX_TO_TX_US,
	.tx_to_rx_us = TX_TO_RX_US,
	.min_arm_lead_us = MIN_ARM_LEAD_US,
	/* Zero: this backend OWNS the radio, so the in-grant lead is the same
	 * lead. The two come apart only under an arbiter. */
	.min_arm_lead_in_grant_us = 0,

	.time_resolution_ns = 250,   /* RAT is 4 MHz - measured 4 000 244 ticks/s */

	.max_window_us = 0,          /* unbounded: nothing is arbitrating */

	/* Appended timestamp is a hardware RAT capture, not a software read.
	 * Calibration (T_SYNC_CAL_US) is still outstanding, but that's a
	 * constant offset, not a loss of hardware capture - reporting false
	 * here would make radiant_event.c wrongly mark the timestamp approximate. */
	.has_sync_timestamp = true,

	.has_rssi = true,
	.has_ed_scan = false,        /* see radiant_radio_ed() */
	.has_rx_crc = true,          /* software CRC: the received value is kept */
	.crc_in_hw = false,          /* see rx_cmd.pktConf.bUseCrc */

	/*
	 * ONE POWER, AND caps NOW SAYS SO. This used to read -20..+5, a
	 * capability the backend didn't actually have: radiant_radio_tx() never
	 * looked at req->power, and setup_cmd.txPower is fixed at RF_open()
	 * time (0x7217, SmartRF's +5 dBm for this band/front end) - so a host
	 * setting transmit power was answered cheerfully and ignored.
	 *
	 * Surfaced by tools/ant_sens.py, whose sensitivity ladder walks the
	 * transmitter's power down and checks its dial against measured RSSI.
	 *
	 * A real range needs RF_setTxPower() plus an RF_TxPowerTable_Entry table
	 * for this part/band/front end from SmartRF Studio, not derivable from
	 * this tree - left as named future work rather than guessed register
	 * values.
	 */
	.tx_power_min_dbm = 5,
	.tx_power_max_dbm = 5,
};

const struct radiant_radio_caps *radiant_radio_caps_get(void)
{
	return &caps;
}

/* ---------------------------------------------------------------------------
 * Helpers
 * ---------------------------------------------------------------------------
 */

/* The first hw_len address bytes as the RF core wants them: first byte on the
 * air in the most significant position of the nSwBits used. */
static uint32_t sync_word_of(const uint8_t *addr, uint8_t hw_len)
{
	uint32_t w = 0;

	for (uint8_t i = 0; i < hw_len; i++) {
		w = (w << 8) | addr[i];
	}

	return w;
}

static bool fmt_supported(const struct radiant_pkt_format *fmt)
{
	if (fmt == NULL) {
		return false;
	}
	if (fmt->phy != RADIANT_PHY_1M_GFSK) {
		return false;
	}
	if (fmt->addr_len < 2u || fmt->addr_len > RADIANT_RADIO_ADDR_MAX) {
		return false;
	}
	/*
	 * RADIANT_LEN_FROM_BODY is refused rather than approximated. The RF
	 * core can decode a length field (hdrConf.numLenBits), but only counted
	 * from the first bit after the sync word - it can't express a length
	 * field sitting after an address byte the matcher didn't consume (the
	 * device type byte, on a 5-byte address). No ANT format needs this
	 * mode; the long-range format does, but it's on a PHY this build
	 * doesn't have either.
	 */
	if (fmt->len_mode != RADIANT_LEN_FIXED) {
		return false;
	}
	if (fmt->body_len > RADIANT_RADIO_BODY_MAX) {
		return false;
	}
	if (fmt->crc.width_bits != 0u &&
	    (fmt->crc.width_bits != 16u || !fmt->crc.cover_addr)) {
		/*
		 * Only "no CRC" and "16-bit covering the address" are
		 * expressible: the software path reassembles the covered bytes
		 * from the filter's address plus payload, which is what
		 * cover_addr means. A non-covering 16-bit CRC would be easy to
		 * add but has never been asked for; refusing beats an untested
		 * path.
		 */
		return false;
	}

	return true;
}

static void deliver_rx_terminal(enum radiant_radio_status status)
{
	struct radiant_rx_event evt = {
		.op = st.op_id,
		.status = status,
	};

	if (st.delivered_terminal) {
		return;
	}
	st.delivered_terminal = true;
	st.kind = OP_NONE;

	if (st.cbs != NULL && st.cbs->rx != NULL) {
		st.cbs->rx(&evt, st.user);
	}
}

static void deliver_tx_terminal(enum radiant_radio_status status)
{
	struct radiant_tx_event evt = {
		.op = st.op_id,
		.status = status,
		.t_sync = st.tx_t_sync,
		.t_sync_exact = (status == RADIANT_RADIO_STATUS_OK),
	};

	if (st.delivered_terminal) {
		return;
	}
	st.delivered_terminal = true;
	st.kind = OP_NONE;

	if (st.cbs != NULL && st.cbs->tx != NULL) {
		st.cbs->tx(&evt, st.user);
	}
}

/* ---------------------------------------------------------------------------
 * Receive
 * ---------------------------------------------------------------------------
 */

static void rx_queue_init(void)
{
	memset(rx_data, 0, sizeof(rx_data));

	for (int i = 0; i < RX_ENTRIES; i++) {
		rfc_dataEntryGeneral_t *e = (rfc_dataEntryGeneral_t *)rx_data[i];

		e->pNextEntry = (i + 1 < RX_ENTRIES) ? rx_data[i + 1] : rx_data[0];
		e->status = DATA_ENTRY_PENDING;
		e->config.type = DATA_ENTRY_TYPE_GEN;
		e->config.lenSz = 0;
		e->length = RX_BUF_BYTES;
	}

	rx_queue.pCurrEntry = rx_data[0];
	rx_queue.pLastEntry = NULL;
	rx_read = 0;
}

/*
 * Which filter this frame is for, or -1. The hardware matched the first
 * hw_addr_len bytes and reports which sync word did it, narrowing the
 * answer to filters sharing that sync word; remaining address bytes are the
 * leading payload bytes, compared here to finish the match.
 *
 * The return value is the HAL's filter_index and the core depends on it for
 * IDENTITY: in wildcard search the third on-air byte is devnum_lo, so the
 * wrong index doesn't drop a frame, it attributes it to the wrong sensor.
 */
static int rx_match(uint8_t sync_id, const uint8_t *payload)
{
	for (uint8_t i = 0; i < st.n_filters; i++) {
		const struct radiant_rx_filter *f = &st.filters[i];

		if (st.sync_group[sync_id] < 0) {
			return -1;
		}
		/* Same sync word? Compare the hardware-matched prefix against
		 * the filter that sync word was programmed from. */
		if (memcmp(f->addr, st.filters[st.sync_group[sync_id]].addr,
			   st.hw_addr_len) != 0) {
			continue;
		}
		if (st.sw_addr_len == 0u ||
		    memcmp(&f->addr[st.hw_addr_len], payload,
			   st.sw_addr_len) == 0) {
			return (int)i;
		}
	}

	return -1;
}

/*
 * t_sync from the appended RAT capture. See T_SYNC_CAL_US for what is and is
 * not measured about this.
 *
 * The capture is taken at the sync-word event, which is after hw_addr_len
 * bytes of address; the HAL wants the end of ALL addr_len of them. The
 * difference is the software-matched bytes' airtime.
 */
static radiant_time_t rx_t_sync(uint32_t rat)
{
	uint64_t folded;
	unsigned int key = irq_lock();

	/*
	 * Fold the capture by its SIGNED DISTANCE FROM NOW, not by OR-ing it
	 * into the current high word. The OR is wrong near a wrap: a capture
	 * just before rollover would get the new high word and land 18
	 * minutes in the future, silently dropping the frame outside
	 * [t_open, t_close] twice an hour. The subtraction below is exact in
	 * 32-bit two's complement for any capture within +-2^31 ticks
	 * (+-9 min) of now, which every real capture is.
	 */
	{
		uint32_t now32 = rat_raw();
		int32_t delta = (int32_t)(rat - now32);

		folded = rat_fold(now32) + (int64_t)delta;
	}
	irq_unlock(key);

	return (radiant_time_t)(folded / RAT_TICKS_PER_US) +
	       (radiant_time_t)(st.sw_addr_len * BYTE_US) +
	       (radiant_time_t)T_SYNC_CAL_US;
}

static void rx_drain(void)
{
	for (;;) {
		rfc_dataEntryGeneral_t *e =
			(rfc_dataEntryGeneral_t *)rx_data[rx_read];
		const uint8_t *p;
		const uint8_t *body;
		uint8_t payload_len;
		uint8_t rssi_raw;
		uint32_t rat;
		rfc_propRxStatus_t status;
		struct radiant_rx_event evt;
		int idx;

		if (e == NULL || e->status != DATA_ENTRY_FINISHED) {
			break;
		}

		p = (const uint8_t *)(&e->data);
		payload_len = (uint8_t)(st.sw_addr_len + st.fmt.body_len +
					(st.fmt.crc.width_bits ? RX_CRC_BYTES : 0u));
		rssi_raw = p[payload_len];
		memcpy(&rat, &p[payload_len + 1u], sizeof(rat));
		memcpy(&status, &p[payload_len + 5u], sizeof(status));

		body = &p[st.sw_addr_len];

		memset(&evt, 0, sizeof(evt));
		evt.op = st.op_id;
		evt.t_sync = rx_t_sync(rat);
		evt.t_sync_exact = true;
		evt.body = body;
		evt.body_len = st.fmt.body_len;
		evt.has_rssi = true;
		evt.rssi_dbm = (int8_t)rssi_raw;

		idx = rx_match(status.status.syncWordId, p);

		/* No per-frame tracing here deliberately: the command status
		 * LOG_WRN in rf_callback() already covers "window armed, radio
		 * ran, nothing came out" at no cost when things work. A LOG_DBG
		 * per frame would block in the RF driver's callback context and,
		 * on the diagnostic build sharing uart0 with the ANT stream,
		 * shred the very broadcasts it's meant to help debug. */

		if (idx < 0) {
			/* The hardware matched a sync word this window did put
			 * on the air, but no filter's full address. That is the
			 * ordinary outcome of matching four bytes where five
			 * were asked for, and it is not an event - the core
			 * never asked to hear this frame. */
			goto next;
		}

		evt.filter_index = (uint8_t)idx;

		/*
		 * Outside [t_open, t_close]. The RX command is started early
		 * enough to hear a frame whose t_sync lands exactly on t_open
		 * and ends late enough to finish one that lands on t_close, so
		 * frames just outside the window are expected rather than
		 * exceptional - and the HAL defines the window in t_sync terms,
		 * so delivering them would widen every window by the preamble
		 * and address airtime.
		 */
		if (evt.t_sync < st.t_open || evt.t_sync > st.t_close) {
			goto next;
		}

		if (st.fmt.crc.width_bits != 0u) {
			uint8_t covered[RADIANT_RADIO_ADDR_MAX +
					RADIANT_RADIO_BODY_MAX];
			size_t n = 0;
			uint32_t want;
			uint32_t got;

			/* The matcher ate the leading address bytes, so put
			 * them back from the filter that matched. */
			memcpy(covered, st.filters[idx].addr, st.fmt.addr_len);
			n = st.fmt.addr_len;
			memcpy(&covered[n], body, st.fmt.body_len);
			n += st.fmt.body_len;

			got = crc_compute(&st.fmt.crc, covered, n);
			want = ((uint32_t)body[st.fmt.body_len] << 8) |
			       body[st.fmt.body_len + 1u];

			evt.has_crc_rx = true;
			evt.crc_rx = want;

				if (got != want) {
				if ((st.flags & RADIANT_RX_REPORT_CRC_FAIL) == 0u) {
					goto next;
				}
				evt.status = RADIANT_RADIO_STATUS_CRC_FAIL;
				evt.body = NULL;
				evt.body_len = 0;
			} else {
				evt.status = RADIANT_RADIO_STATUS_OK;
				evt.has_crc_rx = false;
				evt.crc_rx = 0;
			}
		} else {
			evt.status = RADIANT_RADIO_STATUS_OK;
		}

		if (st.cbs != NULL && st.cbs->rx != NULL) {
			st.cbs->rx(&evt, st.user);
		}

		if (evt.status == RADIANT_RADIO_STATUS_OK &&
		    (st.flags & RADIANT_RX_STOP_ON_FIRST) != 0u) {
			e->status = DATA_ENTRY_PENDING;
			rx_read = (uint8_t)((rx_read + 1u) % RX_ENTRIES);
			RF_cancelCmd(st.rf_handle, st.cmd_handle, 0);
			return;
		}

next:
		e->status = DATA_ENTRY_PENDING;
		rx_read = (uint8_t)((rx_read + 1u) % RX_ENTRIES);
	}
}

static void rf_callback(RF_Handle h, RF_CmdHandle ch, RF_EventMask events)
{
	ARG_UNUSED(h);
	ARG_UNUSED(ch);

	if ((events & RF_EventRxEntryDone) != 0) {
		rx_drain();
	}

	/*
	 * Log the command's own status word whenever it's not a success code.
	 * Every way this backend can fail to hear a frame (rejected trigger,
	 * unacceptable length, unprogrammed synthesiser) looks identical from
	 * the driver's side - RF_EventLastCmdDone, no error, empty window,
	 * scheduler quietly re-arms - and the RF core only distinguishes them
	 * via this 16-bit field.
	 *
	 * PROP_DONE_OK is 0x3400, PROP_DONE_RXTIMEOUT 0x3401; 0x38xx is
	 * PROP_ERROR_*, and 0x0800..0x08FF is a generic command error such as
	 * ERROR_PAST_START.
	 */
	if (st.kind == OP_RX && rx_cmd.status >= 0x3800u) {
		LOG_WRN("rx cmd ended status=0x%04x (start=%u end=%u sw0=%08x)",
			rx_cmd.status, rx_cmd.startTime, rx_cmd.endTime,
			rx_cmd.syncWord0);
	} else if (st.kind == OP_TX && tx_cmd.status >= 0x3800u) {
		LOG_WRN("tx cmd ended status=0x%04x (start=%u)",
			tx_cmd.status, tx_cmd.startTime);
	}

	if ((events & (RF_EventLastCmdDone | RF_EventCmdAborted |
		       RF_EventCmdStopped | RF_EventCmdCancelled)) == 0) {
		return;
	}

	if (st.kind == OP_RX) {
		/* Anything the command processor finished but the entry-done
		 * event did not cover. */
		rx_drain();
		deliver_rx_terminal((events & RF_EventLastCmdDone) != 0
					? RADIANT_RADIO_STATUS_TIMEOUT
					: RADIANT_RADIO_STATUS_ABORTED);
	} else if (st.kind == OP_TX) {
		deliver_tx_terminal((events & RF_EventLastCmdDone) != 0
					? RADIANT_RADIO_STATUS_OK
					: RADIANT_RADIO_STATUS_ABORTED);
	}
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

int radiant_radio_init(const struct radiant_radio_cbs *cbs, void *user)
{
	if (cbs == NULL) {
		return RADIANT_RADIO_EINVAL;
	}

	memset(&st, 0, sizeof(st));
	st.cbs = cbs;
	st.user = user;
	st.next_op_id = 1;
	st.inited = true;

	rx_queue_init();

	k_timer_init(&rat_keepalive, rat_keepalive_fn, NULL);
	k_timer_start(&rat_keepalive, K_SECONDS(60), K_SECONDS(60));

	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_enable(void)
{
	RF_Params params;

	if (!st.inited) {
		return RADIANT_RADIO_ESTATE;
	}
	if (st.enabled) {
		return RADIANT_RADIO_OK_RC;
	}

	RF_Params_init(&params);
	/*
	 * Keep the RF core powered between operations. nInactivityTimeout
	 * defaults to 0, meaning "power down the moment the command queue
	 * drains" - not "never power down" despite the name. Powering down
	 * between windows costs ~1.5ms to recover, more than
	 * caps.min_arm_lead_us, paid on exactly the windows a tracked channel
	 * can't afford to miss. Easy trade on USB-powered hardware; would be
	 * wrong on a coin cell.
	 */
	params.nInactivityTimeout = UINT32_MAX;

	st.rf_handle = RF_open(&st.rf_object, &rf_mode,
			       (RF_RadioSetup *)&setup_cmd, &params);
	if (st.rf_handle == NULL) {
		LOG_ERR("RF_open failed");
		return RADIANT_RADIO_EIO;
	}

	/*
	 * Programme the synthesiser once here rather than per operation: every
	 * ANT+ operation is on the same frequency, and paying FS time inside
	 * every arm would eat into min_arm_lead_us.
	 *
	 * TEST THE BIT, NOT THE VALUE. RF_runCmd's returned RF_EventMask
	 * routinely has more than one bit set on success; comparing the whole
	 * mask against RF_EventLastCmdDone falsely reports a good CMD_FS as
	 * failed, closing the handle and returning EIO from enable() - after
	 * which the radio never reopens and the dongle reports a clean empty
	 * scan forever.
	 */
	if ((RF_runCmd(st.rf_handle, (RF_Op *)&fs_cmd, RF_PriorityNormal,
		       NULL, 0) & RF_EventLastCmdDone) == 0) {
		LOG_ERR("CMD_FS failed, status=0x%04x", fs_cmd.status);
		RF_close(st.rf_handle);
		st.rf_handle = NULL;
		return RADIANT_RADIO_EIO;
	}

	/* Re-anchor the fold on the RAT now that there is one to read. */
	(void)now_us();

	st.enabled = true;
	LOG_INF("enabled: RF core open, synth on %u MHz",
		(unsigned int)fs_cmd.frequency);

	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_disable(void)
{
	if (!st.inited) {
		return RADIANT_RADIO_ESTATE;
	}
	if (!st.enabled) {
		return RADIANT_RADIO_OK_RC;
	}

	(void)radiant_radio_abort();

	/* Sample the RAT one last time so the fold survives the close: it keeps
	 * running, but nothing will read it until the next enable, and a gap
	 * longer than the wrap would lose an epoch. */
	(void)now_us();

	RF_close(st.rf_handle);
	st.rf_handle = NULL;
	st.enabled = false;

	return RADIANT_RADIO_OK_RC;
}

radiant_time_t radiant_radio_now(void)
{
	return now_us();
}

/* ---------------------------------------------------------------------------
 * Arm
 * ---------------------------------------------------------------------------
 */

static int arm_common(const struct radiant_pkt_format *fmt, radiant_time_t at)
{
	if (!st.enabled) {
		return RADIANT_RADIO_ESTATE;
	}
	if (st.kind != OP_NONE) {
		return RADIANT_RADIO_EBUSY;
	}
	if (!fmt_supported(fmt)) {
		return RADIANT_RADIO_ENOTSUP;
	}
	if (at == RADIANT_TIME_NEVER) {
		return RADIANT_RADIO_EINVAL;
	}
	if (at < now_us() + ARM_FLOOR_US) {
		return RADIANT_RADIO_ETIME;
	}

	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_rx(const struct radiant_rx_req *req, uint32_t *op)
{
	uint32_t start_rat;
	uint32_t end_rat;
	int rc;

	if (req == NULL || op == NULL || req->filters == NULL ||
	    req->n_filters == 0u) {
		return RADIANT_RADIO_EINVAL;
	}
	if (req->t_close < req->t_open) {
		return RADIANT_RADIO_EINVAL;
	}
	if (req->n_filters > caps.max_filters) {
		return RADIANT_RADIO_ENOTSUP;
	}

	rc = arm_common(req->fmt, req->t_open);
	if (rc != RADIANT_RADIO_OK_RC) {
		LOG_DBG("rx refused rc=%d n=%u alen=%u open=%llu now=%llu",
			rc, req->n_filters, req->fmt ? req->fmt->addr_len : 0,
			(unsigned long long)req->t_open,
			(unsigned long long)now_us());
		return rc;
	}

	for (uint8_t i = 0; i < req->n_filters; i++) {
		if (req->filters[i].addr_len != req->fmt->addr_len) {
			return RADIANT_RADIO_EINVAL;
		}
	}

	st.fmt = *req->fmt;
	st.n_filters = req->n_filters;
	memcpy(st.filters, req->filters,
	       (size_t)req->n_filters * sizeof(st.filters[0]));
	st.hw_addr_len = MIN(st.fmt.addr_len, caps.addr_len_hw_max);
	st.sw_addr_len = (uint8_t)(st.fmt.addr_len - st.hw_addr_len);
	st.t_open = req->t_open;
	st.t_close = req->t_close;
	st.flags = req->flags;

	/*
	 * Assign the two hardware sync words. Filters sharing a hardware prefix
	 * share a sync word - which is how a tracked window can hold two
	 * channels that differ only in device type without spending both.
	 */
	st.sync_group[0] = -1;
	st.sync_group[1] = -1;
	for (uint8_t i = 0; i < st.n_filters; i++) {
		int slot = -1;

		for (int s = 0; s < 2; s++) {
			if (st.sync_group[s] >= 0 &&
			    memcmp(st.filters[st.sync_group[s]].addr,
				   st.filters[i].addr, st.hw_addr_len) == 0) {
				slot = s;
				break;
			}
		}
		if (slot >= 0) {
			continue;
		}
		for (int s = 0; s < 2; s++) {
			if (st.sync_group[s] < 0) {
				st.sync_group[s] = (int8_t)i;
				slot = s;
				break;
			}
		}
		if (slot < 0) {
			/* More than max_addr_groups distinct prefixes. The HAL
			 * requires a refusal rather than a partial window. */
			return RADIANT_RADIO_ENOTSUP;
		}
	}

	rx_cmd.syncWord0 = sync_word_of(st.filters[st.sync_group[0]].addr,
					st.hw_addr_len);
	if (st.sync_group[1] < 0) {
		/*
		 * ONE GROUP: syncWord1 MUST BE FAR AWAY, NOT EQUAL TO syncWord0.
		 * Duplicating syncWord0 seems safe for a comparator, but this is
		 * a CORRELATOR (caps.min_filter_hamming_bits): two identical
		 * templates are zero bits apart, the worst point on the curve,
		 * and it made every TRACKED window fail (454 EVENT_RX_FAIL
		 * against one packet received). Instead, syncWord1 is syncWord0
		 * with the top matched byte inverted - 8 bits away and a byte
		 * no ANT frame carries (network address starts A6, this makes
		 * it 59) - so it can't match anything real. sync_group[1] still
		 * points at the same filter: since the decoy can't legitimately
		 * fire, a reported syncWordId of 1 can only be misattribution of
		 * the one filter this window has.
		 */
		st.sync_group[1] = st.sync_group[0];
		rx_cmd.syncWord1 = rx_cmd.syncWord0 ^
				   ((uint32_t)0xFFu << (8u * (st.hw_addr_len - 1u)));
	} else {
		rx_cmd.syncWord1 = sync_word_of(st.filters[st.sync_group[1]].addr,
						st.hw_addr_len);
	}
	rx_cmd.maxPktLen = (uint16_t)(st.sw_addr_len + st.fmt.body_len +
				      (st.fmt.crc.width_bits ? RX_CRC_BYTES : 0u));

	/*
	 * The receiver has to be listening before the PREAMBLE of a frame whose
	 * t_sync lands exactly on t_open - the HAL is explicit that defining
	 * the window on "when RX is enabled" instead would make the effective
	 * edge depend on ramp-up and preamble length, which is a silent
	 * per-backend bias.
	 */
	start_rat = us_to_rat(req->t_open -
			      (radiant_time_t)((PREAMBLE_BYTES + st.fmt.addr_len) *
					       BYTE_US) -
			      (radiant_time_t)RX_RAMP_UP_US);
	/*
	 * ...and still listening when a frame whose t_sync lands exactly on
	 * t_close arrives. endType = 1 lets that frame finish; this only has to
	 * cover its address, not its body.
	 */
	end_rat = us_to_rat(req->t_close + (radiant_time_t)BYTE_US +
			    (radiant_time_t)RX_END_SLOP_US);

	/*
	 * pastTrig = 1 on both triggers, not for convenience: with pastTrig = 0
	 * a trigger time already passed is an ERROR (PROP_ERROR_PAR, reported
	 * as plain RF_EventLastCmdDone) indistinguishable from a window that
	 * listened and heard nothing - scheduler re-arms, misses again,
	 * forever. A few microseconds late is ordinary here (command travels
	 * through the driver to another processor's doorbell), so the right
	 * response is to start now and lose the first microseconds, not
	 * refuse the window.
	 */
	rx_cmd.startTrigger.triggerType = TRIG_ABSTIME;
	rx_cmd.startTrigger.pastTrig = 1;
	rx_cmd.startTime = start_rat;
	rx_cmd.endTrigger.triggerType = TRIG_ABSTIME;
	rx_cmd.endTrigger.pastTrig = 1;
	rx_cmd.endTime = end_rat;

	rx_queue_init();
	memset(&rx_output, 0, sizeof(rx_output));

	st.kind = OP_RX;
	st.op_id = st.next_op_id++;
	st.delivered_terminal = false;

	st.cmd_handle = RF_postCmd(st.rf_handle, (RF_Op *)&rx_cmd,
				   RF_PriorityNormal, rf_callback,
				   RF_EventRxEntryDone | RF_EventLastCmdDone |
				   RF_EventCmdAborted | RF_EventCmdStopped |
				   RF_EventCmdCancelled);
	if (st.cmd_handle < 0) {
		st.kind = OP_NONE;
		return RADIANT_RADIO_EIO;
	}

	*op = st.op_id;

	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_tx(const struct radiant_tx_req *req, uint32_t *op)
{
	uint8_t hw_len;
	uint8_t sw_len;
	size_t n = 0;
	int rc;

	if (req == NULL || op == NULL || req->body == NULL) {
		return RADIANT_RADIO_EINVAL;
	}

	rc = arm_common(req->fmt, req->t_sync_at);
	if (rc != RADIANT_RADIO_OK_RC) {
		return rc;
	}

	if (req->addr_len != req->fmt->addr_len ||
	    req->body_len != req->fmt->body_len) {
		return RADIANT_RADIO_EINVAL;
	}

	hw_len = MIN(req->fmt->addr_len, caps.addr_len_hw_max);
	sw_len = (uint8_t)(req->fmt->addr_len - hw_len);

	st.fmt = *req->fmt;
	st.hw_addr_len = hw_len;
	st.sw_addr_len = sw_len;
	st.tx_t_sync = req->t_sync_at;

	/*
	 * req->power is deliberately not read: setup_cmd.txPower is fixed at
	 * RF_open() time (one operating point, tx_power_min_dbm ==
	 * tx_power_max_dbm == 5), so the core has already clamped any request
	 * to the only value available. See the caps comment for what a real
	 * dial would need.
	 */

	/*
	 * Which bytes are "address" vs "body" is a receiver's split, not a
	 * property of the air: transmitting with a 4-byte sync word and the
	 * fifth address byte at the head of the packet produces exactly the
	 * frame a 5-byte matcher expects.
	 */
	memcpy(&tx_buf[n], &req->addr[hw_len], sw_len);
	n += sw_len;
	memcpy(&tx_buf[n], req->body, req->body_len);
	n += req->body_len;

	if (st.fmt.crc.width_bits != 0u) {
		uint8_t covered[RADIANT_RADIO_ADDR_MAX + RADIANT_RADIO_BODY_MAX];
		uint32_t crc;
		size_t c = 0;

		memcpy(covered, req->addr, req->addr_len);
		c = req->addr_len;
		memcpy(&covered[c], req->body, req->body_len);
		c += req->body_len;

		crc = crc_compute(&st.fmt.crc, covered, c);
		tx_buf[n++] = (uint8_t)(crc >> 8);
		tx_buf[n++] = (uint8_t)(crc & 0xFFu);
	}

	tx_cmd.syncWord = sync_word_of(req->addr, hw_len);
	tx_cmd.pktLen = (uint16_t)n;
	tx_cmd.startTrigger.triggerType = TRIG_ABSTIME;
	/* See the note on rx_cmd's triggers: a start time a few microseconds in
	 * the past must start the transmit, not refuse it. A late master frame
	 * is worse than no frame, but "late" here is single-digit microseconds
	 * against a 1.56 ms budget. */
	tx_cmd.startTrigger.pastTrig = 1;
	/*
	 * Work backwards from the t_sync the core asked for, through the
	 * address and preamble airtime and the transmitter's ramp-up, to the
	 * instant the command must start. This is the whole of what makes a
	 * master's period exact by construction rather than exact plus one
	 * backend's ramp-up.
	 */
	tx_cmd.startTime = us_to_rat(req->t_sync_at -
				     (radiant_time_t)((PREAMBLE_BYTES +
						       req->fmt->addr_len) *
						      BYTE_US) -
				     (radiant_time_t)TX_RAMP_UP_US);

	st.kind = OP_TX;
	st.op_id = st.next_op_id++;
	st.delivered_terminal = false;

	st.cmd_handle = RF_postCmd(st.rf_handle, (RF_Op *)&tx_cmd,
				   RF_PriorityNormal, rf_callback,
				   RF_EventLastCmdDone | RF_EventCmdAborted |
				   RF_EventCmdStopped | RF_EventCmdCancelled);
	if (st.cmd_handle < 0) {
		st.kind = OP_NONE;
		return RADIANT_RADIO_EIO;
	}

	*op = st.op_id;

	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_ed(const struct radiant_ed_req *req, uint32_t *op)
{
	ARG_UNUSED(req);
	ARG_UNUSED(op);

	/*
	 * caps.has_ed_scan is false, so this is expected, not degraded. The
	 * part could do it (CMD_PROP_CS, RSSI path), but a measurement whose
	 * scale doesn't match rx_event.rssi_dbm/noise_dbm exactly is worse than
	 * none - establishing "same corrections, same reference" on a new part
	 * is a bench exercise, not a coding one.
	 */
	return RADIANT_RADIO_ENOTSUP;
}

int radiant_radio_abort(void)
{
	if (!st.enabled || st.kind == OP_NONE) {
		return RADIANT_RADIO_OK_RC;
	}

	/*
	 * RF_cancelCmd's callback delivers the terminal event, which is what
	 * the HAL requires: "its terminal event is still delivered... before
	 * this function's caller can observe the slot as free". RF_flushCmd is
	 * synchronous, so by the time it returns the callback has run.
	 */
	RF_flushCmd(st.rf_handle, st.cmd_handle, 0);

	if (st.kind == OP_RX) {
		deliver_rx_terminal(RADIANT_RADIO_STATUS_ABORTED);
	} else if (st.kind == OP_TX) {
		deliver_tx_terminal(RADIANT_RADIO_STATUS_ABORTED);
	}

	return RADIANT_RADIO_OK_RC;
}
