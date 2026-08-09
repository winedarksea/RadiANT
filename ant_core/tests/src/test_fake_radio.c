/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work. Written against
 * ant_core/include/ant_radio_hal.h, ant_core/tests/fake_radio.h and the
 * measurements in docs/spike-a-results.md. Nothing here derives from sdk-ant,
 * from libant.a, or from any adopter-gated ANT+ device profile document.
 *
 * ---------------------------------------------------------------------------
 * Tests for the mock radio
 * ---------------------------------------------------------------------------
 * fake_radio.c is the only backend six of the seven core modules will ever run
 * against, so an untested mock is six modules' worth of tests that verify a
 * fiction. This suite is the check on the checker.
 *
 * What it covers, and why each one is here rather than assumed:
 *
 *   - the virtual clock moves only when a test moves it, and it starts below
 *     the 32-bit microsecond boundary on purpose;
 *   - op ids are non-zero, unique and monotonic, and a late event carrying a
 *     dead one is distinguishable from a live one;
 *   - every accepted operation produces exactly one terminal event, and none
 *     produces two;
 *   - the callback contract's MUST NOTs are enforced rather than documented;
 *   - caps are settable per test and the arm path actually reads them, which
 *     is what makes a scheduler test at max_filters == 2 mean anything;
 *   - a queued frame arrives at its own t_sync with the filter index that
 *     matched, and the address/body split follows the *receiver's* format -
 *     the property ant_search.c's devnum recovery depends on;
 *   - every failure mode the core has to survive can be provoked: a silent
 *     window, a CRC failure, an arm that is already too late, and an abort.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "../fake_radio.h"

/* ---------------------------------------------------------------------------
 * Fixtures
 * ---------------------------------------------------------------------------
 */

/*
 * CRC-16/CCITT-FALSE covering the on-air address, per docs/ant-radio-link.md
 * and confirmed on 2,164 real frames in docs/spike-a-results.md. The mock does
 * not compute it; it is here because the HAL requires a well-formed crc block
 * on every arm call and the tests must supply a realistic one. A macro rather
 * than a const object, because a static initialiser needs a constant
 * expression.
 */
#define ANT_CRC_CCITT_FALSE                                                    \
	{                                                                      \
		.width_bits = 16u, .poly = 0x1021u, .init = 0xFFFFu,           \
		.xor_out = 0u, .reflect_in = false, .reflect_out = false,      \
		.cover_addr = true,                                            \
	}

/*
 * The two formats Spike A measured, expressed in HAL terms.
 *
 * Tracking: a 5-byte on-air address, so the body is [ttype][0x0A][d0..d7] and
 * its length is carried at body[1]. The length byte counts 8 payload plus 2
 * CRC while the body counts 2 header bytes plus 8 payload, so the two agree
 * and len_bias is 0 - which also holds for an advanced-burst frame at 0x1A.
 *
 * Search: a 3-byte on-air address, so three bytes that the tracking matcher
 * consumed land in the body instead and there is no length field ahead of the
 * body at all. Static 12-byte body, exactly the STATLEN=12 the spike ran.
 */
static const struct ant_pkt_format fmt_track = {
	.phy = ANT_PHY_1M_GFSK,
	.addr_len = 5u,
	.len_mode = ANT_LEN_FROM_BODY,
	.body_len = 0u,
	.len_offset = 1u,
	.len_bias = 0,
	.max_body_len = 26u,
	.crc = ANT_CRC_CCITT_FALSE,
};

static const struct ant_pkt_format fmt_search = {
	.phy = ANT_PHY_1M_GFSK,
	.addr_len = 3u,
	.len_mode = ANT_LEN_FIXED,
	.body_len = 12u,
	.len_offset = 0u,
	.len_bias = 0,
	.max_body_len = 12u,
	.crc = ANT_CRC_CCITT_FALSE,
};

/* The sensor Spike A used for ground truth: #14871 (0x3A17), device type 0x0B,
 * transmission type 5. */
#define TEST_DEVNUM     0x3A17u
#define TEST_DEVNUM_LO  0x17u
#define TEST_DEVNUM_HI  0x3Au
#define TEST_DEVTYPE    0x0Bu
#define TEST_TRANSTYPE  5u

static const uint8_t test_payload[8] = {
	0x10u, 0xBDu, 0xFFu, 0x50u, 0xDEu, 0x11u, 0x64u, 0x00u
};

static uint8_t test_frame[FAKE_RADIO_AIR_FRAME_MAX];
static uint8_t test_frame_len;

static const struct ant_rx_filter filt_track = {
	.addr = { 0xA6u, 0xC5u, TEST_DEVNUM_LO, TEST_DEVNUM_HI, TEST_DEVTYPE },
	.addr_len = 5u,
};

/* Eight concrete search addresses differing only in devnum_lo - one sweep set
 * of the 32 that ant_search.c will produce at max_filters == 8. The sensor's
 * devnum_lo is 0x17, which is entry 7. */
static struct ant_rx_filter sweep[8];

static void build_sweep(void)
{
	uint8_t i;

	for (i = 0u; i < 8u; i++) {
		sweep[i].addr[0] = 0xA6u;
		sweep[i].addr[1] = 0xC5u;
		sweep[i].addr[2] = (uint8_t)(0x10u + i);
		sweep[i].addr[3] = 0u;
		sweep[i].addr[4] = 0u;
		sweep[i].addr_len = 3u;
	}
}

/* ---------------------------------------------------------------------------
 * Recording callbacks
 * ---------------------------------------------------------------------------
 */

#define REC_MAX 24

struct rx_rec {
	uint32_t              op;
	enum ant_radio_status status;
	ant_time_t            t_sync;
	bool                  t_sync_exact;
	uint8_t               filter_index;
	uint8_t               body_len;
	uint8_t               body[ANT_RADIO_BODY_MAX];
	bool                  has_rssi;
	int8_t                rssi_dbm;
	ant_time_t            now_in_cb;
	bool                  in_cb_flag;
};

struct tx_rec {
	uint32_t              op;
	enum ant_radio_status status;
	ant_time_t            t_sync;
	bool                  t_sync_exact;
};

static struct rx_rec rxr[REC_MAX];
static uint32_t      n_rxr;
static struct tx_rec txr[REC_MAX];
static uint32_t      n_txr;

static void rx_cb(const struct ant_rx_event *e, void *user)
{
	struct rx_rec *r;

	ARG_UNUSED(user);
	if (n_rxr >= REC_MAX) {
		return;
	}
	r = &rxr[n_rxr++];
	memset(r, 0, sizeof(*r));
	r->op = e->op;
	r->status = e->status;
	r->t_sync = e->t_sync;
	r->t_sync_exact = e->t_sync_exact;
	r->filter_index = e->filter_index;
	r->body_len = e->body_len;
	if (e->body != NULL && e->body_len > 0u) {
		memcpy(r->body, e->body, e->body_len);
	}
	r->has_rssi = e->has_rssi;
	r->rssi_dbm = e->rssi_dbm;
	r->now_in_cb = ant_radio_now();
	r->in_cb_flag = fake_radio_in_callback();
}

static void tx_cb(const struct ant_tx_event *e, void *user)
{
	struct tx_rec *r;

	ARG_UNUSED(user);
	if (n_txr >= REC_MAX) {
		return;
	}
	r = &txr[n_txr++];
	r->op = e->op;
	r->status = e->status;
	r->t_sync = e->t_sync;
	r->t_sync_exact = e->t_sync_exact;
}

static const struct ant_radio_cbs rec_cbs = { rx_cb, tx_cb };

static void up(void)
{
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_init(&rec_cbs, NULL),
		      "init failed");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_enable(), "enable failed");
}

static void before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_radio_reset();
	n_rxr = 0u;
	n_txr = 0u;
	memset(rxr, 0, sizeof(rxr));
	memset(txr, 0, sizeof(txr));
	build_sweep();
	test_frame_len = fake_radio_build_ant_frame(test_frame, TEST_DEVNUM,
						    TEST_DEVTYPE,
						    TEST_TRANSTYPE,
						    test_payload);
}

static void expect_clean(void)
{
	zassert_true(fake_radio_is_idle(), "radio not idle: %s",
		     fake_radio_busy_reason() ? fake_radio_busy_reason() : "?");
	zassert_equal(0u, fake_radio_viol_count(), "violation: %s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
}

ZTEST_SUITE(fake_radio, NULL, NULL, before, NULL, NULL);

/* ---------------------------------------------------------------------------
 * The virtual clock
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_clock_moves_only_when_told)
{
	ant_time_t t0;
	uint32_t op = 0u;
	struct ant_rx_req req;

	up();
	t0 = ant_radio_now();
	zassert_equal(t0, FAKE_RADIO_T_ORIGIN, "reset did not set the origin");

	/* Neither a caps query nor an arm call is allowed to move it. */
	(void)ant_radio_caps_get();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = t0 + 10000u;
	req.t_close = t0 + 20000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");
	zassert_equal(t0, ant_radio_now(), "arming moved the clock");

	fake_radio_advance(5000u);
	zassert_equal(t0 + 5000u, ant_radio_now(), "advance did not move it");

	(void)ant_radio_abort();
	zassert_equal(t0 + 5000u, ant_radio_now(), "abort moved the clock");
	expect_clean();
}

/*
 * The origin sits one second below 2^32 microseconds so that every suite
 * crosses the 32-bit boundary in its first virtual second. RAIL_Time_t is
 * 32-bit and the HAL makes extending it the backend's duty; this is what
 * catches a core module that quietly stored a timestamp in a uint32_t.
 */
ZTEST(fake_radio, test_clock_crosses_the_32_bit_boundary)
{
	ant_time_t t0 = ant_radio_now();

	zassert_true(t0 < 0x100000000ULL, "origin is not below 2^32");
	fake_radio_advance(2000000u);
	zassert_true(ant_radio_now() > 0xFFFFFFFFULL,
		     "one virtual second did not cross the 32-bit boundary");
	zassert_true(ant_radio_now() - t0 == 2000000ULL,
		     "the delta did not survive the crossing");
}

ZTEST(fake_radio, test_clock_will_not_go_backwards)
{
	ant_time_t t0;

	fake_radio_bring_up();
	fake_radio_advance(1000u);
	t0 = ant_radio_now();
	fake_radio_advance_to(t0 - 1u);
	zassert_equal(t0, ant_radio_now(), "the clock moved backwards");
	zassert_equal(FAKE_RADIO_VIOL_CLOCK_BACKWARDS,
		      fake_radio_viol(0)->code, "no violation recorded");
}

/* ---------------------------------------------------------------------------
 * Operation identifiers
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_op_ids_are_nonzero_unique_and_monotonic)
{
	uint32_t ids[6];
	uint32_t i;

	up();
	for (i = 0u; i < 6u; i++) {
		struct ant_rx_req req;
		ant_time_t now = ant_radio_now();

		memset(&req, 0, sizeof(req));
		req.fmt = &fmt_track;
		req.rf_index = ANT_RF_INDEX_ANT_PLUS;
		req.filters = &filt_track;
		req.n_filters = 1u;
		req.t_open = now + 1000u;
		req.t_close = now + 2000u;
		ids[i] = 0u;
		zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &ids[i]),
			      "arm %u failed", (unsigned int)i);
		zassert_not_equal(0u, ids[i], "op id %u was zero", (unsigned int)i);
		if (i > 0u) {
			zassert_true(ids[i] > ids[i - 1u],
				     "op ids are not monotonic: %u then %u",
				     (unsigned int)ids[i - 1u],
				     (unsigned int)ids[i]);
		}
		fake_radio_advance(3000u);
	}
	expect_clean();
	zassert_equal(6u, fake_radio_stats()->ev_timeout,
		      "expected six window closures");
}

/* ---------------------------------------------------------------------------
 * Capabilities
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_caps_presets_and_readback)
{
	const struct ant_radio_caps *c;

	/* Callable before init, per the HAL. */
	c = ant_radio_caps_get();
	zassert_not_null(c, "caps_get returned NULL");
	zassert_equal(8u, c->max_filters, "nRF preset max_filters");
	zassert_equal(5u, c->addr_len_hw_max, "nRF preset addr_len_hw_max");
	zassert_true(c->crc_in_hw, "nRF preset crc_in_hw");
	zassert_equal(ANT_PHY_1M_GFSK, c->phys[0], "phys[0] must be the ANT PHY");

	fake_radio_caps_preset_rail();
	c = ant_radio_caps_get();
	zassert_equal(2u, c->max_filters, "RAIL preset max_filters");
	zassert_equal(4u, c->addr_len_hw_max, "RAIL preset addr_len_hw_max");
	zassert_false(c->crc_in_hw, "RAIL preset crc_in_hw");
	zassert_true(c->phy_switch_us > 0u,
		     "a RAIL PHY switch reloads a generated configuration");

	/* And the one-liner form, which is the one a scheduler suite uses. */
	fake_radio_caps_mut()->max_filters = 3u;
	zassert_equal(3u, ant_radio_caps_get()->max_filters, "caps not mutable");
}

/*
 * The capability gate itself. ant_sched.c must never hardcode 8 and
 * ant_search.c must never hardcode a 32-set sweep; the only way anyone finds
 * out before an EFR32 exists is a suite that lowers max_filters and watches
 * the arm call refuse.
 */
ZTEST(fake_radio, test_max_filters_is_enforced_by_the_arm_call)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now;

	up();
	fake_radio_caps_preset_rail();
	now = ant_radio_now();

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.t_open = now + 1000u;
	req.t_close = now + 2000u;

	req.n_filters = 8u;
	zassert_equal(ANT_RADIO_EINVAL, ant_radio_rx(&req, &op),
		      "eight filters accepted at max_filters == 2");

	req.n_filters = 2u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op),
		      "two filters refused at max_filters == 2");
	fake_radio_advance(5000u);
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Receiving
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_queued_frame_arrives_at_its_t_sync_with_filter_index)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now;
	ant_time_t t_frame;

	up();
	now = ant_radio_now();
	t_frame = now + 50000u;
	zassert_equal(ANT_RADIO_OK_RC,
		      fake_radio_air_frame(t_frame, test_frame, test_frame_len),
		      "could not queue the frame");

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 8u;
	req.t_open = t_frame - 1000u;
	req.t_close = t_frame + 1000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");

	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(2u, n_rxr, "expected one frame and one window close");
	zassert_equal(ANT_RADIO_STATUS_OK, rxr[0].status, "frame not OK");
	zassert_equal(op, rxr[0].op, "event carried the wrong op id");
	zassert_equal(t_frame, rxr[0].t_sync,
		      "t_sync is not the instant the frame was queued for");
	zassert_equal(t_frame, rxr[0].now_in_cb,
		      "ant_radio_now() inside the callback is not the event's "
		      "own instant");
	zassert_true(rxr[0].in_cb_flag, "callback did not run in callback context");

	/* devnum_lo 0x17 is sweep entry 7: the filter index *is* the identity
	 * of the third on-air address byte, which is how ant_search.c recovers
	 * it. */
	zassert_equal(7u, rxr[0].filter_index, "wrong filter matched");
	zassert_equal(0x10u + 7u, sweep[rxr[0].filter_index].addr[2],
		      "index does not map back to devnum_lo");

	/* Under the 3-byte search format the bytes the tracking matcher would
	 * have swallowed land in the body instead - exactly the 12 bytes Spike
	 * A read out of RAM. */
	zassert_equal(12u, rxr[0].body_len, "search body is not 12 bytes");
	zassert_equal(TEST_DEVNUM_HI, rxr[0].body[0], "body[0] is not devnum_hi");
	zassert_equal(TEST_DEVTYPE, rxr[0].body[1], "body[1] is not device type");
	zassert_equal(TEST_TRANSTYPE, rxr[0].body[2],
		      "body[2] is not transmission type");
	zassert_equal(0x0Au, rxr[0].body[3], "body[3] is not the length byte");
	zassert_mem_equal(&rxr[0].body[4], test_payload, 8u, "payload differs");

	zassert_true(rxr[0].has_rssi, "rssi not reported");
	zassert_equal(FAKE_RADIO_RSSI_BENCH_DBM, rxr[0].rssi_dbm,
		      "default RSSI is not the bench figure");

	zassert_equal(ANT_RADIO_STATUS_TIMEOUT, rxr[1].status,
		      "window did not close with a TIMEOUT terminal");
	expect_clean();
}

/*
 * The same queued frame, heard through the tracking format: five address bytes
 * consumed by the matcher, ten body bytes delivered, and the length carried in
 * the body rather than assumed. One frame on the air, two receiver
 * configurations, both right.
 */
ZTEST(fake_radio, test_same_frame_through_the_tracking_format)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t t_frame = ant_radio_now() + 50000u;

	up();
	zassert_equal(ANT_RADIO_OK_RC,
		      fake_radio_air_frame(t_frame, test_frame, test_frame_len),
		      "queue failed");

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = t_frame - 1000u;
	req.t_close = t_frame + 1000u;
	req.flags = ANT_RX_STOP_ON_FIRST;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");

	fake_radio_advance_to(req.t_close + 1u);

	/* STOP_ON_FIRST: the accepted frame's own event is the terminal one, so
	 * there is no separate TIMEOUT. */
	zassert_equal(1u, n_rxr, "expected exactly one event");
	zassert_equal(ANT_RADIO_STATUS_OK, rxr[0].status, "frame not OK");
	zassert_equal(0u, rxr[0].filter_index, "wrong filter");
	zassert_equal(10u, rxr[0].body_len, "tracking body is not 10 bytes");
	zassert_equal(TEST_TRANSTYPE, rxr[0].body[0], "body[0] is not ttype");
	zassert_equal(0x0Au, rxr[0].body[1], "body[1] is not the length byte");
	zassert_mem_equal(&rxr[0].body[2], test_payload, 8u, "payload differs");
	zassert_equal(0u, fake_radio_stats()->ev_timeout,
		      "STOP_ON_FIRST still produced a TIMEOUT");
	expect_clean();
}

ZTEST(fake_radio, test_window_that_hears_nothing_times_out)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now = ant_radio_now();

	up();
	/* A frame is on the air, but for a device the window is not listening
	 * for. Silence is the default outcome of a mistake, not a special
	 * case. */
	sweep[7].addr[2] = 0x99u;
	zassert_equal(ANT_RADIO_OK_RC,
		      fake_radio_air_frame(now + 5000u, test_frame,
					   test_frame_len),
		      "queue failed");

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 8u;
	req.t_open = now + 1000u;
	req.t_close = now + 9000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");

	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(1u, n_rxr, "expected only the terminal event");
	zassert_equal(ANT_RADIO_STATUS_TIMEOUT, rxr[0].status, "not a TIMEOUT");
	zassert_equal(op, rxr[0].op, "wrong op id on the terminal event");
	zassert_equal(1u, fake_radio_stats()->air_missed,
		      "the unheard frame was not counted as missed");
	expect_clean();
}

ZTEST(fake_radio, test_crc_failure_is_reported_only_when_asked)
{
	struct ant_rx_req req;
	struct fake_radio_air f;
	uint32_t op = 0u;
	ant_time_t now = ant_radio_now();

	up();
	fake_radio_air_init(&f);
	f.t_sync = now + 5000u;
	f.len = test_frame_len;
	memcpy(f.bytes, test_frame, test_frame_len);
	f.crc_bad = true;
	zassert_equal(ANT_RADIO_OK_RC, fake_radio_air_add(&f), "queue failed");

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 8u;
	req.t_open = now + 1000u;
	req.t_close = now + 9000u;
	/* Flag clear: the core is not asking for CRC failures. */
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(1u, n_rxr, "a CRC failure leaked through without the flag");
	zassert_equal(ANT_RADIO_STATUS_TIMEOUT, rxr[0].status, "not a TIMEOUT");
	zassert_equal(1u, fake_radio_stats()->crc_fail_suppressed,
		      "suppressed CRC failure was not counted");

	/* Same frame, same window, flag set. */
	n_rxr = 0u;
	now = ant_radio_now();
	f.t_sync = now + 5000u;
	zassert_equal(ANT_RADIO_OK_RC, fake_radio_air_add(&f), "requeue failed");
	req.t_open = now + 1000u;
	req.t_close = now + 9000u;
	req.flags = ANT_RX_REPORT_CRC_FAIL;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(2u, n_rxr, "expected the CRC failure and the close");
	zassert_equal(ANT_RADIO_STATUS_CRC_FAIL, rxr[0].status, "not CRC_FAIL");
	zassert_equal(7u, rxr[0].filter_index,
		      "a CRC failure must still say which filter matched");
	expect_clean();
}

/*
 * Spike A: a 3-byte search address triggers the address matcher on noise
 * several times a second, at 70 dB below the real transmitter. The note it
 * left for whoever writes ant_search.c was "rank and gate on CRC, never on
 * match count", and this is the mock making that failure reproducible.
 */
ZTEST(fake_radio, test_noise_triggers_do_not_look_like_frames)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now = ant_radio_now();
	uint32_t queued;

	up();
	queued = fake_radio_air_noise(now + 2000u, now + 8000u, 6u,
				      FAKE_RADIO_RSSI_NOISE_DBM);
	zassert_equal(6u, queued, "noise was not queued");

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 8u;
	req.t_open = now + 1000u;
	req.t_close = now + 9000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(1u, n_rxr, "noise reached a core that did not ask for it");
	zassert_equal(6u, fake_radio_stats()->crc_fail_suppressed,
		      "six noise triggers should have been counted");
	zassert_equal(0u, fake_radio_stats()->ev_ok,
		      "noise was delivered as a good frame");
	expect_clean();
}

ZTEST(fake_radio, test_a_stream_of_master_frames_is_all_heard)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t first = ant_radio_now() + 10000u;
	uint32_t queued;
	uint32_t i;

	up();
	/* The measured link: 240 frames in 60 s at -17 dBm with essentially
	 * zero loss. Ten of them is the same behaviour at test speed. */
	queued = fake_radio_air_master(first, FAKE_RADIO_ANT_PERIOD_US, 10u,
				       test_frame, test_frame_len);
	zassert_equal(10u, queued, "master stream not queued");

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = first - 1000u;
	req.t_close = first + 9u * FAKE_RADIO_ANT_PERIOD_US + 1000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(11u, n_rxr, "expected ten frames and one close");
	for (i = 0u; i < 10u; i++) {
		zassert_equal(ANT_RADIO_STATUS_OK, rxr[i].status,
			      "frame %u not OK", (unsigned int)i);
		zassert_equal(first + (ant_time_t)i * FAKE_RADIO_ANT_PERIOD_US,
			      rxr[i].t_sync, "frame %u at the wrong t_sync",
			      (unsigned int)i);
		if (i > 0u) {
			zassert_true(rxr[i].t_sync > rxr[i - 1u].t_sync,
				     "events out of time order at %u",
				     (unsigned int)i);
		}
	}
	zassert_equal(ANT_RADIO_STATUS_TIMEOUT, rxr[10].status, "no close");
	zassert_equal(0u, fake_radio_stats()->air_missed, "loss on a clean link");
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Transmit
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_transmit_reports_the_t_sync_it_achieved)
{
	struct ant_tx_req req;
	uint32_t op = 0u;
	ant_time_t want = ant_radio_now() + 20000u;
	static const uint8_t body[10] = {
		TEST_TRANSTYPE, 0x0Au, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u
	};

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.power.dbm = 4;
	req.body = body;
	req.body_len = sizeof(body);
	req.t_sync_at = want;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_tx(&req, &op), "tx arm failed");

	fake_radio_advance_to(want + 1u);
	zassert_equal(1u, n_txr, "no transmit completion");
	zassert_equal(op, txr[0].op, "wrong op id");
	zassert_equal(ANT_RADIO_STATUS_OK, txr[0].status, "transmit failed");
	zassert_equal(want, txr[0].t_sync, "reported t_sync is not the request");
	zassert_true(txr[0].t_sync_exact, "nRF preset claims an exact t_sync");
	expect_clean();

	/* A backend that cannot schedule exactly must report what it did. */
	n_txr = 0u;
	fake_radio_set_tx_offset_us(17);
	want = ant_radio_now() + 20000u;
	req.t_sync_at = want;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_tx(&req, &op), "tx arm failed");
	fake_radio_advance_to(want + 100u);
	zassert_equal(want + 17u, txr[0].t_sync,
		      "the reported t_sync did not follow the offset");
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Failure modes
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_arming_too_late_is_etime)
{
	struct ant_rx_req rx;
	struct ant_tx_req tx;
	uint32_t op = 0u;
	ant_time_t now;
	uint16_t lead;
	static const uint8_t body[10] = {
		TEST_TRANSTYPE, 0x0Au, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
	};

	up();
	now = ant_radio_now();
	lead = ant_radio_caps_get()->min_arm_lead_us;
	zassert_true(lead > 0u, "a zero arm lead cannot be tested against");

	memset(&rx, 0, sizeof(rx));
	rx.fmt = &fmt_track;
	rx.rf_index = ANT_RF_INDEX_ANT_PLUS;
	rx.filters = &filt_track;
	rx.n_filters = 1u;
	rx.t_open = now + lead - 1u;
	rx.t_close = now + 10000u;
	zassert_equal(ANT_RADIO_ETIME, ant_radio_rx(&rx, &op),
		      "a window inside the arm lead was accepted");

	rx.t_open = now + lead;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&rx, &op),
		      "a window exactly at the arm lead was refused");
	(void)ant_radio_abort();
	n_rxr = 0u;

	memset(&tx, 0, sizeof(tx));
	tx.fmt = &fmt_track;
	tx.rf_index = ANT_RF_INDEX_ANT_PLUS;
	tx.body = body;
	tx.body_len = sizeof(body);
	tx.t_sync_at = ant_radio_now() + lead - 1u;
	zassert_equal(ANT_RADIO_ETIME, ant_radio_tx(&tx, &op),
		      "a late transmit was accepted; a late master frame lands "
		      "in the next slot");
	zassert_equal(2u, fake_radio_stats()->rc_etime, "ETIME not counted");
	expect_clean();
}

ZTEST(fake_radio, test_abort_delivers_the_terminal_event)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now = ant_radio_now();

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 100000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");

	fake_radio_advance(5000u);
	zassert_false(fake_radio_is_idle(), "an armed window reads as idle");

	zassert_equal(ANT_RADIO_OK_RC, ant_radio_abort(), "abort failed");
	zassert_equal(1u, n_rxr, "abort delivered no terminal event");
	zassert_equal(ANT_RADIO_STATUS_ABORTED, rxr[0].status, "not ABORTED");
	zassert_equal(op, rxr[0].op, "wrong op id");
	zassert_true(fake_radio_is_idle(), "slot not free after abort");

	/* Nothing to do the second time, and no second event. */
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_abort(), "second abort");
	zassert_equal(1u, n_rxr, "abort produced a second terminal event");
	expect_clean();
}

ZTEST(fake_radio, test_exactly_one_terminal_event_per_operation)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now = ant_radio_now();
	const struct fake_radio_arm *a;
	uint32_t i;
	uint32_t terminals = 0u;

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 9000u;
	/* Two frames inside the window: two non-terminal events, then one
	 * close. */
	zassert_equal(ANT_RADIO_OK_RC,
		      fake_radio_air_frame(now + 3000u, test_frame,
					   test_frame_len), "queue 1");
	zassert_equal(ANT_RADIO_OK_RC,
		      fake_radio_air_frame(now + 6000u, test_frame,
					   test_frame_len), "queue 2");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");

	fake_radio_advance_to(req.t_close + 5000u);

	zassert_equal(3u, fake_radio_event_count(), "expected three events");
	for (i = 0u; i < fake_radio_event_count(); i++) {
		const struct fake_radio_event *e = fake_radio_event(i);

		zassert_equal(op, e->op, "event %u carried a foreign op id",
			      (unsigned int)i);
		if (e->terminal) {
			terminals++;
		}
	}
	zassert_equal(1u, terminals, "expected exactly one terminal event");

	a = fake_radio_arm_for_op(op);
	zassert_not_null(a, "the arm call was not recorded");
	zassert_true(a->terminated, "the arm record shows no terminal");
	zassert_equal(ANT_RADIO_STATUS_TIMEOUT, a->terminal_status,
		      "wrong terminal status recorded");
	zassert_equal(2u, a->n_rx_events, "wrong non-terminal event count");
	expect_clean();
}

ZTEST(fake_radio, test_forced_failure_and_forced_ebusy)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now = ant_radio_now();

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;

	/* The thread-versus-callback race the HAL says a backend resolves by
	 * returning EBUSY. Nothing else can make it happen on demand. */
	fake_radio_force_next_arm(ANT_RADIO_EBUSY);
	zassert_equal(ANT_RADIO_EBUSY, ant_radio_rx(&req, &op),
		      "forced EBUSY did not take");
	zassert_true(fake_radio_is_idle(), "a refused arm left the slot busy");

	fake_radio_force_next_terminal(ANT_RADIO_STATUS_FAILED);
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");
	fake_radio_advance_to(req.t_close + 1u);
	zassert_equal(1u, n_rxr, "no terminal event");
	zassert_equal(ANT_RADIO_STATUS_FAILED, rxr[0].status,
		      "the forced FAILED terminal did not take");
	expect_clean();
}

ZTEST(fake_radio, test_second_arm_while_busy_is_ebusy)
{
	struct ant_rx_req req;
	uint32_t op1 = 0u;
	uint32_t op2 = 0u;
	ant_time_t now = ant_radio_now();

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op1), "arm 1 failed");
	zassert_equal(ANT_RADIO_EBUSY, ant_radio_rx(&req, &op2),
		      "a second arm was accepted while one was in flight");
	zassert_equal(0u, op2, "op id written on a refused arm");
	fake_radio_advance_to(req.t_close + 1u);
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * The late event from a cancelled operation
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_a_late_event_from_a_cancelled_op_is_detectable)
{
	struct ant_rx_req req;
	struct fake_radio_air f;
	uint32_t dead = 0u;
	uint32_t live = 0u;
	ant_time_t now = ant_radio_now();

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 8u;
	req.t_open = now + 1000u;
	req.t_close = now + 20000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &dead), "arm failed");
	fake_radio_advance(2000u);
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_abort(), "abort failed");

	now = ant_radio_now();
	req.t_open = now + 1000u;
	req.t_close = now + 20000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &live), "re-arm");
	zassert_true(live > dead, "the new op did not get a higher id");

	/* The frame was already in the receiver's pipeline when abort() ran. */
	n_rxr = 0u;
	fake_radio_air_init(&f);
	f.t_sync = ant_radio_now();
	f.len = test_frame_len;
	memcpy(f.bytes, test_frame, test_frame_len);
	zassert_equal(ANT_RADIO_OK_RC,
		      fake_radio_inject_late_rx(dead, &f, 7u), "inject failed");

	zassert_equal(1u, n_rxr, "the late event was not delivered");
	zassert_equal(dead, rxr[0].op, "the late event lost its op id");
	zassert_not_equal(live, rxr[0].op,
			  "a late event indistinguishable from a live one is "
			  "the bug the op id exists to prevent");
	zassert_equal(1u, fake_radio_stats()->ev_late, "late event not counted");

	fake_radio_advance_to(req.t_close + 1u);
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * The callback contract
 * ---------------------------------------------------------------------------
 */

static int isr_rc_enable;
static int isr_rc_init;
static int isr_rc_disable;

static void lifecycle_cb(const struct ant_rx_event *e, void *user)
{
	ARG_UNUSED(e);
	ARG_UNUSED(user);
	isr_rc_enable = ant_radio_enable();
	isr_rc_init = ant_radio_init(&rec_cbs, NULL);
	isr_rc_disable = ant_radio_disable();
}

static const struct ant_radio_cbs lifecycle_cbs = { lifecycle_cb, NULL };

ZTEST(fake_radio, test_lifecycle_calls_from_a_callback_are_refused)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now;
	uint32_t i;
	uint32_t lifecycle_viols = 0u;

	isr_rc_enable = ANT_RADIO_OK_RC;
	isr_rc_init = ANT_RADIO_OK_RC;
	isr_rc_disable = ANT_RADIO_OK_RC;

	zassert_equal(ANT_RADIO_OK_RC, ant_radio_init(&lifecycle_cbs, NULL),
		      "init failed");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_enable(), "enable failed");
	now = ant_radio_now();

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(ANT_RADIO_ESTATE, isr_rc_enable, "enable() was allowed");
	zassert_equal(ANT_RADIO_ESTATE, isr_rc_init, "init() was allowed");
	zassert_equal(ANT_RADIO_ESTATE, isr_rc_disable, "disable() was allowed");

	zassert_equal(3u, fake_radio_viol_count(), "expected three violations");
	for (i = 0u; i < fake_radio_viol_count(); i++) {
		if (fake_radio_viol(i)->code ==
		    FAKE_RADIO_VIOL_ISR_LIFECYCLE) {
			lifecycle_viols++;
		}
	}
	zassert_equal(3u, lifecycle_viols, "wrong violation code recorded");
	/* And the radio is still enabled: a refused disable() did not happen. */
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
}

static ant_time_t harness_now_before;
static ant_time_t harness_now_after;

static void harness_cb(const struct ant_rx_event *e, void *user)
{
	ARG_UNUSED(e);
	ARG_UNUSED(user);
	harness_now_before = ant_radio_now();
	fake_radio_advance(1000000u);
	harness_now_after = ant_radio_now();
}

static const struct ant_radio_cbs harness_cbs = { harness_cb, NULL };

ZTEST(fake_radio, test_the_harness_cannot_be_driven_from_a_callback)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now;

	zassert_equal(ANT_RADIO_OK_RC, ant_radio_init(&harness_cbs, NULL),
		      "init failed");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_enable(), "enable failed");
	now = ant_radio_now();

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(harness_now_before, harness_now_after,
		      "the clock advanced from inside a callback");
	zassert_equal(FAKE_RADIO_VIOL_ISR_HARNESS, fake_radio_viol(0)->code,
		      "no harness violation recorded");
}

static void busy_cb(const struct ant_rx_event *e, void *user)
{
	uint32_t i;

	ARG_UNUSED(e);
	ARG_UNUSED(user);
	/* "Do work proportional to anything - queue it and return." */
	for (i = 0u; i < 40u; i++) {
		(void)ant_radio_now();
	}
}

static const struct ant_radio_cbs busy_cbs = { busy_cb, NULL };

ZTEST(fake_radio, test_a_callback_that_does_work_is_flagged)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now;

	zassert_equal(ANT_RADIO_OK_RC, ant_radio_init(&busy_cbs, NULL),
		      "init failed");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_enable(), "enable failed");
	now = ant_radio_now();

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(1u, fake_radio_viol_count(), "expected one violation");
	zassert_equal(FAKE_RADIO_VIOL_ISR_WORK, fake_radio_viol(0)->code,
		      "wrong violation code");
}

static const uint8_t *retained_body;
static uint8_t        retained_len;

static void retain_cb(const struct ant_rx_event *e, void *user)
{
	ARG_UNUSED(user);
	if (e->status == ANT_RADIO_STATUS_OK) {
		retained_body = e->body;
		retained_len = e->body_len;
	}
}

static const struct ant_radio_cbs retain_cbs = { retain_cb, NULL };

/*
 * The HAL forbids retaining event->body past the callback because on hardware
 * it is a DMA buffer that is reused immediately. Nothing can stop a core
 * module from keeping the pointer, so the mock makes keeping it useless.
 */
ZTEST(fake_radio, test_the_event_body_is_poisoned_after_the_callback)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now;
	uint8_t i;

	retained_body = NULL;
	retained_len = 0u;

	zassert_equal(ANT_RADIO_OK_RC, ant_radio_init(&retain_cbs, NULL),
		      "init failed");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_enable(), "enable failed");
	now = ant_radio_now();

	zassert_equal(ANT_RADIO_OK_RC,
		      fake_radio_air_frame(now + 3000u, test_frame,
					   test_frame_len), "queue failed");
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");
	fake_radio_advance_to(req.t_close + 1u);

	zassert_not_null(retained_body, "no frame was delivered");
	zassert_equal(10u, retained_len, "wrong body length");
	for (i = 0u; i < retained_len; i++) {
		zassert_equal(FAKE_RADIO_BODY_POISON, retained_body[i],
			      "byte %u of a retained body survived the "
			      "callback; on hardware it would not have",
			      (unsigned int)i);
	}
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Chaining from a callback - the pattern the contract exists to permit
 * ---------------------------------------------------------------------------
 */

static struct ant_rx_req chain_req;
static uint32_t          chain_op;
static int               chain_rc;
static uint32_t          chain_count;

static void chain_cb(const struct ant_rx_event *e, void *user)
{
	ARG_UNUSED(user);
	rx_cb(e, NULL);
	if (e->status != ANT_RADIO_STATUS_TIMEOUT || chain_count >= 2u) {
		return;
	}
	chain_count++;
	chain_req.t_open = ant_radio_now() + 1000u;
	chain_req.t_close = ant_radio_now() + 5000u;
	chain_rc = ant_radio_rx(&chain_req, &chain_op);
}

static const struct ant_radio_cbs chain_cbs = { chain_cb, NULL };

ZTEST(fake_radio, test_rearming_from_the_completion_callback_works)
{
	uint32_t first = 0u;
	ant_time_t now;

	chain_count = 0u;
	chain_op = 0u;
	chain_rc = ANT_RADIO_EIO;

	zassert_equal(ANT_RADIO_OK_RC, ant_radio_init(&chain_cbs, NULL),
		      "init failed");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_enable(), "enable failed");
	now = ant_radio_now();

	memset(&chain_req, 0, sizeof(chain_req));
	chain_req.fmt = &fmt_track;
	chain_req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	chain_req.filters = &filt_track;
	chain_req.n_filters = 1u;
	chain_req.t_open = now + 1000u;
	chain_req.t_close = now + 5000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&chain_req, &first),
		      "arm failed");

	fake_radio_advance(100000u);

	zassert_equal(ANT_RADIO_OK_RC, chain_rc,
		      "re-arming from the completion callback was refused; "
		      "that is the low-jitter path the contract permits");
	zassert_equal(2u, chain_count, "the chain did not run twice");
	zassert_equal(3u, n_rxr, "expected three window closures");
	zassert_true(chain_op > first, "the chained op did not get a new id");
	expect_clean();
}

static int nonterminal_arm_rc;

static void midflight_cb(const struct ant_rx_event *e, void *user)
{
	struct ant_rx_req req;
	uint32_t op = 0u;

	ARG_UNUSED(user);
	rx_cb(e, NULL);
	if (e->status != ANT_RADIO_STATUS_OK) {
		return;
	}
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = ant_radio_now() + 1000u;
	req.t_close = ant_radio_now() + 2000u;
	nonterminal_arm_rc = ant_radio_rx(&req, &op);
}

static const struct ant_radio_cbs midflight_cbs = { midflight_cb, NULL };

ZTEST(fake_radio, test_arming_from_a_nonterminal_event_is_ebusy)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now;

	nonterminal_arm_rc = ANT_RADIO_OK_RC;

	zassert_equal(ANT_RADIO_OK_RC, ant_radio_init(&midflight_cbs, NULL),
		      "init failed");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_enable(), "enable failed");
	now = ant_radio_now();

	zassert_equal(ANT_RADIO_OK_RC,
		      fake_radio_air_frame(now + 3000u, test_frame,
					   test_frame_len), "queue failed");
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 9000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(ANT_RADIO_EBUSY, nonterminal_arm_rc,
		      "the window was still running, so the slot was not free");
	expect_clean();
}

static uint32_t abort_cb_calls;

static void abort_cb(const struct ant_rx_event *e, void *user)
{
	ARG_UNUSED(user);
	rx_cb(e, NULL);
	abort_cb_calls++;
	if (e->status == ANT_RADIO_STATUS_OK) {
		(void)ant_radio_abort();
		/* The terminal event must not nest inside the callback that
		 * caused it: the HAL says callbacks never nest. */
		zassert_equal(1u, abort_cb_calls,
			      "the aborted operation's terminal event nested "
			      "inside the callback that caused it");
	}
}

static const struct ant_radio_cbs abort_cbs = { abort_cb, NULL };

ZTEST(fake_radio, test_abort_from_a_callback_defers_its_terminal_event)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now;

	abort_cb_calls = 0u;

	zassert_equal(ANT_RADIO_OK_RC, ant_radio_init(&abort_cbs, NULL),
		      "init failed");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_enable(), "enable failed");
	now = ant_radio_now();

	zassert_equal(ANT_RADIO_OK_RC,
		      fake_radio_air_frame(now + 3000u, test_frame,
					   test_frame_len), "queue failed");
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 100000u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");
	fake_radio_advance_to(now + 5000u);

	zassert_equal(2u, n_rxr, "expected the frame and then the abort");
	zassert_equal(ANT_RADIO_STATUS_OK, rxr[0].status, "no frame");
	zassert_equal(ANT_RADIO_STATUS_ABORTED, rxr[1].status,
		      "the deferred terminal event never arrived");
	zassert_equal(op, rxr[1].op, "wrong op id on the deferred terminal");
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Per-operation configuration
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_an_unset_configuration_is_refused)
{
	struct ant_pkt_format fmt;
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now;

	up();
	now = ant_radio_now();

	memset(&req, 0, sizeof(req));
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;

	/* No format at all. */
	req.fmt = NULL;
	zassert_equal(ANT_RADIO_EINVAL, ant_radio_rx(&req, &op),
		      "a NULL format was accepted");

	/* A zeroed format: the exact failure per-operation configuration
	 * exists to prevent, which on hardware would be a subtly wrong window
	 * rather than a refused one. */
	memset(&fmt, 0, sizeof(fmt));
	req.fmt = &fmt;
	zassert_equal(ANT_RADIO_EINVAL, ant_radio_rx(&req, &op),
		      "a zeroed format was accepted");

	/* A PHY this build does not have. */
	fmt = fmt_track;
	fmt.phy = ANT_PHY_LR_GFSK;
	zassert_equal(ANT_RADIO_ENOTSUP, ant_radio_rx(&req, &op),
		      "an unsupported PHY was approximated rather than refused");

	/* A filter whose address length disagrees with the format's. */
	fmt = fmt_track;
	req.filters = sweep; /* addr_len 3 against a 5-byte format */
	zassert_equal(ANT_RADIO_EINVAL, ant_radio_rx(&req, &op),
		      "a filter of the wrong length was accepted");

	/* An RF index off the band. */
	req.filters = &filt_track;
	req.rf_index = ANT_RF_INDEX_MAX + 1u;
	zassert_equal(ANT_RADIO_EINVAL, ant_radio_rx(&req, &op),
		      "an out-of-range RF index was accepted");

	/* An unknown flag bit. */
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.flags = 0x80000000u;
	zassert_equal(ANT_RADIO_EINVAL, ant_radio_rx(&req, &op),
		      "an unknown flag was ignored rather than refused");

	/* ANT_TIME_NEVER is never a valid arm time, so there is no open-ended
	 * window: background scan re-arms bounded ones. */
	req.flags = 0u;
	req.t_close = ANT_TIME_NEVER;
	zassert_equal(ANT_RADIO_EINVAL, ant_radio_rx(&req, &op),
		      "ANT_TIME_NEVER was accepted as a window edge");

	zassert_true(fake_radio_is_idle(), "a refused arm left something armed");
	zassert_equal(0u, fake_radio_viol_count(), "unexpected violation");
	zassert_equal(7u, fake_radio_stats()->arms_rejected,
		      "not every refusal was counted");
}

ZTEST(fake_radio, test_stop_on_first_is_refused_on_a_merged_window)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now = ant_radio_now();

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 4u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	req.flags = ANT_RX_STOP_ON_FIRST;
	zassert_equal(ANT_RADIO_EINVAL, ant_radio_rx(&req, &op),
		      "STOP_ON_FIRST on a multi-filter window was accepted; it "
		      "would drop every master but the first in the window");

	req.n_filters = 1u;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op),
		      "STOP_ON_FIRST refused on a single-filter window");
	fake_radio_advance_to(req.t_close + 1u);
	expect_clean();
}

ZTEST(fake_radio, test_transmit_body_must_match_the_length_rule)
{
	struct ant_tx_req req;
	uint32_t op = 0u;
	uint8_t body[10] = {
		TEST_TRANSTYPE, 0x0Au, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u
	};

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.body = body;
	req.body_len = sizeof(body);
	req.t_sync_at = ant_radio_now() + 20000u;

	zassert_equal(ANT_RADIO_OK_RC, ant_radio_tx(&req, &op),
		      "a well-formed body was refused");
	(void)ant_radio_abort();

	/* A length byte that does not describe the body. A backend DMAs the
	 * bytes it is given and cannot fix this up, so it has to be refused
	 * here or it goes on the air wrong. */
	body[1] = 0x0Cu;
	req.t_sync_at = ant_radio_now() + 20000u;
	zassert_equal(ANT_RADIO_EINVAL, ant_radio_tx(&req, &op),
		      "a body whose length byte disagrees was accepted");
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Bookkeeping a scheduler test reads
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_every_arm_call_is_recorded_with_its_deadline)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now = ant_radio_now();
	const struct fake_radio_arm *a;

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 8u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	req.flags = ANT_RX_REPORT_CRC_FAIL;
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");

	/* A rejected call is recorded too: a scheduler test wants to assert
	 * that a window was never requested too late, and it can only do that
	 * if the rejects are visible. */
	zassert_equal(ANT_RADIO_EBUSY, ant_radio_rx(&req, &op), "expected EBUSY");

	zassert_equal(2u, fake_radio_arm_count(), "arm calls not recorded");

	a = fake_radio_arm(0u);
	zassert_not_null(a, "record 0 missing");
	zassert_equal(FAKE_RADIO_ARM_RX, a->kind, "wrong kind");
	zassert_equal(ANT_RADIO_OK_RC, a->rc, "wrong rc");
	zassert_equal(now, a->t_arm, "wrong arm time");
	zassert_equal(now + 1000u, a->t_open, "wrong t_open");
	zassert_equal(now + 5000u, a->t_close, "wrong t_close");
	zassert_equal(8u, a->n_filters, "wrong filter count");
	zassert_equal(3u, a->fmt.addr_len, "the format was not captured");
	zassert_equal(ANT_RX_REPORT_CRC_FAIL, a->flags, "flags not captured");
	zassert_equal(0x17u, a->filters[7].addr[2], "filters not captured");
	zassert_false(a->from_callback, "wrongly recorded as an ISR arm");

	a = fake_radio_arm(1u);
	zassert_not_null(a, "record 1 missing");
	zassert_equal(ANT_RADIO_EBUSY, a->rc, "the reject was not recorded");
	zassert_equal(0u, a->op, "a rejected call was given an op id");

	fake_radio_advance_to(req.t_close + 1u);
	expect_clean();
}

ZTEST(fake_radio, test_transmit_power_is_clamped_not_refused)
{
	struct ant_tx_req req;
	uint32_t op = 0u;
	static const uint8_t body[10] = {
		TEST_TRANSTYPE, 0x0Au, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
	};
	const struct fake_radio_arm *a;

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.body = body;
	req.body_len = sizeof(body);
	req.t_sync_at = ant_radio_now() + 20000u;
	req.power.dbm = 100;

	/* The link budget does not care about half a dB, and failing here would
	 * make a shared scheduler brittle. */
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_tx(&req, &op),
		      "an unreachable power was refused rather than clamped");
	a = fake_radio_last_arm();
	zassert_not_null(a, "no record");
	zassert_equal(ant_radio_caps_get()->tx_power_max_dbm, a->power_dbm_eff,
		      "power was not clamped to the caps ceiling");
	(void)ant_radio_abort();
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_arming_outside_the_enabled_state_is_estate)
{
	struct ant_rx_req req;
	uint32_t op = 0u;
	ant_time_t now = ant_radio_now();

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_track;
	req.rf_index = ANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;

	zassert_equal(ANT_RADIO_ESTATE, ant_radio_rx(&req, &op),
		      "armed before init");
	zassert_equal(ANT_RADIO_ESTATE, ant_radio_enable(),
		      "enabled before init");
	zassert_equal(ANT_RADIO_EINVAL, ant_radio_init(NULL, NULL),
		      "init accepted NULL callbacks");

	zassert_equal(ANT_RADIO_OK_RC, ant_radio_init(&rec_cbs, NULL), "init");
	zassert_equal(ANT_RADIO_ESTATE, ant_radio_rx(&req, &op),
		      "armed before enable");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_enable(), "enable");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_enable(),
		      "enable is documented as idempotent");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_rx(&req, &op), "arm failed");

	/* disable() aborts what is in flight and still delivers its terminal
	 * event, and the clock stays monotonic across the pair. */
	now = ant_radio_now();
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_disable(), "disable failed");
	zassert_equal(1u, n_rxr, "disable swallowed the terminal event");
	zassert_equal(ANT_RADIO_STATUS_ABORTED, rxr[0].status, "not ABORTED");
	zassert_equal(now, ant_radio_now(), "disable moved the clock");
	zassert_equal(ANT_RADIO_ESTATE, ant_radio_rx(&req, &op),
		      "armed after disable");
	zassert_equal(ANT_RADIO_OK_RC, ant_radio_enable(), "re-enable failed");
	zassert_true(ant_radio_now() >= now, "the clock went backwards");
	expect_clean();
}
