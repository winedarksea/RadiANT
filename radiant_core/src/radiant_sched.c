/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sched.c - the radio schedule.
 *
 * Clean-room: written from radiant_radio_hal.h and radiant_sched.h against the
 * mock in tests/fake_radio.c; the two bounded constants come from the bench
 * measurements in docs/ant-radio-link.md and docs/spike-b-part2-results.md.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * Shape: a table of 32 slots, each holding at most one intent, and pass() -
 * which looks at the table and the clock and decides what the radio does
 * next. Everything else fills the table, feeds pass() an event, or is pass()
 * split up for readability.
 *
 * pass() runs from exactly two places: the tail of a radio callback (the
 * low-jitter re-arm path the HAL's callback contract permits), and
 * radiant_sched_tick(), the commit point for anything posted from thread
 * context.
 *
 * POSTING A REQUEST DOES NOT ARM ANYTHING - that's what makes merging work.
 * Re-planning on every mutation would arm the first channel's window the
 * instant it posted, and a second channel whose window overlaps it exactly
 * would arrive to find the radio already committed and merge with nothing.
 * Requests are collected and committed once; in steady state every channel
 * posts its next window from inside the last one's terminal callback, so the
 * tail pass sees them all together.
 *
 * A window that is armed but not yet open can be torn down and rebuilt for
 * free, so a late-opening channel still joins the merge on the next tick
 * instead of waiting a period. See want_preempt().
 *
 * A pass costs one radiant_radio_now() and at most one arm call regardless of
 * table state - fake_radio.c flags a contract violation past sixteen HAL
 * calls per callback. The slot scans are pure arithmetic, no hardware.
 *
 * ---------------------------------------------------------------------------
 * Abort ordering (the subtle part): an aborted operation's terminal event is
 * STILL DELIVERED (radiant_radio_hal.h), and the op id lets a late event from
 * a cancelled operation be recognised. This file always retires the live
 * operation (clears s.armed_op) BEFORE calling radiant_radio_abort(), and
 * every event handler's first act is comparing the event's op id against
 * s.armed_op and dropping mismatches.
 *
 * Get the order wrong and the ABORTED event of the operation you just
 * replaced is taken for the terminal event of its replacement, which is then
 * retired while its window is still open - radio left listening for nobody.
 * Happens on every preemption with no fault injection, so
 * s.stats.stale_events is normally non-zero, not an error count.
 *
 * ---------------------------------------------------------------------------
 * Thread against interrupt: `s` has exactly two writers - thread context via
 * the three request entry points and radiant_sched_tick(), and the radio ISR
 * via hal_rx()/hal_tx(). A plain bool `in_pass`, unconditionally cleared on
 * the way out of a callback, would let an ISR landing on top of a
 * thread-context pass() tell the thread, on return, that no pass was running.
 * Not rare: a master's EVENT_TX pumps a pass in thread context, and the
 * slave's reply lands 2.19 ms later - inside that window, every exchange.
 *
 * Two fixes, both needed:
 *   - in_pass is SAVED AND RESTORED in hal_rx()/hal_tx(), so a nested entry
 *     defers the commit to the pass that owns it;
 *   - every write to the slot table, and all of pass(), runs with interrupts
 *     off via radiant_event_crit_enter()/_exit() (same primitive
 *     radiant_event.c uses for its ring - the ISR side must not block). Bounded
 *     arithmetic plus at most one arm call, so the section can't grow with
 *     channel count.
 *
 * This deliberately does not take a lock the HAL has to know about - backends
 * call these callbacks from their own interrupt, and the exclusion stays
 * entirely on this side.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <radiant_core/radiant_chanmap.h>
#include <radiant_core/radiant_noise.h>
#include <radiant_core/radiant_radio_hal.h>
#include <radiant_core/radiant_sched.h>
/* For radiant_event_crit_enter()/_exit() only - the port's irq_lock() under a
 * neutral name; nothing here queues an event. See "Thread against interrupt"
 * above. */
#include <radiant_core/radiant_event.h>

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------
 */

/* Whether the energy-detect slot kind is compiled at all. Plain 0/1 macro,
 * not IS_ENABLED() (this file includes no Zephyr header by design). ED
 * branches read `if (SCHED_ED_BUILT && ...)` so the compiler folds it away
 * rather than the preprocessor forking the file into two shapes. */
#if defined(CONFIG_RADIANT_CORE_ED_SCAN)
#define SCHED_ED_BUILT 1
#else
#define SCHED_ED_BUILT 0
#endif

/*
 * SLOT_ED - the fourth kind, and the only one allowed to get nothing. It
 * measures the band on frequencies the link layer isn't using, for adaptive
 * frequency selection. Four rules, each closing off a way it could cost a
 * packet:
 *   - LOWEST PRIORITY, ALWAYS: chosen only when no TX/RX/scan wants the gap.
 *     A searching dongle measures nothing (correct - discovery beats a map).
 *   - NEVER PREEMPTS: skipped entirely by want_preempt().
 *   - MERGES WITH NOTHING: arm_rx_window()'s membership test is
 *     `kind == SLOT_RX`, which an ED slot fails; also a different HAL call.
 *   - TAKES GAPS ONLY: bounded by the next committed op minus arm lead, like
 *     a scan chunk, so it never delays what follows.
 * Together these mean the RX/TX arm sequence under full tracked load is
 * identical whether or not ED is posted.
 */
enum slot_kind {
	SLOT_IDLE = 0,
	SLOT_RX,
	SLOT_TX,
	SLOT_ED
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

	/* Energy detect only; rf_index above is the range's low end. rf_cursor
	 * is where the NEXT chunk starts, advanced per delivered dwell rather
	 * than per armed chunk - advancing at arm time would skip the tail of
	 * every chunk cut short by a tracked window, and always the same
	 * indices since preemption is periodic. */
	uint8_t rf_hi;
	uint8_t rf_cursor;

	const struct radiant_pkt_format *fmt;
	const struct radiant_rx_filter  *filters; /* receive; caller-owned */
	const uint8_t               *body;    /* transmit; caller-owned */

	struct radiant_tx_power power;
	/* Air time the owner may still need after this op ends; carried
	 * verbatim to the HAL request, see radiant_radio_hal.h. */
	uint16_t            follow_on_us;
	uint32_t            chunk_us;
	/* Energy detect only: per-index ceiling. Not folded into chunk_us -
	 * one arm call vs. one index; a chunk is a whole number of dwells. */
	uint32_t            dwell_us;

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
	/* A pass, or a callback that may lead to one, is running. Requests
	 * posted underneath it are recorded and considered on the way out
	 * rather than arming re-entrantly. SAVED AND RESTORED, never
	 * unconditionally cleared: hal_rx()/hal_tx() run in the radio ISR and
	 * can land on top of a thread-context pass() already in progress -
	 * clearing on the way out would tell the suspended thread "no pass is
	 * running" right before it arms. See "Thread against interrupt" above. */
	bool in_pass;
	/* Something in the table changed. Lets a non-terminal receive event
	 * cost nothing at all unless a callback actually posted something. */
	bool dirty;
	/* A request was posted since the last successful arm, so an armed
	 * window that has not opened yet is worth rebuilding. Cleared on every
	 * arm, which bounds re-planning to one attempt per request. */
	bool replan;

	struct radiant_sched_cbs cbs;
	void                *user;

	/* The POINTER is cached, never the contents: the HAL says it is static
	 * for the lifetime of the program, so max_filters/min_arm_lead_us are
	 * re-read on every pass and a preset swap between operations is obeyed
	 * immediately. */
	const struct radiant_radio_caps *caps;

	struct sched_slot ch[RADIANT_SCHED_MAX_CHANNELS];

	/* The live operation. armed_open/armed_end are the window it was armed
	 * with, not what was requested; armed_fmt/armed_rf let "could this new
	 * request have joined it?" be answered without consulting a member slot
	 * that may already have been replaced. */
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

	/* The filter array handed to radiant_radio_rx(). The HAL requires it to
	 * stay valid until the terminal event, hence it lives here rather than
	 * on a stack frame. owner[]/local[] turn an rx_event.filter_index into
	 * "channel c, its own filter i" with no search. */
	struct radiant_rx_filter merged[RADIANT_SCHED_MAX_FILTERS];
	uint8_t              owner[RADIANT_SCHED_MAX_FILTERS];
	uint8_t              local[RADIANT_SCHED_MAX_FILTERS];
	uint8_t              n_merged;

	/* Where the next leader search starts. Without it, 32 channels asking
	 * for the same instant would be served in index order for ever and the
	 * high-numbered ones would never be armed at all. */
	uint8_t cursor;

	/* The PHY the radio is currently configured for, and whether that is
	 * known. `phy_known` is false until the first arm (and after every
	 * radiant_sched_init()), and while false every candidate is charged the
	 * switch cost - the conservative direction, since assuming caps.phys[0]
	 * is wrong exactly on the first op after a reset that left the radio on
	 * something else, and would produce a late window with no counter to
	 * explain it. Updated ONLY on a successful arm - a refused arm leaves
	 * the radio in whatever state it was in. */
	uint8_t phy;
	bool    phy_known;

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

/* How many addresses one window may carry, right now: 8 on nRF (base address
 * + 8 prefixes), 2 on EFR32/RAIL (2 runtime sync words). No other number
 * appears anywhere in this file. */
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

/*
 * How many DISTINCT ADDRESS GROUPS one window may carry, right now.
 * caps.max_filters is how many addresses fit; this is how many distinct
 * values of addr[0 .. addr_len-2] fit - 8 and 2 on nRF, since 7 of its 8
 * logical addresses share one base register. On the 5-byte tracking format
 * the group carries the device number, so it binds on the second tracked
 * channel: without this check, ADR 0005's "32 sensors do not cost 32
 * windows" was false on nRF - the scheduler merged 8 tracked channels into a
 * window the backend refused with RADIANT_RADIO_ENOTSUP, charged to whichever
 * channel happened to lead. Zero means no group constraint.
 */
static uint8_t max_addr_groups(void)
{
	uint8_t mg;

	if (s.caps == NULL) {
		return 1u;
	}
	mg = s.caps->max_addr_groups;
	if (mg == 0u) {
		return max_filters();
	}
	if (mg > RADIANT_SCHED_MAX_FILTERS) {
		mg = RADIANT_SCHED_MAX_FILTERS;
	}
	return mg;
}

/* Is `f` in the same address group as `other`? Compares
 * addr[0 .. addr_len-2] - everything but the last byte, which nRF puts in a
 * prefix register and can vary freely within a group. A one-byte address is
 * entirely prefix, so every such filter shares the (empty) group. */
static bool same_addr_group(const struct radiant_rx_filter *f,
			    const struct radiant_rx_filter *other)
{
	if (f->addr_len != other->addr_len) {
		return false;
	}
	if (f->addr_len <= 1u) {
		return true;
	}
	return memcmp(f->addr, other->addr, (size_t)(f->addr_len - 1u)) == 0;
}

/* How many distinct groups the merged set would hold if `add` were included,
 * counted against s.merged[0 .. s.n_merged). Linear in set size (max 8); a
 * hash would cost more than it saves. */
static uint8_t addr_groups_with(const struct radiant_rx_filter *add,
				uint8_t n_add)
{
	uint8_t groups = 0u;
	uint8_t i;
	uint8_t j;

	for (i = 0u; i < s.n_merged; i++) {
		bool seen = false;

		for (j = 0u; j < i; j++) {
			if (same_addr_group(&s.merged[i], &s.merged[j])) {
				seen = true;
				break;
			}
		}
		if (!seen) {
			groups++;
		}
	}

	for (i = 0u; i < n_add; i++) {
		bool seen = false;

		for (j = 0u; j < s.n_merged; j++) {
			if (same_addr_group(&add[i], &s.merged[j])) {
				seen = true;
				break;
			}
		}
		for (j = 0u; !seen && j < i; j++) {
			if (same_addr_group(&add[i], &add[j])) {
				seen = true;
				break;
			}
		}
		if (!seen) {
			groups++;
		}
	}

	return groups;
}

/* The slack budget: an arm call inside this window of now() fails
 * RADIANT_RADIO_ETIME rather than running late, so the scheduler subtracts it
 * itself and never asks for something the backend must refuse. */
static radiant_time_t arm_lead(void)
{
	return (s.caps == NULL) ? 0u : (radiant_time_t)s.caps->min_arm_lead_us;
}

/*
 * The EXTRA lead this candidate needs because the radio isn't already
 * configured for its PHY. Zero when it already is, or on a backend that
 * switches for free. Not folded into arm_lead(): that's what EVERY op costs,
 * this is what ONE op costs following a different one - folding them would
 * charge every window on a two-PHY build for a switch that happens at most
 * twice a period, eating into the gap the scan and ED map live in.
 * NULL format = an ED chunk, which has no PHY; charged nothing, s.phy
 * untouched (see arm_ed_chunk()).
 */
static radiant_time_t phy_lead(const struct radiant_pkt_format *fmt)
{
	if (s.caps == NULL || fmt == NULL || s.caps->phy_switch_us == 0u) {
		return 0u;
	}
	if (s.phy_known && (uint8_t)fmt->phy == s.phy) {
		return 0u;
	}
	return (radiant_time_t)s.caps->phy_switch_us;
}

/*
 * The lead a gap must leave in front of a committed operation, when
 * something else runs in that gap first. NOT phy_lead(): phy_lead() asks
 * "does this differ from what the radio is configured for NOW", which is
 * wrong here because by the time the committed operation is armed, the radio
 * will be configured for whatever the gap filler left behind, not for today's
 * PHY. Get this wrong and a tracked window preceded by a scan chunk on a
 * different PHY is armed phy_switch_us late, every period, with only a
 * dropped frame to show for it.
 * `after` NULL means the filler leaves the configuration alone (an ED chunk).
 */
static radiant_time_t phy_gap(const struct radiant_pkt_format *after,
			      const struct radiant_pkt_format *next)
{
	if (s.caps == NULL || next == NULL || s.caps->phy_switch_us == 0u) {
		return 0u;
	}
	if (after != NULL) {
		return ((uint8_t)after->phy == (uint8_t)next->phy)
			       ? 0u
			       : (radiant_time_t)s.caps->phy_switch_us;
	}
	return phy_lead(next);
}

/* Record what the radio is now configured for. Called only from a successful
 * arm; see the field's comment. */
static void phy_armed(const struct radiant_pkt_format *fmt)
{
	if (fmt == NULL) {
		return;
	}
	if (!s.phy_known || s.phy != (uint8_t)fmt->phy) {
		s.stats.phy_switches++;
	}
	s.phy = (uint8_t)fmt->phy;
	s.phy_known = true;
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

/* Bound a window by what the backend can arm in ONE operation
 * (caps.max_window_us; 0 = unbounded). Changes the request itself, so the
 * bounds the owner is told about match what was armed. The remainder isn't
 * lost - a continuous request survives a chunk that ended normally; dwell
 * carries across chunk boundaries via radiant_sched_rechunk(). */
static radiant_time_t window_cap(radiant_time_t open, radiant_time_t close)
{
	uint32_t max = (s.caps != NULL) ? s.caps->max_window_us : 0u;

	if (max == 0u || close <= open) {
		return close;
	}
	if ((close - open) <= (radiant_time_t)max) {
		return close;
	}
	return open + (radiant_time_t)max;
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
	} else if (why == RADIANT_SCHED_DONE_DENIED) {
		s.stats.denied++;
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
 * The armed operation reached its own end. Members are consumed and told why,
 * except a continuous request that ended normally: its done() is the
 * per-dwell hook and it re-arms unless the callback cancelled/replaced it.
 * The slot is cleared BEFORE its owner is told, so a done() that immediately
 * posts the next window isn't overwritten by this function's own tidying.
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
		/* An ED request is continuous in the same sense a background
		 * scan is - never consumed by a chunk that ended normally -
		 * but carries no `continuous` flag: there is no bounded form
		 * to distinguish it from. */
		/*
		 * DONE_DENIED and DONE_ABORTED must also keep the request, or
		 * ED scanning stops for good: nothing re-posts an ED request,
		 * so slot_clear() here would silently end the sweep at the
		 * first denial/abort with the map simply going stale and no
		 * counter moving. abort_armed()'s preemption path uses the
		 * same continuous-RX/ED test ("paused, not ended"); this is
		 * the other way a chunk stops (RADIANT_RADIO_STATUS_ABORTED,
		 * e.g. an MPSL timeslot cutting a chunk short) and the two
		 * paths must agree, or a scan meant to pause instead gets
		 * deleted here while radiant_api.c still thinks it's live -
		 * wedging the dongle under arbitration permanently (reproduced
		 * on nRF54L15 DK with a BLE advertiser beside the sweep:
		 * search stalled after 84 arms, zero packets thereafter).
		 */
		keep = (why == RADIANT_SCHED_DONE_OK ||
			why == RADIANT_SCHED_DONE_DENIED ||
			why == RADIANT_SCHED_DONE_ABORTED) &&
		       ((sl->continuous && sl->kind == (uint8_t)SLOT_RX) ||
			sl->kind == (uint8_t)SLOT_ED);
		if (!keep) {
			slot_clear(c);
		}
		notify_done(c, why);
	}
}

/*
 * Take the radio away from whatever holds it. Returns true if the operation
 * had actually started.
 *
 * Whether a member's request survives depends entirely on whether the window
 * had opened. If not, nothing was lost, so every member's request stays
 * pending silently - this is what makes rebuilding around a newly-opened
 * channel free; consuming them here would mean merging quietly stops working
 * for channels that don't open in the same millisecond as their neighbours.
 * If it had opened, members really did lose the rest of their window and are
 * told so, to re-predict rather than wait for a done() that isn't coming -
 * except a background scan, which pauses and resumes in the next gap.
 *
 * skip_ch is the channel that asked for this and needs no telling. Retire
 * first, abort second - see the header comment.
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
		if ((sl->continuous && sl->kind == (uint8_t)SLOT_RX) ||
		    sl->kind == (uint8_t)SLOT_ED) {
			/* Paused, not ended - request survives untouched, but
			 * still told, since an owner accounting listening time
			 * needs to know it stopped early. ED: indices already
			 * measured are in the map; rf_cursor marks where the
			 * next gap resumes. */
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
 * What to do about an arm call the backend refused. Governing rule: a pass
 * must always terminate. EBUSY/ESTATE are transient and describe the radio,
 * not the request, so nothing is consumed and the pass stops; everything else
 * describes the request, so it's consumed and reported - left in the table it
 * would be retried by the very next pass, forever.
 */
static enum step arm_failed(uint8_t ch, int rc)
{
	switch (rc) {
	case RADIANT_RADIO_EBUSY:
		/* Backend resolved a race with a callback about to consume the
		 * op slot; the HAL says the core must handle this. */
		s.stats.arm_ebusy++;
		return STEP_DONE;
	case RADIANT_RADIO_ESTATE:
		/* Not enabled. Every request would fail the same way, so
		 * consuming them would empty the table for no reason of their
		 * own. */
		s.stats.arm_rejected++;
		return STEP_DONE;
	case RADIANT_RADIO_ETIME:
		/* Unreachable by construction (the pass subtracts
		 * min_arm_lead_us itself); reaching it means the backend is
		 * stricter than advertised. Report missed and move on. */
		s.stats.arm_rejected++;
		slot_clear(ch);
		notify_done(ch, RADIANT_SCHED_DONE_MISSED);
		return STEP_RETRY;
	case RADIANT_RADIO_EDENIED: {
		/*
		 * An arbitrated backend has the radio but not the air -
		 * describes another stack, not this request or the radio's
		 * lifecycle. Survival follows the same rule end_armed() uses:
		 * a denied continuous scan or ED sweep lost nothing (the chunk
		 * never opened) and still wants the next gap - consuming it
		 * would be wasteful for a scan (radiant_api.c re-posts it) and
		 * permanent for ED (nothing re-posts it, so the map would just
		 * go stale). A bounded request is consumed and reported like
		 * ETIME above, for the same "a pass must terminate" reason;
		 * it's then the owner's business whether to re-post.
		 * A kept request ends the pass (STEP_DONE) rather than
		 * retrying, or the next step would offer the same slot to the
		 * same arbiter and be refused again up to the iteration bound,
		 * burning the whole budget for nothing.
		 */
		bool keep = (s.ch[ch].continuous &&
			     s.ch[ch].kind == (uint8_t)SLOT_RX) ||
			    s.ch[ch].kind == (uint8_t)SLOT_ED;

		s.stats.arm_denied++;
		if (!keep) {
			slot_clear(ch);
		}
		notify_done(ch, RADIANT_SCHED_DONE_DENIED);
		return keep ? STEP_DONE : STEP_RETRY;
	}
	case RADIANT_RADIO_ENOTSUP:
		/* A filter set the backend cannot put on the air. Unreachable
		 * by construction (arm_rx_window() enforces max_filters and
		 * max_addr_groups before building a set); reaching it means
		 * the core and backend disagree about the hardware, so it gets
		 * its own counter rather than folding into arm_rejected.
		 * MISSED rather than FAILED: `ch` is the leader, and charging
		 * it a hard failure for a constraint from a channel that
		 * merged in behind it would make a healthy master look broken -
		 * the window just didn't happen. */
		s.stats.arm_enotsup++;
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
 * The highest-value function in the module: since all ANT+ is on RF 57, two
 * tracked channels whose predicted windows overlap can be heard by one
 * radiant_radio_rx() carrying one filter each - so 32 sensors cost a handful
 * of windows, not 32, and slave-side collisions between our own tracked
 * channels go to zero.
 *
 * Five membership rules:
 *   1. SAME FORMAT AND SAME RF INDEX (pointer equality on fmt - the core uses
 *      static const formats). Keeps a 3-byte search window out of a 5-byte
 *      tracking window; radio config is per-operation so mixing would put one
 *      request on the air wrong.
 *   2. AT MOST caps.max_filters ADDRESSES (8 on nRF, 2 on RAIL), read here
 *      rather than assumed.
 *   3. EVERY MEMBER MUST OVERLAP THE LEADER'S OWN WINDOW, not merely the
 *      union so far - overlapping the union would let a chain of
 *      barely-touching windows walk the merged span arbitrarily far.
 *   4. THE UNION IS CAPPED (RADIANT_SCHED_MERGE_SPAN_MAX_US) so a merged
 *      window can't outlive the measured 2.19 ms master-to-slave turnaround
 *      and block a reply the link layer owes. Never shrinks the leader's own
 *      request, only bounds what merging adds.
 *   5. AT MOST caps.max_addr_groups DISTINCT ADDRESS GROUPS (2 on nRF: 7 of
 *      its 8 logical addresses share one base register, and the tracking
 *      format's device number lives inside that base). Rule 2 counts
 *      addresses, this counts device numbers - conflating them is what made
 *      ADR 0005's "32 sensors do not cost 32 windows" false on nRF until this
 *      existed.
 */
static enum step arm_rx_window(uint8_t leader, radiant_time_t earliest,
			       radiant_time_t limit)
{
	struct sched_slot   *lead = &s.ch[leader];
	const uint8_t        mf = max_filters();
	const uint8_t        mg = max_addr_groups();
	struct radiant_rx_req    req;
	radiant_time_t           open;
	radiant_time_t           close;
	radiant_time_t           lead_open;
	radiant_time_t           lead_close;
	radiant_time_t           cap_close;
	uint32_t             flags = 0u;
	uint16_t             follow_on = 0u;
	uint32_t             k;
	uint32_t             op = 0u;
	uint8_t              i;
	int                  rc;

	if (lead->n_filters == 0u || lead->n_filters > mf) {
		/* More addresses than this backend can match at once. Say so
		 * once rather than silently arming something smaller than
		 * asked for. */
		s.stats.arm_rejected++;
		slot_clear(leader);
		notify_done(leader, RADIANT_SCHED_DONE_FAILED);
		return STEP_RETRY;
	}

	/* Same test against the group cap, separately: a leader can be inside
	 * max_filters and outside max_addr_groups the moment its own filters
	 * carry two different device numbers. */
	s.n_merged = 0u;
	if (addr_groups_with(lead->filters, lead->n_filters) > mg) {
		s.stats.arm_rejected++;
		slot_clear(leader);
		notify_done(leader, RADIANT_SCHED_DONE_FAILED);
		return STEP_RETRY;
	}

	/* The window cannot open before the radio can be reconfigured for its
	 * PHY. Applied to the LEADER only - exact, not approximate: rule 1
	 * requires pointer-equal formats, so every member is on the leader's
	 * PHY by construction and there is no second switch inside one window. */
	open = t_max(lead->t_start, earliest + phy_lead(lead->fmt));
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
	close = window_cap(open, close);
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
	follow_on = lead->follow_on_us;

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
		/* Rule 5: at most caps.max_addr_groups distinct groups (2 on
		 * nRF). Skipping this member is correct, not refusing the
		 * window - the leader and whatever already merged are still
		 * armable, and this channel gets its own window later. Before
		 * this test the set went to the backend, came back ENOTSUP,
		 * and cost the leader a DONE_FAILED for a constraint it had no
		 * part in. */
		if (addr_groups_with(m->filters, m->n_filters) > mg) {
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
		n_close = window_cap(n_open, n_close);
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
		if (m->follow_on_us > follow_on) {
			follow_on = m->follow_on_us;
		}
	}

	/* RADIANT_RX_STOP_ON_FIRST saves receive current on a single-master
	 * window; the HAL forbids it on any window carrying more than one
	 * filter. So it survives merging only when there was nothing to merge. */
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
	/* The maximum over members, not the leader's: any member's frame may
	 * turn into an acknowledged-data reply, so reserving only the leader's
	 * would under-reserve exactly when merging did its job. Accumulated in
	 * the member loop above. */
	req.follow_on_us = follow_on;

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
	phy_armed(lead->fmt);
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
	req.follow_on_us = sl->follow_on_us;

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
	phy_armed(sl->fmt);
	s.replan = false;
	s.n_merged = 0u;
	s.members[0] = ch;
	s.n_members = 1u;
	sl->in_flight = true;

	s.stats.tx_armed++;
	s.cursor = (uint8_t)(((uint32_t)ch + 1u) % RADIANT_SCHED_MAX_CHANNELS);
	return STEP_DONE;
}

/*
 * Arm one energy-detect chunk into the gap ahead of `limit`. Modelled on the
 * continuous-scan path in arm_rx_window(), not the bounded-window one: a
 * bounded request has a deadline to hit or report missed, this has neither,
 * so the chunk is "as much of the request as fits" - a gap too small for one
 * dwell produces no chunk and no complaint.
 *
 * THE CHUNK IS A WHOLE NUMBER OF DWELLS. The backend gets rf_index_lo..hi and
 * a per-index ceiling, so the longest it can run is (hi-lo+1)*dwell - sizing
 * the range from the gap bounds the operation by construction rather than by
 * a close-compare each backend would have to honour separately.
 *
 * The range never wraps inside one chunk: a sweep that reaches rf_hi stops
 * there and the next chunk restarts at rf_lo (at most one short chunk per
 * sweep), keeping the arm arithmetic a subtraction, not a modulo.
 */
static enum step arm_ed_chunk(uint8_t ch, radiant_time_t earliest,
			      radiant_time_t limit)
{
	struct sched_slot   *sl = &s.ch[ch];
	struct radiant_ed_req    req;
	radiant_time_t           open;
	uint64_t             span;
	uint32_t             dwell;
	uint32_t             n;
	uint32_t             avail;
	uint32_t             op = 0u;
	int                  rc;

	if (!SCHED_ED_BUILT) {
		return STEP_SKIP;
	}

	open = t_max(sl->t_start, earliest);
	if (limit == RADIANT_TIME_NEVER || limit <= open) {
		/* No committed operation to bound the chunk. Unreachable from
		 * arm_next() (always passes a limit here); skipped rather than
		 * unbounded, since an ED op with no end is a radio given away. */
		return STEP_SKIP;
	}

	dwell = (sl->dwell_us != 0u) ? sl->dwell_us : RADIANT_SCHED_ED_DWELL_US;
	span = (uint64_t)(limit - open);
	n = (uint32_t)(span / (uint64_t)dwell);
	if (n == 0u) {
		return STEP_SKIP;
	}

	/* The chunk ceiling, in dwells rather than in microseconds, for the same
	 * reason the gap is: the request that reaches the backend is a range. */
	{
		uint32_t chunk = (sl->chunk_us != 0u) ? sl->chunk_us
						      : RADIANT_SCHED_ED_CHUNK_US;
		uint32_t cap = chunk / dwell;

		if (cap == 0u) {
			cap = 1u;
		}
		if (n > cap) {
			n = cap;
		}
	}

	/* What is left of the range from the cursor to its top. */
	if (sl->rf_cursor < sl->rf_index || sl->rf_cursor > sl->rf_hi) {
		sl->rf_cursor = sl->rf_index;
	}
	avail = (uint32_t)(sl->rf_hi - sl->rf_cursor) + 1u;
	if (n > avail) {
		n = avail;
	}

	memset(&req, 0, sizeof(req));
	req.rf_index_lo = sl->rf_cursor;
	req.rf_index_hi = (uint8_t)(sl->rf_cursor + (n - 1u));
	req.dwell_us = dwell;
	req.t_start = open;

	rc = radiant_radio_ed(&req, &op);
	if (rc != RADIANT_RADIO_OK_RC) {
		return arm_failed(ch, rc);
	}

	s.armed_op = op;
	s.armed_kind = (uint8_t)SLOT_ED;
	s.armed_open = open;
	s.armed_end = open + (radiant_time_t)n * (radiant_time_t)dwell;
	s.armed_fmt = NULL;
	s.armed_rf = req.rf_index_lo;
	s.armed_stop_on_first = false;
	s.armed_continuous = false;
	s.n_merged = 0u;
	s.members[0] = ch;
	s.n_members = 1u;
	sl->in_flight = true;

	/* NEITHER s.cursor NOR s.replan IS TOUCHED. Moving s.cursor here would
	 * let an ED chunk decide which tracked channel leads the next merged
	 * window, making the receive schedule depend on whether ED is
	 * compiled in - an ED slot doesn't compete for the radio, so it has no
	 * place in the fairness rotation of slots that do. Clearing s.replan
	 * here would spend a pending channel's one rebuild opportunity on an
	 * operation it could never join (could_join_armed() requires both
	 * sides SLOT_RX). */
	s.stats.ed_chunks++;
	return STEP_DONE;
}

/* ---------------------------------------------------------------------------
 * Choosing
 * ---------------------------------------------------------------------------
 */

/*
 * Could this request have been part of the window currently armed? Checked
 * against the armed window's own recorded shape, not a member slot (which
 * may already have been replaced). Must mirror arm_rx_window()'s membership
 * rules closely: a false positive here leaves the request pending and asks
 * for another rebuild next tick - which is why re-planning is also gated on
 * s.replan, cleared on a successful arm, bounding the waste to one rebuild
 * per request rather than one per tick forever.
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
	/* Rule 5, mirrored. Without it this function says "yes, it could have
	 * joined", the rebuild's own group test says no, and the request stays
	 * pending - which is the one-wasted-rebuild-per-request case s.replan
	 * bounds, paid on every tracked channel after the second rather than
	 * never. */
	if (addr_groups_with(sl->filters, sl->n_filters) > max_addr_groups()) {
		return false;
	}
	o = t_max(sl->t_start, earliest);
	c = sl->continuous ? RADIANT_TIME_NEVER : sl->t_end;
	return !(o > s.armed_end || c < s.armed_open);
}

/*
 * Displace the live operation? Only for a deadline that can't survive
 * waiting, or a rebuild that costs nothing.
 *
 *   - A TRANSMIT INSIDE/JUST AFTER THE ARMED WINDOW: the turnaround, and the
 *     reason preemption exists - a tracking channel that hears acknowledged
 *     data owes an 8-byte reply ~1.55 ms later (docs/spike-b-part2-results.md),
 *     the tightest deadline in the link layer, requested from inside the
 *     still-armed window's own receive callback. Waiting would mean arming
 *     inside min_arm_lead_us, which backends refuse.
 *   - A BOUNDED REQUEST UNDER A BACKGROUND SCAN: tracked work always
 *     outranks searching; an in-progress scan chunk is just cut short and
 *     resumed afterwards.
 *   - A CHANNEL THAT COULD HAVE JOINED A WINDOW NOT YET OPEN: tearing down
 *     before the first bit of preamble costs nothing, so a late-opened
 *     channel joins the merge immediately instead of waiting a period.
 *   - AN ARMED TRANSMIT IS NEVER DISPLACED: its t_sync is exact and already
 *     committed, and nothing here is more urgent than a reply already owed.
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
		/* An ED request never preempts anything. Skipped before the
		 * scan test below, which it would otherwise satisfy (ED slots
		 * carry continuous == false), letting a measurement displace a
		 * search chunk. */
		if (sl->kind == (uint8_t)SLOT_ED) {
			continue;
		}
		if (sl->kind == (uint8_t)SLOT_TX) {
			/* Plus reconfiguration lead, so a transmit on the other
			 * PHY displaces early enough to be armed rather than
			 * refused. */
			if (sl->t_start < s.armed_end + lead + phy_lead(sl->fmt)) {
				return true;
			}
		} else if (s.armed_continuous && !sl->continuous) {
			if (sl->t_start <
			    s.armed_end + lead + phy_lead(sl->fmt)) {
				return true;
			}
		}
		/* Anything displaces an armed ED chunk. It was sized to end
		 * before whatever was committed when armed, but a request
		 * posted afterwards knows nothing about that; ED is the
		 * cheapest thing here to throw away and its measured indices
		 * are already in the map. */
		if (s.armed_kind == (uint8_t)SLOT_ED &&
		    sl->t_start < s.armed_end + lead) {
			return true;
		}
		if (s.replan && could_join_armed(sl, earliest)) {
			return true;
		}
	}
	return false;
}

/*
 * Pick and arm. Rule: "run whatever starts first", with three refinements
 * from there being one radio and no timer of our own:
 *   - A TRANSMIT BREAKS A TIE and truncates a receive window still open when
 *     the transmit must be armed - truncating costs the window's tail,
 *     overrunning costs the frame.
 *   - A BACKGROUND SCAN FILLS THE GAP ahead of the next committed operation
 *     when the gap is worth an arm - never delays tracked work, and its stop
 *     is computed from what else is pending.
 *   - OTHERWISE THE RADIO IS COMMITTED EARLY, even to something far away:
 *     there's no timer of our own, so leaving the radio idle means nothing
 *     wakes up to arm it. Costs nothing for a receive, helps a transmit's
 *     jitter, and preemption handles anything nearer that turns up later.
 */
static enum step arm_next(radiant_time_t earliest)
{
	const radiant_time_t lead = arm_lead();
	int              tx_ch = -1;
	int              rx_ch = -1;
	int              scan_ch = -1;
	int              ed_ch = -1;
	radiant_time_t       tx_at = RADIANT_TIME_NEVER;
	radiant_time_t       rx_at = RADIANT_TIME_NEVER;
	radiant_time_t       scan_at = RADIANT_TIME_NEVER;
	radiant_time_t       ed_at = RADIANT_TIME_NEVER;
	radiant_time_t       committed;
	const struct radiant_pkt_format *committed_fmt;
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
		if (sl->kind == (uint8_t)SLOT_ED) {
			/* Tested BEFORE sl->continuous, which an ED slot leaves
			 * unset - falling into the bounded-receive bucket here
			 * would give a deadline-less slot a deadline, and the
			 * expiry sweep would delete it next pass. */
			if (ed_ch < 0 || at < ed_at) {
				ed_ch = (int)c;
				ed_at = at;
			}
		} else if (sl->continuous) {
			if (scan_ch < 0 || at < scan_at) {
				scan_ch = (int)c;
				scan_at = at;
			}
		} else if (rx_ch < 0 || at < rx_at) {
			rx_ch = (int)c;
			rx_at = at;
		}
	}

	if (tx_ch < 0 && rx_ch < 0 && scan_ch < 0 && ed_ch < 0) {
		return STEP_DONE;
	}

	/* WHICH operation is committed, not merely when - sizing the gap in
	 * front of it needs its format. A transmit breaks the tie, matching
	 * the arming order below. */
	committed = RADIANT_TIME_NEVER;
	committed_fmt = NULL;
	if (tx_ch >= 0) {
		committed = tx_at;
		committed_fmt = s.ch[tx_ch].fmt;
	}
	if (rx_ch >= 0 && rx_at < committed) {
		committed = rx_at;
		committed_fmt = s.ch[rx_ch].fmt;
	}

	if (scan_ch >= 0) {
		radiant_time_t s_open = t_max(scan_at, earliest);
		radiant_time_t s_limit = RADIANT_TIME_NEVER;
		uint32_t   chunk = s.ch[scan_ch].chunk_us;

		if (chunk == 0u) {
			chunk = RADIANT_SCHED_SCAN_CHUNK_US;
		}
		if (committed != RADIANT_TIME_NEVER) {
			s_limit = t_back(committed,
					 lead + phy_gap(s.ch[scan_ch].fmt,
							committed_fmt));
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

	/*
	 * Energy detect, in whatever gap the scan didn't want. Gated on
	 * scan_ch < 0: a pending scan takes the gap even if it couldn't be
	 * armed into this one (RADIANT_SCHED_SCAN_MIN_US < RADIANT_SCHED_ED_MIN_US),
	 * so "lowest priority" stays a rule rather than a function of gap
	 * length. Consequence: a searching dongle measures nothing at all -
	 * deliberate, since discovery outranks the map.
	 */
	if (SCHED_ED_BUILT && ed_ch >= 0 && scan_ch < 0) {
		radiant_time_t e_open = t_max(ed_at, earliest);
		radiant_time_t e_limit = RADIANT_TIME_NEVER;
		uint32_t   chunk = s.ch[ed_ch].chunk_us;

		if (chunk == 0u) {
			chunk = RADIANT_SCHED_ED_CHUNK_US;
		}
		if (committed != RADIANT_TIME_NEVER) {
			/* NULL: an energy-detect chunk leaves the packet
			 * configuration alone, so the switch the committed
			 * operation needs is the one it needs from here. */
			e_limit = t_back(committed,
					 lead + phy_gap(NULL, committed_fmt));
		}
		/* An unbounded gap is still bounded at the chunk ceiling here:
		 * an overrunning scan costs listening time it wanted anyway,
		 * but an overrunning ED chunk costs whatever window turns up
		 * next. */
		if (e_limit == RADIANT_TIME_NEVER ||
		    e_limit >= e_open + (radiant_time_t)RADIANT_SCHED_ED_MIN_US) {
			radiant_time_t want = e_open + (radiant_time_t)chunk;
			enum step  st;

			st = arm_ed_chunk((uint8_t)ed_ch, earliest,
					  t_min(e_limit, want));
			if (st != STEP_SKIP) {
				return st;
			}
		}
	}

	if (rx_ch >= 0 && (tx_ch < 0 || rx_at <= tx_at)) {
		radiant_time_t limit = RADIANT_TIME_NEVER;
		enum step  st;

		if (tx_ch >= 0) {
			/* Truncated so the transmit can still be armed, and, on
			 * a different PHY, so the radio can still switch between
			 * them. Truncating costs the window's tail; overrunning
			 * costs the frame. */
			limit = t_back(tx_at,
				       lead + phy_gap(s.ch[rx_ch].fmt,
						      s.ch[tx_ch].fmt));
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
 * Report and drop anything whose deadline has already gone, then either
 * preempt or arm. The expiry sweep runs even while an operation is armed,
 * deliberately: a window lost to contention is reported MISSED the moment it
 * becomes unreachable, not silently carried until the radio is free - that
 * count is exactly the number that says whether merging is working.
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
		if (sl->kind == (uint8_t)SLOT_ED) {
			/* Never expires - said explicitly rather than falling
			 * through: the receive branch below reads
			 * `!sl->continuous && sl->t_end < earliest`, and an ED
			 * slot has neither field set, so it would test
			 * 0 < earliest (true on the very first pass) and get
			 * reported MISSED and deleted before a single dwell ran. */
			expired = false;
		} else if (sl->kind == (uint8_t)SLOT_TX) {
			/* t_sync is exact and can't be moved - a transmit that
			 * can no longer hit it doesn't go out. Includes the PHY
			 * switch, unlike a receive window which can absorb a
			 * slipped open: without this a transmit needing
			 * reconfiguration it doesn't have would reach the
			 * backend, get refused ETIME, and be reported through
			 * arm_failed()'s "backend stricter than advertised" path -
			 * true about the wrong thing. MISSED here is what
			 * actually happened. */
			expired = sl->t_start < earliest + phy_lead(sl->fmt);
		} else {
			/* A receive window whose open has passed is still worth
			 * arming for the rest of its span, so only the close
			 * expires it. */
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
 * radiant_radio_now() is read once and shared by every step: a pass reasons
 * about one instant, cheaper in an ISR and simpler than a clock moving
 * underneath the decision.
 */
static void pass(void)
{
	radiant_time_t now;
	uint32_t   guard;
	unsigned int key;

	/*
	 * INTERRUPTS OFF FOR THE WHOLE PASS. See "Thread against interrupt"
	 * above. The check-and-set of in_pass must be atomic, and the table
	 * must stop moving while read: an ISR landing between "choose the
	 * leader" and "arm it" would run end_armed()/clear_armed() underneath
	 * a decision already made against the old armed state. Bounded
	 * arithmetic plus at most one arm call (the budget stated at the top
	 * of this file, enforced by fake_radio.c). Nested entry is free - the
	 * port maps this onto irq_lock()/irq_unlock(), which nest by key.
	 */
	key = radiant_event_crit_enter();
	if (!s.inited || s.in_pass) {
		radiant_event_crit_exit(key);
		return;
	}
	s.in_pass = true;
	now = radiant_radio_now();

	/* Bounded: every retry consumes a request or arms one, and there are
	 * only so many. The margin over channel count covers one window
	 * closing late enough to expire every other channel at once, each
	 * posting its next window from inside the notification. */
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
		/* A backend reporting a filter index the window didn't have.
		 * Counted rather than trusted - routing it to a channel that
		 * didn't ask for it would misattribute a frame with no way to
		 * catch it from the host side. */
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
		/* With RADIANT_RX_STOP_ON_FIRST the first accepted frame's
		 * event is the terminal one; a CRC failure doesn't count - on
		 * a 3-byte search address the matcher fires on noise several
		 * times a second. */
		if (evt->status == RADIANT_RADIO_STATUS_OK && s.armed_stop_on_first) {
			end_armed(RADIANT_SCHED_DONE_OK);
			terminal = true;
		}
		break;
	case RADIANT_RADIO_STATUS_TIMEOUT:
		/* Noise floor, taken here since this is the one place that
		 * knows both that the window ended empty and which frequency
		 * it was on. Before end_armed(), which clears s.armed_rf - a
		 * sample attributed to rf_index 0 is worse than no sample. No
		 * second eligibility test here; the backend already decided
		 * (has_noise set only on an empty terminal timeout). */
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
	case RADIANT_RADIO_STATUS_DENIED:
		/* Accepted, then never granted - distinct from ABORTED (window
		 * HAD opened, cut short): this never opened, so a continuous
		 * scan's dwell is credited nothing and a tracked channel isn't
		 * told its sensor went quiet. May arrive from a cooperative
		 * thread rather than the radio interrupt (radiant_radio_hal.h);
		 * nothing here depends on context. */
		end_armed(RADIANT_SCHED_DONE_DENIED);
		terminal = true;
		break;
	case RADIANT_RADIO_STATUS_FAILED:
	default:
		end_armed(RADIANT_SCHED_DONE_FAILED);
		terminal = true;
		break;
	}

	s.in_pass = was_in_pass;
	/* Nested inside a thread-context pass: leave the commit to the pass
	 * that owns it. s.dirty is set by anything a callback posted, and
	 * that pass's loop re-checks it, so nothing is lost. */
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
	case RADIANT_RADIO_STATUS_DENIED:
		/* The frame never went out; the air was lent. For a master
		 * this only means the slot clock must advance, which
		 * radiant_api.c does off the slot kind, not this status. */
		end_armed(RADIANT_SCHED_DONE_DENIED);
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

#if SCHED_ED_BUILT
/* One energy-detect dwell, or the end of a chunk. Fed to radiant_chanmap.c
 * here, same reason as the noise sample in hal_rx(): this is the one place
 * that knows the number and the frequency. Kept out of the backend so a
 * second backend doesn't have to agree with the first about binning. */
static void hal_ed(const struct radiant_ed_event *evt, void *user)
{
	uint8_t ch;
	bool    terminal = false;
	bool    was_in_pass;

	(void)user;

	if (evt == NULL) {
		return;
	}
	if (s.armed_kind != (uint8_t)SLOT_ED || evt->op == 0u ||
	    evt->op != s.armed_op) {
		s.stats.stale_events++;
		return;
	}

	ch = (s.n_members > 0u) ? s.members[0] : RADIANT_SCHED_CH_NONE;

	was_in_pass = s.in_pass;
	s.in_pass = true;

	switch (evt->status) {
	case RADIANT_RADIO_STATUS_OK: {
		struct sched_slot *sl;

		radiant_chanmap_note(evt->rf_index, evt->min_dbm, evt->mean_dbm,
				     evt->samples);
		s.stats.ed_dwells++;

		if (ch != RADIANT_SCHED_CH_NONE) {
			sl = &s.ch[ch];
			if (sl->kind == (uint8_t)SLOT_ED) {
				/* Past the top of the range, restart at the
				 * bottom. Advanced per dwell, see
				 * struct sched_slot::rf_cursor. */
				sl->rf_cursor = (evt->rf_index >= sl->rf_hi)
							? sl->rf_index
							: (uint8_t)(evt->rf_index + 1u);
			}
			if (s.cbs.ed != NULL) {
				s.cbs.ed(ch, evt, s.user);
			}
		}
		break;
	}
	case RADIANT_RADIO_STATUS_TIMEOUT:
		/* The range completed. TIMEOUT rather than OK, same reason a
		 * receive window reports it: the terminal event of a completed
		 * operation shouldn't look like one of its own results. */
		end_armed(RADIANT_SCHED_DONE_OK);
		terminal = true;
		break;
	case RADIANT_RADIO_STATUS_ABORTED:
		end_armed(RADIANT_SCHED_DONE_ABORTED);
		terminal = true;
		break;
	case RADIANT_RADIO_STATUS_DENIED:
		/* Chunk never ran; rf_cursor stays put so the next chunk
		 * resumes where this one would have started. end_armed()
		 * keeps the request: nothing else re-posts an ED sweep. */
		end_armed(RADIANT_SCHED_DONE_DENIED);
		terminal = true;
		break;
	case RADIANT_RADIO_STATUS_CRC_FAIL:
	case RADIANT_RADIO_STATUS_FAILED:
	default:
		end_armed(RADIANT_SCHED_DONE_FAILED);
		terminal = true;
		break;
	}

	s.in_pass = was_in_pass;
	if (!was_in_pass && (terminal || s.dirty)) {
		pass();
	}
}
#endif /* SCHED_ED_BUILT */

static const struct radiant_radio_cbs sched_radio_cbs = {
	.rx = hal_rx,
	.tx = hal_tx,
#if SCHED_ED_BUILT
	.ed = hal_ed,
#endif
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

	/* The histogram belongs to the samples this module feeds it, so it
	 * resets with this module rather than separately. */
	radiant_noise_reset();
	/* Same reasoning for the channel-quality map; no-op inline without
	 * CONFIG_RADIANT_CORE_ED_SCAN. */
	radiant_chanmap_reset();

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
	/* The radio's PHY becomes unknown again, conservatively: a backend
	 * that came back up in its reset configuration would otherwise
	 * under-lead the first operation of the next session by
	 * phy_switch_us. */
	s.phy_known = false;
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

	/* The mutation window, closed against the radio ISR: an ISR landing
	 * mid-rewrite would see a slot half old request, half new. See
	 * "Thread against interrupt" above. */
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
	sl->follow_on_us = req->follow_on_us;
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
	/* Refused here rather than passed down: a backend handed a zero-length
	 * address has no honest move (nRF's TXADDRESS would emit a
	 * well-formed frame addressed to the wrong device). Checking against
	 * fmt->addr_len, not just zero, also catches a search-format address
	 * (3 bytes) posted against the tracking format (5). */
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
	sl->follow_on_us = req->follow_on_us;
	sl->t_start = req->t_sync_at;

	s.dirty = true;
	s.replan = true;
	radiant_event_crit_exit(key);
	return RADIANT_RADIO_OK_RC;
}

int radiant_sched_request_ed(uint8_t ch, const struct radiant_sched_ed *req)
{
	struct sched_slot *sl;
	unsigned int       key;

	if (!s.inited) {
		return RADIANT_RADIO_ESTATE;
	}
	if (ch >= RADIANT_SCHED_MAX_CHANNELS || req == NULL) {
		return RADIANT_RADIO_EINVAL;
	}
	if (!SCHED_ED_BUILT) {
		return RADIANT_RADIO_ENOTSUP;
	}
	if (req->rf_index_hi < req->rf_index_lo ||
	    req->rf_index_hi > RADIANT_RF_INDEX_MAX) {
		return RADIANT_RADIO_EINVAL;
	}
	/* Refused here rather than reported through done() - the opposite of
	 * radiant_sched_request_rx()'s unmatchable filter set, because that's
	 * a property of the request while this is a property of the build. A
	 * slot no backend on this image will ever accept would otherwise be
	 * scanned and fail on every pass forever. */
	if (s.caps == NULL || !s.caps->has_ed_scan) {
		return RADIANT_RADIO_ENOTSUP;
	}

	/* The mutation window. See radiant_sched_request_rx(). */
	key = radiant_event_crit_enter();

	drop_slot(ch);

	sl = &s.ch[ch];
	sl->kind = (uint8_t)SLOT_ED;
	sl->rf_index = req->rf_index_lo;
	sl->rf_hi = req->rf_index_hi;
	sl->rf_cursor = req->rf_index_lo;
	sl->dwell_us = req->dwell_us;
	sl->chunk_us = req->chunk_us;
	/* Ready now and always. See struct radiant_sched_ed on why there is
	 * no instant in the request. */
	sl->t_start = 0u;
	sl->t_end = 0u;

	s.dirty = true;
	/* s.replan deliberately NOT set, unlike every other post:
	 * could_join_armed() requires both sides SLOT_RX, so it can never
	 * say yes to an ED slot - setting the flag would only buy one wasted
	 * rebuild attempt. */
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
	/* Abort happens here rather than at the next commit, since cancelling
	 * means the radio stops doing something now. Choosing what runs
	 * instead is still the commit's job. */
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
	/* Only a continuous request has a chunk, and only shorter is allowed
	 * (see the header). An in-flight chunk keeps its armed length; this
	 * bounds the NEXT one. */
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
