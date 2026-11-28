/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_sim_model.h - the seam (trainer_source.h) in front of apps/trainer's
 * physics model (trainer_model.h), which is REUSED, not copied: CMakeLists.txt
 * compiles ../trainer/src/trainer_model.c into this application.
 *
 * Kernel-free, so tests/glue checks every unit conversion between the seam and
 * the model. trainer_sim.c is the thin kernel half: a timer that ticks this
 * and pushes the report.
 */
#ifndef TRAINER_SIM_MODEL_H_
#define TRAINER_SIM_MODEL_H_

#include <stdbool.h>
#include <stdint.h>

#include "trainer_model.h"
#include "trainer_source.h"

#ifdef __cplusplus
extern "C" {
#endif

struct trainer_sim_model {
	struct trainer_model model;
	/* The simulated rider follows the workout: Stop/Pause and the rider
	 * stops pedalling; Start/Resume and the rider resumes at this cadence. */
	uint8_t rider_cadence_rpm;
	bool    rider_on;
};

void trainer_sim_model_init(struct trainer_sim_model *s);

int trainer_sim_model_set_target_power(struct trainer_sim_model *s, uint16_t watts);
int trainer_sim_model_set_resistance(struct trainer_sim_model *s, uint16_t permille);
int trainer_sim_model_set_sim(struct trainer_sim_model *s, const struct trainer_sim_params *p);
int trainer_sim_model_set_user(struct trainer_sim_model *s, const struct trainer_user *u);

/* Applies the workout to the simulated rider. Fills *evt with the session
 * event the trainer now owes (a real driver would report it once the brake
 * had done it). */
int trainer_sim_model_workout(struct trainer_sim_model *s, enum trainer_workout w,
			      enum trainer_source_event *evt);

/* The rider, from a shell or a button: power W, cadence rpm (0 = stopped). */
void trainer_sim_model_set_rider(struct trainer_sim_model *s, uint16_t power_w,
				 uint8_t cadence_rpm);

/* Advance the model and produce the report. */
void trainer_sim_model_tick(struct trainer_sim_model *s, uint32_t dt_ms,
			    struct trainer_report *out);

#ifdef __cplusplus
}
#endif

#endif /* TRAINER_SIM_MODEL_H_ */
