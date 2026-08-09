/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Signal generation for the ANT+ sensor simulator.
 *
 * Deliberately free of Zephyr, of floating point and of libm. Three reasons,
 * in order of how much they cost when ignored:
 *
 *   - It is meant to be lifted into zephyr_aerosense unchanged once that
 *     project's ANT TX path exists, and anything Zephyr-flavoured here would
 *     have to be unpicked first.
 *   - It is host-testable, so the arithmetic can be checked against
 *     tools/ant_pages.py without a board.
 *   - It runs in the profile's page-updated callback, which is SDK context.
 *     Pulling in soft-float there for a value that is about to be rounded to
 *     an integer anyway buys nothing.
 *
 * The stock sdk-ant simulators sweep a ramp from 0 to 2000 W. That is fine for
 * demonstrating that a channel transmits and useless as a reference: a
 * receiver cannot be wrong about a value that never settles. This produces a
 * fixed target with bounded noise instead, reproducibly.
 */

#ifndef SIM_SIGNAL_H__
#define SIM_SIGNAL_H__

#include <stdint.h>

/* Target plus bounded noise, from a seeded generator so a run replays. */
struct sim_signal {
	int32_t  target;
	int32_t  noise;
	uint32_t state;
};

void sim_signal_init(struct sim_signal *sig, int32_t target, int32_t noise,
		     uint32_t seed);

/* Next sample. Never negative: a negative cadence is not a slow one. */
int32_t sim_signal_next(struct sim_signal *sig);

/* Counts whole revolutions of something turning at `rpm`.
 *
 * The torque and combined pages report per-revolution events, not
 * per-message ones. At 80 rpm against a ~4 Hz channel only about one message
 * in three carries a new event, and that is the case a receiver's delta
 * arithmetic actually has to survive - so revolutions are counted rather than
 * assumed to be one per message.
 */
struct sim_revs {
	uint32_t phase_micro;   /* fraction of a revolution, in 1/1000000ths */
};

/* dt is in microseconds, not milliseconds, because the channel periods are
 * not whole milliseconds: 8182 ticks of 32768 Hz is 249.694 ms, and rounding
 * that to 250 puts a systematic 0.12 % into every cadence the receiver
 * reconstructs - small enough to look like noise and large enough to argue
 * about.
 */
uint32_t sim_revs_advance(struct sim_revs *revs, uint32_t dt_us, uint32_t rpm);

/* Per-revolution accumulator increments, in the units the pages carry. */
uint16_t sim_period_ticks(uint32_t rpm);        /* 1/2048 s, pages 0x11/0x12 */
uint16_t sim_event_time_ticks(uint32_t rpm);    /* 1/1024 s, page 0x79      */
uint16_t sim_torque_ticks(uint32_t watts, uint32_t rpm); /* 1/32 Nm         */

#endif /* SIM_SIGNAL_H__ */
