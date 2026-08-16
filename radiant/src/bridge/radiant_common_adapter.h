/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_common_adapter.h - ANT+ Common Data Pages (0x40-0x5D) onto the
 * sample bus.
 *
 *
 * No radiant header, direct or transitive - one adapter per profile, not one
 * per profile per sink. Safe from a radio ISR: no allocation, no logging, no
 * blocking, bounded work.
 *
 * ---------------------------------------------------------------------------
 * THIS ADAPTER IS NOT LIKE THE OTHERS, AND THE DIFFERENCE IS THE WHOLE POINT
 * ---------------------------------------------------------------------------
 * Every other adapter in this directory is chosen by the binding's device
 * type: HR 0x78 gets radiant_hr_adapter, and a binding has exactly one device
 * type, so exactly one adapter ever writes a given source.
 *
 * Table 4-1 says of pages 0x40-0x5D, verbatim: "Use of these pages is defined
 * by the transmission type of the ANT channel parameter." Not the device type.
 * So a heart-rate strap, a power meter or a bike light may all carry page 84's
 * weather data, and THIS adapter therefore runs on EVERY bound channel,
 * alongside whichever profile adapter owns that channel's device type. Two
 * producers, one source - the one case radiant_bridge.h's field_id block
 * exists to make safe, and the reason its 0x20-0x3F range is reserved.
 *
 * The dispatch that makes this true is in apps/dongle_thread/src/rx_tap.c:
 * the common-range test runs BEFORE the devtype switch and returns. Deleting
 * it would not break any test in radiant/tests - this adapter is reachable
 * directly - it would just quietly make weather data unreachable on every
 * channel that is not an environment sensor, which is the exact bug finding 1
 * of the plan describes.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT DECODES, AND WHAT IT DECLINES
 * ---------------------------------------------------------------------------
 * Page 84 only. The other pages in the common range are identity (80/81),
 * battery (82), time (83), memory (85), paired devices (86), errors (87) and
 * the request/command surface (70/71/73/74/76) - none of them a MEASUREMENT,
 * and the sample bus carries measurements. radiant_common_adapter_decode()
 * returning 0 for page 0x50 is correct behaviour, not a gap.
 *
 * Declining a subpage is likewise a normal answer, not a failure - the same
 * voice radiant_matter.h uses about radiant_matter_row() returning NULL.
 * Subpages 9-254 are reserved by Table 6-17 and 255 is the stated
 * "slot carries nothing" sentinel; a sensor sending either is behaving.
 *
 * ---------------------------------------------------------------------------
 * TWO LOSSY CONVERSIONS - A FIRST FOR THIS CODEBASE - AND WHY SECTION 3.2 DOES
 * NOT APPLY TO THEM
 * ---------------------------------------------------------------------------
 * docs/radiant-bridge.md section 3.2 (the 1/1024 s trap) is the rule every
 * reader of this directory has already met: difference at the wire width,
 * accumulate exactly, convert only at publication - because converting before
 * accumulating drifts WITHOUT BOUND. radiant_hr_adapter.c's acc_1024 is that
 * rule in code.
 *
 * WIND SPEED AND WIND DIRECTION ARE A DIFFERENT PROBLEM AND SECTION 3.2 IS THE
 * WRONG LENS FOR THEM. Nothing accumulates: both are INSTANTANEOUS quantities,
 * posted with no accumulator and no flags, each sample independent of the one
 * before. The rounding error is bounded by half a unit in the last place, on
 * that sample alone, and IT DOES NOT COMPOUND - there is no running total for
 * it to compound into. This paragraph exists because the next reader will
 * assume section 3.2 applies, reach for a 1/1024-s-shaped fix, and find there
 * is nothing to fix.
 *
 * What IS lossy is the scale itself, and it is unavoidable in the vocabulary's
 * decimal `value_SI = raw * 10^exp` form:
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
 * with no FPU guarantee, and a float here would be slower AND less predictable
 * than the exact integer round-half-up above.
 *
 * A detail worth writing down because someone will try to test it: NEITHER
 * expression has a reachable exact half-step at u16 wire width. For speed, a
 * half-step needs v*10^6 = 180 (mod 360); 10^6 = 280 (mod 360), gcd(280,360) =
 * 40, and 40 does not divide 180, so there is no solution at any v. For
 * direction the smallest solution of v*872665 = 500000 (mod 10^6) is
 * v = 100000, which does not fit in the u16 the wire carries. The tests
 * therefore pin the nearest-to-half values on each side instead, which is the
 * strongest statement the arithmetic admits.
 *
 * The other four conversions ARE exact, and are labelled as such in the code
 * so the distinction stays visible:
 *   temperature (1, 7, 8)  raw = centi_degC + 27315, exp = -2   K
 *   pressure    (2)        raw = centi_kPa,          exp =  1   Pa
 *   humidity    (3)        raw = centi_pct,          exp = -2   %
 *   cycles      (6)        raw = delta,              exp =  0   count
 */

#ifndef RADIANT_COMMON_ADAPTER_H_
#define RADIANT_COMMON_ADAPTER_H_

#include <stdbool.h>
#include <stdint.h>

#include "radiant_bridge.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * field_id allocation, from radiant_bridge.h's reserved common block
 * (RADIANT_FIELD_ID_COMMON_BASE 0x20 .. RADIANT_FIELD_ID_COMMON_MAX 0x3F)
 * ---------------------------------------------------------------------------
 *
 * One id per page-84 subpage, at RADIANT_FIELD_ID_COMMON_BASE + subpage, so
 * the mapping is a rule rather than a table and (source, field_id) stays
 * stable for the life of a sink's stored history. A sink keys on that pair;
 * renumbering these breaks every stored series, exactly as radiant_hr_adapter.h
 * says of its own three.
 *
 * The block holds 32 ids and the base offset consumes one, so subpages 1..31
 * are expressible and 32..254 are not. That is not a problem to solve today -
 * Table 6-17 defines 8 - but it IS the constraint a future subpage runs into,
 * and the answer then is a second block, not a re-base of this one.
 *
 * 0x20 itself (subpage 0) is unallocated and unreachable: Table 6-16 states the
 * subpage value range as 1-254, so 0 never arrives.
 */
#define RADIANT_COMMON_FIELD_TEMPERATURE    (RADIANT_FIELD_ID_COMMON_BASE + 1u) /* 0x21 - 0x10 K */
#define RADIANT_COMMON_FIELD_PRESSURE       (RADIANT_FIELD_ID_COMMON_BASE + 2u) /* 0x22 - 0x12 Pa */
#define RADIANT_COMMON_FIELD_HUMIDITY       (RADIANT_FIELD_ID_COMMON_BASE + 3u) /* 0x23 - 0x11 % */
#define RADIANT_COMMON_FIELD_WIND_SPEED     (RADIANT_FIELD_ID_COMMON_BASE + 4u) /* 0x24 - 0x16 m/s, lossy */
#define RADIANT_COMMON_FIELD_WIND_DIRECTION (RADIANT_FIELD_ID_COMMON_BASE + 5u) /* 0x25 - 0x18 rad, lossy */
#define RADIANT_COMMON_FIELD_CHARGE_CYCLES  (RADIANT_FIELD_ID_COMMON_BASE + 6u) /* 0x26 - 0x36 count, accumulating */
#define RADIANT_COMMON_FIELD_TEMP_MIN       (RADIANT_FIELD_ID_COMMON_BASE + 7u) /* 0x27 - 0x10 K */
#define RADIANT_COMMON_FIELD_TEMP_MAX       (RADIANT_FIELD_ID_COMMON_BASE + 8u) /* 0x28 - 0x10 K */

/* Highest subpage this adapter knows how to post. Anything above it is
 * Table 6-17's reserved range and is declined; `declined` below counts it. */
#define RADIANT_COMMON_SUBPAGE_MAX 8u

struct radiant_common_adapter {
	/*
	 * Subpage 6, charging cycles, is the ONLY accumulating quantity page 84
	 * carries: "The number of times the device has been fully charged",
	 * 0-65535, a running total on the wire rather than a delta. Section
	 * 3.2's differencing rule (not its conversion rule - see the header)
	 * applies to it in full: difference at the field's own u16 wire width,
	 * where unsigned subtraction wraps correctly by construction, so a
	 * sensor that reaches 65535 and rolls over reports 1 more cycle rather
	 * than 65535 fewer.
	 *
	 * The first sighting only establishes prev - there is nothing to
	 * difference against yet, and posting the absolute total once would put
	 * one enormous fake delta at the head of every sink's series.
	 */
	bool     have_prev_cycles;
	uint16_t prev_cycles;

	/* The exact running total from those deltas, and what is actually
	 * published. RADIANT_SAMPLE_ACCUMULATING means "raw is a monotone
	 * counter, not an instant" (radiant_bridge.h) and radiant_rules.c
	 * differences what it receives, so a published delta gets differenced
	 * twice. Counted from this adapter's first sighting, not from the
	 * sensor's lifetime total - which is why the first sighting still only
	 * establishes prev_cycles rather than seeding this with the wire
	 * value. */
	uint64_t acc_cycles;

	/*
	 * Last-seen raw wire value per known subpage, indexed by subpage
	 * (slot 0 unused, so the array reads as last_raw[SUBPAGE_x]).
	 *
	 * THE ADAPTER DOES NOT DEADBAND ON THESE, deliberately. Suppressing an
	 * unchanged sample here would suppress the arrival itself, and
	 * radiant_liveness.c decides a source has gone quiet from arrivals - a
	 * thermometer in a stable room would be declared STALE for being
	 * accurate. The deadband belongs beside the conversion table in
	 * radiant_matter.c, where it can be per-cluster and paired with a
	 * heartbeat. What lives here is the state that decision needs, and the
	 * tests read it directly.
	 */
	bool     have_last[RADIANT_COMMON_SUBPAGE_MAX + 1u];
	uint16_t last_raw[RADIANT_COMMON_SUBPAGE_MAX + 1u];

	/* Reserved and invalid subpages seen, per source. Not a failure count -
	 * a reserved subpage is a normal answer (see the header) - but a
	 * non-zero value here on a real sensor is the first sign that Table 6-17
	 * has grown a row this decoder predates. */
	uint32_t declined;
};

void radiant_common_adapter_init(struct radiant_common_adapter *a);

/*
 * True for the ANT+ Common Data Page range, Table 4-1: 0x40-0x5D inclusive.
 *
 * The caller uses this to decide WHETHER to consult the common path at all,
 * before it dispatches on the binding's device type - see rx_tap.c. It is
 * deliberately the whole range and not "== 0x54": the range is what Table 4-1
 * says is keyed on transmission type, and a device-type decoder must not see
 * ANY of it. Whether the page inside the range is one this adapter posts
 * anything for is decode()'s business.
 */
bool radiant_common_adapter_is_common_page(uint8_t page);

/*
 * Decodes one common-page broadcast and posts what it recognises via
 * radiant_bridge_post(). `body8` is the full 8-byte payload; `t_us` is the
 * t_sync the message was heard at. Safe from a radio ISR.
 *
 * Returns the number of samples posted, which is legitimately 0 for: a page
 * outside the common range, a common page that is not 84, both slots carrying
 * reserved or invalid subpages, and the first sighting of a charging-cycles
 * slot. None of those is an error and none is logged.
 */
uint32_t radiant_common_adapter_decode(struct radiant_common_adapter *a, uint32_t source,
				       const uint8_t body8[8], uint64_t t_us);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_COMMON_ADAPTER_H_ */
