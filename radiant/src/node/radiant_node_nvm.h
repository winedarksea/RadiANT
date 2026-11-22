/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_node_nvm.h - the node application's persistent-storage seam.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md decision 7 (the
 * counters live in the node application, not in radiant).
 *
 * Four fixed-size records, written twice per boot, once per pairing window
 * and about once per epoch counter wrap - small, but the seam earns its keep twice: radiant/tests/
 * fake_nvm.c implements it against RAM with an explicit reboot and an
 * injectable write failure (same rationale as fake_radio.c), and the shipping
 * backend is Zephyr settings over NVS (radiant_node_nvm_settings.c); a node with a
 * secure element would swap in a different backend behind the same seam.
 *
 * No default/RAM-fallback implementation ships: a volatile store here would
 * make the boot counter restart at 1 every power cycle (replayed epoch,
 * repeated pairing scalar) while looking like success. ADR 0009: say so
 * rather than degrade gracefully into a scheme that looks secure - a build
 * with no backend linked fails to link.
 *
 * Durability contract: radiant_node_nvm_store() returns RADIANT_NODE_NVM_OK ONLY when the
 * value would survive an immediate power loss - not queued, not cached for a
 * workqueue to flush. radiant_node_ident.c re-reads via radiant_node_nvm_load() before
 * releasing a derived public key, so a backend that returns early turns that
 * guarantee decorative.
 */

#ifndef RADIANT_NODE_NVM_H_
#define RADIANT_NODE_NVM_H_

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIANT_NODE_NVM_OK       0
#define RADIANT_NODE_NVM_EINVAL  -1
/* Record never written - a first boot, not a failure; the one condition here
 * a caller treats as data. */
#define RADIANT_NODE_NVM_ENOENT  -2
/* Store didn't stick, or load found the wrong size. Fail-closed for every
 * caller. */
#define RADIANT_NODE_NVM_EIO     -3

/* Short names: NVS stores the name with the value, on a flash budget that's
 * the whole argument for this design. */
#define RADIANT_NODE_NVM_KEY_BOOT  "boot"   /* uint32_t, +1 at every boot */
#define RADIANT_NODE_NVM_KEY_PAIR  "pair"   /* uint32_t, +1 per pairing window */
#define RADIANT_NODE_NVM_KEY_KDEV  "kdev"   /* 16 bytes, written once (ADR 0020) */
/* uint32_t, the next epoch nobody has used (docs/radiant-security.md 3.5):
 * written at every boot and about once per counter wrap. Absent on a store
 * from an older image, which reads as 0 - the boot counter is then the floor. */
#define RADIANT_NODE_NVM_KEY_EPOCH "epn"

/* Prepare the backend. Idempotent; radiant_node_ident_boot() calls it first. */
int radiant_node_nvm_init(void);

/* Exact-size accessors only: a load that finds a different size returns
 * RADIANT_NODE_NVM_EIO rather than truncating - a size mismatch means a firmware
 * update redefined the record. */
int radiant_node_nvm_load(const char *key, void *out, size_t len);
int radiant_node_nvm_store(const char *key, const void *val, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_NODE_NVM_H_ */
