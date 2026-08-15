/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The seam's plumbing: registration, the node's 1/1024 s clock, and the one
 * hop from "an ISR saw a beat" to "the workqueue may touch profile_hr".
 *
 * Read src/hrm_sensor.h first - the reasoning for the shape is there, and the
 * only thing worth repeating here is the invariant this file is responsible
 * for: NOTHING in the report_* path touches profile_hr, the BLE stack, or any
 * kernel object that can block. One atomic store, one k_work_submit(), return.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>

#include "hrm_sensor.h"
#include "profile_hr.h"

LOG_MODULE_DECLARE(hrm, CONFIG_HRM_LOG_LEVEL);

static const struct hrm_sensor *registered;

/*
 * ── The lock-free {seq, event_time} record ─────────────────────────────────
 *
 * One 32-bit word, written with a single atomic store and read with a single
 * atomic load, so it can never tear: the sequence number and the instant it
 * belongs to are always consistent with each other, which a pair of separate
 * variables could not promise without a lock the ISR must not take.
 *
 *   bits 31..16  seq, a free-running 16-bit beat counter
 *   bits 15..0   the beat's absolute time in 1/1024 s
 *
 * The consumer compares sequence numbers with modular arithmetic and never
 * with `<`, so the wrap at 65536 beats (about 5 hours at 220 bpm, about 15 at
 * 72) is not an event. It only ever needs the DIFFERENCE.
 *
 * SINGLE PRODUCER. Exactly one sensor registers, so exactly one context calls
 * hrm_sensor_report_beat(), so `producer_seq` needs no protection either. That
 * is the property hrm_sensor_register()'s -EEXIST exists to keep.
 */
static atomic_t beat_word;
static uint16_t producer_seq;

/* The last rate the source reported. Its own word: a rate report and a beat are
 * independent events, and a source that only ever computes a rate (no edge
 * detector of its own to expose) must still work. */
static atomic_t reported_bpm = ATOMIC_INIT(PROFILE_HR_INVALID_BPM);

static void beat_work_fn(struct k_work *w);
static K_WORK_DEFINE(beat_work, beat_work_fn);

uint16_t hrm_sensor_now_1024(void)
{
	/*
	 * 1/1024 s, wrapping at 16 bits (the ANT+ common time base).
	 *
	 * ms * 1024 / 1000, NOT ms << 10 / 1000 and not ms * 1024 >> 10: the
	 * shift forms are a ~2.4 % error against the real 1/1024 s tick, which
	 * is small enough to look like nothing in a log and large enough to
	 * show up as a wrong rate in a receiver that computes bpm from the
	 * interval between event times.
	 */
	uint64_t ms = (uint64_t)k_uptime_get();

	return (uint16_t)((ms * 1024u) / 1000u);
}

int hrm_sensor_register(const struct hrm_sensor *sensor)
{
	if (sensor == NULL || sensor->api == NULL ||
	    sensor->api->start == NULL) {
		return -EINVAL;
	}

	if (registered != NULL) {
		/*
		 * Error level, not warning: two sources both reporting beats
		 * would break the single-producer rule above, and the resulting
		 * node would transmit a plausible-looking heart rate that is
		 * the interleaving of two. There is no safe way to pick one, so
		 * the first registration keeps the seam and this is loud.
		 */
		LOG_ERR("hrm_sensor: '%s' refused, '%s' is already registered",
			sensor->name ? sensor->name : "(unnamed)",
			registered->name ? registered->name : "(unnamed)");
		return -EEXIST;
	}

	registered = sensor;
	return 0;
}

const char *hrm_sensor_name(void)
{
	if (registered == NULL) {
		return NULL;
	}
	return registered->name;
}

int hrm_sensor_start(void)
{
	if (registered == NULL) {
		/*
		 * The CONFIG_HRM_SENSOR_CUSTOM failure, spelled out where an
		 * integrator will actually meet it. Deliberately not a fallback
		 * to the simulator: a strap quietly reporting a simulated rate
		 * on a real chest is invisible from the receiving end, and a
		 * strap reporting nothing is not.
		 */
		LOG_ERR("hrm_sensor: nothing registered - the node will "
			"transmit no heart rate at all. Under "
			"CONFIG_HRM_SENSOR_CUSTOM your driver must call "
			"hrm_sensor_register() before this point.");
		return -ENODEV;
	}

	return registered->api->start(registered);
}

int hrm_sensor_stop(void)
{
	if (registered == NULL) {
		return -ENODEV;
	}
	if (registered->api->stop == NULL) {
		return 0;
	}
	return registered->api->stop(registered);
}

void hrm_sensor_poll(void)
{
	if (registered != NULL && registered->api->poll != NULL) {
		registered->api->poll(registered);
	}
}

void hrm_sensor_report_beat(uint16_t event_time_1024)
{
	uint32_t word;

	producer_seq++;
	word = ((uint32_t)producer_seq << 16) | (uint32_t)event_time_1024;
	atomic_set(&beat_word, (atomic_val_t)word);

	/* Limit-1 by construction (k_work is not delayable and re-submitting a
	 * pending item is a no-op), so a burst of beats cannot queue work
	 * unboundedly; the handler's modular seq difference is what makes a
	 * coalesced submission still account for every beat. */
	k_work_submit(&beat_work);
}

void hrm_sensor_report_rate(uint8_t bpm)
{
	atomic_set(&reported_bpm, (atomic_val_t)bpm);
	k_work_submit(&beat_work);
}

static void beat_work_fn(struct k_work *w)
{
	/* Touched only here, on the system workqueue. */
	static uint16_t consumed_seq;
	static uint8_t applied_bpm = PROFILE_HR_INVALID_BPM;

	uint32_t word = (uint32_t)atomic_get(&beat_word);
	uint16_t seq = (uint16_t)(word >> 16);
	uint16_t event_time = (uint16_t)word;
	uint8_t bpm = (uint8_t)atomic_get(&reported_bpm);
	uint16_t beats;

	ARG_UNUSED(w);

	/* Modular difference. `seq - consumed_seq` in uint16_t arithmetic is
	 * the number of beats since the last pass whether or not either value
	 * wrapped, which is the only reason the counter can be 16 bits. */
	beats = (uint16_t)(seq - consumed_seq);
	consumed_seq = seq;

	if (beats == 0u && bpm == applied_bpm) {
		return;
	}
	applied_bpm = bpm;

	hrm_node_deliver_beats(beats, event_time, bpm);
}
