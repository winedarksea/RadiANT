/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The gate stops asking for extensions the arbiter keeps refusing (ADR 0021,
 * decision 3). MPSL ASSERT 69:108 has only ever followed an EXTEND_FAILED
 * beside a Thread stack, so the gate takes that request away once it is plainly
 * not going to be granted.
 *
 * What these pin:
 *   - after three refusals in a row the next grants end at their first compare,
 *     with no ACTION_EXTEND, and the skip lapses after a fixed number of grants;
 *   - one granted extension breaks a run of refusals;
 *   - a grant placed while skipping asks only for the fixed end margin, and
 *     that margin is the grant's own: it can never reach ACTION_EXTEND, even if
 *     the skip lapses while it runs;
 *   - gate_init() forgets the whole thing.
 *
 * Every case runs on one session: the skip is not tied to the booking design.
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

#define WORK_MS 20

#define LEAD_US  500000u
#define SHORT_US 5000u
#define CHUNK_US 40000u

/* Pinned, as test_gate.c pins them. A 5 ms elastic window's request is
 * head 250 + window 5000 + tail 250 + end margin: 7500 with the extension
 * margin, 6250 with the fixed one. A chunk long enough to be clamped asks for
 * the first bite, and its compare sits one margin before the end of it. */
#define SHORT_EXTEND_LEN_US 7500u
#define SHORT_FIXED_LEN_US  6250u
#define BITE_US             10000u
#define BITE_EXTEND_CC_US   8000u
#define BITE_FIXED_CC_US    9250u

/* The policy, pinned: how many refusals switch it on and how long it lasts. */
#define RUN_OFF     3u
#define SKIP_GRANTS 64u

static struct gate_probe p;

static void init_gate(void)
{
	zassert_equal(RADIANT_RADIO_OK_RC, gate_init(), "gate_init failed");
	zassert_true(fake_mpsl_is_open(), "no session was opened");
	k_msleep(WORK_MS);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START),
		      "the bootstrap slot was not handed straight back");
}

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
	init_gate();
}

static void after(void *fixture)
{
	ARG_UNUSED(fixture);
	gate_shutdown();
	k_msleep(WORK_MS);
}

ZTEST_SUITE(radiant_gate_ext_skip, NULL, NULL, before, after, NULL);

static void assert_invariant(void)
{
	gate_probe_read(&p);
	zassert_equal(p.granted, p.grant_end_calls, "a hand-back was missed");
	zassert_false(p.hw_held, "the radio is still on loan");
	zassert_equal(0u, fake_mpsl_viol_end_when_ended(), "ended twice");
	zassert_equal(0u, fake_mpsl_viol_action_no_slot(), "action outside a slot");
	zassert_equal(0u, fake_mpsl_viol_low_prio_action(), "action from low priority");
}

/* Place an elastic window and take the grant. Returns the length asked for. */
static uint32_t elastic_grant(uint32_t win_us)
{
	radiant_time_t from = radiant_radio_now() + LEAD_US;

	zassert_equal(GATE_PENDING,
		      gate_acquire(GATE_OP_RX, from, from + win_us, 0u,
				   RADIANT_GATE_PRIO_NORMAL),
		      "the window was refused");
	k_msleep(WORK_MS);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_START), NULL);
	return fake_mpsl_last_request()->params.normal.length_us;
}

/* One grant whose extension is refused. */
static void refused_grant(void)
{
	(void)elastic_grant(CHUNK_US);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0),
		      "the chain should still be asking");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED), NULL);
	k_msleep(WORK_MS);
}

static void refused_run(void)
{
	for (uint32_t i = 0u; i < RUN_OFF; i++) {
		refused_grant();
	}
}

/* A grant the gate must end at its first compare without asking. */
static void skipped_grant(uint32_t i)
{
	(void)elastic_grant(CHUNK_US);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0),
		      "skipped grant %u asked for an extension", i);
	k_msleep(WORK_MS);
}

ZTEST(radiant_gate_ext_skip, test_a_run_of_refusals_stops_the_asking)
{
	refused_run();
	skipped_grant(0u);
	gate_probe_read(&p);
	zassert_equal(RUN_OFF, p.extends_failed,
		      "a skipped grant was counted as a refusal");
	assert_invariant();
}

ZTEST(radiant_gate_ext_skip, test_the_skip_lapses_after_its_grants)
{
	refused_run();
	/* Every one of them, the last included: a grant sized for no extension
	 * must not ask for one because the skip ran out while it was live. */
	for (uint32_t i = 0u; i < SKIP_GRANTS; i++) {
		skipped_grant(i);
	}
	(void)elastic_grant(CHUNK_US);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0),
		      "the gate did not ask again once the skip lapsed");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED), NULL);
	k_msleep(WORK_MS);
	assert_invariant();
}

ZTEST(radiant_gate_ext_skip, test_a_granted_extension_breaks_the_run)
{
	refused_grant();
	refused_grant();

	(void)elastic_grant(CHUNK_US);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_NONE,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED), NULL);
	gate_release();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	k_msleep(WORK_MS);

	refused_grant();
	(void)elastic_grant(CHUNK_US);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0),
		      "a granted extension did not break the run of refusals");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED), NULL);
	k_msleep(WORK_MS);
	assert_invariant();
}

ZTEST(radiant_gate_ext_skip, test_a_skipped_grant_asks_for_the_fixed_margin)
{
	uint32_t len;

	len = elastic_grant(SHORT_US);
	zassert_equal(SHORT_EXTEND_LEN_US, len,
		      "an elastic window outside a skip lost its extension margin");
	gate_release();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	k_msleep(WORK_MS);

	refused_run();

	len = elastic_grant(SHORT_US);
	zassert_equal(SHORT_FIXED_LEN_US, len,
		      "a grant that will never extend paid the extension margin");
	gate_probe_read(&p);
	zassert_equal(750u, p.end_margin_us, NULL);
	gate_release();
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	k_msleep(WORK_MS);
	assert_invariant();
}

ZTEST(radiant_gate_ext_skip, test_a_skipped_grant_keeps_listening_to_its_own_margin)
{
	uint32_t len;

	len = elastic_grant(CHUNK_US);
	zassert_equal(BITE_US, len, NULL);
	zassert_equal(BITE_EXTEND_CC_US, fake_mpsl_timer0.cc[0],
		      "the extendable grant's compare moved");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED), NULL);
	k_msleep(WORK_MS);
	refused_grant();
	refused_grant();

	len = elastic_grant(CHUNK_US);
	zassert_equal(BITE_US, len, NULL);
	zassert_equal(BITE_FIXED_CC_US, fake_mpsl_timer0.cc[0],
		      "a skipped grant's compare is not its own margin");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0), NULL);
	k_msleep(WORK_MS);
	assert_invariant();
}

ZTEST(radiant_gate_ext_skip, test_gate_init_forgets_the_skip)
{
	refused_run();

	gate_shutdown();
	k_msleep(WORK_MS);
	fake_mpsl_reset();
	fake_backend_reset();
	init_gate();

	(void)elastic_grant(CHUNK_US);
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_TIMER0),
		      "the skip survived gate_init()");
	zassert_equal(MPSL_TIMESLOT_SIGNAL_ACTION_END,
		      fake_mpsl_signal(MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED), NULL);
	k_msleep(WORK_MS);
	assert_invariant();
}
