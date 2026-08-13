/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf_gate_mpsl.c - the RADIO, borrowed from MPSL a slot at a
 * time.
 *
 * Provenance: clean-room. Written against nrfxlib's mpsl_timeslot.h, the
 * in-tree nrf/samples/mpsl/timeslot and nrf/drivers/mpsl/flash_sync patterns,
 * and the measurements in radiant_core/spike/mpsl_arb. Nothing here derives
 * from sdk-ant or from libant.a.
 *
 * ===========================================================================
 * IT WORKS. ANT+ IS RECEIVED THROUGH MPSL TIMESLOT ARBITRATION.
 * ===========================================================================
 *
 * Measured on the nRF54L15 DK, 2026-08-12, against a real sensor in the room,
 * with the direct build measured on the same bench in the same session:
 *
 *     MPSL gate   DATA=30 in 60 s
 *     direct      DATA=30 in 60 s
 *
 * The same packets. Over that minute the gate placed 273 reservations and was
 * granted 272 - one outstanding at the sample - with 2716 extensions and not
 * one refusal, blocked=0, and the scheduler reporting end ok=34 abrt=0 miss=0
 * fail=0 deny=0. A channel was found, tracked and held.
 *
 * THE ONE STRUCTURAL DIFFERENCE, and it is by design: a scan chunk is 94 200 us
 * here against 260 000 us on the direct build, because caps.max_window_us bounds
 * a window to what MPSL can grant. The sweep therefore takes about three times
 * as many chunks to cover the same dwell. Sweep RATE under arbitration is a
 * measurement this file has not yet taken; packet yield is, and it matches.
 *
 * READ THE SEVEN NUMBERED BUGS BELOW BEFORE CHANGING ANYTHING. Every one of
 * them presented as "the dongle hears nothing", and four of them presented as
 * an MPSL assert with no console output at all.
 *
 * ---------------------------------------------------------------------------
 * THE TWO INSTRUMENTS THAT ANSWERED EVERYTHING, AND THEY ARE STILL HERE
 * ---------------------------------------------------------------------------
 *
 * MPSL asserts by calling mpsl_assert_handle() with a file ID and a line - the
 * release build ships no file name - and then k_fatal_halt(), after which the
 * CPU spins in arch_system_halt() for ever with nothing on the console. Two
 * additions turn that dead end into a diagnosis, and both are cheap enough to
 * keep for good:
 *
 *   THE ASSERT HANDLER TALKS. LOG_PANIC() before its printk, or the message
 *   dies with the thread that would have flushed it. It prints the counters,
 *   the flags, and - decisively - which of OVERSTAYED and INVALID_RETURN
 *   arrived, because those need opposite fixes and the assert location cannot
 *   tell them apart.
 *
 *   THE SIGNAL RING BUFFER. Sixteen bytes of "which signal, what did we return"
 *   printed by the assert handler. It cost one array write per signal and it
 *   named bug 14 outright: the trace read `1:2 2:2`, TIMER0 ending the timeslot
 *   and then the RADIO signal ending it again.
 *
 *   AND, FOR A STALL RATHER THAN A CRASH: gate_dump_fn(), a once-a-second dump
 *   under CONFIG_RADIANT_CORE_SWEEP_DEBUG. Every other diagnostic here is
 *   emitted BY an event, so a gate that has stopped producing events produces
 *   no diagnostics either - and "the last line says placed=2" is then
 *   indistinguishable from "it placed two and is still working".
 *
 * HOW A SILENT HALT IS READ AT ALL, if it ever goes quiet again: halt the CPU
 * over J-Link with a script that does NOT reset - `h` then `regs`, no `r`, or
 * the state is destroyed before it can be read. That put PC inside
 * arch_system_halt() with IPSR = 0x1A, a panic raised from an external IRQ
 * rather than from a fault, which identified the assert handler before the
 * assert handler could speak for itself.
 *
 *     JLink.exe -NoGui 1 -SelectEmuBySN <sn> -CommanderScript <file>
 *     exec DisableAutoUpdateFW / si SWD / speed 4000 /
 *     device nRF54L15_M33 / connect / h / regs / q
 *
 * AND THE REFERENCE THAT SEPARATES "THIS FILE IS WRONG" FROM "MPSL DOES NOT DO
 * THAT HERE": nrf/samples/mpsl/timeslot, built for this exact board and SDK,
 * gets a Timer0 signal on every one of its timeslots - 12 for 12, no assert -
 * about 166 us after SIGNAL_START. Building it is the right first move on any
 * future "does MPSL even work here" question, and it settled one already.
 *
 * ---------------------------------------------------------------------------
 * THE BUG THAT COST THE MOST, AND THE INSTRUMENT THAT FOUND IT
 * ---------------------------------------------------------------------------
 *
 * For several rounds this looked like a wedge: the console stopped mid-line
 * and the dongle answered nothing, with no fault message. It was not a wedge.
 * It was a STACK OVERFLOW IN THE radiant_event THREAD, resetting the board
 * several times a second.
 *
 * It was invisible because of two things compounding:
 *
 *   CONFIG_LOG_MODE_DEFERRED is the default, so the fatal-error message needs
 *   a thread that has just died. The console simply stops.
 *
 *   CONFIG_LOG_PRINTK=y is ALSO the default, which routes printk THROUGH the
 *   log subsystem. So the "synchronous" printk instrumentation added to find
 *   the problem was itself deferred and dropped, and its absence was read as
 *   "this function is never called" - which sent two rounds of debugging after
 *   a mechanism that did not exist.
 *
 * CONFIG_LOG_MODE_IMMEDIATE=y and CONFIG_THREAD_NAME=y together produced the
 * answer in one run: "Stack overflow on CPU 0 / Current thread: radiant_event".
 * Reach for those FIRST on this build; nothing else here is trustworthy while
 * the log can be dropped.
 *
 * (The reported faulting PC is worthless in this fault - 0x13198 resolved to
 * `lbu_output`, a rodata symbol - because the dump also says "context area not
 * valid". The thread name is the only reliable field.)
 *
 * THREE OTHER STACKS WERE RAISED FIRST on plausible reasoning and none was the
 * one: ISR_STACK_SIZE, SYSTEM_WORKQUEUE_STACK_SIZE and MPSL_WORK_STACK_SIZE.
 * They are still raised in radiant_core/Kconfig, because the reasoning for
 * each remains sound and the margins are cheap - but the fix was
 * API_EVENT_STACK_SIZE in radiant_api.c.
 *
 * ---------------------------------------------------------------------------
 * FOURTEEN BUGS FOUND AND FIXED, each documented at its own site
 * ---------------------------------------------------------------------------
 *
 * THE LAST FOUR ARE THE ONES THAT MADE IT WORK, and none of them is about
 * arbitration at all - they are all about the seam between an arm and the air:
 *
 *  12. THE RADIO INTERRUPT IS NOT OURS INSIDE A TIMESLOT. MPSL owns the vector
 *      and delivers it as MPSL_TIMESLOT_SIGNAL_RADIO; the handler
 *      radiant_radio_nrf.c connected is never entered. Windows ran their full
 *      length and never ended. This is the bug that made the gate deaf, and it
 *      is invisible in the arm path.
 *  13. THE END-OF-GRANT DENIAL MUST NOT KILL THE OPERATION ARMED MICROSECONDS
 *      EARLIER. Once 12 was fixed, a completing window arms the next one from
 *      inside the same callback - so the denial queued when the grant ended
 *      arrived to find a perfectly healthy operation waiting for a grant that
 *      was already on its way. The test has to be made when the work RUNS.
 *  14. ACTION_END MAY BE RETURNED ONCE. TIMER0 and RADIO race at the end of a
 *      window and either can win; whichever loses used to end an already-ended
 *      timeslot. One guard at the single return statement, not per branch.
 *  15. AND ONE IN radiant_radio_nrf.c, WHICH IS THE DEEPEST OF THEM:
 *      terminal_sent was cleared in program_rx(), not at the arm - so an
 *      operation waiting for air inherited the previous operation's `true` and
 *      could not be terminated at all. deliver_terminal() refused it silently,
 *      which is precisely when a denial most needs to be delivered. Under the
 *      direct gate the two instants are the same and nothing changes; under
 *      this one they are milliseconds apart and the whole denial mechanism -
 *      all of P1 - lived in that gap. See CLAIM_TERMINAL().
 *
 *   1. The advertised arm lead must be ARM_LEAD_US + the gate's placement
 *      lead - additive, not a maximum.
 *   2. The gate's too-near test must use a smaller floor than the advertised
 *      lead, or it can never pass. The trap radiant_radio_nrf.c already
 *      documents for min_arm_lead_us, reproduced exactly.
 *   3. MAX_WINDOW_US must reserve everything added to a window AFTER the
 *      scheduler caps it. A scan chunk was arriving 184 us over MPSL's
 *      ceiling and every arm was refused for it.
 *   4. g.granted must be cleared in gate_release(), not at ACTION_END.
 *   5. The MPSL_TIMER0 compare must be disarmed on every path that ends a
 *      grant - an interrupt left armed on a peripheral that is no longer ours,
 *      with an event nothing will clear.
 *   6. Every MPSL call and every call into the core must be deferred to a work
 *      queue: arming reaches this file from the RADIO interrupt INSIDE a
 *      timeslot, where mpsl_timeslot_request() is not callable.
 *   7. The event thread's stack, above.
 *
 * And one in radiant_api.c: a denial must not wake the event thread WHEN THE
 * SEARCH SLOT WAS KEPT, or a gate refusing everything spins at 27 000 denials
 * a second. Applied to every denial instead, a denied master transmit stopped
 * being re-posted and became a missed slot - which the api suite caught.
 *
 * ---------------------------------------------------------------------------
 * HOW TO REPRODUCE THE MEASUREMENT
 * ---------------------------------------------------------------------------
 *
 *   west build -b nrf54l15dk/nrf54l15/cpuapp -- -DANT_RADIO=core \
 *       -DRADIANT_BACKEND=nrf -DCONFIG_RADIANT_CORE_BACKEND_NRF_GATE_MPSL=y \
 *       -DCONFIG_MPSL=y -DCONFIG_MPSL_TIMESLOT_SESSION_COUNT=1 \
 *       -DCONFIG_RADIANT_CORE_SWEEP_DEBUG=y
 *
 * CONFIG_MPSL_TIMESLOT_SESSION_COUNT DEFAULTS TO 0, which sizes the session
 * context array to nothing and makes mpsl_timeslot_session_open() fail at
 * runtime rather than at build. gate_init() logs its rc for exactly that
 * reason.
 *
 * DEFAULT (DEFERRED) LOGGING, NOT CONFIG_LOG_MODE_IMMEDIATE. See the warning
 * below: immediate mode turns every diagnostic in this file into a
 * multi-millisecond block inside a grant, and several rounds of "that fix
 * changed nothing" were read through it.
 *
 * RESET THE BOARD BEFORE EVERY RUN. An assert halts the CPU, so a board left
 * from the previous run answers nothing - which is a different failure wearing
 * the same clothes and cost a round to notice.
 *
 * ---------------------------------------------------------------------------
 * ⚠ AND A WARNING ABOUT THE INSTRUMENT ITSELF
 * ---------------------------------------------------------------------------
 *
 * With CONFIG_LOG_MODE_IMMEDIATE=y a printk BLOCKS for roughly 87 us per
 * character on a 115200 console. A sixty-character trace line inside
 * gate_acquire() is therefore about five milliseconds of synchronous blocking
 * in the arm path - against the ~3 ms of lead the request was built with. THE
 * INSTRUMENT MOVES THE THING IT MEASURES, and every "the fix did not change
 * the result" observation above was taken through it.
 *
 * A trace taken through immediate logging is trustworthy about SEQUENCE - which
 * signals arrive and in what order - and NOT about TIMING. That is why the two
 * instruments this file keeps are a ring buffer and a deferred LOG_INF on its
 * own thread: neither is in the grant path at all. Do not put a printk back
 * into gate_acquire() or into on_signal().
 *
 * ONE MORE TRAP OF THE SAME FAMILY, because it cost two rounds and would cost
 * them again: CONFIG_LOG_PRINTK=y is the DEFAULT, so printk is routed through
 * the log subsystem - and in deferred mode a printk from a thread that is about
 * to die is simply dropped. "This log line never appears" means DROPPED, not
 * NEVER EXECUTED. Reading it the other way sent two rounds of debugging after a
 * mechanism that did not exist.
 *
 * CONFIG_RADIANT_CORE_BACKEND_NRF_GATE_MPSL is off by default, and the direct
 * build is measured green beside every result above: core ztest 28 suites, api
 * ztest 22, host tools 587, and DATA=30 in the same 60 s on the same bench.
 * The one change this work made to shipping code is CLAIM_TERMINAL() in
 * radiant_radio_nrf.c, which moves an assignment from the programming half of
 * an arm to the claiming half - the same instant on a direct build.
 *
 * ---------------------------------------------------------------------------
 * EVERY CONSTANT IN THIS FILE IS DERIVED FROM A MEASUREMENT, AND THE
 * MEASUREMENT IS NAMED
 * ---------------------------------------------------------------------------
 *
 * The plan this implements says of the tail margin: "This is the most likely
 * field failure of the whole design: a dongle that asserts once an hour under a
 * busy Thread network. Derive it; do not guess." That applies to all of them,
 * so each one below carries the number it came from and where that number was
 * taken. radiant_core/spike/mpsl_arb/README.md has the raw results; the four
 * that shape this file are:
 *
 *   GRANT LATENCY (EARLIEST)   1692-1695 us, 3 us spread over 100 samples.
 *                              It is the HFXO startup and essentially nothing
 *                              else. A BLE advertiser leaves the median alone
 *                              and adds a tail: 1 in 100 at 2760 us.
 *
 *   ANCHOR ERROR               0-17 us over 640 chained NORMAL requests, and
 *                              NOT cumulative. MPSL_TIMESLOT_START_JITTER_US
 *                              is 0 on this part and the measurement agrees.
 *
 *   BLOCKED TIMING             17 blocks across both loads, NOT ONE OF THEM
 *                              LATE. Every one arrived 11-31 ms BEFORE the
 *                              start it referred to. See the deadline timer
 *                              below - this is the finding that made it a
 *                              backstop instead of the load-bearing component
 *                              the plan expected.
 *
 *   FREE TIMER CONTINUITY      -99 ppm across 321 timeslot edges, PROVIDED the
 *                              application holds its own HFXO request. Without
 *                              it the 1 MHz timebase runs on the internal RC
 *                              between grants: 0.42 % fast, about a
 *                              millisecond of slot-placement error per ANT+
 *                              period, silently. radiant_radio_nrf.c's
 *                              clock_control_on() at init is what holds it, and
 *                              it MUST NOT be removed on this build.
 *
 * ---------------------------------------------------------------------------
 * THE SHAPE, AND THE THREE RULES THAT DICTATE IT
 * ---------------------------------------------------------------------------
 *
 * 1. EXACTLY ONE REQUEST OUTSTANDING PER SESSION. mpsl_timeslot_request()
 *    answers -NRF_EAGAIN unless the session is IDLE. That is the same
 *    constraint radiant_radio_hal.h states as "at most one operation exists at
 *    a time", so the mapping is 1:1 and there is no queue to invent.
 *
 * 2. THE FIRST REQUEST MUST BE EARLIEST, AND EARLIEST IS FORBIDDEN FROM INSIDE
 *    A TIMESLOT. So a session needs an explicit bootstrap from thread context
 *    before any real work, and every later request is NORMAL, measured from the
 *    previous timeslot's START rather than from an absolute time.
 *
 * 3. RETURNING ANYTHING BUT ACTION_NONE FROM A LOW-PRIORITY SIGNAL ASSERTS.
 *    BLOCKED, CANCELLED, SESSION_IDLE and SESSION_CLOSED all run in
 *    mpsl_low_priority_process(). Nothing in this file requests from one of
 *    them; recovery is the core's, through the DENIED terminal.
 *
 * ---------------------------------------------------------------------------
 * HOW A TIMESLOT ENDS, WHICH IS THE ONE GENUINELY AWKWARD PART
 * ---------------------------------------------------------------------------
 *
 * A timeslot can only be ended by returning ACTION_END from a signal callback,
 * and our operations end in the RADIO interrupt - which is not a signal
 * callback. The in-tree answer (esb.c, flash_sync_mpsl.c) is to keep a
 * MPSL_TIMER0 compare live and provoke it: gate_release() writes a compare a
 * few microseconds ahead, MPSL delivers SIGNAL_TIMER0, and ACTION_END is
 * returned from there.
 *
 * MPSL_TIMER0 AND NOT A PRIVATE TIMER, and esb.c enforces the same thing with
 * a BUILD_ASSERT. The timeslot's own clock is what MPSL measures the overstay
 * against; a private TIMER runs off HFCLK and would be measuring something
 * else. radiant_core's own 1 MHz TIMER stays exactly where it was and is not
 * touched here - it is the absolute timebase, not the timeslot clock, and
 * conflating the two is how a backend ends up asserting on a drift it cannot
 * see.
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
 * THE HEAD. How far before the caller's t_from the grant must start.
 *
 * radiant_radio_nrf.c cannot touch the compare or the (D)PPI until the grant
 * has started - a connection armed outside a grant fires TASKS_RXEN inside
 * another stack's event - so everything it does at the head of an operation
 * has to fit inside the grant. That is its own ARM_SETUP + RAMP_UP +
 * AIR_LEAD_WORST, which the backend already computes as ARM_LEAD_US: 168 us on
 * nRF54L, 328 us on nRF52.
 *
 * Not re-derived here. The backend's t_from is ALREADY the instant the
 * hardware must be programmed by - it subtracted the ramp-up and the address
 * airtime itself before calling - so what this adds is only the software
 * setup, plus a margin for the grant landing at its earliest permitted jitter.
 */
#define HEAD_MARGIN_US 250u

/*
 * THE TAIL, AND IT IS THE DANGEROUS HALF: OVERSTAYING ASSERTS RATHER THAN
 * LOSING A WINDOW.
 *
 * Three terms, each measured or published rather than chosen:
 *
 *   RADIO_DISABLE_MAX_US   100 us. radiant_radio_nrf.c's own bound on how long
 *                          a ramp-down takes, and it is what the backend
 *                          already spends waiting in radio_disable_now().
 *
 *   CLOCK DISAGREEMENT     MPSL times the timeslot off LFCLK/GRTC while the
 *                          backend's close runs on an HFCLK-derived 1 MHz
 *                          TIMER, and mpsl_timeslot.h warns about exactly this.
 *                          MEASURED at -99 ppm between the two across 321
 *                          timeslot edges; over the 100 ms maximum grant that
 *                          is 10 us. Charged at 500 ppm - five times the
 *                          measurement - because the measurement is one board
 *                          at one temperature and the failure mode is an
 *                          assert. 100000 * 500e-6 = 50 us.
 *
 *   SIGNAL SLACK           The time between our TIMER0 compare firing and
 *                          ACTION_END being returned. MPSL publishes no figure;
 *                          in-tree users invent their own (flash_sync_mpsl.c
 *                          adds 100 us for the non-erase case, esb.c uses
 *                          50 + max_frame_duration/8). 100 us here, at the
 *                          larger end of both.
 *
 * 250 us total. If SIGNAL_OVERSTAYED is ever seen, this number is the answer
 * and mpsl_assert_handle() below is what reports it - not a silent widening
 * somewhere else.
 */
#define TAIL_MARGIN_US 250u

/*
 * How far ahead a request has to be placed to be honourable at all.
 *
 * MEASURED: the EARLIEST grant latency is 1692-1695 us, which is the HFXO
 * startup (CONFIG_MPSL_HFCLK_LATENCY, 1650 us on nRF54L from the DT
 * hfxo/startup-time-us) plus change. 2500 us is that plus the 2760 us tail
 * sample's worth of margin rounded down - anything nearer than this is a
 * request the arbiter cannot physically honour, and counting its refusal as
 * contention would be measuring ourselves.
 */
#define PLACE_LEAD_US 2500u

/*
 * THE SAME NUMBER AS A FLOOR RATHER THAN AS A BUDGET, AND THE TWO MUST NOT BE
 * THE SAME VALUE.
 *
 * radiant_radio_nrf.c already documents this trap at length for
 * min_arm_lead_us: the core plans against the ADVERTISED figure using a `now`
 * it sampled itself, and the backend then tests that plan against a `now`
 * sampled microseconds later, after a mutex, a pump loop and a scheduler pass.
 * Testing the advertised figure against the later sample fails by however long
 * that took - ALWAYS, on every arm.
 *
 * It was reproduced here exactly. The advertised lead was PLACE_LEAD_US, and
 * the refusal below tests the GRANT START, which is the caller's t_from minus
 * HEAD_MARGIN_US minus the backend's own air lead - about 380 us earlier than
 * the instant the core planned against. So the test could never pass, every arm
 * was refused, and the dongle configured perfectly and heard nothing:
 * chunks=0, deny=517013 in nineteen seconds.
 *
 * So the advertised lead carries the margin (see gate_min_arm_lead_us) and this
 * is what the hardware genuinely needs: the measured 1692-1695 us grant latency
 * with enough rounding to cover the 2760 us tail sample's neighbourhood. A
 * request that really cannot be placed still gets a loud refusal.
 */
#define PLACE_MIN_US 1900u

/*
 * The longest single operation, reported to the core as caps.max_window_us.
 *
 * MPSL_TIMESLOT_LENGTH_MAX_US is 100 000 and is an API ceiling rather than a
 * policy: a 250 ms ANT+ scan chunk cannot be REQUESTED. The head and tail come
 * out of it because they are inside the same grant.
 */
/*
 * EVERYTHING THAT IS ADDED TO A WINDOW AFTER THE SCHEDULER HAS ALREADY CAPPED
 * IT, and getting this wrong refused every single arm.
 *
 * caps.max_window_us bounds `t_close - t_open` in radiant_sched.c. Three things
 * are then added on top before a length reaches MPSL, and none of them is
 * visible to the scheduler:
 *
 *   HEAD_MARGIN_US + TAIL_MARGIN_US   this file's own margins.
 *   FRAME_TAIL_US                     radiant_radio_nrf.c extends t_to by the
 *                                     rest of the longest legal frame after
 *                                     t_close, so a frame whose t_sync is
 *                                     exactly t_close still finishes arriving.
 *                                     ~136 us at 1 M for a 15-byte body; 300
 *                                     covers the coded PHY's longer tail too.
 *   FOLLOW_ON_MAX_US                  the acknowledged-data reserve, up to
 *                                     ~2590 us on a listening master's slot.
 *
 * MEASURED, because this was not caught by reading: a scan chunk arrived as
 * from=+3114 to=+102798, so 99684 us of window against a 99500 us cap - already
 * over, before the margins - and the total came to 100184 against MPSL's 100000
 * ceiling. Every arm was refused, the dongle configured perfectly and heard
 * nothing, and the counter said `deny` rather than anything about length.
 */
#define FRAME_TAIL_US    300u
#define FOLLOW_ON_MAX_US 3000u

/*
 * END_MARGIN_US is in here because gate_acquire() adds it to every request
 * length: the timeslot must outlive the air it covers by the time it takes to
 * hand it back. Leaving it out is the same class of mistake as bug 3 - a window
 * the scheduler believed was legal arriving over MPSL's ceiling - and it is
 * defined further down - legal because this is a macro and nothing expands it
 * until after that definition, and the BUILD_ASSERT on MAX_WINDOW_US below is
 * what keeps that true.
 */
#define GRANT_OVERHEAD_US                                                     \
	(HEAD_MARGIN_US + TAIL_MARGIN_US + END_MARGIN_US + FRAME_TAIL_US +    \
	 FOLLOW_ON_MAX_US)

#define MAX_WINDOW_US (MPSL_TIMESLOT_LENGTH_MAX_US - GRANT_OVERHEAD_US)

/*
 * The extension chain. An in-flight grant grows itself rather than being
 * requested long in the first place, which is what makes the yield point the
 * arbiter's answer instead of a constant here - see ADR 0013.
 *
 * 1 ms a step, capped at the API maximum. esb.c caps its own extensions at
 * 2000 to bound clock drift inside one grant; ours is bounded by
 * MAX_WINDOW_US, which the tail margin above already charges drift against.
 */
#define EXTEND_STEP_US 1000u

/*
 * WHAT AN ELASTIC REQUEST ASKS FOR UP FRONT, WHICH IS NOT WHAT IT WANTS.
 *
 * ADR 0013 makes the sweep the elastic consumer, and the mechanism is a SHORT
 * grant grown by repeated SIGNAL_ACTION_EXTEND - "length 5-10 ms, then extend
 * repeatedly until t_close or EXTEND_FAILED". An extension is granted only if
 * MPSL has nothing else scheduled in the extended region, which is what makes
 * the yield point the arbiter's answer rather than a constant here.
 *
 * ASKING FOR THE WHOLE THING UP FRONT DOES NOT WORK, and that is measured
 * rather than assumed. A scan chunk wants ~96 ms and arrives about every
 * 100 ms, so a single request for the full length is a standing demand for
 * ~96 % of the radio - and MPSL simply refuses it. The trace was a first
 * EARLIEST grant, then SESSION_IDLE, then every following NORMAL request
 * BLOCKED, with no second grant ever: chunks=0 and no packet received, on a
 * board with nothing else on the radio at all.
 *
 * 10 ms is the top of the plan's own range. Below MPSL's maximum by four
 * orders of magnitude, so it is never the binding constraint, and long enough
 * that the extension chain is a handful of steps rather than a hundred.
 */
#define ELASTIC_INITIAL_US 10000u

/*
 * How far before the end of a grant the TIMER0 compare is placed, so that
 * ACTION_END is returned while the timeslot is still ours.
 *
 * NOT the same quantity as TAIL_MARGIN_US and the two must not be conflated -
 * that conflation is what made this file assert on its first timeslot for a
 * whole debugging session. TAIL_MARGIN_US is about the RADIO: ramp-down and
 * clock disagreement. This is about the SIGNAL PATH: an MPSL interrupt, a
 * dispatch into the callback, and the callback's own work, all of which has to
 * complete before the grant expires. Overstaying asserts.
 */
#define END_MARGIN_US 2000u

/*
 * How far ahead of the counter gate_release() places the compare that provokes
 * the end of a grant it no longer needs. Big enough that reading CAPTURE1,
 * writing CC0 and MPSL noticing all fit inside it; small enough that the air
 * handed back is measured in tens of microseconds rather than the milliseconds
 * of follow_on reserve the release exists to give up. See the re-read in
 * gate_release() for what happens when even this loses the race.
 */
#define RELEASE_LEAD_US 30u

/*
 * How long the anchor-establishing timeslot will wait to be placed. See
 * bootstrap_anchor(). Generous because nothing waits on it - it runs at radio
 * init with the channel not yet open - and because the only alternative to
 * waiting is falling back to an EARLIEST grant for a real window, which is the
 * thing it exists to avoid. A second is far past any measured grant latency
 * (~1695 us, worst observed 2760 us under a BLE advertiser).
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

/* The returned parameter must outlive the callback - mpsl_timeslot.h says so
 * directly, and the failure of getting it wrong is a request built from
 * whatever the stack was reused for. One static per session. */
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
	 * MPSL HAS HANDED US THE RADIO AND WE HAVE NOT GIVEN IT BACK.
	 *
	 * Distinct from `granted`, and the split is the fix for the one residual
	 * failure that is PERMANENT AND SILENT. `granted` means "the core may use
	 * this grant", and gate_release() clears it EARLY and deliberately - see
	 * the long comment there, it is load-bearing and a wedged dongle without
	 * it. But every teardown backstop (SESSION_IDLE, SESSION_CLOSED,
	 * gate_shutdown) was ALSO keyed on `granted`, so once gate_release() had
	 * cleared it, a lost TIMER0 compare meant nothing ever reached
	 * timer0_disarm() and therefore nothing ever ran on_grant_end().
	 *
	 * What is lost when that happens is not a packet, it is the receiver:
	 * radio_endpoints_attach(false) is the restore of the 802.15.4 driver's
	 * WRITE-ONCE SUBSCRIBE_RXEN. One missed restore and the other stack's
	 * receiver never ramps again, for ever, with no error anywhere.
	 *
	 * So this flag tracks the HARDWARE loan rather than the core's permission:
	 * set at SIGNAL_START, cleared ONLY by timer0_disarm(), which is also what
	 * makes that function idempotent and gives the invariant its teeth -
	 * exactly one on_grant_end() per granted timeslot, on every exit.
	 */
	volatile bool hw_held;
	/* The core has finished with the grant and wants it given back. */
	volatile bool release_wanted;
	/* This operation is allowed to grow itself - the elastic classes. */
	volatile bool extendable;

	/*
	 * THE ANCHOR. distance_us is measured from the PREVIOUS TIMESLOT'S
	 * START, not from an absolute time, so every later distance is computed
	 * against this - and WHEN A REQUEST IS BLOCKED NO TIMESLOT STARTS, SO
	 * THIS DOES NOT MOVE. Get that wrong and the whole schedule walks;
	 * measured at 0-17 us of error over 640 chained requests when it is
	 * right.
	 */
	radiant_time_t anchor;
	bool           anchor_valid;

	/*
	 * THE LAST INSTANT THE RADIO MAY STILL BE BUSY under the grant we hold
	 * right now - NOT the instant the timeslot expires, which is
	 * END_MARGIN_US later. Absolute, on the backend's own timebase.
	 *
	 * IT IS SET FROM THE GRANT AND NOT FROM THE REQUEST, and that
	 * distinction is load-bearing for the elastic class: an elastic request
	 * asks for ELASTIC_INITIAL_US and wants far more, so the requested end
	 * overstates what is held by up to the whole window. A follow-on
	 * measured against the request would be admitted against air that has
	 * not been granted yet.
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
	 * The staged operation is orphaned and must be told. Decided where the
	 * decision can be made - see end_housekeeping() - and acted on in the
	 * work queue, because the answer changes between the two.
	 */
	/*
	 * MPSL HAS A REQUEST OF OURS AND HAS NOT ANSWERED IT.
	 *
	 * Distinct from `pending`, and the distinction is the whole fix for the
	 * spiral described at DEADLINE_SLACK_US. `pending` is the CORE's
	 * question - "is a grant on its way for what is staged" - and the
	 * backstop timer answers it `false` on the core's behalf so the
	 * operation is not stranded. This one is MPSL's state, and nothing this
	 * side of the API may clear it: a placed request cannot be withdrawn,
	 * there is no per-request cancel, and until BLOCKED, CANCELLED or START
	 * arrives the session is not IDLE and every further request is -EAGAIN.
	 *
	 * So the backstop firing must NOT re-open the placement path. With the
	 * slack above, this should almost never be what refuses an acquire; it
	 * is counted (den_owed) precisely so that "almost never" is a reading
	 * and not an assumption.
	 */
	volatile bool  mpsl_owes;
	volatile bool  deny_wanted;
#define DENY_SRC_BLOCKED     0u
#define DENY_SRC_AFTER_GRANT 1u
#define DENY_SRC_DEADLINE    2u
	/*
	 * WHICH OF THE THREE SUBMITTERS SET deny_wanted. Diagnostic only - nothing
	 * branches on it. Last writer wins, which is correct for a flag that is
	 * itself last-writer-wins: the denial that eventually runs is the one the
	 * most recent submitter asked for. See the sent_* counters.
	 */
	volatile uint8_t deny_src;
	/*
	 * ACTION_END HAS ALREADY BEEN RETURNED FOR THIS GRANT.
	 *
	 * BUG 14, and the trace names it in four characters: `1:2 2:2` - TIMER0
	 * ending the timeslot, and then the RADIO signal ending it again.
	 *
	 * A grant ends and the RADIO event that the window was waiting for is
	 * still pending; MPSL delivers it as one more signal before it tears the
	 * timeslot down. That signal still has to be HANDLED - the event needs
	 * consuming or the operation never gets its terminal - but it must not
	 * be ANSWERED, because there is no longer a timeslot to act on and MPSL
	 * asserts on the second action.
	 *
	 * Which of the two arrives first is a race decided by a few hundred
	 * microseconds, so both orders have to be safe rather than one of them
	 * being the designed one.
	 */
	volatile bool  ended;
} g;

static struct {
	/*
	 * EVERY CALL IN, before any of the reasons one might not become a
	 * request. `placed` counts what reached MPSL; this counts what the core
	 * asked for, and the difference between them is the only way to tell
	 * "the arbiter refused us" from "nobody asked".
	 *
	 * Added because a 50 pp loss measurement against a driven master looked
	 * exactly like the former and was in fact the latter: placed == granted
	 * == blocked 0, and still half the packets missing. With only `placed`
	 * to read, a gate that is never called and a gate that is working
	 * perfectly are the same four numbers.
	 */
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
	/*
	 * WHICH `return GATE_DENIED` FIRED. Same argument as `acq`/`in_grant`
	 * above, one level finer: gate_acquire() has six of them and they shared
	 * one number, so "the gate refuses almost everything" was as far as any
	 * measurement could get.
	 *
	 * Added when a coexistence run showed acq=4351 with deny=4333 while
	 * `near` and `long` were both ZERO - so the refusals were coming from a
	 * branch nothing counted, and there was no way to tell which. The two
	 * candidates have completely different fixes: `pending` means the core
	 * is re-arming faster than one placement can complete and the answer is
	 * a rendezvous, while `no_anchor` means the anchor is being invalidated
	 * and the answer is the bootstrap.
	 */
	uint32_t den_pending;
	uint32_t den_no_anchor;
	uint32_t den_distance;
	uint32_t den_degenerate;
	uint32_t den_no_session;
	/* Refused because MPSL still owed an answer. See g.mpsl_owes. */
	uint32_t den_owed;
	uint32_t extends_ok;
	uint32_t extends_failed;
	/*
	 * How often the backstop timer fired rather than SIGNAL_BLOCKED.
	 *
	 * MEASURED ZERO, and that is the whole reason it is counted. All 17
	 * blocks in the P0 spike arrived BEFORE the start they referred to, so
	 * the timer should never win the race. A non-zero value here says the
	 * measurement did not generalise and the plan's original worry was
	 * right after all - which is a thing to find out from a counter rather
	 * than from a dongle that stops answering.
	 */
	uint32_t deadline_fired;
	/*
	 * WHICH ROAD A *DELIVERED* DENIAL CAME DOWN, AND HOW MANY WERE SUPPRESSED.
	 *
	 * The den_* block above only counts gate_acquire()'s synchronous refusals -
	 * RADIANT_RADIO_EDENIED, returned to the caller at arm time. It came back
	 * EIGHTY-ONE against acq=4013, which killed the working hypothesis that the
	 * gate was refusing the acquires. Everything else the core scores as
	 * DONE_DENIED must therefore be the OTHER code: RADIANT_RADIO_STATUS_DENIED,
	 * an asynchronous terminal for an operation whose arm was ACCEPTED.
	 *
	 * There are exactly three roads to that terminal and they call for three
	 * different fixes, so counting them together answers nothing:
	 *
	 *   sent_after_grant  end_housekeeping() queued a denial when a grant ended,
	 *                     and by the time denied_work_fn() ran there was still
	 *                     no request in flight. The window did not finish inside
	 *                     its grant. Fix is length/extension, not arbitration.
	 *   sent_deadline     the backstop timer beat the grant. Fix is placement
	 *                     lead. Expected to be ZERO - see deadline_fired.
	 *   sent_blocked      MPSL genuinely refused. Fix is demand shaping, and
	 *                     this is the only one of the three that means the air
	 *                     was actually contended.
	 *   deny_suppressed   the g.pending guard did its job: something re-armed
	 *                     between submit and run. Not a denial at all, counted
	 *                     to show the guard is live rather than inferring it.
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
	 * THE COUNTERS THE OVERNIGHT SOAK IS SCORED ON, and they exist because
	 * "it did not crash" is not a result.
	 *
	 *   grant_end_calls  every on_grant_end() this file made. The invariant is
	 *                    that it TRACKS `granted` - one per granted timeslot.
	 *                    Falling behind is the permanent-and-silent failure:
	 *                    a SUBSCRIBE_RXEN never restored. The 1 Hz line shows
	 *                    it as `granted` still climbing while `ok` is frozen.
	 *   late_disarm      a BLOCKED or CANCELLED arrived while the radio was
	 *                    still on loan, which is supposed to be impossible -
	 *                    those signals mean no timeslot started. Non-zero
	 *                    names a real fault that today presents only as a dead
	 *                    receiver, and it is a fault we would otherwise have
	 *                    assumed away rather than measured.
	 *   idle_disarm      SESSION_IDLE (or SESSION_CLOSED) found the radio
	 *                    still on loan and gave it back. This is the DESIGNED
	 *                    backstop for a timeslot MPSL ended by its own length
	 *                    expiry - no signal we handle - so it is allowed to be
	 *                    non-zero, which is exactly why it is not counted as
	 *                    late_disarm.
	 *   release_disarm   gate_release() handed the radio back itself rather
	 *                    than waiting for the compare it just armed. The
	 *                    normal path, so it should track `granted` closely;
	 *                    it is here so that grant_end_calls can be read
	 *                    against a known breakdown instead of a total.
	 */
	uint32_t grant_end_calls;
	uint32_t late_disarm;
	uint32_t idle_disarm;
	uint32_t release_disarm;
	/*
	 * A request that MPSL answered from inside mpsl_timeslot_request()
	 * itself, before the call returned. See BUG 15 in request_work_fn():
	 * non-zero means the flag-ordering fix there is load-bearing on this
	 * bench rather than defensive, and zero on a contended run would mean
	 * the wedge came back by some other road.
	 */
	uint32_t answered_inline;
	/* The last signal seen at all, for the case where the assert is raised
	 * without either of the above - which would mean MPSL is unhappy about
	 * something we returned rather than about when we returned it. */
	uint32_t last_signal;
} stats;

/*
 * THE SIGNAL TRACE, AND WHY IT IS A RING BUFFER AND NOT A printk.
 *
 * Every question left in this file is a question about SEQUENCE - which signal
 * arrived, in what order, and what was returned from it - and the header's own
 * warning is that a printk in this path costs ~87 us per character and moves
 * the timing it is measuring. A byte written to an array costs nothing
 * measurable, and the assert handler has all the time in the world to print
 * sixteen of them.
 *
 * High nibble is the signal, low nibble the action returned: 0 NONE, 1 END,
 * 2 EXTEND. Both fit because MPSL has ten signals and this file returns three
 * actions.
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
 * The backstop deadline timer
 *
 * NOT THE PRIMARY PATH, and the plan expected it to be. Its finding was that
 * denial "arrives late... it is too late to be useful on its own", making a
 * timer here a required component. Measured, every BLOCKED arrived 11-31 ms
 * EARLY: MPSL decides a NORMAL request when it is PLACED, not when the
 * requested start arrives, so the refusal comes back immediately too.
 *
 * It is kept because being wrong about that costs a wedged operation slot - the
 * core's single slot held by an operation that will never complete, with every
 * other channel's window expiring behind it - and because 17 samples against
 * one advertiser is not a guarantee. It costs a k_timer and a counter.
 * ---------------------------------------------------------------------------
 */

/*
 * HOW LONG PAST THE OPERATION'S OWN START THE BACKSTOP WAITS, AND WHY IT IS NOT
 * ZERO ANY MORE.
 *
 * It WAS zero: the timer fired exactly at the start the grant would have been
 * useless after, on the measurement that "every BLOCKED arrived 11-31 ms
 * EARLY". That measurement is real and it is still quoted above - but it was
 * taken with NO SECOND STACK. MPSL answers a request from
 * mpsl_low_priority_process(), a COOPERATIVE THREAD, and with the SoftDevice
 * Controller actually advertising that thread does not get to run within the
 * ~2 ms of arm lead a scan chunk is placed on.
 *
 * The result was not a slow gate, it was a SPIRAL, and the counters name it
 * exactly (nRF54L15, 100 ms connectable advertiser, 260 s):
 *
 *   acq=7229 placed=3730 granted=900 blocked=2830 eagain=3397 dead=3411
 *   sent grant=119 dead=3411 blk=2828 supp=11
 *
 * dead=3411 against eagain=3397 is not a correlation, it is the same event
 * counted twice round the loop:
 *
 *   1. A request is placed and the backstop is armed ~2 ms out.
 *   2. MPSL has not answered yet, so the backstop fires, clears g.pending and
 *      denies the operation - WHILE MPSL STILL OWES AN ANSWER AND STILL HOLDS
 *      THE SESSION. There is no way to withdraw a placed request; the timeslot
 *      API has no per-request cancel, only session close.
 *   3. The core replans immediately and arms again. g.pending is false, so the
 *      den_pending guard - which exists for exactly this - waves it through.
 *   4. mpsl_timeslot_request() answers -NRF_EAGAIN, because the session is not
 *      IDLE. The request is kept for SESSION_IDLE, next_grant is NOT committed,
 *      and a fresh ~2 ms backstop is now running.
 *   5. Go to 2.
 *
 * Half of all acquires (3397 of 7229) were burnt on that loop, and every turn
 * of it delivered a DONE_DENIED to the core, which is why "the gate denies
 * everything" kept being the reading no matter which counter it was read from.
 *
 * 20 ms is chosen against what the backstop is FOR rather than against a
 * measured latency. It is not a scheduling deadline - MPSL's own BLOCKED is,
 * and this only has to beat "MPSL never answers at all", which is a fault and
 * not a load. It is comfortably past the 11-31 ms band the P0 spike measured
 * blocks arriving in, well past any cooperative-thread scheduling delay a
 * connectable advertiser can impose, and still two orders of magnitude below
 * the wedge it is guarding against.
 */
#define DEADLINE_SLACK_US 20000u

static void deadline_expired(struct k_timer *t);
static K_TIMER_DEFINE(deadline, deadline_expired, NULL);

/* ---------------------------------------------------------------------------
 * THE SPLIT: WHAT RUNS IN A CALLBACK AND WHAT RUNS IN A THREAD
 *
 * Exactly one thing has to happen inside the grant's own signal callback -
 * programming the peripheral - because that is the whole reason the grant
 * exists and it cannot be done a moment earlier or later. Everything else moves
 * to this work queue, and the reason is not tidiness:
 *
 *   ISSUING AN MPSL REQUEST. mpsl_timeslot_request() from inside a timeslot,
 *   or from the RADIO interrupt at MPSL_HIGH_IRQ_PRIORITY, is not a supported
 *   call. The one legal way to ask for the next timeslot from within one is
 *   SIGNAL_ACTION_REQUEST returned from the callback - which CLOSES the current
 *   timeslot to do it, handing the radio back and asking for it again with
 *   another stack free to land in the gap. So the request is handed to a thread
 *   instead.
 *
 *   CALLING INTO THE CORE. deliver_terminal() runs the completion callback,
 *   which runs a scheduler pass, which may arm the next operation - an
 *   unbounded-looking amount of work that must not happen inside an MPSL signal
 *   callback at priority 0. Overstaying the timeslot from one ASSERTS.
 *
 * THIS IS WHAT WAS WRONG. Before the split, opening a channel stopped the
 * console mid-second and the dongle answered nothing more - no fault, no MPSL
 * assert, no further housekeeping line. The housekeeping thread had stopped, so
 * the fault was at or above its priority, and the grant path was doing all of
 * the above at priority 0.
 *
 * A DEDICATED QUEUE, not the system one: the system work queue is shared with
 * anything an application cares to put on it, and a radio schedule that has to
 * place a reservation within a couple of milliseconds cannot queue behind a
 * filesystem flush.
 * ---------------------------------------------------------------------------
 */


static void request_work_fn(struct k_work *w);
static void denied_work_fn(struct k_work *w);

static K_WORK_DEFINE(request_work, request_work_fn);
static K_WORK_DEFINE(denied_work, denied_work_fn);

/*
 * WHAT THE NEXT GRANT WILL BE READ WITH, HELD APART FROM WHAT THE CURRENT ONE
 * IS BEING READ WITH.
 *
 * These four used to be written straight into `g` by gate_acquire(). That is
 * safe only while an acquire cannot happen during a grant - and the whole point
 * of the change below is that it can and constantly does: a tracked window's
 * packet arrives mid-window and the receive callback arms the next slot a
 * quarter of a second out, with the current timeslot still live. Writing
 * grant_end or granted_len_us through `g` at that moment would retune the
 * running grant's own end and extension arithmetic to describe a window that
 * has not been placed yet.
 *
 * So they are staged here and committed by request_work_fn() once
 * mpsl_timeslot_request() has actually accepted the request - which is in
 * thread context, about 1.7 ms before SIGNAL_START can read them, and after any
 * -NRF_EAGAIN round trip through SESSION_IDLE (by which time the old grant is
 * definitively over).
 */
static struct {
	uint32_t       want_len_us;
	uint32_t       granted_len_us;
	radiant_time_t grant_end;
	bool           extendable;
} next_grant;

/*
 * HOW BIG THE ELASTIC CLASS ASKS ITS FIRST BITE, AND WHY IT IS A VARIABLE.
 *
 * ADR 0013 says the sweep is the consumer that gives way, and a fixed 10 ms
 * initial request is only half of giving way. Measured against a 100 ms BLE
 * advertiser: placed=570 granted=2 blocked=568. A tracked window - 5 ms, placed
 * at an instant the master chose - coexists with that advertiser perfectly
 * (blocked=0). A scan chunk asking for a fixed 10 ms and then extending does
 * not, and once the channel drops for any reason it can never re-acquire,
 * because acquisition is all sweep.
 *
 * So the first bite backs off when it is refused and grows back when it is not.
 * Halving on BLOCKED reaches the floor in three refusals; growing by one
 * EXTEND_STEP_US on success climbs back slowly enough that a busy neighbour is
 * not fought over every cycle. The chunk it gets is then grown the ordinary way
 * by the extension chain, so the backoff costs dwell only while the air is
 * genuinely contended - which is the trade ADR 0013 asks for and the fixed
 * constant could not express.
 *
 * NOT APPLIED TO TRACKED WINDOWS OR TRANSMITS. Those are not elastic: their
 * length is the window the core chose, and shortening one would hand back air
 * in the middle of somebody's packet.
 */
#define ELASTIC_FLOOR_US 2500u
static uint32_t elastic_initial_us = ELASTIC_INITIAL_US;

/*
 * AND WHERE IT ASKS, WHICH TURNED OUT TO MATTER MORE THAN HOW MUCH.
 *
 * Backing the request off to 3.5 ms did not help at all: blocked=408 out of
 * 410 against a 100 ms advertiser, which no amount of bad luck explains for a
 * window that size. It is a phase lock. A scan chunk is ~94 ms of dwell plus
 * placement overhead, so the sweep re-asks on a cadence within a few
 * milliseconds of the advertiser's 100 ms interval - and the anchor
 * deliberately does NOT move on BLOCKED, which is right for keeping a run of
 * refusals from walking the schedule and is exactly what makes the collision
 * reproduce forever. Every retry lands on the same phase of the neighbour's
 * period as the one before it.
 *
 * So a refused elastic request moves before it tries again. The walk is a
 * deterministic odd-increment one rather than a PRNG: it needs to be
 * uncorrelated with 100 ms, not unpredictable, and a bench result that cannot
 * be replayed is worth much less here. 2137 us is prime, so the sequence visits
 * every offset before repeating, and the span is a little over one ANT+ slot so
 * the sweep's own coverage is not distorted by it.
 *
 * ONLY THE ELASTIC CLASS MOVES. A tracked window's instant is the master's, not
 * ours; skewing it would move the receiver off the packet, which is the very
 * bug this file just finished paying for elsewhere.
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
	 * BUG 15: THE FLAG IS ARMED BEFORE THE CALL, AND THAT ORDERING IS THE
	 * WHOLE OF IT. IT IS A PERMANENT WEDGE OTHERWISE.
	 *
	 * This used to read `rc = mpsl_timeslot_request(); if (rc == 0) { ...
	 * g.mpsl_owes = true; }` - the flag raised AFTER the request it
	 * describes had already been placed. Between those two statements MPSL
	 * may already have ANSWERED, and the answer's whole job is to clear this
	 * flag. BLOCKED is delivered in mpsl_low_priority_process() context,
	 * which is reached from inside this very call when the arbiter can
	 * refuse the request without deferring it. So:
	 *
	 *   mpsl_timeslot_request() -> ... -> on_signal(BLOCKED)
	 *                                       g.mpsl_owes = false   (answered)
	 *   ...returns 0
	 *   g.mpsl_owes = true;                                       (STALE)
	 *
	 * and from that instant the gate believes MPSL owes it an answer that
	 * has already been given. Nothing on this side of the API can clear it:
	 * a placed request cannot be withdrawn, so there is no request left to
	 * be answered, no further BLOCKED, no further START, and SESSION_IDLE
	 * has already been and gone. Every subsequent gate_acquire() is refused
	 * with den_owed for the rest of the boot and the ANT radio is dead.
	 *
	 * MEASURED, and it is not a theory: P4 MED, two runs, `placed=3
	 * granted=2 blocked=1` - every placed request answered - with `owes=1`
	 * and `owed=2403` climbing. It only bites where a request is refused
	 * SYNCHRONOUSLY, which needs a genuinely contending stack, which is why
	 * a year of BLE bring-up never saw it.
	 *
	 * Armed first, the sequence is safe whichever way round it happens: an
	 * answer arriving inside the call finds the flag already true and clears
	 * it; an answer arriving later finds it true and clears it then. The
	 * only case that needs undoing is the request that was NOT placed, and
	 * no answer can be in flight for one of those - so clearing it below
	 * cannot race with anything.
	 *
	 * stats.answered_inline counts the case, so that "this path is
	 * exercised" is a reading rather than an assumption.
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
		 * BUG 10: NOT A REFUSAL, A "NOT YET" - AND REPORTING IT AS A
		 * REFUSAL COST FOURTEEN SCAN CHUNKS IN FIFTEEN.
		 *
		 * -NRF_EAGAIN means only that the session is not IDLE. The
		 * ordinary sequence produces it every single time: an operation
		 * completes, its terminal runs a scheduler pass, the pass arms
		 * the next window, and that request is placed in the handful of
		 * microseconds between gate_release() asking for the end and the
		 * ACTION_END that delivers it. The air was never contended and
		 * nothing was denied; the request was simply early.
		 *
		 * Measured before this: chunks=15 deny=14, one denial per chunk,
		 * with blocked=0 - the arbiter refusing nothing at all while the
		 * core was told it had been refused fourteen times. Under a real
		 * BLE load that statistic is the one that decides whether the
		 * sweep backs off, so it has to mean what it says.
		 *
		 * The request is KEPT, and SESSION_IDLE places it - which is the
		 * signal that says the condition has cleared, and the only one
		 * that says it unconditionally. The deadline timer is left
		 * running, so a session that never goes idle still resolves the
		 * operation rather than stranding it.
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
 * ESTABLISH THE ANCHOR BEFORE ANY REAL WORK NEEDS IT.
 *
 * Rule 2 in this file's header says the first request in a session must be
 * EARLIEST, and this file used to satisfy that by making the first REAL request
 * EARLIEST - which is a different thing and is why the first window never
 * received anything.
 *
 * EARLIEST means "as early as you can", NOT "at the start I described". The
 * grant therefore arrives at an instant MPSL chose, anywhere between the request
 * and the timeout, while the operation staged behind it has an absolute t_open
 * that was computed from a placement lead. When the grant lands late, t_open is
 * already inside the hardware's own arm lead and program_rx() answers ETIME: the
 * measured trace was one grant, one FAILED operation, and a channel that stopped
 * searching. When it lands early, the receiver is programmed for a window most
 * of which falls outside the grant.
 *
 * So the EARLIEST timeslot is spent on nothing but its own start time. It is the
 * API minimum long, carries no operation, and is handed straight back from
 * SIGNAL_START; what it leaves behind is g.anchor, after which every request is
 * NORMAL and lands exactly where it was asked for - 0-17 us over 640 chained
 * requests, measured in the P0 spike.
 *
 * Callable from any context: the MPSL call itself is deferred to the work queue
 * like every other one.
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
	 * THE TEST IS MADE HERE AND NOT WHERE THE WORK WAS SUBMITTED, and the
	 * difference is a wedged dongle.
	 *
	 * A grant ends with the window still armed, so the denial is queued -
	 * and then the RADIO event that the window was waiting for arrives a few
	 * hundred microseconds later, in the same interrupt, completes the
	 * operation properly and arms the next one. By the time this thread
	 * runs, the staged operation is a DIFFERENT operation with a request of
	 * its own in flight, and denying it strands it: nothing has failed, so
	 * nothing will re-arm.
	 *
	 * g.pending is the question "is a grant on its way for whatever is
	 * staged", and it has to be asked at the moment it is acted on.
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
 * A ONCE-A-SECOND DUMP, WHICH IS THE ONLY INSTRUMENT THAT SHOWS A STALL.
 *
 * Every other diagnostic in this file is emitted BY an event, so a gate that
 * has stopped producing events produces no diagnostics either - and "the last
 * line says placed=2" is then indistinguishable from "it placed two and is
 * still working". Both were true at different times in this bring-up and the
 * difference is the whole question.
 *
 * On its own thread and on a fixed period, so it reports the absence of
 * activity as clearly as the presence of it. Deferred logging, so the cost is
 * a memcpy on the work queue and nothing at all in the grant path.
 */
static void gate_dump_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(gate_dump, gate_dump_fn);

static void gate_dump_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	/*
	 * AND IT CARRIES THE INVARIANT, WHICH IS THE DIFFERENCE BETWEEN A SOAK
	 * AND AN OVERNIGHT RUN THAT CAN ONLY REPORT "IT DID NOT CRASH".
	 *
	 * `end=` against `granted=` is the whole scoring rule: exactly one
	 * on_grant_end() per granted timeslot.
	 *
	 * THE BREAKDOWN AFTER IT DOES NOT SUM TO `end=` BY ITSELF, and printing
	 * it as though it did would be the more misleading of the two options at
	 * 3 a.m. Only the three EXCEPTIONAL routes are counted: a release that
	 * handed back rather than waiting for its compare, a session backstop
	 * that found the radio still out, and the must-be-zero one. The ordinary
	 * route - a TIMER0 or RADIO signal ending the grant it was armed for -
	 * has no counter of its own, so it is printed as `norm=`, computed as the
	 * remainder - and named `norm` rather than `sig` because `sig=` already
	 * means last_signal further along this same line. That also makes it
	 * self-checking: the remainder can only go
	 * negative if the invariant has already broken, so it is clamped and the
	 * clamp is visible.
	 *
	 * `late=` is the alarm. It is the only one of the four that names a
	 * fault rather than a route.
	 *
	 * The three assert counters are here for the same reason. They used to be
	 * printed ONLY by mpsl_assert_handle(), i.e. only once it was too late to
	 * do anything but read them, and unknown_signal was not printed anywhere
	 * a running board could show it.
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
 * TAKE THE COMPARE BACK, AND DO IT ON EVERY PATH THAT ENDS A TIMESLOT.
 *
 * MPSL_TIMER0 is NRF_TIMER10 on this part and it belongs to MPSL; we are
 * lent it for the duration of a grant, and its interrupt is dispatched by
 * MPSL's own handler, which turns it into SIGNAL_TIMER0. Leaving COMPARE0
 * enabled past the end of the grant leaves an interrupt armed on a peripheral
 * that is no longer ours and whose event nothing will clear - and an interrupt
 * whose event is never cleared re-enters for ever.
 *
 * THAT IS THE SHAPE OF THE OBSERVED FAILURE. The board stayed alive and
 * answered nothing: no fault message - and Zephyr prints those synchronously,
 * so there was no fault - but no thread ran again either, which is what being
 * pinned in an interrupt at priority 0 looks like from the outside. Deferred
 * logging needs a thread to emit anything, so the console simply stopped
 * mid-line.
 *
 * Called before returning ACTION_END from anywhere, and on every signal that
 * means the grant is gone.
 */
/*
 * HANDING THE RADIO BACK IS A SEPARATE OBLIGATION FROM ENDING THE TIMESLOT,
 * and they were one function because on every path they happen together.
 *
 * They are split because on exactly one path they must NOT. gate_release()
 * needs the radio back at once - see the comment there - but it also needs the
 * TIMER0 compare LEFT ARMED, because returning ACTION_END from the signal that
 * compare produces is the only way MPSL ends a timeslot at all. Disarming the
 * compare there would trade the hazard being fixed for an overstay assert,
 * which is the failure this file's whole end margin exists to prevent.
 *
 * So this is the half that gives the peripheral back, and it is the half that
 * carries the invariant: EXACTLY ONE on_grant_end() PER GRANTED TIMESLOT, ON
 * EVERY EXIT. Not "at least one" - a second call hands back a radio the
 * controller has already picked up. Not "at most one" - a missed one leaves the
 * 802.15.4 driver's write-once SUBSCRIBE_RXEN swapped out for the rest of the
 * boot, which is a receiver that stops and never resumes with nothing printed
 * anywhere. g.hw_held is what makes it exactly one, so no caller has to reason
 * about whether it is the first to arrive.
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
	 * AND GIVE THE RADIO BACK AS IT WAS LENT, on the same call and for the
	 * same reason: this is already the one function every path that ends a
	 * grant has to call, so it is the one place a second "put it back" cannot
	 * be forgotten on a path somebody adds later.
	 *
	 * What it puts back is RADIO->INTENSET, RADIO->SHORTS, and the state
	 * machine itself. Left set, the mask is two interrupt sources armed on a
	 * peripheral the SoftDevice Controller is transmitting on; the shorts are
	 * worse, being hardware edges that fire on the controller's events and
	 * not ours. See radiant_nrf_gate_on_grant_end().
	 *
	 * The timer registers are put back unconditionally - that is free, and a
	 * compare left armed on a peripheral that is no longer ours re-enters for
	 * ever - and the handing back is the idempotent half. See
	 * gate_hand_back() for why the two are separable at all.
	 */
	gate_hand_back();
}

/*
 * WHAT HAS TO HAPPEN WHEN A GRANT ENDS, AND THE ONE CASE IT MUST NOT TOUCH.
 *
 * BUG 13. This was an unconditional k_work_submit(&denied_work), on the
 * reasoning that both handlers behind it are no-ops when there is nothing to do.
 * That stopped being true the moment the RADIO signal was wired up, and the
 * failure is vicious:
 *
 *   A window completes inside its grant. Its terminal runs a scheduler pass,
 *   the pass ARMS THE NEXT WINDOW, and that arm stages an operation and places a
 *   request - all before this callback returns. The grant then ends, the denial
 *   work runs, and radiant_nrf_gate_on_denied() delivers DENIED to the operation
 *   that was armed microseconds ago and is waiting perfectly happily for a grant
 *   that is on its way.
 *
 * So the denial is only for a staged operation that NOTHING is coming for, and
 * g.pending is exactly that test: a request outstanding means the staged
 * operation belongs to it. With no request outstanding a staged operation is
 * orphaned and must be told, which is the wedge bug 5's comment describes.
 *
 * THE TEST IS MADE IN denied_work_fn AND NOT HERE, because the answer changes
 * between the two - see the comment there. All this does is ask the question.
 *
 * The failed-programming finish is unconditional because it carries its own
 * flag and is a no-op without it.
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

	/*
	 * THE INSTRUMENT THAT NAMES THE SPINNING INTERRUPT.
	 *
	 * printk is synchronous, so it survives the scheduler stopping - which
	 * is the whole difficulty here: deferred logging needs a thread, and by
	 * the time this fails no thread runs. Shot-limited so it cannot itself
	 * become the flood.
	 *
	 * Read the trace as a sequence. START then TIMER0 repeating for ever
	 * means the compare is re-firing; START then silence means the RADIO
	 * interrupt is the one spinning and this callback is not being reached
	 * at all. Those two need completely different fixes, and nothing else
	 * available here distinguishes them.
	 */
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
			/*
			 * THE ANCHOR IS THE WHOLE POINT OF THIS TIMESLOT and it
			 * was set three lines up. Nothing is staged behind it,
			 * so the peripheral is not touched and the core is not
			 * called; the air is handed straight back.
			 */
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
		 * PROGRAMME THE PERIPHERAL NOW, INSIDE THE GRANT, AND NOT ONE
		 * REGISTER BEFORE IT. This is the call that does everything
		 * radiant_radio_nrf.c staged at the arm - RADIO configuration,
		 * the compare, the (D)PPI enables - and the whole reason the
		 * seam exists.
		 *
		 * It may complete the operation synchronously (an arm that
		 * turns out to be unreachable delivers its own terminal), which
		 * sets release_wanted before this returns. That is handled
		 * below rather than in a second signal.
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
		 * A TIMER0 compare so that there is always a signal to end the
		 * timeslot from. Without it, an operation whose RADIO interrupt
		 * never comes would hold the grant to its full length and then
		 * OVERSTAY, which asserts.
		 */
		/*
		 * THE COMPARE THAT ENDS THE TIMESLOT, AND ITS MARGIN IS
		 * MEASURED RATHER THAN REASONED.
		 *
		 * TAIL_MARGIN_US (250) covers what the RADIO needs - ramp-down
		 * plus clock disagreement - and is NOT enough to end a timeslot
		 * with. Between this compare firing and ACTION_END being
		 * returned there is an MPSL interrupt, a dispatch into this
		 * callback, and everything the callback does; 250 us of that
		 * runs past the grant, and overstaying ASSERTS.
		 *
		 * Measured, and the numbers are stark. At len - 250 the board
		 * asserted on the FIRST or second timeslot: placed=1 granted=1.
		 * With the compare forced well inside the grant it ran to
		 * placed=1007 granted=1007 - a thousand timeslots - before a
		 * different and much later assert. The mechanism was never
		 * broken; the margin was.
		 *
		 * 2 ms, which is an order of magnitude above the ~166 us that
		 * Nordic's own sample takes from SIGNAL_START to its TIMER0
		 * signal on this board, and small against the 96 ms grant it
		 * comes out of.
		 */
		nrf_timer_cc_set(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL0,
				 (g.granted_len_us > END_MARGIN_US)
					 ? (g.granted_len_us - END_MARGIN_US)
					 : (g.granted_len_us / 2u));
		nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
		break;

	case MPSL_TIMESLOT_SIGNAL_RADIO:
		/*
		 * BUG 12, AND IT IS THE ONE THAT MADE THE GATE DEAF.
		 *
		 * INSIDE A TIMESLOT THE RADIO INTERRUPT IS MPSL'S, NOT OURS.
		 * radiant_radio_nrf.c connects its handler to RADIO_0_IRQn and
		 * every terminal event in the backend comes from there - but
		 * MPSL owns that vector for the duration of a grant and hands
		 * the interrupt over as this signal instead. The connected
		 * handler is simply never entered.
		 *
		 * So a window armed correctly, ran its full length, and never
		 * ended. The measured signature was a scheduler crediting the
		 * whole dwell with NO terminal of its own - chunks=10,
		 * scan_us=942000/10, end ok=0 abrt=0 fail=0 - and every window's
		 * only terminal arriving afterwards from this file as DENIED.
		 * Read as a denial problem it has no solution; nothing had been
		 * denied. The counter that named it was `unknown=2` in the
		 * assert line, 2 being this signal's value, arriving in a
		 * default case that did nothing with it.
		 *
		 * The handler is re-entered by hand and then, if it finished the
		 * operation, the grant is given back from here rather than from
		 * a TIMER0 signal thirty microseconds later. This is a
		 * high-priority signal, so ACTION_END is legal from it.
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
		/* Grow toward what the operation actually wants, not toward the
		 * API ceiling: extending past t_close would hold air nobody is
		 * going to use.
		 *
		 * THE LAST STEP IS THE REMAINDER, not a whole step. Stopping at
		 * the last whole multiple would end the grant up to
		 * EXTEND_STEP_US before the window it was placed for, which is
		 * the receiver being taken away mid-window - indistinguishable
		 * at the core from a sensor that went quiet. The remainder is
		 * only asked for if MPSL will entertain an extension that small;
		 * below that the grant simply ends, and the arithmetic in
		 * gate_acquire() keeps the shortfall under the API minimum. */
		if (!g.release_wanted && g.extendable &&
		    g.granted_len_us < g.want_len_us) {
			uint32_t step = g.want_len_us - g.granted_len_us;

			if (step > EXTEND_STEP_US) {
				step = EXTEND_STEP_US;
			}
			/*
			 * BUG 11: THE CEILING HERE IS THE TIMESLOT'S, NOT THE
			 * WINDOW'S, AND THEY ARE NOT THE SAME NUMBER.
			 *
			 * This read MAX_WINDOW_US, which is what the SCHEDULER
			 * may ask for - MPSL's maximum with every margin already
			 * subtracted. A timeslot covering a window that long is
			 * necessarily LONGER than it: head margin, tail margin
			 * and the end margin are all inside the grant and none
			 * of them is inside the window.
			 *
			 * So the chain stopped 2500 us short of the length it
			 * had asked for, the grant ended before the window's own
			 * close compare, and the operation was still armed when
			 * MPSL took the radio back. Every chunk then got its
			 * terminal from the gate instead of from the radio:
			 * chunks=10 deny=9, with end ok=0 abrt=0 - ten full
			 * 94.2 ms windows, none of which ended by itself.
			 *
			 * want_len_us is bounded against MPSL's maximum in
			 * gate_acquire(), so this is a backstop rather than the
			 * binding constraint.
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
		 * BUG 8, AND IT IS THE ONE THAT SURVIVED EVERY OTHER FIX. This
		 * line used TAIL_MARGIN_US, so an extended grant put its compare
		 * 250 us before the new end while the initial grant put it
		 * END_MARGIN_US before - the two quantities the header warns must
		 * not be conflated, conflated on exactly one of the two paths.
		 *
		 * The symptom was ext=1204/0 with an assert at placed=14: the
		 * chain extended happily, and the moment a window stopped
		 * extending - because it had reached want_len_us - it had 250 us
		 * to get ACTION_END back, which is the overstay this whole margin
		 * exists to prevent. The extension chain was never the problem
		 * and the counters said so; the END that follows it was.
		 *
		 * The compare therefore advances by exactly one step per
		 * extension and the runway ahead of it is END_MARGIN_US at all
		 * times, initial grant and extended grant alike.
		 */
		stats.extends_ok++;
		g.granted_len_us += g.ext_step_us;
		g.grant_end += (radiant_time_t)g.ext_step_us;
		nrf_timer_cc_set(MPSL_TIMER0, 0,
				 g.granted_len_us - END_MARGIN_US);
		break;

	case MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED:
		/*
		 * THE YIELD POINT, DISCOVERED RATHER THAN CONFIGURED, and it
		 * arrives with the grant still live - which is what lets the
		 * window be closed cleanly and reported as a partial.
		 *
		 * The core is told through the ordinary abort path, not through
		 * a denial: RADIANT_RADIO_STATUS_ABORTED already means "the
		 * window opened and was cut short", which the search accounting
		 * handles as "credit what was actually listened to". Denial and
		 * elasticity stay two different signals.
		 *
		 * UNEXERCISED AS OF P0. 4450 extensions against a 1 s BLE
		 * advertiser produced zero refusals, because the advertiser
		 * never wanted the air during a 90 ms window. P4 against
		 * OpenThread is what tests this branch.
		 */
		stats.extends_failed++;
		g.granted = false;
		timer0_disarm();
		(void)radiant_radio_abort();
		/*
		 * AND THE ORPHAN GETS ITS DENIAL, which this path omitted.
		 *
		 * Every other exit that ends a grant runs end_housekeeping();
		 * this one did not, on the reasoning that radiant_radio_abort()
		 * above already delivers the terminal. It does - for an
		 * operation that is ARMED. An operation that was STAGED and had
		 * not yet been programmed has no terminal to abort and nothing
		 * is coming for it, which is precisely the wedge
		 * end_housekeeping() exists to prevent: the core's single
		 * operation slot held for ever by something waiting on a grant
		 * that ended. The g.pending test inside denied_work_fn() is what
		 * keeps this from denying an operation a fresh request IS coming
		 * for, so adding it here is safe as well as necessary.
		 *
		 * WHAT WAS DELIBERATELY NOT CHANGED, and it is a judgement call
		 * recorded rather than settled: radiant_radio_abort() still runs
		 * from this priority-0 callback, where it can spin up to ~100 us
		 * and then take a full scheduler pass through deliver_terminal().
		 * That is more work than this file tells everybody else to do
		 * here. It is left in place because the alternative - deferring
		 * the abort to the work queue - hands the timeslot back with the
		 * peripheral still armed, and this branch is only reachable
		 * under a genuinely contending stack, which is the P4 run
		 * itself. Move it only with a measurement of this path in hand.
		 */
		end_housekeeping();
		rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		break;

	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		/*
		 * LOW PRIORITY CONTEXT - a cooperative thread, not the radio
		 * interrupt. No action may be returned, and none is needed: the
		 * whole response is to tell the core, which is a function call.
		 *
		 * THE ANCHOR DOES NOT MOVE. No timeslot started, so the next
		 * distance is computed against the older anchor. This is the
		 * single most error-prone line in the file and the reason
		 * anchor_valid is not cleared here.
		 *
		 * This is also the first HAL event that can reach the core
		 * outside the radio interrupt - the amendment written into
		 * radiant_radio_hal.h's callback-context paragraph.
		 */
		if (signal == MPSL_TIMESLOT_SIGNAL_BLOCKED) {
			stats.blocked++;
		} else {
			stats.cancelled++;
		}
		/*
		 * AND THE ASSUMPTION ABOVE IS NOW CHECKED RATHER THAN MADE.
		 *
		 * "No timeslot started" is what these two signals mean, and the
		 * anchor rule directly above depends on it - but nothing in this
		 * file enforced it, and the cost of being wrong is not a lost
		 * packet. It is the 802.15.4 driver's write-once SUBSCRIBE_RXEN
		 * left swapped out for the rest of the boot: a receiver that
		 * stops and never resumes, with no error printed anywhere.
		 *
		 * So the radio is handed back if it is somehow still on loan,
		 * and the event is COUNTED. If late_disarm is ever non-zero on
		 * the bench it names a real fault - one that today presents
		 * only as a dead receiver hours later, which is the hardest
		 * possible thing to trace back to here.
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
		 * A RUN OF REFUSALS MEANS THE ANCHOR IS THE PROBLEM, NOT THE
		 * REQUEST - and this is the third thing tried on this symptom,
		 * so the first two are recorded beside it rather than deleted.
		 *
		 * distance_us is measured from the PREVIOUS TIMESLOT'S START,
		 * and the anchor deliberately does not move when a request is
		 * refused. That rule is right - it is what stops a run of
		 * denials walking the schedule - and it has an end: if nothing
		 * is ever granted, the anchor ages without bound and every
		 * subsequent request asks MPSL to place a slot further and
		 * further into the future. Measured against a 100 ms advertiser,
		 * two grants in the first second and then 93 consecutive blocks,
		 * with the distance by then over a minute. Shrinking the request
		 * (10 ms -> 3.5 ms) changed nothing; skewing its phase changed
		 * nothing; both are kept because both are correct for the
		 * contended case and neither was this.
		 *
		 * So after a short run of refusals the anchor is abandoned and
		 * the next acquire re-bootstraps with EARLIEST - which is
		 * exactly the tool for "somewhere soon, you choose", and the one
		 * request type a busy neighbour cannot starve. The count is
		 * small because there is nothing to lose: a fresh anchor costs
		 * one minimum-length timeslot.
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
		 * NO TIMESLOT IS RUNNING AND NO REQUEST IS OUTSTANDING. That is
		 * what IDLE means, and it is the only signal that says so
		 * unconditionally - so it is where g.granted is made true again
		 * by being cleared.
		 *
		 * WITHOUT THIS THE GATE STALLS AFTER EXACTLY ONE GRANT, and the
		 * trace is unambiguous: gate_acq(anchor=0) -> s0 -> s7 ->
		 * gate_acq(anchor=1) -> s5 BLOCKED -> nothing, ever. A timeslot
		 * that MPSL ends by its own length expiry produces no signal we
		 * were handling, so g.granted stayed true from the first grant;
		 * every later gate_acquire() then took the "a grant is already
		 * held" branch, found the new window did not fit inside the long
		 * dead grant_end, and returned GATE_DENIED for the rest of time.
		 *
		 * The END paths clear it too, and they still should - this is
		 * the backstop for the ones that never signal, not a
		 * replacement for them.
		 *
		 * AND THE TEST IS g.hw_held, NOT g.granted. It used to be
		 * g.granted, which made this backstop useless for the one case
		 * it most needed to cover: gate_release() clears g.granted
		 * BEFORE the compare it arms has fired, so a lost compare
		 * arrived here with g.granted already false and the radio still
		 * on loan - and nothing ran on_grant_end() at all. hw_held is
		 * exactly "MPSL handed us the radio and we have not given it
		 * back", which is what the teardown keys on.
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
		 * AND THE DISARM ITSELF IS UNCONDITIONAL, WHICH IS NOT THE SAME
		 * TEST AS THE COUNTER'S.
		 *
		 * The counter asks "was the radio still out?"; this asks "could
		 * a compare still be armed?", and since the A1 split those are
		 * different questions. gate_release() hands the radio back
		 * immediately and deliberately LEAVES the compare armed, because
		 * that compare is the only thing that will produce a signal to
		 * return ACTION_END from. So the state this backstop most needs
		 * to clean up - a release whose compare was then lost - arrives
		 * here with hw_held already false and an interrupt still armed
		 * on a peripheral that is no longer ours, which is the
		 * for-ever-re-entering interrupt of bug 14.
		 */
		timer0_disarm();
		/* "No request is outstanding" is exactly what g.mpsl_owes
		 * tracks, so this is its authoritative clear - the backstop for
		 * any answer path that did not run. The re-place below sets it
		 * again on acceptance. */
		g.mpsl_owes = false;
		/*
		 * AND IT IS WHERE A REQUEST THAT WAS TOO EARLY GETS PLACED.
		 * mpsl_timeslot_request() answers -NRF_EAGAIN until exactly this
		 * point, and the arm that provoked it comes from the terminal of
		 * the operation that was using the timeslot - so "too early" is
		 * the normal case rather than the exception. See request_work_fn.
		 */
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
		/* THE COUNTER IS GUARDED AND THE CALL IS NOT, on both session
		 * signals. See SESSION_IDLE above for why that split is the
		 * right way round: the compare gate_release() leaves armed on
		 * purpose outlives the hand-back, so a disarm skipped because
		 * hw_held is already false would leave an interrupt armed on a
		 * peripheral that is about to stop being ours. */
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
		 * Both are programming errors in this file rather than
		 * conditions to handle, and MPSL asserts immediately after this
		 * returns - so the ONLY way to learn which of them happened is
		 * to record it here and print it from mpsl_assert_handle().
		 *
		 * They need opposite fixes and the assert location cannot tell
		 * them apart: OVERSTAYED says the end margin is too small,
		 * INVALID_RETURN says an action was returned from a signal that
		 * may not carry one. Guessing between them cost a round.
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
	 * THE LAST WORD ON WHAT MAY BE RETURNED, IN ONE PLACE.
	 *
	 * Every branch above decides what it WANTS; this decides what is LEGAL,
	 * and the two were being conflated. An action after the timeslot has
	 * been ended is the assert bug 14 describes, and it cannot be prevented
	 * branch by branch because the branch that is about to be wrong is
	 * whichever one loses a race it cannot see.
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
		/*
		 * Almost always CONFIG_MPSL_TIMESLOT_SESSION_COUNT still at its
		 * default of 0, which sizes the context array to nothing. The
		 * Kconfig help says so; this is what it looks like at runtime.
		 */
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
	 * THE FIX FOR TOTAL SILENCE, AND IT IS WORTH RECORDING WHAT IT LOOKED
	 * LIKE. Without this the core kept posting windows at
	 * `now + 168 us` - the hardware's own arm lead - and every one of them
	 * was nearer than the arbiter can physically grant, so gate_acquire()
	 * refused all of them. The dongle booted, answered every configuration
	 * command with NO_ERROR, opened its channel, and heard nothing at all;
	 * the direct build on the same board in the same minute heard thirteen
	 * messages from the room.
	 *
	 * The scheduler's arming pass is built on caps.min_arm_lead_us, so
	 * telling it the truth is the whole fix.
	 *
	 * THE HEAD MARGIN IS IN HERE TOO, and leaving it out was the second half
	 * of the same bug. What has to clear the arbiter's placement lead is the
	 * GRANT START, and that is HEAD_MARGIN_US earlier than the instant the
	 * caller named. The core plans against what this returns, so this has to
	 * be everything the gate needs - not just the part that is about MPSL.
	 *
	 * The backend adds its own ARM_LEAD_US on top; see
	 * radiant_radio_caps_get(). The two are additive rather than a maximum
	 * because they are sequential: the reservation has to be placed, and
	 * THEN the peripheral has to be programmed inside it.
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

	/*
	 * printk RATHER THAN LOG_*, AND THAT IS THE POINT OF IT.
	 *
	 * Deferred logging drops what it cannot keep up with, and the first
	 * thing an arbiter that refuses everything produces is a flood - so the
	 * diagnostics that would explain the refusal are the first casualty.
	 * That cost two debugging rounds: a three-shot LOG_ERR here never
	 * appeared, which read as "this function is never called" and is not
	 * what it meant. printk is synchronous and cannot be dropped, and the
	 * shot count keeps it affordable.
	 */
	/* Bring-up printks removed: with CONFIG_LOG_MODE_IMMEDIATE a printk blocks
	 * ~87 us PER CHARACTER on a 115200 console, so a 60-character line is 5 ms
	 * of synchronous blocking inside the arm path - far more than the 3 ms of
	 * lead the request was built with. The instrument was moving the thing it
	 * measured. See the header. */

	stats.acquires++;

	if (g.granted) {
		stats.acq_in_grant++;
		/*
		 * A GRANT IS ALREADY HELD, AND THIS IS WHAT follow_on_us WAS
		 * FOR.
		 *
		 * radiant_transfer.c arms the acknowledged-data reply from
		 * inside the receive callback, 1.56 ms before the frame must be
		 * on the air. There is no time to place a request, and none is
		 * needed: the air was reserved when the window that just fired
		 * was requested. Asking the arbiter again here - and being
		 * refused for being too near, which it would be - would make
		 * the whole reserve pointless, reserving air and then declining
		 * to use it.
		 *
		 * The test is whether the follow-on fits inside what we hold.
		 * If it does not, the reserve was too small, and refusing is
		 * the honest answer rather than overstaying - which asserts.
		 */
		/*
		 * AND IT MUST START INSIDE THE GRANT TOO, not merely end inside
		 * it. A window whose start is beyond grant_end would be
		 * programmed against air we do not have; the end test alone
		 * passes for it whenever grant_end is generous.
		 */
		if (t_from >= now &&
		    t_to + (radiant_time_t)follow_on_us +
				    (radiant_time_t)TAIL_MARGIN_US <=
			    g.grant_end) {
			g.release_wanted = false;
			return GATE_GRANTED;
		}
		/*
		 * IT DID NOT FIT - AND THAT IS NOT THE SAME AS BEING REFUSED.
		 *
		 * BUG 18, AND IT WAS FIFTY PERCENT OF EVERY TRACKED PACKET.
		 * This used to `return GATE_DENIED`, on the reasoning that the
		 * branch above is the follow-on reserve and a window that does
		 * not fit inside the reserve has missed it. That is true of a
		 * REPLY. It is false of everything else, and everything else is
		 * the common case:
		 *
		 * a tracked window's packet arrives mid-window; the receive
		 * callback arms the next tracked slot a quarter of a second out,
		 * which is the low-jitter path radiant_radio_hal.h's callback
		 * contract exists to permit; the timeslot that carried the
		 * packet is still live, because gate_release() runs from
		 * deliver_terminal() and that has not happened yet. So the arm
		 * arrived here, was asked whether 250 ms fits inside a 5.5 ms
		 * grant, and was refused. The core lost the slot; the next
		 * scheduler pump re-posted it onto the FOLLOWING one. Perfect
		 * alternation, and against a driven master it measured 50.6 %
		 * loss (exact) with the arbiter refusing NOTHING - blocked=0,
		 * placed==granted, every window MPSL was asked for granted.
		 *
		 * A request that starts after this grant ends is an ordinary
		 * future reservation and is placed like any other. Falling
		 * through does that, and it costs nothing that was working: a
		 * genuine acknowledged-data reply is ~1.56 ms away, which is
		 * inside PLACE_MIN_US, so it still gets its synchronous refusal
		 * from the too-near test below - the same answer by the road
		 * that describes it.
		 *
		 * The counter stays. It is now "arms that had to be placed
		 * rather than served from the reserve", which is a real property
		 * of the traffic and the thing to watch if the reserve is ever
		 * resized.
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
		 * g.pending is clear but MPSL is still holding the last
		 * request. Placing now costs an -NRF_EAGAIN round trip, leaves
		 * next_grant uncommitted, and re-arms the backstop - which is
		 * the spiral at DEADLINE_SLACK_US, one turn of it. Refuse here
		 * instead: the core learns it did not get the air one work-queue
		 * hop sooner and nothing is left half-placed behind it.
		 *
		 * Only reachable when the backstop beat MPSL's answer, so with
		 * the slack above this should stay near zero. It is counted, not
		 * assumed.
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
	 * Move an elastic request off whatever phase the last refusal found. The
	 * whole window slides rather than stretching, so the chunk is the length
	 * the caller asked for and only its instant changes - which for "as much
	 * as fits" work is nothing the core has an opinion about. See
	 * elastic_skew_us. Zero until something is actually refused, so an
	 * uncontended bench places exactly where it always did.
	 */
	if (elastic_skew_us != 0u && op != GATE_OP_TX &&
	    prio == RADIANT_GATE_PRIO_NORMAL) {
		start += (radiant_time_t)elastic_skew_us;
		end   += (radiant_time_t)elastic_skew_us;
	}
	/*
	 * THE RESERVATION IS LONGER THAN THE AIR IT COVERS, by exactly the time
	 * it takes to hand it back.
	 *
	 * `end` is the last instant the RADIO may be busy. The timeslot has to
	 * outlive that by END_MARGIN_US, because the compare that returns
	 * ACTION_END is placed END_MARGIN_US before the timeslot expires - so
	 * without this the grant would be given back while the window it was
	 * placed for still had END_MARGIN_US to run, and the receiver would be
	 * taken away mid-window. That reads at the core as a sensor that went
	 * quiet, which is the worst possible disguise for it.
	 */
	len = (uint64_t)(end - start) + (uint64_t)END_MARGIN_US;
	if (len < MPSL_TIMESLOT_LENGTH_MIN_US) {
		len = MPSL_TIMESLOT_LENGTH_MIN_US;
	}
	if (len > MPSL_TIMESLOT_LENGTH_MAX_US) {
		/*
		 * caps.max_window_us exists precisely so the scheduler never
		 * asks for this, and radiant_sched.c's window_cap() bounds
		 * every request by it. Reaching here means the two disagree -
		 * refused rather than silently shortened, because a grant
		 * quietly shorter than the request is what makes the sweep
		 * credit a dwell it never spent.
		 */
		stats.refused_too_long++;
		if (stats.refused_too_long <= 2u) {
			/* Both constants are UL in nrfxlib's own header, so they
			 * are cast rather than given %lu - this line is the one
			 * diagnostic for a refusal that otherwise looks like
			 * silence, and it should be right the one time it fires
			 * rather than carry a -Wformat warning into every build. */
			printk("gate: too long - %u us, max %u (window cap %u)\n",
			       (unsigned)len,
			       (unsigned)MPSL_TIMESLOT_LENGTH_MAX_US,
			       (unsigned)MAX_WINDOW_US);
		}
		return GATE_DENIED;
	}

	/*
	 * TOO NEAR TO PLACE AT ALL. The measured grant latency is ~1695 us, so
	 * a request whose start is nearer than PLACE_LEAD_US cannot be honoured
	 * by the arbiter whatever it decides.
	 *
	 * DENIED AND NOT ETIME. The instant is perfectly reachable for a
	 * backend that owns the radio; what is missing is the air. This is the
	 * synchronous refusal the acknowledged-data reply path needs - it arms
	 * from inside the receive callback with 1.56 ms to go, which is inside
	 * this bound by construction, so an ERG reply that lands while another
	 * stack holds the radio is refused at the call rather than lost.
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
		 * NO ANCHOR, SO THIS WINDOW CANNOT BE PLACED - and the answer
		 * is to go and get one rather than to ask for this window with
		 * EARLIEST.
		 *
		 * EARLIEST grants a timeslot as early as MPSL can manage, which
		 * is NOT the start the request was built around. Using it for a
		 * real window means programming the receiver for an instant
		 * that has no fixed relationship to the grant: measured, the
		 * one and only grant produced one FAILED operation
		 * (program_rx() answering ETIME) and the channel stopped
		 * searching. See bootstrap_anchor().
		 *
		 * The core is told DENIED, which is exactly what happened - the
		 * air was not obtained - and the bootstrap runs behind it, so
		 * the next window in a quarter of a second finds an anchor.
		 */
		stats.den_no_anchor++;
		bootstrap_anchor();
		return GATE_DENIED;
	} else {
		/*
		 * distance_us IS MEASURED FROM THE PREVIOUS TIMESLOT'S START.
		 * Not from now, not from an absolute time - and the anchor did
		 * not move if the last request was blocked, which is what keeps
		 * a run of denials from walking the schedule.
		 */
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
		/*
		 * PRIORITY_HIGH for tracked slots and transmits, NORMAL for the
		 * elastic work. It ranks us against OURSELVES and against
		 * nothing else - the SoftDevice Controller holds levels above
		 * anything an application may request - so this decides which
		 * of our own requests wins when two of ours collide, and
		 * nothing about the other stack.
		 */
		req.params.normal.priority =
			(prio == RADIANT_GATE_PRIO_HIGH)
				? MPSL_TIMESLOT_PRIORITY_HIGH
				: MPSL_TIMESLOT_PRIORITY_NORMAL;
		req.params.normal.distance_us = (uint32_t)(start - g.anchor);
		req.params.normal.length_us = (uint32_t)len;
	}

	/*
	 * STAGED, NOT WRITTEN THROUGH `g` - because this function now runs
	 * during a live grant as a matter of course, and these describe the
	 * NEXT one. See next_grant and commit_next_grant().
	 */
	next_grant.want_len_us = (uint32_t)len;
	next_grant.grant_end = end;
	/*
	 * Only the gap-filling classes grow themselves. A tracked window and a
	 * transmit have an end the core chose and the air must not be held past
	 * it; a scan chunk and an energy-detect sweep are exactly the "as much
	 * as fits" work ADR 0013 makes elastic.
	 */
	next_grant.extendable = (op != GATE_OP_TX) &&
				(prio == RADIANT_GATE_PRIO_NORMAL);

	/*
	 * THE ELASTIC CLASS ASKS SMALL AND GROWS; everything else asks for
	 * exactly what it needs, because a tracked slot's length is the window
	 * the core chose and there is nothing elastic about it.
	 *
	 * This is ADR 0013's shape and it is now measured working rather than
	 * intended: 15 scan chunks of a 94 200 us window each, 1596 extensions
	 * and not one refusal, the grant growing 1 ms at a time from 10 ms with
	 * the compare that grows it always END_MARGIN_US ahead of the end.
	 *
	 * It was disabled for one round while the timeslot could not be ended at
	 * all - a short grant under a long window is worse than a long one -
	 * and the note that said so has been replaced by this one rather than
	 * left to contradict it.
	 */
	if (next_grant.extendable && len > (uint64_t)elastic_initial_us) {
		len = elastic_initial_us;
	}
	next_grant.granted_len_us = (uint32_t)len;

	/*
	 * HANDED TO A THREAD, NOT CALLED FROM HERE.
	 *
	 * radiant_sched.c arms from wherever the previous operation completed,
	 * which is the RADIO interrupt at priority 0 - and from inside a
	 * timeslot, because the completion that provoked the arm happened
	 * inside one. mpsl_timeslot_request() is not callable from either. The
	 * only in-timeslot way to ask for the next slot is SIGNAL_ACTION_REQUEST
	 * from the signal callback, and that CLOSES the current timeslot to do
	 * it - handing the radio back and asking for it again with another
	 * stack free to land in the gap.
	 *
	 * So the request is built here, where the caller's numbers are, and
	 * placed there, where the call is legal. The core is told PENDING
	 * either way, which is the answer it would have got anyway: the outcome
	 * of a reservation is never known at the arm.
	 */
	g.pending = true;
	k_work_submit(&request_work);

	/*
	 * The backstop. Set past the point at which a grant would be useless -
	 * the operation's own start - PLUS DEADLINE_SLACK_US, because the
	 * original reasoning was measured without a second stack and does not
	 * survive one. See DEADLINE_SLACK_US.
	 */
	k_timer_start(&deadline,
		      K_USEC((uint32_t)(t_from - now) + DEADLINE_SLACK_US),
		      K_NO_WAIT);

	return GATE_PENDING;
}

void gate_release(void)
{
	if (!g.granted) {
		/* Nothing held. The abort path calls this without knowing, and
		 * a pending request is cancelled by the core's own terminal
		 * rather than here. */
		g.pending = false;
		k_timer_stop(&deadline);
		/*
		 * "Nothing held" is g.granted, and g.granted is the CORE's
		 * permission, not the hardware loan - so this branch is reached
		 * with the radio still ours in exactly the window this whole
		 * split exists for: after a previous gate_release() and before
		 * its compare has fired. Idempotent, so on the ordinary double
		 * release it does nothing at all; on the lost-compare case it is
		 * what stops radiant_radio_disable() writing the shared
		 * interrupt mask while the 802.15.4 driver's SUBSCRIBE_RXEN is
		 * still swapped out.
		 */
		gate_hand_back();
		return;
	}

	/*
	 * GIVE THE AIR BACK NOW, WHICH MEANS PROVOKING A SIGNAL TO DO IT FROM.
	 *
	 * A timeslot can only be ended by returning ACTION_END from a callback,
	 * and this runs in the RADIO interrupt. So the flag is set and the
	 * TIMER0 compare is moved to just ahead of the current count; MPSL
	 * delivers SIGNAL_TIMER0 and the END is returned from there.
	 *
	 * THE PROMPTNESS IS THE WHOLE POINT rather than tidiness. On an empty
	 * tracked window this is ~2 ms of follow_on reservation that we have
	 * just stopped needing, four times a second per sensor - and it is the
	 * difference between the reserve costing the other stack's SCHEDULER
	 * and costing the other stack's AIR.
	 */
	/*
	 * g.granted IS CLEARED HERE, NOT WHEN ACTION_END IS RETURNED, AND THE
	 * DIFFERENCE IS A WEDGED DONGLE.
	 *
	 * The timeslot does not actually end until the TIMER0 signal below is
	 * delivered and ACTION_END returned, which is microseconds away. Leaving
	 * g.granted true across that gap means a gate_acquire() in between takes
	 * the "a grant is already held" branch - and for a window that happens
	 * to end inside the old grant_end it returns GATE_GRANTED. The backend
	 * then programmes the compare and the (D)PPI for air we have just handed
	 * back, TASKS_RXEN fires with no timeslot behind it, the operation never
	 * completes, and the core's single operation slot is held for ever.
	 *
	 * That is not hypothetical: it is what this looked like on the bench.
	 * The dongle booted, answered every serial command, opened its channel,
	 * and then went silent - alive, with no fault and no log line, which is
	 * the exact signature the master-slot comment in radiant_channel.c
	 * describes for a different cause.
	 *
	 * Cleared first, so that any acquire between here and the END is offered
	 * the ordinary path: it places a NORMAL request, MPSL holds it until the
	 * session goes IDLE, and the window is placed a moment later.
	 */
	g.granted = false;
	g.release_wanted = true;

	/*
	 * BUG 9: A COMPARE WRITTEN BEHIND THE COUNTER NEVER FIRES, AND THIS ONE
	 * WAS WRITTEN TWO MICROSECONDS AHEAD OF IT.
	 *
	 * The old value here was +2 us. Reading CAPTURE1, writing CC0 and having
	 * MPSL's handler pick it up does not reliably happen inside two ticks of
	 * a 1 MHz timer - and when it does not, TIMER0 has already passed the
	 * compare, no event is generated, and the ONLY signal that would have
	 * ended this timeslot is gone. The grant then runs to its natural length
	 * with nothing to end it and MPSL asserts on the overstay.
	 *
	 * That is the residual assert: placed=21 granted=20 with 1596 clean
	 * extensions, i.e. not the extension chain and not the initial compare -
	 * the one compare in the file that is written relative to NOW.
	 *
	 * Two things fix it and both are needed. RELEASE_LEAD_US gives the write
	 * a realistic head start, and the re-read afterwards catches the case
	 * where even that lost the race: THE BACKSTOP COMPARE IS PUT BACK rather
	 * than left dangling, so there is always a compare in the future that
	 * ends this timeslot. Late is survivable; absent is not.
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
			/* Already inside the end margin. The compare that is
			 * armed is the right one and moving it can only lose. */
			nrf_timer_int_enable(MPSL_TIMER0,
					     NRF_TIMER_INT_COMPARE0_MASK);
		}
		/*
		 * THE RESTORE MAY NOT DEPEND ON A COMPARE ACTUALLY FIRING.
		 *
		 * Everything above is about getting a signal to return
		 * ACTION_END from, and that part is unchanged - MPSL will not
		 * end a timeslot any other way, so the compare STAYS ARMED and
		 * this is gate_hand_back() rather than timer0_disarm(). But
		 * handing the RADIO back is a different obligation, and it was
		 * riding on the same compare. If the compare is lost - which is
		 * exactly BUG 9's failure, and RELEASE_LEAD_US makes it rare
		 * rather than impossible - the timeslot still ends, by expiry,
		 * and SESSION_IDLE still arrives. The restore, though, would
		 * have been waiting on a signal that never came.
		 *
		 * It is not left to that backstop because the gap is unbounded
		 * in the only direction that matters: for as long as it lasts
		 * the 802.15.4 driver's SUBSCRIBE_RXEN is ours and its receiver
		 * is deaf. The core is finished with the grant by definition
		 * here - that is what gate_release() means, and the ~2 ms saved
		 * is the same promptness this function's own header argues for.
		 * The TIMER0 signal arriving a moment later still ends the
		 * timeslot and simply finds the radio already returned.
		 */
		stats.release_disarm++;
		gate_hand_back();
	}
}

bool gate_extend(uint32_t us)
{
	/*
	 * Extensions are driven from inside the signal callback, where the
	 * ACTION_EXTEND return lives - a request from here could not express
	 * one. The backend does not call this; it is in the interface because a
	 * RAIL gate's equivalent would be a function call rather than a return
	 * value, and the seam has to be expressible in both.
	 */
	ARG_UNUSED(us);
	return false;
}

#if defined(CONFIG_MPSL_ASSERT_HANDLER)
void mpsl_assert_handle(const char *const file, const uint32_t line)
{
	/*
	 * The two that matter, and both are this file's fault rather than
	 * conditions to survive:
	 *
	 *   OVERSTAYED  - a timeslot closed late. TAIL_MARGIN_US is too small.
	 *                 This is the failure the plan calls the most likely
	 *                 field failure of the whole design: a dongle that
	 *                 asserts once an hour under a busy Thread network.
	 *   INVALID     - an action returned from a low-priority signal.
	 *
	 * Printed with the counters, because "which of the two was it" is not
	 * answerable from the assert location alone.
	 */
	/*
	 * LOG_PANIC() FIRST, AND WITHOUT IT THIS HANDLER IS SILENT.
	 *
	 * printk is routed through the log subsystem (CONFIG_LOG_PRINTK=y is
	 * the default), and in deferred mode that needs a thread which is about
	 * to be halted - so the message describing the assert is dropped and
	 * the board simply stops with nothing said. LOG_PANIC() switches the
	 * subsystem to synchronous output for exactly this case.
	 *
	 * That silence cost several debugging rounds: halting the CPU over
	 * J-Link showed PC inside arch_system_halt() with IPSR = 0x1A, i.e. a
	 * panic raised from an interrupt - which is this function - while every
	 * console capture showed nothing at all.
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






