/*
 * ANT+ USB Dongle — main entry point.
 *
 * All protocol work happens in the ANT bridge thread (ant_serial_bridge.c)
 * and the ANT event callback (ant_evt_handler).  Main just initialises the
 * subsystems in order, then sleeps forever.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "ant_transport.h"

#if DT_NODE_HAS_STATUS(DT_ALIAS(led0), okay)
#include <zephyr/drivers/gpio.h>
static const struct gpio_dt_spec led =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static bool led_ok;
#endif

#include "diag_flash_log.h"

#include "ant_interface.h"
/* nRF5340 cpuapp builds use the network-processor host header; every
 * single-core target (including the nRF52840 here) uses ant_init.h.
 */
#if defined(CONFIG_ANT_NP_HOST)
#include "ant_host_init.h"
#else
#include "ant_init.h"
#endif

LOG_MODULE_REGISTER(ant_dongle, LOG_LEVEL_INF);

/* Forward declaration — defined in ant_serial_bridge.c */
extern int ant_serial_bridge_init(void);

int main(void)
{
	int ret;

	/* Wait 1 second to ensure Mac OS detects the UF2 bootloader disconnect 
	 * before we bring up the Zephyr USB stack. */
	k_sleep(K_MSEC(1000));

	LOG_INF("ANT+ USB Dongle starting");

	/* ANT/MPSL must own clock startup before USB asks for the HFXO.
	 * If usb_enable() runs first on this target, USB can hang or fail to
	 * enumerate because the clock path is not fully initialized yet. */
	ret = ant_init();
	if (ret) {
		LOG_ERR("ant_init failed: %d", ret);
		(void)diag_flash_log_flush();
		return ret;
	}
	LOG_INF("ant_init ok");

	usb_ant_class_init();
	LOG_INF("usb_ant_class_init ok");

	/* The bridge owns ANT event forwarding and callback registration. */
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
