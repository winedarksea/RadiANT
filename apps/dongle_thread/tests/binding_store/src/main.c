/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original work, against apps/dongle_thread/src/binding_store.h.
 *
 * Covers: a full record round-trips, including the extreme values of every
 * field; the v1 byte layout is pinned byte for byte (a layout change is a
 * version change, not an edit); encode refuses a short buffer and an identity
 * that could never decode, and always writes a terminated label; decode
 * refuses every wrong length, any version but 1, a label with no NUL, and a
 * wildcard or zero identity, and leaves the output zeroed when it refuses.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include "binding_store.h"

static struct binding_record sample(void)
{
	struct binding_record r;

	memset(&r, 0, sizeof(r));
	r.devnum = 11625u;
	r.devtype = 0x78u;
	r.trans = 0x01u;
	r.uuid = 0x0123456789ABCDEFull;
	strcpy(r.label, "strap");
	r.period = 8070u;
	r.seq = 7u;
	return r;
}

ZTEST(binding_store_codec, test_round_trip)
{
	struct binding_record in = sample();
	struct binding_record out;
	uint8_t buf[BINDING_RECORD_LEN];

	zassert_equal(binding_record_encode(&in, buf, sizeof(buf)),
		      BINDING_RECORD_LEN);
	zassert_equal(binding_record_decode(buf, sizeof(buf), &out), 0);
	zassert_mem_equal(&out, &in, sizeof(in));
}

ZTEST(binding_store_codec, test_round_trip_extremes)
{
	struct binding_record in;
	struct binding_record out;
	uint8_t buf[BINDING_RECORD_LEN + 4u];

	memset(&in, 0, sizeof(in));
	in.devnum = 0xFFFFu;
	in.devtype = 0xFFu;
	in.trans = 0xFFu;
	in.uuid = UINT64_MAX;
	memset(in.label, 'x', sizeof(in.label) - 1u);
	in.period = 0xFFFFu;
	in.seq = UINT32_MAX;

	/* A larger buffer is fine; only BINDING_RECORD_LEN is written. */
	zassert_equal(binding_record_encode(&in, buf, sizeof(buf)),
		      BINDING_RECORD_LEN);
	zassert_equal(binding_record_decode(buf, BINDING_RECORD_LEN, &out), 0);
	zassert_mem_equal(&out, &in, sizeof(in));

	/* Smallest valid identity, empty label, period never read. */
	memset(&in, 0, sizeof(in));
	in.devnum = 1u;
	in.devtype = 1u;
	in.uuid = 1u;
	zassert_equal(binding_record_encode(&in, buf, sizeof(buf)),
		      BINDING_RECORD_LEN);
	zassert_equal(binding_record_decode(buf, BINDING_RECORD_LEN, &out), 0);
	zassert_mem_equal(&out, &in, sizeof(in));
}

ZTEST(binding_store_codec, test_v1_layout_is_pinned)
{
	static const uint8_t want[BINDING_RECORD_LEN] = {
		0x01,                                           /* version */
		0x69, 0x2D,                                     /* 11625 */
		0x78,                                           /* devtype */
		0x01,                                           /* trans */
		0xEF, 0xCD, 0xAB, 0x89, 0x67, 0x45, 0x23, 0x01, /* uuid */
		's', 't', 'r', 'a', 'p', 0, 0, 0,
		0, 0, 0, 0, 0, 0, 0, 0,                         /* label */
		0x86, 0x1F,                                     /* 8070 */
		0x07, 0x00, 0x00, 0x00,                         /* seq */
	};
	struct binding_record in = sample();
	uint8_t buf[BINDING_RECORD_LEN];

	zassert_equal(BINDING_RECORD_LEN, 35u);
	zassert_equal(binding_record_encode(&in, buf, sizeof(buf)),
		      BINDING_RECORD_LEN);
	zassert_mem_equal(buf, want, sizeof(want));
}

ZTEST(binding_store_codec, test_encode_refusals)
{
	struct binding_record r = sample();
	uint8_t buf[BINDING_RECORD_LEN];

	zassert_equal(binding_record_encode(&r, buf, sizeof(buf) - 1u), 0u);
	zassert_equal(binding_record_encode(NULL, buf, sizeof(buf)), 0u);
	zassert_equal(binding_record_encode(&r, NULL, sizeof(buf)), 0u);

	r = sample();
	r.devnum = 0u;
	zassert_equal(binding_record_encode(&r, buf, sizeof(buf)), 0u);
	r = sample();
	r.devtype = 0u;
	zassert_equal(binding_record_encode(&r, buf, sizeof(buf)), 0u);
	r = sample();
	r.uuid = 0u;
	zassert_equal(binding_record_encode(&r, buf, sizeof(buf)), 0u);
}

/* A label filling all 16 bytes with no terminator is truncated to 15, and
 * the record still decodes - the encoder never writes what decode refuses. */
ZTEST(binding_store_codec, test_encode_terminates_a_full_label)
{
	struct binding_record in = sample();
	struct binding_record out;
	uint8_t buf[BINDING_RECORD_LEN];

	memset(in.label, 'L', sizeof(in.label));
	zassert_equal(binding_record_encode(&in, buf, sizeof(buf)),
		      BINDING_RECORD_LEN);
	zassert_equal(binding_record_decode(buf, sizeof(buf), &out), 0);
	zassert_equal(strlen(out.label), BINDING_RECORD_LABEL - 1u);
	zassert_equal(out.label[BINDING_RECORD_LABEL - 1u], '\0');
}

static void assert_refused(const uint8_t *buf, size_t len, int want)
{
	struct binding_record out;

	memset(&out, 0xA5, sizeof(out));
	zassert_equal(binding_record_decode(buf, len, &out), want, "len %u",
		      (unsigned int)len);
	{
		static const struct binding_record zero;

		zassert_mem_equal(&out, &zero, sizeof(out));
	}
}

ZTEST(binding_store_codec, test_decode_refusals)
{
	struct binding_record in = sample();
	uint8_t good[BINDING_RECORD_LEN + 1u];
	uint8_t bad[BINDING_RECORD_LEN + 1u];

	zassert_equal(binding_record_encode(&in, good, sizeof(good)),
		      BINDING_RECORD_LEN);
	good[BINDING_RECORD_LEN] = 0u;

	/* Lengths. */
	assert_refused(good, 0u, -EINVAL);
	assert_refused(good, 1u, -EINVAL);
	assert_refused(good, BINDING_RECORD_LEN - 1u, -EINVAL);
	assert_refused(good, BINDING_RECORD_LEN + 1u, -EINVAL);
	assert_refused(NULL, BINDING_RECORD_LEN, -EINVAL);
	zassert_equal(binding_record_decode(good, BINDING_RECORD_LEN, NULL),
		      -EINVAL);

	/* Versions: not ours, whatever the length. */
	memcpy(bad, good, sizeof(bad));
	bad[0] = 0u;
	assert_refused(bad, BINDING_RECORD_LEN, -ENOTSUP);
	bad[0] = 2u;
	assert_refused(bad, BINDING_RECORD_LEN, -ENOTSUP);
	assert_refused(bad, BINDING_RECORD_LEN + 1u, -ENOTSUP);

	/* A label with no NUL in its 16 bytes. */
	memcpy(bad, good, sizeof(bad));
	memset(&bad[13], 'L', BINDING_RECORD_LABEL);
	assert_refused(bad, BINDING_RECORD_LEN, -EINVAL);

	/* Wildcard or zero identity fields. */
	memcpy(bad, good, sizeof(bad));
	bad[1] = 0u;
	bad[2] = 0u;
	assert_refused(bad, BINDING_RECORD_LEN, -EINVAL);
	memcpy(bad, good, sizeof(bad));
	bad[3] = 0u;
	assert_refused(bad, BINDING_RECORD_LEN, -EINVAL);
	memcpy(bad, good, sizeof(bad));
	memset(&bad[5], 0, 8u);
	assert_refused(bad, BINDING_RECORD_LEN, -EINVAL);
}

ZTEST_SUITE(binding_store_codec, NULL, NULL, NULL, NULL, NULL);
