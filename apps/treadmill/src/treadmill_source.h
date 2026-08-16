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
 *   2. the radiant callback path - antr_on_message() -> load_next_page() ->
 *      profile_fec_tx_next() / profile_sdm_next().
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
 * an actuator. So the api has set_incline() and set_speed(), and BOTH return
 * -ENOTSUP when the machine cannot do it.
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
	 * PROFILE_FEC_STATE_READY and zero speed.
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
	 * MOVE THE DECK. `centi_pct` is 0.01 % grade, already clamped to what
	 * FE-C page 17 can report back (±100.00 %).
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

	/* MOVE THE BELT. `mm_s`, same three-answer contract. */
	int (*set_speed)(const struct treadmill_source *src, uint32_t mm_s);
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
 * The control path, called from the ANT+ and BLE sides. See the api's
 * set_incline() for the three-answer contract; these forward it unchanged, and
 * return -ENODEV when nothing is registered.
 *
 * BOTH radios call THESE, not the source directly, and not each other. That is
 * what makes a grade set over FE-C visible over FTMS and vice versa without
 * either module knowing the other exists.
 */
int treadmill_source_set_incline(int32_t centi_pct);
int treadmill_source_set_speed(uint32_t mm_s);

/* ---------------------------------------------------------------------------
 * The report path: ISR-safe by construction. One atomic store and one
 * k_work_submit() each; none of them touches a profile or treadmill_state.
 * ---------------------------------------------------------------------------
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
 * Implemented by the application (src/main.c), called ONLY from the report
 * work handler on the system workqueue.
 *
 * `strides` is how many stride events the handler is accounting for in one
 * pass - 1 in steady state, more only if several landed between submissions.
 * The count still advances by the right amount when it does, because the count
 * is the field a receiver differences and the one that survives a lost packet.
 */
void treadmill_node_deliver(uint32_t strides);

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
