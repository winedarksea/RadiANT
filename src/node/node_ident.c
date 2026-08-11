/* SPDX-License-Identifier: Apache-2.0 */
/*
 * node_ident.c - the hostless node's identity, epoch and pairing scalar.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md. See node_ident.h
 * for the design and for the one rule most likely to be implemented backwards.
 *
 * This file calls radiant_core; nothing in radiant_core knows it exists. The
 * dependency runs application -> module, which is the direction
 * docs/decisions/0002-clean-room-policy.md and the module boundary both already
 * permit.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/kernel.h>   /* __weak, for the clock hook a node overrides */

#include <radiant_core/radiant_sec.h>
#include <radiant_core/radiant_sec_compat.h>

#include "node_ident.h"
#include "node_nvm.h"

/* ── State, all of it derived and none of it authoritative ─────────────────
 *
 * Every field here is either read from NVM at boot or computed from something
 * that was. The authority is the storage; this is a cache with an explicit
 * validity flag so that "not booted" and "booted to zero" cannot be confused.
 */
static struct {
	bool     booted;
	uint32_t boot_counter;
	uint32_t epoch;
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
 * 0xFFFFFFFF so a counter wrap always has room to advance into. The node is the
 * epoch authority now, so the node is what must stay inside that bound - and it
 * must refuse rather than wrap, because a wrapped boot counter is a replayed
 * epoch and there is no error the radio layer could report that would be worse
 * than that being silent.
 *
 * A device that reaches this has booted about four billion times. It needs
 * re-provisioning, which is a new K_dev and therefore a new identity, and that
 * is the correct answer rather than a harsh one.
 */
#define BOOT_COUNTER_MAX (0xFFFFFFFFu - RADIANT_SEC_EPOCH_HEADROOM - 1u)

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

/*
 * Store a counter and confirm it before returning success.
 *
 * The re-read is not paranoia about flash: it is what makes the seam's
 * durability contract checkable by the layer that depends on it, and it is the
 * same call node_ident_pair_scalar() makes before releasing a scalar. A backend
 * that returns OK from a store and then cannot produce the value is caught
 * here, at the write, rather than one power cycle later.
 */
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

/* ── Manufacture ──────────────────────────────────────────────────────────── */

int node_ident_provision(const uint8_t k_dev[NODE_IDENT_K_DEV_BYTES])
{
	uint8_t back[NODE_IDENT_K_DEV_BYTES];
	int     rc;

	if (k_dev == NULL) {
		return NODE_IDENT_EINVAL;
	}
	/* An erased flash region does not read as zero on every part, but a
	 * zeroed buffer that never got filled does, and that is the failure
	 * mode worth refusing: a node whose "secret" is sixteen zero bytes has
	 * a pairing scalar and a device number anyone can compute. */
	if (key_is_all_zero(k_dev, NODE_IDENT_K_DEV_BYTES)) {
		return NODE_IDENT_EINVAL;
	}
	if (node_nvm_init() != NODE_NVM_OK) {
		return NODE_IDENT_EIO;
	}

	/*
	 * Key first, then the counters. The order matters on an interrupted
	 * provisioning: a new key with old counters is safe - the scalars and
	 * epochs under it have never been used - whereas zeroed counters under
	 * the OLD key is precisely the reset-counter-under-an-unchanged-key
	 * condition ADR 0009's consequences section forbids.
	 */
	if (node_nvm_store(NODE_NVM_KEY_KDEV, k_dev,
			   NODE_IDENT_K_DEV_BYTES) != NODE_NVM_OK) {
		return NODE_IDENT_EIO;
	}
	if (node_nvm_load(NODE_NVM_KEY_KDEV, back,
			  NODE_IDENT_K_DEV_BYTES) != NODE_NVM_OK) {
		return NODE_IDENT_EIO;
	}
	if (memcmp(back, k_dev, NODE_IDENT_K_DEV_BYTES) != 0) {
		memset(back, 0, sizeof(back));
		return NODE_IDENT_EIO;
	}
	memset(back, 0, sizeof(back));

	rc = store_u32_confirmed(NODE_NVM_KEY_BOOT, 0u);
	if (rc != NODE_IDENT_OK) {
		return rc;
	}
	rc = store_u32_confirmed(NODE_NVM_KEY_PAIR, 0u);
	if (rc != NODE_IDENT_OK) {
		return rc;
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
	memset(k, 0, sizeof(k));
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
	/* The epoch-less "id" label, epoch 0, base_devnum 0 - there is no base
	 * device number yet, because this call is what produces it. */
	rc = radiant_sec_kdf(&root, RADIANT_SEC_LABEL_ID, 0u, 0u, &id);
	radiant_sec_key_destroy(&root);
	if (rc != RADIANT_SEC_OK) {
		radiant_sec_key_destroy(&id);
		return NODE_IDENT_ENOTSUP;
	}

	raw = (uint16_t)((uint16_t)id.b[0] | ((uint16_t)id.b[1] << 8));
	radiant_sec_key_destroy(&id);

	/*
	 * 1..65535: 0 is the ANT wildcard and a node must never claim it.
	 * Reduction rather than rejection sampling, because a node deriving
	 * this at every boot cannot afford an unbounded retry loop, and the
	 * 1-in-65535 bias this introduces is invisible against a 16-bit space
	 * whose collision bound is ~300 devices anyway
	 * (tools/ant_identity.py, COLLISION_LIKELY_AROUND).
	 */
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
	/* The boot counter is a FLOOR, not a second numbering space: a clock
	 * that comes back wrong after a battery change must not be able to
	 * hand radiant_core an epoch this node has already used. */
	if (minutes < (uint64_t)boot_counter) {
		return boot_counter;
	}
	return (uint32_t)minutes;
}

int node_ident_boot(void)
{
	uint64_t wall = 0u;
	bool     clock_valid;
	bool     present;
	uint32_t counter;
	int      rc;

	node_ident_reset();

	if (node_nvm_init() != NODE_NVM_OK) {
		return NODE_IDENT_EIO;
	}
	if (node_nvm_load(NODE_NVM_KEY_KDEV, s.k_dev,
			  sizeof(s.k_dev)) != NODE_NVM_OK) {
		return NODE_IDENT_ENOKEY;
	}
	if (key_is_all_zero(s.k_dev, sizeof(s.k_dev))) {
		memset(s.k_dev, 0, sizeof(s.k_dev));
		return NODE_IDENT_ENOKEY;
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

	/*
	 * THE WRITE COMES BEFORE ANYTHING USES THE VALUE. If it does not stick,
	 * this node has not booted as far as anything above is concerned, and
	 * no transform enables - the 0xF3 refusal, relocated.
	 */
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

	clock_valid = node_ident_wall_seconds(&wall);
	s.epoch = node_ident_epoch_from(s.boot_counter, clock_valid, wall);
	s.booted = true;
	return NODE_IDENT_OK;

fail:
	node_ident_reset();
	return rc;
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

/* ── Handing the epoch to radiant_core ────────────────────────────────────── */

int node_ident_arm_sec(uint8_t ch, uint16_t period_counts)
{
	if (!s.booted) {
		return NODE_IDENT_ESTATE;
	}
	/*
	 * us_into_epoch = 0. The epoch was derived from THIS power-up moments
	 * ago, so the node is at the start of it by construction - which is the
	 * one thing a hostless node knows about its own phase and a host never
	 * does.
	 */
	if (radiant_sec_set_epoch(ch, s.epoch, 0u, period_counts) !=
	    RADIANT_SEC_OK) {
		return NODE_IDENT_EIO;
	}
	return NODE_IDENT_OK;
}

int node_ident_arm_compat(uint8_t ch, uint64_t now_us, uint16_t period_counts)
{
	if (!s.booted) {
		return NODE_IDENT_ESTATE;
	}
	if (radiant_sec_compat_set_epoch(ch, s.epoch, now_us, 0u,
					 period_counts) != RADIANT_SEC_OK) {
		return NODE_IDENT_EIO;
	}
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
 * SP 800-108 counter mode, AES-CMAC as the PRF:
 *
 *   block_i = i || "pair" || 0x00 || pair_counter[4 LE] || base_devnum[2 LE]
 *             || 0x0100
 *
 * radiant_sec_kdf()'s block with two fields changed, and both changes have to
 * be here rather than there: the SP 800-108 counter runs 1..2 instead of being
 * pinned at 1, and [L]_2 is 256 rather than 128. Calling a 128-bit KDF twice
 * with an unchanged block would return the same sixteen bytes twice, which is
 * the failure mode this layout exists to make impossible - and because [L]_2 is
 * inside the PRF input, the first block here is not the 128-bit "pair" key
 * either, so the two constructions cannot be confused by coincidence.
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
	memset(tag, 0, sizeof(tag));
	memset(block, 0, sizeof(block));
	if (rc != RADIANT_SEC_OK) {
		memset(out, 0, NODE_IDENT_PAIR_SCALAR_BYTES);
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

	/*
	 * ADVANCE AND PERSIST FIRST. Everything else in this function is
	 * bookkeeping; this line is the decision. If the write fails the node
	 * does not enter the pairing window at all - fail closed, because a
	 * node that cannot advance its counter has nothing safe to say.
	 */
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

	/*
	 * THE GATE. The counter this scalar will be derived from must already
	 * be the counter in storage - not in a cache, not queued, in storage as
	 * node_nvm.h defines it. A caller cannot reach a public key without
	 * coming through here, so "the counter advanced before the key went on
	 * the air" is a property of the call graph rather than of somebody's
	 * memory.
	 *
	 * `!=` rather than `<`: a stored counter AHEAD of the one this scalar
	 * was derived from means some other path moved it, and deriving under
	 * a stale value would be exactly the repeat this guards against.
	 */
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
	/* The counter is NOT rolled back. A burnt value costs one number out of
	 * 2^32; a reused one costs forward secrecy and hands out a stable
	 * cross-session identifier. */
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

	/* 1. The counter moves and lands in NVM, or nothing else happens. */
	rc = node_ident_pair_window_open(NULL);
	if (rc != NODE_IDENT_OK) {
		return rc;
	}

	/*
	 * 2. radiant_core's pairing context has to exist before a scalar can be
	 *    installed in it, and entering it is not what puts anything on the
	 *    air - radiant_sec_pair_enter() only opens the state. The public key
	 *    comes into existence in step 4.
	 */
	if (radiant_sec_pair_enter(ch, timeout_s) != RADIANT_SEC_OK) {
		node_ident_pair_window_close();
		return NODE_IDENT_EIO;
	}

	/*
	 * 3. The scalar, which the gate in node_ident_pair_scalar() refuses to
	 *    produce unless step 1 actually reached storage.
	 *
	 *    ADR 0009's precedence rule is host-supplied, else RNG, else KDF.
	 *    The host branch is not reachable here by construction - a node
	 *    calling this file has no host - and the RNG branch is left to the
	 *    build that has one: with caps.has_rng set, a node application
	 *    fills `scalar` from radiant_sec_rng() instead, and step 1 is
	 *    unchanged, which is the entire point of doing it first.
	 */
	rc = node_ident_pair_scalar(scalar);
	if (rc != NODE_IDENT_OK) {
		radiant_sec_pair_leave(ch);
		node_ident_pair_window_close();
		return rc;
	}

	/* 4. Install it. This is where the public key is computed, and the
	 *    counter has been durable since step 1. */
	rc = radiant_sec_pair_set_scalar(ch, scalar);
	memset(scalar, 0, sizeof(scalar));
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
	memset(&s, 0, sizeof(s));
}
