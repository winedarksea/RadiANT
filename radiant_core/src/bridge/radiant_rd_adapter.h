/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_rd_adapter.h - ANT+ Running Dynamics (device type 0x1E) onto the
 * sample bus.
 *
 * Wire layout is src/profiles/profile_rd.h's, decoded through
 * profile_rd_decode_a()/_b() rather than re-derived here. Vocabulary mapping
 * follows docs/radiant-bridge.md section 3.1's "one adapter per profile,
 * not one per profile per sink".
 *
 * No radiant_core header, direct or transitive - same claim
 * radiant_hr_adapter.h makes.
 *
 * Two of the eight metrics are decoded but deliberately not published:
 * cadence (strides/min) and ground contact time (ms) have no home in the
 * section 7 vocabulary (mirrored across docs/radiant-telemetry.md section 7,
 * tools/ant_pages.py, radiant_bridge.h and the registry - allocating a type
 * in only one would be exactly the drift the mirror exists to prevent).
 * Ground contact time is the same unallocated slot as 0x27 time interval;
 * cadence is a second non-SI-rate gap like 0x26 heart rate. Both are still
 * readable from the struct profile_rd_metrics this adapter fills (hence
 * radiant_rd_adapter_decode() takes and keeps one rather than decoding into
 * a local) - nothing silently dropped.
 *
 * The six that do map are exact, not approximated:
 *
 *   vertical oscillation  0x14 length      raw in micrometres, exp -6 (a
 *                                          quarter-millimetre is 250 um, so no
 *                                          rounding happens anywhere)
 *   step length           0x14 length      raw in millimetres, exp -3
 *   stance time           0x24 percentage  raw in quarter-percent * 25, exp -2
 *   ground contact bal.   0x24 percentage  raw in 1/32 % * 3125, exp -5
 *   vertical ratio        0x24 percentage  raw in 1/32 % * 3125, exp -5
 *   step count            0x36 event count accumulating, 7-bit wire width
 *
 * The two percentage scales differ because the two wire scales do: 1/4 % is
 * exactly 25 * 10^-2 and 1/32 % is exactly 3125 * 10^-5. Neither loses a bit,
 * which is the property that made 0x24 usable for both.
 */

#ifndef RADIANT_RD_ADAPTER_H_
#define RADIANT_RD_ADAPTER_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_rd.h"
#include "radiant_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* field_id assignments within one RD source. Stable: a sink keys history on
 * (source, field_id), so renumbering these breaks every sink's stored series.
 * Gaps at 1 and 3 are reserved for cadence/GCT so a future vocabulary
 * allocation doesn't renumber the six that ship today. */
#define RADIANT_RD_FIELD_VERTICAL_OSC 0u /* 0x14 length, instantaneous */
#define RADIANT_RD_FIELD_CADENCE      1u /* RESERVED - no vocabulary type yet */
#define RADIANT_RD_FIELD_STANCE_TIME  2u /* 0x24 percentage, instantaneous */
#define RADIANT_RD_FIELD_GCT          3u /* RESERVED - no vocabulary type yet */
#define RADIANT_RD_FIELD_STEP_COUNT   4u /* 0x36 event count, accumulating */
#define RADIANT_RD_FIELD_BALANCE      5u /* 0x24 percentage, instantaneous */
#define RADIANT_RD_FIELD_VERT_RATIO   6u /* 0x24 percentage, instantaneous */
#define RADIANT_RD_FIELD_STEP_LENGTH  7u /* 0x14 length, instantaneous */

struct radiant_rd_adapter {
	/* The sensor's last complete picture: pages 0x00/0x01 carry disjoint
	 * field sets (unlike heart rate's repeated tail), so this struct is
	 * where the two halves meet. */
	struct profile_rd_metrics m;
	bool seen_a;
	bool seen_b;

	/* Step count as last seen on the wire, for differencing in its own
	 * 7-bit width (see radiant_hr_adapter.h - converting before
	 * differencing is the bug). */
	bool    have_prev_steps;
	uint8_t prev_step_count;
};

void radiant_rd_adapter_init(struct radiant_rd_adapter *a);

/*
 * Decodes one ANT+ running-dynamics message and posts what it carries.
 * `body8` is the full 8-byte payload; pages 0x00/0x01 are decoded, every
 * other page (0x10/0x20/0x4A included - display conversation, not
 * measurement) is ignored and returns 0. `t_us` is the t_sync heard at.
 *
 * Returns samples posted: up to 3 for page 0x00, up to 3 for page 0x01,
 * fewer when a field is marked invalid (never published as a zero). The
 * first page 0x00 an instance sees publishes no step count - no prior
 * reading to difference against yet, same as radiant_hr_adapter_decode().
 */
uint32_t radiant_rd_adapter_decode(struct radiant_rd_adapter *a, uint32_t source,
				   const uint8_t body8[8], uint64_t t_us);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RD_ADAPTER_H_ */
