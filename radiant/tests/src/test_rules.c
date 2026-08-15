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

/* How many times this output CHANGED. The rules publish on change only, so
 * this is the chatter count: a boolean that is meant to assert once and hold
 * must produce exactly one record over a long stream, and a count above one is
 * the flapping failure, not merely a noisy log. */
static uint32_t count_with(uint8_t field_id)
{
	uint32_t i;
	uint32_t n = 0u;

	for (i = 0u; i < cap_n; i++) {
		if (cap_buf[i].field_id == field_id) {
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

	zassert_is_null(last_with(RADIANT_RULE_FIELD_WORN),
			"1 s in: under the 2 s assert dwell");
	zassert_is_null(last_with(RADIANT_RULE_FIELD_ACTIVE), NULL);

	t += 2u * US_PER_S;
	post_beats(src, 2, t);
	post_energy(src, INT64_C(400000000), t);

	w = last_with(RADIANT_RULE_FIELD_WORN);
	a = last_with(RADIANT_RULE_FIELD_ACTIVE);
	zassert_not_null(w, "0x36 must drive WORN");
	zassert_not_null(a, "0x30 must drive ACTIVE");
	zassert_equal(w->raw, 1, NULL);
	zassert_equal(a->raw, 1, NULL);
	zassert_equal(w->field_type, RADIANT_FIELD_OCCUPANCY, NULL);
	zassert_equal(a->field_type, RADIANT_FIELD_OCCUPANCY, NULL);
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
	zassert_not_null(last_with(RADIANT_RULE_FIELD_WORN), NULL);
	zassert_not_null(last_with(RADIANT_RULE_FIELD_ACTIVE), NULL);

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
	zassert_not_null(a, "past the 20 s clear dwell: ACTIVE must clear");
	zassert_equal(a->raw, 0, NULL);

	w = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_is_null(w, "WORN never changed, so it must publish nothing");
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
	zassert_not_null(last_with(RADIANT_RULE_FIELD_ACTIVE), "ACTIVE asserts");
	zassert_is_null(last_with(RADIANT_RULE_FIELD_WORN), NULL);

	/* Resting-range heart rate, held well past the zone dwell. */
	post_hr(src, 60, t);
	t += 4u * US_PER_S;
	post_hr(src, 60, t);
	t += 4u * US_PER_S;
	post_hr(src, 60, t);

	zassert_is_null(last_with(RADIANT_RULE_FIELD_AT_REST),
			"not WORN: at-rest must never assert off ACTIVE");
	zassert_is_null(last_with(RADIANT_RULE_FIELD_ZONE2), NULL);
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
			zassert_is_null(last_with(RADIANT_RULE_FIELD_WORN),
					"under the 2 s dwell: must not assert yet");
		}
	}

	w = last_with(RADIANT_RULE_FIELD_WORN);
	zassert_not_null(w, "60 bpm on a 4.06 Hz stream must read as worn");
	zassert_equal(w->raw, 1, NULL);
	zassert_equal(count_with(RADIANT_RULE_FIELD_WORN), 1u,
		      "asserts once and holds: the three quiet messages between "
		      "beats must not clear it");
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
	zassert_not_null(a, "pedalling for 5 s: ACTIVE must assert");
	zassert_equal(a->raw, 1, NULL);

	/* ~5 s coasting: same message rate, the accumulator holds at its last
	 * value. Well inside the 20 s clear budget, so nothing may be
	 * published at all. */
	cap_n = 0u;
	for (i = 21u; i <= 40u; i++) {
		post_energy(src, INT64_C(20) * INT64_C(250000),
			    (uint64_t)i * HR_MSG_US);
		zassert_is_null(last_with(RADIANT_RULE_FIELD_ACTIVE),
				"a coast is not a stop: ACTIVE must not drop out");
	}

	/* Pedalling resumes; still no transition, because it never cleared. */
	for (i = 41u; i <= 60u; i++) {
		post_energy(src, (int64_t)(i - 20u) * INT64_C(250000),
			    (uint64_t)i * HR_MSG_US);
	}
	zassert_is_null(last_with(RADIANT_RULE_FIELD_ACTIVE),
			"unchanged across coast and resume: nothing to publish");
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
	zassert_not_null(last_with(RADIANT_RULE_FIELD_ACTIVE), "asserted");

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
			zassert_is_null(last_with(RADIANT_RULE_FIELD_ACTIVE),
					"under the 20 s clear dwell: must not clear");
		}
	}

	a = last_with(RADIANT_RULE_FIELD_ACTIVE);
	zassert_not_null(a, "past the 20 s clear dwell: must clear");
	zassert_equal(a->raw, 0, NULL);
	zassert_equal(count_with(RADIANT_RULE_FIELD_ACTIVE), 1u,
		      "one clean transition, not a chatter");
	zassert_equal(a->t_us, 102u * HR_MSG_US,
		      "clears on the first message past 20 s since the last advance");
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
	zassert_not_null(last_with(RADIANT_RULE_FIELD_WORN), "must be worn");

	/* And at rest: 60 bpm is under the 82 bpm threshold, held past the
	 * zone dwell. */
	t = 21u * HR_MSG_US;
	post_hr(src, 60, t);
	t += 4u * US_PER_S;
	post_hr(src, 60, t);
	at_rest = last_with(RADIANT_RULE_FIELD_AT_REST);
	zassert_not_null(at_rest, NULL);
	zassert_equal(at_rest->raw, 1, "60 bpm, worn, held: at rest");

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
	zassert_equal(w->raw, 0, "STALE forces worn false immediately");
	at_rest = last_with(RADIANT_RULE_FIELD_AT_REST);
	zassert_not_null(at_rest, "the zones are gated on worn and must follow");
	zassert_equal(at_rest->raw, 0, NULL);
}

static void *rules_setup(void)
{
	return NULL;
}

ZTEST_SUITE(radiant_rules, NULL, rules_setup, rules_test_reset, NULL, NULL);
