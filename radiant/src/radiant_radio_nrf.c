/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf.c - radiant_radio_hal.h on a bare nRF RADIO.
 *
 * Provenance: clean-room. Written from radiant_radio_hal.h (the backend
 * contract), docs/ant-radio-link.md + docs/spike-a-results.md (address
 * arithmetic, measured on hardware), radiant/spike/rx_raw/src/main.c
 * (proven receive config), and the public Nordic MDK/nrfx headers and
 * nRF52840/nRF54L15 product specs. Nothing derives from sdk-ant, libant.a, or
 * any ANT+ profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * What is measured and what is only seeded is stated at each definition:
 * rev8() and pack_address() (Spike A, nRF54L15, 2026-08-09), apply_format()
 * (Spike B: byte 3 of an ANT frame is a CONTROL byte, not a length),
 * T_SYNC_CAL_US and T_SYNC_CAL_LR_US (seeded from Zephyr's chain delays),
 * RAMP_UP_US and ARM_SETUP_US (datasheet/bracketed), and the errata-153 RSSI
 * table (vendor-sourced).
 *
 * EVERY REGISTER FACT IS FROM nRF54L15; predicted, not measured, to hold on
 * nRF52840 (differs only in ramp-up and TIMING/RXGAIN, not the packet
 * engine). Closed by inspection: the fast ramp-up write; ARM_SETUP_US is
 * per-series (same arm code, half the clock); the TIMER prescaler __ASSERTs
 * it hits exactly 1 MHz; the five-CC requirement is a compile-time #error.
 * Still needing the part: PPI-vs-DPPI routing through nrfx_gppi_conn_alloc(),
 * and the errata-153 RSSI table.
 *
 * The one hardware limit the core cannot see:
 *
 * caps.max_filters is 8, true for SEARCH and misleading for TRACKING - the
 * nRF matcher's eight logical addresses are not eight independent addresses.
 * Logical 0 is BASE0+PREFIX0.AP0; logical 1..7 are BASE1+AP1..AP7. So a
 * window can express at most TWO distinct bases, and seven of eight filters
 * must share one.
 *
 *   SEARCH, 3-byte address [A6 C5 devnum_lo]: base [A6 C5] shared by every
 *   filter, prefix is devnum_lo - eight prefixes, one base, no constraint hit.
 *
 *   TRACKING, 5-byte address [A6 C5 dnl dnh dtype]: base [A6 C5 dnl dnh]
 *   differs per sensor, so two tracked channels can share a window only with
 *   the same device number - never. A merged tracking window is limited to
 *   TWO channels on this backend, not eight.
 *
 * arm_rx() validates the filter set and returns RADIANT_RADIO_ENOTSUP for one
 * it cannot put on the air rather than programming something close;
 * radiant_sched.c may split the window but must not believe eight arbitrary
 * addresses were armed.
 *
 * And one demodulator behaviour the backend absorbs so the core never sees it
 * (docs/decisions/0019-tracking-matches-a-short-address.md, measured on
 * nRF52840, nRF5340 and nRF54L15):
 *
 *   In Nrf_1Mbit the RADIO drops exactly one bit when an alternating run of
 *   about 9+ bits sits inside the address it is matching, or a few bits past
 *   its end. Inside, the frame never matches (a silently missed slot); past
 *   it, the frame arrives with one bit deleted. ~5 % of ANT+ device numbers
 *   put such a run in [A6 C5 dnl dnh dtype]. CONFIG_RADIANT_NRF_SLIP_GUARD
 *   gives exactly those windows a 2-byte hardware match, [A6 C5], so a slip
 *   can only land in bytes the ISR receives, and radiant_addr_split_repair()
 *   restores the bit when the CRC confirms it. Every other window keeps the
 *   5-byte match, because a 2-byte one also matches noise.
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

#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_radio_nrf_diag.h>

/*
 * Who owns the RADIO right now. A private header rather than one under
 * include/radiant/ on purpose: nothing above the backend may know that
 * this question exists, let alone how it is answered.
 */
#include <radiant/radiant_swi.h>

#include "radiant_radio_nrf_gate.h"
#include "radiant_addr_split.h"
#include <radiant/radiant_diag.h>

/*
 * The 0xF7 backend page (ADR 0023), CONFIG_RADIANT_DIAG_COUNTERS only; every
 * NRF_DIAG() site is an empty statement without it. This backend fills the
 * fields it can see without new register reads; the rest read "not measured".
 */
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
static struct {
	uint32_t rx_posted, rx_refused;
	uint32_t term_ok, term_empty, term_error;
	uint32_t frames, sw_mismatch, crc_fail;
} nrf_diag;
#define NRF_DIAG(...) do { __VA_ARGS__; } while (0)
#else
#define NRF_DIAG(...) do { } while (0)
#endif

/* Whether a completion raised inside an MPSL timeslot signal callback has to be
 * queued for the trampoline instead of run there. See the long note above
 * core_cb_rx(). Only the arbitrated build has such a context. */
#if defined(CONFIG_RADIANT_SWI)
#define CORE_CB_DEFERRED 1
#endif

LOG_MODULE_REGISTER(radiant_radio_nrf, CONFIG_RADIANT_LOG_LEVEL);

/*
 * This file pokes RADIO registers directly. Whether it owns the peripheral
 * outright is the gate's answer (src/radiant_radio_nrf_gate.h), not this
 * file's assumption - on the direct gate, anything else touching the RADIO
 * would silently reprogram it underneath us (unexplained loss, not an
 * error); on the MPSL gate, CONFIG_BT sharing the RADIO is the point.
 */
BUILD_ASSERT(IS_ENABLED(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL) ||
		     !IS_ENABLED(CONFIG_BT),
	     "radiant_radio_nrf owns the RADIO outright unless it is arbitrated; "
	     "with CONFIG_BT on, select "
	     "CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL.");

/*
 * Same rule for 802.15.4, measured rather than precautionary: the nRF
 * 802.15.4 driver claims specific RADIO endpoint registers at its own init
 * and never releases them - measured on nRF54L15, PUBLISH_ADDRESS =
 * 0x80000005 (its channel 5), SUBSCRIBE_RXEN = 0x80000017 (its channel 23).
 * An event may PUBLISH to exactly one channel, so on the direct backend this
 * file's own nrfx_gppi_conn_alloc() is refused with -EINVAL and init fails
 * loudly; under the arbiter the two are swapped per grant instead.
 *
 * CONFIG_NET_L2_OPENTHREAD is asserted on rather than
 * CONFIG_NRF_802154_RADIO_DRIVER because the driver symbol is force-selected
 * by things a reader wouldn't expect (it pulls in nrfx's Service Layer,
 * which selects MPSL) - OpenThread is what an application author actually
 * chooses.
 */
BUILD_ASSERT(IS_ENABLED(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL) ||
		     !IS_ENABLED(CONFIG_NET_L2_OPENTHREAD),
	     "radiant_radio_nrf cannot share the RADIO with the 802.15.4 driver "
	     "unarbitrated - that driver holds PUBLISH_ADDRESS and "
	     "SUBSCRIBE_RXEN from its own init. With CONFIG_NET_L2_OPENTHREAD "
	     "on, select CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL.");

/*
 * Under MPSL the interrupt priority is not a tuning choice: mpsl.rst requires
 * the application's RADIO interrupt at MPSL_HIGH_IRQ_PRIORITY (0) when using
 * the timeslot API. The default was already 0 for the acknowledged-data
 * turnaround, so this changes no shipping build - it just makes lowering it
 * a build error instead of an unmeasured bug.
 */
BUILD_ASSERT(!IS_ENABLED(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL) ||
		     CONFIG_RADIANT_BACKEND_NRF_IRQ_PRIO == 0,
	     "MPSL requires the application's RADIO interrupt at priority 0 "
	     "(MPSL_HIGH_IRQ_PRIORITY)");

/* ---------------------------------------------------------------------------
 * The timebase
 *
 * One TIMER at 1 MHz, free-running, never stopped: the backend's whole clock.
 * radiant_radio_now() reads it, t_sync is a hardware capture of it, and every
 * scheduled start/close is a compare against it.
 *
 * The RTC or Zephyr's kernel clock would be cheaper and wrong: t_sync must be
 * captured by hardware from RADIO's own ADDRESS event with no software in the
 * path (this is what gets the extended-message 0xE0 timestamp to ~0.011 ms
 * instead of the ~2.6 ms a host clock sees through USB jitter), and a capture
 * is only useful against the timer that captured it.
 * ---------------------------------------------------------------------------
 */

/*
 * The TIMER comes from devicetree, not a configured address: every TIMER
 * node on this SoC is `status = "disabled"` by default (unaddressable), and
 * a hardcoded address is a guess that bus-faults on the wrong SoC. Devicetree
 * also makes a second user of the same TIMER a devicetree conflict rather
 * than two drivers quietly reprogramming each other's prescaler.
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
 * Compare/capture channel assignment. Five, not four: RX-start and TX-start
 * need separate compares because on a DPPI part an event may PUBLISH to
 * exactly one channel, so two compares can't share one connection
 * (nrfx_gppi_conn_alloc() returns -EINVAL if an endpoint is already
 * attached). Older PPI parts could share; writing it the way that works on
 * both keeps this one backend. Detach/reattach per arm was rejected: the arm
 * path runs in interrupt context inside an 80 us budget.
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
 * Extends the 32-bit us counter (wraps every 71.6 min) to the HAL's 64 bits.
 * Exact provided a fold runs at least once per wrap. Capture AND fold must be
 * one critical section against EVERY context that folds. On an arbitrated
 * build that includes MPSL's zero-latency signal callback (radio_isr(),
 * deliver_terminal(), program_rx()), which irq_lock() does not mask: a ZLI
 * fold between a thread's capture and its fold moves `last` past the thread's
 * stale counter, and the thread's `counter - last` wraps to ~2^32, a permanent
 * +71.6 min step (review 2026-10-31, A13-1). So CLOCK_LOCK is PRIMASK there,
 * for a capture and an add (< 1 us). On the direct build no zero-latency
 * context exists, it is the same irq_lock as ever, and the counter and branch
 * below are compiled out: a build without CORE_CB_DEFERRED is byte-identical.
 */
static struct {
	uint64_t base;   /* microseconds accounted for up to `last` */
	uint32_t last;   /* the counter value `base` was folded at */
} radiant_clock;

#if defined(CORE_CB_DEFERRED)
#define CLOCK_LOCK()    radiant_swi_zli_lock()
#define CLOCK_UNLOCK(k) radiant_swi_zli_unlock(k)
/* Folds that went backwards: one raced another context. Should stay zero. */
static volatile uint32_t radiant_dbg_clock_back;
#else
#define CLOCK_LOCK()    irq_lock()
#define CLOCK_UNLOCK(k) irq_unlock(k)
#endif

static uint32_t timer_capture(uint8_t cc)
{
	nrf_timer_task_trigger(TIMER_ADDR, nrf_timer_capture_task_get(cc));
	return nrf_timer_cc_get(TIMER_ADDR, cc);
}

static uint64_t clock_fold(uint32_t counter)
{
	/* Caller holds CLOCK_LOCK(). */
#if defined(CORE_CB_DEFERRED)
	if ((int32_t)(counter - radiant_clock.last) < 0) {
		radiant_dbg_clock_back++;
	}
#endif
	radiant_clock.base += counter - radiant_clock.last;
	radiant_clock.last = counter;
	return radiant_clock.base;
}

radiant_time_t radiant_radio_now(void)
{
	unsigned int key = CLOCK_LOCK();
	uint64_t now = clock_fold(timer_capture(CC_NOW));

	CLOCK_UNLOCK(key);
	return (radiant_time_t)now;
}

/*
 * Turn a counter value captured in the recent past into an absolute time.
 * The difference is taken SIGNED: a capture microseconds before the last
 * fold is ordinary (RADIO's ADDRESS event fires before the interrupt reads
 * it), and unsigned arithmetic would read that as 71 minutes in the future
 * instead of 3 us in the past. Signed is correct within +-35 min of the last
 * fold, which covers every capture this backend sees.
 */
static radiant_time_t clock_absolute(uint32_t counter)
{
	unsigned int key = CLOCK_LOCK();
	uint64_t base = clock_fold(timer_capture(CC_NOW));
	int32_t  delta = (int32_t)(counter - radiant_clock.last);

	CLOCK_UNLOCK(key);
	return (radiant_time_t)((int64_t)base + delta);
}

/* The counter value an absolute time corresponds to, for programming a compare.
 * Only the low 32 bits reach the hardware, which is all a compare uses. */
static uint32_t clock_counter_at(radiant_time_t t)
{
	unsigned int key = CLOCK_LOCK();
	uint64_t base = clock_fold(timer_capture(CC_NOW));
	uint32_t last = radiant_clock.last;

	CLOCK_UNLOCK(key);
	return last + (uint32_t)((int64_t)t - (int64_t)base);
}

/* ---------------------------------------------------------------------------
 * Capabilities
 * ---------------------------------------------------------------------------
 */

/*
 * DOES THIS BUILD CARRY THE CODED PHY AT ALL? Two independent questions, and
 * conflating them is what this macro fixes:
 *
 *   CAN the part do it - RADIO_MODE_MODE_Ble_LR125Kbit, a hardware fact. Kept
 *   as the INNER test so a future part without the coded mode falls back to
 *   one PHY with no edit needed here (ADR 0007).
 *
 *   DOES this image want it - CONFIG_RADIANT_PHY_LR_CODED, off by default:
 *   the coded PHY's 336 us air lead is charged to min_arm_lead_us on EVERY
 *   window of EVERY PHY, and min_arm_lead_us is the scheduler's exclusion
 *   radius, so a 1 M-only ANT+ image pays 288 us of exclusion for a PHY it
 *   never emits. At one channel that is invisible; at eight it costs about
 *   1% of slots per channel.
 *
 * Every coded site in this file keys on this one macro rather than on the
 * hardware test, so the whole LR path leaves an ANT+ image together - there
 * are no half-compiled branches whose reachability has to be re-derived.
 */
#if defined(RADIO_MODE_MODE_Ble_LR125Kbit) && defined(CONFIG_RADIANT_PHY_LR_CODED)
#define RADIANT_NRF_HAS_LR 1
#else
#define RADIANT_NRF_HAS_LR 0
#endif

/*
 * PHYs this build advertises, most-preferred first: phys[0] must be 1 M
 * because every ANT+ compatibility channel is 1 M on RF 57 (ADR 0007).
 */
static const enum radiant_phy radiant_nrf_phys[] = {
	RADIANT_PHY_1M_GFSK,
#if RADIANT_NRF_HAS_LR
	RADIANT_PHY_LR_CODED,
#endif
};

/*
 * Airtime arithmetic, which several constants below are derived from rather
 * than guessed. At 1 Mbit GFSK one byte is 8 us exactly.
 */
#define US_PER_BYTE            8u
#define PREAMBLE_BYTES         1u    /* PCNF0.PLEN = 8bit; confirmed by Spike A */

/*
 * Same arithmetic for LE Coded at S=8 (Bluetooth spec figures; FEC block 1 is
 * always coded at S=8 regardless of data rate):
 *
 *     preamble          80 us   (10 symbols)
 *     access address   256 us   (32 bits)
 *     coding indicator  16 us   ( 2 bits)
 *     TERM1             24 us   ( 3 bits)
 *     ---------------------------------------
 *     FEC block 1      376 us
 *
 * t_sync is the END of the access address, i.e. 336 us in, not 376 - the
 * coding indicator and TERM1 come after it. Using the FEC block 1 total here
 * would silently place every transmit/window 40 us off, the same
 * cancels-out-of-the-period-estimate failure T_SYNC_CAL is about. Kept as
 * two constants, summed in exactly one place.
 */
#define LR_PREAMBLE_US         80u
#define LR_ACCESS_ADDR_US      256u
#define LR_CI_US               16u
#define LR_TERM1_US            24u
#define LR_FEC1_US             (LR_PREAMBLE_US + LR_ACCESS_ADDR_US + \
				LR_CI_US + LR_TERM1_US)
/* The lead from switching on to t_sync, air only - ramp-up is added by
 * air_lead_us(). */
#define LR_AIR_LEAD_US         (LR_PREAMBLE_US + LR_ACCESS_ADDR_US)

/*
 * Ramp-up and turnaround: datasheet figures (antenna-referenced), not bench
 * measurements. A wrong constant puts the frame on air early or late by that
 * amount without failing loudly. The two-board t_sync rig would measure these
 * too, in the same sitting.
 *
 * FAST RAMP-UP IS NOT THE nRF52 RESET DEFAULT, AND 40 us ASSUMES IT.
 * RADIO->MODECNF0.RU resets to Default (~140 us). Without writing it, every
 * nRF52840 transmit/receive would be ~100 us later than air_lead_us()
 * intends - invisible on nRF54L15, which ramps fast unconditionally.
 * radiant_radio_init() now calls nrf_radio_fast_ramp_up_enable_set(), making
 * 40 us a fact. Corroborated by Zephyr's BLE controller tables: 1 Mbit
 * ramp-up 40.9/40.3 us fast vs 140.9/140.3 default, byte-identical
 * nRF52840/nRF54L15.
 */
/*
 * CONFIG_SOC_COMPATIBLE_*, not CONFIG_SOC_SERIES_* (same note on
 * RADIANT_BACKEND_NRF's `depends on` in Kconfig): Zephyr 4.4/NCS v3.4.0
 * renamed SOC_SERIES_NRF52X/NRF54LX, and a Kconfig dependency on the old
 * names would have kept this file from ever being reached rather than
 * failing loudly. SOC_COMPATIBLE_ is set in both v3.2.4 and v3.4.0.
 */
#if defined(CONFIG_SOC_COMPATIBLE_NRF54LX)
#define RAMP_UP_US             40u
#define RX_TO_TX_US            40u
#define TX_TO_RX_US            40u
/*
 * Software setup cost of an arm call. Measured-adjacent, not measured: this
 * value has run since the backend existed with no RADIANT_RADIO_ETIME
 * refusal, which bounds it from below only. Nordic's 802.15.4 driver budgets
 * 185/150 us TX/RX for a heavier arm path on this series
 * (nrf_802154_delayed_trx.c).
 */
#define ARM_SETUP_US           80u
#define CAPS_NAME              "nrf54l RADIO (direct)"
/* nRF54L splits RADIO's interrupt over two lines; RADIO_0 carries the packet
 * events (ADDRESS, END, DISABLED) this backend uses. */
#define RADIANT_RADIO_IRQn     RADIO_0_IRQn
#define RADIANT_RADIO_IRQn_2   RADIO_1_IRQn
#elif defined(CONFIG_SOC_COMPATIBLE_NRF53X)
/*
 * nRF5340 NETWORK core. There is no application-core branch and there cannot
 * be one: nrf5340_cpuapp has no `radio` node at all, so a build that reached
 * here on cpuapp would fail at RADIANT_TIMER_NODE/NRF_RADIO long before these
 * constants mattered.
 *
 * Fast ramp-up is the same story as nRF52: MODECNF0.RU exists on this part
 * (RADIO_MODECNF0_RU_Msk is defined in nrf5340_network_bitfields.h) and resets
 * to Default, so the 40 us below is only true because radiant_radio_init()
 * calls nrf_radio_fast_ramp_up_enable_set(). That call is already
 * feature-detected on the register mask rather than the SoC, so it needs no
 * change here - but the dependency is why 40 is written and not 140.
 */
#define RAMP_UP_US             40u   /* fast ramp-up, enabled at init */
#define RX_TO_TX_US            40u
#define TX_TO_RX_US            40u
/*
 * BRACKETED THE SAME WAY THE OTHER TWO WERE, AND DELIBERATELY THE LARGEST OF
 * THE THREE. This core is a 64 MHz Cortex-M33 - the same clock as nRF52840
 * with a slightly better core - so a naive reading says "240, same as nRF52".
 * Nordic's own figures say otherwise: nrf_802154_delayed_trx.c budgets
 * TX/RX_SETUP_TIME_MAX 360/290 us for NRF53_SERIES against 270/270 for
 * NRF52_SERIES, i.e. it rates this part *slower* to set up a delayed
 * operation than the nRF52, not faster.
 *
 * 280 us takes Nordic at their word: above the nRF52840's 240 (same clock,
 * rated slower) and below their 290 RX figure for this series. It is an upper
 * bound to be tightened by measurement, and the way to tighten it is to watch
 * RADIANT_RADIO_ETIME refusals stay at zero while lowering it - the same
 * procedure the nRF52 number documents.
 *
 * Erring large is the cheap direction and this file says why: 280 costs 40 us
 * of scheduling slack per window against a 249.7 ms channel period, and widens
 * the scheduler's multi-channel exclusion radius by the same 40 us. Erring
 * small wedges the board with every arm refused and no error visible to the
 * host. ARM_LEAD_US lands at 368 us (280+40+48), which radiant_transfer.h's
 * 1310 us reply-window check clears with 3.5x room.
 */
#define ARM_SETUP_US           280u
#define CAPS_NAME              "nrf53 net RADIO (direct)"
/* One RADIO interrupt line on this core (nrf5340_network.h: RADIO_IRQn = 8);
 * only nRF54L splits the packet events across two. */
#define RADIANT_RADIO_IRQn     RADIO_IRQn
#elif defined(CONFIG_SOC_COMPATIBLE_NRF52X)
#define RAMP_UP_US             40u   /* fast ramp-up, enabled at init */
#define RX_TO_TX_US            40u
#define TX_TO_RX_US            40u
/*
 * Same arm code at half the clock: nRF52840 is 64 MHz Cortex-M4 vs nRF54L15's
 * 128 MHz Cortex-M33, so the nRF54L15 number would silently under-budget
 * here and refuse every arm with RADIANT_RADIO_ETIME.
 *
 * 240 us is bracketed, not guessed: above 2x the nRF54L15 figure (160 us),
 * below Nordic's 270 us for NRF52_SERIES in nrf_802154_delayed_trx.c. It's
 * an upper bound to be tightened by measurement (watch ETIME refusals stay
 * at zero). Erring large costs 80 us of scheduling slack per window against
 * a 249.7 ms period; erring small costs the board.
 */
#define ARM_SETUP_US           240u
#define CAPS_NAME              "nrf52 RADIO (direct)"
#define RADIANT_RADIO_IRQn     RADIO_IRQn
#else
#error "radiant_radio_nrf: no measured constants for this SoC series. Add them \
with the bench measurement that produced them, or select a different backend."
#endif

/*
 * HOW HARD THIS PART CAN TRANSMIT, and it is not the same answer everywhere.
 *
 * nRF52840 and nRF54L15 both carry the full +2..+8 dBm ladder. The nRF5340
 * NETWORK core carries none of it: nrf5340_network_bitfields.h defines
 * RADIO_TXPOWER_TXPOWER_0dBm and then only negative settings (down to
 * Neg40dBm, with a finer -1..-8 ladder than the nRF52 has). 0 dBm is the
 * ceiling in silicon.
 *
 * Feature-detected on the MDK symbol rather than the SoC, for the reason
 * DPPIC_PRESENT is a few hundred lines below: a Zephyr Kconfig rename has
 * silently flipped a default in this project three times, and a missing
 * register symbol is the hardware fact itself.
 *
 * This is a REAL BENCH CONSEQUENCE, not bookkeeping. An nRF5340 endpoint
 * transmits up to 8 dB weaker than an nRF52840 one at the same requested
 * power, so a loss figure measured against it is not comparable with a
 * Feather's without accounting for that. It is also why this feeds
 * caps.tx_power_max_dbm rather than being handled only in the table: the core
 * clamps a requested power against caps (radiant_channel.c), and a caps value
 * of 8 on a part whose ceiling is 0 would have the core report success at a
 * power it never transmitted at.
 */
#if defined(RADIO_TXPOWER_TXPOWER_Pos8dBm)
#define RADIANT_NRF_TXP_MAX_DBM 8
#else
#define RADIANT_NRF_TXP_MAX_DBM 0
#endif

/*
 * ARM_SETUP_US is what min_arm_lead_us bounds: register programming, (D)PPI
 * wiring, the compare having to be written before the counter reaches it.
 * It's per-series because it's CPU time and the parts don't share a clock.
 * Ramp-up and preamble airtime are NOT in it - accounted separately in
 * arm_tx(), since the HAL defines t_sync at the end of the address, not the
 * start of transmission.
 *
 * radiant_transfer.h checks its 1310 us reply-window lead against this, so
 * too-large here fails acknowledged data at config time rather than on air.
 * Worst case below (240+40+48=328 us) has 4x room.
 */

/*
 * min_arm_lead_us must cover everything needed before a frame's t_sync:
 * setup + ramp-up + preamble + longest address. Advertising less than
 * arm_rx() then demands wedges the board - radiant_api.c posts a window at
 * `now + min_arm_lead_us`, every arm is refused RADIANT_RADIO_ETIME, each
 * refusal drives another post, and nothing ever gets on the air with no error
 * visible to the host.
 *
 * THE WORST CASE IS THE CODED PHY'S, ON A BUILD THAT CARRIES IT. At 1 M,
 * preamble+longest address is 48 us; on the coded PHY, preamble+access
 * address is 336 us. That difference does NOT belong in caps.phy_switch_us -
 * phy_switch_us is spent only when the PHY changes, but this lead is owed on
 * every coded operation including back-to-back ones (the common case), so
 * putting it in phy_switch_us would under-lead the steady state and wedge the
 * board.
 *
 * min_arm_lead_us IS THE SCHEDULER'S EXCLUSION RADIUS, not slack, in two
 * places at once:
 *
 *   radiant_sched.c's pass_step() reports a receive window DONE_MISSED as
 *   soon as t_end < now + arm_lead - so no second window can be armed within
 *   arm_lead of the one already committed.
 *
 *   arm_rx_window()'s membership rule 3 (see that function) is what rescues
 *   the pair: two windows that cannot be armed in sequence can still be heard
 *   by one merged window. Its reach is arm_lead for exactly this reason.
 *
 * The two agree by construction, so the lead's real cost is MULTI-CHANNEL: it
 * sets how far apart two tracked channels have to be before the scheduler
 * stops treating them as one piece of work. Widening it for a PHY the image
 * never emits widens that radius for nothing, and a single-channel loss
 * measurement cannot see any of it. That is why the coded PHY is
 * Kconfig-gated.
 *
 * Result with CONFIG_RADIANT_PHY_LR_CODED off, which is the default and the
 * ANT+ case: 168 us on nRF54L15 (80 setup + 40 ramp + 48 air), 328 us on
 * nRF52840 (240 + 40 + 48).
 */
#if RADIANT_NRF_HAS_LR
#define AIR_LEAD_WORST_US      LR_AIR_LEAD_US
#else
#define AIR_LEAD_WORST_US      ((PREAMBLE_BYTES + RADIANT_RADIO_ADDR_MAX) * US_PER_BYTE)
#endif

#define ARM_LEAD_US            (ARM_SETUP_US + RAMP_UP_US + AIR_LEAD_WORST_US)

/*
 * Cost of a PHY change between two operations. Bounded, not measured, same
 * terms as ARM_SETUP_US: a mode change is MODE/PCNF0/PCNF1/CRCCNF writes that
 * apply_format() already performs on every arm and are already inside
 * ARM_SETUP_US, so the genuine incremental cost is near zero. Not literally
 * zero because that would make the scheduler's PHY budgeting dead code on
 * the only shipping backend; 20 us is an upper bound on a dozen register
 * writes at 64 MHz. Does NOT include the coded PHY's longer ramp-up/air
 * lead - those are in ARM_LEAD_US unconditionally, owed on every coded
 * operation, not just the first after a switch.
 */
#if RADIANT_NRF_HAS_LR
#define PHY_SWITCH_US          20u
#else
#define PHY_SWITCH_US          0u
#endif

/* Not const: max_window_us is filled from the gate in radiant_radio_caps_get().
 * Everything else here is a compile-time property of the part and is never
 * written after this initialiser. */
static struct radiant_radio_caps radiant_nrf_caps = {
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
	/* ADR 0022: one wild_last filter covers every devnum_lo, matched as
	 * [A6 C5] in hardware with the last byte received as data - the same
	 * short match ADR 0019 built for tracking. */
	.filter_wildcard_dev = IS_ENABLED(CONFIG_RADIANT_SEARCH_SHORT_MATCH),
	.addr_len_hw_max     = 5,
	.max_body_len        = RADIANT_RADIO_BODY_MAX,

	.phys                = radiant_nrf_phys,
	.n_phys              = (uint8_t)ARRAY_SIZE(radiant_nrf_phys),
	.phy_switch_us       = PHY_SWITCH_US,

	.ramp_up_us          = RAMP_UP_US,
	.rx_to_tx_us         = RX_TO_TX_US,
	.tx_to_rx_us         = TX_TO_RX_US,
	.min_arm_lead_us     = ARM_LEAD_US,
	/* What it costs to programme the peripheral inside air already held.
	 * Equal to the above on the direct gate; the gate raises only the other
	 * one. See the field's comment - the two coming apart is what keeps
	 * acknowledged data working under an arbiter. */
	.min_arm_lead_in_grant_us = ARM_LEAD_US,
	/*
	 * Zero: program_rx() sets the close compare at exactly t_close plus the
	 * longest legal frame's body and CRC, which is the tail radiant_sched.c
	 * already derives from the format itself, so there is no surplus to
	 * declare. Measured on an nRF54L15 DK, search format: the terminal
	 * arrived 149 us after t_close, of which 112 us is that tail and the
	 * remaining ~37 us is the DISABLED->ISR->callback path - real, but not
	 * receiver hold time, and declaring it here would charge the sweep for
	 * it on every backend that shares this number.
	 */
	.rx_close_hold_us    = 0,

	.time_resolution_ns  = 1000,   /* the 1 MHz TIMER */

	/* True: t_sync is a (D)PPI capture of RADIO's own ADDRESS event, with no
	 * software in the path. Note carefully what this does NOT claim - see
	 * T_SYNC_CAL_US, which is now seeded from Zephyr's per-SoC chain delays
	 * rather than zero, and is still a prior rather than a measurement. */
	.has_sync_timestamp  = true,
	.has_rssi            = true,
	/*
	 * Energy detect, tracking the Kconfig rather than the hardware: the
	 * capability is a compile-time property of the backend BUILD (as the HAL
	 * says of caps.phys), and this build compiles no sampling path at all
	 * without CONFIG_RADIANT_ED_SCAN. Advertising true and then refusing
	 * every arm is the one thing the capability query exists to prevent.
	 */
	.has_ed_scan         = IS_ENABLED(CONFIG_RADIANT_ED_SCAN),
	.crc_in_hw           = true,
	/* RXCRC holds the CRC as received, not merely CRCSTATUS's pass/fail
	 * bit, so the core can form an error syndrome and repair a single
	 * flipped bit. The register is documented on both nRF52 and nRF54L. */
	.has_rx_crc          = true,

	.tx_power_min_dbm    = -40,
	/* Per-part; see RADIANT_NRF_TXP_MAX_DBM. 8 on nRF52840/nRF54L15, 0 on
	 * the nRF5340 network core, whose RADIO has no positive setting. */
	.tx_power_max_dbm    = RADIANT_NRF_TXP_MAX_DBM,
};

const struct radiant_radio_caps *radiant_radio_caps_get(void)
{
	/*
	 * max_window_us is a property of the GATE, not of the part: unbounded
	 * where radiant owns the radio, and MPSL's own
	 * MPSL_TIMESLOT_LENGTH_MAX_US where it does not - so it is the one field
	 * filled here rather than in the initialiser. Assigned on every call
	 * rather than once at init because this function is callable BEFORE init
	 * (radiant_sched_init() reads the capabilities to size its own budgets),
	 * and a ceiling that only became true after init would be read as
	 * "unbounded" by exactly the caller that plans against it.
	 */
	radiant_nrf_caps.max_window_us = gate_max_window_us();
	/*
	 * Two leads, only the first moves. min_arm_lead_us is what
	 * radiant_sched.c plans arming against; under an arbiter it must be big
	 * enough to place a reservation (ARM_LEAD_US alone isn't - a build
	 * advertising just that heard nothing, because windows were posted
	 * nearer than the arbiter could grant). min_arm_lead_in_grant_us never
	 * moves: programming inside air already held costs what it always cost,
	 * and radiant_burst.c reads it for acknowledged data.
	 *
	 * Additive, not max: the reservation is placed with the arbiter, THEN
	 * the peripheral is programmed inside the grant - sequential steps, not
	 * alternatives. Taking the max leaves the hardware's own lead
	 * unaccounted for and refuses every arm.
	 */
	radiant_nrf_caps.min_arm_lead_us =
		(uint16_t)(ARM_LEAD_US + gate_min_arm_lead_us());
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
	/* ADR 0018 (d2): true once the gate has its second session. Also kept
	 * current by radiant_nrf_gate_set_elastic_lead(), since the scheduler
	 * caches this pointer and re-reads the fields every pass. */
	radiant_nrf_caps.booking = gate_booking();
#endif
	return &radiant_nrf_caps;
}

#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
void radiant_nrf_gate_set_elastic_lead(uint32_t us)
{
	radiant_nrf_caps.min_arm_lead_elastic_us =
		(us != 0u) ? (uint16_t)(ARM_LEAD_US + us) : 0u;
	radiant_nrf_caps.booking = (us != 0u);
}

void radiant_nrf_gate_set_scan_tail(uint32_t us)
{
	radiant_nrf_caps.scan_tail_us = (uint16_t)us;
}
#endif

/*
 * t_sync is the instant the last address bit was at the antenna; hardware
 * gives us EVENTS_ADDRESS, which differs by demodulator group delay, filter
 * latency and capture offset - a per-part constant, properly measured by
 * pulsing a GPIO from the address-sent event on one board and capturing it
 * on this TIMER on another, minus known cable/air delay.
 *
 * SEEDED FROM ZEPHYR'S BLE CONTROLLER - a defensible prior, not a
 * measurement. Was zero, which is worse: a constant t_sync error cancels out
 * of the period estimate, so drift-lock still succeeds while every window
 * sits off-centre and yield quietly drops a few tenths of a percent (see
 * radiant_radio_hal.h's silent-failure note).
 *
 * Provenance: Zephyr's per-SoC chain delays
 * (zephyr/.../ll_sw/nordic/hal/nrf5/radio/), byte-identical nRF52840/
 * nRF54L15 for 1 Mbit: RX chain delay 9400 ns (capture late vs antenna), TX
 * chain delay 600 ns (event early vs antenna); same sign convention as
 * lll_adv_aux.c.
 *
 * Caveat: these are BLE-1M figures (250 kHz deviation); ANT-1M is ~170 kHz,
 * so RX group delay in particular may differ - not a substitute for the
 * wired two-board trigger the HAL specifies, only a seed pending it. TX is
 * cheap to confirm first: radiant_dbg_tx_err (achieved minus requested
 * t_sync) read -2 us on nRF54L15 with both constants at zero.
 */
#define T_SYNC_CAL_US   (-9)   /* -9.4 us, rounded toward zero */

/*
 * Same constant for the coded PHY - three times as large, and a distinct
 * measurement, not a rounding of the 1M one:
 *
 *     RX chain delay, 1 M:      9.4 us
 *     RX chain delay, S=8:     29.6 us
 *
 * Same provenance/terms as T_SYNC_CAL_US; S=8 figures are byte-identical
 * across nRF52840/nRF5340/nRF54LX. No separate TX constant: Nordic gives TX
 * chain delay as 0.6 us for 1M, S=2 and S=8 alike (modulator/PA delay,
 * unaffected by coding).
 *
 * Reusing the 1M constant here would put every coded receive window 20 us
 * off centre - same silent cancels-out-of-drift-lock failure as above.
 * Still a seed, not a measurement: owed the same bench rig as T_SYNC_CAL_US.
 * See ADR 0007.
 */
#define T_SYNC_CAL_LR_US   (-30)  /* -29.6 us, rounded away from zero */

/* The receive calibration for a PHY. Transmit uses T_SYNC_CAL_TX_US for both;
 * see above. */
static int32_t t_sync_cal_rx_us(enum radiant_phy phy)
{
	return (phy == RADIANT_PHY_LR_CODED) ? T_SYNC_CAL_LR_US : T_SYNC_CAL_US;
}

/*
 * The transmit equivalent, a separate constant on purpose: RX error is
 * demodulator group delay/filter latency, TX error is modulator/PA delay
 * through different silicon - not expected to be close (9.4 us vs 0.6 us).
 */
#define T_SYNC_CAL_TX_US   (1)   /* +0.6 us, rounded away from zero */

/*
 * radiant_radio_tx() folds this into an unsigned lead, so a negative value
 * would wrap to ~4295 seconds and refuse every transmit with ETIME. Physically
 * it can't be negative (antenna can't emit before the modulator), but a wrong
 * edit here would fail silently at runtime, so it's caught at compile time.
 */
BUILD_ASSERT(T_SYNC_CAL_TX_US >= 0,
	     "T_SYNC_CAL_TX_US is folded into an unsigned lead in "
	     "radiant_radio_tx(); a negative value wraps");

/*
 * Apply a calibration to a captured instant, in signed microseconds.
 * radiant_time_t is unsigned and these constants are negative, so the
 * addition is spelled out explicitly rather than left to a cast that would
 * work by modular arithmetic but read like a bug.
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

/* Bit-reverse one byte. The address is emitted LSb-first regardless of
 * PCNF1.ENDIAN, so every address byte needs this going into BASE/PREFIX -
 * confirmed by Spike A: the half of the sweep that skipped it heard nothing. */
static uint8_t rev8(uint8_t b)
{
	b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
	b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
	b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
	return b;
}

/*
 * Split one on-air address into the hardware's BASE and PREFIX. addr[0] is
 * first on the air; the prefix is the last address byte, the base is
 * everything before it, and for BALEN < 4 the used bytes are the HIGH ones:
 *
 *     BASE = (lsb-first packing of the base bytes) << (8 * (4 - BALEN))
 *
 * One expression rather than a case per length. Measured: docs/spike-a-results.md
 * phase A (5-byte) and phase C (3-byte); the shift is what makes them one rule.
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
	/*
	 * Address bytes this window's hardware match leaves to software (ADR
	 * 0019); zero unless a TRACKING window runs under a short
	 * CONFIG_RADIANT_NRF_TRACK_HW_ADDR_LEN. With it non-zero the interrupt
	 * needs the caller's filters to finish the match - the HAL keeps them
	 * alive until the window's terminal event.
	 */
	uint8_t      sw_addr_len;
	const struct radiant_rx_filter *filters;
	/* The format's CRC seed, for radiant_addr_split_repair()'s software
	 * check of a frame with a slipped bit (CONFIG_RADIANT_NRF_SLIP_GUARD). */
	uint16_t     crc_init;
	/*
	 * On a static-length format: the length of every frame in this window.
	 * On a length-from-body format: the format's CEILING, against which the
	 * length byte the frame actually carried is clamped. The flag below says
	 * which, and it is a flag rather than an inference from body_len because
	 * "body_len is the answer" and "body_len is the bound" are two different
	 * meanings for one field and the ISR must not have to guess.
	 */
	uint8_t      body_len;
	bool         len_from_body;
	/*
	 * The PHY this operation was armed on, kept because the interrupt needs
	 * it and the request is long gone by then. It decides the t_sync
	 * calibration, which differs between the two PHYs by 20 us - see
	 * T_SYNC_CAL_LR_US. Latching it at the arm rather than reading
	 * RADIO->MODE back in the ISR keeps the ISR free of register reads and
	 * cannot disagree with what was actually programmed, because the same
	 * arm call did both.
	 */
	enum radiant_phy phy;
	bool         stop_on_first;
	bool         report_crc_fail;
	/*
	 * This operation has had its one terminal event. See CLAIM_TERMINAL();
	 * cleared at the arm under the same lock that claims the slot, not in
	 * program_rx()/program_tx()/program_ed().
	 *
	 * Under an arbitrated gate those two instants are milliseconds apart:
	 * an operation armed and waiting for air has no registers programmed,
	 * so clearing it at programming time inherits the PREVIOUS operation's
	 * `true` and the operation becomes unterminatable - deliver_terminal()
	 * silently refuses it exactly when the gate needs to report that the air
	 * never came (measured: a dongle swept twice and stopped, slot stuck
	 * forever, chunks=2, end ok=1, deny=0).
	 */
	bool         terminal_sent;

	/*
	 * Whether this receive window has delivered a frame, of either status.
	 * Exists for the noise floor only: a window that heard nothing
	 * measures the band; one that heard something has RSSISAMPLE holding
	 * the packet's own level instead. Without this bool, noise samples
	 * from a busy channel would wear a transmitter's signal strength.
	 */
	bool         rx_any;

	/*
	 * The t_sync a transmit asked for, kept so the interrupt can compare it
	 * to the t_sync hardware actually captured - this backend's transmit
	 * timing error, measured on the part. Worth having because a fixed
	 * lateness is invisible on broadcast (+/-400 us window, re-anchors
	 * every frame) but fatal for acknowledged data (peer's window opens
	 * only 1560 us after the packet it sent).
	 */
	radiant_time_t t_sync_req;

#if defined(CONFIG_RADIANT_ED_SCAN)
	/*
	 * The energy-detect sweep in flight.
	 *
	 * ed_deadline_cc is a raw timer counter value, not a radiant_time_t,
	 * deliberately: it is compared inside the sampling loop in the radio
	 * interrupt, and clock_absolute() takes an interrupt lock and a fold on
	 * every call. A 32-bit compare is the same arithmetic
	 * radio_disable_now() uses, correct within 35 min of the last fold. It
	 * advances by one dwell per index rather than being recomputed, so the
	 * whole sweep is bounded by n * dwell from the operation's own start.
	 *
	 * No ed_lo: the sweep never wraps inside one operation (the scheduler
	 * splits a range that would), so the low end is only ever ed_cur's
	 * start value - a second copy would be state that could disagree.
	 */
	uint8_t  ed_hi;
	uint8_t  ed_cur;
	uint32_t ed_dwell_us;
	uint32_t ed_deadline_cc;
#endif

	/*
	 * (D)PPI connections, allocated once at init. nrfx exposes connections
	 * rather than bare channels because on nRF54L the RADIO and TIMER can
	 * sit in different DPPI domains and a connection routes across them;
	 * allocating per endpoint pair also rules out an index slip wiring an
	 * edge to the wrong task.
	 */
	nrfx_gppi_handle_t conn_sync;
	nrfx_gppi_handle_t conn_start;    /* CC_START -> TASKS_RXEN */
	nrfx_gppi_handle_t conn_start_tx; /* CC_START -> TASKS_TXEN */
	nrfx_gppi_handle_t conn_close;
	/*
	 * RXREADY -> TASKS_RSSISTART: one RSSI sample as soon as the receiver
	 * is live, before anything could have transmitted into the window.
	 * The existing ADDRESS -> RSSISTART short only samples on packet
	 * arrival, so a quiet window - exactly the one whose level is the
	 * noise floor - would never sample at all. RXREADY, not the start
	 * compare, because the receiver isn't on yet at the compare. No
	 * contention: RSSISTART is one-shot, so a window that does receive
	 * something overwrites this sample on ADDRESS anyway.
	 */
	nrfx_gppi_handle_t conn_rssi;
	bool               conn_ok;
	/* A grant arrived but its programming failed. The terminal is delivered
	 * later, from thread context, so the signal callback stays short - see
	 * radiant_nrf_gate_on_grant(). */
	bool               gate_failed;

#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
	/* Per-window evidence for CONFIG_RADIANT_NRF_RX_DIAG; see rx_diag below. */
	bool               diag_track;
	bool               diag_ok;
	bool               diag_crc;
	bool               diag_end_noaddr;
	bool               diag_cut;
	radiant_time_t     diag_t_open;
#endif
} radiant_op;

/*
 * The request an arm call validated but has not programmed yet.
 *
 * Copied rather than pointed to: radiant_rx_req is a stack local in
 * radiant_sched.c's arm_rx_window() and is gone the instant that function
 * returns, milliseconds before an arbitrated grant arrives. The inner
 * pointers (filters, body) have the HAL's lifetime promise; the struct
 * holding them does not, so only the struct is copied. Only one is live at a
 * time, same reason radiant_op.kind is one field (HAL rule, also enforced by
 * MPSL with -NRF_EAGAIN on a second request).
 */
static struct {
	struct radiant_rx_req rx;
	struct radiant_tx_req tx;
#if defined(CONFIG_RADIANT_ED_SCAN)
	struct radiant_ed_req ed;
#endif
	/* True between a GATE_PENDING acquire and the grant or denial that
	 * resolves it. Read by the gate callbacks to tell a real deferred
	 * operation from a late signal for one that has already ended. */
	bool pending;
} radiant_staged;

/* Bring-up counters. Logging from the RADIO interrupt is not safe at this
 * priority, so the interrupt counts and thread context reports. */
static volatile uint32_t radiant_dbg_isr;
static volatile uint32_t radiant_dbg_rx_ev;
static volatile uint32_t radiant_dbg_term;
static volatile uint32_t radiant_dbg_ok;
/* ENDs on a receive window with no address match behind them - replays of the
 * previous frame, suppressed in radio_isr(). Must stay small: a few per window
 * teardown is the shape it has; one per channel period forever is the defect it
 * was added for. */
static volatile uint32_t radiant_dbg_rx_no_addr;
/* Achieved t_sync minus requested t_sync on the last transmit, microseconds. */
static volatile int32_t  radiant_dbg_tx_err;
/* Receive windows refused because CC_START landed behind the counter while it
 * was being written (review 2026-10-31, A14-1). Expected 0 or single digits. */
static volatile uint32_t radiant_dbg_start_late;

/* The DMA buffer the RADIO reads and writes. Backend-owned, handed to the core
 * as rx_event.body for the duration of one callback only, which is exactly what
 * radiant_radio_hal.h says about it. */
static uint8_t radiant_rx_buf[RADIANT_RADIO_BODY_MAX + 4] __aligned(4);

/*
 * Transmit buffer. The body is copied here rather than DMA'd from where the
 * core put it - the HAL permits either, but copying ten bytes costs nothing
 * against the 80 us setup budget and guarantees EasyDMA-reachable, aligned,
 * non-flash memory that the HAL contract doesn't itself promise.
 */
static uint8_t radiant_tx_buf[RADIANT_RADIO_BODY_MAX + 4] __aligned(4);

/* A frame radiant_addr_split_repair() reconstructed (ADR 0019): its body is
 * the received bits shifted by one, so it cannot be handed over in place in
 * the DMA buffer. Same one-callback lifetime as that buffer. */
static uint8_t radiant_repair_buf[RADIANT_RADIO_BODY_MAX];
/* Frames that reached the core only because the slip repair recovered them. */
static volatile uint32_t radiant_dbg_slip_repaired;

#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
/*
 * CONFIG_RADIANT_NRF_RX_DIAG: what a tracked receive window actually saw. See
 * that symbol's help for the five outcomes. Written only by the radio
 * interrupt (and the terminal path it shares with aborts), read by a 10 s
 * work item; torn reads of a counter are acceptable for a bench readout.
 */
#define RX_DIAG_RING      8u
#define RX_DIAG_BODY_MAX 16u

static struct {
	uint32_t win;            /* tracked windows that ended */
	uint32_t win_ok;
	uint32_t win_crc;
	uint32_t win_cut;
	uint32_t win_end_noaddr;
	uint32_t win_silent;
	uint32_t win_abort;      /* ended by an abort or a denied grant */
	uint32_t fr_crc;         /* CRC-failed frames on tracked windows */
	uint32_t fr_end_noaddr;  /* ENDs with no ADDRESS on tracked windows */
	uint32_t fr_foreign;     /* short-address matches the software compare
				  * rejected (ADR 0019): another sensor's frame,
				  * noise, or a slip the repair could not fix */
	uint32_t fr_repaired;    /* slipped frames recovered and delivered */
	uint8_t  hw_last;        /* hardware-matched bytes, last tracked window */
	int32_t  off_min;        /* OK frame t_sync - window t_open, us */
	int32_t  off_max;
	struct {
		uint32_t t_ms;
		uint32_t rxcrc;
		uint8_t  rxmatch;
		int8_t   rssi;
		uint8_t  len;
		uint8_t  body[RX_DIAG_BODY_MAX];
	} ring[RX_DIAG_RING];
	uint32_t ring_w;         /* total entries ever written */
	uint32_t ring_r;         /* total entries ever printed */
} rx_diag = { .off_min = INT32_MAX, .off_max = INT32_MIN };

static struct k_work_delayable rx_diag_work;

/* One outcome per tracked window, best first. Called exactly once per window:
 * from the STOP_ON_FIRST terminal or from deliver_terminal(). */
static void rx_diag_window_end(bool aborted)
{
	if (!radiant_op.diag_track) {
		return;
	}
	radiant_op.diag_track = false;
	rx_diag.win++;
	if (radiant_op.diag_ok) {
		rx_diag.win_ok++;
	} else if (radiant_op.diag_crc) {
		rx_diag.win_crc++;
	} else if (radiant_op.diag_cut) {
		rx_diag.win_cut++;
	} else if (aborted) {
		rx_diag.win_abort++;
	} else if (radiant_op.diag_end_noaddr) {
		rx_diag.win_end_noaddr++;
	} else {
		rx_diag.win_silent++;
	}
}

static void rx_diag_crc_fail(uint8_t logical, uint32_t rxcrc, int8_t rssi)
{
	uint32_t slot = rx_diag.ring_w % RX_DIAG_RING;
	/* Raw, software-matched address bytes included (ADR 0019). */
	uint8_t n = MIN((uint8_t)(radiant_op.body_len + radiant_op.sw_addr_len),
			(uint8_t)RX_DIAG_BODY_MAX);

	rx_diag.ring[slot].t_ms = k_uptime_get_32();
	rx_diag.ring[slot].rxcrc = rxcrc;
	rx_diag.ring[slot].rxmatch = logical;
	rx_diag.ring[slot].rssi = rssi;
	rx_diag.ring[slot].len = n;
	memcpy(rx_diag.ring[slot].body, radiant_rx_buf, n);
	rx_diag.ring_w++;
}

static void rx_diag_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	LOG_INF("rxdiag hw=%u win=%u ok=%u crc=%u cut=%u noaddr=%u silent=%u "
		"abort=%u fr_crc=%u fr_noaddr=%u fr_foreign=%u fr_repaired=%u "
		"off=%d..%d dbg_noaddr=%u",
		(unsigned)rx_diag.hw_last,
		rx_diag.win, rx_diag.win_ok, rx_diag.win_crc, rx_diag.win_cut,
		rx_diag.win_end_noaddr, rx_diag.win_silent, rx_diag.win_abort,
		rx_diag.fr_crc, rx_diag.fr_end_noaddr, rx_diag.fr_foreign,
		rx_diag.fr_repaired,
		(int)rx_diag.off_min, (int)rx_diag.off_max,
		(unsigned)radiant_dbg_rx_no_addr);

	/* Anything overwritten before it was printed is reported as a count. */
	if (rx_diag.ring_w - rx_diag.ring_r > RX_DIAG_RING) {
		LOG_INF("rxdiag ring overrun: %u crc entries not printed",
			rx_diag.ring_w - rx_diag.ring_r - RX_DIAG_RING);
		rx_diag.ring_r = rx_diag.ring_w - RX_DIAG_RING;
	}
	while (rx_diag.ring_r != rx_diag.ring_w) {
		char hex[RX_DIAG_BODY_MAX * 2u + 1u];
		uint32_t slot = rx_diag.ring_r % RX_DIAG_RING;
		unsigned int key = irq_lock();
		uint32_t t_ms = rx_diag.ring[slot].t_ms;
		uint32_t rxcrc = rx_diag.ring[slot].rxcrc;
		uint8_t rxm = rx_diag.ring[slot].rxmatch;
		int8_t rssi = rx_diag.ring[slot].rssi;
		uint8_t n = rx_diag.ring[slot].len;
		uint8_t body[RX_DIAG_BODY_MAX];

		memcpy(body, rx_diag.ring[slot].body, n);
		irq_unlock(key);
		for (uint8_t i = 0; i < n; i++) {
			snprintk(&hex[2u * i], 3, "%02x", body[i]);
		}
		hex[2u * n] = '\0';
		/* Bit 31: rejected by the software address compare with a
		 * passing CRC; bit 30: rejected with a failing one; neither: an
		 * accepted frame whose CRC failed. */
		LOG_INF("rxdiag %s t=%u rxmatch=%u rssi=%d rxcrc=%04x body=%s",
			(rxcrc & 0x80000000u) ? "foreign_crcok" :
			(rxcrc & 0x40000000u) ? "foreign_crcbad" : "crcfail",
			t_ms, rxm, rssi, (unsigned)(rxcrc & 0xFFFFFFu), hex);
		rx_diag.ring_r++;
	}
	k_work_reschedule(&rx_diag_work, K_SECONDS(10));
}
#endif /* CONFIG_RADIANT_NRF_RX_DIAG */

/* ---------------------------------------------------------------------------
 * Register programming
 * ---------------------------------------------------------------------------
 */

/*
 * Address bytes a receive window on this format leaves to software (ADR 0019,
 * CONFIG_RADIANT_NRF_TRACK_HW_ADDR_LEN). Zero for every format except TRACKING
 * on 1 M, and zero for every transmit - the transmitter always emits the full
 * address, which is what lets a receiver match any prefix of it.
 */
#if defined(CONFIG_RADIANT_NRF_SLIP_GUARD)
#define RX_SLIP_GUARD_RUN CONFIG_RADIANT_NRF_SLIP_GUARD_RUN
#else
#define RX_SLIP_GUARD_RUN 0
#endif

/*
 * Per window, from all of its filters: CONFIG_RADIANT_NRF_SLIP_GUARD shortens
 * the match to the two fixed bytes when any filter's address is one the
 * demodulator slips on, and leaves every other window at the full match.
 */
static uint8_t rx_sw_addr_len(const struct radiant_rx_req *req)
{
	const struct radiant_pkt_format *fmt = req->fmt;

	return (uint8_t)(fmt->addr_len -
			 radiant_addr_split_rx_hw_len(
				 req->filters, req->n_filters, fmt->addr_len,
				 fmt->phy == RADIANT_PHY_1M_GFSK,
				 CONFIG_RADIANT_NRF_TRACK_HW_ADDR_LEN,
				 RX_SLIP_GUARD_RUN));
}

/*
 * sw_addr_len: how many of the format's trailing address bytes the hardware
 * receives as payload instead of matching (always 0 on transmit; see
 * rx_sw_addr_len()). BALEN shrinks by that many and STATLEN grows by it, so
 * the bit stream the RADIO clocks in - and the CRC over it, with SKIPADDR
 * Include - is unchanged; only where the matcher stops differs.
 */
static int apply_format(const struct radiant_pkt_format *fmt, uint8_t rf_index,
			uint8_t sw_addr_len, bool dry_run)
{
	bool lr;

	if (fmt == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (fmt->phy != RADIANT_PHY_1M_GFSK && fmt->phy != RADIANT_PHY_LR_CODED) {
		return RADIANT_RADIO_ENOTSUP;
	}
#if RADIANT_NRF_HAS_LR
	lr = (fmt->phy == RADIANT_PHY_LR_CODED);
#else
	/*
	 * A part whose RADIO has no coded mode, or an image that did not ask
	 * for it (CONFIG_RADIANT_PHY_LR_CODED - see RADIANT_NRF_HAS_LR).
	 * Refused, never approximated: "close" here is a 1 M frame nobody is
	 * listening for. Refused HERE with `lr` pinned false, so every
	 * `if (lr)` below folds against a compile-time false and no coded
	 * branch is half-compiled.
	 */
	if (fmt->phy == RADIANT_PHY_LR_CODED) {
		return RADIANT_RADIO_ENOTSUP;
	}
	lr = false;
#endif
	if (fmt->addr_len < 2u || fmt->addr_len > 5u) {
		return RADIANT_RADIO_ENOTSUP;
	}
	/* The hardware still needs a base byte and a prefix byte; and the coded
	 * PHY's access address is fixed, so nothing is ever left to software
	 * there. */
	if (sw_addr_len != 0u &&
	    (lr || (uint32_t)fmt->addr_len < (uint32_t)sw_addr_len + 2u)) {
		return RADIANT_RADIO_EINVAL;
	}
	/*
	 * The coded PHY's access address is a FIXED 32 bits. Four address bytes
	 * is not a preference here, it is the only length the hardware will
	 * emit, so anything else is refused rather than padded or truncated -
	 * either of which would put a frame on the air that no receiver this
	 * project configures is matching.
	 */
	if (lr && fmt->addr_len != 4u) {
		return RADIANT_RADIO_ENOTSUP;
	}
	if (rf_index > RADIANT_RF_INDEX_MAX) {
		return RADIANT_RADIO_EINVAL;
	}

	/*
	 * Static length on 1 M is a measurement, not a simplification: Spike B
	 * found byte 3 of an ANT frame is a control byte (six fields), not a
	 * length. The LFLEN=8/CRCINC=1 config Spike A also found working only
	 * looked right because a broadcast's control byte happens to read
	 * 0x0A, which happens to be the right length - it would silently
	 * truncate every acknowledged/burst frame, so length-decoding is
	 * refused on 1 M.
	 *
	 * The refusal is tied to the PHY, not blanket, because the defect is
	 * ANT-specific (the length-offset byte already means something else);
	 * the coded PHY has no such pre-existing byte, so LFLEN=8 there
	 * decodes exactly what was asked. Tying the two together stops a
	 * future edit re-enabling the length field on 1 M by loosening an
	 * unrelated check.
	 */
	if (lr) {
		if (fmt->len_mode != RADIANT_LEN_FROM_BODY) {
			return RADIANT_RADIO_ENOTSUP;
		}
		/*
		 * The RADIO's LENGTH field counts the bytes after itself, so
		 * the HAL's body is the length byte plus those. Any other
		 * offset or bias would need software to rewrite the byte
		 * between the core and EasyDMA, which is a translation layer
		 * this backend refuses to grow.
		 */
		if (fmt->len_offset != 0u || fmt->len_bias != 1) {
			return RADIANT_RADIO_ENOTSUP;
		}
		if (fmt->max_body_len < 2u ||
		    fmt->max_body_len > RADIANT_RADIO_BODY_MAX) {
			return RADIANT_RADIO_EINVAL;
		}
	} else {
		if (fmt->len_mode != RADIANT_LEN_FIXED) {
			return RADIANT_RADIO_ENOTSUP;
		}
		if (fmt->body_len == 0u ||
		    fmt->body_len > RADIANT_RADIO_BODY_MAX) {
			return RADIANT_RADIO_EINVAL;
		}
	}

	/*
	 * CRC-16/CCITT-FALSE, every parameter checked, not just width: the
	 * RADIO's CRC engine has no reflection or final-XOR option, so a
	 * format asking for either must be refused rather than computing the
	 * wrong CRC and having the intended receiver reject it.
	 */
	if (fmt->crc.width_bits != 16u || fmt->crc.poly != 0x1021u ||
	    fmt->crc.xor_out != 0u || fmt->crc.reflect_in || fmt->crc.reflect_out) {
		return RADIANT_RADIO_ENOTSUP;
	}

	/*
	 * Everything above is a pure function of the request; everything below
	 * touches the peripheral - naming that boundary is what lets an arm
	 * call answer EINVAL/ENOTSUP synchronously while register writes wait
	 * for the gate. One function with a flag rather than a separate
	 * check_format(), which would be a second copy of these refusals that
	 * could drift out of step with this one.
	 */
	if (dry_run) {
		return RADIANT_RADIO_OK_RC;
	}

#if RADIANT_NRF_HAS_LR
	if (lr) {
		/*
		 * S=8 only. LR500Kbit is deliberately not selectable: FEC
		 * block 1 is coded at S=8 regardless of data rate, so S=2 is
		 * only ~2.1x cheaper than S=8 here, not 4x, for ~3 dB given
		 * up. One rate keeps one format, one phy_switch_us, one
		 * t_sync calibration. See ADR 0007. LR125Kbit means 125 kbit/s
		 * TX and 125 OR 500 kbit/s RX, so S=8 costs nothing on receive.
		 */
		nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_BLE_LR125KBIT);
	} else
#endif
	{
		nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);
	}
	nrf_radio_frequency_set(NRF_RADIO, (uint16_t)(2400u + rf_index));

	if (lr) {
		/*
		 * PCNF0 for the coded PHY, and every field in it is required.
		 *
		 *   PLEN = LongRange   the 80 us coded preamble. The 8-bit
		 *                      preamble the ANT path uses does not
		 *                      exist on this PHY.
		 *   CILEN = 2          the coding indicator, in bits. It is
		 *                      what tells the receiver whether FEC
		 *                      block 2 is S=8 or S=2, and a zero here
		 *                      is not "no indicator" - it is a frame no
		 *                      coded receiver can interpret.
		 *   TERMLEN = 3        TERM1, the convolutional coder's
		 *                      terminator. Also required, also silently
		 *                      fatal at zero.
		 *   LFLEN = 8          the length field. This is the register
		 *                      the ANT path must never use and the one
		 *                      the whole length extension is.
		 *   S0LEN = 0, S1LEN = 0
		 *                      nothing ahead of the length byte, so the
		 *                      first body byte on air IS the length -
		 *                      which is what makes len_offset 0 and
		 *                      len_bias 1 the only pair this backend
		 *                      accepts.
		 */
		NRF_RADIO->PCNF0 =
			((uint32_t)RADIO_PCNF0_PLEN_LongRange
				<< RADIO_PCNF0_PLEN_Pos) |
			((uint32_t)2u << RADIO_PCNF0_CILEN_Pos) |
			((uint32_t)3u << RADIO_PCNF0_TERMLEN_Pos) |
			((uint32_t)8u << RADIO_PCNF0_LFLEN_Pos);
		/*
		 * STATLEN = 0: the length field alone decides how many bytes
		 * leave the antenna, which is the entire point of this format.
		 * MAXLEN bounds what the receiver will accept into the buffer
		 * and is the format's own max_body_len minus the length byte -
		 * NOT sizeof(radiant_rx_buf), which is what the static path
		 * uses because there the length is not on the air. A MAXLEN
		 * larger than the format allows would let a corrupted length
		 * byte fill the buffer; the CRC would then reject the frame, so
		 * the failure is a loss rather than an overrun, but the bound
		 * belongs where the format states it.
		 */
		NRF_RADIO->PCNF1 =
			((uint32_t)(fmt->max_body_len - 1u)
				<< RADIO_PCNF1_MAXLEN_Pos) |
			((uint32_t)0u                   << RADIO_PCNF1_STATLEN_Pos) |
			((uint32_t)(fmt->addr_len - 1u) << RADIO_PCNF1_BALEN_Pos) |
			(RADIO_PCNF1_ENDIAN_Big         << RADIO_PCNF1_ENDIAN_Pos) |
			(RADIO_PCNF1_WHITEEN_Disabled   << RADIO_PCNF1_WHITEEN_Pos);
	} else {
		NRF_RADIO->PCNF0 = 0u;
		NRF_RADIO->PCNF1 =
			((uint32_t)sizeof(radiant_rx_buf) << RADIO_PCNF1_MAXLEN_Pos) |
			((uint32_t)(fmt->body_len + sw_addr_len)
				<< RADIO_PCNF1_STATLEN_Pos) |
			((uint32_t)(fmt->addr_len - sw_addr_len - 1u)
				<< RADIO_PCNF1_BALEN_Pos) |
			(RADIO_PCNF1_ENDIAN_Big           << RADIO_PCNF1_ENDIAN_Pos) |
			(RADIO_PCNF1_WHITEEN_Disabled     << RADIO_PCNF1_WHITEEN_Pos);
	}

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
 * Transmit power. The register field is NOT dBm, though on nRF52840 it looks
 * like it is (+8 dBm = 0x08, 0 dBm = 0x00 - `TXPOWER = (int8_t)dbm` would
 * pass review). nRF54L15 encodes +8 dBm as 0x3F, 0 dBm as 0x18; the same
 * expression there transmits at an unrelated power with no error. So the
 * mapping goes through the part's own MDK symbols, never the raw number - so a
 * part that names a power differently fails to compile rather than
 * transmitting at a guess.
 *
 * THE POSITIVE HALF IS GUARDED, AND THE ORIGINAL INVARIANT SURVIVES IT. This
 * comment used to say "every entry below is defined on both parts". That
 * stopped being true when the nRF5340 network core arrived, whose RADIO has no
 * positive setting at all (RADIANT_NRF_TXP_MAX_DBM). The guard is a threshold
 * rather than seven independent defined() tests on purpose: a part that has
 * Pos8dBm but is missing, say, Pos5dBm still reaches that row and still fails
 * to compile, which is exactly the protection the old wording bought. What is
 * given up is only the case of a part with a genuinely gapped ladder, and no
 * such part exists in this family.
 *
 * A SHORTER TABLE IS SAFE HERE and does not need a matching change in
 * apply_power(): its nearest-match loop is over ARRAY_SIZE, and
 * radiant_radio_hal.h states the contract outright - "a backend rounds to the
 * nearest setting it has and does not fail for an unreachable value". The
 * failure mode this guard cannot have is the dangerous one: a missing symbol
 * removes a row, it never produces a wrong register value.
 */
struct radiant_txp {
	int8_t              dbm;
	nrf_radio_txpower_t reg;
};

static const struct radiant_txp radiant_txp_table[] = {
#if RADIANT_NRF_TXP_MAX_DBM >= 8
	{   8, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos8dBm  },
	{   7, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos7dBm  },
	{   6, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos6dBm  },
	{   5, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos5dBm  },
	{   4, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos4dBm  },
	{   3, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos3dBm  },
	{   2, (nrf_radio_txpower_t)RADIO_TXPOWER_TXPOWER_Pos2dBm  },
#endif
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
 * Programme up to eight receive filters, or refuse the set. See this file's
 * header for why eight is not eight arbitrary addresses: the set must use at
 * most two distinct bases, with at most one filter on the odd one out.
 * Refusal (RADIANT_RADIO_ENOTSUP) is not negotiable - silently programming a
 * subset would lose frames undiagnosably.
 */
/* Logical address -> index into the caller's filters[]. See the end of
 * apply_filters() for why the inverse map has to exist. */
static uint8_t radiant_filter_slot[8];

/*
 * hw_len: how many leading bytes of each filter the hardware matches - the
 * format's addr_len, or fewer under CONFIG_RADIANT_NRF_TRACK_HW_ADDR_LEN. With
 * a short match several filters can pack to one identical hardware address;
 * each still takes its own logical address (eight filters fit either way), and
 * radiant_addr_split_match() picks the real one by its remaining bytes.
 */
static int apply_filters(const struct radiant_rx_filter *filters, uint8_t n,
			 uint8_t addr_len, uint8_t hw_len, bool dry_run)
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

		if (filters[i].addr_len != addr_len || hw_len > addr_len ||
		    hw_len < 2u) {
			return RADIANT_RADIO_EINVAL;
		}
		pack_address(filters[i].addr, hw_len, &b, &p);

		if (!have_base1) {
			base1 = b;
			have_base1 = true;
		}

		if (b == base1) {
			if (n_base1 >= 7u) {
				/*
				 * Eight sharing a base is one more than BASE1's
				 * seven prefixes, so the eighth goes to logical 0,
				 * which carries its own base as well as its own
				 * prefix. prefixes[0] must be set here too: without
				 * it the eighth filter of a full set silently
				 * matches prefix 0x00, invisibly dropping every
				 * eighth device number in a wildcard sweep.
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

	/*
	 * Same boundary as apply_format(): the packing loop above is the whole
	 * decision of which filter lands on which logical address, and it can
	 * refuse a set - so a dry run must run all of it and skip only the
	 * register writes below, rather than reimplementing the packer.
	 */
	if (dry_run) {
		return RADIANT_RADIO_OK_RC;
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
 * Programme the one address a transmit emits. TXADDRESS names a LOGICAL
 * address (an index into the same BASE/PREFIX registers apply_filters()
 * writes), so this writes BASE0/PREFIX0.AP0 unconditionally rather than
 * emitting whatever a previous operation left loaded. Logical 0, not 1..7,
 * because it's the only one with its own base - 1..7 share BASE1, which
 * would make the transmitted address depend on the last receive window.
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
 * Air between switching the radio on and t_sync: ramp-up plus the whole
 * preamble and address, derived from the definition rather than tuned, so
 * neither the window edge nor the transmit start silently carries one
 * backend's ramp-up.
 *
 * Per PHY, and the two answers differ by a factor of seven - getting it wrong
 * either puts a transmit's address on air late, or opens/closes a receive
 * window around the wrong instant. addr_len is ignored on the coded PHY
 * because its access address is a fixed 32 bits (the format's four address
 * bytes ARE those 32 bits).
 */
static uint32_t air_lead_us(enum radiant_phy phy, uint8_t addr_len)
{
	if (phy == RADIANT_PHY_LR_CODED) {
		return RAMP_UP_US + LR_PREAMBLE_US + LR_ACCESS_ADDR_US;
	}
	return RAMP_UP_US +
	       (uint32_t)(PREAMBLE_BYTES + addr_len) * US_PER_BYTE;
}

/* ---------------------------------------------------------------------------
 * RSSI temperature correction - errata 153 (nRF52840) / 225 (nRF52833)
 *
 * Re-sourced, not re-derived: the seven thresholds and eighth
 * beyond-erratum branch below are copied verbatim from Nordic's open-source
 * 802.15.4 driver (nrf_802154_rssi.c,
 * nrf_802154_rssi_sample_temp_corr_value_get(), BSD-3-Clause, already a
 * dependency the way nrf52_erratas.h is) - not this project's own
 * measurement, unlike every other correction constant here.
 *
 * The sign IS re-derived: Nordic's function corrects a positive register
 * magnitude (`sample + corr`); this file works in negated dBm, so
 * `corrected_dbm = raw_dbm - corr`. Watch for double-counting if code later
 * compares evt.rssi_dbm against a threshold - Nordic applies the opposite
 * sign to a threshold (`cca_ed - corr`), which would double the correction
 * if applied the same direction as the sample (not an issue today:
 * radiant_search.c's cfg.min_rssi_dbm isn't itself temperature-corrected).
 *
 * Not present on nRF54L: nrf_802154_rssi.c implements NRF52_SERIES (this
 * table) and NRF53_SERIES (errata 87, unrelated) and returns flat 0
 * elsewhere, so this correction compiles in for nRF52840/nRF52833 only and
 * nRF54L15 RSSI stays uncorrected, matching Nordic's own choice.
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
 * Cached, not sampled per packet: a TEMP conversion cannot run in the radio
 * ISR. 20 degC is not a guess - it's nrf_802154_temperature_zephyr.c's
 * DEFAULT_TEMPERATURE and also this table's zero-correction band centre, so
 * an unrefreshed cache costs nothing.
 */
static int8_t rssi_temp_cache_c = 20;

/* 60 s, matching CONFIG_NRF_802154_TEMPERATURE_UPDATE_PERIOD's default for
 * the identical job; die temperature moves too slowly for a 20 degC-wide
 * band to need shorter. */
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
 * RSSISAMPLE, in dBm, corrected. The only place this register is read: the
 * HAL requires rssi_dbm, noise_dbm and the ED min/mean on ONE scale (same
 * corrections, same reference) since they're always used as differences -
 * three separate copies of the negation/correction would be three chances to
 * disagree. Register holds a positive magnitude, hence the negation.
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

/*
 * A newly claimed operation has not had its terminal event yet. Written as a
 * macro so the three arm calls say the same thing in the same place - beside
 * `radiant_op.kind = OP_x`, under the same irq_lock, because the two facts are
 * one fact: the slot is claimed AND it is owed exactly one terminal.
 */
#define CLAIM_TERMINAL() (radiant_op.terminal_sent = false)

/*
 * Not under #if CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL: radiant_radio_enable()/
 * disable() reference radio_ep_contended, RADIO_EP_COUNT etc.
 * unconditionally, so guarding the definitions while leaving the uses exposed
 * breaks every non-gate nRF build. Compiling it unconditionally costs 25
 * bytes of .bss and a six-register loop that finds nothing when there's no
 * second stack.
 *
 * IT IS STILL GATED ON THE SILICON, AND MUST BE. RADIO's PUBLISH_/SUBSCRIBE_
 * registers do not exist on a part with the old PPI: on nRF52840 the six
 * lines of radio_ep_reg() are six hard compile errors ('NRF_RADIO_Type' has
 * no member named 'PUBLISH_ADDRESS').
 *
 * DPPIC_PRESENT is the MDK's own discriminator, from <part>_peripherals.h,
 * and it is the hardware fact rather than a Zephyr SoC symbol on purpose:
 * this project has been bitten three times by a Zephyr Kconfig rename
 * silently flipping a default. Note it is defined as EMPTY on nRF53 and as 1
 * on nRF54L, so it must be tested with defined() and never with #if.
 *
 * On a PPI part the RADIO has no endpoint register for another stack to
 * have taken, so there is nothing to borrow, nothing can be contended, and
 * the whole layer is inert by construction rather than by measurement.
 */
#if defined(DPPIC_PRESENT)
#define RADIANT_NRF_RADIO_EP 1
#else
#define RADIANT_NRF_RADIO_EP 0
#endif

/*
 * ---------------------------------------------------------------------------
 * DO WE HOLD THE RADIO AT THIS INSTANT?
 * ---------------------------------------------------------------------------
 *
 * Under the MPSL gate the RADIO is ours only between SIGNAL_START and the
 * ACTION_END that closes the timeslot. Outside that window it belongs to
 * whoever MPSL gave it to - beside OpenThread that is the nRF 802.15.4 driver,
 * which is receiving on it continuously.
 *
 * Three functions in this file write SHARED RADIO registers and must not do so
 * from ordinary thread/work-queue context:
 *
 *   deliver_terminal()     nrf_radio_shorts_set(NRF_RADIO, 0)
 *   radiant_radio_abort()  the same, plus TASKS_STOP and a forced TASKS_DISABLE
 *   ..._gate_on_grant_end() the same, plus TASKS_DISABLE and the endpoint swap
 *
 * A terminal is delivered for every operation INCLUDING a denied one, and a
 * denial is delivered from the gate's work queue with no timeslot held at all.
 * Measured on this board (nRF54L15 DK, thread.conf;bridge.conf, 2026-08-15):
 * every real reservation was refused by the arbiter, so the core ran a denial
 * loop at ~80 terminals a second, each one clearing RADIO->SHORTS underneath a
 * driver that was mid-frame on the same peripheral.
 *
 * SHORTS is what makes that fatal rather than merely rude. nrf_802154 receives
 * with SHORTS_RX = ADDRESS_RSSISTART | PHYEND_DISABLE | ADDRESS_BCSTART, and
 * PHYEND_DISABLE is the ONLY thing that takes its radio down at the end of a
 * frame. Clear SHORTS between ADDRESS and PHYEND and the frame still completes,
 * CRCOK still fires, and its handler (irq_handler_crcok -> rxframe_finish ->
 * wait_until_radio_is_disabled, nrf_802154_trx.c:1715) then spins
 * MAX_RAMPDOWN_CYCLES waiting for a DISABLED that cannot come and trips
 * NRF_802154_ASSERT(radio_is_disabled) at nrf_802154_trx.c:380, reaching
 * nrf_802154_assert_handler() -> k_panic(). Observed 4-45 s after the ANT
 * self-channels open, which is simply how long it takes for one of those ~80
 * writes a second to land inside a Thread frame.
 *
 * So every shared-RADIO write is gated on this. It is a compile-time `true` on
 * the direct backend, where the RADIO is ours forever and nothing changes.
 *
 * NOT the same question as g.hw_held in radiant_radio_nrf_gate_mpsl.c. That
 * one is "has on_grant_end() still to run", and it is deliberately TRUE for a
 * bootstrap timeslot - which never calls radiant_nrf_gate_on_grant() and so
 * never takes the peripheral at all. This flag is set where the peripheral is
 * actually taken, so the bootstrap path stops writing registers it never
 * borrowed.
 */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
static volatile bool radio_on_loan;
#define RADIO_IS_OURS() (radio_on_loan)
#else
#define RADIO_IS_OURS() true
#endif

/* Defined beside the other gate callbacks, where the RADIO-ownership
 * reasoning lives; declared here because the allocation they capture
 * happens at init, above them.
 *
 * Both definitions live inside CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL, while
 * radiant_nrf_gate_on_grant()/_on_grant_end() - which call attach() - are
 * compiled unconditionally. Declaring attach() unconditionally therefore left
 * an undefined reference to a static function in every non-gate build ("used
 * but never defined"), which linked only because --gc-sections happened to
 * discard the unreferenced grant hooks. Without the gate there is no
 * per-grant swap, so the non-gate build gets an explicit no-op instead. */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
static void radio_endpoints_save(void);
__maybe_unused static void radio_endpoints_attach(bool on);
#else
static inline void radio_endpoints_attach(bool on)
{
	(void)on;
}
#endif

/* Give every borrowed register back to its owner, once, at init - not the
 * per-grant swap (see radiant_nrf_gate_on_grant()), just the other half of
 * the allocation-time borrow. Unconditional so the non-gate build has it too. */
static void radio_endpoints_attach_off_at_init(void);

/*
 * ---------------------------------------------------------------------------
 * The six RADIO endpoint registers this backend programs, and who owns each
 * ---------------------------------------------------------------------------
 *
 * A (D)PPI connection has two halves: a DPPIC channel, and a PUBLISH_/
 * SUBSCRIBE_ register in the peripheral. These are the peripheral half, file
 * scope because the grant hooks need them (radio_endpoints_attach()).
 *
 * `radio_ep_contended` is the safety argument for the swap: a bit is set
 * only for a register that ALREADY held another stack's value at init, and
 * only those are ever touched at a grant boundary. In every build without a
 * second stack the mask is zero and the swap is a no-op. Measured
 * 2026-08-12 beside OpenThread, the mask is PUBLISH_ADDRESS (802.15.4
 * driver channel 5) and SUBSCRIBE_RXEN (its channel 23).
 */
#define RADIO_EP_COUNT 6U

/*
 * The per-grant swap, as a switch.
 *
 * 1 = take the contended endpoints on grant entry, give them back on exit.
 * 0 = allocate normally but never move them at a grant boundary, so ANT+
 *     cannot drive a contended endpoint.
 *
 * The allocation-time borrow always happens regardless of this switch (it's
 * what lets init succeed beside another stack).
 *
 * The swap being ON is why a grant that ends without reaching on_grant_end()
 * is dangerous: the register it moves is the 802.15.4 driver's write-once
 * SUBSCRIBE_RXEN, and one missed restore stops that driver's receiver ramping
 * for the rest of the boot - which is why g.hw_held, the exactly-one hand-back
 * invariant, and the gate ztest suite all exist. With radio_ep_contended empty
 * the two settings are identical, so they only differ beside another stack.
 */
#define RADIANT_NRF_GRANT_EP_SWAP 1

__maybe_unused static uint32_t radio_ep_foreign[RADIO_EP_COUNT];
static uint8_t  radio_ep_contended;

/*
 * Which of the six this backend actually programmed - the set the per-grant
 * swap moves. Derived from OUR allocation rather than from what another stack
 * happened to hold at init; see radio_endpoints_attach() for why the
 * difference between those two is a boot-order race.
 */
__maybe_unused static uint8_t  radio_ep_mine_mask;

/* Whether RADIO_0_IRQn was already NVIC-enabled when this grant started, so it
 * can be put back that way. See radiant_nrf_gate_on_grant(). */
__maybe_unused static bool     radio_irq_was_enabled;

/*
 * OUR half of the swap: what the six registers hold when the connections this
 * backend allocated are the ones attached. Defined here rather than beside
 * radio_endpoints_attach() so the grant hooks above it can read it for the
 * bench probe; the reasoning about who owns what lives with that function.
 */
__maybe_unused static struct {
	uint32_t pub_address;
	uint32_t pub_rxready;
	uint32_t sub_rssistart;
	uint32_t sub_rxen;
	uint32_t sub_txen;
	uint32_t sub_disable;
	bool     saved;
} radio_ep;

#if RADIANT_NRF_RADIO_EP
static volatile uint32_t *radio_ep_reg(size_t i)
{
	switch (i) {
	case 0U: return &NRF_RADIO->PUBLISH_ADDRESS;
	case 1U: return &NRF_RADIO->PUBLISH_RXREADY;
	case 2U: return &NRF_RADIO->SUBSCRIBE_RSSISTART;
	case 3U: return &NRF_RADIO->SUBSCRIBE_RXEN;
	case 4U: return &NRF_RADIO->SUBSCRIBE_TXEN;
	default: return &NRF_RADIO->SUBSCRIBE_DISABLE;
	}
}
#endif /* RADIANT_NRF_RADIO_EP */

/* Named rather than counted: WHICH registers are shared decides the shape of
 * the fix, since a taken publication and a taken task subscription fail
 * differently. */
static void radio_endpoints_attach_off_at_init(void)
{
#if RADIANT_NRF_RADIO_EP
	for (size_t r = 0U; r < RADIO_EP_COUNT; r++) {
		if ((radio_ep_contended & (1U << r)) != 0U) {
			*radio_ep_reg(r) = radio_ep_foreign[r];
		}
	}
#endif
	/* On a PPI part radio_ep_contended is unconditionally zero, so the loop
	 * above would be a no-op even if it compiled - and the only caller tests
	 * `shared != 0U` first, so this is never reached there. */
}

__maybe_unused static const char *radio_ep_name(size_t i)
{
	static const char *const names[RADIO_EP_COUNT] = {
		"PUBLISH_ADDRESS", "PUBLISH_RXREADY", "SUBSCRIBE_RSSISTART",
		"SUBSCRIBE_RXEN",  "SUBSCRIBE_TXEN",  "SUBSCRIBE_DISABLE",
	};

	return names[i];
}

static void radio_isr(const void *arg);
/* Both are defined with the interrupt handler, because that is where the
 * reasoning about EVENTS_DISABLED lives; radiant_radio_abort() needs them
 * earlier. See radio_disable_now() for why an abort may not simply leave the
 * event in flight. */
static void radio_disable_now(void);
static void deliver_terminal(enum radiant_radio_status st);

/* ---------------------------------------------------------------------------
 * THE CORE CALLBACK, AND WHY IT DOES NOT RUN WHERE IT IS RAISED
 *
 * See docs/p4-coexistence-bugs.md. Under the MPSL gate the RADIO interrupt is
 * delivered as an MPSL timeslot signal from an IRQ_ZERO_LATENCY vector, which
 * runs above every irq_lock() and every kernel spinlock. The kernel CALLS came
 * out of that context first; this takes the CORE out of it too.
 *
 * Why a lock-free event ring alone would not have been enough:
 * radiant_op.cbs->rx() does not merely queue a packet. It runs
 * api_sched_done() and with it radiant_channel_on_slot(), the scheduler
 * re-arm, api_ch[], api_xfer[], api_stats - all of which the thread-context
 * ANT API touches under k_mutex_lock(&api_lock), and A MUTEX CANNOT EXCLUDE A
 * ZERO-LATENCY INTERRUPT any more than irq_lock() can. An event ring alone
 * would fix the visible symptom and leave the scheduler and channel state
 * exactly as exposed.
 *
 * Measured, with the timeslot session simply never opened so that none of this
 * ran in the zero-latency context: 9 min 52 s with zero lockups, against 13
 * lockups in 620 s with it open. That bisection is what says the cost is worth
 * paying.
 *
 * WHAT STAYS BEHIND. Everything that touches the peripheral or the grant: the
 * (D)PPI disables, RADIO->SHORTS, the interrupt mask, radio_disable_now(), and
 * gate_release() - which must happen promptly or a tracked window holds its
 * follow-on reservation for a reply that is not coming. Only the callback into
 * the core is deferred, and the event is fully built before it is queued, so
 * every register read (rssi_sample_dbm(), RXCRC, the captured t_sync) still
 * happens inside the window it describes.
 *
 * SINGLE PRODUCER, SINGLE CONSUMER, and that is a property of gate_in_signal()
 * rather than a hope: the only producer is the MPSL signal callback, which
 * cannot preempt itself; the only consumer is the trampoline's handler, which
 * runs strictly below it. Every other context - the gate's work queue
 * delivering a denial, the trampoline dispatching - has gate_in_signal() false
 * and calls the core directly, which is legal there and is what makes
 * api_lock mean something again.
 * ---------------------------------------------------------------------------
 */

#if defined(CORE_CB_DEFERRED)

/* The longest body the backend ever hands up - the HAL's own maximum, raised
 * to 40 for the long-range format (radiant_frame.h RADIANT_FRAME_LR_BODY_MAX).
 * Copied because the pointer in struct radiant_rx_event is "valid only for the
 * duration of this callback" and the callback now happens later. It used to be
 * 32, and an LR body of 33-40 bytes was queued with no body and dropped
 * silently on MPSL builds (review 2026-10-31, A14-2). */
#define CORE_CB_BODY_MAX RADIANT_RADIO_BODY_MAX
BUILD_ASSERT(CORE_CB_BODY_MAX <= UINT8_MAX, "core_cb_entry.body_len is uint8_t");

enum core_cb_kind { CORE_CB_RX = 0, CORE_CB_TX, CORE_CB_ED };

struct core_cb_entry {
	uint8_t kind;
	union {
		struct radiant_rx_event rx;
		struct radiant_tx_event tx;
#if defined(CONFIG_RADIANT_ED_SCAN)
		struct radiant_ed_event ed;
#endif
	} ev;
	uint8_t body[CORE_CB_BODY_MAX];
	uint8_t body_len;
};
BUILD_ASSERT(sizeof(((struct core_cb_entry *)0)->body) >= RADIANT_RADIO_BODY_MAX,
	     "a body the HAL accepts would be queued without its bytes");

/*
 * Eight, not one. Two completions can land before the trampoline gets the CPU -
 * a frame and the window's own terminal are the ordinary pair, and a merged
 * window can raise several - and a dropped completion is the expensive kind of
 * bug: it presents as a channel that quietly stops rather than as a crash.
 * Power of two so the mask is free.
 */
#define CORE_CB_RING 8u
static struct core_cb_entry core_cb_q[CORE_CB_RING];
/* head is written only by the producer, tail only by the consumer; each reads
 * the other's. Both 32-bit and free-running, so a torn read is impossible on
 * this core and wrap is a non-event. */
static volatile uint32_t core_cb_head;
static volatile uint32_t core_cb_tail;

/*
 * THE GRANT HANDOFF.
 *
 * follow_on_us reserves air so the core can arm one more operation inside the
 * grant it already holds - the acknowledged-data reply, the ACK window after a
 * data packet, a listening master's turnaround, a queued slave command - each
 * 1.5-2.2 ms out, far nearer than any NEW reservation can be placed
 * (PLACE_MIN_US). But every one of those is armed from the completion of the
 * operation before it, and that completion's terminal (or the abort that
 * replaces a still-open window) used to give the grant back FIRST. The arm then
 * found nothing held, took the placement path, and was refused too-near: the
 * reserve was paid for on every window and could never be used.
 *
 * So a release is OWED, not done, while the core still has a completion to
 * run: inside the trampoline's dispatch, or from the signal context whose
 * completion was just queued for it. When the dispatch drains, the debt is
 * settled - kept if a follow-on was programmed into the held air (its own
 * terminal owes the next release), paid otherwise. The early hand-back that
 * matters (an empty window's ~2 ms reserve) slips only by the trampoline's
 * latency, and a dispatch that never comes is still bounded by the grant's own
 * ending compare.
 *
 * Only while a grant is HELD. With nothing held, gate_release()'s other branch
 * (forget a pending request, stop its backstop) must run now, before the
 * callback can place a fresh one - deferring it would clear the new request's
 * state instead. gate_held() is constant false on the direct gate, so a
 * shipping dongle releases exactly when it always did.
 */
static volatile bool grant_release_owed;
static uint32_t      core_cb_depth;
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS_LOG)
/* Bench diagnostic (ADR 0018 d2), printed by the gate's D2 log: settle calls
 * that [0] released, [1] kept the grant for a follow-on, [2] waited for a
 * queued completion, [3] found nothing owed; [4] d2_settle_now releases;
 * [5] after_core immediate releases; [6] after_core owed. */
uint32_t radiant_d2_dbg[8];
#define D2DBG(i) (radiant_d2_dbg[(i)]++)
#else
#define D2DBG(i) ((void)0)
#endif

static void grant_release_after_core(void)
{
	if (gate_held() && (core_cb_depth != 0u || gate_in_signal())) {
		D2DBG(6);
		grant_release_owed = true;
		if (gate_in_signal()) {
			/* Pended even if the completion failed to queue, so the
			 * debt is always settled by a dispatch. */
			radiant_swi_pend(RADIANT_SWI_CORE_CB);
		}
		return;
	}
	D2DBG(5);
	gate_release();
}

static void grant_release_settle(void)
{
	/* A completion still queued may yet arm the follow-on; its dispatch
	 * settles instead (the enqueue pended it). */
	if (!grant_release_owed) {
		D2DBG(3);
		return;
	}
	if (core_cb_tail != core_cb_head) {
		D2DBG(2);
		return;
	}
	grant_release_owed = false;
	if (radiant_op.kind != OP_NONE && !radiant_staged.pending) {
		/* Programmed into the held air: the follow-on owns the grant. */
		D2DBG(1);
		return;
	}
	if (gate_held()) {
		D2DBG(0);
		gate_release();
	}
}

#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
/*
 * ADR 0018 (d2): the operation just armed was staged behind a NEW request,
 * not programmed into the held air - so nothing can follow into that air any
 * more (one operation at a time), and an owed release can be paid now rather
 * than when the dispatch drains. With two sessions the new request is judged
 * by MPSL against the held timeslot's whole booked length (a tracked window's
 * follow-on reserve included) until that timeslot ends, so every microsecond
 * the release waits comes out of the next request's notice. Measured on
 * gate.conf: the scan chunk after every tracked window reached MPSL with under
 * 1 ms of its 3 ms notice left and was refused, 448 of 448.
 */
static void d2_settle_now(void)
{
	if (grant_release_owed && gate_held()) {
		D2DBG(4);
		grant_release_owed = false;
		gate_release();
	}
}
#endif

static void core_cb_dispatch(void)
{
	core_cb_depth++;
	while (core_cb_tail != core_cb_head) {
		struct core_cb_entry *e = &core_cb_q[core_cb_tail % CORE_CB_RING];

		switch ((enum core_cb_kind)e->kind) {
		case CORE_CB_RX:
			/* Repoint at our copy: the backend buffer the original
			 * pointed into has very likely been reused by now. */
			e->ev.rx.body = (e->body_len != 0u) ? e->body : NULL;
			radiant_op.cbs->rx(&e->ev.rx, radiant_op.user);
			break;
		case CORE_CB_TX:
			radiant_op.cbs->tx(&e->ev.tx, radiant_op.user);
			break;
#if defined(CONFIG_RADIANT_ED_SCAN)
		case CORE_CB_ED:
			radiant_op.cbs->ed(&e->ev.ed, radiant_op.user);
			break;
#endif
		default:
			break;
		}
		/* Advanced AFTER the callback, so the slot cannot be reused
		 * under a callback that is still reading it. */
		core_cb_tail++;
	}
	core_cb_depth--;
	grant_release_settle();
}

static void core_cb_swi(uint32_t bits)
{
	ARG_UNUSED(bits);
	core_cb_dispatch();
}

/*
 * The last slot is kept for terminal events. A full ring of frames from one
 * merged or scan window would otherwise refuse that window's terminal, and the
 * scheduler - still believing the operation armed - would never be called back
 * to arm another. One slot is enough: the next operation can only be armed once
 * the core has seen this one's terminal, so two are never queued at once. A
 * refused frame is ordinary loss.
 */
static bool core_cb_enqueue(const struct core_cb_entry *src, bool terminal)
{
	uint32_t head = core_cb_head;

	uint32_t depth = head - core_cb_tail;

	if (depth >= (terminal ? CORE_CB_RING : CORE_CB_RING - 1u)) {
		return false;
	}
	core_cb_q[head % CORE_CB_RING] = *src;
	/* The slot is complete before the consumer is told it exists. A
	 * compiler barrier is enough: producer and consumer are the same CPU,
	 * so there is no store buffer between them to order. */
	__asm__ volatile("" ::: "memory");
	core_cb_head = head + 1u;
	radiant_swi_pend(RADIANT_SWI_CORE_CB);
	return true;
}
#else
/* No zero-latency context and no trampoline: every completion runs inline,
 * so nothing can be owed. */
static inline void grant_release_after_core(void)
{
	gate_release();
}
#endif /* CORE_CB_DEFERRED */

/*
 * The three seams. Inline when we are not in a zero-latency context - which is
 * every context on the direct backend, and the gate's work queue and the
 * trampoline itself on the arbitrated one - and queued when we are.
 */
static void core_cb_rx(struct radiant_rx_event *evt)
{
#if defined(CORE_CB_DEFERRED)
	if (gate_in_signal()) {
		struct core_cb_entry e;

		memset(&e, 0, sizeof(e));
		e.kind = (uint8_t)CORE_CB_RX;
		e.ev.rx = *evt;
		if (evt->body != NULL && evt->body_len != 0u &&
		    evt->body_len <= CORE_CB_BODY_MAX) {
			memcpy(e.body, evt->body, evt->body_len);
			e.body_len = evt->body_len;
		} else {
			/* No copy, so say "no body" consistently rather than a
			 * NULL body with a non-zero length. */
			e.ev.rx.body_len = 0u;
		}
		/* A frame is terminal only when stop_on_first has already
		 * retired the operation. */
		(void)core_cb_enqueue(&e,
				      (evt->status != RADIANT_RADIO_STATUS_OK &&
				       evt->status != RADIANT_RADIO_STATUS_CRC_FAIL) ||
				      radiant_op.kind != OP_RX);
		return;
	}
#endif
	radiant_op.cbs->rx(evt, radiant_op.user);
}

static void core_cb_tx(struct radiant_tx_event *evt)
{
#if defined(CORE_CB_DEFERRED)
	if (gate_in_signal()) {
		struct core_cb_entry e;

		memset(&e, 0, sizeof(e));
		e.kind = (uint8_t)CORE_CB_TX;
		e.ev.tx = *evt;
		(void)core_cb_enqueue(&e, true);
		return;
	}
#endif
	radiant_op.cbs->tx(evt, radiant_op.user);
}

#if defined(CONFIG_RADIANT_ED_SCAN)
static void core_cb_ed(struct radiant_ed_event *evt)
{
#if defined(CORE_CB_DEFERRED)
	if (gate_in_signal()) {
		struct core_cb_entry e;

		memset(&e, 0, sizeof(e));
		e.kind = (uint8_t)CORE_CB_ED;
		e.ev.ed = *evt;
		/* Per-index results are OK; the sweep's terminal never is. */
		(void)core_cb_enqueue(&e,
				      evt->status != RADIANT_RADIO_STATUS_OK);
		return;
	}
#endif
	radiant_op.cbs->ed(evt, radiant_op.user);
}
#endif


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

	/*
	 * The gate first, before a single peripheral is claimed. On the direct
	 * gate this returns OK having done nothing; on an arbitrated one it opens
	 * the session every later acquire runs through, and a backend that had
	 * claimed the RADIO and the TIMER before finding out the session could
	 * not be opened would hold two peripherals it is not entitled to.
	 */
	err = gate_init();
	if (err != RADIANT_RADIO_OK_RC) {
		LOG_ERR("radio gate did not initialise (%d)", err);
		return err;
	}

#if defined(CORE_CB_DEFERRED)
	/*
	 * After gate_init() - which claims the trampoline line - and before the
	 * RADIO can raise anything. Fatal, not cosmetic: with no consumer
	 * registered, every completion raised from a timeslot signal queues and
	 * is never dispatched, and the dongle opens channels and hears nothing
	 * with no counter moving.
	 */
	err = radiant_swi_register(RADIANT_SWI_CORE_CB, core_cb_swi);
	if (err != 0) {
		LOG_ERR("core completion trampoline did not register (%d)", err);
		return RADIANT_RADIO_EIO;
	}
#endif

	clk = DEVICE_DT_GET_ONE(nordic_nrf_clock);
	if (!device_is_ready(clk)) {
		LOG_ERR("clock controller not ready");
		gate_shutdown();
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
	 * Fast ramp-up. See RAMP_UP_US - without this, every timing constant is
	 * 100 us wrong on nRF52840 (but exactly right on nRF54L15, which is why
	 * a bench that only ran on the latter wouldn't catch it).
	 *
	 * Guarded on the nrfx symbol, not CONFIG_SOC_SERIES_*: nrfx only declares
	 * this helper under `RADIO_MODECNF0_RU_Msk || RADIO_TIMING_RU_Msk`, and
	 * no NCS v3.2.4/v3.4.0 header defines RADIO_TIMING_RU_Msk for any part,
	 * so testing the symbol tracks compile-ability where a series test would
	 * not. Safe: Zephyr's BLE controller writes RADIO_MODECNF0_RU_Fast on
	 * nRF52 with no errata guard (radio.c, radio_phy_set()) and skips
	 * MODECNF0 on nRF54LX; no nRF52840 erratum touches MODECNF0.RU.
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
	 * 1 MHz, derived rather than named: nrf_timer_frequency_set() doesn't
	 * exist on every part (nRF54L TIMERs are prescaler-only, off a non-16 MHz
	 * PCLK), so the prescaler is computed from this instance's own base
	 * frequency rather than a frequency enum that compiles on nRF52 and fails
	 * to link on nRF54L.
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
	/* Nothing borrowed yet; see radio_ep_reg() and the borrow loop below. */
	radio_ep_contended = 0U;

	/*
	 * One allocation per statement, with the name in the error. Four of
	 * these five cross a DPPI domain boundary (TIMER is in DPPIC20, RADIO
	 * in DPPIC10 - a channel at each end plus a PPIB channel), while
	 * conn_rssi is RADIO-to-RADIO, one channel in one domain - a
	 * cross-domain failure and a full-domain failure need different fixes.
	 */
	struct {
		const char     *name;
		bool            cross_domain;
		uint32_t        evt;
		uint32_t        task;
		nrfx_gppi_handle_t *out;
	} conns[] = {
		{ "sync (RADIO ADDRESS -> TIMER CAPTURE)", true,
		  nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS),
		  nrf_timer_task_address_get(TIMER_ADDR,
					     nrf_timer_capture_task_get(CC_SYNC)),
		  &radiant_op.conn_sync },
		{ "start (TIMER CC_START -> RADIO RXEN)", true,
		  nrf_timer_event_address_get(TIMER_ADDR,
					      nrf_timer_compare_event_get(CC_START)),
		  nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_RXEN),
		  &radiant_op.conn_start },
		{ "start_tx (TIMER CC_TXSTART -> RADIO TXEN)", true,
		  nrf_timer_event_address_get(TIMER_ADDR,
					      nrf_timer_compare_event_get(CC_TXSTART)),
		  nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_TXEN),
		  &radiant_op.conn_start_tx },
		{ "close (TIMER CC_CLOSE -> RADIO DISABLE)", true,
		  nrf_timer_event_address_get(TIMER_ADDR,
					      nrf_timer_compare_event_get(CC_CLOSE)),
		  nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_DISABLE),
		  &radiant_op.conn_close },
		{ "rssi (RADIO RXREADY -> RADIO RSSISTART)", false,
		  nrf_radio_event_address_get(NRF_RADIO, NRF_RADIO_EVENT_RXREADY),
		  nrf_radio_task_address_get(NRF_RADIO, NRF_RADIO_TASK_RSSISTART),
		  &radiant_op.conn_rssi },
	};

	/*
	 * Make room on the RADIO's own endpoints before asking for a connection.
	 *
	 * nrfx_gppi_conn_alloc() refuses with -EINVAL when an endpoint is
	 * already attached (an event may PUBLISH to exactly one channel).
	 * Beside OpenThread this is real: the nRF 802.15.4 driver claims
	 * RADIO's ADDRESS/READY/END/PHYEND/DISABLED/CCAIDLE/CCABUSY
	 * publications at its own init on fixed channels and gets there first.
	 * Measured 2026-08-12 on nRF54L15: connection 1 of 5 fails, nothing
	 * allocated, ANT+ refuses to start beside a healthy Thread stack.
	 *
	 * So: snapshot what's there, clear only the endpoints this backend
	 * needs, allocate, then restore exactly the values that were non-zero.
	 * A register that was zero belonged to nobody and keeps our value; one
	 * that was non-zero belongs to the other stack and goes back, since
	 * between grants the RADIO isn't ours and leaving our publication on it
	 * means the other stack publishes its own events onto our channel.
	 *
	 * Where endpoints ARE shared, allocation succeeds but the connection is
	 * inert until radio_endpoints_attach() re-attaches our values inside a
	 * grant - a known gap, announced below rather than discovered as silent
	 * "ANT+ hears nothing".
	 *
	 * On a PPI part (nRF52) none of this applies and none of it compiles;
	 * `shared` is zero by construction - see RADIANT_NRF_RADIO_EP.
	 */
#if RADIANT_NRF_RADIO_EP
	for (size_t r = 0U; r < RADIO_EP_COUNT; r++) {
		radio_ep_foreign[r] = *radio_ep_reg(r);
		if (radio_ep_foreign[r] != 0U) {
			radio_ep_contended |= (uint8_t)(1U << r);
			LOG_WRN("RADIO %s already taken by another stack "
				"(register value 0x%08x) - borrowing it for "
				"allocation and swapping it per grant",
				radio_ep_name(r),
				(unsigned int)radio_ep_foreign[r]);
			*radio_ep_reg(r) = 0U;
		}
	}
#endif
	unsigned int shared = (unsigned int)POPCOUNT(radio_ep_contended);

	for (size_t i = 0U; i < ARRAY_SIZE(conns); i++) {
		/* Signed errno, not nrfx_err_t - this is the Zephyr-facing
		 * wrapper, which is why the chain this replaced tested `< 0`. */
		int err = nrfx_gppi_conn_alloc(conns[i].evt, conns[i].task,
					       conns[i].out);

		if (err < 0) {
			LOG_ERR("(D)PPI connection %u/%u '%s' failed: %d (%s). "
				"%u of %u already allocated.",
				(unsigned int)(i + 1U),
				(unsigned int)ARRAY_SIZE(conns), conns[i].name,
				err,
				conns[i].cross_domain ? "cross-domain, needs a "
							"channel in each DPPIC "
							"plus a PPIB channel"
						      : "same-domain, one channel",
				(unsigned int)i, (unsigned int)ARRAY_SIZE(conns));
			if (err == -EINVAL) {
				LOG_ERR("-EINVAL is an ENDPOINT CONFLICT, not a "
					"shortage: another stack already "
					"PUBLISHes this event. The 802.15.4 "
					"driver takes RADIO ADDRESS/READY/END/"
					"PHYEND/DISABLED/CCAIDLE/CCABUSY at its "
					"init. Counting free channels will not "
					"explain this.");
			} else {
				LOG_ERR("out of (D)PPI resource - on nRF54L the "
					"802.15.4 driver reserves a fixed block "
					"of DPPIC10 and MPSL one more");
			}
			return RADIANT_RADIO_EIO;
		}
	}
	radiant_op.conn_ok = true;

	/* The t_sync capture is hardware to hardware with no software in the
	 * path, which is the whole reason this backend owns a TIMER at all, and
	 * it never needs turning off. */
	nrfx_gppi_conn_enable(radiant_op.conn_sync);

#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
	/*
	 * Take a copy of what the five allocations just wrote into the RADIO's
	 * own PUBLISH and SUBSCRIBE registers. Under arbitration the RADIO is
	 * not ours between grants, and those registers are the part of a (D)PPI
	 * connection that lives in the RADIO rather than in the DPPIC - see
	 * radio_endpoints_attach().
	 *
	 * THIS CALL BELONGS HERE AND NOWHERE ELSE: this is the one instant at
	 * which all six registers hold OUR value - after the allocations wrote
	 * them and BEFORE radio_endpoints_attach_off_at_init() hands the
	 * borrowed ones back to the other stack. Saving after the hand-back
	 * instead leaves `mine[]` in radio_endpoints_attach() holding the OTHER
	 * STACK'S routing for exactly the registers the swap exists to move, so
	 * attach(true) re-applies the 802.15.4 driver's SUBSCRIBE_RXEN, our
	 * CC_START compare never triggers TASKS_RXEN, the receiver never ramps,
	 * and - because TASKS_DISABLE on an already-DISABLED radio raises no
	 * EVENTS_DISABLED - the window produces no terminal either. Measured on
	 * P4 MED: 13001 grants delivered, 0 windows ended OK, 0 of 961 packets,
	 * invisible to the gate's own counters (dead=0, late=0, grant_end_calls
	 * == granted). Only ever appears beside a second stack, where the
	 * contended mask is non-empty.
	 */
	radio_endpoints_save();
#endif

	/* Hand back exactly what was borrowed - see the comment above the loop.
	 * Registers that were zero keep OUR value. */
	if (shared != 0U) {
		radio_endpoints_attach_off_at_init();
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
		LOG_INF("%u RADIO endpoints are shared with another stack and "
			"are swapped in and out with each grant - see "
			"radiant_nrf_gate_on_grant()", shared);
#else
		LOG_ERR("ANT+ WILL NOT TRANSMIT: %u RADIO endpoints are shared "
			"with another stack and this build has no arbiter at "
			"all. Whichever stack wrote the register last wins it "
			"and the loser transmits nothing while reporting "
			"success.", shared);
#endif
	}

	/*
	 * Both RADIO interrupt lines on nRF54L, not one: that part splits
	 * RADIO's interrupt over RADIO_0 and RADIO_1, and connecting the wrong
	 * one means every arm succeeds, nothing faults, and the terminal event
	 * never arrives - indistinguishable from "the radio hears nothing".
	 * The handler is idempotent across lines (tests/clears each event
	 * flag), so being entered twice costs a few cycles, nothing else.
	 */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
	/*
	 * Neither line is connected under the gate - it's the priority, not the
	 * handler, that requires this, since IRQ_CONNECT() also expands to a
	 * z_arm_irq_priority_set() call at init on ARM. RADIO_0 is MPSL's:
	 * mpsl_init.c registers it IRQ_DIRECT_CONNECT under IRQ_ZERO_LATENCY
	 * (unmaskable by BASEPRI). Connecting it here demotes it to an ordinary
	 * maskable interrupt, so any irq_lock() in the system can hold off the
	 * SoftDevice Controller's radio interrupt - it asserts ~10 ms into
	 * bt_enable() with this backend's radio completely idle.
	 *
	 * Nothing is lost: inside a grant MPSL owns the vector and forwards
	 * RADIO events via radiant_nrf_gate_on_radio_irq(); outside a grant the
	 * radio isn't ours to take interrupts from anyway. The NVIC line still
	 * needs irq_enable() (NVIC_EnableIRQ(), no priority) in
	 * radiant_radio_enable() per mpsl.rst. Line 1 goes with it for free:
	 * with no connection, stray RADIO_1 interrupts reach the spurious
	 * handler instead of consuming events radio_isr() was waiting for.
	 */
#else
	IRQ_CONNECT(RADIANT_RADIO_IRQn, CONFIG_RADIANT_BACKEND_NRF_IRQ_PRIO,
		    radio_isr, NULL, 0);
#if defined(RADIANT_RADIO_IRQn_2)
	IRQ_CONNECT(RADIANT_RADIO_IRQn_2, CONFIG_RADIANT_BACKEND_NRF_IRQ_PRIO,
		    radio_isr, NULL, 0);
#endif
#endif /* CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL */

	radiant_op.cbs = cbs;
	radiant_op.user = user;
	radiant_op.next_id = 1u;
	radiant_op.kind = OP_NONE;
	radiant_op.inited = true;

	/*
	 * Prove the clock is running before anything depends on it. A stopped
	 * timer is the quietest possible failure: every arm succeeds, no compare
	 * ever fires, nothing is logged, and it is indistinguishable from "the
	 * radio hears nothing".
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
	 * One die-temperature reading at the one point in this file's lifecycle
	 * guaranteed to be thread context and to run every boot: logs the
	 * confound (radiant_radio_nrf_diag.h) and, on nRF52840/nRF52833, seeds
	 * the errata-153/225 RSSI correction cache from the first packet rather
	 * than the first RSSI_TEMP_UPDATE_PERIOD_MS refresh.
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

#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
	/*
	 * RADIO->INTENSET is a register in a peripheral we don't own between
	 * grants, and leaving it set crashes the other stack: the same bits set
	 * while the SoftDevice Controller advertises on the same peripheral
	 * raise END/DISABLED interrupts nobody scheduled, and it asserts
	 * ("SoftDevice Controller ASSERT: 48, 1792") ~5 ms after the first
	 * advertising event, every boot. So under the gate the mask is
	 * established inside the grant and cleared when it ends - see
	 * radiant_nrf_gate_on_grant()/_on_grant_end(). On a direct build it is
	 * set once and left, since the RADIO is ours forever.
	 *
	 * Line 0 stays enabled, line 1 doesn't - deliberately asymmetric.
	 * Line 0 is MPSL's (mpsl_init.c: MPSL_RADIO_IRQn = RADIO_0_IRQn) and
	 * also the only line this backend raises interrupts on
	 * (nrf_radio_int_enable() writes INTENSET00); disabling it was measured
	 * to run every window to full length without completing (chunks=256,
	 * end ok=0, deny=256). Line 1 is never written by this file, so a
	 * RADIO_1 interrupt can only be the SoftDevice Controller's, and under
	 * the gate this file doesn't connect that vector at all.
	 *
	 * irq_enable() is NVIC_EnableIRQ() and sets no priority, which is why
	 * this call may stay while the IRQ_CONNECT() in init had to go: this
	 * only enables a line MPSL already registered, per mpsl.rst.
	 */
	irq_enable(RADIANT_RADIO_IRQn);
#else
	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_END_MASK |
					NRF_RADIO_INT_DISABLED_MASK);
	irq_enable(RADIANT_RADIO_IRQn);
#if defined(RADIANT_RADIO_IRQn_2)
	irq_enable(RADIANT_RADIO_IRQn_2);
#endif
#endif /* CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL */
#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
	{
		static bool rx_diag_started;

		if (!rx_diag_started) {
			rx_diag_started = true;
			k_work_init_delayable(&rx_diag_work, rx_diag_work_fn);
			k_work_reschedule(&rx_diag_work, K_SECONDS(10));
			LOG_INF("rxdiag on: tracked-window outcomes every 10 s");
		}
	}
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

	/*
	 * Abort first: this and SIGNAL_SESSION_CLOSED are the two ways an
	 * arbitrated build can be torn down with an operation armed and no grant
	 * held. radiant_radio_abort() clears the staged operation and releases
	 * the reservation, so what follows is a teardown with nothing
	 * outstanding - and gate_release() (reached via abort(), on every gate)
	 * hands the peripheral back, so the RADIO is with MPSL before the mask
	 * below is touched. The session stays open across a disable (it belongs
	 * to radiant_radio_init()), so MPSL still owns the register written next.
	 */
	(void)radiant_radio_abort();
	irq_disable(RADIANT_RADIO_IRQn);
#if defined(RADIANT_RADIO_IRQn_2)
	irq_disable(RADIANT_RADIO_IRQn_2);
#endif
	/*
	 * NOT ~0 under the gate - this register is shared. nrf_radio_int_disable()
	 * writes INTENCLR00, and line 0 is MPSL's (mpsl_init.c: MPSL_RADIO_IRQn
	 * = RADIO_0_IRQn), so ~0 would clear the SoftDevice Controller's own
	 * interrupt config while leaving it running, and it later misses its
	 * events and asserts. On a direct build both lines are ours, so ~0
	 * stays as the stronger teardown.
	 */
	if (IS_ENABLED(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)) {
		nrf_radio_int_disable(NRF_RADIO, NRF_RADIO_INT_END_MASK |
						 NRF_RADIO_INT_DISABLED_MASK |
						 NRF_RADIO_INT_RXREADY_MASK);
	} else {
		nrf_radio_int_disable(NRF_RADIO, ~0u);
	}
	radiant_op.enabled = false;
	return RADIANT_RADIO_OK_RC;
}

/* ---------------------------------------------------------------------------
 * Transmit
 * ---------------------------------------------------------------------------
 *
 * struct radiant_tx_req carries its own address bytes, symmetrically with RX,
 * and never inherits one: TXADDRESS selects a logical address whose
 * BASE/PREFIX must already be programmed, so a transmit following a receive
 * on a different channel would otherwise put the PREVIOUS channel's device
 * number on the air - a frame another device may wrongly accept, not just a
 * dropped one. Both the mock and radiant_sched_request_tx() refuse a transmit
 * whose addr_len disagrees with its format.
 */
static int program_tx(const struct radiant_tx_req *req);

int radiant_radio_tx(const struct radiant_tx_req *req, uint32_t *op)
{
	unsigned int key;
	enum gate_rc g;
	int          rc;

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
	CLAIM_TERMINAL();
	irq_unlock(key);

	rc = apply_format(req->fmt, req->rf_index, 0u, true);
	if (rc != RADIANT_RADIO_OK_RC) {
		goto fail;
	}
	/*
	 * Static-length: the format's own length is the only one allowed
	 * (STATLEN decides what leaves the antenna). Length-from-body: STATLEN
	 * is 0, so the RADIO transmits body[0] bytes after the length field
	 * with nothing checking that against body_len - a body of 12 with a
	 * length byte of 30 would transmit 18 bytes of whatever follows the
	 * buffer, CRC included. Refused here rather than trusted to a caller.
	 */
	if (req->fmt->len_mode == RADIANT_LEN_FROM_BODY) {
		if (req->body_len > req->fmt->max_body_len ||
		    req->body_len < 1u + (uint8_t)req->fmt->len_offset) {
			rc = RADIANT_RADIO_EINVAL;
			goto fail;
		}
		if ((uint32_t)req->body[req->fmt->len_offset] +
			    (uint32_t)req->fmt->len_bias !=
		    (uint32_t)req->body_len) {
			rc = RADIANT_RADIO_EINVAL;
			goto fail;
		}
	} else if (req->body_len != req->fmt->body_len) {
		rc = RADIANT_RADIO_EINVAL;
		goto fail;
	}

	/*
	 * The transmit's own air, from the compare that fires TASKS_TXEN to the
	 * last bit of the body and CRC. follow_on_us extends the reservation
	 * past that: on a listening master's own slot the turnaround window is
	 * armed from inside this transmit's completion, and an arbiter that had
	 * not been told would give the radio back in between.
	 */
	{
		uint32_t lead_us = air_lead_us(req->fmt->phy, req->addr_len) +
				   (uint32_t)T_SYNC_CAL_TX_US;

		g = gate_acquire(GATE_OP_TX,
				 req->t_sync_at - (radiant_time_t)lead_us,
				 req->t_sync_at +
					 (radiant_time_t)(((uint32_t)req->body_len +
							   2u) *
							  US_PER_BYTE),
				 req->follow_on_us, RADIANT_GATE_PRIO_HIGH);
	}
	if (g == GATE_DENIED) {
		/*
		 * Synchronous refusal matters here: radiant_transfer.c arms the
		 * acknowledged-data reply from inside the receive callback with
		 * 1.56 ms to go - no time for a request/response round trip or
		 * a retry.
		 */
		rc = RADIANT_RADIO_EDENIED;
		goto fail;
	}

	radiant_op.id = radiant_op.next_id++;
	if (radiant_op.next_id == 0u) {
		radiant_op.next_id = 1u;   /* ids are non-zero by contract */
	}
	*op = radiant_op.id;

	if (g == GATE_PENDING) {
		/* Copied only on the deferred path - see radiant_radio_rx(). The
		 * body pointer keeps its own promise: the HAL requires it to
		 * stay valid and unmodified until the completion callback. */
		radiant_staged.tx = *req;
		radiant_staged.pending = true;
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
		d2_settle_now();
#endif
		return RADIANT_RADIO_OK_RC;
	}

	rc = program_tx(req);
	if (rc != RADIANT_RADIO_OK_RC) {
		*op = 0u;
		gate_release();
		goto fail;
	}
	return RADIANT_RADIO_OK_RC;

fail:
	LOG_DBG("tx refused: %d (addr_len=%u)", rc, (unsigned)req->addr_len);
	radiant_op.kind = OP_NONE;
	return rc;
}

/*
 * Everything a transmit does to the peripheral, from the staged request. The
 * arm's body verbatim; see program_rx() for why it is a function.
 */
static int program_tx(const struct radiant_tx_req *req)
{
	radiant_time_t now;
	radiant_time_t t_start;
	uint32_t lead;
	int rc;

	rc = apply_format(req->fmt, req->rf_index, 0u, false);
	if (rc != RADIANT_RADIO_OK_RC) {
		return rc;
	}
	apply_tx_address(req->addr, req->addr_len);
	apply_power(&req->power);

	memcpy(radiant_tx_buf, req->body, req->body_len);
	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)radiant_tx_buf;

	/*
	 * Working backwards from t_sync: the caller names when the last
	 * address bit must be at the antenna, and the backend (not the core)
	 * subtracts its own ramp-up and preamble, so a master's period is
	 * exact by construction.
	 *
	 * Calibration has to apply here too, not just to the reported t_sync
	 * in the ISR - otherwise a caller's antenna-referenced request would
	 * get an event-referenced placement, self-consistent but uniformly
	 * off the air. TASKS_TXEN fires T_SYNC_CAL_TX_US earlier than pure
	 * event-referenced arithmetic would put it, so ADDRESS lands at
	 * t_sync_at - cal and the antenna lands at t_sync_at.
	 */
	lead = air_lead_us(req->fmt->phy, req->addr_len) +
	       (uint32_t)T_SYNC_CAL_TX_US;
	t_start = req->t_sync_at - (radiant_time_t)lead;

	now = radiant_radio_now();
	if (req->t_sync_at < now + lead) {
		/* Never silently late. A late master frame lands in the next
		 * slot and is worse than no frame at all. */
		return RADIANT_RADIO_ETIME;
	}

	radiant_op.addr_len        = req->addr_len;
	radiant_op.body_len        = req->body_len;
	/* Transmit only ever reports the length it was given, so this is false
	 * whatever the format is - the field governs how the RECEIVE path reads
	 * a length it did not choose. */
	radiant_op.len_from_body   = false;
	radiant_op.phy             = req->fmt->phy;
	radiant_op.stop_on_first   = false;
	radiant_op.report_crc_fail = false;
	radiant_op.terminal_sent   = false;
	radiant_op.t_sync_req      = req->t_sync_at;

	/*
	 * READY_START begins transmission the instant ramp-up completes, with
	 * nothing software in between. Deliberately no end-of-packet short:
	 * nRF54L15 renamed it PHYEND_DISABLE and dropped END_DISABLE, nRF52840
	 * has the reverse, and either #if buys nothing since the transmitter
	 * is already finished by END - the ISR triggers the disable explicitly
	 * instead.
	 */
	nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK);

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

	nrf_timer_cc_set(TIMER_ADDR, CC_TXSTART, clock_counter_at(t_start));
	nrfx_gppi_conn_enable(radiant_op.conn_start_tx);

	/*
	 * Re-check after programming: ARM_SETUP_US is a budget, not a guarantee,
	 * and if the counter has already passed the compare when it is written
	 * the compare never fires - TASKS_TXEN never triggers, no END or
	 * DISABLED arrives, and the operation hangs holding the scheduler's one
	 * slot forever.
	 */
	now = radiant_radio_now();
	if (t_start <= now) {
		nrfx_gppi_conn_disable(radiant_op.conn_start_tx);
		nrf_radio_shorts_set(NRF_RADIO, 0);
		return RADIANT_RADIO_ETIME;
	}

	LOG_DBG("tx op=%u alen=%u blen=%u start=+%d last_err=%d",
		(unsigned)radiant_op.id, (unsigned)req->addr_len,
		(unsigned)req->body_len, (int)(int64_t)(t_start - now),
		(int)radiant_dbg_tx_err);
	return RADIANT_RADIO_OK_RC;
}

/* ---------------------------------------------------------------------------
 * Receive
 * ---------------------------------------------------------------------------
 */

/*
 * A compare written behind the free-running counter never fires (until the
 * 32-bit wrap, ~71 min): no RXEN, and TASKS_DISABLE on a DISABLED radio raises
 * no EVENTS_DISABLED, so no terminal either - the op would hold the
 * scheduler's one slot forever (review 2026-10-31, A14-1). program_rx() clears
 * the event and enables conn_start BEFORE writing the compare, so "counter at
 * or past start_cc with the event still clear" can only mean the compare was
 * written too late, and an event that did fire was wired to TASKS_RXEN when it
 * fired: that window is live and stays armed. The capture comes first: read
 * the other way round, the event could be clear, the compare fire, and the
 * capture then see the counter past start_cc - calling a live window missed.
 */
static bool start_compare_missed(uint32_t start_cc)
{
	uint32_t now_cc = timer_capture(CC_NOW);

	return (int32_t)(now_cc - start_cc) >= 0 &&
	       !nrf_timer_event_check(TIMER_ADDR,
				      nrf_timer_compare_event_get(CC_START));
}

/*
 * Everything a receive window does to the peripheral, from the staged
 * request. Separated from the arm call only because of the gate - on the
 * direct gate this is called one line below the arm and the compiler
 * inlines it back. Can only fail with ETIME (grant arrived too late); every
 * other refusal is a pure function of the request, answered before the gate
 * was consulted.
 */
static int program_rx(const struct radiant_rx_req *req)
{
	radiant_time_t now;
	uint32_t start_cc, close_cc;
	int rc;

	const uint8_t sw_addr_len = rx_sw_addr_len(req);

	rc = apply_format(req->fmt, req->rf_index, sw_addr_len, false);
	if (rc != RADIANT_RADIO_OK_RC) {
		return rc;
	}
	rc = apply_filters(req->filters, req->n_filters, req->fmt->addr_len,
			   (uint8_t)(req->fmt->addr_len - sw_addr_len), false);
	if (rc != RADIANT_RADIO_OK_RC) {
		return rc;
	}
	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)radiant_rx_buf;

	/*
	 * Advertise the worst case, refuse only the impossible.
	 * caps.min_arm_lead_us is the worst case over every address length,
	 * and radiant_api.c posts its window at `now + min_arm_lead_us` using
	 * a `now` sampled earlier in the core - testing the advertised figure
	 * against this later sample would always fail by the elapsed gap, and
	 * did: it kept every window refused after the lead was corrected. So
	 * this tests against what the hardware genuinely needs for this
	 * address length (ramp-up + preamble/address airtime), keeping the
	 * advertised figure's margin for the core's planning only.
	 */
	now = radiant_radio_now();
	if (req->t_open < now + air_lead_us(req->fmt->phy, req->fmt->addr_len)) {
		/* Never silently late. A window opened after the frame it was
		 * meant to catch is indistinguishable from a sensor that went
		 * quiet, and that is the report nobody can act on. */
		return RADIANT_RADIO_ETIME;
	}

	radiant_op.n_filters       = req->n_filters;
	radiant_op.addr_len        = req->fmt->addr_len;
	radiant_op.sw_addr_len     = sw_addr_len;
	radiant_op.filters         = req->filters;
	radiant_op.crc_init        = (uint16_t)req->fmt->crc.init;
	/*
	 * Static-length: every frame is this long, reported verbatim.
	 * Length-from-body: unknown until a frame arrives, so this holds the
	 * format's CEILING, and the ISR clamps the wire length-byte against
	 * it (format's body_len is zero here, which would otherwise report
	 * every long-range frame as zero bytes).
	 */
	radiant_op.len_from_body   = (req->fmt->len_mode == RADIANT_LEN_FROM_BODY);
	radiant_op.body_len        = radiant_op.len_from_body
					     ? req->fmt->max_body_len
					     : req->fmt->body_len;
	radiant_op.phy             = req->fmt->phy;
	radiant_op.stop_on_first   = (req->flags & RADIANT_RX_STOP_ON_FIRST) != 0u;
	radiant_op.report_crc_fail = (req->flags & RADIANT_RX_REPORT_CRC_FAIL) != 0u;
	radiant_op.terminal_sent   = false;
	radiant_op.rx_any          = false;
#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
	radiant_op.diag_track      = (req->fmt->addr_len == 5u &&
				      req->fmt->phy == RADIANT_PHY_1M_GFSK);
	if (radiant_op.diag_track) {
		rx_diag.hw_last = (uint8_t)(req->fmt->addr_len - sw_addr_len);
	}
	radiant_op.diag_ok         = false;
	radiant_op.diag_crc        = false;
	radiant_op.diag_end_noaddr = false;
	radiant_op.diag_cut        = false;
	radiant_op.diag_t_open     = req->t_open;
#endif

	/* SHORTS keeps the receiver armed across packets with no software in
	 * between, letting one window carry several frames (the merged-window
	 * case radiant_sched.c produces). RSSISTART on ADDRESS is free and is
	 * where rssi_dbm comes from. */
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
	 * Both window edges in hardware: open is a compare firing TASKS_RXEN;
	 * close fires TASKS_DISABLE, held open long enough for a frame with
	 * t_sync == t_close to finish arriving (body + CRC).
	 *
	 * The two edges don't take the calibration the same way. OPEN takes
	 * none: chain delay is latency AFTER the antenna, so the radio must
	 * already be receiving from t_open - air_lead regardless of it;
	 * subtracting it here would just open 9 us early for nothing. CLOSE
	 * takes it, negated: that frame's EVENTS_ADDRESS doesn't raise until
	 * t_close - T_SYNC_CAL_US (later, since the constant is negative), and
	 * TASKS_DISABLE must not fire before then plus the rest of the packet.
	 * The body slack below already swamps 9 us today, so this only
	 * matters if that slack is ever tightened.
	 *
	 * Tail is per PHY, by a large factor: at 1 M it's (body+CRC) at 8 us;
	 * on the coded PHY it's the coding indicator + TERM1, then (body+CRC) at
	 * 64 us, then TERM2. Sized from the format's ceiling, since no frame has
	 * arrived yet - too short truncates exactly the long frames the length
	 * extension exists to carry.
	 */
	{
		uint32_t tail;

		if (req->fmt->phy == RADIANT_PHY_LR_CODED) {
			tail = LR_CI_US + LR_TERM1_US +
			       ((uint32_t)radiant_op.body_len + 2u) * 64u + 24u;
		} else {
			tail = ((uint32_t)req->fmt->body_len + 2u) * US_PER_BYTE;
		}
		start_cc = clock_counter_at(
			req->t_open - (radiant_time_t)air_lead_us(req->fmt->phy,
								  req->fmt->addr_len));
		close_cc = clock_counter_at(
			t_sync_cal(req->t_close,
				   -t_sync_cal_rx_us(req->fmt->phy)) +
			(radiant_time_t)tail);
	}

	/*
	 * The order is what start_compare_missed() relies on. Park first: a
	 * stale CC_START left by an aborted window may be just ahead of the
	 * counter, and half a wrap away from start_cc cannot match in the few
	 * instructions before the real write. Then clear the event, which also
	 * takes away anything that stale value raised before it was parked,
	 * and enable the connection, so that the only compare able to set the
	 * event from here on is the real one and it finds TASKS_RXEN wired.
	 * The endpoints were wired at init; only the enable is per operation.
	 */
	nrf_timer_cc_set(TIMER_ADDR, CC_START, start_cc - 0x80000000u);
	nrf_timer_event_clear(TIMER_ADDR, nrf_timer_compare_event_get(CC_START));
	nrfx_gppi_conn_enable(radiant_op.conn_start);
	nrf_timer_cc_set(TIMER_ADDR, CC_START, start_cc);
	nrf_timer_cc_set(TIMER_ADDR, CC_CLOSE, close_cc);

	nrfx_gppi_conn_enable(radiant_op.conn_close);
	/* Receive only. A transmit ramps up through TXREADY and never raises
	 * RXREADY, so leaving this on would cost nothing - but it is enabled and
	 * disabled with the other two so "which connections are live" has one
	 * answer rather than an exception. */
	nrfx_gppi_conn_enable(radiant_op.conn_rssi);

	if (start_compare_missed(start_cc)) {
		/* The ETIME return unwinds like the early check above: the
		 * direct path releases the gate and clears the op, the grant
		 * path reports FAILED after the grant is back, and the
		 * scheduler reports MISSED and retries the pass. */
		nrfx_gppi_conn_disable(radiant_op.conn_start);
		nrfx_gppi_conn_disable(radiant_op.conn_close);
		nrfx_gppi_conn_disable(radiant_op.conn_rssi);
		nrf_radio_shorts_set(NRF_RADIO, 0);
		radiant_dbg_start_late++;
		return RADIANT_RADIO_ETIME;
	}

	LOG_DBG("rx op=%u n=%u alen=%u cov=%u open=+%d close=+%d isr=%u ev=%u ok=%u term=%u noaddr=%u late=%u",
		(unsigned)radiant_op.id, req->n_filters,
		(unsigned)req->fmt->addr_len, (unsigned)req->fmt->crc.cover_addr,
		(int)(int64_t)(req->t_open - now), (int)(int64_t)(req->t_close - now),
		(unsigned)radiant_dbg_isr, (unsigned)radiant_dbg_rx_ev, (unsigned)radiant_dbg_ok,
		(unsigned)radiant_dbg_term, (unsigned)radiant_dbg_rx_no_addr,
		(unsigned)radiant_dbg_start_late);
	return RADIANT_RADIO_OK_RC;
}

#if defined(CONFIG_RADIANT_SCHED_BOOKING)
/*
 * radiant_radio_rx()'s class rule (see the comment at its gate_acquire()),
 * plus RADIANT_RX_ELASTIC, which a booking build's scheduler sets on every
 * scan chunk so a short chunk in a short gap is not mistaken for a tracked
 * window. Only in booking builds: every other build keeps the expression
 * inline, byte for byte.
 */
static uint8_t rx_gate_prio(const struct radiant_rx_req *req)
{
	if ((req->flags & RADIANT_RX_ELASTIC) != 0u) {
		return RADIANT_GATE_PRIO_NORMAL;
	}
	return ((req->t_close - req->t_open) >= 10000u) ? RADIANT_GATE_PRIO_NORMAL
							: RADIANT_GATE_PRIO_HIGH;
}

/*
 * ADR 0018 (d2): book the air for an operation that will be armed later. The
 * instants are computed exactly as radiant_radio_rx()/_tx() compute them for
 * gate_acquire(), so the later arm's air is the booked air. Only the fixed
 * class is bookable: an elastic window is the scan, which gives way.
 */
int radiant_radio_book(const struct radiant_rx_req *rx,
		       const struct radiant_tx_req *tx)
{
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
	radiant_time_t from;
	radiant_time_t to;
	uint16_t       follow_on;
	uint16_t       tail = 0u;
	int            rc;

	if (!radiant_op.inited || !radiant_op.enabled) {
		return RADIANT_RADIO_ESTATE;
	}
	if ((rx == NULL) == (tx == NULL)) {
		return RADIANT_RADIO_EINVAL;
	}
	if (rx != NULL) {
		if (rx->fmt == NULL || rx->t_close < rx->t_open) {
			return RADIANT_RADIO_EINVAL;
		}
		if (rx_gate_prio(rx) != RADIANT_GATE_PRIO_HIGH) {
			return RADIANT_RADIO_ENOTSUP;
		}
		from = rx->t_open -
		       (radiant_time_t)air_lead_us(rx->fmt->phy, rx->fmt->addr_len);
		to = rx->t_close + (radiant_time_t)(((uint32_t)rx->fmt->body_len + 2u) *
						     US_PER_BYTE);
		follow_on = rx->follow_on_us;
		tail = rx->book_tail_us;
	} else {
		uint32_t lead_us;

		if (tx->fmt == NULL) {
			return RADIANT_RADIO_EINVAL;
		}
		lead_us = air_lead_us(tx->fmt->phy, tx->addr_len) +
			  (uint32_t)T_SYNC_CAL_TX_US;
		from = tx->t_sync_at - (radiant_time_t)lead_us;
		to = tx->t_sync_at + (radiant_time_t)(((uint32_t)tx->body_len + 2u) *
						      US_PER_BYTE);
		follow_on = tx->follow_on_us;
	}
	rc = gate_book_tail(from, to, follow_on, tail);
	switch (rc) {
	case GATE_BOOK_OK:
		return RADIANT_RADIO_OK_RC;
	case GATE_BOOK_EBUSY:
		return RADIANT_RADIO_EBUSY;
	case GATE_BOOK_EDENIED:
		return RADIANT_RADIO_EDENIED;
	default:
		return RADIANT_RADIO_ENOTSUP;
	}
#else
	ARG_UNUSED(rx);
	ARG_UNUSED(tx);
	return RADIANT_RADIO_ENOTSUP;
#endif
}
#endif

static int rx_arm(const struct radiant_rx_req *req, uint32_t *op);

int radiant_radio_rx(const struct radiant_rx_req *req, uint32_t *op)
{
	int rc = rx_arm(req, op);

	NRF_DIAG(if (rc == RADIANT_RADIO_OK_RC) {
			nrf_diag.rx_posted++;
		} else {
			nrf_diag.rx_refused++;
		});
	return rc;
}

static int rx_arm(const struct radiant_rx_req *req, uint32_t *op)
{
	unsigned int key;
	enum gate_rc g;
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
	CLAIM_TERMINAL();
	irq_unlock(key);

	/*
	 * Every refusal that's a pure function of the request, answered before
	 * the gate is asked or any register written: the HAL requires
	 * EINVAL/ENOTSUP synchronously, and this only holds under an arbiter
	 * if checked here rather than at a grant that may be milliseconds
	 * away or may never come.
	 */
	rc = apply_format(req->fmt, req->rf_index, rx_sw_addr_len(req),
			  true);
	if (rc != RADIANT_RADIO_OK_RC) {
		goto fail;
	}
	rc = apply_filters(req->filters, req->n_filters, req->fmt->addr_len,
			   (uint8_t)(req->fmt->addr_len -
				     rx_sw_addr_len(req)),
			   true);
	if (rc != RADIANT_RADIO_OK_RC) {
		goto fail;
	}

	/*
	 * The instants the hardware needs, not the instants the caller named:
	 * the window must be live from before t_open by the ramp-up and address
	 * airtime, and the close compare fires after t_close by the rest of the
	 * longest legal frame. A gate adds its own head and tail margins to
	 * these; it does not have to rediscover them.
	 */
	g = gate_acquire(GATE_OP_RX,
			 req->t_open -
				 (radiant_time_t)air_lead_us(req->fmt->phy,
							     req->fmt->addr_len),
			 req->t_close +
				 (radiant_time_t)(((uint32_t)req->fmt->body_len +
						   2u) *
						  US_PER_BYTE),
			 req->follow_on_us,
			 /*
			  * Which of our own requests gives way, derived from
			  * duration alone: ADR 0013 makes the sweep elastic and
			  * tracked slots inviolate, but rule 1 forbids a backend
			  * knowing anything about ANT - window LENGTH isn't ANT
			  * knowledge. A tracked window/turnaround is under a
			  * millisecond (RADIANT_CHANNEL_GUARD_MAX_US bounds it
			  * at 400 us each way); only a scan chunk or ED sweep
			  * runs tens of milliseconds. 10 ms clears both.
			  */
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
			 rx_gate_prio(req));
#else
			 ((req->t_close - req->t_open) >= 10000u)
				 ? RADIANT_GATE_PRIO_NORMAL
				 : RADIANT_GATE_PRIO_HIGH);
#endif
	if (g == GATE_DENIED) {
		rc = RADIANT_RADIO_EDENIED;
		goto fail;
	}

	radiant_op.id = radiant_op.next_id++;
	if (radiant_op.next_id == 0u) {
		radiant_op.next_id = 1u;   /* ids are non-zero by contract */
	}
	*op = radiant_op.id;

	if (g == GATE_PENDING) {
		/*
		 * The operation exists and the core may account for it, but no
		 * peripheral has been touched; radiant_nrf_gate_on_grant() or
		 * _on_denied() resolves it. Copied here and only here: the
		 * struct is a stack local in radiant_sched.c's arm_rx_window()
		 * and is gone once it returns. Not copied on the granted path,
		 * to avoid charging every shipping dongle a struct copy per arm
		 * for a deferral it can never perform.
		 */
		radiant_staged.rx = *req;
		radiant_staged.pending = true;
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
		d2_settle_now();
#endif
		return RADIANT_RADIO_OK_RC;
	}

	rc = program_rx(req);
	if (rc != RADIANT_RADIO_OK_RC) {
		*op = 0u;
		gate_release();
		goto fail;
	}
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
 * This part has no energy-detect mode this backend can use: EDCNT/EDSAMPLE
 * exists only in 802.15.4 mode, and switching to it would reconfigure the
 * peripheral out from under an ANT window's packet config. So this measures
 * with what's available in GFSK mode instead: RSSISAMPLE, one shot per
 * TASKS_RSSISTART, read by software.
 *
 * Which makes the sampling budget the whole design: RSSI is read in the radio
 * interrupt at priority 0, where the HAL forbids work proportional to
 * anything - a loop filling a 400 us dwell or a 10 ms chunk would violate
 * that. So ED_SAMPLES_MAX bounds the burst at a constant regardless of dwell
 * (32 samples at ED_SAMPLE_STEP_US apart is ~64 us of spinning, comparable to
 * radio_disable_now()'s own bound); the dwell bounds it again from the other
 * side; and it is one interrupt per index, not per sample - the burst runs
 * inside RXREADY and the radio is taken down before the callback, rather than
 * costing 200 interrupts/index for a per-sample compare.
 *
 * So min/mean describe a ~64 us burst near the START of the dwell, not the
 * whole dwell - it can see a Wi-Fi transmission in progress but not one that
 * starts 300 us later. The defence is repetition, not dwell length: the
 * scheduler revisits every index continuously and radiant_chanmap.c aggregates
 * min/max across dwells, so a busy index reveals itself over seconds.
 * radiant_ed_event.samples is reported so a consumer can weigh this.
 *
 * An index ends as soon as its burst is done - dwell_us is an upper bound,
 * not a duration, so a sweep finishes inside its scheduled gap rather than
 * over it. Nothing can be received during a sweep, enforced in hardware:
 * RXADDRESSES is cleared, so the matcher can't fire and there is no ADDRESS,
 * DMA, or END.
 */

#if defined(CONFIG_RADIANT_ED_SCAN)

/* The burst, bounded at a constant. See above for why the constant and not the
 * dwell is what bounds the interrupt. */
#define ED_SAMPLES_MAX    32u
/*
 * Spacing between samples, us on this backend's 1 MHz TIMER. Not a settling
 * wait (datasheet RSSI settling is ~0.25 us on both parts) - it's the finest
 * spacing a 1 MHz timer can express with margin; faster would just produce
 * 32 readings of one instant.
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
	core_cb_ed(&evt);

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

#endif /* CONFIG_RADIANT_ED_SCAN */

#if defined(CONFIG_RADIANT_ED_SCAN)
static void program_ed(const struct radiant_ed_req *req);
#endif

int radiant_radio_ed(const struct radiant_ed_req *req, uint32_t *op)
{
#if !defined(CONFIG_RADIANT_ED_SCAN)
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
	enum gate_rc   g;
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
	CLAIM_TERMINAL();
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

	/*
	 * An ED chunk's span is (hi - lo + 1) * dwell by construction - the
	 * scheduler sizes the range from the gap for exactly that reason - so
	 * the reservation is that span and no guesswork. No follow-on: nothing
	 * follows an energy-detect index at a fixed instant.
	 *
	 * PRIO_NORMAL, the request class ADR 0013 is about: energy detect
	 * already "takes gaps only", and under an arbiter it is what gives way
	 * when the other stack wants the air.
	 */
	g = gate_acquire(GATE_OP_ED, req->t_start - (radiant_time_t)RAMP_UP_US,
			 req->t_start +
				 (radiant_time_t)(((uint32_t)req->rf_index_hi -
						   req->rf_index_lo + 1u) *
						  req->dwell_us),
			 0u, RADIANT_GATE_PRIO_NORMAL);
	if (g == GATE_DENIED) {
		rc = RADIANT_RADIO_EDENIED;
		goto fail;
	}

	radiant_op.id = radiant_op.next_id++;
	if (radiant_op.next_id == 0u) {
		radiant_op.next_id = 1u;
	}
	*op = radiant_op.id;

	if (g == GATE_PENDING) {
		radiant_staged.ed = *req;
		radiant_staged.pending = true;
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
		d2_settle_now();
#endif
		return RADIANT_RADIO_OK_RC;
	}

	program_ed(req);
	LOG_DBG("ed op=%u rf=%u..%u dwell=%u start=+%d",
		(unsigned)radiant_op.id, (unsigned)req->rf_index_lo,
		(unsigned)req->rf_index_hi, (unsigned)req->dwell_us,
		(int)(int64_t)(req->t_start - now));
	return RADIANT_RADIO_OK_RC;

fail:
	LOG_DBG("ed refused: %d", rc);
	radiant_op.kind = OP_NONE;
	return rc;
#endif /* CONFIG_RADIANT_ED_SCAN */
}

#if defined(CONFIG_RADIANT_ED_SCAN)
/* Everything an energy-detect chunk does to the peripheral, from the staged
 * request. See program_rx() for why it is a function. */
static void program_ed(const struct radiant_ed_req *req)
{
	uint32_t start_cc;

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
	 * and is taken away again in deliver_terminal(). Leaving it enabled would
	 * put an extra interrupt at the head of every ordinary receive window;
	 * the receive path is byte-identical when no ED operation is running.
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
}
#endif /* CONFIG_RADIANT_ED_SCAN */

/* ---------------------------------------------------------------------------
 * WHAT THE OTHER STACK DOES TO THE RADIO INSIDE OUR GRANT, AND WHY THIS FILE
 * DOES NOTHING ABOUT IT.
 *
 * nrf_802154_trx_disable() calls nrf_radio_reset(), which on nRF54L takes the
 * !defined(RADIO_POWER_POWER_Msk) path: every RADIO SUBSCRIBE and PUBLISH
 * register to zero, INTENCLR00 = 0xffffffff, TASKS_SOFTRESET. It is reached
 * from nrf_802154_core.c's on_timeslot_ended(), which the 802.15.4 driver runs
 * when MPSL takes the radio away from it - i.e. to give it to us. In NCS that
 * notification comes out of mpsl_low_priority_process(), and
 * nrf/subsys/mpsl/init/mpsl_init.c runs that from the MPSL WORK QUEUE THREAD,
 * so the reset lands wherever that thread is next scheduled: routinely inside
 * our own granted timeslot. Visible in the seam probe (SUBSCRIBE_RXEN ours at
 * grant entry, zero at grant end).
 *
 * IT IS NOT WHAT COSTS PACKETS. The `sw:` cross-tab counts SHORT receive
 * windows - the tracked slots the loss figure is computed from - and over 240
 * of them beside an OpenThread leader reads wipe=0, ramp=240, rx=239: not one
 * tracked window loses its routing. The reset does reach the LONG sweep
 * windows, whose grants are tens of milliseconds and therefore wide enough for
 * the MPSL work queue to be scheduled inside; that costs discovery SPEED under
 * a second stack and is a known, unfixed leak, not tracked loss.
 *
 * SO NOTHING IS REPAIRED OR HELD HERE. An in-grant detect-and-repair cannot
 * work in principle - a thread runs at any instant we are not on the CPU, so a
 * check is only ever a check against one instant - and it has nothing to fix
 * on the path that matters. What did account for the loss was a stale
 * MPSL_TIMER0 compare event ending each grant ~14 us after it started, before
 * the window it was bought for was due to open; see
 * radiant_radio_nrf_gate_mpsl.c.
 * ---------------------------------------------------------------------------
 */

/* ---------------------------------------------------------------------------
 * What the gate calls back
 *
 * Neither runs in the direct build (GATE_PENDING is a state the direct gate
 * cannot enter), so these are unreachable on a shipping dongle image but kept
 * because the gate's header declares them - the alternative, an #ifdef on the
 * gate inside the arm paths, is the "backend selected by a part number" shape
 * the HAL's rules keep out.
 * ---------------------------------------------------------------------------
 */

void radiant_nrf_gate_on_grant(void)
{
	unsigned int key;
	int          rc = RADIANT_RADIO_OK_RC;

	key = irq_lock();
	if (!radiant_staged.pending) {
		/*
		 * A grant for an operation that has already ended (aborted, or
		 * resolved before the arbiter got round to us) - not rare, the
		 * same race the op id exists for. Given straight back: programming
		 * for an operation nobody is waiting for would arm the radio
		 * against a window with no owner.
		 */
		irq_unlock(key);
		gate_release();
		return;
	}
	radiant_staged.pending = false;
	irq_unlock(key);

	/*
	 * Take the peripheral, for the duration of this grant and no longer: the
	 * interrupt mask, and the RADIO's own end of our (D)PPI connections. See
	 * radiant_radio_enable() and radio_endpoints_attach() for what leaving
	 * either of them set does to the stack that has the radio the rest of
	 * the time.
	 *
	 * THE LOAN STARTS HERE, on the first line that writes a shared RADIO
	 * register, and not at SIGNAL_START: the two early returns above
	 * (nostage, and the freed-slot `default:` below) leave without touching
	 * the peripheral, and a flag set before them would licence
	 * on_grant_end() to hand back a radio that was never borrowed.
	 */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
	radio_on_loan = true;
#endif
	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_END_MASK |
					NRF_RADIO_INT_DISABLED_MASK);

#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
	/*
	 * TAKE THE NVIC LINE FOR THE GRANT TOO, NOT JUST THE MASK.
	 *
	 * Unmasking END/DISABLED in RADIO->INTENSET00 above only gets as far as
	 * the peripheral. The vector itself is RADIO_0_IRQn, which nrf/subsys/
	 * mpsl/init/mpsl_init.c owns (IRQ_DIRECT_CONNECT, zero-latency) and
	 * which MPSL turns back into MPSL_TIMESLOT_SIGNAL_RADIO for us - and
	 * nrfxlib/mpsl/doc/mpsl.rst is explicit that RADIO is in the "no
	 * interrupts" set on nRF54L: "If the Timeslot API is used for RADIO
	 * access, the application is responsible for enabling and disabling the
	 * interrupt for RADIO."
	 *
	 * radiant_radio_enable() enables it ONCE at startup, which is enough
	 * beside the SoftDevice Controller and not enough beside OpenThread: the
	 * nRF 802.15.4 driver NVIC-disables the same line in its own teardown
	 * (nrf_802154_trx.c, whose nRF54L reset path also writes INTENCLR00 =
	 * 0xffffffff and triggers TASKS_SOFTRESET), so the first time that stack
	 * stops its receiver our enable is gone for the rest of the boot. The
	 * RADIO then raises END and DISABLED inside our grant and the CPU never
	 * takes the interrupt - MPSL has nothing to forward, radio_isr() is never
	 * entered, and the window runs to the grant's TIMER0 end still armed and
	 * is reported DONE_DENIED. Measured on P4 MED: ramp=290/194, ev=0x1f
	 * (receiver up, ADDRESS/END/DISABLED all set), sigrad=0, isr=0, 0 of 961
	 * packets.
	 *
	 * So the line is taken for the duration of the grant and put back the
	 * way it was found, exactly like the endpoint registers. Restoring
	 * rather than unconditionally disabling matters: between grants this
	 * vector is MPSL's and the 802.15.4 driver's, and turning it off under
	 * them would be the same class of mistake in the other direction.
	 */
	radio_irq_was_enabled = (NVIC_GetEnableIRQ(RADIANT_RADIO_IRQn) != 0U);
	if (!radio_irq_was_enabled) {
		NVIC_EnableIRQ(RADIANT_RADIO_IRQn);
	}
#endif

	/*
	 * Take the RADIO endpoints another stack holds, for this grant only.
	 * No-op unless another stack held one at init - see
	 * radio_endpoints_attach(), which moves only the masked registers and
	 * restores THEIR value, not zero. Runs before program_rx()/program_tx()
	 * since an arm whose RADIO endpoint still carries the other stack's
	 * channel never opens.
	 *
	 * Measured 2026-08-12, nRF54L15 beside OpenThread (mask =
	 * PUBLISH_ADDRESS + SUBSCRIBE_RXEN): the Thread child stayed attached
	 * over 900 datagrams/130 s with zero send failures, matching a swap-off
	 * baseline. Residual risk is real though - that did not exercise every
	 * abort path, and a swap of a register another driver treats as
	 * write-once must be exception-safe on every path out of a grant.
	 *
	 * The allocation-time borrow above is different and stays regardless:
	 * bounded to init, hands registers straight back, and is what makes
	 * init succeed at all in this image.
	 */
	radio_endpoints_attach(true);

	switch (radiant_op.kind) {
	case OP_RX:
		rc = program_rx(&radiant_staged.rx);
		break;
	case OP_TX:
		rc = program_tx(&radiant_staged.tx);
		break;
#if defined(CONFIG_RADIANT_ED_SCAN)
	case OP_ED:
		program_ed(&radiant_staged.ed);
		break;
#endif
	default:
		/* The slot was freed under us between the acquire and the
		 * grant. Nothing to programme and nothing to report - whoever
		 * freed it delivered the terminal event. */
		gate_release();
		return;
	}

	if (rc != RADIANT_RADIO_OK_RC) {
		/*
		 * Only reachable failure is ETIME: the grant arrived too late.
		 * FAILED, not DENIED - we WERE given the air and could not use it
		 * in time, and a denial would wrongly blame the other stack in the
		 * stat that separates the two. Handed back to the gate rather than
		 * delivered here: this runs inside the grant's signal callback, and
		 * a terminal here would run the completion callback, a scheduler
		 * pass, and possibly another arm before this callback can return.
		 */
		radiant_op.gate_failed = true;
	}
}

/*
 * Deliver the terminal for a grant whose programming failed. Called by the gate
 * from thread context once the timeslot has been given back - never from
 * inside the signal callback. See radiant_nrf_gate_on_grant().
 */
void radiant_nrf_gate_finish_failed(void)
{
	if (!radiant_op.gate_failed) {
		return;
	}
	radiant_op.gate_failed = false;
	deliver_terminal(RADIANT_RADIO_STATUS_FAILED);
}

#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
/*
 * The RADIO's own view of our (D)PPI, attached and detached with the grant.
 *
 * nrfx_gppi_conn_alloc() writes the RADIO's PUBLISH_* / SUBSCRIBE_* registers;
 * conn_enable()/conn_disable() only toggle the DPPIC channel, leaving those
 * registers as alloc set them - so even a "disabled" connection leaves the
 * RADIO publishing events onto a channel (t_sync capture is enabled at init
 * and never disabled, correct for the direct build where it's always live).
 *
 * Under arbitration those registers belong to whoever holds the timeslot:
 * leaving PUBLISH_ADDRESS set means the SoftDevice Controller's own packets
 * publish onto our channel, on a peripheral it believes it configured
 * itself - it asserts ~4 ms after the first advertising event, with no
 * ANT+ channel ever opened.
 *
 * So they're written on grant entry and cleared on the way out. The DPPI
 * CHANNELS stay allocated across the boundary - only the RADIO's end moves -
 * because allocating/freeing five connections every ~90 ms is churn with a
 * failure mode (allocation can fail) in the one path that must not have one.
 *
 * THE REGISTER CONTENTS ARE SAVED RATHER THAN RECONSTRUCTED:
 * nrfx_gppi_conn_alloc() hands back an opaque handle, not a channel number, so
 * rebuilding these six values would mean knowing how a connection maps onto
 * channels - nrfx's business, and it changes between SoC families. Reading
 * back what alloc wrote asks nrfx no questions and cannot disagree with it.
 */
static void radio_endpoints_save(void)
{
#if !RADIANT_NRF_RADIO_EP
	/*
	 * A PPI part has no endpoint register to save; see RADIANT_NRF_RADIO_EP
	 * for why the whole layer is inert here. The gate IS buildable on an
	 * nRF52840 - MPSL supports the part - so this needs the silicon guard as
	 * well as the gate guard, or the six NRF_RADIO->PUBLISH_/SUBSCRIBE_
	 * dereferences below are six hard compile errors on a part whose
	 * NRF_RADIO_Type has no such members.
	 *
	 * `saved` stays false, which is already radio_endpoints_attach()'s own
	 * early-out condition, so the swap is inert.
	 */
	radio_ep.saved     = false;
	radio_ep_mine_mask = 0U;
	return;
#else
	radio_ep.pub_address   = NRF_RADIO->PUBLISH_ADDRESS;
	radio_ep.pub_rxready   = NRF_RADIO->PUBLISH_RXREADY;
	radio_ep.sub_rssistart = NRF_RADIO->SUBSCRIBE_RSSISTART;
	radio_ep.sub_rxen      = NRF_RADIO->SUBSCRIBE_RXEN;
	radio_ep.sub_txen      = NRF_RADIO->SUBSCRIBE_TXEN;
	radio_ep.sub_disable   = NRF_RADIO->SUBSCRIBE_DISABLE;
	radio_ep.saved         = true;

	/*
	 * The swap set: every endpoint this backend actually programmed. Not
	 * "every endpoint someone else held at init" - see
	 * radio_endpoints_attach(). A register we never wrote stays out of it,
	 * so the swap can only ever move our own routing in and out. Printed
	 * because an empty or inert swap is otherwise indistinguishable from a
	 * healthy gate that simply hears nothing.
	 */
	radio_ep_mine_mask = 0U;
	for (size_t r = 0U; r < RADIO_EP_COUNT; r++) {
		const uint32_t mine[RADIO_EP_COUNT] = {
			radio_ep.pub_address,   radio_ep.pub_rxready,
			radio_ep.sub_rssistart, radio_ep.sub_rxen,
			radio_ep.sub_txen,      radio_ep.sub_disable,
		};

		if (mine[r] == 0U) {
			continue;
		}
		radio_ep_mine_mask |= (uint8_t)(1U << r);
		LOG_INF("RADIO %s: ours=0x%08x, swapped in per grant "
			"(held at init by another stack: %s)",
			radio_ep_name(r), (unsigned int)mine[r],
			((radio_ep_contended & (1U << r)) != 0U) ? "yes" : "no");
	}
	if (radio_ep_mine_mask == 0U) {
		LOG_ERR("no RADIO endpoint was allocated to this backend - the "
			"per-grant swap is empty and ANT+ cannot drive the "
			"radio at all");
	}
#endif /* !RADIANT_NRF_RADIO_EP */
}

/*
 * WHICH REGISTERS ARE SWAPPED, AND WHOSE VALUE GOES BACK, ARE BOTH DECIDED AT
 * THE GRANT AND NOT AT INIT. Deciding once at init, from `radio_ep_contended`
 * - the registers that already held a foreign value when radiant_radio_init()
 * ran - is a boot-order race with a silent failure on the losing side, and
 * both orders happen on the same board, boot to boot:
 *
 *   802.15.4 first. Its values are there, radio_ep_contended comes out as
 *   PUBLISH_ADDRESS + SUBSCRIBE_RXEN, the swap engages, ANT+ works.
 *
 *   RadiANT first. Nothing is taken, the mask comes out EMPTY, and the swap
 *   is compiled in but permanently inert. The 802.15.4 driver then writes its
 *   own PUBLISH_ADDRESS/SUBSCRIBE_RXEN over ours - it writes those registers
 *   directly rather than through nrfx_gppi_conn_alloc(), so nothing refuses
 *   it and nothing is logged - and our CC_START compare no longer reaches
 *   TASKS_RXEN. The receiver never ramps, so no frame can arrive; and because
 *   TASKS_DISABLE on an already-DISABLED radio raises no EVENTS_DISABLED, the
 *   window never produces a terminal either. Every granted timeslot runs to
 *   its TIMER0 end with the operation still armed and is reported DONE_DENIED.
 *   Measured on P4 MED with the seam probe: grant=256 prog=238 ok, sigrad=0,
 *   isr=0, ramp=0/318, SUBSCRIBE_RXEN reading 0x00000000 at every point inside
 *   the grant, 0 of 961 packets. Two consecutive boots of the SAME image gave
 *   the mask 2 and then 0.
 *
 * So the set is radio_ep_mine_mask - the registers this backend actually
 * programmed - and the value handed back is whatever was in the register at
 * THIS grant's entry. Correct by construction under any init order, and it
 * survives the other stack changing its own routing between grants. Restoring
 * ZERO instead is wrong twice over: it touches registers nobody is contending
 * (throwing away our own always-live t_sync capture), and zero is not what was
 * there - the 802.15.4 driver writes SUBSCRIBE_RXEN at its own init, so
 * clearing it silently stops its receiver ramp-up.
 *
 * With no second stack the entry value IS our value, so every write is a
 * write-back of what was already there and the uncontended build is untouched.
 */
static void radio_endpoints_attach(bool on)
{
	if ((RADIANT_NRF_GRANT_EP_SWAP == 0) || !radio_ep.saved ||
	    radio_ep_mine_mask == 0U) {
		return;
	}
#if !RADIANT_NRF_RADIO_EP
	/*
	 * Unreachable on a PPI part - radio_endpoints_save() leaves `saved`
	 * false and the mask zero, so the early-out above has already fired.
	 * The guard is here anyway because the compiler does not know that, and
	 * radio_ep_reg() below simply does not exist on this silicon.
	 */
	ARG_UNUSED(on);
#else

	/* Our values, in radio_ep_reg() order, so the mask indexes both. */
	const uint32_t mine[RADIO_EP_COUNT] = {
		radio_ep.pub_address,   radio_ep.pub_rxready,
		radio_ep.sub_rssistart, radio_ep.sub_rxen,
		radio_ep.sub_txen,      radio_ep.sub_disable,
	};

	for (size_t r = 0U; r < RADIO_EP_COUNT; r++) {
		if ((radio_ep_mine_mask & (1U << r)) == 0U) {
			continue;
		}
		if (on) {
			/* Read before write: this is where the other stack's
			 * current value is learned, every time. */
			radio_ep_foreign[r] = *radio_ep_reg(r);
			*radio_ep_reg(r) = mine[r];
		} else {
			*radio_ep_reg(r) = radio_ep_foreign[r];
		}
	}
#endif /* !RADIANT_NRF_RADIO_EP */
}
#endif /* CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL */

void radiant_nrf_gate_on_grant_end(void)
{
	/*
	 * NOTHING BELOW MAY RUN FOR A TIMESLOT THAT NEVER TOOK THE
	 * PERIPHERAL. This function is reached from gate_hand_back(), which is
	 * driven by g.hw_held - and hw_held is set at SIGNAL_START for EVERY
	 * granted timeslot, including the anchor-establishing bootstrap, which
	 * returns ACTION_END without calling radiant_nrf_gate_on_grant() and so
	 * without enabling an interrupt, attaching an endpoint or programming a
	 * single register. Handing back a radio that was never borrowed means
	 * clearing RADIO->SHORTS and forcing TASKS_DISABLE on a peripheral the
	 * 802.15.4 driver is receiving on. See RADIO_IS_OURS() for the assert
	 * that produces.
	 *
	 * The gate's own bookkeeping (stats.grant_end_calls) is
	 * deliberately left alone: the "exactly one on_grant_end() per granted
	 * timeslot" invariant the soak is scored on stays exactly as it was, and
	 * only the register writes become conditional.
	 */
	if (!RADIO_IS_OURS()) {
		return;
	}

	/*
	 * Hand the peripheral back in the state it was lent in. The interrupt
	 * mask is the part that matters: two bits left set in RADIO->INTENSET
	 * are two bits set while somebody else transmits on the same radio,
	 * and the SoftDevice Controller asserts within milliseconds. See
	 * radiant_radio_enable().
	 *
	 * Everything else this operation programmed ((D)PPI connections,
	 * shorts) is torn down by deliver_terminal() on the ordinary path;
	 * this runs on EVERY path that ends a grant, including ones where no
	 * terminal was owed.
	 *
	 * Exactly our own bits, never ~0 - this register is shared.
	 * nrf_radio_int_disable() writes INTENCLR00, and line 0 is MPSL's
	 * (mpsl_init.c: MPSL_RADIO_IRQn = RADIO_0_IRQn), so ~0 here would
	 * clear the SoftDevice Controller's own interrupt config, and it
	 * then misses its events and asserts. (An earlier version of this
	 * function used ~0 while fixing the opposite bug - same symptom, new
	 * cause.)
	 */
	nrf_radio_int_disable(NRF_RADIO, NRF_RADIO_INT_END_MASK |
					 NRF_RADIO_INT_DISABLED_MASK |
					 NRF_RADIO_INT_RXREADY_MASK);

	/*
	 * Hand it back idle, not merely quiet - masking the interrupt stops
	 * us hearing the radio, it doesn't stop the radio, and the radio
	 * handed back here is what the SoftDevice Controller picks up
	 * microseconds later. Nordic's ncs-esb-ble-mpsl-demo and
	 * inductivekickback/ncs_ble_esb_demo both put the peripheral back to
	 * DISABLED at the end of every timeslot.
	 *
	 * Two things leak without this. SHORTS is the worse: this function
	 * runs on every path that ends a grant, including ones where
	 * deliver_terminal() never ran (a grant that ran out under an armed
	 * receive, an EXTEND_FAILED, a session closing), leaving
	 * RADIO->SHORTS as program_rx() wrote it - our END->DISABLE short
	 * left set means the controller's next advertising packet ends and
	 * disables the controller's own radio, out from under it. Second, the
	 * state itself: a window whose air ran out mid-receive hands back a
	 * RADIO in RX, not the DISABLED state MPSL's next owner assumes.
	 *
	 * Order matters: interrupts masked first so the
	 * DISABLED this provokes isn't mistaken for an operation's terminal;
	 * SHORTS cleared before the disable so it can't chain into anything.
	 * radio_disable_now() is the existing bounded spin, safe to call from
	 * inside a signal callback. Neither register here is shared, unlike
	 * INTENCLR00 above - SHORTS and TASKS_DISABLE belong to whoever holds
	 * the timeslot, which until this function returns is us.
	 */
	nrf_radio_shorts_set(NRF_RADIO, 0);
	radio_disable_now();

	/*
	 * The pending NVIC bit is deliberately NOT cleared, unlike the
	 * sample: line 0 here is also MPSL's, so NVIC_ClearPendingIRQ() could
	 * swallow a controller interrupt - the same shared-register mistake
	 * as ~0 above, one level up. Not needed either: our mask is clear and
	 * every event we raise is consumed above, so a surviving pending bit
	 * enters radio_isr() with kind == OP_NONE and is dropped.
	 */

	/*
	 * The (D)PPI endpoints are NOT detached here - a known remaining leak,
	 * not a decision that they don't matter. Adding
	 * radio_endpoints_attach(false) unconditionally here was measured to
	 * stop the receive path dead (chunks=256, end ok=0, deny=256): every
	 * window ran its full length and never completed, the signature of
	 * the window-end RADIO event not reaching the handler. Left attached,
	 * ANT+ works and the controller doesn't; left detached, neither does.
	 *
	 * What's called below is different: radio_endpoints_attach() moves
	 * only the registers another stack held at init (radio_ep_contended)
	 * and restores THEIR value, not zero. In every build the failure
	 * above was measured on, that mask is empty, so this call is a no-op
	 * and that failure can't recur. Beside OpenThread the mask is
	 * PUBLISH_ADDRESS + SUBSCRIBE_RXEN, neither load-bearing at this
	 * instant (RXEN's edge already fired; ADDRESS has nothing to capture
	 * until the next grant) - the window-end path runs on
	 * SUBSCRIBE_DISABLE and END, both untouched. Unproven rather than
	 * disproven: a 2026-08-12 test of the enabled pair was confounded by
	 * a down Thread leader.
	 */
	radio_endpoints_attach(false);

#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
	/* Put the vector back the way the grant found it. See
	 * radiant_nrf_gate_on_grant(). Last, after the mask is clear and the
	 * radio is idle, so nothing we raised can be taken on the way out. */
	if (!radio_irq_was_enabled) {
		NVIC_DisableIRQ(RADIANT_RADIO_IRQn);
	}
	/* The loan ends on the last line that touches the peripheral, so
	 * anything reached after this - a terminal delivered from the work
	 * queue, an abort from thread context - finds the radio not ours and
	 * leaves its registers alone. See RADIO_IS_OURS(). */
	radio_on_loan = false;
#endif
}

#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
void radiant_nrf_gate_on_booking_due(void)
{
	if (radiant_op.cbs != NULL && radiant_op.cbs->book_due != NULL) {
		radiant_op.cbs->book_due(radiant_op.user);
	}
}
#endif

void radiant_nrf_gate_on_radio_irq(void)
{
	/* Exactly the handler the NVIC would have entered, entered by hand.
	 * radio_isr() tests each event flag and clears what it consumes, so it
	 * is idempotent across the part's two RADIO lines. See
	 * radiant_radio_nrf_gate.h for why the vector isn't ours in a
	 * timeslot. */
	radio_isr(NULL);
}

void radiant_nrf_gate_on_denied(void)
{
	unsigned int key;

	key = irq_lock();
	radiant_staged.pending = false;
	irq_unlock(key);

	/*
	 * Delivered unconditionally, never guarded on radiant_staged.pending: an
	 * operation can be armed and staged, have its staging cleared by an
	 * unrelated terminal, and still be the one the core is waiting on.
	 * Measured with such a guard in place: a grant whose programming failed
	 * delivered FAILED and cleared the flag, the next request was denied,
	 * this function returned silently, and the core's single operation slot
	 * stayed occupied forever (chunks=2, fail=1, deny=0, search stopped for
	 * good).
	 *
	 * deliver_terminal() is idempotent, so calling it unconditionally is the
	 * only way to guarantee the HAL's promise of exactly one terminal per
	 * accepted arm. No register was touched (compare never programmed, RADIO
	 * never configured), so this is a clean terminal, not a teardown. May run
	 * in a cooperative thread rather than the radio interrupt - MPSL delivers
	 * BLOCKED/CANCELLED from mpsl_low_priority_process().
	 */
	deliver_terminal(RADIANT_RADIO_STATUS_DENIED);
}

/* ---------------------------------------------------------------------------
 * Abort
 * ---------------------------------------------------------------------------
 */

int radiant_radio_abort(void)
{
	unsigned int key;
	bool         was_staged;

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
	/* SHORTS and TASKS_STOP are the peripheral's, not ours, unless
	 * a grant is live. An abort commonly arrives from thread context for an
	 * operation that was only ever STAGED - no grant, nothing programmed -
	 * and stopping another stack's receiver mid-frame is how that presented.
	 * See RADIO_IS_OURS(). */
	if (RADIO_IS_OURS()) {
		nrf_radio_shorts_set(NRF_RADIO, 0);
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_STOP);
	}
	/* An operation that was still waiting for a grant is aborted too, and it
	 * is the easy case: nothing was programmed, so there is nothing to tear
	 * down. Clearing the flag under the same lock is what stops a grant that
	 * is already in flight from programming the radio for a window whose
	 * owner has gone. */
	was_staged = radiant_staged.pending;
	radiant_staged.pending = false;
	irq_unlock(key);
	/* Owed rather than paid when the core aborted from its own completion
	 * to arm a follow-on into the same air - see THE GRANT HANDOFF. */
	grant_release_after_core();
	/* An owed release keeps the held grant, which is right, but the
	 * request this operation was staged on is no one's now. */
	if (was_staged) {
		gate_forget_pending();
	}

	/*
	 * The operation slot must be released synchronously here. Triggering
	 * TASKS_DISABLE and returning, leaving radiant_op.kind set until
	 * EVENTS_DISABLED reaches the interrupt handler, breaks the caller that
	 * matters most: radiant_transfer.c arms its acknowledged-data reply from
	 * inside the receive callback (1560 us deadline), already inside the
	 * RADIO interrupt, so that DISABLED cannot be serviced until the handler
	 * returns - the re-arm finds radiant_op.kind still OP_RX, is refused
	 * EBUSY, and the reply sits until the next housekeeping pump (up to 50 ms
	 * later) with its t_sync long past. On the bench that reads as a marginal
	 * link completing about one time in three, when really the frame was
	 * never transmitted.
	 *
	 * So the disable completes and its event is consumed before returning
	 * (radio_disable_now()), and the terminal is delivered from here - not a
	 * double delivery, since deliver_terminal() is idempotent and
	 * radio_disable_now() already took the DISABLED away from the handler.
	 * This keeps the HAL's promise that an aborted operation still produces
	 * exactly one terminal event.
	 */
	/* Only if the radio is ours. Forcing TASKS_DISABLE on another
	 * stack's live receiver is the same mistake as the SHORTS write above,
	 * and the spin that follows it would then be waiting on an event that
	 * driver is entitled to consume. With no grant held there is nothing to
	 * take down anyway - the operation being aborted was never programmed. */
	if (RADIO_IS_OURS()) {
		radio_disable_now();
	}
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
 * Leaving that DISABLED in flight fails with no symptom except silence. The
 * HAL invites a completion callback to arm the next operation immediately,
 * and radiant_transfer.c does exactly that for the acknowledgement's receive
 * window (1.55 ms turnaround leaves no slower option) - so control returns to
 * the EVENTS_DISABLED test at the bottom of radio_isr(), now set by the
 * PREVIOUS operation's disable, and tears down the just-armed window as if it
 * were its own terminal event. The reply window is armed and destroyed inside
 * one interrupt, the peer's acknowledgement arrives at a disabled radio, and
 * the transfer reports NO_ACK with nothing logged - indistinguishable from a
 * genuine on-air failure.
 *
 * Waiting makes it unambiguous: ramp-down is a few microseconds, so the event
 * is consumed here rather than misattributed later. The bound is a backstop,
 * not an expected timeout, spun against this backend's own 1 MHz TIMER so it
 * is a real duration regardless of core clock.
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
	/*
	 * The four disables above are our own DPPIC channels and are safe from
	 * any context; RADIO->SHORTS is not ours between grants.
	 *
	 * This is the write that crashed the contended build: a DENIED terminal
	 * runs from the gate's work queue with no timeslot held, and under a
	 * refusing arbiter that is ~80 times a second. Every one of them cleared
	 * the 802.15.4 driver's PHYEND->DISABLE short mid-frame. See
	 * RADIO_IS_OURS().
	 *
	 * Nothing is lost by skipping it: our shorts only exist inside a grant,
	 * and radiant_nrf_gate_on_grant_end() already cleared them on the way
	 * out of the one that programmed them.
	 */
	if (RADIO_IS_OURS()) {
		nrf_radio_shorts_set(NRF_RADIO, 0);
	}

	radiant_op.kind = OP_NONE;
	/*
	 * Give the air back promptly: a tracked window that closed empty would
	 * otherwise hold ~2 ms of follow_on reservation for a reply now known not
	 * to be coming (four times a second per sensor). But not before the
	 * callback has had its chance to arm a follow-on INTO that reservation,
	 * which is what it was for: an arbitrated grant is handed over while the
	 * completion runs and released once it has, so an empty window still
	 * gives its reserve back within the trampoline's latency. See THE GRANT
	 * HANDOFF.
	 */
	radiant_staged.pending = false;
	grant_release_after_core();

#if defined(CONFIG_RADIANT_ED_SCAN)
	if (kind == OP_ED) {
		struct radiant_ed_event evt;

		/* RXREADY goes back to not being an interrupt, so an ordinary
		 * receive window runs against the same mask it did before this
		 * operation existed (see the enable in radiant_radio_ed()).
		 * Guarded like every other shared-register write here:
		 * INTENCLR00 is the arbiter's register between grants. */
		if (RADIO_IS_OURS()) {
			nrf_radio_int_disable(NRF_RADIO,
					      NRF_RADIO_INT_RXREADY_MASK);
		}

		memset(&evt, 0, sizeof(evt));
		evt.op = radiant_op.id;
		evt.status = st;
		/* No measurement on a terminal event, on the same terms as
		 * radiant_rx_event's frame fields on a TIMEOUT. The indices
		 * that were measured were reported as they happened. */
		core_cb_ed(&evt);
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
		core_cb_tx(&evt);
	} else {
		struct radiant_rx_event evt;

#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
		rx_diag_window_end(st != RADIANT_RADIO_STATUS_TIMEOUT);
#endif
		/* On this backend "OK" is a window that heard any frame. */
		NRF_DIAG(if (st != RADIANT_RADIO_STATUS_TIMEOUT) {
				nrf_diag.term_error++;
			} else if (radiant_op.rx_any) {
				nrf_diag.term_ok++;
			} else {
				nrf_diag.term_empty++;
			});
		memset(&evt, 0, sizeof(evt));
		evt.op = radiant_op.id;
		evt.status = st;
		evt.t_sync = radiant_radio_now();
		evt.t_sync_exact = false;

		/*
		 * Noise floor: RSSI measured inside this window with no packet
		 * present. Each condition is load-bearing: TIMEOUT not ABORTED,
		 * since only a window that ran to close definitely ramped up and
		 * sampled THIS window (RSSISAMPLE has no "invalid" and would
		 * otherwise hold a stale value); nothing received, since a heard
		 * packet overwrites RSSISAMPLE via the ADDRESS short with the
		 * transmitter's level; and the same errata/temperature correction
		 * as rssi_dbm, so the two are on one scale.
		 */
		if (st == RADIANT_RADIO_STATUS_TIMEOUT && !radiant_op.rx_any) {
			evt.noise_dbm = rssi_sample_dbm();
			evt.has_noise = true;
		}

		core_cb_rx(&evt);
	}
}

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);
	radiant_dbg_isr++;

#if defined(CONFIG_RADIANT_ED_SCAN)
	/*
	 * First, and only while an ED sweep is running: RXREADY is unmasked
	 * only for its duration, so this costs one event-flag read on every
	 * other interrupt. The kind test isn't redundant with the mask: an
	 * RXREADY from the last dwell can still be pending in the NVIC after
	 * the terminal event retired the operation, and running a burst
	 * against nobody's operation would sample a receiver that's off.
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
			 * The frame is out. Delivered from END rather than the
			 * DISABLED that follows, because the slot must be free
			 * before the callback runs so it can arm the next
			 * operation - and this makes the DISABLED branch below a
			 * no-op here instead of a second delivery.
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
			/* A hardware capture, exactly as on receive: EVENTS_ADDRESS
			 * fires when the address has been SENT, captured by the
			 * same always-live (D)PPI connection - so a master closes
			 * its slot phase against what actually went out. */
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
#if defined(CORE_CB_DEFERRED)
			/* This completion is a terminal too, so it owes the
			 * release deliver_terminal() would have. See THE GRANT
			 * HANDOFF. Only where a release can be owed: a direct
			 * build's gate has nothing to give back. */
			grant_release_after_core();
#endif
			core_cb_tx(&evt);
		}

		if (radiant_op.kind == OP_RX && !radiant_op.terminal_sent &&
		    !nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS)) {
			/*
			 * AN END WITHOUT AN ADDRESS MATCH IN THIS WINDOW IS NOT A
			 * RECEPTION, AND DELIVERING IT REPLAYS THE PREVIOUS FRAME.
			 *
			 * Everything the branch below reports is left over from
			 * the last real frame when no new one arrived: the body is
			 * radiant_rx_buf, which nothing clears between windows;
			 * crc_ok reads CRCSTATUS, which holds its value until the
			 * next packet; and t_sync reads the CC_SYNC capture, which
			 * only EVENTS_ADDRESS ever writes. So an END arriving with
			 * no address match hands the core a byte-perfect copy of
			 * the previous frame carrying a stale timestamp.
			 *
			 * Measured on an nRF54L15 DK, 2026-08-17: a master with NO
			 * PEER ANYWHERE ON THE AIR delivered one of these per
			 * channel period, indefinitely - 79 acknowledged-data
			 * messages to the host in 20 s, 4.0/s, all of them replays
			 * of one acknowledged packet received minutes earlier. See
			 * docs/treadmill-reference-design.md section 7.8.
			 *
			 * ADDRESS is the right gate rather than a heuristic: it is
			 * cleared when the window is armed (see program_rx), it is
			 * raised by hardware on a sync-word match, and the frame's
			 * own t_sync is already read from the capture it drives -
			 * so a delivery this test rejects had no honest timestamp
			 * to report either.
			 */
			radiant_dbg_rx_no_addr++;
#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
			if (radiant_op.diag_track) {
				radiant_op.diag_end_noaddr = true;
				rx_diag.fr_end_noaddr++;
			}
#endif
		} else if (radiant_op.kind == OP_RX && !radiant_op.terminal_sent) {
			struct radiant_rx_event evt;
			bool crc_ok = nrf_radio_crc_status_check(NRF_RADIO);
			uint8_t logical = (uint8_t)(NRF_RADIO->RXMATCH &
						    RADIO_RXMATCH_RXMATCH_Msk);

			/*
			 * Consumed here so the NEXT packet in this window is
			 * judged on its own address match: END->START keeps the
			 * receiver armed across frames, so one window may carry
			 * several and a flag left set would pass the test above
			 * for a frame that never arrived.
			 */
			nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);

			/*
			 * Which of the caller's filters this frame belongs to.
			 * With the full address matched in hardware, RXMATCH
			 * says so outright. With a short match (ADR 0019) RXMATCH
			 * only names a filter whose LEADING bytes matched; the
			 * rest arrived as the first payload bytes and are
			 * compared here. A frame that fails that compare is
			 * another sensor's on the same network - not a
			 * reception, so no event, and END->START has already
			 * re-armed the receiver for ours. It never ends a
			 * STOP_ON_FIRST window: that stop is in software below
			 * and only an accepted frame reaches it.
			 */
			int fidx = (logical < ARRAY_SIZE(radiant_filter_slot))
					   ? radiant_filter_slot[logical] : 0;

			const uint8_t *body = radiant_rx_buf;

			if (radiant_op.sw_addr_len != 0u) {
				const uint8_t hint = (uint8_t)fidx;
				const uint8_t hw_len = (uint8_t)(
					radiant_op.addr_len -
					radiant_op.sw_addr_len);

				fidx = radiant_addr_split_match(
					radiant_op.filters, radiant_op.n_filters,
					hw_len, radiant_op.sw_addr_len, hint,
					radiant_rx_buf);
				body = radiant_addr_split_body(
					radiant_rx_buf, radiant_op.sw_addr_len);
				/*
				 * The demodulator slip (ADR 0019): our own frame
				 * with one bit deleted in its software-matched
				 * address bytes. Recovered only when that is
				 * exactly what arrived and the CRC over the
				 * reconstruction verifies - so it is delivered
				 * as an OK frame. Costs nothing on a frame that
				 * passed the compare above.
				 */
				if (fidx < 0 && !radiant_op.len_from_body) {
					fidx = radiant_addr_split_repair(
						radiant_op.filters,
						radiant_op.n_filters, hw_len,
						radiant_op.sw_addr_len, hint,
						radiant_rx_buf,
						radiant_op.body_len,
						NRF_RADIO->RXCRC,
						radiant_op.crc_init,
						radiant_repair_buf);
					if (fidx >= 0) {
						crc_ok = true;
						body = radiant_repair_buf;
						radiant_dbg_slip_repaired++;
#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
						if (radiant_op.diag_track) {
							rx_diag.fr_repaired++;
						}
#endif
					}
				}
			}

			radiant_dbg_rx_ev++;
			if (crc_ok && fidx >= 0) {
				radiant_dbg_ok++;
			}
#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
			if (radiant_op.diag_track && fidx < 0) {
				rx_diag.fr_foreign++;
				/* Same ring as CRC failures, marked by RXCRC's
				 * top bit: a short-address match whose software
				 * bytes disagreed is either another sensor or our
				 * own frame with its address bytes corrupted, and
				 * only the raw bytes tell those apart. */
				rx_diag_crc_fail(logical,
						 (crc_ok ? 0x80000000u : 0x40000000u) |
						 NRF_RADIO->RXCRC,
						 rssi_sample_dbm());
			} else if (radiant_op.diag_track) {
				if (crc_ok) {
					int32_t off = (int32_t)(int64_t)(
						clock_absolute(nrf_timer_cc_get(
							TIMER_ADDR, CC_SYNC)) +
						radiant_addr_split_t_sync_us(
							radiant_op.sw_addr_len) -
						radiant_op.diag_t_open);

					radiant_op.diag_ok = true;
					rx_diag.off_min = MIN(rx_diag.off_min, off);
					rx_diag.off_max = MAX(rx_diag.off_max, off);
				} else {
					radiant_op.diag_crc = true;
					rx_diag.fr_crc++;
					rx_diag_crc_fail(logical, NRF_RADIO->RXCRC,
							 rssi_sample_dbm());
				}
			}
#endif
			/* Before the report_crc_fail gate, deliberately. A
			 * window whose CRC failures are suppressed still HEARD
			 * something, and its RSSISAMPLE is that something's
			 * level rather than the band's - so it must not
			 * contribute a noise sample either way. */
			radiant_op.rx_any = true;
			NRF_DIAG(nrf_diag.frames++;
				 if (fidx < 0 && radiant_op.sw_addr_len != 0u) {
					 nrf_diag.sw_mismatch++;
				 } else if (fidx >= 0 && !crc_ok) {
					 nrf_diag.crc_fail++;
				 });
			if (fidx >= 0 && (crc_ok || radiant_op.report_crc_fail)) {
				memset(&evt, 0, sizeof(evt));
				evt.op = radiant_op.id;
				evt.status = crc_ok ? RADIANT_RADIO_STATUS_OK
						    : RADIANT_RADIO_STATUS_CRC_FAIL;

				/* Hardware capture, moved event-referenced ->
				 * antenna-referenced with THIS operation's PHY
				 * calibration - the two differ by 20 us, and
				 * using the wrong one is the silent
				 * off-centre-window failure T_SYNC_CAL_LR_US
				 * documents. */
				evt.t_sync = t_sync_cal(
					clock_absolute(nrf_timer_cc_get(
						TIMER_ADDR, CC_SYNC)),
					t_sync_cal_rx_us(radiant_op.phy));
				/* ADDRESS fired at the end of the hardware's
				 * short address; the HAL's t_sync is the end of
				 * the whole on-air one. Zero when nothing was
				 * left to software. */
				evt.t_sync += (radiant_time_t)
					radiant_addr_split_t_sync_us(
						radiant_op.sw_addr_len);
				evt.t_sync_exact = true;

				evt.filter_index = (uint8_t)fidx;
				/* Wildcard search (ADR 0022): the device-number
				 * byte the hardware no longer matched. Not on a
				 * repaired frame: repair never accepts a wild
				 * filter, so fidx here came from the compare. */
				if (radiant_op.filters[fidx].wild_last) {
					evt.wild_value =
						radiant_addr_split_wild_value(
							radiant_rx_buf,
							radiant_op.sw_addr_len);
				}
				evt.body = body;
				/*
				 * Length comes off the wire and is clamped
				 * before being believed. Long-range frames
				 * carry their own length in body byte 0 (HAL
				 * body includes it, so delivered length is
				 * byte+1). The clamp matters because this also
				 * runs on CRC failures, where a corrupted length
				 * byte could otherwise be reported longer than
				 * what was received, exposing the previous
				 * frame's leftover bytes - MAXLEN stops the
				 * RADIO overrunning the buffer but not this.
				 */
				if (radiant_op.len_from_body) {
					uint32_t n =
						(uint32_t)radiant_rx_buf[0] + 1u;

					if (n > radiant_op.body_len) {
						n = radiant_op.body_len;
					}
					evt.body_len = (uint8_t)n;
				} else {
					evt.body_len = radiant_op.body_len;
				}
				evt.has_rssi = true;

				/*
				 * The CRC as it arrived, on failures only.
				 * RXCRC holds the RECEIVED value on a mismatch
				 * (that's what the register is for), and the
				 * core needs it to form crc_rx ^ crc_computed,
				 * the error syndrome. Not populated on OK: a
				 * passing frame's CRC is recomputable, and a
				 * field that means different things depending on
				 * status is one nobody can reason about.
				 */
				if (!crc_ok) {
					evt.has_crc_rx = true;
					evt.crc_rx = NRF_RADIO->RXCRC;
				}
				evt.rssi_dbm = rssi_sample_dbm();

				/* The first ACCEPTED frame (radiant_radio_hal.h): a
				 * CRC failure reported for repair is not one, and the
				 * scheduler - like the CC26xx backend and the fake -
				 * keeps the window armed on it. Stopping here left it
				 * waiting on a terminal that had already happened. */
				if (radiant_op.stop_on_first && crc_ok) {
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
#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
					rx_diag_window_end(false);
#endif
					radiant_op.kind = OP_NONE;
#if defined(CORE_CB_DEFERRED)
					/* Owed like any other terminal's, or a
					 * tracked window that heard its frame holds
					 * its whole follow-on reserve to the grant's
					 * own end compare. Before the callback, so
					 * the handoff keeps the grant if that
					 * callback arms a follow-on into it. */
					grant_release_after_core();
#endif
				}
				core_cb_rx(&evt);
			}
		}
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

		if (radiant_op.kind == OP_RX) {
#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
			/* An address matched whose END never came: the window
			 * closed on top of a frame. */
			if (nrf_radio_event_check(NRF_RADIO,
						  NRF_RADIO_EVENT_ADDRESS)) {
				radiant_op.diag_cut = true;
			}
#endif
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
#if defined(CONFIG_RADIANT_ED_SCAN)
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

#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
uint8_t radiant_radio_diag_get(struct radiant_radio_diag *out)
{
	unsigned int key = irq_lock();

	out->v[RADIANT_RADIO_DIAG_RX_POSTED] = nrf_diag.rx_posted;
	out->v[RADIANT_RADIO_DIAG_RX_REFUSED] = nrf_diag.rx_refused;
	out->v[RADIANT_RADIO_DIAG_TERM_OK] = nrf_diag.term_ok;
	out->v[RADIANT_RADIO_DIAG_TERM_EMPTY] = nrf_diag.term_empty;
	out->v[RADIANT_RADIO_DIAG_TERM_ERROR] = nrf_diag.term_error;
	out->v[RADIANT_RADIO_DIAG_SYNC_ENTRIES] = nrf_diag.frames;
	out->v[RADIANT_RADIO_DIAG_SW_ADDR_MISMATCH] = nrf_diag.sw_mismatch;
	out->v[RADIANT_RADIO_DIAG_CRC_FAIL] = nrf_diag.crc_fail;
	irq_unlock(key);

	return RADIANT_DIAG_BACKEND_NRF;
}
#endif /* CONFIG_RADIANT_DIAG_COUNTERS */
