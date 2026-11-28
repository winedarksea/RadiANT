/* SPDX-License-Identifier: Apache-2.0 */
#ifndef SMART_TRAINER_TEST_FAKE_H_
#define SMART_TRAINER_TEST_FAKE_H_

#include "trainer_source.h"

struct fake_state {
	int rc;
	int n_power, n_resistance, n_sim, n_user, n_workout;
	uint16_t power_w;
	uint16_t permille;
	struct trainer_sim_params sim;
	struct trainer_user user;
	enum trainer_workout workout;
};

extern struct fake_state fake;
extern const struct trainer_source_ops fake_full;
extern const struct trainer_source_ops fake_erg_only;

void fake_reset(void);

#endif
