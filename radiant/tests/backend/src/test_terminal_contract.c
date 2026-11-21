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

/* ------------------------------------------------------------------------
 * Item 5, the rest: error codes, the window ceiling, window edges and the
 * contents of an RX terminal.
 * ------------------------------------------------------------------------
 */

/* ---- Error codes ---- */

/* More filters than caps.max_filters is well formed but beyond this
 * backend: ENOTSUP, as TI and the HAL's own cap text say. None is EINVAL. */
ZTEST(radiant_backend_terminal, test_too_many_filters_is_enotsup)
{
	zassert_equal(8u, radiant_radio_caps_get()->max_filters);
	zassert_equal(RADIANT_RADIO_ENOTSUP, backend_rx_n_filters(9u));
	zassert_true(backend_op_retired(), "a refused arm kept the slot");
	zassert_equal(RADIANT_RADIO_EINVAL, backend_rx_n_filters(0u));
}

/* init: a missing table or callback is malformed; a second init is the
 * wrong lifecycle state. abort: ESTATE before init, OK with nothing armed. */
ZTEST(radiant_backend_terminal, test_init_and_abort_lifecycle_codes)
{
	zassert_equal(RADIANT_RADIO_ESTATE, backend_abort_uninited());
	zassert_equal(RADIANT_RADIO_EINVAL, backend_init_call(0));
	zassert_equal(RADIANT_RADIO_EINVAL, backend_init_call(1));
	zassert_equal(RADIANT_RADIO_EINVAL, backend_init_call(2));

	backend_arm_ready();
	zassert_equal(RADIANT_RADIO_ESTATE, backend_init_call(3));
	zassert_equal(RADIANT_RADIO_OK_RC, backend_abort());
	zassert_equal(0u, backend_probe()->rx_events + backend_probe()->tx_events);
}

/* ---- A window over caps.max_window_us ---- */

/* ENOTSUP, answered before the gate is asked: the ceiling is advertised, so
 * a longer window is a request this backend cannot take, not a refusal of
 * air. Exactly the ceiling is accepted. */
ZTEST(radiant_backend_terminal, test_a_window_over_the_ceiling_is_enotsup_before_the_gate)
{
	const uint64_t t = backend_now() + 10000u;
	uint32_t op = 1u;

	backend_set_max_window(20000u);
	zassert_equal(20000u, radiant_radio_caps_get()->max_window_us);

	zassert_equal(RADIANT_RADIO_ENOTSUP, backend_rx_window(t, t + 20001u, 0u, &op));
	zassert_equal(0u, op);
	zassert_equal(0u, backend_acquires(), "the gate was asked for it");
	zassert_true(backend_op_retired(), "a refused arm kept the slot");

	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx_window(t, t + 20000u, 0u, &op));
	zassert_equal(1u, backend_acquires());
}

/* An ED sweep spans (hi - lo + 1) * dwell, under the same ceiling. */
ZTEST(radiant_backend_terminal, test_an_ed_sweep_over_the_ceiling_is_enotsup_before_the_gate)
{
	uint32_t op = 1u;

	backend_set_max_window(20000u);
	zassert_equal(RADIANT_RADIO_ENOTSUP, backend_ed_live(10u, 12u, 10000u, &op));
	zassert_equal(0u, op);
	zassert_equal(0u, backend_acquires(), "the gate was asked for it");
	zassert_true(backend_op_retired());
}

/* ---- Window edges: t_sync always lies in [t_open, t_close] ---- */

/* One microsecond before t_open is outside: dropped in software, counted,
 * and the window goes on (a STOP_ON_FIRST window is not ended by it). A
 * frame at t_open is delivered. */
ZTEST(radiant_backend_terminal, test_a_frame_before_t_open_is_dropped)
{
	const uint64_t t_open = backend_now() + 10000u;
	uint32_t op = 0u;

	zassert_equal(RADIANT_RADIO_OK_RC,
		      backend_rx_window(t_open, t_open + 1000u, RADIANT_RX_STOP_ON_FIRST, &op));
	backend_frame_t_sync(true, t_open - 1u);
	backend_dispatch();
	zassert_equal(0u, backend_probe()->rx_events, "a frame before t_open was delivered");
	zassert_equal(1u, backend_window_drops());
	zassert_false(backend_op_retired(), "a dropped frame ended the window");

	backend_frame_t_sync(true, t_open);
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events);
	zassert_equal(RADIANT_RADIO_STATUS_OK, backend_probe()->last_rx_status);
	zassert_equal(t_open, backend_probe()->last_rx_t_sync);
	zassert_true(backend_op_retired(), "the accepted frame did not end the window");
}

/* t_close is inside (delivered, exact); one microsecond later is not. */
ZTEST(radiant_backend_terminal, test_a_frame_at_t_close_is_delivered_and_after_it_dropped)
{
	const uint64_t t_open = backend_now() + 10000u;
	const uint64_t t_close = t_open + 1000u;
	uint32_t op = 0u;

	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx_window(t_open, t_close, 0u, &op));
	backend_frame_t_sync(true, t_close);
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events);
	zassert_equal(t_close, backend_probe()->last_rx_t_sync);
	zassert_true(backend_probe()->last_rx_t_sync_exact);
	zassert_equal(0u, backend_window_drops());

	backend_frame_t_sync(true, t_close + 1u);
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events, "a frame after t_close was delivered");
	zassert_equal(1u, backend_window_drops());
}

/*
 * The case hardware cannot catch: a coded window's close compare is sized
 * for the longest coded frame, so a short coded frame whose t_sync is past
 * t_close still ends (END raised) before the receiver is disabled.
 */
ZTEST(radiant_backend_terminal, test_a_short_coded_frame_after_t_close_is_dropped)
{
	const uint64_t t_open = backend_now() + 10000u;
	const uint64_t t_close = t_open + 1000u;
	uint32_t op = 0u;

	zassert_equal(RADIANT_RADIO_OK_RC,
		      backend_rx_window(t_open, t_close, RADIANT_RX_STOP_ON_FIRST, &op));
	backend_make_coded();
	backend_frame_t_sync(true, t_close + 300u);
	backend_dispatch();
	zassert_equal(0u, backend_probe()->rx_events,
		      "a coded frame past t_close was delivered");
	zassert_equal(1u, backend_window_drops());
	zassert_false(backend_op_retired());

	/* And the window still ends on its own close, with no noise sample:
	 * it heard something. */
	backend_window_close();
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events);
	zassert_equal(RADIANT_RADIO_STATUS_TIMEOUT, backend_probe()->last_rx_status);
	zassert_false(backend_probe()->last_rx_has_noise);
}

/* ---- RX terminal contents: t_sync = requested t_close, not exact ---- */

static void expect_rx_terminal(int status, uint64_t t_close)
{
	zassert_equal(status, backend_probe()->last_rx_status);
	zassert_equal(t_close, backend_probe()->last_rx_t_sync,
		      "terminal %d reported t_sync %llu, t_close %llu", status,
		      (unsigned long long)backend_probe()->last_rx_t_sync,
		      (unsigned long long)t_close);
	zassert_false(backend_probe()->last_rx_t_sync_exact);
}

/* An empty window that ran to close: t_close, and a noise sample. */
ZTEST(radiant_backend_terminal, test_an_empty_timeout_reports_t_close_and_noise)
{
	const uint64_t t_open = backend_now() + 10000u;
	uint32_t op = 0u;

	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx_window(t_open, t_open + 1000u, 0u, &op));
	backend_window_close();
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events);
	expect_rx_terminal(RADIANT_RADIO_STATUS_TIMEOUT, t_open + 1000u);
	zassert_true(backend_probe()->last_rx_has_noise);
}

/* A window that heard a frame: t_close, and no noise sample. */
ZTEST(radiant_backend_terminal, test_a_timeout_after_a_frame_reports_t_close_without_noise)
{
	const uint64_t t_open = backend_now() + 10000u;
	uint32_t op = 0u;

	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx_window(t_open, t_open + 1000u, 0u, &op));
	backend_frame_t_sync(true, t_open + 100u);
	backend_window_close();
	backend_dispatch();
	zassert_equal(2u, backend_probe()->rx_events);
	expect_rx_terminal(RADIANT_RADIO_STATUS_TIMEOUT, t_open + 1000u);
	zassert_false(backend_probe()->last_rx_has_noise);
}

ZTEST(radiant_backend_terminal, test_an_aborted_window_reports_t_close)
{
	const uint64_t t_open = backend_now() + 10000u;
	uint32_t op = 0u;

	zassert_equal(RADIANT_RADIO_OK_RC, backend_rx_window(t_open, t_open + 1000u, 0u, &op));
	zassert_equal(RADIANT_RADIO_OK_RC, backend_abort());
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events);
	expect_rx_terminal(RADIANT_RADIO_STATUS_ABORTED, t_open + 1000u);
	zassert_false(backend_probe()->last_rx_has_noise);
}

ZTEST(radiant_backend_terminal, test_a_denied_window_reports_t_close)
{
	const uint64_t t_open = backend_now() + 10000u;
	uint32_t op = 0u;

	zassert_equal(RADIANT_RADIO_OK_RC,
		      backend_rx_window_staged(t_open, t_open + 1000u, &op));
	backend_deny_staged();
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events);
	expect_rx_terminal(RADIANT_RADIO_STATUS_DENIED, t_open + 1000u);
}

/* Staged, then granted after t_open: the programme fails in the grant and
 * the FAILED goes out from thread context, the window never programmed. */
ZTEST(radiant_backend_terminal, test_a_failed_window_reports_t_close)
{
	const uint64_t t_open = backend_now() + 10000u;
	uint32_t op = 0u;

	zassert_equal(RADIANT_RADIO_OK_RC,
		      backend_rx_window_staged(t_open, t_open + 1000u, &op));
	backend_grant_rx_after(500u);
	zassert_true(backend_gate_failed(), "the late grant was not marked failed");
	backend_finish_failed();
	backend_dispatch();
	zassert_equal(1u, backend_probe()->rx_events);
	expect_rx_terminal(RADIANT_RADIO_STATUS_FAILED, t_open + 1000u);
}
