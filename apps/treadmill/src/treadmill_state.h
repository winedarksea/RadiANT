/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * treadmill_state.h - ONE struct, ONE owner, SI units.
 *
 * ─── Why a state module at all ─────────────────────────────────────────────
 *
 * This node has five outputs: ANT+ FE-C, ANT+ SDM, BLE FTMS, BLE RSC, and
 * (phase 6) whatever a dongle makes of the FE-C stream. Each wants the same
 * physical facts in a different unit at a different resolution with a different
 * sentinel. If each encoder kept its own copy, then a belt speed set once would
 * become five numbers that agree at first and then drift, and the drift would
 * be invisible from any single receiver.
 *
 * So: ONE struct, in SI-ish units chosen to be exact rather than convenient,
 * and every encoder reads it. Nothing else holds a copy. The conversions live
 * at the bottom of this header, one function per direction, each with its own
 * unit test - because "the RSC cadence is twice the SDM cadence" written down
 * in four places is the same statement made four chances to be wrong.
 *
 * ─── The units, and why these ──────────────────────────────────────────────
 *
 * Millimetres and millimetres per second, not metres and floats. Every output
 * format this node has is an integer with a fixed denominator (1/256 m/s,
 * 0.001 m/s, 0.01 km/h, 1/16 m), and integer millimetres divide into all of
 * them without a rounding mode to argue about. There is no floating point
 * anywhere in this application and that is deliberate: a treadmill's distance
 * accumulator runs for hours and a float loses its last millimetre long before
 * it loses its first metre.
 *
 * Incline is 0.01 % because that is FE-C's unit and the finer of the two (FTMS
 * is 0.1 %), so the conversion that loses information runs on the way OUT and
 * never on the way in.
 *
 * ─── No kernel here ────────────────────────────────────────────────────────
 *
 * treadmill_state.c includes no Zephyr header, takes no lock and reads no
 * clock. It is advanced by being TOLD how much time passed. That is what lets
 * apps/treadmill/tests/pages compile it and drive a whole simulated run in a
 * ztest with no radio - the same trick apps/hrm_ble/src/hrm_sim_sched.c uses
 * and for the same reason.
 *
 * ─── The concurrency rule, which is the app's and not this file's ──────────
 *
 * There is NO LOCK in here. The rule that makes that safe is stated in
 * treadmill_source.h: everything that touches this struct runs on the system
 * workqueue, and a source reporting from an ISR stores a value and submits
 * work rather than calling in. Read that header before adding a caller.
 */

#ifndef TREADMILL_STATE_H_
#define TREADMILL_STATE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The FE state values, spelled here as well so this header does not drag
 * profile_fec.h into the ztest that exercises the conversions. Asserted equal
 * to PROFILE_FEC_STATE_* in treadmill_state.c. */
#define TREADMILL_STATE_ASLEEP   1u
#define TREADMILL_STATE_READY    2u
#define TREADMILL_STATE_IN_USE   3u
#define TREADMILL_STATE_FINISHED 4u

/* "No heart rate reading", which is 0 on ANT+ heart rate and on FE-C page 16
 * is the separate sentinel 0xFF. The encoders translate; this struct carries
 * the physiology and not a wire value. */
#define TREADMILL_HR_NONE 0u

struct treadmill_state {
	/* ── The commanded and measured setpoints ───────────────────────── */

	uint32_t belt_speed_mm_s;   /* what the belt is actually doing */
	int32_t  incline_centi_pct; /* what the deck is actually at, 0.01 % */

	/* What a controller last asked for, over FE-C page 51 or the FTMS
	 * control point - the two land HERE, in one place, which is the whole
	 * symmetry this application exists to get right. The simulator ramps
	 * `incline_centi_pct` towards this; a real machine's actuator would. */
	int32_t target_incline_centi_pct;
	uint32_t target_speed_mm_s;

	/* ── The accumulators, which are what survive a lost packet ─────── */

	uint64_t distance_mm;      /* belt distance, monotone within a session */
	uint32_t elapsed_ms;       /* SESSION time: advances only while IN_USE */
	uint32_t strides;          /* one per TWO footfalls */
	uint32_t pos_vertical_mm;  /* climbed, magnitude */
	uint32_t neg_vertical_mm;  /* descended, magnitude - also positive */

	/* MILLI-kilocalories, and the finer unit is not fussiness: FE-C page
	 * 18 and FTMS both report WHOLE kcal, and at ~13 kcal/min a whole-kcal
	 * accumulator that truncated every 100 ms tick would report about a
	 * twentieth of the energy actually burned. Whole kcal is
	 * `energy_mkcal / 1000` at the point of use. */
	uint64_t energy_mkcal;

	/* Fractional carry for the accumulators above, so that integrating a
	 * 3000 mm/s belt on a 100 ms tick does not lose 0.4 mm per tick and
	 * about 14 metres per hour. Not part of the reported state. */
	uint32_t distance_carry_um;   /* micrometres, 0..999 */
	uint32_t vertical_carry;      /* units of 1/10000 mm - the denominator
				       * of `mm x centi_percent`, not a length */
	/* The REMAINDER of the energy divide, in the numerator's own units
	 * (1/1.2e9 of a milli-kcal). Carried rather than dropped for the same
	 * reason distance_carry_um is: a tick's worth of rounding is nothing
	 * and an hour's worth of it is a visible error. */
	uint32_t energy_carry;

	/* ── The instantaneous readings ─────────────────────────────────── */

	/* 1/16 strides per minute, which is SDM's resolution and the finest of
	 * the three cadence units this node emits. FE-C page 19 wants whole
	 * strides/min and BLE RSC wants whole STEPS/min; both conversions are
	 * at the bottom of this header. */
	uint32_t cadence_strides_min_16;

	uint8_t heart_rate_bpm; /* TREADMILL_HR_NONE for no reading */
	bool    hr_from_ant;    /* true once an ANT+ strap is the source */

	uint8_t state; /* TREADMILL_STATE_* */
	bool    lap_toggle;

	/* The user's mass, from FE-C page 55 if a controller ever sends one.
	 * 0.01 kg, and 0 means "not supplied" - the calorie estimate then uses
	 * TREADMILL_DEFAULT_MASS_10G and says so. */
	uint16_t user_mass_10g;
};

/* 75.00 kg. A number has to be picked for the calorie estimate and an
 * unsupplied mass is the common case; picking one and naming it is better than
 * reporting no calories at all, and page 18 is optional anyway. */
#define TREADMILL_DEFAULT_MASS_10G 7500u

void treadmill_state_init(struct treadmill_state *s);

/*
 * Advance the physics by `dt_ms`.
 *
 * Integrates distance, vertical distance and energy from the CURRENT belt
 * speed and incline, and advances the session clock only while the state is
 * IN_USE - which is FE-C's own rule for its elapsed-time accumulator
 * (§8.5.2.2) and is why differencing that field across a pause understates the
 * interval. Strides are NOT integrated here: they arrive as events from the
 * source, because a stride is an edge and integrating a cadence into a count
 * is the same class of mistake apps/hrm_ble/src/hrm_sensor.h rejects for
 * heartbeats.
 */
void treadmill_state_tick(struct treadmill_state *s, uint32_t dt_ms);

/* The belt is doing this now. */
void treadmill_state_set_speed(struct treadmill_state *s, uint32_t mm_s);

/* The deck is at this now. Clamped to the reportable ±100.00 %. */
void treadmill_state_set_incline(struct treadmill_state *s, int32_t centi_pct);

/*
 * A controller asked for this. BOTH control paths land here - FE-C page 51 and
 * the FTMS control point - so a grade set over ANT+ is immediately visible over
 * BLE and the other way round. Returns the value actually adopted after
 * clamping, which is what the command-status echo and the FTMS response code
 * should report.
 */
int32_t treadmill_state_request_incline(struct treadmill_state *s,
					int32_t centi_pct);
uint32_t treadmill_state_request_speed(struct treadmill_state *s,
				       uint32_t mm_s);

/* One stride, i.e. two footfalls. */
void treadmill_state_add_strides(struct treadmill_state *s, uint32_t n);

/* 1/16 strides per minute. */
void treadmill_state_set_cadence_16(struct treadmill_state *s,
				    uint32_t strides_min_16);

void treadmill_state_set_heart_rate(struct treadmill_state *s, uint8_t bpm,
				    bool from_ant);

/* Sets the FE state and flips the lap toggle when a session ends, which is
 * what FE-C §8.5.2.7.1 defines a lap event as - a CHANGE of the bit, not a
 * value of it. */
void treadmill_state_set_state(struct treadmill_state *s, uint8_t fe_state);

/* FE-C page 55's user weight, 0.01 kg. */
void treadmill_state_set_user_mass(struct treadmill_state *s,
				   uint16_t mass_10g);

/* ---------------------------------------------------------------------------
 * The conversions. ONE function per direction, each unit-tested.
 * ---------------------------------------------------------------------------
 *
 * THE ONE THAT WILL BE GOT WRONG IS CADENCE, and it is worth being blunt about
 * it. Three outputs, two units:
 *
 *   FE-C page 19   strides per minute        (one stride = two footfalls)
 *   ANT+ SDM       strides per minute, 1/16  (same quantity, finer)
 *   BLE RSC        STEPS per minute          (twice the number)
 *
 * Nordic's rscs.h comments its cadence field as "1 unit = 1 stride/minute",
 * but its own sample (nrf/samples/bluetooth/peripheral_rscs/src/main.c)
 * simulates 150-180, which is steps: strides would be 75-90. Steps per minute
 * is what real clients expect and what this node emits. If exactly one of the
 * three outputs is ever wrong by a factor of two, it will be this one.
 */

/* Whole strides per minute, for FE-C page 19. Rounds to nearest. */
uint8_t treadmill_cadence_strides_min(uint32_t strides_min_16);

/* Whole STEPS per minute, for BLE RSC. Twice the strides, clamped at 255. */
uint8_t treadmill_rsc_cadence_from_strides(uint32_t strides_min_16);

/*
 * FE-C's 0.01 % to FTMS's 0.1 %, rounding to nearest and away from zero so a
 * commanded -0.05 % does not come back as 0.0 % with the sign lost.
 */
int16_t treadmill_ftms_incline_from_fec(int32_t centi_pct);

/* FTMS's 0.1 % back to FE-C's 0.01 %. Exact - this is the direction that
 * gains resolution rather than losing it, which is why the state struct keeps
 * the finer unit. */
int32_t treadmill_fec_incline_from_ftms(int16_t deci_pct);

/* mm/s to FTMS's 0.01 km/h. 1 mm/s is 0.0036 km/h, so the factor is 36/100
 * and the multiply happens first. */
uint16_t treadmill_ftms_speed_from_mm_s(uint32_t mm_s);

/* FTMS's 0.01 km/h back to mm/s. */
uint32_t treadmill_mm_s_from_ftms_speed(uint16_t centi_kph);

/* mm/s to BLE RSC's 1/256 m/s. */
uint16_t treadmill_rsc_speed_from_mm_s(uint32_t mm_s);

/* Stride length in 0.01 m, from distance and stride count - FE-C page 17's
 * "cycle length" and RSC's "instantaneous stride length" are the same
 * quantity in the same unit, which is the one piece of luck in this file.
 * Returns 0 when there are no strides to divide by. */
uint8_t treadmill_stride_length_cm(uint64_t distance_mm, uint32_t strides);

#ifdef __cplusplus
}
#endif

#endif /* TREADMILL_STATE_H_ */
