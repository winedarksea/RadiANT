/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * THE SEAM: the one boundary a manufacturer implements to turn this reference
 * design into their treadmill.
 *
 * Read apps/hrm_ble/src/hrm_sensor.h and docs/hrm-reference-design.md §3
 * before this file. This is the same seam with one thing added, and the two
 * rules that make it safe are that seam's rules, restated here because a rule
 * stated only in the file it does not constrain is a rule the next author
 * never reads.
 *
 * ─── Rule 1: context. Push, not pull, and everything lands on one queue ────
 *
 * A real machine's belt speed comes from a tachometer edge and its stride
 * events from a deck load cell - both interrupts, both knowing the instant
 * they happened, neither able to touch a profile module safely. `profile_*` is
 * reached from exactly two contexts in this application:
 *
 *   1. the system workqueue, which advances the state and stages pages, and
 *   2. the radiant callback path - radiant_ant_on_message() -> load_next_page() ->
 *      radiant_profile_fec_tx_next() / radiant_profile_sdm_next().
 *
 * There is no lock in either. So treadmill_source_report_*() does NOT touch a
 * profile and does NOT touch treadmill_state: it stores one value atomically
 * and submits one work item, and the handler runs on context 1. An ISR-driven
 * source therefore adds no third context. Do not "simplify" a report_*()
 * function into a direct call.
 *
 * ─── Rule 2: a Kconfig `choice`, never a weak symbol ───────────────────────
 *
 * A weak default a manufacturer overrides is one signature typo away from
 * silently keeping the default - which here means a shipped treadmill
 * reporting a simulated runner while somebody stands on it. Registration plus
 * a `choice` makes that unrepresentable: exactly one arm is compiled, exactly
 * one treadmill_source_register() call is linked, and the boot log names which.
 * CONFIG_TREADMILL_SOURCE_CUSTOM compiles NONE of them and fails loudly, on
 * purpose, with no fallback to the simulator.
 *
 * ─── What this seam has that hrm_sensor.h does not: it is BIDIRECTIONAL ────
 *
 * A heart-rate strap is told nothing. A treadmill is: a controller sets grade
 * over FE-C page 51 or over the FTMS control point, and something has to move
 * an actuator. So the api has set_incline() and set_speed() and the workout
 * hooks, and each returns -ENOTSUP when the machine cannot do it. Every one of
 * them is behind the motion rule in treadmill_session.h; see "The control path
 * is not here" below.
 *
 * -ENOTSUP IS A FIRST-CLASS ANSWER AND NOT A FAILURE. A manual treadmill with
 * a fixed deck is a legitimate product; it advertises no Simulation-mode bit
 * in FE-C page 54, declares no Inclination Target Setting bit in FTMS's
 * Fitness Machine Feature, and answers any command that arrives anyway with a
 * page 71 "not supported" and an FTMS result code 0x02. Returning 0 and doing
 * nothing is the behaviour that makes a controller report the machine as
 * broken instead.
 *
 * ─── The one thing that is NOT here ────────────────────────────────────────
 *
 * There is no "get" of any kind. The source PUSHES what it measures and the
 * application owns the state; a getter would invite a second copy of the truth
 * and a second context reading it. See treadmill_state.h.
 */

#ifndef TREADMILL_SOURCE_H_
#define TREADMILL_SOURCE_H_

#include <stdbool.h>
#include <stdint.h>

struct treadmill_source;

struct treadmill_source_api {
	/*
	 * Bring the machine up. Required. Called once from main(), after the
	 * profiles are initialised and before either ANT+ channel opens, so a
	 * source that needs the radio quiet has it and a source that fails
	 * stops the node before it claims to be transmitting.
	 *
	 * 0 or a negative errno. A failure is logged and the node keeps
	 * running with a stopped belt, which is what a receiver sees as
	 * RADIANT_PROFILE_FEC_STATE_READY and zero speed.
	 */
	int (*start)(const struct treadmill_source *src);

	/* Optional. Nothing calls it yet; it exists so a source with a power
	 * state has somewhere to put it. */
	int (*stop)(const struct treadmill_source *src);

	/*
	 * Optional, called on the node's 100 ms tick. The slow, level-valued
	 * things: belt temperature, motor current, a console's own buttons.
	 * NOT where stride events come from - a stride detected on this tick
	 * carries the tick's timestamp, which is the drift bug hrm_sensor.h
	 * exists to prevent.
	 */
	void (*poll)(const struct treadmill_source *src);

	/*
	 * MOVE THE DECK. `centi_pct` is 0.01 % grade, already inside the
	 * machine range (CONFIG_TREADMILL_MIN/MAX_INCLINE_DECI, enforced in
	 * treadmill_state.c for every command path), which is itself inside
	 * what FE-C page 17 can report back (±100.00 %).
	 *
	 * Return 0 if the command was accepted (the actuator will get there in
	 * its own time - this is not a completion), -ENOTSUP if this machine
	 * cannot change incline at all, or another negative errno for a
	 * transient refusal (a fault, an interlock, a user override). The
	 * three answers map onto three DIFFERENT things the caller must say
	 * over the air, which is why they are three answers:
	 *
	 *   0        -> FE-C page 71 PASS,          FTMS result 0x01 Success
	 *   -ENOTSUP -> FE-C page 71 NOT_SUPPORTED, FTMS result 0x02
	 *   other    -> FE-C page 71 FAIL,          FTMS result 0x04 Operation
	 *                                           Failed
	 */
	int (*set_incline)(const struct treadmill_source *src,
			   int32_t centi_pct);

	/* MOVE THE BELT. `mm_s`, already at most
	 * CONFIG_TREADMILL_MAX_SPEED_CENTI_KPH; same three-answer contract. */
	int (*set_speed)(const struct treadmill_source *src, uint32_t mm_s);

	/*
	 * THE WORKOUT HOOKS, all optional: FTMS Start/Resume (0x07) and
	 * Stop/Pause (0x08). NULL answers -ENOTSUP, which FTMS reports as 0x02
	 * Op Code Not Supported - except workout_stop, whose absence falls back
	 * to set_speed(0) so a Stop can never be unsupported on a machine that
	 * can be told a speed at all.
	 *
	 * workout_pause CALLS THE SOURCE and the source decides what a pause
	 * is: most consoles stop the belt and hold the session's totals; some
	 * slow it to a walk. Once the machine IS paused it reports
	 * TREADMILL_EVT_PAUSED (and TREADMILL_EVT_RESUMED after a resume) -
	 * those reports, not the hook's return, move the session.
	 *
	 * Same three answers as set_incline(): 0 accepted, -ENOTSUP cannot,
	 * anything else a refusal (FTMS 0x04).
	 */
	int (*workout_start)(const struct treadmill_source *src);
	int (*workout_pause)(const struct treadmill_source *src);
	int (*workout_resume)(const struct treadmill_source *src);
	int (*workout_stop)(const struct treadmill_source *src);

	/*
	 * MAY A REMOTE CONTROLLER MOVE THIS MACHINE RIGHT NOW? Condition (a) of
	 * the motion rule (treadmill_session.h): true only while the safety key
	 * is present AND the user has enabled remote control at the console (or
	 * the machine has no such switch and the manufacturer has decided remote
	 * control is always on while the key is in).
	 *
	 * NULL MEANS NO. A source that does not implement this can be watched
	 * over both radios and stopped from either, and never started, sped up
	 * or tilted remotely. Called on the system workqueue; must not block.
	 */
	bool (*remote_motion_allowed)(const struct treadmill_source *src);
};

struct treadmill_source {
	/* Logged at boot, verbatim. A manufacturer reads their own driver's
	 * name there, or the reference simulator's, and there is deliberately
	 * no third answer. */
	const char *name;
	const struct treadmill_source_api *api;
	void *data;
};

/*
 * Register the one source. Call before main()'s treadmill_source_start() - a
 * SYS_INIT() at APPLICATION level, or the source's own init hook.
 *
 * EXACTLY ONE WINS. A second call returns -EEXIST and logs at error level
 * rather than replacing the first: two sources reporting would violate the
 * single-producer rule the atomic report words depend on, and "whichever
 * initialised last" is not a rule anybody could reason about.
 */
int treadmill_source_register(const struct treadmill_source *src);

const char *treadmill_source_name(void);

/* -ENODEV when nothing registered, which under
 * CONFIG_TREADMILL_SOURCE_CUSTOM is the expected failure for an integrator who
 * has not written their driver yet. */
int treadmill_source_start(void);
int treadmill_source_stop(void);
void treadmill_source_poll(void);

/*
 * ─── What "success" means ──────────────────────────────────────────────────
 *
 * A COMMAND RESULT REPORTS THAT THE DRIVER ACCEPTED THE COMMAND: the setter or
 * workout hook returned 0. It is not a completion and not a state. FTMS
 * answers 0x01 and FE-C page 71 PASS on acceptance, and nothing else.
 *
 * FTMS Training Status, the FTMS machine-status notifications and FE-C page
 * 16's state follow this source's STATE REPORTS and EVENTS (report_state(),
 * report_event()), never the command. A machine that accepts Start and never
 * starts the belt stays Idle on both radios, which is the truth.
 *
 * ─── The control path is not here ──────────────────────────────────────────
 *
 * The entry points that MOVE the machine - set_speed, set_incline and the
 * workout hooks - are reached only through src/treadmill_cmd.c, which puts
 * every call behind the one motion rule (treadmill_session.h). Their wrappers
 * are declared in treadmill_source_ctrl.h, which nothing else includes. A
 * radio that called the source directly would be a path around the safety
 * gate; keeping the declarations out of this header is what makes that a
 * compile error rather than a review comment.
 */

/*
 * The report path: ISR-safe by construction. One atomic store and one
 * k_work_submit() each; none of them touches a profile or treadmill_state.
 */

/* The belt is doing this now, in mm/s. */
void treadmill_source_report_speed(uint32_t mm_s);

/* The deck is at this now, in 0.01 % grade. A machine whose actuator has a
 * position sensor reports the MEASURED angle here; the simulator reports where
 * its ramp has got to. Either way this is what page 17 sends back, which is
 * what §8.8.4 asks to match the grade received. */
void treadmill_source_report_incline(int32_t centi_pct);

/* ONE STRIDE, i.e. two footfalls. An edge, not a rate - report it when it
 * happens and let the application derive cadence, for the reason
 * hrm_sensor.h's header gives about beats. */
void treadmill_source_report_stride(void);

/* Cadence in 1/16 strides per minute, for a source that measures a rate
 * directly rather than detecting edges. A source that reports strides need not
 * call this; the application derives cadence from the stride interval. */
void treadmill_source_report_cadence_16(uint32_t strides_min_16);

/* The machine's own state: TREADMILL_STATE_READY when nobody is on it,
 * IN_USE while a session runs, FINISHED when one ends.
 *
 * THIS IS THE FIELD THE WHOLE OF PHASE 6 RESTS ON. FE-C page 16 byte 7 carries
 * it, a dongle decodes it, and radiant_rules.c turns IN_USE into an occupancy
 * boolean - which is the machine's own report rather than an inference from an
 * accumulator, and is why a treadmill is the one case where "is it occupied"
 * needs no heuristic at all. A source that never calls this leaves the node
 * reporting READY forever and the gym dashboard reading zero. */
void treadmill_source_report_state(uint8_t fe_state);

/*
 * Things that HAPPENED at the machine, as opposed to levels it is at. Each is
 * delivered once, in order (a small queue, not a level word: two presses must
 * not coalesce into one).
 */
enum treadmill_source_event {
	/* The console's own buttons. LOCAL: Stop and Pause cancel any remote
	 * request still queued (treadmill_session.h's epoch). */
	TREADMILL_EVT_CONSOLE_START = 0,
	TREADMILL_EVT_CONSOLE_STOP,
	TREADMILL_EVT_CONSOLE_PAUSE,
	/* The machine is now paused / running again, after it carried out a
	 * remote workout_pause / workout_resume. */
	TREADMILL_EVT_PAUSED,
	TREADMILL_EVT_RESUMED,
	/* The safety key (tether clip). OUT latches SAFETY_STOP and revokes every
	 * remote controller; only IN plus a console Start or Stop clears it. A
	 * machine whose key cuts the motor in hardware must still report it:
	 * the radios cannot otherwise know why the belt stopped. */
	TREADMILL_EVT_SAFETY_KEY_OUT,
	TREADMILL_EVT_SAFETY_KEY_IN,
	/* The console's pairing control: open the BLE pairing window, or forget
	 * every bond (radiant_ble_pairing.h). Ignored without BLE. */
	TREADMILL_EVT_PAIRING_WINDOW,
	TREADMILL_EVT_FORGET_BONDS,
};

/* ISR-safe: one queue put and one k_work_submit(). Returns 0, or -ENOSPC when
 * the queue is full (the event is lost and logged; at human button rates the
 * queue never fills). */
int treadmill_source_report_event(enum treadmill_source_event evt);

/*
 * Implemented by the application (src/main.c), called ONLY from the report
 * work handler on the system workqueue.
 *
 * `strides` is how many stride events the handler is accounting for in one
 * pass - 1 in steady state, more only if several landed between submissions.
 * The count still advances by the right amount when it does, because the count
 * is the field a receiver differences and the one that survives a lost packet.
 */
void treadmill_node_deliver(uint32_t strides);

/* Implemented by the application, called from the event work handler on the
 * system workqueue, once per treadmill_source_report_event(), in order. */
void treadmill_node_event(enum treadmill_source_event evt);

/*
 * Which optional entry points this source implements, so the application can
 * derive what it advertises (FTMS Feature/Target Setting bits, FE-C page 54)
 * from what the driver can actually do. All false when nothing registered.
 */
struct treadmill_source_caps {
	bool set_speed;
	bool set_incline;
	bool workout_start;
	bool workout_pause;
	bool workout_resume;
	bool workout_stop;
	bool remote_motion_allowed;
};

void treadmill_source_get_caps(struct treadmill_source_caps *out);

/*
 * The last value of each LEVEL the source reported, sampled by
 * treadmill_node_deliver() on the workqueue.
 *
 * Levels and not events, which is why they are read rather than delivered:
 * missing an intermediate belt speed costs nothing, whereas missing a stride
 * costs a count that a receiver differences - so strides are the parameter and
 * these are the accessors. Calling them anywhere other than the workqueue is
 * not unsafe, merely pointless.
 */
uint32_t treadmill_source_last_speed(void);
int32_t treadmill_source_last_incline(void);
uint32_t treadmill_source_last_cadence_16(void);
uint8_t treadmill_source_last_state(void);

#endif /* TREADMILL_SOURCE_H_ */
