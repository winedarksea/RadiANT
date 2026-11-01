/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The treadmill's heart-rate slave (src/treadmill_hr_rx.c) against a stub
 * antr_* layer that keeps the one piece of channel state the module's event
 * handling depends on: whether the channel has finished closing.
 *
 * The stub's contract is the real stack's (ant_radio.h): open() on a channel
 * that is still open or still CLOSING answers ANTW_CHANNEL_IN_WRONG_STATE, and
 * a search timeout closes the channel ITSELF - RX_SEARCH_TIMEOUT first, then
 * CHANNEL_CLOSED once any scan window bound to it has ended. That gap is the
 * whole of review 2026-10-31's B2-3: the module reopened on the timeout, the
 * reopen was refused, and nothing ever tried again.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/ztest.h>

#include "ant_radio.h"
#include "ant_wire.h"
#include "treadmill_hr_rx.h"
#include "treadmill_state.h"

/* treadmill_hr_rx.c declares the application's log module; main.c owns it in
 * the real image. */
LOG_MODULE_REGISTER(treadmill, CONFIG_TREADMILL_LOG_LEVEL);

#define HR_CH 3u

enum stub_state {
	STUB_UNASSIGNED = 0,
	STUB_ASSIGNED,   /* closed: open() is legal */
	STUB_OPEN,
	STUB_CLOSING,    /* timed out, a scan window still bound */
};

static struct {
	enum stub_state state;
	uint32_t        open_calls;
	uint32_t        open_ok;
	uint8_t         last_prox;
	uint32_t        prox_calls;
} stub;

static uint8_t  last_bpm_seen = 0xEEu;
static uint32_t bpm_calls;

/* ── The stub antr_* layer ─────────────────────────────────────────────── */

antr_err_t antr_channel_assign(uint8_t channel, uint8_t type, uint8_t network,
			       uint8_t ext_assign)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(type);
	ARG_UNUSED(network);
	ARG_UNUSED(ext_assign);
	stub.state = STUB_ASSIGNED;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_id_set(uint8_t channel, uint16_t device_number,
			       uint8_t device_type, uint8_t trans_type)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(device_number);
	ARG_UNUSED(device_type);
	ARG_UNUSED(trans_type);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_period_set(uint8_t channel, uint16_t period)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(period);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_radio_freq_set(uint8_t channel, uint8_t freq)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(freq);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_search_timeout_set(uint8_t channel, uint8_t timeout)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(timeout);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_search_channel_priority_set(uint8_t channel, uint8_t priority)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(priority);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_prox_search_set(uint8_t channel, uint8_t threshold,
				uint8_t custom)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(custom);
	stub.last_prox = threshold;
	stub.prox_calls++;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_lib_config_set(uint8_t config)
{
	ARG_UNUSED(config);
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_open_with_offset(uint8_t channel, uint16_t offset)
{
	ARG_UNUSED(channel);
	ARG_UNUSED(offset);
	stub.open_calls++;
	if (stub.state != STUB_ASSIGNED) {
		return ANTW_CHANNEL_IN_WRONG_STATE;
	}
	stub.state = STUB_OPEN;
	stub.open_ok++;
	return ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_close(uint8_t channel)
{
	ARG_UNUSED(channel);
	if (stub.state != STUB_OPEN) {
		return ANTW_CHANNEL_IN_WRONG_STATE;
	}
	stub.state = STUB_ASSIGNED;
	return ANTW_RESPONSE_NO_ERROR;
}

/* What main.c does with a reading: record it. */
void treadmill_node_heart_rate(uint8_t bpm)
{
	last_bpm_seen = bpm;
	bpm_calls++;
}

/* ── Helpers ───────────────────────────────────────────────────────────── */

static void feed_event(uint8_t code)
{
	uint8_t data[3] = { HR_CH, ANTW_MESG_EVENT_ID, code };
	struct antr_msg m = {
		.id = ANTW_MESG_RESPONSE_EVENT_ID,
		.len = sizeof(data),
		.data = data,
	};

	zassert_true(treadmill_hr_rx_message(&m), "event not claimed");
}

static void feed_bpm(uint8_t bpm)
{
	/* [ch][page, x, x, x, event time lo, hi, count, computed bpm] */
	uint8_t data[9] = { HR_CH, 0x04u, 0u, 0u, 0u, 0u, 0u, 1u, bpm };
	struct antr_msg m = {
		.id = ANTW_MESG_BROADCAST_DATA_ID,
		.len = sizeof(data),
		.data = data,
	};

	zassert_true(treadmill_hr_rx_message(&m), "broadcast not claimed");
}

/* The module hands readings to the system workqueue. */
static void let_work_run(void)
{
	k_sleep(K_MSEC(10));
}

static void before(void *f)
{
	ARG_UNUSED(f);
	memset(&stub, 0, sizeof(stub));
	zassert_ok(treadmill_hr_rx_start(HR_CH));
	zassert_equal(STUB_OPEN, stub.state, "setup: the slave did not open");
}

ZTEST_SUITE(treadmill_hr_rx, NULL, NULL, before, NULL, NULL);

/* ── The cases ─────────────────────────────────────────────────────────── */

/*
 * B2-3: the reopen after a search timeout waits for CHANNEL_CLOSED. The
 * timeout arrives while the channel is still CLOSING (a scan window bound to
 * it); reopening then is refused, and the module used to have no second try.
 */
ZTEST(treadmill_hr_rx, test_reopen_waits_for_channel_closed)
{
	uint8_t  prox_at_open = stub.last_prox;
	uint32_t opens = stub.open_calls;

	stub.state = STUB_CLOSING;
	feed_event(ANTW_EVENT_RX_SEARCH_TIMEOUT);
	zassert_equal(opens, stub.open_calls,
		      "reopened on RX_SEARCH_TIMEOUT, before CHANNEL_CLOSED - "
		      "refused while the channel is CLOSING");

	stub.state = STUB_ASSIGNED;
	feed_event(ANTW_EVENT_CHANNEL_CLOSED);
	zassert_equal(STUB_OPEN, stub.state,
		      "the slave was not reopened after its search timed out: "
		      "no strap will ever be acquired again until reboot");
	zassert_equal(opens + 1u, stub.open_calls, "reopened more than once");
	zassert_equal(prox_at_open > 0u ? prox_at_open - 1u : 0u,
		      stub.last_prox,
		      "the proximity floor did not relax by one bin");
}

/* A CHANNEL_CLOSED with no search timeout before it (a host close, say) is
 * not a reason to reopen. */
ZTEST(treadmill_hr_rx, test_stray_channel_closed_does_not_reopen)
{
	uint32_t opens = stub.open_calls;

	stub.state = STUB_ASSIGNED;
	feed_event(ANTW_EVENT_CHANNEL_CLOSED);
	zassert_equal(opens, stub.open_calls,
		      "reopened on a CHANNEL_CLOSED no timeout asked for");
}

/* B2-2: the strap has gone (RX_FAIL_GO_TO_SEARCH), so its last bpm must stop
 * being reported for the whole search, not linger for 30 s or more. */
ZTEST(treadmill_hr_rx, test_go_to_search_clears_bpm)
{
	feed_bpm(140u);
	let_work_run();
	zassert_equal(140u, last_bpm_seen, "setup: the reading was not delivered");

	feed_event(ANTW_EVENT_RX_FAIL_GO_TO_SEARCH);
	let_work_run();
	zassert_equal(TREADMILL_HR_NONE, last_bpm_seen,
		      "a strap that went away kept its last reading (%u bpm) "
		      "through the search", (unsigned int)last_bpm_seen);
}
