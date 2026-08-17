/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The activity-rate indicator's arithmetic, split out from ant_rgb_led.c.
 *
 * WHY IT IS A SEPARATE TRANSLATION UNIT. This is the only part of the feature
 * with a right answer, so it is the only part worth a test - and ant_rgb_led.c
 * cannot be compiled by the test application at all, because it resolves a real
 * led_strip device from a `rgb-led0` devicetree alias that no test image has.
 * Splitting the pure functions out is what makes radiant/tests/src/
 * test_rgb_led_curve.c possible without a fake LED driver.
 *
 * Nothing here includes a Zephyr header or reads a Kconfig symbol, deliberately:
 * the tick period arrives as an argument rather than as
 * CONFIG_ANT_DONGLE_RGB_LED_TICK_MS so that a suite can sweep it, and so that
 * this file compiles in an image where the feature symbol does not exist.
 */

#ifndef ANT_RGB_LED_CURVE_H_
#define ANT_RGB_LED_CURVE_H_

#include <stdint.h>

/*
 * Hue is a six-sector wheel at full saturation: 256 steps per sector, 1536 for
 * a full rotation. Carried internally shifted left by HUE_FRAC_BITS so the
 * per-tick advance can be a fraction of a hue unit - at the idle end it is
 * about two hundredths of one, and integer truncation of the true step would
 * land on zero and stop the wheel dead.
 */
#define ANT_RGB_LED_HUE_SECTORS   6u
#define ANT_RGB_LED_HUE_MAX       (ANT_RGB_LED_HUE_SECTORS * 256u)
#define ANT_RGB_LED_HUE_FRAC_BITS 8u
#define ANT_RGB_LED_HUE_Q_MAX     (ANT_RGB_LED_HUE_MAX << ANT_RGB_LED_HUE_FRAC_BITS)

/*
 * Milliseconds for one full hue rotation at a given smoothed receive rate,
 * in milli-packets per second.
 *
 * Monotonically non-increasing: more traffic is never a slower rotation. Both
 * ends are clamped, so no rate produces a period outside the curve.
 */
uint32_t ant_rgb_led_period_ms(uint32_t rate_mpps);

/*
 * Hue advance per render tick, in 1/256 of a hue unit, for a given smoothed
 * rate and tick period. `tick_ms` must be non-zero.
 */
uint32_t ant_rgb_led_hue_step(uint32_t rate_mpps, uint32_t tick_ms);

#endif /* ANT_RGB_LED_CURVE_H_ */
