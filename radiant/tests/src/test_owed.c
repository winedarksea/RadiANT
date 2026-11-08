/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work against radiant_owed.h/.c.
 *
 * The list behind mqtt_sink.c's owed "offline" statuses. The case that earns
 * the file is the release review 2026-11-08 item 4 sequence, run exactly as
 * mqtt_sink.c's binding_changed()/drain_owed() run it (the helpers below
 * mirror those two bodies line for line, because mqtt_sink.c itself is in
 * apps/ and no ztest reaches it): release A, bind B, release B during an
 * outage, and after the reconnect BOTH must go out. With the old per-source
 * uuid slot, the second release overwrote the first.
 */

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "radiant_owed.h"

#define A 0xA0A0A0A0A0A0A0A1ull
#define B 0xB0B0B0B0B0B0B0B2ull
#define C 0xC0C0C0C0C0C0C0C3ull

static struct radiant_owed o;

static void before(void *f)
{
	ARG_UNUSED(f);
	radiant_owed_init(&o);
}

/* Mirrors mqtt_binding_changed(). */
static uint64_t bound[RADIANT_BINDING_MAX];

static void model_bind(uint32_t source, uint64_t uuid)
{
	if (uuid == 0u) { /* release */
		if (bound[source] != 0u) {
			(void)radiant_owed_add(&o, bound[source]);
		}
	} else {
		(void)radiant_owed_cancel(&o, uuid);
	}
	bound[source] = uuid;
}

/* Mirrors drain_owed()'s second loop; returns what was "sent". */
static uint32_t model_drain(uint64_t *sent, uint32_t cap)
{
	uint32_t n = 0u;

	for (uint32_t i = 0u; i < RADIANT_OWED_MAX; i++) {
		uint64_t u = radiant_owed_at(&o, i);

		if (u == 0u) {
			continue;
		}
		if (n < cap) {
			sent[n] = u;
		}
		n++;
		(void)radiant_owed_cancel(&o, u);
	}
	return n;
}

static bool has(const uint64_t *v, uint32_t n, uint64_t u)
{
	for (uint32_t i = 0u; i < n; i++) {
		if (v[i] == u) {
			return true;
		}
	}
	return false;
}

ZTEST(radiant_owed, test_release_a_bind_b_release_b_owes_both)
{
	uint64_t sent[RADIANT_OWED_MAX];
	uint32_t n;

	memset(bound, 0, sizeof(bound));
	model_bind(0, A);
	model_bind(0, 0u);	/* release A, broker down */
	model_bind(0, B);	/* the source is reused */
	model_bind(0, 0u);	/* release B, still down */

	n = model_drain(sent, RADIANT_OWED_MAX);	/* reconnect */
	zassert_equal(n, 2u, "both offlines must go out, got %u", n);
	zassert_true(has(sent, n, A), "A's offline was lost");
	zassert_true(has(sent, n, B), "B's offline was lost");
	zassert_false(radiant_owed_any(&o), "the drain must leave nothing");
	zassert_equal(o.overflow, 0u, NULL);
}

ZTEST(radiant_owed, test_dedupe_same_uuid_released_twice)
{
	uint64_t sent[RADIANT_OWED_MAX];

	zassert_true(radiant_owed_add(&o, A), NULL);
	zassert_true(radiant_owed_add(&o, A), NULL);
	zassert_true(radiant_owed_add(&o, B), NULL);
	zassert_true(radiant_owed_add(&o, A), NULL);
	zassert_equal(model_drain(sent, RADIANT_OWED_MAX), 2u, "A must be listed once");
	zassert_equal(o.overflow, 0u, "a duplicate is not an overflow");
}

ZTEST(radiant_owed, test_bind_cancels_a_match_on_another_source)
{
	uint64_t sent[RADIANT_OWED_MAX];
	uint32_t n;

	memset(bound, 0, sizeof(bound));
	model_bind(2, A);
	model_bind(2, 0u);	/* A released from source 2, owed */
	model_bind(5, B);
	model_bind(5, 0u);	/* B released from source 5, owed */
	model_bind(6, A);	/* A re-pinned on a DIFFERENT source */

	n = model_drain(sent, RADIANT_OWED_MAX);
	zassert_equal(n, 1u, "only B is still owed, got %u", n);
	zassert_equal(sent[0], B, NULL);
}

ZTEST(radiant_owed, test_drain_clears_only_what_it_sent)
{
	/* A is sent; B is released while A's publish is in flight. */
	zassert_true(radiant_owed_add(&o, A), NULL);
	uint64_t in_flight = radiant_owed_at(&o, 0);

	zassert_equal(in_flight, A, NULL);
	zassert_true(radiant_owed_add(&o, B), NULL);	/* lands during the send */
	zassert_true(radiant_owed_cancel(&o, in_flight), NULL);

	zassert_true(radiant_owed_any(&o), "B must survive A's clear");
	uint64_t seen = 0u;

	for (uint32_t i = 0u; i < RADIANT_OWED_MAX; i++) {
		if (radiant_owed_at(&o, i) != 0u) {
			seen = radiant_owed_at(&o, i);
		}
	}
	zassert_equal(seen, B, NULL);
}

ZTEST(radiant_owed, test_overflow_is_counted_and_nothing_else_is_lost)
{
	uint64_t u;

	for (u = 1u; u <= RADIANT_OWED_MAX; u++) {
		zassert_true(radiant_owed_add(&o, u), "slot %llu", (unsigned long long)u);
	}
	zassert_equal(o.overflow, 0u, NULL);

	zassert_false(radiant_owed_add(&o, 0x1000u), "a full list must refuse");
	zassert_false(radiant_owed_add(&o, 0x1001u), NULL);
	zassert_equal(o.overflow, 2u, NULL);

	/* Every earlier entry is intact, and the refused ones are not there. */
	for (u = 1u; u <= RADIANT_OWED_MAX; u++) {
		zassert_true(radiant_owed_cancel(&o, u), "entry %llu lost", (unsigned long long)u);
	}
	zassert_false(radiant_owed_any(&o), "a refused uuid must not be stored");

	/* Room again after the drain. */
	zassert_true(radiant_owed_add(&o, 0x1000u), NULL);
}

ZTEST(radiant_owed, test_capacity_covers_every_source_plus_one_predecessor)
{
	zassert_equal(RADIANT_OWED_MAX, 2u * RADIANT_BINDING_MAX, NULL);
	zassert_false(radiant_owed_any(&o), NULL);
	zassert_equal(radiant_owed_at(&o, RADIANT_OWED_MAX), 0u, "out of range reads empty");
}

ZTEST(radiant_owed, test_zero_uuid_and_missing_cancel_are_harmless)
{
	zassert_true(radiant_owed_add(&o, 0u), NULL);
	zassert_false(radiant_owed_any(&o), "0 is the empty marker, never stored");
	zassert_false(radiant_owed_cancel(&o, C), NULL);
	zassert_false(radiant_owed_cancel(&o, 0u), NULL);
}

ZTEST_SUITE(radiant_owed, NULL, NULL, before, NULL, NULL);
