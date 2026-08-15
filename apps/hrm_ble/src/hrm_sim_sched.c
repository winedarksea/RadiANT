/* SPDX-License-Identifier: Apache-2.0 */

#include "hrm_sim_sched.h"

uint32_t hrm_sim_sched_tick(struct hrm_sim_sched *s, uint32_t tick_ms,
			    uint32_t interval_ms)
{
	s->elapsed_ms += tick_ms;

	if (s->elapsed_ms < s->next_beat_ms) {
		return 0u;
	}

	/*
	 * Advance the schedule; don't re-anchor it to the tick.
	 * `next_beat_ms = elapsed_ms + interval_ms` looks equivalent
	 * but isn't: elapsed_ms is quantised to the tick, so
	 * re-anchoring every beat compounds rounding instead of
	 * cancelling it. Measured effect at 72 bpm (833 ms interval):
	 * a 60 s BLE connection saw 67 HR notifications, not 72 -
	 * exactly the event-count/interval disagreement the profile
	 * tests exist to catch. Accumulating the exact interval keeps
	 * the average right; individual beats still land on a tick.
	 *
	 * (The paragraph above is moved verbatim from the beat_work_fn() it
	 * used to live in. The arithmetic below is unchanged too - what is new
	 * is that it is now reachable from a test. tests/sim_sched/ implements
	 * the re-anchoring variant alongside this one and asserts it produces
	 * exactly 67 beats in the first 60 000 ms, so the test demonstrably
	 * discriminates between the bug and the fix rather than merely agreeing
	 * with whatever the code currently does.)
	 */
	s->next_beat_ms += interval_ms;
	if (s->next_beat_ms <= s->elapsed_ms) {
		/* Recovery, not steady state: a bpm change or missed
		 * ticks can leave the schedule far behind; re-anchor
		 * here, once, rather than burst-fire catch-up beats. */
		s->next_beat_ms = s->elapsed_ms + interval_ms;
	}

	return 1u;
}
