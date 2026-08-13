/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: clean-room. This file's structure follows Zephyr's own
 * soc/ti/simplelink/cc13x2_cc26x2/power.c (Apache-2.0, Copyright (c) 2019
 * Linaro Limited), which it replaces; nothing here derives from sdk-ant.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS FILE EXISTS: a NULL function pointer that Zephyr installs and TI's
 * driver dereferences without checking. Measured, not deduced - the spike
 * faulted here on its first run on 2026-08-13:
 *
 *     ***** USAGE FAULT *****
 *     Illegal use of the EPSR
 *     Faulting instruction address (r15/pc): 0x00000000
 *     r14/lr: 0x0000237d   -> switchXOSCHF, PowerCC26X2.c:1357
 *
 * PowerCC26X2.c:1357 is
 *
 *     readyToCal = (*(PowerCC26X2_config.calibrateFxn))(PowerCC26X2_INITIATE_CALIBRATE);
 *
 * with no NULL check, reached the first time anything asks for XOSC_HF - which
 * for this spike is the first RF_open(). And Zephyr's power.c sets
 *
 *     .calibrateFxn = NULL
 *
 * for every build that does not define CONFIG_IEEE802154_CC13XX_CC26XX,
 * CONFIG_BLE_CC13XX_CC26XX or CONFIG_IEEE802154_CC13XX_CC26XX_SUB_GHZ, under a
 * comment reading "disable oscillator calibration functionality for now".
 *
 * THE README FOR THIS SPIKE PREDICTED THIS DEFECT AND GOT ITS SEVERITY WRONG,
 * which is worth leaving on the record rather than quietly correcting. It said
 * an out-of-tree radio user "silently gets oscillator calibration disabled...
 * costs accuracy, not function, so nothing fails". It is not accuracy. The
 * three in-tree radio drivers are the only users Zephyr's power.c was written
 * for, and outside them the very first radio operation branches to address 0.
 *
 * WHY A REPLACEMENT RATHER THAN A PATCH. The three ways out, and why this one:
 *
 *   - Define one of those three Kconfig symbols. Each of them turns on a radio
 *     driver that claims the RF core, which is precisely what a PHY spike may
 *     not share. Rejected.
 *   - Patch modules/hal/ti or the Zephyr tree. Both are pinned checkouts that
 *     scripts/fetch_hal_ti.ps1 and west restore from upstream revisions; a
 *     local edit there is invisible to the repo and is undone by the next
 *     fetch. Rejected.
 *   - Do not compile Zephyr's power.c, and supply its symbols here. That is a
 *     Kconfig choice this project already owns - the file is compiled only
 *     under CONFIG_PM, CONFIG_PM_DEVICE or CONFIG_POWEROFF, and the spike had
 *     turned CONFIG_POWEROFF on for the sole purpose of dragging it in. So the
 *     fix is to turn that back off and provide the three symbols the RF driver
 *     actually needs, with calibrateFxn pointing somewhere.
 *
 * THIS IS P2's PROBLEM TOO. radiant_radio_cc26xx.c will need the same three
 * symbols by the same route; it is not spike scaffolding.
 * ---------------------------------------------------------------------------
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include <driverlib/pwr_ctrl.h>
#include <driverlib/sys_ctrl.h>

#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26X2.h>

/*
 * The radio variant of the configuration, which is what Zephyr installs for
 * its own three radio drivers and what this spike is.
 *
 * calibrateRCOSC_LF and _HF are true for the same reason the in-tree drivers
 * set them: with the RCOSCs uncalibrated the low-frequency clock is off by
 * enough to matter to a protocol whose whole timing budget is a 32.768 kHz
 * derived channel period, and this port exists to hit ANT's timing. Note that
 * the true/false here does NOT decide whether the pointer above is called -
 * PowerCC26X2.c calls it unconditionally and passes the request, and the
 * calibrate function itself consults these flags. Setting the flags false
 * while leaving the pointer NULL is the combination that faults.
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
 * Both empty, and both must exist: the SimpleLink Power driver calls them
 * around Power_sleep() to keep its own RTOS's scheduler out of a critical
 * section. Zephyr's power.c leaves them empty with the note that its scheduler
 * cannot run there anyway because interrupts are disabled, which is equally
 * true here.
 */
void PowerCC26XX_schedulerDisable(void)
{
}

void PowerCC26XX_schedulerRestore(void)
{
}

static int ti_power_init(void)
{
	unsigned int key = irq_lock();

	Power_init();
	irq_unlock(key);

	return 0;
}

/*
 * Unlatch IO pins after a wake from shutdown. Carried over from Zephyr's
 * power.c rather than dropped: without it a board that was ever put into
 * shutdown comes back with its pads latched and its UART mute, which is a
 * failure that looks exactly like the one this spike is trying to diagnose.
 */
static int ti_unlatch_pins(void)
{
	if (SysCtrlResetSourceGet() == RSTSRC_WAKEUP_FROM_SHUTDOWN) {
		PowerCtrlPadSleepDisable();
	}

	return 0;
}

SYS_INIT(ti_power_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);
SYS_INIT(ti_unlatch_pins, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);
