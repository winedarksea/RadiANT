/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sched.c - the radio schedule.
 *
 * Provenance: clean-room. Written from radiant_core/include/radiant_core/radiant_radio_hal.h (the
 * frozen backend contract) and radiant_core/include/radiant_core/radiant_sched.h, against the mock
 * in radiant_core/tests/fake_radio.c, with the two bounded constants in the header
 * taken from the bench measurements in docs/ant-radio-link.md and
 * docs/spike-b-part2-results.md. Nothing here derives from sdk-ant, from
 * libant.a, from disassembly of any binary, or from any adopter-gated ANT+
 * device profile document. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * The shape of it
 * ---------------------------------------------------------------------------
 * A table of 32 slots, each holding at most one intent, and one function -
 * pass() - that looks at the table and at the clock and decides what the radio
 * does next. Everything else in this file either fills the table, feeds pass()
 * an event, or is pass() split up so it can be read.
 *
 * pass() is called from exactly two places, and the list is short on purpose:
 *
 *   - the tail of a radio callback, which is the low-jitter re-arm path the
 *     HAL's callback contract exists to permit;
 *   - radiant_sched_tick(), the commit point for anything posted from thread
 *     context.
 *
 * POSTING A REQUEST DOES NOT ARM ANYTHING, and that is what makes merging work
 * rather than an accident of timing. A scheduler that re-planned on every
 * mutation would arm the first channel's window the instant it was posted, and
 * the second channel - whose window overlaps it exactly - would arrive to find
 * the radio already committed and merge with nothing. Requests are collected,
 * then committed once. In steady state that costs nothing extra, because every
 * channel posts its next window from inside the terminal callback of the last
 * one and the tail pass sees them all together.
 *
 * The transient is handled separately: a window that is armed but has not
 * opened yet can be torn down and rebuilt for free, so a channel that opens
 * late still joins the merge on the next tick instead of waiting a period. See
 * want_preempt().
 *
 * A pass costs one radiant_radio_now() and at most one arm call, whatever the state
 * of the table. That budget is not an optimisation: fake_radio.c records a
 * contract violation when a callback makes more than sixteen HAL calls, because
 * "do work proportional to anything - queue it and return" is exactly what a
 * scheduler is tempted to do in an ISR. The slot scans are pure arithmetic and
 * touch no hardware.
 *
 * ---------------------------------------------------------------------------
 * THE ORDER OF OPERATIONS AROUND AN ABORT, which is the subtle part
 * ---------------------------------------------------------------------------
 * radiant_radio_hal.h: an aborted operation's terminal event is STILL DELIVERED,
 * and the op id exists so that a late event from a cancelled operation is
 * recognisable rather than merely surprising. This file therefore always
 * retires the live operation - clearing s.armed_op - BEFORE calling
 * radiant_radio_abort(), and every event handler's first act is to compare the
 * event's op id against s.armed_op and drop anything that does not match.
 *
 * Get that order wrong and the bug is not a crash. The ABORTED event of the
 * operation you just replaced is taken for the terminal event of the one you
 * armed in its place, the replacement is retired while its window is still
 * open, and the radio is left listening for addresses nobody is routing any
 * more. This happens on every preemption, with no fault injection at all, so
 * s.stats.stale_events is a normal non-zero number and not an error count.
 *
 * ---------------------------------------------------------------------------
 * Thread against interrupt
 * ---------------------------------------------------------------------------
 * There are exactly two writers of `s`: thread context, through the three
 * request entry points and radiant_sched_tick(); and the radio ISR, through
 * hal_rx()/hal_tx(). Until a backend existed that actually raised callbacks,
 * the only thing between them was `in_pass`, a plain bool that was set on the
 * way into a callback and UNCONDITIONALLY CLEARED on the way out - so an ISR
 * landing on top of a thread-context pass() told the thread, on its way back,
 * that no pass was running. radiant_api.c predicted this in its own words at
 * the mutex that cannot cover it.
 *
 * It is not a rare interleaving either. It is scheduled by the protocol: a
 * master's EVENT_TX tells the host to write, the host's write pumps a pass in
 * thread context, and the slave's reply lands 2.19 ms later - inside that
 * window, on every exchange.
 *
 * Two things fix it, and both are needed:
 *
 *   - in_pass is SAVED AND RESTORED in hal_rx()/hal_tx(), so a nested entry
 *     leaves the flag as it found it and defers the commit to the pass that
 *     owns it;
 *   - every write to the slot table, and the whole of pass(), runs with
 *     interrupts off, via the port's radiant_event_crit_enter()/_exit(). That
 *     is the same primitive radiant_event.c uses for its ring, chosen over
 *     anything of this module's own precisely because the ISR side must not
 *     block. A pass is bounded arithmetic plus at most one arm call - the
 *     budget above - so the section cannot grow with the channel count.
 *
 * What this deliberately does NOT do is take a lock the HAL would have to know
 * about. Backends call these callbacks from their own interrupt; the contract
 * stays "call us, we return quickly", and the exclusion is entirely on this
 * side of it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <radiant_core/radiant_noise.h>
#include <radiant_core/radiant_radio_hal.h>
#include <radiant_core/radiant_sched.h>
/*
 * For radiant_event_crit_enter()/_exit() ONLY - the port's irq_lock() under a
 * neutral name, and the one primitive this module needs that the HAL does not
 * supply. Nothing here queues an event. See "Thread against interrupt" below.
 */
#include <radiant_core/radiant_event.h>

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------
 */

enum slot_kind {
	SLOT_IDLE = 0,
	SLOT_RX,
	SLOT_TX
};

/*
 * What one channel wants next.
 *
 * A slot holds either a receive or a transmit, never both, so the two share
 * t_start: for a receive it is t_open and for a transmit it is the requested
 * t_sync. t_end is the receive window's close, or RADIANT_TIME_NEVER for a
 * continuous request, and is unused by a transmit.
 */
struct sched_slot {
	uint8_t kind;      /* enum slot_kind */
	bool    in_flight; /* a member of the operation currently armed */
	bool    continuous;
	bool    stop_on_first;
	bool    report_crc_fail;
	uint8_t rf_index;
	uint8_t n_filters;
	uint8_t body_len;
	/* Transmit only: the on-air address, copied at the post. Six bytes of
	 * the per-channel budget buy the backend the one thing it cannot infer
	 * - see struct radiant_tx_req. */
	uint8_t addr_len;
	uint8_t addr[RADIANT_RADIO_ADDR_MAX];

	const struct radiant_pkt_format *fmt;
	const struct radiant_rx_filter  *filters; /* receive; caller-owned */
	const uint8_t               *body;    /* transmit; caller-owned */

	struct radiant_tx_power power;
	uint32_t            chunk_us;

	radiant_time_t t_start;
	radiant_time_t t_end;
};

/*
 * The plan budgets ~72 B per channel across the whole core. The slot table is
 * the scheduler's entire per-channel cost - there is no allocation anywhere in
 * this file - so holding it inside that figure keeps 32 channels affordable on
 * the part rather than only in the design document.
 */
_Static_assert(sizeof(struct sched_slot) <= 72u,
	       "a scheduler slot outgrew the per-channel RAM budget");

static struct {
	bool inited;
	/*
	 * A pass, or a callback that may lead to one, is running. Requests
	 * posted underneath it are recorded and considered on the way out
	 * rather than arming re-entrantly.
	 *
	 * SAVED AND RESTORED, NEVER UNCONDITIONALLY CLEARED, and the difference
	 * is not hypothetical against a real backend. hal_rx()/hal_tx() run in
	 * the radio ISR and can land on top of a thread-context pass() that is
	 * already inside its own; clearing the flag on the way out of the ISR
	 * would hand the suspended thread a false "no pass is running", and the
	 * very next thing it does is arm. The collision is scheduled rather than
	 * rare: EVENT_TX tells the host to write, the host's write pumps a pass
	 * in thread context, and the slave's reply lands 2.19 ms later - inside
	 * that window, every exchange.
	 */
	bool in_pass;
	/* Something in the table changed. Lets a non-terminal receive event
	 * cost nothing at all unless a callback actually posted something. */
	bool dirty;
	/* A request was posted since the last successful arm, so an armed
	 * window that has not opened yet is worth rebuilding. Cleared on every
	 * arm, which is what bounds re-planning to one attempt per request and
	 * keeps a request that cannot join from provoking one on every tick. */
	bool replan;

	struct radiant_sched_cbs cbs;
	void                *user;

	/* The POINTER is cached, never the contents: the HAL says it is static
	 * for the lifetime of the program, so max_filters and min_arm_lead_us
	 * are re-read on every pass and a suite that swaps capability presets
	 * between operations is obeyed immediately. */
	const struct radiant_radio_caps *caps;

	struct sched_slot ch[RADIANT_SCHED_MAX_CHANNELS];

	/* The live operation. armed_open/armed_end are the window it was armed
	 * with, not what anyone requested; armed_fmt and armed_rf are kept so
	 * that "could this new request have joined it?" is answerable without
	 * consulting a member slot that may already have been replaced. */
	uint32_t                     armed_op;
	uint8_t                      armed_kind;
	bool                         armed_stop_on_first;
	bool                         armed_continuous;
	radiant_time_t                   armed_open;
	radiant_time_t                   armed_end;
	const struct radiant_pkt_format *armed_fmt;
	uint8_t                      armed_rf;
	uint8_t                      members[RADIANT_SCHED_MAX_FILTERS];
	uint8_t                      n_members;

	/*
	 * The filter array handed to radiant_radio_rx(). The HAL requires it to
	 * stay valid until the terminal event, which is why it lives here and
	 * not on a stack frame; one radio means one live window means one
	 * array. owner[] and local[] are how an rx_event.filter_index becomes
	 * "channel c, its own filter i" with no search.
	 */
	struct radiant_rx_filter merged[RADIANT_SCHED_MAX_FILTERS];
	uint8_t              owner[RADIANT_SCHED_MAX_FILTERS];
	uint8_t              local[RADIANT_SCHED_MAX_FILTERS];
	uint8_t              n_merged;

	/* Where the next leader search starts. Without it, 32 channels asking
	 * for the same instant would be served in index order for ever and the
	 * high-numbered ones would never be armed at all. */
	uint8_t cursor;

	struct radiant_sched_stats stats;
} s;

/* What one step of a pass concluded. */
enum step {
	STEP_DONE = 0, /* the schedule is settled; stop */
	STEP_RETRY,    /* the table changed; look again */
	STEP_SKIP      /* this candidate will not fit; try the next */
};

/* ---------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------------
 */

/*
 * How many addresses one window may carry, RIGHT NOW.
 *
 * This function is the whole of the module's portability. Eight on nRF (one
 * base address plus eight prefixes), two on EFR32/RAIL (two runtime sync
 * words), and there is no other number anywhere in this file.
 */
static uint8_t max_filters(void)
{
	uint8_t mf;

	if (s.caps == NULL) {
		return 1u;
	}
	mf = s.caps->max_filters;
	if (mf == 0u) {
		mf = 1u;
	}
	if (mf > RADIANT_SCHED_MAX_FILTERS) {
		mf = RADIANT_SCHED_MAX_FILTERS;
	}
	return mf;
}

/* The slack budget: an arm call inside this window of now() fails
 * RADIANT_RADIO_ETIME rather than running late, so the scheduler subtracts it
 * itself and never asks for something the backend must refuse. */
static radiant_time_t arm_lead(void)
{
	return (s.caps == NULL) ? 0u : (radiant_time_t)s.caps->min_arm_lead_us;
}

static radiant_time_t t_max(radiant_time_t a, radiant_time_t b)
{
	return (a > b) ? a : b;
}

static radiant_time_t t_min(radiant_time_t a, radiant_time_t b)
{
	return (a < b) ? a : b;
}

/* t - d, floored at zero. The HAL's timebase never wraps, but it does start at
 * an unspecified origin and a test may set it low. */
static radiant_time_t t_back(radiant_time_t t, radiant_time_t d)
{
	return (t > d) ? (t - d) : 0u;
}

static void slot_clear(uint8_t ch)
{
	memset(&s.ch[ch], 0, sizeof(s.ch[ch]));
	s.ch[ch].kind = (uint8_t)SLOT_IDLE;
}

static void notify_done(uint8_t ch, enum radiant_sched_done why)
{
	if (why == RADIANT_SCHED_DONE_MISSED) {
		s.stats.missed++;
	}
	if (s.cbs.done != NULL) {
		s.cbs.done(ch, why, s.user);
	}
}

/* Tell one member of a window that has just been armed what it actually got.
 * See struct radiant_sched_cbs::armed - the shape is the arm's, not the
 * request's. */
static void notify_armed(uint8_t ch, radiant_time_t t_open, radiant_time_t t_close)
{
	if (s.cbs.armed != NULL) {
		s.cbs.armed(ch, t_open, t_close, s.user);
	}
}

static void clear_armed(void)
{
	s.armed_op = 0u;
	s.armed_kind = (uint8_t)SLOT_IDLE;
	s.armed_stop_on_first = false;
	s.armed_continuous = false;
	s.armed_open = 0u;
	s.armed_end = 0u;
	s.armed_fmt = NULL;
	s.armed_rf = 0u;
	s.n_members = 0u;
	s.n_merged = 0u;
}

/* ---------------------------------------------------------------------------
 * Ending an operation
 * ---------------------------------------------------------------------------
 */

/*
 * The armed operation reached its own end. Members are consumed and told why -
 * except a continuous request that ended normally, which is not consumed at
 * all: its done() is the per-dwell hook, and it is re-armed on the way out
 * unless the callback cancelled or replaced it.
 *
 * The slot is cleared BEFORE its owner is told, so that a done() callback which
 * immediately posts the next window is not overwritten by this function's own
 * tidying up.
 */
static void end_armed(enum radiant_sched_done why)
{
	uint8_t list[RADIANT_SCHED_MAX_FILTERS];
	uint8_t n = s.n_members;
	uint8_t i;

	if (s.armed_kind == (uint8_t)SLOT_IDLE) {
		return;
	}
	memcpy(list, s.members, sizeof(list));
	clear_armed();

	for (i = 0u; i < n; i++) {
		uint8_t            c = list[i];
		struct sched_slot *sl = &s.ch[c];
		bool               keep;

		sl->in_flight = false;
		keep = sl->continuous && sl->kind == (uint8_t)SLOT_RX &&
		       why == RADIANT_SCHED_DONE_OK;
		if (!keep) {
			slot_clear(c);
		}
		notify_done(c, why);
	}
}

/*
 * Take the radio away from whatever holds it. Returns true if the operation had
 * actually started, which is the only thing the caller needs to know about it.
 *
 * WHETHER A MEMBER'S REQUEST SURVIVES DEPENDS ENTIRELY ON WHETHER THE WINDOW
 * HAD OPENED, and getting that wrong is subtle in both directions.
 *
 * If it had not opened, nothing was lost - not a bit of preamble - so every
 * member's request is exactly as good as it was and stays pending, silently.
 * That is what makes rebuilding a window around a newly-opened channel free.
 * Consuming them here instead would be self-defeating: the rebuild would find
 * only the channel that provoked it, and merging would quietly stop happening
 * for every channel that did not open in the same millisecond as its
 * neighbours.
 *
 * If it had opened, the members really did lose the rest of their window, and
 * telling them so is what lets them re-predict rather than sit waiting for a
 * done() that is not coming. The exception is a background scan, which does not
 * end when it is displaced - it pauses, and resumes in the next gap.
 *
 * skip_ch is the channel that asked for this and therefore does not need to be
 * told. Retire first, abort second: see the header comment on this file.
 */
static bool abort_armed(uint8_t skip_ch)
{
	uint8_t list[RADIANT_SCHED_MAX_FILTERS];
	uint8_t n = s.n_members;
	bool    started;
	uint8_t i;

	if (s.armed_kind == (uint8_t)SLOT_IDLE) {
		return false;
	}

	started = s.armed_open <= radiant_radio_now();

	memcpy(list, s.members, sizeof(list));
	clear_armed();

	(void)radiant_radio_abort();

	for (i = 0u; i < n; i++) {
		uint8_t            c = list[i];
		struct sched_slot *sl = &s.ch[c];

		sl->in_flight = false;
		if (!started) {
			continue;
		}
		if (sl->continuous && sl->kind == (uint8_t)SLOT_RX) {
			/* Paused, not ended - the request survives untouched.
			 * It is still told, because the chunk that was running
			 * really did stop early and an owner accounting in
			 * listening time has to know where it stopped. */
			if (c != skip_ch) {
				notify_done(c, RADIANT_SCHED_DONE_ABORTED);
			}
			continue;
		}
		slot_clear(c);
		if (c != skip_ch) {
			notify_done(c, RADIANT_SCHED_DONE_ABORTED);
		}
	}
	return started;
}

/* Forget this channel's request, aborting it if it is in flight. Silent for ch
 * itself: whoever called already knows. */
static void drop_slot(uint8_t ch)
{
	if (s.ch[ch].in_flight) {
		(void)abort_armed(ch);
	}
	slot_clear(ch);
}

/* ---------------------------------------------------------------------------
 * Arming
 * ---------------------------------------------------------------------------
 */

static void pass(void);

/*
 * What to do about an arm call the backend refused.
 *
 * The governing rule is that a pass must always terminate. EBUSY and ESTATE are
 * transient and describe the radio rather than the request, so nothing is
 * consumed and the pass stops; everything else describes the request, so the
 * request is consumed and reported. A rejected request left in the table would
 * be retried by the very next pass, for ever.
 */
static enum step arm_failed(uint8_t ch, int rc)
{
	switch (rc) {
	case RADIANT_RADIO_EBUSY:
		/* The backend resolved a race with a callback that was about to
		 * consume the operation slot. The HAL says the core must handle
		 * this rather than assume it cannot happen. */
		s.stats.arm_ebusy++;
		return STEP_DONE;
	case RADIANT_RADIO_ESTATE:
		/* Not enabled. Every request in the table would fail the same
		 * way, so consuming them would empty it for a reason that has
		 * nothing to do with any of them. */
		s.stats.arm_rejected++;
		return STEP_DONE;
	case RADIANT_RADIO_ETIME:
		/* Unreachable by construction - the pass subtracts
		 * min_arm_lead_us itself - so reaching it means a backend is
		 * stricter than it advertises. Report the window as missed,
		 * which is what happened, and move on. */
		s.stats.arm_rejected++;
		slot_clear(ch);
		notify_done(ch, RADIANT_SCHED_DONE_MISSED);
		return STEP_RETRY;
	default:
		s.stats.arm_rejected++;
		slot_clear(ch);
		notify_done(ch, RADIANT_SCHED_DONE_FAILED);
		return STEP_RETRY;
	}
}

/*
 * Build one hardware receive window around a leader, merging in every other
 * channel that can share it, and arm it.
 *
 * THIS IS THE HIGHEST-VALUE FUNCTION IN THE MODULE. Everything ANT+ is on RF
 * 57, so two tracked channels whose predicted windows overlap can be heard by
 * one radiant_radio_rx() carrying one filter each. Without it, the scheduler must
 * pick one of the two and lose the other channel's packet outright; with it,
 * slave-side collisions between our own tracked channels are not reduced, they
 * are zero, and 32 sensors cost a handful of windows rather than 32.
 *
 * Four rules decide membership, and each one is a specific failure designed
 * out:
 *
 *   1. SAME FORMAT AND SAME RF INDEX. Radio configuration is per-operation, so
 *      merging two requests that disagree about it would put one of them on the
 *      air wrong. Pointer equality on fmt is the intended test - the core uses a
 *      small set of static const formats - and it is also, for free, what keeps
 *      a 3-byte search window out of a 5-byte tracking window and what will keep
 *      the planned long-range and off-57 channels out of the merge.
 *   2. AT MOST caps.max_filters ADDRESSES. Eight on nRF, two on RAIL. A window
 *      is never built past that number, and the number is read here rather than
 *      assumed anywhere.
 *   3. EVERY MEMBER MUST OVERLAP THE LEADER'S OWN WINDOW. Not merely the union
 *      so far: overlapping the union lets a chain of barely-touching windows
 *      walk the merged span arbitrarily far from where it started.
 *   4. THE UNION IS CAPPED. See RADIANT_SCHED_MERGE_SPAN_MAX_US - a merged window
 *      that could outlive the measured 2.19 ms master-to-slave turnaround could
 *      block a reply the link layer owes. The cap never shrinks the leader's own
 *      request; it only bounds what merging may add to it.
 */
static enum step arm_rx_window(uint8_t leader, radiant_time_t earliest,
			       radiant_time_t limit)
{
	struct sched_slot   *lead = &s.ch[leader];
	const uint8_t        mf = max_filters();
	struct radiant_rx_req    req;
	radiant_time_t           open;
	radiant_time_t           close;
	radiant_time_t           lead_open;
	radiant_time_t           lead_close;
	radiant_time_t           cap_close;
	uint32_t             flags = 0u;
	uint32_t             k;
	uint32_t             op = 0u;
	uint8_t              i;
	int                  rc;

	if (lead->n_filters == 0u || lead->n_filters > mf) {
		/*
		 * More addresses than this backend can match at once. On nRF
		 * with eight filters a sweep asking for eight is fine; the same
		 * request on RAIL's two is not, and the only honest answer is
		 * to say so once rather than to arm something smaller that the
		 * caller did not ask for and cannot tell apart.
		 */
		s.stats.arm_rejected++;
		slot_clear(leader);
		notify_done(leader, RADIANT_SCHED_DONE_FAILED);
		return STEP_RETRY;
	}

	open = t_max(lead->t_start, earliest);
	if (lead->continuous) {
		uint32_t chunk = (lead->chunk_us != 0u) ? lead->chunk_us
							: RADIANT_SCHED_SCAN_CHUNK_US;

		close = open + (radiant_time_t)chunk;
	} else {
		close = lead->t_end;
	}
	if (limit != RADIANT_TIME_NEVER) {
		close = t_min(close, limit);
	}
	if (close < open) {
		return STEP_SKIP;
	}

	lead_open = open;
	lead_close = close;
	cap_close = t_max(lead_close,
			  lead_open + (radiant_time_t)RADIANT_SCHED_MERGE_SPAN_MAX_US);

	s.n_merged = 0u;
	s.n_members = 0u;
	for (i = 0u; i < lead->n_filters; i++) {
		s.merged[s.n_merged] = lead->filters[i];
		s.owner[s.n_merged] = leader;
		s.local[s.n_merged] = i;
		s.n_merged++;
	}
	s.members[s.n_members] = leader;
	s.n_members++;
	if (lead->report_crc_fail) {
		flags |= RADIANT_RX_REPORT_CRC_FAIL;
	}

	for (k = 1u; k < RADIANT_SCHED_MAX_CHANNELS; k++) {
		uint8_t            c = (uint8_t)(((uint32_t)leader + k) %
						 RADIANT_SCHED_MAX_CHANNELS);
		struct sched_slot *m = &s.ch[c];
		radiant_time_t         m_open;
		radiant_time_t         m_close;
		radiant_time_t         n_open;
		radiant_time_t         n_close;

		if (s.n_merged >= mf || s.n_members >= RADIANT_SCHED_MAX_FILTERS) {
			break;
		}
		if (m->kind != (uint8_t)SLOT_RX || m->in_flight) {
			continue;
		}
		if (m->continuous != lead->continuous) {
			continue;
		}
		if (m->fmt != lead->fmt || m->rf_index != lead->rf_index) {
			continue;
		}
		if (m->n_filters == 0u ||
		    (uint32_t)s.n_merged + (uint32_t)m->n_filters > (uint32_t)mf) {
			continue;
		}

		m_open = t_max(m->t_start, earliest);
		m_close = m->continuous ? cap_close : m->t_end;
		if (m_close < m_open) {
			continue;
		}
		/* Rule 3: overlap the leader, not the union so far. */
		if (m_open > lead_close || m_close < lead_open) {
			continue;
		}

		n_open = t_min(open, m_open);
		n_close = t_max(close, m_close);
		if (limit != RADIANT_TIME_NEVER) {
			n_close = t_min(n_close, limit);
		}
		n_close = t_min(n_close, cap_close);
		/* After the caps, would this member get any coverage at all? A
		 * filter spent on a channel the window cannot hear is worse
		 * than not merging it, because on RAIL it is half of them. */
		if (n_close < n_open || m_open > n_close) {
			continue;
		}

		open = n_open;
		close = n_close;
		for (i = 0u; i < m->n_filters; i++) {
			s.merged[s.n_merged] = m->filters[i];
			s.owner[s.n_merged] = c;
			s.local[s.n_merged] = i;
			s.n_merged++;
		}
		s.members[s.n_members] = c;
		s.n_members++;
		if (m->report_crc_fail) {
			flags |= RADIANT_RX_REPORT_CRC_FAIL;
		}
	}

	/*
	 * RADIANT_RX_STOP_ON_FIRST saves receive current on a single-master window
	 * and the HAL forbids it on any window carrying more than one filter,
	 * because more than one master may transmit inside such a window. So it
	 * survives merging only when there was nothing to merge.
	 */
	if (s.n_merged == 1u && lead->stop_on_first && !lead->continuous) {
		flags |= RADIANT_RX_STOP_ON_FIRST;
	}

	memset(&req, 0, sizeof(req));
	req.fmt = lead->fmt;
	req.rf_index = lead->rf_index;
	req.filters = s.merged;
	req.n_filters = s.n_merged;
	req.t_open = open;
	req.t_close = close;
	req.flags = flags;

	rc = radiant_radio_rx(&req, &op);
	if (rc != RADIANT_RADIO_OK_RC) {
		s.n_merged = 0u;
		s.n_members = 0u;
		return arm_failed(leader, rc);
	}

	s.armed_op = op;
	s.armed_kind = (uint8_t)SLOT_RX;
	s.armed_open = open;
	s.armed_end = close;
	s.armed_fmt = lead->fmt;
	s.armed_rf = lead->rf_index;
	s.armed_stop_on_first = (flags & RADIANT_RX_STOP_ON_FIRST) != 0u;
	s.armed_continuous = lead->continuous;
	s.replan = false;
	for (i = 0u; i < s.n_members; i++) {
		s.ch[s.members[i]].in_flight = true;
	}

	s.stats.windows_armed++;
	s.stats.channels_windowed += s.n_members;
	s.stats.filters_armed += s.n_merged;
	if (s.n_members > 1u) {
		s.stats.windows_merged++;
	}
	if (lead->continuous) {
		s.stats.scan_chunks++;
	}

	s.cursor = (uint8_t)(((uint32_t)s.members[s.n_members - 1u] + 1u) %
			     RADIANT_SCHED_MAX_CHANNELS);

	/* Last, with every member's state already settled: a callback that
	 * looked at this module while it was half-armed would see a lie. */
	for (i = 0u; i < s.n_members; i++) {
		notify_armed(s.members[i], open, close);
	}
	return STEP_DONE;
}

static enum step arm_tx_op(uint8_t ch)
{
	struct sched_slot *sl = &s.ch[ch];
	struct radiant_tx_req  req;
	uint32_t           op = 0u;
	int                rc;

	memset(&req, 0, sizeof(req));
	req.fmt = sl->fmt;
	req.rf_index = sl->rf_index;
	req.power = sl->power;
	memcpy(req.addr, sl->addr, sizeof(req.addr));
	req.addr_len = sl->addr_len;
	req.body = sl->body;
	req.body_len = sl->body_len;
	req.t_sync_at = sl->t_start;

	rc = radiant_radio_tx(&req, &op);
	if (rc != RADIANT_RADIO_OK_RC) {
		return arm_failed(ch, rc);
	}

	s.armed_op = op;
	s.armed_kind = (uint8_t)SLOT_TX;
	s.armed_open = sl->t_start;
	s.armed_end = sl->t_start;
	s.armed_fmt = sl->fmt;
	s.armed_rf = sl->rf_index;
	s.armed_stop_on_first = false;
	s.armed_continuous = false;
	s.replan = false;
	s.n_merged = 0u;
	s.members[0] = ch;
	s.n_members = 1u;
	sl->in_flight = true;

	s.stats.tx_armed++;
	s.cursor = (uint8_t)(((uint32_t)ch + 1u) % RADIANT_SCHED_MAX_CHANNELS);
	return STEP_DONE;
}

/* ---------------------------------------------------------------------------
 * Choosing
 * ---------------------------------------------------------------------------
 */

/*
 * Could this request have been part of the window that is currently armed?
 *
 * Answered against the armed window's own recorded shape rather than against a
 * member slot, because a member slot may already have been replaced. It has to
 * mirror the membership rules in arm_rx_window() closely, for one reason: if it
 * says yes and the rebuild then says no, the request stays pending and would
 * ask for another rebuild on the next tick. That is why re-planning is
 * additionally gated on s.replan, which a successful arm clears - so a request
 * that cannot in fact join provokes at most one wasted rebuild rather than one
 * per tick for ever.
 */
static bool could_join_armed(const struct sched_slot *sl, radiant_time_t earliest)
{
	radiant_time_t o;
	radiant_time_t c;

	if (s.armed_kind != (uint8_t)SLOT_RX || sl->kind != (uint8_t)SLOT_RX) {
		return false;
	}
	/* Only worth it before the first bit of preamble: tearing down a window
	 * that has already opened throws away reception, and rebuilding it
	 * cannot give that back. */
	if (s.armed_open <= earliest) {
		return false;
	}
	if (sl->continuous != s.armed_continuous) {
		return false;
	}
	if (sl->fmt != s.armed_fmt || sl->rf_index != s.armed_rf) {
		return false;
	}
	if (sl->n_filters == 0u ||
	    (uint32_t)s.n_merged + (uint32_t)sl->n_filters >
		    (uint32_t)max_filters()) {
		return false;
	}
	o = t_max(sl->t_start, earliest);
	c = sl->continuous ? RADIANT_TIME_NEVER : sl->t_end;
	return !(o > s.armed_end || c < s.armed_open);
}

/*
 * Displace the live operation?
 *
 * Only for a deadline that cannot survive waiting, or for a rebuild that costs
 * nothing.
 *
 * A TRANSMIT INSIDE, OR JUST AFTER, THE ARMED WINDOW. This is the turnaround,
 * and it is the reason preemption exists at all: a tracking channel that hears
 * acknowledged data owes a full 8-byte frame about 1.55 ms later
 * (docs/spike-b-part2-results.md), which is the tightest deadline in the link
 * layer. The request arrives from inside the receive callback of the very
 * window that is still armed, so nothing but preemption can serve it. Waiting
 * for the window to close would mean arming inside min_arm_lead_us, and a
 * backend refuses a late arm rather than running it - correctly, since a late
 * reply is worse than none.
 *
 * A BOUNDED REQUEST UNDER A BACKGROUND SCAN. Tracked work outranks searching,
 * always; a scan chunk that has already started is simply cut short and resumed
 * afterwards.
 *
 * A CHANNEL THAT COULD HAVE JOINED A WINDOW THAT HAS NOT OPENED YET. Tearing
 * down a window before its first bit of preamble costs nothing at all, so a
 * channel opened after its neighbours joins the merge immediately instead of
 * spending a whole channel period in its own window. Without this, merging
 * would only ever happen in steady state, where every channel posts from the
 * same terminal callback - which is the case the tests would exercise and the
 * bench would not.
 *
 * AN ARMED TRANSMIT IS NEVER DISPLACED. Its t_sync is exact and already
 * committed, and this module schedules nothing more urgent than a reply that is
 * already owed.
 */
static bool want_preempt(radiant_time_t earliest)
{
	const radiant_time_t lead = arm_lead();
	uint32_t         i;

	if (s.armed_kind == (uint8_t)SLOT_TX) {
		return false;
	}

	for (i = 0u; i < RADIANT_SCHED_MAX_CHANNELS; i++) {
		const struct sched_slot *sl = &s.ch[i];

		if (sl->kind == (uint8_t)SLOT_IDLE || sl->in_flight) {
			continue;
		}
		if (sl->kind == (uint8_t)SLOT_TX) {
			if (sl->t_start < s.armed_end + lead) {
				return true;
			}
		} else if (s.armed_continuous && !sl->continuous) {
			if (sl->t_start < s.armed_end + lead) {
				return true;
			}
		}
		if (s.replan && could_join_armed(sl, earliest)) {
			return true;
		}
	}
	return false;
}

/*
 * Pick and arm.
 *
 * The rule is "run whatever starts first", with three refinements that all come
 * from there being one radio and no timer of our own:
 *
 *   - A TRANSMIT BREAKS A TIE and truncates a receive window that would
 *     otherwise still be open when the transmit has to be armed. Truncating
 *     costs the tail of a window; overrunning costs the frame.
 *   - A BACKGROUND SCAN FILLS THE GAP ahead of the next committed operation,
 *     and only if the gap is worth an arm. This is what makes it background:
 *     it never delays tracked work and it never has to be told when to stop,
 *     because the stop is computed from what else is pending.
 *   - OTHERWISE THE RADIO IS COMMITTED EARLY, even to something far away.
 *     There is no timer here - this module arms for absolute instants and lets
 *     the backend hold them - so leaving the radio idle would mean nothing woke
 *     up to arm it. Committing early costs nothing for a receive and is
 *     strictly better for a transmit's jitter, and preemption exists for the
 *     case where something nearer turns up afterwards.
 */
static enum step arm_next(radiant_time_t earliest)
{
	const radiant_time_t lead = arm_lead();
	int              tx_ch = -1;
	int              rx_ch = -1;
	int              scan_ch = -1;
	radiant_time_t       tx_at = RADIANT_TIME_NEVER;
	radiant_time_t       rx_at = RADIANT_TIME_NEVER;
	radiant_time_t       scan_at = RADIANT_TIME_NEVER;
	radiant_time_t       committed;
	uint32_t         k;

	for (k = 0u; k < RADIANT_SCHED_MAX_CHANNELS; k++) {
		uint8_t                  c = (uint8_t)(((uint32_t)s.cursor + k) %
						       RADIANT_SCHED_MAX_CHANNELS);
		const struct sched_slot *sl = &s.ch[c];
		radiant_time_t               at;

		if (sl->kind == (uint8_t)SLOT_IDLE || sl->in_flight) {
			continue;
		}
		if (sl->kind == (uint8_t)SLOT_TX) {
			if (tx_ch < 0 || sl->t_start < tx_at) {
				tx_ch = (int)c;
				tx_at = sl->t_start;
			}
			continue;
		}
		at = t_max(sl->t_start, earliest);
		if (sl->continuous) {
			if (scan_ch < 0 || at < scan_at) {
				scan_ch = (int)c;
				scan_at = at;
			}
		} else if (rx_ch < 0 || at < rx_at) {
			rx_ch = (int)c;
			rx_at = at;
		}
	}

	if (tx_ch < 0 && rx_ch < 0 && scan_ch < 0) {
		return STEP_DONE;
	}

	committed = RADIANT_TIME_NEVER;
	if (tx_ch >= 0) {
		committed = tx_at;
	}
	if (rx_ch >= 0) {
		committed = t_min(committed, rx_at);
	}

	if (scan_ch >= 0) {
		radiant_time_t s_open = t_max(scan_at, earliest);
		radiant_time_t s_limit = RADIANT_TIME_NEVER;
		uint32_t   chunk = s.ch[scan_ch].chunk_us;

		if (chunk == 0u) {
			chunk = RADIANT_SCHED_SCAN_CHUNK_US;
		}
		if (committed != RADIANT_TIME_NEVER) {
			s_limit = t_back(committed, lead);
		}
		if (s_limit == RADIANT_TIME_NEVER ||
		    s_limit >= s_open + (radiant_time_t)RADIANT_SCHED_SCAN_MIN_US) {
			radiant_time_t want = s_open + (radiant_time_t)chunk;
			enum step  st;

			st = arm_rx_window((uint8_t)scan_ch, earliest,
					   t_min(s_limit, want));
			if (st != STEP_SKIP) {
				return st;
			}
		}
	}

	if (rx_ch >= 0 && (tx_ch < 0 || rx_at <= tx_at)) {
		radiant_time_t limit = RADIANT_TIME_NEVER;
		enum step  st;

		if (tx_ch >= 0) {
			limit = t_back(tx_at, lead);
		}
		if (limit == RADIANT_TIME_NEVER || limit >= rx_at) {
			st = arm_rx_window((uint8_t)rx_ch, earliest, limit);
			if (st != STEP_SKIP) {
				return st;
			}
		}
	}

	if (tx_ch >= 0) {
		return arm_tx_op((uint8_t)tx_ch);
	}

	if (rx_ch >= 0) {
		enum step st = arm_rx_window((uint8_t)rx_ch, earliest,
					     RADIANT_TIME_NEVER);

		if (st != STEP_SKIP) {
			return st;
		}
	}

	return STEP_DONE;
}

/* ---------------------------------------------------------------------------
 * The pass
 * ---------------------------------------------------------------------------
 */

/*
 * Report and drop anything whose deadline has already gone, then either preempt
 * or arm.
 *
 * The expiry sweep runs even while an operation is armed, and that is
 * deliberate: a window lost to contention should be reported as
 * RADIANT_SCHED_DONE_MISSED at the moment it becomes unreachable, not silently
 * carried until the radio happens to be free. A channel counting those is a
 * channel losing its slot to another, which is exactly the condition merging
 * exists to remove and exactly the number that says whether it worked.
 */
static enum step pass_step(radiant_time_t now)
{
	const radiant_time_t earliest = now + arm_lead();
	uint32_t         i;

	for (i = 0u; i < RADIANT_SCHED_MAX_CHANNELS; i++) {
		struct sched_slot *sl = &s.ch[i];
		bool               expired;

		if (sl->kind == (uint8_t)SLOT_IDLE || sl->in_flight) {
			continue;
		}
		if (sl->kind == (uint8_t)SLOT_TX) {
			/* t_sync is exact and cannot be moved. A transmit that
			 * can no longer hit it does not go out at all. */
			expired = sl->t_start < earliest;
		} else {
			/* A receive window whose open has passed is still worth
			 * arming for the rest of its span - the window is
			 * defined on t_sync, and a t_sync inside what remains is
			 * still a frame caught - so only the close expires it. */
			expired = !sl->continuous && sl->t_end < earliest;
		}
		if (expired) {
			slot_clear((uint8_t)i);
			notify_done((uint8_t)i, RADIANT_SCHED_DONE_MISSED);
			return STEP_RETRY;
		}
	}

	if (s.armed_kind != (uint8_t)SLOT_IDLE) {
		if (!want_preempt(earliest)) {
			return STEP_DONE;
		}
		if (abort_armed(RADIANT_SCHED_CH_NONE)) {
			s.stats.preempted++;
		} else {
			s.stats.replans++;
		}
	}

	return arm_next(earliest);
}

/*
 * One scheduling decision, from whichever of the three entry points asked.
 *
 * radiant_radio_now() is read once and shared by every step: a pass reasons about
 * one instant, which is both cheaper in an ISR and easier to think about than a
 * clock that moves underneath the decision.
 */
static void pass(void)
{
	radiant_time_t now;
	uint32_t   guard;
	unsigned int key;

	/*
	 * INTERRUPTS OFF FOR THE WHOLE PASS. See "Thread against interrupt" in
	 * the header comment.
	 *
	 * The check-and-set of in_pass has to be atomic or it is not a guard at
	 * all, and the table this reads has to stop moving while it is read: a
	 * radio ISR landing between "choose the leader" and "arm it" runs
	 * end_armed() and clear_armed() underneath a decision already made
	 * against the old armed state. A pass costs one radiant_radio_now() and
	 * at most one arm call whatever the table looks like - that budget is
	 * stated at the top of this file and enforced by fake_radio.c - so this
	 * is bounded arithmetic plus one register write, not a section that can
	 * grow. Nested entry from a callback's own request is free: the port
	 * maps these onto irq_lock()/irq_unlock(), which nest by key.
	 */
	key = radiant_event_crit_enter();
	if (!s.inited || s.in_pass) {
		radiant_event_crit_exit(key);
		return;
	}
	s.in_pass = true;
	now = radiant_radio_now();

	/* Bounded because it must be: every retry either consumes a request or
	 * arms one, and there are only so many of each. The margin over the
	 * channel count is for the case that motivated the bound - one window
	 * closing late enough to expire every other channel at once, each of
	 * which posts its next window from inside the notification. */
	for (guard = 0u; guard < (2u * RADIANT_SCHED_MAX_CHANNELS) + 4u; guard++) {
		enum step st;

		s.dirty = false;
		st = pass_step(now);
		if (st != STEP_RETRY && !s.dirty) {
			break;
		}
	}
	s.dirty = false;
	s.in_pass = false;
	radiant_event_crit_exit(key);
}

/* ---------------------------------------------------------------------------
 * Radio events
 * ---------------------------------------------------------------------------
 */

static void route_rx(const struct radiant_rx_event *evt)
{
	uint8_t fi = evt->filter_index;

	if (fi >= s.n_merged) {
		/* A backend reporting a filter index the window did not have.
		 * Counted rather than trusted: routing it to a channel that
		 * did not ask for it is how a frame ends up on the wrong
		 * ANT channel, which is unfalsifiable from the host side. */
		s.stats.stale_events++;
		return;
	}
	if (s.cbs.rx != NULL) {
		s.cbs.rx(s.owner[fi], s.local[fi], evt, s.user);
	}
}

static void hal_rx(const struct radiant_rx_event *evt, void *user)
{
	bool terminal = false;
	bool was_in_pass;

	(void)user;

	if (evt == NULL) {
		return;
	}
	if (s.armed_kind != (uint8_t)SLOT_RX || evt->op == 0u ||
	    evt->op != s.armed_op) {
		/* The terminal event of something already retired - every
		 * preemption produces one. Not an error. */
		s.stats.stale_events++;
		return;
	}

	was_in_pass = s.in_pass;
	s.in_pass = true;

	switch (evt->status) {
	case RADIANT_RADIO_STATUS_OK:
	case RADIANT_RADIO_STATUS_CRC_FAIL:
		route_rx(evt);
		/* With RADIANT_RX_STOP_ON_FIRST the first accepted frame's event is
		 * itself the terminal one. A CRC failure is not an accepted
		 * frame, and on a 3-byte search address the matcher fires on
		 * noise several times a second, so the distinction is not
		 * academic. */
		if (evt->status == RADIANT_RADIO_STATUS_OK && s.armed_stop_on_first) {
			end_armed(RADIANT_SCHED_DONE_OK);
			terminal = true;
		}
		break;
	case RADIANT_RADIO_STATUS_TIMEOUT:
		/*
		 * The noise floor, taken here because this is the one place
		 * that knows both that the window ended empty and which
		 * frequency it was on.
		 *
		 * Before end_armed(), which clears s.armed_rf. A sample
		 * attributed to rf_index 0 instead of the one it was measured
		 * on is worse than no sample: the whole use of this figure is
		 * comparing one frequency against another.
		 *
		 * The backend has already decided whether the window is
		 * eligible - it sets has_noise only on a terminal timeout that
		 * received nothing - so there is no second test of that here,
		 * and deliberately: two places deciding the same condition is
		 * how they come to disagree.
		 */
		if (evt->has_noise) {
			radiant_noise_note(s.armed_rf, evt->noise_dbm);
		}
		end_armed(RADIANT_SCHED_DONE_OK);
		terminal = true;
		break;
	case RADIANT_RADIO_STATUS_ABORTED:
		end_armed(RADIANT_SCHED_DONE_ABORTED);
		terminal = true;
		break;
	case RADIANT_RADIO_STATUS_FAILED:
	default:
		end_armed(RADIANT_SCHED_DONE_FAILED);
		terminal = true;
		break;
	}

	s.in_pass = was_in_pass;
	/*
	 * Nested inside a thread-context pass: leave the commit to the pass that
	 * owns it. s.dirty is set by anything a callback posted and that pass's
	 * loop re-checks it, so nothing is lost - and calling pass() here would
	 * only be refused by its own guard anyway.
	 */
	if (!was_in_pass && (terminal || s.dirty)) {
		pass();
	}
}

static void hal_tx(const struct radiant_tx_event *evt, void *user)
{
	uint8_t ch;
	bool    was_in_pass;

	(void)user;

	if (evt == NULL) {
		return;
	}
	if (s.armed_kind != (uint8_t)SLOT_TX || evt->op == 0u ||
	    evt->op != s.armed_op) {
		s.stats.stale_events++;
		return;
	}

	ch = (s.n_members > 0u) ? s.members[0] : RADIANT_SCHED_CH_NONE;

	was_in_pass = s.in_pass;
	s.in_pass = true;
	if (evt->status == RADIANT_RADIO_STATUS_OK && ch != RADIANT_SCHED_CH_NONE &&
	    s.cbs.tx != NULL) {
		s.cbs.tx(ch, evt, s.user);
	}
	switch (evt->status) {
	case RADIANT_RADIO_STATUS_OK:
		end_armed(RADIANT_SCHED_DONE_OK);
		break;
	case RADIANT_RADIO_STATUS_ABORTED:
		end_armed(RADIANT_SCHED_DONE_ABORTED);
		break;
	default:
		end_armed(RADIANT_SCHED_DONE_FAILED);
		break;
	}
	s.in_pass = was_in_pass;
	if (!was_in_pass) {
		pass();
	}
}

static const struct radiant_radio_cbs sched_radio_cbs = {
	.rx = hal_rx,
	.tx = hal_tx,
};

const struct radiant_radio_cbs *radiant_sched_radio_cbs(void)
{
	return &sched_radio_cbs;
}

/* ---------------------------------------------------------------------------
 * API
 * ---------------------------------------------------------------------------
 */

int radiant_sched_init(const struct radiant_sched_cbs *cbs, void *user)
{
	memset(&s, 0, sizeof(s));

	/* The histogram belongs to the samples this module feeds it, so it is
	 * reset with this module and not separately. A stale distribution from
	 * before a reset would be reported as if it were this session's. */
	radiant_noise_reset();

	s.caps = radiant_radio_caps_get();
	if (s.caps == NULL) {
		return RADIANT_RADIO_EIO;
	}
	if (cbs != NULL) {
		s.cbs = *cbs;
	}
	s.user = user;
	s.inited = true;
	return RADIANT_RADIO_OK_RC;
}

void radiant_sched_reset(void)
{
	unsigned int key = radiant_event_crit_enter();

	if (s.armed_kind != (uint8_t)SLOT_IDLE) {
		clear_armed();
		(void)radiant_radio_abort();
	}
	memset(s.ch, 0, sizeof(s.ch));
	s.cursor = 0u;
	s.dirty = false;
	s.replan = false;
	radiant_event_crit_exit(key);
}

int radiant_sched_request_rx(uint8_t ch, const struct radiant_sched_rx *req)
{
	struct sched_slot *sl;
	unsigned int       key;

	if (!s.inited) {
		return RADIANT_RADIO_ESTATE;
	}
	if (ch >= RADIANT_SCHED_MAX_CHANNELS || req == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (req->fmt == NULL || req->filters == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (req->n_filters == 0u || req->n_filters > RADIANT_SCHED_MAX_FILTERS) {
		return RADIANT_RADIO_EINVAL;
	}
	if (req->t_open == RADIANT_TIME_NEVER) {
		return RADIANT_RADIO_EINVAL;
	}
	if (req->t_close != RADIANT_TIME_NEVER && req->t_close < req->t_open) {
		return RADIANT_RADIO_EINVAL;
	}

	/*
	 * The mutation window, closed against the radio ISR. Everything from
	 * here down rewrites one slot, and drop_slot() may retire the live
	 * operation on the way; an ISR landing in the middle would see a slot
	 * that is half the old request and half the new one. See "Thread
	 * against interrupt" in the header comment.
	 */
	key = radiant_event_crit_enter();

	drop_slot(ch);

	sl = &s.ch[ch];
	sl->kind = (uint8_t)SLOT_RX;
	sl->continuous = (req->t_close == RADIANT_TIME_NEVER);
	sl->stop_on_first = req->stop_on_first;
	sl->report_crc_fail = req->report_crc_fail;
	sl->rf_index = req->rf_index;
	sl->n_filters = req->n_filters;
	sl->fmt = req->fmt;
	sl->filters = req->filters;
	sl->chunk_us = req->chunk_us;
	sl->t_start = req->t_open;
	sl->t_end = req->t_close;

	s.dirty = true;
	s.replan = true;
	radiant_event_crit_exit(key);
	return RADIANT_RADIO_OK_RC;
}

int radiant_sched_request_tx(uint8_t ch, const struct radiant_sched_tx *req)
{
	struct sched_slot *sl;
	unsigned int       key;

	if (!s.inited) {
		return RADIANT_RADIO_ESTATE;
	}
	if (ch >= RADIANT_SCHED_MAX_CHANNELS || req == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (req->fmt == NULL || req->body == NULL || req->body_len == 0u) {
		return RADIANT_RADIO_EINVAL;
	}
	if (req->t_sync_at == RADIANT_TIME_NEVER) {
		return RADIANT_RADIO_EINVAL;
	}
	/*
	 * Refused here rather than passed down, because a backend that is handed
	 * a zero-length address has no honest move left: nRF's TXADDRESS would
	 * simply select whatever BASE/PREFIX the last operation loaded and emit
	 * a well-formed frame addressed to the wrong device. Checking against
	 * fmt->addr_len rather than merely against zero also catches the
	 * search-format address (3 bytes) being posted against the tracking
	 * format (5), which is a transmit nothing should ever make.
	 */
	if (req->addr_len != req->fmt->addr_len ||
	    req->addr_len > RADIANT_RADIO_ADDR_MAX) {
		return RADIANT_RADIO_EINVAL;
	}

	/* The mutation window. See radiant_sched_request_rx(). */
	key = radiant_event_crit_enter();

	drop_slot(ch);

	sl = &s.ch[ch];
	sl->kind = (uint8_t)SLOT_TX;
	sl->rf_index = req->rf_index;
	sl->fmt = req->fmt;
	memcpy(sl->addr, req->addr, sizeof(sl->addr));
	sl->addr_len = req->addr_len;
	sl->body = req->body;
	sl->body_len = req->body_len;
	sl->power = req->power;
	sl->t_start = req->t_sync_at;

	s.dirty = true;
	s.replan = true;
	radiant_event_crit_exit(key);
	return RADIANT_RADIO_OK_RC;
}

int radiant_sched_cancel(uint8_t ch)
{
	unsigned int key;

	if (!s.inited) {
		return RADIANT_RADIO_ESTATE;
	}
	if (ch >= RADIANT_SCHED_MAX_CHANNELS) {
		return RADIANT_RADIO_EINVAL;
	}
	/*
	 * The abort happens here rather than at the next commit, because
	 * cancelling is the one mutation whose whole point is that the radio
	 * stops doing something now. Choosing what it does instead is still the
	 * commit's job - except inside a callback, where the dispatch tail runs
	 * a pass anyway and the radio is never left idle.
	 */
	key = radiant_event_crit_enter();
	drop_slot(ch);
	s.dirty = true;
	radiant_event_crit_exit(key);
	return RADIANT_RADIO_OK_RC;
}

bool radiant_sched_pending(uint8_t ch)
{
	if (!s.inited || ch >= RADIANT_SCHED_MAX_CHANNELS) {
		return false;
	}
	return s.ch[ch].kind != (uint8_t)SLOT_IDLE;
}

int radiant_sched_rechunk(uint8_t ch, uint32_t chunk_us)
{
	unsigned int key;

	if (!s.inited) {
		return RADIANT_RADIO_ESTATE;
	}
	if (ch >= RADIANT_SCHED_MAX_CHANNELS) {
		return RADIANT_RADIO_EINVAL;
	}

	key = radiant_event_crit_enter();
	/*
	 * Only a continuous request has a chunk, and only shorter is allowed -
	 * see the header. An in-flight chunk keeps the length it was armed with;
	 * this bounds the NEXT one, which is the one the caller still has a say
	 * over.
	 */
	if (s.ch[ch].kind == (uint8_t)SLOT_RX && s.ch[ch].continuous &&
	    (s.ch[ch].chunk_us == 0u || chunk_us < s.ch[ch].chunk_us)) {
		s.ch[ch].chunk_us = chunk_us;
	}
	radiant_event_crit_exit(key);
	return RADIANT_RADIO_OK_RC;
}

int radiant_sched_tick(void)
{
	if (!s.inited) {
		return RADIANT_RADIO_ESTATE;
	}
	s.dirty = true;
	pass();
	return RADIANT_RADIO_OK_RC;
}

const struct radiant_sched_stats *radiant_sched_stats_get(void)
{
	return &s.stats;
}

void radiant_sched_stats_reset(void)
{
	memset(&s.stats, 0, sizeof(s.stats));
}
