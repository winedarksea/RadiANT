/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work. Written against radiant_sched.h,
 * radiant_radio_hal.h, fake_radio.h and the measurements in
 * docs/spike-a-results.md and docs/spike-b-part2-results.md. Nothing here
 * derives from sdk-ant, libant.a, or any ANT+ profile document.
 *
 * Tests for the radio schedule. radiant_sched.c is pure logic over
 * radiant_radio_hal.h, so it's fully testable with no board. Guards against
 * two expensive mistakes:
 *
 *   MERGING THAT DOES NOT MERGE - a scheduler arming one window per channel
 *   still works but loses a packet on every window overlap, at a rate that
 *   looks like the ~0.4% collision floor the bench already characterised. The
 *   merge tests assert on fake_radio_arm_count(): overlapping channels must
 *   cost ONE arm call, and the arriving frame must attribute to the right
 *   channel/filter.
 *
 *   A HARDCODED EIGHT - caps.max_filters is 8 on nRF, 2 on EFR32/RAIL. Every
 *   merge test runs at both, and every test asserts the mock rejected no arm
 *   call, which is the only way "it hardcoded 8" is visible before an EFR32
 *   exists.
 *
 * Every test also checks: the radio ends idle, and fake_radio_viol_count() is
 * zero (nothing done in a callback that would deadlock a real radio ISR).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include <radiant/radiant_frame.h>
#include <radiant/radiant_sched.h>

#include "../fake_radio.h"

/* ---------------------------------------------------------------------------
 * Fixtures
 * ---------------------------------------------------------------------------
 */

/* Spike A's ground-truth sensor, plus 31 neighbours that differ only in the
 * device number - which is what a dongle tracking 32 sensors actually looks
 * like, and which puts a different byte in every filter. */
#define TEST_DEVNUM_BASE 0x3A00u
#define TEST_DEV_TYPE    0x0Bu
#define TEST_TRANS_TYPE  0x05u

/* The measured mean ANT+ slot period, 744 intervals over six runs
 * (docs/ant-radio-link.md). Used so the contention tests run at the rate the
 * bench actually sees rather than at a round number. */
#define TEST_PERIOD_US 249696u

/* A tracked window's half-width. The bench says a real master holds its slot to
 * well inside +/-25 us over minutes, so a few hundred microseconds is a guard
 * against our own drift and clock error, not against the master. */
#define TEST_WINDOW_US 400u

static struct radiant_rx_filter track_filter[RADIANT_SCHED_MAX_CHANNELS];
static struct radiant_rx_filter search_filter[RADIANT_SCHED_MAX_FILTERS];

static const uint8_t test_payload[8] = {
	0x10u, 0x00u, 0xFFu, 0xFFu, 0x00u, 0x00u, 0x00u, 0x00u
};

/* Ten bytes, which is exactly radiant_frame_format(TRACKING)->body_len. The mock
 * rejects a transmit whose body disagrees with its format's length rule, and it
 * is right to: a backend DMAs the bytes it was given. */
static const uint8_t test_tx_body[10] = {
	TEST_TRANS_TYPE, RADIANT_CTRL_BROADCAST,
	0x10u, 0x00u, 0xFFu, 0xFFu, 0x00u, 0x00u, 0x00u, 0x00u
};

#define TEST_LOG_MAX 128u

static struct {
	uint32_t n_rx;
	struct {
		uint8_t               ch;
		uint8_t               filter_index;
		enum radiant_radio_status status;
		radiant_time_t            t_sync;
	} rx[TEST_LOG_MAX];

	uint32_t n_tx;
	struct {
		uint8_t    ch;
		radiant_time_t t_sync;
	} tx[TEST_LOG_MAX];

	uint32_t n_done;
	struct {
		uint8_t             ch;
		enum radiant_sched_done why;
	} done[TEST_LOG_MAX];

	uint32_t n_armed;
	struct {
		uint8_t    ch;
		radiant_time_t t_open;
		radiant_time_t t_close;
	} armed[TEST_LOG_MAX];

	uint32_t ok_count[RADIANT_SCHED_MAX_CHANNELS];
	uint32_t missed_count[RADIANT_SCHED_MAX_CHANNELS];
	uint32_t aborted_count[RADIANT_SCHED_MAX_CHANNELS];
	uint32_t denied_count[RADIANT_SCHED_MAX_CHANNELS];

	/* Steady-state driver: re-post the next window from done(), which is
	 * what radiant_channel.c will do and the only way the round-robin fairness
	 * of the scheduler is observable at all. */
	bool       repost;
	radiant_time_t next_open[RADIANT_SCHED_MAX_CHANNELS];

	/* Which channels repost, when only some of them are tracked. Zero means
	 * all of them, so every test predating this field is unaffected. A
	 * mixed test needs it: a standing scan gets DONE_ABORTED every time a
	 * tracked window displaces it, and reposting THAT as a tracked window
	 * would quietly convert the scan into a bounded receive mid-test. */
	uint32_t   repost_mask;

	/* Turnaround driver: post a reply this far after a received frame's
	 * t_sync, from inside the receive callback. 0 disables it. */
	uint32_t reply_after_us;
	uint8_t  reply_ch;
} log;

static void rx_req_init(struct radiant_sched_rx *r, const struct radiant_pkt_format *fmt,
			const struct radiant_rx_filter *filters, uint8_t n,
			radiant_time_t t_open, radiant_time_t t_close)
{
	memset(r, 0, sizeof(*r));
	r->fmt = fmt;
	r->rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	r->filters = filters;
	r->n_filters = n;
	r->t_open = t_open;
	r->t_close = t_close;
}

/*
 * A well-formed transmit request for a channel. The address deliberately
 * comes from track_filter[ch] - a channel transmits on the address it listens
 * on - so "the scheduler passed the right address" is checkable at all.
 */
static void tx_req_init(struct radiant_sched_tx *t, uint8_t ch, radiant_time_t t_sync_at)
{
	memset(t, 0, sizeof(*t));
	t->fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	t->rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	t->power.dbm = 0;
	memcpy(t->addr, track_filter[ch].addr, sizeof(t->addr));
	t->addr_len = track_filter[ch].addr_len;
	t->body = test_tx_body;
	t->body_len = (uint8_t)sizeof(test_tx_body);
	t->t_sync_at = t_sync_at;
}

static void post_track(uint8_t ch, radiant_time_t t_open, radiant_time_t t_close)
{
	struct radiant_sched_rx r;

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING),
		    &track_filter[ch], 1u, t_open, t_close);
	zassert_ok(radiant_sched_request_rx(ch, &r), "channel %u refused", ch);
}

static void on_rx(uint8_t ch, uint8_t filter_index,
		  const struct radiant_rx_event *evt, void *user)
{
	ARG_UNUSED(user);

	if (log.n_rx < TEST_LOG_MAX) {
		log.rx[log.n_rx].ch = ch;
		log.rx[log.n_rx].filter_index = filter_index;
		log.rx[log.n_rx].status = evt->status;
		log.rx[log.n_rx].t_sync = evt->t_sync;
	}
	log.n_rx++;

	if (log.reply_after_us != 0u && evt->status == RADIANT_RADIO_STATUS_OK) {
		struct radiant_sched_tx t;

		tx_req_init(&t, log.reply_ch,
			    evt->t_sync + (radiant_time_t)log.reply_after_us);
		(void)radiant_sched_request_tx(log.reply_ch, &t);
		log.reply_after_us = 0u; /* one reply per test */
	}
}

static void on_tx(uint8_t ch, const struct radiant_tx_event *evt, void *user)
{
	ARG_UNUSED(user);

	if (log.n_tx < TEST_LOG_MAX) {
		log.tx[log.n_tx].ch = ch;
		log.tx[log.n_tx].t_sync = evt->t_sync;
	}
	log.n_tx++;
}

static void on_done(uint8_t ch, enum radiant_sched_done why, void *user)
{
	ARG_UNUSED(user);

	if (log.n_done < TEST_LOG_MAX) {
		log.done[log.n_done].ch = ch;
		log.done[log.n_done].why = why;
	}
	log.n_done++;

	switch (why) {
	case RADIANT_SCHED_DONE_OK:
		log.ok_count[ch]++;
		break;
	case RADIANT_SCHED_DONE_MISSED:
		log.missed_count[ch]++;
		break;
	case RADIANT_SCHED_DONE_ABORTED:
		log.aborted_count[ch]++;
		break;
	case RADIANT_SCHED_DONE_DENIED:
		log.denied_count[ch]++;
		break;
	default:
		break;
	}

	if (log.repost &&
	    (log.repost_mask == 0u ||
	     (log.repost_mask & ((uint32_t)1u << ch)) != 0u)) {
		log.next_open[ch] += (radiant_time_t)TEST_PERIOD_US;
		post_track(ch, log.next_open[ch],
			   log.next_open[ch] + (radiant_time_t)TEST_WINDOW_US);
	}
}

static void on_armed(uint8_t ch, radiant_time_t t_open, radiant_time_t t_close,
		     void *user)
{
	ARG_UNUSED(user);

	if (log.n_armed < TEST_LOG_MAX) {
		log.armed[log.n_armed].ch = ch;
		log.armed[log.n_armed].t_open = t_open;
		log.armed[log.n_armed].t_close = t_close;
	}
	log.n_armed++;
}

static const struct radiant_sched_cbs test_cbs = {
	.rx = on_rx,
	.tx = on_tx,
	.armed = on_armed,
	.done = on_done,
};

/* Build filters from the frame layer so filter and on-air frame cannot
 * disagree about byte order by construction. */
static void build_filters(void)
{
	uint32_t i;

	for (i = 0u; i < RADIANT_SCHED_MAX_CHANNELS; i++) {
		struct radiant_channel_id id = {
			.device_number = (uint16_t)(TEST_DEVNUM_BASE + i),
			.device_type = TEST_DEV_TYPE,
			.trans_type = TEST_TRANS_TYPE,
		};

		zassert_equal(5, radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING,
						radiant_net_addr_ant_plus, &id,
						track_filter[i].addr,
						sizeof(track_filter[i].addr)),
			      "tracking address length changed");
		track_filter[i].addr_len = 5u;
	}

	for (i = 0u; i < RADIANT_SCHED_MAX_FILTERS; i++) {
		struct radiant_channel_id id = {
			.device_number = (uint16_t)(TEST_DEVNUM_BASE + i),
			.device_type = TEST_DEV_TYPE,
			.trans_type = TEST_TRANS_TYPE,
		};

		zassert_equal(3, radiant_frame_addr(RADIANT_FRAME_CFG_SEARCH,
						radiant_net_addr_ant_plus, &id,
						search_filter[i].addr,
						sizeof(search_filter[i].addr)),
			      "search address length changed");
		search_filter[i].addr_len = 3u;
	}
}

/* Put one good ANT+ frame from channel ch's device on the air at t_sync. The
 * same 15 bytes serve both the 5-byte tracking and 3-byte search windows -
 * the split belongs to the receiver, so the mock stores a byte stream. */
static void air_frame_for(uint8_t ch, radiant_time_t t_sync)
{
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t n;

	n = fake_radio_build_ant_frame(frame, (uint16_t)(TEST_DEVNUM_BASE + ch),
				       TEST_DEV_TYPE, TEST_TRANS_TYPE,
				       test_payload);
	zassert_equal(15u, n, "the ANT+ frame is not 15 bytes");
	zassert_ok(fake_radio_air_frame(t_sync, frame, n));
}

static void bring_up(void)
{
	zassert_ok(radiant_sched_init(&test_cbs, NULL));
	zassert_ok(radiant_radio_init(radiant_sched_radio_cbs(), NULL));
	zassert_ok(radiant_radio_enable());
}

/*
 * The end of every test; these assertions matter more than most above them.
 * rc_etime == 0 states "late arming is rejected, never silently run late" -
 * the scheduler subtracts caps.min_arm_lead_us itself. rc_einval/rc_enotsup
 * == 0 is the hardcoded-eight detector: too many filters is RADIANT_RADIO_EINVAL.
 */
static void finish(void)
{
	const struct fake_radio_stats *st = fake_radio_stats();
	uint8_t                        ch;

	log.repost = false;
	log.repost_mask = 0u;
	log.reply_after_us = 0u;
	for (ch = 0u; ch < (uint8_t)RADIANT_SCHED_MAX_CHANNELS; ch++) {
		zassert_ok(radiant_sched_cancel(ch));
	}

	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
	zassert_equal(0u, fake_radio_viol_count(), "%s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
	zassert_equal(0u, st->rc_etime,
		      "the scheduler asked the backend for an instant it had to refuse");
	zassert_equal(0u, st->rc_einval,
		      "the scheduler built a malformed request (too many filters?)");
	zassert_equal(0u, st->rc_enotsup,
		      "the scheduler asked for something this backend cannot do");
	zassert_equal(0u, st->rc_estate, "an arm call was made with the radio down");
}

/* Every accepted window must be finite and well ordered - RADIANT_TIME_NEVER is
 * rejected as a window edge by the HAL. */
static void assert_every_window_bounded(void)
{
	uint32_t i;

	for (i = 0u; i < fake_radio_arm_count(); i++) {
		const struct fake_radio_arm *a = fake_radio_arm(i);

		if (a == NULL || a->kind != FAKE_RADIO_ARM_RX ||
		    a->rc != RADIANT_RADIO_OK_RC) {
			continue;
		}
		zassert_not_equal(RADIANT_TIME_NEVER, a->t_close,
				  "arm %u carried an unbounded window", i);
		zassert_true(a->t_close >= a->t_open,
			     "arm %u closed before it opened", i);
	}
}

static void sched_before(void *f)
{
	ARG_UNUSED(f);
	fake_radio_reset();
	memset(&log, 0, sizeof(log));
	build_filters();
}

ZTEST_SUITE(radiant_sched, NULL, NULL, sched_before, NULL, NULL);

/* ---------------------------------------------------------------------------
 * Arming at an absolute instant
 * ---------------------------------------------------------------------------
 */

/* The window goes on the air at the requested microsecond, at an instant far
 * enough above 2^32 that a uint32_t anywhere in the path would truncate it.
 * The mock's clock origin sits one second below that boundary so every suite
 * crosses it; this test crosses it deliberately. */
ZTEST(radiant_sched, test_a_window_is_armed_at_the_requested_absolute_microsecond)
{
	const struct fake_radio_arm *a;
	radiant_time_t                   t0;
	radiant_time_t                   open;
	radiant_time_t                   close;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 2000000u; /* two seconds: comfortably past 2^32 us */
	close = open + TEST_WINDOW_US;

	post_track(0u, open, close);
	zassert_equal(0u, fake_radio_arm_count(),
		      "posting a request must not arm anything by itself");

	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, fake_radio_arm_count());

	a = fake_radio_arm(0u);
	zassert_not_null(a);
	zassert_equal(FAKE_RADIO_ARM_RX, a->kind);
	zassert_equal(RADIANT_RADIO_OK_RC, a->rc);
	zassert_true(a->t_open > (radiant_time_t)0xFFFFFFFFu,
		     "the test did not actually cross the 32-bit boundary");
	zassert_equal(open, a->t_open, "t_open moved");
	zassert_equal(close, a->t_close, "t_close moved");
	zassert_equal(1u, a->n_filters);
	zassert_equal(RADIANT_RF_INDEX_ANT_PLUS, a->rf_index);
	zassert_mem_equal(a->filters[0].addr, track_filter[0].addr, 5u,
			  "the filter was not passed through unchanged");

	fake_radio_advance_to(close + 1u);
	zassert_equal(1u, log.n_done);
	zassert_equal(RADIANT_SCHED_DONE_OK, log.done[0].why);
	zassert_equal(0u, log.done[0].ch);

	assert_every_window_bounded();
	finish();
}

/* ---------------------------------------------------------------------------
 * Merging
 * ---------------------------------------------------------------------------
 */

/*
 * THE CENTRAL TEST: overlapping tracked windows cost ONE hardware window, and
 * the arriving frame still attributes to the right channel. An unmerged
 * scheduler passes every other test here and fails only this one, showing up
 * on the bench as a fraction of a percent of extra loss, indistinguishable
 * from the collision floor.
 *
 * TWO CHANNELS, NOT THREE - hardware talking. On the 5-byte tracking format
 * the device number lives inside the address BASE, and the part has one
 * BASE0 and one BASE1, so a tracking window expresses two device numbers
 * (caps.max_addr_groups). The third channel gets its own window right after,
 * which the tail of this test checks.
 */
ZTEST(radiant_sched, test_overlapping_windows_collapse_into_one_hardware_window)
{
	const struct fake_radio_arm  *a;
	const struct radiant_sched_stats *st;
	radiant_time_t                    t0;
	radiant_time_t                    open;
	radiant_time_t                    close;
	uint8_t                       ch;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;
	close = open + TEST_WINDOW_US;

	/* Deliberately not identical: masters whose slots have drifted apart by
	 * a few tens of microseconds, which is what overlap looks like in the
	 * field. The third is posted too, so that the group cap is exercised
	 * rather than merely respected by a test that never reaches it. */
	post_track(0u, open, close);
	post_track(1u, open + 40u, close + 40u);
	post_track(2u, open + 90u, close + 90u);

	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count(),
		      "two overlapping channels cost %u windows, not one",
		      fake_radio_arm_count());

	a = fake_radio_arm(0u);
	zassert_not_null(a);
	zassert_equal(RADIANT_RADIO_OK_RC, a->rc);
	zassert_equal(2u, a->n_filters,
		      "the window carried %u filters; the nRF matches two "
		      "device numbers per tracking window, not three",
		      a->n_filters);
	/* The union of the two, not the intersection: truncating to the overlap
	 * would lose coverage for every member. */
	zassert_equal(open, a->t_open);
	zassert_equal(close + 40u, a->t_close);
	/* STOP_ON_FIRST is forbidden on a merged window and nobody asked for
	 * it here either. */
	zassert_equal(0u, a->flags & RADIANT_RX_STOP_ON_FIRST);
	for (ch = 0u; ch < 2u; ch++) {
		zassert_mem_equal(a->filters[ch].addr, track_filter[ch].addr, 5u,
				  "filter %u is not channel %u's", ch, ch);
	}

	/* Channel 1's master transmits inside the shared window. */
	air_frame_for(1u, open + 100u);
	fake_radio_advance_to(a->t_close + 1u);

	zassert_equal(1u, log.n_rx);
	zassert_equal(1u, log.rx[0].ch, "the frame was attributed to the wrong channel");
	zassert_equal(0u, log.rx[0].filter_index,
		      "a one-filter channel must always see its own index 0");
	zassert_equal(RADIANT_RADIO_STATUS_OK, log.rx[0].status);
	zassert_equal(open + 100u, log.rx[0].t_sync);

	/* Both members are told the window ended, exactly once each. */
	for (ch = 0u; ch < 2u; ch++) {
		zassert_equal(1u, log.ok_count[ch], "channel %u", ch);
	}

	st = radiant_sched_stats_get();
	zassert_equal(1u, st->windows_armed);
	zassert_equal(2u, st->channels_windowed);
	zassert_equal(1u, st->windows_merged);
	zassert_equal(2u, st->filters_armed);

	/* AND THE THIRD CHANNEL IS NOT LOST. It was excluded by the group cap,
	 * not dropped: it gets a window of its own as soon as the radio is
	 * free. This is the assertion that separates "the scheduler respects
	 * the hardware" from "the scheduler quietly serves fewer sensors". */
	zassert_true(log.ok_count[2] + log.missed_count[2] >= 1u,
		     "channel 2 was neither served nor told");

	assert_every_window_bounded();
	finish();
}

/* Windows that do not overlap are not merged. A merge rule that ignored the
 * times would pass the test above and silently widen every window, costing
 * receive current and, once the span cap bites, real coverage. */
ZTEST(radiant_sched, test_disjoint_windows_are_not_merged)
{
	radiant_time_t t0;
	radiant_time_t open;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;

	post_track(0u, open, open + TEST_WINDOW_US);
	/* A full slot later: the same sensor rate, half a period out of phase. */
	post_track(1u, open + 120000u, open + 120000u + TEST_WINDOW_US);
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(1u, fake_radio_arm(0u)->n_filters,
		      "a window 120 ms away was merged into this one");
	zassert_equal(open + TEST_WINDOW_US, fake_radio_arm(0u)->t_close);

	/* The second window is armed on its own, from the first one's terminal
	 * callback. */
	fake_radio_advance_to(open + 120000u + TEST_WINDOW_US + 1u);
	zassert_equal(2u, fake_radio_arm_count());
	zassert_equal(1u, fake_radio_arm(1u)->n_filters);
	zassert_equal(open + 120000u, fake_radio_arm(1u)->t_open);
	zassert_equal(1u, log.ok_count[0]);
	zassert_equal(1u, log.ok_count[1]);
	zassert_equal(0u, radiant_sched_stats_get()->windows_merged);

	assert_every_window_bounded();
	finish();
}

/*
 * ADAPTIVE FREQUENCY'S HALF OF THE MERGE RULE (RF-7) - the price, not the
 * feature. ADR 0005 axis 5 buys frequency escape by accepting that a channel
 * leaving RF 57 leaves the merged window with it: "costs its own RX window,
 * same as a second-network channel would; only nodes that opted in pay it."
 * That cost is only paid if the scheduler REFUSES to merge across
 * frequencies - a scheduler that merged anyway would silently drop every
 * packet on the frequency it didn't arm.
 *
 * Three overlapping channels: two on RF 57, one on RF 26. The two on 57 still
 * merge; the third gets its own window.
 */
ZTEST(radiant_sched, test_an_off_57_window_never_joins_the_merged_one)
{
	const struct fake_radio_arm  *a;
	struct radiant_sched_rx       r;
	radiant_time_t                t0;
	radiant_time_t                open;
	radiant_time_t                close;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;
	close = open + TEST_WINDOW_US;

	post_track(0u, open, close);
	post_track(1u, open + 40u, close + 40u);

	/* Same overlap, on 2426 MHz, deliberately much LONGER: it overlaps the
	 * pair above (so a frequency-blind scheduler would fold it in) and
	 * outlives them, so "costs its own RX window" is observed, not just
	 * asserted. */
	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING),
		    &track_filter[2], 1u, open + 80u, close + 40000u);
	r.rf_index = 26u;
	zassert_ok(radiant_sched_request_rx(2u, &r));

	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count(),
		      "the first commit armed %u windows",
		      fake_radio_arm_count());

	a = fake_radio_arm(0u);
	zassert_not_null(a);
	zassert_equal(RADIANT_RF_INDEX_ANT_PLUS, a->rf_index);
	zassert_equal(2u, a->n_filters,
		      "the RF 26 channel was merged into the RF 57 window: it "
		      "carried %u filters", a->n_filters);
	zassert_equal(close + 40u, a->t_close,
		      "the window was widened to cover a channel on another "
		      "frequency");
	zassert_equal(1u, radiant_sched_stats_get()->windows_merged);

	/* And the off-57 channel is not lost - it is served next, on its own
	 * frequency, which is exactly what "costs its own RX window" means. */
	fake_radio_advance_to(close + 40000u + 1u);
	zassert_true(fake_radio_arm_count() >= 2u,
		     "the off-57 channel was never armed");
	a = fake_radio_arm(1u);
	zassert_not_null(a);
	zassert_equal(26u, a->rf_index,
		      "the second window was armed on RF %u", a->rf_index);
	zassert_equal(1u, a->n_filters);
	zassert_equal(1u, log.ok_count[2],
		      "the off-57 channel was never given its own window");

	/* Two hardware windows for three channels: the two that share a
	 * frequency cost one between them and the third costs one on its own.
	 * That is the whole of axis 5's price, in one number. */
	zassert_equal(2u, radiant_sched_stats_get()->windows_armed);

	assert_every_window_bounded();
	finish();
}

/*
 * THE MERGE IS BOUNDED BY caps.max_addr_groups, NOT caps.max_filters, ON THE
 * TRACKING FORMAT - confusing the two is the defect this test pins.
 *
 * Twelve tracked channels want the same window; the nRF preset serves TWO,
 * because the eight logical addresses are one BASE0+AP0 and seven
 * BASE1+AP1..AP7, and the tracking address's device number lives inside the
 * BASE - eight filters is eight addresses, not eight device numbers.
 *
 * This test previously asserted eight and passed, because the mock modelled
 * max_filters with no base at all; the real backend would have refused every
 * arm with RADIANT_RADIO_ENOTSUP, charged to whichever channel happened to lead.
 *
 * The ten that do not fit are reported RADIANT_SCHED_DONE_MISSED, not dropped -
 * the honest answer for a capacity limit rather than unexplained loss.
 */
ZTEST(radiant_sched, test_merging_is_bounded_by_max_addr_groups_on_tracking)
{
	radiant_time_t t0;
	radiant_time_t open;
	uint8_t    ch;

	bring_up(); /* fake_radio_reset() leaves the nRF preset */
	zassert_equal(8u, radiant_radio_caps_get()->max_filters);
	zassert_equal(2u, radiant_radio_caps_get()->max_addr_groups,
		      "the nRF preset must model the two-BASE constraint, or "
		      "this suite certifies a window no part can arm");

	t0 = radiant_radio_now();
	open = t0 + 100000u;
	for (ch = 0u; ch < 12u; ch++) {
		post_track(ch, open, open + TEST_WINDOW_US);
	}
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(2u, fake_radio_arm(0u)->n_filters,
		      "the window carried %u filters; a tracking window on this "
		      "part expresses two device numbers",
		      fake_radio_arm(0u)->n_filters);

	fake_radio_advance_to(open + TEST_WINDOW_US + 1u);
	for (ch = 0u; ch < 2u; ch++) {
		zassert_equal(1u, log.ok_count[ch], "channel %u was not heard", ch);
	}
	for (ch = 2u; ch < 12u; ch++) {
		zassert_equal(1u, log.missed_count[ch],
			      "channel %u was neither served nor told", ch);
	}
	zassert_equal(10u, radiant_sched_stats_get()->missed);

	assert_every_window_bounded();
	finish();
}

/*
 * THE OTHER HALF: the fix costs nothing where it matters most. On the 3-byte
 * SEARCH format the address is [A6 C5 devnum_lo] - the group [A6 C5] is
 * shared by every filter, and the prefix is the device-number byte - so eight
 * search filters ride one window and the group cap never fires. Merging got
 * correct on the format the hardware constrains and stayed at eight on the
 * one the sweep's rate depends on.
 */
ZTEST(radiant_sched, test_search_filters_share_one_address_group)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	radiant_time_t          open;

	bring_up();
	zassert_equal(2u, radiant_radio_caps_get()->max_addr_groups);

	t0 = radiant_radio_now();
	open = t0 + 100000u;

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH),
		    search_filter, 8u, open, open + TEST_WINDOW_US);
	zassert_ok(radiant_sched_request_rx(0u, &r));
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_arm(0u)->rc,
		      "eight search filters were refused; they share the "
		      "[A6 C5] group and must all fit");
	zassert_equal(8u, fake_radio_arm(0u)->n_filters,
		      "the sweep lost filters to a constraint that does not "
		      "apply to a 3-byte address");

	fake_radio_advance_to(open + TEST_WINDOW_US + 1u);
	assert_every_window_bounded();
	finish();
}

/* Same scenario on a backend with two sync words. The only test that can
 * catch a hardcoded eight two ways: n_filters == 2, and finish() asserts the
 * mock rejected nothing (eight filters at max_filters == 2 is EINVAL). */
ZTEST(radiant_sched, test_merging_is_bounded_by_max_filters_at_two)
{
	radiant_time_t t0;
	radiant_time_t open;
	uint8_t    ch;

	fake_radio_caps_preset_rail();
	bring_up();
	zassert_equal(2u, radiant_radio_caps_get()->max_filters);

	t0 = radiant_radio_now();
	open = t0 + 100000u;
	for (ch = 0u; ch < 12u; ch++) {
		post_track(ch, open, open + TEST_WINDOW_US);
	}
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(2u, fake_radio_arm(0u)->n_filters,
		      "the window carried %u filters on a part that has two",
		      fake_radio_arm(0u)->n_filters);
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_arm(0u)->rc);

	fake_radio_advance_to(open + TEST_WINDOW_US + 1u);
	zassert_equal(1u, log.ok_count[0]);
	zassert_equal(1u, log.ok_count[1]);
	for (ch = 2u; ch < 12u; ch++) {
		zassert_equal(1u, log.missed_count[ch], "channel %u", ch);
	}

	assert_every_window_bounded();
	finish();
}

/* A request for more addresses than the backend has matchers is reported once
 * and consumed, not retried forever - the scheduler must not loop on a
 * request the backend will refuse every time, a livelock inside a radio ISR
 * that fake_radio's advance budget exists to catch. */
ZTEST(radiant_sched, test_a_request_for_more_filters_than_exist_is_reported_once)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;

	fake_radio_caps_preset_rail();
	bring_up();

	t0 = radiant_radio_now();
	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), search_filter,
		    8u, t0 + 100000u, t0 + 100000u + 250000u);
	zassert_ok(radiant_sched_request_rx(4u, &r));
	zassert_ok(radiant_sched_tick());

	zassert_equal(0u, fake_radio_arm_count(),
		      "the scheduler offered the backend a window it must refuse");
	zassert_equal(1u, log.n_done);
	zassert_equal(RADIANT_SCHED_DONE_FAILED, log.done[0].why);
	zassert_equal(4u, log.done[0].ch);
	zassert_false(radiant_sched_pending(4u), "the failed request was not consumed");

	/* Two filters is what this part can do, and the same code path arms it. */
	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), search_filter,
		    2u, t0 + 100000u, t0 + 100000u + 250000u);
	zassert_ok(radiant_sched_request_rx(4u, &r));
	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(2u, fake_radio_arm(0u)->n_filters);

	assert_every_window_bounded();
	finish();
}

/* A merged window's filter index is translated back into "channel c, filter i
 * of c's own array". radiant_search.c's devnum_lo recovery rests on this: with
 * a 3-byte search address the matched byte never reaches RAM, so a scheduler
 * passing the global index straight through would give every channel but the
 * first the wrong device number. */
ZTEST(radiant_sched, test_filter_index_is_translated_to_the_channels_own_filter)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	radiant_time_t          open;
	radiant_time_t          close;

	bring_up(); /* nRF: eight filters, so 4 + 2 fits in one window */
	t0 = radiant_radio_now();
	open = t0 + 100000u;
	close = open + 250000u;

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), &search_filter[0],
		    4u, open, close);
	zassert_ok(radiant_sched_request_rx(0u, &r));
	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), &search_filter[4],
		    2u, open, close);
	zassert_ok(radiant_sched_request_rx(1u, &r));
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(6u, fake_radio_arm(0u)->n_filters);

	/* Device number base+2: the third of channel 0's four filters, and the
	 * third of the merged window's six. */
	air_frame_for(2u, open + 500u);
	/* Device number base+5: the second of channel 1's two filters, and the
	 * sixth of the merged window's six. */
	air_frame_for(5u, open + 900u);
	fake_radio_advance_to(close + 1u);

	zassert_equal(2u, log.n_rx);
	zassert_equal(0u, log.rx[0].ch);
	zassert_equal(2u, log.rx[0].filter_index,
		      "channel 0 was given the merged window's index, not its own");
	zassert_equal(1u, log.rx[1].ch);
	zassert_equal(1u, log.rx[1].filter_index,
		      "channel 1 was given the merged window's index, not its own");

	assert_every_window_bounded();
	finish();
}

/* A channel that opens after its neighbours joins the merge on the next
 * commit rather than a channel period later - tearing down a window that has
 * not reached preamble costs nothing. Without this, merging would only ever
 * happen in the steady state a test arranges and the bench does not. */
ZTEST(radiant_sched, test_a_late_channel_joins_a_window_that_has_not_opened_yet)
{
	radiant_time_t t0;
	radiant_time_t open;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;

	post_track(0u, open, open + TEST_WINDOW_US);
	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(1u, fake_radio_arm(0u)->n_filters);

	post_track(1u, open, open + TEST_WINDOW_US);
	zassert_ok(radiant_sched_tick());
	zassert_equal(2u, fake_radio_arm_count(), "the window was not rebuilt");
	zassert_equal(2u, fake_radio_arm(1u)->n_filters,
		      "the late channel did not join the merge");
	/* A rebuild before the window opened costs nobody anything, so channel
	 * 0 must not have been told its window ended. */
	zassert_equal(1u, radiant_sched_stats_get()->replans);
	zassert_equal(0u, radiant_sched_stats_get()->preempted);
	zassert_equal(0u, log.n_done, "a free rebuild was reported as an ending");

	/* Rebuilding is bounded: with nothing new posted, ticking again must
	 * not tear the window down and put it back a second time. */
	zassert_ok(radiant_sched_tick());
	zassert_ok(radiant_sched_tick());
	zassert_equal(2u, fake_radio_arm_count(),
		      "the scheduler is re-planning on every tick");

	fake_radio_advance_to(open + TEST_WINDOW_US + 1u);
	zassert_equal(1u, log.ok_count[0]);
	zassert_equal(1u, log.ok_count[1]);

	assert_every_window_bounded();
	finish();
}

/* ---------------------------------------------------------------------------
 * The merge reach - the arm-lead exclusion radius
 * ---------------------------------------------------------------------------
 */

/*
 * A LOCKED TRACKED WINDOW, which is a different shape from TEST_WINDOW_US and
 * the difference is the whole subject of this section. Once a channel is
 * tracking, radiant_channel_guard_us() is 100 us and its window is the
 * predicted slot instant plus or minus that - 200 us wide. Two such windows
 * therefore OVERLAP only while their centres are less than 2 x guard apart,
 * and "overlap" is exactly what the merge rule used to require.
 */
#define TEST_TRACK_GUARD_US 100u

/*
 * THE ARMING LEAD THE BAND IS MEASURED AT, and it has to be set rather than
 * taken from the preset. fake_radio's nRF preset carries min_arm_lead_us = 200,
 * which is exactly 2 x the tracked guard, so on the preset alone the bad band
 * is empty and none of the three tests below could tell the old rule from the
 * new one. 328 is not a round number either: it is radiant_radio_nrf.c's
 * ARM_LEAD_US on nRF52840 with the coded PHY off (ARM_SETUP 240 + ramp 40 +
 * preamble/address 48), the largest lead any part this backend builds for
 * advertises and therefore the widest exclusion radius the scheduler has to
 * cope with. nRF54L15's 168 is NARROWER than 2 x guard, so the band is empty
 * there and the pair merges by plain overlap - which is worth knowing but is
 * not a test.
 *
 * Not restored afterwards: sched_before() calls fake_radio_reset(), which puts
 * the whole caps table back to the preset for the next test.
 */
#define TEST_NRF52840_ARM_LEAD_US 328u

static void post_tracked_slot(uint8_t ch, radiant_time_t centre)
{
	post_track(ch, centre - (radiant_time_t)TEST_TRACK_GUARD_US,
		   centre + (radiant_time_t)TEST_TRACK_GUARD_US);
}

/*
 * TWO TRACKED SLOTS 300 US APART SHARE ONE WINDOW.
 *
 * 300 us sits in the middle of the band that used to be unservable. It is more
 * than 2 x the 100 us tracked guard, so the two windows do not overlap and the
 * old rule 3 - "every member must overlap the LEADER'S OWN window" - refused to
 * merge them; and it is less than caps.min_arm_lead_us, so the second window
 * could not be armed in sequence after the first either, because by the time
 * the first window's terminal event arrives the second is already inside the
 * backend's arming lead. The later channel was reported DONE_MISSED,
 * deterministically, every period, until the two masters' crystals drifted them
 * apart: roughly 1% of slots per channel at eight sensors and exactly zero at
 * one, which is why every loss figure this project has recorded missed it.
 *
 * The discriminating assertion is ONE arm carrying TWO filters. Merging is the
 * only way to hear both inside the exclusion radius, so a scheduler that has
 * not had the reach added cannot produce that count by any other route.
 */
ZTEST(radiant_sched, test_two_tracked_slots_inside_the_arming_lead_share_one_window)
{
	const struct fake_radio_arm *a;
	radiant_time_t               t0;
	radiant_time_t               c0;
	radiant_time_t               c1;
	uint16_t                     lead;

	bring_up();
	fake_radio_caps_mut()->min_arm_lead_us = TEST_NRF52840_ARM_LEAD_US;
	lead = radiant_radio_caps_get()->min_arm_lead_us;
	zassert_true(lead > 2u * TEST_TRACK_GUARD_US,
		     "this test needs an arming lead wider than two tracked "
		     "guards, or there is no bad band to close: lead %u", lead);
	zassert_true(300u > 2u * TEST_TRACK_GUARD_US && 300u < lead,
		     "300 us is no longer inside the bad band on this preset "
		     "(guard %u, lead %u) - pick a delta that is",
		     (unsigned int)TEST_TRACK_GUARD_US, lead);

	t0 = radiant_radio_now();
	c0 = t0 + 100000u;
	c1 = c0 + 300u;

	post_tracked_slot(0u, c0);
	post_tracked_slot(1u, c1);
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count(),
		      "two tracked slots 300 us apart cost %u windows",
		      fake_radio_arm_count());
	a = fake_radio_arm(0u);
	zassert_not_null(a);
	zassert_equal(RADIANT_RADIO_OK_RC, a->rc);
	zassert_equal(2u, a->n_filters,
		      "the window carried %u filters: the second slot was left "
		      "outside the merge reach", a->n_filters);
	/* The union covers the dead gap between them, which is what the reach
	 * costs: up to arm_lead of extra receive per merged pair per period. */
	zassert_equal(c0 - (radiant_time_t)TEST_TRACK_GUARD_US, a->t_open);
	zassert_equal(c1 + (radiant_time_t)TEST_TRACK_GUARD_US, a->t_close);

	/* The later master transmits at its own slot instant, in the part of
	 * the window that only exists because of the merge. */
	air_frame_for(1u, c1);
	fake_radio_advance_to(a->t_close + 1u);

	zassert_equal(1u, log.n_rx);
	zassert_equal(1u, log.rx[0].ch,
		      "the frame in the merged gap went to the wrong channel");
	zassert_equal(1u, log.ok_count[0]);
	zassert_equal(1u, log.ok_count[1]);
	zassert_equal(0u, radiant_sched_stats_get()->missed,
		      "a tracked slot inside the arming lead was still dropped");
	zassert_equal(1u, radiant_sched_stats_get()->windows_merged);

	assert_every_window_bounded();
	finish();
}

/*
 * THE BAD-BAND SWEEP - the test that would have caught the original defect.
 *
 * Step the separation between two tracked slots across the whole band and
 * assert nothing is ever dropped. A single-point test at one delta is what was
 * missing: the pair-merge tests above this section all sit at delta < 2 x
 * guard, where the windows genuinely overlap, so they passed throughout.
 *
 * THE GEOMETRY, stated so the boundary is a derivation and not a magic number.
 * Write G for the tracked guard (100 us) and L for caps.min_arm_lead_us. The
 * leader's window is [c, c+2G] and the member's is [c+d, c+d+2G].
 *
 *   d <= 2G          the windows overlap; merged under the old rule too.
 *   2G < d <= 2G+L   no overlap, but the member lies within the leader's own
 *                    window extended by the merge reach L, so rule 3 takes it.
 *                    This is the band that used to be dead - under the old rule
 *                    it could neither merge (no overlap) nor be armed in
 *                    sequence, because the second window opens less than L
 *                    after the first one's terminal event.
 *   d > 2G+L         too far to merge, and it no longer has to be: the gap
 *                    between the leader's close and the member's open is
 *                    d - 2G > L, which is exactly enough to arm a second
 *                    window in sequence.
 *
 * The two conditions meet at d = 2G+L with no gap between them, which is the
 * property that makes "missed == 0 at every delta" true rather than merely
 * true-at-the-sampled-points. The step is 25 us: fine enough to land on both
 * sides of every boundary above and to put several samples inside the dead
 * band, without turning a boundary test into a loop nobody reads the output
 * of. At the lead this test sets (TEST_NRF52840_ARM_LEAD_US = 328) the
 * boundaries are 2G = 200 and 2G+L = 528, so the formerly dead band is
 * 200-528 us and the sweep's 0-600 us range covers both sides of both.
 *
 * merge_max is computed from the LIVE caps inside the loop rather than from
 * those numbers, so the assertions follow the lead if the preset or the
 * constant ever moves; the figures above are the worked example, not the test.
 */
ZTEST(radiant_sched, test_no_separation_between_two_tracked_slots_drops_a_slot)
{
	uint16_t lead;
	uint32_t delta;

	bring_up();
	fake_radio_caps_mut()->min_arm_lead_us = TEST_NRF52840_ARM_LEAD_US;
	lead = radiant_radio_caps_get()->min_arm_lead_us;

	for (delta = 0u; delta <= 600u; delta += 25u) {
		const uint32_t merge_max = (2u * TEST_TRACK_GUARD_US) + lead;
		radiant_time_t base = radiant_radio_now() + 100000u;
		uint32_t       arms_before = fake_radio_arm_count();
		uint32_t       missed_before = radiant_sched_stats_get()->missed;
		uint32_t       ok0 = log.ok_count[0];
		uint32_t       ok1 = log.ok_count[1];
		uint32_t       arms;

		post_tracked_slot(0u, base);
		post_tracked_slot(1u, base + (radiant_time_t)delta);
		zassert_ok(radiant_sched_tick());

		fake_radio_advance_to(base + (radiant_time_t)delta +
				      (radiant_time_t)TEST_TRACK_GUARD_US + 1u);

		zassert_equal(missed_before, radiant_sched_stats_get()->missed,
			      "delta %u us: a tracked slot was dropped. Inside "
			      "%u us the pair must merge, beyond it there is "
			      "room to arm a second window in sequence, so no "
			      "separation may cost a slot",
			      delta, merge_max);
		zassert_equal(ok0 + 1u, log.ok_count[0], "delta %u us: channel 0",
			      delta);
		zassert_equal(ok1 + 1u, log.ok_count[1], "delta %u us: channel 1",
			      delta);

		arms = fake_radio_arm_count() - arms_before;
		if (delta <= merge_max) {
			zassert_equal(1u, arms,
				      "delta %u us: %u windows for a pair the "
				      "merge reach covers", delta, arms);
			zassert_equal(2u,
				      fake_radio_arm(arms_before)->n_filters,
				      "delta %u us: the merged window carried "
				      "%u filters", delta,
				      fake_radio_arm(arms_before)->n_filters);
		} else {
			zassert_equal(2u, arms,
				      "delta %u us: %u windows for a pair too "
				      "far apart to merge", delta, arms);
		}
	}

	assert_every_window_bounded();
	finish();
}

/*
 * AND THE RESIDUAL, recorded here rather than left to be rediscovered on a
 * bench: three tracked slots piled inside one arming lead still cost the third
 * its window. caps.max_addr_groups is 2 on nRF - seven of its eight logical
 * addresses share one BASE register and the tracking format's device number
 * lives inside that BASE - so a tracking window expresses at most two distinct
 * device numbers however wide the merge reach is.
 *
 * The reach fixes PAIRS, which is precisely the pairwise bad band the sweep
 * above walks. It does not make three-into-one fit, and the plan that
 * introduced it does not claim elimination. The third channel is told
 * DONE_MISSED, which is the honest answer for a capacity limit, and this test
 * exists so that changing it is a deliberate act.
 */
ZTEST(radiant_sched, test_three_tracked_slots_inside_one_lead_still_drop_the_third)
{
	radiant_time_t t0;
	radiant_time_t base;
	uint16_t       lead;

	bring_up();
	zassert_equal(2u, radiant_radio_caps_get()->max_addr_groups,
		      "the residual asserted below is a consequence of "
		      "max_addr_groups == 2; the preset no longer models it");
	fake_radio_caps_mut()->min_arm_lead_us = TEST_NRF52840_ARM_LEAD_US;
	lead = radiant_radio_caps_get()->min_arm_lead_us;
	zassert_true(300u <= (2u * TEST_TRACK_GUARD_US) + lead,
		     "all three slots must be inside one merge reach for this "
		     "to be testing the group cap and not the reach");

	t0 = radiant_radio_now();
	base = t0 + 100000u;
	post_tracked_slot(0u, base);
	post_tracked_slot(1u, base + 150u);
	post_tracked_slot(2u, base + 300u);
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(2u, fake_radio_arm(0u)->n_filters,
		      "the window carried %u filters; a tracking window on this "
		      "part expresses two device numbers however close the "
		      "third slot is", fake_radio_arm(0u)->n_filters);

	fake_radio_advance_to(base + 300u + TEST_TRACK_GUARD_US + 1u);

	zassert_equal(1u, log.ok_count[0]);
	zassert_equal(1u, log.ok_count[1]);
	zassert_equal(1u, log.missed_count[2],
		      "the third slot of a three-way pile-up was neither served "
		      "nor told - if this now passes with ok_count[2] == 1 the "
		      "group cap has changed and this test should be deleted, "
		      "not weakened");
	zassert_equal(1u, radiant_sched_stats_get()->missed);

	assert_every_window_bounded();
	finish();
}

/* ---------------------------------------------------------------------------
 * Preemption of a window that has not opened
 * ---------------------------------------------------------------------------
 */

/*
 * A COMMITTED-BUT-UNOPENED RECEIVE GIVES WAY TO ONE THAT REALLY STARTS EARLIER.
 *
 * arm_next() commits the radio early and deliberately - there is no timer of
 * our own, so an idle radio is a radio nothing wakes up to arm. The cost is
 * that a request arriving afterwards finds the radio spoken for up to a quarter
 * of a second out, and a tracked window is only 200 us wide. That is how a
 * window re-posted after a MISSED terminal (see api_sched_done() in
 * apps/common/ant_radio_radiant.c) was itself reported MISSED, and eight of
 * those in a row is RX_FAIL_GO_TO_SEARCH: a ~2 s dropout with nothing wrong on
 * the air.
 *
 * The competitor is on RF 26 so that could_join_armed() cannot take it - this
 * must exercise the "cannot share, so must displace" branch of want_preempt()
 * and not the merge one, which existed already.
 *
 * Nothing had opened, so nobody is told anything: a rebuild before the first
 * bit of preamble is free, which is why it counts as a replan and not a
 * preemption.
 */
ZTEST(radiant_sched, test_an_unopened_window_gives_way_to_a_receive_that_starts_earlier)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	radiant_time_t          far_open;
	radiant_time_t          near_open;

	bring_up();
	t0 = radiant_radio_now();
	far_open = t0 + 250000u;   /* what arm_next() commits to, a period out */
	near_open = far_open - 50000u;

	post_track(0u, far_open, far_open + 60000u);
	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(far_open, fake_radio_arm(0u)->t_open);

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING),
		    &track_filter[1], 1u, near_open, near_open + 55000u);
	r.rf_index = 26u;
	zassert_ok(radiant_sched_request_rx(1u, &r));
	zassert_ok(radiant_sched_tick());

	zassert_equal(2u, fake_radio_arm_count(),
		      "a receive starting 50 ms earlier waited behind a window "
		      "that had not opened");
	zassert_equal(near_open, fake_radio_arm(1u)->t_open);
	zassert_equal(26u, fake_radio_arm(1u)->rf_index,
		      "the earlier request was merged rather than armed, so "
		      "this tested the wrong branch");
	zassert_equal(1u, radiant_sched_stats_get()->replans);
	zassert_equal(0u, radiant_sched_stats_get()->preempted,
		      "tearing down a window before it opened was charged as a "
		      "preemption");
	zassert_equal(0u, log.n_done, "a free rebuild was reported as an ending");

	/* And the displaced request is not lost - it is served after, which is
	 * the whole difference between preempting and dropping. */
	fake_radio_advance_to(far_open + 60000u + 1u);
	zassert_equal(1u, log.ok_count[1]);
	zassert_equal(1u, log.ok_count[0],
		      "the displaced window was never armed again");
	zassert_equal(0u, radiant_sched_stats_get()->missed);

	assert_every_window_bounded();
	finish();
}

/*
 * THE OTHER HALF, and the reason the branch is guarded on `s.armed_open >
 * earliest` rather than on the start times alone: once a window has opened, its
 * reception is worth more than an earlier start, and tearing it down cannot
 * give the received preamble back.
 *
 * Same geometry as the test above with one difference - the clock is moved
 * inside the armed window first - so the only thing that can change the outcome
 * is the openness. The competitor's t_start is behind us and its t_end is not,
 * which is the one arrangement in which a request genuinely starts earlier than
 * an OPEN window: anything else is expired by pass_step() before want_preempt()
 * is ever consulted.
 *
 * The cost is stated as an assertion rather than left implicit: the earlier
 * request loses its window entirely (DONE_MISSED), because by the time the open
 * window ends there is no longer room to arm it. That is the trade being made,
 * and it is the right way round.
 */
ZTEST(radiant_sched, test_an_open_window_is_not_torn_down_for_a_receive_that_starts_earlier)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	radiant_time_t          open;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;

	post_track(0u, open, open + 60000u);
	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, fake_radio_arm_count());

	/* Open and listening. */
	fake_radio_advance_to(open + 1000u);
	zassert_equal(1u, fake_radio_arm_count());

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING),
		    &track_filter[1], 1u, open - 5000u, open + 50000u);
	r.rf_index = 26u;
	zassert_ok(radiant_sched_request_rx(1u, &r));
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count(),
		      "a listening window was torn down for a request that "
		      "merely started earlier");
	zassert_equal(0u, radiant_sched_stats_get()->preempted);
	zassert_equal(0u, log.aborted_count[0],
		      "the open window's members were told it ended early");

	fake_radio_advance_to(open + 60000u + 1u);
	zassert_equal(1u, log.ok_count[0], "the open window did not run out");
	zassert_equal(1u, log.missed_count[1],
		      "the earlier request was neither served nor told; it loses "
		      "its window here, and that is the trade");

	assert_every_window_bounded();
	finish();
}

/* ---------------------------------------------------------------------------
 * 32 channels
 * ---------------------------------------------------------------------------
 */

/*
 * THIRTY-TWO CHANNELS CONTENDING FOR THE SAME INSTANT, ON THE BACKEND WITH TWO
 * MATCHERS - every one gets served. Worst case the module is sized for: 32 is
 * the serial protocol's ceiling, 2 is the smallest max_filters any planned
 * backend has, so only two per slot and thirty are turned away each round. A
 * scheduler that always picked the lowest-numbered pending channel would
 * starve channel 2 forever and every other test here would still pass.
 *
 * Channels re-post from done() at the measured 249,696 us slot period (what
 * radiant_channel.c will do), the only way the rotation is observable.
 */
ZTEST(radiant_sched, test_thirty_two_contending_channels_are_all_served)
{
	radiant_time_t base;
	uint32_t   cycles = 19u;
	uint8_t    ch;

	fake_radio_caps_preset_rail();
	bring_up();

	base = radiant_radio_now() + 100000u;
	log.repost = true;
	for (ch = 0u; ch < (uint8_t)RADIANT_SCHED_MAX_CHANNELS; ch++) {
		log.next_open[ch] = base;
		post_track(ch, base, base + TEST_WINDOW_US);
	}
	zassert_ok(radiant_sched_tick());

	fake_radio_advance_to(base + (radiant_time_t)cycles * TEST_PERIOD_US + 1000u);

	for (ch = 0u; ch < (uint8_t)RADIANT_SCHED_MAX_CHANNELS; ch++) {
		zassert_true(log.ok_count[ch] >= 1u,
			     "channel %u was never once armed in %u slots", ch,
			     cycles);
	}
	/* Two per slot and no more, because that is what the part has. */
	zassert_equal(2u * radiant_sched_stats_get()->windows_armed,
		      radiant_sched_stats_get()->channels_windowed,
		      "some window carried other than two channels");
	zassert_equal(radiant_sched_stats_get()->windows_armed,
		      radiant_sched_stats_get()->windows_merged,
		      "some window was not merged at all");

	assert_every_window_bounded();
	finish();
}

/*
 * The same thirty-two channels on the nRF preset - also TWO here, which is
 * the point of running it both ways. The presets used to differ (RAIL two,
 * nRF eight) as an artefact of the mock, not the parts: RAIL fits two because
 * it has two sync words, nRF fits two because seven of its eight logical
 * addresses share a BASE register and a tracked address carries the device
 * number inside it. Same answer, different reasons - now separated as
 * caps.max_filters and caps.max_addr_groups.
 *
 * The real cost: 32 tracked sensors need sixteen windows per slot, not four.
 * ADR 0005's "32 sensors do not cost 32 windows" survives (merging still
 * halves it) but was originally measured against a mock modelling a radio
 * nobody has.
 */
ZTEST(radiant_sched, test_thirty_two_tracked_channels_merge_two_at_a_time_on_nrf)
{
	radiant_time_t base;
	uint32_t   cycles = 19u;
	uint8_t    ch;

	bring_up();
	zassert_equal(8u, radiant_radio_caps_get()->max_filters);
	zassert_equal(2u, radiant_radio_caps_get()->max_addr_groups);

	base = radiant_radio_now() + 100000u;
	log.repost = true;
	for (ch = 0u; ch < (uint8_t)RADIANT_SCHED_MAX_CHANNELS; ch++) {
		log.next_open[ch] = base;
		post_track(ch, base, base + TEST_WINDOW_US);
	}
	zassert_ok(radiant_sched_tick());

	fake_radio_advance_to(base + (radiant_time_t)cycles * TEST_PERIOD_US + 1000u);

	for (ch = 0u; ch < (uint8_t)RADIANT_SCHED_MAX_CHANNELS; ch++) {
		zassert_true(log.ok_count[ch] >= 1u,
			     "channel %u was never once armed in %u slots", ch,
			     cycles);
	}
	zassert_equal(2u * radiant_sched_stats_get()->windows_armed,
		      radiant_sched_stats_get()->channels_windowed,
		      "some window carried other than two tracked channels");

	assert_every_window_bounded();
	finish();
}

/* ---------------------------------------------------------------------------
 * Deadlines
 * ---------------------------------------------------------------------------
 */

/*
 * A request that can no longer be honoured is refused by the scheduler, not
 * offered to the backend and refused there. caps.min_arm_lead_us is the slack
 * budget; the HAL never silently runs late (RADIANT_RADIO_ETIME), so the correct
 * behaviour is "never provoke it" - fake_radio must record no arm call at
 * all. The transmit boundary is tested to the microsecond because t_sync
 * cannot be nudged: a late frame lands in the next slot.
 */
ZTEST(radiant_sched, test_an_unreachable_deadline_is_refused_rather_than_run_late)
{
	struct radiant_sched_tx t;
	radiant_time_t          t0;
	uint16_t            lead;

	bring_up();
	t0 = radiant_radio_now();
	lead = radiant_radio_caps_get()->min_arm_lead_us;
	zassert_true(lead > 1u, "this test needs a non-zero arming lead");

	/* A receive window that closed before we got here. */
	post_track(0u, t0 - 1000u, t0 - 500u);
	zassert_ok(radiant_sched_tick());
	zassert_equal(0u, fake_radio_arm_count());
	zassert_equal(1u, log.missed_count[0]);
	zassert_false(radiant_sched_pending(0u));

	/* One microsecond inside the arming lead: not transmitted at all. */
	tx_req_init(&t, 1u, t0 + (radiant_time_t)lead - 1u);
	zassert_ok(radiant_sched_request_tx(1u, &t));
	zassert_ok(radiant_sched_tick());
	zassert_equal(0u, fake_radio_arm_count(),
		      "a transmit inside min_arm_lead_us was offered to the backend");
	zassert_equal(1u, log.missed_count[1]);

	/* Exactly on the boundary: transmitted, and at the instant asked for. */
	tx_req_init(&t, 2u, t0 + (radiant_time_t)lead);
	zassert_ok(radiant_sched_request_tx(2u, &t));
	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(FAKE_RADIO_ARM_TX, fake_radio_arm(0u)->kind);
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_arm(0u)->rc);
	zassert_equal(t.t_sync_at, fake_radio_arm(0u)->t_sync_at);

	/* AND IT WENT OUT ADDRESSED TO CHANNEL 2, not channel 1 whose transmit
	 * was refused above. On the nRF backend TXADDRESS is an index into
	 * BASE/PREFIX registers some earlier operation loaded, so a scheduler
	 * that forwarded no address would emit a well-formed frame carrying
	 * the previous channel's device number, invisibly. */
	zassert_equal(5u, fake_radio_arm(0u)->addr_len,
		      "the transmit carried no on-air address");
	zassert_mem_equal(fake_radio_arm(0u)->addr, track_filter[2].addr, 5u,
			  "the transmit did not carry channel 2's address");

	fake_radio_advance_to(t.t_sync_at + 10u);
	zassert_equal(1u, log.n_tx);
	zassert_equal(2u, log.tx[0].ch);
	zassert_equal(t.t_sync_at, log.tx[0].t_sync);
	zassert_equal(1u, log.ok_count[2]);

	assert_every_window_bounded();
	finish();
}

/* A window still open when a transmit must be armed is truncated, not
 * abandoned and not allowed to overrun. One radio: the receive tail is worth
 * less than an exact-instant transmit, so the scheduler gives up exactly
 * caps.min_arm_lead_us of it. */
ZTEST(radiant_sched, test_a_window_is_truncated_to_clear_a_transmit_deadline)
{
	struct radiant_sched_tx t;
	radiant_time_t          t0;
	radiant_time_t          open;
	radiant_time_t          tx_at;
	uint16_t            lead;

	bring_up();
	t0 = radiant_radio_now();
	lead = radiant_radio_caps_get()->min_arm_lead_us;
	open = t0 + 100000u;
	tx_at = open + 5000u;

	/* A window that wants to stay open well past the transmit. */
	post_track(0u, open, open + 50000u);

	tx_req_init(&t, 1u, tx_at);
	zassert_ok(radiant_sched_request_tx(1u, &t));

	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(FAKE_RADIO_ARM_RX, fake_radio_arm(0u)->kind,
		      "the receive that starts first must run first");
	zassert_equal(open, fake_radio_arm(0u)->t_open);
	zassert_equal(tx_at - (radiant_time_t)lead, fake_radio_arm(0u)->t_close,
		      "the window was not truncated to clear the transmit");

	fake_radio_advance_to(tx_at + 10u);
	zassert_equal(2u, fake_radio_arm_count());
	zassert_equal(FAKE_RADIO_ARM_TX, fake_radio_arm(1u)->kind);
	zassert_equal(tx_at, fake_radio_arm(1u)->t_sync_at);
	zassert_equal(1u, log.n_tx);
	zassert_equal(tx_at, log.tx[0].t_sync);

	assert_every_window_bounded();
	finish();
}

/*
 * A reply that falls due inside an armed window takes the radio. This is the
 * turnaround, the tightest deadline in the link layer: a tracking channel
 * that hears acknowledged data owes a full 8-byte reply ~1.55 ms later
 * (docs/spike-b-part2-results.md). The request arrives from inside the
 * receive callback of the still-armed window, so only preemption can serve
 * it, and it also produces a late ABORTED terminal (with no fault injection)
 * that the scheduler must recognise rather than mistake for the transmit's end.
 */
ZTEST(radiant_sched, test_a_reply_preempts_the_window_it_was_heard_in)
{
	radiant_time_t t0;
	radiant_time_t open;
	radiant_time_t heard_at;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;
	heard_at = open + 1000u;

	/* Two channels sharing one long window; channel 1 will owe a reply. */
	post_track(0u, open, open + 50000u);
	post_track(1u, open, open + 50000u);
	zassert_ok(radiant_sched_tick());
	zassert_equal(2u, fake_radio_arm(0u)->n_filters);

	/* Measured: data packet -> the receiver's acknowledgement, 1559-1567 us,
	 * two runs, n=161. */
	log.reply_after_us = 1559u;
	log.reply_ch = 1u;
	air_frame_for(1u, heard_at);

	fake_radio_advance_to(heard_at + 1559u + 100u);

	zassert_equal(1u, log.n_rx);
	zassert_equal(1u, log.rx[0].ch);

	zassert_equal(2u, fake_radio_arm_count());
	zassert_equal(FAKE_RADIO_ARM_TX, fake_radio_arm(1u)->kind);
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_arm(1u)->rc);
	zassert_equal(heard_at + 1559u, fake_radio_arm(1u)->t_sync_at,
		      "the reply did not land on the measured turnaround");
	zassert_true(fake_radio_arm(1u)->from_callback,
		     "the reply was not armed from inside the receive callback");

	zassert_equal(1u, log.n_tx);
	zassert_equal(1u, log.tx[0].ch);
	zassert_equal(heard_at + 1559u, log.tx[0].t_sync);

	/* Channel 0 shared the window and is told it ended early, so that it
	 * re-predicts instead of assuming its window ran. One radio; a reply
	 * that is already owed outranks the rest of a window. */
	zassert_equal(1u, log.aborted_count[0]);

	/* The aborted window's terminal event arrived after the transmit was
	 * armed. It carried the dead op id and changed nothing. */
	zassert_equal(1u, radiant_sched_stats_get()->stale_events,
		      "the aborted window's late terminal event was not seen as stale");
	zassert_equal(1u, radiant_sched_stats_get()->tx_armed);

	assert_every_window_bounded();
	finish();
}

/* The same displacement, driven from thread context, to exercise the
 * preemption path itself rather than the incidental abort of replacing a
 * channel's own in-flight request. The clock is moved INTO the window first:
 * an unopened window rebuilds for free, but an already-listening one loses
 * its tail and its members must be told. */
ZTEST(radiant_sched, test_a_transmit_deadline_displaces_an_armed_window)
{
	struct radiant_sched_tx t;
	radiant_time_t          t0;
	radiant_time_t          open;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;

	post_track(0u, open, open + 50000u);
	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, fake_radio_arm_count());

	/* The window is now open and listening. */
	fake_radio_advance_to(open + 1000u);
	zassert_equal(1u, fake_radio_arm_count());

	tx_req_init(&t, 2u, open + 20000u); /* inside the armed window */
	zassert_ok(radiant_sched_request_tx(2u, &t));
	zassert_ok(radiant_sched_tick());

	zassert_equal(2u, fake_radio_arm_count());
	zassert_equal(FAKE_RADIO_ARM_TX, fake_radio_arm(1u)->kind);
	zassert_equal(1u, radiant_sched_stats_get()->preempted,
		      "an open window was cut short but not counted as a preemption");
	zassert_equal(0u, radiant_sched_stats_get()->replans,
		      "an open window was treated as a free rebuild");
	zassert_equal(1u, log.aborted_count[0]);
	zassert_equal(1u, radiant_sched_stats_get()->stale_events);

	fake_radio_advance_to(t.t_sync_at + 10u);
	zassert_equal(1u, log.n_tx);
	zassert_equal(t.t_sync_at, log.tx[0].t_sync);

	assert_every_window_bounded();
	finish();
}

/* ---------------------------------------------------------------------------
 * Late events
 * ---------------------------------------------------------------------------
 */

/* An event carrying the id of an already-ended operation changes nothing. On
 * real hardware the frame was in the receiver's pipeline when the window
 * ended, so the event arrives late; the op id exists to catch it, preventing
 * a frame routed to the wrong channel or a terminal counted twice. */
ZTEST(radiant_sched, test_a_dead_operations_late_event_changes_nothing)
{
	struct fake_radio_air air;
	radiant_time_t            t0;
	radiant_time_t            open;
	radiant_time_t            close;
	uint32_t              dead_op;
	uint8_t               frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t               n;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;
	close = open + TEST_WINDOW_US;

	post_track(0u, open, close);
	zassert_ok(radiant_sched_tick());
	dead_op = fake_radio_arm(0u)->op;
	zassert_not_equal(0u, dead_op, "the mock handed out op id zero");

	fake_radio_advance_to(close + 1u);
	zassert_equal(1u, log.n_done);
	zassert_equal(0u, log.n_rx);

	/* Something else is armed by the time the straggler arrives, so a
	 * scheduler that ignored the op id would route the frame into it. */
	post_track(1u, close + 100000u, close + 100000u + TEST_WINDOW_US);
	zassert_ok(radiant_sched_tick());
	zassert_equal(2u, fake_radio_arm_count());

	n = fake_radio_build_ant_frame(frame, TEST_DEVNUM_BASE, TEST_DEV_TYPE,
				       TEST_TRANS_TYPE, test_payload);
	fake_radio_air_init(&air);
	air.t_sync = close;
	air.len = n;
	memcpy(air.bytes, frame, n);
	zassert_ok(fake_radio_inject_late_rx(dead_op, &air, 0u));
	zassert_ok(fake_radio_inject_late_tx(dead_op, RADIANT_RADIO_STATUS_OK));

	zassert_equal(0u, log.n_rx, "a dead operation's frame was routed to a channel");
	zassert_equal(0u, log.n_tx);
	zassert_equal(1u, log.n_done, "a dead operation's event produced a second done()");
	zassert_equal(2u, radiant_sched_stats_get()->stale_events);

	/* The live window is untouched and still runs to its own end. */
	zassert_equal(2u, fake_radio_arm_count());
	fake_radio_advance_to(close + 100000u + TEST_WINDOW_US + 1u);
	zassert_equal(1u, log.ok_count[1]);

	assert_every_window_bounded();
	finish();
}

/* ---------------------------------------------------------------------------
 * Background scan
 * ---------------------------------------------------------------------------
 */

/* RADIANT_TIME_NEVER is not a valid window edge, so "receive until I say stop" is
 * a chain of bounded windows this module re-arms (that translation lives
 * nowhere else in radiant). Also checks the two properties that make it a
 * *background* scan: it fills the gap ahead of a tracked window, and gets out
 * of the way exactly when that window needs the radio. */
ZTEST(radiant_sched, test_a_continuous_request_becomes_a_chain_of_bounded_windows)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	radiant_time_t          tracked_at;
	uint16_t            lead;
	uint32_t            i;
	uint32_t            chunks_before;

	bring_up();
	t0 = radiant_radio_now();
	lead = radiant_radio_caps_get()->min_arm_lead_us;

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), search_filter,
		    8u, t0, RADIANT_TIME_NEVER);
	r.chunk_us = 50000u;
	zassert_ok(radiant_sched_request_rx(31u, &r));
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(t0 + (radiant_time_t)lead, fake_radio_arm(0u)->t_open,
		      "a scan asking for 'now' must be armed exactly one arming "
		      "lead from now");
	zassert_equal(t0 + (radiant_time_t)lead + 50000u, fake_radio_arm(0u)->t_close);
	zassert_equal(8u, fake_radio_arm(0u)->n_filters);
	zassert_equal(0u, fake_radio_arm(0u)->flags & RADIANT_RX_STOP_ON_FIRST,
		      "a search window must never stop on the first frame");

	/* Four chunks of 50 ms, re-armed from each other's terminal event. */
	fake_radio_advance_to(t0 + 190000u);
	zassert_true(fake_radio_arm_count() >= 4u,
		     "the scan was not re-armed: %u windows",
		     fake_radio_arm_count());
	zassert_equal(fake_radio_arm_count(),
		      radiant_sched_stats_get()->scan_chunks);
	zassert_true(radiant_sched_pending(31u), "the continuous request was consumed");
	for (i = 0u; i < fake_radio_arm_count(); i++) {
		zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_arm(i)->rc);
	}

	/* Now a tracked channel wants the radio. The scan must not be cut short
	 * for a window that is still a long way off... */
	tracked_at = radiant_radio_now() + 200000u;
	chunks_before = fake_radio_arm_count();
	post_track(0u, tracked_at, tracked_at + TEST_WINDOW_US);
	zassert_ok(radiant_sched_tick());
	zassert_equal(chunks_before, fake_radio_arm_count(),
		      "a tracked window 200 ms away cut a scan chunk short");

	/* ...and the tracked window must nonetheless be served on time, with
	 * the scan filling what is left of the gap. */
	fake_radio_advance_to(tracked_at + TEST_WINDOW_US + 1u);
	zassert_equal(1u, log.ok_count[0], "the tracked window lost to the scan");
	zassert_true(radiant_sched_stats_get()->scan_chunks > chunks_before,
		     "the scan stopped filling the gap");

	assert_every_window_bounded();
	finish();
}

/* A scan chunk is reported with the bounds it ACTUALLY got, not the ones
 * requested - the whole reason armed() exists. A wildcard sweep credits its
 * dwell in listening time; told only what it proposed, a 60 ms chunk would be
 * credited as the 250 ms asked for, and "certain within one sweep" would
 * quietly become "certain within four". */
ZTEST(radiant_sched, test_a_scan_chunk_is_reported_with_the_bounds_it_got)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	radiant_time_t          tracked_at;
	uint16_t            lead;

	bring_up();
	t0 = radiant_radio_now();
	lead = radiant_radio_caps_get()->min_arm_lead_us;

	/* The tracked window first, so the gap ahead of it is known before the
	 * scan is ever armed. */
	tracked_at = t0 + 60000u;
	post_track(0u, tracked_at, tracked_at + TEST_WINDOW_US);

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), search_filter,
		    8u, t0, RADIANT_TIME_NEVER);
	r.chunk_us = 250000u;
	zassert_ok(radiant_sched_request_rx(31u, &r));
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, log.n_armed, "%u arms reported", log.n_armed);
	zassert_equal(31u, log.armed[0].ch);
	zassert_equal(t0 + (radiant_time_t)lead, log.armed[0].t_open);
	zassert_equal(tracked_at - (radiant_time_t)lead, log.armed[0].t_close,
		      "the scan was told it had the whole chunk it asked for");

	/* A bounded window is reported too, on the same terms. */
	fake_radio_advance_to(tracked_at + TEST_WINDOW_US + 1u);
	zassert_true(log.n_armed >= 2u);
	zassert_equal(0u, log.armed[1].ch);
	zassert_equal(tracked_at, log.armed[1].t_open);
	zassert_equal(tracked_at + (radiant_time_t)TEST_WINDOW_US,
		      log.armed[1].t_close);

	assert_every_window_bounded();
	finish();
}

/* A scan chunk displaced by a tracked window that turned up after arming is
 * REPORTED, and the request survives. Both halves pull opposite ways: the
 * request must survive or a scan would be consumed by the first tracked
 * channel to want the radio, but an unreported chunk would leave a
 * dwell-crediting sweep believing it listened when it did not. */
ZTEST(radiant_sched, test_a_displaced_scan_chunk_is_reported_and_stays_live)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	radiant_time_t          tracked_at;

	bring_up();
	t0 = radiant_radio_now();

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), search_filter,
		    8u, t0, RADIANT_TIME_NEVER);
	r.chunk_us = 250000u;
	zassert_ok(radiant_sched_request_rx(31u, &r));
	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, fake_radio_arm_count());

	/* Open and listening, with nothing else pending: the chunk got the
	 * whole 250 ms it asked for. */
	fake_radio_advance(20000u);
	zassert_equal(0u, log.n_done);

	/* Now a tracked window turns up inside it. */
	tracked_at = radiant_radio_now() + 30000u;
	post_track(0u, tracked_at, tracked_at + TEST_WINDOW_US);
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, log.aborted_count[31],
		      "a displaced scan chunk was not reported at all");
	zassert_true(radiant_sched_pending(31u),
		     "being displaced consumed the scan");

	fake_radio_advance_to(tracked_at + TEST_WINDOW_US + 1u);
	zassert_equal(1u, log.ok_count[0], "the tracked window lost to the scan");
	zassert_true(radiant_sched_pending(31u), "the scan did not resume");

	assert_every_window_bounded();
	finish();
}

/*
 * A SENSOR TRACKED ALONGSIDE A SEARCH KEEPS EVERY SLOT, OVER MANY PERIODS.
 *
 * test_a_displaced_scan_chunk_is_reported_and_stays_live proves ONE tracked
 * window displaces a running chunk. That is not the same claim as this one,
 * and the difference is what a real session lives in: the scan re-arms from
 * its own terminal callback, so displacement happens again every period, and
 * "the scan yields" has to hold for the whole ride rather than once.
 *
 * Measured on real hardware first, which is why the numbers here are not round
 * ones. In captures/zwift-20260818-160751.pcap a power meter on channel 1 was
 * tracking cleanly at 8182/32768 s when the heart-rate strap on channel 2 was
 * switched off. Eight missed slots later channel 2 went to search
 * (EVENT_RX_FAIL_GO_TO_SEARCH, t=159.259) and channel 1 - which was still on
 * the air, still at full signal - immediately began losing EVERY OTHER SLOT:
 *
 *     157.798 .. 159.546   8 consecutive slots, no loss     (ch2 already
 *                                                            silent since
 *                                                            157.289)
 *     159.796 .. 163.541   OK FAIL OK FAIL OK FAIL ...      (ch2 searching)
 *
 * 10.6% loss before, 44.4% after, and the transition tracks ch2's change of
 * STATE rather than its going off the air 2.3 s earlier. So this is not RF.
 *
 * The suspected mechanism is the chunk ceiling: the sweep asks for the dwell
 * still owed to the current address set, up to RADIANT_SEARCH_DWELL_DEFAULT_US
 * (260 ms), which is LONGER than the 249.696 ms slot period. If a chunk is
 * ever armed without being truncated in front of the next tracked window it
 * swallows that window whole; the window is reported MISSED, re-posted, and
 * wins the next pass - which is exactly the alternation above.
 *
 * chunk_us is therefore deliberately 260000 here and the period is the
 * measured one. Asserting on ok_count is the point: a scheduler that armed the
 * window late, or not at all, reports MISSED, which is the RX_FAIL the host
 * sees.
 */
ZTEST(radiant_sched, test_a_tracked_channel_keeps_every_slot_under_a_search)
{
	struct radiant_sched_rx r;
	radiant_time_t          base;
	uint8_t                 frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t                 n;
	const uint32_t          cycles = 20u;

	bring_up();

	base = radiant_radio_now() + 100000u;

	/* The master keeps transmitting throughout - every loss below is the
	 * dongle's, not the air's. */
	n = fake_radio_build_ant_frame(frame, TEST_DEVNUM_BASE, TEST_DEV_TYPE,
				       TEST_TRANS_TYPE, test_payload);
	zassert_equal(cycles + 1u,
		      fake_radio_air_master(base + (radiant_time_t)TEST_WINDOW_US / 2u,
					    TEST_PERIOD_US, cycles + 1u, frame, n),
		      "the master stream was not queued");

	/* Channel 0 tracks it, re-posting from done() like radiant_channel.c. */
	log.repost = true;
	log.repost_mask = 1u; /* channel 0 only - channel 31 is the search */
	log.next_open[0] = base;
	post_track(0u, base, base + (radiant_time_t)TEST_WINDOW_US);

	/* Channel 31 searches: a standing continuous request, one dwell per
	 * chunk, exactly as api_pump_locked() posts it. */
	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH),
		    search_filter, 8u, base, RADIANT_TIME_NEVER);
	r.chunk_us = 260000u; /* RADIANT_SEARCH_DWELL_DEFAULT_US */
	zassert_ok(radiant_sched_request_rx(31u, &r));

	zassert_ok(radiant_sched_tick());
	fake_radio_advance_to(base + (radiant_time_t)cycles * TEST_PERIOD_US +
			      TEST_PERIOD_US / 2u);

	zassert_true(radiant_sched_pending(31u), "the search stopped searching");
	zassert_equal(0u, log.missed_count[0],
		      "the search cost the tracked channel %u of %u slots "
		      "(%u served) - a searching channel must yield the radio "
		      "to a tracked one every period, not every other one",
		      log.missed_count[0], cycles, log.ok_count[0]);
	zassert_true(log.ok_count[0] >= cycles,
		     "tracked channel served %u of %u slots", log.ok_count[0],
		     cycles);

	assert_every_window_bounded();
	finish();
}

/*
 * Cancelling a continuous request stops it, and stops it completely: the radio
 * is idle immediately rather than at the end of the current chunk, and nothing
 * re-arms it afterwards.
 */
ZTEST(radiant_sched, test_cancelling_a_scan_leaves_the_radio_idle)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;

	bring_up();
	t0 = radiant_radio_now();

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), search_filter,
		    4u, t0, RADIANT_TIME_NEVER);
	zassert_ok(radiant_sched_request_rx(9u, &r));
	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, fake_radio_arm_count());
	zassert_false(fake_radio_is_idle());

	zassert_ok(radiant_sched_cancel(9u));
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
	zassert_false(radiant_sched_pending(9u));
	zassert_equal(0u, log.n_done, "cancel() must be silent for the channel "
				      "that asked for it");

	/* Nothing brings it back. */
	fake_radio_advance(1000000u);
	zassert_equal(1u, fake_radio_arm_count());

	assert_every_window_bounded();
	finish();
}

/* ---------------------------------------------------------------------------
 * Flags and failure modes
 * ---------------------------------------------------------------------------
 */

/* RADIANT_RX_STOP_ON_FIRST survives an unmerged window and is dropped from a merged
 * one. The HAL forbids it on multi-filter windows (more than one master may
 * transmit), so a scheduler that passed it through would get RADIANT_RADIO_EINVAL,
 * which finish() also catches. The other half matters too: it must still be
 * set with nothing to merge, or the receive-current saving is lost. */
ZTEST(radiant_sched, test_stop_on_first_survives_only_an_unmerged_window)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	radiant_time_t          open;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING), &track_filter[0],
		    1u, open, open + 50000u);
	r.stop_on_first = true;
	zassert_ok(radiant_sched_request_rx(0u, &r));
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count());
	zassert_true((fake_radio_arm(0u)->flags & RADIANT_RX_STOP_ON_FIRST) != 0u,
		     "a single-master window lost its stop-on-first");

	/* The first good frame ends the window: no separate timeout event. */
	air_frame_for(0u, open + 500u);
	fake_radio_advance_to(open + 1000u);
	zassert_equal(1u, log.n_rx);
	zassert_equal(1u, log.ok_count[0]);
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());

	/* Two channels, both asking for it, must not get it. */
	open = radiant_radio_now() + 100000u;
	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING), &track_filter[0],
		    1u, open, open + TEST_WINDOW_US);
	r.stop_on_first = true;
	zassert_ok(radiant_sched_request_rx(0u, &r));
	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING), &track_filter[1],
		    1u, open, open + TEST_WINDOW_US);
	r.stop_on_first = true;
	zassert_ok(radiant_sched_request_rx(1u, &r));
	zassert_ok(radiant_sched_tick());

	zassert_equal(2u, fake_radio_arm_count());
	zassert_equal(2u, fake_radio_arm(1u)->n_filters);
	zassert_equal(0u, fake_radio_arm(1u)->flags & RADIANT_RX_STOP_ON_FIRST,
		      "a merged window carried stop-on-first, which the HAL forbids");

	fake_radio_advance_to(open + TEST_WINDOW_US + 1u);
	assert_every_window_bounded();
	finish();
}

/* RADIANT_RADIO_EBUSY leaves the request pending. An arm from thread context can
 * race the callback consuming the operation slot; the backend resolves it
 * with EBUSY, and the core must neither lose the request nor spin on it -
 * the next commit tries again. */
ZTEST(radiant_sched, test_ebusy_leaves_the_request_for_the_next_commit)
{
	radiant_time_t t0;
	radiant_time_t open;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;

	fake_radio_force_next_arm(RADIANT_RADIO_EBUSY);
	post_track(0u, open, open + TEST_WINDOW_US);
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(0u, fake_radio_arm(0u)->op, "the arm was not rejected");
	zassert_equal(1u, radiant_sched_stats_get()->arm_ebusy);
	zassert_true(radiant_sched_pending(0u), "the request was thrown away on EBUSY");
	zassert_equal(0u, log.n_done, "EBUSY was reported to the channel as an ending");

	zassert_ok(radiant_sched_tick());
	zassert_equal(2u, fake_radio_arm_count());
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_arm(1u)->rc);

	fake_radio_advance_to(open + TEST_WINDOW_US + 1u);
	zassert_equal(1u, log.ok_count[0]);

	assert_every_window_bounded();
	finish();
}

/* A backend reporting it could not complete an operation ends the request
 * rather than leaving it in flight forever - a scheduler with no case for
 * RADIANT_RADIO_STATUS_FAILED would hold the channel in flight and never
 * schedule it again. */
ZTEST(radiant_sched, test_a_failed_operation_ends_the_request)
{
	radiant_time_t t0;
	radiant_time_t open;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;

	post_track(0u, open, open + TEST_WINDOW_US);
	fake_radio_force_next_terminal(RADIANT_RADIO_STATUS_FAILED);
	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, fake_radio_arm_count());

	fake_radio_advance_to(open + TEST_WINDOW_US + 1u);
	zassert_equal(1u, log.n_done);
	zassert_equal(RADIANT_SCHED_DONE_FAILED, log.done[0].why);
	zassert_false(radiant_sched_pending(0u));
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());

	assert_every_window_bounded();
	finish();
}

/* ---------------------------------------------------------------------------
 * Denial - an arbitrated backend lending the radio to another stack
 * ---------------------------------------------------------------------------
 */

/* A bounded window the backend refused at the arm call is consumed and
 * reported as DENIED, never MISSED (which a channel counts eight of before
 * deciding its sensor is gone) or FAILED (request-is-broken). A denial is a
 * statement about the other stack, the owner's business - and the request
 * must not be left in the table, or it retries forever. */
ZTEST(radiant_sched, test_a_denied_arm_is_reported_as_denied_and_consumed)
{
	radiant_time_t t0;
	radiant_time_t open;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;

	fake_radio_force_next_arm(RADIANT_RADIO_EDENIED);
	post_track(0u, open, open + TEST_WINDOW_US);
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(0u, fake_radio_arm(0u)->op, "the arm was not rejected");
	zassert_equal(1u, radiant_sched_stats_get()->arm_denied);
	zassert_equal(1u, radiant_sched_stats_get()->denied);
	zassert_equal(0u, radiant_sched_stats_get()->missed,
		      "a denial was charged to the miss counter");
	zassert_equal(0u, radiant_sched_stats_get()->arm_rejected,
		      "a denial was charged to the rejection counter");
	zassert_equal(1u, log.n_done);
	zassert_equal(RADIANT_SCHED_DONE_DENIED, log.done[0].why);
	zassert_false(radiant_sched_pending(0u),
		      "a bounded request survived a denial and would be retried");

	assert_every_window_bounded();
	finish();
}

/* The same fact arriving late: accepted, then never granted. This is the
 * ordinary MPSL shape - the request is placed, the other stack wins the slot,
 * BLOCKED comes back after the window should have opened - and it must reach
 * the owner as DENIED, not ABORTED, since ABORTED means the window HAD opened
 * (which decides whether a scan's dwell is credited). */
ZTEST(radiant_sched, test_a_denied_terminal_is_not_an_abort)
{
	radiant_time_t t0;
	radiant_time_t open;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;

	post_track(0u, open, open + TEST_WINDOW_US);
	fake_radio_force_next_terminal(RADIANT_RADIO_STATUS_DENIED);
	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, fake_radio_arm_count());

	fake_radio_advance_to(open + TEST_WINDOW_US + 1u);
	zassert_equal(1u, log.n_done);
	zassert_equal(RADIANT_SCHED_DONE_DENIED, log.done[0].why);
	zassert_equal(0u, log.aborted_count[0], NULL);
	zassert_equal(1u, radiant_sched_stats_get()->denied);
	zassert_equal(0u, radiant_sched_stats_get()->arm_denied,
		      "a late denial was counted as a synchronous refusal");
	zassert_false(radiant_sched_pending(0u));
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());

	assert_every_window_bounded();
	finish();
}

/* A CONTINUOUS request survives a run of denials, and the pass still
 * terminates - two properties in tension. The request must not be consumed
 * (a denied scan chunk wants the next gap), but retrying it within the same
 * pass would burn the whole iteration budget refusing against the same
 * arbiter. fake_radio's arm log checks the second: one refused arm per
 * commit, not thirty. */
ZTEST(radiant_sched, test_a_denied_scan_chunk_keeps_the_request_and_ends_the_pass)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	uint32_t            arms_after_first;

	bring_up();
	t0 = radiant_radio_now();

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), search_filter,
		    8u, t0, RADIANT_TIME_NEVER);
	r.chunk_us = 50000u;
	zassert_ok(radiant_sched_request_rx(31u, &r));

	fake_radio_force_arm_repeat(RADIANT_RADIO_EDENIED, 8u);
	zassert_ok(radiant_sched_tick());

	arms_after_first = fake_radio_arm_count();
	zassert_equal(1u, arms_after_first,
		      "one commit made %u arm attempts against a denying backend",
		      arms_after_first);
	zassert_true(radiant_sched_pending(31u),
		     "a denied chunk consumed the standing continuous request");

	/* Several more commits, each costing exactly one refusal. */
	zassert_ok(radiant_sched_tick());
	zassert_ok(radiant_sched_tick());
	zassert_equal(3u, fake_radio_arm_count(), NULL);
	zassert_equal(3u, radiant_sched_stats_get()->arm_denied);
	zassert_true(radiant_sched_pending(31u), NULL);

	/* And when the air comes back the scan simply resumes - no re-post, no
	 * pump, no thread. */
	fake_radio_force_arm_repeat(RADIANT_RADIO_OK_RC, 0u);
	zassert_ok(radiant_sched_tick());
	zassert_equal(RADIANT_RADIO_OK_RC,
		      fake_radio_arm(fake_radio_arm_count() - 1u)->rc,
		      "the scan never armed again after the arbiter let go");

	fake_radio_advance(200000u);
	zassert_true(fake_radio_arm_count() > 5u, "the chain did not restart");

	assert_every_window_bounded();
	finish();
}

/*
 * A CONTINUOUS request survives the RADIO reporting ABORTED, not just a
 * preemption - the twin of the denial test above, and the case that actually
 * shipped broken.
 *
 * Two ways a scan chunk stops early went through different code: abort_armed()
 * (scheduler preempting itself) always kept the request, but end_armed()
 * handling a backend RADIANT_RADIO_STATUS_ABORTED (an MPSL timeslot cutting a chunk
 * short) dropped it - the same event meant "paused" down one path and
 * "finished" down the other. No existing test caught it: preemption tests
 * never reach end_armed(), and every end_armed() test used OK. On the bench
 * it caused a total wedge (0/100 packets beside a BLE advertiser, vs 406/406
 * without) because radiant_api.c keeps its search slot bound across an
 * ABORTED, so with the request deleted underneath it neither layer moves.
 *
 * The assertion is on the REQUEST SURVIVING: the wedge is a lost request, and
 * everything downstream is a consequence.
 */
ZTEST(radiant_sched, test_an_aborted_scan_chunk_keeps_the_standing_request)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	uint32_t            arms_before;

	bring_up();
	t0 = radiant_radio_now();

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), search_filter,
		    8u, t0, RADIANT_TIME_NEVER);
	r.chunk_us = 50000u;
	zassert_ok(radiant_sched_request_rx(31u, &r));
	zassert_ok(radiant_sched_tick());
	zassert_equal(1u, fake_radio_arm_count(), NULL);

	/* The chunk armed and opened, and then the air was taken away - the
	 * backend's ABORTED, not the scheduler's own preemption. */
	arms_before = fake_radio_arm_count();
	fake_radio_force_next_terminal(RADIANT_RADIO_STATUS_ABORTED);
	fake_radio_advance(60000u);

	zassert_true(radiant_sched_pending(31u),
		     "an ABORTED chunk consumed the standing continuous request - "
		     "nothing re-posts a background scan, so this is permanent");

	/* And having survived, it resumes on its own, exactly as it does after a
	 * denial. A request that is kept but never armed again is the same wedge
	 * with a different signature. */
	fake_radio_advance(200000u);
	zassert_true(fake_radio_arm_count() > arms_before,
		     "the scan never armed again after an ABORTED chunk");

	assert_every_window_bounded();
	finish();
}

/* ---------------------------------------------------------------------------
 * caps.max_window_us - the arbitrated backend's operation-length ceiling
 * ---------------------------------------------------------------------------
 */

/*
 * A backend that cannot arm a long window gets a short REQUEST, not a long
 * one it has to shorten behind the scheduler's back. The wrong version is
 * plausible and silent: a backend that accepted 250 ms and quietly ran 20 ms
 * would have the sweep credit 250 ms of listening for 20 ms of it, silently
 * breaking "certain within one sweep". fake_radio refuses an over-long window
 * with ENOTSUP, and finish() asserts rc_enotsup == 0, so a regression here
 * fails the whole suite.
 */
ZTEST(radiant_sched, test_a_capped_backend_gets_shorter_chunks_not_shortened_ones)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	uint32_t            i;

	bring_up();
	fake_radio_caps_mut()->max_window_us = 20000u;
	t0 = radiant_radio_now();

	/* A chunk five times the ceiling. */
	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), search_filter,
		    8u, t0, RADIANT_TIME_NEVER);
	r.chunk_us = 100000u;
	zassert_ok(radiant_sched_request_rx(31u, &r));
	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count());
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_arm(0u)->rc,
		      "the capped window was refused rather than bounded");
	zassert_equal(20000u,
		      (uint32_t)(fake_radio_arm(0u)->t_close -
				 fake_radio_arm(0u)->t_open),
		      "the request was not bounded by caps.max_window_us");

	/*
	 * And the dwell is not lost, it is chunked: several windows in a row,
	 * every one of them inside the ceiling. A continuous request is not
	 * consumed by a chunk that ended normally, which is the machinery this
	 * relies on rather than new machinery of its own.
	 */
	fake_radio_advance(250000u);
	zassert_true(fake_radio_arm_count() >= 8u,
		     "only %u chunks in 250 ms against a 20 ms ceiling",
		     fake_radio_arm_count());
	for (i = 0u; i < fake_radio_arm_count(); i++) {
		const struct fake_radio_arm *a = fake_radio_arm(i);

		if (a->kind != FAKE_RADIO_ARM_RX || a->rc != RADIANT_RADIO_OK_RC) {
			continue;
		}
		zassert_true(a->t_close - a->t_open <= 20000u,
			     "arm %u ran %u us against a 20000 us ceiling", i,
			     (uint32_t)(a->t_close - a->t_open));
	}
	zassert_true(radiant_sched_pending(31u), NULL);

	assert_every_window_bounded();
	finish();
	fake_radio_caps_mut()->max_window_us = 0u;
}

/* Zero means unbounded - the answer every backend that owns the radio gives.
 * Nothing changes for them: "inert unless asked for" is the entire
 * compatibility claim of this field. */
ZTEST(radiant_sched, test_an_uncapped_backend_is_completely_unaffected)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	uint16_t            lead;

	bring_up();
	zassert_equal(0u, radiant_radio_caps_get()->max_window_us,
		      "the default preset acquired a window ceiling");
	t0 = radiant_radio_now();
	lead = radiant_radio_caps_get()->min_arm_lead_us;

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), search_filter,
		    8u, t0, RADIANT_TIME_NEVER);
	r.chunk_us = 100000u;
	zassert_ok(radiant_sched_request_rx(31u, &r));
	zassert_ok(radiant_sched_tick());

	zassert_equal(t0 + (radiant_time_t)lead + 100000u,
		      fake_radio_arm(0u)->t_close,
		      "an uncapped backend had its chunk shortened");

	assert_every_window_bounded();
	finish();
}

/* ---------------------------------------------------------------------------
 * follow_on_us - reserving the reply
 * ---------------------------------------------------------------------------
 */

/* The reserve reaches the backend, and on a merged window it is the MAXIMUM
 * over members, not the leader's - any member sharing the window may be the
 * one whose frame becomes an acknowledged-data reply, so taking the leader's
 * number would under-reserve exactly when merging did its job. */
ZTEST(radiant_sched, test_the_follow_on_reserve_reaches_the_backend)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	radiant_time_t          open;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;

	/* Two channels whose windows overlap, so they merge; only the second
	 * asks for a reserve. */
	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING),
		    &track_filter[0], 1u, open, open + TEST_WINDOW_US);
	r.follow_on_us = 0u;
	zassert_ok(radiant_sched_request_rx(0u, &r));

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING),
		    &track_filter[1], 1u, open, open + TEST_WINDOW_US);
	r.follow_on_us = 1960u;
	zassert_ok(radiant_sched_request_rx(1u, &r));

	zassert_ok(radiant_sched_tick());

	zassert_equal(1u, fake_radio_arm_count(), "the two windows did not merge");
	zassert_equal(2u, fake_radio_arm(0u)->n_filters, NULL);
	zassert_equal(1960u, fake_radio_arm(0u)->follow_on_us,
		      "a merged window reserved the leader's follow-on rather "
		      "than the largest its members asked for");

	fake_radio_advance_to(open + TEST_WINDOW_US + 1u);

	assert_every_window_bounded();
	finish();
}

/* CRC failures reach only the channel that asked for them, per-channel even
 * though the HAL flag is per-window. A 3-byte search address triggers the
 * matcher on noise several times a second on a quiet bench, so this keeps
 * radiant_search.c able to rank/gate on CRC - why the flag is off by default. */
ZTEST(radiant_sched, test_crc_failures_are_delivered_only_when_asked_for)
{
	struct radiant_sched_rx r;
	radiant_time_t          t0;
	radiant_time_t          open;
	radiant_time_t          close;

	bring_up();
	t0 = radiant_radio_now();
	open = t0 + 100000u;
	close = open + 100000u;

	post_track(0u, open, close);
	zassert_ok(radiant_sched_tick());
	fake_radio_air_noise(open + 1000u, open + 50000u, 6u,
			     FAKE_RADIO_RSSI_NOISE_DBM);
	fake_radio_advance_to(close + 1u);

	zassert_equal(0u, log.n_rx, "noise triggers reached a channel that did "
				    "not ask for them");
	zassert_equal(6u, fake_radio_stats()->crc_fail_suppressed);
	zassert_equal(1u, log.ok_count[0]);

	/* The same noise, with the flag set. */
	open = radiant_radio_now() + 100000u;
	close = open + 100000u;
	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING), &track_filter[0],
		    1u, open, close);
	r.report_crc_fail = true;
	zassert_ok(radiant_sched_request_rx(0u, &r));
	zassert_ok(radiant_sched_tick());
	fake_radio_air_noise(open + 1000u, open + 50000u, 6u,
			     FAKE_RADIO_RSSI_NOISE_DBM);
	fake_radio_advance_to(close + 1u);

	zassert_equal(6u, log.n_rx, "the channel asked for CRC failures and got %u",
		      log.n_rx);
	zassert_equal(RADIANT_RADIO_STATUS_CRC_FAIL, log.rx[0].status);
	zassert_equal(0u, log.rx[0].ch);

	assert_every_window_bounded();
	finish();
}

/* ---------------------------------------------------------------------------
 * Argument checking
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_sched, test_malformed_requests_are_rejected_at_the_door)
{
	struct radiant_sched_rx r;
	struct radiant_sched_tx t;
	radiant_time_t          t0;

	bring_up();
	t0 = radiant_radio_now();

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING), &track_filter[0],
		    1u, t0 + 1000u, t0 + 2000u);
	zassert_equal(RADIANT_RADIO_EINVAL,
		      radiant_sched_request_rx((uint8_t)RADIANT_SCHED_MAX_CHANNELS, &r),
		      "a channel past the ceiling was accepted");
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_sched_request_rx(0u, NULL));

	r.fmt = NULL;
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_sched_request_rx(0u, &r));

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING), &track_filter[0],
		    0u, t0 + 1000u, t0 + 2000u);
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_sched_request_rx(0u, &r),
		      "a window with no filters was accepted");

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING), &track_filter[0],
		    (uint8_t)(RADIANT_SCHED_MAX_FILTERS + 1u), t0 + 1000u, t0 + 2000u);
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_sched_request_rx(0u, &r));

	rx_req_init(&r, radiant_frame_format(RADIANT_FRAME_CFG_TRACKING), &track_filter[0],
		    1u, t0 + 2000u, t0 + 1000u);
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_sched_request_rx(0u, &r),
		      "a window that closes before it opens was accepted");

	memset(&t, 0, sizeof(t));
	t.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	t.t_sync_at = t0 + 100000u;
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_sched_request_tx(0u, &t),
		      "a transmit with no body was accepted");

	/* A transmit with no address is malformed and refused here rather than
	 * passed down: on nRF the address is a register index the previous
	 * operation loaded, so a backend handed one would emit a well-formed
	 * frame addressed to whichever device was last listened for. */
	tx_req_init(&t, 0u, t0 + 100000u);
	t.addr_len = 0u;
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_sched_request_tx(0u, &t),
		      "a transmit with no on-air address was accepted");

	/* And one whose length disagrees with its format: the search geometry's
	 * 3-byte address posted against the 5-byte tracking format. */
	t.addr_len = 3u;
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_sched_request_tx(0u, &t),
		      "an address that disagreed with the format was accepted");

	tx_req_init(&t, 0u, RADIANT_TIME_NEVER);
	zassert_equal(RADIANT_RADIO_EINVAL, radiant_sched_request_tx(0u, &t),
		      "RADIANT_TIME_NEVER was accepted as a transmit instant");

	zassert_equal(0u, fake_radio_arm_count());
	zassert_false(radiant_sched_pending(0u));

	finish();
}
