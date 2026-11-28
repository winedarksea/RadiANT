/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_source_ctrl.h - THE SOURCE'S ACTUATING ENTRY POINTS, and the one
 * file allowed to include this header is src/treadmill_cmd.c (plus
 * treadmill_source.c, which implements them, and the tests' fake source).
 *
 * Everything declared here can start the belt, change its speed or move the
 * deck. treadmill_cmd.c calls treadmill_motion_permitted() before every one of
 * them; any other caller would be a path around that gate. `git grep
 * treadmill_source_ctrl.h` must list exactly those three files.
 *
 * Each forwards the source's answer unchanged (0, -ENOTSUP or another negative
 * errno - see treadmill_source_api) and returns -ENODEV when nothing is
 * registered. Thread context: the system workqueue.
 */

#ifndef TREADMILL_SOURCE_CTRL_H_
#define TREADMILL_SOURCE_CTRL_H_

#include <stdbool.h>
#include <stdint.h>

int treadmill_source_set_incline(int32_t centi_pct);
int treadmill_source_set_speed(uint32_t mm_s);

/* NULL hooks answer -ENOTSUP, except workout_stop, which falls back to
 * set_speed(0) (and answers -ENOTSUP only if that is absent too). */
int treadmill_source_workout_start(void);
int treadmill_source_workout_pause(void);
int treadmill_source_workout_resume(void);
int treadmill_source_workout_stop(void);

/* Condition (a) of the motion rule. False when nothing is registered or the
 * source does not implement remote_motion_allowed. */
bool treadmill_source_remote_motion_allowed(void);

#endif /* TREADMILL_SOURCE_CTRL_H_ */
