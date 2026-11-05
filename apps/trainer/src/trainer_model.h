/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_model.h - the simulated rider and road behind apps/trainer.
 *
 * KERNEL-FREE ON PURPOSE, exactly as apps/treadmill's treadmill_state.h is: no
 * Zephyr header, no lock, no clock. It is TOLD how much time passed, so
 * tests/model can drive an hour of riding in a counted loop, and everything
 * that touches it in the application runs on the system workqueue.
 *
 * THE PHYSICS IS FE-C SS8.8.4's, NOT A BICYCLE MODEL. Total resistance is the
 * three terms the profile defines (Equations 8-13 to 8-15):
 *
 *   gravitational = (m_user + m_bike) x 9.81 x grade/100
 *   rolling       = (m_user + m_bike) x 9.81 x Crr
 *   wind          = 0.5 x c_wind x (v + v_wind)^2 x drafting
 *
 * where v_wind is positive for a headwind, and the sum is clamped at 0 N: a
 * trainer brake cannot push the rider downhill. The rider's power is fixed
 * (the shell sets it), so the speed reported is the steady state at which
 * resistance x speed equals that power, found by bisection. That speed is
 * VIRTUAL - page 16 says so - because no wheel is turning anywhere.
 *
 * Three modes, one per control page, and the last command received wins:
 *   SIM    pages 50/51/55 set the road; power is the rider's.
 *   ERG    page 49: the trainer holds power at the target, so the REPORTED
 *          power is the target and speed follows it on the current road.
 *   BASIC  page 48: a fraction of page 54's maximum resistance (2000 N) as a
 *          constant brake force on a flat road.
 */

#ifndef TRAINER_MODEL_H_
#define TRAINER_MODEL_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_fec.h"
#include "profile_fec_tx.h"

#ifdef __cplusplus
extern "C" {
#endif

/* SS8.8.4's defaults, applied whenever a command sends a field's sentinel. */
#define TRAINER_DEFAULT_USER_WEIGHT_10G   7500u /* 75.00 kg */
#define TRAINER_DEFAULT_BIKE_WEIGHT_50G   200u  /* 10.00 kg */
#define TRAINER_DEFAULT_WHEEL_DIAM_CM     70u   /* 0.70 m */
#define TRAINER_DEFAULT_WIND_COEFF_CENTI  51u   /* 0.51 kg/m */
#define TRAINER_DEFAULT_DRAFTING_CENTI    100u  /* 1.00 */
#define TRAINER_DEFAULT_CRR_5E5           80u   /* 0.004 in 5e-5 units */

/* Page 54's declared maximum, and the BASIC mode brake at 100 %. */
#define TRAINER_MAX_RESISTANCE_N          2000u

/* The rider before the shell says otherwise. */
#define TRAINER_DEFAULT_RIDER_POWER_W     200u
#define TRAINER_DEFAULT_RIDER_CADENCE_RPM 90u

/* Bisection bounds and depth. 30 m/s is 108 km/h; 40 halvings of that is far
 * below a millimetre per second, which is the resolution page 16 carries. */
#define TRAINER_SPEED_MAX_MS     30.0f
#define TRAINER_SOLVE_ITERATIONS 40u

enum trainer_mode {
	TRAINER_MODE_SIM = 0,
	TRAINER_MODE_ERG,
	TRAINER_MODE_BASIC
};

struct trainer_model {
	/* The rider. */
	uint16_t rider_power_w;
	uint8_t  cadence_rpm;

	enum trainer_mode mode;
	uint8_t  resistance_half_pct; /* page 48 */
	uint16_t target_power_qw;     /* page 49, 0.25 W */

	/* The road, in the profile's own units, defaults already applied. */
	uint8_t  wind_coeff_centi;
	int16_t  wind_speed_kph; /* positive = headwind */
	uint8_t  drafting_centi;
	int32_t  grade_centi_pct;
	uint8_t  crr_5e5;
	uint16_t user_weight_10g;
	uint16_t bike_weight_50g;
	uint8_t  wheel_diameter_cm;

	/* What the last tick produced. */
	uint32_t speed_mm_s;
	uint16_t power_w;      /* reported, page 25 */
	float    resistance_n; /* total, at speed_mm_s */
	bool     erg_speed_low;

	/* Accumulators, advanced once per tick (one page 25 update). */
	uint8_t  event_count;
	uint16_t acc_power_w;
	uint32_t elapsed_ms;
	uint64_t distance_um;
};

void trainer_model_init(struct trainer_model *m);

void trainer_model_set_rider(struct trainer_model *m, uint16_t power_w,
			     uint8_t cadence_rpm);

/* One per control page. 48 selects BASIC, 49 ERG, and the road pages 50 and
 * 51 select SIM, because sending them is how a controller says it is
 * simulating. Page 55 changes no mode: the rider's mass matters in all three.
 * Fields carrying their sentinel take the default. */
void trainer_model_basic_resistance(struct trainer_model *m,
				    uint8_t resistance_half_pct);
void trainer_model_target_power(struct trainer_model *m,
				uint16_t power_quarter_w);
void trainer_model_wind(struct trainer_model *m,
			const struct profile_fec_cmd_wind_resistance *w);
void trainer_model_track(struct trainer_model *m,
			 const struct profile_fec_cmd_track_resistance *t);
void trainer_model_user(struct trainer_model *m,
			const struct profile_fec_cmd_user_config *u);

/* Total resistance in newtons at `speed_ms` on the current road, clamped at
 * zero. BASIC mode is a flat road plus the brake. */
float trainer_model_resistance_n(const struct trainer_model *m, float speed_ms);

/* The steady speed at which resistance x speed = `power_w`. Returns
 * TRAINER_SPEED_MAX_MS if even that is not enough resistance. */
float trainer_model_solve_speed(const struct trainer_model *m, float power_w);

/* Advance by `dt_ms`: solve the speed, bump the page 25 accumulators once. */
void trainer_model_tick(struct trainer_model *m, uint32_t dt_ms);

/* The pages, from the model. Plain fills; the caller hands them to
 * profile_fec_tx's setters. Equipment type 25, speed flagged virtual, the
 * incline equal to the commanded grade (SS8.8.4.1), the page 17 resistance
 * as a share of page 54's maximum. */
void trainer_model_pages(const struct trainer_model *m,
			 struct profile_fec_general *g,
			 struct profile_fec_settings *s,
			 struct profile_fec_trainer *t);

/* The simulation settings as pages 50/51 echo them. */
void trainer_model_echo(const struct trainer_model *m,
			struct profile_fec_cmd_wind_resistance *w,
			struct profile_fec_cmd_track_resistance *t);

#ifdef __cplusplus
}
#endif

#endif /* TRAINER_MODEL_H_ */
