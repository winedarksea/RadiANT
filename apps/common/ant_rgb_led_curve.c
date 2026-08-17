/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The activity-rate curve. See ant_rgb_led_curve.h for why this is a separate
 * translation unit; keep it free of Zephyr headers and Kconfig symbols, or the
 * test application stops being able to link it.
 */

#include "ant_rgb_led_curve.h"

#include <stddef.h>

/*
 * The curve, in milli-packets/second against milliseconds per full rotation.
 * An ANT+ sensor on a standard channel period broadcasts at about 4 Hz, so the
 * rate axis reads roughly as: nothing, one sensor, four sensors, ten or more.
 *
 * Milli-units rather than whole packets/second because the render loop's moving
 * average lives on this scale, and at one sensor the entire signal is four
 * counts - rounding that to an integer packet rate would quantise the low end
 * of the curve into three or four visible steps instead of a smooth ramp.
 */
struct rate_point {
	uint32_t rate_mpps;
	uint32_t period_ms;
};

static const struct rate_point curve[] = {
	{     0u, 20000u },
	{  4000u,  8000u },
	{ 16000u,  2000u },
	{ 40000u,   750u },
};

#define CURVE_LAST (sizeof(curve) / sizeof(curve[0]) - 1u)

uint32_t ant_rgb_led_period_ms(uint32_t rate_mpps)
{
	if (rate_mpps >= curve[CURVE_LAST].rate_mpps) {
		return curve[CURVE_LAST].period_ms;
	}

	for (size_t i = 1u; i <= CURVE_LAST; i++) {
		if (rate_mpps < curve[i].rate_mpps) {
			const struct rate_point *lo = &curve[i - 1u];
			const struct rate_point *hi = &curve[i];
			uint32_t span = hi->rate_mpps - lo->rate_mpps;
			uint32_t drop = lo->period_ms - hi->period_ms;

			return lo->period_ms -
			       (uint32_t)(((uint64_t)drop *
					   (rate_mpps - lo->rate_mpps)) / span);
		}
	}

	/* Unreachable: the loop covers every rate below the last breakpoint. */
	return curve[CURVE_LAST].period_ms;
}

uint32_t ant_rgb_led_hue_step(uint32_t rate_mpps, uint32_t tick_ms)
{
	uint32_t period_ms = ant_rgb_led_period_ms(rate_mpps);

	return (uint32_t)(((uint64_t)ANT_RGB_LED_HUE_Q_MAX * tick_ms) / period_ms);
}
