/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * One snapshot drives both radios.
 *
 * WHAT IT REPLACED. The beat work handler used to call radiant_profile_hr_*
 * and bt_hrs_notify() directly, and only on a beat: a driver that reported
 * only a rate (the documented hrm_sensor_report_rate() path) updated ANT+
 * byte 7 and never notified BLE; a rate, once reported, never went stale; and
 * the beat path kept a count and one instant, so coalesced beats lost their
 * intervals.
 *
 * WHAT IT IS. A kernel-free state machine fed by the sensor's three reports
 * and read by both radios:
 *
 *   { bpm, bpm deadline, contact, RR baseline, pending RR[] }
 *
 *   - ANT+: hrm_publish_beat() returns how many beats to add to the page-0
 *     accumulators (the count and event time stay frozen without beats), and
 *     hrm_publish_bpm() is byte 7: the rate, or 0 (invalid) when it is stale
 *     or contact is lost.
 *   - BLE: hrm_publish_notify_due() is a 1 Hz cadence while a collector is
 *     subscribed, and hrm_publish_meas() the snapshot plus every RR-Interval
 *     since the last notification. A rate-only driver therefore notifies at
 *     1 Hz with no RR, a beat-only driver notifies with RR, and a driver that
 *     reports both gets both.
 *
 * THE RULES, each of which a test in apps/hrm_ble/tests/publish states:
 *   - Every rate report (a repeat of the same value included) and every beat
 *     refreshes the bpm deadline (CONFIG_HRM_BPM_STALE_MS); when it passes,
 *     bpm is invalid on both radios. A rate of 0 is invalid at once.
 *   - An RR-Interval is reported only between two CONSECUTIVE beats (seq + 1)
 *     with contact, no longer than the stale limit. A seq jump (the ring
 *     overflowed), contact loss, expiry or a longer interval starts a new
 *     baseline instead: a missing interval is legal, a wrong one is not.
 *   - Contact loss invalidates bpm immediately and clears pending RR; a fresh
 *     rate or beat is needed after contact returns. Contact SUPPORT is fixed
 *     at init (the driver's capability) and a report never changes it.
 *   - A disconnect, or no subscriber at a notify tick, clears pending RR, so
 *     a collector never receives intervals from before it subscribed.
 *
 * ONE CONTEXT. Every function here is called from the system workqueue only
 * (the sensor worker and the node tick both run there); nothing is locked.
 */

#ifndef HRM_PUBLISH_H_
#define HRM_PUBLISH_H_

#include <stdbool.h>
#include <stdint.h>

#include <radiant/radiant_ble_hrs.h>

/* Pending RR-Intervals between notifications: 16 covers 4 s at 220 bpm, and
 * more than a 23-byte ATT MTU can carry (9). The oldest are dropped first. */
#define HRM_PUBLISH_RR_MAX 16u

/* The BLE notification period. HRS leaves it to the sensor; 1 s is what
 * collectors expect and is the HRM_BLE connection interval as well. */
#define HRM_PUBLISH_NOTIFY_MS 1000u

struct hrm_publish {
	uint32_t stale_ms;
	bool contact_supported;  /* fixed at init */
	bool contact;            /* detected; always true when unsupported */

	uint8_t bpm;             /* the last reported rate */
	bool armed;              /* the deadline below is running */
	uint32_t deadline_ms;

	uint16_t last_seq;       /* newest beat accounted for */
	bool have_baseline;
	uint16_t baseline_time;  /* 1/1024 s */

	uint16_t rr[HRM_PUBLISH_RR_MAX]; /* oldest first */
	uint8_t rr_count;

	bool notifying;
	uint32_t next_notify_ms;

	/* Diagnostics: beats accounted, and intervals not reported
	 * (overflow, gaps, the pending cap). */
	uint32_t beats;
	uint32_t rr_skipped;
};

void hrm_publish_init(struct hrm_publish *p, bool contact_supported, uint32_t stale_ms);

/* A rate report. 0 is RADIANT_PROFILE_HR_INVALID_BPM: invalid at once. */
void hrm_publish_rate(struct hrm_publish *p, uint8_t bpm, uint32_t now_ms);

/*
 * A beat record from the ring (seq, event_time in 1/1024 s). Returns how many
 * beats to add to the ANT+ accumulators with `event_time` - 1 normally, more
 * after a ring overflow, 0 for a seq already accounted for.
 */
uint16_t hrm_publish_beat(struct hrm_publish *p, uint16_t seq, uint16_t event_time,
			  uint32_t now_ms);

/* A contact report. Ignored when the driver declared no contact support. */
void hrm_publish_contact(struct hrm_publish *p, bool detected, uint32_t now_ms);

/* Byte 7 / the BLE rate: the bpm, or 0 when stale, never reported or no
 * contact. */
uint8_t hrm_publish_bpm(const struct hrm_publish *p, uint32_t now_ms);

/* Housekeeping from the node tick: lets an expired deadline end the RR
 * baseline. Returns true when the rate went invalid on this call. */
bool hrm_publish_tick(struct hrm_publish *p, uint32_t now_ms);

/*
 * The 1 Hz cadence. Returns true when a notification is due. With
 * `subscribed` false it clears pending RR and restarts the cadence, so the
 * next subscriber gets a notification at once and no stale intervals.
 */
bool hrm_publish_notify_due(struct hrm_publish *p, uint32_t now_ms, bool subscribed);

/* The measurement to notify. m->rr points into `p` and stays valid until the
 * next call that changes pending RR. */
void hrm_publish_meas(const struct hrm_publish *p, uint32_t now_ms,
		      struct radiant_ble_hrs_meas *m);

/* Drop pending RR: after a notification, and on a disconnect. */
void hrm_publish_rr_clear(struct hrm_publish *p);

#endif /* HRM_PUBLISH_H_ */
