/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * FTMS Stop and Reset when an actuator refuses (treadmill_ftms_actuate.c).
 *
 * Both procedures used to discard the setters' answers: a belt that refused
 * speed 0 was still reported stopped, Training Status still went Idle, and
 * Reset still released control and notified RESET. The fake setters below
 * answer whatever rc a test asks for, so each refusal can be driven without
 * a source or a stack.
 */

#include <errno.h>
#include <stddef.h>

#include <zephyr/ztest.h>

#include "treadmill_ftms_actuate.h"

static int speed_rc;
static int incline_rc;
static int speed_calls;
static int incline_calls;
static uint32_t last_speed;
static int16_t last_incline;
/* 1 = incline was called first, 2 = speed was called first, 0 = neither. */
static int first_called;

static int fake_set_speed(uint32_t mm_s, uint32_t *adopted_mm_s)
{
	speed_calls++;
	last_speed = mm_s;
	if (first_called == 0) {
		first_called = 2;
	}
	if (adopted_mm_s != NULL && speed_rc == 0) {
		*adopted_mm_s = mm_s;
	}
	return speed_rc;
}

static int fake_set_incline(int16_t deci_pct, int32_t *adopted_centi)
{
	incline_calls++;
	last_incline = deci_pct;
	if (first_called == 0) {
		first_called = 1;
	}
	if (adopted_centi != NULL && incline_rc == 0) {
		*adopted_centi = deci_pct * 10;
	}
	return incline_rc;
}

static const struct treadmill_ftms_setters fake = {
	.set_speed_mm_s = fake_set_speed,
	.set_incline_deci = fake_set_incline,
};

static void reset_fake(void *fixture)
{
	ARG_UNUSED(fixture);
	speed_rc = 0;
	incline_rc = 0;
	speed_calls = 0;
	incline_calls = 0;
	last_speed = 0xFFFFFFFFu;
	last_incline = INT16_MAX;
	first_called = 0;
}

ZTEST(treadmill_ftms_actuate, test_stop_success_is_unchanged)
{
	struct treadmill_ftms_outcome out = treadmill_ftms_stop(&fake);

	zassert_equal(out.rc, 0, NULL);
	zassert_true(out.go_idle, NULL);
	zassert_true(out.notify, NULL);
	zassert_false(out.release, "Stop never touches control");
	zassert_equal(speed_calls, 1, NULL);
	zassert_equal(last_speed, 0u, NULL);
	zassert_equal(incline_calls, 0, "Stop does not move the deck");
}

ZTEST(treadmill_ftms_actuate, test_stop_rejected_by_the_belt_fails)
{
	struct treadmill_ftms_outcome out;

	speed_rc = -EIO;
	out = treadmill_ftms_stop(&fake);

	zassert_equal(out.rc, -EIO, "the refusal must reach the client");
	zassert_false(out.go_idle, "a belt still moving is not Idle");
	zassert_false(out.notify, "no STOPPED notification for a refusal");
	zassert_false(out.release, NULL);
}

ZTEST(treadmill_ftms_actuate, test_reset_success_is_unchanged)
{
	struct treadmill_ftms_outcome out = treadmill_ftms_reset(&fake);

	zassert_equal(out.rc, 0, NULL);
	zassert_true(out.go_idle, NULL);
	zassert_true(out.release, NULL);
	zassert_true(out.notify, NULL);
	zassert_equal(incline_calls, 1, NULL);
	zassert_equal(speed_calls, 1, NULL);
	zassert_equal(last_incline, 0, NULL);
	zassert_equal(last_speed, 0u, NULL);
	zassert_equal(first_called, 1, "incline first, as before");
}

ZTEST(treadmill_ftms_actuate, test_reset_incline_refused_speed_accepted)
{
	struct treadmill_ftms_outcome out;

	incline_rc = -EIO;
	out = treadmill_ftms_reset(&fake);

	zassert_equal(out.rc, -EIO, "Reset is answered with a failure");
	zassert_equal(speed_calls, 1, "a refused incline still stops the belt");
	zassert_true(out.go_idle, "speed 0 was accepted, so Idle");
	zassert_false(out.release, "control is kept on a partial Reset");
	zassert_false(out.notify, "no RESET notification on a partial Reset");
}

ZTEST(treadmill_ftms_actuate, test_reset_speed_refused_incline_accepted)
{
	struct treadmill_ftms_outcome out;

	speed_rc = -EBUSY;
	out = treadmill_ftms_reset(&fake);

	zassert_equal(out.rc, -EBUSY, NULL);
	zassert_equal(incline_calls, 1, NULL);
	zassert_false(out.go_idle, "a belt still moving is not Idle");
	zassert_false(out.release, NULL);
	zassert_false(out.notify, NULL);
}

ZTEST(treadmill_ftms_actuate, test_reset_both_refused_reports_the_first)
{
	struct treadmill_ftms_outcome out;

	incline_rc = -EIO;
	speed_rc = -EBUSY;
	out = treadmill_ftms_reset(&fake);

	zassert_equal(out.rc, -EIO, NULL);
	zassert_false(out.go_idle, NULL);
	zassert_false(out.release, NULL);
	zassert_false(out.notify, NULL);
}

/*
 * -ENOTSUP is treadmill_source.h's "this machine cannot do that", a
 * first-class answer and not a failure. A fixed-deck treadmill must still be
 * able to Reset, and a self-paced belt to Stop, as they always could.
 */
ZTEST(treadmill_ftms_actuate, test_unsupported_actuators_do_not_fail)
{
	struct treadmill_ftms_outcome out;

	incline_rc = -ENOTSUP;
	out = treadmill_ftms_reset(&fake);
	zassert_equal(out.rc, 0, "fixed deck: Reset still succeeds");
	zassert_true(out.go_idle, NULL);
	zassert_true(out.release, NULL);
	zassert_true(out.notify, NULL);

	speed_rc = -ENOTSUP;
	out = treadmill_ftms_stop(&fake);
	zassert_equal(out.rc, 0, "self-paced belt: Stop still succeeds");
	zassert_true(out.go_idle, NULL);
	zassert_true(out.notify, NULL);
}

ZTEST_SUITE(treadmill_ftms_actuate, NULL, NULL, reset_fake, NULL, NULL);
