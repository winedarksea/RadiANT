/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * One pass of the sensor worker, kernel-free: the ORDER in which a pass hands
 * the application what the sensor reported since the last one.
 *
 * WHY THE ORDER IS THE WHOLE POINT. A pass sees three things at once - the
 * newest rate, the beats in the ring, and contact (a level plus a loss
 * counter) - but not when each happened relative to the others. The order
 * decides what a beat drained in this pass is believed to be:
 *
 *   1. a loss not seen before goes FIRST, so every beat drained in this pass
 *      goes in without contact - counted on ANT+, no RR, no refresh. Beats
 *      that landed while the strap was off the skin are noise, and in a pass
 *      that contains a loss nobody can tell which beats those were;
 *   2. then the newest rate (ignored by the snapshot while contact is lost);
 *   3. then every beat;
 *   4. then a regain, read LIVE after the drain. A pass that starts with
 *      contact lost therefore drains with contact lost, even when the regain
 *      arrived before the pass ran: the noise beats of the race "loss in pass
 *      N, noise while off, regain before pass N+1" cannot produce intervals
 *      or refresh the rate. A loss seen only in the level is delivered here
 *      too, at once.
 *
 * Conservative in every case: a missing interval is legal, a wrong one is not.
 * A rate reported together with a regain is ignored until the driver's next
 * report.
 *
 * apps/hrm_ble/tests/publish runs it against fake ops and against the real
 * ring and snapshot.
 */

#ifndef HRM_SENSOR_PASS_H_
#define HRM_SENSOR_PASS_H_

#include <stdbool.h>
#include <stdint.h>

struct hrm_pass_state {
	uint32_t rate_seq; /* report counter of the last rate delivered */
	uint32_t losses;   /* loss counter last seen */
	bool contact;      /* contact as last delivered */
};

struct hrm_pass_ops {
	void *ctx;
	bool (*level)(void *ctx);             /* the live contact level */
	void (*rate)(void *ctx, uint8_t bpm);
	void (*drain)(void *ctx);             /* deliver every beat record */
	void (*contact)(void *ctx, bool detected);
};

/* Contact starts "detected", as the snapshot does. */
void hrm_pass_init(struct hrm_pass_state *s);

/*
 * One pass. `rate_word` is (report counter << 8) | bpm, `losses` the loss
 * counter, both read at the start of the pass.
 */
void hrm_pass_run(struct hrm_pass_state *s, uint32_t rate_word, uint32_t losses,
		  const struct hrm_pass_ops *ops);

#endif /* HRM_SENSOR_PASS_H_ */
