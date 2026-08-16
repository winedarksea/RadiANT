/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_swi.c - the software-interrupt trampoline. See radiant_swi.h for why
 * it exists; this file is only about picking the line and proving it is ours.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/irq.h>
#include <zephyr/logging/log.h>

#include <radiant/radiant_swi.h>

LOG_MODULE_REGISTER(radiant_swi, CONFIG_RADIANT_LOG_LEVEL);

#define RADIANT_SWI_IRQN CONFIG_RADIANT_SWI_IRQN

/*
 * THE LINE MUST NOT BE SOMEONE ELSE'S, and this is checked rather than
 * asserted in prose. Two neighbours on this SoC take a software line each:
 *
 *   MPSL       SWI00, i.e. IRQ 28 on nRF54L (CONFIG_MPSL_LOW_PRIO_IRQN).
 *   nrf_802154 EGU10, i.e. IRQ 135 on nRF54L (NRF_802154_EGU_INSTANCE_NO=10).
 *
 * The MPSL one is a Kconfig symbol, so it is compared directly. Anything that
 * claims a line through Zephyr's own IRQ_CONNECT() collides with the
 * IRQ_CONNECT() below at BUILD time - gen_isr_tables refuses two registrations
 * for one table index - which covers every in-tree driver. MPSL is the case
 * that would NOT be caught that way, because it connects its vectors
 * dynamically, hence the explicit test.
 */
#ifdef CONFIG_MPSL_LOW_PRIO_IRQN
BUILD_ASSERT(RADIANT_SWI_IRQN != CONFIG_MPSL_LOW_PRIO_IRQN,
	     "radiant's trampoline has been pointed at the same interrupt line "
	     "MPSL uses for its own low-priority processing. Both would run "
	     "from one vector and each would see the other's work as spurious; "
	     "pick another SWI line in CONFIG_RADIANT_SWI_IRQN.");
#endif

/*
 * Bits asked for and not yet dispatched. atomic_t because a zero-latency
 * interrupt can land between any two instructions of the handler, and only
 * LDREX/STREX is correct against that - a plain |= would lose a bit whenever
 * the ZLI hit between the load and the store, and a lost bit is a stalled ANT
 * channel with nothing in any log to say so.
 */
static atomic_t swi_pending;

#define RADIANT_SWI_SUBS 4
static struct {
	uint32_t              bits;
	radiant_swi_handler_t fn;
} subs[RADIANT_SWI_SUBS];
static uint8_t n_subs;
static bool    line_claimed;

static void radiant_swi_isr(const void *arg)
{
	uint32_t bits;

	ARG_UNUSED(arg);

	/*
	 * Read and clear in one atomic. Everything raised before this line is
	 * ours to dispatch; anything raised after it re-pends the NVIC line and
	 * is dispatched by the next entry. See the header for both orderings.
	 */
	bits = (uint32_t)atomic_and(&swi_pending, 0);
	if (bits == 0u) {
		/* A ZLI raised its bits just before this handler's clear, so
		 * they were dispatched by the run in progress and only the NVIC
		 * pending flag was left over. Nothing to do, and not an error. */
		return;
	}

	for (uint8_t i = 0; i < n_subs; i++) {
		uint32_t mine = bits & subs[i].bits;

		if (mine != 0u && subs[i].fn != NULL) {
			subs[i].fn(mine);
		}
	}
}

int radiant_swi_register(uint32_t bits, radiant_swi_handler_t handler)
{
	if (handler == NULL || bits == 0u) {
		return -EINVAL;
	}
	if (n_subs >= RADIANT_SWI_SUBS) {
		return -ENOMEM;
	}

	if (!line_claimed) {
		/*
		 * Connected STATICALLY, not with irq_connect_dynamic(): a
		 * static connection is what puts an entry in the generated ISR
		 * table, and that table is what makes a second claimant on this
		 * line a build error instead of a bug that only shows up as
		 * one stack silently eating the other's interrupts.
		 *
		 * The priority is an ordinary Zephyr one, which is the entire
		 * point - irq_lock() and every k_spin_lock() mask it, so the
		 * kernel calls the subscribers make here are legal. It must
		 * never be moved into the zero-latency band.
		 */
		IRQ_CONNECT(RADIANT_SWI_IRQN, CONFIG_RADIANT_SWI_IRQ_PRIO,
			    radiant_swi_isr, NULL, 0);
		/*
		 * Enabled after the subscriber is recorded below would be a
		 * race on a line something else might already be pending;
		 * enabled here it is safe because nothing pends before the
		 * first register() returns (the producers are all downstream of
		 * radio init).
		 */
		irq_enable(RADIANT_SWI_IRQN);
		line_claimed = true;
		LOG_INF("swi: trampoline on IRQ %d at priority %d",
			(int)RADIANT_SWI_IRQN,
			(int)CONFIG_RADIANT_SWI_IRQ_PRIO);
	}

	subs[n_subs].bits = bits;
	subs[n_subs].fn = handler;
	n_subs++;
	return 0;
}

void radiant_swi_pend(uint32_t bits)
{
	if (!line_claimed) {
		/* Nothing has registered yet, so there is nobody to run the
		 * work and the NVIC would just latch a pending bit forever.
		 * Dropping is right here and cannot hide a fault: the only
		 * window is before radio init has finished. */
		return;
	}
	/*
	 * ORDER MATTERS. Bits first, then the NVIC. The other way round, the
	 * handler can run between the two and find nothing, and the bits then
	 * sit in swi_pending with no interrupt coming for them.
	 */
	(void)atomic_or(&swi_pending, (atomic_val_t)bits);
	NVIC_SetPendingIRQ((IRQn_Type)RADIANT_SWI_IRQN);
}
