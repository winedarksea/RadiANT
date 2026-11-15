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

#include <radiant/ant_radio.h>
#include <radiant/ant_wire.h>
#include "ant_radio_radiant_priv.h"

#include <radiant/radiant_channel.h>
#include <radiant/radiant_event.h>
#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_sec.h>

#include "fake_radio.h"
#if defined(CONFIG_IRQ_OFFLOAD)
#include <zephyr/irq_offload.h>
#endif

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
	for (bit = 0x20u; bit != 0u; bit = (uint8_t)(bit << 1)) {
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

	/* Keyed, no epoch: pending, and the host can see it (item 5). */
	memset(st, 0xEE, sizeof(st));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_status_get(HOST_CH, st, sizeof(st)));
	zassert_equal(ANTR_SEC_STATUS_KEYED, st[30], "%u", st[30]);

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_epoch_set(HOST_CH, 0u, 0x00ABCDEFu, 0u));

	memset(st, 0xEE, sizeof(st));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_status_get(HOST_CH, st, sizeof(st)));

	zassert_equal(ANTR_SEC_STATUS_KEYED | ANTR_SEC_STATUS_ARMED, st[30],
		      "%u", st[30]);
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
	zassert_equal(0u, st[30], "%u", st[30]);
}

/*
 * Items 5 and 9 through the host surface: a 0xF2 re-sending the installed
 * root changes nothing (still armed, same epoch, 0xF3 still monotone); a
 * different root makes the channel pending until the next 0xF3, which may
 * then go below the old epoch.
 */
ZTEST(api, test_sec_host_a_resent_key_keeps_the_channel_armed)
{
	static const uint8_t other_key[16] = {
		0x0f, 0x1e, 0x2d, 0x3c, 0x4b, 0x5a, 0x69, 0x78,
		0x87, 0x96, 0xa5, 0xb4, 0xc3, 0xd2, 0xe1, 0xf0,
	};
	uint8_t st[ANTR_SEC_STATUS_LEN];

	host_assign(true, true);
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(HOST_CH, 128u, host_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_config(HOST_CH, RADIANT_SEC_SW_AUTH, 2u, 0x01u,
				      0x0Fu));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_epoch_set(HOST_CH, 0u, 500u, 0u));

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(HOST_CH, 128u, host_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_status_get(HOST_CH, st, sizeof(st)));
	zassert_equal(ANTR_SEC_STATUS_KEYED | ANTR_SEC_STATUS_ARMED, st[30]);
	zassert_equal(500u, rd32(&st[6]));
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_epoch_set(HOST_CH, 0u, 499u, 0u));

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(HOST_CH, 128u, other_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_status_get(HOST_CH, st, sizeof(st)));
	zassert_equal(ANTR_SEC_STATUS_KEYED, st[30], "%u", st[30]);
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_epoch_set(HOST_CH, 0u, 9u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_status_get(HOST_CH, st, sizeof(st)));
	zassert_equal(ANTR_SEC_STATUS_KEYED | ANTR_SEC_STATUS_ARMED, st[30]);
	host_done();
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

/* ── The 0xF8 window-verdict event (security review 2026-11-13, item 3) ── */

/* test_api.c's record of everything antr_on_message() was handed. */
uint32_t       api_msglog_count(void);
const uint8_t *api_msglog_get(uint32_t i, uint8_t *id, uint8_t *len);

#define VD_RX_CH   0u  /* the receiver, a real open slave channel */
#define VD_TX_CH   1u  /* a sender: a radiant_sec context and nothing else */
#define VD_RX2_CH  2u  /* a second pair, for the interleaving test */
#define VD_TX2_CH  3u
#define VD_EPOCH   0x00001234u
#define VD_PAGE    0x05u
#define VD_HOPS    6u  /* three windows at W=2: two judged, one waiting */
#define VD_LOG_MAX 64u /* test_api.c's MSGLOG_MAX */
#define VD_F8_LEN  7u

static uint64_t vd_anchor;

static uint64_t vd_slot_t(uint32_t i)
{
	return vd_anchor +
	       (uint64_t)i * (uint64_t)radiant_channel_counts_to_us(
				     RADIANT_CHANNEL_PERIOD_ANT_PLUS);
}

/*
 * A receiver configured through the host surface (0xF2, 0xF1, 0xF3), and a
 * sender keyed the same way directly in radiant_sec. The sender carries only
 * the transform bits: VERDICT_EVENT and DROP_UNVER are receiver policy. Every
 * epoch anchors at the same radiant_radio_now() - nothing moves the fake
 * clock between them - so each sender's slot counter is its receiver's.
 */
static void vd_setup_pair_ext(uint8_t rx, uint8_t tx, uint16_t devnum,
			      uint8_t switches, uint8_t w, uint8_t ext_assign)
{
	int rc[4];

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_network_address_set(0u, host_net_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(
			      rx, (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY, 0u,
			      ext_assign));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(rx, devnum, HOST_DEVTYPE,
					  HOST_TRANS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(rx,
					      RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(rx, HOST_RF));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_open(rx));

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(rx, 128u, host_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_config(rx, switches, w, 0x01u, 0x0Fu));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_epoch_set(rx, 0u, VD_EPOCH, 0u));

	k_mutex_lock(&api_lock, K_FOREVER);
	rc[0] = radiant_sec_set_key(tx, host_key, 128, devnum);
	rc[1] = radiant_sec_set_devnum(tx, devnum);
	rc[2] = radiant_sec_configure(
		tx,
		(uint8_t)(switches & (RADIANT_SEC_SW_CONF | RADIANT_SEC_SW_AUTH)),
		w, 0x01u, 0x0Fu);
	rc[3] = radiant_sec_set_epoch(tx, VD_EPOCH, 0u,
				      RADIANT_CHANNEL_PERIOD_ANT_PLUS);
	vd_anchor = (uint64_t)radiant_radio_now();
	k_mutex_unlock(&api_lock);
	for (int i = 0; i < 4; i++) {
		zassert_equal(RADIANT_SEC_OK, rc[i], "sender setup step %d", i);
	}
	(void)radiant_event_drain(0u);
}

static void vd_setup_pair(uint8_t rx, uint8_t tx, uint16_t devnum,
			  uint8_t switches, uint8_t w)
{
	vd_setup_pair_ext(rx, tx, devnum, switches, w, 0u);
}

static void vd_setup(uint8_t switches, uint8_t w)
{
	vd_setup_pair(VD_RX_CH, VD_TX_CH, HOST_DEVNUM, switches, w);
}

static void vd_done_pair(uint8_t rx, uint8_t tx)
{
	(void)radiant_event_drain(0u);
	(void)antr_channel_close(rx);
	(void)antr_channel_unassign(rx);
	k_mutex_lock(&api_lock, K_FOREVER);
	radiant_sec_channel_release(tx);
	k_mutex_unlock(&api_lock);
	(void)radiant_event_drain(0u);
}

static void vd_done(void)
{
	vd_done_pair(VD_RX_CH, VD_TX_CH);
}

/* Hand `air` to receiver `rx` as heard at slot i - the call the radio ISR
 * makes - and pump as the event thread does. Returns the ingest verdict. */
static int vd_ingest(uint8_t rx, const uint8_t *air, uint32_t i)
{
	int rc;

	k_mutex_lock(&api_lock, K_FOREVER);
	rc = radiant_sec_rx_ingest(rx, air, 8u, true, vd_slot_t(i));
	radiant_sec_pump();
	k_mutex_unlock(&api_lock);
	return rc;
}

/* How many BROADCAST_DATA for `rx` arrived from log index `from` on. */
static uint32_t vd_count_bd(uint32_t from, uint8_t rx)
{
	uint32_t n = 0u;

	for (uint32_t k = from; k < api_msglog_count(); k++) {
		const uint8_t *d;
		uint8_t        id;
		uint8_t        len;

		d = api_msglog_get(k, &id, &len);
		if (id == (uint8_t)ANTW_MESG_BROADCAST_DATA_ID && d[0] == rx) {
			n++;
		}
	}
	return n;
}

/*
 * Packet i of the pair: built by the sender for slot i, optionally with one
 * data bit flipped on the way (an injector), ingested, pumped and - when
 * `drain` - drained. Returns how many BROADCAST_DATA that drain produced, so
 * a test can show the data still arrives on its own packet.
 */
static uint32_t vd_hop_pair(uint8_t rx, uint8_t tx, uint32_t i, bool flip,
			    bool drain)
{
	uint8_t  air[8] = { VD_PAGE, 0xAAu, (uint8_t)i, 0x11u,
			    0x22u,   0x33u, 0x44u,      0x55u };
	uint32_t before = api_msglog_count();
	int      rc_tx;
	int      rc_rx;

	k_mutex_lock(&api_lock, K_FOREVER);
	rc_tx = radiant_sec_tx_transform(tx, air, 8u, vd_slot_t(i));
	k_mutex_unlock(&api_lock);
	if (flip) {
		air[4] ^= 0x01u;
	}
	rc_rx = vd_ingest(rx, air, i);
	zassert_equal(RADIANT_SEC_OK, rc_tx, "the sender refused slot %u", i);
	zassert_equal(RADIANT_SEC_RX_DEFER, rc_rx,
		      "the receiver did not take secured packet %u", i);
	if (!drain) {
		return 0u;
	}
	(void)radiant_event_drain(0u);
	return vd_count_bd(before, rx);
}

static uint32_t vd_hop(uint32_t i, bool flip)
{
	return vd_hop_pair(VD_RX_CH, VD_TX_CH, i, flip, true);
}

/* A forged secured-range page claiming counter `ctr`, heard at slot i. */
static void vd_forge(uint32_t i, uint8_t ctr)
{
	const uint8_t air[8] = { VD_PAGE, ctr,   0xEEu, 0xEEu,
				 0xEEu,   0xEEu, 0xEEu, 0xEEu };

	zassert_equal(RADIANT_SEC_RX_DEFER, vd_ingest(VD_RX_CH, air, i),
		      "the forged page was not taken");
	(void)radiant_event_drain(0u);
}

/* One expected message: a BROADCAST_DATA whose byte [1] is `ctr`'s low byte,
 * or (verdict != 0) a 0xF8 for the window starting at `ctr`, whose seq is
 * `gap` more than one past the previous verdict's - the seq values the
 * firmware spent on window discards in between. */
struct vd_want {
	uint8_t  verdict;
	uint16_t ctr;
	uint8_t  gap;
};

static uint16_t vd_seq(const uint8_t *d)
{
	return rd16(&d[5]);
}

/*
 * The data and 0xF8 messages for channel `rx` from log index `from` on must
 * be exactly `want`. The first verdict's seq is the baseline (the context's
 * own discards have spent some already); each later one must follow it as
 * `gap` says.
 */
static void vd_expect_ch(uint32_t from, uint8_t rx, const struct vd_want *want,
			 uint32_t n, uint8_t w)
{
	uint32_t got = 0u;
	bool     have_seq = false;
	uint16_t seq = 0u;

	zassert_true(api_msglog_count() < VD_LOG_MAX,
		     "the message log overflowed");
	for (uint32_t k = from; k < api_msglog_count(); k++) {
		const uint8_t *d;
		uint8_t        id;
		uint8_t        len;

		d = api_msglog_get(k, &id, &len);
		if ((id != (uint8_t)ANTW_MESG_BROADCAST_DATA_ID &&
		     id != (uint8_t)ANTW_MESG_RADIANT_SEC_VERDICT_ID) ||
		    d[0] != rx) {
			continue;
		}
		zassert_true(got < n, "message %u: more than the %u expected",
			     k, n);
		if (want[got].verdict == 0u) {
			zassert_equal((uint8_t)ANTW_MESG_BROADCAST_DATA_ID, id,
				      "message %u: expected data for counter "
				      "%u, got 0x%02X", got, want[got].ctr, id);
			zassert_true(len >= 9u);
			/* Payload byte [1], the counter's low byte: what a host
			 * maps a packet to its window by. */
			zassert_equal((uint8_t)want[got].ctr, d[2],
				      "message %u: data for the wrong counter",
				      got);
		} else {
			zassert_equal((uint8_t)ANTW_MESG_RADIANT_SEC_VERDICT_ID,
				      id,
				      "message %u: expected the verdict on "
				      "window %u, got 0x%02X", got,
				      want[got].ctr, id);
			zassert_equal(VD_F8_LEN, len, "0xF8 is six bytes");
			zassert_equal(want[got].verdict, d[1],
				      "message %u (window %u): wrong verdict",
				      got, want[got].ctr);
			zassert_equal(want[got].ctr, rd16(&d[2]),
				      "window_start is not u16 LE at [2..3]");
			zassert_equal(w, d[4], "W at [4]");
			if (have_seq) {
				seq = (uint16_t)(seq + want[got].gap);
				zassert_equal(seq, vd_seq(d),
					      "message %u: seq %u, want %u", got,
					      vd_seq(d), seq);
			}
			seq = (uint16_t)(vd_seq(d) + 1u);
			have_seq = true;
		}
		got++;
	}
	zassert_equal(n, got, "%u of %u expected messages arrived", got, n);
}

static void vd_expect(uint32_t from, const struct vd_want *want, uint32_t n,
		      uint8_t w)
{
	vd_expect_ch(from, VD_RX_CH, want, n, w);
}

#define V_OK  ((uint8_t)RADIANT_SEC_VERDICT_VERIFIED)
#define V_BAD ((uint8_t)RADIANT_SEC_VERDICT_UNVERIFIED)

ZTEST(api, test_sec_host_a_good_window_sends_verified_after_its_packets)
{
	/*
	 * Each packet still arrives on its own hop - the event adds a message,
	 * it does not delay one. Window 0's verdict exists once window 1
	 * (counters 2,3) has carried its tag, so it follows counter 3; window
	 * 2's follows counter 5; window 4's tag is never sent here.
	 */
	static const struct vd_want want[] = {
		{ 0u, 0u }, { 0u, 1u }, { 0u, 2u }, { 0u, 3u }, { V_OK, 0u },
		{ 0u, 4u }, { 0u, 5u }, { V_OK, 2u },
	};
	uint32_t from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	for (uint32_t i = 0u; i < VD_HOPS; i++) {
		zassert_equal(1u, vd_hop(i, false),
			      "packet %u was not delivered on its own hop", i);
	}
	vd_expect(from, want, ARRAY_SIZE(want), 2u);
	vd_done();
}

ZTEST(api, test_sec_host_a_flipped_bit_sends_unverified)
{
	/*
	 * One data bit of counter 1 flipped in the air: the forged packet is
	 * still delivered (the default policy), and the verdict that follows
	 * says so. Window 2 is genuine and verifies on its own.
	 */
	static const struct vd_want want[] = {
		{ 0u, 0u }, { 0u, 1u }, { 0u, 2u }, { 0u, 3u }, { V_BAD, 0u },
		{ 0u, 4u }, { 0u, 5u }, { V_OK, 2u },
	};
	uint32_t from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	for (uint32_t i = 0u; i < VD_HOPS; i++) {
		(void)vd_hop(i, i == 1u);
	}
	vd_expect(from, want, ARRAY_SIZE(want), 2u);
	vd_done();
}

/*
 * Item 9's no-op rule reaches the on-air device number too. A Tier 2
 * receiver has moved to a trial number (radiant_sec_try_devnum()) and is
 * verifying windows under it; a 0xF2 re-sending the same root must leave that
 * number alone. Until 2026-11-14 the adapter put it back to the provisioning
 * number after every 0xF2, so every window after the re-send failed to verify.
 * A different root still starts from the provisioning number (radiant_sec's
 * own suite).
 */
#define VD_TRIAL_DEVNUM 0x51C4u

ZTEST(api, test_sec_host_a_resent_key_keeps_a_tier2_trial_devnum)
{
	static const struct vd_want want[] = {
		{ 0u, 0u }, { 0u, 1u }, { 0u, 2u }, { 0u, 3u }, { V_OK, 0u },
		{ 0u, 4u }, { 0u, 5u }, { V_OK, 2u },
		{ 0u, 6u }, { 0u, 7u }, { V_OK, 4u },
	};
	uint32_t from;
	int      rc[2];

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	k_mutex_lock(&api_lock, K_FOREVER);
	rc[0] = radiant_sec_set_devnum(VD_TX_CH, VD_TRIAL_DEVNUM);
	rc[1] = radiant_sec_try_devnum(VD_RX_CH, VD_TRIAL_DEVNUM);
	k_mutex_unlock(&api_lock);
	zassert_equal(RADIANT_SEC_OK, rc[0]);
	zassert_equal(RADIANT_SEC_OK, rc[1]);

	from = api_msglog_count();
	for (uint32_t i = 0u; i < 4u; i++) {
		(void)vd_hop(i, false);
	}
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(VD_RX_CH, 128u, host_key));
	for (uint32_t i = 4u; i < 8u; i++) {
		(void)vd_hop(i, false);
	}
	vd_expect(from, want, ARRAY_SIZE(want), 2u);
	vd_done();
}

ZTEST(api, test_sec_host_the_drop_policy_releases_a_window_before_its_verdict)
{
	/* Held packets come out immediately before the event that releases
	 * them; a forged window's never come out, only its verdict. Counter 3
	 * is flipped, so window 2 fails and window 0 (whose tag bytes counter
	 * 3 still carries intact) verifies. */
	static const struct vd_want want[] = {
		{ 0u, 0u }, { 0u, 1u }, { V_OK, 0u },
		{ V_BAD, 2u },
	};
	uint32_t from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER |
			 RADIANT_SEC_SW_VERDICT_EVENT,
		 2u);
	from = api_msglog_count();
	for (uint32_t i = 0u; i < VD_HOPS; i++) {
		(void)vd_hop(i, i == 3u);
	}
	vd_expect(from, want, ARRAY_SIZE(want), 2u);
	vd_done();
}

ZTEST(api, test_sec_host_a_future_counter_forgery_is_judged_before_the_genuine)
{
	/*
	 * Review 2026-11-13, finding 1. The replay check lets a counter up to
	 * ~127 AHEAD through, so a forged page claiming counter 10 is
	 * delivered at slot 2. It opens window 10, which wrecks window 0's
	 * tag (UNV 0) and is itself judged UNV 10 - and window 10 later comes
	 * round for real and verifies (VER 10). The forgery's verdict is the
	 * FIRST covering one after it; a host that took any matching verdict
	 * would credit it with the genuine VER 10. Seq counts every verdict,
	 * so losing UNV 10 shows as a gap before VER 10.
	 */
	static const struct vd_want want[] = {
		{ 0u, 0u },  { 0u, 1u },  { 0u, 10u }, /* the forgery */
		{ V_BAD, 0u },
		{ 0u, 2u },  { 0u, 3u },  { V_BAD, 10u },
		{ 0u, 4u },  { 0u, 5u },  { V_OK, 2u },
		{ 0u, 6u },  { 0u, 7u },  { V_OK, 4u },
		{ 0u, 8u },  { 0u, 9u },  { V_OK, 6u },
		{ 0u, 10u }, { 0u, 11u }, { V_OK, 8u },
		{ 0u, 12u }, { 0u, 13u }, { V_OK, 10u },
	};
	uint32_t from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	(void)vd_hop(0u, false);
	(void)vd_hop(1u, false);
	vd_forge(2u, 10u);
	for (uint32_t i = 2u; i < 14u; i++) {
		(void)vd_hop(i, false);
	}
	vd_expect(from, want, ARRAY_SIZE(want), 2u);
	vd_done();
}

ZTEST(api, test_sec_host_a_late_reopen_repeats_a_window_start)
{
	/*
	 * A forged page claiming counter 0 after window 0 closed, but before
	 * its verdict, reopens window 0: the genuine window and the reopened
	 * one are each judged, so UNV 0 arrives twice. window_start values
	 * repeat; only the first covering verdict after a packet is its own.
	 */
	static const struct vd_want want[] = {
		{ 0u, 0u }, { 0u, 1u }, { 0u, 0u }, /* the forgery */
		{ V_BAD, 0u },
		{ 0u, 2u }, { 0u, 3u }, { V_BAD, 0u },
		{ 0u, 4u }, { 0u, 5u }, { V_OK, 2u },
	};
	uint32_t from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	(void)vd_hop(0u, false);
	(void)vd_hop(1u, false);
	vd_forge(2u, 0u);
	for (uint32_t i = 2u; i < VD_HOPS; i++) {
		(void)vd_hop(i, false);
	}
	vd_expect(from, want, ARRAY_SIZE(want), 2u);
	vd_done();
}

ZTEST(api, test_sec_host_a_clear_page_out_of_range_is_never_judged)
{
	/*
	 * Page 0x50 is outside the secured range, so even on an opted-in
	 * X_AUTH channel it is RX_PLAIN: delivered by the ordinary path in
	 * the clear, never absorbed, never followed by a 0xF8 of its own.
	 * Its byte [1] = 0 would match window 0's VERIFIED, which is why a
	 * host covers only pages inside 0xF4 [4..5]. And it costs the secured
	 * stream nothing: the events are exactly a clean run's.
	 */
	static const uint8_t clear[8] = { 0x50u, 0x00u, 1u, 2u, 3u, 4u, 5u,
					  6u };
	static const struct vd_want want[] = {
		{ 0u, 0u }, { 0u, 1u }, { 0u, 2u }, { 0u, 3u }, { V_OK, 0u },
		{ 0u, 4u }, { 0u, 5u }, { V_OK, 2u },
	};
	uint32_t from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	(void)vd_hop(0u, false);
	(void)vd_hop(1u, false);
	zassert_equal(RADIANT_SEC_RX_PLAIN, vd_ingest(VD_RX_CH, clear, 2u),
		      "a page outside the secured range was taken");
	(void)radiant_event_drain(0u);
	for (uint32_t i = 2u; i < VD_HOPS; i++) {
		(void)vd_hop(i, false);
	}
	vd_expect(from, want, ARRAY_SIZE(want), 2u);
	vd_done();
}

/* The log index of channel `rx`'s CHANNEL_CLOSED event, or -1. */
static int vd_closed_at(uint32_t from, uint8_t rx)
{
	for (uint32_t k = from; k < api_msglog_count(); k++) {
		const uint8_t *d;
		uint8_t        id;
		uint8_t        len;

		d = api_msglog_get(k, &id, &len);
		if (id == (uint8_t)ANTW_MESG_RESPONSE_EVENT_ID && len >= 3u &&
		    d[0] == rx && d[1] == (uint8_t)ANTW_MESG_EVENT_ID &&
		    d[2] == (uint8_t)ANTW_EVENT_CHANNEL_CLOSED) {
			return (int)k;
		}
	}
	return -1;
}

ZTEST(api, test_sec_host_nothing_follows_channel_closed)
{
	/*
	 * Window 2's packets were delivered, then the host closed the channel.
	 * The packet that would have produced VER 2 is still ingested (the
	 * context outlives the close), but neither it nor the verdict may
	 * follow CHANNEL_CLOSED: the host flushed its pending packets as
	 * unverified there.
	 */
	uint32_t from;
	int      i_closed;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	for (uint32_t i = 0u; i < 5u; i++) {
		(void)vd_hop(i, false);
	}
	zassert_ok(antr_channel_close(VD_RX_CH));
	fake_radio_advance(1000u);
	(void)radiant_event_drain(0u);
	i_closed = vd_closed_at(from, VD_RX_CH);
	zassert_true(i_closed >= 0, "the close never completed");

	(void)vd_hop_pair(VD_RX_CH, VD_TX_CH, 5u, false, true);
	for (uint32_t k = (uint32_t)i_closed; k < api_msglog_count(); k++) {
		uint8_t id;
		uint8_t len;

		(void)api_msglog_get(k, &id, &len);
		zassert_not_equal((uint8_t)ANTW_MESG_BROADCAST_DATA_ID, id,
				  "data after CHANNEL_CLOSED");
		zassert_not_equal((uint8_t)ANTW_MESG_RADIANT_SEC_VERDICT_ID, id,
				  "a verdict after CHANNEL_CLOSED");
	}
	vd_done();
}

ZTEST(api, test_sec_host_two_channels_keep_their_own_verdicts)
{
	/* Interleaved packet by packet, with one forged bit on the second
	 * channel only: each channel's events, and seq, are its own. */
	static const struct vd_want good[] = {
		{ 0u, 0u }, { 0u, 1u }, { 0u, 2u }, { 0u, 3u }, { V_OK, 0u },
		{ 0u, 4u }, { 0u, 5u }, { V_OK, 2u },
	};
	static const struct vd_want bad[] = {
		{ 0u, 0u }, { 0u, 1u }, { 0u, 2u }, { 0u, 3u }, { V_BAD, 0u },
		{ 0u, 4u }, { 0u, 5u }, { V_OK, 2u },
	};
	uint32_t from;

	vd_setup_pair(VD_RX_CH, VD_TX_CH, HOST_DEVNUM,
		      RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	vd_setup_pair(VD_RX2_CH, VD_TX2_CH, (uint16_t)(HOST_DEVNUM + 1u),
		      RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	for (uint32_t i = 0u; i < VD_HOPS; i++) {
		(void)vd_hop_pair(VD_RX_CH, VD_TX_CH, i, false, true);
		(void)vd_hop_pair(VD_RX2_CH, VD_TX2_CH, i, i == 1u, true);
	}
	vd_expect_ch(from, VD_RX_CH, good, ARRAY_SIZE(good), 2u);
	vd_expect_ch(from, VD_RX2_CH, bad, ARRAY_SIZE(bad), 2u);
	vd_done_pair(VD_RX2_CH, VD_TX2_CH);
	vd_done();
}

ZTEST(api, test_sec_host_a_lost_verdict_is_counted_and_leaves_a_seq_gap)
{
	/*
	 * Nobody drains while 40 packets arrive, so the event ring's data
	 * slots fill and later posts fail. Every 0xF8 that could not be queued
	 * is counted in verdict_events_dropped, and the host sees exactly that
	 * many missing seq values - which, by the host rule, settles whatever
	 * it still held as unverified.
	 */
	struct radiant_sec_stats st;
	uint32_t                 from;
	uint32_t                 missing = 0u;
	uint32_t                 n_f8 = 0u;
	int                      prev = -1;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	for (uint32_t i = 0u; i < 40u; i++) {
		(void)vd_hop_pair(VD_RX_CH, VD_TX_CH, i, false, false);
	}
	(void)radiant_event_drain(0u);
	for (uint32_t i = 40u; i < 44u; i++) {
		(void)vd_hop(i, false);
	}

	zassert_true(api_msglog_count() < VD_LOG_MAX,
		     "the message log overflowed");
	for (uint32_t k = from; k < api_msglog_count(); k++) {
		const uint8_t *d;
		uint8_t        id;
		uint8_t        len;

		d = api_msglog_get(k, &id, &len);
		if (id != (uint8_t)ANTW_MESG_RADIANT_SEC_VERDICT_ID) {
			continue;
		}
		zassert_equal(VD_F8_LEN, len);
		if (prev >= 0) {
			missing += (uint16_t)(vd_seq(d) - (uint16_t)prev - 1u);
		}
		prev = (int)vd_seq(d);
		n_f8++;
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	radiant_sec_get_stats(VD_RX_CH, &st);
	k_mutex_unlock(&api_lock);
	zassert_true(n_f8 >= 2u, "setup: too few verdicts arrived to compare");
	zassert_true(st.verdict_events_dropped > 0u,
		     "setup: the ring never filled, so nothing was lost");
	zassert_equal(st.verdict_events_dropped, missing,
		      "%u verdicts dropped but %u seq values missing",
		      st.verdict_events_dropped, missing);

	/* And the host can read the same count at 0xF4 [28..29]. */
	{
		uint8_t f4[ANTR_SEC_STATUS_LEN];

		zassert_equal(31u, ANTR_SEC_STATUS_LEN);
		zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
			      antr_sec_status_get(VD_RX_CH, f4, sizeof(f4)));
		zassert_equal(1u, f4[1], "0xF4 stays structure version 1");
		zassert_equal(st.verdict_events_dropped, rd16(&f4[28]),
			      "verdict_events_dropped is not u16 LE at "
			      "[28..29]");
	}
	vd_done();
}

/* A forged secured-range page claiming counter `ctr` with tag byte `tag`,
 * heard at slot i, pumped and drained. */
static void vd_forge_tag(uint32_t i, uint16_t ctr, uint8_t tag)
{
	const uint8_t air[8] = { VD_PAGE, (uint8_t)ctr, 0xEEu, 0xEEu,
				 0xEEu,   0xEEu,        0xEEu, tag };

	zassert_equal(RADIANT_SEC_RX_DEFER, vd_ingest(VD_RX_CH, air, i),
		      "the forged page was not taken");
	(void)radiant_event_drain(0u);
}

/* What the sender put on air in slot i, without delivering it: an attacker's
 * recording. */
static void vd_capture(uint32_t i, uint8_t air[8])
{
	int rc;

	memset(air, 0, 8u);
	air[0] = VD_PAGE;
	air[2] = (uint8_t)i;
	k_mutex_lock(&api_lock, K_FOREVER);
	rc = radiant_sec_tx_transform(VD_TX_CH, air, 8u, vd_slot_t(i));
	k_mutex_unlock(&api_lock);
	zassert_equal(RADIANT_SEC_OK, rc, "the sender refused slot %u", i);
}

/*
 * Window 2 is genuine and waiting for its verdict when the channel goes
 * quiet. 256 counters later (counters 258/259 share byte [1] with 2/3) the
 * forger sends two pages carrying the tag bytes it recorded from genuine
 * window 4 - which are exactly window 2's tag. The tag matches, but window
 * 258 is not the window after window 2, so the verdict on 2 is UNVERIFIED:
 * before the fix it was VERIFIED, right behind the forged pages, with no gap.
 */
ZTEST(api, test_sec_host_a_window_aliased_256_on_is_not_verified)
{
	static const struct vd_want want[] = {
		{ 0u, 0u },   { 0u, 1u },   { 0u, 2u }, { 0u, 3u }, { V_OK, 0u },
		{ 0u, 258u }, { 0u, 259u }, /* the forgeries */
		{ V_BAD, 2u },
	};
	uint8_t  rec4[8];
	uint8_t  rec5[8];
	uint32_t from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	for (uint32_t i = 0u; i < 4u; i++) {
		(void)vd_hop(i, false);
	}
	vd_capture(4u, rec4);
	vd_capture(5u, rec5);
	vd_forge_tag(258u, 258u, rec4[7]);
	vd_forge_tag(259u, 259u, rec5[7]);
	vd_expect(from, want, ARRAY_SIZE(want), 2u);
	vd_done();
}

/*
 * The same forgery after a host close and reopen instead of a quiet gap. The
 * close and the open each discard the receive windows (and spend a seq), so
 * window 2 is never judged at all; the forged window is judged UNVERIFIED by
 * the genuine window after it, which then verifies on its own.
 */
ZTEST(api, test_sec_host_a_close_and_reopen_leaves_no_window_pending)
{
	static const struct vd_want want[] = {
		{ 0u, 0u },   { 0u, 1u },   { 0u, 2u }, { 0u, 3u }, { V_OK, 0u },
		{ 0u, 258u }, { 0u, 259u }, /* the forgeries */
		{ 0u, 260u }, { 0u, 261u }, { V_BAD, 258u, 2u },
		{ 0u, 262u }, { 0u, 263u }, { V_OK, 260u },
	};
	uint8_t  rec4[8];
	uint8_t  rec5[8];
	uint32_t from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	for (uint32_t i = 0u; i < 4u; i++) {
		(void)vd_hop(i, false);
	}
	vd_capture(4u, rec4);
	vd_capture(5u, rec5);
	zassert_ok(antr_channel_close(VD_RX_CH));
	fake_radio_advance(1000u);
	(void)radiant_event_drain(0u);
	zassert_true(vd_closed_at(from, VD_RX_CH) >= 0,
		     "the close never completed");
	zassert_ok(antr_channel_open(VD_RX_CH));
	(void)radiant_event_drain(0u);

	vd_forge_tag(258u, 258u, rec4[7]);
	vd_forge_tag(259u, 259u, rec5[7]);
	for (uint32_t i = 260u; i < 264u; i++) {
		(void)vd_hop(i, false);
	}
	vd_expect(from, want, ARRAY_SIZE(want), 2u);
	vd_done();
}

/*
 * Review finding 2: a forged page already pumped when the host sends 0xF3
 * (a new epoch) or 0xF1 loses its window there, so its UNVERIFIED never
 * comes - and with the old firmware the next verdicts followed in an
 * unbroken seq, so a host that took the first one after its flush as
 * binding would credit the forgery with VERIFIED 6. The discard now spends
 * a seq value: the first verdict after shows a gap of one.
 */
static void vd_stale_across(bool new_epoch)
{
	static const struct vd_want want[] = {
		{ 0u, 0u }, { 0u, 1u }, { 0u, 2u }, { 0u, 3u }, { V_OK, 0u },
		{ 0u, 6u }, /* the forgery, pumped before the host's command */
		{ 0u, 4u }, { 0u, 5u }, { 0u, 6u }, { 0u, 7u }, { V_OK, 4u, 1u },
		{ 0u, 8u }, { 0u, 9u }, { V_OK, 6u },
	};
	uint32_t from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	for (uint32_t i = 0u; i < 4u; i++) {
		(void)vd_hop(i, false);
	}
	vd_forge_tag(4u, 6u, 0xEEu);

	if (new_epoch) {
		int rc;

		zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
			      antr_sec_epoch_set(VD_RX_CH, 0u, VD_EPOCH + 1u,
						 0u));
		k_mutex_lock(&api_lock, K_FOREVER);
		rc = radiant_sec_set_epoch(VD_TX_CH, VD_EPOCH + 1u, 0u,
					   RADIANT_CHANNEL_PERIOD_ANT_PLUS);
		k_mutex_unlock(&api_lock);
		zassert_equal(RADIANT_SEC_OK, rc);
	} else {
		zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
			      antr_sec_config(VD_RX_CH,
					      RADIANT_SEC_SW_AUTH |
						      RADIANT_SEC_SW_VERDICT_EVENT,
					      2u, 0x01u, 0x0Fu));
	}

	for (uint32_t i = 4u; i < 10u; i++) {
		(void)vd_hop(i, false);
	}
	vd_expect(from, want, ARRAY_SIZE(want), 2u);
	vd_done();
}

ZTEST(api, test_sec_host_a_stale_forgery_across_0xf3_leaves_a_seq_gap)
{
	vd_stale_across(true);
}

ZTEST(api, test_sec_host_a_stale_forgery_across_0xf1_leaves_a_seq_gap)
{
	vd_stale_across(false);
}

ZTEST(api, test_sec_host_a_tag_riding_past_clear_windows_still_verifies)
{
	/*
	 * Slots 2..5 carry only clear pages (two of them heard, page 0x50,
	 * RX_PLAIN), so windows 2 and 4 carry no secured packet and window 0's
	 * tag rides on to window 6. Its VERIFIED comes after counter 7 - later
	 * than 2W-1 periods, which a host's deadline settles as unverified
	 * first (fail-safe) - and nothing but window 0's own packets is
	 * covered by it.
	 */
	static const uint8_t clear[8] = { 0x50u, 0x00u, 1u, 2u, 3u, 4u, 5u,
					  6u };
	static const struct vd_want want[] = {
		{ 0u, 0u }, { 0u, 1u },
		{ 0u, 6u }, { 0u, 7u }, { V_OK, 0u },
		{ 0u, 8u }, { 0u, 9u }, { V_OK, 6u },
	};
	uint32_t from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	(void)vd_hop(0u, false);
	(void)vd_hop(1u, false);
	zassert_equal(RADIANT_SEC_RX_PLAIN, vd_ingest(VD_RX_CH, clear, 2u));
	zassert_equal(RADIANT_SEC_RX_PLAIN, vd_ingest(VD_RX_CH, clear, 4u));
	for (uint32_t i = 6u; i < 10u; i++) {
		(void)vd_hop(i, false);
	}
	vd_expect(from, want, ARRAY_SIZE(want), 2u);
	vd_done();
}

ZTEST(api, test_sec_host_without_the_bit_the_stream_is_unchanged)
{
	struct rec {
		uint8_t id;
		uint8_t len;
		uint8_t d[ANTW_MAX_SIZE_VALUE];
	};
	static struct rec with[16];
	uint32_t          n_with = 0u;
	uint32_t          n_f8 = 0u;
	uint32_t          n_without = 0u;
	uint32_t          from;

	/* With the bit: everything that is not a 0xF8, in order. */
	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	for (uint32_t i = 0u; i < VD_HOPS; i++) {
		(void)vd_hop(i, i == 1u);
	}
	for (uint32_t k = from; k < api_msglog_count(); k++) {
		const uint8_t *d;
		uint8_t        id;
		uint8_t        len;

		d = api_msglog_get(k, &id, &len);
		if (id == (uint8_t)ANTW_MESG_RADIANT_SEC_VERDICT_ID) {
			n_f8++;
			continue;
		}
		zassert_true(n_with < ARRAY_SIZE(with));
		with[n_with].id = id;
		with[n_with].len = len;
		memcpy(with[n_with].d, d, len);
		n_with++;
	}
	zassert_equal(2u, n_f8, "setup: the opted-in run sent no events");
	vd_done();

	/* Without it, the same air at the same instants (fresh contexts, same
	 * epoch, same anchor): the same messages, byte for byte, and no 0xF8. */
	vd_setup(RADIANT_SEC_SW_AUTH, 2u);
	from = api_msglog_count();
	for (uint32_t i = 0u; i < VD_HOPS; i++) {
		(void)vd_hop(i, i == 1u);
	}
	zassert_true(api_msglog_count() < VD_LOG_MAX,
		     "the message log overflowed");
	for (uint32_t k = from; k < api_msglog_count(); k++) {
		const uint8_t *d;
		uint8_t        id;
		uint8_t        len;

		d = api_msglog_get(k, &id, &len);
		zassert_not_equal((uint8_t)ANTW_MESG_RADIANT_SEC_VERDICT_ID, id,
				  "0xF8 sent to a host that never asked");
		zassert_true(n_without < n_with, "more messages than with it");
		zassert_equal(with[n_without].id, id, "message %u", n_without);
		zassert_equal(with[n_without].len, len, "message %u",
			      n_without);
		zassert_mem_equal(with[n_without].d, d, len, "message %u",
				  n_without);
		n_without++;
	}
	zassert_equal(n_with, n_without, "messages missing without the bit");
	zassert_equal(VD_HOPS, n_with, "one BROADCAST_DATA per packet");
	vd_done();
}

/* ---------------------------------------------------------------------------
 * The search: acquiring, re-acquiring and background-scan frames
 * ---------------------------------------------------------------------------
 *
 * Review 2026-11-14, item 1. The frame that acquires a channel, and every
 * frame a background scan hears, reach the host from api_search_acquired(),
 * not from the tracked-RX switch that vd_ingest() stands in for above. So
 * these tests put the sender's bytes on the fake air and let the radio and
 * the search deliver them. host_net_key is the ANT+ key, so the A6 C5 address
 * fake_radio_build_ant_frame() writes is the receiver's.
 */

static uint64_t acq_period_us(void)
{
	return (uint64_t)radiant_channel_counts_to_us(
		RADIANT_CHANNEL_PERIOD_ANT_PLUS);
}

/* The sender's slot index nearest air time t. */
static uint32_t acq_index_at(radiant_time_t t)
{
	return (uint32_t)((t - vd_anchor + acq_period_us() / 2u) /
			  acq_period_us());
}

/* The newest accepted RX window, which must still be open: test_api.c's
 * armed_search_window(). */
static const struct fake_radio_arm *acq_open_rx_arm(void)
{
	uint32_t n = fake_radio_arm_count();

	while (n > 0u) {
		const struct fake_radio_arm *a = fake_radio_arm(--n);

		if (a != NULL && a->kind == FAKE_RADIO_ARM_RX &&
		    a->rc == RADIANT_RADIO_OK_RC) {
			zassert_false(a->terminated,
				      "setup: the newest RX window had already "
				      "run - the search stopped");
			return a;
		}
	}
	zassert_unreachable("setup: no RX window was armed at all");
	return NULL;
}

/* A time inside the search window armed now, for an acquiring frame. */
static radiant_time_t acq_search_t(void)
{
	const struct fake_radio_arm *w = acq_open_rx_arm();
	radiant_time_t               now = radiant_radio_now();
	radiant_time_t               t;

	t = ((w->t_open > now) ? w->t_open : now) + 1000u;
	zassert_true(t + 1000u < w->t_close,
		     "setup: the search window closes too soon");
	return t;
}

/* `pay` on the air at t, on the receiver's channel ID, as it stands. */
static void acq_air_raw(radiant_time_t t, const uint8_t pay[8])
{
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len = fake_radio_build_ant_frame(frame, HOST_DEVNUM,
						 HOST_DEVTYPE, HOST_TRANS, pay);

	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_air_frame(t, frame, len),
		      "could not put the frame on the air");
}

/*
 * The genuine sender: n packets one period apart from air time t, each built
 * for the slot index nearest its air time, with vd_hop_pair()'s plaintext.
 * Returns the first index.
 */
static uint32_t acq_air_genuine(radiant_time_t t, uint32_t n)
{
	uint32_t i0 = acq_index_at(t);

	for (uint32_t k = 0u; k < n; k++) {
		uint8_t pay[8] = { VD_PAGE, 0xAAu, (uint8_t)(i0 + k), 0x11u,
				   0x22u,   0x33u, 0x44u,              0x55u };
		int     rc;

		k_mutex_lock(&api_lock, K_FOREVER);
		rc = radiant_sec_tx_transform(VD_TX_CH, pay, 8u,
					      vd_slot_t(i0 + k));
		k_mutex_unlock(&api_lock);
		zassert_equal(RADIANT_SEC_OK, rc, "the sender refused slot %u",
			      i0 + k);
		acq_air_raw(t + (radiant_time_t)k * acq_period_us(), pay);
	}
	return i0;
}

/* A forged secured-range page claiming counter `ctr`. */
static void acq_air_forged(radiant_time_t t, uint32_t ctr)
{
	const uint8_t pay[8] = { VD_PAGE, (uint8_t)ctr, 0xEEu, 0xEEu,
				 0xEEu,   0xEEu,        0xEEu, 0xEEu };

	acq_air_raw(t, pay);
}

/* Run the radio to t_end, as test_api.c's run_virtual_us(): the sleeps let
 * the event thread run its housekeeping, the sec pump included. */
static void acq_run_to(radiant_time_t t_end)
{
	while (radiant_radio_now() < t_end) {
		radiant_time_t due = fake_radio_next_due();
		radiant_time_t now = radiant_radio_now();
		uint64_t       step = 20000u;

		if (due != RADIANT_TIME_NEVER && due > now &&
		    due - now < step) {
			step = due - now + 1u;
		}
		if (now + step > t_end) {
			step = t_end - now;
		}
		fake_radio_advance(step);
		(void)radiant_event_drain(0u);
		k_sleep(K_MSEC(1));
	}
}

/* One more pump, then drain: every packet the radio handed in has been
 * judged or delivered by the time this returns. */
static void acq_settle(void)
{
	k_mutex_lock(&api_lock, K_FOREVER);
	radiant_sec_pump();
	k_mutex_unlock(&api_lock);
	(void)radiant_event_drain(0u);
}

/* Silence until the tracked receiver drops back to search. */
static void acq_until_search(void)
{
	radiant_time_t t_max = radiant_radio_now() + 20u * acq_period_us();

	while (radiant_channel_state_get(VD_RX_CH) !=
		       RADIANT_CH_STATE_SEARCHING &&
	       radiant_radio_now() < t_max) {
		acq_run_to(radiant_radio_now() + acq_period_us() / 4u);
	}
	zassert_equal(RADIANT_CH_STATE_SEARCHING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the channel never dropped to search");
	/* The search arms its window from the event that dropped it. */
	acq_run_to(radiant_radio_now() + 1000u);
}

/* The BROADCAST_DATA on VD_RX_CH from log index `from` on, by content.
 * Payload bytes [3..6] are 11 22 33 44 in every genuine page (ciphered on
 * the air under X_CONF) and EE in a forged one. */
struct acq_bd {
	uint32_t all;
	uint32_t plain;
	uint32_t forged;
	uint32_t page;  /* those whose page byte is acq_count()'s `page` */
};

static struct acq_bd acq_count(uint32_t from, uint8_t page)
{
	struct acq_bd n = { 0u, 0u, 0u, 0u };

	zassert_true(api_msglog_count() < VD_LOG_MAX,
		     "the message log overflowed");
	for (uint32_t k = from; k < api_msglog_count(); k++) {
		const uint8_t *d;
		uint8_t        id;
		uint8_t        len;

		d = api_msglog_get(k, &id, &len);
		if (id != (uint8_t)ANTW_MESG_BROADCAST_DATA_ID ||
		    d[0] != VD_RX_CH || len < 9u) {
			continue;
		}
		n.all++;
		if (d[4] == 0x11u && d[5] == 0x22u && d[6] == 0x33u &&
		    d[7] == 0x44u) {
			n.plain++;
		}
		if (d[4] == 0xEEu && d[5] == 0xEEu && d[6] == 0xEEu &&
		    d[7] == 0xEEu) {
			n.forged++;
		}
		if (d[1] == page) {
			n.page++;
		}
	}
	return n;
}

static void acq_done(void)
{
	fake_radio_air_clear();
	vd_done();
}

ZTEST(api, test_sec_host_an_acquiring_frame_is_delivered_decrypted)
{
	/* X_CONF: the frame that acquires the channel is a secured page like
	 * any other. Delivered as it came off the air, the host got a page of
	 * ciphertext as if it were plaintext, on every (re)acquisition. */
	struct acq_bd  n;
	radiant_time_t t;
	uint32_t       from;

	vd_setup(RADIANT_SEC_SW_CONF, 2u);
	from = api_msglog_count();
	t = acq_search_t();
	(void)acq_air_genuine(t, 1u);
	acq_run_to(t + 5000u);
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the frame did not acquire the channel");
	acq_settle();
	n = acq_count(from, VD_PAGE);
	zassert_equal(1u, n.all, "the acquiring frame arrived %u times",
		      n.all);
	zassert_equal(1u, n.plain,
		      "the acquiring frame reached the host as ciphertext: it "
		      "skipped radiant_sec_rx_ingest()");
	acq_done();
}

ZTEST(api, test_sec_host_a_forged_acquiring_frame_is_not_delivered_under_drop)
{
	/* X_AUTH + DROP_UNVER: nothing in the secured range reaches the host
	 * before its window verifies. A forger who jams the link until the
	 * slave drops to search, then sends one frame on its channel ID, had
	 * that frame delivered at once (D15 by another door). */
	struct acq_bd  n;
	radiant_time_t t;
	uint32_t       from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER, 2u);
	from = api_msglog_count();
	t = acq_search_t();
	acq_air_forged(t, acq_index_at(t));
	acq_run_to(t + 5000u);
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the forged frame did not acquire the channel");
	acq_settle();
	n = acq_count(from, VD_PAGE);
	zassert_equal(0u, n.all,
		      "a forged secured-range acquiring frame reached the host "
		      "under DROP_UNVER with no verdict");
	acq_done();
}

ZTEST(api, test_sec_host_a_reacquiring_frame_takes_the_hook)
{
	/* The same door after a drop to search: the frame that brings the
	 * channel back is an acquiring frame too. */
	struct acq_bd  n;
	radiant_time_t t;
	uint32_t       from;

	vd_setup(RADIANT_SEC_SW_CONF, 2u);
	t = acq_search_t();
	(void)acq_air_genuine(t, 3u);
	acq_run_to(t + 2u * acq_period_us() + 5000u);
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the channel did not acquire");
	acq_until_search();
	acq_settle();

	from = api_msglog_count();
	t = acq_search_t();
	(void)acq_air_genuine(t, 1u);
	acq_run_to(t + 5000u);
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the frame did not reacquire the channel");
	acq_settle();
	n = acq_count(from, VD_PAGE);
	zassert_equal(1u, n.all, "the reacquiring frame arrived %u times",
		      n.all);
	zassert_equal(1u, n.plain,
		      "the reacquiring frame reached the host as ciphertext");
	acq_done();
}

ZTEST(api, test_sec_host_a_keyed_scan_delivers_no_secured_page_unjudged)
{
	/* A background scan never leaves its search, so every frame it hears
	 * comes through the acquisition path: a key on it did nothing. Owner
	 * decision 2026-11-14: its frames take the same hook, no refusal to
	 * key. Under DROP_UNVER a lone secured page waits for a verdict that
	 * never comes; a page outside the range still arrives at once. */
	static const uint8_t clear[8] = { 0x50u, 0x00u, 1u, 2u, 3u, 4u, 5u,
					  6u };
	struct acq_bd        n;
	radiant_time_t       t;
	uint32_t             from;

	vd_setup_pair_ext(VD_RX_CH, VD_TX_CH, HOST_DEVNUM,
			  RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER, 2u,
			  (uint8_t)ANTW_EXT_PARAM_ALWAYS_SEARCH);
	from = api_msglog_count();
	t = acq_search_t();
	(void)acq_air_genuine(t, 1u);
	acq_run_to(t + 5000u);
	zassert_equal(RADIANT_CH_STATE_SEARCHING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the scan left its search");
	acq_settle();
	n = acq_count(from, VD_PAGE);
	zassert_equal(0u, n.all,
		      "a keyed background scan delivered a secured-range page "
		      "before any verdict");

	t = acq_search_t();
	acq_air_raw(t, clear);
	acq_run_to(t + 5000u);
	(void)radiant_event_drain(0u); /* no pump: it must not need one */
	n = acq_count(from, 0x50u);
	zassert_equal(1u, n.page,
		      "a page outside the secured range did not arrive at once");
	zassert_equal(1u, n.all, "and nothing else did");
	acq_done();
}

ZTEST(api, test_sec_host_a_clear_acquiring_frame_still_arrives_at_once)
{
	/* The other half of the acquisition fix: a page outside the secured
	 * range is RX_PLAIN and is posted from the radio interrupt as before,
	 * with no pump in between. */
	static const uint8_t clear[8] = { 0x50u, 0x00u, 1u, 2u, 3u, 4u, 5u,
					  6u };
	struct acq_bd        n;
	radiant_time_t       t;
	uint32_t             from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER, 2u);
	from = api_msglog_count();
	t = acq_search_t();
	acq_air_raw(t, clear);
	acq_run_to(t + 5000u);
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the frame did not acquire the channel");
	(void)radiant_event_drain(0u);
	n = acq_count(from, 0x50u);
	zassert_equal(1u, n.page,
		      "a clear acquiring frame did not arrive at once");
	acq_done();
}

/* `pay` on the air at t as acknowledged data (control byte 0xAA) on the
 * receiver's channel ID. The search acquires on it like on a broadcast. */
static void acq_air_acked(radiant_time_t t, const uint8_t pay[8])
{
	uint8_t frame[FAKE_RADIO_AIR_FRAME_MAX];
	uint8_t len = fake_radio_build_ant_frame(frame, HOST_DEVNUM,
						 HOST_DEVTYPE, HOST_TRANS, pay);

	frame[6] = 0xAAu;
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_air_frame(t, frame, len),
		      "could not put the frame on the air");
}

ZTEST(api, test_sec_host_an_acked_acquiring_frame_follows_d15)
{
	/* Re-review of item 1: the acquiring frame was ingested as a broadcast
	 * whatever its message type. D15 says a secured-range page sent as
	 * acknowledged data is dropped and counted, never delivered; here it
	 * acquired the channel and was delivered, unverified. */
	static const uint8_t forged[8] = { VD_PAGE, 0x00u, 0xEEu, 0xEEu,
					   0xEEu,   0xEEu, 0xEEu, 0xEEu };
	struct radiant_sec_stats st0;
	struct radiant_sec_stats st1;
	struct acq_bd            n;
	radiant_time_t           t;
	uint32_t                 from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	k_mutex_lock(&api_lock, K_FOREVER);
	radiant_sec_get_stats(VD_RX_CH, &st0);
	k_mutex_unlock(&api_lock);
	from = api_msglog_count();
	t = acq_search_t();
	acq_air_acked(t, forged);
	acq_run_to(t + 5000u);
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the acked frame did not acquire the channel");
	acq_settle();
	k_mutex_lock(&api_lock, K_FOREVER);
	radiant_sec_get_stats(VD_RX_CH, &st1);
	k_mutex_unlock(&api_lock);
	n = acq_count(from, VD_PAGE);
	zassert_equal(0u, n.all,
		      "an acknowledged-data secured-range acquiring frame was "
		      "delivered, judged as a broadcast");
	zassert_equal(st0.dropped_non_broadcast + 1u, st1.dropped_non_broadcast,
		      "the frame was not counted as a dropped non-broadcast");
	acq_done();
}

ZTEST(api, test_sec_host_an_acked_clear_acquiring_frame_still_arrives)
{
	/* The control: acknowledged data outside the secured range is not the
	 * secured stream, and reaches the host as it did before the hook. */
	static const uint8_t clear[8] = { 0x50u, 0x00u, 1u, 2u, 3u, 4u, 5u,
					  6u };
	struct acq_bd        n;
	radiant_time_t       t;
	uint32_t             from;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_DROP_UNVER, 2u);
	from = api_msglog_count();
	t = acq_search_t();
	acq_air_acked(t, clear);
	acq_run_to(t + 5000u);
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the acked frame did not acquire the channel");
	(void)radiant_event_drain(0u);
	n = acq_count(from, 0x50u);
	zassert_equal(1u, n.page,
		      "a clear acked acquiring frame did not arrive at once");
	zassert_equal(1u, n.all, "and nothing else did");
	acq_done();
}

/* A keyed background scan whose channel ID wildcards the device and
 * transmission types, as a scan does; the sender keyed as in
 * vd_setup_pair(). */
static void acq_scan_setup(uint8_t switches)
{
	int rc[4];

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_network_address_set(0u, host_net_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(
			      VD_RX_CH, (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY,
			      0u, (uint8_t)ANTW_EXT_PARAM_ALWAYS_SEARCH));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_id_set(VD_RX_CH, HOST_DEVNUM, 0u, 0u));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_period_set(VD_RX_CH,
					      RADIANT_CHANNEL_PERIOD_ANT_PLUS));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_radio_freq_set(VD_RX_CH, HOST_RF));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_open(VD_RX_CH));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(VD_RX_CH, 128u, host_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_config(VD_RX_CH, switches, 2u, 0x01u, 0x0Fu));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_epoch_set(VD_RX_CH, 0u, VD_EPOCH, 0u));
	k_mutex_lock(&api_lock, K_FOREVER);
	rc[0] = radiant_sec_set_key(VD_TX_CH, host_key, 128, HOST_DEVNUM);
	rc[1] = radiant_sec_set_devnum(VD_TX_CH, HOST_DEVNUM);
	rc[2] = radiant_sec_configure(
		VD_TX_CH,
		(uint8_t)(switches & (RADIANT_SEC_SW_CONF | RADIANT_SEC_SW_AUTH)),
		2u, 0x01u, 0x0Fu);
	rc[3] = radiant_sec_set_epoch(VD_TX_CH, VD_EPOCH, 0u,
				      RADIANT_CHANNEL_PERIOD_ANT_PLUS);
	vd_anchor = (uint64_t)radiant_radio_now();
	k_mutex_unlock(&api_lock);
	for (int i = 0; i < 4; i++) {
		zassert_equal(RADIANT_SEC_OK, rc[i], "sender setup step %d", i);
	}
	(void)radiant_event_drain(0u);
}

ZTEST(api, test_sec_host_a_released_scan_frame_keeps_its_sender_id)
{
	/*
	 * Re-review of item 1: a scan frame the hook holds is delivered later
	 * by radiant_sec_deliver(), which named the channel's wildcard ID and
	 * dropped the frame's RSSI. For a scan the sender's ID and RSSI on
	 * each frame are the point. A clear page from the same sender, posted
	 * at once, is the reference.
	 */
	static const uint8_t clear[8] = { 0x50u, 0x00u, 1u, 2u, 3u, 4u, 5u,
					  6u };
	const uint8_t        cfg = (uint8_t)(ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID |
					     ANTW_LIB_CONFIG_MESG_OUT_INC_RSSI);
	const uint8_t       *sec = NULL;
	const uint8_t       *clr = NULL;
	uint8_t              len_sec = 0u;
	uint8_t              len_clr = 0u;
	radiant_time_t       t;
	uint32_t             from;

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_lib_config_set(cfg));
	acq_scan_setup(RADIANT_SEC_SW_CONF);
	from = api_msglog_count();
	t = acq_search_t();
	(void)acq_air_genuine(t, 1u);
	acq_air_raw(t + 50000u, clear);
	acq_run_to(t + 60000u);
	acq_settle();
	for (uint32_t k = from; k < api_msglog_count(); k++) {
		uint8_t        id;
		uint8_t        len;
		const uint8_t *d = api_msglog_get(k, &id, &len);

		if (id != (uint8_t)ANTW_MESG_BROADCAST_DATA_ID ||
		    d[0] != VD_RX_CH) {
			continue;
		}
		if (d[1] == VD_PAGE) {
			sec = d;
			len_sec = len;
		} else if (d[1] == 0x50u) {
			clr = d;
			len_clr = len;
		}
	}
	(void)antr_lib_config_clear(cfg);
	zassert_not_null(clr, "setup: the clear page did not arrive");
	zassert_not_null(sec, "setup: the secured page was not released");
	zassert_true(len_clr >= 17u && clr[12] == HOST_DEVTYPE,
		     "setup: the clear page carries no sender ID and RSSI");
	zassert_equal(len_clr, len_sec, "the released page's extension differs");
	zassert_equal(clr[9], sec[9],
		      "the released page's extension flags differ (RSSI lost)");
	zassert_mem_equal(&clr[10], &sec[10], 4u,
			  "a released scan frame carried the channel's wildcard "
			  "ID, not the sender's");
	zassert_equal(clr[15], sec[15],
		      "a released scan frame lost the sender's RSSI");
	acq_done();
}

/*
 * The review's side point: the receive window state is not discarded on
 * RX_FAIL_GO_TO_SEARCH. Four genuine packets, then a forgery in the next
 * tracked slot, then silence until the channel drops to search, then the
 * genuine sender again from the search. The forgery's window is still open
 * across the dropout; rx_process() closes it when the first packet of
 * another window arrives. Returns the forgery's counter.
 */
static uint32_t acq_forgery_across_a_dropout(uint8_t switches)
{
	radiant_time_t t;
	uint32_t       i0;

	vd_setup(switches, 2u);
	t = acq_search_t();
	i0 = acq_air_genuine(t, 4u);
	acq_air_forged(t + 4u * acq_period_us(), i0 + 4u);
	acq_run_to(t + 4u * acq_period_us() + 5000u);
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the channel did not acquire and track");
	acq_until_search();

	t = acq_search_t();
	(void)acq_air_genuine(t, 8u);
	acq_run_to(t + 7u * acq_period_us() + 5000u);
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the channel did not reacquire");
	acq_settle();
	return i0 + 4u;
}

ZTEST(api, test_sec_host_a_forgery_held_across_a_dropout_is_never_released)
{
	struct acq_bd n;
	uint32_t      from = api_msglog_count();

	(void)acq_forgery_across_a_dropout(RADIANT_SEC_SW_AUTH |
					   RADIANT_SEC_SW_DROP_UNVER |
					   RADIANT_SEC_SW_VERDICT_EVENT);
	n = acq_count(from, VD_PAGE);
	zassert_equal(0u, n.forged,
		      "the forgery held across the dropout was released");
	zassert_true(n.plain >= 2u,
		     "setup: no genuine window verified after the dropout (%u)",
		     n.plain);
	acq_done();
}

/*
 * The host rule (tools/ant_sec.py VerdictResolver) applied to the log from
 * index `from` on: the forged page claiming counter `ctr` is settled by the
 * first verdict after it that covers its byte [1], unless a seq gap, or the
 * first verdict after a start, settles it unverified before that. Under the
 * deliver policy that verdict must not be VERIFIED. Stricter than a host: the
 * resolver's deadline is not applied. Returns false when nothing judged the
 * forgery (a host's deadline then settles it unverified).
 */
static bool acq_expect_not_credited(uint32_t from, uint32_t ctr,
				    const char *what)
{
	int k_forged = -1;
	int prev_seq = -1;

	zassert_true(api_msglog_count() < VD_LOG_MAX,
		     "the message log overflowed");
	for (uint32_t k = from; k < api_msglog_count(); k++) {
		const uint8_t *d;
		uint8_t        id;
		uint8_t        len;

		d = api_msglog_get(k, &id, &len);
		if (d[0] != VD_RX_CH) {
			continue;
		}
		if (k_forged < 0) {
			if (id == (uint8_t)ANTW_MESG_BROADCAST_DATA_ID &&
			    len >= 9u && d[4] == 0xEEu && d[7] == 0xEEu) {
				k_forged = (int)k;
			} else if (id ==
				   (uint8_t)ANTW_MESG_RADIANT_SEC_VERDICT_ID) {
				prev_seq = (int)vd_seq(d);
			}
			continue;
		}
		if (id != (uint8_t)ANTW_MESG_RADIANT_SEC_VERDICT_ID) {
			continue;
		}
		if (prev_seq < 0 ||
		    vd_seq(d) != (uint16_t)((uint16_t)prev_seq + 1u)) {
			return true; /* a baseline or a gap */
		}
		prev_seq = (int)vd_seq(d);
		if ((uint8_t)((uint8_t)ctr - (uint8_t)rd16(&d[2])) < d[4]) {
			zassert_not_equal(V_OK, d[1],
					  "%s was credited VERIFIED (window %u)",
					  what, rd16(&d[2]));
			return true;
		}
	}
	zassert_true(k_forged >= 0, "setup: %s was not delivered", what);
	return false;
}

ZTEST(api, test_sec_host_a_forgery_across_a_dropout_is_not_credited)
{
	/* The deliver policy: the forgery reaches the host at once, so what
	 * matters is the verdict a host binds to it. */
	uint32_t from = api_msglog_count();
	uint32_t ctr;
	bool     judged;

	ctr = acq_forgery_across_a_dropout(RADIANT_SEC_SW_AUTH |
					   RADIANT_SEC_SW_VERDICT_EVENT);
	judged = acq_expect_not_credited(from, ctr,
					 "the forgery across the dropout");
	zassert_true(judged, "setup: no verdict or seq gap ever settled the "
		     "forgery at counter %u, so nothing was checked", ctr);
	acq_done();
}

ZTEST(api, test_sec_host_a_forged_reacquiring_frame_is_not_credited)
{
	/*
	 * Item 1 under the deliver policy. The channel tracks long enough for
	 * the host to hold a seq baseline, drops to search, and a forged frame
	 * re-acquires it claiming the counter that opens the NEXT genuine
	 * window. Posted without the hook it was in no firmware window, so the
	 * genuine window verified and a host bound that VERIFIED to the forgery
	 * (the first covering verdict after it, with no gap). Through the hook
	 * it is absorbed into that window, which then fails. On a first
	 * acquisition the first verdict only sets the host's baseline, which is
	 * why the tracked phase is needed.
	 *
	 * The window claimed is the one after the next slot. The first tracked
	 * slot after an acquisition is always missed, and that is the core
	 * search's behaviour, not the test harness's: the search RX chunk keeps
	 * running after its only searching channel acquires (radiant_search_end()
	 * only clears the record), so frame 1 is heard inside the chunk and
	 * dropped, and the host sees RX_FAIL after every acquiring frame
	 * (recorded as an open TODO, not changed here). A genuine window missing
	 * a packet fails on its own. So the forged packet is 5 or 6 periods older than the verdict,
	 * past the resolver's 2W-1 periods + slack, and the check here leaves
	 * the deadline out; tools/test_ant_sec.py has the in-deadline case.
	 */
	radiant_time_t t;
	uint32_t       from;
	uint32_t       i;
	uint32_t       ctr;

	vd_setup(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT, 2u);
	from = api_msglog_count();
	t = acq_search_t();
	(void)acq_air_genuine(t, 6u);
	acq_run_to(t + 5u * acq_period_us() + 5000u);
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the channel did not acquire and track");
	acq_until_search();
	acq_settle();

	t = acq_search_t();
	i = acq_index_at(t);
	ctr = (i + 3u) & ~1u; /* a window start, i+2 or i+3, at W=2 */
	acq_air_forged(t, ctr);
	(void)acq_air_genuine(t + acq_period_us(), 6u);
	acq_run_to(t + 6u * acq_period_us() + 5000u);
	zassert_equal(RADIANT_CH_STATE_TRACKING,
		      radiant_channel_state_get(VD_RX_CH),
		      "setup: the forgery did not reacquire the channel");
	acq_settle();
	zassert_true(acq_expect_not_credited(from, ctr,
					     "the forged acquiring frame"),
		     "setup: no verdict ever covered the forgery");
	acq_done();
}

ZTEST(api, test_sec_host_the_verdict_event_needs_x_auth)
{
	uint8_t st[ANTR_SEC_STATUS_LEN];

	host_assign(true, true);
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_key_set(HOST_CH, 128u, host_key));

	/* Only X_AUTH has a verdict. Accepting the bit without it would
	 * promise events that never come, and a host would read the silence
	 * as nothing having failed. */
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_config(HOST_CH, RADIANT_SEC_SW_VERDICT_EVENT,
				      2u, 0x01u, 0x0Fu));
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      antr_sec_config(HOST_CH,
				      RADIANT_SEC_SW_CONF |
					      RADIANT_SEC_SW_VERDICT_EVENT,
				      2u, 0x01u, 0x0Fu));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_config(HOST_CH,
				      RADIANT_SEC_SW_AUTH |
					      RADIANT_SEC_SW_VERDICT_EVENT,
				      2u, 0x01u, 0x0Fu));

	/* And the host can read back that it is on. */
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_sec_status_get(HOST_CH, st, sizeof(st)));
	zassert_equal(RADIANT_SEC_SW_AUTH | RADIANT_SEC_SW_VERDICT_EVENT,
		      st[2]);
	host_done();
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

#if defined(CONFIG_IRQ_OFFLOAD)
struct isr_probe_result {
	bool held;
	bool ran;
};

static void isr_probe(const void *arg)
{
	struct isr_probe_result *r = (struct isr_probe_result *)arg;

	r->held = antr_api_lock_held();
	radiant_sec_port_lock_check("isr_probe");
	r->ran = true;
}

/* The ISR half of antr_api_lock_held(): an interrupt that lands while the
 * test thread owns api_lock sees that thread as k_current_get(), so the
 * owner check alone would pass it. Only the k_is_in_isr() term refuses it. */
ZTEST(api, test_sec_host_the_lock_check_refuses_an_isr_inside_the_lock)
{
	struct isr_probe_result r = { 0 };
	uint32_t                base = antr_api_lock_violations;

	/* Same reason as the test above: with CONFIG_ASSERT the counted
	 * violation halts the image. */
	if (IS_ENABLED(CONFIG_ASSERT)) {
		ztest_test_skip();
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	zassert_true(antr_api_lock_held());
	irq_offload(isr_probe, &r);
	k_mutex_unlock(&api_lock);

	zassert_true(r.ran, "irq_offload did not run the probe");
	zassert_false(r.held,
		      "an ISR interrupting the lock's owner counted as holding "
		      "api_lock");
	zassert_equal(base + 1u, antr_api_lock_violations,
		      "a radiant_sec check from an ISR was not counted");

	antr_api_lock_violations = base;
}
#endif /* CONFIG_IRQ_OFFLOAD */

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

/* RFC 7748 6.1's Alice (A, initiator) and Bob (B, responder), and the nonces
 * and fingerprint tools/test_radiant_crypto.py pins (TestPairCommitReveal), so
 * the host surface is checked against an independent implementation too. */
static const uint8_t pair_sa[RADIANT_SEC_X25519_BYTES] = {
	0x77, 0x07, 0x6d, 0x0a, 0x73, 0x18, 0xa5, 0x7d,
	0x3c, 0x16, 0xc1, 0x72, 0x51, 0xb2, 0x66, 0x45,
	0xdf, 0x4c, 0x2f, 0x87, 0xeb, 0xc0, 0x99, 0x2a,
	0xb1, 0x77, 0xfb, 0xa5, 0x1d, 0xb9, 0x2c, 0x2a,
};
static const uint8_t pair_sb[RADIANT_SEC_X25519_BYTES] = {
	0x5d, 0xab, 0x08, 0x7e, 0x62, 0x4a, 0x8a, 0x4b,
	0x79, 0xe1, 0x7f, 0x8b, 0x83, 0x80, 0x0e, 0xe6,
	0x6f, 0x3b, 0xb1, 0x29, 0x26, 0x18, 0xb6, 0xfd,
	0x1c, 0x2f, 0x8b, 0x27, 0xff, 0x88, 0xe0, 0xeb,
};
/* [role, nonce] as 0xF5 NONCE carries them. */
static const uint8_t pair_na_arg[1 + RADIANT_SEC_PAIR_NONCE_BYTES] = {
	RADIANT_SEC_PAIR_INITIATOR,
	0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
	0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
};
static const uint8_t pair_nb_arg[1 + RADIANT_SEC_PAIR_NONCE_BYTES] = {
	RADIANT_SEC_PAIR_RESPONDER,
	0xa0, 0xa1, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
	0xa8, 0xa9, 0xaa, 0xab, 0xac, 0xad, 0xae, 0xaf,
};
#define PAIR_VEC_FP 362706u

#define PEER_CH     1u
#define PEER_DEVNUM 0x51C4u

static antr_err_t f5(uint8_t ch, uint8_t sub, const uint8_t *arg,
		     uint8_t arg_len, uint8_t *reply, uint8_t *rlen)
{
	return antr_sec_pair(ch, sub, arg, arg_len, reply,
			     2u + RADIANT_SEC_X25519_BYTES, rlen);
}

static uint32_t rd24(const uint8_t *p)
{
	return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16);
}

/* Both ends entered, keys made and crossed over 0xF5: what a host does before
 * the nonces. HOST_CH is A, PEER_CH is B. */
static void f5_keys_cross(void)
{
	uint8_t pub_a[RADIANT_SEC_X25519_BYTES];
	uint8_t pub_b[RADIANT_SEC_X25519_BYTES];
	uint8_t reply[2 + RADIANT_SEC_X25519_BYTES];
	uint8_t rlen = 0u;

	host_assign(true, true);
	zassert_ok(antr_channel_assign(PEER_CH,
				       (uint8_t)ANTW_CHANNEL_TYPE_MASTER, 0u,
				       0u));
	zassert_ok(antr_channel_id_set(PEER_CH, PEER_DEVNUM, HOST_DEVTYPE,
				       HOST_TRANS));

	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_ENTER, NULL, 0u, reply, &rlen));
	zassert_ok(f5(PEER_CH, ANTW_RADIANT_PAIR_ENTER, NULL, 0u, reply, &rlen));
	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_SCALAR, pair_sa,
		      sizeof(pair_sa), reply, &rlen));
	memcpy(pub_a, &reply[2], sizeof(pub_a));
	zassert_ok(f5(PEER_CH, ANTW_RADIANT_PAIR_SCALAR, pair_sb,
		      sizeof(pair_sb), reply, &rlen));
	memcpy(pub_b, &reply[2], sizeof(pub_b));

	/* DH only: the reply is the bare echo - no fingerprint exists yet,
	 * which is what leaves no two-move path for a downgrade. */
	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_EXCHANGE, pub_b, sizeof(pub_b),
		      reply, &rlen));
	zassert_equal(2u, rlen, "EXCHANGE answered with a fingerprint");
	zassert_ok(f5(PEER_CH, ANTW_RADIANT_PAIR_EXCHANGE, pub_a, sizeof(pub_a),
		      reply, &rlen));
	zassert_equal(2u, rlen);
}

static void f5_done(void)
{
	uint8_t reply[2 + RADIANT_SEC_X25519_BYTES];
	uint8_t rlen = 0u;

	(void)f5(HOST_CH, ANTW_RADIANT_PAIR_LEAVE, NULL, 0u, reply, &rlen);
	(void)f5(PEER_CH, ANTW_RADIANT_PAIR_LEAVE, NULL, 0u, reply, &rlen);
	(void)antr_channel_unassign(PEER_CH);
	host_done();
}

static uint16_t enrolments_of(uint8_t ch)
{
	uint8_t st[ANTR_SEC_STATUS_LEN];

	zassert_ok(antr_sec_status_get(ch, st, sizeof(st)));
	return rd16(&st[24]);
}

/* The whole host sequence of docs/ant-serial-protocol.md, both ends. */
ZTEST(api, test_sec_host_pairing_commits_before_it_reveals)
{
	uint8_t  reply[2 + RADIANT_SEC_X25519_BYTES];
	uint8_t  cb[RADIANT_SEC_BLOCK_BYTES];
	uint8_t  rlen = 0u;
	uint32_t fp_a, fp_b;

	f5_keys_cross();

	/* B: NONCE as responder answers Cb, which B's host sends to A. */
	zassert_ok(f5(PEER_CH, ANTW_RADIANT_PAIR_NONCE, pair_nb_arg,
		      sizeof(pair_nb_arg), reply, &rlen));
	zassert_equal(2u + RADIANT_SEC_BLOCK_BYTES, rlen);
	zassert_equal(PEER_CH, reply[0]);
	zassert_equal(ANTW_RADIANT_PAIR_NONCE, reply[1]);
	memcpy(cb, &reply[2], sizeof(cb));

	/* A: its own nonce (bare echo), then the commitment, then Na out. */
	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_NONCE, pair_na_arg,
		      sizeof(pair_na_arg), reply, &rlen));
	zassert_equal(2u, rlen, "an initiator's NONCE answered a commitment");
	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_PEER_COMMIT, cb, sizeof(cb),
		      reply, &rlen));
	zassert_equal(2u, rlen);

	/* B finishes on Na, and only then reveals Nb. */
	zassert_ok(f5(PEER_CH, ANTW_RADIANT_PAIR_PEER_NONCE, &pair_na_arg[1],
		      RADIANT_SEC_PAIR_NONCE_BYTES, reply, &rlen));
	zassert_equal(5u, rlen);
	fp_b = rd24(&reply[2]);

	/* A checks Nb against Cb and finishes. */
	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_PEER_NONCE, &pair_nb_arg[1],
		      RADIANT_SEC_PAIR_NONCE_BYTES, reply, &rlen));
	zassert_equal(5u, rlen);
	fp_a = rd24(&reply[2]);

	zassert_equal(fp_a, fp_b, "the two ends show different digits");
	zassert_equal(PAIR_VEC_FP, fp_a,
		      "0xF5's fingerprint disagrees with radiant_crypto.py");
	zassert_equal(1u, enrolments_of(HOST_CH));
	zassert_equal(1u, enrolments_of(PEER_CH));

	f5_done();
}

ZTEST(api, test_sec_host_pairing_refuses_out_of_order_and_unopened_commitments)
{
	uint8_t reply[2 + RADIANT_SEC_X25519_BYTES];
	uint8_t cb[RADIANT_SEC_BLOCK_BYTES] = { 0 };
	uint8_t bad_nb[RADIANT_SEC_PAIR_NONCE_BYTES];
	uint8_t rlen = 0u;

	f5_keys_cross();

	/* Short and long arguments are malformed, not out of order: lengths
	 * are exact. */
	{
		uint8_t long_arg[2u + RADIANT_SEC_PAIR_NONCE_BYTES];

		memcpy(long_arg, pair_nb_arg, sizeof(pair_nb_arg));
		long_arg[sizeof(long_arg) - 1u] = 0x5Au;
		zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
			      f5(PEER_CH, ANTW_RADIANT_PAIR_NONCE, long_arg,
				 sizeof(long_arg), reply, &rlen));
		zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
			      f5(HOST_CH, ANTW_RADIANT_PAIR_ENTER, long_arg, 2u,
				 reply, &rlen));
	}
	zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
		      f5(PEER_CH, ANTW_RADIANT_PAIR_NONCE, pair_nb_arg,
			 RADIANT_SEC_PAIR_NONCE_BYTES, reply, &rlen));
	zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
		      f5(HOST_CH, ANTW_RADIANT_PAIR_PEER_COMMIT, cb,
			 RADIANT_SEC_BLOCK_BYTES - 1u, reply, &rlen));
	zassert_equal((antr_err_t)ANTW_INVALID_MESSAGE,
		      f5(HOST_CH, ANTW_RADIANT_PAIR_PEER_NONCE, cb,
			 RADIANT_SEC_PAIR_NONCE_BYTES - 1u, reply, &rlen));

	zassert_ok(f5(PEER_CH, ANTW_RADIANT_PAIR_NONCE, pair_nb_arg,
		      sizeof(pair_nb_arg), reply, &rlen));
	memcpy(cb, &reply[2], sizeof(cb));
	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_NONCE, pair_na_arg,
		      sizeof(pair_na_arg), reply, &rlen));

	/* A reveal before the commitment, and a commitment to a responder. */
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      f5(HOST_CH, ANTW_RADIANT_PAIR_PEER_NONCE,
			 &pair_nb_arg[1], RADIANT_SEC_PAIR_NONCE_BYTES, reply,
			 &rlen));
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      f5(PEER_CH, ANTW_RADIANT_PAIR_PEER_COMMIT, cb,
			 sizeof(cb), reply, &rlen));

	/* A nonce that does not open the commitment: refused, nothing
	 * installed or counted, and the exchange is gone. */
	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_PEER_COMMIT, cb, sizeof(cb),
		      reply, &rlen));
	memcpy(bad_nb, &pair_nb_arg[1], sizeof(bad_nb));
	bad_nb[0] ^= 0x01u;
	/* Its own code: the MITM signature must not read as the out-of-order
	 * refusal above (review 2026-11-13). */
	zassert_equal((antr_err_t)ANTW_RADIANT_PAIR_COMMIT_MISMATCH,
		      f5(HOST_CH, ANTW_RADIANT_PAIR_PEER_NONCE, bad_nb,
			 sizeof(bad_nb), reply, &rlen));
	zassert_not_equal((antr_err_t)ANTW_RADIANT_PAIR_COMMIT_MISMATCH,
			  (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED);
	zassert_equal(0u, enrolments_of(HOST_CH));
	zassert_equal((antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED,
		      f5(HOST_CH, ANTW_RADIANT_PAIR_PEER_NONCE,
			 &pair_nb_arg[1], RADIANT_SEC_PAIR_NONCE_BYTES, reply,
			 &rlen),
		      "the right nonce was accepted after a wrong one");

	f5_done();
}

/*
 * How long a pairing command holds api_lock. Each X25519 is a full scalar
 * multiplication on this build's software implementation; the number printed
 * is the figure to read against the RX thread (which needs api_lock for the
 * pump and the scheduler). The commit/reveal adds only CMACs, so NONCE and
 * PEER_NONCE are held to a few milliseconds: the commitment must not grow the
 * hold materially.
 */
ZTEST(api, test_sec_host_pairing_holds_api_lock_and_its_hold_time)
{
	uint8_t  reply[2 + RADIANT_SEC_X25519_BYTES];
	uint8_t  peer[RADIANT_SEC_X25519_BYTES];
	uint8_t  rlen = 0u;
	uint32_t base;
	uint32_t t0, t_scalar, t_exchange, t_nonce, t_reveal;

	host_assign(true, true);
	base = antr_api_lock_violations;

	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_ENTER, NULL, 0u, reply, &rlen));
	t0 = k_cycle_get_32();
	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_SCALAR, pair_sa,
		      sizeof(pair_sa), reply, &rlen));
	t_scalar = k_cycle_get_32() - t0;
	zassert_equal(2u + RADIANT_SEC_X25519_BYTES, rlen);

	/* The peer's public key is the one just produced: a valid, non-
	 * degenerate point, so the exchange runs to the end. This end plays
	 * the responder, and the "initiator's" nonce is just another value. */
	memcpy(peer, &reply[2], sizeof(peer));
	t0 = k_cycle_get_32();
	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_EXCHANGE, peer, sizeof(peer),
		      reply, &rlen));
	t_exchange = k_cycle_get_32() - t0;

	t0 = k_cycle_get_32();
	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_NONCE, pair_nb_arg,
		      sizeof(pair_nb_arg), reply, &rlen));
	t_nonce = k_cycle_get_32() - t0;

	t0 = k_cycle_get_32();
	zassert_ok(f5(HOST_CH, ANTW_RADIANT_PAIR_PEER_NONCE, &pair_na_arg[1],
		      RADIANT_SEC_PAIR_NONCE_BYTES, reply, &rlen));
	t_reveal = k_cycle_get_32() - t0;
	zassert_equal(5u, rlen);

	zassert_equal(base, antr_api_lock_violations,
		      "a 0xF5 command reached radiant_sec without api_lock");

	uint64_t hz = sys_clock_hw_cycles_per_sec();
	uint32_t us_scalar = (uint32_t)(((uint64_t)t_scalar * 1000000u) / hz);
	uint32_t us_exchange =
		(uint32_t)(((uint64_t)t_exchange * 1000000u) / hz);
	uint32_t us_nonce = (uint32_t)(((uint64_t)t_nonce * 1000000u) / hz);
	uint32_t us_reveal = (uint32_t)(((uint64_t)t_reveal * 1000000u) / hz);

	printk("X25519 api_lock hold: SCALAR %u us, EXCHANGE %u us, "
	       "NONCE %u us, PEER_NONCE %u us\n",
	       us_scalar, us_exchange, us_nonce, us_reveal);
	zassert_true(us_scalar < 1000000u && us_exchange < 1000000u);
	zassert_true(us_nonce < 5000u && us_reveal < 5000u,
		     "the commit/reveal holds api_lock for milliseconds: "
		     "NONCE %u us, PEER_NONCE %u us", us_nonce, us_reveal);

	(void)f5(HOST_CH, ANTW_RADIANT_PAIR_LEAVE, NULL, 0u, reply, &rlen);
	host_done();
}
#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */

#endif /* CONFIG_RADIANT_SEC_LOCK_CHECK */

#endif /* CONFIG_RADIANT_SEC_HOST_MESSAGES */
