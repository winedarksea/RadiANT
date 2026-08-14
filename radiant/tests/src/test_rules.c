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
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "radiant_binding.h"
#include "radiant_bridge.h"
#include "radiant_rules.h"

#define CAP_MAX 32

static struct radiant_sample cap_buf[CAP_MAX];
static uint32_t          cap_n;

static bool rule_cap_want(const struct radiant_sample *s)
{
	/* Only what a rule produced, so a test does not have to pick its own
	 * injected samples back out of the capture. */
	return (s->flags & RADIANT_SAMPLE_DERIVED) != 0u;
}

static void rule_cap_publish(const struct radiant_sample *s)
{
	if (cap_n < CAP_MAX) {
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
	memset(cap_buf, 0, sizeof(cap_buf));
	cap_n = 0u;
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

#define US_PER_S ((uint64_t)1000000u)

ZTEST(radiant_rules, test_activity_needs_a_second_sample)
{
	uint32_t src;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);
	post_beats(src, 10, 0u);
	zassert_is_null(last_with(RADIANT_RULE_FIELD_WORN),
			"one sample: nothing to difference, nothing to assert");
}

ZTEST(radiant_rules, test_activity_asserts_after_dwell_not_before)
{
	uint32_t src;
	const struct radiant_sample *w;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &src);
	post_beats(src, 0, 0u);
	post_beats(src, 1, 1u * US_PER_S); /* advancing, but only 1 s in */

	w = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_is_null(w, "under the 2 s assert dwell: must not assert yet");

	post_beats(src, 2, 3u * US_PER_S); /* advancing continuously past 2 s */
	w = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_not_null(w, "past the 2 s assert dwell: must assert");
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
	zassert_not_null(last_with(RADIANT_RULE_FIELD_WORN), "asserted");

	/*
	 * Stops advancing (the accumulator holds at 2 - a real counter does
	 * not go backwards). The clear-side edge starts at this call, not at
	 * the moment the assert happened, so the dwell below is measured from
	 * t=13 s, not from t=3 s.
	 */
	cap_n = 0u;
	post_beats(src, 2, 13u * US_PER_S);
	zassert_is_null(last_with(RADIANT_RULE_FIELD_WORN),
			"the edge just started: must not clear yet");

	post_beats(src, 2, 20u * US_PER_S); /* 7 s since the edge: still under 20 s */
	zassert_is_null(last_with(RADIANT_RULE_FIELD_WORN),
			"under the 20 s clear dwell: must not clear yet");

	post_beats(src, 2, 34u * US_PER_S); /* 21 s since the edge: past it */
	w = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_not_null(w, "past the 20 s clear dwell: must clear");
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

	zassert_is_null(last_with(RADIANT_RULE_FIELD_AT_REST),
			"not worn: at-rest must never assert regardless of HR");
	zassert_is_null(last_with(RADIANT_RULE_FIELD_ZONE2), NULL);
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
	zassert_not_null(last_with(RADIANT_RULE_FIELD_WORN), "must be worn now");

	/* Below the rest threshold, held past the zone dwell. */
	cap_n = 0u;
	post_hr(src, 81, t);
	t += 4u * US_PER_S;
	post_hr(src, 81, t);
	at_rest = last_with(RADIANT_RULE_FIELD_AT_REST);
	zassert_not_null(at_rest, "81 bpm, worn, held: must read at rest");
	zassert_equal(at_rest->raw, 1, NULL);
	zassert_is_null(last_with(RADIANT_RULE_FIELD_ZONE2), NULL);

	/* At the zone2 threshold, held past the zone dwell. */
	cap_n = 0u;
	post_hr(src, 134, t);
	t += 4u * US_PER_S;
	post_hr(src, 134, t);
	zone2 = last_with(RADIANT_RULE_FIELD_ZONE2);
	zassert_not_null(zone2, "134 bpm, worn, held: must read zone 2");
	zassert_equal(zone2->raw, 1, NULL);
	/* And the earlier at-rest output must have cleared, not stuck true. */
	at_rest = last_with(RADIANT_RULE_FIELD_AT_REST);
	zassert_not_null(at_rest, NULL);
	zassert_equal(at_rest->raw, 0, "134 bpm is not resting: must have cleared");

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
	zassert_equal(at_rest->raw, 0, "100 bpm: not resting");
	zassert_not_null(zone2, NULL);
	zassert_equal(zone2->raw, 0, "100 bpm, held: zone 2 must have cleared");
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
	zassert_not_null(last_with(RADIANT_RULE_FIELD_WORN), "must be worn now");

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
	zassert_not_null(w, "STALE must force an immediate transition");
	zassert_equal(w->raw, 0, "STALE forces worn false, bypassing the clear dwell");
}

static void *rules_setup(void)
{
	return NULL;
}

ZTEST_SUITE(radiant_rules, NULL, rules_setup, rules_test_reset, NULL, NULL);
