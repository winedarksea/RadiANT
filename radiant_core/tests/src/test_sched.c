/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work. Written against
 * radiant_core/include/radiant_core/radiant_sched.h, radiant_core/include/radiant_core/radiant_radio_hal.h,
 * radiant_core/tests/fake_radio.h and the measurements in docs/spike-a-results.md
 * and docs/spike-b-part2-results.md. Nothing here derives from sdk-ant, from
 * libant.a, or from any adopter-gated ANT+ device profile document.
 *
 * ---------------------------------------------------------------------------
 * Tests for the radio schedule
 * ---------------------------------------------------------------------------
 * radiant_sched.c is pure logic over radiant_radio_hal.h, so everything about it is
 * testable here and nothing about it needs a board. This suite is written to
 * make the module's two expensive mistakes impossible to ship:
 *
 *   MERGING THAT DOES NOT MERGE. A scheduler that arms one window per channel
 *   still works - it just loses a packet every time two windows overlap, at a
 *   rate that looks exactly like the ~0.4 % collision floor the bench already
 *   characterised. So the merge tests assert on fake_radio_arm_count(): three
 *   overlapping channels must cost ONE arm call, not three, and the frame that
 *   arrives must come back attributed to the right channel and the right one of
 *   that channel's own filters.
 *
 *   A HARDCODED EIGHT. caps.max_filters is 8 on nRF and 2 on EFR32/RAIL, and a
 *   module that assumed 8 would pass every test on the nRF preset and then
 *   build a request the RAIL backend rejects outright. Every merge test here
 *   therefore runs at 2 as well, and every test ends by asserting that the mock
 *   rejected no arm call at all - which is the only way "it hardcoded 8" is
 *   visible before an EFR32 exists.
 *
 * Two more properties are checked at the end of every single test, because they
 * catch a class of bug that no individual assertion does: the radio is idle
 * (nothing was left armed) and fake_radio_viol_count() is zero (nothing was
 * done in a callback that would deadlock a real radio ISR, including making
 * more HAL calls in one callback than the mock's budget allows - which is the
 * check on "a pass is one radiant_radio_now() and one arm call").
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_sched.h>

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

	/* Steady-state driver: re-post the next window from done(), which is
	 * what radiant_channel.c will do and the only way the round-robin fairness
	 * of the scheduler is observable at all. */
	bool       repost;
	radiant_time_t next_open[RADIANT_SCHED_MAX_CHANNELS];

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
 * A well-formed transmit request for a channel.
 *
 * THE ADDRESS COMES FROM track_filter[ch], and that is the assertion rather
 * than a convenience: a channel transmits on the address it listens on, so
 * building the request from the same bytes the receive filter uses is what makes
 * "the scheduler passed the right address to the backend" checkable at all.
 * struct radiant_sched_tx had no address field until the first real backend
 * needed one, and every transmit test in this file was written without one - so
 * the helper exists partly to make sure the next one cannot be.
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
	default:
		break;
	}

	if (log.repost) {
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

/* Build the 32 tracking filters and the 8 search filters out of the frame
 * layer, so a filter and the frame a test puts on the air cannot disagree
 * about byte order by construction. */
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
 * same 15 bytes serve a 5-byte tracking window and a 3-byte search window - the
 * split belongs to the receiver, which is the whole reason the mock stores a
 * byte stream. */
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
 * The end of every test, and the assertions here are worth more than most of
 * the ones above them.
 *
 * rc_etime == 0 is the direct statement of "late arming is rejected rather than
 * silently run late": the scheduler subtracts caps.min_arm_lead_us itself, so
 * the backend must never have had to refuse anything on those grounds. rc_einval
 * and rc_enotsup == 0 is the hardcoded-eight detector, because a window asking
 * for more filters than caps.max_filters is exactly RADIANT_RADIO_EINVAL.
 */
static void finish(void)
{
	const struct fake_radio_stats *st = fake_radio_stats();
	uint8_t                        ch;

	log.repost = false;
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

/* Every accepted window must be finite and well ordered. RADIANT_TIME_NEVER is
 * rejected as a window edge by the HAL, so a scheduler that let a continuous
 * request through unchopped would be caught here rather than by one test. */
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

/*
 * The window goes on the air at the microsecond that was asked for, and the
 * instant is far enough above 2^32 that a uint32_t anywhere in the path
 * truncates it. The mock's clock origin sits one second below the boundary
 * precisely so that every suite crosses it; this test crosses it deliberately
 * and says so, because "the scheduler stores time in the right width" is
 * otherwise only tested by accident.
 */
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
 * THE CENTRAL TEST. Tracked channels whose windows overlap cost ONE hardware
 * window carrying one filter each, and the frame that arrives is still
 * attributed to the one channel it belongs to.
 *
 * The assertion that matters is that the merge happens at all. An unmerged
 * scheduler passes every other assertion in this file and fails only this one -
 * and on the bench it would fail as a fraction of a percent of extra loss,
 * indistinguishable from the collision floor.
 *
 * TWO CHANNELS, NOT THREE, AND THAT IS THE HARDWARE TALKING. This test used to
 * post three and assert a window carrying three filters, which no nRF can arm:
 * on the 5-byte tracking format the device number lives inside the address
 * BASE, the part has one BASE0 and one BASE1, and so a tracking window
 * expresses two device numbers. caps.max_addr_groups is that number, and the
 * third channel gets its own window immediately afterwards rather than being
 * lost - which is what the tail of this test now checks.
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

/*
 * Windows that do not overlap are not merged, however tempting the arithmetic.
 * The negative case is worth a test of its own: a merge rule that ignored the
 * times would pass the test above and would silently widen every window to
 * cover both, which costs receive current and, once the span cap bites, real
 * coverage.
 */
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
 * ADAPTIVE FREQUENCY'S HALF OF THE MERGE RULE (RF-7), AND IT IS THE PRICE RATHER
 * THAN THE FEATURE.
 *
 * ADR 0005 axis 5 buys frequency escape by accepting that a channel which leaves
 * RF 57 leaves the merged window with it: "an adaptive-frequency node costs its
 * own RX window, the same as a second-network channel would. The saving is that
 * only nodes that opted in pay it." That cost is only actually paid if the
 * scheduler REFUSES to merge across frequencies - and a scheduler that merged
 * anyway would arm one window on one of the two frequencies and silently drop
 * every packet from the other, which on a bench is a sensor that stops working
 * when an unrelated node moves.
 *
 * Three channels whose windows all overlap: two on RF 57 and one on RF 26, the
 * middle of the phase's candidate set. The two on 57 still merge - the property
 * axis 5 promises to leave alone for everybody who did not opt in - and the
 * third gets a window of its own.
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

	/*
	 * The same overlap, on 2426 MHz - and deliberately much LONGER, for two
	 * reasons. It overlaps the pair above, so a scheduler that ignored
	 * frequency would fold it in; and it outlives them, so that after the
	 * merged window closes there is still a window left to arm, which is how
	 * "costs its own RX window" is observed rather than asserted. A short
	 * one would simply be reported missed, which proves nothing about
	 * frequency.
	 */
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
 * THE MERGE IS BOUNDED BY caps.max_addr_groups, NOT BY caps.max_filters, ON THE
 * TRACKING FORMAT - and confusing the two is the defect this test was rewritten
 * to pin.
 *
 * Twelve tracked channels want the same window. On the nRF preset TWO of them
 * get it, because the eight logical addresses are one BASE0+AP0 and seven
 * BASE1+AP1..AP7, and on the 5-byte tracking address the device number is
 * inside the BASE. Eight filters is eight addresses; it is not eight device
 * numbers.
 *
 * This test previously asserted eight, and passed, because the mock modelled
 * max_filters and no base at all. The scheduler duly built sets of eight, the
 * real backend refused every one of them with RADIANT_RADIO_ENOTSUP, and the
 * refusal was charged to whichever channel happened to be leading - which on a
 * bench looks like one healthy sensor failing at random.
 *
 * The ten that do not fit are reported RADIANT_SCHED_DONE_MISSED rather than
 * dropped, which is the honest answer: twelve sensors whose slots coincide
 * exactly cannot all be heard at once by this part, and a scheduler that hid it
 * would turn a capacity limit into unexplained loss.
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
 * THE OTHER HALF OF THE SAME FACT, AND THE REASON THE FIX COSTS NOTHING WHERE
 * IT MATTERS MOST.
 *
 * On the 3-byte SEARCH format the address is [A6 C5 devnum_lo]: the group is
 * [A6 C5], every filter shares it, and the prefix is the device-number byte. So
 * eight search filters ride one window with one address group, the sweep
 * geometry is unchanged, and the group cap never fires.
 *
 * Without this test the change above reads as "merging got worse". It did not:
 * merging got correct on the one format where the hardware constrains it, and
 * stayed at eight on the one where the sweep's whole rate depends on it.
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

/*
 * THE SAME SCENARIO ON A BACKEND WITH TWO SYNC WORDS.
 *
 * This is the only test that can catch a hardcoded eight, and it catches it two
 * ways: n_filters is asserted to be 2, and finish() asserts the mock rejected
 * nothing - a request for eight filters at caps.max_filters == 2 is
 * RADIANT_RADIO_EINVAL, so a module that ignored the capability would fail here
 * even if this test's own assertion were deleted.
 */
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

/*
 * A request for more addresses than the backend has matchers is reported once
 * and consumed, not retried for ever.
 *
 * radiant_search.c's sweep asks for caps.max_filters addresses at a time, so on nRF
 * it asks for eight and on RAIL for two. If it ever asks for eight on RAIL, the
 * scheduler must not loop on a request the backend will refuse every time - a
 * livelock inside a radio ISR is the worst way to report a caller's mistake,
 * and it is what fake_radio's advance budget exists to catch.
 */
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

/*
 * A merged window's filter index is translated back into "channel c, filter i
 * of c's own array".
 *
 * This is what radiant_search.c's recovery of devnum_lo rests on: with a 3-byte
 * search address the matched byte never reaches RAM, so which filter matched IS
 * the identity of that byte. A scheduler that handed the merged window's global
 * index straight through would give the search layer the wrong device number
 * for every channel except the first, and the symptom would be a sensor that
 * pairs as a device it is not.
 */
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

/*
 * A channel that opens after its neighbours joins the merge on the next commit
 * rather than a channel period later.
 *
 * Tearing down a window that has not reached its first bit of preamble costs
 * nothing, so there is no reason to make a newly-opened channel wait - and
 * without this, merging would only ever happen in the steady state where every
 * channel re-posts from the same terminal callback, which is exactly the case a
 * test arranges and the bench does not.
 */
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
 * 32 channels
 * ---------------------------------------------------------------------------
 */

/*
 * THIRTY-TWO CHANNELS, ALL CONTENDING FOR THE SAME INSTANT, ON THE BACKEND WITH
 * TWO MATCHERS - and every one of them gets served.
 *
 * This is the worst case the module is sized for: 32 is the serial protocol's
 * ceiling and 2 is the smallest caps.max_filters any planned backend has, so
 * only two channels can be heard per slot and thirty are turned away. A
 * scheduler that always picked the lowest-numbered pending channel would serve
 * channels 0 and 1 for ever and channel 2 would never once be armed, and every
 * other test in this file would still pass.
 *
 * The channels re-post from done() at the measured 249,696 us slot period,
 * which is what radiant_channel.c will do, and it is the only way the rotation is
 * observable.
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
 * The same thirty-two channels on the nRF preset - and the number is TWO here
 * as well, which is the point of running it both ways now.
 *
 * The two presets used to differ: RAIL two, nRF eight. That difference was an
 * artefact of the mock, not of the parts. RAIL fits two because it has two
 * runtime sync words; the nRF fits two because seven of its eight logical
 * addresses share a BASE register and a tracked address carries the device
 * number inside that BASE. Same answer, different reasons, and the reasons are
 * what caps.max_filters and caps.max_addr_groups now say separately.
 *
 * What that costs is real and is stated rather than hidden: thirty-two tracked
 * sensors need sixteen windows per slot rather than four, so this test needs
 * the same nineteen cycles the RAIL one does. ADR 0005's "32 sensors do not
 * cost 32 windows" survives - sixteen is not thirty-two, merging still halves
 * it - but the claim as written was measured against a mock that modelled a
 * radio nobody has.
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
 * offered to the backend and refused there.
 *
 * caps.min_arm_lead_us is the slack budget and the HAL is explicit that a
 * backend never silently runs late - it returns RADIANT_RADIO_ETIME. So the correct
 * behaviour is not "handle ETIME"; it is "never provoke it", and the assertion
 * is that fake_radio recorded no arm call at all. The transmit boundary is
 * tested to the microsecond because a transmit's t_sync cannot be nudged: a
 * late master frame lands in the next slot, and a late acknowledgement is worse.
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

	/*
	 * AND IT WENT OUT ADDRESSED TO CHANNEL 2, not to channel 1 whose
	 * transmit was refused a few lines above.
	 *
	 * This is the assertion the whole file lacked. On the nRF backend the
	 * transmit address is TXADDRESS - an index into BASE/PREFIX registers
	 * that some earlier operation loaded - so a scheduler that forwarded no
	 * address would not fail here; it would emit a well-formed frame
	 * carrying the previous channel's device number. Nothing above the HAL
	 * would see it, and another device might accept it.
	 */
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

/*
 * A window that would still be open when a transmit has to be armed is
 * truncated, not abandoned and not allowed to overrun.
 *
 * There is one radio. The tail of a receive window is worth less than a frame
 * that must go out at an exact instant, so the scheduler gives up the tail -
 * and it gives up exactly enough of it, which is caps.min_arm_lead_us.
 */
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
 * A reply that falls due inside an armed window takes the radio.
 *
 * This is the turnaround, and it is the tightest deadline in the link layer: a
 * tracking channel that hears acknowledged data owes a full 8-byte frame about
 * 1.55 ms later (docs/spike-b-part2-results.md). The request arrives from
 * inside the receive callback of the window that is still armed, so nothing but
 * preemption can serve it.
 *
 * It also produces the late terminal event with no fault injection at all: the
 * aborted window's RADIANT_RADIO_STATUS_ABORTED arrives after the transmit has been
 * armed, carrying the dead operation's id, and the scheduler must recognise it
 * rather than take it for the end of the transmit.
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

/*
 * The same displacement, driven from thread context, so that the preemption
 * path itself is exercised rather than the incidental abort that replacing a
 * channel's own in-flight request performs.
 *
 * The clock is moved INTO the window first, and that is the whole point of the
 * test rather than set dressing: a window that has not opened yet is rebuilt
 * for free and nobody is told anything, while a window that is already
 * listening genuinely loses its tail and its members have to hear about it. The
 * two paths differ in what they do to the members' requests, and only one of
 * them is a preemption.
 */
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

/*
 * An event carrying the id of an operation that has already ended changes
 * nothing.
 *
 * On real hardware the frame was already in the receiver's pipeline when the
 * window ended, so the event arrives after the core has moved on. The op id
 * exists for exactly this, and the failure it prevents is not a crash: it is a
 * frame routed to whichever channel happens to occupy that filter slot in the
 * window armed afterwards, and a terminal event counted twice, retiring a
 * window that is still open.
 */
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

/*
 * RADIANT_TIME_NEVER is not a valid window edge, so "receive until I say stop" has
 * to be a chain of bounded windows that something re-arms. That something is
 * this module, and the translation lives nowhere else in radiant_core.
 *
 * The test also checks the two properties that make it a *background* scan: it
 * fills the gap ahead of a tracked window, and it gets out of the way exactly
 * when the tracked window needs the radio.
 */
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

/*
 * A scan chunk is reported with the bounds it ACTUALLY got, not the ones its
 * request asked for.
 *
 * This is the whole reason armed() exists. A wildcard sweep accounts its dwell
 * in listening time, and the gap a chunk is cut to is computed here, from
 * pending work the sweep's own layer cannot see. Told only what it proposed, it
 * would credit a 60 ms chunk as the 250 ms it asked for, and "certain within one
 * sweep" would quietly become "certain within four".
 */
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

/*
 * A scan chunk displaced by a tracked window that turned up after it was armed
 * is REPORTED, and the request survives it.
 *
 * Both halves matter and they pull in opposite directions. The request has to
 * survive, or a background scan would be consumed by the first tracked channel
 * to want the radio. But the chunk that was running really did stop early, and
 * an owner told nothing at all would go on believing it was listening - which
 * for a sweep crediting its dwell in listening time is how a set gets credited
 * time the radio spent somewhere else.
 */
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

/*
 * RADIANT_RX_STOP_ON_FIRST survives an unmerged window and is dropped from a merged
 * one.
 *
 * The HAL forbids it on a window carrying more than one filter, because more
 * than one master may transmit inside such a window; a scheduler that passed it
 * through would have its merged windows rejected outright with
 * RADIANT_RADIO_EINVAL, which finish() would also catch. The interesting half is
 * the other one: it must still be set when there was nothing to merge, or the
 * receive-current saving it exists for is silently lost.
 */
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

/*
 * RADIANT_RADIO_EBUSY leaves the request pending.
 *
 * The HAL says an arm from thread context races the callback that may be about
 * to consume the operation slot, that the backend resolves the race by
 * returning EBUSY, and that the core must handle it rather than assume it
 * cannot happen. Handling it means neither losing the request nor spinning on
 * it: the next commit tries again.
 */
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

/*
 * A backend that says it could not complete an operation ends the request
 * rather than leaving it in flight for ever. RADIANT_RADIO_STATUS_FAILED is the one
 * terminal status nothing else in the mock produces, and a scheduler that had
 * no case for it would hold the channel in flight and never schedule it again.
 */
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

/*
 * CRC failures reach only the channel that asked for them, and asking is
 * per-channel even though the HAL flag is per-window.
 *
 * A 3-byte search address triggers the matcher on noise several times a second
 * on a quiet bench, so this is not a diagnostic nicety: it is what keeps
 * radiant_search.c able to rank and gate on CRC, and it is why the flag is off by
 * default.
 */
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

	/*
	 * A TRANSMIT WITH NO ADDRESS IS MALFORMED, and it is refused here rather
	 * than passed down, because a backend handed one has no honest move
	 * left. On nRF the address is a register index the previous operation
	 * loaded, so the frame goes out addressed to whichever device was last
	 * listened for - well-formed, on the air, and wrong.
	 */
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
