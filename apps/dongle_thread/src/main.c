/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ANT+ USB Dongle + Thread — main entry point.
 *
 * Identical shape to apps/dongle/src/main.c, plus the two coexistence-load
 * hooks that application carries as dead code (its own Kconfig never
 * declares CONFIG_ANT_DONGLE_BLE_COEX_LOAD or _THREAD_COEX_LOAD, so the #if
 * blocks below are the only place either actually compiles in). All protocol
 * work happens in the ANT bridge thread (apps/common/ant_serial_bridge.c) and
 * in antr_on_message(), which the bridge implements and the radio backend
 * calls. Main just initialises the subsystems in order, then sleeps forever.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>

#include "ant_transport.h"

/* Measured on an nRF54L15 DK, alternating builds: XTAL heard 14 broadcasts,
 * SYNTH heard none, XTAL heard 14 again. Nothing announces the failure - the
 * clock starts, antr_init() returns 0, channels open and acknowledge every
 * command, and the radio just never receives. Refused at build time rather
 * than warned about, since the warning would be read once and the silence
 * debugged forever. Mechanism not established; nRF52840 is unaffected.
 */
BUILD_ASSERT(!(IS_ENABLED(CONFIG_CLOCK_CONTROL_NRF_K32SRC_SYNTH) &&
	       IS_ENABLED(CONFIG_SOC_SERIES_NRF54LX)),
	     "A synthesized low-frequency clock does not keep ANT's timing on "
	     "nRF54L. The build boots and hears nothing. This part needs a real "
	     "32.768 kHz crystal.");

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

LOG_MODULE_REGISTER(ant_dongle_thread, LOG_LEVEL_INF);

/* Forward declaration — defined in apps/common/ant_serial_bridge.c */
extern int ant_serial_bridge_init(void);

/*
 * The UF2 bootloader ejects its mass-storage drive and soft-resets into this
 * image. macOS needs a moment to finish that detach; enumerating into the gap
 * leaves the dongle unusable until replugged. That race only exists right
 * after a flash - a cold plug has nothing to detach from, so RESETREAS
 * distinguishes the cases (software reset vs. power-on vs. pin reset) and
 * only the former waits. RESETREAS latches until cleared, so this clears it
 * after reading, or the wait would never be skipped again.
 *
 * None of this application's own P4 gate boards boot from a UF2 bootloader,
 * so this is a no-op there; it stays so a USB-capable board built from this
 * application behaves the same way apps/dongle's does.
 */
static void settle_after_uf2_bootloader(void)
{
	if (CONFIG_ANT_DONGLE_UF2_SETTLE_MS == 0) {
		return;
	}

#if defined(CONFIG_HWINFO)
	uint32_t cause = 0;

	if (hwinfo_get_reset_cause(&cause) == 0) {
		(void)hwinfo_clear_reset_cause();

		if (cause == 0) {
			return;
		}

		LOG_INF("reset cause 0x%08x: settling %d ms for the bootloader",
			cause, CONFIG_ANT_DONGLE_UF2_SETTLE_MS);
	}
#endif

	k_sleep(K_MSEC(CONFIG_ANT_DONGLE_UF2_SETTLE_MS));
}

int main(void)
{
	int ret;

	settle_after_uf2_bootloader();

	LOG_INF("ANT+ USB Dongle + Thread starting");

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
	 * After antr_init(), for the same reason USB is: the radio backend
	 * holds an explicit HFXO request that's load-bearing - without it the
	 * 1 MHz timebase runs 0.42% fast on the internal RC between timeslots,
	 * invisible in any counter. Failure here is logged, not fatal - ANT+
	 * with no advertiser beats refusing to boot.
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
