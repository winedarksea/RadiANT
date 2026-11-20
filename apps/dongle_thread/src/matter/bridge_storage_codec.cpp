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
} /* namespace RadiantMatter */
