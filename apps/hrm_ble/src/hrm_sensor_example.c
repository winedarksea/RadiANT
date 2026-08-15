/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * CONFIG_HRM_SENSOR_EXAMPLE: the seam's SECOND implementation, written to be
 * read rather than to be shipped.
 *
 * WHY IT EXISTS AT ALL, AND WHY CI BUILDS IT. A seam with one implementation is
 * indistinguishable from a direct call with extra prototypes - it compiles, it
 * looks like an abstraction, and the first person to actually use it discovers
 * which parts of it were never exercised. This is the same argument
 * .github/workflows/build.yml already makes about encryption.conf, applied to
 * an API instead of a Kconfig fragment. The hrm_ble.custom / hrm_ble.example
 * rows in sample.yaml are what keep it honest.
 *
 * WHAT IT DEMONSTRATES, in the order a manufacturer will need it:
 *
 *   1. Registration from a SYS_INIT, before main() calls hrm_sensor_start().
 *   2. Reporting a beat FROM INTERRUPT CONTEXT. The k_timer handler here runs
 *      in the ISR that expires the timer, which is deliberately the same
 *      context an analogue front end's comparator interrupt has. It is the
 *      hard case, and the whole point of the seam being ISR-safe.
 *   3. Timestamping with hrm_sensor_now_1024() - the node's own 1/1024 s base,
 *      so no conversion and no second rounding rule sit between the detector
 *      and byte 4 of page 0x00.
 *   4. A `poll` that does the slow, level-valued work (here: nothing but a
 *      contact-state placeholder), which is what poll is FOR - never beats.
 *
 * WHAT IT IS NOT. There is no detector in it: it fires on a timer, so on the
 * air it is indistinguishable from the simulator. Substituting a timer for an
 * AFE is exactly the substitution a real port removes, and it is called out
 * here so that nobody reads this file as a working front end.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/init.h>

#include "hrm_sensor.h"

LOG_MODULE_DECLARE(hrm, CONFIG_HRM_LOG_LEVEL);

static void beat_timer_fn(struct k_timer *t);
static K_TIMER_DEFINE(beat_timer, beat_timer_fn, NULL);

/*
 * ISR CONTEXT. Everything this handler is allowed to do is in
 * hrm_sensor_report_beat()'s contract: one atomic store and one
 * k_work_submit(). It must not touch profile_hr, must not call into the BLE
 * stack, and must not log at anything but the cheapest level - which is why it
 * does not log at all.
 */
static void beat_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);

	/*
	 * A real driver passes the instant its hardware captured. This one has
	 * none, so it reads the node's clock at the moment of the edge - which
	 * is still strictly better than letting an application tick guess,
	 * because the ISR runs at the edge and the tick does not.
	 */
	hrm_sensor_report_beat(hrm_sensor_now_1024());
}

static int example_start(const struct hrm_sensor *sensor)
{
	const uint32_t bpm = CONFIG_HRM_SENSOR_SIM_BPM;
	const uint32_t interval_ms = (bpm != 0u) ? (60000u / bpm) : 1000u;

	ARG_UNUSED(sensor);

	/*
	 * The rate is reported once, from thread context, and then never again:
	 * a driver whose detector is the source of truth reports the EDGE and
	 * lets the receiver derive the rate from the accumulator pair. Byte 7 is
	 * a convenience beside that pair, not a substitute for it.
	 */
	hrm_sensor_report_rate((uint8_t)bpm);

	k_timer_start(&beat_timer, K_MSEC(interval_ms), K_MSEC(interval_ms));
	return 0;
}

static int example_stop(const struct hrm_sensor *sensor)
{
	ARG_UNUSED(sensor);
	k_timer_stop(&beat_timer);
	hrm_sensor_report_rate(0u); /* PROFILE_HR_INVALID_BPM: no reading. */
	return 0;
}

static void example_poll(const struct hrm_sensor *sensor)
{
	ARG_UNUSED(sensor);

	/*
	 * Runs on the node's 100 ms tick, in thread context. Contact detection,
	 * AGC and battery sampling belong here. Reporting a beat from here
	 * would not be a layering mistake so much as a measurement one: the
	 * beat would carry this tick's timestamp, and a receiver differencing
	 * event times would see the tick period instead of the beat interval.
	 * That is the 72-versus-66.7 bpm bug, re-created on purpose by a driver
	 * instead of by a scheduler.
	 */
}

static const struct hrm_sensor_api example_api = {
	.start = example_start,
	.stop = example_stop,
	.poll = example_poll,
};

static const struct hrm_sensor example_sensor = {
	.name = "example (timer-driven, no front end)",
	.api = &example_api,
};

static int example_register(void)
{
	int err = hrm_sensor_register(&example_sensor);

	if (err != 0) {
		LOG_ERR("hrm_sensor_example: register: %d", err);
	}
	return 0;
}

SYS_INIT(example_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
