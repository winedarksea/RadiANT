/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Spike A - rx_raw: can a bare nRF RADIO hear a real ANT+ broadcast?
 *
 * Provenance: clean-room. Written from
 *   - docs/ant-radio-link.md (the frame layout, the CRC parameters, the
 *     register predictions, and the two facts this program exists to test),
 *   - the public Nordic MDK headers shipped with NCS
 *     (modules/hal/nordic/nrfx/bsp/stable/mdk/nrf54l15_types.h) and the
 *     nRF54L15/nRF52840 product specifications for register semantics,
 *   - Zephyr's public clock-control API for starting the HFXO.
 * Nothing here derives from sdk-ant, from libant.a, or from any adopter-gated
 * ANT+ device profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this program is for
 * ---------------------------------------------------------------------------
 * The whole RadiANT plan rests on one claim: the nRF RADIO can be configured to
 * put a byte-exact ANT frame on the air, and to take one off it, with no
 * software in the bit path. docs/ant-radio-link.md states that claim as a
 * register mapping and marks it a *prediction*. This program's only job is to
 * try to falsify it cheaply, before four months are spent assuming it.
 *
 * It does not assume the mapping is right. It sweeps the plausible readings of
 * the two documented details that are easy to get backwards - whether address
 * bytes must be bit-reversed on their way into BASE/PREFIX, and which end of
 * BASE goes out first - and reports which one, if any, produces CRC-valid
 * frames. A configuration that hears nothing is as much a result as one that
 * hears everything, which is why every phase prints its counters whether or not
 * it succeeded.
 *
 * ---------------------------------------------------------------------------
 * The five phases
 * ---------------------------------------------------------------------------
 *   A  Address-order sweep. Eight permutations of {bit-reversal} x {which end
 *      of BASE is first on air} x {prefix first or last}, all with a 5-byte
 *      address and a *static* 10-byte body. Static length is deliberate: it
 *      makes phase A independent of whether the length byte is a length byte
 *      at all, so one unknown is resolved at a time.
 *
 *   B  Length-field configuration, on whichever address order won phase A.
 *      L0 is the prediction - S0LEN=1, LFLEN=8, CRCINC=1, so the on-air 0x0A
 *      is parsed as "8 payload + 2 CRC". L1 is the documented fallback -
 *      PCNF0=0, STATLEN=10 - which is identical on air and cannot fail for a
 *      reason CRCINC could. If L0 works, CRCINC behaves and the mapping stands
 *      as written.
 *
 *   C  Search configuration. A 3-byte on-air address [A6 C5 devnum_lo] and a
 *      12-byte static body, which puts devnum_hi, device type and transmission
 *      type in RAM where they can be *read* rather than merely matched. This is
 *      what makes the pass criterion non-circular: a 5-byte address match
 *      proves those bytes were on the air, but phase C prints them.
 *
 *   D  Measurement. 60 s tracking, then 60 s search, counting CRC-valid frames
 *      and checking every decoded field against the ground truth that
 *      tools/ant_scan.py reported for this transmitter.
 *
 *   E  The preamble-as-address experiment from docs/ant-radio-link.md: BALEN=2
 *      with the on-air address [55 A6 C5], matching every ANT+ packet in one
 *      window with no sweep. It needs a different CRC setup - the preamble is
 *      *not* covered by ANT's CRC, so SKIPADDR=Skip with CRCINIT preloaded to
 *      the state after A6 C5 (0x233E). That constant is itself a prediction
 *      from the same document, so phase E tests two things at once.
 *
 * The whole sequence then repeats, so a capture started at any moment gets a
 * complete cycle, and two cycles are a reproducibility check for free.
 *
 * ---------------------------------------------------------------------------
 * Why it polls instead of using the RADIO interrupt
 * ---------------------------------------------------------------------------
 * A spike wants to be obviously correct rather than fast. The transmitter runs
 * at 4 Hz, the SHORTS chain (READY_START | END_START) keeps the receiver armed
 * with no software involvement between packets, and the only thing the CPU has
 * to do is notice EVENTS_END and copy the buffer before the next frame lands
 * 250 ms later. An interrupt would add a concurrency question and answer none.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/printk.h>

#include <nrfx.h>

#if defined(NRF54L_ERRATA_20_PRESENT)
#include <hal/nrf_power.h>
#endif

/* This program pokes RADIO registers directly. Anything else that owns the
 * radio - a Bluetooth controller, an MPSL timeslot session - would silently
 * reprogram them underneath it and the results would be noise. Loud beats
 * silent. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT), "Spike A owns the RADIO; CONFIG_BT must be off");

/* ---------------------------------------------------------------------------
 * Ground truth - what the transmitter on this bench actually is.
 *
 * These are not guesses. tools/ant_scan.py was run against a second board while
 * tools/ant_sim.py drove the Feather as an ANT+ master, and it reported
 * "#14871 - power meter". The transmission type comes from ant_sim.py's own
 * configuration line. Override at build time if the bench changes.
 * ---------------------------------------------------------------------------
 */
#ifndef SPIKE_DEVNUM
#define SPIKE_DEVNUM   14871u   /* 0x3A17 */
#endif
#ifndef SPIKE_DTYPE
#define SPIKE_DTYPE    0x0Bu    /* 11, bicycle power */
#endif
#ifndef SPIKE_TTYPE
#define SPIKE_TTYPE    0x05u
#endif

#define ANT_NET_HI     0xA6u    /* first network-address byte on air */
#define ANT_NET_LO     0xC5u
#define ANT_RF_INDEX   57u      /* 2457 MHz */
#define ANT_PREAMBLE   0x55u

#define DEVNUM_LO      ((uint8_t)(SPIKE_DEVNUM & 0xFFu))
#define DEVNUM_HI      ((uint8_t)((SPIKE_DEVNUM >> 8) & 0xFFu))

/* CRC state after CRC-16/CCITT-FALSE has consumed A6 C5, per
 * docs/ant-radio-link.md. Phase E is the first time it is used on hardware. */
#define CRC_AFTER_NET  0x233Eu

/* Phase durations. Long enough at 4 Hz to tell 0 from ~20 without doubt. */
#define SWEEP_MS       6000
#define LENCMP_MS      10000
#define SEARCH_MS      6000
#define MEASURE_MS     60000

#define RX_BUF_LEN     64

static uint8_t rx_buf[RX_BUF_LEN] __aligned(4);

/* ---------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------------
 */

/* Reverse the bits of one byte. docs/ant-radio-link.md predicts every address
 * byte needs this on the way into BASE/PREFIX, because the address is emitted
 * least-significant-bit first whatever PCNF1.ENDIAN says. Phase A is the test:
 * half the permutations apply it and half do not. */
static uint8_t rev8(uint8_t b)
{
	b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
	b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
	b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
	return b;
}

/* CRC-16/CCITT-FALSE: width 16, poly 0x1021, no reflection, no final XOR.
 * The initial value is a parameter because ANT's CRC chains - preloading the
 * state after A6 C5 is what phase E and the EFR32 notes in docs/backends.md
 * both depend on. */
static uint16_t crc16_ccitt_false(const uint8_t *p, size_t n, uint16_t init)
{
	uint16_t crc = init;

	for (size_t i = 0; i < n; i++) {
		crc ^= (uint16_t)p[i] << 8;
		for (int bit = 0; bit < 8; bit++) {
			crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u)
					      : (uint16_t)(crc << 1);
		}
	}
	return crc;
}

static void hex_dump(const uint8_t *p, size_t n)
{
	for (size_t i = 0; i < n; i++) {
		printk("%02X%s", p[i], (i + 1 == n) ? "" : " ");
	}
}

/* ---------------------------------------------------------------------------
 * One radio configuration under test
 * ---------------------------------------------------------------------------
 */
struct rx_cfg {
	const char *name;
	/* The on-air address, first byte first. This is the invariant the whole
	 * sweep is trying to produce; everything below is register arithmetic
	 * that may or may not produce it. */
	uint8_t addr[5];
	uint8_t addr_len;      /* 3 or 5; BALEN = addr_len - 1 */

	bool bitrev;           /* bit-reverse each address byte into the registers */
	/* Two independent questions about BASE0, which is why they are two flags:
	 *   base_reverse      - does the first base byte on air sit in the lowest
	 *                       used register byte, or the highest?
	 *   base_top_aligned  - when BALEN < 4, are the used bytes the low ones or
	 *                       the high ones? ("Truncated from the least
	 *                       significant byte" is the documented wording, and
	 *                       which end that is depends on the answer above.)
	 * With BALEN = 4 the second flag has no effect, so phase A only sweeps the
	 * first; the pair only separates once a 3-byte address is in play. */
	bool base_reverse;
	bool base_top_aligned;
	bool prefix_first;     /* hardware emits PREFIX before BASE, not after */

	bool use_lenfield;     /* PCNF0 = S0LEN 1 | LFLEN 8 | CRCINC 1, vs PCNF0 = 0 */
	uint8_t statlen;       /* used only when !use_lenfield */

	bool crc_skip_addr;    /* CRCCNF.SKIPADDR: Skip rather than Include */
	uint16_t crc_init;

	/* How to read the body back. Offsets into the RAM buffer, or 0xFF for
	 * "this field is in the matched address, not in RAM". */
	uint8_t off_devnum_lo;
	uint8_t off_devnum_hi;
	uint8_t off_dtype;
	uint8_t off_ttype;
	uint8_t off_len;
	uint8_t body_len;      /* bytes the radio will put in RAM */
};

/* Counters for one phase. */
struct rx_stats {
	uint32_t ends;         /* EVENTS_END - i.e. address matched and a body landed */
	uint32_t crc_ok;
	uint32_t crc_err;
	uint32_t id_ok;        /* CRC ok *and* every decoded field matched ground truth */
	uint32_t sw_crc_ok;    /* software CRC over the reconstructed frame agreed */
	uint32_t sw_zero_ok;   /* recomputing over message+CRC gave zero */
	int32_t  rssi_sum;
	int8_t   rssi_last;
};

/* ---------------------------------------------------------------------------
 * Register programming
 * ---------------------------------------------------------------------------
 */
static void radio_apply(const struct rx_cfg *c, uint32_t *out_base,
			uint32_t *out_prefix)
{
	const uint8_t balen = (uint8_t)(c->addr_len - 1u);
	uint8_t base[4];
	uint8_t prefix;
	uint32_t base_reg = 0;

	/* Split the on-air address into what the hardware calls base and prefix.
	 * Which end the prefix sits at is one of the things under test. */
	if (c->prefix_first) {
		prefix = c->addr[0];
		for (uint8_t i = 0; i < balen; i++) {
			base[i] = c->addr[i + 1u];
		}
	} else {
		prefix = c->addr[c->addr_len - 1u];
		for (uint8_t i = 0; i < balen; i++) {
			base[i] = c->addr[i];
		}
	}

	if (c->bitrev) {
		for (uint8_t i = 0; i < balen; i++) {
			base[i] = rev8(base[i]);
		}
		prefix = rev8(prefix);
	}

	/* Pack the base bytes. base[0] is the first of them on the air.
	 *
	 * The offset is where the used window of the register starts, and the
	 * index within it is either forwards or reversed. Writing it as one
	 * expression rather than four cases is the only way the BALEN=4 and
	 * BALEN=2 cases stay provably consistent with each other, which matters:
	 * the whole point of phase C is to carry phase A's answer over to a
	 * shorter address without quietly changing a second thing at the same
	 * time. */
	{
		const uint8_t offset = c->base_top_aligned ? (uint8_t)(4u - balen) : 0u;

		for (uint8_t i = 0; i < balen; i++) {
			uint8_t slot = (uint8_t)(offset +
				(c->base_reverse ? (uint8_t)(balen - 1u - i) : i));

			base_reg |= (uint32_t)base[i] << (8u * slot);
		}
	}

	NRF_RADIO->MODE = (RADIO_MODE_MODE_Nrf_1Mbit << RADIO_MODE_MODE_Pos);
	NRF_RADIO->FREQUENCY = ANT_RF_INDEX;

	NRF_RADIO->BASE0 = base_reg;
	NRF_RADIO->PREFIX0 = prefix;
	NRF_RADIO->TXADDRESS = 0;
	NRF_RADIO->RXADDRESSES = 1u;   /* logical address 0 only */

	if (c->use_lenfield) {
		NRF_RADIO->PCNF0 =
			(1u << RADIO_PCNF0_S0LEN_Pos) |
			(8u << RADIO_PCNF0_LFLEN_Pos) |
			(0u << RADIO_PCNF0_S1LEN_Pos) |
			(RADIO_PCNF0_PLEN_8bit << RADIO_PCNF0_PLEN_Pos) |
			(RADIO_PCNF0_CRCINC_Include << RADIO_PCNF0_CRCINC_Pos);
	} else {
		/* PLEN_8bit and every other field here are zero, so this really is
		 * the one-line fallback docs/ant-radio-link.md describes. */
		NRF_RADIO->PCNF0 = 0;
	}

	NRF_RADIO->PCNF1 =
		((uint32_t)(RX_BUF_LEN - 4) << RADIO_PCNF1_MAXLEN_Pos) |
		((uint32_t)(c->use_lenfield ? 0u : c->statlen) << RADIO_PCNF1_STATLEN_Pos) |
		((uint32_t)balen << RADIO_PCNF1_BALEN_Pos) |
		(RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
		(RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

	NRF_RADIO->CRCCNF =
		(RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
		((uint32_t)(c->crc_skip_addr ? RADIO_CRCCNF_SKIPADDR_Skip
					     : RADIO_CRCCNF_SKIPADDR_Include)
			<< RADIO_CRCCNF_SKIPADDR_Pos);
	/* 0x11021 carries the implicit x^16 term explicitly; the polynomial is
	 * the same 0x1021 the software CRC above uses. */
	NRF_RADIO->CRCPOLY = 0x11021u;
	NRF_RADIO->CRCINIT = c->crc_init;

	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)rx_buf;

	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
			    RADIO_SHORTS_END_START_Msk |
			    RADIO_SHORTS_ADDRESS_RSSISTART_Msk;

	if (out_base) {
		*out_base = base_reg;
	}
	if (out_prefix) {
		*out_prefix = prefix;
	}
}

static void radio_stop(void)
{
	NRF_RADIO->SHORTS = 0;
	NRF_RADIO->EVENTS_DISABLED = 0;
	NRF_RADIO->TASKS_STOP = 1;
	NRF_RADIO->TASKS_DISABLE = 1;
	while (NRF_RADIO->EVENTS_DISABLED == 0) {
		/* microseconds */
	}
	NRF_RADIO->EVENTS_DISABLED = 0;
	(void)NRF_RADIO->EVENTS_DISABLED;
}

static void radio_start_rx(void)
{
	NRF_RADIO->EVENTS_END = 0;
	NRF_RADIO->EVENTS_ADDRESS = 0;
	NRF_RADIO->EVENTS_CRCOK = 0;
	NRF_RADIO->EVENTS_CRCERROR = 0;
	(void)NRF_RADIO->EVENTS_END;
	NRF_RADIO->TASKS_RXEN = 1;
}

/* ---------------------------------------------------------------------------
 * One packet
 * ---------------------------------------------------------------------------
 */

/*
 * Rebuild the 18 bytes that were on the air.
 *
 * Only 10 to 13 of them are ever in RAM: the address bytes are consumed by the
 * hardware matcher and the CRC lands in RADIO->RXCRC. Saying so explicitly
 * matters, because "all 18 raw bytes" from a matched receive is necessarily a
 * reconstruction, and the only byte in it that is a genuine assumption is the
 * preamble - the demodulator does not report what it locked on to.
 */
static size_t rebuild_frame(const struct rx_cfg *c, const uint8_t *body,
			    uint16_t rxcrc, uint8_t *out)
{
	size_t n = 0;

	if (c->addr[0] != ANT_PREAMBLE) {
		out[n++] = ANT_PREAMBLE;   /* assumed, not observed */
	}
	for (uint8_t i = 0; i < c->addr_len; i++) {
		out[n++] = c->addr[i];
	}
	for (uint8_t i = 0; i < c->body_len; i++) {
		out[n++] = body[i];
	}
	out[n++] = (uint8_t)(rxcrc >> 8);
	out[n++] = (uint8_t)(rxcrc & 0xFFu);
	return n;
}

static void report_packet(const struct rx_cfg *c, const uint8_t *body,
			  bool crc_ok, uint32_t rxmatch, uint16_t rxcrc,
			  int8_t rssi, struct rx_stats *st, bool verbose)
{
	uint8_t frame[20];
	size_t flen = rebuild_frame(c, body, rxcrc, frame);
	/* frame[0] is the preamble; ANT's CRC starts at the network address and
	 * covers 15 bytes. */
	uint16_t sw = crc16_ccitt_false(&frame[1], 15, 0xFFFFu);
	uint16_t zero = crc16_ccitt_false(&frame[1], 17, 0xFFFFu);

	uint16_t devnum;
	uint8_t dlo, dhi, dtype, ttype, lenb;

	/* 0xFF means "not in RAM": the byte was consumed by the address matcher,
	 * so the only thing that can be said about it is that the match - and
	 * therefore the CRC over it - succeeded. Phases C and E do have these
	 * bytes in RAM, which is what makes their identity check real rather
	 * than circular. */
	dlo = (c->off_devnum_lo == 0xFFu) ? DEVNUM_LO : body[c->off_devnum_lo];
	dhi = (c->off_devnum_hi == 0xFFu) ? DEVNUM_HI : body[c->off_devnum_hi];
	devnum = (uint16_t)(dlo | ((uint16_t)dhi << 8));
	dtype = (c->off_dtype == 0xFFu) ? SPIKE_DTYPE : body[c->off_dtype];
	ttype = (c->off_ttype == 0xFFu) ? SPIKE_TTYPE : body[c->off_ttype];
	lenb = body[c->off_len];

	if (crc_ok) {
		st->crc_ok++;
		st->rssi_sum += rssi;
		st->rssi_last = rssi;
		if (sw == rxcrc) {
			st->sw_crc_ok++;
		}
		if (zero == 0) {
			st->sw_zero_ok++;
		}
		if (devnum == (uint16_t)SPIKE_DEVNUM &&
		    (dtype & 0x7Fu) == SPIKE_DTYPE && ttype == SPIKE_TTYPE) {
			st->id_ok++;
		}
	} else {
		st->crc_err++;
	}

	if (!verbose) {
		return;
	}

	printk("  PKT crc=%s rxmatch=%u rssi=%ddBm len=0x%02X dev=%u type=0x%02X trans=%u\n",
	       crc_ok ? "OK " : "ERR", (unsigned int)rxmatch, (int)rssi,
	       lenb, devnum, dtype, ttype);
	printk("      raw18 ");
	hex_dump(frame, flen);
	printk("\n      rxcrc=0x%04X sw_crc=0x%04X %s  recompute_over_17=0x%04X %s\n",
	       rxcrc, sw, (sw == rxcrc) ? "match" : "MISMATCH",
	       zero, (zero == 0) ? "zero" : "NONZERO");
}

/* ---------------------------------------------------------------------------
 * Run one configuration for a while
 * ---------------------------------------------------------------------------
 */
static void run_cfg(const struct rx_cfg *c, uint32_t ms, struct rx_stats *st,
		    unsigned int verbose_first)
{
	uint32_t base_reg = 0, prefix_reg = 0;
	int64_t end_at;
	unsigned int printed = 0;

	memset(st, 0, sizeof(*st));
	memset(rx_buf, 0, sizeof(rx_buf));

	radio_apply(c, &base_reg, &prefix_reg);

	printk("[cfg] %-26s addr=", c->name);
	hex_dump(c->addr, c->addr_len);
	printk(" BALEN=%u BASE0=0x%08X PREFIX0=0x%02X PCNF0=0x%08X PCNF1=0x%08X\n",
	       (unsigned int)(c->addr_len - 1u), base_reg,
	       (unsigned int)(prefix_reg & 0xFFu),
	       (unsigned int)NRF_RADIO->PCNF0, (unsigned int)NRF_RADIO->PCNF1);

	radio_start_rx();
	end_at = k_uptime_get() + ms;

	while (k_uptime_get() < end_at) {
		if (NRF_RADIO->EVENTS_END == 0) {
			continue;
		}
		NRF_RADIO->EVENTS_END = 0;
		(void)NRF_RADIO->EVENTS_END;

		{
			uint8_t body[16];
			bool crc_ok = (NRF_RADIO->CRCSTATUS &
				       RADIO_CRCSTATUS_CRCSTATUS_Msk) ==
				      RADIO_CRCSTATUS_CRCSTATUS_CRCOk;
			uint32_t rxmatch = NRF_RADIO->RXMATCH &
					   RADIO_RXMATCH_RXMATCH_Msk;
			uint16_t rxcrc = (uint16_t)(NRF_RADIO->RXCRC & 0xFFFFu);
			int8_t rssi = (int8_t)(-(int32_t)(NRF_RADIO->RSSISAMPLE & 0x7Fu));
			size_t n = c->body_len;

			if (n > sizeof(body)) {
				n = sizeof(body);
			}
			memcpy(body, rx_buf, n);

			st->ends++;
			report_packet(c, body, crc_ok, rxmatch, rxcrc, rssi, st,
				      printed < verbose_first);
			if (printed < verbose_first) {
				printed++;
			}
		}
	}

	radio_stop();

	printk("[res] %-26s end=%u crc_ok=%u crc_err=%u id_ok=%u sw_crc_ok=%u zero_ok=%u",
	       c->name, st->ends, st->crc_ok, st->crc_err, st->id_ok,
	       st->sw_crc_ok, st->sw_zero_ok);
	if (st->crc_ok) {
		printk(" rssi_avg=%ddBm", (int)(st->rssi_sum / (int32_t)st->crc_ok));
	}
	printk("\n");
}

/* ---------------------------------------------------------------------------
 * Configuration templates
 * ---------------------------------------------------------------------------
 */

/* Tracking: 5-byte on-air address A6 C5 devnum_lo devnum_hi device_type, body
 * [trans_type][0x0A][d0..d7]. Nothing but the trans type, the length byte and
 * the payload reaches RAM, so the identity check here is the address match. */
static struct rx_cfg tracking_cfg(const char *name, bool bitrev,
				  bool base_reverse, bool prefix_first,
				  bool use_lenfield)
{
	struct rx_cfg c = {
		.name = name,
		.addr = { ANT_NET_HI, ANT_NET_LO, DEVNUM_LO, DEVNUM_HI, SPIKE_DTYPE },
		.addr_len = 5,
		.bitrev = bitrev,
		.base_reverse = base_reverse,
		.base_top_aligned = false,   /* no effect at BALEN = 4 */
		.prefix_first = prefix_first,
		.use_lenfield = use_lenfield,
		.statlen = 10,
		.crc_skip_addr = false,
		.crc_init = 0xFFFFu,
		.off_devnum_lo = 0xFFu,
		.off_devnum_hi = 0xFFu,
		.off_dtype = 0xFFu,
		.off_ttype = 0,
		.off_len = 1,
		.body_len = 10,
	};
	return c;
}

/* Search: 3-byte on-air address A6 C5 devnum_lo, 12-byte static body
 * [devnum_hi][device_type][trans_type][0x0A][d0..d7]. This is the one that can
 * print the channel ID rather than assume it. */
static struct rx_cfg search_cfg(const char *name, bool bitrev,
				bool base_reverse, bool base_top_aligned)
{
	struct rx_cfg c = {
		.name = name,
		.addr = { ANT_NET_HI, ANT_NET_LO, DEVNUM_LO, 0, 0 },
		.addr_len = 3,
		.bitrev = bitrev,
		.base_reverse = base_reverse,
		.base_top_aligned = base_top_aligned,
		.prefix_first = false,   /* settled by phase A */
		.use_lenfield = false,
		.statlen = 12,
		.crc_skip_addr = false,
		.crc_init = 0xFFFFu,
		.off_devnum_lo = 0xFFu,   /* the prefix byte; in a real search it
					   * comes back from RADIO->RXMATCH */
		.off_devnum_hi = 0,
		.off_dtype = 1,
		.off_ttype = 2,
		.off_len = 3,
		.body_len = 12,
	};
	return c;
}

/* Preamble-as-address: 3-byte on-air address 55 A6 C5, 13-byte static body
 * [devnum_lo][devnum_hi][device_type][trans_type][0x0A][d0..d7].
 *
 * The CRC has to be set up differently and that is the interesting part. ANT's
 * CRC does not cover the preamble, but SKIPADDR=Include would make the hardware
 * cover all three matched bytes. So the address is skipped and CRCINIT is
 * preloaded with the register state after A6 C5. If frames come back CRC-valid,
 * the 0x233E constant in docs/ant-radio-link.md is confirmed on hardware. */
static struct rx_cfg preamble_cfg(const char *name, bool bitrev,
				  bool base_reverse, bool base_top_aligned)
{
	struct rx_cfg c = {
		.name = name,
		.addr = { ANT_PREAMBLE, ANT_NET_HI, ANT_NET_LO, 0, 0 },
		.addr_len = 3,
		.bitrev = bitrev,
		.base_reverse = base_reverse,
		.base_top_aligned = base_top_aligned,
		.prefix_first = false,
		.use_lenfield = false,
		.statlen = 13,
		.crc_skip_addr = true,
		.crc_init = CRC_AFTER_NET,
		.off_devnum_lo = 0,
		.off_devnum_hi = 1,
		.off_dtype = 2,
		.off_ttype = 3,
		.off_len = 4,
		.body_len = 13,
	};
	return c;
}

/* Phase E's frame does not start with an assumed preamble byte, so the generic
 * software check in report_packet() would be looking at the wrong 15 bytes.
 * rebuild_frame() handles that by not prepending a preamble when the address
 * already begins with one - which leaves the CRC window starting one byte
 * later, exactly where ANT's does. */

/* ---------------------------------------------------------------------------
 * Clocks
 * ---------------------------------------------------------------------------
 */
static int hfxo_start(void)
{
	const struct device *clk = DEVICE_DT_GET_ONE(nordic_nrf_clock);
	int err;

	if (!device_is_ready(clk)) {
		printk("FATAL: clock controller not ready\n");
		return -ENODEV;
	}

	/* Blocking on this driver. The RADIO needs the crystal, not the internal
	 * oscillator: a 1 Mbps GFSK link will not hold bit sync on HFINT. */
	err = clock_control_on(clk, CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (err < 0) {
		printk("FATAL: HFXO did not start (%d)\n", err);
		return err;
	}

#if defined(NRF54L_ERRATA_20_PRESENT)
	/* nRF54L errata 20: constant-latency mode while the radio is in use. */
	if (nrf54l_errata_20()) {
		nrf_power_task_trigger(NRF_POWER, NRF_POWER_TASK_CONSTLAT);
	}
#endif
	return 0;
}

/* ---------------------------------------------------------------------------
 * main
 * ---------------------------------------------------------------------------
 */

struct sweep_entry {
	const char *name;
	bool bitrev;
	bool base_reverse;
	bool prefix_first;
};

static const struct sweep_entry sweep[] = {
	{ "P0 rev,a0-at-lsb,pfx-last", true,  false, false },
	{ "P1 rev,a0-at-msb,pfx-last", true,  true,  false },
	{ "P2 raw,a0-at-lsb,pfx-last", false, false, false },
	{ "P3 raw,a0-at-msb,pfx-last", false, true,  false },
	{ "P4 rev,a0-at-lsb,pfx-1st ", true,  false, true  },
	{ "P5 rev,a0-at-msb,pfx-1st ", true,  true,  true  },
	{ "P6 raw,a0-at-lsb,pfx-1st ", false, false, true  },
	{ "P7 raw,a0-at-msb,pfx-1st ", false, true,  true  },
};

/* The extra axis that only exists once BALEN < 4: whether the used base bytes
 * are the low ones or the high ones. Phase A cannot see this axis at all, which
 * is exactly why phase C has to sweep it rather than inherit an answer. */
struct pack_entry {
	const char *name;
	bool base_reverse;
	bool base_top_aligned;
};

static const struct pack_entry packs[] = {
	{ "K0 a0-at-lsb,low-bytes ", false, false },
	{ "K1 a0-at-lsb,high-bytes", false, true  },
	{ "K2 a0-at-msb,low-bytes ", true,  false },
	{ "K3 a0-at-msb,high-bytes", true,  true  },
};

static void banner(void)
{
	uint8_t golden[15] = { 0xA6, 0xC5, 0x34, 0x12, 0x78, 0x01, 0x0A,
			       0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07 };
	uint8_t plus[17];
	uint16_t g = crc16_ccitt_false(golden, sizeof(golden), 0xFFFFu);
	uint16_t chained;
	uint16_t z;

	memcpy(plus, golden, sizeof(golden));
	plus[15] = (uint8_t)(g >> 8);
	plus[16] = (uint8_t)(g & 0xFFu);
	z = crc16_ccitt_false(plus, sizeof(plus), 0xFFFFu);
	chained = crc16_ccitt_false(&golden[2], sizeof(golden) - 2, CRC_AFTER_NET);

	printk("\n============================================================\n");
	printk("Spike A - rx_raw. Can a bare nRF RADIO hear a real ANT frame?\n");
	printk("Board: %s   RF index %u (%u MHz)\n", CONFIG_BOARD_TARGET,
	       ANT_RF_INDEX, 2400u + ANT_RF_INDEX);
	printk("Ground truth from tools/ant_scan.py: device #%u, type 0x%02X, trans %u\n",
	       (unsigned int)SPIKE_DEVNUM, SPIKE_DTYPE, SPIKE_TTYPE);
	printk("Expected on-air address: %02X %02X %02X %02X %02X\n",
	       ANT_NET_HI, ANT_NET_LO, DEVNUM_LO, DEVNUM_HI, SPIKE_DTYPE);

	/* Cheap, decisive, and it runs before the radio does: if these three
	 * lines are wrong, nothing downstream can be trusted. */
	printk("CRC self-test: golden=0x%04X (expect 0x1B12), over-17=0x%04X "
	       "(expect 0x0000), chained-from-0x233E=0x%04X (expect 0x1B12)\n",
	       g, z, chained);
	printk("============================================================\n\n");
}

int main(void)
{
	struct rx_stats st;
	unsigned int cycle = 0;

	/* The console is the whole output of this program, so give the host a
	 * moment to attach before the banner scrolls past. */
	k_sleep(K_MSEC(1500));
	banner();

	if (hfxo_start() < 0) {
		return 0;
	}

	while (1) {
		int best = -1;
		uint32_t best_ok = 0;
		struct rx_cfg c;
		bool use_lenfield;

		printk("\n########## cycle %u ##########\n", ++cycle);

		/* -- Phase A: which address arithmetic puts A6 C5 .. on the air? */
		printk("\n--- phase A: address bit/byte order sweep "
		       "(5-byte address, static 10-byte body, %u ms each) ---\n",
		       SWEEP_MS);
		for (int i = 0; i < (int)ARRAY_SIZE(sweep); i++) {
			c = tracking_cfg(sweep[i].name, sweep[i].bitrev,
					 sweep[i].base_reverse,
					 sweep[i].prefix_first, false);
			run_cfg(&c, SWEEP_MS, &st, 2);
			if (st.crc_ok > best_ok) {
				best_ok = st.crc_ok;
				best = i;
			}
		}

		if (best < 0 || best_ok == 0) {
			printk("\nphase A: NO permutation produced a CRC-valid frame.\n"
			       "Either the transmitter is not running, or the premise "
			       "does not hold on this part.\n");
			continue;
		}
		printk("\nphase A winner: %s (%u CRC-valid in %u ms)\n",
		       sweep[best].name, best_ok, SWEEP_MS);

		/* -- Phase B: is the length byte really a length byte? */
		printk("\n--- phase B: length-field configuration ---\n");
		c = tracking_cfg("L0 CRCINC=1 LFLEN=8", sweep[best].bitrev,
				 sweep[best].base_reverse,
				 sweep[best].prefix_first, true);
		run_cfg(&c, LENCMP_MS, &st, 2);
		use_lenfield = (st.crc_ok > 0);
		printk("phase B: CRCINC path %s\n",
		       use_lenfield ? "WORKS - the mapping stands as written"
				    : "FAILED - falling back to STATLEN=10");

		c = tracking_cfg("L1 PCNF0=0 STATLEN=10", sweep[best].bitrev,
				 sweep[best].base_reverse,
				 sweep[best].prefix_first, false);
		run_cfg(&c, LENCMP_MS, &st, 2);

		/* -- Phase C: read the channel ID off the air instead of matching it */
		printk("\n--- phase C: search configuration (3-byte address, "
		       "STATLEN=12), sweeping base packing ---\n");
		{
			int sbest = -1;
			uint32_t sok = 0;

			for (int i = 0; i < (int)ARRAY_SIZE(packs); i++) {
				c = search_cfg(packs[i].name,
					       sweep[best].bitrev,
					       packs[i].base_reverse,
					       packs[i].base_top_aligned);
				run_cfg(&c, SEARCH_MS, &st, 2);
				/* Rank on id_ok, not crc_ok. A 3-byte address is
				 * short enough that noise triggers the matcher
				 * several times a second, and a config that only
				 * ever produced CRC errors has not found the
				 * transmitter - it has found the noise floor. */
				if (st.id_ok > sok) {
					sok = st.id_ok;
					sbest = i;
				}
			}
			if (sbest >= 0) {
				printk("phase C winner: %s (%u decoded IDs in %u ms)\n",
				       packs[sbest].name, sok, SEARCH_MS);
			} else {
				printk("phase C: no packing decoded a channel ID\n");
			}

			/* -- Phase D: the measurement the pass criterion is read from */
			printk("\n--- phase D: 60 s measurement ---\n");
			c = tracking_cfg("D-tracking", sweep[best].bitrev,
					 sweep[best].base_reverse,
					 sweep[best].prefix_first, use_lenfield);
			run_cfg(&c, MEASURE_MS, &st, 4);
			printk("phase D tracking: %u CRC-valid frames in %u s "
			       "(pass needs >= 50)\n", st.crc_ok, MEASURE_MS / 1000u);

			if (sbest >= 0) {
				c = search_cfg("D-search", sweep[best].bitrev,
					       packs[sbest].base_reverse,
					       packs[sbest].base_top_aligned);
				run_cfg(&c, MEASURE_MS, &st, 4);
				printk("phase D search: %u CRC-valid, %u with a "
				       "decoded channel ID matching #%u/0x%02X/%u\n",
				       st.crc_ok, st.id_ok,
				       (unsigned int)SPIKE_DEVNUM, SPIKE_DTYPE,
				       SPIKE_TTYPE);
			} else {
				printk("phase D search: skipped, no search "
				       "packing decoded anything\n");
			}

			/* -- Phase E: the 30-minute bonus.
			 * Swept over the same packings rather than inheriting
			 * phase C's winner, because [55 A6 C5] and [A6 C5 17]
			 * differ in more than their bytes: one of them starts
			 * where the demodulator would otherwise be locking on. */
			printk("\n--- phase E: preamble-as-address "
			       "(BALEN=2, [55 A6 C5], SKIPADDR=Skip, "
			       "CRCINIT=0x233E) ---\n");
			{
				int ebest = -1;
				uint32_t eok = 0;

				for (int i = 0; i < (int)ARRAY_SIZE(packs); i++) {
					c = preamble_cfg(packs[i].name,
							 sweep[best].bitrev,
							 packs[i].base_reverse,
							 packs[i].base_top_aligned);
					run_cfg(&c, SEARCH_MS, &st, 2);
					if (st.id_ok > eok) {
						eok = st.id_ok;
						ebest = i;
					}
				}

				if (ebest < 0) {
					printk("phase E: no packing decoded a "
					       "channel ID - preamble-as-address "
					       "not usable as configured\n");
				} else {
					c = preamble_cfg("E-preamble-as-address",
							 sweep[best].bitrev,
							 packs[ebest].base_reverse,
							 packs[ebest].base_top_aligned);
					run_cfg(&c, MEASURE_MS, &st, 4);
					printk("phase E: %u CRC-valid frames in "
					       "%u s, %u with a matching channel "
					       "ID\n", st.crc_ok,
					       MEASURE_MS / 1000u, st.id_ok);
				}
			}
		}
	}

	return 0;
}
