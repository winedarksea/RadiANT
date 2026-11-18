/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The GRTC system-timer API as radiant_radio_nrf_lp.c calls it, for
 * radiant/tests/backend's low-power scenario. Original work; the names and
 * signatures are Zephyr's (include/zephyr/drivers/timer/nrf_grtc_timer.h).
 * backend_lp_under_test.c defines them against a simulated SYSCOUNTER.
 */
#ifndef RADIANT_TESTS_BACKEND_FAKE_NRF_GRTC_TIMER_H_
#define RADIANT_TESTS_BACKEND_FAKE_NRF_GRTC_TIMER_H_

#include <stdbool.h>
#include <stdint.h>

typedef void (*z_nrf_grtc_timer_compare_handler_t)(int32_t id, uint64_t expire_time,
						   void *user_data);

int32_t  z_nrf_grtc_timer_chan_alloc(void);
void     z_nrf_grtc_timer_chan_free(int32_t chan);
uint64_t z_nrf_grtc_timer_read(void);
uint32_t z_nrf_grtc_timer_compare_evt_address_get(int32_t chan);
int      z_nrf_grtc_timer_set(int32_t chan, uint64_t target_time,
			      z_nrf_grtc_timer_compare_handler_t handler, void *user_data);
void     z_nrf_grtc_timer_abort(int32_t chan);

#endif
