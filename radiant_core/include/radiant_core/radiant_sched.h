/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sched.h - the radio schedule: which operation the radio performs
 * next, and at which absolute microsecond it is armed.
 *
 * Provenance: clean-room, from radiant_radio_hal.h, docs/ant-radio-link.md,
 * docs/spike-a-results.md, docs/spike-b-part2-results.md, and the free ANT
 * Message Protocol and Usage Rev 5.1 (D00000652). Nothing here derives from
 * sdk-ant, libant.a, or any ANT+ device profile document. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * There is exactly one radio (radiant_radio_hal.h: a second arm call while
 * one is running returns EBUSY), and everything above this module - up to 32
 * channel state machines, a shared search sweep, a 1.55 ms acknowledged-data
 * reply - wants it at once. This is the only place in radiant_core that calls
 * radiant_radio_tx()/rx(); a caller posts an *intent* against a channel slot
 * and gets told what happened. Intents are one-shot, consumed by their
 * terminal event; the owner usually posts the next one from inside the
 * completion callback (the HAL's low-jitter path).
 *
 * POSTING IS NOT COMMITTING: radiant_sched_request_rx/tx() change the plan and
 * return, radiant_sched_tick() acts on it. This is what makes merging
 * possible - a scheduler that armed on every post would commit to the first
 * channel's window before an overlapping second channel had posted, and the
 * two would never share a window. From inside a callback there's nothing to
 * remember; the scheduler commits on the way out.
 *
 * ---------------------------------------------------------------------------
 * RX-window merging - the reason this module exists
 * ---------------------------------------------------------------------------
 * All ANT+ traffic is on RF 57, network A6 C5. Two tracked channels whose
 * predicted windows overlap share one radiant_radio_rx() carrying one filter
 * per channel (struct radiant_rx_req takes an array of filters and reports
 * which matched); deciding what to merge is this file's job alone.
 *
 *   1. Slave-side collisions between our own tracked channels drop to zero -
 *      an unmerged scheduler must choose one of two overlapping windows and
 *      lose the other's packet outright.
 *   2. 32 tracked sensors do not cost 32 windows: unmerged duty at 4 Hz/
 *      ~400 us windows is ~19%; merged, it's a fraction of that.
 *
 * NEVER HARDCODE EIGHT FILTERS: the addresses one window can carry is
 * caps.max_filters (8 on nRF, 2 on EFR32/RAIL), read on every pass.
 * RADIANT_SCHED_MAX_FILTERS is a static-allocation ceiling only; the policy
 * number is always caps.max_filters clamped to it.
 *
 * ---------------------------------------------------------------------------
 * Sized for 32 channels from the first line
 * ---------------------------------------------------------------------------
 * 32 is the serial protocol's ceiling (the burst header's low five bits). The
 * slot table is a fixed array with no allocation; see the size assertion in
 * radiant_sched.c.
 *
 * ---------------------------------------------------------------------------
 * The five HAL facts this module is built around
 * ---------------------------------------------------------------------------
 *   - RADIANT_TIME_NEVER is never a valid window edge, so there's no
 *     open-ended receive. This module accepts it as radiant_sched_rx.t_close
 *     ("until cancelled") and turns it into a chain of bounded, self-re-armed
 *     chunks - the only place in radiant_core that needs to know.
 *   - Every arm returns an op id, and a cancelled operation's terminal event
 *     still arrives. This module retires an operation *before* aborting it,
 *     and drops any event whose op id isn't current - happens on every
 *     preemption, with no fault injection needed.
 *   - The operation slot frees before the terminal callback runs, so the next
 *     window can be armed from inside the callback.
 *   - Callbacks run in radio ISR context. A scheduling pass is pure
 *     arithmetic plus at most one radiant_radio_now() and one arm call, and
 *     runs after every terminal event (and after a non-terminal one only if
 *     something was posted or cancelled).
 *   - Radio configuration is per-operation: two requests merge only if they
 *     name the same struct radiant_pkt_format and RF index. That single test
 *     also keeps search out of tracking windows and will keep future
 *     long-range/adaptive-frequency channels out of the RF 57 merge for free.
 *
 * ---------------------------------------------------------------------------
 * What this module does NOT know
 * ---------------------------------------------------------------------------
 * Nothing about ANT: no channel period, device number, message type, frame
 * layout, drift estimate - just absolute microseconds and opaque filters.
 * Predicting a window's centre is radiant_channel.c's job; choosing sweep
 * addresses is radiant_search.c's; deciding a reply is owed is radiant_ack.c's.
 * A docs/ant-radio-link.md constant appearing in radiant_sched.c means
 * something is in the wrong file (the two exceptions are bounds, not
 * predictions, named at their definitions below).
 */

#ifndef RADIANT_SCHED_H_
#define RADIANT_SCHED_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant_core/radiant_radio_hal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Sizes
 * ---------------------------------------------------------------------------
 */

/* Channel slots. 32 is the serial protocol's ceiling (burst header's low five
 * bits), and sizing for it now is cheaper than widening later. */
#define RADIANT_SCHED_MAX_CHANNELS 32u

/*
 * Static ceiling on filters in one hardware window - an allocation bound (the
 * largest caps.max_filters any planned backend advertises), NOT the policy
 * number. Every decision reads caps.max_filters and clamps to this.
 */
#define RADIANT_SCHED_MAX_FILTERS 8u

/* "No channel." Returned by queries and accepted nowhere. */
#define RADIANT_SCHED_CH_NONE 0xFFu

/*
 * How long a background-scan chunk runs before re-arming: one ANT channel
 * period. A shorter dwell doesn't raise per-transmission acquisition
 * probability, while a full period guarantees a transmitting sensor is heard
 * at least once while its address set is selected. A bound on radio
 * commitment, not a prediction - which is why it's allowed in a scheduler at
 * all. radiant_search.c may override it per request.
 */
#define RADIANT_SCHED_SCAN_CHUNK_US 250000u

/*
 * The shortest gap worth filling with a scan chunk. Below this the arm and
 * teardown cost more than the listening is worth, and the scheduler would
 * thrash on every inter-window gap.
 */
#define RADIANT_SCHED_SCAN_MIN_US 1000u

/*
 * The widest a merged receive window may grow. Merging takes the union of
 * overlapping windows, which needs a bound: this one, from the bench - a
 * master's broadcast is answered 2186-2191 us later
 * (docs/spike-b-part2-results.md), the tightest deadline in the link layer.
 * Real masters hold slot phase to well inside +/-25 us, so this cap is
 * generous and rarely reached.
 *
 * IT BOUNDS OCCUPANCY, NOT REPLY LATENCY. The reply deadline is actually
 * protected by want_preempt() in radiant_sched.c, which tears down an armed
 * merged window the instant a channel's own transmit falls due, regardless of
 * window width; this cap only stops a merge from growing unbounded before
 * anything needs to preempt it.
 */
#define RADIANT_SCHED_MERGE_SPAN_MAX_US 2000u

/*
 * Energy detect: modelled on the continuous background scan, not the bounded
 * window path - never ends, armed one gap-sized chunk at a time. Unlike the
 * scan it has the lowest priority: the scan, every tracked window, and every
 * transmit all outrank it (see "SLOT_ED" in radiant_sched.c).
 *
 * DWELL_US (400 us): one index's ceiling, sized from ~40 us ramp-up plus a
 * bounded RSSI sample burst. A ceiling, not a duration - a backend that's
 * taken every sample leaves early, so a full 0..124 sweep costs less than
 * 125 * 400 us.
 *
 * CHUNK_US (10 ms, 25 indices): bounds one arm call, deliberately far below
 * the ~245 ms gap a 4 Hz tracked channel leaves, so a newly-posted window
 * only has to preempt a short chunk rather than wait out a long one.
 *
 * MIN_US (2 ms, 5 indices): shortest gap worth an ED chunk - below it the arm
 * costs more than the measurement, same reasoning as SCAN_MIN_US.
 */
#define RADIANT_SCHED_ED_DWELL_US 400u
#define RADIANT_SCHED_ED_CHUNK_US 10000u
#define RADIANT_SCHED_ED_MIN_US   2000u

/* ---------------------------------------------------------------------------
 * Requests
 * ---------------------------------------------------------------------------
 */

/*
 * One receive intent. Times are in t_sync terms, exactly as
 * struct radiant_rx_req: don't add ramp-up/preamble airtime here, the backend
 * already does (see the t_sync contract in radiant_radio_hal.h).
 *
 * Filters are caller-owned and must stay valid until done() fires, same as
 * struct radiant_rx_req.filters. The scheduler copies them into the
 * contiguous array it hands the HAL, so a merged window's filters need not be
 * adjacent in anyone's memory.
 */
struct radiant_sched_rx {
	/* Per-operation radio configuration. Two requests merge only if this
	 * pointer and rf_index are identical, so the small set of static const
	 * formats radiant_frame_format() returns is exactly the right thing to
	 * pass: pointer equality is the merge group. */
	const struct radiant_pkt_format *fmt;
	uint8_t                      rf_index;

	const struct radiant_rx_filter *filters;
	uint8_t                     n_filters; /* 1..RADIANT_SCHED_MAX_FILTERS */

	radiant_time_t t_open;
	/*
	 * t_close, or RADIANT_TIME_NEVER for "until cancelled" - the
	 * background-scan form, the one thing this module accepts that the HAL
	 * refuses. Such a request is never consumed by its terminal event: the
	 * scheduler arms bounded chunks of at most chunk_us, calls done() with
	 * RADIANT_SCHED_DONE_OK at each chunk's end, and re-arms unless
	 * cancelled or replaced. Always yields to a bounded request and fills
	 * the gaps between them.
	 */
	radiant_time_t t_close;

	/* Chunk length for a continuous request. 0 means
	 * RADIANT_SCHED_SCAN_CHUNK_US. Ignored for a bounded one. */
	uint32_t chunk_us;

	/* Deliver RADIANT_RADIO_STATUS_CRC_FAIL events for this channel too. The
	 * flag is per-window in the HAL, so asking for it merges it in for
	 * every channel sharing the window; the scheduler still routes each
	 * event to the one channel whose filter matched. */
	bool report_crc_fail;

	/*
	 * Close the window as soon as one good frame is accepted, to save
	 * receive current on a single-master window. Honoured only if the
	 * window is not merged - the HAL forbids this flag on a
	 * multi-filter window since more than one master may transmit inside
	 * it - so this is a request, not a guarantee.
	 */
	bool stop_on_first;

	/*
	 * Air time to reserve for a follow-on transmit after this window
	 * closes, in microseconds. 0 = none. Passed through verbatim to
	 * struct radiant_rx_req::follow_on_us. Not inferred here: whether a
	 * tracked frame becomes an acknowledged-data reply is an ANT-semantics
	 * fact this module has no access to; the caller that knows sets it.
	 */
	uint16_t follow_on_us;
};

/*
 * One transmit intent. t_sync_at is not negotiable: the scheduler either
 * hits it exactly or reports RADIANT_SCHED_DONE_MISSED and transmits nothing
 * - a late master frame lands in the next slot and is worse than none.
 *
 * body is caller-owned, must be DMA-reachable RAM, and must stay unmodified
 * until done() or tx() fires.
 */
struct radiant_sched_tx {
	const struct radiant_pkt_format *fmt;
	uint8_t                      rf_index;
	struct radiant_tx_power          power;
	/* The on-air address to emit, first byte first. Copied into the slot at
	 * the post rather than held by pointer, so it does not join body in the
	 * "must stay valid until done()" contract - see struct radiant_tx_req. */
	uint8_t                      addr[RADIANT_RADIO_ADDR_MAX];
	uint8_t                      addr_len;
	const uint8_t               *body;
	uint8_t                      body_len;
	radiant_time_t                   t_sync_at;
	/* As struct radiant_sched_rx::follow_on_us, measured from t_sync_at.
	 * Non-zero on a listening master's own slot, whose turnaround window is
	 * armed 2.19 ms later from inside the transmit's own completion. */
	uint16_t                     follow_on_us;
};

/*
 * One energy-detect intent. Deliberately has no time field, unlike every
 * other request: it wants whatever is left over, ready the moment it's
 * posted or a chunk ends, never late for anything.
 *
 * Continuous like radiant_sched_rx's RADIANT_TIME_NEVER form: not consumed by
 * its terminal event, done() fires RADIANT_SCHED_DONE_OK per chunk, stays
 * live until radiant_sched_cancel(). Sweeps rf_index_lo..rf_index_hi
 * ascending, wrapping to lo, resuming from wherever a chunk was cut off.
 */
struct radiant_sched_ed {
	uint8_t rf_index_lo;
	uint8_t rf_index_hi;   /* >= rf_index_lo */
	/* Per-index ceiling. 0 means RADIANT_SCHED_ED_DWELL_US. */
	uint32_t dwell_us;
	/* Ceiling on one chunk. 0 means RADIANT_SCHED_ED_CHUNK_US. */
	uint32_t chunk_us;
};

/* ---------------------------------------------------------------------------
 * Completion
 * ---------------------------------------------------------------------------
 */

/*
 * Why a request ended. Exactly one done() fires for every request the
 * scheduler accepted, except one deliberately: radiant_sched_cancel() is silent for
 * the channel it cancels, because the caller already knows.
 */
enum radiant_sched_done {
	/* The operation ran to its natural end: the window closed, or the frame
	 * went out. For a continuous request this fires at every chunk boundary
	 * and the request stays live. */
	RADIANT_SCHED_DONE_OK = 0,
	/*
	 * It ended early - preempted by a nearer deadline, or the radio was
	 * disabled underneath it. The owner should re-predict rather than
	 * assume the window ran.
	 *
	 * A continuous request stays live for this too (scan pauses and resumes
	 * in the next gap), and it's reported rather than swallowed: an owner
	 * that credited a displaced chunk as if it ran would silently break its
	 * own listening-time accounting - this is what once froze the wildcard
	 * sweep on a single address set.
	 */
	RADIANT_SCHED_DONE_ABORTED,
	/*
	 * Never armed: its deadline passed while the radio was busy, or it was
	 * already unreachable inside caps.min_arm_lead_us when posted. Worth
	 * counting: a channel seeing this repeatedly is losing its window to
	 * another, exactly the condition merging exists to remove.
	 */
	RADIANT_SCHED_DONE_MISSED,
	/* The backend refused the operation or could not complete it. A
	 * malformed request lands here rather than looping. */
	RADIANT_SCHED_DONE_FAILED,
	/*
	 * The radio was lent to something else: an arbitrated backend didn't
	 * give us the air, either refusing synchronously (RADIANT_RADIO_EDENIED)
	 * or accepting and never granting (RADIANT_RADIO_STATUS_DENIED).
	 *
	 * NOT a miss: MISSED means the window ran (or would have) and the peer
	 * wasn't heard - eight of those and radiant_channel.c drops to
	 * SEARCHING. A denial is evidence about US, not the peer; nothing was
	 * learnt. Folding the two together would drop sensors whenever the
	 * other stack is busy - exactly what this mechanism exists to prevent.
	 *
	 * Not free either: the slot clock still advanced with no fresh sync, so
	 * radiant_channel.c widens its guard by a second counter of equal
	 * weight and promotes to the miss path once the guard can no longer
	 * cover it.
	 *
	 * Like DONE_OK/DONE_ABORTED, does not consume a continuous request - a
	 * denied scan/ED chunk still wants the next one.
	 */
	RADIANT_SCHED_DONE_DENIED
};

/*
 * Every one of these runs in radio interrupt context, under
 * radiant_radio_hal.h's whole callback contract (no blocking, no retaining
 * evt->body, no lifecycle calls, no proportional work). Posting the next
 * request from inside one is the intended path: the scheduler arms it on the
 * way out with no thread wakeup in between.
 */
struct radiant_sched_cbs {
	/*
	 * One received frame, already routed to the channel whose filter
	 * matched. filter_index indexes that channel's OWN filters[] array,
	 * not the merged window's, so a search sweep recovers devnum_lo as if
	 * it had the radio to itself, and a tracked channel with one filter
	 * always sees 0.
	 */
	void (*rx)(uint8_t ch, uint8_t filter_index,
		   const struct radiant_rx_event *evt, void *user);

	/* The transmit completed. evt->t_sync is what actually went out, which
	 * is what a master closes its slot-phase loop on. */
	void (*tx)(uint8_t ch, const struct radiant_tx_event *evt, void *user);

	/*
	 * One energy-detect dwell finished, for the channel that posted the
	 * ED request. Fires only for OK events - the terminal one reaches
	 * done() like every other operation's.
	 *
	 * Optional: nothing in the core needs it, since the scheduler has
	 * already fed the measurement to radiant_chanmap.c by the time this
	 * runs. Exists for a bench capture that wants individual dwells.
	 */
	void (*ed)(uint8_t ch, const struct radiant_ed_event *evt, void *user);

	/*
	 * A receive window carrying this channel has been armed, with the
	 * shape it ACTUALLY got - routinely not what was asked for (merging
	 * widens, a nearer deadline truncates, a continuous request is armed
	 * one chunk at a time).
	 *
	 * Optional: a tracked channel has no use for it (a window either
	 * catches its slot or not). The wildcard sweep needs it because its
	 * dwell is accounted in listening time and can't credit bounds it
	 * merely proposed. Fires once per member per arm.
	 *
	 * Runs in the arming path (usually the radio interrupt). Do not post
	 * from it: the pass that armed this window is still running.
	 */
	void (*armed)(uint8_t ch, radiant_time_t t_open, radiant_time_t t_close,
		      void *user);

	/* The request is finished with. See enum radiant_sched_done. */
	void (*done)(uint8_t ch, enum radiant_sched_done why, void *user);
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
struct radiant_sched_stats {
	uint32_t windows_armed;     /* accepted radiant_radio_rx() calls */
	uint32_t channels_windowed; /* summed over those windows */
	uint32_t windows_merged;    /* of those, the ones carrying >1 channel */
	uint32_t filters_armed;     /* summed hardware filters used */
	uint32_t tx_armed;          /* accepted radiant_radio_tx() calls */
	uint32_t scan_chunks;       /* continuous-request chunks armed */
	uint32_t ed_chunks;         /* energy-detect chunks armed */
	uint32_t ed_dwells;         /* per-index measurements delivered: sweep
				     * rate against ed_chunks, map cost against
				     * windows_armed */
	uint32_t missed;            /* requests reported RADIANT_SCHED_DONE_MISSED */
	uint32_t denied;            /* RADIANT_SCHED_DONE_DENIED: air lent to
				     * another stack. Zero on a backend that owns
				     * the radio; separates "arbiter took our
				     * slot" from "sensor not there" against
				     * `missed` */
	uint32_t arm_denied;        /* of `denied`, refused synchronously by the
				     * arm call (vs. accepted and never granted) -
				     * different fixes: too-short a grant to hold
				     * the reservation, vs. lost to the other
				     * stack's scheduler */
	uint32_t preempted;         /* running operations cut short for a nearer
				     * deadline */
	uint32_t replans;           /* windows rebuilt before opening - free, lets
				     * a late-opened channel still join a merge */
	uint32_t stale_events;      /* events for an operation already retired */
	uint32_t arm_ebusy;         /* arm calls that lost the backend's race */
	uint32_t arm_rejected;      /* arm calls refused for any other reason */
	uint32_t arm_enotsup;       /* filter sets the backend could not put on
				     * the air. SHOULD STAY ZERO: the scheduler
				     * enforces caps.max_filters/max_addr_groups
				     * itself, so non-zero means core and backend
				     * disagree about the hardware */
	uint32_t phy_switches;      /* operations armed on a PHY not already
				     * configured, charged caps.phy_switch_us extra
				     * arm lead. Cost is per switch, not per frame:
				     * one long-range channel among 31 1M ones pays
				     * this twice per period (out and back), which
				     * is what says whether mixing PHYs is
				     * affordable. Zero-cost on both nRF builds
				     * (phy_switch_us == 0) but still counted. */
};

/* ---------------------------------------------------------------------------
 * API
 *
 * Return codes are radiant_radio_hal.h's, deliberately rather than a third code
 * space: almost every failure here either is a HAL failure or means exactly
 * what the HAL's name for it means.
 * ---------------------------------------------------------------------------
 */

/*
 * The radio callbacks the scheduler needs installed. Pass this to
 * radiant_radio_init():
 *
 *     radiant_sched_init(&my_cbs, ctx);
 *     radiant_radio_init(radiant_sched_radio_cbs(), NULL);
 *     radiant_radio_enable();
 *
 * The scheduler owns radio *events* because it is the only thing that arms; it
 * deliberately does not own the radio *lifecycle*, which belongs to radiant_api.c
 * and to whatever decides the dongle should be powered.
 */
const struct radiant_radio_cbs *radiant_sched_radio_cbs(void);

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
int radiant_sched_init(const struct radiant_sched_cbs *cbs, void *user);

/*
 * Drop every request without firing done(), aborting anything in flight. For
 * teardown; radiant_sched_cancel() is what a running system uses.
 *
 * Call this BEFORE radiant_radio_disable(), not after: disable() aborts the
 * live operation and delivers its terminal event from inside the still-
 * enabled disable call, which this module would otherwise respond to by
 * arming the next thing - powering down with an operation armed on it.
 */
void radiant_sched_reset(void);

/*
 * Post a request against a channel slot, replacing whatever that slot held.
 * Replacing an in-flight request aborts it, silently for that channel.
 *
 * NOTHING IS ARMED HERE. Post as many as are known and then call
 * radiant_sched_tick() once; from inside a callback, do not call tick at all,
 * because the scheduler commits on the way out.
 *
 * RADIANT_RADIO_EINVAL for a bad channel, a null format or filter array, an
 * n_filters outside 1..RADIANT_SCHED_MAX_FILTERS, or t_close before t_open.
 * RADIANT_RADIO_ESTATE before radiant_sched_init(). Note that a request wanting more
 * filters than the backend has is accepted here and reported through
 * done(RADIANT_SCHED_DONE_FAILED), not rejected: how many filters exist is a
 * runtime property and a caller that had to check it before every post would be
 * duplicating this module's only interesting decision.
 */
int radiant_sched_request_rx(uint8_t ch, const struct radiant_sched_rx *req);
int radiant_sched_request_tx(uint8_t ch, const struct radiant_sched_tx *req);

/*
 * Post a standing energy-detect request against a channel slot.
 *
 * RADIANT_RADIO_ENOTSUP when the core was built without
 * CONFIG_RADIANT_CORE_ED_SCAN, and when the backend's caps.has_ed_scan is
 * false. Refused HERE rather than reported through done(), unlike a filter set
 * the backend cannot match: a request that can never be armed is not
 * contention, and leaving it in the table would put a permanently unservable
 * slot in front of the leader search on every pass for the life of the image.
 *
 * RADIANT_RADIO_EINVAL for a bad channel, a range running backwards, or an
 * index above RADIANT_RF_INDEX_MAX.
 */
int radiant_sched_request_ed(uint8_t ch, const struct radiant_sched_ed *req);

/*
 * Drop this channel's request, aborting it if it is in flight.
 *
 * The abort is immediate, unlike a post - the point of cancelling is that the
 * radio stops now - but what the radio does *instead* is still decided at the
 * next commit. done() does not fire for ch; other channels that were sharing an
 * aborted merged window do get RADIANT_SCHED_DONE_ABORTED, because their operation
 * really did end early, and they should re-predict rather than assume their
 * window ran.
 */
int radiant_sched_cancel(uint8_t ch);

/* True if the slot holds a request, in flight or not. */
bool radiant_sched_pending(uint8_t ch);

/*
 * Shorten the chunk ceiling of a continuous request already in the slot.
 * chunk_us bounds ONE chunk, not the total; a caller spending a budget across
 * several chunks (a sweep dwell) must lower it as the budget runs down or the
 * last chunk overshoots.
 *
 * MEASURED on the nRF54L15 without this: one 4 Hz tracked channel left a
 * search chunk at its original ceiling and got 145.7 ms armed per chunk,
 * 2.6 chunks per set - 379 ms spent on a 260 ms dwell, sweep 46% slower than
 * its own budget, with no counter showing why.
 *
 * Only shortens - cannot grow the ceiling, and a bounded request has nothing
 * to shorten (both quietly ignored). Safe from a completion callback.
 */
int radiant_sched_rechunk(uint8_t ch, uint32_t chunk_us);

/*
 * Commit: look at the plan and the clock, and arm.
 *
 * This is the thread-context counterpart of the pass the scheduler runs at the
 * end of every radio callback, and calling it is how anything posted from
 * thread context reaches the radio at all. It is also the only recovery path
 * from the two arm failures that leave a request pending with nothing scheduled
 * to retry it - RADIANT_RADIO_EBUSY, when an arm raced the backend's callback, and
 * RADIANT_RADIO_ESTATE, when the radio was not enabled yet. This module owns no
 * timer: it arms for absolute instants and lets the backend hold them, so
 * nothing else will come along and try again.
 */
int radiant_sched_tick(void);

const struct radiant_sched_stats *radiant_sched_stats_get(void);
void                          radiant_sched_stats_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_SCHED_H_ */
