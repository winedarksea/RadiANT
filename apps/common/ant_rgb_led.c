/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The activity-rate RGB indicator: the render side. The counting hook it reads
 * is shared with the mono led0 heartbeat and lives in ant_activity.h - see that
 * file for why it is a hook of its own rather than CONFIG_ANT_DONGLE_RX_TAP,
 * and for why the counter has exactly one reader per image.
 *
 * THE CONSTRAINT THIS FILE IS SHAPED BY. On nRF52840 the radiant backend's
 * radio_isr is a plain maskable IRQ_CONNECT at priority 0 - radiant_radio_nrf.c
 * says so itself, and says any irq_lock() in the system can hold it off. So the
 * transport is chosen to contain no irq_lock at all: ws2812_spi hands a 24-byte
 * buffer to SPIM EasyDMA and sleeps on a semaphore while the hardware clocks it
 * out. The CPU is free for the whole transfer, the completion interrupt sits at
 * priority 1 where the radio preempts it, and nothing here shares the TIMER or
 * the (D)PPI channels the backend owns.
 *
 * That rules out ws2812_gpio, which bit-bangs with interrupts globally masked
 * for the entire frame - about 30 us per pixel, imposed on the radio ISR on
 * every single update. It is the obvious driver to reach for and it is the one
 * that would quietly cost packets.
 *
 * The render thread runs at CONFIG_ANT_DONGLE_RGB_LED_PRIO, numerically below
 * the serial bridge (5) and the radiant API event thread (6), so it cannot
 * preempt any ANT work even when it does run.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include "ant_activity.h"
#include "ant_rgb_led_curve.h"

/*
 * Loudly, rather than degrading to doing nothing. The recurring failure in this
 * repo is a check that stops applying and stays green (see the generator that
 * wrote to a stale path and then compared it against itself), and "the feature
 * is enabled but silently inert" is the same shape. If the symbol is on, the
 * board must have said where the LED is.
 */
#if !DT_NODE_HAS_STATUS(DT_ALIAS(rgb_led0), okay)
#error "CONFIG_ANT_DONGLE_RGB_LED=y but there is no enabled `rgb-led0` devicetree alias. \
Add an overlay declaring the LED node and aliasing it - see apps/dongle/rgb_feather.overlay."
#endif

LOG_MODULE_REGISTER(ant_rgb_led, LOG_LEVEL_INF);

static const struct device *const strip = DEVICE_DT_GET(DT_ALIAS(rgb_led0));

#define TICK_MS        CONFIG_ANT_DONGLE_RGB_LED_TICK_MS

/*
 * Exponential moving average shift. 1/32 at a 25 ms tick is a time constant of
 * about 0.8 s, which is the point of the whole filter: a single packet arriving
 * inside one 25 ms tick is an instantaneous rate of 40 packets/second, so the
 * unsmoothed signal is pure spikes and would strobe rather than rotate.
 */
#define EWMA_SHIFT     5u

/*
 * Full-saturation hue to RGB. `val` is the brightness ceiling, so the LED never
 * leaves the configured budget: one channel sits at `val`, one ramps between 0
 * and `val`, and the third is off.
 */
static void hue_to_rgb(uint32_t hue, uint8_t val, struct led_rgb *px)
{
	uint32_t sector = (hue >> 8) % ANT_RGB_LED_HUE_SECTORS;
	uint32_t frac = hue & 0xffu;
	uint8_t up = (uint8_t)((frac * val) / 255u);
	uint8_t down = (uint8_t)(val - up);

	switch (sector) {
	case 0u:  px->r = val;  px->g = up;   px->b = 0u;   break;
	case 1u:  px->r = down; px->g = val;  px->b = 0u;   break;
	case 2u:  px->r = 0u;   px->g = val;  px->b = up;   break;
	case 3u:  px->r = 0u;   px->g = down; px->b = val;  break;
	case 4u:  px->r = up;   px->g = 0u;   px->b = val;  break;
	default:  px->r = val;  px->g = 0u;   px->b = down; break;
	}
}

static void ant_rgb_led_thread(void *a, void *b, void *c)
{
	struct led_rgb px = { 0 };
	uint32_t hue_q = 0u;
	uint32_t rate_mpps = 0u;
	bool complained = false;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/*
	 * Give up rather than retry. A strip that is not ready at this point
	 * will not become ready later, and a thread spinning on it would be a
	 * wakeup per tick forever for no output.
	 */
	if (!device_is_ready(strip)) {
		LOG_ERR("rgb-led0 (%s) not ready; indicator disabled",
			strip->name);
		return;
	}

	LOG_INF("rgb indicator up on %s", strip->name);

	while (1) {
		uint32_t n = (uint32_t)atomic_clear(&ant_activity_pkts);
		uint32_t inst_mpps = n * (1000u * 1000u / TICK_MS);

		/*
		 * Asymmetric on purpose. The rise rounds up so the first packet
		 * after a quiet spell moves the wheel visibly within a tick or
		 * two; the decay does not, so a single dropped packet does not
		 * pull the speed back down.
		 */
		if (inst_mpps > rate_mpps) {
			rate_mpps += ((inst_mpps - rate_mpps) +
				      ((1u << EWMA_SHIFT) - 1u)) >> EWMA_SHIFT;
		} else {
			rate_mpps -= (rate_mpps - inst_mpps) >> EWMA_SHIFT;
		}

		hue_q += ant_rgb_led_hue_step(rate_mpps, TICK_MS);
		if (hue_q >= ANT_RGB_LED_HUE_Q_MAX) {
			hue_q -= ANT_RGB_LED_HUE_Q_MAX;
		}

		hue_to_rgb(hue_q >> ANT_RGB_LED_HUE_FRAC_BITS,
			   CONFIG_ANT_DONGLE_RGB_LED_BRIGHTNESS, &px);

		int ret = led_strip_update_rgb(strip, &px, 1);

		/*
		 * Logged once. A per-tick error message at 40 Hz would flood the
		 * deferred log and drop the messages that matter.
		 */
		if (ret != 0 && !complained) {
			complained = true;
			LOG_ERR("led_strip_update_rgb failed: %d", ret);
		}

		k_sleep(K_MSEC(TICK_MS));
	}
}

K_THREAD_DEFINE(ant_rgb_led_tid, CONFIG_ANT_DONGLE_RGB_LED_STACK_SIZE,
		ant_rgb_led_thread, NULL, NULL, NULL,
		CONFIG_ANT_DONGLE_RGB_LED_PRIO, 0, 0);
