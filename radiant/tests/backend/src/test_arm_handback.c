/* SPDX-License-Identifier: Apache-2.0 */
/*
 * AN ARM PREEMPTED BY THE GRANT'S END, on the default MPSL build (the option
 * CONFIG_RADIANT_NRF_LOW_POWER off: the TIMER free-runs through the other
 * stack's events). radiant.backend.lp_mpsl sweeps the same race with the
 * option on.
 *
 * Risk: an in-grant arm (a completion's follow-on from the software interrupt,
 * or an arm from a preemptible thread) runs gate_acquire() -> GATE_GRANTED ->
 * program_tx/rx/ed. The zero-latency signal can preempt it with ACTION_END
 * (the TIMER0 end, EXTEND_FAILED, the extension skip); the hand-back clears
 * SHORTS and the interrupt mask and the RADIO becomes the SoftDevice
 * Controller's or the 802.15.4 driver's. Resumed, the arm wrote MODE, PCNF,
 * the address, CRC, FREQUENCY, PACKETPTR and SHORTS (and INTENSET RXREADY for
 * ED) into that live RADIO, and enabled TIMER->RADIO connections that then
 * fired inside the other stack's event. EXTEND_FAILED and the skip also abort
 * from the signal, which delivered ABORTED to the claimed operation before the
 * arm returned its own result: two outcomes. And the gate's deferred DENIED
 * (gate_wq, cooperative) could land on the operation of a thread arm after its
 * claim.
 *
 * Swept over every point of a transmit, a receive and an energy-detect arm
 * where PRIMASK lets a zero-latency signal in (each outermost unlock of the
 * zero-latency lock or CLOCK_LOCK), for each signal kind, on time and too late
 * to make it (the span's own late check fails, so the signal can land while a
 * failed arm unwinds). Always: no RADIO write after the hand-back, no window
 * connection left on, exactly one outcome per operation - a refused arm and no
 * terminal, or an accepted arm and exactly one terminal (or a live operation
 * if nothing ended the loan).
 */

#include <zephyr/ztest.h>

#include <radiant/radiant_radio_hal.h>

#include "backend_probe.h"

ZTEST_SUITE(radiant_backend_arm_handback, NULL, NULL, NULL, NULL, NULL);

ZTEST(radiant_backend_arm_handback, test_arm_preempted_by_grant_end)
{
	static const char *const kn[] = { "tx", "rx", "ed" };
	static const char *const mn[] = { "end", "end+start", "abort+end", "denied" };

	for (int late = 0; late <= 1; late++) {
	for (int kind = BACKEND_ARM_TX; kind <= BACKEND_ARM_ED; kind++) {
	for (int mode = BACKEND_PRE_END; mode <= BACKEND_PRE_DENIED; mode++) {
		uint32_t points = 0u;
		uint32_t etimes = 0u;

		for (uint32_t n = 1u; n <= 16u; n++) {
			struct backend_preempt_result r;

			backend_preempt_arm(kind, mode, n, late != 0, &r);
			if (!r.fired) {
				/* Fewer unlock points than n: the arm finished first. */
				zassert_equal(r.rc, late ? RADIANT_RADIO_ETIME : RADIANT_RADIO_OK_RC,
					      "%s %s late %d: rc %d", kn[kind], mn[mode], late, r.rc);
				break;
			}
			points++;
			zassert_false(r.marked_at_end, "%s %s n %u: arming mark left set",
				      kn[kind], mn[mode], n);
			zassert_true(!late || r.rc != RADIANT_RADIO_OK_RC,
				     "%s %s n %u: a late arm accepted", kn[kind], mn[mode], n);
			if (mode == BACKEND_PRE_DENIED && !r.marked_at_fire) {
				/* The last point is the span's own unlock: the arm is
				 * over, its operation live, and a denial now is one for
				 * a live operation (the grant-ended-under-it case). One
				 * outcome still: accepted, then DENIED once. */
				zassert_equal(r.rc, RADIANT_RADIO_OK_RC, "%s %s n %u: rc %d",
					      kn[kind], mn[mode], n, r.rc);
				zassert_equal(r.terminals, 1u, "%s %s n %u: %u terminals",
					      kn[kind], mn[mode], n, r.terminals);
				zassert_true(r.op_idle_at_end, NULL);
				zassert_false(r.conns_at_end, NULL);
				continue;
			}
			if (mode == BACKEND_PRE_DENIED) {
				/* The loan goes on: the arm is its operation's only
				 * outcome, live (no terminal yet) or refused. */
				zassert_equal(r.terminals, 0u,
					      "%s %s n %u: DENIED landed on a live arm",
					      kn[kind], mn[mode], n);
				if (r.rc == RADIANT_RADIO_OK_RC) {
					zassert_false(r.op_idle_at_end,
						      "%s %s n %u: OK but the slot is free",
						      kn[kind], mn[mode], n);
				} else {
					zassert_equal(r.rc, RADIANT_RADIO_ETIME,
						      "%s %s n %u: rc %d", kn[kind], mn[mode], n, r.rc);
					zassert_true(r.op_idle_at_end, "%s %s n %u: slot not freed",
						     kn[kind], mn[mode], n);
					zassert_false(r.conns_at_end,
						      "%s %s n %u: a refused arm left a connection",
						      kn[kind], mn[mode], n);
					etimes++;
				}
				continue;
			}
			zassert_false(r.radio_written_after_handback,
				      "%s %s n %u: RADIO written after the hand-back",
				      kn[kind], mn[mode], n);
			zassert_false(r.conns_after_arm,
				      "%s %s n %u: a window connection outlived the hand-back",
				      kn[kind], mn[mode], n);
			zassert_false(r.conns_at_end, "%s %s n %u: a connection left on",
				      kn[kind], mn[mode], n);
			zassert_true(r.op_idle_at_end, "%s %s n %u: the slot never freed",
				     kn[kind], mn[mode], n);
			if (r.rc == RADIANT_RADIO_OK_RC) {
				/* Programmed before the signal: a live operation the
				 * loan's end cut - ABORTED from the signal, or DENIED
				 * from the gate's work, exactly once. */
				zassert_equal(r.terminals, 1u,
					      "%s %s n %u: OK and %u terminals", kn[kind],
					      mn[mode], n, r.terminals);
			} else {
				zassert_equal(r.rc, RADIANT_RADIO_ETIME, "%s %s n %u: rc %d",
					      kn[kind], mn[mode], n, r.rc);
				zassert_true(r.op_idle_after_arm, "%s %s n %u: slot not freed",
					     kn[kind], mn[mode], n);
				zassert_equal(r.terminals, 0u,
					      "%s %s n %u: ETIME and a terminal too",
					      kn[kind], mn[mode], n);
				etimes++;
			}
		}
		zassert_true(points > 0u, "%s %s late %d: no preemption point",
			     kn[kind], mn[mode], late);
		if (mode != BACKEND_PRE_DENIED || late) {
			zassert_true(etimes > 0u,
				     "%s %s late %d: no preemption point failed the arm",
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

	/* A point past the arm's span: the arm programmed, then the loan ended. */
	for (uint32_t n = 16u; n > 0u; n--) {
		backend_preempt_arm(BACKEND_ARM_RX, BACKEND_PRE_END, n, false, &r);
		if (r.fired) {
			break;
		}
	}
	zassert_true(r.fired, NULL);
	zassert_equal(r.rc, RADIANT_RADIO_OK_RC, "the last point is after the span");
	zassert_false(r.op_idle_after_arm, "live until the gate's denial");
	zassert_false(r.conns_after_arm, "a window connection survived the hand-back");
	zassert_equal(r.terminals, 1u, NULL);
}
