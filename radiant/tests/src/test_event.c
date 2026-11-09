/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original work. Every expected byte here was written from
 * src/ant_wire.h (generated from protocol/ant_wire.yaml), from
 * tools/ant_verify.py's extended_fields() - which this module must be the
 * exact inverse of - and from the t_sync contract in
 * radiant/include/radiant/radiant_radio_hal.h, not from running
 * radiant_event.c and writing down what it did. Nothing here derives from
 * sdk-ant, from libant.a, from disassembly of any binary, or from any
 * ANT+ device profile document.
 *
 * The suite for radiant/src/radiant_event.c. Three things here have no
 * functional symptom when wrong, and most of the file is about them:
 *
 *   THE RX TIMESTAMP - reading the clock at drain time instead of the
 *   backend's t_sync passes every functional test and destroys the field's
 *   whole purpose (the sub-ms fusion claim the `timing` gate reads against).
 *   test_rx_timestamp_survives_the_clock_moving advances the clock over a
 *   second between arrival and drain and checks the reported ticks did not
 *   move - the single most important test here.
 *
 *   THE EXTENDED FIELD LAYOUT - field order and the flag byte are a Tier 1
 *   byte-diff, asserted byte by byte. test_ext_offsets_move_with_the_flags
 *   catches the decoder-shaped bug: omitting the channel id and checking
 *   RSSI moved up into its place.
 *
 *   THE CRC GATE - with eight filters armed the bench sees ~1.4 CRC failures
 *   a second on a quiet room, and Spike A measured a 3-byte address
 *   triggering the matcher on noise alone. A CRC-failed frame that becomes
 *   an event is a phantom sensor.
 *
 * native_sim does not build on Windows, so these run under Linux or on a DK;
 * see docs/testing.md and radiant/tests/testcase.yaml.
 */

#include <stdint.h>
#include <string.h>

#include <zephyr/sys/util.h>
#include <zephyr/ztest.h>

#include <radiant/radiant_event.h>
#include <radiant/radiant_frame.h>
#include "fake_radio.h"

/* ---------------------------------------------------------------------------
 * The port hooks, and the one definition of radiant_on_message() in this binary
 *
 * radiant_event.c includes no Zephyr header, so it needs these as symbols the
 * port supplies. The mock radio is single-threaded, so the critical section
 * is a no-op and the wakeup is a counter - the cheapest way to assert a post
 * tried to wake the drain.
 * ---------------------------------------------------------------------------
 */

static uint32_t n_wakeups;

unsigned int radiant_event_crit_enter(void)
{
	return 0u;
}

void radiant_event_crit_exit(unsigned int key)
{
	ARG_UNUSED(key);
}

void radiant_event_wakeup(void)
{
	n_wakeups++;
}

/*
 * The bridge's side of the inverted event path. In the firmware this is
 * src/ant_radio_radiant.c's one-line forward onto src/ant_serial_bridge.c's
 * antr_on_message(); in this binary radiant_on_message() itself is defined
 * here, and it is the only definition, so any future suite that wants
 * delivered messages should read this recorder rather than define a second
 * one.
 */
#define REC_MAX 192

/* Slots received data may use; the rest are reserved for the ownership/
 * outcome channel events (radiant_event.h). */
#define DATA_CAP (RADIANT_EVENT_QUEUE_DEPTH - RADIANT_EVENT_TERMINAL_RESERVE)

struct rec {
	uint8_t id;
	uint8_t len;
	uint8_t data[RADIANT_EVENT_MSG_MAX];
};

static struct rec recs[REC_MAX];
static uint32_t   n_recs;

void radiant_on_message(const struct radiant_msg *msg)
{
	uint8_t n;

	zassert_not_null(msg, "radiant_on_message() called with a null message");
	zassert_not_null(msg->data, "radiant_msg.data must never be null");
	zassert_true(msg->len <= RADIANT_EVENT_MSG_MAX,
		     "message longer than this module can build: %u", msg->len);

	if (n_recs < REC_MAX) {
		n = msg->len;
		recs[n_recs].id = msg->id;
		recs[n_recs].len = n;
		if (n > 0u) {
			memcpy(recs[n_recs].data, msg->data, n);
		}
	}
	n_recs++;
}

/* ---------------------------------------------------------------------------
 * Ground truth
 * ---------------------------------------------------------------------------
 */

/*
 * The frame Spike A pulled off the air (docs/spike-a-results.md), which is
 * also test_frame.c's strongest vector: a real ANT+ power meter with an
 * independently established channel ID.
 */
#define DEVNUM  14871u   /* 0x3A17 */
#define DTYPE   0x0Bu
#define TTYPE   5u

static const uint8_t payload8[RADIANT_FRAME_PAYLOAD_STD] = {
	0x10, 0xBD, 0xFF, 0x50, 0xDE, 0x11, 0x64, 0x00,
};

/* Where the extended tail starts in a received data message with a standard
 * 8-byte payload: one channel byte plus eight payload bytes. This is the
 * offset tools/ant_verify.py and tools/ant_scan.py both read the flag byte
 * from, spelled here as arithmetic so a reader can check it. */
#define FLAG_OFFSET  (1u + RADIANT_FRAME_PAYLOAD_STD)

/* ---------------------------------------------------------------------------
 * Suite plumbing
 * ---------------------------------------------------------------------------
 */

static void before_each(void *f)
{
	ARG_UNUSED(f);
	fake_radio_reset();
	radiant_event_init();
	n_recs = 0u;
	n_wakeups = 0u;
}

/*
 * The two habits fake_radio.h asks every suite to adopt: an operation left
 * armed, and anything a callback did that would deadlock a real radio ISR.
 * Both are end-of-test conditions, so they belong in the after hook rather
 * than repeated at the bottom of a dozen tests.
 */
static void after_each(void *f)
{
	ARG_UNUSED(f);
	zassert_true(fake_radio_is_idle(), "%s", fake_radio_busy_reason());
	zassert_equal(0u, fake_radio_viol_count(), "%s",
		      fake_radio_viol_name(fake_radio_viol(0)->code));
}

ZTEST_SUITE(radiant_event, NULL, NULL, before_each, after_each, NULL);

/* ---------------------------------------------------------------------------
 * A synthetic HAL event, for the tests that are about the queue rather than
 * about the radio
 * ---------------------------------------------------------------------------
 */

static struct radiant_rx_event mk_hal(enum radiant_radio_status status,
				  radiant_time_t t_sync, bool exact,
				  bool has_rssi, int8_t rssi_dbm)
{
	struct radiant_rx_event e;

	memset(&e, 0, sizeof(e));
	e.op = 1u;
	e.status = status;
	e.t_sync = t_sync;
	e.t_sync_exact = exact;
	e.has_rssi = has_rssi;
	e.rssi_dbm = rssi_dbm;
	e.body = payload8;
	e.body_len = (uint8_t)sizeof(payload8);
	return e;
}

static struct radiant_event_rx mk_rx(const struct radiant_rx_event *hal,
				 uint8_t channel, const uint8_t *payload)
{
	struct radiant_event_rx rx;

	memset(&rx, 0, sizeof(rx));
	rx.hal = hal;
	rx.msg_id = (uint8_t)RADIANT_WIRE_MESG_BROADCAST_DATA_ID;
	rx.channel = channel;
	rx.id.device_number = DEVNUM;
	rx.id.device_type = DTYPE;
	rx.id.trans_type = TTYPE;
	rx.payload = payload;
	rx.payload_len = (uint8_t)RADIANT_FRAME_PAYLOAD_STD;
	return rx;
}

/* ---------------------------------------------------------------------------
 * Driving a real receive through the mock radio
 * ---------------------------------------------------------------------------
 */

static int        cb_rc;        /* what radiant_event_post_rx() returned */
static radiant_time_t cb_t_sync;    /* the t_sync the callback was handed */
static uint32_t   cb_calls;

static void rx_cb(const struct radiant_rx_event *e, void *user)
{
	struct radiant_event_rx rx;

	ARG_UNUSED(user);

	if (e->status != RADIANT_RADIO_STATUS_OK &&
	    e->status != RADIANT_RADIO_STATUS_CRC_FAIL) {
		/* The window's terminal timeout. Not a frame. */
		return;
	}
	if (e->body_len <= RADIANT_FRAME_HDR_LEN_TRACKING) {
		return;
	}

	cb_calls++;
	cb_t_sync = e->t_sync;

	memset(&rx, 0, sizeof(rx));
	rx.hal = e;
	rx.msg_id = (uint8_t)RADIANT_WIRE_MESG_BROADCAST_DATA_ID;
	rx.channel = 0u;
	rx.id.device_number = DEVNUM;
	rx.id.device_type = DTYPE;
	rx.id.trans_type = TTYPE;
	/* A tracking body is [ttype][ctrl][d0..d7]: the payload starts after
	 * the two header bytes. The pointer is the mock's own buffer, which it
	 * overwrites with 0xDD the instant this callback returns - so passing
	 * it here is exactly the case the copy-in-the-ISR rule exists for. */
	rx.payload = e->body + RADIANT_FRAME_HDR_LEN_TRACKING;
	rx.payload_len = (uint8_t)(e->body_len - RADIANT_FRAME_HDR_LEN_TRACKING);

	cb_rc = radiant_event_post_rx(&rx);
}

static const struct radiant_radio_cbs cbs = { .rx = rx_cb, .tx = NULL };

/* One ANT+ broadcast, heard by one tracked window. Returns the t_sync the
 * frame was put on the air with, so a test can check the reported timestamp
 * against the transmitter's instant and not only against the receiver's. */
static radiant_time_t run_one_rx(bool crc_bad, uint32_t flags)
{
	uint8_t               frame[FAKE_RADIO_AIR_FRAME_MAX];
	struct fake_radio_air air;
	struct radiant_rx_filter  filt;
	struct radiant_rx_req     req;
	uint32_t              op = 0u;
	uint8_t               n;
	radiant_time_t            t;

	cb_rc = RADIANT_EVENT_ESTATE;
	cb_calls = 0u;

	n = fake_radio_build_ant_frame(frame, (uint16_t)DEVNUM, (uint8_t)DTYPE,
				       (uint8_t)TTYPE, payload8);

	zassert_ok(radiant_radio_init(&cbs, NULL));
	zassert_ok(radiant_radio_enable());

	t = radiant_radio_now() + 100000u;

	fake_radio_air_init(&air);
	air.t_sync = t;
	memcpy(air.bytes, frame, n);
	air.len = n;
	air.crc_bad = crc_bad;
	zassert_equal(RADIANT_RADIO_OK_RC, fake_radio_air_add(&air));

	memset(&filt, 0, sizeof(filt));
	memcpy(filt.addr, frame, RADIANT_FRAME_ADDR_LEN_TRACKING);
	filt.addr_len = (uint8_t)RADIANT_FRAME_ADDR_LEN_TRACKING;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = (uint8_t)RADIANT_RF_INDEX_ANT_PLUS;
	req.filters = &filt;
	req.n_filters = 1u;
	req.t_open = t - 500u;
	req.t_close = t + 500u;
	req.flags = flags;
	zassert_equal(RADIANT_RADIO_OK_RC, radiant_radio_rx(&req, &op));

	fake_radio_advance_to(req.t_close + 1u);
	zassert_equal(1u, cb_calls, "the window did not hear the frame");

	return t;
}

/* ---------------------------------------------------------------------------
 * The tick conversion, pinned against hand-computed values
 *
 * Independent of the implementation: ticks = floor(us * 32768 / 1000000), and
 * the field is 16 bits of a 32768 Hz counter so it wraps every two seconds.
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_event, test_rx_ticks_is_the_32768_hz_counter)
{
	zassert_equal(0u, radiant_event_rx_ticks(0u));

	/* 15625 us is exactly 512 ticks - the reduced ratio itself. */
	zassert_equal(512u, radiant_event_rx_ticks(15625u));

	/* One second is half the field. */
	zassert_equal(32768u, radiant_event_rx_ticks(1000000u));

	/* The last microsecond before the wrap, and the wrap. */
	zassert_equal(65535u, radiant_event_rx_ticks(1999999u));
	zassert_equal(0u, radiant_event_rx_ticks(2000000u));
	zassert_equal(32768u, radiant_event_rx_ticks(5000000u));
}

/*
 * The mock's clock starts one second below 2^32 microseconds precisely so that
 * a module storing a timestamp in a uint32_t fails in its first virtual
 * second. 2^32 us is 967,296 us past a two-second boundary, which is 31,696
 * ticks; a 32-bit truncation of the input would give 0.
 */
ZTEST(radiant_event, test_rx_ticks_is_not_a_uint32)
{
	zassert_equal(31696u, radiant_event_rx_ticks((radiant_time_t)0x100000000ULL));
	zassert_not_equal(radiant_event_rx_ticks((radiant_time_t)0x100000000ULL),
			  radiant_event_rx_ticks(0u));
}

/* ---------------------------------------------------------------------------
 * The extended tail, byte by byte
 * ---------------------------------------------------------------------------
 */

static struct radiant_event_ext mk_ext(void)
{
	struct radiant_event_ext ext;

	memset(&ext, 0, sizeof(ext));
	ext.id.device_number = (uint16_t)DEVNUM;
	/* Pairing bit set, so a test can see that it survives. */
	ext.id.device_type = (uint8_t)(DTYPE | RADIANT_DEVICE_TYPE_PAIRING_BIT);
	ext.id.trans_type = (uint8_t)TTYPE;
	ext.has_rssi = true;
	ext.rssi_dbm = FAKE_RADIO_RSSI_BENCH_DBM;   /* -17 */
	ext.has_t_sync = true;
	ext.t_sync_exact = true;
	ext.t_sync = 1000000u;                      /* exactly 0x8000 ticks */
	return ext;
}

ZTEST(radiant_event, test_ext_all_three_fields_in_order)
{
	struct radiant_event_ext ext = mk_ext();
	uint8_t              out[RADIANT_EVENT_EXT_MAX];
	int                  n;

	n = radiant_event_build_ext((uint8_t)RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS, &ext,
				out, sizeof(out));

	/* flag + 4 + 3 + 2 */
	zassert_equal(10, n, "0xE0 must produce ten bytes, got %d", n);

	zassert_equal(0xE0u, out[0], "flag byte");
	/* Channel id: device number little endian, then type, then trans. */
	zassert_equal(0x17u, out[1]);
	zassert_equal(0x3Au, out[2]);
	zassert_equal(0x8Bu, out[3], "the pairing bit must not be masked off");
	zassert_equal(0x05u, out[4]);
	/* RSSI: measurement type, value, threshold configuration. */
	zassert_equal((uint8_t)RADIANT_WIRE_RSSI_MEASUREMENT_TYPE_DBM, out[5]);
	zassert_equal(0xEFu, out[6], "-17 dBm as a signed byte");
	zassert_equal(0x00u, out[7]);
	/* Receive timestamp, little endian: 1,000,000 us is 0x8000 ticks. */
	zassert_equal(0x00u, out[8]);
	zassert_equal(0x80u, out[9]);
}

ZTEST(radiant_event, test_ext_device_id_only)
{
	struct radiant_event_ext ext = mk_ext();
	uint8_t              out[RADIANT_EVENT_EXT_MAX];
	int                  n;

	n = radiant_event_build_ext(
		(uint8_t)RADIANT_WIRE_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID, &ext, out,
		sizeof(out));

	zassert_equal(5, n);
	zassert_equal((uint8_t)RADIANT_WIRE_EXT_FLAG_CHANNEL_ID, out[0]);
	zassert_equal(0x17u, out[1]);
	zassert_equal(0x3Au, out[2]);
}

/* The decoder-shaped bug, made reproducible: ask for RSSI and the timestamp
 * but not the channel id, so every field must shift up by four with the flag
 * byte saying so. A module writing fields at fixed offsets passes the 0xE0
 * test above and fails this one. */
ZTEST(radiant_event, test_ext_offsets_move_with_the_flags)
{
	struct radiant_event_ext ext = mk_ext();
	uint8_t              out[RADIANT_EVENT_EXT_MAX];
	int                  n;
	uint8_t              cfg;

	cfg = (uint8_t)(RADIANT_WIRE_LIB_CONFIG_MESG_OUT_INC_RSSI |
			RADIANT_WIRE_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP);

	n = radiant_event_build_ext(cfg, &ext, out, sizeof(out));

	zassert_equal(6, n);
	zassert_equal((uint8_t)(RADIANT_WIRE_EXT_FLAG_RSSI | RADIANT_WIRE_EXT_FLAG_RX_TIMESTAMP),
		      out[0]);
	zassert_equal((uint8_t)RADIANT_WIRE_RSSI_MEASUREMENT_TYPE_DBM, out[1],
		      "RSSI must start where the channel id would have been");
	zassert_equal(0xEFu, out[2]);
	zassert_equal(0x00u, out[3]);
	zassert_equal(0x00u, out[4]);
	zassert_equal(0x80u, out[5]);
}

ZTEST(radiant_event, test_ext_absent_when_nothing_is_configured)
{
	struct radiant_event_ext ext = mk_ext();
	uint8_t              out[RADIANT_EVENT_EXT_MAX];

	/* Not a flag byte of zero - no flag byte at all. That is what keeps a
	 * plain broadcast nine bytes long on the wire. */
	zassert_equal(0, radiant_event_build_ext(0u, &ext, out, sizeof(out)));
}

/* ---------------------------------------------------------------------------
 * Degrading rather than lying
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_event, test_inexact_timestamp_downgrades_rather_than_lies)
{
	struct radiant_event_ext ext = mk_ext();
	uint8_t              out[RADIANT_EVENT_EXT_MAX];
	int                  n;

	ext.t_sync_exact = false;

	n = radiant_event_build_ext((uint8_t)RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS, &ext,
				out, sizeof(out));

	/* The field is gone and the flag bit went with it: the host is told
	 * there is no timestamp, rather than handed a worse one in a field it
	 * would read as radio-clock accurate. */
	zassert_equal(8, n);
	zassert_equal((uint8_t)(RADIANT_WIRE_EXT_FLAG_CHANNEL_ID | RADIANT_WIRE_EXT_FLAG_RSSI),
		      out[0]);
	zassert_equal(0u, out[0] & (uint8_t)RADIANT_WIRE_EXT_FLAG_RX_TIMESTAMP);
	zassert_equal(1u, radiant_event_stats_get()->ts_suppressed,
		      "a downgrade must be counted, not silent");

	/* A bench build may opt into the approximate value explicitly. */
	radiant_event_set_ts_policy(RADIANT_EVENT_TS_BEST_EFFORT);
	n = radiant_event_build_ext((uint8_t)RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS, &ext,
				out, sizeof(out));
	zassert_equal(10, n);
	zassert_equal(0xE0u, out[0]);
	zassert_equal(1u, radiant_event_stats_get()->ts_suppressed,
		      "the opt-in must not count as a downgrade");
}

ZTEST(radiant_event, test_missing_rssi_downgrades_rather_than_reporting_zero)
{
	struct radiant_event_ext ext = mk_ext();
	uint8_t              out[RADIANT_EVENT_EXT_MAX];
	int                  n;

	ext.has_rssi = false;

	n = radiant_event_build_ext((uint8_t)RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS, &ext,
				out, sizeof(out));

	/* 0 dBm is a physically implausible reading that a host would
	 * nonetheless believe, so the field is dropped instead. */
	zassert_equal(7, n);
	zassert_equal((uint8_t)(RADIANT_WIRE_EXT_FLAG_CHANNEL_ID |
				RADIANT_WIRE_EXT_FLAG_RX_TIMESTAMP),
		      out[0]);
	zassert_equal(1u, radiant_event_stats_get()->rssi_suppressed);
}

/* ---------------------------------------------------------------------------
 * The test this module exists to pass
 * ---------------------------------------------------------------------------
 */

/* The timestamp must be the instant the frame's address was at the antenna,
 * not whatever the clock reads at drain. Moving the clock by over a second
 * between the two makes a drain-time reading unmissably wrong (~40,448 ticks
 * off), which is why the assertion is against the transmitter's t_sync
 * rather than anything the receiver produced. */
ZTEST(radiant_event, test_rx_timestamp_survives_the_clock_moving)
{
	radiant_time_t t_air;
	uint16_t   reported;
	uint16_t   at_drain;

	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_set(
			      (uint8_t)RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS));

	t_air = run_one_rx(false, 0u);
	zassert_equal(RADIANT_EVENT_OK, cb_rc);
	zassert_equal(1u, radiant_event_queued());
	zassert_equal(1u, n_wakeups, "a queued message must wake the drain");
	zassert_equal(0u, n_recs, "nothing may be delivered from the ISR");

	/* Between arrival and drain, a great deal of time. */
	fake_radio_advance(1234567u);
	at_drain = radiant_event_rx_ticks(radiant_radio_now());

	zassert_equal(1u, radiant_event_drain(0u));
	zassert_equal(1u, n_recs);

	zassert_equal((uint8_t)RADIANT_WIRE_MESG_BROADCAST_DATA_ID, recs[0].id);
	/* channel + 8 payload + flag + 4 + 3 + 2 */
	zassert_equal(19u, recs[0].len);
	zassert_equal(0xE0u, recs[0].data[FLAG_OFFSET]);

	reported = (uint16_t)(recs[0].data[17] |
			      ((uint16_t)recs[0].data[18] << 8));

	zassert_equal(radiant_event_rx_ticks(t_air), reported,
		      "the reported timestamp is not the frame's t_sync");
	zassert_equal(radiant_event_rx_ticks(cb_t_sync), reported,
		      "the reported timestamp is not the one the HAL gave");
	zassert_not_equal(at_drain, reported,
			  "the timestamp moved with the clock - it was read at "
			  "drain time instead of taken from t_sync");
}

/*
 * The body is a DMA buffer the backend reuses the instant the callback
 * returns; the mock overwrites it with 0xDD to make that reproducible. So the
 * payload delivered after the drain must be the eight bytes that were on the
 * air, not eight copies of the poison.
 */
ZTEST(radiant_event, test_body_is_copied_in_the_callback_not_retained)
{
	uint8_t i;

	(void)run_one_rx(false, 0u);
	zassert_equal(RADIANT_EVENT_OK, cb_rc);

	fake_radio_advance(50000u);
	zassert_equal(1u, radiant_event_drain(0u));
	zassert_equal(1u, n_recs);

	zassert_equal(9u, recs[0].len, "no lib config, so no extended tail");
	zassert_equal(0u, recs[0].data[0], "channel byte");
	for (i = 0u; i < (uint8_t)RADIANT_FRAME_PAYLOAD_STD; i++) {
		zassert_equal(payload8[i], recs[0].data[1u + i],
			      "payload byte %u is 0x%02X, not 0x%02X - the "
			      "body was retained rather than copied",
			      i, recs[0].data[1u + i], payload8[i]);
		zassert_not_equal(FAKE_RADIO_BODY_POISON, recs[0].data[1u + i]);
	}
}

/* A CRC-failed frame must never become an event. It is refused at the last
 * gate before the host as well as by whatever decided to offer it. */
ZTEST(radiant_event, test_crc_failed_frame_produces_no_event)
{
	(void)run_one_rx(true, RADIANT_RX_REPORT_CRC_FAIL);

	zassert_equal(RADIANT_EVENT_ECRC, cb_rc,
		      "a CRC_FAIL event must be refused, not queued");
	zassert_equal(0u, radiant_event_queued());
	zassert_false(radiant_event_pending());
	zassert_equal(0u, radiant_event_drain(0u));
	zassert_equal(0u, n_recs);
	zassert_equal(0u, n_wakeups, "a refusal must not wake the drain");
	zassert_equal(1u, radiant_event_stats_get()->rejected_crc);
	zassert_equal(0u, radiant_event_stats_get()->posted);
}

/* The same refusal for a synthetic event, and for the statuses that carry no
 * frame at all - so that a caller which forwards a terminal event by mistake
 * cannot manufacture a message out of a zeroed timestamp and a NULL body. */
ZTEST(radiant_event, test_non_ok_statuses_are_all_refused)
{
	struct radiant_rx_event  hal;
	struct radiant_event_rx  rx;

	hal = mk_hal(RADIANT_RADIO_STATUS_CRC_FAIL, 1000000u, true, true, -50);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(RADIANT_EVENT_ECRC, radiant_event_post_rx(&rx));

	hal = mk_hal(RADIANT_RADIO_STATUS_TIMEOUT, 0u, false, false, 0);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(RADIANT_EVENT_EINVAL, radiant_event_post_rx(&rx));

	hal = mk_hal(RADIANT_RADIO_STATUS_ABORTED, 0u, false, false, 0);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(RADIANT_EVENT_EINVAL, radiant_event_post_rx(&rx));

	hal = mk_hal(RADIANT_RADIO_STATUS_FAILED, 0u, false, false, 0);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(RADIANT_EVENT_EINVAL, radiant_event_post_rx(&rx));

	zassert_equal(0u, radiant_event_queued());
	zassert_equal(0u, radiant_event_stats_get()->posted);
}

/*
 * A backend that cannot capture the address event clears t_sync_exact on every
 * event, and the whole path - not just radiant_event_build_ext() - has to degrade.
 */
ZTEST(radiant_event, test_inexact_backend_degrades_the_whole_path)
{
	fake_radio_caps_mut()->has_sync_timestamp = false;

	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_set(
			      (uint8_t)RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS));

	(void)run_one_rx(false, 0u);
	zassert_equal(RADIANT_EVENT_OK, cb_rc);

	zassert_equal(1u, radiant_event_drain(0u));
	/* channel + 8 payload + flag + 4 + 3, and no timestamp. */
	zassert_equal(17u, recs[0].len);
	zassert_equal((uint8_t)(RADIANT_WIRE_EXT_FLAG_CHANNEL_ID | RADIANT_WIRE_EXT_FLAG_RSSI),
		      recs[0].data[FLAG_OFFSET]);
	zassert_equal(1u, radiant_event_stats_get()->ts_suppressed);
}

/* ---------------------------------------------------------------------------
 * The queue at 32 channels
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_event, test_thirty_two_channels_neither_overflow_nor_reorder)
{
	struct radiant_rx_event hal;
	struct radiant_event_rx rx;
	uint8_t             payload[RADIANT_FRAME_PAYLOAD_STD];
	uint8_t             ch;
	uint32_t            i;

	zassert_true(RADIANT_EVENT_CHANNEL_MAX >= 31u);

	memcpy(payload, payload8, sizeof(payload));

	for (ch = 0u; ch <= 31u; ch++) {
		/* A distinct t_sync and a distinct first payload byte per
		 * channel, so a reordering is visible in two independent
		 * ways. */
		hal = mk_hal(RADIANT_RADIO_STATUS_OK,
			     (radiant_time_t)1000000u + (radiant_time_t)ch, true, true,
			     (int8_t)(-40 - (int8_t)ch));
		payload[0] = ch;
		rx = mk_rx(&hal, ch, payload);
		zassert_equal(RADIANT_EVENT_OK, radiant_event_post_rx(&rx),
			      "channel %u was refused", ch);
	}

	zassert_equal(32u, radiant_event_queued());
	zassert_equal(32u, n_wakeups);
	zassert_equal(0u, radiant_event_stats_get()->dropped_full);

	zassert_equal(32u, radiant_event_drain(0u));
	zassert_equal(32u, n_recs);
	zassert_false(radiant_event_pending());

	for (i = 0u; i < 32u; i++) {
		zassert_equal((uint8_t)RADIANT_WIRE_MESG_BROADCAST_DATA_ID,
			      recs[i].id);
		zassert_equal(9u, recs[i].len);
		zassert_equal((uint8_t)i, recs[i].data[0],
			      "message %u came out of order", i);
		zassert_equal((uint8_t)i, recs[i].data[1]);
	}

	zassert_equal(32u, radiant_event_stats_get()->delivered);
	zassert_equal(32u, radiant_event_stats_get()->high_water);
}

/*
 * A full ring drops the newest message - the queued ones are already in order
 * and losing the head of a burst is worse than losing its tail - and says so
 * with RADIANT_WIRE_EVENT_QUE_OVERFLOW. Never a silent loss: "unexplained loss must be
 * 0" is one of the A/B gates, and a message that vanished without an event is
 * exactly what makes that figure meaningless.
 */
ZTEST(radiant_event, test_overflow_is_reported_never_silent)
{
	struct radiant_rx_event hal;
	struct radiant_event_rx rx;
	uint8_t             payload[RADIANT_FRAME_PAYLOAD_STD];
	uint32_t            i;

	memcpy(payload, payload8, sizeof(payload));
	hal = mk_hal(RADIANT_RADIO_STATUS_OK, 1000000u, true, true, -40);

	/* Received data stops at DEPTH - RESERVE: the top slots are kept for
	 * the ownership/outcome channel events (see the test below). */
	for (i = 0u; i < (uint32_t)DATA_CAP; i++) {
		payload[0] = (uint8_t)i;
		rx = mk_rx(&hal, (uint8_t)(i & RADIANT_EVENT_CHANNEL_MAX), payload);
		zassert_equal(RADIANT_EVENT_OK, radiant_event_post_rx(&rx));
	}

	for (i = 0u; i < 3u; i++) {
		payload[0] = 0xFFu;
		rx = mk_rx(&hal, 0u, payload);
		zassert_equal(RADIANT_EVENT_ENOSPC, radiant_event_post_rx(&rx));
	}

	zassert_equal(3u, radiant_event_stats_get()->dropped_full);
	zassert_equal((uint32_t)DATA_CAP, n_wakeups,
		      "a dropped message must not wake the drain");
	zassert_true(radiant_event_pending());

	zassert_equal((uint32_t)DATA_CAP + 1u, radiant_event_drain(0u));

	/* The marker comes first and is a channel event on channel 0. */
	zassert_equal((uint8_t)RADIANT_WIRE_MESG_RESPONSE_EVENT_ID, recs[0].id);
	zassert_equal(3u, recs[0].len);
	zassert_equal(0u, recs[0].data[0]);
	zassert_equal((uint8_t)RADIANT_WIRE_MESG_EVENT_ID, recs[0].data[1]);
	zassert_equal((uint8_t)RADIANT_WIRE_EVENT_QUE_OVERFLOW, recs[0].data[2]);
	zassert_equal(1u, radiant_event_stats_get()->overflow_marks);

	/* Everything that was accepted still came out in order, and the three
	 * that were refused are not among them. */
	for (i = 0u; i < (uint32_t)DATA_CAP; i++) {
		zassert_equal((uint8_t)i, recs[i + 1u].data[1],
		              "queued message %u is missing or out of order", i);
	}

	zassert_false(radiant_event_pending());
}

/*
 * A ring full of received data still carries the four channel events that
 * are owed exactly once and never re-posted: TRANSFER_NEXT_DATA_BLOCK,
 * TRANSFER_TX_COMPLETED/FAILED and CHANNEL_CLOSED (review 2026-10-31, A11-1 /
 * O2 SF-2). Losing one wedges a host's burst semaphore or its close wait;
 * losing a broadcast costs one period. RX_FAIL is NOT reserved - it is
 * per-slot rate and would let a stall exhaust the reserve.
 */
ZTEST(radiant_event, test_a_full_ring_still_carries_ownership_events)
{
	struct radiant_rx_event hal;
	struct radiant_event_rx rx;
	uint8_t             payload[RADIANT_FRAME_PAYLOAD_STD];
	uint32_t            i;
	uint32_t            n;

	memcpy(payload, payload8, sizeof(payload));
	hal = mk_hal(RADIANT_RADIO_STATUS_OK, 1000000u, true, true, -40);

	/* Data until the first refusal: exactly DEPTH - RESERVE accepted. */
	for (i = 0u; i < (uint32_t)RADIANT_EVENT_QUEUE_DEPTH; i++) {
		payload[0] = (uint8_t)i;
		rx = mk_rx(&hal, (uint8_t)(i & RADIANT_EVENT_CHANNEL_MAX), payload);
		if (radiant_event_post_rx(&rx) != RADIANT_EVENT_OK) {
			break;
		}
	}
	zassert_equal((uint32_t)DATA_CAP, i,
		      "received data took %u slots, the cap is %u", i,
		      (unsigned int)DATA_CAP);

	zassert_equal(RADIANT_EVENT_OK, radiant_event_post_transfer_tx_failed(5u),
		      "a TX_FAILED was dropped by data pressure");
	zassert_equal(RADIANT_EVENT_OK,
		      radiant_event_post_channel_event(
			      5u, (uint8_t)RADIANT_WIRE_EVENT_CHANNEL_CLOSED),
		      "a CHANNEL_CLOSED was dropped by data pressure");
	zassert_equal(RADIANT_EVENT_OK, radiant_event_post_transfer_next_block(4u));
	zassert_equal(RADIANT_EVENT_OK,
		      radiant_event_post_transfer_tx_completed(4u));

	zassert_equal(RADIANT_EVENT_ENOSPC,
		      radiant_event_post_channel_event(
			      5u, (uint8_t)RADIANT_WIRE_EVENT_RX_FAIL),
		      "RX_FAIL must not use the reserve");
	payload[0] = 0xFFu;
	rx = mk_rx(&hal, 0u, payload);
	zassert_equal(RADIANT_EVENT_ENOSPC, radiant_event_post_rx(&rx));

	n = radiant_event_drain(0u);
	zassert_equal((uint32_t)DATA_CAP + 4u + 1u, n);
	zassert_equal((uint8_t)RADIANT_WIRE_EVENT_QUE_OVERFLOW, recs[0].data[2],
		      "the overflow marker must still come first");
	zassert_equal((uint8_t)RADIANT_WIRE_EVENT_TRANSFER_TX_FAILED,
		      recs[n - 4u].data[2]);
	zassert_equal((uint8_t)RADIANT_WIRE_EVENT_CHANNEL_CLOSED,
		      recs[n - 3u].data[2]);
	zassert_equal((uint8_t)RADIANT_WIRE_EVENT_TRANSFER_NEXT_DATA_BLOCK,
		      recs[n - 2u].data[2]);
	zassert_equal((uint8_t)RADIANT_WIRE_EVENT_TRANSFER_TX_COMPLETED,
		      recs[n - 1u].data[2]);
	zassert_equal(1u, radiant_event_stats_get()->tx_failed);
	zassert_equal(1u, radiant_event_stats_get()->tx_completed);
	zassert_equal(1u, radiant_event_stats_get()->next_data_block);

	/* The reserve is a bound, not a promise: fill it with terminals and the
	 * next one is refused, counted, not silently lost. */
	for (i = 0u; i < (uint32_t)DATA_CAP; i++) {
		payload[0] = (uint8_t)i;
		rx = mk_rx(&hal, 0u, payload);
		zassert_equal(RADIANT_EVENT_OK, radiant_event_post_rx(&rx));
	}
	for (i = 0u; i < (uint32_t)RADIANT_EVENT_TERMINAL_RESERVE; i++) {
		zassert_equal(RADIANT_EVENT_OK,
			      radiant_event_post_transfer_tx_failed(1u));
	}
	zassert_equal(RADIANT_EVENT_ENOSPC,
		      radiant_event_post_transfer_tx_failed(1u),
		      "the reserve is not bounded");
	(void)radiant_event_drain(0u);
}

/* ---------------------------------------------------------------------------
 * Channel events, including the three that release the bridge's burst block
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_event, test_channel_event_shape)
{
	zassert_equal(RADIANT_EVENT_OK,
		      radiant_event_post_channel_event(
			      7u, (uint8_t)RADIANT_WIRE_EVENT_CHANNEL_CLOSED));
	zassert_equal(1u, radiant_event_drain(0u));

	/* [channel][MESG_EVENT_ID][code] under MESG_RESPONSE_EVENT_ID. Byte 1
	 * being 0x01 is what tells the bridge this is an unsolicited event and
	 * not a reply to a command. */
	zassert_equal((uint8_t)RADIANT_WIRE_MESG_RESPONSE_EVENT_ID, recs[0].id);
	zassert_equal((uint8_t)RADIANT_WIRE_MESG_RESPONSE_EVENT_SIZE, recs[0].len);
	zassert_equal(7u, recs[0].data[0]);
	zassert_equal((uint8_t)RADIANT_WIRE_MESG_EVENT_ID, recs[0].data[1]);
	zassert_equal((uint8_t)RADIANT_WIRE_EVENT_CHANNEL_CLOSED, recs[0].data[2]);

	/* A channel event carries no extended fields whatever the library
	 * configuration says - they belong to received data only. */
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_set(
			      (uint8_t)RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS));
	zassert_equal(RADIANT_EVENT_OK,
		      radiant_event_post_channel_event(
			      7u, (uint8_t)RADIANT_WIRE_EVENT_RX_FAIL));
	zassert_equal(1u, radiant_event_drain(0u));
	zassert_equal(3u, recs[1].len);
}

/*
 * The three release codes. If NEXT_DATA_BLOCK is not raised exactly once per
 * accepted block the bridge's k_sem_take() waits its full 1000 ms and then
 * answers RADIANT_WIRE_TRANSFER_IN_PROGRESS, so a burst stalls a second a packet and
 * reports a plausible-looking error. Counting them here is what lets
 * radiant_burst.c's own suite assert release-count == accepted-block-count against
 * a number instead of a log.
 */
ZTEST(radiant_event, test_burst_release_codes_are_the_wire_values)
{
	const struct radiant_event_stats *st;

	zassert_equal(RADIANT_EVENT_OK, radiant_event_post_transfer_next_block(4u));
	zassert_equal(RADIANT_EVENT_OK, radiant_event_post_transfer_next_block(4u));
	zassert_equal(RADIANT_EVENT_OK, radiant_event_post_transfer_tx_completed(4u));
	zassert_equal(RADIANT_EVENT_OK, radiant_event_post_transfer_tx_failed(5u));

	zassert_equal(4u, radiant_event_drain(0u));

	zassert_equal((uint8_t)RADIANT_WIRE_EVENT_TRANSFER_NEXT_DATA_BLOCK,
		      recs[0].data[2]);
	zassert_equal((uint8_t)RADIANT_WIRE_EVENT_TRANSFER_NEXT_DATA_BLOCK,
		      recs[1].data[2]);
	zassert_equal((uint8_t)RADIANT_WIRE_EVENT_TRANSFER_TX_COMPLETED,
		      recs[2].data[2]);
	zassert_equal((uint8_t)RADIANT_WIRE_EVENT_TRANSFER_TX_FAILED, recs[3].data[2]);
	zassert_equal(5u, recs[3].data[0]);

	st = radiant_event_stats_get();
	zassert_equal(2u, st->next_data_block);
	zassert_equal(1u, st->tx_completed);
	zassert_equal(1u, st->tx_failed);
}

ZTEST(radiant_event, test_burst_data_uses_the_serial_burst_header)
{
	struct radiant_rx_event hal;
	struct radiant_event_rx rx;

	hal = mk_hal(RADIANT_RADIO_STATUS_OK, 1000000u, true, true, -40);
	rx = mk_rx(&hal, 9u, payload8);
	rx.msg_id = (uint8_t)RADIANT_WIRE_MESG_BURST_DATA_ID;
	rx.burst_seq = 2u;
	rx.burst_last = true;

	zassert_equal(RADIANT_EVENT_OK, radiant_event_post_rx(&rx));
	zassert_equal(1u, radiant_event_drain(0u));

	/* [last<<7 | seq<<5 | channel] = 0x80 | 0x40 | 0x09. */
	zassert_equal((uint8_t)RADIANT_WIRE_MESG_BURST_DATA_ID, recs[0].id);
	zassert_equal(0xC9u, recs[0].data[0]);

	/* A sequence number wider than the header's two bits is a caller bug,
	 * not something to truncate into the channel field. */
	rx.burst_seq = 4u;
	zassert_equal(RADIANT_EVENT_EINVAL, radiant_event_post_rx(&rx));
}

/* ---------------------------------------------------------------------------
 * Library configuration and the reset path
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_event, test_lib_config_is_additive_and_refuses_unknown_bits)
{
	uint8_t cfg = 0xFFu;

	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_get(&cfg));
	zassert_equal(0u, cfg, "library configuration starts cleared");

	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_set(
			      (uint8_t)RADIANT_WIRE_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID));
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_set(
			      (uint8_t)RADIANT_WIRE_LIB_CONFIG_MESG_OUT_INC_RSSI));
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_get(&cfg));
	zassert_equal(0xC0u, cfg, "setting one bit must not clear another");

	/* A bit this module does not implement is refused outright, and
	 * nothing is applied - a half-applied configuration is worse than an
	 * error, because the host cannot tell which half took. */
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_INVALID_PARAMETER_PROVIDED,
		      radiant_event_lib_config_set(0x02u));
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_get(&cfg));
	zassert_equal(0xC0u, cfg);

	/* 0xE0 - what every tool asks for - must be accepted whole. */
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_set(
			      (uint8_t)RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS));
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_get(&cfg));
	zassert_equal(0xE0u, cfg);

	/* The clear path takes the mask the bridge sends for "set it to
	 * zero"; there is no separate clear message on the wire. */
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_clear(
			      (uint8_t)RADIANT_WIRE_LIB_CONFIG_MASK_ALL));
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_get(&cfg));
	zassert_equal(0u, cfg);
}

/*
 * The event filter (MESG_EVENT_FILTER_CONFIG). Bit N suppresses channel event
 * code N+1 on its way to the host - ANT Message Protocol and Usage Rev 5.1,
 * 9.5.2.28 / 9.5.6.1; its own example filters EVENT_TX (3) with 0x04.
 *
 * Written against the document's numbers, not against the module's table, so
 * the module and the test cannot share a mistake: each row is code, bit.
 */
static const struct {
	uint8_t  code;
	uint16_t bit;
} filter_map[] = {
	{ 0x01u, 0x0001u }, /* EVENT_RX_SEARCH_TIMEOUT */
	{ 0x02u, 0x0002u }, /* EVENT_RX_FAIL */
	{ 0x03u, 0x0004u }, /* EVENT_TX (the document's own example) */
	{ 0x04u, 0x0008u }, /* EVENT_TRANSFER_RX_FAILED */
	{ 0x05u, 0x0010u }, /* EVENT_TRANSFER_TX_COMPLETED */
	{ 0x06u, 0x0020u }, /* EVENT_TRANSFER_TX_FAILED */
	{ 0x07u, 0x0040u }, /* EVENT_CHANNEL_CLOSED */
	{ 0x08u, 0x0080u }, /* EVENT_RX_FAIL_GO_TO_SEARCH */
	{ 0x09u, 0x0100u }, /* EVENT_CHANNEL_COLLISION */
	{ 0x0Au, 0x0200u }, /* EVENT_TRANSFER_TX_START */
};
#define N_MAP  (sizeof(filter_map) / sizeof(filter_map[0]))

/* Post one of each code in the table, channel 0. */
static void post_every_mapped_event(void)
{
	for (size_t i = 0u; i < N_MAP; i++) {
		zassert_equal(RADIANT_EVENT_OK,
			      radiant_event_post_channel_event(0u, filter_map[i].code));
	}
}

/* How many delivered messages are the channel event `code`. */
static uint32_t delivered_code(uint8_t code)
{
	uint32_t n = 0u;

	for (uint32_t i = 0u; i < n_recs && i < REC_MAX; i++) {
		if (recs[i].id == (uint8_t)RADIANT_WIRE_MESG_RESPONSE_EVENT_ID &&
		    recs[i].len >= 3u &&
		    recs[i].data[1] == (uint8_t)RADIANT_WIRE_MESG_EVENT_ID &&
		    recs[i].data[2] == code) {
			n++;
		}
	}
	return n;
}

ZTEST(radiant_event, test_the_filter_round_trips)
{
	uint16_t f = 0u;

	/* tools/ant_features.py round-trips this whole. */
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_filter_set(0xFFFFu));
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_filter_get(&f));
	zassert_equal(0xFFFFu, f);
}

ZTEST(radiant_event, test_a_default_filter_of_zero_passes_every_event)
{
	struct radiant_rx_event hal;
	struct radiant_event_rx rx;
	uint16_t f = 0xFFFFu;

	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_filter_get(&f));
	zassert_equal(0u, f, "the default filter is not 0");

	post_every_mapped_event();
	zassert_equal(RADIANT_EVENT_OK,
		      radiant_event_post_channel_event(
			      0u, (uint8_t)RADIANT_WIRE_EVENT_TRANSFER_NEXT_DATA_BLOCK));
	hal = mk_hal(RADIANT_RADIO_STATUS_OK, 1000000u, true, true, -40);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(RADIANT_EVENT_OK, radiant_event_post_rx(&rx));

	zassert_equal((uint32_t)N_MAP + 2u, radiant_event_drain(0u));
	for (size_t i = 0u; i < N_MAP; i++) {
		zassert_equal(1u, delivered_code(filter_map[i].code),
			      "event %u did not reach the host with no filter set",
			      (unsigned int)filter_map[i].code);
	}
	zassert_equal(0u, radiant_event_stats_get()->filtered);
}

/* Each bit takes out its own event and nothing else. Codes 5 and 6 are the two
 * burst-release codes: still handed to the bridge (which frees its buffer and
 * keeps them off the wire), so their bits are checked in the next test. */
ZTEST(radiant_event, test_each_filter_bit_suppresses_exactly_its_event)
{
	for (size_t i = 0u; i < N_MAP; i++) {
		if (filter_map[i].code == 0x05u || filter_map[i].code == 0x06u) {
			continue;
		}
		n_recs = 0u;
		zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
			      radiant_event_filter_set(filter_map[i].bit));
		post_every_mapped_event();
		zassert_equal((uint32_t)N_MAP - 1u, radiant_event_drain(0u),
			      "bit 0x%04x filtered the wrong number of events",
			      (unsigned int)filter_map[i].bit);
		for (size_t j = 0u; j < N_MAP; j++) {
			zassert_equal(i == j ? 0u : 1u, delivered_code(filter_map[j].code),
				      "bit 0x%04x: event %u delivered %u time(s)",
				      (unsigned int)filter_map[i].bit,
				      (unsigned int)filter_map[j].code,
				      (unsigned int)delivered_code(filter_map[j].code));
		}
	}
}

/* The two burst-release codes carry buffer ownership for the bridge, so a
 * filter naming them still delivers them to radiant_on_message(); the bridge
 * drops them after the release. Counted as filtered all the same. */
ZTEST(radiant_event, test_the_burst_release_codes_still_reach_the_bridge_when_filtered)
{
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_filter_set(0x0010u | 0x0020u));
	zassert_equal(RADIANT_EVENT_OK,
		      radiant_event_post_channel_event(
			      0u, (uint8_t)RADIANT_WIRE_EVENT_TRANSFER_TX_COMPLETED));
	zassert_equal(RADIANT_EVENT_OK,
		      radiant_event_post_channel_event(
			      0u, (uint8_t)RADIANT_WIRE_EVENT_TRANSFER_TX_FAILED));
	zassert_equal(2u, radiant_event_drain(0u));
	zassert_equal(1u, delivered_code(0x05u));
	zassert_equal(1u, delivered_code(0x06u));
	zassert_equal(2u, radiant_event_stats_get()->filtered);
}

/* A full filter must not touch anything that is not a filterable event: data,
 * command replies, and event codes with no bit. */
ZTEST(radiant_event, test_a_full_filter_leaves_data_replies_and_unmapped_codes_alone)
{
	struct radiant_rx_event hal;
	struct radiant_event_rx rx;
	uint8_t reply[3] = { 0u, 0x42u, 0x00u }; /* a command's reply: byte 1 != 1 */

	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_filter_set(0xFFFFu));
	hal = mk_hal(RADIANT_RADIO_STATUS_OK, 1000000u, true, true, -40);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(RADIANT_EVENT_OK, radiant_event_post_rx(&rx));
	zassert_equal(RADIANT_EVENT_OK,
		      radiant_event_post_raw(
			      (uint8_t)RADIANT_WIRE_MESG_RESPONSE_EVENT_ID, reply,
			      (uint8_t)sizeof(reply)));
	zassert_equal(RADIANT_EVENT_OK,
		      radiant_event_post_channel_event(
			      0u, (uint8_t)RADIANT_WIRE_EVENT_TRANSFER_NEXT_DATA_BLOCK));
	/* 0x11 is above bit 15's reach (code 16): never filterable. */
	zassert_equal(3u, radiant_event_drain(0u));
	zassert_equal(1u, delivered_code((uint8_t)RADIANT_WIRE_EVENT_TRANSFER_NEXT_DATA_BLOCK));
	zassert_equal(0u, radiant_event_stats_get()->filtered);
}

/*
 * The point of filtering at delivery: everything counted when an event is
 * POSTED - the ring, the release-code counters - has already seen an event the
 * host later asked not to receive, so loss accounting still sees RX_FAIL.
 */
ZTEST(radiant_event, test_a_filtered_event_is_still_counted_when_posted)
{
	const struct radiant_event_stats *st = radiant_event_stats_get();

	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_filter_set(0x0002u | 0x0010u));
	for (unsigned int i = 0u; i < 3u; i++) {
		zassert_equal(RADIANT_EVENT_OK,
			      radiant_event_post_channel_event(
				      0u, (uint8_t)RADIANT_WIRE_EVENT_RX_FAIL));
	}
	zassert_equal(RADIANT_EVENT_OK,
		      radiant_event_post_channel_event(
			      0u, (uint8_t)RADIANT_WIRE_EVENT_TRANSFER_TX_COMPLETED));

	/* Counted on the way in, before anything is delivered. */
	zassert_equal(4u, st->posted);
	zassert_equal(1u, st->tx_completed);
	zassert_equal(4u, radiant_event_queued());

	(void)radiant_event_drain(0u);
	zassert_equal(0u, delivered_code((uint8_t)RADIANT_WIRE_EVENT_RX_FAIL),
		      "the host received an RX_FAIL it filtered");
	zassert_equal(4u, st->filtered);
	zassert_equal(0u, radiant_event_queued(), "a filtered event left on the ring");
}

/*
 * antr_stack_reset() discards rather than drains. Draining would call
 * antr_on_message() recursively from inside an antr_* call, which
 * docs/sdk-ant-contract.md forbids, and a real stick emits nothing between the
 * reset command and the startup message anyway.
 */
ZTEST(radiant_event, test_flush_discards_without_delivering)
{
	struct radiant_rx_event hal;
	struct radiant_event_rx rx;
	uint8_t             cfg = 0xFFu;

	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_set(
			      (uint8_t)RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS));

	hal = mk_hal(RADIANT_RADIO_STATUS_OK, 1000000u, true, true, -40);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(RADIANT_EVENT_OK, radiant_event_post_rx(&rx));
	zassert_equal(RADIANT_EVENT_OK, radiant_event_post_rx(&rx));

	radiant_event_flush();

	zassert_equal(0u, radiant_event_queued());
	zassert_false(radiant_event_pending());
	zassert_equal(0u, radiant_event_drain(0u));
	zassert_equal(0u, n_recs, "a flush must deliver nothing");
	zassert_equal(2u, radiant_event_stats_get()->dropped_flush);

	/* Library configuration goes back to zero with it. */
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_get(&cfg));
	zassert_equal(0u, cfg);
}

ZTEST(radiant_event, test_drain_honours_its_bound)
{
	struct radiant_rx_event hal;
	struct radiant_event_rx rx;
	uint8_t             payload[RADIANT_FRAME_PAYLOAD_STD];
	uint8_t             i;

	memcpy(payload, payload8, sizeof(payload));
	hal = mk_hal(RADIANT_RADIO_STATUS_OK, 1000000u, true, true, -40);

	for (i = 0u; i < 5u; i++) {
		payload[0] = i;
		rx = mk_rx(&hal, i, payload);
		zassert_equal(RADIANT_EVENT_OK, radiant_event_post_rx(&rx));
	}

	zassert_equal(2u, radiant_event_drain(2u));
	zassert_equal(3u, radiant_event_queued());
	zassert_equal(0u, recs[0].data[0]);
	zassert_equal(1u, recs[1].data[0]);

	zassert_equal(3u, radiant_event_drain(0u));
	zassert_equal(2u, recs[2].data[0]);
	zassert_equal(4u, recs[4].data[0]);
	zassert_false(radiant_event_pending());
}

/*
 * antr_init() must leave nothing behind from a previous run of the stack. A
 * message surviving a re-init would reach the host between the reset command
 * and the startup message, which is the one window a real stick is silent in.
 */
ZTEST(radiant_event, test_init_empties_the_ring)
{
	struct radiant_rx_event hal;
	struct radiant_event_rx rx;
	uint8_t             cfg = 0xFFu;

	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_set(
			      (uint8_t)RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS));

	hal = mk_hal(RADIANT_RADIO_STATUS_OK, 1000000u, true, true, -40);
	rx = mk_rx(&hal, 0u, payload8);
	zassert_equal(RADIANT_EVENT_OK, radiant_event_post_rx(&rx));
	zassert_equal(1u, radiant_event_queued());

	radiant_event_init();

	zassert_equal(0u, radiant_event_queued());
	zassert_false(radiant_event_pending());
	zassert_equal(0u, radiant_event_drain(0u));
	zassert_equal(0u, n_recs);
	zassert_equal(0u, radiant_event_stats_get()->posted,
		      "init clears the statistics as well as the ring");
	zassert_equal((radiant_msg_err_t)RADIANT_WIRE_RESPONSE_NO_ERROR,
		      radiant_event_lib_config_get(&cfg));
	zassert_equal(0u, cfg);
}

/* ---------------------------------------------------------------------------
 * Argument checking
 * ---------------------------------------------------------------------------
 */

ZTEST(radiant_event, test_bad_arguments_are_refused_not_truncated)
{
	struct radiant_rx_event  hal;
	struct radiant_event_rx  rx;
	struct radiant_event_ext ext = mk_ext();
	uint8_t              out[RADIANT_EVENT_EXT_MAX];

	hal = mk_hal(RADIANT_RADIO_STATUS_OK, 1000000u, true, true, -40);

	zassert_equal(RADIANT_EVENT_EINVAL, radiant_event_post_rx(NULL));

	rx = mk_rx(&hal, 0u, payload8);
	rx.hal = NULL;
	zassert_equal(RADIANT_EVENT_EINVAL, radiant_event_post_rx(&rx));

	rx = mk_rx(&hal, 0u, NULL);
	zassert_equal(RADIANT_EVENT_EINVAL, radiant_event_post_rx(&rx));

	/* One past the highest channel the burst header can name. */
	rx = mk_rx(&hal, (uint8_t)(RADIANT_EVENT_CHANNEL_MAX + 1u), payload8);
	zassert_equal(RADIANT_EVENT_EINVAL, radiant_event_post_rx(&rx));

	rx = mk_rx(&hal, 0u, payload8);
	rx.payload_len = 0u;
	zassert_equal(RADIANT_EVENT_EINVAL, radiant_event_post_rx(&rx));

	rx = mk_rx(&hal, 0u, payload8);
	rx.payload_len = (uint8_t)(RADIANT_EVENT_PAYLOAD_MAX + 1u);
	zassert_equal(RADIANT_EVENT_EINVAL, radiant_event_post_rx(&rx));

	/* Only the three received-data message IDs are data messages. */
	rx = mk_rx(&hal, 0u, payload8);
	rx.msg_id = (uint8_t)RADIANT_WIRE_MESG_RESPONSE_EVENT_ID;
	zassert_equal(RADIANT_EVENT_EINVAL, radiant_event_post_rx(&rx));

	zassert_equal(RADIANT_EVENT_EINVAL,
		      radiant_event_post_channel_event(
			      (uint8_t)(RADIANT_EVENT_CHANNEL_MAX + 1u), 0x07u));

	zassert_equal(RADIANT_EVENT_EINVAL,
		      radiant_event_post_raw(0x4Eu, payload8,
					 (uint8_t)(RADIANT_EVENT_BODY_MAX + 1u)));

	/* A short output buffer is refused rather than filled with a
	 * truncated tail, which would be a valid-looking message with the
	 * wrong fields in it. */
	zassert_equal(RADIANT_EVENT_EINVAL,
		      radiant_event_build_ext(
			      (uint8_t)RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS, &ext,
			      out, RADIANT_EVENT_EXT_MAX - 1u));
	zassert_equal(RADIANT_EVENT_EINVAL,
		      radiant_event_build_ext(
			      (uint8_t)RADIANT_WIRE_LIB_CONFIG_ALL_EXT_FIELDS, NULL,
			      out, sizeof(out)));

	zassert_equal(0u, radiant_event_queued());
	zassert_equal(0u, n_recs);
}
