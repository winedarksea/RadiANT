/* SPDX-License-Identifier: Apache-2.0 */
/* The POWER task radiant_radio_nrf.c names on an errata-20 part. */
#ifndef RADIANT_TESTS_BACKEND_FAKE_NRF_POWER_H_
#define RADIANT_TESTS_BACKEND_FAKE_NRF_POWER_H_
#include <nrfx.h>
typedef struct { uint32_t unused; } NRF_POWER_Type;
typedef enum { NRF_POWER_TASK_CONSTLAT, NRF_POWER_TASK_LOWPWR } nrf_power_task_t;
#define NRF_POWER ((NRF_POWER_Type *)0)
static inline void nrf_power_task_trigger(NRF_POWER_Type *p, nrf_power_task_t t)
{ (void)p; (void)t; }
#endif
