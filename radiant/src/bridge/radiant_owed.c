/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_owed.c - see radiant_owed.h. Pure functions; no I/O, no locking.
 */

#include <string.h>

#include "radiant_owed.h"

void radiant_owed_init(struct radiant_owed *o)
{
	memset(o, 0, sizeof(*o));
}

bool radiant_owed_add(struct radiant_owed *o, uint64_t uuid)
{
	uint32_t i;
	int32_t hole = -1;

	if (uuid == 0u) {
		return true;
	}
	for (i = 0u; i < RADIANT_OWED_MAX; i++) {
		if (o->uuid[i] == uuid) {
			return true;
		}
		if (o->uuid[i] == 0u && hole < 0) {
			hole = (int32_t)i;
		}
	}
	if (hole < 0) {
		o->overflow++;
		return false;
	}
	o->uuid[hole] = uuid;
	return true;
}

bool radiant_owed_cancel(struct radiant_owed *o, uint64_t uuid)
{
	bool removed = false;
	uint32_t i;

	if (uuid == 0u) {
		return false;
	}
	for (i = 0u; i < RADIANT_OWED_MAX; i++) {
		if (o->uuid[i] == uuid) {
			o->uuid[i] = 0u;
			removed = true;
		}
	}
	return removed;
}

uint64_t radiant_owed_at(const struct radiant_owed *o, uint32_t i)
{
	return (i < RADIANT_OWED_MAX) ? o->uuid[i] : 0u;
}

bool radiant_owed_any(const struct radiant_owed *o)
{
	uint32_t i;

	for (i = 0u; i < RADIANT_OWED_MAX; i++) {
		if (o->uuid[i] != 0u) {
			return true;
		}
	}
	return false;
}

uint32_t radiant_owed_drain(struct radiant_owed *o, const struct radiant_owed_ops *ops)
{
	uint32_t sent = 0u;
	uint32_t i;

	for (i = 0u; i < RADIANT_OWED_MAX; i++) {
		uint64_t uuid;

		ops->lock(ops->ctx);
		uuid = o->uuid[i];
		ops->unlock(ops->ctx);

		if (uuid == 0u || ops->send(ops->ctx, uuid) != 0) {
			continue;
		}

		ops->lock(ops->ctx);
		(void)radiant_owed_cancel(o, uuid);
		ops->unlock(ops->ctx);
		sent++;
	}
	return sent;
}
