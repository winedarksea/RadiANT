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
 * owed, and every later arm got EBUSY.
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
				  "blocked", "deadline", "grant-early" };

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

	for (uint32_t n = 1u; n <= 16u; n++) {
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
				      "%s %s %s n %u: a terminal under the arm in progress",
				      path, kn[kind], mn[mode], n);
		}
		if (hand_back) {
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

ZTEST(radiant_backend_arm_handback, test_staged_arm_refused_while_staging)
{
	for (int late = 0; late <= 1; late++) {
	for (int kind = BACKEND_ARM_TX; kind <= BACKEND_ARM_ED; kind++) {
	for (int mode = BACKEND_PRE_DENIED; mode <= BACKEND_PRE_GRANT_EARLY; mode++) {
		struct sweep_counts c = { 0 };

		sweep(kind, mode, late != 0, false, &c);
		zassert_true(c.points > 0u, "%s %s late %d: no preemption point", kn[kind],
			     mn[mode], late);
		/* The arm's own request refused after gate_acquire() placed it:
		 * reached on time for every arm, and too late for TX/RX (ED's
		 * late arm is refused before the gate). */
		if (mode != BACKEND_PRE_DENIED && (!late || kind != BACKEND_ARM_ED)) {
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
	for (uint32_t n = 16u; n > 0u; n--) {
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
