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
 * `SENSOR_CHAN_PRIV_START + n` - which is a smell, but not the reason it was
 * rejected. The reason is that the sensor API is a POLL-A-SCALAR model and a
 * beat is an EDGE. A sensor-API strap re-derives the beat instant from whatever
 * tick happened to do the fetch, which is structurally the same mistake that
 * produced the 72-versus-66.7 bpm drift bug this application already shipped
 * once (see src/hrm_sim_sched.c). Adopting that API would have made the bug the
 * architecture.
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
 * `profile_hr_beat(&hr, event_time)` plus `profile_hr_set_computed(&hr, bpm)`
 * is precisely what ANT+ page 0x00 carries: an event count and an event time at
 * 1/1024 s, plus the sensor's own computed rate. A seam that did not match the
 * wire format would make every integrator write the same adapter to get back to
 * it.
 *
 * ─── The concurrency detail that must not be got wrong ─────────────────────
 *
 * `profile_hr` is ALREADY reached from two contexts in this application:
 *
 *   1. the system workqueue, which advances the beat accumulators, and
 *   2. the radiant callback path - antr_on_message() -> load_next_page() ->
 *      profile_hr_next() - which reads them to build the next page.
 *
 * An ISR-driven sensor would make that three-way, and profile_hr has no lock.
 * So hrm_sensor_report_beat() does NOT touch profile_hr. It writes one 32-bit
 * `{seq, event_time}` word with a single atomic store and submits one work
 * item; the handler runs on the system workqueue - context 1, which already
 * existed - and does profile_hr_beat(), profile_hr_set_computed() and the BLE
 * notification together. Timestamp fidelity is preserved because the instant
 * travels in the word; the profile's context count stays at two.
 *
 * The word is single-producer by construction: exactly one sensor is
 * registered, so exactly one context calls report_beat(). Do not call it from
 * two.
 */

#ifndef HRM_SENSOR_H_
#define HRM_SENSOR_H_

#include <stdint.h>

struct hrm_sensor;

struct hrm_sensor_api {
	/*
	 * Bring the source up. Required. Called once from main(), between
	 * profile_hr_init() and the ANT+ channel opening, so a source that
	 * needs the radio quiet has it, and a source that fails stops the node
	 * before it ever claims to be transmitting.
	 *
	 * Return 0 or a negative errno; a failure is logged and the node keeps
	 * running with no beats, which reads as PROFILE_HR_INVALID_BPM on the
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
};

/*
 * Register the one source. Call before main()'s hrm_sensor_start() - a
 * SYS_INIT() at APPLICATION level, or the source's own init hook.
 *
 * EXACTLY ONE WINS. A second call returns -EEXIST and logs at error level
 * rather than replacing the first: two sources reporting beats would violate
 * the single-producer rule the {seq, event_time} word depends on, and "the last
 * one to initialise" is not a rule anybody could reason about.
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
 * ISR-SAFE BY CONSTRUCTION: one atomic store and one k_work_submit(). It does
 * not touch profile_hr and it does not call bt_hrs_notify(). See the header
 * comment for why that matters.
 */
void hrm_sensor_report_beat(uint16_t event_time_1024);

/*
 * The source's own computed rate - byte 7 of ANT+ pages 0x00 and 0x04.
 *
 * PROFILE_HR_INVALID_BPM (0) means "no reading", and is what a source should
 * report when it has lost contact. 0xFF IS A REAL 255 bpm ON THIS PROFILE and
 * is not a sentinel; do not use it to mean "unknown".
 *
 * ISR-safe on the same terms as report_beat().
 */
void hrm_sensor_report_rate(uint8_t bpm);

/*
 * The node's 1/1024 s clock, exported so a driver can timestamp against exactly
 * the base the ANT+ page carries. A driver that timestamps against its own
 * clock and converts is one rounding rule away from a receiver computing a
 * different rate than the one on byte 7.
 */
uint16_t hrm_sensor_now_1024(void);

/*
 * Implemented by the application (src/main.c), called ONLY from the beat work
 * handler on the system workqueue.
 *
 * `beats` is how many beats the handler is accounting for in one pass - 1 in
 * steady state, and 0 when the source reported only a rate (a source with no
 * edge detector of its own is legitimate; the page still carries byte 7).
 * `event_time` is meaningless when `beats` is 0 and must be ignored.
 *
 * It can exceed 1 only if two beats landed between submissions,
 * which at the profile's 30..220 bpm range means a >=273 ms workqueue stall.
 * When it does, the count still advances by the right amount and the event time
 * is the LATEST instant: the count is the field that survives a lost packet, so
 * preserving it is what matters, and no instant is fabricated for the beats
 * whose exact time was overwritten.
 */
void hrm_node_deliver_beats(uint16_t beats, uint16_t event_time, uint8_t bpm);

#endif /* HRM_SENSOR_H_ */
