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

#ifndef RADIANT_TESTS_GATE_FAKE_MPSL_H_
#define RADIANT_TESTS_GATE_FAKE_MPSL_H_

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

/* ADR 0018 (d2): allow up to n sessions (1 by default - every older test sees
 * a single-session arbiter, so a d2 gate falls back to one session there),
 * send fake_mpsl_signal() to session `sess` (0 or 1; each answers to its own
 * id), or deliver one signal to a chosen session. */
void     fake_mpsl_set_max_sessions(uint32_t n);
void     fake_mpsl_target(uint32_t sess);
uint8_t  fake_mpsl_signal_s(uint32_t sess, uint32_t signal);
uint32_t fake_mpsl_last_session(void);
bool     fake_mpsl_is_open_s(uint32_t sess);
uint32_t fake_mpsl_requests_s(uint32_t sess);
const mpsl_timeslot_request_t *fake_mpsl_last_request_s(uint32_t sess);

bool     fake_mpsl_is_open(void);
uint32_t fake_mpsl_opens(void);
uint32_t fake_mpsl_closes(void);
uint32_t fake_mpsl_requests(void);   /* calls, accepted or not */
uint32_t fake_mpsl_accepted(void);   /* calls that returned 0 */
/* The last request MPSL was handed, accepted or not. Never NULL. */
const mpsl_timeslot_request_t *fake_mpsl_last_request(void);
/* The extension length asked for by the last ACTION_EXTEND. */
uint32_t fake_mpsl_last_extend_us(void);
/* The struct the gate's callback returned for the last signal. MPSL reads it
 * after the callback returns, so it must still hold the action then. */
const mpsl_timeslot_signal_return_param_t *fake_mpsl_last_return(void);
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
	uint32_t booking_due;
	/* ADR 0018 (d2): the last elastic lead the gate published, and how
	 * often it published one. */
	uint32_t elastic_lead_us;
	uint32_t elastic_lead_sets;
	/*
	 * Assert 69:108: radiant_nrf_gate_radio_irq_unpend(). irq_unpend counts
	 * calls; irq_unpend_cleared those that cleared a pending RADIO vector.
	 * The model is the real backend's: it clears only if on_grant() took the
	 * radio in this timeslot. The *_seq fields order the backend calls (one
	 * counter across on_grant_end and irq_unpend), and unpend_abort_calls is
	 * fake_radio_abort_calls() at the last unpend, so a test can prove the
	 * clear came after the hand-back and after every abort.
	 */
	uint32_t irq_unpend;
	uint32_t irq_unpend_cleared;
	/* radiant_nrf_gate_on_slot_start() calls: one per SIGNAL_START. */
	uint32_t slot_start;
	uint32_t grant_end_seq;
	uint32_t unpend_seq;
	uint32_t unpend_abort_calls;
	/* on_grant()'s place in the same order. */
	uint32_t grant_seq;
	/*
	 * CONFIG_RADIANT_NRF_LOW_POWER (the radiant.gate.lp scenario only; zero
	 * otherwise): radiant_nrf_gate_lp_slot_start()/_end(). The fake keeps the
	 * backend's one bit of state, whether its TIMER is running (lp_awake):
	 *   lp_start           slot_start calls; lp_last_r_start the last r_start
	 *   lp_start_awake     ... that found the TIMER still running (a missed
	 *                      release)
	 *   lp_start_seq       its place in the backend-call order
	 *   lp_end             slot_end calls; lp_end_backstop those with
	 *                      backstop true
	 *   lp_releases        slot_end calls that stopped a running TIMER;
	 *                      lp_backstop_releases those from a backstop
	 *   lp_end_seq         the last slot_end's place in the order
	 *   lp_end_abort_calls fake_radio_abort_calls() at the last slot_end
	 *   lp_end_outside     slot_end(false) calls made outside a timeslot
	 */
	bool     lp_awake;
	uint32_t lp_start;
	uint32_t lp_start_awake;
	uint64_t lp_last_r_start;
	uint32_t lp_start_seq;
	uint32_t lp_end;
	uint32_t lp_end_backstop;
	uint32_t lp_releases;
	uint32_t lp_backstop_releases;
	uint32_t lp_end_seq;
	uint32_t lp_end_abort_calls;
	uint32_t lp_end_outside;
	/*
	 * The grant generation (gate_grant_gen()) as the backend saw it: at the
	 * last on_grant() (must be live, odd), at the last on_grant_end() (must
	 * already be over, even), and every value on_grant() was handed, so a
	 * test can show no two grants shared one.
	 */
	uint32_t gen_at_grant;
	uint32_t gen_at_grant_end;
	uint32_t gen_grant_reused;
	/* Faults, zero on every path (assert_invariant() in test_gate.c): an
	 * on_grant() handed an even (over) generation, an on_grant_end() that
	 * found it still live. */
	uint32_t gen_dead_at_grant;
	uint32_t gen_live_at_grant_end;
};

void                              fake_backend_reset(void);
const struct fake_backend_counts *fake_backend(void);

/* The RADIO vector's NVIC pending bit, as the fake models it: set by a test
 * (a RADIO event latched while a signal ran), cleared only by an unpend in a
 * timeslot whose on_grant() took the radio. */
void fake_backend_radio_irq_pend(void);
bool fake_backend_radio_irq_pending(void);

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

#endif /* RADIANT_TESTS_GATE_FAKE_MPSL_H_ */
