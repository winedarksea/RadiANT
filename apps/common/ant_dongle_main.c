/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The ANT-serial dongle boot sequence — see ant_dongle_main.h for why it lives
 * here rather than in either application's own src/main.c.
 *
 * All protocol work happens in the ANT bridge thread (ant_serial_bridge.c) and
 * in antr_on_message(), which the bridge implements and the radio backend
 * calls. This just initialises the subsystems in order, then sleeps forever.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/logging/log.h>

#include "ant_dongle_main.h"
#include "ant_transport.h"
#include "diag_flash_log.h"

/* The radio contract - whichever backend is compiled in behind it. Bringing
 * the stack up is the only thing the boot sequence needs from it.
 */
#include "ant_radio.h"

/* Measured on an nRF54L15 DK, alternating builds: XTAL heard 14 broadcasts,
 * SYNTH heard none, XTAL heard 14 again. Nothing announces the failure - the
 * clock starts, antr_init() returns 0, channels open and acknowledge every
 * command, and the radio just never receives. Refused at build time rather
 * than warned about, since the warning would be read once and the silence
 * debugged forever. Mechanism not established; nRF52840 is unaffected and
 * synth.conf remains a supported fallback there.
 *
 * apps/dongle_thread needs this MORE than apps/dongle does, being nRF54-only,
 * which is one of the reasons the assert moved here with the rest of the boot
 * sequence rather than staying behind in one application's main.c.
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

/* Registered here rather than in either application's main.c, and deliberately
 * still called `ant_dongle`: this is the name apps/dongle's boot log has always
 * carried, and nothing should have to re-learn a log prefix because a function
 * moved between files. apps/dongle_thread's boot line differs in its text
 * (CONFIG_ANT_DONGLE_MAIN_BANNER), not in its module.
 */
LOG_MODULE_REGISTER(ant_dongle, LOG_LEVEL_INF);

/* Forward declaration — defined in ant_serial_bridge.c */
extern int ant_serial_bridge_init(void);

/* The default post-radio hook: nothing. See the header for why it is weak. */
__weak void ant_dongle_post_radio_init(void)
{
}

/*
 * The UF2 bootloader ejects its mass-storage drive and soft-resets into this
 * image. macOS needs a moment to finish that detach; enumerating into the gap
 * leaves the dongle unusable until replugged. That race only exists right
 * after a flash - a cold plug has nothing to detach from, so RESETREAS
 * distinguishes the cases (software reset vs. power-on vs. pin reset) and
 * only the former waits. RESETREAS latches until cleared, so this clears it
 * after reading, or the wait would never be skipped again.
 *
 * On a board with no UF2 bootloader (every apps/dongle_thread P4 gate board)
 * CONFIG_ANT_DONGLE_UF2_SETTLE_MS may be 0 and this is a no-op; it stays
 * compiled so a USB-capable board built from either application behaves the
 * same way.
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

		/* Power-on reset leaves RESETREAS empty and is the only cause
		 * guaranteeing no mass-storage device preceded us. Every other
		 * cause (bootloader jump, or RESET pressed while the drive is
		 * still mounted) can race, so both wait.
		 */
		if (cause == 0) {
			return;
		}

		LOG_INF("reset cause 0x%08x: settling %d ms for the bootloader",
			cause, CONFIG_ANT_DONGLE_UF2_SETTLE_MS);
	}
#endif

	/* Wait unconditionally if the reset cause could not be established,
	 * rather than assume a cold boot and risk the race.
	 */
	k_sleep(K_MSEC(CONFIG_ANT_DONGLE_UF2_SETTLE_MS));
}

int ant_dongle_main_run(void)
{
	int ret;

	settle_after_uf2_bootloader();

	LOG_INF("%s starting", CONFIG_ANT_DONGLE_MAIN_BANNER);

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

	/* Weak; a no-op unless the application overrode it. See the header for
	 * why it is placed exactly here (the backend's HFXO request) and why
	 * failure inside it is the application's business, not this file's.
	 */
	ant_dongle_post_radio_init();

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
