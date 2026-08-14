/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ti_power.c - the three SimpleLink Power symbols the RF driver
 * needs, with a calibrate function that is not NULL.
 *
 * Clean-room. Structure follows Zephyr's own
 * soc/ti/simplelink/cc13x2_cc26x2/power.c (Apache-2.0, Copyright (c) 2019
 * Linaro Limited), which this file replaces.
 *
 * Zephyr's power.c sets .calibrateFxn = NULL unless one of
 * CONFIG_IEEE802154_CC13XX_CC26XX / CONFIG_BLE_CC13XX_CC26XX /
 * CONFIG_IEEE802154_CC13XX_CC26XX_SUB_GHZ is defined, and
 * PowerCC26X2.c:1357 calls that pointer with no NULL check from
 * switchXOSCHF(), reached on the first RF_open(). Confirmed on hardware
 * (LAUNCHXL-CC26X2R1): a usage fault branching to address zero on first
 * transmission, not a silent loss of calibration.
 *
 * Replaced rather than patched because: defining one of those three Kconfig
 * symbols would turn on a radio driver that claims the RF core, which this
 * backend must own instead; and patching zephyr/ or modules/hal/ti would be
 * invisible to this repo and undone by the next west update. Zephyr only
 * compiles power.c under CONFIG_PM/CONFIG_PM_DEVICE/CONFIG_POWEROFF, none set
 * here, so this isn't a duplicate symbol - CMakeLists.txt asserts that rather
 * than trusting it.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include <driverlib/pwr_ctrl.h>
#include <driverlib/sys_ctrl.h>

#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26X2.h>

/*
 * The radio variant. calibrateRCOSC_LF/_HF are true because ANT's timing
 * budget is derived from a low-frequency clock; the driver calls
 * calibrateFxn unconditionally regardless of these flags, and "flags false,
 * pointer NULL" is the combination Zephyr ships that faults.
 *
 * enablePolicy is false and policyFxn is NULL: Zephyr's idle path owns power
 * states, not TI's. Handing TI's driver the policy would let it enter
 * STANDBY between windows, powering down the debug interface and adding a
 * wake-up cost caps.min_arm_lead_us doesn't budget for.
 */
const PowerCC26X2_Config PowerCC26X2_config = {
	.policyInitFxn     = NULL,
	.policyFxn         = NULL,
	.calibrateFxn      = &PowerCC26XX_calibrate,
	.enablePolicy      = false,
	.calibrateRCOSC_LF = true,
	.calibrateRCOSC_HF = true,
};

/* Both empty, and both must exist: the Power driver calls them around
 * Power_sleep(). Empty is correct - the scheduler can't run there anyway
 * since interrupts are disabled. */
void PowerCC26XX_schedulerDisable(void)
{
}

void PowerCC26XX_schedulerRestore(void)
{
}

static int radiant_ti_power_init(void)
{
	unsigned int key = irq_lock();

	Power_init();
	irq_unlock(key);

	return 0;
}

/* Unlatch IO pins after a wake from shutdown. Carried over from Zephyr's
 * power.c: without it, a board that was in shutdown comes back with pads
 * latched and UART mute - indistinguishable from a dead image. */
static int radiant_ti_unlatch_pins(void)
{
	if (SysCtrlResetSourceGet() == RSTSRC_WAKEUP_FROM_SHUTDOWN) {
		PowerCtrlPadSleepDisable();
	}

	return 0;
}

SYS_INIT(radiant_ti_power_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
SYS_INIT(radiant_ti_unlatch_pins, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);
