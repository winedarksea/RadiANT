/* SPDX-License-Identifier: Apache-2.0 */
/*
 * MPSL assert 69:108: a RADIO event that latched while the timeslot was being
 * ended is taken by MPSL after ACTION_END has closed the timeslot, and MPSL
 * asserts (observed on the bench: ipsr=RADIO_0, 18 us after END).
 *
 * The contract this suite pins:
 *   - every high-priority signal that returns ACTION_END calls
 *     radiant_nrf_gate_radio_irq_unpend() exactly once, after the radio was
 *     handed back (on_grant_end) and after every radiant_radio_abort() that
 *     signal made, so nothing radio-related follows it;
 *   - a timeslot whose on_grant() took the radio then has the vector cleared;
 *   - the low-priority hand-backs (BLOCKED, SESSION_IDLE, SESSION_CLOSED) and
 *     gate_shutdown() never call it: there the radio may already be the other
 *     stack's, and the pending bit its interrupt.
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

#define LEAD_US  500000u
#define WIN_US   5000u
#define CHUNK_US 40000u
/* The ext-skip policy: this many refused extensions switch it on. */
#define RUN_OFF  3u

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
	/* The bootstrap timeslot, handed straight back. */
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

ZTEST_SUITE(radiant_gate_radio_unpend, NULL, NULL, before, after, NULL);

/* Place a window and wait for the placement. */
static void place(uint8_t prio, uint32_t win_us)
{
	radiant_time_t from = radiant_radio_now() + LEAD_US;

	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, from, from + win_us, 0u, prio),
		      "the window was refused");
	k_msleep(WORK_MS);
}

/* A grant whose on_grant() took the radio. */
static void take_grant(uint8_t prio, uint32_t win_us)
{
	place(prio, win_us);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_true(fake_backend()->on_grant > 0u, "on_grant() was not called");
}

/* Deliver a high-priority signal that must end the timeslot, with a RADIO event
 * latched while it runs, and check the clear came last. */
static void end_and_check(uint32_t signal, bool aborts, const char *what)
{
	const uint32_t unpends = fake_backend()->irq_unpend;
	const uint32_t aborts_before = fake_radio_abort_calls();

	fake_backend_radio_irq_pend();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END, fake_mpsl_signal(signal),
		      "%s did not end the timeslot", what);

	zassert_equal(unpends + 1u, fake_backend()->irq_unpend,
		      "%s returned END without clearing the RADIO vector", what);
	zassert_false(fake_backend_radio_irq_pending(),
		      "%s left the RADIO vector pending for MPSL to take after "
		      "the timeslot closed (assert 69:108)", what);
	zassert_true(fake_backend()->unpend_seq > fake_backend()->grant_end_seq,
		     "%s cleared the vector before the radio was handed back", what);
	zassert_equal(fake_radio_abort_calls(), fake_backend()->unpend_abort_calls,
		      "%s aborted after clearing the vector", what);
	if (aborts) {
		zassert_true(fake_backend()->unpend_abort_calls > aborts_before,
			     "%s did not abort before the clear", what);
	}
	zassert_equal(0u, fake_mpsl_viol_end_when_ended(), NULL);
	zassert_equal(0u, fake_mpsl_viol_action_no_slot(), NULL);
	zassert_equal(0u, fake_mpsl_viol_low_prio_action(), NULL);
	k_msleep(WORK_MS);
}

ZTEST(radiant_gate_radio_unpend, test_timer0_end_after_release_clears_it)
{
	take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	gate_release();
	end_and_check(MPSL_TIMESLOT_SIGNAL_TIMER0, false, "TIMER0 after a release");
}

ZTEST(radiant_gate_radio_unpend, test_timer0_backstop_end_clears_it)
{
	/* No release: the fixed grant's own compare cuts the live operation. */
	take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	end_and_check(MPSL_TIMESLOT_SIGNAL_TIMER0, false, "the TIMER0 backstop");
}

ZTEST(radiant_gate_radio_unpend, test_radio_signal_end_clears_it)
{
	take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	fake_backend_release_in_radio_irq(true);
	end_and_check(MPSL_TIMESLOT_SIGNAL_RADIO, false, "the RADIO signal");
}

ZTEST(radiant_gate_radio_unpend, test_start_end_after_a_synchronous_completion_clears_it)
{
	place(RADIANT_GATE_PRIO_HIGH, WIN_US);
	fake_backend_release_in_grant(true);
	end_and_check(MPSL_TIMESLOT_SIGNAL_START, false, "START (completed in grant)");
	zassert_equal(1u, fake_backend()->on_grant, NULL);
}

ZTEST(radiant_gate_radio_unpend, test_extend_failed_and_ext_skip_clear_it_after_abort)
{
	/* A run of refused extensions, each ended by EXTEND_FAILED. */
	for (uint32_t i = 0u; i < RUN_OFF; i++) {
		take_grant(RADIANT_GATE_PRIO_NORMAL, CHUNK_US);
		zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
			      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
		end_and_check(MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED, true,
			      "EXTEND_FAILED");
	}
	/* Now skipping: the first compare ends the grant without asking. */
	take_grant(RADIANT_GATE_PRIO_NORMAL, CHUNK_US);
	end_and_check(MPSL_TIMESLOT_SIGNAL_TIMER0, true, "the ext-skip TIMER0");
}

ZTEST(radiant_gate_radio_unpend, test_a_timeslot_that_never_took_the_radio_keeps_the_bit)
{
	/*
	 * An orphan grant (the operation was forgotten before its START) is handed
	 * straight back. The gate still asks; the backend clears only for a
	 * timeslot that took the radio, so a pending bit here is not ours.
	 */
	const uint32_t unpends = fake_backend()->irq_unpend;

	place(RADIANT_GATE_PRIO_HIGH, WIN_US);
	gate_forget_pending();
	fake_backend_radio_irq_pend();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_equal(0u, fake_backend()->on_grant, NULL);
	zassert_equal(unpends + 1u, fake_backend()->irq_unpend, NULL);
	zassert_true(fake_backend_radio_irq_pending(),
		     "a pending bit was swallowed in a timeslot that never "
		     "took the radio");
	k_msleep(WORK_MS);
}

/* A low-priority signal delivered with the radio still on loan (the backstop
 * paths), or gate_shutdown(): the vector must be left alone. */
static void low_priority_leaves_it(uint32_t signal)
{
	take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	fake_backend_radio_irq_pend();
	if (signal == UINT32_MAX) {
		gate_shutdown();
	} else {
		zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
			      fake_mpsl_signal(signal), NULL);
	}
	zassert_true(fake_backend()->on_grant_end > 0u,
		     "the radio was not handed back");
	zassert_equal(0u, fake_backend()->irq_unpend,
		      "a low-priority hand-back cleared the RADIO vector, which "
		      "may already be the other stack's interrupt");
	zassert_true(fake_backend_radio_irq_pending(), NULL);
	k_msleep(WORK_MS);
}

ZTEST(radiant_gate_radio_unpend, test_blocked_hand_back_leaves_it)
{
	low_priority_leaves_it(MPSL_TIMESLOT_SIGNAL_BLOCKED);
}

ZTEST(radiant_gate_radio_unpend, test_session_idle_hand_back_leaves_it)
{
	low_priority_leaves_it(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE);
}

ZTEST(radiant_gate_radio_unpend, test_session_closed_hand_back_leaves_it)
{
	low_priority_leaves_it(MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED);
}

ZTEST(radiant_gate_radio_unpend, test_shutdown_hand_back_leaves_it)
{
	low_priority_leaves_it(UINT32_MAX);
}
