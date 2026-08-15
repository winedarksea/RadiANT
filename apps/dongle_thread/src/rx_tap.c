/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rx_tap.c - where ANT broadcasts become struct radiant_sample.
 *
 * apps/common/ant_serial_bridge.c calls ant_dongle_rx_tap() at the top of
 * antr_on_message(), behind CONFIG_ANT_DONGLE_RX_TAP. The symbol is resolved
 * at link time and defined here, in exactly the shape antr_on_message() itself
 * uses: no registration call, one definition, and apps/dongle - which never
 * sets the symbol - compiles the call site away entirely, so its image stays
 * byte-identical and the `zero-cost` CI job stays green.
 *
 * The tap does not change what reaches the host. It reads a copy and returns;
 * the frame still goes out the transport exactly as before. A dongle with a
 * PC attached and CONFIG_ANT_DONGLE_RX_TAP=y is the same dongle.
 *
 * ---------------------------------------------------------------------------
 * ONE HONEST DEVIATION FROM THE SPECIFICATION, RECORDED HERE SO IT IS NOT
 * "FIXED"
 * ---------------------------------------------------------------------------
 *
 * struct radiant_sample::t_us is specified (radiant_bridge.h, and
 * docs/radiant-bridge.md section 3) as "the decode-time t_sync, not dequeue
 * time, so a sink delayed behind a full bus still reports when the sample was
 * actually heard". struct antr_msg carries no timestamp at all - it is
 * deliberately wire-shaped, three fields, and ant_radio.h says in as many
 * words that an extra field "would just be one more thing that could disagree
 * with the bytes". So the value taken here is k_uptime_ticks() at tap time,
 * which is a few hundred microseconds after t_sync and jitters with the
 * backend's event-thread scheduling.
 *
 * That is safe HERE SPECIFICALLY, and the reason is worth stating because it
 * does not generalise: the exactness that matters in this path lives in the
 * WIRE field, not in t_us. radiant_hr_adapter.h differences `event_time` at
 * its own 16-bit width and accumulates in 1/1024 s units, converting only at
 * publication - so the accumulator that carries the timing information never
 * reads t_us at all, and t_us imprecision cannot corrupt it. What t_us is used
 * for downstream is ordering and expiry, both at second granularity
 * (radiant_liveness.c's 3x-period rule).
 *
 * DO NOT "fix" this by plumbing a timestamp through the antr_* seam. That
 * seam is a published contract (docs/sdk-ant-contract.md), it is implemented
 * by the sdk_ant backend as well as by radiant, and tools/ant_conformance.py
 * pins the resulting byte stream. Widening struct antr_msg to improve a field
 * that no accumulator reads would put a conformance transcript at risk to buy
 * nothing.
 */

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ant_radio.h"
#include "ant_wire.h"

#include "radiant_binding.h"
#include "radiant_bridge.h"
#include "radiant_hr_adapter.h"

#include "bridge_app.h"

LOG_MODULE_REGISTER(bridge_tap, LOG_LEVEL_INF);

/* Same guard, same reason, as ant_serial_bridge.c's own copy: sdk-ant's
 * Kconfig hangs this symbol off CONFIG_ANT, so it does not exist at all on a
 * stub build, and it only bounds a table here. */
#ifndef CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED
#define CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED 8
#endif

#define TAP_CHANNELS CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED

/*
 * The channel -> binding map. Written only by self_channels.c (from its own
 * thread, inside the pairing window) and read only from the tap; a uint32_t
 * store on the platforms this application targets is atomic, and a torn read
 * is not expressible, so this needs no lock. What it must not do is grow a
 * "create the binding if missing" path: that turns the map into promiscuous
 * ingest, which section 10.1 rule 1 refuses and section 7.2 shows the radio
 * budget cannot afford either.
 */
static uint32_t chan_source[TAP_CHANNELS] = {
	[0 ... TAP_CHANNELS - 1] = RADIANT_BINDING_NONE,
};

static uint32_t unbound_drops;

/* One decoder per binding, not per channel: the accumulator state
 * (prev_event_time, acc_1024) belongs to the SENSOR, and a sensor that is
 * re-acquired on a different channel must not restart its beat-time
 * accumulator from zero. */
static struct radiant_hr_adapter hr_adapters[RADIANT_BINDING_MAX];

int ant_bridge_channel_bind(uint8_t channel, uint32_t source)
{
	if (channel >= TAP_CHANNELS || source >= RADIANT_BINDING_MAX) {
		return -EINVAL;
	}

	radiant_hr_adapter_init(&hr_adapters[source]);
	chan_source[channel] = source;
	return 0;
}

void ant_bridge_channel_unbind(uint8_t channel)
{
	if (channel < TAP_CHANNELS) {
		chan_source[channel] = RADIANT_BINDING_NONE;
	}
}

uint32_t ant_bridge_channel_source(uint8_t channel)
{
	if (channel >= TAP_CHANNELS) {
		return RADIANT_BINDING_NONE;
	}
	return chan_source[channel];
}

uint32_t ant_bridge_unbound_drops(void)
{
	return unbound_drops;
}

void ant_dongle_rx_tap(const struct antr_msg *msg)
{
	uint32_t source;
	uint8_t  channel;
	const struct radiant_binding *b;

	if (msg == NULL || msg->data == NULL) {
		return;
	}

	/*
	 * Broadcast only. Acknowledged and burst data carry sensor payloads
	 * too, but no ANT+ profile this bridge decodes uses them for its
	 * periodic pages, and admitting message classes the adapters have not
	 * been written against is how a bridge starts publishing decoded
	 * garbage. Widen this when an adapter needs it, not before.
	 *
	 * LEN counts the channel byte, so a full 8-byte page is LEN 9. Shorter
	 * is not an error worth logging at this rate - it is a message class
	 * this filter should have excluded - so it is simply not decoded.
	 */
	if (msg->id != ANTW_MESG_BROADCAST_DATA_ID || msg->len < 9u) {
		return;
	}

	channel = msg->data[0];
	source = ant_bridge_channel_source(channel);
	if (source == RADIANT_BINDING_NONE) {
		/* THE OPT-IN RULE, ENFORCED. An unbound channel's traffic is
		 * dropped here and nowhere else; there is no path from this
		 * function to radiant_binding_bind(). */
		unbound_drops++;
		return;
	}

	b = radiant_binding_get(source);
	if (b == NULL) {
		/* Unbound underneath us between the map read and here. Treat
		 * as unbound rather than as an assertion: unbinding is a user
		 * action and it may legitimately race one broadcast. */
		unbound_drops++;
		return;
	}

	/*
	 * Dispatch on the BINDING's device type, not on anything in the
	 * payload. The page number is not a device type - ANT+ page 0x00 means
	 * different things on different profiles - so a decoder chosen from
	 * the payload would decode a power meter's page 0 as a heart rate.
	 *
	 * Only 0x78 today. radiant_rd_adapter.c exists and is tested, but
	 * running dynamics arrives on its own channel with its own binding and
	 * has no bench evidence behind it yet; adding the case here without
	 * that is how an untested path acquires the appearance of a shipped
	 * one.
	 */
	if (b->devtype == 0x78u) {
		(void)radiant_hr_adapter_decode(&hr_adapters[source], source,
						&msg->data[1],
						ant_bridge_now_us());
	}
}
