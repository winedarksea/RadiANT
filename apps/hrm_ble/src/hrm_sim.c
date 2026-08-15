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
 * profile_hr, the seam would not be carrying its own weight.
 *
 * The schedule itself is NOT here. It is a pure function in hrm_sim_sched.c so
 * that a test can drive it; this file is only the kernel plumbing around it.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>

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

static void sim_work_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(sim_work, sim_work_fn);

static void sim_work_fn(struct k_work *w)
{
	const uint32_t bpm = CONFIG_HRM_SENSOR_SIM_BPM;

	ARG_UNUSED(w);

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
	/* No poll: there is no contact to detect and no AGC to run. Leaving it
	 * NULL rather than supplying an empty function is deliberate - the seam
	 * documents poll as optional, and an application that only ever saw
	 * non-NULL ones would grow the assumption. */
};

static const struct hrm_sensor sim_sensor = {
	.name = "simulated",
	.api = &sim_api,
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
