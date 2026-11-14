/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The nRF backend's failure paths around an arm, the three MPSL items the
 * 2026-11-08 TODO clearance left open:
 *
 *   - program_tx() decides "missed" the way program_rx() does: park the
 *     compare, clear its event, enable the connection, write the compare, then
 *     capture the counter and check the event. A compare that fires between
 *     the write and the capture is a live transmit, not a late one.
 *   - an in-grant ETIME releases the grant at once (A14-1) instead of holding
 *     its air to the TIMER0 end, and the FAILED is still delivered later;
 *   - a FAILED owed for a failed grant is cleared by an abort and by the next
 *     claim, so it can never reach the next operation.
 *
 * The TIMER model counts only on a capture (fakes/nrfx.h), so a test places a
 * compare relative to the captures program_tx() makes: the early ETIME check,
 * clock_counter_at() and the missed check, each advancing the counter by step.
 */

#include <zephyr/ztest.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_probe.h"

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	backend_reset();
}

ZTEST_SUITE(radiant_backend_arm, NULL, NULL, before, NULL, NULL);

/*
 * step 10: the captures land at +10 (early check), +20 (clock_counter_at) and
 * +30 (missed check), counted from the clock reading the start is placed from.
 * A start at +25 is written ahead of the counter and passed by the last
 * capture, so its compare fires: the transmit is on the air. The old order
 * re-read the clock and called that late, disabling a transmit already started.
 */
ZTEST(radiant_backend_arm, test_a_compare_that_fires_after_its_write_is_a_live_transmit)
{
	zassert_equal(RADIANT_RADIO_OK_RC, backend_program_tx(25u, 10u),
		      "a transmit whose compare fired was called late");
	zassert_true(backend_tx_conn_live(), "the live transmit lost its start edge");
}

/* A start at +20 is written where the counter already is: it never fires. */
ZTEST(radiant_backend_arm, test_a_compare_written_behind_the_counter_is_etime)
{
	zassert_equal(RADIANT_RADIO_ETIME, backend_program_tx(20u, 10u),
		      "a compare that can never fire was accepted");
	zassert_false(backend_tx_conn_live(),
		      "a refused transmit left its start connection enabled");
	zassert_equal(0u, backend_shorts(), "a refused transmit left SHORTS set");
}

/* Well ahead: written ahead, not yet reached by the last capture. */
ZTEST(radiant_backend_arm, test_a_compare_still_ahead_is_a_transmit)
{
	zassert_equal(RADIANT_RADIO_OK_RC, backend_program_tx(500u, 10u));
	zassert_true(backend_tx_conn_live(), NULL);
}

ZTEST(radiant_backend_arm, test_an_in_grant_etime_releases_the_grant_at_once)
{
	backend_grant_late_tx();

	zassert_true(backend_gate_failed(), "the late grant was not marked failed");
	zassert_equal(1u, backend_releases(),
		      "an in-grant ETIME held the grant's air to its TIMER0 end");
	zassert_false(backend_tx_conn_live(), "nothing may stay armed in a released grant");
	zassert_equal(0u, backend_shorts(), NULL);
	zassert_equal(0u, backend_probe()->tx_events,
		      "FAILED delivered inside the grant's signal callback");

	/* denied_work_fn(), from thread context once the timeslot has ended. */
	backend_finish_failed();
	backend_dispatch();
	zassert_equal(1u, backend_probe()->tx_events, NULL);
	zassert_equal(RADIANT_RADIO_STATUS_FAILED, backend_probe()->last_tx_status, NULL);
	zassert_false(backend_gate_failed(), NULL);
}

ZTEST(radiant_backend_arm, test_an_abort_clears_a_failed_still_owed)
{
	backend_arm_tx();
	backend_set_gate_failed();

	zassert_equal(RADIANT_RADIO_OK_RC, backend_abort());
	backend_dispatch();
	zassert_equal(1u, backend_probe()->tx_events, NULL);
	zassert_equal(RADIANT_RADIO_STATUS_ABORTED, backend_probe()->last_tx_status, NULL);
	zassert_false(backend_gate_failed(), "the abort left the FAILED owed");

	/* The next operation is armed before denied_work_fn() runs. */
	backend_take_slot_unclaimed();
	backend_finish_failed();
	backend_dispatch();
	zassert_equal(0u, backend_probe()->rx_events,
		      "the aborted operation's FAILED reached the next one");
}

ZTEST(radiant_backend_arm, test_a_claim_starts_with_no_failed_owed)
{
	backend_set_gate_failed();
	backend_claim_terminal();
	zassert_false(backend_gate_failed(),
		      "a new operation inherited the last one's FAILED");
}
