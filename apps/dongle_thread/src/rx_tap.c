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
#include "radiant_common_adapter.h"
#include "radiant_env_adapter.h"
#include "radiant_hr_adapter.h"
#include "radiant_power_adapter.h"

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

/*
 * One common-page decoder per binding, for exactly the reason above and not a
 * different one: its state (prev_cycles, the last-seen table) belongs to the
 * SENSOR too, and a sensor re-acquired on another channel must not restart its
 * charging-cycle accumulator from zero either.
 *
 * This array is indexed the same way and has the same lifetime as
 * hr_adapters[], but it is NOT parallel to it in the sense that matters: a
 * binding has exactly one profile adapter and ALSO has this one, always, for
 * every device type. See radiant_common_adapter.h - Table 4-1 keys the common
 * pages on transmission type, so there is no device type for which the common
 * decoder is the wrong decoder.
 */
static struct radiant_common_adapter common_adapters[RADIANT_BINDING_MAX];

/*
 * The other two profile adapters, on the same per-binding rule as
 * hr_adapters[] above and for the same reason - a power meter re-acquired on
 * another channel must not restart its energy integral from zero.
 *
 * ONE ARRAY SERVES BOTH BICYCLE POWER (0x0B) AND FITNESS EQUIPMENT (0x11),
 * which is deliberate and not a saving: a binding has exactly one device
 * type, so the two decode entry points are never both live on one source, and
 * the accumulated-energy integral they feed is the same quantity with the
 * same meaning. Two arrays would be two accumulators for one number.
 */
static struct radiant_power_adapter power_adapters[RADIANT_BINDING_MAX];
static struct radiant_env_adapter   env_adapters[RADIANT_BINDING_MAX];

int ant_bridge_channel_bind(uint8_t channel, uint32_t source)
{
	if (channel >= TAP_CHANNELS || source >= RADIANT_BINDING_MAX) {
		return -EINVAL;
	}

	/*
	 * EVERY adapter is reset here, not just the one this binding's device
	 * type will use. The device type is not known to be stable across a
	 * re-bind of the same slot - self_channels.c rotates what a channel
	 * searches for - so a slot that carried a power meter and now carries a
	 * thermometer must not inherit the power meter's accumulators. Resetting
	 * all of them costs a few hundred bytes of memset at bind time, which is
	 * a user action, and removes the whole class of bug.
	 */
	radiant_hr_adapter_init(&hr_adapters[source]);
	radiant_common_adapter_init(&common_adapters[source]);
	radiant_power_adapter_init(&power_adapters[source]);
	radiant_env_adapter_init(&env_adapters[source]);
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
	uint8_t  page;
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
	 * THAT RULE HAS EXACTLY ONE EXCEPTION AND THE SPECIFICATION IS THE ONE
	 * THAT GRANTS IT. `ANT+ Common Data Pages` Rev 3.1 section 4, Table 4-1
	 * splits the page-number byte into ranges, and says of 0x40-0x5D
	 * verbatim: "Common Data Pages. The common data pages have their
	 * formats defined. Use of these pages is defined by the transmission
	 * type of the ANT channel parameter." Transmission type - NOT device
	 * type. In that range and only that range, the layout does not depend
	 * on the profile, so reading it from the payload is not a guess about
	 * the device, it is the keying the document specifies. The rule above
	 * and this exception are the same rule stated twice: decode a page the
	 * way its OWNER says it is keyed. Table 4-1's other rows (0x00-0x3F
	 * "ANT+ Alliance Device Type Specific", 0x70-0x7F and 0xE0-0xFF
	 * manufacturer specific) are keyed on the device type, which is why the
	 * exception stops dead at 0x5D.
	 *
	 * Consequence, worth stating because it is a small change with a large
	 * blast radius: EVERY bound channel now feeds a second decoder, on
	 * every broadcast, regardless of device type. That is what makes a
	 * weather-reporting bike light or power meter work
	 * (radiant_common_adapter.h), and it is why radiant_bridge.h reserves a
	 * separate field_id block for the two-producers-one-source case.
	 */
	page = msg->data[1];

	/*
	 * THE ONE PLACE THE DEVICE TYPE STILL HAS TO BE CONSULTED BEFORE THE
	 * RANGE TEST, AND IT IS NOT A CONTRADICTION OF THE PARAGRAPH ABOVE.
	 *
	 * Heart rate (0x78) carries a page-change TOGGLE in bit 7 of the page
	 * byte, on every page it sends INCLUDING the common ones -
	 * tools/ant_pages.py's decode_hr() states this and masks the same bit
	 * for the same reason ("a decoder that dispatched on the raw byte would
	 * see each page under two different numbers and find neither"). So on a
	 * strap, page 84 arrives as 0x54 or 0xD4 depending on where the toggle
	 * happens to be, and 0xD4 is not in any range Table 4-1 defines.
	 *
	 * Without this mask the common path would work on every device type
	 * except the single most common ANT+ sensor there is, and it would do
	 * so intermittently - roughly half the broadcasts - which is the worst
	 * possible failure shape to debug on a bench.
	 *
	 * This is masking a bit the PROFILE defines, not choosing a decoder from
	 * the payload. The page number underneath is still the common page
	 * number, still keyed on transmission type, and still handled by the
	 * device-type-independent path below.
	 */
	if (b->devtype == 0x78u) {
		page &= 0x7Fu;
	}

	if (radiant_common_adapter_is_common_page(page)) {
		/*
		 * Handed over with byte [0] already de-toggled, so the adapter
		 * never has to know that one profile decorates the page number.
		 * Eight bytes on the stack, in a function that is already
		 * copying a frame, and it keeps radiant_common_adapter.c free
		 * of a device-type special case that would exist solely to
		 * undo one.
		 */
		uint8_t body[8];

		memcpy(body, &msg->data[1], sizeof(body));
		body[0] = page;

		(void)radiant_common_adapter_decode(&common_adapters[source],
						    source, body,
						    ant_bridge_now_us());
		/*
		 * Returned, not fallen through. A common page has no
		 * device-type-specific meaning by construction, so handing it
		 * on to the profile adapter afterwards could only produce a
		 * misdecode - radiant_hr_adapter_decode() reads bytes [4..7] of
		 * ANY page it is given, so page 84's humidity would publish as
		 * a heart rate.
		 */
		return;
	}

	/*
	 * Below here: device-type dispatch, as before.
	 *
	 * Only 0x78 today. radiant_rd_adapter.c exists and is tested, but
	 * running dynamics arrives on its own channel with its own binding and
	 * has no bench evidence behind it yet; adding the case here without
	 * that is how an untested path acquires the appearance of a shipped
	 * one.
	 *
	 * >>> NEW PROFILE ADAPTERS GO HERE, one `else if` per device type. <<<
	 * Each needs its own per-binding adapter array beside hr_adapters[] and
	 * its own init call in ant_bridge_channel_bind(), for the reason stated
	 * there. Nothing new goes ABOVE the common-range test: a profile adapter
	 * that sees a common page is the bug this restructure exists to remove.
	 *
	 * Bicycle power (0x0B) and fitness equipment (0x11) share ONE adapter
	 * and one per-binding state array. That is not a shortcut - a binding
	 * has exactly one device type, so the two entry points are never both
	 * live on one source, and the energy integral they both feed is the
	 * same accumulator with the same meaning. Two arrays would be two
	 * accumulators for one quantity.
	 *
	 * Running dynamics (radiant_rd_adapter.c) is still NOT here, and the
	 * reason has not changed: it exists and is tested, but it arrives on
	 * its own channel with its own binding and has no bench evidence behind
	 * it. Adding the case without that is how an untested path acquires the
	 * appearance of a shipped one.
	 */
	if (b->devtype == 0x78u) {
		(void)radiant_hr_adapter_decode(&hr_adapters[source], source,
						&msg->data[1],
						ant_bridge_now_us());
	} else if (b->devtype == 0x0Bu) {
		(void)radiant_power_adapter_decode(&power_adapters[source],
						   source, &msg->data[1],
						   ant_bridge_now_us());
	} else if (b->devtype == 0x11u) {
		(void)radiant_power_adapter_decode_fec(&power_adapters[source],
						       source, &msg->data[1],
						       ant_bridge_now_us());
	} else if (b->devtype == 0x19u) {
		(void)radiant_env_adapter_decode(&env_adapters[source], source,
						 &msg->data[1],
						 ant_bridge_now_us());
	}
}
