/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_binding.c - see radiant_binding.h.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>

#include "radiant_binding.h"

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

/*
 * A label, not a key - the header comment on struct radiant_binding::uuid
 * says so directly - so a hardware entropy source is the wrong tool: it
 * would make every product build of this file depend on
 * CONFIG_ENTROPY_GENERATOR and a bound driver for a value nothing
 * security-sensitive ever reads. Uniqueness within one image's lifetime is
 * all section 5 asks for ("stable across a re-roll and a re-pair", i.e.
 * stable for as long as the binding exists - not unguessable), so a
 * monotonic counter folded against the cycle counter at the moment of bind is
 * enough: two binds can never share a counter value, and the cycle count
 * makes a fresh image's sequence not trivially match a previous one's.
 */
static uint32_t bind_sequence;

static uint64_t new_uuid(void)
{
	uint32_t seq = ++bind_sequence;
	uint32_t salt = k_cycle_get_32();

	return ((uint64_t)salt << 32) | (uint64_t)seq;
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
			table[i].used = true;
			table[i].devnum = devnum;
			table[i].devtype = devtype;
			table[i].trans_type = trans_type;
			table[i].uuid = new_uuid();
			memset(table[i].label, 0, sizeof(table[i].label));
			if (label != NULL) {
				strncpy(table[i].label, label,
					sizeof(table[i].label) - 1u);
			}
			if (source_out != NULL) {
				*source_out = i;
			}
			return 0;
		}
	}

	return -ENOSPC;
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
