/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_source.h - THE ONE SEAM a smart-trainer manufacturer implements.
 *
 * Everything else in apps/smart_trainer - ANT+ FE-C, FTMS over BLE, the
 * control arbiter, identity, bonding - is the reference design and stays as it
 * is. A product replaces the simulator (trainer_sim.c) with a driver that
 * fills in struct trainer_source_ops and calls trainer_source_report().
 *
 * ─── Two directions ────────────────────────────────────────────────────────
 *
 * COMMANDS ARE PULLED: the application calls a hook when a controller (an
 * FE-C head unit or an FTMS client) asks for something and the arbiter lets
 * it. Every hook runs on the SYSTEM WORKQUEUE, one at a time, and must not
 * block for long - it should hand the request to the brake and return.
 *
 * MEASUREMENTS ARE PUSHED: the driver calls trainer_source_report() whenever
 * it has a fresh reading (a few times a second is plenty; FE-C transmits at
 * 4 Hz and FTMS notifies at 1 Hz). Any thread, any rate; the latest report
 * wins. Session events (trainer_source_report_event()) likewise.
 *
 * ─── What a hook's return means (Phase 3's success semantics) ──────────────
 *
 * A hook returns 0 when the BRAKE ACCEPTED the command, -ENOTSUP when it cannot
 * do that kind of thing at all, -EINVAL/-ERANGE for a value it refuses, and
 * any other negative errno for a failure. That return, and only that, decides
 * the answer on the wire:
 *
 *     hook   FTMS Control Point     FE-C page 71
 *     0      0x01 Success           PASS
 *     -EINVAL/-ERANGE  0x03         FAIL
 *     other  0x04 Operation Failed  FAIL
 *
 * Training Status and FE state, by contrast, follow the driver's REPORTS: a
 * trainer that accepts Start but never sees a pedal stroke still reports what
 * it reports. A NULL target hook is a capability the machine does not have:
 * the FTMS Target Setting bit and the FE-C page 54 mode bit are cleared, the
 * FTMS op code answers 0x02, and the FE-C page is ignored (FE-C SS8.12.3.1:
 * "an unsupported control page ... shall be ignored").
 *
 * ─── No motion authorisation, but an epoch ─────────────────────────────────
 *
 * A trainer is PASSIVE - it brakes a wheel the rider turns and cannot move
 * anything by itself - so unlike apps/treadmill there is no motion rule. It
 * does keep the session EPOCH: a local stop or reset at the trainer
 * (TRAINER_EVT_CONSOLE_STOP / _RESET) bumps it, and a command that was queued
 * before that is refused (FTMS 0x04, FE-C REJECTED) without reaching a hook.
 *
 * ─── Units ─────────────────────────────────────────────────────────────────
 *
 * Transport-neutral, chosen so both radios convert exactly or nearly so:
 *   power        W
 *   resistance   per mille of the brake's maximum (FE-C page 48 is 0.5 %
 *                steps = 5 per mille; FTMS level 0.1 = 5 per mille, see
 *                trainer_cmd.h)
 *   grade        0.01 % (FE-C page 51 and FTMS 0x11 share it)
 *   Crr          1e-5 (FE-C 5e-5 x5, FTMS 1e-4 x10)
 *   Cw           0.01 kg/m (both)
 *   wind         mm/s, positive = headwind (FTMS's unit; FE-C km/h x 1000/3.6)
 *   drafting     0.01, 100 = no drafting (FE-C only; FTMS leaves it)
 *   mass         0.01 kg rider, 0.05 kg bike (FE-C page 55's units)
 *   wheel        0.1 mm circumference (FTMS 0x12's unit; FE-C page 55's
 *                diameter x pi)
 */
#ifndef TRAINER_SOURCE_H_
#define TRAINER_SOURCE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Simulation parameters (FE-C pages 50 + 51, FTMS 0x11). */
struct trainer_sim_params {
	int32_t  grade_centi_pct; /* 0.01 %, FE-C range -200.00..+200.00 */
	uint16_t crr_e5;          /* 1e-5 */
	uint8_t  cw_centi;        /* 0.01 kg/m */
	int32_t  wind_mm_s;       /* positive = headwind */
	uint8_t  drafting_centi;  /* 0.01; 100 = none */
};

/* FE-C SS8.8.4's defaults, which a field's "use default" sentinel selects. */
#define TRAINER_SIM_DEFAULT_CRR_E5         400u  /* 0.004 */
#define TRAINER_SIM_DEFAULT_CW_CENTI       51u   /* 0.51 kg/m */
#define TRAINER_SIM_DEFAULT_DRAFTING_CENTI 100u

/* The user and the bike (FE-C page 55, FTMS 0x12). */
struct trainer_user {
	uint16_t user_weight_10g;    /* 0.01 kg */
	uint16_t bike_weight_50g;    /* 0.05 kg */
	uint16_t wheel_circ_deci_mm; /* 0.1 mm */
};

#define TRAINER_USER_DEFAULT_WEIGHT_10G      7500u  /* 75.00 kg */
#define TRAINER_USER_DEFAULT_BIKE_50G        200u   /* 10.00 kg */
#define TRAINER_USER_DEFAULT_WHEEL_DECI_MM   21050u /* 2105 mm, 700x25c */

/* The optional workout hook's four requests (FTMS 0x07 / 0x08). */
enum trainer_workout {
	TRAINER_WORKOUT_START = 0,
	TRAINER_WORKOUT_RESUME,
	TRAINER_WORKOUT_PAUSE,
	TRAINER_WORKOUT_STOP,
};

struct trainer_source_ops {
	/* Logged at boot, so a reader sees THEIR driver's name. */
	const char *name;

	/* Optional. Called once from main(), before the channels open. */
	int (*start)(void);

	/* ── Targets. NULL = not supported (see the header comment). ─────── */

	/* ERG: hold this power regardless of cadence (FE-C 49, FTMS 0x05). */
	int (*set_target_power)(uint16_t watts);
	/* Basic resistance, per mille of maximum (FE-C 48, FTMS 0x04). */
	int (*set_resistance)(uint16_t permille);
	/* Simulation: the road (FE-C 50/51, FTMS 0x11). */
	int (*set_sim)(const struct trainer_sim_params *p);
	/* The rider and bike (FE-C 55, FTMS 0x12). Every field is valid: the
	 * application substitutes the defaults above for a sentinel. */
	int (*set_user)(const struct trainer_user *u);

	/* ── Optional ─────────────────────────────────────────────────────── */

	/* FTMS Start/Resume and Stop/Pause. NULL answers 0x04 (they are
	 * mandatory op codes, so "not supported" is not an answer). A driver
	 * that accepts reports the result through trainer_source_report_event()
	 * once the trainer is really in that state. */
	int (*workout)(enum trainer_workout w);

	/* ── Static facts, read once at boot ──────────────────────────────── */

	/* FE-C page 54's maximum resistance, N. 0 = unknown (sent invalid). */
	uint16_t max_resistance_n;
	/* FTMS Supported Power Range, W. Both 0 = the Kconfig defaults. */
	uint16_t min_power_w;
	uint16_t max_power_w;
};

/* A measurement. `_valid` false sends that field's sentinel. */
struct trainer_report {
	uint16_t power_w;
	bool     power_valid;
	uint8_t  cadence_rpm;
	bool     cadence_valid;
	uint32_t speed_mm_s;   /* wheel or virtual speed */
	bool     speed_valid;
	bool     speed_virtual; /* FE-C page 16 capabilities bit 3 */
	/* The brake force now, for FE-C page 17's resistance field outside
	 * basic-resistance mode. Invalid = 0xFF there. */
	uint16_t resistance_n;
	bool     resistance_valid;
	/* ERG cannot be met at this cadence (FE-C page 25 flags). */
	bool     erg_speed_low;
	/* The rider is turning the cranks. A session that is IDLE starts
	 * (RUNNING) on the first report with this set. PAUSED and STOPPED are
	 * left only by an explicit FTMS Start/Resume, a TRAINER_EVT_STARTED
	 * or a console action - pedalling on after a Pause does not resume. */
	bool     pedalling;
};

/* Session events a driver reports. */
enum trainer_source_event {
	/* The trainer is now in this state (the result of a workout hook, or
	 * of the rider). Not epoch-bumping. */
	TRAINER_EVT_STARTED = 0,
	TRAINER_EVT_PAUSED,
	TRAINER_EVT_STOPPED,
	/* A stop or reset at the trainer itself. Bump the epoch: anything
	 * queued from a controller before it is refused. RESET also returns
	 * the session to IDLE and RELEASES THE BRAKE through the hooks, the
	 * same release as FTMS Reset (trainer_cmd.h): the driver need not do
	 * it itself, and should not report the reset before it happened. */
	TRAINER_EVT_CONSOLE_STOP,
	TRAINER_EVT_CONSOLE_RESET,
	/* The BLE pairing window, opened at the trainer (radiant_ble_pairing.h),
	 * and the service action that forgets every bond. */
	TRAINER_EVT_PAIRING_WINDOW,
	TRAINER_EVT_FORGET_BONDS,
};

/* Register the driver. Once, before main() runs trainer_source_start() -
 * from a SYS_INIT at APPLICATION level, as trainer_sim.c does. -EALREADY on a
 * second call, -EINVAL for NULL. */
int trainer_source_register(const struct trainer_source_ops *ops);

/* The registered driver, or NULL. */
const struct trainer_source_ops *trainer_source_get(void);

/* Push a measurement / an event. Any thread. 0, or -EINVAL. */
int trainer_source_report(const struct trainer_report *r);
int trainer_source_report_event(enum trainer_source_event evt);

#ifdef __cplusplus
}
#endif

#endif /* TRAINER_SOURCE_H_ */
