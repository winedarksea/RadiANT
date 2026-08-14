/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Clean-room: this file's structure follows Zephyr's own
 * soc/ti/simplelink/cc13x2_cc26x2/power.c (Apache-2.0, Copyright (c) 2019
 * Linaro Limited), which it replaces; nothing derives from sdk-ant.
 *
 * Why this file exists: a NULL function pointer Zephyr installs and TI's
 * driver dereferences without checking. Measured on the spike's first run,
 * 2026-08-13:
 *     ***** USAGE FAULT *****
 *     Illegal use of the EPSR
 *     Faulting instruction address (r15/pc): 0x00000000
 *     r14/lr: 0x0000237d   -> switchXOSCHF, PowerCC26X2.c:1357
 * That line, `(*(PowerCC26X2_config.calibrateFxn))(...)`, has no NULL check
 * and runs the first time anything asks for XOSC_HF (this spike's first
 * RF_open()). Zephyr's power.c sets `.calibrateFxn = NULL` for any build not
 * defining CONFIG_IEEE802154_CC13XX_CC26XX, CONFIG_BLE_CC13XX_CC26XX or
 * ..._SUB_GHZ, under a comment reading "disable oscillator calibration
 * functionality for now".
 *
 * This spike's own README originally predicted the defect but got its
 * severity wrong ("costs accuracy, not function, so nothing fails") - left
 * on record rather than quietly corrected. It's not accuracy: outside
 * Zephyr's three in-tree radio drivers, the very first radio operation
 * branches to address 0.
 *
 * Why a replacement rather than a patch: defining one of those three Kconfig
 * symbols turns on a radio driver that claims the RF core, which a PHY
 * spike may not share - rejected. Patching modules/hal/ti or the Zephyr
 * tree is invisible to the repo and undone by the next
 * scripts/fetch_hal_ti.ps1/west fetch - rejected. Instead: don't compile
 * Zephyr's power.c (it's gated on CONFIG_PM/PM_DEVICE/POWEROFF, and the
 * spike had turned CONFIG_POWEROFF on only to drag it in) and supply the
 * three symbols the RF driver needs here, with calibrateFxn pointing
 * somewhere real.
 *
 * P2's problem too: radiant_radio_cc26xx.c needs the same three symbols by
 * the same route - not spike scaffolding.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>

#include <driverlib/pwr_ctrl.h>
#include <driverlib/sys_ctrl.h>

#include <ti/drivers/Power.h>
#include <ti/drivers/power/PowerCC26X2.h>

/* The radio variant of the configuration - what Zephyr installs for its own
 * three radio drivers and what this spike is. calibrateRCOSC_LF/_HF are true
 * for the same reason the in-tree drivers set them: uncalibrated RCOSCs
 * leave the low-frequency clock off by enough to matter to ANT's 32.768 kHz
 * derived channel period. Note true/false here does NOT decide whether the
 * pointer above is called - PowerCC26X2.c calls it unconditionally; the
 * calibrate function itself consults these flags. Flags false with the
 * pointer NULL is the combination that faults. */
const PowerCC26X2_Config PowerCC26X2_config = {
	.policyInitFxn     = NULL,
	.policyFxn         = NULL,
	.calibrateFxn      = &PowerCC26XX_calibrate,
	.enablePolicy      = false,
	.calibrateRCOSC_LF = true,
	.calibrateRCOSC_HF = true,
};

/* Both empty, and both must exist: the SimpleLink Power driver calls them
 * around Power_sleep() to keep its own RTOS's scheduler out of a critical
 * section. Zephyr's power.c leaves them empty since its scheduler can't run
 * there anyway (interrupts disabled) - equally true here. */
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
