/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Three hardenings from the 69:108 review, each pinned here:
 *
 *   (a) A low-priority signal returns a struct the zero-latency invocation
 *       for the same session never writes. MPSL reads the returned struct
 *       after the callback returns; if it were the session's own rv, a
 *       zero-latency END or EXTEND written in between would be handed to MPSL
 *       as the low-priority signal's action, which asserts.
 *   (b) Whether a timeslot took the radio is forgotten at every SIGNAL_START,
 *       so a timeslot that ended without our high-priority END cannot make a
 *       later bootstrap or orphan timeslot clear the RADIO vector.
 *   (c) Outside a high-priority signal, MPSL_TIMER0 is written only while a
 *       compare we armed is still armed: a low-priority backstop after a normal
 *       END leaves MPSL's timer alone, while a lost release compare is still
 *       disarmed.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <mpsl_timeslot.h>
#include <nrf_errno.h>

#include "radiant_radio_nrf_gate.h"

#include "../../fake_radio.h"
#include "../fake_mpsl.h"
#include "../fake_swi.h"

#define WORK_MS 20
#define LEAD_US 500000u
#define WIN_US  5000u

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	gate_shutdown();
	k_msleep(WORK_MS);
	fake_radio_bring_up();
	fake_mpsl_reset();
	fake_backend_reset();
	fake_swi_reset();
	zassert_equal(RADIANT_RADIO_OK_RC, gate_init(), NULL);
	k_msleep(WORK_MS);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	k_msleep(WORK_MS);
	fake_backend_reset();
}

static void after(void *fixture)
{
	ARG_UNUSED(fixture);
	gate_shutdown();
	k_msleep(WORK_MS);
}

ZTEST_SUITE(radiant_gate_hardening, NULL, NULL, before, after, NULL);

static void place(void)
{
	radiant_time_t from = radiant_radio_now() + LEAD_US;

	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, from, from + WIN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH),
		      "the window was refused");
	k_msleep(WORK_MS);
}

/* A grant whose on_grant() took the radio, with its compare armed. */
static void take_grant(void)
{
	place();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_true((fake_mpsl_timer0.inten & NRF_TIMER_INT_COMPARE0_MASK) != 0u,
		     "the grant's compare was not armed");
}

/* (a) ------------------------------------------------------------------ */

ZTEST(radiant_gate_hardening, test_a_low_priority_return_survives_a_zero_latency_end)
{
	const mpsl_timeslot_signal_return_param_t *hi;
	const mpsl_timeslot_signal_return_param_t *lo;

	take_grant();
	hi = fake_mpsl_last_return();

	/* A low-priority signal for the same session. MPSL has its pointer
	 * but has not read it yet ... */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE), NULL);
	lo = fake_mpsl_last_return();

	/* ... when the zero-latency callback for the same session ends the
	 * timeslot and writes END into its own return struct. */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);

	zassert_not_equal(hi, lo,
			  "a low-priority signal returned the session's own rv");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE, lo->callback_action,
		      "the low-priority signal's return now carries a "
		      "zero-latency action, which MPSL asserts on");
	k_msleep(WORK_MS);
}

/* (b) ------------------------------------------------------------------ */

ZTEST(radiant_gate_hardening, test_b_a_slot_without_our_end_does_not_lend_the_next)
{
	/* A timeslot that took the radio and ended without our high-priority
	 * END: SESSION_IDLE is the backstop that hands it back. */
	take_grant();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE), NULL);
	k_msleep(WORK_MS);

	/* The next timeslot is an orphan that never takes the radio. A
	 * pending RADIO interrupt then is not ours and must survive. */
	place();
	gate_forget_pending();
	fake_backend_radio_irq_pend();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_true(fake_backend()->slot_start > 0u,
		     "SIGNAL_START did not tell the backend a timeslot began");
	zassert_true(fake_backend_radio_irq_pending(),
		     "a stale 'radio taken' from an earlier timeslot cleared a "
		     "RADIO interrupt in one that never took the radio");
	k_msleep(WORK_MS);
}

/* (c) ------------------------------------------------------------------ */

/* End a grant normally: release, then the compare's TIMER0 END. */
static void normal_end(void)
{
	take_grant();
	gate_release();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	zassert_true((fake_mpsl_timer0.inten & NRF_TIMER_INT_COMPARE0_MASK) == 0u,
		     NULL);
}

static void backstop_leaves_timer0(uint32_t signal)
{
	uint32_t disables;

	normal_end();
	/* The timer is MPSL's again: model MPSL arming its own compare. */
	fake_mpsl_timer0.inten |= NRF_TIMER_INT_COMPARE0_MASK;
	fake_mpsl_timer0.event_compare[0] = true;
	disables = fake_mpsl_timer0.int_disables;

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE, fake_mpsl_signal(signal),
		      NULL);

	zassert_equal(disables, fake_mpsl_timer0.int_disables,
		      "a low-priority backstop wrote MPSL_TIMER0 with no compare "
		      "of ours armed");
	zassert_true((fake_mpsl_timer0.inten & NRF_TIMER_INT_COMPARE0_MASK) != 0u,
		     "MPSL's own compare interrupt was disabled");
	zassert_true(fake_mpsl_timer0.event_compare[0],
		     "MPSL's own compare event was cleared");
	k_msleep(WORK_MS);
}

ZTEST(radiant_gate_hardening, test_c_session_idle_after_end_leaves_timer0)
{
	backstop_leaves_timer0(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE);
}

ZTEST(radiant_gate_hardening, test_c_session_closed_after_end_leaves_timer0)
{
	backstop_leaves_timer0(MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED);
}

ZTEST(radiant_gate_hardening, test_c_a_lost_release_compare_is_still_disarmed)
{
	/* The case the backstop was written for: gate_release() armed the
	 * compare and returned the radio, and the compare never fired. */
	take_grant();
	gate_release();
	zassert_true((fake_mpsl_timer0.inten & NRF_TIMER_INT_COMPARE0_MASK) != 0u,
		     NULL);
	fake_mpsl_timer0.int_disable_zli_depth = 0u;

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE), NULL);

	zassert_true((fake_mpsl_timer0.inten & NRF_TIMER_INT_COMPARE0_MASK) == 0u,
		     "SESSION_IDLE left a lost compare of ours armed");
	zassert_true(fake_mpsl_timer0.int_disable_zli_depth > 0u,
		     "the disarm was not masked against the zero-latency callback");
	k_msleep(WORK_MS);
}

ZTEST(radiant_gate_hardening, test_c_a_high_priority_end_still_disarms_unconditionally)
{
	uint32_t disables = fake_mpsl_timer0.int_disables;

	/* An orphan timeslot never arms a compare, yet inside our timeslot the
	 * timer is ours, so the hand-back still clears COMPARE0 as before. */
	place();
	gate_forget_pending();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_equal(disables + 1u, fake_mpsl_timer0.int_disables,
		      "the in-timeslot disarm no longer runs");
	k_msleep(WORK_MS);
}
