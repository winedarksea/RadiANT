/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf_gate_mpsl.c - the RADIO, borrowed from MPSL a slot at a
 * time.
 *
 * Provenance: clean-room, against nrfxlib's mpsl_timeslot.h and the in-tree
 * nrf/samples/mpsl/timeslot and nrf/drivers/mpsl/flash_sync patterns. Nothing
 * here derives from sdk-ant or libant.a.
 *
 * Measured on the nRF54L15 DK against a real sensor: MPSL gate and direct
 * build both DATA=30 in 60 s, the gate placing 273 reservations, 272 granted,
 * 2716 extensions, blocked=0. A scan chunk is 94 200 us here vs 260 000 us
 * direct, because caps.max_window_us bounds a window to what MPSL can grant,
 * so sweep takes ~3x the chunks for the same dwell.
 *
 * CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL is off by default. The only change to
 * shipping code is CLAIM_TERMINAL() in radiant_radio_nrf.c, moving an
 * assignment from the programming half of an arm to the claiming half - the
 * same instant on a direct build.
 *
 * WARNING ABOUT THE INSTRUMENT ITSELF: with CONFIG_LOG_MODE_IMMEDIATE=y a
 * printk blocks ~87 us/char on a 115200 console - a 60-char trace line in
 * gate_acquire() is ~5 ms of blocking against the ~3 ms of lead the request
 * was built with. It moves the thing it measures and is trustworthy about
 * SEQUENCE, not TIMING. Do not put a printk back into gate_acquire() or
 * on_signal(); the ring buffer and deferred LOG_INF are the only instruments
 * that don't touch the grant path. Also: CONFIG_LOG_PRINTK=y (default)
 * routes printk through the log subsystem, so in deferred mode a printk from
 * a dying thread is silently dropped, and deferred LOG_* drops messages under
 * a flood of refusals - "never appears" means DROPPED, not "never executed".
 *
 * Every constant below is derived from a measurement taken with the mpsl_arb
 * timeslot spike (radiant/spike/mpsl_arb); the four load-bearing ones:
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
#include <zephyr/sys/reboot.h>

#include <mpsl_timeslot.h>
#include <mpsl_hwres.h>
#include <nrf_errno.h>
#include <hal/nrf_timer.h>

#include <radiant/radiant_swi.h>

#include "radiant_radio_nrf_gate.h"

LOG_MODULE_REGISTER(radiant_gate_mpsl, CONFIG_RADIANT_LOG_LEVEL);

/*
 * CONFIG_MPSL_TIMESLOT_SESSION_COUNT carries `default 0`, which sizes MPSL's
 * session context array to nothing. With it, this file configures, compiles,
 * links and boots - gate_init() logs a failed mpsl_timeslot_session_open() and
 * the gate then never gets air. A .config read-back cannot see it (both
 * symbols are present and both look right), so the check has to be here, where
 * the two are related.
 *
 * Wrapped in `#ifdef CONFIG_MPSL` because radiant/tests/gate builds THIS FILE
 * with no MPSL at all - the fakes under tests/gate/fakes/ shadow
 * <mpsl_timeslot.h> precisely so the signal sequences are the suite's rather
 * than an arbiter's, and that image has no session to size. Every real build
 * reaches here through CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL, which
 * `select MPSL`, so the guard excludes the fake and nothing else. An undefined
 * symbol expands to nothing rather than to 0, which inside BUILD_ASSERT is a
 * syntax error rather than the readable message below - hence the #ifndef too.
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
 * it covers by the hand-back time. The BUILD_ASSERT on MAX_WINDOW_US further
 * down is what makes the forward reference safe.
 *
 * The EXTEND margin specifically, and deliberately, even though the
 * non-extendable classes are charged the smaller END_MARGIN_FIXED_US: this
 * feeds MAX_WINDOW_US, which is what the scheduler is told a window may be, and
 * that ceiling has to hold for the LONGEST overhead any class can carry.
 * Computing it from the smaller margin would hand the scheduler a ceiling an
 * elastic request then exceeds.
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
 * TWO OF THEM, split by grant class: it is not the same question for both.
 *
 * END_MARGIN_EXTEND_US (2000) is the EXTENDABLE class's. That class
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
 * (stats.overstayed, and the board halts), not a dropped packet.
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
	 * The hardware loan: MPSL has handed us the radio and we have not
	 * given it back. Set at SIGNAL_START, cleared only by timer0_disarm()
	 * (idempotent) - exactly one on_grant_end() per granted timeslot, on
	 * every exit. Every teardown backstop (SESSION_IDLE, SESSION_CLOSED,
	 * gate_shutdown) must key on THIS, not on `granted`, which is the
	 * core's permission and is cleared early by gate_release(): a lost
	 * TIMER0 compare then reaches neither timer0_disarm() nor
	 * on_grant_end(), and the silent cost is that
	 * radio_endpoints_attach(false) - the 802.15.4 driver's write-once
	 * SUBSCRIBE_RXEN restore - never runs and that receiver never ramps
	 * again.
	 */
	volatile bool hw_held;
	/* The core has finished with the grant and wants it given back. */
	volatile bool release_wanted;
	/* This operation is allowed to grow itself - the elastic classes. */
	volatile bool extendable;
	/* This elastic grant was placed while the gate had stopped asking for
	 * extensions (ext_skip), and was sized for that: it ends at its first
	 * compare and never asks, even if the skip lapses while it runs. */
	volatile bool no_extend;
	/*
	 * The hand-back margin THIS grant was reserved with, and the only
	 * figure any compare for it may be placed against. Split by class:
	 * END_MARGIN_EXTEND_US when g.extendable, END_MARGIN_FIXED_US
	 * otherwise. Carried in state rather than re-derived at each site
	 * because the reservation MPSL was given was already sized with it -
	 * a compare placed against the other constant would be arithmetic
	 * about a timeslot that was never requested.
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
	/* Which `return GATE_DENIED` fired - gate_acquire() has six, counted
	 * apart because they need different fixes: `pending` means the core is
	 * re-arming faster than one placement completes; `no_anchor` means the
	 * anchor needs rebootstrapping. */
	uint32_t den_pending;
	uint32_t den_no_anchor;
	uint32_t den_distance;
	uint32_t den_degenerate;
	uint32_t den_no_session;
	/* Refused because MPSL still owed an answer. See g.mpsl_owes. */
	uint32_t den_owed;
	uint32_t extends_ok;
	uint32_t extends_failed;
	/* Elastic grants ended at their first compare without asking, because
	 * the arbiter had refused the last few extensions (ext_skip). */
	uint32_t ext_skipped;
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
 * How long past the operation's own start the backstop waits. It must not be
 * near zero. MPSL answers from mpsl_low_priority_process(), a cooperative
 * thread that a real SoftDevice Controller advertiser can starve within the
 * ~2 ms arm lead a scan chunk is placed on, and a backstop that fires first
 * clears g.pending and denies while MPSL still owns the session (a placed
 * request cannot be withdrawn); the core re-arms immediately;
 * mpsl_timeslot_request() returns -NRF_EAGAIN; a fresh backstop starts;
 * repeat. That is a spiral, not a slow gate - measured at zero (100 ms
 * connectable advertiser, 260 s): acq=7229, half burned in that loop
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
 *
 * A DEDICATED WORK QUEUE (gate_wq below), not k_sys_work_q: these items are on
 * the critical path of a timeslot that has already been reserved -
 * request_work_fn() must reach mpsl_timeslot_request() inside the placement
 * lead - and behind the system queue they wait on unrelated work of unbounded
 * length. It also owns its stack: radiant/Kconfig asks for
 * CONFIG_SYSTEM_WORKQUEUE_STACK_SIZE=4096 and
 * nrf/subsys/net/openthread/Kconfig.defconfig overrides it to 1120 (Kconfig
 * takes the first definition whose condition holds in parse order) on EXACTLY
 * the image that compiles this file - a silent reboot loop, precisely where
 * the deep stack was needed. CONFIG_RADIANT_GATE_WQ_PRIO defaults (-1) to the
 * same priority CONFIG_SYSTEM_WORKQUEUE_PRIORITY does in these images, so this
 * is contention isolation ONLY; raising it needs its own measurement.
 *
 * ─── NO KERNEL CALL MAY BE MADE FROM THIS FILE'S SIGNAL CALLBACK ────────────
 *
 * MPSL's TIMER0/RADIO/RTC0 vectors are connected with IRQ_ZERO_LATENCY
 * (nrf/subsys/mpsl/init/mpsl_init.c:187) whenever CONFIG_ZERO_LATENCY_IRQS is
 * on. A zero-latency interrupt is, by definition, ABOVE the priority that
 * irq_lock() and every Zephyr spinlock mask - that is the entire point of it.
 * So the kernel's own data structures are NOT protected against it, and any
 * kernel API called from on_signal() can land in the middle of another
 * context's critical section. Nordic's own timeslot sample makes no kernel
 * call from its callback either (nrf/samples/mpsl/timeslot/src/main.c: the
 * callback only writes a static return parameter; every k_msgq_put() is in a
 * thread-context wrapper).
 *
 * Nothing may be submitted straight from that callback, whichever queue it
 * lands on, and the deferral has to be a SOFTWARE INTERRUPT rather than a poll
 * thread - a poll thread k_msleep()s onto the same kernel timeout list the
 * race corrupts. It must also cover radiant_event_wakeup() first: the ANT
 * completion path runs in this same callback and ends in a k_sem_give() on a
 * semaphore whose waiter has a timeout, once per received packet, where the
 * gate's own calls fire once per reservation. What is left in this file is the
 * three work submissions and the deadline timer, all routed through
 * radiant_swi_pend() so the kernel call happens one interrupt priority down,
 * where irq_lock() reaches it. See radiant/include/radiant/radiant_swi.h for
 * the mechanism and docs/p4-coexistence-bugs.md for the measurements and the
 * deferral arm that was measured worse and thrown away.
 * ---------------------------------------------------------------------------
 */

static void request_work_fn(struct k_work *w);
static void denied_work_fn(struct k_work *w);

static K_WORK_DEFINE(request_work, request_work_fn);
static K_WORK_DEFINE(denied_work, denied_work_fn);

/*
 * The queue itself. Started once per boot by gate_init() and never stopped -
 * see gate_wq_started there. Cost is CONFIG_RADIANT_GATE_WQ_STACK_SIZE (4096
 * by default) of RAM in any image that compiles this file.
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
 * What the next grant will be read with, held apart from the current one. An
 * acquire constantly happens during a live grant (a tracked window's receive
 * callback commonly arms the next slot mid-window), and writing these four
 * straight into `g` would retune the running grant's own end/extension
 * arithmetic for a window not yet placed. So they are staged here and
 * committed by request_work_fn() once mpsl_timeslot_request() accepts.
 */
static struct {
	uint32_t       want_len_us;
	uint32_t       granted_len_us;
	radiant_time_t grant_end;
	bool           extendable;
	bool           no_extend;
	/* The margin this request's length was built with. See g.end_margin_us
	 * - it has to travel with the request, not be re-derived later. */
	uint32_t       end_margin_us;
	/* Bench probe only: which class this request belongs to, so a BLOCKED
	 * or a START can be attributed to it. */
	bool           high_prio;
} next_grant;

/* The class of the request MPSL currently holds, for the probe counters. */
static bool grant_high_prio;
/* The class of the request most recently PLACED. BLOCKED/CANCELLED answer the
 * placed request, and may arrive from inside mpsl_timeslot_request() - before
 * commit_next_grant() has made it the current one - so reading g.extendable /
 * grant_high_prio there attributed the first refusal after every class change
 * to the previous grant's class (review 2026-10-31, C5-1). Only one request is
 * ever outstanding, so recording at placement is right for inline and
 * deferred answers alike. */
static bool placed_extendable;
static bool placed_high_prio;

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
 * the schedule) makes every retry land on the same phase. So a refused
 * elastic request shifts phase - a deterministic odd increment, not a PRNG,
 * since it needs to be uncorrelated with 100 ms rather than unpredictable;
 * 2137 us is prime (visits every offset before repeating) and the span is a
 * bit over one ANT+ slot so the sweep's own coverage isn't distorted. Only
 * the elastic class may move: a tracked window's instant belongs to the
 * master. Advanced but NOT applied to a placement - see gate_acquire().
 */
#define ELASTIC_SKEW_STEP_US 2137u
#define ELASTIC_SKEW_SPAN_US 13000u
static uint32_t elastic_skew_us;

/* Consecutive refusals before the anchor is abandoned; see the BLOCKED case. */
#define BLOCKED_RUN_REANCHOR 4u
static uint32_t blocked_run;

/*
 * STOP ASKING FOR EXTENSIONS THE ARBITER KEEPS REFUSING (ADR 0021, decision 3).
 * Beside OpenThread every extension is refused, and every refusal is an
 * EXTEND_FAILED handled inside the zero-latency callback. MPSL ASSERT 69:108 has
 * only ever followed one of those. So after EXT_FAIL_RUN_OFF refusals in a row
 * the next EXT_SKIP_GRANTS elastic grants end at their first compare with the
 * ABORTED terminal a refusal would have produced, without the request; then one
 * is asked for again. Under BLE extensions are mostly granted, so a run is rare.
 *
 * A grant placed while skipping cannot extend, so it is sized like one that
 * never could: END_MARGIN_FIXED_US instead of END_MARGIN_EXTEND_US. That margin
 * has to hold a radio abort as well as the signal path here, because the window
 * is still open at the compare. The class is decided per grant (g.no_extend),
 * so the skip lapsing while a grant runs cannot make it ask from a margin
 * MPSL's own extension minimum was never budgeted into.
 */
#define EXT_FAIL_RUN_OFF 3u
#define EXT_SKIP_GRANTS  64u
static uint32_t ext_fail_run;
static uint32_t ext_skip;

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
	g.no_extend      = next_grant.no_extend;
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
	placed_extendable = next_grant.extendable;
	placed_high_prio = next_grant.high_prio;
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
 * chained requests). Callable from any context; the MPSL call itself is
 * deferred like every other one.
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
	next_grant.no_extend = false;
	next_grant.high_prio = false;
	/* The conservative margin, not the class-derived one: this slot is handed
	 * straight back from SIGNAL_START and never places a compare against it,
	 * and both margins are longer than the 100 us slot anyway, so every
	 * compare site would fall to its own short-grant fallback. */
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
 * A window completing inside its grant can arm the next window (staging an
 * operation, placing a request) before this callback returns, so a denial
 * submitted from here must not be unconditional: it would hit that
 * freshly-armed operation instead of an orphan. g.pending is the test for
 * "is a staged operation actually orphaned" - made in denied_work_fn, not
 * here, since the answer can change between submit and run (see there).
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
		if (g.no_extend && ext_skip > 0u) {
			ext_skip--;
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
			/* Either the operation completed synchronously or nothing
			 * was staged for this grant, and the backend released it.
			 * Give the timeslot back now and let the work queue tell
			 * the core. A programming failure (ETIME from the on-grant
			 * program_rx()) does NOT come through here: it only sets
			 * radiant_op.gate_failed, the grant is held to its TIMER0
			 * end, and end_housekeeping() reports FAILED after that. */
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
		 * g.end_margin_us, not TAIL_MARGIN_US (250 us) - the latter
		 * only covers what the RADIO needs and is not enough time for
		 * the MPSL interrupt/dispatch/callback between the compare
		 * firing and ACTION_END returning (measured: at len-250 the
		 * board asserted on the first or second timeslot; with a margin
		 * well inside the grant it ran 1007 timeslots clean).
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
		/*
		 * A STALE COMPARE ENDS THE GRANT 14 us AFTER IT STARTS. MPSL
		 * restarts MPSL_TIMER0 from zero at every timeslot, but CC0
		 * still holds whatever the LAST grant left in it until the
		 * cc_set above rewrites it - and that rewrite happens ~31 us
		 * into the timeslot, after this callback has programmed the
		 * whole receive window. Anything the counter passes in those
		 * 31 us latches EVENTS_COMPARE0, and a latched compare event
		 * means nrf_timer_int_enable() below raises SIGNAL_TIMER0
		 * immediately, ending the timeslot. It is self-sustaining, so
		 * it presents as a steady state rather than as jitter:
		 * gate_release() sets CC0 to the ENDING grant's own count plus
		 * a small lead, so a grant that ended at 14 us leaves CC0 at
		 * about 20 and the next one walks past it before the rewrite.
		 * Measured on P4 MED, 60 s beside an OpenThread leader: stale
		 * compare found set at 291 of 317 grants; 214 of 216 tracked
		 * windows given a grant that lasted under a millisecond (median
		 * 14 us) against a window due to open 231 us in; 2 of 216
		 * windows ever ramping the receiver; 14 packets of 239.
		 *
		 * The clear belongs BETWEEN the cc_set and the int_enable, and
		 * only there: before the cc_set it would be clearing an event
		 * whose compare is still the old value, and after the
		 * int_enable it is already too late. gate_release() arms the
		 * same compare and keeps the same three-step ordering.
		 */
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
		nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
		break;

	case MPSL_TIMESLOT_SIGNAL_RADIO:
		/*
		 * Inside a timeslot the RADIO interrupt is MPSL's: it owns
		 * RADIO_0_IRQn for the grant and delivers it as this signal
		 * instead, so radiant_radio_nrf.c's handler is never entered
		 * and a window would run its full length and never end (which
		 * presents as every window being denied later by this file).
		 *
		 * The handler is re-entered by hand here; if it finished the
		 * operation, the grant is given back from here instead of
		 * waiting for TIMER0. High-priority signal, so ACTION_END is
		 * legal.
		 */
		stats.sig_radio++;
		radiant_nrf_gate_on_radio_irq();
		if (g.release_wanted && !g.granted) {
			timer0_disarm();
			end_housekeeping();
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_TIMER0:
		stats.sig_timer0++;
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
		if (g.no_extend && !g.release_wanted && g.extendable &&
		    g.granted_len_us < g.want_len_us) {
			/* As EXTEND_FAILED below, without asking. See ext_skip. */
			stats.ext_skipped++;
			g.granted = false;
			timer0_disarm();
			(void)radiant_radio_abort();
			end_housekeeping();
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		}
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
			 * The ceiling here is the TIMESLOT's, not the
			 * window's: MAX_WINDOW_US is the scheduler's ceiling
			 * with margins already subtracted, and a timeslot
			 * covering a window that long is necessarily longer
			 * than it, since head/tail/end margins are inside the
			 * grant but not the window. Using it stopped the chain
			 * 2500 us short, ending the grant before the window's
			 * own close with the operation still armed (measured:
			 * chunks=10 deny=9). want_len_us is already bounded
			 * against MPSL's maximum in gate_acquire(), so this is
			 * a backstop.
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
		 * The compare advances by exactly one step per extension, so
		 * g.end_margin_us of runway always stays ahead of it.
		 * TAIL_MARGIN_US here instead would leave an extended grant's
		 * compare only 250 us before the new end and MPSL asserts on
		 * the overstay (measured: ext=1204/0 asserting at placed=14) -
		 * these are the two quantities the file elsewhere warns must
		 * not be conflated.
		 *
		 * g.end_margin_us is necessarily END_MARGIN_EXTEND_US on this
		 * path - only an extendable grant can be extended - and it is
		 * read from state rather than named directly so this line
		 * cannot drift from the one that armed the first compare.
		 */
		stats.extends_ok++;
		ext_fail_run = 0u;
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
		 */
		stats.extends_failed++;
		if (++ext_fail_run >= EXT_FAIL_RUN_OFF) {
			ext_fail_run = 0u;
			ext_skip = EXT_SKIP_GRANTS;
		}
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
			if (placed_high_prio) {
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
			timer0_disarm();
		}
		/* Answered, and refused. See g.mpsl_owes. */
		g.mpsl_owes = false;
		/*
		 * GIVE WAY BY ASKING FOR LESS, not merely by asking again. See
		 * elastic_initial_us. Only the elastic class is shortened, and
		 * the placed request's extendable flag is exactly that class -
		 * it says this request is allowed to grow, which is the same
		 * thing as saying its length was ours to choose. The PLACED
		 * request's, not g.extendable: see placed_extendable.
		 */
		if (placed_extendable) {
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
		 * is cleared here, backstopping the END paths that never
		 * signal. Without it the gate stalls after exactly one grant:
		 * a timeslot MPSL ends by its own length expiry signals
		 * nothing we handle, so g.granted would stay true and every
		 * later gate_acquire() would take the "grant already held"
		 * branch against a long-dead grant_end and return GATE_DENIED
		 * forever.
		 *
		 * Test is g.hw_held, not g.granted: gate_release() clears
		 * g.granted before its compare fires, so a lost compare would
		 * otherwise arrive here with g.granted already false and the
		 * radio still on loan. hw_held is exactly "radio out and not
		 * yet returned".
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

#if defined(CONFIG_RADIANT_GATE_MPSL_STATS_LOG)
/*
 * Bench only: the counters a soak is scored on, every 5 s, from the gate's own
 * work queue - never from inside a grant. Cumulative since boot.
 */
static void gate_stats_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(gate_stats_work, gate_stats_fn);

static void gate_stats_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	LOG_INF("GATESTAT pl=%u gr=%u bl=%u ext=%u/%u skip=%u near=%u eag=%u "
		"dl=%u hi %u/%u/%u/%u",
		stats.placed, stats.granted, stats.blocked, stats.extends_ok,
		stats.extends_failed, stats.ext_skipped, stats.refused_too_near,
		stats.refused_eagain, stats.deadline_fired, stats.hi_acq,
		stats.hi_pending, stats.hi_blocked, stats.hi_granted);
	(void)k_work_reschedule_for_queue(&gate_wq, &gate_stats_work, K_SECONDS(5));
}
#endif

int gate_init(void)
{
	int32_t rc;

	if (session_open) {
		return RADIANT_RADIO_OK_RC;
	}
	memset(&g, 0, sizeof(g));
	memset(&stats, 0, sizeof(stats));
	ext_fail_run = 0u;
	ext_skip = 0u;
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
	 * The queue before the trampoline, and the trampoline before the
	 * session: the moment a session is open a grant can arrive, every
	 * deferral from inside one goes through the trampoline, and the
	 * trampoline's only job is to submit to this queue.
	 *
	 * Both ONCE PER BOOT, not once per gate_init(), and neither is ever
	 * given back. A suspend/resume cycle (gate_shutdown() then gate_init(),
	 * which is also how every test in radiant/tests/gate starts) would
	 * otherwise add a subscriber entry each time until the table is full on
	 * the fourth (measured as `gate: radiant_swi_register: -12`, and a gate
	 * that then refuses to initialise at all), and k_work_queue_start()
	 * asserts outright on a queue already carrying K_WORK_QUEUE_STARTED.
	 * Nothing needs stopping: the handler is a file static that is always
	 * valid, an idle queue thread costs only its stack, and draining it at
	 * shutdown would mean blocking here on work that calls back into the
	 * core.
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
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS_LOG)
	(void)k_work_reschedule_for_queue(&gate_wq, &gate_stats_work, K_SECONDS(5));
#endif
	/* Spend one minimum-length EARLIEST timeslot on nothing but its start
	 * time, so that every window the core asks for is a NORMAL request that
	 * lands where it was asked for. */
	bootstrap_anchor();
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
	 * and must not be held past it. It has to be decided here because `len`
	 * is the request MPSL is handed and depends on the class: a
	 * non-extendable grant is charged the shorter END_MARGIN_FIXED_US.
	 *
	 * Locals, staged into next_grant further down, so that the early
	 * refusals in between still leave next_grant untouched - it describes
	 * the NEXT grant and is committed only when MPSL accepts.
	 */
	const bool     extendable = (op != GATE_OP_TX) &&
				(prio == RADIANT_GATE_PRIO_NORMAL);
	/* An elastic grant placed during a skip will never ask to grow. */
	const bool     no_extend = extendable && (ext_skip > 0u);
	const uint32_t end_margin_us = (extendable && !no_extend)
					       ? END_MARGIN_EXTEND_US
					       : END_MARGIN_FIXED_US;

	if (!session_open) {
		stats.den_no_session++;
		return GATE_DENIED;
	}
	now = radiant_radio_now();

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
		 * Not fitting is not the same as being refused. The common
		 * case here is a tracked window's next slot arming a quarter
		 * second out mid-window (radiant_radio_hal.h's low-jitter
		 * callback contract) while the current timeslot is still
		 * live: refusing it because 250 ms does not fit inside a
		 * 5.5 ms grant lost the slot every other window, measured at
		 * 50.6% loss (exact) with blocked=0 the whole time.
		 *
		 * So a request starting after this grant ends falls through
		 * and is placed like any ordinary future reservation. A
		 * genuine ~1.56 ms-out reply still gets its synchronous
		 * refusal from the too-near test below. The counter means
		 * "arms placed rather than served from the reserve".
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
	 * NO PHASE SKEW HERE. `start`/`end` describe the TIMESLOT, while the
	 * operation staged behind it keeps the absolute t_open/t_close the core
	 * chose - so adding elastic_skew_us to both (as this function once did)
	 * buys a grant that begins after the window it was meant to cover was
	 * already due to open, and program_rx() then finds req->t_open in the
	 * past and refuses RADIANT_RADIO_ETIME: the right answer to the wrong
	 * question, since it is the reservation that was misplaced, not the
	 * window that was late. Measured with the skew walking to 11401 us:
	 * grant=1236, prog=127 ok against 689 ETIME, 642 chunks FAILED, one
	 * CRC-good frame in 60 s, every refusal feeding the skew one more step.
	 *
	 * The reservation MUST cover the operation - that is the whole contract
	 * of this function - so a gate cannot decorrelate itself by sliding one
	 * and not the other, and shifting the operation too is not this layer's
	 * to do (the core owns those instants). The phase problem has a
	 * mechanism that does not lie about placement: BLOCKED_RUN_REANCHOR
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
	/* length_us is written BELOW, after the elastic clamp - see there. */

	/* Staged, not written through `g` - this function can run during a
	 * live grant, and these describe the NEXT one. See next_grant and
	 * commit_next_grant(). */
	next_grant.want_len_us = (uint32_t)len;
	next_grant.grant_end = end;
	/* Decided at the top of the function, because `len` above depends on
	 * it. See the declarations there. */
	next_grant.extendable = extendable;
	next_grant.no_extend = no_extend;
	/* The margin travels with the request built from it: every compare for
	 * this grant is placed against g.end_margin_us, which is this value once
	 * commit_next_grant() runs. */
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
	 * THE LENGTH ASSIGNMENT MUST BE THE LAST WORD, BELOW EVERY CLAMP, and it
	 * reads next_grant.granted_len_us rather than `len` so the request and
	 * this file's own bookkeeping can never disagree - what we ask for IS what
	 * SIGNAL_START will measure the grant against.
	 *
	 * Above the clamp, `len` is the whole window plus margins - for a scan
	 * chunk, MAX_WINDOW_US + 2500 = 96700 us. Assigning there shortened only
	 * next_grant.granted_len_us while the request MPSL was handed still asked
	 * for the whole 96.7 ms, so every mechanism written to make the elastic
	 * class give way (elastic_initial_us halving on BLOCKED down to
	 * ELASTIC_FLOOR_US, growing back one EXTEND_STEP_US per grant) moved a
	 * number that was never in the request, and ADR 0013's "ask small and
	 * grow" was dead code beside any contending stack: a grant that large is
	 * never given, so SIGNAL_START never arrives to grow it. Measured on
	 * P4 MED, 600 s: placed=57481 granted=11517 blocked=45984, every block
	 * answered inline from inside mpsl_timeslot_request() (inl=45984), all
	 * 11517 grants being 100 us bootstrap anchors (len=100/100), and
	 * radiant_nrf_gate_on_grant() entered zero times.
	 */
	req.params.normal.length_us = next_grant.granted_len_us;

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

bool gate_held(void)
{
	/* g.granted, not g.hw_held: the core's permission is what an in-grant
	 * arm needs, and gate_release() clears it the moment the air is handed
	 * back even while the compare that ends the timeslot is still due. */
	return g.granted;
}

void gate_forget_pending(void)
{
	if (g.pending) {
		g.pending = false;
		deadline_stop();
	}
}

static void gate_release_masked(void);

/*
 * A ZLI mask, not irq_lock(): the grant's own TIMER0 signal is a zero-latency
 * interrupt, which irq_lock() does not mask. Since THE GRANT HANDOFF
 * (radiant_radio_nrf.c) the usual release runs from the trampoline rather than
 * from inside a signal, and a TIMER0 end landing between the g.granted test
 * and the CC0 write below would end the timeslot and then have this re-arm
 * COMPARE0 on a timer MPSL owns again - the interrupt that re-enters forever
 * (see timer0_disarm()). Or hand the radio back twice. A few register writes;
 * the end margin is there to absorb this much signal latency.
 */
void gate_release(void)
{
	uint32_t key = radiant_swi_zli_lock();

	gate_release_masked();
	radiant_swi_zli_unlock(key);
}

static void gate_release_masked(void)
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
	 *
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
	       "ext=%u/%u skip=%u deadline=%u\n"
	       "    over=%u invalid=%u unknown=%u last_sig=%u "
	       "len=%u/%u granted=%d pending=%d rel=%d ext_ok=%d\n",
	       file, line, stats.placed, stats.granted, stats.blocked,
	       stats.extends_ok, stats.extends_failed, stats.ext_skipped,
	       stats.deadline_fired,
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
	/*
	 * Reboot on a shipping image, halt on a bench one - the same split
	 * apps/common/diag_flash_log.c makes. Before this, every image halted
	 * here forever whatever CONFIG_RESET_ON_FATAL_ERROR said, because
	 * k_fatal_halt() never reaches the fatal hook that honours it. The dump
	 * above is already out: LOG_PANIC() made it synchronous.
	 *
	 * Not k_panic(): this handler can run in MPSL's priority-0 zero-latency
	 * signal context, where an SVC escalates to a HardFault. sys_reboot() is
	 * irq_lock, arch_zli_lock, sys_clock_disable() and NVIC_SystemReset, with
	 * no SVC anywhere on that path. On nRF54L sys_clock_disable() is the
	 * GRTC's: nrfx_grtc_uninit() plus an LFCLK onoff_release(), whose only
	 * lock is a k_spinlock - BASEPRI, not a kernel call - so it is safe from
	 * there too.
	 */
#if defined(CONFIG_RESET_ON_FATAL_ERROR)
	sys_reboot(SYS_REBOOT_COLD);
#else
	k_fatal_halt(K_ERR_KERNEL_PANIC);
#endif
}
#endif







