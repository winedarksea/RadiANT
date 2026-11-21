/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AN ARM PREEMPTED BY THE GRANT'S END, on the default MPSL build (the option
 * CONFIG_RADIANT_NRF_LOW_POWER off: the TIMER free-runs through the other
 * stack's events). radiant.backend.lp_mpsl sweeps the granted path with the
 * option on.
 *
 * Risk, the granted path: an in-grant arm (a completion's follow-on from the
 * software interrupt, or an arm from a preemptible thread) runs gate_acquire()
 * -> GATE_GRANTED -> program_tx/rx/ed. The zero-latency signal can preempt it
 * with ACTION_END (the TIMER0 end, EXTEND_FAILED, the extension skip); the
 * hand-back clears SHORTS and the interrupt mask and the RADIO becomes the
 * SoftDevice Controller's or the 802.15.4 driver's. Resumed, the arm wrote
 * MODE, PCNF, the address, CRC, FREQUENCY, PACKETPTR and SHORTS (and INTENSET
 * RXREADY for ED) into that live RADIO and enabled TIMER->RADIO connections
 * that then fired inside the other stack's event. EXTEND_FAILED and the skip
 * also abort from the signal, which gave the claimed operation ABORTED and
 * then the arm's own result. A queued denial could land on a thread arm's
 * claimed operation.
 *
 * Risk, the staged path: an arm whose gate_acquire() answers GATE_PENDING,
 * preempted by gate_wq before it has staged the request, when MPSL refuses
 * that request (BLOCKED), its deadline expires, or its START arrives first.
 * The gate's denial then reached the backend while the arm was marked and was
 * dropped: the arm staged and returned OK with no grant, deadline or denial
 * owed, and every later arm got EBUSY. And the two-event case: a stale denial
 * held while the arm was marked, then the request's START between the staging
 * and the arm's settle (on_grant() programs the radio): the settle refused the
 * already-programmed arm EDENIED and freed the slot under a live radio.
 *
 * Production arms run inside the scheduler's pass under irq_lock, so only the
 * zero-latency signal reaches them there; the denial cases model arms made
 * outside a pass, which the HAL allows.
 *
 * Swept over every preemption point of a transmit, a receive and an energy-
 * detect arm (each outermost unlock of the zero-latency lock or CLOCK_LOCK,
 * and the return of gate_acquire()), on time and too late to make it. After
 * the arm the rest of the world runs until nothing is owed (queued denials,
 * the request's grant if still placed, the end of any loan). Always: no RADIO
 * write after a hand-back, no window connection outliving it, no terminal
 * delivered under an arm in progress, exactly one outcome per operation (a
 * refused arm and no terminal, or an accepted arm and exactly one terminal),
 * the slot free at the end, and the next arm taken.
 */

#include <zephyr/ztest.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_probe.h"

ZTEST_SUITE(radiant_backend_arm_handback, NULL, NULL, NULL, NULL, NULL);

static const char *const kn[] = { "tx", "rx", "ed" };
static const char *const mn[] = { "end", "end+start", "abort+end", "denied",
				  "blocked", "deadline", "grant-early", "denied+start" };

struct sweep_counts {
	uint32_t points;
	uint32_t refused;   /* ETIME or EDENIED */
	uint32_t edenied;
};

static void sweep(int kind, int mode, bool late, bool held, struct sweep_counts *c)
{
	const bool hand_back = mode == BACKEND_PRE_END || mode == BACKEND_PRE_END_START ||
			       mode == BACKEND_PRE_ABORT_END;
	const char *path = held ? "granted" : "staged";

	for (uint32_t n = 1u; n <= 24u; n++) {
		struct backend_preempt_result r;

		backend_preempt_arm(kind, mode, n, late, held, &r);
		if (!r.fired) {
			break;
		}
		c->points++;
		zassert_false(r.marked_after_arm, "%s %s %s n %u: arming mark left set",
			      path, kn[kind], mn[mode], n);
		zassert_true(r.rc == RADIANT_RADIO_OK_RC || r.rc == RADIANT_RADIO_ETIME ||
			     r.rc == RADIANT_RADIO_EDENIED,
			     "%s %s %s n %u: rc %d", path, kn[kind], mn[mode], n, r.rc);
		if (late && !r.staged_path) {
			zassert_true(r.rc != RADIANT_RADIO_OK_RC,
				     "%s %s %s n %u: a late arm accepted", path, kn[kind],
				     mn[mode], n);
		}
		if (r.marked_at_fire) {
			zassert_equal(r.terminals_during_arm, 0u,
				      "%s %s %s n %u: a terminal under the arm in progress (rc %d)",
				      path, kn[kind], mn[mode], n, r.rc);
		}
		/* A hand-back the arm was claimed after belongs to a loan the arm
		 * never saw (END+START before the claim gives it the new one). */
		if (hand_back && r.marked_at_fire) {
			zassert_false(r.radio_written_after_handback,
				      "%s %s %s n %u: RADIO written after the hand-back",
				      path, kn[kind], mn[mode], n);
			zassert_false(r.conns_after_arm,
				      "%s %s %s n %u: a window connection outlived the hand-back",
				      path, kn[kind], mn[mode], n);
		}
		zassert_true(r.op_idle_final, "%s %s %s n %u: the slot is wedged (rc %d)",
			     path, kn[kind], mn[mode], n, r.rc);
		zassert_false(r.conns_final, "%s %s %s n %u: a connection left on",
			      path, kn[kind], mn[mode], n);
		zassert_equal(r.terminals_final, (r.rc == RADIANT_RADIO_OK_RC) ? 1u : 0u,
			      "%s %s %s n %u: rc %d and %u terminals", path, kn[kind],
			      mn[mode], n, r.rc, r.terminals_final);
		zassert_equal(r.next_rc, RADIANT_RADIO_OK_RC,
			      "%s %s %s n %u: the next arm got %d", path, kn[kind], mn[mode],
			      n, r.next_rc);
		if (r.rc != RADIANT_RADIO_OK_RC) {
			/* A refused arm owns nothing on the radio: no window armed
			 * for an operation the caller was told does not exist. */
			zassert_false(r.conns_after_arm,
				      "%s %s %s n %u: refused (rc %d) with the radio armed",
				      path, kn[kind], mn[mode], n, r.rc);
			c->refused++;
		}
		if (r.rc == RADIANT_RADIO_EDENIED) {
			c->edenied++;
		}
	}
}

ZTEST(radiant_backend_arm_handback, test_arm_preempted_by_grant_end)
{
	for (int late = 0; late <= 1; late++) {
	for (int kind = BACKEND_ARM_TX; kind <= BACKEND_ARM_ED; kind++) {
	for (int mode = BACKEND_PRE_END; mode <= BACKEND_PRE_DENIED; mode++) {
		struct sweep_counts c = { 0 };

		sweep(kind, mode, late != 0, true, &c);
		zassert_true(c.points > 0u, "%s %s late %d: no preemption point", kn[kind],
			     mn[mode], late);
		if (mode != BACKEND_PRE_DENIED || late) {
			zassert_true(c.refused > 0u,
				     "%s %s late %d: no preemption point failed the arm",
				     kn[kind], mn[mode], late);
		}
	}
	}
	}
}

/*
 * Risk (review 2026-11-19, TODO "EXTEND_FAILED ... can abort a successor
 * staged behind a held grant"): the old operation ends early in a growing
 * grant with the release owed (grant_release_after_core()); the follow-on,
 * armed from the dispatch, misses the held air and is staged behind a new
 * request (GATE_PENDING). Once the arm's staged branch has cleared the arming
 * mark (arm_settle_staged()) and before the owed release is paid
 * (d2_settle_now() on a D2 build, grant_release_settle() after the dispatch
 * otherwise), the grant's TIMER0 compare asks EXTEND; on refusal
 * radiant_radio_abort() from the signal aborted the staged successor: ABORTED
 * for air it never had and its request forgotten (one slot lost).
 *
 * The abort is for the grant that just ended. A staged successor never had
 * that grant's air: it must stay staged, its request still placed, and end
 * with exactly one terminal from its own grant. Swept over every preemption
 * point of the staged transmit, receive and energy-detect arm, and the point
 * after the arm returns; radiant.backend.d2 runs it with d2_settle_now().
 */
ZTEST(radiant_backend_arm_handback, test_staged_successor_survives_extend_failed)
{
	for (int late = 0; late <= 1; late++) {
	for (int kind = BACKEND_ARM_TX; kind <= BACKEND_ARM_ED; kind++) {
		uint32_t points = 0u;
		uint32_t unmarked_staged = 0u;
		bool     after_arm = false;

		if (late && kind == BACKEND_ARM_ED) {
			continue;   /* refused before the gate */
		}
		for (uint32_t n = 1u; n <= 24u; n++) {
			struct backend_preempt_result r;

			backend_preempt_staged_in_grant(kind, n, late != 0, &r);
			if (!r.fired) {
				break;
			}
			points++;
			if (r.staged_at_fire && !r.marked_at_fire) {
				unmarked_staged++;
			}
			after_arm |= r.fired_after_arm;
			zassert_equal(r.rc, RADIANT_RADIO_OK_RC, "%s late %d n %u: rc %d",
				      kn[kind], late, n, r.rc);
			zassert_true(r.staged_path && r.staged_after_arm,
				     "%s late %d n %u: not staged", kn[kind], late, n);
			zassert_equal(r.aborted_final, 0u,
				      "%s late %d n %u: ABORTED for air it never had",
				      kn[kind], late, n);
			zassert_equal(r.forgets_after_point, 0u,
				      "%s late %d n %u: its request forgotten", kn[kind], late, n);
			zassert_true(r.staged_after_point && r.req_pending_after_point,
				     "%s late %d n %u: no longer staged behind its request",
				     kn[kind], late, n);
			zassert_equal(r.terminals_final, 1u, "%s late %d n %u: %u terminals",
				      kn[kind], late, n, r.terminals_final);
			zassert_true(r.op_idle_final, "%s late %d n %u: the slot is wedged",
				     kn[kind], late, n);
			zassert_false(r.conns_final, NULL);
			zassert_equal(r.next_rc, RADIANT_RADIO_OK_RC, NULL);
		}
		zassert_true(points > 0u, "%s late %d: no preemption point", kn[kind], late);
		/* The gap itself: past the settle, the mark gone, the arm staged. */
		zassert_true(unmarked_staged > 0u, "%s late %d: the gap was never reached",
			     kn[kind], late);
		zassert_true(after_arm, "%s late %d: the point after the arm never ran",
			     kn[kind], late);
	}
	}
}

ZTEST(radiant_backend_arm_handback, test_staged_arm_refused_while_staging)
{
	for (int late = 0; late <= 1; late++) {
	for (int kind = BACKEND_ARM_TX; kind <= BACKEND_ARM_ED; kind++) {
	for (int mode = BACKEND_PRE_DENIED; mode <= BACKEND_PRE_DENY_THEN_START; mode++) {
		struct sweep_counts c = { 0 };

		sweep(kind, mode, late != 0, false, &c);
		zassert_true(c.points > 0u, "%s %s late %d: no preemption point", kn[kind],
			     mn[mode], late);
		/* The arm's own request refused after gate_acquire() placed it:
		 * reached on time for every arm, and too late for TX/RX (ED's
		 * late arm is refused before the gate). */
		if (mode != BACKEND_PRE_DENIED && mode != BACKEND_PRE_DENY_THEN_START &&
		    (!late || kind != BACKEND_ARM_ED)) {
			zassert_true(c.edenied > 0u,
				     "%s %s late %d: the staged path's refusal never ran",
				     kn[kind], mn[mode], late);
		}
	}
	}
	}
}

/*
 * Risk: the hand-back left a live operation's TIMER-driven connections
 * enabled. The TIMER keeps counting on this build, so its start or close
 * compare fired TXEN, RXEN or DISABLE into the other stack's radio before the
 * gate's deferred DENIED disabled them.
 */
ZTEST(radiant_backend_arm_handback, test_handback_disables_live_connections)
{
	struct backend_preempt_result r;

	/* The last point: past the arm's span, the arm programmed, then the
	 * loan ended. */
	for (uint32_t n = 24u; n > 0u; n--) {
		backend_preempt_arm(BACKEND_ARM_RX, BACKEND_PRE_END, n, false, true, &r);
		if (r.fired) {
			break;
		}
	}
	zassert_true(r.fired, NULL);
	zassert_equal(r.rc, RADIANT_RADIO_OK_RC, "the last point is after the span");
	zassert_false(r.conns_after_arm, "a window connection survived the hand-back");
	zassert_equal(r.terminals_final, 1u, NULL);
}

/*
 * Risk (TODO "DPPI hand-back order", 2026-11-14; closed by a8ad20d): beside
 * OpenThread the grant-boundary endpoint swap moves only PUBLISH_ADDRESS and
 * SUBSCRIBE_RXEN, so RADIO SUBSCRIBE_DISABLE/TXEN/RSSISTART stay wired to
 * radiant's DPPI channels between grants. EXTEND_FAILED and the extension
 * skip hand the radio back (timer0_disarm()) before radiant_radio_abort()
 * disables the operation's connections, and a TIMER0 END that cuts a live
 * operation leaves them until the gate's DENIED. A start or close compare in
 * that gap fired TXEN or DISABLE on the 802.15.4 driver's radio. The four
 * window connections must be off at the hand-back itself, before any abort,
 * on every END shape (plain, and hand-back-then-abort) of a live receive and
 * transmit.
 */
ZTEST(radiant_backend_arm_handback, test_handback_disables_connections_before_abort)
{
	static const int modes[] = { BACKEND_PRE_END, BACKEND_PRE_ABORT_END };

	for (int kind = BACKEND_ARM_TX; kind <= BACKEND_ARM_RX; kind++) {
	for (size_t m = 0u; m < ARRAY_SIZE(modes); m++) {
		struct backend_preempt_result r;

		/* The last point: the arm programmed, the operation live. */
		for (uint32_t n = 24u; n > 0u; n--) {
			backend_preempt_arm(kind, modes[m], n, false, true, &r);
			if (r.fired) {
				break;
			}
		}
		zassert_true(r.fired, "%s %s", kn[kind], mn[modes[m]]);
		zassert_equal(r.rc, RADIANT_RADIO_OK_RC, "%s %s: not past the span",
			      kn[kind], mn[modes[m]]);
		/* Positive control: the live operation had its window wired. */
		zassert_true(r.conns_before_handback, "%s %s: nothing was connected",
			     kn[kind], mn[modes[m]]);
		zassert_false(r.conns_at_handback,
			      "%s %s: a window connection outlived the hand-back",
			      kn[kind], mn[modes[m]]);
		zassert_equal(r.terminals_final, 1u, "%s %s", kn[kind], mn[modes[m]]);
	}
	}
}

/*
 * Risk (re-review of the staged-path fix): a stale denial held while the arm
 * was marked, then the request's own START between the staging and the arm's
 * settle. on_grant() programs the radio (or fails ETIME and owes FAILED); the
 * settle saw "denial held, nothing pending" and refused the arm EDENIED,
 * freeing the slot under a programmed radio - two outcomes. The arm must stay
 * accepted: OK, then exactly one terminal (FAILED for a late one).
 */
ZTEST(radiant_backend_arm_handback, test_stale_denial_then_start_keeps_the_arm)
{
	for (int late = 0; late <= 1; late++) {
	for (int kind = BACKEND_ARM_TX; kind <= BACKEND_ARM_ED; kind++) {
		struct backend_preempt_result r;

		if (late && kind == BACKEND_ARM_ED) {
			continue;   /* refused before the gate */
		}
		/* Point 2: the claim's unlock, the arm marked and nothing placed. */
		backend_preempt_arm(kind, BACKEND_PRE_DENY_THEN_START, 2u, late != 0, false, &r);
		zassert_true(r.fired && r.marked_at_fire, "%s late %d", kn[kind], late);
		zassert_true(r.staged_path, "%s late %d: not staged", kn[kind], late);
		zassert_equal(r.rc, RADIANT_RADIO_OK_RC, "%s late %d: rc %d", kn[kind], late,
			      r.rc);
		zassert_equal(r.terminals_final, 1u, "%s late %d: %u terminals", kn[kind], late,
			      r.terminals_final);
		zassert_true(r.op_idle_final, NULL);
		zassert_false(r.conns_final, NULL);
		if (late) {
			zassert_equal(kind == BACKEND_ARM_TX ? backend_probe()->last_tx_status
							     : backend_probe()->last_rx_status,
				      RADIANT_RADIO_STATUS_FAILED,
				      "%s: a late programme owes FAILED", kn[kind]);
		}
	}
	}
}
