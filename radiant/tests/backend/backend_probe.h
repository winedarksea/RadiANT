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
	uint8_t  last_rx_body[16];
	uint8_t  last_rx_body_len;
	uint32_t last_rx_crc_rx;
	uint8_t  last_rx_filter_index;
	int8_t   last_rx_rssi_dbm;
};

struct backend_probe *backend_probe(void);
void     backend_reset(void);
void     backend_arm_rx(bool stop_on_first);
void     backend_arm_tx(void);
void     backend_frame(bool crc_ok);
void     backend_frame_at(bool crc_ok, uint32_t since_address_us);
void     backend_end_only(uint32_t advance_us);
uint8_t *backend_rx_dma_buf(void);
bool     backend_addr_carry(void);
bool     backend_rx_any(void);
uint32_t backend_torn_air_us(void);
void     backend_set_sw_addr_len(uint8_t n);
void     backend_set_report_crc_fail(void);
uint32_t backend_late_isr_drops(void);
void     backend_two_filters(void);
int      backend_program_rx(void);
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

struct radiant_rx_req;
struct radiant_tx_req;
int      backend_rx(const struct radiant_rx_req *req);
int      backend_tx(const struct radiant_tx_req *req);
int      backend_grant_rx(const struct radiant_rx_req *req);
void     backend_grant_end(void);
void     backend_disable_enable(void);
uint8_t  backend_filter_slot(uint8_t logical);

#endif
