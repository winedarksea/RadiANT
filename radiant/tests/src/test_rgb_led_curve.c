/* SPDX-License-Identifier: Apache-2.0 */

/*
 * apps/common/ant_rgb_led_curve.c - the activity-rate indicators' arithmetic.
 *
 * This suite exists because the rest of the feature cannot be tested here:
 * ant_rgb_led.c resolves a real led_strip device and ant_heartbeat_led.c a real
 * GPIO, both from devicetree aliases no test image has. What it CAN check is
 * the part with a right answer, and the properties worth checking are not the
 * breakpoint values (those are a taste decision) but the invariants a future
 * edit to the curve could break silently: that the mapping never inverts, that
 * the hue step never rounds to zero, and that the mono LED's gap never does
 * either.
 *
 * THE ZERO CASES ARE THE LOAD-BEARING ONES. A zero hue step is not a subtle
 * degradation - it is a stationary LED, indistinguishable from crashed
 * firmware, and it is what integer arithmetic on a 20-second rotation does if
 * the fixed-point shift is ever removed as "unnecessary precision". A zero gap
 * is worse: the caller sleeps for it, so it is a spin on the main thread.
 *
 * ONE CURVE, TWO READERS since the mono led0 heartbeat gained an activity mode.
 * ant_activity_gap_ms() rescales the same breakpoints onto the gap between
 * flashes, anchored so that a silent room returns
 * CONFIG_ANT_DONGLE_HEARTBEAT_LED_OFF_MS exactly - see the second half of this
 * file, where that identity is the first thing asserted.
 */

#include <zephyr/ztest.h>

#include "ant_rgb_led_curve.h"

/* The shipping tick, and the two ends of ANT_DONGLE_RGB_LED_TICK_MS's range. */
#define TICK_DEFAULT 25u
#define TICK_MIN     5u
#define TICK_MAX     200u

ZTEST_SUITE(rgb_led_curve, NULL, NULL, NULL, NULL, NULL);

/*
 * More traffic is never a slower rotation. Swept finely enough to catch a
 * mis-ordered breakpoint or a sign error in the interpolation, both of which
 * would still produce plausible-looking values at the breakpoints themselves.
 */
ZTEST(rgb_led_curve, test_period_is_monotonic_non_increasing)
{
	uint32_t prev = ant_rgb_led_period_ms(0u);

	for (uint32_t rate = 0u; rate <= 60000u; rate += 37u) {
		uint32_t got = ant_rgb_led_period_ms(rate);

		zassert_true(got <= prev,
			     "period rose at %u mpps: %u -> %u",
			     rate, prev, got);
		prev = got;
	}
}

/* Both ends clamp: no rate produces a period outside the curve. */
ZTEST(rgb_led_curve, test_period_is_clamped_at_both_ends)
{
	zassert_equal(ant_rgb_led_period_ms(0u), 20000u,
		      "idle rotation is not the documented 20 s");

	/* Far beyond the last breakpoint, including a value that would
	 * overflow a 32-bit intermediate if the interpolation ever ran here.
	 */
	zassert_equal(ant_rgb_led_period_ms(40000u), 750u, "floor moved");
	zassert_equal(ant_rgb_led_period_ms(1000000u), 750u, "no high clamp");
	zassert_equal(ant_rgb_led_period_ms(UINT32_MAX), 750u,
		      "UINT32_MAX rate escaped the clamp");
}

/* The breakpoints are interpolated between, not stepped between. */
ZTEST(rgb_led_curve, test_period_interpolates_between_breakpoints)
{
	/* Halfway from (0, 20000) to (4000, 8000) is 14000. */
	zassert_equal(ant_rgb_led_period_ms(2000u), 14000u,
		      "midpoint of the first segment is not interpolated");

	/* Strictly inside a segment, the value must differ from both ends. */
	uint32_t mid = ant_rgb_led_period_ms(8000u);

	zassert_true(mid < 8000u && mid > 2000u,
		     "second segment not interpolated: got %u", mid);
}

/*
 * THE ONE THAT MATTERS. At the slowest rotation and the shortest tick, the
 * advance must still be non-zero, or the wheel stops and the indicator becomes
 * a lie. This is what the 1/256 fixed point in the hue units buys.
 */
ZTEST(rgb_led_curve, test_hue_step_never_rounds_to_zero)
{
	for (uint32_t tick = TICK_MIN; tick <= TICK_MAX; tick++) {
		zassert_true(ant_rgb_led_hue_step(0u, tick) > 0u,
			     "idle hue step is zero at tick %u ms", tick);
	}
}

/* A full rotation takes the period the curve promises, within rounding. */
ZTEST(rgb_led_curve, test_hue_step_completes_a_rotation_in_the_period)
{
	static const uint32_t rates[] = { 0u, 2000u, 4000u, 16000u, 40000u };

	for (size_t i = 0u; i < ARRAY_SIZE(rates); i++) {
		uint32_t period = ant_rgb_led_period_ms(rates[i]);
		uint32_t step = ant_rgb_led_hue_step(rates[i], TICK_DEFAULT);
		uint32_t ticks = period / TICK_DEFAULT;
		uint64_t travelled = (uint64_t)step * ticks;

		/* Integer truncation of the step always undershoots, never
		 * overshoots; one tick of slack is the whole tolerance.
		 */
		zassert_true(travelled <= ANT_RGB_LED_HUE_Q_MAX,
			     "rate %u overshoots a rotation", rates[i]);
		zassert_true(travelled + step >= ANT_RGB_LED_HUE_Q_MAX,
			     "rate %u undershoots by more than one tick",
			     rates[i]);
	}
}

/* Faster traffic is a faster wheel - the property the LED actually shows. */
ZTEST(rgb_led_curve, test_hue_step_is_monotonic_non_decreasing)
{
	uint32_t prev = ant_rgb_led_hue_step(0u, TICK_DEFAULT);

	for (uint32_t rate = 0u; rate <= 60000u; rate += 37u) {
		uint32_t got = ant_rgb_led_hue_step(rate, TICK_DEFAULT);

		zassert_true(got >= prev,
			     "hue step fell at %u mpps: %u -> %u",
			     rate, prev, got);
		prev = got;
	}
}

/*
 * ── ant_activity_gap_ms(): the same curve, read by the mono led0 ─────────
 *
 * The shipping idle gap, and the two ends of
 * CONFIG_ANT_DONGLE_HEARTBEAT_LED_OFF_MS's range.
 */
#define GAP_DEFAULT 4000u
#define GAP_MIN     1u
#define GAP_MAX     60000u

/*
 * THE LOAD-BEARING ONE. CONFIG_ANT_DONGLE_HEARTBEAT_LED_OFF_MS is documented as
 * meaning the same thing whether the activity mode is on or off, so a quiet
 * bench looks identical either way. That holds only if a zero rate returns the
 * anchor EXACTLY - which it does because the scaling divides by the curve's own
 * idle value rather than by a literal 20000. Retuning the curve must not move
 * where the mono LED rests, and this is what would notice.
 */
ZTEST(rgb_led_curve, test_gap_at_idle_is_exactly_the_anchor)
{
	static const uint32_t anchors[] = {
		GAP_MIN, 30u, 250u, 1000u, GAP_DEFAULT, 10000u, GAP_MAX,
	};

	for (size_t i = 0u; i < ARRAY_SIZE(anchors); i++) {
		zassert_equal(ant_activity_gap_ms(0u, anchors[i]), anchors[i],
			      "idle gap is not the anchor for %u",
			      anchors[i]);
	}
}

/* More traffic is never a longer wait, for any anchor. */
ZTEST(rgb_led_curve, test_gap_is_monotonic_non_increasing)
{
	static const uint32_t anchors[] = { 30u, GAP_DEFAULT, GAP_MAX };

	for (size_t i = 0u; i < ARRAY_SIZE(anchors); i++) {
		uint32_t prev = ant_activity_gap_ms(0u, anchors[i]);

		for (uint32_t rate = 0u; rate <= 60000u; rate += 37u) {
			uint32_t got = ant_activity_gap_ms(rate, anchors[i]);

			zassert_true(got <= prev,
				     "gap rose at %u mpps (anchor %u): %u -> %u",
				     rate, anchors[i], prev, got);
			prev = got;
		}
	}
}

/*
 * THE OTHER ONE THAT MATTERS, and the mono LED's version of the zero hue step
 * above. The caller sleeps for this value, so a zero is not a fast blink - it
 * is a spin on the main thread, at whatever priority that thread happens to
 * hold. It is reachable honestly: a small anchor at the busy end of the curve
 * truncates to zero long before the rate axis runs out.
 */
ZTEST(rgb_led_curve, test_gap_never_returns_zero)
{
	for (uint32_t anchor = GAP_MIN; anchor <= 200u; anchor++) {
		for (uint32_t rate = 0u; rate <= 60000u; rate += 97u) {
			zassert_true(ant_activity_gap_ms(rate, anchor) > 0u,
				     "zero gap at %u mpps, anchor %u",
				     rate, anchor);
		}

		zassert_true(ant_activity_gap_ms(UINT32_MAX, anchor) > 0u,
			     "zero gap at UINT32_MAX, anchor %u", anchor);
	}
}

/*
 * The shape the Kconfig help text promises, at the shipping anchor: about 4 s
 * idle, 1.6 s with one sensor, 400 ms with four, 150 ms with ten. Those are the
 * numbers a person reads before deciding whether they want this on, so they are
 * asserted rather than left to drift with the curve.
 */
ZTEST(rgb_led_curve, test_gap_matches_the_documented_shape)
{
	zassert_equal(ant_activity_gap_ms(4000u, GAP_DEFAULT), 1600u,
		      "one sensor is not the documented 1.6 s");
	zassert_equal(ant_activity_gap_ms(16000u, GAP_DEFAULT), 400u,
		      "four sensors is not the documented 400 ms");
	zassert_equal(ant_activity_gap_ms(40000u, GAP_DEFAULT), 150u,
		      "the floor is not the documented 150 ms");

	/* And the floor is a floor: no rate beyond it goes lower. */
	zassert_equal(ant_activity_gap_ms(UINT32_MAX, GAP_DEFAULT), 150u,
		      "UINT32_MAX rate escaped the floor");
}
