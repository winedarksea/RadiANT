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

/*
 * NOT FROM THE DOC - see the header's scope note. Three seconds is a guess at
 * "long enough that a 4 Hz stream crossing a threshold once does not chatter,
 * short enough that a real zone change reads as prompt", the same shape of
 * reasoning section 6.2 gives for needing a dwell at all, but with no
 * measured or specified number behind THIS constant the way 6.1's 2 s/20 s
 * are behind ACTIVITY_ASSERT_US/ACTIVITY_CLEAR_US.
 */
#define ZONE_DWELL_US ((uint64_t)3 * 1000000u)

/* Section 6.2's worked example, transcribed: HR_max ~ N(180, 15^2),
 * HR_rest ~ N(65, 10^2) => HRR = 115, rest < HR_rest + 0.15*HRR = 82.25,
 * zone2 >= HR_rest + 0.60*HRR = 134. Hardcoded rather than recomputed from
 * the two priors at runtime: recomputing would need the floating-point
 * Karvonen formula section 6.3's personalisation requires anyway, and until
 * that lands (explicitly out of scope here - see the header) the inputs
 * never change, so carrying the formula would be a second, unexercised
 * implementation of arithmetic the section itself already solved once.
 */
#define REST_THRESHOLD_BPM  82u
#define ZONE2_THRESHOLD_BPM 134u

struct activity_state {
	bool     tracked;    /* an accumulator sample has been seen for this source */
	uint8_t  field_type; /* which one: RADIANT_FIELD_EVENT_COUNT or _ENERGY */
	bool     have_prev;
	int64_t  prev_raw;   /* the accumulator's own previous raw value, for the delta */

	bool     edge_track; /* dwell_update()'s memory of the last raw test result -
			      * NOT the same thing as prev_raw above, which is the
			      * accumulator's value; this is the boolean "was it
			      * advancing" that the dwell timer edges on */
	uint64_t raw_edge_us;
	bool     debounced;  /* the published, dwelled output */
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
	struct activity_state activity;
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
	struct activity_state *a = &states[source].activity;
	bool                changed;
	bool                stale = (s->flags & RADIANT_SAMPLE_STALE) != 0u;
	bool                raw_now;

	a->tracked = true;
	a->field_type = s->field_type;

	if (stale) {
		/* Liveness overrides the ordinary dwell entirely - see the
		 * header's scope note on why this module reacts to STALE
		 * without producing it. */
		raw_now = false;
	} else if (a->have_prev) {
		raw_now = (s->raw - a->prev_raw) > 0;
	} else {
		a->have_prev = true;
		a->prev_raw = s->raw;
		return; /* nothing to difference against yet, same as the HR
			 * adapter's own first-message rule */
	}
	a->prev_raw = s->raw;

	changed = dwell_update(raw_now, &a->raw_edge_us, &a->edge_track,
			       &a->debounced, s->t_us, ACTIVITY_ASSERT_US,
			       ACTIVITY_CLEAR_US, stale);
	if (changed) {
		uint8_t field_id = (s->field_type == RADIANT_FIELD_EVENT_COUNT)
					   ? RADIANT_RULE_FIELD_WORN
					   : RADIANT_RULE_FIELD_ACTIVE;
		post_occupancy(source, field_id, a->debounced, s->t_us);
	}

	if (stale) {
		/* Zones are gated on "worn"; losing liveness must drop them
		 * too, immediately, for the same reason section 6.2 gates
		 * them on worn in the first place - a strap not heard from
		 * is not a strap correctly reporting "at rest". */
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
	const struct activity_state *a = &states[source].activity;
	bool               worn;
	bool               changed;

	z->have_hr = true;
	z->hr_bpm = (uint8_t)s->raw;

	/*
	 * "Both require the monitor to be worn" (section 6.2) - gated on the
	 * SAME binding's activity output, and specifically the WORN one
	 * (0x36-derived), not ACTIVE: a power meter binding has no HR to
	 * evaluate a zone against in the first place, so field_type is the
	 * discriminator rather than an extra flag.
	 */
	worn = a->tracked && a->debounced &&
	       a->field_type == RADIANT_FIELD_EVENT_COUNT;

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
		 * guard section 6's opening paragraph implies by saying
		 * derived records reach sinks "through the same path", not
		 * "through the same path including this one". */
		return false;
	}
	if (s->source == RADIANT_BRIDGE_DIAG_SOURCE) {
		return false;
	}
	if (s->source >= RADIANT_BINDING_MAX) {
		/* No rule state slot for a source outside the binding table's
		 * range - see radiant_binding.h. Declining rather than
		 * asserting: a caller posting samples ahead of ever binding
		 * anything is a sequencing choice this module does not police. */
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
