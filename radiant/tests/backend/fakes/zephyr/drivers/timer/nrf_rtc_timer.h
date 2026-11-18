/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The RTC1 system-timer API as radiant_radio_nrf_lp.c calls it, for
 * radiant/tests/backend's lp_rtc scenario. Original work; the names,
 * signatures and NRF_RTC_TIMER_MAX_SCHEDULE_SPAN are Zephyr's
 * (include/zephyr/drivers/timer/nrf_rtc_timer.h, with the default 24-bit
 * counter). backend_lp_under_test.c defines the functions against a
 * simulated RTC1 and the driver's scheduling rules.
 */
#ifndef RADIANT_TESTS_BACKEND_FAKE_NRF_RTC_TIMER_H_
#define RADIANT_TESTS_BACKEND_FAKE_NRF_RTC_TIMER_H_

#include <stdbool.h>
#include <stdint.h>

#define NRF_RTC_TIMER_MAX_SCHEDULE_SPAN (1ul << 23)

typedef void (*z_nrf_rtc_timer_compare_handler_t)(int32_t id, uint64_t expire_time,
						  void *user_data);

int32_t  z_nrf_rtc_timer_chan_alloc(void);
void     z_nrf_rtc_timer_chan_free(int32_t chan);
uint64_t z_nrf_rtc_timer_read(void);
uint32_t z_nrf_rtc_timer_compare_evt_address_get(int32_t chan);
int      z_nrf_rtc_timer_set(int32_t chan, uint64_t target_time,
			     z_nrf_rtc_timer_compare_handler_t handler, void *user_data);
int      z_nrf_rtc_timer_exact_set(int32_t chan, uint64_t target_time,
				   z_nrf_rtc_timer_compare_handler_t handler,
				   void *user_data);
void     z_nrf_rtc_timer_abort(int32_t chan);

#endif
