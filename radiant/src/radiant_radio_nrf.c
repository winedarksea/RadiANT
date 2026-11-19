/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf.c - radiant_radio_hal.h on a bare nRF RADIO.
 *
 * Clean-room: written from the HAL contract, the spike results and public
 * Nordic headers and specs. Nothing from sdk-ant, libant or ANT+ profile
 * documents (ADR 0002).
 *
 * Register facts were measured on nRF54L15. nRF52840 is predicted to match
 * except ramp-up and TIMING/RXGAIN. Not yet closed: PPI-vs-DPPI routing via
 * nrfx_gppi_conn_alloc(), and the errata-153 RSSI table (vendor-sourced).
 *
 * Hardware limit: caps.max_filters is 8 for SEARCH, but TRACKING gets only two
 * distinct bases. Logical 0 is BASE0+PREFIX0.AP0; logical 1..7 are BASE1+AP1..AP7.
 * A SEARCH address [A6 C5 devnum_lo] shares base [A6 C5], so all eight filters
 * fit. A TRACKING address [A6 C5 dnl dnh dtype] has a per-sensor base, so a
 * merged window holds at most two channels. arm_rx() returns ENOTSUP for a set
 * it cannot air; radiant_sched.c may split the window.
 *
 * Demodulator quirk (ADR 0019): in Nrf_1Mbit the RADIO drops one bit when an
 * alternating run of about 9+ bits lies inside the matched address, or a few
 * bits past its end. About 5 % of device numbers produce such a run in the
 * tracking address. CONFIG_RADIANT_NRF_SLIP_GUARD gives those windows a 2-byte
 * hardware match [A6 C5], so the slip lands in software-received bytes, and
 * radiant_addr_split_repair() restores the bit once the CRC confirms it. Other
 * windows keep the 5-byte match, since a 2-byte one also matches noise.
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
/*
 * nRF54L errata 20: the backend holds CONSTLAT from init. On an MPSL build a
 * raw TASKS_CONSTLAT does not survive: MPSL's own low-latency callbacks
 * (nrf/subsys/mpsl/init/mpsl_init.c) trigger CONSTLAT around each of its
 * events and LOWPWR after. Those writes are raw unless CONFIG_NRFX_POWER or
 * CONFIG_NRF_SYS_EVENT is on (on nRF54L both use nrfx's reference count), so
 * the first event's release turned the backend's request off (Gate GA
 * 2026-11-18: CONSTLATSTAT 0/200 on an MPSL L15 image, 200/200 direct). With
 * NRFX_POWER (selected by RADIANT_BACKEND_NRF_GATE_MPSL on nRF54L) MPSL goes
 * through nrfx's reference count, and so does this request - if it is made at
 * all: CONFIG_RADIANT_NRF54L_MPSL_CONSTLAT_HOLD=n leaves CONSTLAT to MPSL's
 * per-event acquire/release.
 */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL) && defined(CONFIG_SOC_SERIES_NRF54L)
#include <nrfx_power.h>
#define CONSTLAT_REFCOUNTED 1
BUILD_ASSERT(IS_ENABLED(CONFIG_NRFX_POWER),
	     "MPSL nRF54L build without CONFIG_NRFX_POWER: MPSL would raw-trigger "
	     "LOWPWR over the backend's CONSTLAT request (errata 20)");
#if defined(CONFIG_RADIANT_NRF54L_MPSL_CONSTLAT_HOLD)
/* The one reference is taken once per boot, not once per init attempt: an
 * init that fails after taking it and is retried must not take a second. */
static bool constlat_held;
#endif
#endif
#endif

#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_radio_nrf_diag.h>

/*
 * CONFIG_RADIANT_POWER_DIAG hooks. The header is included only when the option
 * is on: even unused static inlines renumber GCC's internal switch tables
 * (CSWTCH.n), which the zero-cost symbol comparison sees as a difference.
 */
#if defined(CONFIG_RADIANT_POWER_DIAG)
#include <radiant/radiant_power_diag.h>
#define POWER_DIAG(...) do { __VA_ARGS__; } while (0)
#else
#define POWER_DIAG(...) do { } while (0)
#endif

/* Private header: nothing above the backend may know who owns the RADIO. */
#include <radiant/radiant_swi.h>

#include "radiant_radio_nrf_gate.h"
#include "radiant_addr_split.h"
#include "radiant_rx_torn.h"
#include <radiant/radiant_diag.h>

/* 0xF7 backend page (ADR 0023). Fields needing an extra register read stay "not measured". */
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
static struct {
	uint32_t rx_posted, rx_refused;
	uint32_t term_ok, term_empty, term_error;
	uint32_t frames, sw_mismatch, crc_fail;
	/* Page 7: search frame cut by close or abort; END within RADIANT_DIAG_SRCH_NEAR_CLOSE_US of close. */
	uint32_t srch_cut_close, srch_cut_abort, srch_end_near_close;
	/* Page 7 fields 6-7: an arm's start compare written behind the counter (ETIME at programming). */
	uint32_t tx_start_missed, rx_start_missed;
	/* Page 9 fields 5-7: the early arm check (see nrf_diag_early()). */
	uint32_t pass_to_arm_max_us, arm_slack_min_us, early_etime;
	/* Page 10 fields 4-5: frames the late-ISR guard dropped as torn (see
	 * radiant_rx_torn() and the END branch), and the worst END-interrupt
	 * lateness of a frame it accepted. */
	uint32_t late_isr_drops, end_late_max_us;
} nrf_diag = {
	.arm_slack_min_us = RADIANT_DIAG_NO_SAMPLE,
};
#define NRF_DIAG(...) do { __VA_ARGS__; } while (0)

/*
 * Page 9 fields 5-7, at a transmit's or receive window's early ETIME check:
 * `now` is the backend's own clock read, `at` the instant checked and `lead`
 * the air lead it must clear. Field 5 is sampled only for an arm a scheduler
 * pass is making (radiant_diag_pass_now set), and never inside the MPSL
 * signal handler: a stamp seen there belongs to whatever thread pass it
 * interrupted. A refused arm counts in field 7 and never in field 6, so field
 * 6 is never negative.
 */
static void nrf_diag_early(radiant_time_t now, radiant_time_t at, uint32_t lead)
{
	const radiant_time_t floor = now + (radiant_time_t)lead;
	const uint32_t stamp = radiant_diag_pass_now;

	if (stamp != 0u && !gate_in_signal()) {
		/* Low 32 bits of the radio clock: a pass-to-arm span is far
		 * below the 71-minute wrap, so the unsigned difference is it. */
		const uint32_t us = (uint32_t)now - stamp;

		if (us > nrf_diag.pass_to_arm_max_us) {
			nrf_diag.pass_to_arm_max_us = us;
		}
	}
	if (at < floor) {
		nrf_diag.early_etime++;
	} else {
		const uint64_t slack = at - floor;
		const uint32_t s32 = (slack > RADIANT_DIAG_NO_SAMPLE - 1u)
					     ? RADIANT_DIAG_NO_SAMPLE - 1u : (uint32_t)slack;

		if (s32 < nrf_diag.arm_slack_min_us) {
			nrf_diag.arm_slack_min_us = s32;
		}
	}
}
#else
#define NRF_DIAG(...) do { } while (0)
#endif

/*
 * Bench diagnostic for the MPSL soak, logged by the gate's 5 s stats line
 * (radiant_radio_nrf_gate_mpsl.c, "ARMDBG"): [0] a FAILED still owed for an
 * in-grant programming failure, cleared by an abort; [1] grants released early
 * on an in-grant ETIME; [2] transmit and [3] receive start compares written
 * behind the counter. Compiled out of every other build.
 */
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS_LOG)
uint32_t radiant_arm_dbg[4];
#define ARMDBG(i) (radiant_arm_dbg[(i)]++)
#else
#define ARMDBG(i) ((void)0)
#endif

/* Completions raised in an MPSL signal callback are queued, not run there (see core_cb_rx()). */
#if defined(CONFIG_RADIANT_SWI)
#define CORE_CB_DEFERRED 1
#endif

LOG_MODULE_REGISTER(radiant_radio_nrf, CONFIG_RADIANT_LOG_LEVEL);

/* Direct RADIO access is only safe if this backend owns the peripheral; the gate decides (radiant_radio_nrf_gate.h). */
BUILD_ASSERT(IS_ENABLED(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL) ||
		     !IS_ENABLED(CONFIG_BT),
	     "radiant_radio_nrf owns the RADIO outright unless it is arbitrated; "
	     "with CONFIG_BT on, select "
	     "CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL.");

/*
 * The nRF 802.15.4 driver claims PUBLISH_ADDRESS and SUBSCRIBE_RXEN at its
 * init and never releases them. An event publishes to one channel only, so
 * the direct backend's nrfx_gppi_conn_alloc() fails with -EINVAL at init. The
 * arbiter swaps the endpoints per grant instead. The check keys on OpenThread,
 * which is what applications select; the driver symbol is selected implicitly.
 */
BUILD_ASSERT(IS_ENABLED(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL) ||
		     !IS_ENABLED(CONFIG_NET_L2_OPENTHREAD),
	     "radiant_radio_nrf cannot share the RADIO with the 802.15.4 driver "
	     "unarbitrated - that driver holds PUBLISH_ADDRESS and "
	     "SUBSCRIBE_RXEN from its own init. With CONFIG_NET_L2_OPENTHREAD "
	     "on, select CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL.");

/* mpsl.rst requires the RADIO interrupt at MPSL_HIGH_IRQ_PRIORITY (0) with the timeslot API. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL) ||
		     CONFIG_RADIANT_BACKEND_NRF_IRQ_PRIO == 0,
	     "MPSL requires the application's RADIO interrupt at priority 0 "
	     "(MPSL_HIGH_IRQ_PRIORITY)");

/* ---- The timebase: one free-running 1 MHz TIMER ---- */

/*
 * The TIMER comes from devicetree: TIMER nodes default to disabled, and a
 * second user of the same TIMER becomes a devicetree conflict.
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
 * Five compares. An event publishes to one DPPI channel only, so RX-start and
 * TX-start cannot share a connection. Detach/reattach per arm was rejected:
 * the arm path runs in interrupt context with an 80 us budget.
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
 * Extends the 32-bit us counter (wraps every 71.6 min) to 64 bits. Exact if a
 * fold runs at least once per wrap. A capture and its fold must be one
 * critical section against every context that folds. On an arbitrated build
 * that includes the zero-latency MPSL callbacks, which irq_lock() does not
 * mask: a fold in between would make `counter - last` wrap by 71.6 min. So
 * CLOCK_LOCK is PRIMASK there (under 1 us). The direct build uses irq_lock,
 * and the debug counter below is compiled out.
 */
static struct {
	uint64_t base;   /* microseconds accounted for up to `last` */
	uint32_t last;   /* the counter value `base` was folded at */
} radiant_clock;

#if defined(CORE_CB_DEFERRED)
#define CLOCK_LOCK()    radiant_swi_zli_lock()
#define CLOCK_UNLOCK(k) radiant_swi_zli_unlock(k)
/* Backwards folds: a race with another context. Should stay zero. */
static volatile uint32_t radiant_dbg_clock_back;
#else
#define CLOCK_LOCK()    irq_lock()
#define CLOCK_UNLOCK(k) irq_unlock(k)
#endif

/*
 * CONFIG_RADIANT_NRF_LOW_POWER (power plan Track B): radiant_radio_nrf_lp.c,
 * included at the end of this file, stops this TIMER and releases the HFXO in
 * long idle gaps and is the build's gate. On the MPSL build (pass B6) it is not
 * the gate: the MPSL gate stops and restarts this TIMER around each timeslot
 * through radiant_nrf_gate_lp_slot_start()/_end(), and the boot HFXO request
 * below is dropped once the option is running (MPSL starts the crystal for
 * every timeslot). LP_HOOK() marks the few places this file tells it
 * something; with the option off they compile to nothing.
 */
#if defined(CONFIG_RADIANT_NRF_LOW_POWER)
static uint64_t lp_now_locked(void);
static bool lp_timer_stopped(void);
static void lp_staged(void);
static void lp_terminal_done(void);
static void lp_on_enable(void);
static int lp_hfxo_boot(void);
static void lp_init(void);
static void lp_constlat(bool on);
#define LP_HOOK(...) do { __VA_ARGS__; } while (0)
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
/*
 * Pass B6: AN ARM PREEMPTED BY THE TIMESLOT'S END. An arm served inside a held
 * grant (a completion's follow-on, from the software interrupt) can be
 * preempted by the zero-latency ACTION_END - which hands the RADIO back to the
 * other stack and stops this TIMER (radiant_nrf_gate_lp_slot_end()) - or by
 * the next START. Resumed, it would write MODE, FREQUENCY, PACKETPTR, SHORTS
 * and compares into a RADIO that is no longer ours, from a TIMER that no longer
 * runs. So (LP_SLOT_GUARD, only with the option on the MPSL build):
 *
 *   - the claim records the timeslot generation it was made in (lp_arm_gen,
 *     lp_slot_gen(): 0 while the TIMER is stopped) and that an arm is in
 *     progress (lp_arming); radiant_radio_nrf_gate_on_grant() records the
 *     generation of the START it programmes from;
 *   - program_tx/rx/ed run their whole register span under the zero-latency
 *     lock (PRIMASK), and start it by checking that the generation is still
 *     the claim's: if a timeslot ended (or another began) since, nothing is
 *     written and the arm fails ETIME. Inside the span nothing can end the
 *     timeslot, so every RADIO write, compare and connection enable belongs
 *     to the timeslot that is running, and start_compare_missed() reads the
 *     running TIMER. The span has no loop but the filter writes (at most
 *     caps.max_filters) and the body copy (at most RADIANT_RADIO_BODY_MAX
 *     bytes): some tens of us at the nRF54L15's 128 MHz, an estimate, not a
 *     measurement. It can only delay this timeslot's own TIMER0 end signal,
 *     which fires END_MARGIN_FIXED_US (750 us) or END_MARGIN_EXTEND_US before
 *     the grant expires; a span that finds its timeslot gone leaves at once.
 *   - radiant_radio_abort() from the zero-latency signal (EXTEND_FAILED, the
 *     extension skip), and the gate's deferred denial, leave an arm in
 *     progress to that arm, which then fails ETIME at its span: one outcome
 *     for the operation, not ETIME plus ABORTED or DENIED. The mark is cleared
 *     only once the span programmed, or after a failed arm has freed the slot.
 *   - Known cost, accepted: an arm claimed while the TIMER is stopped records
 *     generation 0, so if a START lands between the claim and a GRANTED
 *     answer the span refuses it (ETIME: the scheduler's MISSED, one slot).
 *     Taking the generation after GRANTED instead would reopen the race the
 *     generation exists to close.
 */
#define LP_SLOT_GUARD 1
static uint32_t lp_slot_gen(void);
static uint32_t lp_arm_gen;
static volatile bool lp_arming;
#define LP_ARM_CLAIM() do { lp_arm_gen = lp_slot_gen(); lp_arming = true; } while (0)
#define LP_ARM_DONE()  do { lp_arming = false; } while (0)

/* Open a programme's span: the zero-latency lock, and whether the timeslot
 * the arm was claimed in is still the one running. */
static inline uint32_t lp_span_begin(bool *ok)
{
	uint32_t key = radiant_swi_zli_lock();
	uint32_t gen = lp_slot_gen();

	*ok = gen != 0u && gen == lp_arm_gen;
	return key;
}

/* Close a span that programmed the operation: from here an abort from the
 * signal is a normal abort of a live operation. */
static inline void lp_span_end(uint32_t key)
{
	lp_arming = false;
	radiant_swi_zli_unlock(key);
}

/* Close a span that failed: the arm stays marked as arming until its caller
 * has given the slot back (`fail:`, OP_NONE first), so an abort from the
 * signal in between still leaves it the arm's ETIME. */
static inline void lp_span_fail(uint32_t key)
{
	radiant_swi_zli_unlock(key);
}
#endif
#else
#define LP_HOOK(...) do { } while (0)
#endif
#if defined(LP_SLOT_GUARD)
#define LP_GUARD_ONLY(...) do { __VA_ARGS__; } while (0)
#else
#define LP_GUARD_ONLY(...) do { } while (0)
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
#if defined(CONFIG_RADIANT_NRF_LOW_POWER)
	/* Piecewise: the TIMER awake, the LF counter asleep, clamped monotonic. */
	uint64_t now = lp_now_locked();
#else
	uint64_t now = clock_fold(timer_capture(CC_NOW));
#endif

	CLOCK_UNLOCK(key);
	return (radiant_time_t)now;
}

/*
 * Absolute time of a counter value captured shortly before the last fold. The
 * difference is signed, since an ADDRESS capture can precede the interrupt that
 * reads it. Valid within +-35 min of the last fold.
 */
static radiant_time_t clock_absolute(uint32_t counter)
{
	unsigned int key = CLOCK_LOCK();
#if defined(LP_SLOT_GUARD)
	/* A stopped TIMER is never folded: the result is meaningless (the
	 * caller's timeslot is over) but the fold is left intact. */
	uint64_t base = lp_timer_stopped() ? lp_now_locked()
					   : clock_fold(timer_capture(CC_NOW));
#else
	uint64_t base = clock_fold(timer_capture(CC_NOW));
#endif
	int32_t  delta = (int32_t)(counter - radiant_clock.last);

	CLOCK_UNLOCK(key);
	return (radiant_time_t)((int64_t)base + delta);
}

/* Counter value for an absolute time. Only the low 32 bits reach the compare. */
static uint32_t clock_counter_at(radiant_time_t t)
{
	unsigned int key = CLOCK_LOCK();
#if defined(LP_SLOT_GUARD)
	/* As clock_absolute(). (A programme's own reads are inside its span,
	 * where the TIMER cannot stop; this covers any other caller.) */
	uint64_t base = lp_timer_stopped() ? lp_now_locked()
					   : clock_fold(timer_capture(CC_NOW));
#else
	uint64_t base = clock_fold(timer_capture(CC_NOW));
#endif
	uint32_t last = radiant_clock.last;

	CLOCK_UNLOCK(key);
	return last + (uint32_t)((int64_t)t - (int64_t)base);
}

/* ---- Capabilities ---- */

/*
 * Coded PHY support needs two conditions, kept apart. The part must have it
 * (RADIO_MODE_MODE_Ble_LR125Kbit, the inner test, so a part without it falls
 * back to 1 M, ADR 0007). The image must want it (CONFIG_RADIANT_PHY_LR_CODED,
 * off by default): the coded air lead is charged to min_arm_lead_us on every
 * window, and that lead is the scheduler's exclusion radius. A 1 M-only ANT+
 * image would pay 288 us for a PHY it never emits, about 1 % of slots per
 * channel at eight channels. Every coded site keys on this macro, so the LR
 * path leaves the image as a whole.
 */
#if defined(RADIO_MODE_MODE_Ble_LR125Kbit) && defined(CONFIG_RADIANT_PHY_LR_CODED)
#define RADIANT_NRF_HAS_LR 1
#else
#define RADIANT_NRF_HAS_LR 0
#endif

/* Advertised PHYs, most preferred first. phys[0] must be 1 M: ANT+ channels are 1 M on RF 57 (ADR 0007). */
static const enum radiant_phy radiant_nrf_phys[] = {
	RADIANT_PHY_1M_GFSK,
#if RADIANT_NRF_HAS_LR
	RADIANT_PHY_LR_CODED,
#endif
};

/* Airtime: at 1 Mbit GFSK one byte is exactly 8 us. */
#define US_PER_BYTE            8u
#define PREAMBLE_BYTES         1u    /* PCNF0.PLEN = 8bit; confirmed by Spike A */

/*
 * LE Coded at S=8 (Bluetooth spec; FEC block 1 is always S=8): preamble 80 us +
 * access address 256 us + coding indicator 16 us + TERM1 24 us = 376 us.
 * t_sync is the END of the access address, 336 us in. Using the 376 us total
 * would put every transmit and window 40 us off.
 */
#define LR_PREAMBLE_US         80u
#define LR_ACCESS_ADDR_US      256u
#define LR_CI_US               16u
#define LR_TERM1_US            24u
#define LR_FEC1_US             (LR_PREAMBLE_US + LR_ACCESS_ADDR_US + \
				LR_CI_US + LR_TERM1_US)
/* Switch-on to t_sync, air only. Ramp-up is added by air_lead_us(). */
#define LR_AIR_LEAD_US         (LR_PREAMBLE_US + LR_ACCESS_ADDR_US)

/*
 * Ramp-up and turnaround are datasheet figures, not measurements. A wrong value
 * moves the frame on air by that amount without failing loudly.
 *
 * Fast ramp-up is not the nRF52 reset default (MODECNF0.RU resets to ~140 us,
 * which would put every nRF52840 frame ~100 us late). radiant_radio_init()
 * enables it, which is what makes 40 us true. Zephyr's BLE controller tables
 * agree: 1 Mbit ramp-up is 40 us fast, 140 us default.
 */
/*
 * Select on CONFIG_SOC_COMPATIBLE_*: Zephyr 4.4 / NCS v3.4.0 renamed the
 * SOC_SERIES_* symbols, and a stale dependency would silently skip this file.
 */
#if defined(CONFIG_SOC_COMPATIBLE_NRF54LX)
#define RAMP_UP_US             40u
#define RX_TO_TX_US            40u
#define TX_TO_RX_US            40u
/*
 * Software cost of an arm call. Not measured: no RADIANT_RADIO_ETIME refusal
 * has been seen, which bounds it from below only. Nordic's 802.15.4 driver
 * budgets 185/150 us TX/RX on this series (nrf_802154_delayed_trx.c).
 */
#define ARM_SETUP_US           80u
#define CAPS_NAME              "nrf54l RADIO (direct)"
/* nRF54L splits RADIO's interrupt over two lines; RADIO_0 carries the packet events. */
#define RADIANT_RADIO_IRQn     RADIO_0_IRQn
#define RADIANT_RADIO_IRQn_2   RADIO_1_IRQn
#elif defined(CONFIG_SOC_COMPATIBLE_NRF53X)
/*
 * nRF5340 network core only. cpuapp has no radio node, so a cpuapp build fails
 * at RADIANT_TIMER_NODE before these constants matter. Fast ramp-up works as on
 * nRF52 (MODECNF0.RU, enabled in radiant_radio_init()).
 */
#define RAMP_UP_US             40u   /* fast ramp-up, enabled at init */
#define RX_TO_TX_US            40u
#define TX_TO_RX_US            40u
/*
 * Bracketed, and the largest of the three. The 64 MHz Cortex-M33 looks like the
 * nRF52840, but nrf_802154_delayed_trx.c rates this series slower to set up
 * (TX/RX 360/290 us against 270/270 us on nRF52). 280 us is above the nRF52840's
 * 240 and below the 290 us RX figure. It is an upper bound: tighten it by
 * watching RADIANT_RADIO_ETIME stay at zero while lowering it.
 *
 * Erring large costs 40 us of slack per window and widens the exclusion radius
 * by the same amount. Erring small wedges the board silently, with every arm
 * refused. ARM_LEAD_US is 368 us (280+40+48), which radiant_transfer.h's
 * 1310 us reply check clears with 3.5x room.
 */
#define ARM_SETUP_US           280u
#define CAPS_NAME              "nrf53 net RADIO (direct)"
/* One RADIO interrupt line on this core; only nRF54L splits the packet events. */
#define RADIANT_RADIO_IRQn     RADIO_IRQn
#elif defined(CONFIG_SOC_COMPATIBLE_NRF52X)
#define RAMP_UP_US             40u   /* fast ramp-up, enabled at init */
#define RX_TO_TX_US            40u
#define TX_TO_RX_US            40u
/*
 * Same arm code at half the clock (64 MHz against 128 MHz). The nRF54L15 figure
 * would under-budget here and refuse every arm. 240 us is above twice the
 * nRF54L15 figure (160 us) and below Nordic's 270 us for NRF52_SERIES. It is an
 * upper bound: tighten it by watching ETIME refusals stay at zero.
 */
#define ARM_SETUP_US           240u
#define CAPS_NAME              "nrf52 RADIO (direct)"
#define RADIANT_RADIO_IRQn     RADIO_IRQn
#else
#error "radiant_radio_nrf: no measured constants for this SoC series. Add them \
with the bench measurement that produced them, or select a different backend."
#endif

/*
 * Maximum transmit power differs by part. nRF52840 and nRF54L15 have the full
 * +2..+8 dBm ladder. The nRF5340 network core has no positive setting, so 0 dBm
 * is its silicon ceiling. Feature-detected on the MDK symbol, not the SoC name.
 *
 * This matters on the bench: an nRF5340 endpoint transmits up to 8 dB below an
 * nRF52840 at the same request, so loss figures are not comparable without
 * that correction. The value feeds caps.tx_power_max_dbm because the core
 * clamps requests against caps (radiant_channel.c). A caps value of 8 on a
 * 0 dBm part would report success at a power never transmitted.
 */
#if defined(RADIO_TXPOWER_TXPOWER_Pos8dBm)
#define RADIANT_NRF_TXP_MAX_DBM 8
#else
#define RADIANT_NRF_TXP_MAX_DBM 0
#endif

/*
 * ARM_SETUP_US is CPU time to program registers, wire (D)PPI and write the
 * compare before the counter reaches it. Ramp-up and preamble airtime are
 * added separately in arm_tx(), since t_sync is defined at the end of the
 * address. radiant_transfer.h checks its 1310 us reply lead against this, so
 * a too-large value fails at config time. Worst case 240+40+48 = 328 us.
 */

/*
 * min_arm_lead_us must cover setup, ramp-up, preamble and the longest address.
 * If it is too small, radiant_api.c posts windows nearer than arm_rx() accepts,
 * every arm is refused with ETIME, and nothing reaches air with no visible error.
 *
 * On the coded PHY the lead is preamble plus access address (336 us), not 48 us.
 * It does not belong in caps.phy_switch_us: that is paid only on a PHY change,
 * but this lead is owed on every coded operation, so back-to-back coded windows
 * would be under-led. The coded PHY is therefore Kconfig-gated.
 *
 * min_arm_lead_us is also the scheduler's exclusion radius: pass_step() marks a
 * window DONE_MISSED if t_end < now + arm_lead, and arm_rx_window()'s merge
 * rule 3 rescues pairs that cannot be armed in sequence. Widening it for a PHY
 * the image never emits costs multi-channel capacity that a single-channel loss
 * test cannot see.
 *
 * Coded PHY off (default): 168 us on nRF54L15 (80+40+48), 328 us on nRF52840 (240+40+48).
 */
#if RADIANT_NRF_HAS_LR
#define AIR_LEAD_WORST_US      LR_AIR_LEAD_US
#else
#define AIR_LEAD_WORST_US      ((PREAMBLE_BYTES + RADIANT_RADIO_ADDR_MAX) * US_PER_BYTE)
#endif

#define ARM_LEAD_US            (ARM_SETUP_US + RAMP_UP_US + AIR_LEAD_WORST_US)

/*
 * PHY change cost. The mode-register writes are already inside ARM_SETUP_US,
 * so the increment is near zero. It is 20 us, an upper bound on a dozen writes,
 * so the scheduler's PHY budgeting is not dead code. Coded ramp-up and air lead
 * are in ARM_LEAD_US, owed on every coded operation.
 */
#if RADIANT_NRF_HAS_LR
#define PHY_SWITCH_US          20u
#else
#define PHY_SWITCH_US          0u
#endif

/* Not const: max_window_us is filled from the gate in radiant_radio_caps_get(). */
static struct radiant_radio_caps radiant_nrf_caps = {
	.name                = CAPS_NAME,

	/* Eight logical addresses, but at most two BASEs (see header). */
	.max_filters         = 8,
	/* Logical 0 is BASE0+AP0; logical 1..7 are BASE1+AP1..AP7. */
	.max_addr_groups     = 2,
	/* ADR 0022: one wildcard filter covers every devnum_lo, via the ADR 0019 short match. */
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
	/* Cost of programming inside air already held. Equals min_arm_lead_us on the direct gate. */
	.min_arm_lead_in_grant_us = ARM_LEAD_US,
	/*
	 * Zero: program_rx() already adds the longest frame's body and CRC tail to
	 * the close compare. On nRF54L15 the terminal arrives 149 us after t_close:
	 * 112 us of tail and ~37 us of DISABLED->ISR->callback path. The latter is
	 * not receiver hold time, so it is not declared here.
	 */
	.rx_close_hold_us    = 0,

	.time_resolution_ns  = 1000,   /* the 1 MHz TIMER */

	/* t_sync is a (D)PPI capture of ADDRESS, with no software in the path. Its calibration is a prior (T_SYNC_CAL_US). */
	.has_sync_timestamp  = true,
	.has_rssi            = true,
	/* Follows the Kconfig: without CONFIG_RADIANT_ED_SCAN this build has no sampling path. */
	.has_ed_scan         = IS_ENABLED(CONFIG_RADIANT_ED_SCAN),
	.crc_in_hw           = true,
	/* RXCRC holds the received CRC, not just pass/fail, so the core can repair one flipped bit. */
	.has_rx_crc          = true,

	.tx_power_min_dbm    = -40,
	/* See RADIANT_NRF_TXP_MAX_DBM. */
	.tx_power_max_dbm    = RADIANT_NRF_TXP_MAX_DBM,
};

const struct radiant_radio_caps *radiant_radio_caps_get(void)
{
	/*
	 * max_window_us comes from the gate: unbounded when radiant owns the RADIO,
	 * MPSL_TIMESLOT_LENGTH_MAX_US under MPSL. Set on every call, because the
	 * scheduler reads capabilities before init and must see the ceiling then too.
	 */
	radiant_nrf_caps.max_window_us = gate_max_window_us();
	/*
	 * Two leads, only the first moves. min_arm_lead_us is what the scheduler
	 * arms against. Under an arbiter it must also cover placing a reservation,
	 * so the gate's lead is added: reserve first, then program inside the grant.
	 * Taking the max would leave the hardware lead unaccounted for and refuse
	 * every arm. min_arm_lead_in_grant_us never moves; radiant_burst.c reads it.
	 */
	radiant_nrf_caps.min_arm_lead_us =
		(uint16_t)(ARM_LEAD_US + gate_min_arm_lead_us());
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
	/* ADR 0018 (d2): true once the gate has its second session. Kept current by radiant_nrf_gate_set_elastic_lead(). */
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
 * t_sync is the instant the last address bit reached the antenna. EVENTS_ADDRESS
 * differs from that by demodulator delay, filter latency and capture offset: a
 * per-part constant, properly measured with a GPIO pulse on one board captured
 * on this TIMER on another.
 *
 * Seeded, not measured: Zephyr's BLE controller chain delays (nrf5 radio, same
 * for nRF52840 and nRF54L15 at 1 Mbit): RX 9400 ns, TX 600 ns. These are BLE
 * 1M figures (250 kHz deviation); ANT is ~170 kHz, so RX may differ. Zero would
 * be worse: a constant error cancels out of the period estimate, so windows sit
 * off-centre with no alarm. TX is the cheap one to confirm, since
 * radiant_dbg_tx_err read -2 us on nRF54L15 with both constants at zero.
 */
#define T_SYNC_CAL_US   (-9)   /* -9.4 us, rounded toward zero */

/*
 * Coded PHY RX delay is 29.6 us at S=8 against 9.4 us at 1 M (a distinct figure,
 * not a rounding). Reusing the 1 M value would put every coded window 20 us off
 * centre. TX has no separate constant: the chain delay is 0.6 us for 1 M, S=2 and
 * S=8 alike. Seeded like T_SYNC_CAL_US (ADR 0007).
 */
#define T_SYNC_CAL_LR_US   (-30)  /* -29.6 us, rounded away from zero */

/* Receive calibration for a PHY. Transmit uses T_SYNC_CAL_TX_US. */
static int32_t t_sync_cal_rx_us(enum radiant_phy phy)
{
	return (phy == RADIANT_PHY_LR_CODED) ? T_SYNC_CAL_LR_US : T_SYNC_CAL_US;
}

/* Separate from RX on purpose: RX error is demodulator delay, TX is modulator/PA delay. */
#define T_SYNC_CAL_TX_US   (1)   /* +0.6 us, rounded away from zero */

/* radiant_radio_tx() folds this into an unsigned lead; a negative value would wrap to ~4295 s. */
BUILD_ASSERT(T_SYNC_CAL_TX_US >= 0,
	     "T_SYNC_CAL_TX_US is folded into an unsigned lead in "
	     "radiant_radio_tx(); a negative value wraps");

/* Signed calibration on an unsigned time. Spelled out, not cast, and clamps at zero. */
static radiant_time_t t_sync_cal(radiant_time_t captured, int32_t cal_us)
{
	if (cal_us >= 0) {
		return captured + (radiant_time_t)cal_us;
	}
	return (captured > (radiant_time_t)(-cal_us))
		       ? (captured - (radiant_time_t)(-cal_us))
		       : 0u;
}

/* ---- Address arithmetic (measured) ---- */

/* Bit-reverse a byte: addresses go on air LSb-first whatever PCNF1.ENDIAN says (Spike A). */
static uint8_t rev8(uint8_t b)
{
	b = (uint8_t)(((b & 0xF0u) >> 4) | ((b & 0x0Fu) << 4));
	b = (uint8_t)(((b & 0xCCu) >> 2) | ((b & 0x33u) << 2));
	b = (uint8_t)(((b & 0xAAu) >> 1) | ((b & 0x55u) << 1));
	return b;
}

/*
 * Split an on-air address into BASE and PREFIX. addr[0] goes out first and the
 * prefix is the last byte. For BALEN < 4 the used bytes sit in the HIGH end:
 *
 *     BASE = (lsb-first packing of the base bytes) << (8 * (4 - BALEN))
 *
 * One expression covers both the 5-byte and 3-byte lengths (Spike A phases A and C).
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

/* ---- Operation state: one operation in flight (HAL contract) ---- */

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
	 * Address bytes the hardware match leaves to software (ADR 0019). Zero except
	 * for a TRACKING window under a short CONFIG_RADIANT_NRF_TRACK_HW_ADDR_LEN.
	 * The HAL keeps the filters alive until the terminal event, so the ISR can
	 * finish the match.
	 */
	uint8_t      sw_addr_len;
	const struct radiant_rx_filter *filters;
	/* CRC seed for the software check in radiant_addr_split_repair(). */
	uint16_t     crc_init;
	/*
	 * Static length: the length of every frame. Length-from-body: the format's
	 * ceiling, to which a frame's length byte is clamped. len_from_body says which.
	 */
	uint8_t      body_len;
	bool         len_from_body;
	/*
	 * The PHY the operation was armed on. It selects the t_sync calibration
	 * (T_SYNC_CAL_LR_US). Latched here so the ISR does no register reads and
	 * cannot disagree with what was programmed.
	 */
	enum radiant_phy phy;
	bool         stop_on_first;
	bool         report_crc_fail;
	/*
	 * The one terminal event has been sent (see CLAIM_TERMINAL()). Cleared at arm
	 * time under the claiming lock, not in program_*(): under an arbiter an armed
	 * operation waits for air with nothing programmed, and clearing late would
	 * inherit the previous operation's true and swallow a denied grant's terminal.
	 */
	bool         terminal_sent;

	/*
	 * A receive window delivered a frame. Used only for the noise floor: an empty
	 * window measures the band, otherwise RSSISAMPLE holds the packet's level.
	 */
	bool         rx_any;

	/*
	 * An ADDRESS the END branch cleared, or found already consumed, that
	 * belongs to a frame still to END: the late-ISR guard's carry. The END
	 * branch accepts it in place of EVENTS_ADDRESS for the next END, so that
	 * frame is not discarded as "not a reception". Cleared at arm time and
	 * consumed by the END it covers. See the case list in radio_isr().
	 */
	bool         addr_carry;

	/*
	 * The t_sync a transmit asked for, compared with the captured one to measure
	 * TX timing error. A fixed lateness is invisible on broadcast but fatal for
	 * acknowledged data, where the peer's window opens 1560 us after the packet.
	 */
	radiant_time_t t_sync_req;

#if defined(CONFIG_RADIANT_ED_SCAN)
	/*
	 * Energy-detect sweep in flight. ed_deadline_cc is a raw counter value, not a
	 * radiant_time_t: the sampling loop compares it in the ISR, and clock_absolute()
	 * takes a lock on every call. Valid within 35 min of the last fold. It advances
	 * one dwell per index, bounding the sweep to n * dwell. There is no ed_lo: the
	 * scheduler splits any range that would wrap.
	 */
	uint8_t  ed_hi;
	uint8_t  ed_cur;
	uint32_t ed_dwell_us;
	uint32_t ed_deadline_cc;
#endif

	/*
	 * (D)PPI connections, allocated once at init. nRF54L may place RADIO and TIMER
	 * in different DPPI domains, so nrfx connections are used. Per-pair allocation
	 * rules out an index slip wiring an edge to the wrong task.
	 */
	nrfx_gppi_handle_t conn_sync;
	nrfx_gppi_handle_t conn_start;    /* CC_START -> TASKS_RXEN */
	nrfx_gppi_handle_t conn_start_tx; /* CC_START -> TASKS_TXEN */
	nrfx_gppi_handle_t conn_close;
	/*
	 * RXREADY -> TASKS_RSSISTART: one RSSI sample as soon as the receiver is live,
	 * so a quiet window (whose level is the noise floor) is sampled too. The
	 * ADDRESS short only samples on arrival. One-shot, so a received frame
	 * overwrites this sample anyway.
	 */
	nrfx_gppi_handle_t conn_rssi;
	bool               conn_ok;
	/* Grant arrived but programming failed. The terminal goes out later, from thread context (radiant_nrf_gate_on_grant()). */
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
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
	/* 0xF7 page 7: search window, and when its CC_CLOSE fires (tail included). */
	bool               page7_search;
	radiant_time_t     page7_close_at;
#endif
} radiant_op;

/*
 * Register images an arm call computes when it validates a request, so the
 * request is validated once and programmed from the result (format_prepare(),
 * filters_prepare()). All uint32_t, so memcmp() compares them without padding.
 */
struct nrf_fmt_regs {
	uint32_t mode;
	uint32_t frequency;
	uint32_t pcnf0;
	uint32_t pcnf1;
	uint32_t crccnf;
	uint32_t crcinit;
};

struct nrf_filt_regs {
	uint32_t base0;
	uint32_t base1;
	uint32_t prefix0;
	uint32_t prefix1;
	uint32_t rxaddresses;
};

/* Everything a receive window programs that depends on its format and filters. */
struct nrf_rx_prep {
	struct nrf_fmt_regs  fmt;
	struct nrf_filt_regs filt;
	/* radiant_filter_slot[] for this filter set (logical address -> filters[] index). */
	uint8_t              slot[8];
	/* rx_sw_addr_len() for this request. */
	uint8_t              sw_addr_len;
};

/*
 * The request an arm call validated but has not programmed yet. Copied, not
 * pointed to: arm_rx_window()'s stack local is gone before an arbitrated grant
 * arrives. The tx body pointer keeps the HAL lifetime; the rest is copied.
 * One request is live at a time (HAL rule; MPSL rejects a second with -NRF_EAGAIN).
 * The register images the arm computed travel with it, so a grant programs
 * exactly what was validated. So do the format and filter CONTENTS: rx.fmt,
 * rx.filters and tx.fmt are repointed at the copies below, so the radiant_op
 * fields program_rx()/program_tx() fill at the grant, and the ISR's split match
 * (radiant_op.filters), see the arm-time request even if the caller has since
 * rewritten its arrays. Copied on the deferred path only: a direct arm programs
 * in the same call. Costs 56 bytes of filters plus two formats of RAM.
 */
static struct {
	struct radiant_rx_req     rx;
	struct nrf_rx_prep        rx_prep;
	struct radiant_pkt_format rx_fmt;
	struct radiant_rx_filter  rx_filters[8];
	struct radiant_tx_req     tx;
	struct nrf_fmt_regs       tx_regs;
	struct radiant_pkt_format tx_fmt;
#if defined(CONFIG_RADIANT_ED_SCAN)
	struct radiant_ed_req ed;
#endif
	/* True from a GATE_PENDING acquire until its grant or denial. Lets gate callbacks reject late signals. */
	bool pending;
} radiant_staged;

/* Debug counters: logging from the RADIO ISR is unsafe, so the ISR counts and thread context reports. */
static volatile uint32_t radiant_dbg_isr;
static volatile uint32_t radiant_dbg_rx_ev;
static volatile uint32_t radiant_dbg_term;
static volatile uint32_t radiant_dbg_ok;
/* ENDs with no address match (replays of the previous frame, suppressed in radio_isr()). A few per window teardown is normal; one per channel period is a defect. */
static volatile uint32_t radiant_dbg_rx_no_addr;
/* Achieved t_sync minus requested t_sync on the last transmit, microseconds. */
static volatile int32_t  radiant_dbg_tx_err;
/* Receive windows refused because CC_START landed behind the counter. Expected 0 or single digits. */
static volatile uint32_t radiant_dbg_start_late;

/*
 * RADIO DMA buffer. Never read after the END branch's snapshot: with END->START
 * the receiver writes the next frame here as soon as its ADDRESS matches.
 */
static uint8_t radiant_rx_buf[RADIANT_RADIO_BODY_MAX + 4] __aligned(4);

/*
 * The END branch's copy of radiant_rx_buf (software address bytes plus body),
 * taken between two CC_SYNC reads; see radio_isr(). Everything downstream of
 * the snapshot reads this, and it is what rx_event.body points at (or into)
 * for the one callback. Only the END branch writes it, and that branch does
 * not nest, so it holds still for a direct callback; MPSL builds copy it
 * again in core_cb_rx().
 */
static uint8_t radiant_rx_snap[sizeof(radiant_rx_buf)] __aligned(4);

/* Transmit buffer. Copying the body costs little against the 80 us setup budget and guarantees EasyDMA-reachable, aligned memory. */
static uint8_t radiant_tx_buf[RADIANT_RADIO_BODY_MAX + 4] __aligned(4);

/* Frame rebuilt by radiant_addr_split_repair() (ADR 0019). Its bits are shifted by one, so it cannot be used in place. */
static uint8_t radiant_repair_buf[RADIANT_RADIO_BODY_MAX];
/* Frames that reached the core only because the slip repair recovered them. */
static volatile uint32_t radiant_dbg_slip_repaired;

#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
/*
 * CONFIG_RADIANT_NRF_RX_DIAG: what each tracked receive window saw. Written by
 * the radio interrupt and its terminal path, read by a 10 s work item. Torn
 * counter reads are acceptable for a bench readout.
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

/* One outcome per tracked window, best first. Called once per window, from the stop-on-first terminal or deliver_terminal(). */
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

/* `frame` is the END branch's snapshot, never the live DMA buffer. */
static void rx_diag_crc_fail(uint8_t logical, uint32_t rxcrc, int8_t rssi,
			     const uint8_t *frame)
{
	uint32_t slot = rx_diag.ring_w % RX_DIAG_RING;
	/* Includes the software-matched address bytes (ADR 0019). */
	uint8_t n = MIN((uint8_t)(radiant_op.body_len + radiant_op.sw_addr_len),
			(uint8_t)RX_DIAG_BODY_MAX);

	rx_diag.ring[slot].t_ms = k_uptime_get_32();
	rx_diag.ring[slot].rxcrc = rxcrc;
	rx_diag.ring[slot].rxmatch = logical;
	rx_diag.ring[slot].rssi = rssi;
	rx_diag.ring[slot].len = n;
	memcpy(rx_diag.ring[slot].body, frame, n);
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

	/* Entries overwritten before they were printed are reported as a count. */
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
		/* Bit 31: software compare rejected, CRC passed. Bit 30: rejected, CRC failed. Neither: accepted, CRC failed. */
		LOG_INF("rxdiag %s t=%u rxmatch=%u rssi=%d rxcrc=%04x body=%s",
			(rxcrc & 0x80000000u) ? "foreign_crcok" :
			(rxcrc & 0x40000000u) ? "foreign_crcbad" : "crcfail",
			t_ms, rxm, rssi, (unsigned)(rxcrc & 0xFFFFFFu), hex);
		rx_diag.ring_r++;
	}
	k_work_reschedule(&rx_diag_work, K_SECONDS(10));
}
#endif /* CONFIG_RADIANT_NRF_RX_DIAG */

/* ---- Register programming ---- */

/*
 * Address bytes a receive window leaves to software (ADR 0019). Zero except for
 * TRACKING on 1 M, and zero for every transmit: the transmitter always sends the
 * full address, so a receiver can match any prefix of it.
 */
#if defined(CONFIG_RADIANT_NRF_SLIP_GUARD)
#define RX_SLIP_GUARD_RUN CONFIG_RADIANT_NRF_SLIP_GUARD_RUN
#else
#define RX_SLIP_GUARD_RUN 0
#endif

/* Per window: SLIP_GUARD shortens the match to two bytes if any filter's address is one the demodulator slips on. */
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
 * sw_addr_len: trailing address bytes the hardware receives as payload instead
 * of matching (always 0 on transmit). BALEN shrinks by that count and STATLEN
 * grows by it, so the bit stream and its CRC are unchanged.
 */
static int format_prepare(const struct radiant_pkt_format *fmt,
			  uint8_t rf_index, uint8_t sw_addr_len,
			  struct nrf_fmt_regs *out)
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
	 * No coded mode on this part, or the image did not ask for it (see
	 * RADIANT_NRF_HAS_LR). Refused, never approximated: a 1 M frame is not
	 * "close" to a coded one. lr is pinned false, so every `if (lr)` below
	 * folds at compile time.
	 */
	if (fmt->phy == RADIANT_PHY_LR_CODED) {
		return RADIANT_RADIO_ENOTSUP;
	}
	lr = false;
#endif
	if (fmt->addr_len < 2u || fmt->addr_len > 5u) {
		return RADIANT_RADIO_ENOTSUP;
	}
	/* Hardware needs a base byte and a prefix byte. The coded PHY never leaves bytes to software. */
	if (sw_addr_len != 0u &&
	    (lr || (uint32_t)fmt->addr_len < (uint32_t)sw_addr_len + 2u)) {
		return RADIANT_RADIO_EINVAL;
	}
	/*
	 * The coded PHY's access address is a fixed 32 bits, so four address bytes
	 * is the only length the hardware emits. Anything else is refused rather
	 * than padded or truncated, which would put on air a frame nobody matches.
	 */
	if (lr && fmt->addr_len != 4u) {
		return RADIANT_RADIO_ENOTSUP;
	}
	if (rf_index > RADIANT_RF_INDEX_MAX) {
		return RADIANT_RADIO_EINVAL;
	}

	/*
	 * Static length on 1 M (Spike B): byte 3 of an ANT frame is a control byte,
	 * not a length. Length decoding would silently truncate acknowledged and
	 * burst frames, so it is refused on 1 M. The refusal is per PHY: the coded
	 * PHY has no such byte, so LFLEN=8 is correct there.
	 */
	if (lr) {
		if (fmt->len_mode != RADIANT_LEN_FROM_BODY) {
			return RADIANT_RADIO_ENOTSUP;
		}
		/*
		 * The RADIO's LENGTH field counts the bytes after itself, so the HAL body
		 * is the length byte plus those. Any other offset or bias would need a
		 * translation layer between the core and EasyDMA.
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
	 * CRC-16/CCITT-FALSE, every parameter checked. The RADIO CRC engine has no
	 * reflection or final XOR, so a format asking for either is refused.
	 */
	if (fmt->crc.width_bits != 16u || fmt->crc.poly != 0x1021u ||
	    fmt->crc.xor_out != 0u || fmt->crc.reflect_in || fmt->crc.reflect_out) {
		return RADIANT_RADIO_ENOTSUP;
	}

	/*
	 * Everything above refuses; nothing below can. The whole function is a pure
	 * function of its arguments and touches no register: it computes what
	 * format_write() puts in the peripheral. An arm call runs it once, before the
	 * gate is asked, so EINVAL/ENOTSUP are answered synchronously, and programs
	 * from its result. One function, not a check plus a separate encoder, so the
	 * refusals cannot drift away from what is written.
	 */
#if RADIANT_NRF_HAS_LR
	if (lr) {
		/*
		 * S=8 only. FEC block 1 is S=8 at any data rate, so S=2 would save only
		 * ~2.1x, not 4x, for ~3 dB. One rate keeps one format, one phy_switch_us
		 * and one t_sync calibration (ADR 0007). LR125Kbit RX also accepts 500 kbit/s.
		 */
		out->mode = (uint32_t)NRF_RADIO_MODE_BLE_LR125KBIT;
	} else
#endif
	{
		out->mode = (uint32_t)NRF_RADIO_MODE_NRF_1MBIT;
	}
	out->frequency = 2400u + (uint32_t)rf_index;

	if (lr) {
		/*
		 * Every PCNF0 field is required. PLEN=LongRange is the 80 us coded preamble.
		 * CILEN=2 is the coding indicator; zero there is an uninterpretable frame,
		 * not "no indicator". TERMLEN=3 is TERM1, also silently fatal at zero.
		 * LFLEN=8 is the length field. S0LEN and S1LEN are 0, so the first body byte
		 * on air is the length, which is why only len_offset 0 and len_bias 1 are accepted.
		 */
		out->pcnf0 =
			((uint32_t)RADIO_PCNF0_PLEN_LongRange
				<< RADIO_PCNF0_PLEN_Pos) |
			((uint32_t)2u << RADIO_PCNF0_CILEN_Pos) |
			((uint32_t)3u << RADIO_PCNF0_TERMLEN_Pos) |
			((uint32_t)8u << RADIO_PCNF0_LFLEN_Pos);
		/*
		 * STATLEN = 0: the length field alone sets how many bytes leave the antenna.
		 * MAXLEN is the format's max_body_len minus the length byte, not
		 * sizeof(radiant_rx_buf), so a corrupted length cannot overfill the buffer.
		 */
		out->pcnf1 =
			((uint32_t)(fmt->max_body_len - 1u)
				<< RADIO_PCNF1_MAXLEN_Pos) |
			((uint32_t)0u                   << RADIO_PCNF1_STATLEN_Pos) |
			((uint32_t)(fmt->addr_len - 1u) << RADIO_PCNF1_BALEN_Pos) |
			(RADIO_PCNF1_ENDIAN_Big         << RADIO_PCNF1_ENDIAN_Pos) |
			(RADIO_PCNF1_WHITEEN_Disabled   << RADIO_PCNF1_WHITEEN_Pos);
	} else {
		out->pcnf0 = 0u;
		out->pcnf1 =
			((uint32_t)sizeof(radiant_rx_buf) << RADIO_PCNF1_MAXLEN_Pos) |
			((uint32_t)(fmt->body_len + sw_addr_len)
				<< RADIO_PCNF1_STATLEN_Pos) |
			((uint32_t)(fmt->addr_len - sw_addr_len - 1u)
				<< RADIO_PCNF1_BALEN_Pos) |
			(RADIO_PCNF1_ENDIAN_Big           << RADIO_PCNF1_ENDIAN_Pos) |
			(RADIO_PCNF1_WHITEEN_Disabled     << RADIO_PCNF1_WHITEEN_Pos);
	}

	/* cover_addr (core) and SKIPADDR (hardware) have opposite senses. Inverting them fails every CRC. */
	out->crccnf =
		(RADIO_CRCCNF_LEN_Two << RADIO_CRCCNF_LEN_Pos) |
		((uint32_t)(fmt->crc.cover_addr ? RADIO_CRCCNF_SKIPADDR_Include
					       : RADIO_CRCCNF_SKIPADDR_Skip)
			<< RADIO_CRCCNF_SKIPADDR_Pos);
	out->crcinit = fmt->crc.init;

	/* PACKETPTR is set by the arm paths, not here: it depends on direction, and a transmit must not DMA from the receive buffer. */
	return RADIANT_RADIO_OK_RC;
}

/*
 * ---- Arm cache (perf plan 2026-11-15, B4) ----
 *
 * Two different things, kept apart on purpose:
 *
 * 1. Memos of the validation. format_prepare() and filters_prepare() (with
 *    rx_sw_addr_len()) are pure functions of the request, so a request equal
 *    field by field to one already accepted gets the same register images
 *    without running them again. Keyed on CONTENT, never on the pointers: a
 *    caller may rewrite a format or a filter array in place, and a pointer
 *    match would then skip a refusal. A memo holds only accepted requests, so
 *    every refusal still runs the full check and returns the same code. Nothing
 *    the hardware does can make a memo stale, so it is on in every build.
 *
 * 2. A shadow of what the RADIO holds (RADIANT_NRF_REG_SHADOW). format_write()
 *    and filters_write() skip the writes when the registers already hold the
 *    computed values. This one CAN go stale, and stale is a deaf radio with no
 *    error, so every other writer of MODE, FREQUENCY, PCNF0/1, CRCCNF, CRCPOLY,
 *    CRCINIT, BASE0/1, PREFIX0/1 or RXADDRESSES invalidates it:
 *      - apply_tx_address() (BASE0, PREFIX0.AP0): reg_shadow_forget_addr();
 *      - program_ed() and ed_enter_index() (MODE, RXADDRESSES, FREQUENCY):
 *        reg_shadow_forget() in program_ed(), before the first ED write;
 *      - radiant_radio_init(), _enable() and _disable(): reg_shadow_forget();
 *      - radiant_nrf_gate_on_grant() and _on_grant_end(): reg_shadow_forget(),
 *        since the other stack owns the RADIO between grants.
 *    Nothing else here writes them: abort and radio_disable_now() touch SHORTS
 *    and tasks only, which leave the configuration registers alone, and the ISR
 *    only reads. TXADDRESS, TXPOWER and PACKETPTR are written on every arm.
 *    Compiled out wherever another radio user can exist (MPSL gate, MPSL, BT,
 *    802.15.4). There the other stack owns the registers between grants, and on
 *    nRF54L the 802.15.4 driver's nrf_radio_reset() can land inside our own
 *    timeslot (see radiant_nrf_gate_on_grant()), which no hook here can see.
 *    Nearly every arm there opens a new grant anyway, so it would rarely hit.
 *
 *    Also nRF52 only. The nRF52 RADIO keeps its registers until RADIO->POWER
 *    or a reset, and nothing here touches POWER. On nRF54L and the nRF5340
 *    network core, whether the RADIO keeps them across CPU idle, HFXO release
 *    and automatic power-domain handling is unverified, and no bench could
 *    check it. To widen it: show on that part that registers written before an
 *    idle/HFXO-off period read back unchanged after it (or cite the datasheet
 *    guarantee), then run a tracked-loss A/B with and without the shadow. The
 *    memos are pure and stay on everywhere.
 *    A test may force it on by defining RADIANT_NRF_REG_SHADOW first.
 */
#if !defined(RADIANT_NRF_REG_SHADOW)
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL) || defined(CONFIG_MPSL) ||   \
	defined(CONFIG_BT) || defined(CONFIG_NRF_802154_RADIO_DRIVER) ||        \
	defined(CONFIG_IEEE802154)
#define RADIANT_NRF_REG_SHADOW 0
/* SOC_SERIES_NRF52X before Zephyr 4.4 (NCS v3.2.4), SOC_SERIES_NRF52 after. */
#elif defined(CONFIG_SOC_SERIES_NRF52X) || defined(CONFIG_SOC_SERIES_NRF52)
#define RADIANT_NRF_REG_SHADOW 1
#else
#define RADIANT_NRF_REG_SHADOW 0
#endif
#endif

/*
 * Both memos evict the least recently used entry (stamp = arm counter, compared
 * by age, so the 32-bit wrap is harmless). A miss is a full check, not an error.
 *
 * Formats: a Zwift-style load arms three in rotation, the search format (scan
 * chunks), tracking receive (short match) and tracking transmit (sw_addr_len 0).
 * Four (about 60 B each) keeps all three hot with a spare for a coded-PHY or a
 * second tracking variant.
 *
 * Filter sets: the scan's set is constant and each tracked channel has its own,
 * so the stream looks like S,A,S,B,C,S,D... LRU keeps S hot while at most N-1
 * other sets arm between two scan chunks. N=3 (about 96 B each) covers two
 * tracked windows back to back between chunks. The tracked sets mostly miss
 * whatever N is (eight channels rotate through), so a bigger N buys little for
 * its RAM.
 *
 * Concurrency: memo and shadow are touched only by the arm call that holds the
 * operation slot (radiant_op.kind is claimed under irq_lock(), so a nested arm
 * gets EBUSY before it reads them) and by program_*() for that operation. An
 * abort from interrupt context during a thread arm is the existing radiant_op
 * race class (it frees the slot under the arm); the abort never writes the memo.
 */
#define FMT_MEMO_N  4u
#define FILT_MEMO_N 3u

static uint32_t arm_memo_clock;

static struct {
	struct radiant_pkt_format key;
	uint8_t                   rf_index;
	uint8_t                   sw_addr_len;
	bool                      valid;
	uint32_t                  used;
	struct nrf_fmt_regs       regs;
} fmt_memo[FMT_MEMO_N];

/* Field by field: a struct copy carries padding, which memcmp() would compare. */
static bool fmt_same(const struct radiant_pkt_format *a,
		     const struct radiant_pkt_format *b)
{
	return a->phy == b->phy && a->addr_len == b->addr_len &&
	       a->len_mode == b->len_mode && a->body_len == b->body_len &&
	       a->len_offset == b->len_offset && a->len_bias == b->len_bias &&
	       a->max_body_len == b->max_body_len &&
	       a->crc.width_bits == b->crc.width_bits &&
	       a->crc.poly == b->crc.poly && a->crc.init == b->crc.init &&
	       a->crc.xor_out == b->crc.xor_out &&
	       a->crc.reflect_in == b->crc.reflect_in &&
	       a->crc.reflect_out == b->crc.reflect_out &&
	       a->crc.cover_addr == b->crc.cover_addr;
}

/* format_prepare(), answered from the memo when this exact request was accepted before. */
static int format_prepare_memo(const struct radiant_pkt_format *fmt,
			       uint8_t rf_index, uint8_t sw_addr_len,
			       struct nrf_fmt_regs *out)
{
	int rc;

	if (fmt == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	for (uint8_t i = 0u; i < FMT_MEMO_N; i++) {
		if (fmt_memo[i].valid && fmt_memo[i].rf_index == rf_index &&
		    fmt_memo[i].sw_addr_len == sw_addr_len &&
		    fmt_same(&fmt_memo[i].key, fmt)) {
			fmt_memo[i].used = ++arm_memo_clock;
			*out = fmt_memo[i].regs;
			return RADIANT_RADIO_OK_RC;
		}
	}
	rc = format_prepare(fmt, rf_index, sw_addr_len, out);
	if (rc == RADIANT_RADIO_OK_RC) {
		uint8_t i = 0u;

		/* An unused entry first, else the least recently used. */
		for (uint8_t j = 0u; j < FMT_MEMO_N; j++) {
			if (!fmt_memo[j].valid) {
				i = j;
				break;
			}
			if ((uint32_t)(arm_memo_clock - fmt_memo[j].used) >
			    (uint32_t)(arm_memo_clock - fmt_memo[i].used)) {
				i = j;
			}
		}
		fmt_memo[i].used = ++arm_memo_clock;
		fmt_memo[i].key = *fmt;
		fmt_memo[i].rf_index = rf_index;
		fmt_memo[i].sw_addr_len = sw_addr_len;
		fmt_memo[i].regs = *out;
		fmt_memo[i].valid = true;
	}
	return rc;
}

#if RADIANT_NRF_REG_SHADOW
static struct {
	bool                 fmt_valid;
	bool                 filt_valid;
	struct nrf_fmt_regs  fmt;
	struct nrf_filt_regs filt;
} reg_shadow;

/* Something else wrote, or may have written, the RADIO's configuration. */
static inline void reg_shadow_forget(void)
{
	reg_shadow.fmt_valid = false;
	reg_shadow.filt_valid = false;
}

/* Something else wrote BASE0/PREFIX0 (a transmit address). */
static inline void reg_shadow_forget_addr(void)
{
	reg_shadow.filt_valid = false;
}
#else
static inline void reg_shadow_forget(void)
{
}

static inline void reg_shadow_forget_addr(void)
{
}
#endif

/* Put a format_prepare() result in the peripheral, unless it is there already. */
static void format_write(const struct nrf_fmt_regs *r)
{
#if RADIANT_NRF_REG_SHADOW
	if (reg_shadow.fmt_valid &&
	    memcmp(&reg_shadow.fmt, r, sizeof(*r)) == 0) {
		return;
	}
#endif
	nrf_radio_mode_set(NRF_RADIO, (nrf_radio_mode_t)r->mode);
	nrf_radio_frequency_set(NRF_RADIO, (uint16_t)r->frequency);
	NRF_RADIO->PCNF0   = r->pcnf0;
	NRF_RADIO->PCNF1   = r->pcnf1;
	NRF_RADIO->CRCCNF  = r->crccnf;
	NRF_RADIO->CRCPOLY = 0x11021u;
	NRF_RADIO->CRCINIT = r->crcinit;
#if RADIANT_NRF_REG_SHADOW
	reg_shadow.fmt = *r;
	reg_shadow.fmt_valid = true;
#endif
}

/*
 * The TXPOWER field is not dBm. nRF52840 happens to match (+8 dBm = 0x08), but
 * nRF54L15 encodes +8 dBm as 0x3F and 0 dBm as 0x18, so a raw cast would transmit
 * at an unrelated power with no error. Map through the part's MDK symbols; a part
 * that names a power differently then fails to compile instead of guessing.
 *
 * The positive rows are guarded by one threshold, so a part missing any of them
 * still fails to compile. A shorter table is safe: apply_power() picks the nearest
 * row, and a missing symbol removes a row rather than writing a wrong value.
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
 * The HAL rounds to the nearest setting and never fails for an unreachable
 * value, so this returns no error. use_raw writes the register directly for bench
 * power sweeps. It is not portable, and the core must never set it.
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
 * Program up to eight receive filters, or refuse the set. The set must use at
 * most two distinct bases, with at most one filter on the odd one out (see
 * header). Refusal is not negotiable: silently programming a subset would lose
 * frames undiagnosably.
 */
/* Logical address -> index into the caller's filters[] (inverse map built by filters_prepare(), installed by filters_write()). */
static uint8_t radiant_filter_slot[8];

/*
 * hw_len: leading bytes of each filter the hardware matches (addr_len, or fewer
 * under TRACK_HW_ADDR_LEN). Filters may pack to one hardware address; each still
 * takes its own logical address, and radiant_addr_split_match() picks the real one.
 *
 * Pure, like format_prepare(): it refuses or computes the register images and
 * the slot map, and filters_write() installs them.
 */
static int filters_prepare(const struct radiant_rx_filter *filters, uint8_t n,
			   uint8_t addr_len, uint8_t hw_len,
			   struct nrf_filt_regs *out, uint8_t slot_map[8])
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

	/* Majority base goes to BASE1 (logical 1..7); at most one other filter goes to BASE0. */
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
				 * The eighth filter on BASE1 goes to logical 0, and prefixes[0] must be
				 * set too. Without it the eighth filter matches prefix 0x00 and silently
				 * drops every eighth device number in a wildcard sweep.
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

	/* The packing loop above decides the filter-to-address mapping and is the only part that can refuse the set. */
	out->base0   = base0;
	out->base1   = base1;
	out->prefix0 = ((uint32_t)prefixes[0]) |
		       ((uint32_t)prefixes[1] << 8) |
		       ((uint32_t)prefixes[2] << 16) |
		       ((uint32_t)prefixes[3] << 24);
	out->prefix1 = ((uint32_t)prefixes[4]) |
		       ((uint32_t)prefixes[5] << 8) |
		       ((uint32_t)prefixes[6] << 16) |
		       ((uint32_t)prefixes[7] << 24);

	{
		uint32_t mask = 0;

		for (uint8_t i = 0; i < n; i++) {
			mask |= (1u << slot_of[i]);
		}
		out->rxaddresses = mask;
	}

	/*
	 * rx_event.filter_index indexes the CALLER's filters[], not logical addresses:
	 * radiant_search.c recovers devnum_lo from it. Entries for logical addresses
	 * the set does not enable are zero; RXMATCH can only name an enabled one.
	 */
	memset(slot_map, 0, 8u);
	for (uint8_t i = 0; i < n; i++) {
		slot_map[slot_of[i]] = i;
	}
	return RADIANT_RADIO_OK_RC;
}

static struct {
	struct radiant_rx_filter key[8];
	uint8_t                  n;
	uint8_t                  addr_len;
	bool                     phy_1m;
	bool                     valid;
	uint32_t                 used;
	struct nrf_filt_regs     regs;
	uint8_t                  slot[8];
	uint8_t                  sw_addr_len;
} filt_memo[FILT_MEMO_N];

BUILD_ASSERT(sizeof(struct radiant_rx_filter) ==
		     RADIANT_RADIO_ADDR_MAX + 2u,
	     "filt_memo compares filters with memcmp(), which needs a "
	     "radiant_rx_filter without padding");

/*
 * Find the filter half of a receive request in the memo: the slot map, the
 * register images and rx_sw_addr_len(), which depend only on the filters, the
 * address length and the PHY. Returns the entry, or -1 on a miss. Only accepted sets are stored,
 * so a hit can never stand in for a refusal.
 */
static int filt_memo_find(const struct radiant_rx_req *req)
{
	const struct radiant_pkt_format *fmt = req->fmt;
	const bool phy_1m = (fmt->phy == RADIANT_PHY_1M_GFSK);

	if (req->filters == NULL || req->n_filters == 0u ||
	    req->n_filters > ARRAY_SIZE(filt_memo[0].key)) {
		return -1;
	}
	for (uint8_t i = 0u; i < FILT_MEMO_N; i++) {
		if (filt_memo[i].valid && filt_memo[i].n == req->n_filters &&
		    filt_memo[i].addr_len == fmt->addr_len &&
		    filt_memo[i].phy_1m == phy_1m &&
		    memcmp(filt_memo[i].key, req->filters,
			   (size_t)req->n_filters *
				   sizeof(struct radiant_rx_filter)) == 0) {
			filt_memo[i].used = ++arm_memo_clock;
			return (int)i;
		}
	}
	return -1;
}

static void filt_memo_store(const struct radiant_rx_req *req,
			    const struct nrf_rx_prep *prep)
{
	uint8_t i = 0u;

	/* An unused entry first, else the least recently used. */
	for (uint8_t j = 0u; j < FILT_MEMO_N; j++) {
		if (!filt_memo[j].valid) {
			i = j;
			break;
		}
		if ((uint32_t)(arm_memo_clock - filt_memo[j].used) >
		    (uint32_t)(arm_memo_clock - filt_memo[i].used)) {
			i = j;
		}
	}
	filt_memo[i].used = ++arm_memo_clock;
	memcpy(filt_memo[i].key, req->filters,
	       (size_t)req->n_filters * sizeof(struct radiant_rx_filter));
	filt_memo[i].n = req->n_filters;
	filt_memo[i].addr_len = req->fmt->addr_len;
	filt_memo[i].phy_1m = (req->fmt->phy == RADIANT_PHY_1M_GFSK);
	filt_memo[i].regs = prep->filt;
	memcpy(filt_memo[i].slot, prep->slot, sizeof(filt_memo[i].slot));
	filt_memo[i].sw_addr_len = prep->sw_addr_len;
	filt_memo[i].valid = true;
}

/* Drop every memo entry. Never needed for correctness (they are pure); tests use it. */
__maybe_unused static void arm_memo_forget(void)
{
	memset(fmt_memo, 0, sizeof(fmt_memo));
	memset(filt_memo, 0, sizeof(filt_memo));
	arm_memo_clock = 0u;
}

/*
 * Everything a receive request needs validated, computed once per arm call.
 * The refusal order is the one rx_arm() always had: format first, then
 * filters. rx_sw_addr_len() runs before either check, as it always did (its
 * result feeds both).
 */
static int rx_prepare(const struct radiant_rx_req *req, struct nrf_rx_prep *prep)
{
	int hit = filt_memo_find(req);
	int rc;

	if (hit >= 0) {
		prep->sw_addr_len = filt_memo[hit].sw_addr_len;
	} else {
		prep->sw_addr_len = rx_sw_addr_len(req);
	}
	rc = format_prepare_memo(req->fmt, req->rf_index, prep->sw_addr_len,
				 &prep->fmt);
	if (rc != RADIANT_RADIO_OK_RC) {
		return rc;
	}
	if (hit >= 0) {
		prep->filt = filt_memo[hit].regs;
		memcpy(prep->slot, filt_memo[hit].slot, sizeof(prep->slot));
		return RADIANT_RADIO_OK_RC;
	}
	rc = filters_prepare(req->filters, req->n_filters, req->fmt->addr_len,
			     (uint8_t)(req->fmt->addr_len - prep->sw_addr_len),
			     &prep->filt, prep->slot);
	if (rc == RADIANT_RADIO_OK_RC) {
		filt_memo_store(req, prep);
	}
	return rc;
}

/* Install a filters_prepare() result: the slot map always, the registers unless they are there already. */
static void filters_write(const struct nrf_filt_regs *r, const uint8_t slot_map[8])
{
	memcpy(radiant_filter_slot, slot_map, sizeof(radiant_filter_slot));
#if RADIANT_NRF_REG_SHADOW
	if (reg_shadow.filt_valid &&
	    memcmp(&reg_shadow.filt, r, sizeof(*r)) == 0) {
		return;
	}
#endif
	NRF_RADIO->BASE0       = r->base0;
	NRF_RADIO->BASE1       = r->base1;
	NRF_RADIO->PREFIX0     = r->prefix0;
	NRF_RADIO->PREFIX1     = r->prefix1;
	NRF_RADIO->RXADDRESSES = r->rxaddresses;
#if RADIANT_NRF_REG_SHADOW
	reg_shadow.filt = *r;
	reg_shadow.filt_valid = true;
#endif
}

/*
 * Program the one address a transmit emits. TXADDRESS is a logical index into
 * the BASE/PREFIX registers, so BASE0/PREFIX0.AP0 is written every time rather
 * than reusing what the last operation loaded. Logical 0 is the only one with
 * its own base; 1..7 share BASE1, which would tie the address to the last receive.
 */
static void apply_tx_address(const uint8_t *addr, uint8_t addr_len)
{
	uint32_t base;
	uint8_t  prefix;

	pack_address(addr, addr_len, &base, &prefix);

	/* BASE0 and PREFIX0 are half of a receive filter set: the next receive rewrites them. */
	reg_shadow_forget_addr();
	NRF_RADIO->BASE0 = base;
	NRF_RADIO->PREFIX0 = (NRF_RADIO->PREFIX0 & ~0xFFu) | (uint32_t)prefix;
	nrf_radio_txaddress_set(NRF_RADIO, 0u);
}

/*
 * Air from switch-on to t_sync: ramp-up plus preamble and address, derived from
 * the definition rather than tuned. Per PHY, and the two differ by about seven
 * times, so a wrong value misplaces a transmit or a receive window. addr_len is
 * ignored on the coded PHY, whose access address is fixed at 32 bits.
 */
static uint32_t air_lead_us(enum radiant_phy phy, uint8_t addr_len)
{
	if (phy == RADIANT_PHY_LR_CODED) {
		return RAMP_UP_US + LR_PREAMBLE_US + LR_ACCESS_ADDR_US;
	}
	return RAMP_UP_US +
	       (uint32_t)(PREAMBLE_BYTES + addr_len) * US_PER_BYTE;
}

/*
 * RSSI temperature correction, errata 153 (nRF52840) / 225 (nRF52833). The
 * thresholds are copied from Nordic's nrf_802154_rssi.c (BSD-3-Clause), not
 * measured here. This file works in negated dBm, so corrected_dbm = raw_dbm - corr.
 * Nordic applies the opposite sign to thresholds, so a threshold compared with
 * rssi_dbm must not be corrected the same way.
 *
 * Nordic returns no correction for nRF54L, so only nRF52840 and nRF52833 compile it in.
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
	/* Beyond the erratum's table. Nordic notes the nRF52840 cannot run above 85 degC anyway. */
	return -4;
}

/*
 * Cached: a TEMP conversion cannot run in the radio ISR. 20 degC is Nordic's
 * default temperature and this table's zero-correction band centre.
 */
static int8_t rssi_temp_cache_c = 20;

/* 60 s, as Nordic's TEMPERATURE_UPDATE_PERIOD default: die temperature moves slowly. */
#define RSSI_TEMP_UPDATE_PERIOD_MS 60000u

static struct k_work_delayable rssi_temp_work;

static void rssi_temp_work_handler(struct k_work *work)
{
	int16_t centi_c;

	ARG_UNUSED(work);

	if (radiant_radio_nrf_die_temp_c(&centi_c) == RADIANT_RADIO_OK_RC) {
		rssi_temp_cache_c = (int8_t)(centi_c / 100);
	}
	/* A failed read keeps the last good value; resetting to 20 degC would step every RSSI report. */
	k_work_reschedule(&rssi_temp_work, K_MSEC(RSSI_TEMP_UPDATE_PERIOD_MS));
}

#endif /* RSSI_TEMP_CORR_PRESENT */

/*
 * RSSISAMPLE in dBm, corrected. The only read of this register. The HAL needs
 * rssi_dbm, noise_dbm and the ED levels on one scale, so the correction lives
 * here alone. The register holds a positive magnitude, hence the negation.
 */
/* A RSSISAMPLE value in corrected dBm. Split from rssi_sample_dbm() so the
 * END branch can convert the value it snapshotted with the frame. */
static int8_t rssi_dbm_from(uint32_t rssisample)
{
	int8_t raw_dbm = (int8_t)(-(int32_t)(rssisample & 0x7Fu));

#if RSSI_TEMP_CORR_PRESENT
	/* raw_dbm - corr, not + corr. */
	return (int8_t)(raw_dbm - rssi_temp_corr_get(rssi_temp_cache_c));
#else
	return raw_dbm;
#endif
}

static int8_t rssi_sample_dbm(void)
{
	return rssi_dbm_from(NRF_RADIO->RSSISAMPLE);
}

/* ---- Lifecycle ---- */

/*
 * A newly claimed operation is owed exactly one terminal event. A macro so the
 * three arm calls share it, beside `radiant_op.kind = OP_x` under the same irq_lock.
 * Under the gate it also starts with no FAILED owed: gate_failed belongs to the
 * operation whose grant failed, and no path may carry it into a new one (a
 * claim needs the slot free, so the failed operation has had its terminal).
 */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL) || defined(CONFIG_RADIANT_NRF_LOW_POWER)
#define CLAIM_TERMINAL()                                                       \
	do {                                                                   \
		radiant_op.terminal_sent = false;                              \
		radiant_op.gate_failed = false;                                \
	} while (0)
#else
#define CLAIM_TERMINAL() (radiant_op.terminal_sent = false)
#endif

/*
 * Not behind the GATE_MPSL #if: radiant_radio_enable()/disable() use these
 * unconditionally, and the cost is 25 bytes of .bss.
 *
 * Still gated on the silicon: parts with the older PPI have no PUBLISH_/
 * SUBSCRIBE_ registers, so the accesses below would not compile. DPPIC_PRESENT
 * is the MDK's discriminator. It is defined empty on nRF53 and as 1 on nRF54L,
 * so test it with defined(), never #if.
 */
#if defined(DPPIC_PRESENT)
#define RADIANT_NRF_RADIO_EP 1
#else
#define RADIANT_NRF_RADIO_EP 0
#endif

/*
 * Whether this backend holds the RADIO now. Under the MPSL gate that is only
 * between SIGNAL_START and the ACTION_END that closes the timeslot. Outside it
 * the RADIO belongs to the 802.15.4 driver, which receives on it continuously.
 *
 * deliver_terminal(), radiant_radio_abort() and the on_grant_end() path write
 * shared RADIO registers (SHORTS, TASKS_STOP/DISABLE, endpoints). Denied
 * operations are delivered from the gate's work queue with no timeslot held.
 * Clearing SHORTS mid-frame leaves the 802.15.4 driver waiting for a DISABLED
 * that never comes, and its assert fires. So every shared-RADIO write is gated
 * on this. It is a constant true on the direct backend.
 *
 * Not g.hw_held in the MPSL gate file: that is also true for a bootstrap
 * timeslot, which never takes the peripheral. This flag is set only where the
 * peripheral is actually taken.
 */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
static volatile bool radio_on_loan;
#define RADIO_IS_OURS() (radio_on_loan)
#else
#define RADIO_IS_OURS() true
#endif

/*
 * Defined with the gate callbacks. The grant hooks call attach() unconditionally,
 * so non-gate builds get an explicit no-op instead of a dangling reference.
 */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
static void radio_endpoints_save(void);
__maybe_unused static void radio_endpoints_attach(bool on);
#else
static inline void radio_endpoints_attach(bool on)
{
	(void)on;
}
#endif

/* Return borrowed endpoints to their owner at init. This is not the per-grant swap. Unconditional. */
static void radio_endpoints_attach_off_at_init(void);

/*
 * The six RADIO endpoint registers this backend programs. A (D)PPI connection is
 * a DPPIC channel plus a PUBLISH_/SUBSCRIBE_ register; these are the peripheral halves.
 *
 * radio_ep_contended is the safety argument for the swap: a bit is set only for a
 * register that already held another stack's value at init, and only those move
 * at a grant boundary. Beside OpenThread that is PUBLISH_ADDRESS and SUBSCRIBE_RXEN.
 */
#define RADIO_EP_COUNT 6U

/*
 * Per-grant endpoint swap. 1: take the contended endpoints on grant entry and
 * return them on exit. 0: never move them, so ANT+ cannot drive a contended
 * endpoint. The allocation-time borrow happens either way.
 *
 * With the swap on, a grant that ends without reaching on_grant_end() leaves
 * the 802.15.4 driver's SUBSCRIBE_RXEN taken, which stops its receiver for the
 * rest of boot. Hence g.hw_held and the gate ztests. With no contended endpoints
 * the two settings are identical.
 */
#define RADIANT_NRF_GRANT_EP_SWAP 1

__maybe_unused static uint32_t radio_ep_foreign[RADIO_EP_COUNT];
static uint8_t  radio_ep_contended;

/* Endpoints this backend allocated (the set the swap moves), not what another stack held at init. */
__maybe_unused static uint8_t  radio_ep_mine_mask;

/* Whether RADIO_0_IRQn was NVIC-enabled at grant start, so it can be restored. */
__maybe_unused static bool     radio_irq_was_enabled;

/*
 * This timeslot's on_grant() took the radio (set beside radio_on_loan, which the
 * hand-back clears too early to serve here). Cleared at every SIGNAL_START by
 * radiant_nrf_gate_on_slot_start() and consumed by
 * radiant_nrf_gate_radio_irq_unpend() when the timeslot's ACTION_END is returned.
 */
__maybe_unused static volatile bool radio_lent_this_slot;

/* This backend's half of the swap: the register values when our connections are attached. */
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

/* Per register: a taken publication and a taken subscription fail differently. */
static void radio_endpoints_attach_off_at_init(void)
{
#if RADIANT_NRF_RADIO_EP
	for (size_t r = 0U; r < RADIO_EP_COUNT; r++) {
		if ((radio_ep_contended & (1U << r)) != 0U) {
			*radio_ep_reg(r) = radio_ep_foreign[r];
		}
	}
#endif
	/* On PPI parts radio_ep_contended is always zero, so there is nothing to restore. */
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
/* Defined with the ISR, where the EVENTS_DISABLED reasoning lives (see radio_disable_now()). */
static void radio_disable_now(void);
static void deliver_terminal(enum radiant_radio_status st);

/*
 * ---- The core callback, and why it does not run where it is raised ----
 *
 * Under the MPSL gate the RADIO interrupt arrives as a timeslot signal from an
 * IRQ_ZERO_LATENCY vector, above every irq_lock() and kernel spinlock (see
 * docs/p4-coexistence-bugs.md). A lock-free event ring alone is not enough:
 * radiant_op.cbs->rx() re-arms the scheduler and touches channel and API state
 * that thread context guards with k_mutex_lock(&api_lock), and a mutex cannot
 * exclude a zero-latency interrupt. Measured: lockups with the timeslot session
 * open (13 in 620 s), none without it (9 min 52 s).
 *
 * Still in the signal context: everything touching the peripheral or the grant
 * (DPPI disables, SHORTS, the IRQ mask, radio_disable_now(), gate_release(),
 * which must run promptly). Only the core callback is deferred. The event is
 * fully built before it is queued, so every register read still happens in the
 * window it describes.
 *
 * Single producer, single consumer: the producer is the signal callback, which
 * cannot preempt itself, and the consumer is the trampoline handler, which runs
 * below it. Other contexts have gate_in_signal() false and call the core directly.
 */

#if defined(CORE_CB_DEFERRED)

/*
 * Longest body handed up: the HAL maximum, raised to 40 for the long-range format
 * (RADIANT_FRAME_LR_BODY_MAX). Copied because the rx_event pointer is valid only
 * for the duration of the callback, which now runs later.
 */
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
 * Eight slots: two or more completions can land before the trampoline runs, and
 * a dropped completion stalls a channel silently. Power of two for the mask.
 */
#define CORE_CB_RING 8u
static struct core_cb_entry core_cb_q[CORE_CB_RING];
/* head is written only by the producer, tail only by the consumer. Both are free-running 32-bit counters. */
static volatile uint32_t core_cb_head;
static volatile uint32_t core_cb_tail;

/*
 * Grant handoff. follow_on_us reserves air for the next operation, armed from the
 * current completion (an acknowledged reply, an ACK window, a master's turnaround).
 * That air is 1.5-2.2 ms out, far nearer than a new reservation can be placed. So
 * the terminal (or abort) ending the current operation must not release the grant
 * first, or the follow-on finds nothing held and is refused.
 *
 * The release is therefore owed while a completion is still to run: during the
 * trampoline dispatch, or from the signal context that queued one. When the
 * dispatch drains, the debt is settled: kept if a follow-on was programmed into
 * the held air, otherwise paid. A dispatch that never comes is bounded by the
 * grant's own ending compare.
 *
 * Only while a grant is held. With none held, gate_release() must run now, before
 * the callback can place a new request. gate_held() is constant false on the
 * direct gate.
 */
static volatile bool grant_release_owed;
static uint32_t      core_cb_depth;
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS_LOG)
/* Bench diagnostic (ADR 0018 d2): settle outcomes [0]-[3], d2_settle_now [4], after_core immediate [5] and owed [6]. */
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
			/* Pend even if the enqueue failed, so a dispatch always settles the debt. */
			radiant_swi_pend(RADIANT_SWI_CORE_CB);
		}
		return;
	}
	D2DBG(5);
	gate_release();
}

static void grant_release_settle(void)
{
	/* A queued completion may still arm the follow-on; its dispatch settles instead. */
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
 * ADR 0018 (d2): the operation just armed was staged behind a new request, so
 * nothing can follow into the held air and an owed release can be paid now. MPSL
 * judges the new request against the held timeslot's booked length until it ends,
 * so every microsecond of delay comes out of the next request's notice.
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
			/* Repoint at our copy: the backend buffer may already be reused. */
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
		/* Advance AFTER the callback, so the slot is not reused while still read. */
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
 * The last slot is reserved for terminal events. A full ring of frames would
 * otherwise refuse the window's terminal, and the scheduler would never arm
 * another operation. One slot suffices, since only one operation is armed at a
 * time. A refused frame is ordinary loss.
 */
static bool core_cb_enqueue(const struct core_cb_entry *src, bool terminal)
{
	uint32_t head = core_cb_head;

	uint32_t depth = head - core_cb_tail;

	if (depth >= (terminal ? CORE_CB_RING : CORE_CB_RING - 1u)) {
		return false;
	}
	core_cb_q[head % CORE_CB_RING] = *src;
	/* Slot complete before the consumer is told. A compiler barrier suffices: producer and consumer share one CPU. */
	__asm__ volatile("" ::: "memory");
	core_cb_head = head + 1u;
	radiant_swi_pend(RADIANT_SWI_CORE_CB);
	return true;
}
#else
/* No trampoline: every completion runs inline, so nothing is owed. */
static inline void grant_release_after_core(void)
{
	gate_release();
}
#endif /* CORE_CB_DEFERRED */

/* The three seams: queued when raised in a zero-latency context, inline otherwise. */
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
			/* No copy: report "no body" consistently (NULL with length 0). */
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

	/* 0xF7 pages 9-10: the A0 cycle clock, at this core's clock. */
	NRF_DIAG(radiant_diag_cycles_init(SystemCoreClock));

	/*
	 * Gate first, before any peripheral is claimed. On an arbitrated build this
	 * opens the session every later acquire needs. Claiming the RADIO and TIMER
	 * first would leave this backend holding peripherals it may not own.
	 */
	err = gate_init();
	if (err != RADIANT_RADIO_OK_RC) {
		LOG_ERR("radio gate did not initialise (%d)", err);
		return err;
	}

#if defined(CORE_CB_DEFERRED)
	/*
	 * After gate_init() (which claims the trampoline line) and before the RADIO
	 * can raise anything. Fatal: without a consumer, completions queue forever
	 * and the dongle hears nothing with no counter moving.
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

	/* The RADIO needs the HFXO crystal: 1 Mbps GFSK does not hold bit sync on HFINT. Blocking.
	 * Held from here until reset: the power proxy reads it as ~uptime. */
	POWER_DIAG(radiant_power_diag_hfxo(true));
#if defined(CONFIG_RADIANT_NRF_LOW_POWER)
	/* Through the HFXO's onoff manager, which every later request uses. */
	err = lp_hfxo_boot();
#else
	err = clock_control_on(clk, CLOCK_CONTROL_NRF_SUBSYS_HF);
#endif
	if (err < 0) {
		POWER_DIAG(radiant_power_diag_hfxo(false));
		LOG_ERR("HFXO did not start (%d)", err);
		return RADIANT_RADIO_EIO;
	}

#if defined(NRF54L_ERRATA_20_PRESENT)
	if (nrf54l_errata_20()) {
#if defined(CONSTLAT_REFCOUNTED)
#if defined(CONFIG_RADIANT_NRF54L_MPSL_CONSTLAT_HOLD)
		/*
		 * One reference, never freed: MPSL's release at the end of each of
		 * its events then drops the count to 1, not 0, and CONSTLAT stays on
		 * as it does on a direct build. MPSL's callbacks run in a zero-latency
		 * interrupt, which nrfx's irq_lock() critical section does not mask,
		 * so the count's read-modify-write here is fenced with PRIMASK.
		 * -EALREADY (MPSL is inside an event) is success.
		 */
		if (!constlat_held) {
			uint32_t cl_key = radiant_swi_zli_lock();

			(void)nrfx_power_constlat_mode_request();
			radiant_swi_zli_unlock(cl_key);
			constlat_held = true;
		}
		POWER_DIAG(radiant_power_diag_constlat_requested());
#else
		/* HOLD=n: no request; MPSL's per-event acquire/release is all. */
#endif
#elif defined(CONFIG_RADIANT_NRF_LOW_POWER)
		/* nrfx's reference, freed and taken again with the HFXO. */
		lp_constlat(true);
		POWER_DIAG(radiant_power_diag_constlat_requested());
#else
		nrf_power_task_trigger(NRF_POWER, NRF_POWER_TASK_CONSTLAT);
		POWER_DIAG(radiant_power_diag_constlat_requested());
#endif
	}
#endif

	/*
	 * Fast ramp-up (see RAMP_UP_US). Without it every timing constant is ~100 us
	 * wrong on nRF52840, though exact on nRF54L15. Guarded on the nrfx symbol,
	 * since no NCS header defines RADIO_TIMING_RU_Msk and a series test would not
	 * track compile-ability. Zephyr's BLE controller sets it the same way on nRF52.
	 */
#if defined(RADIO_MODECNF0_RU_Msk) || defined(RADIO_TIMING_RU_Msk)
	nrf_radio_fast_ramp_up_enable_set(NRF_RADIO, true);
	if (!nrf_radio_fast_ramp_up_check(NRF_RADIO)) {
		/* Loud: otherwise the link works but is 100 us out of phase on every frame. */
		LOG_ERR("fast ramp-up did not take; every timing constant in "
			"this backend is now ~100 us optimistic");
		return RADIANT_RADIO_EIO;
	}
#endif

	/* 1 MHz, 32-bit, free-running from here until reset. */
	nrf_timer_mode_set(TIMER_ADDR, NRF_TIMER_MODE_TIMER);
	nrf_timer_bit_width_set(TIMER_ADDR, NRF_TIMER_BIT_WIDTH_32);
	/*
	 * 1 MHz, derived from this instance's base frequency: nRF54L TIMERs are
	 * prescaler-only, so nrf_timer_frequency_set() is not on every part.
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

	/* Nothing this backend wrote is in the RADIO yet (see RADIANT_NRF_REG_SHADOW). */
	reg_shadow_forget();

	/*
	 * Five connections, allocated once: ADDRESS -> capture (t_sync, always live),
	 * the three window edges (enabled per operation), and RXREADY -> RSSISTART.
	 */
	/* Nothing borrowed yet; see radio_ep_reg() and the borrow loop below. */
	radio_ep_contended = 0U;

	/*
	 * Four of these cross a DPPI domain (TIMER in DPPIC20, RADIO in DPPIC10);
	 * conn_rssi stays in one domain. The two kinds of failure need different fixes.
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
	 * Clear the RADIO endpoints before allocating: nrfx_gppi_conn_alloc() returns
	 * -EINVAL if an endpoint is already attached. Beside a Thread stack the 802.15.4
	 * driver claims several RADIO publications at its own init (measured on nRF54L15:
	 * connection 1 of 5 fails). So snapshot the registers, clear the ones needed,
	 * allocate, then restore the non-zero values. A zero register belongs to nobody;
	 * a non-zero one belongs to the other stack and must go back, since the RADIO is
	 * not ours between grants.
	 *
	 * Where endpoints are shared, the connection stays inert until
	 * radio_endpoints_attach() re-attaches our values inside a grant. On a PPI part
	 * (nRF52) none of this applies: `shared` is zero (RADIANT_NRF_RADIO_EP).
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
		/* Signed errno from the Zephyr wrapper, so test < 0. */
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

	/* The t_sync capture is hardware to hardware and is never turned off. */
	nrfx_gppi_conn_enable(radiant_op.conn_sync);

#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
	/*
	 * Save the six RADIO endpoint registers now, while all hold OUR values: after
	 * the allocations, before radio_endpoints_attach_off_at_init() hands the borrowed
	 * ones back. Saving after the hand-back would record the other stack's routing
	 * for the registers the swap moves. The receiver would then never ramp, and no
	 * terminal would arrive. Only relevant beside a second stack.
	 */
	radio_endpoints_save();
#endif

	/* Hand back exactly what was borrowed. Registers that were zero keep our value. */
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
	 * Connect both RADIO interrupt lines on nRF54L, which splits RADIO's interrupt
	 * over RADIO_0 and RADIO_1. Connecting the wrong one leaves the terminal event
	 * silently undelivered. The handler is idempotent across lines.
	 */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
	/*
	 * Under the gate neither line is connected. The priority matters, not the handler:
	 * RADIO_0 belongs to MPSL, which registers it as IRQ_DIRECT_CONNECT at zero
	 * latency. Connecting it here would make it maskable, and the SoftDevice
	 * Controller's radio interrupt would miss its events during bt_enable(). Inside a
	 * grant MPSL forwards RADIO events to radiant_nrf_gate_on_radio_irq(). The NVIC
	 * line is enabled in radiant_radio_enable() per mpsl.rst.
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
	 * Prove the clock runs before anything depends on it. A stopped timer fails
	 * silently: arms succeed, no compare fires, and nothing is logged.
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

	/* Low power starts awake; a refusal leaves the HFXO on (LP_OFF). */
	LP_HOOK(lp_init());

	/*
	 * One die-temperature reading, in thread context, on every boot. It logs the
	 * confound (radiant_radio_nrf_diag.h) and seeds the errata-153/225 RSSI cache
	 * on nRF52840/nRF52833 before the first refresh.
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
	 * RADIO->INTENSET is shared with the other stack: leaving END/DISABLED set
	 * makes the SoftDevice Controller assert on unscheduled interrupts. So under
	 * the gate the mask is set inside each grant and cleared when it ends
	 * (radiant_nrf_gate_on_grant()/_on_grant_end()). A direct build sets it once.
	 *
	 * Line 0 stays enabled and line 1 does not. Line 0 is MPSL's and the only line
	 * this backend raises interrupts on; disabling it runs every window to full
	 * length without completing. Line 1 is never written here. irq_enable() sets no
	 * priority, so it is safe on a line MPSL has already registered.
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
	/* Not needed by any path today (disable forgets too); kept so an enable never trusts what preceded it. */
	reg_shadow_forget();
	radiant_op.enabled = true;
	LP_HOOK(lp_on_enable());
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
	 * Abort first. Abort clears the staged operation and releases the reservation,
	 * and gate_release() hands the RADIO back, so it is with MPSL before the mask
	 * below is touched. The session stays open across a disable.
	 */
	(void)radiant_radio_abort();
	irq_disable(RADIANT_RADIO_IRQn);
#if defined(RADIANT_RADIO_IRQn_2)
	irq_disable(RADIANT_RADIO_IRQn_2);
#endif
	/*
	 * Not ~0 under the gate: INTENCLR is shared, and line 0 belongs to MPSL.
	 * ~0 would clear the SoftDevice Controller's interrupt config while it keeps
	 * running. On a direct build both lines are ours, so ~0 is the stronger teardown.
	 */
	if (IS_ENABLED(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)) {
		nrf_radio_int_disable(NRF_RADIO, NRF_RADIO_INT_END_MASK |
						 NRF_RADIO_INT_DISABLED_MASK |
						 NRF_RADIO_INT_RXREADY_MASK);
	} else {
		nrf_radio_int_disable(NRF_RADIO, ~0u);
	}
	/* While disabled the RADIO is anyone's (a host could hand it to another user). */
	reg_shadow_forget();
	radiant_op.enabled = false;
	return RADIANT_RADIO_OK_RC;
}

/* ---- Transmit ---- */

/*
 * radiant_tx_req carries its own address and never inherits one. TXADDRESS selects
 * a logical address whose BASE/PREFIX must already be programmed, so a transmit after
 * a receive on another channel would send the previous device number. Both the mock
 * and radiant_sched_request_tx() refuse an addr_len that disagrees with the format.
 */
static int program_tx(const struct radiant_tx_req *req,
		      const struct nrf_fmt_regs *fmt_regs);

int radiant_radio_tx(const struct radiant_tx_req *req, uint32_t *op)
{
	unsigned int key;
	enum gate_rc g;
	int          rc;
	struct nrf_fmt_regs fmt_regs;

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
	/* No address, no transmit: never an inherited one. */
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
#if defined(LP_SLOT_GUARD)
	/* The claim and its arming mark as one step against the zero-latency
	 * signal too (AN ARM PREEMPTED BY THE TIMESLOT'S END): an abort from it
	 * sees either no operation or one marked as arming. */
	uint32_t lp_ck;
#endif
	key = irq_lock();
	LP_GUARD_ONLY(lp_ck = radiant_swi_zli_lock());
	if (radiant_op.kind != OP_NONE) {
		LP_GUARD_ONLY(radiant_swi_zli_unlock(lp_ck));
		irq_unlock(key);
		return RADIANT_RADIO_EBUSY;
	}
	radiant_op.kind = OP_TX;
	CLAIM_TERMINAL();
	LP_GUARD_ONLY(LP_ARM_CLAIM());
	LP_GUARD_ONLY(radiant_swi_zli_unlock(lp_ck));
	irq_unlock(key);

	/* The only validation of the format: program_tx() writes what this computes. */
	rc = format_prepare_memo(req->fmt, req->rf_index, 0u, &fmt_regs);
	if (rc != RADIANT_RADIO_OK_RC) {
		goto fail;
	}
	/*
	 * Static length: the format's own length is the only one allowed. Length-from-body:
	 * STATLEN is 0, so the RADIO sends body[0] bytes with nothing checking them against
	 * body_len, which could send bytes past the buffer. Refused here, not trusted to the caller.
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
	 * Reserve the transmit's air, from the TXEN compare to the last bit of body and CRC.
	 * follow_on_us extends it: a listening master's turnaround is armed from this
	 * transmit's completion, so the arbiter must already know about it.
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
		 * Refuse synchronously: radiant_transfer.c arms the acknowledged reply from the
		 * receive callback with 1.56 ms to go, leaving no room for a retry.
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
		/* Copied only on the deferred path (see radiant_radio_rx()). The body must stay valid until completion. */
		radiant_staged.tx = *req;
		radiant_staged.tx_fmt = *req->fmt;
		radiant_staged.tx.fmt = &radiant_staged.tx_fmt;
		radiant_staged.tx_regs = fmt_regs;
		radiant_staged.pending = true;
		LP_GUARD_ONLY(LP_ARM_DONE());
		LP_HOOK(lp_staged());
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
		d2_settle_now();
#endif
		return RADIANT_RADIO_OK_RC;
	}

	rc = program_tx(req, &fmt_regs);
	if (rc != RADIANT_RADIO_OK_RC) {
		*op = 0u;
		gate_release();
		goto fail;
	}
	return RADIANT_RADIO_OK_RC;

fail:
	LOG_DBG("tx refused: %d (addr_len=%u)", rc, (unsigned)req->addr_len);
	radiant_op.kind = OP_NONE;
	/* Slot free before the mark goes: an abort from the signal in between
	 * sees no operation, never one it would deliver ABORTED to. */
	LP_GUARD_ONLY(compiler_barrier(); LP_ARM_DONE());
	return rc;
}

/*
 * Everything a transmit does to the peripheral, from the staged request. The
 * arm's body verbatim; see program_rx() for why it is a function.
 */
/*
 * A compare written behind the free-running counter never fires (until the 32-bit
 * wrap, ~71 min). Nothing then reaches DISABLED either, so the operation would hold
 * the scheduler's slot forever. program_rx() and program_tx() enable the start
 * connection before writing the compare on channel cc, so "counter past start_cc
 * with the event still clear" means the compare was written late. The capture
 * comes first: read the other way round, a compare could fire unseen and a live
 * operation would be called missed.
 */
static bool start_compare_missed(uint8_t cc, uint32_t start_cc)
{
	uint32_t now_cc = timer_capture(CC_NOW);

	return (int32_t)(now_cc - start_cc) >= 0 &&
	       !nrf_timer_event_check(TIMER_ADDR,
				      nrf_timer_compare_event_get(cc));
}

static int program_tx(const struct radiant_tx_req *req,
		      const struct nrf_fmt_regs *fmt_regs)
{
	radiant_time_t now;
	radiant_time_t t_start;
	uint32_t lead;
	uint32_t start_cc;
#if defined(LP_SLOT_GUARD)
	bool lp_ok;
	const uint32_t lp_key = lp_span_begin(&lp_ok);

	if (!lp_ok) {
		/* Its timeslot ended (or another began) since the claim. */
		lp_span_fail(lp_key);
		return RADIANT_RADIO_ETIME;
	}
#endif

	/* Validated by the arm call, which computed fmt_regs (format_prepare()). */
	format_write(fmt_regs);
	apply_tx_address(req->addr, req->addr_len);
	apply_power(&req->power);

	memcpy(radiant_tx_buf, req->body, req->body_len);
	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)radiant_tx_buf;

	/*
	 * Work back from t_sync: the caller names when the last address bit reaches the
	 * antenna, and the backend subtracts its own ramp-up and preamble. Calibration
	 * applies here too, or the request would be event-referenced and uniformly off
	 * the air. TXEN fires T_SYNC_CAL_TX_US early, so ADDRESS lands at t_sync_at - cal.
	 */
	lead = air_lead_us(req->fmt->phy, req->addr_len) +
	       (uint32_t)T_SYNC_CAL_TX_US;
	t_start = req->t_sync_at - (radiant_time_t)lead;

	now = radiant_radio_now();
	NRF_DIAG(nrf_diag_early(now, req->t_sync_at, lead));
	if (req->t_sync_at < now + lead) {
		/* Never silently late: a late master frame lands in the next slot. */
#if defined(LP_SLOT_GUARD)
		lp_span_fail(lp_key);
#endif
		return RADIANT_RADIO_ETIME;
	}

	radiant_op.addr_len        = req->addr_len;
	radiant_op.body_len        = req->body_len;
	/* Transmit reports only the length it was given, so this is always false here. */
	radiant_op.len_from_body   = false;
	radiant_op.phy             = req->fmt->phy;
	radiant_op.stop_on_first   = false;
	radiant_op.report_crc_fail = false;
	radiant_op.terminal_sent   = false;
	radiant_op.t_sync_req      = req->t_sync_at;

	/*
	 * READY_START begins transmission as soon as ramp-up completes. There is no
	 * end-of-packet short: its name differs between nRF52840 and nRF54L15, and the
	 * ISR triggers the disable explicitly.
	 */
	nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK);

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

	/*
	 * program_rx()'s order, for start_compare_missed(): park the compare (a stale
	 * CC_TXSTART may sit just ahead of the counter), clear its event, enable the
	 * connection, then write the real compare, so the only compare able to set the
	 * event is this one. If the counter has already passed it, the compare never
	 * fires and the operation would hold the scheduler's one slot forever. The
	 * earlier order (compare, enable, then `t_start <= now`) could call a live
	 * transmit late, or miss a compare that passed between the write and the enable.
	 */
	start_cc = clock_counter_at(t_start);
	nrf_timer_cc_set(TIMER_ADDR, CC_TXSTART, start_cc - 0x80000000u);
	nrf_timer_event_clear(TIMER_ADDR, nrf_timer_compare_event_get(CC_TXSTART));
	nrfx_gppi_conn_enable(radiant_op.conn_start_tx);
	nrf_timer_cc_set(TIMER_ADDR, CC_TXSTART, start_cc);

	if (start_compare_missed(CC_TXSTART, start_cc)) {
		nrfx_gppi_conn_disable(radiant_op.conn_start_tx);
		nrf_radio_shorts_set(NRF_RADIO, 0);
		NRF_DIAG(nrf_diag.tx_start_missed++);
		ARMDBG(2);
#if defined(LP_SLOT_GUARD)
		lp_span_fail(lp_key);
#endif
		return RADIANT_RADIO_ETIME;
	}
#if defined(LP_SLOT_GUARD)
	lp_span_end(lp_key);
#endif

	LOG_DBG("tx op=%u alen=%u blen=%u start=+%d last_err=%d",
		(unsigned)radiant_op.id, (unsigned)req->addr_len,
		(unsigned)req->body_len, (int)(int64_t)(t_start - now),
		(int)radiant_dbg_tx_err);
	return RADIANT_RADIO_OK_RC;
}

/* ---- Receive ---- */

/*
 * Everything a receive window does to the peripheral, from the staged request and
 * the register images its arm call computed (rx_prepare()). Kept apart from the
 * arm call because of the gate. It can only fail with ETIME; every other refusal
 * is answered before the gate is consulted, and is not re-checked here.
 */
static int program_rx(const struct radiant_rx_req *req,
		      const struct nrf_rx_prep *prep)
{
	radiant_time_t now;
	uint32_t start_cc, close_cc;

	const uint8_t sw_addr_len = prep->sw_addr_len;
#if defined(LP_SLOT_GUARD)
	bool lp_ok;
	const uint32_t lp_key = lp_span_begin(&lp_ok);

	if (!lp_ok) {
		/* Its timeslot ended (or another began) since the claim. */
		lp_span_fail(lp_key);
		return RADIANT_RADIO_ETIME;
	}
#endif

	format_write(&prep->fmt);
	filters_write(&prep->filt, prep->slot);
	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)radiant_rx_buf;

	/*
	 * Advertise the worst case but refuse only the impossible. radiant_api.c posts
	 * windows at now + min_arm_lead_us using an earlier `now`, so testing the
	 * advertised figure here would always fail by the elapsed gap. Test the lead this
	 * address length really needs; the advertised figure is for the core's planning.
	 */
	now = radiant_radio_now();
	NRF_DIAG(nrf_diag_early(now, req->t_open,
				air_lead_us(req->fmt->phy, req->fmt->addr_len)));
	if (req->t_open < now + air_lead_us(req->fmt->phy, req->fmt->addr_len)) {
		/* Never silently late: a late window looks like a quiet sensor. */
#if defined(LP_SLOT_GUARD)
		lp_span_fail(lp_key);
#endif
		return RADIANT_RADIO_ETIME;
	}

	radiant_op.n_filters       = req->n_filters;
	radiant_op.addr_len        = req->fmt->addr_len;
	radiant_op.sw_addr_len     = sw_addr_len;
	radiant_op.filters         = req->filters;
	radiant_op.crc_init        = (uint16_t)req->fmt->crc.init;
	/*
	 * Static length: every frame is this long. Length-from-body: the format's ceiling,
	 * against which the ISR clamps the wire length byte (the format's body_len is zero here).
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
	radiant_op.addr_carry      = false;
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

	/* SHORTS keep the receiver armed across packets, so one window can carry several frames. RSSISTART on ADDRESS gives rssi_dbm. */
	nrf_radio_shorts_set(NRF_RADIO,
			     NRF_RADIO_SHORT_READY_START_MASK |
			     NRF_RADIO_SHORT_END_START_MASK |
			     NRF_RADIO_SHORT_ADDRESS_RSSISTART_MASK);

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	/* Cleared here too: a (D)PPI connection fires on the flag, so a stale RXREADY would sample before this receiver is live. */
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RXREADY);

	/*
	 * Both window edges in hardware: open is a compare firing TASKS_RXEN; close
	 * fires TASKS_DISABLE, late enough for a frame with t_sync == t_close to finish.
	 *
	 * OPEN takes no calibration: the radio must already be receiving from t_open
	 * minus the air lead. CLOSE takes the negated calibration, because that frame's
	 * ADDRESS event lands -T_SYNC_CAL_US after t_close. The body slack covers the rest.
	 *
	 * The tail is per PHY: at 1 M it is (body+CRC) at 8 us per byte; on the coded PHY
	 * it is CI + TERM1, then (body+CRC) at 64 us per byte, then TERM2. It is sized from
	 * the format's ceiling, since no frame has arrived yet.
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
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
		/* The search format is the only one with a 3-byte address
		 * (radiant_frame.h), the same shape test the RX diag uses
		 * for tracking's 5; this layer does not include the frame
		 * module. */
		radiant_op.page7_search = (req->fmt->addr_len == 3u);
		radiant_op.page7_close_at =
			t_sync_cal(req->t_close, -t_sync_cal_rx_us(req->fmt->phy)) +
			(radiant_time_t)tail;
#endif
	}

	/*
	 * Order matters for start_compare_missed() (program_tx() follows it). Park the compare first, since a stale
	 * CC_START may sit just ahead of the counter. Then clear the event and enable the
	 * connection, so the only compare able to set the event is the real one. The
	 * endpoints were wired at init; only the enable is per operation.
	 */
	nrf_timer_cc_set(TIMER_ADDR, CC_START, start_cc - 0x80000000u);
	nrf_timer_event_clear(TIMER_ADDR, nrf_timer_compare_event_get(CC_START));
	nrfx_gppi_conn_enable(radiant_op.conn_start);
	nrf_timer_cc_set(TIMER_ADDR, CC_START, start_cc);
	nrf_timer_cc_set(TIMER_ADDR, CC_CLOSE, close_cc);

	nrfx_gppi_conn_enable(radiant_op.conn_close);
	/* Receive only. Enabled and disabled with the others, so "which connections are live" has one answer. */
	nrfx_gppi_conn_enable(radiant_op.conn_rssi);

	if (start_compare_missed(CC_START, start_cc)) {
		/* ETIME unwinds as in the early check: the scheduler reports MISSED and retries. */
		nrfx_gppi_conn_disable(radiant_op.conn_start);
		nrfx_gppi_conn_disable(radiant_op.conn_close);
		nrfx_gppi_conn_disable(radiant_op.conn_rssi);
		nrf_radio_shorts_set(NRF_RADIO, 0);
		radiant_dbg_start_late++;
		NRF_DIAG(nrf_diag.rx_start_missed++);
		ARMDBG(3);
#if defined(LP_SLOT_GUARD)
		lp_span_fail(lp_key);
#endif
		return RADIANT_RADIO_ETIME;
	}
#if defined(LP_SLOT_GUARD)
	lp_span_end(lp_key);
#endif

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
 * radiant_radio_rx()'s class rule, plus RADIANT_RX_ELASTIC, which a booking
 * build's scheduler sets on scan chunks so a short gap is not mistaken for a
 * tracked window.
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
 * ADR 0018 (d2): book air for an operation armed later. The instants match those
 * gate_acquire() gets in rx/tx, so the later arm's air is the booked air. Only the
 * fixed class is bookable; an elastic window is the scan, which gives way.
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
	struct nrf_rx_prep prep;

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

	/* One operation in flight. The lock resolves a thread arming while the freeing callback runs: EBUSY, never corruption. */
#if defined(LP_SLOT_GUARD)
	/* The claim and its arming mark as one step against the zero-latency
	 * signal too (AN ARM PREEMPTED BY THE TIMESLOT'S END): an abort from it
	 * sees either no operation or one marked as arming. */
	uint32_t lp_ck;
#endif
	key = irq_lock();
	LP_GUARD_ONLY(lp_ck = radiant_swi_zli_lock());
	if (radiant_op.kind != OP_NONE) {
		LP_GUARD_ONLY(radiant_swi_zli_unlock(lp_ck));
		irq_unlock(key);
		return RADIANT_RADIO_EBUSY;
	}
	radiant_op.kind = OP_RX;
	CLAIM_TERMINAL();
	LP_GUARD_ONLY(LP_ARM_CLAIM());
	LP_GUARD_ONLY(radiant_swi_zli_unlock(lp_ck));
	irq_unlock(key);

	/*
	 * Refusals that depend only on the request are answered here, before the gate is
	 * asked or any register is written. The HAL requires them synchronously. This
	 * is the request's only validation: program_rx() writes what it computed.
	 */
	rc = rx_prepare(req, &prep);
	if (rc != RADIANT_RADIO_OK_RC) {
		goto fail;
	}

	/*
	 * The instants the hardware needs, not the ones the caller named: the window is
	 * live from before t_open by the ramp-up and address airtime, and closes after
	 * t_close by the rest of the longest legal frame.
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
			  * Which requests give way, from duration alone (ADR 0013): tracked windows
			  * are under a millisecond, scan chunks and ED sweeps run tens of milliseconds.
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
		 * The operation exists, but no peripheral is touched until radiant_nrf_gate_on_grant()
		 * or _on_denied() resolves it. Copied here only: the request is a stack local in the
		 * scheduler and is gone once arm returns. Not copied on the granted path.
		 */
		radiant_staged.rx = *req;
		radiant_staged.rx_prep = prep;
		/* The arm-time contents (see radiant_staged). n_filters <= caps.max_filters, checked by rx_prepare(). */
		__ASSERT_NO_MSG(req->n_filters <=
				ARRAY_SIZE(radiant_staged.rx_filters));
		radiant_staged.rx_fmt = *req->fmt;
		radiant_staged.rx.fmt = &radiant_staged.rx_fmt;
		memcpy(radiant_staged.rx_filters, req->filters,
		       (size_t)req->n_filters * sizeof(struct radiant_rx_filter));
		radiant_staged.rx.filters = radiant_staged.rx_filters;
		radiant_staged.pending = true;
		LP_GUARD_ONLY(LP_ARM_DONE());
		LP_HOOK(lp_staged());
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
		d2_settle_now();
#endif
		return RADIANT_RADIO_OK_RC;
	}

	rc = program_rx(req, &prep);
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
	/* Slot free before the mark goes: an abort from the signal in between
	 * sees no operation, never one it would deliver ABORTED to. */
	LP_GUARD_ONLY(compiler_barrier(); LP_ARM_DONE());
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

/* Burst length, a constant: it bounds the interrupt, not the dwell. */
#define ED_SAMPLES_MAX    32u
/*
 * Sample spacing on the 1 MHz TIMER. Not a settling wait (RSSI settles in ~0.25 us):
 * the finest spacing the timer can express with margin.
 */
#define ED_SAMPLE_STEP_US 2u

/* Shortest dwell: ramp-up plus a few samples. A shorter one would overrun the index ceiling the HAL forbids. */
#define ED_DWELL_MIN_US   (RAMP_UP_US + 4u * ED_SAMPLE_STEP_US)

/* Only the frequency changes between indices; mode, shorts and the empty address set were set at the arm. */
static void ed_enter_index(uint8_t rf_index)
{
	nrf_radio_frequency_set(NRF_RADIO, (uint16_t)(2400u + rf_index));
	/* Cleared before the task: a stale RXREADY would be taken for this index before the receiver ramped. */
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RXREADY);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_RXEN);
}

/*
 * One index's receiver is live: take the burst, report it, move on. Runs in the
 * radio interrupt, so all of it is bounded by ED_SAMPLES_MAX except radio_disable_now().
 */
static void ed_dwell(void)
{
	struct radiant_ed_event evt;
	int32_t  sum = 0;
	int8_t   min_dbm = 0;
	uint16_t n = 0u;
	uint8_t  rf = radiant_op.ed_cur;
	uint32_t i;

	/* The first index started from a compare, later ones by software. Disable the connection so the compare cannot fire again after a counter wrap. */
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

		/* Tested after the first sample, so an index always measures. A zero-sample OK event would be dropped as a hole. */
		if ((int32_t)(timer_capture(CC_NOW) -
			      radiant_op.ed_deadline_cc) >= 0) {
			break;
		}
	}

	/* The receiver is off before the callback: the callback may arm the next operation, and a DISABLED in flight would end it. */
	radio_disable_now();

	memset(&evt, 0, sizeof(evt));
	evt.op = radiant_op.id;
	evt.status = RADIANT_RADIO_STATUS_OK;
	evt.rf_index = rf;
	evt.min_dbm = min_dbm;
	/* Division truncates toward zero, i.e. toward the loud end for negative dBm: a conservative error for picking quiet frequencies. */
	evt.mean_dbm = (int8_t)(sum / (int32_t)n);
	evt.samples = n;
	core_cb_ed(&evt);

	/* The callback may abort or replace this operation, as the scheduler does when a tracked window falls due. */
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
#if defined(LP_SLOT_GUARD)
/* program_ed() found its timeslot gone (LP_SLOT_GUARD): nothing written. */
static bool lp_ed_lost;
#endif
#endif

int radiant_radio_ed(const struct radiant_ed_req *req, uint32_t *op)
{
#if !defined(CONFIG_RADIANT_ED_SCAN)
	(void)req;
	if (op != NULL) {
		*op = 0u;
	}
	/* Backstop: caps.has_ed_scan is false here, so the core should not reach this. */
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
	/* No consumer, no operation: a sweep whose events have nowhere to go would still occupy the radio. */
	if (radiant_op.cbs == NULL || radiant_op.cbs->ed == NULL) {
		return RADIANT_RADIO_ENOTSUP;
	}

	/* One operation in flight, resolved as in the other arms. */
#if defined(LP_SLOT_GUARD)
	/* The claim and its arming mark as one step against the zero-latency
	 * signal too (AN ARM PREEMPTED BY THE TIMESLOT'S END): an abort from it
	 * sees either no operation or one marked as arming. */
	uint32_t lp_ck;
#endif
	key = irq_lock();
	LP_GUARD_ONLY(lp_ck = radiant_swi_zli_lock());
	if (radiant_op.kind != OP_NONE) {
		LP_GUARD_ONLY(radiant_swi_zli_unlock(lp_ck));
		irq_unlock(key);
		return RADIANT_RADIO_EBUSY;
	}
	radiant_op.kind = OP_ED;
	CLAIM_TERMINAL();
	LP_GUARD_ONLY(LP_ARM_CLAIM());
	LP_GUARD_ONLY(radiant_swi_zli_unlock(lp_ck));
	irq_unlock(key);

	/* Test against what the hardware needs (ramp-up alone, there is no address), not the advertised worst case. See radiant_radio_rx(). */
	now = radiant_radio_now();
	if (req->t_start < now + (radiant_time_t)RAMP_UP_US) {
		rc = RADIANT_RADIO_ETIME;
		goto fail;
	}

	/*
	 * An ED chunk spans (hi - lo + 1) * dwell, and the reservation is exactly that.
	 * No follow-on: nothing follows an energy-detect index at a fixed instant.
	 * PRIO_NORMAL (ADR 0013): ED takes gaps only and gives way to the other stack.
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
		LP_GUARD_ONLY(LP_ARM_DONE());
		LP_HOOK(lp_staged());
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
		d2_settle_now();
#endif
		return RADIANT_RADIO_OK_RC;
	}

	program_ed(req);
#if defined(LP_SLOT_GUARD)
	if (lp_ed_lost) {
		rc = RADIANT_RADIO_ETIME;
		*op = 0u;
		gate_release();
		goto fail;
	}
#endif
	LOG_DBG("ed op=%u rf=%u..%u dwell=%u start=+%d",
		(unsigned)radiant_op.id, (unsigned)req->rf_index_lo,
		(unsigned)req->rf_index_hi, (unsigned)req->dwell_us,
		(int)(int64_t)(req->t_start - now));
	return RADIANT_RADIO_OK_RC;

fail:
	LOG_DBG("ed refused: %d", rc);
	radiant_op.kind = OP_NONE;
	/* Slot free before the mark goes: an abort from the signal in between
	 * sees no operation, never one it would deliver ABORTED to. */
	LP_GUARD_ONLY(compiler_barrier(); LP_ARM_DONE());
	return rc;
#endif /* CONFIG_RADIANT_ED_SCAN */
}

#if defined(CONFIG_RADIANT_ED_SCAN)
/* Everything an energy-detect chunk does to the peripheral, from the staged
 * request. See program_rx() for why it is a function. */
static void program_ed(const struct radiant_ed_req *req)
{
	uint32_t start_cc;
#if defined(LP_SLOT_GUARD)
	bool lp_ok;
	const uint32_t lp_key = lp_span_begin(&lp_ok);

	lp_ed_lost = !lp_ok;
	if (!lp_ok) {
		/* Its timeslot ended since the claim: radiant_radio_ed() fails. */
		lp_span_fail(lp_key);
		return;
	}
#endif

	radiant_op.ed_hi = req->rf_index_hi;
	radiant_op.ed_cur = req->rf_index_lo;
	radiant_op.ed_dwell_us = req->dwell_us;
	radiant_op.terminal_sent = false;
	radiant_op.rx_any = false;

	/* MODE, RXADDRESSES and (per index, ed_enter_index()) FREQUENCY change under the next receive's feet. */
	reg_shadow_forget();
	nrf_radio_mode_set(NRF_RADIO, NRF_RADIO_MODE_NRF_1MBIT);
	/*
	 * No logical address is enabled, so the matcher cannot fire: no ADDRESS, DMA or END.
	 * That makes this a measurement, not a receive window. PACKETPTR still points at a
	 * legal buffer, since a null DMA pointer is a fault waiting to happen.
	 */
	NRF_RADIO->RXADDRESSES = 0u;
	NRF_RADIO->PACKETPTR = (uint32_t)(uintptr_t)radiant_rx_buf;
	/* READY_START alone: the receiver must reach RX for RSSI to mean anything. No END_START: there is no packet to chain to. */
	nrf_radio_shorts_set(NRF_RADIO, NRF_RADIO_SHORT_READY_START_MASK);

	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);
	nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_RXREADY);
	nrf_radio_frequency_set(NRF_RADIO,
				(uint16_t)(2400u + radiant_op.ed_cur));

	/*
	 * RXREADY is an interrupt for this operation only, removed again in deliver_terminal().
	 * Left enabled it would add an interrupt to every ordinary receive window.
	 */
	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_RXREADY_MASK);

	start_cc = clock_counter_at(req->t_start - (radiant_time_t)RAMP_UP_US);
	radiant_op.ed_deadline_cc = start_cc + req->dwell_us;
	nrf_timer_cc_set(TIMER_ADDR, CC_START, start_cc);
	nrfx_gppi_conn_enable(radiant_op.conn_start);
	/* conn_close is deliberately not enabled: an index ends when its burst does, and a close compare would cut it short. */
#if defined(LP_SLOT_GUARD)
	lp_span_end(lp_key);
#endif
}
#endif /* CONFIG_RADIANT_ED_SCAN */

/*
 * What the other stack does to the RADIO inside our grant. nrf_802154_trx_disable()
 * reaches nrf_radio_reset() on nRF54L: every RADIO SUBSCRIBE and PUBLISH register is
 * zeroed and TASKS_SOFTRESET fires. The MPSL work queue thread can run this at any
 * point, so it may land inside our own timeslot.
 *
 * It does not cost tracked packets: short windows beside an OpenThread leader kept
 * their routing. It does reach the long sweep windows, costing discovery speed under
 * a second stack. That is a known, unfixed leak, not tracked loss.
 *
 * Nothing is repaired or held here: an in-grant check is only ever a check at one
 * instant. The loss that mattered was a stale MPSL_TIMER0 compare ending each grant
 * early; see radiant_radio_nrf_gate_mpsl.c.
 */

/*
 * What the gate calls back. Unreachable in the direct build (GATE_PENDING cannot
 * occur there), but the gate header declares them. The low-power gate
 * (CONFIG_RADIANT_NRF_LOW_POWER, radiant_radio_nrf_lp.c) calls on_grant() and
 * finish_failed() after its wake, from the GRTC or clock interrupt.
 */

void radiant_nrf_gate_on_grant(void)
{
	unsigned int key;
	int          rc = RADIANT_RADIO_OK_RC;

	key = irq_lock();
	if (!radiant_staged.pending) {
		/*
		 * A grant for an operation that already ended (aborted, or resolved first).
		 * Given straight back: programming for an operation nobody awaits would arm
		 * the radio against a window with no owner.
		 */
		irq_unlock(key);
		gate_release();
		return;
	}
	radiant_staged.pending = false;
	irq_unlock(key);

	/*
	 * Take the peripheral for this grant only: the interrupt mask and the RADIO's end
	 * of our (D)PPI connections. See radiant_radio_enable() and radio_endpoints_attach().
	 * The loan starts at the first shared-register write, not at SIGNAL_START: the early
	 * returns above leave the peripheral untouched, and on_grant_end() must not hand
	 * back a radio that was never borrowed.
	 */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
	radio_on_loan = true;
	radio_lent_this_slot = true;
#endif
	/* The other stack had the RADIO until now: nothing in it is known (see RADIANT_NRF_REG_SHADOW). */
	reg_shadow_forget();
	nrf_radio_int_enable(NRF_RADIO, NRF_RADIO_INT_END_MASK |
					NRF_RADIO_INT_DISABLED_MASK);

#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
	/*
	 * Take the NVIC line for the grant too, not just the mask. The vector is RADIO_0_IRQn,
	 * owned by MPSL (zero latency), which turns it into the radio timeslot signal. mpsl.rst
	 * says the application enables and disables the RADIO interrupt itself on nRF54L. The
	 * 802.15.4 driver NVIC-disables the same line at its own teardown, so the enable from
	 * startup would be gone after its first stop. The window would then run to the grant's
	 * end with no interrupt and be reported DONE_DENIED.
	 *
	 * So the line is taken for the grant and restored to its prior state, like the endpoint
	 * registers. Between grants it belongs to MPSL and the 802.15.4 driver.
	 */
	radio_irq_was_enabled = (NVIC_GetEnableIRQ(RADIANT_RADIO_IRQn) != 0U);
	if (!radio_irq_was_enabled) {
		NVIC_EnableIRQ(RADIANT_RADIO_IRQn);
	}
#endif

	/*
	 * Take the RADIO endpoints another stack holds, for this grant only. A no-op unless
	 * another stack held one at init (see radio_endpoints_attach()). Runs before
	 * program_rx()/program_tx(), since an endpoint still carrying the other channel never opens.
	 * A register another driver treats as write-once must be restored on every exit path.
	 * The init-time borrow is separate: it is bounded to init and hands registers straight back.
	 */
	radio_endpoints_attach(true);

	switch (radiant_op.kind) {
	case OP_RX:
		LP_GUARD_ONLY(lp_arm_gen = lp_slot_gen());
		rc = program_rx(&radiant_staged.rx, &radiant_staged.rx_prep);
		break;
	case OP_TX:
		LP_GUARD_ONLY(lp_arm_gen = lp_slot_gen());
		rc = program_tx(&radiant_staged.tx, &radiant_staged.tx_regs);
		break;
#if defined(CONFIG_RADIANT_ED_SCAN)
	case OP_ED:
		LP_GUARD_ONLY(lp_arm_gen = lp_slot_gen());
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
		 *
		 * The grant is released now, not held to its TIMER0 end: nothing is
		 * programmed (the ETIME paths leave no connection enabled and no
		 * SHORTS set), so its remaining air is the other stack's. SIGNAL_START
		 * sees release_wanted and ends the timeslot at once; the hand-back
		 * (radio_endpoints_attach(false)) runs from that path, and
		 * end_housekeeping() has denied_work_fn() report FAILED from thread
		 * context as before.
		 */
		radiant_op.gate_failed = true;
		ARMDBG(1);
		gate_release();
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
 * The RADIO's own PUBLISH_* / SUBSCRIBE_* registers, attached and detached with the grant.
 * conn_enable()/conn_disable() only toggle the DPPIC channel, so these registers keep
 * whatever alloc wrote. Under arbitration they belong to the timeslot holder: left set,
 * the other stack's packets would publish onto our channel.
 *
 * So they are written on grant entry and cleared on exit. The DPPI channels stay
 * allocated across the boundary, since per-grant allocation can fail. The values are
 * saved, not rebuilt: nrfx_gppi_conn_alloc() returns an opaque handle, and reading back
 * what alloc wrote cannot disagree with it.
 */
static void radio_endpoints_save(void)
{
#if !RADIANT_NRF_RADIO_EP
	/*
	 * A PPI part has no endpoint register to save (see RADIANT_NRF_RADIO_EP). The gate
	 * can be built on nRF52840 too, so the silicon guard is needed, or the register
	 * accesses below fail to compile. `saved` stays false, making the swap inert.
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
	 * The swap set: the endpoints this backend programmed, not every endpoint another
	 * stack held at init. A register we never wrote stays out, so the swap moves only our
	 * own routing. Logged, because an inert swap looks like a healthy gate that hears nothing.
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
 * Which registers are swapped, and whose value goes back, is decided at each grant,
 * not at init. Deciding at init from radio_ep_contended is a boot-order race with a
 * silent failure on the losing side. If RadiANT initialises first, the mask is empty,
 * the 802.15.4 driver overwrites our PUBLISH_ADDRESS and SUBSCRIBE_RXEN directly,
 * the receiver never ramps, and no terminal arrives.
 *
 * So the set is radio_ep_mine_mask (the registers this backend programmed), and the
 * value restored is whatever the register held at this grant's entry. Restoring zero
 * is wrong: it would drop our always-live t_sync capture, and zero is not what the
 * 802.15.4 driver wrote to SUBSCRIBE_RXEN at its init.
 *
 * With no second stack the entry value is our own, so every write is a no-op.
 */
static void radio_endpoints_attach(bool on)
{
	if ((RADIANT_NRF_GRANT_EP_SWAP == 0) || !radio_ep.saved ||
	    radio_ep_mine_mask == 0U) {
		return;
	}
#if !RADIANT_NRF_RADIO_EP
	/*
	 * Unreachable on a PPI part (`saved` is false and the mask is zero). Kept because
	 * radio_ep_reg() does not exist on that silicon.
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
			/* Read before write: learn the other stack's current value each time. */
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
	 * Nothing below may run for a timeslot that never took the peripheral. This is
	 * reached via g.hw_held, which is also set for the bootstrap timeslot. That one
	 * never calls on_grant(), so handing back its radio would clear SHORTS and force
	 * DISABLE on a radio the 802.15.4 driver is using (see RADIO_IS_OURS()). The gate's
	 * own counter (stats.grant_end_calls) is left alone.
	 */
	if (!RADIO_IS_OURS()) {
		return;
	}

	/*
	 * Hand back the interrupt mask. Bits left set in RADIO->INTENSET fire while another
	 * stack transmits, and the SoftDevice Controller asserts within milliseconds (see
	 * radiant_radio_enable()). This runs on every path that ends a grant.
	 *
	 * Exactly our own bits, never ~0: the register is shared and line 0 is MPSL's.
	 */
	nrf_radio_int_disable(NRF_RADIO, NRF_RADIO_INT_END_MASK |
					 NRF_RADIO_INT_DISABLED_MASK |
					 NRF_RADIO_INT_RXREADY_MASK);
	/* The other stack has the RADIO from here (see RADIANT_NRF_REG_SHADOW). */
	reg_shadow_forget();

	/*
	 * Hand the radio back idle, not merely quiet: the SoftDevice Controller picks it up
	 * microseconds later and expects DISABLED. Left-over SHORTS (END->DISABLE from
	 * program_rx()) would chain the controller's next packet into a disable of its radio.
	 *
	 * Order matters: the mask is cleared first so this DISABLED is not taken as a
	 * terminal, and SHORTS are cleared before the disable so nothing chains.
	 */
	nrf_radio_shorts_set(NRF_RADIO, 0);
	radio_disable_now();

	/*
	 * The pending NVIC bit is NOT cleared here, but it is not harmless either. A RADIO
	 * event (END, DISABLED) that fires while a TIMER0 or RADIO signal is running, at
	 * the same priority, latches RADIO_0 pending, and clearing the mask above does not
	 * unlatch it. Under MPSL the vector is MPSL's: taken after our ACTION_END has
	 * closed the timeslot, it is a RADIO interrupt with no timeslot open, and MPSL
	 * asserts 69:108 (observed: ipsr=RADIO_0, 18 us after END). So the gate clears it,
	 * inside the timeslot, as the last radio act before ACTION_END
	 * (radiant_nrf_gate_radio_irq_unpend()). Not here, because this also runs on the
	 * low-priority hand-backs (BLOCKED, SESSION_IDLE, SESSION_CLOSED, gate_shutdown()),
	 * where the radio may already be the other stack's and the bit its interrupt.
	 */

	/*
	 * Known gap: the (D)PPI endpoints are not detached here. Detaching them all
	 * unconditionally stopped the receive path dead (every window ran to full
	 * length). radio_endpoints_attach(false) restores only the endpoints another
	 * stack held at init, which is empty without a second stack. Beside OpenThread
	 * the mask is PUBLISH_ADDRESS and SUBSCRIBE_RXEN, neither needed at this
	 * instant. Unproven either way.
	 */
	radio_endpoints_attach(false);

#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
	/* Restore the vector as the grant found it. Last, so nothing we raised is taken on the way out. */
	if (!radio_irq_was_enabled) {
		NVIC_DisableIRQ(RADIANT_RADIO_IRQn);
	}
	/* The loan ends here. Later callers find the radio not ours (see RADIO_IS_OURS()). */
	radio_on_loan = false;
#endif
}

#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
void radiant_nrf_gate_on_slot_start(void)
{
	radio_lent_this_slot = false;
}

bool radiant_nrf_gate_radio_irq_unpend(void)
{
	bool was_pending;

	/*
	 * Only for a timeslot that took the radio. radio_lent_this_slot is cleared at
	 * every SIGNAL_START (radiant_nrf_gate_on_slot_start()) and set only by this
	 * timeslot's on_grant(), so it never carries over from a timeslot that ended
	 * without our high-priority END. A bootstrap or orphan timeslot never enabled a
	 * RADIO interrupt, so a pending bit then cannot be ours (the 802.15.4 driver
	 * clears its own at hand-over, nrf_802154_trx.c, after nrf_radio_reset()). By now
	 * on_grant_end() has cleared our INTENSET bits and disabled the radio, so no
	 * source of ours can re-pend the line before ACTION_END is returned.
	 */
	if (!radio_lent_this_slot) {
		return false;
	}
	radio_lent_this_slot = false;
	was_pending = (NVIC_GetPendingIRQ(RADIANT_RADIO_IRQn) != 0U);
	NVIC_ClearPendingIRQ(RADIANT_RADIO_IRQn);
	return was_pending;
}
#endif

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
	/* The handler the NVIC would have entered, called by hand. radio_isr() is idempotent across both RADIO lines. */
	radio_isr(NULL);
}

void radiant_nrf_gate_on_denied(void)
{
	unsigned int key;

#if defined(LP_SLOT_GUARD)
	/*
	 * The gate's deferred denial (end_housekeeping() -> denied_work_fn(), a
	 * cooperative thread) preempting an arm made from a preemptible thread:
	 * nothing is staged (g.pending was false), the operation in the slot is
	 * that arm's, and the arm settles it itself (ETIME at its span, or OK and
	 * live). An arm needs a free slot to claim, so the operation the denial
	 * was for has already had its terminal.
	 */
	if (lp_arming) {
		return;
	}
#endif
	key = irq_lock();
	radiant_staged.pending = false;
	irq_unlock(key);

	/*
	 * Delivered unconditionally, never guarded on radiant_staged.pending: an operation
	 * can have its staging cleared by an unrelated terminal and still be the one the
	 * core awaits. deliver_terminal() is idempotent, which guarantees exactly one
	 * terminal per accepted arm. No register was touched, so this is a clean terminal.
	 * May run in a cooperative thread.
	 */
	deliver_terminal(RADIANT_RADIO_STATUS_DENIED);
}

/* ---- Abort ---- */

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
#if defined(LP_SLOT_GUARD)
	/*
	 * The timeslot's end (EXTEND_FAILED, the extension skip) aborting an
	 * arm still in progress underneath it: nothing of it is programmed, and
	 * the arm fails ETIME at its span (AN ARM PREEMPTED BY THE TIMESLOT'S
	 * END). Leaving it to the arm gives the operation one outcome.
	 */
	if (lp_arming && gate_in_signal()) {
		irq_unlock(key);
		return RADIANT_RADIO_OK_RC;
	}
#endif

	/* Take the hardware edges away first, or a compare can re-fire against a retired operation. */
	nrfx_gppi_conn_disable(radiant_op.conn_start);
	nrfx_gppi_conn_disable(radiant_op.conn_start_tx);
	nrfx_gppi_conn_disable(radiant_op.conn_close);
	nrfx_gppi_conn_disable(radiant_op.conn_rssi);
	/* SHORTS and TASKS_STOP are touched only while a grant is held (see RADIO_IS_OURS()). */
	if (RADIO_IS_OURS()) {
		/* The late-ISR guard's carry is an ADDRESS it consumed for a frame still on air. */
		NRF_DIAG(if (radiant_op.kind == OP_RX && radiant_op.page7_search &&
			     (radiant_op.addr_carry ||
			      nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS)) &&
			     !nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END)) {
				nrf_diag.srch_cut_abort++;
			});
		nrf_radio_shorts_set(NRF_RADIO, 0);
		nrf_radio_task_trigger(NRF_RADIO, NRF_RADIO_TASK_STOP);
	}
	/* A staged, unprogrammed operation has nothing to tear down. Clearing the flag under the lock stops a grant in flight from arming it. */
	was_staged = radiant_staged.pending;
	radiant_staged.pending = false;
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL) || defined(CONFIG_RADIANT_NRF_LOW_POWER)
	/*
	 * A FAILED still owed for a grant whose programming failed belongs to this
	 * operation, which ends ABORTED below. Left set, denied_work_fn() would
	 * deliver it to whatever operation is armed next.
	 */
	if (radiant_op.gate_failed) {
		radiant_op.gate_failed = false;
		ARMDBG(0);
	}
#endif
	irq_unlock(key);
	/* Owed, not paid, when aborted from the core's own completion (see the grant handoff note). */
	grant_release_after_core();
	/* The staged request now has no owner. */
	if (was_staged) {
		gate_forget_pending();
	}

	/*
	 * The slot is released synchronously. If DISABLE were left in flight, the caller
	 * that matters (radiant_transfer.c, arming its acknowledgement from the receive
	 * callback with a 1560 us deadline) would be refused EBUSY and miss its reply.
	 * So the disable completes and its event is consumed here, and the terminal is
	 * delivered from here. deliver_terminal() is idempotent, so this is not a double delivery.
	 */
	/* Only if the radio is ours: forcing DISABLE on another stack's live receiver is the same mistake as the SHORTS write above. */
	if (RADIO_IS_OURS()
#if defined(CONFIG_RADIANT_NRF_LOW_POWER)
	    /* Staged behind a low-power wake: the RADIO was never touched. */
	    && !was_staged
#endif
	    ) {
		radio_disable_now();
	}
	deliver_terminal(RADIANT_RADIO_STATUS_ABORTED);

	return RADIANT_RADIO_OK_RC;
}

/* ---- Bench diagnostic: die temperature (thread context only; see radiant_radio_nrf_diag.h) ---- */

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

/* ---- The interrupt ---- */

/*
 * Stop the radio and consume the EVENTS_DISABLED it raises, before any completion
 * callback runs. A DISABLED left in flight would be taken as the terminal of the
 * next operation that a callback arms at once (radiant_transfer.c does this for
 * the acknowledgement window), which would silently tear that window down. Ramp-down
 * takes microseconds. The bound is a backstop against this backend's own 1 MHz TIMER.
 */
#define RADIO_DISABLE_MAX_US 100u

static void radio_disable_now(void)
{
	uint32_t deadline;

#if defined(CONFIG_RADIANT_NRF_LOW_POWER)
	/* Asleep nothing is programmed, and the backstop below would spin on a
	 * stopped TIMER. */
	if (lp_timer_stopped()) {
		return;
	}
#endif
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

/* Every operation ends here, through exactly one callback. The kind is read before the slot is released. */
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
	 * The four disables above touch our own DPPIC channels and are safe from any context.
	 * RADIO->SHORTS is not ours between grants: a DENIED terminal runs from the gate's
	 * work queue with no timeslot held, and clearing SHORTS there cut the 802.15.4
	 * driver's PHYEND->DISABLE short mid-frame. Skipping it loses nothing, since our
	 * shorts exist only inside a grant.
	 */
	if (RADIO_IS_OURS()) {
		nrf_radio_shorts_set(NRF_RADIO, 0);
	}

	radiant_op.kind = OP_NONE;
	/*
	 * Give the air back promptly, so a tracked window that closed empty does not hold
	 * its follow-on reservation for a reply that will not come. Not before the callback
	 * has had its chance to arm a follow-on into that reservation (see the grant handoff note).
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
		/* No measurement on a terminal event. Indices measured earlier were already reported. */
		core_cb_ed(&evt);
		LP_HOOK(lp_terminal_done());
		return;
	}
#endif

	if (kind == OP_TX) {
		struct radiant_tx_event evt;

		memset(&evt, 0, sizeof(evt));
		evt.op = radiant_op.id;
		evt.status = st;
		/* Nothing went out, so CC_SYNC holds an earlier operation's capture. Report it as inexact. */
		evt.t_sync = radiant_radio_now();
		evt.t_sync_exact = false;
		core_cb_tx(&evt);
	} else {
		struct radiant_rx_event evt;

#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
		rx_diag_window_end(st != RADIANT_RADIO_STATUS_TIMEOUT);
#endif
		/* "OK" means the window heard any frame, however it ended. */
		NRF_DIAG(if (radiant_op.rx_any) {
				nrf_diag.term_ok++;
			} else if (st == RADIANT_RADIO_STATUS_TIMEOUT) {
				nrf_diag.term_empty++;
			} else {
				nrf_diag.term_error++;
			});
		memset(&evt, 0, sizeof(evt));
		evt.op = radiant_op.id;
		evt.status = st;
		evt.t_sync = radiant_radio_now();
		evt.t_sync_exact = false;

		/*
		 * Noise floor: RSSI sampled in a window with no packet. Each condition matters:
		 * TIMEOUT only, since only a window that ran to close surely sampled this window
		 * (RSSISAMPLE has no "invalid" state); nothing received, since a packet overwrites
		 * RSSISAMPLE; and the same correction as rssi_dbm, so the two share one scale.
		 */
		if (st == RADIANT_RADIO_STATUS_TIMEOUT && !radiant_op.rx_any) {
			evt.noise_dbm = rssi_sample_dbm();
			evt.has_noise = true;
		}

		core_cb_rx(&evt);
	}
	/* After the callback: it may have armed the next operation. */
	LP_HOOK(lp_terminal_done());
}

/*
 * Slack in the late-ISR guard's 1 M airtime. Both ends of `now - CC_SYNC` are
 * 1 MHz counts, so the difference can read up to 1 us short of the real
 * ADDRESS-to-END time, and the (D)PPI capture lands a fraction of a microsecond
 * after the event. 2 us keeps an on-time interrupt from ever reading as torn
 * (it already runs some microseconds after END). It is a reasoned figure, not
 * a measurement: page 10 field 4 near 0 on the bench is its check.
 */
#define RX_TORN_SLACK_US 2u

/*
 * radiant_rx_torn()'s air_us for the receive in flight: the least time from a
 * frame's ADDRESS capture to its END. On 1 M that is the software address
 * bytes, the body and the CRC at 8 us a byte, less RX_TORN_SLACK_US. A
 * length-from-body format (the coded PHY's) takes its shortest legal frame,
 * the length byte alone, at 64 us a byte (S=8) with CI, TERM1 and TERM2 left
 * out, so it is a lower bound there too. Computed from the latched operation,
 * a few instructions, so the ISR reads no register for it.
 */
static uint32_t rx_torn_air_us(void)
{
	uint32_t body = radiant_op.len_from_body ? 1u : (uint32_t)radiant_op.body_len;
	uint32_t bytes = (uint32_t)radiant_op.sw_addr_len + body + 2u;

	if (radiant_op.phy == RADIANT_PHY_LR_CODED) {
		return bytes * 64u;
	}
	return bytes * US_PER_BYTE - RX_TORN_SLACK_US;
}

/*
 * How long after its nominal ADDRESS-to-END airtime a 1 M frame's END is
 * surely raised: 1 us for the quantisation of the two 1 MHz counts, 1 us for
 * the radio's END after the last CRC bit. A reasoned figure, like the slack.
 */
#define RX_TORN_SETTLE_US 2u

/*
 * The settle wait's bound. A capture that passed the time test is at least
 * airtime - RX_TORN_SLACK_US old, and the wait ends at airtime +
 * RX_TORN_SETTLE_US, so it never needs more than their sum. The bound is
 * measured on the same TIMER, so it is a ceiling on the wait, not a guard
 * against a stalled clock (that TIMER never stops; radio_disable_now() relies
 * on it the same way). The +RX_TORN_SETTLE_US end point assumes END follows
 * the nominal airtime within ~1 us: a rising radiant_dbg_rx_no_addr under
 * load is the sign it does not.
 */
#define RX_TORN_WAIT_MAX_US (RX_TORN_SLACK_US + RX_TORN_SETTLE_US)
BUILD_ASSERT(RX_TORN_WAIT_MAX_US <= 4u, "the settle wait runs in the RADIO ISR");

/*
 * The late-ISR guard's settle wait (1 M only; see the END branch). A capture in
 * the band [airtime - slack, airtime + settle) could be N's or N+1's in its
 * last microseconds, so spin on the clock until the frame it dates must have
 * raised END. An on-time interrupt usually starts past the band and does not
 * spin at all.
 */
static void rx_torn_settle(uint32_t sync, uint32_t t_snap)
{
	const uint32_t settled = rx_torn_air_us() + RX_TORN_SLACK_US +
				 RX_TORN_SETTLE_US;
	uint32_t now = t_snap;

	while ((uint32_t)(now - sync) < settled &&
	       (uint32_t)(now - t_snap) < RX_TORN_WAIT_MAX_US) {
		now = timer_capture(CC_NOW);
	}
}

static void radio_isr(const void *arg)
{
	ARG_UNUSED(arg);
	radiant_dbg_isr++;

#if defined(CONFIG_RADIANT_ED_SCAN)
	/*
	 * RXREADY is unmasked only during an ED sweep, so this is one flag read on other
	 * interrupts. The kind test is still needed: a stale RXREADY can be pending after
	 * the operation has retired, and a burst would then sample a receiver that is off.
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
			 * The frame is out. Delivered from END, not the DISABLED that follows, because
			 * the slot must be free before the callback can arm the next operation.
			 */
			radiant_op.terminal_sent = true;
			radiant_dbg_term++;
			nrfx_gppi_conn_disable(radiant_op.conn_start_tx);
			nrf_radio_shorts_set(NRF_RADIO, 0);
			/* Consumed here, not left in flight (see radio_disable_now()). */
			radio_disable_now();

			memset(&evt, 0, sizeof(evt));
			evt.op = radiant_op.id;
			evt.status = RADIANT_RADIO_STATUS_OK;
			/* A hardware capture, as on receive: ADDRESS fires once the address is sent, so a master closes against what went out. */
			evt.t_sync = t_sync_cal(
				clock_absolute(
					nrf_timer_cc_get(TIMER_ADDR, CC_SYNC)),
				T_SYNC_CAL_TX_US);
			evt.t_sync_exact = true;

			/* Achieved minus requested t_sync: this backend's TX timing error in microseconds. Zero is the target. */
			radiant_dbg_tx_err =
				(int32_t)(int64_t)(evt.t_sync -
						   radiant_op.t_sync_req);

			radiant_op.kind = OP_NONE;
#if defined(CORE_CB_DEFERRED)
			/* This completion is a terminal too, so it owes the release deliver_terminal() would have made. */
			grant_release_after_core();
#endif
			core_cb_tx(&evt);
			/* A terminal outside deliver_terminal(): the same hook. */
			LP_HOOK(lp_terminal_done());
		}

		/*
		 * THE LATE-ISR GUARD. END->START keeps the receiver running, so the next
		 * frame (N+1) is DMA'd into radiant_rx_buf as soon as its ADDRESS matches,
		 * and that ADDRESS re-captures CC_SYNC. This interrupt can run late: on the
		 * direct build a thread-context scheduler pass holds interrupts off for up
		 * to ~215 us. So the buffer and every register the frame is judged by
		 * (CC_SYNC, CRCSTATUS, RXCRC, RXMATCH, RSSISAMPLE) are read once, into
		 * locals and radiant_rx_snap, between two CC_SYNC reads, and nothing below
		 * the snapshot reads the buffer or those registers again.
		 *
		 * Order: the ADDRESS test, then the ADDRESS clear, then CC_SYNC (read 1),
		 * the registers, the copy, CC_SYNC (read 2), the clock, the torn test,
		 * the settle wait, the END check. Where N+1's ADDRESS can land:
		 *  (a) Before the clear (before entry, or between the test and the
		 *      clear). The clear wipes its flag. Its capture has reached CC_SYNC
		 *      by read 1, or at the latest by read 2: (D)PPI moves it within a
		 *      cycle or two of the 16 MHz clock, and the register reads and the
		 *      copy between the clear and read 2 take longer than that.
		 *      - Capture in read 1, N+1 still on air: less than its airtime has
		 *        passed since the capture, so radiant_rx_torn() says torn. N is
		 *        dropped and counted, and the ADDRESS is carried.
		 *      - Capture only in read 2: the reads differ, torn, carried.
		 *      - N+1 ended after the END clear at the top of radio_isr(): END is
		 *        raised again by the END check. The registers may predate that
		 *        END, so the snapshot is retaken (rx_end_again, carried) and N+1
		 *        is delivered from a consistent one; N is counted lost.
		 *      - N+1 ended before that clear: one END event for both frames, and
		 *        buffer, registers and capture are all N+1's. N+1 is delivered
		 *        and N is lost unseen, as before this guard; nothing can tell.
		 *  (b) Between the clear and read 1. EVENTS_ADDRESS stays latched for
		 *      N+1. Read 1 or read 2 holds its capture, so the snapshot is torn
		 *      (conservatively: N+1's bytes may not have landed yet, but N's
		 *      registers cannot be told from N+1's). The carry is set as well,
		 *      which is redundant with the latched flag; N+1's END consumes both.
		 *  (c) Between read 1 and read 2 (around the copy). The reads differ:
		 *      torn. The flag is latched and the carry set.
		 *  (d) After read 2. The copy ended before N+1's first byte could land
		 *      (the DMA follows the ADDRESS by at least a byte time), so the
		 *      snapshot is N's and is delivered. The flag stays latched for N+1;
		 *      no carry is needed.
		 * So no case depends on when the capture lands after the clear, and there
		 * is no third CC_SYNC read.
		 *
		 * The settle wait closes the one band the time test cannot judge: a
		 * capture between airtime - RX_TORN_SLACK_US and airtime +
		 * RX_TORN_SETTLE_US could be N's (an on-time interrupt) or N+1's in its
		 * last microseconds, whose END is not yet raised and whose CRC registers
		 * would then be N's. In that band only, the branch spins on the clock
		 * until the frame the capture dates must have ended (at most
		 * RX_TORN_WAIT_MAX_US) before the END check, so N+1 raises END and the
		 * snapshot is retaken. 1 M only: a length-from-body (coded) frame has no
		 * fixed airtime, so there the time test uses the shortest legal frame and
		 * a longer N+1 still on air past it is the known residual.
		 *
		 * DISABLED (the window's close) is handled after this branch, as before. A
		 * frame it cut off mid-air shows there as EVENTS_ADDRESS or the carry, and a
		 * carried frame whose END never comes is forgotten at the next arm.
		 */
rx_end_again:
		if (radiant_op.kind == OP_RX && !radiant_op.terminal_sent &&
		    !radiant_op.addr_carry &&
		    !nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS)) {
			/*
			 * An END without an ADDRESS match in this window is not a reception. The body,
			 * CRCSTATUS and the CC_SYNC capture all keep their previous values, so delivering
			 * it would replay the last frame with a stale timestamp. ADDRESS is the right test:
			 * it is cleared at arm time, raised by hardware on a sync-word match, and drives the capture.
			 * The carry stands in for an ADDRESS the guard below consumed early.
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
			uint32_t sync;
			bool crc_ok;
			uint32_t rxcrc;
			uint32_t rssisample;
			uint8_t logical;
			const size_t snap_len =
				MIN((size_t)radiant_op.sw_addr_len + radiant_op.body_len,
				    sizeof(radiant_rx_snap));
			uint32_t t_snap;
			bool torn;
			bool end_again;

			/* Consumed first, so the next packet in this window is judged on its own match (END->START keeps the receiver armed). */
			radiant_op.addr_carry = false;
			nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_ADDRESS);

			/* The snapshot: CC_SYNC first, then the registers, then the buffer. */
			sync = nrf_timer_cc_get(TIMER_ADDR, CC_SYNC);
			crc_ok = nrf_radio_crc_status_check(NRF_RADIO);
			rxcrc = NRF_RADIO->RXCRC;
			rssisample = NRF_RADIO->RSSISAMPLE;
			logical = (uint8_t)(NRF_RADIO->RXMATCH & RADIO_RXMATCH_RXMATCH_Msk);
			memcpy(radiant_rx_snap, radiant_rx_buf, snap_len);
			{
				const uint32_t sync_after =
					nrf_timer_cc_get(TIMER_ADDR, CC_SYNC);

				t_snap = timer_capture(CC_NOW);
				torn = radiant_rx_torn(sync, sync_after, t_snap,
						       rx_torn_air_us());
			}
			if (!torn && radiant_op.phy != RADIANT_PHY_LR_CODED) {
				rx_torn_settle(sync, t_snap);
			}
			end_again = nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_END);

			/* Cases (a), (b) and (c): an ADDRESS for a frame whose END is still to be handled. */
			if (torn || end_again) {
				radiant_op.addr_carry = true;
				/* N is lost: torn, or a second frame ended while this one was read (case (a)). */
				NRF_DIAG(nrf_diag.late_isr_drops++);
			}
			if (torn) {
				/* A window that heard something must not give a noise sample. */
				radiant_op.rx_any = true;
			}
			if (end_again) {
				/* Case (a), N+1 ended: take the snapshot again, from N+1's END. */
				nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_END);
				goto rx_end_again;
			}

			NRF_DIAG(if (radiant_op.page7_search &&
				     radiant_radio_now() + RADIANT_DIAG_SRCH_NEAR_CLOSE_US >=
					     radiant_op.page7_close_at) {
					nrf_diag.srch_end_near_close++;
				});
			if (torn) {
				goto rx_end_done;
			}

			/*
			 * Page 10 field 5: how late this END interrupt runs, against the end
			 * of the frame the snapshot's CC_SYNC dates (the software address
			 * bytes, the body and the 16-bit CRC after the capture), at the
			 * snapshot's own counter read. The coded PHY's airtime is not
			 * bytes x 8 us, so it is skipped. Only accepted frames are measured:
			 * an interrupt late past the next frame's ADDRESS tears the snapshot
			 * and is counted in field 4 instead, and one late past the next
			 * frame's END (case (a), last bullet) sees that frame's capture. So
			 * this covers lateness up to the gap to the next ADDRESS.
			 */
			NRF_DIAG(if (radiant_op.phy != RADIANT_PHY_LR_CODED) {
					uint32_t n = radiant_op.body_len;
					int32_t late;

					if (radiant_op.len_from_body) {
						n = MIN((uint32_t)radiant_rx_snap[0] + 1u, n);
					}
					late = (int32_t)(t_snap -
							 (sync + ((uint32_t)radiant_op.sw_addr_len +
								  n + 2u) * US_PER_BYTE));
					if (late > 0 &&
					    (uint32_t)late > nrf_diag.end_late_max_us) {
						nrf_diag.end_late_max_us = (uint32_t)late;
					}
				});

			/*
			 * Which of the caller's filters this frame belongs to. A full hardware match
			 * says so in RXMATCH. A short match (ADR 0019) leaves the rest to compare here.
			 * A frame that fails is another sensor's: no event. It never ends a
			 * STOP_ON_FIRST window, which stops in software on an accepted frame only.
			 */
			int fidx = (logical < ARRAY_SIZE(radiant_filter_slot))
					   ? radiant_filter_slot[logical] : 0;

			const uint8_t *body = radiant_rx_snap;

			if (radiant_op.sw_addr_len != 0u) {
				const uint8_t hint = (uint8_t)fidx;
				const uint8_t hw_len = (uint8_t)(
					radiant_op.addr_len -
					radiant_op.sw_addr_len);

				fidx = radiant_addr_split_match(
					radiant_op.filters, radiant_op.n_filters,
					hw_len, radiant_op.sw_addr_len, hint,
					radiant_rx_snap);
				body = radiant_addr_split_body(
					radiant_rx_snap, radiant_op.sw_addr_len);
				/*
				 * Demodulator slip (ADR 0019): our frame with one bit deleted in its software-matched
				 * bytes. Recovered only if the CRC over the reconstruction verifies, then delivered as OK.
				 */
				if (fidx < 0 && !radiant_op.len_from_body) {
					fidx = radiant_addr_split_repair(
						radiant_op.filters,
						radiant_op.n_filters, hw_len,
						radiant_op.sw_addr_len, hint,
						radiant_rx_snap,
						radiant_op.body_len,
						rxcrc,
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
				/* Same ring as CRC failures, marked by RXCRC's top bit: another sensor, or our frame with corrupted address bytes. */
				rx_diag_crc_fail(logical,
						 (crc_ok ? 0x80000000u : 0x40000000u) |
						 rxcrc,
						 rssi_dbm_from(rssisample),
						 radiant_rx_snap);
			} else if (radiant_op.diag_track) {
				if (crc_ok) {
					int32_t off = (int32_t)(int64_t)(
						clock_absolute(sync) +
						radiant_addr_split_t_sync_us(
							radiant_op.sw_addr_len) -
						radiant_op.diag_t_open);

					radiant_op.diag_ok = true;
					rx_diag.off_min = MIN(rx_diag.off_min, off);
					rx_diag.off_max = MAX(rx_diag.off_max, off);
				} else {
					radiant_op.diag_crc = true;
					rx_diag.fr_crc++;
					rx_diag_crc_fail(logical, rxcrc,
							 rssi_dbm_from(rssisample),
							 radiant_rx_snap);
				}
			}
#endif
			/* Set before the report_crc_fail gate, deliberately: a window that heard something must not give a noise sample. */
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

				/* Hardware capture, moved to antenna reference with this operation's PHY calibration (the PHYs differ by 20 us). */
				evt.t_sync = t_sync_cal(clock_absolute(sync),
							t_sync_cal_rx_us(radiant_op.phy));
				/* ADDRESS fired at the end of the short hardware address; t_sync is the end of the whole on-air address. */
				evt.t_sync += (radiant_time_t)
					radiant_addr_split_t_sync_us(
						radiant_op.sw_addr_len);
				evt.t_sync_exact = true;

				evt.filter_index = (uint8_t)fidx;
				/* Wildcard search (ADR 0022): the device-number byte the hardware did not match. Not set on a repaired frame. */
				if (radiant_op.filters[fidx].wild_last) {
					evt.wild_value =
						radiant_addr_split_wild_value(
							radiant_rx_snap,
							radiant_op.sw_addr_len);
				}
				evt.body = body;
				/*
				 * The length comes off the wire and is clamped before use. Long-range frames carry
				 * it in body byte 0 (delivered length is byte + 1). The clamp matters on CRC
				 * failures, where a corrupted length would expose the previous frame's bytes.
				 */
				if (radiant_op.len_from_body) {
					uint32_t n =
						(uint32_t)radiant_rx_snap[0] + 1u;

					if (n > radiant_op.body_len) {
						n = radiant_op.body_len;
					}
					evt.body_len = (uint8_t)n;
				} else {
					evt.body_len = radiant_op.body_len;
				}
				evt.has_rssi = true;

				/* The received CRC, on failures only: the core forms the error syndrome from it. */
				if (!crc_ok) {
					evt.has_crc_rx = true;
					evt.crc_rx = rxcrc;
				}
				evt.rssi_dbm = rssi_dbm_from(rssisample);

				/* The first accepted frame ends a STOP_ON_FIRST window. A CRC failure reported for repair is not accepted. */
				if (radiant_op.stop_on_first && crc_ok) {
					/* This event IS the terminal one. */
					radiant_op.terminal_sent = true;
					nrfx_gppi_conn_disable(radiant_op.conn_start);
					nrfx_gppi_conn_disable(radiant_op.conn_close);
					nrf_radio_shorts_set(NRF_RADIO, 0);
					/* As in the transmit branch: the callback may arm the next operation, so consume the DISABLED first. */
					radio_disable_now();
#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
					rx_diag_window_end(false);
#endif
					radiant_op.kind = OP_NONE;
#if defined(CORE_CB_DEFERRED)
					/* Owed like any other terminal, or the follow-on reserve is held to the grant's end. Before the callback. */
					grant_release_after_core();
#endif
				}
				core_cb_rx(&evt);
				/* After a STOP_ON_FIRST terminal, as deliver_terminal() does. */
				LP_HOOK(if (radiant_op.kind == OP_NONE) {
						lp_terminal_done();
					});
			}
		}
rx_end_done:
		;
	}

	if (nrf_radio_event_check(NRF_RADIO, NRF_RADIO_EVENT_DISABLED)) {
		nrf_radio_event_clear(NRF_RADIO, NRF_RADIO_EVENT_DISABLED);

		if (radiant_op.kind == OP_RX) {
			/*
			 * Page 7: a search frame cut off mid-air. The END branch clears ADDRESS for every
			 * finished frame, and carries one it consumed for a frame still on air (the
			 * late-ISR guard), so either means an ADDRESS whose END never came.
			 */
			NRF_DIAG(if (radiant_op.page7_search &&
				     (radiant_op.addr_carry ||
				      nrf_radio_event_check(NRF_RADIO,
							    NRF_RADIO_EVENT_ADDRESS))) {
					nrf_diag.srch_cut_close++;
				});
#if defined(CONFIG_RADIANT_NRF_RX_DIAG)
			/* An address matched whose END never came: the window
			 * closed on top of a frame. */
			if (radiant_op.addr_carry ||
			    nrf_radio_event_check(NRF_RADIO,
						  NRF_RADIO_EVENT_ADDRESS)) {
				radiant_op.diag_cut = true;
			}
#endif
			/* The window closed on its own compare, or an abort disabled it. Both are terminal. */
			deliver_terminal(RADIANT_RADIO_STATUS_TIMEOUT);
		} else if (radiant_op.kind == OP_TX) {
			/* A transmit that reaches DISABLED without END never went out. ABORTED is the only honest status. */
			deliver_terminal(RADIANT_RADIO_STATUS_ABORTED);
		}
#if defined(CONFIG_RADIANT_ED_SCAN)
		else if (radiant_op.kind == OP_ED) {
			/* An ED sweep consumes its DISABLED in radio_disable_now(). Reaching here means it outlived that bound: the sweep ends as ABORTED. */
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
	out->v[RADIANT_RADIO_DIAG_SRCH_CUT_CLOSE] = nrf_diag.srch_cut_close;
	out->v[RADIANT_RADIO_DIAG_SRCH_CUT_ABORT] = nrf_diag.srch_cut_abort;
	out->v[RADIANT_RADIO_DIAG_SRCH_END_NEAR_CLOSE] = nrf_diag.srch_end_near_close;
	out->v[RADIANT_RADIO_DIAG_TX_START_MISSED] = nrf_diag.tx_start_missed;
	out->v[RADIANT_RADIO_DIAG_RX_START_MISSED] = nrf_diag.rx_start_missed;
	out->v[RADIANT_RADIO_DIAG_PASS_TO_ARM_MAX_US] = nrf_diag.pass_to_arm_max_us;
	out->v[RADIANT_RADIO_DIAG_ARM_SLACK_MIN_US] = nrf_diag.arm_slack_min_us;
	out->v[RADIANT_RADIO_DIAG_EARLY_ETIME] = nrf_diag.early_etime;
	out->v[RADIANT_RADIO_DIAG_LATE_ISR_DROPS] = nrf_diag.late_isr_drops;
	out->v[RADIANT_RADIO_DIAG_END_LATE_MAX_US] = nrf_diag.end_late_max_us;
	/* Page 10 fields 6-7 (cc26xx's per-entry split) stay "not measured". */
	/* Page 8's backend fields (drain frames/frame/CRC/callback time, slow
	 * drains) stay RADIANT_DIAG_NOT_MEASURED: this backend trusts the
	 * hardware CRC and delivers each frame from its own interrupt, with no
	 * drain loop to time. */
	irq_unlock(key);

	return RADIANT_DIAG_BACKEND_NRF;
}
#endif /* CONFIG_RADIANT_DIAG_COUNTERS */

#if defined(CONFIG_RADIANT_NRF_LOW_POWER)
/* Part of this translation unit: see the file's own header. */
#define RADIANT_RADIO_NRF_LP_INCLUDED 1
#include "radiant_radio_nrf_lp.c"
#endif
