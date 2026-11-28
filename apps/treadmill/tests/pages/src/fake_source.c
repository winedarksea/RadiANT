/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * See fake_source.h.
 */

#include <string.h>

#include "fake_source.h"
#include "treadmill_source_ctrl.h"

struct fake_source fake;

void fake_source_reset(void)
{
	memset(&fake, 0, sizeof(fake));
}

int fake_source_motion_calls(void)
{
	return fake.speed_calls + fake.incline_calls + fake.start_calls + fake.resume_calls;
}

int treadmill_source_set_speed(uint32_t mm_s)
{
	fake.speed_calls++;
	fake.last_speed = mm_s;
	if (fake.first == 0) {
		fake.first = 2;
	}
	return fake.speed_rc;
}

int treadmill_source_set_incline(int32_t centi_pct)
{
	fake.incline_calls++;
	fake.last_incline = centi_pct;
	if (fake.first == 0) {
		fake.first = 1;
	}
	return fake.incline_rc;
}

int treadmill_source_workout_start(void)
{
	fake.start_calls++;
	return fake.start_rc;
}

int treadmill_source_workout_pause(void)
{
	fake.pause_calls++;
	return fake.pause_rc;
}

int treadmill_source_workout_resume(void)
{
	fake.resume_calls++;
	return fake.resume_rc;
}

int treadmill_source_workout_stop(void)
{
	fake.stop_calls++;
	return fake.stop_rc;
}
