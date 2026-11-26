/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_binding.c - see radiant_binding.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>

#include <radiant/radiant_binding.h>

static struct radiant_binding table[RADIANT_BINDING_MAX];

void radiant_binding_init(void)
{
	memset(table, 0, sizeof(table));
}

static bool same_key(const struct radiant_binding *b, uint16_t devnum,
		     uint8_t devtype, uint8_t trans_type)
{
	return b->used && b->devnum == devnum && b->devtype == devtype &&
	       b->trans_type == trans_type;
}

uint32_t radiant_binding_find(uint16_t devnum, uint8_t devtype, uint8_t trans_type)
{
	uint32_t i;

	for (i = 0u; i < RADIANT_BINDING_MAX; i++) {
		if (same_key(&table[i], devnum, devtype, trans_type)) {
			return i;
		}
	}
	return RADIANT_BINDING_NONE;
}

static bool uuid_in_use(uint64_t uuid)
{
	uint32_t i;

	for (i = 0u; i < RADIANT_BINDING_MAX; i++) {
		if (table[i].used && table[i].uuid == uuid) {
			return true;
		}
	}
	return false;
}

/*
 * uuid is a label, not a key (struct radiant_binding::uuid), so a
 * monotonic counter folded with the cycle counter at bind time is enough -
 * no need for a hardware entropy source.
 *
 * Never 0 (the "no uuid" value readers treat as unbound) and never one
 * already in the table. The second matters since radiant_binding_restore():
 * a restored uuid came from an earlier boot's counter and cycle count, so
 * the same pair turning up again is unlikely rather than impossible.
 */
static uint32_t bind_sequence;

static uint64_t new_uuid(void)
{
	uint64_t uuid;

	do {
		uint32_t seq = ++bind_sequence;
		uint32_t salt = k_cycle_get_32();

		uuid = ((uint64_t)salt << 32) | (uint64_t)seq;
	} while (uuid == 0u || uuid_in_use(uuid));

	return uuid;
}

static void fill(struct radiant_binding *b, uint16_t devnum, uint8_t devtype,
		 uint8_t trans_type, uint64_t uuid, const char *label)
{
	memset(b, 0, sizeof(*b));
	b->used = true;
	b->devnum = devnum;
	b->devtype = devtype;
	b->trans_type = trans_type;
	b->uuid = uuid;
	if (label != NULL) {
		strncpy(b->label, label, sizeof(b->label) - 1u);
	}
}

int radiant_binding_bind(uint16_t devnum, uint8_t devtype, uint8_t trans_type,
		     const char *label, uint32_t *source_out)
{
	uint32_t existing;
	uint32_t i;

	existing = radiant_binding_find(devnum, devtype, trans_type);
	if (existing != RADIANT_BINDING_NONE) {
		if (source_out != NULL) {
			*source_out = existing;
		}
		return 0;
	}

	for (i = 0u; i < RADIANT_BINDING_MAX; i++) {
		if (!table[i].used) {
			fill(&table[i], devnum, devtype, trans_type, new_uuid(),
			     label);
			if (source_out != NULL) {
				*source_out = i;
			}
			return 0;
		}
	}

	return -ENOSPC;
}

int radiant_binding_restore(uint16_t devnum, uint8_t devtype, uint8_t trans_type,
			    uint64_t uuid, const char *label, uint16_t period,
			    uint32_t *source_out)
{
	uint32_t existing;
	uint32_t i;

	if (uuid == 0u) {
		return -EINVAL;
	}

	existing = radiant_binding_find(devnum, devtype, trans_type);
	if (existing != RADIANT_BINDING_NONE) {
		if (source_out != NULL) {
			*source_out = existing;
		}
		/* The same restore twice is not an error; the same channel ID
		 * under a different identity is. */
		return (table[existing].uuid == uuid) ? 0 : -EEXIST;
	}
	if (uuid_in_use(uuid)) {
		/* Two channel IDs under one identity would merge two sensors'
		 * topics and endpoints. Refuse rather than pick one. */
		return -EEXIST;
	}

	for (i = 0u; i < RADIANT_BINDING_MAX; i++) {
		if (!table[i].used) {
			fill(&table[i], devnum, devtype, trans_type, uuid, label);
			table[i].period = period;
			if (source_out != NULL) {
				*source_out = i;
			}
			return 0;
		}
	}

	return -ENOSPC;
}

int radiant_binding_set_period(uint32_t source, uint16_t period)
{
	if (source >= RADIANT_BINDING_MAX || !table[source].used) {
		return -ENOENT;
	}
	table[source].period = period;
	return 0;
}

const struct radiant_binding *radiant_binding_get(uint32_t source)
{
	if (source >= RADIANT_BINDING_MAX || !table[source].used) {
		return NULL;
	}
	return &table[source];
}

int radiant_binding_unbind(uint32_t source)
{
	if (source >= RADIANT_BINDING_MAX || !table[source].used) {
		return -ENOENT;
	}
	memset(&table[source], 0, sizeof(table[source]));
	return 0;
}
