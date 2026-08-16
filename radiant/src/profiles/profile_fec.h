/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_fec.h - ANT+ Fitness Equipment Control (FE-C), device type 0x11.
 *
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
