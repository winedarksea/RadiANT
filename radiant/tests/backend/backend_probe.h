/* SPDX-License-Identifier: Apache-2.0 */
#ifndef RADIANT_TESTS_BACKEND_PROBE_H_
#define RADIANT_TESTS_BACKEND_PROBE_H_

#include <stdbool.h>
#include <stdint.h>

struct backend_probe {
	bool     arm_follow_on;
	uint32_t rx_events;
	uint32_t tx_events;
	uint32_t ed_events;
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
int      backend_rx_staged(const struct radiant_rx_req *req);
int      backend_grant_staged(void);
uint8_t  backend_op_filter_byte(uint8_t i, uint8_t b);
uint8_t  backend_op_n_filters(void);
uint16_t backend_op_crc_init(void);
bool     backend_filt_memo_holds(const struct radiant_rx_req *req);
void     backend_program_ed(void);

/* An arm preempted by the grant's end (test_arm_handback.c): the simulated
 * event, taken at the n-th preemption point of the arm (each outermost unlock
 * of the zero-latency lock, and the return of gate_acquire()). */
enum { BACKEND_ARM_TX, BACKEND_ARM_RX, BACKEND_ARM_ED };
enum {
	BACKEND_PRE_END,         /* hand-back and ACTION_END (TIMER0, a release) */
	BACKEND_PRE_END_START,   /* the same, then the next START */
	BACKEND_PRE_ABORT_END,   /* hand-back, then radiant_radio_abort() (EXTEND_FAILED, skip) */
	BACKEND_PRE_DENIED,      /* a queued denial reaching gate_wq */
	BACKEND_PRE_BLOCKED,     /* MPSL refuses the placed request; gate_wq denies */
	BACKEND_PRE_DEADLINE,    /* the placed request's deadline expires; gate_wq denies */
	BACKEND_PRE_GRANT_EARLY, /* the placed request's START before the arm staged it */
	BACKEND_PRE_DENY_THEN_START, /* a stale denial, then the request's START after
				      * the staging and before the arm settles */
};
struct backend_preempt_result {
	int      rc;
	bool     fired;
	bool     marked_at_fire;     /* the event found the arm in progress */
	bool     radio_written_after_handback;
	bool     conns_after_arm;
	uint32_t terminals_during_arm;
	bool     marked_after_arm;
	bool     staged_path;        /* gate_acquire() answered GATE_PENDING */
	/* After the world has settled: every denial run, the request granted
	 * if still placed, every loan ended. */
	bool     conns_final;
	bool     op_idle_final;
	uint32_t terminals_final;
	int      next_rc;            /* the next arm, in a fresh grant */
};
void     backend_preempt_off(void);
void     backend_preempt_arm(int kind, int mode, uint32_t n, bool late, bool held,
			     struct backend_preempt_result *out);

#endif
