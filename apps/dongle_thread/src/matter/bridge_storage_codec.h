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
} /* namespace RadiantMatter */
