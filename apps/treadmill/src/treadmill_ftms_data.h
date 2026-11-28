/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_ftms_data.h - the FTMS Treadmill Data record (0x2ACD), and the
 * baseline that lets FTMS Reset zero what FTMS reports without touching ANT+.
 *
 * §4.16.2 RESET, BOTH WAYS. FTMS Reset sets the time fields (and, here, every
 * accumulated field of the record: distance, elevation gains, elapsed time)
 * to 0. The ANT+ side's accumulators must NOT move - a head unit tracking the
 * same run would see its distance go backwards. So Reset captures a baseline
 * (the accumulators' values at that instant) and this encoder reports
 * `value - baseline`. One baseline, not one per client: Reset is a machine
 * procedure whose status goes to every client, and with CONFIG_BT_MAX_CONN=1
 * there is one client anyway. If an accumulator falls below its baseline (the
 * session itself restarted underneath), that field re-bases to zero.
 *
 * Record layout and traps (Table 4.5): bit 0 "More Data" is INVERTED (clear
 * means Instantaneous Speed present); bit 3 covers Inclination AND Ramp Angle;
 * bit 4 covers the positive/negative elevation PAIR. Without a heart-rate
 * receiver the HR field and flag are absent, matching the derived feature bit.
 *
 *   flags 2, speed 2, total distance 3, inclination 2, ramp angle 2,
 *   +elevation 2, -elevation 2, [heart rate 1], elapsed time 2   = 17 or 18
 *
 * Kernel-free.
 */

#ifndef TREADMILL_FTMS_DATA_H_
#define TREADMILL_FTMS_DATA_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "treadmill_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The record never needs FTMS §4.19's multi-notification split: it fits the
 * 20-byte payload of a default 23-byte ATT_MTU. */
#define TREADMILL_FTMS_DATA_MAX 18u

struct treadmill_ftms_baseline {
	uint64_t distance_mm;
	uint32_t elapsed_ms;
	uint32_t pos_vertical_mm;
	uint32_t neg_vertical_mm;
};

/* Zero the FTMS view: the baseline becomes the accumulators now. */
void treadmill_ftms_baseline_capture(struct treadmill_ftms_baseline *b,
				     const struct treadmill_state *s);

/* Encode one record. `b` may be re-based (see the header). Returns the length,
 * or -ENOMEM when cap cannot hold it. */
int treadmill_ftms_data_encode(const struct treadmill_state *s,
			       struct treadmill_ftms_baseline *b, bool heart_rate,
			       uint8_t *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif /* TREADMILL_FTMS_DATA_H_ */
