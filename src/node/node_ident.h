/* SPDX-License-Identifier: Apache-2.0 */
/*
 * node_ident.h - the hostless node's identity, epoch and pairing scalar.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md, which is this
 * file's specification in full - the NVM boot counter, K_dev, the deterministic
 * pairing scalar KDF(K_dev, "pair" || pair_counter), the rule that the counter
 * advances before the public key is transmitted, and the caps.has_rng seam. See
 * also docs/decisions/0006 for the identity tiers and docs/radiant-security.md
 * sections 3.5 and 7.4 for the two host-shaped assumptions ADR 0009 amends.
 *
 * ── What this is for ───────────────────────────────────────────────────────
 *
 * A dongle sits behind a PC, and the PC is its epoch authority: 0xF3 advances
 * the epoch, 0xF5 supplies a pairing scalar, and radiant_core need never
 * persist anything. A HEART-RATE STRAP HAS NO PC. It cannot receive 0xF3, so
 * without this file it can never enable a transform, and it has no CSPRNG, so
 * without this file it can never produce a pairing scalar. Both holes close on
 * one thing - state that survives a power cycle - and this is that state.
 *
 * ── Where it lives, and why that is not an accident ────────────────────────
 *
 * OUTSIDE radiant_core, deliberately, and ADR 0009 decision 7 pins it:
 * radiant_core is told an epoch and refuses one that has not advanced, and that
 * is the whole of its job at this boundary. Putting persistence inside it would
 * hand every consumer a flash dependency, a storage backend and a failure mode
 * to serve one class of node.
 *
 * So there is NO new radiant_core entry point for a node-derived epoch. This
 * file calls radiant_sec_set_epoch() and radiant_sec_compat_set_epoch() - the
 * same functions the host adapter calls - with a value it derived itself. The
 * host path is not modified, not deprecated and not wrapped; a node has an
 * epoch authority now, and which one it is depends on which code calls, not on
 * which function exists.
 *
 * ── The one rule most likely to be implemented backwards ───────────────────
 *
 * pair_counter advances, and is DURABLY WRITTEN, before the derived public key
 * leaves the radio. Never on completion of the pairing. Every transactional
 * instinct says commit on success, and here that instinct produces:
 *
 *   - a repeated scalar, hence a repeated X25519 private key, hence no forward
 *     secrecy - one later compromise of K_dev retroactively unlocks every
 *     pairing that shared the scalar rather than just the one; and
 *   - a node that broadcasts an invariant 32-byte public key every time it
 *     opens a pairing window, which is a permanent unique name on the air. ADR
 *     0006 spent a whole decision establishing that a re-rollable identity is
 *     the point; an invariant enrolment key puts the fingerprint back.
 *
 * And the failure would be the COMMON case, not a rare one: pairing windows are
 * abandoned all the time - the user walks away, the peer never answers, the
 * window times out - and an abandoned window never "completes", so advance-on-
 * completion reuses the scalar on the very next attempt.
 *
 * The ordering is enforced structurally rather than by comment. See
 * node_ident_pair_scalar() below.
 */

#ifndef NODE_IDENT_H_
#define NODE_IDENT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* K_dev is one AES key. RADIANT_SEC_KEY_MAX_BYTES is 16 and the KDF's PRF is
 * AES-CMAC, so 128 bits is what a root can be in this project - which is worth
 * stating next to a 32-byte scalar, because the scalar's length is an X25519
 * requirement and not a claim about its entropy. */
#define NODE_IDENT_K_DEV_BYTES        16
#define NODE_IDENT_PAIR_SCALAR_BYTES  32

#define NODE_IDENT_OK        0
#define NODE_IDENT_EINVAL   -1
#define NODE_IDENT_ENOTSUP  -2
/* Not provisioned: there is no K_dev. A device shipped without one has neither
 * a stable identity nor a pairing path, and this is what that looks like from
 * every entry point below. */
#define NODE_IDENT_ENOKEY   -3
/* The counter could not be advanced or could not be persisted. FAIL CLOSED -
 * a node that cannot move its counter has nothing safe to say. */
#define NODE_IDENT_EIO      -4
/* node_ident_boot() has not run, or ran and failed. */
#define NODE_IDENT_ESTATE   -5

/*
 * ── Manufacture ────────────────────────────────────────────────────────────
 *
 * Write K_dev and zero both counters, as one operation. There is deliberately
 * no way to reset a counter on its own: ADR 0009's consequences section pins
 * that "the counters are never reset independently of K_dev", because a reset
 * counter under an unchanged key is a repeated pairing scalar AND a replayed
 * epoch at the same time - the exact pair of failures this whole file exists to
 * prevent.
 *
 * A factory reset is therefore this call with a NEW K_dev, which re-rolls the
 * node's identity (the Tier 0 device number is derived from K_dev) and its
 * counters together. That is the same coupling tools/ant_identity.py's reroll
 * has on the host side, arrived at from the other direction.
 *
 * Refuses an all-zero key, which is what an erased flash region reads as and
 * therefore the one wrong value that would otherwise look provisioned.
 */
int node_ident_provision(const uint8_t k_dev[NODE_IDENT_K_DEV_BYTES]);

bool node_ident_is_provisioned(void);

/*
 * ── Boot ───────────────────────────────────────────────────────────────────
 *
 * Call once, early, before any channel is armed. Advances the boot counter,
 * persists it, and derives this power-up's epoch and the Tier 0 device number.
 *
 * IF THE COUNTER CANNOT BE ADVANCED AND PERSISTED, NO TRANSFORM ENABLES. That
 * is exactly the 0xF3 refusal relocated to the node, and it is the reason this
 * returns a code that callers must check rather than logging and continuing: a
 * node that reuses an epoch reuses keystream, and it does so silently.
 *
 * Returns NODE_IDENT_ENOKEY when unprovisioned, NODE_IDENT_EIO when the store
 * failed, NODE_IDENT_ESTATE when the counter has run out of room (see the
 * headroom note in the .c - a device with 2^32 boots behind it is a device that
 * must be re-provisioned, not one that wraps).
 */
int node_ident_boot(void);

int node_ident_boot_counter(uint32_t *out);
int node_ident_epoch(uint32_t *out);
int node_ident_pair_counter(uint32_t *out);

/*
 * The identity Tier 0 device number, 1..65535, derived from K_dev rather than
 * stored - so a node cannot lose it, and a factory reset re-rolls it. ADR 0009:
 * K_dev "is also the identity Tier 0 device-number source, so it is needed
 * regardless of anything in this ADR".
 *
 * This is also the base device number the key derivation binds, which is why
 * there is one function and not two: on a Tier 0 node the on-air number and the
 * provisioning-time number are the same value, and the day a Tier 2 node
 * re-rolls the first at every power-up it is radiant_sec_set_devnum() that
 * moves, never this.
 */
int node_ident_devnum(uint16_t *out);

/*
 * ── The epoch, and the clock this project does not have ────────────────────
 *
 * Pure, so both branches are testable without a clock and without a reboot.
 *
 * `clock_valid` false - the honest default on every target this project
 * currently builds for - makes the epoch the boot counter. Neither nRF5340 nor
 * nRF54L15 has a battery-backed real-time source across a power cycle; the RTC
 * peripheral counts from reset, which is a measure of uptime and monotone in
 * exactly the way that does not help.
 *
 * `clock_valid` true makes it coarse real time, minutes since
 * NODE_IDENT_EPOCH_BASE_S, which is section 3.5's recommended discharge and is
 * monotone for free. The boot counter is kept as a FLOOR under it - not a mix
 * of two numbering spaces, but a guarantee that a clock which comes back wrong
 * after a battery change cannot hand radiant_core an epoch this node has
 * already used. In practice the floor never binds: minutes since the base date
 * outruns any plausible boot count within the first year.
 *
 * NOTHING MAY INFER ELAPSED REAL TIME FROM THE EPOCH. Where there is no clock
 * it counts power cycles. The attestation counter is the time-derived quantity;
 * this is not.
 */
#define NODE_IDENT_EPOCH_BASE_S 1767225600ull  /* 2026-01-01T00:00:00Z */

uint32_t node_ident_epoch_from(uint32_t boot_counter, bool clock_valid,
			       uint64_t wall_s);

/*
 * The clock, if this node has one. Weak and answering "no", so a build with no
 * real-time source needs no configuration to get the boot-counter epoch, and a
 * node that has one overrides this in its own translation unit.
 */
bool node_ident_wall_seconds(uint64_t *out);

/*
 * ── Handing the epoch to radiant_core ──────────────────────────────────────
 *
 * The existing API, called with a self-derived value. `us_into_epoch` is how
 * far into the epoch this node believes it is - zero at boot, which is the
 * whole of the phase anchoring for a node that just derived its epoch from the
 * power-up it is currently in.
 *
 * Both refuse before node_ident_boot() has succeeded, so the fail-closed rule
 * is not something a caller has to remember to honour.
 */
int node_ident_arm_sec(uint8_t ch, uint16_t period_counts);
int node_ident_arm_compat(uint8_t ch, uint64_t now_us, uint16_t period_counts);

/*
 * ── Pairing ────────────────────────────────────────────────────────────────
 *
 * Three sources of one scalar, so ADR 0009 states a precedence rule once:
 * host-supplied (0xF5) if present, else the RNG when caps.has_rng, else
 * KDF(K_dev, "pair" || pair_counter). ALL THREE ADVANCE pair_counter BEFORE
 * TRANSMISSION - the counter's second job is forward secrecy and its first is
 * not being a fingerprint, and only the first depends on where the bytes came
 * from.
 */
enum node_ident_scalar_src {
	NODE_IDENT_SCALAR_HOST = 0,
	NODE_IDENT_SCALAR_RNG,
	NODE_IDENT_SCALAR_KDF,
};

/* Pure, and it names no backend: `has_rng` is caps.has_rng and nothing here
 * knows whether that bit came from CRACEN, from CryptoCell or from a test. */
enum node_ident_scalar_src node_ident_scalar_src(bool host_supplied,
						 bool has_rng);

/*
 * The derivation itself, pure and taking the counter as an argument - which is
 * the point of the signature. A function that read the counter from NVM would
 * make "is this deterministic in the counter" untestable without a reboot, and
 * would hide the property the security argument rests on.
 *
 * SP 800-108 counter mode with AES-CMAC as the PRF, the same construction
 * radiant_sec_kdf() uses, with [L]_2 = 256 and two iterations because an X25519
 * scalar is 32 bytes and the PRF emits 16. Mirrored by
 * tools/radiant_crypto.py's node_pair_scalar(), and the vectors in
 * radiant_core/tests/src/test_node_ident.c come from that mirror.
 *
 * Not clamped: RFC 7748 clamping happens on a copy inside radiant_sec_x25519(),
 * and a second implementation of that rule here is a second place for it to be
 * wrong.
 */
int node_ident_pair_derive(const uint8_t k_dev[NODE_IDENT_K_DEV_BYTES],
			   uint32_t pair_counter, uint16_t base_devnum,
			   uint8_t out[NODE_IDENT_PAIR_SCALAR_BYTES]);

/*
 * ── The ordering rule, made structural ─────────────────────────────────────
 *
 * node_ident_pair_window_open() increments pair_counter and persists it, and
 * returns a failure if either step does not complete. Nothing is derived and no
 * window is entered on failure.
 *
 * node_ident_pair_scalar() RE-READS THE COUNTER THROUGH node_nvm_load() AND
 * REFUSES unless what is in storage is the counter the scalar was derived from.
 * That is the enforcement: the only route from this node's state to a
 * transmittable public key runs through radiant_sec_pair_set_scalar(), whose
 * only argument is this function's output, and this function cannot answer
 * until the advanced counter is in NVM. Reordering the calls does not defeat
 * it, and neither does forgetting to check a return code, because there is no
 * scalar to forget about.
 *
 * A window that is abandoned is closed with node_ident_pair_window_close(),
 * which does NOT roll the counter back. That is not an oversight either: a
 * burnt counter value is the cheap half of this trade and reusing one is the
 * expensive half.
 */
int node_ident_pair_window_open(uint32_t *ctr_out);
int node_ident_pair_scalar(uint8_t out[NODE_IDENT_PAIR_SCALAR_BYTES]);
bool node_ident_pair_window_is_open(void);
void node_ident_pair_window_close(void);

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
/*
 * The whole sequence for a channel: open the window (counter advances and is
 * persisted), derive, hand the scalar to radiant_core, and enter pairing mode.
 * Any failure leaves the channel not pairing.
 *
 * The order inside is load-bearing and is asserted in the suite: the counter is
 * in NVM before radiant_sec_pair_set_scalar() is called, and
 * radiant_sec_pair_set_scalar() is what computes the public key.
 */
int node_ident_pair_begin(uint8_t ch, uint8_t timeout_s);

/* The public key to put on the air. A thin wrapper over
 * radiant_sec_pair_local_pubkey() that exists so a caller has one obvious route
 * and so the window's bookkeeping is in one place. */
int node_ident_pair_pubkey(uint8_t ch,
			   uint8_t out[NODE_IDENT_PAIR_SCALAR_BYTES]);
#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */

/* Drop all cached state. For tests and for a caller that wants a clean start
 * without touching NVM - it resets nothing persistent, which is the whole
 * point. */
void node_ident_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* NODE_IDENT_H_ */
