/* SPDX-License-Identifier: Apache-2.0 */
/* nrfx's CONSTLAT reference count, as radiant_radio_nrf_lp.c calls it.
 * Defined by backend_lp_under_test.c. */
#ifndef RADIANT_TESTS_BACKEND_FAKE_NRFX_POWER_H_
#define RADIANT_TESTS_BACKEND_FAKE_NRFX_POWER_H_
int nrfx_power_constlat_mode_request(void);
int nrfx_power_constlat_mode_free(void);
#endif
