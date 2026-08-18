/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_liveness.c - see radiant_liveness.h.
 */

#include <stdbool.h>
#include <string.h>

#include "radiant_binding.h"
#include "radiant_bridge.h"
#include "radiant_liveness.h"

/*
 * The channel period arrives in ANT's own 32768 Hz counts (see
 * antr_channel_period_set()'s contract: 8192 is the 4 Hz most ANT+ profiles
 * use). Converting to microseconds is 1000000/32768, which reduces exactly to
 * 15625/512 - so this is integer arithmetic with no rounding at all, not an
 * approximation that happens to be close. Kept in that order (multiply then
 * divide) for the same reason: 65535 * 3 * 15625 is ~3.07e9, which is why the
 * intermediate is uint64_t rather than uint32_t.
 */
#define STALE_MULTIPLE 3u

static uint64_t stale_after_us(uint16_t period_32k)
{
	return ((uint64_t)period_32k * STALE_MULTIPLE * 15625u) / 512u;
}

struct entry {
	bool     used;
	uint8_t  field_id;
	uint64_t last_us; /* t_us of the last fresh sample seen */

	/* The record re-posted with STALE set. A copy rather than a
	 * reconstruction: section 9.2's expiry is "the last reading, now known
	 * to be old", and a sink that renders the value needs the value. */
	struct radiant_sample last;
};

/*
 * The quiet is a property of the SOURCE (see the header). `last_us` is the
 * most recent arrival from ANY field of it, and `stale` latches the one STALE
 * burst per quiet period - the same edge the per-field flag used to hold, one
 * level up, which is where the rotation stops confusing it.
 */
struct source_state {
	bool     stale;
	bool     seen;    /* at least one arrival, so last_us means something */
	uint64_t last_us;
};

static struct entry table[RADIANT_BINDING_MAX][RADIANT_LIVENESS_FIELDS_PER_SOURCE];
static struct source_state sources[RADIANT_BINDING_MAX];

static struct radiant_liveness_stats stats;

void radiant_liveness_reset(void)
{
	memset(table, 0, sizeof(table));
	memset(sources, 0, sizeof(sources));
	memset(&stats, 0, sizeof(stats));
}

const struct radiant_liveness_stats *radiant_liveness_stats_get(void)
{
	return &stats;
}

/*
 * First refusal (radiant_sink::want).
 *
 * Three exclusions, each for its own reason:
 *
 *   - STALE. This module's own output comes back round the bus. Recording it
 *     as a fresh arrival would re-arm the edge from the edge, so a source that
 *     went away would produce one STALE record every 3x period forever instead
 *     of once.
 *   - DERIVED. Rule outputs (radiant_rules.c) are published on CHANGE only -
 *     an occupancy boolean that stays false is correct and silent - so they
 *     have no arrival interval to expire against. They are covered anyway:
 *     eval_activity() forces them false the moment the accumulator they are
 *     derived from arrives marked STALE.
 *   - Anything outside the binding table. RADIANT_BRIDGE_DIAG_SOURCE is the
 *     bus's own drop counter and only moves when there are drops, so "it has
 *     been quiet" is the healthy case for it.
 */
static bool liveness_want(const struct radiant_sample *s)
{
	if ((s->flags & (RADIANT_SAMPLE_STALE | RADIANT_SAMPLE_DERIVED)) != 0u) {
		return false;
	}
	return s->source < RADIANT_BINDING_MAX;
}

static void liveness_publish(const struct radiant_sample *s)
{
	struct entry *row = table[s->source];
	struct entry *slot = NULL;
	uint32_t      i;

	for (i = 0u; i < RADIANT_LIVENESS_FIELDS_PER_SOURCE; i++) {
		if (row[i].used && row[i].field_id == s->field_id) {
			slot = &row[i];
			break;
		}
		if (!row[i].used && slot == NULL) {
			slot = &row[i]; /* remember the first free, keep looking
					 * for an exact match */
		}
	}

	if (slot == NULL) {
		/* Refuse rather than evict - see the header's note on
		 * RADIANT_LIVENESS_FIELDS_PER_SOURCE. */
		stats.no_slot++;
		return;
	}

	if (!slot->used) {
		slot->used = true;
		slot->field_id = s->field_id;
		stats.tracked++;
	}

	slot->last = *s;
	slot->last_us = s->t_us;

	/* Any arrival re-arms the whole source, including fields that were not
	 * on this page. That is the point: the device is demonstrably there. */
	sources[s->source].last_us = s->t_us;
	sources[s->source].seen = true;
	sources[s->source].stale = false;
}

uint32_t radiant_liveness_tick(uint64_t now_us)
{
	uint32_t posted = 0u;
	uint32_t src;
	uint32_t i;

	for (src = 0u; src < RADIANT_BINDING_MAX; src++) {
		const struct radiant_binding *b = radiant_binding_get(src);
		uint64_t threshold_us;

		if (b == NULL) {
			/* Unbound since the last sample. Free the row: leaving
			 * it would let a later bind() of a different device
			 * into this slot inherit a stranger's last reading. */
			for (i = 0u; i < RADIANT_LIVENESS_FIELDS_PER_SOURCE; i++) {
				if (table[src][i].used) {
					stats.tracked--;
				}
			}
			memset(table[src], 0, sizeof(table[src]));
			memset(&sources[src], 0, sizeof(sources[src]));
			continue;
		}

		if (b->period == 0u) {
			/* No period was ever set for this binding, so there is
			 * no honest expiry to apply. Counted, not guessed at -
			 * see the header. Counted per pass rather than per
			 * binding-lifetime on purpose: a single event is easy
			 * to miss in a log, a counter climbing at 1 Hz is not.
			 */
			stats.no_period++;
			continue;
		}

		threshold_us = stale_after_us(b->period);

		if (!sources[src].seen || sources[src].stale) {
			continue;
		}
		/* `now_us <= last_us` is not an error: a tick can land in the
		 * same microsecond as an arrival, and a caller whose clock has
		 * not advanced is simply early. Treat it as "not yet", never as
		 * an enormous elapsed time from an unsigned wrap. */
		if (now_us <= sources[src].last_us ||
		    (now_us - sources[src].last_us) <= threshold_us) {
			continue;
		}

		/* Latched before the loop, not inside it: the burst below is one
		 * event about one source, and a source with no tracked fields
		 * left must still not be re-examined every sweep. */
		sources[src].stale = true;

		for (i = 0u; i < RADIANT_LIVENESS_FIELDS_PER_SOURCE; i++) {
			struct entry *e = &table[src][i];
			struct radiant_sample out;

			if (!e->used) {
				continue;
			}

			out = e->last;
			out.flags |= RADIANT_SAMPLE_STALE;
			/* Stamped with the moment the expiry was DECIDED, not
			 * the moment the value was heard. A sink reading t_us
			 * to order its writes would otherwise put the expiry
			 * before the reading it expires. The value itself is
			 * unchanged; STALE is what says it is no longer to be
			 * believed. */
			out.t_us = now_us;

			stats.stale_posted++;
			posted++;
			radiant_bridge_post(&out);
		}
	}

	return posted;
}

RADIANT_SINK_DEFINE(radiant_liveness_sink, liveness_want, liveness_publish, NULL);
