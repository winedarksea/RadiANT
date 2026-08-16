/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fake_swi.c - the software-interrupt trampoline, as a test instrument.
 *
 * The real one (radiant/src/radiant_swi.c) claims an NVIC line and lets the
 * hardware run the handler. There is no NVIC line to claim in this image and,
 * more to the point, the thing the suite needs to assert is not "an interrupt
 * fired" but "the gate ASKED FOR THE DEFERRAL INSTEAD OF CALLING THE KERNEL" -
 * which is a property of the call, not of the interrupt controller.
 *
 * So this dispatches synchronously from radiant_swi_pend(). That is a faithful
 * model of the real thing whenever the pending context is not itself masking
 * the line: the NVIC tail-chains straight into the handler. It is NOT faithful
 * to the case the trampoline exists for - a zero-latency interrupt pending it,
 * where the handler runs after the ZLI returns - and no fake can be, because
 * the whole point is a priority relationship the host image does not have.
 * What the suite can prove is the routing; that the routing lands one priority
 * down is proved by radiant_swi.c's BUILD_ASSERT and by the bench.
 */

#include <zephyr/kernel.h>

#include <radiant/radiant_swi.h>

#include "fake_swi.h"

#define FAKE_SWI_SUBS 4
static struct {
	uint32_t              bits;
	radiant_swi_handler_t fn;
} subs[FAKE_SWI_SUBS];
static uint8_t n_subs;

static struct fake_swi_counts counts;

int radiant_swi_register(uint32_t bits, radiant_swi_handler_t handler)
{
	if (handler == NULL || bits == 0u) {
		return -EINVAL;
	}
	if (n_subs >= FAKE_SWI_SUBS) {
		return -ENOMEM;
	}
	subs[n_subs].bits = bits;
	subs[n_subs].fn = handler;
	n_subs++;
	counts.registers++;
	return 0;
}

void radiant_swi_pend(uint32_t bits)
{
	counts.pends++;
	counts.bits_seen |= bits;
	if ((bits & RADIANT_SWI_GATE_REQUEST) != 0u) {
		counts.request++;
	}
	if ((bits & RADIANT_SWI_GATE_DENY) != 0u) {
		counts.deny++;
	}
	if ((bits & RADIANT_SWI_GATE_DEADLINE) != 0u) {
		counts.deadline++;
	}

	for (uint8_t i = 0; i < n_subs; i++) {
		uint32_t mine = bits & subs[i].bits;

		if (mine != 0u && subs[i].fn != NULL) {
			subs[i].fn(mine);
		}
	}
}

void fake_swi_reset(void)
{
	memset(&counts, 0, sizeof(counts));
	/* Subscriptions deliberately survive: gate_init() registers once and
	 * returns early on a second call, exactly as the session does. */
}

const struct fake_swi_counts *fake_swi(void)
{
	return &counts;
}
