/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The mono led0 heartbeat. See ant_heartbeat_led.h for why it is a shared file;
 * this is the loop itself.
 *
 * WHAT IT COSTS THE RADIO, stated because it is the reason the pattern is shaped
 * this way. This runs on the main thread and does two things per cycle: a
 * gpio_pin_set_dt() - one register write - and a k_sleep(). No irq_lock(), no
 * DMA, no share of the TIMER or (D)PPI channels the radiant backend owns, and
 * nothing in interrupt context. At the default 30 ms / 4000 ms that is two
 * wakeups per four seconds, against the eight per four seconds of the 1 Hz
 * 50 %-duty blink it replaced.
 *
 * That is also why the activity mode below is free where the NeoPixel's is not:
 * the constraint on ant_rgb_led.c is its TRANSPORT (a 24-byte SPIM EasyDMA
 * transfer, chosen over ws2812_gpio's ~30 us of masked interrupts per pixel).
 * A mono LED has no transport, so varying the interval costs exactly what a
 * fixed interval costs - one integer expression per flash, on a thread that was
 * about to sleep anyway.
 *
 * gpio_pin_set_dt(), not gpio_pin_toggle_dt(). `set` honours GPIO_ACTIVE_LOW
 * from the devicetree and `toggle` does not, so with an asymmetric pattern an
 * active-low led0 would otherwise be dark for 30 ms every four seconds and lit
 * the rest of the time - the exact inverse of what is configured, and a thing
 * nobody would think to check.
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>

#include "ant_heartbeat_led.h"

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)

#include <zephyr/drivers/gpio.h>

static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

#if defined(CONFIG_ANT_DONGLE_HEARTBEAT_LED_ACTIVITY)

#include <zephyr/sys/atomic.h>

#include "ant_activity.h"
#include "ant_rgb_led_curve.h"

/*
 * The gap before the next flash, from how much traffic arrived during the last
 * one's cycle.
 *
 * NO EWMA HERE, and that is a deliberate difference from ant_rgb_led.c, which
 * smooths with a 1/32 filter. That is right at its 25 ms tick; on a loop whose
 * sample window is SECONDS the same filter is a time constant of about two
 * minutes, and the indicator would spend a full minute catching up with a sensor
 * that had already been switched off. The window here is its own smoothing: four
 * seconds of a single ANT+ sensor is about sixteen packets, which is a far
 * steadier number than any 25 ms bucket.
 *
 * The window is MEASURED rather than assumed, because the loop's own period is
 * what this function is deciding - so the interval between two calls is a
 * quantity from the last decision, not a constant. Getting that wrong would
 * silently scale the whole rate axis by whatever the last gap happened to be.
 *
 * The one filter that remains is a 50 % one-pole, and it earns its place at the
 * BUSY end: once the gap has closed to 150 ms the window is short enough for a
 * single packet's timing to move the rate, and the LED would hunt. Its time
 * constant therefore scales with the blink period - slow when idle, fast when
 * busy - which is the behaviour wanted rather than a compromise.
 */
static uint32_t next_gap_ms(void)
{
	static int64_t last_ms;      /* 0 = boot: the first window is real */
	static uint32_t rate_mpps;

	int64_t now = k_uptime_get();
	uint32_t n = (uint32_t)atomic_clear(&ant_activity_pkts);
	uint32_t elapsed_ms = (uint32_t)(now - last_ms);

	last_ms = now;

	/* Cannot happen with ON_MS >= 1, but a zero window here would be a
	 * divide by zero rather than a wrong colour.
	 */
	if (elapsed_ms == 0u) {
		return CONFIG_ANT_DONGLE_HEARTBEAT_LED_OFF_MS;
	}

	/* Milli-packets per second: the scale ant_rgb_led_curve.c's rate axis
	 * is written on. See its header for why it is milli- and not whole.
	 */
	uint32_t inst_mpps =
		(uint32_t)(((uint64_t)n * 1000u * 1000u) / elapsed_ms);

	rate_mpps = (rate_mpps + inst_mpps) / 2u;

	return ant_activity_gap_ms(rate_mpps,
				   CONFIG_ANT_DONGLE_HEARTBEAT_LED_OFF_MS);
}

#else /* !CONFIG_ANT_DONGLE_HEARTBEAT_LED_ACTIVITY */

static uint32_t next_gap_ms(void)
{
	return CONFIG_ANT_DONGLE_HEARTBEAT_LED_OFF_MS;
}

#endif /* CONFIG_ANT_DONGLE_HEARTBEAT_LED_ACTIVITY */

#endif /* DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay) */

void ant_heartbeat_led_run(void)
{
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
	/* INACTIVE, not ACTIVE: the LED starts dark and the loop below is the
	 * only thing that ever lights it. Configuring it on and leaving it on
	 * would put a 4-second lamp on any path that failed before the loop.
	 */
	bool led_ok = (gpio_is_ready_dt(&led) == true) &&
		      (gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) == 0);

	while (led_ok) {
		(void)gpio_pin_set_dt(&led, 1);
		k_sleep(K_MSEC(CONFIG_ANT_DONGLE_HEARTBEAT_LED_ON_MS));
		(void)gpio_pin_set_dt(&led, 0);

		/* Called AFTER the flash, so the window it measures is one
		 * whole cycle rather than one gap.
		 */
		k_sleep(K_MSEC(next_gap_ms()));
	}
#endif

	/* No led0, or it never came ready. Sleep rather than spin: the caller
	 * has nothing else to do and a busy loop here would be a wakeup per
	 * tick forever for no output.
	 */
	for (;;) {
		k_sleep(K_FOREVER);
	}
}
