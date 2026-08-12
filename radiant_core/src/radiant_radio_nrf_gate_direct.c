/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_nrf_gate_direct.c - the radio is ours, always.
 *
 * Provenance: clean-room. Nothing here derives from sdk-ant.
 *
 * The gate for a build where radiant_core owns the RADIO outright, which is
 * every shipping dongle image today. Every question has the same answer and
 * the answer is a constant, so the whole file is thirty lines and an argument.
 *
 * THE ARGUMENT IS THE POINT OF THE FILE. radiant_radio_nrf.c is unchanged in
 * behaviour by the seam it now goes through: gate_acquire() returns GRANTED
 * unconditionally, so the compiler folds the branch and the direct path emits
 * the same instructions it emitted before, in the same order. That is what
 * makes the A/B gate for P2' a formality rather than the only evidence - which
 * matters here more than usual, because radiant_radio_hal.h's own analysis says
 * a t_sync calibration regression moves RX yield by a few tenths of a percent,
 * "at the same order as the bench's characterised ~0.4 % collision floor, which
 * is precisely the range in which a real regression is easiest to mistake for
 * noise". An A/B run cannot see the failure this refactor is most likely to
 * cause. A file that provably changes nothing can.
 *
 * radiant_nrf_gate_on_grant() and radiant_nrf_gate_on_denied() are never called
 * from here: GATE_PENDING is a state this gate cannot enter, so there is
 * nothing to call them from. They exist for the arbitrated gate and are
 * referenced by no path in this build.
 */

#include "radiant_radio_nrf_gate.h"

enum gate_rc gate_acquire(enum gate_op op, radiant_time_t t_from,
			  radiant_time_t t_to, uint16_t follow_on_us,
			  uint8_t prio)
{
	(void)op;
	(void)t_from;
	(void)t_to;
	/*
	 * follow_on_us is READ AND DISCARDED, and that is the field's stated
	 * contract rather than an omission: "a backend that owns the radio
	 * ignores it". There is nothing to reserve air against, so reserving it
	 * would be an accounting entry with no counterparty.
	 */
	(void)follow_on_us;
	/*
	 * And prio likewise. It ranks our own requests against each other in a
	 * gate that has to choose; this one never has to choose, because there
	 * is at most one operation at a time and no other claimant.
	 */
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
	/*
	 * FALSE, AND IT IS NEVER ASKED. A window on this gate runs to the close
	 * it was armed with; there is no arbiter to shorten it and therefore
	 * nothing to grow it back from. Returning true would be the more
	 * "capable"-looking answer and would be a lie about a window that was
	 * never cut short in the first place.
	 */
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
	/* Nobody to ask, so nothing to wait for: the hardware's own arm lead
	 * stands unchanged, which is what every bench result to date was
	 * measured against. */
	return 0u;
}

uint32_t gate_max_window_us(void)
{
	/*
	 * Unbounded. A receive window on this backend is limited by the 32-bit
	 * 1 MHz TIMER's own range, which is ~71 minutes, and by nothing else -
	 * so there is no ceiling worth advertising and the scheduler's
	 * window_cap() folds away to one comparison against zero.
	 */
	return 0u;
}
