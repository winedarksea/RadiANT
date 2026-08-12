/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf_gate.h - who owns the RADIO right now.
 *
 * Provenance: clean-room. Written from nrfxlib's mpsl_timeslot.h and from the
 * existing structure of radiant_radio_nrf.c. Nothing here derives from sdk-ant.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS IS, AND WHY IT IS NOT A SECOND BACKEND
 * ---------------------------------------------------------------------------
 *
 * radiant_radio_nrf.c is 2800 lines that know how to turn a struct
 * radiant_rx_req into RADIO registers, a TIMER compare and two (D)PPI
 * connections. Exactly one thing changes when a second protocol stack is on the
 * chip: it is no longer true that the radio is ours whenever we want it. That
 * one fact is this file, and it is a seam INSIDE the backend rather than a
 * second backend beside it.
 *
 * THE ALTERNATIVE WAS PROPOSED AND ITS OWN GATE CANNOT PROVE IT. Splitting the
 * file into a core plus two front ends would be verified by an A/B run - and
 * radiant_radio_hal.h states at length that a t_sync calibration error changes
 * RX yield "by a few tenths of a percent, at the same order as the bench's
 * characterised ~0.4 % collision floor, which is precisely the range in which a
 * real regression is easiest to mistake for noise." By the project's own
 * written analysis, an A/B gate cannot detect the most likely failure of that
 * refactor. So the file is not split: the direct path below compiles to the
 * same machine code it did before, modulo three inlined functions that return
 * constants, and the A/B becomes a formality rather than the only evidence.
 *
 * ---------------------------------------------------------------------------
 * THE ORDER OF EVENTS, WHICH IS THE WHOLE CONTRACT
 * ---------------------------------------------------------------------------
 *
 * An arm call now has two halves separated by this interface:
 *
 *   1. VALIDATE AND STAGE. Everything that is a pure function of the request -
 *      every EINVAL and every ENOTSUP radiant_radio_hal.h requires an arm call
 *      to answer synchronously - happens before the gate is consulted, and
 *      touches no peripheral.
 *   2. gate_acquire(). Asks for the air.
 *      - GATE_GRANTED: programme now. This is the direct backend, always.
 *      - GATE_PENDING: a reservation was placed. Programme nothing yet;
 *        radiant_nrf_gate_on_grant() will be called when the air arrives, and
 *        radiant_nrf_gate_on_denied() if it does not.
 *      - GATE_DENIED: the arm call fails RADIANT_RADIO_EDENIED, immediately.
 *   3. PROGRAMME. RADIO configuration, the TIMER compares and the (D)PPI
 *      enables - all of it, in one step, and none of it before the gate said
 *      yes.
 *
 * WHY EVEN THE RADIO CONFIGURATION WAITS. It would be easy to defer only the
 * compare and the (D)PPI enable, on the argument that those are what actually
 * fire TASKS_RXEN. That is wrong and quietly so: MODE, FREQUENCY, PCNF0/1,
 * BASE0/1, PREFIX0/1 and PACKETPTR are the same registers the other stack's
 * live operation is using. Writing them from outside a grant does not delay our
 * frame, it corrupts theirs - and the symptom is loss on a protocol whose stack
 * we are not debugging.
 *
 * WHY THE COMPARE IN PARTICULAR CANNOT BE PROGRAMMED EARLY. The GPPI connection
 * fires TASKS_RXEN whether or not anyone else owns the radio. A request that is
 * then BLOCKED, with the compare already armed, enables the receiver INSIDE
 * another stack's event. There is no error anywhere; there is a lost packet in
 * a protocol nobody is looking at.
 *
 * ---------------------------------------------------------------------------
 * WHAT IS DELIBERATELY ABSENT
 * ---------------------------------------------------------------------------
 *
 * NO MPSL TYPE OR SYMBOL APPEARS ABOVE THE BACKEND, and none appears in this
 * file either. That is what keeps the TI CC26xx path open: its equivalent is
 * the RF driver's multi-protocol command scheduling rather than a timeslot API,
 * and this interface is expressible in both because it says "may I have the air
 * between these two instants" and nothing about how.
 *
 * NO QUEUE. At most one operation exists at a time (radiant_radio_hal.h), and
 * MPSL answers -NRF_EAGAIN to a second request while a session is not IDLE. The
 * two constraints are the same constraint, so the mapping is 1:1 and there is
 * nothing to invent.
 */

#ifndef RADIANT_RADIO_NRF_GATE_H_
#define RADIANT_RADIO_NRF_GATE_H_

#include <stdbool.h>
#include <stdint.h>

#include <radiant_core/radiant_radio_hal.h>

#ifdef __cplusplus
extern "C" {
#endif

enum gate_rc {
	/* The air is ours from now until at least t_to. Programme immediately.
	 * The direct gate returns this and nothing else. */
	GATE_GRANTED = 0,
	/*
	 * A reservation has been placed and its outcome is not yet known. The
	 * arm call returns success - the operation exists, from the core's point
	 * of view - and exactly one of radiant_nrf_gate_on_grant() or
	 * radiant_nrf_gate_on_denied() follows.
	 *
	 * THE ARM RETURNS SUCCESS AND THAT IS THE DESIGN RATHER THAN A
	 * COMPROMISE. min_arm_lead_us is a GUARANTEE - arming inside it fails
	 * ETIME rather than running late - while grant latency is a
	 * distribution with an unbounded tail, and radiant_burst.c refuses
	 * acknowledged data at configuration time unless min_arm_lead_us stays
	 * below 1310 us. The two quantities are different in kind and making
	 * the number bigger cannot reconcile them. So arbitration uncertainty
	 * reaches the core as DENIAL, never as lead.
	 */
	GATE_PENDING,
	/*
	 * No air, and none coming, for an operation whose instant is otherwise
	 * perfectly reachable. The arm call fails RADIANT_RADIO_EDENIED.
	 *
	 * The case that makes this a synchronous answer rather than only an
	 * event: radiant_transfer.c arms the acknowledged-data reply by calling
	 * radiant_radio_tx() from INSIDE the receive callback, 1.56 ms before
	 * the frame must be on the air. There is no time to place a request,
	 * hear back and recover, and no retry exists. A gate that knows at that
	 * instant that the reply is not covered must say so from the call.
	 */
	GATE_DENIED,
};

enum gate_op {
	GATE_OP_RX = 0,
	GATE_OP_TX,
	GATE_OP_ED,
};

/*
 * Ask for the air between t_from and t_to, inclusive of everything the backend
 * has to do at either edge.
 *
 * t_from is already the instant the hardware must be programmed by - the
 * caller has subtracted ramp-up and address airtime - so a gate adds only its
 * own margins to it, and adds them at both ends:
 *
 *   HEAD. Under a timeslot the compare and the (D)PPI cannot be touched until
 *   the grant starts, so the grant has to start before t_from by
 *   ARM_SETUP_US + RAMP_UP_US + AIR_LEAD_WORST_US.
 *
 *   TAIL. The dangerous half, because overstaying ASSERTS rather than losing a
 *   window. It cannot be a fixed number: MPSL schedules on LFCLK/GRTC while
 *   this backend's close runs on an HFCLK-derived 1 MHz TIMER, so the tail must
 *   cover RADIO_DISABLE_MAX_US plus the relative clock error accumulated over
 *   the whole grant. Deriving it rather than guessing is the single most likely
 *   field failure of the arbitrated build: a dongle that asserts once an hour
 *   under a busy Thread network.
 *
 * follow_on_us is struct radiant_rx_req::follow_on_us verbatim - air the core
 * may still need for one follow-on operation after this one ends. A gate that
 * reserves it MUST release it the moment it knows the follow-on is not coming
 * (see gate_release), because that is the difference between the reserve
 * costing the other stack's SCHEDULER and costing the other stack's AIR.
 *
 * prio is RADIANT_GATE_PRIO_*. It ranks our own requests against each other and
 * nothing else - see finding 1 of the multiprotocol plan, and do not add levels
 * to it in the belief that they outrank another stack.
 */
#define RADIANT_GATE_PRIO_NORMAL 0u  /* scans, energy detect - the elastic work */
#define RADIANT_GATE_PRIO_HIGH   1u  /* tracked slots and transmits */

enum gate_rc gate_acquire(enum gate_op op, radiant_time_t t_from,
			  radiant_time_t t_to, uint16_t follow_on_us,
			  uint8_t prio);

/*
 * This operation is over; give back anything still held.
 *
 * Called from the terminal path of every operation, including an aborted one,
 * and REQUIRED to be called promptly rather than merely eventually. On an empty
 * tracked window the follow_on reserve is ~2 ms of the other stack's schedule
 * that we have just stopped needing, four times a second per sensor.
 *
 * Idempotent, and safe to call when nothing is held - the abort path calls it
 * without knowing.
 */
void gate_release(void);

/*
 * Try to grow the current grant by us microseconds. False if the arbiter
 * refused, which is a YIELD POINT DISCOVERED RATHER THAN CONFIGURED: an
 * extension is granted only when nothing else is scheduled in the extended
 * region, which is exactly the definition of "the sweep is the elastic
 * consumer" (ADR 0013) enforced by the arbiter instead of guessed at by a
 * constant.
 *
 * A refusal arrives with the grant still live, so the caller closes its window
 * cleanly and reports RADIANT_RADIO_STATUS_ABORTED - which the search
 * accounting already handles as "credit what was actually listened to". It is
 * NOT a denial: denial and elasticity are two different signals and neither may
 * be expressed as the other.
 *
 * Always false on the direct gate, where the radio was never anyone else's and
 * no window is ever cut short.
 */
bool gate_extend(uint32_t us);

/* Lifecycle, mirroring radiant_radio_init() / _disable(). */
int  gate_init(void);
void gate_shutdown(void);

/*
 * The longest single operation this gate can cover, in microseconds, or 0 for
 * unbounded. Reported to the core as caps.max_window_us, where the scheduler
 * bounds its REQUEST by it rather than the gate silently shortening a grant -
 * see that field for why the difference is a measured sweep bug rather than a
 * matter of taste.
 */
uint32_t gate_max_window_us(void);

/*
 * How far ahead of `now` a NEW reservation has to be placed, in microseconds,
 * or 0 for "the hardware's own arm lead and nothing more".
 *
 * Reported to the core as caps.min_arm_lead_us, so that radiant_sched.c's
 * arming pass leaves enough room for the gate to ask and be answered. 0 on the
 * direct gate, where there is nobody to ask.
 *
 * IT DOES NOT APPLY TO AN OPERATION THAT FOLLOWS ONE ALREADY IN FLIGHT. The
 * acknowledged-data reply is armed from inside the receive callback into air
 * that follow_on_us already reserved, so it needs no reservation of its own and
 * is bound by caps.min_arm_lead_in_grant_us instead. Conflating the two would
 * refuse acknowledged data on every arbitrated build - see radiant_burst.c.
 */
uint32_t gate_min_arm_lead_us(void);

/* ---------------------------------------------------------------------------
 * The other direction: what a gate calls back into radiant_radio_nrf.c.
 *
 * All three are safe to call from an interrupt or from a cooperative thread.
 * The second one is why radiant_radio_hal.h's callback-context guarantee was
 * amended: MPSL delivers BLOCKED and CANCELLED from mpsl_low_priority_process(),
 * so a DENIED terminal is the first HAL event that can reach the core outside
 * the radio interrupt.
 * ---------------------------------------------------------------------------
 */

/* The air arrived. Programme the staged operation and let the hardware run. */
void radiant_nrf_gate_on_grant(void);

/* The air did not arrive and will not. Deliver the staged operation's terminal
 * event with RADIANT_RADIO_STATUS_DENIED and free the slot. */
void radiant_nrf_gate_on_denied(void);

/*
 * Finish a grant whose programming failed, from THREAD context.
 *
 * radiant_nrf_gate_on_grant() runs inside the grant's own signal callback and
 * must return promptly - Nordic's own timeslot sample does almost nothing
 * there. So when programming fails it only records the fact, and the gate calls
 * this once the timeslot has been handed back. A no-op when nothing failed.
 */
void radiant_nrf_gate_finish_failed(void);

/*
 * THE RADIO INTERRUPT, DELIVERED BY THE ARBITER INSTEAD OF BY THE NVIC.
 *
 * This is the one thing about an arbitrated backend that is not visible
 * anywhere in the arm path, and it is the difference between a window that
 * completes and a window that does not.
 *
 * radiant_radio_nrf.c connects its handler to RADIO_0_IRQn and every terminal
 * event in the file comes from there. Inside an MPSL timeslot that vector is
 * MPSL's: the RADIO interrupt is delivered to the timeslot signal callback as
 * MPSL_TIMESLOT_SIGNAL_RADIO, and the connected handler is never entered. So a
 * receive window under the MPSL gate armed correctly, ran for its full length,
 * and then simply never ended - END and DISABLED both fired, and nothing was
 * listening.
 *
 * WHAT IT LOOKED LIKE, because it looked like several other things first: the
 * scheduler credited the full dwell (scan_us 94 200 us a chunk) and recorded
 * NO terminal at all - end ok=0 abrt=0 fail=0 - with every window's only
 * terminal arriving later from the gate as DENIED. Read as a denial problem it
 * is insoluble; the arbiter had refused nothing.
 *
 * A no-op in the direct build, where the NVIC delivers the interrupt to the
 * handler that was connected to it and this is never called.
 */
void radiant_nrf_gate_on_radio_irq(void);

/*
 * THE GRANT IS OVER. GIVE THE PERIPHERAL BACK AS IT WAS LENT.
 *
 * Called on every path that ends a timeslot, before ACTION_END is returned -
 * including the paths where no terminal event was delivered, because the
 * peripheral has to be handed back whether or not an operation was using it.
 *
 * What it actually does is clear RADIO->INTENSET, and that is not
 * housekeeping. On a direct build the interrupt mask can be set once at enable
 * and left for ever, because the RADIO is ours for ever. Under arbitration
 * those same bits stay set while the OTHER stack transmits on the same
 * peripheral, and its events then raise interrupts nobody scheduled. The
 * SoftDevice Controller does not survive it - "SoftDevice Controller ASSERT:
 * 48, 1792", about five milliseconds after the first advertising event, on
 * every boot. ANT+ alone never sees it, because with nothing else on the radio
 * there is nothing else to raise the events, which is why it survived every
 * measurement taken before an advertiser existed.
 *
 * A no-op in the direct build, where no grant ever ends.
 */
void radiant_nrf_gate_on_grant_end(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RADIO_NRF_GATE_H_ */
