/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_bridge.c - the ring buffer and the sink dispatch loop.
 *
 * See radiant_bridge.h for the design. The two halves of this file have
 * different rules: radiant_bridge_post() may run in a radio ISR and must never
 * block, take an unbounded lock, or call into a sink; radiant_bridge_drain()
 * is thread context only and is the only place a sink's publish() is called.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>

#include "radiant_bridge.h"

/*
 * 32: absorbs ~4 decode-to-drain periods for an 8-sensor deployment at
 * ~4 Hz each producing up to 3 records (HR adapter worst case) - generous,
 * and still well under a kilobyte. Not yet a Kconfig choice.
 */
#define RADIANT_BRIDGE_RING_CAP 32u

static struct radiant_sample ring[RADIANT_BRIDGE_RING_CAP];
static uint32_t              ring_head; /* next slot to write */
static uint32_t              ring_count;

static struct radiant_bridge_stats stats;
static uint32_t                diag_reported_drops;

void radiant_bridge_reset(void)
{
	unsigned int key = irq_lock();

	memset(ring, 0, sizeof(ring));
	ring_head = 0u;
	ring_count = 0u;
	memset(&stats, 0, sizeof(stats));
	diag_reported_drops = 0u;

	irq_unlock(key);
}

void radiant_bridge_post(const struct radiant_sample *s)
{
	unsigned int key;

	if (s == NULL) {
		return;
	}

	key = irq_lock();

	stats.posted++;

	if (ring_count == RADIANT_BRIDGE_RING_CAP) {
		/* Full: drop oldest (section 3.3). Write cursor == read
		 * cursor when full, so writing here already discards the
		 * oldest slot; just count it. */
		stats.dropped++;
	} else {
		ring_count++;
	}

	ring[ring_head] = *s;
	ring_head = (ring_head + 1u) % RADIANT_BRIDGE_RING_CAP;

	irq_unlock(key);
}

/* The oldest queued record's index, given the current head and count. Caller
 * holds the lock. */
static uint32_t ring_tail_locked(void)
{
	return (ring_head + RADIANT_BRIDGE_RING_CAP - ring_count) %
	       RADIANT_BRIDGE_RING_CAP;
}

/* Pop the oldest record into *out. Returns false if the ring was empty.
 * Caller holds no lock; this takes and releases its own, kept short so a
 * radio ISR posting concurrently is never blocked for more than the copy. */
static bool ring_pop(struct radiant_sample *out)
{
	unsigned int key = irq_lock();
	bool         got = false;

	if (ring_count != 0u) {
		*out = ring[ring_tail_locked()];
		ring_count--;
		got = true;
	}

	irq_unlock(key);
	return got;
}

static void dispatch_one(const struct radiant_sample *s)
{
	STRUCT_SECTION_FOREACH(radiant_sink, sink) {
		if (sink->want != NULL && !sink->want(s)) {
			continue;
		}
		if (sink->publish != NULL) {
			sink->publish(s);
			stats.drained++;
		}
	}
}

uint32_t radiant_bridge_drain(void)
{
	struct radiant_sample s;
	uint32_t          n = 0u;

	while (ring_pop(&s)) {
		dispatch_one(&s);
		n++;
	}

	/* Self-published only when dropped moved, so an idle bus doesn't
	 * manufacture traffic. */
	if (stats.dropped != diag_reported_drops) {
		struct radiant_sample diag = {
			.source = RADIANT_BRIDGE_DIAG_SOURCE,
			.field_id = RADIANT_BRIDGE_DIAG_FIELD_DROPPED,
			.field_type = RADIANT_FIELD_EVENT_COUNT,
			.flags = RADIANT_SAMPLE_ACCUMULATING,
			.exp = 0,
			.raw = (int64_t)stats.dropped,
			.t_us = 0u, /* the caller owns a clock; the bus does not */
		};

		diag_reported_drops = stats.dropped;
		dispatch_one(&diag);
	}

	return n;
}

const struct radiant_bridge_stats *radiant_bridge_stats_get(void)
{
	return &stats;
}
