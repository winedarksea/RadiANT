/*
 * ANT+ USB Dongle — main entry point.
 *
 * All protocol work happens in the ANT bridge thread (ant_serial_bridge.c)
 * and the ANT event callback (ant_evt_handler).  Main just initialises the
 * subsystems in order, then sleeps forever.
 */

#include <zephyr/kernel.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/logging/log.h>

#if CONFIG_ANT_DONGLE_BCD_DEVICE != 0
#include <zephyr/sys/byteorder.h>
#include <usb_descriptor.h>
#endif

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

/* Forward declarations — defined in ant_serial_bridge.c */
extern int ant_serial_bridge_init(void);
/* Forward declaration — defined in usb_ant_class.c */
extern void usb_ant_class_init(void);

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

#if CONFIG_ANT_DONGLE_BCD_DEVICE != 0
	/* Bring-up knob. Zephyr pins bcdDevice to USB_BCD_DRN, derived from the
	 * kernel version, so it is identical across rebuilds — and Windows keys
	 * its cached MS OS descriptor verdict on VID+PID+bcdDevice. Bumping this
	 * between iterations gives Windows a device it has no cached verdict for.
	 * Must run before usb_enable(), which is what publishes the descriptor.
	 */
	{
		struct usb_device_descriptor *dev_desc =
			(struct usb_device_descriptor *)usb_get_device_descriptor();

		dev_desc->bcdDevice = sys_cpu_to_le16(CONFIG_ANT_DONGLE_BCD_DEVICE);
		LOG_INF("bcdDevice overridden to 0x%04X",
			(unsigned int)CONFIG_ANT_DONGLE_BCD_DEVICE);
	}
#endif

	LOG_INF("calling usb_enable");
	ret = usb_enable(NULL);
	if (ret) {
		LOG_ERR("usb_enable failed: %d", ret);
		(void)diag_flash_log_flush();
		return ret;
	}

	LOG_INF("USB enabled — VID 0x0FCF PID 0x1009");
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
