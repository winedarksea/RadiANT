/* SPDX-License-Identifier: Apache-2.0 */
/* The GRTC source check in radiant_radio_nrf_lp.c's init. Original work;
 * the names are nrfx's (hal/nrf_grtc.h). */
#ifndef RADIANT_TESTS_BACKEND_FAKE_NRF_GRTC_H_
#define RADIANT_TESTS_BACKEND_FAKE_NRF_GRTC_H_
#include <nrfx.h>
typedef struct { uint32_t unused; } NRF_GRTC_Type;
#define NRF_GRTC_HAS_CLKSEL_LFLPRC 1
typedef enum {
	NRF_GRTC_CLKSEL_LFXO = 0, NRF_GRTC_CLKSEL_LFCLK = 1, NRF_GRTC_CLKSEL_LFLPRC = 2
} nrf_grtc_clksel_t;
#define NRF_GRTC ((NRF_GRTC_Type *)0)
/* Set by the test: what the GRTC counts. */
extern nrf_grtc_clksel_t fake_grtc_clksel;
static inline nrf_grtc_clksel_t nrf_grtc_clksel_get(const NRF_GRTC_Type *p)
{
	(void)p;
	return fake_grtc_clksel;
}
#endif
