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
#include <stdio.h>
#include <string.h>

#include "bridge_storage_codec.h"

using namespace RadiantMatter::StorageCodec;
using namespace RadiantMatter;

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

/* The same shape contract bridge_core_asserts.cpp asserts on the real
 * BridgedDeviceV2 (types and field order), so the mirror cannot drift from
 * it without one side failing to compile. */
RADIANT_MATTER_ASSERT_RECORD_V2_SHAPE(RecordV2);

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
 * label, no user data.
 *
 * NOT A STORE/LOAD ROUND TRIP. The record is built by the hand-written Writer
 * above, not by StoreBridgedDevice()'s serialiser, which lives in the vendored
 * bridge_storage_manager.cpp behind CHIP and persistent-storage headers. That
 * the Writer's layout matches StoreBridgedDevice() (endpoint, type, length,
 * ID, length, label, then optional user-data size and bytes, each scalar as
 * its own native bytes) was established by reading it, in the 2026-11-20
 * review. */
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

/* ---------------------------------------------------------------------------
 * The endpoint high-water mark (security review 2026-11-20 item B), the
 * missing-versus-unreadable mark and the provider quota (security review
 * 2026-11-21).
 *
 * Bridge mirrors what BridgeManager and RestoreBridgedDevices() do with the
 * EndpointHighWater and ProviderSlots functions - Init's load and seed, the
 * restore and its raise, the write-back, AddSingleDevice()'s admission, mark
 * gate, insert, mark store and unwind - over a fake key-value store, so a
 * "reboot" is Boot() again with the persisted state kept. The CHIP side does
 * not build on native_sim; the logic it calls is what is under test here.
 * ------------------------------------------------------------------------- */

namespace
{

/* Fixed endpoints 0 (root), 1 (aggregator), 2 (placeholder). */
constexpr uint16_t kFirstDynamic = 3;

struct Bridge {
	/* Persisted. */
	bool hwPresent = false;
	uint8_t hw[EndpointHighWater::kSize + 1] = { 0 };
	size_t hwLen = 0;
	struct {
		uint16_t ep;
		bool present;
		bool restorable;
		bool live;
	} recs[8] = {};
	size_t nRecs = 0;
	unsigned hwWrites = 0;
	bool mkPresent = false; /* br/brd_ep_mk, the format marker */
	unsigned mkWrites = 0;

	/* Faults. */
	bool hwReadFault = false;  /* the key exists, its load fails */
	bool hwWriteFault = false; /* every store of the key fails */
	bool mkWriteFault = false; /* every store of the marker fails */
	bool createFault = false;  /* CreateEndpoint()'s internal-error path */

	/* Runtime. */
	uint16_t current = 0;
	EndpointHighWater::Mark mark; /* sEndpointMark */
	unsigned live = 0;            /* endpoints a controller could see */
	uint16_t providers = 0;       /* mNumberOfProviders */
	uint16_t maxProviders = 8;    /* kMaxDataProviders */

	bool StoreHw(uint16_t next)
	{
		uint8_t b[EndpointHighWater::kSize];

		if (hwWriteFault) {
			return false;
		}
		EndpointHighWater::Encode(next, b);
		memcpy(hw, b, sizeof(b));
		hwLen = sizeof(b);
		hwPresent = true;
		hwWrites++;
		/* BridgeStorageManager::StoreEndpointHighWater(): the marker
		 * after the mark, best effort. */
		if (EndpointHighWater::NeedsMarker(true, mkPresent)) {
			StoreMk();
		}
		return true;
	}

	void StoreMk()
	{
		if (!mkWriteFault) {
			mkPresent = true;
			mkWrites++;
		}
	}

	/* The NVS settings loader's answer to an entry that fails to read: it
	 * deletes the setting and never reports it, so it is simply gone. */
	void HideHw()
	{
		hwPresent = false;
		hwLen = 0;
	}

	/* A value put in the store behind the bridge's back (corruption, or a
	 * repair). */
	void PokeHw(const uint8_t *b, size_t n)
	{
		memcpy(hw, b, n);
		hwLen = n;
		hwPresent = true;
	}

	/* BridgeStorageManager::LoadEndpointHighWater(). */
	EndpointHighWater::MarkLoad LoadHw(uint16_t &stored)
	{
		const bool loaded = hwPresent && !hwReadFault;
		const EndpointHighWater::MarkLoad load =
			EndpointHighWater::Classify(loaded, hw, hwLen, hwPresent, mkPresent, stored);

		if (EndpointHighWater::NeedsMarker(load == EndpointHighWater::MarkLoad::Valid, mkPresent)) {
			StoreMk();
		}
		return load;
	}

	/* BridgeManager::Init(): load and seed, restore, persist a raised
	 * counter. */
	void Boot()
	{
		uint16_t stored = 0;
		const EndpointHighWater::MarkLoad load = LoadHw(stored);

		mark = EndpointHighWater::Mark{};
		live = 0;
		providers = 0;
		current = EndpointHighWater::Adopt(mark, kFirstDynamic, load, stored, kFirstDynamic);
		for (size_t i = 0; i < nRecs; i++) {
			recs[i].live = false;
			if (recs[i].present && recs[i].restorable) {
				/* AddSingleDevice() with an index: no mark gate. */
				zassert_true(ProviderSlots::Admit(providers, maxProviders, true));
				ProviderSlots::Inserted(providers, true);
				recs[i].live = true;
				live++;
				current = EndpointHighWater::Raise(current, recs[i].ep, kFirstDynamic);
			}
		}
		if (EndpointHighWater::NeedsWriteBack(mark, kFirstDynamic, current) && StoreHw(current)) {
			mark.persisted = current;
		}
	}

	/* AddSingleDevice() with no index: admit the provider, check the mark is
	 * known (re-reading it if not), insert, mark the id if the stored mark
	 * does not cover it, then CreateEndpoint() makes it live, then (in
	 * ant_bridge.cpp) the record is stored, unless power fails first.
	 * Returns 0 for a refused bind. */
	uint16_t Bind(bool recordStored = true)
	{
		if (!ProviderSlots::Admit(providers, maxProviders, true)) {
			return 0;
		}
		if (!EndpointHighWater::MayAllocate(mark)) {
			uint16_t stored = 0;
			const EndpointHighWater::MarkLoad load = LoadHw(stored);

			current = EndpointHighWater::Adopt(mark, kFirstDynamic, load, stored, current);
			if (!EndpointHighWater::MayAllocate(mark)) {
				return 0;
			}
		}
		ProviderSlots::Inserted(providers, true);

		const uint16_t id = current;
		const uint16_t next = EndpointHighWater::Advance(id, kFirstDynamic);

		if (EndpointHighWater::NeedsMark(id, next, mark.persisted)) {
			if (!StoreHw(next)) {
				ProviderSlots::Erased(providers, true, true);
				return 0;
			}
			mark.persisted = next;
		}
		current = next;
		if (createFault) {
			/* CreateEndpoint() -> SafelyRemoveDevice(): erases the pair and
			 * gives the slot back itself; AddSingleDevice()'s own erase
			 * then finds nothing. */
			providers--;
			ProviderSlots::Erased(providers, true, false);
			return 0;
		}
		live++;
		if (!recordStored) {
			return id;
		}
		zassert_true(nRecs < ARRAY_SIZE(recs));
		recs[nRecs].ep = id;
		recs[nRecs].present = true;
		recs[nRecs].restorable = true;
		recs[nRecs].live = true;
		nRecs++;
		return id;
	}

	/* RemoveEndpointForRow(): SafelyRemoveDevice() gives the slot back. */
	void Unbind(uint16_t ep)
	{
		for (size_t i = 0; i < nRecs; i++) {
			if (recs[i].ep == ep) {
				recs[i].present = false;
				if (recs[i].live) {
					recs[i].live = false;
					live--;
					providers--;
				}
			}
		}
	}

	/* A record that will not restore: corrupt, no heap, unserved type. */
	void Spoil(uint16_t ep)
	{
		for (size_t i = 0; i < nRecs; i++) {
			if (recs[i].ep == ep) {
				recs[i].restorable = false;
			}
		}
	}

	/* A store from a build before the key: records only. */
	void AddLegacyRecord(uint16_t ep)
	{
		zassert_true(nRecs < ARRAY_SIZE(recs));
		recs[nRecs].ep = ep;
		recs[nRecs].present = true;
		recs[nRecs].restorable = true;
		recs[nRecs].live = false;
		nRecs++;
	}
};

} /* namespace */

ZTEST(matter_storage, test_endpoint_hw_round_trip)
{
	static const uint16_t values[] = { kFirstDynamic, 4, 0x0100, 0x7FFF, 0xFFFE };

	for (size_t i = 0; i < ARRAY_SIZE(values); i++) {
		uint8_t b[EndpointHighWater::kSize];
		uint16_t out = 0;

		EndpointHighWater::Encode(values[i], b);
		zassert_true(EndpointHighWater::Decode(b, sizeof(b), out), "value %u", values[i]);
		zassert_equal(out, values[i]);
	}
}

ZTEST(matter_storage, test_endpoint_hw_decode_rejects_bad_values)
{
	uint8_t b[EndpointHighWater::kSize + 1] = { 9, 0, 0 };
	uint8_t invalid[EndpointHighWater::kSize];
	uint16_t out = 1234;

	zassert_false(EndpointHighWater::Decode(nullptr, EndpointHighWater::kSize, out));
	zassert_false(EndpointHighWater::Decode(b, 0, out));
	zassert_false(EndpointHighWater::Decode(b, EndpointHighWater::kSize - 1, out));
	zassert_false(EndpointHighWater::Decode(b, EndpointHighWater::kSize + 1, out));
	EndpointHighWater::Encode(EndpointHighWater::kInvalid, invalid);
	zassert_false(EndpointHighWater::Decode(invalid, sizeof(invalid), out));
	zassert_equal(out, 1234, "a refused value was written out");
}

ZTEST(matter_storage, test_endpoint_hw_seed)
{
	zassert_equal(EndpointHighWater::Seed(kFirstDynamic, false, 40), kFirstDynamic);
	zassert_equal(EndpointHighWater::Seed(kFirstDynamic, true, 1), kFirstDynamic);
	zassert_equal(EndpointHighWater::Seed(kFirstDynamic, true, kFirstDynamic), kFirstDynamic);
	zassert_equal(EndpointHighWater::Seed(kFirstDynamic, true, 40), 40);
}

ZTEST(matter_storage, test_endpoint_hw_advance_keeps_the_wrap_guard)
{
	zassert_equal(EndpointHighWater::Advance(kFirstDynamic, kFirstDynamic), kFirstDynamic + 1);
	zassert_equal(EndpointHighWater::Advance(0xFFFF, kFirstDynamic), kFirstDynamic);
	zassert_equal(EndpointHighWater::Raise(10, 4, kFirstDynamic), 10);
	zassert_equal(EndpointHighWater::Raise(4, 10, kFirstDynamic), 11);
}

/* 0xFFFF is chip::kInvalidEndpointId: never tried, never stored. Upstream's
 * guard only caught the wrap to 0, so 0xFFFE advanced (or a restored 0xFFFE
 * raised) to 0xFFFF - CreateEndpoint() then failed, and the stored 0xFFFF
 * was refused at the next boot, which fell back to reuse. */
ZTEST(matter_storage, test_endpoint_hw_never_yields_the_invalid_id)
{
	zassert_equal(EndpointHighWater::Advance(0xFFFE, kFirstDynamic), kFirstDynamic);
	zassert_equal(EndpointHighWater::Advance(0xFFFD, kFirstDynamic), 0xFFFE);
	zassert_equal(EndpointHighWater::Raise(10, 0xFFFE, kFirstDynamic), kFirstDynamic);
	zassert_equal(EndpointHighWater::Raise(kFirstDynamic, 0xFFFD, kFirstDynamic), 0xFFFE);
}

ZTEST(matter_storage, test_endpoint_hw_needs_mark)
{
	zassert_true(EndpointHighWater::NeedsMark(3, 4, 0), "nothing stored yet");
	zassert_true(EndpointHighWater::NeedsMark(3, 4, 3), "mark 3 does not cover id 3");
	zassert_false(EndpointHighWater::NeedsMark(3, 4, 4), "mark 4 covers id 3");
	zassert_false(EndpointHighWater::NeedsMark(5, 6, 9), "a covered id cost a write");
	zassert_false(EndpointHighWater::NeedsMark(0xFFFE, kFirstDynamic, 0xFFFE), "the wrap lowered the mark");
}

/* Power lost after CreateEndpoint() exposed the id and before its record was
 * stored: the mark was written first, so the id is not handed out again. */
ZTEST(matter_storage, test_endpoint_hw_power_loss_before_the_record_does_not_reuse)
{
	Bridge br;

	br.Boot();
	zassert_equal(br.Bind(), 3);
	zassert_equal(br.Bind(false), 4);
	br.Boot();
	zassert_equal(br.Bind(), 5, "endpoint 4 was exposed, lost, and reused");
	zassert_equal(br.hwWrites, 3, "one write per new id, and only one");
}

/* No key at all: exactly upstream's max(restored) + 1. */
ZTEST(matter_storage, test_endpoint_hw_missing_key_falls_back_to_max_restored_plus_one)
{
	Bridge br;

	br.AddLegacyRecord(3);
	br.AddLegacyRecord(7);
	br.AddLegacyRecord(5);
	br.Boot();
	zassert_equal(br.current, 8);

	Bridge empty;

	empty.Boot();
	zassert_equal(empty.current, kFirstDynamic);
	zassert_equal(empty.hwWrites, 0, "an empty bridge wrote the key at boot");
}

/* The plain case: unbind the newest sensor, reboot, bind another. */
ZTEST(matter_storage, test_endpoint_hw_removed_top_endpoint_not_reused_after_reboot)
{
	Bridge br;

	br.Boot();
	zassert_equal(br.Bind(), 3);
	zassert_equal(br.Bind(), 4);
	zassert_equal(br.Bind(), 5);
	br.Unbind(5);
	br.Boot();
	zassert_equal(br.Bind(), 6, "endpoint 5 was reused for a different device");
}

/* The top record does not restore (corrupt, heap, unserved type). */
ZTEST(matter_storage, test_endpoint_hw_skipped_top_record_not_reused_after_reboot)
{
	Bridge br;

	br.Boot();
	br.Bind();
	br.Bind();
	const uint16_t top = br.Bind();

	br.Spoil(top);
	br.Boot();
	zassert_equal(br.Bind(), top + 1, "endpoint %u was reused for a different device", top);

	/* Every record gone, two reboots: still past everything ever used. */
	br.Unbind(3);
	br.Unbind(4);
	br.Unbind(top + 1);
	br.Boot();
	br.Boot();
	zassert_equal(br.Bind(), top + 2);
}

/* A store from before the key: the first boot persists what it restored, so
 * unbinding the top device before any new bind cannot reuse it either. */
ZTEST(matter_storage, test_endpoint_hw_upgraded_store_persists_restored_high_water)
{
	Bridge br;

	br.AddLegacyRecord(3);
	br.AddLegacyRecord(4);
	br.AddLegacyRecord(5);
	br.Boot();
	zassert_equal(br.current, 6);
	zassert_true(br.hwPresent, "the restored high-water mark was not persisted");
	br.Unbind(5);
	br.Boot();
	zassert_equal(br.Bind(), 6, "endpoint 5 was reused after an upgrade");

	/* And a boot that changes nothing writes nothing. */
	const unsigned writes = br.hwWrites;

	br.Boot();
	zassert_equal(br.hwWrites, writes, "an unchanged boot rewrote the key");
}

/* ---- missing versus unreadable mark (security review 2026-11-21) ------- */

ZTEST(matter_storage, test_endpoint_hw_classify)
{
	uint8_t good[EndpointHighWater::kSize];
	uint8_t invalid[EndpointHighWater::kSize];
	uint16_t out = 1234;

	EndpointHighWater::Encode(9, good);
	EndpointHighWater::Encode(EndpointHighWater::kInvalid, invalid);

	zassert_equal(EndpointHighWater::Classify(false, good, 0, false, false, out), EndpointHighWater::MarkLoad::Absent);
	zassert_equal(EndpointHighWater::Classify(false, good, 0, true, false, out), EndpointHighWater::MarkLoad::Unreadable,
		      "a key that exists but did not load was taken for a legacy store");
	zassert_equal(EndpointHighWater::Classify(true, good, 1, true, false, out), EndpointHighWater::MarkLoad::Unreadable,
		      "a short value was taken for a legacy store");
	zassert_equal(EndpointHighWater::Classify(true, good, EndpointHighWater::kSize + 1, true, false, out),
		      EndpointHighWater::MarkLoad::Unreadable, "an oversized value was taken for a legacy store");
	zassert_equal(EndpointHighWater::Classify(true, invalid, sizeof(invalid), true, false, out),
		      EndpointHighWater::MarkLoad::Unreadable, "0xFFFF was taken for a legacy store");
	zassert_equal(out, 1234, "a refused value was written out");
	zassert_equal(EndpointHighWater::Classify(true, good, sizeof(good), true, false, out), EndpointHighWater::MarkLoad::Valid);
	zassert_equal(out, 9);

	/* With the marker (security review 2026-11-22): a missing key is a
	 * lost one. Every other row reads as without it. */
	out = 1234;
	zassert_equal(EndpointHighWater::Classify(false, good, 0, false, true, out),
		      EndpointHighWater::MarkLoad::Unreadable, "a hidden key on a marked store was taken for a legacy store");
	zassert_equal(EndpointHighWater::Classify(false, good, 0, true, true, out), EndpointHighWater::MarkLoad::Unreadable);
	zassert_equal(EndpointHighWater::Classify(true, invalid, sizeof(invalid), true, true, out),
		      EndpointHighWater::MarkLoad::Unreadable);
	zassert_equal(out, 1234);
	zassert_equal(EndpointHighWater::Classify(true, good, sizeof(good), true, true, out), EndpointHighWater::MarkLoad::Valid);
	zassert_equal(out, 9);

	/* The marker follows the mark, never precedes it. */
	zassert_true(EndpointHighWater::NeedsMarker(true, false));
	zassert_false(EndpointHighWater::NeedsMarker(true, true));
	zassert_false(EndpointHighWater::NeedsMarker(false, false), "a marker beside no mark reads as a lost mark");
	zassert_false(EndpointHighWater::NeedsMarker(false, true));
}

ZTEST(matter_storage, test_endpoint_hw_unknown_mark_blocks_allocation_and_write_back)
{
	EndpointHighWater::Mark mark;

	zassert_equal(EndpointHighWater::Adopt(mark, kFirstDynamic, EndpointHighWater::MarkLoad::Unreadable, 0, 7), 7);
	zassert_false(EndpointHighWater::MayAllocate(mark), "an unreadable mark allowed a new id");
	zassert_false(EndpointHighWater::NeedsWriteBack(mark, kFirstDynamic, 7),
		      "an unreadable mark would be overwritten with max(restored) + 1");

	/* A later read that succeeds raises the counter to the stored mark, or
	 * keeps a counter the restore raised past it. */
	zassert_equal(EndpointHighWater::Adopt(mark, kFirstDynamic, EndpointHighWater::MarkLoad::Valid, 12, 7), 12);
	zassert_true(EndpointHighWater::MayAllocate(mark));
	zassert_equal(mark.persisted, 12);
	zassert_equal(EndpointHighWater::Adopt(mark, kFirstDynamic, EndpointHighWater::MarkLoad::Valid, 5, 7), 7);

	/* Confirmed absence is the legacy fallback, as before. */
	EndpointHighWater::Mark legacy;

	zassert_equal(EndpointHighWater::Adopt(legacy, kFirstDynamic, EndpointHighWater::MarkLoad::Absent, 0,
					       kFirstDynamic),
		      kFirstDynamic);
	zassert_true(EndpointHighWater::MayAllocate(legacy));
	zassert_true(EndpointHighWater::NeedsWriteBack(legacy, kFirstDynamic, 6));
	zassert_false(EndpointHighWater::NeedsWriteBack(legacy, kFirstDynamic, kFirstDynamic));
}

/* The review's case: the top endpoint was unbound, then the mark cannot be
 * read at the next boot. The fallback would hand its id to a new sensor. */
ZTEST(matter_storage, test_endpoint_hw_read_fault_does_not_reuse_unbound_top)
{
	Bridge br;

	br.Boot();
	zassert_equal(br.Bind(), 3);
	zassert_equal(br.Bind(), 4);
	zassert_equal(br.Bind(), 5);
	br.Unbind(5);

	const unsigned writes = br.hwWrites;

	br.hwReadFault = true;
	br.Boot();
	zassert_equal(br.live, 2, "the stored devices were not restored");
	zassert_equal(br.hwWrites, writes, "the unreadable mark was overwritten at boot");
	for (int i = 0; i < 3; i++) {
		zassert_equal(br.Bind(), 0, "a new id (endpoint 5?) was handed out under an unreadable mark");
	}
	zassert_equal(br.providers, 2, "a deferred bind kept a provider slot");

	/* The store recovers: the next retry reads the mark and goes on past it. */
	br.hwReadFault = false;
	zassert_equal(br.Bind(), 6, "endpoint 5 was reused after the read recovered");
}

/* A malformed value (wrong size, or 0xFFFF) is unreadable too, not legacy. */
ZTEST(matter_storage, test_endpoint_hw_malformed_mark_defers_until_repaired)
{
	uint8_t invalid[EndpointHighWater::kSize];
	uint8_t repaired[EndpointHighWater::kSize];
	const uint8_t shortValue[1] = { 9 };
	Bridge br;

	EndpointHighWater::Encode(EndpointHighWater::kInvalid, invalid);
	br.AddLegacyRecord(3);
	br.AddLegacyRecord(4);
	br.AddLegacyRecord(5);
	br.PokeHw(invalid, sizeof(invalid));
	br.Boot();
	zassert_equal(br.live, 3);
	zassert_equal(br.current, 6, "restored ids did not raise the counter");
	zassert_equal(br.hwWrites, 0, "a malformed mark was overwritten at boot");
	zassert_equal(br.Bind(), 0, "a new id was handed out under a malformed mark");

	br.PokeHw(shortValue, sizeof(shortValue));
	zassert_equal(br.Bind(), 0, "a new id was handed out under a short mark");

	EndpointHighWater::Encode(9, repaired);
	br.PokeHw(repaired, sizeof(repaired));
	zassert_equal(br.Bind(), 9, "the repaired mark was not adopted");
	zassert_equal(br.Bind(), 10);
}

/* ---- a hidden mark: the format marker (security review 2026-11-22) ----- */

/* The review's case again, with the fault Zephyr really produces: the mark is
 * not unreadable but GONE (the NVS loader deleted it). Without the marker this
 * is a legacy store and max(restored) + 1 hands out the unbound top id. */
ZTEST(matter_storage, test_endpoint_hw_hidden_mark_on_a_marked_store_does_not_reuse)
{
	Bridge br;

	br.Boot();
	zassert_equal(br.Bind(), 3);
	zassert_equal(br.Bind(), 4);
	zassert_equal(br.Bind(), 5);
	zassert_true(br.mkPresent, "the first stored mark wrote no marker");
	br.Unbind(5);

	br.HideHw();
	br.Boot();
	zassert_equal(br.live, 2, "the stored devices were not restored");
	zassert_false(br.hwPresent, "the hidden mark was overwritten with max(restored) + 1 at boot");
	for (int i = 0; i < 3; i++) {
		zassert_equal(br.Bind(), 0, "endpoint 5 was handed out after its mark vanished");
	}
	zassert_equal(br.providers, 2, "a deferred bind kept a provider slot");
}

/* One marker write per store, not one per mark write. */
ZTEST(matter_storage, test_endpoint_hw_marker_is_written_once)
{
	Bridge br;

	br.Boot();
	zassert_false(br.mkPresent, "an empty bridge wrote the marker before any mark");
	for (int i = 0; i < 4; i++) {
		zassert_not_equal(br.Bind(), 0);
	}
	br.Boot();
	zassert_equal(br.Bind(), 7);
	zassert_equal(br.mkWrites, 1);
}

/* A store from before the marker (it has the mark): the boot that finds the
 * mark Valid writes the marker, so the next loss is caught. */
ZTEST(matter_storage, test_endpoint_hw_upgraded_store_gets_its_marker_at_boot)
{
	Bridge br;
	uint8_t b[EndpointHighWater::kSize];

	br.AddLegacyRecord(3);
	br.AddLegacyRecord(4);
	EndpointHighWater::Encode(6, b);
	br.PokeHw(b, sizeof(b));
	br.Boot();
	zassert_true(br.mkPresent, "a Valid mark on an unmarked store got no marker");
	zassert_equal(br.hwWrites, 0, "an unchanged mark was rewritten");

	br.HideHw();
	br.Boot();
	zassert_equal(br.Bind(), 0, "endpoint 5 or 6 was handed out after the mark vanished");
}

/* The marker lost alone: the mark still loads, nothing is at risk, and the
 * marker is put back. Both lost is the double fault the design accepts. */
ZTEST(matter_storage, test_endpoint_hw_lost_marker_alone_is_rewritten)
{
	Bridge br;

	br.Boot();
	zassert_equal(br.Bind(), 3);
	zassert_equal(br.Bind(), 4);
	br.mkPresent = false;
	br.Boot();
	zassert_true(br.mkPresent);
	zassert_equal(br.Bind(), 5);
}

/* A marker write that fails is retried with the next mark write or load, and
 * until then the store reads as unmarked (as before the marker existed). */
ZTEST(matter_storage, test_endpoint_hw_failed_marker_write_is_retried)
{
	Bridge br;

	br.mkWriteFault = true;
	br.Boot();
	zassert_equal(br.Bind(), 3);
	zassert_false(br.mkPresent);
	br.mkWriteFault = false;
	zassert_equal(br.Bind(), 4);
	zassert_true(br.mkPresent, "the marker was not retried with the next mark");
}

/* ---- the provider quota (security review 2026-11-21) ------------------- */

ZTEST(matter_storage, test_provider_slots_follow_the_map)
{
	uint16_t count = 0;

	zassert_true(ProviderSlots::Admit(count, 2, true));
	zassert_equal(count, 0, "admission alone took a slot");
	ProviderSlots::Inserted(count, true);
	zassert_equal(count, 1);
	ProviderSlots::Erased(count, true, false);
	zassert_equal(count, 1, "an erase that removed nothing gave a slot back");
	ProviderSlots::Erased(count, true, true);
	zassert_equal(count, 0);

	/* A provider already in the map (a second device on it) is not counted. */
	ProviderSlots::Inserted(count, false);
	ProviderSlots::Erased(count, false, true);
	zassert_equal(count, 0);

	count = 2;
	zassert_false(ProviderSlots::Admit(count, 2, true), "the quota was not enforced");
	zassert_true(ProviderSlots::Admit(count, 2, false), "a known provider was refused");
	zassert_equal(count, 2);
}

/* The review's case: the mark write fails on every retry; once storage
 * recovers, a bind must still succeed. */
ZTEST(matter_storage, test_provider_slot_returned_after_failed_reservations)
{
	Bridge br;

	br.maxProviders = 2;
	br.Boot();
	br.hwWriteFault = true;
	for (int i = 0; i < 5; i++) {
		zassert_equal(br.Bind(), 0);
	}
	zassert_equal(br.providers, 0, "failed endpoint reservations kept %u provider slots", br.providers);

	br.hwWriteFault = false;
	zassert_equal(br.Bind(), 3, "storage recovered but the bind was refused");
	zassert_equal(br.Bind(), 4);
	zassert_equal(br.Bind(), 0, "the provider quota is no longer enforced");
	zassert_equal(br.providers, 2);
}

/* CreateEndpoint()'s internal-error path already gives the slot back through
 * SafelyRemoveDevice(); AddSingleDevice()'s unwind must not do it again. */
ZTEST(matter_storage, test_provider_slot_not_returned_twice)
{
	Bridge br;

	br.maxProviders = 2;
	br.Boot();
	zassert_equal(br.Bind(), 3);
	br.createFault = true;
	zassert_equal(br.Bind(), 0);
	zassert_equal(br.Bind(), 0);
	zassert_equal(br.providers, 1, "provider count %u after two failed creations", br.providers);
	br.createFault = false;
	zassert_equal(br.Bind(), 6);
	zassert_equal(br.Bind(), 0, "a double give-back let a third provider in");
}

/* ---- renamed-label writes (security review 2026-11-22) ----------------- */

namespace
{

/* ant_bridge.cpp's flush, reduced to the write decision: every row of a
 * source composes the same label in a pass and asks LabelPersist whether to
 * store it. Counts the StoreBridgedDevice() calls that would happen. */
struct ModelSource {
	static constexpr size_t kRows = 3;
	LabelPersist::Stored rows[kRows] = {};
	LabelPersist::Budget budget = {};
	uint32_t writes = 0;

	void Create(const char *label)
	{
		for (LabelPersist::Stored &r : rows) {
			LabelPersist::Persisted(r, LabelPersist::Hash(label, kNodeLabelSize - 1));
		}
	}

	void Pass(const char *label, uint32_t pass)
	{
		uint32_t h = LabelPersist::Hash(label, kNodeLabelSize - 1);

		for (LabelPersist::Stored &r : rows) {
			if (LabelPersist::Decide(r, h, budget, pass) == LabelPersist::Decision::Persist) {
				writes++;
				LabelPersist::Persisted(r, h);
			}
		}
	}
};

} /* namespace */

ZTEST(matter_storage, test_label_persist_hash_is_fnv1a_over_the_stored_bytes)
{
	/* FNV-1a 32 reference vectors. */
	zassert_equal(LabelPersist::Hash("", 31), 0x811c9dc5u);
	zassert_equal(LabelPersist::Hash("a", 31), 0xe40c292cu);
	zassert_equal(LabelPersist::Hash("foobar", 31), 0xbf9cf968u);
	/* Bounded by maxLen (a record stores at most capacity - 1 bytes) and
	 * by the NUL, whichever comes first. */
	zassert_equal(LabelPersist::Hash("foobar", 3), LabelPersist::Hash("foo", 31));
	zassert_equal(LabelPersist::Hash(nullptr, 31), 0x811c9dc5u);
}

ZTEST(matter_storage, test_label_persist_unchanged_label_writes_nothing)
{
	/* The adoption case: every boot re-pushes each adopted row's composed
	 * name, which is normally the stored one. */
	ModelSource src;

	src.Create("Indoor Bike 51234 Speed");
	for (uint32_t pass = 1; pass <= 100; pass++) {
		src.Pass("Indoor Bike 51234 Speed", pass);
	}
	zassert_equal(src.writes, 0, "%u writes for an unchanged label", src.writes);
	zassert_equal(src.budget.used, 0, "an unchanged label charged the budget");
}

ZTEST(matter_storage, test_label_persist_unknown_stored_label_persists)
{
	/* A creation whose store failed: nothing is known about the record. */
	LabelPersist::Stored stored = {};
	LabelPersist::Budget budget = {};
	uint32_t h = LabelPersist::Hash("Treadmill 7 Speed", kNodeLabelSize - 1);

	zassert_equal(LabelPersist::Decide(stored, h, budget, 1), LabelPersist::Decision::Persist);
	zassert_equal(budget.used, 1);
	LabelPersist::Persisted(stored, h);
	zassert_equal(LabelPersist::Decide(stored, h, budget, 2), LabelPersist::Decision::Unchanged);
}

ZTEST(matter_storage, test_label_persist_alternating_label_is_bounded_per_boot)
{
	/* The review's attack: the equipment type flips each second, so the
	 * composed label alternates. Every row of the source used to be
	 * rewritten every pass - 300 writes here. */
	ModelSource src;
	uint32_t pass;

	src.Create("Indoor Bike 51234 Speed");
	for (pass = 1; pass <= 100; pass++) {
		src.Pass((pass & 1u) ? "Treadmill 51234 Speed" : "Indoor Bike 51234 Speed", pass);
	}
	zassert_equal(src.budget.used, LabelPersist::kMaxPersistedRenamesPerSource);
	zassert_equal(src.writes, LabelPersist::kMaxPersistedRenamesPerSource * ModelSource::kRows,
		      "%u writes", src.writes);

	/* Over budget the stored label stays whatever the last write left
	 * (here the bike name, written by pass 4), and a label equal to it is
	 * still Unchanged rather than OverBudget. */
	uint32_t bike = LabelPersist::Hash("Indoor Bike 51234 Speed", kNodeLabelSize - 1);
	uint32_t mill = LabelPersist::Hash("Treadmill 51234 Speed", kNodeLabelSize - 1);

	zassert_equal(LabelPersist::Decide(src.rows[0], bike, src.budget, pass),
		      LabelPersist::Decision::Unchanged);
	zassert_equal(LabelPersist::Decide(src.rows[0], mill, src.budget, pass + 1),
		      LabelPersist::Decision::OverBudget);
}

ZTEST(matter_storage, test_label_persist_one_charge_per_rename_event_per_source)
{
	ModelSource a;
	ModelSource b;

	a.Create("Fitness Equip 1 Speed");
	b.Create("Fitness Equip 2 Speed");

	/* One rename of A: all three rows written, one charge. */
	a.Pass("Indoor Bike 1 Speed", 1);
	zassert_equal(a.writes, ModelSource::kRows);
	zassert_equal(a.budget.used, 1, "one rename event charged %u", a.budget.used);

	/* A spending its whole budget leaves B's untouched. */
	a.Pass("Treadmill 1 Speed", 2);
	a.Pass("Rower 1 Speed", 3);
	a.Pass("Climber 1 Speed", 4);
	a.Pass("Elliptical 1 Speed", 5);
	zassert_equal(a.budget.used, LabelPersist::kMaxPersistedRenamesPerSource);
	b.Pass("Indoor Bike 2 Speed", 5);
	zassert_equal(b.writes, ModelSource::kRows, "a busy source spent another source's budget");
	zassert_equal(b.budget.used, 1);
}

/* ---- UniqueID forms (TODO 2026-11-24) ----------------------------------- */

namespace
{
constexpr uint64_t kKey = 0x3f2a91c40e77b212ULL;
constexpr uint8_t kTypeTemperature = 0x10; /* RADIANT_FIELD_TEMPERATURE */
constexpr uint8_t kTypeOccupancy = 0x02;   /* RADIANT_FIELD_OCCUPANCY */
} /* namespace */

ZTEST(matter_storage, test_unique_id_derived_is_the_old_form_byte_for_byte)
{
	static const uint64_t keys[] = { kKey, 0, UINT64_MAX, 0x00000001ffffffffULL, 0xabcdef0000000000ULL };
	static const uint8_t ids[] = { 0, 1, 4, 0x26, 0xff };
	char got[kUniqueIDSize + 1];
	char old[kUniqueIDSize + 1];

	/* A known vector, then the formatter ant_bridge.cpp used before. */
	zassert_equal(UniqueId::Format(got, sizeof(got), kKey, 5, kTypeOccupancy, true), 18u);
	zassert_mem_equal(got, "3f2a91c40e77b21205", 19);

	for (size_t k = 0; k < ARRAY_SIZE(keys); k++) {
		for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
			snprintf(old, sizeof(old), "%08x%08x%02x", static_cast<unsigned int>(keys[k] >> 32),
				 static_cast<unsigned int>(keys[k] & 0xFFFFFFFFu), ids[i]);
			/* The field type does not enter a derived id. */
			zassert_equal(UniqueId::Format(got, sizeof(got), keys[k], ids[i], kTypeTemperature, true),
				      UniqueId::kDerivedLength);
			zassert_mem_equal(got, old, UniqueId::kDerivedLength + 1, "key %u/%u id %u", (unsigned)k,
					  (unsigned)i, ids[i]);
		}
	}
}

ZTEST(matter_storage, test_unique_id_measured_carries_the_field_type)
{
	char temp[kUniqueIDSize + 1];
	char other[kUniqueIDSize + 1];
	char derived[kUniqueIDSize + 1];

	zassert_equal(UniqueId::Format(temp, sizeof(temp), kKey, 0, kTypeTemperature, false), 21u);
	zassert_mem_equal(temp, "3f2a91c40e77b21200t10", 22);
	zassert_equal(strlen(temp), UniqueId::kMeasuredLength);

	/* Within Matter's 32 and a stored record's 31 plus NUL. */
	zassert_true(UniqueId::kMeasuredLength <= UniqueId::kMatterMaxLength);
	zassert_true(UniqueId::kMeasuredLength < kUniqueIDSize);

	/* Same sensor and field id: another type, or the rule output on that id,
	 * is another device. */
	zassert_equal(UniqueId::Format(other, sizeof(other), kKey, 0, kTypeOccupancy, false), 21u);
	zassert_true(strcmp(temp, other) != 0);
	zassert_equal(UniqueId::Format(derived, sizeof(derived), kKey, 0, kTypeTemperature, true), 18u);
	zassert_true(strcmp(temp, derived) != 0);
	/* And the measured form is the derived one plus the suffix, so the stored
	 * prefix still reads as the sensor's key and field. */
	zassert_mem_equal(temp, derived, UniqueId::kDerivedLength);
	zassert_mem_equal(temp + UniqueId::kDerivedLength, "t10", 4);
}

ZTEST(matter_storage, test_unique_id_refuses_a_buffer_without_room_for_the_nul)
{
	char buf[kUniqueIDSize + 1];

	memset(buf, 'x', sizeof(buf));
	zassert_equal(UniqueId::Format(buf, UniqueId::kMeasuredLength, kKey, 0, kTypeTemperature, false), 0u);
	zassert_equal(buf[0], '\0');
	zassert_equal(buf[1], 'x', "wrote past the refusal");
	zassert_equal(UniqueId::Format(buf, UniqueId::kMeasuredLength + 1, kKey, 0, kTypeTemperature, false),
		      UniqueId::kMeasuredLength);
	zassert_equal(buf[UniqueId::kMeasuredLength], '\0');

	zassert_equal(UniqueId::Format(buf, UniqueId::kDerivedLength, kKey, 0, 0, true), 0u);
	zassert_equal(UniqueId::Format(buf, UniqueId::kDerivedLength + 1, kKey, 0, 0, true),
		      UniqueId::kDerivedLength);
	zassert_equal(UniqueId::Format(nullptr, 64, kKey, 0, 0, true), 0u);
	zassert_equal(UniqueId::Format(buf, 0, kKey, 0, 0, true), 0u);
}

/* ---- orphan collection (TODO 2026-11-24) -------------------------------- */

namespace
{
/* ant_bridge.cpp's RestoredDevice, as far as the policy reads it. */
struct ModelRestored {
	bool used;
	bool claimed;
	char uniqueID[kUniqueIDSize + 1];
};

ModelRestored Entry(bool used, bool claimed, const char *uniqueID)
{
	ModelRestored r = {};

	r.used = used;
	r.claimed = claimed;
	strncpy(r.uniqueID, uniqueID, sizeof(r.uniqueID) - 1);
	return r;
}

/* No current row wants anything. */
bool NoRowAdopts(const ModelRestored &)
{
	return false;
}

/* ant_bridge.cpp's RowWouldAdopt(): the UniqueIDs of the rows that exist now. */
struct RowsAdopt {
	const char *const *ids;
	size_t n;

	bool operator()(const ModelRestored &r) const
	{
		for (size_t i = 0; i < n; i++) {
			if (strcmp(ids[i], r.uniqueID) == 0) {
				return true;
			}
		}
		return false;
	}
};
} /* namespace */

ZTEST(matter_storage, test_orphan_collected_only_when_the_bridge_is_full)
{
	zassert_false(Orphans::Needed(0, 16));
	zassert_false(Orphans::Needed(15, 16));
	zassert_true(Orphans::Needed(16, 16));
	zassert_true(Orphans::Needed(17, 16));
}

ZTEST(matter_storage, test_orphan_pick_is_the_oldest_unclaimed)
{
	/* Creation order: 0 claimed, 1 a skipped (unreadable) record, 2 and 3
	 * unclaimed. 2 is the oldest orphan. */
	ModelRestored r[] = { Entry(true, true, "a"), Entry(false, false, ""), Entry(true, false, "c"),
			      Entry(true, false, "d") };

	zassert_equal(Orphans::PickOldest(r, ARRAY_SIZE(r), NoRowAdopts), 2);

	/* Collected (ant_bridge.cpp clears `used`): the next one is 3. */
	r[2].used = false;
	zassert_equal(Orphans::PickOldest(r, ARRAY_SIZE(r), NoRowAdopts), 3);

	/* A claim before collection takes it out of the running. */
	r[3].claimed = true;
	zassert_equal(Orphans::PickOldest(r, ARRAY_SIZE(r), NoRowAdopts), Orphans::kNone);
}

ZTEST(matter_storage, test_orphan_none_when_every_restored_endpoint_is_claimed)
{
	ModelRestored all[16];

	for (size_t i = 0; i < ARRAY_SIZE(all); i++) {
		all[i] = Entry(true, true, "x");
	}
	zassert_equal(Orphans::PickOldest(all, ARRAY_SIZE(all), NoRowAdopts), Orphans::kNone);

	ModelRestored none[4] = {};

	zassert_equal(Orphans::PickOldest(none, ARRAY_SIZE(none), NoRowAdopts), Orphans::kNone);
	zassert_equal(Orphans::PickOldest(static_cast<const ModelRestored *>(nullptr), 4, NoRowAdopts),
		      Orphans::kNone);

	/* The last entry alone. */
	all[15].claimed = false;
	zassert_equal(Orphans::PickOldest(all, ARRAY_SIZE(all), NoRowAdopts), 15);
}

ZTEST(matter_storage, test_orphan_pick_skips_one_a_current_row_will_adopt)
{
	/* 0 is the oldest unclaimed entry, but a row exists for it - created
	 * later in the same flush pass, say - so it is not an orphan. 1 is. */
	ModelRestored r[] = { Entry(true, false, "3f2a91c40e77b21200"), Entry(true, false, "00000000000000aa00t10"),
			      Entry(true, false, "00000000000000bb01") };
	static const char *const wantFirst[] = { "3f2a91c40e77b21200" };
	static const char *const wantTwo[] = { "00000000000000aa00t10", "3f2a91c40e77b21200" };

	zassert_equal(Orphans::PickOldest(r, ARRAY_SIZE(r), RowsAdopt{ wantFirst, ARRAY_SIZE(wantFirst) }), 1);
	zassert_equal(Orphans::PickOldest(r, ARRAY_SIZE(r), RowsAdopt{ wantTwo, ARRAY_SIZE(wantTwo) }), 2);
	/* A row whose UniqueID only shares a prefix does not protect it. */
	static const char *const prefix[] = { "00000000000000aa00" };

	zassert_equal(Orphans::PickOldest(r, ARRAY_SIZE(r), RowsAdopt{ prefix, ARRAY_SIZE(prefix) }), 0);
}

ZTEST(matter_storage, test_orphan_none_when_every_unclaimed_entry_is_wanted)
{
	ModelRestored r[] = { Entry(true, true, "a"), Entry(true, false, "b"), Entry(true, false, "c") };
	static const char *const want[] = { "c", "b" };

	/* Nothing to collect: the create fails and retries rather than evicting
	 * an endpoint a row is about to adopt. */
	zassert_equal(Orphans::PickOldest(r, ARRAY_SIZE(r), RowsAdopt{ want, ARRAY_SIZE(want) }), Orphans::kNone);
}

ZTEST(matter_storage, test_duplicates_of_an_adopted_unique_id_are_all_retired)
{
	/* A pre-2026-11-24 store: "x" three times (the mismatch path stored a
	 * second endpoint under the same UniqueID), in no useful order. Entry 2
	 * is adopted; 0 and 4 are its duplicates, 3 is unused, 1 is another
	 * device. */
	ModelRestored r[] = { Entry(true, false, "x"), Entry(true, false, "y"), Entry(true, true, "x"),
			      Entry(false, false, "x"), Entry(true, false, "x") };

	zassert_equal(Orphans::NextDuplicate(r, ARRAY_SIZE(r), "x", 2), 0);
	r[0].used = false; /* retired */
	zassert_equal(Orphans::NextDuplicate(r, ARRAY_SIZE(r), "x", 2), 4);
	r[4].used = false;
	zassert_equal(Orphans::NextDuplicate(r, ARRAY_SIZE(r), "x", 2), Orphans::kNone);
	zassert_true(r[1].used, "another device's record was touched");
}

ZTEST(matter_storage, test_duplicates_with_no_adopter_and_edges)
{
	ModelRestored r[] = { Entry(true, false, "x"), Entry(true, true, "x"), Entry(true, false, "xx") };

	/* No entry kept (the mismatch path): every unclaimed "x" goes, a
	 * claimed one never does, and "xx" is a different UniqueID. */
	zassert_equal(Orphans::NextDuplicate(r, ARRAY_SIZE(r), "x", Orphans::kNone), 0);
	r[0].used = false;
	zassert_equal(Orphans::NextDuplicate(r, ARRAY_SIZE(r), "x", Orphans::kNone), Orphans::kNone);

	zassert_equal(Orphans::NextDuplicate(static_cast<const ModelRestored *>(nullptr), 3, "x", 0), Orphans::kNone);
	zassert_equal(Orphans::NextDuplicate(r, ARRAY_SIZE(r), nullptr, 0), Orphans::kNone);
}

/* ---- restored endpoints that can never be claimed (2026-11-26) ---------- */

ZTEST(matter_storage, test_unique_id_parse_round_trips_both_forms)
{
	static const uint64_t keys[] = { kKey, 0, UINT64_MAX, 0x00000001ffffffffULL, 0xabcdef0000000000ULL };
	static const uint8_t ids[] = { 0, 1, 5, 0x26, 0xff };
	static const uint8_t types[] = { 0, kTypeOccupancy, kTypeTemperature, 0xff };
	char s[kUniqueIDSize + 1];

	for (size_t k = 0; k < ARRAY_SIZE(keys); k++) {
		for (size_t i = 0; i < ARRAY_SIZE(ids); i++) {
			for (size_t t = 0; t < ARRAY_SIZE(types); t++) {
				for (int d = 0; d < 2; d++) {
					uint64_t key = ~keys[k];
					uint8_t fieldId = static_cast<uint8_t>(~ids[i]);
					bool derived = d == 0;

					zassert_true(UniqueId::Format(s, sizeof(s), keys[k], ids[i], types[t], d != 0) > 0);
					zassert_true(UniqueId::Parse(s, &key, &fieldId, &derived), "%s", s);
					zassert_equal(key, keys[k], "%s", s);
					zassert_equal(fieldId, ids[i], "%s", s);
					zassert_equal(derived, d != 0, "%s", s);
				}
			}
		}
	}
}

ZTEST(matter_storage, test_unique_id_parse_rejects_bad_strings)
{
	static const char *const bad[] = {
		"",
		"3f2a91c40e77b2120",       /* 17: one short of derived */
		"3f2a91c40e77b212050",     /* 19 */
		"3f2a91c40e77b21205t1",    /* 20: one short of measured */
		"3f2a91c40e77b21205t100",  /* 22 */
		"3F2A91C40E77B21205",      /* uppercase: Format never writes it */
		"3f2a91c40e77b2120g",      /* non-hex field id */
		"3f2a91c4 e77b21205",      /* non-hex key */
		"-f2a91c40e77b21205",
		"3f2a91c40e77b21205x10",   /* measured without its 't' */
		"3f2a91c40e77b21205T10",
		"3f2a91c40e77b21205tg0",   /* non-hex field type */
		"3f2a91c40e77b2120t510",   /* 't' misplaced */
		"3f2a91c40e77b212051t0",
		"t3f2a91c40e77b21205a",
		"3f2a91c40e77b21205t10 ",
	};
	uint64_t key = 0x1234;
	uint8_t fieldId = 0x56;
	bool derived = true;

	for (size_t i = 0; i < ARRAY_SIZE(bad); i++) {
		zassert_false(UniqueId::Parse(bad[i], &key, &fieldId, &derived), "accepted \"%s\"", bad[i]);
	}
	/* Nothing written on a refusal. */
	zassert_equal(key, 0x1234u);
	zassert_equal(fieldId, 0x56);
	zassert_true(derived);

	/* A measured id is not derived, nor a derived one measured: the form is
	 * the whole string, never a prefix. */
	zassert_true(UniqueId::Parse("3f2a91c40e77b21205t10", &key, &fieldId, &derived));
	zassert_false(derived, "measured id read as derived");
	zassert_true(UniqueId::Parse("3f2a91c40e77b21205", &key, &fieldId, &derived));
	zassert_true(derived, "derived id read as measured");

	/* Nulls. */
	zassert_false(UniqueId::Parse(nullptr, &key, &fieldId, &derived));
	zassert_false(UniqueId::Parse("3f2a91c40e77b21205", nullptr, &fieldId, &derived));
	zassert_false(UniqueId::Parse("3f2a91c40e77b21205", &key, nullptr, &derived));
	zassert_false(UniqueId::Parse("3f2a91c40e77b21205", &key, &fieldId, nullptr));

	/* A string far longer than either form, unterminated within 22 bytes. */
	char longer[40];

	memset(longer, 'a', sizeof(longer) - 1);
	longer[sizeof(longer) - 1] = '\0';
	zassert_false(UniqueId::Parse(longer, &key, &fieldId, &derived));
}

namespace
{
/* radiant_rules_output_enabled() for a build with some outputs off: bit N set
 * means field id N is derived. */
struct OutputsOn {
	uint32_t mask;

	bool operator()(uint8_t fieldId) const
	{
		return fieldId < 32 && (mask & (1u << fieldId)) != 0;
	}
};

/* The field ids radiant_rules.h assigns (RADIANT_RULE_FIELD_*). */
constexpr uint8_t kFieldWorn = 0;
constexpr uint8_t kFieldAtRest = 2;
constexpr uint8_t kFieldZone2 = 3;
constexpr uint8_t kFieldZone3 = 5;
constexpr uint32_t kAllButZone3 = 0x1fu; /* 0..4 */

ModelRestored EntryFor(bool claimed, uint64_t key, uint8_t fieldId, uint8_t fieldType, bool derived)
{
	char id[kUniqueIDSize + 1];

	(void)UniqueId::Format(id, sizeof(id), key, fieldId, fieldType, derived);
	return Entry(true, claimed, id);
}
} /* namespace */

ZTEST(matter_storage, test_restored_retire_at_boot_picks_only_disabled_derived)
{
	constexpr uint64_t kOther = 0x00000000000000aaULL;
	/* 0 Zone 2 (on), 1 Zone 3 (off), 2 a temperature on field 5 (measured:
	 * not a rule output whatever its id), 3 unused, 4 another sensor's Zone
	 * 3, 5 an unparseable id, 6 At Rest (on). */
	ModelRestored r[] = { EntryFor(false, kKey, kFieldZone2, kTypeOccupancy, true),
			      EntryFor(false, kKey, kFieldZone3, kTypeOccupancy, true),
			      EntryFor(false, kKey, kFieldZone3, kTypeTemperature, false),
			      Entry(false, false, ""),
			      EntryFor(false, kOther, kFieldZone3, kTypeOccupancy, true),
			      Entry(true, false, "not a unique id"),
			      EntryFor(false, kKey, kFieldAtRest, kTypeOccupancy, true) };
	const OutputsOn noZone3{ kAllButZone3 };

	zassert_equal(Orphans::RestoredToRetireAtBoot(r, ARRAY_SIZE(r), 0, noZone3), 1);
	/* The caller asks again from the next index, retired or not. */
	zassert_equal(Orphans::RestoredToRetireAtBoot(r, ARRAY_SIZE(r), 2, noZone3), 4);
	zassert_equal(Orphans::RestoredToRetireAtBoot(r, ARRAY_SIZE(r), 5, noZone3), Orphans::kNone);

	/* Every output on: nothing goes. */
	zassert_equal(Orphans::RestoredToRetireAtBoot(r, ARRAY_SIZE(r), 0, OutputsOn{ 0xffffffffu }), Orphans::kNone);

	/* The rules sink not built (every output off): every derived entry, still
	 * no measured, unused or unparseable one. */
	const OutputsOn none{ 0 };
	int got[ARRAY_SIZE(r)];
	size_t n = 0;

	for (int i = Orphans::RestoredToRetireAtBoot(r, ARRAY_SIZE(r), 0, none); i != Orphans::kNone;
	     i = Orphans::RestoredToRetireAtBoot(r, ARRAY_SIZE(r), static_cast<size_t>(i) + 1, none)) {
		got[n++] = i;
	}
	zassert_equal(n, 4u);
	zassert_equal(got[0], 0);
	zassert_equal(got[1], 1);
	zassert_equal(got[2], 4);
	zassert_equal(got[3], 6);

	/* At Rest off alone picks only At Rest; Worn off picks nothing here. */
	zassert_equal(Orphans::RestoredToRetireAtBoot(r, ARRAY_SIZE(r), 0, OutputsOn{ ~(1u << kFieldAtRest) }), 6);
	zassert_equal(Orphans::RestoredToRetireAtBoot(r, ARRAY_SIZE(r), 0, OutputsOn{ ~(1u << kFieldWorn) }),
		      Orphans::kNone);

	/* Edges. */
	zassert_equal(Orphans::RestoredToRetireAtBoot(static_cast<const ModelRestored *>(nullptr), 4, 0, none),
		      Orphans::kNone);
	zassert_equal(Orphans::RestoredToRetireAtBoot(r, ARRAY_SIZE(r), ARRAY_SIZE(r), none), Orphans::kNone);
}

ZTEST(matter_storage, test_restored_purge_picks_only_unclaimed_with_the_key)
{
	constexpr uint64_t kOther = 0x00000000000000aaULL;
	/* The evicted sensor kKey: 0 Worn unclaimed, 1 Zone 2 claimed (a live
	 * row's, the unbind removes it), 2 a measured temperature unclaimed.
	 * 3 another sensor, 4 unused but kKey's, 5 kKey's Zone 3 unclaimed, 6 a
	 * key that differs only in its low bit. */
	ModelRestored r[] = { EntryFor(false, kKey, kFieldWorn, kTypeOccupancy, true),
			      EntryFor(true, kKey, kFieldZone2, kTypeOccupancy, true),
			      EntryFor(false, kKey, 0, kTypeTemperature, false),
			      EntryFor(false, kOther, kFieldWorn, kTypeOccupancy, true),
			      EntryFor(false, kKey, kFieldAtRest, kTypeOccupancy, true),
			      EntryFor(false, kKey, kFieldZone3, kTypeOccupancy, true),
			      EntryFor(false, kKey ^ 1u, kFieldWorn, kTypeOccupancy, true) };

	r[4].used = false;

	int got[ARRAY_SIZE(r)];
	size_t n = 0;

	for (int i = Orphans::RestoredToPurgeForKey(r, ARRAY_SIZE(r), 0, kKey); i != Orphans::kNone;
	     i = Orphans::RestoredToPurgeForKey(r, ARRAY_SIZE(r), static_cast<size_t>(i) + 1, kKey)) {
		got[n++] = i;
		r[i].used = false; /* retired */
	}
	zassert_equal(n, 3u);
	zassert_equal(got[0], 0);
	zassert_equal(got[1], 2);
	zassert_equal(got[2], 5);
	zassert_true(r[1].used && r[1].claimed, "a claimed entry was purged");
	zassert_true(r[3].used, "another sensor's entry was purged");
	zassert_true(r[6].used, "a neighbouring key's entry was purged");

	/* The other sensor's turn takes only its own. */
	zassert_equal(Orphans::RestoredToPurgeForKey(r, ARRAY_SIZE(r), 0, kOther), 3);
	zassert_equal(Orphans::RestoredToPurgeForKey(r, ARRAY_SIZE(r), 4, kOther), Orphans::kNone);

	/* An unparseable id never matches, not even key 0. */
	ModelRestored junk[] = { Entry(true, false, ""), Entry(true, false, "zz") };

	zassert_equal(Orphans::RestoredToPurgeForKey(junk, ARRAY_SIZE(junk), 0, 0), Orphans::kNone);
	zassert_equal(Orphans::RestoredToPurgeForKey(static_cast<const ModelRestored *>(nullptr), 4, 0, kKey),
		      Orphans::kNone);
}

ZTEST(matter_storage, test_purge_keys_dedup_cancel_and_overflow)
{
	Orphans::PurgeKeys<3> set = {};

	zassert_false(set.Pending());
	zassert_true(set.Add(1));
	zassert_true(set.Add(1), "a held key is not dropped");
	zassert_equal(set.count, 1);
	zassert_true(set.Pending());

	/* A re-bind before the flush cancels; a key not held is a no-op. */
	zassert_true(set.Add(2));
	set.Cancel(1);
	set.Cancel(99);
	zassert_equal(set.count, 1);
	zassert_equal(set.keys[0], 2u);

	/* Full: the next new key is dropped and counted, a held one still fits. */
	zassert_true(set.Add(3));
	zassert_true(set.Add(4));
	zassert_false(set.Add(5));
	zassert_true(set.Add(4));
	zassert_equal(set.count, 3);
	zassert_equal(set.dropped, 1);

	/* A drop alone keeps the flush coming, so it is logged. */
	set.count = 0;
	zassert_true(set.Pending());
	set.dropped = 0;
	zassert_false(set.Pending());

	/* The drop count saturates. */
	Orphans::PurgeKeys<1> one = {};

	zassert_true(one.Add(7));
	for (int i = 0; i < 300; i++) {
		(void)one.Add(static_cast<uint64_t>(100 + i));
	}
	zassert_equal(one.dropped, UINT8_MAX);
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
