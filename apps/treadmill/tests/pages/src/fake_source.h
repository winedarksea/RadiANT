/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * A fake of treadmill_source_ctrl.h for the motion-path tests: records every
 * call into the source's actuating entry points and answers what a test asks.
 */

#ifndef FAKE_SOURCE_H_
#define FAKE_SOURCE_H_

#include <stdint.h>

struct fake_source {
	int speed_rc, incline_rc, start_rc, pause_rc, resume_rc, stop_rc;
	int speed_calls, incline_calls, start_calls, pause_calls, resume_calls, stop_calls;
	uint32_t last_speed;
	int32_t last_incline;
	/* 1 incline first, 2 speed first, 0 neither: Reset's order. */
	int first;
};

extern struct fake_source fake;

void fake_source_reset(void);

/* Every call that could move the machine, summed. */
int fake_source_motion_calls(void);

#endif /* FAKE_SOURCE_H_ */
