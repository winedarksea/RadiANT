/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_rules.h - the rule evaluator: docs/radiant-bridge.md section 6.1
 * and 6.2 (the activity outputs and the zone outputs). Runs as an ordinary
 * bridge sink and re-enters the bus with DERIVED records, per section 6: no
 * sink needs to know a rule engine exists.
 *
 * Implemented:
 *   6.1 Activity - a sensor is active when its accumulating field is
 *       advancing (0x36 beat count -> worn, 0x30 energy -> bike in use),
 *       with the ~2s assert / ~20s clear dwell the section specifies.
 *       Worn is not produced for a source bound as ANT+ Environment
 *       (device type 0x19), whose event count steps on every message.
 *   6.2 Zones - Karvonen (HRR) thresholds: rest < rest + 0.15 HRR, zone 2+ >=
 *       rest + 0.60 HRR, zone 3+ >= rest + 0.70 HRR, all gated on the same
 *       binding's worn state. With the default prior (HR_max 180, HR_rest
 *       65: CONFIG_RADIANT_BRIDGE_HR_MAX_PRIOR / _HR_REST_PRIOR) that is
 *       rest < 82, zone 2 >= 134, zone 3 >= 146.
 *   6.3-6.5 Personalisation - radiant_hr_profile.h. Each bound strap with a
 *       profile slot feeds a 1 Hz histogram while worn and reading; its
 *       thresholds are recomputed only when the binding first reports and at
 *       session end (worn clears, or STALE), never mid-session, so a boolean
 *       never moves under a running automation. No slot = the prior.
 *   Equipment occupancy - FE-C's own state enum, which is the one input in
 *       this module that is a report rather than an inference. See
 *       RADIANT_RULE_FIELD_EQUIPMENT_IN_USE below. A trainer sends READY at
 *       zero cadence, so the clear delay is
 *       CONFIG_RADIANT_BRIDGE_RULES_EQUIPMENT_CLEAR_S (default 30 s).
 *
 * Publish pacing: every output is posted once, as its current value (so
 * normally `false`), on the first sample that feeds it, and afterwards only
 * when it changes. The first post is what makes a Matter endpoint or an MQTT
 * entity exist for an idle sensor; the attribute then holds its value, so no
 * heartbeat is needed. A bind or unbind (binding_changed) resets the source's
 * state, so a sensor that reuses a slot announces afresh and inherits nothing.
 *
 * Not implemented:
 *   - Liveness/STALE production is radiant_liveness.c's job. This module
 *     honours RADIANT_SAMPLE_STALE (forces activity false immediately,
 *     bypassing the clear dwell) but doesn't manufacture it.
 *   - Cadence/speed (0x35, 0x34): falls out "for free" per 6.1 but
 *     explicitly not enabled in v1 - unused output, unmaintained here.
 *   - The zone dwell duration is not stated numerically in 6.2 the way the
 *     activity dwell is - 3 s is this implementation's own guess, not a
 *     transcribed number.
 */

#ifndef RADIANT_RULES_H_
#define RADIANT_RULES_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* struct radiant_sample::field_id for the derived RADIANT_FIELD_OCCUPANCY
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

/*
 * Zone 3 or above: HR >= HR_rest + 0.70 x HRR (146 bpm with the default
 * prior), the third band of the five-zone HRR model whose second band is
 * RADIANT_RULE_FIELD_ZONE2. Numbered after EQUIPMENT_IN_USE because ids are
 * stable once shipped (a sink keys history on them).
 */
#define RADIANT_RULE_FIELD_ZONE3 5u

/*
 * Is this output compiled in? CONFIG_RADIANT_BRIDGE_RULES_OUT_* (radiant/Kconfig)
 * builds a compile-time mask indexed by the RADIANT_RULE_FIELD_* ids above; a
 * disabled output is never posted, on announce, on change or on the STALE
 * drop, so it gets no Matter endpoint and no MQTT entity. This lets a sink
 * (the Matter app retiring a restored endpoint) ask without knowing the
 * Kconfig names. False for an id outside the table, and false in a bridge
 * build without the rules sink, where no derived output exists at all.
 *
 * The stub is keyed on CONFIG_RADIANT_BRIDGE as well as _RULES because
 * radiant/tests compiles radiant_rules.c directly with neither defined: that
 * image must see the prototype, not a second definition.
 */
#if defined(CONFIG_RADIANT_BRIDGE) && !defined(CONFIG_RADIANT_BRIDGE_RULES)
static inline bool radiant_rules_output_enabled(uint8_t field_id)
{
	(void)field_id;
	return false;
}
#else
bool radiant_rules_output_enabled(uint8_t field_id);
#endif

/* Reset every binding's rule state. Test hook. */
void radiant_rules_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RULES_H_ */
