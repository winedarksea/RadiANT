/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf_diag.h - a bench-diagnostic accessor for the nRF backend,
 * deliberately NOT part of radiant_radio_hal.h.
 *
 * Provenance: clean-room. The TEMP peripheral's task/event/result names and
 * the 0.25 degC step of its result register come from the public Nordic
 * MDK headers shipped with NCS (nrfx's hal/nrf_temp.h), which are Apache-2.0
 * / BSD-3-Clause and already a dependency. Nothing here derives from sdk-ant.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * Why this exists
 * ---------------------------------------------------------------------------
 * nRF52840 errata 153 (and 225, which is nRF52820/nRF52833-only and does not
 * apply to this part) say the RADIO's RSSI has a temperature-dependent error.
 * radiant_radio_nrf.c now corrects for it - see "RSSI temperature correction"
 * in that file, sourced from Nordic's own open-source 802.15.4 driver rather
 * than derived here - and this accessor is both the correction cache's
 * periodic refresh and, independently, a way for a bench run to record die
 * temperature alongside RSSI: not every confound is a correctable one, and
 * making one visible remains worth doing even after another is fixed. See
 * docs/sdk-ant-comparison.md item 1.
 *
 * This is deliberately not in radiant_radio_hal.h: the frozen HAL contract
 * is what every backend - including the mock and a future EFR32/RAIL one -
 * has to satisfy, and a TEMP peripheral reading is specific to this one.
 * A caller that wants it includes this header directly, the way bench
 * tooling or a debug shell command would.
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
 * THREAD CONTEXT ONLY. The TEMP peripheral's own datasheet-quoted conversion
 * time is on the order of tens of microseconds and this function busy-waits
 * for it, which is a cost the radio ISR's timing budgets (see
 * radiant_radio_nrf.c's ARM_SETUP_US and the 80 us arm-from-interrupt path)
 * were never measured against. Call it from a bench thread or a debug
 * console handler between radio operations, never from radiant_radio_nrf.c's
 * own ISR or from inside a radiant_radio_cbs callback.
 *
 * Returns RADIANT_RADIO_OK_RC, or RADIANT_RADIO_EINVAL if out is NULL.
 */
int radiant_radio_nrf_die_temp_c(int16_t *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RADIO_NRF_DIAG_H_ */
