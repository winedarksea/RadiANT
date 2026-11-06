/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_common_adapter.h - ANT+ Common Data Pages (0x40-0x5D) onto the
 * sample bus.
 *
 * No radiant header, direct or transitive - one adapter per profile, not one
 * per profile per sink. Safe from a radio ISR: no allocation, no logging, no
 * blocking, bounded work.
 *
 * ---------------------------------------------------------------------------
 * Not chosen by device type, unlike the others
 * ---------------------------------------------------------------------------
 * Every other adapter here is chosen by the binding's device type, so
 * exactly one adapter ever writes a given source. Table 4-1 says of pages
 * 0x40-0x5D, verbatim: "Use of these pages is defined by the transmission
 * type of the ANT channel parameter" - not the device type. So a heart-rate
 * strap, a power meter or a bike light may all carry page 84's weather data,
 * and this adapter runs on EVERY bound channel, alongside whichever profile
 * adapter owns that channel's device type. Two producers, one source - the
 * case radiant_bridge.h's field_id block exists to make safe, and the reason
 * its 0x20-0x3F range is reserved.
 *
 * apps/dongle_thread/src/rx_tap.c runs the common-range test BEFORE the
 * devtype switch, and returns there. This adapter is also reachable
 * directly, so no test in radiant/tests depends on that ordering - but
 * removing it would quietly make weather data unreachable on every channel
 * that is not an environment sensor.
 *
 * ---------------------------------------------------------------------------
 * What it decodes, and what it declines
 * ---------------------------------------------------------------------------
 * Page 84 only. The other pages in the common range are identity (80/81),
 * battery (82), time (83), memory (85), paired devices (86), errors (87) and
 * the request/command surface (70/71/73/74/76) - none of them a MEASUREMENT,
 * and the sample bus carries measurements.
 *
 * Declining a subpage is likewise a normal answer, not a failure. Subpages
 * 9-254 are reserved by Table 6-17 and 255 is the stated "slot carries
 * nothing" sentinel; a sensor sending either is behaving.
 *
 * ---------------------------------------------------------------------------
 * Subpages 7 and 8 are decoded, not posted
 * ---------------------------------------------------------------------------
 * Minimum and maximum operating temperature are NOT THE PRESENT TEMPERATURE,
 * on either reading of Table 6-17. The row NAMES say "Operating Temperature",
 * i.e. the device's rated limits; the (transposed - profile_common.h defect
 * (a)) description cells say "recorded temperature", i.e. extremes the device
 * has seen. A rated limit is a constant and a recorded extreme is a statistic
 * over an unstated window; neither is a measurement of the room. They are
 * decoded (last_raw[7] and last_raw[8] below hold them, exactly as received)
 * but deliberately never posted.
 *
 * Every consumer downstream treats a RADIANT_FIELD_TEMPERATURE record as a
 * live reading, and posting these two cost three things at once:
 *
 *  - radiant_matter.c allocates a Matter Temperature Measurement endpoint per
 *    (source, field_id), so a sensor that sends its limits minted TWO EXTRA
 *    endpoints - a "temperature" of -20 degC and one of +60 degC, neither of
 *    which tracks the room - against RADIANT_MATTER_MAX_ENDPOINTS' budget of 16,
 *    which already does not close in the worst case.
 *  - apps/dongle_thread's MQTT sink announced two more Home Assistant
 *    entities with `device_class: temperature`, which an automation keyed on
 *    "any temperature in this room" would happily read.
 *  - radiant_liveness.c keeps a last-sample slot per (source, field_id) and
 *    re-posts each one STALE when the source goes quiet, so two constants
 *    held two of its table's slots and "expired" with the sensor - a limit or
 *    a recorded extreme is no less true for the sensor being out of range,
 *    and marking it stale says that it is.
 *
 * Same judgement, and very nearly the same argument, as radiant_env_adapter.h's
 * "WHY THE 24-HOUR LOW AND HIGH ARE DECIDEDLY NOT PUBLISHED": a number with
 * the right unit and the wrong meaning is worse on a bus of measurements than
 * no number. The two field ids below stay reserved, so a later decision to
 * publish them - as a description of the sensor, on some channel that is not
 * this one - renumbers nothing.
 *
 * Not counted in `declined` either: that counter is for subpages this decoder
 * does not know, and these two are known and refused on purpose.
 *
 * ---------------------------------------------------------------------------
 * Two lossy conversions, and why section 3.2 does not apply to them
 * ---------------------------------------------------------------------------
 * docs/radiant-bridge.md section 3.2 (the 1/1024 s trap): difference at the
 * wire width, accumulate exactly, convert only at publication, because
 * converting before accumulating drifts without bound. radiant_hr_adapter.c's
 * acc_1024 is that rule in code.
 *
 * Wind speed and wind direction are a different problem: nothing
 * accumulates. Both are INSTANTANEOUS quantities, posted with no accumulator
 * and no flags, each sample independent of the one before. The rounding
 * error is bounded to half a unit in the last place on that sample alone,
 * and it does not compound - there is no running total for it to compound
 * into.
 *
 * What IS lossy is the scale itself, unavoidable in the vocabulary's decimal
 * `value_SI = raw * 10^exp` form:
 *
 *   Speed. 0.01 km/h = 10/3600 m/s = 1/360 m/s exactly. For the conversion to
 *   be exact we would need 10^k / 360 to be an integer for some k; 360 = 8*9*5
 *   has a factor of 3 and 10^k has none, so no k works. Ever. We take
 *   exp = -6 and round half up:
 *       raw = (v * 1000000 + 180) / 360
 *   Worst-case error 0.5 urad-equivalent, i.e. 5e-7 m/s, against a wire
 *   resolution of 1/360 = 2.8e-3 m/s. Five thousand times finer than the
 *   sensor's own quantisation.
 *
 *   Direction. 0.05 deg = 0.05 * pi/180 rad = 872.66462...e-6 rad, irrational,
 *   so no (raw, exp) pair is exact for any scale. We take the 6-digit
 *   truncation 872665e-9 and round half up:
 *       raw = (v * 872665 + 500000) / 1000000
 *   Error from the constant itself is under 4e-10 rad per 0.05 deg step, so
 *   even at the maximum legal 7199 steps (359.95 deg) the constant contributes
 *   under 3e-6 rad - a thousandth of one wire step.
 *
 * Both are integer arithmetic on purpose: this runs in an ISR on a Cortex-M
 * with no FPU guarantee, and a float here would be slower and less
 * predictable than the exact integer round-half-up above.
 *
 * Neither expression has a reachable exact half-step at u16 wire width: for
 * speed, v*10^6 = 180 (mod 360) has no solution since gcd(280,360) = 40 does
 * not divide 180; for direction the smallest solution needs v = 100000,
 * which overflows the u16 wire width. Tests pin the nearest-to-half values
 * on each side instead.
 *
 * The other four conversions ARE exact, and are labelled as such in the code
 * so the distinction stays visible:
 *   temperature (1)        raw = centi_degC + 27315, exp = -2   K
 *   pressure    (2)        raw = centi_kPa,          exp =  1   Pa
 *   humidity    (3)        raw = centi_pct,          exp = -2   %
 *   cycles      (6)        raw = delta,              exp =  0   count
 *
 * ---------------------------------------------------------------------------
 * Shared conventions for this directory's adapters
 * ---------------------------------------------------------------------------
 * The power, env, hr and rd adapters each point back here for three things
 * they would otherwise restate in full:
 *
 * field_id stability. Assignments within one source are stable: a sink keys
 * its stored history on the pair (source, field_id). Renumbering an id
 * breaks every sink's stored series, so a reserved-but-unpublished field
 * keeps its slot rather than being reused.
 *
 * Accumulator discipline (docs/radiant-bridge.md section 3.2). Difference at
 * the field's own WIRE width - unsigned subtraction there wraps correctly by
 * construction, so a rollover needs no special case - accumulate the deltas
 * into a uint64_t, and publish that running TOTAL, never the per-message
 * delta. RADIANT_SAMPLE_ACCUMULATING means "raw is a monotone counter, not an
 * instant" (radiant_bridge.h), and radiant_rules.c differences what it
 * receives; a published delta would be differenced a second time, and two
 * equal consecutive deltas - the steady state of any real periodic
 * accumulator - would then read as "not advancing".
 *
 * First-call baseline. The first message an adapter instance sees has no
 * previous raw value to difference against, so it publishes no accumulator
 * delta - it only records the baseline. Any instantaneous field on that same
 * message is published as normal.
 */

#ifndef RADIANT_COMMON_ADAPTER_H_
#define RADIANT_COMMON_ADAPTER_H_

#include <stdbool.h>
#include <stdint.h>

#include "radiant_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * field_id allocation, from radiant_bridge.h's reserved common block
 * (RADIANT_FIELD_ID_COMMON_BASE 0x20 .. RADIANT_FIELD_ID_COMMON_MAX 0x3F).
 *
 * One id per page-84 subpage, at RADIANT_FIELD_ID_COMMON_BASE + subpage, so
 * the mapping is a rule rather than a table (field_id stability: see above).
 * The block holds 32 ids and the base offset consumes one, so subpages 1..31
 * are expressible and 32..254 are not - not a problem today (Table 6-17
 * defines 8), but the answer for a future subpage is a second block, not a
 * re-base of this one. 0x20 itself (subpage 0) is unreachable: Table 6-16
 * states the subpage range as 1-254.
 */
#define RADIANT_COMMON_FIELD_TEMPERATURE    (RADIANT_FIELD_ID_COMMON_BASE + 1u) /* 0x21 - 0x10 K */
#define RADIANT_COMMON_FIELD_PRESSURE       (RADIANT_FIELD_ID_COMMON_BASE + 2u) /* 0x22 - 0x12 Pa */
#define RADIANT_COMMON_FIELD_HUMIDITY       (RADIANT_FIELD_ID_COMMON_BASE + 3u) /* 0x23 - 0x11 % */
#define RADIANT_COMMON_FIELD_WIND_SPEED     (RADIANT_FIELD_ID_COMMON_BASE + 4u) /* 0x24 - 0x16 m/s, lossy */
#define RADIANT_COMMON_FIELD_WIND_DIRECTION (RADIANT_FIELD_ID_COMMON_BASE + 5u) /* 0x25 - 0x18 rad, lossy */
#define RADIANT_COMMON_FIELD_CHARGE_CYCLES  (RADIANT_FIELD_ID_COMMON_BASE + 6u) /* 0x26 - 0x36 count, accumulating */
#define RADIANT_COMMON_FIELD_TEMP_MIN       (RADIANT_FIELD_ID_COMMON_BASE + 7u) /* 0x27 RESERVED - decoded, deliberately not posted */
#define RADIANT_COMMON_FIELD_TEMP_MAX       (RADIANT_FIELD_ID_COMMON_BASE + 8u) /* 0x28 RESERVED - decoded, deliberately not posted */

/* Highest subpage this adapter knows. Anything above it is Table 6-17's
 * reserved range and is declined; `declined` below counts it. Knowing a
 * subpage is not the same as posting it: 7 and 8 are known and not posted
 * (see the header). */
#define RADIANT_COMMON_SUBPAGE_MAX 8u

struct radiant_common_adapter {
	/*
	 * Subpage 6, charging cycles, is the ONLY accumulating quantity page 84
	 * carries: "The number of times the device has been fully charged",
	 * 0-65535, a running total on the wire rather than a delta.
	 * Accumulator discipline: see the header above - a sensor that reaches
	 * 65535 and rolls over reports 1 more cycle rather than 65535 fewer.
	 *
	 * The first sighting only establishes prev (first-call baseline, see
	 * above); posting the absolute total once would put one enormous fake
	 * delta at the head of every sink's series.
	 */
	bool     have_prev_cycles;
	uint16_t prev_cycles;

	/* The exact running total from those deltas, and what is actually
	 * published (see above). Counted from this adapter's first sighting,
	 * not from the sensor's lifetime total. */
	uint64_t acc_cycles;

	/*
	 * Last-seen raw wire value per known subpage (slot 0 unused).
	 *
	 * THE ADAPTER DOES NOT DEADBAND ON THESE, deliberately: suppressing an
	 * unchanged sample here would suppress the arrival itself, and
	 * radiant_liveness.c decides a source has gone quiet from arrivals - a
	 * thermometer in a stable room would be declared STALE for being
	 * accurate. The deadband belongs beside the conversion table in
	 * radiant_matter.c instead, where it can be per-cluster and paired
	 * with a heartbeat.
	 */
	bool     have_last[RADIANT_COMMON_SUBPAGE_MAX + 1u];
	uint16_t last_raw[RADIANT_COMMON_SUBPAGE_MAX + 1u];

	/* Reserved and invalid subpages seen, per source. Not a failure count
	 * - a reserved subpage is a normal answer - but a non-zero value here
	 * on a real sensor is the first sign Table 6-17 has grown a row this
	 * decoder predates. */
	uint32_t declined;
};

void radiant_common_adapter_init(struct radiant_common_adapter *a);

/*
 * True for the ANT+ Common Data Page range, Table 4-1: 0x40-0x5D inclusive.
 *
 * The caller uses this to decide WHETHER to consult the common path at all,
 * before it dispatches on the binding's device type - see rx_tap.c. It is
 * deliberately the whole range and not "== 0x54": a device-type decoder must
 * not see ANY of it. Whether the page inside the range is one this adapter
 * posts anything for is decode()'s business.
 */
bool radiant_common_adapter_is_common_page(uint8_t page);

/*
 * Decodes one common-page broadcast and posts what it recognises via
 * radiant_bridge_post(). `body8` is the full 8-byte payload; `t_us` is the
 * t_sync the message was heard at. Safe from a radio ISR.
 *
 * Returns the number of samples posted, which is legitimately 0 for: a page
 * outside the common range, a common page that is not 84, both slots carrying
 * reserved or invalid subpages, the first sighting of a charging-cycles slot,
 * and slots carrying subpage 7 or 8 (decoded, never posted - see the header).
 * None of those is an error and none is logged.
 */
uint32_t radiant_common_adapter_decode(struct radiant_common_adapter *a, uint32_t source,
				       const uint8_t body8[8], uint64_t t_us);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_COMMON_ADAPTER_H_ */
