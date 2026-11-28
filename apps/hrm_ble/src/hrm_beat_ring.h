/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The beat ring: how a beat travels from the sensor's interrupt to the
 * workqueue without losing its instant.
 *
 * WHAT IT REPLACED. One 32-bit {seq, event_time} word. It kept the beat COUNT
 * exact across a coalesced work submission, but only the LATEST instant: two
 * beats landing between submissions left one interval unrecoverable, and an
 * RR-Interval (BLE HRS) is exactly that interval.
 *
 * WHAT IT IS. A bounded single-producer / single-consumer ring of
 * HRM_BEAT_RING_LEN {seq, event_time} records, plus the old `latest` word kept
 * beside it. The producer is the one registered sensor's report_beat() - ISR
 * or thread, but ONE context (hrm_sensor_register()'s -EEXIST keeps it one).
 * The consumer is the system workqueue.
 *
 *   - Every record carries its beat's sequence number, so the consumer sees a
 *     gap as a gap. A beat that finds the ring full is NOT stored - it never
 *     overwrites a slot the consumer may be reading - but it still advances
 *     `seq` and `latest`.
 *   - The consumer drains the ring in order, then - only if `dropped` says a
 *     beat found the ring full - reads `latest`. The dropped beats show up
 *     there, so the COUNT is never short (the ANT+ event count is the field
 *     that survives a lost packet), and the seq jump tells hrm_publish the
 *     intervals across them are unknown.
 *   - A record whose seq the consumer has already accounted for (it read it
 *     through `latest` first) is skipped by hrm_publish_beat().
 *
 * MEMORY ORDERING. The indices and slots are Zephyr atomic_t and every access
 * is atomic_get()/atomic_set(). With CONFIG_ATOMIC_OPERATIONS_BUILTIN (every
 * Cortex-M with LDREX/STREX, and native_sim) those are __atomic_load_n and
 * __atomic_exchange_n at __ATOMIC_SEQ_CST, which compile to DMB-fenced
 * accesses: the slot store is ordered before the head store that publishes it,
 * and the consumer's slot loads are ordered before the tail store that frees
 * them. On a part without them (ATOMIC_OPERATIONS_C, Cortex-M0) each is done
 * under irq_lock(), which is stronger still. `volatile` alone would give
 * neither guarantee.
 *
 * Kernel-free apart from <zephyr/sys/atomic.h>; apps/hrm_ble/tests/publish
 * drives it on native_sim.
 */

#ifndef HRM_BEAT_RING_H_
#define HRM_BEAT_RING_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/atomic.h>

/* A power of two. 8 beats is 2.2 s at 220 bpm - a workqueue stall that long
 * is a fault, and even then only RR-Intervals are lost, never the count. */
#define HRM_BEAT_RING_LEN 8u

struct hrm_beat_ring {
	atomic_t head;                    /* written by the producer only */
	atomic_t tail;                    /* written by the consumer only */
	atomic_t slot[HRM_BEAT_RING_LEN]; /* (seq << 16) | event_time */
	atomic_t latest;                  /* the newest beat, stored or not */
	uint16_t seq;                     /* producer-private */
	atomic_t dropped;                 /* beats that found the ring full */
};

static inline uint16_t hrm_beat_rec_seq(uint32_t rec)
{
	return (uint16_t)(rec >> 16);
}

static inline uint16_t hrm_beat_rec_time(uint32_t rec)
{
	return (uint16_t)rec;
}

/* Zero it, or call this, before either side runs. */
void hrm_beat_ring_init(struct hrm_beat_ring *r);

/*
 * PRODUCER. One beat at `event_time` (1/1024 s). ISR-safe: atomic loads and
 * stores only, bounded, no locks. Returns false when the ring was full and the
 * beat went only into `latest` (its interval will not be reported).
 */
bool hrm_beat_ring_push(struct hrm_beat_ring *r, uint16_t event_time);

/*
 * CONSUMER. Hand every stored record to `fn`, oldest first, then - only when
 * a beat found the ring full since the last drain - the `latest` word (it may
 * repeat a record - the receiver skips a seq it has already seen). Returns the number
 * of ring records handed over, `latest` not counted.
 */
typedef void (*hrm_beat_ring_fn)(void *ctx, uint16_t seq, uint16_t event_time);
unsigned int hrm_beat_ring_drain(struct hrm_beat_ring *r, hrm_beat_ring_fn fn, void *ctx);

#endif /* HRM_BEAT_RING_H_ */
