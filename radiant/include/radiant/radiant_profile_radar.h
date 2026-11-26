/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_profile_radar.h - ANT+ Bike Radar, device type 0x28.
 *
 * Provenance: ANT+ Device Profile - Bike Radar Rev 2.1 (D00001665,
 * deprecated "as is"), cross-checked on air against three Garmin units
 * (Varia RTL510, RTL515, RearVue 820) recorded with tools/ant_record.py;
 * captures under archive/captures/radio/2026-11-25-varia-*. Where the spec
 * and the units differ, the decoders accept both and the master emits what
 * the units emit (docs/device-profiles.md section 3.18 lists each case).
 * No sdk-ant source was consulted. See docs/decisions/0002-clean-room-policy.md.
 *
 * SAFETY: this is telemetry, not a certified radar display. Nothing here is
 * fit to warn a rider of traffic.
 *
 * ---------------------------------------------------------------------------
 * Target pages carry four targets each, packed across byte boundaries
 * ---------------------------------------------------------------------------
 * Page 48 (targets 1-4) and page 49 (targets 5-8) share one layout:
 *   byte 1: threat level, 2 bits per target   byte 2: side, 2 bits each
 *   bytes 3-5: range, 6 bits per target (24-bit little-endian field)
 *   byte 6/7: closing speed, 4 bits per target (low nibble first)
 * Range is in 3.125 m steps (0-196.875 m), closing speed in 3.04 m/s steps
 * (0-45.6 m/s). The structs below keep the WIRE CODES, so a decode re-encodes
 * byte for byte; the _MM/_MM_S macros give the physical value.
 *
 * A target with threat level 0 is "no target": the spec forbids a display
 * from decoding its side, range or speed. The decoder still fills them (for
 * byte-exact round trips) and clears `valid`; consumers read `valid`.
 * Targets are NOT guaranteed sorted (Rev 2.1), whatever the radar does.
 */

#ifndef RADIANT_PROFILE_RADAR_H_
#define RADIANT_PROFILE_RADAR_H_

#include <stdbool.h>
#include <stdint.h>

#include <radiant/radiant_profile_common.h>

#ifdef __cplusplus
extern "C" {
#endif

#define RADIANT_PROFILE_RADAR_DEVICE_TYPE 0x28u
/* 4084/32768 s, about 8.02 Hz. Measured 4084.0 on all three units. */
#define RADIANT_PROFILE_RADAR_PERIOD      4084u
#define RADIANT_PROFILE_RADAR_RF_FREQ     57u
/* Low nibble 5; the high nibble may carry device-number bits 16-19 (every
 * Garmin unit measured does: it is the serial number's bits 16-19). */
#define RADIANT_PROFILE_RADAR_TRANS_TYPE  0x05u

#define RADIANT_PROFILE_RADAR_PAGE_STATUS    0x01u
#define RADIANT_PROFILE_RADAR_PAGE_COMMAND   0x02u
#define RADIANT_PROFILE_RADAR_PAGE_TARGETS_A 0x30u /* 48: targets 1-4 */
#define RADIANT_PROFILE_RADAR_PAGE_TARGETS_B 0x31u /* 49: targets 5-8 */
#define RADIANT_PROFILE_RADAR_PAGE_ERROR     0x57u /* common page 87 */
#define RADIANT_PROFILE_RADAR_PAGE_REQUEST   0x46u /* common page 70 */

#define RADIANT_PROFILE_RADAR_TARGETS_PER_PAGE 4u
#define RADIANT_PROFILE_RADAR_MAX_TARGETS      8u

#define RADIANT_PROFILE_RADAR_RANGE_STEP_MM   3125u /* 3.125 m */
#define RADIANT_PROFILE_RADAR_SPEED_STEP_MM_S 3040u /* 3.04 m/s */
#define RADIANT_PROFILE_RADAR_RANGE_CODE_MAX  63u
#define RADIANT_PROFILE_RADAR_SPEED_CODE_MAX  15u
#define RADIANT_PROFILE_RADAR_RANGE_MM(code) \
	((uint32_t)(code) * RADIANT_PROFILE_RADAR_RANGE_STEP_MM)
#define RADIANT_PROFILE_RADAR_SPEED_MM_S(code) \
	((uint32_t)(code) * RADIANT_PROFILE_RADAR_SPEED_STEP_MM_S)

/* Table 6-4. */
#define RADIANT_PROFILE_RADAR_THREAT_NONE     0u
#define RADIANT_PROFILE_RADAR_THREAT_APPROACH 1u
#define RADIANT_PROFILE_RADAR_THREAT_FAST     2u
/* Table 6-5. */
#define RADIANT_PROFILE_RADAR_SIDE_BEHIND 0u
#define RADIANT_PROFILE_RADAR_SIDE_RIGHT  1u
#define RADIANT_PROFILE_RADAR_SIDE_LEFT   2u

/* Table 6-8, page 1 byte 1 bits 0-1. */
#define RADIANT_PROFILE_RADAR_STATE_BROADCASTING       0u
#define RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_REQUESTED 1u
#define RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_ABORTED   2u
#define RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_FORCED    3u

/* Table 6-10, page 2 byte 1 bits 0-1. 2 and 3 are reserved. */
#define RADIANT_PROFILE_RADAR_CMD_ABORT_SHUTDOWN 0u
#define RADIANT_PROFILE_RADAR_CMD_SHUTDOWN       1u

/* Table 6-12 error levels; table 6-13 profile error codes. */
#define RADIANT_PROFILE_RADAR_ERR_LEVEL_WARNING  1u
#define RADIANT_PROFILE_RADAR_ERR_LEVEL_CRITICAL 2u
#define RADIANT_PROFILE_RADAR_ERR_SATURATED      0u
#define RADIANT_PROFILE_RADAR_ERR_UNIT_SKEW      1u
#define RADIANT_PROFILE_RADAR_ERR_COMPONENT_SYSTEM 0x0Fu

struct radiant_profile_radar_target {
	bool    valid;      /* threat != 0; nothing else is meaningful if false */
	uint8_t threat;     /* RADIANT_PROFILE_RADAR_THREAT_*, 2 bits */
	uint8_t side;       /* RADIANT_PROFILE_RADAR_SIDE_*, 2 bits */
	uint8_t range_code; /* 0-63, x 3.125 m */
	uint8_t speed_code; /* 0-15, x 3.04 m/s */
};

struct radiant_profile_radar_status {
	uint8_t state;          /* RADIANT_PROFILE_RADAR_STATE_* */
	/* The INVERTED clear-targets bit, decoded: true when the wire bit is
	 * 0, i.e. the radar asks the display to clear its targets. */
	bool    clear_targets;
	/* Byte 1 bits 2-7 were 0x00 rather than the spec's 0x3F - the form
	 * the spec calls "legacy". Reported, never refused. */
	bool    legacy_reserved;
};

struct radiant_profile_radar_error {
	uint8_t  component;    /* 4 bits; 0xF = system / not one component */
	uint8_t  level;        /* RADIANT_PROFILE_RADAR_ERR_LEVEL_*, 2 bits */
	uint8_t  profile_code; /* table 6-13; 0xFF invalid */
	uint32_t mfg_code;     /* 0xFFFFFFFF invalid */
};

/* ---------------------------------------------------------------------------
 * Codecs. Encoders write eight bytes; decoders read eight. All return 0, or
 * -EINVAL for a NULL argument or a body whose page number is not theirs.
 * ---------------------------------------------------------------------------
 */

/* Pages 48 and 49. `page` picks which; `t` is that page's four targets. A
 * target with threat 0 is encoded with every field 0, as the spec requires,
 * whatever else the struct holds. */
int radiant_profile_radar_encode_targets(uint8_t page,
					 const struct radiant_profile_radar_target t[4],
					 uint8_t *out);
/* Accepts page 48 or 49 and returns which in *page (may be NULL). */
int radiant_profile_radar_decode_targets(const uint8_t *body, uint8_t *page,
					 struct radiant_profile_radar_target out[4]);

int radiant_profile_radar_encode_status(const struct radiant_profile_radar_status *s,
					uint8_t *out);
int radiant_profile_radar_decode_status(const uint8_t *body,
					struct radiant_profile_radar_status *s);

/* Page 2. Reserved bits of byte 1 follow `reserved_bits` (the spec prints
 * "set to 0x1F" for a 6-bit field; RADIANT_PROFILE_RADAR_CMD_RESERVED is
 * the value this tree sends, see docs). */
#define RADIANT_PROFILE_RADAR_CMD_RESERVED 0x3Fu
int radiant_profile_radar_encode_command(uint8_t command, uint8_t *out);
/* *command = bits 0-1 of byte 1; reserved bits ignored. */
int radiant_profile_radar_decode_command(const uint8_t *body, uint8_t *command);

int radiant_profile_radar_encode_error(const struct radiant_profile_radar_error *e,
				       uint8_t *out);
int radiant_profile_radar_decode_error(const uint8_t *body,
				       struct radiant_profile_radar_error *e);

/* Common page 70 from a display: *page = requested page, *count = times to
 * send (bits 0-6; 0 is read as 1), *acked = bit 7. -EINVAL if not page 70
 * or command type != 1. */
int radiant_profile_radar_decode_request(const uint8_t *body, uint8_t *page,
					 uint8_t *count, bool *acked);
int radiant_profile_radar_encode_request(uint8_t page, uint8_t count, uint8_t *out);

/* Range/speed in physical units to wire codes, rounded and clamped. */
uint8_t radiant_profile_radar_range_code(uint32_t range_mm);
uint8_t radiant_profile_radar_speed_code(uint32_t speed_mm_s);

/* ---------------------------------------------------------------------------
 * The master (a radar)
 * ---------------------------------------------------------------------------
 */

enum radiant_profile_radar_slot {
	RADIANT_PROFILE_RADAR_SLOT_TARGETS_A = 1,
	RADIANT_PROFILE_RADAR_SLOT_TARGETS_B,
	RADIANT_PROFILE_RADAR_SLOT_STATUS,
	RADIANT_PROFILE_RADAR_SLOT_ERROR,
	RADIANT_PROFILE_RADAR_SLOT_COMMON_80,
	RADIANT_PROFILE_RADAR_SLOT_COMMON_81,
	RADIANT_PROFILE_RADAR_SLOT_COMMON_82,
	/* The radar has finished its shutdown pattern: power off now. The
	 * body is not filled. Returned again on every later call. */
	RADIANT_PROFILE_RADAR_SLOT_OFF,
};

struct radiant_profile_radar_cfg {
	struct radiant_profile_common_id      id;
	struct radiant_profile_common_battery battery;
	/* One background page (80, 81, 82 in turn) every N messages while
	 * reporting <= 4 targets (64 page-48s, then one). 0 = the default, 65 - the
	 * cadence measured on
	 * all three Garmin units (spec: recommended 65, required <= 260). */
	uint16_t background_every;
	/* Messages of the shutdown pattern before power-off (spec >= 2.5 s =
	 * 21 messages) and of the shutdown-aborted pattern (spec >= 10 s = 81
	 * messages). 0 = the defaults below. */
	uint16_t shutdown_msgs;
	uint16_t aborted_msgs;
	/* Status page every Nth message in a shutdown/aborted pattern (spec:
	 * 1/8 to 4/8, so 2..8). 0 = the default, 2: every Garmin unit
	 * alternates page 1 with the main page (C5 captures). */
	uint8_t  status_every;
	/* What a Shutdown COMMAND does. false (the default) = the Rev 2.1
	 * "stay on" option: after `stay_on_delay_msgs` the aborted pattern,
	 * then normal operation - what all three Garmin units do (C5). true =
	 * Shutdown Requested, then power off (the spec's other option). */
	bool     power_off_on_command;
	/* Messages between a Shutdown command and the aborted pattern when
	 * staying on. 0 = the default, 12 (~1.5 s, measured on all three
	 * units: 1.52 s). */
	uint8_t  stay_on_delay_msgs;
	/* Send page 1's reserved bits as 0x00 (the legacy form, as the RTL510
	 * does) instead of 0x3F (RTL515, RearVue 820, spec). */
	bool     legacy_status;
};

#define RADIANT_PROFILE_RADAR_DEFAULT_BACKGROUND_EVERY 65u
#define RADIANT_PROFILE_RADAR_DEFAULT_SHUTDOWN_MSGS    24u /* ~3.0 s */
#define RADIANT_PROFILE_RADAR_DEFAULT_ABORTED_MSGS     81u /* 10.1 s; units: 10.0 s */
#define RADIANT_PROFILE_RADAR_DEFAULT_STATUS_EVERY     2u
#define RADIANT_PROFILE_RADAR_DEFAULT_STAY_ON_DELAY    12u
/* Page 70 with count 0 (the Common Pages "invalid" value): copies sent. The
 * spec gives no answer; all three Garmin units sent 5 (C4 captures). */
#define RADIANT_PROFILE_RADAR_REQ_COUNT0_COPIES        5u

struct radiant_profile_radar {
	struct radiant_profile_radar_cfg cfg;
	struct radiant_profile_radar_target targets[RADIANT_PROFILE_RADAR_MAX_TARGETS];
	struct radiant_profile_radar_error  error;
	bool     error_active;
	/* false = target detection not functioning (spec 6.3.1): page 1 with
	 * clear-targets is the main page. */
	bool     detecting;
	uint8_t  state;          /* RADIANT_PROFILE_RADAR_STATE_* */
	uint16_t state_left;     /* messages left in a shutdown/aborted pattern */
	bool     off;
	uint32_t m;              /* message counter */
	uint16_t since_background;
	uint8_t  background_next; /* 0, 1, 2 = page 80, 81, 82 */
	bool     ab_toggle;       /* >4 targets: next is page 49 when set */
	uint8_t  req_page;        /* a page-70 request being served, 0 = none */
	uint8_t  req_left;
	uint8_t  stay_on_wait;    /* >0: messages until the aborted pattern */
	uint16_t pattern_m;       /* message index within the current pattern */
};

int radiant_profile_radar_init(struct radiant_profile_radar *r,
			       const struct radiant_profile_radar_cfg *cfg);

/* Replace the reported targets (n <= 8, entries with threat 0 are dropped
 * and the rest packed in order). -EINVAL for n > 8. */
int radiant_profile_radar_set_targets(struct radiant_profile_radar *r,
				      const struct radiant_profile_radar_target *t,
				      uint8_t n);

/* Enter (e != NULL) or leave (NULL) the error state. */
void radiant_profile_radar_set_error(struct radiant_profile_radar *r,
				     const struct radiant_profile_radar_error *e);

void radiant_profile_radar_set_detecting(struct radiant_profile_radar *r, bool on);

/* A local decision to power off (low battery, button): forced = cannot be
 * aborted (state 3), otherwise Shutdown Requested (state 1). */
void radiant_profile_radar_begin_shutdown(struct radiant_profile_radar *r, bool forced);

/* A page received from a display (page 2 command or page 70 request).
 * Undefined pages and reserved commands are ignored (spec section 7, items
 * 7 and 13) and return -ENOTSUP. */
int radiant_profile_radar_rx(struct radiant_profile_radar *r, const uint8_t *body);

/* Fill `body` with the next message. */
enum radiant_profile_radar_slot radiant_profile_radar_next(struct radiant_profile_radar *r,
							   uint8_t *body);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_RADAR_H_ */
