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
 *   { reported bpm, freshness, deadline, contact, RR baseline, recent RR,
 *     pending RR[] }
 *
 *   - ANT+: hrm_publish_beat() returns how many beats to add to the page-0
 *     accumulators (the count and event time stay frozen without beats), and
 *     hrm_publish_bpm() is byte 7: a valid rate, or 0.
 *   - BLE: hrm_publish_ble_step() is the node tick's whole BLE decision - a
 *     dropped link clears pending RR, nobody subscribed clears it too, and
 *     while a collector is subscribed a measurement is due once a second.
 *     hrm_publish_rr_consume() then removes exactly the intervals a
 *     successful notification carried.
 *
 * THE RULES, each stated by a test in apps/hrm_ble/tests/publish:
 *   - Every rate report (a repeat of the same value included) and every beat
 *     refreshes the deadline (CONFIG_HRM_BPM_STALE_MS); when it passes, bpm is
 *     invalid on both radios. A rate of 0 is invalid at once.
 *     The deadline is checked on every report as well as on the tick, in
 *     DELIVERY time: a report arriving after it is a gap even if no tick ran
 *     in between - and so is a worker stall longer than the stale limit,
 *     which costs the first coalesced interval and the reported rate.
 *   - AFTER A GAP (contact loss or expiry) the old reported rate is never
 *     revived. Until a fresh rate report arrives, bpm is derived from the
 *     snapshot's own recent valid RR-Intervals (60 * 1024 / their mean), and
 *     is 0 until there is at least one. A fresh report overrides it.
 *   - An RR-Interval is reported only between two CONSECUTIVE beats (seq + 1)
 *     with contact, no longer than the stale limit in 1/1024 s, and with the
 *     delivery times (ms, no wrap) close enough that the 64 s wrap of the
 *     event times cannot alias a long pause into a plausible interval. A seq
 *     jump (the ring overflowed), contact loss, expiry or a longer interval
 *     starts a new baseline instead: a missing interval is legal, a wrong
 *     one is not.
 *   - Contact loss invalidates bpm immediately and clears pending RR. A beat
 *     delivered without contact counts on ANT+ and does nothing else. Contact
 *     SUPPORT is fixed at init (the driver's capability).
 *   - Pending RR follow HRS 1.0: the oldest that fit a notification are sent,
 *     the rest wait for the next. Only what a successful notification carried
 *     is removed. The buffer drops its OLDEST entry when full - the one legal
 *     loss. A disconnect, or no subscriber at a tick, clears it, so a
 *     collector never receives intervals from before it subscribed.
 *
 * ONE CONTEXT. Every function here is called from the system workqueue only
 * (the sensor worker and the node tick both run there); nothing is locked.
 */

#ifndef HRM_PUBLISH_H_
#define HRM_PUBLISH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant/radiant_ble_hrs.h>

/* Pending RR-Intervals between notifications: 16 covers 4 s at 220 bpm, and
 * more than a 23-byte ATT MTU can carry (9). The oldest are dropped first. */
#define HRM_PUBLISH_RR_MAX 16u

/* Recent valid RR-Intervals kept for a derived rate. */
#define HRM_PUBLISH_DERIVE_N 4u

/* The BLE notification period. HRS leaves it to the sensor; 1 s is what
 * collectors expect and is the HRM_BLE connection interval as well. */
#define HRM_PUBLISH_NOTIFY_MS 1000u

struct hrm_publish {
	uint32_t stale_ms;
	bool contact_supported;  /* fixed at init */
	bool contact;            /* detected; always true when unsupported */

	uint8_t bpm;             /* the last reported rate */
	bool rate_fresh;         /* reported since the last gap */
	bool armed;              /* the deadline below is running */
	uint32_t deadline_ms;

	uint16_t last_seq;       /* newest beat accounted for */
	bool have_baseline;
	uint16_t baseline_time;  /* 1/1024 s */
	uint32_t baseline_ms;    /* delivery time of the baseline beat */

	uint16_t recent[HRM_PUBLISH_DERIVE_N]; /* valid RR, ring, for derive */
	uint8_t recent_count;
	uint8_t recent_next;

	uint16_t rr[HRM_PUBLISH_RR_MAX]; /* pending, oldest first */
	uint8_t rr_count;

	bool notifying;
	uint32_t next_notify_ms;

	/* Diagnostics: beats accounted, and intervals not reported
	 * (overflow, gaps, the pending cap). */
	uint32_t beats;
	uint32_t rr_skipped;
};

void hrm_publish_init(struct hrm_publish *p, bool contact_supported, uint32_t stale_ms);

/* A rate report. 0 is RADIANT_PROFILE_HR_INVALID_BPM: invalid at once. Without
 * contact it is ignored - a strap off the chest has no rate to report. */
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

/* Byte 7 / the BLE rate: the fresh reported rate, else the rate derived from
 * recent RR-Intervals, else 0 - and 0 when stale or without contact. */
uint8_t hrm_publish_bpm(const struct hrm_publish *p, uint32_t now_ms);

/* Housekeeping from the node tick: an expired deadline is a gap. Returns true
 * when the rate went invalid on this call. */
bool hrm_publish_tick(struct hrm_publish *p, uint32_t now_ms);

/*
 * The node tick's BLE decision, kernel-free. `disconnected`: a link dropped
 * since the last tick (clears pending RR). `subscribed`: a collector has
 * notifications on (without one, pending RR is cleared and the cadence
 * restarts, so the next subscriber is notified at once). Returns true when a
 * measurement is due, with *m filled; the caller notifies it and, on success,
 * calls hrm_publish_rr_consume() with the number of intervals sent.
 */
bool hrm_publish_ble_step(struct hrm_publish *p, uint32_t now_ms, bool subscribed,
			  bool disconnected, struct radiant_ble_hrs_meas *m);

/* The measurement as it stands. m->rr points into `p` and stays valid until
 * the next call that changes pending RR. */
void hrm_publish_meas(const struct hrm_publish *p, uint32_t now_ms,
		      struct radiant_ble_hrs_meas *m);

/* Remove the `n` oldest pending RR - those a notification carried. */
void hrm_publish_rr_consume(struct hrm_publish *p, size_t n);

/* Drop all pending RR: on a disconnect, and with nobody subscribed. */
void hrm_publish_rr_clear(struct hrm_publish *p);

#endif /* HRM_PUBLISH_H_ */
