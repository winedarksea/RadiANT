/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, against radiant_rules.h/.c, which is
 * itself a transcription of docs/radiant-bridge.md sections 6.1 and 6.2.
 *
 * Five things earn their place: the accumulator rule needs a second sample
 * (6.1 is about a DELTA); the dwell is asymmetric in both directions (~2 s to
 * assert, ~20 s to clear); zones are gated on worn (6.2: a strap on a table
 * must never read "at rest"); the thresholds are 6.2's own worked numbers
 * (rest < 82, zone2 >= 134); and derived outputs must not re-trigger the rule
 * that made them (rules_want() is the loop guard for this).
 *
 * A sixth was added with radiant_power_adapter.c, which is the first producer
 * to post TWO accumulating fields on ONE source (0x36 event count and 0x30
 * energy). The three cases at the bottom of this file are the regression for
 * the two-slot fix in radiant_rules.c: before it, one shared prev_raw and one
 * shared dwell served both fields, so each was differenced against the other's
 * previous value and one state machine published WORN and ACTIVE alternately.
 * Every case above this line is the HR-only path and must stay bit-identical.
 *
 * A seventh group is the 4 Hz cadence block at the bottom. Every case written
 * before it advances the accumulator on EVERY sample, which no sensor does -
 * and that is why the whole suite passed while RADIANT_RULE_FIELD_WORN could
 * not assert for any heart rate below the message rate. Those cases post the
 * cadence a real strap and a real trainer produce; see the comment above them.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include <radiant/radiant_binding.h>
#include <radiant/radiant_bridge.h>
#include <radiant/radiant_hr_profile.h>
#include <radiant/radiant_rules.h>

#define CAP_MAX 128

/* The source's default, repeated: radiant/tests does not enable
 * RADIANT_BRIDGE_RULES, so the Kconfig symbol is never defined here. */
#ifndef CONFIG_RADIANT_BRIDGE_RULES_EQUIPMENT_CLEAR_S
#define CONFIG_RADIANT_BRIDGE_RULES_EQUIPMENT_CLEAR_S 30
#endif
#define EQUIPMENT_CLEAR_S ((uint32_t)CONFIG_RADIANT_BRIDGE_RULES_EQUIPMENT_CLEAR_S)

static struct radiant_sample cap_buf[CAP_MAX];
/* cap_first[i]: cap_buf[i] was the first record of its (source, field_id)
 * since the last rules_test_reset() - the announcement emit() makes on an
 * output's first input. Tracked across a test's own `cap_n = 0u` resets. */
static bool              cap_first[CAP_MAX];
static uint32_t          cap_n;
static uint8_t           cap_seen[RADIANT_BINDING_MAX]; /* BIT(field_id) */

static bool rule_cap_want(const struct radiant_sample *s)
{
	/* Only what a rule produced, so a test does not have to pick its own
	 * injected samples back out of the capture. */
	return (s->flags & RADIANT_SAMPLE_DERIVED) != 0u;
}

static void rule_cap_publish(const struct radiant_sample *s)
{
	bool first = false;

	if (s->source < RADIANT_BINDING_MAX && s->field_id < 8u) {
		first = (cap_seen[s->source] & BIT(s->field_id)) == 0u;
		cap_seen[s->source] |= (uint8_t)BIT(s->field_id);
	}
	if (cap_n < CAP_MAX) {
		cap_first[cap_n] = first;
		cap_buf[cap_n++] = *s;
	}
}

RADIANT_SINK_DEFINE(test_rule_cap_sink, rule_cap_want, rule_cap_publish, NULL);

static void rules_test_reset(void *unused)
{
	ARG_UNUSED(unused);
	radiant_bridge_reset();
	radiant_binding_init();
	radiant_rules_reset();
	radiant_hr_profile_set_store(NULL);
	radiant_hr_profile_reset();
	memset(cap_buf, 0, sizeof(cap_buf));
	memset(cap_first, 0, sizeof(cap_first));
	memset(cap_seen, 0, sizeof(cap_seen));
	cap_n = 0u;
}

/* The bit a test would set by binding/unbinding through the real self-channel
 * path: announce the change to every sink, rules included. */
static void announce_binding(uint32_t source)
{
	radiant_bridge_binding_changed(source, radiant_binding_get(source));
	memset(&cap_seen[source], 0, sizeof(cap_seen[source]));
}

/* Find the LAST captured derived sample with this field_id, or NULL. Tests
 * care about the most recent verdict, not the whole history. */
static const struct radiant_sample *last_with(uint8_t field_id)
{
	int32_t i;

	for (i = (int32_t)cap_n - 1; i >= 0; i--) {
		if (cap_buf[i].field_id == field_id) {
			return &cap_buf[i];
		}
	}
	return NULL;
}

/* The latest verdict for this output is "occupied". An output that has only
 * been announced (false) or never posted at all is not. */
static bool is_on(uint8_t field_id)
{
	const struct radiant_sample *s = last_with(field_id);

	return s != NULL && s->raw != 0;
}

/* How many times this output CHANGED. The rules publish on change (plus one
 * `false` announcement on an output's first input, which is not a change and
 * is not counted), so this is the chatter count: a boolean that is meant to
 * assert once and hold must produce exactly one record over a long stream,
 * and a count above one is the flapping failure, not merely a noisy log. */
static uint32_t count_with(uint8_t field_id)
{
	uint32_t i;
	uint32_t n = 0u;

	for (i = 0u; i < cap_n; i++) {
		if (cap_buf[i].field_id == field_id &&
		    !(cap_first[i] && cap_buf[i].raw == 0)) {
			n++;
		}
	}
	return n;
}

static void post_beats(uint32_t source, int64_t raw, uint64_t t_us)
{
	struct radiant_sample s = {
		.source = source,
		.field_id = 0u,
		.field_type = RADIANT_FIELD_EVENT_COUNT,
		.flags = RADIANT_SAMPLE_ACCUMULATING,
		.exp = 0,
		.raw = raw,
		.t_us = t_us,
	};

	radiant_bridge_post(&s);
	radiant_bridge_drain();
}

static void post_hr(uint32_t source, int64_t bpm, uint64_t t_us)
{
	struct radiant_sample s = {
		.source = source,
		.field_id = 0u,
		.field_type = RADIANT_FIELD_HEART_RATE,
		.flags = 0u,
		.exp = 0,
		.raw = bpm,
		.t_us = t_us,
	};

	radiant_bridge_post(&s);
	radiant_bridge_drain();
}

/*
 * The other accumulating series a real source posts alongside the event count:
 * 0x30 energy, in microjoules at exp -6, exactly as radiant_power_adapter.c
 * publishes it. A different field_id AND a different field_type, which is what
 * makes it land in the other activity slot.
 */
static void post_energy(uint32_t source, int64_t raw_uj, uint64_t t_us)
{
	struct radiant_sample s = {
		.source = source,
		.field_id = 2u,
		.field_type = RADIANT_FIELD_ENERGY,
		.flags = RADIANT_SAMPLE_ACCUMULATING,
		.exp = -6,
		.raw = raw_uj,
		.t_us = t_us,
	};

	radiant_bridge_post(&s);
	radiant_bridge_drain();
}

/*
 * The FE state, exactly as radiant_power_adapter.c publishes it: field_id 0x08,
 * type 0x40 ENUM_GENERIC, instantaneous, raw = PROFILE_FEC_STATE_*. Posting it
 * with the real field_id rather than 0 matters here, because the whole defect
 * this rule fixes was that nothing read this field at all.
 */
#define FEC_STATE_READY    2
#define FEC_STATE_IN_USE   3
#define FEC_STATE_FINISHED 4

static void post_fe_state(uint32_t source, int64_t state, uint64_t t_us,
			  uint8_t extra_flags)
{
	struct radiant_sample s = {
		.source = source,
		.field_id = 0x08u,
		.field_type = RADIANT_FIELD_ENUM_GENERIC,
		.flags = extra_flags,
		.exp = 0,
		.raw = state,
		.t_us = t_us,
	};

	radiant_bridge_post(&s);
	radiant_bridge_drain();
}

/*
 * THE SECOND ENUM_GENERIC SERIES ON THE SAME SOURCE, and the reason
 * radiant_rules.c gates on field_id and not only on field_type.
 *
 * radiant_power_adapter.c posts the FE-C EQUIPMENT TYPE (Table 8-8: treadmill
 * 19, rower 22, indoor bike 25) on field_id 0x0D, as ENUM_GENERIC, from the
 * same page 16 as the state above - it is what radiant_naming.c turns into
 * "Treadmill 51234 In Use". Every one of those codes is != FEC_STATE_IN_USE,
 * so a rule engine that routes on the TYPE alone reads each one as "not in
 * use" and fights the real state sample over one shared slot, four times a
 * second.
 */
static void post_fe_type(uint32_t source, int64_t type, uint64_t t_us)
{
	struct radiant_sample s = {
		.source = source,
		.field_id = 0x0Du,
		.field_type = RADIANT_FIELD_ENUM_GENERIC,
		.flags = 0u,
		.exp = 0,
		.raw = type,
		.t_us = t_us,
	};

	radiant_bridge_post(&s);
	radiant_bridge_drain();
}

#define FEC_TYPE_TREADMILL 19
#define FEC_TYPE_ROWER     22

#define US_PER_S ((uint64_t)1000000u)

ZTEST(radiant_rules, test_activity_needs_a_second_sample)
{
	uint32_t src;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);
	post_beats(src, 10, 0u);
	zassert_false(is_on(RADIANT_RULE_FIELD_WORN));
}

ZTEST(radiant_rules, test_activity_asserts_after_dwell_not_before)
{
	uint32_t src;
	const struct radiant_sample *w;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);
	post_beats(src, 0, 0u);
	post_beats(src, 1, 1u * US_PER_S); /* advancing, but only 1 s in */

	w = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_true(w == NULL || w->raw == 0);

	post_beats(src, 2, 3u * US_PER_S); /* advancing continuously past 2 s */
	w = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_not_null(w);
	zassert_equal(w->raw, 1, NULL);
	zassert_equal(w->field_type, RADIANT_FIELD_OCCUPANCY, NULL);
	zassert_equal(w->source, src, NULL);
}

ZTEST(radiant_rules, test_activity_clears_after_its_own_longer_dwell)
{
	uint32_t src;
	const struct radiant_sample *w;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);
	post_beats(src, 0, 0u);
	post_beats(src, 1, 1u * US_PER_S); /* raw turns true here; edge starts here */
	post_beats(src, 2, 3u * US_PER_S); /* 2 s since the edge: asserts */
	zassert_true(is_on(RADIANT_RULE_FIELD_WORN));

	/*
	 * Stops advancing (the accumulator holds at 2 - a real counter does
	 * not go backwards). The clear-side edge starts at this call, not at
	 * the moment the assert happened, so the dwell below is measured from
	 * t=13 s, not from t=3 s.
	 */
	cap_n = 0u;
	post_beats(src, 2, 13u * US_PER_S);
	zassert_is_null(last_with(RADIANT_RULE_FIELD_WORN));

	post_beats(src, 2, 20u * US_PER_S); /* 7 s since the edge: still under 20 s */
	zassert_is_null(last_with(RADIANT_RULE_FIELD_WORN));

	post_beats(src, 2, 34u * US_PER_S); /* 21 s since the edge: past it */
	w = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_not_null(w);
	zassert_equal(w->raw, 0, NULL);
}

ZTEST(radiant_rules, test_zones_never_assert_while_not_worn)
{
	uint32_t src;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);
	/* No activity posted at all - never worn. A resting-range HR value
	 * must not produce "at rest". */
	post_hr(src, 60, 0u);
	post_hr(src, 60, 4u * US_PER_S);
	post_hr(src, 60, 8u * US_PER_S);

	zassert_false(is_on(RADIANT_RULE_FIELD_AT_REST));
	zassert_false(is_on(RADIANT_RULE_FIELD_ZONE2));
}

/* Page 84 subpage 6's charge-cycles total, exactly as radiant_common_adapter.c
 * posts it: EVENT_COUNT + ACCUMULATING, in the common field_id block, on the
 * same source as the profile's own beat count. */
static void post_cycles(uint32_t source, int64_t raw, uint64_t t_us)
{
	struct radiant_sample s = {
		.source = source,
		.field_id = 0x26u,
		.field_type = RADIANT_FIELD_EVENT_COUNT,
		.flags = RADIANT_SAMPLE_ACCUMULATING | RADIANT_SAMPLE_ROTATED,
		.exp = 0,
		.raw = raw,
		.t_us = t_us,
	};

	radiant_bridge_post(&s);
	radiant_bridge_drain();
}

/*
 * A frozen strap still sending page 84 must never read as worn (review
 * 2026-10-31, C1-2). The charge-cycles counter shared the WORN slot's
 * prev_raw with the beat count, so every interleave (500, 3, 500) was a +497
 * "advance" and WORN asserted after the dwell and never cleared.
 */
ZTEST(radiant_rules, test_a_common_page_counter_does_not_drive_worn)
{
	uint32_t src;
	uint64_t t;
	uint32_t step = 0u;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);

	for (t = 0u; t <= 10u * US_PER_S; t += 250000u) {
		post_beats(src, 500, t);
		if ((step++ % 4u) == 0u) {
			post_cycles(src, 3, t);
		}
	}

	zassert_false(is_on(RADIANT_RULE_FIELD_WORN));
}

/*
 * HR 0 is the profile's "no reading" (radiant_profile_hr.h), posted during a skin-
 * contact dropout for liveness's sake. It must not satisfy the < 82 bpm rest
 * threshold (review 2026-10-31, C2-2): WORN holds for 20 s after the beats
 * stop, so the dropout used to assert AT_REST after the 3 s zone dwell.
 */
ZTEST(radiant_rules, test_a_zero_heart_rate_is_not_at_rest)
{
	uint32_t src;
	uint64_t t = 0u;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);

	post_beats(src, 0, t);
	t += 1u * US_PER_S;
	post_beats(src, 1, t);
	t += 2u * US_PER_S;
	post_beats(src, 2, t);
	zassert_true(is_on(RADIANT_RULE_FIELD_WORN));

	cap_n = 0u;
	post_hr(src, 100, t);
	post_hr(src, 0, t + 1u * US_PER_S);
	post_hr(src, 0, t + 5u * US_PER_S);
	zassert_false(is_on(RADIANT_RULE_FIELD_AT_REST));
}

/* The thresholds at their exact boundaries: 82 is not resting, 133 is not
 * zone 2, and a zone held for exactly the dwell asserts (>=, not >). Neither
 * 82 nor 133 was ever posted, and dwell edges were only checked 1 s past
 * (review 2026-10-31, E3-6). */
ZTEST(radiant_rules, test_zone_boundaries_82_is_not_rest_and_133_is_not_zone2)
{
	uint32_t src;
	uint64_t t = 0u;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);
	post_beats(src, 0, t);
	t += 1u * US_PER_S;
	post_beats(src, 1, t);
	t += 2u * US_PER_S;
	post_beats(src, 2, t);
	zassert_true(is_on(RADIANT_RULE_FIELD_WORN));

	cap_n = 0u;
	post_hr(src, 82, t);
	t += 4u * US_PER_S;
	post_hr(src, 82, t);
	zassert_false(is_on(RADIANT_RULE_FIELD_AT_REST));
	post_hr(src, 133, t);
	t += 4u * US_PER_S;
	post_hr(src, 133, t);
	zassert_false(is_on(RADIANT_RULE_FIELD_ZONE2));

	/* Exactly the zone dwell asserts (>=, not >). */
	post_hr(src, 81, t);
	t += 3u * US_PER_S;
	post_hr(src, 81, t);
	zassert_true(is_on(RADIANT_RULE_FIELD_AT_REST));
}

ZTEST(radiant_rules, test_zone_thresholds_are_82_and_134)
{
	uint32_t src;
	uint64_t t = 0u;
	const struct radiant_sample *at_rest;
	const struct radiant_sample *zone2;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);

	/* Establish worn: three calls, exactly the pattern
	 * test_activity_asserts_after_dwell_not_before proves - the edge
	 * starts at the first true delta, and the assert needs 2 s to have
	 * passed since THAT call, not since the baseline. */
	post_beats(src, 0, t);
	t += 1u * US_PER_S;
	post_beats(src, 1, t);
	t += 2u * US_PER_S;
	post_beats(src, 2, t);
	zassert_true(is_on(RADIANT_RULE_FIELD_WORN));

	/* Below the rest threshold, held past the zone dwell. */
	cap_n = 0u;
	post_hr(src, 81, t);
	t += 4u * US_PER_S;
	post_hr(src, 81, t);
	at_rest = last_with(RADIANT_RULE_FIELD_AT_REST);
	zassert_not_null(at_rest);
	zassert_equal(at_rest->raw, 1, NULL);
	zassert_false(is_on(RADIANT_RULE_FIELD_ZONE2));

	/* At the zone2 threshold, held past the zone dwell. */
	cap_n = 0u;
	post_hr(src, 134, t);
	t += 4u * US_PER_S;
	post_hr(src, 134, t);
	zone2 = last_with(RADIANT_RULE_FIELD_ZONE2);
	zassert_not_null(zone2);
	zassert_equal(zone2->raw, 1, NULL);
	/* And the earlier at-rest output must have cleared, not stuck true. */
	at_rest = last_with(RADIANT_RULE_FIELD_AT_REST);
	zassert_not_null(at_rest, NULL);
	zassert_equal(at_rest->raw, 0);

	/* Between the two thresholds, held past the zone dwell: zone 2 must
	 * CLEAR and at-rest must stay clear. cap_n is deliberately not reset -
	 * with change-only publishing, no new event for an already-false output
	 * is correct, not silent. */
	t += 1u * US_PER_S;
	post_hr(src, 100, t);
	t += 4u * US_PER_S;
	post_hr(src, 100, t);
	at_rest = last_with(RADIANT_RULE_FIELD_AT_REST);
	zone2 = last_with(RADIANT_RULE_FIELD_ZONE2);
	zassert_not_null(at_rest, NULL);
	zassert_equal(at_rest->raw, 0);
	zassert_not_null(zone2, NULL);
	zassert_equal(zone2->raw, 0);
}

ZTEST(radiant_rules, test_stale_forces_immediate_clear)
{
	uint32_t src;
	uint64_t t = 0u;
	struct radiant_sample s;
	const struct radiant_sample *w;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);
	post_beats(src, 0, t);
	t += 1u * US_PER_S;
	post_beats(src, 1, t);
	t += 2u * US_PER_S;
	post_beats(src, 2, t);
	zassert_true(is_on(RADIANT_RULE_FIELD_WORN));

	cap_n = 0u;
	/* One instant later - nowhere near the 20 s clear dwell - but marked
	 * STALE. Must clear immediately, not wait out the dwell. */
	memset(&s, 0, sizeof(s));
	s.source = src;
	s.field_type = RADIANT_FIELD_EVENT_COUNT;
	s.flags = RADIANT_SAMPLE_ACCUMULATING | RADIANT_SAMPLE_STALE;
	s.raw = 1; /* unchanged - irrelevant under STALE */
	s.t_us = t + 1u;
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	w = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_not_null(w);
	zassert_equal(w->raw, 0);
}

/* ---------------------------------------------------------------------------
 * Two accumulating producers on one source (the A5 fix)
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_rules, test_two_accumulators_on_one_source_are_independent)
{
	uint32_t src;
	uint64_t t = 0u;
	const struct radiant_sample *w;
	const struct radiant_sample *a;

	/* A trainer: device type 0x11, posting both series every message the
	 * way radiant_power_adapter.c does. */
	radiant_binding_bind(1u, 0x11u, 1u, NULL, &src);

	post_beats(src, 0, t);
	post_energy(src, 0, t);

	/* Both start advancing here, so both dwell edges start here. Note the
	 * two raws are wildly different magnitudes - 1 against 200000000 - and
	 * that is the whole point: a shared prev_raw would difference one
	 * against the other and produce a huge positive delta one message and a
	 * huge negative one the next. */
	t += 1u * US_PER_S;
	post_beats(src, 1, t);
	post_energy(src, INT64_C(200000000), t);

	zassert_false(is_on(RADIANT_RULE_FIELD_WORN));
	zassert_false(is_on(RADIANT_RULE_FIELD_ACTIVE));

	t += 2u * US_PER_S;
	post_beats(src, 2, t);
	post_energy(src, INT64_C(400000000), t);

	w = last_with(RADIANT_RULE_FIELD_WORN);
	a = last_with(RADIANT_RULE_FIELD_ACTIVE);
	zassert_not_null(w);
	zassert_not_null(a);
	zassert_equal(w->raw, 1);
	zassert_equal(a->raw, 1);
	zassert_equal(w->field_type, RADIANT_FIELD_OCCUPANCY);
	zassert_equal(a->field_type, RADIANT_FIELD_OCCUPANCY);
}

ZTEST(radiant_rules, test_one_accumulator_may_clear_while_the_other_holds)
{
	uint32_t src;
	uint64_t t = 0u;
	const struct radiant_sample *w;
	const struct radiant_sample *a;

	radiant_binding_bind(1u, 0x11u, 1u, NULL, &src);

	post_beats(src, 0, t);
	post_energy(src, 0, t);
	t += 1u * US_PER_S;
	post_beats(src, 1, t);
	post_energy(src, INT64_C(200000000), t);
	t += 2u * US_PER_S;
	post_beats(src, 2, t);
	post_energy(src, INT64_C(400000000), t);
	zassert_true(is_on(RADIANT_RULE_FIELD_WORN));
	zassert_true(is_on(RADIANT_RULE_FIELD_ACTIVE));

	/*
	 * The rider coasts: the sensor keeps updating (event count still
	 * climbing) but the energy integral stops. ACTIVE must clear on its own
	 * 20 s dwell and WORN must not move - two dwells, two edges, one
	 * source. With one shared slot this could not even be expressed.
	 */
	cap_n = 0u;
	t += 1u * US_PER_S;
	post_beats(src, 3, t);
	post_energy(src, INT64_C(400000000), t); /* clear-side edge starts here */

	t += 25u * US_PER_S;
	post_beats(src, 12, t);
	post_energy(src, INT64_C(400000000), t);

	a = last_with(RADIANT_RULE_FIELD_ACTIVE);
	zassert_not_null(a);
	zassert_equal(a->raw, 0);

	w = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_true(w == NULL || w->raw == 0);
}

ZTEST(radiant_rules, test_zones_read_the_event_count_slot_specifically)
{
	uint32_t src;
	uint64_t t = 0u;

	/* A source with an advancing 0x30 energy accumulator and NO 0x36 at
	 * all: ACTIVE asserts, WORN never does, and section 6.2's gate is on
	 * WORN. A shared slot let ACTIVE's debounced output stand in for it. */
	radiant_binding_bind(1u, 0x11u, 1u, NULL, &src);

	post_energy(src, 0, t);
	t += 1u * US_PER_S;
	post_energy(src, INT64_C(200000000), t);
	t += 2u * US_PER_S;
	post_energy(src, INT64_C(400000000), t);
	zassert_true(is_on(RADIANT_RULE_FIELD_ACTIVE));
	zassert_false(is_on(RADIANT_RULE_FIELD_WORN));

	/* Resting-range heart rate, held well past the zone dwell. */
	post_hr(src, 60, t);
	t += 4u * US_PER_S;
	post_hr(src, 60, t);
	t += 4u * US_PER_S;
	post_hr(src, 60, t);

	zassert_false(is_on(RADIANT_RULE_FIELD_AT_REST));
	zassert_false(is_on(RADIANT_RULE_FIELD_ZONE2));
}

/* ---------------------------------------------------------------------------
 * A REAL MESSAGE CADENCE. These are the regression for the "advanced since the
 * previous sample" defect.
 * ---------------------------------------------------------------------------
 * Every case above this line advances the accumulator on EVERY sample, and
 * that is exactly why they all passed while RADIANT_RULE_FIELD_WORN could not
 * assert on any real strap. An ANT+ heart-rate channel period is 8070/32768 s
 * = 4.06 Hz, and at 60 bpm one beat arrives per second - so the beat count
 * moves on about one message in four and holds on the other three.
 *
 * Under the old rule ("did raw advance since the previous sample", fed to a
 * dwell keyed on when that boolean last flipped) the flip happened every
 * message, the dwell edge restarted every message, and 2 s never elapsed. The
 * cases below post the cadence a sensor actually produces, so they fail
 * against that construction and pass against "when did the accumulator last
 * move".
 */
#define HR_MSG_US ((uint64_t)246277u) /* 8070/32768 s, the ANT+ HR channel period */

ZTEST(radiant_rules, test_worn_asserts_on_a_4hz_stream_at_60bpm)
{
	uint32_t src;
	uint32_t i;
	const struct radiant_sample *w;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);

	/*
	 * 121 messages = 29.8 s of a 60 bpm strap. The accumulator is the
	 * RUNNING TOTAL of beats (radiant_hr_adapter.c posts exactly this), so
	 * at one beat per second it is simply the whole number of elapsed
	 * seconds: 0,0,0,0,1,1,1,1,2,... - it advances on messages 5, 9, 13...
	 *
	 * Expected: first advance at t = 1.231 s (message 5), so the 2 s assert
	 * dwell is satisfied at t = 3.231 s, which is message 14 (t = 3.448 s).
	 * Nothing may assert before 3 s, and nothing may clear for the rest of
	 * the run - the longest quiet gap is three messages, 0.74 s, against a
	 * 20 s clear budget.
	 */
	for (i = 0u; i <= 120u; i++) {
		uint64_t t = (uint64_t)i * HR_MSG_US;

		post_beats(src, (int64_t)(t / US_PER_S), t);

		if (t < 3u * US_PER_S) {
			zassert_false(is_on(RADIANT_RULE_FIELD_WORN));
		}
	}

	w = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_not_null(w);
	zassert_equal(w->raw, 1);
	zassert_equal(count_with(RADIANT_RULE_FIELD_WORN), 1u);
}

ZTEST(radiant_rules, test_bike_in_use_survives_a_coast)
{
	uint32_t src;
	uint32_t i;
	const struct radiant_sample *a;

	/* Plan step G4: a rider coasting produces messages carrying no new
	 * energy. "Bike in use" must not drop out at every descent and every
	 * traffic light - 6.1's asymmetric dwell exists for this. */
	radiant_binding_bind(1u, 0x11u, 1u, NULL, &src);

	/* ~5 s pedalling: the integrator adds work every message (250 mJ). */
	for (i = 0u; i <= 20u; i++) {
		post_energy(src, (int64_t)i * INT64_C(250000),
			    (uint64_t)i * HR_MSG_US);
	}
	a = last_with(RADIANT_RULE_FIELD_ACTIVE);
	zassert_not_null(a);
	zassert_equal(a->raw, 1);

	/* ~5 s coasting: same message rate, the accumulator holds at its last
	 * value. Well inside the 20 s clear budget, so nothing may be
	 * published at all. */
	cap_n = 0u;
	for (i = 21u; i <= 40u; i++) {
		post_energy(src, INT64_C(20) * INT64_C(250000),
			    (uint64_t)i * HR_MSG_US);
		zassert_is_null(last_with(RADIANT_RULE_FIELD_ACTIVE));
	}

	/* Pedalling resumes; still no transition, because it never cleared. */
	for (i = 41u; i <= 60u; i++) {
		post_energy(src, (int64_t)(i - 20u) * INT64_C(250000),
			    (uint64_t)i * HR_MSG_US);
	}
	zassert_is_null(last_with(RADIANT_RULE_FIELD_ACTIVE));
}

ZTEST(radiant_rules, test_activity_clears_only_after_a_real_stop)
{
	uint32_t src;
	uint32_t i;
	uint64_t last_advance_us;
	const struct radiant_sample *a;

	radiant_binding_bind(1u, 0x11u, 1u, NULL, &src);

	for (i = 0u; i <= 20u; i++) {
		post_energy(src, (int64_t)i * INT64_C(250000),
			    (uint64_t)i * HR_MSG_US);
	}
	last_advance_us = 20u * HR_MSG_US; /* 4.925 s: the last message that moved it */
	zassert_true(is_on(RADIANT_RULE_FIELD_ACTIVE));

	/*
	 * The rider stops but the trainer keeps transmitting - 6.1's stated
	 * reason for the asymmetric dwell. The clear is measured from the last
	 * ADVANCE (t = 4.925 s), not from the first quiet message, so it lands
	 * at t = 24.925 s: message 102 (25.120 s), and not message 101
	 * (24.874 s).
	 */
	cap_n = 0u;
	for (i = 21u; i <= 130u; i++) {
		uint64_t t = (uint64_t)i * HR_MSG_US;

		post_energy(src, INT64_C(20) * INT64_C(250000), t);

		if ((t - last_advance_us) < 20u * US_PER_S) {
			zassert_is_null(last_with(RADIANT_RULE_FIELD_ACTIVE));
		}
	}

	a = last_with(RADIANT_RULE_FIELD_ACTIVE);
	zassert_not_null(a);
	zassert_equal(a->raw, 0, NULL);
	zassert_equal(count_with(RADIANT_RULE_FIELD_ACTIVE), 1u);
	zassert_equal(a->t_us, 102u * HR_MSG_US);
}

ZTEST(radiant_rules, test_stale_clears_the_zones_too_on_a_real_stream)
{
	uint32_t src;
	uint32_t i;
	uint64_t t;
	struct radiant_sample s;
	const struct radiant_sample *w;
	const struct radiant_sample *at_rest;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);

	/* Worn, off the same 4 Hz / 60 bpm stream as above. */
	for (i = 0u; i <= 20u; i++) {
		t = (uint64_t)i * HR_MSG_US;
		post_beats(src, (int64_t)(t / US_PER_S), t);
	}
	zassert_true(is_on(RADIANT_RULE_FIELD_WORN));

	/* And at rest: 60 bpm is under the 82 bpm threshold, held past the
	 * zone dwell. */
	t = 21u * HR_MSG_US;
	post_hr(src, 60, t);
	t += 4u * US_PER_S;
	post_hr(src, 60, t);
	at_rest = last_with(RADIANT_RULE_FIELD_AT_REST);
	zassert_not_null(at_rest, NULL);
	zassert_equal(at_rest->raw, 1);

	/* The strap goes off the air. Liveness overrides both dwells: worn
	 * clears immediately, and 6.2's gate means the zone output must go
	 * with it in the same call, not 20 s later. */
	cap_n = 0u;
	memset(&s, 0, sizeof(s));
	s.source = src;
	s.field_type = RADIANT_FIELD_EVENT_COUNT;
	s.flags = RADIANT_SAMPLE_ACCUMULATING | RADIANT_SAMPLE_STALE;
	s.raw = 4; /* the last running total, unchanged - irrelevant under STALE */
	s.t_us = t + 1u;
	radiant_bridge_post(&s);
	radiant_bridge_drain();

	w = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_not_null(w, NULL);
	zassert_equal(w->raw, 0);
	at_rest = last_with(RADIANT_RULE_FIELD_AT_REST);
	zassert_not_null(at_rest);
	zassert_equal(at_rest->raw, 0, NULL);
}

/* ---------------------------------------------------------------------------
 * The equipment-state rule: the eighth group, and the first input to this
 * module that is a REPORT rather than an inference.
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_rules, test_fe_state_reaches_the_rule_engine_at_all)
{
	const struct radiant_sample *out;

	/* THE REGRESSION FOR THE WHOLE DEFECT. rules_want() accepted only
	 * ACCUMULATING samples and RADIANT_FIELD_HEART_RATE, so the FE state
	 * was on the bus from the day the FE-C decoder landed and nothing read
	 * it: a bound treadmill published a speed and a state enum and
	 * asserted no occupancy at all. A single sample is enough, which is
	 * the other half of the point - unlike every accumulator rule here,
	 * this one needs no second sample to difference against. */
	post_fe_state(0u, FEC_STATE_IN_USE, 1u * US_PER_S, 0u);

	out = last_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE);
	zassert_not_null(out);
	zassert_equal(out->raw, 1, NULL);
	zassert_equal(out->field_type, RADIANT_FIELD_OCCUPANCY);
	zassert_true((out->flags & RADIANT_SAMPLE_DERIVED) != 0u, NULL);
}

ZTEST(radiant_rules, test_in_use_asserts_immediately_with_no_dwell)
{
	/* The activity rule waits 2 s before believing an accumulator is
	 * advancing, because "advancing" is an inference. This is not: the
	 * machine said IN_USE, and adding a dwell would only add latency to a
	 * fact already established. */
	post_fe_state(0u, FEC_STATE_READY, 1u * US_PER_S, 0u);
	zassert_false(is_on(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE));

	post_fe_state(0u, FEC_STATE_IN_USE, 1u * US_PER_S + 250000u, 0u);
	zassert_equal(last_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE)->raw, 1,
		      NULL);
	zassert_equal(count_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE), 1u,
		      NULL);
}

ZTEST(radiant_rules, test_a_walk_break_does_not_flap_the_output)
{
	uint64_t t = 10u * US_PER_S;

	post_fe_state(0u, FEC_STATE_IN_USE, t, 0u);
	zassert_equal(last_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE)->raw, 1,
		      NULL);

	/* Two seconds of READY - somebody stepping off for a drink - inside
	 * the clear dwell. The output must hold. */
	for (uint32_t i = 0; i < 8u; i++) {
		t += 250000u; /* 4 Hz, the FE-C rate */
		post_fe_state(0u, FEC_STATE_READY, t, 0u);
	}
	zassert_equal(count_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE), 1u);

	/* Back on the belt, still one record: nothing changed. */
	t += 250000u;
	post_fe_state(0u, FEC_STATE_IN_USE, t, 0u);
	zassert_equal(count_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE), 1u,
		      NULL);

	/* Now genuinely finished, and held past the dwell. */
	for (uint32_t i = 0; i < (EQUIPMENT_CLEAR_S + 2u) * 4u; i++) {
		t += 250000u;
		post_fe_state(0u, FEC_STATE_FINISHED, t, 0u);
	}
	zassert_equal(last_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE)->raw, 0,
		      NULL);
	zassert_equal(count_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE), 2u,
		      NULL);
}

ZTEST(radiant_rules, test_reserved_states_are_not_occupied)
{
	/* Table 8-10 reserves 0 and 5..7, and radiant_profile_fec.c passes them
	 * through rather than remapping them - "reserved" there means a future
	 * state, not an error. Anything that is not exactly IN_USE must read
	 * as unoccupied, which is the safe direction: a future state read as
	 * occupied would leave a gym counting machines nobody is on. */
	post_fe_state(0u, FEC_STATE_IN_USE, 1u * US_PER_S, 0u);
	zassert_equal(last_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE)->raw, 1,
		      NULL);

	for (uint32_t i = 0; i < 40u; i++) {
		post_fe_state(0u, 7, (2u + i) * US_PER_S, 0u);
	}
	zassert_equal(last_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE)->raw, 0,
		      NULL);
}

ZTEST(radiant_rules, test_stale_drops_equipment_occupancy_immediately)
{
	post_fe_state(0u, FEC_STATE_IN_USE, 1u * US_PER_S, 0u);
	zassert_equal(last_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE)->raw, 1,
		      NULL);

	/* THE PROPERTY A GYM DASHBOARD DEPENDS ON. An unplugged treadmill must
	 * go unoccupied rather than stick at "in use" forever, and it must do
	 * so without waiting out the clear dwell - liveness is not a walk
	 * break. Note the sample still carries IN_USE: it is the STALE flag
	 * that decides, not the value. */
	post_fe_state(0u, FEC_STATE_IN_USE, 2u * US_PER_S,
		      RADIANT_SAMPLE_STALE);
	zassert_equal(last_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE)->raw, 0,
		      NULL);
}

ZTEST(radiant_rules, test_equipment_occupancy_is_per_source)
{
	uint32_t a;
	uint32_t b;

	zassert_equal(radiant_binding_bind(10u, 0x11u, 5u, NULL, &a), 0);
	zassert_equal(radiant_binding_bind(11u, 0x11u, 5u, NULL, &b), 0);
	zassert_equal(a, 0u);
	zassert_equal(b, 1u);

	post_fe_state(0u, FEC_STATE_IN_USE, 1u * US_PER_S, 0u);
	post_fe_state(1u, FEC_STATE_READY, 1u * US_PER_S, 0u);

	/* Two treadmills, two bindings, two answers - which is what makes N
	 * machines N occupancy endpoints rather than one shared verdict. */
	zassert_equal(cap_n, 2u, NULL);
	zassert_equal(cap_buf[0].source, 0u, NULL);
	zassert_equal(cap_buf[0].raw, 1, NULL);
	/* The idle one is announced, not silent: its endpoint must exist. */
	zassert_equal(cap_buf[1].source, 1u, NULL);
	zassert_equal(cap_buf[1].raw, 0, NULL);
}

ZTEST(radiant_rules, test_the_derived_output_does_not_re_enter_the_rule)
{
	/* rules_want() declines DERIVED samples, and widening it to accept
	 * ENUM_GENERIC must not have opened a loop: the occupancy output this
	 * rule posts is OCCUPANCY (0x02) rather than ENUM_GENERIC, but the
	 * loop guard is what makes that not merely a coincidence. */
	post_fe_state(0u, FEC_STATE_IN_USE, 1u * US_PER_S, 0u);
	zassert_equal(count_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE), 1u,
		      NULL);

	/* And an already-derived ENUM_GENERIC (nothing produces one today) is
	 * still declined rather than evaluated. */
	post_fe_state(0u, FEC_STATE_READY, 2u * US_PER_S,
		      RADIANT_SAMPLE_DERIVED);
	zassert_equal(count_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE), 1u,
		      NULL);
}

ZTEST(radiant_rules, test_the_equipment_type_is_not_an_equipment_state)
{
	uint64_t t = 1u * US_PER_S;

	/* Page 16 as it actually arrives: the type first, then the state, in
	 * that order, exactly as radiant_power_adapter.c's
	 * decode_fec_general() posts them. */
	post_fe_type(0u, FEC_TYPE_TREADMILL, t);
	post_fe_state(0u, FEC_STATE_IN_USE, t, 0u);

	zassert_equal(last_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE)->raw, 1, NULL);

	/*
	 * NOW THE TYPE ALONE, PAST THE CLEAR DWELL. Nothing here says anything
	 * about the state, so nothing here may change the verdict: the rules
	 * are sample-driven and have no clock of their own, so with the
	 * field_id gate the occupancy output simply holds at 1 until the next
	 * FE-state sample or a STALE flag says otherwise. Liveness is what
	 * clears a machine that has gone off the air; another field is not.
	 *
	 * WITHOUT THE GATE THIS FAILS AND IT IS THE FAILURE THAT MATTERS: every
	 * type code (19 treadmill, 22 rower, 25 indoor bike) is != IN_USE, so
	 * each one drives eval_equipment() as "not in use", the clear dwell
	 * expires on them, and the endpoint reports the machine free while
	 * someone is still on it. Confirmed by reverting the gate and
	 * re-running: this case drops to 0 with a second record.
	 *
	 * The interleaved arrangement is deliberately NOT the assertion here.
	 * Alternating type and state at one timestamp hides the defect - each
	 * type sample re-arms the clear edge and the state sample immediately
	 * re-asserts - so a test written that way passes either way and proves
	 * nothing.
	 */
	for (uint32_t i = 0; i < EQUIPMENT_CLEAR_S * 8u; i++) { /* twice the clear dwell */
		t += 250000u;
		post_fe_type(0u, FEC_TYPE_TREADMILL, t);
	}

	zassert_equal(count_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE), 1u);
	zassert_equal(last_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE)->raw, 1);
}

ZTEST(radiant_rules, test_an_equipment_type_alone_asserts_nothing)
{
	/* A type sample must not be able to CREATE an occupancy verdict either.
	 * 22 (rower) is not 3 (IN_USE) but nothing here should be comparing
	 * them at all - and a rule that never runs publishes nothing, which is
	 * what distinguishes "ignored" from "evaluated and happened to say
	 * false". */
	post_fe_type(0u, FEC_TYPE_ROWER, 1u * US_PER_S);
	post_fe_type(0u, FEC_TYPE_TREADMILL, 2u * US_PER_S);

	zassert_equal(cap_n, 0u);
}

/*
 * WORN and an Environment sensor (TODO 2026-11-24). Its event count is
 * RADIANT_FIELD_EVENT_COUNT and steps on every message, so it drove the WORN
 * slot - posted on field 0, the field its temperature is, which the Matter
 * plane then merged. WORN is skipped for a source BOUND as device type 0x19;
 * an unbound source, which carries no device type at all, keeps the rule.
 */
static uint32_t worn_after_steady_count(uint32_t source)
{
	uint32_t i;

	/* One step a second for five seconds: well past the 2 s assert. */
	for (i = 0u; i <= 5u; i++) {
		post_beats(source, (int64_t)i, (uint64_t)i * US_PER_S);
	}
	return count_with(RADIANT_RULE_FIELD_WORN);
}

ZTEST(radiant_rules, test_worn_is_not_produced_for_a_bound_environment_sensor)
{
	uint32_t source = RADIANT_BINDING_NONE;

	zassert_equal(radiant_binding_bind(4321u, 0x19u, 5u, NULL, &source), 0);
	zassert_equal(worn_after_steady_count(source), 0u);
	zassert_equal(cap_n, 0u);
}

ZTEST(radiant_rules, test_worn_skip_ignores_the_pairing_bit)
{
	uint32_t source = RADIANT_BINDING_NONE;

	zassert_equal(radiant_binding_bind(4321u, 0x99u, 5u, NULL, &source), 0);
	zassert_equal(worn_after_steady_count(source), 0u);
}

ZTEST(radiant_rules, test_worn_is_kept_for_an_unbound_source_and_a_bound_strap)
{
	uint32_t strap = RADIANT_BINDING_NONE;

	/* Unbound: no device type to judge by, so the rule runs as it always
	 * has (every other case in this file depends on that). */
	zassert_true(radiant_binding_get(5u) == NULL);
	zassert_equal(worn_after_steady_count(5u), 1u);
	zassert_equal(last_with(RADIANT_RULE_FIELD_WORN)->source, 5u);

	cap_n = 0u;
	zassert_equal(radiant_binding_bind(1234u, 0x78u, 1u, NULL, &strap), 0);
	zassert_not_equal(strap, 5u);
	zassert_equal(worn_after_steady_count(strap), 1u);
	zassert_equal(last_with(RADIANT_RULE_FIELD_WORN)->raw, 1);
}

/*
 * ---------------------------------------------------------------------------
 * Reviewer follow-ups 2026-11-26: announce on first input, reset on rebind,
 * the 30 s equipment clear, Zone 3, and per-strap thresholds.
 * ---------------------------------------------------------------------------
 */

static void post_beats_stale(uint32_t source, int64_t raw, uint64_t t_us)
{
	struct radiant_sample s = {
		.source = source,
		.field_id = 0u,
		.field_type = RADIANT_FIELD_EVENT_COUNT,
		.flags = RADIANT_SAMPLE_ACCUMULATING | RADIANT_SAMPLE_STALE,
		.exp = 0,
		.raw = raw,
		.t_us = t_us,
	};

	radiant_bridge_post(&s);
	radiant_bridge_drain();
}

/* Worn asserted by t + 3 s, the three-call pattern used above. Returns the
 * time of the last call. */
static uint64_t make_worn(uint32_t src, int64_t *beats, uint64_t t)
{
	post_beats(src, (*beats)++, t);
	t += 1u * US_PER_S;
	post_beats(src, (*beats)++, t);
	t += 2u * US_PER_S;
	post_beats(src, (*beats)++, t);
	zassert_true(is_on(RADIANT_RULE_FIELD_WORN));
	return t;
}

ZTEST(radiant_rules, test_an_idle_trainer_announces_in_use_false)
{
	const struct radiant_sample *out;
	uint32_t src;

	/* THE REVIEWER'S POINT 3. A trainer nobody has ridden reports READY
	 * forever; change-only publishing posted nothing, so the Matter plane
	 * never created its In Use endpoint until somebody first rode it. */
	zassert_equal(radiant_binding_bind(52233u, 0x11u, 5u, NULL, &src), 0);
	zassert_equal(src, 0u);
	post_fe_state(0u, FEC_STATE_READY, 1u * US_PER_S, 0u);

	out = last_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE);
	zassert_not_null(out, "an idle machine must still post In Use");
	zassert_equal(out->raw, 0);
	zassert_equal(out->field_type, RADIANT_FIELD_OCCUPANCY);

	/* Once only: further READY samples are not changes. */
	for (uint32_t i = 0u; i < 20u; i++) {
		post_fe_state(0u, FEC_STATE_READY, (2u + i) * US_PER_S, 0u);
	}
	zassert_equal(cap_n, 1u);
}

ZTEST(radiant_rules, test_an_unbound_source_is_not_announced)
{
	/* The tail of an evicted sensor (or a harness) has no endpoint to
	 * create; only a change is posted for it. */
	post_fe_state(3u, FEC_STATE_READY, 1u * US_PER_S, 0u);
	zassert_equal(cap_n, 0u);
	post_fe_state(3u, FEC_STATE_IN_USE, 2u * US_PER_S, 0u);
	zassert_true(is_on(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE));
}

ZTEST(radiant_rules, test_first_inputs_announce_every_output_they_feed)
{
	uint32_t src;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);

	/* One beat count: Worn exists at once, before the delta it needs. */
	post_beats(src, 7, 0u);
	zassert_not_null(last_with(RADIANT_RULE_FIELD_WORN));
	zassert_false(is_on(RADIANT_RULE_FIELD_WORN));

	/* One HR sample, strap not yet worn: all three zone outputs exist. */
	post_hr(src, 150, 0u);
	zassert_not_null(last_with(RADIANT_RULE_FIELD_AT_REST));
	zassert_not_null(last_with(RADIANT_RULE_FIELD_ZONE2));
	zassert_not_null(last_with(RADIANT_RULE_FIELD_ZONE3));
	zassert_false(is_on(RADIANT_RULE_FIELD_ZONE3));

	/* One energy sample: Active. */
	post_energy(src, 0, 0u);
	zassert_not_null(last_with(RADIANT_RULE_FIELD_ACTIVE));
	zassert_false(is_on(RADIANT_RULE_FIELD_ACTIVE));

	/* Five announcements, and nothing else: no output asserted. */
	zassert_equal(cap_n, 5u);
}

ZTEST(radiant_rules, test_in_use_holds_for_the_configured_clear_delay)
{
	uint64_t t0 = 10u * US_PER_S;
	uint64_t t = t0;

	/* THE REVIEWER'S POINT 2. A trainer drops to READY the moment
	 * cadence reaches zero; with a 5 s clear, In Use cleared at every
	 * traffic light. */
	post_fe_state(0u, FEC_STATE_IN_USE, t, 0u);
	zassert_true(is_on(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE));

	/* READY at 4 Hz up to one second short of the delay: holds. */
	while (t < t0 + (uint64_t)(EQUIPMENT_CLEAR_S - 1u) * US_PER_S) {
		t += 250000u;
		post_fe_state(0u, FEC_STATE_READY, t, 0u);
	}
	zassert_true(is_on(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE),
		     "In Use must hold for the whole clear delay");

	/* The edge was the first READY, at t0 + 0.25 s. Past the delay: clears. */
	while (t < t0 + 250000u + (uint64_t)EQUIPMENT_CLEAR_S * US_PER_S) {
		t += 250000u;
		post_fe_state(0u, FEC_STATE_READY, t, 0u);
	}
	zassert_false(is_on(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE));
	zassert_equal(count_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE), 2u);
}

ZTEST(radiant_rules, test_a_rebound_slot_inherits_nothing)
{
	uint32_t src;
	uint32_t src2;
	int64_t  beats = 0;
	uint64_t t;
	const struct radiant_sample *out;

	/* THE REVIEWER'S POINT 4. Sensor A, occupied and worn, in slot N. */
	zassert_equal(radiant_binding_bind(100u, 0x11u, 5u, NULL, &src), 0);
	announce_binding(src);
	t = make_worn(src, &beats, 0u);
	post_fe_state(src, FEC_STATE_IN_USE, t, 0u);
	zassert_true(is_on(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE));

	/* Evicted, and sensor B paired into the same slot before liveness or
	 * anything else noticed. */
	zassert_equal(radiant_binding_unbind(src), 0);
	announce_binding(src);
	zassert_equal(radiant_binding_bind(200u, 0x11u, 5u, NULL, &src2), 0);
	zassert_equal(src2, src, "the test needs the slot reused");
	announce_binding(src2);

	/* B's first report is IN_USE. Before the reset it matched A's stale
	 * `debounced` and was swallowed as "no change". */
	cap_n = 0u;
	post_fe_state(src2, FEC_STATE_IN_USE, t + 1u * US_PER_S, 0u);
	out = last_with(RADIANT_RULE_FIELD_EQUIPMENT_IN_USE);
	zassert_not_null(out, "the new sensor's first occupancy was suppressed");
	zassert_equal(out->raw, 1);

	/* And its Worn starts from nothing: announced false on the first beat
	 * count, not inherited true, and B's count is not differenced against
	 * A's. */
	post_beats(src2, 5000, t + 1u * US_PER_S);
	out = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_not_null(out);
	zassert_equal(out->raw, 0);
}

ZTEST(radiant_rules, test_zone3_asserts_at_146_and_not_at_145)
{
	uint32_t src;
	int64_t  beats = 0;
	uint64_t t;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);
	t = make_worn(src, &beats, 0u);

	/* 145 held past the dwell: zone 2 yes, zone 3 no. */
	post_hr(src, 145, t);
	t += 4u * US_PER_S;
	post_beats(src, beats++, t);
	post_hr(src, 145, t);
	zassert_true(is_on(RADIANT_RULE_FIELD_ZONE2));
	zassert_false(is_on(RADIANT_RULE_FIELD_ZONE3));

	/* 146: zone 3. HR_rest 65 + 0.70 x 115 = 145.5, rounded up. */
	post_hr(src, 146, t);
	t += 4u * US_PER_S;
	post_beats(src, beats++, t);
	post_hr(src, 146, t);
	zassert_true(is_on(RADIANT_RULE_FIELD_ZONE3));
	zassert_true(is_on(RADIANT_RULE_FIELD_ZONE2));

	/* STALE drops zone 3 with the others, immediately. */
	post_beats_stale(src, beats, t + 1u);
	zassert_false(is_on(RADIANT_RULE_FIELD_ZONE3));
	zassert_false(is_on(RADIANT_RULE_FIELD_ZONE2));
}

ZTEST(radiant_rules, test_personal_thresholds_move_only_at_session_end)
{
	uint32_t src;
	int64_t  beats = 0;
	uint64_t t;
	uint32_t i;

	/* A bound strap gets a profile slot. One hour at 142 bpm: under the
	 * prior's zone 3 (146), and enough wear (3600 samples vs. the 1800 s
	 * prior weight) that the blend moves HR_rest to its 100 bpm clamp and
	 * zone 3 down to 140. */
	zassert_equal(radiant_binding_bind(31u, 0x78u, 1u, NULL, &src), 0);
	t = make_worn(src, &beats, 0u);

	for (i = 0u; i < 3600u; i++) {
		t += 1u * US_PER_S;
		post_beats(src, beats++, t);
		post_hr(src, 142, t);
		/* Mid-session the thresholds never move. */
		if (is_on(RADIANT_RULE_FIELD_ZONE3)) {
			break;
		}
	}
	zassert_equal(i, 3600u, "zone 3 moved mid-session (sample %u)", i);
	zassert_not_null(radiant_hr_profile_hist(radiant_binding_get(src)->uuid));
	zassert_equal(radiant_hr_hist_total(
			      radiant_hr_profile_hist(radiant_binding_get(src)->uuid)),
		      3600u, "1 Hz decimation: one sample per second of wear");

	/* Session end (the strap goes off the air), then a new session. */
	post_beats_stale(src, beats, t + 1u);
	t += 10u * US_PER_S;
	t = make_worn(src, &beats, t);
	post_hr(src, 142, t);
	t += 4u * US_PER_S;
	post_beats(src, beats++, t);
	post_hr(src, 142, t);
	zassert_true(is_on(RADIANT_RULE_FIELD_ZONE3),
		     "the personalised zone 3 (140) applies from the next session");
}

ZTEST(radiant_rules, test_an_unbound_source_uses_the_prior_and_no_slot)
{
	int64_t  beats = 0;
	uint64_t t;

	/* Every older case here posts to an unbound source: no uuid, so no
	 * profile slot, and the thresholds are the prior's - the n = 0 path. */
	t = make_worn(6u, &beats, 0u);
	for (uint32_t i = 0u; i < 10u; i++) {
		t += 1u * US_PER_S;
		post_beats(6u, beats++, t);
		post_hr(6u, 146, t);
	}
	zassert_true(is_on(RADIANT_RULE_FIELD_ZONE3));
	zassert_is_null(radiant_hr_profile_hist(0u));
}

static void *rules_setup(void)
{
	return NULL;
}

ZTEST_SUITE(radiant_rules, NULL, rules_setup, rules_test_reset, NULL, NULL);
