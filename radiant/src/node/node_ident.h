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
 * file is that state, kept OUTSIDE radiant (ADR 0009 decision 7):
 * radiant just refuses an epoch that hasn't advanced. It calls the same
 * radiant_sec_set_epoch()/radiant_sec_compat_set_epoch() the host adapter
 * uses, with a self-derived value - no new radiant entry point.
 *
 * THE ORDERING RULE: pair_counter advances and is DURABLY WRITTEN before the
 * derived public key leaves the radio - never on completion of pairing.
 * Committing on completion instead would repeat the scalar (breaking forward
 * secrecy) and would broadcast an invariant public key as a permanent
 * fingerprint (defeating ADR 0006's re-rollable identity) - and this is the
 * common case, since abandoned pairing windows never "complete". Enforced
 * structurally, not by comment - see node_ident_pair_scalar() below.
 *
 * THREAD SAFETY: node_ident_arm_sec(), _arm_compat(), _pair_begin(),
 * _pair_pubkey(), _pair_scalar() and _pair_derive() reach either lock-checked
 * radiant_sec entry points or the shared AES key schedule
 * (radiant_sec_aes.c, not re-entrant). On a build that links
 * ant_radio_radiant.c they must run under api_lock, which the RX pump also
 * takes. Apps cannot take that lock (it is port-private), so the first caller
 * adds a locked antr_* wrapper in radiant/src/antr/, following the antr_sec_*
 * pattern in ant_sec_host.c: validate, lock, call a _locked body, unlock.
 * No app calls these today.
 *
 * node_ident_boot() is the exception: it runs the AES schedule unlocked,
 * which is safe only while no channel is keyed (the pump does no AES on an
 * unkeyed channel). Keep it before any channel is secured.
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
/* Not yet: an arm after the first needs a fresh epoch, and its reservation is
 * still on its way to storage. Retry (the work queue lands it in milliseconds
 * when storage is healthy). */
#define NODE_IDENT_EAGAIN   -6
/* Refused: another channel of the same kind is live under the same root and
 * base device number (see the epoch reservation below). Release it, or use a
 * different root or device number. */
#define NODE_IDENT_EBUSY    -7

/*
 * ── Provisioning ───────────────────────────────────────────────────────────
 *
 * Called by node_ident_boot() itself on a first boot when
 * CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION is on (ADR 0020), or by a factory
 * step that writes its own key.
 *
 * Writes K_dev and zeroes both counters as one operation; there is no way to
 * reset a counter on its own (ADR 0009: counters are never reset independently
 * of K_dev, since that would replay the epoch and repeat the pairing scalar).
 * A factory reset is this call with a new K_dev, re-rolling identity (Tier 0
 * device number derives from K_dev) and counters together.
 *
 * Safe to retry: called with the K_dev already installed, it keeps the boot
 * counter, pairing counter and epoch reservation, creating at zero only those
 * an interrupted first provisioning never wrote. A K_dev read that fails for
 * any reason but "never written" returns NODE_IDENT_EIO and writes nothing -
 * so a corrupt K_dev record (wrong size, read error) makes provisioning fail
 * closed instead of overwriting it.
 *
 * The same rule applies to a new-key provisioning interrupted after its key
 * write: retrying with that key keeps the old counters, so it cannot clear an
 * exhausted boot counter (NODE_IDENT_ESTATE from node_ident_boot()). Use a
 * different key for that.
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
 * With CONFIG_RADIANT_NODE_IDENT_SELF_PROVISION (ADR 0020), a boot that finds
 * NO K_dev record - the storage backend's NODE_NVM_ENOENT, and nothing else -
 * first generates one from radiant_sec_rng() and provisions it through
 * node_ident_provision(), then boots normally. Any other K_dev load failure is
 * NODE_IDENT_EIO and provisions nothing: a flaky read must never re-roll the
 * identity.
 *
 * Returns NODE_IDENT_ENOKEY when unprovisioned (or when self-provisioning had
 * no usable entropy), NODE_IDENT_EIO on a storage failure, NODE_IDENT_ESTATE
 * when the counter has run out of room (see the headroom note in the .c). On
 * any failure the caller falls back to its Kconfig device number with no
 * transforms, and should say so with node_ident_strerror().
 */
int node_ident_boot(void);

/* A fixed English reason for a NODE_IDENT_* code, for the one boot log line
 * every node application prints on failure. Never NULL. */
const char *node_ident_strerror(int rc);

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
 * clock that comes back wrong after a battery change can't hand radiant
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
 * ── Handing the epoch to radiant ──────────────────────────────────────
 *
 * The existing API, called with a self-derived value. `us_into_epoch` is zero
 * at boot (the node just derived its epoch from the current power-up). Both
 * refuse before node_ident_boot() has succeeded. Need api_lock: see THREAD
 * SAFETY at the top of this file.
 */
int node_ident_arm_sec(uint8_t ch, uint16_t period_counts);
int node_ident_arm_compat(uint8_t ch, uint64_t now_us, uint16_t period_counts);

/*
 * ── The epoch reservation (docs/radiant-security.md 3.5) ──────────────────
 *
 * A counter wrap moves a channel to epoch + 1 with no boot in between, so the
 * boot counter alone would hand the next power-up an epoch already used. The
 * node therefore persists N, the next epoch nobody has used:
 *
 *   boot:  E = max(N, boot counter, wall minutes); persist N = E + 2 (confirmed)
 *          before E is used. node_ident_epoch() is E.
 *   arm:   the channel gets E and an epoch ceiling of N - 1 = E + 1, so the
 *          first wrap is already reserved.
 *   wrap:  radiant calls radiant_sec_epoch_advanced() (compat: the compat
 *          twin); this file's override only queues a work item on the system
 *          work queue, which persists N = e + 2 and then raises every armed
 *          channel's ceiling to N - 1. A second wrap before that lands is
 *          withheld by radiant, not taken - the write has one whole wrap,
 *          ~4.5 h at 4 Hz, to land, and is retried every second if it fails.
 *
 *   gap:   a sender silent for more than one wrap wants an epoch past the
 *          ceiling; radiant_sec_epoch_wanted() reserves it the same way.
 *   re-arm: only the FIRST arm of each kind in a boot gets E. Every later
 *          arm - the same channel again after an unassign or re-key, or
 *          another channel - gets the next unused epoch F = N, and returns
 *          NODE_IDENT_EAGAIN until N = F + 2 is in storage; the caller
 *          retries. No arm ever restarts the counter under an epoch an
 *          earlier arm in this boot used.
 *
 * Epochs therefore step by about 2 per boot. One extra NVM write per boot and
 * about one per wrap. These overrides make node_ident.c the owner of the
 * epoch hooks in any image that links it; only channels armed through
 * node_ident_arm_sec()/_arm_compat() are followed, and only while radiant
 * still holds their ceiling. While it does, radiant refuses anyone else's
 * set_epoch on the channel (a host 0xF3 gets RADIANT_SEC_ESTATE, which the
 * host adapter answers as CHANNEL_IN_WRONG_STATE); a new root (set_key) or a
 * release ends the following. Channel numbers 32 and up are refused (EINVAL).
 *
 * LIVE CHANNELS MUST DIFFER IN ROOT OR BASE DEVICE NUMBER, AND THE ARM
 * ENFORCES IT. Arms get distinct epochs, but a wrap on one live channel may
 * reach an epoch a later arm took (A at E, B at E + 2, A's second wrap lands
 * on E + 2), and a channel a host epoched at E collides with no wrap at all.
 * So an arm returns NODE_IDENT_EBUSY while another channel of the same kind
 * is running - an epoch set or this node's ceiling carried, whoever set it -
 * under the same root (by key check value) and the same base device number
 * (review 2026-11-14 item 2). Keyed with no epoch yet does not count.
 * Channels that differ in root or number arm freely; the same channel
 * re-armed is the re-arm rule above. Releasing the other channel lets the arm
 * through: that is unassign (antr_channel_unassign() releases both the
 * radiant_sec and the compat context) or a SYSTEM_RESET, not a close - a
 * closed but assigned channel keeps blocking. A host replacing its root does
 * too.
 * Two device types on one root and one device number cannot both be live:
 * that would need the device type in the KDF or the nonce.
 *
 * node_ident_epoch_next() reports N as last confirmed in storage.
 */
int node_ident_epoch_next(uint32_t *out);

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
 * in radiant/tests/src/test_node_ident.c come from that mirror.
 *
 * Not clamped: RFC 7748 clamping happens in radiant_sec_x25519() - a second
 * implementation here would be a second place for it to be wrong. The thread
 * that then pairs with the scalar needs RADIANT_SEC_PAIR_STACK_BYTES
 * (radiant_sec.h) of stack below its own frames.
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
 * persisted), derive, hand the scalar to radiant, enter pairing mode. Any
 * failure leaves the channel not pairing. Order is load-bearing and asserted
 * in the suite: counter is in NVM before radiant_sec_pair_set_scalar() (which
 * computes the public key) is called. Needs api_lock: see THREAD SAFETY at
 * the top of this file.
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
