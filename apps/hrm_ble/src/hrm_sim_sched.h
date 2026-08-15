/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The simulated source's beat schedule, as a PURE FUNCTION.
 *
 * It is separated from src/hrm_sim.c - which owns the delayable work item, the
 * kernel timebase and the profile - for exactly one reason: so that a ztest can
 * drive 36 000 virtual ticks through it in a few microseconds, on any board,
 * with no radio, no workqueue and no clock, and assert the beat count.
 *
 * That test is the cheapest of the three drift checks in the reference design's
 * production checklist, and it is the one that DID NOT EXIST when this
 * application shipped a 67-beats-per-minute strap that claimed 72. See
 * apps/hrm_ble/tests/sim_sched/ and hrm_sim_sched.c's own comment.
 */

#ifndef HRM_SIM_SCHED_H_
#define HRM_SIM_SCHED_H_

#include <stdint.h>

struct hrm_sim_sched {
	/* Wall time the schedule has been advanced through, in ms. Quantised to
	 * the tick, which is the whole subject of hrm_sim_sched.c's comment. */
	uint32_t elapsed_ms;
	/* When the next beat is due, in the same units. NOT quantised. */
	uint32_t next_beat_ms;
};

/*
 * Advance by one tick. Returns 1 if a beat is due on this tick, 0 otherwise.
 *
 * Zero-initialise the struct; the first tick then fires a beat, which anchors
 * the schedule. `interval_ms` may change between calls (a bpm change): the
 * recovery branch inside handles it without burst-firing catch-up beats.
 *
 * Pure: no globals, no kernel calls, no I/O. Everything it knows is in `s`.
 */
uint32_t hrm_sim_sched_tick(struct hrm_sim_sched *s, uint32_t tick_ms,
			    uint32_t interval_ms);

/* beats-per-minute to the millisecond interval the scheduler runs on. Shared
 * with the test so the test cannot quietly disagree about the rounding. */
static inline uint32_t hrm_sim_sched_interval_ms(uint32_t bpm)
{
	return (bpm != 0u) ? (60000u / bpm) : 1000u;
}

#endif /* HRM_SIM_SCHED_H_ */
