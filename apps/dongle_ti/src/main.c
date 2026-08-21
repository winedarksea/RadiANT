/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ANT+ Dongle, TI CC26x2 port — main entry point.
 *
 * Same shape as apps/dongle/src/main.c, minus the UF2-bootloader settle. All
 * protocol work happens in the ANT bridge thread (apps/common/
 * ant_serial_bridge.c) and in antr_on_message(), which the bridge implements
 * and the radio backend calls.
 *
 * The LED heartbeat used to be a second copy of the loop in
 * apps/common/ant_dongle_main.c and is now the same shared call that file
 * makes - see apps/common/ant_heartbeat_led.h for why. The board-specific
 * alias assumption it used to carry is unchanged in substance: the shared file
 * is still guarded on `led0`, which boards/ti/cc2652p_dongle defines (DIO6) and
 * no LaunchXL variant in this tree does.
 *
 * UNVERIFIED PENDING HARDWARE beyond a single bring-up bench - see this
 * application's own CMakeLists.txt header and README.md.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ant_heartbeat_led.h"
#include "ant_transport.h"

/* The radio contract - whichever backend is compiled in behind it. Bringing
 * the stack up is the only thing main() needs from it.
 */
#include "ant_radio.h"

LOG_MODULE_REGISTER(ant_dongle_ti, LOG_LEVEL_INF);

/* Forward declaration — defined in apps/common/ant_serial_bridge.c */
extern int ant_serial_bridge_init(void);

int main(void)
{
	int ret;

	LOG_INF("ANT+ Dongle (TI CC26x2) starting");

	ret = antr_init();
	if (ret) {
		LOG_ERR("antr_init failed: %d", ret);
		return ret;
	}
	LOG_INF("antr_init ok");

	/* Allocate the transport's buffers and start its writer thread, BEFORE
	 * the bridge exists to use them and before the UART is opened.
	 *
	 * Omitting this call was this port's longest-standing bug and it is
	 * worth being explicit about why it was invisible: the two ring buffers
	 * live in BSS, so a missing ring_buf_init() leaves them valid-looking
	 * objects of size ZERO rather than null pointers. Nothing crashes.
	 * ring_buf_put() simply accepts no bytes, so every byte the host sends
	 * is counted as dropped, and every byte the firmware sends waits a
	 * second for space that can never appear and is abandoned. The board
	 * boots, logs "transport up", blinks its LED, and is deaf and mute.
	 *
	 * apps/common/ant_dongle_main.c has always called this; this file is a
	 * hand-written main() for a board that file does not fit, and it is
	 * exactly the kind of thing a hand-written copy loses. ant_transport_
	 * enable() now refuses to open a UART whose buffers are unallocated, so
	 * this cannot fail silently again.
	 */
	usb_ant_class_init();
	LOG_INF("usb_ant_class_init ok");

	/* The bridge owns ANT event forwarding: it defines antr_on_message(),
	 * which the radio backend calls. Nothing is registered.
	 */
	ret = ant_serial_bridge_init();
	if (ret) {
		LOG_ERR("ant_serial_bridge_init failed: %d", ret);
		return ret;
	}
	LOG_INF("ant_serial_bridge_init ok");

	/* This board's only transport - see boards/cc26x2r1_launchxl.conf.
	 * Opens the UART carrying the ANT serial protocol.
	 */
	LOG_INF("enabling transport");
	ret = ant_transport_enable();
	if (ret) {
		LOG_ERR("ant_transport_enable failed: %d", ret);
		return ret;
	}
	LOG_INF("transport up");

	/* The heartbeat owns the main thread from here; it never returns, and
	 * on a board with no led0 alias it sleeps. Bring-up wants a louder
	 * pattern than the shipping 30 ms / 4 s one - see ti_bringup.conf.
	 */
	ant_heartbeat_led_run();

	return 0;
}
