/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_rules.h - the rule evaluator: docs/radiant-bridge.md section 6.1
 * and 6.2 (the two activity outputs and the two zone outputs). Runs as an
 * ordinary bridge sink and re-enters the bus with DERIVED records, exactly as
 * section 6's opening states: "Derived outputs re-enter the bus as ordinary
 * struct radiant_sample records... so no sink needs to know a rule engine
 * exists."
 *
 * ---------------------------------------------------------------------------
 * Scope: what P6 implements of section 6, and what it defers
 * ---------------------------------------------------------------------------
 * Implemented, and this is deliberately the whole of "correct by
 * construction" per the doc's own framing:
 *   6.1 Activity - "a sensor is active when its accumulating field is
 *       advancing", applied to whichever accumulator a binding actually
 *       produces (0x36 beat count -> worn, 0x30 energy -> bike in use), with
 *       the asymmetric ~2 s assert / ~20 s clear dwell the section specifies
 *       numerically.
 *   6.2 Zones - Karvonen (HRR) thresholds against the POPULATION PRIOR the
 *       section publishes as its "never personalised" numbers: rest < 82
 *       bpm, zone 2+ >= 134 bpm, both gated on the same binding's worn state.
 *
 * NOT implemented:
 *   - 6.3-6.5, the personalisation histogram, its NVM persistence, and its
 *     eviction policy. Section 6.3 itself says why this is a legitimate,
 *     complete implementation and not a partial one: "prior-only is n = 0 of
 *     this same path, not a second implementation... the output semantics
 *     never change... cold start needs no special case." What is here IS the
 *     n = 0 case, correctly. Personalisation is additive on top of it, not a
 *     prerequisite for it.
 *   - Liveness / STALE. Section 6.1's closing paragraph makes clearing
 *     activity on a missed heartbeat a SINK's job ("a sink publishes STALE
 *     after 3x the expected interval"), and no such heartbeat-tracking sink
 *     exists in this codebase yet. This module HONOURS RADIANT_SAMPLE_STALE
 *     if it ever appears on an incoming sample (forces the activity output
 *     false immediately, bypassing the clear dwell - a stronger signal than
 *     "not advancing" deserves a stronger response) but does not manufacture
 *     the flag itself.
 *   - Cadence and speed (0x35, 0x34). Section 6.1 names them as falling out
 *     of the same rule "for free" and explicitly NOT enabled in v1 - "an
 *     output nobody consumes is an endpoint to maintain." Left exactly that
 *     unenabled here.
 *   - Publish pacing (the doc's closing "Publish pacing" note, change-or-
 *     heartbeat). That is a SINK'S obligation on the samples this module
 *     produces, not this module's - a rule evaluator emitting a boolean the
 *     instant it changes is correct; a sink choosing when to actually publish
 *     it over MQTT or Matter is a different layer's job.
 *   - The zone dwell duration is NOT stated numerically anywhere in section
 *     6.2 the way the activity dwell is (2 s / 20 s). RADIANT_RULE_ZONE_-
 *     DWELL_US below is this implementation's own choice, flagged as such in
 *     its own comment, and should be revisited against a real gate rather
 *     than trusted as a transcribed number.
 */

#ifndef RADIANT_RULES_H_
#define RADIANT_RULES_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* struct radiant_sample::field_id for the four derived RADIANT_FIELD_OCCUPANCY
 * outputs this module produces. Stable, per the same rule radiant_hr_adapter.h
 * states for its own field_id assignments: a sink keys history on
 * (source, field_id). */
#define RADIANT_RULE_FIELD_WORN         0u /* section 6.1: 0x36 beat count advancing */
#define RADIANT_RULE_FIELD_ACTIVE       1u /* section 6.1: 0x30 energy advancing ("bike in use") */
#define RADIANT_RULE_FIELD_AT_REST      2u /* section 6.2 */
#define RADIANT_RULE_FIELD_ZONE2        3u /* section 6.2, "training, zone 2 or above" */

/* Reset every binding's rule state. Test hook, mirrors radiant_bridge_reset()
 * and radiant_binding_init(). */
void radiant_rules_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RULES_H_ */
