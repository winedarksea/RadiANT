/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RADIANT_TESTS_BACKEND_PROBE_H_
#define RADIANT_TESTS_BACKEND_PROBE_H_

#include <stdbool.h>
#include <stdint.h>

struct backend_probe {
	bool     arm_follow_on;
	uint32_t rx_events;
	uint32_t tx_events;
	int      last_rx_status;
	int      last_tx_status;
};

struct backend_probe *backend_probe(void);
void     backend_reset(void);
void     backend_arm_rx(bool stop_on_first);
void     backend_arm_tx(void);
void     backend_frame(bool crc_ok);
void     backend_tx_end(void);
void     backend_window_close(void);
void     backend_dispatch(void);
uint32_t backend_releases(void);
bool     backend_grant_held(void);
bool     backend_op_retired(void);

#endif
