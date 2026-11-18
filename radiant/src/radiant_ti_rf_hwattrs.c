/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ti_rf_hwattrs.c - radiant's own RFCC26XX_hwAttrs, the RF driver's
 * hardware-attributes object, for the options that need a say in it.
 *
 * Provenance: clean-room. The field values are copied from the definition
 * this replaces, hal_ti's simplelink/kernel/zephyr/dpl/config.c (compiled
 * when CONFIG_CC13X2_CC26X2_HAS_CUSTOM_RF_HWATTRS is not set); the callback's
 * contract (events, arguments, contexts) is from TI's public RF driver
 * headers and from reading RFCC26X2_multiMode.c in the hal_ti revision
 * scripts/fetch_hal_ti.ps1 pins. Nothing here derives from sdk-ant or any
 * ANT+ document.
 *
 * WHY THIS FILE EXISTS. TI's RF driver reads one global, RFCC26XX_hwAttrs,
 * for its interrupt priorities, for whether the crystal stays on between
 * radio operations (xoscHfAlwaysNeeded), and for a "global callback" it
 * invokes on RF core power-up, power-down and command dispatch. Zephyr's
 * definition sets the first three and no callback. Defining our own needs
 * CONFIG_CC13X2_CC26X2_HAS_CUSTOM_RF_HWATTRS, which turns Zephyr's off; that
 * is what CONFIG_RADIANT_CC26XX_RF_HWATTRS selects. Each option that needs a
 * field changed selects that symbol in turn:
 *
 *   - CONFIG_RADIANT_POWER_DIAG: adds the global callback, which counts RF
 *     core power-ups and on-time for radiant_power_diag.
 *   - CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE: xoscHfAlwaysNeeded = false (see
 *     XOSC_HF RELEASE below).
 *
 * With none of them on, this file is not compiled and Zephyr's definition is
 * the one linked: a default image is unchanged.
 *
 * REFUSED on a board that ships its own RFCC26XX_hwAttrs (Zephyr's
 * board_antenna.c for the SKY13317 CC1352 LaunchPads and the Beagle boards:
 * a duplicate definition, so say so rather than fail at link), and, for the
 * power-diag callback, where the RF core is not ours alone or the board may
 * need a callback of its own: the cc2652p_dongle (its antenna switch is not
 * settled, see its defconfig) and every 802.15.4 coexistence build (Zephyr's
 * 802.15.4 or BLE driver, or radiant's coex fork, is a second RF client and
 * the counts would mix two clients' power cycles). The last two are keyed on
 * each option; CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE takes both, for its own
 * reasons (end of the next section).
 *
 * XOSC_HF RELEASE (CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE). How hal_ti's
 * RFCC26X2_multiMode.c honours xoscHfAlwaysNeeded:
 *
 *   true (hal_ti's value): RF_init(), at the first RF_open(), calls
 *   Power_setDependency(PowerCC26XX_XOSC_HF), and only RF_close() of the last
 *   client releases it. The Power driver starts the crystal and switches
 *   SCLK_HF to it from the OSC interrupt; it then runs for the life of the
 *   radio, whether or not the RF core is powered.
 *
 *   false: the RF core's wake-up (RF_FsmEventWakeup in RF_fsmPowerUpState)
 *   sets the dependency, and RF_fsmPowerDownState releases it once the core
 *   is off; the release switches SCLK_HF back to RCOSC_HF and stops the
 *   crystal (OSCHF_SwitchToRcOscTurnOffXosc). RF_fsmSetupState submits the
 *   setup command at once but, while SCLK_HF is not yet XOSC_HF, holds back
 *   CMD_SYNC_START_RAT and the chained CMD_FS: RF_fsmXOSCState polls every
 *   RF_XOSC_HF_SWITCH_CHECK_PERIOD_US (50 us, which ClockP rounds to 2 ticks
 *   of the 32768 Hz system clock, so 61..92 us between polls) and submits the
 *   rest of the chain once the switch has happened.
 *
 * Meanwhile the CPU and every peripheral on SCLK_HF run from RCOSC_HF. The
 * Power driver calls calibrateFxn (radiant_ti_power.c: PowerCC26XX_calibrate
 * with calibrateRCOSC_HF = true) at every switch to the crystal, so RCOSC_HF
 * is re-trimmed against it once per RF power-up - once per 246 ms slot on
 * hr_transmitter. The calibration holds its own XOSC_HF dependency for a few
 * hundred microseconds, well inside the ~4.3 ms the core stays on per slot,
 * so it does not keep the crystal on longer. TI's CC26x2 datasheets give a
 * calibrated RCOSC_HF of about +/-0.25 %, far inside a UART's tolerance: a
 * console keeps working. (An ANT host link is still refused, as policy - see
 * the end of this section.) Unaffected: the AON RTC and with
 * it Zephyr's system timer, ClockP and the RF driver's wake timer, all on
 * the 32.768 kHz crystal (SCLK_LF = XOSC_LF in the CCFG); and the radio timer,
 * which runs only while the core is on, after the RAT sync. RAT/RTC sync is
 * not new either: gate GC1 showed the core already powering down every slot
 * with the crystal held, so CMD_SYNC_STOP_RAT/START_RAT already ran per slot.
 *
 * Lead. The driver wakes the core nPowerUpDuration + nPowerUpDurationMargin
 * (314 us) + 25 us per active RAT channel before a command's start. With the
 * crystal held, gate GC1 read nPowerUpDuration 424..480 us (median 448) and
 * no late first command. Released, a power-up also waits for the crystal.
 * The wait overlaps the core's boot and the setup command, so it adds at
 * most the crystal start-up (about 200 us typical in the CC26x2 datasheets)
 * plus one poll period (<= 92 us): expect nPowerUpDuration of roughly
 * 450..750 us. The driver times every power-up on the RTC, wait included,
 * adopts a longer one at once and averages in a shorter one, so the next
 * wake already budgets for it. A late start needs one power-up to exceed the
 * running estimate by more than the 314 us margin, against poll-grid and
 * start-up jitter well under 100 us. The first power-up after RF_open() is
 * not measured and the estimate starts at the driver's 2500 us, so the first
 * transmit after boot wakes early, not late. The backend's own budgets do
 * not move: a cold arm is only ever posted far ahead (the driver keeps the
 * core on when the next command is nearer than nPowerUpDuration + 314 +
 * 1000 us, a threshold this option lengthens), and in-grant arms
 * (IN_GRANT_LEAD_US) find the core, and so the crystal, already on. cold and
 * txl on the power-diag line are what would show this to be wrong.
 *
 * Refused where power_diag is, for its own reasons: on 802.15.4/BLE
 * coexistence builds the second client's timing (and CC26XX_COEX_PHY_SWITCH_
 * LEAD_US) was budgeted with the crystal always on and has never run against
 * a peer; on the cc2652p_dongle, a dongle whose antenna switch is not
 * settled (see its defconfig). Also refused, as policy, in any image with an
 * ANT host link (an ANT_DONGLE_TRANSPORT_* choice: apps/dongle,
 * apps/dongle_ti, apps/dongle_thread): a dongle's RF core is rarely off long
 * enough to gain anything, and a host link on RCOSC_HF has never been tested.
 *
 * WHAT THE COUNTS MEAN (CONFIG_RADIANT_POWER_DIAG).
 *
 *   on_us runs from the RF_GlobalEventRadioSetup that starts a power-up (the
 *   driver submits the setup command, so the RF core is booted and clocked)
 *   to the RF_GlobalEventRadioPowerDown that ends it (the RF core clock is
 *   already off). The wake before setup is not in it: powerups x pu_dur_us
 *   bounds that from above. Timed on the AON RTC, which runs whether or not
 *   the RF core does - never on the radio timer, which stops with the core.
 *
 *   powerups counts setup events seen while the core was off. The driver
 *   also raises RF_GlobalEventRadioSetup for a PA change and for a setup
 *   command a client posts while the core is on; those are reconfigurations,
 *   not power-ups, and are not counted.
 *
 *   pu_dur_us is the driver's own nPowerUpDuration for the current client:
 *   what it measured the last power-up to take (low-pass filtered, clamped to
 *   300..2500 us), and what it subtracts from a command's start time to
 *   decide when to wake. Read at every dispatch and at each power-down, so
 *   it is the value as of the driver's last measurement. That lags by one
 *   power-up at the first dispatch after a power-up: the RF hardware
 *   interrupt dispatches the pending command (CmdStart) before the driver's
 *   software interrupt, at the same priority, measures that power-up. The
 *   power-down read sees it.
 *
 *   cold_late and tx_late compare the radio timer, read when the driver hands
 *   a command to the RF core (RF_GlobalEventCmdStart, just before the
 *   doorbell), against that command's absolute start time. At or past it, the
 *   command starts late: the backend posts every operation with pastTrig = 1,
 *   so the RF core starts it at once rather than failing it, and nothing else
 *   reports it. cold_late is the first command after a power-up (the driver
 *   woke too late for it); tx_late is any CMD_PROP_TX/TX_ADV, cold or warm,
 *   and tx_late_max_us the worst. Both are lower bounds: the RF core takes a
 *   few more microseconds after the doorbell.
 *
 *   fs_errors counts RF_ERROR_CMDFS_SYNTH_PROG, the synthesiser-programming
 *   failure the driver reports through RF_Params::pErrCb when the CMD_FS it
 *   chained to a power-up setup fails (the driver's default callback ignores
 *   it). The backend installs radiant_ti_rf_err_cb() as its pErrCb.
 *
 * CONTEXTS. Every site that invokes the global callback runs in interrupt
 * context: the RF CPE0 hardware interrupt, the driver's software interrupt
 * (on Zephyr a SwiP is itself the lowest-priority hardware interrupt) and a
 * ClockP expiry (a k_timer, also an interrupt). RF_postCmd() never calls it
 * directly. Every update still takes irq_lock, because those interrupts and
 * the thread that reads the counters can preempt one another; each holds it
 * for a few dozen instructions. The CmdStart path, which runs just before
 * every doorbell, reads one register and touches no clock but the radio
 * timer. radiant_ti_rf_err_cb() is called from the software interrupt.
 */

#include <stdbool.h>
#include <stdint.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <ti/drivers/rf/RF.h>

#include <driverlib/interrupt.h>

#if defined(CONFIG_RADIANT_POWER_DIAG)
#include <driverlib/aon_rtc.h>
#include <driverlib/rf_mailbox.h>
#include <driverlib/rf_prop_cmd.h>
#include <inc/hw_memmap.h>
#include <inc/hw_rfc_rat.h>
#include <inc/hw_types.h>

#include <radiant/radiant_power_diag.h>
#endif

#include "radiant_ti_rf_hwattrs.h"

#if !defined(CONFIG_CC13X2_CC26X2_HAS_CUSTOM_RF_HWATTRS)
#error "radiant_ti_rf_hwattrs.c defines RFCC26XX_hwAttrs, so " \
	"CONFIG_CC13X2_CC26X2_HAS_CUSTOM_RF_HWATTRS must be set (Kconfig: " \
	"RADIANT_CC26XX_RF_HWATTRS selects it); without it hal_ti defines a second one"
#endif

/* Zephyr compiles a board_antenna.c defining RFCC26XX_hwAttrs (with an
 * antenna-switch callback) on exactly the boards that define this symbol:
 * boards/ti/common for the SKY13317 CC1352 LaunchPads, and both Beagle
 * boards. The board's own defconfig sets HAS_CUSTOM_RF_HWATTRS, so the check
 * above does not catch it. */
#if defined(CONFIG_BOARD_ANTENNA_INIT_PRIO) || defined(CONFIG_BOARD_CC1352_LP_SKY13317) || \
	defined(CONFIG_BOARD_BEAGLECONNECT_FREEDOM) || defined(CONFIG_BOARD_BEAGLEPLAY)
#error "this board defines its own RFCC26XX_hwAttrs (Zephyr's board_antenna.c, with " \
	"an antenna-switch callback); radiant_ti_rf_hwattrs.c would be a second one. " \
	"Turn off the option that selects CONFIG_RADIANT_CC26XX_RF_HWATTRS " \
	"(CONFIG_RADIANT_POWER_DIAG, CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE) for this board."
#endif

#if defined(CONFIG_RADIANT_POWER_DIAG) && defined(CONFIG_BOARD_CC2652P_DONGLE)
#error "radiant's power-diag RF callback is refused on the cc2652p_dongle: its " \
	"antenna switch may need a callback of its own (see the board defconfig). " \
	"Turn off CONFIG_RADIANT_POWER_DIAG for this board."
#endif

#if defined(CONFIG_RADIANT_POWER_DIAG) && \
	(defined(CONFIG_RADIANT_BACKEND_CC26XX_COEX) || \
	 defined(CONFIG_RADIANT_COEX154_CC26XX_FORK) || \
	 defined(CONFIG_IEEE802154_CC13XX_CC26XX) || \
	 defined(CONFIG_IEEE802154_CC13XX_CC26XX_SUB_GHZ) || defined(CONFIG_BLE_CC13XX_CC26XX))
#error "radiant's power-diag RF callback is refused on a build with a second RF " \
	"client (802.15.4/BLE coexistence): the RF core's power cycles are not " \
	"radiant's alone. Turn off CONFIG_RADIANT_POWER_DIAG for this build."
#endif

#if defined(CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE) && defined(CONFIG_BOARD_CC2652P_DONGLE)
#error "CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE is refused on the cc2652p_dongle: a " \
	"dongle, and its antenna switch may need a callback of its own (see the board " \
	"defconfig). Turn it off for this board."
#endif

#if defined(CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE) && \
	(defined(CONFIG_RADIANT_BACKEND_CC26XX_COEX) || \
	 defined(CONFIG_RADIANT_COEX154_CC26XX_FORK) || \
	 defined(CONFIG_IEEE802154_CC13XX_CC26XX) || \
	 defined(CONFIG_IEEE802154_CC13XX_CC26XX_SUB_GHZ) || defined(CONFIG_BLE_CC13XX_CC26XX))
#error "CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE is refused on a build with a second RF " \
	"client (802.15.4/BLE coexistence): that client's timing was budgeted with the " \
	"crystal always on. Turn it off for this build."
#endif

#if defined(CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE) && \
	(defined(CONFIG_ANT_DONGLE_TRANSPORT_UART) || \
	 defined(CONFIG_ANT_DONGLE_TRANSPORT_USB_LEGACY) || \
	 defined(CONFIG_ANT_DONGLE_TRANSPORT_USB_NEXT))
#error "CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE is refused in an image with an ANT " \
	"host link (CONFIG_ANT_DONGLE_TRANSPORT_*): a dongle's RF core is rarely off " \
	"long enough to gain anything, and a host link on RCOSC_HF is untested. " \
	"The option is for battery nodes (radiant/samples/hr_transmitter ti_lowpower.conf)."
#endif

/* xoscHfAlwaysNeeded: hal_ti's true unless the release option is on. */
#if defined(CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE)
#define TI_XOSC_HF_ALWAYS_NEEDED false
#else
#define TI_XOSC_HF_ALWAYS_NEEDED true
#endif

#if defined(CONFIG_RADIANT_POWER_DIAG)

/* RAT ticks per microsecond (the RF core's radio timer runs at 4 MHz). */
#define TI_RAT_TICKS_PER_US 4u

static struct {
	uint64_t on_rtc;    /* completed on-intervals, AON RTC units (2^-32 s) */
	uint64_t since_rtc; /* start of the running one, valid while on */
	uint32_t powerups;
	uint32_t pu_dur_us;
	uint32_t fs_errors;
	uint32_t cold_late;
	uint32_t tx_late;
	uint32_t tx_late_max_us;
	bool     on;
	bool     first_cmd; /* the next dispatch is the first since a power-up */
} trf = {
	.pu_dur_us = RADIANT_POWER_DIAG_NOT_MEASURED,
};

/* AON RTC units (32.32 fixed-point seconds) to microseconds. The low 16
 * fraction bits are below the RTC's 1/32768 s step, so dropping them first
 * loses nothing and keeps the product in 64 bits for about 8.9 years of
 * accumulated on-time (2^64 / 10^6 / 2^16 s). */
static uint64_t rtc_to_us(uint64_t rtc)
{
	return ((rtc >> 16) * 1000000u) >> 16;
}

static void note_pu_dur(RF_Handle h)
{
	if (h != NULL) {
		trf.pu_dur_us = h->clientConfig.nPowerUpDuration;
	}
}

static void note_cmd_start(RF_Handle h, const RF_Cmd *cmd)
{
	const RF_Op *op = (cmd != NULL) ? cmd->pOp : NULL;
	bool first = trf.first_cmd;
	int32_t late;

	trf.first_cmd = false;
	/* As of the driver's last measurement: at the first dispatch after a
	 * power-up, that power-up is not measured yet (see the header). */
	note_pu_dur(h);
	/* Only an absolute start can be late, and the radio timer only runs
	 * while the core is on (it always is at a dispatch). */
	if (op == NULL || !trf.on || op->startTrigger.triggerType != TRIG_ABSTIME) {
		return;
	}

	late = (int32_t)(HWREG(RFC_RAT_BASE + RFC_RAT_O_RATCNT) - op->startTime);
	if (late < 0) {
		return;
	}
	if (first) {
		trf.cold_late++;
	}
	if (op->commandNo == CMD_PROP_TX || op->commandNo == CMD_PROP_TX_ADV) {
		uint32_t us = (uint32_t)late / TI_RAT_TICKS_PER_US;

		trf.tx_late++;
		if (us > trf.tx_late_max_us) {
			trf.tx_late_max_us = us;
		}
	}
}

static void ti_rf_global_cb(RF_Handle h, RF_GlobalEvent event, void *arg)
{
	unsigned int key = irq_lock();

	if ((event & RF_GlobalEventCmdStart) != 0) {
		note_cmd_start(h, (const RF_Cmd *)arg);
	}
	if ((event & RF_GlobalEventRadioSetup) != 0 && !trf.on) {
		trf.on = true;
		trf.since_rtc = AONRTCCurrent64BitValueGet();
		trf.powerups++;
		trf.first_cmd = true;
	}
	if ((event & RF_GlobalEventRadioPowerDown) != 0) {
		if (trf.on) {
			uint64_t now = AONRTCCurrent64BitValueGet();

			if (now > trf.since_rtc) {
				trf.on_rtc += now - trf.since_rtc;
			}
			trf.on = false;
		}
		note_pu_dur(h);
	}

	irq_unlock(key);
}

void radiant_ti_rf_err_cb(RF_Handle h, RF_CmdHandle ch, RF_EventMask e)
{
	ARG_UNUSED(h);

	/* The driver passes the error code where a command handle would be. */
	if ((e & RF_EventError) != 0 && ch == RF_ERROR_CMDFS_SYNTH_PROG) {
		unsigned int key = irq_lock();

		trf.fs_errors++;
		irq_unlock(key);
	}
}

void radiant_ti_rf_diag_read(struct radiant_ti_rf_diag *out)
{
	unsigned int key = irq_lock();
	uint64_t on = trf.on_rtc;

	if (trf.on) {
		uint64_t now = AONRTCCurrent64BitValueGet();

		if (now > trf.since_rtc) {
			on += now - trf.since_rtc;
		}
	}
	out->on_us = (uint32_t)rtc_to_us(on);
	out->powerups = trf.powerups;
	out->pu_dur_us = trf.pu_dur_us;
	out->fs_errors = trf.fs_errors;
	out->cold_late = trf.cold_late;
	out->tx_late = trf.tx_late;
	out->tx_late_max_us = trf.tx_late_max_us;
	irq_unlock(key);
}

#define TI_RF_GLOBAL_CB   ti_rf_global_cb
#define TI_RF_GLOBAL_MASK (RF_GlobalEventRadioSetup | RF_GlobalEventRadioPowerDown | \
			   RF_GlobalEventCmdStart)
#else
#define TI_RF_GLOBAL_CB   NULL
#define TI_RF_GLOBAL_MASK 0u
#endif /* CONFIG_RADIANT_POWER_DIAG */

/*
 * hal_ti's values, field for field, except the callback and, with
 * CONFIG_RADIANT_CC26XX_XOSC_HF_RELEASE, xoscHfAlwaysNeeded. Left true, the
 * RF driver holds XOSC_HF from the first RF_open() to the last RF_close();
 * false, it holds it only while the RF core is powered (see XOSC_HF RELEASE
 * at the top).
 */
const RFCC26XX_HWAttrsV2 RFCC26XX_hwAttrs = {
	.hwiPriority        = INT_PRI_LEVEL7, /* lowest: as hal_ti */
	.swiPriority        = 0,              /* lowest: as hal_ti */
	.xoscHfAlwaysNeeded = TI_XOSC_HF_ALWAYS_NEEDED,
	.globalCallback     = TI_RF_GLOBAL_CB,
	.globalEventMask    = TI_RF_GLOBAL_MASK,
};
