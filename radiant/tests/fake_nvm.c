/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fake_nvm.c - src/node/node_nvm.h against RAM, with a reboot you can call.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md and
 * src/node/node_nvm.h.
 *
 * See fake_nvm.h for why this exists: the properties this phase must assert
 * (a counter that survives a power cycle, a write that fails) belong to the
 * seam, not to NVS, so the seam gets an implementation that can do both on
 * demand - as fake_radio.c is to the radio HAL.
 *
 * No allocation, no threads, fixed records. The record set is closed because
 * node_nvm.h's is: three names.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "fake_nvm.h"
#include "node_nvm.h"

#define REC_MAX_LEN 16

struct rec {
	const char *key;
	uint8_t     buf[REC_MAX_LEN];
	size_t      len;      /* the length actually stored, 0 when absent */
};

static struct rec parts[] = {
	{ NODE_NVM_KEY_BOOT, { 0 }, 0u },
	{ NODE_NVM_KEY_PAIR, { 0 }, 0u },
	{ NODE_NVM_KEY_KDEV, { 0 }, 0u },
};

static bool     initialised;
static uint32_t fail_remaining;
static bool     swallow;
static uint32_t stores;

static struct rec *find(const char *key)
{
	for (size_t i = 0u; i < sizeof(parts) / sizeof(parts[0]); i++) {
		if (strcmp(parts[i].key, key) == 0) {
			return &parts[i];
		}
	}
	return NULL;
}

/* ── The control surface ──────────────────────────────────────────────────── */

void fake_nvm_wipe(void)
{
	for (size_t i = 0u; i < sizeof(parts) / sizeof(parts[0]); i++) {
		memset(parts[i].buf, 0, sizeof(parts[i].buf));
		parts[i].len = 0u;
	}
	initialised = false;
	fail_remaining = 0u;
	swallow = false;
	stores = 0u;
}

void fake_nvm_reboot(void)
{
	/* Contents survive - the whole point - so this is not a memset. */
	initialised = false;
}

void fake_nvm_fail_stores(uint32_t n)
{
	fail_remaining = n;
}

void fake_nvm_swallow_stores(bool on)
{
	swallow = on;
}

bool fake_nvm_peek_u32(const char *key, uint32_t *out)
{
	const struct rec *r = find(key);

	if (r == NULL || r->len != sizeof(uint32_t) || out == NULL) {
		return false;
	}
	memcpy(out, r->buf, sizeof(uint32_t));
	return true;
}

uint32_t fake_nvm_store_count(void)
{
	return stores;
}

/* ── node_nvm.h ───────────────────────────────────────────────────────────── */

int node_nvm_init(void)
{
	initialised = true;
	return NODE_NVM_OK;
}

int node_nvm_load(const char *key, void *out, size_t len)
{
	const struct rec *r;

	if (key == NULL || out == NULL || len == 0u || len > REC_MAX_LEN) {
		return NODE_NVM_EINVAL;
	}
	if (!initialised) {
		return NODE_NVM_EIO;
	}
	r = find(key);
	if (r == NULL) {
		return NODE_NVM_EINVAL;
	}
	if (r->len == 0u) {
		return NODE_NVM_ENOENT;
	}
	if (r->len != len) {
		return NODE_NVM_EIO;
	}
	memcpy(out, r->buf, len);
	return NODE_NVM_OK;
}

int node_nvm_store(const char *key, const void *val, size_t len)
{
	struct rec *r;

	if (key == NULL || val == NULL || len == 0u || len > REC_MAX_LEN) {
		return NODE_NVM_EINVAL;
	}
	if (!initialised) {
		return NODE_NVM_EIO;
	}
	r = find(key);
	if (r == NULL) {
		return NODE_NVM_EINVAL;
	}

	stores++;

	if (fail_remaining > 0u) {
		fail_remaining--;
		return NODE_NVM_EIO;
	}
	if (swallow) {
		/* Reported as durable, never written; node_ident.c's re-read
		 * has to catch this. */
		return NODE_NVM_OK;
	}

	memcpy(r->buf, val, len);
	r->len = len;
	return NODE_NVM_OK;
}
