/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf_gate_mpsl.c - the RADIO, borrowed from MPSL a slot at a
 * time.
 *
 * Provenance: clean-room, against nrfxlib's mpsl_timeslot.h and the in-tree
 * nrf/samples/mpsl/timeslot and nrf/drivers/mpsl/flash_sync patterns. Nothing
 * here derives from sdk-ant or libant.a.
 *
 * Measured on the nRF54L15 DK, 2026-08-12, against a real sensor: MPSL gate
 * and direct build both DATA=30 in 60 s (same packets). The gate placed 273
 * reservations, granted 272, 2716 extensions, blocked=0. Structural
 * difference: a scan chunk is 94 200 us here vs 260 000 us direct, because
 * caps.max_window_us bounds a window to what MPSL can grant, so sweep takes
 * ~3x the chunks for the same dwell; packet yield matches, sweep rate under
 * arbitration is not separately measured.
 *
 * Debugging instruments kept for good:
 *   - mpsl_assert_handle() below calls LOG_PANIC() before printk (deferred
 *     logging drops the message otherwise, since the thread that would flush
 *     it is the one being halted) and prints OVERSTAYED vs INVALID_RETURN,
 *     which need opposite fixes and are indistinguishable from the assert
 *     site alone.
 *   - a 16-entry signal ring buffer (trace[]), printed by the assert handler.
 *   - gate_dump_fn() under CONFIG_RADIANT_SWEEP_DEBUG: a once-a-second
 *     dump, because every other diagnostic here fires on an event and a
 *     stalled gate produces none.
 *   - to read a silent halt (MPSL's release build ships no assert file name,
 *     just k_fatal_halt() spinning in arch_system_halt()): halt over J-Link
 *     WITHOUT resetting (`h` then `regs`, never `r`), or state is destroyed:
 *       JLink.exe -NoGui 1 -SelectEmuBySN <sn> -CommanderScript <file>
 *       exec DisableAutoUpdateFW / si SWD / speed 4000 /
 *       device nRF54L15_M33 / connect / h / regs / q
 *   - nrf/samples/mpsl/timeslot, built for this board/SDK, is the reference
 *     for "does MPSL even work here" - 12 for 12 Timer0 signals, no assert.
 *
 * Fifteen bugs found and fixed, documented at their own sites; the last four
 * made it work and are all about the seam between an arm and the air:
 *  12. Inside a timeslot the RADIO interrupt is MPSL's, delivered as
 *      MPSL_TIMESLOT_SIGNAL_RADIO - the handler radiant_radio_nrf.c connects
 *      to RADIO_0_IRQn is never entered, so windows ran full length and never
 *      ended. This is the bug that made the gate deaf.
 *  13. An end-of-grant denial must not kill an operation armed microseconds
 *      earlier from inside the same callback (once 12 was fixed). The test
 *      has to be made when the work RUNS, not when it's queued.
 *  14. ACTION_END may be returned once. TIMER0 and RADIO race at window end;
 *      whichever loses must not end an already-ended timeslot - one guard at
 *      the single return statement.
 *  15. In radiant_radio_nrf.c: terminal_sent was cleared in program_rx(), not
 *      at the arm, so an operation waiting for air inherited the previous
 *      operation's `true` and could never be terminated. See CLAIM_TERMINAL().
 *
 *   1. Advertised arm lead = ARM_LEAD_US + the gate's placement lead,
 *      additive not max.
 *   2. The gate's too-near test needs a smaller floor than the advertised
 *      lead or it can never pass (same trap as min_arm_lead_us elsewhere).
 *   3. MAX_WINDOW_US must reserve everything added to a window after the
 *      scheduler caps it, or every arm is refused for a few hundred us of
 *      overrun.
 *   4. g.granted must be cleared in gate_release(), not at ACTION_END.
 *   5. MPSL_TIMER0's compare must be disarmed on every path that ends a
 *      grant, or an interrupt is left armed with nothing to clear its event.
 *   6. Every MPSL call and core call must be deferred to a work queue:
 *      arming reaches this file from the RADIO interrupt inside a timeslot,
 *      where mpsl_timeslot_request() is not callable.
 *   7. radiant_event's stack was overflowing (API_EVENT_STACK_SIZE in
 *      radiant_api.c) - silent because deferred logging needs a thread that
 *      had just died, and CONFIG_LOG_PRINTK=y routes printk through it too.
 *      CONFIG_LOG_MODE_IMMEDIATE + CONFIG_THREAD_NAME found it in one run.
 *
 * And one in radiant_api.c: a denial must not wake the event thread when the
 * search slot was kept, or a refusing gate spins at 27000 denials/sec.
 *
 * To reproduce the measurement:
 *   west build -b nrf54l15dk/nrf54l15/cpuapp -- -DANT_RADIO=core \
 *       -DRADIANT_BACKEND=nrf -DCONFIG_RADIANT_BACKEND_NRF_GATE_MPSL=y \
 *       -DCONFIG_MPSL=y -DCONFIG_MPSL_TIMESLOT_SESSION_COUNT=1 \
 *       -DCONFIG_RADIANT_SWEEP_DEBUG=y
 *
 * CONFIG_MPSL_TIMESLOT_SESSION_COUNT defaults to 0 (session context array
 * sized to nothing, mpsl_timeslot_session_open() fails at runtime - hence
 * gate_init() logging its rc). Use default deferred logging, not
 * CONFIG_LOG_MODE_IMMEDIATE (see warning below). Reset the board before
 * every run - an assert halts the CPU and a left-over board answers nothing.
 *
 * WARNING ABOUT THE INSTRUMENT ITSELF: with CONFIG_LOG_MODE_IMMEDIATE=y a
 * printk blocks ~87 us/char on a 115200 console - a 60-char trace line in
 * gate_acquire() is ~5 ms of blocking against the ~3 ms of lead the request
 * was built with. It moves the thing it measures and is trustworthy about
 * SEQUENCE, not TIMING. Do not put a printk back into gate_acquire() or
 * on_signal(); the ring buffer and deferred LOG_INF are the only instruments
 * that don't touch the grant path. Also: CONFIG_LOG_PRINTK=y (default)
 * routes printk through the log subsystem, so in deferred mode a printk from
 * a dying thread is silently dropped - "never appears" means DROPPED, not
 * "never executed".
 *
 * CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL is off by default; the direct
 * build stays green (core ztest 28 suites, api ztest 22, host tools 587,
 * DATA=30/60s). The only change to shipping code is CLAIM_TERMINAL() in
 * radiant_radio_nrf.c, moving an assignment from the programming half of an
 * arm to the claiming half - the same instant on a direct build.
 *
 * Every constant below is derived from a measurement in
 * radiant/spike/mpsl_arb/README.md; the four load-bearing ones:
 *   GRANT LATENCY (EARLIEST)  1692-1695 us (HFXO startup), tail 2760 us at
 *                             1-in-100 under a BLE advertiser.
 *   ANCHOR ERROR              0-17 us over 640 chained NORMAL requests, not
 *                             cumulative (MPSL_TIMESLOT_START_JITTER_US=0).
 *   BLOCKED TIMING            17 blocks, all 11-31 ms BEFORE the start they
 *                             referred to - why the deadline timer below is
 *                             a backstop rather than load-bearing.
 *   FREE TIMER CONTINUITY     -99 ppm across 321 timeslot edges, provided
 *                             the app holds its own HFXO request (else the
 *                             1 MHz timebase runs on internal RC between
 *                             grants: 0.42% fast). radiant_radio_nrf.c's
 *                             clock_control_on() at init holds it and must
 *                             not be removed on this build.
 *
 * Three rules dictate the shape:
 * 1. Exactly one request outstanding per session - mpsl_timeslot_request()
 *    answers -NRF_EAGAIN unless IDLE, matching the HAL's own one-operation
 *    rule 1:1.
 * 2. The first request must be EARLIEST, which is forbidden from inside a
 *    timeslot - so a session bootstraps from thread context before any real
 *    work, and every later request is NORMAL, measured from the previous
 *    timeslot's START.
 * 3. Returning anything but ACTION_NONE from a low-priority signal
 *    (BLOCKED/CANCELLED/SESSION_IDLE/SESSION_CLOSED, all in
 *    mpsl_low_priority_process()) asserts. Recovery is the core's, through
 *    the DENIED terminal.
 *
 * How a timeslot ends: only by returning ACTION_END from a signal callback,
 * but our operations end in the RADIO interrupt, not a signal callback. The
 * in-tree answer (esb.c, flash_sync_mpsl.c) is to keep an MPSL_TIMER0 compare
 * live: gate_release() writes a compare a few us ahead, MPSL delivers
 * SIGNAL_TIMER0, ACTION_END returns from there. MPSL_TIMER0 specifically, not
 * a private timer (esb.c enforces this with a BUILD_ASSERT too) - it's the
 * clock MPSL measures the overstay against; radiant's own 1 MHz TIMER is
 * the absolute timebase and is untouched here, and conflating the two is how
 * a backend asserts on a drift it can't see.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#if defined(CONFIG_RADIANT_SWEEP_DEBUG) && defined(CONFIG_INIT_STACKS)
/* For the stack high-water probe in gate_dump_fn(). See stacks_report(). */
#include <zephyr/debug/stack.h>
#endif

#include <mpsl_timeslot.h>
#include <mpsl_hwres.h>
#include <nrf_errno.h>
#include <hal/nrf_timer.h>

#include <radiant/radiant_swi.h>

#include "radiant_radio_nrf_gate.h"

LOG_MODULE_REGISTER(radiant_gate_mpsl, CONFIG_RADIANT_LOG_LEVEL);

/*
 * E1 of the Matter plan: the default-0 trap in this file's own header comment
 * (line 80), converted from a runtime rc into a compile error.
 *
 * CONFIG_MPSL_TIMESLOT_SESSION_COUNT carries `default 0`, which sizes MPSL's
 * session context array to nothing. With it, this file configures, compiles,
 * links and boots - gate_init() logs a failed mpsl_timeslot_session_open() and
 * the gate then never gets air. build/m_link/template/zephyr/.config from the
 * Matter flash spike is exactly that image: GATE_MPSL=y with COUNT=0. A
 * .config read-back cannot see it (both symbols are present and both look
 * right), so the check has to be here, where the two are related.
 *
 * Wrapped in `#ifdef CONFIG_MPSL` and not applied unconditionally, because
 * radiant/tests/gate builds THIS FILE with no MPSL at all - the fakes under
 * tests/gate/fakes/ shadow <mpsl_timeslot.h> precisely so the signal
 * sequences are the suite's rather than an arbiter's, and that image has no
 * session to size. Every real build reaches here through
 * CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL, which `select MPSL`, so the guard
 * excludes the fake and nothing else. An undefined symbol expands to nothing
 * rather than to 0, which inside BUILD_ASSERT is a syntax error rather than
 * the readable message below - hence the #ifndef as well.
 */
#ifdef CONFIG_MPSL
#ifndef CONFIG_MPSL_TIMESLOT_SESSION_COUNT
#error "CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL=y needs CONFIG_MPSL_TIMESLOT_SESSION_COUNT >= 1 (it is not defined at all here)"
#endif
BUILD_ASSERT(CONFIG_MPSL_TIMESLOT_SESSION_COUNT >= 1,
	     "CONFIG_MPSL_TIMESLOT_SESSION_COUNT defaults to 0, which sizes "
	     "MPSL's session array to nothing: the gate links and boots and "
	     "then never gets air. Set it to 2 (as apps/dongle_thread/"
	     "thread.conf does) - the SoftDevice Controller takes no public "
	     "session but the RRAM flash-sync driver adds one invisibly.");
#endif

/* ---------------------------------------------------------------------------
 * Margins
 * ---------------------------------------------------------------------------
 */

/*
 * How far before the caller's t_from the grant must start: this file's own
 * ARM_SETUP overhead plus margin for earliest-permitted grant jitter, on top
 * of the backend's own ARM_LEAD_US (168 us nRF54L / 328 us nRF52), which is
 * not re-derived here since the backend's t_from already accounts for its own
 * ramp-up and air lead.
 */
#define HEAD_MARGIN_US 250u

/*
 * The tail margin - overstaying asserts rather than merely losing a window.
 * Three measured/published terms: RADIO_DISABLE_MAX_US (100 us ramp-down,
 * radiant_radio_nrf.c), clock disagreement between MPSL's LFCLK/GRTC and the
 * backend's HFCLK-derived TIMER (measured -99 ppm/321 edges = 10 us over a
 * 100 ms grant, charged at 500 ppm = 50 us for margin), and signal slack
 * between the TIMER0 compare firing and ACTION_END returning (100 us, in
 * line with esb.c/flash_sync_mpsl.c's own figures). Total 250 us.
 */
#define TAIL_MARGIN_US 250u

/*
 * How far ahead a request must be placed to be honourable. Measured EARLIEST
 * grant latency is 1692-1695 us (HFXO startup); 2500 us covers that plus the
 * 2760 us tail sample. Nearer than this the arbiter cannot physically grant,
 * so counting its refusal as contention would be measuring ourselves.
 */
#define PLACE_LEAD_US CONFIG_RADIANT_GATE_MPSL_PLACE_LEAD_US

/*
 * The same number as a floor rather than a budget - and it must NOT equal
 * PLACE_LEAD_US. The core plans against the advertised lead using its own
 * `now`; the test below runs against GRANT START (t_from minus HEAD_MARGIN_US
 * minus the backend's air lead), sampled ~380 us later. Testing the
 * advertised figure against that later sample always fails if the two
 * constants are equal (measured: chunks=0, deny=517013/19s). So the
 * advertised lead (gate_min_arm_lead_us) carries the margin, and this is the
 * bare hardware minimum: measured grant latency rounded up toward the 2760 us
 * tail.
 */
#define PLACE_MIN_US CONFIG_RADIANT_GATE_MPSL_PLACE_MIN_US

/*
 * The longest single operation, reported as caps.max_window_us.
 * MPSL_TIMESLOT_LENGTH_MAX_US (100000) is an API ceiling, not policy - a
 * 250 ms ANT+ scan chunk cannot be requested whole.
 *
 * Everything added to a window after the scheduler has already capped it -
 * getting this wrong refused every arm (measured: a chunk needing 100184 us
 * total against MPSL's 100000 ceiling, every arm silently refused as `deny`).
 * caps.max_window_us bounds t_close - t_open in radiant_sched.c; on top of it,
 * invisibly to the scheduler: HEAD_MARGIN_US + TAIL_MARGIN_US, FRAME_TAIL_US
 * (radiant_radio_nrf.c's longest-legal-frame tail past t_close, ~136 us at
 * 1M/15B, 300 covers coded PHY too), and FOLLOW_ON_MAX_US (ack-data reserve,
 * up to ~2590 us on a listening master's slot).
 */
#define FRAME_TAIL_US    300u
#define FOLLOW_ON_MAX_US 3000u

/*
 * END_MARGIN_EXTEND_US (defined below) is included here because gate_acquire()
 * adds an end margin to every request length so the timeslot outlives the air
 * it covers by the hand-back time - same class of mistake as bug 3 if omitted.
 * The BUILD_ASSERT on MAX_WINDOW_US further down is what makes the forward
 * reference safe.
 *
 * The EXTEND margin specifically, and deliberately, even though the
 * non-extendable classes are now charged the smaller END_MARGIN_FIXED_US: this
 * feeds MAX_WINDOW_US, which is what the scheduler is told a window may be, and
 * that ceiling has to hold for the LONGEST overhead any class can carry.
 * Computing it from the smaller margin would hand the scheduler a ceiling an
 * elastic request then exceeds - bug 3 again, one indirection along.
 */
#define GRANT_OVERHEAD_US                                                     \
	(HEAD_MARGIN_US + TAIL_MARGIN_US + END_MARGIN_EXTEND_US +             \
	 FRAME_TAIL_US + FOLLOW_ON_MAX_US)

#define MAX_WINDOW_US (MPSL_TIMESLOT_LENGTH_MAX_US - GRANT_OVERHEAD_US)

/*
 * The extension chain: an in-flight grant grows itself rather than being
 * requested long up front, so the yield point is the arbiter's answer rather
 * than a constant here (ADR 0013). 1 ms a step; esb.c caps its own at 2000 to
 * bound clock drift, ours is bounded by MAX_WINDOW_US instead.
 */
#define EXTEND_STEP_US 1000u

/*
 * What an elastic request asks for up front - not what it wants. ADR 0013
 * makes the sweep the elastic consumer: a short grant grown by repeated
 * SIGNAL_ACTION_EXTEND, an extension only granted if MPSL has nothing else
 * scheduled in the extended region.
 *
 * Asking for the whole ~96 ms chunk up front does not work - measured as a
 * standing demand for ~96% of the radio that MPSL simply refuses (one
 * EARLIEST grant, then every NORMAL request BLOCKED forever, chunks=0). 10 ms
 * is the top of the plan's range: well below MPSL's maximum, long enough that
 * the extension chain is a handful of steps rather than a hundred.
 */
#define ELASTIC_INITIAL_US 10000u

/*
 * How far before the end of a grant the TIMER0 compare is placed, so
 * ACTION_END returns while the timeslot is still ours. NOT the same quantity
 * as TAIL_MARGIN_US - conflating them asserted on the first timeslot for a
 * whole debugging session. TAIL_MARGIN_US covers the RADIO (ramp-down, clock
 * disagreement); this covers the SIGNAL PATH (MPSL interrupt dispatch plus
 * callback work, all of which must finish before the grant expires).
 *
 * TWO OF THEM, split by grant class, because it is not the same question for
 * both and one number was being charged to both.
 *
 * END_MARGIN_EXTEND_US (2000) is the EXTENDABLE class's, unchanged. That class
 * asks MPSL to grow the grant from this very compare, and an extension has to
 * be requested at least MPSL_TIMESLOT_EXTENSION_MARGIN_MIN_US before the end of
 * the grant - so for an elastic slot the margin must contain the signal path
 * AND that minimum. The three BUILD_ASSERTs that follow bind to this one.
 *
 * END_MARGIN_FIXED_US (750) is the NON-EXTENDABLE class's: every transmit, and
 * every tracked receive window (under 10 ms, and its end is the core's choice,
 * not ours to grow). A non-extendable slot CAN NEVER REACH ACTION_EXTEND - the
 * SIGNAL_TIMER0 case is gated on g.extendable - so
 * MPSL_TIMESLOT_EXTENSION_MARGIN_MIN_US does not bind it at all, and the margin
 * only has to cover the SIGNAL PATH: MPSL's dispatch plus this callback's work
 * between the compare firing and ACTION_END returning. Nordic's own timeslot
 * sample measures that path at ~166 us (SIGNAL_START to TIMER0); 250 us is the
 * value MEASURED ASSERTING here during bring-up (the board asserted on the first
 * or second timeslot at len-250). 750 is 3x the figure that actually failed
 * rather than a fresh guess.
 *
 * What it buys: an end margin is charged to the RESERVATION, so it is air held
 * and not used. 1250 us comes off every tracked-window and every transmit
 * reservation - about 38 % of a ~7500 us tracked slot's footprint - and that is
 * the lever available against the 16.9 pp of 802.15.4 loss measured in
 * docs/radiant-bridge.md §7.3c, where the attributable cost is this stack's
 * DEMAND for air rather than arbitration itself.
 *
 * What it risks: an end margin that is too small is an MPSL OVERSTAY assert
 * (stats.overstayed, and the board halts), not a dropped packet. §7.3c carries
 * the soak gate this split has to clear before its numbers are re-taken.
 */
#define END_MARGIN_EXTEND_US 2000u
#define END_MARGIN_FIXED_US  750u

/*
 * How far ahead of the counter gate_release() places the compare that
 * provokes the end of a grant it no longer needs - big enough for
 * CAPTURE1/CC0/MPSL to all land inside it, small enough that the air handed
 * back is tens of microseconds rather than milliseconds. See the re-read in
 * gate_release() for what happens if even this loses the race.
 */
#define RELEASE_LEAD_US 30u

/*
 * Timeout for the anchor-establishing timeslot (bootstrap_anchor()).
 * Generous because nothing waits on it and the alternative (falling back to
 * EARLIEST for a real window) is what it exists to avoid. Far past any
 * measured grant latency (~1695 us, worst 2760 us under BLE).
 */
#define BOOTSTRAP_TIMEOUT_US 1000000u

BUILD_ASSERT(PLACE_MIN_US < PLACE_LEAD_US,
	     "the too-near floor must be strictly below the advertised lead: "
	     "the core plans against the advertised figure using its own `now` "
	     "and the test below runs against a sample taken later, so equal "
	     "values refuse every arm (measured: chunks=0, deny=517013/19s)");
BUILD_ASSERT(EXTEND_STEP_US >= MPSL_TIMESLOT_EXTENSION_TIME_MIN_US,
	     "an extension smaller than MPSL's own minimum is refused, and the "
	     "refusal would read as the arbiter yielding");
BUILD_ASSERT(END_MARGIN_EXTEND_US > MPSL_TIMESLOT_EXTENSION_MARGIN_MIN_US,
	     "an extension is asked for END_MARGIN_EXTEND_US before the end of "
	     "the grant, so that is the margin MPSL's own minimum has to fit "
	     "in. Only the extendable class is bound by it: a fixed-margin "
	     "grant never reaches ACTION_EXTEND (the TIMER0 case is gated on "
	     "g.extendable), which is why END_MARGIN_FIXED_US may be smaller "
	     "than MPSL's extension minimum");
BUILD_ASSERT(ELASTIC_INITIAL_US > END_MARGIN_EXTEND_US + EXTEND_STEP_US,
	     "the first grant must be long enough to contain the compare that "
	     "grows it, or the chain cannot start");
BUILD_ASSERT(END_MARGIN_FIXED_US > TAIL_MARGIN_US,
	     "the end margin must outlast the tail it contains: the compare "
	     "fires END_MARGIN before expiry and the RADIO's own ramp-down and "
	     "clock disagreement (TAIL_MARGIN_US) still have to fit inside what "
	     "is left, or the timeslot is overstayed");
BUILD_ASSERT(END_MARGIN_FIXED_US <= END_MARGIN_EXTEND_US,
	     "the fixed margin is the SHORT one by construction - it drops the "
	     "extension minimum the extendable class has to carry. A FIXED "
	     "larger than EXTEND means the split has been inverted, and "
	     "GRANT_OVERHEAD_US/MAX_WINDOW_US (computed from EXTEND) would then "
	     "under-reserve the class with the bigger overhead");
BUILD_ASSERT(MAX_WINDOW_US > 0u && MAX_WINDOW_US < MPSL_TIMESLOT_LENGTH_MAX_US,
	     "the head and tail margins have eaten the whole grant");

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------
 */

/* The returned parameter must outlive the callback (mpsl_timeslot.h). One
 * static per session. */
static mpsl_timeslot_signal_return_param_t rv;
static mpsl_timeslot_request_t             req;

static mpsl_timeslot_session_id_t session_id;
static bool                       session_open;
/* The trampoline subscription is taken once per boot and never given back; see
 * gate_init(). Separate from session_open, which comes and goes. */
static bool                       swi_registered;

static struct {
	/* A reservation is outstanding: placed and not yet resolved. */
	volatile bool pending;
	/* A grant is held: SIGNAL_START arrived and ACTION_END has not. */
	volatile bool granted;
	/*
	 * MPSL has handed us the radio and we have not given it back. Distinct
	 * from `granted` (the core's permission, cleared early by
	 * gate_release() - see comment there) because every teardown backstop
	 * (SESSION_IDLE, SESSION_CLOSED, gate_shutdown) used to key on
	 * `granted` too, so a lost TIMER0 compare after gate_release() meant
	 * nothing ever reached timer0_disarm() / on_grant_end(). The silent
	 * cost: radio_endpoints_attach(false), the 802.15.4 driver's
	 * write-once SUBSCRIBE_RXEN restore, never runs and that receiver
	 * never ramps again.
	 *
	 * So this tracks the hardware loan, set at SIGNAL_START, cleared only
	 * by timer0_disarm() (which is idempotent) - exactly one
	 * on_grant_end() per granted timeslot, on every exit.
	 */
	volatile bool hw_held;
	/* The core has finished with the grant and wants it given back. */
	volatile bool release_wanted;
	/* This operation is allowed to grow itself - the elastic classes. */
	volatile bool extendable;
	/*
	 * The hand-back margin THIS grant was reserved with, and the only
	 * figure any compare for it may be placed against. Split by class:
	 * END_MARGIN_EXTEND_US when g.extendable, END_MARGIN_FIXED_US
	 * otherwise. Carried in state rather than re-derived at each site
	 * because the reservation MPSL was given was already sized with it -
	 * a compare placed against the other constant would be arithmetic
	 * about a timeslot that was never requested (bug 8's shape, and bug
	 * 25's lesson about the file's own numbers disagreeing with the
	 * arbiter's).
	 */
	uint32_t end_margin_us;

	/*
	 * The anchor. distance_us is measured from the PREVIOUS timeslot's
	 * START, not an absolute time, so every later distance is computed
	 * against this - and a BLOCKED request means no timeslot started, so
	 * this must not move then. 0-17 us of error over 640 chained requests
	 * when kept right.
	 */
	radiant_time_t anchor;
	bool           anchor_valid;

	/*
	 * The last instant the radio may still be busy under the grant we hold
	 * - NOT the timeslot expiry, which is g.end_margin_us later. Set from
	 * the GRANT, not the request: an elastic request asks for
	 * ELASTIC_INITIAL_US but wants more, so a follow-on measured against
	 * the request would be admitted against air not yet granted.
	 */
	radiant_time_t grant_end;
	/* What we hold right now, grown by the extension chain. */
	uint32_t       granted_len_us;
	/* What the operation actually wants. The elastic class asks for
	 * ELASTIC_INITIAL_US and extends toward this; everything else has the
	 * two equal from the start. */
	uint32_t       want_len_us;
	/* The length of the extension currently outstanding. Usually
	 * EXTEND_STEP_US; the last one is the remainder. */
	uint32_t       ext_step_us;
	/*
	 * The session's anchor-establishing timeslot is outstanding. Nothing is
	 * staged behind it and the core must hear about neither its grant nor
	 * its refusal - see bootstrap_anchor().
	 */
	volatile bool  bootstrapping;
	/*
	 * MPSL has a request of ours and has not answered it. Distinct from
	 * `pending` (the core's question, "is a grant coming for what's
	 * staged" - the backstop timer answers `false` on the core's behalf so
	 * nothing strands). This is MPSL's own state and nothing on our side
	 * can clear it early: a placed request cannot be withdrawn, so the
	 * backstop firing must not re-open the placement path. Counted via
	 * den_owed since it should almost never be what refuses an acquire.
	 */
	volatile bool  mpsl_owes;
	volatile bool  deny_wanted;
#define DENY_SRC_BLOCKED     0u
#define DENY_SRC_AFTER_GRANT 1u
#define DENY_SRC_DEADLINE    2u
	/* Which of the three submitters set deny_wanted. Diagnostic only,
	 * last-writer-wins. See the sent_* counters. */
	volatile uint8_t deny_src;
	/*
	 * ACTION_END has already been returned for this grant (bug 14: the
	 * trace shows `1:2 2:2`, TIMER0 ending the timeslot then RADIO ending
	 * it again). A grant can end while the RADIO event the window was
	 * waiting for is still pending; MPSL delivers it as one more signal
	 * that must be HANDLED (consumed) but not ANSWERED (a second action
	 * asserts). Which arrives first is a race, so both orders must be safe.
	 */
	volatile bool  ended;
} g;

static struct {
	/* Every call in, before any reason it might not become a request.
	 * `placed` counts what reached MPSL; this counts what the core asked
	 * for, so the difference distinguishes "the arbiter refused us" from
	 * "nobody asked" - a distinction a placed==granted==blocked=0 reading
	 * cannot make on its own (a 50pp loss run once looked exactly like a
	 * healthy gate on those three numbers alone). */
	uint32_t acquires;
	uint32_t acq_in_grant;
	uint32_t acq_in_grant_denied;
	uint32_t placed;
	uint32_t granted;
	uint32_t blocked;
	uint32_t cancelled;
	uint32_t refused_eagain;
	uint32_t refused_too_near;
	uint32_t refused_too_long;
	/* Which `return GATE_DENIED` fired - gate_acquire() has six and they
	 * used to share one number (a coexistence run once showed deny=4333
	 * with near/long both zero and no way to tell which branch). Different
	 * causes need different fixes: `pending` means the core is re-arming
	 * faster than one placement completes; `no_anchor` means the anchor
	 * needs rebootstrapping. */
	uint32_t den_pending;
	uint32_t den_no_anchor;
	uint32_t den_distance;
	uint32_t den_degenerate;
	uint32_t den_no_session;
	/* Refused because MPSL still owed an answer. See g.mpsl_owes. */
	uint32_t den_owed;
	uint32_t extends_ok;
	uint32_t extends_failed;
	/* How often the backstop timer fired rather than SIGNAL_BLOCKED.
	 * Expected zero (all 17 blocks in the P0 spike arrived before the
	 * start they referred to); non-zero means that measurement did not
	 * generalise. */
	uint32_t deadline_fired;
	/*
	 * Which road a *delivered* denial came down, and how many were
	 * suppressed. The den_* block above only counts gate_acquire()'s
	 * synchronous refusals (RADIANT_RADIO_EDENIED); everything else the
	 * core scores as DONE_DENIED is the async terminal
	 * RADIANT_RADIO_STATUS_DENIED for an operation whose arm was accepted,
	 * and it has three distinct causes needing distinct fixes:
	 *
	 *   sent_after_grant  a grant ended before the window finished inside
	 *                     it. Fix is length/extension, not arbitration.
	 *   sent_deadline     the backstop timer beat the grant. Fix is
	 *                     placement lead. Expected zero, see deadline_fired.
	 *   sent_blocked      MPSL genuinely refused - the only one that means
	 *                     the air was actually contended.
	 *   deny_suppressed   the g.pending guard caught a re-arm between
	 *                     submit and run; not a denial, counted to confirm
	 *                     the guard is live.
	 */
	uint32_t sent_after_grant;
	uint32_t sent_deadline;
	uint32_t sent_blocked;
	uint32_t deny_suppressed;
	/*
	 * The two signals that mean this file is wrong, counted separately
	 * because MPSL asserts before anything else can observe which arrived
	 * and they call for opposite fixes. Printed by mpsl_assert_handle().
	 */
	uint32_t overstayed;
	uint32_t invalid_return;
	uint32_t unknown_signal;
	/*
	 * The counters the overnight soak is scored on.
	 *   grant_end_calls  every on_grant_end() made; invariant is one per
	 *                    granted timeslot. Falling behind means a
	 *                    SUBSCRIBE_RXEN never restored.
	 *   late_disarm      BLOCKED/CANCELLED arrived while the radio was
	 *                    still on loan - should be impossible since those
	 *                    signals mean no timeslot started. Non-zero names
	 *                    a real fault (today: a dead receiver).
	 *   idle_disarm      SESSION_IDLE/CLOSED found the radio still on loan
	 *                    and gave it back - the designed backstop for a
	 *                    timeslot MPSL ended by length expiry, so non-zero
	 *                    is normal here (unlike late_disarm).
	 *   release_disarm   gate_release() handed the radio back itself
	 *                    instead of waiting for its own compare - the
	 *                    normal path, kept so grant_end_calls has a known
	 *                    breakdown rather than just a total.
	 */
	uint32_t grant_end_calls;
	uint32_t late_disarm;
	uint32_t idle_disarm;
	uint32_t release_disarm;
	/* A request MPSL answered from inside mpsl_timeslot_request() itself,
	 * before the call returned - see bug 15's flag-ordering fix in
	 * request_work_fn(). */
	uint32_t answered_inline;
	/* The last signal seen at all, for the case where the assert is raised
	 * without either of the above - which would mean MPSL is unhappy about
	 * something we returned rather than about when we returned it. */
	uint32_t last_signal;
	/* Bench probe: how many times MPSL delivered each of the two signals
	 * that can END a live grant. `sig_radio` at zero while grants are being
	 * delivered says the RADIO never raised an event inside the timeslot -
	 * a fact no other counter here distinguishes from a healthy grant that
	 * simply heard nothing. */
	uint32_t sig_radio;
	uint32_t sig_timer0;
	/* Bench probe: the same life story, for HIGH-priority (tracked window
	 * and transmit) requests only. "Tracked windows never get the air" and
	 * "tracked windows get the air and hear nothing" need opposite fixes
	 * and no counter here separated them. */
	uint32_t hi_acq;
	uint32_t hi_pending;  /* ...that got as far as a placed reservation */
	uint32_t hi_reserve;  /* ...served from air already held */
	uint32_t hi_blocked;  /* ...refused by the arbiter */
	uint32_t hi_granted;  /* ...that actually got the air */
	/* Which call site ended each HIGH-priority grant. Indexed by the
	 * END_SITE_* enum below; sized as a literal because the enum is
	 * declared with the rest of the grant bookkeeping, further down. */
	uint32_t hi_end_site[9];
	/* SIGNAL_START found MPSL_TIMER0's COMPARE0 event already set before it
	 * enabled the interrupt - a grant that ends the instant it is armed. */
	uint32_t start_stale_compare;
} stats;

/*
 * A ring buffer rather than a printk: a printk in this path costs ~87 us/char
 * and moves the timing it measures, while a byte written to an array costs
 * nothing, and the assert handler has time to print sixteen of them.
 * High nibble is the signal, low nibble the action returned (0 NONE, 1 END,
 * 2 EXTEND).
 */
#define TRACE_LEN 16u
static uint8_t  trace[TRACE_LEN];
static uint32_t trace_at;

static inline void trace_put(uint32_t signal, uint32_t action)
{
	trace[trace_at % TRACE_LEN] =
		(uint8_t)(((signal & 0xFu) << 4) | (action & 0xFu));
	trace_at++;
}

/* (The bring-up printk shot counters that stood here were written nowhere and
 * read nowhere; they went with the bring-up.) */

/* ---------------------------------------------------------------------------
 * The backstop deadline timer - not the primary path, but needed because
 * denial can arrive too late to be useful alone. Measured: every BLOCKED
 * arrived 11-31 ms EARLY (MPSL decides a NORMAL request at placement, not at
 * the requested start). Kept anyway since being wrong about that wedges the
 * core's single operation slot forever, and 17 samples against one advertiser
 * isn't a guarantee.
 * ---------------------------------------------------------------------------
 */

/*
 * How long past the operation's own start the backstop waits. Was zero
 * (fire exactly when the grant would be useless), but that was measured with
 * no second stack. MPSL answers from mpsl_low_priority_process(), a
 * cooperative thread that a real SoftDevice Controller advertiser can starve
 * within the ~2 ms arm lead a scan chunk is placed on. At zero this produced
 * a spiral, not a slow gate: the backstop fires before MPSL answers, clears
 * g.pending and denies while MPSL still owns the session (a placed request
 * cannot be withdrawn); the core re-arms immediately; mpsl_timeslot_request()
 * returns -NRF_EAGAIN; a fresh ~2 ms backstop starts; repeat. Measured
 * (100 ms connectable advertiser, 260 s): acq=7229, half burned in that loop
 * (dead=3411 ~= eagain=3397), each turn delivering a DONE_DENIED.
 *
 * 20 ms only needs to beat "MPSL never answers", not race MPSL's own BLOCKED
 * timing - comfortably past the 11-31 ms block band and any advertiser
 * scheduling delay, still well below the wedge it guards against.
 */
#define DEADLINE_SLACK_US 20000u

static void deadline_expired(struct k_timer *t);
static K_TIMER_DEFINE(deadline, deadline_expired, NULL);

/*
 * Arming and disarming reach this file from the signal callback, i.e. from a
 * zero-latency interrupt, where k_timer_start()/k_timer_stop() are illegal
 * (bug 27). So the callback records WHAT it wants here and pends the
 * trampoline; deadline_apply() below does it from ordinary interrupt context.
 *
 * Last writer wins, deliberately. The two requests are start and stop, they are
 * only ever issued about the same single outstanding reservation, and the
 * backstop is a backstop: `dead=0` over every bench run so far, so losing a
 * race between an arm and a disarm costs at worst one spurious denial of an
 * operation that had already been resolved - which denied_work_fn()'s own
 * g.pending re-test then suppresses (bug 13).
 */
#define DEADLINE_REQ_NONE 0u
#define DEADLINE_REQ_ARM  1u
#define DEADLINE_REQ_STOP 2u
static atomic_t deadline_req;
static atomic_t deadline_req_us;

static inline void deadline_arm(uint32_t us)
{
	atomic_set(&deadline_req_us, (atomic_val_t)us);
	atomic_set(&deadline_req, (atomic_val_t)DEADLINE_REQ_ARM);
	radiant_swi_pend(RADIANT_SWI_GATE_DEADLINE);
}

static inline void deadline_stop(void)
{
	atomic_set(&deadline_req, (atomic_val_t)DEADLINE_REQ_STOP);
	radiant_swi_pend(RADIANT_SWI_GATE_DEADLINE);
}

static void deadline_apply(void)
{
	atomic_val_t req = atomic_and(&deadline_req, 0);

	if (req == (atomic_val_t)DEADLINE_REQ_ARM) {
		k_timer_start(&deadline,
			      K_USEC((uint32_t)atomic_get(&deadline_req_us)),
			      K_NO_WAIT);
	} else if (req == (atomic_val_t)DEADLINE_REQ_STOP) {
		k_timer_stop(&deadline);
	}
}

/* ---------------------------------------------------------------------------
 * The split: only programming the peripheral happens inside the grant's own
 * signal callback (the whole reason the grant exists, and it can't be done
 * earlier or later). Everything else moves to this work queue:
 *   - issuing an MPSL request: mpsl_timeslot_request() is not callable from
 *     inside a timeslot or the RADIO interrupt at MPSL_HIGH_IRQ_PRIORITY. The
 *     only in-timeslot option, SIGNAL_ACTION_REQUEST, closes the timeslot to
 *     do it - not what's wanted here.
 *   - calling into the core: deliver_terminal() runs a scheduler pass that
 *     may arm the next operation, unbounded work that must not run inside an
 *     MPSL signal callback (overstaying asserts).
 * Before this split, opening a channel silently stopped the console mid-line
 * with no fault and no further housekeeping - the housekeeping thread had
 * stopped because the above was running at priority 0.
 *
 * A DEDICATED WORK QUEUE (gate_wq below), not the system one, and this comment
 * used to claim that before it was true. What the code actually did was
 * k_work_submit(), i.e. k_sys_work_q - shared with every driver, every net
 * stack timer callback and every logging backend in the image.
 *
 * Two reasons it is its own queue now, in order of how much they cost:
 *
 *   STACK. radiant/Kconfig asks for CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=4096
 *   and explains why (a terminal delivery here ends in a whole scheduler
 *   pass). It did not get it on the arms that matter:
 *   nrf/subsys/net/openthread/Kconfig.defconfig pins
 *   CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=1120 - and an OpenThread image is
 *   EXACTLY the image that compiles this file. Kconfig takes the first
 *   definition whose condition holds in parse order and OpenThread's is parsed
 *   first, so the request was silently overridden precisely where the deep
 *   stack was needed, and the symptom is the silent reboot loop the Kconfig
 *   comment describes. apps/dongle_thread/thread.conf works around it with a
 *   hand-written assignment (measured there at 1016 bytes of 1120), which is a
 *   per-application thing to remember; a stack this file owns is not.
 *
 *   CONTENTION. This queue's items are on the critical path of a timeslot
 *   that has already been reserved: request_work_fn() must reach
 *   mpsl_timeslot_request() inside the placement lead, and denied_work_fn()
 *   resolves the core's single operation slot. Behind k_sys_work_q they queue
 *   behind unrelated work of unbounded length.
 *
 * The priority is CONFIG_RADIANT_GATE_WQ_PRIO, whose default (-1) is the same
 * priority CONFIG_SYSTEM_WORKQUEUE_PRIORITY defaults to in these images - so
 * this change is contention isolation ONLY. Raising it above the system queue
 * is a separate decision that needs its own measurement.
 *
 * CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=4096 stays in radiant/Kconfig for now:
 * the BLE-side builds still route work through the system queue, and dropping
 * the request is a follow-up once there is high-water data from `gatewq=` in
 * stacks_report().
 *
 * BUG 27 (below) is a separate matter and is why nothing here may be submitted
 * straight from the signal callback, whichever queue it lands on.
 *
 * ─── BUG 27: NO KERNEL CALL MAY BE MADE FROM THIS FILE'S SIGNAL CALLBACK ────
 *
 * MPSL's TIMER0/RADIO/RTC0 vectors are connected with IRQ_ZERO_LATENCY
 * (nrf/subsys/mpsl/init/mpsl_init.c:187) whenever CONFIG_ZERO_LATENCY_IRQS is
 * on. A zero-latency interrupt is, by definition, ABOVE the priority that
 * irq_lock() and every Zephyr spinlock mask - that is the entire point of it.
 * So the kernel's own data structures are NOT protected against it, and any
 * kernel API called from on_signal() can land in the middle of another
 * context's critical section.
 *
 * on_signal() used to call k_work_submit() (three sites) and k_timer_stop() /
 * k_timer_start() (five sites). MEASURED failure, nRF54L15 DK, Matter arm
 * (bench-logs/m_wqstack.log):
 *
 *     ASSERTION FAIL @ spinlock.h:132
 *     ***** HARD FAULT ***** Fault escalation
 *     >>> ZEPHYR FATAL ERROR 4: Kernel panic on CPU 0
 *     Fault during interrupt handling
 *     lr=0x00036213 -> z_work_submit_to_queue  zephyr/kernel/work.c:382
 *
 * spinlock.h:132 is `__ASSERT(z_spin_lock_valid(l), "Invalid spinlock %p")` -
 * the work queue's own lock, taken recursively because a ZLI fired while a
 * thread was already inside k_work_submit() holding it. Without
 * CONFIG_SPIN_VALIDATE to catch it, the same race instead corrupts the list it
 * was editing: that is the sys_dlist_remove/z_abort_timeout bus fault seen once
 * on the bridge arm, and the silent RESET_CPU_LOCKUP reboot loop on the Matter
 * arm - a wild pointer faulting during exception entry never gets far enough to
 * print anything.
 *
 * Nordic's own timeslot sample makes no kernel call from its callback either
 * (nrf/samples/mpsl/timeslot/src/main.c: the callback only writes a static
 * return parameter; every k_msgq_put() is in a thread-context wrapper).
 *
 * AN ATTEMPT THAT WAS MEASURED WORSE AND THROWN AWAY, because it is the reason
 * the fix has the shape it does. Deferring the gate's calls to a thread this
 * file owned, polled at 1 kHz:
 *
 *   arm     change                                       result
 *   ------  -------------------------------------------  ----------------------
 *   bridge  k_work_submit/k_timer straight from the ZLI   fault at 199-660 s
 *   bridge  gate defers via a flag + 1 kHz poll thread    BUS FAULT at 25 s
 *   Matter  k_work_submit/k_timer straight from the ZLI   13 boots / 210 s
 *   Matter  gate defers via a flag + 1 kHz poll thread    29 boots / 240 s
 *
 * Two lessons, both now built into the fix. First, removing the gate's OWN
 * kernel calls does not remove the decisive one: the ANT completion path runs
 * in this same callback and ends in a k_sem_give() on a semaphore whose waiter
 * has a timeout, so it edits the kernel timeout list -
 *
 *   SIGNAL_RADIO (ZLI) -> radiant_nrf_gate_on_radio_irq() -> deliver_terminal()
 *     -> radiant_event_post_rx() -> radiant_event_wakeup()   [radiant_event.c:404]
 *     -> k_sem_give(&api_event_sem)                 [apps/common/ant_radio_radiant.c]
 *
 * and that fires once per received packet, where the gate's calls fire once per
 * reservation. Second, a POLL THREAD is the wrong deferral: k_msleep() puts an
 * entry on that same timeout list a thousand times a second, so it multiplies
 * traffic on the very structure being corrupted.
 *
 * So the deferral is a software interrupt, not a thread, and it covers
 * radiant_event_wakeup() first. See radiant/include/radiant/radiant_swi.h for
 * the mechanism and docs/p4-zli-kernel-calls.md for the measurements. What is
 * left in this file is the three work submissions and the deadline timer, all
 * routed through radiant_swi_pend() so the kernel call happens one interrupt
 * priority down, where irq_lock() reaches it.
 * ---------------------------------------------------------------------------
 */

static void request_work_fn(struct k_work *w);
static void denied_work_fn(struct k_work *w);

static K_WORK_DEFINE(request_work, request_work_fn);
static K_WORK_DEFINE(denied_work, denied_work_fn);

/*
 * The queue itself. Started once per boot by gate_init() and never stopped -
 * see gate_wq_started there, guarded in the same style as swi_registered and
 * for a harder reason: k_work_queue_start() asserts on an already-started
 * queue, and gate_shutdown()/gate_init() is an ordinary suspend/resume cycle
 * here (and how every test in radiant/tests/gate starts).
 *
 * Cost is CONFIG_RADIANT_GATE_WQ_STACK_SIZE (4096 by default) of RAM in any
 * image that compiles this file, and that budget includes gate_dump_fn(),
 * which runs here too - a stall diagnostic must not depend on the queue it is
 * diagnosing.
 */
static struct k_work_q gate_wq;
static K_THREAD_STACK_DEFINE(gate_wq_stack, CONFIG_RADIANT_GATE_WQ_STACK_SIZE);
static bool gate_wq_started;

/*
 * The gate's end of the trampoline. Runs at CONFIG_RADIANT_SWI_IRQ_PRIO - an
 * ordinary interrupt priority - so k_work_submit_to_queue() and the k_timer
 * calls in deadline_apply() are legal here and were not where they came from.
 *
 * Still submitted to a work queue rather than run inline: what these end in is
 * deliver_terminal() and a whole scheduler pass, which is unbounded work and
 * has no business in any interrupt handler. The trampoline changes WHICH
 * interrupt asks for the deferral, not that it is deferred.
 */
static void gate_swi_handler(uint32_t bits)
{
	if ((bits & RADIANT_SWI_GATE_DEADLINE) != 0u) {
		deadline_apply();
	}
	if ((bits & RADIANT_SWI_GATE_REQUEST) != 0u) {
		k_work_submit_to_queue(&gate_wq, &request_work);
	}
	if ((bits & RADIANT_SWI_GATE_DENY) != 0u) {
		k_work_submit_to_queue(&gate_wq, &denied_work);
	}
}

/*
 * What the next grant will be read with, held apart from the current one.
 * These four used to be written straight into `g`, which is only safe if an
 * acquire cannot happen during a live grant - but it constantly does (a
 * tracked window's receive callback commonly arms the next slot mid-window),
 * and writing through `g` then would retune the running grant's own end/
 * extension arithmetic for a window not yet placed. So they're staged here
 * and committed by request_work_fn() once mpsl_timeslot_request() actually
 * accepts the request.
 */
static struct {
	uint32_t       want_len_us;
	uint32_t       granted_len_us;
	radiant_time_t grant_end;
	bool           extendable;
	/* The margin this request's length was built with. See g.end_margin_us
	 * - it has to travel with the request, not be re-derived later. */
	uint32_t       end_margin_us;
	/* Bench probe only: which class this request belongs to, so a BLOCKED
	 * or a START can be attributed to it. */
	bool           high_prio;
} next_grant;

/* The class of the request MPSL currently holds, for the probe counters. */
static bool grant_high_prio;
/* The length of the last HIGH-priority grant, which is the class a tracked
 * window rides in. Printed beside the seam's `open lead` because the two
 * together are the whole question "was there ever enough air after the grant
 * started for this window to open in". */
static uint32_t hi_grant_len_us;
/* And the margin that grant was reserved with, so the dump's `cc0=` is derived
 * from the same number the compare was, rather than from a constant that is now
 * only one of two. A HIGH-priority grant is never extendable, so this reads
 * END_MARGIN_FIXED_US on every real window - but recording it is what keeps the
 * instrument honest if that ever stops being true (bug 25's lesson). */
static uint32_t hi_end_margin_us = END_MARGIN_EXTEND_US;
/* How long the last HIGH-priority grant actually lasted, and the worst case. */
static uint32_t hi_end_us;
static uint32_t hi_end_us_max;
/* MPSL_TIMER0's count when SIGNAL_START armed the end compare - how much of the
 * timeslot the callback itself consumed before the grant had an end at all. */
static uint32_t t0_at_arm_us;
/*
 * Which call site last ended a grant. Set immediately before every
 * gate_hand_back()/timer0_disarm(), so the hand-back can attribute itself.
 * Names are in end_site_name[].
 */
static uint8_t end_site;
#define END_SITE(n) (end_site = (uint8_t)(n))
enum {
	END_SITE_NONE = 0,
	END_SITE_BOOTSTRAP,   /* SIGNAL_START, anchor-only timeslot */
	END_SITE_START_REL,   /* SIGNAL_START, operation finished synchronously */
	END_SITE_RADIO,       /* SIGNAL_RADIO finished the operation */
	END_SITE_TIMER0,      /* the grant ran out */
	END_SITE_EXT_FAILED,  /* an extension was refused */
	END_SITE_LATE,        /* BLOCKED/CANCELLED arriving with the radio out */
	END_SITE_SESSION,     /* SESSION_IDLE / SESSION_CLOSED / shutdown */
	END_SITE_RELEASE,     /* gate_release() with no grant held */
	END_SITE_MAX,
};
static const char *const end_site_name[END_SITE_MAX] = {
	"none", "boot", "startrel", "radio", "timer0", "extfail", "late",
	"sess", "rel",
};

/*
 * How big the elastic class asks its first bite, and why it's a variable.
 * ADR 0013 makes the sweep the consumer that gives way; a fixed 10 ms request
 * measured badly against a 100 ms BLE advertiser (placed=570 granted=2
 * blocked=568), while a tracked 5 ms window at the master's chosen instant
 * coexists fine. So the first bite halves on BLOCKED (reaching the floor in
 * three refusals) and grows back by one EXTEND_STEP_US on success
 * (deliberately slower), and is then grown normally by the extension chain -
 * costing dwell only while the air is genuinely contended.
 *
 * Not applied to tracked windows or transmits: their length is the core's
 * choice and shortening one hands back air mid-packet.
 */
#define ELASTIC_FLOOR_US 2500u
static uint32_t elastic_initial_us = ELASTIC_INITIAL_US;

/*
 * Where the elastic request asks, which matters more than how much. Backing
 * the request length off alone did not help (blocked=408/410 against a
 * 100 ms advertiser) because it's a phase lock: a ~94 ms scan chunk re-asks
 * on a cadence within ms of the advertiser's 100 ms interval, and the anchor
 * deliberately not moving on BLOCKED (correct, to stop denial runs walking
 * the schedule) makes every retry land on the same phase.
 *
 * So a refused elastic request also shifts phase before retrying. The walk
 * is a deterministic odd increment, not a PRNG, since it needs to be
 * uncorrelated with 100 ms rather than unpredictable. 2137 us is prime (visits
 * every offset before repeating); span is a bit over one ANT+ slot so the
 * sweep's own coverage isn't distorted.
 *
 * Only the elastic class moves - a tracked window's instant belongs to the
 * master, and skewing it would move the receiver off the packet.
 */
#define ELASTIC_SKEW_STEP_US 2137u
#define ELASTIC_SKEW_SPAN_US 13000u
static uint32_t elastic_skew_us;

/* Consecutive refusals before the anchor is abandoned; see the BLOCKED case. */
#define BLOCKED_RUN_REANCHOR 4u
static uint32_t blocked_run;

#if defined(CONFIG_RADIANT_SWEEP_DEBUG)
/*
 * WHAT MPSL WAS ACTUALLY ASKED FOR, as opposed to what this file believes it
 * asked for. Bug 25 was exactly a disagreement between those two, and no
 * counter here could see it: `len=%u/%u` in the dump prints g.granted_len_us
 * and g.want_len_us, both of which were right. These are read straight out of
 * `req` at the moment it is finished, so they cannot drift from it.
 */
static uint32_t last_req_len_us;
static uint32_t last_req_dist_us;
/*
 * HOW FAR AHEAD OF `now` THE REQUESTED START ACTUALLY IS - the quantity bug 26
 * turned out to be about, and the one number the dump never carried. `dist` is
 * measured from the anchor, which ages, so a distance of 42 ms says nothing
 * about whether MPSL was given any notice.
 */
static uint32_t last_req_lead_us;
static uint32_t min_req_lead_us = UINT32_MAX;
#endif

BUILD_ASSERT(ELASTIC_FLOOR_US >= MPSL_TIMESLOT_LENGTH_MIN_US,
	     "the elastic floor must still be a timeslot MPSL will grant");
BUILD_ASSERT(ELASTIC_FLOOR_US > END_MARGIN_EXTEND_US,
	     "an elastic grant must outlive its own hand-back margin");

static void commit_next_grant(void)
{
	g.want_len_us    = next_grant.want_len_us;
	g.granted_len_us = next_grant.granted_len_us;
	g.grant_end      = next_grant.grant_end;
	g.extendable     = next_grant.extendable;
	g.end_margin_us  = next_grant.end_margin_us;
	grant_high_prio  = next_grant.high_prio;
}

/*
 * Place the reservation. Thread context, so the MPSL call is legal and a
 * refusal can be handled by simply telling the core.
 */
static void request_work_fn(struct k_work *w)
{
	int32_t rc;

	ARG_UNUSED(w);

	if (!g.pending || !session_open) {
		return;
	}

	/*
	 * Bug 15: the flag must be armed BEFORE the call, or it's a permanent
	 * wedge. mpsl_timeslot_request() can synchronously deliver BLOCKED via
	 * on_signal() from inside this very call (mpsl_low_priority_process()
	 * context) when the arbiter can refuse without deferring. If the flag
	 * were set after the call returned 0, that answer's clear would land
	 * first and the later `g.mpsl_owes = true` would leave it permanently
	 * stale - unclearable, since a placed request can't be withdrawn and no
	 * further BLOCKED/START/SESSION_IDLE is coming. Every later
	 * gate_acquire() then dies with den_owed. Measured on P4 MED: owes=1,
	 * owed=2403 climbing. Only bites under a genuinely contending stack.
	 *
	 * Armed first, both orderings are safe: an inline answer finds the
	 * flag already true and clears it; a later answer clears it then. Only
	 * the not-placed case needs undoing below, and no answer can be in
	 * flight for that. stats.answered_inline counts the inline case.
	 */
	g.mpsl_owes = true;
	rc = mpsl_timeslot_request(session_id, &req);
	if (rc == 0) {
		if (!g.mpsl_owes) {
			/* Answered from inside the call. The flag has done its
			 * job; do NOT raise it again. */
			stats.answered_inline++;
		}
		/* Accepted, so the staged numbers are now the numbers this
		 * session's next grant will be read with. See next_grant. */
		commit_next_grant();
		stats.placed++;
		return;
	}
	/* Not placed, so nothing is owed. See the note above for why this
	 * write cannot lose a race. */
	g.mpsl_owes = false;

	if (rc == -NRF_EAGAIN) {
		/*
		 * Bug 10: -NRF_EAGAIN is a "not yet", not a refusal - it just
		 * means the session isn't IDLE, the ordinary case whenever a
		 * request is placed between gate_release() asking for the end
		 * and the ACTION_END that delivers it. Reporting it as a
		 * denial once cost chunks=15 deny=14 with blocked=0, which
		 * would have made the sweep back off for contention that
		 * never happened.
		 *
		 * The request is kept; SESSION_IDLE places it once the
		 * condition clears. The deadline timer stays running so a
		 * session that never goes idle still resolves the operation.
		 */
		stats.refused_eagain++;
		return;
	}
	/*
	 * The reservation could not even be asked for. That is a denial like
	 * any other from the core's point of view - it did not get the air -
	 * and it is reported the same way rather than through a second path.
	 */
	g.pending = false;
	deadline_stop();
	if (g.bootstrapping) {
		/* No operation is behind the bootstrap. Leaving anchor_valid
		 * false is the whole recovery: the next real request falls back
		 * to EARLIEST exactly as it did before there was a bootstrap. */
		g.bootstrapping = false;
		return;
	}
	radiant_nrf_gate_on_denied();
}

/*
 * Establish the anchor before any real work needs it (rule 2). Using the
 * first REAL request as EARLIEST doesn't work: EARLIEST means "as early as
 * you can", not "at the start I described", so the grant lands at an instant
 * MPSL chose while the operation staged behind it has an absolute t_open
 * computed from a placement lead - late landings hit program_rx()'s ETIME
 * (measured: one grant, one FAILED operation, search stopped), early
 * landings program a window mostly outside the grant.
 *
 * So the EARLIEST timeslot carries no operation - API minimum length, handed
 * straight back from SIGNAL_START - and exists only to set g.anchor, after
 * which every request is NORMAL and lands within 0-17 us (measured, 640
 * chained requests).
 *
 * Callable from any context; the MPSL call itself is deferred like every
 * other one.
 */
static void bootstrap_anchor(void)
{
	if (!session_open || g.pending || g.granted || g.anchor_valid) {
		return;
	}
	memset(&req, 0, sizeof(req));
	req.request_type = MPSL_TIMESLOT_REQ_TYPE_EARLIEST;
	req.params.earliest.hfclk = MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED;
	req.params.earliest.priority = MPSL_TIMESLOT_PRIORITY_NORMAL;
	req.params.earliest.length_us = MPSL_TIMESLOT_LENGTH_MIN_US;
	req.params.earliest.timeout_us = BOOTSTRAP_TIMEOUT_US;

	/* Staged like every other request, and it has to be: request_work_fn()
	 * commits next_grant on every acceptance, so a bootstrap that wrote
	 * through `g` would have its own numbers overwritten by whatever the
	 * last real window staged. It cannot grow and it is the shortest
	 * timeslot there is. */
	next_grant.extendable = false;
	next_grant.high_prio = false;
	/* The conservative one, not the class-derived one. The bootstrap slot is
	 * handed straight back from SIGNAL_START and so never places a compare
	 * against this margin at all; if a future path ever does, the wider
	 * figure is the safe default (both are longer than this 100 us slot, so
	 * every compare site falls to its own short-grant fallback either way). */
	next_grant.end_margin_us = END_MARGIN_EXTEND_US;
	next_grant.want_len_us = MPSL_TIMESLOT_LENGTH_MIN_US;
	next_grant.granted_len_us = MPSL_TIMESLOT_LENGTH_MIN_US;
	next_grant.grant_end = 0u;
	g.bootstrapping = true;
	g.pending = true;
	radiant_swi_pend(RADIANT_SWI_GATE_REQUEST);
}

/*
 * Tell the core it did not get the air. Always from this queue, never from a
 * timer ISR or an MPSL callback, because it ends in deliver_terminal() and from
 * there in a whole scheduler pass.
 */
static void denied_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	/* A grant whose programming failed is finished here too, for the same
	 * reason the denial is: both end in a scheduler pass, and neither may
	 * run inside an MPSL signal callback. It carries its own flag, so it is
	 * unconditional; the denial is not - see end_housekeeping(). */
	radiant_nrf_gate_finish_failed();
	if (!g.deny_wanted) {
		return;
	}
	g.deny_wanted = false;
	/*
	 * The test is made here, not where the work was submitted - the
	 * difference is a wedged dongle. A grant can end with the window still
	 * armed, queuing a denial, but then the RADIO event the window was
	 * waiting for can arrive microseconds later in the same interrupt,
	 * complete the operation and arm a new one before this thread runs.
	 * Denying that new (different) operation strands it, since nothing
	 * failed and nothing will re-arm. g.pending answers "is a grant coming
	 * for whatever is staged" and must be asked at the moment it's acted
	 * on.
	 */
	if (!g.pending) {
		switch (g.deny_src) {
		case DENY_SRC_AFTER_GRANT:
			stats.sent_after_grant++;
			break;
		case DENY_SRC_DEADLINE:
			stats.sent_deadline++;
			break;
		default:
			stats.sent_blocked++;
			break;
		}
		radiant_nrf_gate_on_denied();
	} else {
		stats.deny_suppressed++;
	}
}

#if defined(CONFIG_RADIANT_SWEEP_DEBUG)
/*
 * A once-a-second dump - the only instrument that shows a stall. Every other
 * diagnostic here is emitted BY an event, so a gate that stopped producing
 * events produces no diagnostics either. Fixed period, so it reports absence
 * of activity as clearly as presence; deferred logging keeps the cost off the
 * grant path.
 *
 * On gate_wq rather than the system queue, like everything else here: a stall
 * diagnostic that queues behind unrelated system work goes quiet for the same
 * reasons the gate does, and would then be evidence of nothing. Its stack
 * demand (a handful of LOG_INF argument frames and the 96-byte `sites` buffer)
 * is part of the CONFIG_RADIANT_GATE_WQ_STACK_SIZE budget.
 */
static void gate_dump_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(gate_dump, gate_dump_fn);

#if defined(CONFIG_INIT_STACKS)
/*
 * HOW MUCH OF EACH STACK THIS BACKEND ACTUALLY USES, measured rather than
 * argued about.
 *
 * Two stacks decide whether this gate can live in an image, and both fail the
 * same way from the outside - a board that reboots with nothing printed:
 *
 *   ISR    every MPSL timeslot signal (START/RADIO/TIMER0) is delivered from
 *          an IRQ_ZERO_LATENCY vector at priority 0, which can preempt any
 *          other ISR, so this backend's grant path lands on top of whatever
 *          interrupt was already running. Overflow here is the one that
 *          produces a SILENT reset: MSP is already past the guard, so the
 *          fault handler's own stacking faults, escalates, and the core
 *          LOCKS UP (reset reason 0x100) before anything can be printed.
 *
 *   sysWQ  everything this file defers - request_work_fn(), denied_work_fn()
 *          and, through them, deliver_terminal() and a whole scheduler pass.
 *          radiant/Kconfig asks for 4096 and says why; on a Thread build it
 *          does not get it (nrf/subsys/net/openthread/Kconfig.defconfig
 *          pins 1120), which is exactly the arm where this file is compiled.
 *          Overflow here hits a guarded thread stack, so it PRINTS an MPU
 *          fault - a different signature from the ISR case, and that
 *          difference is how the two are told apart.
 *
 * Free to compute: CONFIG_INIT_STACKS fills both with 0xAA at boot, so the
 * high-water mark is just the length of the run of fill still standing. Costs
 * one scan a second, off the grant path, on the dump work item.
 */
K_KERNEL_STACK_ARRAY_DECLARE(z_interrupt_stacks, CONFIG_MP_MAX_NUM_CPUS,
			     CONFIG_ISR_STACK_SIZE);

/*
 * The run of untouched 0xAA still standing at the bottom of a stack.
 * Open-coded rather than calling z_stack_space_get(), which lives in
 * <kernel_internal.h> - a private kernel header that is not on a module's
 * include path (thread_analyzer.c can include it; this file cannot).
 */
static size_t fill_run(const uint8_t *buf, size_t size)
{
	size_t i;

	for (i = 0u; i < size; i++) {
		if (buf[i] != 0xAAu) {
			break;
		}
	}
	return i;
}

static void stacks_report(void)
{
	size_t isr_size = K_KERNEL_STACK_SIZEOF(z_interrupt_stacks[0]);
	size_t isr_unused;
	size_t wq_size = 0;
	size_t wq_unused = 0;
	size_t gwq_size = 0;
	size_t gwq_unused = 0;

	isr_unused = fill_run(K_KERNEL_STACK_BUFFER(z_interrupt_stacks[0]),
			      isr_size);
	/*
	 * The system work queue's own thread. k_work_queue_thread_get() is the
	 * supported way to name it; k_thread_stack_space_get() then reads the
	 * same 0xAA fill.
	 *
	 * Kept even though this file no longer submits anything there: the rest
	 * of the image still does, radiant/Kconfig still asks for 4096, and
	 * whether that request can be dropped is exactly what this number
	 * answers.
	 */
	{
		k_tid_t wq = k_work_queue_thread_get(&k_sys_work_q);

		if (wq != NULL &&
		    k_thread_stack_space_get(wq, &wq_unused) == 0) {
			wq_size = CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE;
		}
	}
	/*
	 * And the gate's own queue - the one that now carries request_work_fn(),
	 * denied_work_fn() and this dump, i.e. every deep call this file makes.
	 * `gatewq` approaching its size is the MPU fault that used to be blamed
	 * on `syswq`; well under it is the evidence that would justify sizing
	 * CONFIG_RADIANT_GATE_WQ_STACK_SIZE down. Reads 0/0 before gate_init()
	 * has started the queue.
	 */
	if (gate_wq_started) {
		k_tid_t gwq = k_work_queue_thread_get(&gate_wq);

		if (gwq != NULL &&
		    k_thread_stack_space_get(gwq, &gwq_unused) == 0) {
			gwq_size = CONFIG_RADIANT_GATE_WQ_STACK_SIZE;
		}
	}
	/*
	 * Printed as USED/SIZE, not unused: the number that matters is how
	 * close the peak came to the ceiling, and a reader should not have to
	 * subtract. `isr` at or near its size is the silent-lockup fault.
	 */
	LOG_INF("stacks: isr=%u/%u syswq=%u/%u gatewq=%u/%u",
		(unsigned int)(isr_size - isr_unused), (unsigned int)isr_size,
		(unsigned int)(wq_size > wq_unused ? wq_size - wq_unused : 0u),
		(unsigned int)wq_size,
		(unsigned int)(gwq_size > gwq_unused ? gwq_size - gwq_unused
						     : 0u),
		(unsigned int)gwq_size);
}
#else
static inline void stacks_report(void) { }
#endif /* CONFIG_INIT_STACKS */

static void gate_dump_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	/*
	 * `end=` against `granted=` is the scoring invariant: exactly one
	 * on_grant_end() per granted timeslot. The breakdown after it does NOT
	 * sum to `end=` on its own - only the three exceptional routes are
	 * counted (release handing back early, session backstop, the
	 * must-be-zero late case), so the ordinary route (a TIMER0/RADIO
	 * signal ending its own grant) is printed as `norm=`, the clamped
	 * remainder. `late=` is the only one of the four naming a fault rather
	 * than a route.
	 *
	 * The three assert counters are printed here too rather than only from
	 * mpsl_assert_handle(), so a running board can show them before it's
	 * too late to read them.
	 */
	uint32_t routed = stats.release_disarm + stats.idle_disarm +
			  stats.late_disarm;
	uint32_t by_signal = (stats.grant_end_calls > routed)
				     ? (stats.grant_end_calls - routed)
				     : 0u;
	struct radiant_nrf_win_diag wd;

	radiant_nrf_win_diag_get(&wd);
	stacks_report();
	/* Everything a tracked window's request can do, in one line. den is the
	 * remainder: acquires that never reached a reservation at all. */
	LOG_INF("hi: acq=%u den=%u resv=%u pend=%u blk=%u grant=%u",
		stats.hi_acq,
		(stats.hi_acq > (stats.hi_pending + stats.hi_reserve))
			? (stats.hi_acq - stats.hi_pending - stats.hi_reserve)
			: 0u,
		stats.hi_reserve, stats.hi_pending, stats.hi_blocked,
		stats.hi_granted);
	/*
	 * The seam. Read left to right this is the life of a granted window:
	 * MPSL started it (grant), the backend found something staged and
	 * programmed it (prog), the RADIO raised an event inside the timeslot
	 * and MPSL routed it to us (sigrad/isr), and the core got its terminal
	 * (term). The first zero going left to right is where the window dies.
	 */
	LOG_INF("seam: grant=%u nostage=%u prog=%u/%u cbdrop=%u/%u | sigrad=%u sigt0=%u "
		"isr=%u end=%u dis=%u term=%u | ramp=%u/%u state=%u ev=0x%02x "
		"rxen saved=0x%08x cont=0x%03x a=0x%08x p=0x%08x e=0x%08x inten=0x%08x",
		wd.grants, wd.nostage, wd.prog_ok, wd.prog_fail,
		wd.cb_dropped, wd.cb_depth_max,
		stats.sig_radio, stats.sig_timer0,
		wd.isr, wd.ev_end, wd.ev_disabled, wd.term,
		wd.ramped, wd.never_ramped, wd.last_state,
		(unsigned int)wd.last_events,
		(unsigned int)wd.saved_rxen, (unsigned int)wd.contended,
		(unsigned int)wd.rxen_attach, (unsigned int)wd.rxen_prog,
		(unsigned int)wd.last_sub_rxen, (unsigned int)wd.last_inten);

	/*
	 * BUG 23, in one line: of the SHORT receive windows - the tracked slots
	 * the loss figure is computed from - how many lost our routing to the
	 * other stack's in-grant RADIO reset, how many still brought the
	 * receiver up, and how many actually caught their frame. `wipe` against
	 * `rx` is the correlation that says whether the reset accounts for the
	 * loss or only part of it. `held` is the mitigation's own account: how
	 * many of those windows the grant callback stayed on the CPU for, and
	 * how many of THOSE had the frame in RAM before it let go.
	 */
	LOG_INF("sw: n=%u wipe=%u ramp=%u wipe+ramp=%u rx=%u wipe+rx=%u | "
		"started=%u early=%u/%uus last=%dus | open lead=%dus max=%uus late=%u"
		" glen=%u cc0=%u",
		wd.sw, wd.sw_wipe, wd.sw_ramp, wd.sw_wipe_ramp, wd.sw_rx,
		wd.sw_wipe_rx, wd.sw_started, wd.sw_early, wd.sw_early_us_max,
		wd.sw_last_delta_us, wd.sw_open_lead_us, wd.sw_open_lead_max,
		wd.sw_open_late, hi_grant_len_us,
		(hi_grant_len_us > hi_end_margin_us)
			? (hi_grant_len_us - hi_end_margin_us)
			: 0u);

	/*
	 * How every tracked window's grant actually ended, by call site, and how
	 * long those grants lasted. The window opens HEAD_MARGIN_US into the
	 * grant and the grant is sized to outlive it by milliseconds, so
	 * anything here that is not `timer0` or `radio`, or an `end` of a few
	 * hundred microseconds, is a grant taken away before the air it was
	 * asked for arrived.
	 */
	{
		char sites[96];
		size_t at = 0;

		for (size_t i = 0; i < ARRAY_SIZE(stats.hi_end_site); i++) {
			if (stats.hi_end_site[i] == 0u || at >= sizeof(sites) - 1) {
				continue;
			}
			at += (size_t)snprintk(&sites[at], sizeof(sites) - at,
					       "%s=%u ", end_site_name[i],
					       stats.hi_end_site[i]);
		}
		sites[(at < sizeof(sites)) ? at : sizeof(sites) - 1] = '\0';
		LOG_INF("hiend: %s| lasted=%uus max=%uus | dur=%uus max=%uus "
			"short=%u | t0arm=%uus stale=%u",
			sites, hi_end_us, hi_end_us_max, wd.sw_dur_us,
			wd.sw_dur_us_max, wd.sw_dur_short, t0_at_arm_us,
			stats.start_stale_compare);
	}

	LOG_INF("gate: acq=%u in_grant=%u/%u placed=%u granted=%u blocked=%u cancel=%u eagain=%u "
		"near=%u long=%u ext=%u/%u dead=%u | "
		"den pend=%u anch=%u dist=%u degen=%u nosess=%u owed=%u | "
		"sent grant=%u dead=%u blk=%u supp=%u | "
		"end=%u (norm=%u rel=%u idle=%u late=%u) | "
		"bad over=%u inval=%u unk=%u inl=%u | "
		"req=%u/+%u lead=%u/%u len=%u/%u em=%u el=%u skew=%u brun=%u sig=%u "
		"g=%d hw=%d p=%d rel=%d end=%d boot=%d anchor=%d owes=%d",
		stats.acquires, stats.acq_in_grant, stats.acq_in_grant_denied,
		stats.placed, stats.granted, stats.blocked, stats.cancelled,
		stats.refused_eagain, stats.refused_too_near,
		stats.refused_too_long, stats.extends_ok, stats.extends_failed,
		stats.deadline_fired,
		stats.den_pending, stats.den_no_anchor, stats.den_distance,
		stats.den_degenerate, stats.den_no_session, stats.den_owed,
		stats.sent_after_grant, stats.sent_deadline,
		stats.sent_blocked, stats.deny_suppressed,
		stats.grant_end_calls, by_signal, stats.release_disarm,
		stats.idle_disarm, stats.late_disarm,
		stats.overstayed, stats.invalid_return, stats.unknown_signal,
		stats.answered_inline,
		last_req_len_us, last_req_dist_us,
		last_req_lead_us,
		(min_req_lead_us == UINT32_MAX) ? 0u : min_req_lead_us,
		g.granted_len_us, g.want_len_us, g.end_margin_us,
		elastic_initial_us, elastic_skew_us, blocked_run,
		stats.last_signal, (int)g.granted, (int)g.hw_held, (int)g.pending,
		(int)g.release_wanted, (int)g.ended, (int)g.bootstrapping,
		(int)g.anchor_valid, (int)g.mpsl_owes);
	if (session_open) {
		k_work_schedule_for_queue(&gate_wq, &gate_dump, K_SECONDS(1));
	}
}
#endif /* CONFIG_RADIANT_SWEEP_DEBUG */

static void deadline_expired(struct k_timer *t)
{
	ARG_UNUSED(t);

	if (!g.pending) {
		return;
	}
	stats.deadline_fired++;
	g.pending = false;
	/* A k_timer expiry is ISR context. Hand it on. This IS a refusal - the
	 * grant did not arrive in time to be of use - so the staged operation
	 * is orphaned and must be told. */
	g.deny_src = DENY_SRC_DEADLINE;
	g.deny_wanted = true;
	radiant_swi_pend(RADIANT_SWI_GATE_DENY);
}

/* ---------------------------------------------------------------------------
 * The signal callback
 *
 * Runs at MPSL's high priority for signals 0-4 and in mpsl_low_priority_process()
 * for 5-9. Returning anything but ACTION_NONE from one of the latter asserts,
 * so rv is set to NONE first and overwritten only where it is legal.
 * ---------------------------------------------------------------------------
 */

/*
 * Take the compare back on every path that ends a timeslot. MPSL_TIMER0
 * belongs to MPSL; leaving COMPARE0 enabled past the grant leaves an
 * interrupt armed on a peripheral we no longer own, whose event nothing
 * clears - it re-enters forever. Observed failure shape: board alive,
 * answers nothing, no fault message, no thread runs again (pinned in an
 * interrupt at priority 0; deferred logging needs a thread, so the console
 * just stops mid-line). Call on every exit that ends the grant.
 */
/*
 * Handing the radio back is split from ending the timeslot because
 * gate_release() needs the radio back at once but must leave the TIMER0
 * compare armed - it's the only way MPSL can be asked to return ACTION_END.
 * Disarming it there trades the hazard being fixed for an overstay assert.
 *
 * This half carries the invariant: exactly one on_grant_end() per granted
 * timeslot, on every exit. A second call hands back a radio the controller
 * already picked up; a missed one leaves the 802.15.4 driver's write-once
 * SUBSCRIBE_RXEN swapped out with nothing printed anywhere. g.hw_held is what
 * makes it exactly one.
 */
static inline void gate_hand_back(void)
{
	if (!g.hw_held) {
		return;
	}
	g.hw_held = false;
	stats.grant_end_calls++;
#if defined(CONFIG_RADIANT_SWEEP_DEBUG)
	/*
	 * WHICH PATH ENDED A TRACKED WINDOW'S GRANT, AND HOW LONG IT LASTED.
	 * `end=(norm rel idle late)` above is a clamped remainder: three routes
	 * are counted and everything else falls into `norm`, so a grant ended by
	 * a path nobody thought to count is reported as the ordinary one. That
	 * hid this: the seam probe said tracked windows were programmed
	 * correctly and never ramped, and the reason turned out to be that their
	 * grant was already over - which `norm` cannot say and this can.
	 */
	if (grant_high_prio) {
		if (end_site < ARRAY_SIZE(stats.hi_end_site)) {
			stats.hi_end_site[end_site]++;
		}
		hi_end_us = (uint32_t)(radiant_radio_now() - g.anchor);
		if (hi_end_us > hi_end_us_max) {
			hi_end_us_max = hi_end_us;
		}
	}
#endif
	radiant_nrf_gate_on_grant_end();
}

static inline void timer0_disarm(void)
{
	nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
	/*
	 * And give the radio back on the same call, so a future path that ends
	 * a grant can't forget to. Puts back RADIO->INTENSET, RADIO->SHORTS
	 * and the state machine (see radiant_nrf_gate_on_grant_end()); left
	 * set, those shorts fire on the SoftDevice Controller's own events.
	 * Timer registers are restored unconditionally; the hand-back is the
	 * idempotent half (gate_hand_back()).
	 */
	/* NO END_SITE() HERE. timer0_disarm() is reached from eight different
	 * places and this is the one line all of them run through; tagging it
	 * would overwrite the caller's tag and attribute every grant end to this
	 * function. (It did, for one bench run: sess=154 for grants that ended
	 * nowhere near a session signal.) */
	gate_hand_back();
}

/*
 * Bug 13: this used to be an unconditional k_work_submit(&denied_work) on
 * the reasoning that the handlers behind it are no-ops when there's nothing
 * to do - which stopped being true once the RADIO signal was wired up. A
 * window completing inside its grant can arm the next window (staging an
 * operation, placing a request) before this callback returns, and an
 * unconditional denial would then hit that freshly-armed operation instead
 * of an orphan. g.pending is the test for "is a staged operation actually
 * orphaned" - made in denied_work_fn, not here, since the answer can change
 * between submit and run (see comment there).
 *
 * The failed-programming finish stays unconditional; it carries its own flag.
 */
static inline void end_housekeeping(void)
{
	g.deny_src = DENY_SRC_AFTER_GRANT;
	g.deny_wanted = true;
	radiant_swi_pend(RADIANT_SWI_GATE_DENY);
}

/* A denial with nothing ambiguous about it: no grant is coming for the staged
 * operation. Used by the paths that ARE refusals. */
static inline void submit_denial(void)
{
	g.deny_src = DENY_SRC_BLOCKED;
	g.deny_wanted = true;
	radiant_swi_pend(RADIANT_SWI_GATE_DENY);
}

/* See gate_in_signal() in radiant_radio_nrf_gate.h. Set for the whole of the
 * callback so the backend can tell a zero-latency completion from an ordinary
 * one; a plain bool because MPSL delivers every signal this file answers from
 * one interrupt priority, so the callback cannot preempt itself. */
static volatile bool in_signal;

bool gate_in_signal(void)
{
	return in_signal;
}

static mpsl_timeslot_signal_return_param_t *on_signal(
	mpsl_timeslot_session_id_t id, uint32_t signal)
{
	ARG_UNUSED(id);

	in_signal = true;

	/* The trace records sequence for a debugging halt: START then TIMER0
	 * repeating means the compare is re-firing; START then silence means
	 * the RADIO interrupt is spinning and this callback isn't reached at
	 * all - two different fixes, otherwise indistinguishable. */
	rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
	stats.last_signal = signal;

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		stats.granted++;
		if (grant_high_prio) {
			stats.hi_granted++;
			hi_grant_len_us = g.granted_len_us;
			hi_end_margin_us = g.end_margin_us;
		}
		/* Answered. The session is ours again as far as placing the
		 * next request goes. See g.mpsl_owes. */
		g.mpsl_owes = false;
		blocked_run = 0u;
		/* The air was there for it, so take a little more next time. One
		 * step per grant, against halving per refusal - deliberately
		 * asymmetric, so a neighbour that has just started advertising is
		 * conceded to quickly and reclaimed from slowly. */
		if (g.extendable && elastic_initial_us < ELASTIC_INITIAL_US) {
			elastic_initial_us += EXTEND_STEP_US;
			if (elastic_initial_us > ELASTIC_INITIAL_US) {
				elastic_initial_us = ELASTIC_INITIAL_US;
			}
		}
		g.anchor = radiant_radio_now();
		g.anchor_valid = true;
		g.pending = false;
		g.granted = true;
		/* The radio is on loan from this instant until timer0_disarm()
		 * gives it back, whatever else happens to g.granted in between. */
		g.hw_held = true;
		g.release_wanted = false;
		/* A fresh timeslot, so ACTION_END is legal again. */
		g.ended = false;
		deadline_stop();

		if (g.bootstrapping) {
			/* The anchor set above is the whole point of this
			 * timeslot; nothing is staged behind it, so hand the
			 * air straight back without touching the peripheral
			 * or the core. */
			g.bootstrapping = false;
			g.granted = false;
			END_SITE(END_SITE_BOOTSTRAP);
			timer0_disarm();
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		}
		/*
		 * What we actually hold, which for an elastic request is a
		 * fraction of what was asked for. g.end_margin_us of it is
		 * spoken for by the handing back, so it is not air the radio
		 * may use. The margin is this grant's own (EXTEND for the
		 * elastic class, FIXED for tracked/TX) - the same figure
		 * gate_acquire() sized the request with.
		 */
		g.grant_end = g.anchor + (radiant_time_t)g.granted_len_us -
			      (radiant_time_t)g.end_margin_us;

		/*
		 * Programme the peripheral now, inside the grant - the call
		 * that does everything radiant_radio_nrf.c staged at the arm
		 * (RADIO configuration, compare, (D)PPI enables). May complete
		 * the operation synchronously (setting release_wanted before
		 * this returns), handled below rather than in a second signal.
		 */
		radiant_nrf_gate_on_grant();

		if (g.release_wanted) {
			/* Either the operation completed synchronously, or its
			 * programming failed and recorded the fact. Give the
			 * timeslot back now and let the work queue tell the core
			 * about it. */
			g.granted = false;
			END_SITE(END_SITE_START_REL);
			timer0_disarm();
			end_housekeeping();
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		}
		/*
		 * A TIMER0 compare so there's always a signal to end the
		 * timeslot from - without it an operation whose RADIO
		 * interrupt never comes would hold the grant to its full
		 * length and overstay (asserts).
		 *
		 * g.end_margin_us, not TAIL_MARGIN_US (250 us) - the latter
		 * only covers what the RADIO needs and is not enough time for
		 * the MPSL interrupt/dispatch/callback between the compare
		 * firing and ACTION_END returning. Measured: at len-250 the
		 * board asserted on the first or second timeslot; with a margin
		 * well inside the grant it ran 1007 timeslots clean.
		 *
		 * Which margin depends on the class and is decided once, at
		 * gate_acquire(), because the REQUEST was already sized with
		 * it: END_MARGIN_EXTEND_US (2 ms, room for MPSL's extension
		 * minimum as well as the signal path) for the elastic class,
		 * END_MARGIN_FIXED_US (750 us, signal path only) for a tracked
		 * window or a transmit, which can never extend.
		 */
		nrf_timer_cc_set(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL0,
				 (g.granted_len_us > g.end_margin_us)
					 ? (g.granted_len_us - g.end_margin_us)
					 : (g.granted_len_us / 2u));
#if defined(CONFIG_RADIANT_SWEEP_DEBUG)
		nrf_timer_task_trigger(MPSL_TIMER0, NRF_TIMER_TASK_CAPTURE1);
		t0_at_arm_us = nrf_timer_cc_get(MPSL_TIMER0, 1);
		if (nrf_timer_event_check(MPSL_TIMER0,
					  NRF_TIMER_EVENT_COMPARE0)) {
			stats.start_stale_compare++;
		}
#endif
		/*
		 * BUG 23: THE GRANT ENDED 14 MICROSECONDS AFTER IT STARTED,
		 * BECAUSE THE COMPARE THAT ENDS IT HAD ALREADY FIRED.
		 *
		 * MPSL restarts MPSL_TIMER0 from zero at every timeslot, but
		 * CC0 still holds whatever the LAST grant left in it until the
		 * cc_set above rewrites it - and that rewrite happens ~31 us
		 * into the timeslot, after this callback has programmed the
		 * whole receive window. Anything the counter passes in those
		 * 31 us latches EVENTS_COMPARE0, and a latched compare event
		 * means nrf_timer_int_enable() below raises SIGNAL_TIMER0
		 * immediately. The gate then does exactly what it is supposed
		 * to do with a TIMER0 signal: it ends the timeslot.
		 *
		 * It is self-sustaining, which is why it presents as a steady
		 * state rather than as jitter. gate_release() sets CC0 to the
		 * ENDING grant's own count plus a small lead, so a grant that
		 * ended at 14 us leaves CC0 at about 20 - and the next grant
		 * walks past it before it can be rewritten, ends at 14 us too,
		 * and leaves the same value behind. One early end is enough to
		 * keep every later one early.
		 *
		 * Measured on P4 MED, 60 s beside an OpenThread leader: stale
		 * compare found set at 291 of 317 grants; 214 of 216 tracked
		 * windows given a grant that lasted under a millisecond (median
		 * 14 us) against a window due to open 231 us in; 2 of 216
		 * windows ever ramping the receiver; 14 packets of 239.
		 *
		 * The clear belongs BETWEEN the cc_set and the int_enable, and
		 * only there: before the cc_set it would be clearing an event
		 * whose compare is still the old value, and after the
		 * int_enable it is already too late. gate_release() has had
		 * this exact three-step ordering all along - cc_set, event
		 * clear, int_enable - and this path is the one place that
		 * arms the same compare and skipped the middle step.
		 */
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
		nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
		break;

	case MPSL_TIMESLOT_SIGNAL_RADIO:
		/*
		 * Bug 12, the one that made the gate deaf. Inside a timeslot
		 * the RADIO interrupt is MPSL's: it owns RADIO_0_IRQn for the
		 * grant and delivers it as this signal instead, so
		 * radiant_radio_nrf.c's handler is never entered. Windows ran
		 * full length and never ended - measured as chunks=10 with no
		 * terminal of their own, every window instead denied later by
		 * this file, which reads like a denial problem but isn't one.
		 *
		 * The handler is re-entered by hand here; if it finished the
		 * operation, the grant is given back from here instead of
		 * waiting for TIMER0. High-priority signal, so ACTION_END is
		 * legal.
		 */
		stats.sig_radio++;
		radiant_nrf_gate_on_radio_irq();
		if (g.release_wanted && !g.granted) {
			END_SITE(END_SITE_RADIO);
			timer0_disarm();
			end_housekeeping();
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_TIMER0:
		stats.sig_timer0++;
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
		/*
		 * Either the core has finished and gate_release() provoked this
		 * compare, or the grant has run out and the operation is still
		 * going. Both end the timeslot; the second is the elastic case
		 * and tries to grow first.
		 */
		/* Grow toward what the operation actually wants, not the API
		 * ceiling - extending past t_close would hold unused air.
		 *
		 * The last step is the remainder, not a whole step: stopping
		 * at the last whole multiple would end the grant up to
		 * EXTEND_STEP_US before the window's own close, taking the
		 * receiver away mid-window (indistinguishable at the core
		 * from a sensor going quiet). Only asked for if MPSL will
		 * grant an extension that small; gate_acquire() keeps any
		 * shortfall under the API minimum otherwise. */
		if (!g.release_wanted && g.extendable &&
		    g.granted_len_us < g.want_len_us) {
			uint32_t step = g.want_len_us - g.granted_len_us;

			if (step > EXTEND_STEP_US) {
				step = EXTEND_STEP_US;
			}
			/*
			 * Bug 11: the ceiling here is the TIMESLOT's, not the
			 * window's - this used to read MAX_WINDOW_US (the
			 * scheduler's ceiling, with margins already
			 * subtracted), but a timeslot covering a window that
			 * long is necessarily longer than it, since head/tail/
			 * end margins are inside the grant but not the window.
			 * That stopped the chain 2500 us short, ending the
			 * grant before the window's own close and leaving the
			 * operation still armed (measured: chunks=10 deny=9,
			 * none of the ten windows ending by itself).
			 * want_len_us is already bounded against MPSL's
			 * maximum in gate_acquire(), so this is a backstop.
			 */
			if (step >= MPSL_TIMESLOT_EXTENSION_TIME_MIN_US &&
			    g.granted_len_us + step <=
				    MPSL_TIMESLOT_LENGTH_MAX_US) {
				g.ext_step_us = step;
				rv.callback_action =
					MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND;
				rv.params.extend.length_us = step;
				break;
			}
		}
		g.granted = false;
		END_SITE(END_SITE_TIMER0);
		timer0_disarm();
		/* Where a grant whose programming failed gets its terminal,
		 * without that work happening in here - and where an operation
		 * that is still armed when its air runs out gets told, but only
		 * if nothing is coming for it. See end_housekeeping(). */
		end_housekeeping();
		rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		break;

	case MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED:
		/*
		 * Bug 8, the one that survived every other fix: this line
		 * used TAIL_MARGIN_US instead of the end margin, so an
		 * extended grant's compare sat only 250 us before the new
		 * end (vs the full margin on the initial grant) - the two
		 * quantities the file elsewhere warns must not be conflated,
		 * conflated here. Symptom was ext=1204/0 asserting at
		 * placed=14: the extension chain was fine, the END that
		 * followed it wasn't. Compare now advances by exactly one
		 * step per extension, keeping g.end_margin_us of runway ahead
		 * of it always.
		 *
		 * g.end_margin_us is necessarily END_MARGIN_EXTEND_US on this
		 * path - only an extendable grant can be extended - and it is
		 * read from state rather than named directly so this line
		 * cannot drift from the one that armed the first compare.
		 */
		stats.extends_ok++;
		g.granted_len_us += g.ext_step_us;
		g.grant_end += (radiant_time_t)g.ext_step_us;
		nrf_timer_cc_set(MPSL_TIMER0, 0,
				 g.granted_len_us - g.end_margin_us);
		break;

	case MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED:
		/*
		 * The yield point, discovered rather than configured. Arrives
		 * with the grant still live, so the window closes cleanly and
		 * is reported as a partial via the ordinary abort path
		 * (RADIANT_RADIO_STATUS_ABORTED), not a denial.
		 *
		 * Unexercised as of P0 - 4450 extensions against a 1 s BLE
		 * advertiser produced zero refusals; P4 against OpenThread is
		 * what tests this branch.
		 */
		stats.extends_failed++;
		g.granted = false;
		END_SITE(END_SITE_EXT_FAILED);
		timer0_disarm();
		(void)radiant_radio_abort();
		/*
		 * The orphan still gets its denial: radiant_radio_abort()
		 * above only delivers a terminal for an ARMED operation, not
		 * one that was staged but not yet programmed, which would
		 * otherwise wedge the core's single operation slot. The
		 * g.pending test in denied_work_fn() keeps this from denying
		 * an operation a fresh request is actually coming for.
		 *
		 * Deliberately not changed: radiant_radio_abort() still runs
		 * from this priority-0 callback (up to ~100 us plus a
		 * scheduler pass), more work than this file asks elsewhere.
		 * Deferring it would hand the timeslot back with the
		 * peripheral still armed, and this branch is only reachable
		 * under real contention - move it only with a measurement of
		 * this path in hand.
		 */
		end_housekeeping();
		rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		break;

	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		/*
		 * Low priority context - a cooperative thread, not the radio
		 * interrupt. No action may be returned; the response is just
		 * to tell the core.
		 *
		 * The anchor does not move here: no timeslot started, so the
		 * next distance is computed against the older anchor (hence
		 * anchor_valid stays set). Also the first HAL event that can
		 * reach the core outside the radio interrupt - see
		 * radiant_radio_hal.h's callback-context paragraph.
		 */
		if (signal == MPSL_TIMESLOT_SIGNAL_BLOCKED) {
			stats.blocked++;
			if (grant_high_prio) {
				stats.hi_blocked++;
			}
		} else {
			stats.cancelled++;
		}
		/*
		 * The "no timeslot started" assumption above is checked, not
		 * just assumed: hand the radio back if it's somehow still on
		 * loan, and count it. late_disarm non-zero would name a real
		 * fault (today: a dead receiver hours later) that would
		 * otherwise be untraceable.
		 */
		if (g.hw_held) {
			stats.late_disarm++;
			g.granted = false;
			END_SITE(END_SITE_LATE);
			timer0_disarm();
		}
		/* Answered, and refused. See g.mpsl_owes. */
		g.mpsl_owes = false;
		/*
		 * GIVE WAY BY ASKING FOR LESS, not merely by asking again. See
		 * elastic_initial_us. Only the elastic class is shortened, and
		 * `g.extendable` is exactly that class - it is the flag that
		 * says this request is allowed to grow, which is the same thing
		 * as saying its length was ours to choose.
		 */
		if (g.extendable) {
			if (elastic_initial_us > ELASTIC_FLOOR_US) {
				elastic_initial_us /= 2u;
				if (elastic_initial_us < ELASTIC_FLOOR_US) {
					elastic_initial_us = ELASTIC_FLOOR_US;
				}
			}
			/* And ask somewhere else next time. See elastic_skew_us. */
			elastic_skew_us += ELASTIC_SKEW_STEP_US;
			if (elastic_skew_us >= ELASTIC_SKEW_SPAN_US) {
				elastic_skew_us -= ELASTIC_SKEW_SPAN_US;
			}
		}
		/*
		 * A run of refusals means the anchor itself is the problem,
		 * not the request length or phase (both were tried first and
		 * changed nothing). Since the anchor deliberately does not
		 * move on refusal, an unbroken run of blocks ages it without
		 * bound - measured against a 100 ms advertiser: two grants
		 * then 93 consecutive blocks, distance over a minute. So
		 * after a short run the anchor is abandoned and the next
		 * acquire re-bootstraps with EARLIEST, the one request type a
		 * busy neighbour can't starve; cheap since a fresh anchor
		 * costs one minimum-length timeslot.
		 */
		if (++blocked_run >= BLOCKED_RUN_REANCHOR) {
			blocked_run = 0u;
			g.anchor_valid = false;
		}
		deadline_stop();
		if (g.pending && g.bootstrapping) {
			/* Nothing is behind it and nothing needs telling; the
			 * next real request bootstraps again. */
			g.pending = false;
			g.bootstrapping = false;
			break;
		}
		if (g.pending) {
			g.pending = false;
			/*
			 * Handed on rather than called. This IS a cooperative
			 * thread - mpsl_low_priority_process() - so calling the
			 * core from here would be legal under the amended
			 * callback contract; it is still deferred, because what
			 * it leads to is a scheduler pass that may place the
			 * next reservation, and doing that from inside MPSL's
			 * own low-priority processing is asking MPSL to
			 * re-enter itself.
			 */
			submit_denial();
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_SESSION_IDLE:
		/*
		 * IDLE means no timeslot running and no request outstanding,
		 * the only signal that says so unconditionally - so g.granted
		 * is cleared here as a backstop. Without it the gate stalls
		 * after exactly one grant: a timeslot MPSL ends by its own
		 * length expiry signals nothing we handle, so g.granted would
		 * stay true and every later gate_acquire() would take the
		 * "grant already held" branch against a long-dead grant_end
		 * and return GATE_DENIED forever. The END paths still clear
		 * it too; this only backstops the ones that never signal.
		 *
		 * Test is g.hw_held, not g.granted: gate_release() clears
		 * g.granted before its compare fires, so a lost compare would
		 * otherwise arrive here with g.granted already false and the
		 * radio still on loan, and this backstop would miss it.
		 * hw_held is exactly "radio out and not yet returned".
		 */
		g.granted = false;
		if (g.hw_held) {
			/* Counted apart from late_disarm on purpose. This one is
			 * the designed backstop for a timeslot MPSL ended by its
			 * own length expiry, which signals nothing we handle, so
			 * it can be non-zero in a healthy run and cannot carry a
			 * must-be-zero bar. late_disarm cannot. */
			stats.idle_disarm++;
		}
		/*
		 * The disarm itself is unconditional, which is a different
		 * question from the counter's ("was the radio still out?" vs
		 * "could a compare still be armed?"). gate_release() hands the
		 * radio back immediately but deliberately leaves the compare
		 * armed, so the state most needing cleanup here - a release
		 * whose compare was then lost - arrives with hw_held already
		 * false and an interrupt still armed on a peripheral that's no
		 * longer ours (the re-entering interrupt of bug 14).
		 */
		END_SITE(END_SITE_SESSION);
		timer0_disarm();
		/* "No request is outstanding" is exactly what g.mpsl_owes
		 * tracks, so this is its authoritative clear - the backstop for
		 * any answer path that did not run. The re-place below sets it
		 * again on acceptance. */
		g.mpsl_owes = false;
		/* Where a request that was too early finally gets placed -
		 * mpsl_timeslot_request() answers -NRF_EAGAIN until this
		 * point, which is the normal case, not the exception. See
		 * request_work_fn. */
		if (g.pending) {
			radiant_swi_pend(RADIANT_SWI_GATE_REQUEST);
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED:
		/*
		 * Can arrive with an operation armed and no grant held, and is
		 * one of the two paths (radiant_radio_disable() is the other)
		 * that otherwise only run during suspend and resume - which is
		 * to say never on a bench. The armed operation must still get
		 * its terminal event, or the core waits for one that is not
		 * coming.
		 */
		g.granted = false;
		g.anchor_valid = false;
		g.mpsl_owes = false;
		/* Counter guarded, call unconditional - see SESSION_IDLE
		 * above; the compare gate_release() leaves armed outlives the
		 * hand-back, so a skipped disarm would leave an interrupt
		 * armed on a peripheral about to stop being ours. */
		if (g.hw_held) {
			stats.idle_disarm++;
		}
		END_SITE(END_SITE_SESSION);
		timer0_disarm();
		deadline_stop();
		if (g.pending) {
			g.pending = false;
			if (g.bootstrapping) {
				g.bootstrapping = false;
			} else {
				submit_denial();
			}
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_INVALID_RETURN:
	case MPSL_TIMESLOT_SIGNAL_OVERSTAYED:
	default:
		/*
		 * Both are programming errors here, and MPSL asserts right
		 * after this returns, so recording which one happened here is
		 * the only way to read it from mpsl_assert_handle(). Opposite
		 * fixes: OVERSTAYED means the end margin is too small,
		 * INVALID_RETURN means an action was returned from a signal
		 * that may not carry one.
		 */
		if (signal == MPSL_TIMESLOT_SIGNAL_OVERSTAYED) {
			stats.overstayed++;
		} else if (signal == MPSL_TIMESLOT_SIGNAL_INVALID_RETURN) {
			stats.invalid_return++;
		} else {
			stats.unknown_signal = signal;
		}
		break;
	}

	/*
	 * The last word on what may be returned, in one place: every branch
	 * above decides what it wants, this decides what is legal (bug 14 -
	 * an action after the timeslot already ended). Can't be done branch
	 * by branch since the wrong branch is whichever loses a race it can't
	 * see.
	 */
	if (g.ended) {
		rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
	} else if (rv.callback_action == MPSL_TIMESLOT_SIGNAL_ACTION_END) {
		g.ended = true;
	}

	trace_put(signal, rv.callback_action);
	in_signal = false;
	return &rv;
}

/* ---------------------------------------------------------------------------
 * The interface
 * ---------------------------------------------------------------------------
 */

int gate_init(void)
{
	int32_t rc;

	if (session_open) {
		return RADIANT_RADIO_OK_RC;
	}
	memset(&g, 0, sizeof(g));
	memset(&stats, 0, sizeof(stats));
	/*
	 * The memset above would leave the hand-back margin at zero, and a zero
	 * margin means a compare placed at the very end of a grant - an overstay
	 * by construction. Opened at the WIDER of the two: every grant commits
	 * its own class's margin (commit_next_grant()), so this only covers the
	 * window before the first commit, and covering it conservatively is free.
	 */
	g.end_margin_us = END_MARGIN_EXTEND_US;

	if (IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_NO_SESSION)) {
		/* Bisection aid, off by default. See the Kconfig help. */
		LOG_WRN("gate: session NOT opened (diagnostic build)");
		return RADIANT_RADIO_OK_RC;
	}
	/*
	 * The trampoline before the session: once a session is open a grant can
	 * arrive, and every deferral from inside one goes through this line.
	 *
	 * ONCE PER BOOT, not once per gate_init(). The subscription outlives the
	 * session on purpose - a subscriber list has no way to unregister and no
	 * need to, since the handler is a file static that is always valid.
	 * Without this guard a suspend/resume cycle (gate_shutdown() then
	 * gate_init(), which is also how every test in radiant/tests/gate
	 * starts) adds an entry each time and the table is full on the fourth:
	 * measured as `gate: radiant_swi_register: -12` and a gate that then
	 * refuses to initialise at all.
	 */
	/*
	 * The queue before the trampoline, and the trampoline before the
	 * session: the moment a session is open a grant can arrive, and the
	 * trampoline's only job is to submit to this queue.
	 *
	 * ONCE PER BOOT and never stopped, for the same reason the subscription
	 * above is - but enforced harder: k_work_queue_start() asserts on a
	 * queue that already carries K_WORK_QUEUE_STARTED, so a suspend/resume
	 * (gate_shutdown() then gate_init()) without this guard is a kernel
	 * assert rather than a leak. Nothing needs stopping either; an idle
	 * queue thread costs only its stack, and draining it at shutdown would
	 * mean blocking here on work that calls back into the core.
	 */
	if (!gate_wq_started) {
		static const struct k_work_queue_config gate_wq_cfg = {
			.name = "radiant_gate",
		};

		k_work_queue_start(&gate_wq, gate_wq_stack,
				   K_THREAD_STACK_SIZEOF(gate_wq_stack),
				   CONFIG_RADIANT_GATE_WQ_PRIO, &gate_wq_cfg);
		gate_wq_started = true;
	}
	if (!swi_registered) {
		rc = (int32_t)radiant_swi_register(RADIANT_SWI_GATE_REQUEST |
						   RADIANT_SWI_GATE_DENY |
						   RADIANT_SWI_GATE_DEADLINE,
						   gate_swi_handler);
		if (rc != 0) {
			LOG_ERR("gate: radiant_swi_register: %d", (int)rc);
			return RADIANT_RADIO_EIO;
		}
		swi_registered = true;
	}
	rc = mpsl_timeslot_session_open(on_signal, &session_id);
	LOG_INF("gate: session_open rc=%d, max_window=%u us, place_lead=%u us",
		(int)rc, (unsigned)MAX_WINDOW_US, (unsigned)PLACE_LEAD_US);
	if (rc != 0) {
		/* Almost always CONFIG_MPSL_TIMESLOT_SESSION_COUNT still at its
		 * default of 0, sizing the context array to nothing. */
		return RADIANT_RADIO_EIO;
	}
	session_open = true;
	/* Spend one minimum-length EARLIEST timeslot on nothing but its start
	 * time, so that every window the core asks for is a NORMAL request that
	 * lands where it was asked for. */
	bootstrap_anchor();
#if defined(CONFIG_RADIANT_SWEEP_DEBUG)
	k_work_schedule_for_queue(&gate_wq, &gate_dump, K_SECONDS(1));
#endif
	return RADIANT_RADIO_OK_RC;
}

void gate_shutdown(void)
{
	if (!session_open) {
		return;
	}
	deadline_stop();
	/* g.hw_held, not g.granted: the session must not be closed with the
	 * 802.15.4 driver's SUBSCRIBE_RXEN still swapped out, and gate_release()
	 * clears g.granted a good deal earlier than the radio comes back. */
	if (g.hw_held) {
		stats.idle_disarm++;
		END_SITE(END_SITE_SESSION);
		timer0_disarm();
	}
	(void)mpsl_timeslot_session_close(session_id);
	session_open = false;
	g.pending = false;
	g.granted = false;
	g.anchor_valid = false;
}

uint32_t gate_max_window_us(void)
{
	return MAX_WINDOW_US;
}

uint32_t gate_min_arm_lead_us(void)
{
	/*
	 * Without this the core posted windows at `now + 168 us` (the
	 * hardware's own arm lead), all nearer than the arbiter can grant, so
	 * gate_acquire() refused everything silently. HEAD_MARGIN_US is
	 * included too since what must clear the arbiter's placement lead is
	 * the GRANT START, HEAD_MARGIN_US earlier than the caller's instant.
	 * The backend adds its own ARM_LEAD_US on top (radiant_radio_caps_get())
	 * additively, not as a max, since placing the reservation and
	 * programming the peripheral are sequential steps.
	 */
	return PLACE_LEAD_US + HEAD_MARGIN_US;
}

enum gate_rc gate_acquire(enum gate_op op, radiant_time_t t_from,
			  radiant_time_t t_to, uint16_t follow_on_us,
			  uint8_t prio)
{
	radiant_time_t now;
	radiant_time_t start;
	radiant_time_t end;
	uint64_t       len;
	/*
	 * THE CLASSIFICATION, DECIDED BEFORE THE LENGTH RATHER THAN AFTER IT.
	 *
	 * Only the gap-filling classes (scan chunk, energy-detect sweep) grow
	 * themselves; a tracked window or a transmit has an end the core chose
	 * and must not be held past it. That has always been the test - it used
	 * to be written straight into next_grant.extendable fifty lines below,
	 * after `len` was already computed. It cannot stay there: `len` is the
	 * request MPSL is handed and it now depends on the class, since a
	 * non-extendable grant is charged the shorter END_MARGIN_FIXED_US.
	 *
	 * Locals, and staged into next_grant at the old site, so that the early
	 * refusals between here and there still leave next_grant untouched -
	 * it describes the NEXT grant and is committed only when MPSL accepts.
	 */
	const bool     extendable = (op != GATE_OP_TX) &&
				(prio == RADIANT_GATE_PRIO_NORMAL);
	const uint32_t end_margin_us =
		extendable ? END_MARGIN_EXTEND_US : END_MARGIN_FIXED_US;

	if (!session_open) {
		stats.den_no_session++;
		return GATE_DENIED;
	}
	now = radiant_radio_now();

	/* Bring-up printks removed: with CONFIG_LOG_MODE_IMMEDIATE a printk
	 * blocks ~87 us/char, so a 60-char trace line here would be ~5 ms of
	 * blocking in the arm path against the ~3 ms lead the request was
	 * built with. See the header - the instrument was moving the thing it
	 * measured. (Deferred LOG_* was tried first and dropped the messages
	 * under a refusal flood, reading as "never called".) */

	stats.acquires++;
	if (prio == RADIANT_GATE_PRIO_HIGH) {
		stats.hi_acq++;
	}

	if (g.granted) {
		stats.acq_in_grant++;
		/*
		 * A grant is already held, which is what follow_on_us is for:
		 * radiant_transfer.c arms the acknowledged-data reply from
		 * inside the receive callback, 1.56 ms before it must be on
		 * the air - too late to place a new request, and none is
		 * needed since the air was already reserved. Test is whether
		 * the follow-on fits inside what we hold; if not, refusing is
		 * honest (overstaying would assert).
		 *
		 * Must start inside the grant too, not just end inside it - a
		 * window starting beyond grant_end would be programmed
		 * against air we don't have, which the end test alone misses
		 * whenever grant_end is generous.
		 */
		if (t_from >= now &&
		    t_to + (radiant_time_t)follow_on_us +
				    (radiant_time_t)TAIL_MARGIN_US <=
			    g.grant_end) {
			g.release_wanted = false;
			if (prio == RADIANT_GATE_PRIO_HIGH) {
				stats.hi_reserve++;
			}
			return GATE_GRANTED;
		}
		/*
		 * Not fitting is not the same as being refused. Bug 18, 50%
		 * of every tracked packet: this used to `return GATE_DENIED`
		 * unconditionally here, true only for a reply. The common
		 * case is a tracked window's next slot arming a quarter
		 * second out mid-window (radiant_radio_hal.h's low-jitter
		 * callback contract) while the current timeslot is still
		 * live - asking whether 250 ms fits inside a 5.5 ms grant
		 * and being refused lost the slot every other window,
		 * measured at 50.6% loss (exact) with blocked=0 the whole
		 * time.
		 *
		 * A request starting after this grant ends is placed like
		 * any ordinary future reservation by falling through. A
		 * genuine ~1.56 ms-out reply still gets its synchronous
		 * refusal from the too-near test below. The counter now
		 * means "arms placed rather than served from the reserve".
		 */
		stats.acq_in_grant_denied++;
	}

	if (g.pending) {
		/* One request outstanding per session, which is also the HAL's
		 * one-operation rule. The backend refuses a second arm with
		 * EBUSY before reaching here, so this is a backstop. */
		stats.den_pending++;
		return GATE_DENIED;
	}

	if (g.mpsl_owes) {
		/*
		 * g.pending is clear but MPSL still holds the last request.
		 * Placing now would just cost an -NRF_EAGAIN round trip and
		 * re-arm the backstop - one turn of the DEADLINE_SLACK_US
		 * spiral. Refuse here instead. Only reachable when the
		 * backstop beat MPSL's answer, so should stay near zero;
		 * counted rather than assumed.
		 */
		stats.den_owed++;
		return GATE_DENIED;
	}

	start = (t_from > (radiant_time_t)HEAD_MARGIN_US)
			? (t_from - (radiant_time_t)HEAD_MARGIN_US)
			: 0u;
	/* The reservation covers the operation, the air a follow-on may need,
	 * and the tail. follow_on_us is measured from t_to by the HAL's
	 * definition, so it simply extends the end. */
	end = t_to + (radiant_time_t)follow_on_us +
	      (radiant_time_t)TAIL_MARGIN_US;

	if (end <= start) {
		stats.den_degenerate++;
		return GATE_DENIED;
	}

	/*
	 * BUG 22: THE PHASE SKEW USED TO BE APPLIED HERE, AND IT MOVED THE
	 * RESERVATION WITHOUT MOVING THE OPERATION.
	 *
	 * The two lines that were here added elastic_skew_us to both `start`
	 * and `end` for an elastic request. But `start`/`end` describe the
	 * TIMESLOT, while the operation staged behind it keeps the absolute
	 * t_open/t_close the core chose - so a skew of S bought a grant that
	 * began S microseconds after the window it was supposed to cover was
	 * already due to open. program_rx() then finds req->t_open in the past
	 * and refuses RADIANT_RADIO_ETIME, which is exactly the right answer to
	 * the wrong question: it is the reservation that was misplaced, not the
	 * window that was late.
	 *
	 * This is only visible once windows work at all. With the skew walking
	 * to 11401 us: grant=1236, prog=127 ok against 689 ETIME, 642 chunks
	 * FAILED, one CRC-good frame in 60 s. Every refusal fed the skew one
	 * more step, so it is self-sustaining as well as self-inflicted.
	 *
	 * The reservation MUST cover the operation - that is the whole contract
	 * of this function - so a gate cannot decorrelate itself by sliding one
	 * and not the other. Removed rather than repaired: shifting the
	 * operation too is not this layer's to do (the core owns those
	 * instants), and the phase problem the skew was written for already has
	 * a mechanism that does not lie about placement - BLOCKED_RUN_REANCHOR
	 * abandons the anchor after a run of refusals and the next request
	 * re-bootstraps with EARLIEST, which lands wherever the arbiter has
	 * room and so changes phase by construction.
	 *
	 * WHAT IS NOT PROVEN. The skew was introduced against a 100 ms BLE
	 * advertiser (measured then: placed=570 granted=2 blocked=568) and this
	 * removal has been measured only against OpenThread. The elastic
	 * request-length backoff (elastic_initial_us) is untouched and was the
	 * other half of that fix. Re-measure the BLE arm before treating the
	 * advertiser case as settled.
	 */
	(void)elastic_skew_us;
	/*
	 * The reservation is longer than the air it covers, by exactly the
	 * hand-back time: the timeslot must outlive `end` by this class's end
	 * margin, since the compare returning ACTION_END is placed that far
	 * before expiry. Without it the receiver would be taken away
	 * mid-window, which reads at the core as a sensor gone quiet.
	 *
	 * This is the site the split is FOR. A tracked window or a transmit
	 * pays END_MARGIN_FIXED_US here instead of END_MARGIN_EXTEND_US, so
	 * every such reservation is 1250 us shorter than it used to be - air
	 * that was reserved and never used, and that the other stack could not
	 * have while it was reserved.
	 */
	len = (uint64_t)(end - start) + (uint64_t)end_margin_us;
	if (len < MPSL_TIMESLOT_LENGTH_MIN_US) {
		len = MPSL_TIMESLOT_LENGTH_MIN_US;
	}
	if (len > MPSL_TIMESLOT_LENGTH_MAX_US) {
		/* caps.max_window_us exists so the scheduler never asks for
		 * this (radiant_sched.c's window_cap()); reaching here means
		 * the two disagree, so refuse rather than silently shorten. */
		stats.refused_too_long++;
		if (stats.refused_too_long <= 2u) {
			/* Cast rather than %lu since both constants are UL in
			 * nrfxlib's header - avoids a -Wformat warning. */
			printk("gate: too long - %u us, max %u (window cap %u)\n",
			       (unsigned)len,
			       (unsigned)MPSL_TIMESLOT_LENGTH_MAX_US,
			       (unsigned)MAX_WINDOW_US);
		}
		return GATE_DENIED;
	}

	/*
	 * Too near to place at all - measured grant latency is ~1695 us, so a
	 * request nearer than PLACE_LEAD_US can't be honoured whatever the
	 * arbiter decides. DENIED, not ETIME: the instant itself is reachable,
	 * only the air is missing. This is the synchronous refusal the
	 * acknowledged-data reply path needs, since it arms with 1.56 ms to go.
	 */
	if (start < now + (radiant_time_t)PLACE_MIN_US) {
		stats.refused_too_near++;
		if (stats.refused_too_near <= 3u) {
			printk("gate: too near - start +%d us, need +%u\n", (int)(int64_t)(start - now), PLACE_MIN_US);
		}
		return GATE_DENIED;
	}

	memset(&req, 0, sizeof(req));

	if (!g.anchor_valid) {
		/*
		 * No anchor, so go get one rather than asking for this window
		 * with EARLIEST - EARLIEST grants a timeslot with no fixed
		 * relationship to the request's start, and using it for a
		 * real window once produced one FAILED operation (ETIME) and
		 * a channel that stopped searching. See bootstrap_anchor().
		 * DENIED is accurate here - the air genuinely wasn't obtained
		 * - and the bootstrap behind it gets an anchor for next time.
		 */
		stats.den_no_anchor++;
		bootstrap_anchor();
		return GATE_DENIED;
	}
	/* distance_us is measured from the previous timeslot's START,
	 * not now or an absolute time - and the anchor doesn't move
	 * on a blocked request, keeping a run of denials from walking
	 * the schedule. */
	if (start <= g.anchor) {
		stats.refused_too_near++;
		return GATE_DENIED;
	}
	if ((start - g.anchor) > MPSL_TIMESLOT_DISTANCE_MAX_US) {
		/* Further out than a NORMAL request can express. Fall
		 * back to a fresh bootstrap rather than truncating the
		 * distance, which would place the grant in the wrong
		 * place entirely. */
		stats.den_distance++;
		g.anchor_valid = false;
		return GATE_DENIED;
	}
	req.request_type = MPSL_TIMESLOT_REQ_TYPE_NORMAL;
	req.params.normal.hfclk =
		MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED;
	/* PRIORITY_HIGH for tracked slots/transmits, NORMAL for
	 * elastic work. Only ranks our own requests against each
	 * other - the SoftDevice Controller holds levels above
	 * anything an application can request. */
	req.params.normal.priority =
		(prio == RADIANT_GATE_PRIO_HIGH)
			? MPSL_TIMESLOT_PRIORITY_HIGH
			: MPSL_TIMESLOT_PRIORITY_NORMAL;
	req.params.normal.distance_us = (uint32_t)(start - g.anchor);
	/* length_us is written BELOW, after the elastic clamp - see bug 25. */

	/* Staged, not written through `g` - this function can run during a
	 * live grant, and these describe the NEXT one. See next_grant and
	 * commit_next_grant(). */
	next_grant.want_len_us = (uint32_t)len;
	next_grant.grant_end = end;
	/* Decided at the top of the function, because `len` above depends on
	 * it. See the declarations there. */
	next_grant.extendable = extendable;
	/* And the margin travels with the request that was built from it: every
	 * compare for this grant is placed against g.end_margin_us, which is
	 * this value once commit_next_grant() runs. */
	next_grant.end_margin_us = end_margin_us;
	next_grant.high_prio = (prio == RADIANT_GATE_PRIO_HIGH);

	/*
	 * The elastic class asks small and grows (ADR 0013); everything else
	 * asks for exactly what it needs. Measured working: 15 scan chunks of
	 * 94200 us, 1596 extensions, zero refusals, growing 1 ms at a time
	 * from 10 ms.
	 */
	if (extendable && len > (uint64_t)elastic_initial_us) {
		len = elastic_initial_us;
	}
	next_grant.granted_len_us = (uint32_t)len;

	/*
	 * BUG 25: THE ELASTIC BACKOFF NEVER REACHED THE ARBITER. THIS ASSIGNMENT
	 * USED TO SIT TWENTY LINES ABOVE, BEFORE THE CLAMP.
	 *
	 * `len` above the clamp is the whole window plus margins - for a scan
	 * chunk, MAX_WINDOW_US + 2500 = 96700 us. The clamp then shortened only
	 * next_grant.granted_len_us, this file's own bookkeeping for the grant it
	 * expected to receive; the request MPSL was handed still asked for the
	 * whole 96.7 ms. So every mechanism written to make the elastic class give
	 * way - elastic_initial_us halving on BLOCKED down to ELASTIC_FLOOR_US,
	 * growing back one EXTEND_STEP_US per grant - moved a number that was
	 * never in the request, and the whole extension chain (ADR 0013's "ask
	 * small and grow") was dead code beside any contending stack: a grant that
	 * large is never given, so SIGNAL_START never arrives to grow it.
	 *
	 * The gate's own header (ELASTIC_INITIAL_US) records exactly this shape as
	 * a measurement from bring-up - "a standing demand for ~96% of the radio
	 * that MPSL simply refuses (one EARLIEST grant, then every NORMAL request
	 * BLOCKED forever, chunks=0)" - and that is what the P4 MED bench showed
	 * 600 s of: placed=57481 granted=11517 blocked=45984, every block answered
	 * inline from inside mpsl_timeslot_request() (inl=45984), all 11517 grants
	 * being 100 us bootstrap anchors (len=100/100), and
	 * radiant_nrf_gate_on_grant() entered zero times.
	 *
	 * It was invisible to the ztest suite because the elastic test asserts on
	 * granted_len_us < want_len_us - the two internal numbers, both correct -
	 * and never on what fake_mpsl_last_request() was actually given. A test for
	 * that is added alongside this fix.
	 *
	 * So: one assignment, and it must be the LAST word on the length, below
	 * every clamp. next_grant.granted_len_us is used rather than `len` so the
	 * two can never disagree again - what we ask for IS what SIGNAL_START will
	 * measure the grant against.
	 */
	req.params.normal.length_us = next_grant.granted_len_us;
#if defined(CONFIG_RADIANT_SWEEP_DEBUG)
	last_req_len_us = next_grant.granted_len_us;
	last_req_dist_us = req.params.normal.distance_us;
	last_req_lead_us = (uint32_t)(start - now);
	if (last_req_lead_us < min_req_lead_us) {
		min_req_lead_us = last_req_lead_us;
	}
#endif

	/*
	 * Handed to a thread, not called from here: radiant_sched.c arms from
	 * the RADIO interrupt at priority 0, possibly from inside a timeslot,
	 * where mpsl_timeslot_request() is not callable. The request is built
	 * here and placed on the work queue where the call is legal; the core
	 * is told PENDING either way.
	 */
	g.pending = true;
	if (prio == RADIANT_GATE_PRIO_HIGH) {
		stats.hi_pending++;
	}
	radiant_swi_pend(RADIANT_SWI_GATE_REQUEST);

	/* The backstop: the operation's own start plus DEADLINE_SLACK_US
	 * (see its comment for why not zero). */
	deadline_arm((uint32_t)(t_from - now) + DEADLINE_SLACK_US);

	return GATE_PENDING;
}

void gate_release(void)
{
	if (!g.granted) {
		/* Nothing held. The abort path calls this without knowing,
		 * and a pending request is cancelled by the core's own
		 * terminal rather than here. */
		g.pending = false;
		deadline_stop();
		/*
		 * This branch is reached with the radio still ours in the
		 * window between a previous gate_release() and its compare
		 * firing (g.granted is the core's permission, not the
		 * hardware loan). Idempotent: a no-op on ordinary double
		 * release; on the lost-compare case it stops
		 * radiant_radio_disable() writing the shared interrupt mask
		 * while SUBSCRIBE_RXEN is still swapped out.
		 */
		END_SITE(END_SITE_RELEASE);
		gate_hand_back();
		return;
	}

	/*
	 * Give the air back by provoking a signal: a timeslot can only be
	 * ended by returning ACTION_END from a callback, and this runs in
	 * the RADIO interrupt. So the flag is set and the TIMER0 compare is
	 * moved to just ahead of the current count; MPSL delivers
	 * SIGNAL_TIMER0 and END returns from there. Promptness matters: an
	 * empty tracked window's ~2 ms follow_on reserve, given back late,
	 * costs the other stack air rather than just scheduler time.
	 */
	/*
	 * g.granted is cleared here, not when ACTION_END returns, or a
	 * gate_acquire() in the few microseconds before the TIMER0 signal
	 * would take the "grant already held" branch, program the compare/
	 * (D)PPI for air already handed back, and wedge the core's single
	 * operation slot forever (bench-observed: dongle answers serial
	 * commands, opens its channel, then goes silent with no fault).
	 * Clearing it first routes any such acquire through the ordinary
	 * NORMAL-request path instead.
	 */
	g.granted = false;
	g.release_wanted = true;

	/*
	 * Bug 9: a compare written behind the counter never fires. The old
	 * value here was +2 us, and reading CAPTURE1/writing CC0/MPSL
	 * noticing does not reliably happen inside two ticks of a 1 MHz
	 * timer - when it doesn't, the only signal that would end this
	 * timeslot is gone and MPSL asserts on the overstay (measured:
	 * placed=21 granted=20, 1596 clean extensions - not the extension
	 * chain, this compare). RELEASE_LEAD_US gives the write a realistic
	 * head start; the re-read below puts the backstop compare back if
	 * even that lost the race, so a future compare always exists. Late
	 * is survivable; absent is not.
	 */
	{
		/* The backstop compare is the one SIGNAL_START armed, so it is
		 * placed against the SAME margin that armed it - this grant's
		 * class margin, not a constant. Naming END_MARGIN_EXTEND_US
		 * here would put the backstop 1250 us early on every tracked
		 * window and every transmit: not an overstay, but it would end
		 * those grants before the air they were reserved for, which at
		 * the core reads as a sensor gone quiet. */
		uint32_t cc_backstop = (g.granted_len_us > g.end_margin_us)
					       ? (g.granted_len_us - g.end_margin_us)
					       : (g.granted_len_us / 2u);
		uint32_t want;

		nrf_timer_task_trigger(MPSL_TIMER0, NRF_TIMER_TASK_CAPTURE1);
		want = nrf_timer_cc_get(MPSL_TIMER0, 1) + RELEASE_LEAD_US;

		if (want < cc_backstop) {
			nrf_timer_cc_set(MPSL_TIMER0, 0, want);
			nrf_timer_event_clear(MPSL_TIMER0,
					      NRF_TIMER_EVENT_COMPARE0);
			nrf_timer_int_enable(MPSL_TIMER0,
					     NRF_TIMER_INT_COMPARE0_MASK);

			nrf_timer_task_trigger(MPSL_TIMER0,
					       NRF_TIMER_TASK_CAPTURE1);
			if (nrf_timer_cc_get(MPSL_TIMER0, 1) >= want &&
			    !nrf_timer_event_check(MPSL_TIMER0,
						   NRF_TIMER_EVENT_COMPARE0)) {
				nrf_timer_cc_set(MPSL_TIMER0, 0, cc_backstop);
			}
		} else {
			/* Already inside the end margin - the armed compare
			 * is the right one, moving it can only lose. */
			nrf_timer_int_enable(MPSL_TIMER0,
					     NRF_TIMER_INT_COMPARE0_MASK);
		}
		/*
		 * The radio restore must not depend on the compare actually
		 * firing. The compare stays armed (gate_hand_back(), not
		 * timer0_disarm()) since MPSL only ends a timeslot that way -
		 * but if it's lost (bug 9, rare with RELEASE_LEAD_US but not
		 * impossible), SESSION_IDLE still arrives eventually while
		 * SUBSCRIBE_RXEN sits swapped out and the receiver stays deaf
		 * in the meantime. So hand back now rather than wait for that
		 * backstop; a later TIMER0 signal just finds the radio
		 * already returned.
		 */
		stats.release_disarm++;
		END_SITE(END_SITE_RELEASE);
		gate_hand_back();
	}
}

bool gate_extend(uint32_t us)
{
	/* Extensions are driven from inside the signal callback via the
	 * ACTION_EXTEND return - a call here couldn't express one. Present in
	 * the interface only because a RAIL gate's equivalent would be a
	 * function call, and the seam must be expressible for both. */
	ARG_UNUSED(us);
	return false;
}

#if defined(CONFIG_MPSL_ASSERT_HANDLER)
void mpsl_assert_handle(const char *const file, const uint32_t line)
{
	/*
	 * The two that matter, both this file's fault:
	 *   OVERSTAYED - timeslot closed late, TAIL_MARGIN_US too small (the
	 *                plan's most-likely field failure: an occasional
	 *                assert under a busy Thread network).
	 *   INVALID    - an action returned from a low-priority signal.
	 * Printed with the counters since the assert site alone can't tell
	 * them apart.
	 */
	/*
	 * LOG_PANIC() first, or this handler is silent: printk routes through
	 * the log subsystem by default (CONFIG_LOG_PRINTK=y), and in deferred
	 * mode that needs a thread that's about to be halted. LOG_PANIC()
	 * switches to synchronous output for exactly this case.
	 */
	LOG_PANIC();
	printk("\n*** MPSL ASSERT %s:%u  placed=%u granted=%u blocked=%u "
	       "ext=%u/%u deadline=%u\n"
	       "    over=%u invalid=%u unknown=%u last_sig=%u "
	       "len=%u/%u granted=%d pending=%d rel=%d ext_ok=%d\n",
	       file, line, stats.placed, stats.granted, stats.blocked,
	       stats.extends_ok, stats.extends_failed, stats.deadline_fired,
	       stats.overstayed, stats.invalid_return, stats.unknown_signal,
	       stats.last_signal, g.granted_len_us, g.want_len_us,
	       (int)g.granted, (int)g.pending, (int)g.release_wanted,
	       (int)g.extendable);
	printk("    trace (sig:act, oldest first):");
	for (uint32_t i = 0; i < TRACE_LEN; i++) {
		uint32_t idx = (trace_at >= TRACE_LEN)
				       ? ((trace_at + i) % TRACE_LEN)
				       : i;

		if (trace_at < TRACE_LEN && i >= trace_at) {
			break;
		}
		printk(" %u:%u", trace[idx] >> 4, trace[idx] & 0xFu);
	}
	printk("\n");
	k_fatal_halt(K_ERR_KERNEL_PANIC);
}
#endif







