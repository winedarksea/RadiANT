/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ti_rf_hwattrs.h - what src/radiant_ti_rf_hwattrs.c offers the rest
 * of the stack. Private to radiant/src; built only with
 * CONFIG_RADIANT_CC26XX_RF_HWATTRS, and the power-diag parts only with
 * CONFIG_RADIANT_POWER_DIAG as well.
 *
 * Provenance: clean-room. See the .c file.
 */

#ifndef RADIANT_TI_RF_HWATTRS_H_
#define RADIANT_TI_RF_HWATTRS_H_

#include <stdint.h>

#include <ti/drivers/rf/RF.h>

#ifdef __cplusplus
extern "C" {
#endif

#if defined(CONFIG_RADIANT_POWER_DIAG)

/*
 * The RF driver's view of the RF core, as counted by the global callback in
 * radiant_ti_rf_hwattrs.c. Every count is cumulative since boot; on_us is a
 * free-running u32 (take differences modulo 2^32).
 */
struct radiant_ti_rf_diag {
	uint32_t on_us;          /* RF core powered, setup to power-down, us */
	uint32_t powerups;       /* RF core power-ups */
	uint32_t pu_dur_us;      /* driver's nPowerUpDuration; NOT_MEASURED until read */
	uint32_t fs_errors;      /* RF_ERROR_CMDFS_SYNTH_PROG through pErrCb */
	uint32_t cold_late;      /* first command after a power-up dispatched late */
	uint32_t tx_late;        /* transmit commands dispatched late */
	uint32_t tx_late_max_us; /* the latest of those, us past its start time */
};

/* A consistent copy of the counters, the running on-interval included. Any
 * thread context. */
void radiant_ti_rf_diag_read(struct radiant_ti_rf_diag *out);

/* RF_Params::pErrCb for the backend's client: counts synthesiser errors and
 * does nothing else, exactly as the driver's default callback does nothing. */
void radiant_ti_rf_err_cb(RF_Handle h, RF_CmdHandle ch, RF_EventMask e);

#endif /* CONFIG_RADIANT_POWER_DIAG */

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_TI_RF_HWATTRS_H_ */
