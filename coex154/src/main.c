/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * P3.5 - 802.15.4 FRAME LOSS CAUSED BY ANT BLACKOUTS.
 *
 * docs/radiant-bridge.md section 7.3a derives, and does not measure, that a
 * ~4 % ANT duty cycle costs ~18 % of MAX-SIZE 802.15.4 frames, and its own
 * warning box revises that to ~24 % once the follow-on reserve is counted. The
 * plan moved this measurement ahead of every line of Matter code for one
 * reason, quoted: "if the real figure is 24 %, the Matter data model is being
 * written against a link that does not work."
 *
 * So this is a stub receiver and a stub transmitter, and it is deliberately NOT
 * OpenThread. Thread would answer a different question - it retries, it defers
 * its own transmissions around a schedule MPSL has told it about, and it would
 * turn air loss into latency and hide exactly the number being looked for.
 * P4 is where a real stack goes; this is the ruler P4's numbers are read
 * against.
 *
 * ---------------------------------------------------------------------------
 * WHY nrf_802154 DIRECTLY AND NOT ZEPHYR'S ieee802154 DRIVER
 * ---------------------------------------------------------------------------
 *
 * Zephyr's driver delivers every received frame through net_recv_data() into an
 * L2, and CONFIG_IEEE802154_RAW_MODE does not change that - it only stops the
 * driver registering an interface, leaving the same net_pkt path with nothing
 * at the end of it. Counting frames does not need a network stack, and a
 * network stack between the radio and the counter is one more place for a frame
 * to go missing for a reason that has nothing to do with the arbiter.
 *
 * The direct API is also the one that carries the arbitration: with CONFIG_MPSL
 * on, nrf_802154 asks MPSL's RAAL for the radio exactly as the gate asks for a
 * timeslot. That contention IS the measurement.
 *
 * ---------------------------------------------------------------------------
 * HOW LOSS IS COMPUTED, AND WHY IT IS NOT "EXPECTED MINUS RECEIVED"
 * ---------------------------------------------------------------------------
 *
 * Every frame carries its own sequence number. The receiver takes the FIRST and
 * LAST sequence it actually saw and computes the loss over THAT span:
 *
 *     sent     = last_seq - first_seq + 1
 *     received = frames counted in the span
 *     loss     = 1 - received / sent
 *
 * Not "the generator was told to send 3000, we got 2400". The two differ
 * whenever the boards are not started together, which is always, and the
 * difference lands entirely in the number being reported. tools/ant_verify.py
 * makes the same distinction and calls it `loss (exact)`; this is the same
 * quantity for the other radio, so the two are directly comparable - which is
 * the whole point of running them on one bench.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/printk.h>
#include <string.h>

#include <nrf_802154.h>

LOG_MODULE_REGISTER(coex154, LOG_LEVEL_INF);

/* ---------------------------------------------------------------------------
 * The frame
 * ---------------------------------------------------------------------------
 *
 * A plain 802.15.4-2006 data frame, short addressing both ends, PAN id
 * compressed. Nine bytes of MHR:
 *
 *   [0]     PSDU length, including the 2-byte FCS, NOT including this byte
 *   [1..2]  FCF - 0x8841: data (0x1), PAN id compression (bit 6),
 *           dst addressing mode short (0x2 at bits 10-11),
 *           src addressing mode short (0x2 at bits 14-15)
 *   [3]     sequence number (the MAC's own, 8-bit and wrapping - NOT what the
 *           loss arithmetic uses; see the payload counter)
 *   [4..5]  destination PAN id
 *   [6..7]  destination short address
 *   [8..9]  source short address
 *
 * then the payload, then two bytes the RADIO's CRC engine fills in.
 *
 * THE MAC SEQUENCE NUMBER IS NOT USABLE FOR THIS. It is eight bits, so it wraps
 * every 256 frames - about five seconds at the default interval - and a wrap is
 * indistinguishable from a 256-frame gap. The counter that the loss is computed
 * from is 32 bits and lives in the payload.
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

	/* Filler up to the FCS, which the hardware writes. A fixed pattern
	 * rather than zeroes so that a frame truncated mid-air and somehow
	 * passing CRC would still be visible in a dump. */
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
	/* The largest run of consecutive missing counters. A blackout is a
	 * BURST - the frames lost to one ANT slot are adjacent - so the mean
	 * loss rate alone cannot distinguish "4 % spread evenly", which a
	 * retrying stack absorbs, from "one 300 ms hole", which it does not. */
	uint32_t worst_gap;
	uint32_t out_of_order;
} rx;

/*
 * THE CALLBACK THE DRIVER ACTUALLY CALLS IS THE TIMESTAMP ONE, and getting
 * this wrong costs a receiver that initialises perfectly, reports no error from
 * nrf_802154_receive(), and counts ZERO frames for ever - including zero
 * ambient traffic, which is the tell.
 *
 * nrf_802154_callouts.h says it plainly, in a note attached to
 * nrf_802154_received_raw: "Default implementation of this function provided by
 * the nRF 802.15.4 Radio Driver calls nrf_802154_received_timestamp_raw". The
 * two are declared `extern` twenty lines apart with near-identical
 * documentation, and defining the wrong one is not a link error - the driver
 * has a default for it, so the build succeeds and the application's function is
 * simply never reached.
 *
 * Both are defined here and both funnel into rx_count(), so it does not matter
 * which one this driver build decides to notify through.
 */
static void rx_count(uint8_t *data)
{
	uint32_t counter;

	if (!frame_counter(data, data[0], &counter)) {
		rx.foreign++;
		nrf_802154_buffer_free_raw(data);
		return;
	}

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
		/* The generator numbers monotonically and the radio does not
		 * reorder, so this is a duplicate or a corrupt counter that
		 * still passed CRC. Counted rather than folded into the loss,
		 * because it would flatter it. */
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
	/*
	 * A frame the radio started and could not finish. Logged rather than
	 * counted into loss: it is the SAME event as a missing counter, seen
	 * from the other side, and adding it would double-count. It is here
	 * because NRF_802154_RX_ERROR_ABORTED specifically means the driver
	 * gave the radio back - which is what an ANT blackout looks like from
	 * inside 802.15.4, and seeing it at all confirms the two stacks really
	 * are contending rather than politely taking turns.
	 */
	LOG_DBG("rx failed: %u", (unsigned int)error);
}

static void report(void)
{
	uint32_t sent;
	uint32_t lost;
	uint32_t ppm;

	if (!rx.have_first) {
		LOG_INF("P3.5 rx: nothing heard yet (foreign=%u)", rx.foreign);
		return;
	}

	sent = rx.last_seq - rx.first_seq + 1u;
	lost = (sent > rx.received) ? (sent - rx.received) : 0u;
	/* Parts per million, then printed as a percentage with three decimals.
	 * Integer arithmetic on purpose - a float here would pull in a
	 * formatter this image has no other use for. */
	ppm = (sent > 0u) ? (uint32_t)(((uint64_t)lost * 1000000u) / sent) : 0u;

	LOG_INF("P3.5 rx: span=%u recv=%u lost=%u loss=%u.%03u %% | "
		"worst_gap=%u dup=%u foreign=%u | psdu=%u ch=%u",
		sent, rx.received, lost, ppm / 10000u, (ppm % 10000u) / 10u,
		rx.worst_gap, rx.out_of_order, rx.foreign,
		(unsigned int)PSDU_LEN, (unsigned int)CONFIG_COEX154_CHANNEL);
}

/* ---------------------------------------------------------------------------
 * The ANT load - the thing that takes the radio away
 * ---------------------------------------------------------------------------
 */

#if defined(CONFIG_RADIANT_CORE) && CONFIG_COEX154_ANT_MASTERS > 0

#include "ant_radio.h"
#include "ant_wire.h"

/*
 * ANT+ device profile numbers are irrelevant here - nothing decodes these
 * frames, and nothing is meant to. What matters is the SHAPE of the demand:
 * one transmit slot per channel per period, at the period a real ANT+ sensor
 * uses, so the blackout cadence the 802.15.4 receiver sees is the cadence
 * section 7.3a's arithmetic is written against.
 *
 * 8070 counts of 32768 Hz is 246.3 ms, the ANT+ heart-rate period, and it is
 * the slowest of the common profiles - which makes it the CONSERVATIVE choice.
 * A faster profile would black the radio out more often and produce a worse
 * (larger) loss figure; picking the kindest common period keeps this
 * measurement from flattering the pessimistic prediction it is testing.
 */
#define ANT_PERIOD    8070u
#define ANT_FREQ      57u   /* 2457 MHz, the ANT+ channel */
#define ANT_DEV_TYPE  0x78u
#define ANT_TRANS     1u

static void ant_load_start(void)
{
	static const uint8_t net_key[8] = {
		/* The public network key is not a secret and is not needed:
		 * nothing receives these. Network 0 with the default all-zero
		 * key transmits perfectly well and keeps this file free of a
		 * key it has no right to. */
		0, 0, 0, 0, 0, 0, 0, 0
	};
	uint8_t ch;

	if (antr_init() != 0) {
		LOG_ERR("antr_init failed - no ANT load, this is NOT the "
			"contended build any more");
		return;
	}
	(void)antr_network_address_set(0u, net_key);

	for (ch = 0u; ch < (uint8_t)CONFIG_COEX154_ANT_MASTERS; ch++) {
		antr_err_t rc;

		/* MASTER_TX_ONLY rather than MASTER: a bidirectional master
		 * also opens a receive window after each slot for an
		 * acknowledged reply, which is air this measurement would
		 * attribute to the transmit blackout. Nothing is going to
		 * reply, so the window is pure contamination. */
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
		 * SPREAD ACROSS THE PERIOD, NOT ALL AT ZERO.
		 *
		 * Eight masters opened with no offset all place their first
		 * slot as soon as possible and then run on the same period, so
		 * they stay bunched for ever - eight blackouts inside a few
		 * milliseconds and then 240 ms of clear air. That is a real
		 * shape, and section 7.3a explicitly says clustering is
		 * "guaranteed rather than avoidable" when the phases come from
		 * N independent sensors' clocks. But it is the WORST case, not
		 * the modelled one: the table's arithmetic assumes the
		 * blackouts are spread over the period, and measuring the
		 * bunched case against a spread prediction would report a
		 * number that is too kind and blame the wrong thing for it.
		 *
		 * So they are spread evenly, which measures what 7.3a
		 * predicts. The bunched case is a separate run, and worth one:
		 * set every offset to zero and the same rig measures the tail.
		 */
		(void)antr_channel_open_with_offset(
			ch, (uint16_t)((ANT_PERIOD / CONFIG_COEX154_ANT_MASTERS) * ch));
	}

	LOG_INF("ANT load: %u masters at %u ms, freq 24%02u MHz",
		(unsigned int)CONFIG_COEX154_ANT_MASTERS,
		(unsigned int)((ANT_PERIOD * 1000u) / 32768u),
		(unsigned int)ANT_FREQ);
}

/*
 * radiant_core hands every event back through antr_on_message(), the same seam
 * the USB bridge uses. A master needs exactly one thing from it: refill the
 * payload when the slot has gone out, or the channel repeats the last one -
 * which transmits identically and would do for a blackout generator, but
 * silently hides a channel that has stopped.
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
	LOG_INF("ANT load: none - this is the CONTROL arm");
}

#if defined(CONFIG_RADIANT_CORE)
void antr_on_message(const struct antr_msg *msg)
{
	ARG_UNUSED(msg);
}
#endif

#endif /* CONFIG_RADIANT_CORE && CONFIG_COEX154_ANT_MASTERS > 0 */

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
	 * SEPARATED FROM THE SYNCHRONOUS REFUSAL, and the first run is exactly
	 * why. One counter served both and reported offered=2000 sent=1999
	 * failed=2000 - every frame apparently succeeding AND failing, which
	 * is not a thing that can happen and was really two different events
	 * sharing a number. The same mistake, and the same fix, as the gate's
	 * `deny` counter conflating EDENIED with STATUS_DENIED.
	 */
	tx_cb_failed++;
	tx_last_error = (uint8_t)error;
	k_sem_give(&tx_done);
}

static void role_run(void)
{
	nrf_802154_transmit_metadata_t meta = {
		/*
		 * CCA OFF, AND THAT IS THE MEASUREMENT RATHER THAN A SHORTCUT.
		 *
		 * With CCA on, the generator would back off whenever the DUT's
		 * ANT slot happened to be radiating - and the frames it then did
		 * not send would be counted as frames the DUT did not hear. That
		 * turns a receiver-side loss measurement into a
		 * transmitter-side deferral measurement and reads LOWER, which
		 * is the worst direction for a number this one is meant to
		 * settle.
		 *
		 * A real stack does defer, and that deferral is a genuine part
		 * of the cost - but it is the SCHEDULER cost, which section
		 * 7.3a's warning box insists on recording separately from the
		 * AIR cost. This image measures air. P4 measures the other.
		 */
		.cca = false,
		.tx_power = { .use_metadata_value = false },
		.tx_channel = { .use_metadata_value = false },
	};
	uint32_t counter = 0u;
	uint8_t  mac_seq = 0u;

	/*
	 * THE DRIVER MUST BE OUT OF SLEEP BEFORE IT WILL ACCEPT A TRANSMIT.
	 * nrf_802154_init() leaves it in SLEEP, and transmit_raw() from there
	 * is refused synchronously with no callback and no error code - a
	 * silent false. Putting it in receive first is what the driver's own
	 * transmit path expects; the extra receive costs this role nothing
	 * because nothing is addressed to it.
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
		 * == NRF_802154_TX_ERROR_NONE, NOT a truth test.
		 *
		 * nrf_802154_transmit_raw() returns nrf_802154_tx_error_t, and
		 * NRF_802154_TX_ERROR_NONE is ZERO - so `if (transmit_raw(...))`
		 * is exactly INVERTED. nrf_802154_receive() a few lines above
		 * returns a plain bool where true means success, which is what
		 * makes this worth a comment rather than a fix: two functions,
		 * adjacent in the same header, with opposite conventions.
		 *
		 * The symptom was a set of counters that could not all be true
		 * at once - offered=2000 sent=1998 refused=1999 - every frame
		 * both transmitted and refused. The frames were going out
		 * perfectly; only the bookkeeping was upside down. Worth
		 * remembering that the impossible-looking counter was what
		 * exposed it, which is the second time in this session that
		 * splitting one number into two found the bug.
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
 * REQUIRED ON THE SERIALISED BUILD AND ON NO OTHER, which is why it is not in
 * the block above. The Spinel host calls this when the IPC link to the network
 * core fails - a lost response, a timeout, a decode error - and it is declared
 * `extern` with no default, so leaving it out is a link error rather than a
 * silent stub. In tree it is supplied by Zephyr's ieee802154_nrf5 driver, which
 * this application deliberately does not use.
 *
 * IT MUST BE LOUD. This is the traffic GENERATOR: every frame it fails to send
 * is a frame the DUT will be recorded as having lost. A serialisation failure
 * that printed nothing would land in the loss column and be read as a blackout,
 * which is precisely the wrong conclusion and the whole reason the generator
 * was moved to the core with a console. See boards/nrf5340dk_nrf5340_cpuapp.conf.
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
