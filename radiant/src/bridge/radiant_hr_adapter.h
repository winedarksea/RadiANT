/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_hr_adapter.h - ANT+ Heart Rate (device type 0x78) onto the sample
 * bus.
 *
 * See docs/radiant-bridge.md section 3.1 (table) and 3.2 (the 1/1024s trap).
 * Bytes [4..7] (event time, beat count, computed hr) are the same on every
 * HR page per src/profiles/profile_hr.h's profile_hr_encode(), so this
 * decodes any heart-rate page, not just page 0x00.
 *
 * No radiant header, direct or transitive - one adapter per profile,
 * not one per profile per sink.
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
#define RADIANT_HR_FIELD_BEAT_COUNT   1u /* 0x36 event count, accumulating running total, differenced at u8 wire width */
#define RADIANT_HR_FIELD_BEAT_TIME_MS 2u /* 0x37 duration, accumulating, converted from 1/1024 s */

struct radiant_hr_adapter {
	bool     have_prev;
	uint16_t prev_event_time; /* raw ANT 1/1024 s counter, as last seen on the wire */
	uint8_t  prev_beat_count; /* raw ANT beat count, as last seen on the wire */

	/* Exact accumulator in 1/1024s units (section 3.2): difference at the
	 * field's own width, accumulate exactly, convert only at publication. */
	uint64_t acc_1024;

	/*
	 * Exact beat accumulator, same discipline as acc_1024 one line above:
	 * difference at the field's own u8 wire width, accumulate exactly here,
	 * and publish the running total. RADIANT_HR_FIELD_BEAT_COUNT used to
	 * publish the per-message DELTA instead, which contradicted
	 * radiant_bridge.h's definition of RADIANT_SAMPLE_ACCUMULATING ("raw is
	 * a monotone counter, not an instant") and disagreed with
	 * RADIANT_HR_FIELD_BEAT_TIME_MS, the other accumulator posted by the
	 * same function, which always published a running total correctly.
	 * radiant_rules.c differences consecutive raws to decide "is this
	 * accumulator advancing", so a delta made it difference a delta - and
	 * two equal consecutive deltas (the steady state of any real strap)
	 * then read as "not advancing". uint64_t rather than uint32_t so the
	 * total cannot wrap in any session length: 2^32 beats is ~136 years at
	 * 60 bpm, but a counter that can never wrap needs no reader to think
	 * about whether it can.
	 */
	uint64_t acc_beats;
};

void radiant_hr_adapter_init(struct radiant_hr_adapter *a);

/*
 * Decodes the common tail of one ANT+ heart-rate message (bytes [4..7]:
 * event_time LE, beat_count, computed_hr) and posts up to three records via
 * radiant_bridge_post(). Safe from a radio ISR. `body8` is the full 8-byte
 * payload; only [4..7] are read, so the caller need not know which page
 * arrived. `t_us` is the t_sync the message was heard at.
 *
 * The first call just establishes prev_event_time/prev_beat_count with no
 * delta (nothing to difference against yet). Returns samples posted: 0 on
 * the first call, up to 3 thereafter.
 */
uint32_t radiant_hr_adapter_decode(struct radiant_hr_adapter *a, uint32_t source,
				const uint8_t body8[8], uint64_t t_us);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_HR_ADAPTER_H_ */
