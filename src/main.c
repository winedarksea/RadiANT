/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ANT+ USB Dongle — main entry point.
 *
 * All protocol work happens in the ANT bridge thread (ant_serial_bridge.c)
 * and in antr_on_message(), which the bridge implements and the radio backend
 * calls.  Main just initialises the subsystems in order, then sleeps forever.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>

#include "ant_transport.h"

/* Measured on an nRF54L15 DK, alternating builds against a live power meter in
 * one sitting: XTAL heard 14 broadcasts, SYNTH heard none, XTAL heard 14 again.
 *
 * Nothing announces the failure. The LFSYNT source exists in the nRF54L
 * register map, the clock starts, antr_init() returns 0, channels open and
 * acknowledge every command - and the radio receives nothing, ever. A dongle
 * built this way looks completely healthy to a host and is deaf.
 *
 * That is worth refusing to build rather than warning about, because the
 * warning would be read once and the silence would be debugged forever. The
 * exact mechanism is not established; on nRF52840 this configuration works and
 * is a supported fallback, which is why synth.conf still exists.
 */
BUILD_ASSERT(!(IS_ENABLED(CONFIG_CLOCK_CONTROL_NRF_K32SRC_SYNTH) &&
	       IS_ENABLED(CONFIG_SOC_SERIES_NRF54LX)),
	     "A synthesized low-frequency clock does not keep ANT's timing on "
	     "nRF54L. The build boots and hears nothing. This part needs a real "
	     "32.768 kHz crystal; see synth.conf.");

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
#include <zephyr/drivers/gpio.h>
static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static bool led_ok;
#endif

#include "diag_flash_log.h"

#if defined(CONFIG_ANT_DONGLE_BLE_COEX_LOAD)
#include "ble_coex_load.h"
#endif

#if defined(CONFIG_ANT_DONGLE_THREAD_COEX_LOAD)
#include "thread_coex_load.h"
#endif

/* The radio contract - whichever backend is compiled in behind it. Bringing
 * the stack up is the only thing main() needs from it.
 */
#include "ant_radio.h"

LOG_MODULE_REGISTER(ant_dongle, LOG_LEVEL_INF);

/* Forward declaration — defined in ant_serial_bridge.c */
extern int ant_serial_bridge_init(void);

/*
 * The UF2 bootloader ejects its mass-storage drive and soft-resets into this
 * image. macOS needs a moment to finish processing that detach; enumerating
 * into the gap leaves the dongle unusable until it is physically replugged.
 *
 * That race exists only on the boot immediately following a flash. A cold plug
 * - what a user does every day, and the only thing most users ever do - has no
 * mass-storage device to detach from, so the wait there buys nothing and just
 * delays the dongle appearing on the bus. RESETREAS separates the two cases:
 * the bootloader's jump into the application is a software reset, a plug is
 * power-on, and the RESET button is a pin reset.
 *
 * RESETREAS latches its bits until they are cleared, so this clears them after
 * reading. Without that, the software-reset bit set by one flash would still
 * be set on every boot afterwards and the wait would never be skipped.
 */
static void settle_after_uf2_bootloader(void)
{
	if (CONFIG_ANT_DONGLE_UF2_SETTLE_MS == 0) {
		return;
	}

#if defined(CONFIG_HWINFO)
	uint32_t cause = 0;

	/* Zephyr's weak z_impl_hwinfo_get_reset_cause() answers -ENOSYS on a
	 * part with no driver, and the whole file is only compiled when
	 * CONFIG_HWINFO is set - hence the #if rather than IS_ENABLED(), which
	 * would leave an unresolved symbol at link time.
	 */
	if (hwinfo_get_reset_cause(&cause) == 0) {
		(void)hwinfo_clear_reset_cause();

		/* A power-on reset leaves RESETREAS empty, and it is the only
		 * cause that guarantees no mass-storage device preceded us -
		 * nothing was mounted, so the host has nothing to detach. Every
		 * other cause can race: the bootloader's own jump after writing
		 * an image, and equally the RESET button pressed to leave the
		 * bootloader while its drive is still mounted, which is a pin
		 * reset rather than a software one. Both wait.
		 */
		if (cause == 0) {
			return;
		}

		LOG_INF("reset cause 0x%08x: settling %d ms for the bootloader",
			cause, CONFIG_ANT_DONGLE_UF2_SETTLE_MS);
	}
#endif

	/* Either this boot followed a software reset, or the reset cause could
	 * not be established - in which case wait unconditionally rather than
	 * assume a cold boot and risk the race.
	 */
	k_sleep(K_MSEC(CONFIG_ANT_DONGLE_UF2_SETTLE_MS));
}

int main(void)
{
	int ret;

	settle_after_uf2_bootloader();

	LOG_INF("ANT+ USB Dongle starting");

	/* ANT/MPSL must own clock startup before USB asks for the HFXO.
	 * If usb_enable() runs first on this target, USB can hang or fail to
	 * enumerate because the clock path is not fully initialized yet. */
	ret = antr_init();
	if (ret) {
		LOG_ERR("antr_init failed: %d", ret);
		(void)diag_flash_log_flush();
		return ret;
	}
	LOG_INF("antr_init ok");

#if defined(CONFIG_ANT_DONGLE_BLE_COEX_LOAD)
	/*
	 * AFTER antr_init(), FOR THE SAME REASON USB IS. The radio backend takes
	 * an explicit HFXO request at init that the multiprotocol spike showed is
	 * load-bearing rather than tidy - without it the 1 MHz timebase runs on
	 * the internal RC between timeslots, 0.42 % fast, which is about a
	 * millisecond of slot-placement error per ANT+ period and shows up in no
	 * counter anywhere. Starting the controller first would have it cycling
	 * the clock before anything held it.
	 *
	 * A failure here is logged and not fatal: this is a bench instrument, and
	 * a dongle that still does ANT+ with no advertiser is more useful than
	 * one that refuses to boot.
	 */
	(void)ble_coex_load_start();
#endif

#if defined(CONFIG_ANT_DONGLE_THREAD_COEX_LOAD)
	/*
	 * After antr_init() for the identical HFXO reason above, and non-fatal
	 * for the identical bench-instrument reason. The two loads are legal
	 * together - that build is the three-stack case - so this is a second
	 * `if`, not an `else`.
	 */
	(void)thread_coex_load_start();
#endif

	usb_ant_class_init();
	LOG_INF("usb_ant_class_init ok");

	/* The bridge owns ANT event forwarding: it defines antr_on_message(),
	 * which the radio backend calls. Nothing is registered.
	 */
	ret = ant_serial_bridge_init();
	if (ret) {
		LOG_ERR("ant_serial_bridge_init failed: %d", ret);
		(void)diag_flash_log_flush();
		return ret;
	}
	LOG_INF("ant_serial_bridge_init ok");

	/* Whichever of the three transports was compiled in. On a USB build
	 * this enumerates; on a UART build it opens the port. Anything
	 * stack-specific that has to happen first — the bcdDevice override, the
	 * USBD descriptor and configuration registration — belongs to the
	 * transport and is done in there.
	 */
	LOG_INF("enabling transport");
	ret = ant_transport_enable();
	if (ret) {
		LOG_ERR("ant_transport_enable failed: %d", ret);
		(void)diag_flash_log_flush();
		return ret;
	}

	LOG_INF("transport up");
	(void)diag_flash_log_flush();

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
	led_ok = (gpio_is_ready_dt(&led) == true) &&
		 (gpio_pin_configure_dt(&led, GPIO_OUTPUT_ACTIVE) == 0);
#endif

	/* Blink LED at 1 Hz as a heartbeat; nothing else to do here */
	while (1) {
#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
		if (led_ok) {
			gpio_pin_toggle_dt(&led);
		}
#endif
		k_sleep(K_MSEC(500));
	}

	return 0;
}
