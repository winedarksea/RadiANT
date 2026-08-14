/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Signal generation for the ANT+ sensor simulator.
 *
 * No Zephyr, no floating point, no libm: meant to be lifted into
 * zephyr_aerosense unchanged, host-testable against tools/ant_pages.py, and
 * runs in the profile's page-updated callback (SDK context) where
 * soft-float buys nothing for a value about to be rounded to an integer.
 *
 * Stock sdk-ant simulators ramp 0-2000 W, which never settles and so can't
 * serve as a reference. This holds a fixed target with bounded noise
 * instead, reproducibly.
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
 * per-message ones: at 80 rpm against a ~4 Hz channel only about one
 * message in three carries a new event, and that's the case a receiver's
 * delta arithmetic has to survive.
 */
struct sim_revs {
	uint32_t phase_micro;   /* fraction of a revolution, in 1/1000000ths */
};

/* dt is in microseconds, not milliseconds: channel periods aren't whole
 * milliseconds (8182 ticks @ 32768 Hz = 249.694 ms), and rounding to 250
 * would put a systematic 0.12% into every reconstructed cadence.
 */
uint32_t sim_revs_advance(struct sim_revs *revs, uint32_t dt_us, uint32_t rpm);

/* Per-revolution accumulator increments, in the units the pages carry. */
uint16_t sim_period_ticks(uint32_t rpm);        /* 1/2048 s, pages 0x11/0x12 */
uint16_t sim_event_time_ticks(uint32_t rpm);    /* 1/1024 s, page 0x79      */
uint16_t sim_torque_ticks(uint32_t watts, uint32_t rpm); /* 1/32 Nm         */

#endif /* SIM_SIGNAL_H__ */
