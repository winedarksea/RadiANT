/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The 0xF1-0xF4 adapter: antr_sec_config(), antr_sec_key_set(),
 * antr_sec_epoch_set(), antr_sec_status_get(). Tests the ADAPTER, not the
 * transforms (radiant_sec's own suite covers those): the two ordering
 * rules, the refusals, and the status encoding.
 *
 * Ordering rules: a channel needs its ID before its key (the device number
 * is bound into the KDF and read from the channel, not the wire) and its
 * period before its epoch (period turns a phase into a packet index).
 * Neither fails loudly if skipped - keys under device 0 verify against
 * nothing, a counter from the wrong period drifts like clock error - so
 * both are refused with ANTW_CHANNEL_IN_WRONG_STATE.
 *
 * Compiled only into the radiant.api.sec_host scenario, the only one
 * that links the adapter.
 */

#include <zephyr/kernel.h>

/* The whole file is behind the symbol because CMakeLists.txt globs every
 * `.c` under `src/` into every scenario and two of three don't link the adapter; the
 * antr_sec_* declarations are behind the same #ifdef in src/ant_radio.h so
 * the two halves appear and disappear together. */
#if defined(CONFIG_RADIANT_SEC_HOST_MESSAGES)

#include <zephyr/ztest.h>
#include <string.h>

#include "ant_radio.h"
#include "ant_wire.h"
#include "ant_radio_radiant_priv.h"

#include <radiant/radiant_channel.h>
#include <radiant/radiant_sec.h>

#define HOST_CH     0u
#define HOST_DEVNUM 0x3A17u
#define HOST_DEVTYPE 0x0Bu
#define HOST_TRANS  0x05u
#define HOST_RF     57u

static const uint8_t host_key[16] = {
	0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
	0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
};

static const uint8_t host_net_key[8] = {
	0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45,
};

/* Assign and identify a channel, without opening it: none of these tests need
 * the radio, and an open channel would leave one armed for the next test. */
static void host_assign(bool with_id, bool with_period)
{
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_network_address_set(0u, host_net_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(HOST_CH,
					  (uint8_t)ANTW_CHANNEL_TYPE_MASTER,
					  0u, 0u));
	if (with_id) {
		zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
			      antr_channel_id_set(HOST_CH, HOST_DEVNUM,
						  HOST_DEVTYPE, HOST_TRANS));
	}
	if (with_period) {
		zassert_equal(
			(antr_err_t)ANTW_RESPONSE_NO_ERROR,
			antr_channel_period_set(
				HOST_CH, RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	}
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(HOST_CH, HOST_RF));
}

static void host_done(void)
{
	(void)antr_channel_unassign(HOST_CH);
}

static uint16_t rd16(const uint8_t *p)
{
	return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	       ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

ZTEST(api, test_sec_host_a_key_needs_a_channel_id_first)
{
	host_assign(false, true);

	/*
	 * WRONG STATE, not INVALID_PARAMETER. The key is fine; it arrived too
	 * early. A host that cannot tell those apart retries the wrong thing -
	 * it re-sends the key rather than sending the channel ID.
	 */
	zassert_equal((antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE,
		      antr_sec_key_set(HOST_CH, 128u, host_key),
		      "a key was accepted on a channel with no ID; the KDF "
		      "would have bound device number 0");

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(HOST_CH, HOST_DEVNUM, HOST_DEVTYPE,
					  HOST_TRANS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(HOST_CH, 128u, host_key));
	host_done();
}

ZTEST(api, test_sec_host_only_a_128_bit_key_is_taken)
{
	host_assign(true, true);

	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_key_set(HOST_CH, 192u, host_key));
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_key_set(HOST_CH, 64u, host_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(HOST_CH, 128u, host_key));
	host_done();
}

ZTEST(api, test_sec_host_a_reserved_switch_bit_is_refused_not_masked)
{
	uint8_t bit;

	host_assign(true, true);
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(HOST_CH, 128u, host_key));

	/*
	 * Masking would enable the switches this version understands and
	 * silently drop the one it does not - so a host asking for a later
	 * switch would be told "yes" and get something weaker than it asked
	 * for, which is the one answer a security surface must never give.
	 */
	for (bit = 0x10u; bit != 0u; bit = (uint8_t)(bit << 1)) {
		zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
			      antr_sec_config(HOST_CH,
					      (uint8_t)(RADIANT_SEC_SW_AUTH |
							bit),
					      2u, 0x01u, 0x0Fu),
			      "reserved bit 0x%02X was accepted",
			      (unsigned)bit);
	}

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_config(HOST_CH, RADIANT_SEC_SW_AUTH, 2u, 0x01u,
				      0x0Fu));
	host_done();
}

ZTEST(api, test_sec_host_the_config_refusals_reach_the_host)
{
	host_assign(true, true);
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(HOST_CH, 128u, host_key));

	/* W must divide 256 and 65536. */
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_config(HOST_CH, RADIANT_SEC_SW_AUTH, 3u, 0x01u,
				      0x0Fu));
	/* 0x00 is the descriptor: securing it makes the node undiscoverable. */
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_config(HOST_CH, RADIANT_SEC_SW_AUTH, 2u, 0x00u,
				      0x0Fu));
	/* 0x50 is common page 80, which every ANT+ receiver understands. */
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_config(HOST_CH, RADIANT_SEC_SW_AUTH, 2u, 0x01u,
				      0x50u));
	/* The descriptor has no counter and therefore no nonce. */
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_config(HOST_CH,
				      RADIANT_SEC_SW_CONF |
					      RADIANT_SEC_SW_DESC_CONF,
				      2u, 0x01u, 0x0Fu));
	host_done();
}

ZTEST(api, test_sec_host_an_epoch_needs_a_key_and_only_moves_forwards)
{
	host_assign(true, true);

	/* No key yet: the epoch has nothing to anchor. */
	zassert_equal((antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE,
		      antr_sec_epoch_set(HOST_CH, 0u, 10u, 0u));

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(HOST_CH, 128u, host_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_config(HOST_CH, RADIANT_SEC_SW_AUTH, 2u, 0x01u,
				      0x0Fu));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_epoch_set(HOST_CH, 0u, 10u, 0u));

	/*
	 * D3. A reboot that restarted the counter under an unchanged epoch is a
	 * two-time pad for X_CONF and a full session replay against X_AUTH, so
	 * the same epoch twice is refused rather than treated as idempotent.
	 */
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_epoch_set(HOST_CH, 0u, 10u, 0u));
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_epoch_set(HOST_CH, 0u, 9u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_epoch_set(HOST_CH, 0u, 11u, 0u));

	/* The top of the range is reserved so a counter wrap has room. */
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_epoch_set(HOST_CH, 0u, 0xFFFFFFFFu, 0u));

	/* Reserved flag bits. */
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_epoch_set(HOST_CH, 0x02u, 12u, 0u));
	host_done();
}

ZTEST(api, test_sec_host_status_reports_enrolments)
{
	uint8_t st[ANTR_SEC_STATUS_LEN];

	host_assign(true, true);
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(HOST_CH, 128u, host_key));

	memset(st, 0xEE, sizeof(st));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_status_get(HOST_CH, st, sizeof(st)));
	zassert_equal(0u, rd16(&st[24]),
		      "a freshly keyed channel reports enrolments");

	/*
	 * Driven through the internal hook rather than a real pairing: this
	 * tests the ADAPTER's encoding of the counter, and test_profile_enrol.c
	 * already covers which pairings bump it. An enrolment the owner did not
	 * perform is the attack, so the counter has to reach the host at all.
	 */
	k_mutex_lock(&api_lock, K_FOREVER);
	radiant_sec_stat_enrolment(HOST_CH);
	radiant_sec_stat_enrolment(HOST_CH);
	k_mutex_unlock(&api_lock);

	memset(st, 0xEE, sizeof(st));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_status_get(HOST_CH, st, sizeof(st)));
	zassert_equal((uint8_t)ANTR_SEC_STATUS_VERSION, st[1]);
	zassert_equal(2u, rd16(&st[24]),
		      "enrolments is not little-endian at [24..25]");
	host_done();
}

ZTEST(api, test_sec_host_status_reports_what_was_configured)
{
	uint8_t st[ANTR_SEC_STATUS_LEN];

	host_assign(true, true);
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(HOST_CH, 128u, host_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_config(HOST_CH,
				      RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF,
				      4u, 0x02u, 0x1Fu));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_epoch_set(HOST_CH, 0u, 0x00ABCDEFu, 0u));

	memset(st, 0xEE, sizeof(st));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_status_get(HOST_CH, st, sizeof(st)));

	zassert_equal(HOST_CH, st[0]);
	zassert_equal(1u, st[1], "0xF4 is not structure version 1");
	zassert_equal(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF, st[2]);
	zassert_equal(4u, st[3]);
	zassert_equal(0x02u, st[4]);
	zassert_equal(0x1Fu, st[5]);
	zassert_equal(0x00ABCDEFu, rd32(&st[6]),
		      "the epoch is not little-endian at [6..9]");
	zassert_equal(0u, rd16(&st[12]), "nothing has verified yet");
	zassert_equal((uint8_t)RADIANT_SEC_VERDICT_CLEAR, st[23]);
	zassert_equal(0u, rd16(&st[24]), "nothing has enrolled");
	host_done();
}

ZTEST(api, test_sec_host_an_unkeyed_channel_reads_as_zeros_not_as_an_error)
{
	uint8_t st[ANTR_SEC_STATUS_LEN];

	/*
	 * "No security on this channel" is a state a host must be able to read.
	 * Making it an error would force a host to treat every ordinary channel
	 * as a fault, and a host that learns to ignore one error learns to
	 * ignore them all.
	 */
	memset(st, 0xEE, sizeof(st));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_status_get(HOST_CH, st, sizeof(st)));

	zassert_equal(HOST_CH, st[0]);
	/* The version describes the layout, not the channel, so it is set
	 * even here: a reader must never see version 0. */
	zassert_equal(1u, st[1], "0xF4 is not structure version 1");
	zassert_equal(0u, st[2], "an unkeyed channel reports switches on");
	zassert_equal((uint8_t)RADIANT_SEC_VERDICT_CLEAR, st[23]);
	zassert_equal(0u, rd16(&st[24]));
}

ZTEST(api, test_sec_host_a_short_status_buffer_is_refused)
{
	uint8_t st[ANTR_SEC_STATUS_LEN];

	/* Refused rather than truncated: a short reply on the wire would be
	 * parsed by field offset and would misreport every counter after the
	 * cut. */
	zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
		      antr_sec_status_get(HOST_CH, st,
					  ANTR_SEC_STATUS_LEN - 1u));
	zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
		      antr_sec_status_get(HOST_CH, NULL, sizeof(st)));
}

ZTEST(api, test_sec_host_a_channel_out_of_range_is_refused_everywhere)
{
	uint8_t st[ANTR_SEC_STATUS_LEN];
	uint8_t bad = (uint8_t)RADIANT_CHANNEL_COUNT;

	zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
		      antr_sec_config(bad, RADIANT_SEC_SW_AUTH, 2u, 0x01u,
				      0x0Fu));
	zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
		      antr_sec_key_set(bad, 128u, host_key));
	zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
		      antr_sec_epoch_set(bad, 0u, 1u, 0u));
	zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
		      antr_sec_status_get(bad, st, sizeof(st)));
}

#if defined(CONFIG_RADIANT_SEC_LOCK_CHECK)

/*
 * TODO security item 8: the 0xF1-0xF5 commands take api_lock. The check is
 * the port hook in ant_radio_radiant.c, counted rather than asserted here
 * (this image builds without CONFIG_ASSERT). Without the lock in
 * ant_sec_host.c every call below counts a violation.
 */
ZTEST(api, test_sec_host_commands_hold_api_lock)
{
	uint8_t  st[ANTR_SEC_STATUS_LEN];
	uint32_t base;

	host_assign(true, true);
	base = antr_api_lock_violations;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(HOST_CH, 128u, host_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_config(HOST_CH, RADIANT_SEC_SW_AUTH, 2u, 0x01u,
				      0x0Fu));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_epoch_set(HOST_CH, 0u, 1u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_status_get(HOST_CH, st, sizeof(st)));
	zassert_equal(base, antr_api_lock_violations,
		      "a 0xF1-0xF4 command reached radiant_sec without "
		      "api_lock");

	host_done();
}

/* The instrument itself: a call that really is unlocked must be counted, or
 * the test above passes on a hook that never fires. The counter is put back
 * so the suite's after-check does not see the deliberate violation. */
ZTEST(api, test_sec_host_the_lock_check_sees_an_unlocked_call)
{
	struct radiant_sec_state state;
	uint32_t                 base = antr_api_lock_violations;

	/* With CONFIG_ASSERT the unlocked call below halts the image, which is
	 * the check working and not something a test can survive: skip, and run
	 * this in the ASSERT-off scenario (the unit is the counter). */
	if (IS_ENABLED(CONFIG_ASSERT)) {
		ztest_test_skip();
	}

	zassert_false(antr_api_lock_held());
	radiant_sec_get_state(HOST_CH, &state);
	zassert_equal(base + 1u, antr_api_lock_violations,
		      "an unlocked radiant_sec call was not counted");

	k_mutex_lock(&api_lock, K_FOREVER);
	zassert_true(antr_api_lock_held());
	radiant_sec_get_state(HOST_CH, &state);
	k_mutex_unlock(&api_lock);
	zassert_equal(base + 1u, antr_api_lock_violations,
		      "a locked call was counted as a violation");

	antr_api_lock_violations = base;
}

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
/*
 * How long a pairing command holds api_lock. Each X25519 is a full scalar
 * multiplication on this build's software implementation; the number printed
 * is the figure to read against the RX thread (which needs api_lock for the
 * pump and the scheduler). Asserts only that the exchange works under the
 * lock and takes under a second.
 */
ZTEST(api, test_sec_host_pairing_holds_api_lock_and_its_hold_time)
{
	static const uint8_t scalar[RADIANT_SEC_X25519_BYTES] = {
		0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
		0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
		0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
		0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
	};
	uint8_t  reply[2 + RADIANT_SEC_X25519_BYTES];
	uint8_t  rlen = 0u;
	uint32_t base;
	uint32_t t0, t_scalar, t_exchange;

	host_assign(true, true);
	base = antr_api_lock_violations;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_pair(HOST_CH, ANTW_RADIANT_PAIR_ENTER, NULL, 0u,
				    reply, sizeof(reply), &rlen));
	t0 = k_cycle_get_32();
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_pair(HOST_CH, ANTW_RADIANT_PAIR_SCALAR, scalar,
				    sizeof(scalar), reply, sizeof(reply),
				    &rlen));
	t_scalar = k_cycle_get_32() - t0;
	zassert_equal(2u + RADIANT_SEC_X25519_BYTES, rlen);

	/* The peer's public key is the one just produced: a valid, non-
	 * degenerate point, so the exchange runs to the end. */
	uint8_t peer[RADIANT_SEC_X25519_BYTES];

	memcpy(peer, &reply[2], sizeof(peer));
	t0 = k_cycle_get_32();
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_pair(HOST_CH, ANTW_RADIANT_PAIR_EXCHANGE, peer,
				    sizeof(peer), reply, sizeof(reply), &rlen));
	t_exchange = k_cycle_get_32() - t0;

	zassert_equal(base, antr_api_lock_violations,
		      "a 0xF5 command reached radiant_sec without api_lock");

	uint32_t us_scalar = (uint32_t)(((uint64_t)t_scalar * 1000000u) /
					sys_clock_hw_cycles_per_sec());
	uint32_t us_exchange = (uint32_t)(((uint64_t)t_exchange * 1000000u) /
					  sys_clock_hw_cycles_per_sec());

	printk("X25519 api_lock hold: SCALAR %u us, EXCHANGE %u us\n",
	       us_scalar, us_exchange);
	zassert_true(us_scalar < 1000000u && us_exchange < 1000000u);

	(void)antr_sec_pair(HOST_CH, ANTW_RADIANT_PAIR_LEAVE, NULL, 0u, reply,
			    sizeof(reply), &rlen);
	host_done();
}
#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */

#endif /* CONFIG_RADIANT_SEC_LOCK_CHECK */

#endif /* CONFIG_RADIANT_SEC_HOST_MESSAGES */
