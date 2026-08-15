/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_binding.h - the binding table: docs/radiant-bridge.md section 5.
 *
 * Nothing downstream may key on an ANT device number: it's 16 bits,
 * re-rollable by design, and can collide across two straps in one room.
 * `source` (radiant_bridge.h's struct radiant_sample::source) is a table
 * index instead.
 *
 * Not implemented here, deliberately rather than by omission:
 *   - Persistence. Section 5 doesn't require it (that's section 6.5's
 *     personalisation histogram, out of scope for P6). RAM-backed for now,
 *     storage-backend-agnostic like radiant's HAL.
 *   - Reconciling a re-paired strap to its old uuid is a user action, not
 *     an inference; no such UI exists, so a re-pair binds as a new entry.
 *   - Consent/policy thresholds (section 5's `policy` member) - unused,
 *     since nothing in P6's scope (bus + built-in rules of 6.1/6.2) reads it.
 */

#ifndef RADIANT_BINDING_H_
#define RADIANT_BINDING_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 8, for the household SKU: sized with the Matter endpoint count, ANT
 * tracked-channel count, and coexistence budget (section 7) together -
 * raising it is a radio decision, not a memory one. */
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

	/* Stable across a re-roll/re-pair for THIS binding only - not a
	 * promise a re-paired strap gets the same binding back. Generated
	 * once at bind time; a label, not a key, so non-cryptographic is fine. */
	uint64_t uuid;

	/*
	 * The channel period in ANT's own 32768 Hz counts (8192 = the 4 Hz
	 * most ANT+ profiles use). 0 means "never set", which is a distinct
	 * state from any real period and is treated as such by every reader.
	 *
	 * This is the ONLY source of an expected inter-arrival interval in the
	 * tree: radiant_liveness.c multiplies it by 3 to decide when a source
	 * has gone quiet, and docs/radiant-bridge.md section 9.2's MQTT
	 * `expire_after` is the same number. It is set from the radio - the
	 * binder calls antr_channel_period_get() at bind time - and NOT
	 * inferred from observed arrivals; see radiant_liveness.h for why an
	 * estimator here is worse than no estimate.
	 */
	uint16_t period;
};

void radiant_binding_init(void);

/*
 * Finds the binding for (devnum, devtype, trans_type), creating one if none
 * exists and a slot is free. *source_out receives the table index either
 * way. Returns 0 on success, -ENOSPC if full with no match. This is the
 * opt-in act (section 5): never called automatically by the bus itself.
 */
int radiant_binding_bind(uint16_t devnum, uint8_t devtype, uint8_t trans_type,
		     const char *label, uint32_t *source_out);

/* Look up an existing binding by (devnum, devtype, trans_type) without
 * creating one. RADIANT_BINDING_NONE on a miss. */
uint32_t radiant_binding_find(uint16_t devnum, uint8_t devtype, uint8_t trans_type);

/*
 * Record the channel period (32768 Hz counts) for an existing binding.
 * Returns 0, or -ENOENT if `source` is out of range or not bound.
 *
 * A separate call rather than a widened radiant_binding_bind() signature, and
 * the reason is the layering rather than convenience: the value comes from
 * antr_channel_period_get(), which lives in apps/common/ant/ant_radio.h - a
 * header this directory reaches neither directly nor transitively, and the
 * absence of which is a property radiant/tests/CMakeLists.txt states as the
 * reason src/bridge is compiled where it is. So the period necessarily arrives
 * from the caller a moment after the bind, and every existing call site would
 * otherwise have had to pass a 0 it does not know.
 *
 * "A moment after" is still bind time, which is what matters: this must be a
 * fact read off the channel, not a number learned from the traffic.
 */
int radiant_binding_set_period(uint32_t source, uint16_t period);

/* NULL if `source` is out of range or the slot is not bound. */
const struct radiant_binding *radiant_binding_get(uint32_t source);

/* Frees the slot. The uuid is not reused; a new bind() of the same physical
 * device after this gets a new one, per the re-pair note above. */
int radiant_binding_unbind(uint32_t source);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_BINDING_H_ */
