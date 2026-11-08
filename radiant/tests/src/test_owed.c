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

/* Mirrors mqtt_binding_changed() (the add/cancel calls; the drain below is
 * the shared radiant_owed_drain(), not a copy). */
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

/* The send callback: records what was offered, and refuses (non-zero) from
 * the `fail_from`-th call on, like the sink's send after a dropped link. */
static uint64_t offered[RADIANT_OWED_MAX];
static uint32_t n_offered;
static uint32_t fail_from;
static uint32_t n_calls;

static void nop(void *ctx)
{
	ARG_UNUSED(ctx);
}

static uint64_t add_during_send; /* a release landing while a publish is in flight */

static int send_cb(void *ctx, uint64_t uuid)
{
	ARG_UNUSED(ctx);
	n_calls++;
	if (add_during_send != 0u) {
		(void)radiant_owed_add(&o, add_during_send);
		add_during_send = 0u;
	}
	if (n_calls >= fail_from) {
		return 1;
	}
	offered[n_offered++] = uuid;
	return 0;
}

static const struct radiant_owed_ops ops = { .lock = nop, .unlock = nop, .send = send_cb };

static uint32_t model_drain(uint32_t fail_at)
{
	n_offered = 0u;
	n_calls = 0u;
	fail_from = fail_at;
	return radiant_owed_drain(&o, &ops);
}

static uint32_t slots_used(void)
{
	uint32_t n = 0u;

	for (uint32_t i = 0u; i < RADIANT_OWED_MAX; i++) {
		if (radiant_owed_at(&o, i) != 0u) {
			n++;
		}
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
	uint32_t n;

	memset(bound, 0, sizeof(bound));
	model_bind(0, A);
	model_bind(0, 0u);	/* release A, broker down */
	model_bind(0, B);	/* the source is reused */
	model_bind(0, 0u);	/* release B, still down */

	n = model_drain(UINT32_MAX);	/* reconnect */
	zassert_equal(n, 2u, "both offlines must go out, got %u", n);
	zassert_true(has(offered, n_offered, A), "A's offline was lost");
	zassert_true(has(offered, n_offered, B), "B's offline was lost");
	zassert_false(radiant_owed_any(&o), "the drain must leave nothing");
	zassert_equal(o.overflow, 0u, NULL);
}

ZTEST(radiant_owed, test_dedupe_same_uuid_released_twice)
{
	zassert_true(radiant_owed_add(&o, A), NULL);
	zassert_true(radiant_owed_add(&o, A), NULL);
	zassert_true(radiant_owed_add(&o, B), NULL);
	zassert_true(radiant_owed_add(&o, A), NULL);
	zassert_equal(slots_used(), 2u, "A must occupy one slot, not three");

	/* And the capacity view of the same thing: duplicates must not eat slots. */
	for (int i = 0; i < 40; i++) {
		zassert_true(radiant_owed_add(&o, A), NULL);
	}
	for (uint64_t u = 100u; u < 100u + RADIANT_OWED_MAX - 2u; u++) {
		zassert_true(radiant_owed_add(&o, u), "duplicates ate the list");
	}
	zassert_equal(o.overflow, 0u, "a duplicate is not an overflow");
}

ZTEST(radiant_owed, test_bind_cancels_a_match_on_another_source)
{
	uint32_t n;

	memset(bound, 0, sizeof(bound));
	/* Fill the first slots with others so A sits in a LATER slot. */
	zassert_true(radiant_owed_add(&o, C), NULL);
	zassert_true(radiant_owed_add(&o, 0xD0u), NULL);
	model_bind(2, A);
	model_bind(2, 0u);	/* A released from source 2, owed, slot 2 */
	model_bind(5, B);
	model_bind(5, 0u);	/* B released from source 5, owed */
	zassert_true(radiant_owed_at(&o, 0) != A && radiant_owed_at(&o, 1) != A, NULL);
	model_bind(6, A);	/* A re-pinned on a DIFFERENT source */

	n = model_drain(UINT32_MAX);
	zassert_equal(n, 3u, "C, 0xD0 and B are still owed, got %u", n);
	zassert_false(has(offered, n_offered, A), "A's bind must have cancelled it");
	zassert_true(has(offered, n_offered, B), NULL);
}

ZTEST(radiant_owed, test_failed_send_keeps_the_uuid_and_later_slots_follow_the_sink)
{
	uint32_t n;

	zassert_true(radiant_owed_add(&o, A), NULL);
	zassert_true(radiant_owed_add(&o, B), NULL);
	zassert_true(radiant_owed_add(&o, C), NULL);

	/* The second send fails (link drops); the third is refused too, as the
	 * sink's send does once disconnected. Only A went out. */
	n = model_drain(2u);
	zassert_equal(n, 1u, NULL);
	zassert_equal(n_calls, 3u, "every owed slot is still offered, so send() can refuse");
	zassert_equal(slots_used(), 2u, "B and C must stay owed");
	zassert_false(has(offered, n_offered, B) || has(offered, n_offered, C), NULL);

	/* Reconnected: the rest goes out. */
	n = model_drain(UINT32_MAX);
	zassert_equal(n, 2u, NULL);
	zassert_false(radiant_owed_any(&o), NULL);
}

ZTEST(radiant_owed, test_drain_clears_only_what_it_sent)
{
	/* Slot 0 empty, A in slot 1. B is added while A's publish is in flight
	 * and lands in the hole at slot 0, behind the walk. Clearing "the
	 * source's entry" (the old shape) would take B too. */
	zassert_true(radiant_owed_add(&o, C), NULL);
	zassert_true(radiant_owed_add(&o, A), NULL);
	zassert_true(radiant_owed_cancel(&o, C), NULL);

	add_during_send = B;
	zassert_equal(model_drain(UINT32_MAX), 1u, NULL);
	zassert_equal(offered[0], A, NULL);
	zassert_true(radiant_owed_any(&o), "B must survive A's clear");
	zassert_equal(slots_used(), 1u, NULL);
	zassert_equal(model_drain(UINT32_MAX), 1u, NULL);
	zassert_equal(offered[0], B, NULL);
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
