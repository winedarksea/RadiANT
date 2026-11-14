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
int      backend_program_tx(uint32_t d_us, uint32_t step);
bool     backend_tx_conn_live(void);
uint32_t backend_shorts(void);
void     backend_grant_late_tx(void);
void     backend_set_gate_failed(void);
bool     backend_gate_failed(void);
void     backend_finish_failed(void);
int      backend_abort(void);
void     backend_take_slot_unclaimed(void);
void     backend_claim_terminal(void);
bool     backend_grant_held(void);
bool     backend_op_retired(void);

#endif
