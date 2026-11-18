/* SPDX-License-Identifier: Apache-2.0 */
/* The names the backend takes from this header. The low-power scenario
 * (backend_lp_under_test.c) defines the two functions. */
#define CLOCK_CONTROL_NRF_SUBSYS_HF ((clock_control_subsys_t)0)

struct onoff_manager;
enum nrf_lfclk_start_mode {
	CLOCK_CONTROL_NRF_LF_START_NOWAIT,
	CLOCK_CONTROL_NRF_LF_START_AVAILABLE,
	CLOCK_CONTROL_NRF_LF_START_STABLE,
};
struct onoff_manager *z_nrf_clock_control_get_onoff(clock_control_subsys_t sys);
void z_nrf_clock_control_lf_on(enum nrf_lfclk_start_mode start_mode);
