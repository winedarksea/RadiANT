/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_lr.c - the long-range coded PHY, the length extension, and the two
 * things that only exist because of them.
 *
 * Provenance: docs/decisions/0007-long-range-phy.md plus the Bluetooth core
 * specification's published LE Coded PHY timings (FEC block 1's fixed 376 us
 * and the 8 us coded bit at S=8). See docs/decisions/0002-clean-room-policy.md.
 *
 * This suite cannot establish the phase's actual gate (>= 6 dB improvement in
 * the 5%-loss point for S=8 vs 1M, which needs real silicon). What it does
 * establish is everything that would make that measurement meaningless if
 * wrong:
 *   1. Geometry/airtime arithmetic matches the ADR's FEC-block table.
 *   2. The length field is safe both directions (this is the first
 *      length-from-body format in the project; the failure mode unique to it
 *      is a read past the end of a DMA buffer).
 *   3. Discovery still sweeps 1M only, so "certain within one sweep" holds
 *      with two PHYs in the build.
 *   4. The scheduler actually reads caps.phy_switch_us (it was zero and
 *      unread from the day it was written until this phase).
 *   5. The duty bound refuses at the boundary, one count either side.
 *   6. The collapse round-trips through the unmodified 1M accumulator, which
 *      is what makes a node's choice to collapse invisible to a receiver.
 */

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <radiant/radiant_frame.h>
#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_sched.h>
#include <radiant/radiant_search.h>

#include "fake_radio.h"

#include "profile_schedule.h"
#include "profile_telemetry.h"

/* ---------------------------------------------------------------------------
 * 1. Geometry
 * ---------------------------------------------------------------------------
 */

ZTEST(lr, test_the_format_is_the_one_the_adr_describes)
{
	const struct radiant_pkt_format *f =
		radiant_frame_format(RADIANT_FRAME_CFG_LR);

	zassert_not_null(f, "the long-range configuration has no format");

	zassert_equal(RADIANT_PHY_LR_CODED, f->phy,
		      "the long-range format is not on the coded PHY");
	/* Four, not five. The coded PHY's access address is a fixed 32 bits, so
	 * this is the hardware's number rather than a preference - and it is one
	 * more than search's three, which is where the false-trigger argument
	 * comes from. */
	zassert_equal(4, f->addr_len, "the long-range address is not 4 bytes");
	zassert_equal(RADIANT_LEN_FROM_BODY, f->len_mode,
		      "the long-range format is not length-from-body");
	zassert_equal(0, f->len_offset, "the length byte moved off body[0]");
	zassert_equal(1, f->len_bias,
		      "the length no longer counts everything after itself");
	zassert_equal(40, f->max_body_len,
		      "the long-range body ceiling is not 40");

	/* The buffer ceiling was raised FROM 32 for this and for nothing else.
	 * If it ever drops back, the backend's DMA buffers are too small for the
	 * longest frame this project can build, and the symptom is a truncation
	 * the CRC turns into ordinary packet loss. */
	zassert_true(f->max_body_len <= RADIANT_RADIO_BODY_MAX,
		     "the long-range body no longer fits the HAL ceiling");

	/* The two ANT formats did not move. This is the whole compatibility
	 * claim of the phase, asserted rather than assumed. */
	zassert_equal(RADIANT_PHY_1M_GFSK,
		      radiant_frame_format(RADIANT_FRAME_CFG_TRACKING)->phy, NULL);
	zassert_equal(RADIANT_PHY_1M_GFSK,
		      radiant_frame_format(RADIANT_FRAME_CFG_SEARCH)->phy, NULL);
	zassert_equal(RADIANT_LEN_FIXED,
		      radiant_frame_format(RADIANT_FRAME_CFG_TRACKING)->len_mode,
		      "the ANT tracking format acquired a length field");
	zassert_equal(RADIANT_LEN_FIXED,
		      radiant_frame_format(RADIANT_FRAME_CFG_SEARCH)->len_mode,
		      "the ANT search format acquired a length field");
}

ZTEST(lr, test_the_address_is_a_whole_device_number)
{
	struct radiant_channel_id id = {
		.device_number = 0xBEEFu,
		.device_type = 0x60u,
		.trans_type = 0x05u,
	};
	uint8_t addr[RADIANT_FRAME_ADDR_MAX];
	int rc;

	rc = radiant_frame_addr(RADIANT_FRAME_CFG_LR, radiant_net_addr_ant_plus,
				&id, addr, sizeof(addr));
	zassert_equal(4, rc, "the long-range address is not 4 bytes");

	zassert_equal(0xA6u, addr[0], NULL);
	zassert_equal(0xC5u, addr[1], NULL);
	/* Low byte first, exactly as the two ANT configurations do it. */
	zassert_equal(0xEFu, addr[2], "devnum_lo is not the third byte");
	zassert_equal(0xBEu, addr[3], "devnum_hi is not the fourth byte");

	/*
	 * THE DEVICE TYPE IS NOT IN THE ADDRESS, which is the difference from
	 * tracking and the reason it is in the body. A test that only checked
	 * the length would pass with the device type written over devnum_hi.
	 */
	zassert_equal(5, radiant_frame_addr_len(RADIANT_FRAME_CFG_TRACKING),
		      "tracking's address changed");
}

ZTEST(lr, test_the_airtime_reproduces_the_fec_block_table)
{
	/*
	 * 1 M, eight-byte payload, tracking geometry:
	 *   (preamble 1 + address 5 + body 10 + CRC 2) * 8 us = 144 us
	 * The ADR's table says ~150 us, which is this rounded.
	 */
	zassert_equal(144u,
		      radiant_frame_airtime_us(RADIANT_FRAME_CFG_TRACKING, 8u),
		      "the 1 M airtime is not preamble + address + body + CRC");

	/*
	 * S=8, eight-byte payload: FEC block1 376us + body(12)+CRC(2)=14 bytes
	 * at 64us (896us) + TERM2 24us = 1296us. The ADR's ~1.23ms table used a
	 * three-byte header; this format's fourth byte is the length byte the
	 * extension needs, accounting for the 64us difference.
	 */
	zassert_equal(1296u, radiant_frame_airtime_us(RADIANT_FRAME_CFG_LR, 8u),
		      "the S=8 airtime does not match the FEC-block arithmetic");

	/* Every payload byte is 64 us and they compound - the sentence the duty
	 * bound exists because of. */
	zassert_equal(1296u + 64u,
		      radiant_frame_airtime_us(RADIANT_FRAME_CFG_LR, 9u), NULL);

	/* The longest frame this format can build: body 40 + CRC 2 = 42 bytes.
	 * 376 + 42*64 + 24 = 3088 us. */
	zassert_equal(3088u,
		      radiant_frame_airtime_us(RADIANT_FRAME_CFG_LR,
					       RADIANT_FRAME_PAYLOAD_LR_MAX),
		      "the longest long-range frame is not ~3.1 ms");

	/* A payload the format cannot carry is 0, not a plausible number. */
	zassert_equal(0u,
		      radiant_frame_airtime_us(RADIANT_FRAME_CFG_LR,
					       RADIANT_FRAME_PAYLOAD_LR_MAX + 1u),
		      NULL);
}

/* ---------------------------------------------------------------------------
 * 2. The length field, in both directions
 * ---------------------------------------------------------------------------
 */

ZTEST(lr, test_the_body_round_trips_and_states_its_own_length)
{
	static const uint8_t payload[12] = {
		0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
		0x16, 0x17, 0x18, 0x19, 0x1A, 0x1B,
	};
	struct radiant_channel_id id = {
		.device_number = 0x1234u,
		.device_type = 0x60u,
		.trans_type = 0x05u,
	};
	struct radiant_channel_id got;
	uint8_t body[RADIANT_FRAME_LR_BODY_MAX];
	const uint8_t *pl = NULL;
	uint8_t pl_len = 0;
	uint8_t ctrl = 0;
	int rc;

	rc = radiant_frame_lr_body(&id, RADIANT_CTRL_BROADCAST, payload,
				   sizeof(payload), body, sizeof(body));
	zassert_equal(4 + (int)sizeof(payload), rc,
		      "the body is not header plus payload");

	/* The length byte counts everything after itself, which is the RADIO's
	 * own convention and is what makes len_bias 1. */
	zassert_equal((uint8_t)(rc - 1), body[0],
		      "the length byte does not count everything after itself");
	zassert_equal(0x60u, body[1], "the device type is not in the body");
	zassert_equal(0x05u, body[2], NULL);
	zassert_equal(RADIANT_CTRL_BROADCAST, body[3], NULL);

	memset(&got, 0, sizeof(got));
	rc = radiant_frame_lr_parse(body, (uint8_t)(4 + sizeof(payload)), &got,
				    &ctrl, &pl, &pl_len);
	zassert_equal(RADIANT_FRAME_OK, rc, "a well formed body did not parse");
	zassert_equal(0x60u, got.device_type, NULL);
	zassert_equal(0x05u, got.trans_type, NULL);
	zassert_equal(RADIANT_CTRL_BROADCAST, ctrl, NULL);
	zassert_equal(sizeof(payload), pl_len, NULL);
	zassert_mem_equal(payload, pl, sizeof(payload), NULL);

	/* The device number is not in the body - it's in the on-air address,
	 * consumed by the matcher, same as devnum_lo in search. */
	zassert_equal(0u, got.device_number,
		      "the parser invented a device number");
}

ZTEST(lr, test_a_length_that_disagrees_with_the_frame_is_refused)
{
	struct radiant_channel_id id = {
		.device_number = 1u, .device_type = 0x60u, .trans_type = 1u,
	};
	uint8_t body[RADIANT_FRAME_LR_BODY_MAX];
	int rc;

	rc = radiant_frame_lr_body(&id, RADIANT_CTRL_BROADCAST, NULL, 0u, body,
				   sizeof(body));
	zassert_equal(4, rc, "a payload-free body is not just the header");

	/* Checked explicitly rather than left to the CRC: the CRC covers the
	 * length byte, but "the CRC would have caught it" is only probabilistic
	 * - the residual case is a read past the end of a backend's DMA buffer. */
	body[0] = 30u; /* claims 31 body bytes; 4 were delivered */
	zassert_equal(RADIANT_FRAME_ETRUNC,
		      radiant_frame_lr_parse(body, 4u, NULL, NULL, NULL, NULL),
		      "a length byte longer than the frame was believed");

	body[0] = 1u; /* claims 2; 4 delivered */
	zassert_equal(RADIANT_FRAME_ETRUNC,
		      radiant_frame_lr_parse(body, 4u, NULL, NULL, NULL, NULL),
		      "a length byte shorter than the frame was believed");

	/* Shorter than the header cannot be a frame at all. */
	zassert_equal(RADIANT_FRAME_ETRUNC,
		      radiant_frame_lr_parse(body, 3u, NULL, NULL, NULL, NULL),
		      NULL);

	/* An over-long payload is refused at composition, not truncated. */
	{
		static uint8_t big[RADIANT_FRAME_PAYLOAD_LR_MAX + 1];

		zassert_equal(RADIANT_FRAME_EINVAL,
			      radiant_frame_lr_body(&id, RADIANT_CTRL_BROADCAST,
						    big, sizeof(big), body,
						    sizeof(body)),
			      "an over-long payload was accepted");
	}
}

ZTEST(lr, test_the_ant_encoders_refuse_the_long_range_configuration)
{
	struct radiant_frame f;
	struct radiant_frame_wire w;
	struct radiant_channel_id id = {
		.device_number = 1u, .device_type = 0x60u, .trans_type = 1u,
	};
	struct radiant_ctrl_fields cf;
	uint8_t buf[64];

	memset(&cf, 0, sizeof(cf));
	cf.slot_opener = true; /* 0x0A, a measured broadcast */
	zassert_equal(RADIANT_FRAME_OK,
		      radiant_frame_make(&f, &id, &cf, NULL, 0u), NULL);

	/* Refused, not approximated: struct radiant_frame_wire's body buffer is
	 * sized for ANT, and body_write()'s tracking branch would lay a
	 * long-range frame out with a transmission type where the length
	 * belongs. No shared path exists on purpose. */
	zassert_equal(RADIANT_FRAME_EINVAL,
		      radiant_frame_encode(RADIANT_FRAME_CFG_LR,
					   radiant_net_addr_ant_plus, &f, &w),
		      "the ANT encoder accepted the long-range configuration");

	memset(&w, 0, sizeof(w));
	w.addr_len = 4u;
	w.body_len = 12u;
	zassert_equal(RADIANT_FRAME_EINVAL,
		      radiant_frame_decode(RADIANT_FRAME_CFG_LR, &w, 0u, &f),
		      "the ANT decoder accepted the long-range configuration");

	zassert_equal(RADIANT_FRAME_EINVAL,
		      radiant_frame_from_bytes(RADIANT_FRAME_CFG_LR, buf,
					       sizeof(buf), &w),
		      "the flat form accepted the long-range configuration");
}

/* ---------------------------------------------------------------------------
 * 3. Discovery did not move
 * ---------------------------------------------------------------------------
 */

ZTEST(lr, test_discovery_stays_on_one_megabit)
{
	struct radiant_search s;
	struct radiant_search_cfg cfg;
	struct radiant_search_window win;
	struct radiant_search_id_filter want;

	fake_radio_caps_preset_nrf();
	/* A backend that HAS the coded PHY. The question is whether search uses
	 * it, and a build without it could not answer. */
	fake_radio_set_phys(fake_phys_1m_lr, fake_phys_1m_lr_count);

	radiant_search_cfg_default(&cfg);
	zassert_equal(RADIANT_SEARCH_OK,
		      radiant_search_init(&s, &cfg, NULL, NULL), NULL);

	memset(&want, 0, sizeof(want));
	zassert_equal(RADIANT_SEARCH_OK,
		      radiant_search_begin(&s, 0u, RADIANT_SEARCH_MODE_ACQUIRE,
					   &want, 0u,
					   RADIANT_SEARCH_TIMEOUT_NONE),
		      NULL);
	zassert_equal(RADIANT_SEARCH_OK,
		      radiant_search_window(&s, 1000u, &win), NULL);

	/* An LR node is found via its 1M descriptor or a sync handoff, never by
	 * sweeping the coded PHY: a second swept PHY would double the sweep,
	 * and "certain within one sweep" would quietly become "two". */
	zassert_equal(RADIANT_PHY_1M_GFSK, win.fmt->phy,
		      "the search sweep moved to the coded PHY");
	zassert_equal(radiant_frame_format(RADIANT_FRAME_CFG_SEARCH), win.fmt,
		      "the search sweep is not using the search format");
	zassert_equal(RADIANT_RF_INDEX_ANT_PLUS, win.rf_index,
		      "the search sweep moved off RF 57");

	/* And the sweep length is unchanged: 8 filters, 32 sets. */
	zassert_equal(8u, radiant_search_filters_per_window(&s), NULL);
	zassert_equal(32u, radiant_search_sets(&s), NULL);
}

/* ---------------------------------------------------------------------------
 * 4. The scheduler pays for the switch
 * ---------------------------------------------------------------------------
 */

static struct radiant_rx_filter lr_filter(uint16_t devnum)
{
	struct radiant_rx_filter f;
	struct radiant_channel_id id;
	int rc;

	memset(&f, 0, sizeof(f));
	memset(&id, 0, sizeof(id));
	id.device_number = devnum;
	id.device_type = 0x60u;

	rc = radiant_frame_addr(RADIANT_FRAME_CFG_LR, radiant_net_addr_ant_plus,
				&id, f.addr, sizeof(f.addr));
	f.addr_len = (uint8_t)rc;
	return f;
}

static struct radiant_rx_filter tracking_filter(uint16_t devnum)
{
	struct radiant_rx_filter f;
	struct radiant_channel_id id;
	int rc;

	memset(&f, 0, sizeof(f));
	memset(&id, 0, sizeof(id));
	id.device_number = devnum;
	id.device_type = 0x78u;

	rc = radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING,
				radiant_net_addr_ant_plus, &id, f.addr,
				sizeof(f.addr));
	f.addr_len = (uint8_t)rc;
	return f;
}

ZTEST(lr, test_a_change_of_phy_is_budgeted_and_a_repeat_is_not)
{
	static struct radiant_rx_filter f1m;
	static struct radiant_rx_filter flr;
	struct radiant_sched_rx req;
	const struct fake_radio_arm *rec;
	const struct radiant_sched_stats *st;
	radiant_time_t open_same;
	radiant_time_t open_switch;
	uint32_t lead;
	uint32_t switch_us;

	/* RAIL's preset (not nRF's, which switches PHY for free) is the only
	 * combination that distinguishes a scheduler that budgets the switch
	 * from one that doesn't. */
	fake_radio_caps_preset_rail();
	fake_radio_set_phys(fake_phys_1m_lr, fake_phys_1m_lr_count);
	lead = fake_radio_caps_mut()->min_arm_lead_us;
	switch_us = fake_radio_caps_mut()->phy_switch_us;
	zassert_true(switch_us > 0u,
		     "this test needs a backend that pays for a switch");

	zassert_ok(radiant_sched_init(NULL, NULL));
	zassert_ok(radiant_radio_init(radiant_sched_radio_cbs(), NULL));
	zassert_ok(radiant_radio_enable());

	f1m = tracking_filter(0x1111u);
	flr = lr_filter(0x2222u);

	/* First window, 1 M. The PHY is unknown at this point, so it is charged
	 * the switch - the conservative direction, stated in radiant_sched.c. */
	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &f1m;
	req.n_filters = 1u;
	req.t_open = radiant_radio_now() + 1u;
	req.t_close = req.t_open + 5000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_sched_request_rx(0u, &req),
		      NULL);
	radiant_sched_tick();
	fake_radio_advance(200000u);

	/* Second window, 1 M again: no switch, so it opens at the plain lead. */
	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &f1m;
	req.n_filters = 1u;
	req.t_open = radiant_radio_now() + 1u;
	req.t_close = req.t_open + 5000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_sched_request_rx(0u, &req),
		      NULL);
	radiant_sched_tick();

	rec = fake_radio_last_arm();
	zassert_not_null(rec, "no receive window was armed");
	zassert_equal(FAKE_RADIO_ARM_RX, rec->kind, NULL);
	open_same = rec->t_open - rec->t_arm;
	fake_radio_advance(200000u);

	/* Third window, coded: the PHY changes, so it must be led by the switch
	 * as well. */
	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_LR);
	req.rf_index = RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &flr;
	req.n_filters = 1u;
	req.t_open = radiant_radio_now() + 1u;
	req.t_close = req.t_open + 5000u;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_sched_request_rx(0u, &req),
		      NULL);
	radiant_sched_tick();

	rec = fake_radio_last_arm();
	zassert_not_null(rec, "no coded window was armed");
	zassert_equal(FAKE_RADIO_ARM_RX, rec->kind, NULL);
	zassert_equal(RADIANT_PHY_LR_CODED, rec->fmt.phy,
		      "the coded window was not armed on the coded PHY");
	open_switch = rec->t_open - rec->t_arm;

	/* The assertion the field exists for: a window on a PHY the radio isn't
	 * already configured for opens later by exactly phy_switch_us. Before
	 * this phase the difference was zero - nothing read the field. */
	zassert_equal(open_same + switch_us, open_switch,
		      "a change of PHY was not budgeted: same %u, switched %u, "
		      "lead %u, switch %u",
		      (unsigned int)open_same, (unsigned int)open_switch,
		      lead, switch_us);

	st = radiant_sched_stats_get();
	zassert_true(st->phy_switches >= 2u,
		     "the PHY switch was not counted (%u)",
		     st->phy_switches);
}

/* ---------------------------------------------------------------------------
 * 5. The duty bound
 * ---------------------------------------------------------------------------
 */

ZTEST(lr, test_the_duty_bound_refuses_at_the_boundary)
{
	/* The vocabulary this phase inherits rather than redefines. */
	zassert_equal(125u, profile_sched_coding_kbps(PROFILE_SCHED_CODING_S8),
		      NULL);
	zassert_true(profile_sched_coding_implemented(PROFILE_SCHED_CODING_S8),
		     "S=8 is the rate this phase makes real and it is refused");
	zassert_false(profile_sched_coding_implemented(PROFILE_SCHED_CODING_S2),
		      "S=2 became implemented without a decision");

	/* Frame airtime through the profile layer is the frame layer's, which
	 * is the point of the delegation. */
	zassert_equal(1296u,
		      profile_sched_frame_us(PROFILE_SCHED_CODING_S8, 12u),
		      NULL);
	zassert_equal(144u,
		      profile_sched_frame_us(PROFILE_SCHED_CODING_NONE, 10u),
		      NULL);
	/* A rate nothing builds budgets nothing, rather than budgeting as S=8 -
	 * which is the conservative direction and therefore the bug that would
	 * never be noticed. */
	zassert_equal(0u, profile_sched_frame_us(PROFILE_SCHED_CODING_S2, 12u),
		      NULL);

	/*
	 * THE BOUNDARY, ONE COUNT APART. The longest long-range frame is
	 * 3088 us, so it needs a period of at least 4 * 3088 = 12352 us.
	 *   405 counts = 12359 us -> fits
	 *   404 counts = 12329 us -> does not
	 */
	zassert_equal(0, profile_sched_duty_check(PROFILE_SCHED_CODING_S8, 405u,
						  40u),
		      "a frame at 24.98 %% of its period was refused");
	zassert_equal(-EINVAL,
		      profile_sched_duty_check(PROFILE_SCHED_CODING_S8, 404u,
					       40u),
		      "a frame over 25 %% of its period was accepted");

	/* The bound binds only where BOTH factors are present. At 4 Hz - the
	 * ordinary case - a full-length coded frame is 1.2 % and passes. */
	zassert_equal(0, profile_sched_duty_check(PROFILE_SCHED_CODING_S8, 8192u,
						  40u),
		      "a 40-byte frame at 4 Hz was refused");
	/* And an eight-byte 1 M frame is never anywhere near it. */
	zassert_equal(0, profile_sched_duty_check(PROFILE_SCHED_CODING_NONE,
						  8192u, 10u),
		      NULL);

	/* A sparse node has no period, so the rule is skipped rather than
	 * applied to zero (same as the drift bound with no announced clock) -
	 * treating 0 as infinitely fast would refuse every asset tag. */
	zassert_equal(0, profile_sched_duty_check(PROFILE_SCHED_CODING_S8, 0u,
						  40u),
		      "an asynchronous node was held to a period it has not got");

	/* A body the format cannot carry is an error, not a duty verdict. */
	zassert_equal(-EINVAL,
		      profile_sched_duty_check(PROFILE_SCHED_CODING_S8, 8192u,
					       41u),
		      NULL);
}

/* ---------------------------------------------------------------------------
 * 6. The descriptor-set collapse
 * ---------------------------------------------------------------------------
 */

static void build_lr_descriptor(struct profile_descriptor *d, uint8_t n_fields)
{
	uint8_t i;

	memset(d, 0, sizeof(*d));
	d->version = PROFILE_TLM_VERSION;
	d->schema_id = 0x42u;
	d->period = 8192u; /* 4 Hz */
	d->rf_index = PROFILE_TLM_RF_INDEX_DEFAULT;
	/* The LR bit and the coding rate are two statements about one PHY and
	 * must agree - profile_sched_check() refuses them if they do not. */
	d->flags = PROFILE_TLM_FLAG_LR_PHY;
	d->has_schedule = true;
	d->schedule.coding = PROFILE_SCHED_CODING_S8;
	d->schedule.tx_power_dbm = PROFILE_SCHED_TX_POWER_UNSTATED;
	d->n_fields = n_fields;

	for (i = 0u; i < n_fields; i++) {
		d->fields[i].id = (uint8_t)(i + 1u);
		d->fields[i].type = PROFILE_TLM_TYPE_TEMPERATURE;
		d->fields[i].width_code = 4u; /* 8 bits */
		d->fields[i].page = (uint8_t)(1u + (i / 6u));
		d->fields[i].bit_offset = (uint8_t)(8u * (i % 6u));
	}
}

ZTEST(lr, test_the_collapse_round_trips_through_the_unchanged_accumulator)
{
	struct profile_descriptor d;
	struct profile_descriptor got;
	struct profile_desc_rx rx;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	uint8_t payload[RADIANT_FRAME_PAYLOAD_LR_MAX];
	int n_frames;
	int bodies;
	int i;
	int rc = 0;

	/* Two fields plus a schedule frame: 2 + 1 + 2 = 5 frames. */
	build_lr_descriptor(&d, 2u);

	n_frames = profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES);
	zassert_equal(5, n_frames, "the set is not five frames");

	/* Four whole 8-byte frames fit in a 36-byte payload; five frames
	 * therefore take two transmissions rather than five. */
	zassert_equal(4u,
		      profile_desc_frames_per_body(RADIANT_FRAME_PAYLOAD_LR_MAX),
		      NULL);
	bodies = profile_desc_long_count(&d, RADIANT_FRAME_PAYLOAD_LR_MAX);
	zassert_equal(2, bodies, "five frames did not collapse into two wakes");

	/* Makes the collapse invisible to a receiver: bytes feed the same
	 * profile_desc_rx_feed() accumulator a 1M receiver always used, with
	 * the same ordering rules. */
	profile_desc_rx_init(&rx);
	for (i = 0; i < bodies; i++) {
		int len = profile_desc_long_payload(&d, frames,
						    (uint8_t)n_frames,
						    RADIANT_FRAME_PAYLOAD_LR_MAX,
						    (uint8_t)i, payload,
						    sizeof(payload));

		zassert_true(len > 0, "payload %d did not build (%d)", i, len);
		zassert_equal(0, len % PROFILE_TLM_FRAME_LEN,
			      "a partial descriptor frame was packed");
		rc = profile_desc_rx_feed_long(&rx, payload, (uint8_t)len);
		zassert_true(rc >= 0, "feeding payload %d failed (%d)", i, rc);
	}
	zassert_equal(1, rc, "the set did not complete");
	zassert_true(profile_desc_rx_complete(&rx), NULL);

	zassert_equal(0, profile_desc_rx_take(&rx, &got), NULL);
	zassert_equal(d.schema_id, got.schema_id, NULL);
	zassert_equal(d.period, got.period, NULL);
	zassert_equal(d.n_fields, got.n_fields, NULL);
	zassert_equal(d.flags, got.flags, NULL);
	zassert_equal(PROFILE_SCHED_CODING_S8, got.schedule.coding,
		      "the coding rate did not survive the collapse");
	zassert_mem_equal(d.fields, got.fields,
			  sizeof(d.fields[0]) * d.n_fields,
			  "the schema did not survive the collapse");
}

ZTEST(lr, test_how_complete_the_collapse_actually_is)
{
	struct profile_descriptor d;

	/* The arithmetic the ADR corrects the plan on: four whole frames per
	 * transmission is what 36 payload bytes buys. */

	/* The sparse asset tag - no fields, no schedule frame. Two frames, one
	 * wake: the collapse is TOTAL for the node the envelope was written
	 * for, which is the node that needed it most. */
	build_lr_descriptor(&d, 0u);
	d.has_schedule = false;
	d.flags = 0u; /* no LR bit without a rate to go with it */
	zassert_equal(2u, profile_desc_frame_count(&d), NULL);
	d.flags = PROFILE_TLM_FLAG_LR_PHY;
	d.has_schedule = true;
	zassert_equal(3u, profile_desc_frame_count(&d), NULL);
	zassert_equal(1, profile_desc_long_count(&d,
						 RADIANT_FRAME_PAYLOAD_LR_MAX),
		      "an asset tag did not collapse to one wake");

	/* Eight fields plus a schedule frame is eleven frames: three wakes, not
	 * one. The plan said one; one would need a ~76-byte body, which at S=8
	 * is ~5.4 ms of airtime. Three is a 73 % cut and is the honest number. */
	build_lr_descriptor(&d, 8u);
	zassert_equal(11u, profile_desc_frame_count(&d), NULL);
	zassert_equal(3, profile_desc_long_count(&d,
						 RADIANT_FRAME_PAYLOAD_LR_MAX),
		      "the eight-field case is not three wakes");
}

ZTEST(lr, test_a_collapse_is_refused_where_the_phy_does_not_hide_it)
{
	struct profile_descriptor d;
	uint8_t frames[PROFILE_TLM_MAX_FRAMES * PROFILE_TLM_FRAME_LEN];
	uint8_t payload[RADIANT_FRAME_PAYLOAD_LR_MAX];
	int n_frames;

	build_lr_descriptor(&d, 2u);
	n_frames = profile_desc_encode(&d, frames, PROFILE_TLM_MAX_FRAMES);
	zassert_true(n_frames > 0, NULL);

	/* Length extension is permitted only where the PHY already makes us
	 * invisible: a 32-byte frame on RF 57 under the ANT+ network address is
	 * a frame every stock ANT receiver would try (and fail) to parse. */
	d.flags &= (uint8_t)~PROFILE_TLM_FLAG_LR_PHY;
	d.schedule.coding = PROFILE_SCHED_CODING_NONE;
	zassert_equal(-EINVAL,
		      profile_desc_long_check(&d,
					      RADIANT_FRAME_PAYLOAD_LR_MAX),
		      "a 1 M node was allowed to collapse its descriptor set");
	zassert_equal(-EINVAL,
		      profile_desc_long_payload(&d, frames, (uint8_t)n_frames,
						RADIANT_FRAME_PAYLOAD_LR_MAX,
						0u, payload, sizeof(payload)),
		      "the refusal was skippable by asking for a payload");

	/* A payload too small for one whole frame is a refusal, not a silent
	 * fallback: a fallback would make a node's transmission shape depend on
	 * a number nobody looked at. */
	build_lr_descriptor(&d, 2u);
	zassert_equal(-EINVAL, profile_desc_long_count(&d, 7u), NULL);

	/* A feed that is not a whole number of frames is refused rather than
	 * partially accepted, which would accept exactly the corrupted cases
	 * while looking like robustness. */
	{
		struct profile_desc_rx rx;

		profile_desc_rx_init(&rx);
		memset(payload, 0, sizeof(payload));
		zassert_equal(-EPROTO,
			      profile_desc_rx_feed_long(&rx, payload, 12u),
			      "a partial descriptor frame was accepted");
	}
}

static void lr_before(void *fixture)
{
	ARG_UNUSED(fixture);
	fake_radio_reset();
}

ZTEST_SUITE(lr, NULL, NULL, lr_before, NULL, NULL);
