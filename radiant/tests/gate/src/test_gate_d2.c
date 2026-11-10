/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ADR 0018 candidate (d2): the gate with two timeslot sessions and bookings.
 * docs/adr18-eval/d2.md.
 *
 * What these pin, each a property the design rests on:
 *   - a booking is placed in the session the scan is NOT using, while the
 *     scan's grant is live (Phase 0c F2 layout A), and the arm it was for is
 *     staged behind it with no second request;
 *   - every refusal of a booking reaches the core only as a synchronous
 *     refusal of that arm, and a refused scan extension reaches no booking;
 *   - a grant on one session is never torn down by the other session's
 *     low-priority signals (they share MPSL_TIMER0 and the radio);
 *   - a booking's grant with nothing staged is handed straight back without
 *     the backend, so the other session's staged op is never programmed into
 *     the wrong grant;
 *   - the elastic lead backs off on an elastic BLOCKED and recovers on grants;
 *   - with one session (the arbiter refuses the second) the gate is master's.
 *
 * Built against the same fakes as test_gate.c. The suite's before() asks the
 * fake arbiter for two sessions; test_gate.c's never does, so every older case
 * runs against the d2 gate in its single-session fallback.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <mpsl_timeslot.h>
#include <nrf_errno.h>

#include "radiant_radio_nrf_gate.h"

#include "../../fake_radio.h"
#include "../fake_mpsl.h"
#include "../fake_swi.h"
#include "../gate_probe.h"

#if defined(CONFIG_RADIANT_GATE_MPSL_D2)

#define WORK_MS 20

#define ROLE_NONE 0u
#define ROLE_OP   1u
#define ROLE_BOOK 2u

/* A scan chunk that starts well ahead, and a tracked window behind it. Both a
 * long way out, so no backstop (t_from - now + 20 ms) fires mid-test. */
#define SCAN_LEAD_US   400000u
#define SCAN_LEN_US    60000u
#define TRACK_AFTER_US 70000u   /* tracked window's t_from after the scan's */
#define TRACK_LEN_US   1000u

static struct gate_probe p;

static void d2_before(void *fixture)
{
	ARG_UNUSED(fixture);

	gate_shutdown();
	k_msleep(WORK_MS);

	fake_radio_bring_up();
	fake_mpsl_reset();
	fake_backend_reset();
	fake_swi_reset();
	gate_probe_reset_elastic();
	fake_mpsl_set_max_sessions(2u);

	zassert_equal(RADIANT_RADIO_OK_RC, gate_init(), "gate_init failed");
	zassert_true(fake_mpsl_is_open_s(0u) && fake_mpsl_is_open_s(1u),
		     "both sessions must be open");
	k_msleep(WORK_MS);
	/* Each session spends one EARLIEST slot on its anchor. */
	zassert_equal(1u, fake_mpsl_requests_s(0u), NULL);
	zassert_equal(1u, fake_mpsl_requests_s(1u), NULL);
	zassert_equal(MPSL_TIMESLOT_REQ_TYPE_EARLIEST,
		      fake_mpsl_last_request_s(1u)->request_type, NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal_s(1u, MPSL_TIMESLOT_SIGNAL_START), NULL);
	gate_probe_read(&p);
	zassert_equal(2u, p.n_open, NULL);
	zassert_true(p.d2_on, NULL);
	zassert_true(p.s_anchor_valid[0] && p.s_anchor_valid[1], NULL);
}

static void d2_after(void *fixture)
{
	ARG_UNUSED(fixture);
	gate_shutdown();
	k_msleep(WORK_MS);
	fake_mpsl_set_max_sessions(1u);
	fake_mpsl_target(0u);
	gate_probe_reset_elastic();
}

ZTEST_SUITE(radiant_gate_d2, NULL, NULL, d2_before, d2_after, NULL);

static void assert_invariant(void)
{
	gate_probe_read(&p);
	zassert_equal(p.granted, p.grant_end_calls, "%u granted, %u hand-backs",
		      p.granted, p.grant_end_calls);
	zassert_false(p.hw_held, "the radio is still on loan");
	zassert_equal(p.grant_end_calls, fake_backend()->on_grant_end, NULL);
	zassert_equal(0u, fake_mpsl_viol_low_prio_action(), NULL);
	zassert_equal(0u, fake_mpsl_viol_end_when_ended(), NULL);
	zassert_equal(0u, fake_mpsl_viol_action_no_slot(), NULL);
	zassert_equal(0u, p.overstayed + p.invalid_return + p.unknown_signal, NULL);
}

/* A scan chunk placed and granted. Returns the scan's t_from. */
static radiant_time_t scan_granted(void)
{
	radiant_time_t from = radiant_radio_now() + SCAN_LEAD_US;

	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, from, from + SCAN_LEN_US, 0u,
				   RADIANT_GATE_PRIO_NORMAL), NULL);
	k_msleep(WORK_MS);
	zassert_equal(0u, fake_mpsl_last_session(), "the scan went to session 0");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_START), NULL);
	gate_probe_read(&p);
	zassert_true(p.granted_flag && p.hw_held, NULL);
	return from;
}

ZTEST(radiant_gate_d2, test_d2_a_booking_goes_to_the_free_session_and_serves_its_arm)
{
	radiant_time_t scan = scan_granted();
	radiant_time_t t = scan + TRACK_AFTER_US;
	uint32_t       req0 = fake_mpsl_requests_s(0u);
	uint32_t       req1 = fake_mpsl_requests_s(1u);

	/* Booked while the scan's grant is live: F2 layout A. */
	zassert_equal(GATE_BOOK_OK, gate_book(t, t + TRACK_LEN_US, 0u), NULL);
	k_msleep(WORK_MS);
	zassert_equal(req1 + 1u, fake_mpsl_requests_s(1u),
		      "the booking was not placed in the other session");
	zassert_equal(req0, fake_mpsl_requests_s(0u), NULL);
	zassert_equal(MPSL_TIMESLOT_REQ_TYPE_NORMAL,
		      fake_mpsl_last_request_s(1u)->request_type, NULL);
	zassert_equal(MPSL_TIMESLOT_PRIORITY_HIGH,
		      fake_mpsl_last_request_s(1u)->params.normal.priority, NULL);
	/* The booked timeslot is the fixed class's: window + 250 head + 250
	 * tail + 750 end margin. */
	zassert_equal(TRACK_LEN_US + 1250u,
		      fake_mpsl_last_request_s(1u)->params.normal.length_us, NULL);
	gate_probe_read(&p);
	zassert_equal(ROLE_BOOK, p.role[1], NULL);
	zassert_true(p.granted_flag, "booking must not disturb the live grant");

	/* The scan ends on its own close and hands back. */
	gate_release();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_SESSION_IDLE), NULL);

	/* The tracked window is armed into its booking: no new request. */
	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t, t + TRACK_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);
	k_msleep(WORK_MS);
	zassert_equal(req1 + 1u, fake_mpsl_requests_s(1u), "a second request was placed");
	gate_probe_read(&p);
	zassert_equal(1u, p.book_hit, NULL);
	zassert_equal(1u, p.op_sess, NULL);
	zassert_true(p.pending, NULL);

	/* Its grant arrives and is programmed. */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal_s(1u, MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_equal(2u, fake_backend()->on_grant, "the booked op was not programmed");
	gate_probe_read(&p);
	zassert_true(p.granted_flag, NULL);
	zassert_false(p.extendable, NULL);
	zassert_equal(750u, p.end_margin_us, NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal_s(1u, MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	assert_invariant();
}

ZTEST(radiant_gate_d2, test_d2_a_refused_booking_refuses_only_its_own_arm)
{
	radiant_time_t t = radiant_radio_now() + SCAN_LEAD_US;

	zassert_equal(GATE_BOOK_OK, gate_book(t, t + TRACK_LEN_US, 0u), NULL);
	k_msleep(WORK_MS);
	gate_probe_read(&p);
	zassert_equal(0u, fake_mpsl_last_session(), "the free session is 0 here");
	/* MPSL refuses it at placement. Nobody is waiting, so nobody is told. */
	(void)fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_BLOCKED);
	k_msleep(WORK_MS);
	gate_probe_read(&p);
	zassert_equal(1u, p.book_blocked, NULL);
	zassert_true(p.book_failed[0], NULL);
	zassert_equal(0u, fake_backend()->on_denied, "a booking refusal reached the core");

	/* The arm it was for is refused synchronously, as a refused fresh request
	 * for that window would have been. */
	zassert_equal(GATE_DENIED,
		      gate_acquire(GATE_OP_RX, t, t + TRACK_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);
	gate_probe_read(&p);
	zassert_equal(1u, p.book_denied, NULL);
	zassert_equal(ROLE_NONE, p.role[0], "the session is free again");
	assert_invariant();
}

ZTEST(radiant_gate_d2, test_d2_a_booking_granted_with_nothing_staged_bypasses_the_backend)
{
	radiant_time_t t = radiant_radio_now() + SCAN_LEAD_US;

	zassert_equal(GATE_BOOK_OK, gate_book(t, t + TRACK_LEN_US, 0u), NULL);
	k_msleep(WORK_MS);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_START),
		      "an unclaimed booking's grant must be handed straight back");
	gate_probe_read(&p);
	zassert_equal(1u, p.book_early, NULL);
	zassert_equal(0u, fake_backend()->on_grant,
		      "the backend was asked to programme a grant nobody owns");
	assert_invariant();
}

ZTEST(radiant_gate_d2, test_d2_the_other_sessions_signals_never_tear_down_a_live_grant)
{
	radiant_time_t t;

	(void)scan_granted();
	t = radiant_radio_now() + SCAN_LEAD_US + TRACK_AFTER_US;
	zassert_equal(GATE_BOOK_OK, gate_book(t, t + TRACK_LEN_US, 0u), NULL);
	k_msleep(WORK_MS);

	/* Session 1 is refused and goes idle while session 0's grant runs. */
	(void)fake_mpsl_signal_s(1u, MPSL_TIMESLOT_SIGNAL_BLOCKED);
	(void)fake_mpsl_signal_s(1u, MPSL_TIMESLOT_SIGNAL_SESSION_IDLE);
	gate_probe_read(&p);
	zassert_true(p.granted_flag, "session 1's IDLE cleared session 0's grant");
	zassert_true(p.hw_held, "session 1's IDLE took session 0's radio");
	zassert_true((fake_mpsl_timer0.inten & NRF_TIMER_INT_COMPARE0_MASK) != 0u,
		     "session 1's IDLE disarmed the compare that ends session 0's grant");
	zassert_equal(0u, p.late_disarm, NULL);

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED), NULL);
	k_msleep(WORK_MS);
	assert_invariant();
}

ZTEST(radiant_gate_d2, test_d2_a_refused_scan_extension_costs_the_booked_slot_nothing)
{
	radiant_time_t scan = scan_granted();
	radiant_time_t t = scan + TRACK_AFTER_US;

	zassert_equal(GATE_BOOK_OK, gate_book(t, t + TRACK_LEN_US, 0u), NULL);
	k_msleep(WORK_MS);
	/* The scan's extension is refused where the booking sits (F2 A). */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED), NULL);
	k_msleep(WORK_MS);
	gate_probe_read(&p);
	zassert_equal(ROLE_BOOK, p.role[1], "the extension refusal touched the booking");
	zassert_false(p.book_failed[1], NULL);

	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t, t + TRACK_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal_s(1u, MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal_s(1u, MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	gate_probe_read(&p);
	zassert_equal(1u, p.book_hit, NULL);
	assert_invariant();
}

ZTEST(radiant_gate_d2, test_d2_an_aborted_booked_op_re_arms_into_the_same_booking)
{
	radiant_time_t t = radiant_radio_now() + SCAN_LEAD_US;
	uint32_t       req;

	zassert_equal(GATE_BOOK_OK, gate_book(t, t + TRACK_LEN_US, 0u), NULL);
	k_msleep(WORK_MS);
	req = fake_mpsl_requests();
	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t, t + TRACK_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);
	/* The core tears it down before it opens (a rebuild) ... */
	gate_forget_pending();
	gate_probe_read(&p);
	zassert_false(p.pending, NULL);
	zassert_equal(ROLE_BOOK, p.role[0], "the placed request was forgotten");
	/* ... and re-arms the same window: it rejoins the booking. */
	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t, t + TRACK_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);
	k_msleep(WORK_MS);
	zassert_equal(req, fake_mpsl_requests(), "a fresh request was placed");
	gate_probe_read(&p);
	zassert_equal(2u, p.book_hit, NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_equal(1u, fake_backend()->on_grant, NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	assert_invariant();
}

ZTEST(radiant_gate_d2, test_d2_a_booking_needs_a_full_lead_and_a_free_session)
{
	radiant_time_t now = radiant_radio_now();
	radiant_time_t t;

	/* 5 ms of notice is a fresh-request lead the other stack was swept to
	 * refuse (bug 26); a booking exists to give it a full one. */
	zassert_equal(GATE_BOOK_EDENIED, gate_book(now + 5000u, now + 6000u, 0u), NULL);
	gate_probe_read(&p);
	zassert_equal(1u, p.book_near, NULL);

	/* Two bookings fill both sessions; a third finds none. */
	t = now + SCAN_LEAD_US;
	zassert_equal(GATE_BOOK_OK, gate_book(t, t + TRACK_LEN_US, 0u), NULL);
	zassert_equal(GATE_BOOK_OK,
		      gate_book(t + 20000u, t + 20000u + TRACK_LEN_US, 0u), NULL);
	zassert_equal(GATE_BOOK_EBUSY,
		      gate_book(t + 40000u, t + 40000u + TRACK_LEN_US, 0u), NULL);
	/* Asking again for one already booked is not a second booking. */
	zassert_equal(GATE_BOOK_OK, gate_book(t, t + TRACK_LEN_US, 0u), NULL);
	gate_probe_read(&p);
	zassert_equal(2u, p.book_placed, NULL);
	zassert_equal(1u, p.book_busy, NULL);
	/* And with both sessions held, an ordinary acquire finds none either. */
	zassert_equal(GATE_DENIED,
		      gate_acquire(GATE_OP_RX, t + 80000u, t + 80000u + TRACK_LEN_US,
				   0u, RADIANT_GATE_PRIO_HIGH), NULL);
	gate_probe_read(&p);
	zassert_equal(1u, p.den_nosess, NULL);
	k_msleep(WORK_MS);
	assert_invariant();
}

ZTEST(radiant_gate_d2, test_d2_the_elastic_lead_is_short_backs_off_and_recovers)
{
	radiant_time_t now = radiant_radio_now();

	gate_probe_read(&p);
	zassert_equal(CONFIG_RADIANT_GATE_MPSL_D2_SCAN_LEAD_US, p.el_lead_us, NULL);
	zassert_equal(CONFIG_RADIANT_GATE_MPSL_D2_SCAN_LEAD_US + 250u,
		      fake_backend()->elastic_lead_us,
		      "the published lead must include the head margin");

	/* A tracked window at 3 ms of notice is too near; a scan chunk is not. */
	zassert_equal(GATE_DENIED,
		      gate_acquire(GATE_OP_RX, now + 3000u, now + 4000u, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);
	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, now + 3000u, now + 43000u, 0u,
				   RADIANT_GATE_PRIO_NORMAL), NULL);
	k_msleep(5);
	/* Refused: the next one gives twice the notice. */
	(void)fake_mpsl_signal_s(fake_mpsl_last_session(), MPSL_TIMESLOT_SIGNAL_BLOCKED);
	k_msleep(WORK_MS);
	gate_probe_read(&p);
	zassert_equal(2u * CONFIG_RADIANT_GATE_MPSL_D2_SCAN_LEAD_US, p.el_lead_us, NULL);
	zassert_equal(2u * CONFIG_RADIANT_GATE_MPSL_D2_SCAN_LEAD_US + 250u,
		      fake_backend()->elastic_lead_us, NULL);

	/* A granted elastic request gives 250 us back. */
	now = radiant_radio_now();
	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, now + SCAN_LEAD_US,
				   now + SCAN_LEAD_US + SCAN_LEN_US, 0u,
				   RADIANT_GATE_PRIO_NORMAL), NULL);
	k_msleep(WORK_MS);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal_s(fake_mpsl_last_session(),
					 MPSL_TIMESLOT_SIGNAL_START), NULL);
	gate_probe_read(&p);
	zassert_equal(2u * CONFIG_RADIANT_GATE_MPSL_D2_SCAN_LEAD_US - 250u,
		      p.el_lead_us, NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal_s(fake_mpsl_last_session(),
					 MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED), NULL);
	k_msleep(WORK_MS);
	assert_invariant();
}

/* Beside a stack that refuses every extension (OpenThread: ext=0/3470 in one
 * run), the d2 Thread runs halted on MPSL ASSERT 69:108 right after an
 * EXTEND_FAILED. After three refusals in a row the gate stops asking: the next
 * elastic grants end at their first compare exactly as a refused extension
 * would have ended them, with no ACTION_EXTEND. */
ZTEST(radiant_gate_d2, test_d2_refused_extensions_are_not_asked_for_again)
{
	for (uint32_t i = 0u; i < 4u; i++) {
		radiant_time_t from = radiant_radio_now() + SCAN_LEAD_US;
		uint8_t        act;

		zassert_equal(GATE_PENDING,
			      gate_acquire(GATE_OP_RX, from, from + SCAN_LEN_US, 0u,
					   RADIANT_GATE_PRIO_NORMAL), NULL);
		k_msleep(WORK_MS);
		zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
			      fake_mpsl_signal_s(fake_mpsl_last_session(),
						 MPSL_TIMESLOT_SIGNAL_START), NULL);
		act = fake_mpsl_signal_s(fake_mpsl_last_session(),
					 MPSL_TIMESLOT_SIGNAL_TIMER0);
		if (i < 3u) {
			zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND, act,
				      "grant %u: the chain should still ask", i);
			zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
				      fake_mpsl_signal_s(fake_mpsl_last_session(),
							 MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED),
				      NULL);
		} else {
			zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END, act,
				      "after three refusals the gate asked again");
		}
		k_msleep(WORK_MS);
	}
	gate_probe_read(&p);
	zassert_equal(3u, p.extends_failed, NULL);
	assert_invariant();
}

/*
 * A low-priority signal runs in MPSL's cooperative thread, and the other
 * session's START is a zero-latency interrupt, so only a masked check-then-act
 * keeps that START from landing between the test of g.ts_live and the disarm.
 * The fake cannot land one there; it can say whether the disarm was masked.
 */
ZTEST(radiant_gate_d2, test_d2_session_idle_disarms_with_the_callback_masked)
{
	(void)scan_granted();
	fake_mpsl_timer0.int_disable_zli_depth = 0u;

	(void)fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_SESSION_IDLE);

	zassert_true(fake_mpsl_timer0.int_disable_zli_depth > 0u,
		     "SESSION_IDLE tore a grant down unmasked");
	zassert_equal(0u, fake_swi()->zli_depth, "unbalanced lock");
	assert_invariant();
}

ZTEST(radiant_gate_d2, test_d2_session_closed_disarms_with_the_callback_masked)
{
	(void)scan_granted();
	fake_mpsl_timer0.int_disable_zli_depth = 0u;

	(void)fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED);

	zassert_true(fake_mpsl_timer0.int_disable_zli_depth > 0u,
		     "SESSION_CLOSED tore a grant down unmasked");
	zassert_equal(0u, fake_swi()->zli_depth, "unbalanced lock");
}

ZTEST(radiant_gate_d2, test_d2_blocked_disarms_with_the_callback_masked)
{
	(void)scan_granted();
	fake_mpsl_timer0.int_disable_zli_depth = 0u;

	(void)fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_BLOCKED);

	zassert_true(fake_mpsl_timer0.int_disable_zli_depth > 0u,
		     "BLOCKED tore a stale grant down unmasked");
	zassert_equal(0u, fake_swi()->zli_depth, "unbalanced lock");
}

/* Two writes make the arm the booking's owner, and a START between them hands
 * the grant back. */
ZTEST(radiant_gate_d2, test_d2_the_booking_match_is_masked)
{
	radiant_time_t t = radiant_radio_now() + SCAN_LEAD_US;
	uint32_t       locks;

	zassert_equal(GATE_BOOK_OK, gate_book(t, t + TRACK_LEN_US, 0u), NULL);
	k_msleep(WORK_MS);
	locks = fake_swi()->zli_locks;

	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t, t + TRACK_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);

	zassert_true(fake_swi()->zli_locks > locks, "the match ran unmasked");
	zassert_equal(0u, fake_swi()->zli_depth, "unbalanced lock");
	gate_probe_read(&p);
	zassert_equal(1u, p.book_hit, NULL);
}

/* A booking nobody has armed into reaches the core a little before it starts,
 * once. An armed one, or one still far off, does not. */
ZTEST(radiant_gate_d2, test_d2_a_booking_due_tells_the_core_once)
{
	radiant_time_t t = radiant_radio_now() + SCAN_LEAD_US;

	zassert_equal(GATE_BOOK_OK, gate_book(t, t + TRACK_LEN_US, 0u), NULL);
	k_msleep(WORK_MS);

	gate_probe_fire_booking_due();
	zassert_equal(0u, fake_backend()->booking_due, "told while still far off");

	fake_radio_advance_to(t - 2100u);
	gate_probe_fire_booking_due();
	zassert_equal(1u, fake_backend()->booking_due, "the core was not told");
	gate_probe_fire_booking_due();
	zassert_equal(1u, fake_backend()->booking_due, "told twice");
}

ZTEST(radiant_gate_d2, test_d2_an_armed_booking_is_not_due)
{
	radiant_time_t t = radiant_radio_now() + SCAN_LEAD_US;

	zassert_equal(GATE_BOOK_OK, gate_book(t, t + TRACK_LEN_US, 0u), NULL);
	k_msleep(WORK_MS);
	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, t, t + TRACK_LEN_US, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);

	fake_radio_advance_to(t - 2100u);
	gate_probe_fire_booking_due();
	zassert_equal(0u, fake_backend()->booking_due, NULL);
}

ZTEST(radiant_gate_d2, test_d2_one_session_is_masters_gate)
{
	gate_shutdown();
	k_msleep(WORK_MS);
	fake_mpsl_reset();
	fake_backend_reset();
	fake_mpsl_set_max_sessions(1u);
	zassert_equal(RADIANT_RADIO_OK_RC, gate_init(), NULL);
	k_msleep(WORK_MS);
	gate_probe_read(&p);
	zassert_equal(1u, p.n_open, NULL);
	zassert_false(p.d2_on, NULL);
	zassert_equal(0u, fake_backend()->elastic_lead_us,
		      "a single-session gate advertised an elastic lead");
	zassert_equal(GATE_BOOK_ENOTSUP,
		      gate_book(radiant_radio_now() + SCAN_LEAD_US,
				radiant_radio_now() + SCAN_LEAD_US + 1000u, 0u), NULL);
	zassert_false(gate_booking(), NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal_s(0u, MPSL_TIMESLOT_SIGNAL_START), NULL);
	assert_invariant();
}

#endif /* CONFIG_RADIANT_GATE_MPSL_D2 */
