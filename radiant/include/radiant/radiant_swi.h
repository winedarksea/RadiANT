/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_swi.h - the one legal way out of a zero-latency interrupt.
 *
 * WHY THIS EXISTS (bug 27, docs/p4-zli-kernel-calls.md).
 *
 * With the MPSL gate, radiant's radio work runs in MPSL's timeslot signal
 * callback, and MPSL connects its TIMER0/RADIO/RTC0 vectors with
 * IRQ_ZERO_LATENCY (nrf/subsys/mpsl/init/mpsl_init.c:187) whenever
 * CONFIG_ZERO_LATENCY_IRQS is on - which the SoftDevice Controller selects, so
 * any Matter build has it. A zero-latency interrupt runs ABOVE the priority
 * that irq_lock() and every Zephyr spinlock mask; that is the whole point of
 * it. So no kernel API may be called from there: k_work_submit() takes the work
 * queue's spinlock, k_sem_give() manipulates the ready queue and, when the
 * waiter has a timeout, the kernel timeout list.
 *
 * Measured consequences before this existed: `ASSERTION FAIL @ spinlock.h:132`
 * out of z_work_submit_to_queue(), a silent RESET_CPU_LOCKUP reboot loop on the
 * Matter arm (13 boots/210 s), and sys_dlist_insert/sys_dlist_remove bus faults
 * on the bridge arm.
 *
 * THE SHAPE. The zero-latency side does two things that cannot fail and cannot
 * take a lock: an atomic OR into a word, and NVIC_SetPendingIRQ() on a line
 * that has no peripheral behind it. The NVIC then runs that line's handler at
 * an ordinary Zephyr priority - where irq_lock() DOES mask it - and the kernel
 * calls happen there.
 *
 * This is the pattern both neighbouring stacks already use: MPSL takes SWI00
 * for its own low-priority processing (CONFIG_MPSL_LOW_PRIO_IRQN=28 on nRF54L)
 * and nrf_802154 takes EGU10 for its notifications (NRF_802154_EGU_INSTANCE_NO).
 * radiant takes a different line again, and radiant_swi.c asserts that it is
 * different rather than trusting this comment.
 *
 * NO WAKEUP CAN BE DROPPED, which matters because a dropped one presents as a
 * silently stalled ANT channel rather than a crash. radiant_swi_pend() ORs its
 * bits in and THEN pends the line; the handler clears the whole word in one
 * atomic before dispatching. A ZLI landing before the clear has its bits
 * handled by the run in progress and leaves the NVIC line pending, costing one
 * extra handler entry with nothing to do; a ZLI landing after the clear leaves
 * both the bits and the NVIC pending bit set, so the handler runs again. Both
 * orders deliver.
 */

#ifndef RADIANT_SWI_H_
#define RADIANT_SWI_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The work a zero-latency context can ask for. One bit per producer, because
 * the handler must be able to tell which of them asked without a lock.
 */
/* radiant_event_wakeup(): give the API event semaphore. THE ONE THAT MATTERS -
 * it is on the ANT completion path (SIGNAL_RADIO -> deliver_terminal() ->
 * radiant_event_post_rx()), and the API event thread waits on that semaphore
 * with a timeout, so giving it from a ZLI edits the kernel timeout list. A
 * trampoline that covered the gate but not this one was MEASURED to make the
 * fault rate worse, not better. */
#define RADIANT_SWI_EVENT_WAKEUP  (1U << 0)
/* The MPSL gate: place the staged reservation (request_work). */
#define RADIANT_SWI_GATE_REQUEST  (1U << 1)
/* The MPSL gate: tell the core it did not get the air (denied_work). */
#define RADIANT_SWI_GATE_DENY     (1U << 2)
/* The MPSL gate: arm or disarm its backstop deadline. */
#define RADIANT_SWI_GATE_DEADLINE (1U << 3)
/* The backend: hand a completed operation to the core. The (a) split - the core
 * state machine must not run in a zero-latency context, because the ANT API
 * excludes itself from it with a mutex and a mutex cannot do that. See the long
 * note above core_cb_rx() in radiant_radio_nrf.c. */
#define RADIANT_SWI_CORE_CB       (1U << 4)

/* Called from the trampoline's own handler, at ordinary interrupt priority.
 * Kernel APIs are legal here; blocking is not. `bits` carries only the bits
 * this subscriber registered for. */
typedef void (*radiant_swi_handler_t)(uint32_t bits);

/*
 * Claim the interrupt line and take the first subscription. Idempotent on the
 * line; call once per subscriber, from thread context, before any producer can
 * pend. Returns 0, or a negative errno if the subscriber table is full.
 */
int radiant_swi_register(uint32_t bits, radiant_swi_handler_t handler);

/*
 * Ask for `bits` of work. SAFE FROM ANY CONTEXT INCLUDING A ZERO-LATENCY
 * INTERRUPT - an atomic OR and an NVIC write, no kernel object, no allocation,
 * nothing that can take a lock. Called from thread context it simply runs the
 * handler as soon as the NVIC gets round to it, which is immediately.
 */
void radiant_swi_pend(uint32_t bits);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_SWI_H_ */
