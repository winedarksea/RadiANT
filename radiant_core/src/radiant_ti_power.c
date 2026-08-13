/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ti_power.c - the three SimpleLink Power symbols the RF driver needs,
 * with a calibrate function that is not NULL.
 *
 * Provenance: clean-room. Structure follows Zephyr's own
 * soc/ti/simplelink/cc13x2_cc26x2/power.c (Apache-2.0, Copyright (c) 2019
 * Linaro Limited), which this file replaces. Nothing here derives from
 * sdk-ant.
 *
 * ---------------------------------------------------------------------------
 * A NULL FUNCTION POINTER THAT ZEPHYR INSTALLS AND TI'S DRIVER DEREFERENCES
 * ---------------------------------------------------------------------------
 * Zephyr's power.c sets
 *
 *     .calibrateFxn = NULL
 *
 * for any build that does not define CONFIG_IEEE802154_CC13XX_CC26XX,
 * CONFIG_BLE_CC13XX_CC26XX or CONFIG_IEEE802154_CC13XX_CC26XX_SUB_GHZ, under a
 * comment reading "disable oscillator calibration functionality for now". And
 * PowerCC26X2.c:1357 is
 *
 *     readyToCal = (*(PowerCC26X2_config.calibrateFxn))(PowerCC26X2_INITIATE_CALIBRATE);
 *
 * with no NULL check, reached from switchXOSCHF() the first time anything asks
 * for the high-frequency crystal - which is the first RF_open(). MEASURED on a
 * LAUNCHXL-CC26X2R1 on 2026-08-13, by radiant_core/spike/ti_phy:
 *
 *     ***** USAGE FAULT *****   Illegal use of the EPSR
 *     Faulting instruction address (r15/pc): 0x00000000
 *     r14/lr: 0x0000237d  ->  switchXOSCHF, PowerCC26X2.c:1357
 *
 * So this is not "an out-of-tree radio user silently loses oscillator
 * calibration". It is "an out-of-tree radio user branches to address zero on
 * its first transmission". Zephyr's SoC layer here is written for its own three
 * in-tree radio drivers and does not degrade gracefully outside them.
 *
 * ---------------------------------------------------------------------------
 * WHY REPLACE THE FILE RATHER THAN FIX IT
 * ---------------------------------------------------------------------------
 *   - Defining one of those three Kconfig symbols turns on a radio driver that
 *     claims the RF core. This backend must own it.
 *   - Patching zephyr/ or modules/hal/ti is invisible to this repo and is undone
 *     by the next west update or scripts/fetch_hal_ti.ps1.
 *   - Zephyr compiles power.c only under CONFIG_PM, CONFIG_PM_DEVICE or
 *     CONFIG_POWEROFF. None of those is set on a cc26xx RadiANT build, so the
 *     file is simply not there and this one is not a duplicate. CMakeLists.txt
 *     asserts that rather than trusting it, because a duplicate symbol here
 *     would be a link error and a MISSING one would be the fault above.
 * ---------------------------------------------------------------------------
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include <driverlib/pwr_ctrl.h>
#include <driverlib/sys_ctrl.h>

#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26X2.h>

/*
 * The radio variant, which is what Zephyr installs for its own radio drivers.
 *
 * calibrateRCOSC_LF and _HF are true because ANT's entire timing budget is
 * derived from a low-frequency clock and this port exists to hit ANT's timing.
 * Note that these flags do NOT decide whether calibrateFxn is called - the
 * driver calls it unconditionally and passes the request; the function itself
 * consults the flags. "Flags false, pointer NULL" is the combination that
 * faults, and it is the combination Zephyr ships.
 *
 * enablePolicy is false and policyFxn is NULL: Zephyr's idle path owns power
 * states, not TI's. Handing the policy to TI's driver would let it enter
 * STANDBY between windows, which powers down the debug interface and adds a
 * wake-up cost that caps.min_arm_lead_us does not budget for.
 */
const PowerCC26X2_Config PowerCC26X2_config = {
	.policyInitFxn     = NULL,
	.policyFxn         = NULL,
	.calibrateFxn      = &PowerCC26XX_calibrate,
	.enablePolicy      = false,
	.calibrateRCOSC_LF = true,
	.calibrateRCOSC_HF = true,
};

/*
 * Both empty, and both must exist: the Power driver calls them around
 * Power_sleep() to keep its own RTOS's scheduler out of a critical section.
 * Zephyr's version is empty too, with the note that its scheduler cannot run
 * there anyway because interrupts are disabled - equally true here.
 */
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

/*
 * Unlatch IO pins after a wake from shutdown. Carried over from Zephyr's
 * power.c rather than dropped: without it a board that was ever put into
 * shutdown comes back with its pads latched and its UART mute - which on a
 * dongle whose only diagnostic channel IS that UART is indistinguishable from
 * an image that does not boot.
 */
static int radiant_ti_unlatch_pins(void)
{
	if (SysCtrlResetSourceGet() == RSTSRC_WAKEUP_FROM_SHUTDOWN) {
		PowerCtrlPadSleepDisable();
	}

	return 0;
}

SYS_INIT(radiant_ti_power_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
SYS_INIT(radiant_ti_unlatch_pins, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);
