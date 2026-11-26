/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_profile_radar.c - ANT+ Bike Radar 0x28: page codecs and the master's
 * transmission patterns. Vectors mirror tools/test_ant_pages.py's
 * TestRadarPages; the "varia_" vectors are frames captured on air from
 * Garmin units (archive/captures/radio/2026-11-25-varia-*), the "spec_only_"
 * cases are behaviour no unit has been seen to produce.
 */

#include <zephyr/ztest.h>

#include <string.h>

#include <radiant/radiant_profile_radar.h>

static const uint8_t two_targets[8] = {0x30, 0x09, 0x09, 0x0A, 0x05, 0x00, 0x53, 0x00};

ZTEST(radiant_profile_radar, test_targets_bit_placement)
{
	struct radiant_profile_radar_target t[4];
	struct radiant_profile_radar_target got[4];
	uint8_t body[8];
	uint8_t page = 0u;

	memset(t, 0, sizeof(t));
	t[0] = (struct radiant_profile_radar_target){true, 1u, 1u, 10u, 3u};
	t[1] = (struct radiant_profile_radar_target){true, 2u, 2u, 20u, 5u};
	zassert_equal(radiant_profile_radar_encode_targets(0x30u, t, body), 0);
	zassert_mem_equal(body, two_targets, 8);

	zassert_equal(radiant_profile_radar_decode_targets(body, &page, got), 0);
	zassert_equal(page, 0x30u);
	zassert_true(got[0].valid);
	zassert_equal(got[0].side, RADIANT_PROFILE_RADAR_SIDE_RIGHT);
	zassert_equal(got[1].threat, RADIANT_PROFILE_RADAR_THREAT_FAST);
	zassert_equal(got[1].range_code, 20u);
	zassert_equal(RADIANT_PROFILE_RADAR_RANGE_MM(got[1].range_code), 62500u);
	zassert_equal(RADIANT_PROFILE_RADAR_SPEED_MM_S(got[1].speed_code), 15200u);
	zassert_false(got[2].valid);
	zassert_false(got[3].valid);
}

ZTEST(radiant_profile_radar, test_full_scale_fields_do_not_bleed)
{
	struct radiant_profile_radar_target t[4];
	struct radiant_profile_radar_target got[4];
	uint8_t body[8];

	for (int i = 0; i < 4; i++) {
		t[i] = (struct radiant_profile_radar_target){true, 3u, 3u, 63u, 15u};
	}
	zassert_equal(radiant_profile_radar_encode_targets(0x31u, t, body), 0);
	for (int i = 1; i < 8; i++) {
		zassert_equal(body[i], 0xFFu, "byte %d", i);
	}
	t[2].range_code = 0u;
	t[2].speed_code = 0u;
	zassert_equal(radiant_profile_radar_encode_targets(0x31u, t, body), 0);
	zassert_equal(radiant_profile_radar_decode_targets(body, NULL, got), 0);
	zassert_equal(got[1].range_code, 63u);
	zassert_equal(got[2].range_code, 0u);
	zassert_equal(got[3].range_code, 63u);
	zassert_equal(got[2].speed_code, 0u);
	zassert_equal(got[3].speed_code, 15u);
}

ZTEST(radiant_profile_radar, test_threat_none_encodes_all_zero)
{
	struct radiant_profile_radar_target t[4];
	uint8_t body[8];

	memset(t, 0, sizeof(t));
	t[0] = (struct radiant_profile_radar_target){false, 0u, 2u, 40u, 9u};
	zassert_equal(radiant_profile_radar_encode_targets(0x30u, t, body), 0);
	for (int i = 1; i < 8; i++) {
		zassert_equal(body[i], 0u);
	}
}

ZTEST(radiant_profile_radar, test_varia_idle_page_48)
{
	/* Every Garmin unit idles on page 48 with no targets: all zero. */
	static const uint8_t idle[8] = {0x30, 0, 0, 0, 0, 0, 0, 0};
	struct radiant_profile_radar_target got[4];

	zassert_equal(radiant_profile_radar_decode_targets(idle, NULL, got), 0);
	for (int i = 0; i < 4; i++) {
		zassert_false(got[i].valid);
	}
}

ZTEST(radiant_profile_radar, test_unit_rounding)
{
	zassert_equal(radiant_profile_radar_range_code(0u), 0u);
	zassert_equal(radiant_profile_radar_range_code(1562u), 0u);
	zassert_equal(radiant_profile_radar_range_code(1563u), 1u);
	zassert_equal(radiant_profile_radar_range_code(196875u), 63u);
	zassert_equal(radiant_profile_radar_range_code(500000u), 63u);
	zassert_equal(radiant_profile_radar_speed_code(15200u), 5u);
	zassert_equal(radiant_profile_radar_speed_code(99999u), 15u);
}

ZTEST(radiant_profile_radar, test_status_both_reserved_forms)
{
	struct radiant_profile_radar_status s = {
		.state = RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_REQUESTED,
		.clear_targets = true,
	};
	struct radiant_profile_radar_status got;
	uint8_t body[8];
	static const uint8_t spec[8] = {0x01, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFE};
	static const uint8_t legacy[8] = {0x01, 0x02, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

	zassert_equal(radiant_profile_radar_encode_status(&s, body), 0);
	zassert_mem_equal(body, spec, 8);

	zassert_equal(radiant_profile_radar_decode_status(legacy, &got), 0);
	zassert_equal(got.state, RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_ABORTED);
	zassert_false(got.clear_targets, "bit set = no action");
	zassert_true(got.legacy_reserved);
	zassert_equal(radiant_profile_radar_decode_status(spec, &got), 0);
	zassert_true(got.clear_targets);
	zassert_false(got.legacy_reserved);
}

ZTEST(radiant_profile_radar, test_command_and_request)
{
	uint8_t body[8];
	uint8_t cmd = 0xFFu, page = 0u, count = 0u;
	bool acked = true;

	zassert_equal(radiant_profile_radar_encode_command(
			      RADIANT_PROFILE_RADAR_CMD_SHUTDOWN, body), 0);
	zassert_equal(body[0], 0x02u);
	zassert_equal(body[1] & 0x03u, 1u);
	zassert_equal(radiant_profile_radar_decode_command(body, &cmd), 0);
	zassert_equal(cmd, RADIANT_PROFILE_RADAR_CMD_SHUTDOWN);
	zassert_equal(radiant_profile_radar_encode_command(2u, body), -EINVAL);

	zassert_equal(radiant_profile_radar_encode_request(0x52u, 2u, body), 0);
	zassert_equal(radiant_profile_radar_decode_request(body, &page, &count, &acked), 0);
	zassert_equal(page, 0x52u);
	zassert_equal(count, 2u);
	zassert_false(acked);
}

ZTEST(radiant_profile_radar, test_spec_only_error_page)
{
	struct radiant_profile_radar_error e = {
		.component = 0x0Fu, .level = RADIANT_PROFILE_RADAR_ERR_LEVEL_WARNING,
		.profile_code = RADIANT_PROFILE_RADAR_ERR_UNIT_SKEW, .mfg_code = 0x12345678u,
	};
	struct radiant_profile_radar_error got;
	uint8_t body[8];
	static const uint8_t want[8] = {0x57, 0xFF, 0x4F, 0x01, 0x78, 0x56, 0x34, 0x12};

	zassert_equal(radiant_profile_radar_encode_error(&e, body), 0);
	zassert_mem_equal(body, want, 8);
	zassert_equal(radiant_profile_radar_decode_error(body, &got), 0);
	zassert_equal(got.level, 1u);
	zassert_equal(got.mfg_code, 0x12345678u);
}

static struct radiant_profile_radar master;

static void master_init_cfg(bool power_off_on_command)
{
	struct radiant_profile_radar_cfg cfg;

	memset(&cfg, 0, sizeof(cfg));
	cfg.id.manufacturer_id = 0x00FFu;
	cfg.id.serial_number = 0xFFFFFFFFu;
	cfg.power_off_on_command = power_off_on_command;
	zassert_equal(radiant_profile_radar_init(&master, &cfg), 0);
}

static void master_init(void)
{
	master_init_cfg(false);
}

ZTEST(radiant_profile_radar, test_varia_shutdown_command_stays_on)
{
	/* The Edge's exact command bytes (C2/C3 captures). Every Garmin unit
	 * answered them the same way (C5): ~1.5 s later, "Shutdown Aborted"
	 * alternating with page 48 for 10.0 s, then normal - never off. */
	static const uint8_t edge_shutdown[8] = {0x02, 0xFD, 0xFF, 0xFF, 0xFF, 0xFF,
						 0xFF, 0xFF};
	uint8_t body[8];
	int first = -1, last = -1, status = 0;

	master_init();
	zassert_equal(radiant_profile_radar_rx(&master, edge_shutdown), 0);
	for (int i = 0; i < 200; i++) {
		enum radiant_profile_radar_slot k = radiant_profile_radar_next(&master, body);

		zassert_not_equal(k, RADIANT_PROFILE_RADAR_SLOT_OFF);
		if (k == RADIANT_PROFILE_RADAR_SLOT_STATUS) {
			zassert_equal(body[1] & 3u, RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_ABORTED);
			zassert_equal(body[7] & 1u, 1u, "aborted: no clear request");
			if (first < 0) {
				first = i;
			}
			last = i;
			status++;
		}
	}
	zassert_equal(first, 11, "pattern starts ~1.5 s (12 messages) later");
	zassert_true(last - first >= 79, "~10 s of pattern, got %d", last - first);
	zassert_true(status * 2 >= last - first, "status every other message");
	zassert_equal(master.state, RADIANT_PROFILE_RADAR_STATE_BROADCASTING);
}

ZTEST(radiant_profile_radar, test_master_background_cadence)
{
	uint8_t body[8];
	int n48 = 0, bg[3] = {0, 0, 0};

	master_init();
	for (int i = 0; i < 650; i++) {
		enum radiant_profile_radar_slot k = radiant_profile_radar_next(&master, body);

		if (k == RADIANT_PROFILE_RADAR_SLOT_TARGETS_A) {
			n48++;
		} else if (k >= RADIANT_PROFILE_RADAR_SLOT_COMMON_80 &&
			   k <= RADIANT_PROFILE_RADAR_SLOT_COMMON_82) {
			bg[k - RADIANT_PROFILE_RADAR_SLOT_COMMON_80]++;
		} else {
			ztest_test_fail();
		}
	}
	/* One background page in 65 (measured on all three Garmin units),
	 * rotating 80/81/82. */
	zassert_equal(bg[0] + bg[1] + bg[2], 10);
	zassert_true(bg[0] >= 3 && bg[1] >= 3 && bg[2] >= 3);
	zassert_equal(n48, 640);
}

ZTEST(radiant_profile_radar, test_spec_only_more_than_four_targets_alternate)
{
	struct radiant_profile_radar_target t[6];
	uint8_t body[8];
	int a = 0, b = 0;

	master_init();
	for (int i = 0; i < 6; i++) {
		t[i] = (struct radiant_profile_radar_target){true, 1u, 0u, (uint8_t)(5 + i), 2u};
	}
	zassert_equal(radiant_profile_radar_set_targets(&master, t, 6u), 0);
	for (int i = 0; i < 200; i++) {
		enum radiant_profile_radar_slot k = radiant_profile_radar_next(&master, body);

		a += (k == RADIANT_PROFILE_RADAR_SLOT_TARGETS_A);
		b += (k == RADIANT_PROFILE_RADAR_SLOT_TARGETS_B);
		if (k == RADIANT_PROFILE_RADAR_SLOT_TARGETS_B) {
			struct radiant_profile_radar_target got[4];

			zassert_equal(radiant_profile_radar_decode_targets(body, NULL, got), 0);
			zassert_equal(got[0].range_code, 9u, "target 5");
			zassert_equal(got[1].range_code, 10u, "target 6");
			zassert_false(got[2].valid);
		}
	}
	zassert_equal(a, 100);
	zassert_equal(b, 100);
}

ZTEST(radiant_profile_radar, test_spec_only_shutdown_pattern_then_off)
{
	uint8_t body[8], cmd[8];
	int status = 0, n = 0;
	enum radiant_profile_radar_slot k;

	master_init_cfg(true);
	(void)radiant_profile_radar_encode_command(RADIANT_PROFILE_RADAR_CMD_SHUTDOWN, cmd);
	zassert_equal(radiant_profile_radar_rx(&master, cmd), 0);
	while ((k = radiant_profile_radar_next(&master, body)) !=
	       RADIANT_PROFILE_RADAR_SLOT_OFF) {
		n++;
		if (k == RADIANT_PROFILE_RADAR_SLOT_STATUS) {
			struct radiant_profile_radar_status s;

			status++;
			zassert_equal(radiant_profile_radar_decode_status(body, &s), 0);
			zassert_equal(s.state, RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_REQUESTED);
		}
		zassert_true(n < 1000);
	}
	/* >= 2.5 s of pattern (21 messages), status 1/8..4/8. */
	zassert_true(n >= 21, "%d", n);
	zassert_true(status * 8 >= n && status * 2 <= n, "%d of %d", status, n);
	zassert_equal(radiant_profile_radar_next(&master, body), RADIANT_PROFILE_RADAR_SLOT_OFF);
}

ZTEST(radiant_profile_radar, test_spec_only_abort_shutdown_pattern)
{
	uint8_t body[8], cmd[8];
	int aborted = 0;

	master_init_cfg(true);
	(void)radiant_profile_radar_encode_command(RADIANT_PROFILE_RADAR_CMD_SHUTDOWN, cmd);
	zassert_equal(radiant_profile_radar_rx(&master, cmd), 0);
	(void)radiant_profile_radar_next(&master, body);
	(void)radiant_profile_radar_encode_command(RADIANT_PROFILE_RADAR_CMD_ABORT_SHUTDOWN, cmd);
	zassert_equal(radiant_profile_radar_rx(&master, cmd), 0);
	zassert_equal(master.state, RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_ABORTED);
	for (int i = 0; i < 200; i++) {
		zassert_not_equal(radiant_profile_radar_next(&master, body),
				  RADIANT_PROFILE_RADAR_SLOT_OFF);
		if (body[0] == RADIANT_PROFILE_RADAR_PAGE_STATUS &&
		    (body[1] & 3u) == RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_ABORTED) {
			aborted++;
		}
	}
	/* >= 10 s of 1-in-8 aborted status, then back to broadcasting. */
	zassert_true(aborted >= 10, "%d", aborted);
	zassert_equal(master.state, RADIANT_PROFILE_RADAR_STATE_BROADCASTING);
}

ZTEST(radiant_profile_radar, test_spec_only_forced_shutdown_ignores_abort)
{
	uint8_t body[8], cmd[8];

	master_init();
	radiant_profile_radar_begin_shutdown(&master, true);
	(void)radiant_profile_radar_encode_command(RADIANT_PROFILE_RADAR_CMD_ABORT_SHUTDOWN, cmd);
	zassert_equal(radiant_profile_radar_rx(&master, cmd), 0);
	zassert_equal(master.state, RADIANT_PROFILE_RADAR_STATE_SHUTDOWN_FORCED);
	for (int i = 0; i < 100; i++) {
		if (radiant_profile_radar_next(&master, body) == RADIANT_PROFILE_RADAR_SLOT_OFF) {
			return;
		}
	}
	ztest_test_fail();
}

ZTEST(radiant_profile_radar, test_error_state_and_request)
{
	struct radiant_profile_radar_error e = {.component = 0xFu, .level = 2u,
						.profile_code = 0u, .mfg_code = 0xFFFFFFFFu};
	uint8_t body[8], req[8];
	int err = 0;

	master_init();
	radiant_profile_radar_set_error(&master, &e);
	for (int i = 0; i < 64; i++) {
		err += radiant_profile_radar_next(&master, body) == RADIANT_PROFILE_RADAR_SLOT_ERROR;
	}
	zassert_true(err * 8 >= 64 * 7, "%d", err);

	radiant_profile_radar_set_error(&master, NULL);
	(void)radiant_profile_radar_encode_request(0x51u, 2u, req);
	zassert_equal(radiant_profile_radar_rx(&master, req), 0);
	zassert_equal(radiant_profile_radar_next(&master, body),
		      RADIANT_PROFILE_RADAR_SLOT_COMMON_81);
	zassert_equal(radiant_profile_radar_next(&master, body),
		      RADIANT_PROFILE_RADAR_SLOT_COMMON_81);
	/* An unsupported page is refused, not answered with something else. */
	(void)radiant_profile_radar_encode_request(0x30u, 2u, req);
	zassert_equal(radiant_profile_radar_rx(&master, req), -ENOTSUP);
}

ZTEST(radiant_profile_radar, test_spec_only_not_detecting_sends_status)
{
	uint8_t body[8];
	struct radiant_profile_radar_status s;

	master_init();
	radiant_profile_radar_set_detecting(&master, false);
	zassert_equal(radiant_profile_radar_next(&master, body),
		      RADIANT_PROFILE_RADAR_SLOT_STATUS);
	zassert_equal(radiant_profile_radar_decode_status(body, &s), 0);
	zassert_true(s.clear_targets);
	zassert_equal(s.state, RADIANT_PROFILE_RADAR_STATE_BROADCASTING);
}

ZTEST_SUITE(radiant_profile_radar, NULL, NULL, NULL, NULL, NULL);
