/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ant_sched.h - the radio schedule: which operation the radio performs next,
 * and at which absolute microsecond it is armed.
 *
 * Provenance: clean-room. Written from ant_core/include/ant_radio_hal.h (the
 * frozen backend contract), from the on-air and timing facts recorded in
 * docs/ant-radio-link.md, docs/spike-a-results.md and
 * docs/spike-b-part2-results.md, and from the free ANT Message Protocol and
 * Usage Rev 5.1 (D00000652). Nothing here derives from sdk-ant, from libant.a,
 * from disassembly of any binary, or from any adopter-gated ANT+ device profile
 * document. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this module is
 * ---------------------------------------------------------------------------
 * There is exactly one radio and it does exactly one thing at a time
 * (ant_radio_hal.h: "a second arm call while one is armed or running returns
 * ANT_RADIO_EBUSY"). Everything above this module - up to 32 channel state
 * machines, a shared wildcard search sweep, an acknowledged-data reply that is
 * owed inside 1.55 ms - wants the radio at once. This module is the single
 * place that decides who gets it, and it is the only place in ant_core that
 * calls ant_radio_tx() or ant_radio_rx().
 *
 * A caller does not arm anything. It posts an *intent* against a channel slot -
 * "hear this address between T0 and T1", "put these bytes on the air with
 * t_sync exactly T" - and gets told what happened. Intents are one-shot: an
 * accepted request is consumed by its terminal event, and the owner posts the
 * next one, usually from inside the completion callback, which is the
 * low-jitter path the HAL's callback contract exists to permit.
 *
 * POSTING IS NOT COMMITTING. ant_sched_request_rx() and ant_sched_request_tx()
 * change the plan and return; ant_sched_tick() acts on it. From thread context
 * a caller therefore posts everything it knows about and ticks once. That is
 * not ceremony, it is what makes merging possible at all: a scheduler that
 * armed on every post would commit the radio to the first channel's window
 * before the second channel - whose window overlaps it exactly - had said a
 * word, and the two would never share a window. From inside a callback there is
 * nothing to remember, because the scheduler runs the commit itself on the way
 * out.
 *
 * ---------------------------------------------------------------------------
 * RX-WINDOW MERGING, which is the reason this module is interesting
 * ---------------------------------------------------------------------------
 * All ANT+ traffic is on RF 57, network A6 C5 (docs/ant-radio-link.md). So two
 * tracked channels whose predicted receive windows overlap do not need two
 * hardware windows: one ant_radio_rx() carrying one filter per channel hears
 * both. The HAL was shaped to make that expressible - struct ant_rx_req takes
 * an array of filters and every event reports which one matched - and deciding
 * what to merge is this file's job and nothing else's.
 *
 * Two things follow, and they are the whole return on the module:
 *
 *   1. SLAVE-SIDE COLLISIONS BETWEEN OUR OWN TRACKED CHANNELS DROP TO ZERO. An
 *      unmerged scheduler must choose one of two overlapping windows and lose
 *      the other channel's packet outright. A merged window loses neither,
 *      because the radio was listening for both addresses at once.
 *   2. 32 TRACKED SENSORS DO NOT COST 32 WINDOWS. At 4 Hz with ~400 us windows
 *      the unmerged duty cycle is ~19 %; merged, it is a fraction of that, and
 *      the schedule has room left for the search sweep.
 *
 * NEVER HARDCODE EIGHT FILTERS. The number of addresses one window can carry is
 * caps.max_filters, and it is 8 on nRF (one base address plus eight prefixes)
 * and 2 on EFR32/RAIL (two runtime sync words). This module reads that field on
 * every scheduling pass and never assumes it. ANT_SCHED_MAX_FILTERS below is a
 * static-allocation ceiling, not a policy number: the policy number is always
 * caps.max_filters, clamped to it.
 *
 * ---------------------------------------------------------------------------
 * Sized for 32 channels from the first line
 * ---------------------------------------------------------------------------
 * 32 is the serial protocol's natural ceiling - the burst header carries the
 * channel number in its low five bits - and retrofitting a channel count
 * through a scheduler costs far more than starting at the ceiling. The slot
 * table is a fixed array with no allocation anywhere; see the size assertion in
 * ant_sched.c.
 *
 * ---------------------------------------------------------------------------
 * The five HAL facts this module is built around
 * ---------------------------------------------------------------------------
 *   - ANT_TIME_NEVER IS NEVER A VALID WINDOW EDGE. There is therefore no such
 *     thing as an open-ended receive, and background scan cannot be expressed
 *     as one. This module accepts ANT_TIME_NEVER as ant_sched_rx.t_close,
 *     meaning "until cancelled", and turns it into a chain of bounded chunks
 *     that it re-arms itself. That translation is the only place in ant_core
 *     that needs to know the restriction exists.
 *   - EVERY ARM RETURNS AN OP ID, AND A CANCELLED OPERATION'S TERMINAL EVENT
 *     STILL ARRIVES. So this module retires an operation *before* aborting it,
 *     and every event whose op id is not the live one is counted and dropped.
 *     That is not defensive programming against a hypothetical: it happens on
 *     every preemption, with no fault injection.
 *   - THE OPERATION SLOT FREES BEFORE THE TERMINAL CALLBACK RUNS, so the next
 *     window is armed from inside the callback.
 *   - CALLBACKS RUN IN RADIO ISR CONTEXT. A scheduling pass is pure arithmetic
 *     over the slot table plus at most one ant_radio_now() and one arm call. A
 *     pass runs after every terminal event, and after a non-terminal one only
 *     if a callback actually posted or cancelled something.
 *   - RADIO CONFIGURATION IS PER-OPERATION. Every request carries its own
 *     struct ant_pkt_format, and two requests merge only if they name the same
 *     format and the same RF index. That single test is also what keeps a
 *     search window out of a tracking window, and what will keep the planned
 *     long-range and adaptive-frequency channels out of the RF 57 merge without
 *     another line of policy.
 *
 * ---------------------------------------------------------------------------
 * What this module does NOT know
 * ---------------------------------------------------------------------------
 * Nothing about ANT. No channel period, no device number, no message type, no
 * frame layout, no drift estimate. It moves absolute microseconds and opaque
 * filters around. Predicting the next window's centre is ant_channel.c's job;
 * choosing the sweep's addresses is ant_search.c's; deciding a reply is owed is
 * ant_ack.c's. If a constant from docs/ant-radio-link.md appears in
 * ant_sched.c, something has been put in the wrong file - the two exceptions
 * are named at their definitions below and both are bounds, not predictions.
 */

#ifndef ANT_SCHED_H_
#define ANT_SCHED_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ant_radio_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Sizes
 * ---------------------------------------------------------------------------
 */

/*
 * Channel slots. 32 because the serial protocol's burst header carries the
 * channel number in its low five bits, so 32 is the ceiling a host can address
 * at all, and because sizing a scheduler for 8 and widening it later is the
 * expensive order to do it in.
 */
#define ANT_SCHED_MAX_CHANNELS 32u

/*
 * Static ceiling on filters in one hardware window. This is an allocation
 * bound - the largest caps.max_filters any planned backend advertises - and it
 * is NOT the policy number. Every decision in ant_sched.c reads
 * caps.max_filters and clamps it to this; a build for a part with more filters
 * raises this constant and changes nothing else.
 */
#define ANT_SCHED_MAX_FILTERS 8u

/* "No channel." Returned by queries and accepted nowhere. */
#define ANT_SCHED_CH_NONE 0xFFu

/*
 * How long a background-scan chunk runs before the scheduler re-arms it.
 *
 * One ANT channel period. docs/ant-radio-link.md argues the optimum for a
 * wildcard sweep dwell: a shorter dwell does not raise the per-transmission
 * acquisition probability, while a dwell of one full period guarantees a
 * transmitting sensor is heard at least once while its address set is
 * selected. It is a bound on how long the radio may be committed to the scan,
 * not a prediction about any master's behaviour, which is why a link-layer
 * number is allowed to appear in a scheduler at all. ant_search.c may override
 * it per request.
 */
#define ANT_SCHED_SCAN_CHUNK_US 250000u

/*
 * The shortest gap worth filling with a scan chunk. Below this the arm and
 * teardown cost more than the listening is worth, and the scheduler would
 * thrash on every inter-window gap.
 */
#define ANT_SCHED_SCAN_MIN_US 1000u

/*
 * The widest a merged receive window may grow.
 *
 * Merging takes the union of overlapping windows, and a union is only bounded
 * if something bounds it. This is the bound, and the number comes from the
 * bench: a master's broadcast is answered by its slave 2186-2191 us later
 * (docs/spike-b-part2-results.md), and that reply is the tightest deadline in
 * the link layer. A merged window that could outlive that interval could block
 * a reply the link layer owes, which is a worse failure than declining to merge
 * one more channel. Windows are hundreds of microseconds wide - a real master
 * holds its slot to well inside +/-25 us over minutes, so the guards are about
 * our own drift and clock error - so this cap is generous in practice and is
 * only ever reached by a caller asking for something unusual.
 */
#define ANT_SCHED_MERGE_SPAN_MAX_US 2000u

/* ---------------------------------------------------------------------------
 * Requests
 * ---------------------------------------------------------------------------
 */

/*
 * One receive intent.
 *
 * TIMES ARE IN t_sync TERMS, exactly as struct ant_rx_req is: the window must
 * catch any frame whose last address bit reaches the antenna between t_open and
 * t_close. Do not add ramp-up or preamble airtime here - the backend already
 * did, and adding it twice is the silent yield loss the t_sync contract in
 * ant_radio_hal.h has a page of prose about.
 *
 * FILTERS ARE CALLER-OWNED and must stay valid until done() fires, on the same
 * terms as struct ant_rx_req.filters. A tracked channel passes one; the search
 * sweep passes as many as caps.max_filters allows. The scheduler copies them
 * into the contiguous array it hands the HAL, so a merged window's filters need
 * not be adjacent in anyone's memory.
 */
struct ant_sched_rx {
	/* Per-operation radio configuration. Two requests merge only if this
	 * pointer and rf_index are identical, so the small set of static const
	 * formats ant_frame_format() returns is exactly the right thing to
	 * pass: pointer equality is the merge group. */
	const struct ant_pkt_format *fmt;
	uint8_t                      rf_index;

	const struct ant_rx_filter *filters;
	uint8_t                     n_filters; /* 1..ANT_SCHED_MAX_FILTERS */

	ant_time_t t_open;
	/*
	 * t_close, or ANT_TIME_NEVER for "until cancelled".
	 *
	 * ANT_TIME_NEVER is the background-scan form and it is the one thing
	 * this module accepts that the HAL refuses. Such a request is never
	 * consumed by its terminal event: the scheduler arms bounded chunks of
	 * at most chunk_us, calls done() with ANT_SCHED_DONE_OK at the end of
	 * each one - which is the per-dwell hook a sweep needs - and re-arms
	 * unless the callback cancelled or replaced it. It always yields to a
	 * bounded request, and it fills the gaps between them.
	 */
	ant_time_t t_close;

	/* Chunk length for a continuous request. 0 means
	 * ANT_SCHED_SCAN_CHUNK_US. Ignored for a bounded one. */
	uint32_t chunk_us;

	/* Deliver ANT_RADIO_STATUS_CRC_FAIL events for this channel too. The
	 * flag is per-window in the HAL, so asking for it merges it in for
	 * every channel sharing the window; the scheduler still routes each
	 * event to the one channel whose filter matched. */
	bool report_crc_fail;

	/*
	 * Close the window as soon as one good frame is accepted, to save
	 * receive current on a single-master window.
	 *
	 * HONOURED ONLY IF THE WINDOW IS NOT MERGED. The HAL forbids
	 * ANT_RX_STOP_ON_FIRST on a window carrying more than one filter, and
	 * for a good reason: more than one master may transmit inside it. So
	 * this is a request, not a guarantee, and a channel must behave
	 * correctly either way.
	 */
	bool stop_on_first;
};

/*
 * One transmit intent.
 *
 * t_sync_at is the instant the last bit of the on-air address must be at the
 * antenna, and it is not negotiable: the scheduler either arms it to hit that
 * instant exactly or reports ANT_SCHED_DONE_MISSED and transmits nothing. A
 * late master frame lands in the next slot and is worse than no frame, and a
 * late acknowledgement is worse still.
 *
 * body is caller-owned, must be DMA-reachable RAM, and must stay unmodified
 * until done() or tx() fires.
 */
struct ant_sched_tx {
	const struct ant_pkt_format *fmt;
	uint8_t                      rf_index;
	struct ant_tx_power          power;
	const uint8_t               *body;
	uint8_t                      body_len;
	ant_time_t                   t_sync_at;
};

/* ---------------------------------------------------------------------------
 * Completion
 * ---------------------------------------------------------------------------
 */

/*
 * Why a request ended. Exactly one done() fires for every request the
 * scheduler accepted, except one deliberately: ant_sched_cancel() is silent for
 * the channel it cancels, because the caller already knows.
 */
enum ant_sched_done {
	/* The operation ran to its natural end: the window closed, or the frame
	 * went out. For a continuous request this fires at every chunk boundary
	 * and the request stays live. */
	ANT_SCHED_DONE_OK = 0,
	/* It ended early - preempted by a nearer deadline, or the radio was
	 * disabled underneath it. The owner should re-predict and re-post
	 * rather than assume the window ran. */
	ANT_SCHED_DONE_ABORTED,
	/*
	 * It was never armed, because its deadline passed while the radio was
	 * busy with something else, or because it was already unreachable
	 * inside caps.min_arm_lead_us when it was posted.
	 *
	 * This is the honest report of contention and it is worth counting
	 * rather than logging: a channel seeing it repeatedly is a channel
	 * whose window is being lost to another, which is precisely the
	 * condition merging exists to remove.
	 */
	ANT_SCHED_DONE_MISSED,
	/* The backend refused the operation or could not complete it. A
	 * malformed request lands here rather than looping. */
	ANT_SCHED_DONE_FAILED
};

/*
 * Every one of these runs in radio interrupt context, under the whole of
 * ant_radio_hal.h's callback contract: do not block, do not retain evt->body,
 * do not call the radio lifecycle functions, and do not do work proportional to
 * anything.
 *
 * Posting the next request from inside one is not merely allowed, it is the
 * intended path: the scheduler runs one arming pass on the way out, so a window
 * posted here is armed with no thread wakeup in between.
 */
struct ant_sched_cbs {
	/*
	 * One received frame, already routed to the channel whose filter
	 * matched.
	 *
	 * filter_index is the index into that channel's OWN filters[] array,
	 * not into the merged window's - so a search sweep recovers devnum_lo
	 * from it exactly as if it had the radio to itself, and a tracked
	 * channel with one filter always sees 0.
	 */
	void (*rx)(uint8_t ch, uint8_t filter_index,
		   const struct ant_rx_event *evt, void *user);

	/* The transmit completed. evt->t_sync is what actually went out, which
	 * is what a master closes its slot-phase loop on. */
	void (*tx)(uint8_t ch, const struct ant_tx_event *evt, void *user);

	/* The request is finished with. See enum ant_sched_done. */
	void (*done)(uint8_t ch, enum ant_sched_done why, void *user);
};

/* ---------------------------------------------------------------------------
 * Counters
 *
 * Not decoration. windows_armed against channels_windowed is the merge factor,
 * and it is the one number that says whether the highest-value item in the
 * module is doing anything; missed and preempted are what contention looks like
 * before it looks like packet loss.
 * ---------------------------------------------------------------------------
 */
struct ant_sched_stats {
	uint32_t windows_armed;     /* accepted ant_radio_rx() calls */
	uint32_t channels_windowed; /* summed over those windows */
	uint32_t windows_merged;    /* of those, the ones carrying >1 channel */
	uint32_t filters_armed;     /* summed hardware filters used */
	uint32_t tx_armed;          /* accepted ant_radio_tx() calls */
	uint32_t scan_chunks;       /* continuous-request chunks armed */
	uint32_t missed;            /* requests reported ANT_SCHED_DONE_MISSED */
	uint32_t preempted;         /* running operations cut short for a nearer
				     * deadline; their members lost the rest of
				     * their window and were told so */
	uint32_t replans;           /* windows rebuilt before they opened, which
				     * costs nothing and loses nobody - the
				     * mechanism by which a channel that opened
				     * late still joins the merge */
	uint32_t stale_events;      /* events for an operation already retired */
	uint32_t arm_ebusy;         /* arm calls that lost the backend's race */
	uint32_t arm_rejected;      /* arm calls refused for any other reason */
};

/* ---------------------------------------------------------------------------
 * API
 *
 * Return codes are ant_radio_hal.h's, deliberately rather than a third code
 * space: almost every failure here either is a HAL failure or means exactly
 * what the HAL's name for it means.
 * ---------------------------------------------------------------------------
 */

/*
 * The radio callbacks the scheduler needs installed. Pass this to
 * ant_radio_init():
 *
 *     ant_sched_init(&my_cbs, ctx);
 *     ant_radio_init(ant_sched_radio_cbs(), NULL);
 *     ant_radio_enable();
 *
 * The scheduler owns radio *events* because it is the only thing that arms; it
 * deliberately does not own the radio *lifecycle*, which belongs to ant_api.c
 * and to whatever decides the dongle should be powered.
 */
const struct ant_radio_cbs *ant_sched_radio_cbs(void);

/*
 * Clear the slot table and latch the capability pointer. cbs may be NULL for a
 * test that only asserts on counters; it is copied, and user is passed back
 * unchanged.
 *
 * The capabilities POINTER is cached, not its contents: the HAL says the
 * returned pointer is static for the lifetime of the program, so every pass
 * re-reads max_filters and min_arm_lead_us through it and a suite that swaps
 * presets between operations is seen immediately.
 */
int ant_sched_init(const struct ant_sched_cbs *cbs, void *user);

/*
 * Drop every request without firing done(), aborting anything in flight. For
 * teardown; ant_sched_cancel() is what a running system uses.
 *
 * CALL THIS BEFORE ant_radio_disable(), not after. Disabling aborts the live
 * operation and delivers its terminal event, and this module's response to a
 * terminal event is to arm the next thing - from inside the callback, which is
 * still inside the disable call and still in the enabled state. The radio would
 * then be powered down with an operation armed on it.
 */
void ant_sched_reset(void);

/*
 * Post a request against a channel slot, replacing whatever that slot held.
 * Replacing an in-flight request aborts it, silently for that channel.
 *
 * NOTHING IS ARMED HERE. Post as many as are known and then call
 * ant_sched_tick() once; from inside a callback, do not call tick at all,
 * because the scheduler commits on the way out.
 *
 * ANT_RADIO_EINVAL for a bad channel, a null format or filter array, an
 * n_filters outside 1..ANT_SCHED_MAX_FILTERS, or t_close before t_open.
 * ANT_RADIO_ESTATE before ant_sched_init(). Note that a request wanting more
 * filters than the backend has is accepted here and reported through
 * done(ANT_SCHED_DONE_FAILED), not rejected: how many filters exist is a
 * runtime property and a caller that had to check it before every post would be
 * duplicating this module's only interesting decision.
 */
int ant_sched_request_rx(uint8_t ch, const struct ant_sched_rx *req);
int ant_sched_request_tx(uint8_t ch, const struct ant_sched_tx *req);

/*
 * Drop this channel's request, aborting it if it is in flight.
 *
 * The abort is immediate, unlike a post - the point of cancelling is that the
 * radio stops now - but what the radio does *instead* is still decided at the
 * next commit. done() does not fire for ch; other channels that were sharing an
 * aborted merged window do get ANT_SCHED_DONE_ABORTED, because their operation
 * really did end early, and they should re-predict rather than assume their
 * window ran.
 */
int ant_sched_cancel(uint8_t ch);

/* True if the slot holds a request, in flight or not. */
bool ant_sched_pending(uint8_t ch);

/*
 * Commit: look at the plan and the clock, and arm.
 *
 * This is the thread-context counterpart of the pass the scheduler runs at the
 * end of every radio callback, and calling it is how anything posted from
 * thread context reaches the radio at all. It is also the only recovery path
 * from the two arm failures that leave a request pending with nothing scheduled
 * to retry it - ANT_RADIO_EBUSY, when an arm raced the backend's callback, and
 * ANT_RADIO_ESTATE, when the radio was not enabled yet. This module owns no
 * timer: it arms for absolute instants and lets the backend hold them, so
 * nothing else will come along and try again.
 */
int ant_sched_tick(void);

const struct ant_sched_stats *ant_sched_stats_get(void);
void                          ant_sched_stats_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ANT_SCHED_H_ */
