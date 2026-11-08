/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_owed.h - the MQTT sink's list of "offline" statuses still owed to the
 * broker.
 *
 * It was one uuid per SOURCE, so release A, bind B on the same source, release
 * B during a broker outage overwrote A's pending "offline": A's retained status
 * then read "online" until A was bound again (release review 2026-11-08, item
 * 4). It is now one de-duplicated list of sensor uuids, independent of which
 * source held them: 2 x RADIANT_BINDING_MAX entries, i.e. every source's
 * current sensor plus one displaced predecessor each.
 *
 * Pure data, no locking and no I/O. The caller (apps/dongle_thread/src/
 * mqtt_sink.c) holds its owed spinlock around every call, which is what lets
 * radiant/tests drive the policy.
 *
 * Slots hold 0 when empty (a uuid is never 0: radiant_binding.h). Order is
 * not meaningful.
 */
#ifndef RADIANT_OWED_H
#define RADIANT_OWED_H

#include <stdbool.h>
#include <stdint.h>

#include "radiant_binding.h"

#define RADIANT_OWED_MAX (2u * RADIANT_BINDING_MAX)

struct radiant_owed {
	uint64_t uuid[RADIANT_OWED_MAX];
	/* Adds that found the list full and were dropped. Should stay 0. */
	uint32_t overflow;
};

void radiant_owed_init(struct radiant_owed *o);

/* Records an owed offline. Already present: no-op, true. 0: ignored, true.
 * List full: counted in `overflow`, false. */
bool radiant_owed_add(struct radiant_owed *o, uint64_t uuid);

/* Removes `uuid` wherever it is. Used both by the drain after a SUCCESSFUL
 * send (only the uuid it sent) and by a bind, which cancels a match on any
 * source. True if something was removed. */
bool radiant_owed_cancel(struct radiant_owed *o, uint64_t uuid);

/* The uuid in slot `i` (0 = empty, or out of range). The drain walks the slots
 * by index so a failed send does not stall the rest. */
uint64_t radiant_owed_at(const struct radiant_owed *o, uint32_t i);

bool radiant_owed_any(const struct radiant_owed *o);

#endif /* RADIANT_OWED_H */
