/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The grant generation (gate_grant_gen()): the gate's half of AN ARM
 * PREEMPTED BY THE GRANT'S END (radiant_radio_nrf.c, ARM_SLOT_GUARD). The
 * backend records it at an arm's claim and refuses, with nothing written, an
 * arm whose span finds it changed or over. That is sound only if:
 *
 *   - every SIGNAL_START makes it live (odd) and new, before on_grant() or
 *     anything else can programme the radio;
 *   - every end of the loan makes it over (even) before the backend's
 *     on_grant_end() - each ACTION_END path, gate_release()'s early hand-back,
 *     and the low-priority backstops;
 *   - an extension changes nothing (the loan goes on).
 *
 * assert_invariant() in test_gate.c checks the first two on every radiant.gate
 * path through the fake backend's counters; this suite names the paths.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

#include <mpsl_timeslot.h>

#include "radiant_radio_nrf_gate.h"

#include "../../fake_radio.h"
#include "../fake_mpsl.h"
#include "../fake_swi.h"
#include "../gate_probe.h"

#define WORK_MS  20
#define LEAD_US  500000u
#define WIN_US   5000u
#define CHUNK_US 40000u

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	gate_shutdown();
	k_msleep(WORK_MS);
	fake_radio_bring_up();
	fake_mpsl_reset();
	fake_backend_reset();
	fake_swi_reset();
	gate_probe_reset_elastic();
	zassert_equal(RADIANT_RADIO_OK_RC, gate_init(), NULL);
	k_msleep(WORK_MS);
	/* The bootstrap: a START and its immediate ACTION_END. */
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	k_msleep(WORK_MS);
}

static void after(void *fixture)
{
	ARG_UNUSED(fixture);
	gate_shutdown();
	k_msleep(WORK_MS);
	gate_probe_reset_elastic();
}

ZTEST_SUITE(radiant_gate_grant_gen, NULL, NULL, before, after, NULL);

/* Place a window and take its grant; returns the generation on_grant() saw. */
static uint32_t take_grant(uint8_t prio, uint32_t win_us)
{
	radiant_time_t from = radiant_radio_now() + LEAD_US;
	const uint32_t grants = fake_backend()->on_grant;

	zassert_equal(GATE_PENDING, gate_acquire(GATE_OP_RX, from, from + win_us, 0u, prio),
		      "the window was refused");
	k_msleep(WORK_MS);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	zassert_equal(grants + 1u, fake_backend()->on_grant, "on_grant() was not called");
	zassert_true(GATE_GEN_LIVE(fake_backend()->gen_at_grant),
		     "on_grant() ran before the generation went live");
	zassert_equal(fake_backend()->gen_at_grant, gate_grant_gen(), NULL);
	return fake_backend()->gen_at_grant;
}

static void assert_over(const char *what)
{
	zassert_false(GATE_GEN_LIVE(gate_grant_gen()), "%s left the generation live", what);
	zassert_false(GATE_GEN_LIVE(fake_backend()->gen_at_grant_end),
		      "%s: on_grant_end() ran with the generation live", what);
	zassert_equal(0u, fake_backend()->gen_dead_at_grant, NULL);
	zassert_equal(0u, fake_backend()->gen_live_at_grant_end, NULL);
	zassert_equal(0u, fake_mpsl_viol_end_when_ended(), NULL);
	zassert_equal(0u, fake_mpsl_viol_action_no_slot(), NULL);
	zassert_equal(0u, fake_mpsl_viol_low_prio_action(), NULL);
}

/*
 * Risk: an arm claimed between two timeslots, or under the bootstrap, reads a
 * generation a later grant reuses, and programmes a RADIO that is not ours.
 */
ZTEST(radiant_gate_grant_gen, test_bootstrap_ends_over_and_every_grant_is_new)
{
	uint32_t last;

	assert_over("the bootstrap");
	last = gate_grant_gen();
	for (int i = 0; i < 4; i++) {
		uint32_t g = take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);

		zassert_true((int32_t)(g - last) > 0, "grant %d reused a generation", i);
		last = g;
		gate_release();
		zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
			      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
		assert_over("release + TIMER0");
		k_msleep(WORK_MS);
	}
	zassert_equal(0u, fake_backend()->gen_grant_reused, NULL);
}

/*
 * Risk: gate_release() hands the RADIO back at once but leaves the timeslot
 * running until its TIMER0 compare; an arm resumed in that gap must already
 * find the loan over, not wait for the ACTION_END.
 */
ZTEST(radiant_gate_grant_gen, test_release_ends_the_generation_at_once)
{
	(void)take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	gate_release();
	zassert_true(fake_mpsl_in_timeslot(), "the timeslot is still running");
	assert_over("gate_release()");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	assert_over("its TIMER0 end");
}

/* Risk: the grant's own TIMER0 end (no release) leaves the generation live. */
ZTEST(radiant_gate_grant_gen, test_timer0_end)
{
	(void)take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	assert_over("TIMER0");
}

/* Risk: the RADIO signal's own end (the operation completed in the signal). */
ZTEST(radiant_gate_grant_gen, test_radio_signal_end)
{
	(void)take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	fake_backend_release_in_radio_irq(true);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_RADIO), NULL);
	fake_backend_release_in_radio_irq(false);
	assert_over("SIGNAL_RADIO");
}

/* Risk: a grant released from inside on_grant() (a synchronous completion,
 * or a failed programme) ends the timeslot from SIGNAL_START itself. */
ZTEST(radiant_gate_grant_gen, test_release_in_grant)
{
	radiant_time_t from = radiant_radio_now() + LEAD_US;

	fake_backend_release_in_grant(true);
	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, from, from + WIN_US, 0u, RADIANT_GATE_PRIO_HIGH),
		      NULL);
	k_msleep(WORK_MS);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	fake_backend_release_in_grant(false);
	zassert_true(GATE_GEN_LIVE(fake_backend()->gen_at_grant), NULL);
	assert_over("a release inside on_grant()");
}

/*
 * Risk: an elastic grant's extension ends the generation (failing a live
 * follow-on), or EXTEND_FAILED - which hands back and then aborts from the
 * signal - leaves it live, so an arm under it resumes into the other stack's
 * RADIO.
 */
ZTEST(radiant_gate_grant_gen, test_extension_keeps_and_extend_failed_ends)
{
	uint32_t g = take_grant(RADIANT_GATE_PRIO_NORMAL, CHUNK_US);

	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), "no extension asked");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED), NULL);
	zassert_equal(g, gate_grant_gen(), "an extension moved the generation");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED), NULL);
	assert_over("EXTEND_FAILED");
}

/* Risk: the low-priority backstops (a loan MPSL ended by expiry) return the
 * radio with the generation still live. */
static void backstop(uint32_t signal)
{
	struct gate_probe p;

	(void)take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE, fake_mpsl_signal(signal), NULL);
	gate_probe_read(&p);
	zassert_false(p.hw_held, "signal %u kept the radio", signal);
	assert_over("a low-priority backstop");
}

ZTEST(radiant_gate_grant_gen, test_session_idle_backstop)
{
	backstop(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE);
}

ZTEST(radiant_gate_grant_gen, test_blocked_backstop)
{
	backstop(MPSL_TIMESLOT_SIGNAL_BLOCKED);
}

ZTEST(radiant_gate_grant_gen, test_session_closed_backstop)
{
	backstop(MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED);
}

/* Risk: gate_shutdown() with a loan running leaves the generation live into
 * the next session. */
ZTEST(radiant_gate_grant_gen, test_shutdown_ends_a_live_loan)
{
	(void)take_grant(RADIANT_GATE_PRIO_HIGH, WIN_US);
	gate_shutdown();
	assert_over("gate_shutdown()");
}
