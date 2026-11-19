/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The radiant.backend.lp_mpsl scenario's handle on
 * backend_lp_mpsl_under_test.c: the nRF backend with
 * CONFIG_RADIANT_NRF_LOW_POWER on the MPSL gate's build (power plan pass B6).
 * Every time here is in microseconds of world time; the GRTC reads world time
 * plus LMT_LF_OFFSET.
 */
#ifndef RADIANT_TESTS_BACKEND_LP_MPSL_PROBE_H_
#define RADIANT_TESTS_BACKEND_LP_MPSL_PROBE_H_

#include <stdbool.h>
#include <stdint.h>

/* World time the boot TIMER starts at: until the first release radio time t
 * is world LMT_W0 + t. */
#define LMT_W0        1000u
/* The GRTC at world 0: not zero, so a conversion that forgot the anchor shows. */
#define LMT_LF_OFFSET 5000000ull

/* lp_state, as the stats struct reports it. */
#define LMT_OFF    0
#define LMT_AWAKE  1
#define LMT_ASLEEP 3

struct lmt_stats {
	uint32_t size, state, sleeps, wakes, late_anchors, late_wakes, range_errors,
		 lf_taken, timer_taken, disagreements, max_disagree_us;
	int32_t  last_disagree_q;
	uint32_t hfxo_errors, refused, slot_awake_us, start_err_n;
	int32_t  start_err_min_us, start_err_max_us, start_err_last_us;
	uint32_t missed_releases;
};

/* Boot as radiant_radio_init() does: the HFXO requested, the TIMER started at
 * LMT_W0, low power initialised 50 us later (or refused, with an RC LFCLK). */
void     lmt_reset(void);
void     lmt_reset_lf_rc(void);
void     lmt_stats(struct lmt_stats *out);

void     lmt_run(uint64_t us);
uint64_t lmt_world(void);
uint64_t lmt_radio_now(void);
/* The radio TIMER's rate against the world (the HFXO against the LFXO). */
void     lmt_set_hf_ppm(int32_t ppm);

/* What the MPSL gate calls: SIGNAL_START (with the requested start, or 0),
 * and a timeslot's end (from the ACTION_END funnel, or a backstop). */
void     lmt_slot_start(uint64_t r_start);
void     lmt_slot_end(bool backstop);

/* Inside a timeslot: radiant_radio_tx() at radio time t_sync, and the world
 * time its TXEN compare fires (false if none is programmed on a running
 * TIMER). lmt_retire_op() ends it as its END interrupt would. */
uint64_t lmt_tx_lead(void);
int      lmt_tx_at(uint64_t t_sync);
bool     lmt_tx_programmed(uint64_t *world_start);
void     lmt_retire_op(void);

int      lmt_state(void);
bool     lmt_timer_running(void);
uint32_t lmt_timer_count(void);
uint32_t lmt_timer_starts(void);
bool     lmt_hfxo_on(void);
uint32_t lmt_hfxo_requests(void);
uint32_t lmt_hfxo_releases(void);
bool     lmt_timer_at_anchor(void);
uint32_t lmt_disable_tasks(void);
void     lmt_radio_disable_now(void);
/* (D)PPI: the TIMER-driven window connections share one handle in this
 * model; RXREADY->RSSISTART and ADDRESS->capture have their own. */
uint32_t lmt_conn_on(void);
void     lmt_conn_enable_all(void);
uint32_t lmt_conn_bit_timer(void);
uint32_t lmt_conn_bits_other(void);

#endif
