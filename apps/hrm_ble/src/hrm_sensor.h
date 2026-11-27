/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The heart-rate source seam: the one boundary a manufacturer implements to
 * turn this reference design into their product.
 *
 * ─── Why this shape, and not the two obvious alternatives ──────────────────
 *
 * NOT ZEPHYR'S `sensor` API. `zephyr/drivers/sensor.h` has no heart-rate
 * channel and no beat trigger, so a heart-rate driver has to invent an unnamed
 * `SENSOR_CHAN_PRIV_START + n` - a smell, but not why it was rejected: the
 * sensor API is a POLL-A-SCALAR model and a beat is an EDGE. A sensor-API
 * strap re-derives the beat instant from whatever tick happened to do the
 * fetch, which is structurally the same mistake that produced the
 * 72-versus-66.7 bpm drift bug this application already shipped once (see
 * src/hrm_sim_sched.c).
 *
 * NOT A WEAK SYMBOL. A weak default that a manufacturer overrides is one
 * signature typo away from silently keeping the default - which here means a
 * shipped chest strap reporting a constant simulated 72 bpm. Registration plus
 * a Kconfig `choice` makes the mistake unrepresentable instead: exactly one arm
 * is compiled, and CONFIG_HRM_SENSOR_CUSTOM has no fallback at all.
 *
 * PUSH, NOT PULL, and the direction IS the design. A real detector is an ISR on
 * an analogue front end's comparator output; it knows when the beat happened.
 * An application tick does not, and cannot recover it. So the driver hands over
 * the hardware-captured instant and the application never guesses one.
 *
 * ─── Why the seam is here and not lower ────────────────────────────────────
 *
 * `radiant_profile_hr_beat(&hr, event_time)` plus `radiant_profile_hr_set_computed(&hr, bpm)`
 * is precisely what ANT+ page 0x00 carries: an event count and an event time at
 * 1/1024 s, plus the sensor's own computed rate. A seam that did not match the
 * wire format would make every integrator write the same adapter to get back to
 * it.
 *
 * ─── The concurrency detail that must not be got wrong ─────────────────────
 *
 * `radiant_profile_hr` is ALREADY reached from two contexts in this application:
 *
 *   1. the system workqueue, which advances the beat accumulators, and
 *   2. the radiant callback path - radiant_ant_on_message() -> load_next_page() ->
 *      radiant_profile_hr_next() - which reads them to build the next page.
 *
 * An ISR-driven sensor would make that three-way, and radiant_profile_hr has no lock.
 * So hrm_sensor_report_beat() does NOT touch radiant_profile_hr. It pushes one
 * `{seq, event_time}` record into a lock-free single-producer/single-consumer
 * ring (src/hrm_beat_ring.h) and submits one work item; the handler runs on
 * the system workqueue - context 1, which already existed - drains EVERY
 * record, and hands each to the application, which updates one snapshot
 * (src/hrm_publish.h) that both radios read: the ANT+ accumulators and byte 7,
 * and the BLE Heart Rate Measurement with its RR-Intervals. Timestamp fidelity
 * is preserved because each instant travels in its own record; the profile's
 * context count stays at two.
 *
 * Two contexts with no lock is safe one way round only: the workqueue is
 * cooperative, so the event thread never interrupts a writer - but the
 * workqueue CAN preempt the event thread mid-encode. So the reader,
 * load_next_page(), wraps radiant_profile_hr_next() in k_sched_lock().
 *
 * The ring is single-producer by construction: exactly one sensor is
 * registered, so exactly one context calls report_beat(). Do not call it from
 * two. report_rate() and report_contact() are safe from any context.
 */

#ifndef HRM_SENSOR_H_
#define HRM_SENSOR_H_

#include <stdbool.h>
#include <stdint.h>

struct hrm_sensor;

struct hrm_sensor_api {
	/*
	 * Bring the source up. Required. Called once from main(), between
	 * radiant_profile_hr_init() and the ANT+ channel opening, so a source that
	 * needs the radio quiet has it, and a source that fails stops the node
	 * before it ever claims to be transmitting.
	 *
	 * Return 0 or a negative errno; a failure is logged and the node keeps
	 * running with no beats, which reads as RADIANT_PROFILE_HR_INVALID_BPM on the
	 * air.
	 */
	int (*start)(const struct hrm_sensor *sensor);

	/* Optional. Nothing in this application calls it yet - it exists so a
	 * source with a power state has somewhere to put it, and so that a
	 * future suspend path does not have to change this struct. */
	int (*stop)(const struct hrm_sensor *sensor);

	/*
	 * Optional, called on the node's 100 ms tick. This is where contact
	 * detection, AGC and battery sampling belong - the slow, level-valued
	 * things. It is explicitly NOT where beats come from: a beat detected
	 * on this tick would carry the tick's timestamp, which is the drift bug
	 * again.
	 */
	void (*poll)(const struct hrm_sensor *sensor);
};

struct hrm_sensor {
	/*
	 * Logged at boot, verbatim. A manufacturer reads their own driver's
	 * name there, or the reference simulator's, and there is deliberately
	 * no third answer.
	 */
	const char *name;
	const struct hrm_sensor_api *api;
	void *data;
	/*
	 * The source can detect skin contact and will call
	 * hrm_sensor_report_contact(). A CAPABILITY, fixed at registration:
	 * it becomes the BLE Measurement's "contact supported" bit, which a
	 * collector expects never to change during a connection. Contact
	 * STATUS is what report_contact() reports, live.
	 */
	bool contact_supported;
};

/*
 * Register the one source. Call before main()'s hrm_sensor_start() - a
 * SYS_INIT() at APPLICATION level, or the source's own init hook.
 *
 * EXACTLY ONE WINS. A second call returns -EEXIST and logs at error level
 * rather than replacing the first: two sources reporting beats would violate
 * the single-producer rule the beat ring depends on.
 *
 * Returns 0, -EINVAL for a sensor with no api->start, or -EEXIST.
 */
int hrm_sensor_register(const struct hrm_sensor *sensor);

/* The registered source's name, or NULL when nothing registered. */
const char *hrm_sensor_name(void);

/*
 * Start the registered source. Returns -ENODEV when nothing registered, which
 * under CONFIG_HRM_SENSOR_CUSTOM is the expected failure for an integrator who
 * has not written their driver yet - see that symbol's help for why it is not a
 * fallback.
 */
int hrm_sensor_start(void);

/* Stop the registered source, if it has a stop. */
int hrm_sensor_stop(void);

/* Run the source's optional poll. Called from the node's 100 ms tick. */
void hrm_sensor_poll(void);

/*
 * ONE BEAT, at the instant it happened.
 *
 * `event_time_1024` is the beat's ABSOLUTE time on the node's 1/1024 s clock,
 * wrapping at 16 bits - not "now", and not an interval. Use
 * hrm_sensor_now_1024() if your hardware gave you no better instant, but
 * understand that doing so is what the push direction exists to avoid.
 *
 * ISR-SAFE BY CONSTRUCTION: one push into the lock-free beat ring and one
 * k_work_submit(). It does not touch radiant_profile_hr or the BLE stack. See
 * the header comment for why that matters. Every beat reaches the ANT+ event
 * count; its interval reaches BLE as an RR-Interval unless more than
 * HRM_BEAT_RING_LEN beats pile up before the workqueue runs, in which case
 * those intervals are dropped (never mis-stated) and the count stays exact.
 */
void hrm_sensor_report_beat(uint16_t event_time_1024);

/*
 * The source's own computed rate - byte 7 of ANT+ pages 0x00 and 0x04, and the
 * BLE Heart Rate Measurement value.
 *
 * RADIANT_PROFILE_HR_INVALID_BPM (0) means "no reading". 0xFF IS A REAL 255
 * bpm ON THIS PROFILE and is not a sentinel; do not use it to mean "unknown".
 *
 * A RATE GOES STALE. Each report - a repeat of the same value included - and
 * each beat keeps it valid for CONFIG_HRM_BPM_STALE_MS; after that both radios
 * send 0 until the next report. Report it at least that often while you have
 * a reading.
 *
 * Safe from any context, ISR included.
 */
void hrm_sensor_report_rate(uint8_t bpm);

/*
 * Skin contact STATUS, for a source that declared `contact_supported`.
 * Report it on every change (repeats are harmless).
 *
 * Losing contact invalidates the rate on both radios at once and discards any
 * RR-Intervals not yet notified; after contact returns, the rate is valid
 * again from the next report_rate() or beat. Ignored for a source without
 * contact support - the BLE capability bit never changes.
 *
 * Safe from any context, ISR included; a loss followed by a regain before the
 * workqueue runs is still seen as a loss.
 */
void hrm_sensor_report_contact(bool detected);

/* The registered source's `contact_supported`; false when none registered. */
bool hrm_sensor_contact_supported(void);

/*
 * The node's 1/1024 s clock, exported so a driver can timestamp against exactly
 * the base the ANT+ page carries. A driver that timestamps against its own
 * clock and converts is one rounding rule away from a receiver computing a
 * different rate than the one on byte 7.
 */
uint16_t hrm_sensor_now_1024(void);

/*
 * Implemented by the application (src/main.c), called ONLY from the sensor
 * work handler on the system workqueue, in this order within one pass: the
 * latest rate (if any report arrived), every beat record, then contact.
 *
 *   deliver_rate     one call per pass in which report_rate() was called,
 *                    with the newest value - repeats included, since each
 *                    report refreshes the stale deadline;
 *   deliver_beat     one call per ring record, oldest first, plus the newest
 *                    beat when the ring overflowed (its seq jump says how
 *                    many beats it stands for);
 *   deliver_contact  false for every loss since the last pass, then true if
 *                    contact is back.
 *
 * Contact last is deliberate: a loss that landed among the pass's beats then
 * clears their intervals rather than letting them through - the conservative
 * order, since a missing interval is legal and a wrong one is not.
 */
void hrm_node_deliver_rate(uint8_t bpm);
void hrm_node_deliver_beat(uint16_t seq, uint16_t event_time);
void hrm_node_deliver_contact(bool detected);

#endif /* HRM_SENSOR_H_ */
