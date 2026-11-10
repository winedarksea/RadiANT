/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gate_in_signal() means "this is the zero-latency callback". A low-priority
 * signal runs in MPSL's cooperative thread, so a completion raised while one is
 * running must not be routed to the single-producer ring: that context is a
 * second producer, and the trampoline can preempt it and see the flag set.
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

#define WORK_MS 20

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
	fake_swi_reset();
}

static void after(void *fixture)
{
	ARG_UNUSED(fixture);
	gate_shutdown();
	k_msleep(WORK_MS);
}

ZTEST_SUITE(radiant_gate_signal_context, NULL, NULL, before, after, NULL);

static void place(void)
{
	radiant_time_t from = radiant_radio_now() + 500000u;

	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, from, from + 1000u, 0u,
				   RADIANT_GATE_PRIO_HIGH), NULL);
	k_msleep(WORK_MS);
	fake_swi_reset();
}

ZTEST(radiant_gate_signal_context, test_a_high_priority_signal_is_the_signal_context)
{
	place();
	(void)fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START);
	zassert_true(fake_swi()->pends_in_signal > 0u,
		     "START pended nothing from inside the signal context");
	gate_release();
	(void)fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0);
	k_msleep(WORK_MS);
}

ZTEST(radiant_gate_signal_context, test_a_refusal_is_not_the_signal_context)
{
	place();
	(void)fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_BLOCKED);
	zassert_true(fake_swi()->pends > 0u, "BLOCKED pended no denial");
	zassert_equal(0u, fake_swi()->pends_in_signal,
		      "a low-priority signal claimed the zero-latency context");
	k_msleep(WORK_MS);
}

ZTEST(radiant_gate_signal_context, test_session_idle_is_not_the_signal_context)
{
	place();
	(void)fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_SESSION_IDLE);
	zassert_true(fake_swi()->pends > 0u, "SESSION_IDLE re-pended nothing");
	zassert_equal(0u, fake_swi()->pends_in_signal,
		      "a low-priority signal claimed the zero-latency context");
	k_msleep(WORK_MS);
}
