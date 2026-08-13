/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_profile_controls.c - ANT+ Controls 0x10: page tables, the shared
 * sequence counter, and the master's queue-of-one. Vectors mirror
 * tools/test_ant_pages.py's TestControlsPages.
 */

#include <zephyr/ztest.h>

#include <string.h>

#include "profile_controls.h"

static void controls_init_default(struct profile_controls *ctrl)
{
	struct profile_controls_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id.manufacturer_id = 0x00FFu;
	cfg.id.model_number = 1u;
	cfg.id.sw_revision_supplemental = PROFILE_COMMON_INVALID_U8;
	cfg.id.serial_number = PROFILE_COMMON_INVALID_U32;
	cfg.serial_number = PROFILE_CONTROLS_SERIAL_UNKNOWN;

	zassert_equal(profile_controls_init(ctrl, &cfg), 0);
}

ZTEST(profile_controls, test_av_command_bit_placement)
{
	struct profile_controls ctrl;
	uint8_t body[8];

	controls_init_default(&ctrl);
	ctrl.cfg.serial_number = 0x1234u;

	zassert_equal(profile_controls_encode_av(&ctrl, true,
						 PROFILE_CONTROLS_AV_CMD_PLAY,
						 50u, body), 0);
	zassert_equal(body[0], PROFILE_CONTROLS_PAGE_AV_COMMAND);
	zassert_equal((uint16_t)(body[1] | (body[2] << 8)), 0x1234u);
	zassert_equal(body[3], 0u, "sequence starts at 0");
	zassert_equal(body[6], 50u);
	zassert_equal(body[7] & 0x80u, 0x80u, "video flag");
	zassert_equal(body[7] & 0x7Fu, PROFILE_CONTROLS_AV_CMD_PLAY);
}

ZTEST(profile_controls, test_sequence_is_shared_across_both_page_types)
{
	struct profile_controls ctrl;
	uint8_t body[8];

	controls_init_default(&ctrl);

	/* Audio command (seq 0), generic command (seq 1), audio command
	 * (seq 2) - one series, per the document's own example. */
	zassert_equal(profile_controls_encode_av(&ctrl, false,
						 PROFILE_CONTROLS_AV_CMD_PAUSE,
						 PROFILE_CONTROLS_AV_VOLUME_DEFAULT,
						 body), 0);
	zassert_equal(body[3], 0u);

	zassert_equal(profile_controls_encode_generic(&ctrl, 1u, 2u,
						      PROFILE_CONTROLS_GENERIC_START,
						      body), 0);
	zassert_equal(body[5], 1u);

	zassert_equal(profile_controls_encode_av(&ctrl, false,
						 PROFILE_CONTROLS_AV_CMD_STOP,
						 PROFILE_CONTROLS_AV_VOLUME_DEFAULT,
						 body), 0);
	zassert_equal(body[3], 2u);
}

ZTEST(profile_controls, test_generic_command_is_sixteen_bits)
{
	struct profile_controls ctrl;
	uint8_t body[8];

	controls_init_default(&ctrl);
	zassert_equal(profile_controls_encode_generic(&ctrl, 1u, 2u,
						      PROFILE_CONTROLS_GENERIC_CUSTOM_MIN,
						      body), 0);
	zassert_equal((uint16_t)(body[6] | (body[7] << 8)),
		      PROFILE_CONTROLS_GENERIC_CUSTOM_MIN,
		      "16-bit command field, despite the source table's own "
		      "length column");
}

ZTEST(profile_controls, test_generic_reserved_range_is_refused)
{
	struct profile_controls ctrl;
	uint8_t body[8];

	controls_init_default(&ctrl);
	zassert_equal(profile_controls_encode_generic(&ctrl, 0u, 0u, 5u, body),
		      -EINVAL, "5..31 reserved");
	zassert_equal(profile_controls_encode_generic(&ctrl, 0u, 0u, 1000u,
						      body),
		      -EINVAL, "37..32767 reserved");
}

ZTEST(profile_controls, test_no_command_does_not_advance_sequence)
{
	struct profile_controls ctrl;
	uint8_t body[8];

	controls_init_default(&ctrl);
	zassert_equal(profile_controls_encode_generic(&ctrl, 1u, 2u,
						      PROFILE_CONTROLS_GENERIC_START,
						      body), 0);
	zassert_equal(ctrl.sequence, 1u);

	zassert_equal(profile_controls_encode_generic(&ctrl, 1u, 2u,
						      PROFILE_CONTROLS_GENERIC_NO_COMMAND,
						      body), 0);
	zassert_equal(ctrl.sequence, 1u, "No Command must not look like a "
		      "new command to a receiver differencing sequence "
		      "numbers");
}

ZTEST(profile_controls, test_av_command_number_is_seven_bits)
{
	struct profile_controls ctrl;
	uint8_t body[8];

	controls_init_default(&ctrl);
	zassert_equal(profile_controls_encode_av(&ctrl, false, 0x80u, 0u, body),
		      -EINVAL);
}

ZTEST(profile_controls, test_round_trip)
{
	struct profile_controls ctrl;
	uint8_t  body[8];
	bool     av = false;
	uint8_t  command = 0u;
	uint8_t  volume = 0u;
	uint16_t serial = 0u;
	uint8_t  seq = 0u;

	controls_init_default(&ctrl);
	zassert_equal(profile_controls_encode_av(&ctrl, true,
						 PROFILE_CONTROLS_AV_CMD_MUTE,
						 77u, body), 0);
	zassert_equal(profile_controls_decode_av(body, &av, &command, &volume,
						 &serial, &seq), 0);
	zassert_true(av);
	zassert_equal(command, PROFILE_CONTROLS_AV_CMD_MUTE);
	zassert_equal(volume, 77u);
	zassert_equal(seq, 0u);
}

ZTEST(profile_controls, test_queue_of_one)
{
	struct profile_controls ctrl;

	controls_init_default(&ctrl);
	zassert_equal(profile_controls_send_av(&ctrl, false,
					       PROFILE_CONTROLS_AV_CMD_PLAY,
					       0u), 0);
	zassert_equal(profile_controls_send_av(&ctrl, false,
					       PROFILE_CONTROLS_AV_CMD_STOP,
					       0u), -EBUSY,
		      "a second command before the first is served");
}

ZTEST(profile_controls, test_idle_when_nothing_queued)
{
	struct profile_controls ctrl;
	uint8_t body[8];

	controls_init_default(&ctrl);
	/* No periodic main page on this device type - an idle data slot is
	 * correct, not a bug, when no command is pending. */
	zassert_equal(profile_controls_next(&ctrl, body), PROFILE_SLOT_IDLE);
}

ZTEST(profile_controls, test_send_then_serve)
{
	struct profile_controls ctrl;
	uint8_t body[8];

	controls_init_default(&ctrl);
	zassert_equal(profile_controls_send_generic(&ctrl, 1u, 2u,
						    PROFILE_CONTROLS_GENERIC_LAP),
		      0);
	zassert_equal(profile_controls_next(&ctrl, body), PROFILE_SLOT_DATA);
	zassert_equal(body[0], PROFILE_CONTROLS_PAGE_GENERIC);
	zassert_equal((uint16_t)(body[6] | (body[7] << 8)),
		      PROFILE_CONTROLS_GENERIC_LAP);

	/* Served exactly once. */
	zassert_equal(profile_controls_next(&ctrl, body), PROFILE_SLOT_IDLE);
}

ZTEST_SUITE(profile_controls, NULL, NULL, NULL, NULL, NULL);
