/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf_gate_mpsl.c - the RADIO, borrowed from MPSL a slot at a
 * time.
 *
 * Clean-room: written from nrfxlib's mpsl_timeslot.h and the Nordic timeslot and
 * flash_sync samples. Nothing here derives from sdk-ant or libant.a.
 *
 * Off by default (CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL). Shipping code changes
 * only by CLAIM_TERMINAL() in radiant_radio_nrf.c.
 *
 * A printk costs ~87 us/char at 115200 and moves the grant path, so timing taken
 * through it is invalid (sequence is fine). Use the trace ring or deferred LOG_INF.
 *
 * Constants (mpsl_arb spike): grant latency 1692-1695 us (2760 us tail); anchor
 * error 0-17 us over 640 chained NORMAL requests; BLOCKED arrives 11-31 ms before
 * its start; timer drift -99 ppm, but only while the app holds its own HFXO
 * request (clock_control_on(); do not remove).
 *
 * Rules:
 * 1. One request outstanding per session (-NRF_EAGAIN unless IDLE).
 * 2. The first request must be EARLIEST, which is illegal inside a timeslot, so
 *    sessions bootstrap from thread context. Later requests are NORMAL, measured
 *    from the previous timeslot's START.
 * 3. A low-priority signal returning anything but ACTION_NONE asserts. Recovery
 *    is the core's, via the DENIED terminal.
 *
 * Ending a timeslot: operations finish in the RADIO interrupt, so gate_release()
 * keeps an MPSL_TIMER0 compare armed and ACTION_END returns from SIGNAL_TIMER0
 * (as esb.c and flash_sync_mpsl.c do). It must be MPSL_TIMER0, the clock MPSL
 * measures overstay against, not radiant's own 1 MHz TIMER.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/sys/reboot.h>

#include <mpsl_timeslot.h>
#include <mpsl_hwres.h>
#include <nrf_errno.h>
#include <hal/nrf_timer.h>

#include <radiant/radiant_swi.h>
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS_LOG)
#include <radiant/radiant_sched.h>
#endif

#include "radiant_radio_nrf_gate.h"

LOG_MODULE_REGISTER(radiant_gate_mpsl, CONFIG_RADIANT_LOG_LEVEL);

/*
 * CONFIG_MPSL_TIMESLOT_SESSION_COUNT defaults to 0, which sizes MPSL's session
 * array to nothing: the image links and boots, but the gate never gets air.
 *
 * Guarded by CONFIG_MPSL because the gate tests build this file without MPSL, using
 * fakes for <mpsl_timeslot.h>. The #ifndef is needed because an undefined symbol
 * expands to nothing, a syntax error in BUILD_ASSERT.
 */
#ifdef CONFIG_MPSL
#ifndef CONFIG_MPSL_TIMESLOT_SESSION_COUNT
#error "CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL=y needs CONFIG_MPSL_TIMESLOT_SESSION_COUNT >= 1 (it is not defined at all here)"
#endif
BUILD_ASSERT(CONFIG_MPSL_TIMESLOT_SESSION_COUNT >= 1,
	     "CONFIG_MPSL_TIMESLOT_SESSION_COUNT defaults to 0, which sizes "
	     "MPSL's session array to nothing: the gate links and boots and "
	     "then never gets air. Set it to 2 (as apps/dongle_thread/"
	     "thread.conf does) - the SoftDevice Controller takes no public "
	     "session but the RRAM flash-sync driver adds one invisibly.");
#endif

/* Margins */

/*
 * How far before the caller's t_from the grant starts: ARM_SETUP overhead plus
 * earliest-grant jitter. The backend's ARM_LEAD_US (168 us nRF54L, 328 us nRF52)
 * is not added here, since the backend's t_from already accounts for it.
 */
#define HEAD_MARGIN_US 250u

/*
 * Tail margin, 250 us. Overstaying asserts, so this is a hard limit: 100 us
 * RADIO ramp-down (RADIO_DISABLE_MAX_US), 50 us clock drift between MPSL's
 * GRTC and the HFCLK-derived TIMER (-99 ppm over 100 ms, charged at 500 ppm),
 * and 100 us signal latency from the TIMER0 compare to ACTION_END.
 */
#define TAIL_MARGIN_US 250u

/*
 * How far ahead a request must be placed to be honourable. 2500 us covers the
 * 1692-1695 us EARLIEST grant latency plus the 2760 us tail sample. Nearer than
 * this the arbiter cannot grant, so a refusal would be self-inflicted.
 */
#define PLACE_LEAD_US CONFIG_RADIANT_GATE_MPSL_PLACE_LEAD_US

/*
 * Too-near floor, the bare hardware minimum. Must stay strictly below
 * PLACE_LEAD_US: the core plans against the advertised lead using its own `now`,
 * while this test samples grant start ~380 us later, so equal values refuse every
 * arm. The advertised lead (gate_min_arm_lead_us()) carries the margin.
 */
#define PLACE_MIN_US CONFIG_RADIANT_GATE_MPSL_PLACE_MIN_US

/*
 * Longest single operation, reported as caps.max_window_us. The scheduler caps only
 * t_close - t_open, so everything added after that must fit under MPSL's ceiling
 * (MPSL_TIMESLOT_LENGTH_MAX_US), or every arm is refused as too long: the head and
 * tail margins, FRAME_TAIL_US (longest frame past t_close; 300 covers coded PHY) and
 * FOLLOW_ON_MAX_US (ack-data reserve).
 */
#define FRAME_TAIL_US    300u
#define FOLLOW_ON_MAX_US 3000u

/*
 * GRANT_OVERHEAD_US includes END_MARGIN_EXTEND_US (defined below) although the
 * fixed class is charged the smaller END_MARGIN_FIXED_US. The ceiling must hold
 * for the largest overhead any class can carry.
 */
#define GRANT_OVERHEAD_US                                                     \
	(HEAD_MARGIN_US + TAIL_MARGIN_US + END_MARGIN_EXTEND_US +             \
	 FRAME_TAIL_US + FOLLOW_ON_MAX_US)

#define MAX_WINDOW_US (MPSL_TIMESLOT_LENGTH_MAX_US - GRANT_OVERHEAD_US)

/*
 * Extension step. An in-flight grant grows itself rather than requesting its
 * full length up front (ADR 0013). esb.c caps its step at 2000 us to bound clock
 * drift; here MAX_WINDOW_US bounds it instead.
 */
#define EXTEND_STEP_US 1000u

/*
 * What an elastic request asks for up front, not what it wants (ADR 0013). The
 * sweep grows the grant with SIGNAL_ACTION_EXTEND. Requesting the whole ~96 ms
 * chunk up front fails: MPSL refuses it and then blocks every later NORMAL
 * request. 10 ms is well below MPSL's maximum, yet the chain is only a few steps.
 */
#define ELASTIC_INITIAL_US 10000u

/*
 * Hand-back margin: how far before a grant's end the TIMER0 compare fires, so
 * ACTION_END returns while the timeslot is still ours. It covers the signal path
 * only; TAIL_MARGIN_US covers the radio. Conflating them asserted on bring-up.
 *
 * END_MARGIN_EXTEND_US (2000) is for the extendable class: its extension must be
 * requested MPSL_TIMESLOT_EXTENSION_MARGIN_MIN_US before the end, so the margin
 * must also hold that minimum (see the BUILD_ASSERTs below). END_MARGIN_FIXED_US
 * (750) is for the non-extendable class (transmits, tracked windows), which never
 * reaches ACTION_EXTEND. Nordic's sample measures the signal path at ~166 us.
 *
 * The margin is charged to the reservation as air held but unused. Too small a
 * margin is an MPSL overstay assert (the board halts), not a dropped packet.
 */
#define END_MARGIN_EXTEND_US 2000u
/* 750 unless the bench arm (assert 69:108) moves it; Kconfig caps it at 2000. */
#define END_MARGIN_FIXED_US  ((uint32_t)CONFIG_RADIANT_GATE_MPSL_ARM_END_MARGIN_FIXED_US)

/*
 * HFCLK configuration of every request. NO_GUARANTEE is a bench arm (assert
 * 69:108), safe only because radiant_radio_init() holds an HFXO request
 * (clock_control_on()) for the life of the image before any operation is
 * programmed; the bootstrap timeslots placed earlier never touch the radio.
 */
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_NO_GUARANTEE)
#define GATE_HFCLK_CFG MPSL_TIMESLOT_HFCLK_CFG_NO_GUARANTEE
#else
#define GATE_HFCLK_CFG MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED
#endif

/*
 * How far ahead of the counter gate_release() places the compare that ends a
 * grant it no longer needs. Big enough for the CAPTURE1, CC0 and MPSL writes to
 * land inside it, small enough to hand back only tens of us of air.
 */
#define RELEASE_LEAD_US 30u

/*
 * Timeout for the anchor-establishing timeslot (bootstrap_anchor()). Generous,
 * since nothing waits on it; far past the measured grant latency.
 */
#define BOOTSTRAP_TIMEOUT_US 1000000u

BUILD_ASSERT(PLACE_MIN_US < PLACE_LEAD_US,
	     "the too-near floor must be strictly below the advertised lead: "
	     "the core plans against the advertised figure using its own `now` "
	     "and the test below runs against a sample taken later, so equal "
	     "values refuse every arm (measured: chunks=0, deny=517013/19s)");
BUILD_ASSERT(EXTEND_STEP_US >= MPSL_TIMESLOT_EXTENSION_TIME_MIN_US,
	     "an extension smaller than MPSL's own minimum is refused, and the "
	     "refusal would read as the arbiter yielding");
BUILD_ASSERT(END_MARGIN_EXTEND_US > MPSL_TIMESLOT_EXTENSION_MARGIN_MIN_US,
	     "an extension is asked for END_MARGIN_EXTEND_US before the end of "
	     "the grant, so that is the margin MPSL's own minimum has to fit "
	     "in. Only the extendable class is bound by it: a fixed-margin "
	     "grant never reaches ACTION_EXTEND (the TIMER0 case is gated on "
	     "g.extendable), which is why END_MARGIN_FIXED_US may be smaller "
	     "than MPSL's extension minimum");
BUILD_ASSERT(ELASTIC_INITIAL_US > END_MARGIN_EXTEND_US + EXTEND_STEP_US,
	     "the first grant must be long enough to contain the compare that "
	     "grows it, or the chain cannot start");
BUILD_ASSERT(END_MARGIN_FIXED_US > TAIL_MARGIN_US,
	     "the end margin must outlast the tail it contains: the compare "
	     "fires END_MARGIN before expiry and the RADIO's own ramp-down and "
	     "clock disagreement (TAIL_MARGIN_US) still have to fit inside what "
	     "is left, or the timeslot is overstayed");
BUILD_ASSERT(END_MARGIN_FIXED_US <= END_MARGIN_EXTEND_US,
	     "the fixed margin is the SHORT one by construction - it drops the "
	     "extension minimum the extendable class has to carry. A FIXED "
	     "larger than EXTEND means the split has been inverted, and "
	     "GRANT_OVERHEAD_US/MAX_WINDOW_US (computed from EXTEND) would then "
	     "under-reserve the class with the bigger overhead");
BUILD_ASSERT(MAX_WINDOW_US > 0u && MAX_WINDOW_US < MPSL_TIMESLOT_LENGTH_MAX_US,
	     "the head and tail margins have eaten the whole grant");

/* State */

/*
 * ADR 0018 (d2): a pool of timeslot sessions; any request goes in whichever is
 * free. With one session this reduces exactly to the single-session gate.
 */
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
#define NSESS 2u
#else
#define NSESS 1u
#endif
#define SESS_NONE 0xFFu

/* What a session's outstanding request is for. */
#define ROLE_NONE 0u /* nothing the core is waiting on (free, or an orphan) */
#define ROLE_OP   1u /* the staged operation is waiting on it (g.op_sess) */
#define ROLE_BOOK 2u /* a booking: placed for an operation not yet armed */

/*
 * Staged numbers for the next grant, committed to `g` at the session's
 * SIGNAL_START. Kept apart from the live grant because a request can be accepted
 * while the other session's grant is running, and writing `g` at acceptance would
 * rewrite that grant's numbers.
 */
struct next_grant {
	uint32_t       want_len_us;
	uint32_t       granted_len_us;
	radiant_time_t grant_end;
	bool           extendable;
	bool           no_extend;
	/* Margin the length was built with; see g.end_margin_us. */
	uint32_t       end_margin_us;
	/* Bench probe: request class, for attributing BLOCKED and START. */
	bool           high_prio;
};

struct sess {
	/* Must outlive the callback (mpsl_timeslot.h). */
	mpsl_timeslot_signal_return_param_t rv;
	mpsl_timeslot_request_t             req;
	mpsl_timeslot_session_id_t          id;
	bool                                open;
	/*
	 * Anchor. distance_us is measured from the previous timeslot's START in this
	 * session. A BLOCKED request started no timeslot, so the anchor must not move.
	 */
	radiant_time_t anchor;
	bool           anchor_valid;
	/*
	 * MPSL owes us an answer to a request placed in this session. A placed request
	 * cannot be withdrawn, so nothing on our side clears this early; see
	 * request_work_fn().
	 */
	volatile bool  mpsl_owes;
	/* req is built; request_work_fn() should place it. */
	volatile bool  want_place;
	/* Placement order, from place_seq_next; see want_place_set(). */
	uint32_t       place_seq;
	/* Anchor-establishing timeslot outstanding. The core hears nothing about its
	 * grant or refusal; see bootstrap_anchor(). */
	volatile bool  bootstrapping;
	volatile uint8_t role;
	/* A booking MPSL refused (or could not place). Its arm is refused synchronously. */
	volatile bool  book_failed;
	/* Air covered by this request: [start, end] as gate_acquire() computed them. */
	radiant_time_t r_start;
	radiant_time_t r_end;
	/* Consecutive refusals before the anchor is abandoned; see BLOCKED. */
	uint32_t       blocked_run;
	/*
	 * Class of the most recently placed request. BLOCKED/CANCELLED can arrive from
	 * inside mpsl_timeslot_request(), before the live grant's numbers are updated,
	 * so the class must be recorded at placement, not read from g.
	 */
	bool           placed_extendable;
	bool           placed_high_prio;
	struct next_grant ng;
	/* MPSL's notice at placement: start - now. */
	int32_t        notice_us;
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
	/* Bench counters. */
	uint32_t       n_placed;
	uint32_t       n_granted;
	uint32_t       n_blocked;
	/* End of this session's last timeslot (start + length asked). Tells a refusal
	 * for notice apart from one for adjacency to the other session's booking. */
	radiant_time_t ts_end;
	/* Whether this request was deferred by -NRF_EAGAIN first. */
	bool           deferred;
	radiant_time_t hold_at;
#endif
	/* Placement held until the other session's running timeslot ends; see place_one(). */
	volatile bool  wait_live;
	/* The core has been told this booking's start is close. */
	bool           due_told;
	/* Scan tail of a booking (ADR 0021 decision 5), and whether a refused booking
	 * was already re-placed without it. */
	uint16_t       book_tail_us;
	bool           book_replaced;
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_REQ_DISCIPLINE)
	/* Bench arm (69:108): SESSION_IDLE seen since this session's last START. */
	volatile bool  idle_seen;
#endif
};

static struct sess ss[NSESS];

static bool build_normal(struct sess *s, radiant_time_t start, radiant_time_t end,
			 uint64_t len, bool extendable, bool no_extend,
			 uint32_t end_margin_us, uint8_t prio);
/* How many sessions opened. 1 means the single-session gate. */
static uint8_t     n_open;

/* The primary session is open: the gate is up. */
#define session_open (ss[0].open)

static inline struct sess *sess_of(mpsl_timeslot_session_id_t id)
{
	for (uint32_t i = 0u; i < NSESS; i++) {
		if (ss[i].open && ss[i].id == id) {
			return &ss[i];
		}
	}
	return &ss[0];
}

static inline uint8_t sess_idx(const struct sess *s)
{
	return (uint8_t)(s - &ss[0]);
}

/* Trampoline subscription: taken once per boot and never released; see gate_init(). */
static bool                       swi_registered;

static struct {
	/* A reservation is outstanding: placed and not yet resolved. */
	volatile bool pending;
	/* A grant is held: SIGNAL_START arrived and ACTION_END has not. */
	volatile bool granted;
	/*
	 * The hardware loan: MPSL has given us the radio and we have not returned it.
	 * Set at SIGNAL_START and cleared only by timer0_disarm(), which is idempotent,
	 * so each granted timeslot gets exactly one on_grant_end(). Teardown paths must
	 * key on this flag, not on `granted`: gate_release() clears `granted` early, so
	 * a lost TIMER0 compare would skip on_grant_end(), and the 802.15.4 driver's
	 * SUBSCRIBE_RXEN restore would never run.
	 */
	volatile bool hw_held;
	/* The core has finished with the grant and wants it given back. */
	volatile bool release_wanted;
	/* This operation may grow itself (the elastic classes). */
	volatile bool extendable;
	/* Placed during an extension skip, so sized as non-extendable: it ends at its
	 * first compare and never asks, even if the skip lapses while it runs. */
	volatile bool no_extend;
	/*
	 * Hand-back margin this grant was reserved with; the only margin any compare for
	 * it may use. END_MARGIN_EXTEND_US when extendable, else END_MARGIN_FIXED_US.
	 * Carried in state because the reservation was already sized with it.
	 */
	uint32_t end_margin_us;

	/* Session the staged operation waits on (valid while `pending`), and session
	 * whose timeslot is running (valid while `ts_live`). Always 0 with one session. */
	volatile uint8_t op_sess;
	volatile uint8_t live_sess;
	/* A timeslot of ours is running: from SIGNAL_START until ACTION_END, or until
	 * that session's SESSION_IDLE/CLOSED backstop. MPSL never runs two of our
	 * timeslots at once, so the radio state below describes this one. */
	volatile bool    ts_live;

	/*
	 * Last instant the radio may be busy under the grant we hold, not the timeslot
	 * expiry. Taken from the GRANT rather than the request, so a follow-on is never
	 * admitted against air not yet granted.
	 */
	radiant_time_t grant_end;
	/* What we hold right now, grown by the extension chain. */
	uint32_t       granted_len_us;
	/* What the operation wants. The elastic class asks for ELASTIC_INITIAL_US and
	 * extends toward this; everything else has the two equal. */
	uint32_t       want_len_us;
	/* Length of the outstanding extension: EXTEND_STEP_US, or the remainder. */
	uint32_t       ext_step_us;
	/* mpsl_owes (per session) is not `pending`. The backstop answers "no grant
	 * coming" for the core, but it must not clear mpsl_owes: a placed request cannot
	 * be withdrawn, so the backstop must not reopen the placement path. */
	volatile bool  deny_wanted;
#define DENY_SRC_BLOCKED     0u
#define DENY_SRC_AFTER_GRANT 1u
#define DENY_SRC_DEADLINE    2u
	/* Which submitter set deny_wanted (DENY_SRC_*). Diagnostic only; last writer wins. */
	volatile uint8_t deny_src;
	/*
	 * ACTION_END already returned for this grant. A grant can end while a RADIO event
	 * is still pending. MPSL then delivers that event as another signal, which must
	 * be handled but not answered (a second action asserts). The two orders race, so
	 * both must be safe.
	 */
	volatile bool  ended;
} g;

static struct {
	/* Every gate_acquire() call, before any reason it could be refused. With
	 * `placed`, this separates "the arbiter refused us" from "nobody asked". */
	uint32_t acquires;
	uint32_t acq_in_grant;
	uint32_t acq_in_grant_denied;
	uint32_t placed;
	uint32_t granted;
	uint32_t blocked;
	uint32_t cancelled;
	uint32_t refused_eagain;
	uint32_t refused_too_near;
	uint32_t refused_too_long;
	/* Which `return GATE_DENIED` fired. den_pending: re-armed faster than one
	 * placement completes. den_no_anchor: the anchor needs re-bootstrapping. */
	uint32_t den_pending;
	uint32_t den_no_anchor;
	uint32_t den_distance;
	uint32_t den_degenerate;
	uint32_t den_no_session;
	/* Refused because MPSL still owed an answer. See g.mpsl_owes. */
	uint32_t den_owed;
	uint32_t extends_ok;
	uint32_t extends_failed;
	/* Elastic grants ended at their first compare without asking, because
	 * the arbiter had refused the last few extensions (ext_skip). */
	uint32_t ext_skipped;
	/* Backstop fired before SIGNAL_BLOCKED. Expected zero; non-zero means the
	 * early-BLOCKED measurement did not generalise. */
	uint32_t deadline_fired;
	/*
	 * Delivered denials (DONE_DENIED), by cause:
	 *   sent_after_grant  grant ended before the window finished. Fix: length or extension.
	 *   sent_deadline     backstop beat the grant. Fix: placement lead.
	 *   sent_blocked      MPSL refused: the only true contention.
	 *   deny_suppressed   g.pending guard caught a re-arm; not a denial.
	 */
	uint32_t sent_after_grant;
	uint32_t sent_deadline;
	uint32_t sent_blocked;
	uint32_t deny_suppressed;
	/* Signals that mean this file is wrong. Kept apart because they have opposite
	 * fixes; printed by mpsl_assert_handle(). */
	uint32_t overstayed;
	uint32_t invalid_return;
	uint32_t unknown_signal;
	/*
	 * Soak-test counters:
	 *   grant_end_calls  on_grant_end() calls; should be one per granted timeslot.
	 *   late_disarm      BLOCKED/CANCELLED while the radio was on loan. Should be
	 *                    impossible; non-zero is a real fault.
	 *   idle_disarm      SESSION_IDLE/CLOSED returned a radio still on loan. Normal
	 *                    when MPSL ended a timeslot by length expiry.
	 *   release_disarm   gate_release() returned the radio itself (the normal path).
	 */
	uint32_t grant_end_calls;
	uint32_t late_disarm;
	uint32_t idle_disarm;
	uint32_t release_disarm;
	/* Requests MPSL answered from inside mpsl_timeslot_request() (see place_one()). */
	uint32_t answered_inline;
	/* Last signal seen, for asserts that are neither OVERSTAYED nor INVALID_RETURN. */
	uint32_t last_signal;
	/* Times each signal that can end a grant was delivered. sig_radio at zero means
	 * the RADIO never raised an event inside a timeslot. */
	uint32_t sig_radio;
	uint32_t sig_timer0;
	/* The same counters for high-priority (tracked window and transmit) requests. */
	uint32_t hi_acq;
	uint32_t hi_pending;  /* ...that got as far as a placed reservation */
	uint32_t hi_reserve;  /* ...served from air already held */
	uint32_t hi_blocked;  /* ...refused by the arbiter */
	uint32_t hi_granted;  /* ...that actually got the air */
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
	/*
	 * ADR 0018 booking counters: book_placed; book_hit (matched by its arm);
	 * book_denied (arm refused because MPSL refused the booking); book_blocked and
	 * book_unplaced (MPSL refused, or not placeable); book_mismatch (arm overlapped
	 * but did not fit); book_early (granted before its op was staged; expected 0);
	 * book_busy and book_near (gate_book() refused for want of a session or notice).
	 */
	uint32_t book_placed;
	uint32_t book_hit;
	uint32_t book_denied;
	uint32_t book_blocked;
	uint32_t book_unplaced;
	uint32_t book_mismatch;
	uint32_t book_early;
	uint32_t book_busy;
	uint32_t book_near;
	/* Tailed bookings re-placed at plain length. */
	uint32_t book_replaced;
	/* Grants with nothing staged on their session: handed straight back. */
	uint32_t orphan_start;
	/* Acquires refused because every session had a request out. */
	uint32_t den_nosess;
	/* Elastic requests BLOCKED, and the elastic lead's range. */
	uint32_t el_blocked;
	uint32_t el_lead_max;
	uint32_t el_lead_min;
	/* Bench diagnostics. Elastic refusals whose timeslot ended within 3 ms of the
	 * other session's booked start (count, and smallest gap); early booking grants
	 * with an op staged on the other session (early_other) or none (early_none);
	 * booking hits less than 1 ms before the booked start (hit_late). */
	uint32_t el_blk_adj;
	int32_t  el_blk_gap_min;
	uint32_t early_other;
	uint32_t early_none;
	uint32_t hit_late;
	/* Elastic refusals and grants by notice MPSL got: <1, 1-2, 2-4, 4-8, >=8 ms.
	 * el_blk_deferred counts refusals that were first deferred by -NRF_EAGAIN. */
	uint32_t el_blk_by_notice[5];
	uint32_t el_gr_by_notice[5];
	uint32_t el_blk_deferred;
	/* Placements held back by a running timeslot on the other session. */
	uint32_t defer_live;
	/* Elastic requests dropped locally because their notice fell below
	 * EL_NOTICE_MIN_US while they waited; the core re-arms them at a fresh lead. */
	uint32_t el_late_drop;
	/* Fixed-grant release timing (rel_fixed_*, last and max, us after START), and
	 * held-back placement wait (hold_*). */
	/* gate_release() compare outcomes: rel_br_ok (moved), rel_br_inside (already
	 * inside the end margin), rel_br_restore (lost the race, backstop restored). */
	/* Fixed-grant TIMER0 signals: released by the core (t0_fix_rel) or the grant's
	 * own backstop (t0_fix_backstop). */
	uint32_t skip_left_min;
	int32_t  start_err_last;
	int32_t  start_err_min;
	int32_t  start_err_max;
	uint32_t start_err_n;
	uint32_t t0_fix_rel;
	uint32_t t0_fix_backstop;
	uint32_t t0_fix_at;
	uint32_t rel_br_ok;
	uint32_t rel_br_inside;
	uint32_t rel_br_restore;
	uint32_t rel_cap_last;
	uint32_t rel_est_last;
	uint32_t rel_fixed_last;
	uint32_t rel_fixed_max;
	uint32_t hold_last;
	uint32_t hold_max;
	/*
	 * Assert 69:108 evidence. High-priority ACTION_ENDs returned (end_hi) and those
	 * that found the RADIO vector pending when the gate cleared it (unpend), by the
	 * signal that ended the timeslot: END_SRC_*. Each unpend is an assert avoided.
	 */
#define END_SRC_START     0u
#define END_SRC_T0_SKIP   1u /* TIMER0 ext-skip (or the EL_FIXED arm's cut) */
#define END_SRC_T0_OTHER  2u
#define END_SRC_RADIO     3u
#define END_SRC_EXT_FAIL  4u
#define END_SRC_N         5u
	uint32_t end_hi[END_SRC_N];
	uint32_t unpend[END_SRC_N];
#endif
} stats;

/* A bench counter: gone unless a bench image asked for it. */
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
#define GSTAT(x) do { x; } while (0)
#else
#define GSTAT(x) do { } while (0)
#endif

/*
 * Trace ring, not printk: a printk here costs ~87 us/char and moves the timing
 * it measures. Each entry is the signal (high nibble) and the action returned
 * (low nibble: 0 NONE, 1 END, 2 EXTEND).
 */
#if !defined(CONFIG_RADIANT_GATE_MPSL_STATS)
#define TRACE_LEN 16u
static uint8_t  trace[TRACE_LEN];
static uint32_t trace_at;

static inline void trace_put(uint32_t signal, uint32_t action)
{
	trace[trace_at % TRACE_LEN] =
		(uint8_t)(((signal & 0xFu) << 4) | (action & 0xFu));
	trace_at++;
}
#define TRACE_SIGNAL(si, signal, action) trace_put((signal), (action))
#else
/*
 * Bench images (assert 69:108): a longer trace that also names the session and
 * the time since the previous entry, and records each mpsl_timeslot_request()
 * as a pseudo-entry (TRACE_REQ), so requests interleave with signals.
 *
 * Writers: on_signal() at high priority (zero-latency) and in MPSL's
 * low-priority thread, and place_one() on the gate's queue. The index is taken
 * and the entry written under radiant_swi_zli_lock(), which also masks the
 * zero-latency callback, so no writer can tear another's index. The index is
 * only ever used modulo TRACE_LEN, so even a lost entry cannot go out of range.
 */
#define TRACE_LEN     32u
#define TRACE_REQ     0x80u /* hdr: a request pseudo-entry; low bits: session */
#define TRACE_RC_WAIT 127   /* act of a request entry whose call has not returned */
struct trace_ent {
	uint8_t  hdr;   /* TRACE_REQ | session index */
	uint8_t  sig;   /* signal (0 on a request entry) */
	int8_t   act;   /* action returned; on a request entry the rc, saturated */
	uint8_t  rsvd;
	uint16_t dt_us; /* since the previous entry, saturating */
	int16_t  aux;   /* request entry: notice (start - now) in 4 us units, saturating */
};
static struct trace_ent trace[TRACE_LEN];
static uint32_t         trace_at;
/* Low 32 bits of radiant_radio_now() at the last entry. */
static uint32_t         trace_t_last;

static uint32_t trace_write(uint8_t hdr, uint8_t sig, int8_t act, int16_t aux)
{
	const uint32_t zkey = radiant_swi_zli_lock();
	const uint32_t now = (uint32_t)radiant_radio_now();
	const uint32_t dt = now - trace_t_last;
	const uint32_t at = trace_at;
	struct trace_ent *const e = &trace[at % TRACE_LEN];

	e->hdr = hdr;
	e->sig = sig;
	e->act = act;
	e->rsvd = 0u;
	e->dt_us = (dt > UINT16_MAX) ? UINT16_MAX : (uint16_t)dt;
	e->aux = aux;
	trace_t_last = now;
	trace_at = at + 1u;
	radiant_swi_zli_unlock(zkey);
	return at;
}

static inline int8_t trace_sat8(int32_t v)
{
	return (int8_t)((v > 126) ? 126 : (v < -128) ? -128 : v);
}

static inline int16_t trace_sat16(int32_t v)
{
	return (int16_t)((v > INT16_MAX) ? INT16_MAX
			 : (v < INT16_MIN) ? INT16_MIN : v);
}

/* Fill in a request entry's rc once the call returns, unless it was overwritten. */
static inline void trace_req_rc(uint32_t at, int32_t rc)
{
	if ((uint32_t)(trace_at - at) < TRACE_LEN) {
		trace[at % TRACE_LEN].act = trace_sat8(rc);
	}
}
#define TRACE_SIGNAL(si, signal, action)                                     \
	(void)trace_write((uint8_t)((si) & 0x7Fu), (uint8_t)(signal),        \
			  (int8_t)(action), 0)

/*
 * Bench state the assert handler reads (assert 69:108). Low 32 bits of
 * radiant_radio_now(); the handler takes differences, so a wrap every ~71 min
 * costs nothing. Written by plain stores; a torn read only costs a diagnostic.
 */
#define API_NONE    0u
#define API_REQUEST 1u
#define API_OPEN    2u
#define API_CLOSE   3u
static struct {
	volatile uint8_t  last_sess;  /* session of the last signal */
	volatile uint32_t t_start;    /* last SIGNAL_START */
	volatile uint32_t t_end;      /* last ACTION_END returned */
	volatile uint8_t  api;        /* API_* in flight, or API_NONE */
	volatile uint8_t  api_sess;
	volatile uint8_t  api_last;   /* last API_* called */
	volatile int32_t  api_rc;     /* its return code */
	volatile uint32_t api_t_in;
	volatile uint32_t api_t_out;
	volatile uint32_t api_calls;
} dbg;

static inline void api_enter(uint8_t api, uint8_t si)
{
	dbg.api_sess = si;
	dbg.api_last = api;
	dbg.api_t_in = (uint32_t)radiant_radio_now();
	dbg.api_calls++;
	dbg.api = api;
}

static inline void api_exit(int32_t rc)
{
	dbg.api_rc = rc;
	dbg.api_t_out = (uint32_t)radiant_radio_now();
	dbg.api = API_NONE;
}
#endif /* CONFIG_RADIANT_GATE_MPSL_STATS */

#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
#define API_ENTER(api, si) api_enter((api), (si))
#define API_EXIT(rc)       api_exit(rc)
#else
#define API_ENTER(api, si) do { } while (0)
#define API_EXIT(rc)       do { } while (0)
#endif

/*
 * Bench arm (assert 69:108): the timeslot API under MPSL's own mutex, as
 * nrf/drivers/mpsl/flash_sync/flash_sync_mpsl.c calls it. A k_mutex, so thread
 * context only, which every caller is: place_one() runs only from
 * request_work_fn() on gate_wq (or its delayed twin below), session_open from
 * gate_init() in the thread running antr_init(), session_close from
 * gate_shutdown() on radiant_radio_init()'s failure path. A BLOCKED that MPSL
 * delivers from inside mpsl_timeslot_request() reaches on_signal() with the
 * lock held; that branch calls no MPSL API, and the mutex is recursive anyway.
 */
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_LOCK)
#include <multithreading_lock.h>
#define ARM_LOCK()                                                            \
	do {                                                                  \
		__ASSERT(!k_is_in_isr(), "ARM_LOCK from an ISR");             \
		(void)MULTITHREADING_LOCK_ACQUIRE();                          \
	} while (0)
#define ARM_UNLOCK() MULTITHREADING_LOCK_RELEASE()
#else
#define ARM_LOCK()   do { } while (0)
#define ARM_UNLOCK() do { } while (0)
#endif

/* Bench arm counters (assert 69:108): printed by the assert dump and the stats log. */
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_REQ_DISCIPLINE) ||                  \
	defined(CONFIG_RADIANT_GATE_MPSL_ARM_EL_FIXED) ||                    \
	defined(CONFIG_RADIANT_GATE_MPSL_ARM_DPPI_FIRST)
#define ARM_COUNTERS 1
static struct {
	uint32_t held_idle;    /* REQ_DISCIPLINE: request held for SESSION_IDLE */
	uint32_t held_gap;     /* REQ_DISCIPLINE: request held for the END gap */
	uint32_t el_fixed_cut; /* EL_FIXED: fixed first bite cut a longer chunk */
	uint32_t dppi_first;   /* DPPI_FIRST: connections detached before hand-back */
} arm_stats;
#endif

/*
 * Bench arm (assert 69:108): take the operation's RADIO (D)PPI connections away
 * before timer0_disarm() returns the radio, so no compare of ours can trigger a
 * RADIO task on the next holder's radio. Register writes only (DPPIC CHENCLR).
 */
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_DPPI_FIRST)
#define DPPI_DETACH_FIRST()                                                   \
	do {                                                                  \
		radiant_nrf_gate_dppi_detach();                               \
		arm_stats.dppi_first++;                                       \
	} while (0)
#else
#define DPPI_DETACH_FIRST() do { } while (0)
#endif

/*
 * The three MPSL timeslot API calls, in one place each, so the bench probes
 * (and any lock around them) cannot miss a call site. Every caller is a thread.
 */
static mpsl_timeslot_signal_return_param_t *on_signal(
	mpsl_timeslot_session_id_t id, uint32_t signal);

static inline int32_t ts_request(struct sess *s)
{
	int32_t rc;

	API_ENTER(API_REQUEST, sess_idx(s));
	ARM_LOCK();
	rc = mpsl_timeslot_request(s->id, &s->req);
	ARM_UNLOCK();
	API_EXIT(rc);
	return rc;
}

static inline int32_t ts_session_open(struct sess *s)
{
	int32_t rc;

	API_ENTER(API_OPEN, sess_idx(s));
	ARM_LOCK();
	rc = mpsl_timeslot_session_open(on_signal, &s->id);
	ARM_UNLOCK();
	API_EXIT(rc);
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_REQ_DISCIPLINE)
	/* A new session has had no START: its first request may go at once. */
	s->idle_seen = true;
#endif
	return rc;
}

static inline int32_t ts_session_close(struct sess *s)
{
	int32_t rc;

	API_ENTER(API_CLOSE, sess_idx(s));
	ARM_LOCK();
	rc = mpsl_timeslot_session_close(s->id);
	ARM_UNLOCK();
	API_EXIT(rc);
	return rc;
}

/*
 * Backstop deadline timer. Not the primary path: BLOCKED has been seen 11-31 ms
 * before the requested start, so it cannot be relied on alone. Being wrong about
 * that wedges the core's single operation slot.
 */

/*
 * How long past the operation's start the backstop waits. Must not be near zero:
 * MPSL answers from a cooperative thread that a connectable advertiser can starve.
 * A backstop that fires first denies while MPSL still owns the session, and the
 * core re-arms into an -NRF_EAGAIN loop. 20 ms outlasts the block band and any
 * advertiser delay, and is well below that wedge.
 */
#define DEADLINE_SLACK_US 20000u

static void deadline_expired(struct k_timer *t);
static K_TIMER_DEFINE(deadline, deadline_expired, NULL);

/*
 * Arm and disarm requests arrive from the zero-latency signal callback, where
 * k_timer_start()/k_timer_stop() are illegal. The callback records the request
 * here and pends the trampoline; deadline_apply() runs it in ordinary interrupt
 * context. Last writer wins: both requests concern one reservation, and a lost
 * race costs at most a spurious denial, which denied_work_fn() suppresses.
 */
#define DEADLINE_REQ_NONE 0u
#define DEADLINE_REQ_ARM  1u
#define DEADLINE_REQ_STOP 2u
static atomic_t deadline_req;
static atomic_t deadline_req_us;

static inline void deadline_arm(uint32_t us)
{
	atomic_set(&deadline_req_us, (atomic_val_t)us);
	atomic_set(&deadline_req, (atomic_val_t)DEADLINE_REQ_ARM);
	radiant_swi_pend(RADIANT_SWI_GATE_DEADLINE);
}

static inline void deadline_stop(void)
{
	atomic_set(&deadline_req, (atomic_val_t)DEADLINE_REQ_STOP);
	radiant_swi_pend(RADIANT_SWI_GATE_DEADLINE);
}

static void deadline_apply(void)
{
	atomic_val_t req = atomic_and(&deadline_req, 0);

	if (req == (atomic_val_t)DEADLINE_REQ_ARM) {
		k_timer_start(&deadline,
			      K_USEC((uint32_t)atomic_get(&deadline_req_us)),
			      K_NO_WAIT);
	} else if (req == (atomic_val_t)DEADLINE_REQ_STOP) {
		k_timer_stop(&deadline);
	}
}

/*
 * Only programming the peripheral happens inside the grant's signal callback. The
 * rest is deferred through the trampoline (a software interrupt) to gate_wq:
 *   - mpsl_timeslot_request() is not callable from a timeslot or the RADIO
 *     interrupt.
 *   - deliver_terminal() runs a scheduler pass, which is unbounded work.
 *
 * gate_wq is dedicated, not k_sys_work_q, so placement is not delayed behind
 * unrelated work. It owns its stack: the system queue's stack is overridden to
 * 1120 bytes on this image, and too shallow a stack reboots it.
 * CONFIG_RADIANT_GATE_WQ_PRIO matches the system queue, so the queue isolates
 * contention only.
 *
 * NO KERNEL CALL FROM THE SIGNAL CALLBACK. MPSL's TIMER0/RADIO/RTC0 vectors run at
 * IRQ_ZERO_LATENCY, above the priority irq_lock() and spinlocks mask. Deferral must
 * be a software interrupt, not a poll thread (its k_msleep() would touch the timeout
 * list). The ANT completion path (radiant_event_wakeup()) also runs in this callback
 * and is deferred too. See radiant/include/radiant/radiant_swi.h.
 */

static void request_work_fn(struct k_work *w);
static void denied_work_fn(struct k_work *w);

static K_WORK_DEFINE(request_work, request_work_fn);
static K_WORK_DEFINE(denied_work, denied_work_fn);

/* The queue: started once per boot by gate_init(), never stopped. Costs CONFIG_RADIANT_GATE_WQ_STACK_SIZE of RAM. */
static struct k_work_q gate_wq;
static K_THREAD_STACK_DEFINE(gate_wq_stack, CONFIG_RADIANT_GATE_WQ_STACK_SIZE);
static bool gate_wq_started;

/*
 * The gate's end of the trampoline. Runs at CONFIG_RADIANT_SWI_IRQ_PRIO, an
 * ordinary interrupt priority, so the k_work and k_timer calls here are legal.
 * Work is still submitted to gate_wq, not run inline: deliver_terminal() and the
 * scheduler pass are unbounded.
 */
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
static void booking_due_fire(void);
static void booking_due_stop(void);
#endif

static void gate_swi_handler(uint32_t bits)
{
	if ((bits & RADIANT_SWI_GATE_DEADLINE) != 0u) {
		deadline_apply();
	}
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
	if ((bits & RADIANT_SWI_GATE_DUE) != 0u) {
		booking_due_fire();
	}
#endif
	if ((bits & RADIANT_SWI_GATE_REQUEST) != 0u) {
		k_work_submit_to_queue(&gate_wq, &request_work);
	}
	if ((bits & RADIANT_SWI_GATE_DENY) != 0u) {
		k_work_submit_to_queue(&gate_wq, &denied_work);
	}
}

/* Class of the request MPSL currently holds, for the probe counters. */
static bool grant_high_prio;

/*
 * Size of the elastic class's first bite (ADR 0013). A fixed 10 ms request does
 * badly against a 100 ms advertiser, while a tracked window coexists. So the
 * first bite halves on each BLOCKED, down to ELASTIC_FLOOR_US, and grows back by
 * one EXTEND_STEP_US per grant. Not applied to tracked windows or transmits:
 * their length is the core's choice, and shortening one hands back air mid-packet.
 */
#define ELASTIC_FLOOR_US 2500u
static uint32_t elastic_initial_us = ELASTIC_INITIAL_US;

/*
 * Phase shift for refused elastic requests. Backing off the length does not help:
 * a scan chunk re-asks in phase with a 100 ms advertiser, and the anchor does not
 * move on BLOCKED. Each refusal adds a prime step (2137 us, which visits every offset
 * before repeating) over a span a little over one ANT+ slot. Only the elastic class
 * shifts. Advanced here but NOT applied to a placement (see gate_acquire()).
 */
#define ELASTIC_SKEW_STEP_US 2137u
#define ELASTIC_SKEW_SPAN_US 13000u
static uint32_t elastic_skew_us;

/* Consecutive refusals before the anchor is abandoned; counted per session (struct sess). */
#define BLOCKED_RUN_REANCHOR 4u

/*
 * Stop asking for extensions the arbiter keeps refusing (ADR 0021, decision 3).
 * After EXT_FAIL_RUN_OFF refusals in a row, the next EXT_SKIP_GRANTS elastic grants
 * end at their first compare without asking, then one is asked for again. Such a
 * grant is sized as non-extendable (END_MARGIN_FIXED_US), since the window is still
 * open at the compare.
 */
#define EXT_FAIL_RUN_OFF 3u
#define EXT_SKIP_GRANTS  64u
static uint32_t ext_fail_run;
static uint32_t ext_skip;

BUILD_ASSERT(ELASTIC_FLOOR_US >= MPSL_TIMESLOT_LENGTH_MIN_US,
	     "the elastic floor must still be a timeslot MPSL will grant");
BUILD_ASSERT(ELASTIC_FLOOR_US > END_MARGIN_EXTEND_US,
	     "an elastic grant must outlive its own hand-back margin");

/*
 * Elastic lead (ADR 0021, decision 4). A scan chunk in a free session needs less
 * than PLACE_LEAD_US. The lead is OPEN (D2_SCAN_LEAD_US), or CONTENDED
 * (D2_CONTENDED_LEAD_US) after a run of refused extensions, until one is granted.
 * Keyed on extensions, not refused requests: a BLE advertiser refuses at any notice.
 * A long unbroken run of refused requests (EL_BLOCK_RUN_CONTENDED) also counts.
 */
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
#define EL_LEAD_START_US     CONFIG_RADIANT_GATE_MPSL_D2_SCAN_LEAD_US
#define EL_LEAD_CONTENDED_US CONFIG_RADIANT_GATE_MPSL_D2_CONTENDED_LEAD_US
#define EL_BLOCK_RUN_CONTENDED 16u
static uint32_t el_block_run;
/* Elastic requests with less notice than this are refused by MPSL even with no other stack. */
#define EL_NOTICE_MIN_US 2000u
static uint32_t el_lead_us = EL_LEAD_START_US;
static uint32_t scan_tail_us = CONFIG_RADIANT_GATE_MPSL_D2_SCAN_TAIL_US;
/* Booking is live: two sessions opened. */
static bool d2_on;

static void el_lead_publish(void)
{
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
	if (el_lead_us > stats.el_lead_max) {
		stats.el_lead_max = el_lead_us;
	}
	if (stats.el_lead_min == 0u || el_lead_us < stats.el_lead_min) {
		stats.el_lead_min = el_lead_us;
	}
#endif
	/* The core plans against grant start clearing the lead: lead + HEAD_MARGIN_US,
	 * as gate_min_arm_lead_us() advertises. */
	radiant_nrf_gate_set_elastic_lead(d2_on ? el_lead_us + HEAD_MARGIN_US : 0u);
	radiant_nrf_gate_set_scan_tail(d2_on ? scan_tail_us : 0u);
}

/* From a signal, so no kernel call: this writes the caps struct and nothing else. */
static void el_lead_state(bool contended)
{
	const uint32_t lead = contended ? EL_LEAD_CONTENDED_US : EL_LEAD_START_US;

	if (!IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_LEAD_SWEEP) && lead != el_lead_us) {
		el_lead_us = lead;
		el_lead_publish();
	}
}

/* Too-near floor for elastic requests: three quarters of the lead, as PLACE_MIN_US is of PLACE_LEAD_US. */
static inline uint32_t el_floor_us(void)
{
	return (el_lead_us * 3u) / 4u;
}
#else
#define d2_on false
#endif

/* Commit a session's staged numbers as the live grant's. At SIGNAL_START. */
static void commit_next_grant(const struct sess *s)
{
	g.want_len_us    = s->ng.want_len_us;
	g.granted_len_us = s->ng.granted_len_us;
	g.grant_end      = s->ng.grant_end;
	g.extendable     = s->ng.extendable;
	g.no_extend      = s->ng.no_extend;
	g.end_margin_us  = s->ng.end_margin_us;
	grant_high_prio  = s->ng.high_prio;
}

/* The staged operation will not be served by its request (aborted, forgotten, or
 * its backstop fired). The request is still MPSL's, so the session stays busy
 * until it is answered. Its window is kept as a booking, so an operation re-armed
 * into the same air is staged behind it. A grant nobody arms into is handed back
 * (orphan_start). */
static void op_forget(void)
{
	struct sess *s;
	/* Masked: a START between reading mpsl_owes and writing role would leave a booking with no request. */
	const uint32_t zkey = radiant_swi_zli_lock();

	if (g.pending) {
		g.pending = false;
		s = &ss[g.op_sess];
		if (s->role == ROLE_OP) {
			s->role = (s->mpsl_owes || s->want_place) ? ROLE_BOOK
								  : ROLE_NONE;
		}
	}
	radiant_swi_zli_unlock(zkey);
}

/* Places a reservation. Thread context, so the MPSL call is legal. */
static void place_one(struct sess *s);

#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
/* Bench diagnostic bins for the notice a request was placed with. */
static inline uint32_t notice_bin(int32_t us)
{
	return (us < 1000) ? 0u : (us < 2000) ? 1u : (us < 4000) ? 2u
		: (us < 8000) ? 3u : 4u;
}
#endif

/*
 * Requests reach MPSL in the order the core made them. MPSL is first come, first
 * served on overlapping air, so the order of two waiting requests decides which
 * is refused. Placing by session index instead let a later channel's booking
 * starve an earlier channel's fresh request. The core puts the established
 * operation first (ADR 0013 rule 1).
 */
static uint32_t place_seq_next;

static void want_place_set(struct sess *s)
{
	s->place_seq = place_seq_next++;
	s->want_place = true;
}

static void request_work_fn(struct k_work *w)
{
	bool tried[NSESS] = {false};

	ARG_UNUSED(w);

	for (uint32_t n = 0u; n < NSESS; n++) {
		struct sess *first = NULL;

		for (uint32_t i = 0u; i < NSESS; i++) {
			if (ss[i].open && ss[i].want_place && !tried[i] &&
			    (first == NULL ||
			     (int32_t)(ss[i].place_seq - first->place_seq) < 0)) {
				first = &ss[i];
			}
		}
		if (first == NULL) {
			return;
		}
		tried[sess_idx(first)] = true;
		place_one(first);
	}
}

#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_REQ_DISCIPLINE)
/*
 * Bench arm (assert 69:108). Low 32 bits of radiant_radio_now() at the last
 * ACTION_END returned on either session, written by on_signal(). One aligned
 * word, so the thread reads it whole. A false hold needs the 71-minute wrap to
 * land inside the gap; it costs one retry.
 */
static volatile uint32_t req_t_end32;
static volatile bool     req_end_seen;

/* The gap retry: the same pass request_work_fn() makes, on the same queue. */
static void req_gap_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	request_work_fn(NULL);
}
static K_WORK_DELAYABLE_DEFINE(req_gap_work, req_gap_fn);
#endif

static void place_one(struct sess *s)
{
	int32_t rc;

	/*
	 * Do not place while the other session's timeslot is still scheduled over this
	 * request's start. MPSL would judge the request at once against that timeslot's
	 * whole booked length, including its follow-on reserve, and refuse it. (With one
	 * session, -NRF_EAGAIN holds the request until SESSION_IDLE.) Hold it instead:
	 * the ACTION_END in on_signal() re-pends placement. wait_live is set before the
	 * test, so an end between the test and the return still re-pends.
	 */
	if (!s->bootstrapping && g.ts_live && g.live_sess != sess_idx(s)) {
		s->wait_live = true;
		if (g.ts_live && g.live_sess != sess_idx(s) &&
		    ss[g.live_sess].anchor + (radiant_time_t)g.granted_len_us +
				    300u >
			    s->r_start) {
			GSTAT(stats.defer_live++);
			GSTAT(s->hold_at = radiant_radio_now());
			return;
		}
	}
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
	if (s->wait_live && s->hold_at != 0u) {
		uint32_t d = (uint32_t)(radiant_radio_now() - s->hold_at);

		stats.hold_last = d;
		if (d > stats.hold_max) {
			stats.hold_max = d;
		}
		s->hold_at = 0u;
	}
#endif
	s->wait_live = false;
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
	/*
	 * A scan chunk whose notice ran out while it waited would only be refused by
	 * MPSL, so refuse it here instead. The core re-arms it at a fresh lead, and no
	 * BLOCKED is counted against the arbiter.
	 */
	if (s->role == ROLE_OP && s->ng.extendable && d2_on &&
	    (int64_t)(s->r_start - radiant_radio_now()) < (int64_t)EL_NOTICE_MIN_US) {
		s->want_place = false;
		s->role = ROLE_NONE;
		GSTAT(stats.el_late_drop++);
		if (g.pending && g.op_sess == sess_idx(s)) {
			g.pending = false;
			deadline_stop();
			radiant_nrf_gate_on_denied();
		}
		return;
	}
#endif
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_REQ_DISCIPLINE)
	/*
	 * Bench arm (assert 69:108). want_place stays set on both holds. Rule 1: a
	 * session that has had a START is asked again only after its SESSION_IDLE,
	 * which re-pends placement (so -NRF_EAGAIN is never provoked). Rule 2: no
	 * request within GAP_US of any returned ACTION_END; retried from delayable
	 * work on this queue (thread context, so k_work is legal here). An END
	 * landing between this test and the call is not excluded: masking the
	 * zero-latency callback across an MPSL call is not an option.
	 */
	if (!s->idle_seen) {
		arm_stats.held_idle++;
		return;
	}
	if (req_end_seen) {
		const uint32_t since =
			(uint32_t)radiant_radio_now() - req_t_end32;

		if (since < (uint32_t)CONFIG_RADIANT_GATE_MPSL_ARM_REQ_GAP_US) {
			arm_stats.held_gap++;
			(void)k_work_schedule_for_queue(
				&gate_wq, &req_gap_work,
				K_USEC(CONFIG_RADIANT_GATE_MPSL_ARM_REQ_GAP_US -
				       since));
			return;
		}
	}
#endif

	/*
	 * mpsl_owes must be armed BEFORE the call. mpsl_timeslot_request() can deliver
	 * BLOCKED synchronously, from inside this call. If the flag were set after a
	 * successful return, that answer would clear it first, and the later set would
	 * leave it stale forever: a placed request cannot be withdrawn, so every later
	 * acquire would be denied as owed. Armed first, both orders are safe. Only the
	 * not-placed case is undone below.
	 */
	s->placed_extendable = s->ng.extendable;
	s->placed_high_prio = s->ng.high_prio;
	s->notice_us = (int32_t)(int64_t)(s->r_start - radiant_radio_now());
	s->mpsl_owes = true;
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
	{
		/* Entered before the call: an answer delivered inline lands after it. */
		const uint32_t tat = trace_write(
			(uint8_t)(TRACE_REQ | sess_idx(s)), 0u, TRACE_RC_WAIT,
			trace_sat16(s->notice_us / 4));

		rc = ts_request(s);
		trace_req_rc(tat, rc);
	}
#else
	rc = ts_request(s);
#endif
	if (rc == 0) {
		s->want_place = false;
		if (!s->mpsl_owes) {
			/* Answered inside the call; the flag is already cleared. */
			stats.answered_inline++;
		}
		/* Accepted. Staged numbers commit at SIGNAL_START (struct next_grant). */
		stats.placed++;
		GSTAT(s->n_placed++);
		return;
	}
	/* Not placed, so nothing is owed. See the note above for why this
	 * write cannot lose a race. */
	s->mpsl_owes = false;

	if (rc == -NRF_EAGAIN) {
		/*
		 * -NRF_EAGAIN means "not yet", not a refusal: the session is not IDLE, which
		 * is normal until the ACTION_END that ends the previous timeslot. The request
		 * is kept and SESSION_IDLE places it. Reporting it as a denial would make the
		 * sweep back off for contention that never happened. The deadline timer keeps
		 * running, so a session that never goes idle still resolves the operation.
		 */
		stats.refused_eagain++;
		GSTAT(s->deferred = true);
		return;
	}
	/* Not placed: reported as a denial, like any other refusal. */
	s->want_place = false;
	if (s->bootstrapping) {
		/* No operation is behind a bootstrap. anchor_valid stays false, so the next
		 * real request falls back to EARLIEST. */
		s->bootstrapping = false;
		return;
	}
	if (s->role == ROLE_BOOK) {
		/* A booking that never reached MPSL: its arm is refused when it comes. */
		s->book_failed = true;
		GSTAT(stats.book_unplaced++);
		return;
	}
	s->role = ROLE_NONE;
	if (g.pending && g.op_sess == sess_idx(s)) {
		g.pending = false;
		deadline_stop();
		radiant_nrf_gate_on_denied();
	}
}

/*
 * Establish the anchor before any real work needs it (rule 2). An operation must
 * not use EARLIEST, which has no fixed relation to the requested start. The
 * bootstrap timeslot carries no operation: it is the minimum length, handed back at
 * SIGNAL_START, and exists only to set g.anchor.
 */
static void bootstrap_anchor(struct sess *s)
{
	if (!s->open || s->want_place || s->mpsl_owes || s->anchor_valid ||
	    s->role != ROLE_NONE ||
	    (g.ts_live && g.live_sess == sess_idx(s))) {
		return;
	}
	memset(&s->req, 0, sizeof(s->req));
	s->req.request_type = MPSL_TIMESLOT_REQ_TYPE_EARLIEST;
	s->req.params.earliest.hfclk = GATE_HFCLK_CFG;
	s->req.params.earliest.priority = MPSL_TIMESLOT_PRIORITY_NORMAL;
	s->req.params.earliest.length_us = MPSL_TIMESLOT_LENGTH_MIN_US;
	s->req.params.earliest.timeout_us = BOOTSTRAP_TIMEOUT_US;

	/* Staged like every other request. It cannot grow. */
	s->ng.extendable = false;
	s->ng.high_prio = false;
	s->ng.no_extend = false;
	/* Conservative margin: this slot never places a compare, and both margins
	 * exceed its length anyway. */
	s->ng.end_margin_us = END_MARGIN_EXTEND_US;
	s->ng.want_len_us = MPSL_TIMESLOT_LENGTH_MIN_US;
	s->ng.granted_len_us = MPSL_TIMESLOT_LENGTH_MIN_US;
	s->ng.grant_end = 0u;
	s->bootstrapping = true;
	want_place_set(s);
	radiant_swi_pend(RADIANT_SWI_GATE_REQUEST);
}

/*
 * Tells the core it did not get the air. Runs only from this queue, never from a
 * timer ISR or an MPSL callback, since it ends in a scheduler pass.
 */
static void denied_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);
	/* A grant whose programming failed is finished here unconditionally; it has
	 * its own flag. The denial is conditional (see end_housekeeping()). */
	radiant_nrf_gate_finish_failed();
	if (!g.deny_wanted) {
		return;
	}
	g.deny_wanted = false;
	/*
	 * Tested here, not where the work was submitted. The RADIO event for the window
	 * can arrive in the same interrupt, complete the operation, and arm a new one
	 * before this thread runs. Denying that new operation would strand it. g.pending
	 * ("is a grant coming for what is staged?") must be read when acted on.
	 */
	if (!g.pending) {
		switch (g.deny_src) {
		case DENY_SRC_AFTER_GRANT:
			stats.sent_after_grant++;
			break;
		case DENY_SRC_DEADLINE:
			stats.sent_deadline++;
			break;
		default:
			stats.sent_blocked++;
			break;
		}
		radiant_nrf_gate_on_denied();
	} else {
		stats.deny_suppressed++;
	}
}

static void deadline_expired(struct k_timer *t)
{
	ARG_UNUSED(t);

	if (!g.pending) {
		return;
	}
	stats.deadline_fired++;
	/* The request stays MPSL's; op_forget() keeps it as a booking. */
	op_forget();
	/* ISR context: hand the denial on. The grant came too late, so the staged
	 * operation is orphaned and must be told. */
	g.deny_src = DENY_SRC_DEADLINE;
	g.deny_wanted = true;
	radiant_swi_pend(RADIANT_SWI_GATE_DENY);
}

/*
 * Signal callback. Signals 0-4 run at MPSL's high priority; 5-9 run in
 * mpsl_low_priority_process(), where returning anything but ACTION_NONE asserts.
 * rv is therefore reset to NONE first and set only where legal.
 */

/*
 * Returns the radio. Split from timer0_disarm() because gate_release() needs the
 * radio back at once but must leave the TIMER0 compare armed; that compare is the
 * only way MPSL is asked to return ACTION_END. Exactly one call per granted
 * timeslot: a second call hands back a radio the controller already took, and a
 * missed call leaves the 802.15.4 driver's SUBSCRIBE_RXEN swapped out. g.hw_held
 * enforces this.
 */
static inline void gate_hand_back(void)
{
	if (!g.hw_held) {
		return;
	}
	g.hw_held = false;
	stats.grant_end_calls++;
	radiant_nrf_gate_on_grant_end();
}

/*
 * Disarms TIMER0 on every path that ends a grant. MPSL_TIMER0 belongs to MPSL. A
 * COMPARE0 left enabled re-enters forever on an event nothing clears: the board
 * stays alive and answers nothing, with no fault message.
 */
static inline void timer0_disarm(void)
{
	nrf_timer_int_disable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
	nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
	/* Also returns the radio, so no path can forget to. Radio shorts left set
	 * would fire on the SoftDevice Controller's own events. */
	gate_hand_back();
}

/*
 * Denial after a window ends inside its grant. Not unconditional: the window may
 * already have armed the next operation, so denied_work_fn() checks g.pending to
 * see whether anything is really orphaned. The failed-programming finish is
 * unconditional.
 */
static inline void end_housekeeping(void)
{
	g.deny_src = DENY_SRC_AFTER_GRANT;
	g.deny_wanted = true;
	radiant_swi_pend(RADIANT_SWI_GATE_DENY);
}

/* Denial with no ambiguity: no grant is coming for the staged operation. */
static inline void submit_denial(void)
{
	g.deny_src = DENY_SRC_BLOCKED;
	g.deny_wanted = true;
	radiant_swi_pend(RADIANT_SWI_GATE_DENY);
}

/*
 * Set for the whole of a high-priority signal, so the backend can tell a
 * zero-latency completion from an ordinary one (see gate_in_signal() in
 * radiant_radio_nrf_gate.h). Not set for low-priority signals: the trampoline
 * could preempt those, making a second producer for the backend's single-producer
 * ring. A plain bool, since only the zero-latency callback writes it.
 */
static volatile bool in_signal;

bool gate_in_signal(void)
{
	return in_signal;
}

static mpsl_timeslot_signal_return_param_t *on_signal(
	mpsl_timeslot_session_id_t id, uint32_t signal)
{
	const bool high_prio_signal =
		signal <= MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED;
	struct sess   *s = sess_of(id);
	const uint8_t  si = sess_idx(s);
	/* Per-session return parameter; every branch writes through rv. */
	mpsl_timeslot_signal_return_param_t *const rvp = &s->rv;
	uint32_t zkey;
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
	/* This TIMER0 signal ended its grant by the skip (evidence counters only). */
	bool skip_end = false;
#endif
#define rv (*rvp)

	if (high_prio_signal) {
		in_signal = true;
	}

	/* Trace sequence: START then TIMER0 repeating means the compare re-fires;
	 * START then silence means the RADIO interrupt spins and never reaches here. */
	rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
	stats.last_signal = signal;
	GSTAT(dbg.last_sess = si);

	switch (signal) {
	case MPSL_TIMESLOT_SIGNAL_START:
		stats.granted++;
		GSTAT(s->n_granted++);
		/* Answered: the session accepts a new request. See struct sess::mpsl_owes. */
		s->mpsl_owes = false;
		s->blocked_run = 0u;
		s->anchor = radiant_radio_now();
		GSTAT(dbg.t_start = (uint32_t)s->anchor);
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_REQ_DISCIPLINE)
		s->idle_seen = false;
#endif
		s->anchor_valid = true;
		/* Radio on loan from here until timer0_disarm() returns it. */
		g.hw_held = true;
		g.ts_live = true;
		g.live_sess = si;
		g.release_wanted = false;
		/* A fresh timeslot, so ACTION_END is legal again. */
		g.ended = false;

		if (s->bootstrapping) {
			/* The anchor is the point of this timeslot. Hand the air straight back
			 * without touching the peripheral or the core. */
			s->bootstrapping = false;
			g.granted = false;
			timer0_disarm();
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		}
		if (!g.pending || g.op_sess != si) {
			/*
			 * Nothing staged on THIS session (an aborted request, or a booking nobody
			 * armed into). Hand the air back without touching the backend: the staged
			 * operation may belong to the other session, and programming it here would
			 * put it on the air at the wrong instant.
			 */
			if (s->role == ROLE_BOOK) {
				GSTAT(stats.book_early++);
				if (g.pending) {
					GSTAT(stats.early_other++);
				} else {
					GSTAT(stats.early_none++);
				}
			} else {
				GSTAT(stats.orphan_start++);
			}
			s->role = ROLE_NONE;
			s->book_failed = false;
			g.granted = false;
			timer0_disarm();
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		}
		/* The staged operation's grant: its numbers go live now (struct next_grant). */
		commit_next_grant(s);
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
		/* How far the grant landed from its requested start (bench). */
		if (!g.extendable) {
			int32_t e = (int32_t)(int64_t)(s->anchor - s->r_start);

			stats.start_err_last = e;
			if (stats.start_err_n == 0u || e < stats.start_err_min) {
				stats.start_err_min = e;
			}
			if (stats.start_err_n == 0u || e > stats.start_err_max) {
				stats.start_err_max = e;
			}
			stats.start_err_n++;
		}
#endif
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
		if (g.extendable) {
			stats.el_gr_by_notice[notice_bin(s->notice_us)]++;
		}
#endif
		GSTAT(s->deferred = false);
		s->role = ROLE_NONE;
		if (grant_high_prio) {
			stats.hi_granted++;
		}
		/* Air was granted: grow the first bite by one step. Halving on refusal is
		 * faster, by design. */
		if (g.extendable && elastic_initial_us < ELASTIC_INITIAL_US) {
			elastic_initial_us += EXTEND_STEP_US;
			if (elastic_initial_us > ELASTIC_INITIAL_US) {
				elastic_initial_us = ELASTIC_INITIAL_US;
			}
		}
		if (g.no_extend && ext_skip > 0u) {
			ext_skip--;
		}
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
		if (g.extendable) {
			el_block_run = 0u;
		}
#endif
		g.pending = false;
		g.granted = true;
		deadline_stop();
		/* Air we hold. g.end_margin_us of it is reserved for the hand-back. */
		g.grant_end = s->anchor + (radiant_time_t)g.granted_len_us -
			      (radiant_time_t)g.end_margin_us;

		/* Program the peripheral now, inside the grant. This may complete the
		 * operation synchronously (setting release_wanted), handled below. */
		radiant_nrf_gate_on_grant();

		if (g.release_wanted) {
			/* Completed synchronously, nothing was staged, or programming failed
			 * (ETIME). A failure sets radiant_op.gate_failed and releases, so its
			 * unused air goes back now rather than at the TIMER0 end (A14-1), and
			 * end_housekeeping() has denied_work_fn() report FAILED. */
			g.granted = false;
			timer0_disarm();
			end_housekeeping();
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		}
		/*
		 * Arm a TIMER0 compare so there is always a signal to end the timeslot from.
		 * Without it an operation whose RADIO interrupt never comes overstays and
		 * asserts. Use g.end_margin_us, not TAIL_MARGIN_US: the latter covers only the
		 * radio, and at len-250 the board asserted on the first or second timeslot.
		 * The margin was fixed per class in gate_acquire(), when the request was sized.
		 */
		nrf_timer_cc_set(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL0,
				 (g.granted_len_us > g.end_margin_us)
					 ? (g.granted_len_us - g.end_margin_us)
					 : (g.granted_len_us / 2u));
		/*
		 * A stale compare ends the grant 14 us after it starts. CC0 still holds the last
		 * grant's value until the cc_set above, ~31 us in; if the counter passes it in
		 * that window, EVENTS_COMPARE0 latches and the int_enable raises SIGNAL_TIMER0
		 * at once. Clear between cc_set and int_enable. gate_release() uses the same order.
		 */
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
		nrf_timer_int_enable(MPSL_TIMER0, NRF_TIMER_INT_COMPARE0_MASK);
		break;

	case MPSL_TIMESLOT_SIGNAL_RADIO:
		/*
		 * Inside a timeslot MPSL owns RADIO_0_IRQn and delivers the RADIO interrupt
		 * as this signal, so radiant_radio_nrf.c's handler is never entered. Re-enter
		 * it by hand. If that finishes the operation, return the grant now rather
		 * than wait for TIMER0. This is a high-priority signal, so ACTION_END is legal.
		 */
		stats.sig_radio++;
		radiant_nrf_gate_on_radio_irq();
		if (g.release_wanted && !g.granted) {
			timer0_disarm();
			end_housekeeping();
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_TIMER0:
		stats.sig_timer0++;
		nrf_timer_event_clear(MPSL_TIMER0, NRF_TIMER_EVENT_COMPARE0);
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
		if (!g.extendable && !g.ended) {
			if (g.release_wanted) {
				stats.t0_fix_rel++;
			} else {
				stats.t0_fix_backstop++;
			}
			stats.t0_fix_at = (uint32_t)(radiant_radio_now() - s->anchor);
		}
#endif
		if (g.no_extend && !g.release_wanted && g.extendable &&
		    g.granted_len_us < g.want_len_us) {
			/* As EXTEND_FAILED below, without asking. See ext_skip. */
			stats.ext_skipped++;
			GSTAT(skip_end = true);
			g.granted = false;
			DPPI_DETACH_FIRST();
			timer0_disarm();
			(void)radiant_radio_abort();
			end_housekeeping();
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS) &&                               \
	!defined(CONFIG_RADIANT_GATE_MPSL_ARM_NO_T0_CAPTURE)
			/*
			 * How much of the fixed end margin this path left. NOTE: this runs
			 * after timer0_disarm() handed the radio back and overwrites
			 * MPSL_TIMER0 CC1; the NO_T0_CAPTURE bench arm compiles it out.
			 */
			{
				uint32_t left;

				nrf_timer_task_trigger(MPSL_TIMER0,
						       NRF_TIMER_TASK_CAPTURE1);
				left = g.granted_len_us -
				       nrf_timer_cc_get(MPSL_TIMER0, 1);
				if (stats.skip_left_min == 0u ||
				    left < stats.skip_left_min) {
					stats.skip_left_min = left;
				}
			}
#endif
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		}
		/*
		 * Released by gate_release(), or the grant ran out. Both end the timeslot;
		 * an elastic grant tries to grow first, toward what the operation wants.
		 * The last step is the remainder, so the grant does not stop short of the
		 * window's close. Only asked for if MPSL grants an extension that small.
		 */
		if (!g.release_wanted && g.extendable &&
		    g.granted_len_us < g.want_len_us) {
			uint32_t step = g.want_len_us - g.granted_len_us;

			if (step > EXTEND_STEP_US) {
				step = EXTEND_STEP_US;
			}
			/*
			 * The ceiling is the TIMESLOT's, not the window's. A timeslot covering a
			 * window of MAX_WINDOW_US is longer by the head, tail and end margins.
			 * Using MAX_WINDOW_US here stopped the chain 2500 us short, ending the grant
			 * before the window closed. want_len_us is already bounded in gate_acquire().
			 */
			if (step >= MPSL_TIMESLOT_EXTENSION_TIME_MIN_US &&
			    g.granted_len_us + step <=
				    MPSL_TIMESLOT_LENGTH_MAX_US) {
				g.ext_step_us = step;
				rv.callback_action =
					MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND;
				rv.params.extend.length_us = step;
				break;
			}
		}
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_EL_FIXED)
		/*
		 * Bench arm (69:108): a scan chunk granted only a fixed first bite.
		 * Its window outlives the grant, so cut it as the ext-skip path does
		 * rather than hand back a radio with the window still armed.
		 */
		if (!g.release_wanted && !g.extendable &&
		    g.granted_len_us < g.want_len_us) {
			arm_stats.el_fixed_cut++;
			GSTAT(skip_end = true);
			g.granted = false;
			DPPI_DETACH_FIRST();
			timer0_disarm();
			(void)radiant_radio_abort();
			end_housekeeping();
			rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
			break;
		}
#endif
		g.granted = false;
		DPPI_DETACH_FIRST();
		timer0_disarm();
		/* See end_housekeeping(): the terminal is deferred, and only sent if nothing is coming. */
		end_housekeeping();
		rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		break;

	case MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED:
		/*
		 * The compare advances one step per extension, so g.end_margin_us of runway
		 * stays ahead of it. TAIL_MARGIN_US here would leave only 250 us, and MPSL
		 * asserts on the overstay. g.end_margin_us is always END_MARGIN_EXTEND_US on
		 * this path. It is read from state so it cannot drift from the compare that
		 * armed the first grant.
		 */
		stats.extends_ok++;
		ext_fail_run = 0u;
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
		el_lead_state(false);
#endif
		g.granted_len_us += g.ext_step_us;
		g.grant_end += (radiant_time_t)g.ext_step_us;
		nrf_timer_cc_set(MPSL_TIMER0, 0,
				 g.granted_len_us - g.end_margin_us);
		break;

	case MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED:
		/*
		 * The yield point, discovered rather than configured. The grant is still live,
		 * so the window closes cleanly and reports ABORTED (a partial), not a denial.
		 */
		stats.extends_failed++;
		if (++ext_fail_run >= EXT_FAIL_RUN_OFF) {
			ext_fail_run = 0u;
			ext_skip = EXT_SKIP_GRANTS;
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
			el_lead_state(true);
#endif
		}
		g.granted = false;
		DPPI_DETACH_FIRST();
		timer0_disarm();
		(void)radiant_radio_abort();
		/*
		 * An orphan still gets its denial. radiant_radio_abort() terminates only an
		 * armed operation, not one staged but not yet programmed, which would wedge
		 * the core's single operation slot. The g.pending test in denied_work_fn()
		 * stops this from denying an operation a fresh request is coming for.
		 * radiant_radio_abort() runs here at priority 0 (up to ~100 us). Deferring it
		 * would return the timeslot with the peripheral still armed, so measure first.
		 */
		end_housekeeping();
		rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_END;
		break;

	case MPSL_TIMESLOT_SIGNAL_BLOCKED:
	case MPSL_TIMESLOT_SIGNAL_CANCELLED:
		/*
		 * Low-priority context (a cooperative thread). No action may be returned; the
		 * only job is to tell the core. The anchor does not move, since no timeslot
		 * started. This is the first HAL event that reaches the core outside the radio
		 * interrupt (see radiant_radio_hal.h).
		 */
		if (signal == MPSL_TIMESLOT_SIGNAL_BLOCKED) {
			stats.blocked++;
			GSTAT(s->n_blocked++);
			if (s->placed_high_prio) {
				stats.hi_blocked++;
			}
		} else {
			stats.cancelled++;
		}
		/* "No timeslot started" is checked, not assumed: return the radio if it is
		 * still on loan, and count it (late_disarm). */
		{
			/* Masked: another session's START can land between the test and the disarm. */
			const uint32_t zkey = radiant_swi_zli_lock();

			if (g.hw_held && (!g.ts_live || g.live_sess == si)) {
				stats.late_disarm++;
				g.granted = false;
				timer0_disarm();
				g.ts_live = false;
			}
			radiant_swi_zli_unlock(zkey);
		}
		/* Answered, and refused. See struct sess::mpsl_owes. */
		s->mpsl_owes = false;
		/*
		 * Give way by asking for less, not merely by asking again (see
		 * elastic_initial_us). Only the elastic class shrinks. The decision uses the
		 * placed request's flag (placed_extendable), not g.extendable.
		 */
		if (s->placed_extendable) {
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
			if (++el_block_run >= EL_BLOCK_RUN_CONTENDED) {
				el_block_run = 0u;
				el_lead_state(true);
			}
#endif
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
			stats.el_blocked++;
			stats.el_blk_by_notice[notice_bin(s->notice_us)]++;
			if (s->deferred) {
				stats.el_blk_deferred++;
			}
			for (uint32_t i = 0u; i < NSESS; i++) {
				const struct sess *o = &ss[i];

				if (o != s && o->open &&
				    (o->role == ROLE_BOOK || o->role == ROLE_OP)) {
					int32_t gap = (int32_t)(int64_t)(o->r_start -
									 s->ts_end);

					if (gap < 3000 && gap > -100000) {
						stats.el_blk_adj++;
						if (stats.el_blk_adj == 1u ||
						    gap < stats.el_blk_gap_min) {
							stats.el_blk_gap_min = gap;
						}
					}
				}
			}
#endif
			if (elastic_initial_us > ELASTIC_FLOOR_US) {
				elastic_initial_us /= 2u;
				if (elastic_initial_us < ELASTIC_FLOOR_US) {
					elastic_initial_us = ELASTIC_FLOOR_US;
				}
			}
			/* And ask somewhere else next time. See elastic_skew_us. */
			elastic_skew_us += ELASTIC_SKEW_STEP_US;
			if (elastic_skew_us >= ELASTIC_SKEW_SPAN_US) {
				elastic_skew_us -= ELASTIC_SKEW_SPAN_US;
			}
		}
		/*
		 * A run of refusals means the anchor itself is the problem; length and phase
		 * were tried first. The anchor does not move on refusal, so an unbroken run
		 * ages it without bound. After BLOCKED_RUN_REANCHOR refusals it is abandoned,
		 * and the next acquire re-bootstraps with EARLIEST, which a busy neighbour
		 * cannot starve. A fresh anchor costs one minimum-length timeslot.
		 */
		if (++s->blocked_run >= BLOCKED_RUN_REANCHOR) {
			s->blocked_run = 0u;
			s->anchor_valid = false;
		}
		if (s->bootstrapping) {
			/* Nothing is behind it. The next real request bootstraps again. */
			s->bootstrapping = false;
			break;
		}
		if (s->role == ROLE_BOOK) {
			GSTAT(stats.book_blocked++);
		}
		/*
		 * A refused booking that asked for air past its follow-on (a scan tail, or a
		 * joined operation) is placed again at plain length, while it still has the
		 * notice it needs. That air belongs to the listener or joiner. Whether or not
		 * the core has armed into it, the staged operation and its backstop stay as
		 * they are. Only a booking carries book_tail_us.
		 */
		if ((s->role == ROLE_BOOK || s->role == ROLE_OP) &&
		    s->book_tail_us != 0u && !s->book_replaced &&
		    s->anchor_valid &&
		    s->r_start > radiant_radio_now() + (radiant_time_t)PLACE_MIN_US) {
			const radiant_time_t end =
				s->r_end - (radiant_time_t)s->book_tail_us;
			const uint64_t len = (uint64_t)(end - s->r_start) +
					     (uint64_t)END_MARGIN_FIXED_US;

			s->book_replaced = true;
			s->book_tail_us = 0u;
			if (build_normal(s, s->r_start, end,
					 (len < MPSL_TIMESLOT_LENGTH_MIN_US)
						 ? MPSL_TIMESLOT_LENGTH_MIN_US
						 : len,
					 false, false, END_MARGIN_FIXED_US,
					 RADIANT_GATE_PRIO_HIGH)) {
				s->r_end = end;
				want_place_set(s);
				GSTAT(stats.book_replaced++);
				radiant_swi_pend(RADIANT_SWI_GATE_REQUEST);
				break;
			}
		}
		if (s->role == ROLE_BOOK) {
			/* Nobody is waiting on it yet; its arm is refused synchronously when it comes. */
			s->book_failed = true;
			break;
		}
		s->role = ROLE_NONE;
		if (g.pending && g.op_sess == si) {
			deadline_stop();
			g.pending = false;
			/*
			 * Handed on, not called. The core's scheduler pass may place the next
			 * reservation, and doing that inside MPSL's own low-priority processing
			 * would have MPSL re-enter itself.
			 */
			submit_denial();
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_SESSION_IDLE:
		/*
		 * IDLE means no timeslot is running and no request is outstanding. It is the only
		 * unconditional signal of that, so it clears g.granted and backstops the END paths
		 * that never signal (an expiry-ended timeslot would otherwise leave a dead grant).
		 * Test g.hw_held, not g.granted: a lost compare would be missed while the radio is
		 * still on loan. With two sessions, the radio-level backstop below runs only when
		 * the running timeslot, if any, is this session's.
		 */
		/* Masked: another session's START after this test would be torn down by the writes below. */
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_REQ_DISCIPLINE)
		/* Before the re-pends below, so the placement they trigger is not held. */
		s->idle_seen = true;
#endif
		zkey = radiant_swi_zli_lock();
		if (g.ts_live && g.live_sess != si) {
			radiant_swi_zli_unlock(zkey);
			s->mpsl_owes = false;
			if (s->want_place) {
				radiant_swi_pend(RADIANT_SWI_GATE_REQUEST);
			}
			break;
		}
		g.ts_live = false;
		g.granted = false;
		for (uint32_t i = 0u; i < NSESS; i++) {
			if (ss[i].wait_live) {
				radiant_swi_pend(RADIANT_SWI_GATE_REQUEST);
				break;
			}
		}
		if (g.hw_held) {
			/* Not late_disarm: this is the designed backstop for an expiry-ended
			 * timeslot, so it may be non-zero in a healthy run. */
			stats.idle_disarm++;
		}
		/*
		 * The disarm is unconditional, unlike the counter. gate_release() leaves the
		 * compare armed, so a release whose compare was lost arrives here with
		 * hw_held already false and an interrupt still armed.
		 */
		timer0_disarm();
		radiant_swi_zli_unlock(zkey);
		/* Authoritative clear of mpsl_owes ("no request outstanding"). The re-place
		 * below sets it again on acceptance. */
		s->mpsl_owes = false;
		/* A request that was too early is placed here. -NRF_EAGAIN until this point
		 * is normal. See request_work_fn(). */
		if (s->want_place) {
			radiant_swi_pend(RADIANT_SWI_GATE_REQUEST);
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED:
		/*
		 * Can arrive with an operation armed and no grant held. Along with
		 * radiant_radio_disable(), this is one of the paths that run only during
		 * suspend and resume. The armed operation must still get its terminal event.
		 */
		s->anchor_valid = false;
		s->mpsl_owes = false;
		s->want_place = false;
		s->bootstrapping = false;
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_REQ_DISCIPLINE)
		s->idle_seen = true;
#endif
		if (s->role == ROLE_BOOK) {
			s->book_failed = true;
		} else {
			s->role = ROLE_NONE;
		}
		/* Counter guarded, call unconditional (see SESSION_IDLE). Own timeslot only. */
		zkey = radiant_swi_zli_lock();
		if (!g.ts_live || g.live_sess == si) {
			g.granted = false;
			g.ts_live = false;
			if (g.hw_held) {
				stats.idle_disarm++;
			}
			timer0_disarm();
		}
		radiant_swi_zli_unlock(zkey);
		if (g.pending && g.op_sess == si) {
			deadline_stop();
			g.pending = false;
			submit_denial();
		}
		break;

	case MPSL_TIMESLOT_SIGNAL_INVALID_RETURN:
	case MPSL_TIMESLOT_SIGNAL_OVERSTAYED:
	default:
		/*
		 * Programming errors. MPSL asserts right after this returns, so this is the
		 * only place to record which one occurred. OVERSTAYED: the end margin is too
		 * small. INVALID_RETURN: an action was returned from a signal that may not
		 * carry one.
		 */
		if (signal == MPSL_TIMESLOT_SIGNAL_OVERSTAYED) {
			stats.overstayed++;
		} else if (signal == MPSL_TIMESLOT_SIGNAL_INVALID_RETURN) {
			stats.invalid_return++;
		} else {
			stats.unknown_signal = signal;
		}
		break;
	}

	/*
	 * Final say on the returned action, in one place: no action after the timeslot
	 * has already ended (bug 14). Branch-by-branch checks miss the race.
	 */
	if (g.ended) {
		rv.callback_action = MPSL_TIMESLOT_SIGNAL_ACTION_NONE;
	} else if (rv.callback_action == MPSL_TIMESLOT_SIGNAL_ACTION_END) {
		g.ended = true;
		g.ts_live = false;
		GSTAT(dbg.t_end = (uint32_t)radiant_radio_now());
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_REQ_DISCIPLINE)
		req_t_end32 = (uint32_t)radiant_radio_now();
		req_end_seen = true;
#endif
		/* A placement held back for this timeslot (place_one()). */
		for (uint32_t i = 0u; i < NSESS; i++) {
			if (ss[i].wait_live) {
				radiant_swi_pend(RADIANT_SWI_GATE_REQUEST);
				break;
			}
		}
		/*
		 * Assert 69:108. A RADIO event that fired while this signal ran (same priority,
		 * so it could only pend) leaves MPSL's RADIO vector pending even after the
		 * hand-back cleared our mask. MPSL takes it once ACTION_END has closed the
		 * timeslot: a RADIO interrupt with no timeslot open, which asserts. Clear it
		 * here, while the timeslot and its RADIO are still ours (mpsl_timeslot.h), and
		 * after every hand-back and radiant_radio_abort() on the paths above: with our
		 * mask cleared and the radio disabled nothing of ours can pend it again before
		 * the return. Every high-priority END funnels through here; low-priority
		 * signals never return END, and their hand-backs (BLOCKED, SESSION_IDLE,
		 * SESSION_CLOSED, gate_shutdown()) must not clear it: the radio may already be
		 * the other stack's. The backend clears only if this timeslot took the radio.
		 */
		if (high_prio_signal) {
			const bool was_pending = radiant_nrf_gate_radio_irq_unpend();

#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
			{
				const uint32_t src =
					(signal == MPSL_TIMESLOT_SIGNAL_START) ? END_SRC_START
					: (signal == MPSL_TIMESLOT_SIGNAL_RADIO) ? END_SRC_RADIO
					: (signal == MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED)
						? END_SRC_EXT_FAIL
					: skip_end ? END_SRC_T0_SKIP
						   : END_SRC_T0_OTHER;

				stats.end_hi[src]++;
				if (was_pending) {
					stats.unpend[src]++;
				}
			}
#else
			ARG_UNUSED(was_pending);
#endif
		}
	}

	TRACE_SIGNAL(si, signal, rv.callback_action);
	if (high_prio_signal) {
		in_signal = false;
	}
	return rvp;
#undef rv
}

/* The interface */

#if defined(CONFIG_RADIANT_GATE_MPSL_STATS_LOG)
/* Bench only: the counters a bench reads, logged every 5 s from gate_wq (never from a grant). Cumulative since boot. */
static void gate_stats_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(gate_stats_work, gate_stats_fn);

extern uint32_t radiant_d2_dbg[8];
extern uint32_t radiant_arm_dbg[4];

static void gate_stats_fn(struct k_work *w)
{
	ARG_UNUSED(w);
#if defined(CONFIG_RADIANT_GATE_MPSL_LEAD_SWEEP)
	{
		static const uint16_t leads_ms[] = {4u, 6u, 8u, 12u, 16u, 24u};
		static uint32_t ticks;

		if ((ticks % 12u) == 0u) {
			el_lead_us = 1000u * leads_ms[(ticks / 12u) % ARRAY_SIZE(leads_ms)];
			el_lead_publish();
			LOG_INF("D2SWEEP step=%u lead_us=%u", ticks / 12u,
				(unsigned)el_lead_us);
		}
		ticks++;
	}
#endif
#if defined(CONFIG_RADIANT_GATE_MPSL_TAIL_SWEEP)
	{
		static const uint16_t tails_ms[] = {0u, 5u, 10u, 20u};
		static uint32_t ticks;

		if ((ticks % 24u) == 0u) {
			scan_tail_us = 1000u * tails_ms[(ticks / 24u) % ARRAY_SIZE(tails_ms)];
			el_lead_publish();
			LOG_INF("D2SWEEP step=%u tail_us=%u", ticks / 24u,
				(unsigned)scan_tail_us);
		}
		ticks++;
	}
#endif
	LOG_INF("D2SETTLE rel=%u keep=%u wait=%u none=%u now=%u imm=%u owed=%u",
		radiant_d2_dbg[0], radiant_d2_dbg[1], radiant_d2_dbg[2],
		radiant_d2_dbg[3], radiant_d2_dbg[4], radiant_d2_dbg[5],
		radiant_d2_dbg[6]);
	LOG_INF("ARMDBG abort_clr=%u etime_rel=%u tx_miss=%u rx_miss=%u",
		radiant_arm_dbg[0], radiant_arm_dbg[1], radiant_arm_dbg[2],
		radiant_arm_dbg[3]);
	LOG_INF("D2STAT pl=%u gr=%u bl=%u s0=%u/%u/%u s1=%u/%u/%u ext=%u/%u "
		"near=%u eag=%u dl=%u",
		stats.placed, stats.granted, stats.blocked, ss[0].n_placed,
		ss[0].n_granted, ss[0].n_blocked, ss[NSESS - 1u].n_placed,
		ss[NSESS - 1u].n_granted, ss[NSESS - 1u].n_blocked,
		stats.extends_ok, stats.extends_failed, stats.refused_too_near,
		stats.refused_eagain, stats.deadline_fired);
	LOG_INF("D2BOOK pl=%u hit=%u den=%u blk=%u unpl=%u mis=%u early=%u "
		"busy=%u near=%u orph=%u nosess=%u owed=%u | el=%u [%u..%u] "
		"elblk=%u adj=%u/%d | eo=%u en=%u hl=%u dfl=%u | sent ag=%u dl=%u "
		"bl=%u sup=%u | hi %u/%u/%u/%u",
		stats.book_placed, stats.book_hit, stats.book_denied,
		stats.book_blocked, stats.book_unplaced, stats.book_mismatch,
		stats.book_early, stats.book_busy, stats.book_near,
		stats.orphan_start, stats.den_nosess, stats.den_owed,
		(unsigned)el_lead_us, stats.el_lead_min, stats.el_lead_max,
		stats.el_blocked, stats.el_blk_adj, (int)stats.el_blk_gap_min,
		stats.early_other, stats.early_none, stats.hit_late,
		stats.defer_live, stats.sent_after_grant, stats.sent_deadline,
		stats.sent_blocked, stats.deny_suppressed, stats.hi_acq,
		stats.hi_pending, stats.hi_blocked, stats.hi_granted);
	LOG_INF("D2REL fixed rel %u/%u us after start, hold %u/%u us (last/max) "
		"late_drop %u | br ok=%u inside=%u restore=%u cap=%u est=%u | "
		"t0fix rel=%u bs=%u at=%u | ext_skipped=%u",
		stats.rel_fixed_last, stats.rel_fixed_max, stats.hold_last,
		stats.hold_max, stats.el_late_drop, stats.rel_br_ok,
		stats.rel_br_inside, stats.rel_br_restore, stats.rel_cap_last,
		stats.rel_est_last, stats.t0_fix_rel, stats.t0_fix_backstop,
		stats.t0_fix_at, stats.ext_skipped);
	LOG_INF("D2START n=%u err last/min/max %d/%d/%d us | skip_left_min=%u | "
		"due saved=%u late=%u orphan=%u tail=%u repl=%u join=%u jdefer=%u",
		stats.start_err_n, (int)stats.start_err_last,
		(int)stats.start_err_min, (int)stats.start_err_max,
		stats.skip_left_min,
		radiant_sched_stats_get()->book_saved,
		radiant_sched_stats_get()->book_late,
		radiant_sched_stats_get()->book_orphan,
		radiant_sched_stats_get()->scan_tail, stats.book_replaced,
		radiant_sched_stats_get()->join, radiant_sched_stats_get()->join_defer);
	LOG_INF("D2EL blk by notice %u/%u/%u/%u/%u gr %u/%u/%u/%u/%u deferred-blk %u",
		stats.el_blk_by_notice[0], stats.el_blk_by_notice[1],
		stats.el_blk_by_notice[2], stats.el_blk_by_notice[3],
		stats.el_blk_by_notice[4], stats.el_gr_by_notice[0],
		stats.el_gr_by_notice[1], stats.el_gr_by_notice[2],
		stats.el_gr_by_notice[3], stats.el_gr_by_notice[4],
		stats.el_blk_deferred);
	LOG_INF("UNPEND end start/t0skip/t0other/radio/extfail %u/%u/%u/%u/%u "
		"pending %u/%u/%u/%u/%u",
		stats.end_hi[END_SRC_START], stats.end_hi[END_SRC_T0_SKIP],
		stats.end_hi[END_SRC_T0_OTHER], stats.end_hi[END_SRC_RADIO],
		stats.end_hi[END_SRC_EXT_FAIL], stats.unpend[END_SRC_START],
		stats.unpend[END_SRC_T0_SKIP], stats.unpend[END_SRC_T0_OTHER],
		stats.unpend[END_SRC_RADIO], stats.unpend[END_SRC_EXT_FAIL]);
#if defined(ARM_COUNTERS)
	LOG_INF("ARM69 held_idle=%u held_gap=%u el_fixed_cut=%u dppi_first=%u",
		arm_stats.held_idle, arm_stats.held_gap, arm_stats.el_fixed_cut,
		arm_stats.dppi_first);
#endif
	(void)k_work_reschedule_for_queue(&gate_wq, &gate_stats_work, K_SECONDS(5));
}
#endif

int gate_init(void)
{
	int32_t rc;

	if (session_open) {
		return RADIANT_RADIO_OK_RC;
	}
	memset(&g, 0, sizeof(g));
	memset(&stats, 0, sizeof(stats));
	ext_fail_run = 0u;
	ext_skip = 0u;
	for (uint32_t i = 0u; i < NSESS; i++) {
		memset(&ss[i], 0, sizeof(ss[i]));
	}
	n_open = 0u;
	/*
	 * A zero hand-back margin would place a compare at the very end of a grant, an
	 * overstay by construction. Use the wider margin: each grant commits its own
	 * class's margin (commit_next_grant()), so this covers only the window before the
	 * first commit.
	 */
	g.end_margin_us = END_MARGIN_EXTEND_US;

	/*
	 * Order: queue, then trampoline, then session, since a grant can arrive as soon as
	 * a session is open. The queue and trampoline are set up once per boot and never
	 * released: re-registering on each gate_init() would fill the subscriber table.
	 */
	if (!gate_wq_started) {
		static const struct k_work_queue_config gate_wq_cfg = {
			.name = "radiant_gate",
		};

		k_work_queue_start(&gate_wq, gate_wq_stack,
				   K_THREAD_STACK_SIZEOF(gate_wq_stack),
				   CONFIG_RADIANT_GATE_WQ_PRIO, &gate_wq_cfg);
		gate_wq_started = true;
	}
	if (!swi_registered) {
		rc = (int32_t)radiant_swi_register(RADIANT_SWI_GATE_REQUEST |
						   RADIANT_SWI_GATE_DENY |
						   RADIANT_SWI_GATE_DEADLINE |
						   RADIANT_SWI_GATE_DUE,
						   gate_swi_handler);
		if (rc != 0) {
			LOG_ERR("gate: radiant_swi_register: %d", (int)rc);
			return RADIANT_RADIO_EIO;
		}
		swi_registered = true;
	}
	rc = ts_session_open(&ss[0]);
	LOG_INF("gate: session_open rc=%d, max_window=%u us, place_lead=%u us",
		(int)rc, (unsigned)MAX_WINDOW_US, (unsigned)PLACE_LEAD_US);
	if (rc != 0) {
		/* Usually CONFIG_MPSL_TIMESLOT_SESSION_COUNT is still at its default of 0. */
		return RADIANT_RADIO_EIO;
	}
	ss[0].open = true;
	n_open = 1u;
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
	/*
	 * The second session (ADR 0018 d2). If it fails, the gate runs single-session
	 * with caps.booking false, as an app that never raised
	 * CONFIG_MPSL_TIMESLOT_SESSION_COUNT would.
	 */
	rc = ts_session_open(&ss[1]);
	LOG_INF("gate: (d2) second session_open rc=%d, scan lead %u us", (int)rc,
		(unsigned)EL_LEAD_START_US);
	if (rc == 0) {
		ss[1].open = true;
		n_open = 2u;
	}
	d2_on = (n_open == 2u);
	el_lead_us = EL_LEAD_START_US;
	el_block_run = 0u;
	el_lead_publish();
#endif
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS_LOG)
	(void)k_work_reschedule_for_queue(&gate_wq, &gate_stats_work, K_SECONDS(5));
#endif
	/* One minimum-length EARLIEST timeslot per session sets the anchor, so every
	 * later window is a NORMAL request that lands where it was asked for. */
	for (uint32_t i = 0u; i < NSESS; i++) {
		bootstrap_anchor(&ss[i]);
	}
	return RADIANT_RADIO_OK_RC;
}

void gate_shutdown(void)
{
	if (!session_open) {
		return;
	}
	deadline_stop();
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
	booking_due_stop();
#endif
	/* Test g.hw_held, not g.granted: the session must not close while SUBSCRIBE_RXEN is swapped out. */
	if (g.hw_held) {
		stats.idle_disarm++;
		timer0_disarm();
	}
	for (uint32_t i = NSESS; i-- > 0u;) {
		if (ss[i].open) {
			(void)ts_session_close(&ss[i]);
		}
		ss[i].open = false;
		ss[i].anchor_valid = false;
		ss[i].want_place = false;
		ss[i].role = ROLE_NONE;
	}
	n_open = 0u;
#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
	d2_on = false;
	el_lead_publish();
#endif
	g.pending = false;
	g.granted = false;
	g.ts_live = false;
}

uint32_t gate_max_window_us(void)
{
	return MAX_WINDOW_US;
}

uint32_t gate_min_arm_lead_us(void)
{
	/*
	 * Without this, the core posted windows at now + 168 us, all nearer than the
	 * arbiter can grant, and every acquire was refused silently. HEAD_MARGIN_US is
	 * included because the GRANT START must clear the lead. The backend adds its
	 * ARM_LEAD_US on top (radiant_radio_caps_get()) additively, since placing the
	 * reservation and programming the peripheral are sequential.
	 */
	return PLACE_LEAD_US + HEAD_MARGIN_US;
}

static bool build_normal(struct sess *s, radiant_time_t start, radiant_time_t end,
			 uint64_t len, bool extendable, bool no_extend,
			 uint32_t end_margin_us, uint8_t prio);

/*
 * A session with nothing outstanding, or NULL if none is free. When both are free,
 * prefer the one with no running timeslot: a request into a running session waits
 * for it to go idle, which can take milliseconds. A scan chunk at the short elastic
 * lead cannot afford that wait.
 */
/*
 * A booking MPSL refused waits for the arm it was for. If that arm never comes (its
 * channel closed, its slot moved, or it was an aborted request kept as a booking),
 * nothing else frees the session, and later acquires would be refused until reset.
 * Once the booked air is over, no arm can match it, so the session is freed here.
 */
static void reap_dead_bookings(void)
{
	const radiant_time_t now = radiant_radio_now();

	for (uint32_t i = 0u; i < NSESS; i++) {
		struct sess *c = &ss[i];

		if (c->open && c->role == ROLE_BOOK && c->book_failed &&
		    c->r_end < now) {
			c->role = ROLE_NONE;
			c->book_failed = false;
		}
	}
}

static struct sess *pick_free(bool prefer_live)
{
	struct sess *s = NULL;

	reap_dead_bookings();
	for (uint32_t i = 0u; i < NSESS; i++) {
		struct sess *c = &ss[i];
		bool         c_live;
		bool         s_live;

		if (!c->open || c->role != ROLE_NONE || c->mpsl_owes ||
		    c->want_place || c->bootstrapping) {
			continue;
		}
		if (s == NULL) {
			s = c;
			continue;
		}
		c_live = g.ts_live && g.live_sess == (uint8_t)i;
		s_live = g.ts_live && g.live_sess == sess_idx(s);
		if (c_live != s_live && c_live == prefer_live) {
			s = c;
		}
	}
	return s;
}

enum gate_rc gate_acquire(enum gate_op op, radiant_time_t t_from,
			  radiant_time_t t_to, uint16_t follow_on_us,
			  uint8_t prio)
{
	radiant_time_t now;
	radiant_time_t start;
	radiant_time_t end;
	uint64_t       len;
	struct sess   *s;
	/*
	 * Classification, decided before the length. Only the gap-filling classes (scan
	 * chunk, energy-detect sweep) grow; a tracked window or transmit has an end the
	 * core chose. The length depends on the class, so it must be known here. These
	 * locals are staged into next_grant only after the early refusals, which must
	 * leave next_grant untouched.
	 */
	const bool     elastic_op = (op != GATE_OP_TX) &&
				(prio == RADIANT_GATE_PRIO_NORMAL);
	/* Bench arm EL_FIXED (69:108): no elastic class at all. */
	const bool     extendable =
		elastic_op && !IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_ARM_EL_FIXED);
	/*
	 * An elastic grant placed during a skip will never ask to grow. Bench arms
	 * (69:108): EXT_NEVER treats every elastic grant as skipped, EXT_ALWAYS none.
	 */
	const bool     no_extend =
		extendable &&
		(IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_ARM_EXT_NEVER) ||
		 (!IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_ARM_EXT_ALWAYS) &&
		  (ext_skip > 0u)));
	const uint32_t end_margin_us = (extendable && !no_extend)
					       ? END_MARGIN_EXTEND_US
					       : END_MARGIN_FIXED_US;

	if (!session_open) {
		stats.den_no_session++;
		return GATE_DENIED;
	}
	now = radiant_radio_now();

	stats.acquires++;
	if (prio == RADIANT_GATE_PRIO_HIGH) {
		stats.hi_acq++;
	}

	if (g.granted) {
		stats.acq_in_grant++;
		/*
		 * A grant is already held. This is what follow_on_us reserves: the
		 * acknowledged-data reply is armed from the receive callback, 1.56 ms before it
		 * must go out, too late to place a request. Serve it from the held air if its
		 * own air fits: it must start no earlier than now and end, with the tail,
		 * inside the grant.
		 *
		 * Its own follow_on_us is clipped at grant_end, not required to fit. A fixed
		 * grant never grows, so a chain of follow-ons can never outlive the first
		 * grant; requiring the follow-on to fit refused the originator's ACK window
		 * (follow-on NEXT_PACKET_US since e042a10, ~1.5 ms past the data packet's
		 * grant) and every originated acknowledged message failed on MPSL builds.
		 * (A one-packet transfer's window carries no follow-on since 2026-11-14;
		 * a multi-packet burst's still does.) Whatever is later armed into the clipped part (a retry, a burst's next
		 * packet) is refused by this same test and fails as before e042a10.
		 */
		if (t_from >= now &&
		    t_to + (radiant_time_t)TAIL_MARGIN_US <= g.grant_end) {
			g.release_wanted = false;
			if (prio == RADIANT_GATE_PRIO_HIGH) {
				stats.hi_reserve++;
			}
			return GATE_GRANTED;
		}
		/*
		 * Not fitting is not refusal. The common case is a tracked window's next slot,
		 * armed a quarter second out while the current timeslot is still live. Refusing
		 * it lost every other window. Such a request falls through and is placed like
		 * any future reservation. A genuinely close reply is still refused by the
		 * too-near test below.
		 */
		stats.acq_in_grant_denied++;
	}

	if (g.pending) {
		/* One request per session (the HAL's one-operation rule). The backend's EBUSY normally catches this first. */
		stats.den_pending++;
		return GATE_DENIED;
	}

	start = (t_from > (radiant_time_t)HEAD_MARGIN_US)
			? (t_from - (radiant_time_t)HEAD_MARGIN_US)
			: 0u;
	/* Covers the operation, the follow-on air and the tail. */
	end = t_to + (radiant_time_t)follow_on_us +
	      (radiant_time_t)TAIL_MARGIN_US;

	if (end <= start) {
		stats.den_degenerate++;
		return GATE_DENIED;
	}

	/*
	 * No phase skew here. The reservation must cover the operation, whose t_open and
	 * t_close are absolute. Skewing the reservation alone starts the grant after its
	 * window was due, and program_rx() refuses the stale t_open with ETIME. Phase is
	 * broken by BLOCKED_RUN_REANCHOR instead.
	 *
	 * Not proven: the skew was introduced against a 100 ms BLE advertiser, and its
	 * removal was measured only against OpenThread. Re-measure the BLE case.
	 */
	(void)elastic_skew_us;
	/*
	 * The timeslot outlives `end` by this class's end margin, since the compare that
	 * returns ACTION_END fires that far before expiry. A tracked window or transmit
	 * pays END_MARGIN_FIXED_US here, 1250 us less than EXTEND.
	 */
	len = (uint64_t)(end - start) + (uint64_t)end_margin_us;
	if (len < MPSL_TIMESLOT_LENGTH_MIN_US) {
		len = MPSL_TIMESLOT_LENGTH_MIN_US;
	}
	if (len > MPSL_TIMESLOT_LENGTH_MAX_US) {
		/* The scheduler is told caps.max_window_us so it never asks for this.
		 * Reaching here means the two disagree: refuse rather than shorten silently. */
		stats.refused_too_long++;
		if (stats.refused_too_long <= 2u) {
			/* Cast, not %lu: nrfxlib's constants are UL and would trip -Wformat. */
			printk("gate: too long - %u us, max %u (window cap %u)\n",
			       (unsigned)len,
			       (unsigned)MPSL_TIMESLOT_LENGTH_MAX_US,
			       (unsigned)MAX_WINDOW_US);
		}
		return GATE_DENIED;
	}

#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
	/*
	 * ADR 0018 (d2): is this the operation a booking was placed for? If its air lies
	 * inside the booked air, the request already with MPSL serves it, and nothing new
	 * is requested. A booking MPSL refused refuses its operation here, synchronously.
	 */
	for (uint32_t i = 0u; i < NSESS; i++) {
		struct sess *b = &ss[i];

		if (!b->open || b->role != ROLE_BOOK) {
			continue;
		}
		/* Same class only: a chunk's booking grows and must not serve a tracked
		 * window (ADR 0013 rule 1), and a chunk must not take a tracked booking. */
		if (start >= b->r_start && end <= b->r_end &&
		    b->ng.extendable == elastic_op) {
			if (b->book_failed) {
				b->book_failed = false;
				b->role = ROLE_NONE;
				GSTAT(stats.book_denied++);
				return GATE_DENIED;
			}
			{
				/* One step, as far as this session's START can see. The role is
				 * re-tested inside the mask: a START since the test above has already
				 * handed the grant back. */
				const uint32_t zkey = radiant_swi_zli_lock();
				const bool     still_booked = (b->role == ROLE_BOOK);

				if (still_booked) {
					g.op_sess = (uint8_t)i;
					g.pending = true;
					b->role = ROLE_OP;
				}
				radiant_swi_zli_unlock(zkey);
				if (!still_booked) {
					continue;
				}
			}
			GSTAT(stats.book_hit++);
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
			if (b->r_start < now + 1000u) {
				stats.hit_late++;
			}
#endif
			if (prio == RADIANT_GATE_PRIO_HIGH) {
				stats.hi_pending++;
			}
			deadline_arm((uint32_t)(t_from - now) + DEADLINE_SLACK_US);
			return GATE_PENDING;
		}
		if (start < b->r_end && end > b->r_start) {
			GSTAT(stats.book_mismatch++);
		}
	}
#endif

	/* Any free session; one whose timeslot is running is used only if nothing else is free. */
	s = pick_free(false);
	if (s == NULL) {
		/* Every session has a request out. Placing now would only cost an -NRF_EAGAIN
		 * round trip. Reachable when the backstop beat MPSL's answer, so it is counted. */
		if (n_open <= 1u) {
			stats.den_owed++;
		} else {
			GSTAT(stats.den_nosess++);
		}
		return GATE_DENIED;
	}

	/*
	 * Too near to place at all (grant latency is ~1695 us). Reported as DENIED, not
	 * ETIME: the instant is reachable, the air is not. The acknowledged-data reply
	 * path relies on this synchronous refusal. Elastic requests use their own floor.
	 */
	{
		uint32_t place_min = PLACE_MIN_US;

#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
		/* elastic_op, not extendable: the EL_FIXED arm keeps the scan's lead. */
		if (elastic_op && d2_on) {
			place_min = el_floor_us();
		}
#endif
		if (start < now + (radiant_time_t)place_min) {
			stats.refused_too_near++;
			if (stats.refused_too_near <= 3u) {
				printk("gate: too near - start +%d us, need +%u\n",
				       (int)(int64_t)(start - now), place_min);
			}
			return GATE_DENIED;
		}
	}

	if (!s->anchor_valid) {
		/*
		 * No anchor. Re-bootstrap rather than request this window with EARLIEST, which
		 * has no fixed relation to the start (see bootstrap_anchor()). DENIED is accurate:
		 * the air was not obtained. The bootstrap provides an anchor for next time.
		 */
		stats.den_no_anchor++;
		bootstrap_anchor(s);
		return GATE_DENIED;
	}
	if (!build_normal(s, start, end, len, extendable, no_extend, end_margin_us,
			  prio)) {
		return GATE_DENIED;
	}
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_EL_FIXED)
	/*
	 * Bench arm (69:108): a scan chunk asks for one fixed first bite and never
	 * grows. want_len_us keeps the whole chunk, so the TIMER0 compare can tell a
	 * cut chunk from a whole one and abort it (see SIGNAL_TIMER0).
	 */
	if (elastic_op && s->ng.granted_len_us > elastic_initial_us) {
		s->ng.granted_len_us = elastic_initial_us;
		s->req.params.normal.length_us = elastic_initial_us;
		GSTAT(s->ts_end = start + (radiant_time_t)elastic_initial_us);
	}
#endif

	/*
	 * Placed later from the work queue, not here: radiant_sched.c arms from the RADIO
	 * interrupt, where mpsl_timeslot_request() is not callable. The core is told PENDING.
	 */
	s->r_start = start;
	s->r_end = end;
	s->book_failed = false;
	/* A fresh request carries no droppable air: its whole length is the operation's. */
	s->book_tail_us = 0u;
	s->role = ROLE_OP;
	g.op_sess = sess_idx(s);
	g.pending = true;
	want_place_set(s);
	if (prio == RADIANT_GATE_PRIO_HIGH) {
		stats.hi_pending++;
	}
	radiant_swi_pend(RADIANT_SWI_GATE_REQUEST);

	/* Backstop: the operation's start plus DEADLINE_SLACK_US. */
	deadline_arm((uint32_t)(t_from - now) + DEADLINE_SLACK_US);

	return GATE_PENDING;
}

/*
 * Build a NORMAL request into session s for [start, end]; len is the whole timeslot
 * including the end margin. Returns false if the anchor cannot express it.
 */
static bool build_normal(struct sess *s, radiant_time_t start, radiant_time_t end,
			 uint64_t len, bool extendable, bool no_extend,
			 uint32_t end_margin_us, uint8_t prio)
{
	mpsl_timeslot_request_t *const rq = &s->req;

	/* distance_us is measured from this session's previous START. A blocked request leaves the anchor unchanged. */
	if (start <= s->anchor) {
		stats.refused_too_near++;
		return false;
	}
	if ((start - s->anchor) > MPSL_TIMESLOT_DISTANCE_MAX_US) {
		/* Further out than a NORMAL request can express. Re-bootstrap rather than
		 * truncate: a truncated distance would place the grant in the wrong place. */
		stats.den_distance++;
		s->anchor_valid = false;
		return false;
	}
	memset(rq, 0, sizeof(*rq));
	rq->request_type = MPSL_TIMESLOT_REQ_TYPE_NORMAL;
	rq->params.normal.hfclk = GATE_HFCLK_CFG;
	/* HIGH for tracked windows and transmits, NORMAL for elastic. This ranks only our own requests. */
	rq->params.normal.priority =
		(prio == RADIANT_GATE_PRIO_HIGH)
			? MPSL_TIMESLOT_PRIORITY_HIGH
			: MPSL_TIMESLOT_PRIORITY_NORMAL;
	rq->params.normal.distance_us = (uint32_t)(start - s->anchor);
	/* length_us is written BELOW, after the elastic clamp - see there. */

	/* Staged, not written to `g`: this can run during a live grant, and these describe the NEXT one. */
	s->ng.want_len_us = (uint32_t)len;
	s->ng.grant_end = end;
	/* Decided at the top of gate_acquire(), since len depends on it. */
	s->ng.extendable = extendable;
	s->ng.no_extend = no_extend;
	/* The margin travels with the request: every compare for this grant uses g.end_margin_us. */
	s->ng.end_margin_us = end_margin_us;
	s->ng.high_prio = (prio == RADIANT_GATE_PRIO_HIGH);

	/* The elastic class asks small and grows (ADR 0013); everything else asks for what it needs. */
	if (extendable && len > (uint64_t)elastic_initial_us) {
		len = elastic_initial_us;
	}
	s->ng.granted_len_us = (uint32_t)len;

	/*
	 * The length assignment must be the last word, below every clamp, and must read
	 * next_grant.granted_len_us rather than `len`, so the request matches this file's
	 * bookkeeping. Assigning above the clamp left MPSL asked for the whole ~96.7 ms
	 * window, so the elastic give-way mechanisms moved a number the request never
	 * contained, and a grant that large is never given, so it never grows.
	 */
	rq->params.normal.length_us = s->ng.granted_len_us;
	GSTAT(s->ts_end = start + (radiant_time_t)s->ng.granted_len_us);
	return true;
}

bool gate_held(void)
{
	/* g.granted, not g.hw_held: an in-grant arm needs the core's permission, which gate_release() clears early. */
	return g.granted;
}

void gate_forget_pending(void)
{
	if (g.pending) {
		op_forget();
		deadline_stop();
	}
}

#if defined(CONFIG_RADIANT_GATE_MPSL_D2)
bool gate_booking(void)
{
	return d2_on;
}

/*
 * A booking depends on the core arming into it. Normally the chunk in front of it
 * ends a few ms before, and that completion arms the operation. If it does not, the
 * booked grant starts with nothing staged and the slot is lost. So one timer, for
 * the earliest outstanding booking, tells the core 2 ms before the start. Later than
 * any healthy chunk's end, so the ordinary path never triggers it. The expiry only
 * pends the trampoline.
 */
#define BOOK_DUE_LEAD_US 2000u
#define BOOK_DUE_SLOP_US 200u

static void booking_due_expired(struct k_timer *t)
{
	ARG_UNUSED(t);
	radiant_swi_pend(RADIANT_SWI_GATE_DUE);
}
static K_TIMER_DEFINE(booking_due_timer, booking_due_expired, NULL);

static void booking_due_rearm(void)
{
	radiant_time_t best = RADIANT_TIME_NEVER;
	radiant_time_t now = radiant_radio_now();

	for (uint32_t i = 0u; i < NSESS; i++) {
		const struct sess *b = &ss[i];

		if (b->open && b->role == ROLE_BOOK && !b->book_failed &&
		    !b->due_told && b->r_start < best) {
			best = b->r_start;
		}
	}
	if (best == RADIANT_TIME_NEVER) {
		return;
	}
	k_timer_start(&booking_due_timer,
		      K_USEC((best > now + BOOK_DUE_LEAD_US)
				     ? (uint32_t)(best - now - BOOK_DUE_LEAD_US)
				     : 0u),
		      K_NO_WAIT);
}

static void booking_due_stop(void)
{
	k_timer_stop(&booking_due_timer);
}

static void booking_due_fire(void)
{
	radiant_time_t now = radiant_radio_now();
	bool           due = false;

	for (uint32_t i = 0u; i < NSESS; i++) {
		struct sess *b = &ss[i];

		if (b->open && b->role == ROLE_BOOK && !b->book_failed &&
		    !b->due_told &&
		    b->r_start <= now + BOOK_DUE_LEAD_US + BOOK_DUE_SLOP_US) {
			b->due_told = true;
			due = true;
		}
	}
	if (due) {
		radiant_nrf_gate_on_booking_due();
	}
	booking_due_rearm();
}

/*
 * ADR 0018 (d2): place a request for an operation that will be armed later. Always
 * the fixed class, always in a session other than one carrying a staged operation,
 * and refused rather than placed with less than PLACE_MIN_US of notice.
 */
int gate_book(radiant_time_t t_from, radiant_time_t t_to, uint16_t follow_on_us)
{
	return gate_book_tail(t_from, t_to, follow_on_us, 0u);
}

int gate_book_tail(radiant_time_t t_from, radiant_time_t t_to, uint16_t follow_on_us,
		   uint16_t tail_us)
{
	radiant_time_t now;
	radiant_time_t start;
	radiant_time_t end;
	uint64_t       len;
	struct sess   *s;

	if (!d2_on || !session_open) {
		return GATE_BOOK_ENOTSUP;
	}
	now = radiant_radio_now();
	start = (t_from > (radiant_time_t)HEAD_MARGIN_US)
			? (t_from - (radiant_time_t)HEAD_MARGIN_US)
			: 0u;
	end = t_to + (radiant_time_t)follow_on_us + (radiant_time_t)tail_us +
	      (radiant_time_t)TAIL_MARGIN_US;
	if (end <= start) {
		return GATE_BOOK_EDENIED;
	}
	len = (uint64_t)(end - start) + (uint64_t)END_MARGIN_FIXED_US;
	if (len < MPSL_TIMESLOT_LENGTH_MIN_US) {
		len = MPSL_TIMESLOT_LENGTH_MIN_US;
	}
	if (len > MPSL_TIMESLOT_LENGTH_MAX_US) {
		return GATE_BOOK_EDENIED;
	}
	/* Already booked (the scheduler asks again after a rebuild)? */
	for (uint32_t i = 0u; i < NSESS; i++) {
		if (ss[i].open && ss[i].role == ROLE_BOOK && !ss[i].book_failed &&
		    !ss[i].ng.extendable &&
		    start >= ss[i].r_start && end <= ss[i].r_end) {
			return GATE_BOOK_OK;
		}
	}
	s = pick_free(false);
	if (s == NULL) {
		GSTAT(stats.book_busy++);
		return GATE_BOOK_EBUSY;
	}
	if (start < now + (radiant_time_t)PLACE_MIN_US) {
		GSTAT(stats.book_near++);
		return GATE_BOOK_EDENIED;
	}
	if (!s->anchor_valid) {
		bootstrap_anchor(s);
		return GATE_BOOK_EDENIED;
	}
	if (!build_normal(s, start, end, len, false, false, END_MARGIN_FIXED_US,
			  RADIANT_GATE_PRIO_HIGH)) {
		return GATE_BOOK_EDENIED;
	}
	s->r_start = start;
	s->r_end = end;
	s->book_failed = false;
	s->role = ROLE_BOOK;
	want_place_set(s);
	s->due_told = false;
	s->book_tail_us = tail_us;
	s->book_replaced = false;
	GSTAT(stats.book_placed++);
	radiant_swi_pend(RADIANT_SWI_GATE_REQUEST);
	booking_due_rearm();
	return GATE_BOOK_OK;
}
#endif

static void gate_release_masked(void);

/*
 * Runs under a ZLI mask, not irq_lock(), because the TIMER0 signal is a zero-latency
 * interrupt that irq_lock() does not mask. A TIMER0 end landing between the g.granted
 * test and the CC0 write would have this re-arm COMPARE0 on a timer MPSL owns again,
 * or hand the radio back twice. The end margin absorbs the mask's latency.
 */
void gate_release(void)
{
	uint32_t key = radiant_swi_zli_lock();

	gate_release_masked();
	radiant_swi_zli_unlock(key);
}

static void gate_release_masked(void)
{
	if (!g.granted) {
		/* Nothing held. A pending request is cancelled by the core's terminal, not here (see op_forget()). */
		op_forget();
		deadline_stop();
		/*
		 * Reached while the radio is still on loan, between a previous gate_release() and
		 * its compare firing. Idempotent. On a lost compare, returning the radio here stops
		 * radiant_radio_disable() touching the shared interrupt mask while SUBSCRIBE_RXEN
		 * is still swapped out.
		 */
		gate_hand_back();
		return;
	}

	/*
	 * Give the air back by provoking a signal: this runs in the RADIO interrupt, so
	 * the TIMER0 compare moves to just ahead of the count, and SIGNAL_TIMER0 ends the
	 * grant. g.granted is cleared here, not when ACTION_END returns, so an acquire in
	 * that gap cannot take the "grant already held" branch for air already handed back.
	 */
	g.granted = false;
	g.release_wanted = true;
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
	if (!g.extendable) {
		uint32_t d = (uint32_t)(radiant_radio_now() - ss[g.live_sess].anchor);

		stats.rel_fixed_last = d;
		if (d > stats.rel_fixed_max) {
			stats.rel_fixed_max = d;
		}
	}
#endif

	/*
	 * A compare written behind the counter never fires, and MPSL then asserts on the
	 * overstay. Two ticks of lead is not enough for the CAPTURE1, CC0 and MPSL writes,
	 * so the compare is placed RELEASE_LEAD_US ahead. The re-read below restores the
	 * backstop compare if even that loses the race. Late is survivable; absent is not.
	 */
	{
		/* Same margin as the compare SIGNAL_START armed. END_MARGIN_EXTEND_US here
		 * would end tracked windows and transmits 1250 us early. */
		uint32_t cc_backstop = (g.granted_len_us > g.end_margin_us)
					       ? (g.granted_len_us - g.end_margin_us)
					       : (g.granted_len_us / 2u);
		uint32_t want;

		nrf_timer_task_trigger(MPSL_TIMER0, NRF_TIMER_TASK_CAPTURE1);
		want = nrf_timer_cc_get(MPSL_TIMER0, 1) + RELEASE_LEAD_US;
		GSTAT(stats.rel_cap_last = want - RELEASE_LEAD_US);
		GSTAT(stats.rel_est_last =
			(uint32_t)(radiant_radio_now() - ss[g.live_sess].anchor));

		if (want < cc_backstop) {
			nrf_timer_cc_set(MPSL_TIMER0, 0, want);
			nrf_timer_event_clear(MPSL_TIMER0,
					      NRF_TIMER_EVENT_COMPARE0);
			nrf_timer_int_enable(MPSL_TIMER0,
					     NRF_TIMER_INT_COMPARE0_MASK);

			nrf_timer_task_trigger(MPSL_TIMER0,
					       NRF_TIMER_TASK_CAPTURE1);
			if (nrf_timer_cc_get(MPSL_TIMER0, 1) >= want &&
			    !nrf_timer_event_check(MPSL_TIMER0,
						   NRF_TIMER_EVENT_COMPARE0)) {
				nrf_timer_cc_set(MPSL_TIMER0, 0, cc_backstop);
				GSTAT(stats.rel_br_restore++);
			} else {
				GSTAT(stats.rel_br_ok++);
			}
		} else {
			GSTAT(stats.rel_br_inside++);
			/* Already inside the end margin: the armed compare is right, and moving it can only lose. */
			nrf_timer_int_enable(MPSL_TIMER0,
					     NRF_TIMER_INT_COMPARE0_MASK);
		}
		/*
		 * The radio restore must not depend on the compare firing. The compare stays
		 * armed (gate_hand_back(), not timer0_disarm()), since that is how MPSL ends a
		 * timeslot. If it is lost, SESSION_IDLE would return the radio only eventually,
		 * leaving the receiver deaf. So return it now; a later TIMER0 signal finds it
		 * already returned.
		 */
		stats.release_disarm++;
		gate_hand_back();
	}
}

bool gate_extend(uint32_t us)
{
	/* Extensions are driven from the signal callback (ACTION_EXTEND). This stub keeps the seam the same as a RAIL gate's. */
	ARG_UNUSED(us);
	return false;
}

#if defined(CONFIG_MPSL_ASSERT_HANDLER) && defined(CONFIG_RADIANT_GATE_MPSL_STATS)
/*
 * Bench dump for assert 69:108. Everything here is a register or memory read and
 * printk: it may run in MPSL's zero-latency context, so no kernel call that needs
 * an SVC and no lock. Every line is "key=value" pairs under the *** MPSL ASSERT line.
 */
#include <zephyr/linker/linker-defs.h>
#include <cmsis_core.h>
#include <hal/nrf_radio.h>

/* The ISR stack, to bound the MSP walk (declared as kernel_internal.h does). */
K_KERNEL_PINNED_STACK_ARRAY_DECLARE(z_interrupt_stacks, CONFIG_MP_MAX_NUM_CPUS,
				    CONFIG_ISR_STACK_SIZE);

/* IRQ lines MPSL connects (nrf/subsys/mpsl/init/mpsl_init.c). */
#if defined(CONFIG_SOC_COMPATIBLE_NRF54LX)
#define DIAG_IRQ_MPSL_TIMER ((int32_t)TIMER10_IRQn)
#define DIAG_IRQ_MPSL_RTC   ((int32_t)GRTC_3_IRQn)
#define DIAG_IRQ_RADIO      ((int32_t)RADIO_0_IRQn)
#define DIAG_IRQ_CLOCK      ((int32_t)CLOCK_POWER_IRQn)
#elif defined(CONFIG_SOC_COMPATIBLE_NRF52X) || defined(CONFIG_SOC_COMPATIBLE_NRF53X)
#define DIAG_IRQ_MPSL_TIMER ((int32_t)TIMER0_IRQn)
#define DIAG_IRQ_MPSL_RTC   ((int32_t)RTC0_IRQn)
#define DIAG_IRQ_RADIO      ((int32_t)RADIO_IRQn)
#define DIAG_IRQ_CLOCK      ((int32_t)NRFX_IRQ_NUMBER_GET(NRF_CLOCK))
#else
#define DIAG_IRQ_MPSL_TIMER (-100)
#define DIAG_IRQ_MPSL_RTC   (-100)
#define DIAG_IRQ_RADIO      (-100)
#define DIAG_IRQ_CLOCK      (-100)
#endif

/* Words walked up from the handler's stack pointer. */
#define DIAG_STACK_WORDS 48u

static const char *diag_ipsr_name(uint32_t ipsr)
{
	const int32_t irq = (int32_t)ipsr - 16;

	if (ipsr == 0u) {
		return "thread";
	}
	if (ipsr < 16u) {
		return "sys_exception";
	}
	if (irq == DIAG_IRQ_MPSL_TIMER) {
		return "MPSL_TIMER0";
	}
	if (irq == DIAG_IRQ_RADIO) {
		return "RADIO";
	}
	if (irq == DIAG_IRQ_MPSL_RTC) {
		return "MPSL_RTC_GRTC";
	}
	if (irq == DIAG_IRQ_CLOCK) {
		return "CLOCK_POWER";
	}
#if defined(CONFIG_MPSL_LOW_PRIO_IRQN)
	if (irq == CONFIG_MPSL_LOW_PRIO_IRQN) {
		return "MPSL_LOW_PRIO_SWI";
	}
#endif
#if defined(CONFIG_RADIANT_SWI_IRQN)
	if (irq == CONFIG_RADIANT_SWI_IRQN) {
		return "RADIANT_SWI";
	}
#endif
	return "other_irq";
}

/*
 * Walk up the stack from sp (never past top), printing only words that look like
 * a return address (inside the image's text, Thumb bit set) or an EXC_RETURN
 * (0xFFFFFFxx, marked X: an exception frame sits just above it). Resolve against
 * build/.../zephyr/zephyr.map (or arm-zephyr-eabi-addr2line -f on zephyr.elf),
 * clearing bit 0 first.
 */
static void diag_stack_walk(uintptr_t sp, uintptr_t top)
{
	const uintptr_t lo = (uintptr_t)__text_region_start;
	const uintptr_t hi = (uintptr_t)__text_region_end;

	printk("    stack_sp=0x%08x stack_top=0x%08x words=", (unsigned)sp,
	       (unsigned)top);
	for (uint32_t i = 0u; i < DIAG_STACK_WORDS; i++) {
		const uintptr_t a = sp + 4u * i;
		uint32_t        v;

		if (a + 4u > top || a < sp) {
			break;
		}
		v = *(const volatile uint32_t *)a;
		if ((v & 1u) != 0u && (uintptr_t)(v & ~1u) >= lo &&
		    (uintptr_t)(v & ~1u) < hi) {
			printk(" +%u:%08x", (unsigned)(4u * i), (unsigned)v);
		} else if ((v & 0xFFFFFF00u) == 0xFFFFFF00u) {
			printk(" +%u:X%08x", (unsigned)(4u * i), (unsigned)v);
		}
	}
	printk("\n");
}

/* Fallback bound when sp is outside the stack we expected: the end of SRAM. */
static uintptr_t diag_ram_end(void)
{
	return (uintptr_t)CONFIG_SRAM_BASE_ADDRESS +
	       (uintptr_t)CONFIG_SRAM_SIZE * 1024u;
}

static void diag_context(uint32_t ipsr, uint32_t control, uintptr_t msp,
			 uintptr_t psp, uintptr_t ret)
{
	const struct k_thread *const t = k_current_get();
#if defined(CONFIG_THREAD_STACK_INFO)
	const uintptr_t tlo = (t != NULL) ? (uintptr_t)t->stack_info.start : 0u;
	const uintptr_t thi = (t != NULL) ? tlo + (uintptr_t)t->stack_info.size : 0u;
#else
	const uintptr_t tlo = 0u;
	const uintptr_t thi = 0u;
#endif
	const uintptr_t ilo = (uintptr_t)z_interrupt_stacks[0];
	const uintptr_t ihi = (uintptr_t)z_interrupt_stacks[0] +
			      sizeof(z_interrupt_stacks[0]);
	/* Handler mode always runs on MSP; thread mode on PSP when CONTROL.SPSEL. */
	const bool      on_psp = (ipsr == 0u) && ((control & 2u) != 0u);
	const uintptr_t sp = on_psp ? psp : msp;
	uintptr_t       top;

	printk("    ipsr=%u ctx=%s control=0x%x msp=0x%08x psp=0x%08x ret=0x%08x "
	       "thread=%p thread_stack=0x%08x..0x%08x\n",
	       (unsigned)ipsr, diag_ipsr_name(ipsr), (unsigned)control,
	       (unsigned)msp, (unsigned)psp, (unsigned)ret, (void *)t,
	       (unsigned)tlo, (unsigned)thi);
	/*
	 * In handler mode, the hardware frame of the thread the outermost interrupt
	 * preempted is at PSP (r0 r1 r2 r3 r12 lr pc xpsr). Nested interrupts' frames
	 * are on MSP and show up in the walk below, after an X (EXC_RETURN) word.
	 */
	if (ipsr != 0u && psp >= tlo && psp + 32u <= thi) {
		const volatile uint32_t *const f = (const volatile uint32_t *)psp;

		printk("    psp_frame_lr=0x%08x psp_frame_pc=0x%08x "
		       "psp_frame_xpsr=0x%08x\n",
		       (unsigned)f[5], (unsigned)f[6], (unsigned)f[7]);
	} else {
		printk("    psp_frame=none\n");
	}
	if (on_psp) {
		top = (sp >= tlo && sp < thi) ? thi : diag_ram_end();
	} else {
		top = (sp >= ilo && sp < ihi) ? ihi : diag_ram_end();
	}
	diag_stack_walk(sp, top);
}

static void diag_clock(void)
{
#if defined(CLOCK_XO_STAT_STATE_Msk)
	/* nRF54L: HFXO under CLOCK.XO; tuning events. Reads only. */
	printk("    xo_run=0x%x xo_stat=0x%x ev_xostarted=%u ev_xotuned=%u "
	       "ev_xotuneerror=%u ev_xotunefailed=%u clk_intenset=0x%x "
	       "pll_stat=0x%x lf_src=0x%x lf_run=0x%x lf_stat=0x%x\n",
	       (unsigned)NRF_CLOCK->XO.RUN, (unsigned)NRF_CLOCK->XO.STAT,
	       (unsigned)NRF_CLOCK->EVENTS_XOSTARTED,
	       (unsigned)NRF_CLOCK->EVENTS_XOTUNED,
	       (unsigned)NRF_CLOCK->EVENTS_XOTUNEERROR,
	       (unsigned)NRF_CLOCK->EVENTS_XOTUNEFAILED,
	       (unsigned)NRF_CLOCK->INTENSET, (unsigned)NRF_CLOCK->PLL.STAT,
	       (unsigned)NRF_CLOCK->LFCLK.SRC, (unsigned)NRF_CLOCK->LFCLK.RUN,
	       (unsigned)NRF_CLOCK->LFCLK.STAT);
#elif defined(CLOCK_HFCLKSTAT_STATE_Msk)
	/* nRF52/nRF53. */
	printk("    hfclkrun=0x%x hfclkstat=0x%x ev_hfclkstarted=%u "
	       "lfclkrun=0x%x lfclkstat=0x%x ev_lfclkstarted=%u "
	       "clk_intenset=0x%x\n",
	       (unsigned)NRF_CLOCK->HFCLKRUN, (unsigned)NRF_CLOCK->HFCLKSTAT,
	       (unsigned)NRF_CLOCK->EVENTS_HFCLKSTARTED,
	       (unsigned)NRF_CLOCK->LFCLKRUN, (unsigned)NRF_CLOCK->LFCLKSTAT,
	       (unsigned)NRF_CLOCK->EVENTS_LFCLKSTARTED,
	       (unsigned)NRF_CLOCK->INTENSET);
#else
	printk("    clock=unknown_soc\n");
#endif
}

#define DIAG_T0_CC_N (sizeof(MPSL_TIMER0->CC) / sizeof(MPSL_TIMER0->CC[0]))

/*
 * MPSL_TIMER0 exactly as MPSL and the gate left it: every CC, every COMPARE event,
 * INTENSET and SHORTS are read BEFORE this handler captures the counter into CC1,
 * which it may do only because the system is going down.
 */
static void diag_timer0(void)
{
	uint32_t cc[DIAG_T0_CC_N];
	uint32_t ev = 0u;
	uint32_t inten = MPSL_TIMER0->INTENSET;
	uint32_t shorts = MPSL_TIMER0->SHORTS;
	uint32_t count;

	for (uint32_t i = 0u; i < DIAG_T0_CC_N; i++) {
		cc[i] = MPSL_TIMER0->CC[i];
		if (MPSL_TIMER0->EVENTS_COMPARE[i] != 0u) {
			ev |= 1u << i;
		}
	}
	nrf_timer_task_trigger(MPSL_TIMER0, NRF_TIMER_TASK_CAPTURE1);
	count = nrf_timer_cc_get(MPSL_TIMER0, NRF_TIMER_CC_CHANNEL1);

	printk("    t0_intenset=0x%08x t0_shorts=0x%x t0_ev_compare=0x%02x "
	       "t0_count=%u (captured into CC1 after the reads)\n",
	       (unsigned)inten, (unsigned)shorts, (unsigned)ev, (unsigned)count);
	printk("   ");
	for (uint32_t i = 0u; i < DIAG_T0_CC_N; i++) {
		printk(" t0_cc%u=%u", (unsigned)i, (unsigned)cc[i]);
	}
	printk("\n");
}

static void diag_radio(void)
{
	printk("    radio_state=%u radio_shorts=0x%08x radio_intenset=0x%08x",
	       (unsigned)nrf_radio_state_get(NRF_RADIO),
	       (unsigned)nrf_radio_shorts_get(NRF_RADIO),
	       (unsigned)nrf_radio_int_enable_check(NRF_RADIO, UINT32_MAX));
#if defined(RADIO_SUBSCRIBE_TXEN_EN_Msk)
	/* DPPI routing: whose channels the RADIO's endpoints point at right now. */
	printk(" sub_rxen=0x%08x sub_txen=0x%08x sub_disable=0x%08x "
	       "sub_rssistart=0x%08x pub_address=0x%08x pub_rxready=0x%08x",
	       (unsigned)NRF_RADIO->SUBSCRIBE_RXEN,
	       (unsigned)NRF_RADIO->SUBSCRIBE_TXEN,
	       (unsigned)NRF_RADIO->SUBSCRIBE_DISABLE,
	       (unsigned)NRF_RADIO->SUBSCRIBE_RSSISTART,
	       (unsigned)NRF_RADIO->PUBLISH_ADDRESS,
	       (unsigned)NRF_RADIO->PUBLISH_RXREADY);
#endif
	printk("\n");
}

static void diag_gate_state(uint32_t now)
{
	printk("    last_sess=%u since_start_us=%u since_end_us=%u ts_live=%d "
	       "live_sess=%u hw_held=%d ended=%d extendable=%d no_extend=%d "
	       "end_margin_us=%u op_sess=%u deny_wanted=%d ext_skip=%u "
	       "ext_fail_run=%u elastic_initial_us=%u\n",
	       (unsigned)dbg.last_sess, (unsigned)(now - dbg.t_start),
	       (unsigned)(now - dbg.t_end), (int)g.ts_live,
	       (unsigned)g.live_sess, (int)g.hw_held, (int)g.ended,
	       (int)g.extendable, (int)g.no_extend, (unsigned)g.end_margin_us,
	       (unsigned)g.op_sess, (int)g.deny_wanted, (unsigned)ext_skip,
	       (unsigned)ext_fail_run, (unsigned)elastic_initial_us);
	for (uint32_t i = 0u; i < NSESS; i++) {
		const struct sess *const s = &ss[i];
		const bool earliest =
			s->req.request_type == MPSL_TIMESLOT_REQ_TYPE_EARLIEST;

		printk("    s%u: id=%u open=%d role=%u owes=%d want_place=%d "
		       "wait_live=%d boot=%d anchor_valid=%d book_failed=%d "
		       "r_start_minus_now=%d req_type=%s req_prio=%u req_hfclk=%u "
		       "req_len=%u req_dist_or_timeout=%u notice_us=%d "
		       "placed=%u granted=%u blocked=%u\n",
		       (unsigned)i, (unsigned)s->id, (int)s->open,
		       (unsigned)s->role, (int)s->mpsl_owes, (int)s->want_place,
		       (int)s->wait_live, (int)s->bootstrapping,
		       (int)s->anchor_valid, (int)s->book_failed,
		       (int)(int64_t)(s->r_start - (radiant_time_t)radiant_radio_now()),
		       earliest ? "earliest" : "normal",
		       (unsigned)(earliest ? s->req.params.earliest.priority
					   : s->req.params.normal.priority),
		       (unsigned)(earliest ? s->req.params.earliest.hfclk
					   : s->req.params.normal.hfclk),
		       (unsigned)(earliest ? s->req.params.earliest.length_us
					   : s->req.params.normal.length_us),
		       (unsigned)(earliest ? s->req.params.earliest.timeout_us
					   : s->req.params.normal.distance_us),
		       (int)s->notice_us, (unsigned)s->n_placed,
		       (unsigned)s->n_granted, (unsigned)s->n_blocked);
	}
	printk("    end_start=%u end_t0skip=%u end_t0other=%u end_radio=%u "
	       "end_extfail=%u unpend_start=%u unpend_t0skip=%u unpend_t0other=%u "
	       "unpend_radio=%u unpend_extfail=%u\n",
	       stats.end_hi[END_SRC_START], stats.end_hi[END_SRC_T0_SKIP],
	       stats.end_hi[END_SRC_T0_OTHER], stats.end_hi[END_SRC_RADIO],
	       stats.end_hi[END_SRC_EXT_FAIL], stats.unpend[END_SRC_START],
	       stats.unpend[END_SRC_T0_SKIP], stats.unpend[END_SRC_T0_OTHER],
	       stats.unpend[END_SRC_RADIO], stats.unpend[END_SRC_EXT_FAIL]);
	printk("    api_inflight=%u api_sess=%u api_last=%u api_rc=%d "
	       "api_calls=%u since_api_in_us=%u since_api_out_us=%u "
	       "(api: 0 none, 1 request, 2 session_open, 3 session_close)\n",
	       (unsigned)dbg.api, (unsigned)dbg.api_sess,
	       (unsigned)dbg.api_last, (int)dbg.api_rc,
	       (unsigned)dbg.api_calls, (unsigned)(now - dbg.api_t_in),
	       (unsigned)(now - dbg.api_t_out));
}

static void diag_trace(void)
{
	const uint32_t at = trace_at;
	const uint32_t n = (at < TRACE_LEN) ? at : TRACE_LEN;

	printk("    trace (sess:sig:act+dt_us; request entries "
	       "sess:R:rc@notice_us+dt_us; oldest first):");
	for (uint32_t k = 0u; k < n; k++) {
		const struct trace_ent *const e = &trace[(at - n + k) % TRACE_LEN];

		if ((e->hdr & TRACE_REQ) != 0u) {
			printk(" %u:R:%d@%d+%u", (unsigned)(e->hdr & 0x7Fu),
			       (int)e->act, (int)e->aux * 4, (unsigned)e->dt_us);
		} else {
			printk(" %u:%u:%d+%u", (unsigned)(e->hdr & 0x7Fu),
			       (unsigned)e->sig, (int)e->act, (unsigned)e->dt_us);
		}
	}
	printk("\n");
}
#endif /* CONFIG_MPSL_ASSERT_HANDLER && CONFIG_RADIANT_GATE_MPSL_STATS */

#if defined(CONFIG_MPSL_ASSERT_HANDLER)
void mpsl_assert_handle(const char *const file, const uint32_t line)
{
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
	/* Taken first, before anything below moves the stack or the registers. */
	const uint32_t  d_ipsr = __get_IPSR();
	const uint32_t  d_control = __get_CONTROL();
	const uintptr_t d_msp = (uintptr_t)__get_MSP();
	const uintptr_t d_psp = (uintptr_t)__get_PSP();
	/* Inside m_assert_handler (mpsl_init.c), or libmpsl if that tail-called here. */
	const uintptr_t d_ret = (uintptr_t)__builtin_return_address(0);
	const uint32_t  d_now = (uint32_t)radiant_radio_now();
#endif
	/*
	 * Two signals that mean this file is wrong: OVERSTAYED (the end margin is too
	 * small) and INVALID (an action returned from a low-priority signal). The counters
	 * tell them apart; the assert site cannot.
	 *
	 * 69:108 is neither (over=0 invalid=0): it was observed in the RADIO_0 interrupt
	 * (ipsr=RADIO), 18 us after on_signal() returned ACTION_END, with no timeslot
	 * open - MPSL taking a RADIO interrupt that latched while our grant was being
	 * ended. on_signal() now clears that pending state before every high-priority
	 * END (radiant_nrf_gate_radio_irq_unpend()); the unpend_* counters below count
	 * the asserts that avoided.
	 */
	/*
	 * LOG_PANIC() first, or this handler is silent: printk goes through the log
	 * subsystem, which in deferred mode needs a thread that is about to be halted.
	 */
	LOG_PANIC();
	printk("\n*** MPSL ASSERT %s:%u  placed=%u granted=%u blocked=%u "
	       "ext=%u/%u skip=%u deadline=%u\n"
	       "    over=%u invalid=%u unknown=%u last_sig=%u "
	       "len=%u/%u granted=%d pending=%d rel=%d ext_ok=%d\n",
	       file, line, stats.placed, stats.granted, stats.blocked,
	       stats.extends_ok, stats.extends_failed, stats.ext_skipped,
	       stats.deadline_fired,
	       stats.overstayed, stats.invalid_return, stats.unknown_signal,
	       stats.last_signal, g.granted_len_us, g.want_len_us,
	       (int)g.granted, (int)g.pending, (int)g.release_wanted,
	       (int)g.extendable);
#if defined(CONFIG_RADIANT_GATE_MPSL_ARM_LOCK) ||                            \
	defined(CONFIG_RADIANT_GATE_MPSL_ARM_REQ_DISCIPLINE) ||              \
	defined(CONFIG_RADIANT_GATE_MPSL_ARM_EXT_ALWAYS) ||                  \
	defined(CONFIG_RADIANT_GATE_MPSL_ARM_EXT_NEVER) ||                   \
	defined(CONFIG_RADIANT_GATE_MPSL_ARM_EL_FIXED) ||                    \
	defined(CONFIG_RADIANT_GATE_MPSL_ARM_NO_GUARANTEE) ||                \
	defined(CONFIG_RADIANT_GATE_MPSL_ARM_DPPI_FIRST) ||                  \
	defined(CONFIG_RADIANT_GATE_MPSL_ARM_NO_T0_CAPTURE) ||               \
	(CONFIG_RADIANT_GATE_MPSL_ARM_END_MARGIN_FIXED_US != 750)
	/* Bench arms (assert 69:108) compiled into this image. */
	printk("    arms=%s%s%s%s%s%s%s%s end_margin_fixed_us=%u\n",
	       IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_ARM_LOCK) ? "lock," : "",
	       IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_ARM_REQ_DISCIPLINE) ? "req," : "",
	       IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_ARM_EXT_ALWAYS) ? "ext_always," : "",
	       IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_ARM_EXT_NEVER) ? "ext_never," : "",
	       IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_ARM_EL_FIXED) ? "el_fixed," : "",
	       IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_ARM_NO_GUARANTEE) ? "no_guarantee," : "",
	       IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_ARM_DPPI_FIRST) ? "dppi_first," : "",
	       IS_ENABLED(CONFIG_RADIANT_GATE_MPSL_ARM_NO_T0_CAPTURE) ? "no_t0_capture," : "",
	       (unsigned)END_MARGIN_FIXED_US);
#endif
#if defined(ARM_COUNTERS)
	printk("    arm_held_idle=%u arm_held_gap=%u arm_el_fixed_cut=%u "
	       "arm_dppi_first=%u\n",
	       arm_stats.held_idle, arm_stats.held_gap, arm_stats.el_fixed_cut,
	       arm_stats.dppi_first);
#endif
#if defined(CONFIG_RADIANT_GATE_MPSL_STATS)
	diag_context(d_ipsr, d_control, d_msp, d_psp, d_ret);
	diag_clock();
	diag_timer0();
	diag_radio();
	diag_gate_state(d_now);
	diag_trace();
#else
	printk("    trace (sig:act, oldest first):");
	for (uint32_t i = 0; i < TRACE_LEN; i++) {
		uint32_t idx = (trace_at >= TRACE_LEN)
				       ? ((trace_at + i) % TRACE_LEN)
				       : i;

		if (trace_at < TRACE_LEN && i >= trace_at) {
			break;
		}
		printk(" %u:%u", trace[idx] >> 4, trace[idx] & 0xFu);
	}
	printk("\n");
#endif
	/*
	 * Reboot on a shipping image, halt on a bench image (as apps/common/diag_flash_log.c
	 * does). Not k_panic(): this can run in MPSL's zero-latency signal context, where an
	 * SVC escalates to a HardFault. sys_reboot() involves no SVC.
	 */
#if defined(CONFIG_RESET_ON_FATAL_ERROR)
	sys_reboot(SYS_REBOOT_COLD);
#else
	k_fatal_halt(K_ERR_KERNEL_PANIC);
#endif
}
#endif







