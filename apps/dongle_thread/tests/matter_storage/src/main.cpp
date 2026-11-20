/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * The bridged-device record decoder (src/matter/bridge_storage_codec.h)
 * against hand-built records - security review 2026-11-20 item 2.
 *
 * Record is a struct of exactly BridgeStorageManager::BridgedDeviceV2's
 * shape: the real one is nested in a class whose header pulls in CHIP, and
 * the decoder reads every capacity with sizeof() off the field, so the shape
 * is what matters. kUniqueIDSize is CHIP's kMaxUniqueIDLength (32) and
 * kNodeLabelSize is 32, as in matter_bridged_device.h.
 *
 * The cases whose names start with test_wrap_ give a length near SIZE_MAX.
 * Upstream's `readSize < counter + length` wraps for those and memcpy()s
 * gigabytes, so against the old logic they crash rather than fail - they are
 * named to sort last so the other cases report first.
 */

#include <zephyr/ztest.h>

#include <stdint.h>
#include <string.h>

#include "bridge_storage_codec.h"

using namespace RadiantMatter::StorageCodec;

namespace
{

constexpr size_t kUniqueIDSize = 32;
constexpr size_t kNodeLabelSize = 32;
constexpr size_t kMaxUserDataSize = 128;

struct RecordV2 {
	uint16_t mEndpointId;
	uint16_t mDeviceType;
	size_t mUniqueIDLength;
	char mUniqueID[kUniqueIDSize] = { 0 };
	size_t mNodeLabelLength;
	char mNodeLabel[kNodeLabelSize] = { 0 };
	size_t mUserDataSize = 0;
	uint8_t *mUserData = nullptr;
};

struct RecordV1 {
	uint16_t mEndpointId;
	uint16_t mDeviceType;
	size_t mNodeLabelLength;
	char mNodeLabel[kNodeLabelSize] = { 0 };
	size_t mUserDataSize = 0;
	uint8_t *mUserData = nullptr;
};

/* The decoded struct with guard bytes behind it: an overrun of the last
 * string lands in the guard instead of somewhere unobservable. */
struct Guarded {
	RecordV2 rec;
	uint8_t guard[256];
};

/* The loader's own source-buffer size, sizeof(BridgedDeviceV2) + 128. */
constexpr size_t kLoaderBufSize = sizeof(RecordV2) + kMaxUserDataSize;

/* Builds a serialised record field by field. Strings are passed with an
 * explicit declared length and an explicit count of bytes actually present,
 * so a record can claim more than it carries. */
struct Writer {
	uint8_t buf[512];
	size_t n = 0;

	Writer() { memset(buf, 0xA5, sizeof(buf)); }

	void Bytes(const void *p, size_t len)
	{
		zassert_true(n + len <= sizeof(buf), "test record too big");
		memcpy(buf + n, p, len);
		n += len;
	}
	void U16(uint16_t v) { Bytes(&v, sizeof(v)); }
	void Size(size_t v) { Bytes(&v, sizeof(v)); }
	void Fill(char c, size_t len)
	{
		zassert_true(n + len <= sizeof(buf), "test record too big");
		memset(buf + n, c, len);
		n += len;
	}
	/* A well-formed string: its length, then that many bytes. */
	void Str(char c, size_t len)
	{
		Size(len);
		Fill(c, len);
	}
};

/* Endpoint 7, device type 0x0107, then the unique ID. */
void Header(Writer &w, char idChar, size_t idLen)
{
	w.U16(7);
	w.U16(0x0107);
	w.Str(idChar, idLen);
}

void ValidV2(Writer &w, size_t idLen, size_t labelLen)
{
	Header(w, 'i', idLen);
	w.Str('l', labelLen);
}

bool AllAre(const char *p, char c, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (p[i] != c) {
			return false;
		}
	}
	return true;
}

bool GuardIntact(const Guarded &g)
{
	for (size_t i = 0; i < sizeof(g.guard); i++) {
		if (g.guard[i] != 0xEE) {
			return false;
		}
	}
	return true;
}

void InitGuarded(Guarded &g)
{
	g.rec = RecordV2();
	memset(g.guard, 0xEE, sizeof(g.guard));
}

} /* namespace */

ZTEST_SUITE(matter_storage, NULL, NULL, NULL, NULL, NULL);

/* ---- well-formed records ---------------------------------------------- */

/* What ant_bridge.cpp actually stores: an 18-character unique ID and a
 * label, no user data. */
ZTEST(matter_storage, test_v2_typical_record_round_trips)
{
	Writer w;
	RecordV2 r;

	w.U16(42);
	w.U16(0x0302);
	w.Size(18);
	w.Bytes("0123456789abcdef01", 18);
	w.Size(11);
	w.Bytes("HRM 12345 A", 11);

	zassert_true(DecodeBridgedDeviceV2(w.buf, w.n, r));
	zassert_equal(r.mEndpointId, 42);
	zassert_equal(r.mDeviceType, 0x0302);
	zassert_equal(r.mUniqueIDLength, 18);
	zassert_str_equal(r.mUniqueID, "0123456789abcdef01");
	zassert_equal(r.mNodeLabelLength, 11);
	zassert_str_equal(r.mNodeLabel, "HRM 12345 A");
	zassert_equal(r.mUserDataSize, 0);
}

ZTEST(matter_storage, test_v2_zero_length_strings_accepted)
{
	Writer w;
	RecordV2 r;

	memset(r.mUniqueID, 'X', sizeof(r.mUniqueID));
	memset(r.mNodeLabel, 'X', sizeof(r.mNodeLabel));
	ValidV2(w, 0, 0);

	zassert_true(DecodeBridgedDeviceV2(w.buf, w.n, r));
	zassert_equal(r.mUniqueIDLength, 0);
	zassert_equal(r.mNodeLabelLength, 0);
	zassert_equal(r.mUniqueID[0], '\0', "empty unique ID not terminated");
	zassert_equal(r.mNodeLabel[0], '\0', "empty label not terminated");
}

/* Capacity - 1 is the longest string that still has room for its NUL. */
ZTEST(matter_storage, test_v2_capacity_minus_one_accepted)
{
	Writer w;
	RecordV2 r;

	ValidV2(w, kUniqueIDSize - 1, kNodeLabelSize - 1);

	zassert_true(DecodeBridgedDeviceV2(w.buf, w.n, r));
	zassert_equal(r.mUniqueIDLength, kUniqueIDSize - 1);
	zassert_true(AllAre(r.mUniqueID, 'i', kUniqueIDSize - 1));
	zassert_equal(r.mUniqueID[kUniqueIDSize - 1], '\0');
	zassert_equal(r.mNodeLabelLength, kNodeLabelSize - 1);
	zassert_true(AllAre(r.mNodeLabel, 'l', kNodeLabelSize - 1));
	zassert_equal(r.mNodeLabel[kNodeLabelSize - 1], '\0');
}

/* The terminator is written, not inherited from a zeroed struct: a reused
 * struct holding a longer string from an earlier load must not show its
 * tail. */
ZTEST(matter_storage, test_v2_terminator_written_over_stale_content)
{
	Writer w;
	RecordV2 r;

	memset(r.mUniqueID, 'X', sizeof(r.mUniqueID));
	memset(r.mNodeLabel, 'X', sizeof(r.mNodeLabel));
	ValidV2(w, 5, 7);

	zassert_true(DecodeBridgedDeviceV2(w.buf, w.n, r));
	zassert_equal(strlen(r.mUniqueID), 5);
	zassert_equal(strlen(r.mNodeLabel), 7);
}

/* ---- destination capacity --------------------------------------------- */

ZTEST(matter_storage, test_v2_unique_id_at_capacity_rejected)
{
	Writer w;
	Guarded g;

	InitGuarded(g);
	ValidV2(w, kUniqueIDSize, 4);
	zassert_false(DecodeBridgedDeviceV2(w.buf, w.n, g.rec), "no room for the NUL");
	zassert_true(GuardIntact(g));
}

ZTEST(matter_storage, test_v2_unique_id_over_capacity_rejected)
{
	Writer w;
	Guarded g;

	InitGuarded(g);
	ValidV2(w, kUniqueIDSize + 1, 4);
	zassert_false(DecodeBridgedDeviceV2(w.buf, w.n, g.rec));
	zassert_true(GuardIntact(g));
}

ZTEST(matter_storage, test_v2_label_at_capacity_rejected)
{
	Writer w;
	Guarded g;

	InitGuarded(g);
	ValidV2(w, 18, kNodeLabelSize);
	zassert_false(DecodeBridgedDeviceV2(w.buf, w.n, g.rec), "no room for the NUL");
	zassert_true(GuardIntact(g));
}

ZTEST(matter_storage, test_v2_label_over_capacity_rejected)
{
	Writer w;
	Guarded g;

	InitGuarded(g);
	ValidV2(w, 18, kNodeLabelSize + 1);
	zassert_false(DecodeBridgedDeviceV2(w.buf, w.n, g.rec));
	zassert_true(GuardIntact(g));
}

/* The finding itself: a label length that fits in the loader's source buffer
 * (sizeof(BridgedDeviceV2) + 128 bytes) but not in mNodeLabel. Upstream copied
 * it, running ~170 bytes past the struct in RestoreBridgedDevices()'s frame. */
ZTEST(matter_storage, test_v2_label_fitting_source_buffer_but_not_field_rejected)
{
	Writer w;
	Guarded g;

	InitGuarded(g);
	Header(w, 'i', 0);
	const size_t fill = kLoaderBufSize - w.n - sizeof(size_t);
	w.Str('l', fill);
	zassert_equal(w.n, kLoaderBufSize);
	zassert_true(fill > kNodeLabelSize + 100, "case lost its point: %zu", fill);

	zassert_false(DecodeBridgedDeviceV2(w.buf, w.n, g.rec));
	zassert_true(GuardIntact(g), "decoder wrote past the struct");
}

/* ---- bytes left in the record ----------------------------------------- */

ZTEST(matter_storage, test_v2_unique_id_longer_than_record_rejected)
{
	Writer w;
	RecordV2 r;

	w.U16(7);
	w.U16(0x0107);
	w.Size(20);
	w.Fill('i', 10);

	zassert_false(DecodeBridgedDeviceV2(w.buf, w.n, r));
}

ZTEST(matter_storage, test_v2_label_longer_than_record_rejected)
{
	Writer w;
	RecordV2 r;

	Header(w, 'i', 18);
	w.Size(12);
	w.Fill('l', 11);

	zassert_false(DecodeBridgedDeviceV2(w.buf, w.n, r));
	/* ... and the exact length is accepted, so the bound is not off by one. */
	w.Fill('l', 1);
	zassert_true(DecodeBridgedDeviceV2(w.buf, w.n, r));
}

ZTEST(matter_storage, test_v2_truncated_header_rejected)
{
	Writer w;
	RecordV2 r;

	ValidV2(w, 4, 4);
	/* Every proper prefix of a valid record is refused. */
	for (size_t len = 0; len < w.n; len++) {
		zassert_false(DecodeBridgedDeviceV2(w.buf, len, r), "prefix of %zu accepted", len);
	}
	zassert_true(DecodeBridgedDeviceV2(w.buf, w.n, r));
}

ZTEST(matter_storage, test_v2_null_buffer_rejected)
{
	RecordV2 r;

	zassert_false(DecodeBridgedDeviceV2(static_cast<const uint8_t *>(nullptr), 100, r));
}

/* ---- user data: upstream's bounds, unchanged ------------------------- */

/* No buffer from the caller: whatever follows is ignored, size reads 0. */
ZTEST(matter_storage, test_user_data_not_requested_ignores_trailer)
{
	Writer w;
	RecordV2 r;

	ValidV2(w, 4, 4);
	w.Size(3);
	w.Fill('u', 3);
	r.mUserDataSize = 99;

	zassert_true(DecodeBridgedDeviceV2(w.buf, w.n, r));
	zassert_equal(r.mUserDataSize, 0);
}

ZTEST(matter_storage, test_user_data_copied_when_it_fits)
{
	Writer w;
	RecordV2 r;
	uint8_t ud[8] = { 0 };

	ValidV2(w, 4, 4);
	w.Size(5);
	w.Bytes("\x01\x02\x03\x04\x05", 5);
	r.mUserData = ud;
	r.mUserDataSize = sizeof(ud);

	zassert_true(DecodeBridgedDeviceV2(w.buf, w.n, r));
	zassert_equal(r.mUserDataSize, 5);
	zassert_mem_equal(ud, "\x01\x02\x03\x04\x05", 5);
	zassert_equal(ud[5], 0, "copied past the stored size");
}

ZTEST(matter_storage, test_user_data_exactly_caller_capacity_accepted)
{
	Writer w;
	RecordV2 r;
	uint8_t ud[kMaxUserDataSize];

	ValidV2(w, 18, 31);
	w.Size(sizeof(ud));
	w.Fill('u', sizeof(ud));
	r.mUserData = ud;
	r.mUserDataSize = sizeof(ud);

	zassert_true(DecodeBridgedDeviceV2(w.buf, w.n, r));
	zassert_equal(r.mUserDataSize, sizeof(ud));
}

ZTEST(matter_storage, test_user_data_zero_size_accepted)
{
	Writer w;
	RecordV2 r;
	uint8_t ud[4];

	ValidV2(w, 4, 4);
	w.Size(0);
	r.mUserData = ud;
	r.mUserDataSize = sizeof(ud);

	zassert_true(DecodeBridgedDeviceV2(w.buf, w.n, r));
	zassert_equal(r.mUserDataSize, 0);
}

ZTEST(matter_storage, test_user_data_over_caller_capacity_rejected)
{
	Writer w;
	RecordV2 r;
	uint8_t ud[4];

	ValidV2(w, 4, 4);
	w.Size(5);
	w.Fill('u', 5);
	r.mUserData = ud;
	r.mUserDataSize = sizeof(ud);

	zassert_false(DecodeBridgedDeviceV2(w.buf, w.n, r));
}

/* A caller who asked for user data gets a failure, not silence, when the
 * record has none - upstream's behaviour. */
ZTEST(matter_storage, test_user_data_size_field_missing_rejected)
{
	Writer w;
	RecordV2 r;
	uint8_t ud[4];

	ValidV2(w, 4, 4);
	r.mUserData = ud;
	r.mUserDataSize = sizeof(ud);

	zassert_false(DecodeBridgedDeviceV2(w.buf, w.n, r));
}

ZTEST(matter_storage, test_user_data_shorter_than_declared_rejected)
{
	Writer w;
	RecordV2 r;
	uint8_t ud[8];

	ValidV2(w, 4, 4);
	w.Size(6);
	w.Fill('u', 5);
	r.mUserData = ud;
	r.mUserDataSize = sizeof(ud);

	zassert_false(DecodeBridgedDeviceV2(w.buf, w.n, r));
}

/* ---- store side --------------------------------------------------------- */

ZTEST(matter_storage, test_store_refuses_what_load_would_refuse)
{
	RecordV2 r;

	r.mUniqueIDLength = kUniqueIDSize - 1;
	r.mNodeLabelLength = kNodeLabelSize - 1;
	zassert_true(StorableBridgedDeviceV2(r));

	r.mUniqueIDLength = 0;
	r.mNodeLabelLength = 0;
	zassert_true(StorableBridgedDeviceV2(r));

	r.mUniqueIDLength = kUniqueIDSize;
	zassert_false(StorableBridgedDeviceV2(r));
	r.mUniqueIDLength = 18;
	r.mNodeLabelLength = kNodeLabelSize;
	zassert_false(StorableBridgedDeviceV2(r));
	r.mNodeLabelLength = SIZE_MAX;
	zassert_false(StorableBridgedDeviceV2(r));
}

ZTEST(matter_storage, test_string_fits_edges)
{
	zassert_false(StringFits(0, 0), "a zero-size array holds no NUL");
	zassert_true(StringFits(0, 1));
	zassert_false(StringFits(1, 1));
	zassert_true(StringFits(31, 32));
	zassert_false(StringFits(32, 32));
	zassert_false(StringFits(SIZE_MAX, 32));
}

/* ---- version 1 (compiled out of the image, same rules) ---------------- */

ZTEST(matter_storage, test_v1_label_bounds)
{
	RecordV1 r;

	{
		Writer w;
		w.U16(3);
		w.U16(0x0302);
		w.Str('l', kNodeLabelSize - 1);
		zassert_true(DecodeBridgedDeviceV1(w.buf, w.n, r));
		zassert_equal(r.mNodeLabel[kNodeLabelSize - 1], '\0');
	}
	{
		Writer w;
		w.U16(3);
		w.U16(0x0302);
		w.Str('l', kNodeLabelSize);
		zassert_false(DecodeBridgedDeviceV1(w.buf, w.n, r));
	}
	{
		Writer w;
		w.U16(3);
		w.U16(0x0302);
		w.Size(10);
		w.Fill('l', 9);
		zassert_false(DecodeBridgedDeviceV1(w.buf, w.n, r));
	}
}

/* ---- lengths near SIZE_MAX: `counter + length` wraps ------------------- */

ZTEST(matter_storage, test_wrap_unique_id_length_rejected)
{
	static const size_t lengths[] = {
		SIZE_MAX,
		/* counter + length wraps to just below counter, which is below any
		 * record size: upstream's check passes. */
		SIZE_MAX - 3,
		SIZE_MAX - (2 + 2 + sizeof(size_t)) + 1,
	};

	for (size_t i = 0; i < ARRAY_SIZE(lengths); i++) {
		Writer w;
		Guarded g;

		InitGuarded(g);
		w.U16(7);
		w.U16(0x0107);
		w.Size(lengths[i]);
		w.Fill('i', 64);
		zassert_false(DecodeBridgedDeviceV2(w.buf, w.n, g.rec), "length %zx accepted", lengths[i]);
		zassert_true(GuardIntact(g));
	}
}

ZTEST(matter_storage, test_wrap_label_length_rejected)
{
	static const size_t lengths[] = { SIZE_MAX, SIZE_MAX - 7 };

	for (size_t i = 0; i < ARRAY_SIZE(lengths); i++) {
		Writer w;
		Guarded g;

		InitGuarded(g);
		Header(w, 'i', 18);
		w.Size(lengths[i]);
		w.Fill('l', 64);
		zassert_false(DecodeBridgedDeviceV2(w.buf, w.n, g.rec), "length %zx accepted", lengths[i]);
		zassert_true(GuardIntact(g));
	}
}

ZTEST(matter_storage, test_wrap_v1_label_length_rejected)
{
	Writer w;
	RecordV1 r;

	w.U16(3);
	w.U16(0x0302);
	w.Size(SIZE_MAX - 1);
	w.Fill('l', 16);
	zassert_false(DecodeBridgedDeviceV1(w.buf, w.n, r));
}
