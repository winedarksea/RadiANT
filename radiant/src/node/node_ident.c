/* SPDX-License-Identifier: Apache-2.0 */
/*
 * node_ident.c - the hostless node's identity, epoch and pairing scalar.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md. See node_ident.h
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

#include "node_ident.h"
#include "node_nvm.h"

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
	/* N, the next epoch nobody has used: what NODE_NVM_KEY_EPOCH holds, as
	 * confirmed by the last write. Every channel this node arms carries a
	 * ceiling of epoch_next - 1, so no epoch is used before N above it has
	 * reached storage (docs/radiant-security.md 3.5). */
	uint32_t epoch_next;
	/* The channels node_ident_arm_sec()/_arm_compat() armed: the only ones
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
	uint8_t  k_dev[NODE_IDENT_K_DEV_BYTES];
	bool     have_k_dev;

	/* The pairing window. `scalar_ctr` is the counter value the live
	 * scalar was derived from, and it is what node_ident_pair_scalar()
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
 * Outside `s` because node_ident_reset() memsets `s` and must not memset a
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
	int rc = node_nvm_load(key, out, sizeof(*out));

	if (rc == NODE_NVM_ENOENT) {
		*out = 0u;
		*present = false;
		return NODE_IDENT_OK;
	}
	if (rc != NODE_NVM_OK) {
		return NODE_IDENT_EIO;
	}
	*present = true;
	return NODE_IDENT_OK;
}

/* Store then re-read to confirm. Catches a backend that returns OK but can't
 * actually produce the value, at the write rather than a power cycle later. */
static int store_u32_confirmed(const char *key, uint32_t val)
{
	uint32_t back = 0u;

	if (node_nvm_store(key, &val, sizeof(val)) != NODE_NVM_OK) {
		return NODE_IDENT_EIO;
	}
	if (node_nvm_load(key, &back, sizeof(back)) != NODE_NVM_OK) {
		return NODE_IDENT_EIO;
	}
	if (back != val) {
		return NODE_IDENT_EIO;
	}
	return NODE_IDENT_OK;
}

/* ── Provisioning (first boot, ADR 0020, or a factory step) ──────────────── */

static const char *const prov_keys[] = {
	NODE_NVM_KEY_BOOT, NODE_NVM_KEY_PAIR, NODE_NVM_KEY_EPOCH,
};

/* Load the stored K_dev and compare it with `k` in constant time. `*eq` is
 * false whenever the load is not NODE_NVM_OK; the load's code is returned. */
static int kdev_load_eq(const uint8_t *k, bool *eq)
{
	uint8_t back[NODE_IDENT_K_DEV_BYTES];
	int     rc = node_nvm_load(NODE_NVM_KEY_KDEV, back, sizeof(back));

	*eq = rc == NODE_NVM_OK && radiant_sec_ct_equal(back, k, sizeof(back));
	/* A failed read may still have filled part of it. */
	radiant_sec_wipe(back, sizeof(back));
	return rc;
}

int node_ident_provision(const uint8_t k_dev[NODE_IDENT_K_DEV_BYTES])
{
	uint32_t v;
	bool     present;
	bool     same;
	bool     ok;
	int      rc;

	if (k_dev == NULL) {
		return NODE_IDENT_EINVAL;
	}
	/* Refuse an all-zero key: a node whose "secret" is sixteen zero bytes
	 * has a pairing scalar and device number anyone can compute. */
	if (key_is_all_zero(k_dev, NODE_IDENT_K_DEV_BYTES)) {
		return NODE_IDENT_EINVAL;
	}
	if (node_nvm_init() != NODE_NVM_OK) {
		return NODE_IDENT_EIO;
	}

	/* Read before writing (review 2026-11-17 item 7). The installed key
	 * again is a retry, not a factory reset: zeroing its counters would
	 * repeat pairing scalars and replay epochs. Only "never written" may
	 * go on without a match; any other read failure writes nothing. */
	rc = kdev_load_eq(k_dev, &same);
	if (rc != NODE_NVM_OK && rc != NODE_NVM_ENOENT) {
		return NODE_IDENT_EIO;
	}

	/* A new key: key first, then counters, so an interrupted provisioning
	 * leaves the new key with old (unused) counters, never zeroed counters
	 * under the old key - the reset-under-unchanged-key condition ADR 0009
	 * forbids. Retrying with that key then takes the "same" path below. */
	if (!same) {
		if (node_nvm_store(NODE_NVM_KEY_KDEV, k_dev,
				   NODE_IDENT_K_DEV_BYTES) != NODE_NVM_OK) {
			return NODE_IDENT_EIO;
		}
		/* Confirm by re-reading; any load failure leaves ok false. */
		(void)kdev_load_eq(k_dev, &ok);
		if (!ok) {
			return NODE_IDENT_EIO;
		}
	}

	/* Boot counter, pairing counter, epoch reservation: all three belong
	 * to the key. A new key zeroes them; the same key keeps them and
	 * creates only those an interrupted first provisioning never wrote. */
	for (size_t i = 0u; i < ARRAY_SIZE(prov_keys); i++) {
		if (same) {
			rc = load_u32(prov_keys[i], &v, &present);
			if (rc != NODE_IDENT_OK) {
				return rc;
			}
			if (present) {
				continue;
			}
		}
		rc = store_u32_confirmed(prov_keys[i], 0u);
		if (rc != NODE_IDENT_OK) {
			return rc;
		}
	}

	/* Provisioning invalidates everything derived from the old key. */
	node_ident_reset();
	return NODE_IDENT_OK;
}

bool node_ident_is_provisioned(void)
{
	uint8_t k[NODE_IDENT_K_DEV_BYTES];
	bool    ok;

	if (node_nvm_init() != NODE_NVM_OK) {
		return false;
	}
	ok = node_nvm_load(NODE_NVM_KEY_KDEV, k, sizeof(k)) == NODE_NVM_OK &&
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
		return NODE_IDENT_ENOTSUP;
	}
	/* base_devnum 0: none exists yet - this call produces it. */
	rc = radiant_sec_kdf(&root, RADIANT_SEC_LABEL_ID, 0u, 0u, &id);
	radiant_sec_key_destroy(&root);
	if (rc != RADIANT_SEC_OK) {
		radiant_sec_key_destroy(&id);
		return NODE_IDENT_ENOTSUP;
	}

	raw = (uint16_t)((uint16_t)id.b[0] | ((uint16_t)id.b[1] << 8));
	radiant_sec_key_destroy(&id);

	/* 1..65535: 0 is the ANT wildcard. Reduction, not rejection sampling -
	 * no unbounded retry at boot - the bias is negligible against the
	 * ~300-device collision bound (tools/ant_identity.py). */
	*out = (uint16_t)((raw % 65535u) + 1u);
	return NODE_IDENT_OK;
}

/* ── Boot ─────────────────────────────────────────────────────────────────── */

__weak bool node_ident_wall_seconds(uint64_t *out)
{
	(void)out;
	return false;
}

uint32_t node_ident_epoch_from(uint32_t boot_counter, bool clock_valid,
			       uint64_t wall_s)
{
	uint64_t minutes;

	if (!clock_valid || wall_s < NODE_IDENT_EPOCH_BASE_S) {
		return boot_counter;
	}
	minutes = (wall_s - NODE_IDENT_EPOCH_BASE_S) / 60ull;
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
 * first time it boots with no K_dev record. Reached ONLY on NODE_NVM_ENOENT -
 * see node_ident_boot() for why no other failure may get here.
 *
 * node_ident_provision() does the rest: refuses an all-zero key, writes the
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

	if (derive_devnum(k, &devnum) != NODE_IDENT_OK) {
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
	uint8_t k[NODE_IDENT_K_DEV_BYTES];
	int     rc;

	if (!radiant_sec_caps()->has_rng) {
		return NODE_IDENT_ENOKEY;
	}
	for (uint32_t draw = 0u; draw < SELF_PROVISION_DRAWS; draw++) {
		if (radiant_sec_rng(k, sizeof(k)) != RADIANT_SEC_OK) {
			radiant_sec_wipe(k, sizeof(k));
			return NODE_IDENT_ENOKEY;
		}
		/* An all-zero draw is what a failed backend leaves: never
		 * redrawn, so node_ident_provision() refuses it below as the
		 * RNG failure it is. */
		if (all_zero(k, sizeof(k)) || !devnum_is_hard_to_receive(k)) {
			break;
		}
	}
	rc = node_ident_provision(k);
	radiant_sec_wipe(k, sizeof(k));
	if (rc == NODE_IDENT_EINVAL) {
		return NODE_IDENT_ENOKEY;
	}
	return rc;
}
#endif /* CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION */

int node_ident_boot(void)
{
	uint64_t wall = 0u;
	bool     clock_valid;
	bool     present;
	uint32_t counter;
	int      nrc;
	int      rc;

	node_ident_reset();

	if (node_nvm_init() != NODE_NVM_OK) {
		return NODE_IDENT_EIO;
	}

	/*
	 * Three outcomes, and only one of them may lead to a new key. "Never
	 * written" (ENOENT) is a first boot. Anything else - a read error, a
	 * record of the wrong size - is a failure, reported as EIO and never
	 * repaired by re-provisioning: a flaky read that re-rolled K_dev would
	 * silently move the device number and orphan every paired receiver.
	 */
	nrc = node_nvm_load(NODE_NVM_KEY_KDEV, s.k_dev, sizeof(s.k_dev));
	if (nrc == NODE_NVM_ENOENT) {
#if defined(CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION)
		rc = self_provision();
		if (rc != NODE_IDENT_OK) {
			goto fail;
		}
		LOG_INF("identity provisioned on first boot");
		nrc = node_nvm_load(NODE_NVM_KEY_KDEV, s.k_dev,
				    sizeof(s.k_dev));
		if (nrc != NODE_NVM_OK) {
			rc = NODE_IDENT_EIO;
			goto fail;
		}
#else
		return NODE_IDENT_ENOKEY;
#endif
	} else if (nrc != NODE_NVM_OK) {
		rc = NODE_IDENT_EIO;
		goto fail;
	}
	/* A stored all-zero key is not "never written", so it is not repaired
	 * either; node_ident_provision() can never have written one. */
	if (key_is_all_zero(s.k_dev, sizeof(s.k_dev))) {
		rc = NODE_IDENT_ENOKEY;
		goto fail;
	}
	s.have_k_dev = true;

	rc = load_u32(NODE_NVM_KEY_BOOT, &counter, &present);
	if (rc != NODE_IDENT_OK) {
		goto fail;
	}
	if (counter >= BOOT_COUNTER_MAX) {
		rc = NODE_IDENT_ESTATE;
		goto fail;
	}
	counter++;

	/* Write must land before anything uses the value - the 0xF3 refusal,
	 * relocated. If it doesn't stick, this node has not booted. */
	rc = store_u32_confirmed(NODE_NVM_KEY_BOOT, counter);
	if (rc != NODE_IDENT_OK) {
		goto fail;
	}
	s.boot_counter = counter;

	rc = load_u32(NODE_NVM_KEY_PAIR, &s.pair_counter, &present);
	if (rc != NODE_IDENT_OK) {
		goto fail;
	}

	rc = derive_devnum(s.k_dev, &s.devnum);
	if (rc != NODE_IDENT_OK) {
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
	 * was.
	 */
	{
		uint32_t next;

		rc = load_u32(NODE_NVM_KEY_EPOCH, &next, &present);
		if (rc != NODE_IDENT_OK) {
			goto fail;
		}
		clock_valid = node_ident_wall_seconds(&wall);
		s.epoch = node_ident_epoch_from(s.boot_counter, clock_valid,
						wall);
		if (next > s.epoch) {
			s.epoch = next;
		}
		if (s.epoch > EPOCH_BOOT_MAX) {
			rc = NODE_IDENT_ESTATE;
			goto fail;
		}
		rc = store_u32_confirmed(NODE_NVM_KEY_EPOCH, s.epoch + 2u);
		if (rc != NODE_IDENT_OK) {
			goto fail;
		}
		s.epoch_next = s.epoch + 2u;
	}
	s.booted = true;
	return NODE_IDENT_OK;

fail:
	node_ident_reset();
	return rc;
}

const char *node_ident_strerror(int rc)
{
	switch (rc) {
	case NODE_IDENT_OK:
		return "ok";
	case NODE_IDENT_EINVAL:
		return "invalid argument";
	case NODE_IDENT_ENOTSUP:
		return "the crypto backend refused K_dev";
	case NODE_IDENT_ENOKEY:
		return "no K_dev (not provisioned, and none could be generated)";
	case NODE_IDENT_EIO:
		return "identity storage failed";
	case NODE_IDENT_ESTATE:
		return "boot counter exhausted, or identity not booted";
	case NODE_IDENT_EAGAIN:
		return "a fresh epoch is being reserved; retry the arm";
	case NODE_IDENT_EBUSY:
		return "another live channel has this root and device number";
	default:
		return "unknown error";
	}
}

int node_ident_boot_counter(uint32_t *out)
{
	if (out == NULL) {
		return NODE_IDENT_EINVAL;
	}
	if (!s.booted) {
		return NODE_IDENT_ESTATE;
	}
	*out = s.boot_counter;
	return NODE_IDENT_OK;
}

int node_ident_epoch(uint32_t *out)
{
	if (out == NULL) {
		return NODE_IDENT_EINVAL;
	}
	if (!s.booted) {
		return NODE_IDENT_ESTATE;
	}
	*out = s.epoch;
	return NODE_IDENT_OK;
}

int node_ident_pair_counter(uint32_t *out)
{
	if (out == NULL) {
		return NODE_IDENT_EINVAL;
	}
	if (!s.booted) {
		return NODE_IDENT_ESTATE;
	}
	*out = s.pair_counter;
	return NODE_IDENT_OK;
}

int node_ident_devnum(uint16_t *out)
{
	if (out == NULL) {
		return NODE_IDENT_EINVAL;
	}
	if (!s.booted) {
		return NODE_IDENT_ESTATE;
	}
	*out = s.devnum;
	return NODE_IDENT_OK;
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
 * its first wrap). Until then the arm returns NODE_IDENT_EAGAIN and the caller
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
		return NODE_IDENT_ESTATE;
	}
	if (ch_bit(ch) == 0u) {
		return NODE_IDENT_EINVAL;
	}
	if (twin) {
		return NODE_IDENT_EBUSY;
	}
	if (!first_done) {
		*out = s.epoch;
		return NODE_IDENT_OK;
	}
	if (s.arm_fresh == 0u) {
		if (s.epoch_next > EPOCH_BOOT_MAX) {
			return NODE_IDENT_ESTATE;
		}
		s.arm_fresh = s.epoch_next;
	}
	if (s.epoch_next < s.arm_fresh + 2u) {
		epoch_reserve(s.arm_fresh);
		return NODE_IDENT_EAGAIN;
	}
	*out = s.arm_fresh;
	return NODE_IDENT_OK;
}

/* After a successful arm with `epoch`: a fresh epoch is spent. */
static void arm_done(uint32_t epoch)
{
	if (s.arm_fresh != 0u && epoch == s.arm_fresh) {
		s.arm_fresh = 0u;
	}
}

int node_ident_arm_sec(uint8_t ch, uint16_t period_counts)
{
	uint32_t e = 0u;
	int      rc = arm_epoch(s.sec_first_done, ch, radiant_sec_live_twin(ch),
				    &e);

	if (rc != NODE_IDENT_OK) {
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
		return NODE_IDENT_EIO;
	}
	s.sec_first_done = true;
	arm_done(e);
	s.sec_armed |= ch_bit(ch);
	return NODE_IDENT_OK;
}

int node_ident_arm_compat(uint8_t ch, uint64_t now_us, uint16_t period_counts)
{
	uint32_t e = 0u;
	int      rc = arm_epoch(s.compat_first_done, ch,
				    radiant_sec_compat_live_twin(ch), &e);

	if (rc != NODE_IDENT_OK) {
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
		return NODE_IDENT_EIO;
	}
	s.compat_first_done = true;
	arm_done(e);
	s.compat_armed |= ch_bit(ch);
	return NODE_IDENT_OK;
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
		if (store_u32_confirmed(NODE_NVM_KEY_EPOCH, e + 2u) !=
		    NODE_IDENT_OK) {
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

int node_ident_epoch_next(uint32_t *out)
{
	if (out == NULL) {
		return NODE_IDENT_EINVAL;
	}
	if (!s.booted) {
		return NODE_IDENT_ESTATE;
	}
	*out = s.epoch_next;
	return NODE_IDENT_OK;
}

/* ── Pairing ──────────────────────────────────────────────────────────────── */

enum node_ident_scalar_src node_ident_scalar_src(bool host_supplied,
						 bool has_rng)
{
	if (host_supplied) {
		return NODE_IDENT_SCALAR_HOST;
	}
	if (has_rng) {
		return NODE_IDENT_SCALAR_RNG;
	}
	return NODE_IDENT_SCALAR_KDF;
}

/*
 * SP 800-108 counter mode, AES-CMAC as PRF:
 *   block_i = i || "pair" || 0x00 || pair_counter[4 LE] || base_devnum[2 LE] || 0x0100
 * Differs from radiant_sec_kdf()'s block: counter runs 1..2 (not pinned at 1)
 * and [L]_2 = 256 (not 128), so two calls don't repeat the same 16 bytes, and
 * the block can't be confused with the 128-bit "pair" construction.
 */
int node_ident_pair_derive(const uint8_t k_dev[NODE_IDENT_K_DEV_BYTES],
			   uint32_t pair_counter, uint16_t base_devnum,
			   uint8_t out[NODE_IDENT_PAIR_SCALAR_BYTES])
{
	struct radiant_sec_key root;
	uint8_t                block[14];
	uint8_t                tag[RADIANT_SEC_BLOCK_BYTES];
	int                    rc;

	if (k_dev == NULL || out == NULL) {
		return NODE_IDENT_EINVAL;
	}
	rc = radiant_sec_key_import(&root, k_dev, 128u);
	if (rc != RADIANT_SEC_OK) {
		return NODE_IDENT_ENOTSUP;
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
		radiant_sec_wipe(out, NODE_IDENT_PAIR_SCALAR_BYTES);
		return NODE_IDENT_ENOTSUP;
	}
	return NODE_IDENT_OK;
}

int node_ident_pair_window_open(uint32_t *ctr_out)
{
	uint32_t next;
	int      rc;

	if (!s.booted) {
		return NODE_IDENT_ESTATE;
	}
	if (s.pair_counter >= 0xFFFFFFFFu) {
		return NODE_IDENT_ESTATE;
	}
	next = s.pair_counter + 1u;

	/* Advance and persist first; if the write fails, do not enter the
	 * window at all (fail closed). */
	rc = store_u32_confirmed(NODE_NVM_KEY_PAIR, next);
	if (rc != NODE_IDENT_OK) {
		return rc;
	}

	s.pair_counter = next;
	s.scalar_ctr = next;
	s.window_open = true;
	if (ctr_out != NULL) {
		*ctr_out = next;
	}
	return NODE_IDENT_OK;
}

int node_ident_pair_scalar(uint8_t out[NODE_IDENT_PAIR_SCALAR_BYTES])
{
	uint32_t stored = 0u;

	if (out == NULL) {
		return NODE_IDENT_EINVAL;
	}
	if (!s.booted || !s.have_k_dev) {
		return NODE_IDENT_ESTATE;
	}
	if (!s.window_open) {
		return NODE_IDENT_ESTATE;
	}

	/* The gate: the derivation counter must already be in storage (not
	 * cached/queued). `!=` not `<` - a stored counter ahead means some
	 * other path moved it, and deriving under a stale value repeats it. */
	if (node_nvm_load(NODE_NVM_KEY_PAIR, &stored,
			  sizeof(stored)) != NODE_NVM_OK) {
		return NODE_IDENT_EIO;
	}
	if (stored != s.scalar_ctr) {
		return NODE_IDENT_EIO;
	}

	return node_ident_pair_derive(s.k_dev, s.scalar_ctr, s.devnum, out);
}

bool node_ident_pair_window_is_open(void)
{
	return s.window_open;
}

void node_ident_pair_window_close(void)
{
	/* Counter is NOT rolled back: a burnt value costs 1/2^32; a reused one
	 * costs forward secrecy and a stable cross-session identifier. */
	s.window_open = false;
	s.scalar_ctr = 0u;
}

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

int node_ident_pair_begin(uint8_t ch, uint8_t timeout_s)
{
	uint8_t scalar[NODE_IDENT_PAIR_SCALAR_BYTES];
	int     rc;

	if (!s.booted) {
		return NODE_IDENT_ESTATE;
	}
	if (!radiant_sec_caps()->has_x25519) {
		return NODE_IDENT_ENOTSUP;
	}

	/* 1. Counter moves and lands in NVM, or nothing else happens. */
	rc = node_ident_pair_window_open(NULL);
	if (rc != NODE_IDENT_OK) {
		return rc;
	}

	/* 2. Open radiant's pairing context; no key exists yet. */
	if (radiant_sec_pair_enter(ch, timeout_s) != RADIANT_SEC_OK) {
		node_ident_pair_window_close();
		return NODE_IDENT_EIO;
	}

	/* 3. Derive the scalar (gated on step 1 having reached storage). Host
	 * branch unreachable here (no host); RNG branch is left to a build
	 * with caps.has_rng, which fills `scalar` itself after step 1. */
	rc = node_ident_pair_scalar(scalar);
	if (rc != NODE_IDENT_OK) {
		radiant_sec_pair_leave(ch);
		node_ident_pair_window_close();
		return rc;
	}

	/* 4. Install it; this is where the public key is computed. */
	rc = radiant_sec_pair_set_scalar(ch, scalar);
	radiant_sec_wipe(scalar, sizeof(scalar));
	if (rc != RADIANT_SEC_OK) {
		radiant_sec_pair_leave(ch);
		node_ident_pair_window_close();
		return NODE_IDENT_EIO;
	}
	return NODE_IDENT_OK;
}

int node_ident_pair_pubkey(uint8_t ch,
			   uint8_t out[NODE_IDENT_PAIR_SCALAR_BYTES])
{
	if (out == NULL) {
		return NODE_IDENT_EINVAL;
	}
	if (!s.window_open) {
		return NODE_IDENT_ESTATE;
	}
	if (radiant_sec_pair_local_pubkey(ch, out) != RADIANT_SEC_OK) {
		return NODE_IDENT_EIO;
	}
	return NODE_IDENT_OK;
}

#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */

void node_ident_reset(void)
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
