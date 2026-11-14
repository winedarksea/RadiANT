/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_sched.c - the radio schedule.
 *
 * Clean-room: written from radiant_radio_hal.h and radiant_sched.h against
 * tests/fake_radio.c. Bounded constants: docs/ant-radio-link.md and
 * docs/spike-b-results.md. See docs/decisions/0002-clean-room-policy.md.
 *
 * A table of 32 slots, each holding at most one intent. pass() decides what the
 * radio does next. It runs from a radio callback's tail and from
 * radiant_sched_tick(). Posting a request never arms anything: arming on every
 * post would commit the first window before an overlapping second one could
 * merge with it. A pass makes one radiant_radio_now() call and at most one arm
 * call; fake_radio.c flags more than sixteen HAL calls per callback.
 *
 * Abort ordering: an aborted operation still delivers its terminal event. The
 * live operation is retired (s.armed_op cleared) before radiant_radio_abort(),
 * and every event handler drops events whose op id is not s.armed_op. In the
 * other order, the replaced operation's ABORTED event would end its replacement.
 * Preemption makes such stale events routine, so s.stats.stale_events is
 * normally non-zero.
 *
 * Thread against interrupt: `s` is written by thread context (the request entry
 * points and radiant_sched_tick()) and by the radio ISR (hal_rx(), hal_tx()).
 * in_pass is saved and restored, so a nested entry defers to the owning pass.
 * Slot-table writes and pass() run with interrupts off via
 * radiant_event_crit_enter()/_exit(). No lock is exposed to the HAL.
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
/* Only for radiant_event_crit_enter()/_exit(); nothing here queues an event. */
#include <radiant/radiant_event.h>

/* --- State --- */

/* Energy-detect slot kind compiled in. Plain 0/1, not IS_ENABLED(), so ED
 * branches stay `if (SCHED_ED_BUILT && ...)` and the compiler folds them. */
#if defined(CONFIG_RADIANT_ED_SCAN)
#define SCHED_ED_BUILT 1
#else
#define SCHED_ED_BUILT 0
#endif

/*
 * SLOT_ED measures frequencies the link layer is not using (adaptive frequency
 * selection). It must never cost a packet: lowest priority, never preempts,
 * merges with nothing, and takes only gaps. So the RX/TX arm sequence is the
 * same whether or not ED is posted.
 */
enum slot_kind {
	SLOT_IDLE = 0,
	SLOT_RX,
	SLOT_TX,
	SLOT_ED
};

/*
 * What one channel wants next. A slot holds a receive or a transmit, never both,
 * so they share t_start (t_open for a receive, the requested t_sync for a
 * transmit). t_end is the receive close, or RADIANT_TIME_NEVER for continuous;
 * unused by a transmit.
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
	/* Transmit only: the on-air address, copied at the post. */
	uint8_t addr_len;
	uint8_t addr[RADIANT_RADIO_ADDR_MAX];

	/* Energy detect only; rf_index is the range's low end. rf_cursor is where
	 * the next chunk starts. It advances per delivered dwell, not per armed
	 * chunk, so chunks cut short by preemption do not skip the same indices. */
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
	/* Energy detect only: per-index ceiling. Kept apart from chunk_us. */
	uint32_t            dwell_us;

	radiant_time_t t_start;
	radiant_time_t t_end;
};

/* The slot table is the scheduler's whole per-channel RAM cost: 72 B each. */
_Static_assert(sizeof(struct sched_slot) <= 72u,
	       "a scheduler slot outgrew the per-channel RAM budget");

static struct {
	bool inited;
	/* A pass, or a callback that may lead to one, is running. Requests posted
	 * under it are considered on the way out, not armed re-entrantly. Saved and
	 * restored, never cleared: an ISR can land on a pass already in progress. */
	bool in_pass;
	/* A backend callback (hal_rx/hal_tx/hal_ed) is running, including the pass
	 * it ends with. The only context a follow-on reserve may be used from (see
	 * in_reserve()). Saved and restored like in_pass. */
	bool in_radio_cb;
	/* The table changed, so a pass is due. Lets a non-terminal receive event
	 * cost nothing unless a callback posted something. */
	bool dirty;
	/* A request was posted since the last arm, so an armed window not yet open
	 * is worth rebuilding. Cleared on every arm: one attempt per request. */
	bool replan;

	struct radiant_sched_cbs cbs;
	void                *user;

	/* The pointer is cached, not the contents: the HAL guarantees it is static,
	 * so limits are re-read on every pass. */
	const struct radiant_radio_caps *caps;

	struct sched_slot ch[RADIANT_SCHED_MAX_CHANNELS];
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
	/* 0xF7 page 7. diag_seen bit c: channel c's bounded receive has been looked at.
	 * diag_ahead bit c: that first look found it still armable, so a late open
	 * is the scheduler's doing. */
	uint32_t diag_seen;
	uint32_t diag_ahead;
#endif

	/* The live operation. armed_open/armed_end are the window it was armed
	 * with, not what was requested. armed_fmt/armed_rf answer "could a new
	 * request join it?" without a member slot that may have been replaced. */
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

	/* Last instant covered by the most recent operation's follow_on_us reserve,
	 * or 0. Survives a completed operation; cleared by any other terminal, an
	 * abort outside a callback, or a reset. See in_reserve(). */
	radiant_time_t               reserve_end;

	/* Filter array for radiant_radio_rx(). The HAL needs it valid until the
	 * terminal event, so it lives here. owner[]/local[] map an rx_event
	 * filter_index to (channel, that channel's filter index). */
	struct radiant_rx_filter merged[RADIANT_SCHED_MAX_FILTERS];
	uint8_t              owner[RADIANT_SCHED_MAX_FILTERS];
	uint8_t              local[RADIANT_SCHED_MAX_FILTERS];
	uint8_t              n_merged;

	/* Where the next leader search starts, so high-numbered channels are not
	 * starved by lower ones asking for the same instant. */
	uint8_t cursor;

	/* The PHY the radio is configured for, valid only while phy_known. Until
	 * the first arm, every candidate is charged the switch cost (the
	 * conservative choice). Updated only on a successful arm. */
	uint8_t phy;
	bool    phy_known;

#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	/*
	 * ADR 0018 (d2): operations booked with the backend (radiant_radio_book()),
	 * armed later at the in-grant lead. Keyed by booked air, not channel: a
	 * merged window's members are covered by their leader's booking.
	 */
	struct {
		bool                             used;
		uint8_t                          kind; /* SLOT_RX or SLOT_TX */
		uint8_t                          rf;
		uint8_t                          ch;   /* TX only */
		const struct radiant_pkt_format *fmt;
		radiant_time_t                   open;
		radiant_time_t                   close;
		/* Air end including a scan tail (ADR 0021 decision 5), or 0. */
		radiant_time_t                   tail_end;
		/* Phase 3c R1 join: air end once joined operations are covered, or 0.
		 * `ch` is then the operation the request is for. */
		radiant_time_t                   air_end;
	} bk[RADIANT_SCHED_BOOK_MAX];
	/* Air end of the last fixed-class operation, follow-on reserve included. A
	 * scan chunk in the other session must not start before it (arm_next()). */
	radiant_time_t fix_end;
	/* End of the armed operation's booked tail, or 0. While set, a completing
	 * operation lets a scan chunk arm into the air still held. */
	radiant_time_t tail_end;
	/* Dither for the tail length, so the gap after a tail does not sit at one
	 * phase of the tracked period. */
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

/* --- Small helpers --- */

/* Addresses one window may carry: 8 on nRF, 2 on EFR32/RAIL. */
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
 * Distinct address groups one window may carry now. Unlike max_filters (which
 * counts addresses), this counts device numbers (ADR 0005). radiant_sched_init()
 * refuses a zero.
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

/* Same group: equal addr[0 .. addr_len-2]. The last byte is a prefix register
 * and may vary within a group. A one-byte address is all prefix. */
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

/* Distinct groups the merged set would hold with `add` included. Linear scan:
 * the set holds at most 8 filters. */
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

/* Slack budget: an arm within this distance of now() fails RADIANT_RADIO_ETIME,
 * so the scheduler subtracts it itself. */
static radiant_time_t arm_lead(void)
{
	return (s.caps == NULL) ? 0u : (radiant_time_t)s.caps->min_arm_lead_us;
}

/* Lead inside air a previous operation already holds. A 0 in caps means
 * min_arm_lead_us. */
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
 * Does this request fall inside the follow-on reserve the last completed
 * operation asked the backend to hold (an ACK reply, a queued command)? If so,
 * it needs only the in-grant lead. Without this, arbitrated-backend follow-ons
 * (1.5-2.2 ms out) were all reported MISSED. Only the end is tested; the backend
 * refuses with EDENIED if the air was not held. Valid only inside a backend
 * callback, since a grant is held past its operation only during the completion.
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
 * ADR 0018 (d2). With caps.booking, a committed operation's air is placed a lead
 * ahead, so it arms at the in-grant lead and the chunk in front stops
 * RADIANT_SCHED_BOOK_GAP_US before it. Inert unless caps.booking is set.
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
		/* Joined to another operation's grant (phase 3c). */
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
 * The operation a joined request was for got no grant, so its joiners are no
 * longer covered and are booked or armed on their own. The booking keeps its air.
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
 * PHASE 3C R1 JOIN. Extends an operation's air past committed operations that
 * start within RADIANT_SCHED_JOIN_GAP_US of it, so they ride its grant instead
 * of being refused. Chains. Returns air_end unchanged if none joins.
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
 * Defer booking channel c while a neighbour near now is in flight or committed:
 * its completion posts the next slot, which must be known before c is booked.
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

/* True if a scan is running. A tail is only worth requesting while one is. */
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
 * ADR 0021 decision 5: the air to request past a booking's follow-on, or 0.
 * Dithered between half and 1.5 times the configured length. Never when another
 * committed operation is close, since a tail over its instant would get that
 * neighbour's booking refused. Checked against the known instants and a period
 * on, since ANT+ channels repeat.
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

/* Tail end of the booking this armed window lies in, or 0. */
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

/* Joined air end of the booking this armed window lies in, or 0. */
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
 * Book slot c with the backend: the air as the arm would ask for it, widened to
 * the bounded receives that could merge in, so the later arm lies inside the
 * booking whichever members join. Bounded receive or transmit only.
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
		/* Phase 3c: an operation just behind joins as this booking's tail, so
		 * this grant carries it. A joined booking asks no scan tail. */
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

/* The committed (bounded, not in flight) operation starting first at or after
 * `after`, or -1. Ties go to the lower index. skip_ch is excluded. */
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
 * After a pass: book the committed operations that follow, earliest first, while
 * the backend has room. An operation put off by join_defer() is skipped, as are
 * later ones inside its possible joined reach (booking those first would get the
 * deferred one's request refused).
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

/* Earliest this slot can be armed: the pass's floor (now + arm_lead()), nearer
 * for a follow-on inside the held reserve. */
static radiant_time_t slot_earliest(const struct sched_slot *sl,
				    radiant_time_t earliest)
{
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	/* Booked operations arm into air already placed; scan chunks at the
	 * elastic lead. */
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
 * Extra lead this candidate needs because the radio is not on its PHY (zero if
 * it is, or the backend switches for free). Kept apart from arm_lead(), which
 * every op pays. A NULL format (ED chunk) has no PHY.
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
 * The lead a gap must leave before a committed operation when a filler runs in
 * it first. Not phy_lead(): the radio will be on the filler's PHY by then. A NULL
 * `after` means the filler leaves the configuration alone (ED).
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
 * How long a receive window holds the radio past its own t_close. t_close bounds
 * t_sync (the end of the address), so a frame arriving at the last instant still
 * has its body and CRC to come. Charged to gap fillers, on top of arm_lead().
 */
static radiant_time_t rx_tail(const struct radiant_pkt_format *fmt)
{
	if (fmt == NULL) {
		/* An energy-detect chunk receives no frame: no airtime tail, no hold. */
		return 0u;
	}
	return (radiant_time_t)radiant_frame_tail_us(fmt) +
	       ((s.caps == NULL) ? 0u
				 : (radiant_time_t)s.caps->rx_close_hold_us);
}

/* Record the configured PHY. Called only from a successful arm. */
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

/* t - d, floored at zero (the timebase may start low). */
static radiant_time_t t_back(radiant_time_t t, radiant_time_t d)
{
	return (t > d) ? (t - d) : 0u;
}

/* t + d, saturated at RADIANT_TIME_NEVER. A continuous request's close is
 * RADIANT_TIME_NEVER, and plain addition would wrap it to a small value. */
static radiant_time_t t_fwd(radiant_time_t t, radiant_time_t d)
{
	return (RADIANT_TIME_NEVER - t < d) ? RADIANT_TIME_NEVER : (t + d);
}

/* Bound a window by what the backend can arm in one operation (caps.max_window_us,
 * 0 = unbounded). A continuous request survives a chunk that ended normally. */
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

/* Tell a member of a just-armed window what it got. The bounds are the arm's,
 * not the request's. See struct radiant_sched_cbs::armed. */
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

/* --- Ending an operation --- */

/*
 * The armed operation reached its own end. Members are consumed and told why,
 * except a continuous request that ended normally (it re-arms). Slots are
 * cleared before the owner is told, so a done() that posts is not overwritten.
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
	/* The reserve outlives only a completed operation (see in_reserve()). */
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
		/*
		 * DENIED and ABORTED must keep ED and continuous-RX requests, or scanning
		 * stops for good: nothing re-posts them. abort_armed() uses the same test.
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
 * Take the radio away from whatever holds it. Returns true if the operation had
 * started. Members of a window that had not opened keep their requests silently.
 * Members of an open window are told ABORTED, except a background scan, which
 * pauses. skip_ch asked for this and is not told.
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
	/* Outside a callback the abort returns the grant, so the reserve goes too.
	 * Inside one it is the handoff: the follow-on this abort makes room for is
	 * what the reserve is for. */
	if (!s.in_radio_cb) {
		s.reserve_end = 0u;
	}
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	/* An aborted operation's air cannot carry a chunk or its joiners. */
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
			/* Paused, not ended: the request survives but is still told,
			 * since an owner accounting listening time must know it stopped.
			 * ED indices already measured are in the map; rf_cursor marks
			 * where the next gap resumes. */
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

/* Forget this channel's request, aborting it if in flight. ch is not notified. */
static void drop_slot(uint8_t ch)
{
	if (s.ch[ch].in_flight) {
		(void)abort_armed(ch);
	}
	slot_clear(ch);
}

/* --- Arming --- */

static void pass(void);

/*
 * Handle a refused arm call. A pass must always terminate. EBUSY and ESTATE
 * describe the radio, so nothing is consumed. Anything else describes the
 * request, which is consumed and reported, or later passes would retry forever.
 */
static enum step arm_failed(uint8_t ch, int rc)
{
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	/* Consumed: nothing carries whatever joined this request. */
	if (rc != RADIANT_RADIO_EBUSY && rc != RADIANT_RADIO_ESTATE) {
		bk_drop_join(ch);
	}
#endif
	switch (rc) {
	case RADIANT_RADIO_EBUSY:
		/* The backend raced a callback consuming the op slot. */
		s.stats.arm_ebusy++;
		return STEP_DONE;
	case RADIANT_RADIO_ESTATE:
		/* Not enabled: every request would fail the same way, so keep them. */
		s.stats.arm_rejected++;
		return STEP_DONE;
	case RADIANT_RADIO_ETIME:
		/* The pass subtracts min_arm_lead_us, but the nRF backend can still
		 * return this when its start compare was written too late. Report
		 * missed. */
		s.stats.arm_rejected++;
		slot_clear(ch);
		notify_done(ch, RADIANT_SCHED_DONE_MISSED);
		return STEP_RETRY;
	case RADIANT_RADIO_EDENIED: {
		/*
		 * Another stack holds the air. A denied scan or ED sweep keeps its
		 * request, as in end_armed(); a bounded request is consumed. A kept
		 * request ends the pass, or the same arbiter refuses it again.
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
		/* Unreachable by construction, so core and backend disagree. MISSED, not
		 * FAILED: `ch` is the leader, and a merged-in channel's constraint must not
		 * look like its failure. */
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
 * Build one receive window around a leader, merge every channel that can share
 * it, and arm it. All ANT+ is on RF 57, so overlapping tracked windows share one
 * radiant_radio_rx(), one filter each.
 *
 * Membership rules:
 *   1. Same format and RF index (pointer equality on fmt).
 *   2. At most caps.max_filters addresses.
 *   3. Every member lies within the leader's window extended by arm_lead(), not
 *      merely the union so far, or chains of near-touching windows would grow
 *      without bound. The reach is arm_lead() because closer windows cannot be
 *      armed in sequence (pass_step() reports them MISSED), so merging is the
 *      only way to hear both.
 *   4. The union is capped at RADIANT_SCHED_MERGE_SPAN_MAX_US, below the 2.19 ms
 *      master-to-slave turnaround. It bounds only what merging adds.
 *   5. At most caps.max_addr_groups device-number groups (2 on nRF).
 */
static enum step arm_rx_window(uint8_t leader, radiant_time_t earliest,
			       radiant_time_t limit)
{
	struct sched_slot   *lead = &s.ch[leader];
	const uint8_t        mf = max_filters();
	const uint8_t        mg = max_addr_groups();
	/* Rule 3's reach, read once so all members use the caller's radius. */
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
		/* More addresses than this backend can match: refuse, rather than arm
		 * something smaller than asked for. */
		s.stats.arm_rejected++;
		slot_clear(leader);
		notify_done(leader, RADIANT_SCHED_DONE_FAILED);
		return STEP_RETRY;
	}

	/* Group limit, checked separately: a leader can fit max_filters yet carry
	 * two device numbers. */
	s.n_merged = 0u;
	if (addr_groups_with(lead->filters, lead->n_filters) > mg) {
		s.stats.arm_rejected++;
		slot_clear(leader);
		notify_done(leader, RADIANT_SCHED_DONE_FAILED);
		return STEP_RETRY;
	}

	/* The window cannot open before the radio is reconfigured for its PHY. Only
	 * the leader is charged: rule 1 puts every member on its PHY. */
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
		/* Rule 5: skip this member rather than refuse the window. The leader
		 * and anything already merged stay armable; this channel gets its
		 * own window later. */
		if (addr_groups_with(m->filters, m->n_filters) > mg) {
			continue;
		}

		/*
		 * Use the leader's phy_lead (rule 1 puts every member on its PHY). Rule 3
		 * lets a member start first, so m_open can become the union's open.
		 */
		m_open = t_max(m->t_start, earliest + phy_lead(lead->fmt));
		m_close = m->continuous ? cap_close : m->t_end;
		if (m_close < m_open) {
			continue;
		}
		/*
		 * Rule 3: within the leader's window extended by the merge reach (see
		 * the header). t_fwd() rather than `+`, because could_join_armed()
		 * mirrors this test with a close that may be RADIANT_TIME_NEVER, and
		 * the two must saturate alike.
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
		/* After the caps, does this member get any coverage? A filter spent on
		 * a channel the window cannot hear is worse than not merging it. */
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

	/* STOP_ON_FIRST saves current on a single-master window, but the HAL forbids
	 * it with more than one filter, so it survives only when nothing merged. */
	if (s.n_merged == 1u && lead->stop_on_first && !lead->continuous) {
		flags |= RADIANT_RX_STOP_ON_FIRST;
	}
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	/* ADR 0018 (d2): a scan chunk is the elastic class at any length. */
	if (lead->continuous && booking()) {
		flags |= RADIANT_RX_ELASTIC;
	}
	/*
	 * No R1 join for a window armed as a fresh request: a refused request
	 * carrying a joiner would cost this window too. Only a booking carries
	 * joiners (arm_rx_leader()).
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
	/* The maximum over members: any member's frame may become an acknowledged
	 * reply, so reserving only the leader's would under-reserve. */
	req.follow_on_us = follow_on;

	rc = radiant_radio_rx(&req, &op);
	if (rc != RADIANT_RADIO_OK_RC) {
		s.n_merged = 0u;
		s.n_members = 0u;
		return arm_failed(leader, rc);
	}

#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
	/* 0xF7 page 7: a bounded window armed past its own start. Uses the
	 * leader's own open, not the union's. */
	if (!lead->continuous && (s.diag_ahead & ((uint32_t)1u << leader)) != 0u &&
	    lead_open > lead->t_start) {
		RADIANT_DIAG_INC(trk_open_late);
		RADIANT_DIAG_MAX(trk_open_late_max_us, lead_open - lead->t_start);
	}
#endif
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
		/* A booking with joiners holds its air to their end. */
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

	/* Last, once every member's state is settled: a callback must not see a
	 * half-armed module. */
	for (i = 0u; i < s.n_members; i++) {
		notify_armed(s.members[i], open, close);
	}
	return STEP_DONE;
}

/* arm_rx_window() for a bounded leader, at the floor in_reserve() allows. A
 * follow-on inside the held reserve opens at the in-grant lead, and no member
 * may carry the union past the reserve. */
static enum step arm_rx_leader(uint8_t leader, radiant_time_t earliest,
			       radiant_time_t limit)
{
	const struct sched_slot *sl = &s.ch[leader];

#if defined(CONFIG_RADIANT_SCHED_BOOKING)
	/*
	 * Phase 4 F1: book a bounded window before arming it fresh, since
	 * book_ahead() skips windows already in flight. A window too near to book,
	 * or with no free session, is armed fresh at its own length.
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
 * Arm one energy-detect chunk into the gap ahead of `limit`: as much of the
 * request as fits, in whole dwells (the backend gets a range, so the gap bounds
 * it). A range never wraps: a sweep reaching rf_hi restarts at rf_lo next chunk.
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
		/* No committed operation bounds the chunk. Skipped rather than left
		 * unbounded: an ED op with no end gives the radio away. */
		return STEP_SKIP;
	}

	dwell = (sl->dwell_us != 0u) ? sl->dwell_us : RADIANT_SCHED_ED_DWELL_US;
	span = (uint64_t)(limit - open);
	n = (uint32_t)(span / (uint64_t)dwell);
	if (n == 0u) {
		return STEP_SKIP;
	}

	/* The chunk ceiling in dwells, since the backend receives a range. */
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

	/* Neither s.cursor nor s.replan is touched: an ED slot must not choose the
	 * leader of the next merged window. */
	s.stats.ed_chunks++;
	return STEP_DONE;
}

/* --- Choosing --- */

/*
 * Could this request have joined the armed window? Checked against the armed
 * window's recorded shape. Must mirror arm_rx_window()'s membership rules.
 */
static bool could_join_armed(const struct sched_slot *sl, radiant_time_t earliest)
{
	radiant_time_t o;
	radiant_time_t c;

	if (s.armed_kind != (uint8_t)SLOT_RX || sl->kind != (uint8_t)SLOT_RX) {
		return false;
	}
	/* Only before the first bit of preamble: tearing down an open window throws
	 * away reception. */
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
	/* Rule 5, mirrored, so this agrees with the rebuild's own group test. */
	if (addr_groups_with(sl->filters, sl->n_filters) > max_addr_groups()) {
		return false;
	}
	/*
	 * Rule 3, mirrored, merge reach included. The two must agree exactly, or a
	 * joinable channel never triggers the rebuild that would take it.
	 */
	o = t_max(sl->t_start, earliest);
	c = sl->continuous ? RADIANT_TIME_NEVER : sl->t_end;
	return !(o > t_fwd(s.armed_end, arm_lead()) ||
		 t_fwd(c, arm_lead()) < s.armed_open);
}

/*
 * Displace the live operation? Only for a deadline that cannot survive waiting,
 * or a rebuild that costs nothing:
 *   - A transmit inside or just after the armed window (the turnaround: the
 *     acknowledged-data reply is owed about 1.55 ms after a receive).
 *   - A bounded request under a background scan (tracked work outranks search).
 *   - A channel that could join a window not yet open (free before preamble).
 *   - A bounded receive starting earlier than an unopened bounded receive.
 * An armed transmit is never displaced: its t_sync is exact and committed.
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
		/* An ED request never preempts. Skipped before the scan test below,
		 * which it would otherwise satisfy (ED has continuous == false). */
		if (sl->kind == (uint8_t)SLOT_ED) {
			continue;
		}
		if (sl->kind == (uint8_t)SLOT_TX) {
			/* Plus the reconfiguration lead, so a transmit on another PHY
			 * displaces early enough to be armed. */
			if (sl->t_start < s.armed_end + lead + phy_lead(sl->fmt)) {
				return true;
			}
		} else if (s.armed_continuous && !sl->continuous) {
			radiant_time_t need = lead;

#if defined(CONFIG_RADIANT_SCHED_BOOKING)
			/* A booked operation needs only the gap the chunk in front
			 * was cut to leave (arm_next()). */
			if (is_booked(sl)) {
				need = book_lead() +
				       (radiant_time_t)RADIANT_SCHED_DISPATCH_SLACK_US;
				/* A chunk overrunning its close this near the booked open is
				 * taken down regardless. See hal_book_due(). */
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
		/* Anything displaces an armed ED chunk. It was sized before later
		 * requests were posted, and ED is the cheapest thing to discard. */
		if (s.armed_kind == (uint8_t)SLOT_ED &&
		    sl->t_start < s.armed_end + lead) {
			return true;
		}
		if (s.replan && could_join_armed(sl, earliest)) {
			return true;
		}
		/*
		 * An unopened bounded receive gives way to one that starts earlier, or
		 * arm_next()'s early commitment would starve a re-posted window. The
		 * replacement must start strictly earlier, so it cannot be displaced back.
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
 * Pick and arm: run whatever starts first. A transmit breaks a tie and truncates
 * an open receive window if it must be armed first. A background scan fills the
 * gap before the next committed operation, if the gap is worth an arm. Otherwise
 * the radio is committed early, since nothing else would arm it.
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
			/* Tested before sl->continuous, which an ED slot leaves unset: the
			 * bounded bucket would give it a deadline and the expiry sweep
			 * would delete it. */
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

	/* Which operation is committed, not just when: sizing the gap needs its
	 * format. A transmit breaks the tie, as in the arming order below. */
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
		/* The scan's floor: the ordinary one, or the elastic lead under booking. */
#if defined(CONFIG_RADIANT_SCHED_BOOKING)
		radiant_time_t s_earliest = slot_earliest(&s.ch[scan_ch], earliest);
#else
		const radiant_time_t s_earliest = earliest;
#endif
		radiant_time_t s_open = t_max(scan_at, s_earliest);
		radiant_time_t s_limit = RADIANT_TIME_NEVER;

#if defined(CONFIG_RADIANT_SCHED_BOOKING)
		/*
		 * ADR 0021 decision 5: the booking asked for a tail and its grant is
		 * still held, so a chunk arms into it at the in-grant lead. It needs no
		 * request, so it cannot be refused. Not if a committed operation wants it.
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
		 * Not before the last tracked timeslot ends (its close plus follow-on
		 * reserve). Opening earlier lost tracked slots in testing.
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
		 * ADR 0018 (d2): book the committed operation BEFORE arming the chunk in
		 * front of it, so its request reaches the arbiter first. The chunk then
		 * runs to a small gap before it instead of a whole lead.
		 */
		if (committed != RADIANT_TIME_NEVER && booking()) {
			int c1 = (tx_ch >= 0 && tx_at == committed) ? tx_ch : rx_ch;
			/*
			 * Book here only if this limit would bind on the chunk. Otherwise
			 * book_ahead() books it after the chunk arms, so the chunk gets the
			 * idle session.
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
				 * R1: c2 must still be bookable with a full lead once a session
				 * frees. If c1's grant ends too late, stop the chunk a lead before
				 * c2. This costs listening, never a slot.
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
			 * The gap before a committed operation holds the lead, the PHY switch,
			 * and the chunk's receive tail (rx_tail()). Truncating to
			 * `committed - lead` alone leaves nothing to arm with.
			 */
			s_limit = t_back(committed,
					 lead + RADIANT_SCHED_DISPATCH_SLACK_US +
						 phy_gap(s.ch[scan_ch].fmt,
							 committed_fmt) +
						 rx_tail(s.ch[scan_ch].fmt));
			/* DISPATCH_SLACK is software time, separate from receiver hold: the
			 * terminal interrupt and this pass run after the hardware tail. */
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
	 * Energy detect, in whatever gap the scan did not want. Gated on scan_ch < 0,
	 * so a pending scan takes the gap even when it cannot be armed there. A
	 * searching dongle therefore measures nothing: discovery outranks the map.
	 */
	if (SCHED_ED_BUILT && ed_ch >= 0 && scan_ch < 0) {
		radiant_time_t e_open = t_max(ed_at, earliest);
		radiant_time_t e_limit = RADIANT_TIME_NEVER;
		uint32_t   chunk = s.ch[ed_ch].chunk_us;

		if (chunk == 0u) {
			chunk = RADIANT_SCHED_ED_CHUNK_US;
		}
		if (committed != RADIANT_TIME_NEVER) {
			/* NULL format: the ED chunk leaves the packet configuration alone.
			 * No rx_tail(): an ED chunk measures a band and receives no frame,
			 * so no body is still arriving when its dwell ends. */
			e_limit = t_back(committed,
					 lead + RADIANT_SCHED_DISPATCH_SLACK_US +
					 phy_gap(NULL, committed_fmt));
		}
		/* An unbounded gap is still capped at the chunk: an overrunning ED
		 * chunk would cost whatever window comes next. */
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
			/* Truncated so the transmit can still be armed. rx_tail() as in the
			 * scan branch: without it the master's own frame is what gets lost. */
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

/* --- The pass --- */

/*
 * Report and drop anything whose deadline has passed, then preempt or arm. The
 * expiry sweep runs even while an operation is armed: a window lost to
 * contention is reported MISSED as soon as it is unreachable.
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
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
		if (sl->kind == (uint8_t)SLOT_RX && !sl->continuous &&
		    (s.diag_seen & ((uint32_t)1u << i)) == 0u) {
			s.diag_seen |= (uint32_t)1u << i;
			if (sl->t_start >= slot_earliest(sl, earliest) + phy_lead(sl->fmt)) {
				s.diag_ahead |= (uint32_t)1u << i;
			}
		}
#endif
		if (sl->kind == (uint8_t)SLOT_ED) {
			/* Never expires. Stated explicitly: the receive branch would test
			 * the unset t_end and report the slot MISSED before a dwell ran. */
			expired = false;
		} else if (sl->kind == (uint8_t)SLOT_TX) {
			/* t_sync is exact: a transmit that can no longer hit it does not go
			 * out. The PHY switch counts too, or the backend would refuse it
			 * ETIME and the failure would be misreported. */
			expired = sl->t_start <
				  slot_earliest(sl, earliest) + phy_lead(sl->fmt);
			RADIANT_DIAG_TX_MARGIN((int64_t)sl->t_start -
					       (int64_t)(slot_earliest(sl, earliest) +
							 phy_lead(sl->fmt)));
			if (expired) {
				RADIANT_DIAG_INC(tx_expired);
			}
		} else {
			/* A receive window whose open has passed is still armable for its rest,
			 * so only the close expires it, and the close must clear the PHY
			 * switch (review A8-4). */
			expired = !sl->continuous &&
				  sl->t_end < slot_earliest(sl, earliest) +
						      phy_lead(sl->fmt);
		}
		if (expired) {
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
			/* 0xF7 page 7: a bounded receive expired while a scan chunk held the radio. */
			if (sl->kind == (uint8_t)SLOT_RX && !sl->continuous &&
			    s.armed_kind == (uint8_t)SLOT_RX && s.armed_continuous) {
				RADIANT_DIAG_INC(trk_missed_behind_scan);
			}
#endif
			slot_clear((uint8_t)i);
			notify_done((uint8_t)i, RADIANT_SCHED_DONE_MISSED);
			return STEP_RETRY;
		}
	}

	if (s.armed_kind != (uint8_t)SLOT_IDLE) {
		if (now > t_fwd(s.armed_end, RADIANT_SCHED_ARMED_STALE_US)) {
			/* No terminal event long past the end: the backend lost it. Nothing
			 * else displaces an open window, so the radio would be held forever
			 * (review A9-3). abort_armed() tells the members ABORTED, so owners
			 * re-post. */
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
 * One scheduling decision, from whichever entry point asked. radiant_radio_now()
 * is read once, so the pass reasons about one instant.
 */
static void pass(void)
{
	radiant_time_t now;
	uint32_t   guard;
	unsigned int key;

	/* Interrupts stay off for the whole pass: the in_pass check-and-set and the
	 * table reads must not interleave with an ISR. Nested entry is free. */
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

	/* Bounded: each retry consumes or arms a request. The margin over the
	 * channel count covers one window closing late enough to expire them all. */
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
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
	{
		/* 0xF7 page 8: the pass's interrupts-off span, measured from the
		 * one `now` it planned with. A break leaves guard at the last
		 * iteration's index; running out leaves it at the bound. */
		const uint32_t limit = (2u * RADIANT_SCHED_MAX_CHANNELS) + 4u;
		const uint32_t us = (uint32_t)(radiant_radio_now() - now);

		RADIANT_DIAG_MAX(pass_max_us, us);
		RADIANT_DIAG_MAX(pass_steps_max,
				 (guard < limit) ? guard + 1u : limit);
		if (us > RADIANT_DIAG_PASS_SLOW_US) {
			RADIANT_DIAG_INC(pass_over_100us);
		}
	}
#endif
	s.dirty = false;
	s.in_pass = false;
	radiant_event_crit_exit(key);
}

/* --- Radio events --- */

static void route_rx(const struct radiant_rx_event *evt)
{
	uint8_t fi = evt->filter_index;

	if (fi >= s.n_merged) {
		/* A filter index the window did not have. Counted, not routed: that
		 * would misattribute a frame. */
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
		/* The terminal event of something already retired (every preemption
		 * makes one). Not an error. */
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
		/* With STOP_ON_FIRST the first accepted frame is terminal. A CRC
		 * failure does not count: a 3-byte search address matches noise
		 * several times a second. */
		if (evt->status == RADIANT_RADIO_STATUS_OK && s.armed_stop_on_first) {
			end_armed(RADIANT_SCHED_DONE_OK);
			terminal = true;
		}
		break;
	case RADIANT_RADIO_STATUS_TIMEOUT:
		/* Noise floor. This is the one place that knows the window ended
		 * empty and on which frequency, so sample before end_armed() clears
		 * s.armed_rf. The backend sets has_noise only on an empty timeout. */
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
		/* Accepted but never granted, so the window never opened: unlike
		 * ABORTED, a scan's dwell is credited nothing and a tracked channel
		 * is not told its sensor went quiet. May arrive from a thread. */
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
	/* Nested in a thread-context pass: the owning pass commits. It re-checks
	 * s.dirty, so nothing is lost. */
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
		/* The frame never went out and the air was lent. radiant_api.c advances
		 * the slot clock from the slot kind, not this status. */
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
/* One energy-detect dwell, or the end of a chunk. Binned here, not in the
 * backend, so a second backend need not agree on binning. */
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
				/* Past the top of the range, restart at the bottom. Advanced per
				 * dwell (see struct sched_slot::rf_cursor). */
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
		/* The range completed. TIMEOUT, not OK, as for a receive window. */
		end_armed(RADIANT_SCHED_DONE_OK);
		terminal = true;
		break;
	case RADIANT_RADIO_STATUS_ABORTED:
		end_armed(RADIANT_SCHED_DONE_ABORTED);
		terminal = true;
		break;
	case RADIANT_RADIO_STATUS_DENIED:
		/* The chunk never ran: rf_cursor stays put, so the next chunk resumes
		 * here. end_armed() keeps the request. */
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
 * A booking is due and nothing is armed into it. Force a pass, so that
 * want_preempt() takes down an overdue chunk in front. Counted before the pass,
 * which can clear the slot.
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

/* --- API --- */

int radiant_sched_init(const struct radiant_sched_cbs *cbs, void *user)
{
	memset(&s, 0, sizeof(s));

	/* The noise histogram resets with the samples this module feeds it. */
	radiant_noise_reset();
	/* Likewise the channel-quality map (a no-op without CONFIG_RADIANT_ED_SCAN). */
	radiant_chanmap_reset();

	s.caps = radiant_radio_caps_get();
	if (s.caps == NULL) {
		return RADIANT_RADIO_EIO;
	}
	/* Every backend must state its address-group limit (equal to max_filters
	 * if it has no separate limit). Treating 0 as unlimited would hide a backend
	 * that forgot it until a merged window was refused ENOTSUP. */
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
	/* Unconditional: a completed operation's reserve survives while idle, and
	 * the abort above (or the session ending) returns the grant it described. */
	s.reserve_end = 0u;
	memset(s.ch, 0, sizeof(s.ch));
	s.cursor = 0u;
	s.dirty = false;
	s.replan = false;
	/* The PHY becomes unknown again: a backend that came back in its reset
	 * configuration would otherwise under-lead the next session's first operation. */
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

	/* The mutation window, closed against the radio ISR (see "Thread against
	 * interrupt"). */
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
#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
	s.diag_seen &= ~((uint32_t)1u << ch);
	s.diag_ahead &= ~((uint32_t)1u << ch);
#endif

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
	/* Refused here: a backend given a zero-length address has no honest move.
	 * Checking against fmt->addr_len also catches a 3-byte search address
	 * posted against the 5-byte tracking format. */
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
	/* Refused here, not reported through done(): this is a property of the
	 * build, not the request. Otherwise a slot no backend accepts would fail on
	 * every pass forever. */
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
	/* Ready now and always: the request carries no instant (see struct
	 * radiant_sched_ed). */
	sl->t_start = 0u;
	sl->t_end = 0u;

	s.dirty = true;
	/* s.replan is deliberately NOT set: could_join_armed() requires both sides
	 * to be SLOT_RX, so it can never say yes for an ED slot. */
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
	/* Abort now, since cancelling means the radio stops now. Choosing what runs
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
	/* Only a continuous request has a chunk, and it may only shorten. An
	 * in-flight chunk keeps its armed length; this bounds the next one. */
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
