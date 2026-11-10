/* SPDX-License-Identifier: Apache-2.0 */

/*
 * The health counters behind MESG_RADIANT_HEALTH (0xF6). See ant_health.h for
 * why they exist and why they are off by default.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>

#include <radiant/radiant_event.h>
#if defined(CONFIG_RADIANT_SEARCH_COVERAGE)
#include <radiant/radiant_search.h>
#endif

#include "ant_health.h"
#include "ant_wire.h"

/*
 * Saturating at 16 bits rather than counting at 32 and truncating on the wire.
 *
 * Truncation is the trap: a 32-bit counter at 70000 sends 4464, which reads as
 * a small, healthy number and is not one. Saturation sends 65535 and sets the
 * flag, so the reply says "at least this many" and can never say less than the
 * truth. The counters are witnesses to loss; a witness that under-reports is
 * worse than no witness.
 */
#define ANT_HEALTH_MAX 0xFFFFu

static atomic_t counters[ANT_HEALTH_COUNT];
static atomic_t depth_tx;
static atomic_t depth_event;
static atomic_t saturated;

void ant_health_note(enum ant_health_counter c)
{
	atomic_val_t v;

	if ((unsigned int)c >= (unsigned int)ANT_HEALTH_COUNT) {
		return;
	}

	/* atomic_inc() returns the value BEFORE the increment. Testing it
	 * against the ceiling rather than the new value means the clamp below
	 * runs once at the boundary and then every time after, which is what
	 * keeps the stored value pinned rather than wrapping past it. */
	v = atomic_inc(&counters[c]);
	if (v >= (atomic_val_t)ANT_HEALTH_MAX) {
		atomic_set(&counters[c], (atomic_val_t)ANT_HEALTH_MAX);
		atomic_set(&saturated, 1);
	}
}

void ant_health_depth(uint8_t which, uint32_t used)
{
	atomic_t *slot;
	atomic_val_t seen;

	switch (which) {
	case ANT_HEALTH_DEPTH_TX:
		slot = &depth_tx;
		break;
	case ANT_HEALTH_DEPTH_EVENT:
		slot = &depth_event;
		break;
	default:
		return;
	}

	if (used > 0xFFu) {
		used = 0xFFu;
	}

	/* A plain compare-and-set loop: two producers can race, and the loser
	 * simply retries against the winner's value. High water is a maximum,
	 * so a lost update that was smaller changes nothing anyway. */
	do {
		seen = atomic_get(slot);
		if ((atomic_val_t)used <= seen) {
			return;
		}
	} while (!atomic_cas(slot, seen, (atomic_val_t)used));
}

/* u16 little-endian, the byte order every multi-byte field in the ANT serial
 * protocol uses. Saturating here too, because the event-queue figures below
 * come from a 32-bit counter this file does not own. */
static void put_u16(uint8_t *p, uint32_t v)
{
	if (v > ANT_HEALTH_MAX) {
		v = ANT_HEALTH_MAX;
		atomic_set(&saturated, 1);
	}
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)((v >> 8) & 0xFFu);
}

size_t ant_health_fill(uint8_t *out, size_t cap)
{
	const struct radiant_event_stats *ev;

	if (out == NULL || cap < ANT_HEALTH_BODY_LEN) {
		return 0u;
	}

	memset(out, 0, ANT_HEALTH_BODY_LEN);

	/*
	 * Read the event queue's own numbers here rather than mirroring them
	 * into this file's counters. radiant_event.c already owns both, has
	 * always owned both, and a mirror would be a second place for them to
	 * be wrong. The cost is that they are not saturated at the source, so
	 * put_u16() clamps and sets the flag.
	 */
	ev = radiant_event_stats_get();

	/* [0] structure version, pinned at 1 for the whole beta. Fields are only
	 * ever added at the end and an offset is never reused, so a host reads
	 * which fields exist from the body length, and a meaning that changed
	 * would be a new field. Do not bump this for an added field. */
	out[0] = 1u;
	/* [1] flags, bit 0 = something saturated, so the whole set is a floor
	 * rather than a total. */
	out[1] = (atomic_get(&saturated) != 0) ? 0x01u : 0x00u;

	put_u16(&out[2], (uint32_t)atomic_get(&counters[ANT_HEALTH_TX_DROP]));
	out[4] = (uint8_t)atomic_get(&depth_tx);
	out[5] = (uint8_t)atomic_get(&depth_event);
	put_u16(&out[6], ev != NULL ? ev->overflow_marks : 0u);
	put_u16(&out[8],
		(uint32_t)atomic_get(&counters[ANT_HEALTH_RX_BACKPRESSURE]));
	put_u16(&out[10],
		(uint32_t)atomic_get(&counters[ANT_HEALTH_KEY_REJECT]));
	put_u16(&out[12],
		(uint32_t)atomic_get(&counters[ANT_HEALTH_SCHED_MISSED]));
	put_u16(&out[14],
		(uint32_t)atomic_get(&counters[ANT_HEALTH_SCHED_FAILED]));
	put_u16(&out[16],
		(uint32_t)atomic_get(&counters[ANT_HEALTH_SCHED_DENIED]));
	put_u16(&out[18],
		(uint32_t)atomic_get(&counters[ANT_HEALTH_BURST_STALL]));
	put_u16(&out[20],
		(uint32_t)atomic_get(&counters[ANT_HEALTH_SWEEP_BUSY]));
	/* [22..23] HOST_DESYNC. Not the idle half of the sweep decline: see
	 * ANT_HEALTH_SWEEP_BUSY for why that field cannot exist. */
	put_u16(&out[22],
		(uint32_t)atomic_get(&counters[ANT_HEALTH_HOST_DESYNC]));
	put_u16(&out[24],
		(uint32_t)atomic_get(&counters[ANT_HEALTH_HOST_CSUM]));
	put_u16(&out[26],
		(uint32_t)atomic_get(&counters[ANT_HEALTH_RX_NOT_OWNER]));
	put_u16(&out[28],
		(uint32_t)atomic_get(&counters[ANT_HEALTH_TX_TIMEOUT]));

	/*
	 * The event queue's own high water goes in [5] from radiant_event.c
	 * when this backend feeds it; ant_health_depth() is the path for
	 * backends that do not. Whichever wrote last wins, and both are the
	 * same quantity, so there is nothing to reconcile.
	 */
	if (ev != NULL && (uint32_t)ev->high_water > (uint32_t)out[5]) {
		out[5] = (uint8_t)((ev->high_water > 0xFFu) ? 0xFFu
							    : ev->high_water);
	}

#if defined(CONFIG_RADIANT_SEARCH_COVERAGE)
	/* [30..31] sets_advanced_uncovered. Clamped WITHOUT put_u16()'s flag:
	 * this counter climbs for as long as a search runs, so one long-running
	 * field would otherwise mark every other field in the reply as a
	 * floor - the reason the idle sweep field was left out (see
	 * ANT_HEALTH_SWEEP_BUSY). A counter that stops at 65535 says "at least
	 * this many" on its own. */
	{
		uint32_t n = radiant_search_sets_uncovered_get();

		if (n > ANT_HEALTH_MAX) {
			n = ANT_HEALTH_MAX;
		}
		out[30] = (uint8_t)(n & 0xFFu);
		out[31] = (uint8_t)((n >> 8) & 0xFFu);
	}
#endif

	return ANT_HEALTH_BODY_LEN;
}
