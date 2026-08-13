/* SPDX-License-Identifier: Apache-2.0 */
/*
 * node_nvm_settings.c - node_nvm.h over Zephyr's settings subsystem.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md. The ADR specifies
 * the records but not a backend; this is the Zephyr-standard answer -
 * settings over NVS in storage_partition - rather than a private format.
 *
 * The settings subtree-walk read path is the wrong shape/cost for a lookup
 * node_ident.c makes before every public key. So the three records are read
 * once at init into a RAM cache, and node_nvm_load() answers from it. This
 * narrows what a load after store proves: it confirms the cache, not that the
 * flash sector reads back. The durability claim rests on settings_save_one()
 * returning 0 (committed, for the NVS backend); re-reading flash to verify
 * would cost a second write's worth of wear per pairing window.
 *
 * Wear: ADR 0009 promises writes once per boot, once per pairing window. A
 * strap used twice daily for ten years is ~7000 boot-counter writes, well
 * inside Nordic flash's 10k-cycle/page endurance with NVS levelling - this is
 * an argument, not a measurement; no board here runs this backend yet.
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
	/* Wrong-length record left absent rather than partially loaded
	 * (see node_nvm.h). */
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

	/* Flash first, cache second - the other order would let a caller's
	 * post-store load (node_ident.c does this) confirm a value that never
	 * reached the part. */
	rc = settings_save_one(path, val, len);
	if (rc != 0) {
		return NODE_NVM_EIO;
	}
	memcpy(r->buf, val, len);
	r->present = true;
	return NODE_NVM_OK;
}
