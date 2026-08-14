/* SPDX-License-Identifier: Apache-2.0 */
/*
 * node_nvm.h - the node application's persistent-storage seam.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md decision 7 (the
 * counters live in the node application, not in radiant).
 *
 * Three fixed-size records, written once per boot and once per pairing
 * window - small, but the seam earns its keep twice: radiant/tests/
 * fake_nvm.c implements it against RAM with an explicit reboot and an
 * injectable write failure (same rationale as fake_radio.c), and the shipping
 * backend is Zephyr settings over NVS (node_nvm_settings.c); a node with a
 * secure element would swap in a different backend behind the same seam.
 *
 * No default/RAM-fallback implementation ships: a volatile store here would
 * make the boot counter restart at 1 every power cycle (replayed epoch,
 * repeated pairing scalar) while looking like success. ADR 0009: say so
 * rather than degrade gracefully into a scheme that looks secure - a build
 * with no backend linked fails to link.
 *
 * Durability contract: node_nvm_store() returns NODE_NVM_OK ONLY when the
 * value would survive an immediate power loss - not queued, not cached for a
 * workqueue to flush. node_ident.c re-reads via node_nvm_load() before
 * releasing a derived public key, so a backend that returns early turns that
 * guarantee decorative.
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
/* Record never written - a first boot, not a failure; the one condition here
 * a caller treats as data. */
#define NODE_NVM_ENOENT  -2
/* Store didn't stick, or load found the wrong size. Fail-closed for every
 * caller. */
#define NODE_NVM_EIO     -3

/* Short names: NVS stores the name with the value, on a flash budget that's
 * the whole argument for this design. */
#define NODE_NVM_KEY_BOOT  "boot"   /* uint32_t, +1 at every boot */
#define NODE_NVM_KEY_PAIR  "pair"   /* uint32_t, +1 per pairing window */
#define NODE_NVM_KEY_KDEV  "kdev"   /* 16 bytes, provisioned at manufacture */

/* Prepare the backend. Idempotent; node_ident_boot() calls it first. */
int node_nvm_init(void);

/* Exact-size accessors only: a load that finds a different size returns
 * NODE_NVM_EIO rather than truncating - a size mismatch means a firmware
 * update redefined the record. */
int node_nvm_load(const char *key, void *out, size_t len);
int node_nvm_store(const char *key, const void *val, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* NODE_NVM_H_ */
