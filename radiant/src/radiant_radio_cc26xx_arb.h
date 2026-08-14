/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_cc26xx_arb.h - the two-symbol interface between
 * radiant_radio_cc26xx.c (a HAL backend, includes radiant_radio_hal.h) and
 * radiant_radio_cc26xx_arb.c (RFCC26XX_schedulerPolicy, code the TI RF
 * driver calls above BOTH RF core clients and must not include
 * radiant_radio_hal.h - see that file's own header for why it is a separate
 * translation unit rather than a static function beside the backend).
 *
 * Only built under CONFIG_RADIANT_BACKEND_CC26XX_COEX; see
 * radiant/CMakeLists.txt.
 */
#ifndef RADIANT_RADIO_CC26XX_ARB_H_
#define RADIANT_RADIO_CC26XX_ARB_H_

#include <ti/drivers/rf/RF.h>

/*
 * Which of the backend's own requests gives way to the other, tagged onto
 * RF_ScheduleCmdParams.activityInfo (radiant_cc26xx_arb.c reads it back off
 * RF_Cmd.activityInfo - the driver copies the field through unchanged).
 *
 * The 10 ms split below mirrors radiant_radio_nrf_gate_mpsl.c's rule at its
 * gate_acquire() call sites: ADR 0013 makes the sweep elastic and tracked
 * slots inviolate, and window LENGTH is not ANT-specific knowledge (rule 1),
 * so it is what a HAL backend is allowed to look at. A tracked window or
 * transmit turnaround is under a millisecond; only a scan chunk or ED sweep
 * runs tens of milliseconds.
 */
#define RADIANT_CC26XX_PRIO_NORMAL 0u  /* sweep, scan, ED - the elastic work */
#define RADIANT_CC26XX_PRIO_HIGH   1u  /* tracked slot, transmit */

/* The backend's own RF_Handle, as an opaque pointer arb.c compares against
 * RF_Cmd::pClient - its only way to tell "ours" from "the neighbour's"
 * without knowing anything about ANT. Defined in radiant_radio_cc26xx.c. */
void *radiant_cc26xx_client(void);

#endif /* RADIANT_RADIO_CC26XX_ARB_H_ */
