/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_ftms_actuate.h - WHAT FTMS STOP AND RESET DO WHEN THE DECK SAYS NO.
 *
 * FTMS Stop (0x08 with parameter 0x01) and Reset (0x01) both command the
 * actuators to zero and then change what the client is told: Training Status,
 * a Machine Status notification and, for Reset, control itself. They used to
 * discard the actuators' answers and report SUCCESS whatever happened, so a
 * client was told the belt had stopped when the source had refused to stop
 * it. These two functions make the decision from the answers instead.
 *
 * Same rule as radiant_ctrl_arbiter.c: no Zephyr header, no lock, no clock, and
 * the actuators come in as function pointers. That is what lets
 * apps/treadmill/tests/pages drive the decision with a fake that fails on
 * demand; nothing tests treadmill_ble.c itself.
 *
 * -ENOTSUP IS NOT A FAILURE HERE, matching treadmill_source.h's contract: a
 * machine with a fixed deck or a self-paced belt has nothing to move, so a
 * Reset or Stop on it is not refused for that. Any other non-zero answer is.
 */

#ifndef TREADMILL_FTMS_ACTUATE_H_
#define TREADMILL_FTMS_ACTUATE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The two setters, shaped like treadmill_node_set_speed_mm_s() and
 * treadmill_node_set_incline_deci(). Either answers 0 or a negative errno. */
struct treadmill_ftms_setters {
	int (*set_speed_mm_s)(uint32_t mm_s, uint32_t *adopted_mm_s);
	int (*set_incline_deci)(int16_t deci_pct, int32_t *adopted_centi);
};

struct treadmill_ftms_outcome {
	/* 0 if every setter accepted (or answered -ENOTSUP), otherwise the
	 * first failure. Non-zero means the procedure is answered with a
	 * failure result code. */
	int  rc;
	/* Set Training Status to Idle and clear any pause. Only when speed 0
	 * was accepted: a belt still moving is not idle. */
	bool go_idle;
	/* Revoke the FTMS controller and release the control token (Reset
	 * only), and only when both setters accepted. */
	bool release;
	/* Send the procedure's Machine Status notification. Only when the
	 * procedure succeeded as a whole. */
	bool notify;
};

/* FTMS Stop: speed to 0. On failure, nothing changes and nothing is sent. */
struct treadmill_ftms_outcome
treadmill_ftms_stop(const struct treadmill_ftms_setters *setters);

/* FTMS Reset: incline to 0, then speed to 0. Both are always attempted, so a
 * refused incline still stops the belt. */
struct treadmill_ftms_outcome
treadmill_ftms_reset(const struct treadmill_ftms_setters *setters);

#ifdef __cplusplus
}
#endif

#endif /* TREADMILL_FTMS_ACTUATE_H_ */
