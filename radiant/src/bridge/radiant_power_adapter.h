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
 * A binding has exactly one device type, so an instance only ever sees one
 * of the two entry points below. They share one state struct because both
 * produce THE SAME THREE SERIES from the same two quantities - instantaneous
 * watts and an update event count - so two adapters would duplicate the
 * energy integral, the one decision in this file. FE-C additionally decodes
 * page 16, whose fields take their own block (0x08+) so (source, field_id)
 * stays stable whichever device type a binding turns out to be.
 *
 * Pages neither entry point understands return 0 and post nothing,
 * deliberately: misreading a power meter's 0x11/0x12/0x13/0x20 page, or one
 * of FE-C's 0x13-0x1A/54/71/calibration pages, as a power page would publish
 * a plausible wattage; declining it publishes nothing.
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
 * It is a SUM OF WATT SAMPLES: units are watts, not joules or watt-seconds.
 * The spec's own use (Rev 5.1 Equation 1, Rev 5.0 Equation 8-2) divides its
 * delta by the event-count delta to get AVERAGE WATTS - a mean, which is why
 * the sum has no time in it. Publishing it as RADIANT_FIELD_ENERGY (0x30,
 * joules) would be off by exactly the update interval, would change scale if
 * the sensor changed its update rate, and would be wrong in a way that looks
 * right.
 *
 * So this adapter integrates instead:
 *
 *     acc_uJ += inst_W * (t_us - prev_t_us)
 *
 * 1 W x 1 us is exactly 1 uJ - a unit identity with no scale factor to get
 * wrong - published at exp = -6, the mechanism tools/ant_pages.py's
 * TLM_TIME_INTEGRAL_PARTNER declares for the 0x1C -> 0x30 pair.
 *
 * It is a RIGHT-HAND Riemann sum: [prev_t_us, t_us] is charged at the
 * wattage reported at its END. Left-hand would be equally defensible (one
 * update interval of lag either way); right-hand is chosen because it makes
 * a coast stop the accumulator on the first zero-watt message rather than
 * one message later, and "did the accumulator advance" is precisely what
 * radiant_rules.c's activity rule asks (docs/radiant-bridge.md section 6.1).
 *
 * The accumulated-power field is therefore DECODED AND NOT PUBLISHED: its
 * honest home would be a "mean power since the last message" instantaneous
 * field, which is 0x1C again with a different meaning - two series with one
 * type on one source, which the namespace permits but no sink could tell
 * apart. Left out until something needs it.
 *
 * ---------------------------------------------------------------------------
 * Accumulator discipline: see radiant_common_adapter.h. Applied here three
 * times:
 * ---------------------------------------------------------------------------
 *
 *   event count   differenced as uint8_t (both profiles roll at 256), summed
 *                 into a uint64_t.
 *   energy        accumulated in uJ, an exact integer, and only ever divided
 *                 by 10^6 by the CONSUMER via exp = -6. Nothing here rounds.
 *   elapsed time  differenced as uint8_t (rolls at 64 s) - see the reserved
 *                 field_id block below for why it is not published.
 *
 * ---------------------------------------------------------------------------
 * First-call baseline: see radiant_common_adapter.h.
 * ---------------------------------------------------------------------------
 * Non-power pages (FE-C page 16) never establish the baseline: they carry
 * neither quantity, and letting one start the clock would charge the first
 * real interval at a wattage nobody reported.
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
 * field_id assignments within one power/FE-C source (field_id stability: see
 * radiant_common_adapter.h). Every id here lives in
 * RADIANT_FIELD_ID_PROFILE_BASE .. RADIANT_FIELD_ID_PROFILE_MAX (0x00-0x1F),
 * leaving 0x20+ to the common-page adapter, which runs on this same source -
 * see radiant_bridge.h's block.
 *
 * 0x00-0x07: the quantities BOTH device types carry.
 * 0x08-0x0F: FE-C page 16 only.
 */
#define RADIANT_POWER_FIELD_INST_POWER  0x00u /* 0x1C active power, instantaneous, W */
#define RADIANT_POWER_FIELD_EVENT_COUNT 0x01u /* 0x36 event count, accumulating, u8 wire width */
#define RADIANT_POWER_FIELD_ENERGY      0x02u /* 0x30 energy, accumulating, integrated, exp -6 */

/*
 * RESERVED, 0x03-0x07 (decoded, not posted, so a later addition doesn't
 * renumber the three that ship today). 0x03 cadence: no vocabulary type -
 * rpm is a non-SI rate (radiant_rd_adapter.h reserves the same gap; 0x19
 * angular rate is rad/s and 1 rpm = pi/30 rad/s is irrational). 0x04 pedal
 * power balance: 0x24 percentage would fit, but the differentiation bit
 * (Rev 5.1 section 8.2.1) makes it two different numbers one series can't
 * carry. 0x05 mean power: the decoded accumulated-power field as average
 * watts, see the 0x30 discussion above. 0x06-0x07 unallocated.
 */
#define RADIANT_POWER_FIELD_CADENCE     0x03u /* RESERVED */
#define RADIANT_POWER_FIELD_BALANCE     0x04u /* RESERVED */
#define RADIANT_POWER_FIELD_MEAN_POWER  0x05u /* RESERVED */

#define RADIANT_POWER_FIELD_FEC_STATE 0x08u /* 0x40 enum, instantaneous, PROFILE_FEC_STATE_* */
#define RADIANT_POWER_FIELD_FEC_SPEED 0x09u /* 0x16 speed, instantaneous, exp -3 */

/*
 * THE EQUIPMENT TYPE IS AN IDENTITY, NOT A MEASUREMENT. Table 8-8 byte 1:
 * treadmill / elliptical / rower / climber / ski erg / indoor bike. Posted
 * on the bus rather than kept elsewhere because it is LEARNED FROM TRAFFIC -
 * there is no reading it off the channel at bind time (the distinction
 * radiant_binding.h draws for `set_period`), so it cannot be a struct
 * radiant_binding member; radiant_naming.c is the consumer and each sink
 * caches the byte itself.
 *
 * IT IS POSTED FIRST, ahead of state and speed in the same page, so a sink
 * creating an endpoint on the first page 16 already has the subtype and
 * names it correctly the first time.
 *
 * THE RULE ENGINE MUST NOT SEE IT AS AN FE STATE: both samples are
 * RADIANT_FIELD_ENUM_GENERIC on one source, so radiant_rules.c discriminates
 * on field_id as well as field_type (its rules_want()) - otherwise codes
 * 19/22/25 would read as "not IN_USE" and fight the real state sample over
 * one slot.
 */
#define RADIANT_POWER_FIELD_FEC_TYPE 0x0Du /* 0x40 enum, instantaneous, PROFILE_FEC_TYPE_* */

/*
 * RESERVED, 0x0A-0x0C and 0x0E-0x0F: FE-C page 16's other three quantities,
 * decoded and not posted. 0x0A elapsed time / 0x0B distance (both
 * accumulating, 0x37/0x34) would share radiant_rules.c's one activity slot
 * per source and fight each other - needs a per-field-type slot map first
 * (elapsed time is also session time, not duration: it only increments
 * IN_USE, section 8.5.2.2). 0x0C heart rate (0x26, instantaneous): not
 * posted, since a trainer relaying a strap's bpm would duplicate the
 * at-rest/zone-2 entities the strap's own binding already publishes.
 * 0x0E-0x0F unallocated (0x0D is the equipment type, above).
 */
#define RADIANT_POWER_FIELD_FEC_ELAPSED  0x0Au /* RESERVED */
#define RADIANT_POWER_FIELD_FEC_DISTANCE 0x0Bu /* RESERVED */
#define RADIANT_POWER_FIELD_FEC_HR       0x0Cu /* RESERVED */

/*
 * The longest interval the energy integral will charge at the closing
 * message's wattage. The adapter survives a tracking dropout by design
 * (rx_tap.c), so the first message after a re-acquire would otherwise be
 * billed for the whole silence - the same unknown stretch FE-C's 0xFFF branch
 * charges nothing, arriving with no message to say so. Longer than any gap
 * ordinary packet loss leaves between power pages on a 4 Hz stream; shorter
 * than any dropout that ends in a re-acquire.
 */
#define RADIANT_POWER_MAX_GAP_US (5u * 1000000u)

struct radiant_power_adapter {
	/* True once a power-bearing page has established the baseline: both
	 * the event count and the integration clock are set on that first
	 * message. */
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
 * NOTE that 0x10 means a DIFFERENT page here than in
 * radiant_power_adapter_decode() - page numbers are per device type. Calling
 * the wrong entry point does not fail loudly; it decodes an FE-C general
 * page as bicycle power and reports a speed as watts. The dispatcher owns
 * getting this right.
 *
 * Returns samples posted: up to 3 for page 0x10 (equipment type, state,
 * speed - the speed is absent when the equipment marks it invalid), 1 or 3 for
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
