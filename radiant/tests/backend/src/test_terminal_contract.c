/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The two HAL contract points of TODO "Backend drift" item 5 that change
 * behaviour, on the nRF backend:
 *
 *   - a transmit that did not go out (FAILED, DENIED, ABORTED) reports the
 *     t_sync it was armed for, with t_sync_exact false. t_sync is meaningful
 *     only on OK; a fixed, known value keeps a reader that ignores the status
 *     from picking up a clock reading taken at some arbitrary later instant;
 *   - radiant_radio_abort() from inside a callback delivers the aborted
 *     operation's terminal nested, before abort returns, exactly once, and
 *     the slot is free on return (a re-arm from the same callback succeeds).
 */

#include <string.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_probe.h"

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	backend_reset();
}

ZTEST_SUITE(radiant_backend_terminal, NULL, NULL, before, NULL, NULL);

/* ---- t_sync of a transmit that did not go out ---- */

ZTEST(radiant_backend_terminal, test_an_aborted_transmit_reports_its_requested_t_sync)
{
	uint32_t op = 0u;
	const uint64_t t = backend_now() + 10000u;

	zassert_equal(RADIANT_RADIO_OK_RC, backend_tx_live(t, &op));
	zassert_equal(RADIANT_RADIO_OK_RC, backend_abort());
	backend_dispatch();

	zassert_equal(1u, backend_probe()->tx_events);
	zassert_equal(RADIANT_RADIO_STATUS_ABORTED, backend_probe()->last_tx_status);
	zassert_equal(t, backend_probe()->last_tx_t_sync,
		      "ABORTED reported %llu, armed for %llu",
		      (unsigned long long)backend_probe()->last_tx_t_sync,
		      (unsigned long long)t);
	zassert_false(backend_probe()->last_tx_t_sync_exact);
}

ZTEST(radiant_backend_terminal, test_a_denied_transmit_reports_its_requested_t_sync)
{
	uint32_t op = 0u;
	const uint64_t t = backend_now() + 10000u;

	zassert_equal(RADIANT_RADIO_OK_RC, backend_tx_staged(t, &op));
	backend_deny_staged();
	backend_dispatch();

	zassert_equal(1u, backend_probe()->tx_events);
	zassert_equal(RADIANT_RADIO_STATUS_DENIED, backend_probe()->last_tx_status);
	zassert_equal(t, backend_probe()->last_tx_t_sync,
		      "DENIED reported %llu, armed for %llu",
		      (unsigned long long)backend_probe()->last_tx_t_sync,
		      (unsigned long long)t);
	zassert_false(backend_probe()->last_tx_t_sync_exact);
}

/* Staged, then granted after its t_sync: the arm fails in the grant and the
 * FAILED goes out from thread context. Nothing was programmed, so only the
 * arm call can have kept the requested time. */
ZTEST(radiant_backend_terminal, test_a_failed_transmit_reports_its_requested_t_sync)
{
	uint32_t op = 0u;
	const uint64_t t = backend_now() + 10000u;

	zassert_equal(RADIANT_RADIO_OK_RC, backend_tx_staged(t, &op));
	backend_grant_after(1000u);
	zassert_true(backend_gate_failed(), "the late grant was not marked failed");
	backend_finish_failed();
	backend_dispatch();

	zassert_equal(1u, backend_probe()->tx_events);
	zassert_equal(RADIANT_RADIO_STATUS_FAILED, backend_probe()->last_tx_status);
	zassert_equal(t, backend_probe()->last_tx_t_sync,
		      "FAILED reported %llu, armed for %llu",
		      (unsigned long long)backend_probe()->last_tx_t_sync,
		      (unsigned long long)t);
	zassert_false(backend_probe()->last_tx_t_sync_exact);
}

/* ---- Abort from inside a callback ---- */

static struct {
	bool     done;
	int      abort_rc;
	uint32_t rx_events_at_return;
	int      rx_status_at_return;
	bool     retired_at_return;
	int      rearm_rc;
	uint32_t rearm_op;
} nest;

/* From the frame callback of the window it aborts. */
static void abort_from_frame(int status)
{
	if (status != RADIANT_RADIO_STATUS_OK || nest.done) {
		return;
	}
	nest.done = true;
	nest.abort_rc = backend_abort();
	nest.rx_events_at_return = backend_probe()->rx_events;
	nest.rx_status_at_return = backend_probe()->last_rx_status;
	nest.retired_at_return = backend_op_retired();
	nest.rearm_rc = backend_tx_live(backend_now() + 10000u, &nest.rearm_op);
}

ZTEST(radiant_backend_terminal, test_abort_from_a_frame_callback_delivers_nested)
{
	memset(&nest, 0, sizeof(nest));
	backend_arm_rx(false);
	backend_arm_ready();
	backend_probe()->on_rx = abort_from_frame;

	backend_frame(true);
	backend_dispatch();

	zassert_true(nest.done, "the frame never reached the core");
	zassert_equal(RADIANT_RADIO_OK_RC, nest.abort_rc);
	zassert_equal(2u, nest.rx_events_at_return,
		      "the ABORTED terminal was not delivered before abort returned");
	zassert_equal(RADIANT_RADIO_STATUS_ABORTED, nest.rx_status_at_return);
	zassert_true(nest.retired_at_return, "the slot was not free on return");
	zassert_equal(RADIANT_RADIO_OK_RC, nest.rearm_rc,
		      "a re-arm from the same callback was refused");

	/* Exactly once: nothing more is owed to the aborted window. */
	backend_dispatch();
	zassert_equal(2u, backend_probe()->rx_events);
	zassert_equal(0u, backend_probe()->tx_events);
	zassert_false(backend_op_retired(), "the re-armed transmit lost its slot");
}

/* From a transmit's completion: the follow-on it armed is aborted there. */
static void arm_then_abort_from_tx(int status)
{
	uint32_t op = 0u;

	if (status != RADIANT_RADIO_STATUS_OK || nest.done) {
		return;
	}
	nest.done = true;
	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx_live(&op));
	nest.abort_rc = backend_abort();
	nest.rx_events_at_return = backend_probe()->rx_events;
	nest.rx_status_at_return = backend_probe()->last_rx_status;
	nest.retired_at_return = backend_op_retired();
	nest.rearm_rc = backend_rx_live(&nest.rearm_op);
}

ZTEST(radiant_backend_terminal, test_abort_from_a_transmit_callback_delivers_nested)
{
	memset(&nest, 0, sizeof(nest));
	backend_arm_tx();
	backend_arm_ready();
	backend_probe()->on_tx = arm_then_abort_from_tx;

	backend_tx_end();
	backend_dispatch();

	zassert_true(nest.done, "the transmit never reached the core");
	zassert_equal(1u, backend_probe()->tx_events);
	zassert_equal(RADIANT_RADIO_OK_RC, nest.abort_rc);
	zassert_equal(1u, nest.rx_events_at_return,
		      "the ABORTED terminal was not delivered before abort returned");
	zassert_equal(RADIANT_RADIO_STATUS_ABORTED, nest.rx_status_at_return);
	zassert_true(nest.retired_at_return, "the slot was not free on return");
	zassert_equal(RADIANT_RADIO_OK_RC, nest.rearm_rc,
		      "a re-arm from the same callback was refused");

	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events);
	zassert_equal(1u, backend_probe()->tx_events);
	zassert_false(backend_op_retired(), "the re-armed window lost its slot");
}
