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
 *   - gate_dump_fn() under CONFIG_RADIANT_CORE_SWEEP_DEBUG: a once-a-second
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
 *       -DRADIANT_BACKEND=nrf -DCONFIG_RADIANT_CORE_BACKEND_NRF_GATE_MPSL=y \
 *       -DCONFIG_MPSL=y -DCONFIG_MPSL_TIMESLOT_SESSION_COUNT=1 \
 *       -DCONFIG_RADIANT_CORE_SWEEP_DEBUG=y
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
 * CONFIG_RADIANT_CORE_BACKEND_NRF_GATE_MPSL is off by default; the direct
 * build stays green (core ztest 28 suites, api ztest 22, host tools 587,
 * DATA=30/60s). The only change to shipping code is CLAIM_TERMINAL() in
 * radiant_radio_nrf.c, moving an assignment from the programming half of an
 * arm to the claiming half - the same instant on a direct build.
 *
 * Every constant below is derived from a measurement in
 * radiant_core/spike/mpsl_arb/README.md; the four load-bearing ones:
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
 * clock MPSL measures the overstay against; radiant_core's own 1 MHz TIMER is
 * the absolute timebase and is untouched here, and conflating the two is how
 * a backend asserts on a drift it can't see.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>

#include <mpsl_timeslot.h>
#include <mpsl_hwres.h>
#include <nrf_errno.h>
#include <hal/nrf_timer.h>

#include "radiant_radio_nrf_gate.h"

LOG_MODULE_REGISTER(radiant_gate_mpsl, CONFIG_RADIANT_CORE_LOG_LEVEL);

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
#define PLACE_LEAD_US 2500u

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
#define PLACE_MIN_US 1900u

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
 * END_MARGIN_US (defined below) is included here because gate_acquire() adds
 * it to every request length so the timeslot outlives the air it covers by
 * the hand-back time - same class of mistake as bug 3 if omitted. The
 * BUILD_ASSERT on MAX_WINDOW_US further down is what makes the forward
 * reference safe.
 */
#define GRANT_OVERHEAD_US                                                     \
	(HEAD_MARGIN_US + TAIL_MARGIN_US + END_MARGIN_US + FRAME_TAIL_US +    \
	 FOLLOW_ON_MAX_US)

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
 */
#define END_MARGIN_US 2000u

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

BUILD_ASSERT(EXTEND_STEP_US >= MPSL_TIMESLOT_EXTENSION_TIME_MIN_US,
	     "an extension smaller than MPSL's own minimum is refused, and the "
	     "refusal would read as the arbiter yielding");
BUILD_ASSERT(END_MARGIN_US > MPSL_TIMESLOT_EXTENSION_MARGIN_MIN_US,
	     "an extension is asked for END_MARGIN_US before the end of the "
	     "grant, so that is the margin MPSL's own minimum has to fit in");
BUILD_ASSERT(ELASTIC_INITIAL_US > END_MARGIN_US + EXTEND_STEP_US,
	     "the first grant must be long enough to contain the compare that "
	     "grows it, or the chain cannot start");
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
	 * - NOT the timeslot expiry, which is END_MARGIN_US later. Set from
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
 * A dedicated queue rather than the system one, since a radio schedule that
 * must place a reservation within a couple of ms cannot queue behind
 * something like a filesystem flush.
 * ---------------------------------------------------------------------------
 */


static void request_work_fn(struct k_work *w);
static void denied_work_fn(struct k_work *w);

static K_WORK_DEFINE(request_work, request_work_fn);
static K_WORK_DEFINE(denied_work, denied_work_fn);

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
} next_grant;

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

BUILD_ASSERT(ELASTIC_FLOOR_US >= MPSL_TIMESLOT_LENGTH_MIN_US,
	     "the elastic floor must still be a timeslot MPSL will grant");
BUILD_ASSERT(ELASTIC_FLOOR_US > END_MARGIN_US,
	     "an elastic grant must outlive its own hand-back margin");

static void commit_next_grant(void)
{
	g.want_len_us    = next_grant.want_len_us;
	g.granted_len_us = next_grant.granted_len_us;
	g.grant_end      = next_grant.grant_end;
	g.extendable     = next_grant.extendable;
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
	k_timer_stop(&deadline);
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
	next_grant.want_len_us = MPSL_TIMESLOT_LENGTH_MIN_US;
	next_grant.granted_len_us = MPSL_TIMESLOT_LENGTH_MIN_US;
	next_grant.grant_end = 0u;
	g.bootstrapping = true;
	g.pending = true;
	k_work_submit(&request_work);
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

#if defined(CONFIG_RADIANT_CORE_SWEEP_DEBUG)
/*
 * A once-a-second dump - the only instrument that shows a stall. Every other
 * diagnostic here is emitted BY an event, so a gate that stopped producing
 * events produces no diagnostics either. Fixed period on its own thread, so
 * it reports absence of activity as clearly as presence; deferred logging
 * keeps the cost off the grant path.
 */
static void gate_dump_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(gate_dump, gate_dump_fn);

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

	LOG_INF("gate: acq=%u in_grant=%u/%u placed=%u granted=%u blocked=%u cancel=%u eagain=%u "
		"near=%u long=%u ext=%u/%u dead=%u | "
		"den pend=%u anch=%u dist=%u degen=%u nosess=%u owed=%u | "
		"sent grant=%u dead=%u blk=%u supp=%u | "
		"end=%u (norm=%u rel=%u idle=%u late=%u) | "
		"bad over=%u inval=%u unk=%u inl=%u | "
		"len=%u/%u el=%u skew=%u brun=%u sig=%u "
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
		g.granted_len_us, g.want_len_us,
		elastic_initial_us, elastic_skew_us, blocked_run,
		stats.last_signal, (int)g.granted, (int)g.hw_held, (int)g.pending,
		(int)g.release_wanted, (int)g.ended, (int)g.bootstrapping,
		(int)g.anchor_valid, (int)g.mpsl_owes);
	if (session_open) {
		k_work_schedule(&gate_dump, K_SECONDS(1));
	}
}
#endif /* CONFIG_RADIANT_CORE_SWEEP_DEBUG */

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
	k_work_submit(&denied_work);
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
	k_work_submit(&denied_work);
}

/* A denial with nothing ambiguous about it: no grant is coming for the staged
 * operation. Used by the paths that ARE refusals. */
static inline void submit_denial(void)
{
	g.deny_src = DENY_SRC_BLOCKED;
	g.deny_wanted = true;
	k_work_submit(&denied_work);
}

static mpsl_timeslot_signal_return_param_t *on_signal(
	mpsl_timeslot_session_id_t id, uint32_t signal)
{
	ARG_UNUSED(id);

	/* The trace records sequence for a debugging halt: START then TIMER0
	 * repeating means the compare is re-firing; START then silence means
	 * the RADIO interrupt is spinning and this callback isn't reached at
	 * all - two different fixes, otherwise indistinguishable. */
	rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
	stats.last_signal = signal;

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		stats.granted++;
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
		k_timer_stop(&deadline);

		if (g.bootstrapping) {
			/* The anchor set above is the whole point of this
			 * timeslot; nothing is staged behind it, so hand the
			 * air straight back without touching the peripheral
			 * or the core. */
			g.bootstrapping = false;
			g.granted = false;
			timer0_disarm();
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		}
		/*
		 * What we actually hold, which for an elastic request is a
		 * fraction of what was asked for. END_MARGIN_US of it is spoken
		 * for by the handing back, so it is not air the radio may use.
		 */
		g.grant_end = g.anchor + (radiant_time_t)g.granted_len_us -
			      (radiant_time_t)END_MARGIN_US;

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
		 * END_MARGIN_US (2 ms), not TAIL_MARGIN_US (250 us) - the
		 * latter only covers what the RADIO needs and is not enough
		 * time for the MPSL interrupt/dispatch/callback between the
		 * compare firing and ACTION_END returning. Measured: at
		 * len-250 the board asserted on the first or second timeslot;
		 * with a margin well inside the grant it ran 1007 timeslots
		 * clean. 2 ms is an order of magnitude above Nordic's own
		 * sample's ~166 us SIGNAL_START-to-TIMER0, small against a
		 * 96 ms grant.
		 */
		nrf_timer_cc_set(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL0,
				 (g.granted_len_us > END_MARGIN_US)
					 ? (g.granted_len_us - END_MARGIN_US)
					 : (g.granted_len_us / 2u));
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
		radiant_nrf_gate_on_radio_irq();
		if (g.release_wanted && !g.granted) {
			timer0_disarm();
			end_housekeeping();
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_TIMER0:
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
		 * used TAIL_MARGIN_US instead of END_MARGIN_US, so an
		 * extended grant's compare sat only 250 us before the new
		 * end (vs END_MARGIN_US on the initial grant) - the two
		 * quantities the file elsewhere warns must not be conflated,
		 * conflated here. Symptom was ext=1204/0 asserting at
		 * placed=14: the extension chain was fine, the END that
		 * followed it wasn't. Compare now advances by exactly one
		 * step per extension, keeping END_MARGIN_US of runway ahead
		 * of it always.
		 */
		stats.extends_ok++;
		g.granted_len_us += g.ext_step_us;
		g.grant_end += (radiant_time_t)g.ext_step_us;
		nrf_timer_cc_set(MPSL_TIMER0, 0,
				 g.granted_len_us - END_MARGIN_US);
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
		k_timer_stop(&deadline);
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
			k_work_submit(&request_work);
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
		timer0_disarm();
		k_timer_stop(&deadline);
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

	if (IS_ENABLED(CONFIG_RADIANT_CORE_GATE_MPSL_NO_SESSION)) {
		/* Bisection aid, off by default. See the Kconfig help. */
		LOG_WRN("gate: session NOT opened (diagnostic build)");
		return RADIANT_RADIO_OK_RC;
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
#if defined(CONFIG_RADIANT_CORE_SWEEP_DEBUG)
	k_work_schedule(&gate_dump, K_SECONDS(1));
#endif
	return RADIANT_RADIO_OK_RC;
}

void gate_shutdown(void)
{
	if (!session_open) {
		return;
	}
	k_timer_stop(&deadline);
	/* g.hw_held, not g.granted: the session must not be closed with the
	 * 802.15.4 driver's SUBSCRIBE_RXEN still swapped out, and gate_release()
	 * clears g.granted a good deal earlier than the radio comes back. */
	if (g.hw_held) {
		stats.idle_disarm++;
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

	/* Move an elastic request off whatever phase the last refusal found -
	 * the window slides rather than stretching. See elastic_skew_us; zero
	 * until something is actually refused. */
	if (elastic_skew_us != 0u && op != GATE_OP_TX &&
	    prio == RADIANT_GATE_PRIO_NORMAL) {
		start += (radiant_time_t)elastic_skew_us;
		end   += (radiant_time_t)elastic_skew_us;
	}
	/*
	 * The reservation is longer than the air it covers, by exactly the
	 * hand-back time: the timeslot must outlive `end` by END_MARGIN_US
	 * since the compare returning ACTION_END is placed that far before
	 * expiry. Without it the receiver would be taken away mid-window,
	 * which reads at the core as a sensor gone quiet.
	 */
	len = (uint64_t)(end - start) + (uint64_t)END_MARGIN_US;
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
	req.params.normal.length_us = (uint32_t)len;

	/* Staged, not written through `g` - this function can run during a
	 * live grant, and these describe the NEXT one. See next_grant and
	 * commit_next_grant(). */
	next_grant.want_len_us = (uint32_t)len;
	next_grant.grant_end = end;
	/* Only the gap-filling classes (scan chunk, energy-detect sweep) grow
	 * themselves - a tracked window/transmit has an end the core chose
	 * and must not be held past it. */
	next_grant.extendable = (op != GATE_OP_TX) &&
				(prio == RADIANT_GATE_PRIO_NORMAL);

	/*
	 * The elastic class asks small and grows (ADR 0013); everything else
	 * asks for exactly what it needs. Measured working: 15 scan chunks of
	 * 94200 us, 1596 extensions, zero refusals, growing 1 ms at a time
	 * from 10 ms.
	 */
	if (next_grant.extendable && len > (uint64_t)elastic_initial_us) {
		len = elastic_initial_us;
	}
	next_grant.granted_len_us = (uint32_t)len;

	/*
	 * Handed to a thread, not called from here: radiant_sched.c arms from
	 * the RADIO interrupt at priority 0, possibly from inside a timeslot,
	 * where mpsl_timeslot_request() is not callable. The request is built
	 * here and placed on the work queue where the call is legal; the core
	 * is told PENDING either way.
	 */
	g.pending = true;
	k_work_submit(&request_work);

	/* The backstop: the operation's own start plus DEADLINE_SLACK_US
	 * (see its comment for why not zero). */
	k_timer_start(&deadline,
		      K_USEC((uint32_t)(t_from - now) + DEADLINE_SLACK_US),
		      K_NO_WAIT);

	return GATE_PENDING;
}

void gate_release(void)
{
	if (!g.granted) {
		/* Nothing held. The abort path calls this without knowing,
		 * and a pending request is cancelled by the core's own
		 * terminal rather than here. */
		g.pending = false;
		k_timer_stop(&deadline);
		/*
		 * This branch is reached with the radio still ours in the
		 * window between a previous gate_release() and its compare
		 * firing (g.granted is the core's permission, not the
		 * hardware loan). Idempotent: a no-op on ordinary double
		 * release; on the lost-compare case it stops
		 * radiant_radio_disable() writing the shared interrupt mask
		 * while SUBSCRIBE_RXEN is still swapped out.
		 */
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
		uint32_t cc_backstop = (g.granted_len_us > END_MARGIN_US)
					       ? (g.granted_len_us - END_MARGIN_US)
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






