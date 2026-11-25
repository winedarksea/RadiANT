/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_ftms_actuate.c - see treadmill_ftms_actuate.h. Kernel-free.
 */

#include <errno.h>
#include <stddef.h>

#include "treadmill_ftms_actuate.h"

/* -ENOTSUP is the source's "nothing to move here", not a refusal. */
static int accepted_or_rc(int rc)
{
	return (rc == -ENOTSUP) ? 0 : rc;
}

struct treadmill_ftms_outcome
treadmill_ftms_stop(const struct treadmill_ftms_setters *setters)
{
	struct treadmill_ftms_outcome out = { 0 };

	out.rc = accepted_or_rc(setters->set_speed_mm_s(0u, NULL));
	out.go_idle = (out.rc == 0);
	out.notify = (out.rc == 0);
	return out;
}

struct treadmill_ftms_outcome
treadmill_ftms_reset(const struct treadmill_ftms_setters *setters)
{
	struct treadmill_ftms_outcome out = { 0 };
	int incline_rc = accepted_or_rc(setters->set_incline_deci(0, NULL));
	int speed_rc = accepted_or_rc(setters->set_speed_mm_s(0u, NULL));

	out.rc = (incline_rc != 0) ? incline_rc : speed_rc;
	out.go_idle = (speed_rc == 0);
	out.release = (incline_rc == 0 && speed_rc == 0);
	out.notify = out.release;
	return out;
}
