/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The seam's plumbing: registration, the node's 1/1024 s clock, and the one
 * hop from "an ISR saw a beat" to "the workqueue may touch radiant_profile_hr".
 *
 * Read src/hrm_sensor.h first - the reasoning for the shape is there, and the
 * only thing worth repeating here is the invariant this file is responsible
 * for: NOTHING in the report_* path touches radiant_profile_hr, the BLE stack, or any
 * kernel object that can block. Atomic loads and stores, one k_work_submit(),
 * return.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <errno.h>

#include "hrm_sensor.h"
#include "hrm_beat_ring.h"
#include <radiant/radiant_profile_hr.h>

LOG_MODULE_DECLARE(hrm, CONFIG_HRM_LOG_LEVEL);

static const struct hrm_sensor *registered;

/*
 * ── Beats: the SPSC ring ───────────────────────────────────────────────────
 *
 * src/hrm_beat_ring.h. One record per beat, each with its own instant, so a
 * coalesced work submission still yields every interval; the record's seq
 * keeps the count exact even when the ring is full. Single producer: exactly
 * one sensor registers (hrm_sensor_register()'s -EEXIST).
 */
static struct hrm_beat_ring beat_ring;

/*
 * ── Rates: {seq, bpm} in one word ──────────────────────────────────────────
 *
 *   bits 31..8  a report counter, so a repeat of the SAME rate is still seen
 *               as a report (it refreshes the stale deadline)
 *   bits  7..0  the rate
 *
 * Updated with a compare-and-swap so it is safe from any number of contexts.
 */
static atomic_t rate_word;

/*
 * ── Contact: a level and a loss counter ────────────────────────────────────
 *
 * The level alone would let a loss and a regain between two passes cancel
 * out, and a loss must always clear pending RR. So every loss also bumps a
 * counter; the worker delivers one loss per pass whenever it moved, then the
 * current level. atomic_inc() makes it safe from several contexts (the
 * simulator reports from a button ISR and from its own work item).
 */
static atomic_t contact_level = ATOMIC_INIT(1);
static atomic_t contact_losses;

static void sensor_work_fn(struct k_work *w);
static K_WORK_DEFINE(sensor_work, sensor_work_fn);

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

bool hrm_sensor_contact_supported(void)
{
	return (registered != NULL) && registered->contact_supported;
}

void hrm_sensor_report_beat(uint16_t event_time_1024)
{
	(void)hrm_beat_ring_push(&beat_ring, event_time_1024);

	/* Limit-1 by construction (re-submitting a pending item is a no-op),
	 * so a burst of beats cannot queue work unboundedly; the ring holds
	 * every instant until the handler runs. */
	k_work_submit(&sensor_work);
}

void hrm_sensor_report_rate(uint8_t bpm)
{
	uint32_t old;
	uint32_t new;

	do {
		old = (uint32_t)atomic_get(&rate_word);
		new = (((old >> 8) + 1u) << 8) | (uint32_t)bpm;
	} while (!atomic_cas(&rate_word, (atomic_val_t)old, (atomic_val_t)new));

	k_work_submit(&sensor_work);
}

void hrm_sensor_report_contact(bool detected)
{
	/* Level first: a worker that sees the new loss count reads a level at
	 * least as new as this report. */
	(void)atomic_set(&contact_level, detected ? 1 : 0);
	if (!detected) {
		(void)atomic_inc(&contact_losses);
	}
	k_work_submit(&sensor_work);
}

static void deliver_beat_rec(void *ctx, uint16_t seq, uint16_t event_time)
{
	ARG_UNUSED(ctx);
	hrm_node_deliver_beat(seq, event_time);
}

static void sensor_work_fn(struct k_work *w)
{
	/* Touched only here, on the system workqueue. */
	static uint32_t consumed_rate_seq;
	static atomic_val_t consumed_losses;
	static bool applied_contact = true;

	atomic_val_t losses = atomic_get(&contact_losses);
	bool level = atomic_get(&contact_level) != 0;
	bool lost = (losses != consumed_losses);
	uint32_t rate = (uint32_t)atomic_get(&rate_word);

	ARG_UNUSED(w);

	/* 0. A regain whose loss an earlier pass already delivered goes
	 * FIRST, so the rate and beats a driver reports with it count. */
	if (!lost && level && !applied_contact) {
		applied_contact = true;
		hrm_node_deliver_contact(true);
	}

	/* 1. The newest rate, once per pass in which any report arrived. */
	if ((rate >> 8) != consumed_rate_seq) {
		consumed_rate_seq = rate >> 8;
		hrm_node_deliver_rate((uint8_t)rate);
	}

	/* 2. Every beat, oldest first (hrm_beat_ring_drain()). */
	(void)hrm_beat_ring_drain(&beat_ring, deliver_beat_rec, NULL);

	/* 3. A loss in this pass goes LAST, so it clears the intervals of
	 * the beats it landed among rather than letting them through; then
	 * the level as it is now. */
	if (lost) {
		consumed_losses = losses;
		applied_contact = false;
		hrm_node_deliver_contact(false);
	}
	level = atomic_get(&contact_level) != 0;
	if (level != applied_contact) {
		applied_contact = level;
		hrm_node_deliver_contact(level);
	}
}
