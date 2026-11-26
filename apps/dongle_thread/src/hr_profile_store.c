/* SPDX-License-Identifier: Apache-2.0 */
/*
 * hr_profile_store.c - Zephyr settings behind radiant_hr_profile's storage
 * seam (docs/radiant-bridge.md section 6.5). The library holds the
 * histograms and decides WHEN to write; this file only knows WHERE.
 *
 * Key: "radiant/hrp/<uuid as 16 hex digits>". Value: struct
 * radiant_hr_profile_record, version 1, 78 bytes, stored as raw bytes (same
 * device writes and reads it; no cross-endian transport).
 *
 * Write budget: the library calls save() only at session end, only after 5
 * minutes of new wear, at most RADIANT_HR_PROFILE_SAVES_PER_BOOT times per
 * slot per boot. erase() comes from user eviction (the button). Nothing here
 * is traffic-driven.
 */

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>

#include <radiant/radiant_hr_profile.h>

#include "hr_profile_store.h"

LOG_MODULE_REGISTER(hr_profile_store, LOG_LEVEL_INF);

#define KEY_PREFIX "radiant/hrp/"
#define KEY_LEN    (sizeof(KEY_PREFIX) - 1u + 16u + 1u)

static void make_key(char key[KEY_LEN], uint64_t uuid)
{
	(void)snprintf(key, KEY_LEN, KEY_PREFIX "%08x%08x",
		       (unsigned int)(uuid >> 32), (unsigned int)(uuid & 0xFFFFFFFFu));
}

struct load_ctx {
	struct radiant_hr_profile_record *rec;
	int                               rc;
};

static int load_cb(const char *key, size_t len, settings_read_cb read_cb,
		   void *cb_arg, void *param)
{
	struct load_ctx *ctx = param;
	const char      *next;
	ssize_t          got;

	/* Exactly the key asked for, not something beneath it. */
	if (settings_name_next(key, &next) != 0) {
		return 0;
	}
	if (len != sizeof(*ctx->rec)) {
		LOG_WRN("hrp: stored record is %u bytes, want %u; ignored",
			(unsigned int)len, (unsigned int)sizeof(*ctx->rec));
		ctx->rc = -EINVAL;
		return 0;
	}
	got = read_cb(cb_arg, ctx->rec, sizeof(*ctx->rec));
	ctx->rc = (got == (ssize_t)sizeof(*ctx->rec)) ? 0 : -EIO;
	return 0;
}

static int store_load(uint64_t uuid, struct radiant_hr_profile_record *rec)
{
	char            key[KEY_LEN];
	struct load_ctx ctx = { .rec = rec, .rc = -ENOENT };
	int             rc;

	make_key(key, uuid);
	rc = settings_load_subtree_direct(key, load_cb, &ctx);
	if (rc != 0) {
		return rc;
	}
	if (ctx.rc == 0 && rec->version != RADIANT_HR_PROFILE_RECORD_VERSION) {
		LOG_WRN("hrp: %s has version %u; ignored", key, rec->version);
		return -EINVAL;
	}
	return ctx.rc;
}

static int store_save(uint64_t uuid, const struct radiant_hr_profile_record *rec)
{
	char key[KEY_LEN];
	int  rc;

	make_key(key, uuid);
	rc = settings_save_one(key, rec, sizeof(*rec));
	if (rc != 0) {
		LOG_WRN("hrp: save %s failed (%d)", key, rc);
	} else {
		LOG_INF("hrp: saved %s", key);
	}
	return rc;
}

static int store_erase(uint64_t uuid)
{
	char key[KEY_LEN];

	make_key(key, uuid);
	return settings_delete(key);
}

static const struct radiant_hr_profile_store ops = {
	.load = store_load,
	.save = store_save,
	.erase = store_erase,
};

void hr_profile_store_init(void)
{
	radiant_hr_profile_set_store(&ops);
}
