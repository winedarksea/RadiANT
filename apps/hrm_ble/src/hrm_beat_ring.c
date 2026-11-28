/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The beat ring. See hrm_beat_ring.h for the design and the memory-ordering
 * argument; the comments here mark where each half of it is kept.
 */

#include <string.h>
#include <zephyr/toolchain.h>

#include "hrm_beat_ring.h"

#define RING_MASK (HRM_BEAT_RING_LEN - 1u)

BUILD_ASSERT((HRM_BEAT_RING_LEN & RING_MASK) == 0u, "ring length must be a power of two");

void hrm_beat_ring_init(struct hrm_beat_ring *r)
{
	memset(r, 0, sizeof(*r));
}

bool hrm_beat_ring_push(struct hrm_beat_ring *r, uint16_t event_time)
{
	uint32_t head;
	uint32_t tail;
	uint32_t rec;

	r->seq++;
	rec = ((uint32_t)r->seq << 16) | (uint32_t)event_time;

	/* `head` is ours; `tail` is the consumer's, and the SEQ_CST load
	 * orders it before the slot store below: a slot is only reused after
	 * the consumer's tail store has said it finished reading it. */
	head = (uint32_t)atomic_get(&r->head);
	tail = (uint32_t)atomic_get(&r->tail);

	if ((uint32_t)(head - tail) < HRM_BEAT_RING_LEN) {
		/* Slot first, then the head that publishes it. */
		(void)atomic_set(&r->slot[head & RING_MASK], (atomic_val_t)rec);
		(void)atomic_set(&r->head, (atomic_val_t)(head + 1u));
		(void)atomic_set(&r->latest, (atomic_val_t)rec);
		return true;
	}

	/* Full: never overwrite - the consumer may be reading the oldest
	 * slot right now. The beat still counts, via `latest`, which is
	 * stored BEFORE `dropped` says there is something to read in it. */
	(void)atomic_set(&r->latest, (atomic_val_t)rec);
	(void)atomic_inc(&r->dropped);
	return false;
}

unsigned int hrm_beat_ring_drain(struct hrm_beat_ring *r, hrm_beat_ring_fn fn, void *ctx)
{
	uint32_t head = (uint32_t)atomic_get(&r->head); /* acquire the slots */
	uint32_t tail = (uint32_t)atomic_get(&r->tail);
	unsigned int n = 0u;
	uint32_t rec;

	while (tail != head) {
		rec = (uint32_t)atomic_get(&r->slot[tail & RING_MASK]);
		tail++;
		n++;
		fn(ctx, hrm_beat_rec_seq(rec), hrm_beat_rec_time(rec));
	}
	/* Free the slots only after reading them. */
	(void)atomic_set(&r->tail, (atomic_val_t)tail);

	/* Beats that found the ring full are not in it. `latest` carries the
	 * newest of them; its seq jump is what tells the receiver how many,
	 * and that the intervals across them are unknown. Read only when
	 * something was dropped, so a beat stored during this drain is never
	 * mistaken for a gap. */
	if (atomic_set(&r->dropped, 0) != 0) {
		rec = (uint32_t)atomic_get(&r->latest);
		fn(ctx, hrm_beat_rec_seq(rec), hrm_beat_rec_time(rec));
	}
	return n;
}
