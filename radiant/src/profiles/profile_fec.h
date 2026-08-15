/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_fec.h - ANT+ Fitness Equipment Control (FE-C), device type 0x11.
 *
 * Provenance: D000001231, "ANT+ Device Profile - Fitness Equipment", Rev 5.0
 * (the deprecated-but-primary document, read from the PDF, not from a mirror).
 * Every layout below cites the section and table it came from. Nothing here
 * derives from sdk-ant or from libant.a, and nothing here consults
 * tools/ant_pages.py - the Python side has no FE-C pages at all, so this file
 * is currently the only copy and the three-copy vocabulary rule
 * (radiant_bridge.h) does not yet apply to it.
 *
 * ---------------------------------------------------------------------------
 * What this file IS
 * ---------------------------------------------------------------------------
 * Two decoders, for the two pages a smart trainer sends that carry data this
 * project can use: page 16 (0x10) General FE Data and page 25 (0x19) Specific
 * Trainer Data. Pure page codecs - no state, no radiant header, no bridge
 * header, out-params only, 0 or -EINVAL.
 *
 * ---------------------------------------------------------------------------
 * What this file is NOT
 * ---------------------------------------------------------------------------
 *   - NOT AN ENCODER. Unlike profile_hr.c / profile_power.c / profile_rd.c,
 *     this project does not emulate fitness equipment. There is no
 *     struct profile_fec, no profile_sched hookup and no common-page rotation,
 *     because nothing in this repository transmits device type 0x11. If that
 *     changes, the encoders belong here beside the decoders and the master
 *     belongs in a struct, exactly the way profile_power.c is arranged.
 *   - NOT A CONTROLLER. The whole "C" of FE-C - control pages 48-51, the
 *     command-status page 71, calibration pages 0x01/0x02, and the capabilities
 *     page 54 (spec sections 6.2, 6.4, 6.5, 8.8) - is out of scope. This
 *     project is a receiver; it reads a trainer's workout data and never tells
 *     the trainer what resistance to apply.
 *   - NOT THE TORQUE PATH. Page 26 (0x1A) Specific Trainer Torque Data
 *     (section 8.6.8) carries wheel ticks, wheel period and accumulated torque,
 *     from which power must be *computed* (section 8.6.8.6). Page 25 gives
 *     power directly, every trainer that sends 0x1A also sends 0x19, and a
 *     second, differently-scaled route to the same watts is a second thing to
 *     get wrong. Deliberately absent.
 *   - NOT THE OTHER EQUIPMENT TYPES. Pages 19-24 (treadmill, elliptical,
 *     rower, climber, Nordic skier; sections 8.6.1-8.6.6) are not decoded.
 *     Page 16 IS decoded for all of them, because section 8.5.2 requires every
 *     FE type to send it, so a treadmill still yields elapsed time, distance,
 *     speed, heart rate and equipment state through this file.
 *
 * ---------------------------------------------------------------------------
 * Four traps in these two pages
 * ---------------------------------------------------------------------------
 *   THE FE STATE FIELD IS A NIBBLE INSIDE A NIBBLE. Table 8-7 and Table 8-25
 *   both give byte 7 as "FE State Bit Field, 4 Bits (4:7)". Table 8-10 then
 *   numbers the bits WITHIN that 4-bit field: bits 0-2 are the state, bit 3 is
 *   the lap toggle. So the state is (byte7 >> 4) & 0x07 and the lap toggle is
 *   bit 7 of byte 7 - NOT (byte7 >> 4) & 0x0F, which folds the lap toggle into
 *   the state and turns IN_USE (3) into 11 every other lap.
 *
 *   0xFFF INSTANTANEOUS POWER INVALIDATES ACCUMULATED POWER TOO. Table 8-25,
 *   bytes 5 and 6 bits 0-3: "0xFFF indicates BOTH the instantaneous and
 *   accumulated power fields are invalid". This is the only sentinel in this
 *   repository where one field's invalid value condemns a different field, and
 *   a receiver that honoured only the instantaneous half would keep
 *   integrating a frozen accumulator.
 *
 *   DISTANCE HAS NO INVALID VALUE - IT HAS AN ENABLE BIT. Section 8.5.2.3
 *   states this outright: every one of byte 3's 256 values is a legal
 *   distance, and whether byte 3 means anything at all is bit 2 of the
 *   capabilities nibble (Table 8-9). The general rule the spec gives in
 *   section 8.5.2.6.1 is that ACCUMULATING optional fields are gated by the
 *   capabilities bits and INSTANTANEOUS ones (speed, heart rate) carry an
 *   all-ones sentinel. Reaching for a sentinel on distance reads 255 m as
 *   "absent".
 *
 *   SPEED MAY BE A LIE, ON PURPOSE. Capabilities bit 3 (section 8.5.2.6.2,
 *   section 6.4.2) says bytes 4-5 are VIRTUAL speed - what the rider would
 *   have been doing had the trainer been able to apply negative resistance.
 *   It is not an error and it is not invalid; it is simply not a measurement
 *   of anything physical. Reported as a flag so a consumer can decide.
 *
 * ---------------------------------------------------------------------------
 * Elapsed time is not a duration, quite
 * ---------------------------------------------------------------------------
 * Section 8.5.2.2: the elapsed-time accumulator "shall only increment when the
 * fitness equipment is in the IN_USE state". It is session time, not wall
 * time, so differencing it across a pause understates the interval by however
 * long the pause was. That is the correct behaviour for a workout timer and
 * the wrong basis for integrating power over time - which is why
 * radiant_power_adapter.c integrates against the bridge's own t_us and not
 * against this field.
 */

#ifndef RADIANT_PROFILE_FEC_H_
#define RADIANT_PROFILE_FEC_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_FEC_DEVICE_TYPE 0x11u /* sections 7.1.2 / 7.2.2: 17 decimal */

/* Channel period in counts of 1/32768 s: 8192, exactly 4 Hz (sections 7.1/7.2,
 * [SD_0003] and [MD_0003]). The one rate this profile defines. */
#define PROFILE_FEC_PERIOD 8192u

#define PROFILE_FEC_PAGE_GENERAL 0x10u /* section 8.5.2, page 16 */
#define PROFILE_FEC_PAGE_TRAINER 0x19u /* section 8.6.7, page 25 */

/* Table 8-10, bits 0-2 of the FE state nibble. 0 and 5-7 are reserved; a
 * decoder must pass them through rather than remap them, because "reserved"
 * here means a future state, not an error. */
#define PROFILE_FEC_STATE_RESERVED 0u
#define PROFILE_FEC_STATE_ASLEEP   1u
#define PROFILE_FEC_STATE_READY    2u
#define PROFILE_FEC_STATE_IN_USE   3u
#define PROFILE_FEC_STATE_FINISHED 4u

/* Table 8-8, byte 1 bits 0-4 of page 16. Bits 5-7 are reserved and are masked
 * off ("Do Not Interpret"). */
#define PROFILE_FEC_TYPE_MASK         0x1Fu
#define PROFILE_FEC_TYPE_TREADMILL    19u
#define PROFILE_FEC_TYPE_ELLIPTICAL   20u
#define PROFILE_FEC_TYPE_ROWER        22u
#define PROFILE_FEC_TYPE_CLIMBER      23u
#define PROFILE_FEC_TYPE_NORDIC_SKIER 24u
#define PROFILE_FEC_TYPE_TRAINER      25u

/* Table 8-9, page 16 capabilities nibble bits 0-1: where byte 6's heart rate
 * came from. 0 is the DEFAULT and means unknown/other, i.e. it does not by
 * itself invalidate byte 6 - only 0xFF does that. */
#define PROFILE_FEC_HR_SRC_INVALID 0u
#define PROFILE_FEC_HR_SRC_ANTPLUS 1u
#define PROFILE_FEC_HR_SRC_EM_5KHZ 2u
#define PROFILE_FEC_HR_SRC_CONTACT 3u

/* The instantaneous sentinels. Table 8-7 bytes 4-5 and byte 6; Table 8-25
 * byte 2 and the 12-bit power field. */
#define PROFILE_FEC_INVALID_SPEED      0xFFFFu
#define PROFILE_FEC_INVALID_HR         0xFFu
#define PROFILE_FEC_INVALID_CADENCE    0xFFu
#define PROFILE_FEC_INVALID_INST_POWER 0x0FFFu

/* Table 8-27, byte 6's high nibble on page 25. */
#define PROFILE_FEC_TRAINER_STATUS_POWER_CAL      (1u << 0)
#define PROFILE_FEC_TRAINER_STATUS_RESISTANCE_CAL (1u << 1)
#define PROFILE_FEC_TRAINER_STATUS_USER_CFG       (1u << 2)

/* Table 8-28, byte 7's low nibble on page 25, bits 0-1. */
#define PROFILE_FEC_TARGET_POWER_OK           0u
#define PROFILE_FEC_TARGET_POWER_SPEED_LOW    1u
#define PROFILE_FEC_TARGET_POWER_SPEED_HIGH   2u
#define PROFILE_FEC_TARGET_POWER_UNDETERMINED 3u

/*
 * ---------------------------------------------------------------------------
 * Page 16 (0x10) General FE Data - Table 8-7
 * ---------------------------------------------------------------------------
 *   [0]    page number, 0x10
 *   [1]    equipment type bit field (Table 8-8: bits 0-4 type, 5-7 reserved)
 *   [2]    elapsed time, 0.25 s resolution, u8 accumulator, rolls at 64 s
 *   [3]    distance traveled, 1 m, u8 accumulator, rolls at 256 m,
 *          NO invalid value - gated by capabilities bit 2
 *   [4..5] instantaneous speed, u16 LE, 0.001 m/s, 0xFFFF invalid
 *   [6]    instantaneous heart rate, bpm, 0xFF invalid
 *   [7]    bits 0-3 capabilities (Table 8-9), bits 4-7 FE state (Table 8-10)
 */
struct profile_fec_general {
	uint8_t equipment_type; /* already masked to bits 0-4 */

	uint8_t  elapsed_time_qs; /* quarter-seconds; wraps, and is meant to */
	uint8_t  distance_m;      /* metres; wraps, and is meant to */
	bool     distance_valid;  /* capabilities bit 2, NOT a sentinel test */

	uint16_t speed_mm_s;   /* 0.001 m/s */
	bool     speed_valid;  /* bytes 4-5 != 0xFFFF */
	bool     speed_virtual; /* capabilities bit 3 - real value, unreal quantity */

	uint8_t  heart_rate_bpm;
	bool     heart_rate_valid; /* byte 6 != 0xFF */
	uint8_t  hr_source;        /* PROFILE_FEC_HR_SRC_* */

	uint8_t  state;      /* PROFILE_FEC_STATE_* */
	bool     lap_toggle; /* section 8.5.2.7.1: a CHANGE is the lap event */
};

/*
 * ---------------------------------------------------------------------------
 * Page 25 (0x19) Specific Trainer/Stationary Bike Data - Table 8-25
 * ---------------------------------------------------------------------------
 *   [0]    page number, 0x19
 *   [1]    update event count, u8 accumulator, rolls at 256
 *   [2]    instantaneous cadence, rpm, 0xFF invalid
 *   [3..4] accumulated power, u16 LE, 1 W, rolls at 65536 W
 *   [5] + [6] bits 0-3   instantaneous power, 12 bits LE, 1 W,
 *                        0xFFF invalidates BOTH power fields
 *   [6] bits 4-7         trainer status bit field (Table 8-27)
 *   [7] bits 0-3         flags bit field (Table 8-28)
 *   [7] bits 4-7         FE state bit field (Table 8-10)
 *
 * Note the accumulated-power field starts at byte 3, one earlier than
 * Bicycle Power page 0x10's, which puts it at bytes 4-5. Two profiles, two
 * offsets, same quantity, same units.
 */
struct profile_fec_trainer {
	uint8_t event_count;

	uint8_t cadence_rpm;
	bool    cadence_valid;

	/* Section 8.6.7.3: "the running sum of the instantaneous power data",
	 * incremented once per event count. WATT-SAMPLES, not joules. See
	 * radiant_power_adapter.h. */
	uint16_t acc_power_w;
	uint16_t inst_power_w; /* 12-bit, 0-4094 W */
	bool     power_valid;  /* false zeroes the MEANING of both fields above */

	uint8_t trainer_status; /* PROFILE_FEC_TRAINER_STATUS_* bits */
	uint8_t flags;          /* Table 8-28, whole nibble */

	uint8_t state;      /* PROFILE_FEC_STATE_* */
	bool    lap_toggle;
};

/*
 * Decodes an 8-byte page 0x10 body into *out. Returns 0, or -EINVAL for a NULL
 * argument or a byte [0] that is not PROFILE_FEC_PAGE_GENERAL.
 *
 * *out is fully written on success. Fields the equipment marked absent still
 * carry their raw wire value; the accompanying `_valid` flag is the only thing
 * a caller may branch on.
 */
int profile_fec_decode_general(const uint8_t *body,
			       struct profile_fec_general *out);

/*
 * Decodes an 8-byte page 0x19 body into *out. Returns 0, or -EINVAL for a NULL
 * argument or a byte [0] that is not PROFILE_FEC_PAGE_TRAINER.
 *
 * On the 0xFFF instantaneous-power sentinel, power_valid is false and BOTH
 * acc_power_w and inst_power_w must be ignored (Table 8-25).
 */
int profile_fec_decode_trainer(const uint8_t *body,
			       struct profile_fec_trainer *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_FEC_H_ */
