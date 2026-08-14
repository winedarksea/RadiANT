/* SPDX-License-Identifier: Apache-2.0 */
/*
 * profile_controls.h - ANT+ Controls, device type 0x10.
 *
 * THIS FILE CONTAINS NO CRYPTOGRAPHY AND CALLS radiant_sec NOWHERE. No
 * compat client is attached: this device type's page space does not
 * intersect the heart-rate compat allocation at all.
 *
 * SCOPE: implements the actual command path - page 16 (audio/video
 * transport) and page 73 (generic menu/stopwatch, and manufacturer-custom
 * commands) to a controllable master. NOT implemented: the peripheral
 * enumeration model (pages 1, 2, 5, 7, 8, 17, 20, 70, 72) or the text-transfer
 * sub-protocol. A receiver built against this header still interoperates
 * with any Rev 2.0 controllable device's command pages regardless.
 *
 * THE SEQUENCE NUMBER IS SHARED ACROSS EVERY COMMAND PAGE, not per-page-type:
 * tables 9-1 and 9-9 draw from the SAME counter, so an audio command then a
 * generic command then another audio command are N, N+1, N+2. Two independent
 * per-page counters would let a receiver differencing sequence numbers
 * silently misread a stream with no gaps.
 *
 * PAGE 73'S COMMAND FIELD IS 16 BITS, NOT 8, despite table 9-9's length
 * column reading "1 Byte" against its own byte range "6-7" and table 9-8's
 * value range 0..65535. The range/value columns are treated as authoritative
 * and the length column as a transcription artifact;
 * profile_controls_encode_generic() packs the command little-endian across
 * bytes [6..7].
 *
 * "NO COMMAND" IS A REAL PAGE A SENDER MUST BE ABLE TO EMIT. Generic commands
 * go out as PAIRS with the manufacturer ID split across the exchange; when
 * only one command is pending, the second page's command field must be
 * PROFILE_CONTROLS_GENERIC_NO_COMMAND (0xFFFF) with the PRECEDING packet's
 * serial/manufacturer bytes, not fresh ones - so it can't look like a new
 * command to a receiver differencing sequence numbers.
 */

#ifndef RADIANT_PROFILE_CONTROLS_H_
#define RADIANT_PROFILE_CONTROLS_H_

#include <stdbool.h>
#include <stdint.h>

#include "profile_common.h"
#include "profile_sched.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PROFILE_CONTROLS_DEVICE_TYPE 0x10u

/* 8192 counts of 1/32768 s = 4 Hz, the profile's one permitted rate. */
#define PROFILE_CONTROLS_PERIOD 8192u

#define PROFILE_CONTROLS_RF_FREQ 57u

#define PROFILE_CONTROLS_PAGE_AV_COMMAND 0x10u
#define PROFILE_CONTROLS_PAGE_GENERIC    0x49u

/* Table 9-1 byte [7] bit 7. */
#define PROFILE_CONTROLS_AV_AUDIO 0u
#define PROFILE_CONTROLS_AV_VIDEO 1u

/* Table 9-2's command numbers, 0-19. Support (required/optional/not
 * supported) is per audio-device-class and is not transcribed - a policy
 * matter for whoever builds a receiver, not a wire fact. */
#define PROFILE_CONTROLS_AV_CMD_RESERVED           0u
#define PROFILE_CONTROLS_AV_CMD_PLAY               1u
#define PROFILE_CONTROLS_AV_CMD_PAUSE              2u
#define PROFILE_CONTROLS_AV_CMD_STOP               3u
#define PROFILE_CONTROLS_AV_CMD_VOLUME_UP          4u
#define PROFILE_CONTROLS_AV_CMD_VOLUME_DOWN        5u
#define PROFILE_CONTROLS_AV_CMD_MUTE               6u
#define PROFILE_CONTROLS_AV_CMD_TRACK_AHEAD        7u
#define PROFILE_CONTROLS_AV_CMD_TRACK_BACK         8u
#define PROFILE_CONTROLS_AV_CMD_REPEAT_TRACK       9u
#define PROFILE_CONTROLS_AV_CMD_REPEAT_ALL         10u
#define PROFILE_CONTROLS_AV_CMD_REPEAT_OFF         11u
#define PROFILE_CONTROLS_AV_CMD_SHUFFLE_TRACKS     12u
#define PROFILE_CONTROLS_AV_CMD_SHUFFLE_ALBUMS     13u
#define PROFILE_CONTROLS_AV_CMD_SHUFFLE_OFF        14u
#define PROFILE_CONTROLS_AV_CMD_FAST_FORWARD       15u
#define PROFILE_CONTROLS_AV_CMD_FAST_REWIND        16u
#define PROFILE_CONTROLS_AV_CMD_CUSTOM_REPEAT      17u
#define PROFILE_CONTROLS_AV_CMD_CUSTOM_SHUFFLE     18u
#define PROFILE_CONTROLS_AV_CMD_RECORD             19u

/* Page 16 byte [4], volume increment percent. 0x00 reserved, 0xFF is
 * "default increment / invalid" and NOT a real 255%. */
#define PROFILE_CONTROLS_AV_VOLUME_DEFAULT PROFILE_COMMON_INVALID_U8
#define PROFILE_CONTROLS_SERIAL_UNKNOWN    0xFFFFu

/* Table 9-8's generic command numbers. */
#define PROFILE_CONTROLS_GENERIC_MENU_UP     0u
#define PROFILE_CONTROLS_GENERIC_MENU_DOWN   1u
#define PROFILE_CONTROLS_GENERIC_MENU_SELECT 2u
#define PROFILE_CONTROLS_GENERIC_MENU_BACK   3u
#define PROFILE_CONTROLS_GENERIC_HOME        4u
#define PROFILE_CONTROLS_GENERIC_START       32u
#define PROFILE_CONTROLS_GENERIC_STOP        33u
#define PROFILE_CONTROLS_GENERIC_RESET       34u
#define PROFILE_CONTROLS_GENERIC_LENGTH      35u
#define PROFILE_CONTROLS_GENERIC_LAP         36u
/* 32768..65534: manufacturer custom commands, not individually named. */
#define PROFILE_CONTROLS_GENERIC_CUSTOM_MIN  32768u
#define PROFILE_CONTROLS_GENERIC_CUSTOM_MAX  65534u
#define PROFILE_CONTROLS_GENERIC_NO_COMMAND  0xFFFFu

struct profile_controls_cfg {
	struct profile_common_id id;
	/* This node's own serial number, reported in page 16 byte [1..2].
	 * PROFILE_CONTROLS_SERIAL_UNKNOWN if not used - the document's own
	 * default for a remote with no serial to report. */
	uint16_t serial_number;
	uint16_t common_82_every;
	struct profile_common_battery battery;
};

struct profile_controls {
	struct profile_controls_cfg cfg;
	struct profile_sched        sched;

	/* ONE counter, shared by every command page type - see the header. */
	uint8_t sequence;

	/* At most one queued command, per profile_controls_send_av() /
	 * _send_generic()'s -EBUSY rule. */
	bool pending;
	bool pending_generic; /* which of the two shapes below is valid */
	bool     av_flag;
	uint8_t  av_command;
	uint8_t  av_volume;
	uint16_t g_serial;
	uint16_t g_manufacturer;
	uint16_t g_command;

	uint8_t rotation[1]; /* placeholder; this profile has no periodic main
			      * page of its own - see profile_controls.c */
};

/* The page encoders. Each writes eight bytes and returns 0, or -EINVAL. Both
 * advance the shared sequence counter on success and neither may be called
 * with `ctrl == NULL`, since the counter lives there - unlike profile_hr.h's
 * encoders these are not free functions over caller-supplied state, because
 * the sequence must not fork between two independent counters. */

/* Page 0x10. `av` is PROFILE_CONTROLS_AV_AUDIO or _VIDEO. `volume_percent`
 * is 1-100, or PROFILE_CONTROLS_AV_VOLUME_DEFAULT for "use the device's own
 * increment". */
int profile_controls_encode_av(struct profile_controls *ctrl, bool av,
			       uint8_t command, uint8_t volume_percent,
			       uint8_t *out);

int profile_controls_decode_av(const uint8_t *body, bool *av,
			       uint8_t *command, uint8_t *volume_percent,
			       uint16_t *serial_number, uint8_t *sequence);

/* Page 0x49. `slave_serial`/`slave_manufacturer_id` identify the REMOTE
 * sending the command, per section 9.5.1 - not this node's own identity.
 * `command` is a PROFILE_CONTROLS_GENERIC_* value, a raw 0..65535 (the
 * command range this profile actually enumerates, 5..31 and 37..32767 being
 * reserved and refused), or PROFILE_CONTROLS_GENERIC_NO_COMMAND. */
int profile_controls_encode_generic(struct profile_controls *ctrl,
				    uint16_t slave_serial,
				    uint16_t slave_manufacturer_id,
				    uint16_t command, uint8_t *out);

int profile_controls_decode_generic(const uint8_t *body, uint16_t *slave_serial,
				    uint16_t *slave_manufacturer_id,
				    uint16_t *command, uint8_t *sequence);

/* The master. A controllable device's broadcast stream carries common pages
 * 80/81/82 on profile_sched.c's cadence and otherwise has nothing required
 * to say every period - the command pages are slave-initiated (page 73,
 * acknowledged) or master-response (page 16), not periodic main-page content.
 * profile_controls_next() serves an idle data slot whenever no command page
 * is queued. */

int profile_controls_init(struct profile_controls *ctrl,
			  const struct profile_controls_cfg *cfg);

/* Queue an audio/video command to go out in the next data slot. Only one may
 * be queued at a time; a second call before the first is served returns
 * -EBUSY, since this module has no queue depth beyond one. */
int profile_controls_send_av(struct profile_controls *ctrl, bool av,
			     uint8_t command, uint8_t volume_percent);

int profile_controls_send_generic(struct profile_controls *ctrl,
				  uint16_t slave_serial,
				  uint16_t slave_manufacturer_id,
				  uint16_t command);

enum profile_slot_kind profile_controls_next(struct profile_controls *ctrl,
					     uint8_t *body);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_PROFILE_CONTROLS_H_ */
