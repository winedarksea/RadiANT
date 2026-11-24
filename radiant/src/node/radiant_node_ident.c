/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_node_ident.c - the hostless node's identity, epoch and pairing scalar.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md. See radiant_node_ident.h
 * for the design and the pairing ordering rule.
 *
 * Calls radiant; radiant does not know this file exists
 * (application -> module, per docs/decisions/0002-clean-room-policy.md).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>   /* __weak, for the clock hook a node overrides */
#include <zephyr/logging/log.h>

#include <radiant/radiant_frame.h>
#include <radiant/radiant_sec.h>
#include <radiant/radiant_sec_compat.h>

#include <radiant/radiant_node_ident.h>
#include <radiant/radiant_node_nvm.h>

/* The level symbol exists only where src/node/Kconfig is sourced with
 * RADIANT_NODE_IDENT on; every caller that compiles this file does that, and
 * the fallback keeps the file compilable in an image that lists it bare. */
#if defined(CONFIG_RADIANT_NODE_IDENT_LOG_LEVEL)
LOG_MODULE_REGISTER(radiant_node_ident, CONFIG_RADIANT_NODE_IDENT_LOG_LEVEL);
#else
LOG_MODULE_REGISTER(radiant_node_ident, LOG_LEVEL_INF);
#endif

/* Cache of NVM-backed state; `booted` disambiguates "not booted" from
 * "booted to zero". Storage is the authority, not this struct. */
static struct {
	bool     booted;
	uint32_t boot_counter;
	uint32_t epoch;
	/* N, the next epoch nobody has used: what RADIANT_NODE_NVM_KEY_EPOCH holds, as
	 * confirmed by the last write. Every channel this node arms carries a
	 * ceiling of epoch_next - 1, so no epoch is used before N above it has
	 * reached storage (docs/radiant-security.md 3.5). */
	uint32_t epoch_next;
	/* The channels radiant_node_ident_arm_sec()/_arm_compat() armed: the only ones
	 * whose epoch advances this file follows. Bit per channel number. */
	uint32_t sec_armed;
	uint32_t compat_armed;
	/* E goes to the first arm of each kind; every later arm in the boot
	 * gets a fresh epoch, `arm_fresh`, once N above it is in storage
	 * (0 = none requested). */
	bool     sec_first_done;
	bool     compat_first_done;
	uint32_t arm_fresh;
	uint16_t devnum;
	uint8_t  k_dev[RADIANT_NODE_IDENT_K_DEV_BYTES];
	bool     have_k_dev;

	/* The pairing window. `scalar_ctr` is the counter value the live
	 * scalar was derived from, and it is what radiant_node_ident_pair_scalar()
	 * checks NVM against. */
	bool     window_open;
	uint32_t pair_counter;
	uint32_t scalar_ctr;
} s;

/*
 * radiant_sec_set_epoch() refuses an epoch within RADIANT_SEC_EPOCH_HEADROOM of
 * 0xFFFFFFFF. The node is now the epoch authority, so it must refuse rather
 * than wrap a boot counter here - a wrapped counter is a silently replayed
 * epoch. Hitting this means ~4 billion boots; the fix is re-provisioning
 * (new K_dev, new identity). If that is interrupted after the key write,
 * retrying with the same new key keeps the old counters and so cannot clear
 * the exhausted boot counter: use a different key.
 */
#define BOOT_COUNTER_MAX (0xFFFFFFFFu - RADIANT_SEC_EPOCH_HEADROOM - 1u)

/* The highest epoch a boot may take: it reserves the two above it (E + 1 for
 * the first wrap, E + 2 for the next boot), and both must stay under the limit
 * radiant_sec_set_epoch() accepts. */
#define EPOCH_BOOT_MAX (BOOT_COUNTER_MAX - 2u)

/*
 * The reservation's moving half. radiant_sec_epoch_advanced() (and the compat
 * twin) run in the pump, mid-transform, so they only record the epoch and
 * submit this work item; the item persists N = e + 2 and then raises every
 * armed channel's ceiling to N - 1. Until it lands, a second wrap is withheld,
 * which gives the write one whole wrap (~4.5 h at 4 Hz) to happen. A write
 * that fails is retried; it never lowers N and never raises a ceiling first.
 *
 * Outside `s` because radiant_node_ident_reset() memsets `s` and must not memset a
 * work item the queue may be holding.
 */
/* A failed persist retries with exponential backoff, 1 s doubling to 30 s,
 * and logs once per failure run rather than once per try. */
#define EPOCH_RETRY_MIN_MS 1000u
#define EPOCH_RETRY_MAX_MS 30000u

static struct k_spinlock ep_lock;
static uint32_t          ep_pending;     /* highest epoch reported, under ep_lock */
static bool              ep_have;        /* ep_pending is not yet persisted */
static uint32_t          ep_retry_ms;    /* 0 = the last persist succeeded */

static void epoch_work_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(ep_work, epoch_work_fn);

static bool key_is_all_zero(const uint8_t *k, size_t len)
{
	uint8_t acc = 0u;

	for (size_t i = 0u; i < len; i++) {
		acc |= k[i];
	}
	return acc == 0u;
}

static int load_u32(const char *key, uint32_t *out, bool *present)
{
	int rc = radiant_node_nvm_load(key, out, sizeof(*out));

	if (rc == RADIANT_NODE_NVM_ENOENT) {
		*out = 0u;
		*present = false;
		return RADIANT_NODE_IDENT_OK;
	}
	if (rc != RADIANT_NODE_NVM_OK) {
		return RADIANT_NODE_IDENT_EIO;
	}
	*present = true;
	return RADIANT_NODE_IDENT_OK;
}

/* Store then re-read to confirm. Catches a backend that returns OK but can't
 * actually produce the value, at the write rather than a power cycle later. */
static int store_u32_confirmed(const char *key, uint32_t val)
{
	uint32_t back = 0u;

	if (radiant_node_nvm_store(key, &val, sizeof(val)) != RADIANT_NODE_NVM_OK) {
		return RADIANT_NODE_IDENT_EIO;
	}
	if (radiant_node_nvm_load(key, &back, sizeof(back)) != RADIANT_NODE_NVM_OK) {
		return RADIANT_NODE_IDENT_EIO;
	}
	if (back != val) {
		return RADIANT_NODE_IDENT_EIO;
	}
	return RADIANT_NODE_IDENT_OK;
}

/* ── The record table (security review 2026-11-22) ───────────────────────────
 *
 * A record that reads as ABSENT is not always one that was never written.
 * Zephyr's NVS settings loader (settings_nvs.c, settings_nvs_load()) deletes a
 * setting whose name or value entry fails to read and never reports it, so a
 * flash fault looks exactly like "never written". This code used to read any
 * absent counter as 0: losing N alone sent the next boot back to the boot
 * counter, about half of N - an epoch already used, with the slot counter at
 * 0, which is keystream reuse under X_CONF - and losing K_dev alone made a
 * self-provisioning node draw a new identity, which ADR 0020 excludes.
 *
 * What makes the faults visible is the order every writer creates records in:
 * K_dev, then the boot counter, then the pairing counter, then N, then the
 * format marker F (provisioning, this image's or an older one's; boot creates
 * B before P and N, and F last). Nothing deletes a record except
 * radiant_node_ident_wipe(). So a healthy store holds a prefix of that order,
 * give or take records an older image never wrote, and F, once written, says
 * every record is there. One lost record then shows, except F itself, whose
 * loss alone is harmless (N is still there).
 *
 * K = kdev, F = fmt, B = boot, P = pair, N = epn; 1 present, 0 absent, - any.
 * A record that fails to READ (RADIANT_NODE_NVM_EIO) is EIO before this table,
 * and so is an F holding a value other than FMT_V1 (a later format).
 *
 *   K F B P N  what it is                                  the boot
 *   0 0 0 0 0  a factory-fresh part                        first boot (ADR 0020), or ENOKEY
 *   0 any other  K_dev lost from under its counters        EIO, writes nothing
 *   1 1 1 1 1  this image's store                          boots
 *   1 1 any 0  a record lost after it was written          EIO, writes nothing
 *   1 0 0 0 0  provisioning cut after the key              boots: B 0 -> 1, creates P, N, F
 *   1 0 1 0 0  provisioning cut after B                    boots, creates P, N, F
 *   1 0 1 0 1  the same, then booted by an older image     boots, creates P, F
 *   1 0 1 1 0  an older image's store (it had no N)        migrates: B is the floor; N, F
 *   1 0 1 1 1  cut before the marker, or F lost alone      boots, writes F
 *   1 0 0 - -  P or N without B: B was lost                EIO, writes nothing
 *
 * The unmarked rows stay accepted because the stores they describe exist: an
 * older image never wrote F (nor, before 2026-11-13, N). A fault there that
 * takes F as well as another record is a double fault, and still reads as one
 * of those rows; the marker makes every single loss refuse. radiant_node_ident_
 * provision() with the installed key refuses the same rows, and a boot that
 * accepts an unmarked store completes it (P, N, then F) before it returns.
 *
 * The way out of an EIO row is radiant_node_ident_wipe() and a reboot (a new
 * identity), or, where K_dev is the record lost, radiant_node_ident_provision()
 * with a new key. test_node_ident.c's record_table[] restates this table row by
 * row.
 */

/* The format marker's only value in this image. */
#define FMT_V1 1u

/* Indices into struct records, in prov_keys[] order. */
enum { REC_BOOT, REC_PAIR, REC_NEXT, REC_COUNT };

static const char *const prov_keys[REC_COUNT] = {
	RADIANT_NODE_NVM_KEY_BOOT, RADIANT_NODE_NVM_KEY_PAIR, RADIANT_NODE_NVM_KEY_EPOCH,
};

struct records {
	uint32_t val[REC_COUNT];  /* 0 when absent */
	bool     has[REC_COUNT];
	bool     has_fmt;
};

/* Load the records beside K_dev. EIO on any read failure, or on a marker
 * this image does not know. */
static int records_load(struct records *r)
{
	uint32_t fmt;
	int      rc;

	for (size_t i = 0u; i < REC_COUNT; i++) {
		rc = load_u32(prov_keys[i], &r->val[i], &r->has[i]);
		if (rc != RADIANT_NODE_IDENT_OK) {
			return rc;
		}
	}
	rc = load_u32(RADIANT_NODE_NVM_KEY_FMT, &fmt, &r->has_fmt);
	if (rc == RADIANT_NODE_IDENT_OK && r->has_fmt && fmt != FMT_V1) {
		rc = RADIANT_NODE_IDENT_EIO;
	}
	return rc;
}

/* The table above: false for every row that only a lost record explains. */
static bool records_consistent(bool have_kdev, const struct records *r)
{
	const bool counters = r->has[REC_BOOT] || r->has[REC_PAIR] ||
			      r->has[REC_NEXT];

	if (!have_kdev) {
		return !counters && !r->has_fmt;
	}
	if (r->has_fmt) {
		return r->has[REC_BOOT] && r->has[REC_PAIR] && r->has[REC_NEXT];
	}
	return r->has[REC_BOOT] || !counters;
}

/* ── Provisioning (first boot, ADR 0020, or a factory step) ──────────────── */

/* Load the stored K_dev and compare it with `k` in constant time. `*eq` is
 * false whenever the load is not RADIANT_NODE_NVM_OK; the load's code is returned. */
static int kdev_load_eq(const uint8_t *k, bool *eq)
{
	uint8_t back[RADIANT_NODE_IDENT_K_DEV_BYTES];
	int     rc = radiant_node_nvm_load(RADIANT_NODE_NVM_KEY_KDEV, back, sizeof(back));

	*eq = rc == RADIANT_NODE_NVM_OK && radiant_sec_ct_equal(back, k, sizeof(back));
	/* A failed read may still have filled part of it. */
	radiant_sec_wipe(back, sizeof(back));
	return rc;
}

int radiant_node_ident_provision(const uint8_t k_dev[RADIANT_NODE_IDENT_K_DEV_BYTES])
{
	struct records r = { 0 };
	bool           same;
	bool           ok;
	int            rc;

	if (k_dev == NULL) {
		return RADIANT_NODE_IDENT_EINVAL;
	}
	/* Refuse an all-zero key: a node whose "secret" is sixteen zero bytes
	 * has a pairing scalar and device number anyone can compute. */
	if (key_is_all_zero(k_dev, RADIANT_NODE_IDENT_K_DEV_BYTES)) {
		return RADIANT_NODE_IDENT_EINVAL;
	}
	if (radiant_node_nvm_init() != RADIANT_NODE_NVM_OK) {
		return RADIANT_NODE_IDENT_EIO;
	}

	/* Read before writing (review 2026-11-17 item 7). The installed key
	 * again is a retry, not a factory reset: zeroing its counters would
	 * repeat pairing scalars and replay epochs. Only "never written" may
	 * go on without a match; any other read failure writes nothing. */
	rc = kdev_load_eq(k_dev, &same);
	if (rc != RADIANT_NODE_NVM_OK && rc != RADIANT_NODE_NVM_ENOENT) {
		return RADIANT_NODE_IDENT_EIO;
	}

	/* A new key: key first, then counters, so an interrupted provisioning
	 * leaves the new key with old (unused) counters, never zeroed counters
	 * under the old key - the reset-under-unchanged-key condition ADR 0009
	 * forbids. Retrying with that key then takes the "same" path below. */
	if (!same) {
		if (radiant_node_nvm_store(RADIANT_NODE_NVM_KEY_KDEV, k_dev,
				   RADIANT_NODE_IDENT_K_DEV_BYTES) != RADIANT_NODE_NVM_OK) {
			return RADIANT_NODE_IDENT_EIO;
		}
		/* Confirm by re-reading; any load failure leaves ok false. */
		(void)kdev_load_eq(k_dev, &ok);
		if (!ok) {
			return RADIANT_NODE_IDENT_EIO;
		}
	}

	/* Boot counter, pairing counter, epoch reservation: all three belong
	 * to the key. A new key zeroes them; the same key keeps them and
	 * creates only those an interrupted first provisioning never wrote -
	 * which the record table tells from one lost after use, refused here
	 * as at boot (security review 2026-11-22). */
	if (same) {
		rc = records_load(&r);
		if (rc != RADIANT_NODE_IDENT_OK) {
			return rc;
		}
		if (!records_consistent(true, &r)) {
			return RADIANT_NODE_IDENT_EIO;
		}
	}
	for (size_t i = 0u; i < REC_COUNT; i++) {
		if (r.has[i]) {
			continue;
		}
		rc = store_u32_confirmed(prov_keys[i], 0u);
		if (rc != RADIANT_NODE_IDENT_OK) {
			return rc;
		}
	}
	/* The marker last: from here on every record is mandatory. */
	if (!r.has_fmt) {
		rc = store_u32_confirmed(RADIANT_NODE_NVM_KEY_FMT, FMT_V1);
		if (rc != RADIANT_NODE_IDENT_OK) {
			return rc;
		}
	}

	/* Provisioning invalidates everything derived from the old key. */
	radiant_node_ident_reset();
	return RADIANT_NODE_IDENT_OK;
}

/* K_dev first: from the first delete on, a wipe cut short leaves no K_dev
 * beside some records, which boot refuses (the record table) and a second
 * wipe finishes. Any other order can leave K_dev with part of its set, and
 * once the marker is gone too the table takes that for an older image's
 * store - without N, booting from the boot counter. The marker goes last. */
static const struct {
	const char *key;
	size_t      len;
} wipe_keys[] = {
	{ RADIANT_NODE_NVM_KEY_KDEV, RADIANT_NODE_IDENT_K_DEV_BYTES },
	{ RADIANT_NODE_NVM_KEY_BOOT, sizeof(uint32_t) },
	{ RADIANT_NODE_NVM_KEY_PAIR, sizeof(uint32_t) },
	{ RADIANT_NODE_NVM_KEY_EPOCH, sizeof(uint32_t) },
	{ RADIANT_NODE_NVM_KEY_FMT, sizeof(uint32_t) },
};

int radiant_node_ident_wipe(void)
{
	uint8_t back[RADIANT_NODE_IDENT_K_DEV_BYTES];
	int     rc;

	/* Nothing armed after this may use the identity being erased. */
	radiant_node_ident_reset();
	if (radiant_node_nvm_init() != RADIANT_NODE_NVM_OK) {
		return RADIANT_NODE_IDENT_EIO;
	}
	for (size_t i = 0u; i < ARRAY_SIZE(wipe_keys); i++) {
		if (radiant_node_nvm_delete(wipe_keys[i].key) != RADIANT_NODE_NVM_OK) {
			return RADIANT_NODE_IDENT_EIO;
		}
		/* Confirmed, as every store is: only a deleted record reads
		 * ENOENT at its own size. */
		rc = radiant_node_nvm_load(wipe_keys[i].key, back, wipe_keys[i].len);
		radiant_sec_wipe(back, sizeof(back));
		if (rc != RADIANT_NODE_NVM_ENOENT) {
			return RADIANT_NODE_IDENT_EIO;
		}
	}
	LOG_WRN("identity wiped; the next boot is a first boot");
	return RADIANT_NODE_IDENT_OK;
}

bool radiant_node_ident_is_provisioned(void)
{
	uint8_t k[RADIANT_NODE_IDENT_K_DEV_BYTES];
	bool    ok;

	if (radiant_node_nvm_init() != RADIANT_NODE_NVM_OK) {
		return false;
	}
	ok = radiant_node_nvm_load(RADIANT_NODE_NVM_KEY_KDEV, k, sizeof(k)) == RADIANT_NODE_NVM_OK &&
	     !key_is_all_zero(k, sizeof(k));
	radiant_sec_wipe(k, sizeof(k));
	return ok;
}

/* ── The Tier 0 device number ─────────────────────────────────────────────── */

static int derive_devnum(const uint8_t *k_dev, uint16_t *out)
{
	struct radiant_sec_key root;
	struct radiant_sec_key id;
	uint16_t               raw;
	int                    rc;

	rc = radiant_sec_key_import(&root, k_dev, 128u);
	if (rc != RADIANT_SEC_OK) {
		return RADIANT_NODE_IDENT_ENOTSUP;
	}
	/* base_devnum 0: none exists yet - this call produces it. */
	rc = radiant_sec_kdf(&root, RADIANT_SEC_LABEL_ID, 0u, 0u, &id);
	radiant_sec_key_destroy(&root);
	if (rc != RADIANT_SEC_OK) {
		radiant_sec_key_destroy(&id);
		return RADIANT_NODE_IDENT_ENOTSUP;
	}

	raw = (uint16_t)((uint16_t)id.b[0] | ((uint16_t)id.b[1] << 8));
	radiant_sec_key_destroy(&id);

	/* 1..65535: 0 is the ANT wildcard. Reduction, not rejection sampling -
	 * no unbounded retry at boot - the bias is negligible against the
	 * ~300-device collision bound (tools/ant_identity.py). */
	*out = (uint16_t)((raw % 65535u) + 1u);
	return RADIANT_NODE_IDENT_OK;
}

/* ── Boot ─────────────────────────────────────────────────────────────────── */

__weak bool radiant_node_ident_wall_seconds(uint64_t *out)
{
	(void)out;
	return false;
}

uint32_t radiant_node_ident_epoch_from(uint32_t boot_counter, bool clock_valid,
			       uint64_t wall_s)
{
	uint64_t minutes;

	if (!clock_valid || wall_s < RADIANT_NODE_IDENT_EPOCH_BASE_S) {
		return boot_counter;
	}
	minutes = (wall_s - RADIANT_NODE_IDENT_EPOCH_BASE_S) / 60ull;
	if (minutes > (uint64_t)BOOT_COUNTER_MAX) {
		minutes = (uint64_t)BOOT_COUNTER_MAX;
	}
	/* Boot counter is a floor: a clock that comes back wrong after a
	 * battery change must not hand radiant an already-used epoch. */
	if (minutes < (uint64_t)boot_counter) {
		return boot_counter;
	}
	return (uint32_t)minutes;
}

#if defined(CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION)
/*
 * ADR 0020: K_dev generated on the part, from its own entropy source, the
 * first time it boots with no K_dev record. Reached ONLY on RADIANT_NODE_NVM_ENOENT -
 * see radiant_node_ident_boot() for why no other failure may get here.
 *
 * radiant_node_ident_provision() does the rest: refuses an all-zero key, writes the
 * key before the counters, confirms each by read-back. An all-zero draw is
 * what a failed backend leaves (radiant_sec_rng_*.c zero-fill on failure), so
 * EINVAL from it is an RNG failure and is reported as one.
 */

/* Redraw bound. 11.8 % of numbers are hard to receive for some ANT+ type, so
 * sixteen draws all being bad is about 1 in 10^15: on exhaustion the last draw
 * is kept rather than failing the boot (release review 2026-11-08, item 7). */
#define SELF_PROVISION_DRAWS 16u

/* The device types tools/ant_identity.py's ANT_PLUS_DEVICE_TYPES checks: a
 * K_dev has no device type of its own, so its number must be easy to receive
 * as any of them. */
static const uint8_t ant_plus_types[] = { 0x0B, 0x11, 0x78, 0x7C, 0x79, 0x7B, 0x7A };

/* True when the number this K_dev derives has an alternating run of
 * RADIANT_ADDR_ALT_RUN_AVOID or more in its on-air address, for any type. The
 * number is permanent (it is derived from K_dev, and K_dev is stored once), so
 * a bad draw cannot be fixed afterwards. */
static bool devnum_is_hard_to_receive(const uint8_t *k)
{
	uint16_t devnum;

	if (derive_devnum(k, &devnum) != RADIANT_NODE_IDENT_OK) {
		return false;
	}
	for (size_t i = 0u; i < sizeof(ant_plus_types); i++) {
		if (radiant_channel_id_alt_run(radiant_net_addr_ant_plus,
					       devnum, ant_plus_types[i]) >=
		    RADIANT_ADDR_ALT_RUN_AVOID) {
			return true;
		}
	}
	return false;
}

static bool all_zero(const uint8_t *k, size_t n)
{
	uint8_t acc = 0u;

	for (size_t i = 0u; i < n; i++) {
		acc |= k[i];
	}
	return acc == 0u;
}

static int self_provision(void)
{
	uint8_t k[RADIANT_NODE_IDENT_K_DEV_BYTES];
	int     rc;

	if (!radiant_sec_caps()->has_rng) {
		return RADIANT_NODE_IDENT_ENOKEY;
	}
	for (uint32_t draw = 0u; draw < SELF_PROVISION_DRAWS; draw++) {
		if (radiant_sec_rng(k, sizeof(k)) != RADIANT_SEC_OK) {
			radiant_sec_wipe(k, sizeof(k));
			return RADIANT_NODE_IDENT_ENOKEY;
		}
		/* An all-zero draw is what a failed backend leaves: never
		 * redrawn, so radiant_node_ident_provision() refuses it below as the
		 * RNG failure it is. */
		if (all_zero(k, sizeof(k)) || !devnum_is_hard_to_receive(k)) {
			break;
		}
	}
	rc = radiant_node_ident_provision(k);
	radiant_sec_wipe(k, sizeof(k));
	if (rc == RADIANT_NODE_IDENT_EINVAL) {
		return RADIANT_NODE_IDENT_ENOKEY;
	}
	return rc;
}
#endif /* CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION */

int radiant_node_ident_boot(void)
{
	struct records r = { 0 };
	uint64_t       wall = 0u;
	bool           clock_valid;
	uint32_t       counter;
	int            nrc;
	int            rc;

	radiant_node_ident_reset();

	if (radiant_node_nvm_init() != RADIANT_NODE_NVM_OK) {
		return RADIANT_NODE_IDENT_EIO;
	}

	/*
	 * Three outcomes, and only one of them may lead to a new key. "Never
	 * written" (ENOENT) is a first boot. Anything else - a read error, a
	 * record of the wrong size - is a failure, reported as EIO and never
	 * repaired by re-provisioning: a flaky read that re-rolled K_dev would
	 * silently move the device number and orphan every paired receiver.
	 *
	 * And "never written" is believed only beside an empty store: a K_dev
	 * that reads as absent next to counters it used was lost, not never
	 * written (the record table above). So is any other record missing
	 * from a set that cannot lack it. Both are EIO, and neither writes.
	 */
	nrc = radiant_node_nvm_load(RADIANT_NODE_NVM_KEY_KDEV, s.k_dev, sizeof(s.k_dev));
	if (nrc != RADIANT_NODE_NVM_OK && nrc != RADIANT_NODE_NVM_ENOENT) {
		rc = RADIANT_NODE_IDENT_EIO;
		goto fail;
	}
	rc = records_load(&r);
	if (rc != RADIANT_NODE_IDENT_OK) {
		goto fail;
	}
	if (!records_consistent(nrc == RADIANT_NODE_NVM_OK, &r)) {
		LOG_ERR("identity record lost; radiant_node_ident_wipe() re-provisions");
		rc = RADIANT_NODE_IDENT_EIO;
		goto fail;
	}
	if (nrc == RADIANT_NODE_NVM_ENOENT) {
#if defined(CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION)
		rc = self_provision();
		if (rc != RADIANT_NODE_IDENT_OK) {
			goto fail;
		}
		LOG_INF("identity provisioned on first boot");
		nrc = radiant_node_nvm_load(RADIANT_NODE_NVM_KEY_KDEV, s.k_dev,
				    sizeof(s.k_dev));
		if (nrc != RADIANT_NODE_NVM_OK) {
			rc = RADIANT_NODE_IDENT_EIO;
			goto fail;
		}
		rc = records_load(&r);
		if (rc != RADIANT_NODE_IDENT_OK) {
			goto fail;
		}
#else
		return RADIANT_NODE_IDENT_ENOKEY;
#endif
	}
	/* A stored all-zero key is not "never written", so it is not repaired
	 * either; radiant_node_ident_provision() can never have written one. */
	if (key_is_all_zero(s.k_dev, sizeof(s.k_dev))) {
		rc = RADIANT_NODE_IDENT_ENOKEY;
		goto fail;
	}
	s.have_k_dev = true;

	counter = r.val[REC_BOOT];
	if (counter >= BOOT_COUNTER_MAX) {
		rc = RADIANT_NODE_IDENT_ESTATE;
		goto fail;
	}
	counter++;

	/* Write must land before anything uses the value - the 0xF3 refusal,
	 * relocated. If it doesn't stick, this node has not booted. */
	rc = store_u32_confirmed(RADIANT_NODE_NVM_KEY_BOOT, counter);
	if (rc != RADIANT_NODE_IDENT_OK) {
		goto fail;
	}
	s.boot_counter = counter;

	/* Absent only on an unmarked store whose provisioning never wrote it
	 * (the table): 0 is then its true value, and it is created so that
	 * the marker below can say every record is there. */
	s.pair_counter = r.val[REC_PAIR];
	if (!r.has[REC_PAIR]) {
		rc = store_u32_confirmed(RADIANT_NODE_NVM_KEY_PAIR, 0u);
		if (rc != RADIANT_NODE_IDENT_OK) {
			goto fail;
		}
	}

	rc = derive_devnum(s.k_dev, &s.devnum);
	if (rc != RADIANT_NODE_IDENT_OK) {
		goto fail;
	}

	/*
	 * The epoch: never below N, the next one nobody has used. N is what
	 * makes a wrap's epoch (used with no boot in between) and a second boot
	 * inside one wall-clock minute safe; the boot counter and the clock only
	 * ever raise it. Then N = E + 2 lands in storage BEFORE E is handed out:
	 * E + 1 is reserved for this session's first wrap, so a crash right after
	 * that wrap still leaves the next boot above it. An older image's store
	 * has no N record, and the boot counter alone is then the floor it always
	 * was - but only an unmarked store gets here without N: on a marked one
	 * a missing N was lost, and the table refused it above.
	 */
	clock_valid = radiant_node_ident_wall_seconds(&wall);
	s.epoch = radiant_node_ident_epoch_from(s.boot_counter, clock_valid, wall);
	if (r.val[REC_NEXT] > s.epoch) {
		s.epoch = r.val[REC_NEXT];
	}
	if (s.epoch > EPOCH_BOOT_MAX) {
		rc = RADIANT_NODE_IDENT_ESTATE;
		goto fail;
	}
	rc = store_u32_confirmed(RADIANT_NODE_NVM_KEY_EPOCH, s.epoch + 2u);
	if (rc != RADIANT_NODE_IDENT_OK) {
		goto fail;
	}
	s.epoch_next = s.epoch + 2u;

	/* Every record is in storage now; mark the store so that a later loss
	 * of any of them is refused rather than read as 0. Once per store. */
	if (!r.has_fmt) {
		rc = store_u32_confirmed(RADIANT_NODE_NVM_KEY_FMT, FMT_V1);
		if (rc != RADIANT_NODE_IDENT_OK) {
			goto fail;
		}
	}
	s.booted = true;
	return RADIANT_NODE_IDENT_OK;

fail:
	radiant_node_ident_reset();
	return rc;
}

const char *radiant_node_ident_strerror(int rc)
{
	switch (rc) {
	case RADIANT_NODE_IDENT_OK:
		return "ok";
	case RADIANT_NODE_IDENT_EINVAL:
		return "invalid argument";
	case RADIANT_NODE_IDENT_ENOTSUP:
		return "the crypto backend refused K_dev";
	case RADIANT_NODE_IDENT_ENOKEY:
		return "no K_dev (not provisioned, and none could be generated)";
	case RADIANT_NODE_IDENT_EIO:
		return "identity storage failed, or lost a record";
	case RADIANT_NODE_IDENT_ESTATE:
		return "boot counter exhausted, or identity not booted";
	case RADIANT_NODE_IDENT_EAGAIN:
		return "a fresh epoch is being reserved; retry the arm";
	case RADIANT_NODE_IDENT_EBUSY:
		return "another live channel has this root and device number";
	default:
		return "unknown error";
	}
}

int radiant_node_ident_boot_counter(uint32_t *out)
{
	if (out == NULL) {
		return RADIANT_NODE_IDENT_EINVAL;
	}
	if (!s.booted) {
		return RADIANT_NODE_IDENT_ESTATE;
	}
	*out = s.boot_counter;
	return RADIANT_NODE_IDENT_OK;
}

int radiant_node_ident_epoch(uint32_t *out)
{
	if (out == NULL) {
		return RADIANT_NODE_IDENT_EINVAL;
	}
	if (!s.booted) {
		return RADIANT_NODE_IDENT_ESTATE;
	}
	*out = s.epoch;
	return RADIANT_NODE_IDENT_OK;
}

int radiant_node_ident_pair_counter(uint32_t *out)
{
	if (out == NULL) {
		return RADIANT_NODE_IDENT_EINVAL;
	}
	if (!s.booted) {
		return RADIANT_NODE_IDENT_ESTATE;
	}
	*out = s.pair_counter;
	return RADIANT_NODE_IDENT_OK;
}

int radiant_node_ident_devnum(uint16_t *out)
{
	if (out == NULL) {
		return RADIANT_NODE_IDENT_EINVAL;
	}
	if (!s.booted) {
		return RADIANT_NODE_IDENT_ESTATE;
	}
	*out = s.devnum;
	return RADIANT_NODE_IDENT_OK;
}

/* ── Handing the epoch to radiant ────────────────────────────────────── */

/* Bit for channel `ch` in sec_armed/compat_armed; 0 for a channel number the
 * mask cannot hold, which the arm calls refuse. */
static uint32_t ch_bit(uint8_t ch)
{
	return (ch < 32u) ? (1u << ch) : 0u;
}

/* Ask the work item to make N = epoch + 2 durable, then raise the ceilings.
 * Safe from the pump (spinlock and a work submit only). */
static void epoch_reserve(uint32_t epoch)
{
	k_spinlock_key_t key = k_spin_lock(&ep_lock);

	if (!ep_have || epoch > ep_pending) {
		ep_pending = epoch;
	}
	ep_have = true;
	k_spin_unlock(&ep_lock, key);
	(void)k_work_schedule(&ep_work, K_NO_WAIT);
}

/*
 * The epoch an arm may use (re-review 2026-11-13 M2). Every arm restarts the
 * slot counter at 0, so two arms under one epoch with the same root and
 * device number - the same channel re-armed after an unassign or re-key, or
 * another channel armed under the node root once the first was released -
 * would run the counter twice under one (root, epoch, devnum): a two-time pad.
 * So E goes to the FIRST arm of each kind only; every later arm takes the next
 * unused epoch F = N, and only once N = F + 2 is durable (F for it, F + 1 for
 * its first wrap). Until then the arm returns RADIANT_NODE_IDENT_EAGAIN and the caller
 * retries; the reservation is the same work item a wrap uses.
 *
 * Distinct arm epochs do not keep two LIVE channels apart: B armed at E + 2
 * raises A's ceiling to E + 3 and A's second wrap lands on E + 2 (review
 * 2026-11-14 item 2). So `twin` - another live channel of this kind under the
 * same root and base device number, radiant's answer - refuses the arm before
 * any epoch is reserved for it.
 */
static int arm_epoch(bool first_done, uint8_t ch, bool twin, uint32_t *out)
{
	if (!s.booted) {
		return RADIANT_NODE_IDENT_ESTATE;
	}
	if (ch_bit(ch) == 0u) {
		return RADIANT_NODE_IDENT_EINVAL;
	}
	if (twin) {
		return RADIANT_NODE_IDENT_EBUSY;
	}
	if (!first_done) {
		*out = s.epoch;
		return RADIANT_NODE_IDENT_OK;
	}
	if (s.arm_fresh == 0u) {
		if (s.epoch_next > EPOCH_BOOT_MAX) {
			return RADIANT_NODE_IDENT_ESTATE;
		}
		s.arm_fresh = s.epoch_next;
	}
	if (s.epoch_next < s.arm_fresh + 2u) {
		epoch_reserve(s.arm_fresh);
		return RADIANT_NODE_IDENT_EAGAIN;
	}
	*out = s.arm_fresh;
	return RADIANT_NODE_IDENT_OK;
}

/* After a successful arm with `epoch`: a fresh epoch is spent. */
static void arm_done(uint32_t epoch)
{
	if (s.arm_fresh != 0u && epoch == s.arm_fresh) {
		s.arm_fresh = 0u;
	}
}

int radiant_node_ident_arm_sec(uint8_t ch, uint16_t period_counts)
{
	uint32_t e = 0u;
	int      rc = arm_epoch(s.sec_first_done, ch, radiant_sec_live_twin(ch),
				    &e);

	if (rc != RADIANT_NODE_IDENT_OK) {
		return rc;
	}
	/* us_into_epoch = 0: the epoch is this arm's own, so the node is at
	 * its start by construction. A channel this node already follows
	 * carries its ceiling, and radiant refuses set_epoch there (that is
	 * what stops a host 0xF3); the node, being the authority, clears it
	 * and re-sets it - restoring it if the epoch is refused anyway. */
	rc = radiant_sec_set_epoch(ch, e, 0u, period_counts);
	if (rc == RADIANT_SEC_ESTATE) {
		(void)radiant_sec_set_epoch_ceiling(ch, UINT32_MAX);
		rc = radiant_sec_set_epoch(ch, e, 0u, period_counts);
		if (rc != RADIANT_SEC_OK) {
			(void)radiant_sec_set_epoch_ceiling(ch,
							    s.epoch_next - 1u);
		}
	}
	if (rc != RADIANT_SEC_OK ||
	    radiant_sec_set_epoch_ceiling(ch, s.epoch_next - 1u) !=
		    RADIANT_SEC_OK) {
		return RADIANT_NODE_IDENT_EIO;
	}
	s.sec_first_done = true;
	arm_done(e);
	s.sec_armed |= ch_bit(ch);
	return RADIANT_NODE_IDENT_OK;
}

int radiant_node_ident_arm_compat(uint8_t ch, uint64_t now_us, uint16_t period_counts)
{
	uint32_t e = 0u;
	int      rc = arm_epoch(s.compat_first_done, ch,
				    radiant_sec_compat_live_twin(ch), &e);

	if (rc != RADIANT_NODE_IDENT_OK) {
		return rc;
	}
	rc = radiant_sec_compat_set_epoch(ch, e, now_us, 0u, period_counts);
	if (rc == RADIANT_SEC_ESTATE) {
		(void)radiant_sec_compat_set_epoch_ceiling(ch, UINT32_MAX);
		rc = radiant_sec_compat_set_epoch(ch, e, now_us, 0u,
						  period_counts);
		if (rc != RADIANT_SEC_OK) {
			(void)radiant_sec_compat_set_epoch_ceiling(
				ch, s.epoch_next - 1u);
		}
	}
	if (rc != RADIANT_SEC_OK ||
	    radiant_sec_compat_set_epoch_ceiling(ch, s.epoch_next - 1u) !=
		    RADIANT_SEC_OK) {
		return RADIANT_NODE_IDENT_EIO;
	}
	s.compat_first_done = true;
	arm_done(e);
	s.compat_armed |= ch_bit(ch);
	return RADIANT_NODE_IDENT_OK;
}

/* ── Following the epoch through counter wraps ───────────────────────────── */

/* Record that `epoch` has been used (or, from a _wanted hook, epoch + 1 is
 * wanted - the same reservation, N = epoch + 2) and let the work item persist
 * it. Caller's context (the pump, mid-transform): no flash, no blocking, no
 * radiant_sec call. radiant calls the hooks only for a channel that still
 * carries a ceiling, so a channel re-keyed or released is no longer followed
 * (review item 3). epoch_reserve() uses k_work_schedule(), not reschedule: a
 * _wanted hook fires every slot while TX is withheld, and must not cut a
 * persist retry's backoff short. */
static void epoch_note(uint32_t armed, uint8_t ch, uint32_t epoch)
{
	if (!s.booted || (armed & ch_bit(ch)) == 0u) {
		return;
	}
	epoch_reserve(epoch);
}

void radiant_sec_epoch_advanced(uint8_t ch, uint32_t epoch)
{
	epoch_note(s.sec_armed, ch, epoch);
}

/* A catch-up of several wraps (the sender was silent for more than one)
 * wants an epoch past the ceiling; without this it would never advance. */
void radiant_sec_epoch_wanted(uint8_t ch, uint32_t epoch)
{
	epoch_note(s.sec_armed, ch, epoch - 1u);
}

#if defined(CONFIG_RADIANT_SEC_COMPAT)
void radiant_sec_compat_epoch_advanced(uint8_t ch, uint32_t epoch)
{
	epoch_note(s.compat_armed, ch, epoch);
}

void radiant_sec_compat_epoch_wanted(uint8_t ch, uint32_t epoch)
{
	epoch_note(s.compat_armed, ch, epoch - 1u);
}
#endif

static void epoch_work_fn(struct k_work *work)
{
	k_spinlock_key_t key;
	uint32_t         e;
	uint32_t         m;
	uint8_t          ch;
	bool             have;

	ARG_UNUSED(work);
	key = k_spin_lock(&ep_lock);
	e = ep_pending;
	have = ep_have;
	ep_have = false;
	k_spin_unlock(&ep_lock, key);
	if (!have || !s.booted) {
		return;
	}

	/* Persist first: a ceiling raised before N is in storage would let a
	 * wrap use an epoch the next boot could take again. Never lowers N. */
	if (e <= EPOCH_BOOT_MAX && e + 2u > s.epoch_next) {
		if (store_u32_confirmed(RADIANT_NODE_NVM_KEY_EPOCH, e + 2u) !=
		    RADIANT_NODE_IDENT_OK) {
			key = k_spin_lock(&ep_lock);
			if (!ep_have || e > ep_pending) {
				ep_pending = e;
			}
			ep_have = true;
			k_spin_unlock(&ep_lock, key);
			if (ep_retry_ms == 0u) {
				LOG_WRN("epoch %u not persisted; secured TX "
					"holds at the next wrap until it is",
					(unsigned)e);
				ep_retry_ms = EPOCH_RETRY_MIN_MS;
			} else if (ep_retry_ms < EPOCH_RETRY_MAX_MS / 2u) {
				ep_retry_ms *= 2u;
			} else {
				ep_retry_ms = EPOCH_RETRY_MAX_MS;
			}
			(void)k_work_reschedule(&ep_work, K_MSEC(ep_retry_ms));
			return;
		}
		if (ep_retry_ms != 0u) {
			LOG_INF("epoch %u persisted after retries", (unsigned)e);
			ep_retry_ms = 0u;
		}
		s.epoch_next = e + 2u;
	}

	/* Raise, never set: a channel whose ceiling another party's set_epoch
	 * cleared (a host 0xF3, a release and re-key) is left alone rather
	 * than capped under an epoch it never reserved. Lock-free by design;
	 * see radiant_sec_raise_epoch_ceiling(). */
	for (ch = 0u, m = s.sec_armed; m != 0u; ch++, m >>= 1) {
		if ((m & 1u) != 0u) {
			(void)radiant_sec_raise_epoch_ceiling(
				ch, s.epoch_next - 1u);
		}
	}
	for (ch = 0u, m = s.compat_armed; m != 0u; ch++, m >>= 1) {
		if ((m & 1u) != 0u) {
			(void)radiant_sec_compat_raise_epoch_ceiling(
				ch, s.epoch_next - 1u);
		}
	}
}

int radiant_node_ident_epoch_next(uint32_t *out)
{
	if (out == NULL) {
		return RADIANT_NODE_IDENT_EINVAL;
	}
	if (!s.booted) {
		return RADIANT_NODE_IDENT_ESTATE;
	}
	*out = s.epoch_next;
	return RADIANT_NODE_IDENT_OK;
}

/* ── Pairing ──────────────────────────────────────────────────────────────── */

enum radiant_node_ident_scalar_src radiant_node_ident_scalar_src(bool host_supplied,
						 bool has_rng)
{
	if (host_supplied) {
		return RADIANT_NODE_IDENT_SCALAR_HOST;
	}
	if (has_rng) {
		return RADIANT_NODE_IDENT_SCALAR_RNG;
	}
	return RADIANT_NODE_IDENT_SCALAR_KDF;
}

/*
 * SP 800-108 counter mode, AES-CMAC as PRF:
 *   block_i = i || "pair" || 0x00 || pair_counter[4 LE] || base_devnum[2 LE] || 0x0100
 * Differs from radiant_sec_kdf()'s block: counter runs 1..2 (not pinned at 1)
 * and [L]_2 = 256 (not 128), so two calls don't repeat the same 16 bytes, and
 * the block can't be confused with the 128-bit "pair" construction.
 */
int radiant_node_ident_pair_derive(const uint8_t k_dev[RADIANT_NODE_IDENT_K_DEV_BYTES],
			   uint32_t pair_counter, uint16_t base_devnum,
			   uint8_t out[RADIANT_NODE_IDENT_PAIR_SCALAR_BYTES])
{
	struct radiant_sec_key root;
	uint8_t                block[14];
	uint8_t                tag[RADIANT_SEC_BLOCK_BYTES];
	int                    rc;

	if (k_dev == NULL || out == NULL) {
		return RADIANT_NODE_IDENT_EINVAL;
	}
	rc = radiant_sec_key_import(&root, k_dev, 128u);
	if (rc != RADIANT_SEC_OK) {
		return RADIANT_NODE_IDENT_ENOTSUP;
	}

	block[1]  = (uint8_t)RADIANT_SEC_LABEL_PAIR[0];
	block[2]  = (uint8_t)RADIANT_SEC_LABEL_PAIR[1];
	block[3]  = (uint8_t)RADIANT_SEC_LABEL_PAIR[2];
	block[4]  = (uint8_t)RADIANT_SEC_LABEL_PAIR[3];
	block[5]  = 0x00u;                        /* the separator */
	block[6]  = (uint8_t)pair_counter;
	block[7]  = (uint8_t)(pair_counter >> 8);
	block[8]  = (uint8_t)(pair_counter >> 16);
	block[9]  = (uint8_t)(pair_counter >> 24);
	block[10] = (uint8_t)base_devnum;
	block[11] = (uint8_t)(base_devnum >> 8);
	block[12] = 0x01u;                        /* [L]_2 = 256 bits, BE */
	block[13] = 0x00u;

	for (uint8_t i = 1u; i <= 2u; i++) {
		block[0] = i;
		rc = radiant_sec_cmac(&root, block, sizeof(block), tag);
		if (rc != RADIANT_SEC_OK) {
			break;
		}
		memcpy(out + ((size_t)(i - 1u) * RADIANT_SEC_BLOCK_BYTES), tag,
		       RADIANT_SEC_BLOCK_BYTES);
	}

	radiant_sec_key_destroy(&root);
	radiant_sec_wipe(tag, sizeof(tag));
	radiant_sec_wipe(block, sizeof(block));
	if (rc != RADIANT_SEC_OK) {
		radiant_sec_wipe(out, RADIANT_NODE_IDENT_PAIR_SCALAR_BYTES);
		return RADIANT_NODE_IDENT_ENOTSUP;
	}
	return RADIANT_NODE_IDENT_OK;
}

int radiant_node_ident_pair_window_open(uint32_t *ctr_out)
{
	uint32_t next;
	int      rc;

	if (!s.booted) {
		return RADIANT_NODE_IDENT_ESTATE;
	}
	if (s.pair_counter >= 0xFFFFFFFFu) {
		return RADIANT_NODE_IDENT_ESTATE;
	}
	next = s.pair_counter + 1u;

	/* Advance and persist first; if the write fails, do not enter the
	 * window at all (fail closed). */
	rc = store_u32_confirmed(RADIANT_NODE_NVM_KEY_PAIR, next);
	if (rc != RADIANT_NODE_IDENT_OK) {
		return rc;
	}

	s.pair_counter = next;
	s.scalar_ctr = next;
	s.window_open = true;
	if (ctr_out != NULL) {
		*ctr_out = next;
	}
	return RADIANT_NODE_IDENT_OK;
}

/*
 * The gate in front of every scalar source: an open window whose counter is
 * already in storage (not cached/queued). `!=` not `<` - a stored counter
 * ahead means some other path moved it, and a scalar under a stale window
 * would be the repeat the counter exists to prevent.
 */
static int pair_ctr_durable(void)
{
	uint32_t stored = 0u;

	if (!s.window_open) {
		return RADIANT_NODE_IDENT_ESTATE;
	}
	if (radiant_node_nvm_load(RADIANT_NODE_NVM_KEY_PAIR, &stored,
			  sizeof(stored)) != RADIANT_NODE_NVM_OK) {
		return RADIANT_NODE_IDENT_EIO;
	}
	if (stored != s.scalar_ctr) {
		return RADIANT_NODE_IDENT_EIO;
	}
	return RADIANT_NODE_IDENT_OK;
}

int radiant_node_ident_pair_scalar(uint8_t out[RADIANT_NODE_IDENT_PAIR_SCALAR_BYTES])
{
	int rc;

	if (out == NULL) {
		return RADIANT_NODE_IDENT_EINVAL;
	}
	if (!s.booted || !s.have_k_dev) {
		return RADIANT_NODE_IDENT_ESTATE;
	}
	rc = pair_ctr_durable();
	if (rc != RADIANT_NODE_IDENT_OK) {
		return rc;
	}
	return radiant_node_ident_pair_derive(s.k_dev, s.scalar_ctr, s.devnum, out);
}

bool radiant_node_ident_pair_window_is_open(void)
{
	return s.window_open;
}

void radiant_node_ident_pair_window_close(void)
{
	/* Counter is NOT rolled back: a burnt value costs 1/2^32; a reused one
	 * costs forward secrecy and a stable cross-session identifier. */
	s.window_open = false;
	s.scalar_ctr = 0u;
}

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

int radiant_node_ident_pair_begin(uint8_t ch, uint8_t timeout_s)
{
	uint8_t scalar[RADIANT_NODE_IDENT_PAIR_SCALAR_BYTES];
	int     rc;

	if (!s.booted) {
		return RADIANT_NODE_IDENT_ESTATE;
	}
	if (!radiant_sec_caps()->has_x25519) {
		return RADIANT_NODE_IDENT_ENOTSUP;
	}

	/* 1. Counter moves and lands in NVM, or nothing else happens. */
	rc = radiant_node_ident_pair_window_open(NULL);
	if (rc != RADIANT_NODE_IDENT_OK) {
		return rc;
	}

	/* 2. Open radiant's pairing context; no key exists yet. */
	if (radiant_sec_pair_enter(ch, timeout_s) != RADIANT_SEC_OK) {
		radiant_node_ident_pair_window_close();
		return RADIANT_NODE_IDENT_EIO;
	}

	/* 3. The scalar, gated on step 1 having reached storage. No host here,
	 * so ADR 0009's precedence leaves the RNG when caps.has_rng says one
	 * exists and the KDF otherwise. A failed draw, or one that "succeeded"
	 * with nothing in the buffer, closes the window; the counter stays
	 * burnt. */
	if (radiant_node_ident_scalar_src(false, radiant_sec_caps()->has_rng) ==
	    RADIANT_NODE_IDENT_SCALAR_RNG) {
		rc = pair_ctr_durable();
		if (rc == RADIANT_NODE_IDENT_OK &&
		    (radiant_sec_rng(scalar, sizeof(scalar)) != RADIANT_SEC_OK ||
		     key_is_all_zero(scalar, sizeof(scalar)))) {
			rc = RADIANT_NODE_IDENT_EIO;
		}
	} else {
		rc = radiant_node_ident_pair_scalar(scalar);
	}
	if (rc != RADIANT_NODE_IDENT_OK) {
		radiant_sec_wipe(scalar, sizeof(scalar));
		radiant_sec_pair_leave(ch);
		radiant_node_ident_pair_window_close();
		return rc;
	}

	/* 4. Install it; this is where the public key is computed. */
	rc = radiant_sec_pair_set_scalar(ch, scalar);
	radiant_sec_wipe(scalar, sizeof(scalar));
	if (rc != RADIANT_SEC_OK) {
		radiant_sec_pair_leave(ch);
		radiant_node_ident_pair_window_close();
		return RADIANT_NODE_IDENT_EIO;
	}
	return RADIANT_NODE_IDENT_OK;
}

int radiant_node_ident_pair_pubkey(uint8_t ch,
			   uint8_t out[RADIANT_NODE_IDENT_PAIR_SCALAR_BYTES])
{
	if (out == NULL) {
		return RADIANT_NODE_IDENT_EINVAL;
	}
	if (!s.window_open) {
		return RADIANT_NODE_IDENT_ESTATE;
	}
	if (radiant_sec_pair_local_pubkey(ch, out) != RADIANT_SEC_OK) {
		return RADIANT_NODE_IDENT_EIO;
	}
	return RADIANT_NODE_IDENT_OK;
}

#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */

void radiant_node_ident_reset(void)
{
	k_spinlock_key_t key;

	/* A queued persist belongs to the session being reset; on hardware a
	 * reset is a reboot and nothing is queued. Cancelled, not flushed: the
	 * next boot reserves above whatever it left. */
	{
		struct k_work_sync sync;

		/* _sync: a persist already running finishes (or is waited
		 * out) before `s` is wiped under it. */
		(void)k_work_cancel_delayable_sync(&ep_work, &sync);
	}
	key = k_spin_lock(&ep_lock);
	ep_have = false;
	ep_pending = 0u;
	ep_retry_ms = 0u;
	k_spin_unlock(&ep_lock, key);
	memset(&s, 0, sizeof(s));
}
