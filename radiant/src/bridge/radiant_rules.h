/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_rules.h - the rule evaluator: docs/radiant-bridge.md section 6.1
 * and 6.2 (the two activity outputs and the two zone outputs). Runs as an
 * ordinary bridge sink and re-enters the bus with DERIVED records, per
 * section 6: no sink needs to know a rule engine exists.
 *
 * Implemented:
 *   6.1 Activity - a sensor is active when its accumulating field is
 *       advancing (0x36 beat count -> worn, 0x30 energy -> bike in use),
 *       with the ~2s assert / ~20s clear dwell the section specifies.
 *   6.2 Zones - Karvonen (HRR) thresholds against the section's "never
 *       personalised" population prior: rest < 82 bpm, zone2+ >= 134 bpm,
 *       both gated on the same binding's worn state.
 *   Equipment occupancy - FE-C's own state enum, which is the one input in
 *       this module that is a report rather than an inference. See
 *       RADIANT_RULE_FIELD_EQUIPMENT_IN_USE below.
 *
 * Not implemented:
 *   - 6.3-6.5, the personalisation histogram/NVM/eviction. What's here is
 *     the correct n=0 (prior-only) case of that same path; personalisation
 *     is additive on top, not a prerequisite.
 *   - Liveness/STALE production is a sink's job (6.1); no such
 *     heartbeat-tracking sink exists yet. This module honours
 *     RADIANT_SAMPLE_STALE if it appears (forces activity false
 *     immediately, bypassing the clear dwell) but doesn't manufacture it.
 *   - Cadence/speed (0x35, 0x34): falls out "for free" per 6.1 but
 *     explicitly not enabled in v1 - unused output, unmaintained here.
 *   - Publish pacing (change-or-heartbeat) is a sink's obligation on these
 *     samples, not this module's.
 *   - The zone dwell duration is not stated numerically in 6.2 the way the
 *     activity dwell is - RADIANT_RULE_ZONE_DWELL_US below is this
 *     implementation's own guess, not a transcribed number.
 */

#ifndef RADIANT_RULES_H_
#define RADIANT_RULES_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* struct radiant_sample::field_id for the four derived RADIANT_FIELD_OCCUPANCY
 * outputs this module produces. Stable: a sink keys history on (source, field_id). */
#define RADIANT_RULE_FIELD_WORN         0u /* section 6.1: 0x36 beat count advancing */
#define RADIANT_RULE_FIELD_ACTIVE       1u /* section 6.1: 0x30 energy advancing ("bike in use") */
#define RADIANT_RULE_FIELD_AT_REST      2u /* section 6.2 */
#define RADIANT_RULE_FIELD_ZONE2        3u /* section 6.2, "training, zone 2 or above" */

/*
 * THE FIFTH OUTPUT, AND THE ONLY ONE THAT IS NOT AN INFERENCE.
 *
 * section 8.2 admits the four booleans above stretch Matter's Occupancy Sensor
 * ("a rider is occupying the trainer"). Fitness equipment is the case where
 * that stretch disappears: FE-C page 16 byte 7 carries an explicit FE STATE -
 * READY (2), IN_USE (3), FINISHED (4) - which is the machine's own report
 * rather than an answer to section 6.1's "is an accumulator advancing?"
 * heuristic. radiant_power_adapter.c has been posting it as
 * RADIANT_FIELD_ENUM_GENERIC since the FE-C decoder landed, and its own
 * comment says exactly why it stops there: "turning IN_USE into 'bike in use'
 * here would put a rule in an adapter; radiant_rules.c owns derived booleans."
 * This is that rule.
 *
 * WHY THIS MATTERS MORE FOR A TREADMILL THAN FOR A TRAINER. The ACTIVE output
 * above is driven by the 0x30 energy accumulator, which the FE-C adapter posts
 * only from page 25 (Specific Trainer Data). A treadmill sends page 19 instead,
 * which nothing in this tree decodes - so before this rule a bound treadmill
 * published a speed and a state enum and asserted no activity at all.
 *
 * NO VOCABULARY ADDITION IS NEEDED and that matters: section 13 forbids adding
 * a RADIANT_FIELD_* until it is allocated in docs/radiant-telemetry.md and
 * tools/ant_pages.py in the same change. This output is
 * RADIANT_FIELD_OCCUPANCY, which is already in all three copies, on a new
 * field_id beside the four above.
 */
#define RADIANT_RULE_FIELD_EQUIPMENT_IN_USE 4u /* FE-C state == IN_USE */

/* Reset every binding's rule state. Test hook. */
void radiant_rules_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RULES_H_ */
