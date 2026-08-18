/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_liveness.h - the only producer of RADIANT_SAMPLE_STALE.
 *
 * WHY THIS EXISTS AT ALL. RADIANT_SAMPLE_STALE has been defined
 * (radiant_bridge.h) and honoured (radiant_rules.c's eval_activity(), which
 * forces an immediate clear on it and says in its own comment that it "reacts
 * to STALE but doesn't produce it") since P6, and nothing in the tree has ever
 * set the bit. docs/radiant-bridge.md section 9.2 states the cost of that gap
 * plainly: without an expiry, "a strap that walks out of range leaves its last
 * reading on screen indefinitely - which for a heart-rate-driven AC is not a
 * stale number, it is a wrong actuator."
 *
 * So this file lands before any sink does. A sink written against a bus that
 * never marks anything stale grows that assumption into itself - the MQTT
 * discovery generator's `expire_after` (section 9.2) and the Matter
 * availability model both read the flag, and both would have been written
 * against a bus where it is provably always clear.
 *
 * WHAT IT IS. An ordinary sink - the precedent is radiant_rules.c, which
 * registers with RADIANT_SINK_DEFINE() and re-enters the bus with its own
 * records. This one remembers the last sample per (source, field_id), and a
 * 1 Hz caller re-posts those samples with STALE set, once, when the SOURCE has
 * gone quiet for longer than 3x the binding's channel period.
 *
 * THE QUIET IS MEASURED PER SOURCE, NOT PER FIELD, and that is a correction
 * made against a measurement rather than an original choice.
 *
 * Per field, the rule reads "3x the channel period" as if every field were
 * carried by every message. That is true of radiant_hr_adapter.c - bytes 4..7
 * are on every HR main page - and it is false of every profile that rotates
 * pages. An FE-C trainer carries speed and FE state on page 16, power and
 * event count on page 25, accumulated energy on 26 and equipment type on 54,
 * so each individual field arrives roughly once a second on a 4 Hz channel
 * while the threshold is 0.74 s. Measured on a real Wahoo #52233: 165 STALE
 * records in 150 s, every one of them from the trainer's two sources and not
 * one from the heart-rate strap on the same bench, each field expiring and
 * being re-armed by its own next page forever. Downstream that is not noise:
 * mqtt_sink.c drives the per-binding availability topic from STALE, so a
 * healthy trainer flapped its whole device offline and back once a second.
 *
 * A source is quiet when NOTHING from it has arrived for 3x its period. That
 * is the question the module exists to answer ("has this sensor gone away"),
 * it is unchanged for a strap whose fields all ride every message, and it
 * cannot be confused by a page rotation because a rotation is still traffic.
 * When the source does go quiet EVERY tracked field of it is posted stale, so
 * a sink still learns per field and nothing downstream had to change.
 *
 * What this gives up, deliberately: a device that keeps transmitting but drops
 * one page out of its rotation is no longer reported. That is not the failure
 * the header's opening paragraph is about - the reading is still being
 * refreshed by a live device - and detecting it needs a per-field expected
 * cadence, which is exactly the estimator the next paragraph rules out.
 *
 * THE INTERVAL DOES NOT COME FROM OBSERVED ARRIVALS, and that is a decision
 * rather than an omission. An estimator fed by arrivals learns from an
 * intermittently-in-range strap that its own gaps are normal, and then never
 * reports it missing - the exact failure this module exists to prevent, made
 * self-concealing. The interval comes from struct radiant_binding::period,
 * which the binder sets from antr_channel_period_get() at bind time. A binding
 * whose period was never set is NOT guessed at: it is skipped and counted
 * (radiant_liveness_stats::no_period), because a wrong expiry is worse than a
 * missing one and a counter is how the difference gets noticed.
 *
 * No radiant header, direct or transitive - same claim radiant_rules.c and
 * radiant_hr_adapter.c make about themselves. This is the layer above the
 * profile decoder.
 */

#ifndef RADIANT_LIVENESS_H_
#define RADIANT_LIVENESS_H_

#include <stdint.h>

#include "radiant_binding.h"
#include "radiant_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Fields tracked per source. Sized on the widest adapter in the tree, which is
 * NOT the one this constant was originally sized on.
 *
 * It said four, "radiant_hr_adapter.c publishes three field_ids and
 * radiant_rd_adapter.c publishes fewer". radiant_power_adapter.c publishes
 * SIX on one FE-C source - instantaneous power, event count, accumulated
 * energy, equipment type, FE state and speed - and has done since it landed.
 * Measured on a real Wahoo trainer: no_slot reached 64 in 150 s while tracked
 * sat at 7, so two of that trainer's six fields were not being watched at all
 * and would not have been marked stale if the trainer had been unplugged.
 *
 * Eight is that real worst case plus two. A ninth distinct field_id on one
 * source is still refused and counted (radiant_liveness_stats::no_slot) rather
 * than evicting a tracked one, because evicting is how a field silently stops
 * being watched - and no_slot is now printed by the caller every sweep, which
 * is what would have caught this the first time.
 */
#define RADIANT_LIVENESS_FIELDS_PER_SOURCE 8u

struct radiant_liveness_stats {
	uint32_t tracked;      /* entries currently holding a last sample */
	uint32_t stale_posted; /* STALE records this module has put on the bus */
	uint32_t no_slot;      /* samples dropped for want of a table entry */
	uint32_t no_period;    /* tick passes over a binding with period == 0 */
};

/* Test/reset hook, and the boot-time initialiser. Not for production use
 * beyond that; the sink itself needs no registration call. */
void radiant_liveness_reset(void);

/*
 * The 1 Hz sweep. Returns how many STALE records it posted this pass (they go
 * through radiant_bridge_post(), so a sink sees them on the next drain, not
 * from inside this call).
 *
 * `now_us` MUST be on the same clock as the t_us the samples carried in. In
 * apps/dongle_thread that is k_uptime_ticks() converted to microseconds, taken
 * at the RX tap; if the two ever diverge this module either expires everything
 * immediately or nothing at all, and neither failure announces itself. Passed
 * in rather than read here so the whole rule is a pure function of time and a
 * ztest can drive thirty seconds in six calls.
 *
 * Edge-triggered: one STALE record per quiet period, not one per second. The
 * edge is re-armed by the next fresh sample for that (source, field_id).
 */
uint32_t radiant_liveness_tick(uint64_t now_us);

const struct radiant_liveness_stats *radiant_liveness_stats_get(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_LIVENESS_H_ */
