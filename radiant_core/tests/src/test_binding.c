/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, against radiant_binding.h/.c, itself
 * a transcription of docs/radiant-bridge.md section 5.
 *
 * Four things earn their place:
 *   1. THE SAME KEY RETURNS THE SAME SOURCE. Binding is idempotent by key -
 *      posting the same sensor twice must not fragment its history across
 *      two sources.
 *   2. A FULL TABLE REFUSES A NEW KEY, NOT AN EXISTING ONE. -ENOSPC only when
 *      the table is full AND the key is new; an existing binding must still
 *      resolve even with every other slot occupied.
 *   3. UUID SURVIVES THE LOOKUP PATH AND DIFFERS ACROSS BINDINGS. Section 5's
 *      whole reason for the field existing.
 *   4. UNBIND FREES THE SLOT AND THE NEXT BIND OF THE SAME KEY IS A NEW
 *      IDENTITY - re-pair is a user action per the header comment, not an
 *      automatic reconciliation, and this is the negative-space test of that:
 *      nothing here should be found "the same" without one.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include "radiant_binding.h"

static void binding_reset(void *unused)
{
	ARG_UNUSED(unused);
	radiant_binding_init();
}

ZTEST(radiant_binding, test_bind_is_idempotent_by_key)
{
	uint32_t s1, s2;

	zassert_equal(radiant_binding_bind(1234u, 0x78u, 1u, "strap", &s1), 0, NULL);
	zassert_equal(radiant_binding_bind(1234u, 0x78u, 1u, "strap", &s2), 0, NULL);
	zassert_equal(s1, s2, "same key must resolve to the same source");
}

ZTEST(radiant_binding, test_different_keys_get_different_sources)
{
	uint32_t s1, s2;

	zassert_equal(radiant_binding_bind(1u, 0x78u, 1u, NULL, &s1), 0, NULL);
	zassert_equal(radiant_binding_bind(2u, 0x78u, 1u, NULL, &s2), 0, NULL);
	zassert_not_equal(s1, s2, NULL);

	zassert_equal(radiant_binding_find(1u, 0x78u, 1u), s1, NULL);
	zassert_equal(radiant_binding_find(2u, 0x78u, 1u), s2, NULL);
	zassert_equal(radiant_binding_find(3u, 0x78u, 1u), RADIANT_BINDING_NONE, NULL);
}

ZTEST(radiant_binding, test_table_full_refuses_new_key_not_existing_one)
{
	uint32_t sources[RADIANT_BINDING_MAX];
	uint32_t extra;
	int      rc;
	uint32_t i;

	for (i = 0u; i < RADIANT_BINDING_MAX; i++) {
		rc = radiant_binding_bind((uint16_t)(100u + i), 0x78u, 1u, NULL,
					  &sources[i]);
		zassert_equal(rc, 0, "table is not yet full at slot %u", i);
	}

	rc = radiant_binding_bind(9999u, 0x78u, 1u, NULL, &extra);
	zassert_equal(rc, -ENOSPC, "table is full: a new key must be refused");

	/* An EXISTING key still resolves - fullness must not break lookups
	 * of what is already bound. */
	rc = radiant_binding_bind(100u, 0x78u, 1u, NULL, &extra);
	zassert_equal(rc, 0, "an existing binding must resolve even when full");
	zassert_equal(extra, sources[0], NULL);
}

ZTEST(radiant_binding, test_uuid_is_nonzero_and_differs_across_bindings)
{
	uint32_t s1, s2;
	const struct radiant_binding *b1, *b2;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &s1);
	radiant_binding_bind(2u, 0x78u, 1u, NULL, &s2);

	b1 = radiant_binding_get(s1);
	b2 = radiant_binding_get(s2);

	zassert_not_null(b1, NULL);
	zassert_not_null(b2, NULL);
	zassert_not_equal(b1->uuid, 0u, "a real binding must have a nonzero uuid");
	zassert_not_equal(b1->uuid, b2->uuid,
		       "two different bindings must not share a uuid");
}

ZTEST(radiant_binding, test_unbind_then_rebind_is_a_new_identity)
{
	uint32_t source, rebound;
	uint64_t first_uuid;
	const struct radiant_binding *b;

	radiant_binding_bind(42u, 0x78u, 1u, "old strap", &source);
	b = radiant_binding_get(source);
	first_uuid = b->uuid;

	zassert_equal(radiant_binding_unbind(source), 0, NULL);
	zassert_is_null(radiant_binding_get(source),
			"an unbound slot must not answer get()");

	zassert_equal(radiant_binding_bind(42u, 0x78u, 1u, "new strap", &rebound), 0,
		     NULL);
	b = radiant_binding_get(rebound);
	zassert_not_equal(b->uuid, first_uuid,
		       "re-pairing the same key must not silently inherit the old uuid");
}

static void *binding_setup(void)
{
	return NULL;
}

ZTEST_SUITE(radiant_binding, NULL, binding_setup, binding_reset, NULL, NULL);
