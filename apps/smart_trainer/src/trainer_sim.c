/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_sim.c - CONFIG_SMART_TRAINER_SOURCE_SIMULATED: the reference driver.
 *
 * A simulated rider on apps/trainer's FE-C SS8.8.4 physics model
 * (trainer_model.c, compiled from apps/trainer, not copied), behind the seam
 * a real driver implements. The conversions live in trainer_sim_model.c,
 * which is kernel-free and under test; this file is a 250 ms timer, the hooks
 * and the bench buttons.
 *
 * Every hook runs on the system workqueue, as does the tick and each button,
 * so `sim` needs no lock.
 *
 * The bench controls (CONFIG_SMART_TRAINER_SIM_BUTTONS):
 *   sw0  pairing: short press opens the BLE pairing window; hold >= 5 s to
 *        forget every bond
 *   sw1  the rider: each press stops / resumes pedalling
 *   sw2  console stop (bumps the session epoch)
 *   sw3  console reset (bumps the epoch, targets back to default)
 */

#include <errno.h>

#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "trainer_sim_model.h"
#include "trainer_source.h"

LOG_MODULE_DECLARE(smart_trainer, CONFIG_SMART_TRAINER_LOG_LEVEL);

#define SIM_TICK_MS 250u

static struct trainer_sim_model sim;

static void sim_tick_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(sim_tick, sim_tick_fn);

static void sim_tick_fn(struct k_work *w)
{
	struct trainer_report r;

	ARG_UNUSED(w);
	trainer_sim_model_tick(&sim, SIM_TICK_MS, &r);
	(void)trainer_source_report(&r);
	k_work_schedule(&sim_tick, K_MSEC(SIM_TICK_MS));
}

static int sim_start(void)
{
	k_work_schedule(&sim_tick, K_NO_WAIT);
	return 0;
}

static int sim_set_target_power(uint16_t watts)
{
	LOG_INF("sim: ERG %u W", watts);
	return trainer_sim_model_set_target_power(&sim, watts);
}

static int sim_set_resistance(uint16_t permille)
{
	LOG_INF("sim: resistance %u.%u %%", permille / 10u, permille % 10u);
	return trainer_sim_model_set_resistance(&sim, permille);
}

static int sim_set_sim(const struct trainer_sim_params *p)
{
	LOG_INF("sim: grade %d centi-%%, crr %u e-5, cw %u, wind %d mm/s",
		(int)p->grade_centi_pct, p->crr_e5, p->cw_centi, (int)p->wind_mm_s);
	return trainer_sim_model_set_sim(&sim, p);
}

static int sim_set_user(const struct trainer_user *u)
{
	return trainer_sim_model_set_user(&sim, u);
}

static int sim_workout(enum trainer_workout w)
{
	enum trainer_source_event evt;
	int rc = trainer_sim_model_workout(&sim, w, &evt);

	if (rc == 0) {
		/* The simulated brake is instant; a real one reports once it
		 * really is in the new state. If the event cannot be queued the
		 * session would never follow, so the procedure fails (0x04). */
		rc = trainer_source_report_event(evt);
	}
	return rc;
}

static const struct trainer_source_ops sim_source = {
	.name = "simulated rider (apps/trainer model)",
	.start = sim_start,
	.set_target_power = sim_set_target_power,
	.set_resistance = sim_set_resistance,
	.set_sim = sim_set_sim,
	.set_user = sim_set_user,
	.workout = sim_workout,
	.max_resistance_n = TRAINER_MAX_RESISTANCE_N,
	.min_power_w = 0u,
	.max_power_w = 0u, /* the Kconfig range */
};

#if defined(CONFIG_SMART_TRAINER_SIM_BUTTONS)

#define BTN_QUIET_MS       30u
#define BTN_FORGET_HOLD_MS 5000

struct sim_button {
	struct gpio_dt_spec spec;
	struct gpio_callback cb;
	struct k_work_delayable settle;
	bool pressed;
	int64_t down_ms;
	uint8_t id;
};

static struct sim_button buttons[4] = {
	{ .spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios), .id = 0 },
	{ .spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios), .id = 1 },
	{ .spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios), .id = 2 },
	{ .spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios), .id = 3 },
};

static void button_action(uint8_t id, int64_t held_ms)
{
	switch (id) {
	case 0:
		(void)trainer_source_report_event(held_ms >= BTN_FORGET_HOLD_MS
							  ? TRAINER_EVT_FORGET_BONDS
							  : TRAINER_EVT_PAIRING_WINDOW);
		break;
	case 1:
		trainer_sim_model_set_rider(&sim, sim.model.rider_power_w,
					    sim.rider_on ? 0u
							 : TRAINER_DEFAULT_RIDER_CADENCE_RPM);
		LOG_INF("sim rider: %s", sim.rider_on ? "pedalling" : "stopped");
		break;
	case 2:
		trainer_sim_model_set_rider(&sim, sim.model.rider_power_w, 0u);
		(void)trainer_source_report_event(TRAINER_EVT_CONSOLE_STOP);
		break;
	case 3:
		(void)trainer_source_report_event(TRAINER_EVT_CONSOLE_RESET);
		break;
	default:
		break;
	}
}

static void button_settled(struct k_work *w)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(w);
	struct sim_button *b = CONTAINER_OF(dw, struct sim_button, settle);
	bool level = gpio_pin_get_dt(&b->spec) > 0;

	if (level && !b->pressed) {
		b->pressed = true;
		b->down_ms = k_uptime_get();
	} else if (!level && b->pressed) {
		b->pressed = false;
		button_action(b->id, k_uptime_get() - b->down_ms);
	}
}

static void button_isr(const struct device *port, struct gpio_callback *cb,
		       gpio_port_pins_t pins)
{
	struct sim_button *b = CONTAINER_OF(cb, struct sim_button, cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	(void)k_work_reschedule(&b->settle, K_MSEC(BTN_QUIET_MS));
}

static int buttons_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		struct sim_button *b = &buttons[i];
		int err;

		if (!gpio_is_ready_dt(&b->spec)) {
			return -ENODEV;
		}
		err = gpio_pin_configure_dt(&b->spec, GPIO_INPUT);
		if (err != 0) {
			return err;
		}
		k_work_init_delayable(&b->settle, button_settled);
		gpio_init_callback(&b->cb, button_isr, BIT(b->spec.pin));
		err = gpio_add_callback_dt(&b->spec, &b->cb);
		if (err != 0) {
			return err;
		}
		err = gpio_pin_interrupt_configure_dt(&b->spec, GPIO_INT_EDGE_BOTH);
		if (err != 0) {
			return err;
		}
	}
	return 0;
}
#endif /* CONFIG_SMART_TRAINER_SIM_BUTTONS */

static int sim_register(void)
{
	trainer_sim_model_init(&sim);
#if defined(CONFIG_SMART_TRAINER_SIM_BUTTONS)
	{
		int err = buttons_init();

		if (err != 0) {
			LOG_WRN("sim: buttons unavailable (%d)", err);
		}
	}
#endif
	return trainer_source_register(&sim_source);
}

/* APPLICATION level, before main(): main() must find a source registered. */
SYS_INIT(sim_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
