/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_diag.h - the diagnostic counters behind MESG_RADIANT_DIAG (0xF7).
 *
 * Provenance: clean-room. Written from this project's own source and bench
 * measurements; see docs/decisions/0023-radiant-diag-message.md.
 *
 * WHAT THIS IS. Thirteen fixed pages of eight u32 counters each, read by a host
 * with MESG_REQUEST [page, 0xF7]. Pages 0, 5 and 6 are counters the core
 * already keeps (radiant_api_stats, radiant_search_stats). Pages 1 and 2 are
 * counted here, behind CONFIG_RADIANT_DIAG_COUNTERS. Pages 3 and 4 come from
 * the backend through radiant_radio_diag_get(), one generic table that every
 * backend fills with what it can measure. Page 7 (ADR 0022 gate 3) is mixed:
 * its first three fields come from that same backend table, the other five
 * are counted here. Page 8 (timing) is mixed the same way: receive-drain
 * timings from the backend table, scheduler pass timings counted here.
 * Pages 9 (pass breakdown) and 10 (receive integrity) are mixed the same way;
 * their cycle-timed fields use the A0 cycle clock below. Pages 11 (scheduler
 * contention) and 12 (stale events by site) are the core's.
 *
 * ZERO COST WHEN OFF. Without CONFIG_RADIANT_DIAG_COUNTERS every RADIANT_DIAG_*
 * macro below is an empty statement that does not evaluate its arguments,
 * radiant_diag.c is not built and no backend defines radiant_radio_diag_get(),
 * so the zero-cost CI job's section and symbol comparison is unchanged.
 *
 * NOT ATOMIC. The counters are plain u32s incremented from the radio callback
 * and from threads. A lost increment under a rare interleaving is accepted:
 * these are diagnostic and read as before/after differences, and atomics on
 * the radio callback path would cost every image that turns them on.
 */

#ifndef RADIANT_DIAG_H_
#define RADIANT_DIAG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Pages. Fixed forever (pinned v1): a new meaning is a new page. */
#define RADIANT_DIAG_PAGE_API        0u
#define RADIANT_DIAG_PAGE_MASTER_RX  1u
#define RADIANT_DIAG_PAGE_TRANSFER   2u
#define RADIANT_DIAG_PAGE_BACKEND_A  3u
#define RADIANT_DIAG_PAGE_BACKEND_B  4u
#define RADIANT_DIAG_PAGE_SEARCH_A   5u
#define RADIANT_DIAG_PAGE_SEARCH_B   6u
#define RADIANT_DIAG_PAGE_HANDOFF    7u
#define RADIANT_DIAG_PAGE_TIMING     8u
#define RADIANT_DIAG_PAGE_PASS       9u
#define RADIANT_DIAG_PAGE_RX_INTEGRITY 10u
#define RADIANT_DIAG_PAGE_SCHED      11u
#define RADIANT_DIAG_PAGE_STALE      12u
#define RADIANT_DIAG_PAGES           13u

/*
 * Page 7, the search/track hand-off (ADR 0022 gate 3). Does a search
 * reception running up to a scan chunk's close delay the tracked window armed
 * after it? Fields 0-2 are the backend's (a search frame in flight when the
 * chunk closed or was aborted; a search frame ending close to the chunk's
 * close), 3-5 the core's (bounded receive windows opened late, the worst
 * lateness, and bounded windows expired while a scan chunk held the radio).
 *
 * Fields 6-7 are the backend's again: a transmit (6) and a receive window (7)
 * whose start compare was written behind the counter, so the arm was refused
 * ETIME at programming (the scheduler reports it MISSED). Expected 0 or single
 * digits; a backend without such a check reads RADIANT_DIAG_NOT_MEASURED.
 * Until 2026-11-14 they were reserved. Before that they held "a tracked slot
 * opened just after the search heard the same device number, and how many of
 * those missed". The 2026-11-12 gate-3 run showed that count cannot measure a
 * hand-off cost: it only counts when the search HEARS a frame, which the short
 * match does nine times as often, so it grows with discovery whatever made the
 * slot miss (it read 100 % missed on the off image too).
 */
#define RADIANT_DIAG_HANDOFF_SRCH_CUT_CLOSE        0u
#define RADIANT_DIAG_HANDOFF_SRCH_CUT_ABORT        1u
#define RADIANT_DIAG_HANDOFF_SRCH_END_NEAR_CLOSE   2u
#define RADIANT_DIAG_HANDOFF_TRK_OPEN_LATE         3u
#define RADIANT_DIAG_HANDOFF_TRK_OPEN_LATE_MAX_US  4u
#define RADIANT_DIAG_HANDOFF_TRK_MISSED_BEHIND_SCAN 5u
#define RADIANT_DIAG_HANDOFF_TX_START_MISSED       6u
#define RADIANT_DIAG_HANDOFF_RX_START_MISSED       7u

/*
 * Page 8, timing (measurement only; 2026-11-13 performance review). Where a
 * backend's receive drain and the scheduler's interrupts-off pass spend their
 * time, as worst cases since boot ("level" fields: read the later value, a
 * difference means nothing) plus two counts.
 *
 *   0 drain_max_frames  frames examined by the drain that set drain_max_us
 *                       (page 4) - backend
 *   1 frame_max_us      longest work on one receive entry, the core's rx
 *                       callback excluded - backend
 *   2 crc_max_us        longest software CRC over one frame - backend
 *   3 rx_cb_max_us      longest synchronous core rx callback - backend
 *   4 pass_max_us       longest scheduler pass (interrupts off) - core
 *   5 pass_steps_max    most pass_step() iterations in one pass - core
 *   6 pass_over_100us   passes longer than 100 us - core, count
 *   7 drain_over_250us  receive drains longer than 250 us - backend, count
 *
 * cc26xx measures 0 and 7 on the RAT (0.25 us ticks, truncated to us) and,
 * since 2026-11-15, 1-3 on the A0 cycle clock (same meaning, still us; a RAT
 * read cost a few us per sample). The nRF backend trusts its hardware CRC and
 * has no drain loop, so they read RADIANT_DIAG_NOT_MEASURED there. 4-6 use
 * radiant_radio_now() on both.
 */
#define RADIANT_DIAG_TIMING_DRAIN_MAX_FRAMES 0u
#define RADIANT_DIAG_TIMING_FRAME_MAX_US     1u
#define RADIANT_DIAG_TIMING_CRC_MAX_US       2u
#define RADIANT_DIAG_TIMING_RX_CB_MAX_US     3u
#define RADIANT_DIAG_TIMING_PASS_MAX_US      4u
#define RADIANT_DIAG_TIMING_PASS_STEPS_MAX   5u
#define RADIANT_DIAG_TIMING_PASS_OVER_100US  6u
#define RADIANT_DIAG_TIMING_DRAIN_OVER_250US 7u

/*
 * Page 9, pass breakdown (measurement only; 2026-11-15 performance review).
 * Worst cases since boot except field 7 ("level" fields: read the later
 * value, a difference means nothing).
 *
 *   0 sweep_max_us       expiry sweep (pass_step()'s slot loop), summed over
 *                        one pass, notify_done() callbacks excluded - core
 *   1 decide_max_us      the rest of pass_step() (stale/preempt test,
 *                        arm_next(), notify_armed()), backend arm calls and
 *                        notify_done() callbacks excluded, summed - core
 *   2 arm_call_max_us    backend calls (radiant_radio_rx/tx/ed and
 *                        abort_armed()'s radiant_radio_abort()) made by one
 *                        pass, summed - core. A callback nested inside one
 *                        counts here and in 3; 1 is the remainder, floored
 *                        at 0
 *   3 done_cb_max_us     notify_done() consumer callbacks within one
 *                        scheduler entry, summed - core. An entry is a pass,
 *                        or the body of a backend callback (hal_rx/hal_tx/
 *                        hal_ed) not nested in a pass, where most callbacks
 *                        run; the pass such a body ends with is a separate
 *                        entry. Until 2026-11-16 only a pass's own callbacks
 *                        reached it, so it read 0 on hardware. A callback
 *                        from radiant_sched_cancel() (thread, no entry) is
 *                        not timed
 *   4 book_max_us        book_ahead() - core; RADIANT_DIAG_NOT_MEASURED
 *                        without CONFIG_RADIANT_SCHED_BOOKING
 *   5 pass_to_arm_max_us the pass's `now` to the backend's own `now` at its
 *                        early arm check, radio clock - backend. A max from
 *                        0, no "no sample" value. An arm not made from a
 *                        pass is not sampled, so MPSL builds (arms granted
 *                        later are programmed with the stamp cleared) rarely
 *                        sample it.
 *   6 arm_slack_min_us   smallest t_open - (now' + air lead) at that check
 *                        on an arm it accepted, i32, never negative (refused
 *                        arms are field 7). RADIANT_DIAG_NO_SAMPLE until the
 *                        first accepted arm - backend
 *   7 early_etime        arms refused ETIME by that early check, rx + tx -
 *                        backend, count
 *
 * 0-4 are on the A0 cycle clock and read RADIANT_DIAG_NOT_MEASURED where it
 * does not run (native_sim, a core without DWT).
 */
#define RADIANT_DIAG_PASS_SWEEP_MAX_US       0u
#define RADIANT_DIAG_PASS_DECIDE_MAX_US      1u
#define RADIANT_DIAG_PASS_ARM_CALL_MAX_US    2u
#define RADIANT_DIAG_PASS_DONE_CB_MAX_US     3u
#define RADIANT_DIAG_PASS_BOOK_MAX_US        4u
#define RADIANT_DIAG_PASS_TO_ARM_MAX_US      5u
#define RADIANT_DIAG_PASS_ARM_SLACK_MIN_US   6u
#define RADIANT_DIAG_PASS_EARLY_ETIME        7u

/* Page 9 field 6 before its first sample. Not RADIANT_DIAG_NOT_MEASURED:
 * the backend measures it, it has not seen an accepted arm yet. */
#define RADIANT_DIAG_NO_SAMPLE 0x7FFFFFFFu

/*
 * Page 10, receive integrity (measurement only; 2026-11-15 review).
 *
 *   0 crc_repaired           radiant_api_stats, counts. RADIANT_DIAG_NOT_
 *   1 crc_unrepairable       MEASURED without CONFIG_RADIANT_CRC_REPAIR
 *   2 crc_repair_refuted
 *   3 crc_repair_implausible
 *   4 late_isr_drops         frames dropped by the nRF late-ISR guard (a torn
 *                            END snapshot, or a frame lost because the next
 *                            one ended while it was read) - nRF, count
 *   5 end_late_max_us        worst END-interrupt lateness, now - (CC_SYNC +
 *                            software address, body and CRC airtime) - nRF,
 *                            from accepted snapshots only. Past the next
 *                            frame's ADDRESS the snapshot is torn and lands
 *                            in field 4 instead
 *   6 t_sync_max_us          longest rx_t_sync() - cc26xx, A0 clock
 *   7 match_max_us           longest rx_match() plus the address/body copies
 *                            of one receive entry - cc26xx, A0 clock
 *
 * 5-7 are levels. A backend without the field reads RADIANT_DIAG_NOT_MEASURED.
 */
/*
 * Page 11, scheduler contention (2026-11-15, TODO.md section 1): the
 * radiant_sched_stats that never reached the host, and the API's split of a
 * lost tracked slot into its causes. All counts.
 *
 *   0 windows_merged   receive windows armed carrying more than one channel
 *   1 preempted        running operations cut short for a nearer deadline
 *   2 replans          windows rebuilt before opening
 *   3 arm_rejected     arm calls refused other than EBUSY
 *   4 tx_armed         accepted transmits
 *   5 stale_events     events for an operation already retired
 *   6 slots_collided   tracked slots the scheduler dropped (never armed,
 *                      expired behind another window, or cut short):
 *                      EVENT_CHANNEL_COLLISION, not RX_FAIL. Against page 0
 *                      field 4 (slots_missed: armed, ran, heard nothing)
 *   7 turn_yields      bounded receives let go so that one which lost the
 *                      last contention could be served (ADR 0024)
 */
#define RADIANT_DIAG_SCHED_WINDOWS_MERGED 0u
#define RADIANT_DIAG_SCHED_PREEMPTED      1u
#define RADIANT_DIAG_SCHED_REPLANS        2u
#define RADIANT_DIAG_SCHED_ARM_REJECTED   3u
#define RADIANT_DIAG_SCHED_TX_ARMED       4u
#define RADIANT_DIAG_SCHED_STALE_EVENTS   5u
#define RADIANT_DIAG_SCHED_SLOTS_COLLIDED 6u
#define RADIANT_DIAG_SCHED_TURN_YIELDS    7u

/*
 * Page 12, stale scheduler events by site (2026-11-16). Page 11 field 5 split
 * by where radiant_sched.c dropped the event; 0-4 sum to it (up to a lost
 * increment, see NOT ATOMIC). All counts, core.
 *
 *   0 rx_frame         a receive event carrying a frame (OK or CRC_FAIL) for
 *                      an operation already retired: heard, then dropped
 *   1 rx_end           any other receive event for a retired operation (the
 *                      ABORTED terminal every preemption makes, mostly)
 *   2 rx_filter_index  a frame for the armed window with a filter index the
 *                      window did not have (route_rx())
 *   3 tx               a transmit event for a retired operation
 *   4 ed               an energy-detect event for a retired operation;
 *                      RADIANT_DIAG_NOT_MEASURED without CONFIG_RADIANT_ED_SCAN
 *   5-7                reserved, RADIANT_DIAG_NOT_MEASURED
 */
#define RADIANT_DIAG_STALE_RX_FRAME        0u
#define RADIANT_DIAG_STALE_RX_END          1u
#define RADIANT_DIAG_STALE_RX_FILTER_INDEX 2u
#define RADIANT_DIAG_STALE_TX              3u
#define RADIANT_DIAG_STALE_ED              4u

#define RADIANT_DIAG_RXI_CRC_REPAIRED           0u
#define RADIANT_DIAG_RXI_CRC_UNREPAIRABLE       1u
#define RADIANT_DIAG_RXI_CRC_REPAIR_REFUTED     2u
#define RADIANT_DIAG_RXI_CRC_REPAIR_IMPLAUSIBLE 3u
#define RADIANT_DIAG_RXI_LATE_ISR_DROPS         4u
#define RADIANT_DIAG_RXI_END_LATE_MAX_US        5u
#define RADIANT_DIAG_RXI_T_SYNC_MAX_US          6u
#define RADIANT_DIAG_RXI_MATCH_MAX_US           7u

/* Thresholds of the two page-8 counts. */
#define RADIANT_DIAG_PASS_SLOW_US  100u
#define RADIANT_DIAG_DRAIN_SLOW_US 250u

/* A search frame ending this close to its chunk's hardware close counts as
 * "near close" (field 2): its END interrupt chain then runs into the time
 * the next arm needs. */
#define RADIANT_DIAG_SRCH_NEAR_CLOSE_US 300u

/* Reply body: [0] page, [1] version, [2] flags, [3] backend id, eight u32 LE. */
#define RADIANT_DIAG_FIELDS          8u
#define RADIANT_DIAG_VERSION         1u
#define RADIANT_DIAG_BODY_LEN        (4u + 4u * RADIANT_DIAG_FIELDS)
/* [2] bit 0: at least one field on this page reads RADIANT_DIAG_NOT_MEASURED. */
#define RADIANT_DIAG_FLAG_UNMEASURED 0x01u
/* A field this build or backend cannot count. Never 0, which means zero. */
#define RADIANT_DIAG_NOT_MEASURED    0xFFFFFFFFu

/* [3] backend id. */
#define RADIANT_DIAG_BACKEND_NULL    0u
#define RADIANT_DIAG_BACKEND_NRF     1u
#define RADIANT_DIAG_BACKEND_CC26XX  2u
#define RADIANT_DIAG_BACKEND_FAKE    0xFFu

/*
 * The backend table: pages 3 and 4 in this order, then page 7's first three
 * fields, then page 8's backend fields (0-3, then 7), then page 7's fields
 * 6-7, then page 9's fields 5-7, then page 10's fields 4-7. Every backend fills every index it can measure and leaves the rest
 * RADIANT_DIAG_NOT_MEASURED (radiant_radio_diag_get()'s caller pre-fills them
 * so).
 */
enum radiant_radio_diag_idx {
	/* page 3 */
	RADIANT_RADIO_DIAG_RX_POSTED = 0,  /* radiant_radio_rx() accepted */
	RADIANT_RADIO_DIAG_RX_REFUSED,     /* radiant_radio_rx() refused */
	RADIANT_RADIO_DIAG_TERM_OK,        /* RX windows that heard a frame */
	RADIANT_RADIO_DIAG_TERM_EMPTY,     /* ran to close, nothing heard */
	RADIANT_RADIO_DIAG_TERM_ERROR,     /* ended otherwise, nothing heard */
	RADIANT_RADIO_DIAG_STATUS_ERR,     /* radio command finished in error */
	RADIANT_RADIO_DIAG_STATUS_LAST,    /* the last such status, raw */
	RADIANT_RADIO_DIAG_SYNC_ENTRIES,   /* frames the hardware matched */
	/* page 4 */
	RADIANT_RADIO_DIAG_SW_ADDR_MISMATCH, /* matched in hardware, not in software */
	RADIANT_RADIO_DIAG_WINDOW_DROPS,   /* t_sync outside [t_open, t_close] */
	RADIANT_RADIO_DIAG_CRC_FAIL,       /* address matched, CRC did not */
	RADIANT_RADIO_DIAG_ALT_SYNC_HITS,  /* a decoy / unused sync word fired */
	RADIANT_RADIO_DIAG_RX_NOK,         /* hardware's own not-OK/ignored count */
	RADIANT_RADIO_DIAG_RX_BUF_FULL,    /* hardware ran out of receive buffer */
	RADIANT_RADIO_DIAG_DRAIN_MAX_US,   /* longest receive-callback drain */
	RADIANT_RADIO_DIAG_MIN_LEAD_US,    /* smallest (at - now) seen at an arm */
	/* page 7, fields 0-2 */
	RADIANT_RADIO_DIAG_SRCH_CUT_CLOSE, /* search frame in flight at the close */
	RADIANT_RADIO_DIAG_SRCH_CUT_ABORT, /* search frame in flight at an abort */
	RADIANT_RADIO_DIAG_SRCH_END_NEAR_CLOSE, /* search END near the close */
	/* page 8, fields 0-3 and 7 */
	RADIANT_RADIO_DIAG_DRAIN_MAX_FRAMES, /* frames in the longest drain */
	RADIANT_RADIO_DIAG_FRAME_MAX_US,   /* longest one-entry work, callback excluded */
	RADIANT_RADIO_DIAG_CRC_MAX_US,     /* longest software CRC */
	RADIANT_RADIO_DIAG_RX_CB_MAX_US,   /* longest synchronous rx callback */
	RADIANT_RADIO_DIAG_DRAIN_OVER_250US, /* drains over RADIANT_DIAG_DRAIN_SLOW_US */
	/* page 7, fields 6-7 */
	RADIANT_RADIO_DIAG_TX_START_MISSED, /* transmit start compare behind the counter */
	RADIANT_RADIO_DIAG_RX_START_MISSED, /* receive start compare behind the counter */
	/* page 9, fields 5-7 */
	RADIANT_RADIO_DIAG_PASS_TO_ARM_MAX_US, /* pass now -> backend now, worst */
	RADIANT_RADIO_DIAG_ARM_SLACK_MIN_US, /* i32 slack at the early check */
	RADIANT_RADIO_DIAG_EARLY_ETIME,    /* early-check ETIME refusals */
	/* page 10, fields 4-7 */
	RADIANT_RADIO_DIAG_LATE_ISR_DROPS, /* frames the late-ISR guard dropped */
	RADIANT_RADIO_DIAG_END_LATE_MAX_US, /* worst END-interrupt lateness */
	RADIANT_RADIO_DIAG_T_SYNC_MAX_US,  /* longest rx_t_sync() */
	RADIANT_RADIO_DIAG_MATCH_MAX_US,   /* longest rx_match() plus copies */
	RADIANT_RADIO_DIAG_COUNT
};

/* Where page 7's backend fields start in the table. */
#define RADIANT_RADIO_DIAG_HANDOFF_BASE RADIANT_RADIO_DIAG_SRCH_CUT_CLOSE

struct radiant_radio_diag {
	uint32_t v[RADIANT_RADIO_DIAG_COUNT];
};

#if defined(CONFIG_RADIANT_DIAG_COUNTERS)

/* Pages 1 and 2's own counters. Read through radiant_diag_core_get(). */
struct radiant_diag_core {
	/* page 1: a master's turnaround (receive window after its own slot) */
	uint32_t mrx_posted;      /* accepted by the scheduler */
	uint32_t mrx_refused;     /* refused by the scheduler */
	uint32_t mrx_skipped;     /* never asked for: no channel id or address */
	uint32_t turn_dropped;    /* requested, channel no longer TRACKING */
	uint32_t mrx_done_ok;     /* window ran */
	uint32_t mrx_done_missed; /* window never armed (MISSED) */
	uint32_t mrx_done_other;  /* DENIED, FAILED or ABORTED */
	uint32_t mrx_frames;      /* frames delivered in a MASTER_RX slot */
	/* page 2: a burst's next host block */
	uint32_t cont_submits;    /* continuation blocks submitted */
	uint32_t cont_late;       /* of those, past their ack + 1550 us instant */
	uint32_t turn_min_us;     /* ack t_sync -> submit, smallest */
	uint32_t turn_max_us;     /* ack t_sync -> submit, largest */
	uint32_t tx_expired;      /* transmit slots pass_step() reported MISSED */
	int32_t  tx_margin_min_us; /* smallest t_start - earliest seen there */
	uint8_t  turn_seen;       /* turn_min_us is valid */
	uint8_t  margin_seen;     /* tx_margin_min_us is valid */
	/* page 7, fields 3-5 (radiant_sched.c) */
	uint32_t trk_open_late;   /* bounded RX armed opening past its t_start */
	uint32_t trk_open_late_max_us; /* the largest such lateness */
	uint32_t trk_missed_behind_scan; /* bounded RX expired, scan chunk armed */
	/* page 8, fields 4-6 (radiant_sched.c pass()) */
	uint32_t pass_max_us;     /* longest pass, interrupts off */
	uint32_t pass_steps_max;  /* most pass_step() iterations in one pass */
	uint32_t pass_over_100us; /* passes over RADIANT_DIAG_PASS_SLOW_US */
	/* page 9, fields 0-4 (radiant_sched.c pass()), worst per-pass sums in
	 * A0 cycles: converted when the page is read, so a pass never divides */
	uint32_t pb_sweep_max_cyc;
	uint32_t pb_decide_max_cyc;
	uint32_t pb_arm_max_cyc;
	uint32_t pb_done_max_cyc;
	uint32_t pb_book_max_cyc;
	/* page 12, fields 0-4 (radiant_sched.c): stale events by site */
	uint32_t stale_rx_frame;
	uint32_t stale_rx_end;
	uint32_t stale_rx_filter;
	uint32_t stale_tx;
	uint32_t stale_ed;
};

extern struct radiant_diag_core radiant_diag_core;

const struct radiant_diag_core *radiant_diag_core_get(void);
void radiant_diag_core_reset(void);

#define RADIANT_DIAG_INC(field) ((void)radiant_diag_core.field++)
#define RADIANT_DIAG_MAX(field, us)                                           	do {                                                                  		uint32_t x__ = (uint32_t)(us);                                		if (x__ > radiant_diag_core.field) {                          			radiant_diag_core.field = x__;                        		}                                                             	} while (0)
#define RADIANT_DIAG_TURN(us)                                                 \
	do {                                                                  \
		uint32_t d__ = (uint32_t)(us);                                \
		if (!radiant_diag_core.turn_seen ||                           \
		    d__ < radiant_diag_core.turn_min_us) {                    \
			radiant_diag_core.turn_min_us = d__;                  \
		}                                                             \
		if (d__ > radiant_diag_core.turn_max_us) {                    \
			radiant_diag_core.turn_max_us = d__;                  \
		}                                                             \
		radiant_diag_core.turn_seen = 1u;                             \
	} while (0)
#define RADIANT_DIAG_TX_MARGIN(us)                                            \
	do {                                                                  \
		int32_t m__ = (int32_t)(us);                                  \
		if (!radiant_diag_core.margin_seen ||                         \
		    m__ < radiant_diag_core.tx_margin_min_us) {               \
			radiant_diag_core.tx_margin_min_us = m__;             \
		}                                                             \
		radiant_diag_core.margin_seen = 1u;                           \
	} while (0)

/*
 * Page 9 field 5's stamp: the low 32 bits of the `now` (radio-clock us) of
 * the scheduler pass whose arm call is in progress, or 0 when no pass is
 * arming. 32 bits so one store writes it: the nRF MPSL signal handler, a
 * zero-latency interrupt, can program an arm in the middle of a thread pass.
 * The scheduler sets it around its radiant_radio_rx/tx/ed calls; a backend
 * reads it at its early arm check and takes the unsigned 32-bit difference.
 * A pass whose `now` has low bits 0 (once in 71 minutes) is not sampled.
 */
extern uint32_t radiant_diag_pass_now;

/*
 * A0, the cycle clock: the core's own cycle counter (DWT CYCCNT on a Cortex-M
 * that has one), for spans too short for the radio clock. Every DWT access is
 * here, so no caller needs an architecture test.
 *
 * A backend's radiant_radio_init() calls radiant_diag_cycles_init() with its
 * core clock (a per-backend constant or SystemCoreClock - never
 * sys_clock_hw_cycles_per_sec(), which is the 32 kHz RTC on nRF). It turns the
 * counter on and checks that it advances across a short busy loop. Until then,
 * on a target without the counter, or if it did not move,
 * radiant_diag_cycles_to_us() returns RADIANT_DIAG_NOT_MEASURED for every span,
 * and that is what the page reports.
 *
 * CYCCNT stops while the core sleeps: time only spans that cannot idle
 * (interrupts off, or inside an ISR). It wraps every 67 s at 64 MHz, so a span
 * is the unsigned difference of two reads.
 */
#if defined(CONFIG_CPU_CORTEX_M_HAS_DWT)
/* DWT_CYCCNT, the architectural address on ARMv7-M and ARMv8-M Mainline. */
#define RADIANT_DIAG_DWT_CYCCNT_ADDR 0xE0001004u
static inline uint32_t radiant_diag_cycles(void)
{
	return *(volatile const uint32_t *)RADIANT_DIAG_DWT_CYCCNT_ADDR;
}
#else
/* No cycle counter (native_sim): a plain variable, 0 unless a unit test
 * advances it to drive the timed spans. radiant_diag_cycles_to_us() still
 * reads RADIANT_DIAG_NOT_MEASURED here, so a page never reports it. */
extern uint32_t radiant_diag_cycles_sim;
static inline uint32_t radiant_diag_cycles(void)
{
	return radiant_diag_cycles_sim;
}
#endif
void radiant_diag_cycles_init(uint32_t core_hz);
uint32_t radiant_diag_cycles_to_us(uint32_t cycles);

/*
 * Backend hook. Each backend built with CONFIG_RADIANT_DIAG_COUNTERS defines
 * it. The caller pre-fills every index with RADIANT_DIAG_NOT_MEASURED; the
 * backend overwrites what it measures. Returns the backend id.
 */
uint8_t radiant_radio_diag_get(struct radiant_radio_diag *out);

#else

#define RADIANT_DIAG_INC(field)    do { } while (0)
#define RADIANT_DIAG_MAX(field, us) do { } while (0)
#define RADIANT_DIAG_TURN(us)      do { } while (0)
#define RADIANT_DIAG_TX_MARGIN(us) do { } while (0)

#endif /* CONFIG_RADIANT_DIAG_COUNTERS */

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_DIAG_H_ */
