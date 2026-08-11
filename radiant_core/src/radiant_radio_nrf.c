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
 *   1. THE t_sync CALIBRATION CONSTANTS ARE SEEDED, NOT MEASURED. They were
 *      zero; they are now Zephyr's own per-SoC 1 Mbit chain delays, which its
 *      header calls "based on empirical measurements and sniffer logs" and
 *      which are byte-identical for nRF52840 and nRF54L15. That is a defensible
 *      prior and not the wired two-board trigger radiant_radio_hal.h specifies:
 *      the figures are BLE-1M, at 250 kHz deviation, and ANT-1M is ~170 kHz, so
 *      the receive number in particular carries a different filter bandwidth.
 *      Read the failure mode before assuming it does not matter - a CONSTANT
 *      t_sync error cancels out of the period estimate, so the drift PLL still
 *      locks and nothing looks wrong while every receive window shifts and
 *      yield falls a few tenths of a percent. No error code, no log line. See
 *      T_SYNC_CAL_US.
 *
 *   2. THE RAMP-UP AND TURNAROUND CONSTANTS ARE DATASHEET FIGURES. Transmit is
 *      implemented and works, but nothing has yet measured what this part
 *      actually does antenna-referenced, and radiant_radio_tx() computes its
 *      start instant from those figures. A wrong constant here does not fail
 *      loudly: it puts the frame on the air early or late by that amount, and
 *      a receiver whose window is wide enough will still hear it - which is
 *      exactly why "it transmits" is not evidence that the number is right.
 *      Ramp-up itself is now at least SELF-CONSISTENT rather than assumed:
 *      radiant_radio_init() enables fast ramp-up and verifies the register took
 *      it, which is what makes RAMP_UP_US = 40 true on nRF52840 instead of
 *      about 140. See RAMP_UP_US.
 *
 *   3. EVERY REGISTER FACT IS FROM nRF54L15. The nRF52840 is the shipping part
 *      and the mapping is predicted to hold on it - the two RADIOs differ in
 *      ramp-up and in the TIMING/RXGAIN block, not in the packet engine - but
 *      predicted is not measured. What has been closed by inspection rather
 *      than by a run: fast ramp-up is not the nRF52 reset default and is now
 *      written (item 2); ARM_SETUP_US is per series, because the same arm code
 *      runs at half the clock; the TIMER prescaler solves to exactly 1 MHz from
 *      a 16 MHz PCLK and __ASSERTs if it ever does not; the five-CC requirement
 *      is a compile-time #error, which is what caught the product board having
 *      no radiant,radio-timer chosen node at all. What still needs the part:
 *      the PPI-vs-DPPI routing through the four nrfx_gppi_conn_alloc() calls,
 *      and the errata-153 RSSI table.
 *
 *   4. THE RSSI TEMPERATURE CORRECTION TABLE IS VENDOR-SOURCED, NOT BENCH-
 *      MEASURED. The seven thresholds and the eighth beyond-erratum branch in
 *      "RSSI temperature correction" below are copied from Nordic's own
 *      open-source 802.15.4 driver, not derived from anything on this bench -
 *      this project has not independently confirmed the dB figures against a
 *      calibrated reference at any temperature. It is re-sourced rather than
 *      re-derived because errata 153 is a documented Nordic hardware fact
 *      about the die, not a fact about this bench's antenna or environment,
 *      the same reasoning that already applies to nrf52_erratas.h.
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

#include <stdlib.h>
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
#include <hal/nrf_temp.h>
#include <hal/nrf_timer.h>
#include <helpers/nrfx_gppi.h>

#if defined(NRF54L_ERRATA_20_PRESENT)
#include <hal/nrf_power.h>
#endif

#include <radiant_core/radiant_radio_hal.h>
#include <radiant_core/radiant_radio_nrf_diag.h>

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

/*
 * Compare/capture channel assignment.
 *
 * FIVE, AND THE FIFTH IS NOT AN INDULGENCE. Receive-start and transmit-start
 * have their own compares because they cannot share one on a DPPI part: an
 * event may PUBLISH to exactly one channel, so a second connection from the same
 * compare event is refused outright - nrfx_gppi_conn_alloc() returns -EINVAL
 * when either endpoint is already attached. On the older PPI parts two channels
 * may share an event and one compare would have done; writing it the way that
 * works on both is what keeps this file one backend rather than two.
 *
 * The alternative - detaching and reattaching the event endpoint at every arm -
 * was rejected because the arm path runs in interrupt context inside an 80 us
 * budget, and because an endpoint left attached to the wrong connection is a
 * silent failure of exactly the kind this file keeps paying for.
 */
#define CC_SYNC     0u   /* captured from RADIO EVENTS_ADDRESS by (D)PPI */
#define CC_START    1u   /* fires TASKS_RXEN at the start of an RX window */
#define CC_CLOSE    2u   /* fires TASKS_DISABLE at the end of an RX window */
#define CC_NOW      3u   /* software captures here to read the counter */
#define CC_TXSTART  4u   /* fires TASKS_TXEN */

#if DT_PROP_OR(RADIANT_TIMER_NODE, cc_num, 0) < 5
#error "radiant,radio-timer names a TIMER with fewer than 5 CC channels (or a \
binding with no cc-num). This backend needs five: t_sync capture, RX start, \
window close, software now-capture, and TX start. On nRF52 that means TIMER3 or \
TIMER4; every nRF54L TIMER has at least six."
#endif

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
 * DATASHEET FIGURES, NOT BENCH MEASUREMENTS - see note 2 in this file's header.
 * They are antenna-referenced in the datasheet's own terms, which is the right
 * definition, but nothing on this bench has confirmed them. The two-board
 * trigger that calibrates t_sync is the same rig that would measure these, so
 * they are expected to be corrected together and in one sitting.
 *
 * FAST RAMP-UP IS NOT THE RESET DEFAULT ON nRF52, AND 40 us ASSUMES IT.
 *
 * RADIO->MODECNF0.RU resets to Default, which is the ~140 us path. Nothing in
 * this file used to write it, so on nRF52840 every transmit put its address on
 * the air about 100 us after the t_sync the scheduler asked for, and every
 * receive window entered receive 100 us later than t_open - air_lead_us()
 * intended. The nRF54L15 runs the same code correctly because that part ramps
 * fast unconditionally - which is exactly why a bench that only ever ran on
 * nRF54L15 could not see it.
 *
 * radiant_radio_init() now calls nrf_radio_fast_ramp_up_enable_set(), so 40 us
 * is a fact rather than an aspiration. Corroborated by Zephyr's own BLE
 * controller, whose per-SoC tables give 1 Mbit ramp-up as 40.9 us TX / 40.3 us
 * RX fast and 140.9 / 140.3 default, BYTE-IDENTICAL for nRF52840 and nRF54L15
 * (zephyr/subsys/bluetooth/controller/ll_sw/nordic/hal/nrf5/radio/).
 */
/*
 * CONFIG_SOC_COMPATIBLE_*, NOT CONFIG_SOC_SERIES_* - see the same note on
 * RADIANT_CORE_BACKEND_NRF's `depends on` in radiant_core/Kconfig. Zephyr 4.4
 * (NCS v3.4.0) renamed SOC_SERIES_NRF52X to SOC_SERIES_NRF52 and
 * SOC_SERIES_NRF54LX to SOC_SERIES_NRF54L. Here the failure would at least have
 * been loud - the #else below is an #error - but only if this file were reached
 * at all, and the Kconfig dependency using the same stale names meant it was
 * not. The SOC_COMPATIBLE_ pair is set in both NCS v3.2.4 and v3.4.0.
 */
#if defined(CONFIG_SOC_COMPATIBLE_NRF54LX)
#define RAMP_UP_US             40u
#define RX_TO_TX_US            40u
#define TX_TO_RX_US            40u
/*
 * The software setup cost of an arm call. MEASURED-ADJACENT, NOT MEASURED: this
 * value has run on this part since the backend existed and no arm has been
 * refused RADIANT_RADIO_ETIME on it, which bounds it from below and says
 * nothing about the margin. Nordic's own 802.15.4 driver budgets 185 us TX /
 * 150 us RX for the same job on this series
 * (modules/hal/nordic/drivers/nrf_802154/.../nrf_802154_delayed_trx.c), for a
 * heavier arm path than this one.
 */
#define ARM_SETUP_US           80u
#define CAPS_NAME              "nrf54l RADIO (direct)"
/* nRF54L splits RADIO's interrupt over two lines; RADIO_0 carries the packet
 * events (ADDRESS, END, DISABLED) this backend uses. */
#define RADIANT_RADIO_IRQn     RADIO_0_IRQn
#define RADIANT_RADIO_IRQn_2   RADIO_1_IRQn
#elif defined(CONFIG_SOC_COMPATIBLE_NRF52X)
#define RAMP_UP_US             40u   /* fast ramp-up, enabled at init */
#define RX_TO_TX_US            40u
#define TX_TO_RX_US            40u
/*
 * THE SAME ARM CODE AT HALF THE CLOCK. The nRF52840 is a 64 MHz Cortex-M4; the
 * nRF54L15 is a 128 MHz Cortex-M33. One number for both was the nRF54L15's
 * number silently applied to a part that needs about twice as long, and too
 * small here does not degrade - it refuses every arm with RADIANT_RADIO_ETIME,
 * which is the shape of wedge this file's history already records once.
 *
 * 240 us is bracketed rather than guessed: above 2 x the figure that has run on
 * nRF54L15 (160 us), below Nordic's own 270 us for both directions on
 * NRF52_SERIES in nrf_802154_delayed_trx.c. It is an UPPER BOUND to be
 * tightened by measurement, not a measurement - the instrumentation is the DBG
 * per-window line carrying op, filter count, window edges and the
 * isr/event/ok/terminal counters, and the number to watch is ETIME refusals at
 * zero with margin to spare.
 *
 * Erring large costs 80 us of scheduling slack per window against a 249.7 ms
 * period and cannot produce a refusal the core could not have predicted; erring
 * small costs the board.
 */
#define ARM_SETUP_US           240u
#define CAPS_NAME              "nrf52 RADIO (direct)"
#define RADIANT_RADIO_IRQn     RADIO_IRQn
#else
#error "radiant_radio_nrf: no measured constants for this SoC series. Add them \
with the bench measurement that produced them, or select a different backend."
#endif

/*
 * ARM_SETUP_US is what min_arm_lead_us actually bounds: register programming,
 * (D)PPI wiring, and the compare having to be programmed before the counter
 * reaches it. It is defined PER SERIES above, because it is a CPU-time cost and
 * the two parts do not share a clock. Ramp-up and preamble airtime are NOT in
 * it - they are accounted separately in arm_tx(), because the HAL defines
 * t_sync at the end of the address rather than at the start of transmission and
 * mixing the two is how one backend's ramp-up ends up baked into a core
 * constant.
 *
 * radiant_transfer.h checks its 1310 us reply-window lead against this, so a
 * number that is too large here fails acknowledged data at configuration time
 * rather than on the air. That is the right direction to be wrong in, and the
 * worst case below - 240 + 40 + 48 = 328 us - has a factor of four of room.
 */

/*
 * min_arm_lead_us MUST cover everything the backend needs before a frame's
 * t_sync, and an earlier revision of this file deliberately excluded ramp-up
 * and preamble/address airtime from it - "accounted separately in arm_rx()".
 * That was wrong, and wrong in the way that wedges a board.
 *
 * The core has exactly one number to lead by. radiant_api.c posts its search
 * window at `now + caps->min_arm_lead_us`; if arm_rx() then demands more than
 * it advertised, EVERY arm is refused RADIANT_RADIO_ETIME. A refused arm
 * completes synchronously, the completion drives another post, and the board
 * wedges - which is exactly what it did: nothing on the air, no error the host
 * could see, and the stack walked down to within 408 bytes of PSPLIM.
 *
 * So the advertised lead is the real one: software setup, plus ramp-up, plus
 * the preamble and the LONGEST address this backend will be asked to receive.
 * Advertising the worst case costs a few tens of microseconds of scheduling
 * slack and cannot produce a refusal the core could not have predicted.
 */
#define ARM_LEAD_US            (ARM_SETUP_US + RAMP_UP_US + \
				(PREAMBLE_BYTES + RADIANT_RADIO_ADDR_MAX) * US_PER_BYTE)

static const struct radiant_radio_caps radiant_nrf_caps = {
	.name                = CAPS_NAME,

	/* Eight logical addresses. See this file's header for the constraint
	 * that number does not express: at most two distinct BASEs - which is
	 * what max_addr_groups below now expresses, so the scheduler stops
	 * building sets apply_filters() has to refuse. */
	.max_filters         = 8,
	/* Logical 0 is BASE0 + AP0; logical 1..7 are BASE1 + AP1..AP7. Two
	 * bases, and on the 5-byte tracking format the device number is inside
	 * the base. */
	.max_addr_groups     = 2,
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
	 * T_SYNC_CAL_US, which is now seeded from Zephyr's per-SoC chain delays
	 * rather than zero, and is still a prior rather than a measurement. */
	.has_sync_timestamp  = true,
	.has_rssi            = true,
	/*
	 * Energy detect, and it tracks the Kconfig rather than the hardware.
	 *
	 * The capability is a compile-time property of the backend BUILD - the
	 * HAL says so of caps.phys and says the same here - and this build
	 * compiles no sampling path at all without CONFIG_RADIANT_CORE_ED_SCAN.
	 * Advertising true and then refusing every arm would be the one thing
	 * the capability query exists to prevent: core policy testing a
	 * capability, believing it, and finding out on the air.
	 */
	.has_ed_scan         = IS_ENABLED(CONFIG_RADIANT_CORE_ED_SCAN),
	.crc_in_hw           = true,
	/* RXCRC holds the CRC as received, not merely CRCSTATUS's pass/fail
	 * bit, so the core can form an error syndrome and repair a single
	 * flipped bit. The register is documented on both nRF52 and nRF54L. */
	.has_rx_crc          = true,

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
 * SEEDED FROM ZEPHYR'S BLE CONTROLLER, WHICH IS A DEFENSIBLE PRIOR AND NOT A
 * MEASUREMENT. It was zero, and zero was worse: read radiant_radio_hal.h's "THE
 * FAILURE MODE, WHICH IS SILENT" before assuming a wrong constant announces
 * itself. A constant error cancels out of the period estimate, so the drift PLL
 * still locks and nothing looks broken while every window sits off-centre and
 * yield drops by a few tenths of a percent - the same order as this bench's
 * characterised ~0.4 % collision floor.
 *
 * Provenance, so the number can be argued with rather than merely trusted.
 * Nordic ships per-SoC chain delays their own header calls "based on empirical
 * measurements and sniffer logs", in
 * zephyr/subsys/bluetooth/controller/ll_sw/nordic/hal/nrf5/radio/. For 1 Mbit -
 * the only PHY this project uses - nRF52840 and nRF54L15 carry BYTE-IDENTICAL
 * values:
 *
 *     RX chain delay, 1M: 9400 ns  (the capture is LATE  vs the antenna)
 *     TX chain delay, 1M:  600 ns  (the event  is EARLY  vs the antenna)
 *
 * Zephyr applies them with exactly the sign convention these constants want -
 * see lll_adv_aux.c, where chain delay converts an event-referenced timer value
 * to an antenna-referenced one.
 *
 * THE CAVEAT, WRITTEN DOWN RATHER THAN SKIPPED: these are BLE-1M figures -
 * 250 kHz deviation, measured for the BLE packet engine - and ANT-1M is about
 * 170 kHz. A different RX filter bandwidth means a different group delay, so
 * the receive number in particular is a prior and not a substitute for the
 * wired two-board trigger the HAL contract specifies. It is landed as a seed
 * carrying its provenance; the bench measurement remains the confirmation.
 *
 * The transmit half is nearly free to confirm and should be confirmed first:
 * radiant_dbg_tx_err reads achieved minus requested t_sync every frame, it read
 * -2 us on nRF54L15 with both constants at zero, and a correct
 * T_SYNC_CAL_TX_US is exactly where that residual lives.
 */
#define T_SYNC_CAL_US   (-9)   /* -9.4 us, rounded toward zero */

/*
 * The same thing for transmit, and it is a SEPARATE constant on purpose.
 *
 * On receive the error is demodulator group delay and filter latency. On
 * transmit it is modulator and PA delay, in the other direction, through
 * entirely different silicon. They are not the same number and there is no
 * reason to expect them to be close - 9.4 us against 0.6 us, a factor of
 * fifteen - so sharing one constant between them would bake an assumption in at
 * the exact point where nothing would ever check it.
 */
#define T_SYNC_CAL_TX_US   (1)   /* +0.6 us, rounded away from zero */

/*
 * radiant_radio_tx() folds this into an unsigned lead, so a negative value
 * would wrap to about 4295 seconds and refuse every transmit with ETIME. It is
 * positive on both parts and there is no physical reason for a transmit chain
 * delay to be negative - the antenna cannot emit before the modulator - but the
 * failure if one were ever written here is spectacular and silent at the point
 * of edit, so it is caught at compile time instead.
 */
BUILD_ASSERT(T_SYNC_CAL_TX_US >= 0,
	     "T_SYNC_CAL_TX_US is folded into an unsigned lead in "
	     "radiant_radio_tx(); a negative value wraps");

/*
 * Apply a calibration to a captured instant, in signed microseconds.
 *
 * radiant_time_t is unsigned and these constants are negative, so the addition
 * is spelled out rather than left to a cast: `captured + (radiant_time_t)(-9)`
 * happens to produce the right answer by modular arithmetic and reads like a
 * bug at every future review.
 */
static radiant_time_t t_sync_cal(radiant_time_t captured, int32_t cal_us)
{
	if (cal_us >= 0) {
		return captured + (radiant_time_t)cal_us;
	}
	return (captured > (radiant_time_t)(-cal_us))
		       ? (captured - (radiant_time_t)(-cal_us))
		       : 0u;
}

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

enum op_kind { OP_NONE = 0, OP_TX, OP_RX, OP_ED };

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
	 * Whether this receive window has delivered a frame - of either status.
	 *
	 * It exists for the noise floor and for nothing else. A window that
	 * heard nothing is a measurement of what the band sounds like when
	 * nobody is transmitting; a window that heard something is a
	 * measurement of whoever transmitted, and RSSISAMPLE by then holds the
	 * packet's own level rather than the band's. One bool separates the two
	 * populations, and without it every noise sample from a busy channel
	 * would be a transmitter's signal strength wearing the wrong name.
	 */
	bool         rx_any;

	/*
	 * The t_sync a transmit ASKED for, kept so the interrupt can compare it
	 * against the t_sync the hardware actually captured.
	 *
	 * That difference is the whole of this backend's transmit timing error -
	 * ramp-up plus T_SYNC_CAL_TX_US - measured on the part rather than taken
	 * from the datasheet, and it costs one word and one subtraction. It is
	 * worth having because a transmit that is late by a fixed amount is
	 * invisible on broadcast, where the receiver's window is +/-400 us wide
	 * and re-anchors on every frame, and fatal on an acknowledged exchange,
	 * where the peer opens a narrow window 1560 us after the packet it sent.
	 */
	radiant_time_t t_sync_req;

#if defined(CONFIG_RADIANT_CORE_ED_SCAN)
	/*
	 * The energy-detect sweep in flight.
	 *
	 * ed_deadline_cc is a RAW TIMER COUNTER VALUE rather than a
	 * radiant_time_t, and deliberately: it is compared inside the sampling
	 * loop, which runs in the radio interrupt, and clock_absolute() takes
	 * an interrupt lock and a fold on every call. A 32-bit compare against
	 * a captured counter is the same arithmetic radio_disable_now() already
	 * uses for its own bound, and it cannot be wrong for anything inside
	 * 35 minutes of the last fold.
	 *
	 * It advances by one dwell per index rather than being recomputed, so
	 * the whole sweep is bounded by n * dwell measured from the operation's
	 * own start. An index that finishes its burst early hands the unused
	 * remainder to the indices after it and the sweep still cannot exceed
	 * what the scheduler sized the gap for - which is the promise the gate
	 * for this phase rests on.
	 */
	/* No ed_lo: the sweep never wraps inside one operation - the scheduler
	 * splits a range that would - so the low end is only ever the value
	 * ed_cur starts at, and keeping a second copy of it would be state that
	 * could disagree with itself. */
	uint8_t  ed_hi;
	uint8_t  ed_cur;
	uint32_t ed_dwell_us;
	uint32_t ed_deadline_cc;
#endif

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
	nrfx_gppi_handle_t conn_start;    /* CC_START -> TASKS_RXEN */
	nrfx_gppi_handle_t conn_start_tx; /* CC_START -> TASKS_TXEN */
	nrfx_gppi_handle_t conn_close;
	/*
	 * RXREADY -> TASKS_RSSISTART: one RSSI sample the instant the receiver
	 * is live, before anything can have transmitted into the window.
	 *
	 * The existing ADDRESS -> RSSISTART short samples when a packet arrives,
	 * which means a window that hears nothing never samples at all - and a
	 * window that hears nothing is precisely the one whose level is the
	 * noise floor. RXREADY rather than the start compare because the sample
	 * has to be taken after ramp-up: at the compare the receiver is not on
	 * yet and the number would be meaningless.
	 *
	 * There is no contention with the per-packet value. RSSISTART takes one
	 * shot, so a window that goes on to receive something overwrites this
	 * sample with the packet's own on ADDRESS - and that window is not one
	 * the noise floor wants a sample from anyway.
	 */
	nrfx_gppi_handle_t conn_rssi;
	bool               conn_ok;
} radiant_op;

/* Bring-up counters. Logging from the RADIO interrupt is not safe at this
 * priority, so the interrupt counts and thread context reports. */
static volatile uint32_t radiant_dbg_isr;
static volatile uint32_t radiant_dbg_rx_ev;
static volatile uint32_t radiant_dbg_term;
static volatile uint32_t radiant_dbg_ok;
/* Achieved t_sync minus requested t_sync on the last transmit, microseconds. */
static volatile int32_t  radiant_dbg_tx_err;

/* The DMA buffer the RADIO reads and writes. Backend-owned, handed to the core
 * as rx_event.body for the duration of one callback only, which is exactly what
 * radiant_radio_hal.h says about it. */
static uint8_t radiant_rx_buf[RADIANT_RADIO_BODY_MAX + 4] __aligned(4);

/*
 * The transmit buffer, and why the body is copied into it rather than DMA'd
 * from where the core put it.
 *
 * radiant_radio_hal.h permits either: it requires the caller's body to stay
 * valid until the completion callback precisely so that a backend MAY point
 * EasyDMA at it. Copying ten bytes costs nothing against this backend's 80 us
 * setup budget, and it buys the one guarantee the contract does not make - that
 * the buffer is somewhere this RADIO's EasyDMA can actually reach, correctly
 * aligned, and not in flash. The core's transmit bodies live in per-channel
 * structures today and that is fine; it is not a property the HAL states, so it
 * is not one this file should depend on.
 */
static uint8_t radiant_tx_buf[RADIANT_RADIO_BODY_MAX + 4] __aligned(4);

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

	/* PACKETPTR is deliberately NOT set here. It is the one register in this
	 * function whose value depends on the direction rather than on the
	 * format, and leaving it to the two arm paths is what stops a transmit
	 * from DMA-ing out of the receive buffer. */
	return RADIANT_RADIO_OK_RC;
}

/*
 * Transmit power.
 *
 * THE REGISTER FIELD IS NOT dBm, AND ON ONE OF THESE TWO PARTS IT LOOKS AS IF
 * IT IS. nRF52840 encodes +8 dBm as 0x08 and 0 dBm as 0x00 - signed dBm, near
 * enough that `TXPOWER = (int8_t)dbm` passes a code review. nRF54L15 encodes
 * +8 dBm as 0x3F and 0 dBm as 0x18. The same expression on that part transmits
 * at some unrelated power with no error anywhere, and a transmit-power bug is
 * invisible on a bench two feet across.
 *
 * So the mapping goes through the part's own MDK symbols and never through the
 * number. Every entry below is defined by both nRF52840 and nRF54L15, which is
 * why the table needs no per-series #if - and a part that does not define one of
 * them fails to compile here rather than transmitting at a guess.
 */
struct radiant_txp {
	int8_t              dbm;
	nrf_radio_txpower_t reg;
};

static const struct radiant_txp radiant_txp_table[] = {
	{   8, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos8dBm  },
	{   7, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos7dBm  },
	{   6, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos6dBm  },
	{   5, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos5dBm  },
	{   4, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos4dBm  },
	{   3, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos3dBm  },
	{   2, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos2dBm  },
	{   0, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_0dBm     },
	{  -4, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Neg4dBm  },
	{  -8, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Neg8dBm  },
	{ -12, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Neg12dBm },
	{ -16, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Neg16dBm },
	{ -20, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Neg20dBm },
	{ -40, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Neg40dBm },
};

/*
 * radiant_radio_hal.h: "A backend rounds to the nearest setting it has and does
 * not fail for an unreachable value - the link budget does not care about half a
 * dB, and failing here would make a shared scheduler brittle." So this never
 * returns an error. use_raw goes straight to the register, which is what makes a
 * bench power sweep possible without a HAL change; it is by definition not
 * portable and nothing in the core may set it.
 */
static void apply_power(const struct radiant_tx_power *p)
{
	size_t best = 0;
	int    best_err;

	if (p->use_raw) {
		NRF_RADIO->TXPOWER = p->raw;
		return;
	}

	best_err = abs((int)p->dbm - (int)radiant_txp_table[0].dbm);
	for (size_t i = 1; i < ARRAY_SIZE(radiant_txp_table); i++) {
		int err = abs((int)p->dbm - (int)radiant_txp_table[i].dbm);

		if (err < best_err) {
			best_err = err;
			best = i;
		}
	}
	nrf_radio_txpower_set(NRF_RADIO, radiant_txp_table[best].reg);
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
				/*
				 * Eight sharing a base is one more than BASE1's
				 * seven prefixes, so the eighth goes to logical
				 * 0 - which carries its own base as well as its
				 * own prefix.
				 *
				 * prefixes[0] is set HERE and forgetting it was
				 * a real bug with a very specific signature: the
				 * eighth filter of every full set silently
				 * matched prefix 0x00 instead of the address
				 * asked for. A wildcard sweep enumerates
				 * devnum_lo in blocks of eight, so it was
				 * exactly one device number in every eight that
				 * could never be found - and the bench sensor,
				 * #14871, has devnum_lo 0x17, the eighth of its
				 * block. The sweep looked perfect in the log,
				 * the window opened, and the one address that
				 * mattered was not on the air.
				 */
				if (lone >= 0) {
					return RADIANT_RADIO_ENOTSUP;
				}
				base0 = b;
				prefixes[0] = p;
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

/*
 * Programme the one address a transmit emits.
 *
 * TXADDRESS names a LOGICAL address, not an address: it is an index into the
 * same BASE/PREFIX registers apply_filters() writes, so a transmit that named no
 * address of its own would emit whatever the previous operation happened to
 * leave loaded there. That is the whole reason struct radiant_tx_req grew an
 * addr field, and it is why this function writes BASE0 and PREFIX0.AP0
 * unconditionally rather than looking for a slot that already matches.
 *
 * Logical 0 rather than one of 1..7 because it is the only one with a base of
 * its own; logical 1..7 all share BASE1, and reusing one of those would make the
 * transmitted address depend on the last receive window's majority base.
 */
static void apply_tx_address(const uint8_t *addr, uint8_t addr_len)
{
	uint32_t base;
	uint8_t  prefix;

	pack_address(addr, addr_len, &base, &prefix);

	NRF_RADIO->BASE0 = base;
	NRF_RADIO->PREFIX0 = (NRF_RADIO->PREFIX0 & ~0xFFu) | (uint32_t)prefix;
	nrf_radio_txaddress_set(NRF_RADIO, 0u);
}

/*
 * How much air there is between switching the radio on and t_sync.
 *
 * t_sync is the end of the address, so a receiver has to have been listening,
 * and a transmitter has to have started transmitting, for the ramp-up plus the
 * whole preamble and address before that instant. Both directions want the same
 * number and neither wants a tuned constant: deriving it from the definition is
 * what stops the window edge - or the transmit start - from silently carrying
 * one backend's ramp-up, which is the class of error the t_sync contract in
 * radiant_radio_hal.h exists to prevent.
 */
static uint32_t air_lead_us(uint8_t addr_len)
{
	return RAMP_UP_US +
	       (uint32_t)(PREAMBLE_BYTES + addr_len) * US_PER_BYTE;
}

/* ---------------------------------------------------------------------------
 * RSSI temperature correction - errata 153 (nRF52840) / 225 (nRF52833)
 *
 * Re-sourced, not re-derived. This is NOT sdk-ant: it is Nordic's own
 * open-source 802.15.4 driver, BSD-3-Clause, shipped inside NCS and already a
 * dependency the same way nrf52_erratas.h is -
 * modules/hal/nordic/drivers/nrf_802154/driver/src/nrf_802154_rssi.c,
 * nrf_802154_rssi_sample_temp_corr_value_get(), whose own comment names the
 * erratum: "Implementation based on Errata 153 for nRF52840 SoC and Errata
 * 225 for nRF52833 SoCs." The seven thresholds and the eighth,
 * beyond-the-erratum branch below are copied from that function's table
 * verbatim; nothing here is this project's own measurement, and that is
 * stated because every other correction constant in this file is measured or
 * is explicitly flagged as a datasheet figure - see this file's header. The
 * one thing re-derived rather than copied is the sign: that function corrects
 * a positive register MAGNITUDE (`sample + corr`), this file works in
 * negated dBm, and RSSISAMPLE's magnitude convention is the same either way,
 * so `corrected_dbm = raw_dbm - corr`. Nordic's own accessors apply the
 * opposite sign to a THRESHOLD (`cca_ed - corr`, note the same minus) -
 * correcting a measured sample and a threshold it is compared against in the
 * same direction double-counts the correction, which is the mistake this
 * comment exists to head off in any code that later compares evt.rssi_dbm
 * against a configured threshold (radiant_search.c's cfg.min_rssi_dbm is not
 * itself temperature-corrected, so no such double-counting exists today).
 *
 * NOT PRESENT ON nRF54L. nrf_802154_rssi.c implements NRF52_SERIES (this
 * table) and NRF53_SERIES (errata 87, an unrelated cubic polynomial) and
 * returns a flat 0 for every other part, including nRF54L. This bench's own
 * part is nRF54L15 (see this file's header), so the correction below compiles
 * in for nRF52840/nRF52833 only and this bench's RSSI stays uncorrected,
 * matching Nordic's own choice rather than inventing a number for a part
 * nobody has characterised it on.
 *
 * radiant_radio_nrf_die_temp_c() is declared in radiant_radio_nrf_diag.h,
 * included at the top of this file, and defined further down: the handler
 * below only needs its prototype, and the definition needs nothing from here.
 */
#if defined(CONFIG_SOC_NRF52840) || defined(CONFIG_SOC_NRF52833)
#define RSSI_TEMP_CORR_PRESENT 1
#else
#define RSSI_TEMP_CORR_PRESENT 0
#endif

#if RSSI_TEMP_CORR_PRESENT

static int8_t rssi_temp_corr_get(int8_t temp_c)
{
	if (temp_c <= -30) {
		return 3;
	}
	if (temp_c <= -10) {
		return 2;
	}
	if (temp_c <= 10) {
		return 1;
	}
	if (temp_c <= 30) {
		return 0;
	}
	if (temp_c <= 50) {
		return -1;
	}
	if (temp_c <= 70) {
		return -2;
	}
	if (temp_c <= 85) {
		return -3;
	}
	/* Beyond the erratum's own table. Nordic's comment: "nRF52840 cannot
	 * work for a temperature above 85 degrees Celsius, so this part won't
	 * affect its operation, even if it isn't present in its errata." */
	return -4;
}

/*
 * Cached, not sampled per packet - see radiant_radio_nrf_die_temp_c()'s own
 * header on why a TEMP conversion cannot run in the radio ISR. 20 degC is not
 * a guess: it is nrf_802154_temperature_zephyr.c's own DEFAULT_TEMPERATURE,
 * the value that function reports before its first real reading, and it is
 * also the centre of this table's zero-correction band, so an unrefreshed
 * cache costs nothing rather than costing an unknown amount.
 */
static int8_t rssi_temp_cache_c = 20;

/* 60 s, matching CONFIG_NRF_802154_TEMPERATURE_UPDATE_PERIOD's own default
 * for the identical job. Die temperature does not move fast enough for this
 * table - a full band is 20 degC wide - to need a shorter one. */
#define RSSI_TEMP_UPDATE_PERIOD_MS 60000u

static struct k_work_delayable rssi_temp_work;

static void rssi_temp_work_handler(struct k_work *work)
{
	int16_t centi_c;

	ARG_UNUSED(work);

	if (radiant_radio_nrf_die_temp_c(&centi_c) == RADIANT_RADIO_OK_RC) {
		rssi_temp_cache_c = (int8_t)(centi_c / 100);
	}
	/* A failed read leaves the cache at its last good value rather than
	 * resetting to 20 degC, which would be a silent step change in every
	 * RSSI report that followed it. */
	k_work_reschedule(&rssi_temp_work, K_MSEC(RSSI_TEMP_UPDATE_PERIOD_MS));
}

#endif /* RSSI_TEMP_CORR_PRESENT */

/*
 * RSSISAMPLE, in dBm, corrected.
 *
 * THE ONLY PLACE THIS REGISTER IS READ, and that is the point of the function
 * rather than a tidiness argument. radiant_radio_hal.h requires rssi_dbm,
 * noise_dbm and now the energy-detect min/mean to be on ONE SCALE - same
 * corrections, same reference - because the whole use of the numbers is
 * subtracting them from each other: a margin, or "RF 26 is 14 dB quieter than
 * RF 57". Three copies of a negation and a temperature term is three chances
 * for one of them to be right and the comparison to be wrong by the difference,
 * which looks entirely plausible in a log.
 *
 * The register holds a POSITIVE MAGNITUDE in its low seven bits and the value
 * wanted is negative dBm, hence the negation; see the errata block above for
 * the sign derivation of the correction itself.
 */
static int8_t rssi_sample_dbm(void)
{
	int8_t raw_dbm = (int8_t)(-(int32_t)(NRF_RADIO->RSSISAMPLE & 0x7Fu));

#if RSSI_TEMP_CORR_PRESENT
	/* raw_dbm - corr, not + corr. */
	return (int8_t)(raw_dbm - rssi_temp_corr_get(rssi_temp_cache_c));
#else
	return raw_dbm;
#endif
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

static void radio_isr(const void *arg);
/* Both are defined with the interrupt handler, because that is where the
 * reasoning about EVENTS_DISABLED lives; radiant_radio_abort() needs them
 * earlier. See radio_disable_now() for why an abort may not simply leave the
 * event in flight. */
static void radio_disable_now(void);
static void deliver_terminal(enum radiant_radio_status st);


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

	/*
	 * FAST RAMP-UP. See RAMP_UP_US - without this, every timing constant in
	 * this file is 100 us wrong on nRF52840 and exactly right on nRF54L15,
	 * which is the worst possible distribution of a bug across a bench and a
	 * product board.
	 *
	 * GUARDED, not unconditional, and the guard is the nrfx symbol rather
	 * than CONFIG_SOC_SERIES_*. nrfx only DECLARES this helper under
	 * `RADIO_MODECNF0_RU_Msk || RADIO_TIMING_RU_Msk`, and no header in
	 * NCS v3.2.4 or v3.4.0 defines RADIO_TIMING_RU_Msk for any part - so an
	 * unconditional call is a compile error on nRF54L15 today, and becomes
	 * correct there by itself if a future SDK adds the register. Testing for
	 * the symbol tracks that exactly; testing for the SoC series would not.
	 *
	 * Safe on this part: Zephyr's BLE controller writes RADIO_MODECNF0_RU_Fast
	 * on nRF52 under CONFIG_BT_CTLR_RADIO_ENABLE_FAST with no errata guard of
	 * any kind (radio.c, radio_phy_set()), and skips MODECNF0 entirely on
	 * nRF54LX. No nRF52840 erratum touches MODECNF0.RU.
	 */
#if defined(RADIO_MODECNF0_RU_Msk) || defined(RADIO_TIMING_RU_Msk)
	nrf_radio_fast_ramp_up_enable_set(NRF_RADIO, true);
	if (!nrf_radio_fast_ramp_up_check(NRF_RADIO)) {
		/* Loud, because the alternative is a link that works and is
		 * 100 us out of phase on every frame with nothing to show
		 * for it. */
		LOG_ERR("fast ramp-up did not take; every timing constant in "
			"this backend is now ~100 us optimistic");
		return RADIANT_RADIO_EIO;
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
	 * Five connections, allocated once and kept: ADDRESS -> capture (the
	 * t_sync path, always live), the three window edges, which are enabled
	 * and disabled per operation rather than reallocated, and
	 * RXREADY -> RSSISTART for the noise floor.
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
						nrf_timer_compare_event_get(CC_TXSTART)),
		    nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_TXEN),
		    &radiant_op.conn_start_tx) < 0 ||
	    nrfx_gppi_conn_alloc(
		    nrf_timer_event_address_get(TIMER_ADDR,
						nrf_timer_compare_event_get(CC_CLOSE)),
		    nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_DISABLE),
		    &radiant_op.conn_close) < 0 ||
	    nrfx_gppi_conn_alloc(
		    nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_RXREADY),
		    nrf_radio_task_address_get(NRF_RADIO,
					       NRF_RADIO_TASK_RSSISTART),
		    &radiant_op.conn_rssi) < 0) {
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

	/*
	 * One die-temperature reading, at the one point in this file's
	 * lifecycle guaranteed to be thread context and guaranteed to run on
	 * every boot: the confound made visible on the log every bench
	 * capture already records (see radiant_radio_nrf_diag.h), and on
	 * nRF52840/nRF52833 also the seed for the errata-153/225 RSSI
	 * correction cache below - real from the first packet rather than
	 * from the first RSSI_TEMP_UPDATE_PERIOD_MS refresh.
	 */
	{
		int16_t centi_c;

		if (radiant_radio_nrf_die_temp_c(&centi_c) == RADIANT_RADIO_OK_RC) {
			LOG_INF("die temperature at init: %d.%02u degC",
				centi_c / 100, (unsigned)abs(centi_c % 100));
#if RSSI_TEMP_CORR_PRESENT
			rssi_temp_cache_c = (int8_t)(centi_c / 100);
#endif
		}
	}

#if RSSI_TEMP_CORR_PRESENT
	k_work_init_delayable(&rssi_temp_work, rssi_temp_work_handler);
	k_work_schedule(&rssi_temp_work, K_MSEC(RSSI_TEMP_UPDATE_PERIOD_MS));
#endif

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
 * Transmit
 * ---------------------------------------------------------------------------
 *
 * This entry point returned RADIANT_RADIO_ENOTSUP for as long as struct
 * radiant_tx_req had no address in it, and the refusal was the right answer at
 * the time. The request carried fmt, rf_index, power, body, body_len and
 * t_sync_at; `body` is the tracking-geometry body [ttype][ctrl][d0..d7] that
 * radiant_transfer_build_body() produces, and the five on-air address bytes
 * [A6 C5 devnum_lo devnum_hi device_type] were nowhere in the request at all.
 *
 * The contract was asymmetric and that was the whole of the problem: an RX
 * request carries struct radiant_rx_filter with an explicit on-air address,
 * because the receiver has to match one. The TX request carried no counterpart,
 * because - it looks like - the address was implicitly assumed to be "whatever
 * the receiver was last configured for". On this backend that assumption has a
 * name: TXADDRESS selects a logical address whose BASE/PREFIX must already be
 * programmed, so a transmit that followed a receive on a different channel would
 * have put the PREVIOUS channel's device number on the air. A frame addressed to
 * the wrong sensor is not a dropped frame; it is a frame another device may
 * accept.
 *
 * It was not findable before the first real backend, and that is the point of
 * having written one rather than estimated it. Both spikes were receive-only,
 * the mock in radiant_core/tests/fake_radio.c recorded a TX request without an
 * address and so could not notice one was missing, and every test above the HAL
 * asserted on body bytes rather than on address bytes. The fix added addr and
 * addr_len to struct radiant_tx_req and to struct radiant_sched_tx, and made
 * both the mock and radiant_sched_request_tx() refuse a transmit whose addr_len
 * disagrees with its format - so the requirement is now stated in three places
 * that a build has to satisfy, rather than assumed in none.
 */
int radiant_radio_tx(const struct radiant_tx_req *req, uint32_t *op)
{
	unsigned int   key;
	radiant_time_t now;
	radiant_time_t t_start;
	uint32_t       lead;
	int            rc;

	if (op == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	*op = 0u;

	if (req == NULL || req->fmt == NULL || req->body == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (!radiant_op.inited || !radiant_op.enabled) {
		return RADIANT_RADIO_ESTATE;
	}
	/* No address, no transmit - never an inherited one. See the note above. */
	if (req->addr_len != req->fmt->addr_len ||
	    req->addr_len < 2u || req->addr_len > RADIANT_RADIO_ADDR_MAX) {
		return RADIANT_RADIO_EINVAL;
	}
	if (req->body_len == 0u || req->body_len > RADIANT_RADIO_BODY_MAX) {
		return RADIANT_RADIO_EINVAL;
	}
	if (req->t_sync_at == RADIANT_TIME_NEVER) {
		return RADIANT_RADIO_EINVAL;
	}

	/* One operation in flight, resolved the same way arm_rx does. */
	key = irq_lock();
	if (radiant_op.kind != OP_NONE) {
		irq_unlock(key);
		return RADIANT_RADIO_EBUSY;
	}
	radiant_op.kind = OP_TX;
	irq_unlock(key);

	rc = apply_format(req->fmt, req->rf_index);
	if (rc != RADIANT_RADIO_OK_RC) {
		goto fail;
	}
	/* apply_format has already refused anything but RADIANT_LEN_FIXED, so
	 * the format's own length is the only length this frame may have. A
	 * backend cannot fix a disagreement up: it DMAs the bytes it is given
	 * and STATLEN decides how many leave the antenna. */
	if (req->body_len != req->fmt->body_len) {
		rc = RADIANT_RADIO_EINVAL;
		goto fail;
	}

	apply_tx_address(req->addr, req->addr_len);
	apply_power(&req->power);

	memcpy(radiant_tx_buf, req->body, req->body_len);
	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)radiant_tx_buf;

	/*
	 * Working backwards from t_sync, which is the whole reason the HAL
	 * defines TX in those terms: the caller names the instant the last
	 * address bit must be at the antenna, and the backend - not the core -
	 * subtracts its own ramp-up and preamble. A master's period is then
	 * exact by construction and carries none of this part's constants.
	 */
	/*
	 * THE CALIBRATION IS ON BOTH SIDES, and it has to be.
	 *
	 * Until both constants were zero, calibration was applied only to the
	 * REPORTED t_sync, in the RADIO ISR, and not here. That is invisible at
	 * zero and wrong the moment it is not: the caller asks for an
	 * antenna-referenced instant, this path would place an event-referenced
	 * one, and the reported value would then differ from the scheduled one
	 * by exactly the calibration - a self-consistent stack that is
	 * uniformly off the air.
	 *
	 * TASKS_TXEN therefore fires one T_SYNC_CAL_TX_US earlier than the
	 * event-referenced arithmetic alone would put it, so that the ADDRESS
	 * event lands at t_sync_at - cal and the ANTENNA lands at t_sync_at.
	 */
	lead = air_lead_us(req->addr_len) + (uint32_t)T_SYNC_CAL_TX_US;
	t_start = req->t_sync_at - (radiant_time_t)lead;

	now = radiant_radio_now();
	if (req->t_sync_at < now + lead) {
		/* Never silently late. A late master frame lands in the next
		 * slot and is worse than no frame at all. */
		rc = RADIANT_RADIO_ETIME;
		goto fail;
	}

	radiant_op.addr_len        = req->addr_len;
	radiant_op.body_len        = req->body_len;
	radiant_op.stop_on_first   = false;
	radiant_op.report_crc_fail = false;
	radiant_op.terminal_sent   = false;
	radiant_op.t_sync_req      = req->t_sync_at;

	/*
	 * READY_START begins the transmission the instant ramp-up completes, so
	 * nothing between the compare and the antenna is software - which is the
	 * whole point of scheduling the start from a timer compare.
	 *
	 * There is deliberately NO end-of-packet short. The obvious one is
	 * END_DISABLE, and it does not exist on nRF54L15: that part renamed the
	 * end-of-packet shortcut to PHYEND_DISABLE and defines no END_DISABLE at
	 * all, while nRF52840 defines END_DISABLE and not PHYEND_DISABLE. Either
	 * choice would need a per-series #if for no gain, because the
	 * transmitter is already finished by the time END fires - the disable
	 * costs interrupt latency and nothing on the air. The ISR triggers it
	 * explicitly instead.
	 */
	nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK);

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

	nrf_timer_cc_set(TIMER_ADDR, CC_TXSTART, clock_counter_at(t_start));
	nrfx_gppi_conn_enable(radiant_op.conn_start_tx);

	/*
	 * RE-CHECK AFTER PROGRAMMING, because the failure this catches has no
	 * other symptom.
	 *
	 * Everything above costs time - a format, an address, a power table
	 * scan and a body copy - and ARM_SETUP_US is a budget rather than a
	 * guarantee. If the counter has already passed the compare by the time
	 * it is written, the compare simply never fires: TASKS_TXEN is never
	 * triggered, no END arrives, no DISABLED arrives, and the operation
	 * hangs holding the one slot the whole scheduler shares. The receive
	 * path at least closes its own window; this one would wait for ever.
	 *
	 * So the arm is confirmed against the clock one last time and unwound
	 * into a loud ETIME if it lost the race. The cost is one timer capture.
	 */
	now = radiant_radio_now();
	if (t_start <= now) {
		nrfx_gppi_conn_disable(radiant_op.conn_start_tx);
		nrf_radio_shorts_set(NRF_RADIO, 0);
		rc = RADIANT_RADIO_ETIME;
		goto fail;
	}

	radiant_op.id = radiant_op.next_id++;
	if (radiant_op.next_id == 0u) {
		radiant_op.next_id = 1u;   /* ids are non-zero by contract */
	}
	*op = radiant_op.id;
	LOG_DBG("tx op=%u alen=%u blen=%u start=+%d last_err=%d",
		(unsigned)radiant_op.id, (unsigned)req->addr_len,
		(unsigned)req->body_len, (int)(int64_t)(t_start - now),
		(int)radiant_dbg_tx_err);
	return RADIANT_RADIO_OK_RC;

fail:
	LOG_DBG("tx refused: %d (addr_len=%u)", rc, (unsigned)req->addr_len);
	radiant_op.kind = OP_NONE;
	return rc;
}

/* ---------------------------------------------------------------------------
 * Receive
 * ---------------------------------------------------------------------------
 */

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
	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)radiant_rx_buf;

	/*
	 * ADVERTISE THE WORST CASE, REFUSE ONLY THE IMPOSSIBLE.
	 *
	 * caps.min_arm_lead_us is the worst case over every address length, and
	 * the core plans with it: radiant_api.c posts its window at exactly
	 * `now + min_arm_lead_us`. But that `now` is sampled in the core, and
	 * this one is sampled here, microseconds later after a mutex, a pump
	 * loop and a scheduler pass. Testing the advertised figure against the
	 * later sample therefore fails by however long that took - ALWAYS, on
	 * every arm, however much lead the core actually left.
	 *
	 * That is not a hypothetical: it is what kept every window refused after
	 * the lead itself was corrected, and a refused arm is what wedges the
	 * board. So the test is against what the HARDWARE genuinely needs for
	 * this address length - ramp-up plus preamble and address airtime - and
	 * the advertised figure keeps its safety margin for the core's planning.
	 * A window that really cannot be made still gets a loud ETIME.
	 */
	now = radiant_radio_now();
	if (req->t_open < now + air_lead_us(req->fmt->addr_len)) {
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
	radiant_op.rx_any          = false;

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
	/* Cleared here as well as the three above, because a (D)PPI connection
	 * fires on the event flag rather than on an edge: a stale RXREADY left
	 * over from a previous window would trigger RSSISTART before this one's
	 * receiver is live, and the sample would be of nothing. */
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RXREADY);

	/*
	 * Both edges of the window in hardware. The open is a compare firing
	 * TASKS_RXEN; the close is a compare firing TASKS_DISABLE, held open
	 * long enough for a frame whose t_sync is exactly t_close to finish
	 * arriving - body plus CRC - rather than being cut off mid-packet.
	 *
	 * THE TWO EDGES DO NOT TAKE THE CALIBRATION THE SAME WAY, and writing
	 * that out is the point of this comment rather than applying the term
	 * symmetrically because symmetry looks tidier.
	 *
	 * The OPEN takes none. Both edges are antenna-referenced, and the
	 * receive chain delay is latency AFTER the antenna: for a frame whose
	 * address ends at the antenna at t_open, the radio has to be receiving
	 * from t_open - air_lead whatever the demodulator does with it
	 * afterwards. Subtracting the chain delay here would open the window
	 * 9 us early for no reason and buy nothing.
	 *
	 * The CLOSE takes it, negated. That same frame does not raise
	 * EVENTS_ADDRESS until t_close - T_SYNC_CAL_US (the constant is
	 * negative, so this is later), and TASKS_DISABLE must not have fired
	 * before then plus the rest of the packet. The body slack below already
	 * swamps 9 us, so this changes nothing today - which is exactly when it
	 * is free to write, and it stops the next person who shortens that slack
	 * from also having to rediscover the chain delay.
	 */
	start_cc = clock_counter_at(req->t_open -
				    (radiant_time_t)air_lead_us(req->fmt->addr_len));
	close_cc = clock_counter_at(t_sync_cal(req->t_close, -T_SYNC_CAL_US) +
				    (radiant_time_t)((req->fmt->body_len + 2u) *
						     US_PER_BYTE));

	nrf_timer_cc_set(TIMER_ADDR, CC_START, start_cc);
	nrf_timer_cc_set(TIMER_ADDR, CC_CLOSE, close_cc);

	/* The endpoints were wired at init; only the enable is per operation. */
	nrfx_gppi_conn_enable(radiant_op.conn_start);
	nrfx_gppi_conn_enable(radiant_op.conn_close);
	/* Receive only. A transmit ramps up through TXREADY and never raises
	 * RXREADY, so leaving this on would cost nothing - but it is enabled
	 * and disabled with the other two so that "which connections are live"
	 * has one answer rather than an exception. */
	nrfx_gppi_conn_enable(radiant_op.conn_rssi);

	radiant_op.id = radiant_op.next_id++;
	if (radiant_op.next_id == 0u) {
		radiant_op.next_id = 1u;   /* ids are non-zero by contract */
	}
	*op = radiant_op.id;
	LOG_DBG("rx op=%u n=%u alen=%u cov=%u open=+%d close=+%d isr=%u ev=%u ok=%u term=%u",
		(unsigned)radiant_op.id, req->n_filters,
		(unsigned)req->fmt->addr_len, (unsigned)req->fmt->crc.cover_addr,
		(int)(int64_t)(req->t_open - now), (int)(int64_t)(req->t_close - now),
		(unsigned)radiant_dbg_isr, (unsigned)radiant_dbg_rx_ev, (unsigned)radiant_dbg_ok,
		(unsigned)radiant_dbg_term);
	return RADIANT_RADIO_OK_RC;

fail:
	LOG_DBG("rx refused: %d (n=%u addr_len=%u)", rc, req->n_filters,
		req->fmt->addr_len);
	radiant_op.kind = OP_NONE;
	return rc;
}

/* ---------------------------------------------------------------------------
 * Energy detect
 * ---------------------------------------------------------------------------
 *
 * THIS PART HAS NO ENERGY-DETECT MODE THIS BACKEND CAN USE, and saying so is
 * the first thing to understand about the code below. The nRF RADIO's EDCNT /
 * EDSAMPLE hardware averager exists only in IEEE 802.15.4 mode, and this
 * backend is a 1 Mbit GFSK receiver - switching modes to take a measurement
 * would reconfigure the peripheral out from under an ANT window's packet
 * configuration. So the measurement is built out of what IS available in this
 * mode: RSSISAMPLE, one shot per TASKS_RSSISTART, read by software.
 *
 * WHICH MAKES THE SAMPLING BUDGET THE WHOLE DESIGN. Reading RSSI in software
 * means reading it in the radio interrupt at priority 0, and
 * radiant_radio_hal.h's callback contract says "do not do work proportional to
 * anything". A loop that filled a 400 us dwell would be exactly that, and a
 * loop that filled a 10 ms chunk would be a scheduler-visible stall. So:
 *
 *   - ED_SAMPLES_MAX bounds the burst at a CONSTANT, whatever the dwell is.
 *     Thirty-two samples at ED_SAMPLE_STEP_US apart is ~64 us of spinning,
 *     which is comparable to what radio_disable_now() already spends waiting
 *     for a ramp-down and well inside this file's established practice.
 *   - The dwell bounds it again, from the other side, so a caller that asks
 *     for less than the burst needs gets a shorter burst rather than an
 *     overrun.
 *   - ONE INTERRUPT PER INDEX, not per sample. The index's RXREADY is the only
 *     event taken; the burst runs inside it and the radio is taken down again
 *     before the callback. A per-sample compare would have been more faithful
 *     to the dwell and would have cost 200 interrupts per index.
 *
 * WHAT THAT MEANS FOR THE NUMBER, stated plainly because the alternative is a
 * consumer reading more into it than is there: this backend's min and mean
 * describe a ~64 us burst near the START of the dwell, not the dwell as a
 * whole. It sees a Wi-Fi transmission that is in progress; it does not see one
 * that begins 300 us later. The defence is repetition rather than dwell
 * length - the scheduler revisits every index continuously, and
 * radiant_chanmap.c aggregates the minimum and the maximum across dwells, so
 * a busy index reveals itself over seconds. radiant_ed_event.samples is
 * reported for exactly this reason: a consumer that could not see how much
 * evidence one event carries would weigh this the same as a backend with a
 * hardware averager behind it.
 *
 * AN INDEX ENDS AS SOON AS ITS BURST IS DONE. radiant_radio_hal.h defines
 * dwell_us as an upper bound rather than a duration precisely so that this is
 * allowed, and it matters twice: the receiver stops drawing current for the
 * remaining ~330 us of a default dwell, and a whole sweep finishes well inside
 * the gap the scheduler sized for it, which is the direction an operation
 * sharing a radio with a tracked channel should be wrong in.
 *
 * NOTHING CAN BE RECEIVED DURING AN ED SWEEP, and that is enforced in hardware
 * rather than by not looking: RXADDRESSES is cleared, so no logical address is
 * enabled, so the matcher cannot fire, so there is no ADDRESS, no DMA and no
 * END. A sweep that could deliver a frame would be a frame delivered against
 * an operation whose filters nobody set.
 */

#if defined(CONFIG_RADIANT_CORE_ED_SCAN)

/* The burst, bounded at a constant. See above for why the constant and not the
 * dwell is what bounds the interrupt. */
#define ED_SAMPLES_MAX    32u
/*
 * Spacing between samples, in microseconds of this backend's own 1 MHz TIMER.
 * The datasheet's RSSI settling time is a quarter of a microsecond on both
 * parts, so this is not a settling wait - it is the finest spacing a 1 MHz
 * timer can express with a margin, and taking samples faster than that would
 * produce thirty-two readings of one instant rather than of a burst.
 */
#define ED_SAMPLE_STEP_US 2u

/* The shortest dwell this backend can honour: ramp-up plus a handful of
 * samples. Below it an index would necessarily overrun its own ceiling, which
 * the HAL forbids - so it is refused rather than silently exceeded. */
#define ED_DWELL_MIN_US   (RAMP_UP_US + 4u * ED_SAMPLE_STEP_US)

/* Put the receiver on one index. The frequency is the only thing that changes
 * between indices; mode, shorts and the empty address set were programmed once
 * at the arm. */
static void ed_enter_index(uint8_t rf_index)
{
	nrf_radio_frequency_set(NRF_RADIO, (uint16_t)(2400u + rf_index));
	/* Cleared before the task, not after: RXREADY is the event this
	 * operation's interrupt is taken on, and a stale one would be taken for
	 * this index's before the receiver had ramped. */
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RXREADY);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
}

/*
 * One index's receiver is live. Take the burst, report it, and move on.
 *
 * Runs in the radio interrupt. Everything in it is bounded by ED_SAMPLES_MAX
 * except radio_disable_now(), which has its own bound.
 */
static void ed_dwell(void)
{
	struct radiant_ed_event evt;
	int32_t  sum = 0;
	int8_t   min_dbm = 0;
	uint16_t n = 0u;
	uint8_t  rf = radiant_op.ed_cur;
	uint32_t i;

	/* The first index started from a compare; every index after it is
	 * triggered by software. Taking the connection away here means the
	 * compare cannot fire a second time against an operation that has moved
	 * on - which it could, 71 minutes later, when the counter comes back
	 * round to it. */
	nrfx_gppi_conn_disable(radiant_op.conn_start);

	for (i = 0u; i < ED_SAMPLES_MAX; i++) {
		uint32_t t0;
		int8_t   dbm;

		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RSSISTART);
		t0 = timer_capture(CC_NOW);
		while ((uint32_t)(timer_capture(CC_NOW) - t0) < ED_SAMPLE_STEP_US) {
			/* One timer tick. See ED_SAMPLE_STEP_US. */
		}

		dbm = rssi_sample_dbm();
		if (n == 0u || dbm < min_dbm) {
			min_dbm = dbm;
		}
		sum += (int32_t)dbm;
		n++;

		/* Tested AFTER the first sample, so an index always produces a
		 * measurement. An OK event carrying zero samples is the one
		 * thing radiant_chanmap.c drops outright, and delivering one
		 * would turn a short dwell into a hole in the map rather than
		 * into a coarse entry. */
		if ((int32_t)(timer_capture(CC_NOW) -
			      radiant_op.ed_deadline_cc) >= 0) {
			break;
		}
	}

	/* The receiver is off before the callback runs, on the same reasoning as
	 * every other path in this file: the callback may arm the next
	 * operation, and a DISABLED left in flight would end that one instead. */
	radio_disable_now();

	memset(&evt, 0, sizeof(evt));
	evt.op = radiant_op.id;
	evt.status = RADIANT_RADIO_STATUS_OK;
	evt.rf_index = rf;
	evt.min_dbm = min_dbm;
	/* Integer division truncates toward zero, which on negative dBm is
	 * toward the LOUD end - so a mean is wrong by at most one dB and wrong
	 * in the direction that makes an index look busier than it is. That is
	 * the conservative direction for anything that later picks a quiet
	 * frequency from this. */
	evt.mean_dbm = (int8_t)(sum / (int32_t)n);
	evt.samples = n;
	radiant_op.cbs->ed(&evt, radiant_op.user);

	/* The callback is entitled to abort or replace this operation, and the
	 * scheduler does exactly that when a tracked window falls due. */
	if (radiant_op.kind != OP_ED || radiant_op.terminal_sent) {
		return;
	}

	if (rf >= radiant_op.ed_hi) {
		deliver_terminal(RADIANT_RADIO_STATUS_TIMEOUT);
		return;
	}

	radiant_op.ed_cur = (uint8_t)(rf + 1u);
	radiant_op.ed_deadline_cc += radiant_op.ed_dwell_us;
	ed_enter_index(radiant_op.ed_cur);
}

#endif /* CONFIG_RADIANT_CORE_ED_SCAN */

int radiant_radio_ed(const struct radiant_ed_req *req, uint32_t *op)
{
#if !defined(CONFIG_RADIANT_CORE_ED_SCAN)
	(void)req;
	if (op != NULL) {
		*op = 0u;
	}
	/* caps.has_ed_scan is false on this build and the core reads it before
	 * it ever gets here; this is the backstop, and it is the HAL's existing
	 * rule for a well-formed request a backend cannot do. */
	return RADIANT_RADIO_ENOTSUP;
#else
	unsigned int   key;
	radiant_time_t now;
	uint32_t       start_cc;
	int            rc;

	if (op == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	*op = 0u;

	if (req == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (!radiant_op.inited || !radiant_op.enabled) {
		return RADIANT_RADIO_ESTATE;
	}
	if (req->rf_index_hi < req->rf_index_lo ||
	    req->rf_index_hi > RADIANT_RF_INDEX_MAX || req->dwell_us == 0u) {
		return RADIANT_RADIO_EINVAL;
	}
	if (req->dwell_us < ED_DWELL_MIN_US) {
		return RADIANT_RADIO_ENOTSUP;
	}
	/* No consumer, no operation. A sweep whose events have nowhere to go
	 * would still occupy the radio. */
	if (radiant_op.cbs == NULL || radiant_op.cbs->ed == NULL) {
		return RADIANT_RADIO_ENOTSUP;
	}

	/* One operation in flight, resolved exactly as the other two arms
	 * resolve it. */
	key = irq_lock();
	if (radiant_op.kind != OP_NONE) {
		irq_unlock(key);
		return RADIANT_RADIO_EBUSY;
	}
	radiant_op.kind = OP_ED;
	irq_unlock(key);

	/* Against what the hardware genuinely needs, not against the advertised
	 * worst case - see the long note in radiant_radio_rx() on why testing
	 * the advertised figure against a later `now` refuses every arm. There
	 * is no address to receive here, so the need is ramp-up alone. */
	now = radiant_radio_now();
	if (req->t_start < now + (radiant_time_t)RAMP_UP_US) {
		rc = RADIANT_RADIO_ETIME;
		goto fail;
	}

	radiant_op.ed_hi = req->rf_index_hi;
	radiant_op.ed_cur = req->rf_index_lo;
	radiant_op.ed_dwell_us = req->dwell_us;
	radiant_op.terminal_sent = false;
	radiant_op.rx_any = false;

	nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);
	/*
	 * NO LOGICAL ADDRESS IS ENABLED, which is what makes this a measurement
	 * rather than a receive window with the filters left over from the last
	 * one. The matcher cannot fire, so there is no ADDRESS event, no DMA
	 * into radiant_rx_buf and no END. PACKETPTR is still pointed somewhere
	 * legal because a peripheral with a null DMA pointer is a fault waiting
	 * for a configuration mistake, not because anything will be written.
	 */
	NRF_RADIO->RXADDRESSES = 0u;
	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)radiant_rx_buf;
	/* READY_START alone: the receiver has to reach the RX state for RSSI to
	 * mean anything, and no END_START because there is no packet to chain
	 * to. */
	nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK);

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RXREADY);
	nrf_radio_frequency_set(NRF_RADIO,
				(uint16_t)(2400u + radiant_op.ed_cur));

	/*
	 * RXREADY becomes an interrupt for the duration of this operation only,
	 * and is taken away again in deliver_terminal(). Leaving it enabled
	 * would put an extra interrupt at the head of every ordinary receive
	 * window - a few hundred nanoseconds each, on the path this phase's
	 * gate says must be unchanged. The cheapest way to keep that promise is
	 * for the receive path to be byte-identical when no ED operation is
	 * running, which it is.
	 */
	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_RXREADY_MASK);

	start_cc = clock_counter_at(req->t_start - (radiant_time_t)RAMP_UP_US);
	radiant_op.ed_deadline_cc = start_cc + req->dwell_us;
	nrf_timer_cc_set(TIMER_ADDR, CC_START, start_cc);
	nrfx_gppi_conn_enable(radiant_op.conn_start);
	/* conn_close is deliberately NOT enabled. A window's close is a
	 * compare because nothing else knows when the window is over; an ED
	 * index ends when its own burst does, and a compare firing
	 * TASKS_DISABLE underneath a burst would take the receiver away
	 * mid-measurement. */

	radiant_op.id = radiant_op.next_id++;
	if (radiant_op.next_id == 0u) {
		radiant_op.next_id = 1u;
	}
	*op = radiant_op.id;
	LOG_DBG("ed op=%u rf=%u..%u dwell=%u start=+%d",
		(unsigned)radiant_op.id, (unsigned)req->rf_index_lo,
		(unsigned)req->rf_index_hi, (unsigned)req->dwell_us,
		(int)(int64_t)(req->t_start - now));
	return RADIANT_RADIO_OK_RC;

fail:
	LOG_DBG("ed refused: %d", rc);
	radiant_op.kind = OP_NONE;
	return rc;
#endif /* CONFIG_RADIANT_CORE_ED_SCAN */
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

	/* Take the operation's hardware edges away first, or a compare can
	 * re-fire against an operation that has already been retired. All four
	 * unconditionally: which are live depends on the kind, and disabling a
	 * channel that was not enabled costs a register write. */
	nrfx_gppi_conn_disable(radiant_op.conn_start);
	nrfx_gppi_conn_disable(radiant_op.conn_start_tx);
	nrfx_gppi_conn_disable(radiant_op.conn_close);
	nrfx_gppi_conn_disable(radiant_op.conn_rssi);
	nrf_radio_shorts_set(NRF_RADIO, 0);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_STOP);
	irq_unlock(key);

	/*
	 * THE OPERATION SLOT IS RELEASED HERE, SYNCHRONOUSLY, AND IT HAS TO BE.
	 *
	 * This used to trigger TASKS_DISABLE and return, leaving radiant_op.kind
	 * set until EVENTS_DISABLED reached the interrupt handler - which is
	 * correct-looking and breaks the one caller that matters most.
	 *
	 * radiant_transfer.c answers acknowledged data by arming its reply FROM
	 * INSIDE the receive callback, because 1560 us is the tightest deadline
	 * in the link layer. That arm reaches radiant_sched.c, which drops the
	 * channel's live request and calls this function - and we are already
	 * inside the RADIO interrupt, so the DISABLED this provokes cannot be
	 * serviced until the handler returns. The re-arm therefore found
	 * radiant_op.kind still OP_RX and was refused RADIANT_RADIO_EBUSY. The
	 * scheduler treats EBUSY as transient and consumes nothing, so the reply
	 * sat pending until the housekeeping pump ran up to 50 ms later, by which
	 * time its t_sync was long past and it was dropped as a missed window.
	 *
	 * Nothing reports any of that. On the bench it read as an acknowledged
	 * exchange that completed about one time in three, which looks exactly
	 * like a marginal link and is not one: the frame was never transmitted.
	 *
	 * So the disable is completed and its event consumed before returning
	 * (radio_disable_now()), and the terminal event is delivered from here.
	 * Delivering it is safe rather than a double delivery: deliver_terminal()
	 * is idempotent on radiant_op.terminal_sent, and radio_disable_now() has
	 * taken the DISABLED away from the handler, so the branch that would have
	 * delivered it later now sees OP_NONE and drops through. It also keeps
	 * radiant_radio_hal.h's promise that an aborted operation still produces
	 * exactly one terminal event, rather than quietly dropping it.
	 *
	 * Every caller retires its own bookkeeping BEFORE calling this - that is
	 * radiant_sched.c's documented order - so the callback this ends up
	 * making finds an operation nobody owns and returns immediately. The
	 * delivery is a contract obligation being met, not a code path anyone
	 * depends on for behaviour.
	 */
	radio_disable_now();
	deliver_terminal(RADIANT_RADIO_STATUS_ABORTED);

	return RADIANT_RADIO_OK_RC;
}

/* ---------------------------------------------------------------------------
 * Bench diagnostic: die temperature
 *
 * See radiant_radio_nrf_diag.h for why this exists and why it is not part of
 * the HAL contract. Thread context only - see that header.
 * ---------------------------------------------------------------------------
 */

int radiant_radio_nrf_die_temp_c(int16_t *out)
{
	int32_t raw;

	if (out == NULL) {
		return RADIANT_RADIO_EINVAL;
	}

	nrf_temp_task_trigger(NRF_TEMP, NRF_TEMP_TASK_START);
	while (!nrf_temp_event_check(NRF_TEMP, NRF_TEMP_EVENT_DATARDY)) {
		/* Datasheet-quoted conversion time is tens of microseconds;
		 * see the header for why this may only be called from thread
		 * context. */
	}
	nrf_temp_event_clear(NRF_TEMP, NRF_TEMP_EVENT_DATARDY);

	/* TEMP is 2's-complement, 0.25 degC steps (hal/nrf_temp.h). *25 turns
	 * quarter-degrees into centi-degrees without an intermediate float. */
	raw = nrf_temp_result_get(NRF_TEMP);
	*out = (int16_t)(raw * 25 / 10);

	nrf_temp_task_trigger(NRF_TEMP, NRF_TEMP_TASK_STOP);
	return RADIANT_RADIO_OK_RC;
}

/* ---------------------------------------------------------------------------
 * The interrupt
 * ---------------------------------------------------------------------------
 */

/*
 * Take the radio down AND consume the EVENTS_DISABLED it raises, before this
 * interrupt runs any completion callback.
 *
 * THE BUG THIS EXISTS TO PREVENT HAS NO SYMPTOM EXCEPT SILENCE. Both branches
 * that retire an operation from EVENTS_END - a finished transmit, and a receive
 * that ends on RADIANT_RX_STOP_ON_FIRST - used to trigger TASKS_DISABLE and then
 * call the completion callback with the DISABLED still in flight. The HAL
 * invites that callback to arm the next operation immediately, and
 * radiant_transfer.c takes the invitation: it arms the acknowledgement's receive
 * window from inside the transmit callback, because at a 1.55 ms turnaround
 * nothing slower meets the deadline. Control then returned here, to the
 * EVENTS_DISABLED test at the bottom of radio_isr() - which by now was set, by
 * the disable belonging to the operation that had already ended. It read as the
 * terminal event of the window armed microseconds earlier and tore it down.
 *
 * So the reply window was armed and destroyed inside one interrupt, the peer's
 * acknowledgement arrived at a disabled radio, and the transfer reported
 * RADIANT_TRANSFER_FAIL_NO_ACK - a plausible on-air failure, with the radio
 * never having listened. Nothing logs, nothing faults, and the acknowledged-data
 * path is exactly the one a trainer uses for resistance.
 *
 * Waiting is what makes it unambiguous: ramp-down from TXIDLE or RXIDLE is a
 * few microseconds, so the event is consumed here rather than left to be
 * misattributed later. The bound is a backstop against a state the datasheet
 * does not promise DISABLED from, not a timeout anyone expects to reach, and it
 * is spun against this backend's own 1 MHz TIMER so it stays a real duration
 * whatever the core clock is.
 */
#define RADIO_DISABLE_MAX_US 100u

static void radio_disable_now(void)
{
	uint32_t deadline;

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_DISABLE);

	deadline = timer_capture(CC_NOW) + RADIO_DISABLE_MAX_US;
	while (!nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
		if ((int32_t)(timer_capture(CC_NOW) - deadline) >= 0) {
			/* Leave the event alone: if it arrives late the branch
			 * at the bottom of radio_isr() sees it with kind ==
			 * OP_NONE and drops it, which is the same outcome by a
			 * slower road. */
			return;
		}
	}
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
}

/* Every operation ends here, and it ends down exactly one of the two callbacks.
 * The kind has to be read BEFORE the slot is released, which is why it is
 * latched into a local rather than tested around the call. */
static void deliver_terminal(enum radiant_radio_status st)
{
	enum op_kind kind = radiant_op.kind;

	if (radiant_op.terminal_sent || kind == OP_NONE) {
		return;
	}
	radiant_op.terminal_sent = true;
	radiant_dbg_term++;

	nrfx_gppi_conn_disable(radiant_op.conn_start);
	nrfx_gppi_conn_disable(radiant_op.conn_start_tx);
	nrfx_gppi_conn_disable(radiant_op.conn_close);
	nrfx_gppi_conn_disable(radiant_op.conn_rssi);
	nrf_radio_shorts_set(NRF_RADIO, 0);

	radiant_op.kind = OP_NONE;

#if defined(CONFIG_RADIANT_CORE_ED_SCAN)
	if (kind == OP_ED) {
		struct radiant_ed_event evt;

		/* RXREADY goes back to not being an interrupt. See the note at
		 * the enable in radiant_radio_ed(): an ordinary receive window
		 * must cost exactly what it cost before this operation existed,
		 * and the way to be sure of that is for it to run against the
		 * same interrupt mask. */
		nrf_radio_int_disable(NRF_RADIO, NRF_RADIO_INT_RXREADY_MASK);

		memset(&evt, 0, sizeof(evt));
		evt.op = radiant_op.id;
		evt.status = st;
		/* No measurement on a terminal event, on the same terms as
		 * radiant_rx_event's frame fields on a TIMEOUT. The indices
		 * that were measured were reported as they happened. */
		radiant_op.cbs->ed(&evt, radiant_op.user);
		return;
	}
#endif

	if (kind == OP_TX) {
		struct radiant_tx_event evt;

		memset(&evt, 0, sizeof(evt));
		evt.op = radiant_op.id;
		evt.status = st;
		/* Nothing went out, so there is no captured address instant to
		 * report and CC_SYNC still holds some earlier operation's. Say
		 * so rather than reporting a stale capture as exact. */
		evt.t_sync = radiant_radio_now();
		evt.t_sync_exact = false;
		radiant_op.cbs->tx(&evt, radiant_op.user);
	} else {
		struct radiant_rx_event evt;

		memset(&evt, 0, sizeof(evt));
		evt.op = radiant_op.id;
		evt.status = st;
		evt.t_sync = radiant_radio_now();
		evt.t_sync_exact = false;

		/*
		 * The noise floor: RSSI measured inside this window with no
		 * packet present.
		 *
		 * Three conditions, and each one is doing work.
		 *
		 * TIMEOUT, not ABORTED. A window that ran to its own close
		 * definitely ramped up, so RXREADY fired and the (D)PPI took a
		 * sample from THIS window. One aborted before its start compare
		 * never ramped up at all, and RSSISAMPLE would still hold a
		 * previous window's value - the register has no "invalid", so
		 * the only defence is not reading it when it cannot be fresh.
		 *
		 * Nothing received. A window that heard something has had
		 * RSSISAMPLE overwritten by the ADDRESS short with the packet's
		 * own level, which is the transmitter's strength and the
		 * opposite of what this field means.
		 *
		 * The same errata and temperature correction as rssi_dbm, so
		 * the two are on one scale and can be subtracted to get a
		 * margin. Correcting one and not the other would produce a
		 * margin that is wrong by exactly the correction and looks
		 * entirely reasonable.
		 */
		if (st == RADIANT_RADIO_STATUS_TIMEOUT && !radiant_op.rx_any) {
			evt.noise_dbm = rssi_sample_dbm();
			evt.has_noise = true;
		}

		radiant_op.cbs->rx(&evt, radiant_op.user);
	}
}

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);
	radiant_dbg_isr++;

#if defined(CONFIG_RADIANT_CORE_ED_SCAN)
	/*
	 * FIRST, AND ONLY WHILE AN ED SWEEP IS RUNNING.
	 *
	 * RXREADY is only unmasked for the duration of one, so this test costs
	 * an event-flag read on every other interrupt and nothing else - which
	 * is the whole of what this phase adds to the receive path. The kind
	 * test is not redundant with the mask: an RXREADY raised by the last
	 * dwell can still be pending in the NVIC when the terminal event has
	 * already retired the operation, and running a burst against an
	 * operation nobody owns would sample a receiver that is off.
	 */
	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_RXREADY)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RXREADY);
		if (radiant_op.kind == OP_ED && !radiant_op.terminal_sent) {
			ed_dwell();
		}
	}
#endif

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);

		if (radiant_op.kind == OP_TX && !radiant_op.terminal_sent) {
			struct radiant_tx_event evt;

			/*
			 * The frame is out. This event is the terminal one, and
			 * it is delivered from END rather than from the
			 * DISABLED that follows - because the slot has to be
			 * free before the callback runs, so that the callback
			 * can arm the next operation. Releasing it here also
			 * makes the DISABLED branch below a no-op for this
			 * operation rather than a second delivery.
			 */
			radiant_op.terminal_sent = true;
			radiant_dbg_term++;
			nrfx_gppi_conn_disable(radiant_op.conn_start_tx);
			nrf_radio_shorts_set(NRF_RADIO, 0);
			/* In place of the end-of-packet short this part may or
			 * may not spell the same way. See radiant_radio_tx().
			 * Consumed here, not left in flight: see
			 * radio_disable_now() for what a DISABLED outliving its
			 * own operation does to the window the callback below
			 * is about to arm. */
			radio_disable_now();

			memset(&evt, 0, sizeof(evt));
			evt.op = radiant_op.id;
			evt.status = RADIANT_RADIO_STATUS_OK;
			/*
			 * A hardware capture, exactly as on receive: EVENTS_ADDRESS
			 * fires when the address has been SENT, and the same
			 * always-live (D)PPI connection captures it. So a master
			 * closes its slot phase against what actually went out
			 * rather than against what it asked for.
			 */
			evt.t_sync = t_sync_cal(
				clock_absolute(
					nrf_timer_cc_get(TIMER_ADDR, CC_SYNC)),
				T_SYNC_CAL_TX_US);
			evt.t_sync_exact = true;

			/* Achieved minus requested: this backend's transmit
			 * timing error, in microseconds, measured every frame.
			 * See t_sync_req. Zero is the target. */
			radiant_dbg_tx_err =
				(int32_t)(int64_t)(evt.t_sync -
						   radiant_op.t_sync_req);

			radiant_op.kind = OP_NONE;
			radiant_op.cbs->tx(&evt, radiant_op.user);
		}

		if (radiant_op.kind == OP_RX && !radiant_op.terminal_sent) {
			struct radiant_rx_event evt;
			bool crc_ok = nrf_radio_crc_status_check(NRF_RADIO);
			uint8_t logical = (uint8_t)(NRF_RADIO->RXMATCH &
						    RADIO_RXMATCH_RXMATCH_Msk);

			radiant_dbg_rx_ev++;
			if (crc_ok) {
				radiant_dbg_ok++;
			}
			/* Before the report_crc_fail gate, deliberately. A
			 * window whose CRC failures are suppressed still HEARD
			 * something, and its RSSISAMPLE is that something's
			 * level rather than the band's - so it must not
			 * contribute a noise sample either way. */
			radiant_op.rx_any = true;
			if (crc_ok || radiant_op.report_crc_fail) {
				memset(&evt, 0, sizeof(evt));
				evt.op = radiant_op.id;
				evt.status = crc_ok ? RADIANT_RADIO_STATUS_OK
						    : RADIANT_RADIO_STATUS_CRC_FAIL;

				/* The hardware capture, moved from
				 * event-referenced to antenna-referenced. */
				evt.t_sync = t_sync_cal(
					clock_absolute(nrf_timer_cc_get(
						TIMER_ADDR, CC_SYNC)),
					T_SYNC_CAL_US);
				evt.t_sync_exact = true;

				evt.filter_index =
					(logical < ARRAY_SIZE(radiant_filter_slot))
						? radiant_filter_slot[logical] : 0u;
				evt.body = radiant_rx_buf;
				evt.body_len = radiant_op.body_len;
				evt.has_rssi = true;

				/*
				 * The CRC as it arrived, on failures only.
				 *
				 * RXCRC latches what the engine computed over
				 * the received bytes... which is the same thing
				 * as what arrived, because on a mismatch the
				 * hardware keeps the RECEIVED value here rather
				 * than the computed one - that is what the
				 * register is for. The core needs it to form
				 * crc_rx ^ crc_computed, which is the error
				 * syndrome; without it a CRC failure carries no
				 * information beyond "failed".
				 *
				 * Not populated on OK, and deliberately so: a
				 * frame that passed has a CRC the core can
				 * recompute, and a field that is sometimes the
				 * received value and sometimes the computed one
				 * is a field nobody can reason about.
				 */
				if (!crc_ok) {
					evt.has_crc_rx = true;
					evt.crc_rx = NRF_RADIO->RXCRC;
				}
				evt.rssi_dbm = rssi_sample_dbm();

				if (radiant_op.stop_on_first) {
					/* This event IS the terminal one. */
					radiant_op.terminal_sent = true;
					nrfx_gppi_conn_disable(radiant_op.conn_start);
					nrfx_gppi_conn_disable(radiant_op.conn_close);
					nrf_radio_shorts_set(NRF_RADIO, 0);
					/* Same reason as the transmit branch
					 * above: this callback may arm the next
					 * operation, and a DISABLED left in
					 * flight would end that one instead. */
					radio_disable_now();
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
		} else if (radiant_op.kind == OP_TX) {
			/*
			 * A transmit that reaches DISABLED without having
			 * reached END never went out - the END branch above
			 * releases the slot for one that did. So this is the
			 * abort path and nothing else, and ABORTED is the only
			 * honest status for it.
			 */
			deliver_terminal(RADIANT_RADIO_STATUS_ABORTED);
		}
#if defined(CONFIG_RADIANT_CORE_ED_SCAN)
		else if (radiant_op.kind == OP_ED) {
			/*
			 * An ED sweep consumes its own DISABLED inside
			 * radio_disable_now() at the end of every index, so
			 * reaching this branch means one outlived that bound -
			 * the backstop radio_disable_now() documents. The sweep
			 * cannot continue from here (the receiver is down and
			 * nothing will re-enter it), so it ends, and ABORTED is
			 * the honest status for a sweep that did not reach its
			 * last index.
			 */
			deliver_terminal(RADIANT_RADIO_STATUS_ABORTED);
		}
#endif
	}
}
