/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_rules.c - see radiant_rules.h.
 */

#include <stdbool.h>
#include <string.h>

#include "radiant_binding.h"
#include "radiant_bridge.h"
#include "radiant_rules.h"

/* Section 6.1's numbers, stated in the section by name. */
#define ACTIVITY_ASSERT_US ((uint64_t)2 * 1000000u)
#define ACTIVITY_CLEAR_US  ((uint64_t)20 * 1000000u)

/* Not from the doc (see header's scope note): 3s is a guess at "long
 * enough not to chatter on a 4Hz stream, short enough to read as prompt" -
 * unlike 6.1's 2s/20s, this number isn't measured or specified. */
#define ZONE_DWELL_US ((uint64_t)3 * 1000000u)

/* Section 6.2's worked example: HR_max~N(180,15^2), HR_rest~N(65,10^2) =>
 * HRR=115, rest < 82.25, zone2 >= 134. Hardcoded rather than recomputed via
 * the Karvonen formula, since personalisation (6.3) is out of scope and
 * the inputs don't change until it lands. */
#define REST_THRESHOLD_BPM  82u
#define ZONE2_THRESHOLD_BPM 134u

/*
 * ---------------------------------------------------------------------------
 * TWO ACTIVITY SLOTS PER SOURCE, KEYED ON field_type. Read this before adding
 * a third accumulating producer.
 * ---------------------------------------------------------------------------
 * This used to be ONE struct per source with one `prev_raw` and a `field_type`
 * member recording which accumulator last wrote it. That was correct for
 * exactly as long as one adapter posted one accumulating field per source,
 * which was true while radiant_hr_adapter.c (0x36 beat count) was the only
 * producer in the tree.
 *
 * radiant_power_adapter.c posts TWO accumulating fields on ONE source: 0x36
 * event count and 0x30 energy. With a single slot the two interleave, and the
 * failure is not a lost sample - it is worse:
 *
 *   - `field_type` alternates, so the field_id at the bottom of
 *     eval_activity() alternates between WORN and ACTIVE, and one dwell state
 *     machine publishes both booleans in turn;
 *   - `prev_raw` alternates too, so each field is differenced against the
 *     OTHER field's previous value. Energy in microjoules minus an event
 *     count is a large positive number and an event count minus energy is a
 *     large negative one, so `raw_now` flaps every message regardless of
 *     whether the rider is pedalling;
 *   - eval_zone()'s "worn" gate reads that same shared `debounced`, so a
 *     power meter would drive heart-rate zone outputs.
 *
 * The fix is structural rather than a guard: each source gets its own slot for
 * RADIANT_FIELD_EVENT_COUNT (which drives RADIANT_RULE_FIELD_WORN) and one for
 * every other accumulating type (which drives RADIANT_RULE_FIELD_ACTIVE), each
 * with its own prev_raw, dwell edge and debounced output. The slot index IS
 * the discriminator, so the `field_type` member is gone - it existed only to
 * answer "which output does this drive", which the slot now answers by
 * construction and cannot get out of step with the state it labels.
 *
 * THE ACTIVE SLOT IS STILL SINGLE-OCCUPANCY. Two different non-event-count
 * accumulating types on one source (0x30 energy and 0x34 distance, say) would
 * reproduce the same defect between themselves. That is why
 * radiant_power_adapter.h reserves its FE-C elapsed-time and distance
 * field_ids instead of publishing them. A general fix is a per-field-type slot
 * map; it is not needed until a second such producer exists, and it would cost
 * RADIANT_BINDING_MAX x N of this struct in bss.
 */
#define ACTIVITY_SLOT_WORN   0u /* RADIANT_FIELD_EVENT_COUNT */
#define ACTIVITY_SLOT_ACTIVE 1u /* every other accumulating field_type */
#define ACTIVITY_SLOTS       2u

/*
 * ---------------------------------------------------------------------------
 * "ADVANCING" IS A STATE WITH A MEMORY, NOT A TEST AGAINST THE PREVIOUS SAMPLE.
 * Read this before simplifying eval_activity() back to `s->raw > prev_raw`.
 * ---------------------------------------------------------------------------
 * The obvious implementation of 6.1's "the accumulating field is advancing" is
 * "did it advance since the previous sample", fed to the same dwell helper the
 * zone path uses. That is what this file did, it passed every test it had, and
 * it could not work on any real sensor. The arithmetic:
 *
 *   An ANT+ heart-rate strap broadcasts at 8070/32768 s = 4.06 Hz. At 60 bpm a
 *   beat arrives once a second, so the beat-count accumulator advances on
 *   roughly ONE MESSAGE IN FOUR. "Advanced since the previous sample" is
 *   therefore true, false, false, false, true, false, false, false...
 *
 *   A dwell keyed on "when did that boolean last flip" restarts its edge on
 *   every one of those flips, so the time since the edge never exceeds one
 *   message period (~246 ms) and the 2 s ACTIVITY_ASSERT_US dwell is never
 *   satisfied. RADIANT_RULE_FIELD_WORN could not assert for ANY heart rate
 *   below the message rate - i.e. for any human being. The old tests passed
 *   only because their helper advanced the accumulator on every single sample,
 *   which no sensor does.
 *
 * The fix is to stop asking "did it move since last time" and start tracking
 * WHEN IT LAST MOVED, which is what 6.1's two numbers actually describe:
 *
 *   assert  = it has been advancing for ACTIVITY_ASSERT_US   (2 s)
 *   clear   = it last advanced ACTIVITY_CLEAR_US ago         (20 s)
 *
 * A gap of three quiet messages is then simply 0.74 s of a 20 s clear budget,
 * and the same property is what 6.1 demands of "bike in use": a rider coasting
 * emits messages that carry no new energy, and the output must not drop out at
 * every descent and every traffic light.
 *
 * dwell_update() is deliberately NOT used here any more. It is still exactly
 * right for the zone path, where the input is an instantaneous heart-rate
 * reading and "has the threshold test held since it last flipped" is the
 * correct question. Two different questions, two mechanisms.
 */
struct activity_state {
	bool     tracked;    /* an accumulator sample has been seen for this slot */
	bool     have_prev;
	int64_t  prev_raw;   /* the accumulator's own previous raw value, for the delta */

	bool     advancing;         /* the accumulator has moved within ACTIVITY_CLEAR_US */
	uint64_t last_advance_us;   /* t_us of the last sample with a positive delta */
	uint64_t advance_since_us;  /* t_us at which the current advancing run began */
	bool     debounced;         /* the published, dwelled output */
};

struct zone_state {
	bool    have_hr;
	uint8_t hr_bpm;

	bool     rest_raw;
	uint64_t rest_edge_us;
	bool     rest_debounced;

	bool     zone2_raw;
	uint64_t zone2_edge_us;
	bool     zone2_debounced;
};

struct rule_state {
	struct activity_state activity[ACTIVITY_SLOTS];
	struct zone_state      zone;
};

static struct rule_state states[RADIANT_BINDING_MAX];

void radiant_rules_reset(void)
{
	memset(states, 0, sizeof(states));
}

static void post_occupancy(uint32_t source, uint8_t field_id, bool occupied,
			   uint64_t t_us)
{
	struct radiant_sample s = {
		.source = source,
		.field_id = field_id,
		.field_type = RADIANT_FIELD_OCCUPANCY,
		.flags = RADIANT_SAMPLE_DERIVED,
		.exp = 0,
		.raw = occupied ? 1 : 0,
		.t_us = t_us,
	};

	radiant_bridge_post(&s);
}

/*
 * The dwell rule shared by activity and zones: `raw` is this instant's
 * threshold test, `debounced` is what was last published. `raw` must hold
 * for `assert_us`/`clear_us` (as appropriate) since it last flipped before
 * `debounced` is allowed to follow it. Returns true if `debounced` changed.
 */
static bool dwell_update(bool raw, uint64_t *edge_us, bool *raw_prev,
			 bool *debounced, uint64_t now_us, uint64_t assert_us,
			 uint64_t clear_us, bool force_now)
{
	bool changed = false;

	if (raw != *raw_prev) {
		*raw_prev = raw;
		*edge_us = now_us;
	}

	if (force_now) {
		if (*debounced != raw) {
			*debounced = raw;
			changed = true;
		}
		return changed;
	}

	if (raw && !*debounced && (now_us - *edge_us) >= assert_us) {
		*debounced = true;
		changed = true;
	} else if (!raw && *debounced && (now_us - *edge_us) >= clear_us) {
		*debounced = false;
		changed = true;
	}

	return changed;
}

static void eval_activity(uint32_t source, const struct radiant_sample *s)
{
	/* The slot, not a member of the slot: see the block comment on
	 * struct activity_state. 0x36 is the only type that drives WORN. */
	uint32_t slot = (s->field_type == RADIANT_FIELD_EVENT_COUNT)
				? ACTIVITY_SLOT_WORN
				: ACTIVITY_SLOT_ACTIVE;
	struct activity_state *a = &states[source].activity[slot];
	uint8_t  field_id = (slot == ACTIVITY_SLOT_WORN)
				    ? RADIANT_RULE_FIELD_WORN
				    : RADIANT_RULE_FIELD_ACTIVE;
	bool     stale = (s->flags & RADIANT_SAMPLE_STALE) != 0u;
	uint64_t t = s->t_us;
	bool     desired;

	a->tracked = true;

	if (!stale) {
		int64_t delta;

		if (!a->have_prev) {
			a->have_prev = true;
			a->prev_raw = s->raw;
			return; /* nothing to difference against yet */
		}

		delta = s->raw - a->prev_raw;
		a->prev_raw = s->raw;

		if (delta > 0) {
			/* A run of advancing starts at the FIRST positive
			 * delta, not at every one of them: re-stamping
			 * advance_since_us on each beat is precisely the bug
			 * this replaced, and would push the assert out
			 * forever. */
			if (!a->advancing) {
				a->advance_since_us = t;
			}
			a->advancing = true;
			a->last_advance_us = t;
		} else if (a->advancing &&
			   (t - a->last_advance_us) >= ACTIVITY_CLEAR_US) {
			a->advancing = false;
		}

		/* Both of 6.1's numbers, in one line: advancing now, and long
		 * enough that the 2 s assert dwell is satisfied. The clear side
		 * is not a second dwell - it is the moment `advancing` above
		 * goes false, 20 s after the accumulator last moved. */
		desired = a->advancing &&
			  (t - a->advance_since_us) >= ACTIVITY_ASSERT_US;
	} else {
		/* Liveness overrides the dwell entirely (see header's scope
		 * note - this module reacts to STALE but doesn't produce it).
		 * The advancing run is abandoned too, so a source that comes
		 * back on air must earn its 2 s again rather than resuming a
		 * run that started before the outage. */
		a->advancing = false;
		a->prev_raw = s->raw;
		desired = false;
	}

	if (desired != a->debounced) {
		a->debounced = desired;
		post_occupancy(source, field_id, a->debounced, t);
	}

	if (stale) {
		/* Zones are gated on "worn"; losing liveness must drop them
		 * too, immediately (section 6.2). Deliberately NOT gated on
		 * the slot: STALE is a property of the source's liveness
		 * (radiant_liveness.c stamps it per binding), so whichever
		 * accumulator carries the flag, the strap is off the air. */
		struct zone_state *z = &states[source].zone;

		if (z->rest_debounced) {
			z->rest_debounced = false;
			post_occupancy(source, RADIANT_RULE_FIELD_AT_REST, false,
				       s->t_us);
		}
		if (z->zone2_debounced) {
			z->zone2_debounced = false;
			post_occupancy(source, RADIANT_RULE_FIELD_ZONE2, false,
				       s->t_us);
		}
	}
}

static void eval_zone(uint32_t source, const struct radiant_sample *s)
{
	struct zone_state    *z = &states[source].zone;
	const struct activity_state *a =
		&states[source].activity[ACTIVITY_SLOT_WORN];
	bool               worn;
	bool               changed;

	z->have_hr = true;
	z->hr_bpm = (uint8_t)s->raw;

	/* "Both require the monitor to be worn" (section 6.2) - gated on the
	 * same binding's WORN output (0x36-derived), not ACTIVE. Reading the
	 * EVENT_COUNT slot specifically is what enforces that now; it used to
	 * be a field_type test against shared state, which a second producer
	 * could overwrite between the two calls. */
	worn = a->tracked && a->debounced;

	changed = dwell_update(worn && z->hr_bpm < REST_THRESHOLD_BPM,
			       &z->rest_edge_us, &z->rest_raw, &z->rest_debounced,
			       s->t_us, ZONE_DWELL_US, ZONE_DWELL_US, !worn);
	if (changed) {
		post_occupancy(source, RADIANT_RULE_FIELD_AT_REST, z->rest_debounced,
			       s->t_us);
	}

	changed = dwell_update(worn && z->hr_bpm >= ZONE2_THRESHOLD_BPM,
			       &z->zone2_edge_us, &z->zone2_raw, &z->zone2_debounced,
			       s->t_us, ZONE_DWELL_US, ZONE_DWELL_US, !worn);
	if (changed) {
		post_occupancy(source, RADIANT_RULE_FIELD_ZONE2, z->zone2_debounced,
			       s->t_us);
	}
}

static bool rules_want(const struct radiant_sample *s)
{
	if ((s->flags & RADIANT_SAMPLE_DERIVED) != 0u) {
		/* Never re-evaluate this module's own output - the loop
		 * guard section 6 implies. */
		return false;
	}
	if (s->source == RADIANT_BRIDGE_DIAG_SOURCE) {
		return false;
	}
	if (s->source >= RADIANT_BINDING_MAX) {
		/* No rule state slot for a source outside the binding table's
		 * range. Declining rather than asserting. */
		return false;
	}
	return (s->flags & RADIANT_SAMPLE_ACCUMULATING) != 0u ||
	       s->field_type == RADIANT_FIELD_HEART_RATE;
}

static void rules_publish(const struct radiant_sample *s)
{
	if ((s->flags & RADIANT_SAMPLE_ACCUMULATING) != 0u) {
		eval_activity(s->source, s);
	} else if (s->field_type == RADIANT_FIELD_HEART_RATE) {
		eval_zone(s->source, s);
	}
}

RADIANT_SINK_DEFINE(radiant_rules_sink, rules_want, rules_publish, NULL);
