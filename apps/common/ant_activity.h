/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The receive-activity counter: how much sensor traffic this dongle is actually
 * hearing, as one number that any indicator can read.
 *
 * TWO CONSUMERS, ONE HOOK. apps/common/ant_rgb_led.c turns it into the speed of
 * a NeoPixel hue wheel; apps/common/ant_heartbeat_led.c turns it into the gap
 * between flashes of the plain led0. Both drain the same atomic, and neither
 * owns it - which is why this is its own header rather than living inside
 * either one, as it did while the NeoPixel was the only consumer.
 *
 * WHY A SEPARATE HOOK RATHER THAN CONFIG_ANT_DONGLE_RX_TAP. The tap is resolved
 * at link time and has exactly one definition per image, already owned by
 * apps/dongle_thread/src/rx_tap.c. Two features cannot both have it. This costs
 * one call at the single call site in ant_serial_bridge.c and cannot collide.
 *
 * WHERE THE COUNT HAPPENS. antr_on_message() runs on the radiant API event
 * thread - ordinary thread context, never the radio callback - so the atomic
 * increment below is free relative to the framing work already being done per
 * message. Nothing in this feature runs in interrupt context at all.
 */

#ifndef ANT_ACTIVITY_H_
#define ANT_ACTIVITY_H_

#include <stdint.h>

#if defined(CONFIG_ANT_DONGLE_ACTIVITY_COUNT)

#include <zephyr/sys/atomic.h>

#include "ant_wire.h"

/*
 * Packets seen since a consumer last drained this. Written by
 * ant_activity_note_msg() from the event thread, read-and-cleared by whichever
 * indicator is compiled in - a single producer and a single consumer, so an
 * atomic is the whole of the synchronisation needed.
 *
 * SINGLE consumer is a real constraint, not a description. atomic_clear() takes
 * the count away, so two indicators draining the same counter would each see
 * roughly half the traffic and both read as idle. Nothing enforces it today
 * because no board has both a led0 activity blink and a NeoPixel; a board that
 * wants both needs a second counter, not a second reader.
 */
extern atomic_t ant_activity_pkts;

/*
 * Count one chip->host message if it carries sensor data.
 *
 * Deliberately NOT every message. EVENT_TX on a master channel, EVENT_RX_FAIL,
 * channel responses and capability replies are all bookkeeping, and counting
 * them would make an idle dongle with one open master channel look as busy as
 * one tracking four sensors - which is precisely the distinction the indicators
 * exist to show. Only the seven data-bearing IDs count.
 *
 * The extended forms (0x5D/0x5E/0x5F) matter in practice rather than in theory:
 * tools/ant_probe.py and friends ask for library config 0xE0 (channel id, RSSI
 * and receive timestamp), and a dongle answering that emits extended messages
 * for every received packet. Filtering to 0x4E alone would leave the indicator
 * dead during exactly the sessions most likely to be watched.
 */
static inline void ant_activity_note_msg(uint8_t msg_id)
{
	switch (msg_id) {
	case ANTW_MESG_BROADCAST_DATA_ID:
	case ANTW_MESG_ACKNOWLEDGED_DATA_ID:
	case ANTW_MESG_BURST_DATA_ID:
	case ANTW_MESG_EXT_BROADCAST_DATA_ID:
	case ANTW_MESG_EXT_ACKNOWLEDGED_DATA_ID:
	case ANTW_MESG_EXT_BURST_DATA_ID:
	case ANTW_MESG_ADV_BURST_DATA_ID:
		(void)atomic_inc(&ant_activity_pkts);
		break;
	default:
		break;
	}
}

/*
 * The rate -> interval arithmetic lives in ant_rgb_led_curve.h, which reaches no
 * Zephyr header and no Kconfig symbol so that radiant/tests can link it. See
 * that file for why the split exists and why it kept its name.
 */

#else /* !CONFIG_ANT_DONGLE_ACTIVITY_COUNT */

/*
 * A macro, not an empty inline function, for the reason spelled out in
 * ant_health.h:155-167: an inline still evaluates its argument, and the
 * `zero-cost` CI job compares section sizes against the merge base. With the
 * symbol off there must be nothing here at all.
 */
#define ant_activity_note_msg(msg_id) ((void)0)

#endif /* CONFIG_ANT_DONGLE_ACTIVITY_COUNT */

#endif /* ANT_ACTIVITY_H_ */
