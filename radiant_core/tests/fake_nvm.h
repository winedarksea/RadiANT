/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fake_nvm.h - the control surface for the test image's storage backend.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md and
 * src/node/node_nvm.h, whose interface this implements. Derived from this
 * project's own documents; no external specification.
 *
 * fake_nvm.c is to node_nvm.h what fake_radio.c is to radiant_radio_hal.h: the
 * one implementation of the seam in this image, so the suites drive the real
 * node_ident.c rather than a copy of it, on a board that has no storage
 * partition.
 *
 * It models one thing the real backend cannot be asked to do on demand, and the
 * phase turns on it: A POWER CYCLE. fake_nvm_reboot() throws away nothing -
 * that is the point. A boot counter is only a boot counter if it survives one,
 * and a test that cannot cause one can only assert that a number went up.
 */

#ifndef FAKE_NVM_H_
#define FAKE_NVM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Erase the simulated part and forget it was ever initialised. A factory-fresh
 * device, which is where most of these tests start. */
void fake_nvm_wipe(void);

/*
 * A power cycle. The contents survive; the "have I been initialised" flag does
 * not, so the next node_nvm_init() is a real one.
 */
void fake_nvm_reboot(void);

/*
 * Make the next `n` stores fail, as a flash write that did not take. Zero turns
 * it off. This is what makes the fail-closed rules assertable: ADR 0009 says a
 * node that cannot advance and persist its counter does not enter the pairing
 * window and enables no transform, and "cannot persist" has to be producible on
 * demand or that rule is only ever a comment.
 */
void fake_nvm_fail_stores(uint32_t n);

/*
 * Sever the store from the load: the value is reported as written and is not.
 * Nothing in a correct backend does this; it exists so that node_ident.c's
 * confirm-by-re-reading is exercised rather than assumed dead code, and so a
 * future backend that returns early from a queued write is caught by a test
 * that already exists.
 */
void fake_nvm_swallow_stores(bool on);

/* Read a record straight out of the simulated part, bypassing node_nvm.h.
 * A suite asserting "the counter is in NVM by now" must not ask the layer it is
 * testing whether it is. Returns false when the record is absent. */
bool fake_nvm_peek_u32(const char *key, uint32_t *out);

/* How many stores have been attempted, for asserting write frequency - ADR
 * 0009's flash-wear argument is "once per boot, once per pairing window", and
 * this is what turns that into an assertion. */
uint32_t fake_nvm_store_count(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_NVM_H_ */
