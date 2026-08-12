/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_binding.h - the binding table: docs/radiant-bridge.md section 5.
 *
 * "Nothing downstream may key on an ANT device number." The table is the
 * pivot the sample bus's `source` field is an index into (section 3's
 * comment on `struct radiant_sample::source` says so directly), and section
 * 5's three reasons are why: a device number is 16 bits, it is re-rollable by
 * design (identity Tier 0), and a collision across two straps in one room is
 * a one-in-65535 event that will happen at a trade show.
 *
 * ---------------------------------------------------------------------------
 * What P6 implements of section 5, and what it does not
 * ---------------------------------------------------------------------------
 * Implemented: the table itself, capacity-bounded lookup and bind/unbind,
 * `uuid` generation at first bind.
 *
 * NOT implemented, named rather than silently missing:
 *   - Persistence. Section 5 does not say the table must survive a reboot -
 *     that requirement is stated for the PERSONALISATION HISTOGRAM in section
 *     6.5, which is out of scope for P6 (see radiant_rules.h). A real product
 *     build would persist this table the same way src/node/node_nvm_settings.c
 *     persists node identity, through the settings subsystem; this module is
 *     storage-backend-agnostic on purpose (RAM here, NVS later) for the same
 *     reason radiant_core's HAL is backend-agnostic - swapping the backend
 *     should not touch a caller.
 *   - "Matching a re-paired strap back to its uuid is a user action, not an
 *     inference." No such reconciliation UI exists in this codebase, so a
 *     re-paired strap binds as a NEW entry with a new uuid until something
 *     above this layer offers that choice to a human.
 *   - Consent / policy thresholds. Section 5's binding record has a `policy`
 *     member; this one does not, because nothing in P6's scope (the bus and
 *     the four built-in derived rules of section 6.1/6.2) reads one yet.
 */

#ifndef RADIANT_BINDING_H_
#define RADIANT_BINDING_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Eight for the household SKU (section 5: "Table capacity sizes the Matter
 * endpoint count, the ANT tracked-channel count and the coexistence budget of
 * section 7 together... Raising it is a radio decision before it is a memory
 * decision" - so it is not raised here without that decision being made).
 */
#define RADIANT_BINDING_MAX 8u

/* An invalid source / lookup miss. Never a real table index: valid indices
 * are [0, RADIANT_BINDING_MAX). Distinct from RADIANT_BRIDGE_DIAG_SOURCE
 * (radiant_bridge.h), which is a real, reserved, out-of-band source. */
#define RADIANT_BINDING_NONE UINT32_MAX

struct radiant_binding {
	bool     used;
	uint16_t devnum;
	uint8_t  devtype;
	uint8_t  trans_type;

	/* "What a human calls it." Empty ("") until a caller sets one; a
	 * binding with no label is still a binding. */
	char label[16];

	/*
	 * "Stable across a re-roll and a re-pair" is a promise about THIS
	 * binding's uuid never changing while it exists - not, per the header
	 * comment above, a promise that a re-paired physical strap gets the
	 * SAME binding back. Generated once at bind time from sys_rand32_get()
	 * x2 - a label, not a key, so a non-cryptographic RNG is the right
	 * tool and pulling in radiant_sec for it would be the wrong kind of
	 * dependency for a bridge-layer module to have.
	 */
	uint64_t uuid;
};

void radiant_binding_init(void);

/*
 * Find the binding for (devnum, devtype, trans_type), creating one if none
 * exists and a slot is free. *source_out receives the table index either
 * way. Returns 0 on success, -ENOSPC if the table is full and no existing
 * binding matches - the caller's contract per section 5's "binding is
 * explicit and opt-in, always": this function is the opt-in act, so it is
 * deliberately not automatic on every frame the bus happens to see one of -
 * a caller (the not-yet-built pairing flow) decides when to call it.
 */
int radiant_binding_bind(uint16_t devnum, uint8_t devtype, uint8_t trans_type,
		     const char *label, uint32_t *source_out);

/* Look up an existing binding by (devnum, devtype, trans_type) without
 * creating one. RADIANT_BINDING_NONE on a miss. */
uint32_t radiant_binding_find(uint16_t devnum, uint8_t devtype, uint8_t trans_type);

/* NULL if `source` is out of range or the slot is not bound. */
const struct radiant_binding *radiant_binding_get(uint32_t source);

/* Frees the slot. The uuid is not reused; a new bind() of the same physical
 * device after this gets a new one, per the re-pair note above. */
int radiant_binding_unbind(uint32_t source);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_BINDING_H_ */
