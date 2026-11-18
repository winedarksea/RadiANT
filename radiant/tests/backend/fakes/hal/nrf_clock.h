/* SPDX-License-Identifier: Apache-2.0 */
/* The LFCLK source check in radiant_radio_nrf_lp.c's init. Original work;
 * the names are nrfx's (hal/nrf_clock.h). */
#ifndef RADIANT_TESTS_BACKEND_FAKE_NRF_CLOCK_H_
#define RADIANT_TESTS_BACKEND_FAKE_NRF_CLOCK_H_
#include <nrfx.h>
typedef struct { uint32_t unused; } NRF_CLOCK_Type;
typedef enum { NRF_CLOCK_DOMAIN_LFCLK, NRF_CLOCK_DOMAIN_HFCLK } nrf_clock_domain_t;
typedef enum {
	NRF_CLOCK_LFCLK_RC = 1, NRF_CLOCK_LFCLK_XTAL = 2, NRF_CLOCK_LFCLK_SYNTH = 3
} nrf_clock_lfclk_t;
#define NRF_CLOCK ((NRF_CLOCK_Type *)0)
/* Set by the test: what LFCLK runs from. */
extern nrf_clock_lfclk_t fake_lfclk_src;
static inline bool nrf_clock_is_running(const NRF_CLOCK_Type *p, nrf_clock_domain_t d,
					void *src)
{
	(void)p;
	(void)d;
	*(nrf_clock_lfclk_t *)src = fake_lfclk_src;
	return true;
}
#endif
