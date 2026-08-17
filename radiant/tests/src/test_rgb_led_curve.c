/* SPDX-License-Identifier: Apache-2.0 */

/*
 * apps/common/ant_rgb_led_curve.c - the activity-rate indicator's arithmetic.
 *
 * This suite exists because the rest of the feature cannot be tested here: the
 * render loop resolves a real led_strip device from a devicetree alias no test
 * image has. What it CAN check is the part with a right answer, and the two
 * properties worth checking are not the breakpoint values (those are a taste
 * decision) but the invariants a future edit to the curve could break silently:
 * that the mapping never inverts, and that the hue step never rounds to zero.
 *
 * The second one is the load-bearing test. A zero step is not a subtle
 * degradation - it is a stationary LED, indistinguishable from a crashed
 * firmware, and it is what integer arithmetic on a 20-second rotation does if
 * the fixed-point shift is ever removed as "unnecessary precision".
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
	zassert_equal(ant_rgb_led_period_ms(40000u), 1500u, "floor moved");
	zassert_equal(ant_rgb_led_period_ms(1000000u), 1500u, "no high clamp");
	zassert_equal(ant_rgb_led_period_ms(UINT32_MAX), 1500u,
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

	zassert_true(mid < 8000u && mid > 3000u,
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
