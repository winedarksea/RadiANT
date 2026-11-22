/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_node_nvm_settings.c - radiant_node_nvm.h over Zephyr's settings subsystem.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md. The ADR specifies
 * the records but not a backend; this is the Zephyr-standard answer -
 * settings over NVS in storage_partition - rather than a private format.
 *
 * The settings subtree-walk read path is the wrong shape/cost for a lookup
 * radiant_node_ident.c makes before every public key. So the four records are read
 * once at init into a RAM cache, and radiant_node_nvm_load() answers from it. This
 * narrows what a load after store proves: it confirms the cache, not that the
 * flash sector reads back. The durability claim rests on settings_save_one()
 * returning 0 (committed, for the NVS backend); re-reading flash to verify
 * would cost a second write's worth of wear per pairing window.
 *
 * Wear: ADR 0009 promised writes once per boot, once per pairing window. The
 * epoch reservation (docs/radiant-security.md 3.5) adds one more per boot
 * (the next-free-epoch record) and one per counter wrap: about one write
 * every 4.5 h of continuous 4 Hz secured TX, ~5 a day. A strap used twice
 * daily for ten years, two hours a ride, is ~14 600 boot writes plus well
 * under one wrap write a day - NVS spreads them over the whole partition's
 * sectors, so per-page cycles stay inside Nordic flash's 10k endurance for
 * any partition of a few sectors or more. An argument, not a measurement; no
 * board here runs this backend yet.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include "radiant_node_nvm.h"

#define SUBTREE "radiant"

struct record {
	const char *key;
	void       *buf;
	size_t      len;
	bool        present;
	/* A record was found in flash but could not be loaded (wrong length,
	 * read error). NOT the same as absent: radiant_node_ident.c generates a new
	 * K_dev on "absent" (ADR 0020), so a record that fails to load must
	 * answer RADIANT_NODE_NVM_EIO, never RADIANT_NODE_NVM_ENOENT, or one bad read would
	 * re-roll the node's identity. */
	bool        bad;
};

static uint32_t cache_boot;
static uint32_t cache_pair;
static uint32_t cache_epoch;
static uint8_t  cache_kdev[16];

static struct record records[] = {
	{ RADIANT_NODE_NVM_KEY_BOOT, &cache_boot, sizeof(cache_boot), false, false },
	{ RADIANT_NODE_NVM_KEY_PAIR, &cache_pair, sizeof(cache_pair), false, false },
	{ RADIANT_NODE_NVM_KEY_KDEV, cache_kdev,  sizeof(cache_kdev), false, false },
	{ RADIANT_NODE_NVM_KEY_EPOCH, &cache_epoch, sizeof(cache_epoch), false, false },
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
	/* Zero length is how the settings subsystem records a deletion: the
	 * record is genuinely absent (a future factory reset deletes K_dev to
	 * get exactly this). */
	if (len == 0u) {
		r->present = false;
		r->bad = false;
		return 0;
	}
	/* Wrong-length record never partially loaded, and marked bad rather
	 * than left looking absent (see radiant_node_nvm.h and the `bad` field). */
	if (len != r->len) {
		r->present = false;
		r->bad = true;
		return -EINVAL;
	}
	got = read_cb(cb_arg, r->buf, r->len);
	if (got < 0 || (size_t)got != r->len) {
		r->present = false;
		r->bad = true;
		return -EIO;
	}
	r->present = true;
	r->bad = false;
	return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(radiant_node, SUBTREE, NULL, set_cb, NULL,
			       NULL);

int radiant_node_nvm_init(void)
{
	int rc;

	if (initialised) {
		return RADIANT_NODE_NVM_OK;
	}
	rc = settings_subsys_init();
	if (rc != 0) {
		return RADIANT_NODE_NVM_EIO;
	}
	rc = settings_load_subtree(SUBTREE);
	if (rc != 0) {
		return RADIANT_NODE_NVM_EIO;
	}
	initialised = true;
	return RADIANT_NODE_NVM_OK;
}

int radiant_node_nvm_load(const char *key, void *out, size_t len)
{
	const struct record *r;

	if (key == NULL || out == NULL) {
		return RADIANT_NODE_NVM_EINVAL;
	}
	if (!initialised) {
		return RADIANT_NODE_NVM_EIO;
	}
	r = find(key);
	if (r == NULL) {
		return RADIANT_NODE_NVM_EINVAL;
	}
	if (len != r->len) {
		return RADIANT_NODE_NVM_EIO;
	}
	if (r->bad) {
		return RADIANT_NODE_NVM_EIO;
	}
	if (!r->present) {
		return RADIANT_NODE_NVM_ENOENT;
	}
	memcpy(out, r->buf, len);
	return RADIANT_NODE_NVM_OK;
}

int radiant_node_nvm_store(const char *key, const void *val, size_t len)
{
	struct record *r;
	char           path[sizeof(SUBTREE) + 8];
	int            rc;

	if (key == NULL || val == NULL) {
		return RADIANT_NODE_NVM_EINVAL;
	}
	if (!initialised) {
		return RADIANT_NODE_NVM_EIO;
	}
	r = find(key);
	if (r == NULL) {
		return RADIANT_NODE_NVM_EINVAL;
	}
	if (len != r->len) {
		return RADIANT_NODE_NVM_EINVAL;
	}

	rc = snprintk(path, sizeof(path), SUBTREE "/%s", key);
	if (rc < 0 || (size_t)rc >= sizeof(path)) {
		return RADIANT_NODE_NVM_EINVAL;
	}

	/* Flash first, cache second - the other order would let a caller's
	 * post-store load (radiant_node_ident.c does this) confirm a value that never
	 * reached the part. */
	rc = settings_save_one(path, val, len);
	if (rc != 0) {
		return RADIANT_NODE_NVM_EIO;
	}
	memcpy(r->buf, val, len);
	r->present = true;
	r->bad = false;
	return RADIANT_NODE_NVM_OK;
}
