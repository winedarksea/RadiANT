/*
 * SPDX-License-Identifier: Apache-2.0
 */

/* See bridge_storage_codec.h. No CHIP, no Zephyr, no storage: this file is
 * linked as-is into apps/dongle_thread/tests/matter_storage. */

#include "bridge_storage_codec.h"

#include <string.h>

namespace RadiantMatter
{
namespace StorageCodec
{

namespace
{
/* True if `want` bytes are left after `counter` in a `size`-byte buffer. The
 * first comparison keeps the subtraction from wrapping; the second is the
 * only form of the check that cannot wrap for a `want` near SIZE_MAX. */
bool Left(size_t size, size_t counter, size_t want)
{
	return counter <= size && want <= size - counter;
}
} /* namespace */

bool ReadField(const uint8_t *buf, size_t size, size_t &counter, void *dst, size_t fieldSize)
{
	if (!Left(size, counter, fieldSize)) {
		return false;
	}

	memcpy(dst, buf + counter, fieldSize);
	counter += fieldSize;
	return true;
}

bool ReadString(const uint8_t *buf, size_t size, size_t &counter, size_t strLen, char *dst, size_t dstSize)
{
	if (!StringFits(strLen, dstSize) || !Left(size, counter, strLen)) {
		return false;
	}

	memcpy(dst, buf + counter, strLen);
	dst[strLen] = '\0';
	counter += strLen;
	return true;
}

bool ReadUserData(const uint8_t *buf, size_t size, size_t &counter, uint8_t *userData, size_t &userDataSize)
{
	/* The caller did not ask for user data. Whatever follows is ignored. */
	if (userData == nullptr) {
		userDataSize = 0;
		return true;
	}

	const size_t capacity = userDataSize;
	size_t stored;

	if (!ReadField(buf, size, counter, &stored, sizeof(stored))) {
		return false;
	}

	/* More than the caller's buffer holds. */
	if (stored > capacity) {
		return false;
	}

	if (!Left(size, counter, stored)) {
		return false;
	}

	memcpy(userData, buf + counter, stored);
	counter += stored;
	userDataSize = stored;
	return true;
}

} /* namespace StorageCodec */

namespace EndpointHighWater
{

void Encode(uint16_t next, uint8_t (&out)[kSize])
{
	memcpy(out, &next, kSize);
}

bool Decode(const uint8_t *buf, size_t size, uint16_t &next)
{
	uint16_t value;

	if (buf == nullptr || size != kSize) {
		return false;
	}
	memcpy(&value, buf, kSize);
	if (value == kInvalid) {
		return false;
	}
	next = value;
	return true;
}

uint16_t Seed(uint16_t first, bool haveStored, uint16_t storedNext)
{
	return haveStored && storedNext > first ? storedNext : first;
}

namespace
{
/* The wrap guard: below the dynamic range (the wrap to 0) or kInvalid. */
uint16_t Wrap(uint16_t next, uint16_t first)
{
	return next < first || next == kInvalid ? first : next;
}
} /* namespace */

uint16_t Raise(uint16_t current, uint16_t restoredId, uint16_t first)
{
	return current > restoredId ? current : Wrap(static_cast<uint16_t>(restoredId + 1), first);
}

uint16_t Advance(uint16_t current, uint16_t first)
{
	return Wrap(static_cast<uint16_t>(current + 1), first);
}

bool NeedsMark(uint16_t id, uint16_t next, uint16_t persistedNext)
{
	return id >= persistedNext && next > id;
}

MarkLoad Classify(bool loaded, const uint8_t *buf, size_t size, bool present, bool marked, uint16_t &next)
{
	if (loaded) {
		return Decode(buf, size, next) ? MarkLoad::Valid : MarkLoad::Unreadable;
	}
	/* Missing beside the marker: written once, then lost (2026-11-22). */
	return (present || marked) ? MarkLoad::Unreadable : MarkLoad::Absent;
}

bool NeedsMarker(bool markStored, bool marked)
{
	return markStored && !marked;
}

uint16_t Adopt(Mark &mark, uint16_t first, MarkLoad load, uint16_t storedNext, uint16_t current)
{
	switch (load) {
	case MarkLoad::Valid: {
		const uint16_t seed = Seed(first, true, storedNext);

		mark.known = true;
		mark.persisted = storedNext;
		return current > seed ? current : seed;
	}
	case MarkLoad::Absent:
		mark.known = true;
		mark.persisted = 0;
		return current;
	case MarkLoad::Unreadable:
	default:
		mark.known = false;
		mark.persisted = 0;
		return current;
	}
}

bool NeedsWriteBack(const Mark &mark, uint16_t first, uint16_t current)
{
	return mark.known && current != Seed(first, mark.persisted != 0, mark.persisted);
}

bool MayAllocate(const Mark &mark)
{
	return mark.known;
}

} /* namespace EndpointHighWater */

namespace ProviderSlots
{

bool Admit(uint16_t count, uint16_t max, bool isNewProvider)
{
	return !isNewProvider || count < max;
}

void Inserted(uint16_t &count, bool isNewProvider)
{
	if (isNewProvider) {
		count++;
	}
}

void Erased(uint16_t &count, bool isNewProvider, bool erased)
{
	if (isNewProvider && erased && count > 0) {
		count--;
	}
}

} /* namespace ProviderSlots */

namespace LabelPersist
{

uint32_t Hash(const char *label, size_t maxLen)
{
	uint32_t h = 0x811c9dc5u;

	if (label == nullptr) {
		return h;
	}
	for (size_t i = 0; i < maxLen && label[i] != '\0'; i++) {
		h ^= static_cast<uint8_t>(label[i]);
		h *= 0x01000193u;
	}
	return h;
}

Decision Decide(const Stored &stored, uint32_t newHash, Budget &budget, uint32_t pass)
{
	if (stored.known && stored.hash == newHash) {
		return Decision::Unchanged;
	}
	if (budget.used > 0 && budget.pass == pass) {
		/* Another row of this rename event already paid. */
		return Decision::Persist;
	}
	if (budget.used >= kMaxPersistedRenamesPerSource) {
		return Decision::OverBudget;
	}
	budget.used++;
	budget.pass = pass;
	return Decision::Persist;
}

void Persisted(Stored &stored, uint32_t newHash)
{
	stored.known = true;
	stored.hash = newHash;
}

} /* namespace LabelPersist */
} /* namespace RadiantMatter */
