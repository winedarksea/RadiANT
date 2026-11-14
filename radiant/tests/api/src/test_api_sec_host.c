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
#define VD_F8_LEN  6u

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
static void vd_setup_pair(uint8_t rx, uint8_t tx, uint16_t devnum,
			  uint8_t switches, uint8_t w)
{
	int rc[4];

	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_network_address_set(0u, host_net_key));
	zassert_equal((antr_err_t)ANTW_RESPONSE_NO_ERROR,
		      antr_channel_assign(
			      rx, (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY, 0u,
			      0u));
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

/* One expected message: a BROADCAST_DATA whose byte [1] is `ctr`, or
 * (verdict != 0) a 0xF8 for the window starting at `ctr`. */
struct vd_want {
	uint8_t verdict;
	uint8_t ctr;
};

/*
 * The data and 0xF8 messages for channel `rx` from log index `from` on must
 * be exactly `want`, and the 0xF8 seq must count 0, 1, 2... - the first
 * verdict after the host's 0xF1 is 0, and none is lost here.
 */
static void vd_expect_ch(uint32_t from, uint8_t rx, const struct vd_want *want,
			 uint32_t n, uint8_t w)
{
	uint32_t got = 0u;
	uint8_t  seq = 0u;

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
			zassert_equal(want[got].ctr, d[2],
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
			zassert_equal(seq, d[5], "message %u: seq %u, want %u",
				      got, d[5], seq);
			seq++;
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
			missing += (uint8_t)(d[5] - (uint8_t)prev - 1u);
		}
		prev = d[5];
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
