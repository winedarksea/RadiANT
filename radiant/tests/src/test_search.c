/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work. Written against radiant_search.h,
 * radiant_frame.h, radiant_radio_hal.h, fake_radio.h and the measurements in
 * docs/spike-a-results.md and docs/spike-b-results.md. Nothing here derives
 * from sdk-ant, libant.a, or any ANT+ device profile document.
 *
 * Tests for wildcard search. radiant_search.c is policy, not plumbing, so every
 * test is about a decision. The seven that earn their place:
 *
 *   1. THE SWEEP IS ARITHMETIC: 8 filters -> 32 sets, 2 filters -> 128 sets,
 *      same code and instance shape. Running at fake_radio_caps_preset_rail()
 *      is the only thing that catches a hardcoded 8 before a RAIL backend
 *      exists.
 *   2. NOISE WITH A BAD CRC NEVER ACQUIRES - the single most important test
 *      here. Noise fires the 3-byte matcher ~1.4/s with eight filters armed
 *      (docs/spike-b-results.md, TX off), so ranking on match count would
 *      acquire a device number out of thermal noise within seconds.
 *   3. devnum_lo COMES FROM THE FILTER INDEX, proved by delivering a frame
 *      whose own low byte disagrees with the filter it arrived on and
 *      requiring the filter to win.
 *   4. ONE SWEEP SERVES EVERY SEARCHING CHANNEL - without it eight searches
 *      take ~64 s and tools/ant_session.py fails outright.
 *   5. THE SEEN CACHE STEERS, AND EXPIRES, so it never steers at a sensor
 *      that left the room.
 *   6. A TRUNCATED WINDOW DOES NOT SKIP A SET - the scheduler cuts search
 *      short for tracked channels, and a short window advancing the sweep
 *      would silently degrade coverage from certain-within-one-sweep to
 *      probabilistic.
 *   7. IDLE AND ZERO VIOLATIONS AT THE END OF EVERY TEST, via end_of_test().
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "../fake_radio.h"

#include <radiant/radiant_frame.h>
#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_search.h>

/* ---------------------------------------------------------------------------
 * Fixtures
 * ---------------------------------------------------------------------------
 */

static struct radiant_search g_s;

/* What the acquired/timeout callbacks recorded. */
#define CAP_MAX 32
struct capture {
	uint32_t n_acquired;
	uint32_t n_timeout;
	uint8_t  chan[CAP_MAX];
	struct radiant_search_result res[CAP_MAX];
	uint8_t  timeout_chan[CAP_MAX];
	uint32_t n_timeout_chan;
	/* Windows completed at the moment of each acquisition, so a test can
	 * assert that two channels were served by the SAME window rather than
	 * by two. */
	uint32_t at_window[CAP_MAX];
};
static struct capture g_cap;
static uint32_t g_windows_run;

static void on_acquired(uint8_t channel, const struct radiant_search_result *r, void *user)
{
	ARG_UNUSED(user);
	if (g_cap.n_acquired < CAP_MAX) {
		g_cap.chan[g_cap.n_acquired] = channel;
		g_cap.res[g_cap.n_acquired] = *r;
		g_cap.at_window[g_cap.n_acquired] = g_windows_run;
	}
	g_cap.n_acquired++;
}

static void on_timeout(uint8_t channel, void *user)
{
	ARG_UNUSED(user);
	if (g_cap.n_timeout_chan < CAP_MAX) {
		g_cap.timeout_chan[g_cap.n_timeout_chan] = channel;
		g_cap.n_timeout_chan++;
	}
	g_cap.n_timeout++;
}

static const struct radiant_search_cbs search_cbs = {
	.acquired = on_acquired,
	.timeout = on_timeout,
};

/* The HAL rx callback forwards straight into the module, exactly what
 * radiant_sched.c will do; nothing in this path calls the HAL, so a violation
 * here means a real contract breach, not a noisy fixture. */
static void rx_cb(const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(user);
	radiant_search_on_rx(&g_s, evt);
}

static const struct radiant_radio_cbs radio_cbs = {
	.rx = rx_cb,
	.tx = NULL,
};

static const uint8_t payload8[8] = { 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17 };

static const struct radiant_search_id_filter want_any = { 0u, 0u, 0u };

static void bring_up(bool report_crc_fail)
{
	struct radiant_search_cfg cfg;

	memset(&g_s, 0, sizeof(g_s));
	memset(&g_cap, 0, sizeof(g_cap));
	g_windows_run = 0u;

	radiant_search_cfg_default(&cfg);
	cfg.report_crc_fail = report_crc_fail;

	zassert_ok(radiant_search_init(&g_s, &cfg, &search_cbs, NULL),
		   "radiant_search_init failed");
	zassert_ok(radiant_radio_init(&radio_cbs, NULL));
	zassert_ok(radiant_radio_enable());
}

static void setup(void *f)
{
	ARG_UNUSED(f);
	fake_radio_reset();
}

static void end_of_test(void)
{
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
	zassert_equal(0u, fake_radio_viol_count(), "%s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
}

ZTEST_SUITE(radiant_search, NULL, NULL, setup, NULL, NULL);

/* ---------------------------------------------------------------------------
 * The pump - a two-line stand-in for radiant_sched.c
 *
 * The module never touches the radio, so the suite arms for it - the same
 * three calls the scheduler will make, which is how the API's shape gets
 * checked.
 * ---------------------------------------------------------------------------
 */

static struct radiant_search_window g_w;
static uint32_t g_op;

/* Ask for a window and arm it. Returns the radiant_search_window() code. */
static int win_open(void)
{
	struct radiant_rx_req req;
	radiant_time_t earliest;
	int rc;

	/* The scheduler, not the search module, owns the arming lead; getting
	 * this wrong shows up as RADIANT_RADIO_ETIME below. */
	earliest = radiant_radio_now() + radiant_radio_caps_get()->min_arm_lead_us + 100u;

	rc = radiant_search_window(&g_s, earliest, &g_w);
	if (rc != RADIANT_SEARCH_OK) {
		return rc;
	}

	memset(&req, 0, sizeof(req));
	req.fmt = g_w.fmt;
	req.rf_index = g_w.rf_index;
	req.filters = g_w.filters;
	req.n_filters = g_w.n_filters;
	req.t_open = g_w.t_open;
	req.t_close = g_w.t_close;
	req.flags = g_w.flags;

	zassert_ok(radiant_radio_rx(&req, &g_op), "arming the search window failed");
	radiant_search_armed(&g_s, g_op, req.t_open, req.t_close);
	return RADIANT_SEARCH_OK;
}

/* Same, but the scheduler gave back only len_us of it - a tracked channel
 * wanted the radio. */
static int win_open_truncated(uint32_t len_us)
{
	struct radiant_rx_req req;
	radiant_time_t earliest;
	int rc;

	earliest = radiant_radio_now() + radiant_radio_caps_get()->min_arm_lead_us + 100u;

	rc = radiant_search_window(&g_s, earliest, &g_w);
	if (rc != RADIANT_SEARCH_OK) {
		return rc;
	}

	memset(&req, 0, sizeof(req));
	req.fmt = g_w.fmt;
	req.rf_index = g_w.rf_index;
	req.filters = g_w.filters;
	req.n_filters = g_w.n_filters;
	req.t_open = g_w.t_open;
	req.t_close = g_w.t_open + len_us;
	req.flags = g_w.flags;

	zassert_ok(radiant_radio_rx(&req, &g_op));
	radiant_search_armed(&g_s, g_op, req.t_open, req.t_close);
	return RADIANT_SEARCH_OK;
}

/* Let the armed window run to its close and hand the module the time. */
static void win_run(void)
{
	const struct fake_radio_arm *a = fake_radio_arm_for_op(g_op);

	zassert_not_null(a);
	fake_radio_advance_to(a->t_close + 1u);
	g_windows_run++;
	radiant_search_tick(&g_s, radiant_radio_now());
}

/* Run windows until nothing is searching or the budget runs out. Returns how
 * many windows ran. */
static uint32_t pump(uint32_t max_windows)
{
	uint32_t n = 0;

	while (n < max_windows) {
		if (win_open() != RADIANT_SEARCH_OK) {
			break;
		}
		win_run();
		n++;
	}
	return n;
}

/* ---------------------------------------------------------------------------
 * 1. The sweep is arithmetic, at both capabilities
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_search, test_sweep_geometry_nrf)
{
	bring_up(false);

	zassert_equal(8u, radiant_search_filters_per_window(&g_s));
	zassert_equal(32u, radiant_search_sets(&g_s),
		      "eight filters must cover 256 device-number low bytes in 32 sets");
	/* 32 x 260 ms = 8.32 s worst case, comfortably inside ANT's 25 s
	 * default search timeout. */
	zassert_equal(32ULL * RADIANT_SEARCH_DWELL_DEFAULT_US, radiant_search_sweep_us(&g_s));

	end_of_test();
}

ZTEST(radiant_search, test_sweep_geometry_rail)
{
	fake_radio_caps_preset_rail();
	bring_up(false);

	zassert_equal(2u, radiant_search_filters_per_window(&g_s));
	zassert_equal(128u, radiant_search_sets(&g_s),
		      "two sync words must cover 256 low bytes in 128 sets - if this "
		      "reads 32 then the sweep length is hardcoded");
	/* 33.3 s, which is OUTSIDE ANT's 25 s default timeout. That is a real
	 * consequence of a two-filter radio and a fact a RAIL backend has to
	 * answer for; it is asserted here so the number is on the record rather
	 * than discovered when the second backend fails to pair. */
	zassert_true(radiant_search_sweep_us(&g_s) > 25000000ULL);

	end_of_test();
}

/* A caps preset the sweep cannot express must fail loudly rather than sweep
 * something it cannot arm. */
ZTEST(radiant_search, test_wildcard_capability_is_refused)
{
	struct radiant_search_cfg cfg;

	fake_radio_caps_mut()->filter_wildcard_dev = true;
	radiant_search_cfg_default(&cfg);
	memset(&g_s, 0, sizeof(g_s));

	zassert_equal(RADIANT_SEARCH_ENOTSUP,
		      radiant_search_init(&g_s, &cfg, &search_cbs, NULL),
		      "struct radiant_rx_filter has no wildcard field, so a backend "
		      "advertising one needs a HAL change, not a silent sweep");

	end_of_test();
}

/* ---------------------------------------------------------------------------
 * The window this module asks for
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_search, test_no_channel_means_no_window)
{
	struct radiant_search_window w;

	bring_up(false);

	zassert_equal(RADIANT_SEARCH_ENOENT,
		      radiant_search_window(&g_s, radiant_radio_now() + 1000u, &w),
		      "an idle search must not ask for the radio");
	zassert_equal(0u, radiant_search_n_searching(&g_s));

	end_of_test();
}

ZTEST(radiant_search, test_window_shape_and_filter_addresses)
{
	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());

	zassert_equal_ptr(radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), g_w.fmt,
			  "search must use the measured PCNF0=0 / BALEN=2 / "
			  "STATLEN=12 geometry");
	zassert_equal(RADIANT_RF_INDEX_ANT_PLUS, g_w.rf_index);
	zassert_equal(8u, g_w.n_filters);
	zassert_equal(0u, g_w.set_index);
	zassert_false(g_w.from_cache);
	zassert_equal(RADIANT_SEARCH_DWELL_DEFAULT_US, (uint32_t)(g_w.t_close - g_w.t_open),
		      "dwell is one channel period; a shorter one loses the "
		      "within-one-sweep guarantee for nothing");
	zassert_equal(0u, g_w.flags & RADIANT_RX_STOP_ON_FIRST,
		      "STOP_ON_FIRST on a search window would hand one channel an "
		      "acquisition and cost every other channel the dwell");

	/* Set 0 is devnum_lo 0..7, each as the on-air address A6 C5 lo. */
	for (uint8_t j = 0; j < g_w.n_filters; j++) {
		zassert_equal(3u, g_w.filters[j].addr_len);
		zassert_equal(RADIANT_NET_ADDR_ANT_PLUS_0, g_w.filters[j].addr[0]);
		zassert_equal(RADIANT_NET_ADDR_ANT_PLUS_1, g_w.filters[j].addr[1]);
		zassert_equal(j, g_w.filters[j].addr[2]);
	}

	win_run();
	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

ZTEST(radiant_search, test_window_shape_at_two_filters)
{
	fake_radio_caps_preset_rail();
	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(2u, g_w.n_filters, "the window must be sized by caps, not by 8");
	zassert_equal(0u, g_w.filters[0].addr[2]);
	zassert_equal(1u, g_w.filters[1].addr[2]);
	win_run();

	/* And the next window is the next two values, not the next eight. */
	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(1u, g_w.set_index);
	zassert_equal(2u, g_w.filters[0].addr[2]);
	zassert_equal(3u, g_w.filters[1].addr[2]);
	win_run();

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/*
 * A backend whose matcher cannot separate two addresses one bit apart gets
 * the two devnum_lo values of each set spread to opposite ends of the byte.
 * Not a preference: on a CC26x2 the correlator received ZERO frames from a
 * known 4 Hz transmitter at -47 dBm when the two sync words were one bit
 * apart, vs ~20 at eight bits apart - the consecutive layout made the part
 * deaf for a whole sweep with no error anywhere. See
 * caps.min_filter_hamming_bits.
 */
static uint8_t popcount8(uint8_t v)
{
	uint8_t n = 0;

	while (v != 0u) {
		n = (uint8_t)(n + (v & 1u));
		v = (uint8_t)(v >> 1);
	}
	return n;
}

ZTEST(radiant_search, test_spread_pairs_when_the_matcher_needs_distance)
{
	fake_radio_caps_preset_rail();
	fake_radio_caps_mut()->min_filter_hamming_bits = 4;
	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(2u, g_w.n_filters);
	zassert_equal(0x00u, g_w.filters[0].addr[2]);
	zassert_equal(0xFFu, g_w.filters[1].addr[2],
		      "set 0 must hold 0x00 and its complement, not 0x00 and 0x01");
	win_run();

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(0x01u, g_w.filters[0].addr[2]);
	zassert_equal(0xFEu, g_w.filters[1].addr[2]);
	win_run();

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/* Coverage survives the change: 128 sets still name all 256 values exactly
 * once, and the lookup the seen cache and a named-device search depend on
 * still points at the set that really carries the device. */
ZTEST(radiant_search, test_spread_pairs_still_cover_every_value_once)
{
	uint8_t seen[256] = { 0 };

	fake_radio_caps_preset_rail();
	fake_radio_caps_mut()->min_filter_hamming_bits = 4;
	bring_up(false);

	zassert_equal(128u, radiant_search_sets(&g_s));

	for (uint16_t set = 0; set < 128u; set++) {
		uint8_t a = (uint8_t)set;
		uint8_t b = (uint8_t)(set ^ 0xFFu);

		seen[a]++;
		seen[b]++;
		zassert_true(popcount8((uint8_t)(a ^ b)) >= 4,
			     "set %u pairs two addresses the matcher cannot split",
			     (unsigned)set);
	}

	for (unsigned v = 0; v < 256u; v++) {
		zassert_equal(1u, seen[v], "devnum_lo 0x%02X covered %u times",
			      v, (unsigned)seen[v]);
		zassert_equal((uint16_t)((v < 0x80u) ? v : (v ^ 0xFFu)),
			      radiant_search_set_for_devnum(&g_s, (uint16_t)v),
			      "the set lookup must find the set that really holds it");
	}

	end_of_test();
}

/* The default layout is untouched, which is what keeps the nRF backend and its
 * A/B baseline exactly where they were. */
ZTEST(radiant_search, test_consecutive_pairs_when_no_distance_is_needed)
{
	fake_radio_caps_preset_rail();
	zassert_equal(0u, radiant_radio_caps_get()->min_filter_hamming_bits);
	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(0u, g_w.filters[0].addr[2]);
	zassert_equal(1u, g_w.filters[1].addr[2]);
	win_run();

	zassert_equal(1u, radiant_search_set_for_devnum(&g_s, 3u));

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/* The invariant that the two configurations are two views of the same 15
 * bytes; a future format that breaks it announces itself here, not on air. */
ZTEST(radiant_search, test_crc_coverage_is_fifteen_either_way)
{
	zassert_equal(15, radiant_frame_covered_len(RADIANT_FRAME_CFG_SEARCH,
						RADIANT_FRAME_PAYLOAD_STD));
	zassert_equal(15, radiant_frame_covered_len(RADIANT_FRAME_CFG_TRACKING,
						RADIANT_FRAME_PAYLOAD_STD));
	end_of_test();
}

/* ---------------------------------------------------------------------------
 * Acquisition
 * ---------------------------------------------------------------------------
 */

/* devnum 0x01FF sits in the LAST set at eight filters (0xFF / 8 = 31), so this
 * is the worst case of the sweep and not a lucky early hit. */
#define WORST_CASE_DEVNUM 0x01FFu
#define WORST_CASE_DTYPE  0x0Bu
#define WORST_CASE_TTYPE  0x05u

ZTEST(radiant_search, test_acquires_within_one_sweep)
{
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;
	uint32_t windows;
	const struct radiant_search_stats *st;

	bring_up(false);

	len = fake_radio_build_ant_frame(frame, WORST_CASE_DEVNUM, WORST_CASE_DTYPE,
					 WORST_CASE_TTYPE, payload8);

	/* A real master: 45 frames at the measured 4.005 Hz, spanning more than
	 * a full 8.32 s sweep. Dwell > period, so every window necessarily
	 * contains a transmission - the argument for dwell = one channel
	 * period. */
	(void)fake_radio_air_master(radiant_radio_now() + 1000u, FAKE_RADIO_ANT_PERIOD_US,
				    45u, frame, len);

	zassert_ok(radiant_search_begin(&g_s, 3u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	windows = pump(radiant_search_sets(&g_s) + 1u);

	zassert_equal(1u, g_cap.n_acquired, "not acquired within one sweep");
	zassert_true(windows <= radiant_search_sets(&g_s),
		     "took %u windows for a %u-set sweep", windows,
		     radiant_search_sets(&g_s));
	zassert_equal(3u, g_cap.chan[0]);
	zassert_equal(WORST_CASE_DEVNUM, g_cap.res[0].id.device_number);
	zassert_equal(WORST_CASE_DTYPE, g_cap.res[0].id.device_type);
	zassert_equal(WORST_CASE_TTYPE, g_cap.res[0].id.trans_type);
	zassert_equal(31u, g_cap.res[0].set_index);
	zassert_equal(7u, g_cap.res[0].filter_index, "0xFF is slot 7 of set 31");

	/* The first broadcast comes with the channel ID, so the channel does not
	 * have to wait another period for data it has already heard. */
	zassert_equal(RADIANT_FRAME_PAYLOAD_STD, g_cap.res[0].frame.payload_len);
	zassert_mem_equal(payload8, g_cap.res[0].frame.payload, sizeof(payload8));
	zassert_equal(RADIANT_CTRL_BROADCAST, g_cap.res[0].frame.ctrl_byte);

	/* Acquire mode: the channel has left the search. */
	zassert_false(radiant_search_is_searching(&g_s, 3u));
	zassert_equal(0u, radiant_search_n_searching(&g_s));

	st = radiant_search_get_stats(&g_s);
	zassert_equal(1u, st->acquired);
	zassert_equal(0u, st->crc_fail);
	zassert_equal(0u, st->bad_filter_index);

	end_of_test();
}

/*
 * The same acquisition at two filters. Deliberately a device in an early set:
 * a 128-set sweep would need 128 arm records and the mock's arm log holds 64,
 * so exercising the full RAIL sweep would fail on the harness rather than on
 * the module. What matters here is that the SAME code acquires with a
 * two-filter window and a 128-set geometry, which the geometry test above
 * pins separately.
 */
ZTEST(radiant_search, test_acquires_at_two_filters)
{
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;

	fake_radio_caps_preset_rail();
	bring_up(false);

	len = fake_radio_build_ant_frame(frame, 0x0302u, 0x78u, 1u, payload8);
	(void)fake_radio_air_master(radiant_radio_now() + 1000u, FAKE_RADIO_ANT_PERIOD_US,
				    12u, frame, len);

	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	(void)pump(8u);

	zassert_equal(1u, g_cap.n_acquired);
	zassert_equal(0x0302u, g_cap.res[0].id.device_number);
	zassert_equal(1u, g_cap.res[0].set_index, "0x02 / 2 = set 1");
	zassert_equal(0u, g_cap.res[0].filter_index);

	end_of_test();
}

/* ---------------------------------------------------------------------------
 * 3. devnum_lo comes from the filter index and from nowhere else
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_search, test_devnum_lo_from_filter_index_not_body)
{
	struct fake_radio_air air;
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 1u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(0u, g_w.set_index);

	/*
	 * A frame built for device 0xBEEF (low byte 0xEF) delivered on filter
	 * slot 3 of set 0 (device_number low byte 0x03). Real silicon can't
	 * produce this pairing; it's arranged deliberately as the only
	 * construction that tells "read the filter table" apart from "read the
	 * frame". Correct answer: 0xBE03. If radiant_search.c ever parses the low
	 * byte out of the frame, this reads 0xBEEF and nothing else notices.
	 */
	len = fake_radio_build_ant_frame(frame, 0xBEEFu, 0x0Bu, 5u, payload8);

	fake_radio_air_init(&air);
	air.t_sync = g_w.t_open + 1000u;
	memcpy(air.bytes, frame, len);
	air.len = len;
	air.match = FAKE_RADIO_MATCH_INDEX;
	air.filter_index = 3u;
	zassert_ok(fake_radio_air_add(&air));

	win_run();

	zassert_equal(1u, g_cap.n_acquired);
	zassert_equal(0xBE03u, g_cap.res[0].id.device_number,
		      "devnum_lo must come from filter_index, never from the body");
	zassert_equal(3u, g_cap.res[0].filter_index);

	end_of_test();
}

/*
 * The same recovery when the window was MERGED by the scheduler. radiant_sched.c
 * combines several channels into one hardware window, so the HAL's
 * filter_index indexes the merged array and the scheduler hands back the
 * index into the requesting channel's own filters beside the event. Reading
 * evt->filter_index directly would recover a real but wrong device number -
 * wrong, plausible, and invisible on a bench.
 */
ZTEST(radiant_search, test_merged_window_uses_the_supplied_filter_index)
{
	struct radiant_rx_event evt;
	struct radiant_search_window w;
	uint8_t body[RADIANT_FRAME_HDR_LEN_SEARCH + RADIANT_FRAME_PAYLOAD_STD];

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 2u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_ok(radiant_search_window(&g_s, radiant_radio_now() + 1000u, &w));
	zassert_equal(0u, w.set_index);
	/* No HAL operation id exists for a merged request; the scheduler routed
	 * the event to us and says so with the sentinel. */
	radiant_search_armed(&g_s, RADIANT_SEARCH_OP_EXTERNAL, w.t_open, w.t_close);

	body[0] = 0xBEu;              /* devnum_hi */
	body[1] = 0x0Bu;              /* device type */
	body[2] = 0x05u;              /* transmission type */
	body[3] = RADIANT_CTRL_BROADCAST; /* the control byte */
	memcpy(&body[4], payload8, sizeof(payload8));

	memset(&evt, 0, sizeof(evt));
	evt.op = 0x0BADBEEFu;   /* somebody else's operation id */
	evt.status = RADIANT_RADIO_STATUS_OK;
	evt.t_sync = radiant_radio_now();
	evt.t_sync_exact = true;
	evt.filter_index = 0u;  /* the MERGED index; must be ignored */
	evt.body = body;
	evt.body_len = (uint8_t)sizeof(body);
	evt.has_rssi = true;
	evt.rssi_dbm = FAKE_RADIO_RSSI_BENCH_DBM;

	radiant_search_on_rx_indexed(&g_s, &evt, 6u);

	zassert_equal(1u, g_cap.n_acquired);
	zassert_equal(0xBE06u, g_cap.res[0].id.device_number,
		      "the scheduler's remapped index must win");
	zassert_not_equal(0xBE00u, g_cap.res[0].id.device_number,
			  "reading evt->filter_index on a merged window recovers a "
			  "real but wrong device number");
	zassert_equal(0u, radiant_search_get_stats(&g_s)->late_events,
		      "the EXTERNAL sentinel means the caller owns the routing");

	radiant_search_on_done(&g_s, true, true, w.t_close);
	end_of_test();
}

/*
 * THE SWEEP MUST STILL ADVANCE WHEN EVERY WINDOW IS CUT SHORT.
 *
 * Regression test: a tracked slave takes the radio every 249.7 ms, so with
 * anything tracking, EVERY search window is preempted and ends
 * RADIANT_SCHED_DONE_ABORTED. Since an abort credited nothing, dwell never
 * reached dwell_us and the sweep sat on one set forever - a sensor elsewhere
 * was undiscoverable for as long as any channel tracked. Measured on air
 * 2026-08-10: a -25 dBm transmitter in set 2 was invisible to a 60 s scan.
 *
 * Driven through the EXTERNAL sentinel, exactly how radiant_api.c drives it, and
 * needing no HAL: window -> armed -> cut short.
 */
ZTEST(radiant_search, test_a_sweep_advances_even_when_every_window_is_cut_short)
{
	struct radiant_search_window w;
	radiant_time_t earliest;
	uint16_t first_set;
	int i;

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	earliest = radiant_radio_now() + radiant_radio_caps_get()->min_arm_lead_us + 100u;
	zassert_ok(radiant_search_window(&g_s, earliest, &w));
	first_set = w.set_index;

	/*
	 * Each pass listens for a slice and is then cut short, which is what a
	 * tracked channel does to a search window. Bounded so a stuck sweep
	 * fails the test instead of hanging it: one dwell is 260 ms and each
	 * pass credits 20 ms, so 13 passes is the honest figure and 100 leaves
	 * room without letting a genuinely frozen sweep spin for ever.
	 */
	for (i = 0; i < 100 && w.set_index == first_set; i++) {
		radiant_time_t cut = w.t_open + 20000u;

		if (cut > w.t_close) {
			cut = w.t_close;
		}
		radiant_search_armed(&g_s, RADIANT_SEARCH_OP_EXTERNAL, w.t_open,
				 w.t_close);
		radiant_search_on_done(&g_s, false, true, cut);

		earliest = cut + radiant_radio_caps_get()->min_arm_lead_us + 100u;
		zassert_ok(radiant_search_window(&g_s, earliest, &w));
	}

	zassert_not_equal(first_set, w.set_index,
			  "the sweep never left set %u: a window that is cut "
			  "short credits no dwell, so with a tracked channel "
			  "taking the radio every period the sweep freezes and "
			  "every device outside this set is undiscoverable",
			  first_set);
	end_of_test();
}

/* The other half of the contract: a window that never opened must credit
 * nothing, or "credit what was listened to" drifts into "credit anything
 * asked for", and a sweep skipping a set on every refused arm loses coverage
 * silently. */
ZTEST(radiant_search, test_a_window_that_never_opened_credits_nothing)
{
	struct radiant_search_window w;
	radiant_time_t earliest;
	uint16_t first_set;
	int i;

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	earliest = radiant_radio_now() + radiant_radio_caps_get()->min_arm_lead_us + 100u;
	zassert_ok(radiant_search_window(&g_s, earliest, &w));
	first_set = w.set_index;

	for (i = 0; i < 20; i++) {
		radiant_search_armed(&g_s, RADIANT_SEARCH_OP_EXTERNAL, w.t_open,
				 w.t_close);
		/* Never armed: MISSED or FAILED, and ended_at is meaningless. */
		radiant_search_on_done(&g_s, false, false, w.t_close);

		zassert_ok(radiant_search_window(&g_s, earliest, &w));
		zassert_equal(first_set, w.set_index,
			      "a request that never reached the air credited "
			      "dwell and skipped set %u",
			      first_set);
	}
	end_of_test();
}

/* stats.bad_filter_index has no test: the mock cannot produce an index
 * outside the window's filter array, so it's only reachable from a real
 * backend that miscounted. The guard stays in radiant_search.c to avoid
 * indexing filter_lo[] past the array end, and the counter stays so the
 * bench sees it if it ever happens. */

/* ---------------------------------------------------------------------------
 * 2. Noise never acquires
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_search, test_noise_with_bad_crc_never_acquires)
{
	const struct radiant_search_stats *st;
	uint32_t queued;

	/* CRC failures reported, which is the only way the module can be shown
	 * to receive them and still refuse to act. */
	bring_up(true);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	/* Spike B measured 19, 22, 27 CRC failures per 15 s window with eight
	 * 3-byte matchers armed, TX off - about 1.4/s. Thirty over three dwells
	 * matches that rate, with no transmitter here at all. */
	queued = fake_radio_air_noise(radiant_radio_now() + 1000u,
				      radiant_radio_now() + 3u * RADIANT_SEARCH_DWELL_DEFAULT_US,
				      30u, FAKE_RADIO_RSSI_NOISE_DBM);
	zassert_equal(30u, queued);

	(void)pump(4u);

	st = radiant_search_get_stats(&g_s);
	zassert_true(st->crc_fail > 0u, "the noise never reached the module");
	zassert_equal(0u, st->frames_ok);
	zassert_equal(0u, st->acquired);
	zassert_equal(0u, g_cap.n_acquired,
		      "a CRC failure is evidence about the noise floor and about "
		      "nothing else");
	zassert_equal(0u, radiant_search_seen_count(&g_s, radiant_radio_now()),
		      "noise must not populate the seen cache either");
	zassert_true(radiant_search_is_searching(&g_s, 0u),
		     "the channel must still be searching");

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/* And with reporting off - the default - the module never even sees them, which
 * is what keeps a search window cheap. */
ZTEST(radiant_search, test_noise_is_suppressed_by_default)
{
	const struct radiant_search_stats *st;

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	(void)fake_radio_air_noise(radiant_radio_now() + 1000u,
				   radiant_radio_now() + 2u * RADIANT_SEARCH_DWELL_DEFAULT_US,
				   20u, FAKE_RADIO_RSSI_NOISE_DBM);

	(void)pump(3u);

	st = radiant_search_get_stats(&g_s);
	zassert_equal(0u, st->crc_fail);
	zassert_equal(0u, g_cap.n_acquired);
	zassert_true(fake_radio_stats()->crc_fail_suppressed > 0u,
		     "the mock should have dropped them before the core saw them");

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/* A real transmitter buried in noise is still acquired: the CRC gate rejects
 * the noise without rejecting the signal. */
ZTEST(radiant_search, test_signal_is_found_through_noise)
{
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;

	bring_up(true);

	/* devnum_lo 0x02 -> set 0, so this needs only the first window. */
	len = fake_radio_build_ant_frame(frame, 0x0402u, 0x0Bu, 5u, payload8);
	(void)fake_radio_air_master(radiant_radio_now() + 1000u, FAKE_RADIO_ANT_PERIOD_US,
				    4u, frame, len);
	(void)fake_radio_air_noise(radiant_radio_now() + 1500u,
				   radiant_radio_now() + RADIANT_SEARCH_DWELL_DEFAULT_US,
				   12u, FAKE_RADIO_RSSI_NOISE_DBM);

	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	(void)pump(2u);

	zassert_equal(1u, g_cap.n_acquired);
	zassert_equal(0x0402u, g_cap.res[0].id.device_number);
	zassert_true(radiant_search_get_stats(&g_s)->crc_fail > 0u);
	/* The RSSI column is what separates the two by ~70 dB on the bench. */
	zassert_equal(FAKE_RADIO_RSSI_BENCH_DBM, g_cap.res[0].rssi_dbm);

	end_of_test();
}

/* ---------------------------------------------------------------------------
 * 4. One sweep serves every searching channel
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_search, test_one_window_serves_several_channels)
{
	struct radiant_search_id_filter want_pwr = { 0u, 0x0Bu, 0u };
	struct radiant_search_id_filter want_hrm = { 0u, 0x78u, 0u };
	uint8_t f_pwr[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t f_hrm[FAKE_RADIO_AIR_FRAME_MAX];
	uint32_t arms_before;
	uint8_t n_pwr, n_hrm;

	bring_up(false);

	/* Two sensors whose low bytes share set 2 at eight filters: 0x10 and
	 * 0x13 both divide to 2. One window, two masters, two channels. */
	n_pwr = fake_radio_build_ant_frame(f_pwr, 0x0510u, 0x0Bu, 5u, payload8);
	n_hrm = fake_radio_build_ant_frame(f_hrm, 0x0613u, 0x78u, 1u, payload8);

	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_pwr,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	zassert_ok(radiant_search_begin(&g_s, 1u, RADIANT_SEARCH_MODE_ACQUIRE, &want_hrm,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	zassert_equal(2u, radiant_search_n_searching(&g_s));

	/* Walk the sweep to set 2 with nothing on the air. */
	while (true) {
		zassert_equal(RADIANT_SEARCH_OK, win_open());
		if (g_w.set_index == 2u) {
			break;
		}
		win_run();
	}

	arms_before = fake_radio_arm_count();

	zassert_ok(fake_radio_air_frame(g_w.t_open + 1000u, f_pwr, n_pwr));
	zassert_ok(fake_radio_air_frame(g_w.t_open + 2000u, f_hrm, n_hrm));
	win_run();

	zassert_equal(2u, g_cap.n_acquired, "one window must serve both channels");
	zassert_equal(arms_before, fake_radio_arm_count(),
		      "serving two channels must not cost two windows");
	zassert_equal(g_cap.at_window[0], g_cap.at_window[1],
		      "both acquisitions must come from the same window");

	/* Each channel got its own sensor, matched on device type. */
	zassert_equal(0u, g_cap.chan[0]);
	zassert_equal(0x0510u, g_cap.res[0].id.device_number);
	zassert_equal(1u, g_cap.chan[1]);
	zassert_equal(0x0613u, g_cap.res[1].id.device_number);

	zassert_equal(0u, radiant_search_n_searching(&g_s));
	end_of_test();
}

/* A channel whose filter does not match must not be handed somebody else's
 * sensor. The other half of the same mechanism. */
ZTEST(radiant_search, test_a_channel_only_gets_what_it_asked_for)
{
	struct radiant_search_id_filter want_hrm = { 0u, 0x78u, 0u };
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_hrm,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	len = fake_radio_build_ant_frame(frame, 0x0703u, 0x0Bu, 5u, payload8);
	zassert_ok(fake_radio_air_frame(g_w.t_open + 1000u, frame, len));
	win_run();

	zassert_equal(0u, g_cap.n_acquired);
	/* The frame was still decoded, and the cache still learned the device -
	 * which is what makes a later search for it fast. */
	zassert_equal(1u, radiant_search_get_stats(&g_s)->frames_ok);
	zassert_equal(1u, radiant_search_seen_count(&g_s, radiant_radio_now()));
	zassert_true(radiant_search_is_searching(&g_s, 0u));

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/*
 * Two wildcard ACQUIRE channels both match the same frame, as several
 * simultaneous "find any device" channels do during a multi-sensor pairing
 * session. The frame must claim only one; the other keeps searching for the
 * NEXT device. Before the fix, id_match()'s all-zero wildcard matched both
 * and handle_ok() offered the acquisition to each in turn, so one frame
 * emptied every wildcard channel at once.
 */
ZTEST(radiant_search, test_a_frame_claims_only_one_wildcard_acquire_channel)
{
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	zassert_ok(radiant_search_begin(&g_s, 1u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	zassert_equal(2u, radiant_search_n_searching(&g_s));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	len = fake_radio_build_ant_frame(frame, 0x0703u, 0x78u, 1u, payload8);
	zassert_ok(fake_radio_air_frame(g_w.t_open + 1000u, frame, len));
	win_run();

	zassert_equal(1u, g_cap.n_acquired,
		      "one frame must claim exactly one wildcard ACQUIRE channel");
	zassert_equal(0u, g_cap.chan[0], "the first matching channel in order claims it");
	zassert_equal(0x0703u, g_cap.res[0].id.device_number);

	zassert_equal(1u, radiant_search_n_searching(&g_s),
		      "the second wildcard channel must still be searching");
	zassert_false(radiant_search_is_searching(&g_s, 0u));
	zassert_true(radiant_search_is_searching(&g_s, 1u));

	zassert_ok(radiant_search_end(&g_s, 1u));
	end_of_test();
}

/* The pairing bit rides on the on-air device type, and a search must match
 * through it while still reporting it. */
ZTEST(radiant_search, test_pairing_bit_matches_and_survives)
{
	struct radiant_search_id_filter want_pwr = { 0u, 0x0Bu, 0u };
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_pwr,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	len = fake_radio_build_ant_frame(frame, 0x0804u,
					 (uint8_t)(0x0Bu | RADIANT_DEVICE_TYPE_PAIRING_BIT),
					 5u, payload8);
	zassert_ok(fake_radio_air_frame(g_w.t_open + 1000u, frame, len));
	win_run();

	zassert_equal(1u, g_cap.n_acquired,
		      "a sensor in pairing mode must still be found");
	zassert_equal((uint8_t)(0x0Bu | RADIANT_DEVICE_TYPE_PAIRING_BIT),
		      g_cap.res[0].id.device_type,
		      "and the pairing bit must reach the caller intact");

	end_of_test();
}

/* ---------------------------------------------------------------------------
 * 5. The seen cache
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_search, test_seen_cache_reacquires_in_one_window)
{
	struct radiant_search_id_filter want_pwr = { 0u, 0x0Bu, 0u };
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;
	uint32_t first_windows;
	const struct radiant_search_stats *st;

	bring_up(false);

	/* devnum_lo 0xA1 -> set 20 at eight filters: far enough into the sweep
	 * that the difference between steered and unsteered is unmistakable. */
	len = fake_radio_build_ant_frame(frame, 0x09A1u, 0x0Bu, 5u, payload8);
	(void)fake_radio_air_master(radiant_radio_now() + 1000u, FAKE_RADIO_ANT_PERIOD_US,
				    40u, frame, len);

	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_pwr,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	first_windows = pump(radiant_search_sets(&g_s) + 1u);

	zassert_equal(1u, g_cap.n_acquired);
	zassert_equal(20u, g_cap.res[0].set_index);
	zassert_true(first_windows >= 20u,
		     "the cold search should have had to walk the sweep");
	zassert_false(g_cap.res[0].from_cache);
	zassert_equal(1u, radiant_search_seen_count(&g_s, radiant_radio_now()));

	/* The sensor drops and the channel searches again. Same filter, no
	 * device number named - the cache is the only thing that can steer. */
	fake_radio_air_clear();
	(void)fake_radio_air_master(radiant_radio_now() + 1000u, FAKE_RADIO_ANT_PERIOD_US,
				    8u, frame, len);

	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_pwr,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_true(g_w.from_cache, "the cache should have steered this window");
	zassert_equal(20u, g_w.set_index);
	win_run();

	zassert_equal(2u, g_cap.n_acquired,
		      "re-acquisition should take one dwell, not half a sweep");
	zassert_true(g_cap.res[1].from_cache);

	st = radiant_search_get_stats(&g_s);
	zassert_equal(1u, st->cache_acquires);
	zassert_true(st->cache_steers >= 1u);

	end_of_test();
}

/* A channel that names its device number knows devnum_lo outright, so the same
 * queue-jump applies with no cache involved at all. */
ZTEST(radiant_search, test_named_device_number_steers_directly)
{
	struct radiant_search_id_filter want_one = { 0x09A1u, 0u, 0u };

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_one,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(20u, g_w.set_index, "0xA1 / 8 = 20");
	zassert_true(g_w.from_cache);
	zassert_equal(0xA1u, g_w.filters[1].addr[2]);
	win_run();

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

ZTEST(radiant_search, test_seen_cache_expires_at_sixty_seconds)
{
	struct radiant_search_id_filter want_pwr = { 0u, 0x0Bu, 0u };
	struct radiant_channel_id id = { 0x09A1u, 0x0Bu, 5u };
	radiant_time_t t0;

	bring_up(false);

	t0 = radiant_radio_now();
	radiant_search_seen_note(&g_s, &id, t0);
	zassert_equal(1u, radiant_search_seen_count(&g_s, t0));
	zassert_true(radiant_search_seen_lookup(&g_s, &want_pwr, t0, NULL));

	/* One microsecond inside the lifetime. */
	zassert_true(radiant_search_seen_lookup(&g_s, &want_pwr,
					    t0 + RADIANT_SEARCH_SEEN_LIFETIME_US - 1u,
					    NULL));
	/* One microsecond outside it. */
	zassert_false(radiant_search_seen_lookup(&g_s, &want_pwr,
					     t0 + RADIANT_SEARCH_SEEN_LIFETIME_US + 1u,
					     NULL));
	zassert_equal(0u, radiant_search_seen_count(&g_s,
						t0 + RADIANT_SEARCH_SEEN_LIFETIME_US + 1u));

	/* And an expired entry must not steer: the sweep starts where the round
	 * robin left it. */
	fake_radio_advance(RADIANT_SEARCH_SEEN_LIFETIME_US + 1000000u);
	radiant_search_tick(&g_s, radiant_radio_now());
	zassert_equal(0u, radiant_search_seen_count(&g_s, radiant_radio_now()));

	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_pwr,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_false(g_w.from_cache, "a stale entry must not steer the sweep");
	zassert_equal(0u, g_w.set_index);
	win_run();

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/* A sensor transmitting four times a second must not evict the other fifteen
 * entries in four seconds. */
ZTEST(radiant_search, test_seen_cache_refreshes_in_place)
{
	struct radiant_channel_id a = { 0x1111u, 0x0Bu, 5u };
	struct radiant_channel_id b = { 0x2222u, 0x78u, 1u };
	radiant_time_t t = radiant_radio_now();

	bring_up(false);

	radiant_search_seen_note(&g_s, &b, t);
	for (uint32_t i = 0; i < 64u; i++) {
		radiant_search_seen_note(&g_s, &a, t + i * 1000u);
	}
	zassert_equal(2u, radiant_search_seen_count(&g_s, t + 64000u),
		      "a chatty sensor must refresh its entry, not fill the ring");

	end_of_test();
}

/* ---------------------------------------------------------------------------
 * Scan mode
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_search, test_scan_mode_reports_every_frame_and_never_leaves)
{
	uint8_t f_a[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t f_b[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t n_a, n_b;
	const struct radiant_search_stats *st;

	bring_up(false);

	/* Two different sensors in set 0 (low bytes 0x01 and 0x05). A scanning
	 * receiver wants both, and wants them repeatedly. */
	n_a = fake_radio_build_ant_frame(f_a, 0x0A01u, 0x0Bu, 5u, payload8);
	n_b = fake_radio_build_ant_frame(f_b, 0x0B05u, 0x78u, 1u, payload8);

	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_SCAN, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_ok(fake_radio_air_frame(g_w.t_open + 1000u, f_a, n_a));
	zassert_ok(fake_radio_air_frame(g_w.t_open + 40000u, f_b, n_b));
	zassert_ok(fake_radio_air_frame(g_w.t_open + 90000u, f_a, n_a));
	win_run();

	zassert_equal(3u, g_cap.n_acquired,
		      "scan mode reports every matching frame, not the first");
	zassert_true(radiant_search_is_searching(&g_s, 0u),
		     "a scanning channel never leaves the search");

	st = radiant_search_get_stats(&g_s);
	zassert_equal(3u, st->scan_reports);
	zassert_equal(0u, st->acquired);

	/*
	 * And the sweep keeps re-arming bounded windows, because RADIANT_TIME_NEVER
	 * is not a legal window edge - there is no open-ended receive to leave
	 * running.
	 */
	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(1u, g_w.set_index);
	zassert_true(g_w.t_close != RADIANT_TIME_NEVER);
	win_run();

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/*
 * radiant_search_chan_mode() is what a caller must check before converting a
 * matched channel to tracking - api_search_acquired() used to skip it and
 * called radiant_channel_on_acquired() unconditionally, silently turning a
 * background scan into a one-shot acquire. radiant_api.c has no slave-side test
 * scaffolding of its own (see the API composition suite's README), so this
 * is the coverage there is.
 */
ZTEST(radiant_search, test_chan_mode_reports_which_kind_of_search)
{
	bring_up(false);

	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	zassert_ok(radiant_search_begin(&g_s, 1u, RADIANT_SEARCH_MODE_SCAN, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_MODE_ACQUIRE, radiant_search_chan_mode(&g_s, 0u));
	zassert_equal(RADIANT_SEARCH_MODE_SCAN, radiant_search_chan_mode(&g_s, 1u));
	/* Not searching at all: the safe default is the mode that does not
	 * keep searching, so a caller that got this wrong fails toward ending
	 * a search rather than toward leaving one running forever. */
	zassert_equal(RADIANT_SEARCH_MODE_ACQUIRE, radiant_search_chan_mode(&g_s, 2u));

	zassert_ok(radiant_search_end(&g_s, 0u));
	zassert_ok(radiant_search_end(&g_s, 1u));
	end_of_test();
}

/* ---------------------------------------------------------------------------
 * 6. A truncated window does not skip a set
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_search, test_truncated_windows_do_not_skip_a_set)
{
	const struct radiant_search_stats *st;

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	/* Four quarter-length windows: the scheduler kept taking the radio back
	 * for a tracked channel. The set must stay selected until the dwell is
	 * actually spent on it. */
	for (uint32_t i = 0; i < 4u; i++) {
		zassert_equal(RADIANT_SEARCH_OK,
			      win_open_truncated(RADIANT_SEARCH_DWELL_DEFAULT_US / 4u));
		zassert_equal(0u, g_w.set_index,
			      "window %u advanced the sweep on a short dwell", i);
		win_run();
	}

	st = radiant_search_get_stats(&g_s);
	zassert_equal(1u, st->sets_advanced,
		      "four quarter windows are one dwell, not four sets");

	/* The dwell is now fully credited, so the fifth window moves on. */
	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(1u, g_w.set_index);
	win_run();

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/*
 * A SET WITH ONLY A SLIVER OF DWELL LEFT IS FINISHED, AND SAYS SO IN BOTH
 * ANSWERS AT ONCE. The caller's contract needs radiant_search_set_complete() and
 * radiant_search_dwell_remaining() to agree - "not finished" plus "600 us" is a
 * sweep pinned forever on chunks too short to hear anything, since an
 * arbitrated backend is likely to refuse a chunk that short, which credits
 * ZERO and never shrinks the residue.
 *
 * MEASURED: on the nRF54L15 DK beside a 1 s BLE advertiser, chunks collapsed
 * from 94200 us to 3378 us and stayed there for 11339 consecutive chunks,
 * sets_advanced stuck at 1, frames_ok at 0, while MPSL granted 97.7% of every
 * request. Same image with the advertiser off found the sensor in two chunks.
 *
 * Asserted on both functions deliberately: either alone can pass while the
 * pair is still inconsistent, and the inconsistency is what wedges the sweep.
 */
ZTEST(radiant_search, test_a_sliver_of_remaining_dwell_finishes_the_set)
{
	uint32_t owed;

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	/* Spend all but a sliver - less than the epsilon - of the set's budget. */
	zassert_equal(RADIANT_SEARCH_OK,
		      win_open_truncated(RADIANT_SEARCH_DWELL_DEFAULT_US -
					 (RADIANT_SEARCH_DWELL_EPSILON_US / 2u)));
	win_run();

	owed = radiant_search_dwell_remaining(&g_s);
	zassert_equal(0u, owed,
		      "a %u us residue was offered as the next chunk's ceiling; "
		      "too short to hear a 250 ms master and, once refused, "
		      "never repaid", owed);
	zassert_true(radiant_search_set_complete(&g_s),
		     "the set is not finished but has nothing arm-worthy left - "
		     "this is the state that pins the sweep on one set for ever");

	/* And the proof that it is not merely cosmetic: the sweep moves on, and
	 * the fresh set restores a full-length chunk ceiling. */
	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(1u, g_w.set_index, "the sweep did not advance");
	zassert_equal(RADIANT_SEARCH_DWELL_DEFAULT_US,
		      radiant_search_dwell_remaining(&g_s),
		      "a fresh set must restore the full dwell ceiling");
	win_run();

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/* An aborted window credits nothing, which costs a dwell and cannot lose
 * coverage. Crediting it would. */
ZTEST(radiant_search, test_aborted_window_credits_nothing)
{
	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(0u, g_w.set_index);
	zassert_ok(radiant_radio_abort());

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(0u, g_w.set_index, "an aborted window must not advance the sweep");
	win_run();

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/* The scheduler asked, then could not arm. Same rule. */
ZTEST(radiant_search, test_arm_failure_leaves_the_set_selected)
{
	struct radiant_search_window w;

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_ok(radiant_search_window(&g_s, radiant_radio_now() + 1000u, &w));
	zassert_equal(0u, w.set_index);
	radiant_search_arm_failed(&g_s);

	zassert_ok(radiant_search_window(&g_s, radiant_radio_now() + 1000u, &w));
	zassert_equal(0u, w.set_index);

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/* Only one window may be in flight; the HAL has one operation slot. */
ZTEST(radiant_search, test_second_window_while_one_is_armed_is_refused)
{
	struct radiant_search_window w;

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(RADIANT_SEARCH_ESTATE,
		      radiant_search_window(&g_s, radiant_radio_now() + 1000u, &w));
	win_run();

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/* ---------------------------------------------------------------------------
 * Timeouts and late events
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_search, test_search_timeout_fires_and_the_channel_leaves)
{
	bring_up(false);

	/* Two dwells' worth, so the timeout lands mid-sweep with nothing heard. */
	zassert_ok(radiant_search_begin(&g_s, 7u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), 2u * RADIANT_SEARCH_DWELL_DEFAULT_US));

	(void)pump(6u);

	zassert_equal(1u, g_cap.n_timeout);
	zassert_equal(7u, g_cap.timeout_chan[0]);
	zassert_false(radiant_search_is_searching(&g_s, 7u));
	zassert_equal(0u, radiant_search_n_searching(&g_s));
	zassert_equal(1u, radiant_search_get_stats(&g_s)->timeouts);

	/* And with nothing searching the module stops asking for the radio. */
	zassert_equal(RADIANT_SEARCH_ENOENT,
		      radiant_search_window(&g_s, radiant_radio_now() + 1000u, &g_w));

	end_of_test();
}

/* Scan mode's infinite search does not expire. */
ZTEST(radiant_search, test_scan_mode_has_no_timeout)
{
	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_SCAN, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	(void)pump(4u);
	radiant_search_tick(&g_s, radiant_radio_now() + 10u * RADIANT_SEARCH_TIMEOUT_DEFAULT_US);

	zassert_equal(0u, g_cap.n_timeout);
	zassert_true(radiant_search_is_searching(&g_s, 0u));

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/*
 * An event carrying the id of an operation that already ended. On real hardware
 * the frame was in the receiver's pipeline when abort() ran. It must be counted
 * and ignored - never acquired on, because the filter table it would be indexed
 * against belongs to a different set by now.
 */
ZTEST(radiant_search, test_late_event_never_acquires)
{
	struct fake_radio_air air;
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len;
	uint32_t dead_op;

	bring_up(false);
	zassert_ok(radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				    radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));

	zassert_equal(RADIANT_SEARCH_OK, win_open());
	dead_op = g_op;
	win_run();
	zassert_equal(0u, g_cap.n_acquired);

	len = fake_radio_build_ant_frame(frame, 0x0C03u, 0x0Bu, 5u, payload8);
	fake_radio_air_init(&air);
	air.t_sync = radiant_radio_now();
	memcpy(air.bytes, frame, len);
	air.len = len;
	zassert_ok(fake_radio_inject_late_rx(dead_op, &air, 3u));

	zassert_equal(0u, g_cap.n_acquired, "a late event must not acquire");
	zassert_equal(1u, radiant_search_get_stats(&g_s)->late_events);

	zassert_ok(radiant_search_end(&g_s, 0u));
	end_of_test();
}

/* ---------------------------------------------------------------------------
 * Argument handling
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_search, test_rejects_bad_arguments)
{
	struct radiant_search_cfg cfg;
	struct radiant_search_window w;

	radiant_search_cfg_default(&cfg);
	zassert_equal(RADIANT_SEARCH_EINVAL, radiant_search_init(NULL, &cfg, NULL, NULL));
	zassert_equal(RADIANT_SEARCH_EINVAL, radiant_search_init(&g_s, NULL, NULL, NULL));

	cfg.rf_index = (uint8_t)(RADIANT_RF_INDEX_MAX + 1u);
	zassert_equal(RADIANT_SEARCH_EINVAL, radiant_search_init(&g_s, &cfg, NULL, NULL));

	radiant_search_cfg_default(&cfg);
	cfg.dwell_us = 0u;
	zassert_equal(RADIANT_SEARCH_EINVAL, radiant_search_init(&g_s, &cfg, NULL, NULL));

	bring_up(false);
	zassert_equal(RADIANT_SEARCH_EINVAL,
		      radiant_search_begin(&g_s, 0u, RADIANT_SEARCH_MODE_ACQUIRE, NULL,
				       radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	zassert_equal(RADIANT_SEARCH_ENOSPC,
		      radiant_search_begin(&g_s, (uint8_t)RADIANT_SEARCH_MAX_CHANNELS,
				       RADIANT_SEARCH_MODE_ACQUIRE, &want_any,
				       radiant_radio_now(), RADIANT_SEARCH_TIMEOUT_NONE));
	zassert_equal(RADIANT_SEARCH_ENOENT, radiant_search_end(&g_s, 0u));
	zassert_equal(RADIANT_SEARCH_EINVAL,
		      radiant_search_window(&g_s, radiant_radio_now(), NULL));
	zassert_equal(RADIANT_SEARCH_ENOENT,
		      radiant_search_window(&g_s, radiant_radio_now() + 1000u, &w));

	/* Nothing armed, so nothing owns any operation. */
	zassert_false(radiant_search_owns_op(&g_s, 0u));
	zassert_false(radiant_search_owns_op(&g_s, 1u));

	end_of_test();
}

/* Every channel slot the project is sized for must actually be usable, because
 * a 32-channel dongle is the capability and an off-by-one here would cap it at
 * 31 silently. */
ZTEST(radiant_search, test_all_channel_slots_can_search)
{
	bring_up(false);

	for (uint8_t c = 0; c < (uint8_t)RADIANT_SEARCH_MAX_CHANNELS; c++) {
		zassert_ok(radiant_search_begin(&g_s, c, RADIANT_SEARCH_MODE_ACQUIRE,
					    &want_any, radiant_radio_now(),
					    RADIANT_SEARCH_TIMEOUT_NONE));
	}
	zassert_equal((uint8_t)RADIANT_SEARCH_MAX_CHANNELS, radiant_search_n_searching(&g_s));

	/* And they are all served by ONE window, which is the whole point. */
	zassert_equal(RADIANT_SEARCH_OK, win_open());
	zassert_equal(1u, fake_radio_stats()->arms_rx);
	win_run();

	for (uint8_t c = 0; c < (uint8_t)RADIANT_SEARCH_MAX_CHANNELS; c++) {
		zassert_ok(radiant_search_end(&g_s, c));
	}
	end_of_test();
}
