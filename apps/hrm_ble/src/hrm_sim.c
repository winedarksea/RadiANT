/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CONFIG_HRM_SENSOR_SIMULATED: the reference implementation of
 * src/hrm_sensor.h's seam, and the bench instrument this application started
 * as.
 *
 * A DK has no electrodes. This produces beats at a fixed rate so that page
 * 0x00's event count and event time - the fields that actually survive a lost
 * packet - advance the way a real strap's do. A node that merely set a computed
 * bpm would be exercising the one field that does not matter.
 *
 * IT IS A BEAT GENERATOR, NOT A RATE SETTING, and it reports through exactly
 * the same two calls a real driver uses. That is what makes it a reference
 * rather than a special case: if the simulator needed a private path into
 * radiant_profile_hr, the seam would not be carrying its own weight.
 *
 * The schedule itself is NOT here. It is a pure function in hrm_sim_sched.c so
 * that a test can drive it; this file is only the kernel plumbing around it.
 *
 * SKIN CONTACT, for the bench (M1: "the contact bit toggling with the sim's
 * contact control"). The simulator declares contact SUPPORT, and its contact
 * STATUS is toggled by button 0 (CONFIG_HRM_SENSOR_SIM_CONTACT_BUTTON) and/or
 * a timed pattern (CONFIG_HRM_SENSOR_SIM_CONTACT_TOGGLE_S). Without contact it
 * does what a strap off the chest does: no beats, no rate, and
 * hrm_sensor_report_contact(false). With contact back it re-anchors the beat
 * schedule and reports the rate at once.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>
#if defined(CONFIG_HRM_SENSOR_SIM_CONTACT_BUTTON)
#include <zephyr/drivers/gpio.h>
#endif

#include "hrm_sensor.h"
#include "hrm_sim_sched.h"

LOG_MODULE_DECLARE(hrm, CONFIG_HRM_LOG_LEVEL);

/*
 * 100 ms, which is also the node tick in main.c and is not a coincidence: the
 * schedule's quantisation and the node's housekeeping cadence are the same
 * number so that there is one interval to reason about rather than two that
 * beat against each other.
 */
#define SIM_TICK_MS 100u

static struct hrm_sim_sched sched;

/* Simulated skin contact: 1 detected, 0 lost. Written by the button ISR and
 * the timed pattern, read by the beat work item. */
static atomic_t sim_contact = ATOMIC_INIT(1);

static void sim_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(sim_work, sim_work_fn);

/* Flip contact and report it. Safe from the button ISR: one atomic, and
 * hrm_sensor_report_contact() is ISR-safe. */
static void sim_contact_toggle(void)
{
	bool detected = (atomic_xor(&sim_contact, 1) & 1) == 0;

	hrm_sensor_report_contact(detected);
}

#if defined(CONFIG_HRM_SENSOR_SIM_CONTACT_BUTTON)
static const struct gpio_dt_spec contact_button = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static struct gpio_callback contact_button_cb;

/* Interrupt context. A 250 ms lockout is the debounce: a DK button bounces
 * for a few ms, and nobody presses twice in a quarter second on purpose. */
static void contact_button_isr(const struct device *port, struct gpio_callback *cb,
			       gpio_port_pins_t pins)
{
	static uint32_t last_ms;
	uint32_t now = k_uptime_get_32();

	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);

	if (last_ms != 0u && (now - last_ms) < 250u) {
		return;
	}
	last_ms = now;
	sim_contact_toggle();
}

static int contact_button_init(void)
{
	int err;

	if (!gpio_is_ready_dt(&contact_button)) {
		return -ENODEV;
	}
	err = gpio_pin_configure_dt(&contact_button, GPIO_INPUT);
	if (err != 0) {
		return err;
	}
	gpio_init_callback(&contact_button_cb, contact_button_isr, BIT(contact_button.pin));
	err = gpio_add_callback_dt(&contact_button, &contact_button_cb);
	if (err != 0) {
		return err;
	}
	return gpio_pin_interrupt_configure_dt(&contact_button, GPIO_INT_EDGE_TO_ACTIVE);
}
#endif /* CONFIG_HRM_SENSOR_SIM_CONTACT_BUTTON */

static void sim_work_fn(struct k_work *w)
{
	const uint32_t bpm = CONFIG_HRM_SENSOR_SIM_BPM;
	static bool had_contact = true;
	bool contact;

	ARG_UNUSED(w);

#if CONFIG_HRM_SENSOR_SIM_CONTACT_TOGGLE_S > 0
	{
		static uint32_t since_ms;

		since_ms += SIM_TICK_MS;
		if (since_ms >= CONFIG_HRM_SENSOR_SIM_CONTACT_TOGGLE_S * 1000u) {
			since_ms = 0u;
			sim_contact_toggle();
		}
	}
#endif

	contact = atomic_get(&sim_contact) != 0;
	if (!contact) {
		/* Off the chest: no beats and no rate. The node's stale
		 * deadline would invalidate the rate anyway; the contact report
		 * does it at once. */
		had_contact = false;
		k_work_schedule(&sim_work, K_MSEC(SIM_TICK_MS));
		return;
	}
	if (!had_contact) {
		/* Back on: a fresh schedule (its first tick beats, anchoring
		 * it) and the rate straight away, as a strap does once it has
		 * a reading again. */
		had_contact = true;
		sched = (struct hrm_sim_sched){ 0 };
		hrm_sensor_report_rate((uint8_t)bpm);
	}

	if (hrm_sim_sched_tick(&sched, SIM_TICK_MS,
			       hrm_sim_sched_interval_ms(bpm)) != 0u) {
		/*
		 * hrm_sensor_now_1024() rather than a time carried out of the
		 * scheduler, and the difference is worth stating because it
		 * looks like a bug and is not. The scheduler's `elapsed_ms` is
		 * a count of ticks, not a clock: it drifts against k_uptime by
		 * however much the workqueue is late. The instant that goes on
		 * the air must be the real one, so it is read here, in the
		 * context that is about to report it. The scheduler decides
		 * WHETHER a beat is due; the clock decides WHEN it happened.
		 *
		 * A real driver does better still - its AFE captured the edge -
		 * which is exactly why the seam takes an instant rather than
		 * calling this itself.
		 *
		 * The rate is reported with every beat, a repeat of the same
		 * value included: each report keeps it from going stale
		 * (CONFIG_HRM_BPM_STALE_MS).
		 */
		hrm_sensor_report_beat(hrm_sensor_now_1024());
		hrm_sensor_report_rate((uint8_t)bpm);
	}

	k_work_schedule(&sim_work, K_MSEC(SIM_TICK_MS));
}

static int sim_start(const struct hrm_sensor *sensor)
{
	ARG_UNUSED(sensor);

	/* Report the rate before the first beat so that the very first page the
	 * ANT+ master stages - which is built before the channel opens, because
	 * a master transmits the instant it does - already carries byte 7. */
	hrm_sensor_report_rate((uint8_t)CONFIG_HRM_SENSOR_SIM_BPM);
	hrm_sensor_report_contact(true);

#if defined(CONFIG_HRM_SENSOR_SIM_CONTACT_BUTTON)
	{
		int err = contact_button_init();

		if (err != 0) {
			LOG_WRN("hrm_sim: contact button: %d (contact stays on)", err);
		} else {
			LOG_INF("hrm_sim: button 0 toggles skin contact");
		}
	}
#endif
#if CONFIG_HRM_SENSOR_SIM_CONTACT_TOGGLE_S > 0
	LOG_INF("hrm_sim: skin contact toggles every %u s",
		(unsigned)CONFIG_HRM_SENSOR_SIM_CONTACT_TOGGLE_S);
#endif

	k_work_schedule(&sim_work, K_MSEC(SIM_TICK_MS));
	return 0;
}

static int sim_stop(const struct hrm_sensor *sensor)
{
	ARG_UNUSED(sensor);
	(void)k_work_cancel_delayable(&sim_work);
	return 0;
}

static const struct hrm_sensor_api sim_api = {
	.start = sim_start,
	.stop = sim_stop,
	/* No poll: contact is driven by the button ISR and the beat work item,
	 * and there is no AGC to run. Leaving it NULL rather than supplying an
	 * empty function is deliberate - the seam documents poll as optional,
	 * and an application that only ever saw non-NULL ones would grow the
	 * assumption. */
};

static const struct hrm_sensor sim_sensor = {
	.name = "simulated",
	.api = &sim_api,
	/* Simulated contact is part of what the bench checks (the BLE
	 * Measurement's contact bits), so the simulator declares support. */
	.contact_supported = true,
};

static int sim_register(void)
{
	int err = hrm_sensor_register(&sim_sensor);

	if (err != 0) {
		LOG_ERR("hrm_sim: register: %d", err);
	}
	return 0;
}

/*
 * APPLICATION level, which runs before main(). Registration has to happen
 * before main()'s hrm_sensor_start(), and a SYS_INIT is how a real driver would
 * do it too - so the reference implementation uses the mechanism it is
 * recommending rather than a call main() makes on its behalf.
 */
SYS_INIT(sim_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
