/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_hr_adapter.h - ANT+ Heart Rate (device type 0x78) onto the sample
 * bus.
 *
 * Provenance: docs/radiant-bridge.md section 3.1's table and section 3.2's
 * 1/1024 s trap, both transcribed rather than redesigned. The wire layout it
 * reads - bytes [4..7] are the same event time, beat count and computed heart
 * rate on every page - is src/profiles/profile_hr.h's own documented fact
 * (profile_hr_encode(), the master-side encoder this decodes the inverse of),
 * so this file decodes from ANY heart-rate page, not only page 0x00.
 *
 * NO RADIANT_CORE HEADER, and none transitively - matching profile_hr.h's own
 * claim about itself. This module knows one thing radiant_core does not need
 * to: how to turn a heart-rate page into vocabulary. It is the "one adapter
 * per profile, not one per profile per sink" file section 3.1 describes.
 */

#ifndef RADIANT_HR_ADAPTER_H_
#define RADIANT_HR_ADAPTER_H_

#include <stdbool.h>
#include <stdint.h>

#include "radiant_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* field_id assignments within one HR source. Stable: a sink keys history on
 * (source, field_id), so renumbering these breaks every sink's stored series. */
#define RADIANT_HR_FIELD_COMPUTED_BPM 0u /* 0x26 heart rate, instantaneous */
#define RADIANT_HR_FIELD_BEAT_COUNT   1u /* 0x36 event count, accumulating, u8 wire width */
#define RADIANT_HR_FIELD_BEAT_TIME_MS 2u /* 0x37 duration, accumulating, converted from 1/1024 s */

struct radiant_hr_adapter {
	bool     have_prev;
	uint16_t prev_event_time; /* raw ANT 1/1024 s counter, as last seen on the wire */
	uint8_t  prev_beat_count; /* raw ANT beat count, as last seen on the wire */

	/*
	 * Exact accumulator in 1/1024 s units, per section 3.2: "difference in
	 * the field's own width first... accumulate exactly, and convert only
	 * at publication." u64 so it never itself needs a second accumulator.
	 */
	uint64_t acc_1024;
};

void radiant_hr_adapter_init(struct radiant_hr_adapter *a);

/*
 * Decode the common tail of one ANT+ heart-rate message (bytes [4..7]:
 * event_time LE, beat_count, computed_hr - see profile_hr_encode()) and post
 * up to three records to the bus via radiant_bridge_post(). Safe to call from
 * a radio ISR, since radiant_bridge_post() is.
 *
 * `body8` is the full 8-byte ANT payload; only bytes [4..7] are read, so the
 * caller does not need to know or care which page arrived. `t_us` is the
 * t_sync the message was heard at.
 *
 * The FIRST message an adapter instance sees establishes prev_event_time and
 * prev_beat_count with no delta to accumulate - correct, not a special case
 * to work around: there is no prior sample to difference against, so nothing
 * before "now" happened on this bus. Returns the number of samples posted
 * (0 on the first call, up to 3 thereafter).
 */
uint32_t radiant_hr_adapter_decode(struct radiant_hr_adapter *a, uint32_t source,
				const uint8_t body8[8], uint64_t t_us);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_HR_ADAPTER_H_ */
