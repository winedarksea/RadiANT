/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work. Written against
 * radiant/include/radiant/radiant_radio_hal.h, radiant/tests/fake_radio.h and the
 * measurements in docs/spike-a-results.md. Nothing here derives from sdk-ant,
 * from libant.a, or from any ANT+ device profile document.
 *
 * Tests for the mock radio. fake_radio.c is the only backend six of the
 * seven core modules ever run against, so an untested mock is six modules'
 * worth of tests verifying a fiction - this suite is the check on the
 * checker. Covers: the virtual clock moves only when told, starting below
 * the 32-bit microsecond boundary on purpose; op ids are non-zero, unique,
 * monotonic, and a late event with a dead one is distinguishable from a live
 * one; every accepted operation produces exactly one terminal event; the
 * callback contract's MUST NOTs are enforced, not just documented; caps are
 * settable per test and the arm path actually reads them; a queued frame
 * arrives at its own t_sync with the matching filter index, address/body
 * split following the receiver's format; every failure mode the core must
 * survive can be provoked (silent window, CRC failure, late arm, abort).
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

/* CRC-16/CCITT-FALSE covering the on-air address, per docs/ant-radio-link.md
 * and confirmed on 2,164 real frames (docs/spike-a-results.md). The HAL
 * requires a well-formed crc block on every arm call; a macro rather than a
 * const object because a static initialiser needs a constant expression. */
#define RADIANT_CRC_CCITT_FALSE                                                    \
	{                                                                      \
		.width_bits = 16u, .poly = 0x1021u, .init = 0xFFFFu,           \
		.xor_out = 0u, .reflect_in = false, .reflect_out = false,      \
		.cover_addr = true,                                            \
	}

/*
 * Two formats in ANT's tracking and search geometries, expressed in HAL
 * terms.
 *
 * WARNING: fmt_hal_len_from_body is NOT ANT's tracking format - the real one
 * is radiant_frame_format(), and it is RADIANT_LEN_FIXED. This fixture exists
 * only to exercise the mock's RADIANT_LEN_FROM_BODY path (a legitimate HAL
 * mode some other format will need), borrowing ANT's 5-byte address/10-byte
 * body sizes for convenience and declaring a length at body[1] that ANT does
 * not have. Byte 3 of an ANT body is actually a 6-field CONTROL byte
 * (docs/spike-b-results.md); a backend that mapped tracking onto
 * RADIANT_LEN_FROM_BODY (nRF PCNF0.LFLEN=8) would read an acknowledged
 * frame's 0xAA as LENGTH=170, overrun MAXLEN, and silently drop everything
 * but broadcasts - and no length field can parse this byte at all (0x0A's
 * low five bits read 10, 0xA2's read 2, for the same 8-byte payload).
 *
 * fmt_search IS ANT's search format: 3-byte on-air address, static 12-byte
 * body (STATLEN=12, as both spikes ran), no length field at all.
 */
static const struct radiant_pkt_format fmt_hal_len_from_body = {
	.phy = RADIANT_PHY_1M_GFSK,
	.addr_len = 5u,
	.len_mode = RADIANT_LEN_FROM_BODY,
	.body_len = 0u,
	.len_offset = 1u,
	.len_bias = 0,
	.max_body_len = 26u,
	.crc = RADIANT_CRC_CCITT_FALSE,
};

static const struct radiant_pkt_format fmt_search = {
	.phy = RADIANT_PHY_1M_GFSK,
	.addr_len = 3u,
	.len_mode = RADIANT_LEN_FIXED,
	.body_len = 12u,
	.len_offset = 0u,
	.len_bias = 0,
	.max_body_len = 12u,
	.crc = RADIANT_CRC_CCITT_FALSE,
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

static const struct radiant_rx_filter filt_track = {
	.addr = { 0xA6u, 0xC5u, TEST_DEVNUM_LO, TEST_DEVNUM_HI, TEST_DEVTYPE },
	.addr_len = 5u,
};

/* Eight concrete search addresses differing only in devnum_lo - one sweep set
 * of the 32 that radiant_search.c will produce at max_filters == 8. The sensor's
 * devnum_lo is 0x17, which is entry 7. */
static struct radiant_rx_filter sweep[8];

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
	enum radiant_radio_status status;
	radiant_time_t            t_sync;
	bool                  t_sync_exact;
	uint8_t               filter_index;
	uint8_t               body_len;
	uint8_t               body[RADIANT_RADIO_BODY_MAX];
	bool                  has_rssi;
	int8_t                rssi_dbm;
	radiant_time_t            now_in_cb;
	bool                  in_cb_flag;
};

struct tx_rec {
	uint32_t              op;
	enum radiant_radio_status status;
	radiant_time_t            t_sync;
	bool                  t_sync_exact;
};

static struct rx_rec rxr[REC_MAX];
static uint32_t      n_rxr;
static struct tx_rec txr[REC_MAX];
static uint32_t      n_txr;

static void rx_cb(const struct radiant_rx_event *e, void *user)
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
	r->now_in_cb = radiant_radio_now();
	r->in_cb_flag = fake_radio_in_callback();
}

static void tx_cb(const struct radiant_tx_event *e, void *user)
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

static const struct radiant_radio_cbs rec_cbs = { rx_cb, tx_cb };

static void up(void)
{
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&rec_cbs, NULL));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());
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
	zassert_true(fake_radio_is_idle(), "%s",
		     fake_radio_busy_reason() ? fake_radio_busy_reason() : "?");
	zassert_equal(0u, fake_radio_viol_count(), "%s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
}

ZTEST_SUITE(fake_radio, NULL, NULL, before, NULL, NULL);

/* ---------------------------------------------------------------------------
 * The virtual clock
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_clock_moves_only_when_told)
{
	radiant_time_t t0;
	uint32_t op = 0u;
	struct radiant_rx_req req;

	up();
	t0 = radiant_radio_now();
	zassert_equal(t0, FAKE_RADIO_T_ORIGIN);

	/* Neither a caps query nor an arm call is allowed to move it. */
	(void)radiant_radio_caps_get();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = t0 + 10000u;
	req.t_close = t0 + 20000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	zassert_equal(t0, radiant_radio_now());

	fake_radio_advance(5000u);
	zassert_equal(t0 + 5000u, radiant_radio_now());

	(void)radiant_radio_abort();
	zassert_equal(t0 + 5000u, radiant_radio_now());
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
	radiant_time_t t0 = radiant_radio_now();

	zassert_true(t0 < 0x100000000ULL);
	fake_radio_advance(2000000u);
	zassert_true(radiant_radio_now() > 0xFFFFFFFFULL);
	zassert_true(radiant_radio_now() - t0 == 2000000ULL);
}

ZTEST(fake_radio, test_clock_will_not_go_backwards)
{
	radiant_time_t t0;

	fake_radio_bring_up();
	fake_radio_advance(1000u);
	t0 = radiant_radio_now();
	fake_radio_advance_to(t0 - 1u);
	zassert_equal(t0, radiant_radio_now());
	zassert_equal(FAKE_RADIO_VIOL_CLOCK_BACKWARDS,
		      fake_radio_viol(0)->code);
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
		struct radiant_rx_req req;
		radiant_time_t now = radiant_radio_now();

		memset(&req, 0, sizeof(req));
		req.fmt = &fmt_hal_len_from_body;
		req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
		req.filters = &filt_track;
		req.n_filters = 1u;
		req.t_open = now + 1000u;
		req.t_close = now + 2000u;
		ids[i] = 0u;
		zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &ids[i]),
			      "arm %u", (unsigned int)i);
		zassert_not_equal(0u, ids[i], "id %u", (unsigned int)i);
		if (i > 0u) {
			zassert_true(ids[i] > ids[i - 1u],
				     "%u then %u",
				     (unsigned int)ids[i - 1u],
				     (unsigned int)ids[i]);
		}
		fake_radio_advance(3000u);
	}
	expect_clean();
	zassert_equal(6u, fake_radio_stats()->ev_timeout);
}

/* ---------------------------------------------------------------------------
 * Capabilities
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_caps_presets_and_readback)
{
	const struct radiant_radio_caps *c;

	/* Callable before init, per the HAL. */
	c = radiant_radio_caps_get();
	zassert_not_null(c);
	zassert_equal(8u, c->max_filters);
	zassert_equal(5u, c->addr_len_hw_max);
	zassert_true(c->crc_in_hw);
	zassert_equal(RADIANT_PHY_1M_GFSK, c->phys[0]);

	fake_radio_caps_preset_rail();
	c = radiant_radio_caps_get();
	zassert_equal(2u, c->max_filters);
	zassert_equal(4u, c->addr_len_hw_max);
	zassert_false(c->crc_in_hw);
	zassert_true(c->phy_switch_us > 0u);

	/* And the one-liner form, which is the one a scheduler suite uses. */
	fake_radio_caps_mut()->max_filters = 3u;
	zassert_equal(3u, radiant_radio_caps_get()->max_filters);
}

/*
 * The capability gate itself. radiant_sched.c must never hardcode 8 and
 * radiant_search.c must never hardcode a 32-set sweep; the only way anyone finds
 * out before an EFR32 exists is a suite that lowers max_filters and watches
 * the arm call refuse.
 */
ZTEST(fake_radio, test_max_filters_is_enforced_by_the_arm_call)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now;

	up();
	fake_radio_caps_preset_rail();
	now = radiant_radio_now();

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.t_open = now + 1000u;
	req.t_close = now + 2000u;

	/* Beyond the capability, well formed: ENOTSUP (HAL item 5), as every
	 * real backend answers. None at all is malformed. */
	req.n_filters = 8u;
	zassert_equal(RADIANT_RADIO_ENOTSUP, radiant_radio_rx(&req, &op));
	req.n_filters = 0u;
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_rx(&req, &op));

	req.n_filters = 2u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance(5000u);
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Receiving
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_queued_frame_arrives_at_its_t_sync_with_filter_index)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now;
	radiant_time_t t_frame;

	up();
	now = radiant_radio_now();
	t_frame = now + 50000u;
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(t_frame, test_frame, test_frame_len));

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 8u;
	req.t_open = t_frame - 1000u;
	req.t_close = t_frame + 1000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));

	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(2u, n_rxr);
	zassert_equal(RADIANT_RADIO_STATUS_OK, rxr[0].status);
	zassert_equal(op, rxr[0].op);
	zassert_equal(t_frame, rxr[0].t_sync);
	zassert_equal(t_frame, rxr[0].now_in_cb);
	zassert_true(rxr[0].in_cb_flag);

	/* devnum_lo 0x17 is sweep entry 7: the filter index *is* the identity
	 * of the third on-air address byte, which is how radiant_search.c recovers
	 * it. */
	zassert_equal(7u, rxr[0].filter_index);
	zassert_equal(0x10u + 7u, sweep[rxr[0].filter_index].addr[2]);

	/* Under the 3-byte search format the bytes the tracking matcher would
	 * have swallowed land in the body instead - exactly the 12 bytes Spike
	 * A read out of RAM. */
	zassert_equal(12u, rxr[0].body_len);
	zassert_equal(TEST_DEVNUM_HI, rxr[0].body[0]);
	zassert_equal(TEST_DEVTYPE, rxr[0].body[1]);
	zassert_equal(TEST_TRANSTYPE, rxr[0].body[2]);
	zassert_equal(0x0Au, rxr[0].body[3]);
	zassert_mem_equal(&rxr[0].body[4], test_payload, 8u);

	zassert_true(rxr[0].has_rssi);
	zassert_equal(FAKE_RADIO_RSSI_BENCH_DBM, rxr[0].rssi_dbm);

	zassert_equal(RADIANT_RADIO_STATUS_TIMEOUT, rxr[1].status);
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
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t t_frame = radiant_radio_now() + 50000u;

	up();
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(t_frame, test_frame, test_frame_len));

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = t_frame - 1000u;
	req.t_close = t_frame + 1000u;
	req.flags = RADIANT_RX_STOP_ON_FIRST;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));

	fake_radio_advance_to(req.t_close + 1u);

	/* STOP_ON_FIRST: the accepted frame's own event is the terminal one, so
	 * there is no separate TIMEOUT. */
	zassert_equal(1u, n_rxr);
	zassert_equal(RADIANT_RADIO_STATUS_OK, rxr[0].status);
	zassert_equal(0u, rxr[0].filter_index);
	zassert_equal(10u, rxr[0].body_len);
	zassert_equal(TEST_TRANSTYPE, rxr[0].body[0]);
	zassert_equal(0x0Au, rxr[0].body[1]);
	zassert_mem_equal(&rxr[0].body[2], test_payload, 8u);
	zassert_equal(0u, fake_radio_stats()->ev_timeout);
	expect_clean();
}

ZTEST(fake_radio, test_window_that_hears_nothing_times_out)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now = radiant_radio_now();

	up();
	/* A frame is on the air, but for a device the window is not listening
	 * for. Silence is the default outcome of a mistake, not a special
	 * case. */
	sweep[7].addr[2] = 0x99u;
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(now + 5000u, test_frame,
					   test_frame_len));

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 8u;
	req.t_open = now + 1000u;
	req.t_close = now + 9000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));

	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(1u, n_rxr);
	zassert_equal(RADIANT_RADIO_STATUS_TIMEOUT, rxr[0].status);
	zassert_equal(op, rxr[0].op);
	zassert_equal(1u, fake_radio_stats()->air_missed);
	expect_clean();
}

ZTEST(fake_radio, test_crc_failure_is_reported_only_when_asked)
{
	struct radiant_rx_req req;
	struct fake_radio_air f;
	uint32_t op = 0u;
	radiant_time_t now = radiant_radio_now();

	up();
	fake_radio_air_init(&f);
	f.t_sync = now + 5000u;
	f.len = test_frame_len;
	memcpy(f.bytes, test_frame, test_frame_len);
	f.crc_bad = true;
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_air_add(&f));

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 8u;
	req.t_open = now + 1000u;
	req.t_close = now + 9000u;
	/* Flag clear: the core is not asking for CRC failures. */
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(1u, n_rxr);
	zassert_equal(RADIANT_RADIO_STATUS_TIMEOUT, rxr[0].status);
	zassert_equal(1u, fake_radio_stats()->crc_fail_suppressed);

	/* Same frame, same window, flag set. */
	n_rxr = 0u;
	now = radiant_radio_now();
	f.t_sync = now + 5000u;
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_air_add(&f));
	req.t_open = now + 1000u;
	req.t_close = now + 9000u;
	req.flags = RADIANT_RX_REPORT_CRC_FAIL;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(2u, n_rxr);
	zassert_equal(RADIANT_RADIO_STATUS_CRC_FAIL, rxr[0].status);
	zassert_equal(7u, rxr[0].filter_index);
	expect_clean();
}

/*
 * Spike A: a 3-byte search address triggers the address matcher on noise
 * several times a second, at 70 dB below the real transmitter. The note it
 * left for whoever writes radiant_search.c was "rank and gate on CRC, never on
 * match count", and this is the mock making that failure reproducible.
 */
ZTEST(fake_radio, test_noise_triggers_do_not_look_like_frames)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now = radiant_radio_now();
	uint32_t queued;

	up();
	queued = fake_radio_air_noise(now + 2000u, now + 8000u, 6u,
				      FAKE_RADIO_RSSI_NOISE_DBM);
	zassert_equal(6u, queued);

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 8u;
	req.t_open = now + 1000u;
	req.t_close = now + 9000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(1u, n_rxr);
	zassert_equal(6u, fake_radio_stats()->crc_fail_suppressed);
	zassert_equal(0u, fake_radio_stats()->ev_ok);
	expect_clean();
}

ZTEST(fake_radio, test_a_stream_of_master_frames_is_all_heard)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t first = radiant_radio_now() + 10000u;
	uint32_t queued;
	uint32_t i;

	up();
	/* The measured link: 240 frames in 60 s at -17 dBm with essentially
	 * zero loss. Ten of them is the same behaviour at test speed. */
	queued = fake_radio_air_master(first, FAKE_RADIO_ANT_PERIOD_US, 10u,
				       test_frame, test_frame_len);
	zassert_equal(10u, queued);

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = first - 1000u;
	req.t_close = first + 9u * FAKE_RADIO_ANT_PERIOD_US + 1000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(11u, n_rxr);
	for (i = 0u; i < 10u; i++) {
		zassert_equal(RADIANT_RADIO_STATUS_OK, rxr[i].status, "frame %u", (unsigned int)i);
		zassert_equal(first + (radiant_time_t)i * FAKE_RADIO_ANT_PERIOD_US,
			      rxr[i].t_sync, "frame %u",
			      (unsigned int)i);
		if (i > 0u) {
			zassert_true(rxr[i].t_sync > rxr[i - 1u].t_sync, "at %u", (unsigned int)i);
		}
	}
	zassert_equal(RADIANT_RADIO_STATUS_TIMEOUT, rxr[10].status);
	zassert_equal(0u, fake_radio_stats()->air_missed);
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Transmit
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_transmit_reports_the_t_sync_it_achieved)
{
	struct radiant_tx_req req;
	uint32_t op = 0u;
	radiant_time_t want = radiant_radio_now() + 20000u;
	static const uint8_t body[10] = {
		TEST_TRANSTYPE, 0x0Au, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u
	};

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.power.dbm = 4;
	memcpy(req.addr, filt_track.addr, sizeof(req.addr));
	req.addr_len = filt_track.addr_len;
	req.body = body;
	req.body_len = sizeof(body);
	req.t_sync_at = want;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_tx(&req, &op));

	fake_radio_advance_to(want + 1u);
	zassert_equal(1u, n_txr);
	zassert_equal(op, txr[0].op);
	zassert_equal(RADIANT_RADIO_STATUS_OK, txr[0].status);
	zassert_equal(want, txr[0].t_sync);
	zassert_true(txr[0].t_sync_exact);
	expect_clean();

	/* A backend that cannot schedule exactly must report what it did. */
	n_txr = 0u;
	fake_radio_set_tx_offset_us(17);
	want = radiant_radio_now() + 20000u;
	req.t_sync_at = want;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_tx(&req, &op));
	fake_radio_advance_to(want + 100u);
	zassert_equal(want + 17u, txr[0].t_sync);
	expect_clean();
}

/* HAL item 5: t_sync is meaningful only on OK. A transmit that did not go
 * out reports the t_sync it was armed for, not exact - never 0 or a clock
 * reading, which a reader that skipped the status check would take as a
 * real anchor. */
ZTEST(fake_radio, test_a_transmit_that_did_not_go_out_reports_its_requested_t_sync)
{
	static const enum radiant_radio_status forced[] = {
		RADIANT_RADIO_STATUS_FAILED, RADIANT_RADIO_STATUS_DENIED,
	};
	struct radiant_tx_req req;
	uint32_t op = 0u;
	radiant_time_t want;
	static const uint8_t body[10] = {
		TEST_TRANSTYPE, 0x0Au, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u
	};

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.power.dbm = 4;
	memcpy(req.addr, filt_track.addr, sizeof(req.addr));
	req.addr_len = filt_track.addr_len;
	req.body = body;
	req.body_len = sizeof(body);

	for (size_t i = 0u; i < ARRAY_SIZE(forced); i++) {
		n_txr = 0u;
		want = radiant_radio_now() + 20000u;
		req.t_sync_at = want;
		fake_radio_force_next_terminal(forced[i]);
		zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_tx(&req, &op));
		fake_radio_advance_to(want + 100u);
		zassert_equal(1u, n_txr);
		zassert_equal(forced[i], txr[0].status);
		zassert_equal(want, txr[0].t_sync, "status %d", (int)forced[i]);
		zassert_false(txr[0].t_sync_exact);
	}

	/* Aborted from thread context, long before its t_sync. */
	n_txr = 0u;
	want = radiant_radio_now() + 20000u;
	req.t_sync_at = want;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_tx(&req, &op));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_abort());
	zassert_equal(1u, n_txr);
	zassert_equal(RADIANT_RADIO_STATUS_ABORTED, txr[0].status);
	zassert_equal(want, txr[0].t_sync);
	zassert_false(txr[0].t_sync_exact);
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Failure modes
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_arming_too_late_is_etime)
{
	struct radiant_rx_req rx;
	struct radiant_tx_req tx;
	uint32_t op = 0u;
	radiant_time_t now;
	uint16_t lead;
	static const uint8_t body[10] = {
		TEST_TRANSTYPE, 0x0Au, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
	};

	up();
	now = radiant_radio_now();
	lead = radiant_radio_caps_get()->min_arm_lead_us;
	zassert_true(lead > 0u);

	memset(&rx, 0, sizeof(rx));
	rx.fmt = &fmt_hal_len_from_body;
	rx.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	rx.filters = &filt_track;
	rx.n_filters = 1u;
	rx.t_open = now + lead - 1u;
	rx.t_close = now + 10000u;
	zassert_equal(RADIANT_RADIO_ETIME, radiant_radio_rx(&rx, &op));

	rx.t_open = now + lead;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&rx, &op));
	(void)radiant_radio_abort();
	n_rxr = 0u;

	memset(&tx, 0, sizeof(tx));
	tx.fmt = &fmt_hal_len_from_body;
	tx.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	memcpy(tx.addr, filt_track.addr, sizeof(tx.addr));
	tx.addr_len = filt_track.addr_len;
	tx.body = body;
	tx.body_len = sizeof(body);
	tx.t_sync_at = radiant_radio_now() + lead - 1u;
	zassert_equal(RADIANT_RADIO_ETIME, radiant_radio_tx(&tx, &op));
	zassert_equal(2u, fake_radio_stats()->rc_etime);
	expect_clean();
}

ZTEST(fake_radio, test_abort_delivers_the_terminal_event)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now = radiant_radio_now();

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 100000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));

	fake_radio_advance(5000u);
	zassert_false(fake_radio_is_idle());

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_abort());
	zassert_equal(1u, n_rxr);
	zassert_equal(RADIANT_RADIO_STATUS_ABORTED, rxr[0].status);
	zassert_equal(op, rxr[0].op);
	zassert_true(fake_radio_is_idle());

	/* Nothing to do the second time, and no second event. */
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_abort());
	zassert_equal(1u, n_rxr);
	expect_clean();
}

ZTEST(fake_radio, test_exactly_one_terminal_event_per_operation)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now = radiant_radio_now();
	const struct fake_radio_arm *a;
	uint32_t i;
	uint32_t terminals = 0u;

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 9000u;
	/* Two frames inside the window: two non-terminal events, then one
	 * close. */
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(now + 3000u, test_frame,
					   test_frame_len));
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(now + 6000u, test_frame,
					   test_frame_len));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));

	fake_radio_advance_to(req.t_close + 5000u);

	zassert_equal(3u, fake_radio_event_count());
	for (i = 0u; i < fake_radio_event_count(); i++) {
		const struct fake_radio_event *e = fake_radio_event(i);

		zassert_equal(op, e->op, "event %u", (unsigned int)i);
		if (e->terminal) {
			terminals++;
		}
	}
	zassert_equal(1u, terminals);

	a = fake_radio_arm_for_op(op);
	zassert_not_null(a);
	zassert_true(a->terminated);
	zassert_equal(RADIANT_RADIO_STATUS_TIMEOUT, a->terminal_status);
	zassert_equal(2u, a->n_rx_events);
	expect_clean();
}

ZTEST(fake_radio, test_forced_failure_and_forced_ebusy)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now = radiant_radio_now();

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;

	/* The thread-versus-callback race the HAL says a backend resolves by
	 * returning EBUSY. Nothing else can make it happen on demand. */
	fake_radio_force_next_arm(RADIANT_RADIO_EBUSY);
	zassert_equal(RADIANT_RADIO_EBUSY, radiant_radio_rx(&req, &op));
	zassert_true(fake_radio_is_idle());

	fake_radio_force_next_terminal(RADIANT_RADIO_STATUS_FAILED);
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);
	zassert_equal(1u, n_rxr);
	zassert_equal(RADIANT_RADIO_STATUS_FAILED, rxr[0].status);
	expect_clean();
}

ZTEST(fake_radio, test_second_arm_while_busy_is_ebusy)
{
	struct radiant_rx_req req;
	uint32_t op1 = 0u;
	uint32_t op2 = 0u;
	radiant_time_t now = radiant_radio_now();

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op1));
	zassert_equal(RADIANT_RADIO_EBUSY, radiant_radio_rx(&req, &op2));
	zassert_equal(0u, op2);
	fake_radio_advance_to(req.t_close + 1u);
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * The late event from a cancelled operation
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_a_late_event_from_a_cancelled_op_is_detectable)
{
	struct radiant_rx_req req;
	struct fake_radio_air f;
	uint32_t dead = 0u;
	uint32_t live = 0u;
	radiant_time_t now = radiant_radio_now();

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 8u;
	req.t_open = now + 1000u;
	req.t_close = now + 20000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &dead));
	fake_radio_advance(2000u);
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_abort());

	now = radiant_radio_now();
	req.t_open = now + 1000u;
	req.t_close = now + 20000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &live));
	zassert_true(live > dead);

	/* The frame was already in the receiver's pipeline when abort() ran. */
	n_rxr = 0u;
	fake_radio_air_init(&f);
	f.t_sync = radiant_radio_now();
	f.len = test_frame_len;
	memcpy(f.bytes, test_frame, test_frame_len);
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_inject_late_rx(dead, &f, 7u));

	zassert_equal(1u, n_rxr);
	zassert_equal(dead, rxr[0].op);
	zassert_not_equal(live, rxr[0].op);
	zassert_equal(1u, fake_radio_stats()->ev_late);

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

static void lifecycle_cb(const struct radiant_rx_event *e, void *user)
{
	ARG_UNUSED(e);
	ARG_UNUSED(user);
	isr_rc_enable = radiant_radio_enable();
	isr_rc_init = radiant_radio_init(&rec_cbs, NULL);
	isr_rc_disable = radiant_radio_disable();
}

static const struct radiant_radio_cbs lifecycle_cbs = { lifecycle_cb, NULL };

ZTEST(fake_radio, test_lifecycle_calls_from_a_callback_are_refused)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now;
	uint32_t i;
	uint32_t lifecycle_viols = 0u;

	isr_rc_enable = RADIANT_RADIO_OK_RC;
	isr_rc_init = RADIANT_RADIO_OK_RC;
	isr_rc_disable = RADIANT_RADIO_OK_RC;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&lifecycle_cbs, NULL));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());
	now = radiant_radio_now();

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(RADIANT_RADIO_ESTATE, isr_rc_enable);
	zassert_equal(RADIANT_RADIO_ESTATE, isr_rc_init);
	zassert_equal(RADIANT_RADIO_ESTATE, isr_rc_disable);

	zassert_equal(3u, fake_radio_viol_count());
	for (i = 0u; i < fake_radio_viol_count(); i++) {
		if (fake_radio_viol(i)->code ==
		    FAKE_RADIO_VIOL_ISR_LIFECYCLE) {
			lifecycle_viols++;
		}
	}
	zassert_equal(3u, lifecycle_viols);
	/* And the radio is still enabled: a refused disable() did not happen. */
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
}

static radiant_time_t harness_now_before;
static radiant_time_t harness_now_after;

static void harness_cb(const struct radiant_rx_event *e, void *user)
{
	ARG_UNUSED(e);
	ARG_UNUSED(user);
	harness_now_before = radiant_radio_now();
	fake_radio_advance(1000000u);
	harness_now_after = radiant_radio_now();
}

static const struct radiant_radio_cbs harness_cbs = { harness_cb, NULL };

ZTEST(fake_radio, test_the_harness_cannot_be_driven_from_a_callback)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&harness_cbs, NULL));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());
	now = radiant_radio_now();

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(harness_now_before, harness_now_after);
	zassert_equal(FAKE_RADIO_VIOL_ISR_HARNESS, fake_radio_viol(0)->code);
}

static void busy_cb(const struct radiant_rx_event *e, void *user)
{
	uint32_t i;

	ARG_UNUSED(e);
	ARG_UNUSED(user);
	/* "Do work proportional to anything - queue it and return." */
	for (i = 0u; i < 40u; i++) {
		(void)radiant_radio_now();
	}
}

static const struct radiant_radio_cbs busy_cbs = { busy_cb, NULL };

ZTEST(fake_radio, test_a_callback_that_does_work_is_flagged)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&busy_cbs, NULL));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());
	now = radiant_radio_now();

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(1u, fake_radio_viol_count());
	zassert_equal(FAKE_RADIO_VIOL_ISR_WORK, fake_radio_viol(0)->code);
}

static const uint8_t *retained_body;
static uint8_t        retained_len;

static void retain_cb(const struct radiant_rx_event *e, void *user)
{
	ARG_UNUSED(user);
	if (e->status == RADIANT_RADIO_STATUS_OK) {
		retained_body = e->body;
		retained_len = e->body_len;
	}
}

static const struct radiant_radio_cbs retain_cbs = { retain_cb, NULL };

/*
 * The HAL forbids retaining event->body past the callback because on hardware
 * it is a DMA buffer that is reused immediately. Nothing can stop a core
 * module from keeping the pointer, so the mock makes keeping it useless.
 */
ZTEST(fake_radio, test_the_event_body_is_poisoned_after_the_callback)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now;
	uint8_t i;

	retained_body = NULL;
	retained_len = 0u;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&retain_cbs, NULL));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());
	now = radiant_radio_now();

	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(now + 3000u, test_frame,
					   test_frame_len));
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);

	zassert_not_null(retained_body);
	zassert_equal(10u, retained_len);
	for (i = 0u; i < retained_len; i++) {
		zassert_equal(FAKE_RADIO_BODY_POISON, retained_body[i], "byte %u", (unsigned int)i);
	}
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Chaining from a callback - the pattern the contract exists to permit
 * ---------------------------------------------------------------------------
 */

static struct radiant_rx_req chain_req;
static uint32_t          chain_op;
static int               chain_rc;
static uint32_t          chain_count;

static void chain_cb(const struct radiant_rx_event *e, void *user)
{
	ARG_UNUSED(user);
	rx_cb(e, NULL);
	if (e->status != RADIANT_RADIO_STATUS_TIMEOUT || chain_count >= 2u) {
		return;
	}
	chain_count++;
	chain_req.t_open = radiant_radio_now() + 1000u;
	chain_req.t_close = radiant_radio_now() + 5000u;
	chain_rc = radiant_radio_rx(&chain_req, &chain_op);
}

static const struct radiant_radio_cbs chain_cbs = { chain_cb, NULL };

ZTEST(fake_radio, test_rearming_from_the_completion_callback_works)
{
	uint32_t first = 0u;
	radiant_time_t now;

	chain_count = 0u;
	chain_op = 0u;
	chain_rc = RADIANT_RADIO_EIO;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&chain_cbs, NULL));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());
	now = radiant_radio_now();

	memset(&chain_req, 0, sizeof(chain_req));
	chain_req.fmt = &fmt_hal_len_from_body;
	chain_req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	chain_req.filters = &filt_track;
	chain_req.n_filters = 1u;
	chain_req.t_open = now + 1000u;
	chain_req.t_close = now + 5000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&chain_req, &first));

	fake_radio_advance(100000u);

	zassert_equal(RADIANT_RADIO_OK_RC, chain_rc);
	zassert_equal(2u, chain_count);
	zassert_equal(3u, n_rxr);
	zassert_true(chain_op > first);
	expect_clean();
}

static int nonterminal_arm_rc;

static void midflight_cb(const struct radiant_rx_event *e, void *user)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;

	ARG_UNUSED(user);
	rx_cb(e, NULL);
	if (e->status != RADIANT_RADIO_STATUS_OK) {
		return;
	}
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = radiant_radio_now() + 1000u;
	req.t_close = radiant_radio_now() + 2000u;
	nonterminal_arm_rc = radiant_radio_rx(&req, &op);
}

static const struct radiant_radio_cbs midflight_cbs = { midflight_cb, NULL };

ZTEST(fake_radio, test_arming_from_a_nonterminal_event_is_ebusy)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now;

	nonterminal_arm_rc = RADIANT_RADIO_OK_RC;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&midflight_cbs, NULL));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());
	now = radiant_radio_now();

	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(now + 3000u, test_frame,
					   test_frame_len));
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 9000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);

	zassert_equal(RADIANT_RADIO_EBUSY, nonterminal_arm_rc);
	expect_clean();
}

static uint32_t abort_cb_calls;
static uint32_t abort_n_at_return;
static bool     abort_body_intact;
static struct radiant_rx_req abort_rearm_req;
static int      abort_rearm_rc;
static uint32_t abort_rearm_op;

static void abort_cb(const struct radiant_rx_event *e, void *user)
{
	ARG_UNUSED(user);
	rx_cb(e, NULL);
	abort_cb_calls++;
	if (e->status == RADIANT_RADIO_STATUS_OK && abort_cb_calls == 1u) {
		uint8_t copy[RADIANT_RADIO_BODY_MAX];
		uint8_t len = e->body_len;

		memcpy(copy, e->body, len);
		(void)radiant_radio_abort();
		/* The one exception to "callbacks never nest" (HAL item 5): the
		 * aborted operation's terminal is delivered nested, before
		 * radiant_radio_abort() returns ... */
		abort_n_at_return = n_rxr;
		/* ... the outer event, body included, is still valid ... */
		abort_body_intact = (memcmp(copy, e->body, len) == 0);
		/* ... and the slot is free for a re-arm from the same callback. */
		abort_rearm_rc = radiant_radio_rx(&abort_rearm_req, &abort_rearm_op);
	}
}

static const struct radiant_radio_cbs abort_cbs = { abort_cb, NULL };

ZTEST(fake_radio, test_abort_from_a_callback_delivers_its_terminal_nested)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now;

	abort_cb_calls = 0u;
	abort_n_at_return = 0u;
	abort_body_intact = false;
	abort_rearm_rc = RADIANT_RADIO_EINVAL;
	abort_rearm_op = 0u;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&abort_cbs, NULL));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());
	now = radiant_radio_now();

	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(now + 3000u, test_frame,
					   test_frame_len));
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 100000u;
	abort_rearm_req = req;
	abort_rearm_req.t_open = now + 10000u;
	abort_rearm_req.t_close = now + 20000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(now + 5000u);

	zassert_equal(2u, n_rxr);
	zassert_equal(RADIANT_RADIO_STATUS_OK, rxr[0].status);
	zassert_equal(RADIANT_RADIO_STATUS_ABORTED, rxr[1].status);
	zassert_equal(op, rxr[1].op);
	zassert_true(rxr[1].in_cb_flag);
	zassert_equal(2u, abort_n_at_return,
		      "the ABORTED terminal was not delivered before abort returned");
	zassert_true(abort_body_intact, "the nested terminal clobbered the outer body");
	zassert_equal(RADIANT_RADIO_OK_RC, abort_rearm_rc,
		      "a re-arm after the abort was refused");
	zassert_not_equal(op, abort_rearm_op);

	/* Exactly one terminal for the aborted window; the re-armed one runs. */
	fake_radio_advance_to(now + 30000u);
	zassert_equal(3u, n_rxr);
	zassert_equal(abort_rearm_op, rxr[2].op);
	zassert_equal(RADIANT_RADIO_STATUS_TIMEOUT, rxr[2].status);
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Per-operation configuration
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_an_unset_configuration_is_refused)
{
	struct radiant_pkt_format fmt;
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now;

	up();
	now = radiant_radio_now();

	memset(&req, 0, sizeof(req));
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;

	/* No format at all. */
	req.fmt = NULL;
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_rx(&req, &op));

	/* A zeroed format: the exact failure per-operation configuration
	 * exists to prevent, which on hardware would be a subtly wrong window
	 * rather than a refused one. */
	memset(&fmt, 0, sizeof(fmt));
	req.fmt = &fmt;
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_rx(&req, &op));

	/* A PHY this build does not have. Still ENOTSUP now that the coded PHY
	 * is a real one: the default presets advertise 1 M alone, and a backend
	 * must refuse a PHY it was not built with rather than approximate it. */
	fmt = fmt_hal_len_from_body;
	fmt.phy = RADIANT_PHY_LR_CODED;
	zassert_equal(RADIANT_RADIO_ENOTSUP, radiant_radio_rx(&req, &op));

	/* A filter whose address length disagrees with the format's. */
	fmt = fmt_hal_len_from_body;
	req.filters = sweep; /* addr_len 3 against a 5-byte format */
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_rx(&req, &op));

	/* An RF index off the band. */
	req.filters = &filt_track;
	req.rf_index = RADIANT_RF_INDEX_MAX + 1u;
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_rx(&req, &op));

	/* An unknown flag bit. */
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.flags = 0x80000000u;
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_rx(&req, &op));

	/* RADIANT_TIME_NEVER is never a valid arm time, so there is no open-ended
	 * window: background scan re-arms bounded ones. */
	req.flags = 0u;
	req.t_close = RADIANT_TIME_NEVER;
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_rx(&req, &op));

	zassert_true(fake_radio_is_idle());
	zassert_equal(0u, fake_radio_viol_count());
	zassert_equal(7u, fake_radio_stats()->arms_rejected);
}

ZTEST(fake_radio, test_stop_on_first_is_refused_on_a_merged_window)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now = radiant_radio_now();

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 4u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	req.flags = RADIANT_RX_STOP_ON_FIRST;
	/* It would drop every master but the first in the window. */
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_rx(&req, &op));

	req.n_filters = 1u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);
	expect_clean();
}

ZTEST(fake_radio, test_transmit_body_must_match_the_length_rule)
{
	struct radiant_tx_req req;
	uint32_t op = 0u;
	uint8_t body[10] = {
		TEST_TRANSTYPE, 0x0Au, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u
	};

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	memcpy(req.addr, filt_track.addr, sizeof(req.addr));
	req.addr_len = filt_track.addr_len;
	req.body = body;
	req.body_len = sizeof(body);
	req.t_sync_at = radiant_radio_now() + 20000u;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_tx(&req, &op));
	(void)radiant_radio_abort();

	/* A declared length that does not describe the body. A backend DMAs
	 * the bytes it is given and cannot fix this up, so it has to be
	 * refused here or it goes on the air wrong.
	 *
	 * NOTE this is the RADIANT_LEN_FROM_BODY fixture, not ANT's tracking
	 * format. ANT frames have no length byte; byte 3 is a control byte and
	 * a 0x0C there is a bits-2:0 violation, not a length mismatch. See
	 * radiant/tests/src/test_frame.c. */
	body[1] = 0x0Cu;
	req.t_sync_at = radiant_radio_now() + 20000u;
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_tx(&req, &op));
	expect_clean();
}

/*
 * A transmit must name the address it emits, and the mock must say so. This
 * was missed for a long time because a mock that only echoes a request
 * cannot notice a field the contract never mentioned - struct radiant_tx_req
 * had no address field at all, and every suite above the HAL asserted on
 * body bytes instead.
 *
 * On real hardware the omission is not a refusal: nRF's TXADDRESS indexes
 * BASE/PREFIX registers the previous operation loaded, so a transmit with no
 * address of its own inherits the last rx window's device number and emits
 * a well-formed frame to the wrong sensor. The mock is now strict where the
 * hardware is silent.
 */
ZTEST(fake_radio, test_a_transmit_must_carry_its_own_on_air_address)
{
	struct radiant_tx_req req;
	uint32_t op = 0u;
	static const uint8_t body[10] = {
		TEST_TRANSTYPE, 0x0Au, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 8u
	};

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.body = body;
	req.body_len = sizeof(body);
	req.t_sync_at = radiant_radio_now() + 20000u;

	/* addr_len still zero: the request as it could have been written before
	 * the field existed. */
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_tx(&req, &op));

	/* An address of the wrong length for the format - search's three bytes
	 * against a five-byte format - is equally unusable. */
	memcpy(req.addr, filt_track.addr, sizeof(req.addr));
	req.addr_len = 3u;
	req.t_sync_at = radiant_radio_now() + 20000u;
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_tx(&req, &op));

	/* And the well-formed one is recorded byte for byte, so a scheduler test
	 * can assert on which device a frame was addressed to. */
	req.addr_len = filt_track.addr_len;
	req.t_sync_at = radiant_radio_now() + 20000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_tx(&req, &op));
	zassert_equal(5u, fake_radio_arm(fake_radio_arm_count() - 1u)->addr_len);
	zassert_mem_equal(fake_radio_arm(fake_radio_arm_count() - 1u)->addr, filt_track.addr, 5u);

	(void)radiant_radio_abort();
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Bookkeeping a scheduler test reads
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_every_arm_call_is_recorded_with_its_deadline)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now = radiant_radio_now();
	const struct fake_radio_arm *a;

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_search;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = sweep;
	req.n_filters = 8u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	req.flags = RADIANT_RX_REPORT_CRC_FAIL;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));

	/* A rejected call is recorded too: a scheduler test wants to assert
	 * that a window was never requested too late, and it can only do that
	 * if the rejects are visible. */
	zassert_equal(RADIANT_RADIO_EBUSY, radiant_radio_rx(&req, &op));

	zassert_equal(2u, fake_radio_arm_count());

	a = fake_radio_arm(0u);
	zassert_not_null(a);
	zassert_equal(FAKE_RADIO_ARM_RX, a->kind);
	zassert_equal(RADIANT_RADIO_OK_RC, a->rc);
	zassert_equal(now, a->t_arm);
	zassert_equal(now + 1000u, a->t_open);
	zassert_equal(now + 5000u, a->t_close);
	zassert_equal(8u, a->n_filters);
	zassert_equal(3u, a->fmt.addr_len);
	zassert_equal(RADIANT_RX_REPORT_CRC_FAIL, a->flags);
	zassert_equal(0x17u, a->filters[7].addr[2]);
	zassert_false(a->from_callback);

	a = fake_radio_arm(1u);
	zassert_not_null(a);
	zassert_equal(RADIANT_RADIO_EBUSY, a->rc);
	zassert_equal(0u, a->op);

	fake_radio_advance_to(req.t_close + 1u);
	expect_clean();
}

ZTEST(fake_radio, test_transmit_power_is_clamped_not_refused)
{
	struct radiant_tx_req req;
	uint32_t op = 0u;
	static const uint8_t body[10] = {
		TEST_TRANSTYPE, 0x0Au, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u
	};
	const struct fake_radio_arm *a;

	up();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	memcpy(req.addr, filt_track.addr, sizeof(req.addr));
	req.addr_len = filt_track.addr_len;
	req.body = body;
	req.body_len = sizeof(body);
	req.t_sync_at = radiant_radio_now() + 20000u;
	req.power.dbm = 100;

	/* The link budget does not care about half a dB, and failing here would
	 * make a shared scheduler brittle. */
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_tx(&req, &op));
	a = fake_radio_last_arm();
	zassert_not_null(a);
	zassert_equal(radiant_radio_caps_get()->tx_power_max_dbm, a->power_dbm_eff);
	(void)radiant_radio_abort();
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

ZTEST(fake_radio, test_arming_outside_the_enabled_state_is_estate)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now = radiant_radio_now();

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;

	zassert_equal(RADIANT_RADIO_ESTATE, radiant_radio_rx(&req, &op));
	zassert_equal(RADIANT_RADIO_ESTATE, radiant_radio_enable());
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_radio_init(NULL, NULL));

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&rec_cbs, NULL));
	zassert_equal(RADIANT_RADIO_ESTATE, radiant_radio_rx(&req, &op));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));

	/* disable() aborts what is in flight and still delivers its terminal
	 * event, and the clock stays monotonic across the pair. */
	now = radiant_radio_now();
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_disable());
	zassert_equal(1u, n_rxr);
	zassert_equal(RADIANT_RADIO_STATUS_ABORTED, rxr[0].status);
	zassert_equal(now, radiant_radio_now());
	zassert_equal(RADIANT_RADIO_ESTATE, radiant_radio_rx(&req, &op));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());
	zassert_true(radiant_radio_now() >= now);
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * HAL item 5: the contract the backends were made to agree on
 * ---------------------------------------------------------------------------
 */

/* A second init without a fresh start is ESTATE in every lifecycle state,
 * as on the nRF and TI backends: initialised, enabled, and disabled again. */
ZTEST(fake_radio, test_a_second_init_is_estate)
{
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_init(&rec_cbs, NULL));
	zassert_equal(RADIANT_RADIO_ESTATE, radiant_radio_init(&rec_cbs, NULL));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_enable());
	zassert_equal(RADIANT_RADIO_ESTATE, radiant_radio_init(&rec_cbs, NULL));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_disable());
	zassert_equal(RADIANT_RADIO_ESTATE, radiant_radio_init(&rec_cbs, NULL));
	expect_clean();
}

/* An RX terminal reports the requested t_close, not exact: on TIMEOUT and
 * on ABORTED alike (the mirror of the TX rule). */
ZTEST(fake_radio, test_an_rx_terminal_reports_t_close)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now;

	up();
	now = radiant_radio_now();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;

	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);
	zassert_equal(1u, n_rxr);
	zassert_equal(RADIANT_RADIO_STATUS_TIMEOUT, rxr[0].status);
	zassert_equal(req.t_close, rxr[0].t_sync);
	zassert_false(rxr[0].t_sync_exact);

	now = radiant_radio_now();
	req.t_open = now + 1000u;
	req.t_close = now + 5000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_abort());
	zassert_equal(2u, n_rxr);
	zassert_equal(RADIANT_RADIO_STATUS_ABORTED, rxr[1].status);
	zassert_equal(req.t_close, rxr[1].t_sync);
	zassert_false(rxr[1].t_sync_exact);
	expect_clean();
}

/* A window over caps.max_window_us is ENOTSUP before anything else about
 * when it is: the ceiling is a property of the request, checked ahead of
 * the lead (and, on a booking build, of arbitration). */
ZTEST(fake_radio, test_a_window_over_the_ceiling_is_enotsup_first)
{
	struct radiant_rx_req req;
	uint32_t op = 0u;
	radiant_time_t now;

	up();
	fake_radio_caps_mut()->max_window_us = 2000u;
	now = radiant_radio_now();
	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt_track;
	req.n_filters = 1u;
	/* Too late as well as too long. */
	req.t_open = now + 1u;
	req.t_close = req.t_open + 2001u;
	zassert_equal(RADIANT_RADIO_ENOTSUP, radiant_radio_rx(&req, &op));
	zassert_equal(0u, op);

	req.t_open = now + 1000u;
	req.t_close = req.t_open + 2000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));
	fake_radio_advance_to(req.t_close + 1u);
	expect_clean();
}

/* ---------------------------------------------------------------------------
 * The hardware presets' timing (backend drift item 7)
 * ---------------------------------------------------------------------------
 */

/* caps.rx_close_hold_us is modelled, not just advertised: on the CC26x2 the
 * receiver stays on 508 us past t_close (its end trigger stops sync search,
 * not reception), so the radio is busy until then and an empty window's
 * terminal lands there. A frame inside the hold is still outside the window
 * and not heard: acceptance stays [t_open, t_close]. */
ZTEST(fake_radio, test_the_close_hold_keeps_the_radio_busy_past_t_close)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter one = filt_track;
	uint32_t op = 0u;
	uint32_t op2 = 0u;
	radiant_time_t now;
	uint16_t hold;

	fake_radio_caps_preset_cc26xx(false);
	hold = radiant_radio_caps_get()->rx_close_hold_us;
	zassert_equal(508u, hold);
	up();
	now = radiant_radio_now();

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &one;
	req.n_filters = 1u;
	req.t_open = now + 2000u;
	req.t_close = now + 3000u;
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_air_frame(req.t_close + 100u, test_frame,
					   test_frame_len));
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));

	fake_radio_advance_to(req.t_close + hold - 1u);
	zassert_equal(0u, n_rxr, "terminal before the hold ran out");
	zassert_false(fake_radio_is_idle());
	zassert_equal(RADIANT_RADIO_EBUSY, radiant_radio_rx(&req, &op2));

	fake_radio_advance_to(req.t_close + hold);
	zassert_equal(1u, n_rxr);
	zassert_equal(RADIANT_RADIO_STATUS_TIMEOUT, rxr[0].status);
	zassert_equal(req.t_close + hold, rxr[0].now_in_cb);
	zassert_equal(0u, fake_radio_stats()->air_delivered);
	expect_clean();
}

/* The terminal delay knob stacks on the hold. */
ZTEST(fake_radio, test_the_terminal_delay_stacks_on_the_close_hold)
{
	struct radiant_rx_req req;
	struct radiant_rx_filter one = filt_track;
	uint32_t op = 0u;
	radiant_time_t now;

	fake_radio_caps_preset_cc26xx(false);
	fake_radio_set_rx_terminal_delay_us(40u);
	up();
	now = radiant_radio_now();

	memset(&req, 0, sizeof(req));
	req.fmt = &fmt_hal_len_from_body;
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &one;
	req.n_filters = 1u;
	req.t_open = now + 2000u;
	req.t_close = now + 3000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));

	fake_radio_advance_to(req.t_close + 508u + 40u - 1u);
	zassert_equal(0u, n_rxr);
	fake_radio_advance_to(req.t_close + 508u + 40u);
	zassert_equal(1u, n_rxr);
	zassert_equal(req.t_close + 548u, rxr[0].now_in_cb);
	expect_clean();
}
