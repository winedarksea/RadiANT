/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_cc26xx.c - radiant_radio_hal.h on TI's CC13x2/CC26x2 RF core.
 *
 * Clean-room: written from the HAL header, docs/ant-radio-link.md, TI's public
 * TRM and driverlib headers, and the radiant/spike/ti_phy measurements. Not
 * derived from sdk-ant, libant.a or ANT+ profile documents (see
 * docs/decisions/0002-clean-room-policy.md).
 *
 * Unlike radiant_radio_nrf.c:
 *   - The RF core is a command processor: ops are posted to a doorbell and run
 *     on a separate CPU, not driven by PPI and an ISR.
 *   - Two independent sync words, so max_filters and max_addr_groups are 2.
 *   - At most four address bytes are matched in hardware (nSwBits 8..32); the
 *     rest are compared in software.
 *
 * Measured on LAUNCHXL-CC26X2R1: rxBw 98, aaFilter 0xB, 40/40 frames CRC-valid
 * at about -67 dBm; RAT 4.000 MHz, free-running across RF_close/RF_open.
 * Seeded, not measured: T_SYNC_CAL_US, TX_RAMP_UP_US, RX_RAMP_UP_US,
 * MIN_ARM_LEAD_US, RX_TO_TX_US, TX_TO_RX_US. These are pessimistic where that
 * is safe. T_SYNC_CAL_US has no safe direction.
 *
 * Measured on air, not inside the part: T_SYNC_CAL_US + TX start error together
 * put an RX->TX turnaround about 100 us early (slave reply at +2085 us against
 * +2190). CONFIG_RADIANT_CC26XX_TX_TIME_CAL_US corrects the sum on the transmit
 * side only, so receive windows do not move. A single clock cannot split the sum.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

#include <driverlib/rf_mailbox.h>
#include <driverlib/rf_common_cmd.h>
#include <driverlib/rf_prop_cmd.h>
#include <driverlib/rf_prop_mailbox.h>
#include <driverlib/rf_data_entry.h>
#include <rf_patches/rf_patch_cpe_prop.h>
#if defined(CONFIG_RADIANT_BACKEND_CC26XX_MULTI_PATCH)
#include <rf_patches/rf_patch_cpe_multi_protocol.h>
#endif

#include <ti/drivers/rf/RF.h>

#include <radiant/radiant_radio_hal.h>

#include <radiant/radiant_diag.h>

#include "radiant_addr_split.h"
/* The RADIANT_CC26XX_PRIO_* values are used unconditionally; the accessor
 * radiant_cc26xx_client() is defined only under coex. */
#include "radiant_radio_cc26xx_arb.h"

/* Only with the option on: see radiant_power_diag.h on why even unused
 * inlines are kept out of a default build. */
#if defined(CONFIG_RADIANT_POWER_DIAG)
#include <radiant/radiant_power_diag.h>
#include "radiant_ti_rf_hwattrs.h"
#endif

LOG_MODULE_REGISTER(radiant_cc26xx, CONFIG_RADIANT_LOG_LEVEL);

/* --- PHY constants: measured on the spike, or from SmartRF's settings --- */

#define ANT_RF_INDEX_DEFAULT   57u
#define RAT_HZ                 4000000u
#define RAT_TICKS_PER_US       (RAT_HZ / 1000000u)   /* 4 */

/* 1 Mbps: one on-air byte is 8 us. All airtime arithmetic uses this. */
#define BYTE_US                8u

/* ANT's one-byte preamble runs straight into the first address bit, which
 * preamMode 2 reproduces. SmartRF's four-byte preamble (preamMode 0) is wrong
 * for ANT. */
#define PREAMBLE_BYTES         1u

/*
 * Anti-aliasing filter bandwidth: TI's tc901 value. Not sensitive once
 * AGC_REF_VALUE and the deviation are right (0x5 and 0xB scored 96% and 93%,
 * within run-to-run spread).
 */
#define AA_FILTER_OVERRIDE_INDEX 2
/* Overridable with CONFIG_RADIANT_CC26XX_AA_FILTER (Kconfig default 0x5). */
#define AA_FILTER_VALUE          CONFIG_RADIANT_CC26XX_AA_FILTER

/*
 * AGC reference level. TI's tc901 default (0x22) is for a 250 kbps PHY, not
 * this 1 Mbps one. Frame delivery at 4.0049 Hz / -47 dBm: 0x22 37-47%,
 * 0x2E 87-97%, 0x34 91-100%, 0x3A 87-93%. The re-sweep found the axis flat
 * from 0x34 up, so do not move it on one packet. Overridable with
 * CONFIG_RADIANT_CC26XX_AGC_REF (Kconfig default 0x34).
 */
#define AGC_REF_OVERRIDE_INDEX   3
#define AGC_REF_VALUE            CONFIG_RADIANT_CC26XX_AGC_REF

/*
 * RX filter bandwidth code. 98 = 1567.2 kHz, the first code above Carson's
 * 2 * (250 + 500) = 1500 kHz for 250 kHz deviation. Re-sweeping found 98 a
 * local optimum, with 97 and 99 both worse by several times the run-to-run
 * noise.
 *
 * Delivery is measured as a fraction of frames the transmitter sent, not of
 * frames detected. The detected-frame figure hides PHYs that drop most frames.
 * Overridable with CONFIG_RADIANT_CC26XX_RX_BW_CODE (Kconfig default 98).
 */
#define RX_BW_CODE               CONFIG_RADIANT_CC26XX_RX_BW_CODE

/*
 * Deviation, in units of 250 Hz. 1000 units (250 kHz) is at least as good as
 * docs/ant-radio-link.md's ~170 kHz at every AGC level tried. It matches
 * modulation index 0.5 at 1 Mbps.
 *
 * This is the demodulator's expected deviation, a receiver-side setting, not a
 * claim about what ANT transmits. If ANT's real deviation is measured at
 * 170 kHz, split the RX and TX fields rather than reverting this one.
 */
#define DEVIATION_UNITS          1000

/*
 * Address bytes the sync-word matcher compares, for every operation (see
 * setup_cmd.formatConf). 3 by default.
 *
 * CONFIG_RADIANT_SEARCH_SHORT_MATCH (ADR 0022) makes it 2, so a search window
 * can take devnum_lo as payload and sweep all device numbers in one set. The
 * cost is that tracking then compares three address bytes in software, and the
 * shorter sync word also fires on noise and other sensors' frames.
 * radiant_cc26xx_sw_addr_mismatch counts that cost.
 */
#if defined(CONFIG_RADIANT_SEARCH_SHORT_MATCH)
#define CC26XX_HW_ADDR_LEN       2u
#else
#define CC26XX_HW_ADDR_LEN       3u
#endif

static uint32_t phy_overrides[] = {
	/* override_prop_common.json: DC/DC dither off, IPEAK 3 in Tx. */
	(uint32_t)0x00F388D3,
	/* override_tc901.json: Tx PA ramp time, PACTL2.RC=0x1. */
	ADI_2HALFREG_OVERRIDE(0, 16, 0x8, 0x8, 17, 0x1, 0x0),
	/* override_tc901.json: anti-aliasing filter (AA_FILTER_VALUE). */
	ADI_HALFREG_OVERRIDE(0, 61, 0xF, AA_FILTER_VALUE),
	/* override_tc901.json: AGC reference level (AGC_REF_VALUE). */
	HW_REG_OVERRIDE(0x609C, AGC_REF_VALUE),
	/* override_hposc.json */
	HPOSC_OVERRIDE(0),
	(uint32_t)0xFFFFFFFF
};

/*
 * Always the _PA layout, even on parts without a PA; do not make this
 * conditional. TI's RF_decodeOverridePointers() reads pRegOverrideTxStd and
 * pRegOverrideTx20 at runtime on CC2652P-class chips. A plain
 * rfc_CMD_PROP_RADIO_DIV_SETUP_t there reads past the object and faults in
 * RF_open(), while the identical binary runs on CC2652R. The _PA command number
 * is the same, and the driver reads the tail only under that runtime check, so
 * the layout is safe on non-PA parts.
 *
 * Both tail pointers stay NULL, which the driver guards. This backend has no
 * high-PA path. Populating them needs a measured 20 dBm PHY and an identified
 * antenna switch (boards/ti/cc2652p_dongle/cc2652p_dongle.dts).
 */
static volatile rfc_CMD_PROP_RADIO_DIV_SETUP_PA_t setup_cmd = {
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
		 * Fixed for every operation. nSwBits is consumed by RF_open(), so it
		 * cannot change under a live handle; writing it later changes RAM
		 * only and the radio hears nothing. Search matches [A6 C5 devnum_lo]
		 * and tracking matches [A6 C5 dnl dnh dtype]. Re-running the setup
		 * per length is not possible from callback context, and 32 bits
		 * exceeds the 3 bytes on air for search. So use the 3 bytes both
		 * share and finish the rest in software. caps.addr_len_hw_max
		 * reports the same figure.
		 *
		 * With CONFIG_RADIANT_SEARCH_SHORT_MATCH this is 16 bits (see
		 * CC26XX_HW_ADDR_LEN).
		 */
		.nSwBits = 8u * CC26XX_HW_ADDR_LEN,
		.bBitReversal = 0,
		/* On-air order is MSB first throughout ANT. Wrong here means silence,
		 * not a weak link. */
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
	/*
	 * RX intermediate frequency, MHz in signed 4.12: 0x1000 = 1.0 MHz. The
	 * tc901 value of 0.5 MHz put the ~1.4 MHz-wide signal across DC and lost
	 * frames from some carrier offsets. See CONFIG_RADIANT_CC26XX_INT_FREQ.
	 */
	.intFreq = CONFIG_RADIANT_CC26XX_INT_FREQ,
	.loDivider = 0,                /* 0 = 2.4 GHz band */
	/* NULL: no high-PA override lists (see the _PA layout above). */
	.pRegOverrideTxStd = NULL,
	.pRegOverrideTx20 = NULL,
};

/*
 * TX power table, 2.4 GHz. Taken from Zephyr's ieee802154_cc13xx_cc26xx.c
 * (Apache-2.0), not SmartRF Studio. The +5 dBm entry equals setup_cmd.txPower
 * 0x7217: RF_TxPowerTable_DEFAULT_PA_ENTRY(23, 0, 0, 57).
 */
static const RF_TxPowerTable_Entry radiant_cc26xx_txp[] = {
	{ -20, RF_TxPowerTable_DEFAULT_PA_ENTRY(6, 3, 0, 2) },
	{ -15, RF_TxPowerTable_DEFAULT_PA_ENTRY(10, 3, 0, 3) },
	{ -10, RF_TxPowerTable_DEFAULT_PA_ENTRY(15, 3, 0, 5) },
	{  -5, RF_TxPowerTable_DEFAULT_PA_ENTRY(22, 3, 0, 9) },
	{   0, RF_TxPowerTable_DEFAULT_PA_ENTRY(19, 1, 0, 20) },
	{   1, RF_TxPowerTable_DEFAULT_PA_ENTRY(22, 1, 0, 20) },
	{   2, RF_TxPowerTable_DEFAULT_PA_ENTRY(25, 1, 0, 25) },
	{   3, RF_TxPowerTable_DEFAULT_PA_ENTRY(29, 1, 0, 28) },
	{   4, RF_TxPowerTable_DEFAULT_PA_ENTRY(35, 1, 0, 39) },
	{   5, RF_TxPowerTable_DEFAULT_PA_ENTRY(23, 0, 0, 57) },
	RF_TxPowerTable_TERMINATION_ENTRY,
};

/* Bench-only carrier trim (CONFIG_RADIANT_CC26XX_FREQ_TRIM_KHZ). fractFreq is
 * in 1/65536 of a MHz; a negative trim borrows one MHz. */
#if defined(CONFIG_RADIANT_CC26XX_FREQ_TRIM_KHZ)
#define FS_TRIM_KHZ CONFIG_RADIANT_CC26XX_FREQ_TRIM_KHZ
#else
#define FS_TRIM_KHZ 0
#endif
#define FS_TRIM_FRACT ((((FS_TRIM_KHZ) + 1000) * 65536 / 1000) % 65536)
#define FS_TRIM_MHZ   ((FS_TRIM_KHZ) < 0 ? -1 : 0)

static volatile rfc_CMD_FS_t fs_cmd = {
	.commandNo = CMD_FS,
	.condition.rule = COND_NEVER,
	.frequency = 2400u + ANT_RF_INDEX_DEFAULT + FS_TRIM_MHZ,
	.fractFreq = FS_TRIM_FRACT,
	.synthConf.bTxMode = 0,
	.synthConf.refFreq = 0,
};

/*
 * The patch is the one every PHY constant here was measured under. The
 * MULTI_PATCH build uses the 802.15.4 patch (same mode value, different
 * function). Both RF core clients must name the same patch: the patch
 * resident at power-up is used for the whole power cycle, since it is never
 * re-applied on a client switch. The RF core keeps a pointer to this, so it
 * must not live on a stack.
 */
static RF_Mode rf_mode = {
#if defined(CONFIG_RADIANT_BACKEND_CC26XX_MULTI_PATCH)
	.rfMode = RF_MODE_MULTIPLE,
	.cpePatchFxn = &rf_patch_cpe_multi_protocol,
#else
	.rfMode = RF_MODE_PROPRIETARY_2_4,
	.cpePatchFxn = &rf_patch_cpe_prop,
#endif
};

/* --- Timing: seeded values, not yet measured --- */

/*
 * Correction from the captured RAT timestamp to t_sync (last address bit at the
 * antenna). Absorbs demodulator delay and capture offset. Zero is a placeholder
 * with no safe direction: an error shifts every RX window and eats one side of
 * its guard. Measuring it needs a wired two-board trigger. Until then the A/B
 * timing gate against the nRF backend only bounds the error.
 */
#define T_SYNC_CAL_US          0

/*
 * Ramp-up and turnaround, from the CC26x2 datasheet plus margin for command
 * dispatch. Deliberately generous: too large costs schedulable gap, too small
 * causes late windows.
 */
#define TX_RAMP_UP_US          150
#define RX_RAMP_UP_US          150

/*
 * The scheduler endTime for every transmit. Under coex a command with an
 * unspecified end is placed only at the queue tail, so transmits need a real
 * end time, as receives do (RX_END_SLOP_US). Covers the airtime past t_sync
 * plus margin. Unmeasured.
 */
#define TX_TAIL_SLOP_US        500

#if defined(CONFIG_RADIANT_BACKEND_CC26XX_COEX)
/*
 * caps.min_arm_lead_us under coex: MIN_ARM_LEAD_US plus the client phy-switch
 * lead (nPhySwitchingDuration 500) plus a 314 us margin. Honest only because
 * RX reserves follow_on_us in its endTime, so the phy-switch lead is paid
 * between clients, never inside our own span.
 */
#define CC26XX_COEX_PHY_SWITCH_LEAD_US 814
/* Bounds a sweep chunk so it cannot block the other client for long. A
 * starting point, not tuned against a fairness target. */
#define CC26XX_COEX_MAX_WINDOW_US      20000
#endif

/*
 * Extra receive time past t_close + BYTE_US. The end trigger stops sync
 * search, not just reception, so ending exactly at the window edge loses the
 * frame when it arrives late in the window. Measured loss: 79% with no slop,
 * 8-9% with 500 or 2000 us. 500 is the bounded cost. A missed frame at the
 * edge also loses the tracking anchor.
 *
 * Overridable with CONFIG_RADIANT_CC26XX_RX_END_SLOP_US (Kconfig default 500).
 */
#define RX_END_SLOP_US         CONFIG_RADIANT_CC26XX_RX_END_SLOP_US
#define RX_TO_TX_US            250
#define TX_TO_RX_US            250

/*
 * Minimum time between radiant_radio_now() and the earliest arm this backend
 * can honour. Larger than the nRF value: an arm posts through the driver to
 * another processor's doorbell.
 */
#define MIN_ARM_LEAD_US        600

/*
 * Lead below which an arm is refused. This is the hardware limit. MIN_ARM_LEAD_US
 * is the scheduler's budget, and the difference absorbs the ~55 us the
 * scheduler spends between deciding and arming. Between the floor and the lead
 * the window starts late (pastTrig) and loses its head.
 */
#define ARM_FLOOR_US           150

/* The CC26x2's Cortex-M4F runs at 48 MHz; the A0 cycle clock converts with it
 * (0xF7 pages 8-10, diagnostic builds only). */
#define CC_CORE_HZ             48000000u

/*
 * Lead for an operation armed from a radio callback into air the previous
 * operation reserved (follow_on_us), on the non-coex build. The RF core is
 * already awake, so the cold-arm wake cost is not owed. Roughly halfway
 * between the floor and MIN_ARM_LEAD_US.
 */
#define IN_GRANT_LEAD_US       400

/* --- Receive buffers: sized for the worst case the HAL permits --- */

/* Four entries: one merged window can deliver several frames back to back. */
#define RX_CRC_BYTES      2u
/* Address bytes left in the payload for the longest permitted address. Sizes
 * the rows, rx_cmd.maxPktLen and tx_buf. */
#define RX_SW_ADDR_MAX    (RADIANT_RADIO_ADDR_MAX - CC26XX_HW_ADDR_LEN)
BUILD_ASSERT(CC26XX_HW_ADDR_LEN >= 2u &&
	     CC26XX_HW_ADDR_LEN <= RADIANT_RADIO_ADDR_MAX,
	     "the sync-word matcher takes 2..ADDR_MAX address bytes");
#define RX_PAYLOAD_MAX    (RX_SW_ADDR_MAX + RADIANT_RADIO_BODY_MAX + RX_CRC_BYTES)
#define RX_APPEND_BYTES   (1u /* RSSI */ + 4u /* RAT */ + 1u /* status */)
#define RX_BUF_BYTES      (RX_PAYLOAD_MAX + RX_APPEND_BYTES)
#define RX_ENTRIES        4

/*
 * Each row must be word aligned for the RF core. __aligned(4) on the array only
 * aligns row 0, so the row size must be a multiple of four. A misaligned row
 * fails receive silently.
 */
#define RX_ROW_BYTES  ROUND_UP(sizeof(rfc_dataEntryGeneral_t) + RX_BUF_BYTES, 4)

static uint8_t rx_data[RX_ENTRIES][RX_ROW_BYTES] __aligned(4);
static dataQueue_t rx_queue;
static rfc_propRxOutput_t rx_output;

/*
 * Diagnostic counters for the 0xF7 page (ADR 0023). CC_DIAG() is a no-op
 * without CONFIG_RADIANT_DIAG_COUNTERS. rx_output is cleared on each arm, so
 * its counts are folded into rxo_* first.
 */
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
static struct {
	uint32_t rx_posted, rx_refused;
	uint32_t term_ok, term_empty, term_error;
	uint32_t status_err, status_last;
	uint32_t window_drops, crc_fail, alt_sync_hits;
	uint32_t rxo_nok, rxo_buf_full;
	uint32_t drain_max_us;
	/* page 8 (timing) */
	uint32_t drain_max_frames, frame_max_us, crc_max_us, rx_cb_max_us;
	uint32_t drain_over_250us;
	uint32_t min_lead_us;
	uint32_t win_frames;   /* OK frames in the window now open */
	bool     lead_seen;
	/* page 9 fields 5-7: the early arm check (cc_diag_early()) */
	uint32_t pass_to_arm_max_us, arm_slack_min_us, early_etime;
	/* page 10 fields 6-7: one receive entry's rx_t_sync(), and its
	 * rx_match() plus address/body copies, on the A0 clock */
	uint32_t t_sync_max_us, match_max_us;
} cc_diag = {
	.arm_slack_min_us = RADIANT_DIAG_NO_SAMPLE,
};
#define CC_DIAG(...) do { __VA_ARGS__; } while (0)

/*
 * Page 9 fields 5-7 at arm_common()'s ETIME check, the same check as the nRF
 * backend's: `now` is that check's clock read, `at` the instant checked
 * against now + `floor`. Field 5 only for an arm a scheduler pass is making;
 * a refused arm counts in field 7 and never in field 6.
 */
static void cc_diag_early(radiant_time_t now, radiant_time_t at, uint32_t floor)
{
	const radiant_time_t lim = now + (radiant_time_t)floor;

	const uint32_t stamp = radiant_diag_pass_now;

	if (stamp != 0u) {
		/* Low 32 bits of the radio clock; the span is far below a wrap. */
		const uint32_t us = (uint32_t)now - stamp;

		if (us > cc_diag.pass_to_arm_max_us) {
			cc_diag.pass_to_arm_max_us = us;
		}
	}
	if (at < lim) {
		cc_diag.early_etime++;
	} else {
		const uint64_t slack = at - lim;
		const uint32_t s32 = (slack > RADIANT_DIAG_NO_SAMPLE - 1u)
					     ? RADIANT_DIAG_NO_SAMPLE - 1u : (uint32_t)slack;

		if (s32 < cc_diag.arm_slack_min_us) {
			cc_diag.arm_slack_min_us = s32;
		}
	}
}

static void cc_diag_fold_rx_output(void)
{
	cc_diag.rxo_nok += (uint32_t)rx_output.nRxNok + rx_output.nRxIgnored;
	cc_diag.rxo_buf_full += rx_output.nRxBufFull;
}
#else
#define CC_DIAG(...) do { } while (0)
#endif

/*
 * Software's read cursor. rx_queue.pCurrEntry is where the RF core writes next,
 * so draining from it misses the finished entry behind it.
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
		 * The CRC is verified in software. The RF core cannot express
		 * ANT's CRC, which covers the on-air address bytes that the
		 * sync-word matcher consumes first. Software CRC gives crc_rx on
		 * every failure, which radiant_crc_repair.c needs. The cost is CPU
		 * per frame.
		 */
		.bUseCrc = 0,
		.bCrcIncSw = 0,
		.bCrcIncHdr = 0,
		/*
		 * A packet in progress when the end trigger fires is allowed to
		 * finish. Otherwise a frame cut by the window edge is counted as
		 * a corrupted frame.
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
/* A transmitted packet is the same size as a received row before its status
 * bytes, so the size guards in tx and rx use one figure. */
BUILD_ASSERT(sizeof(tx_buf) == RX_PAYLOAD_MAX,
	     "tx_buf and the receive rows must be sized from one figure");

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

/* --- State --- */

enum op_kind {
	OP_NONE = 0,
	OP_RX,
	OP_TX,
};

/* Retired commands whose callbacks can be owed at once; see st.retired. */
#define RETIRED_MAX 4u

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

	/* The armed request, copied: the HAL only guarantees the filters array
	 * outlives the operation. */
	struct radiant_pkt_format fmt;
	struct radiant_rx_filter  filters[8];
	uint8_t                   n_filters;
	uint8_t                   hw_addr_len;
	uint8_t                   sw_addr_len;
	radiant_time_t            t_open;
	radiant_time_t            t_close;
	uint32_t                  flags;
	radiant_time_t            tx_t_sync;

	/* Filter index per hardware sync word, or -1 if unused. */
	int8_t                    sync_group[2];

	/* 64-bit fold of the 32-bit RAT. See rat_fold(). */
	uint32_t                  rat_last;
	uint64_t                  rat_hi;
	int64_t                   rat_uptime_ref;

	/* The live operation's one terminal has been claimed (claim_locked()).
	 * Written only under irq_lock. */
	bool                      delivered_terminal;

	/* The live receive drained at least one finished entry: the receiver
	 * ran, so a preemption now is ABORTED, not DENIED. Reset per arm. */
	bool                      op_heard;

	/*
	 * Commands this backend ended itself (abort(), rx_drain()'s STOP_ON_FIRST)
	 * whose driver callbacks are still owed. They arrive after st may hold
	 * later operations, so they must never be attributed to one. Each entry
	 * leaves when its OWN terminal callback arrives, so several can be owed
	 * at once (a reply armed and aborted inside one callback). NO_HANDLE in
	 * an unused entry. Written only under irq_lock.
	 */
	RF_CmdHandle              retired[RETIRED_MAX];
	uint8_t                   retired_next;

	/* The transmit power last requested of apply_power() on this handle, and
	 * whether it is valid. Cleared by RF_open()/RF_close(). */
	struct radiant_tx_power   txp_last;
	bool                      txp_valid;
} st;

#define NO_HANDLE ((RF_CmdHandle)-1)

/* --- The operation slot's terminal and retired commands ---
 *
 * The RF callback runs in the driver's SWI (an NVIC interrupt at the lowest
 * level); abort() may run from a thread. Every test-and-set of st.kind /
 * st.delivered_terminal and every st.retired write is under irq_lock, and
 * the core's callback always runs after the lock is released, as on the
 * nRF backend.
 */

static void retired_clear(void)
{
	for (uint32_t i = 0u; i < RETIRED_MAX; i++) {
		st.retired[i] = NO_HANDLE;
	}
	st.retired_next = 0u;
}

/* Under irq_lock. Overwrites the oldest entry when full: four owed
 * callbacks at once is already beyond any path the core takes. */
static void retired_add(RF_CmdHandle ch)
{
	st.retired[st.retired_next] = ch;
	st.retired_next = (uint8_t)((st.retired_next + 1u) % RETIRED_MAX);
}

/* Under irq_lock. True if ch is a retired command; its entry goes when
 * `terminal` (its own last callback). */
static bool retired_take(RF_CmdHandle ch, bool terminal)
{
	for (uint32_t i = 0u; i < RETIRED_MAX; i++) {
		if (st.retired[i] == ch && ch != NO_HANDLE) {
			if (terminal) {
				st.retired[i] = NO_HANDLE;
			}
			return true;
		}
	}
	return false;
}

/* What a claimed terminal reports, captured under the claim's lock. */
struct claim {
	enum op_kind   kind;
	uint32_t       op;
	RF_CmdHandle   ch;
	radiant_time_t tx_t_sync;
};

/*
 * Under irq_lock: take the live operation's one terminal. False if there is
 * none to take (no operation, or its terminal already claimed). The slot is
 * free from here on: st.kind is OP_NONE and st.cmd_handle NO_HANDLE, so a
 * callback for this command can no longer reach the core. `retire` records
 * the handle in st.retired, for a command whose driver callback is still owed
 * (the caller is about to cancel it).
 */
static bool claim_locked(struct claim *c, bool retire)
{
	if (st.kind == OP_NONE || st.delivered_terminal) {
		return false;
	}
	c->kind = st.kind;
	c->op = st.op_id;
	c->ch = st.cmd_handle;
	c->tx_t_sync = st.tx_t_sync;
	st.delivered_terminal = true;
	st.kind = OP_NONE;
	st.cmd_handle = NO_HANDLE;
	if (retire && c->ch >= 0) {
		retired_add(c->ch);
	}
	return true;
}

/* A cancel that found no command (RF_StatInvalidParamsError) owes no
 * callback, so its entry would only linger. */
static void retired_cancel_result(RF_CmdHandle ch, RF_Stat rc)
{
	if (rc == RF_StatInvalidParamsError) {
		unsigned int key = irq_lock();

		(void)retired_take(ch, true);
		irq_unlock(key);
	}
}

static struct k_timer rat_keepalive;

/* --- Time: folding the 32-bit 4 MHz RAT into 64-bit microseconds --- */

/*
 * The RAT wraps every 2^32 ticks (about 1073 s). The fold adds 2^32 on each
 * backwards step, so it is correct only if sampled more often than the wrap.
 * The 60 s keepalive timer guarantees that. The RAT free-runs across
 * RF_close/RF_open, so no epoch is needed on enable.
 */

static uint64_t rat_fold(uint32_t now32)
{
	if (now32 < st.rat_last) {
#if defined(CONFIG_RADIANT_POWER_DIAG)
		/* Measurement only: counts a small back-step (an RTC-synthesised
		 * read while the RF core was off, rounded up) that the line below
		 * takes for a wrap. See src/radiant_rat_fold.h. */
		radiant_power_diag_rat_backward(st.rat_last, now32);
#endif
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

	/* With no RAT (before RF_open or after RF_close), use uptime. Callers in
	 * that state only decide whether arming is worthwhile. */
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

/* RAT tick for an absolute microsecond, truncated to the 32-bit trigger field. */
static uint32_t us_to_rat(radiant_time_t t)
{
	return (uint32_t)((uint64_t)t * RAT_TICKS_PER_US);
}

/* --- CRC: generic over struct radiant_crc_cfg, not fixed to one polynomial --- */

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

static uint32_t crc_generic(const struct radiant_crc_cfg *cfg,
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

/*
 * ANT's CRC (CRC-16/CCITT-FALSE: poly 0x1021, MSB first, no reflection) is
 * exactly what Zephyr's crc16_itu_t() computes: the seed is used as given and
 * no final XOR is applied, so xor_out is ours to apply. It does a few
 * shift/xor ops per byte where the loop above does eight conditional steps,
 * which was ~60 us per RX frame and per TX arm on the 48 MHz M4F.
 *
 * Recomputed by crc_self_check() at each init (deterministic, so a failed
 * check stays failed); kept outside st, which init memsets.
 */
static bool crc_fast_ok = true;

static bool crc_is_ccitt16(const struct radiant_crc_cfg *cfg)
{
	return cfg->width_bits == 16u && cfg->poly == 0x1021u &&
	       !cfg->reflect_in && !cfg->reflect_out;
}

static uint32_t crc_compute(const struct radiant_crc_cfg *cfg,
			    const uint8_t *p, size_t n)
{
	if (crc_fast_ok && crc_is_ccitt16(cfg)) {
		return (crc16_itu_t((uint16_t)cfg->init, p, n) ^ cfg->xor_out) &
		       0xFFFFu;
	}

	return crc_generic(cfg, p, n);
}

/*
 * One comparison at init, on radiant/spike/rx_raw's golden ANT frame (network
 * bytes A6 C5 then device 0x1234, type 0x78, trans 0x01, then a payload).
 * Both paths give 0x1B12. Should a library crc16_itu_t() ever disagree with
 * the generic loop (a hardware CRC handler overriding the __weak software one,
 * say), every frame would fail its CRC with nothing to say why - so say why,
 * and fall back.
 */
static void crc_self_check(void)
{
	static const uint8_t golden[15] = {
		0xA6, 0xC5, 0x34, 0x12, 0x78, 0x01, 0x0A,
		0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07,
	};
	static const struct radiant_crc_cfg ant = {
		.width_bits = 16u, .poly = 0x1021u, .init = 0xFFFFu,
	};
	const uint32_t fast =
		(uint32_t)crc16_itu_t((uint16_t)ant.init, golden, sizeof(golden));
	const uint32_t slow = crc_generic(&ant, golden, sizeof(golden));

	crc_fast_ok = (fast == slow);
	if (!crc_fast_ok) {
		LOG_ERR("crc16_itu_t gave 0x%04x, generic 0x%04x: using generic",
			fast, slow);
	}
}

/* --- Capabilities --- */

static const enum radiant_phy phys[] = { RADIANT_PHY_1M_GFSK };

/* Non-const under coex only: radiant_radio_enable() patches three fields once
 * the RF core is open. */
#if defined(CONFIG_RADIANT_BACKEND_CC26XX_COEX)
static struct radiant_radio_caps caps = {
#else
static const struct radiant_radio_caps caps = {
#endif
	.name = "cc26xx",

	/*
	 * Two sync words only, against the nRF's eight. The search sweep therefore
	 * takes 32 sets at eight filters but 128 at two, which by construction
	 * breaks ab_gates.toml's max_absolute_s = 5.0. Stated, not tuned around.
	 */
	.max_filters = 2,

	/*
	 * The sync words have no shared-BASE constraint, so two tracked channels
	 * can share a window. The value matches the nRF's, for a different reason.
	 */
	.max_addr_groups = 2,

	/*
	 * Four bits. The sync words are matched by a correlator, which cannot
	 * separate close templates. At -47 dBm, one bit apart delivered 0 frames,
	 * two bits 3, four bits 18, and eight bits 20. A single sync word scored
	 * 21-24. Consecutive devnum_lo values sit one bit apart, so a search sweep
	 * over them received nothing.
	 */
	.min_filter_hamming_bits = 4,

	/*
	 * ADR 0022: one wild_last filter covers every devnum_lo. The last address
	 * byte is compared in software and reported from the payload. Only with
	 * the 2-byte sync word.
	 */
	.filter_wildcard_dev = IS_ENABLED(CONFIG_RADIANT_SEARCH_SHORT_MATCH),

	/*
	 * Three, not the silicon's four. nSwBits is fixed in the setup command
	 * (see setup_cmd.formatConf). Fourth and fifth bytes are matched in
	 * software. Two under CONFIG_RADIANT_SEARCH_SHORT_MATCH.
	 */
	.addr_len_hw_max = CC26XX_HW_ADDR_LEN,

	.max_body_len = RADIANT_RADIO_BODY_MAX,

	.phys = phys,
	.n_phys = ARRAY_SIZE(phys),
	.phy_switch_us = 0,

	.ramp_up_us = TX_RAMP_UP_US,
	.rx_to_tx_us = RX_TO_TX_US,
	.tx_to_rx_us = TX_TO_RX_US,
	/* Patched upward under coex in radiant_radio_enable(). */
	.min_arm_lead_us = MIN_ARM_LEAD_US,
	/* See IN_GRANT_LEAD_US. Patched to 600 under coex. */
	.min_arm_lead_in_grant_us = IN_GRANT_LEAD_US,
	/*
	 * The receiver stays on BYTE_US + RX_END_SLOP_US past t_close, a surplus the
	 * scheduler must see. Undeclared, a background scan chunk would run into
	 * the next tracked window's arm lead. This is slightly conservative.
	 */
	.rx_close_hold_us = (uint16_t)(BYTE_US + RX_END_SLOP_US),

	.time_resolution_ns = 250,   /* RAT is 4 MHz */

	/* Unbounded outside coex. Patched to CC26XX_COEX_MAX_WINDOW_US under coex. */
	.max_window_us = 0,

	/* The timestamp is a hardware RAT capture. T_SYNC_CAL_US is a constant
	 * offset, not a loss of capture. */
	.has_sync_timestamp = true,

	.has_rssi = true,
	.has_ed_scan = false,        /* see radiant_radio_ed() */
	.has_rx_crc = true,          /* software CRC: the received value is kept */
	.crc_in_hw = false,          /* see rx_cmd.pktConf.bUseCrc */

	/*
	 * Served by apply_power() and radiant_cc26xx_txp[], not by setup_cmd.txPower
	 * (fixed at RF_open()). Without apply_power() the range would be accepted
	 * and ignored.
	 */
	.tx_power_min_dbm = -20,
	.tx_power_max_dbm = 5,
};

const struct radiant_radio_caps *radiant_radio_caps_get(void)
{
	return &caps;
}

/* --- Helpers --- */

/* The first hw_len address bytes, first on air in the most significant position. */
static uint32_t sync_word_of(const uint8_t *addr, uint8_t hw_len)
{
	uint32_t w = 0;

	for (uint8_t i = 0; i < hw_len; i++) {
		w = (w << 8) | addr[i];
	}

	return w;
}

/*
 * Rounds to the nearest table entry; an unreachable value keeps the current
 * power (as the HAL requires). use_raw writes rawValue and paType directly,
 * for bench work off the table.
 *
 * Acts, and logs, only when the request differs from the last one on this
 * handle. Every transmit carries its power, so without the check a lone
 * channel re-set the PA and logged at INF on every slot (9748 lines in 40 min
 * at gate GC1). Skipping a repeat is safe because the setting is not lost
 * while the handle is open: RF_setTxPower() writes it into setup_cmd, which
 * the driver replays at every RF core power-up (and on a client switch under
 * coex), so only RF_open()/RF_close() clear the cache. Channels with
 * different powers still get theirs: each change of request applies. An
 * off-table request is remembered too (the PA keeps the last power that
 * applied), so a repeat does not warn again; a failed RF_setTxPower() is not,
 * so the next transmit retries it (and warns once).
 *
 * A success is cached only when the RF core was active at the call.
 * RF_setTxPower() reports success once setup_cmd is updated, even when the
 * immediate CMD_SET_TX_POWER it then sends is dropped because the core is not
 * active (RF_executeDirectImmediateCmd() in RFCC26X2_multiMode.c). While the
 * core is off that is harmless (the next power-up replays setup_cmd), but
 * while it is powering up or switching PHY the setup already submitted
 * carries the old value, and a cached request would keep the old power on air
 * until the core next powered down. RF_GET_RADIO_STATE cannot tell "off" from
 * "powering up", so every inactive state leaves the cache invalid and the next
 * transmit applies the power again. The state read and the call share one
 * irq_lock(), so the driver cannot change state between them. Active with the
 * other coex client current is cached: the switch back replays setup_cmd.
 */
static bool txp_same(const struct radiant_tx_power *a,
		     const struct radiant_tx_power *b)
{
	if (a->use_raw != b->use_raw) {
		return false;
	}
	/* 22 bits: RF_TxPowerTable_Value::rawValue is a 22-bit field
	 * (RFCC26X2.h), so raw values equal under the mask program the same PA. */
	return a->use_raw ? ((a->raw & 0x3FFFFFu) == (b->raw & 0x3FFFFFu))
			  : (a->dbm == b->dbm);
}

static void apply_power(const struct radiant_tx_power *p)
{
	static bool warned_set_fail;
	RF_TxPowerTable_Value v;

	if (st.txp_valid && txp_same(p, &st.txp_last)) {
		return;
	}

	if (p->use_raw) {
		v.rawValue = p->raw & 0x3FFFFFu;
		v.paType = RF_TxPowerTable_DefaultPA;
	} else {
		v = RF_TxPowerTable_findValue(
			(RF_TxPowerTable_Entry *)radiant_cc26xx_txp, p->dbm);
		if (v.rawValue == RF_TxPowerTable_INVALID_VALUE) {
			LOG_WRN("tx power %d dBm off the table, keeping current",
				p->dbm);
			st.txp_last = *p;
			st.txp_valid = true;
			return;
		}
	}

	/* DBG, not INF: channels with different powers alternate requests, and
	 * each change applies, so this can still run once per transmit. */
	LOG_DBG("tx power req=%d dBm use_raw=%d -> rawValue=0x%x paType=%u",
		p->dbm, p->use_raw, (unsigned int)v.rawValue, v.paType);

	if (st.rf_handle == NULL) {
		return;
	}

	unsigned int key = irq_lock();
	RF_InfoVal info = { .bRadioState = false };
	bool active = RF_getInfo(st.rf_handle, RF_GET_RADIO_STATE, &info) ==
			      RF_StatSuccess &&
		      info.bRadioState;
	RF_Stat rc = RF_setTxPower(st.rf_handle, v);

	irq_unlock(key);

	if (rc == RF_StatSuccess) {
		/* Not cached while the core is inactive: see above. */
		st.txp_last = *p;
		st.txp_valid = active;
	} else if (!warned_set_fail) {
		warned_set_fail = true;
		LOG_WRN("RF_setTxPower failed (req=%d dBm use_raw=%d raw=0x%x); "
			"retrying on each transmit, not logged again",
			p->dbm, p->use_raw, (unsigned int)v.rawValue);
	}
}

#if defined(CONFIG_RADIANT_BACKEND_CC26XX_COEX)
/* Lets radiant_radio_cc26xx_arb.c tell this client from the neighbour's. */
void *radiant_cc26xx_client(void)
{
	return (void *)st.rf_handle;
}

/*
 * post_op(): RF_scheduleCmd() under coex, RF_postCmd() otherwise.
 *
 * allowDelay must be RF_AllowDelayNone. Otherwise the window is appended
 * behind a running background RX, starts late with pastTrig, and every frame
 * fails the window test. Refusing (EDENIED) is better than a silently late
 * window.
 */

/*
 * Count of RF_scheduleCmd() refusals, the same events that return
 * RADIANT_RADIO_EDENIED. Read it through the 0xF6 health reply, since a
 * debugger connect clears SRAM.
 */
volatile uint32_t radiant_cc26xx_submit_refusals;

/*
 * Logged from the system work queue, not from the arm call. The app uses
 * CONFIG_LOG_MODE_IMMEDIATE, where LOG_WRN blocks on the UART with interrupts
 * off inside the driver's SWI. Rate-limited to one line per second.
 */
static int64_t refusal_logged_ms = INT64_MIN / 2;

static void refusal_log_work_fn(struct k_work *work)
{
	ARG_UNUSED(work);
	LOG_WRN("RF_scheduleCmd refused %u arm(s) so far",
		(unsigned int)radiant_cc26xx_submit_refusals);
}

static K_WORK_DEFINE(refusal_log_work, refusal_log_work_fn);

static void note_submit_refusal(void)
{
	int64_t now = k_uptime_get();

	radiant_cc26xx_submit_refusals++;
	if (now - refusal_logged_ms >= 1000) {
		refusal_logged_ms = now;
		(void)k_work_submit(&refusal_log_work);
	}
}

static RF_CmdHandle post_op(RF_Op *op, uint32_t start_rat, uint32_t end_rat,
			     uint32_t activity, RF_Callback cb,
			     RF_EventMask events)
{
	RF_ScheduleCmdParams sch;
	RF_CmdHandle ch;

	RF_ScheduleCmdParams_init(&sch);
	sch.startTime = start_rat;
	sch.startType = RF_StartAbs;
	sch.allowDelay = RF_AllowDelayNone;
	sch.endTime = end_rat;
	sch.endType = RF_EndAbs;
	sch.activityInfo = activity;

	ch = RF_scheduleCmd(st.rf_handle, op, &sch, cb, events);
	if (ch < 0) {
		note_submit_refusal();
	}

	return ch;
}
#else
static RF_CmdHandle post_op(RF_Op *op, uint32_t start_rat, uint32_t end_rat,
			     uint32_t activity, RF_Callback cb,
			     RF_EventMask events)
{
	ARG_UNUSED(start_rat);
	ARG_UNUSED(end_rat);
	ARG_UNUSED(activity);

	return RF_postCmd(st.rf_handle, op, RF_PriorityNormal, cb, events);
}
#endif

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
	 * RADIANT_LEN_FROM_BODY is refused. The RF core counts a length field only
	 * from the first bit after the sync word, so it cannot place one after an
	 * unmatched address byte.
	 */
	if (fmt->len_mode != RADIANT_LEN_FIXED) {
		return false;
	}
	if (fmt->body_len > RADIANT_RADIO_BODY_MAX) {
		return false;
	}
	if (fmt->crc.width_bits != 0u &&
	    (fmt->crc.width_bits != 16u || !fmt->crc.cover_addr)) {
		/* Only "no CRC" and "16-bit CRC covering the address" are supported.
		 * Other CRCs have no tested path. */
		return false;
	}

	return true;
}

/* The core's terminal callback for an operation claim_locked() took. Called
 * with no lock held: the core may arm the next operation from inside it. */
static void deliver_terminal(const struct claim *c, enum radiant_radio_status status)
{
	if (c->kind == OP_RX) {
		struct radiant_rx_event evt = {
			.op = c->op,
			.status = status,
		};

		/* A window that heard a frame counts as OK, even if the reply armed
		 * from the frame callback aborted it. */
		CC_DIAG(if (cc_diag.win_frames != 0u) {
				cc_diag.term_ok++;
			} else if (status == RADIANT_RADIO_STATUS_TIMEOUT) {
				cc_diag.term_empty++;
			} else {
				cc_diag.term_error++;
			});

		if (st.cbs != NULL && st.cbs->rx != NULL) {
			st.cbs->rx(&evt, st.user);
		}
	} else if (c->kind == OP_TX) {
		struct radiant_tx_event evt = {
			.op = c->op,
			.status = status,
			.t_sync = c->tx_t_sync,
			.t_sync_exact = (status == RADIANT_RADIO_STATUS_OK),
		};

		if (st.cbs != NULL && st.cbs->tx != NULL) {
			st.cbs->tx(&evt, st.user);
		}
	}
}

/* The live operation ended by itself (its own terminal callback): claim it
 * and tell the core, unless an abort claimed it first. */
static void end_live_op(enum radiant_radio_status status)
{
	struct claim c;
	unsigned int key = irq_lock();
	const bool won = claim_locked(&c, false);

	irq_unlock(key);
	if (won) {
		deliver_terminal(&c, status);
	}
}

/* --- Receive --- */

/*
 * Diagnostic counters, volatile so they are not cached or dropped. A debugger
 * connect clears SRAM, so read them through the 0xF6 health reply.
 *
 *   radiant_cc26xx_sync_frames       entries rx_drain() examined (hardware
 *                                    matched a sync word)
 *   radiant_cc26xx_sw_addr_mismatch  of those, the ones whose software-compared
 *                                    address bytes matched no filter. Each
 *                                    costs one frame of airtime.
 *
 * Counted the same way with or without CONFIG_RADIANT_SEARCH_SHORT_MATCH. No
 * log line: rx_drain() runs in the driver callback.
 */
volatile uint32_t radiant_cc26xx_sync_frames;
volatile uint32_t radiant_cc26xx_sw_addr_mismatch;

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
 * Index of the filter this frame belongs to, or -1. The hardware matched the
 * first hw_addr_len bytes and reported the sync word. The rest are compared
 * here, using the same rule as the nRF backend (ADR 0019). The index is part of
 * the HAL contract: a wrong index attributes the frame to the wrong sensor.
 */
static int rx_match(uint8_t sync_id, const uint8_t *payload)
{
	if (sync_id >= ARRAY_SIZE(st.sync_group) || st.sync_group[sync_id] < 0) {
		return -1;
	}
	return radiant_addr_split_match(st.filters, st.n_filters,
					st.hw_addr_len, st.sw_addr_len,
					(uint8_t)st.sync_group[sync_id], payload);
}

/*
 * t_sync from the appended RAT capture. The capture is taken at the sync-word
 * event, so the software-matched address bytes' airtime is added.
 */
static radiant_time_t rx_t_sync(uint32_t rat)
{
	uint64_t folded;
	unsigned int key = irq_lock();

	/*
	 * Fold by signed distance from now, not by OR-ing into the high word. The OR
	 * puts a capture just before a wrap 18 minutes in the future.
	 */
	{
		uint32_t now32 = rat_raw();
		int32_t delta = (int32_t)(rat - now32);

		folded = rat_fold(now32) + (int64_t)delta;
	}
	irq_unlock(key);

	return (radiant_time_t)(folded / RAT_TICKS_PER_US) +
	       (radiant_time_t)radiant_addr_split_t_sync_us(st.sw_addr_len) +
	       (radiant_time_t)T_SYNC_CAL_US;
}

/*
 * 0xF7 page 8 and page 10 per-entry timing, on the A0 cycle clock
 * (radiant_diag.h). Until 2026-11-15 page 8 used the RAT; each
 * RF_getCurrentTime() read cost a few us and sat inside the spans it timed.
 * drain_max_us (page 4) stays on the RAT. Where the cycle clock does not run,
 * radiant_diag_cycles_to_us() answers RADIANT_DIAG_NOT_MEASURED, which wins
 * the max, and radiant_radio_diag_get() reports the field so even before a
 * sample. Off, the clock reads 0 and the level macro only evaluates its
 * argument, so the timing locals below compile away without
 * unused-variable warnings.
 */
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
static inline uint32_t cc_diag_t(void)
{
	return radiant_diag_cycles();
}

static inline void cc_diag_level(uint32_t *field, uint32_t cycles)
{
	const uint32_t us = radiant_diag_cycles_to_us(cycles);

	if (us > *field) {
		*field = us;
	}
}
#define CC_DIAG_LEVEL(field, ticks) cc_diag_level(&cc_diag.field, (ticks))

/*
 * crc_max_us (page 8 field 2) is the CRC computation alone. rx_drain() runs
 * with interrupts on, so an ISR landing inside the span used to be counted as
 * CRC time (70 us before the crc16_itu_t() fast path, 32 and 19 us after).
 * The diag build masks interrupts for that span only: a few microseconds over
 * at most RADIANT_RADIO_ADDR_MAX + RADIANT_RADIO_BODY_MAX bytes. Off, nothing
 * is masked. The transmit CRC is not timed and needs no mask:
 * radiant_radio_tx() is called only from a scheduler pass, which runs with
 * interrupts off, and its CRC is inside page 9 field 2 (backend arm calls).
 */
static inline unsigned int cc_diag_lock(void)
{
	return irq_lock();
}

static inline void cc_diag_unlock(unsigned int key)
{
	irq_unlock(key);
}
#else
static inline uint32_t cc_diag_t(void)
{
	return 0u;
}
#define CC_DIAG_LEVEL(field, ticks) ((void)(ticks))

static inline unsigned int cc_diag_lock(void)
{
	return 0u;
}

static inline void cc_diag_unlock(unsigned int key)
{
	ARG_UNUSED(key);
}
#endif

/* Returns the number of finished entries it examined. */
static uint32_t rx_drain_entries(void)
{
	uint32_t frames = 0u;
	/* The operation this drain serves. A frame callback may abort it and arm
	 * the next, whose rx_queue_init() resets rx_data and rx_read: from then
	 * on the queue is the new operation's, and this drain must not touch it. */
	const uint32_t drain_op = st.op_id;

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
		uint32_t t_entry;
		uint32_t cb_ticks = 0u;
		uint32_t m_cyc;   /* page 10 field 7: rx_match() and the copies */

		if (e == NULL || e->status != DATA_ENTRY_FINISHED) {
			break;
		}
		frames++;
		/* The receiver ran: a preemption from here on is ABORTED. */
		st.op_heard = true;
		t_entry = cc_diag_t();

		p = (const uint8_t *)(&e->data);
		payload_len = (uint8_t)(st.sw_addr_len + st.fmt.body_len +
					(st.fmt.crc.width_bits ? RX_CRC_BYTES : 0u));
		rssi_raw = p[payload_len];
		memcpy(&rat, &p[payload_len + 1u], sizeof(rat));
		memcpy(&status, &p[payload_len + 5u], sizeof(status));

		body = radiant_addr_split_body(p, st.sw_addr_len);

		memset(&evt, 0, sizeof(evt));
		evt.op = st.op_id;
		{
			const uint32_t t_ts = cc_diag_t();

			evt.t_sync = rx_t_sync(rat);
			CC_DIAG_LEVEL(t_sync_max_us, cc_diag_t() - t_ts);
		}
		evt.t_sync_exact = true;
		evt.body = body;
		evt.body_len = st.fmt.body_len;
		evt.has_rssi = true;
		evt.rssi_dbm = (int8_t)rssi_raw;

		{
			const uint32_t t_m = cc_diag_t();

			idx = rx_match(status.status.syncWordId, p);
			m_cyc = cc_diag_t() - t_m;
		}
		radiant_cc26xx_sync_frames++;
		/* syncWord1 is 0 for one group, so this should stay 0. */
		CC_DIAG(if (status.status.syncWordId == 1u &&
			    st.sync_group[1] == st.sync_group[0]) {
				cc_diag.alt_sync_hits++;
			});

		if (idx < 0) {
			/* Sync word matched, but no filter's full address did. This is
			 * the normal result of a short hardware match, not an event. */
			radiant_cc26xx_sw_addr_mismatch++;
			goto next;
		}

		evt.filter_index = (uint8_t)idx;
		/*
		 * Wildcard search (ADR 0022): the unmatched final address byte, as
		 * radiant_addr_split_wild_value() picks it on the nRF.
		 */
		if (IS_ENABLED(CONFIG_RADIANT_SEARCH_SHORT_MATCH) &&
		    st.filters[idx].wild_last) {
			evt.wild_value = radiant_addr_split_wild_value(
				p, st.sw_addr_len);
		}

		/*
		 * Outside [t_open, t_close]. The receiver is armed early and late, so
		 * such frames are expected. The HAL defines the window in t_sync
		 * terms, so they are dropped.
		 */
		if (evt.t_sync < st.t_open || evt.t_sync > st.t_close) {
			CC_DIAG(cc_diag.window_drops++);
			goto next;
		}

		if (st.fmt.crc.width_bits != 0u) {
			uint8_t covered[RADIANT_RADIO_ADDR_MAX +
					RADIANT_RADIO_BODY_MAX];
			size_t n = 0;
			uint32_t want;
			uint32_t got;
			const uint32_t t_cp = cc_diag_t();

			/* Restore the leading address bytes the matcher consumed. */
			memcpy(covered, st.filters[idx].addr, st.fmt.addr_len);
			n = st.fmt.addr_len;
			/* A wild filter's last byte is not known to the filter, so use
			 * the byte received on air. Otherwise every wildcard frame
			 * fails its CRC. */
			if (IS_ENABLED(CONFIG_RADIANT_SEARCH_SHORT_MATCH) &&
			    st.filters[idx].wild_last) {
				covered[n - 1u] = evt.wild_value;
			}
			memcpy(&covered[n], body, st.fmt.body_len);
			n += st.fmt.body_len;
			m_cyc += cc_diag_t() - t_cp;

			{
				/* Masked in the diag build only: see cc_diag_lock(). */
				const unsigned int key = cc_diag_lock();
				const uint32_t t_crc = cc_diag_t();
				uint32_t crc_cyc;

				got = crc_compute(&st.fmt.crc, covered, n);
				crc_cyc = cc_diag_t() - t_crc;
				cc_diag_unlock(key);
				CC_DIAG_LEVEL(crc_max_us, crc_cyc);
			}
			want = ((uint32_t)body[st.fmt.body_len] << 8) |
			       body[st.fmt.body_len + 1u];

			evt.has_crc_rx = true;
			evt.crc_rx = want;

			if (got != want) {
				CC_DIAG(cc_diag.crc_fail++);
				if ((st.flags & RADIANT_RX_REPORT_CRC_FAIL) == 0u) {
					goto next;
				}
				/*
				 * Keep evt.body (the received, damaged bytes) and
				 * crc_rx: CONFIG_RADIANT_CRC_REPAIR needs both. The
				 * entry is released at next:, after the callback, so
				 * the body lives as long as the HAL promises.
				 */
				evt.status = RADIANT_RADIO_STATUS_CRC_FAIL;
			} else {
				evt.status = RADIANT_RADIO_STATUS_OK;
				evt.has_crc_rx = false;
				evt.crc_rx = 0;
			}
		} else {
			evt.status = RADIANT_RADIO_STATUS_OK;
		}

		if (evt.status == RADIANT_RADIO_STATUS_OK &&
		    (st.flags & RADIANT_RX_STOP_ON_FIRST) != 0u) {
			uint8_t copy[RADIANT_RADIO_BODY_MAX];

			/*
			 * This frame ends the window. Retire the operation before the
			 * callback, since the core may arm the next one from inside it.
			 * Release the entry and cancel first, since that arm may reset
			 * the queue and replace st.cmd_handle.
			 */
			{
				const uint32_t t_cp = cc_diag_t();

				memcpy(copy, body, st.fmt.body_len);
				m_cyc += cc_diag_t() - t_cp;
			}
			CC_DIAG_LEVEL(match_max_us, m_cyc);
			evt.body = copy;
			e->status = DATA_ENTRY_PENDING;
			rx_read = (uint8_t)((rx_read + 1u) % RX_ENTRIES);
			{
				struct claim c;
				unsigned int key = irq_lock();
				const bool won = claim_locked(&c, true);

				irq_unlock(key);
				if (!won) {
					/* An abort took the terminal first: the frame
					 * belongs to an operation the core has retired. */
					return frames;
				}
				retired_cancel_result(c.ch,
					RF_cancelCmd(st.rf_handle, c.ch, 0));
			}
			/* This frame is the window's terminal too. */
			CC_DIAG(cc_diag.win_frames++; cc_diag.term_ok++);
			CC_DIAG_LEVEL(frame_max_us, cc_diag_t() - t_entry);
			if (st.cbs != NULL && st.cbs->rx != NULL) {
				const uint32_t t_cb = cc_diag_t();

				st.cbs->rx(&evt, st.user);
				CC_DIAG_LEVEL(rx_cb_max_us, cc_diag_t() - t_cb);
			}
			return frames;
		}

		CC_DIAG(if (evt.status == RADIANT_RADIO_STATUS_OK) {
				cc_diag.win_frames++;
			});
		if (st.cbs != NULL && st.cbs->rx != NULL) {
			const uint32_t t_cb = cc_diag_t();

			st.cbs->rx(&evt, st.user);
			cb_ticks = cc_diag_t() - t_cb;
			CC_DIAG_LEVEL(rx_cb_max_us, cb_ticks);
		}
		/* The callback aborted this window (and maybe armed the next, which
		 * now owns rx_data and rx_read): stop without touching the queue. */
		if (st.kind != OP_RX || st.op_id != drain_op) {
			return frames;
		}

next:
		/* This entry's own work, the core's callback excluded. */
		CC_DIAG_LEVEL(frame_max_us, cc_diag_t() - t_entry - cb_ticks);
		CC_DIAG_LEVEL(match_max_us, m_cyc);
		e->status = DATA_ENTRY_PENDING;
		rx_read = (uint8_t)((rx_read + 1u) % RX_ENTRIES);
	}

	return frames;
}

/* rx_drain_entries() plus timing for the 0xF7 page. The drain is part of the
 * turnaround a chained transmit must fit inside. */
static void rx_drain(void)
{
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
	uint32_t t0 = rat_raw();
	uint32_t us;
	uint32_t frames;

	frames = rx_drain_entries();
	us = (rat_raw() - t0) / RAT_TICKS_PER_US;
	if (us > cc_diag.drain_max_us) {
		cc_diag.drain_max_us = us;
		cc_diag.drain_max_frames = frames;
	}
	if (us > RADIANT_DIAG_DRAIN_SLOW_US) {
		cc_diag.drain_over_250us++;
	}
#else
	(void)rx_drain_entries();
#endif
}

#define TERMINAL_EVENTS (RF_EventLastCmdDone | RF_EventCmdAborted | \
			 RF_EventCmdStopped | RF_EventCmdCancelled |   \
			 RF_EventCmdPreempted)

/*
 * Whether a driver callback for ch belongs to the live operation. A retired
 * command's callbacks never do (its entry goes with its own terminal). The
 * live operation's handle always does. Anything else is live only while an
 * arm is between RF_postCmd/RF_scheduleCmd and storing st.cmd_handle (which
 * only an arm from a thread outside irq_lock can be caught in): st.cmd_handle
 * is then NO_HANDLE, or the arm's refusal code.
 */
static bool callback_is_live(RF_CmdHandle ch, bool terminal)
{
	unsigned int key = irq_lock();
	bool live;

	if (st.kind != OP_NONE && ch == st.cmd_handle) {
		/* A live handle equal to a lingering entry (a wrap): not retired. */
		(void)retired_take(ch, true);
		live = true;
	} else if (retired_take(ch, terminal)) {
		live = false;
	} else {
		live = (st.kind != OP_NONE) && (st.cmd_handle < 0);
	}
	irq_unlock(key);
	return live;
}

/* Command errors: generic ERROR_* (0x08xx) and PROP_ERROR_* (0x38xx). */
static bool status_error(uint16_t s)
{
	return (s & 0xFF00u) == 0x0800u || s >= 0x3800u;
}

/*
 * A preempted operation had the air (ABORTED) or never did (DENIED). A
 * command preempted in the driver's pending queue ends Cancelled|Preempted
 * and never reached the RF core. One the RF core held ends Aborted (or
 * Stopped)|Preempted, but it is dispatched ahead of its start trigger, so it
 * listened only if its window opened: a frame was drained, or t_open passed.
 * A transmit started once its preamble's first bit was due.
 */
static bool op_had_air(enum op_kind kind, RF_EventMask events)
{
	const radiant_time_t now = now_us();

	if ((events & (RF_EventCmdAborted | RF_EventCmdStopped)) == 0) {
		return false;
	}
	if (kind == OP_RX) {
		return st.op_heard || now >= st.t_open;
	}
	return now + (radiant_time_t)((PREAMBLE_BYTES + st.fmt.addr_len) *
				      BYTE_US) >= st.tx_t_sync;
}

static void rf_callback(RF_Handle h, RF_CmdHandle ch, RF_EventMask events)
{
	const bool terminal = (events & TERMINAL_EVENTS) != 0;
	enum op_kind kind;
	uint16_t cmd_status;
	enum radiant_radio_status status;

	ARG_UNUSED(h);

	if (!callback_is_live(ch, terminal)) {
		return;
	}

	if ((events & RF_EventRxEntryDone) != 0) {
		rx_drain();
		/* A frame may have ended the window (STOP_ON_FIRST), or its
		 * callback aborted it. */
		if (!callback_is_live(ch, terminal)) {
			return;
		}
	}

	if (!terminal) {
		return;
	}

	kind = st.kind;
	if (kind == OP_RX) {
		/* Drain anything finished that the entry-done event did not cover. */
		rx_drain();
		if (!callback_is_live(ch, terminal)) {
			return;
		}
	}

	/*
	 * The op's own status word: every way this backend can fail to hear or
	 * send a frame looks alike to the driver, and only this field tells them
	 * apart. Counted once per command end.
	 */
	cmd_status = (kind == OP_RX) ? rx_cmd.status : tx_cmd.status;
	if (status_error(cmd_status)) {
		CC_DIAG(cc_diag.status_err++; cc_diag.status_last = cmd_status);
		if (kind == OP_RX) {
			LOG_WRN("rx cmd ended status=0x%04x (start=%u end=%u sw0=%08x)",
				cmd_status, rx_cmd.startTime, rx_cmd.endTime,
				rx_cmd.syncWord0);
		} else {
			LOG_WRN("tx cmd ended status=0x%04x (start=%u)",
				cmd_status, tx_cmd.startTime);
		}
	}

	/*
	 * Preempted is tested before Cancelled/Aborted, because RF_abortCmd sets
	 * both flags when the execute hook aborts a command. DENIED only for an
	 * operation that never had the air (the core credits it as no listening);
	 * one cut short mid-window is ABORTED, like the MPSL gate's lost grant.
	 * LastCmdDone (never beside an abort bit) is TIMEOUT for a receive and OK
	 * for a transmit.
	 */
	if ((events & RF_EventCmdPreempted) != 0) {
		status = op_had_air(kind, events) ? RADIANT_RADIO_STATUS_ABORTED
						  : RADIANT_RADIO_STATUS_DENIED;
	} else if ((events & RF_EventLastCmdDone) == 0) {
		status = RADIANT_RADIO_STATUS_ABORTED;
	} else if (kind == OP_RX) {
		status = RADIANT_RADIO_STATUS_TIMEOUT;
	} else {
		status = RADIANT_RADIO_STATUS_OK;
	}

	end_live_op(status);
}

/* --- Lifecycle --- */

int radiant_radio_init(const struct radiant_radio_cbs *cbs, void *user)
{
	if (cbs == NULL) {
		return RADIANT_RADIO_EINVAL;
	}

	memset(&st, 0, sizeof(st));
	st.cbs = cbs;
	st.user = user;
	st.next_op_id = 1;
	retired_clear();
	st.cmd_handle = NO_HANDLE;
	st.inited = true;

	/* 0xF7 pages 8-10: the A0 cycle clock, at this core's fixed clock. */
	CC_DIAG(radiant_diag_cycles_init(CC_CORE_HZ));

	crc_self_check();
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
	 * UINT32_MAX is RF_Params_init()'s own default (SemaphoreP_WAIT_FOREVER),
	 * so this changes nothing: the inactivity timer stays off. A finite value
	 * would add a ~1.5 ms wake, more than min_arm_lead_us. Whether the driver
	 * still powers the core down between distant slots is unverified.
	 */
	params.nInactivityTimeout = UINT32_MAX;
#if defined(CONFIG_RADIANT_POWER_DIAG)
	/* Counts CMD_FS synth errors; does nothing else, like the driver's
	 * default error callback it replaces. */
	params.pErrCb = radiant_ti_rf_err_cb;
#endif

	st.rf_handle = RF_open(&st.rf_object, &rf_mode,
			       (RF_RadioSetup *)&setup_cmd, &params);
	if (st.rf_handle == NULL) {
		LOG_ERR("RF_open failed");
		return RADIANT_RADIO_EIO;
	}
	/* A new handle: the first transmit applies its power (apply_power()). */
	st.txp_valid = false;

	/*
	 * Set the synthesiser once here, not per arm, since every ANT+ operation
	 * uses the same frequency. Test the event bit, not the whole mask: a
	 * successful RF_runCmd can return other bits as well.
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

	/* A fresh handle may number its commands from where the last began. */
	retired_clear();
	st.cmd_handle = NO_HANDLE;

#if defined(CONFIG_RADIANT_BACKEND_CC26XX_COEX)
	/* Patch the three caps that a shared RF core makes honest. */
	caps.min_arm_lead_us = MIN_ARM_LEAD_US + CC26XX_COEX_PHY_SWITCH_LEAD_US;
	/*
	 * 600, not 0: radiant_burst.c reads 0 as "same as min_arm_lead_us", and
	 * ERG mode needs lead + 250 <= 1560. With 0, min_arm_lead_us (1414) fails
	 * that test, and ERG is silently refused. 600 is valid because RX reserves
	 * follow_on_us, so the phy-switch lead is paid only between clients.
	 */
	caps.min_arm_lead_in_grant_us = 600;
	caps.max_window_us = CC26XX_COEX_MAX_WINDOW_US;
#endif

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

	/* Sample the RAT once more so the fold survives the close. */
	(void)now_us();

	RF_close(st.rf_handle);
	st.rf_handle = NULL;
	st.txp_valid = false;
	st.enabled = false;

	return RADIANT_RADIO_OK_RC;
}

radiant_time_t radiant_radio_now(void)
{
	return now_us();
}

/* --- Arm --- */

static int arm_common(const struct radiant_pkt_format *fmt, radiant_time_t at)
{
	if (!st.enabled) {
		return RADIANT_RADIO_ESTATE;
	}
	{
		/* Read against the RF SWI's terminal claim (claim_locked()). Under the
		 * same lock, the per-op state a terminal reads: no frame heard yet. */
		unsigned int key = irq_lock();
		const bool busy = (st.kind != OP_NONE);

		if (!busy) {
			st.op_heard = false;
		}
		irq_unlock(key);
		if (busy) {
			return RADIANT_RADIO_EBUSY;
		}
	}
	if (!fmt_supported(fmt)) {
		return RADIANT_RADIO_ENOTSUP;
	}
	if (at == RADIANT_TIME_NEVER) {
		return RADIANT_RADIO_EINVAL;
	}
	CC_DIAG({
		radiant_time_t now = now_us();
		uint32_t lead = (at > now) ? (uint32_t)MIN(at - now, UINT32_MAX - 1u)
					   : 0u;

		if (!cc_diag.lead_seen || lead < cc_diag.min_lead_us) {
			cc_diag.min_lead_us = lead;
			cc_diag.lead_seen = true;
		}
	});
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
	{
		/* The same check, one clock read shared with page 9. */
		const radiant_time_t now = now_us();

		cc_diag_early(now, at, ARM_FLOOR_US);
		if (at < now + ARM_FLOOR_US) {
			return RADIANT_RADIO_ETIME;
		}
	}
#else
	if (at < now_us() + ARM_FLOOR_US) {
		return RADIANT_RADIO_ETIME;
	}
#endif

	return RADIANT_RADIO_OK_RC;
}

static int rx_arm(const struct radiant_rx_req *req, uint32_t *op)
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
		/* A wild_last filter needs its last byte left to software, so the
		 * address must be longer than the hardware match. */
		if (req->filters[i].wild_last &&
		    (!IS_ENABLED(CONFIG_RADIANT_SEARCH_SHORT_MATCH) ||
		     req->filters[i].addr_len <= caps.addr_len_hw_max)) {
			return RADIANT_RADIO_ENOTSUP;
		}
	}
	/*
	 * Two sync words 1-3 bits apart leave the correlator deaf to both, with no
	 * error anywhere (caps.min_filter_hamming_bits). The scheduler's rule 6
	 * never asks for that; refuse it loudly if anything does, before st is
	 * touched. Pairs sharing a sync word are distance 0 and pass.
	 */
	for (uint8_t i = 1; i < req->n_filters; i++) {
		for (uint8_t j = 0; j < i; j++) {
			if (!radiant_rx_filters_separable(&caps, &req->filters[i],
							  &req->filters[j])) {
				return RADIANT_RADIO_ENOTSUP;
			}
		}
	}
	/* A longer frame would overrun its row, so refuse it. */
	if ((uint32_t)(req->fmt->addr_len -
		       MIN(req->fmt->addr_len, caps.addr_len_hw_max)) +
	    req->fmt->body_len +
	    (req->fmt->crc.width_bits ? RX_CRC_BYTES : 0u) > RX_PAYLOAD_MAX) {
		return RADIANT_RADIO_ENOTSUP;
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
	 * Assign the two hardware sync words. Filters sharing a prefix share one, so
	 * two channels that differ only in device type fit in one window.
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
			/* More than max_addr_groups prefixes: refuse, not a partial window. */
			return RADIANT_RADIO_ENOTSUP;
		}
	}

	rx_cmd.syncWord0 = sync_word_of(st.filters[st.sync_group[0]].addr,
					st.hw_addr_len);
	if (st.sync_group[1] < 0) {
		/*
		 * One group: syncWord1 = 0 turns the second correlator off (TI
		 * rf_prop_cmd.h). Never copy syncWord0 here. The two templates would be
		 * zero bits apart, the worst case for the correlator, and every tracked
		 * window then failed. sync_group[1] still names the same filter, so a
		 * stray syncWordId of 1 is attributed to it.
		 */
		st.sync_group[1] = st.sync_group[0];
		rx_cmd.syncWord1 = 0u;
	} else {
		rx_cmd.syncWord1 = sync_word_of(st.filters[st.sync_group[1]].addr,
						st.hw_addr_len);
	}
	rx_cmd.maxPktLen = (uint16_t)(st.sw_addr_len + st.fmt.body_len +
				      (st.fmt.crc.width_bits ? RX_CRC_BYTES : 0u));

	/*
	 * Listen before the preamble of a frame whose t_sync is t_open. The window
	 * is defined by t_sync, not by when RX is enabled, so ramp-up and preamble
	 * length do not bias it.
	 */
	start_rat = us_to_rat(req->t_open -
			      (radiant_time_t)((PREAMBLE_BYTES + st.fmt.addr_len) *
					       BYTE_US) -
			      (radiant_time_t)RX_RAMP_UP_US);
	/*
	 * Still listening for a frame whose t_sync is t_close. It only needs to
	 * cover the address, since endType = 1 lets the frame finish.
	 */
	end_rat = us_to_rat(req->t_close + (radiant_time_t)BYTE_US +
			    (radiant_time_t)RX_END_SLOP_US);

	/*
	 * pastTrig = 1 on both triggers. With 0, a time already passed is an error
	 * that looks like an empty window, so the scheduler would re-arm forever.
	 * A few microseconds late is normal, so start now and lose that head.
	 */
	rx_cmd.startTrigger.triggerType = TRIG_ABSTIME;
	rx_cmd.startTrigger.pastTrig = 1;
	rx_cmd.startTime = start_rat;
	rx_cmd.endTrigger.triggerType = TRIG_ABSTIME;
	rx_cmd.endTrigger.pastTrig = 1;
	rx_cmd.endTime = end_rat;

	rx_queue_init();
	CC_DIAG(cc_diag_fold_rx_output(); cc_diag.win_frames = 0u);
	memset(&rx_output, 0, sizeof(rx_output));

	st.kind = OP_RX;
	st.op_id = st.next_op_id++;
	st.delivered_terminal = false;

	st.cmd_handle = post_op(
		(RF_Op *)&rx_cmd, start_rat,
		/*
		 * The scheduler's endTime, which adds follow_on_us to the hardware end
		 * trigger. A tracked frame may become a transmit up to follow_on_us
		 * later, so the reservation applies to every window (zero for search
		 * and ED sweeps).
		 */
		end_rat + us_to_rat(req->follow_on_us),
		/*
		 * Priority from window length alone, as radiant_radio_nrf_gate_mpsl.c
		 * does. Tracked windows are under a millisecond. Only scan chunks and
		 * ED sweeps run tens of milliseconds.
		 */
		((req->t_close - req->t_open) >= 10000u)
			? RADIANT_CC26XX_PRIO_NORMAL
			: RADIANT_CC26XX_PRIO_HIGH,
		rf_callback,
		RF_EventRxEntryDone | RF_EventLastCmdDone |
			RF_EventCmdAborted | RF_EventCmdStopped |
			RF_EventCmdCancelled | RF_EventCmdPreempted);
	if (st.cmd_handle < 0) {
		st.kind = OP_NONE;
		/* A refusal is routine under arbitration, so report EDENIED. */
		return RADIANT_RADIO_EDENIED;
	}

	*op = st.op_id;

	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_rx(const struct radiant_rx_req *req, uint32_t *op)
{
	int rc = rx_arm(req, op);

	CC_DIAG(if (rc == RADIANT_RADIO_OK_RC) {
			cc_diag.rx_posted++;
		} else {
			cc_diag.rx_refused++;
		});
	return rc;
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
	/* tx_buf holds the software address bytes, the body and the CRC. */
	if ((uint32_t)(req->fmt->addr_len -
		       MIN(req->fmt->addr_len, caps.addr_len_hw_max)) +
	    req->body_len +
	    (req->fmt->crc.width_bits ? RX_CRC_BYTES : 0u) > sizeof(tx_buf)) {
		return RADIANT_RADIO_ENOTSUP;
	}

	hw_len = MIN(req->fmt->addr_len, caps.addr_len_hw_max);
	sw_len = (uint8_t)(req->fmt->addr_len - hw_len);

	st.fmt = *req->fmt;
	st.hw_addr_len = hw_len;
	st.sw_addr_len = sw_len;
	st.tx_t_sync = req->t_sync_at;

	apply_power(&req->power);

	/*
	 * The address/body split is the receiver's view, not a property of the air.
	 * The fifth address byte goes at the head of the payload.
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
	/* As for rx_cmd: a start time a few microseconds in the past starts the
	 * transmit rather than refusing it. */
	tx_cmd.startTrigger.pastTrig = 1;
	/*
	 * Start time = t_sync minus preamble, address and ramp-up airtime. This is
	 * what makes the master's period exact.
	 */
	tx_cmd.startTime = us_to_rat(req->t_sync_at -
				     (radiant_time_t)((PREAMBLE_BYTES +
						       req->fmt->addr_len) *
						      BYTE_US) -
				     (radiant_time_t)TX_RAMP_UP_US +
				     (radiant_time_t)CONFIG_RADIANT_CC26XX_TX_TIME_CAL_US);

	st.kind = OP_TX;
	st.op_id = st.next_op_id++;
	st.delivered_terminal = false;

	st.cmd_handle = post_op(
		(RF_Op *)&tx_cmd, tx_cmd.startTime,
		/*
		 * TX_ADV has no end trigger, so this end time is scheduler bookkeeping
		 * only. It lets RF_verifyGap place a neighbour after this transmit.
		 */
		us_to_rat(req->t_sync_at + (radiant_time_t)(n * BYTE_US) +
			  (radiant_time_t)TX_TAIL_SLOP_US +
			  (radiant_time_t)CONFIG_RADIANT_CC26XX_TX_TIME_CAL_US),
		/* Transmits are always high priority, never the elastic sweep class. */
		RADIANT_CC26XX_PRIO_HIGH, rf_callback,
		RF_EventLastCmdDone | RF_EventCmdAborted |
			RF_EventCmdStopped | RF_EventCmdCancelled |
			RF_EventCmdPreempted);
	if (st.cmd_handle < 0) {
		st.kind = OP_NONE;
		return RADIANT_RADIO_EDENIED;
	}

	*op = st.op_id;

	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_ed(const struct radiant_ed_req *req, uint32_t *op)
{
	ARG_UNUSED(req);
	ARG_UNUSED(op);

	/*
	 * Expected, since caps.has_ed_scan is false. The part could measure energy,
	 * but a scale that does not match rssi_dbm and noise_dbm is worse than none.
	 */
	return RADIANT_RADIO_ENOTSUP;
}

int radiant_radio_abort(void)
{
	struct claim c;
	unsigned int key;
	bool won;

	/*
	 * Claim the terminal first, under the lock, so the RF SWI cannot deliver
	 * one in between (abort() may be called from a thread): from here the
	 * operation's own callbacks are retired and reach no one, including the
	 * flush's asynchronous cancel, which can arrive after the next operation
	 * is armed. Then flush, then deliver ABORTED synchronously, as the HAL
	 * requires before the slot is seen as free. The flush comes before the
	 * core's callback, which may arm the next operation (the arb hook passes
	 * an own command whose termination is stored as absent; see arb.c).
	 */
	key = irq_lock();
	won = st.enabled && claim_locked(&c, true);
	irq_unlock(key);
	if (!won) {
		return RADIANT_RADIO_OK_RC;
	}

	retired_cancel_result(c.ch, RF_flushCmd(st.rf_handle, c.ch, 0));
	deliver_terminal(&c, RADIANT_RADIO_STATUS_ABORTED);

	return RADIANT_RADIO_OK_RC;
}

#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
uint8_t radiant_radio_diag_get(struct radiant_radio_diag *out)
{
	unsigned int key = irq_lock();

	out->v[RADIANT_RADIO_DIAG_RX_POSTED] = cc_diag.rx_posted;
	out->v[RADIANT_RADIO_DIAG_RX_REFUSED] = cc_diag.rx_refused;
	out->v[RADIANT_RADIO_DIAG_TERM_OK] = cc_diag.term_ok;
	out->v[RADIANT_RADIO_DIAG_TERM_EMPTY] = cc_diag.term_empty;
	out->v[RADIANT_RADIO_DIAG_TERM_ERROR] = cc_diag.term_error;
	out->v[RADIANT_RADIO_DIAG_STATUS_ERR] = cc_diag.status_err;
	out->v[RADIANT_RADIO_DIAG_STATUS_LAST] = cc_diag.status_last;
	out->v[RADIANT_RADIO_DIAG_SYNC_ENTRIES] = radiant_cc26xx_sync_frames;
	out->v[RADIANT_RADIO_DIAG_SW_ADDR_MISMATCH] = radiant_cc26xx_sw_addr_mismatch;
	out->v[RADIANT_RADIO_DIAG_WINDOW_DROPS] = cc_diag.window_drops;
	out->v[RADIANT_RADIO_DIAG_CRC_FAIL] = cc_diag.crc_fail;
	out->v[RADIANT_RADIO_DIAG_ALT_SYNC_HITS] = cc_diag.alt_sync_hits;
	/* The window still running is not folded yet. */
	out->v[RADIANT_RADIO_DIAG_RX_NOK] = cc_diag.rxo_nok +
		(uint32_t)rx_output.nRxNok + rx_output.nRxIgnored;
	out->v[RADIANT_RADIO_DIAG_RX_BUF_FULL] = cc_diag.rxo_buf_full +
		rx_output.nRxBufFull;
	out->v[RADIANT_RADIO_DIAG_DRAIN_MAX_US] = cc_diag.drain_max_us;
	out->v[RADIANT_RADIO_DIAG_MIN_LEAD_US] = cc_diag.lead_seen
		? cc_diag.min_lead_us : RADIANT_DIAG_NOT_MEASURED;
	out->v[RADIANT_RADIO_DIAG_DRAIN_MAX_FRAMES] = cc_diag.drain_max_frames;
	out->v[RADIANT_RADIO_DIAG_DRAIN_OVER_250US] = cc_diag.drain_over_250us;
	out->v[RADIANT_RADIO_DIAG_PASS_TO_ARM_MAX_US] = cc_diag.pass_to_arm_max_us;
	out->v[RADIANT_RADIO_DIAG_ARM_SLACK_MIN_US] = cc_diag.arm_slack_min_us;
	out->v[RADIANT_RADIO_DIAG_EARLY_ETIME] = cc_diag.early_etime;
	/* The A0-timed fields, only while that clock runs (a zero-cycle span
	 * converts to "not measured" exactly when it does not). Page 10 fields
	 * 4-5 are the nRF END interrupt's and stay "not measured" here. */
	if (radiant_diag_cycles_to_us(0u) != RADIANT_DIAG_NOT_MEASURED) {
		out->v[RADIANT_RADIO_DIAG_FRAME_MAX_US] = cc_diag.frame_max_us;
		out->v[RADIANT_RADIO_DIAG_CRC_MAX_US] = cc_diag.crc_max_us;
		out->v[RADIANT_RADIO_DIAG_RX_CB_MAX_US] = cc_diag.rx_cb_max_us;
		out->v[RADIANT_RADIO_DIAG_T_SYNC_MAX_US] = cc_diag.t_sync_max_us;
		out->v[RADIANT_RADIO_DIAG_MATCH_MAX_US] = cc_diag.match_max_us;
	}
	irq_unlock(key);

	return RADIANT_DIAG_BACKEND_CC26XX;
}
#endif /* CONFIG_RADIANT_DIAG_COUNTERS */
