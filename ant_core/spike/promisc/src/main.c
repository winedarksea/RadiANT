/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Spike B - promisc: where does ANT signal the packet type?
 *
 * Provenance: clean-room. Written from
 *   - docs/ant-radio-link.md (the frame layout, the CRC parameters and the
 *     open question this program exists to close),
 *   - docs/spike-a-results.md (the measured BASE0 packing rule for BALEN < 4,
 *     and the body layout a 3-byte address puts in RAM),
 *   - ant_core/spike/rx_raw/src/main.c, which is this repository's own
 *     Apache-2.0 code and is where rev8(), the CRC and the register
 *     programming come from,
 *   - the public Nordic MDK headers shipped with NCS and the nRF5340 /
 *     nRF54L15 product specifications for register semantics,
 *   - Zephyr's public clock-control and IRQ APIs.
 * Nothing here derives from sdk-ant, from libant.a, or from any adopter-gated
 * ANT+ device profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * The question
 * ---------------------------------------------------------------------------
 * The 18-byte ANT frame accounts for every byte: preamble, 2-byte network
 * address, 2-byte device number, device type, transmission type, one byte
 * rtl_433 calls "length", 8 payload bytes, 2 CRC bytes. There is nowhere in it
 * for the thing ANT obviously must signal - broadcast versus acknowledged
 * versus burst-sequence-N versus burst-last.
 *
 * The standing guess is that the "length" byte is really ANT's control byte,
 * and that rtl_433 only ever watched sensor broadcasts, so a varying field
 * looked constant. Spike A saw 0x0A on 2,164 broadcasts, which is exactly what
 * *both* readings predict, so it settled nothing.
 *
 * This program captures every frame carrying a known channel ID - in both
 * directions, because a slave answers on the master's channel ID - timestamps
 * each one, and prints byte 3 of the body. A host script then makes the same
 * device send a broadcast, then acknowledged data, then a burst. If byte 3
 * moves while the payload stays 8 bytes long, it is a control byte. If it
 * stays 0x0A throughout and only ever changes when the payload length changes,
 * it is a length.
 *
 * ---------------------------------------------------------------------------
 * Why this configuration and not Spike A's tracking one
 * ---------------------------------------------------------------------------
 * Spike A's 5-byte-address tracking configuration matches the channel ID in
 * hardware, which means the channel ID never reaches RAM - and, worse here, it
 * parses byte 3 as a LENGTH field. A receiver that has already decided the byte
 * is a length cannot report that it was not one: a frame whose byte 3 said 0x1C
 * would be read as a 28-byte body and fail its CRC, and the byte itself would
 * be lost in the wreckage.
 *
 * So this uses the *search* configuration Spike A measured - PCNF0 = 0,
 * BALEN = 2, STATLEN = 12 - which treats the whole body as opaque bytes. Byte 3
 * lands in RAM whatever it means. That is the entire reason for the choice.
 *
 * The packing rule is the one Spike A had to correct on the bench: with
 * BALEN < 4 the *high* bytes of BASE0 are used, so
 *
 *     BASE0 = (lsb-first packing of the base bytes) << (8 * (4 - BALEN))
 *
 * giving BASE0 = 0xA3650000 for [A6 C5], with PREFIX = rev8(devnum_lo). The
 * naive low-bytes packing hears nothing but noise.
 *
 * ---------------------------------------------------------------------------
 * Eight logical addresses, on purpose
 * ---------------------------------------------------------------------------
 * Only one prefix is needed to hear one device. This programs all eight anyway,
 * with the real devnum_lo at a slot chosen not to be zero, because two rows of
 * docs/ant-radio-link.md are still [inferred] and both are free to close here:
 * that eight logical addresses can be armed in one window (which the whole
 * 32-set wildcard sweep depends on), and that RADIO->RXMATCH identifies which
 * prefix matched (which is how devnum_lo is recovered when it is not known in
 * advance). If RXMATCH comes back as the slot the real device was put in, both
 * rows are answered by a run that was happening anyway.
 *
 * It costs something and the cost is stated: eight 3-byte filters fire on noise
 * roughly eight times as often as one. That is why noise is counted rather than
 * printed, and why nothing here is ever ranked on match count.
 *
 * ---------------------------------------------------------------------------
 * Why interrupts here when Spike A polled
 * ---------------------------------------------------------------------------
 * Spike A polled because its transmitter ran at 4 Hz and an interrupt would
 * have added a concurrency question and answered none. A burst is the opposite
 * case: packets arrive back to back, and printing one at 115200 baud takes
 * longer than the gap to the next. So the RADIO interrupt writes fixed-size
 * records into a ring and main() drains the ring to the console. Single
 * producer, single consumer, one word of shared state each way.
 *
 * Timestamps are captured in software at the top of the ISR, not by (D)PPI.
 * That is a deliberate accuracy limit and it is stated wherever a number is
 * reported: interrupt entry adds a small, roughly constant offset, so
 * *differences* between timestamps - inter-packet spacing, reply turnaround,
 * slot jitter - carry the ISR's jitter (order 1-2 us) and nothing more, while
 * an absolute timestamp carries the offset as well. Every figure this spike is
 * asked for is a difference.
 *
 * ---------------------------------------------------------------------------
 * Part 2: proving the capture is complete
 * ---------------------------------------------------------------------------
 * Part 1 of this spike answered the broadcast/acknowledged half of the question
 * from a histogram, where a lost frame costs a count and nothing else. The burst
 * half is different: the whole result is "sequence 0, then 1, then 2, ..., then
 * the last one", and a *silently dropped* frame in the middle of that is
 * indistinguishable from an encoding that skips a value. A gap in the sequence
 * would be read as a finding.
 *
 * So every frame that completes now carries two numbers that make a gap
 * impossible to miss rather than merely unlikely:
 *
 *   n=   a counter incremented in the ISR on every END event, before the ring
 *        is consulted. It counts frames the radio finished, not frames that got
 *        printed, so noise and foreign frames consume values too - gaps in the
 *        printed n are expected and are not the check.
 *   dr=  the value of ring_dropped at the moment this record was enqueued. This
 *        *is* the check. printk is synchronous and cannot lose a line once it
 *        starts, so the ring is the only place a frame can vanish; dr constant
 *        at zero across a whole capture is the positive statement that nothing
 *        was lost. spike_b_analyse.py fails the run if it ever increments.
 *   q=   ring occupancy at enqueue, so headroom is reported rather than assumed.
 *
 * ---------------------------------------------------------------------------
 * Part 2: what counts as "ours"
 * ---------------------------------------------------------------------------
 * Part 1 only ever had to recognise one transmitter, so it required the device
 * number, the device type and the transmission type all to match before it would
 * print. That is too strict for the reply frame, which is the thing part 2 exists
 * to see and whose layout is exactly what is not yet known: a reply that put
 * anything unexpected in the type or transmission-type byte would have been
 * silently discarded as "foreign", and the run would have looked like the slave
 * never transmitted.
 *
 * So the test is now the two things that cannot be wrong without the frame
 * belonging to someone else - the hardware address match landing on the slot the
 * real devnum_lo was written into, and devnum_hi reading back correctly - and
 * the device type and transmission type are *reported* rather than required. A
 * frame whose type bytes are not the expected ones is printed with a `!` so it
 * cannot be mistaken for an ordinary one.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/irq.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/sys/printk.h>

#include <nrfx.h>
/* Only for the interrupt-enable call. The nRF54L's RADIO has two interrupt
 * lines and therefore INTENSET00/INTENSET10 where the nRF5340 has a single
 * INTENSET, so the one register that genuinely differs between the two parts
 * is reached through the HAL. Everything else is written directly, as in
 * Spike A, because the point of a spike is that the register words it
 * programmed are visible in its source. */
#include <hal/nrf_radio.h>

#if defined(NRF54L_ERRATA_20_PRESENT)
#include <hal/nrf_power.h>
#endif

/* This program pokes RADIO registers directly. Anything else that owns the
 * radio - a Bluetooth controller, an MPSL timeslot session, the 802.15.4
 * driver the network core ships with - would reprogram them underneath it. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT), "Spike B owns the RADIO; CONFIG_BT must be off");
BUILD_ASSERT(!IS_ENABLED(CONFIG_NRF_802154_RADIO_DRIVER),
	     "Spike B owns the RADIO; the 802.15.4 driver must be off");

/* ---------------------------------------------------------------------------
 * Which device to listen to
 *
 * Only devnum_lo is a hardware filter - it is the prefix byte. devnum_hi, the
 * device type and the transmission type land in RAM and are checked in
 * software, which is what makes an "is this our device?" decision reportable
 * rather than assumed.
 * ---------------------------------------------------------------------------
 */
#ifndef SPIKE_DEVNUM
#define SPIKE_DEVNUM   14871u   /* 0x3A17 - the bench transmitter, per Spike A */
#endif
#ifndef SPIKE_DTYPE
#define SPIKE_DTYPE    0x0Bu    /* 11, bicycle power */
#endif
#ifndef SPIKE_TTYPE
#define SPIKE_TTYPE    0x05u
#endif

/* Which of the eight logical addresses gets the real devnum_lo. Deliberately
 * not 0: RXMATCH reads 0 both when slot 0 matched and when nothing was
 * programmed, so slot 0 cannot distinguish "RXMATCH works" from "RXMATCH is
 * stuck". Spike A's RXMATCH row stayed [inferred] for exactly that reason. */
#ifndef SPIKE_REAL_SLOT
#define SPIKE_REAL_SLOT 5u
#endif

#define ANT_NET_HI     0xA6u
#define ANT_NET_LO     0xC5u
#define ANT_RF_INDEX   57u      /* 2457 MHz */

#define DEVNUM_LO      ((uint8_t)(SPIKE_DEVNUM & 0xFFu))
#define DEVNUM_HI      ((uint8_t)((SPIKE_DEVNUM >> 8) & 0xFFu))

/* Body bytes the radio puts in RAM with a 3-byte address and STATLEN = 12:
 *   [devnum_hi][device_type][trans_type][byte 3][d0..d7]
 * Byte 3 is the field under investigation. Naming it OFF_CTRL rather than
 * OFF_LEN is not a conclusion, it is a refusal to encode the guess in the
 * source of the experiment that tests it. */
#define BODY_LEN       12u
#define OFF_DEVNUM_HI  0u
#define OFF_DTYPE      1u
#define OFF_TTYPE      2u
#define OFF_CTRL       3u
#define OFF_PAYLOAD    4u

#define RX_BUF_LEN     32u

/* Ring capacity. A burst is the only thing that can outrun the console, and
 * even a 24-packet burst at 115200 baud drains in well under a second. */
#define RING_LEN       512u

/* How often the running summary prints. */
#define SUMMARY_MS     15000

static uint8_t rx_buf[RX_BUF_LEN] __aligned(4);

/* The eight prefixes, on-air values (before bit reversal). Seven are decoys
 * chosen not to collide with the real devnum_lo; their only job is to prove
 * that arming eight filters does not break the one that matters and that
 * RXMATCH names the right one. */
static uint8_t prefix_onair[8];

/* ---------------------------------------------------------------------------
 * Time base
 *
 * Both parts can produce a 1 MHz free-running counter, but not the same way,
 * and the difference is not worth hiding behind an abstraction that pretends
 * otherwise:
 *
 *  - nRF54L15: Zephyr's system clock *is* the GRTC at 1 MHz, so
 *    k_cycle_get_32() already returns microseconds. The BUILD_ASSERT is what
 *    stops that from silently becoming false.
 *  - nRF5340 network core: the system clock is a 32768 Hz RTC - 30 us of
 *    granularity, which is coarser than the numbers this spike exists to
 *    measure. So a TIMER is run at 1 MHz instead.
 * ---------------------------------------------------------------------------
 */
#if defined(NRF5340_XXAA_NETWORK)

#define SPIKE_TIME_SOURCE "TIMER0 @ 1 MHz"

static void timebase_start(void)
{
	NRF_TIMER0->TASKS_STOP = 1;
	NRF_TIMER0->MODE = TIMER_MODE_MODE_Timer;
	NRF_TIMER0->BITMODE = TIMER_BITMODE_BITMODE_32Bit;
	NRF_TIMER0->PRESCALER = 4;   /* 16 MHz >> 4 = 1 MHz */
	NRF_TIMER0->TASKS_CLEAR = 1;
	NRF_TIMER0->TASKS_START = 1;
}

static inline uint32_t us_now(void)
{
	NRF_TIMER0->TASKS_CAPTURE[3] = 1;
	return NRF_TIMER0->CC[3];
}

#else

#define SPIKE_TIME_SOURCE "k_cycle_get_32() @ 1 MHz"

BUILD_ASSERT(CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC == 1000000,
	     "this spike prints microseconds and assumes a 1 MHz cycle counter");

static void timebase_start(void) { }

static inline uint32_t us_now(void)
{
	return k_cycle_get_32();
}

#endif

/* The RADIO interrupt is numbered differently on the two parts. The MDK series
 * macros are used rather than Kconfig because they come from the same header
 * that defines the enumerators. */
#if defined(NRF54L_SERIES)
#define SPIKE_RADIO_IRQN  RADIO_0_IRQn
#else
#define SPIKE_RADIO_IRQN  RADIO_IRQn
#endif

/* ---------------------------------------------------------------------------
 * Records and the ring
 * ---------------------------------------------------------------------------
 */
struct rec {
	uint32_t t_addr;    /* us at the ADDRESS interrupt - closest thing to t_sync */
	uint32_t t_end;     /* us at the END interrupt */
	uint32_t idx;       /* ISR END sequence: counts frames, not printed lines */
	uint32_t dropped;   /* ring_dropped as it stood when this record was made */
	uint16_t rxcrc;
	uint8_t  crc_ok;
	uint8_t  rxmatch;
	uint8_t  depth;     /* ring occupancy at enqueue, capped at 255 */
	int8_t   rssi;
	uint8_t  body[BODY_LEN];
};

static struct rec ring[RING_LEN];
static volatile uint32_t ring_head;   /* written by the ISR only */
static volatile uint32_t ring_tail;   /* written by main() only */
static volatile uint32_t ring_dropped;

static volatile uint32_t isr_t_addr;
static volatile uint32_t isr_addr_events;

/* Counters. Written by the ISR, read by main() for the summary; a torn read of
 * a counter costs a wrong summary line and nothing else, which is why there is
 * no lock. */
static volatile uint32_t n_addr;
static volatile uint32_t n_end;
static volatile uint32_t n_crc_ok;
static volatile uint32_t n_crc_err;

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);

	if (NRF_RADIO->EVENTS_ADDRESS) {
		NRF_RADIO->EVENTS_ADDRESS = 0;
		(void)NRF_RADIO->EVENTS_ADDRESS;
		isr_t_addr = us_now();
		isr_addr_events++;
		n_addr++;
	}

	if (NRF_RADIO->EVENTS_END) {
		uint32_t t_end = us_now();
		uint32_t head = ring_head;
		uint32_t next = (head + 1u) % RING_LEN;
		uint32_t tail = ring_tail;
		bool ok;

		NRF_RADIO->EVENTS_END = 0;
		(void)NRF_RADIO->EVENTS_END;

		ok = (NRF_RADIO->CRCSTATUS & RADIO_CRCSTATUS_CRCSTATUS_Msk) ==
		     RADIO_CRCSTATUS_CRCSTATUS_CRCOk;
		n_end++;
		if (ok) {
			n_crc_ok++;
		} else {
			n_crc_err++;
		}

		if (next == tail) {
			ring_dropped++;
		} else {
			struct rec *r = &ring[head];
			uint32_t depth = (head + RING_LEN - tail) % RING_LEN;

			r->t_addr = isr_t_addr;
			r->t_end = t_end;
			r->idx = n_end;
			r->dropped = ring_dropped;
			r->depth = (uint8_t)(depth > 255u ? 255u : depth);
			r->rxcrc = (uint16_t)(NRF_RADIO->RXCRC & 0xFFFFu);
			r->crc_ok = ok ? 1u : 0u;
			r->rxmatch = (uint8_t)(NRF_RADIO->RXMATCH &
					       RADIO_RXMATCH_RXMATCH_Msk);
			r->rssi = (int8_t)(-(int32_t)(NRF_RADIO->RSSISAMPLE & 0x7Fu));
			memcpy(r->body, rx_buf, BODY_LEN);
			ring_head = next;
		}
	}
}

/* ---------------------------------------------------------------------------
 * Radio programming - Spike A's measured search configuration, widened to
 * eight logical addresses.
 * ---------------------------------------------------------------------------
 */
static uint8_t rev8(uint8_t b)
{
	b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
	b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
	b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
	return b;
}

static void radio_configure(void)
{
	/* BALEN = 2: on-air address is [A6 C5 devnum_lo], three bytes, the
	 * shortest the nRF can match. */
	const uint8_t balen = 2u;
	const uint8_t base_onair[2] = { ANT_NET_HI, ANT_NET_LO };
	uint32_t base_reg = 0;
	uint32_t p0 = 0, p1 = 0;

	/* (lsb-first packing) << (8 * (4 - BALEN)). Written in the general form
	 * so it collapses to the tracking case at BALEN = 4 rather than being a
	 * second, separately-wrong expression. */
	for (uint8_t i = 0; i < balen; i++) {
		base_reg |= (uint32_t)rev8(base_onair[i]) << (8u * i);
	}
	base_reg <<= 8u * (4u - balen);

#if defined(NRF53_ERRATA_117_PRESENT)
	/* nRF5340 anomaly 117: MODE must be accompanied by a trim write, and
	 * without it the receiver is deaf in a way that looks exactly like a
	 * wrong address. The value for every mode other than 2 Mbit and
	 * 802.15.4 is the one at 0x01FF0080. */
	if (nrf53_errata_117()) {
		*((volatile uint32_t *)0x41008588) =
			*((volatile uint32_t *)0x01FF0080);
	}
#endif

	NRF_RADIO->MODE = (RADIO_MODE_MODE_Nrf_1Mbit << RADIO_MODE_MODE_Pos);
	NRF_RADIO->FREQUENCY = ANT_RF_INDEX;

	/* Logical address 0 uses BASE0; 1..7 use BASE1. Both are the ANT+
	 * network address, so all eight differ only in their prefix - which is
	 * exactly the nRF24 pipe model the frame descends from. */
	NRF_RADIO->BASE0 = base_reg;
	NRF_RADIO->BASE1 = base_reg;

	for (uint8_t i = 0; i < 4u; i++) {
		p0 |= (uint32_t)rev8(prefix_onair[i]) << (8u * i);
		p1 |= (uint32_t)rev8(prefix_onair[i + 4u]) << (8u * i);
	}
	NRF_RADIO->PREFIX0 = p0;
	NRF_RADIO->PREFIX1 = p1;
	NRF_RADIO->RXADDRESSES = 0xFFu;

	/* PCNF0 = 0: no S0, no LENGTH field, no S1. The body is opaque, which
	 * is the whole point - see the header comment. */
	NRF_RADIO->PCNF0 = 0;
	NRF_RADIO->PCNF1 =
		((uint32_t)(RX_BUF_LEN - 4u) << RADIO_PCNF1_MAXLEN_Pos) |
		((uint32_t)BODY_LEN << RADIO_PCNF1_STATLEN_Pos) |
		((uint32_t)balen << RADIO_PCNF1_BALEN_Pos) |
		(RADIO_PCNF1_ENDIAN_Big << RADIO_PCNF1_ENDIAN_Pos) |
		(RADIO_PCNF1_WHITEEN_Disabled << RADIO_PCNF1_WHITEEN_Pos);

	NRF_RADIO->CRCCNF =
		(RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
		(RADIO_CRCCNF_SKIPADDR_Include << RADIO_CRCCNF_SKIPADDR_Pos);
	NRF_RADIO->CRCPOLY = 0x11021u;
	NRF_RADIO->CRCINIT = 0xFFFFu;

	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)rx_buf;

	NRF_RADIO->SHORTS = RADIO_SHORTS_READY_START_Msk |
			    RADIO_SHORTS_END_START_Msk |
			    RADIO_SHORTS_ADDRESS_RSSISTART_Msk;

	printk("[cfg] BALEN=%u BASE0=BASE1=0x%08X PREFIX0=0x%08X PREFIX1=0x%08X\n",
	       balen, (unsigned int)NRF_RADIO->BASE0,
	       (unsigned int)NRF_RADIO->PREFIX0, (unsigned int)NRF_RADIO->PREFIX1);
	printk("[cfg] RXADDRESSES=0x%02X PCNF0=0x%08X PCNF1=0x%08X CRCCNF=0x%08X\n",
	       (unsigned int)NRF_RADIO->RXADDRESSES,
	       (unsigned int)NRF_RADIO->PCNF0, (unsigned int)NRF_RADIO->PCNF1,
	       (unsigned int)NRF_RADIO->CRCCNF);
	printk("[cfg] on-air prefixes (slot:byte):");
	for (uint8_t i = 0; i < 8u; i++) {
		printk(" %u:%02X%s", i, prefix_onair[i],
		       (i == SPIKE_REAL_SLOT) ? "*" : "");
	}
	printk("   (* = devnum_lo of the device under test)\n");
}

static int hfxo_start(void)
{
	const struct device *clk = DEVICE_DT_GET_ONE(nordic_nrf_clock);
	int err;

	if (!device_is_ready(clk)) {
		printk("FATAL: clock controller not ready\n");
		return -ENODEV;
	}

	err = clock_control_on(clk, CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (err < 0) {
		printk("FATAL: HFXO did not start (%d)\n", err);
		return err;
	}

#if defined(NRF54L_ERRATA_20_PRESENT)
	if (nrf54l_errata_20()) {
		nrf_power_task_trigger(NRF_POWER, NRF_POWER_TASK_CONSTLAT);
	}
#endif
	return 0;
}

/* ---------------------------------------------------------------------------
 * Reporting
 * ---------------------------------------------------------------------------
 */

/* Histogram of byte 3 over CRC-valid frames from the device under test. This
 * is the answer, in one line, once the host script has done its work. */
static uint32_t ctrl_hist[256];
static uint32_t n_ours;          /* CRC ok and the device number matches */
static uint32_t n_foreign;       /* CRC ok, some other ANT+ device */
static uint32_t n_longframe;     /* CRC bad but the device number is ours */
static uint32_t n_oddtype;       /* ours, but dtype/ttype are not the expected pair */

static uint32_t prev_ours_t;
static bool     have_prev_ours;

static void print_rec(const struct rec *r)
{
	const uint8_t dhi = r->body[OFF_DEVNUM_HI];
	const uint8_t dty = r->body[OFF_DTYPE];
	const uint8_t tty = r->body[OFF_TTYPE];
	const uint8_t ctrl = r->body[OFF_CTRL];
	/* devnum_lo is not in RAM. It is recovered from the prefix table using
	 * the logical address the hardware reports it matched, which is the
	 * mechanism ant_search.c will use and which Spike A could not test. */
	const uint8_t dlo = prefix_onair[r->rxmatch & 7u];
	const uint16_t devnum = (uint16_t)(dlo | ((uint16_t)dhi << 8));
	/* See the header note "what counts as ours": the device number decides,
	 * the type bytes are reported. Requiring them would throw away exactly
	 * the frame this run exists to see. */
	const bool ours = (devnum == (uint16_t)SPIKE_DEVNUM);
	const bool typical = ((dty & 0x7Fu) == SPIKE_DTYPE) && (tty == SPIKE_TTYPE);
	int32_t dt;

	if (r->crc_ok) {
		if (!ours) {
			n_foreign++;
			return;   /* a real ANT+ device that is not ours; counted, not printed */
		}
		n_ours++;
		ctrl_hist[ctrl]++;
		if (!typical) {
			n_oddtype++;
		}
	} else {
		if (!ours) {
			return;   /* noise through a 3-byte matcher; counted in n_crc_err */
		}
		/* CRC bad but the channel ID reads correctly. With STATLEN
		 * fixed at 12 that is what a frame with a *different payload
		 * length* looks like: the right header, then the wrong number
		 * of bytes taken off the air. Worth printing loudly - it is
		 * the only evidence this configuration can offer about a
		 * non-standard payload length. (It used to name a predicted
		 * 0x9A advanced-burst encoding; that prediction was withdrawn
		 * with the length reading of byte 3 - see
		 * docs/spike-b-part2-results.md. Every longframe either part
		 * of Spike B printed has been an ordinary bit error.) */
		n_longframe++;
	}

	dt = have_prev_ours ? (int32_t)(r->t_addr - prev_ours_t) : 0;
	prev_ours_t = r->t_addr;
	have_prev_ours = true;

	printk("P n=%u dr=%u q=%u t=%010u dt=%+8d %s m=%u rssi=%d dn=%u ty=%02X%s "
	       "tt=%u B3=%02X pl=%02X%02X%02X%02X%02X%02X%02X%02X crc=%04X air=%u\n",
	       r->idx, r->dropped, r->depth,
	       r->t_addr, dt, r->crc_ok ? "OK" : "ER", r->rxmatch, r->rssi,
	       devnum, dty, typical ? "" : "!", tty, ctrl,
	       r->body[4], r->body[5], r->body[6], r->body[7],
	       r->body[8], r->body[9], r->body[10], r->body[11],
	       r->rxcrc, (unsigned int)(r->t_end - r->t_addr));
}

static void print_summary(void)
{
	printk("S t=%010u ours=%u foreign=%u longframe=%u oddtype=%u "
	       "crc_ok=%u crc_err=%u addr=%u end=%u dropped=%u  B3:",
	       us_now(), n_ours, n_foreign, n_longframe, n_oddtype,
	       n_crc_ok, n_crc_err, n_addr, n_end, ring_dropped);
	for (unsigned int i = 0; i < 256u; i++) {
		if (ctrl_hist[i]) {
			printk(" %02X=%u", i, ctrl_hist[i]);
		}
	}
	printk("\n");
}

static void banner(void)
{
	printk("\n============================================================\n");
	printk("Spike B - promisc. Where does ANT signal the packet type?\n");
	printk("Board: %s   RF index %u (%u MHz)   time: %s\n",
	       CONFIG_BOARD_TARGET, ANT_RF_INDEX, 2400u + ANT_RF_INDEX,
	       SPIKE_TIME_SOURCE);
	printk("Listening for device #%u (0x%04X), type 0x%02X, trans %u\n",
	       (unsigned int)SPIKE_DEVNUM, (unsigned int)SPIKE_DEVNUM,
	       SPIKE_DTYPE, SPIKE_TTYPE);
	printk("Body in RAM: [devnum_hi][dtype][ttype][BYTE 3][d0..d7]  "
	       "- byte 3 is the field under test\n");
	printk("P lines: t/dt/air in us, dt against the previous frame of this "
	       "device\n");
	printk("  n=END-event counter (all frames, incl. noise)  "
	       "dr=frames lost by the ring  q=ring depth at enqueue\n");
	printk("  dr must stay 0 for the capture to be complete; a '!' after ty "
	       "means dtype/ttype were not the expected pair\n");
	printk("============================================================\n\n");
}

int main(void)
{
	int64_t next_summary;

	/* The console is the whole output of this program; give the host a
	 * moment to attach before the banner scrolls past. */
	k_sleep(K_MSEC(1500));
	banner();

	for (uint8_t i = 0; i < 8u; i++) {
		/* Decoys spaced across the byte range and forced away from the
		 * real value, so a stuck RXMATCH cannot be mistaken for a
		 * working one. */
		prefix_onair[i] = (uint8_t)(0x11u * (i + 1u));
		if (prefix_onair[i] == DEVNUM_LO) {
			prefix_onair[i] = (uint8_t)(DEVNUM_LO ^ 0xFFu);
		}
	}
	prefix_onair[SPIKE_REAL_SLOT] = DEVNUM_LO;

	timebase_start();

	if (hfxo_start() < 0) {
		return 0;
	}

	radio_configure();

	IRQ_CONNECT(SPIKE_RADIO_IRQN, 0, radio_isr, NULL, 0);
	irq_enable(SPIKE_RADIO_IRQN);

	NRF_RADIO->EVENTS_ADDRESS = 0;
	NRF_RADIO->EVENTS_END = 0;
	nrf_radio_int_enable(NRF_RADIO,
			     NRF_RADIO_INT_ADDRESS_MASK | NRF_RADIO_INT_END_MASK);
	NRF_RADIO->TASKS_RXEN = 1;

	printk("[run] receiver armed\n");

	next_summary = k_uptime_get() + SUMMARY_MS;

	while (1) {
		uint32_t tail = ring_tail;

		if (tail != ring_head) {
			print_rec(&ring[tail]);
			ring_tail = (tail + 1u) % RING_LEN;
			continue;
		}

		if (k_uptime_get() >= next_summary) {
			next_summary += SUMMARY_MS;
			print_summary();
		}

		k_sleep(K_MSEC(2));
	}

	return 0;
}
