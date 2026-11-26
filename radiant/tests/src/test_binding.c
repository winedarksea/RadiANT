/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original clean-room work, against radiant_binding.h/.c, itself
 * a transcription of docs/radiant-bridge.md section 5.
 *
 * Covers: bind is idempotent by key; -ENOSPC only for a new key on a full
 * table, an existing key still resolves; uuid is nonzero and differs per
 * binding; unbind-then-rebind of the same key is a new identity (re-pair is
 * a user action, not automatic reconciliation); radiant_binding_restore()
 * puts back a stored uuid/label/period, is idempotent for the same identity,
 * refuses a conflicting one, and is what lets an application keep a uuid
 * across an unbind and a reboot.
 */

#include <errno.h>
#include <stdint.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

#include <radiant/radiant_binding.h>

static void binding_reset(void *unused)
{
	ARG_UNUSED(unused);
	radiant_binding_init();
}

ZTEST(radiant_binding, test_bind_is_idempotent_by_key)
{
	uint32_t s1, s2;

	zassert_equal(radiant_binding_bind(1234u, 0x78u, 1u, "strap", &s1), 0);
	zassert_equal(radiant_binding_bind(1234u, 0x78u, 1u, "strap", &s2), 0);
	zassert_equal(s1, s2);
}

ZTEST(radiant_binding, test_different_keys_get_different_sources)
{
	uint32_t s1, s2;

	zassert_equal(radiant_binding_bind(1u, 0x78u, 1u, NULL, &s1), 0);
	zassert_equal(radiant_binding_bind(2u, 0x78u, 1u, NULL, &s2), 0);
	zassert_not_equal(s1, s2);

	zassert_equal(radiant_binding_find(1u, 0x78u, 1u), s1);
	zassert_equal(radiant_binding_find(2u, 0x78u, 1u), s2);
	zassert_equal(radiant_binding_find(3u, 0x78u, 1u), RADIANT_BINDING_NONE);
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
		zassert_equal(rc, 0, "slot %u", i);
	}

	rc = radiant_binding_bind(9999u, 0x78u, 1u, NULL, &extra);
	zassert_equal(rc, -ENOSPC);

	/* An EXISTING key still resolves - fullness must not break lookups
	 * of what is already bound. */
	rc = radiant_binding_bind(100u, 0x78u, 1u, NULL, &extra);
	zassert_equal(rc, 0);
	zassert_equal(extra, sources[0]);
}

ZTEST(radiant_binding, test_uuid_is_nonzero_and_differs_across_bindings)
{
	uint32_t s1, s2;
	const struct radiant_binding *b1, *b2;

	radiant_binding_bind(1u, 0x78u, 1u, NULL, &s1);
	radiant_binding_bind(2u, 0x78u, 1u, NULL, &s2);

	b1 = radiant_binding_get(s1);
	b2 = radiant_binding_get(s2);

	zassert_not_null(b1);
	zassert_not_null(b2);
	zassert_not_equal(b1->uuid, 0u);
	zassert_not_equal(b1->uuid, b2->uuid);
}

ZTEST(radiant_binding, test_unbind_then_rebind_is_a_new_identity)
{
	uint32_t source, rebound;
	uint64_t first_uuid;
	const struct radiant_binding *b;

	radiant_binding_bind(42u, 0x78u, 1u, "old strap", &source);
	b = radiant_binding_get(source);
	first_uuid = b->uuid;

	zassert_equal(radiant_binding_unbind(source), 0);
	zassert_is_null(radiant_binding_get(source));

	zassert_equal(radiant_binding_bind(42u, 0x78u, 1u, "new strap", &rebound), 0);
	b = radiant_binding_get(rebound);
	zassert_not_equal(b->uuid, first_uuid);
}

/* A heart-rate strap and a power meter can share a 16-bit device number; the
 * key is (devnum, devtype, trans_type), or their samples share one source.
 * Every other test here used devtype 0x78 / trans_type 1, so dropping either
 * clause from same_key() survived (review 2026-10-31, E3-5). */
ZTEST(radiant_binding, test_the_key_is_all_three_fields)
{
	uint32_t hr, pwr, tt5;

	zassert_equal(radiant_binding_bind(1234u, 0x78u, 1u, NULL, &hr), 0);
	zassert_equal(radiant_binding_bind(1234u, 0x0Bu, 1u, NULL, &pwr), 0);
	zassert_equal(radiant_binding_bind(1234u, 0x78u, 5u, NULL, &tt5), 0);
	zassert_not_equal(hr, pwr);
	zassert_not_equal(hr, tt5);
	zassert_not_equal(pwr, tt5);
	zassert_equal(radiant_binding_find(1234u, 0x0Bu, 1u), pwr);
	zassert_equal(radiant_binding_find(1234u, 0x78u, 5u), tt5);

	zassert_equal(-ENOENT, radiant_binding_set_period(RADIANT_BINDING_MAX, 8192u));
	zassert_equal(0, radiant_binding_unbind(hr));
	zassert_equal(-ENOENT, radiant_binding_set_period(hr, 8192u));
}

/* A reboot: the table is empty and the application hands back what it
 * stored. Everything it stored comes back, and lookups find it. */
ZTEST(radiant_binding, test_restore_puts_back_the_stored_identity)
{
	const struct radiant_binding *b;
	uint32_t source = RADIANT_BINDING_NONE;

	zassert_equal(radiant_binding_restore(11625u, 0x78u, 0x01u,
					      0x0123456789ABCDEFull, "strap",
					      8070u, &source), 0);
	b = radiant_binding_get(source);
	zassert_not_null(b);
	zassert_equal(b->devnum, 11625u);
	zassert_equal(b->devtype, 0x78u);
	zassert_equal(b->trans_type, 0x01u);
	zassert_equal(b->uuid, 0x0123456789ABCDEFull);
	zassert_mem_equal(b->label, "strap", sizeof("strap"));
	zassert_equal(b->period, 8070u);
	zassert_equal(radiant_binding_find(11625u, 0x78u, 0x01u), source);

	/* bind() of the restored key resolves to it and does not re-mint. */
	{
		uint32_t again;

		zassert_equal(radiant_binding_bind(11625u, 0x78u, 0x01u, NULL,
						   &again), 0);
		zassert_equal(again, source);
		zassert_equal(radiant_binding_get(again)->uuid,
			      0x0123456789ABCDEFull);
	}
}

ZTEST(radiant_binding, test_restore_is_idempotent_and_refuses_conflicts)
{
	uint32_t s1, s2, s3 = RADIANT_BINDING_NONE;

	zassert_equal(radiant_binding_restore(1u, 0x78u, 1u, 0xAAull, NULL, 0u,
					      &s1), 0);
	/* The same identity twice: same source, no second row. */
	zassert_equal(radiant_binding_restore(1u, 0x78u, 1u, 0xAAull, NULL, 0u,
					      &s2), 0);
	zassert_equal(s1, s2);
	/* The same channel ID under another uuid: refused, and told where. */
	zassert_equal(radiant_binding_restore(1u, 0x78u, 1u, 0xBBull, NULL, 0u,
					      &s3), -EEXIST);
	zassert_equal(s3, s1);
	zassert_equal(radiant_binding_get(s1)->uuid, 0xAAull);
	/* The same uuid on another channel ID: refused, nothing bound. */
	zassert_equal(radiant_binding_restore(2u, 0x78u, 1u, 0xAAull, NULL, 0u,
					      NULL), -EEXIST);
	zassert_equal(radiant_binding_find(2u, 0x78u, 1u), RADIANT_BINDING_NONE);
	/* uuid 0 is "no identity". */
	zassert_equal(radiant_binding_restore(3u, 0x78u, 1u, 0u, NULL, 0u,
					      NULL), -EINVAL);
	zassert_equal(radiant_binding_find(3u, 0x78u, 1u), RADIANT_BINDING_NONE);
}

ZTEST(radiant_binding, test_restore_full_table_is_enospc)
{
	uint32_t i;

	for (i = 0u; i < RADIANT_BINDING_MAX; i++) {
		zassert_equal(radiant_binding_restore((uint16_t)(100u + i), 0x78u,
						      1u, 0x1000ull + i, NULL,
						      0u, NULL), 0, "slot %u", i);
	}
	zassert_equal(radiant_binding_restore(9999u, 0x78u, 1u, 0x9999ull, NULL,
					      0u, NULL), -ENOSPC);
}

/* The app-level re-pair: unbind (a host took the channel), then the same
 * channel ID pairs again and the application restores the uuid it kept. A
 * fresh bind beside it never mints that uuid. */
ZTEST(radiant_binding, test_restore_after_unbind_keeps_the_uuid)
{
	uint32_t source, other, back;
	uint64_t kept;

	zassert_equal(radiant_binding_bind(42u, 0x0Bu, 5u, NULL, &source), 0);
	kept = radiant_binding_get(source)->uuid;
	zassert_equal(radiant_binding_unbind(source), 0);

	zassert_equal(radiant_binding_restore(42u, 0x0Bu, 5u, kept, NULL, 0u,
					      &back), 0);
	zassert_equal(radiant_binding_get(back)->uuid, kept);
	zassert_equal(radiant_binding_get(back)->period, 0u);

	zassert_equal(radiant_binding_bind(43u, 0x0Bu, 5u, NULL, &other), 0);
	zassert_not_equal(radiant_binding_get(other)->uuid, kept);
	zassert_not_equal(radiant_binding_get(other)->uuid, 0u);
}

static void *binding_setup(void)
{
	return NULL;
}

ZTEST_SUITE(radiant_binding, NULL, binding_setup, binding_reset, NULL, NULL);
