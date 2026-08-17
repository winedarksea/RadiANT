/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The activity-rate RGB indicator: a smart LED (WS2812 and relatives) whose hue
 * rotates continuously, at a speed set by how much ANT+ traffic the dongle is
 * actually receiving. Slow crawl means nothing is on the air; fast means several
 * sensors are being tracked.
 *
 * WHY A SEPARATE HOOK RATHER THAN CONFIG_ANT_DONGLE_RX_TAP. The tap is resolved
 * at link time and has exactly one definition per image, already owned by
 * apps/dongle_thread/src/rx_tap.c. Two features cannot both have it. This costs
 * one #if at the single call site in ant_serial_bridge.c and cannot collide.
 *
 * WHERE THE COUNT HAPPENS. antr_on_message() runs on the radiant API event
 * thread - ordinary thread context, never the radio callback - so the atomic
 * increment below is free relative to the framing work already being done per
 * message. Nothing in this feature runs in interrupt context at all.
 *
 * The strip is named by the `rgb-led0` devicetree alias, not by a pin or a
 * Kconfig number, which is the whole of the board-portability story: another
 * board needs an overlay declaring its own node and that alias, and no change
 * here. See apps/dongle/rgb_feather.overlay for the worked example.
 */

#ifndef ANT_RGB_LED_H_
#define ANT_RGB_LED_H_

#include <stdint.h>

#if defined(CONFIG_ANT_DONGLE_RGB_LED)

#include <zephyr/sys/atomic.h>

#include "ant_wire.h"

/*
 * Packets seen since the render thread last drained this. Written by
 * ant_rgb_led_note_msg() from the event thread, read-and-cleared once per tick
 * by the render thread - a single producer and a single consumer, so an atomic
 * is the whole of the synchronisation needed.
 */
extern atomic_t ant_rgb_led_pkts;

/*
 * Count one chip->host message if it carries sensor data.
 *
 * Deliberately NOT every message. EVENT_TX on a master channel, EVENT_RX_FAIL,
 * channel responses and capability replies are all bookkeeping, and counting
 * them would make an idle dongle with one open master channel look as busy as
 * one tracking four sensors - which is precisely the distinction the LED
 * exists to show. Only the seven data-bearing IDs count.
 *
 * The extended forms (0x5D/0x5E/0x5F) matter in practice rather than in theory:
 * tools/ant_probe.py and friends ask for library config 0xE0 (channel id, RSSI
 * and receive timestamp), and a dongle answering that emits extended messages
 * for every received packet. Filtering to 0x4E alone would leave the LED dead
 * during exactly the sessions most likely to be watched.
 */
static inline void ant_rgb_led_note_msg(uint8_t msg_id)
{
	switch (msg_id) {
	case ANTW_MESG_BROADCAST_DATA_ID:
	case ANTW_MESG_ACKNOWLEDGED_DATA_ID:
	case ANTW_MESG_BURST_DATA_ID:
	case ANTW_MESG_EXT_BROADCAST_DATA_ID:
	case ANTW_MESG_EXT_ACKNOWLEDGED_DATA_ID:
	case ANTW_MESG_EXT_BURST_DATA_ID:
	case ANTW_MESG_ADV_BURST_DATA_ID:
		(void)atomic_inc(&ant_rgb_led_pkts);
		break;
	default:
		break;
	}
}

/*
 * The rate -> rotation-period curve and the hue arithmetic live in
 * ant_rgb_led_curve.h, which reaches no Zephyr header and no Kconfig symbol so
 * that radiant/tests can link it. See that file for why the split exists.
 */

#else /* !CONFIG_ANT_DONGLE_RGB_LED */

/*
 * A macro, not an empty inline function, for the reason spelled out in
 * ant_health.h:155-167: an inline still evaluates its argument, and the
 * `zero-cost` CI job compares section sizes against the merge base. With the
 * symbol off there must be nothing here at all.
 */
#define ant_rgb_led_note_msg(msg_id) ((void)0)

#endif /* CONFIG_ANT_DONGLE_RGB_LED */

#endif /* ANT_RGB_LED_H_ */
