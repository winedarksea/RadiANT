/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_env_adapter.h - ANT+ Environment (device type 0x19) onto the sample
 * bus.
 *
 * Wire layout is src/profiles/profile_env.h's, decoded through
 * profile_env_decode_temperature()/_capabilities() rather than re-derived
 * here. Vocabulary mapping follows docs/radiant-bridge.md section 3.1's "one
 * adapter per profile, not one per profile per sink".
 *
 * No radiant header, direct or transitive - the same claim
 * radiant_hr_adapter.h and radiant_rd_adapter.h make.
 *
 * ---------------------------------------------------------------------------
 * The one field that is published, and why the conversion is exact
 * ---------------------------------------------------------------------------
 * The current temperature arrives as a signed count of 0.01 degC.
 * RADIANT_FIELD_TEMPERATURE's canonical unit is KELVIN, and the offset between
 * the two scales is 273.15 K - which is a whole number of hundredths. So
 *
 *     raw = centi_C + 27315,  exp = -2
 *
 * is EXACT: no rounding, no accumulated drift, no scale that only nearly
 * divides. That is not the usual case in this codebase (see
 * radiant_hr_adapter.c's 1/1024 s trap, and package C's wind conversions where
 * 0.01 km/h = 1/360 m/s can never be exact in the vocabulary's decimal
 * raw x 10^exp form), so it is worth saying plainly rather than leaving the
 * next reader to check.
 *
 * The event count is published as RADIANT_FIELD_EVENT_COUNT, accumulating, and
 * differenced at its own u8 wire width before anything else happens to it -
 * radiant_hr_adapter.c's section 3.2 rule, which unsigned subtraction at that
 * width satisfies by construction with no wraparound special case.
 *
 * ---------------------------------------------------------------------------
 * WHY THE 24-HOUR LOW AND HIGH ARE DECIDEDLY NOT PUBLISHED
 * ---------------------------------------------------------------------------
 * They are decoded - a caller can read them straight out of the struct
 * profile_env_temperature this adapter keeps, so nothing is silently dropped -
 * and they are deliberately never posted to the bus.
 *
 * They are STATISTICS OVER A 24-HOUR WINDOW, not measurements of the present.
 * Every consumer downstream of the bus treats a RADIANT_FIELD_TEMPERATURE
 * record as a live reading: radiant_matter.c maps it to a Matter Temperature
 * Measurement cluster and allocates an endpoint per (source, field_id), so
 * posting these two would mint TWO EXTRA Matter Temperature Sensor endpoints
 * per environment sensor - three thermometers where the user has one, two of
 * which read like live values, never move for hours, and would drive
 * automations from yesterday's weather. radiant_liveness.c would also expire
 * them on the live channel's period, marking a perfectly current 24-hour
 * statistic STALE.
 *
 * This is the same judgement radiant_rd_adapter.h records for cadence and
 * ground contact time, reached for a different reason: there the vocabulary
 * has no type, here the type would be actively misleading. Their field_ids are
 * reserved below so that a later decision to publish them - as a distinct
 * field type, or with a "statistic" flag the vocabulary does not yet have -
 * does not renumber the two fields that ship today.
 *
 * ---------------------------------------------------------------------------
 * Page 0 is stored, not posted
 * ---------------------------------------------------------------------------
 * The capabilities page is a description of the sensor, not a reading, so it
 * has no place on a bus of measurements. It is kept because it carries the
 * declared default transmission rate, and this profile is the one that has two
 * (0.5 Hz and 4 Hz) - see profile_env.h's header on the liveness consequence.
 */

#ifndef RADIANT_ENV_ADAPTER_H_
#define RADIANT_ENV_ADAPTER_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_env.h"
#include "radiant_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * field_id assignments within one Environment source. Stable: a sink keys
 * history on (source, field_id), so renumbering these breaks every sink's
 * stored series.
 *
 * All four are inside RADIANT_FIELD_ID_PROFILE_BASE..RADIANT_FIELD_ID_PROFILE_MAX
 * (0x00-0x1F). The block above that, 0x20-0x3F, belongs to the common-page
 * adapter, which runs on this channel too - see radiant_bridge.h. A profile
 * adapter reaching into it would silently merge two different series onto one
 * source.
 */
#define RADIANT_ENV_FIELD_TEMPERATURE 0u /* 0x10 temperature, K, exp -2 */
#define RADIANT_ENV_FIELD_EVENT_COUNT 1u /* 0x36 event count, accumulating, u8 wire width */
#define RADIANT_ENV_FIELD_LOW_24H     2u /* RESERVED - decoded, deliberately not posted */
#define RADIANT_ENV_FIELD_HIGH_24H    3u /* RESERVED - decoded, deliberately not posted */

/* The exact offset from degC to K in hundredths, the whole of this adapter's
 * unit conversion. Named rather than inlined so the test asserts the same
 * number the code uses. */
#define RADIANT_ENV_KELVIN_OFFSET_CENTI 27315

struct radiant_env_adapter {
	/* The sensor's last page 0x01, kept whole so a caller can read the two
	 * 24-hour statistics this adapter does not publish. */
	struct profile_env_temperature t;
	bool have_temperature;

	/* The sensor's last page 0x00. Not posted; see the header. */
	struct profile_env_capabilities caps;
	bool have_caps;

	/* Event count as last seen on the wire, for differencing in its own u8
	 * width (radiant_hr_adapter.h - converting before differencing is the
	 * bug, and here there is nothing to convert, which is why the raw byte
	 * is what gets stored). */
	bool    have_prev;
	uint8_t prev_event_count;

	/* The exact running total, which is what is actually published:
	 * RADIANT_SAMPLE_ACCUMULATING means "raw is a monotone counter, not an
	 * instant" (radiant_bridge.h), and radiant_rules.c differences what it
	 * receives. A published delta would be differenced a second time, and
	 * this counter's delta is the constant 1 - so the source would read as
	 * permanently idle. Same discipline as radiant_hr_adapter.h's
	 * acc_1024: difference at the wire width, accumulate exactly, publish
	 * the accumulation. */
	uint64_t acc_events;
};

void radiant_env_adapter_init(struct radiant_env_adapter *a);

/*
 * Decodes one ANT+ Environment message and posts what it carries. Safe from a
 * radio ISR (bounded work, no allocation, no blocking). `body8` is the full
 * 8-byte payload and `t_us` is the t_sync it was heard at.
 *
 * Page 0x01 posts up to two records: the current temperature (omitted when the
 * sensor marked it invalid - never published as a zero, which would read as
 * 273.15 K), and the event-count delta (omitted on the first page 0x01 an
 * instance sees, since there is no prior reading to difference against - the
 * same first-call behaviour as radiant_hr_adapter_decode()).
 *
 * Page 0x00 is absorbed into the adapter's state and posts nothing, returning
 * 0. Every other page - including reserved 0x02..0x3F and the common data
 * pages, which have their own adapter - is ignored and returns 0.
 *
 * Returns the number of samples posted.
 */
uint32_t radiant_env_adapter_decode(struct radiant_env_adapter *a, uint32_t source,
				    const uint8_t body8[8], uint64_t t_us);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_ENV_ADAPTER_H_ */
