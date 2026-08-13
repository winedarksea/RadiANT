/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf_gate.h - who owns the RADIO right now.
 *
 * Provenance: clean-room. Written from nrfxlib's mpsl_timeslot.h and from the
 * existing structure of radiant_radio_nrf.c. Nothing here derives from sdk-ant.
 *
 * WHAT THIS IS. radiant_radio_nrf.c turns a struct radiant_rx_req into RADIO
 * registers, a TIMER compare and two (D)PPI connections. When a second
 * protocol stack is on the chip, exactly one thing changes: the radio is no
 * longer ours whenever we want it. That is this file - a seam INSIDE the
 * backend rather than a second backend beside it. Splitting into a core plus
 * two front ends was considered instead, but an A/B run can't prove it safe:
 * radiant_radio_hal.h notes a t_sync calibration error changes RX yield by a
 * few tenths of a percent, about the size of the bench's characterised ~0.4%
 * collision floor - too easy to mistake for noise. So instead the direct path
 * compiles to the same machine code as before (see radiant_radio_nrf_gate_direct.c),
 * and the A/B becomes a formality rather than the only evidence.
 *
 * THE ORDER OF EVENTS, WHICH IS THE WHOLE CONTRACT. An arm call has two
 * halves separated by this interface:
 *
 *   1. VALIDATE AND STAGE. Everything that is a pure function of the request
 *      (every EINVAL/ENOTSUP an arm call must answer synchronously) happens
 *      before the gate is consulted, touching no peripheral.
 *   2. gate_acquire(). Asks for the air.
 *      - GATE_GRANTED: programme now. Always true on the direct backend.
 *      - GATE_PENDING: a reservation was placed. Programme nothing yet;
 *        radiant_nrf_gate_on_grant() fires when the air arrives, or
 *        radiant_nrf_gate_on_denied() if it does not.
 *      - GATE_DENIED: the arm call fails RADIANT_RADIO_EDENIED immediately.
 *   3. PROGRAMME. RADIO configuration, TIMER compares and (D)PPI enables -
 *      all in one step, none of it before the gate says yes.
 *
 * WHY EVEN THE RADIO CONFIGURATION WAITS. MODE, FREQUENCY, PCNF0/1, BASE0/1,
 * PREFIX0/1 and PACKETPTR are the same registers the other stack's live
 * operation is using; writing them outside a grant corrupts their frame, not
 * ours. And the compare specifically can't be programmed early either: the
 * GPPI connection fires TASKS_RXEN regardless of ownership, so an armed
 * compare on a request that then gets BLOCKED enables the receiver inside
 * another stack's event - no error anywhere, just a lost packet in a
 * protocol nobody is watching.
 *
 * WHAT IS DELIBERATELY ABSENT. No MPSL type or symbol appears above the
 * backend or in this file, which keeps the TI CC26xx path open (its
 * equivalent is RF-driver command scheduling, not a timeslot API - this
 * interface just asks "may I have the air between these two instants").
 * No queue either: at most one operation exists at a time, and MPSL answers
 * -NRF_EAGAIN to a second request while a session is not IDLE - the mapping
 * is 1:1.
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
	 * arm call still returns success, and exactly one of
	 * radiant_nrf_gate_on_grant() / radiant_nrf_gate_on_denied() follows.
	 *
	 * That's deliberate: min_arm_lead_us is a GUARANTEE (arming inside it
	 * fails ETIME rather than running late) while grant latency has an
	 * unbounded tail, and radiant_burst.c requires min_arm_lead_us to stay
	 * below 1310us to allow acknowledged data. So arbitration uncertainty
	 * reaches the core as DENIAL, never as lead.
	 */
	GATE_PENDING,
	/*
	 * No air, and none coming, for an otherwise-reachable instant. The arm
	 * call fails RADIANT_RADIO_EDENIED synchronously rather than only as an
	 * event, because radiant_transfer.c arms the acknowledged-data reply
	 * from INSIDE the receive callback, 1.56ms before the frame must be on
	 * the air - too late to place a request and hear back, and no retry
	 * exists.
	 */
	GATE_DENIED,
};

enum gate_op {
	GATE_OP_RX = 0,
	GATE_OP_TX,
	GATE_OP_ED,
};

/*
 * Ask for the air between t_from and t_to, inclusive of everything the
 * backend has to do at either edge.
 *
 * t_from is already the instant the hardware must be programmed by (caller
 * has subtracted ramp-up and address airtime), so a gate adds only its own
 * margins, at both ends:
 *
 *   HEAD. Under a timeslot the compare and (D)PPI can't be touched until the
 *   grant starts, so the grant must start before t_from by
 *   ARM_SETUP_US + RAMP_UP_US + AIR_LEAD_WORST_US.
 *
 *   TAIL. The dangerous half - overstaying ASSERTS rather than losing a
 *   window - and can't be a fixed number: MPSL schedules on LFCLK/GRTC while
 *   this backend's close runs on an HFCLK-derived 1MHz TIMER, so the tail
 *   must cover RADIO_DISABLE_MAX_US plus clock error accumulated over the
 *   whole grant. Get this wrong and the field failure is a dongle asserting
 *   once an hour under a busy Thread network.
 *
 * follow_on_us is struct radiant_rx_req::follow_on_us verbatim - air the
 * core may still need for one follow-on operation after this one ends. A
 * gate that reserves it MUST release it as soon as it knows the follow-on
 * isn't coming (see gate_release) - the difference between costing the other
 * stack's SCHEDULER and costing its AIR.
 *
 * prio is RADIANT_GATE_PRIO_*. It ranks our own requests against each other
 * and nothing else - do not add levels believing they outrank another stack.
 */
#define RADIANT_GATE_PRIO_NORMAL 0u  /* scans, energy detect - the elastic work */
#define RADIANT_GATE_PRIO_HIGH   1u  /* tracked slots and transmits */

enum gate_rc gate_acquire(enum gate_op op, radiant_time_t t_from,
			  radiant_time_t t_to, uint16_t follow_on_us,
			  uint8_t prio);

/*
 * This operation is over; give back anything still held.
 *
 * Called from the terminal path of every operation, including an aborted
 * one, and REQUIRED to be called promptly, not merely eventually - on an
 * empty tracked window the follow_on reserve is ~2ms of the other stack's
 * schedule that we've just stopped needing, four times a second per sensor.
 *
 * Idempotent and safe to call when nothing is held (the abort path does so
 * without knowing).
 */
void gate_release(void);

/*
 * Try to grow the current grant by us microseconds. False if the arbiter
 * refused - an extension is granted only when nothing else is scheduled in
 * the extended region, which is the arbiter enforcing "the sweep is the
 * elastic consumer" (ADR 0013) rather than a constant guessing at it.
 *
 * A refusal leaves the grant live, so the caller closes its window cleanly
 * and reports RADIANT_RADIO_STATUS_ABORTED, which search accounting already
 * treats as "credit what was actually listened to". This is NOT a denial -
 * the two signals must stay distinct.
 *
 * Always false on the direct gate: the radio was never anyone else's, so no
 * window is ever cut short.
 */
bool gate_extend(uint32_t us);

/* Lifecycle, mirroring radiant_radio_init() / _disable(). */
int  gate_init(void);
void gate_shutdown(void);

/*
 * Longest single operation this gate can cover, in microseconds, or 0 for
 * unbounded. Reported as caps.max_window_us, so the scheduler bounds its
 * REQUEST by it instead of the gate silently shortening a grant.
 */
uint32_t gate_max_window_us(void);

/*
 * How far ahead of `now` a NEW reservation must be placed, in microseconds,
 * or 0 for "the hardware's own arm lead and nothing more". Reported as
 * caps.min_arm_lead_us so radiant_sched.c's arming pass leaves room for the
 * gate to ask and be answered. 0 on the direct gate (nobody to ask).
 *
 * Does NOT apply to an operation following one already in flight: the
 * acknowledged-data reply is armed inside the receive callback into air
 * follow_on_us already reserved, so it needs no reservation and is bound by
 * caps.min_arm_lead_in_grant_us instead. Conflating the two would refuse
 * acknowledged data on every arbitrated build - see radiant_burst.c.
 */
uint32_t gate_min_arm_lead_us(void);

/* ---------------------------------------------------------------------------
 * The other direction: what a gate calls back into radiant_radio_nrf.c.
 *
 * All three are safe to call from an interrupt or a cooperative thread.
 * radiant_nrf_gate_on_denied() is why radiant_radio_hal.h's callback-context
 * guarantee was amended: MPSL delivers BLOCKED/CANCELLED from
 * mpsl_low_priority_process(), so a DENIED terminal is the first HAL event
 * that can reach the core outside the radio interrupt.
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
 * must return promptly, so on failure it only records the fact; the gate
 * calls this once the timeslot has been handed back. No-op when nothing
 * failed.
 */
void radiant_nrf_gate_finish_failed(void);

/*
 * THE RADIO INTERRUPT, DELIVERED BY THE ARBITER INSTEAD OF BY THE NVIC.
 *
 * radiant_radio_nrf.c connects its handler to RADIO_0_IRQn, but inside an
 * MPSL timeslot that vector is MPSL's: the RADIO interrupt arrives at the
 * timeslot signal callback as MPSL_TIMESLOT_SIGNAL_RADIO and the connected
 * handler is never entered. Without this call a receive window under the
 * MPSL gate arms correctly, runs its full length, and then never ends - END
 * and DISABLED both fire with nothing listening.
 *
 * No-op in the direct build, where the NVIC delivers straight to the
 * connected handler and this is never called.
 */
void radiant_nrf_gate_on_radio_irq(void);

/*
 * THE GRANT IS OVER. GIVE THE PERIPHERAL BACK AS IT WAS LENT.
 *
 * Called on every path that ends a timeslot, before ACTION_END is returned,
 * including paths with no terminal event, because the peripheral must be
 * handed back regardless. Clears RADIO->INTENSET: on a direct build the
 * interrupt mask is set once at enable and left forever since the RADIO is
 * always ours, but under arbitration those bits staying set while the OTHER
 * stack transmits raises interrupts nobody scheduled - fatal to the
 * SoftDevice Controller, which asserts within milliseconds of the first
 * advertising event.
 *
 * No-op in the direct build, where no grant ever ends.
 */
void radiant_nrf_gate_on_grant_end(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RADIO_NRF_GATE_H_ */
