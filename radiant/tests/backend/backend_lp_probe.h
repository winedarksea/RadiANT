/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The low-power scenario's handle on backend_lp_under_test.c: the nRF backend
 * with CONFIG_RADIANT_NRF_LOW_POWER, its GRTC, HFXO and TIMER simulated on one
 * world clock in microseconds (the GRTC's ticks).
 */
#ifndef RADIANT_TESTS_BACKEND_LP_PROBE_H_
#define RADIANT_TESTS_BACKEND_LP_PROBE_H_

#include <stdbool.h>
#include <stdint.h>

/* World time the radio TIMER starts at: radio time t is world LPT_W0 + t. */
#define LPT_W0 1000u

/* lp_state, as the stats struct reports it. */
#define LPT_OFF       0
#define LPT_AWAKE     1
#define LPT_RELEASING 2
#define LPT_ASLEEP    3
#define LPT_WAKING    4

struct lpt_stats {
	uint32_t state, sleeps, wakes, late_anchors, late_wakes, hfxo_late_edge2,
		 reanchors, range_errors, lf_taken, timer_taken, disagreements,
		 max_disagree_us, max_wake_slack_us, max_wake_late_us, asap_wakes,
		 release_cancels, spurious, hfxo_errors, refused, edge_order;
};

struct lpt_probe {
	uint32_t tx_events;
	int      last_tx_status;
	uint32_t failed_events;
	uint32_t rx_events;
	int      last_rx_status;
	/* On a FAILED transmit, arm another this far ahead (0: do not). */
	uint32_t on_failed_arm_us;
	/* On a good transmit, arm the next one period_us after it (0: do not). */
	uint32_t period_us;
	uint64_t next_sync;
	int      nested_rc;
	uint32_t nested_arms;
};

/* Boot: TIMER at LPT_W0, HFXO and CONSTLAT held, low power initialised and
 * its boot re-anchor taken, radio enabled, no linger. */
void     lpt_reset(void);
/* lpt_reset() with LFCLK running from the RC oscillator. */
void     lpt_reset_lf_rc(void);
struct lpt_probe *lpt_probe(void);
void     lpt_stats(struct lpt_stats *out);

uint64_t lpt_world(void);
uint64_t lpt_radio_now(void);
/* Run the world to `world` (absolute) or by `us`, firing what falls due. */
void     lpt_run_to(uint64_t world);
void     lpt_run(uint64_t us);

/* The transmit's TXEN lead before t_sync. */
uint64_t lpt_tx_lead(void);
/* radiant_radio_tx() at radio time t_sync. */
int      lpt_tx_at(uint64_t t_sync);
/* World time the programmed transmit's TXEN compare fires; false if no
 * transmit is programmed on a running TIMER. */
bool     lpt_tx_programmed(uint64_t *world_start);
/* Run to the programmed transmit, then its END interrupt. */
void     lpt_tx_end(void);
/* radiant_radio_rx() of a tracking window opening at radio time t_open. */
int      lpt_rx_at(uint64_t t_open, uint32_t len_us);
/* World time the programmed receive's RXEN compare fires. */
bool     lpt_rx_programmed(uint64_t *world_start);
/* The receive's RXEN lead before t_open. */
uint64_t lpt_rx_lead(void);

int      lpt_abort(void);
int      lpt_disable(void);
int      lpt_enable(void);
/* radio_disable_now(), as the ISR and abort paths call it. */
void     lpt_radio_disable_now(void);
/* gate_acquire() directly, as an arm call makes it. */
int      lpt_acquire(uint64_t t_from, bool prio_normal);
/* An arm that the wake overtook: the request is copied, then lp_staged(). */
void     lpt_stage_tx_late(uint64_t t_sync);

int      lpt_state(void);
bool     lpt_staged(void);
bool     lpt_op_idle(void);
bool     lpt_hfxo_on(void);
bool     lpt_timer_running(void);
int      lpt_constlat_refs(void);
uint32_t lpt_hfxo_requests(void);
uint64_t lpt_hfxo_on_us(void);
uint32_t lpt_disable_tasks(void);
bool     lpt_timer_at_anchor(void);

/* Knobs. */
void     lpt_set_hfxo_startup(uint32_t us);
/* The next anchor-channel compare fires without its DPPI (no TIMER START,
 * no capture). */
void     lpt_break_next_dppi(void);
/* The next anchor-channel compare write takes `ticks` (a preemption). */
void     lpt_delay_next_anchor_set(uint32_t ticks);
void     lpt_set_disable_hangs(bool on);
/* Hold the GRTC interrupt off until world time `world` (0: none). */
void     lpt_hold_isr_until(uint64_t world);
/* Make HFXO requests (onoff_request) and/or starts (the callback) fail. */
void     lpt_hfxo_errors(bool on_request, bool on_start);
/* Another client holds the HFXO, so a request completes synchronously. */
void     lpt_hfxo_shared(bool on);
bool     lpt_hf_fallback(void);
void     lpt_set_fallback_rc(int rc);
/* Let the system work queue run (a deferred FAILED). */
void     lpt_run_work(void);
/* lp_hfxo_boot() against an HFXO that never starts: its rc, and whether the
 * client was left queued on the manager. */
int      lpt_boot_hfxo_timeout(bool *still_queued);

#endif
