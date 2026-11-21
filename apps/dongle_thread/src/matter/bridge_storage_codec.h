/*
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * The bounds-checked half of BridgeStorageManager: turning one stored bridged
 * device record back into a struct, and refusing to store one whose strings
 * would not come back.
 *
 * WHY THIS IS NOT IN bridge_storage_manager.cpp. Upstream's loader checked
 * only that a stored length fitted in the bytes read, never that it fitted in
 * the destination array: mUniqueID and mNodeLabel are char[32], the source
 * buffer is sizeof(BridgedDeviceV2) + 128 bytes, so a corrupted record with a
 * length near 200 overran the stack struct in RestoreBridgedDevices() at
 * boot - every boot. And `counter + length` could wrap. The decoding lives
 * here so that it includes no CHIP and no persistent-storage header and can be
 * tested on its own (apps/dongle_thread/tests/matter_storage).
 *
 * The rules, for every length-prefixed string:
 *   - the length must leave room for a NUL in the destination
 *     (length <= sizeof(dst) - 1), and the NUL is written;
 *   - the length must fit in the bytes LEFT, compared by subtraction
 *     (length > size - counter fails), never by `counter + length`, which
 *     wraps for a length near SIZE_MAX.
 * The user-data bounds are upstream's.
 *
 * The record format does not change: what was stored by any earlier build
 * decodes the same, because every writer this tree has ever had stored
 * NUL-terminated strings of at most 31 bytes.
 *
 * The decoders are templates over the record struct so that the struct stays
 * where upstream put it (nested in BridgeStorageManager, sized from a CHIP
 * constant) and every capacity is read with sizeof() off the real field.
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

/*
 * The shape a V2 record struct must have, stated once and asserted on both
 * sides: by bridge_core_asserts.cpp against the vendored
 * BridgeStorageManager::BridgedDeviceV2, and by
 * apps/dongle_thread/tests/matter_storage against its mirror RecordV2. If a
 * future NCS re-copy changes a field's type or reorders the struct, the
 * Matter build stops instead of the test quietly checking a different shape.
 *
 * Types: the serialised record holds each length as the field's own bytes
 * (sizeof(size_t), little-endian on every target), so the types ARE the
 * format. Order: the decoder reads by field name, so struct order does not
 * change the format; it is pinned so the mirror stays recognisably the same
 * struct, in the serialised order. The array capacities are asserted
 * separately on each side, as numbers, because the test needs them as
 * numbers.
 *
 * __is_same is the GCC/Clang builtin rather than std::is_same because the
 * test builds with Zephyr's minimal C++ library, which has no <type_traits>.
 */
#define RADIANT_MATTER_ASSERT_RECORD_V2_SHAPE(T)                                                                   \
	static_assert(__is_same(decltype(T::mEndpointId), uint16_t), #T "::mEndpointId is not uint16_t"); \
	static_assert(__is_same(decltype(T::mDeviceType), uint16_t), #T "::mDeviceType is not uint16_t"); \
	static_assert(__is_same(decltype(T::mUniqueIDLength), size_t),                                   \
		      #T "::mUniqueIDLength is not size_t");                                                        \
	static_assert(__is_same(decltype(T::mUniqueID), char[sizeof(T::mUniqueID)]),                \
		      #T "::mUniqueID is not a char array");                                                        \
	static_assert(__is_same(decltype(T::mNodeLabelLength), size_t),                                  \
		      #T "::mNodeLabelLength is not size_t");                                                       \
	static_assert(__is_same(decltype(T::mNodeLabel), char[sizeof(T::mNodeLabel)]),               \
		      #T "::mNodeLabel is not a char array");                                                       \
	static_assert(__is_same(decltype(T::mUserDataSize), size_t), #T "::mUserDataSize is not size_t"); \
	static_assert(__is_same(decltype(T::mUserData), uint8_t *), #T "::mUserData is not uint8_t *");  \
	static_assert(offsetof(T, mEndpointId) < offsetof(T, mDeviceType) &&                                       \
			      offsetof(T, mDeviceType) < offsetof(T, mUniqueIDLength) &&                           \
			      offsetof(T, mUniqueIDLength) < offsetof(T, mUniqueID) &&                             \
			      offsetof(T, mUniqueID) < offsetof(T, mNodeLabelLength) &&                            \
			      offsetof(T, mNodeLabelLength) < offsetof(T, mNodeLabel) &&                           \
			      offsetof(T, mNodeLabel) < offsetof(T, mUserDataSize) &&                              \
			      offsetof(T, mUserDataSize) < offsetof(T, mUserData),                                 \
		      #T "'s fields are not in the serialised order")

namespace RadiantMatter
{
namespace StorageCodec
{

/* Copies `size` bytes at buf[counter] into dst and advances counter. False,
 * with nothing copied, if fewer than `size` bytes are left. */
bool ReadField(const uint8_t *buf, size_t size, size_t &counter, void *dst, size_t fieldSize);

/* True if a string of strLen bytes plus its NUL fits in dstSize bytes. */
inline bool StringFits(size_t strLen, size_t dstSize)
{
	return dstSize > 0 && strLen <= dstSize - 1;
}

/* Copies a strLen-byte string at buf[counter] into dst, NUL-terminates it and
 * advances counter. False, with nothing copied, if it does not fit in dst
 * with its NUL or if fewer than strLen bytes are left. */
bool ReadString(const uint8_t *buf, size_t size, size_t &counter, size_t strLen, char *dst, size_t dstSize);

/* The optional trailing user data, upstream's rules: a null userData means
 * the caller does not want it (userDataSize becomes 0, success, whatever
 * follows). Otherwise a size field must follow, its value must not exceed the
 * caller's userDataSize, and that many bytes must be left. */
bool ReadUserData(const uint8_t *buf, size_t size, size_t &counter, uint8_t *userData, size_t &userDataSize);

/*
 * Version 2 record: endpoint id, device type, unique-id length, unique id,
 * node-label length, node label, then optional user-data size and user data.
 * T is BridgeStorageManager::BridgedDeviceV2 (or a struct of the same shape).
 */
template <class T> bool DecodeBridgedDeviceV2(const uint8_t *buf, size_t size, T &out)
{
	size_t counter = 0;

	if (buf == nullptr) {
		return false;
	}

	return ReadField(buf, size, counter, &out.mEndpointId, sizeof(out.mEndpointId)) &&
	       ReadField(buf, size, counter, &out.mDeviceType, sizeof(out.mDeviceType)) &&
	       ReadField(buf, size, counter, &out.mUniqueIDLength, sizeof(out.mUniqueIDLength)) &&
	       ReadString(buf, size, counter, out.mUniqueIDLength, out.mUniqueID, sizeof(out.mUniqueID)) &&
	       ReadField(buf, size, counter, &out.mNodeLabelLength, sizeof(out.mNodeLabelLength)) &&
	       ReadString(buf, size, counter, out.mNodeLabelLength, out.mNodeLabel, sizeof(out.mNodeLabel)) &&
	       ReadUserData(buf, size, counter, out.mUserData, out.mUserDataSize);
}

/*
 * Version 1 record: endpoint id, device type, node-label length, node label,
 * then optional user data. Only CONFIG_BRIDGE_MIGRATE_VERSION_1 reads it.
 */
template <class T> bool DecodeBridgedDeviceV1(const uint8_t *buf, size_t size, T &out)
{
	size_t counter = 0;

	if (buf == nullptr) {
		return false;
	}

	return ReadField(buf, size, counter, &out.mEndpointId, sizeof(out.mEndpointId)) &&
	       ReadField(buf, size, counter, &out.mDeviceType, sizeof(out.mDeviceType)) &&
	       ReadField(buf, size, counter, &out.mNodeLabelLength, sizeof(out.mNodeLabelLength)) &&
	       ReadString(buf, size, counter, out.mNodeLabelLength, out.mNodeLabel, sizeof(out.mNodeLabel)) &&
	       ReadUserData(buf, size, counter, out.mUserData, out.mUserDataSize);
}

/* The store-side twin: a V2 record is only written if both of its strings
 * would decode, so a bad caller fails at store time instead of producing a
 * record that is refused at every boot. */
template <class T> bool StorableBridgedDeviceV2(const T &device)
{
	return StringFits(device.mUniqueIDLength, sizeof(device.mUniqueID)) &&
	       StringFits(device.mNodeLabelLength, sizeof(device.mNodeLabel));
}

} /* namespace StorageCodec */

/*
 * The bridged-endpoint high-water mark (security review 2026-11-20 item B).
 *
 * Matter forbids reusing a bridged endpoint id for a different device.
 * Upstream BridgeManager rebuilt its next-id counter at every boot as
 * max(restored ids) + 1, so an id came back into use whenever its record was
 * not restored: the highest endpoint was unbound before a reboot, or its
 * record failed to load or to restore (corrupt, CHIP heap, a device type this
 * build does not serve). BridgeManager now persists the next id it will hand
 * out (br/brd_ep_hw, see BridgeStorageManager) before each new id goes live and
 * seeds the counter from it at boot. A store written before the key existed
 * decodes as absent and behaves as upstream did; restoring then raises the
 * counter and writes the key once.
 *
 * The stored value is the NEXT id (one past the highest ever handed out), as
 * a uint16_t in native byte order (little-endian on every target), the way
 * brd_cnt stores its uint8_t. These functions are the whole of the counter
 * arithmetic BridgeManager uses, so apps/dongle_thread/tests/matter_storage
 * can test it, and a model of the bind/unbind/reboot sequence, without CHIP.
 * The test does not exercise bridge_manager.cpp itself (see src/matter/README.md).
 */
namespace EndpointHighWater
{

constexpr size_t kSize = sizeof(uint16_t);
/* chip::kInvalidEndpointId. Never a valid next id: stored, it is corrupt. */
constexpr uint16_t kInvalid = 0xFFFF;

void Encode(uint16_t next, uint8_t (&out)[kSize]);

/* False (next untouched) unless buf holds exactly one valid stored value. */
bool Decode(const uint8_t *buf, size_t size, uint16_t &next);

/* The counter at boot, before any record is restored: the stored next id if
 * there is one and it is past the first dynamic id, else the first. */
uint16_t Seed(uint16_t first, bool haveStored, uint16_t storedNext);

/* Upstream's raise past a restored endpoint id, and upstream's step after a
 * fresh allocation with its wrap guard. Both differ from upstream in one
 * way: a result of kInvalid (from 0xFFFE) wraps to `first` like the wrap to 0
 * already did, so 0xFFFF is never tried as an endpoint id and never stored -
 * a stored 0xFFFF is refused at boot, which would fall back to reuse. */
uint16_t Raise(uint16_t current, uint16_t restoredId, uint16_t first);
uint16_t Advance(uint16_t current, uint16_t first);

/* True if trying endpoint `id` (whose successor is `next`) needs the mark
 * written first: the stored mark `persistedNext` does not cover it yet.
 * BridgeManager writes `next` BEFORE CreateEndpoint() makes `id` visible to a
 * controller, so a power loss between the two cannot leave an exposed id
 * uncovered; a failed creation only burns an id nobody saw. An id the mark
 * already covers (the ENDPOINT_EXISTS skip loop only meets those) costs no
 * write. At the 16-bit wrap (next <= id) nothing is written: the mark keeps
 * its top value, and ids after the wrap are upstream's reuse, as before. */
bool NeedsMark(uint16_t id, uint16_t next, uint16_t persistedNext);

} /* namespace EndpointHighWater */
} /* namespace RadiantMatter */
