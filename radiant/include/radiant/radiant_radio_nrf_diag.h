/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf_diag.h - a bench-diagnostic accessor for the nRF backend,
 * deliberately NOT part of radiant_radio_hal.h.
 *
 * Provenance: clean-room. TEMP peripheral task/event/result names and the
 * 0.25 degC result-register step come from Nordic's public MDK headers
 * (nrfx's hal/nrf_temp.h, Apache-2.0/BSD-3-Clause, already a dependency).
 * Nothing here derives from sdk-ant. See docs/decisions/0002-clean-room-policy.md.
 *
 * nRF52840 errata 153/225 give the RADIO's RSSI a temperature-dependent
 * error; radiant_radio_nrf.c corrects for it (see "RSSI temperature
 * correction" there). This accessor refreshes that correction cache and lets
 * a bench run record die temperature alongside RSSI independently. Kept out
 * of radiant_radio_hal.h because the frozen HAL contract applies to every
 * backend and a TEMP reading is nRF-specific; include this directly for bench
 * tooling or a debug shell.
 */

#ifndef RADIANT_RADIO_NRF_DIAG_H_
#define RADIANT_RADIO_NRF_DIAG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Read the die temperature, in centi-degrees Celsius (2650 == 26.50 degC).
 *
 * THREAD CONTEXT ONLY: this busy-waits tens of microseconds for the TEMP
 * peripheral's conversion, a cost never budgeted against the radio ISR's
 * timing (radiant_radio_nrf.c's ARM_SETUP_US, the 80 us arm-from-interrupt
 * path). Call from a bench thread or debug console between radio operations,
 * never from the radio ISR or a radiant_radio_cbs callback.
 *
 * Returns RADIANT_RADIO_OK_RC, or RADIANT_RADIO_EINVAL if out is NULL.
 */
int radiant_radio_nrf_die_temp_c(int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RADIO_NRF_DIAG_H_ */
