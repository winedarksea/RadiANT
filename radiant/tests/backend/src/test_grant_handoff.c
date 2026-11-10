/* SPDX-License-Identifier: Apache-2.0 */
/*
 * THE GRANT HANDOFF, on the two completion paths that retire an operation
 * without deliver_terminal(): a stop_on_first frame and the end of a transmit.
 * A release is owed while the completion is still to run, kept if that
 * completion arms a follow-on, and paid when the dispatch drains otherwise.
 */

#include <zephyr/ztest.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_probe.h"

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	backend_reset();
}

ZTEST_SUITE(radiant_backend_handoff, NULL, NULL, before, NULL, NULL);

ZTEST(radiant_backend_handoff, test_a_frame_owes_the_release_until_the_dispatch)
{
	backend_arm_rx(true);
	backend_frame(true);

	zassert_true(backend_op_retired(), "the frame did not end the window");
	zassert_equal(0u, backend_releases(),
		      "released before the completion could arm a follow-on");
	zassert_true(backend_grant_held(), NULL);

	backend_dispatch();

	zassert_equal(1u, backend_probe()->rx_events, NULL);
	zassert_equal(1u, backend_releases(),
		      "a window that heard its frame kept its reserve to the "
		      "grant's own end");
}

ZTEST(radiant_backend_handoff, test_a_frame_keeps_the_grant_for_a_follow_on)
{
	backend_probe()->arm_follow_on = true;
	backend_arm_rx(true);
	backend_frame(true);
	backend_dispatch();

	zassert_equal(0u, backend_releases(),
		      "the grant was given back under the follow-on it reserved "
		      "air for");
	zassert_true(backend_grant_held(), NULL);
}

ZTEST(radiant_backend_handoff, test_a_transmit_owes_the_release_until_the_dispatch)
{
	backend_arm_tx();
	backend_tx_end();

	zassert_true(backend_op_retired(), NULL);
	zassert_equal(0u, backend_releases(), NULL);

	backend_dispatch();

	zassert_equal(1u, backend_probe()->tx_events, NULL);
	zassert_equal(RADIANT_RADIO_STATUS_OK, backend_probe()->last_tx_status, NULL);
	zassert_equal(1u, backend_releases(),
		      "a transmit with nothing behind it kept its reserve");
}

ZTEST(radiant_backend_handoff, test_a_transmit_keeps_the_grant_for_a_follow_on)
{
	backend_probe()->arm_follow_on = true;
	backend_arm_tx();
	backend_tx_end();
	backend_dispatch();

	zassert_equal(0u, backend_releases(), NULL);
	zassert_true(backend_grant_held(), NULL);
}

ZTEST(radiant_backend_handoff, test_the_follow_on_owes_the_next_release)
{
	backend_probe()->arm_follow_on = true;
	backend_arm_tx();
	backend_tx_end();
	backend_dispatch();
	zassert_equal(0u, backend_releases(), NULL);

	/* The follow-on is a transmit too, and this time nothing follows it. */
	backend_probe()->arm_follow_on = false;
	backend_tx_end();
	backend_dispatch();

	zassert_equal(2u, backend_probe()->tx_events, NULL);
	zassert_equal(1u, backend_releases(), NULL);
}

ZTEST(radiant_backend_handoff, test_a_frame_in_an_open_window_owes_nothing)
{
	backend_arm_rx(false);
	backend_frame(true);
	backend_dispatch();

	zassert_false(backend_op_retired(), "the window should still be open");
	zassert_equal(0u, backend_releases(), "the window still needs its grant");
	zassert_true(backend_grant_held(), NULL);
}

ZTEST(radiant_backend_handoff, test_a_crc_failure_does_not_end_a_stop_on_first_window)
{
	backend_arm_rx(true);
	backend_frame(false);
	backend_dispatch();

	zassert_false(backend_op_retired(), NULL);
	zassert_equal(0u, backend_releases(), NULL);
}

ZTEST(radiant_backend_handoff, test_an_empty_window_still_releases_through_its_terminal)
{
	backend_arm_rx(true);
	backend_window_close();
	zassert_equal(0u, backend_releases(), NULL);
	backend_dispatch();

	zassert_equal(RADIANT_RADIO_STATUS_TIMEOUT, backend_probe()->last_rx_status, NULL);
	zassert_equal(1u, backend_releases(), NULL);
}
