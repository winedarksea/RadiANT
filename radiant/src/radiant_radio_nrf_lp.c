/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf_lp.c - release the HFXO and stop the radio TIMER in long
 * idle gaps (CONFIG_RADIANT_NRF_LOW_POWER, power plan Track B, passes B3, B5
 * and B6).
 *
 * Provenance: clean-room. Written from radiant_radio_nrf.c's own clock and
 * gate seam, src/radiant_radio_nrf_lp.h (the arithmetic, pass B2), the spike
 * results in docs/spike-lp-clock-results.md (pass B1), and Zephyr's public
 * nrf_grtc_timer, nrf_rtc_timer, onoff and clock_control APIs, and the nrfx
 * TIMER HAL's SHUTDOWN task with the nRF52 anomaly 78 workaround nrfx's own
 * timer driver applies. Nothing derives from sdk-ant
 * or any ANT+ document.
 *
 * NOT A TRANSLATION UNIT OF ITS OWN. radiant_radio_nrf.c #includes this file
 * at its end, only when the option is on, so the code here reaches the
 * backend's TIMER fold, CLOCK_LOCK and staged operation without widening any
 * of them into an interface, and so radiant/tests/backend's under-test file
 * compiles both by including one. CMake compiles it through that include and
 * never on its own; with the option off not one line of it is seen.
 *
 * SCOPE. The direct (unarbitrated) backend, on whichever LF counter the system
 * timer runs - one state machine, two counters behind THE LF COUNTER SEAM
 * below:
 *   - nRF54L15 (pass B3): the GRTC SYSCOUNTER (1 MHz), read extended (64-bit
 *     z_nrf_grtc_timer_read()); the GRTC and the radio TIMER share DPPIC20,
 *     so the anchor is one same-domain DPPI connection with no PPIB (spike
 *     B1 S2).
 *   - nRF52840 (pass B5): RTC1 (32768 Hz, 24-bit), read extended (64-bit
 *     z_nrf_rtc_timer_read()) with two of its user channels; the anchor is
 *     one PPI channel with a FORK; the radio TIMER is SHUT DOWN after it is
 *     stopped (nRF52 anomaly 78).
 * The MPSL gate's build (pass B6, GRTC only) shares the timebase, the counter
 * seam's read, the stats and the HFXO boot request, and nothing of the state
 * machine below: there the timeslot START is the wake. Its own section, at
 * the end of this file (LP_MPSL), says why.
 *
 * THE GATE SEAM (direct build). This file is the build's gate (radiant_radio_nrf_gate.h) in
 * place of radiant_radio_nrf_gate_direct.c. Awake, gate_acquire() answers
 * GATE_GRANTED exactly as the direct gate does and the arm programs at once.
 * When the operation asked for is far enough away (radiant_lp_should_sleep),
 * it answers GATE_PENDING instead: the arm call stages the request
 * (radiant_staged, the path the MPSL gate already uses), and this file
 * releases the HFXO and later calls radiant_nrf_gate_on_grant() once the
 * TIMER is running again on a stable crystal. Arming itself - TIMER CC
 * compares wired to RADIO tasks by DPPI - is unchanged, because EDGE 2 below
 * brings the TIMER back on an exact anchor before anything is programmed.
 *
 * ONE SLEEP, FOUR EDGES. The LF counter carries two of this file's compares:
 * the ANCHOR channel, whose compare event is wired by (D)PPI to TIMER START
 * and, forked, TIMER CAPTURE[5] (CC5) - permanently, since START on a running
 * TIMER does nothing - and the HF channel, an interrupt only.
 *
 *   RELEASE   (anchor ch.) CC5 captures the running TIMER on a known LF tick;
 *             the interrupt then stops and clears the TIMER (on the
 *             nRF52, stops and SHUTS IT DOWN: lp_timer_halt()), takes the
 *             release (radiant_lp_release), releases the HFXO and frees
 *             CONSTLAT, and plans the wake for the staged operation.
 *   EDGE 1    (HF ch.) the interrupt requests the HFXO (and CONSTLAT),
 *             start-up + RADIANT_LP_HFXO_ISR_MARGIN_US before EDGE 2.
 *   EDGE 2    (anchor ch.) (D)PPI starts the cleared TIMER on the tick; the
 *             interrupt loads the fold (radiant_lp_wake) and grants the staged
 *             operation, LP_ARM_LEAD_US before it starts.
 *   REANCHOR  (anchor ch.) a capture of the running TIMER after a wake whose
 *             TIMER did not start on a stable crystal (see below).
 *
 * The edges are never the anchor the wrong way round: no TIMER or RADIO event
 * ever triggers an LF counter CAPTURE (spike B1's stale-SYSCOUNTER trap on
 * the GRTC; the nRF52 RTC has no capture task at all).
 *
 * FAILURE PATHS, ALL COUNTED (struct radiant_nrf_lp_stats):
 *   - A compare that, re-read after it is written, is not still
 *     LP_POSTWRITE_LEAD ticks ahead is a LATE ANCHOR (the header's driver
 *     rules: z_nrf_grtc_timer_set() writes a passed value unchanged;
 *     z_nrf_rtc_timer_exact_set(), which every anchoring RTC edge uses,
 *     refuses one closer than its 3-tick minimum). At a
 *     release nothing has been stopped yet, so the backend simply stays
 *     awake. At EDGE 2 the TIMER cannot be trusted to have started on the
 *     tick: the staged operation fails through the existing FAILED terminal,
 *     once, and the TIMER is started in software (radiant_lp_wake(false))
 *     and re-anchored, which bounds the error to one software start.
 *   - The HFXO callback had not run when EDGE 2 fired (a start-up longer
 *     than CONFIG_RADIANT_NRF_LP_HFXO_STARTUP_US): the TIMER started on
 *     HFINT. The segment is marked timer_at_anchor = false (radiant_lp_wake),
 *     nothing is programmed until the crystal is up, then a REANCHOR edge
 *     restores the exact relation and the operation is granted - and fails
 *     late through program_*()'s own ETIME check, as FAILED, if it must.
 *   - An operation armed while asleep closer than the wake lead (or whose
 *     plan comes back RADIANT_LP_WAKE_PAST) wakes AS SOON AS POSSIBLE: the
 *     HFXO is requested in software and EDGE 2 placed one start-up later;
 *     the grant then runs program_*()'s ETIME check, so a wake that came too
 *     late fails the operation (FAILED) rather than transmitting late.
 *   - EDGE 2 serviced while still ASLEEP (the LF interrupt was held off
 *     past both wake edges, and the anchor channel happens to have the
 *     lower index, which the drivers service first): the TIMER did start on
 *     the tick, so the handler does EDGE 1's work first and then takes the
 *     edge (counted in edge_order). Which channel is lower depends on the
 *     allocation: on RTC1 the HF channel is 1 and the anchor 2, so EDGE 1 is
 *     always serviced first and edge_order stays 0 there.
 *   - An HFXO request or start that fails is permanent here (the nRF clock
 *     driver's onoff manager has no reset transition): the option drops to
 *     always-on for the rest of the run (refused = 2, the TIMER started in
 *     software, the HFXO requested through the clock_control API instead),
 *     and a staged operation fails.
 *   - A release whose edge arrives when an operation has been claimed or
 *     programmed meanwhile, or the wanted one is no longer far, or a linger
 *     began, stops nothing: the capture becomes a re-anchor (counted in
 *     release_cancels), so an arm racing a just-committed release is never
 *     put to sleep and woken late.
 *
 * WHERE THE DECISION IS MADE. gate_acquire() (every arm), the end of every
 * terminal (lp_terminal_done(), after the core's callback has had its chance
 * to arm the next operation), every wake (lp_settle()), the release edge
 * itself, and the end of a linger (a compare on the HF channel at
 * linger_until). An operation is never programmed while asleep, so
 * radio_disable_now() returns at once then (its TIMER-timed backstop would
 * spin on a stopped TIMER), and an abort of a staged operation has nothing to
 * tear down (radiant_radio_abort() already handles staged operations;
 * gate_forget_pending() cancels the wake edges).
 *
 * NO TERMINAL INSIDE AN ARM CALL. The scheduler records its armed operation
 * after the arm call returns, so a terminal delivered from inside it would be
 * dropped as stale. gate_acquire() therefore never delivers: if its own path
 * leaves the TIMER running on the crystal, or drops to always-on, it answers
 * GATE_GRANTED and lets program_*()'s ETIME check decide. lp_staged() (an arm
 * finishing its staging after a wake overtook it) programmes but defers any
 * FAILED to the system work queue, checked against the operation id.
 *
 * LINGER. No sleep for CONFIG_RADIANT_NRF_LP_LINGER_MS after enable and after
 * every RADIANT_GATE_PRIO_NORMAL acquire. PRIO_NORMAL is what the backend
 * passes for a scan chunk or receive window of 10 ms or more and for energy
 * detect (rx_arm(), radiant_radio_ed()): searches and acquisition never sleep.
 *
 * LOCKING. Every field of `lp` and every call into radiant_radio_nrf_lp.h,
 * including the LF counter read that feeds it, is under CLOCK_LOCK, the
 * same critical section as the TIMER fold (irq_lock on the direct build;
 * PRIMASK on the MPSL build, whose hooks run in the zero-latency timeslot
 * signal and make no kernel call). The completion work a wake
 * triggers - radiant_nrf_gate_on_grant(), a FAILED terminal and the core
 * callback and scheduler pass behind it - runs after the lock is dropped
 * (lp_flush()), from the LF counter's interrupt (the system timer's: GRTC
 * or RTC1, priority 1 by default on both families), the clock interrupt (the HFXO callback) or the system work
 * queue (a deferred FAILED). None is zero-latency, but none is the RADIO
 * interrupt either: they rely on the core's callbacks being callable from
 * any ordinary interrupt or thread, under the scheduler's own interrupt lock,
 * as the MPSL gate's denials (from MPSL's thread) already do.
 *
 * STATS (CONFIG_RADIANT_NRF_LP_STATS). The global `radiant_nrf_lp_stats`,
 * every field a u32, little-endian, no padding; read it over SWD by symbol
 * (scripts/jlink_peek.ps1 -Symbol radiant_nrf_lp_stats). It is a separate
 * symbol rather than part of radiant_power_diag because that struct has no
 * spare field and its layout is pinned at version 1.
 *
 *   off  field              meaning
 *   0x00 magic              0x52504C31 ("RPL1")
 *   0x04 size               sizeof (0x5C)
 *   0x08 state              current state: 0 off (refused, degraded or not
 *                           initialised), 1 awake, 2 releasing, 3 asleep,
 *                           4 waking
 *   0x0C sleeps             releases completed (TIMER stopped, HFXO released)
 *   0x10 wakes              wakes completed (EDGE 2 or a software start)
 *   0x14 late_anchors       anchoring compares not in time after the write,
 *                           or whose capture was missing (release, EDGE 2,
 *                           re-anchor)
 *   0x18 late_wakes         operations failed (FAILED) because the wake came
 *                           too late for them: a late EDGE 2, the grant's own
 *                           ETIME check, or an HFXO failure
 *   0x1C hfxo_late_edge2    EDGE 2 fired before the HFXO callback
 *   0x20 reanchors          re-anchors taken (radiant_lp_reanchor true)
 *   0x24 range_errors       asleep readings refused as out of range (a lost
 *                           timebase; expected 0 with the extended counter)
 *   0x28 lf_taken           releases that took the LF image of their edge
 *   0x2C timer_taken        releases that took the TIMER's reading
 *   0x30 disagreements      releases with an exact anchor whose LF image lay
 *                           more than RADIANT_LP_RELEASE_TOL_US from the TIMER
 *   0x34 max_disagree_us    largest |LF image - TIMER| at an exact release
 *   0x38 max_wake_slack_us  largest time from "awake and granted" to the
 *                           operation's start (how early a wake was)
 *   0x3C max_wake_late_us   largest radiant_lp_late_by_us() at a grant after
 *                           a wake (0 = every wake in time)
 *   0x40 asap_wakes         wakes started in software without EDGE 1's lead
 *   0x44 release_cancels    committed releases called off: by a nearer arm
 *                           before the edge, or at the edge (see above)
 *   0x48 spurious           compare interrupts for an edge no longer armed
 *   0x4C hfxo_errors        HFXO requests or callbacks that returned an error
 *   0x50 refused            1 if the option refused itself at init (LFCLK
 *                           not the crystal, no LF channel, no (D)PPI); 2 if
 *                           it dropped to always-on after an HFXO error
 *   0x54 last_disagree_q    signed LF image minus TIMER at the last exact
 *                           release, q units (us on the GRTC, 1/512 us
 *                           on the RTC)
 *   0x58 edge_order         EDGE 2 handled before EDGE 1 (a held interrupt)
 *
 * The MPSL build (pass B6) appends six fields (size 0x74) and uses only some
 * of the ones above: sleeps and wakes count timeslot ends and STARTs (the
 * epoch release at init is not counted); lf_taken, timer_taken,
 * disagreements, max_disagree_us and last_disagree_q judge each timeslot's
 * TIMER against the GRTC at its end; range_errors and refused as above. The
 * rest stay 0: there is no edge to be late, no HFXO request after init, no
 * staged wake.
 *
 *   0x5C slot_awake_us      us the TIMER ran in timeslots (GRTC-timed,
 *                           wraps at 2^32): the duty proxy, since MPSL
 *                           holds the HFXO for at least this long on the
 *                           backend's behalf (plus its own ramp ahead of
 *                           each START, which this does not see)
 *   0x60 start_err_n        STARTs of timed timeslots (not the bootstrap,
 *                           not the elastic class) cross-checked below
 *   0x64 start_err_min_us   signed: the START's GRTC reading minus the start
 *   0x68 start_err_max_us   the gate asked MPSL for. Both are on the LF
 *   0x6C start_err_last_us  timebase, so this is MPSL's own dispatch
 *                           latency plus any drift of this timebase against
 *                           MPSL's grid (the gate's spike: 0..17 us)
 *   0x70 missed_releases    timeslots whose TIMER was still running at the
 *                           next START or at a low-priority backstop (the
 *                           ACTION_END funnel was not passed); each takes the
 *                           LF image whatever the TIMER read
 */

#if !defined(RADIANT_RADIO_NRF_LP_INCLUDED)
#error "radiant_radio_nrf_lp.c is part of radiant_radio_nrf.c, which includes it"
#endif

#include <zephyr/sys/onoff.h>
#if defined(CONFIG_NRF_GRTC_TIMER)
#include <zephyr/drivers/timer/nrf_grtc_timer.h>
#elif defined(CONFIG_NRF_RTC_TIMER)
#include <zephyr/drivers/timer/nrf_rtc_timer.h>
#endif
#include <hal/nrf_clock.h>
#if defined(NRF54L_ERRATA_20_PRESENT)
#include <nrfx_power.h>
#endif

#include "radiant_radio_nrf_lp.h"

BUILD_ASSERT(IS_ENABLED(CONFIG_NRF_GRTC_TIMER) || IS_ENABLED(CONFIG_NRF_RTC_TIMER),
	     "RADIANT_NRF_LOW_POWER keeps time on the system timer's LF counter: "
	     "the GRTC (nRF54L) or RTC1 (nRF52)");
/*
 * LP_MPSL: the MPSL gate's build (pass B6). The direct build's state machine
 * (the edges, the staged wake, the gate) is compiled only without it; the MPSL
 * build shares the timebase, the counter, the stats and the HFXO boot request,
 * and adds its own section at the end of this file.
 */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
#define LP_MPSL 1
#endif
#if defined(CORE_CB_DEFERRED) && !defined(LP_MPSL)
#error "RADIANT_NRF_LOW_POWER's direct build assumes its CLOCK_LOCK (irq_lock)"
#endif
#if !defined(LP_MPSL)
BUILD_ASSERT(DT_PROP_OR(RADIANT_TIMER_NODE, cc_num, 0) >= 6,
	     "RADIANT_NRF_LOW_POWER needs CC5 of the radio TIMER for its anchor");
#endif

/*
 * The LF counter's rate: the system timer's cycle rate on both families
 * (1 MHz GRTC SYSCOUNTER, 32768 Hz RTC1), checked against the counter format
 * below. The backend test harness supplies its own value.
 */
#ifndef RADIANT_LP_LF_HZ
#define RADIANT_LP_LF_HZ CONFIG_SYS_CLOCK_HW_CYCLES_PER_SEC
#endif

/*
 * After an HFXO failure: keep the crystal on through the clock_control API
 * (the onoff manager stays in its error state; the nRF driver has no reset).
 * Asynchronous, so callable from an interrupt. The test harness supplies its
 * own.
 */
#ifndef LP_HF_FALLBACK
#define LP_HF_FALLBACK()                                                       \
	clock_control_async_on(DEVICE_DT_GET_ONE(nordic_nrf_clock),      \
				     CLOCK_CONTROL_NRF_SUBSYS_HF, NULL, NULL)
#endif

/* The TIMER compare the anchor connection captures into. */
#define LP_CC_ANCHOR 5u
/* CC5 before EDGE 2: the edge's capture of the cleared TIMER writes 0. */
#define LP_CC5_UNSET 0xFFFFFFFFu

/* ---- THE LF COUNTER SEAM (pass B5) ----
 *
 * Everything that differs between the two LF counters, and nothing else: the
 * state machine below is one copy, so a fix to it is a fix on both. Each
 * counter supplies
 *
 *   LP_CNT_NAME, LP_CNT_NUM, LP_CNT_SHIFT, LP_CNT_HZ
 *                       its name (logs) and its radiant_radio_nrf_lp.h
 *                       format, used EXTENDED (the driver's 64-bit count,
 *                       so RTC1's 24-bit wrap every 512 s never reaches the
 *                       arithmetic);
 *   LP_MIN_LEAD         least ticks ahead a compare is written with;
 *   LP_POSTWRITE_LEAD   how far ahead a compare must still be when the
 *                       counter is re-read after the write (the "past
 *                       compare" bound radiant_lp_compare_in_time()
 *                       applies), and before a committed release may be
 *                       called off;
 *   LP_RELEASE_GRACE    a terminal-decided release's edge lead, ~300 us:
 *                       long enough that the arm a scheduler pass makes
 *                       right after an abort can still call it off before
 *                       the edge (the edge itself refuses to sleep anyway);
 *   LP_MAX_SLEEP_TICKS  furthest a wake is planned from the anchor (the
 *                       range bound: further sleeps rebase there);
 *   lp_cnt_read()       the extended counter, read under CLOCK_LOCK;
 *   lp_cnt_chan_alloc() / lp_cnt_chan_free() / lp_cnt_evt_address()
 *                       the two channels, and the anchor channel's compare
 *                       event for the (D)PPI connection - nrfx_gppi either
 *                       way: DPPI on the nRF54L, PPI with a FORK on the
 *                       nRF52, the same calls for both;
 *   lp_cnt_set(ch, tick, handler, exact) / lp_cnt_abort(ch)
 *                       a compare and its withdrawal; `exact` (the
 *                       anchoring edges) means "on this tick or not at all";
 *   LP_CNT_WAIT_EDGE()  the counter just after it has stepped, for a
 *                       software TIMER start (just a read at 1 us ticks).
 *
 * and lp_timer_halt(), which stops the radio TIMER for a sleep (a property of
 * the SoC's TIMER rather than of the counter, but it differs on the same
 * line: the nRF52 has SHUTDOWN and anomaly 78).
 */
typedef void (*lp_cnt_handler_t)(int32_t id, uint64_t expire, void *user);

#if defined(CONFIG_NRF_GRTC_TIMER)

#define LP_CNT_NAME   "GRTC"
#define LP_CNT_NUM    RADIANT_LP_GRTC_NUM
#define LP_CNT_SHIFT  RADIANT_LP_GRTC_SHIFT
#define LP_CNT_HZ     1000000
/* Lead a compare is written with, and how far ahead it must still be when
 * re-read after the write: the GRTC latch bound nrf_grtc_timer.c waits on
 * (MAX_CC_LATCH_WAIT_TIME_US, 77) plus a margin for the re-read itself.
 * z_nrf_grtc_timer_set() writes any value unchanged, so the re-read is the
 * only check there is. */
#define LP_MIN_LEAD        RADIANT_LP_GRTC_MIN_LEAD_TICKS
#define LP_POSTWRITE_LEAD  (77u + 8u)
#define LP_RELEASE_GRACE   300u
#define LP_MAX_SLEEP_TICKS (600ull * 1000000ull)

static inline uint64_t lp_cnt_read(void)
{
	return z_nrf_grtc_timer_read();
}

static inline int32_t lp_cnt_chan_alloc(void)
{
	return z_nrf_grtc_timer_chan_alloc();
}

static inline void lp_cnt_chan_free(int32_t ch)
{
	z_nrf_grtc_timer_chan_free(ch);
}

static inline uint32_t lp_cnt_evt_address(int32_t ch)
{
	return z_nrf_grtc_timer_compare_evt_address_get(ch);
}

static inline int lp_cnt_set(int32_t ch, uint64_t tick, lp_cnt_handler_t h, bool exact)
{
	/* The driver never moves a value: every write is as exact as it gets,
	 * and the caller's re-read decides whether it was in time. */
	ARG_UNUSED(exact);
	return z_nrf_grtc_timer_set(ch, tick, h, NULL);
}

static inline void lp_cnt_abort(int32_t ch)
{
	z_nrf_grtc_timer_abort(ch);
}

#ifndef LP_CNT_WAIT_EDGE
#define LP_CNT_WAIT_EDGE() lp_cnt_read()   /* 1 us ticks: the read is the edge */
#endif

#elif defined(CONFIG_NRF_RTC_TIMER)

#define LP_CNT_NAME   "RTC1"
#define LP_CNT_NUM    RADIANT_LP_RTC_NUM
#define LP_CNT_SHIFT  RADIANT_LP_RTC_SHIFT
#define LP_CNT_HZ     32768
/*
 * nrf_rtc_timer.c's MIN_CYCLES_FROM_NOW (3): the product specification
 * guarantees a compare written two ticks ahead, but the driver's own tests
 * saw N + 2 missed after an N + 1 compare, so it keeps three, and
 * z_nrf_rtc_timer_exact_set() refuses anything closer. Every anchoring edge
 * uses exact_set(): z_nrf_rtc_timer_set() silently moves a too-close compare
 * LATER, onto a tick the anchor arithmetic would not know about.
 *
 * The edges are written one tick further out than that: the backend picks a
 * tick from its own counter read and the driver checks against its own read
 * a few microseconds later, and at 30.5 us a tick a step falls between the
 * two reads about one time in ten. With three ticks that step would leave
 * two, the driver would refuse, and a tenth of all releases would be lost
 * (late anchors, a whole period awake each); with four it leaves three.
 * (The GRTC's 108 against its 85 is the same margin.)
 *
 * The re-read after a successful exact_set() needs no latch margin (the
 * driver itself checked the compare against the counter after writing it),
 * but the same bound also decides whether a committed release may still be
 * withdrawn (lp_cancel_release_locked()): two ticks (61 us) is far more than
 * an abort's register writes take to land ahead of the compare.
 */
#define LP_MIN_LEAD        (RADIANT_LP_RTC_MIN_LEAD_TICKS + 1u)
#define LP_POSTWRITE_LEAD  2u
#define LP_RELEASE_GRACE   10u   /* 305 us */
/* z_nrf_rtc_timer_set()/_exact_set() refuse a target more than
 * NRF_RTC_TIMER_MAX_SCHEDULE_SPAN (2^23 ticks, 256 s) ahead; half of it
 * (128 s) leaves the time between planning and writing far inside. */
#define LP_MAX_SLEEP_TICKS ((uint64_t)NRF_RTC_TIMER_MAX_SCHEDULE_SPAN / 2u)

/* The format, the span and the driver behaviour above are the 24-bit RTC's.
 * With a narrower counter (nRF52832: 15 bits) the driver moves a CC of 0 to
 * 1 even in exact_set() and the schedule span is 2^14 ticks; Kconfig refuses
 * the option there, this is the backstop. */
BUILD_ASSERT(CONFIG_NRF_RTC_COUNTER_BIT_WIDTH == 24,
	     "RADIANT_NRF_LOW_POWER assumes RTC1's full 24-bit counter");
BUILD_ASSERT(CONFIG_NRF_RTC_TIMER_USER_CHAN_COUNT >= 2,
	     "RADIANT_NRF_LOW_POWER takes two RTC1 user channels "
	     "(CONFIG_NRF_RTC_TIMER_USER_CHAN_COUNT, which radiant/Kconfig "
	     "defaults to 2 with the option)");

static inline uint64_t lp_cnt_read(void)
{
	return z_nrf_rtc_timer_read();
}

static inline int32_t lp_cnt_chan_alloc(void)
{
	return z_nrf_rtc_timer_chan_alloc();
}

static inline void lp_cnt_chan_free(int32_t ch)
{
	z_nrf_rtc_timer_chan_free(ch);
}

static inline uint32_t lp_cnt_evt_address(int32_t ch)
{
	return z_nrf_rtc_timer_compare_evt_address_get(ch);
}

static inline int lp_cnt_set(int32_t ch, uint64_t tick, lp_cnt_handler_t h, bool exact)
{
	return exact ? z_nrf_rtc_timer_exact_set(ch, tick, h, NULL)
		     : z_nrf_rtc_timer_set(ch, tick, h, NULL);
}

static inline void lp_cnt_abort(int32_t ch)
{
	z_nrf_rtc_timer_abort(ch);
}

/*
 * The counter just after it steps. A software TIMER start (a late anchor's
 * recovery, a degrade) loads its fold from the LF reading it makes; read
 * mid-tick, that reading is up to one tick (30.5 us) behind the start, an
 * error the next release takes into the timebase for good. Starting on the
 * step bounds it by this loop's latency instead. At most one tick, under
 * CLOCK_LOCK, on a recovery path only. Bounded by a count, not a time (the
 * system clock IS this counter): 4096 reads take well over a tick. The test
 * harness supplies its own.
 */
#ifndef LP_CNT_WAIT_EDGE
static uint64_t lp_cnt_wait_edge(void)
{
	uint64_t t0 = lp_cnt_read();
	uint64_t t = t0;

	for (uint32_t n = 0u; n < 4096u && t == t0; n++) {
		t = lp_cnt_read();
	}
	return t;
}
#define LP_CNT_WAIT_EDGE() lp_cnt_wait_edge()
#endif

#endif /* the LF counter */

BUILD_ASSERT(RADIANT_LP_LF_HZ == LP_CNT_HZ,
	     "the low-power counter format does not match the system timer's rate");

/*
 * Stop the radio TIMER for a sleep. On parts with a TIMER SHUTDOWN task (the
 * nRF52 - anomaly 78: STOP alone leaves the TIMER drawing current) STOP is
 * followed by SHUTDOWN, as nrfx's own timer driver does. SHUTDOWN zeroes the
 * counter (spike B1 S1; the CC registers survive), so it stands in for the
 * CLEAR every other part gets: either way the TIMER is left stopped at 0,
 * so that EDGE 2's (D)PPI start counts the segment from the edge. The fold
 * does not depend on the 0: lp_wake_edge_locked() loads `last` from the CC5
 * capture EDGE 2 itself took, so a counter that a stray START or a SHUTDOWN
 * left non-zero would shift nothing. No fold reads the TIMER while it is
 * stopped (lp_now_locked() and radio_disable_now() check lp_timer_stopped()
 * first). A release reads its CC5 capture before calling this.
 */
static inline void lp_timer_halt(void)
{
	nrf_timer_task_trigger(TIMER_ADDR, NRF_TIMER_TASK_STOP);
#if NRF_TIMER_HAS_SHUTDOWN
	nrf_timer_task_trigger(TIMER_ADDR, NRF_TIMER_TASK_SHUTDOWN);
#else
	nrf_timer_task_trigger(TIMER_ADDR, NRF_TIMER_TASK_CLEAR);
#endif
}

/* ---- End of the seam ---- */

/*
 * EDGE 2's lead over the operation: the backend's own programming budget
 * plus the LF interrupt's latency and the wake bookkeeping before
 * radiant_nrf_gate_on_grant() runs. A starting value: max_wake_late_us
 * and late_wakes are how a too-small one shows.
 */
#define LP_EDGE2_ISR_US 150u
#define LP_ARM_LEAD_US  (ARM_SETUP_US + LP_EDGE2_ISR_US)
/* EDGE 1's lead over EDGE 2. */
#define LP_HFXO_LEAD_US (CONFIG_RADIANT_NRF_LP_HFXO_STARTUP_US + \
			 RADIANT_LP_HFXO_ISR_MARGIN_US)
/* A release or re-anchor capture older than this at its interrupt is not one. */
#define LP_CAPTURE_MAX_AGE_US 20000
#define LP_LINGER_US ((uint64_t)CONFIG_RADIANT_NRF_LP_LINGER_MS * 1000u)

enum lp_state {
	LP_OFF = 0,     /* not initialised, refused or degraded: always on */
	LP_AWAKE,       /* TIMER running, HFXO requested */
	LP_RELEASING,   /* the release edge is armed; the TIMER still runs */
	LP_ASLEEP,      /* TIMER stopped and cleared, HFXO released */
	LP_WAKING,      /* HFXO requested, EDGE 2 armed (or a software start owed) */
};

enum lp_anchor { LP_ANCHOR_NONE = 0, LP_ANCHOR_RELEASE, LP_ANCHOR_WAKE, LP_ANCHOR_REANCHOR };
enum lp_hf { LP_HF_NONE = 0, LP_HF_EDGE1, LP_HF_REBASE, LP_HF_LINGER };

/* Work owed once the lock is dropped (lp_flush()). */
#define LP_ACT_SETTLE     (1u << 0)
#define LP_ACT_FAIL       (1u << 1)
#define LP_ACT_RECONSIDER (1u << 2)

static struct {
	struct radiant_lp_tb    tb;
	struct radiant_lp_clock clk;
	struct onoff_manager   *hf_mgr;
	struct onoff_client     hf_cli;
	int32_t                 ch_anchor;
	int32_t                 ch_hf;
	nrfx_gppi_handle_t      conn_anchor;
	uint64_t                anchor_tick;   /* armed anchor-channel compare */
	uint64_t                hf_tick;       /* armed HF-channel compare */
	uint64_t                hfxo_req_tick; /* when the HFXO was last requested */
	uint64_t                want_t;        /* t_from of the staged operation */
	uint64_t                linger_until;
	uint64_t                wake_lead_us;
	uint64_t                lf_overhead_us;
	uint32_t                defer_op;      /* operation a deferred FAILED is for */
	uint8_t                 state;         /* enum lp_state */
	uint8_t                 anchor;        /* enum lp_anchor */
	uint8_t                 hf;            /* enum lp_hf */
	uint8_t                 act;           /* LP_ACT_* owed after the lock */
	uint8_t                 nest;          /* inside our onoff_request() */
	bool                    want;          /* an operation is staged for a wake */
	bool                    grant_owed;    /* woke before the arm finished staging */
	bool                    fail_owed;     /* that operation fails (late anchor) */
	bool                    defer_fail;    /* a FAILED waits on the work queue */
	bool                    just_woke;     /* the next grant follows a wake */
	volatile bool           hfxo_req;
	volatile bool           hfxo_ready;
	bool                    hfxo_wait;     /* awake on HFINT until the HFXO is up */
	bool                    sw_start_pending;
	bool                    constlat_held;
	bool                    conn_alloc;
} lp = {
	.ch_anchor = -1,
	.ch_hf = -1,
};

#if defined(CONFIG_RADIANT_NRF_LP_STATS)
struct radiant_nrf_lp_stats {
	uint32_t magic;
	uint32_t size;
	uint32_t state;
	uint32_t sleeps;
	uint32_t wakes;
	uint32_t late_anchors;
	uint32_t late_wakes;
	uint32_t hfxo_late_edge2;
	uint32_t reanchors;
	uint32_t range_errors;
	uint32_t lf_taken;
	uint32_t timer_taken;
	uint32_t disagreements;
	uint32_t max_disagree_us;
	uint32_t max_wake_slack_us;
	uint32_t max_wake_late_us;
	uint32_t asap_wakes;
	uint32_t release_cancels;
	uint32_t spurious;
	uint32_t hfxo_errors;
	uint32_t refused;
	int32_t  last_disagree_q;
	uint32_t edge_order;
#if defined(LP_MPSL)
	/* Appended by the MPSL build only (pass B6); see the file header. */
	uint32_t slot_awake_us;
	uint32_t start_err_n;
	int32_t  start_err_min_us;
	int32_t  start_err_max_us;
	int32_t  start_err_last_us;
	uint32_t missed_releases;
#endif
};
#if defined(LP_MPSL)
BUILD_ASSERT(sizeof(struct radiant_nrf_lp_stats) == 0x74, "layout in the file header");
#else
BUILD_ASSERT(sizeof(struct radiant_nrf_lp_stats) == 0x5C, "layout in the file header");
#endif

struct radiant_nrf_lp_stats radiant_nrf_lp_stats = {
	.magic = 0x52504C31u,
	.size = sizeof(struct radiant_nrf_lp_stats),
};
#define LP_STAT(...) do { __VA_ARGS__; } while (0)
#define LP_STAT_MAX(f, v)                                                      \
	do {                                                                   \
		uint64_t v_ = (v);                                             \
		uint32_t s_ = (v_ > UINT32_MAX) ? UINT32_MAX : (uint32_t)v_;   \
		if (s_ > radiant_nrf_lp_stats.f) {                             \
			radiant_nrf_lp_stats.f = s_;                           \
		}                                                              \
	} while (0)
#else
#define LP_STAT(...) do { } while (0)
#define LP_STAT_MAX(f, v) do { (void)(v); } while (0)
#endif

#if !defined(LP_MPSL)
/* late_wakes from outside CLOCK_LOCK. */
static void lp_count_late_wake(void)
{
	unsigned int key = CLOCK_LOCK();

	LP_STAT(radiant_nrf_lp_stats.late_wakes++);
	CLOCK_UNLOCK(key);
}
#endif

static void lp_set_state(enum lp_state s)
{
	lp.state = (uint8_t)s;
	LP_STAT(radiant_nrf_lp_stats.state = (uint32_t)s);
}

static inline uint64_t lp_ticks(void)
{
	return lp_cnt_read();
}

/* Whole ticks covering `us`, rounded up. */
static inline uint64_t lp_us_to_ticks_ceil(uint64_t us)
{
	return ((us << lp.tb.shift) + lp.tb.num - 1u) / lp.tb.num;
}

static bool lp_timer_stopped(void)
{
	return lp.state == LP_ASLEEP || lp.state == LP_WAKING;
}

/* ---- The piecewise clock (radiant_radio_now()) ---- */

/* Under CLOCK_LOCK. */
static uint64_t lp_now_locked(void)
{
	uint64_t t;

	if (lp.state == LP_OFF) {
		return clock_fold(timer_capture(CC_NOW));
	}
	if (lp_timer_stopped()) {
		t = radiant_lp_clock_now(&lp.tb, &lp.clk, true, 0u, lp_ticks());
		LP_STAT(radiant_nrf_lp_stats.range_errors = lp.clk.range_errors);
		return t;
	}
	return radiant_lp_clock_now(&lp.tb, &lp.clk, false,
				    clock_fold(timer_capture(CC_NOW)), 0u);
}

#if !defined(LP_MPSL)
/*
 * The TIMER-domain time of a CC5 capture taken shortly before now, or false
 * if CC5 does not hold one (the sentinel written when the edge was armed, a
 * capture from the future, or one older than LP_CAPTURE_MAX_AGE_US). Under
 * CLOCK_LOCK, TIMER running.
 */
static bool lp_capture_time(uint64_t *timer_us)
{
	uint64_t base = clock_fold(timer_capture(CC_NOW));
	int32_t  d = (int32_t)(nrf_timer_cc_get(TIMER_ADDR, LP_CC_ANCHOR) -
			       radiant_clock.last);

	if (d > 0 || d < -(int32_t)LP_CAPTURE_MAX_AGE_US) {
		return false;
	}
	*timer_us = (uint64_t)((int64_t)base + d);
	return true;
}

/*
 * Start the TIMER from zero in software now, the fold loaded from the LF
 * counter (radiant_lp_wake(..., false)), on the counter's step where it has
 * one (LP_CNT_WAIT_EDGE). Under CLOCK_LOCK.
 *
 * Started on the step, the TIMER's zero is that tick's exact time to within
 * the wait loop's latency, so the tick's sub-microsecond part is carried into
 * the segment as an EDGE 2 wake carries it; radiant_lp_wake(..., false) drops
 * it, and on RTC1 (1/512 us units) that would leave the segment - and,
 * through the release that takes its TIMER reading, the timebase for good -
 * up to a microsecond behind. The relation still is not a (D)PPI start, so
 * timer_at_anchor stays false and the segment is re-anchored as before. On
 * the GRTC (1 us ticks) the fraction is always 0.
 */
static void lp_timer_sw_start_locked(void)
{
	uint64_t t;

	/* From zero, whatever a stray edge may have done meanwhile. */
	nrf_timer_task_trigger(TIMER_ADDR, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(TIMER_ADDR, NRF_TIMER_TASK_CLEAR);
	t = LP_CNT_WAIT_EDGE();
	nrf_timer_task_trigger(TIMER_ADDR, NRF_TIMER_TASK_START);
	radiant_clock.base = radiant_lp_wake(&lp.tb, &lp.clk, t, false);
	radiant_clock.last = 0u;
	lp.clk.awake_frac = (uint32_t)(lp.clk.anchor_q &
				       ((UINT64_C(1) << lp.tb.shift) - 1u));
}
#endif /* !LP_MPSL */

/* ---- HFXO and CONSTLAT ---- */

/*
 * nRF54L errata 20: CONSTLAT whenever the radio can be active. Through nrfx's
 * reference count (the direct build without this option raw-triggers it at
 * init and never lets go): requested with the HFXO, freed with it.
 */
static void lp_constlat(bool on)
{
#if defined(NRF54L_ERRATA_20_PRESENT) && !defined(LP_MPSL)
	if (!nrf54l_errata_20() || on == lp.constlat_held) {
		return;
	}
	if (on) {
		(void)nrfx_power_constlat_mode_request();
	} else {
		(void)nrfx_power_constlat_mode_free();
	}
	lp.constlat_held = on;
#else
	/* The MPSL build leaves CONSTLAT to MPSL (see its section below). */
	ARG_UNUSED(on);
#endif
}

#if !defined(LP_MPSL)
static void lp_flush(void);
static void lp_settle(void);
static void lp_reconsider(void);
static void lp_reanchor_arm_locked(void);
static void lp_sw_start_locked(void);
static void lp_cancel_edges_locked(void);

/*
 * The HFXO failed (a request or its start returned an error). The nRF clock
 * driver's onoff manager has no reset transition, so it stays failed: drop
 * to always-on for the rest of the run. Under CLOCK_LOCK, any state.
 */
static void lp_degrade_locked(void)
{
	int rc;

	if (lp.state == LP_OFF) {
		return;   /* already always-on: counted once */
	}
	LP_STAT(radiant_nrf_lp_stats.hfxo_errors++; radiant_nrf_lp_stats.refused = 2u);
	lp_cancel_edges_locked();
	if (lp_timer_stopped()) {
		lp_timer_sw_start_locked();
	}
	lp.hfxo_wait = false;
	lp.sw_start_pending = false;
	rc = LP_HF_FALLBACK();
	if (rc < 0 && rc != -EALREADY) {
		/* No crystal by either route: the radio will not hold bit sync. */
		LP_STAT(radiant_nrf_lp_stats.hfxo_errors++);
		LOG_WRN("low power: HFXO failed and the always-on request was "
			"refused too (%d)", rc);
	}
	lp_constlat(true);
	if (lp.want) {
		/* The staged operation cannot be met on this plan. */
		lp.act |= LP_ACT_FAIL;
	}
	lp.want = false;
	lp.grant_owed = false;
	lp.fail_owed = false;
	lp_set_state(LP_OFF);
}
#endif /* !LP_MPSL */

static void lp_hfxo_cb(struct onoff_manager *mgr, struct onoff_client *cli,
		       uint32_t st, int res)
{
	unsigned int key;
	bool flush;

	ARG_UNUSED(mgr);
	ARG_UNUSED(cli);
	ARG_UNUSED(st);

	key = CLOCK_LOCK();
	/* Run synchronously inside our own onoff_request(): that caller holds
	 * the lock and owns the work it leaves. */
	flush = (lp.nest == 0u);
	if (!lp.hfxo_req) {
		/* Released before it started. */
		CLOCK_UNLOCK(key);
		return;
	}
	if (res < 0) {
#if !defined(LP_MPSL)
		if (lp.state != LP_OFF) {
			lp_degrade_locked();
		} else {
			LP_STAT(radiant_nrf_lp_stats.hfxo_errors++);
		}
#else
		/* Only the boot request exists here (LP_OFF). */
		LP_STAT(radiant_nrf_lp_stats.hfxo_errors++);
#endif
	} else {
		lp.hfxo_ready = true;   /* also the boot request, in LP_OFF */
#if !defined(LP_MPSL)
		if (lp.state == LP_AWAKE && lp.hfxo_wait) {
			/* EDGE 2 started the TIMER on HFINT: restore the exact
			 * relation on the crystal, then grant. */
			lp.hfxo_wait = false;
			lp_reanchor_arm_locked();
			lp.act |= LP_ACT_SETTLE;
		} else if (lp.state == LP_WAKING && lp.sw_start_pending) {
			lp_sw_start_locked();
		}
#endif
	}
	CLOCK_UNLOCK(key);
#if !defined(LP_MPSL)
	if (flush) {
		lp_flush();
	}
#else
	ARG_UNUSED(flush);
#endif
}

#if !defined(LP_MPSL)
/* Under CLOCK_LOCK. May degrade (state LP_OFF) if the request fails. */
static void lp_hfxo_on_locked(void)
{
	int r;

	if (lp.hfxo_req) {
		return;
	}
	lp_constlat(true);
	lp.hfxo_req = true;
	lp.hfxo_ready = false;
	lp.hfxo_req_tick = lp_ticks();
	POWER_DIAG(radiant_power_diag_hfxo(true));
	sys_notify_init_callback(&lp.hf_cli.notify, lp_hfxo_cb);
	lp.nest++;
	r = onoff_request(lp.hf_mgr, &lp.hf_cli);
	lp.nest--;
	if (r < 0) {
		lp_degrade_locked();
	}
}
#endif /* !LP_MPSL */

/* Under CLOCK_LOCK. */
static void lp_hfxo_off_locked(void)
{
	if (lp.hfxo_req) {
		lp.hfxo_req = false;
		lp.hfxo_ready = false;
		(void)onoff_cancel_or_release(lp.hf_mgr, &lp.hf_cli);
		POWER_DIAG(radiant_power_diag_hfxo(false));
	}
	lp_constlat(false);
}

/*
 * The boot request, in place of clock_control_on(): the HFXO's onoff manager
 * and the blocking clock_control API may not both control it (the driver
 * answers -EPERM), and every later request goes through the manager. Thread
 * context, radiant_radio_init().
 */
static int lp_hfxo_boot(void)
{
	int r;

	if (lp.hfxo_req && lp.hfxo_ready) {
		return 0;   /* an init retry: still held from the first attempt */
	}
	lp.hf_mgr = z_nrf_clock_control_get_onoff(CLOCK_CONTROL_NRF_SUBSYS_HF);
	if (lp.hf_mgr == NULL) {
		return -ENODEV;
	}
	lp.hfxo_ready = false;
	lp.hfxo_req = true;
	sys_notify_init_callback(&lp.hf_cli.notify, lp_hfxo_cb);
	r = onoff_request(lp.hf_mgr, &lp.hf_cli);
	if (r < 0) {
		lp.hfxo_req = false;
		return r;
	}
	for (uint32_t i = 0u; i < 5000u && !lp.hfxo_ready; i++) {
		k_busy_wait(10);
	}
	if (!lp.hfxo_ready) {
		/* Take the client back off the manager's queue before a retry
		 * re-initialises and re-queues it. */
		lp.hfxo_req = false;
		(void)onoff_cancel_or_release(lp.hf_mgr, &lp.hf_cli);
		return -ETIMEDOUT;
	}
	return 0;
}

#if !defined(LP_MPSL)
/* ---- Compares ---- */

static void lp_anchor_handler(int32_t id, uint64_t expire, void *user);
static void lp_hf_handler(int32_t id, uint64_t expire, void *user);

/*
 * Arm the anchor channel at `tick` for `what`, with CC5 primed so a missing
 * capture is recognisable. False, with nothing armed, if the compare is not
 * still LP_POSTWRITE_LEAD ahead when re-read after the write (a late anchor).
 * Under CLOCK_LOCK.
 */
static bool lp_set_anchor_locked(uint64_t tick, enum lp_anchor what)
{
	int rc;

	nrf_timer_cc_set(TIMER_ADDR, LP_CC_ANCHOR,
			 (what == LP_ANCHOR_WAKE) ? LP_CC5_UNSET
						  : timer_capture(CC_NOW) + 0x80000000u);
	lp.anchor = (uint8_t)what;
	lp.anchor_tick = tick;
	rc = lp_cnt_set(lp.ch_anchor, tick, lp_anchor_handler, true);
	if (rc != 0 ||
	    !radiant_lp_compare_in_time(&lp.tb, lp_ticks(), tick, LP_POSTWRITE_LEAD)) {
		lp_cnt_abort(lp.ch_anchor);
		lp.anchor = LP_ANCHOR_NONE;
		LP_STAT(radiant_nrf_lp_stats.late_anchors++);
		return false;
	}
	return true;
}

/* EDGE 1, rebases and the linger's end anchor nothing: a late one is merely
 * an early or late interrupt. Still checked, so a stale value is never left
 * armed. */
static bool lp_set_hf_locked(uint64_t tick, enum lp_hf what)
{
	int rc;

	lp.hf = (uint8_t)what;
	lp.hf_tick = tick;
	rc = lp_cnt_set(lp.ch_hf, tick, lp_hf_handler, false);
	if (rc != 0 ||
	    !radiant_lp_compare_in_time(&lp.tb, lp_ticks(), tick, LP_POSTWRITE_LEAD)) {
		lp_cnt_abort(lp.ch_hf);
		lp.hf = LP_HF_NONE;
		return false;
	}
	return true;
}

static void lp_cancel_hf_locked(void)
{
	if (lp.hf != LP_HF_NONE) {
		lp_cnt_abort(lp.ch_hf);
		lp.hf = LP_HF_NONE;
	}
}

static void lp_cancel_edges_locked(void)
{
	bool wake = (lp.anchor == LP_ANCHOR_WAKE && lp.state == LP_ASLEEP);

	lp_cancel_hf_locked();
	if (lp.anchor != LP_ANCHOR_NONE) {
		lp_cnt_abort(lp.ch_anchor);
		lp.anchor = LP_ANCHOR_NONE;
	}
	if (wake) {
		/* EDGE 2's (D)PPI may already have started the TIMER with its
		 * interrupt still held off: put it back stopped and cleared -
		 * after the abort, so no START can land behind the STOP. */
		lp_timer_halt();
	}
}

/* Under CLOCK_LOCK, TIMER running. A failure is counted and left: the next
 * release then takes the TIMER's reading, as it would without the re-anchor. */
static void lp_reanchor_arm_locked(void)
{
	(void)lp_set_anchor_locked(radiant_lp_release_edge(&lp.tb, lp_ticks(), LP_MIN_LEAD),
				   LP_ANCHOR_REANCHOR);
}

/* ---- Decisions ---- */

static bool lp_should_sleep_locked(uint64_t now, uint64_t next)
{
	return radiant_lp_should_sleep(now, next, lp.wake_lead_us, lp.lf_overhead_us,
				       CONFIG_RADIANT_NRF_LP_KEEP_ON_US, lp.linger_until);
}

/* Commit a release: arm its edge `lead` ticks on. Under CLOCK_LOCK, state
 * AWAKE. False (still awake, nothing armed) on a late anchor. */
static bool lp_commit_release_locked(uint32_t lead)
{
	uint64_t edge = radiant_lp_release_edge(&lp.tb, lp_ticks(), lead);

	if (lp.anchor == LP_ANCHOR_REANCHOR) {
		/* Superseded: the release captures the same TIMER. */
		lp_cnt_abort(lp.ch_anchor);
		lp.anchor = LP_ANCHOR_NONE;
	}
	if (lp.hf == LP_HF_LINGER) {
		lp_cancel_hf_locked();
	}
	if (!lp_set_anchor_locked(edge, LP_ANCHOR_RELEASE)) {
		return false;
	}
	lp_set_state(LP_RELEASING);
	return true;
}

/* Call off a committed release for a nearer arm, only while its compare is
 * certainly still ahead. Under CLOCK_LOCK, state RELEASING. */
static bool lp_cancel_release_locked(void)
{
	if (!radiant_lp_compare_in_time(&lp.tb, lp_ticks(), lp.anchor_tick,
					LP_POSTWRITE_LEAD)) {
		return false;
	}
	lp_cnt_abort(lp.ch_anchor);
	lp.anchor = LP_ANCHOR_NONE;
	lp_set_state(LP_AWAKE);
	LP_STAT(radiant_nrf_lp_stats.release_cancels++);
	return true;
}

static void lp_want_locked(uint64_t t_from)
{
	lp.want = true;
	lp.want_t = t_from;
	lp.grant_owed = false;
}

static void lp_late_anchor_locked(void);

/*
 * Wake now: request the HFXO in software and place EDGE 2 one start-up later
 * (or at the wanted operation's own EDGE 2 if that is later still). Under
 * CLOCK_LOCK, state ASLEEP. May leave LP_ACT_SETTLE owed (a synchronous
 * software start) or degrade.
 */
static void lp_wake_asap_locked(void)
{
	uint64_t now_t;
	uint64_t e2;
	uint64_t ideal;

	lp_cancel_edges_locked();
	lp_set_state(LP_WAKING);
	lp_hfxo_on_locked();
	if (lp.state == LP_OFF) {
		return;
	}
	LP_STAT(radiant_nrf_lp_stats.asap_wakes++);
	now_t = lp_ticks();
	e2 = lp.hfxo_ready ? now_t + LP_MIN_LEAD
			   : lp.hfxo_req_tick + lp_us_to_ticks_ceil(LP_HFXO_LEAD_US);
	if (e2 < now_t + LP_MIN_LEAD) {
		e2 = now_t + LP_MIN_LEAD;
	}
	if (lp.want &&
	    radiant_lp_wake_compare(&lp.tb, &lp.clk,
				    radiant_lp_wake_at(lp.want_t, LP_ARM_LEAD_US),
				    LP_MAX_SLEEP_TICKS, &ideal) == RADIANT_LP_WAKE_OK &&
	    ideal > e2) {
		e2 = ideal;
	}
	if (!lp_set_anchor_locked(e2, LP_ANCHOR_WAKE)) {
		lp_late_anchor_locked();
	}
}

/* Plan the wake for the wanted operation. Under CLOCK_LOCK, state ASLEEP. */
static void lp_plan_locked(void)
{
	struct radiant_lp_wake_plan p;
	enum radiant_lp_wake_rc rc;

	lp_cancel_edges_locked();
	if (!lp.want) {
		return;   /* nothing to wake for: the extended counter never wraps */
	}
	rc = radiant_lp_wake_plan(&lp.tb, &lp.clk, lp.want_t, LP_ARM_LEAD_US,
				  LP_HFXO_LEAD_US, LP_MIN_LEAD, LP_MAX_SLEEP_TICKS, &p);
	if (rc == RADIANT_LP_WAKE_CLAMPED) {
		if (lp_set_hf_locked(p.timer_cmp, LP_HF_REBASE)) {
			return;
		}
	} else if (rc == RADIANT_LP_WAKE_OK &&
		   radiant_lp_compare_in_time(&lp.tb, lp_ticks(), p.hfxo_cmp, LP_MIN_LEAD)) {
		if (lp_set_anchor_locked(p.timer_cmp, LP_ANCHOR_WAKE) &&
		    lp_set_hf_locked(p.hfxo_cmp, LP_HF_EDGE1)) {
			return;
		}
		lp_cancel_edges_locked();
	}
	lp_wake_asap_locked();
}

/*
 * A new operation wanted while the wake is under way. The wake only moves
 * earlier: a later operation is decided again once awake (lp_settle()).
 * Under CLOCK_LOCK, state WAKING. May leave LP_ACT_SETTLE owed.
 */
static void lp_wake_earlier_locked(void)
{
	uint64_t t2;
	uint64_t now_t;
	uint64_t floor_t;

	if (lp.anchor != LP_ANCHOR_WAKE ||
	    radiant_lp_wake_compare(&lp.tb, &lp.clk,
				    radiant_lp_wake_at(lp.want_t, LP_ARM_LEAD_US),
				    LP_MAX_SLEEP_TICKS, &t2) == RADIANT_LP_WAKE_CLAMPED ||
	    t2 >= lp.anchor_tick) {
		return;
	}
	now_t = lp_ticks();
	floor_t = now_t + LP_MIN_LEAD;
	if (!lp.hfxo_ready) {
		uint64_t up = lp.hfxo_req_tick + lp_us_to_ticks_ceil(LP_HFXO_LEAD_US);

		floor_t = (up > floor_t) ? up : floor_t;
	}
	if (t2 < floor_t) {
		t2 = floor_t;
	}
	if (t2 >= lp.anchor_tick ||
	    !radiant_lp_compare_in_time(&lp.tb, now_t, lp.anchor_tick, LP_MIN_LEAD)) {
		return;
	}
	if (!lp_set_anchor_locked(t2, LP_ANCHOR_WAKE)) {
		lp_late_anchor_locked();
	}
}

/* ---- Edges ---- */

/*
 * RELEASE. Under CLOCK_LOCK, state RELEASING. Sleeps only if nothing was
 * claimed or programmed meanwhile and the wanted operation (or none) is
 * still far with no linger; otherwise the capture is a re-anchor and
 * LP_ACT_SETTLE is owed.
 */
static void lp_release_edge_locked(uint64_t edge)
{
	uint64_t timer_us;
	uint64_t now;
	int64_t  diff = 0;
	bool     exact = lp.clk.timer_at_anchor;
	bool     took_lf;
	bool     busy;

	if (!lp_capture_time(&timer_us)) {
		/* No capture on the edge: nothing is stopped. */
		LP_STAT(radiant_nrf_lp_stats.late_anchors++);
		lp_set_state(LP_AWAKE);
		lp.act |= LP_ACT_SETTLE;
		return;
	}
	now = lp_now_locked();
	/* An operation claimed and not staged runs (or is about to) on this TIMER. */
	busy = radiant_op.kind != OP_NONE && !radiant_staged.pending;
	if (busy || !lp_should_sleep_locked(now, lp.want ? lp.want_t : RADIANT_LP_NO_OP)) {
		if (radiant_lp_reanchor(&lp.tb, &lp.clk, edge, timer_us)) {
			LP_STAT(radiant_nrf_lp_stats.reanchors++);
		}
		LP_STAT(radiant_nrf_lp_stats.release_cancels++);
		lp_set_state(LP_AWAKE);
		lp.act |= LP_ACT_SETTLE;
		return;
	}
	/* timer_us came from CC5 above: read before the halt (SHUTDOWN). */
	lp_timer_halt();
	took_lf = radiant_lp_release(&lp.tb, &lp.clk, edge, timer_us,
				     RADIANT_LP_RELEASE_TOL_US, &diff);
	ARG_UNUSED(took_lf);
	ARG_UNUSED(exact);
	LP_STAT(if (took_lf) {
			radiant_nrf_lp_stats.lf_taken++;
		} else {
			radiant_nrf_lp_stats.timer_taken++;
		}
		if (exact) {
			uint64_t mag = (uint64_t)((diff < 0) ? -diff : diff) >> lp.tb.shift;

			radiant_nrf_lp_stats.last_disagree_q =
				(diff > INT32_MAX) ? INT32_MAX
				: (diff < INT32_MIN) ? INT32_MIN : (int32_t)diff;
			LP_STAT_MAX(max_disagree_us, mag);
			if (!took_lf) {
				radiant_nrf_lp_stats.disagreements++;
			}
		});
	lp_set_state(LP_ASLEEP);
	lp_hfxo_off_locked();
	LP_STAT(radiant_nrf_lp_stats.sleeps++);
	lp_plan_locked();
}

/* Software start on a stable HFXO (a late anchor's recovery). Under
 * CLOCK_LOCK. Leaves LP_ACT_SETTLE owed. */
static void lp_sw_start_locked(void)
{
	lp_timer_sw_start_locked();
	lp.sw_start_pending = false;
	lp.just_woke = true;
	lp_set_state(LP_AWAKE);
	LP_STAT(radiant_nrf_lp_stats.wakes++);
	lp_reanchor_arm_locked();
	lp.act |= LP_ACT_SETTLE;
}

/* EDGE 2 cannot be trusted. Under CLOCK_LOCK, state WAKING. */
static void lp_late_anchor_locked(void)
{
	if (lp.anchor != LP_ANCHOR_NONE) {
		lp_cnt_abort(lp.ch_anchor);
		lp.anchor = LP_ANCHOR_NONE;
	}
	lp.fail_owed = lp.want;
	if (lp.hfxo_ready) {
		lp_sw_start_locked();
	} else {
		lp.sw_start_pending = true;
	}
}

/* EDGE 2. Under CLOCK_LOCK, state WAKING. */
static void lp_wake_edge_locked(uint64_t edge)
{
	uint32_t cc5 = nrf_timer_cc_get(TIMER_ADDR, LP_CC_ANCHOR);

	if (cc5 == LP_CC5_UNSET) {
		/* The edge did not start the TIMER. */
		LP_STAT(radiant_nrf_lp_stats.late_anchors++);
		lp_late_anchor_locked();
		return;
	}
	radiant_clock.base = radiant_lp_wake(&lp.tb, &lp.clk, edge, lp.hfxo_ready);
	radiant_clock.last = cc5;
	lp.just_woke = true;
	lp_set_state(LP_AWAKE);
	LP_STAT(radiant_nrf_lp_stats.wakes++);
	if (!lp.hfxo_ready) {
		/* Granted from the HFXO callback, after a re-anchor. */
		lp.hfxo_wait = true;
		LP_STAT(radiant_nrf_lp_stats.hfxo_late_edge2++);
		return;
	}
	lp.act |= LP_ACT_SETTLE;
}

static void lp_anchor_handler(int32_t id, uint64_t expire, void *user)
{
	unsigned int key;
	uint8_t      what;

	ARG_UNUSED(id);
	ARG_UNUSED(user);

	key = CLOCK_LOCK();
	what = lp.anchor;
	if (what == LP_ANCHOR_NONE || expire != lp.anchor_tick) {
		LP_STAT(radiant_nrf_lp_stats.spurious++);
		CLOCK_UNLOCK(key);
		return;
	}
	lp.anchor = LP_ANCHOR_NONE;
	switch (what) {
	case LP_ANCHOR_RELEASE:
		if (lp.state == LP_RELEASING) {
			lp_release_edge_locked(expire);
		}
		break;
	case LP_ANCHOR_WAKE:
		if (lp.state == LP_ASLEEP) {
			/*
			 * EDGE 2 serviced before EDGE 1: the interrupt was held
			 * off past both, and the driver services the lower
			 * channel first (nrfx's GRTC loop and nrf_rtc_timer's
			 * both go upwards; which of our two channels is lower
			 * depends on the allocation). The (D)PPI started the
			 * TIMER on this tick all the same, so do EDGE 1's work
			 * and take the edge; the HFXO is not up yet, so it is
			 * the hfxo_wait path.
			 */
			lp_cancel_hf_locked();
			lp_set_state(LP_WAKING);
			LP_STAT(radiant_nrf_lp_stats.edge_order++);
			lp_hfxo_on_locked();
		}
		if (lp.state == LP_WAKING) {
			lp_wake_edge_locked(expire);
		}
		break;
	default:
		if (lp.state == LP_AWAKE) {
			uint64_t timer_us;

			if (!lp_capture_time(&timer_us)) {
				LP_STAT(radiant_nrf_lp_stats.late_anchors++);
			} else if (radiant_lp_reanchor(&lp.tb, &lp.clk, expire, timer_us)) {
				LP_STAT(radiant_nrf_lp_stats.reanchors++);
			}
		}
		break;
	}
	CLOCK_UNLOCK(key);
	lp_flush();
}

static void lp_hf_handler(int32_t id, uint64_t expire, void *user)
{
	unsigned int key;
	uint8_t      what;

	ARG_UNUSED(id);
	ARG_UNUSED(user);

	key = CLOCK_LOCK();
	what = lp.hf;
	if (what == LP_HF_NONE || expire != lp.hf_tick ||
	    lp.state != ((what == LP_HF_LINGER) ? LP_AWAKE : LP_ASLEEP)) {
		LP_STAT(radiant_nrf_lp_stats.spurious++);
		CLOCK_UNLOCK(key);
		return;
	}
	lp.hf = LP_HF_NONE;
	if (what == LP_HF_REBASE) {
		radiant_lp_rebase(&lp.tb, &lp.clk, expire);
		lp_plan_locked();
	} else if (what == LP_HF_LINGER) {
		lp.act |= LP_ACT_RECONSIDER;
	} else {
		/* EDGE 1. */
		lp_set_state(LP_WAKING);
		lp_hfxo_on_locked();
	}
	CLOCK_UNLOCK(key);
	lp_flush();
}

/* ---- After a wake, a grant, a terminal ---- */

/* Fail the staged operation now. No lock held; never inside an arm call. */
static void lp_fail_staged(void)
{
	unsigned int key = irq_lock();
	bool was = radiant_staged.pending;

	radiant_staged.pending = false;
	irq_unlock(key);
	if (was) {
		lp_count_late_wake();
		deliver_terminal(RADIANT_RADIO_STATUS_FAILED);
	}
}

/* Programme the staged operation. No lock held, state AWAKE on the HFXO,
 * never inside an arm call. */
static void lp_grant(void)
{
	radiant_nrf_gate_on_grant();
	if (radiant_op.gate_failed) {
		/* program_*()'s ETIME: the wake came too late for it. */
		lp_count_late_wake();
		radiant_nrf_gate_finish_failed();
	}
}

/*
 * A FAILED owed to the operation an arm call is staging, delivered from the
 * system work queue so it never runs inside that arm call, and only if the
 * same operation is still the live one.
 */
static void lp_fail_work_fn(struct k_work *w)
{
	unsigned int key;
	bool go;

	ARG_UNUSED(w);
	/*
	 * The check and the terminal under one lock: released in between, an
	 * interrupt could end this operation and arm the next, which would then
	 * take the FAILED. The core callback behind deliver_terminal() runs with
	 * interrupts off here, as its scheduler pass does in any case; this is
	 * the work queue, so nothing else is held up but interrupts.
	 */
	key = irq_lock();
	go = lp.defer_fail && radiant_op.id == lp.defer_op &&
	     radiant_op.kind != OP_NONE && !radiant_op.terminal_sent;
	lp.defer_fail = false;
	if (go) {
		radiant_staged.pending = false;
		radiant_op.gate_failed = false;
		LP_STAT(radiant_nrf_lp_stats.late_wakes++);
		deliver_terminal(RADIANT_RADIO_STATUS_FAILED);
	}
	irq_unlock(key);
}

static K_WORK_DEFINE(lp_fail_work, lp_fail_work_fn);

static void lp_defer_fail(void)
{
	unsigned int key = irq_lock();

	radiant_staged.pending = false;
	lp.defer_fail = true;
	lp.defer_op = radiant_op.id;
	irq_unlock(key);
	(void)k_work_submit(&lp_fail_work);
}

/*
 * Sleep again if nothing is armed (RADIANT_LP_NO_OP) and no linger holds; if
 * only a linger holds, come back when it ends. No lock held.
 */
static void lp_reconsider(void)
{
	unsigned int key;

	if (lp.state == LP_OFF) {
		return;
	}
	key = CLOCK_LOCK();
	if (lp.state == LP_AWAKE && !lp.hfxo_wait && !lp.want &&
	    radiant_op.kind == OP_NONE) {
		uint64_t now = lp_now_locked();

		if (lp_should_sleep_locked(now, RADIANT_LP_NO_OP)) {
			(void)lp_commit_release_locked(LP_RELEASE_GRACE);
		} else if (now < lp.linger_until && lp.hf == LP_HF_NONE) {
			uint64_t t = lp_ticks();
			uint64_t tick = t + lp_us_to_ticks_ceil(lp.linger_until - now);

			(void)lp_set_hf_locked((tick > t + LP_MIN_LEAD) ? tick : t + LP_MIN_LEAD,
					       LP_HF_LINGER);
		}
	}
	CLOCK_UNLOCK(key);
}

/*
 * Awake on a stable HFXO, after a wake or a release that did not happen:
 * grant the wanted operation, fail it (late anchor), sleep again if it turned
 * out to be far (the wake only moves earlier), or reconsider with nothing
 * wanted. No lock held; never inside an arm call.
 */
static void lp_settle(void)
{
	unsigned int key = CLOCK_LOCK();
	bool fail = false;
	bool grant = false;

	if (lp.state != LP_AWAKE || lp.hfxo_wait) {
		CLOCK_UNLOCK(key);
		return;
	}
	if (lp.want) {
		uint64_t now = lp_now_locked();

		if (lp.fail_owed) {
			fail = radiant_staged.pending;
		} else if (lp_should_sleep_locked(now, lp.want_t) &&
			   lp_commit_release_locked(LP_MIN_LEAD)) {
			lp.just_woke = false;
			CLOCK_UNLOCK(key);
			return;
		} else {
			if (lp.just_woke) {
				LP_STAT_MAX(max_wake_slack_us,
					    (lp.want_t > now) ? lp.want_t - now : 0u);
				LP_STAT_MAX(max_wake_late_us,
					    radiant_lp_late_by_us(now, lp.want_t,
								  ARM_SETUP_US));
			}
			grant = radiant_staged.pending;
		}
		if (fail || grant) {
			lp.want = false;
			lp.fail_owed = false;
		} else {
			/* The arm is still copying its request: lp_staged() finishes. */
			lp.grant_owed = true;
		}
	}
	lp.just_woke = false;
	CLOCK_UNLOCK(key);
	if (fail) {
		lp_fail_staged();
	} else if (grant) {
		lp_grant();
	}
	lp_reconsider();
}

/* Run what the lock owed. No lock held; never inside an arm call. */
static void lp_flush(void)
{
	unsigned int key = CLOCK_LOCK();
	uint8_t act = lp.act;

	lp.act = 0u;
	CLOCK_UNLOCK(key);
	if ((act & LP_ACT_FAIL) != 0u) {
		lp_fail_staged();
	}
	if ((act & LP_ACT_SETTLE) != 0u) {
		lp_settle();
	} else if ((act & LP_ACT_RECONSIDER) != 0u) {
		lp_reconsider();
	}
}

/*
 * LP_HOOK: an arm call has just staged its request (radiant_staged.pending).
 * Inside the arm call, so it programmes but never delivers: a FAILED goes to
 * the work queue.
 */
static void lp_staged(void)
{
	unsigned int key = CLOCK_LOCK();
	bool owed = lp.grant_owed;
	bool fail = lp.fail_owed;

	if (owed) {
		lp.grant_owed = false;
		lp.want = false;
		lp.fail_owed = false;
	}
	CLOCK_UNLOCK(key);
	if (!owed) {
		return;
	}
	if (!fail) {
		radiant_nrf_gate_on_grant();
		fail = radiant_op.gate_failed;
	}
	if (fail) {
		lp_defer_fail();
	}
}

/* LP_HOOK: the end of deliver_terminal(), after the core's callback. */
static void lp_terminal_done(void)
{
	lp_reconsider();
}

/* LP_HOOK: radiant_radio_enable(). Linger, and be awake for what follows. */
static void lp_on_enable(void)
{
	unsigned int key;

	if (lp.state == LP_OFF) {
		return;
	}
	key = CLOCK_LOCK();
	lp.linger_until = radiant_lp_linger(lp.linger_until, lp_now_locked(), LP_LINGER_US);
	if (lp.state == LP_ASLEEP) {
		lp_wake_asap_locked();
	} else if (lp.state == LP_AWAKE) {
		/* Come back at the linger's end. (A RELEASING edge sees the
		 * linger and stays awake.) */
		lp.act |= LP_ACT_RECONSIDER;
	}
	CLOCK_UNLOCK(key);
	lp_flush();
}

/* ---- The gate (radiant_radio_nrf_gate.h), in place of the direct gate ---- */

enum gate_rc gate_acquire(enum gate_op op, radiant_time_t t_from,
			  radiant_time_t t_to, uint16_t follow_on_us,
			  uint8_t prio)
{
	unsigned int key;
	enum gate_rc rc = GATE_GRANTED;
	uint64_t now;

	ARG_UNUSED(op);
	ARG_UNUSED(t_to);
	ARG_UNUSED(follow_on_us);

	if (lp.state == LP_OFF) {
		return GATE_GRANTED;
	}
	key = CLOCK_LOCK();
	now = lp_now_locked();
	if (prio == RADIANT_GATE_PRIO_NORMAL) {
		lp.linger_until = radiant_lp_linger(lp.linger_until, now, LP_LINGER_US);
	}
	switch (lp.state) {
	case LP_AWAKE:
		if (lp.hfxo_wait) {
			/* On HFINT: granted from the HFXO callback. */
			lp_want_locked(t_from);
			rc = GATE_PENDING;
		} else if (lp_should_sleep_locked(now, t_from) &&
			   lp_commit_release_locked(LP_MIN_LEAD)) {
			lp_want_locked(t_from);
			rc = GATE_PENDING;
		}
		break;
	case LP_RELEASING:
		if (!lp_should_sleep_locked(now, t_from)) {
			/* The TIMER still runs on the crystal: programme now.
			 * Calling the edge off spares an interrupt; if it is
			 * too close for that, the edge sees this operation and
			 * stays awake. */
			(void)lp_cancel_release_locked();
			break;
		}
		lp_want_locked(t_from);   /* planned by the release edge */
		rc = GATE_PENDING;
		break;
	case LP_ASLEEP:
		lp_want_locked(t_from);
		lp_plan_locked();
		rc = GATE_PENDING;
		break;
	default: /* LP_WAKING */
		lp_want_locked(t_from);
		lp_wake_earlier_locked();
		rc = GATE_PENDING;
		break;
	}
	/*
	 * Nothing is delivered from inside an arm call. If this call's own path
	 * left the TIMER running on the crystal (a software start after a late
	 * anchor) or dropped to always-on, the arm programmes now and its ETIME
	 * check decides.
	 */
	if (lp.state == LP_OFF ||
	    ((lp.act & LP_ACT_SETTLE) != 0u && lp.state == LP_AWAKE && !lp.hfxo_wait)) {
		lp.act &= (uint8_t)~(LP_ACT_SETTLE | LP_ACT_FAIL);
		lp.want = false;
		lp.fail_owed = false;
		rc = GATE_GRANTED;
	}
	CLOCK_UNLOCK(key);
	return rc;
}

void gate_release(void)
{
	unsigned int key = CLOCK_LOCK();

	/* The operation ended or failed: nothing is wanted any more. */
	lp.want = false;
	lp.grant_owed = false;
	lp.fail_owed = false;
	CLOCK_UNLOCK(key);
}

void gate_forget_pending(void)
{
	unsigned int key = CLOCK_LOCK();

	lp.want = false;
	lp.grant_owed = false;
	lp.fail_owed = false;
	/* Asleep, EDGE 1 has not fired and EDGE 2 is a start-up beyond it, so
	 * both can go; the wake is spared. Once waking, it runs its course. */
	if (lp.state == LP_ASLEEP) {
		lp_cancel_edges_locked();
	}
	CLOCK_UNLOCK(key);
}

bool gate_held(void)
{
	return false;
}

bool gate_in_signal(void)
{
	return false;
}

bool gate_extend(uint32_t us)
{
	ARG_UNUSED(us);
	return false;
}

int gate_init(void)
{
	return RADIANT_RADIO_OK_RC;
}

void gate_shutdown(void)
{
}

uint32_t gate_min_arm_lead_us(void)
{
	/* Unchanged: an arm inside the wake lead fails late, counted, rather
	 * than widening every window's exclusion radius. */
	return 0u;
}

uint32_t gate_max_window_us(void)
{
	return 0u;
}

/* ---- Init ---- */

static void lp_refuse(const char *why, int err)
{
	if (lp.conn_alloc) {
		nrfx_gppi_conn_free(lp_cnt_evt_address(lp.ch_anchor),
				    nrf_timer_task_address_get(TIMER_ADDR, NRF_TIMER_TASK_START),
				    lp.conn_anchor);
		lp.conn_alloc = false;
	}
	if (lp.ch_anchor >= 0) {
		lp_cnt_chan_free(lp.ch_anchor);
		lp.ch_anchor = -1;
	}
	if (lp.ch_hf >= 0) {
		lp_cnt_chan_free(lp.ch_hf);
		lp.ch_hf = -1;
	}
	lp_set_state(LP_OFF);
	LP_STAT(radiant_nrf_lp_stats.refused = 1u);
	LOG_WRN("low power refused (%s, %d): the HFXO stays on as without "
		"CONFIG_RADIANT_NRF_LOW_POWER", why, err);
}

/*
 * After the TIMER runs and the backend's connections exist. Any failure
 * leaves the option off for this boot (state LP_OFF: always awake, the boot
 * HFXO request and CONSTLAT held), which is the build without it.
 */
static void lp_init(void)
{
	nrf_clock_lfclk_t src = (nrf_clock_lfclk_t)0;
	unsigned int key;
	int err;

	if (lp.state != LP_OFF) {
		return;
	}
	if (!radiant_lp_tb_init(&lp.tb, LP_CNT_NUM, LP_CNT_SHIFT,
				RADIANT_LP_EXTENDED_WIDTH)) {
		lp_refuse("counter format", -EINVAL);
		return;
	}
	/* The gaps are timed on the LF clock: only the crystal will do. */
	z_nrf_clock_control_lf_on(CLOCK_CONTROL_NRF_LF_START_STABLE);
	if (!nrf_clock_is_running(NRF_CLOCK, NRF_CLOCK_DOMAIN_LFCLK, &src) ||
	    src != NRF_CLOCK_LFCLK_XTAL) {
		lp_refuse("LFCLK is not running from the crystal", (int)src);
		return;
	}
	lp.ch_anchor = lp_cnt_chan_alloc();
	lp.ch_hf = (lp.ch_anchor >= 0) ? lp_cnt_chan_alloc() : -1;
	if (lp.ch_anchor < 0 || lp.ch_hf < 0) {
		lp_refuse("no free " LP_CNT_NAME " channel", -ENOMEM);
		return;
	}
	err = nrfx_gppi_conn_alloc(lp_cnt_evt_address(lp.ch_anchor),
				   nrf_timer_task_address_get(TIMER_ADDR, NRF_TIMER_TASK_START),
				   &lp.conn_anchor);
	if (err == 0) {
		lp.conn_alloc = true;
		err = nrfx_gppi_ep_attach(nrf_timer_task_address_get(
						  TIMER_ADDR,
						  nrf_timer_capture_task_get(LP_CC_ANCHOR)),
					  lp.conn_anchor);
	}
	if (err != 0) {
		lp_refuse("anchor (D)PPI connection", err);
		return;
	}
	/* Permanent: START on a running TIMER does nothing. */
	nrfx_gppi_conn_enable(lp.conn_anchor);

	radiant_lp_clock_init(&lp.clk);
	lp.wake_lead_us = radiant_lp_wake_lead_us(&lp.tb, LP_ARM_LEAD_US, LP_HFXO_LEAD_US);
	lp.lf_overhead_us = radiant_lp_lf_overhead_us(&lp.tb, LP_MIN_LEAD);

	key = CLOCK_LOCK();
	lp.clk.hwm_us = clock_fold(timer_capture(CC_NOW));
	lp_set_state(LP_AWAKE);
	/* The boot TIMER has free-run since before any LF edge was known. */
	lp_reanchor_arm_locked();
	CLOCK_UNLOCK(key);

	LOG_INF("low power on: sleep beyond %u us (wake lead %u, LF %u, keep-on %u), "
		LP_CNT_NAME " ch %d/%d", (unsigned)(lp.wake_lead_us + lp.lf_overhead_us +
					    CONFIG_RADIANT_NRF_LP_KEEP_ON_US),
		(unsigned)lp.wake_lead_us, (unsigned)lp.lf_overhead_us,
		(unsigned)CONFIG_RADIANT_NRF_LP_KEEP_ON_US, (int)lp.ch_anchor,
		(int)lp.ch_hf);
}

#else /* LP_MPSL */

/* ---- The MPSL build (pass B6): the timeslot is the wake ----
 *
 * WHY NOTHING ABOVE APPLIES. Under the MPSL gate this backend touches the RADIO
 * only inside an MPSL timeslot, and the gate asks for every timeslot with
 * MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED (radiant_radio_nrf_gate_mpsl.c,
 * GATE_HFCLK_CFG): MPSL starts the HFXO before SIGNAL_START, delivers the
 * signal only once it is running and stable, and keeps it until the timeslot
 * ends. So the timeslot itself is the wake, with its crystal start already
 * paid for, and a timeslot that MPSL cannot start on a stable crystal is one
 * it does not start (BLOCKED or CANCELLED, which the gate already turns into
 * DENIED). There is no EDGE 1 to be late for and no TIMER to start on HFINT.
 *
 * What this section does instead, called by the gate:
 *
 *   radiant_nrf_gate_lp_slot_start()   SIGNAL_START, first thing. Start the
 *       stopped and cleared TIMER, from zero, right after a read of the GRTC
 *       SYSCOUNTER, both under CLOCK_LOCK (PRIMASK, in the zero-latency
 *       signal): the awake segment is anchored to that tick as an EDGE 2
 *       (D)PPI start anchors it, so radiant_lp_wake(..., true). The error is
 *       the read-to-START latency (well under 1 us; the read retries only
 *       while the counter reports BUSY) plus the 1 us tick: the same bound as
 *       the (D)PPI start, whose spike B1 S3 errors were {-1, 0} us.
 *   radiant_nrf_gate_lp_slot_end()     the funnel every high-priority
 *       ACTION_END passes, after the hand-back, every radiant_radio_abort()
 *       and the RADIO vector clear: read the GRTC, capture the TIMER, stop
 *       and clear it, and take the release (radiant_lp_release: the LF image
 *       when the two agree within RADIANT_LP_RELEASE_TOL_US, which a timeslot
 *       of a few ms always does). From then until the next START
 *       radiant_radio_now() is the GRTC's. Also called from the gate's
 *       low-priority backstops (an expiry-ended timeslot, a session closing)
 *       and gate_shutdown(), where a TIMER still running is a missed release.
 *
 * Both are reads made by an awake CPU (spike B1's stale-capture trap is a DPPI
 * capture while the CPU sleeps). The Zephyr GRTC driver's compare and abort
 * calls take a kernel lock and are never made from the zero-latency signal:
 * no GRTC channel, no (D)PPI connection and no HFXO request is used after
 * init.
 *
 * WHY NOT THE DIRECT BUILD'S TWO-EDGE WAKE AHEAD OF THE START. It would need
 * the backend's own HFXO request a start-up ahead of each timeslot, two GRTC
 * interrupts per slot, and compare writes the zero-latency context may not
 * make, and it would add a failure mode the gate does not have (an EDGE 1 held
 * off by a BLE event loses the slot). It buys nothing on accuracy: both
 * anchors are one 1 us GRTC tick. Re-anchoring at every START also means no
 * error carries from one timeslot into the next.
 *
 * THE MISSED RELEASE. If a START finds the TIMER still running (an earlier
 * timeslot ended without passing the funnel), MPSL may have released the HFXO
 * after that timeslot and the TIMER may have counted part of the gap on HFINT.
 * The release then takes the LF image whatever the TIMER says, and is counted
 * (missed_releases).
 *
 * CONSTLAT is MPSL's: its per-event acquire covers the timeslots
 * (RADIANT_NRF54L_MPSL_CONSTLAT_HOLD, default n), so lp_constlat() is empty
 * here.
 */

#if !defined(CONFIG_NRF_GRTC_TIMER)
#error "RADIANT_NRF_LOW_POWER on an MPSL build anchors in software at SIGNAL_START, which needs the GRTC's 1 us tick (Kconfig refuses RTC1 here)"
#endif

/* A missed release's tolerance: take the LF image whatever the TIMER read. */
#define LP_MISSED_TOL_US 1000000u

/* Where the TIMER last started (LF ticks): the awake-time stat. */
static uint64_t lp_slot_t0;

/*
 * The release: the TIMER's reading of the LF tick `t` read just before it,
 * then the TIMER stopped and cleared. Under CLOCK_LOCK, state AWAKE. `tol_us`:
 * RADIANT_LP_RELEASE_TOL_US, or LP_MISSED_TOL_US for a missed release. Read
 * in the wake's order (the counter, then the TIMER), so the read-to-TIMER
 * latency is the same at both ends of the segment and cancels.
 */
static void lp_slot_release_locked(uint32_t tol_us, bool count)
{
	uint64_t t = lp_cnt_read();
	uint64_t timer_us = clock_fold(timer_capture(CC_NOW));
	int64_t  diff = 0;
	bool     exact = lp.clk.timer_at_anchor;
	bool     took_lf;

	lp_timer_halt();
	/*
	 * The TIMER restarts from zero at the next START, where a compare left
	 * from this timeslot would fire early, inside that timeslot's operation:
	 * its TIMER-driven connections stop with it. Every terminal disables
	 * them already; this covers a timeslot that ended with an operation
	 * still armed (its DENIED comes later, from the gate's work).
	 */
	nrfx_gppi_conn_disable(radiant_op.conn_start);
	nrfx_gppi_conn_disable(radiant_op.conn_start_tx);
	nrfx_gppi_conn_disable(radiant_op.conn_close);
	took_lf = radiant_lp_release(&lp.tb, &lp.clk, t, timer_us, tol_us, &diff);
	ARG_UNUSED(took_lf);
	ARG_UNUSED(exact);
	ARG_UNUSED(count);
	LP_STAT(if (count) {
			radiant_nrf_lp_stats.sleeps++;
			radiant_nrf_lp_stats.slot_awake_us +=
				(uint32_t)radiant_lp_ticks_to_us(&lp.tb, t - lp_slot_t0);
			if (took_lf) {
				radiant_nrf_lp_stats.lf_taken++;
			} else {
				radiant_nrf_lp_stats.timer_taken++;
			}
			if (exact) {
				uint64_t mag = (uint64_t)((diff < 0) ? -diff : diff) >>
					       lp.tb.shift;

				radiant_nrf_lp_stats.last_disagree_q =
					(diff > INT32_MAX) ? INT32_MAX
					: (diff < INT32_MIN) ? INT32_MIN : (int32_t)diff;
				LP_STAT_MAX(max_disagree_us, mag);
				if (!took_lf) {
					radiant_nrf_lp_stats.disagreements++;
				}
			}
		});
	lp_set_state(LP_ASLEEP);
}

/*
 * The wake: start the stopped TIMER from zero on the counter's current tick
 * and load the fold from that tick's asleep reading. Under CLOCK_LOCK, state
 * ASLEEP. r_start: the requested start of this timeslot, for the START
 * cross-check, or 0 for none.
 */
static void lp_slot_wake_locked(radiant_time_t r_start)
{
	uint64_t t;

	/* From zero, whatever a stray START may have done meanwhile. */
	nrf_timer_task_trigger(TIMER_ADDR, NRF_TIMER_TASK_STOP);
	nrf_timer_task_trigger(TIMER_ADDR, NRF_TIMER_TASK_CLEAR);
	t = lp_cnt_read();
	nrf_timer_task_trigger(TIMER_ADDR, NRF_TIMER_TASK_START);
	if (!radiant_lp_asleep_ok(&lp.tb, &lp.clk, t)) {
		/* A reading the asleep conversion cannot place (a lost
		 * timebase): counted, and the segment continues from the
		 * high-water mark rather than from garbage. */
		lp.clk.range_errors++;
		LP_STAT(radiant_nrf_lp_stats.range_errors = lp.clk.range_errors);
		lp.clk.anchor_q = lp.clk.hwm_us << lp.tb.shift;
		lp.clk.anchor_ticks = t & lp.tb.mask;
	}
	radiant_clock.base = radiant_lp_wake(&lp.tb, &lp.clk, t, true);
	radiant_clock.last = 0u;
	lp_slot_t0 = t;
	lp_set_state(LP_AWAKE);
	LP_STAT(radiant_nrf_lp_stats.wakes++);
	LP_STAT(if (r_start != 0u) {
			int64_t e = (int64_t)(radiant_clock.base - (uint64_t)r_start);
			int32_t e32 = (e > INT32_MAX) ? INT32_MAX
				      : (e < INT32_MIN) ? INT32_MIN : (int32_t)e;

			if (radiant_nrf_lp_stats.start_err_n == 0u ||
			    e32 < radiant_nrf_lp_stats.start_err_min_us) {
				radiant_nrf_lp_stats.start_err_min_us = e32;
			}
			if (radiant_nrf_lp_stats.start_err_n == 0u ||
			    e32 > radiant_nrf_lp_stats.start_err_max_us) {
				radiant_nrf_lp_stats.start_err_max_us = e32;
			}
			radiant_nrf_lp_stats.start_err_last_us = e32;
			radiant_nrf_lp_stats.start_err_n++;
		});
	ARG_UNUSED(r_start);
}

void radiant_nrf_gate_lp_slot_start(radiant_time_t r_start)
{
	unsigned int key;

	if (lp.state == LP_OFF) {
		return;
	}
	key = CLOCK_LOCK();
	if (lp.state == LP_AWAKE) {
		LP_STAT(radiant_nrf_lp_stats.missed_releases++);
		lp_slot_release_locked(LP_MISSED_TOL_US, true);
	}
	lp_slot_wake_locked(r_start);
	CLOCK_UNLOCK(key);
}

void radiant_nrf_gate_lp_slot_end(bool backstop)
{
	unsigned int key;

	if (lp.state == LP_OFF) {
		return;
	}
	key = CLOCK_LOCK();
	if (lp.state == LP_AWAKE) {
		if (backstop) {
			LP_STAT(radiant_nrf_lp_stats.missed_releases++);
		}
		lp_slot_release_locked(backstop ? LP_MISSED_TOL_US : RADIANT_LP_RELEASE_TOL_US,
				       true);
	}
	CLOCK_UNLOCK(key);
}

/* LP_HOOKs the direct build's state machine needs and this one does not:
 * nothing is staged behind a wake, and a terminal or an enable decides
 * nothing (the next timeslot's START does). */
static void lp_staged(void)
{
}

static void lp_terminal_done(void)
{
}

static void lp_on_enable(void)
{
}

/* ---- Init ---- */

static void lp_refuse(const char *why, int err)
{
	lp_set_state(LP_OFF);
	LP_STAT(radiant_nrf_lp_stats.refused = 1u);
	LOG_WRN("low power refused (%s, %d): the HFXO stays on as without "
		"CONFIG_RADIANT_NRF_LOW_POWER", why, err);
}

/*
 * After the TIMER runs and the backend's connections exist. radiant_radio_init()
 * opens the gate's sessions first, so a bootstrap timeslot may already have
 * come and gone (it never takes the radio, and its START found the option
 * still LP_OFF and did nothing); no operation can have been armed yet. Any
 * failure leaves the option off for this boot: the boot HFXO request held and
 * the TIMER free-running, which is the build without it.
 */
static void lp_init(void)
{
	nrf_clock_lfclk_t src = (nrf_clock_lfclk_t)0;
	unsigned int key;

	if (lp.state != LP_OFF) {
		return;
	}
	if (!radiant_lp_tb_init(&lp.tb, LP_CNT_NUM, LP_CNT_SHIFT,
				RADIANT_LP_EXTENDED_WIDTH)) {
		lp_refuse("counter format", -EINVAL);
		return;
	}
	/* The gaps between timeslots are timed on the LF clock: only the
	 * crystal will do. */
	z_nrf_clock_control_lf_on(CLOCK_CONTROL_NRF_LF_START_STABLE);
	if (!nrf_clock_is_running(NRF_CLOCK, NRF_CLOCK_DOMAIN_LFCLK, &src) ||
	    src != NRF_CLOCK_LFCLK_XTAL) {
		lp_refuse("LFCLK is not running from the crystal", (int)src);
		return;
	}

	radiant_lp_clock_init(&lp.clk);
	key = CLOCK_LOCK();
	lp.clk.hwm_us = clock_fold(timer_capture(CC_NOW));
	lp_set_state(LP_AWAKE);
	/* The boot TIMER has free-run since before any LF tick was known: its
	 * reading becomes the epoch (timer_at_anchor is false, so the TIMER's
	 * value is taken). Not a timeslot, so not counted. */
	lp_slot_release_locked(RADIANT_LP_RELEASE_TOL_US, false);
	CLOCK_UNLOCK(key);
	/* MPSL brings the crystal up for every timeslot from here on. Thread
	 * context; the onoff manager is not called under PRIMASK. */
	lp_hfxo_off_locked();

	LOG_INF("low power on (MPSL): TIMER stopped between timeslots, "
		"started from the " LP_CNT_NAME " at each START");
}

#endif /* LP_MPSL */
