/* SPDX-License-Identifier: Apache-2.0 */
/*
 * node_nvm_settings.c - node_nvm.h over Zephyr's settings subsystem.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md. The ADR specifies
 * two u32 counters and one key in NVM and deliberately does not specify a
 * storage backend; this is the standard Zephyr answer - the settings subsystem
 * over NVS in the board's storage_partition - chosen because this repository
 * had no persistence layer at all before this phase and inventing a private one
 * to hold three records would be the worse of the two mistakes available.
 *
 * ── The cache, and why loads do not touch flash ────────────────────────────
 *
 * The settings subsystem's read path is a subtree walk with a callback, which
 * is the wrong shape for "give me the boot counter" and the wrong cost for a
 * path that node_ident.c takes before every public key it emits. So the three
 * records are read once at init into a RAM cache, and node_nvm_load() answers
 * from it.
 *
 * That is a real narrowing of what node_nvm_load() proves, and it is stated
 * here rather than discovered: after a successful store, a load confirms that
 * this image agrees with itself about the value, not that the flash sector
 * reads back. The durability claim rests on settings_save_one() returning 0,
 * which for the NVS backend means the record is committed. Re-reading flash to
 * check would be a second write's worth of wear per pairing window on a device
 * whose entire storage argument is write frequency.
 *
 * ── Wear ───────────────────────────────────────────────────────────────────
 *
 * ADR 0009's table promises "once per boot, once per pairing window", and NVS
 * over a two-sector partition wear-levels within the sectors it is given. A
 * strap worn twice a day for ten years is on the order of 7 000 boot-counter
 * writes; at 4 bytes plus a name in an NVS sector pair that is comfortably
 * inside any Nordic flash's 10 000-cycle-per-page endurance once NVS's own
 * levelling is counted, because 7 000 records do not land on 7 000 erases.
 * THAT IS AN ARGUMENT, NOT A MEASUREMENT. The ADR asks for the number to be
 * measured in the implementation phase and this phase did not measure it: no
 * board in this project runs the settings backend yet.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "node_nvm.h"

#define SUBTREE "radiant"

struct record {
	const char *key;
	void       *buf;
	size_t      len;
	bool        present;
};

static uint32_t cache_boot;
static uint32_t cache_pair;
static uint8_t  cache_kdev[16];

static struct record records[] = {
	{ NODE_NVM_KEY_BOOT, &cache_boot, sizeof(cache_boot), false },
	{ NODE_NVM_KEY_PAIR, &cache_pair, sizeof(cache_pair), false },
	{ NODE_NVM_KEY_KDEV, cache_kdev,  sizeof(cache_kdev), false },
};

static bool initialised;

static struct record *find(const char *key)
{
	for (size_t i = 0u; i < ARRAY_SIZE(records); i++) {
		if (strcmp(records[i].key, key) == 0) {
			return &records[i];
		}
	}
	return NULL;
}

static int set_cb(const char *name, size_t len, settings_read_cb read_cb,
		  void *cb_arg)
{
	const char   *next;
	struct record *r;
	ssize_t        got;

	if (settings_name_steq(name, "", &next) && next == NULL) {
		return -ENOENT;
	}
	r = find(name);
	if (r == NULL) {
		return -ENOENT;
	}
	/*
	 * A stored record of the wrong length is left ABSENT rather than
	 * partially loaded. See node_nvm.h: the only way this happens is a
	 * firmware update that redefined a record, and half an old boot counter
	 * is a boot counter that went backwards.
	 */
	if (len != r->len) {
		return -EINVAL;
	}
	got = read_cb(cb_arg, r->buf, r->len);
	if (got < 0 || (size_t)got != r->len) {
		return -EIO;
	}
	r->present = true;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(radiant_node, SUBTREE, NULL, set_cb, NULL,
			       NULL);

int node_nvm_init(void)
{
	int rc;

	if (initialised) {
		return NODE_NVM_OK;
	}
	rc = settings_subsys_init();
	if (rc != 0) {
		return NODE_NVM_EIO;
	}
	rc = settings_load_subtree(SUBTREE);
	if (rc != 0) {
		return NODE_NVM_EIO;
	}
	initialised = true;
	return NODE_NVM_OK;
}

int node_nvm_load(const char *key, void *out, size_t len)
{
	const struct record *r;

	if (key == NULL || out == NULL) {
		return NODE_NVM_EINVAL;
	}
	if (!initialised) {
		return NODE_NVM_EIO;
	}
	r = find(key);
	if (r == NULL) {
		return NODE_NVM_EINVAL;
	}
	if (len != r->len) {
		return NODE_NVM_EIO;
	}
	if (!r->present) {
		return NODE_NVM_ENOENT;
	}
	memcpy(out, r->buf, len);
	return NODE_NVM_OK;
}

int node_nvm_store(const char *key, const void *val, size_t len)
{
	struct record *r;
	char           path[sizeof(SUBTREE) + 8];
	int            rc;

	if (key == NULL || val == NULL) {
		return NODE_NVM_EINVAL;
	}
	if (!initialised) {
		return NODE_NVM_EIO;
	}
	r = find(key);
	if (r == NULL) {
		return NODE_NVM_EINVAL;
	}
	if (len != r->len) {
		return NODE_NVM_EINVAL;
	}

	rc = snprintk(path, sizeof(path), SUBTREE "/%s", key);
	if (rc < 0 || (size_t)rc >= sizeof(path)) {
		return NODE_NVM_EINVAL;
	}

	/*
	 * Flash first, cache second. The other order would leave a caller that
	 * checks by loading - which node_ident.c does before it lets a public
	 * key out - agreeing with a value that never reached the part.
	 */
	rc = settings_save_one(path, val, len);
	if (rc != 0) {
		return NODE_NVM_EIO;
	}
	memcpy(r->buf, val, len);
	r->present = true;
	return NODE_NVM_OK;
}
