/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Spike TI-PHY - can a CC26x2 RF core hear a real ANT+ frame?
 *
 * Clean-room: written from docs/ant-radio-link.md, TI's public CC13x2/CC26x2
 * Technical Reference Manual and driverlib headers (rf_prop_cmd.h,
 * rf_prop_mailbox.h, rf_mailbox.h), the public SimpleLink RF driver API, and
 * SmartRF Studio's published settings database as shipped in the SimpleLink
 * CC13xx/CC26xx SDK (source/ti/devices/radioconfig/.meta/config/
 * cc2652r_prop_pg21/). Nothing derives from sdk-ant, libant.a, or an
 * ANT+ profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * What this is for: the TI port rests on one untested claim - that TI's RF
 * core can be configured for ANT's PHY (1 Mbps 2-GFSK, ~170 kHz deviation)
 * and demodulate a byte-exact ANT frame. Not obviously true: SmartRF ships
 * only three 2.4 GHz proprietary PHYs for this part, all stopping at 250 kbps
 * (tc900 100 kbps/50 kHz, tc901 250 kbps/125 kHz, tc902 250 kbps/62.5 kHz
 * MSK). So this is a go/no-go, written to FAIL LOUDLY rather than succeed
 * quietly - if it can't receive a frame, the TI backend isn't written and the
 * port stops here, the cheapest place to stop. Sixth spike in this
 * directory, same shape: bare, no radiant, no HAL, just the peripheral
 * and a console.
 *
 * Two things the settings database already answered, before any hardware:
 *   1. The rate is reachable on paper. symbolRate is a 21-bit rate word (1
 *      Mbps nowhere near a ceiling) and the RX filter bandwidth table runs
 *      to 1883.7 kHz. Carson's rule wants ~1340 kHz for ANT at 1 Mbps/170 kHz
 *      deviation, and the table has codes at 1243.2 and 1567.2 either side.
 *      The stock settings stop at 250 kbps because nobody at TI wrote a
 *      faster one, not because the front end can't.
 *   2. The hardware sync word is four bytes, not five. nSwBits is documented
 *      8..32; ANT's tracking address is five bytes, so this part can't match
 *      a tracked channel's address in hardware - a backend must match four
 *      and finish the fifth in software. A real caps difference from the
 *      nRF, knowable from a header.
 *
 * The geometry, and why it's the search one: ANT's frame after the preamble
 * is 17 bytes, CRC covering the first 15. This spike splits them the way a
 * SEARCH window does (3-byte sync word A6 C5 devnum_lo, 12-byte body, 2-byte
 * CRC) rather than a TRACKING window (5-byte address, 10-byte body), for two
 * reasons: inherited from Spike B, a receiver that misparses a body byte
 * can't report what it is, so search geometry puts every interesting byte in
 * RAM; and new, it fits inside the 4-byte hardware limit above with a byte
 * to spare, answering the PHY question without also answering the
 * address-matcher question at the same time.
 *
 * Running it: two boards, one transmits one receives (a board can't hear
 * itself). TRANSMITTER: the Feather, already flashed with the ordinary
 * dongle image (0FCF:1009), driven as an ANT+ master from the host - costs
 * no Feather flash:
 *   & <python> tools\ant_sim.py --serial 6183 --seconds 720 -q
 * Confirm device number/type/trans type independently with tools/ant_scan.py
 * and set them below - a spike that matches its own assumption proves
 * nothing. RECEIVER: the LaunchXL. Output is on the XDS110 backchannel COM
 * port at 115200 (COM14 on this bench) - the spike's arrangement, opposite
 * the dongle image's which reserves that port for ANT frames; see prj.conf.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>

#include <driverlib/rf_mailbox.h>
#include <driverlib/rf_common_cmd.h>
#include <driverlib/rf_prop_cmd.h>
#include <driverlib/rf_prop_mailbox.h>
#include <driverlib/rf_data_entry.h>
#include <rf_patches/rf_patch_cpe_prop.h>
#if defined(SPIKE_TI_PHY_MULTI_PATCH)
#include <rf_patches/rf_patch_cpe_multi_protocol.h>
#endif

#include <ti/drivers/rf/RF.h>

/* Ground truth: the transmitter's channel ID, as heard by a SECOND,
 * independent ANT stick - not the values ant_sim.py is asked for, since it
 * derives the device number from the transmitting stick's own identity, not
 * the command line. Confirmed 2026-08-13:
 *   & <python> tools\ant_sim.py --serial 6183 --seconds 900 -q
 *   & <python> tools\ant_scan.py --serial 772A --seconds 20
 *     found: #52233 - power meter
 * so devnum_lo = 0x09, sync word A6 C5 09. Transmission type 5 is
 * ant_sim.py's default and not independently observable via ant_scan.py -
 * the one value here taken on trust; a mismatch on it alone wouldn't
 * invalidate a receive. */
/* 14871 is ant_sim.py's device number, not an ambient sensor's - the point
 * of re-running this. Earlier sweeps ran against whatever was transmitting
 * in the room (a real device on 52233, unpaced by this bench), making
 * "entries" a measurement of the room as much as the setting. A known,
 * bench-launched transmitter at 4.00 Hz turns "frames caught" into a rate
 * against a known denominator. Start the transmitter first:
 *   python tools\ant_sim.py --profile power --seconds 260 */
#define SPIKE_DEVNUM  14871u
#define SPIKE_DTYPE   0x0Bu   /* bicycle power */
#define SPIKE_TTYPE   5u

/* ANT+ network address and RF index. Both public - see docs/ant-radio-link.md. */
#define ANT_NET_HI    0xA6u
#define ANT_NET_LO    0xC5u
#define ANT_RF_INDEX  57u
#define ANT_FREQ_MHZ  (2400u + ANT_RF_INDEX)

/* The frame, split the search way. */
#define SPIKE_SYNC_BYTES  3u
#define SPIKE_BODY_BYTES  12u
#define SPIKE_CRC_BYTES   2u
#define SPIKE_CRC_COVER   (SPIKE_SYNC_BYTES + SPIKE_BODY_BYTES)   /* 15 */

/* Phase 0: the CRC, before the radio is touched. CRC-16/CCITT-FALSE: width
 * 16, poly 0x1021, init 0xFFFF, no reflection, no final XOR, covering the
 * on-air address bytes as well as the body.
 *
 * Software here, deliberately - the plan's instruction is bytes on the
 * ground before optimising. Worth noting the hardware path looks unlike the
 * EFR32 one in docs/backends.md: that CRC engine can fold a constant prefix
 * (CRC(A6 C5) = 0x233E) into the initial value to start after the sync
 * word, but the trick fails for the 5-byte tracking format whose sync word
 * carries the device number. TI's RX command has bCrcIncSw ("include sync
 * word in CRC calculation"), so the fold may not be needed here in either
 * geometry - whether the engine computes CCITT-FALSE specifically is a
 * measurement, not a reading. */
static uint16_t crc_ccitt_false(const uint8_t *p, size_t n)
{
	uint16_t crc = 0xFFFFu;

	for (size_t i = 0; i < n; i++) {
		crc ^= (uint16_t)p[i] << 8;
		for (int b = 0; b < 8; b++) {
			crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
					      : (uint16_t)(crc << 1);
		}
	}

	return crc;
}

/* The golden vector from docs/ant-radio-link.md, re-derived at boot before
 * the radio is touched (Spike A does the same): if this fails, nothing
 * printed later about a CRC means anything - the fault is in fifteen bytes
 * of arithmetic, not a radio. */
static bool crc_self_test(void)
{
	static const uint8_t msg[15] = {
		0xA6, 0xC5, 0x34, 0x12, 0x78, 0x01, 0x0A, 0x00,
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07
	};
	static const uint8_t msg_plus_crc[17] = {
		0xA6, 0xC5, 0x34, 0x12, 0x78, 0x01, 0x0A, 0x00,
		0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x1B, 0x12
	};
	static const uint8_t prefix_bytes[2] = { 0xA6, 0xC5 };

	uint16_t c = crc_ccitt_false(msg, sizeof(msg));
	uint16_t z = crc_ccitt_false(msg_plus_crc, sizeof(msg_plus_crc));
	uint16_t prefix = crc_ccitt_false(prefix_bytes, sizeof(prefix_bytes));

	printk("  CRC(15 golden bytes)      = 0x%04X  (expect 0x1B12)  %s\n",
	       c, c == 0x1B12u ? "ok" : "FAIL");
	printk("  CRC(message + its CRC)    = 0x%04X  (expect 0x0000)  %s\n",
	       z, z == 0x0000u ? "ok" : "FAIL");
	printk("  CRC(A6 C5)                = 0x%04X  (expect 0x233E)  %s\n",
	       prefix, prefix == 0x233Eu ? "ok" : "FAIL");

	return (c == 0x1B12u) && (z == 0x0000u) && (prefix == 0x233Eu);
}

/* The PHY. Every field below whose value follows a documented formula
 * carries the formula; every override word carries SmartRF's own comment.
 *
 * Command is CMD_PROP_RADIO_DIV_SETUP, not CMD_PROP_RADIO_SETUP - "DIV"
 * reads as sub-GHz but isn't: SmartRF's own 2.4 GHz settings use the DIV
 * form (loDivider=0, centerFreq=2440) because that's the command that
 * carries a centre frequency at all. Getting this wrong transmits nowhere
 * near where you asked.
 *
 * modulation.deviation is in units of deviationStepSz (250 Hz at setting 0):
 * 170000/250 = 680. Checked against tc901: 125 kHz -> 500, same rule.
 *
 * symbolRate follows TRM 25.10.5.2: rateWord = symbolRate*preScale*2^20/24e6.
 * preScale 15, 1 Mbps: 1e6*15*1048576/24e6 = 655360. Checked against tc901's
 * 250 kbps -> 163840; 655360 = 4*163840, the rate ratio as sanity check.
 *
 * rxBw is a CODE from the settings-database table (93=621.6 94=783.6
 * 95=941.8 96=1092.5 97=1243.2 98=1567.2 99=1883.7 kHz). ANT needs ~1340 kHz
 * with no code there, so the first attempt sweeps the codes either side
 * rather than choosing - the field most likely wrong, why phase 2 sweeps.
 *
 * formatConf.bMsbFirst = TRUE, not what a hasty reading of
 * docs/ant-radio-link.md's "Fact two" produces: that fact bit-reverses
 * address bytes into the nRF's BASE/PREFIX because the nRF serialises its
 * address LSBit-first but payload MSBit-first under ENDIAN=Big. The
 * invariant is the ON-AIR order, which for the whole ANT frame is
 * MSBit-first - a radio serialising sync word and payload the same way
 * needs no reversal anywhere. Getting this backwards produces SILENCE, not
 * a weak link, matching what Spike A measured on the nRF. */

/* SmartRF's override set for 2.4 GHz proprietary mode on CC2652R, from
 * setting_tc901.json, with the one bandwidth-dependent entry lifted into the
 * sweep below. AGC reference level and PA ramp are common to any 2.4 GHz
 * proprietary PHY on this part; anti-aliasing filter bandwidth is not (a
 * function of rxBw, 0x5 is tc901's value for 530 kHz) - ANT's ~1.3 MHz needs
 * something wider, not derivable from anything shipped, so it's swept. */
#define AA_FILTER_OVERRIDE_INDEX 2
/* The AGC reference level entry, swept alongside the filter bandwidth. TI's
 * default is 0x2E and tc901 lowers it to 0x22; which one suits a 1 Mbps PHY
 * with a one-byte preamble is not something tc901 can answer. */
#define AGC_REF_OVERRIDE_INDEX   3

static uint32_t phy_overrides[] = {
	/* override_prop_common.json:
	 * "DC/DC regulator: In Tx, use DCDCCTL5[3:0]=0x3 (DITHER_EN=0 and IPEAK=3)."
	 */
	(uint32_t)0x00F388D3,
	/* override_tc901.json:
	 * "Tx: Configure PA ramp time, PACTL2.RC=0x1 (in ADI0, set PACTL2[4:3]=0x1)"
	 */
	ADI_2HALFREG_OVERRIDE(0, 16, 0x8, 0x8, 17, 0x1, 0x0),
	/* override_tc901.json:
	 * "Rx: Set anti-aliasing filter bandwidth (in ADI0, set IFAMPCTL3[7:4])"
	 * SWEPT - see AA_FILTER_OVERRIDE_INDEX. tc901's value for 530 kHz is
	 * 0x5; ANT's bandwidth is more than twice that.
	 */
	ADI_HALFREG_OVERRIDE(0, 61, 0xF, 0x5),
	/* override_tc901.json:
	 * "Rx: Set AGC reference level to 0x22 (default: 0x2E)"
	 */
	HW_REG_OVERRIDE(0x609C, 0x0022),
	/* override_hposc.json:
	 * "HPOSC frequency offset override, freqOffset=2^22*(F_nom-F_hposc)/F_hposc"
	 */
	HPOSC_OVERRIDE(0),
	(uint32_t)0xFFFFFFFF
};

static volatile rfc_CMD_PROP_RADIO_DIV_SETUP_t setup_cmd = {
	.commandNo = CMD_PROP_RADIO_DIV_SETUP,
	.condition.rule = COND_NEVER,
	.modulation = {
		.modType = 1,          /* 2-GFSK */
		.deviation = 680,      /* 680 * 250 Hz = 170 kHz */
		.deviationStepSz = 0,  /* 250 Hz */
	},
	.symbolRate = {
		.preScale = 15,
		.rateWord = 655360,    /* 1 Mbps, TRM 25.10.5.2 */
		.decimMode = 0,
	},
	.rxBw = 97,                    /* 1243.2 kHz; swept in phase 2 */
	/*
	 * ONE PREAMBLE BYTE, because ANT has one. nPreamBytes = 1 means one
	 * BYTE; 0 would mean one BIT. preamMode 2 - "send same first bit in
	 * preamble and sync word" - reproduces the nRF's rule exactly, which
	 * is the whole reason ANT's preamble runs continuously into the first
	 * address bit. SmartRF's own settings use four preamble bytes and
	 * preamMode 0, which is right for their PHYs and wrong for this one.
	 *
	 * On transmit this is the first time this project CHOOSES the preamble
	 * byte rather than inheriting it: docs/ant-radio-link.md records that
	 * the byte itself has never been directly observed, because the nRF
	 * derives it from the first address bit and does not report what it
	 * locked onto.
	 */
	.preamConf = {
		.nPreamBytes = 1,
		.preamMode = 2,
	},
	.formatConf = {
		.nSwBits = SPIKE_SYNC_BYTES * 8u,   /* 24 */
		.bBitReversal = 0,
		.bMsbFirst = 1,                     /* see the note above */
		.fecMode = 0,                       /* uncoded */
		.whitenMode = 0,                    /* ANT does not whiten */
	},
	.config = {
		.frontEndMode = 0,     /* differential; the LaunchXL's balun */
		.biasMode = 0,         /* internal bias */
		.analogCfgMode = 0,    /* write analog config: first after boot */
		.bNoFsPowerUp = 0,
	},
	.txPower = 0x7217,             /* SmartRF's 2.4 GHz value, tc901 */
	.pRegOverride = phy_overrides,
	.centerFreq = ANT_FREQ_MHZ,
	.intFreq = 0x0800,             /* SmartRF's 2.4 GHz value, tc901 */
	.loDivider = 0,                /* 0 = 2.4 GHz band, not a divider */
};

static volatile rfc_CMD_FS_t fs_cmd = {
	.commandNo = CMD_FS,
	.condition.rule = COND_NEVER,
	.frequency = ANT_FREQ_MHZ,
	.fractFreq = 0,
	.synthConf.bTxMode = 0,
	.synthConf.refFreq = 0,
};

/* â”€â”€ Receive â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 *
 * bUseCrc is 0 and maxPktLen counts the CRC bytes, so the two CRC bytes arrive
 * as ordinary payload and this program verifies them itself. Deliberate, and
 * the plan's instruction: get bytes on the ground before optimising. It also
 * means a frame that fails its CRC is still PRINTED, which is the difference
 * between "the PHY is wrong" and "the PHY is right and one bit flipped" - and
 * on a first bring-up those need completely different next moves.
 */
#define RX_ENTRY_BYTES   (SPIKE_BODY_BYTES + SPIKE_CRC_BYTES)
#define RX_APPEND_BYTES  (1u /* RSSI */ + 4u /* RAT timestamp */ + 1u /* status */)
#define RX_BUF_BYTES     (RX_ENTRY_BYTES + RX_APPEND_BYTES)
#define RX_ENTRIES       4

static uint8_t rx_data[RX_ENTRIES][sizeof(rfc_dataEntryGeneral_t) + RX_BUF_BYTES]
	__aligned(4);
static dataQueue_t rx_queue;
static rfc_propRxOutput_t rx_output;

static volatile rfc_CMD_PROP_RX_ADV_t rx_cmd = {
	.commandNo = CMD_PROP_RX_ADV,
	.condition.rule = COND_NEVER,
	.pktConf = {
		.bFsOff = 0,
		.bRepeatOk = 1,     /* stay in sync search after a good packet */
		.bRepeatNok = 1,    /* and after a bad one */
		.bUseCrc = 0,       /* software CRC - see above */
		.bCrcIncSw = 0,
		.bCrcIncHdr = 0,
		.endType = 0,
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
	/* syncWord0 is filled in at run time; the nSwBits least significant
	 * bits are the ones used.
	 *
	 * syncWord1 IS THE WHOLE OF caps.max_filters ON THIS PART. Two, where
	 * the nRF has eight - which is not a detail, it is a product
	 * regression on discovery time that radiant_search.c's policy has to
	 * absorb. See the measurement summary at the end.
	 */
	.syncWord0 = 0,
	.syncWord1 = 0,
	.maxPktLen = RX_ENTRY_BYTES,
	.hdrConf = {
		.numHdrBits = 0,    /* fixed length - ANT has no length field */
		.lenPos = 0,
		.numLenBits = 0,
	},
	.startTrigger.triggerType = TRIG_NOW,
	.endTrigger.triggerType = TRIG_NEVER,
	.pQueue = &rx_queue,
	.pOutput = (uint8_t *)&rx_output,
};

/* â”€â”€ Transmit â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€â”€
 *
 * The same 18 bytes as any other ANT frame. Which bytes are "address" and
 * which are "body" is a RECEIVER's split, not a property of the air, so
 * transmitting with a 3-byte sync word and a 12-byte body produces exactly the
 * frame a 5-byte matcher expects to see.
 */
static uint8_t tx_buf[SPIKE_BODY_BYTES + SPIKE_CRC_BYTES];

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
	.pktLen = SPIKE_BODY_BYTES + SPIKE_CRC_BYTES,
	.startTrigger.triggerType = TRIG_NOW,
	.syncWord = 0,
	.pPkt = tx_buf,
};

static RF_Object rf_object;
static RF_Handle rf_handle;

/*
 * RF patches. The RF core keeps a pointer to this, so it must not live on a
 * stack. RF_MODE_PROPRIETARY_2_4 with rf_patch_cpe_prop is what SmartRF's own
 * 2.4 GHz proprietary settings for this part select; RF_MODE_MULTIPLE with the
 * multi-protocol patch is the mode a later ANT-beside-BLE build would need,
 * and choosing it here would answer two questions at once.
 */
static RF_Mode rf_mode = {
#if defined(SPIKE_TI_PHY_MULTI_PATCH)
	.rfMode = RF_MODE_MULTIPLE,
	.cpePatchFxn = &rf_patch_cpe_multi_protocol,
#else
	.rfMode = RF_MODE_PROPRIETARY_2_4,
	.cpePatchFxn = &rf_patch_cpe_prop,
#endif
};

static uint32_t sync_word_for(uint16_t devnum)
{
	/* [A6][C5][devnum_lo], MSByte first, in the low nSwBits bits. */
	return ((uint32_t)ANT_NET_HI << 16) |
	       ((uint32_t)ANT_NET_LO << 8) |
	       (uint32_t)(devnum & 0xFFu);
}

static void rx_queue_init(void)
{
	memset(rx_data, 0, sizeof(rx_data));

	for (int i = 0; i < RX_ENTRIES; i++) {
		rfc_dataEntryGeneral_t *e = (rfc_dataEntryGeneral_t *)rx_data[i];

		e->pNextEntry = (i + 1 < RX_ENTRIES) ? rx_data[i + 1] : rx_data[0];
		e->status = DATA_ENTRY_PENDING;
		e->config.type = DATA_ENTRY_TYPE_GEN;
		e->config.lenSz = 0;      /* fixed length, no length prefix */
		e->length = RX_BUF_BYTES;
	}

	rx_queue.pCurrEntry = rx_data[0];
	rx_queue.pLastEntry = NULL;
}

static void print_hex(const char *label, const uint8_t *p, size_t n)
{
	printk("%s", label);
	for (size_t i = 0; i < n; i++) {
		printk(" %02X", p[i]);
	}
	printk("\n");
}

/*
 * One received entry. Reassembles the 15 CRC-covered bytes - the sync word is
 * not in the buffer, because the matcher consumed it, so it is put back from
 * what we asked to match - and checks the CRC in software.
 *
 * Returns true if the CRC passed.
 */
static bool report_frame(const uint8_t *body, uint8_t rssi_raw, uint32_t rat)
{
	uint8_t covered[SPIKE_CRC_COVER];
	uint16_t crc_calc, crc_rx;

	covered[0] = ANT_NET_HI;
	covered[1] = ANT_NET_LO;
	covered[2] = (uint8_t)(SPIKE_DEVNUM & 0xFFu);
	memcpy(&covered[3], body, SPIKE_BODY_BYTES);

	crc_calc = crc_ccitt_false(covered, sizeof(covered));
	crc_rx = (uint16_t)((body[SPIKE_BODY_BYTES] << 8) |
			    body[SPIKE_BODY_BYTES + 1]);

	print_hex("    raw18 55", covered, sizeof(covered));
	printk("          crc rx=0x%04X calc=0x%04X %s  rssi=%d dBm  rat=%u\n",
	       crc_rx, crc_calc, crc_rx == crc_calc ? "OK" : "FAIL",
	       (int8_t)rssi_raw, rat);

	if (crc_rx != crc_calc) {
		return false;
	}

	/*
	 * The channel ID, read out of RAM rather than assumed. This is what
	 * makes a pass non-circular: a sync-word match proves the first three
	 * bytes were on the air but cannot print the rest.
	 */
	printk("          channel id #%u / 0x%02X / %u   control=0x%02X\n",
	       (unsigned)(((uint16_t)body[0] << 8) | (SPIKE_DEVNUM & 0xFFu)),
	       body[1], body[2], body[3]);

	return true;
}

/* Drain whatever the queue holds. Returns entries seen; *good counts CRC
 * passes.
 */
static uint32_t drain_rx(uint32_t *good)
{
	uint32_t seen = 0;

	for (;;) {
		rfc_dataEntryGeneral_t *e =
			(rfc_dataEntryGeneral_t *)rx_queue.pCurrEntry;

		if (e == NULL || e->status != DATA_ENTRY_FINISHED) {
			break;
		}

		const uint8_t *p = (const uint8_t *)(&e->data);
		uint32_t rat;

		memcpy(&rat, &p[RX_ENTRY_BYTES + 1], sizeof(rat));

		seen++;
		if (report_frame(p, p[RX_ENTRY_BYTES], rat)) {
			(*good)++;
		}

		e->status = DATA_ENTRY_PENDING;
		rx_queue.pCurrEntry = e->pNextEntry;
	}

	return seen;
}

/*
 * One sweep point: open the RF core with a given RX bandwidth code and
 * anti-aliasing filter setting, listen, and report.
 *
 * OPENING AND CLOSING THE CORE PER POINT IS THE POINT. The setup command is
 * handed to RF_open() and is not a thing that can be changed underneath a live
 * handle, so a bandwidth sweep is a power-cycle sweep - which happens to be
 * exactly the experiment measurement 2 needs, so the RAT is read either side
 * of every one of them.
 */
struct sweep_result {
	uint8_t rx_bw;
	uint8_t aa_filter;
	uint16_t deviation;   /* units of 250 Hz */
	uint16_t agc_ref;     /* HW_REG_OVERRIDE(0x609C, ...); TI's default is 0x2E */
	bool     windowed;    /* arm per-window like the backend, not continuously */
	/*
	 * Rewrite syncWord0/syncWord1 between windows the way the search sweep
	 * does, alternating the real address with a decoy. Half the windows can
	 * then hear the transmitter and half cannot, so a healthy result is
	 * about half the windowed score - and a zero says the REWRITE is the
	 * defect rather than the windowing.
	 */
	bool     sw_cycle;
	uint8_t  pre_bytes;   /* setup_cmd.preamConf.nPreamBytes */
	uint8_t  pre_mode;    /* setup_cmd.preamConf.preamMode */
	/*
	 * What syncWord0 differs from the real address by, in the live windows.
	 *
	 * 1 is what radiant_search's sweep actually produces: it pairs devnum_lo
	 * 2n with 2n+1, so the two words the hardware is asked to match
	 * simultaneously differ in exactly one bit, every set, all 128 of them.
	 * Whether a 24-bit correlator can hold two words that close is not a
	 * question the search module could have known to ask.
	 */
	uint32_t decoy_xor;
	uint32_t windows;
	uint32_t windows_live;   /* windows whose sync word could match */
	uint32_t expect;      /* frames the transmitter SENT during this point */
	uint32_t seen;
	uint32_t good;
	uint32_t nRxOk;
	uint32_t nRxNok;
	bool opened;
};

/* The denominator, which the first version of this sweep didn't have.
 * ant_sim.py transmits at 8182/32768 s = 4.0049 Hz, so listen_ms offers a
 * KNOWN frame count - the difference between "caught more than that
 * setting" (a room statement) and "caught 45% of what was sent" (a radio
 * statement). P1's gate lacked this: its denominator was frames already
 * detected, so a PHY silently dropping half of everything scored the same
 * as a perfect one. Every number below is a fraction of frames TRANSMITTED. */
#define SIM_HZ_NUM   8182u
#define SIM_HZ_DEN   32768u

static uint32_t frames_expected(uint32_t listen_ms)
{
	/* listen_ms/1000 seconds at SIM_HZ_DEN/SIM_HZ_NUM Hz, rounded. */
	uint64_t n = (uint64_t)listen_ms * SIM_HZ_DEN;
	uint64_t d = (uint64_t)SIM_HZ_NUM * 1000u;

	return (uint32_t)((n + d / 2u) / d);
}

static void sweep_point(struct sweep_result *r, uint32_t listen_ms)
{
	RF_Params rf_params;

	printk("\n  -- rxBw=%u aa=0x%X dev=%u agc=0x%02X pre=%u/%u --\n",
	       r->rx_bw, r->aa_filter, r->deviation, r->agc_ref,
	       r->pre_bytes, r->pre_mode);

	setup_cmd.rxBw = r->rx_bw;
	setup_cmd.modulation.deviation = r->deviation;
	setup_cmd.preamConf.nPreamBytes = r->pre_bytes;
	setup_cmd.preamConf.preamMode = r->pre_mode;
	phy_overrides[AA_FILTER_OVERRIDE_INDEX] =
		ADI_HALFREG_OVERRIDE(0, 61, 0xF, r->aa_filter);
	phy_overrides[AGC_REF_OVERRIDE_INDEX] =
		HW_REG_OVERRIDE(0x609C, r->agc_ref);
	r->expect = frames_expected(listen_ms);

	rx_queue_init();
	memset(&rx_output, 0, sizeof(rx_output));
	rx_cmd.syncWord0 = sync_word_for(SPIKE_DEVNUM);

	RF_Params_init(&rf_params);
	rf_handle = RF_open(&rf_object, &rf_mode,
			    (RF_RadioSetup *)&setup_cmd, &rf_params);
	if (rf_handle == NULL) {
		printk("    RF_open FAILED\n");
		r->opened = false;
		return;
	}
	r->opened = true;
	printk("    setup status = 0x%04X %s\n", setup_cmd.status,
	       setup_cmd.status == PROP_DONE_OK ? "(PROP_DONE_OK)" : "");

	RF_EventMask ev = RF_runCmd(rf_handle, (RF_Op *)&fs_cmd,
				    RF_PriorityNormal, NULL, 0);

	if (fs_cmd.status != DONE_OK) {
		printk("    CMD_FS refused %u MHz: status 0x%04X, ev 0x%08llX\n",
		       ANT_FREQ_MHZ, fs_cmd.status, (unsigned long long)ev);
		RF_close(rf_handle);
		rf_handle = NULL;
		return;
	}

	if (r->windowed) {
		/* Windowed receive, the way the backend actually runs.
		 * Everything measured so far left one CMD_PROP_RX_ADV running
		 * for the whole point; the backend never does that -
		 * radiant_sched arms a fresh command per window, 260 ms
		 * receive in every 282 ms, since the search sweep must change
		 * sync words between windows. Last untested difference
		 * between a spike hearing 96% and a backend hearing almost
		 * nothing: same PHY, same sync word, same room, one variable. */
		const uint32_t win_us = 260190u;   /* what the sched asks for */
		const uint32_t period_us = 281949u;
		uint32_t t = RF_getCurrentTime() + 4u * 2000u;
		int64_t deadline = k_uptime_get() + listen_ms;

		while (k_uptime_get() < deadline) {
			bool live = true;

			if (r->sw_cycle) {
				/* Alternate the real pair with a decoy pair, so
				 * the sync words are REWRITTEN every window the
				 * way radiant_search's sweep rewrites them. */
				live = ((r->windows & 1u) == 0u);
				if (live) {
					rx_cmd.syncWord0 =
						sync_word_for(SPIKE_DEVNUM) ^
						r->decoy_xor;
					rx_cmd.syncWord1 =
						sync_word_for(SPIKE_DEVNUM);
				} else {
					rx_cmd.syncWord0 = 0x00A6C500u;
					rx_cmd.syncWord1 = 0x00A6C501u;
				}
			}
			if (live) {
				r->windows_live++;
			}

			rx_cmd.startTrigger.triggerType = TRIG_ABSTIME;
			rx_cmd.startTrigger.pastTrig = 1;
			rx_cmd.startTime = t;
			rx_cmd.endTrigger.triggerType = TRIG_ABSTIME;
			rx_cmd.endTrigger.pastTrig = 1;
			rx_cmd.endTime = t + 4u * win_us;

			RF_EventMask we = RF_runCmd(rf_handle, (RF_Op *)&rx_cmd,
						    RF_PriorityNormal, NULL, 0);
			(void)we;
			r->seen += drain_rx(&r->good);
			r->windows++;
			t += 4u * period_us;
		}

		/* Put the command back the way the continuous points want it. */
		rx_cmd.startTrigger.triggerType = TRIG_NOW;
		rx_cmd.endTrigger.triggerType = TRIG_NEVER;
		rx_cmd.syncWord0 = sync_word_for(SPIKE_DEVNUM);
		rx_cmd.syncWord1 = 0;
	} else {
		RF_CmdHandle h = RF_postCmd(rf_handle, (RF_Op *)&rx_cmd,
					    RF_PriorityNormal, NULL,
					    RF_EventRxEntryDone);

		if (h < 0) {
			printk("    RF_postCmd(CMD_PROP_RX_ADV) refused: %d\n", h);
			RF_close(rf_handle);
			rf_handle = NULL;
			return;
		}

		int64_t deadline = k_uptime_get() + listen_ms;

		while (k_uptime_get() < deadline) {
			r->seen += drain_rx(&r->good);
			k_msleep(5);
		}
		r->seen += drain_rx(&r->good);

		RF_flushCmd(rf_handle, h, 0);
	}

	r->nRxOk = rx_output.nRxOk;
	r->nRxNok = rx_output.nRxNok;

	printk("    entries=%u crcOk=%u   nRxOk=%u nRxNok=%u nRxIgnored=%u "
	       "nRxBufFull=%u lastRssi=%d\n",
	       r->seen, r->good, rx_output.nRxOk, rx_output.nRxNok,
	       rx_output.nRxIgnored, rx_output.nRxBufFull, rx_output.lastRssi);

	RF_close(rf_handle);
	rf_handle = NULL;
}

int main(void)
{
	printk("\n\n=== spike ti_phy - can a CC26x2 hear ANT+? ===\n");
	printk("target: %u MHz (RF index %u), 1 Mbps 2-GFSK, ~170 kHz deviation\n",
	       ANT_FREQ_MHZ, ANT_RF_INDEX);
	printk("ground truth: #%u / 0x%02X / %u\n",
	       SPIKE_DEVNUM, SPIKE_DTYPE, SPIKE_TTYPE);

	printk("\n-- phase 0: CRC self-test, before the radio is touched --\n");
	if (!crc_self_test()) {
		printk("\nSTOP. The CRC implementation is wrong. Nothing this "
		       "program prints about a received frame would mean "
		       "anything.\n");
		return 0;
	}

	printk("\n-- phase 1: the PHY, as programmed --\n");
	printk("  cmd=CMD_PROP_RADIO_DIV_SETUP centerFreq=%u loDivider=%u "
	       "intFreq=0x%04X\n",
	       setup_cmd.centerFreq, setup_cmd.loDivider, setup_cmd.intFreq);
	printk("  modType=%u deviation=%u (x250 Hz = %u kHz)\n",
	       setup_cmd.modulation.modType, setup_cmd.modulation.deviation,
	       (setup_cmd.modulation.deviation * 250u) / 1000u);
	printk("  preScale=%u rateWord=%u (= %u kbps)\n",
	       setup_cmd.symbolRate.preScale, setup_cmd.symbolRate.rateWord,
	       (unsigned)(((uint64_t)setup_cmd.symbolRate.rateWord * 24000000ULL) /
			  ((uint64_t)setup_cmd.symbolRate.preScale * 1048576ULL) / 1000ULL));
	printk("  nSwBits=%u msbFirst=%u whiten=%u preamBytes=%u preamMode=%u\n",
	       setup_cmd.formatConf.nSwBits, setup_cmd.formatConf.bMsbFirst,
	       setup_cmd.formatConf.whitenMode,
	       setup_cmd.preamConf.nPreamBytes, setup_cmd.preamConf.preamMode);
	printk("  sync word %u bits = 0x%06X  [A6 C5 %02X]\n",
	       SPIKE_SYNC_BYTES * 8u, sync_word_for(SPIKE_DEVNUM),
	       (unsigned)(SPIKE_DEVNUM & 0xFFu));

	/*
	 * Phase 2. A SWEEP, not a single attempt, and the reason is the same
	 * one that made Spike A sweep eight address permutations: the two
	 * fields most likely to be wrong here fail identically to a dead
	 * antenna, so trying one value and hearing nothing proves nothing.
	 */
	printk("\n-- phase 2: receive a real ANT+ frame (sweeping rxBw) --\n");
	printk("  rxBw codes: 96=1092.5 kHz  97=1243.2  98=1567.2 (ANT wants ~1340)\n");

	/*
	 * Sweep history, compressed: each round changes one axis at a time
	 * against a known frame count (percentage of frames the simulator
	 * actually sent).
	 *
	 * R1 (deviation, untested before): 680 units = 170 kHz, mod index
	 * 0.34 at 1 Mbps - too narrow a correlator window degrades sync
	 * detection before it costs a bit error (frames missed wholesale,
	 * nRxNok zero on locked frames).
	 * R2 (AGC reference level): tc901's AGC ref (0x22, tuned for 530 kHz)
	 * was the real defect, costing half of every transmission; TI's
	 * default 0x2E scored 87-90% regardless of deviation, retiring the
	 * modulation-index theory.
	 * R3 (confirm on bigger denominator): rxBw 98 / aa 0x5 / deviation
	 * 1000 / agc 0x34 hit 40/40. Deviation 1000 = 250 kHz = mod index 0.5,
	 * and Carson's rule then wants 1500 kHz = code 98 - the physics
	 * agrees; earlier rounds' narrow-filter picks were compensating for
	 * wrong deviation.
	 * R4 (continuous vs windowed): the backend runs this same PHY and
	 * hears almost nothing (0/5 armed windows over 150 s). Only
	 * difference left: the spike listens continuously, the backend arms
	 * a fresh 260 ms command every 282 ms - so run each PHY both ways.
	 * R5 (sync-word rewrite): continuous scored 93-98%, windowed 85% -
	 * windowing alone can't explain zero. The backend also rewrites
	 * syncWord0/1 between windows (128 pairs); "cyc" points below
	 * alternate the real pair with a decoy, real address in syncWord1
	 * (the slot the backend was using). Half-live windows should score
	 * ~half of 85%; a zero means the rewrite itself is the defect.
	 * R6 (preamble, last correlator knob): the backend now tracks
	 * end-to-end but loses 8.3% vs a 1.5% gate (nRF loses ~0.4%) - the
	 * PHY's own detection rate, not windowing. ANT sends one preamble
	 * byte; preamMode 2 reproduces its rule on transmit but neither field
	 * had been swept on receive. More preamble than is on air may never
	 * lock, so the baseline is repeated first and last.
	 */
	static struct sweep_result sweep[] = {
		{ .rx_bw = 98, .aa_filter = 0x5, .deviation = 1000, .agc_ref = 0x34,
		  .pre_bytes = 1, .pre_mode = 2 },
		{ .rx_bw = 98, .aa_filter = 0x5, .deviation = 1000, .agc_ref = 0x34,
		  .pre_bytes = 1, .pre_mode = 0 },
		{ .rx_bw = 98, .aa_filter = 0x5, .deviation = 1000, .agc_ref = 0x34,
		  .pre_bytes = 0, .pre_mode = 2 },
		{ .rx_bw = 98, .aa_filter = 0x5, .deviation = 1000, .agc_ref = 0x34,
		  .pre_bytes = 2, .pre_mode = 2 },
		{ .rx_bw = 98, .aa_filter = 0x5, .deviation = 1000, .agc_ref = 0x34,
		  .pre_bytes = 4, .pre_mode = 0 },
		{ .rx_bw = 98, .aa_filter = 0x5, .deviation = 1000, .agc_ref = 0x34,
		  .pre_bytes = 1, .pre_mode = 2 },
	};

	uint32_t rat_before_first = 0;
	uint32_t rat_after_last = 0;

	for (size_t i = 0; i < ARRAY_SIZE(sweep); i++) {
		sweep_point(&sweep[i], 15000);
		if (i == 0 && sweep[i].opened) {
			/* Read after the first close: the question measurement
			 * 2 asks is whether the RAT keeps running with the RF
			 * core powered down.
			 */
			rat_before_first = RF_getCurrentTime();
		}
	}
	rat_after_last = RF_getCurrentTime();

	printk("\n  sweep summary - crcOk as a PERCENTAGE OF FRAMES SENT\n");
	printk("  rxBw  aa   dev  agc  mode  wins   sent  crcOk   %%   nRxNok\n");
	uint32_t total_good = 0;
	int best = -1;

	for (size_t i = 0; i < ARRAY_SIZE(sweep); i++) {
		uint32_t pct = sweep[i].expect ?
			(sweep[i].good * 100u) / sweep[i].expect : 0u;

		printk("  %4u  0x%X  %4u  0x%02X  %-4s%04x %4u/%-4u  %5u  %5u  %3u%%  %6u\n",
		       sweep[i].rx_bw, sweep[i].aa_filter, sweep[i].deviation,
		       sweep[i].agc_ref,
		       sweep[i].sw_cycle ? "cyc" :
			       (sweep[i].windowed ? "win" : "cont"),
		       sweep[i].windows_live, sweep[i].windows,
		       sweep[i].expect, sweep[i].good, pct, sweep[i].nRxNok);
		total_good += sweep[i].good;
		if (best < 0 || sweep[i].good > sweep[best].good) {
			best = (int)i;
		}
	}

	if (total_good == 0) {
		printk("\nNOTHING HEARD, ON ANY SETTING. Before concluding the "
		       "RF core cannot do ANT's PHY, check in this order - "
		       "they all fail identically:\n"
		       "  1. Is the transmitter actually transmitting? "
		       "tools/ant_scan.py on a third board.\n"
		       "  2. bMsbFirst. Backwards bit order is silence, not a "
		       "weak link. Try 0.\n"
		       "  3. preamConf: one byte and preamMode 2 is ANT's rule, "
		       "but the demodulator may want more preamble to lock at "
		       "1 Mbps. Try nPreamBytes 2-4.\n"
		       "  4. Widen the sweep: rxBw 95 and 99 are outside it.\n"
		       "  5. The MCE/RFE. SmartRF's 2.4 GHz prop settings use "
		       "ROM for both; the BLE 1M setting overrides the RFE to "
		       "RAM. If the demodulator simply cannot track 1 Mbps in "
		       "prop mode, that is the go/no-go answer and the port "
		       "stops.\n");
	} else {
		uint32_t bpct = sweep[best].expect ?
			(sweep[best].good * 100u) / sweep[best].expect : 0u;

		printk("\n  BEST: rxBw=%u aa=0x%X dev=%u agc=0x%02X - %u of %u "
		       "frames sent = %u%%\n",
		       sweep[best].rx_bw, sweep[best].aa_filter,
		       sweep[best].deviation, sweep[best].agc_ref,
		       sweep[best].good, sweep[best].expect, bpct);
		/*
		 * AND THE BAR IS THE PERCENTAGE, NOT THE FACT THAT SOMETHING
		 * ARRIVED. The link this backend has to match loses about 0.4%
		 * on this bench, so anything under ~99% here is a PHY that is
		 * not finished, however many frames it decoded perfectly.
		 */
		if (bpct >= 95u) {
			printk("  THE PHY QUESTION IS ANSWERED YES.\n");
		} else {
			printk("  NOT GOOD ENOUGH. Frames are being MISSED, not\n"
			       "  corrupted - check nRxNok above: if it is zero,\n"
			       "  every frame that locked was perfect and the\n"
			       "  problem is the SYNC SEARCH, not the demodulator.\n"
			       "  Next knobs, in order: deviation (widen the sweep\n"
			       "  past 1000), preamble handling (nPreamBytes and\n"
			       "  preamMode on the RX side), then the RFE - the BLE\n"
			       "  1M setting overrides it to RAM and prop mode does\n"
			       "  not.\n");
		}
	}

	printk("\n-- phase 3: the RAT --\n");
	{
		uint32_t r0 = RF_getCurrentTime();
		int64_t k0 = k_uptime_get();

		k_msleep(1000);

		uint32_t r1 = RF_getCurrentTime();
		int64_t k1 = k_uptime_get();

		uint32_t dr = r1 - r0;
		int64_t dk = k1 - k0;

		printk("  RAT ticks in %lld ms: %u -> %u ticks/ms "
		       "(expect 4000, i.e. 4 MHz)\n",
		       (long long)dk, dr,
		       dk ? (unsigned)(dr / (uint32_t)dk) : 0u);
		printk("  across %u RF_close/RF_open cycles: %u -> %u, "
		       "delta %u ticks - %s\n",
		       (unsigned)ARRAY_SIZE(sweep) - 1u, rat_before_first,
		       rat_after_last, rat_after_last - rat_before_first,
		       rat_after_last != rat_before_first
			       ? "the RAT kept running"
			       : "THE RAT STOPPED - the 64-bit fold cannot be "
				 "a simple extension");
		printk("  32-bit RAT at 4 MHz wraps every %u s = %u min; the "
		       "backend owes the core a 64-bit fold\n",
		       (unsigned)(0x100000000ULL / 4000000ULL),
		       (unsigned)(0x100000000ULL / 4000000ULL / 60ULL));
	}

	if (total_good == 0) {
		printk("\n=== spike ti_phy: NO-GO on receive. Not transmitting; "
		       "a transmitter built on a PHY that cannot receive would "
		       "prove nothing. ===\n");
		return 0;
	}

	printk("\n-- phase 4: transmit a frame the Feather can hear --\n");
	{
		uint8_t covered[SPIKE_CRC_COVER];
		uint16_t crc;
		RF_Params rf_params;

		/* A plain broadcast: control byte 0x0A - slot-opening, bit 7
		 * clear. See docs/ant-radio-link.md "Fact one".
		 */
		uint8_t body[SPIKE_BODY_BYTES] = {
			(uint8_t)(SPIKE_DEVNUM >> 8),
			SPIKE_DTYPE,
			SPIKE_TTYPE,
			0x0A,
			0, 1, 2, 3, 4, 5, 6, 7
		};

		covered[0] = ANT_NET_HI;
		covered[1] = ANT_NET_LO;
		covered[2] = (uint8_t)(SPIKE_DEVNUM & 0xFFu);
		memcpy(&covered[3], body, SPIKE_BODY_BYTES);
		crc = crc_ccitt_false(covered, sizeof(covered));

		memcpy(tx_buf, body, SPIKE_BODY_BYTES);
		tx_buf[SPIKE_BODY_BYTES] = (uint8_t)(crc >> 8);
		tx_buf[SPIKE_BODY_BYTES + 1] = (uint8_t)crc;

		tx_cmd.syncWord = sync_word_for(SPIKE_DEVNUM);

		setup_cmd.rxBw = sweep[best].rx_bw;
		phy_overrides[AA_FILTER_OVERRIDE_INDEX] =
			ADI_HALFREG_OVERRIDE(0, 61, 0xF, sweep[best].aa_filter);

		RF_Params_init(&rf_params);
		rf_handle = RF_open(&rf_object, &rf_mode,
				    (RF_RadioSetup *)&setup_cmd, &rf_params);
		if (rf_handle == NULL) {
			printk("  RF_open FAILED on the transmit pass\n");
			return 0;
		}

		fs_cmd.synthConf.bTxMode = 1;
		RF_runCmd(rf_handle, (RF_Op *)&fs_cmd, RF_PriorityNormal,
			  NULL, 0);

		print_hex("  sending", tx_buf, sizeof(tx_buf));

		for (int i = 0; i < 40; i++) {
			RF_EventMask ev = RF_runCmd(rf_handle,
						    (RF_Op *)&tx_cmd,
						    RF_PriorityNormal, NULL, 0);

			if (i == 0) {
				printk("  first TX status = 0x%04X, ev = 0x%08llX\n",
				       tx_cmd.status,
				       (unsigned long long)ev);
			}
			k_msleep(250);
		}

		printk("  40 frames sent at 4 Hz. Point tools/ant_scan.py at "
		       "the Feather; it should report #%u / 0x%02X / %u.\n",
		       SPIKE_DEVNUM, SPIKE_DTYPE, SPIKE_TTYPE);

		RF_close(rf_handle);
	}

	printk("\n-- measurements for docs/backends.md --\n");
	printk("  max_filters        2   (CMD_PROP_RX_ADV has syncWord0 and "
	       "syncWord1 and nothing else)\n");
	printk("  addr_len_hw_max    4   (formatConf.nSwBits is 8..32, so a "
	       "5-byte ANT tracking address does NOT fit)\n");
	printk("  max_addr_groups    2   (each sync word is independent, so "
	       "this equals max_filters - unlike the nRF)\n");
	printk("  time_resolution_ns 250 (RAT is 4 MHz)\n");
	printk("  has_sync_timestamp yes (rxConf.bAppendTimestamp is a RAT "
	       "capture, not a software read)\n");
	printk("  NOT MEASURED HERE: ramp-up, rx_to_tx, min_arm_lead, and the "
	       "t_sync calibration constant. Those need the absolute-start-time "
	       "path (TRIG_ABSTIME) and the wired two-board trigger, which is "
	       "P2/P3 work rather than a go/no-go.\n");

	printk("\n=== spike ti_phy done ===\n");

	return 0;
}
