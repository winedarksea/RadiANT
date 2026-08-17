/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_lev.h - ANT+ Light Electric Vehicle, device type 0x14.
 *
 * Page codecs for 0x01, 0x02, 0x22, 0x03, 0x04, 0x05, 0x10 and the request
 * page 0x46, the travel-mode mapping of Table 6-1, and a master driving the
 * 4-page rotation. Fields are in wire scale; the caller converts.
 */

#ifndef RADIANT_PROFILE_LEV_H_
#define RADIANT_PROFILE_LEV_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_common.h"
#include "profile_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_LEV_DEVICE_TYPE       0x14u
#define PROFILE_LEV_PERIOD            8192u /* 4 Hz */
#define PROFILE_LEV_RF_FREQ           57u
#define PROFILE_LEV_TRANS_TYPE        0x05u
#define PROFILE_LEV_TRANS_TYPE_SEARCH 0x00u

#define PROFILE_LEV_PAGE_SPEED_SYSTEM_1   0x01u
#define PROFILE_LEV_PAGE_SPEED_DISTANCE   0x02u
#define PROFILE_LEV_PAGE_SPEED_SYSTEM_2   0x03u
#define PROFILE_LEV_PAGE_BATTERY          0x04u
#define PROFILE_LEV_PAGE_CAPABILITIES     0x05u
#define PROFILE_LEV_PAGE_DISPLAY          0x10u
#define PROFILE_LEV_PAGE_SPEED_DISTANCE_2 0x22u /* 34: fuel instead of range */
#define PROFILE_LEV_PAGE_REQUEST          0x46u /* common page 70 */

/* Wire scales. */
#define PROFILE_LEV_SPEED_PER_KMH    10u  /* 0.1 km/h */
#define PROFILE_LEV_ODOMETER_PER_KM  100u /* 0.01 km */
#define PROFILE_LEV_FUEL_PER_WH_KM   10u  /* 0.1 Wh/km */
#define PROFILE_LEV_VOLTAGE_PER_V    4u   /* 0.25 V */
#define PROFILE_LEV_CHARGE_DIST_PER_KM 10u /* 0.1 km */

/* Field maxima. */
#define PROFILE_LEV_SPEED_MAX       4095u
#define PROFILE_LEV_ODOMETER_MAX    0xFFFFFFu
#define PROFILE_LEV_RANGE_MAX       4095u
#define PROFILE_LEV_FUEL_MAX        4095u
#define PROFILE_LEV_CYCLES_MAX      4095u
#define PROFILE_LEV_WHEEL_MAX       4095u
#define PROFILE_LEV_PERCENT_MAX     100u

/* Sentinels. Zero means "unknown" for every optional numeric field; % assist
 * and the display page's travel mode are the two that use 0xFF instead. */
#define PROFILE_LEV_TEMPERATURE_UNKNOWN 0x00u
#define PROFILE_LEV_RANGE_UNKNOWN       0u
#define PROFILE_LEV_FUEL_UNKNOWN        0u
#define PROFILE_LEV_CYCLES_UNKNOWN      0u
#define PROFILE_LEV_VOLTAGE_UNKNOWN     0u
#define PROFILE_LEV_CHARGE_DIST_UNKNOWN 0u
#define PROFILE_LEV_WHEEL_UNKNOWN       0u
#define PROFILE_LEV_ASSIST_PCT_UNKNOWN  0xFFu
#define PROFILE_LEV_TRAVEL_MODE_UNSET   0xFFu

/* Error codes, byte [5] of page 1. 5..15 reserved, 16..255 manufacturer. */
#define PROFILE_LEV_ERROR_NONE          0u
#define PROFILE_LEV_ERROR_BATTERY       1u
#define PROFILE_LEV_ERROR_DRIVE_TRAIN   2u
#define PROFILE_LEV_ERROR_BATTERY_EOL   3u
#define PROFILE_LEV_ERROR_OVERHEATING   4u
#define PROFILE_LEV_ERROR_MFG_FIRST     16u

/* Temperature levels, Table 5-3. */
#define PROFILE_LEV_TEMP_UNKNOWN   0u
#define PROFILE_LEV_TEMP_COLD      1u
#define PROFILE_LEV_TEMP_COLD_WARM 2u
#define PROFILE_LEV_TEMP_WARM      3u
#define PROFILE_LEV_TEMP_WARM_HOT  4u
#define PROFILE_LEV_TEMP_HOT       5u

/* Travel-mode and capability levels are 0..7 in both halves. */
#define PROFILE_LEV_LEVEL_MAX 7u

/* Gear state, Table 5-6. */
#define PROFILE_LEV_GEAR_NONE     0u
#define PROFILE_LEV_REAR_GEAR_MAX 15u
#define PROFILE_LEV_FRONT_GEAR_MAX 3u

/* Page 3 byte [1] bit 7. */
#define PROFILE_LEV_SOC_EMPTY_WARNING 0x80u

/* Display command bits, page 16 bytes [4..5] as a little-endian u16. */
#define PROFILE_LEV_CMD_TURN_RIGHT 0x0001u
#define PROFILE_LEV_CMD_TURN_LEFT  0x0002u
#define PROFILE_LEV_CMD_HIGH_BEAM  0x0004u
#define PROFILE_LEV_CMD_LIGHT_ON   0x0008u

/* System state bits, byte [3] of pages 1 and 3. */
#define PROFILE_LEV_SYS_TURN_RIGHT 0x01u
#define PROFILE_LEV_SYS_TURN_LEFT  0x02u
#define PROFILE_LEV_SYS_HIGH_BEAM  0x04u
#define PROFILE_LEV_SYS_LIGHT_ON   0x08u
#define PROFILE_LEV_SYS_THROTTLE   0x10u

/* Request page byte [5]: bits 6..0 count, bit 7 acknowledged if possible. */
#define PROFILE_LEV_REQUEST_ACK_BIT    0x80u
#define PROFILE_LEV_REQUEST_COUNT_MAX  4u
#define PROFILE_LEV_REQUEST_CMD_PAGE   0x01u
#define PROFILE_LEV_REQUEST_CMD_ANTFS  0x02u

/* Cadence. A common page every 20th channel period; page 2 at least once
 * every 120 messages when page 34 is used in its place. */
#define PROFILE_LEV_COMMON_EVERY   20u
#define PROFILE_LEV_PAGE_2_EVERY   120u
#define PROFILE_LEV_ROTATION_SLOTS 4u

/* ── Bit-field pack/unpack ──────────────────────────────────────────────── */

uint8_t profile_lev_pack_temperature(bool motor_alert, uint8_t motor_level,
				     bool battery_alert, uint8_t battery_level);
void profile_lev_unpack_temperature(uint8_t raw, bool *motor_alert,
				    uint8_t *motor_level, bool *battery_alert,
				    uint8_t *battery_level);

uint8_t profile_lev_pack_travel_mode(uint8_t assist, uint8_t regen);
void profile_lev_unpack_travel_mode(uint8_t raw, uint8_t *assist,
				    uint8_t *regen);

uint8_t profile_lev_pack_gear(bool gears_exist, bool manual, uint8_t rear,
			      uint8_t front);
void profile_lev_unpack_gear(uint8_t raw, bool *gears_exist, bool *manual,
			     uint8_t *rear, uint8_t *front);

/* Page 5 byte [2] and page 16's gear/light command field. */
uint8_t profile_lev_pack_modes_supported(uint8_t assist, uint8_t regen);
void profile_lev_unpack_modes_supported(uint8_t raw, uint8_t *assist,
					uint8_t *regen);

uint16_t profile_lev_pack_command(uint8_t rear, uint8_t front, uint16_t flags);
void profile_lev_unpack_command(uint16_t raw, uint8_t *rear, uint8_t *front,
				uint16_t *flags);

/* ── Travel-mode mapping, Table 6-1 ─────────────────────────────────────── */

/* The mode number a device supporting `n_supported` modes should send for its
 * `index`-th setting (0-based, least first). */
int profile_lev_mode_setting(uint8_t n_supported, uint8_t index, uint8_t *mode);

/* The inverse: which of `n_supported` settings a received mode 1..7 falls in. */
int profile_lev_mode_group(uint8_t n_supported, uint8_t mode, uint8_t *index);

/* ── Pages ──────────────────────────────────────────────────────────────── */

struct profile_lev_page1 {
	uint8_t  temperature; /* packed, 0 = unknown */
	uint8_t  travel_mode; /* packed */
	uint8_t  system;      /* PROFILE_LEV_SYS_* */
	uint8_t  gear;        /* packed */
	uint8_t  error;
	uint16_t speed;       /* 0.1 km/h, 12 bits */
};

struct profile_lev_page2 {
	uint32_t odometer;    /* 0.01 km, u24 accumulator */
	uint16_t range_km;    /* 12 bits; 0 = unknown */
	uint16_t speed;
};

/* Page 34: page 2 with fuel consumption where the range is. */
struct profile_lev_page34 {
	uint32_t odometer;
	uint16_t fuel;        /* 0.1 Wh/km, 12 bits; 0 = unknown */
	uint16_t speed;
};

struct profile_lev_page3 {
	uint8_t  soc_pct;       /* 0..100 */
	bool     battery_empty; /* the byte's bit 7 */
	uint8_t  travel_mode;
	uint8_t  system;
	uint8_t  gear;
	uint8_t  assist_pct;    /* 0..100, or 0xFF */
	uint16_t speed;
};

struct profile_lev_page4 {
	uint16_t charge_cycles;   /* 12 bits; 0 = unknown */
	uint16_t fuel;            /* 0.1 Wh/km, 12 bits; 0 = unknown */
	uint8_t  voltage_quarter; /* 0.25 V; 0 = unknown */
	uint16_t charge_distance; /* 0.1 km; 0 = unknown */
};

struct profile_lev_page5 {
	uint8_t  modes_supported; /* packed */
	uint16_t wheel_mm;        /* 12 bits; 0 = unknown */
};

struct profile_lev_display {
	uint16_t wheel_mm;        /* 12 bits; 0xFFF = not set */
	uint8_t  travel_mode;     /* packed, or 0xFF */
	uint16_t command;         /* packed */
	uint16_t manufacturer_id; /* the display's */
};

struct profile_lev_request {
	uint8_t transmit_response; /* count in bits 6..0, ack in bit 7 */
	uint8_t page;
	uint8_t command_type;
};

/* Each encoder writes eight bytes and returns 0, or -EINVAL for a null
 * argument or a field wider than its wire field. Each decoder returns -EINVAL
 * if byte [0] is not its own page number. */
int profile_lev_encode_page1(const struct profile_lev_page1 *p, uint8_t *out);
int profile_lev_decode_page1(const uint8_t *body, struct profile_lev_page1 *p);

int profile_lev_encode_page2(const struct profile_lev_page2 *p, uint8_t *out);
int profile_lev_decode_page2(const uint8_t *body, struct profile_lev_page2 *p);

int profile_lev_encode_page34(const struct profile_lev_page34 *p, uint8_t *out);
int profile_lev_decode_page34(const uint8_t *body, struct profile_lev_page34 *p);

int profile_lev_encode_page3(const struct profile_lev_page3 *p, uint8_t *out);
int profile_lev_decode_page3(const uint8_t *body, struct profile_lev_page3 *p);

int profile_lev_encode_page4(const struct profile_lev_page4 *p, uint8_t *out);
int profile_lev_decode_page4(const uint8_t *body, struct profile_lev_page4 *p);

int profile_lev_encode_page5(const struct profile_lev_page5 *p, uint8_t *out);
int profile_lev_decode_page5(const uint8_t *body, struct profile_lev_page5 *p);

int profile_lev_encode_display(const struct profile_lev_display *d,
			       uint8_t *out);
int profile_lev_decode_display(const uint8_t *body,
			       struct profile_lev_display *d);

int profile_lev_encode_request(const struct profile_lev_request *r,
			       uint8_t *out);
int profile_lev_decode_request(const uint8_t *body,
			       struct profile_lev_request *r);

/* ── The master ─────────────────────────────────────────────────────────── */

struct profile_lev_cfg {
	struct profile_common_id id;

	/* Send page 34 in the second rotation slot, dropping to page 2 once
	 * every PROFILE_LEV_PAGE_2_EVERY messages. */
	bool use_page_34;

	/* Include page 4 in the fourth slot, alternating with page 5. */
	bool send_page_4;
};

struct profile_lev {
	struct profile_lev_cfg cfg;
	struct profile_sched   sched;

	struct profile_lev_page1  page1;
	struct profile_lev_page2  page2;
	struct profile_lev_page34 page34;
	struct profile_lev_page3  page3;
	struct profile_lev_page4  page4;
	struct profile_lev_page5  page5;

	uint32_t messages;
	uint8_t  slot;          /* 0..3 within the rotation */
	uint16_t since_common;
	uint16_t since_page_2;
	bool     common_81_next;
	bool     page_5_next;

	uint8_t request_page;
	uint8_t request_left;

	uint8_t rotation[1];
};

int profile_lev_init(struct profile_lev *lev,
		     const struct profile_lev_cfg *cfg);

void profile_lev_set_page1(struct profile_lev *lev,
			   const struct profile_lev_page1 *p);
void profile_lev_set_page2(struct profile_lev *lev,
			   const struct profile_lev_page2 *p);
void profile_lev_set_page34(struct profile_lev *lev,
			    const struct profile_lev_page34 *p);
void profile_lev_set_page3(struct profile_lev *lev,
			   const struct profile_lev_page3 *p);
void profile_lev_set_page4(struct profile_lev *lev,
			   const struct profile_lev_page4 *p);
void profile_lev_set_page5(struct profile_lev *lev,
			   const struct profile_lev_page5 *p);

/* Arm an on-request reply: the page goes out on the next slot, `count` times
 * (clamped to PROFILE_LEV_REQUEST_COUNT_MAX), and the rotation then restarts
 * at page 1. -ENOTSUP for a page this master does not send. */
int profile_lev_request(struct profile_lev *lev, uint8_t page, uint8_t count);

/* The same, from a received common page 70 body. */
int profile_lev_apply_request(struct profile_lev *lev, const uint8_t *body);

/* Fill `body` with the next message and say what it was. */
enum profile_slot_kind profile_lev_next(struct profile_lev *lev, uint8_t *body);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_LEV_H_ */
