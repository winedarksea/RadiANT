/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_power_adapter.h - ANT+ Bicycle Power (device type 0x0B) and ANT+
 * Fitness Equipment (device type 0x11) onto the sample bus.
 *
 * Wire layouts are src/profiles/profile_power.h's and src/profiles/profile_fec.h's,
 * decoded through profile_power_decode_std() and profile_fec_decode_*() rather
 * than re-derived here. Vocabulary mapping follows docs/radiant-bridge.md
 * section 3.1's "one adapter per profile, not one per profile per sink".
 *
 * No radiant header, direct or transitive - the same claim
 * radiant_hr_adapter.h and radiant_rd_adapter.h make. ISR-safe: it decodes,
 * arithmetic-only, and calls radiant_bridge_post(), which is itself ISR-safe.
 *
 * ---------------------------------------------------------------------------
 * Why one adapter for two device types
 * ---------------------------------------------------------------------------
 * A binding has exactly one device type, so an instance of this struct only
 * ever sees one of the two entry points below. They are in one file, and share
 * one state struct, because they produce THE SAME THREE SERIES from the same
 * two quantities: instantaneous watts and an update event count. A power meter
 * and a smart trainer are the same measurement arriving in two envelopes; two
 * adapters would be two copies of the energy integral, which is the part of
 * this file with a decision in it.
 *
 * The FE-C entry point additionally decodes page 16, which a power meter has
 * no equivalent of. Those fields take their own field_id block (0x08+) so that
 * (source, field_id) stays stable whichever device type a binding turns out to
 * be.
 *
 * Pages neither entry point understands return 0 and post nothing. That is a
 * deliberate contract, not an oversight: a power meter's rotation carries
 * 0x11/0x12/0x13/0x20 and the common pages, and FE-C's carries 0x13-0x1A, 54,
 * 71 and the calibration pages. MISREADING one of those as a power page would
 * publish a plausible wattage; declining it publishes nothing.
 *
 * ---------------------------------------------------------------------------
 * THE 0x30 ENERGY DECISION - the one design call in this file
 * ---------------------------------------------------------------------------
 * Both profiles carry a field named "accumulated power":
 *
 *   Bicycle Power Rev 5.1 section 8.4 - "Accumulated power is the running sum
 *   of the instantaneous power data and is incremented at each update of the
 *   update event count."
 *   Fitness Equipment Rev 5.0 section 8.6.7.3 - word for word the same.
 *
 * It is a SUM OF WATT SAMPLES. Its units are watts, not joules and not
 * watt-seconds. The spec's own use for it (Rev 5.1 Equation 1, Rev 5.0
 * Equation 8-2) is to divide its delta by the event-count delta and get
 * AVERAGE WATTS - a mean, which is why the sum has no time in it. Publishing
 * that number as RADIANT_FIELD_ENERGY (0x30, joules) would be off by exactly
 * the update interval, would change scale if the sensor changed its update
 * rate, and would be wrong in a way that looks right.
 *
 * So this adapter does not relabel it. It integrates instead:
 *
 *     acc_uJ += inst_W * (t_us - prev_t_us)
 *
 * 1 W x 1 us is exactly 1 uJ, so that line is a unit identity with no scale
 * factor to get wrong, and it publishes at exp = -6. This is the mechanism
 * tools/ant_pages.py's TLM_TIME_INTEGRAL_PARTNER already declares for the
 * 0x1C -> 0x30 pair (watts to joules is a time integral), applied rather than
 * merely declared.
 *
 * It is a RIGHT-HAND Riemann sum: the interval [prev_t_us, t_us] is charged at
 * the wattage reported at its END. Left-hand would be equally defensible and
 * the difference is one update interval of lag; right-hand is chosen because
 * it makes a coast stop the accumulator on the first zero-watt message rather
 * than one message later, and "did the accumulator advance" is precisely what
 * radiant_rules.c's activity rule asks (docs/radiant-bridge.md section 6.1).
 *
 * The accumulated-power field on the wire is therefore DECODED AND NOT
 * PUBLISHED. Its honest home would be a "mean power since the last message"
 * instantaneous field, which is 0x1C again with a different meaning - two
 * series with one type on one source, which the field_id namespace permits but
 * no sink could tell apart. Left out until something needs it.
 *
 * ---------------------------------------------------------------------------
 * Accumulator discipline (docs/radiant-bridge.md section 3.2)
 * ---------------------------------------------------------------------------
 * Difference at the WIRE width, accumulate exactly, convert only at
 * publication. Applied here three times:
 *
 *   event count   differenced as uint8_t (both profiles roll at 256), summed
 *                 into a uint64_t. Unsigned subtraction at that width wraps
 *                 correctly by construction, so the rollover needs no special
 *                 case - and widening before subtracting is the bug that
 *                 produces one absurd sample every 256 events.
 *   energy        accumulated in uJ, an exact integer, and only ever divided
 *                 by 10^6 by the CONSUMER via exp = -6. Nothing here rounds.
 *   elapsed time  differenced as uint8_t (rolls at 64 s) - see the reserved
 *                 field_id block below for why it is not published.
 *
 * WHAT IS PUBLISHED FOR AN ACCUMULATING FIELD IS THE RUNNING TOTAL, NOT THE
 * DELTA. RADIANT_SAMPLE_ACCUMULATING is documented in radiant_bridge.h as
 * "raw is a monotone counter, not an instant", and radiant_rules.c's
 * eval_activity() tests `s->raw - prev_raw > 0`, which is only meaningful
 * against a total. (radiant_hr_adapter.c posts a per-message DELTA for its
 * beat count instead. That is a discrepancy in the older file, not a
 * convention this one is departing from; it is called out here rather than
 * quietly imitated.)
 *
 * ---------------------------------------------------------------------------
 * The first call establishes a baseline and integrates nothing
 * ---------------------------------------------------------------------------
 * There is no previous event count to difference against and no previous
 * timestamp to integrate over, so the first power-bearing page an instance
 * sees publishes instantaneous watts only. Same rule as
 * radiant_hr_adapter_decode(). Non-power pages (FE-C page 16) never establish
 * the baseline: they carry neither quantity, and letting one start the clock
 * would charge the first real interval at a wattage nobody reported.
 */

#ifndef RADIANT_POWER_ADAPTER_H_
#define RADIANT_POWER_ADAPTER_H_

#include <stdbool.h>
#include <stdint.h>

#include "radiant_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * field_id assignments within one power/FE-C source. Stable: a sink keys
 * history on (source, field_id), so renumbering these breaks every sink's
 * stored series. Every id here lives in RADIANT_FIELD_ID_PROFILE_BASE ..
 * RADIANT_FIELD_ID_PROFILE_MAX (0x00-0x1F), leaving 0x20+ to the common-page
 * adapter, which runs on this same source - see radiant_bridge.h's block.
 *
 * 0x00-0x07: the quantities BOTH device types carry.
 * 0x08-0x0F: FE-C page 16 only.
 */
#define RADIANT_POWER_FIELD_INST_POWER  0x00u /* 0x1C active power, instantaneous, W */
#define RADIANT_POWER_FIELD_EVENT_COUNT 0x01u /* 0x36 event count, accumulating, u8 wire width */
#define RADIANT_POWER_FIELD_ENERGY      0x02u /* 0x30 energy, accumulating, integrated, exp -6 */

/*
 * RESERVED, 0x03-0x07. Decoded by this adapter and deliberately not posted, so
 * that publishing one later does not renumber the three that ship today:
 *
 *   0x03 cadence (rpm)          no vocabulary type. rpm is a non-SI rate, the
 *                               same gap radiant_rd_adapter.h reserves id 1
 *                               for; 0x19 angular rate is rad/s and the
 *                               conversion is irrational (1 rpm = pi/30 rad/s).
 *   0x04 pedal power balance    0x24 percentage would fit exactly, but the
 *                               number means two different things depending on
 *                               the differentiation bit (Rev 5.1 section
 *                               8.2.1) and a single percentage series cannot
 *                               carry that distinction.
 *   0x05 mean power             the decoded accumulated-power field, as
 *                               average watts. See the 0x30 discussion above.
 *   0x06-0x07 unallocated
 */
#define RADIANT_POWER_FIELD_CADENCE     0x03u /* RESERVED */
#define RADIANT_POWER_FIELD_BALANCE     0x04u /* RESERVED */
#define RADIANT_POWER_FIELD_MEAN_POWER  0x05u /* RESERVED */

#define RADIANT_POWER_FIELD_FEC_STATE 0x08u /* 0x40 enum, instantaneous, PROFILE_FEC_STATE_* */
#define RADIANT_POWER_FIELD_FEC_SPEED 0x09u /* 0x16 speed, instantaneous, exp -3 */

/*
 * RESERVED, 0x0A-0x0F. FE-C page 16 carries three more quantities that this
 * adapter decodes and does not post. The first two are held back by a
 * CONSUMER-side constraint, which is worth stating precisely because it is not
 * obvious from this file:
 *
 *   0x0A elapsed time (0x37 duration, accumulating)
 *   0x0B distance     (0x34 distance, accumulating)
 *        radiant_rules.c gives each source ONE activity slot for
 *        non-event-count accumulating types, so a source posting 0x30, 0x37
 *        and 0x34 would have all three fight over one prev_raw and one dwell -
 *        which is precisely the two-producer defect that fix exists for, at a
 *        larger arity. Publishing these needs a per-field-type slot map first.
 *        (Elapsed time has a second problem of its own: section 8.5.2.2 only
 *        increments it in the IN_USE state, so it is session time, not
 *        duration.)
 *   0x0C heart rate   (0x26 heart rate, instantaneous)
 *        Not posted because RADIANT_FIELD_HEART_RATE is what drives
 *        radiant_rules.c's zone rules, and a trainer relaying a strap's bpm
 *        would produce a second, duplicate pair of at-rest/zone-2 entities on
 *        the trainer's binding. The strap's own binding already publishes it.
 *   0x0D-0x0F unallocated.
 */
#define RADIANT_POWER_FIELD_FEC_ELAPSED  0x0Au /* RESERVED */
#define RADIANT_POWER_FIELD_FEC_DISTANCE 0x0Bu /* RESERVED */
#define RADIANT_POWER_FIELD_FEC_HR       0x0Cu /* RESERVED */

struct radiant_power_adapter {
	/* True once a power-bearing page has established the baseline. Covers
	 * both the event count and the integration clock, because both are set
	 * on that same first message. */
	bool have_prev;

	uint8_t  prev_event_count; /* raw, as last seen on the wire */
	uint64_t acc_events;       /* exact running total, differenced at u8 */

	uint64_t prev_t_us;        /* the t_sync of the last power-bearing page */
	uint64_t acc_uj;           /* exact energy integral, microjoules */
};

void radiant_power_adapter_init(struct radiant_power_adapter *a);

/*
 * Decodes one ANT+ Bicycle Power (device type 0x0B) message. `body8` is the
 * full 8-byte payload; only page 0x10 is decoded and every other page returns
 * 0 having posted nothing. `t_us` is the t_sync the message was heard at.
 *
 * Returns samples posted: 1 on the first page 0x10 an instance sees
 * (instantaneous watts, no baseline yet), 3 thereafter.
 */
uint32_t radiant_power_adapter_decode(struct radiant_power_adapter *a,
				      uint32_t source, const uint8_t body8[8],
				      uint64_t t_us);

/*
 * Decodes one ANT+ Fitness Equipment (device type 0x11) message. Pages 0x10
 * (General FE Data) and 0x19 (Specific Trainer Data) are decoded; every other
 * page returns 0 having posted nothing.
 *
 * NOTE that 0x10 means a DIFFERENT page here than it does in
 * radiant_power_adapter_decode() - page numbers are per device type. Calling
 * the wrong entry point for a binding therefore does not fail loudly; it
 * decodes an FE-C general page as bicycle power and reports a speed as watts.
 * The dispatcher owns getting this right, which is why the two entry points
 * are named after the device type rather than the page.
 *
 * Returns samples posted: up to 2 for page 0x10 (state, speed), 1 or 3 for
 * page 0x19 on the same first-call rule as above, and 0 when page 0x19 marks
 * its power fields invalid.
 */
uint32_t radiant_power_adapter_decode_fec(struct radiant_power_adapter *a,
					  uint32_t source,
					  const uint8_t body8[8], uint64_t t_us);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_POWER_ADAPTER_H_ */
