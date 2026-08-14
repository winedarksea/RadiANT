/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The proof that ANT+ HR reception in radiant is a downstream integration,
 * not an internal implementation detail: everything below is written against
 * <radiant/...> headers and radiant/src/bridge/*.h, exactly as a third party
 * dropping this module into their own project would write it. See this
 * directory's own CMakeLists.txt for how radiant/ gets here, and
 * radiant/docs/integration.md for the same walkthrough with the reasoning
 * left in.
 *
 * DELIBERATELY STOPS SHORT OF A WORKING RECEIVER. radiant_radio_init() and
 * radiant_sched_init() are not called - this build has no radio behind it
 * (CONFIG_RADIANT_BACKEND_NULL) and nothing here drives a scheduler tick, so
 * radiant_channel_open() below only ever reaches the SEARCHING state and
 * stays there: it proves the channel API links and can be sequenced
 * correctly, not that a sensor gets found. A real port (see
 * apps/common/ant_radio_radiant.c, outside this module) additionally wires
 * radiant_sched_init(), radiant_radio_init() and a work thread that ticks
 * the scheduler and drains events - that is the antr_* adapter's job, and it
 * is deliberately not reachable from here, which is the whole point of an
 * import-smoke test.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <radiant/radiant_channel.h>
#include <radiant/radiant_event.h>
#include <radiant/radiant_msg.h>
#include <radiant/radiant_wire.h>

#include "radiant_bridge.h"
#include "radiant_hr_adapter.h"

LOG_MODULE_REGISTER(import_smoke, LOG_LEVEL_INF);

/* ANT+ Heart Rate, device type 0x78 - see docs/device-profiles.md. Spelled
 * as a literal rather than pulled from src/profiles/profile_hr.h's
 * PROFILE_HR_DEVICE_TYPE: that header is the TRANSMIT side (encoding pages
 * for a node to send) and has no business in a receive-side demo. */
#define DEMO_HR_DEVICE_TYPE 0x78u
#define DEMO_CHANNEL        0u
#define DEMO_NETWORK        0u

static struct radiant_hr_adapter hr_adapter;

/* ---------------------------------------------------------------------------
 * The two port hooks radiant_event.h declares and radiant_event.c calls -
 * see that header's "Two hooks the port provides". A real port maps these
 * onto irq_lock()/irq_unlock() and a work-queue wakeup (see
 * apps/common/ant_radio_radiant.c); this demo has no drain thread to wake,
 * so radiant_event_wakeup() is a no-op and radiant_event_drain() is polled
 * from the loop below instead.
 * ---------------------------------------------------------------------------
 */
unsigned int radiant_event_crit_enter(void)
{
	return irq_lock();
}

void radiant_event_crit_exit(unsigned int key)
{
	irq_unlock(key);
}

void radiant_event_wakeup(void)
{
}

/*
 * radiant_msg.h's seam: radiant_event.c calls this for every message the
 * host (or, here, this demo) should see. A broadcast or acknowledged data
 * message on the HR channel is hr_adapter.h's decode input; anything else is
 * not this demo's concern.
 */
void radiant_on_message(const struct radiant_msg *msg)
{
	if ((msg->id != RADIANT_WIRE_MESG_BROADCAST_DATA_ID &&
	     msg->id != RADIANT_WIRE_MESG_ACKNOWLEDGED_DATA_ID) ||
	    msg->len != 9u) {
		return;
	}

	uint8_t channel = msg->data[0];

	(void)radiant_hr_adapter_decode(&hr_adapter, channel, &msg->data[1],
					 (uint64_t)k_uptime_get() * 1000u);
}

/* The sink: anything the bridge posts with field_type HEART_RATE gets
 * logged. A real sink would forward to MQTT, Matter or wherever this
 * project's product actually wants heart rate to go - see
 * docs/radiant-bridge.md for the sink registry's design. */
static bool hr_sink_want(const struct radiant_sample *s)
{
	return s->field_type == RADIANT_FIELD_HEART_RATE;
}

static void hr_sink_publish(const struct radiant_sample *s)
{
	LOG_INF("HR source=%u bpm=%lld", s->source, s->raw);
}

RADIANT_SINK_DEFINE(hr_console_sink, hr_sink_want, hr_sink_publish, NULL);

int main(void)
{
	radiant_hr_adapter_init(&hr_adapter);

	radiant_channel_init();
	radiant_event_init();

	radiant_channel_err_t err;

	/* type 0x00: bidirectional slave receive channel - the RADIANT_CH_TYPE_MASTER_BIT
	 * (0x10) bit clear is what makes this a receiver rather than a transmitter. */
	err = radiant_channel_assign(DEMO_CHANNEL, 0x00u, DEMO_NETWORK, 0u);
	if (err != RADIANT_CH_OK) {
		LOG_ERR("radiant_channel_assign: 0x%02x", err);
		return -1;
	}

	/* Device number 0 is the wildcard - this channel will match any HR
	 * sensor's device number, same as an unconfigured host would ask for. */
	err = radiant_channel_id_set(DEMO_CHANNEL, 0u, DEMO_HR_DEVICE_TYPE, 0u);
	if (err != RADIANT_CH_OK) {
		LOG_ERR("radiant_channel_id_set: 0x%02x", err);
		return -1;
	}

	err = radiant_channel_open(DEMO_CHANNEL, 0u, (radiant_time_t)0);
	if (err != RADIANT_CH_OK) {
		LOG_ERR("radiant_channel_open: 0x%02x", err);
		return -1;
	}

	LOG_INF("radiant: channel %u assigned, HR wildcard, opened (searching)",
		DEMO_CHANNEL);

	while (1) {
		(void)radiant_event_drain(0);
		(void)radiant_bridge_drain();
		k_sleep(K_SECONDS(1));
	}

	return 0;
}
