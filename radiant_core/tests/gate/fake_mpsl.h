/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fake_mpsl.h - the test-side control surface of the fake arbiter.
 *
 * Provenance: original clean-room work, written against nrfxlib's
 * mpsl_timeslot.h and radiant_core/src/radiant_radio_nrf_gate.h. Nothing here
 * derives from sdk-ant or from MPSL's implementation.
 *
 * ---------------------------------------------------------------------------
 * What this is
 * ---------------------------------------------------------------------------
 *
 * radiant_radio_nrf_gate_mpsl.c talks to exactly two things: MPSL below it and
 * radiant_radio_nrf.c above it. This file is both of them, so the gate itself is
 * the only real code in the image and every signal sequence is a function call
 * rather than a bench session.
 *
 * IT IS DELIBERATELY STRICTER THAN A STUB, for the same reason fake_radio.c is.
 * The three rules MPSL enforces by ASSERTING - and asserting is all it does, at
 * a location that cannot say which rule was broken - are enforced here as
 * counters a test can read:
 *
 *   low_prio_action   an action returned from a signal delivered in
 *                     mpsl_low_priority_process() (BLOCKED, CANCELLED,
 *                     SESSION_IDLE, SESSION_CLOSED). Rule 3 of the gate's own
 *                     header; MPSL answers it with SIGNAL_INVALID_RETURN and
 *                     then an assert.
 *   end_when_ended    ACTION_END returned when no timeslot is running. This is
 *                     BUG 14 - `1:2 2:2` in the gate's trace - and the g.ended
 *                     guard is what stands between the gate and it.
 *   action_no_slot    any action at all returned outside a timeslot.
 *
 * A scenario that trips one of these has found a real fault, so every test in
 * the suite ends by asserting all three are zero.
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

/*
 * MPSL_TIMER0, as a register model. A scenario drives `counter` and
 * `counter_step` to aim the race in gate_release() and reads cc[] back to assert
 * where the compare that ends the timeslot was left. See fakes/hal/nrf_timer.h.
 */
extern fake_timer_t fake_mpsl_timer0;

/* ---------------------------------------------------------------------------
 * The arbiter
 * ---------------------------------------------------------------------------
 */

/* Session closed, every counter zeroed, the timer model back to a frozen
 * counter at zero. Does NOT touch the gate: call gate_shutdown() for that. */
void fake_mpsl_reset(void);

/*
 * Deliver one signal and return the action the gate answered with.
 *
 * This is the whole instrument. A scenario is a sequence of these, and the
 * assertion is on what came back and on how many times the radio was handed
 * over.
 */
uint8_t fake_mpsl_signal(uint32_t signal);

/*
 * Make the next `count` calls to mpsl_timeslot_request() answer rc rather than
 * 0. -NRF_EAGAIN is the interesting one: it is not a refusal, it is a "not
 * yet", and BUG 10 was reporting it as the former.
 */
void fake_mpsl_force_request_rc(int32_t rc, uint32_t count);

/*
 * ANSWER THE NEXT `count` ACCEPTED REQUESTS FROM INSIDE
 * mpsl_timeslot_request() ITSELF, before it returns 0.
 *
 * NOT AN ARTIFICIAL SEQUENCE. BLOCKED and CANCELLED are documented to run "in
 * the same context as mpsl_low_priority_process", and a request the arbiter can
 * refuse without deferring is refused right there - the call is already in that
 * context, so the answer comes back on the caller's own stack, inside the call.
 * A contending stack is what makes a synchronous refusal possible at all, which
 * is why the whole of BLE bring-up never produced one and one OpenThread run
 * produced it immediately.
 *
 * It is the ONLY way to express BUG 15 as a test, because the fault is not in
 * any signal handler: it is in the ORDER of two statements in request_work_fn(),
 * and only a signal delivered BETWEEN them can see it.
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

/*
 * Make radiant_nrf_gate_on_grant() call gate_release() before it returns.
 *
 * Not a convenience: it is the real shape of an operation that completes
 * synchronously inside its own grant (an arm that turns out to be unreachable
 * delivers its own terminal), and it is the only way to reach the
 * `if (g.release_wanted)` branch of SIGNAL_START.
 */
void fake_backend_release_in_grant(bool on);

/* The same for the RADIO signal: radiant_nrf_gate_on_radio_irq() completing the
 * operation is what makes SIGNAL_RADIO end the timeslot. */
void fake_backend_release_in_radio_irq(bool on);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_CORE_TESTS_GATE_FAKE_MPSL_H_ */
