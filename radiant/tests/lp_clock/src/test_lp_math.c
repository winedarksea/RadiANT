/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, written against
 * radiant/src/radiant_radio_nrf_lp.h.
 *
 * The low-power timebase's arithmetic, one property at a time: the counter
 * formats, the fraction carry, wrap at 24 and 52 bits, the monotonic clamp,
 * the release choice between the LF image and the TIMER's capture, the wake's
 * fraction carry, the wake compare's rounding (never late), and the sleep
 * decision's boundaries. test_lp_drift.c runs them together for 100 000
 * cycles.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/ztest.h>

#include "radiant_radio_nrf_lp.h"

static struct radiant_lp_tb rtc;
static struct radiant_lp_tb grtc;

static void *lp_math_setup(void)
{
	zassert_true(radiant_lp_tb_init(&rtc, RADIANT_LP_RTC_NUM,
					RADIANT_LP_RTC_SHIFT,
					RADIANT_LP_RTC_WIDTH));
	zassert_true(radiant_lp_tb_init(&grtc, RADIANT_LP_GRTC_NUM,
					RADIANT_LP_GRTC_SHIFT,
					RADIANT_LP_GRTC_WIDTH));
	return NULL;
}

ZTEST_SUITE(radiant_lp_math, NULL, lp_math_setup, NULL, NULL, NULL);

/* xorshift32: deterministic, so a failure reproduces. */
static uint32_t rng = 0x2545F491u;

static uint32_t rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static uint64_t rnd64(void)
{
	return ((uint64_t)rnd() << 32) | rnd();
}

/* --- Formats --- */

ZTEST(radiant_lp_math, test_formats_are_exact)
{
	/* 15625 / 2^9 us is exactly 1e6 / 32768 us. */
	zassert_equal((uint64_t)RADIANT_LP_RTC_NUM * 32768u,
		      UINT64_C(1000000) << RADIANT_LP_RTC_SHIFT);
	zassert_equal(rtc.mask, 0xFFFFFFu);
	zassert_equal(grtc.mask, (UINT64_C(1) << 52) - 1u);
	/* 32768 ticks are exactly one second on the RTC, 1e6 on the GRTC. */
	zassert_equal(radiant_lp_ticks_to_us(&rtc, 32768u), 1000000u);
	zassert_equal(radiant_lp_ticks_to_us(&grtc, 1000000u), 1000000u);
	/* One RTC tick is 30.517578125 us: 30 toward zero. */
	zassert_equal(radiant_lp_ticks_to_us(&rtc, 1u), 30u);
	/* The longest trusted distance is half the range. */
	zassert_equal(radiant_lp_tb_half(&rtc), 0x7FFFFFu);
}

ZTEST(radiant_lp_math, test_format_init_refuses_what_64_bits_cannot_carry)
{
	struct radiant_lp_tb tb = { .mask = 0xABu, .num = 7u };

	zassert_false(radiant_lp_tb_init(&tb, 0u, 0u, 24u));
	zassert_false(radiant_lp_tb_init(&tb, 1u, 32u, 24u));
	zassert_false(radiant_lp_tb_init(&tb, 1u, 0u, 1u));
	zassert_false(radiant_lp_tb_init(&tb, 1u, 0u, 65u));
	/* 50 + bitlength(15625) = 50 + 14 = 64 bits: refused; 49 fits. */
	zassert_false(radiant_lp_tb_init(&tb, 15625u, 9u, 50u));
	zassert_equal(tb.mask, 0xABu, "a refused init must not write");
	zassert_equal(tb.num, 7u);
	zassert_true(radiant_lp_tb_init(&tb, 15625u, 9u, 49u));
	zassert_true(radiant_lp_tb_init(&tb, 1u, 0u, 62u));
}

/* --- The fraction carry --- */

/*
 * Any split of a distance, converted piece by piece with the carry, sums to
 * the one-shot conversion of the whole, and leaves the whole's remainder.
 * Without the carry the same pieces lose about half a microsecond each.
 */
ZTEST(radiant_lp_math, test_carry_piecewise_sum_equals_one_shot)
{
	struct radiant_lp_conv cv = { 0 };
	uint64_t total_ticks = 0;
	uint64_t sum_us = 0;
	uint64_t naive_us = 0;

	for (uint32_t i = 0; i < 1000000u; i++) {
		uint64_t piece = rnd() % 20000u;

		total_ticks += piece;
		sum_us += radiant_lp_conv_step(&rtc, &cv, piece);
		naive_us += radiant_lp_ticks_to_us(&rtc, piece);
		if ((i & 0xFFFFu) == 0u) {
			zassert_equal(sum_us,
				      radiant_lp_ticks_to_us(&rtc, total_ticks),
				      "piecewise sum left the one-shot at %u", i);
		}
	}
	zassert_equal(sum_us, radiant_lp_ticks_to_us(&rtc, total_ticks));
	zassert_equal(cv.frac, (uint32_t)((total_ticks * RADIANT_LP_RTC_NUM) &
					  ((1u << RADIANT_LP_RTC_SHIFT) - 1u)));
	/* The loss the carry exists to prevent: ~0.5 us a piece, 1e6 pieces. */
	zassert_true(sum_us - naive_us > 400000u, "naive loss %llu",
		     (unsigned long long)(sum_us - naive_us));
}

ZTEST(radiant_lp_math, test_carry_tick_by_tick_closes_on_whole_seconds)
{
	struct radiant_lp_conv cv = { 0 };
	uint64_t sum_us = 0;

	/* 512 single ticks are exactly 15625 us; 32768 exactly a second. */
	for (uint32_t i = 1; i <= 32768u; i++) {
		sum_us += radiant_lp_conv_step(&rtc, &cv, 1u);
		if (i == 512u) {
			zassert_equal(sum_us, 15625u);
			zassert_equal(cv.frac, 0u);
		}
	}
	zassert_equal(sum_us, 1000000u);
	zassert_equal(cv.frac, 0u);
}

ZTEST(radiant_lp_math, test_carry_is_identity_on_the_grtc)
{
	struct radiant_lp_conv cv = { 0 };

	for (uint32_t i = 0; i < 1000u; i++) {
		uint64_t t = rnd64() & grtc.mask;

		zassert_equal(radiant_lp_conv_step(&grtc, &cv, t), t);
		zassert_equal(cv.frac, 0u);
	}
}

/* --- Counter wrap --- */

ZTEST(radiant_lp_math, test_wrap_at_24_bits)
{
	struct radiant_lp_clock c;

	zassert_equal(radiant_lp_ticks_since(&rtc, 0xFFFFF0u, 0x000010u), 0x20u);
	/* An extended (wider than 24-bit) value is reduced first. */
	zassert_equal(radiant_lp_ticks_since(&rtc, UINT64_C(0x12FFFFFFF0),
					     UINT64_C(0x1300000010)), 0x20u);

	radiant_lp_clock_init(&c);
	c.anchor_q = UINT64_C(5000000) << RADIANT_LP_RTC_SHIFT;
	c.anchor_ticks = 0xFFFFF0u;
	/* 0x20 ticks across the wrap: 32 * 15625 / 512 = 976.5625 us. */
	zassert_equal(radiant_lp_asleep_q(&rtc, &c, 0x10u),
		      c.anchor_q + 32u * RADIANT_LP_RTC_NUM);
	zassert_equal(radiant_lp_asleep_us(&rtc, &c, 0x10u), 5000976u);
	/* The same reading given as an extended counter. */
	zassert_equal(radiant_lp_asleep_us(&rtc, &c, UINT64_C(0x7000000010)),
		      5000976u);

	/* Reached: at or past, by less than half the range, across the wrap. */
	zassert_true(radiant_lp_tick_reached(&rtc, 0x10u, 0xFFFFF0u));
	zassert_true(radiant_lp_tick_reached(&rtc, 0xFFFFF0u, 0xFFFFF0u));
	zassert_false(radiant_lp_tick_reached(&rtc, 0xFFFFEFu, 0xFFFFF0u));
	zassert_false(radiant_lp_tick_reached(&rtc, 0xFFFFF0u, 0x10u));
}

ZTEST(radiant_lp_math, test_wrap_at_52_bits)
{
	struct radiant_lp_clock c;
	uint64_t top = (UINT64_C(1) << 52) - 5u;

	zassert_equal(radiant_lp_ticks_since(&grtc, top, 3u), 8u);
	zassert_equal(radiant_lp_ticks_since(&grtc, top | (UINT64_C(0xF) << 60),
					     3u | (UINT64_C(0x3) << 56)), 8u);

	radiant_lp_clock_init(&c);
	c.anchor_q = UINT64_C(123456789);
	c.anchor_ticks = top;
	zassert_equal(radiant_lp_asleep_us(&grtc, &c, 3u), 123456797u);
	zassert_true(radiant_lp_tick_reached(&grtc, 3u, top));
	zassert_false(radiant_lp_tick_reached(&grtc, top, 3u));
}

ZTEST(radiant_lp_math, test_compare_in_time_needs_the_lead_and_the_future)
{
	uint32_t lead = RADIANT_LP_RTC_MIN_LEAD_TICKS;

	zassert_false(radiant_lp_compare_in_time(&rtc, 100u, 100u + lead - 1u, lead));
	zassert_true(radiant_lp_compare_in_time(&rtc, 100u, 100u + lead, lead));
	/* Across the wrap. */
	zassert_true(radiant_lp_compare_in_time(&rtc, 0xFFFFFFu, lead - 1u, lead));
	zassert_false(radiant_lp_compare_in_time(&rtc, 0xFFFFFFu, lead - 2u, lead));
	/* Behind the counter is the past, not 512 s ahead. */
	zassert_false(radiant_lp_compare_in_time(&rtc, 100u, 99u, lead));
	/* Exactly half the range ahead is the last trusted distance. */
	zassert_true(radiant_lp_compare_in_time(&rtc, 0u, 0x7FFFFFu, lead));
	zassert_false(radiant_lp_compare_in_time(&rtc, 0u, 0x800000u, lead));
	/* GRTC, its own lead, across its own wrap. */
	zassert_true(radiant_lp_compare_in_time(&grtc, grtc.mask, 200u,
						RADIANT_LP_GRTC_MIN_LEAD_TICKS));
	zassert_false(radiant_lp_compare_in_time(&grtc, grtc.mask, 50u,
						 RADIANT_LP_GRTC_MIN_LEAD_TICKS));
}

/* --- The monotonic clamp --- */

ZTEST(radiant_lp_math, test_mono_holds_the_high_water_mark)
{
	struct radiant_lp_clock c;

	radiant_lp_clock_init(&c);
	zassert_equal(radiant_lp_mono(&c, 1000u), 1000u);
	zassert_equal(radiant_lp_mono(&c, 999u), 1000u, "went backwards");
	zassert_equal(c.hwm_us, 1000u);
	zassert_equal(radiant_lp_mono(&c, 1000u), 1000u);
	zassert_equal(radiant_lp_mono(&c, 1001u), 1001u);
	zassert_equal(c.hwm_us, 1001u);
}

/*
 * A release whose LF image sits just below the TIMER's last reading moves the
 * raw timebase back; every value handed out stands still instead, then moves
 * on once the LF conversion passes it.
 */
ZTEST(radiant_lp_math, test_release_step_back_is_held_by_the_clamp)
{
	struct radiant_lp_clock c;
	uint64_t wake_us, prev, now;
	int64_t diff;

	radiant_lp_clock_init(&c);
	c.anchor_q = UINT64_C(10000000) << RADIANT_LP_RTC_SHIFT;
	c.anchor_ticks = 1000u;
	wake_us = radiant_lp_wake(&rtc, &c, 1000u, true);
	zassert_equal(wake_us, 10000000u);

	/* 100 ticks later the LF image is 10003051.7578125 us; the TIMER read
	 * 10003053 at that edge (1.24 us ahead), the last awake value handed out.
	 */
	prev = radiant_lp_clock_now(&rtc, &c, false, 10003053u, 0u);
	zassert_true(radiant_lp_release(&rtc, &c, 1100u, 10003053u,
					RADIANT_LP_RELEASE_TOL_US, &diff));
	zassert_true(diff < 0);
	zassert_equal(radiant_lp_asleep_us(&rtc, &c, 1100u), 10003051u);
	now = radiant_lp_clock_now(&rtc, &c, true, 0u, 1100u);
	zassert_equal(now, prev, "a reading ran backwards past the clamp");
	/* One tick later the conversion is ahead again. */
	now = radiant_lp_clock_now(&rtc, &c, true, 0u, 1101u);
	zassert_equal(now, 10003082u);
}

/* --- Release and wake --- */

ZTEST(radiant_lp_math, test_first_release_takes_the_timer)
{
	struct radiant_lp_clock c;
	int64_t diff = 77;

	radiant_lp_clock_init(&c);
	/* TIMER free-running since boot: no wake edge to measure from. */
	zassert_false(radiant_lp_release(&rtc, &c, 0x123456u, 4567890u,
					 RADIANT_LP_RELEASE_TOL_US, &diff));
	zassert_equal(diff, 0);
	zassert_equal(c.anchor_q, UINT64_C(4567890) << RADIANT_LP_RTC_SHIFT);
	zassert_equal(c.anchor_ticks, 0x123456u);
	zassert_equal(radiant_lp_asleep_us(&rtc, &c, 0x123456u), 4567890u);
	zassert_false(c.timer_at_anchor);
}

ZTEST(radiant_lp_math, test_wake_carries_the_fraction_into_the_release)
{
	struct radiant_lp_clock c;
	uint64_t wake_us;
	int64_t diff;

	radiant_lp_clock_init(&c);
	c.anchor_q = UINT64_C(2000000) << RADIANT_LP_RTC_SHIFT;
	c.anchor_ticks = 0u;

	/* Wake 3 ticks on: 91.552734375 us, i.e. 91 us + 283/512. */
	wake_us = radiant_lp_wake(&rtc, &c, 3u, true);
	zassert_equal(wake_us, 2000091u);
	zassert_equal(c.awake_frac, (3u * RADIANT_LP_RTC_NUM) & 511u);
	zassert_equal(c.awake_frac, 283u);
	zassert_true(c.timer_at_anchor);

	/* Release 7 ticks after the wake, the TIMER having counted 213 us from
	 * zero: LF image 2000091 + 213.623046875 + 0.552734375 = 2000305.17...,
	 * TIMER reading (213 + 2000091) + 283/512 = 2000304.55...; 0.62 us apart.
	 */
	zassert_true(radiant_lp_release(&rtc, &c, 10u, 2000091u + 213u,
					RADIANT_LP_RELEASE_TOL_US, &diff));
	zassert_equal(c.anchor_q, (UINT64_C(2000000) << 9) + 10u * 15625u,
		      "the release is the exact LF image of tick 10");
	zassert_equal(diff, (int64_t)((UINT64_C(2000000) << 9) + 10u * 15625u) -
			    (int64_t)(((UINT64_C(2000304)) << 9) + 283u));
	zassert_equal(c.awake_frac, 0u);
	zassert_false(c.timer_at_anchor);
}

ZTEST(radiant_lp_math, test_release_falls_back_to_the_timer_beyond_tolerance)
{
	struct radiant_lp_clock c;
	uint64_t wake_us, lf_us;
	int64_t diff;

	radiant_lp_clock_init(&c);
	c.anchor_q = UINT64_C(1000000) << RADIANT_LP_RTC_SHIFT;
	c.anchor_ticks = 0u;
	wake_us = radiant_lp_wake(&rtc, &c, 0u, true);
	zassert_equal(wake_us, 1000000u);

	/* A 1 s linger: 32768 ticks are 1000000 us in the LF domain; the HF
	 * TIMER, 50 ppm fast, counted 1000050. Taken from the TIMER: no step.
	 */
	lf_us = radiant_lp_asleep_us(&rtc, &c, 32768u);
	zassert_equal(lf_us, 2000000u);
	zassert_false(radiant_lp_release(&rtc, &c, 32768u, 2000050u,
					 RADIANT_LP_RELEASE_TOL_US, &diff));
	zassert_equal(diff, -(INT64_C(50) << 9));
	zassert_equal(radiant_lp_asleep_us(&rtc, &c, 32768u), 2000050u);

	/* Exactly at the tolerance is taken; one q unit beyond is not. */
	radiant_lp_clock_init(&c);
	(void)radiant_lp_wake(&rtc, &c, 0u, true);
	/* 512 ticks = 15625 us exactly in the LF domain. */
	zassert_true(radiant_lp_release(&rtc, &c, 512u,
					15625u + RADIANT_LP_RELEASE_TOL_US,
					RADIANT_LP_RELEASE_TOL_US, NULL));
	radiant_lp_clock_init(&c);
	(void)radiant_lp_wake(&rtc, &c, 0u, true);
	zassert_true(radiant_lp_release(&rtc, &c, 512u,
					15625u - RADIANT_LP_RELEASE_TOL_US,
					RADIANT_LP_RELEASE_TOL_US, NULL));

	/* In q units. 313 ticks are 4890625 q, which is 1 mod 512, so a TIMER
	 * reading of 9549 us (4889088 q) is tol + 1 q below the LF image and
	 * 9550 is within it. 199 ticks are 3109375 q (511 mod 512): 6076 us is
	 * tol + 1 q above, 6075 within.
	 */
	zassert_equal(RADIANT_LP_RELEASE_TOL_US, 3u, "the figures below assume 3");
	radiant_lp_clock_init(&c);
	(void)radiant_lp_wake(&rtc, &c, 0u, true);
	zassert_false(radiant_lp_release(&rtc, &c, 313u, 9549u,
					 RADIANT_LP_RELEASE_TOL_US, NULL));
	radiant_lp_clock_init(&c);
	(void)radiant_lp_wake(&rtc, &c, 0u, true);
	zassert_true(radiant_lp_release(&rtc, &c, 313u, 9550u,
					RADIANT_LP_RELEASE_TOL_US, NULL));
	radiant_lp_clock_init(&c);
	(void)radiant_lp_wake(&rtc, &c, 0u, true);
	zassert_false(radiant_lp_release(&rtc, &c, 199u, 6076u,
					 RADIANT_LP_RELEASE_TOL_US, NULL));
	radiant_lp_clock_init(&c);
	(void)radiant_lp_wake(&rtc, &c, 0u, true);
	zassert_true(radiant_lp_release(&rtc, &c, 199u, 6075u,
					RADIANT_LP_RELEASE_TOL_US, NULL));

	/* The wake's carried fraction is part of the TIMER's reading: it moves
	 * both candidates alike (so the choice above does not depend on it),
	 * and the TIMER's candidate, when taken, keeps it.
	 */
	radiant_lp_clock_init(&c);
	c.anchor_q = 1u;
	(void)radiant_lp_wake(&rtc, &c, 0u, true);
	zassert_equal(c.awake_frac, 1u);
	zassert_false(radiant_lp_release(&rtc, &c, 313u, 9549u,
					 RADIANT_LP_RELEASE_TOL_US, NULL));
	zassert_equal(c.anchor_q, (UINT64_C(9549) << 9) + 1u);
}

ZTEST(radiant_lp_math, test_software_started_wake_carries_nothing)
{
	struct radiant_lp_clock c;

	radiant_lp_clock_init(&c);
	c.anchor_q = 0u;
	c.anchor_ticks = 0u;
	/* A late anchor's recovery: the TIMER was started in software. */
	zassert_equal(radiant_lp_wake(&rtc, &c, 3u, false), 91u);
	zassert_equal(c.awake_frac, 0u);
	zassert_false(c.timer_at_anchor);
	zassert_false(radiant_lp_release(&rtc, &c, 10u, 300u,
					 RADIANT_LP_RELEASE_TOL_US, NULL),
		      "no wake edge to measure from: the TIMER's reading");
	zassert_equal(c.anchor_q, UINT64_C(300) << 9);
}

/* --- The wake compare: never late, and no earlier than it must be --- */

static void check_wake_compare(const struct radiant_lp_tb *tb, uint32_t n)
{
	struct radiant_lp_clock c;

	for (uint32_t i = 0; i < n; i++) {
		uint64_t cmp, d, q_at, wake_q;
		uint64_t wake_at;
		enum radiant_lp_wake_rc rc;

		radiant_lp_clock_init(&c);
		/* Any anchor, with any fraction, anywhere in the counter. */
		c.anchor_q = rnd64() & ((UINT64_C(1) << 50) - 1u);
		c.anchor_ticks = rnd64() & tb->mask;
		/* Up to ~8.4 s on. */
		wake_at = (c.anchor_q >> tb->shift) + (rnd() & 0x7FFFFFu);
		wake_q = wake_at << tb->shift;
		if (wake_q < c.anchor_q) {
			continue; /* PAST, below */
		}
		rc = radiant_lp_wake_compare(tb, &c, wake_at,
					     radiant_lp_tb_half(tb), &cmp);
		zassert_equal(rc, RADIANT_LP_WAKE_OK);
		d = radiant_lp_ticks_since(tb, c.anchor_ticks, cmp);
		q_at = radiant_lp_asleep_q(tb, &c, cmp);
		zassert_equal(q_at, c.anchor_q + d * tb->num);
		zassert_true(q_at <= wake_q, "wake compare is late");
		zassert_true(q_at + tb->num > wake_q,
			     "wake compare is a whole tick earlier than needed");
		zassert_true(radiant_lp_asleep_us(tb, &c, cmp) <= wake_at);
	}
}

ZTEST(radiant_lp_math, test_wake_compare_rounds_toward_earlier_rtc)
{
	check_wake_compare(&rtc, 200000u);
}

ZTEST(radiant_lp_math, test_wake_compare_rounds_toward_earlier_grtc)
{
	check_wake_compare(&grtc, 200000u);
}

ZTEST(radiant_lp_math, test_wake_compare_edges)
{
	struct radiant_lp_clock c;
	uint64_t cmp;

	radiant_lp_clock_init(&c);
	c.anchor_q = UINT64_C(1000) << RADIANT_LP_RTC_SHIFT;
	c.anchor_ticks = 0xFFFFFEu;

	/* A wake exactly on a tick's exact time is that tick: 512 ticks on is
	 * exactly 15625 us; across the wrap.
	 */
	zassert_equal(radiant_lp_wake_compare(&rtc, &c, 1000u + 15625u,
					      rtc.mask, &cmp),
		      RADIANT_LP_WAKE_OK);
	zassert_equal(cmp, (0xFFFFFEu + 512u) & 0xFFFFFFu);
	/* One us earlier is the tick before. */
	zassert_equal(radiant_lp_wake_compare(&rtc, &c, 1000u + 15624u,
					      rtc.mask, &cmp),
		      RADIANT_LP_WAKE_OK);
	zassert_equal(cmp, (0xFFFFFEu + 511u) & 0xFFFFFFu);
	/* At the anchor: the anchor tick. Before it: PAST. */
	zassert_equal(radiant_lp_wake_compare(&rtc, &c, 1000u, rtc.mask, &cmp),
		      RADIANT_LP_WAKE_OK);
	zassert_equal(cmp, 0xFFFFFEu);
	zassert_equal(radiant_lp_wake_compare(&rtc, &c, 999u, rtc.mask, &cmp),
		      RADIANT_LP_WAKE_PAST);
	/* Beyond max_ticks, and beyond half the range whatever max_ticks says. */
	zassert_equal(radiant_lp_wake_compare(&rtc, &c, 1000u + 15625u, 100u,
					      &cmp),
		      RADIANT_LP_WAKE_CLAMPED);
	zassert_equal(cmp, (0xFFFFFEu + 100u) & 0xFFFFFFu);
	zassert_equal(radiant_lp_wake_compare(&rtc, &c, UINT64_C(600000000),
					      UINT64_MAX, &cmp),
		      RADIANT_LP_WAKE_CLAMPED);
	zassert_equal(cmp, (0xFFFFFEu + 0x7FFFFFu) & 0xFFFFFFu);
	/* No operation at all: clamped, not an overflow. */
	zassert_equal(radiant_lp_wake_compare(&rtc, &c, RADIANT_LP_NO_OP,
					      UINT64_MAX, &cmp),
		      RADIANT_LP_WAKE_CLAMPED);
	zassert_equal(cmp, (0xFFFFFEu + 0x7FFFFFu) & 0xFFFFFFu);
}

/* --- Decisions --- */

ZTEST(radiant_lp_math, test_should_sleep_boundaries)
{
	const uint64_t lead = 2112u, lf = 214u;
	const uint32_t keep = 3000u;
	const uint64_t now = UINT64_C(5000000);
	const uint64_t all = lead + lf + keep;

	/* Strictly more than the whole budget: wake lead + LF overhead + keep. */
	zassert_false(radiant_lp_should_sleep(now, now + all, lead, lf, keep, 0u));
	zassert_true(radiant_lp_should_sleep(now, now + all + 1u, lead, lf, keep, 0u));
	/* The LF overhead is part of it: without it this gap would sleep. */
	zassert_false(radiant_lp_should_sleep(now, now + lead + keep + 1u, lead,
					      lf, keep, 0u));
	/* An operation now, or already due, never sleeps. */
	zassert_false(radiant_lp_should_sleep(now, now, lead, lf, keep, 0u));
	zassert_false(radiant_lp_should_sleep(now, now - 1u, lead, lf, keep, 0u));
	/* Nothing pending sleeps. */
	zassert_true(radiant_lp_should_sleep(now, RADIANT_LP_NO_OP, lead, lf, keep, 0u));
	/* Lingering: not before the deadline, from it on. */
	zassert_false(radiant_lp_should_sleep(now, RADIANT_LP_NO_OP, lead, lf, keep,
					      now + 1u));
	zassert_false(radiant_lp_should_sleep(now, now + 1000000u, lead, lf, keep,
					      now + 1u));
	zassert_true(radiant_lp_should_sleep(now, now + 1000000u, lead, lf, keep,
					     now));
	/* Zero budget: any gap at all. */
	zassert_true(radiant_lp_should_sleep(now, now + 1u, 0u, 0u, 0u, 0u));
	/* No 32-bit overflow in the sum. */
	zassert_false(radiant_lp_should_sleep(now, now + UINT32_MAX, UINT32_MAX,
					      UINT32_MAX, UINT32_MAX, 0u));
}

ZTEST(radiant_lp_math, test_lead_budgets_round_up)
{
	/* A tick in whole us, up: 30.5 -> 31 on the RTC, 1 on the GRTC. */
	zassert_equal(radiant_lp_tick_us_ceil(&rtc), 31u);
	zassert_equal(radiant_lp_tick_us_ceil(&grtc), 1u);
	/* arm + hfxo + two ticks of rounding. */
	zassert_equal(radiant_lp_wake_lead_us(&rtc, 300u, 1750u), 300u + 1750u + 62u);
	zassert_equal(radiant_lp_wake_lead_us(&grtc, 300u, 1750u), 300u + 1750u + 2u);
	/* 2 * min_lead + 1 ticks: 7 RTC ticks = 213.6 us -> 214; 217 GRTC. */
	zassert_equal(radiant_lp_lf_overhead_us(&rtc, RADIANT_LP_RTC_MIN_LEAD_TICKS),
		      214u);
	zassert_equal(radiant_lp_lf_overhead_us(&grtc, RADIANT_LP_GRTC_MIN_LEAD_TICKS),
		      217u);
	/* The release edge: min_lead on, across the raw wrap. */
	zassert_equal(radiant_lp_release_edge(&rtc, 0xFFFFFEu,
					      RADIANT_LP_RTC_MIN_LEAD_TICKS), 1u);
}

ZTEST(radiant_lp_math, test_linger_pushes_out_never_in)
{
	zassert_equal(radiant_lp_linger(0u, 1000u, 1000000u), 1001000u);
	zassert_equal(radiant_lp_linger(5000000u, 1000u, 1000000u), 5000000u);
	zassert_equal(radiant_lp_linger(1001000u, 2000u, 1000000u), 1002000u);
}

ZTEST(radiant_lp_math, test_wake_at_and_late_by)
{
	zassert_equal(radiant_lp_wake_at(10000u, 1700u), 8300u);
	zassert_equal(radiant_lp_wake_at(1000u, 1700u), 0u);
	zassert_equal(radiant_lp_wake_at(RADIANT_LP_NO_OP, 1700u),
		      RADIANT_LP_NO_OP - 1700u);

	/* Ready exactly at the operation is in time; a microsecond more is late. */
	zassert_equal(radiant_lp_late_by_us(9800u, 10000u, 200u), 0u);
	zassert_equal(radiant_lp_late_by_us(9801u, 10000u, 200u), 1u);
	zassert_equal(radiant_lp_late_by_us(12000u, 10000u, 200u), 2200u);
	zassert_equal(radiant_lp_late_by_us(0u, 10000u, 200u), 0u);
}

/* --- Extended counters, and the range guard --- */

ZTEST(radiant_lp_math, test_extended_counter_is_never_masked)
{
	struct radiant_lp_tb ext;
	struct radiant_lp_clock c;
	uint64_t cmp;

	zassert_true(radiant_lp_tb_init(&ext, RADIANT_LP_RTC_NUM,
					RADIANT_LP_RTC_SHIFT,
					RADIANT_LP_EXTENDED_WIDTH));
	zassert_equal(ext.mask, UINT64_MAX);
	/* 63 - bitlength(15625) = 49 bits of distance. */
	zassert_equal(radiant_lp_tb_half(&ext), (UINT64_C(1) << 49) - 1u);

	/* Past the 24-bit RTC wrap, the extended count just carries on. */
	radiant_lp_clock_init(&c);
	c.anchor_q = 0u;
	c.anchor_ticks = UINT64_C(0x3FFFFF0);
	zassert_equal(radiant_lp_ticks_since(&ext, c.anchor_ticks,
					     UINT64_C(0x4000010)), 0x20u);
	zassert_equal(radiant_lp_asleep_us(&ext, &c, UINT64_C(0x4000010)), 976u);
	/* A compare comes back absolute, as z_nrf_rtc_timer_set() takes it:
	 * 600 s on is beyond any 24-bit value.
	 */
	zassert_equal(radiant_lp_wake_compare(&ext, &c, UINT64_C(600000000),
					      UINT64_MAX, &cmp),
		      RADIANT_LP_WAKE_OK);
	zassert_equal(cmp, UINT64_C(0x3FFFFF0) + UINT64_C(19660800));
	/* Reached means at or after; before is not, by any amount. */
	zassert_true(radiant_lp_tick_reached(&ext, cmp, cmp));
	zassert_true(radiant_lp_tick_reached(&ext, cmp + 1000000u, cmp));
	zassert_false(radiant_lp_tick_reached(&ext, cmp - 1u, cmp));
	zassert_true(radiant_lp_compare_in_time(&ext, cmp - 3u, cmp, 3u));
	zassert_false(radiant_lp_compare_in_time(&ext, cmp - 2u, cmp, 3u));
	zassert_false(radiant_lp_compare_in_time(&ext, cmp + 1u, cmp, 3u));
}

/*
 * A missed compare. On the nRF52 RTC an equality compare whose value the
 * counter has passed fires one counter period (512 s) later. With an extended
 * counter the overrun is visible as distance; with a raw 24-bit one it is
 * visible only past half the range, and before that it is indistinguishable
 * from a short sleep - which is why extended is the recommended mode.
 */
ZTEST(radiant_lp_math, test_missed_compare_is_refused_not_wrapped)
{
	struct radiant_lp_tb ext;
	struct radiant_lp_clock c;
	uint64_t hwm;

	zassert_true(radiant_lp_tb_init(&ext, RADIANT_LP_RTC_NUM,
					RADIANT_LP_RTC_SHIFT,
					RADIANT_LP_EXTENDED_WIDTH));

	/* Raw: 300 s after the anchor is past half the range. */
	radiant_lp_clock_init(&c);
	c.anchor_q = UINT64_C(1000000) << RADIANT_LP_RTC_SHIFT;
	c.anchor_ticks = 0x100000u;
	hwm = radiant_lp_clock_now(&rtc, &c, true, 0u, 0x100000u + 32768u);
	zassert_equal(hwm, 2000000u);
	zassert_true(radiant_lp_asleep_ok(&rtc, &c, 0x100000u + 0x7FFFFFu));
	zassert_false(radiant_lp_asleep_ok(&rtc, &c, 0x100000u + 300u * 32768u));
	zassert_equal(radiant_lp_clock_now(&rtc, &c, true, 0u,
					   0x100000u + 300u * 32768u), hwm,
		      "an out-of-range reading must hold, not convert");
	zassert_equal(c.range_errors, 1u);
	/* A raw reading BEFORE the anchor wraps to ~512 s ahead: refused too. */
	zassert_false(radiant_lp_asleep_ok(&rtc, &c, 0x100000u - 1u));

	/* Extended: before the anchor is refused at any distance, and a 512 s
	 * overrun converts correctly (it is a real 512 s).
	 */
	radiant_lp_clock_init(&c);
	c.anchor_q = UINT64_C(1000000) << RADIANT_LP_RTC_SHIFT;
	c.anchor_ticks = UINT64_C(0x5000000);
	zassert_false(radiant_lp_asleep_ok(&ext, &c, UINT64_C(0x4FFFFFF)));
	zassert_equal(radiant_lp_clock_now(&ext, &c, true, 0u,
					   UINT64_C(0x4FFFFFF)), 0u);
	zassert_equal(c.range_errors, 1u);
	zassert_true(radiant_lp_asleep_ok(&ext, &c, UINT64_C(0x5000000) +
					  (UINT64_C(512) * 32768u)));
	zassert_equal(radiant_lp_asleep_us(&ext, &c, UINT64_C(0x5000000) +
					   (UINT64_C(512) * 32768u)),
		      513000000u);
	/* The release refuses to judge an out-of-range edge: TIMER's reading. */
	radiant_lp_clock_init(&c);
	c.timer_at_anchor = true;
	c.anchor_ticks = UINT64_C(0x5000000);
	zassert_false(radiant_lp_release(&ext, &c, UINT64_C(0x4FFFFFF), 777u,
					 RADIANT_LP_RELEASE_TOL_US, NULL));
	zassert_equal(c.anchor_q, UINT64_C(777) << 9);
}

/* --- Rebase and re-anchor --- */

ZTEST(radiant_lp_math, test_rebase_moves_the_anchor_not_the_time)
{
	struct radiant_lp_clock c;
	uint64_t cmp, at_99, at_far;

	radiant_lp_clock_init(&c);
	c.anchor_q = (UINT64_C(7000000) << 9) + 123u;
	c.anchor_ticks = 0xFFFF00u;

	/* No operation: the wake is clamped at half the range (256 s). */
	zassert_equal(radiant_lp_wake_compare(&rtc, &c, RADIANT_LP_NO_OP,
					      UINT64_MAX, &cmp),
		      RADIANT_LP_WAKE_CLAMPED);
	at_99 = radiant_lp_asleep_q(&rtc, &c, cmp - 1u);
	radiant_lp_rebase(&rtc, &c, cmp);
	zassert_equal(c.anchor_ticks, cmp);
	zassert_false(c.timer_at_anchor);
	zassert_equal(c.anchor_q, at_99 + RADIANT_LP_RTC_NUM,
		      "a rebase is the exact LF image of its edge");
	/* Readings a further 200 s on are in range again, and continuous. */
	at_far = radiant_lp_asleep_q(&rtc, &c, cmp + 200u * 32768u);
	zassert_true(radiant_lp_asleep_ok(&rtc, &c, cmp + 200u * 32768u));
	zassert_equal(at_far, at_99 + RADIANT_LP_RTC_NUM +
			      UINT64_C(200) * 32768u * RADIANT_LP_RTC_NUM);
}

ZTEST(radiant_lp_math, test_reanchor_restores_the_lf_release)
{
	struct radiant_lp_clock c;
	uint64_t base;
	int64_t diff;

	radiant_lp_clock_init(&c);
	c.anchor_q = 0u;
	/* A software-started wake at tick 1000 (the TIMER started some us after
	 * that tick was read): no exact relation, no carried fraction.
	 */
	base = radiant_lp_wake(&rtc, &c, 1000u, false);
	zassert_equal(base, 30517u);
	zassert_false(c.timer_at_anchor);

	/* (D)PPI captures the running TIMER on tick 1010 as base + 290 us
	 * (the start was ~15 us late: 10 ticks are 305.18 us).
	 */
	radiant_lp_reanchor(&rtc, &c, 1010u, base + 290u);
	zassert_true(c.timer_at_anchor);
	zassert_equal(c.anchor_ticks, 1010u);
	zassert_equal(c.anchor_q, (UINT64_C(30807) << 9));

	/* 100 ticks later (3051.76 us), the TIMER reads 3052 us more: the
	 * release takes the LF image of the edge again.
	 */
	zassert_true(radiant_lp_release(&rtc, &c, 1110u, base + 290u + 3052u,
					RADIANT_LP_RELEASE_TOL_US, &diff));
	zassert_equal(c.anchor_q, (UINT64_C(30807) << 9) + 100u * 15625u);
	/* Without the re-anchor the same release would have taken the TIMER. */
	radiant_lp_clock_init(&c);
	(void)radiant_lp_wake(&rtc, &c, 1000u, false);
	zassert_false(radiant_lp_release(&rtc, &c, 1110u, base + 290u + 3052u,
					 RADIANT_LP_RELEASE_TOL_US, NULL));
}

/* --- The two-edge wake plan --- */

static void check_wake_plan(const struct radiant_lp_tb *tb, uint32_t n)
{
	struct radiant_lp_clock c;
	const uint32_t arm = 300u;
	const uint32_t hfxo = 1650u + RADIANT_LP_HFXO_ISR_MARGIN_US;
	uint64_t lead = radiant_lp_wake_lead_us(tb, arm, hfxo);

	for (uint32_t i = 0; i < n; i++) {
		struct radiant_lp_wake_plan plan;
		uint64_t op, q1, q2, d1, d2;
		enum radiant_lp_wake_rc rc;

		radiant_lp_clock_init(&c);
		c.anchor_q = rnd64() & ((UINT64_C(1) << 50) - 1u);
		c.anchor_ticks = rnd64() & tb->mask;
		/* At least the whole lead past the anchor, up to ~8 s. */
		op = (c.anchor_q >> tb->shift) + 1u + lead + (rnd() & 0x7FFFFFu);
		rc = radiant_lp_wake_plan(tb, &c, op, arm, hfxo,
					  radiant_lp_tb_half(tb), &plan);
		zassert_equal(rc, RADIANT_LP_WAKE_OK);
		d1 = radiant_lp_ticks_since(tb, c.anchor_ticks, plan.hfxo_cmp);
		d2 = radiant_lp_ticks_since(tb, c.anchor_ticks, plan.timer_cmp);
		q1 = c.anchor_q + d1 * tb->num;
		q2 = c.anchor_q + d2 * tb->num;
		zassert_true(d1 < d2, "EDGE 1 must precede EDGE 2");
		/* EDGE 2: never late for the arm lead, less than a tick early. */
		zassert_true(q2 <= (op - arm) << tb->shift);
		zassert_true(q2 + tb->num > (op - arm) << tb->shift);
		/* EDGE 1: the HFXO gets its whole lead, and no tick more. */
		zassert_true(q2 - q1 >= (uint64_t)hfxo << tb->shift);
		zassert_true(q2 - q1 < ((uint64_t)hfxo << tb->shift) + tb->num);
		/* The whole wake fits the budget should_sleep() charges. */
		zassert_true((op << tb->shift) - q1 <= lead << tb->shift);
	}
}

ZTEST(radiant_lp_math, test_wake_plan_two_edges_rtc)
{
	check_wake_plan(&rtc, 100000u);
}

ZTEST(radiant_lp_math, test_wake_plan_two_edges_grtc)
{
	check_wake_plan(&grtc, 100000u);
}

ZTEST(radiant_lp_math, test_wake_plan_edges)
{
	struct radiant_lp_clock c;
	struct radiant_lp_wake_plan plan;

	radiant_lp_clock_init(&c);
	c.anchor_q = UINT64_C(1000) << RADIANT_LP_RTC_SHIFT;
	c.anchor_ticks = 0xFFFFF0u;

	/* Exactly: op - arm = anchor + 15625 us is tick 512; the HFXO edge is
	 * the last tick at or before 15625 - 1750 = 13875 us: 454 ticks
	 * (13854.98 us), and 455 would be 13885.5.
	 */
	zassert_equal(radiant_lp_wake_plan(&rtc, &c, 1000u + 15625u + 300u, 300u,
					   1750u, UINT64_MAX, &plan),
		      RADIANT_LP_WAKE_OK);
	zassert_equal(plan.timer_cmp, (0xFFFFF0u + 512u) & 0xFFFFFFu);
	zassert_equal(plan.hfxo_cmp, (0xFFFFF0u + 454u) & 0xFFFFFFu);

	/* Room for EDGE 2 but not for the HFXO lead before it: PAST. */
	zassert_equal(radiant_lp_wake_plan(&rtc, &c, 1000u + 1000u + 300u, 300u,
					   1750u, UINT64_MAX, &plan),
		      RADIANT_LP_WAKE_PAST);
	/* EDGE 2 on the anchor itself: PAST (EDGE 1 must be strictly before). */
	zassert_equal(radiant_lp_wake_plan(&rtc, &c, 1000u + 300u, 300u, 0u,
					   UINT64_MAX, &plan),
		      RADIANT_LP_WAKE_PAST);
	/* A zero HFXO lead still keeps EDGE 1 a tick before EDGE 2. */
	zassert_equal(radiant_lp_wake_plan(&rtc, &c, 1000u + 15625u + 300u, 300u,
					   0u, UINT64_MAX, &plan),
		      RADIANT_LP_WAKE_OK);
	zassert_equal(plan.hfxo_cmp, (0xFFFFF0u + 511u) & 0xFFFFFFu);
	/* Beyond max_ticks: CLAMPED, both fields the clamped (rebase) tick. */
	zassert_equal(radiant_lp_wake_plan(&rtc, &c, 1000u + 15625u + 300u, 300u,
					   1750u, 100u, &plan),
		      RADIANT_LP_WAKE_CLAMPED);
	zassert_equal(plan.timer_cmp, (0xFFFFF0u + 100u) & 0xFFFFFFu);
	zassert_equal(plan.hfxo_cmp, plan.timer_cmp);
	zassert_equal(radiant_lp_wake_plan(&rtc, &c, RADIANT_LP_NO_OP, 300u,
					   1750u, UINT64_MAX, &plan),
		      RADIANT_LP_WAKE_CLAMPED);
}
