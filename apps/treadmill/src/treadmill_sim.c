/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The reference implementation of src/treadmill_source.h: a synthetic runner
 * on a synthetic machine, and the only "hardware" this application has.
 *
 * It exists twice over, exactly as apps/hrm_ble/src/hrm_sim.c does. It is the
 * bench instrument - a DK has no belt and no deck - and it is the worked
 * example of the seam that a manufacturer reads before writing their own. That
 * second job is why it implements set_incline() and set_speed() properly, with
 * a ramp and a limit, rather than snapping to the commanded value: a real
 * actuator takes seconds to move and a controller that never sees an
 * intermediate value is a controller nobody has tested the reporting path of.
 *
 * ─── What it deliberately does NOT do ──────────────────────────────────────
 *
 * It does not integrate distance, count strides into a total, or compute
 * energy. treadmill_state.c does all of that, from the belt speed and incline
 * this file reports. A simulator that kept its own totals would be a second
 * copy of the truth and the first thing to disagree with the pages.
 *
 * ─── The stride generator, and why it is a phase accumulator ───────────────
 *
 * Strides are EDGES. Emitting `cadence * dt / 60` strides per tick and
 * rounding is the drift bug apps/hrm_ble/src/hrm_sim_sched.c was written to
 * document: at 90 strides/min on a 100 ms tick that is 0.15 strides per tick,
 * which rounds to zero forever. So this keeps a fractional phase and emits a
 * stride each time it crosses one, which produces exactly the right count over
 * any interval and puts the events where they actually fall.
 */

#include <errno.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "treadmill_source.h"
#include "treadmill_state.h"

LOG_MODULE_DECLARE(treadmill, CONFIG_TREADMILL_LOG_LEVEL);

/* The simulator's own tick. Independent of the node's 100 ms housekeeping tick
 * on purpose: a source owns when it looks at its own hardware, and coupling
 * the two would make "the sim ticks with the node" a property somebody later
 * relies on. */
#define SIM_TICK_MS 100u

/*
 * How fast the deck moves, in 0.01 % of grade per second. A real incline
 * actuator on a commercial treadmill takes roughly ten seconds end to end over
 * a 15 % range, so 150 (1.50 %/s) is the right order. It is a Kconfig-free
 * constant because it is a property of this simulated machine and not a
 * setting: a manufacturer replaces the whole file.
 */
#define INCLINE_RAMP_CENTI_PER_S 150

/* And the belt, in mm/s per second. A treadmill that jumped from 0 to 4 m/s
 * would throw its rider off, and every console ramps. */
#define SPEED_RAMP_MM_S_PER_S 300u

struct sim_data {
	uint32_t speed_mm_s;
	uint32_t target_speed_mm_s;
	int32_t  incline_centi;
	int32_t  target_incline_centi;

	/* Stride phase in 1/1000 of a stride, and the cadence that drives it. */
	uint32_t stride_phase_milli;
	uint32_t cadence_16;

	bool     running;
	uint32_t elapsed_ms;
};

static struct sim_data sim;

static void sim_tick_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(sim_tick, sim_tick_fn);

/*
 * Cadence from belt speed, because a treadmill has no way to measure a
 * runner's cadence and every console that reports one estimates it.
 *
 * The relation used is the one that matches observed running: stride length
 * grows with speed, so cadence grows more slowly than speed does. A linear fit
 * through two well-known points - about 76 strides/min at 2.2 m/s (a jog) and
 * about 90 at 4.5 m/s (a fast run) - is 6.1 strides/min per m/s with an
 * intercept of 62.6. That is a MODEL AND NOT A MEASUREMENT, and it is here
 * only so the cadence field carries something a receiver will believe rather
 * than a constant. A real machine reports what its deck sensor saw.
 *
 * Returned in 1/16 strides/min, which is the finest of the three cadence units
 * this node emits (see treadmill_state.h).
 */
static uint32_t sim_cadence_16(uint32_t speed_mm_s)
{
	uint32_t strides_min_16;

	if (speed_mm_s < 500u) {
		return 0u; /* walking below 0.5 m/s: no running cadence at all */
	}
	/* (62.6 + 6.1 * v) strides/min, v in m/s, in 1/16 units:
	 * 16 * 62.6 = 1002, 16 * 6.1 / 1000 = 0.0976 per mm/s. */
	strides_min_16 = 1002u + (speed_mm_s * 976u) / 10000u;
	return strides_min_16;
}

/* Move `value` towards `target` by at most `step`. */
static int32_t ramp_i32(int32_t value, int32_t target, int32_t step)
{
	if (target > value) {
		value += step;
		return (value > target) ? target : value;
	}
	if (target < value) {
		value -= step;
		return (value < target) ? target : value;
	}
	return value;
}

static void sim_tick_fn(struct k_work *w)
{
	uint32_t strides_due;
	uint32_t phase_step;

	ARG_UNUSED(w);

	sim.elapsed_ms += SIM_TICK_MS;

	/* The workout, and it is a WORKOUT rather than a constant so that every
	 * accumulator on every page actually moves on a bench: a node reporting
	 * a fixed speed makes a stuck encoder and a working one look the same.
	 *
	 *   0-10 s    stopped, READY - a receiver must see the unoccupied state
	 *   10-40 s   warm-up at 1.4 m/s
	 *   40-100 s  run at 3.0 m/s
	 *   100-130 s cool-down at 1.2 m/s
	 *   130 s+    stopped, FINISHED, then round again
	 */
	{
		uint32_t phase_s = sim.elapsed_ms / 1000u;
		uint32_t cycle_s = phase_s % 150u;
		uint32_t want;
		uint8_t  state;

		if (cycle_s < 10u) {
			want = 0u;
			state = TREADMILL_STATE_READY;
		} else if (cycle_s < 40u) {
			want = 1400u;
			state = TREADMILL_STATE_IN_USE;
		} else if (cycle_s < 100u) {
			want = 3000u;
			state = TREADMILL_STATE_IN_USE;
		} else if (cycle_s < 130u) {
			want = 1200u;
			state = TREADMILL_STATE_IN_USE;
		} else {
			want = 0u;
			state = TREADMILL_STATE_FINISHED;
		}

		/* A commanded speed overrides the built-in workout: a bench run
		 * that sends an FTMS Set Target Speed must see the belt obey,
		 * and the profile of a canned workout is only the default. */
		if (sim.target_speed_mm_s != 0u) {
			want = sim.target_speed_mm_s;
			state = TREADMILL_STATE_IN_USE;
		}

		sim.speed_mm_s = (uint32_t)ramp_i32(
			(int32_t)sim.speed_mm_s, (int32_t)want,
			(int32_t)((SPEED_RAMP_MM_S_PER_S * SIM_TICK_MS) / 1000u));
		treadmill_source_report_speed(sim.speed_mm_s);
		treadmill_source_report_state(state);
	}

	/* The deck, ramping towards whatever the last controller asked for.
	 * REPORTED FROM WHERE IT ACTUALLY IS, not from the target - §8.8.4
	 * asks page 17's incline to match the grade received, and a machine
	 * that reported the target during a ten-second ramp would be claiming
	 * to be somewhere it is not. */
	sim.incline_centi = ramp_i32(
		sim.incline_centi, sim.target_incline_centi,
		(int32_t)((INCLINE_RAMP_CENTI_PER_S * (int32_t)SIM_TICK_MS) /
			  1000));
	treadmill_source_report_incline(sim.incline_centi);

	/* Cadence, and the strides that go with it. */
	sim.cadence_16 = sim_cadence_16(sim.speed_mm_s);
	treadmill_source_report_cadence_16(sim.cadence_16);

	/* Phase in 1/1000 of a stride. cadence(1/16 strides/min) * tick(ms) is
	 * (strides/min / 16) * ms, so milli-strides = cadence_16 * dt_ms * 1000
	 * / (16 * 60000) = cadence_16 * dt_ms / 960. */
	phase_step = (sim.cadence_16 * SIM_TICK_MS) / 960u;
	sim.stride_phase_milli += phase_step;
	strides_due = sim.stride_phase_milli / 1000u;
	sim.stride_phase_milli %= 1000u;
	while (strides_due-- > 0u) {
		treadmill_source_report_stride();
	}

	k_work_schedule(&sim_tick, K_MSEC(SIM_TICK_MS));
}

static int sim_start(const struct treadmill_source *src)
{
	ARG_UNUSED(src);

	sim.running = true;
	k_work_schedule(&sim_tick, K_MSEC(SIM_TICK_MS));
	LOG_INF("simulated treadmill: a 150 s workout on repeat, 0 to 3.0 m/s");
	return 0;
}

static int sim_stop(const struct treadmill_source *src)
{
	ARG_UNUSED(src);

	sim.running = false;
	(void)k_work_cancel_delayable(&sim_tick);
	return 0;
}

/*
 * The control path. This machine CAN move both, so both return 0 - which is
 * what makes it the arm that exercises the reporting loop. A manufacturer
 * whose machine cannot must return -ENOTSUP here AND clear the corresponding
 * capability bit (FE-C page 54 bit 2, FTMS Target Setting bit 1); doing one
 * without the other is what makes a controller report the machine as broken.
 */
static int sim_set_incline(const struct treadmill_source *src, int32_t centi_pct)
{
	ARG_UNUSED(src);

	sim.target_incline_centi = centi_pct;
	LOG_INF("incline commanded: %d.%02d %%", centi_pct / 100,
		(centi_pct < 0 ? -centi_pct : centi_pct) % 100);
	return 0;
}

static int sim_set_speed(const struct treadmill_source *src, uint32_t mm_s)
{
	ARG_UNUSED(src);

	sim.target_speed_mm_s = mm_s;
	LOG_INF("speed commanded: %u mm/s", mm_s);
	return 0;
}

static const struct treadmill_source_api sim_api = {
	.start = sim_start,
	.stop = sim_stop,
	.set_incline = sim_set_incline,
	.set_speed = sim_set_speed,
	/* No poll: everything this source does happens on its own tick, and an
	 * empty poll would suggest the node's tick drives it. */
};

static const struct treadmill_source sim_source = {
	.name = "simulated treadmill",
	.api = &sim_api,
	.data = &sim,
};

static int sim_register(void)
{
	return treadmill_source_register(&sim_source);
}

/* APPLICATION level, before main(): treadmill_source_start() must find a
 * source already registered, and a SYS_INIT here is the same hook a real
 * driver's own init would use. */
SYS_INIT(sim_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
