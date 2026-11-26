/* SPDX-License-Identifier: Apache-2.0 */
/*
 * binding_store.c - settings I/O for the binding records (binding_store.h).
 *
 * The cache below mirrors what is on flash, so a lookup at bind time reads no
 * flash, and a write happens only where binding_store.h says it does: a bind
 * or a user eviction. The boot restore runs on bridge_pump.c's thread and
 * everything else on the self-channel thread; the mutex covers the hand-over.
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/toolchain.h>

#include <radiant/radiant_binding.h>

#include "binding_store.h"

#if defined(CONFIG_ANT_DONGLE_BINDING_STORE)
#include <zephyr/settings/settings.h>
#endif

LOG_MODULE_REGISTER(binding_store, LOG_LEVEL_INF);

/* Given once, by binding_store_boot_restore(), whether or not anything was
 * restored, so the self-channel thread never waits on an empty store. */
static K_SEM_DEFINE(boot_done, 0, 1);

static uint32_t boot_sources[BINDING_STORE_SLOTS];
static size_t   boot_n;

__weak void binding_store_on_evict(uint64_t uuid)
{
	ARG_UNUSED(uuid);
}

size_t binding_store_boot_sources(uint32_t *sources, size_t max,
				  uint32_t timeout_ms)
{
	size_t n;

	if (k_sem_take(&boot_done, K_MSEC(timeout_ms)) != 0) {
		LOG_WRN("binding restore had not run after %u ms: no sensor is "
			"re-pinned at this boot", (unsigned int)timeout_ms);
		return 0u;
	}
	/* Put it back: this is a "has happened" flag, not a token. */
	k_sem_give(&boot_done);

	n = MIN(boot_n, max);
	if (sources != NULL) {
		memcpy(sources, boot_sources, n * sizeof(sources[0]));
	}
	return n;
}

#if defined(CONFIG_ANT_DONGLE_BINDING_STORE)

#define SUBTREE "radiant/bind"

struct slot {
	bool                  valid;
	struct binding_record rec;
};

static struct slot cache[BINDING_STORE_SLOTS];
static uint32_t    next_seq = 1u;
static K_MUTEX_DEFINE(cache_lock);

static bool same_key(const struct binding_record *r, uint16_t devnum,
		     uint8_t devtype, uint8_t trans)
{
	return r->devnum == devnum && r->devtype == devtype && r->trans == trans;
}

static int find_slot(uint16_t devnum, uint8_t devtype, uint8_t trans)
{
	for (int i = 0; i < (int)BINDING_STORE_SLOTS; i++) {
		if (cache[i].valid &&
		    same_key(&cache[i].rec, devnum, devtype, trans)) {
			return i;
		}
	}
	return -1;
}

static void slot_key(int i, char *out, size_t len)
{
	(void)snprintf(out, len, SUBTREE "/%d", i);
}

static int load_one(const char *key, size_t len, settings_read_cb read_cb,
		    void *cb_arg, void *param)
{
	uint8_t buf[BINDING_RECORD_LEN];
	struct binding_record rec;
	const char *next = NULL;
	unsigned long n;
	char *end = NULL;
	ssize_t got;
	int dup;
	int rc;

	ARG_UNUSED(param);

	/* Only "<n>" directly under the subtree; anything deeper or
	 * non-numeric is not ours to interpret. */
	if (settings_name_next(key, &next) == 0 || next != NULL) {
		return 0;
	}
	n = strtoul(key, &end, 10);
	if (end == key || *end != '\0' || n >= BINDING_STORE_SLOTS) {
		LOG_WRN("ignoring stored binding key %s/%s", SUBTREE, key);
		return 0;
	}
	if (len != BINDING_RECORD_LEN) {
		LOG_WRN("stored binding %lu: %u bytes, not %u - ignored", n,
			(unsigned int)len, BINDING_RECORD_LEN);
		return 0;
	}
	got = read_cb(cb_arg, buf, sizeof(buf));
	if (got != (ssize_t)sizeof(buf)) {
		LOG_WRN("stored binding %lu: read %d - ignored", n, (int)got);
		return 0;
	}
	rc = binding_record_decode(buf, sizeof(buf), &rec);
	if (rc != 0) {
		LOG_WRN("stored binding %lu: undecodable (%d) - ignored", n, rc);
		return 0;
	}

	/* The same channel ID in two slots should not happen; if it does,
	 * the later pairing wins and the other slot is reused by the next
	 * save. */
	dup = find_slot(rec.devnum, rec.devtype, rec.trans);
	if (dup >= 0) {
		if (cache[dup].rec.seq >= rec.seq) {
			return 0;
		}
		cache[dup].valid = false;
	}

	cache[n].valid = true;
	cache[n].rec = rec;
	if (rec.seq >= next_seq) {
		next_seq = rec.seq + 1u;
	}
	return 0;
}

static void load_all(void)
{
	int rc;

	memset(cache, 0, sizeof(cache));
	next_seq = 1u;

	rc = settings_subsys_init();
	if (rc != 0) {
		LOG_ERR("settings init failed (%d): paired sensors will not "
			"survive a reboot", rc);
		return;
	}
	rc = settings_load_subtree_direct(SUBTREE, load_one, NULL);
	if (rc != 0) {
		LOG_ERR("loading stored bindings failed (%d)", rc);
	}
}

size_t binding_store_boot_restore(size_t max)
{
	int order[BINDING_STORE_SLOTS];
	size_t picked = 0u;

	k_mutex_lock(&cache_lock, K_FOREVER);
	load_all();

	if (max > BINDING_STORE_SLOTS) {
		max = BINDING_STORE_SLOTS;
	}

	/* The `max` newest, by selection: eight slots, once per boot. */
	while (picked < max) {
		int best = -1;

		for (int i = 0; i < (int)BINDING_STORE_SLOTS; i++) {
			bool taken = false;

			if (!cache[i].valid) {
				continue;
			}
			for (size_t k = 0u; k < picked; k++) {
				taken = taken || order[k] == i;
			}
			if (!taken && (best < 0 || cache[i].rec.seq > cache[best].rec.seq)) {
				best = i;
			}
		}
		if (best < 0) {
			break;
		}
		order[picked++] = best;
	}

	/* Restored oldest first, so the self channels hand out bind order in
	 * the order the sensors were paired. */
	boot_n = 0u;
	for (size_t k = picked; k > 0u; k--) {
		const struct binding_record *r = &cache[order[k - 1u]].rec;
		uint32_t source = RADIANT_BINDING_NONE;
		int rc = radiant_binding_restore(r->devnum, r->devtype, r->trans,
						 r->uuid, r->label, r->period,
						 &source);

		if (rc != 0) {
			LOG_WRN("stored binding for device %u (type 0x%02x) not "
				"restored: %d", r->devnum, r->devtype, rc);
			continue;
		}
		boot_sources[boot_n++] = source;
		LOG_INF("restored binding: device %u (type 0x%02x, trans %u) as "
			"source %u", r->devnum, r->devtype, r->trans,
			(unsigned int)source);
	}
	k_mutex_unlock(&cache_lock);

	k_sem_give(&boot_done);
	return boot_n;
}

bool binding_store_lookup(uint16_t devnum, uint8_t devtype, uint8_t trans,
			  struct binding_record *out)
{
	int i;

	k_mutex_lock(&cache_lock, K_FOREVER);
	i = find_slot(devnum, devtype, trans);
	if (i >= 0 && out != NULL) {
		*out = cache[i].rec;
	}
	k_mutex_unlock(&cache_lock);
	return i >= 0;
}

static int pick_save_slot(const struct radiant_binding *b)
{
	int oldest_dormant = -1;
	int oldest = -1;
	int i = find_slot(b->devnum, b->devtype, b->trans_type);

	if (i >= 0) {
		return i;
	}
	for (i = 0; i < (int)BINDING_STORE_SLOTS; i++) {
		if (!cache[i].valid) {
			return i;
		}
	}
	for (i = 0; i < (int)BINDING_STORE_SLOTS; i++) {
		const struct binding_record *r = &cache[i].rec;

		if (oldest < 0 || r->seq < cache[oldest].rec.seq) {
			oldest = i;
		}
		if (radiant_binding_find(r->devnum, r->devtype, r->trans) ==
			    RADIANT_BINDING_NONE &&
		    (oldest_dormant < 0 || r->seq < cache[oldest_dormant].rec.seq)) {
			oldest_dormant = i;
		}
	}
	return (oldest_dormant >= 0) ? oldest_dormant : oldest;
}

int binding_store_save(const struct radiant_binding *b)
{
	uint8_t buf[BINDING_RECORD_LEN];
	char key[sizeof(SUBTREE "/255")];
	struct binding_record rec;
	int slot;
	int rc;

	if (b == NULL) {
		return -EINVAL;
	}

	memset(&rec, 0, sizeof(rec));
	rec.devnum = b->devnum;
	rec.devtype = b->devtype;
	rec.trans = b->trans_type;
	rec.uuid = b->uuid;
	(void)strncpy(rec.label, b->label, sizeof(rec.label) - 1u);
	rec.period = b->period;

	k_mutex_lock(&cache_lock, K_FOREVER);
	rec.seq = next_seq;
	if (binding_record_encode(&rec, buf, sizeof(buf)) != sizeof(buf)) {
		k_mutex_unlock(&cache_lock);
		return -EINVAL;
	}
	slot = pick_save_slot(b);
	slot_key(slot, key, sizeof(key));
	if (cache[slot].valid &&
	    !same_key(&cache[slot].rec, rec.devnum, rec.devtype, rec.trans)) {
		LOG_INF("binding store full: forgetting device %u (type 0x%02x), "
			"the oldest pairing not bound now",
			cache[slot].rec.devnum, cache[slot].rec.devtype);
	}
	rc = settings_save_one(key, buf, sizeof(buf));
	if (rc == 0) {
		cache[slot].valid = true;
		cache[slot].rec = rec;
		next_seq++;
	} else {
		LOG_ERR("saving binding for device %u failed (%d): it will not "
			"survive a reboot", rec.devnum, rc);
	}
	k_mutex_unlock(&cache_lock);
	return rc;
}

void binding_store_evict(const struct radiant_binding *b)
{
	char key[sizeof(SUBTREE "/255")];
	uint64_t uuid;
	int slot;

	if (b == NULL) {
		return;
	}
	uuid = b->uuid;

	k_mutex_lock(&cache_lock, K_FOREVER);
	slot = find_slot(b->devnum, b->devtype, b->trans_type);
	if (slot >= 0) {
		int rc;

		slot_key(slot, key, sizeof(key));
		rc = settings_delete(key);
		if (rc != 0) {
			/* Still dropped from the cache, so this boot does not
			 * reuse its uuid; a reboot may restore it. */
			LOG_ERR("deleting binding for device %u failed (%d): it "
				"may come back at the next boot", b->devnum, rc);
		}
		cache[slot].valid = false;
	}
	k_mutex_unlock(&cache_lock);

	binding_store_on_evict(uuid);
}

#else /* !CONFIG_ANT_DONGLE_BINDING_STORE */

size_t binding_store_boot_restore(size_t max)
{
	ARG_UNUSED(max);
	boot_n = 0u;
	k_sem_give(&boot_done);
	return 0u;
}

bool binding_store_lookup(uint16_t devnum, uint8_t devtype, uint8_t trans,
			  struct binding_record *out)
{
	ARG_UNUSED(devnum);
	ARG_UNUSED(devtype);
	ARG_UNUSED(trans);
	ARG_UNUSED(out);
	return false;
}

int binding_store_save(const struct radiant_binding *b)
{
	ARG_UNUSED(b);
	return 0;
}

void binding_store_evict(const struct radiant_binding *b)
{
	if (b != NULL) {
		binding_store_on_evict(b->uuid);
	}
}

#endif /* CONFIG_ANT_DONGLE_BINDING_STORE */
