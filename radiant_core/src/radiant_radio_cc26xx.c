/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_cc26xx.c - radiant_radio_hal.h on TI's CC13x2/CC26x2 RF core.
 *
 * Provenance: clean-room. Written against radiant_radio_hal.h, against
 * docs/ant-radio-link.md, against TI's public CC13x2/CC26x2 Technical
 * Reference Manual and driverlib headers, against the public SimpleLink RF
 * driver API, and against the PHY parameters MEASURED by
 * radiant_core/spike/ti_phy on 2026-08-13. Nothing here derives from sdk-ant,
 * from libant.a, or from an adopter-gated ANT+ device profile document. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ===========================================================================
 * WHAT THIS FILE IS WRITTEN AGAINST, AND WHAT IT IS DELIBERATELY NOT
 * ===========================================================================
 * It is written against the HAL contract and against the spike's measurements.
 * It is NOT written against radiant_radio_nrf.c. That was an instruction and
 * it is worth restating as a design note, because the two radios disagree
 * about nearly everything that matters here and a port-by-analogy would have
 * inherited the wrong shape three times over:
 *
 *   - The nRF RADIO is a register peripheral driven by PPI and an ISR. This is
 *     a COMMAND PROCESSOR: software posts a radio operation to a doorbell and
 *     a separate CPU inside the RF core executes it. There is no "start the
 *     receiver" register to write at the right moment; there is a command with
 *     a trigger in it, and the trigger is programmed before the command is
 *     posted.
 *   - The nRF matcher has one shared BASE and eight prefixes. This has TWO
 *     FULLY INDEPENDENT SYNC WORDS and nothing else, so max_filters and
 *     max_addr_groups are both 2 and the constraint that makes them differ on
 *     the nRF does not exist here.
 *   - The nRF matches all five bytes of a tracking address. This matches at
 *     most FOUR (formatConf.nSwBits is 8..32), so the fifth arrives as a
 *     payload byte and the match is completed in software. The HAL anticipated
 *     exactly this - see caps.addr_len_hw_max and the filter_index contract -
 *     which is the strongest evidence available that the boundary was drawn in
 *     the right place.
 *
 * ===========================================================================
 * THE NUMBERS THIS FILE RESTS ON, AND HOW SOLID EACH ONE IS
 * ===========================================================================
 * Marked honestly, because a seeded constant that reads like a measured one is
 * the defect this project keeps re-learning.
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
 *   SEEDED, NOT MEASURED - and every one of them is a P3 bench item:
 *     T_SYNC_CAL_US, TX_RAMP_UP_US, RX_RAMP_UP_US, MIN_ARM_LEAD_US,
 *     RX_TO_TX_US, TX_TO_RX_US.
 *
 *   The seeded ones are deliberately PESSIMISTIC where pessimism is safe. A
 *   min_arm_lead_us that is too large costs schedulable gap and shows up as a
 *   scheduler statistic; one that is too small produces windows that open late
 *   and is indistinguishable from poor sensitivity. Only T_SYNC_CAL_US has no
 *   safe direction - see its own comment.
 * ===========================================================================
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
 * The PHY
 *
 * Every value here was either measured by the spike or taken from SmartRF's
 * own settings database for this part; nothing is a guess. The two that would
 * produce SILENCE rather than a weak link if wrong are called out.
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
 * Anti-aliasing filter bandwidth, left at TI's tc901 value.
 *
 * Swept and found not to matter once the two settings that DO matter are right:
 * 0x5 and 0xB scored 96% and 93% of frames sent on the same run, which is
 * inside the run-to-run spread. See AGC_REF_VALUE for what did matter.
 */
#define AA_FILTER_OVERRIDE_INDEX 2
#define AA_FILTER_VALUE          0x5

/*
 * AGC REFERENCE LEVEL, AND THIS IS THE ONE THAT WAS WRONG.
 *
 * tc901 sets it to 0x22. That is SmartRF's value for a 250 kbps PHY with a
 * 530 kHz receive bandwidth, and this file copied it in with the rest of the
 * override block without asking whether it transferred to a PHY four times the
 * rate and three times the bandwidth. It does not. Against tools/ant_sim.py at
 * a known 4.0049 Hz and -47 dBm:
 *
 *   agc    frames received, as a percentage of frames SENT
 *   0x22   37%, 47%, 47%
 *   0x2E   87%, 90%, 95%, 97%      <- TI's own default
 *   0x34   91%, 93%, 96%, 96%, 97%, 100%
 *   0x3A   87%, 93%
 *
 * Better than half of every transmission was being dropped by an override
 * inherited from a different PHY. The sweep that found it repeated its first
 * point last, every round, so the effect is the setting and not drift.
 *
 * 0x34 over TI's 0x2E is a smaller and less certain step than 0x22 -> 0x2E, and
 * it is taken because it was ahead in every round rather than because any one
 * round settled it.
 */
#define AGC_REF_OVERRIDE_INDEX   3
#define AGC_REF_VALUE            0x0034

/*
 * RX filter bandwidth code. 98 = 1567.2 kHz.
 *
 * The earlier sweeps kept choosing 96 (1092.5 kHz), and they were compensating
 * for a deviation that was wrong - see DEVIATION_UNITS. At 250 kHz deviation
 * Carson's rule asks for 2 * (250 + 500) = 1500 kHz, and 98 is the code above
 * it. Once the deviation and the AGC reference level were right, 98 beat 96 on
 * the same run (96% against 93%), which is the order the arithmetic predicts.
 *
 * HOW THIS WAS MEASURED, BECAUSE THE FIRST TIME IT WAS NOT. P1's gate reported
 * 40 of 40 CRC-valid and read as a clean pass. Its denominator was the frames
 * ALREADY DETECTED, so a PHY dropping half of everything scored exactly the
 * same as a perfect one - and this one was dropping better than half. Every
 * percentage in this file is now a fraction of frames a known 4.0049 Hz
 * transmitter actually SENT. A PHY gate needs a denominator it does not choose.
 */
#define RX_BW_CODE               98

/*
 * Deviation, in units of 250 Hz. 1000 units = 250 kHz.
 *
 * docs/ant-radio-link.md's figure is ~170 kHz and this file used 680 units to
 * match it. Measured, 1000 is at least as good at every AGC reference level
 * tried and better at some, and it is the value the modulation index argues
 * for: 250 kHz deviation at 1 Mbps is an index of 0.5, which is what a
 * BLE-shaped 1 Mbps GFSK link uses.
 *
 * THIS IS NOT A CLAIM ABOUT WHAT ANT TRANSMITS. It is the deviation this
 * DEMODULATOR should be told to expect, which is a receiver-side setting; the
 * two coincide for a matched filter and need not for a real one. The transmit
 * path uses the same field, so if a future bench measurement shows ANT's real
 * deviation is 170 kHz, this becomes a reason to split the field rather than a
 * reason to change it back - the receive number is measured and the transmit
 * number would not be.
 */
#define DEVIATION_UNITS          1000

static uint32_t phy_overrides[] = {
	/* override_prop_common.json: "DC/DC regulator: In Tx, use
	 * DCDCCTL5[3:0]=0x3 (DITHER_EN=0 and IPEAK=3)." */
	(uint32_t)0x00F388D3,
	/* override_tc901.json: "Tx: Configure PA ramp time, PACTL2.RC=0x1" */
	ADI_2HALFREG_OVERRIDE(0, 16, 0x8, 0x8, 17, 0x1, 0x0),
	/* override_tc901.json: "Rx: Set anti-aliasing filter bandwidth".
	 * MEASURED, not inherited: tc901's own value is 0x5 for a 530 kHz PHY
	 * and ANT's bandwidth is three times that. */
	ADI_HALFREG_OVERRIDE(0, 61, 0xF, AA_FILTER_VALUE),
	/* override_tc901.json: "Rx: Set AGC reference level to 0x22 (default:
	 * 0x2E)" - MEASURED AND OVERRIDDEN. See AGC_REF_VALUE: tc901's 0x22 was
	 * losing more than half of every transmission on this PHY. */
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
		 * TWENTY-FOUR BITS, FIXED, FOR EVERY OPERATION - and the
		 * reasoning is the most consequential thing in this file.
		 *
		 * nSwBits lives in the SETUP command, and the setup command is
		 * consumed by RF_open(). It is not a field that can be rewritten
		 * under a live handle: assigning to it between operations
		 * changes this struct in RAM and changes nothing in the radio.
		 * MEASURED - a first draft did exactly that, built clean, opened
		 * its channel, reported no error anywhere, and heard nothing at
		 * all for thirty seconds while a sensor two feet away was being
		 * decoded by another dongle.
		 *
		 * ANT's two geometries want different lengths: search matches
		 * [A6 C5 devnum_lo] and tracking matches
		 * [A6 C5 dnl dnh dtype]. The three ways to serve both:
		 *
		 *   1. Re-run CMD_PROP_RADIO_DIV_SETUP whenever the length
		 *      changes. Correct, and it costs a command chain -
		 *      setup -> CMD_FS -> the operation - because the synth
		 *      must be reprogrammed after a setup. That chain has to
		 *      run inside min_arm_lead_us, and the core arms from
		 *      inside completion callbacks where nothing may block.
		 *   2. Fix nSwBits at 32 and pad. Impossible: the search
		 *      address is three bytes on the air and there is no fourth
		 *      byte to match.
		 *   3. FIX IT AT 24 - the three bytes both geometries share -
		 *      and finish the remaining bytes in software.
		 *
		 * Three is chosen. It costs two extra software-compared bytes
		 * on a tracked frame and some spurious wake-ups on frames from
		 * other sensors that share a devnum_lo, and it buys a backend
		 * with no reconfiguration path at all in its arm calls. The
		 * HAL anticipated exactly this trade - caps.addr_len_hw_max is
		 * documented as informational, "a shorter hardware match means
		 * more spurious wakeups and more receive current, which the
		 * scheduler may reasonably care about" - so caps reports 3,
		 * which is what this backend actually does, rather than the 4
		 * the silicon could do under option 1.
		 */
		.nSwBits = 24,
		.bBitReversal = 0,
		/*
		 * bMsbFirst = 1, AND THIS IS THE ONE THAT LOOKS BACKWARDS.
		 *
		 * docs/ant-radio-link.md's "Fact two" says address bytes must be
		 * bit-reversed into the nRF's BASE/PREFIX. That is a fact about
		 * the nRF's address serialiser, which emits the address LSBit
		 * first while emitting the payload MSBit first under ENDIAN=Big.
		 * The invariant is the ON-AIR order, and on air the whole ANT
		 * frame is MSBit first. A radio that serialises sync word and
		 * payload the same way needs no reversal anywhere.
		 *
		 * Getting it wrong produces silence, not a weak link. Measured
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
 * RF_MODE_PROPRIETARY_2_4 with rf_patch_cpe_prop is what SmartRF's own 2.4 GHz
 * proprietary settings for this part select, and what the spike proved works.
 * RF_MODE_MULTIPLE with the multi-protocol patch is what an ANT-beside-BLE
 * build would need; choosing it here would have answered two questions at
 * once, which is how the multiprotocol work on the nRF side went wrong.
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
 * T_SYNC_CAL_US - the correction from what the hardware captured to what the
 * HAL defines t_sync to be.
 *
 * THE MODEL. CMD_PROP_RX_ADV's appended timestamp is a RAT capture taken when
 * the sync word is found. This file converts it to the HAL's t_sync - "the
 * last bit of the ON-AIR ADDRESS was at the antenna" - by adding the airtime
 * of the address bytes that are NOT in the sync word (see rx_t_sync()) and
 * then this constant, which absorbs demodulator group delay, filter latency
 * and capture offset.
 *
 * IT IS ZERO AND THAT IS A PLACEHOLDER, NOT A MEASUREMENT. The HAL's t_sync
 * section spells out why this is the subtlest number in the port: a CONSTANT
 * error cancels out of the period estimate, so the drift PLL still locks and
 * nothing looks wrong, while every RX window shifts by that amount and eats
 * its guard on one side. There is no error code and no log line - swapping
 * backends simply changes yield by a few tenths of a percent, at the same
 * order as this bench's characterised ~0.4 % collision floor.
 *
 * UNLIKE EVERY OTHER SEEDED CONSTANT HERE, THIS ONE HAS NO SAFE DIRECTION.
 * Measuring it needs the wired two-board trigger the HAL describes: one board
 * pulses a GPIO from its own address-sent event, the other captures that pulse
 * on the same timer it timestamps t_sync with. Until that is done, the A/B
 * `timing` gate against the nRF backend is the only evidence available and it
 * bounds the error rather than measuring it.
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
 * EXTRA RECEIVE TIME AT THE LATE EDGE OF EVERY WINDOW, AND IT IS WORTH 70
 * POINTS OF PACKET LOSS ON A TRACKED CHANNEL.
 *
 * The HAL's window is [t_open, t_close] in t_sync terms and the obvious end
 * trigger is t_close plus one byte, so that a frame whose sync lands exactly on
 * t_close still gets its address through. That is correct arithmetic and it
 * measured 79% loss.
 *
 *   end = t_close + BYTE_US            79.0% loss   (126 of 601 packets)
 *   end = t_close + BYTE_US +  500 us   8.3% loss   (551 of 601)
 *   end = t_close + BYTE_US + 2000 us   9.0% loss   (547 of 601)
 *
 * The frames were never arriving late: with the slop in place, 543 of 554
 * landed INSIDE the core's own window, 0 to 200 us before t_close. A window
 * placed correctly was still losing four frames in five.
 *
 * What the arithmetic misses is that endTrigger stops SYNC SEARCH, not just
 * packet reception. endType = 1 lets a packet that has already started finish;
 * it does not give the correlator the run-up it needs to start one. A tracked
 * window is about 400 us wide, so an end trigger placed to the microsecond
 * leaves the search almost no armed time at the edge where the frame actually
 * is - and a miss there is not a lost packet, it is a lost ANCHOR: the channel
 * free-runs, drifts, and misses the next four too. That is the 1-in-5 pattern
 * the gap histogram showed, and it is why the loss is so much worse than the
 * per-frame odds suggest.
 *
 * 500 and 2000 measure the same, so the cost is bounded and 500 is taken. This
 * is receive current on every window and it is the price of a correlator that
 * needs to be running before the thing it is looking for arrives.
 */
#define RX_END_SLOP_US         500
#define RX_TO_TX_US            250
#define TX_TO_RX_US            250

/*
 * MIN_ARM_LEAD_US - between radiant_radio_now() and the earliest instant an
 * arm call can still honour.
 *
 * Larger than the nRF backend's, and that is a property of the part rather
 * than caution: an arm here is a command posted through a driver to a separate
 * processor's doorbell, not a register write. It covers building the command,
 * RF_postCmd's own work, and the RF core waking its command processor.
 */
#define MIN_ARM_LEAD_US        600

/*
 * ARM_FLOOR_US - the lead below which an arm is REFUSED, as distinct from the
 * lead the scheduler is asked to budget.
 *
 * These have to be two numbers and the reason is measured. radiant_sched.c
 * subtracts caps.min_arm_lead_us itself and then calls, so it aims at
 * approximately now + min_arm_lead_us; the microseconds that then elapse
 * between its decision and this function's clock read come straight off that
 * margin. With one number the first real arm looked like this:
 *
 *   rx refused rc=-4 n=2 alen=3 open=2573983 now=2573438
 *
 * 545 us of lead against a 600 us test - fifty-five microseconds short, on
 * every window, forever. The channel opened, the radio ran, no counter moved
 * and the dongle reported a clean empty scan.
 *
 * So min_arm_lead_us stays at the honest BUDGETING figure - what the scheduler
 * should leave if it wants a window to open on time - and the REFUSAL is
 * against what the hardware genuinely cannot do. Below this the command would
 * be posted with its trigger already past; pastTrig = 1 means that still
 * starts the window rather than erroring, just late, so the floor is about
 * when lateness stops being worth having rather than when the radio breaks.
 *
 * The HAL's rule is "a backend never silently transmits late", and that is
 * kept: this is not silent. A window that opens between the floor and the
 * advertised lead has lost part of its head, and the frames it then fails to
 * hear are counted as misses by the same accounting as any other miss.
 */
#define ARM_FLOOR_US           150

/* ---------------------------------------------------------------------------
 * Receive buffers
 *
 * SIZED FOR THE WORST CASE THE HAL PERMITS, not for ANT: RADIANT_RADIO_BODY_MAX
 * plus the one address byte the hardware matcher cannot reach plus two CRC
 * bytes plus the appended RSSI/timestamp/status. ANT's frames are 10 or 12
 * body bytes and this is 40, which costs static RAM and nothing else.
 *
 * FOUR ENTRIES because a merged window may deliver several frames back to back
 * and the callback that drains them runs at RF driver priority, not at the
 * radio's. Two would be enough for ANT's slot geometry and four is the cheapest
 * insurance against a burst.
 * ---------------------------------------------------------------------------
 */
#define RX_CRC_BYTES      2u
#define RX_SW_ADDR_MAX    (RADIANT_RADIO_ADDR_MAX - 4u)   /* 1 */
#define RX_PAYLOAD_MAX    (RX_SW_ADDR_MAX + RADIANT_RADIO_BODY_MAX + RX_CRC_BYTES)
#define RX_APPEND_BYTES   (1u /* RSSI */ + 4u /* RAT */ + 1u /* status */)
#define RX_BUF_BYTES      (RX_PAYLOAD_MAX + RX_APPEND_BYTES)
#define RX_ENTRIES        4

/*
 * EVERY ENTRY 4-BYTE ALIGNED INDIVIDUALLY, WHICH __aligned(4) ON THE ARRAY DOES
 * NOT GIVE YOU.
 *
 * The RF core requires each rfc_dataEntryGeneral_t to be word aligned. Aligning
 * a two-dimensional array aligns the FIRST row; every later row starts at
 * base + i * rowsize, so the rows are only aligned if rowsize is a multiple of
 * four. Here the natural row is sizeof(rfc_dataEntryGeneral_t) + 21 = 33 bytes,
 * and rows 1..3 land on odd addresses.
 *
 * MEASURED, and it presented as a total receive failure with no error anywhere:
 * the RF core filled entry 0 correctly - nRxOk = 1, status FINISHED - advanced
 * its write pointer to the misaligned entry 1, and the frame in entry 0 was
 * never delivered. The spike escaped this by arithmetic luck; its row is 32
 * bytes, and nothing about it was a decision.
 *
 * ROUND_UP on the row is the fix, and it is cheap - three bytes per entry.
 */
#define RX_ROW_BYTES  ROUND_UP(sizeof(rfc_dataEntryGeneral_t) + RX_BUF_BYTES, 4)

static uint8_t rx_data[RX_ENTRIES][RX_ROW_BYTES] __aligned(4);
static dataQueue_t rx_queue;
static rfc_propRxOutput_t rx_output;

/*
 * THE READ CURSOR IS SOFTWARE'S, AND IT IS NOT rx_queue.pCurrEntry.
 *
 * pCurrEntry belongs to the RF core: it is where the core will WRITE next, and
 * the core advances it as it fills entries. Draining from it reads the entry
 * the radio is about to use - status PENDING - so the loop breaks immediately
 * and the finished entry behind the pointer is never seen. TI's own rfQueue
 * helper keeps a separate read pointer for exactly this reason.
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
 * The RAT is 32 bits at 4 MHz, so it wraps every 2^32 / 4e6 = 1073 s, a little
 * under 18 minutes. The HAL's rule 2 requires 64-bit absolute microseconds that
 * the core may subtract freely, and the HAL's own commentary names this exact
 * duty: "a dongle left plugged in over a weekend must not acquire a scheduling
 * bug at minute 72". Here it would be minute 18.
 *
 * The fold is the ordinary one - detect a step backwards, add 2^32 - and it is
 * correct only if it is SAMPLED MORE OFTEN THAN THE WRAP PERIOD. The scheduler
 * calls radiant_radio_now() constantly, so in practice it is sampled thousands
 * of times a second; but "in practice" is how this class of bug survives to
 * ship, so a k_timer forces a sample every 60 s regardless of what the core is
 * doing. That covers a disabled radio, an idle dongle and a core stuck behind
 * a long USB stall equally.
 *
 * THE MEASUREMENT THAT MAKES THIS SIMPLE. The spike established that the RAT
 * FREE-RUNS ACROSS RF_close()/RF_open() - 200 337 850 ticks over five power
 * cycles, exactly the elapsed wall time. So there is no epoch to re-establish
 * on enable, and radiant_radio_disable()/enable() does not perturb the
 * timebase, which is precisely what the HAL requires of that pair.
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

	/*
	 * Before RF_open and after RF_close there is no RAT to read, and the
	 * HAL still requires a monotonic answer. Synthesising one from
	 * k_uptime is not as good as the RAT and does not have to be: the only
	 * callers in that state are the core deciding whether it is worth
	 * arming anything, and nothing is being scheduled against it.
	 */
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
 * CRC
 *
 * Generic over struct radiant_crc_cfg rather than hardcoded to CCITT-FALSE.
 * The HAL expresses the CRC as a value and a backend that hardcoded ANT's
 * would silently transmit the wrong CRC the first time a format arrived with a
 * different one - and the long-range format ADR 0007 authors is exactly such a
 * format.
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
	 * TWO, AND IT IS A PRODUCT REGRESSION THAT IS KNOWN IN ADVANCE.
	 * CMD_PROP_RX_ADV has syncWord0 and syncWord1 and nothing else. The nRF
	 * has eight. radiant_search.c enumerates max_filters concrete addresses
	 * per window and sweeps enough sets to cover all 256 values of
	 * devnum_lo: 32 sets at eight filters, 128 SETS AT TWO, which breaks
	 * ab_gates.toml's [gates.acquisition] max_absolute_s = 5.0 by
	 * construction.
	 *
	 * The instruction on that is explicit and is repeated here so it cannot
	 * be quietly disregarded: RECORD the measured discovery time, STATE the
	 * regression, and decide separately whether the sweep wants a different
	 * strategy at low filter counts. DO NOT TUNE THE GATE TO FIT.
	 */
	.max_filters = 2,

	/*
	 * Equal to max_filters, unlike the nRF. Each sync word is a fully
	 * independent 32-bit value, so there is no shared-BASE constraint and
	 * two tracked channels with different device numbers CAN share a
	 * window. The nRF's max_addr_groups = 2 comes from a register layout;
	 * this 2 comes from there being two sync words at all. Same number,
	 * unrelated reasons - and it means the tracking case is not the extra
	 * penalty here that it is there.
	 */
	.max_addr_groups = 2,

	/*
	 * FOUR BITS, AND THIS IS THE SINGLE MOST EXPENSIVE THING THIS PART DOES
	 * DIFFERENTLY FROM THE nRF.
	 *
	 * The two sync words are matched by a correlator, not a comparator, and
	 * it cannot separate two templates that are close. Measured against a
	 * transmitter at -47 dBm sending a known 4.0049 frames per second, with
	 * syncWord1 set to its address and syncWord0 a controlled distance away
	 * - out of about thirty frames each point could have received:
	 *
	 *     1 bit apart    0 frames      4 bits apart   18 frames
	 *     2 bits apart   3 frames      8 bits apart   20 frames
	 *
	 * A single sync word scores 21-24 on the same run, so four bits is
	 * where the penalty stops mattering and one bit is total deafness.
	 *
	 * This is declared rather than worked around because the backend cannot
	 * fix it: it does not choose the addresses, it is handed them. The
	 * search sweep chose consecutive devnum_lo values - 2k and 2k+1, one
	 * bit apart, every set - which was free on the nRF and made this
	 * backend receive nothing at all through a whole sweep while reporting
	 * no error anywhere.
	 */
	.min_filter_hamming_bits = 4,

	.filter_wildcard_dev = false,

	/*
	 * THREE, WHICH IS LESS THAN THE SILICON CAN DO, AND DELIBERATELY.
	 * formatConf.nSwBits is documented 8..32, so the matcher could reach
	 * four bytes - but nSwBits lives in the setup command that RF_open()
	 * consumed, and serving ANTs two address lengths from one open handle
	 * therefore means fixing it at the three bytes they share. The full
	 * reasoning, and the silent failure a first draft produced by assuming
	 * otherwise, is in setup_cmd.formatConf above.
	 *
	 * So on a tracked channel the fourth and fifth on-air bytes arrive as
	 * the leading payload bytes and are matched in software; see
	 * rx_match(). The HAL says this changes cost and not semantics, and
	 * that is exactly right: more spurious wakeups and more receive
	 * current, identical delivered frames.
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

	/* The appended timestamp is a RAT capture of the sync-word event, not a
	 * software read. The CALIBRATION of it is still outstanding - see
	 * T_SYNC_CAL_US - but that is a constant offset, not a loss of
	 * hardware capture, and reporting false here would make radiant_event.c
	 * advertise the 0xE0 timestamp as approximate when it is not. */
	.has_sync_timestamp = true,

	.has_rssi = true,
	.has_ed_scan = false,        /* see radiant_radio_ed() */
	.has_rx_crc = true,          /* software CRC: the received value is kept */
	.crc_in_hw = false,          /* see rx_cmd.pktConf.bUseCrc */

	/*
	 * ONE POWER, AND caps NOW SAYS SO. This read -20 .. +5 and that was a
	 * capability this backend does not have: radiant_radio_tx() never looked
	 * at req->power at all, and setup_cmd.txPower is the fixed 0x7217 that
	 * RF_open() consumed - SmartRF's +5 dBm setting for this band on the
	 * LaunchXL's differential front end. So a host that set a transmit power
	 * was answered cheerfully and ignored, and the range in the capability
	 * query was the only place that claimed otherwise.
	 *
	 * Found by tools/ant_sens.py: its ladder walks the TRANSMITTER's power
	 * down, and a backend that cannot move its own is a backend that can
	 * never be the instrument in a sensitivity measurement. That the tool
	 * checks its dial against measured RSSI rather than trusting it is the
	 * only reason this surfaced rather than producing a fabricated dB figure.
	 *
	 * Making the range real needs RF_setTxPower() - which RFCC26X2.h does
	 * provide - and an RF_TxPowerTable_Entry table of raw register values for
	 * this part, band and front end. Those come out of SmartRF Studio and are
	 * not derivable from anything in this tree, so the honest move is to
	 * report the one operating point that IS implemented and leave the table
	 * as named future work rather than guess register values into a header.
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
	 * core CAN decode a length field - hdrConf.numLenBits exists - but the
	 * combination this backend would need is a length field sitting AFTER
	 * an address byte the matcher did not consume, which hdrConf cannot
	 * express: the header is counted from the first bit after the sync
	 * word, and on a 5-byte address that bit is the device type.
	 *
	 * No ANT format uses this mode and none can (radiant_radio_hal.h), so
	 * nothing ANT-shaped is lost. The long-range format does use it, and it
	 * is on a PHY this build does not have either - so the refusal is
	 * consistent rather than partial.
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
		 * expressible, because the software path reassembles the
		 * covered bytes from the filter's address plus the payload and
		 * that reassembly is what cover_addr means. A 16-bit CRC that
		 * did NOT cover the address would be easy to add and has never
		 * been asked for; refusing is better than a path with no test.
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
 * Which filter this frame is for, or -1.
 *
 * The hardware matched the first hw_addr_len bytes and told us WHICH sync word
 * did it. That narrows the answer to the filters sharing that sync word; the
 * remaining address bytes are the leading payload bytes, and comparing them is
 * what finishes the match.
 *
 * THE RETURN VALUE IS THE HAL'S filter_index AND THE CORE DEPENDS ON IT FOR
 * IDENTITY, not merely for bookkeeping: in wildcard search the third on-air
 * byte is devnum_lo, so the index IS the identity of that byte. Returning the
 * wrong one does not drop a frame, it attributes it to the wrong sensor.
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
	 * Fold the CAPTURE by its SIGNED DISTANCE FROM NOW rather than by
	 * OR-ing it into the current high word.
	 *
	 * The OR is the obvious thing and it is wrong near a wrap: a capture
	 * taken a few hundred microseconds before the counter rolled over gets
	 * the NEW high word and lands 2^32 ticks - eighteen minutes - in the
	 * future. Nothing catches it. The frame then falls outside
	 * [t_open, t_close] and is silently dropped, twice an hour, on a
	 * backend that otherwise works perfectly.
	 *
	 * The subtraction below is exact in 32-bit two's complement for any
	 * capture within +-2^31 ticks (+-9 minutes) of now, which every capture
	 * is by a factor of thousands, and it needs no reasoning about epochs
	 * at all.
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

		/*
		 * Per-frame tracing lived here through the whole bring-up and
		 * is deliberately gone. What it was for - "the window armed,
		 * the radio ran, and nothing came out" - is answered by the
		 * command status at LOG_WRN in rf_callback(), which costs
		 * nothing when things are working. A LOG_DBG per frame at
		 * CONFIG_LOG_MODE_IMMEDIATE is not free: it blocks in the RF
		 * driver's callback context, and on the diagnostic build that
		 * shares uart0 with the ANT stream it shreds the very
		 * broadcasts it is being used to look for.
		 */

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
	 * THE COMMAND'S OWN STATUS WORD, AT WARNING LEVEL, WHENEVER IT IS NOT
	 * A SUCCESS CODE.
	 *
	 * This is the single most useful line in the file when something is
	 * wrong, and it is here because its absence cost an evening. Every way
	 * this backend can fail to hear a frame - a trigger the core rejected,
	 * a length it would not accept, a synthesiser that was never
	 * programmed - ends the same way from the driver's point of view:
	 * RF_EventLastCmdDone, no error, an empty window, and a scheduler that
	 * calmly re-arms. The RF core distinguishes them all, in one 16-bit
	 * field, and says nothing unless it is read.
	 *
	 * PROP_DONE_OK is 0x3400 and PROP_DONE_RXTIMEOUT 0x3401; anything in
	 * the 0x38xx range is PROP_ERROR_*, and 0x0800..0x08FF is a generic
	 * command error such as ERROR_PAST_START.
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
	 * KEEP THE RF CORE POWERED BETWEEN OPERATIONS.
	 *
	 * nInactivityTimeout is a microsecond count and its DEFAULT IS ZERO,
	 * which means "power the core down the moment the command queue drains"
	 * - not "never power down", which is what the name suggests and what an
	 * earlier version of this comment claimed while setting it to 0 and
	 * congratulating itself.
	 *
	 * Powering down between windows costs roughly 1.5 ms to come back,
	 * which is more than caps.min_arm_lead_us and is paid on exactly the
	 * windows a tracked channel cannot afford to miss. A dongle is
	 * mains-powered over USB, so the trade is easy here; it would be the
	 * wrong one on a coin cell.
	 */
	params.nInactivityTimeout = UINT32_MAX;

	st.rf_handle = RF_open(&st.rf_object, &rf_mode,
			       (RF_RadioSetup *)&setup_cmd, &params);
	if (st.rf_handle == NULL) {
		LOG_ERR("RF_open failed");
		return RADIANT_RADIO_EIO;
	}

	/*
	 * Programme the synthesiser once, here, rather than per operation:
	 * every ANT+ operation is on the same frequency, and paying the FS time
	 * inside every arm would put it inside min_arm_lead_us.
	 *
	 * TEST THE BIT, NOT THE VALUE. RF_runCmd returns an RF_EventMask and a
	 * successful command routinely sets more than one bit in it. An earlier
	 * version compared the whole mask against RF_EventLastCmdDone, decided
	 * a perfectly good CMD_FS had failed, closed the handle and returned
	 * EIO from enable() - after which the radio was never opened again, the
	 * scheduler armed nothing, and a host could still assign a channel, set
	 * a frequency and OPEN it, all answered cheerfully, and then hear
	 * nothing at all. That is the second time in this bring-up that a
	 * failure produced an entirely clean-looking empty scan.
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
		 * ONE GROUP, AND syncWord1 MUST BE FAR AWAY RATHER THAN EQUAL.
		 *
		 * A first draft left it uninitialised, which dropped every
		 * frame; the fix was to duplicate syncWord0, on the reasoning
		 * that the same word cannot match anything the window did not
		 * ask for. That reasoning is about a COMPARATOR and this is a
		 * CORRELATOR - see caps.min_filter_hamming_bits. Two identical
		 * templates are zero bits apart, which is the far end of the
		 * curve that reads 0 frames at one bit, and it made every
		 * TRACKED window fail: 454 EVENT_RX_FAIL against one packet
		 * received, on a link the search path was walking over.
		 *
		 * So the second word is syncWord0 with the top matched byte
		 * inverted. That is eight bits away, and it is a byte no ANT
		 * frame can carry - the network address begins A6 and this
		 * makes it 59 - so it cannot match anything real either.
		 *
		 * sync_group[1] is still pointed at the same filter. The decoy
		 * cannot legitimately fire, so a syncWordId of 1 here can only
		 * be the core misreporting the word that did, and attributing
		 * that frame to the one filter this window has is right.
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
	 * pastTrig = 1 ON BOTH TRIGGERS, AND IT IS NOT A CONVENIENCE.
	 *
	 * With pastTrig = 0 the RF core treats a trigger time that has already
	 * passed as an ERROR: the command ends immediately with
	 * PROP_ERROR_PAR and the driver reports RF_EventLastCmdDone, which
	 * this backend cannot tell apart from a window that opened, listened
	 * and heard nothing. The scheduler then re-arms, misses again, and the
	 * dongle reports a clean empty scan forever.
	 *
	 * A few microseconds late is a real and ordinary condition here -
	 * posting a command travels through the driver to another processors
	 * doorbell - and the right response to it is to start now and lose the
	 * first microseconds of the window, not to refuse the window.
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
	 * req->power IS DELIBERATELY NOT READ, AND caps SAYS SO.
	 *
	 * setup_cmd.txPower is fixed at RF_open() time and this backend has one
	 * operating point, so tx_power_min_dbm == tx_power_max_dbm == 5 and the
	 * core has already clamped anything the host asked for to the only value
	 * available. Ignoring the field here is therefore correct rather than
	 * forgotten - but it read as forgotten for the whole of P2, because caps
	 * advertised a range instead. See the caps comment for what a real dial
	 * would take.
	 */

	/*
	 * WHICH BYTES ARE "ADDRESS" AND WHICH ARE "BODY" IS A RECEIVER'S SPLIT,
	 * NOT A PROPERTY OF THE AIR. Transmitting with a 4-byte sync word and
	 * the fifth address byte at the head of the packet produces exactly the
	 * frame a 5-byte matcher expects to see.
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
	 * caps.has_ed_scan is false, so this is the ordinary answer rather than
	 * a degraded one - the HAL says so in as many words.
	 *
	 * The part could do it: CMD_PROP_CS and CMD_PROP_RADIO_DIV_SETUP's RSSI
	 * path are both available, and a sweep would be a chain of carrier-sense
	 * commands. It is not here because a measurement whose scale does not
	 * match rx_event.rssi_dbm and noise_dbm exactly is worse than none -
	 * the HAL requires "same corrections, same reference", and establishing
	 * that on a new part is a bench exercise, not a coding one.
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
