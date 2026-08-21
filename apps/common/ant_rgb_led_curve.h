/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The activity-rate indicators' arithmetic, split out from ant_rgb_led.c.
 *
 * WHY IT IS A SEPARATE TRANSLATION UNIT. This is the only part of the feature
 * with a right answer, so it is the only part worth a test - and ant_rgb_led.c
 * cannot be compiled by the test application at all, because it resolves a real
 * led_strip device from a `rgb-led0` devicetree alias that no test image has.
 * Splitting the pure functions out is what makes radiant/tests/src/
 * test_rgb_led_curve.c possible without a fake LED driver. The same is true of
 * ant_heartbeat_led.c and its `led0`.
 *
 * IT SERVES BOTH INDICATORS NOW, and kept its name anyway. One curve maps
 * receive rate onto a duration; ant_rgb_led.c spends that duration on a hue
 * rotation and ant_heartbeat_led.c spends it on the gap between flashes, so
 * "how fast is the wheel" and "how often does it blink" cannot disagree about
 * what the air is doing. Renaming the file would have churned the ztest suite
 * name and its registration in radiant/tests/CMakeLists.txt for no behavioural
 * gain, so the ant_rgb_led_ prefix stays on the hue half and the shared half is
 * spelled ant_activity_.
 *
 * Nothing here includes a Zephyr header or reads a Kconfig symbol, deliberately:
 * the tick period arrives as an argument rather than as
 * CONFIG_ANT_DONGLE_RGB_LED_TICK_MS so that a suite can sweep it, and so that
 * this file compiles in an image where neither feature symbol exists.
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

/*
 * Milliseconds to wait before the mono led0's next flash, at a given receive
 * rate, given the gap wanted when nothing at all is being heard.
 *
 * The same curve as above, rescaled: `gap_idle_ms` is an ANCHOR, so a rate of
 * zero returns it exactly whatever the curve's own breakpoints happen to be,
 * and every busier rate is that value shrunk in the curve's proportion. Two
 * things follow, both of them the point. CONFIG_ANT_DONGLE_HEARTBEAT_LED_OFF_MS
 * means the same thing whether the activity mode is on or off - a quiet bench
 * looks identical either way - and re-tuning the curve for the NeoPixel can
 * never silently move where the mono LED rests.
 *
 * Monotonically non-increasing, and never returns zero: the caller sleeps for
 * this, and a zero would be a spin rather than a fast blink.
 */
uint32_t ant_activity_gap_ms(uint32_t rate_mpps, uint32_t gap_idle_ms);

#endif /* ANT_RGB_LED_CURVE_H_ */
