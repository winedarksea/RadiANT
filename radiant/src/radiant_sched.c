/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sched.c - the radio schedule.
 *
 * Clean-room: written from radiant_radio_hal.h and radiant_sched.h against the
 * mock in tests/fake_radio.c; the two bounded constants come from the bench
 * measurements in docs/ant-radio-link.md and docs/spike-b-results.md.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * Shape: a table of 32 slots, each holding at most one intent, and pass() -
 * which looks at the table and the clock and decides what the radio does
 * next. pass() runs from exactly two places: the tail of a radio callback
 * (the low-jitter re-arm path the HAL's callback contract permits), and
 * radiant_sched_tick(), the commit point for anything posted from thread
 * context. POSTING A REQUEST DOES NOT ARM ANYTHING - that's what makes
 * merging work: re-planning on every mutation would arm the first channel's
 * window the instant it posted, and a second channel whose window overlaps
 * it exactly would arrive to find the radio already committed and merge with
 * nothing. Requests are collected and committed once; in steady state every
 * channel posts its next window from inside the last one's terminal
 * callback, so the tail pass sees them all together. A window that's armed
 * but not yet open can be torn down and rebuilt for free, so a late-opening
 * channel still joins the merge on the next tick instead of waiting a period
 * (see want_preempt()). A pass costs one radiant_radio_now() and at most one
 * arm call regardless of table state - fake_radio.c flags a contract
 * violation past sixteen HAL calls per callback; the slot scans are pure
 * arithmetic, no hardware.
 *
 * Abort ordering (the subtle part): an aborted operation's terminal event is
 * STILL DELIVERED (radiant_radio_hal.h), and the op id lets a late event from
 * a cancelled operation be recognised. This file always retires the live
 * operation (clears s.armed_op) BEFORE calling radiant_radio_abort(), and
 * every event handler's first act is comparing the event's op id against
 * s.armed_op and dropping mismatches. Get the order wrong and the ABORTED
 * event of the operation you just replaced is taken for the terminal event of
 * its replacement, which is then retired while its window is still open -
 * radio left listening for nobody. This happens on every preemption with no
 * fault injection, so s.stats.stale_events is normally non-zero, not an error
 * count.
 *
 * Thread against interrupt: `s` has exactly two writers - thread context via
 * the three request entry points and radiant_sched_tick(), and the radio ISR
 * via hal_rx()/hal_tx(). A plain bool `in_pass`, unconditionally cleared on
 * the way out of a callback, would let an ISR landing on top of a
 * thread-context pass() tell the thread, on return, that no pass was running
 * - not rare: a master's EVENT_TX pumps a pass in thread context, and the
 * slave's reply lands 2.19 ms later, inside that window, every exchange. Two
 * fixes, both needed: in_pass is SAVED AND RESTORED in hal_rx()/hal_tx(), so
 * a nested entry defers the commit to the pass that owns it; and every write
 * to the slot table, and all of pass(), runs with interrupts off via
 * radiant_event_crit_enter()/_exit() (same primitive radiant_event.c uses for
 * its ring - the ISR side must not block). This deliberately does not take a
 * lock the HAL has to know about - backends call these callbacks from their
 * own interrupt, and the exclusion stays entirely on this side.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <radiant/radiant_chanmap.h>
#include <radiant/radiant_diag.h>
#include <radiant/radiant_frame.h>
#include <radiant/radiant_noise.h>
#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_sched.h>
/* For radiant_event_crit_enter()/_exit() only - the port's irq_lock() under a
 * neutral name; nothing here queues an event. See "Thread against interrupt"
 * above. */
#include <radiant/radiant_event.h>

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------
 */

/* Whether the energy-detect slot kind is compiled at all. Plain 0/1 macro,
 * not IS_ENABLED() (this file includes no Zephyr header by design). ED
 * branches read `if (SCHED_ED_BUILT && ...)` so the compiler folds it away
 * rather than the preprocessor forking the file into two shapes. */
#if defined(CONFIG_RADIANT_ED_SCAN)
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
	/* A backend callback (hal_rx/hal_tx/hal_ed) is running, including the
	 * pass it ends with. The only context a follow-on reserve can be used
	 * from - see in_reserve(). Saved and restored like in_pass. */
	bool in_radio_cb;
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

	/* The last instant the most recently armed operation's follow_on_us
	 * reserve covers, or 0 for none. Outlives that operation on purpose: the
	 * reserve is FOR whatever its completion arms. But only a completion:
	 * a non-OK terminal, an abort outside a callback or a reset clears it,
	 * since the grant it describes is gone. See in_reserve(). */
	radiant_time_t               reserve_end;

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

#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	/*
	 * ADR 0018 (d2): committed operations booked with the backend
	 * (radiant_radio_book()), so they may be armed later at the in-grant
	 * lead. Keyed by the booked air, not by channel: a merged window's
	 * members are covered by their leader's booking. Pruned by time.
	 */
	struct {
		bool                             used;
		uint8_t                          kind; /* SLOT_RX or SLOT_TX */
		uint8_t                          rf;
		uint8_t                          ch;   /* TX only */
		const struct radiant_pkt_format *fmt;
		radiant_time_t                   open;
		radiant_time_t                   close;
		/* Where this booking's air ends, if it asked for a scan tail
		 * (ADR 0021 decision 5); 0 if it did not. */
		radiant_time_t                   tail_end;
		/* Phase 3c R1 join: where the air ends once the committed
		 * operations joined to it are covered, or 0 if none is. `ch` is
		 * then the operation the request is for. */
		radiant_time_t                   air_end;
	} bk[RADIANT_SCHED_BOOK_MAX];
	/* Where the last fixed-class operation's air ends, follow-on reserve
	 * included (close + follow_on_us). A scan chunk placed in the other
	 * session must not start before the arbiter has that timeslot ending:
	 * see arm_next(). */
	radiant_time_t fix_end;
	/* The end of the armed operation's booked tail, or 0: while it is set
	 * and the operation completes, a scan chunk can be armed into the air
	 * still held, at the in-grant lead. */
	radiant_time_t tail_end;
	/* The tail's dither. Any cheap sequence: what matters is that the gap
	 * left after a tail is not at one phase of the tracked period. */
	uint32_t tail_seq;
#endif

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
 * channel happened to lead. radiant_sched_init() refuses a zero.
 */
static uint8_t max_addr_groups(void)
{
	uint8_t mg;

	if (s.caps == NULL) {
		return 1u;
	}
	mg = s.caps->max_addr_groups;
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

/* The lead inside air a previous operation already holds. 0 in the caps means
 * "same as min_arm_lead_us", the value every backend reported before the
 * field existed. */
static radiant_time_t in_grant_lead(void)
{
	if (s.caps == NULL) {
		return 0u;
	}
	return (radiant_time_t)((s.caps->min_arm_lead_in_grant_us != 0u)
					? s.caps->min_arm_lead_in_grant_us
					: s.caps->min_arm_lead_us);
}

/*
 * Does this request fall inside the follow-on reserve the last armed operation
 * asked the backend to hold? Then it is the follow-on that reserve exists for -
 * an ACK reply, the ACK window after a data packet, a slave's queued command -
 * and needs only the in-grant lead, not a fresh reservation's.
 *
 * Without this, an arbitrated backend's follow-ons never reached it: every one
 * is 1.5-2.2 ms out against a min_arm_lead_us of 24 ms+, so pass_step()
 * reported it MISSED and the air it had reserved went unused on every exchange
 * (measured: 60 of 60 acknowledged commands failed on an MPSL build that
 * completed 60 of 60 on the direct one).
 *
 * Only the END is tested, as the backend's own check does; the backend
 * refuses honestly (EDENIED) if the air turns out not to be held. Never true
 * on a backend whose two leads coincide, so the schedule there is unchanged.
 *
 * Only from inside a backend callback. The nRF backend holds a grant past its
 * operation only while the core's completion is still running (THE GRANT
 * HANDOFF in radiant_radio_nrf.c) and gives it back when that dispatch
 * drains, so from thread context there is never held air to arm into: a
 * request there inside an old reserve would be armed at the in-grant lead
 * only to be refused, EDENIED where the truth is MISSED, and a bounded window
 * that could still have opened at the ordinary floor lost outright.
 */
static bool in_reserve(const struct sched_slot *sl)
{
	radiant_time_t last;

	if (!s.in_radio_cb || s.reserve_end == 0u ||
	    in_grant_lead() >= arm_lead()) {
		return false;
	}
	if (sl->kind == (uint8_t)SLOT_TX) {
		last = sl->t_start;
	} else if (sl->kind == (uint8_t)SLOT_RX && !sl->continuous) {
		last = sl->t_end;
	} else {
		return false;
	}
	return last <= s.reserve_end;
}

#if defined(CONFIG_RADIANT_SCHED_BOOKING)
/*
 * ADR 0018 (d2). A booking backend (caps.booking) places the air for a
 * committed operation a lead ahead, while another operation (normally a scan
 * chunk) is armed; the booked operation is then armed at the in-grant lead,
 * and the chunk in front of it stops RADIANT_SCHED_BOOK_GAP_US before it
 * instead of a whole min_arm_lead_us. Scan chunks are placed at
 * caps.min_arm_lead_elastic_us. Inert unless caps.booking is set.
 * docs/adr18-eval/d2.md.
 */
static radiant_time_t t_max(radiant_time_t a, radiant_time_t b);
static radiant_time_t t_min(radiant_time_t a, radiant_time_t b);
static radiant_time_t t_fwd(radiant_time_t t, radiant_time_t d);

static bool booking(void)
{
	return s.caps != NULL && s.caps->booking;
}

static radiant_time_t el_lead(void)
{
	radiant_time_t l = (s.caps != NULL)
				   ? (radiant_time_t)s.caps->min_arm_lead_elastic_us
				   : 0u;

	return (l != 0u) ? l : arm_lead();
}

/* The floor for arming a booked operation (see RADIANT_SCHED_BOOK_ARM_US). */
static radiant_time_t book_lead(void)
{
	return in_grant_lead() + (radiant_time_t)RADIANT_SCHED_BOOK_ARM_US;
}

/* A bounded operation: a receive window that is not a scan, or a transmit. */
static bool slot_bounded(const struct sched_slot *sl)
{
	return (sl->kind == (uint8_t)SLOT_RX && !sl->continuous) ||
	       sl->kind == (uint8_t)SLOT_TX;
}

/* Where a bounded operation's air ends: its close (a transmit's instant) plus
 * the follow-on reserve it asks to have held. */
static radiant_time_t slot_air_end(const struct sched_slot *sl)
{
	const radiant_time_t last =
		(sl->kind == (uint8_t)SLOT_TX) ? sl->t_start : sl->t_end;

	return last + (radiant_time_t)sl->follow_on_us;
}

static bool is_booked(const struct sched_slot *sl)
{
	const uint8_t idx = (uint8_t)(sl - &s.ch[0]);

	if (!booking()) {
		return false;
	}
	for (uint32_t i = 0u; i < RADIANT_SCHED_BOOK_MAX; i++) {
		if (!s.bk[i].used) {
			continue;
		}
		/* Joined to another operation's request (phase 3c): armed into
		 * that grant from its completion, at the booked lead. */
		if (s.bk[i].air_end != 0u && s.bk[i].ch != idx && slot_bounded(sl) &&
		    sl->t_start > s.bk[i].open && slot_air_end(sl) <= s.bk[i].air_end) {
			return true;
		}
		if (s.bk[i].kind != sl->kind) {
			continue;
		}
		if (sl->kind == (uint8_t)SLOT_RX && !sl->continuous &&
		    s.bk[i].fmt == sl->fmt && s.bk[i].rf == sl->rf_index &&
		    sl->t_start >= s.bk[i].open && sl->t_end <= s.bk[i].close) {
			return true;
		}
		if (sl->kind == (uint8_t)SLOT_TX && s.bk[i].ch == idx &&
		    s.bk[i].open == sl->t_start) {
			return true;
		}
	}
	return false;
}

/* Forget bookings whose air is over, joined air included. */
static void bk_prune(radiant_time_t now)
{
	for (uint32_t i = 0u; i < RADIANT_SCHED_BOOK_MAX; i++) {
		if (s.bk[i].used && t_max(s.bk[i].close, s.bk[i].air_end) < now) {
			s.bk[i].used = false;
		}
	}
}

/*
 * The operation a joined request was for did not get its grant (denied,
 * aborted, failed), so nothing will carry its joiners: they are no longer
 * covered, and are booked or armed on their own from here. The booking keeps
 * its own air.
 */
static void bk_drop_join(uint8_t ch)
{
	for (uint32_t i = 0u; i < RADIANT_SCHED_BOOK_MAX; i++) {
		if (s.bk[i].used && s.bk[i].air_end != 0u && s.bk[i].ch == ch) {
			s.bk[i].air_end = 0u;
		}
	}
}

/*
 * PHASE 3C, R1 JOIN. Where the air an operation opening at `open` should end,
 * given that it holds up to `air_end` for itself: past every committed
 * operation behind it that would start within RADIANT_SCHED_JOIN_GAP_US of that
 * air, since a request of its own would overlap this one's and the arbiter
 * would refuse one of the two. Chains: a joined operation's air is covered too,
 * so a third behind it joins as well. `air_end` back unchanged if none joins.
 */
static radiant_time_t join_end(uint8_t c, radiant_time_t open, radiant_time_t air_end)
{
	radiant_time_t end = air_end;
	bool           grew = true;

	while (grew) {
		grew = false;
		for (uint32_t k = 0u; k < RADIANT_SCHED_MAX_CHANNELS; k++) {
			const struct sched_slot *m = &s.ch[k];
			radiant_time_t           m_end;

			if ((uint8_t)k == c || !slot_bounded(m) || m->in_flight ||
			    m->t_start <= open ||
			    m->t_start >= t_fwd(end, RADIANT_SCHED_JOIN_GAP_US)) {
				continue;
			}
			m_end = t_fwd(slot_air_end(m), RADIANT_SCHED_JOIN_SLACK_US);
			if (m_end <= end || m_end - open > RADIANT_SCHED_JOIN_MAX_US) {
				continue;
			}
			end = m_end;
			grew = true;
		}
	}
	return end;
}

/*
 * Put off booking channel c while another bounded operation that started, or
 * starts, close to now is still in flight or committed: c has usually just
 * completed, that neighbour is next, and ITS next slot is posted only when it
 * completes. Booking c now would make c's request before the neighbour's next
 * slot is known, so the neighbour could not join it and its own request would
 * be refused. The neighbour's completion books c instead, a few milliseconds
 * later and still most of a period ahead. Never for an operation that is itself
 * close.
 */
static bool join_defer(uint8_t c, radiant_time_t now)
{
	const struct sched_slot *cs = &s.ch[c];

	if (cs->t_start <= t_fwd(now, 3u * RADIANT_SCHED_JOIN_DEFER_US)) {
		return false;
	}
	for (uint32_t k = 0u; k < RADIANT_SCHED_MAX_CHANNELS; k++) {
		const struct sched_slot *m = &s.ch[k];

		if ((uint8_t)k == c || !slot_bounded(m) || slot_air_end(m) < now ||
		    m->t_start > t_fwd(now, RADIANT_SCHED_JOIN_DEFER_US)) {
			continue;
		}
		return true;
	}
	return false;
}

/* True if anything is searching: a tail is air nobody else would use. */
static bool scan_running(void)
{
	for (uint32_t k = 0u; k < RADIANT_SCHED_MAX_CHANNELS; k++) {
		if (s.ch[k].kind == (uint8_t)SLOT_RX && s.ch[k].continuous) {
			return true;
		}
	}
	return false;
}

/*
 * ADR 0021 decision 5: the air to ask for past a booking's follow-on, or 0.
 *
 * Dithered between half and one and a half times the configured length, so the
 * deaf gap a tail leaves after it moves from period to period.
 *
 * Never when another committed operation is close. The arbiter is first come,
 * first served: a tail over a neighbour's instant makes the neighbour's booking
 * the one refused, which would cost a tracked slot for the sake of listening.
 * Tested twice: against the others' known instants, and against where they are
 * NOW, a period before the booked air, since a neighbour's next slot is usually
 * not posted yet and ANT+ channels repeat, so one close to this slot now is
 * close to the next one too. Both add the lead, because the neighbour's own
 * grant needs placing clear of the tail.
 */
static uint16_t scan_tail_for(uint8_t c, radiant_time_t now, uint16_t fo)
{
	const uint32_t base = (s.caps != NULL) ? s.caps->scan_tail_us : 0u;
	uint32_t       tail;
	radiant_time_t reach;
	radiant_time_t own;

	if (base == 0u || !scan_running()) {
		return 0u;
	}
	s.tail_seq = s.tail_seq * 1664525u + 1013904223u;
	tail = base / 2u + ((s.tail_seq >> 8) % base);
	reach = now + (radiant_time_t)fo + (radiant_time_t)tail + arm_lead();
	own = s.ch[c].t_end + (radiant_time_t)fo + (radiant_time_t)tail + arm_lead();
	for (uint32_t k = 0u; k < RADIANT_SCHED_MAX_CHANNELS; k++) {
		const struct sched_slot *m = &s.ch[k];

		if (k == c || m->continuous ||
		    (m->kind != (uint8_t)SLOT_RX && m->kind != (uint8_t)SLOT_TX)) {
			continue;
		}
		if ((m->t_start <= reach && m->t_start + arm_lead() >= now) ||
		    (m->t_start <= own && m->t_start + arm_lead() >= s.ch[c].t_start)) {
			return 0u;
		}
	}
	return (uint16_t)tail;
}

/* The tail end of the booking this armed window lies in, or 0. */
static radiant_time_t booked_tail_end(const struct sched_slot *sl, radiant_time_t open,
				      radiant_time_t close)
{
	for (uint32_t i = 0u; i < RADIANT_SCHED_BOOK_MAX; i++) {
		if (s.bk[i].used && s.bk[i].kind == (uint8_t)SLOT_RX &&
		    s.bk[i].fmt == sl->fmt && s.bk[i].rf == sl->rf_index &&
		    open >= s.bk[i].open && close <= s.bk[i].close) {
			return s.bk[i].tail_end;
		}
	}
	return 0u;
}

/* The joined air of the booking this armed window lies in, or 0. */
static radiant_time_t booked_air_end(const struct sched_slot *sl, radiant_time_t open,
				     radiant_time_t close)
{
	for (uint32_t i = 0u; i < RADIANT_SCHED_BOOK_MAX; i++) {
		if (s.bk[i].used && s.bk[i].kind == (uint8_t)SLOT_RX &&
		    s.bk[i].fmt == sl->fmt && s.bk[i].rf == sl->rf_index &&
		    open >= s.bk[i].open && close <= s.bk[i].close) {
			return s.bk[i].air_end;
		}
	}
	return 0u;
}

/*
 * Book slot c with the backend: the air exactly as the arm would ask for it,
 * widened to whatever bounded receive could merge into its window (rule 4's
 * span, the largest follow-on among them), so the later arm lies inside the
 * booking whichever members join. A bounded receive or a transmit only.
 */
static int book_slot(uint8_t c)
{
	struct sched_slot *sl = &s.ch[c];
	uint32_t           free_i = RADIANT_SCHED_BOOK_MAX;
	radiant_time_t     open;
	radiant_time_t     close;
	radiant_time_t     tail_end = 0u;
	radiant_time_t     join_air = 0u;
	int                rc;

	for (uint32_t i = 0u; i < RADIANT_SCHED_BOOK_MAX; i++) {
		if (!s.bk[i].used) {
			free_i = i;
			break;
		}
	}
	if (free_i >= RADIANT_SCHED_BOOK_MAX) {
		s.stats.book_busy++;
		return RADIANT_RADIO_EBUSY;
	}
	if (sl->kind == (uint8_t)SLOT_RX && !sl->continuous) {
		struct radiant_rx_req req;
		uint16_t              fo = sl->follow_on_us;
		radiant_time_t        span_end =
			sl->t_start + (radiant_time_t)RADIANT_SCHED_MERGE_SPAN_MAX_US;

		open = sl->t_start;
		close = sl->t_end;
		for (uint32_t k = 0u; k < RADIANT_SCHED_MAX_CHANNELS; k++) {
			const struct sched_slot *m = &s.ch[k];

			if (k == c || m->kind != (uint8_t)SLOT_RX || m->continuous ||
			    m->in_flight || m->fmt != sl->fmt ||
			    m->rf_index != sl->rf_index ||
			    m->t_start < sl->t_start || m->t_start > span_end) {
				continue;
			}
			close = t_max(close, t_min(m->t_end, span_end));
			if (m->follow_on_us > fo) {
				fo = m->follow_on_us;
			}
		}
		memset(&req, 0, sizeof(req));
		req.fmt = sl->fmt;
		req.rf_index = sl->rf_index;
		req.filters = sl->filters;
		req.n_filters = sl->n_filters;
		req.t_open = open;
		req.t_close = close;
		/* Phase 3c: a committed operation close behind joins this
		 * booking as its tail, so it is carried by this grant rather than
		 * refused by the arbiter. A joined booking asks no scan tail: the
		 * joiner is what follows it. The gate places a refused tailed
		 * booking again at plain length, so a refusal costs the joiner,
		 * never this slot. */
		join_air = join_end(c, open, close + (radiant_time_t)fo);
		if (join_air > close + (radiant_time_t)fo) {
			req.follow_on_us = fo;
			req.book_tail_us =
				(uint16_t)(join_air - (close + (radiant_time_t)fo));
		} else {
			const uint16_t tail = scan_tail_for(c, radiant_radio_now(), fo);

			join_air = 0u;
			req.follow_on_us = fo;
			req.book_tail_us = tail;
			if (tail != 0u) {
				tail_end = close + (radiant_time_t)fo +
					   (radiant_time_t)tail;
			}
		}
		rc = radiant_radio_book(&req, NULL);
	} else if (sl->kind == (uint8_t)SLOT_TX) {
		struct radiant_tx_req req;

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
		open = sl->t_start;
		close = sl->t_start;
		rc = radiant_radio_book(NULL, &req);
	} else {
		return RADIANT_RADIO_EINVAL;
	}
	if (rc == RADIANT_RADIO_OK_RC) {
		s.bk[free_i].used = true;
		s.bk[free_i].kind = sl->kind;
		s.bk[free_i].rf = sl->rf_index;
		s.bk[free_i].ch = c;
		s.bk[free_i].fmt = sl->fmt;
		s.bk[free_i].open = open;
		s.bk[free_i].close = close;
		s.bk[free_i].tail_end = tail_end;
		s.bk[free_i].air_end = join_air;
		s.stats.booked++;
		if (join_air != 0u) {
			s.stats.join++;
		}
	} else if (rc == RADIANT_RADIO_EBUSY) {
		s.stats.book_busy++;
	} else {
		s.stats.book_refused++;
	}
	return rc;
}

/* The committed (bounded, not in flight) operation that starts first after
 * `after`, or -1. Ties go to the lower index; skip_ch is excluded. */
static int next_committed(radiant_time_t after, int skip_ch)
{
	int            best = -1;
	radiant_time_t best_at = RADIANT_TIME_NEVER;

	for (uint32_t k = 0u; k < RADIANT_SCHED_MAX_CHANNELS; k++) {
		const struct sched_slot *sl = &s.ch[k];

		if ((int)k == skip_ch || sl->in_flight ||
		    !((sl->kind == (uint8_t)SLOT_RX && !sl->continuous) ||
		      sl->kind == (uint8_t)SLOT_TX)) {
			continue;
		}
		if (sl->t_start < after || sl->t_start >= best_at) {
			continue;
		}
		best = (int)k;
		best_at = sl->t_start;
	}
	return best;
}

/*
 * After a pass: book the committed operations that follow, earliest first,
 * while the backend has room. This is what keeps an R1 pair (two tracked
 * channels 2.6-24.4 ms apart) on both slots: each is booked a period ahead,
 * in the session the other's grant just freed.
 *
 * An operation put off by join_defer() is skipped, not the end of the walk
 * (phase-4-fix.md, F1): later ones are still booked, except any starting inside
 * the air the deferred one may yet ask for (its joined reach). Booking one of
 * those first would place a later request over the earlier one's air, and the
 * arbiter would refuse the earlier one.
 */
static void book_ahead(radiant_time_t now)
{
	radiant_time_t after = t_fwd(now, arm_lead());
	radiant_time_t hold = 0u;
	uint32_t       made = 0u;

	if (!booking()) {
		return;
	}
	while (made < 2u) {
		const int c = next_committed(after, -1);

		if (c < 0) {
			return;
		}
		after = s.ch[c].t_start + 1u;
		if (is_booked(&s.ch[c]) || (hold != 0u && s.ch[c].t_start < hold)) {
			continue;
		}
		if (join_defer((uint8_t)c, now)) {
			s.stats.join_defer++;
			hold = t_fwd(s.ch[c].t_start,
				     RADIANT_SCHED_JOIN_MAX_US + RADIANT_SCHED_JOIN_GAP_US);
			continue;
		}
		if (book_slot((uint8_t)c) != RADIANT_RADIO_OK_RC) {
			return;
		}
		made++;
	}
}
#endif /* CONFIG_RADIANT_SCHED_BOOKING */

/* The earliest this slot can be armed, given the pass's ordinary floor
 * (now + arm_lead()): nearer for a follow-on inside the held reserve. */
static radiant_time_t slot_earliest(const struct sched_slot *sl,
				    radiant_time_t earliest)
{
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	/* A booked operation is armed into air already placed; a scan chunk
	 * at the elastic lead. */
	if (booking()) {
		if (is_booked(sl)) {
			return earliest - arm_lead() + book_lead();
		}
		if (sl->kind == (uint8_t)SLOT_RX && sl->continuous) {
			return earliest - arm_lead() + el_lead();
		}
	}
#endif
	return in_reserve(sl) ? (earliest - arm_lead() + in_grant_lead())
			      : earliest;
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

/*
 * How long a receive window in this format holds the radio PAST ITS OWN
 * t_close. See radiant_frame_tail_us(): t_close bounds a frame's t_sync, and
 * t_sync is the end of the address, so the body and CRC of a frame arriving at
 * the last legal instant are still to come.
 *
 * Charged to whatever fills a gap in front of a committed operation, on top of
 * arm_lead(). Leaving only the lead is a defect that costs the committed
 * window entirely, and it is invisible at one channel - see arm_next().
 */
static radiant_time_t rx_tail(const struct radiant_pkt_format *fmt)
{
	if (fmt == NULL) {
		/* An energy-detect chunk receives no frame, so it has neither
		 * the airtime tail nor the backend's receiver hold. */
		return 0u;
	}
	return (radiant_time_t)radiant_frame_tail_us(fmt) +
	       ((s.caps == NULL) ? 0u
				 : (radiant_time_t)s.caps->rx_close_hold_us);
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

/* t + d, saturated at RADIANT_TIME_NEVER. The merge-reach tests below widen a
 * window edge by arm_lead(), and one of those edges is legitimately
 * RADIANT_TIME_NEVER (a continuous request has no close): plain addition would
 * wrap UINT64_MAX to a tiny number and turn "no deadline" into "already over". */
static radiant_time_t t_fwd(radiant_time_t t, radiant_time_t d)
{
	return (RADIANT_TIME_NEVER - t < d) ? RADIANT_TIME_NEVER : (t + d);
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
	/* The reserve outlives only an operation that completed: one denied,
	 * aborted or failed holds no grant (see in_reserve()). Cleared before
	 * the owners are told, since their done() may post the next. */
	if (why != RADIANT_SCHED_DONE_OK) {
		s.reserve_end = 0u;
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
		s.tail_end = 0u;
		/* No grant, so nothing carries its joiners. */
		if (n > 0u) {
			bk_drop_join(list[0]);
		}
#endif
	}

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
	/* Outside a callback the abort gives the grant straight back, so the
	 * reserve goes with it. Inside one it is the handoff: the backend owes
	 * the release until the dispatch drains, and the follow-on this abort
	 * makes room for is exactly what the reserve is for. */
	if (!s.in_radio_cb) {
		s.reserve_end = 0u;
	}
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	/* An aborted operation's air is not there to arm a chunk into, nor to
	 * carry what joined it. */
	s.tail_end = 0u;
	if (n > 0u && !s.in_radio_cb) {
		bk_drop_join(list[0]);
	}
#endif

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
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	/* A consumed refusal: nothing will carry whatever joined this one. */
	if (rc != RADIANT_RADIO_EBUSY && rc != RADIANT_RADIO_ESTATE) {
		bk_drop_join(ch);
	}
#endif
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
		/* The pass subtracts min_arm_lead_us itself, so this is not
		 * the usual way to be late, but it is reachable: since A14-1
		 * the nRF backend returns it when the start compare was
		 * written too late (radiant_dbg_start_late, expected 0 or
		 * single digits). Report missed and move on. */
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
 *   3. EVERY MEMBER MUST LIE WITHIN THE LEADER'S OWN WINDOW EXTENDED BY THE
 *      MERGE REACH, not merely within the union so far - reaching from the
 *      union would let a chain of barely-touching windows walk the merged
 *      span arbitrarily far.
 *
 *      THE REACH IS arm_lead(), AND THAT IS THE WHOLE POINT OF THE RULE.
 *      Below that distance a second window is not merely inconvenient to arm,
 *      it is physically impossible: pass_step() reports a window DONE_MISSED
 *      once t_end < now + arm_lead, so two windows closer together than the
 *      lead can never be armed in sequence. Merging is then the ONLY way to
 *      hear both, which is exactly when this rule must say yes.
 *
 *      It used to require literal overlap, which reached only 2x the tracked
 *      guard (radiant_channel_guard_us(), 100 us once locked) - about 200 us.
 *      Everything between that and arm_lead was a BAD BAND: two tracked
 *      channels landing there could neither merge nor be armed in sequence,
 *      so the later one was dropped deterministically every period until the
 *      two crystals drifted apart. Invisible at one channel, which is why
 *      every loss figure this project recorded missed it.
 *
 *      WHAT IT COSTS: the merged window now spans the dead gap between the
 *      pair rather than only their overlap, so up to arm_lead of extra
 *      receive per merged pair per period - about 0.07% duty at 4 Hz.
 *      Receive current, no airtime, and rule 4 still bounds the total.
 *
 *      WHAT IT DOES NOT FIX, stated so it is not rediscovered: rule 5 caps a
 *      window at caps.max_addr_groups distinct device numbers, which is TWO
 *      on nRF (radiant_radio_nrf.c's max_addr_groups). This rescues PAIRS -
 *      which is precisely the pairwise bad band - but three tracked channels
 *      piled inside one lead still cost the third its window.
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
	/* Rule 3's reach. Read once, so every member is judged against the same
	 * radius the pass that called us subtracted from now(). */
	const radiant_time_t reach = arm_lead();
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

		/*
		 * The leader's phy_lead, not none, and rule 1 is what makes
		 * that exact rather than approximate: every member is on the
		 * leader's PHY by pointer equality, so there is one
		 * reconfiguration for the window and the union must not open
		 * inside it. It reads as a no-op today because phy_lead() is
		 * zero whenever the radio is already on that PHY, which is
		 * always on a single-PHY image - but rule 3's reach below now
		 * lets a member sit ENTIRELY EARLIER than the leader, so this
		 * is the first version in which m_open can actually become the
		 * union's open. An arm inside the reconfiguration lead is an
		 * ETIME refusal, which arm_failed() reports as "the backend is
		 * stricter than advertised" - true about the wrong thing.
		 */
		m_open = t_max(m->t_start, earliest + phy_lead(lead->fmt));
		m_close = m->continuous ? cap_close : m->t_end;
		if (m_close < m_open) {
			continue;
		}
		/*
		 * Rule 3: within the leader's own window extended by the merge
		 * reach, not within the union so far. See the reach's reasoning
		 * in this function's header - it is arm_lead() because that is
		 * the distance inside which a second arm is impossible.
		 *
		 * Widened by addition on both sides rather than by subtracting
		 * from the other, so nothing has to be floored at zero on a
		 * timebase a test may start near it. t_fwd() rather than `+`
		 * because could_join_armed() mirrors this test against a close
		 * that really can be RADIANT_TIME_NEVER, and the two must not
		 * differ in how they saturate.
		 */
		if (m_open > t_fwd(lead_close, reach) ||
		    t_fwd(m_close, reach) < lead_open) {
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
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	/* ADR 0018 (d2): a scan chunk is the elastic class whatever its length,
	 * so a booking backend places it at the elastic lead. */
	if (lead->continuous && booking()) {
		flags |= RADIANT_RX_ELASTIC;
	}
	/*
	 * No R1 join for a window armed as a FRESH request (phase-4-fix.md, F2):
	 * a refused request that carried a joiner would cost this window too,
	 * and only a booking is placed again at plain length. A bounded window
	 * is booked before it is armed (arm_rx_leader()), and the booking
	 * carries the joiner; one armed fresh asks for its own air only.
	 */
#endif

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
	s.reserve_end = (follow_on != 0u) ? close + (radiant_time_t)follow_on : 0u;
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	s.tail_end = 0u;
	if (!lead->continuous) {
		s.fix_end = close + (radiant_time_t)follow_on;
		s.tail_end = booked_tail_end(lead, open, close);
		if (s.tail_end != 0u) {
			s.fix_end = t_max(s.fix_end, s.tail_end);
		}
		/* A booking carrying joiners holds its air to their end; no scan
		 * chunk opens before that timeslot has ended either. */
		s.fix_end = t_max(s.fix_end, booked_air_end(lead, open, close));
	}
#endif
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

/* arm_rx_window() for a bounded leader, at the floor in_reserve() allows. A
 * follow-on inside the held reserve opens at the in-grant lead, and no member
 * may carry the union past the reserve: the backend would refuse the whole
 * window, the follow-on with it. */
static enum step arm_rx_leader(uint8_t leader, radiant_time_t earliest,
			       radiant_time_t limit)
{
	const struct sched_slot *sl = &s.ch[leader];

#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	/*
	 * Phase 4, F1: a bounded window about to be armed as a fresh request is
	 * booked first and armed into its booking. With no scan running the
	 * radio is committed early, to the next window even a period off, and
	 * that window would otherwise never be booked: in flight, book_ahead()
	 * skips it. The booking joins the neighbour close behind it, which has
	 * posted its next slot by now (an operation is armed only when it is the
	 * earliest committed one), so no deferral is needed here. A window too
	 * near to book, or with no session free, is armed fresh at its own
	 * length; nothing armed fresh carries a joiner (F2).
	 */
	if (booking() && !is_booked(sl) && !in_reserve(sl) && sl->t_start >= earliest) {
		(void)book_slot(leader);
	}
	if (is_booked(sl)) {
		return arm_rx_window(leader, slot_earliest(sl, earliest), limit);
	}
#endif
	if (in_reserve(sl)) {
		return arm_rx_window(leader, slot_earliest(sl, earliest),
				     t_min(limit, s.reserve_end));
	}
	return arm_rx_window(leader, earliest, limit);
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
	s.reserve_end = (sl->follow_on_us != 0u)
				? sl->t_start + (radiant_time_t)sl->follow_on_us
				: 0u;
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	s.tail_end = 0u;
#endif
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
	s.reserve_end = 0u;
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
	/*
	 * Rule 3, mirrored, MERGE REACH INCLUDED. The two must agree exactly:
	 * if this said "no" where the rebuild would say "yes" a joinable
	 * channel would never trigger the rebuild that would take it, and if it
	 * said "yes" where the rebuild says "no" the request stays pending and
	 * asks again - which is the one-wasted-rebuild-per-request case s.replan
	 * bounds, now paid on every tracked pair inside a lead of each other
	 * rather than never.
	 */
	o = t_max(sl->t_start, earliest);
	c = sl->continuous ? RADIANT_TIME_NEVER : sl->t_end;
	return !(o > t_fwd(s.armed_end, arm_lead()) ||
		 t_fwd(c, arm_lead()) < s.armed_open);
}

/*
 * Displace the live operation? Only for a deadline that can't survive
 * waiting, or a rebuild that costs nothing.
 *
 *   - A TRANSMIT INSIDE/JUST AFTER THE ARMED WINDOW: the turnaround, and the
 *     reason preemption exists - a tracking channel that hears acknowledged
 *     data owes an 8-byte reply ~1.55 ms later (docs/spike-b-results.md),
 *     the tightest deadline in the link layer, requested from inside the
 *     still-armed window's own receive callback. Waiting would mean arming
 *     inside min_arm_lead_us, which backends refuse.
 *   - A BOUNDED REQUEST UNDER A BACKGROUND SCAN: tracked work always
 *     outranks searching; an in-progress scan chunk is just cut short and
 *     resumed afterwards.
 *   - A CHANNEL THAT COULD HAVE JOINED A WINDOW NOT YET OPEN: tearing down
 *     before the first bit of preamble costs nothing, so a late-opened
 *     channel joins the merge immediately instead of waiting a period.
 *   - A BOUNDED RECEIVE THAT REALLY STARTS EARLIER THAN AN ARMED BOUNDED
 *     RECEIVE THAT HAS NOT OPENED: the same "costs nothing before preamble"
 *     argument, for a channel that cannot join the armed window rather than
 *     one that can. It is what stops arm_next()'s deliberate early commitment
 *     from starving a window re-posted behind it - see the branch itself.
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
			radiant_time_t need = lead;

#if defined(CONFIG_RADIANT_SCHED_BOOKING)
			/* A booked operation needs only the gap the chunk in
			 * front of it was cut to leave (arm_next()); with the
			 * whole lead here every pass mid-chunk would tear the
			 * chunk down. */
			if (is_booked(sl)) {
				need = book_lead() +
				       (radiant_time_t)RADIANT_SCHED_DISPATCH_SLACK_US;
				/* And a chunk that has overrun its own close
				 * this near the booked open is taken down
				 * whatever it said it would do. See
				 * hal_book_due(). */
				if (sl->t_start <= earliest - lead + book_lead() +
				    (radiant_time_t)RADIANT_SCHED_BOOK_DUE_US) {
					return true;
				}
			}
#endif
			if (sl->t_start <
			    s.armed_end + need + phy_lead(sl->fmt)) {
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
		/*
		 * AN UNOPENED BOUNDED RECEIVE GIVES WAY TO ONE THAT REALLY
		 * STARTS EARLIER. Without this, arm_next()'s "commit the radio
		 * early" rule is a trap for the channel it did not choose: a
		 * tracked window re-posted after its predecessor was reported
		 * MISSED (see api_sched_done() in radiant/src/antr/ant_radio_radiant.c)
		 * arrives to find the radio already committed to something up to
		 * a quarter of a second away, waits behind it, and is itself
		 * reported MISSED. Eight of those in a row is
		 * RX_FAIL_GO_TO_SEARCH - a ~2 s dropout produced entirely by
		 * scheduling, with nothing wrong on the air.
		 *
		 * could_join_armed() above does not cover it. That branch is for
		 * a channel that can SHARE the armed window; this is for one
		 * that cannot - different format, different device number past
		 * rule 5, or simply too far away to merge - and must therefore
		 * displace it.
		 *
		 * Guarded exactly as could_join_armed() is, and for the same
		 * reason: `s.armed_open > earliest` means not a bit of preamble
		 * has been received yet, so the teardown throws nothing away.
		 * An armed CONTINUOUS scan is excluded because the branch above
		 * already handles bounded-under-scan, on its own terms.
		 *
		 * Bounded: the replacement is strictly earlier than what it
		 * displaced (phy_lead included, so the winner can actually be
		 * armed rather than refused ETIME), so the displaced request
		 * cannot turn round and displace it back.
		 */
		if (s.armed_kind == (uint8_t)SLOT_RX && !s.armed_continuous &&
		    sl->kind == (uint8_t)SLOT_RX && !sl->continuous &&
		    s.armed_open > earliest &&
		    sl->t_start + phy_lead(sl->fmt) < s.armed_open) {
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
		at = t_max(sl->t_start, slot_earliest(sl, earliest));
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
		/* The scan's own floor: the ordinary one, or (d2) the elastic
		 * lead. slot_earliest() is the identity without booking. */
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
		radiant_time_t s_earliest = slot_earliest(&s.ch[scan_ch], earliest);
#else
		const radiant_time_t s_earliest = earliest;
#endif
		radiant_time_t s_open = t_max(scan_at, s_earliest);
		radiant_time_t s_limit = RADIANT_TIME_NEVER;

#if defined(CONFIG_RADIANT_SCHED_BOOKING)
		/*
		 * ADR 0021 decision 5. The tracked window just completed and
		 * its booking asked for a tail: the grant is still held, so a
		 * chunk goes straight into it at the in-grant lead, up to where
		 * the booked air ends. It needs no request, so nothing about it
		 * can be refused; the release owed by the completion is kept
		 * for it (THE GRANT HANDOFF). Not if a committed operation
		 * wants any of that air: a follow-on comes first.
		 */
		if (booking() && s.in_radio_cb && s.tail_end != 0u) {
			const radiant_time_t t_ig =
				earliest - arm_lead() + in_grant_lead();
			const radiant_time_t t_open = t_max(s.ch[scan_ch].t_start, t_ig);
			const radiant_time_t t_lim =
				t_back(s.tail_end, rx_tail(s.ch[scan_ch].fmt) +
							   RADIANT_SCHED_DISPATCH_SLACK_US);

			if (t_lim >= t_open + (radiant_time_t)RADIANT_SCHED_SCAN_MIN_US &&
			    (committed == RADIANT_TIME_NEVER ||
			     committed > s.tail_end + book_lead())) {
				enum step st = arm_rx_window((uint8_t)scan_ch, t_ig, t_lim);

				if (st != STEP_SKIP) {
					s.stats.scan_tail++;
					return st;
				}
			}
		}
		/*
		 * Not before the last tracked window's timeslot has ended. That
		 * timeslot is booked to its close plus the follow-on reserve,
		 * and with two sessions the arbiter judges the chunk's request
		 * against all of it at once. Opening the chunk earlier, once the
		 * early release has given that air back, lost tracked slots on
		 * the bench (phase-3a.md, cause not found). A chunk opened
		 * inside it waits for that end and then has too little notice
		 * left; opening after it costs the same listening without the
		 * wasted request.
		 */
		if (booking() && s.fix_end != 0u) {
			s_earliest = t_max(s_earliest,
					   s.fix_end +
						   (radiant_time_t)RADIANT_SCHED_BOOK_TAIL_US);
			s_open = t_max(s_open, s_earliest);
		}
#endif
		uint32_t   chunk = s.ch[scan_ch].chunk_us;
		bool       booked_gap = false;

		if (chunk == 0u) {
			chunk = RADIANT_SCHED_SCAN_CHUNK_US;
		}
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
		/*
		 * ADR 0018 (d2): book the committed operation BEFORE arming the
		 * chunk in front of it, so its request is with the arbiter before
		 * the chunk's grant can reach its instant (Phase 0c F2: first
		 * come, first served). Then the chunk runs to a small gap before
		 * it instead of a whole lead.
		 */
		if (committed != RADIANT_TIME_NEVER && booking()) {
			int c1 = (tx_ch >= 0 && tx_at == committed) ? tx_ch : rx_ch;
			/*
			 * Book here, before the chunk, only if today's limit
			 * would bind on this chunk. A committed operation
			 * further off than one capped chunk is booked by
			 * book_ahead() AFTER the chunk is armed: the chunk's
			 * grant cannot reach it before that booking is placed,
			 * and the chunk then has the idle session (the gate's
			 * pick_free()) instead of waiting out the one the last
			 * operation is still releasing.
			 */
			const radiant_time_t c_want = window_cap(
				s_open, s_open + (radiant_time_t)chunk);
			const bool binds =
				t_back(committed, arm_lead() +
						  RADIANT_SCHED_DISPATCH_SLACK_US +
						  rx_tail(s.ch[scan_ch].fmt)) <
				c_want;

			if (c1 >= 0 && binds && !is_booked(&s.ch[c1]) &&
			    s.ch[c1].t_start >= earliest) {
				(void)book_slot((uint8_t)c1);
			}
			if (c1 >= 0 && is_booked(&s.ch[c1])) {
				const struct sched_slot *k1 = &s.ch[c1];
				radiant_time_t           c1_end;
				int                      c2;

				committed = t_max(k1->t_start,
						  earliest - arm_lead() + book_lead());
				s_limit = t_back(committed,
						 book_lead() +
							 RADIANT_SCHED_DISPATCH_SLACK_US +
							 RADIANT_SCHED_BOOK_GAP_US +
							 phy_gap(s.ch[scan_ch].fmt,
								 committed_fmt) +
							 rx_tail(s.ch[scan_ch].fmt));
				booked_gap = true;
				/*
				 * R1. A second committed operation that is not
				 * booked must still be bookable with a full lead
				 * once a session frees. If c1's own grant ends
				 * too late for that, the only session that can
				 * free in time is this chunk's: stop the chunk a
				 * lead before c2 instead. Costs listening, never a
				 * slot. (Booking c2 now instead would leave the
				 * chunk no session at all until c1 ends, which
				 * costs more listening than the cut.)
				 */
				c1_end = (k1->kind == (uint8_t)SLOT_TX)
						 ? k1->t_start
						 : k1->t_end;
				c1_end += (radiant_time_t)k1->follow_on_us;
				c2 = next_committed(k1->t_start + 1u, c1);
				if (c2 >= 0 && !is_booked(&s.ch[c2]) &&
				    t_fwd(c1_end, book_lead()) >
					    t_back(s.ch[c2].t_start, arm_lead())) {
					radiant_time_t r1 = t_back(
						s.ch[c2].t_start,
						arm_lead() +
							RADIANT_SCHED_DISPATCH_SLACK_US +
							rx_tail(s.ch[scan_ch].fmt));

					if (r1 < s_limit) {
						s_limit = r1;
						s.stats.book_r1_cut++;
					}
				}
			}
		}
#endif
		if (committed != RADIANT_TIME_NEVER && !booked_gap) {
			/*
			 * THE GAP IN FRONT OF A COMMITTED OPERATION IS THREE
			 * THINGS, NOT TWO. The lead the committed window needs
			 * to be armed, the reconfiguration between the two
			 * PHYs - and the tail of the chunk being truncated,
			 * which is what this line used to be missing.
			 *
			 * A scan chunk does not end at its t_close. t_close is
			 * the latest t_sync it will accept, and t_sync is the
			 * end of the ADDRESS, so the receiver stays on for the
			 * body and CRC of a frame that arrives right at the
			 * bell. Truncating to `committed - lead` therefore
			 * hands the whole lead to that tail and leaves nothing
			 * to arm with.
			 *
			 * Measured on an nRF54L15 DK against a simulated power
			 * meter, one tracked channel plus Zwift's background
			 * scanning channel: the chunk's terminal arrived 149 us
			 * after its t_close (112 us of search-format tail plus
			 * the DISABLED->ISR path) against a min_arm_lead_us of
			 * exactly 149. The tracked window was then armed with
			 * 23 us of its 254 us span left, heard nothing, and the
			 * next one - the one the short remainder chunk did not
			 * reach - was fine. That is a clean OK/FAIL/OK/FAIL at
			 * 50.0 % loss, which is what both Zwift captures show
			 * and what tools/ant_search_contention.py reproduces.
			 */
			s_limit = t_back(committed,
					 lead + RADIANT_SCHED_DISPATCH_SLACK_US +
						 phy_gap(s.ch[scan_ch].fmt,
							 committed_fmt) +
						 rx_tail(s.ch[scan_ch].fmt));
			/* DISPATCH_SLACK is software time, separate from receiver
			 * hold: the terminal interrupt and this pass execute after
			 * the hardware tail. Exact TX deadlines cannot absorb it. */
		}
		if (s_limit == RADIANT_TIME_NEVER ||
		    s_limit >= s_open + (radiant_time_t)RADIANT_SCHED_SCAN_MIN_US) {
			radiant_time_t want = s_open + (radiant_time_t)chunk;
			enum step  st;

			st = arm_rx_window((uint8_t)scan_ch, s_earliest,
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
			 * operation needs is the one it needs from here.
			 *
			 * And no rx_tail(): an ED chunk measures a band, it
			 * does not receive a frame, so there is no body still
			 * arriving when its dwell is up. That is why the tail
			 * is charged per gap filler rather than added to
			 * arm_lead(), which every operation would pay. */
			e_limit = t_back(committed,
					 lead + RADIANT_SCHED_DISPATCH_SLACK_US +
					 phy_gap(NULL, committed_fmt));
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
			 * costs the frame.
			 *
			 * rx_tail() for the same reason as the scan branch
			 * above: this window's own t_close bounds a t_sync, so
			 * the receiver is still on for a body and CRC after it.
			 * A transmit's t_sync is exact and cannot slip, so
			 * without this the frame that gets lost is the master's
			 * own - the same defect, on the side of the scheduler
			 * a slave-only dongle never exercises. */
			limit = t_back(tx_at,
				       lead + RADIANT_SCHED_DISPATCH_SLACK_US +
					       phy_gap(s.ch[rx_ch].fmt,
						       s.ch[tx_ch].fmt) +
					       rx_tail(s.ch[rx_ch].fmt));
		}
		if (limit == RADIANT_TIME_NEVER || limit >= rx_at) {
			st = arm_rx_leader((uint8_t)rx_ch, earliest, limit);
			if (st != STEP_SKIP) {
				return st;
			}
		}
	}

	if (tx_ch >= 0) {
		return arm_tx_op((uint8_t)tx_ch);
	}

	if (rx_ch >= 0) {
		enum step st = arm_rx_leader((uint8_t)rx_ch, earliest,
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
			expired = sl->t_start <
				  slot_earliest(sl, earliest) + phy_lead(sl->fmt);
			RADIANT_DIAG_TX_MARGIN((int64_t)sl->t_start -
					       (int64_t)(slot_earliest(sl, earliest) +
							 phy_lead(sl->fmt)));
			if (expired) {
				RADIANT_DIAG_INC(tx_expired);
			}
		} else {
			/* A receive window whose open has passed is still worth
			 * arming for the rest of its span, so only the close
			 * expires it - and the close must clear the PHY switch,
			 * exactly as arm_rx_window() computes its open. Otherwise
			 * a window that can no longer be armed survives here,
			 * arm_rx_window() returns STEP_SKIP for it, and arm_next()
			 * ends the pass with the radio idle and every later
			 * receive unarmed until the next pump (review 2026-10-31,
			 * A8-4). phy_lead() is 0 on a build without a PHY switch
			 * to pay for, so this is unchanged there. */
			expired = !sl->continuous &&
				  sl->t_end < slot_earliest(sl, earliest) +
						      phy_lead(sl->fmt);
		}
		if (expired) {
			slot_clear((uint8_t)i);
			notify_done((uint8_t)i, RADIANT_SCHED_DONE_MISSED);
			return STEP_RETRY;
		}
	}

	if (s.armed_kind != (uint8_t)SLOT_IDLE) {
		if (now > t_fwd(s.armed_end, RADIANT_SCHED_ARMED_STALE_US)) {
			/* No terminal, long past its end: the backend lost it
			 * (e.g. a compare written behind the counter). Nothing
			 * else ever displaces an already-open window, so without
			 * this the one radio slot is held forever (review
			 * 2026-10-31, A9-3). abort_armed() aborts the radio and
			 * tells the members ABORTED, so owners re-post. */
			s.stats.armed_stale++;
			(void)abort_armed(RADIANT_SCHED_CH_NONE);
		} else if (!want_preempt(earliest)) {
			return STEP_DONE;
		} else if (abort_armed(RADIANT_SCHED_CH_NONE)) {
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
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	bk_prune(now);
#endif

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
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	book_ahead(now);
#endif
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
	bool was_in_cb;

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
	was_in_cb = s.in_radio_cb;
	s.in_radio_cb = true;

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
	s.in_radio_cb = was_in_cb;
}

static void hal_tx(const struct radiant_tx_event *evt, void *user)
{
	uint8_t ch;
	bool    was_in_pass;
	bool    was_in_cb;

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
	was_in_cb = s.in_radio_cb;
	s.in_radio_cb = true;
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
	s.in_radio_cb = was_in_cb;
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
	bool    was_in_cb;

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
	was_in_cb = s.in_radio_cb;
	s.in_radio_cb = true;

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
	s.in_radio_cb = was_in_cb;
}
#endif /* SCHED_ED_BUILT */

#if defined(CONFIG_RADIANT_SCHED_BOOKING)
/*
 * A booking's start is close and nothing is armed into it. The core only runs
 * on events, so if the chunk in front of the booking has not ended (its request
 * refused late, its grant running on) nothing would arm the operation before
 * the grant arrived with nothing staged, and the slot would go. So force a
 * pass; want_preempt() takes the overdue chunk down.
 *
 * Counted, so a live slot lost this way is told apart from a booking whose
 * channel has gone. Decided before the pass, which can report the slot MISSED
 * and clear it.
 */
static void hal_book_due(void *user)
{
	bool           wanted = false;
	radiant_time_t now;
	unsigned int   key;

	(void)user;

	key = radiant_event_crit_enter();
	now = radiant_radio_now();
	for (uint32_t i = 0u; i < RADIANT_SCHED_MAX_CHANNELS; i++) {
		const struct sched_slot *sl = &s.ch[i];

		if (((sl->kind == (uint8_t)SLOT_RX && !sl->continuous) ||
		     sl->kind == (uint8_t)SLOT_TX) &&
		    !sl->in_flight && is_booked(sl) &&
		    sl->t_start <= now + arm_lead()) {
			wanted = true;
		}
	}
	s.dirty = true;
	pass();
	if (!wanted) {
		s.stats.book_orphan++;
	} else if (s.armed_kind != (uint8_t)SLOT_IDLE && !s.armed_continuous) {
		s.stats.book_saved++;
	} else {
		s.stats.book_late++;
	}
	radiant_event_crit_exit(key);
}
#endif

static const struct radiant_radio_cbs sched_radio_cbs = {
	.rx = hal_rx,
	.tx = hal_tx,
#if SCHED_ED_BUILT
	.ed = hal_ed,
#endif
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	.book_due = hal_book_due,
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
	 * CONFIG_RADIANT_ED_SCAN. */
	radiant_chanmap_reset();

	s.caps = radiant_radio_caps_get();
	if (s.caps == NULL) {
		return RADIANT_RADIO_EIO;
	}
	/* Every backend must state its address-group limit (equal to
	 * max_filters when it has none). Treating 0 as "no limit" would hide a
	 * backend that forgot it until the first merged window came back
	 * RADIANT_RADIO_ENOTSUP. */
	if (s.caps->max_addr_groups == 0u) {
		s.caps = NULL;
		return RADIANT_RADIO_EINVAL;
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
	/* Unconditional: a completed operation's reserve survives it while
	 * idle, and the abort above (or the session ending) gives back the
	 * grant it described. */
	s.reserve_end = 0u;
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
