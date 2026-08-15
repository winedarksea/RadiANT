/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The check that did not exist.
 *
 * This application once shipped a heart-rate node that a 60 s BLE hold measured
 * at 67 HR notifications while byte 7 of its ANT+ page claimed 72 bpm. The bug
 * was three lines of scheduling arithmetic, and 681 ztest cases did not catch
 * it because the arithmetic lived inside a k_work handler that needed a radio,
 * a workqueue and a stopwatch to observe.
 *
 * src/hrm_sim_sched.c exists so that it does not. Everything below is a counted
 * loop over a pure function: no clock, no kernel object, no board.
 *
 * ── The four assertions and why each is here ───────────────────────────────
 *
 *  1. steady_rate_72        the exact beat counts, pinned. If this file's
 *                           numbers and the scheduler ever disagree, one of
 *                           them changed on purpose and the diff says which.
 *  2. reanchoring_drifts    the SAME loop over a local reimplementation of the
 *                           BUG, asserting it produces exactly 67 in the first
 *                           minute. Without this, assertion 1 only proves the
 *                           test agrees with whatever the code does - it does
 *                           not prove the test can tell the two apart.
 *  3. every_rate_in_range   the property, over the whole 30..220 bpm Kconfig
 *                           range, rather than the one rate the bench used.
 *  4. one_beat_per_tick     the scheduler never burst-fires, including across a
 *                           rate change, which is the failure the recovery
 *                           branch inside it exists to prevent.
 */

#include <zephyr/ztest.h>

#include "hrm_sim_sched.h"

/* The simulator's tick. Kept as a literal rather than included from hrm_sim.c,
 * which would drag in the kernel: if SIM_TICK_MS there ever changes, assertion
 * 1's pinned numbers are wrong and this is where that shows up. */
#define TICK_MS 100u

#define MINUTE_TICKS (60000u / TICK_MS)  /*   600 ticks */
#define HOUR_TICKS   (MINUTE_TICKS * 60u) /* 36000 ticks */

/*
 * The bug, reimplemented locally so assertion 2 can be made about it.
 *
 * The single difference from hrm_sim_sched_tick() is `next = elapsed +
 * interval` instead of `next += interval`. It looks equivalent. elapsed_ms is
 * quantised to the tick, so re-anchoring turns the 833 ms interval into a
 * 900 ms one and compounds the rounding instead of cancelling it.
 */
static uint32_t reanchoring_tick(struct hrm_sim_sched *s, uint32_t tick_ms,
				 uint32_t interval_ms)
{
	s->elapsed_ms += tick_ms;
	if (s->elapsed_ms < s->next_beat_ms) {
		return 0u;
	}
	s->next_beat_ms = s->elapsed_ms + interval_ms;
	return 1u;
}

ZTEST_SUITE(hrm_sim_sched, NULL, NULL, NULL, NULL, NULL);

/*
 * 72 bpm, one hour, every number pinned.
 *
 * 60000 / 72 truncates to an 833 ms interval, against a true 833.33, so the
 * scheduler is expected to run very slightly FAST - 4322 beats where 60 minutes
 * at exactly 72 bpm would be 4320. Two of those are accounted for: one is the
 * beat at t = 0 that anchors the schedule, and the rest is 3 600 000 ms of the
 * 0.33 ms per-interval truncation. Neither is drift and both are stable, which
 * is why they can be pinned rather than tolerated.
 */
ZTEST(hrm_sim_sched, test_steady_rate_72)
{
	struct hrm_sim_sched s = {0};
	const uint32_t interval = hrm_sim_sched_interval_ms(72u);
	uint32_t total = 0;
	uint32_t minute_counts[60];

	zassert_equal(interval, 833u, "60000/72 should truncate to 833 ms");

	for (uint32_t m = 0; m < 60u; m++) {
		uint32_t in_minute = 0;

		for (uint32_t i = 0; i < MINUTE_TICKS; i++) {
			in_minute += hrm_sim_sched_tick(&s, TICK_MS, interval);
		}
		minute_counts[m] = in_minute;
		total += in_minute;
	}

	/* The first minute carries the anchoring beat at t = 0 as well as its
	 * own 72, which is arithmetic and not an off-by-one: a schedule has to
	 * start somewhere and the alternative is a node whose first beat is one
	 * interval late. */
	zassert_equal(minute_counts[0], 73u,
		      "first minute: %u beats, expected 73 (72 + the anchor)",
		      minute_counts[0]);

	for (uint32_t m = 1; m < 60u; m++) {
		zassert_equal(minute_counts[m], 72u,
			      "minute %u: %u beats, expected exactly 72",
			      m + 1u, minute_counts[m]);
	}

	zassert_equal(total, 4322u,
		      "one hour at 72 bpm: %u beats, expected 4322", total);
}

/*
 * The same loop, over the bug. 67 in the first minute is the number the bench
 * actually measured over a 60 s BLE hold, which is what makes this assertion
 * evidence rather than a restatement of the code.
 */
ZTEST(hrm_sim_sched, test_reanchoring_drifts)
{
	struct hrm_sim_sched s = {0};
	const uint32_t interval = hrm_sim_sched_interval_ms(72u);
	uint32_t first_minute = 0;
	uint32_t total = 0;

	for (uint32_t i = 0; i < MINUTE_TICKS; i++) {
		first_minute += reanchoring_tick(&s, TICK_MS, interval);
	}
	total = first_minute;
	for (uint32_t i = MINUTE_TICKS; i < HOUR_TICKS; i++) {
		total += reanchoring_tick(&s, TICK_MS, interval);
	}

	/* If this ever reads 72, the local reimplementation above has been
	 * "fixed" and assertion 1 has stopped being able to fail. */
	zassert_equal(first_minute, 67u,
		      "the re-anchoring variant should produce 67 beats in the "
		      "first minute - the figure measured on the bench - but "
		      "produced %u", first_minute);

	/* 833 ms rounded up to the 100 ms tick is 900, and 3 600 000 / 900 is
	 * exactly 4000. The bug is a 7.4 % rate error, not a rounding wobble. */
	zassert_equal(total, 4000u,
		      "the re-anchoring variant over one hour: %u, expected "
		      "4000", total);
}

/*
 * The property, over the range Kconfig actually admits.
 *
 * 1 % rather than something tighter, and the bound is derived rather than
 * chosen: the only systematic error the correct scheduler has is the truncation
 * in 60000/bpm, which is worst at the top of the range - at 220 bpm the
 * interval is 272 ms against a true 272.7, about 0.27 %. 1 % leaves margin for
 * the anchoring beat at every rate and still catches the re-anchoring bug
 * wherever the interval is not a whole number of ticks (7.4 % at 72 bpm).
 *
 * It does NOT catch the bug at 30, 60, 120 or 200 bpm, where the interval
 * divides the tick exactly and the two implementations agree. That is why
 * assertion 2 names a specific rate instead of relying on this one.
 */
ZTEST(hrm_sim_sched, test_every_rate_in_range)
{
	for (uint32_t bpm = 30u; bpm <= 220u; bpm++) {
		struct hrm_sim_sched s = {0};
		const uint32_t interval = hrm_sim_sched_interval_ms(bpm);
		uint32_t beats = 0;
		uint32_t ideal = 60u * bpm; /* one hour at `bpm` */
		uint32_t err_permille;

		for (uint32_t i = 0; i < HOUR_TICKS; i++) {
			beats += hrm_sim_sched_tick(&s, TICK_MS, interval);
		}

		err_permille = (beats > ideal)
			? ((beats - ideal) * 1000u) / ideal
			: ((ideal - beats) * 1000u) / ideal;

		zassert_true(err_permille <= 10u,
			     "%u bpm: %u beats in one hour, expected about %u "
			     "(%u permille off, budget 10)",
			     bpm, beats, ideal, err_permille);
	}
}

/*
 * No burst-firing, ever - including across a rate change, which is the case the
 * recovery branch inside the scheduler exists for.
 *
 * The failure this rules out is not cosmetic. A burst would push several
 * profile_hr_beat() calls through the seam in one tick, all carrying the same
 * instant; the event count would leap while the event time stood still, and a
 * receiver differencing the pair would compute an enormous rate from a strap
 * that had merely been told to slow down.
 */
ZTEST(hrm_sim_sched, test_one_beat_per_tick)
{
	struct hrm_sim_sched s = {0};

	/* Ten minutes at 72, then the rate is halved mid-schedule - the worst
	 * direction, because the next beat is now due much later than the
	 * pending `next_beat_ms` implies. */
	for (uint32_t i = 0; i < MINUTE_TICKS * 10u; i++) {
		uint32_t n = hrm_sim_sched_tick(&s, TICK_MS,
						hrm_sim_sched_interval_ms(72u));

		zassert_true(n <= 1u, "tick %u produced %u beats", i, n);
	}

	for (uint32_t i = 0; i < MINUTE_TICKS * 10u; i++) {
		uint32_t n = hrm_sim_sched_tick(&s, TICK_MS,
						hrm_sim_sched_interval_ms(36u));

		zassert_true(n <= 1u, "tick %u after the rate change produced "
			     "%u beats", i, n);
	}

	/* And the other direction: doubling it must not produce a run of
	 * catch-up beats either. */
	for (uint32_t i = 0; i < MINUTE_TICKS * 10u; i++) {
		uint32_t n = hrm_sim_sched_tick(&s, TICK_MS,
						hrm_sim_sched_interval_ms(144u));

		zassert_true(n <= 1u, "tick %u after the second rate change "
			     "produced %u beats", i, n);
	}
}
