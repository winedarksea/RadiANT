/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The fake source the command tests drive, shared by the suites. Each hook
 * counts its calls, keeps its last argument and returns `fake.rc`.
 */

#include <string.h>

#include "fake.h"

struct fake_state fake;

static int f_power(uint16_t w)
{
	fake.n_power++;
	fake.power_w = w;
	return fake.rc;
}

static int f_resistance(uint16_t pm)
{
	fake.n_resistance++;
	fake.permille = pm;
	return fake.rc;
}

static int f_sim(const struct trainer_sim_params *p)
{
	fake.n_sim++;
	fake.sim = *p;
	return fake.rc;
}

static int f_user(const struct trainer_user *u)
{
	fake.n_user++;
	fake.user = *u;
	return fake.rc;
}

static int f_workout(enum trainer_workout w)
{
	fake.n_workout++;
	fake.workout = w;
	return fake.rc;
}

const struct trainer_source_ops fake_full = {
	.name = "fake",
	.set_target_power = f_power,
	.set_resistance = f_resistance,
	.set_sim = f_sim,
	.set_user = f_user,
	.workout = f_workout,
	.max_resistance_n = 2000u,
};

/* ERG only: no resistance, no simulation, no user, no workout. */
const struct trainer_source_ops fake_erg_only = {
	.name = "fake erg",
	.set_target_power = f_power,
	.max_resistance_n = 0u,
};

void fake_reset(void)
{
	memset(&fake, 0, sizeof(fake));
}
