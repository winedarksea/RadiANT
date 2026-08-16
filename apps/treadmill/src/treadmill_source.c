/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The seam's plumbing: registration, the control path, and the one hop from
 * "an ISR saw a stride" to "the workqueue may touch a profile".
 *
 * Read src/treadmill_source.h first. The invariant this file is responsible
 * for is the same one apps/hrm_ble/src/hrm_sensor.c holds: NOTHING in the
 * report_* path touches a profile module, treadmill_state, the BLE stack, or
 * any kernel object that can block. One atomic store, one k_work_submit(),
 * return.
 *
 * The control path is the other direction and has the opposite rule: it is
 * called from thread context on both radios and it calls straight into the
 * source, because moving an actuator is exactly the kind of work that may
 * block and the caller is already somewhere it can.
 */

#include <errno.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "treadmill_source.h"
#include "treadmill_state.h"

LOG_MODULE_DECLARE(treadmill, CONFIG_TREADMILL_LOG_LEVEL);

static const struct treadmill_source *registered;

/*
 * ── The lock-free report words ─────────────────────────────────────────────
 *
 * Four independent atomics rather than one packed word, and the difference
 * from hrm_sensor.c's single {seq, event_time} record is worth stating.
 *
 * There, the sequence number and the instant HAD to be consistent with each
 * other: a beat is an edge and its time is what the page carries. Here the
 * quantities are independent - a speed reading and an incline reading and a
 * stride count have no relationship that a torn read could violate. Each is a
 * level (or a counter) that the handler samples; the newest value wins and no
 * pairing is lost.
 *
 * The STRIDE counter is the one that must not be sampled destructively. It is
 * a free-running 16-bit count and the handler takes a modular difference, so a
 * burst that lands between submissions still accounts for every stride - the
 * same argument hrm_sensor.c makes about beats, and the reason the count is
 * what a receiver differences rather than the cadence.
 *
 * SINGLE PRODUCER. Exactly one source registers, so exactly one context calls
 * the report functions, so `producer_strides` needs no protection. That is
 * what treadmill_source_register()'s -EEXIST exists to keep true.
 */
static atomic_t reported_speed_mm_s;
static atomic_t reported_incline_centi;
static atomic_t reported_cadence_16;
static atomic_t reported_state = ATOMIC_INIT(TREADMILL_STATE_READY);
static atomic_t stride_count;
static uint16_t producer_strides;

static void report_work_fn(struct k_work *w);
static K_WORK_DEFINE(report_work, report_work_fn);

int treadmill_source_register(const struct treadmill_source *src)
{
	if (src == NULL || src->api == NULL || src->api->start == NULL) {
		return -EINVAL;
	}

	if (registered != NULL) {
		/*
		 * Error level, not warning. Two sources both reporting would
		 * break the single-producer rule above, and the resulting node
		 * would report the interleaving of two belts as one. There is
		 * no safe way to pick, so the first registration keeps the seam
		 * and this is loud.
		 */
		LOG_ERR("treadmill_source: '%s' refused, '%s' is already "
			"registered",
			src->name ? src->name : "(unnamed)",
			registered->name ? registered->name : "(unnamed)");
		return -EEXIST;
	}

	registered = src;
	return 0;
}

const char *treadmill_source_name(void)
{
	return (registered == NULL) ? NULL : registered->name;
}

int treadmill_source_start(void)
{
	if (registered == NULL) {
		/*
		 * The CONFIG_TREADMILL_SOURCE_CUSTOM failure, spelled out where
		 * an integrator will actually meet it. Deliberately not a
		 * fallback to the simulator: a machine quietly reporting a
		 * simulated runner while somebody stands on it is invisible
		 * from the receiving end, and one reporting nothing is not.
		 */
		LOG_ERR("treadmill_source: nothing registered - the node will "
			"report a stopped belt forever. Under "
			"CONFIG_TREADMILL_SOURCE_CUSTOM your driver must call "
			"treadmill_source_register() before this point.");
		return -ENODEV;
	}

	return registered->api->start(registered);
}

int treadmill_source_stop(void)
{
	if (registered == NULL) {
		return -ENODEV;
	}
	if (registered->api->stop == NULL) {
		return 0;
	}
	return registered->api->stop(registered);
}

void treadmill_source_poll(void)
{
	if (registered != NULL && registered->api->poll != NULL) {
		registered->api->poll(registered);
	}
}

int treadmill_source_set_incline(int32_t centi_pct)
{
	if (registered == NULL) {
		return -ENODEV;
	}
	if (registered->api->set_incline == NULL) {
		/* A source that did not implement the entry point is a machine
		 * with a fixed deck, which is -ENOTSUP and not an error: see
		 * the api's own contract for what each radio must then say. */
		return -ENOTSUP;
	}
	return registered->api->set_incline(registered, centi_pct);
}

int treadmill_source_set_speed(uint32_t mm_s)
{
	if (registered == NULL) {
		return -ENODEV;
	}
	if (registered->api->set_speed == NULL) {
		return -ENOTSUP;
	}
	return registered->api->set_speed(registered, mm_s);
}

/* ── The report path ────────────────────────────────────────────────────── */

void treadmill_source_report_speed(uint32_t mm_s)
{
	atomic_set(&reported_speed_mm_s, (atomic_val_t)mm_s);
	k_work_submit(&report_work);
}

void treadmill_source_report_incline(int32_t centi_pct)
{
	atomic_set(&reported_incline_centi, (atomic_val_t)centi_pct);
	k_work_submit(&report_work);
}

void treadmill_source_report_cadence_16(uint32_t strides_min_16)
{
	atomic_set(&reported_cadence_16, (atomic_val_t)strides_min_16);
	k_work_submit(&report_work);
}

void treadmill_source_report_state(uint8_t fe_state)
{
	atomic_set(&reported_state, (atomic_val_t)fe_state);
	k_work_submit(&report_work);
}

void treadmill_source_report_stride(void)
{
	producer_strides++;
	atomic_set(&stride_count, (atomic_val_t)producer_strides);

	/* Limit-1 by construction: k_work is not delayable and re-submitting a
	 * pending item is a no-op, so a burst of strides cannot queue work
	 * unboundedly. The handler's modular difference is what makes a
	 * coalesced submission still account for every one of them. */
	k_work_submit(&report_work);
}

/*
 * The handler. Runs on the system workqueue - the context that already
 * existed - so the profile modules stay at two contexts however the source is
 * driven, including from an ISR.
 */
static void report_work_fn(struct k_work *w)
{
	/* Touched only here, on the system workqueue. */
	static uint16_t consumed_strides;

	uint16_t seq = (uint16_t)atomic_get(&stride_count);
	uint16_t strides;

	ARG_UNUSED(w);

	/* Modular difference: `seq - consumed_strides` in uint16_t arithmetic
	 * is the number of strides since the last pass whether or not either
	 * value wrapped, which is the only reason the counter can be 16 bits.
	 * At 90 strides/min the wrap is twelve hours away and it is still not
	 * an event when it comes. */
	strides = (uint16_t)(seq - consumed_strides);
	consumed_strides = seq;

	treadmill_node_deliver(strides);
}

/* ---------------------------------------------------------------------------
 * Accessors for the levels, read by the application's deliver function. Not in
 * the public header: they exist so treadmill_node_deliver() can sample the
 * report words on the workqueue without every caller learning the atomics.
 * ---------------------------------------------------------------------------
 */

uint32_t treadmill_source_last_speed(void)
{
	return (uint32_t)atomic_get(&reported_speed_mm_s);
}

int32_t treadmill_source_last_incline(void)
{
	return (int32_t)atomic_get(&reported_incline_centi);
}

uint32_t treadmill_source_last_cadence_16(void)
{
	return (uint32_t)atomic_get(&reported_cadence_16);
}

uint8_t treadmill_source_last_state(void)
{
	return (uint8_t)atomic_get(&reported_state);
}
