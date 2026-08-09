/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf.c - radiant_radio_hal.h on a bare nRF RADIO.
 *
 * Provenance: clean-room. Written from
 *   - radiant_core/include/radiant_core/radiant_radio_hal.h, the frozen backend
 *     contract, which is the whole of what this file has to satisfy,
 *   - docs/ant-radio-link.md's register mapping and docs/spike-a-results.md's
 *     measurements of it, which settled the address arithmetic on hardware,
 *   - radiant_core/spike/rx_raw/src/main.c, this repository's own proven receive
 *     code, for the configuration that was observed to work,
 *   - the public Nordic MDK headers and nrfx shipped with NCS, and the
 *     nRF52840/nRF54L15 product specifications for register semantics.
 * Nothing here derives from sdk-ant, from libant.a, or from any adopter-gated
 * ANT+ device profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What is measured and what is not - read this before trusting a number
 * ---------------------------------------------------------------------------
 *
 * MEASURED, on nRF54L15, Spike A, 2026-08-09. The address arithmetic and the
 * packet configuration below are not predictions: eight permutations were swept
 * on air and exactly one produced CRC-valid frames, in all six cycles of two
 * runs. Bit-reverse every address byte; the first on-air base byte sits in the
 * LEAST significant used byte of BASE0; the prefix goes out LAST. See
 * pack_address().
 *
 * MEASURED, Spike B: byte 3 of an ANT frame is a CONTROL byte, not a length.
 * So the packet configuration is the static one - PCNF0 = 0, STATLEN = body
 * length - and not the LFLEN=8/CRCINC=1 form that Spike A also found working.
 * Spike A only ever heard broadcasts, where the two are indistinguishable.
 *
 * NOT MEASURED, and each is called out again at its definition:
 *
 *   1. THE t_sync CALIBRATION CONSTANT IS ZERO AND THAT IS A PLACEHOLDER.
 *      radiant_radio_hal.h spends a page on why: a CONSTANT t_sync error
 *      cancels out of the period estimate, so the drift PLL still locks and
 *      nothing looks wrong, while every receive window shifts by that amount
 *      and yield falls by a few tenths of a percent - the same order as this
 *      bench's characterised ~0.4 % collision floor. There is no error code and
 *      no log line. Measuring it needs the wired two-board trigger described
 *      there, and until that is done the A/B `timing` gate is the only thing
 *      that would notice.
 *
 *   2. TRANSMIT HAS NEVER BEEN EXERCISED BY ANYTHING. Both spikes were
 *      receive-only. The ramp-up and turnaround constants below are datasheet
 *      figures, not antenna-referenced measurements, and arm_tx() computes a
 *      start instant from them. A wrong constant here does not fail loudly: it
 *      puts the frame on the air early or late by that amount.
 *
 *   3. EVERY REGISTER FACT IS FROM nRF54L15. The nRF52840 is the shipping part
 *      and the mapping is predicted to hold on it - the two RADIOs differ in
 *      ramp-up and in the TIMING/RXGAIN block, not in the packet engine - but
 *      predicted is not measured.
 *
 * ---------------------------------------------------------------------------
 * The one hardware limit the core cannot see, and what this file does about it
 * ---------------------------------------------------------------------------
 *
 * caps.max_filters is 8, and that is true for SEARCH and misleading for
 * TRACKING, so it is worth stating plainly here rather than being discovered as
 * a stream of RADIANT_RADIO_ENOTSUP.
 *
 * The nRF matcher has eight logical addresses, but they are not eight
 * independent addresses. Logical 0 is BASE0 + PREFIX0.AP0; logical 1..7 are
 * BASE1 + AP1..AP7. So a window can express AT MOST TWO DISTINCT BASES, and
 * seven of the eight filters must share one.
 *
 * That falls out exactly right for the search geometry and exactly wrong for
 * the tracking one:
 *
 *   SEARCH, 3-byte address [A6 C5 devnum_lo]: base is [A6 C5], shared by every
 *   filter, and the prefix is devnum_lo. Eight prefixes, one base - which is
 *   precisely the eight-prefix slot map Spike B ran and the 32-set sweep
 *   radiant_search.c builds. Eight filters, no constraint hit.
 *
 *   TRACKING, 5-byte address [A6 C5 dnl dnh dtype]: base is [A6 C5 dnl dnh] and
 *   differs per sensor. Two tracked channels can share a window only if they
 *   have the same device number, which is to say never. So a merged tracking
 *   window is limited to TWO channels on this backend, not eight.
 *
 * This is a property of the hardware, not of the core, so it is expressed the
 * way radiant_radio_hal.h says to express it: arm_rx() validates the filter set
 * and returns RADIANT_RADIO_ENOTSUP for one it cannot put on the air. It never
 * programmes something close. radiant_sched.c is free to react by splitting the
 * window; what it must not do is believe eight arbitrary addresses were armed.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/clock_control.h>
#include <zephyr/drivers/clock_control/nrf_clock_control.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include <nrfx.h>
#include <hal/nrf_radio.h>
#include <hal/nrf_timer.h>
#include <helpers/nrfx_gppi.h>

#if defined(NRF54L_ERRATA_20_PRESENT)
#include <hal/nrf_power.h>
#endif

#include <radiant_core/radiant_radio_hal.h>

LOG_MODULE_REGISTER(radiant_radio_nrf, CONFIG_RADIANT_CORE_LOG_LEVEL);

/* This file pokes RADIO registers directly and owns the peripheral outright.
 * Anything else that owns it - a Bluetooth controller, an MPSL timeslot session
 * - would silently reprogram them underneath us and the symptom would be
 * unexplained loss rather than an error. Loud beats silent; the MPSL-arbitrated
 * backend is a separate file and a separate Kconfig choice for this reason. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_BT),
	     "radiant_radio_nrf owns the RADIO outright; CONFIG_BT must be off. "
	     "Coexistence with a BLE controller is RADIANT_CORE_BACKEND_MPSL.");

/* ---------------------------------------------------------------------------
 * The timebase
 *
 * One TIMER at 1 MHz, free-running, never stopped. It is the backend's whole
 * clock: radiant_radio_now() reads it, t_sync is a hardware capture of it, and
 * every scheduled start and window close is a compare against it.
 *
 * Using the RTC or Zephyr's kernel clock instead would be cheaper and wrong.
 * t_sync has to be captured by hardware from RADIO's own ADDRESS event with no
 * software in the path - that is what makes the extended-message 0xE0 timestamp
 * read 0.011 ms on the radio clock instead of the ~2.6 ms a host clock sees
 * through USB jitter - and a capture is only useful against the timer that
 * captured it.
 * ---------------------------------------------------------------------------
 */

/*
 * The TIMER comes from devicetree, not from a configured address, and the
 * first attempt at this file got it from a Kconfig hex instead. That was wrong
 * twice over and the board said so immediately:
 *
 *     ***** BUS FAULT *****  Precise data bus error
 *     BFAR Address: 0x400f0504     r3/a4: 0x400f0000
 *
 * - the address was a guess and a wrong one. TIMER20 on nRF54L15 is at
 *   0x500CA000, inside peripheral@50000000; 0x400F0000 is an nRF53-shaped
 *   number that means nothing on this part. 0x504 is MODE, so it faulted on
 *   the very first register write.
 * - and even the right address would have faulted, because every TIMER node on
 *   this SoC is `status = "disabled"` by default and an unclocked peripheral
 *   is not addressable.
 *
 * Devicetree fixes both at once and adds a third thing a constant cannot: the
 * node is a resource the rest of the system can see is taken, so a second user
 * of the same TIMER is a devicetree conflict rather than two drivers quietly
 * reprogramming each other's prescaler.
 */
#define RADIANT_TIMER_NODE  DT_CHOSEN(radiant_radio_timer)

#if !DT_NODE_EXISTS(RADIANT_TIMER_NODE)
#error "radiant_radio_nrf needs a timer. Add to the board overlay:\n\
    / { chosen { radiant,radio-timer = &timer20; }; };\n\
    &timer20 { status = \"okay\"; };\n\
It must be a TIMER this backend owns outright - it runs at 1 MHz forever and \
is what t_sync is hardware-captured into."
#endif
#if !DT_NODE_HAS_STATUS(RADIANT_TIMER_NODE, okay)
#error "radiant,radio-timer names a disabled node. Set status = \"okay\" on it."
#endif

#define TIMER_ADDR      ((NRF_TIMER_Type *)DT_REG_ADDR(RADIANT_TIMER_NODE))

/* Compare/capture channel assignment. Four of the six, so a part with only four
 * still fits. */
#define CC_SYNC     0u   /* captured from RADIO EVENTS_ADDRESS by (D)PPI */
#define CC_START    1u   /* fires TASKS_TXEN or TASKS_RXEN */
#define CC_CLOSE    2u   /* fires TASKS_DISABLE at the end of an RX window */
#define CC_NOW      3u   /* software captures here to read the counter */

/*
 * Extending a 32-bit microsecond counter to the HAL's 64 bits.
 *
 * At 1 MHz the counter wraps every 71.6 minutes, and radiant_time_t must not.
 * The fold below is exact provided it runs at least once per wrap, which
 * fold_tick() guarantees by firing every half range; everything else is
 * modular arithmetic that cannot lose a tick even if two folds race, because
 * the subtraction is done under an interrupt lock and `last` only moves
 * forwards.
 */
static struct {
	uint64_t base;   /* microseconds accounted for up to `last` */
	uint32_t last;   /* the counter value `base` was folded at */
} radiant_clock;


static uint32_t timer_capture(uint8_t cc)
{
	nrf_timer_task_trigger(TIMER_ADDR, nrf_timer_capture_task_get(cc));
	return nrf_timer_cc_get(TIMER_ADDR, cc);
}

static uint64_t clock_fold(uint32_t counter)
{
	/* Caller holds the interrupt lock. */
	radiant_clock.base += (uint32_t)(counter - radiant_clock.last);
	radiant_clock.last = counter;
	return radiant_clock.base;
}

radiant_time_t radiant_radio_now(void)
{
	unsigned int key = irq_lock();
	uint64_t now = clock_fold(timer_capture(CC_NOW));

	irq_unlock(key);
	return (radiant_time_t)now;
}

/*
 * Turn a counter value captured in the recent past into an absolute time.
 *
 * The difference is taken SIGNED on purpose. A capture taken microseconds
 * before the last fold is a perfectly ordinary thing - the RADIO's ADDRESS
 * event fires before the interrupt that reads it - and treating that as
 * unsigned would produce a timestamp 71 minutes in the future rather than 3 us
 * in the past. Signed arithmetic is correct for anything within +-35 minutes of
 * the last fold, which is every capture this backend will ever see.
 */
static radiant_time_t clock_absolute(uint32_t counter)
{
	unsigned int key = irq_lock();
	uint64_t base = clock_fold(timer_capture(CC_NOW));
	int32_t  delta = (int32_t)(counter - radiant_clock.last);

	irq_unlock(key);
	return (radiant_time_t)((int64_t)base + delta);
}

/* The counter value an absolute time corresponds to, for programming a compare.
 * Only the low 32 bits reach the hardware, which is all a compare uses. */
static uint32_t clock_counter_at(radiant_time_t t)
{
	unsigned int key = irq_lock();
	uint64_t base = clock_fold(timer_capture(CC_NOW));
	uint32_t last = radiant_clock.last;

	irq_unlock(key);
	return (uint32_t)(last + (uint32_t)((int64_t)t - (int64_t)base));
}

/* ---------------------------------------------------------------------------
 * Capabilities
 * ---------------------------------------------------------------------------
 */

static const enum radiant_phy radiant_nrf_phys[] = { RADIANT_PHY_1M_GFSK };

/*
 * Airtime arithmetic, which several constants below are derived from rather
 * than guessed. At 1 Mbit GFSK one byte is 8 us exactly.
 */
#define US_PER_BYTE            8u
#define PREAMBLE_BYTES         1u    /* PCNF0.PLEN = 8bit; confirmed by Spike A */

/*
 * Ramp-up and turnaround.
 *
 * DATASHEET FIGURES, NOT MEASUREMENTS - see note 2 in this file's header. They
 * are antenna-referenced in the datasheet's own terms, which is the right
 * definition, but nothing on this bench has confirmed them and nothing has
 * exercised transmit at all. The two-board trigger that calibrates t_sync is
 * the same rig that would measure these, so they are expected to be corrected
 * together and in one sitting.
 */
#if defined(CONFIG_SOC_SERIES_NRF54LX)
#define RAMP_UP_US             40u
#define RX_TO_TX_US            40u
#define TX_TO_RX_US            40u
#define CAPS_NAME              "nrf54l RADIO (direct)"
/* nRF54L splits RADIO's interrupt over two lines; RADIO_0 carries the packet
 * events (ADDRESS, END, DISABLED) this backend uses. */
#define RADIANT_RADIO_IRQn     RADIO_0_IRQn
#define RADIANT_RADIO_IRQn_2   RADIO_1_IRQn
#elif defined(CONFIG_SOC_SERIES_NRF52X)
#define RAMP_UP_US             40u   /* fast ramp-up; 140 us on the slow path */
#define RX_TO_TX_US            40u
#define TX_TO_RX_US            40u
#define CAPS_NAME              "nrf52 RADIO (direct)"
#define RADIANT_RADIO_IRQn     RADIO_IRQn
#else
#error "radiant_radio_nrf: no measured constants for this SoC series. Add them \
with the bench measurement that produced them, or select a different backend."
#endif

/*
 * The software setup cost of an arm call, which is what min_arm_lead_us
 * actually bounds: register programming, (D)PPI wiring, and the compare having
 * to be programmed before the counter reaches it. Ramp-up and preamble airtime
 * are NOT in this number - they are accounted separately in arm_tx(), because
 * the HAL defines t_sync at the end of the address rather than at the start of
 * transmission and mixing the two is how one backend's ramp-up ends up baked
 * into a core constant.
 *
 * radiant_transfer.h checks its 1310 us reply-window lead against this, so a
 * number that is too large here fails acknowledged data at configuration time
 * rather than on the air. That is the right direction to be wrong in.
 */
#define ARM_LEAD_US            80u

static const struct radiant_radio_caps radiant_nrf_caps = {
	.name                = CAPS_NAME,

	/* Eight logical addresses. See this file's header for the constraint
	 * that number does not express: at most two distinct BASEs. */
	.max_filters         = 8,
	.filter_wildcard_dev = false,
	.addr_len_hw_max     = 5,
	.max_body_len        = RADIANT_RADIO_BODY_MAX,

	.phys                = radiant_nrf_phys,
	.n_phys              = (uint8_t)ARRAY_SIZE(radiant_nrf_phys),
	.phy_switch_us       = 0,   /* one PHY; nothing to switch */

	.ramp_up_us          = RAMP_UP_US,
	.rx_to_tx_us         = RX_TO_TX_US,
	.tx_to_rx_us         = TX_TO_RX_US,
	.min_arm_lead_us     = ARM_LEAD_US,

	.time_resolution_ns  = 1000,   /* the 1 MHz TIMER */

	/* True: t_sync is a (D)PPI capture of RADIO's own ADDRESS event, with no
	 * software in the path. Note carefully what this does NOT claim - see
	 * T_SYNC_CAL_US, whose value is still zero. */
	.has_sync_timestamp  = true,
	.has_rssi            = true,
	.crc_in_hw           = true,

	.tx_power_min_dbm    = -40,
	.tx_power_max_dbm    = 8,
};

const struct radiant_radio_caps *radiant_radio_caps_get(void)
{
	return &radiant_nrf_caps;
}

/*
 * THE CALIBRATION CONSTANT, AND IT IS NOT CALIBRATED.
 *
 * t_sync is defined as the instant the last bit of the on-air address was at
 * the antenna. What the hardware gives us is the instant RADIO raised
 * EVENTS_ADDRESS, which differs from it by demodulator group delay, filter
 * latency and event-capture offset. The difference is a per-part constant and
 * it is measured, not looked up: one board pulses a GPIO from its own
 * address-sent event, the other captures that pulse on this same TIMER, and the
 * difference minus the known cable and air delay is this number.
 *
 * ZERO IS A PLACEHOLDER AND IT IS THE WRONG ANSWER. It is written as a named
 * constant with this comment rather than left out so that the gap is visible in
 * the code that depends on it. Read radiant_radio_hal.h's "THE FAILURE MODE,
 * WHICH IS SILENT" before assuming it does not matter: a constant error here
 * cancels out of the period estimate, so nothing looks broken while every
 * window sits off-centre and yield drops into the noise floor.
 */
#define T_SYNC_CAL_US   0

/* ---------------------------------------------------------------------------
 * Address arithmetic - the measured part
 * ---------------------------------------------------------------------------
 */

/* Bit-reverse one byte. The address is emitted least-significant-bit first
 * whatever PCNF1.ENDIAN says, so every address byte needs this on the way into
 * BASE/PREFIX. Predicted in docs/ant-radio-link.md, and the half of Spike A's
 * sweep that did NOT do it heard nothing at all. */
static uint8_t rev8(uint8_t b)
{
	b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
	b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
	b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
	return b;
}

/*
 * Split one on-air address into the hardware's BASE and PREFIX.
 *
 * addr[0] is the first byte on the air. The prefix is the LAST address byte,
 * the base is everything before it, the first base byte sits in the least
 * significant USED byte of the register, and for BALEN < 4 the used bytes are
 * the HIGH ones - which is the same statement as
 *
 *     BASE = (lsb-first packing of the base bytes) << (8 * (4 - BALEN))
 *
 * and is why this is one expression rather than a case per address length. All
 * of it is [measured]: docs/spike-a-results.md, phases A and C. Phase A settled
 * the 5-byte case and phase C the 3-byte one, and the shift is what makes them
 * the same rule rather than two facts that happen to coexist.
 */
static void pack_address(const uint8_t *addr, uint8_t addr_len,
			 uint32_t *base, uint8_t *prefix)
{
	const uint8_t balen = (uint8_t)(addr_len - 1u);
	uint32_t reg = 0;

	for (uint8_t i = 0; i < balen; i++) {
		reg |= (uint32_t)rev8(addr[i]) << (8u * i);
	}

	*base = reg << (8u * (4u - balen));
	*prefix = rev8(addr[addr_len - 1u]);
}

/* ---------------------------------------------------------------------------
 * Operation state
 *
 * One operation in flight, which is the HAL's contract and not a simplification:
 * "The HAL permits exactly one operation in flight" is what lets radiant_sched.c
 * be the single arming authority.
 * ---------------------------------------------------------------------------
 */

enum op_kind { OP_NONE = 0, OP_TX, OP_RX };

static struct {
	const struct radiant_radio_cbs *cbs;
	void                       *user;

	bool         inited;
	bool         enabled;

	enum op_kind kind;
	uint32_t     id;          /* the op id handed back by the arm call */
	uint32_t     next_id;     /* monotonically increasing, non-zero */

	/* RX bookkeeping for the window in flight. */
	uint8_t      n_filters;
	uint8_t      addr_len;
	uint8_t      body_len;
	bool         stop_on_first;
	bool         report_crc_fail;
	bool         terminal_sent;

	/*
	 * (D)PPI connections, allocated once at init.
	 *
	 * This nrfx exposes connections rather than bare channels, and that is
	 * the API to use rather than an inconvenience to work around: on nRF54L
	 * the RADIO and the TIMER can sit in different DPPI domains, and a
	 * connection knows how to route across them where a raw channel number
	 * does not. Allocating per endpoint pair also means the start and close
	 * edges cannot be wired to the wrong task by an index slip.
	 */
	nrfx_gppi_handle_t conn_sync;
	nrfx_gppi_handle_t conn_start;
	nrfx_gppi_handle_t conn_close;
	bool               conn_ok;
} radiant_op;

/* Bring-up counters. Logging from the RADIO interrupt is not safe at this
 * priority, so the interrupt counts and thread context reports. */
static volatile uint32_t radiant_dbg_isr;
static volatile uint32_t radiant_dbg_rx_ev;
static volatile uint32_t radiant_dbg_term;

/* The DMA buffer the RADIO reads and writes. Backend-owned, handed to the core
 * as rx_event.body for the duration of one callback only, which is exactly what
 * radiant_radio_hal.h says about it. */
static uint8_t radiant_rx_buf[RADIANT_RADIO_BODY_MAX + 4] __aligned(4);

/* ---------------------------------------------------------------------------
 * Register programming
 * ---------------------------------------------------------------------------
 */

static int apply_format(const struct radiant_pkt_format *fmt, uint8_t rf_index)
{
	if (fmt == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (fmt->phy != RADIANT_PHY_1M_GFSK) {
		return RADIANT_RADIO_ENOTSUP;
	}
	if (fmt->addr_len < 2u || fmt->addr_len > 5u) {
		return RADIANT_RADIO_ENOTSUP;
	}
	if (rf_index > RADIANT_RF_INDEX_MAX) {
		return RADIANT_RADIO_EINVAL;
	}

	/*
	 * Static length only, and that is a measurement rather than a
	 * simplification. Spike B established that byte 3 of an ANT frame is a
	 * control byte carrying six independent fields, not a length; the
	 * LFLEN=8/CRCINC=1 configuration that Spike A also found working is a
	 * broadcast-only receiver, because on a broadcast the control byte
	 * happens to read 0x0A and 0x0A happens to be the right length. A
	 * length-decoding format is therefore something this backend must
	 * refuse rather than approximate - it would silently truncate every
	 * acknowledged and burst frame.
	 */
	if (fmt->len_mode != RADIANT_LEN_FIXED) {
		return RADIANT_RADIO_ENOTSUP;
	}
	if (fmt->body_len == 0u || fmt->body_len > RADIANT_RADIO_BODY_MAX) {
		return RADIANT_RADIO_EINVAL;
	}

	/*
	 * CRC-16/CCITT-FALSE, and every parameter is checked rather than the
	 * width alone. The RADIO's CRC engine is not configurable in reflection
	 * or final-XOR, so a format asking for either is something this backend
	 * must refuse - computing a different CRC than the one requested would
	 * put frames on the air that the intended receiver rejects, and CRC
	 * mismatches are the hardest failure to attribute after the fact.
	 */
	if (fmt->crc.width_bits != 16u || fmt->crc.poly != 0x1021u ||
	    fmt->crc.xor_out != 0u || fmt->crc.reflect_in || fmt->crc.reflect_out) {
		return RADIANT_RADIO_ENOTSUP;
	}

	nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);
	nrf_radio_frequency_set(NRF_RADIO, (uint16_t)(2400u + rf_index));

	NRF_RADIO->PCNF0 = 0u;
	NRF_RADIO->PCNF1 =
		((uint32_t)sizeof(radiant_rx_buf) << RADIO_PCNF1_MAXLEN_Pos) |
		((uint32_t)fmt->body_len          << RADIO_PCNF1_STATLEN_Pos) |
		((uint32_t)(fmt->addr_len - 1u)   << RADIO_PCNF1_BALEN_Pos) |
		(RADIO_PCNF1_ENDIAN_Big           << RADIO_PCNF1_ENDIAN_Pos) |
		(RADIO_PCNF1_WHITEEN_Disabled     << RADIO_PCNF1_WHITEEN_Pos);

	/* cover_addr is the core's word for it and SKIPADDR is the hardware's,
	 * with the opposite sense. Getting this inverted is a whole-frame CRC
	 * mismatch, not a subtle one, so the two names are written next to each
	 * other on purpose. */
	NRF_RADIO->CRCCNF =
		(RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
		((uint32_t)(fmt->crc.cover_addr ? RADIO_CRCCNF_SKIPADDR_Include
					       : RADIO_CRCCNF_SKIPADDR_Skip)
			<< RADIO_CRCCNF_SKIPADDR_Pos);
	NRF_RADIO->CRCPOLY = 0x11021u;
	NRF_RADIO->CRCINIT = fmt->crc.init;

	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)radiant_rx_buf;
	return RADIANT_RADIO_OK_RC;
}

/*
 * Programme up to eight receive filters, or refuse the set.
 *
 * See this file's header for why eight is not eight arbitrary addresses. The
 * rule enforced here is exactly the hardware's: logical address 0 is
 * BASE0+AP0, logical 1..7 are BASE1+AP1..AP7, so the set must contain at most
 * two distinct bases and at most one filter may use the odd one out.
 *
 * The refusal is RADIANT_RADIO_ENOTSUP and it is not negotiable. Programming
 * "the first two" and reporting success would lose frames for a reason nothing
 * above this line could ever diagnose.
 */
/* Logical address -> index into the caller's filters[]. See the end of
 * apply_filters() for why the inverse map has to exist. */
static uint8_t radiant_filter_slot[8];

static int apply_filters(const struct radiant_rx_filter *filters, uint8_t n,
			 uint8_t addr_len)
{
	uint32_t base0 = 0, base1 = 0;
	uint8_t  prefixes[8];
	uint8_t  slot_of[8];
	bool     have_base1 = false;
	uint8_t  n_base1 = 0;
	int8_t   lone = -1;

	if (filters == NULL || n == 0u || n > radiant_nrf_caps.max_filters) {
		return RADIANT_RADIO_EINVAL;
	}

	memset(prefixes, 0, sizeof(prefixes));

	/* First pass: everything that shares the majority base goes to BASE1
	 * (logical 1..7); at most one leftover can go to BASE0 (logical 0). */
	for (uint8_t i = 0; i < n; i++) {
		uint32_t b;
		uint8_t  p;

		if (filters[i].addr_len != addr_len) {
			return RADIANT_RADIO_EINVAL;
		}
		pack_address(filters[i].addr, addr_len, &b, &p);

		if (!have_base1) {
			base1 = b;
			have_base1 = true;
		}

		if (b == base1) {
			if (n_base1 >= 7u) {
				/* Eight sharing a base is one more than BASE1's
				 * seven prefixes; the eighth could only go to
				 * logical 0, which needs its own base. */
				if (lone >= 0) {
					return RADIANT_RADIO_ENOTSUP;
				}
				base0 = b;
				slot_of[i] = 0u;
				lone = (int8_t)i;
				continue;
			}
			n_base1++;
			slot_of[i] = n_base1;     /* logical 1..7 */
			prefixes[n_base1] = p;
			continue;
		}

		if (lone >= 0) {
			return RADIANT_RADIO_ENOTSUP;   /* a third base */
		}
		base0 = b;
		prefixes[0] = p;
		slot_of[i] = 0u;
		lone = (int8_t)i;
	}

	if (lone < 0) {
		/* Nothing needed logical 0; leave it unmatchable rather than
		 * aliasing it onto a real address. */
		base0 = ~base1;
		prefixes[0] = (uint8_t)~prefixes[1];
	}

	NRF_RADIO->BASE0   = base0;
	NRF_RADIO->BASE1   = base1;
	NRF_RADIO->PREFIX0 = ((uint32_t)prefixes[0]) |
			     ((uint32_t)prefixes[1] << 8) |
			     ((uint32_t)prefixes[2] << 16) |
			     ((uint32_t)prefixes[3] << 24);
	NRF_RADIO->PREFIX1 = ((uint32_t)prefixes[4]) |
			     ((uint32_t)prefixes[5] << 8) |
			     ((uint32_t)prefixes[6] << 16) |
			     ((uint32_t)prefixes[7] << 24);

	{
		uint32_t mask = 0;

		for (uint8_t i = 0; i < n; i++) {
			mask |= (1u << slot_of[i]);
		}
		NRF_RADIO->RXADDRESSES = mask;
	}

	/*
	 * rx_event.filter_index must index the CALLER's filters[] array, not
	 * the hardware's logical addresses - radiant_search.c recovers
	 * devnum_lo from it and would otherwise recover the wrong byte. The map
	 * is kept for the interrupt handler to invert.
	 */
	for (uint8_t i = 0; i < n; i++) {
		radiant_filter_slot[slot_of[i]] = i;
	}
	return RADIANT_RADIO_OK_RC;
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

static void radio_isr(const void *arg);


int radiant_radio_init(const struct radiant_radio_cbs *cbs, void *user)
{
	const struct device *clk;
	int err;

	if (cbs == NULL || cbs->rx == NULL || cbs->tx == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (radiant_op.inited) {
		return RADIANT_RADIO_ESTATE;
	}

	clk = DEVICE_DT_GET_ONE(nordic_nrf_clock);
	if (!device_is_ready(clk)) {
		LOG_ERR("clock controller not ready");
		return RADIANT_RADIO_EIO;
	}

	/* The RADIO needs the crystal, not the internal oscillator: a 1 Mbps
	 * GFSK link will not hold bit sync on HFINT. Blocking call. */
	err = clock_control_on(clk, CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (err < 0) {
		LOG_ERR("HFXO did not start (%d)", err);
		return RADIANT_RADIO_EIO;
	}

#if defined(NRF54L_ERRATA_20_PRESENT)
	if (nrf54l_errata_20()) {
		nrf_power_task_trigger(NRF_POWER, NRF_POWER_TASK_CONSTLAT);
	}
#endif

	/* 1 MHz, 32-bit, free-running from here until reset. */
	nrf_timer_mode_set(TIMER_ADDR, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(TIMER_ADDR, NRF_TIMER_BIT_WIDTH_32);
	/*
	 * 1 MHz, derived rather than named.
	 *
	 * nrf_timer_frequency_set() does not exist on every part - the nRF54L
	 * TIMERs run from a PCLK that is not 16 MHz and are prescaler-only - so
	 * the prescaler is computed from this instance's own base frequency.
	 * Naming a frequency enum would have compiled on nRF52 and failed to
	 * link on nRF54L, which is how a "portable" backend turns out to be one
	 * part's backend with the other part's #ifdef missing.
	 */
	{
		uint32_t base_hz = NRF_TIMER_BASE_FREQUENCY_GET(TIMER_ADDR);
		uint32_t presc = 0;

		while ((base_hz >> presc) > 1000000u) {
			presc++;
		}
		__ASSERT((base_hz >> presc) == 1000000u,
			 "TIMER base %u Hz cannot be divided to exactly 1 MHz",
			 base_hz);
		nrf_timer_prescaler_set(TIMER_ADDR, presc);
	}
	nrf_timer_task_trigger(TIMER_ADDR, NRF_TIMER_TASK_CLEAR);
	nrf_timer_task_trigger(TIMER_ADDR, NRF_TIMER_TASK_START);

	radiant_clock.base = 0;
	radiant_clock.last = 0;

	/*
	 * Three connections, allocated once and kept: ADDRESS -> capture (the
	 * t_sync path, always live), and the two window edges, which are
	 * enabled and disabled per operation rather than reallocated.
	 */
	if (nrfx_gppi_conn_alloc(
		    nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS),
		    nrf_timer_task_address_get(TIMER_ADDR,
					       nrf_timer_capture_task_get(CC_SYNC)),
		    &radiant_op.conn_sync) < 0 ||
	    nrfx_gppi_conn_alloc(
		    nrf_timer_event_address_get(TIMER_ADDR,
						nrf_timer_compare_event_get(CC_START)),
		    nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_RXEN),
		    &radiant_op.conn_start) < 0 ||
	    nrfx_gppi_conn_alloc(
		    nrf_timer_event_address_get(TIMER_ADDR,
						nrf_timer_compare_event_get(CC_CLOSE)),
		    nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_DISABLE),
		    &radiant_op.conn_close) < 0) {
		LOG_ERR("no free (D)PPI connections");
		return RADIANT_RADIO_EIO;
	}
	radiant_op.conn_ok = true;

	/* The t_sync capture is hardware to hardware with no software in the
	 * path, which is the whole reason this backend owns a TIMER at all, and
	 * it never needs turning off. */
	nrfx_gppi_conn_enable(radiant_op.conn_sync);

	/*
	 * BOTH RADIO interrupt lines on nRF54L, not one.
	 *
	 * That part splits RADIO's interrupt over RADIO_0 and RADIO_1, and
	 * which events land on which line is not something to guess at: connect
	 * one and the wrong one, and every arm succeeds, nothing faults, no
	 * error is logged, and the terminal event simply never arrives - the
	 * channel open hangs waiting for a completion that has nowhere to be
	 * delivered from. That is precisely the symptom this cost, and it looks
	 * identical to "the radio hears nothing".
	 *
	 * The handler is idempotent across lines: it tests each event flag and
	 * clears what it consumes, so being entered twice for one event costs a
	 * few cycles and changes no behaviour.
	 */
	IRQ_CONNECT(RADIANT_RADIO_IRQn, CONFIG_RADIANT_CORE_BACKEND_NRF_IRQ_PRIO,
		    radio_isr, NULL, 0);
#if defined(RADIANT_RADIO_IRQn_2)
	IRQ_CONNECT(RADIANT_RADIO_IRQn_2, CONFIG_RADIANT_CORE_BACKEND_NRF_IRQ_PRIO,
		    radio_isr, NULL, 0);
#endif

	radiant_op.cbs = cbs;
	radiant_op.user = user;
	radiant_op.next_id = 1u;
	radiant_op.kind = OP_NONE;
	radiant_op.inited = true;

	/*
	 * Prove the clock is running before anything depends on it.
	 *
	 * A stopped timer is the quietest possible failure in this backend: every
	 * arm succeeds, every compare is programmed against a counter that never
	 * reaches it, no interrupt ever fires, no terminal event is ever
	 * delivered, and the layer above waits forever with nothing logged. It
	 * is indistinguishable from "the radio hears nothing", and it costs a
	 * board iteration to tell apart. Two microsecond readings 1000 us apart
	 * cost nothing at init and make it a startup line instead.
	 */
	{
		radiant_time_t t0 = radiant_radio_now();
		radiant_time_t t1;

		k_busy_wait(1000);
		t1 = radiant_radio_now();

		LOG_INF("timer @%p, %u us elapsed over a 1000 us wait",
			(void *)TIMER_ADDR, (unsigned)(uint32_t)(t1 - t0));
		if (t1 == t0) {
			LOG_ERR("the radio timebase is not counting - every arm "
				"would be scheduled against a compare the "
				"counter never reaches");
			return RADIANT_RADIO_EIO;
		}
	}
	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_enable(void)
{
	if (!radiant_op.inited) {
		return RADIANT_RADIO_ESTATE;
	}
	if (radiant_op.enabled) {
		return RADIANT_RADIO_OK_RC;
	}

	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_END_MASK |
					NRF_RADIO_INT_DISABLED_MASK);
	irq_enable(RADIANT_RADIO_IRQn);
#if defined(RADIANT_RADIO_IRQn_2)
	irq_enable(RADIANT_RADIO_IRQn_2);
#endif
	radiant_op.enabled = true;
	return RADIANT_RADIO_OK_RC;
}

int radiant_radio_disable(void)
{
	if (!radiant_op.inited) {
		return RADIANT_RADIO_ESTATE;
	}
	if (!radiant_op.enabled) {
		return RADIANT_RADIO_OK_RC;
	}

	(void)radiant_radio_abort();
	irq_disable(RADIANT_RADIO_IRQn);
#if defined(RADIANT_RADIO_IRQn_2)
	irq_disable(RADIANT_RADIO_IRQn_2);
#endif
	nrf_radio_int_disable(NRF_RADIO, ~0u);
	radiant_op.enabled = false;
	return RADIANT_RADIO_OK_RC;
}

/* ---------------------------------------------------------------------------
 * Transmit - BLOCKED, and not by this file
 * ---------------------------------------------------------------------------
 *
 * struct radiant_tx_req cannot express a transmission. It carries fmt,
 * rf_index, power, body, body_len and t_sync_at, and THERE IS NO ADDRESS IN IT.
 * `body` is the tracking-geometry body [ttype][ctrl][d0..d7] that
 * radiant_transfer_build_body() produces; the five on-air address bytes
 * [A6 C5 devnum_lo devnum_hi device_type] are not part of it, and nothing else
 * in the request names them.
 *
 * The contract is asymmetric and that is the whole of the problem: an RX
 * request carries struct radiant_rx_filter with an explicit on-air address,
 * because the receiver has to match one. The TX request carries no counterpart,
 * because - it looks like - the address was implicitly assumed to be "whatever
 * the receiver was last configured for". On this backend that assumption has a
 * name: TXADDRESS selects a logical address whose BASE/PREFIX must already be
 * programmed, so a transmit that follows a receive on a different channel would
 * put the PREVIOUS channel's device number on the air. A frame addressed to the
 * wrong sensor is not a dropped frame; it is a frame another device may accept.
 *
 * This was not findable before now, and that is the point of having written
 * the backend rather than estimated it. Both spikes were receive-only, the mock
 * in radiant_core/tests/fake_radio.c records a TX request without an address
 * and so cannot notice one is missing, and every test above the HAL asserts on
 * body bytes rather than on address bytes.
 *
 * THE FIX IS SMALL AND IT IS NOT MINE TO MAKE HERE. struct radiant_tx_req needs
 * the same two fields struct radiant_rx_filter already has:
 *
 *     const uint8_t *addr;
 *     uint8_t        addr_len;      / * must equal fmt->addr_len * /
 *
 * and the callers already hold the answer - radiant_transfer_cfg carries
 * net_addr[] and struct radiant_channel_id, which is exactly the five bytes.
 * The change touches radiant_radio_hal.h, struct radiant_sched_tx, the two
 * places in radiant_burst.c that fill it, fake_radio.c, and one assertion per
 * transmit test. It is a contract change to a frozen header, so it is a
 * decision rather than a patch, and it belongs in a commit of its own with the
 * tests that would have caught it.
 *
 * Until then this entry point refuses rather than inventing an address. A
 * backend that transmitted to a stale address would pass every test in the tree
 * and be wrong on the air, which is the failure mode this project keeps finding
 * and keeps writing down.
 */
int radiant_radio_tx(const struct radiant_tx_req *req, uint32_t *op)
{
	ARG_UNUSED(req);

	if (op != NULL) {
		*op = 0u;
	}
	if (!radiant_op.inited || !radiant_op.enabled) {
		return RADIANT_RADIO_ESTATE;
	}
	return RADIANT_RADIO_ENOTSUP;
}

/* ---------------------------------------------------------------------------
 * Receive
 * ---------------------------------------------------------------------------
 */

/*
 * How early the receiver must be listening for a frame whose t_sync lands
 * exactly at t_open.
 *
 * t_sync is the end of the address, so the receiver has to have been on for the
 * ramp-up plus the whole preamble and address before that instant. Getting this
 * from the definition rather than from a tuned constant is what stops the
 * window edge from silently carrying one backend's ramp-up - the same class of
 * error the t_sync contract exists to prevent, in the other direction.
 */
static uint32_t rx_lead_us(uint8_t addr_len)
{
	return RAMP_UP_US +
	       (uint32_t)(PREAMBLE_BYTES + addr_len) * US_PER_BYTE;
}

int radiant_radio_rx(const struct radiant_rx_req *req, uint32_t *op)
{
	unsigned int key;
	radiant_time_t now;
	uint32_t start_cc, close_cc;
	int rc;

	if (op == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	*op = 0u;

	if (req == NULL || req->fmt == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (!radiant_op.inited || !radiant_op.enabled) {
		return RADIANT_RADIO_ESTATE;
	}
	if (req->t_close < req->t_open) {
		return RADIANT_RADIO_EINVAL;
	}

	/* One operation in flight. The lock is what resolves the race
	 * radiant_radio_hal.h names explicitly: a thread arming while the
	 * callback that frees the slot is about to run. EBUSY, never
	 * corruption. */
	key = irq_lock();
	if (radiant_op.kind != OP_NONE) {
		irq_unlock(key);
		return RADIANT_RADIO_EBUSY;
	}
	radiant_op.kind = OP_RX;
	irq_unlock(key);

	rc = apply_format(req->fmt, req->rf_index);
	if (rc != RADIANT_RADIO_OK_RC) {
		goto fail;
	}
	rc = apply_filters(req->filters, req->n_filters, req->fmt->addr_len);
	if (rc != RADIANT_RADIO_OK_RC) {
		goto fail;
	}

	now = radiant_radio_now();
	if (req->t_open < now + ARM_LEAD_US + rx_lead_us(req->fmt->addr_len)) {
		/* Never silently late. A window opened after the frame it was
		 * meant to catch is indistinguishable from a sensor that went
		 * quiet, and that is the report nobody can act on. */
		rc = RADIANT_RADIO_ETIME;
		goto fail;
	}

	radiant_op.n_filters       = req->n_filters;
	radiant_op.addr_len        = req->fmt->addr_len;
	radiant_op.body_len        = req->fmt->body_len;
	radiant_op.stop_on_first   = (req->flags & RADIANT_RX_STOP_ON_FIRST) != 0u;
	radiant_op.report_crc_fail = (req->flags & RADIANT_RX_REPORT_CRC_FAIL) != 0u;
	radiant_op.terminal_sent   = false;

	/*
	 * SHORTS keeps the receiver armed across packets with no software in
	 * between, which is what lets one window carry several frames - the
	 * merged-window case radiant_sched.c exists to produce. RSSISTART on
	 * ADDRESS is free and is where rssi_dbm comes from.
	 */
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK |
			     NRF_RADIO_SHORT_END_START_MASK |
			     NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK);

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

	/*
	 * Both edges of the window in hardware. The open is a compare firing
	 * TASKS_RXEN; the close is a compare firing TASKS_DISABLE, held open
	 * long enough for a frame whose t_sync is exactly t_close to finish
	 * arriving - body plus CRC - rather than being cut off mid-packet.
	 */
	start_cc = clock_counter_at(req->t_open -
				    (radiant_time_t)rx_lead_us(req->fmt->addr_len));
	close_cc = clock_counter_at(req->t_close +
				    (radiant_time_t)((req->fmt->body_len + 2u) *
						     US_PER_BYTE));

	nrf_timer_cc_set(TIMER_ADDR, CC_START, start_cc);
	nrf_timer_cc_set(TIMER_ADDR, CC_CLOSE, close_cc);

	/* The endpoints were wired at init; only the enable is per operation. */
	nrfx_gppi_conn_enable(radiant_op.conn_start);
	nrfx_gppi_conn_enable(radiant_op.conn_close);

	radiant_op.id = radiant_op.next_id++;
	if (radiant_op.next_id == 0u) {
		radiant_op.next_id = 1u;   /* ids are non-zero by contract */
	}
	*op = radiant_op.id;
	LOG_DBG("rx op=%u n=%u open=+%d close=+%d isr=%u ev=%u term=%u",
		(unsigned)radiant_op.id, req->n_filters,
		(int)(int64_t)(req->t_open - now), (int)(int64_t)(req->t_close - now),
		(unsigned)radiant_dbg_isr, (unsigned)radiant_dbg_rx_ev,
		(unsigned)radiant_dbg_term);
	return RADIANT_RADIO_OK_RC;

fail:
	LOG_WRN("rx refused: %d (n=%u addr_len=%u)", rc, req->n_filters,
		req->fmt->addr_len);
	radiant_op.kind = OP_NONE;
	return rc;
}

/* ---------------------------------------------------------------------------
 * Abort
 * ---------------------------------------------------------------------------
 */

int radiant_radio_abort(void)
{
	unsigned int key;

	if (!radiant_op.inited) {
		return RADIANT_RADIO_ESTATE;
	}

	key = irq_lock();
	if (radiant_op.kind == OP_NONE) {
		irq_unlock(key);
		return RADIANT_RADIO_OK_RC;
	}

	/* Take the window's hardware edges away first, or the close compare can
	 * re-fire against an operation that has already been retired. */
	nrfx_gppi_conn_disable(radiant_op.conn_start);
	nrfx_gppi_conn_disable(radiant_op.conn_close);
	nrf_radio_shorts_set(NRF_RADIO, 0);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_STOP);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);
	irq_unlock(key);

	/*
	 * The terminal event is NOT delivered from here. It is delivered from
	 * EVENTS_DISABLED in the interrupt handler, with status ABORTED,
	 * because radiant_radio_hal.h guarantees a cancelled operation's
	 * terminal event still arrives and delivering it twice - once here and
	 * once from the ISR that is already on its way - is worse than not
	 * delivering it at all.
	 */
	return RADIANT_RADIO_OK_RC;
}

/* ---------------------------------------------------------------------------
 * The interrupt
 * ---------------------------------------------------------------------------
 */

static void deliver_terminal(enum radiant_radio_status st)
{
	struct radiant_rx_event evt;

	if (radiant_op.terminal_sent) {
		return;
	}
	radiant_op.terminal_sent = true;
	radiant_dbg_term++;

	nrfx_gppi_conn_disable(radiant_op.conn_start);
	nrfx_gppi_conn_disable(radiant_op.conn_close);
	nrf_radio_shorts_set(NRF_RADIO, 0);

	memset(&evt, 0, sizeof(evt));
	evt.op = radiant_op.id;
	evt.status = st;
	evt.t_sync = radiant_radio_now();
	evt.t_sync_exact = false;

	radiant_op.kind = OP_NONE;
	radiant_op.cbs->rx(&evt, radiant_op.user);
}

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);
	radiant_dbg_isr++;

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);

		if (radiant_op.kind == OP_RX && !radiant_op.terminal_sent) {
			struct radiant_rx_event evt;
			bool crc_ok = nrf_radio_crc_status_check(NRF_RADIO);
			uint8_t logical = (uint8_t)(NRF_RADIO->RXMATCH &
						    RADIO_RXMATCH_RXMATCH_Msk);

			radiant_dbg_rx_ev++;
			if (crc_ok || radiant_op.report_crc_fail) {
				memset(&evt, 0, sizeof(evt));
				evt.op = radiant_op.id;
				evt.status = crc_ok ? RADIANT_RADIO_STATUS_OK
						    : RADIANT_RADIO_STATUS_CRC_FAIL;

				/* The hardware capture, corrected by the
				 * constant that is still zero. */
				evt.t_sync = clock_absolute(
					nrf_timer_cc_get(TIMER_ADDR, CC_SYNC)) +
					(radiant_time_t)T_SYNC_CAL_US;
				evt.t_sync_exact = true;

				evt.filter_index =
					(logical < ARRAY_SIZE(radiant_filter_slot))
						? radiant_filter_slot[logical] : 0u;
				evt.body = radiant_rx_buf;
				evt.body_len = radiant_op.body_len;
				evt.has_rssi = true;
				evt.rssi_dbm = (int8_t)(-(int32_t)
					(NRF_RADIO->RSSISAMPLE & 0x7Fu));

				if (radiant_op.stop_on_first) {
					/* This event IS the terminal one. */
					radiant_op.terminal_sent = true;
					nrfx_gppi_conn_disable(radiant_op.conn_start);
					nrfx_gppi_conn_disable(radiant_op.conn_close);
					nrf_radio_shorts_set(NRF_RADIO, 0);
					nrf_radio_task_trigger(NRF_RADIO,
							       NRF_RADIO_TASK_DISABLE);
					radiant_op.kind = OP_NONE;
				}
				radiant_op.cbs->rx(&evt, radiant_op.user);
			}
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

		if (radiant_op.kind == OP_RX) {
			/* The window closed on its own compare, or an abort
			 * disabled it. Both are terminal; TIMEOUT is what a
			 * window that simply ended reports. */
			deliver_terminal(RADIANT_RADIO_STATUS_TIMEOUT);
		}
	}
}
