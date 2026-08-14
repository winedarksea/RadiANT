/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf_gate_direct.c - the radio is ours, always.
 *
 * Provenance: clean-room. Nothing here derives from sdk-ant.
 *
 * Gate for builds where radiant owns the RADIO outright (every shipping
 * dongle image today): gate_acquire() always returns GRANTED, so the compiler
 * folds the branch and radiant_radio_nrf.c emits the same code it did before
 * this seam existed. That makes the no-op provable rather than resting only
 * on an A/B run - relevant because a t_sync regression here would move RX
 * yield by a few tenths of a percent, about the size of the bench's
 * characterised ~0.4% collision floor, i.e. easy to mistake for noise.
 *
 * radiant_nrf_gate_on_grant/_on_denied/_on_radio_irq are never called from
 * here - GATE_PENDING cannot occur and the RADIO IRQ reaches its handler via
 * the NVIC as normal - they exist only for the arbitrated gate.
 */

#include "radiant_radio_nrf_gate.h"

enum gate_rc gate_acquire(enum gate_op op, radiant_time_t t_from,
			  radiant_time_t t_to, uint16_t follow_on_us,
			  uint8_t prio)
{
	(void)op;
	(void)t_from;
	(void)t_to;
	/* A backend that owns the radio ignores follow_on_us by contract -
	 * nothing to reserve air against. */
	(void)follow_on_us;
	/* prio ranks our own requests when a gate has to choose; this one
	 * never does - at most one operation at a time, no other claimant. */
	(void)prio;

	return GATE_GRANTED;
}

void gate_release(void)
{
	/* Nothing was held. */
}

bool gate_extend(uint32_t us)
{
	(void)us;
	/* Always false: a window here runs to the close it was armed with -
	 * no arbiter to shorten it, so nothing to extend back from. */
	return false;
}

int gate_init(void)
{
	return RADIANT_RADIO_OK_RC;
}

void gate_shutdown(void)
{
}

uint32_t gate_min_arm_lead_us(void)
{
	/* Nobody to ask, so nothing to wait for - the hardware's own arm lead
	 * stands unchanged. */
	return 0u;
}

uint32_t gate_max_window_us(void)
{
	/* Unbounded: limited only by the 32-bit 1 MHz TIMER's ~71 min range. */
	return 0u;
}
