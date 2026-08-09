/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work. Written against
 * radiant_core/include/radiant_core/radiant_channel.h, radiant_core/include/radiant_core/radiant_radio_hal.h,
 * radiant_core/tests/fake_radio.h, docs/sdk-ant-contract.md and the measurements
 * in docs/ant-radio-link.md. Nothing here derives from sdk-ant, from libant.a,
 * or from any adopter-gated ANT+ device profile document.
 *
 * ---------------------------------------------------------------------------
 * The channel state machine
 * ---------------------------------------------------------------------------
 * radiant_channel.c calls no HAL function, so most of this suite is pure-function
 * assertion: a transition table, and the exact wire byte every refusal
 * produces. Three groups of tests are not, and they are the reason the mock is
 * linked in at all:
 *
 *   - a master's first transmission is one full channel period after open, and
 *     the test proves it by actually arming that transmit against the mock and
 *     reading the t_sync the arm recorded. Asserting only on
 *     radiant_channel_next_slot() would prove the arithmetic and not that a
 *     backend accepts the result;
 *   - a close that races an operation in flight. The HAL guarantees a
 *     cancelled operation's terminal event still arrives, with no fault
 *     injection needed, so this is the ordinary outcome of closing a channel
 *     with a window open - not an edge case - and the mock is the only place
 *     it can be made to happen on demand;
 *   - a late event carrying an operation id nobody owns any more. The mock
 *     will never produce one by itself, and a module that has not been shown
 *     one has not been tested against it.
 *
 * Every test finishes with fake_radio_is_idle() and fake_radio_viol_count() ==
 * 0, per the two habits fake_radio.h asks for: the first catches a module that
 * left the radio armed, the second catches one that did something in a
 * callback that would deadlock a real radio ISR.
 *
 * The rx and tx callbacks below forward terminal events straight into
 * radiant_channel_on_terminal(), which is precisely what radiant_sched.c will do. They
 * run in the mock's callback context, so this suite is also the check that the
 * state machine is callable from a radio ISR - it takes no lock, allocates
 * nothing and does no work proportional to anything.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include <radiant_core/radiant_channel.h>
#include <radiant_core/radiant_frame.h>
#include "../fake_radio.h"

/* ---------------------------------------------------------------------------
 * Fixtures
 * ---------------------------------------------------------------------------
 */

/* The Spike A sensor, so the numbers in this file match the ones in
 * docs/spike-a-results.md and in the other suites: #14871, device type 0x0B
 * (power), transmission type 5. */
#define TEST_DEVNUM    0x3A17u
#define TEST_DEVTYPE   0x0Bu
#define TEST_TRANSTYPE 5u

/* Channel types, spelled here rather than pulled from src/ant_wire.h: this
 * application does not include the serial layer's generated header, and
 * radiant_channel.h asserts the master bit against it wherever it is in scope. */
#define TYPE_SLAVE          0x00u
#define TYPE_MASTER         0x10u
#define TYPE_SHARED_SLAVE   0x20u
#define TYPE_SHARED_MASTER  0x30u
#define TYPE_SLAVE_RX_ONLY  0x40u
#define TYPE_MASTER_TX_ONLY 0x50u

/*
 * The ANT+ period in microseconds, computed the way radiant_channel.c computes it.
 * Spelled as a literal as well as asserted against the conversion, because a
 * test that derives its expectation from the code under test proves only that
 * the code is self-consistent.
 */
#define RADIANT_ANT_PLUS_PERIOD_US 249695u

/* ── The event sink ─────────────────────────────────────────────────────────
 *
 * radiant_channel_event_out() is declared by radiant_channel.h and implemented by
 * radiant_event.c in the firmware. Here it is a recorder. The inversion is the
 * same one src/ant_radio.h uses for antr_on_message(): resolved at link time,
 * exactly one definition per image.
 */
struct evt_rec {
	uint8_t channel;
	uint8_t code;
};

#define EVT_MAX 64
static struct evt_rec evts[EVT_MAX];
static uint32_t n_evts;

void radiant_channel_event_out(uint8_t channel, uint8_t event_code)
{
	if (n_evts < EVT_MAX) {
		evts[n_evts].channel = channel;
		evts[n_evts].code = event_code;
	}
	n_evts++;
}

static uint32_t evt_count_of(uint8_t channel, uint8_t code)
{
	uint32_t n = 0u;
	uint32_t i;

	for (i = 0u; i < n_evts && i < EVT_MAX; i++) {
		if (evts[i].channel == channel && evts[i].code == code) {
			n++;
		}
	}

	return n;
}

/* ── The scheduler stand-in ─────────────────────────────────────────────────
 *
 * What radiant_sched.c will do with a terminal event, reduced to its one
 * obligation: hand it to the channel layer and let the op id decide whether it
 * belongs to anybody.
 */
static uint32_t n_terminal;      /* terminal events seen */
static uint32_t n_terminal_stale; /* ... that no channel owned */

static void feed_terminal(uint32_t op, enum radiant_radio_status status)
{
	n_terminal++;
	if (radiant_channel_on_terminal(op, status, radiant_radio_now()) < 0) {
		n_terminal_stale++;
	}
}

static void rx_cb(const struct radiant_rx_event *e, void *user)
{
	ARG_UNUSED(user);

	if (e->status == RADIANT_RADIO_STATUS_TIMEOUT ||
	    e->status == RADIANT_RADIO_STATUS_ABORTED ||
	    e->status == RADIANT_RADIO_STATUS_FAILED) {
		feed_terminal(e->op, e->status);
	}
}

static void tx_cb(const struct radiant_tx_event *e, void *user)
{
	ARG_UNUSED(user);
	/* Every transmit event is terminal. */
	feed_terminal(e->op, e->status);
}

static const struct radiant_radio_cbs cbs = { rx_cb, tx_cb };

/*
 * A tracking-geometry filter for the test sensor. FILE SCOPE ON PURPOSE:
 * radiant_rx_req.filters is core-owned and must stay valid until the terminal
 * event, so a stack local in a helper would be a dangling pointer the instant
 * the helper returned - and the mock would happily read it.
 */
static struct radiant_rx_filter track_filter;

static void before(void *fixture)
{
	ARG_UNUSED(fixture);

	fake_radio_reset();
	radiant_channel_init();

	memset(evts, 0, sizeof(evts));
	n_evts = 0u;
	n_terminal = 0u;
	n_terminal_stale = 0u;

	memset(&track_filter, 0, sizeof(track_filter));
	track_filter.addr[0] = RADIANT_NET_ADDR_ANT_PLUS_0;
	track_filter.addr[1] = RADIANT_NET_ADDR_ANT_PLUS_1;
	track_filter.addr[2] = (uint8_t)(TEST_DEVNUM & 0xFFu);
	track_filter.addr[3] = (uint8_t)(TEST_DEVNUM >> 8);
	track_filter.addr[4] = TEST_DEVTYPE;
	track_filter.addr_len = RADIANT_FRAME_ADDR_LEN_TRACKING;
}

static void up(void)
{
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&cbs, NULL),
		      "radio init failed");
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable(), "radio enable failed");
}

/* The two end-of-test habits fake_radio.h asks for, in one call so no test can
 * forget half of them. */
static void radio_clean(void)
{
	zassert_true(fake_radio_is_idle(), "radio left busy: %s",
		     fake_radio_busy_reason());
	zassert_equal(0u, fake_radio_viol_count(), "contract violation: %s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
}

/* Assign + set an ID, the minimum a channel needs before it may be opened. */
static void assign_and_id(uint8_t ch, uint8_t type)
{
	zassert_equal(RADIANT_CH_OK, radiant_channel_assign(ch, type, 0u, 0u),
		      "assign(%u) failed", ch);
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(ch, TEST_DEVNUM, TEST_DEVTYPE,
					 TEST_TRANSTYPE),
		      "id_set(%u) failed", ch);
}

/* Arm a receive window the way radiant_sched.c would, and bind it to the channel.
 * Returns the op id. */
static uint32_t arm_rx_for(uint8_t ch, radiant_time_t t_open, radiant_time_t t_close)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &track_filter;
	req.n_filters = 1u;
	req.t_open = t_open;
	req.t_close = t_close;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op), "rx arm failed");
	zassert_not_equal(0u, op, "op id was zero");

	radiant_channel_bind_op(ch, op);
	return op;
}

ZTEST_SUITE(radiant_channel, NULL, NULL, before, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Sizing and units
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_channel, test_thirty_two_channels_and_nothing_beyond)
{
	uint8_t status = 0xFFu;
	uint8_t i;

	zassert_equal(32u, RADIANT_CHANNEL_COUNT,
		      "the burst header addresses 32 channels in five bits");

	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		zassert_equal(RADIANT_CH_STATE_UNASSIGNED,
			      radiant_channel_state_get(i),
			      "channel %u did not start unassigned", i);
		zassert_equal(RADIANT_CH_OK, radiant_channel_status_get(i, &status),
			      "status_get(%u)", i);
		zassert_equal(0x00u, status,
			      "channel %u reported status 0x%02x, not 0", i,
			      status);
	}

	/* One past the end, and the top of the byte. Both are a bad channel
	 * NUMBER, which is INVALID_MESSAGE and never INVALID_PARAM. */
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_status_get(RADIANT_CHANNEL_COUNT, &status), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_status_get(255u, &status), NULL);

	radio_clean();
}

ZTEST(radiant_channel, test_period_conversion_is_the_measured_one)
{
	/*
	 * 8182 counts, not 8192. src/ant_radio.h calls 8192 "the 4 Hz that
	 * most ANT+ profiles use"; the bench measured 249,696.4 us over 744
	 * intervals, which is 8182 counts, and 8192 would be 250,000. The
	 * difference is 305 us per slot - enough to walk a receive window off
	 * a master inside a minute.
	 */
	zassert_equal(RADIANT_ANT_PLUS_PERIOD_US,
		      radiant_channel_counts_to_us(RADIANT_CHANNEL_PERIOD_ANT_PLUS),
		      "the ANT+ period is not %u us", RADIANT_ANT_PLUS_PERIOD_US);
	zassert_equal(250000u, radiant_channel_counts_to_us(8192u), NULL);
	zassert_equal(1000000u, radiant_channel_counts_to_us(32768u),
		      "32768 counts is not one second");
	zassert_equal(2500000u, radiant_channel_search_ticks_to_us(1u), NULL);
	zassert_equal(25000000u,
		      radiant_channel_search_ticks_to_us(
			      RADIANT_CHANNEL_SEARCH_TIMEOUT_DEFAULT),
		      "ANT's default search timeout is not 25 s");

	radio_clean();
}

/* ---------------------------------------------------------------------------
 * Legal transitions
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_channel, test_full_lifecycle_master)
{
	radiant_time_t t0;
	uint8_t status = 0u;

	up();
	t0 = radiant_radio_now();

	/* UNASSIGNED -> ASSIGNED */
	zassert_equal(RADIANT_CH_OK, radiant_channel_assign(0u, TYPE_MASTER, 0u, 0u),
		      NULL);
	zassert_equal(RADIANT_CH_STATE_ASSIGNED, radiant_channel_state_get(0u), NULL);
	zassert_true(radiant_channel_is_master(0u), NULL);
	zassert_false(radiant_channel_is_open(0u), NULL);

	/* Configuration is legal here and only here. */
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(0u, TEST_DEVNUM, TEST_DEVTYPE,
					 TEST_TRANSTYPE),
		      NULL);
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_period_set(0u, RADIANT_CHANNEL_PERIOD_ANT_PLUS),
		      NULL);
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_rf_freq_set(0u, RADIANT_RF_INDEX_ANT_PLUS), NULL);
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_crc_mode_set(0u, RADIANT_CH_CRC_MODE_DEFAULT),
		      NULL);

	zassert_equal(RADIANT_CH_OK, radiant_channel_status_get(0u, &status), NULL);
	zassert_equal((uint8_t)(TYPE_MASTER | RADIANT_CH_STATUS_ASSIGNED), status,
		      "assigned master status was 0x%02x", status);

	/* ASSIGNED -> TRACKING. A master never searches. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(0u, 0u, t0), NULL);
	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(0u), NULL);
	zassert_true(radiant_channel_is_open(0u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_status_get(0u, &status), NULL);
	zassert_equal((uint8_t)(TYPE_MASTER | RADIANT_CH_STATUS_TRACKING), status,
		      NULL);

	/* TRACKING -> ASSIGNED. No operation is bound, so the close completes
	 * synchronously and the event is raised before close() returns. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_close(0u, t0), NULL);
	zassert_equal(RADIANT_CH_STATE_ASSIGNED, radiant_channel_state_get(0u), NULL);
	zassert_equal(1u, evt_count_of(0u, RADIANT_CH_EVENT_CHANNEL_CLOSED),
		      "exactly one CHANNEL_CLOSED, got %u",
		      evt_count_of(0u, RADIANT_CH_EVENT_CHANNEL_CLOSED));

	/* ASSIGNED -> UNASSIGNED */
	zassert_equal(RADIANT_CH_OK, radiant_channel_unassign(0u), NULL);
	zassert_equal(RADIANT_CH_STATE_UNASSIGNED, radiant_channel_state_get(0u), NULL);
	zassert_false(radiant_channel_is_master(0u), NULL);

	radio_clean();
}

ZTEST(radiant_channel, test_full_lifecycle_slave_searches_then_tracks)
{
	struct radiant_channel_id acquired;
	struct radiant_channel_id id;
	radiant_time_t t0;
	uint8_t status = 0u;

	up();
	t0 = radiant_radio_now();

	/* A wildcard slave: every field zero is "match anything". */
	zassert_equal(RADIANT_CH_OK, radiant_channel_assign(3u, TYPE_SLAVE, 0u, 0u),
		      NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_id_set(3u, 0u, 0u, 0u), NULL);
	zassert_false(radiant_channel_is_master(3u), NULL);

	/* ASSIGNED -> SEARCHING, with the first window now rather than one
	 * period out: a searching slave has nothing to align to yet. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(3u, 0u, t0), NULL);
	zassert_equal(RADIANT_CH_STATE_SEARCHING, radiant_channel_state_get(3u), NULL);
	zassert_equal(t0, radiant_channel_next_slot(3u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_status_get(3u, &status), NULL);
	zassert_equal((uint8_t)(TYPE_SLAVE | RADIANT_CH_STATUS_SEARCHING), status,
		      NULL);

	/* The configured ID is still the wildcard while searching. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_id_get(3u, &id), NULL);
	zassert_equal(0u, id.device_number, NULL);

	/* SEARCHING -> TRACKING, resolving the wildcards. This is how a host
	 * learns which sensor it found. */
	acquired.device_number = TEST_DEVNUM;
	acquired.device_type = TEST_DEVTYPE;
	acquired.trans_type = TEST_TRANSTYPE;
	radiant_channel_on_acquired(3u, &acquired, t0 + 1234u);

	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(3u), NULL);
	zassert_equal(RADIANT_TIME_NEVER, radiant_channel_search_deadline(3u),
		      "a tracking channel still has a search deadline");
	zassert_equal(t0 + 1234u + RADIANT_ANT_PLUS_PERIOD_US,
		      radiant_channel_next_slot(3u),
		      "the slot clock did not re-anchor on the frame heard");

	zassert_equal(RADIANT_CH_OK, radiant_channel_id_get(3u, &id), NULL);
	zassert_equal(TEST_DEVNUM, id.device_number, NULL);
	zassert_equal(TEST_DEVTYPE, id.device_type, NULL);
	zassert_equal(TEST_TRANSTYPE, id.trans_type, NULL);

	/* Closing discards the acquired ID: a reopened wildcard channel must
	 * search again rather than silently narrow to last session's sensor. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_close(3u, t0), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_id_get(3u, &id), NULL);
	zassert_equal(0u, id.device_number,
		      "the acquired ID survived the close");

	radio_clean();
}

ZTEST(radiant_channel, test_tracking_returns_to_search_after_missed_slots)
{
	struct radiant_channel_id acquired = { TEST_DEVNUM, TEST_DEVTYPE,
					   TEST_TRANSTYPE };
	radiant_time_t t0;
	radiant_time_t expected;
	uint8_t i;

	up();
	t0 = radiant_radio_now();

	assign_and_id(1u, TYPE_SLAVE);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(1u, 0u, t0), NULL);
	radiant_channel_on_acquired(1u, &acquired, t0);

	expected = t0 + RADIANT_ANT_PLUS_PERIOD_US;
	zassert_equal(expected, radiant_channel_next_slot(1u), NULL);

	/* One short of the threshold: still tracking, and the phase has
	 * advanced by exactly one period per miss rather than drifting with
	 * the time the miss was noticed. */
	for (i = 0u; i < RADIANT_CHANNEL_RX_FAIL_TO_SEARCH - 1u; i++) {
		zassert_false(radiant_channel_on_slot_missed(1u, t0 + 999999u),
			      "went to search after %u misses", i + 1u);
		expected += RADIANT_ANT_PLUS_PERIOD_US;
		zassert_equal(expected, radiant_channel_next_slot(1u),
			      "phase drifted on miss %u", i + 1u);
	}
	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(1u), NULL);

	/* The last one. */
	zassert_true(radiant_channel_on_slot_missed(1u, t0 + 999999u), NULL);
	zassert_equal(RADIANT_CH_STATE_SEARCHING, radiant_channel_state_get(1u), NULL);
	zassert_equal(1u, evt_count_of(1u, RADIANT_CH_EVENT_RX_FAIL_GO_TO_SEARCH),
		      NULL);

	/* A frame heard again re-anchors and clears the counter. */
	radiant_channel_on_acquired(1u, &acquired, t0 + 5000000u);
	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(1u), NULL);
	zassert_equal(t0 + 5000000u + RADIANT_ANT_PLUS_PERIOD_US,
		      radiant_channel_next_slot(1u), NULL);

	zassert_equal(RADIANT_CH_OK, radiant_channel_close(1u, radiant_radio_now()), NULL);
	radio_clean();
}

ZTEST(radiant_channel, test_thirty_two_channels_are_independent)
{
	radiant_time_t t0;
	uint8_t i;

	up();
	t0 = radiant_radio_now();

	/* Alternate master and slave, give each its own device number and its
	 * own period, and open them all. */
	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		uint8_t type = ((i & 1u) != 0u) ? TYPE_MASTER : TYPE_SLAVE;

		zassert_equal(RADIANT_CH_OK, radiant_channel_assign(i, type, 0u, 0u),
			      "assign(%u)", i);
		zassert_equal(RADIANT_CH_OK,
			      radiant_channel_id_set(i, (uint16_t)(1000u + i),
						 TEST_DEVTYPE, TEST_TRANSTYPE),
			      "id_set(%u)", i);
		zassert_equal(RADIANT_CH_OK,
			      radiant_channel_period_set(
				      i, (uint16_t)(RADIANT_CHANNEL_PERIOD_ANT_PLUS +
						    i)),
			      "period_set(%u)", i);
		zassert_equal(RADIANT_CH_OK,
			      radiant_channel_open(i, 0u, t0 + (radiant_time_t)i),
			      "open(%u)", i);
	}

	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		struct radiant_channel_id id;
		uint16_t period = 0u;
		bool master = ((i & 1u) != 0u);

		zassert_equal(master ? RADIANT_CH_STATE_TRACKING
				     : RADIANT_CH_STATE_SEARCHING,
			      radiant_channel_state_get(i), "state(%u)", i);
		zassert_equal(master, radiant_channel_is_master(i), "master(%u)",
			      i);

		zassert_equal(RADIANT_CH_OK, radiant_channel_id_get(i, &id), NULL);
		zassert_equal((uint16_t)(1000u + i), id.device_number,
			      "channel %u read back another channel's ID", i);

		zassert_equal(RADIANT_CH_OK, radiant_channel_period_get(i, &period),
			      NULL);
		zassert_equal((uint16_t)(RADIANT_CHANNEL_PERIOD_ANT_PLUS + i),
			      period, "channel %u read back another period", i);

		/* Slot clocks differ by construction: masters wait a period,
		 * slaves do not. */
		zassert_equal(master ? (t0 + (radiant_time_t)i +
					radiant_channel_counts_to_us(period))
				     : (t0 + (radiant_time_t)i),
			      radiant_channel_next_slot(i), "next_slot(%u)", i);
	}

	/* Closing one leaves the other 31 alone. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_close(7u, t0), NULL);
	zassert_equal(RADIANT_CH_STATE_ASSIGNED, radiant_channel_state_get(7u), NULL);
	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		if (i == 7u) {
			continue;
		}
		zassert_true(radiant_channel_is_open(i),
			     "closing 7 disturbed channel %u", i);
	}

	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		if (i != 7u) {
			zassert_equal(RADIANT_CH_OK, radiant_channel_close(i, t0),
				      "close(%u)", i);
		}
		zassert_equal(RADIANT_CH_OK, radiant_channel_unassign(i),
			      "unassign(%u)", i);
	}

	/* Exactly one CHANNEL_CLOSED per channel and not one more: a host
	 * library that reopens on the event hangs if one is missed and reopens
	 * twice if one is duplicated. */
	zassert_equal((uint32_t)RADIANT_CHANNEL_COUNT, n_evts,
		      "expected %u close events, got %u",
		      (uint32_t)RADIANT_CHANNEL_COUNT, n_evts);
	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		zassert_equal(1u,
			      evt_count_of(i, RADIANT_CH_EVENT_CHANNEL_CLOSED),
			      "channel %u close events", i);
	}

	radio_clean();
}

/* ---------------------------------------------------------------------------
 * Illegal transitions, and the exact byte each one puts on the wire
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_channel, test_illegal_lifecycle_transitions)
{
	radiant_time_t t0;

	up();
	t0 = radiant_radio_now();

	/* Nothing works on an unassigned channel except a query. */
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE, radiant_channel_unassign(0u),
		      "unassign of an unassigned channel");
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE, radiant_channel_open(0u, 0u, t0),
		      "open of an unassigned channel");
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE, radiant_channel_close(0u, t0),
		      "close of an unassigned channel");
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE,
		      radiant_channel_id_set(0u, 1u, 1u, 1u),
		      "id_set on an unassigned channel");
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE,
		      radiant_channel_tx_power_set(0u, 3u, 0u),
		      "tx_power_set on an unassigned channel");
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE,
		      radiant_channel_search_timeout_set(0u, 4u),
		      "search_timeout_set on an unassigned channel");

	zassert_equal(RADIANT_CH_OK, radiant_channel_assign(0u, TYPE_MASTER, 0u, 0u),
		      NULL);

	/* Assigning twice is WRONG_STATE, not a silent reconfiguration. */
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE,
		      radiant_channel_assign(0u, TYPE_SLAVE, 0u, 0u), NULL);
	zassert_true(radiant_channel_is_master(0u),
		     "a refused assign changed the channel type");

	/* Open before the ID is set is its own code, and it is not
	 * WRONG_STATE: the channel is in the right state, it is missing a
	 * setting, and a host distinguishes the two. */
	zassert_equal(RADIANT_CH_ERR_ID_NOT_SET, radiant_channel_open(0u, 0u, t0),
		      NULL);

	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_id_set(0u, TEST_DEVNUM, TEST_DEVTYPE,
					 TEST_TRANSTYPE),
		      NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(0u, 0u, t0), NULL);

	/* Open twice. */
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE, radiant_channel_open(0u, 0u, t0),
		      NULL);

	/* Unassigning an open channel is refused rather than obliged: closing
	 * it here would swallow the CHANNEL_CLOSED the host is waiting on. */
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE, radiant_channel_unassign(0u), NULL);
	zassert_equal(RADIANT_CH_STATE_TRACKING, radiant_channel_state_get(0u), NULL);
	zassert_equal(0u, evt_count_of(0u, RADIANT_CH_EVENT_CHANNEL_CLOSED),
		      "a refused unassign raised a close event");

	/* Everything that retunes the radio is refused while open. */
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE,
		      radiant_channel_id_set(0u, 1u, 1u, 1u), "id_set while open");
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE, radiant_channel_period_set(0u, 4096u),
		      "period_set while open");
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE, radiant_channel_rf_freq_set(0u, 57u),
		      "rf_freq_set while open");
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE, radiant_channel_crc_mode_set(0u, 0u),
		      "crc_mode_set while open");
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE,
		      radiant_channel_freq_hop_table_set(0u, 1u, 2u, 3u),
		      "freq_hop_table_set while open");

	/* What does NOT retune the radio stays legal while open. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_tx_power_set(0u, 4u, 0u),
		      "tx_power_set while open should be legal");
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_timeout_set(0u, 12u),
		      "search_timeout_set while open should be legal");

	/* Close, then close again. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_close(0u, t0), NULL);
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE, radiant_channel_close(0u, t0),
		      "a second close");
	zassert_equal(1u, evt_count_of(0u, RADIANT_CH_EVENT_CHANNEL_CLOSED),
		      "a refused close raised a second event");

	radio_clean();
}

ZTEST(radiant_channel, test_out_of_range_channel_is_invalid_message)
{
	const uint8_t bad = RADIANT_CHANNEL_COUNT;
	struct radiant_channel_id id;
	uint8_t table[3];
	uint16_t u16 = 0u;
	uint8_t u8 = 0u;
	uint8_t u8b = 0u;

	/*
	 * A bad channel NUMBER is INVALID_MESSAGE and a bad parameter VALUE is
	 * INVALID_PARAMETER_PROVIDED. docs/sdk-ant-contract.md calls the
	 * distinction load-bearing and host-observable, and
	 * tools/ant_conformance.py byte-diffs it, so it is checked on every
	 * entry point rather than sampled.
	 */
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_assign(bad, TYPE_MASTER, 0u, 0u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE, radiant_channel_unassign(bad),
		      NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE, radiant_channel_open(bad, 0u, 0u),
		      NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE, radiant_channel_close(bad, 0u),
		      NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_id_set(bad, 1u, 1u, 1u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE, radiant_channel_id_get(bad, &id),
		      NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_period_set(bad, 8182u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_period_get(bad, &u16), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_rf_freq_set(bad, 57u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_rf_freq_get(bad, &u8), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_tx_power_set(bad, 3u, 0u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_tx_power_get(bad, &u8, &u8b), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_search_timeout_set(bad, 10u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_lp_search_timeout_set(bad, 10u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_crc_mode_set(bad, 0u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_crc_mode_get(bad, &u8), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_freq_hop_table_set(bad, 1u, 2u, 3u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_freq_hop_table_get(bad, table), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_search_waveform_set(bad, 316u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_search_waveform_get(bad, &u16), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_prox_search_set(bad, 1u, 0u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_prox_search_get(bad, &u8, &u8b), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_search_priority_set(bad, 1u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_search_priority_get(bad, &u8), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_sharing_cycles_set(bad, 2u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_sharing_cycles_get(bad, &u8), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_sdu_mask_config_set(bad, 0u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_sdu_mask_config_get(bad, &u8), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_MESSAGE,
		      radiant_channel_status_get(bad, &u8), NULL);

	radio_clean();
}

ZTEST(radiant_channel, test_bad_parameter_values_and_bad_network)
{
	uint8_t i;

	/* The network number has its own code, and it is checked AFTER the
	 * state - an already-assigned channel is WRONG_STATE whatever network
	 * the host names. */
	zassert_equal(RADIANT_CH_ERR_INVALID_NETWORK,
		      radiant_channel_assign(0u, TYPE_MASTER,
					 RADIANT_CHANNEL_NETWORK_COUNT, 0u),
		      NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_NETWORK,
		      radiant_channel_assign(0u, TYPE_MASTER, 255u, 0u), NULL);
	zassert_equal(RADIANT_CH_STATE_UNASSIGNED, radiant_channel_state_get(0u),
		      "a refused assign left the channel assigned");

	for (i = 0u; i < RADIANT_CHANNEL_NETWORK_COUNT; i++) {
		zassert_equal(RADIANT_CH_OK,
			      radiant_channel_assign(i, TYPE_MASTER, i, 0u),
			      "network %u should be assignable", i);
	}

	/* Period: zero is not a period, and anything under the floor is a
	 * schedule the radio can never keep. */
	zassert_equal(RADIANT_CH_ERR_INVALID_PARAM, radiant_channel_period_set(0u, 0u),
		      NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_PARAM,
		      radiant_channel_period_set(
			      0u, (uint16_t)(RADIANT_CHANNEL_PERIOD_MIN_COUNTS - 1u)),
		      NULL);
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_period_set(0u, RADIANT_CHANNEL_PERIOD_MIN_COUNTS),
		      NULL);
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_period_set(0u, RADIANT_CHANNEL_PERIOD_MAX_COUNTS),
		      NULL);

	/* radiant_sched.c may raise the floor from caps, and may not lower it. */
	radiant_channel_period_floor_set(1000u);
	zassert_equal(RADIANT_CH_ERR_INVALID_PARAM,
		      radiant_channel_period_set(0u, 999u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_period_set(0u, 1000u), NULL);
	radiant_channel_period_floor_set(1u);
	zassert_equal(RADIANT_CH_ERR_INVALID_PARAM,
		      radiant_channel_period_set(
			      0u, (uint16_t)(RADIANT_CHANNEL_PERIOD_MIN_COUNTS - 1u)),
		      "the protocol floor was lowered");

	/* RF index: 0..124 means 2400..2524 MHz. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_rf_freq_set(0u, RADIANT_RF_INDEX_MAX),
		      NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_PARAM,
		      radiant_channel_rf_freq_set(0u, RADIANT_RF_INDEX_MAX + 1u), NULL);

	/* CRC: the supported set is exactly {default}. Accepting and ignoring
	 * anything else gives a host a link that works and framing that does
	 * not. */
	zassert_equal(RADIANT_CH_ERR_INVALID_PARAM, radiant_channel_crc_mode_set(0u, 1u),
		      NULL);

	/* Frequency-hop table: every entry is an RF index. */
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_freq_hop_table_set(0u, 0u, 57u,
						     RADIANT_RF_INDEX_MAX),
		      NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_PARAM,
		      radiant_channel_freq_hop_table_set(0u, 0u, 57u, 125u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_PARAM,
		      radiant_channel_freq_hop_table_set(0u, 125u, 57u, 1u), NULL);

	/* Proximity: 0 disables, 1..10 are thresholds. */
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_prox_search_set(
			      0u, RADIANT_CH_PROX_THRESHOLD_MAX, 0u),
		      NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_PARAM,
		      radiant_channel_prox_search_set(
			      0u, RADIANT_CH_PROX_THRESHOLD_MAX + 1u, 0u),
		      NULL);

	/* Transmit power: 0..5, or anything at all behind the custom bit. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_tx_power_set(0u, 5u, 0u), NULL);
	zassert_equal(RADIANT_CH_ERR_INVALID_PARAM,
		      radiant_channel_tx_power_set(0u, 6u, 0u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_tx_power_set(0u, 0x80u, 0x42u),
		      "the custom bit means `custom` is the value, so there is "
		      "no level to range-check");

	radio_clean();
}

/* ---------------------------------------------------------------------------
 * The measured one: a master does not transmit when you open it
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_channel, test_master_first_transmission_is_one_period_not_zero)
{
	static const uint8_t body[10] = { TEST_TRANSTYPE, RADIANT_CTRL_BROADCAST,
					  1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u };
	struct radiant_tx_req req;
	const struct fake_radio_arm *arm;
	uint32_t op = 0u;
	radiant_time_t t_open;
	radiant_time_t want;

	up();
	t_open = radiant_radio_now();

	assign_and_id(0u, TYPE_MASTER);
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_period_set(0u, RADIANT_CHANNEL_PERIOD_ANT_PLUS),
		      NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(0u, 0u, t_open), NULL);

	/*
	 * Measured, 8 of 8 runs (docs/ant-radio-link.md, "Master open-time"):
	 * MESG_OPEN_CHANNEL to the first EVENT_TX is one full channel period.
	 * A Zwift-style host that expects data immediately after opening a
	 * master is going to wait a slot, and that is correct behaviour rather
	 * than latency to be optimised away.
	 */
	want = t_open + RADIANT_ANT_PLUS_PERIOD_US;
	zassert_equal(want, radiant_channel_next_slot(0u),
		      "a master's first slot was not one period out");
	zassert_not_equal(t_open, radiant_channel_next_slot(0u),
			  "a master transmitted the instant it was opened");

	/* Now prove a backend accepts that decision, rather than only that the
	 * arithmetic is right. This is what radiant_sched.c will do. */
	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.power.dbm = 0;
	req.body = body;
	req.body_len = (uint8_t)sizeof(body);
	req.t_sync_at = radiant_channel_next_slot(0u);
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_tx(&req, &op), "tx arm failed");
	radiant_channel_bind_op(0u, op);

	arm = fake_radio_last_arm();
	zassert_equal(FAKE_RADIO_ARM_TX, arm->kind, NULL);
	zassert_equal(want, arm->t_sync_at,
		      "the arm asked for the wrong instant");

	fake_radio_advance_to(want + 1000u);

	zassert_equal(1u, n_terminal, "expected exactly one terminal event");
	zassert_equal(0u, n_terminal_stale, "the transmit event was stale");
	zassert_equal(-1, radiant_channel_op_owner(op),
		      "the terminal event did not release the binding");

	/* The next slot is one period after the one that just went out. */
	radiant_channel_on_slot(0u, want);
	zassert_equal(want + RADIANT_ANT_PLUS_PERIOD_US, radiant_channel_next_slot(0u),
		      NULL);

	zassert_equal(RADIANT_CH_OK, radiant_channel_close(0u, radiant_radio_now()), NULL);
	radio_clean();
}

ZTEST(radiant_channel, test_open_offset_phases_on_top_of_the_period)
{
	radiant_time_t t_open;

	up();
	t_open = radiant_radio_now();

	/*
	 * The offset is a phase adjustment applied on top of the period a
	 * master waits anyway, not a replacement for it. Read the other way,
	 * an offset of 0 would mean "transmit now", which the open-time
	 * measurement falsifies. src/ant_radio.h does not say which reading it
	 * means; this is the only one consistent with the bench.
	 */
	assign_and_id(0u, TYPE_MASTER);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(0u, 4096u, t_open), NULL);
	zassert_equal(t_open + 125000u + RADIANT_ANT_PLUS_PERIOD_US,
		      radiant_channel_next_slot(0u),
		      "4096 counts is 125 ms and must be added to the period");

	/* Two masters phased half a period apart do not want the radio at the
	 * same instant, which is the whole purpose of the parameter. */
	assign_and_id(1u, TYPE_MASTER);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(1u, 0u, t_open), NULL);
	zassert_not_equal(radiant_channel_next_slot(0u), radiant_channel_next_slot(1u),
			  "the offset did not phase the two slots apart");

	/* A slave's offset delays its first window and nothing else. */
	assign_and_id(2u, TYPE_SLAVE);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(2u, 4096u, t_open), NULL);
	zassert_equal(t_open + 125000u, radiant_channel_next_slot(2u), NULL);

	zassert_equal(RADIANT_CH_OK, radiant_channel_close(0u, t_open), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_close(1u, t_open), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_close(2u, t_open), NULL);
	radio_clean();
}

ZTEST(radiant_channel, test_earliest_slot_picks_the_soonest_open_channel)
{
	radiant_time_t t0;
	uint8_t which = 0xFFu;

	up();
	t0 = radiant_radio_now();

	zassert_equal(RADIANT_TIME_NEVER, radiant_channel_earliest_slot(&which),
		      "an idle stack wants the radio");

	assign_and_id(5u, TYPE_SLAVE);
	assign_and_id(9u, TYPE_SLAVE);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(5u, 0u, t0 + 5000u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(9u, 0u, t0 + 1000u), NULL);

	zassert_equal(t0 + 1000u, radiant_channel_earliest_slot(&which), NULL);
	zassert_equal(9u, which, NULL);

	/* A CLOSING channel stops wanting the radio immediately, even though
	 * its clock still holds a time - arming one more window for it would
	 * produce the second in-flight operation that makes the close race
	 * hard. */
	(void)arm_rx_for(9u, t0 + 1000u, t0 + 2000u);
	zassert_equal(RADIANT_CH_OK, radiant_channel_close(9u, t0), NULL);
	zassert_equal(RADIANT_CH_STATE_CLOSING, radiant_channel_state_get(9u), NULL);
	zassert_equal(RADIANT_TIME_NEVER, radiant_channel_next_slot(9u), NULL);
	zassert_equal(t0 + 5000u, radiant_channel_earliest_slot(&which), NULL);
	zassert_equal(5u, which, NULL);

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_abort(), NULL);
	zassert_equal(RADIANT_CH_STATE_ASSIGNED, radiant_channel_state_get(9u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_close(5u, t0), NULL);
	radio_clean();
}

/* ---------------------------------------------------------------------------
 * Search timeouts
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_channel, test_search_timeout_expiry_closes_the_channel)
{
	radiant_time_t t0;

	up();
	t0 = radiant_radio_now();

	assign_and_id(4u, TYPE_SLAVE);
	/* One tick: 2.5 s. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_timeout_set(4u, 1u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_lp_search_timeout_set(4u, 0u),
		      NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(4u, 0u, t0), NULL);

	zassert_equal(t0 + 2500000u, radiant_channel_search_deadline(4u), NULL);

	/* One microsecond short. */
	zassert_equal(0u, radiant_channel_tick(t0 + 2499999u),
		      "the search gave up early");
	zassert_equal(RADIANT_CH_STATE_SEARCHING, radiant_channel_state_get(4u), NULL);
	zassert_equal(0u, n_evts, "an event was raised before the deadline");

	/* And at it. Both events, in this order: a host library waits for
	 * RX_SEARCH_TIMEOUT to know the search failed and for CHANNEL_CLOSED
	 * to know it may unassign. */
	zassert_equal(1u, radiant_channel_tick(t0 + 2500000u), NULL);
	zassert_equal(2u, n_evts, "expected two events, got %u", n_evts);
	zassert_equal(4u, evts[0].channel, NULL);
	zassert_equal(RADIANT_CH_EVENT_RX_SEARCH_TIMEOUT, evts[0].code, NULL);
	zassert_equal(4u, evts[1].channel, NULL);
	zassert_equal(RADIANT_CH_EVENT_CHANNEL_CLOSED, evts[1].code, NULL);

	zassert_equal(RADIANT_CH_STATE_ASSIGNED, radiant_channel_state_get(4u), NULL);
	zassert_equal(1u, radiant_channel_search_timeout_count(), NULL);

	/* And it does not fire again. */
	zassert_equal(0u, radiant_channel_tick(t0 + 100000000u), NULL);
	zassert_equal(2u, n_evts, NULL);

	/* A channel closed by a search timeout is unassignable, which is the
	 * whole reason the close event is raised as well as the timeout. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_unassign(4u), NULL);

	radio_clean();
}

ZTEST(radiant_channel, test_search_timeout_budget_is_high_plus_low_priority)
{
	radiant_time_t t0;

	up();
	t0 = radiant_radio_now();

	assign_and_id(0u, TYPE_SLAVE);
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_timeout_set(0u, 2u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_lp_search_timeout_set(0u, 3u),
		      NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(0u, 0u, t0), NULL);

	/* Low priority runs after high priority expires, so the budget is the
	 * sum: 5 ticks = 12.5 s. */
	zassert_equal(t0 + 12500000u, radiant_channel_search_deadline(0u), NULL);
	zassert_equal(0u, radiant_channel_tick(t0 + 12499999u), NULL);
	zassert_equal(1u, radiant_channel_tick(t0 + 12500000u), NULL);

	/* 255 on either byte is "search forever", and it is the one value that
	 * is not a duration. */
	assign_and_id(1u, TYPE_SLAVE);
	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_search_timeout_set(
			      1u, RADIANT_CHANNEL_SEARCH_INFINITE),
		      NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(1u, 0u, t0), NULL);
	zassert_equal(RADIANT_TIME_NEVER, radiant_channel_search_deadline(1u), NULL);
	zassert_equal(0u, radiant_channel_tick(t0 + 0xFFFFFFFFull), NULL);
	zassert_equal(RADIANT_CH_STATE_SEARCHING, radiant_channel_state_get(1u), NULL);

	/* Both zero means the search has no budget at all and the next tick
	 * closes it, rather than leaving the channel in SEARCHING forever. */
	assign_and_id(2u, TYPE_SLAVE);
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_timeout_set(2u, 0u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_lp_search_timeout_set(2u, 0u),
		      NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(2u, 0u, t0), NULL);
	zassert_equal(1u, radiant_channel_tick(t0), NULL);
	zassert_equal(RADIANT_CH_STATE_ASSIGNED, radiant_channel_state_get(2u), NULL);

	zassert_equal(RADIANT_CH_OK, radiant_channel_close(1u, t0), NULL);
	radio_clean();
}

ZTEST(radiant_channel, test_extending_a_timeout_mid_search_does_not_restart_it)
{
	radiant_time_t t0;

	up();
	t0 = radiant_radio_now();

	assign_and_id(0u, TYPE_SLAVE);
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_timeout_set(0u, 2u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(0u, 0u, t0), NULL);
	zassert_equal(t0 + 5000000u, radiant_channel_search_deadline(0u), NULL);

	/* Four seconds in, the host doubles the timeout. The deadline is
	 * re-derived from when the search BEGAN, so the channel now gives up
	 * at 10 s and not at 14 s. A restart would let a host that polls the
	 * setting keep a channel searching forever without meaning to. */
	zassert_equal(0u, radiant_channel_tick(t0 + 4000000u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_timeout_set(0u, 4u), NULL);
	zassert_equal(t0 + 10000000u, radiant_channel_search_deadline(0u), NULL);
	zassert_equal(0u, radiant_channel_tick(t0 + 9999999u), NULL);
	zassert_equal(1u, radiant_channel_tick(t0 + 10000000u), NULL);

	radio_clean();
}

ZTEST(radiant_channel, test_search_timeout_with_an_operation_in_flight_defers)
{
	radiant_time_t t0;
	uint32_t op;

	up();
	t0 = radiant_radio_now();

	assign_and_id(0u, TYPE_SLAVE);
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_timeout_set(0u, 1u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(0u, 0u, t0), NULL);

	op = arm_rx_for(0u, t0 + 1000u, t0 + 4000000u);

	/* The deadline passes while the window is still open. */
	fake_radio_advance_to(t0 + 2500000u);
	zassert_equal(1u, radiant_channel_tick(radiant_radio_now()), NULL);

	/* RX_SEARCH_TIMEOUT now; CHANNEL_CLOSED when the operation ends. */
	zassert_equal(RADIANT_CH_STATE_CLOSING, radiant_channel_state_get(0u), NULL);
	zassert_equal(1u, evt_count_of(0u, RADIANT_CH_EVENT_RX_SEARCH_TIMEOUT),
		      NULL);
	zassert_equal(0u, evt_count_of(0u, RADIANT_CH_EVENT_CHANNEL_CLOSED),
		      "the close completed while the radio still had the op");
	zassert_equal(0, radiant_channel_op_owner(op),
		      "the binding was dropped early");

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_abort(), NULL);

	zassert_equal(RADIANT_CH_STATE_ASSIGNED, radiant_channel_state_get(0u), NULL);
	zassert_equal(1u, evt_count_of(0u, RADIANT_CH_EVENT_CHANNEL_CLOSED), NULL);
	zassert_equal(0u, n_terminal_stale,
		      "the aborted operation's terminal event was not "
		      "attributed to the channel that owned it");

	radio_clean();
}

/* ---------------------------------------------------------------------------
 * Close with an operation in flight, and the late event that follows
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_channel, test_close_with_an_operation_in_flight)
{
	radiant_time_t t0;
	uint32_t op;
	uint8_t status = 0u;

	up();
	t0 = radiant_radio_now();

	assign_and_id(2u, TYPE_SLAVE);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(2u, 0u, t0), NULL);
	op = arm_rx_for(2u, t0 + 1000u, t0 + 500000u);

	zassert_equal(RADIANT_CH_OK, radiant_channel_close(2u, t0), NULL);
	zassert_equal(RADIANT_CH_STATE_CLOSING, radiant_channel_state_get(2u), NULL);

	/*
	 * While CLOSING the channel reports the state it is leaving, not
	 * ASSIGNED. Reporting ASSIGNED would tell a host it may unassign a
	 * channel that radiant_channel_unassign() then refuses, and a status byte
	 * that disagrees with the next call's return code is worse than one
	 * that lags.
	 */
	zassert_equal(RADIANT_CH_OK, radiant_channel_status_get(2u, &status), NULL);
	zassert_equal((uint8_t)(TYPE_SLAVE | RADIANT_CH_STATUS_SEARCHING), status,
		      "a closing channel reported 0x%02x", status);

	zassert_equal(0u, evt_count_of(2u, RADIANT_CH_EVENT_CHANNEL_CLOSED),
		      "the close completed before the operation ended");
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE, radiant_channel_unassign(2u),
		      "a closing channel was unassignable");
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE, radiant_channel_open(2u, 0u, t0),
		      "a closing channel was reopenable");
	zassert_equal(RADIANT_CH_ERR_WRONG_STATE, radiant_channel_close(2u, t0),
		      "a second close was accepted");

	/*
	 * The abort delivers the terminal event before it returns - the HAL
	 * says so explicitly, so the core never has to reason about an
	 * operation that ended without one - and that event is what completes
	 * the close.
	 */
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_abort(), NULL);
	zassert_equal(1u, n_terminal, NULL);
	zassert_equal(0u, n_terminal_stale, NULL);

	zassert_equal(RADIANT_CH_STATE_ASSIGNED, radiant_channel_state_get(2u), NULL);
	zassert_equal(1u, evt_count_of(2u, RADIANT_CH_EVENT_CHANNEL_CLOSED),
		      "exactly one CHANNEL_CLOSED per successful close");
	zassert_equal(-1, radiant_channel_op_owner(op), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_unassign(2u), NULL);

	radio_clean();
}

ZTEST(radiant_channel, test_late_terminal_event_after_reopen_is_stale)
{
	radiant_time_t t0;
	uint32_t op_old;
	uint32_t op_new;

	up();
	t0 = radiant_radio_now();

	assign_and_id(6u, TYPE_SLAVE);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(6u, 0u, t0), NULL);
	op_old = arm_rx_for(6u, t0 + 1000u, t0 + 500000u);

	zassert_equal(RADIANT_CH_OK, radiant_channel_close(6u, t0), NULL);
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_abort(), NULL);
	zassert_equal(RADIANT_CH_STATE_ASSIGNED, radiant_channel_state_get(6u), NULL);

	/* Reopen immediately. This is the close-then-reopen race: the old
	 * operation is gone, a new one is armed, and the two must never be
	 * confused. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(6u, 0u, radiant_radio_now()),
		      NULL);
	op_new = arm_rx_for(6u, radiant_radio_now() + 1000u,
			    radiant_radio_now() + 500000u);
	zassert_not_equal(op_old, op_new, "the mock reused an op id");
	zassert_equal(6, radiant_channel_op_owner(op_new), NULL);

	/*
	 * Now the late event. On real hardware the frame was already in the
	 * receiver's pipeline when the abort ran, so it arrives after the core
	 * moved on. The mock will never do this by itself, and a module that
	 * has not been shown one has not been tested against it.
	 */
	zassert_equal(0u, radiant_channel_stale_op_count(), NULL);
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_inject_late_tx(op_old,
						RADIANT_RADIO_STATUS_ABORTED),
		      "late injection failed");

	zassert_equal(1u, radiant_channel_stale_op_count(),
		      "the late event was not recognised as stale");
	zassert_equal(1u, n_terminal_stale, NULL);

	/* And nothing about the live channel moved. */
	zassert_equal(RADIANT_CH_STATE_SEARCHING, radiant_channel_state_get(6u), NULL);
	zassert_equal(6, radiant_channel_op_owner(op_new),
		      "a late event for a dead op released the live binding");
	zassert_equal(1u, evt_count_of(6u, RADIANT_CH_EVENT_CHANNEL_CLOSED),
		      "the late event raised a second close");

	zassert_equal(RADIANT_CH_OK, radiant_channel_close(6u, radiant_radio_now()), NULL);
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_abort(), NULL);
	radio_clean();
}

ZTEST(radiant_channel, test_op_zero_never_owns_a_channel)
{
	/*
	 * radiant_radio_tx()/rx() only ever hand out non-zero ids, so 0 is the
	 * cleared binding. If it could match, every idle channel would claim
	 * every event whose op failed to be recorded - which is exactly the
	 * bug the op id exists to prevent, arriving through the back door.
	 */
	zassert_equal(-1, radiant_channel_op_owner(0u), NULL);

	assign_and_id(0u, TYPE_SLAVE);
	zassert_equal(-1, radiant_channel_op_owner(0u), NULL);

	radiant_channel_bind_op(0u, 77u);
	zassert_equal(0, radiant_channel_op_owner(77u), NULL);
	zassert_equal(-1, radiant_channel_op_owner(0u), NULL);

	radiant_channel_bind_op(0u, 0u);
	zassert_equal(-1, radiant_channel_op_owner(77u), NULL);
	zassert_equal(-1, radiant_channel_op_owner(0u), NULL);

	radio_clean();
}

/* ---------------------------------------------------------------------------
 * Stack reset
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_channel, test_stack_reset_closes_and_unassigns_everything)
{
	radiant_time_t t0;
	uint8_t i;
	uint8_t status = 0xFFu;

	up();
	t0 = radiant_radio_now();

	for (i = 0u; i < 8u; i++) {
		assign_and_id(i, (i & 1u) ? TYPE_MASTER : TYPE_SLAVE);
		zassert_equal(RADIANT_CH_OK, radiant_channel_open(i, 0u, t0), NULL);
	}
	/* Channels 8 and 9 are assigned but never opened; they must be
	 * unassigned by the reset without a close event, because they were
	 * never on air and a host that saw one would think it missed
	 * something. */
	assign_and_id(8u, TYPE_SLAVE);
	assign_and_id(9u, TYPE_MASTER);

	n_evts = 0u;
	radiant_channel_reset_all();

	zassert_equal(8u, n_evts,
		      "expected one CHANNEL_CLOSED per open channel, got %u",
		      n_evts);
	for (i = 0u; i < 8u; i++) {
		zassert_equal(1u, evt_count_of(i, RADIANT_CH_EVENT_CHANNEL_CLOSED),
			      "channel %u", i);
	}

	for (i = 0u; i < RADIANT_CHANNEL_COUNT; i++) {
		zassert_equal(RADIANT_CH_STATE_UNASSIGNED,
			      radiant_channel_state_get(i), "channel %u", i);
		zassert_equal(RADIANT_CH_OK, radiant_channel_status_get(i, &status),
			      NULL);
		zassert_equal(0x00u, status, "channel %u status 0x%02x", i,
			      status);
	}

	radio_clean();
}

ZTEST(radiant_channel, test_reset_orphans_an_operation_and_the_event_is_stale)
{
	radiant_time_t t0;
	uint32_t op;

	up();
	t0 = radiant_radio_now();

	assign_and_id(0u, TYPE_SLAVE);
	zassert_equal(RADIANT_CH_OK, radiant_channel_open(0u, 0u, t0), NULL);
	op = arm_rx_for(0u, t0 + 1000u, t0 + 500000u);

	radiant_channel_reset_all();
	zassert_equal(RADIANT_CH_STATE_UNASSIGNED, radiant_channel_state_get(0u), NULL);
	zassert_equal(-1, radiant_channel_op_owner(op),
		      "the reset left a binding behind");

	/*
	 * radiant_sched.c is required to abort before resetting; this is what
	 * happens when the terminal event lands afterwards. It is the same
	 * path a late event takes, which is the point of having only one.
	 */
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_abort(), NULL);
	zassert_equal(1u, n_terminal, NULL);
	zassert_equal(1u, n_terminal_stale, NULL);
	zassert_equal(1u, radiant_channel_stale_op_count(), NULL);

	radio_clean();
}

/* ---------------------------------------------------------------------------
 * Configuration round-trips
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_channel, test_configuration_round_trips_and_defaults)
{
	uint8_t table[3] = { 0u, 0u, 0u };
	uint16_t u16 = 0u;
	uint8_t a = 0u;
	uint8_t b = 0u;

	up();

	/* Defaults, on a freshly assigned channel. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_assign(0u, TYPE_SLAVE, 0u, 0u),
		      NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_period_get(0u, &u16), NULL);
	zassert_equal(RADIANT_CHANNEL_PERIOD_ANT_PLUS, u16,
		      "the default period is not the measured ANT+ one");
	zassert_equal(RADIANT_CH_OK, radiant_channel_rf_freq_get(0u, &a), NULL);
	zassert_equal(66u, a,
		      "the default RF index is not ANT's power-on 66; 57 would "
		      "let a host that forgot to set it half-work");
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_waveform_get(0u, &u16),
		      NULL);
	zassert_equal(RADIANT_CH_SEARCH_WAVEFORM_DEFAULT, u16, NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_sdu_mask_config_get(0u, &a), NULL);
	zassert_equal(RADIANT_CH_SDU_MASK_OFF, a, NULL);

	/* Round-trips. tools/ant_features.py checks that the advisory ones
	 * come back even where a backend merges windows and ignores them. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_waveform_set(0u, 200u),
		      NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_waveform_get(0u, &u16),
		      NULL);
	zassert_equal(200u, u16, NULL);

	zassert_equal(RADIANT_CH_OK, radiant_channel_sharing_cycles_set(0u, 7u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_sharing_cycles_get(0u, &a), NULL);
	zassert_equal(7u, a, NULL);

	zassert_equal(RADIANT_CH_OK, radiant_channel_prox_search_set(0u, 4u, 0x9Bu),
		      NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_prox_search_get(0u, &a, &b), NULL);
	zassert_equal(4u, a, NULL);
	zassert_equal(0x9Bu, b, NULL);

	/* Any non-zero priority normalises to 1: the contract documents 0 and
	 * 1 and permits no INVALID_PARAMETER_PROVIDED here, so a 2 cannot be
	 * refused with a code this call never returns. */
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_priority_set(0u, 2u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_priority_get(0u, &a), NULL);
	zassert_equal(1u, a, NULL);

	zassert_equal(RADIANT_CH_OK, radiant_channel_tx_power_set(0u, 0x80u, 0x33u),
		      NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_tx_power_get(0u, &a, &b), NULL);
	zassert_equal(0x80u, a, NULL);
	zassert_equal(0x33u, b, NULL);

	zassert_equal(RADIANT_CH_OK,
		      radiant_channel_freq_hop_table_set(0u, 3u, 39u, 57u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_freq_hop_table_get(0u, table),
		      NULL);
	zassert_equal(3u, table[0], NULL);
	zassert_equal(39u, table[1], NULL);
	zassert_equal(57u, table[2], NULL);

	zassert_equal(RADIANT_CH_OK, radiant_channel_sdu_mask_config_set(0u, 0x81u),
		      NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_sdu_mask_config_get(0u, &a), NULL);
	zassert_equal(0x81u, a, NULL);

	/*
	 * Assigning resets the configuration. A channel reassigned to a
	 * different type must not inherit the last session's ID, or a host
	 * that opens without setting one silently tracks the previous sensor
	 * instead of being told RADIANT_CH_ERR_ID_NOT_SET.
	 */
	zassert_equal(RADIANT_CH_OK, radiant_channel_unassign(0u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_assign(0u, TYPE_MASTER, 1u, 0u),
		      NULL);
	zassert_equal(RADIANT_CH_ERR_ID_NOT_SET,
		      radiant_channel_open(0u, 0u, radiant_radio_now()),
		      "a reassigned channel inherited the previous ID");
	zassert_equal(RADIANT_CH_OK, radiant_channel_search_waveform_get(0u, &u16),
		      NULL);
	zassert_equal(RADIANT_CH_SEARCH_WAVEFORM_DEFAULT, u16,
		      "assign did not reset the configuration");

	radio_clean();
}

ZTEST(radiant_channel, test_status_byte_carries_type_network_and_state)
{
	static const uint8_t types[] = {
		TYPE_SLAVE,          TYPE_MASTER,         TYPE_SHARED_SLAVE,
		TYPE_SHARED_MASTER,  TYPE_SLAVE_RX_ONLY,  TYPE_MASTER_TX_ONLY
	};
	radiant_time_t t0;
	uint8_t status = 0u;
	uint8_t i;

	up();
	t0 = radiant_radio_now();

	/*
	 * Rev 5.1 section 9.5.7.1: state in bits 1:0, network in bits 3:2,
	 * channel type in bits 7:4. The type values already sit in the top
	 * nibble, so shifting them would report a channel type of zero on
	 * every live master and the only symptom would be a host that refuses
	 * to pair.
	 */
	for (i = 0u; i < (uint8_t)ARRAY_SIZE(types); i++) {
		uint8_t net = (uint8_t)(i % RADIANT_CHANNEL_NETWORK_COUNT);
		bool master = (types[i] & RADIANT_CH_TYPE_MASTER_BIT) != 0u;

		zassert_equal(RADIANT_CH_OK,
			      radiant_channel_assign(i, types[i], net, 0u), NULL);
		zassert_equal(RADIANT_CH_OK, radiant_channel_status_get(i, &status),
			      NULL);
		zassert_equal((uint8_t)(types[i] | (uint8_t)(net << 2) |
					RADIANT_CH_STATUS_ASSIGNED),
			      status, "assigned type 0x%02x net %u -> 0x%02x",
			      types[i], net, status);

		zassert_equal(RADIANT_CH_OK,
			      radiant_channel_id_set(i, TEST_DEVNUM, TEST_DEVTYPE,
						 TEST_TRANSTYPE),
			      NULL);
		zassert_equal(RADIANT_CH_OK, radiant_channel_open(i, 0u, t0), NULL);
		zassert_equal(RADIANT_CH_OK, radiant_channel_status_get(i, &status),
			      NULL);
		zassert_equal((uint8_t)(types[i] | (uint8_t)(net << 2) |
					(master ? RADIANT_CH_STATUS_TRACKING
						: RADIANT_CH_STATUS_SEARCHING)),
			      status, "open type 0x%02x -> 0x%02x", types[i],
			      status);
		zassert_equal(master, radiant_channel_is_master(i),
			      "type 0x%02x master bit", types[i]);

		zassert_equal(RADIANT_CH_OK, radiant_channel_close(i, t0), NULL);
	}

	radio_clean();
}

ZTEST(radiant_channel, test_an_unrecognised_channel_type_is_accepted)
{
	uint8_t status = 0u;

	/*
	 * antr_channel_assign()'s permitted error set is {NO_ERROR,
	 * INVALID_MESSAGE, CHANNEL_IN_WRONG_STATE, INVALID_NETWORK_NUMBER},
	 * and INVALID_MESSAGE is already spoken for by the channel number, so
	 * there is no code left with which to reject a channel type. Returning
	 * INVALID_PARAMETER_PROVIDED would put a byte on the wire that the
	 * contract says this function never produces. The type is therefore
	 * carried verbatim, only bit 4 is interpreted, and the byte comes back
	 * in the status reply.
	 */
	up();
	zassert_equal(RADIANT_CH_OK, radiant_channel_assign(0u, 0x70u, 0u, 0u), NULL);
	zassert_equal(RADIANT_CH_OK, radiant_channel_status_get(0u, &status), NULL);
	zassert_equal((uint8_t)(0x70u | RADIANT_CH_STATUS_ASSIGNED), status, NULL);
	zassert_true(radiant_channel_is_master(0u),
		     "bit 4 is the master bit whatever the rest of the byte "
		     "says");

	radio_clean();
}
