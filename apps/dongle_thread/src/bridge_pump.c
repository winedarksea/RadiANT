/* SPDX-License-Identifier: Apache-2.0 */
/*
 * bridge_pump.c - the thread that drains the sample bus, and the 1 Hz clock
 * the liveness table runs on.
 *
 * WHY THIS FILE EXISTS. radiant_bridge_drain() has been implemented and unit
 * tested since P5, and until this file was written NOTHING IN ANY FIRMWARE
 * IMAGE CALLED IT. The suites call it by hand, which is why the gap survived:
 * every ztest passes with no pump at all. A bus that is posted to and never
 * drained fills its 32-slot ring, starts counting drops, and every sink stays
 * silent - which reads on a console exactly like "no sensors in range".
 *
 * PRIORITY. Numerically 2 below ANTR_HOST_THREAD_PRIORITY, i.e. lower
 * priority than both the bridge thread (5) and radiant's own event thread
 * (API_EVENT_PRIORITY, 6). A sink's publish() may do real work - mqtt_sink.c
 * does a TCP write in it - and a thread that could preempt the event thread
 * while blocked in a socket call would be delaying an ANT receive window to
 * talk to a broker. The ordering rule at ant_radio_radiant.c:230 is a recorded
 * bench observation (an event delivered before the response that caused it),
 * and this thread sits below the whole of it rather than beside any part of
 * it.
 *
 * WHY A THREAD AND NOT A WORK ITEM. The system work queue is shared with the
 * coexistence loads, with Zephyr's own subsystems and, in a bridge build, with
 * the network stack. A publish() that blocks on a broker that has gone away
 * would then block those too. Its own thread costs one stack and makes the
 * blocking local.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/iterable_sections.h>

#include "ant_radio.h" /* ANTR_HOST_THREAD_PRIORITY */

#include "radiant_binding.h"
#include "radiant_bridge.h"
#include "radiant_liveness.h"

#include "bridge_app.h"

LOG_MODULE_REGISTER(bridge_pump, LOG_LEVEL_INF);

#define PUMP_STACK_SIZE 2048
#define PUMP_PRIORITY   (ANTR_HOST_THREAD_PRIORITY + 2)

BUILD_ASSERT(PUMP_PRIORITY > ANTR_HOST_THREAD_PRIORITY,
	     "The bridge pump must be strictly lower priority than the ANT "
	     "host thread: a sink doing a socket write must never be able to "
	     "delay an ANT window.");

/* The liveness sweep's period. 1 Hz is section 9.2's granularity, not a tuning
 * knob: the expiry itself is 3x the channel period (250 ms x 3 = 750 ms for a
 * 4 Hz profile), so a 1 s sweep resolves it to within one sweep, and a faster
 * sweep would only make the report earlier by a fraction of a second while
 * running the whole table more often. */
#define LIVENESS_TICK_MS 1000

uint64_t ant_bridge_now_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

/*
 * The count that proves the linker fragment landed.
 *
 * RADIANT_SINK_DEFINE() uses STRUCT_SECTION_ITERABLE, which emits into a
 * section that nothing places unless
 * radiant/src/bridge/radiant_bridge_sections.ld is pulled in with
 * zephyr_linker_sources(SECTIONS ...). Forgetting it is an undefined reference
 * at the final link IF the start/end symbols are referenced - but
 * STRUCT_SECTION_FOREACH() over an unplaced section is not a compile error and
 * not a link error either: it iterates ZERO sinks, silently, and every sink in
 * the image becomes dead code that still occupies flash.
 *
 * So the count goes on the console at boot. "bridge: 0 sinks" means everything
 * downstream of this file is vacuous, and it is worth two lines to be told
 * that rather than to infer it from an absence of MQTT messages.
 */
static size_t count_sinks(void)
{
	size_t n = 0;

	STRUCT_SECTION_FOREACH(radiant_sink, sink) {
		(void)sink;
		n++;
	}
	return n;
}

static void pump_thread_fn(void *a, void *b, void *c)
{
	int64_t next_tick_ms;
	size_t  sinks;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	radiant_binding_init();
	radiant_liveness_reset();
	radiant_bridge_reset();

	sinks = count_sinks();
	LOG_INF("bridge: %u sinks", (unsigned int)sinks);
	if (sinks == 0u) {
		LOG_ERR("bridge: no sinks linked - either none is compiled in, "
			"or radiant_bridge_sections.ld was not pulled in with "
			"zephyr_linker_sources(). Nothing downstream can work.");
	}

	next_tick_ms = k_uptime_get() + LIVENESS_TICK_MS;

	for (;;) {
		k_sleep(K_MSEC(CONFIG_ANT_DONGLE_BRIDGE_PUMP_MS));

		(void)radiant_bridge_drain();

		if (k_uptime_get() >= next_tick_ms) {
			/*
			 * Advance by a whole period rather than re-anchoring to
			 * now. Re-anchoring turns every scheduling delay into a
			 * permanent phase shift - the same drift bug the
			 * hrm_ble beat scheduler was written to avoid - and
			 * while a liveness sweep does not care about a few
			 * milliseconds, a sweep whose period silently grew to
			 * 1.2 s would make every expiry late by an amount
			 * nothing reports.
			 */
			next_tick_ms += LIVENESS_TICK_MS;
			if (k_uptime_get() >= next_tick_ms) {
				/* Fell more than a whole period behind (a long
				 * publish, most likely). Catch up rather than
				 * running the sweep repeatedly to make up lost
				 * passes: the sweep is idempotent within a
				 * quiet period, so the lost passes had nothing
				 * to do. */
				next_tick_ms = k_uptime_get() + LIVENESS_TICK_MS;
			}

			uint32_t staled = radiant_liveness_tick(ant_bridge_now_us());

			if (staled != 0u) {
				/*
				 * SAY SO. A working STALE producer and one that
				 * was never linked in are indistinguishable from
				 * outside - both leave the last value sitting on
				 * the bus, and neither says anything - which is
				 * exactly the observation the bench procedure
				 * for this needs to make. radiant/src/bridge/ is
				 * deliberately log-free (it is an embeddable
				 * library and reaches no path outside itself),
				 * so the line belongs here, on the application
				 * side, beside the "bridge: N sinks" line that
				 * exists for the same reason.
				 *
				 * Edge-triggered by radiant_liveness.c itself -
				 * it posts once per binding per quiet period -
				 * so this cannot become a 1 Hz drip.
				 */
				const struct radiant_liveness_stats *ls =
					radiant_liveness_stats_get();

				LOG_INF("liveness: %u binding(s) went STALE "
					"(total %u, no_period %u)",
					(unsigned int)staled,
					(unsigned int)(ls != NULL ? ls->stale_posted : 0u),
					(unsigned int)(ls != NULL ? ls->no_period : 0u));

				/* Drain again immediately: the STALE records
				 * were posted inside the tick above, and a
				 * heart-rate expiry that waited a further pump
				 * period to reach a sink would be adding
				 * latency to the one message whose whole point
				 * is promptness. */
				(void)radiant_bridge_drain();
			}
		}
	}
}

K_THREAD_DEFINE(bridge_pump_tid, PUMP_STACK_SIZE, pump_thread_fn, NULL, NULL,
		NULL, PUMP_PRIORITY, 0, 0);
