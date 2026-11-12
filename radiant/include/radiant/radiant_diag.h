/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_diag.h - the diagnostic counters behind MESG_RADIANT_DIAG (0xF7).
 *
 * Provenance: clean-room. Written from this project's own source and bench
 * measurements; see docs/decisions/0023-radiant-diag-message.md.
 *
 * WHAT THIS IS. Eight fixed pages of eight u32 counters each, read by a host
 * with MESG_REQUEST [page, 0xF7]. Pages 0, 5 and 6 are counters the core
 * already keeps (radiant_api_stats, radiant_search_stats). Pages 1 and 2 are
 * counted here, behind CONFIG_RADIANT_DIAG_COUNTERS. Pages 3 and 4 come from
 * the backend through radiant_radio_diag_get(), one generic table that every
 * backend fills with what it can measure. Page 7 (ADR 0022 gate 3) is mixed:
 * its first three fields come from that same backend table, the other five
 * are counted here.
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
#define RADIANT_DIAG_PAGES           8u

/*
 * Page 7, the search/track hand-off (ADR 0022 gate 3). Does a search
 * reception running up to a scan chunk's close delay the tracked window armed
 * after it? Fields 0-2 are the backend's (a search frame in flight when the
 * chunk closed or was aborted; a search frame ending close to the chunk's
 * close), 3-7 the core's (bounded receive windows opened late, the worst
 * lateness, bounded windows expired while a scan chunk held the radio, and
 * tracked slots that opened just after the search heard the same device
 * number, with how many of those heard nothing).
 */
#define RADIANT_DIAG_HANDOFF_SRCH_CUT_CLOSE        0u
#define RADIANT_DIAG_HANDOFF_SRCH_CUT_ABORT        1u
#define RADIANT_DIAG_HANDOFF_SRCH_END_NEAR_CLOSE   2u
#define RADIANT_DIAG_HANDOFF_TRK_OPEN_LATE         3u
#define RADIANT_DIAG_HANDOFF_TRK_OPEN_LATE_MAX_US  4u
#define RADIANT_DIAG_HANDOFF_TRK_MISSED_BEHIND_SCAN 5u
#define RADIANT_DIAG_HANDOFF_TRK_SLOTS_AFTER_SIBLING 6u
#define RADIANT_DIAG_HANDOFF_TRK_MISS_AFTER_SIBLING 7u

/* A search frame ending this close to its chunk's hardware close counts as
 * "near close" (field 2): its END interrupt chain then runs into the time
 * the next arm needs. */
#define RADIANT_DIAG_SRCH_NEAR_CLOSE_US 300u
/* A tracked slot opening within this long after the search heard the same
 * device number counts as "after a sibling" (field 6): the trainer's other
 * channels share its device number. */
#define RADIANT_DIAG_SIBLING_US 2000u

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
 * fields. Every backend fills every index it can measure and leaves the rest
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
	/* page 7, fields 3-7 (radiant_sched.c and the API) */
	uint32_t trk_open_late;   /* bounded RX armed opening past its t_start */
	uint32_t trk_open_late_max_us; /* the largest such lateness */
	uint32_t trk_missed_behind_scan; /* bounded RX expired, scan chunk armed */
	uint32_t trk_slots_after_sibling; /* tracked slot just after the search
					   * heard its device number */
	uint32_t trk_miss_after_sibling;  /* of those, slots that heard nothing */
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
