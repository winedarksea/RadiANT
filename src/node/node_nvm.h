/* SPDX-License-Identifier: Apache-2.0 */
/*
 * node_nvm.h - the node application's persistent-storage seam.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md, decision 7 ("the
 * counters live in the node application, not in radiant_core") and the
 * "Why this is cheaper than the ratcheting that was deferred" table, whose
 * second row is this header's whole reason for existing.
 *
 * ── Why a seam and not a settings call ─────────────────────────────────────
 *
 * Three records, fixed size, written once per boot and once per pairing window.
 * That is a small enough surface that an interface for it looks like ceremony,
 * and it earns itself twice:
 *
 *   - radiant_core/tests/fake_nvm.c implements this against RAM with an
 *     explicit reboot and an injectable write failure, which is how the boot
 *     counter's persistence and its fail-closed rule get asserted on a board
 *     with no storage partition. Exactly the fake_radio.c arrangement, for
 *     exactly the same reason.
 *   - The shipping backend is the Zephyr settings subsystem over NVS
 *     (src/node/node_nvm_settings.c), and a node with a secure element would
 *     put the counters there instead. ADR 0009 names that as the thing v1 does
 *     not have; a seam is what makes it an added file later.
 *
 * ── A backend is REQUIRED, and its absence is not a soft failure ───────────
 *
 * There is no default implementation and no RAM fallback in the shipping tree.
 * A volatile store behind this interface would be a node whose boot counter
 * restarts at 1 on every power cycle - a replayed epoch and a repeated pairing
 * scalar, which is the precise failure ADR 0009 exists to prevent, arriving
 * silently and looking like success.
 *
 * ADR 0009 says so in as many words: "A node with no NVM at all... cannot do
 * any of this, and it is not a RadiANT security target. Say so rather than
 * degrading gracefully into a scheme that looks secure." A build that links no
 * backend therefore fails to link, which is the loudest available way to say
 * it.
 *
 * ── The durability contract ────────────────────────────────────────────────
 *
 * node_nvm_store() returns NODE_NVM_OK ONLY when the value will survive an
 * immediate power loss. Not "queued", not "written to a cache that a workqueue
 * will flush". Everything above this header treats a successful store as the
 * moment the counter moved, and node_ident.c re-reads through node_nvm_load()
 * before letting a derived public key out, so a backend that returns early
 * turns a structural guarantee into a decorative one.
 */

#ifndef NODE_NVM_H_
#define NODE_NVM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define NODE_NVM_OK       0
#define NODE_NVM_EINVAL  -1
/* The record has never been written. A FIRST BOOT, NOT A FAILURE - the boot
 * counter's absence is how a node recognises its own first power-up, and it is
 * the one condition here that a caller treats as data. */
#define NODE_NVM_ENOENT  -2
/* The store did not stick, or the load found something the wrong size. Both are
 * fail-closed conditions for every caller. */
#define NODE_NVM_EIO     -3

/*
 * The record names. Short, because an NVS-backed settings tree stores the name
 * with the value, and these are written on a device whose flash budget is the
 * argument for the whole design.
 */
#define NODE_NVM_KEY_BOOT  "boot"   /* uint32_t, +1 at every boot */
#define NODE_NVM_KEY_PAIR  "pair"   /* uint32_t, +1 per pairing window */
#define NODE_NVM_KEY_KDEV  "kdev"   /* 16 bytes, provisioned at manufacture */

/*
 * Prepare the backend. Idempotent, and every other call here may assume it has
 * happened - node_ident_boot() makes it first.
 */
int node_nvm_init(void);

/*
 * Exact-size accessors only. `len` must equal the record's size, and a load
 * that finds a different size returns NODE_NVM_EIO rather than a truncation:
 * the only way a size changes is a firmware update that redefined a record, and
 * silently reading half of an old one is how a boot counter comes back smaller
 * than it went in.
 */
int node_nvm_load(const char *key, void *out, size_t len);
int node_nvm_store(const char *key, const void *val, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NODE_NVM_H_ */
