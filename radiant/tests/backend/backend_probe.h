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
	/* The last transmit terminal's t_sync and exactness (HAL item 5). */
	uint64_t last_tx_t_sync;
	bool     last_tx_t_sync_exact;
	/* The last rx event's t_sync, exactness and noise sample (HAL item 5:
	 * an RX terminal reports the requested t_close, not exact). */
	uint64_t last_rx_t_sync;
	bool     last_rx_t_sync_exact;
	bool     last_rx_has_noise;
	int8_t   last_rx_noise_dbm;
	/* Run inside the core's rx/tx callback after it is recorded, so a
	 * test can arm or abort from there as the core does. */
	void (*on_rx)(int status);
	void (*on_tx)(int status);
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

/* HAL item 5 (test_terminal_contract.c): real arm calls that leave the
 * operation in the slot. backend_tx_live()/backend_rx_live() arm inside the
 * held grant; backend_tx_staged() arms behind a placed request, which
 * backend_grant_after() grants once the clock has moved `late_us` past the
 * transmit's t_sync (the arm then fails in the grant), and
 * backend_deny_staged() denies. backend_arm_ready() marks the backend
 * initialised and enabled without arming anything. */
void     backend_arm_ready(void);
int      backend_tx_live(uint64_t t_sync_at, uint32_t *op);
int      backend_rx_live(uint32_t *op);
int      backend_tx_staged(uint64_t t_sync_at, uint32_t *op);
void     backend_grant_after(uint32_t late_us);
void     backend_deny_staged(void);
uint64_t backend_now(void);

/* HAL item 5, the rest (test_terminal_contract.c). backend_rx_window() arms a
 * tracking receive on [t_open, t_close] inside the held grant;
 * backend_rx_window_staged() behind a placed request, granted by
 * backend_grant_rx_after() once the clock is `late_us` past t_open.
 * backend_frame_t_sync() delivers a frame whose event carries t_sync `t`;
 * backend_make_coded() turns the window in flight into a coded one with a
 * one-byte frame. backend_init_call(): 0 NULL table, 1 no rx, 2 no tx, 3 a
 * whole table. backend_set_max_window() sets gate_max_window_us(). */
int      backend_rx_window(uint64_t t_open, uint64_t t_close, uint32_t flags, uint32_t *op);
int      backend_rx_window_staged(uint64_t t_open, uint64_t t_close, uint32_t *op);
void     backend_grant_rx_after(uint32_t late_us);
int      backend_rx_n_filters(uint8_t n);
/* A wildcard search window: 3-byte search format, one wild_last filter, so
 * one address byte is matched in software (backend_sw_addr_len() 1). */
int      backend_rx_search_window(uint64_t t_open, uint64_t t_close, uint32_t *op);
uint8_t  backend_sw_addr_len(void);
int      backend_ed_live(uint8_t lo, uint8_t hi, uint32_t dwell_us, uint32_t *op);
void     backend_set_max_window(uint32_t us);
uint32_t backend_acquires(void);
void     backend_make_coded(void);
void     backend_frame_t_sync(bool crc_ok, uint64_t t);
uint32_t backend_window_drops(void);
int      backend_init_call(int which);
int      backend_abort_uninited(void);

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
	bool     staged_at_fire;     /* the operation was staged when the event came */
	/* A window connection on as the event's hand-back began, and still on
	 * when radiant_nrf_gate_on_grant_end() returned (before any abort). */
	bool     conns_before_handback;
	bool     conns_at_handback;
	uint32_t aborted_final;      /* ABORTED terminals, queued or delivered */
	/* backend_preempt_staged_in_grant() only. */
	bool     staged_after_arm;   /* the arm returned with its operation staged */
	bool     fired_after_arm;    /* the event came after the arm returned */
	bool     staged_after_point; /* still staged once the event's point passed */
	bool     req_pending_after_point; /* its request still placed (g.pending) */
	uint32_t forgets_after_point;     /* gate_forget_pending() calls by then */
	uint32_t aborted_after_point;     /* ABORTED terminals by then */
};
void     backend_preempt_off(void);
void     backend_preempt_arm(int kind, int mode, uint32_t n, bool late, bool held,
			     struct backend_preempt_result *out);
/* An arm made from the completion's dispatch inside a held grant (the old
 * operation ended early, its release owed) that the gate answers GATE_PENDING:
 * the successor is staged behind its own request. EXTEND_FAILED / the
 * extension skip (hand-back, then radiant_radio_abort() from the signal) is
 * taken at the n-th preemption point of the arm, or, at the point after the
 * last, once the arm has returned (the rest of the dispatch, before
 * grant_release_settle()). */
void     backend_preempt_staged_in_grant(int kind, uint32_t n, bool late,
					 struct backend_preempt_result *out);
/* An arm staged behind a placed request (GATE_PENDING, grant held), then
 * EXTEND_FAILED from the signal; with !request_placed the request is gone
 * first (g.pending false). Returns the arm's rc; ABORTED terminals and whether
 * the operation is still staged come back through the pointers. */
int      backend_signal_abort_staged(int kind, bool request_placed, uint32_t *aborted,
				     bool *still_staged);


/* test_caps_parity.c: the knobs this image built the backend with, and what
 * the stub gate answers for its lead and window ceiling (0 and 0, the direct
 * gate, until set; backend_reset() restores that). */
extern const bool backend_cfg_short_match;
extern const bool backend_cfg_ed_scan;
void     backend_set_gate_caps(uint32_t lead_us, uint32_t max_window_us);

#endif
