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
 * ─── The machine around the runner ─────────────────────────────────────────
 *
 * Since the motion rule (treadmill_session.h) it also models the parts of a
 * console the rule depends on: a SAFETY KEY, a REMOTE-ENABLE switch, the
 * console's Start/Pause/Stop, and the workout hooks FTMS Start/Stop/Pause
 * reach. With CONFIG_TREADMILL_SIM_BUTTONS the DK's four buttons are those
 * controls (see the table above buttons[] below); without it the key is in and
 * remote control is CONFIG_TREADMILL_SIM_REMOTE_ENABLED, forever.
 *
 * The sim refuses motion with the key out by itself too (-EACCES from its
 * setters). That is defence in depth and NOT the rule: the rule is enforced
 * before the source is ever called, and a real driver must not rely on the
 * radios to keep its belt still with the tether pulled.
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
#if defined(CONFIG_TREADMILL_SIM_BUTTONS)
#include <zephyr/drivers/gpio.h>
#endif

#include "treadmill_source.h"
#include "treadmill_state.h"

LOG_MODULE_DECLARE(treadmill, CONFIG_TREADMILL_LOG_LEVEL);

/* The simulator's own tick. Independent of the node's 100 ms housekeeping tick
 * on purpose: a source owns when it looks at its own hardware. */
#define SIM_TICK_MS 100u

/* How fast the deck moves, in 0.01 % of grade per second: about ten seconds
 * end to end over a 15 % range, like a commercial incline actuator. */
#define INCLINE_RAMP_CENTI_PER_S 150

/* And the belt, in mm/s per second. Every console ramps. */
#define SPEED_RAMP_MM_S_PER_S 300u

/* The canned workout's cycle and the window in which the belt moves. */
#define CYCLE_S      150u
#define CYCLE_MOVE_S 10u  /* the belt starts at 10 s */
#define CYCLE_END_S  130u /* and stops at 130 s */

/* A halted machine reports FINISHED for this long, then READY, the way a
 * console shows the session summary and then goes back to its start screen. */
#define HALT_SUMMARY_MS 20000u

struct sim_data {
	uint32_t speed_mm_s;
	uint32_t target_speed_mm_s;
	int32_t  incline_centi;
	int32_t  target_incline_centi;

	/* Stride phase in 1/960000 of a stride - the exact unit of
	 * cadence_16 x ms - and the cadence that drives it. */
	uint32_t stride_phase_num;
	uint32_t cadence_16;

	/* A controller has commanded a speed, INCLUDING 0: target 0 used to
	 * read as "no override" and the canned workout resumed after a Stop. */
	bool     speed_overridden;

	bool     running;
	/* The canned workout's clock. Frozen while halted or paused, so a
	 * resumed workout continues where it was. */
	uint32_t elapsed_ms;

	/* The console. */
	bool     halted;    /* stopped (console, remote or key): belt at 0 */
	uint32_t halted_ms; /* how long, for the FINISHED -> READY summary */
	bool     paused;
	bool     key_in;
	bool     remote_enabled;
};

static struct sim_data sim = {
	.key_in = true,
	.remote_enabled = IS_ENABLED(CONFIG_TREADMILL_SIM_REMOTE_ENABLED),
};

static void sim_tick_fn(struct k_work *w);
static K_WORK_DELAYABLE_DEFINE(sim_tick, sim_tick_fn);

/*
 * Cadence from belt speed, because a treadmill has no way to measure a
 * runner's cadence and every console that reports one estimates it. A linear
 * fit through ~76 strides/min at 2.2 m/s and ~90 at 4.5 m/s: 6.1 strides/min
 * per m/s, intercept 62.6. A MODEL AND NOT A MEASUREMENT. 1/16 strides/min.
 */
static uint32_t sim_cadence_16(uint32_t speed_mm_s)
{
	if (speed_mm_s < 500u) {
		return 0u; /* walking below 0.5 m/s: no running cadence at all */
	}
	/* 16 * 62.6 = 1002, 16 * 6.1 / 1000 = 0.0976 per mm/s. */
	return 1002u + (speed_mm_s * 976u) / 10000u;
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

/* Start (or restart) the session: un-halt, and if the canned workout's clock
 * is in one of its resting windows, jump it to where the belt moves, so a
 * Start visibly starts something. */
static void sim_begin(void)
{
	uint32_t now_s = sim.elapsed_ms / 1000u;
	uint32_t cycle_s = now_s % CYCLE_S;

	sim.halted = false;
	sim.paused = false;
	sim.speed_overridden = false;
	if (cycle_s < CYCLE_MOVE_S || cycle_s >= CYCLE_END_S) {
		uint32_t base = now_s - cycle_s;

		if (cycle_s >= CYCLE_END_S) {
			base += CYCLE_S;
		}
		sim.elapsed_ms = (base + CYCLE_MOVE_S) * 1000u;
	}
}

static void sim_halt(void)
{
	sim.halted = true;
	sim.halted_ms = 0u;
	sim.paused = false;
	sim.speed_overridden = false;
}

static void sim_tick_fn(struct k_work *w)
{
	uint32_t strides_due;
	uint32_t want;
	uint8_t  state;

	ARG_UNUSED(w);

	if (sim.halted) {
		want = 0u;
		sim.halted_ms += SIM_TICK_MS;
		state = (sim.halted_ms < HALT_SUMMARY_MS) ? TREADMILL_STATE_FINISHED
							  : TREADMILL_STATE_READY;
	} else if (sim.paused) {
		/* This machine's pause stops the belt and keeps the session.
		 * The session already says PAUSED (the event said so); the FE
		 * state stays IN_USE, which the session ignores while paused. */
		want = 0u;
		state = TREADMILL_STATE_IN_USE;
	} else {
		/* The workout, a WORKOUT rather than a constant so that every
		 * accumulator on every page actually moves on a bench:
		 *
		 *   0-10 s    stopped, READY - the unoccupied state
		 *   10-40 s   warm-up at 1.4 m/s
		 *   40-100 s  run at 3.0 m/s
		 *   100-130 s cool-down at 1.2 m/s
		 *   130 s+    stopped, FINISHED, then round again
		 */
		uint32_t cycle_s;

		sim.elapsed_ms += SIM_TICK_MS;
		cycle_s = (sim.elapsed_ms / 1000u) % CYCLE_S;

		if (cycle_s < CYCLE_MOVE_S) {
			want = 0u;
			state = TREADMILL_STATE_READY;
		} else if (cycle_s < 40u) {
			want = 1400u;
			state = TREADMILL_STATE_IN_USE;
		} else if (cycle_s < 100u) {
			want = 3000u;
			state = TREADMILL_STATE_IN_USE;
		} else if (cycle_s < CYCLE_END_S) {
			want = 1200u;
			state = TREADMILL_STATE_IN_USE;
		} else {
			want = 0u;
			state = TREADMILL_STATE_FINISHED;
		}

		/* A commanded speed overrides the built-in workout. */
		if (sim.speed_overridden) {
			want = sim.target_speed_mm_s;
			state = (want != 0u) ? TREADMILL_STATE_IN_USE
					     : TREADMILL_STATE_FINISHED;
		}
	}

	sim.speed_mm_s = (uint32_t)ramp_i32(
		(int32_t)sim.speed_mm_s, (int32_t)want,
		(int32_t)((SPEED_RAMP_MM_S_PER_S * SIM_TICK_MS) / 1000u));
	treadmill_source_report_speed(sim.speed_mm_s);
	treadmill_source_report_state(state);

	/* The deck, ramping towards whatever the last controller asked for and
	 * REPORTED FROM WHERE IT ACTUALLY IS (§8.8.4). */
	sim.incline_centi = ramp_i32(
		sim.incline_centi, sim.target_incline_centi,
		(int32_t)((INCLINE_RAMP_CENTI_PER_S * (int32_t)SIM_TICK_MS) /
			  1000));
	treadmill_source_report_incline(sim.incline_centi);

	sim.cadence_16 = sim_cadence_16(sim.speed_mm_s);
	treadmill_source_report_cadence_16(sim.cadence_16);

	/* strides = cadence_16 * dt_ms / 960000, accumulated exactly. */
	sim.stride_phase_num += sim.cadence_16 * SIM_TICK_MS;
	strides_due = sim.stride_phase_num / 960000u;
	sim.stride_phase_num %= 960000u;
	while (strides_due-- > 0u) {
		treadmill_source_report_stride();
	}

	k_work_schedule(&sim_tick, K_MSEC(SIM_TICK_MS));
}

/* ── The console (reached from the bench buttons) ───────────────────────── */

#if defined(CONFIG_TREADMILL_SIM_BUTTONS)
static void console_start(void)
{
	if (!sim.key_in) {
		LOG_WRN("sim console: Start ignored, the safety key is out");
		return;
	}
	sim_begin();
	(void)treadmill_source_report_event(TREADMILL_EVT_CONSOLE_START);
	LOG_INF("sim console: Start");
}

static void console_pause(void)
{
	if (sim.halted || sim.paused) {
		return;
	}
	sim.paused = true;
	(void)treadmill_source_report_event(TREADMILL_EVT_CONSOLE_PAUSE);
	LOG_INF("sim console: Pause");
}

static void console_stop(void)
{
	sim_halt();
	(void)treadmill_source_report_event(TREADMILL_EVT_CONSOLE_STOP);
	LOG_INF("sim console: Stop");
}

static void safety_key(bool in)
{
	sim.key_in = in;
	if (!in) {
		/* The tether: the belt stops NOW, not on a ramp. */
		sim_halt();
		sim.speed_mm_s = 0u;
		treadmill_source_report_speed(0u);
		(void)treadmill_source_report_event(TREADMILL_EVT_SAFETY_KEY_OUT);
		LOG_WRN("sim: SAFETY KEY OUT - belt stopped, remote control revoked");
	} else {
		(void)treadmill_source_report_event(TREADMILL_EVT_SAFETY_KEY_IN);
		LOG_INF("sim: safety key in - press Start or Stop to clear the latch");
	}
}
#endif /* CONFIG_TREADMILL_SIM_BUTTONS */

/* ── The seam ───────────────────────────────────────────────────────────── */

static int sim_start(const struct treadmill_source *src)
{
	ARG_UNUSED(src);

	sim.running = true;
	k_work_schedule(&sim_tick, K_MSEC(SIM_TICK_MS));
	LOG_INF("simulated treadmill: a 150 s workout on repeat, 0 to 3.0 m/s; "
		"remote control %s", sim.remote_enabled ? "enabled" : "disabled");
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
 * whose machine cannot leaves the entry point NULL; the application derives
 * the capability bits from that (treadmill_caps.c).
 */
static int sim_set_incline(const struct treadmill_source *src, int32_t centi_pct)
{
	ARG_UNUSED(src);

	if (!sim.key_in) {
		return -EACCES; /* defence in depth, see the header */
	}
	sim.target_incline_centi = centi_pct;
	LOG_INF("incline commanded: %d.%02d %%", centi_pct / 100,
		(centi_pct < 0 ? -centi_pct : centi_pct) % 100);
	return 0;
}

static int sim_set_speed(const struct treadmill_source *src, uint32_t mm_s)
{
	ARG_UNUSED(src);

	if (mm_s != 0u) {
		if (!sim.key_in) {
			return -EACCES;
		}
		/* A speed from rest starts the belt - the auto-start the motion
		 * rule's condition (c) has already authorised. */
		sim.halted = false;
		sim.paused = false;
	}
	sim.target_speed_mm_s = mm_s;
	sim.speed_overridden = true;
	LOG_INF("speed commanded: %u mm/s", mm_s);
	return 0;
}

static int sim_workout_start(const struct treadmill_source *src)
{
	ARG_UNUSED(src);

	if (!sim.key_in) {
		return -EACCES;
	}
	sim_begin();
	LOG_INF("workout start (remote)");
	return 0;
}

static int sim_workout_pause(const struct treadmill_source *src)
{
	ARG_UNUSED(src);

	if (sim.halted) {
		return -EINVAL; /* nothing running to pause */
	}
	if (!sim.paused) {
		sim.paused = true;
		(void)treadmill_source_report_event(TREADMILL_EVT_PAUSED);
		LOG_INF("workout pause (remote)");
	}
	return 0;
}

static int sim_workout_resume(const struct treadmill_source *src)
{
	ARG_UNUSED(src);

	if (!sim.key_in) {
		return -EACCES;
	}
	if (!sim.paused) {
		return -EINVAL;
	}
	sim.paused = false;
	(void)treadmill_source_report_event(TREADMILL_EVT_RESUMED);
	LOG_INF("workout resume (remote)");
	return 0;
}

static int sim_workout_stop(const struct treadmill_source *src)
{
	ARG_UNUSED(src);

	/* A remote Stop is not a console action: no event, and the session
	 * follows the FINISHED this reports on the next tick. */
	sim_halt();
	LOG_INF("workout stop (remote)");
	return 0;
}

static bool sim_remote_motion_allowed(const struct treadmill_source *src)
{
	ARG_UNUSED(src);

	return sim.key_in && sim.remote_enabled;
}

static const struct treadmill_source_api sim_api = {
	.start = sim_start,
	.stop = sim_stop,
	.set_incline = sim_set_incline,
	.set_speed = sim_set_speed,
	.workout_start = sim_workout_start,
	.workout_pause = sim_workout_pause,
	.workout_resume = sim_workout_resume,
	.workout_stop = sim_workout_stop,
	.remote_motion_allowed = sim_remote_motion_allowed,
	/* No poll: everything this source does happens on its own tick and
	 * its own button work items. */
};

static const struct treadmill_source sim_source = {
	.name = "simulated treadmill",
	.api = &sim_api,
	.data = &sim,
};

/* ── The bench controls: the DK's four buttons ──────────────────────────────
 *
 *   sw0  safety key: each press pulls it / puts it back
 *   sw1  remote-enable switch: each press toggles it
 *   sw2  console: short press Start (or Resume), or Pause while running;
 *        hold >= 1.5 s for Stop
 *   sw3  pairing: short press opens the BLE pairing window; hold >= 5 s to
 *        forget every bond
 *
 * Each button is debounced by a delayed sample (as apps/hrm_ble's contact
 * button) and acted on at RELEASE, so a hold can be told from a press. All of
 * it runs on the system workqueue, the same context as the tick and the
 * hooks, so `sim` needs no lock.
 */
#if defined(CONFIG_TREADMILL_SIM_BUTTONS)

#define BTN_QUIET_MS       30u
#define BTN_STOP_HOLD_MS   1500
#define BTN_FORGET_HOLD_MS 5000

struct sim_button {
	struct gpio_dt_spec spec;
	struct gpio_callback cb;
	struct k_work_delayable settle;
	bool pressed;
	int64_t down_ms;
	uint8_t id;
};

static struct sim_button buttons[4] = {
	{ .spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios), .id = 0 },
	{ .spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw1), gpios), .id = 1 },
	{ .spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw2), gpios), .id = 2 },
	{ .spec = GPIO_DT_SPEC_GET(DT_ALIAS(sw3), gpios), .id = 3 },
};

static void button_action(uint8_t id, int64_t held_ms)
{
	switch (id) {
	case 0:
		safety_key(!sim.key_in);
		break;
	case 1:
		sim.remote_enabled = !sim.remote_enabled;
		LOG_INF("sim console: remote control %s",
			sim.remote_enabled ? "ENABLED" : "disabled");
		break;
	case 2:
		if (held_ms >= BTN_STOP_HOLD_MS) {
			console_stop();
		} else if (!sim.halted && !sim.paused) {
			console_pause();
		} else {
			console_start();
		}
		break;
	case 3:
		(void)treadmill_source_report_event(held_ms >= BTN_FORGET_HOLD_MS
							    ? TREADMILL_EVT_FORGET_BONDS
							    : TREADMILL_EVT_PAIRING_WINDOW);
		break;
	default:
		break;
	}
}

static void button_settled(struct k_work *w)
{
	struct k_work_delayable *dw = k_work_delayable_from_work(w);
	struct sim_button *b = CONTAINER_OF(dw, struct sim_button, settle);
	bool level = gpio_pin_get_dt(&b->spec) > 0;

	if (level && !b->pressed) {
		b->pressed = true;
		b->down_ms = k_uptime_get();
	} else if (!level && b->pressed) {
		b->pressed = false;
		button_action(b->id, k_uptime_get() - b->down_ms);
	}
}

static void button_isr(const struct device *port, struct gpio_callback *cb,
		       gpio_port_pins_t pins)
{
	struct sim_button *b = CONTAINER_OF(cb, struct sim_button, cb);

	ARG_UNUSED(port);
	ARG_UNUSED(pins);
	(void)k_work_reschedule(&b->settle, K_MSEC(BTN_QUIET_MS));
}

static int buttons_init(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(buttons); i++) {
		struct sim_button *b = &buttons[i];
		int err;

		if (!gpio_is_ready_dt(&b->spec)) {
			return -ENODEV;
		}
		err = gpio_pin_configure_dt(&b->spec, GPIO_INPUT);
		if (err != 0) {
			return err;
		}
		k_work_init_delayable(&b->settle, button_settled);
		gpio_init_callback(&b->cb, button_isr, BIT(b->spec.pin));
		err = gpio_add_callback_dt(&b->spec, &b->cb);
		if (err != 0) {
			return err;
		}
		err = gpio_pin_interrupt_configure_dt(&b->spec, GPIO_INT_EDGE_BOTH);
		if (err != 0) {
			return err;
		}
	}
	return 0;
}
#endif /* CONFIG_TREADMILL_SIM_BUTTONS */

static int sim_register(void)
{
#if defined(CONFIG_TREADMILL_SIM_BUTTONS)
	int err = buttons_init();

	if (err != 0) {
		LOG_WRN("sim: buttons unavailable (%d); the key stays in and "
			"remote control stays %s", err,
			sim.remote_enabled ? "enabled" : "disabled");
	}
#endif
	return treadmill_source_register(&sim_source);
}

/* APPLICATION level, before main(): treadmill_source_start() must find a
 * source already registered. */
SYS_INIT(sim_register, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
