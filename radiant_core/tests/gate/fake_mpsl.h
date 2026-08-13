/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fake_mpsl.h - the test-side control surface of the fake arbiter.
 *
 * radiant_radio_nrf_gate_mpsl.c talks to exactly two things: MPSL below it
 * and radiant_radio_nrf.c above it; this file is both, so the gate is the
 * only real code in the image and every signal sequence is a function call.
 *
 * Deliberately stricter than a stub, like fake_radio.c: the three rules MPSL
 * enforces by asserting (at a location that can't say which rule broke) are
 * enforced here as counters a test can read:
 *
 *   low_prio_action   an action returned from a signal delivered in
 *                     mpsl_low_priority_process() (BLOCKED, CANCELLED,
 *                     SESSION_IDLE, SESSION_CLOSED) - rule 3 of the gate's
 *                     own header.
 *   end_when_ended    ACTION_END returned when no timeslot is running (bug
 *                     14; the g.ended guard is what stops it).
 *   action_no_slot    any action returned outside a timeslot.
 *
 * A scenario that trips one has found a real fault; every test ends by
 * asserting all three are zero.
 */

#ifndef RADIANT_CORE_TESTS_GATE_FAKE_MPSL_H_
#define RADIANT_CORE_TESTS_GATE_FAKE_MPSL_H_

#include <stdbool.h>
#include <stdint.h>

#include <hal/nrf_timer.h>
#include <mpsl_timeslot.h>

#ifdef __cplusplus
extern "C" {
#endif

/* MPSL_TIMER0 as a register model. A scenario drives `counter` and
 * `counter_step` to aim the race in gate_release() and reads cc[] back to
 * assert where the ending compare was left. See fakes/hal/nrf_timer.h. */
extern fake_timer_t fake_mpsl_timer0;

/* ---------------------------------------------------------------------------
 * The arbiter
 * ---------------------------------------------------------------------------
 */

/* Session closed, every counter zeroed, the timer model back to a frozen
 * counter at zero. Does NOT touch the gate: call gate_shutdown() for that. */
void fake_mpsl_reset(void);

/* Deliver one signal and return the action the gate answered with. The
 * whole instrument: a scenario is a sequence of these. */
uint8_t fake_mpsl_signal(uint32_t signal);

/* Make the next `count` calls to mpsl_timeslot_request() answer rc rather
 * than 0. -NRF_EAGAIN is the interesting one: a "not yet", not a refusal -
 * bug 10 reported it as the latter. */
void fake_mpsl_force_request_rc(int32_t rc, uint32_t count);

/*
 * Answer the next `count` accepted requests from inside
 * mpsl_timeslot_request() itself, before it returns 0.
 *
 * Not an artificial sequence: BLOCKED and CANCELLED run in the same context
 * as mpsl_low_priority_process, so a request the arbiter can refuse without
 * deferring is refused right there, answer landing on the caller's own
 * stack. A contending stack is what makes that possible at all - BLE
 * bring-up never produced one, one OpenThread run produced it immediately.
 *
 * The only way to express bug 15 as a test: the fault is in the ORDER of
 * two statements in request_work_fn(), visible only to a signal delivered
 * between them.
 */
void fake_mpsl_answer_inline(uint32_t signal, uint32_t count);

bool     fake_mpsl_is_open(void);
uint32_t fake_mpsl_opens(void);
uint32_t fake_mpsl_closes(void);
uint32_t fake_mpsl_requests(void);   /* calls, accepted or not */
uint32_t fake_mpsl_accepted(void);   /* calls that returned 0 */
/* The last request MPSL was handed, accepted or not. Never NULL. */
const mpsl_timeslot_request_t *fake_mpsl_last_request(void);
/* The extension length asked for by the last ACTION_EXTEND. */
uint32_t fake_mpsl_last_extend_us(void);
/* True between a START signal and the ACTION_END that answered one. */
bool     fake_mpsl_in_timeslot(void);

uint32_t fake_mpsl_viol_low_prio_action(void);
uint32_t fake_mpsl_viol_end_when_ended(void);
uint32_t fake_mpsl_viol_action_no_slot(void);

/* ---------------------------------------------------------------------------
 * The other side: what radiant_radio_nrf.c would have done
 * ---------------------------------------------------------------------------
 */

struct fake_backend_counts {
	/* THE ONE THIS SUITE EXISTS FOR. Exactly one per granted timeslot, on
	 * every exit. */
	uint32_t on_grant_end;
	uint32_t on_grant;
	uint32_t on_denied;
	uint32_t finish_failed;
	uint32_t on_radio_irq;
};

void                              fake_backend_reset(void);
const struct fake_backend_counts *fake_backend(void);

/* Make radiant_nrf_gate_on_grant() call gate_release() before it returns.
 * The real shape of an operation completing synchronously inside its own
 * grant, and the only way to reach the `if (g.release_wanted)` branch of
 * SIGNAL_START. */
void fake_backend_release_in_grant(bool on);

/* The same for the RADIO signal: radiant_nrf_gate_on_radio_irq() completing the
 * operation is what makes SIGNAL_RADIO end the timeslot. */
void fake_backend_release_in_radio_irq(bool on);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_CORE_TESTS_GATE_FAKE_MPSL_H_ */
