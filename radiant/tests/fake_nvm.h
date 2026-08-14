/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fake_nvm.h - the control surface for the test image's storage backend.
 *
 * Provenance: docs/decisions/0009-hostless-node-identity.md and
 * src/node/node_nvm.h, whose interface this implements.
 *
 * fake_nvm.c is to node_nvm.h what fake_radio.c is to radiant_radio_hal.h:
 * the one implementation of the seam in this image, so suites drive the real
 * node_ident.c on a board that has no storage partition.
 *
 * It models a power cycle, which the real backend cannot be asked to do on
 * demand: fake_nvm_reboot() throws away nothing. A boot counter is only a
 * boot counter if it survives one.
 */

#ifndef FAKE_NVM_H_
#define FAKE_NVM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Erase the simulated part and forget it was ever initialised: a
 * factory-fresh device, where most of these tests start. */
void fake_nvm_wipe(void);

/* A power cycle: contents survive, but the "initialised" flag does not, so
 * the next node_nvm_init() is a real one. */
void fake_nvm_reboot(void);

/* Make the next `n` stores fail, as a flash write that did not take (0 turns
 * it off). Makes ADR 0009's fail-closed rule assertable: a node that cannot
 * persist its counter must not enter the pairing window. */
void fake_nvm_fail_stores(uint32_t n);

/* Sever the store from the load: reported as written but is not. Exercises
 * node_ident.c's confirm-by-re-reading and catches a backend that returns
 * early from a queued write. */
void fake_nvm_swallow_stores(bool on);

/* Read a record straight out of the simulated part, bypassing node_nvm.h -
 * a suite asserting "the counter is in NVM by now" must not ask the layer
 * under test. Returns false when the record is absent. */
bool fake_nvm_peek_u32(const char *key, uint32_t *out);

/* How many stores have been attempted, so ADR 0009's flash-wear argument
 * ("once per boot, once per pairing window") is assertable. */
uint32_t fake_nvm_store_count(void);

#ifdef __cplusplus
}
#endif

#endif /* FAKE_NVM_H_ */
