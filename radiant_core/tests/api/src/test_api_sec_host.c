/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The 0xF1-0xF4 adapter: antr_sec_config(), antr_sec_key_set(),
 * antr_sec_epoch_set() and antr_sec_status_get().
 *
 * These tests are about the ADAPTER, not about the transforms - radiant_sec's
 * own suite covers those. What is only testable here is the part the adapter
 * adds: the two ordering rules, the refusals, and the status encoding.
 *
 * THE ORDERING RULES ARE THE POINT.
 *
 * A channel must have its ID before its key, because the provisioning device
 * number is bound into the KDF and antr_sec_key_set() reads it from the channel
 * rather than taking it on the wire. It must have its period before its epoch,
 * because the period is what turns a phase into a packet index. Neither is
 * guessable and both fail silently if guessed - keys derived under device
 * number 0 verify against nothing, and a counter derived from the wrong period
 * drifts in a way that looks exactly like clock error. So both are refused with
 * ANTW_CHANNEL_IN_WRONG_STATE, and both refusals are asserted here.
 *
 * Compiled only into the radiant_core.api.sec_host scenario, which is the only
 * one that links the adapter.
 */

#include <zephyr/kernel.h>

/*
 * The whole file is behind the symbol, because CMakeLists.txt globs src/*.c
 * into every scenario this application defines and two of the three do not link
 * the adapter. A file that guarded only its ZTEST bodies would still fail at the
 * antr_sec_* declarations, which are themselves inside the same #ifdef in
 * src/ant_radio.h - deliberately, so the two halves appear and disappear
 * together.
 */
#if defined(CONFIG_RADIANT_SEC_HOST_MESSAGES)

#include <zephyr/ztest.h>
#include <string.h>

#include "ant_radio.h"
#include "ant_wire.h"

#include <radiant_core/radiant_channel.h>
#include <radiant_core/radiant_sec.h>

#define HOST_CH     0u
#define HOST_DEVNUM 0x3A17u
#define HOST_DEVTYPE 0x0Bu
#define HOST_TRANS  0x05u
#define HOST_RF     57u

#define STATUS_LEN 23u

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

ZTEST(api, test_sec_host_status_reports_what_was_configured)
{
	uint8_t st[STATUS_LEN];

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
	zassert_equal(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_CONF, st[1]);
	zassert_equal(4u, st[2]);
	zassert_equal(0x02u, st[3]);
	zassert_equal(0x1Fu, st[4]);
	zassert_equal(0x00ABCDEFu, rd32(&st[5]),
		      "the epoch is not little-endian at [5..8]");
	zassert_equal(0u, rd16(&st[11]), "nothing has verified yet");
	zassert_equal((uint8_t)RADIANT_SEC_VERDICT_CLEAR, st[22]);
	host_done();
}

ZTEST(api, test_sec_host_an_unkeyed_channel_reads_as_zeros_not_as_an_error)
{
	uint8_t st[STATUS_LEN];

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
	zassert_equal(0u, st[1], "an unkeyed channel reports switches on");
	zassert_equal((uint8_t)RADIANT_SEC_VERDICT_CLEAR, st[22]);
}

ZTEST(api, test_sec_host_a_short_status_buffer_is_refused)
{
	uint8_t st[STATUS_LEN];

	/* Refused rather than truncated: a short reply on the wire would be
	 * parsed by field offset and would misreport every counter after the
	 * cut. */
	zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
		      antr_sec_status_get(HOST_CH, st, STATUS_LEN - 1u));
	zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
		      antr_sec_status_get(HOST_CH, NULL, sizeof(st)));
}

ZTEST(api, test_sec_host_a_channel_out_of_range_is_refused_everywhere)
{
	uint8_t st[STATUS_LEN];
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

#endif /* CONFIG_RADIANT_SEC_HOST_MESSAGES */
