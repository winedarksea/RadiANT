/* SPDX-License-Identifier: Apache-2.0 */
/*
 * node_ident.h - the hostless node's identity, epoch and pairing scalar.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md (NVM boot counter,
 * K_dev, pairing scalar KDF(K_dev, "pair" || pair_counter), counter-advances-
 * before-transmit rule, caps.has_rng seam). See also ADR 0006 (identity tiers)
 * and docs/radiant-security.md sections 3.5, 7.4.
 *
 * A dongle has a PC as epoch authority (0xF3/0xF5); a heart-rate strap has
 * none, so it needs its own persisted epoch and pairing-scalar state. This
 * file is that state, kept OUTSIDE radiant_core (ADR 0009 decision 7):
 * radiant_core just refuses an epoch that hasn't advanced. It calls the same
 * radiant_sec_set_epoch()/radiant_sec_compat_set_epoch() the host adapter
 * uses, with a self-derived value - no new radiant_core entry point.
 *
 * THE ORDERING RULE: pair_counter advances and is DURABLY WRITTEN before the
 * derived public key leaves the radio - never on completion of pairing.
 * Committing on completion instead would repeat the scalar (breaking forward
 * secrecy) and would broadcast an invariant public key as a permanent
 * fingerprint (defeating ADR 0006's re-rollable identity) - and this is the
 * common case, since abandoned pairing windows never "complete". Enforced
 * structurally, not by comment - see node_ident_pair_scalar() below.
 */

#ifndef NODE_IDENT_H_
#define NODE_IDENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* K_dev is one 128-bit AES key (RADIANT_SEC_KEY_MAX_BYTES, AES-CMAC PRF); the
 * 32-byte scalar length is an X25519 requirement, not an entropy claim. */
#define NODE_IDENT_K_DEV_BYTES        16
#define NODE_IDENT_PAIR_SCALAR_BYTES  32

#define NODE_IDENT_OK        0
#define NODE_IDENT_EINVAL   -1
#define NODE_IDENT_ENOTSUP  -2
/* Not provisioned: no K_dev, hence no stable identity or pairing path. */
#define NODE_IDENT_ENOKEY   -3
/* Counter could not be advanced/persisted. Fail closed. */
#define NODE_IDENT_EIO      -4
/* node_ident_boot() has not run, or ran and failed. */
#define NODE_IDENT_ESTATE   -5

/*
 * ── Manufacture ────────────────────────────────────────────────────────────
 *
 * Writes K_dev and zeroes both counters as one operation; there is no way to
 * reset a counter on its own (ADR 0009: counters are never reset independently
 * of K_dev, since that would replay the epoch and repeat the pairing scalar).
 * A factory reset is this call with a new K_dev, re-rolling identity (Tier 0
 * device number derives from K_dev) and counters together.
 *
 * Refuses an all-zero key (what erased flash reads as, and would otherwise
 * look provisioned).
 */
int node_ident_provision(const uint8_t k_dev[NODE_IDENT_K_DEV_BYTES]);

bool node_ident_is_provisioned(void);

/*
 * ── Boot ───────────────────────────────────────────────────────────────────
 *
 * Call once, early, before any channel is armed. Advances and persists the
 * boot counter, derives this power-up's epoch and the Tier 0 device number.
 *
 * If the counter can't be advanced and persisted, no transform enables (the
 * 0xF3 refusal, relocated) - callers must check the return code, since a
 * reused epoch reuses keystream silently.
 *
 * Returns NODE_IDENT_ENOKEY when unprovisioned, NODE_IDENT_EIO on store
 * failure, NODE_IDENT_ESTATE when the counter has run out of room (see the
 * headroom note in the .c).
 */
int node_ident_boot(void);

int node_ident_boot_counter(uint32_t *out);
int node_ident_epoch(uint32_t *out);
int node_ident_pair_counter(uint32_t *out);

/*
 * The identity Tier 0 device number, 1..65535, derived from K_dev rather than
 * stored - so a node can't lose it, and a factory reset re-rolls it. Also the
 * base device number the key derivation binds; on a Tier 0 node the on-air and
 * provisioning-time numbers are the same value, which is why there is one
 * function and not two (a Tier 2 node re-rolling the on-air number would move
 * radiant_sec_set_devnum(), never this).
 */
int node_ident_devnum(uint16_t *out);

/*
 * ── The epoch, and the clock this project does not have ────────────────────
 *
 * Pure, so both branches are testable without a clock or reboot.
 *
 * `clock_valid` false (the honest default on every current target - neither
 * nRF5340 nor nRF54L15 has a battery-backed RTC across a power cycle) makes
 * the epoch the boot counter.
 *
 * `clock_valid` true makes it minutes since NODE_IDENT_EPOCH_BASE_S (section
 * 3.5's recommended discharge). The boot counter stays a FLOOR under it, so a
 * clock that comes back wrong after a battery change can't hand radiant_core
 * an already-used epoch; in practice the floor never binds.
 *
 * Nothing may infer elapsed real time from the epoch - with no clock it just
 * counts power cycles. The attestation counter is the time-derived quantity;
 * this is not.
 */
#define NODE_IDENT_EPOCH_BASE_S 1767225600ull  /* 2026-01-01T00:00:00Z */

uint32_t node_ident_epoch_from(uint32_t boot_counter, bool clock_valid,
			       uint64_t wall_s);

/* The clock, if this node has one. Weak, answers "no" by default, so a build
 * with no real-time source needs no configuration; a node with one overrides
 * this in its own translation unit. */
bool node_ident_wall_seconds(uint64_t *out);

/*
 * ── Handing the epoch to radiant_core ──────────────────────────────────────
 *
 * The existing API, called with a self-derived value. `us_into_epoch` is zero
 * at boot (the node just derived its epoch from the current power-up). Both
 * refuse before node_ident_boot() has succeeded.
 */
int node_ident_arm_sec(uint8_t ch, uint16_t period_counts);
int node_ident_arm_compat(uint8_t ch, uint64_t now_us, uint16_t period_counts);

/*
 * ── Pairing ────────────────────────────────────────────────────────────────
 *
 * Three sources of one scalar (ADR 0009 precedence): host-supplied (0xF5) if
 * present, else RNG when caps.has_rng, else KDF(K_dev, "pair" || pair_counter).
 * All three advance pair_counter before transmission.
 */
enum node_ident_scalar_src {
	NODE_IDENT_SCALAR_HOST = 0,
	NODE_IDENT_SCALAR_RNG,
	NODE_IDENT_SCALAR_KDF,
};

/* Pure; names no backend - `has_rng` is caps.has_rng regardless of source. */
enum node_ident_scalar_src node_ident_scalar_src(bool host_supplied,
						 bool has_rng);

/*
 * Pure, taking the counter as an argument rather than reading NVM, so
 * determinism-in-the-counter is testable without a reboot.
 *
 * SP 800-108 counter mode, AES-CMAC PRF (same construction as
 * radiant_sec_kdf()), [L]_2 = 256, two iterations (32-byte scalar, 16-byte PRF
 * output). Mirrored by tools/radiant_crypto.py's node_pair_scalar(); vectors
 * in radiant_core/tests/src/test_node_ident.c come from that mirror.
 *
 * Not clamped: RFC 7748 clamping happens in radiant_sec_x25519() - a second
 * implementation here would be a second place for it to be wrong.
 */
int node_ident_pair_derive(const uint8_t k_dev[NODE_IDENT_K_DEV_BYTES],
			   uint32_t pair_counter, uint16_t base_devnum,
			   uint8_t out[NODE_IDENT_PAIR_SCALAR_BYTES]);

/*
 * ── The ordering rule, made structural ─────────────────────────────────────
 *
 * node_ident_pair_window_open() increments and persists pair_counter,
 * returning failure (deriving nothing, entering no window) if either step
 * fails.
 *
 * node_ident_pair_scalar() re-reads the counter via node_nvm_load() and
 * refuses unless storage matches the counter the scalar was derived from - so
 * it cannot answer until the advanced counter is in NVM, and there is no
 * scalar to reorder around or forget to check.
 *
 * node_ident_pair_window_close() does NOT roll the counter back: a burnt
 * value is cheap, a reused one is not.
 */
int node_ident_pair_window_open(uint32_t *ctr_out);
int node_ident_pair_scalar(uint8_t out[NODE_IDENT_PAIR_SCALAR_BYTES]);
bool node_ident_pair_window_is_open(void);
void node_ident_pair_window_close(void);

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
/*
 * The whole sequence for a channel: open the window (counter advances and is
 * persisted), derive, hand the scalar to radiant_core, enter pairing mode. Any
 * failure leaves the channel not pairing. Order is load-bearing and asserted
 * in the suite: counter is in NVM before radiant_sec_pair_set_scalar() (which
 * computes the public key) is called.
 */
int node_ident_pair_begin(uint8_t ch, uint8_t timeout_s);

/* The public key to put on the air; thin wrapper over
 * radiant_sec_pair_local_pubkey(). */
int node_ident_pair_pubkey(uint8_t ch,
			   uint8_t out[NODE_IDENT_PAIR_SCALAR_BYTES]);
#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */

/* Drop all cached state; touches nothing persistent. For tests / clean
 * restart. */
void node_ident_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* NODE_IDENT_H_ */
