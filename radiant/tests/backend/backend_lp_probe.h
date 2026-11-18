/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The low-power scenarios' handle on backend_lp_under_test.c: the nRF backend
 * with CONFIG_RADIANT_NRF_LOW_POWER, its LF counter (the GRTC, or RTC1 with
 * -DLPT_RTC), HFXO and TIMER simulated on one world clock. Every time here is
 * in microseconds of world time.
 */
#ifndef RADIANT_TESTS_BACKEND_LP_PROBE_H_
#define RADIANT_TESTS_BACKEND_LP_PROBE_H_

#include <stdbool.h>
#include <stdint.h>

/* World time the radio TIMER starts at: radio time t is world LPT_W0 + t. */
#define LPT_W0 1000u

/*
 * What differs between the two scenarios, for the cases shared by both
 * (src_lp/). The wake leads are the backend's own: LP_ARM_LEAD_US (230)
 * after EDGE 2, and CONFIG_RADIANT_NRF_LP_HFXO_STARTUP_US + 100 from EDGE 1
 * to EDGE 2.
 *
 *   LPT_TICK_US            one LF tick, rounded up. A wake edge is the last
 *                          tick at or before its ideal time, so an edge is
 *                          early by less than this.
 *   LPT_ASLEEP_SLACK_US    how far an asleep reading may lag world time: it
 *                          converts the last tick that began (0 on the 1 us
 *                          GRTC).
 *   LPT_HFXO_LEAD          EDGE 1 to EDGE 2: 800 + 100 (nRF54L15), 500 + 100
 *                          (nRF52840).
 *   LPT_HFXO_STARTUP       the simulated crystal's start-up: spike B1's
 *                          maximum on each board (680, 418).
 *   LPT_CONSTLAT_ON        CONSTLAT references while awake: 1 on the nRF54L
 *                          (errata 20), 0 on the nRF52 (no CONSTLAT).
 *   LPT_ABORT_KEEPS_LATCHED  1 if an aborted compare's latched event still
 *                          reaches its handler (nrfx's GRTC), 0 if the
 *                          abort clears it (nrf_rtc_timer).
 *   LPT_ANCHOR_SERVICED_FIRST  1 if the anchor channel is serviced before
 *                          the HF channel when both are pending: channel
 *                          allocation order against the drivers' ascending
 *                          loops (this fake's GRTC hands out 0 then 1; RTC1's
 *                          driver hands out 2 then 1).
 *   LPT_LF_OFFSET          the LF counter at world 0: RTC1 starts 3 s short
 *                          of its 24-bit wrap, so every long case crosses it.
 */
#if defined(LPT_RTC)
#define LPT_TICK_US               31u
#define LPT_ASLEEP_SLACK_US       31u
#define LPT_HFXO_LEAD             600u
#define LPT_HFXO_STARTUP          418u
#define LPT_CONSTLAT_ON           0
#define LPT_ABORT_KEEPS_LATCHED   0
#define LPT_ANCHOR_SERVICED_FIRST 0
#define LPT_LF_OFFSET             ((1ull << 24) - 3ull * 32768ull)
#else
#define LPT_TICK_US               1u
#define LPT_ASLEEP_SLACK_US       0u
#define LPT_HFXO_LEAD             900u
#define LPT_HFXO_STARTUP          680u
#define LPT_CONSTLAT_ON           1
#define LPT_ABORT_KEEPS_LATCHED   1
#define LPT_ANCHOR_SERVICED_FIRST 1
#define LPT_LF_OFFSET             0ull
#endif

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
/* lpt_reset() with the LF counter at `lf_off` at world 0. */
void     lpt_reset_lf_offset(uint64_t lf_off);
struct lpt_probe *lpt_probe(void);
void     lpt_stats(struct lpt_stats *out);

uint64_t lpt_world(void);
uint64_t lpt_radio_now(void);
/* Run the world to `world` (absolute) or by `us`, firing what falls due. */
void     lpt_run_to(uint64_t world);
void     lpt_run(uint64_t us);
/* World time of the armed anchor-channel compare; false if none is armed. */
bool     lpt_anchor_world(uint64_t *world);
/* Run to `us` before the armed anchor-channel compare (nothing if none),
 * or to just after it. */
void     lpt_run_to_anchor_minus(uint32_t us);
void     lpt_run_past_anchor(void);
/* The LF counter (extended), and a run to just after its next step. */
uint64_t lpt_lf_ticks(void);
void     lpt_run_to_next_lf_tick(uint32_t plus_us);

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

/* The hardware and driver models, for the RTC-specific cases. */
struct lpt_hw {
	uint32_t timer_count;        /* the TIMER's counter now */
	uint32_t shutdowns;          /* TIMER SHUTDOWN tasks */
	uint32_t stop_then_shutdown; /* ... of them straight after a STOP */
	uint32_t cc5;
	bool     cc5_poisoned;       /* CC5 still holds what SHUTDOWN left */
	uint64_t timer_t0_world_q;   /* world time (1/512 us) the TIMER last started */
	uint32_t exact_refusals;     /* exact compares the driver refused */
	uint32_t span_refusals;      /* compares refused as beyond its span */
	uint32_t min_exact_ahead;    /* least ticks ahead an accepted exact compare was */
	uint32_t wait_edges;         /* software starts that waited for the LF step */
	uint32_t hf_sets;            /* writes to the HF channel */
};
void     lpt_hw(struct lpt_hw *out);

/* Knobs. */
void     lpt_set_hfxo_startup(uint32_t us);
/* The next anchor-channel compare fires without its DPPI (no TIMER START,
 * no capture). */
void     lpt_break_next_dppi(void);
/* The next anchor-channel compare write is preempted for `us`: on the GRTC
 * after the write lands (before the caller's re-read), on RTC1 before the
 * driver checks it. */
void     lpt_delay_next_anchor_set(uint32_t us);
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
