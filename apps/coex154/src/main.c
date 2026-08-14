/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * P3.5 - measures 802.15.4 frame loss caused by ANT blackouts. Stub RX/TX,
 * deliberately not OpenThread: a retrying stack would turn air loss into
 * latency and hide the number being measured. See docs/radiant-bridge.md
 * section 7.3a.
 *
 * Uses nrf_802154 directly rather than Zephyr's ieee802154 driver: the
 * Zephyr driver always routes frames through net_pkt/L2 even in RAW_MODE,
 * one more place to lose a frame for reasons unrelated to the arbiter. The
 * direct API is also what carries MPSL arbitration, via RAAL.
 *
 * Loss is computed over the span the receiver actually saw
 * (last_seq - first_seq + 1), not "expected minus received" - the two
 * boards are never started together, and that difference would land
 * entirely in the reported loss. Matches tools/ant_verify.py's
 * `loss (exact)` for comparability.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include <nrf_802154.h>

LOG_MODULE_REGISTER(coex154, LOG_LEVEL_INF);

/* ---------------------------------------------------------------------------
 * THE 802.15.4 DRIVER'S OWN ASSERT, MADE NON-FATAL AND COUNTED
 * ---------------------------------------------------------------------------
 *
 * Measured on 2026-08-13: with radiant opening even one ANT master this
 * image panicked about one second in, every time. Symbolicated, the panic is
 *
 *   NRF_802154_ASSERT(radio_is_disabled)
 *   nrfxlib/nrf_802154/driver/src/nrf_802154_trx.c:380,
 *   in wait_until_radio_is_disabled(), inlined into rxframe_finish() (:1718)
 *
 * i.e. the driver finished receiving a frame, triggered TASKS_DISABLE, spun
 * MAX_RAMPDOWN_CYCLES waiting for RADIO->STATE to read DISABLED, and never saw
 * it - because by then the gate had taken the RADIO for an ANT slot and ramped
 * it back up. That is a real coexistence defect and it is reported as one; it
 * is NOT a property of this application.
 *
 * nrf_802154_assert_handler() is __weak in
 * zephyr/modules/hal_nordic/nrf_802154/nrf_802154_assert_handler.c, whose only
 * body is k_panic(). Overriding it here is the supported hook and it is the
 * difference between "P3.5 cannot be measured at all" and "P3.5 measured, with
 * the driver's invariant violated N times, N printed beside the loss figure".
 *
 * READ drv_assert BEFORE READING loss. A run with drv_assert > 0 is a run in
 * which the 15.4 driver continued past a state check it does not expect to
 * survive; its loss number is a lower bound on a broken configuration, not a
 * clean measurement of arbitration. Do not quote the percentage without it.
 */
static volatile uint32_t drv_assert;

void nrf_802154_assert_handler(void)
{
	drv_assert++;
}

/* ---------------------------------------------------------------------------
 * The frame: a plain 802.15.4-2006 data frame, short addressing, PAN id
 * compressed. Nine bytes of MHR:
 *
 *   [0]     PSDU length, including 2-byte FCS, not including this byte
 *   [1..2]  FCF 0x8841: data, PAN id compression, short dst/src addressing
 *   [3]     MAC sequence number (8-bit, wraps every 256 frames - not what
 *           the loss arithmetic uses; see the 32-bit counter in the payload)
 *   [4..5]  destination PAN id
 *   [6..7]  destination short address
 *   [8..9]  source short address
 *
 * then the payload, then two bytes the radio's CRC engine fills in.
 * ---------------------------------------------------------------------------
 */
#define MHR_LEN     10u
#define FCS_LEN     2u
#define MAGIC       0x52414e54u /* "RANT" */
#define PAYLOAD_MIN 8u          /* magic + counter */

#define PSDU_LEN ((uint8_t)CONFIG_COEX154_PSDU_LEN)

BUILD_ASSERT(CONFIG_COEX154_PSDU_LEN >= (int)(MHR_LEN - 1u + PAYLOAD_MIN + FCS_LEN),
	     "PSDU too short to carry the magic and the counter");
BUILD_ASSERT(CONFIG_COEX154_PSDU_LEN <= 127,
	     "127 is the 802.15.4 maximum PSDU");

#define ADDR_TX 0x1111u
#define ADDR_RX 0x2222u

/* Offset of the payload within the raw buffer (which includes the length byte
 * at [0], so the MHR occupies [0..9]). */
#define PAYLOAD_OFF MHR_LEN

static void frame_build(uint8_t *buf, uint8_t mac_seq, uint32_t counter)
{
	uint32_t i;

	buf[0] = PSDU_LEN;
	buf[1] = 0x41u; /* data frame, PAN id compression */
	buf[2] = 0x88u; /* short dst, short src */
	buf[3] = mac_seq;
	buf[4] = (uint8_t)(CONFIG_COEX154_PAN_ID & 0xffu);
	buf[5] = (uint8_t)((CONFIG_COEX154_PAN_ID >> 8) & 0xffu);
	buf[6] = (uint8_t)(ADDR_RX & 0xffu);
	buf[7] = (uint8_t)(ADDR_RX >> 8);
	buf[8] = (uint8_t)(ADDR_TX & 0xffu);
	buf[9] = (uint8_t)(ADDR_TX >> 8);

	buf[PAYLOAD_OFF + 0u] = (uint8_t)(MAGIC >> 24);
	buf[PAYLOAD_OFF + 1u] = (uint8_t)(MAGIC >> 16);
	buf[PAYLOAD_OFF + 2u] = (uint8_t)(MAGIC >> 8);
	buf[PAYLOAD_OFF + 3u] = (uint8_t)MAGIC;
	buf[PAYLOAD_OFF + 4u] = (uint8_t)(counter >> 24);
	buf[PAYLOAD_OFF + 5u] = (uint8_t)(counter >> 16);
	buf[PAYLOAD_OFF + 6u] = (uint8_t)(counter >> 8);
	buf[PAYLOAD_OFF + 7u] = (uint8_t)counter;

	/* Filler up to the FCS (hardware-written). A fixed pattern rather than
	 * zeroes so a frame truncated mid-air and somehow passing CRC would
	 * still be visible in a dump. */
	for (i = PAYLOAD_OFF + PAYLOAD_MIN; i < (uint32_t)PSDU_LEN + 1u - FCS_LEN;
	     i++) {
		buf[i] = (uint8_t)i;
	}
}

/* Returns false if this is not one of ours. */
static bool frame_counter(const uint8_t *psdu, uint8_t len, uint32_t *out)
{
	uint32_t magic;

	if (len < MHR_LEN - 1u + PAYLOAD_MIN + FCS_LEN) {
		return false;
	}
	/* nrf_802154 hands the buffer back with the length byte at [0], the
	 * same layout transmit_raw takes. */
	magic = ((uint32_t)psdu[PAYLOAD_OFF + 0u] << 24) |
		((uint32_t)psdu[PAYLOAD_OFF + 1u] << 16) |
		((uint32_t)psdu[PAYLOAD_OFF + 2u] << 8) |
		(uint32_t)psdu[PAYLOAD_OFF + 3u];
	if (magic != MAGIC) {
		return false;
	}
	*out = ((uint32_t)psdu[PAYLOAD_OFF + 4u] << 24) |
	       ((uint32_t)psdu[PAYLOAD_OFF + 5u] << 16) |
	       ((uint32_t)psdu[PAYLOAD_OFF + 6u] << 8) |
	       (uint32_t)psdu[PAYLOAD_OFF + 7u];
	return true;
}

/* ---------------------------------------------------------------------------
 * Receiver
 * ---------------------------------------------------------------------------
 */

#if defined(CONFIG_COEX154_ROLE_RX)

static struct {
	uint32_t received;   /* ours, counted */
	uint32_t foreign;    /* on channel, not ours - ambient, and worth seeing */
	uint32_t first_seq;
	uint32_t last_seq;
	bool     have_first;
	/* Largest run of consecutive missing counters. A blackout is a
	 * burst - losses from one ANT slot are adjacent - so mean loss
	 * rate alone can't distinguish "4% spread evenly" (a retrying
	 * stack absorbs it) from "one 300ms hole" (it doesn't). */
	uint32_t worst_gap;
	uint32_t out_of_order;
	/* The PSDU length actually seen on air. CONFIG_COEX154_PSDU_LEN is a
	 * property of the GENERATOR, and the generator is a different image on a
	 * different board - so printing the receiver's own copy of that symbol
	 * would report 127 through an entire 40-byte pass and label the wrong
	 * column of section 7.3a's table. */
	uint8_t  last_len;
	/* Frames the radio started and could not finish. `aborted` is the
	 * subset the driver blames on losing the radio - an ANT blackout seen
	 * from inside 802.15.4, and the only counter here that names the
	 * mechanism rather than the symptom. */
	uint32_t failed;
	uint32_t aborted;
} rx;

/*
 * The driver actually calls the TIMESTAMP callback, not received_raw -
 * getting this wrong means a receiver that initialises cleanly and counts
 * zero frames forever, including zero ambient traffic (the tell).
 * nrf_802154 supplies a default received_raw that forwards to
 * received_timestamp_raw, so defining the wrong one is not a link error.
 * Both are defined here and both funnel into rx_count().
 */
static void rx_count(uint8_t *data)
{
	uint32_t counter;

	if (!frame_counter(data, data[0], &counter)) {
		rx.foreign++;
		nrf_802154_buffer_free_raw(data);
		return;
	}

	rx.last_len = data[0];

	if (!rx.have_first) {
		rx.have_first = true;
		rx.first_seq = counter;
		rx.last_seq = counter;
		rx.received = 1u;
		nrf_802154_buffer_free_raw(data);
		return;
	}

	rx.received++;
	if (counter > rx.last_seq) {
		uint32_t gap = counter - rx.last_seq - 1u;

		if (gap > rx.worst_gap) {
			rx.worst_gap = gap;
		}
		rx.last_seq = counter;
	} else {
		/* The generator numbers monotonically and the radio doesn't
		 * reorder, so this is a duplicate or a corrupt counter that
		 * passed CRC. Counted separately rather than folded into
		 * loss, which it would flatter. */
		rx.out_of_order++;
	}

	nrf_802154_buffer_free_raw(data);
}

void nrf_802154_received_raw(uint8_t *data, int8_t power, uint8_t lqi)
{
	ARG_UNUSED(power);
	ARG_UNUSED(lqi);
	rx_count(data);
}

void nrf_802154_received_timestamp_raw(uint8_t *data, int8_t power, uint8_t lqi,
				       uint64_t time)
{
	ARG_UNUSED(power);
	ARG_UNUSED(lqi);
	ARG_UNUSED(time);
	rx_count(data);
}

void nrf_802154_receive_failed(nrf_802154_rx_error_t error, uint32_t id)
{
	ARG_UNUSED(id);
	rx.failed++;
	if (error == NRF_802154_RX_ERROR_ABORTED) {
		rx.aborted++;
	}
	/* A frame the radio started and couldn't finish. Logged, not counted
	 * into loss - it's the same event as a missing counter seen from the
	 * other side. NRF_802154_RX_ERROR_ABORTED is what an ANT blackout
	 * looks like from inside 802.15.4; seeing it confirms real contention.
	 */
	LOG_DBG("rx failed: %u", (unsigned int)error);
}

static void report(void)
{
	uint32_t sent;
	uint32_t lost;
	uint32_t ppm;

	if (!rx.have_first) {
		LOG_INF("P3.5 rx: nothing heard yet (foreign=%u rxfail=%u "
			"drv_assert=%u)", rx.foreign, rx.failed, drv_assert);
		return;
	}

	sent = rx.last_seq - rx.first_seq + 1u;
	lost = (sent > rx.received) ? (sent - rx.received) : 0u;
	/* Parts per million, printed as a percentage with three decimals.
	 * Integer arithmetic - a float here would pull in a formatter this
	 * image has no other use for. */
	ppm = (sent > 0u) ? (uint32_t)(((uint64_t)lost * 1000000u) / sent) : 0u;

	LOG_INF("P3.5 rx: span=%u recv=%u lost=%u loss=%u.%03u %% | "
		"worst_gap=%u dup=%u foreign=%u | rxfail=%u abort=%u "
		"drv_assert=%u | psdu=%u ch=%u",
		sent, rx.received, lost, ppm / 10000u, (ppm % 10000u) / 10u,
		rx.worst_gap, rx.out_of_order, rx.foreign,
		rx.failed, rx.aborted, drv_assert,
		(unsigned int)rx.last_len, (unsigned int)CONFIG_COEX154_CHANNEL);
}

/* ---------------------------------------------------------------------------
 * The ANT load - the thing that takes the radio away
 * ---------------------------------------------------------------------------
 */

#if defined(CONFIG_RADIANT)

#include "ant_radio.h"
#include "ant_wire.h"

/*
 * Device profile numbers are irrelevant - nothing decodes these frames.
 * What matters is the shape of the demand: one transmit slot per channel
 * per period, at a real ANT+ sensor's period, so the blackout cadence the
 * receiver sees matches what 7.3a's arithmetic is written against.
 *
 * 8070 counts @ 32768 Hz = 246.3 ms, the ANT+ HR period and the slowest
 * common profile - the conservative choice, since a faster profile would
 * blank the radio out more and inflate the loss figure.
 */
#define ANT_PERIOD    8070u
#define ANT_FREQ      57u   /* 2457 MHz, the ANT+ channel */
#define ANT_DEV_TYPE  0x78u
#define ANT_TRANS     1u

/*
 * Guarded divisor. CONFIG_COEX154_ANT_MASTERS is legitimately 0 in the
 * arbiter-only arm, and the offset expression below divides by it - a
 * compile-time division by zero is a hard error even on a branch that never
 * runs, because the constant is folded before the branch is discarded.
 */
#define ANT_N       ((unsigned int)CONFIG_COEX154_ANT_MASTERS)
#define ANT_N_DIV   (ANT_N > 0u ? ANT_N : 1u)

static void ant_load_start(void)
{
	static const uint8_t net_key[8] = {
		/* All-zero: nothing receives these frames, so the real ANT+
		 * network key isn't needed and this file stays free of one it
		 * has no right to. */
		0, 0, 0, 0, 0, 0, 0, 0
	};
	uint8_t ch;

	if (antr_init() != 0) {
		LOG_ERR("antr_init failed - no ANT load, this is NOT the "
			"contended build any more");
		return;
	}
	(void)antr_network_address_set(0u, net_key);

	/*
	 * THE ARBITER-ONLY ARM, and it stops here on purpose. radiant is
	 * linked, initialised and holding its MPSL session; no channel is open,
	 * so it asks MPSL for no air at all. That is the arm that separates "an
	 * arbiter is present" from "air was taken away" - and it only means that
	 * if antr_init() above has actually run, which is why the zero case is
	 * inside this function rather than compiled out around it.
	 */
	if (ANT_N == 0u) {
		LOG_INF("ANT load: none - radiant is up but owns no "
			"channel. ARBITER-ONLY arm.");
		return;
	}

	for (ch = 0u; ch < (uint8_t)ANT_N; ch++) {
		antr_err_t rc;

		/* MASTER_TX_ONLY, not MASTER: a bidirectional master opens a
		 * receive window after each slot that nothing will reply to,
		 * which this measurement would misattribute to the transmit
		 * blackout. */
		rc = antr_channel_assign(ch, ANTW_CHANNEL_TYPE_MASTER_TX_ONLY,
					 0u, 0u);
		if (rc != 0) {
			LOG_ERR("ch%u assign: %u", ch, (unsigned int)rc);
			return;
		}
		(void)antr_channel_id_set(ch, (uint16_t)(0x7000u + ch),
					  ANT_DEV_TYPE, ANT_TRANS);
		(void)antr_channel_period_set(ch, ANT_PERIOD);
		(void)antr_channel_radio_freq_set(ch, ANT_FREQ);

		/*
		 * Offsets are spread across the period rather than all zero.
		 * With no offset all masters bunch into a few ms of blackout
		 * followed by ~240 ms clear air - a real but worst-case shape.
		 * 7.3a's arithmetic assumes loss spread over the period, so
		 * spreading offsets is what actually measures that prediction;
		 * the bunched case is a separate run (set every offset to
		 * zero).
		 */
#if defined(CONFIG_COEX154_ANT_BUNCHED)
		(void)antr_channel_open_with_offset(ch, 0u);
#else
		(void)antr_channel_open_with_offset(
			ch, (uint16_t)((ANT_PERIOD / ANT_N_DIV) * ch));
#endif
	}

	LOG_INF("ANT load: %u masters at %u ms, freq 24%02u MHz, offsets %s",
		ANT_N, (unsigned int)((ANT_PERIOD * 1000u) / 32768u),
		(unsigned int)ANT_FREQ,
		IS_ENABLED(CONFIG_COEX154_ANT_BUNCHED) ? "BUNCHED (all zero)"
						       : "spread");
}

/*
 * radiant delivers events through antr_on_message(), the same seam the
 * USB bridge uses. Refill the payload on EVENT_TX, or the channel silently
 * repeats the last one - fine for a blackout generator but would hide a
 * channel that has actually stopped.
 */
void antr_on_message(const struct antr_msg *msg)
{
	static uint32_t tx_events;
	uint8_t payload[8];

	if (msg == NULL || msg->data == NULL) {
		return;
	}
	if (msg->id != ANTW_MESG_RESPONSE_EVENT_ID || msg->len < 3u) {
		return;
	}
	if (msg->data[2] != ANTW_EVENT_TX) {
		return;
	}

	tx_events++;
	memset(payload, 0, sizeof(payload));
	payload[0] = msg->data[0];            /* which channel */
	payload[7] = (uint8_t)tx_events;      /* something that changes */
	(void)antr_broadcast_message_tx(msg->data[0], sizeof(payload), payload);
}

#else

static void ant_load_start(void)
{
	LOG_INF("ANT load: none - radiant is not even linked. BENCH FLOOR "
		"arm: this is the room, the antennas and the two boards, with "
		"no arbiter in the image at all.");
}

#endif /* CONFIG_RADIANT */

static void role_run(void)
{
	ant_load_start();

	nrf_802154_promiscuous_set(true);
	nrf_802154_auto_ack_set(false);
	if (!nrf_802154_receive()) {
		LOG_ERR("nrf_802154_receive() refused");
		return;
	}

	LOG_INF("P3.5 receiver up: channel %u, PSDU %u bytes",
		(unsigned int)CONFIG_COEX154_CHANNEL, (unsigned int)PSDU_LEN);

	for (;;) {
		k_sleep(K_SECONDS(CONFIG_COEX154_REPORT_S));
		report();
	}
}

#else /* CONFIG_COEX154_ROLE_TX */

/* ---------------------------------------------------------------------------
 * Transmitter
 * ---------------------------------------------------------------------------
 */

static uint8_t  tx_buf[128];
static uint32_t tx_sent;
static K_SEM_DEFINE(tx_done, 0, 1);

void nrf_802154_transmitted_raw(uint8_t *frame, const nrf_802154_transmit_done_metadata_t *meta)
{
	ARG_UNUSED(frame);
	ARG_UNUSED(meta);
	tx_sent++;
	k_sem_give(&tx_done);
}

static uint32_t tx_cb_failed;   /* the driver said no, asynchronously */
static uint32_t tx_refused;     /* transmit_raw() said no, synchronously */
static uint8_t  tx_last_error;

void nrf_802154_transmit_failed(uint8_t *frame, nrf_802154_tx_error_t error,
				const nrf_802154_transmit_done_metadata_t *meta)
{
	ARG_UNUSED(frame);
	ARG_UNUSED(meta);
	/*
	 * Kept separate from the synchronous refusal counter (tx_refused):
	 * sharing one counter produced offered=2000 sent=1999 failed=2000 -
	 * every frame apparently both succeeding and failing. Same class of
	 * bug as the gate's `deny` counter conflating EDENIED with
	 * STATUS_DENIED.
	 */
	tx_cb_failed++;
	tx_last_error = (uint8_t)error;
	k_sem_give(&tx_done);
}

static void role_run(void)
{
	nrf_802154_transmit_metadata_t meta = {
		/*
		 * CCA off is the measurement, not a shortcut: with CCA on the
		 * generator would defer around the DUT's ANT slot, turning
		 * receiver-side loss into transmitter-side deferral (reading
		 * lower - the wrong direction). A real stack's deferral is a
		 * genuine cost, but it's the scheduler cost (7.3a), separate
		 * from the air cost measured here.
		 */
		.cca = false,
		.tx_power = { .use_metadata_value = false },
		.tx_channel = { .use_metadata_value = false },
	};
	uint32_t counter = 0u;
	uint8_t  mac_seq = 0u;

	/*
	 * The driver must leave SLEEP before it accepts a transmit -
	 * nrf_802154_init() leaves it there, and transmit_raw() from SLEEP is
	 * refused synchronously with no callback. Calling receive() first
	 * puts it in a transmit-capable state at no cost, since nothing is
	 * addressed to this role.
	 */
	if (!nrf_802154_receive()) {
		LOG_ERR("nrf_802154_receive() refused - the driver is not in a "
			"state that will transmit either");
	}

	LOG_INF("P3.5 generator up: channel %u, PSDU %u bytes, every %u ms",
		(unsigned int)CONFIG_COEX154_CHANNEL, (unsigned int)PSDU_LEN,
		(unsigned int)CONFIG_COEX154_TX_INTERVAL_MS);

	for (;;) {
		frame_build(tx_buf, mac_seq++, counter);

		/*
		 * transmit_raw() returns nrf_802154_tx_error_t where NONE == 0,
		 * so `if (transmit_raw(...))` would be inverted - unlike
		 * nrf_802154_receive() above, which returns a plain bool.
		 * Symptom of getting this wrong: offered=2000 sent=1998
		 * refused=1999, every frame apparently both transmitted and
		 * refused. Compare explicitly against NRF_802154_TX_ERROR_NONE.
		 */
		if (nrf_802154_transmit_raw(tx_buf, &meta) ==
		    NRF_802154_TX_ERROR_NONE) {
			/* Wait for the terminal callback rather than firing
			 * blind: the buffer is the radio's until it comes
			 * back, and overwriting it mid-air would corrupt the
			 * very counter the receiver is reading. */
			(void)k_sem_take(&tx_done, K_MSEC(200));
		} else {
			tx_refused++;
		}
		counter++;

		if ((counter % 500u) == 0u) {
			LOG_INF("P3.5 tx: offered=%u sent=%u cb_failed=%u "
				"refused=%u last_err=%u",
				counter, tx_sent, tx_cb_failed, tx_refused,
				(unsigned int)tx_last_error);
		}
		if (CONFIG_COEX154_TX_COUNT != 0 &&
		    counter >= (uint32_t)CONFIG_COEX154_TX_COUNT) {
			LOG_INF("P3.5 tx: done, offered=%u sent=%u cb_failed=%u "
				"refused=%u", counter, tx_sent, tx_cb_failed,
				tx_refused);
			return;
		}
		k_sleep(K_MSEC(CONFIG_COEX154_TX_INTERVAL_MS));
	}
}

#endif

/* ---------------------------------------------------------------------------
 * Callbacks the driver requires a definition for whichever role is built.
 * nrf_802154 declares these as plain functions, not weak symbols with a
 * default, so a missing one is a link error and an unused one is dead code
 * rather than a mistake.
 * ---------------------------------------------------------------------------
 */

#if defined(CONFIG_COEX154_ROLE_RX)
void nrf_802154_transmitted_raw(uint8_t *frame, const nrf_802154_transmit_done_metadata_t *meta)
{
	ARG_UNUSED(frame);
	ARG_UNUSED(meta);
}

void nrf_802154_transmit_failed(uint8_t *frame, nrf_802154_tx_error_t error,
				const nrf_802154_transmit_done_metadata_t *meta)
{
	ARG_UNUSED(frame);
	ARG_UNUSED(error);
	ARG_UNUSED(meta);
}
#else
void nrf_802154_received_raw(uint8_t *data, int8_t power, uint8_t lqi)
{
	ARG_UNUSED(power);
	ARG_UNUSED(lqi);
	nrf_802154_buffer_free_raw(data);
}

void nrf_802154_received_timestamp_raw(uint8_t *data, int8_t power, uint8_t lqi,
				       uint64_t time)
{
	ARG_UNUSED(power);
	ARG_UNUSED(lqi);
	ARG_UNUSED(time);
	nrf_802154_buffer_free_raw(data);
}

void nrf_802154_receive_failed(nrf_802154_rx_error_t error, uint32_t id)
{
	ARG_UNUSED(error);
	ARG_UNUSED(id);
}
#endif

#if defined(CONFIG_NRF_802154_SERIALIZATION)
#include <serialization/nrf_802154_serialization_error.h>

/*
 * Required only on the serialised build - the Spinel host calls this when
 * the IPC link to the network core fails, and it's `extern` with no
 * default, so leaving it out is a link error. The in-tree default lives in
 * Zephyr's ieee802154_nrf5 driver, which this app doesn't use.
 *
 * Must log loudly: this is the traffic generator, so a silent
 * serialisation failure would land in the loss column and be misread as an
 * ANT blackout. See boards/nrf5340dk_nrf5340_cpuapp.conf.
 */
void nrf_802154_serialization_error(const nrf_802154_ser_err_data_t *p_err)
{
	LOG_ERR("802.15.4 SERIALISATION ERROR %d - the generator is not a "
		"reference any more; discard this run",
		(int)(p_err != NULL ? p_err->reason : 0));
}
#endif

void nrf_802154_cca_done(bool channel_free)
{
	ARG_UNUSED(channel_free);
}

void nrf_802154_cca_failed(nrf_802154_cca_error_t error)
{
	ARG_UNUSED(error);
}

void nrf_802154_energy_detected(const nrf_802154_energy_detected_t *result)
{
	ARG_UNUSED(result);
}

void nrf_802154_energy_detection_failed(nrf_802154_ed_error_t error)
{
	ARG_UNUSED(error);
}

void nrf_802154_tx_ack_started(const uint8_t *data)
{
	ARG_UNUSED(data);
}

int main(void)
{
	static const uint8_t ext_addr_rx[8] = { 8, 7, 6, 5, 4, 3, 2, 1 };
	static const uint8_t ext_addr_tx[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	uint8_t short_addr[2];

	/* printk, synchronously, before anything that can hang. The one thing
	 * worse than a board that prints nothing is not knowing whether it
	 * reached main() at all. */
	printk("coex154: main(), calling nrf_802154_init()\n");
	nrf_802154_init();
	printk("coex154: nrf_802154_init() returned\n");
	nrf_802154_channel_set(CONFIG_COEX154_CHANNEL);
	nrf_802154_pan_id_set((const uint8_t[]){
		(uint8_t)(CONFIG_COEX154_PAN_ID & 0xffu),
		(uint8_t)((CONFIG_COEX154_PAN_ID >> 8) & 0xffu) });

#if defined(CONFIG_COEX154_ROLE_RX)
	short_addr[0] = (uint8_t)(ADDR_RX & 0xffu);
	short_addr[1] = (uint8_t)(ADDR_RX >> 8);
	nrf_802154_extended_address_set(ext_addr_rx);
#else
	short_addr[0] = (uint8_t)(ADDR_TX & 0xffu);
	short_addr[1] = (uint8_t)(ADDR_TX >> 8);
	nrf_802154_extended_address_set(ext_addr_tx);
#endif
	ARG_UNUSED(ext_addr_rx);
	ARG_UNUSED(ext_addr_tx);
	nrf_802154_short_address_set(short_addr);

	role_run();
	return 0;
}
