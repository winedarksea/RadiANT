/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ANT Serial Bridge — translates the ANT USB serial protocol (0xA4-framed
 * messages) to/from the radio contract in ant_radio.h.
 *
 * Inbound (host → chip):
 *   Parser thread drains ant_rx_ring_buf byte by byte through a state
 *   machine, validates the XOR checksum, and calls the matching antr_*
 *   function. Each command gets an ANTW_MESG_RESPONSE_EVENT_ID (0x40) ack.
 *
 * Outbound (chip → host):
 *   The radio backend calls antr_on_message() - implemented here - for every
 *   message the host should see; it is wrapped in a serial frame and pushed
 *   to usb_ant_send(). No registration step: the symbol resolves at link
 *   time, so ant_serial_bridge_init() only starts the parser thread.
 *
 * Wire frame format (ANT Serial Protocol):
 *   [SYNC=0xA4] [LEN] [ID] [payload: LEN bytes] [XOR checksum]
 *
 * The checksum is the XOR of every preceding byte INCLUDING the SYNC byte.
 * Omitting SYNC yields a value off by exactly 0xA4, so two implementations
 * that both omit it agree with each other and with nobody else - how that
 * bug once survived a passing test suite.
 *
 * Nothing here names a symbol from sdk-ant: protocol constants come from
 * src/ant_wire.h (ANTW_*) and every radio call goes through src/ant_radio.h
 * (antr_*), which is what lets this file build with no sdk-ant present.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

#include "ant_transport.h"

#include "ant_health.h"
#include "ant_radio.h"
#include "ant_rgb_led.h"
#include "ant_wire.h"

LOG_MODULE_REGISTER(ant_bridge, LOG_LEVEL_INF);

/* sdk-ant's Kconfig hangs this symbol off CONFIG_ANT, so on a radio-stub build
 * (CONFIG_ANT=n) it does not exist at all and the device-wide transmit power
 * fan-out below fails to compile. It only bounds a loop, and the stub reports
 * the same eight channels the real build allocates.
 */
#ifndef CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED
#define CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED 8
#endif

/* The transport - USB on either stack, or a bare UART - is declared in
 * ant_transport.h and selected at build time. Nothing below depends on which.
 */

/* ── Bridge thread ─────────────────────────────────────────────────────────── */

#define BRIDGE_STACK_SIZE  2048

/* Taken from src/ant_radio.h, not respelled here, so this and a backend's
 * event thread (which must sit below it - see ANTR_HOST_THREAD_PRIORITY)
 * stay consistent by construction.
 */
#define BRIDGE_PRIORITY    ANTR_HOST_THREAD_PRIORITY

K_THREAD_STACK_DEFINE(bridge_stack, BRIDGE_STACK_SIZE);
static struct k_thread bridge_thread_data;

/* Set while a system reset is tearing channels down, so the resulting
 * EVENT_CHANNEL_CLOSED storm is not forwarded to the host. A real stick emits
 * nothing between the reset command and the startup message.
 */
static atomic_t reset_in_progress;

/* ── Frame helpers ─────────────────────────────────────────────────────────── */

/* Build and transmit an ANTW_MESG_RESPONSE_EVENT_ID (0x40) reply. */
static void send_response(uint8_t ch, uint8_t msg_id, uint8_t code)
{
	uint8_t buf[7];
	buf[0] = ANTW_SYNC_TX;                  /* 0xA4 */
	buf[1] = ANTW_MESG_RESPONSE_EVENT_SIZE; /* 3 */
	buf[2] = ANTW_MESG_RESPONSE_EVENT_ID;   /* 0x40 */
	buf[3] = ch;
	buf[4] = msg_id;
	buf[5] = code;
	buf[6] = buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4] ^ buf[5];
	usb_ant_send(buf, sizeof(buf));
}

/* Startup message sent after antr_stack_reset(). */
static void send_startup(void)
{
	uint8_t buf[5];
	buf[0] = ANTW_SYNC_TX;
	buf[1] = ANTW_MESG_STARTUP_MESG_SIZE;  /* 1 */
	buf[2] = ANTW_MESG_STARTUP_MESG_ID;    /* 0x6F */
	buf[3] = 0x00;                          /* reset by command */
	buf[4] = buf[0] ^ buf[1] ^ buf[2] ^ buf[3];
	usb_ant_send(buf, sizeof(buf));
}

static void send_message(uint8_t msg_id, const uint8_t *payload, uint8_t len)
{
	/* ANTW_MAX_FRAME_SIZE is ANTW_MAX_SIZE_VALUE + ANTW_MSG_OVERHEAD, the
	 * largest frame that can legally exist. Never ANTW_MESG_MAX_SIZE: that
	 * is built from ANTW_MAX_DATA_SIZE, which does not count the leading
	 * channel byte the LEN byte does, so it is one short and a full-length
	 * message would write its checksum past the end.
	 */
	uint8_t buf[ANTW_MAX_FRAME_SIZE];
	uint8_t xor = ANTW_SYNC_TX ^ len ^ msg_id;

	buf[0] = ANTW_SYNC_TX;
	buf[1] = len;
	buf[2] = msg_id;

	for (uint8_t index = 0; index < len; index++) {
		buf[3 + index] = payload[index];
		xor ^= payload[index];
	}

	buf[3 + len] = xor;
	usb_ant_send(buf, len + 4U);
}

/* ── Transmit power ────────────────────────────────────────────────────────── */

/* ANTW_RADIO_TX_POWER_LVL_5 (+8 dBm) exists only on the nRF52820, nRF52833 and
 * nRF52840; every other part this builds for tops out at LVL_4 (+4 dBm).
 * src/ant_wire.h states that restriction in the constant's doc comment rather
 * than exposing a symbol for it, so it is restated here.
 */
#if defined(CONFIG_ANT_DONGLE_TX_POWER_BOOST)
#if defined(CONFIG_SOC_NRF52840) || defined(CONFIG_SOC_NRF52833) || \
	defined(CONFIG_SOC_NRF52820)
#define ANT_DONGLE_TX_POWER_DEFAULT ANTW_RADIO_TX_POWER_LVL_5
#else
#define ANT_DONGLE_TX_POWER_DEFAULT ANTW_RADIO_TX_POWER_LVL_4
#endif
#else
#define ANT_DONGLE_TX_POWER_DEFAULT ANTW_RADIO_TX_POWER_LVL_3
#endif

/* What each channel should transmit at, whether or not it is assigned yet.
 * antr_channel_radio_tx_power_set() only takes on an assigned channel (why
 * the 0x47 fan-out below swallows its errors), and hosts set power before
 * assigning - holding it here and reapplying after each assign survives
 * that ordering.
 */
static uint8_t tx_power[CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED];

/* Raw register value paired with ANTW_RADIO_TX_POWER_LVL_CUSTOM, held per
 * channel for the same reason. Meaningless unless the matching tx_power[]
 * entry has the custom bit set, and not portable across parts.
 */
static uint8_t tx_power_custom[CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED];

static void tx_power_reset(void)
{
	for (uint8_t i = 0; i < ARRAY_SIZE(tx_power); i++) {
		tx_power[i] = ANT_DONGLE_TX_POWER_DEFAULT;
		tx_power_custom[i] = 0U;
	}
}

/*
 * Map a host's requested power level onto what this radio can actually do.
 * Levels 0-2 (-20/-12/-4 dBm) are honoured exactly - a deliberate request to
 * be quiet. Level 3 (0 dBm, the common default) and level 4 (+4 dBm, the
 * ceiling on the impersonated nRF51422) are both raised to the boosted
 * default, since a host asking for level 4 wants everything the hardware
 * has, and on an nRF52840 that's +8 dBm - the only way the extra 8 dB is
 * reachable, as no retail stick ever had LVL_5 to request by name.
 * ANTW_RADIO_TX_POWER_LVL_CUSTOM (0x80) names a raw register value, passed
 * through untouched.
 */
static uint8_t tx_power_resolve(uint8_t requested)
{
#if defined(CONFIG_ANT_DONGLE_TX_POWER_BOOST)
	if (requested & ANTW_RADIO_TX_POWER_LVL_CUSTOM) {
		return requested;
	}

	if (requested == ANTW_RADIO_TX_POWER_LVL_3 ||
	    requested == ANTW_RADIO_TX_POWER_LVL_4) {
		return ANT_DONGLE_TX_POWER_DEFAULT;
	}
#endif
	return requested;
}

static void tx_power_apply(uint8_t ch)
{
	if (ch < ARRAY_SIZE(tx_power)) {
		(void)antr_channel_radio_tx_power_set(ch, tx_power[ch],
						      tx_power_custom[ch]);
	}
}

/* ── Request sub-handler (opcode 0x4D) ─────────────────────────────────────── */

static void handle_request(const uint8_t *body, uint8_t len)
{
	antr_err_t err = 0;
	uint8_t payload[ANTW_MAX_SIZE_VALUE] = {0};
	uint16_t value16;

	if (len < 2) {
		send_response(0, ANTW_MESG_REQUEST_ID, ANTW_INVALID_MESSAGE);
		return;
	}
	uint8_t ch     = body[0];
	uint8_t req_id = body[1];

	switch (req_id) {
#if defined(CONFIG_ANT_DONGLE_HEALTH_COUNTERS)
	case ANTW_MESG_RADIANT_HEALTH_ID: {
		/*
		 * REQUEST-ONLY, which is the whole reason it is safe to add at
		 * all: nothing emits 0xF6 unsolicited, so a host that never
		 * asks sees a byte stream identical to a build without the
		 * feature - and tools/ab_gates.toml requires the conformance
		 * transcripts to stay byte-identical.
		 *
		 * The channel byte is ignored: everything counted is
		 * device-wide, and answering per-channel would imply a
		 * partition of the counters that does not exist.
		 */
		size_t n = ant_health_fill(payload, sizeof(payload));

		if (n == 0u) {
			err = (antr_err_t)ANTW_INVALID_MESSAGE;
			break;
		}
		send_message(ANTW_MESG_RADIANT_HEALTH_ID, payload, (uint8_t)n);
		break;
	}
#endif

	case ANTW_MESG_CAPABILITIES_ID: {
		err = antr_capabilities_get(payload);
		if (!err) {
			send_message(ANTW_MESG_CAPABILITIES_ID, payload,
				     ANTW_MESG_CAPABILITIES_SIZE);
		}
		break;
	}

	case ANTW_MESG_VERSION_ID: {
		err = antr_version_get(payload);
		if (!err) {
			send_message(ANTW_MESG_VERSION_ID, payload,
				     ANTW_MESG_VERSION_SIZE);
		}
		break;
	}

	case ANTW_MESG_CHANNEL_STATUS_ID: {
		payload[0] = ch;
		err = antr_channel_status_get(ch, &payload[1]);
		if (!err) {
			send_message(ANTW_MESG_CHANNEL_STATUS_ID, payload,
				     ANTW_MESG_CHANNEL_STATUS_SIZE);
		}
		break;
	}

	case ANTW_MESG_CHANNEL_ID_ID: {
		payload[0] = ch;
		err = antr_channel_id_get(ch, &value16, &payload[3], &payload[4]);
		if (!err) {
			payload[1] = (uint8_t)value16;
			payload[2] = (uint8_t)(value16 >> 8);
			send_message(ANTW_MESG_CHANNEL_ID_ID, payload,
				     ANTW_MESG_CHANNEL_ID_SIZE);
		}
		break;
	}

	case ANTW_MESG_CHANNEL_MESG_PERIOD_ID: {
		payload[0] = ch;
		err = antr_channel_period_get(ch, &value16);
		if (!err) {
			payload[1] = (uint8_t)value16;
			payload[2] = (uint8_t)(value16 >> 8);
			send_message(ANTW_MESG_CHANNEL_MESG_PERIOD_ID, payload,
				     ANTW_MESG_CHANNEL_MESG_PERIOD_SIZE);
		}
		break;
	}

	case ANTW_MESG_CHANNEL_RADIO_FREQ_ID: {
		payload[0] = ch;
		err = antr_channel_radio_freq_get(ch, &payload[1]);
		if (!err) {
			send_message(ANTW_MESG_CHANNEL_RADIO_FREQ_ID, payload,
				     ANTW_MESG_CHANNEL_RADIO_FREQ_SIZE);
		}
		break;
	}

#ifdef ANTW_MESG_CHANNEL_CRC_MODE_ID
	case ANTW_MESG_CHANNEL_CRC_MODE_ID: {
		payload[0] = ch;
		err = antr_channel_radio_crc_mode_get(ch, &payload[1]);
		if (!err) {
			send_message(ANTW_MESG_CHANNEL_CRC_MODE_ID, payload,
				     ANTW_MESG_CHANNEL_CRC_MODE_SIZE);
		}
		break;
	}
#else
	/*
	 * K1: unresolved, see protocol/ant_wire.yaml.
	 *
	 * The message id of MESG_CHANNEL_CRC_MODE is a Nordic extension that
	 * appears nowhere in this repository and is not in Rev 5.1, so there is
	 * no ANTW_ constant to dispatch on and the read is left unimplemented:
	 * a request for it falls through to the default arm and is answered
	 * ANTW_INVALID_MESSAGE. antr_channel_radio_crc_mode_get() is unchanged
	 * and still declared - only the wire id is missing. When
	 * src/ant_radio_sdk_ant.c recovers the value and it lands in
	 * protocol/ant_wire.yaml, this arm compiles back in with no edit.
	 */
#endif

	case ANTW_MESG_SET_SEARCH_CH_PRIORITY_ID: {
		payload[0] = ch;
		err = antr_search_channel_priority_get(ch, &payload[1]);
		if (!err) {
			send_message(ANTW_MESG_SET_SEARCH_CH_PRIORITY_ID, payload,
				     ANTW_MESG_SET_SEARCH_CH_PRIORITY_SIZE);
		}
		break;
	}

#ifdef ANTW_MESG_PENDING_TRANSMIT_CLEAR_ID
	case ANTW_MESG_PENDING_TRANSMIT_CLEAR_ID: {
		payload[0] = ch;
		err = antr_pending_transmit_get(ch, &payload[1]);
		if (!err) {
			send_message(ANTW_MESG_PENDING_TRANSMIT_CLEAR_ID, payload,
				     ANTW_MESG_PENDING_TRANSMIT_GET_SIZE);
		}
		break;
	}
#else
	/*
	 * K1: unresolved, see protocol/ant_wire.yaml.
	 *
	 * Same situation as the CRC mode above - the reply size
	 * (ANTW_MESG_PENDING_TRANSMIT_GET_SIZE) is known, the message id is
	 * not, and there is nothing legitimate to dispatch on. The read is left
	 * unimplemented rather than guessed; antr_pending_transmit_get() is
	 * still declared and gains its caller back the moment the id resolves.
	 */
#endif

#ifdef CONFIG_RADIANT_SEC_HOST_MESSAGES
	case ANTW_MESG_RADIANT_SEC_STATUS_ID: {
		/*
		 * The only read arm in the family. 0xF2 has none anywhere - a
		 * root key goes in and never comes out, and a request for it
		 * falls through to the default below and is answered
		 * ANTW_INVALID_MESSAGE, which is the same answer a device that
		 * had never heard of 0xF2 would give.
		 */
		err = antr_sec_status_get(ch, payload, (uint8_t)sizeof(payload));
		if (!err) {
			send_message(ANTW_MESG_RADIANT_SEC_STATUS_ID, payload,
				     23);
		}
		break;
	}
#endif /* CONFIG_RADIANT_SEC_HOST_MESSAGES */

	case ANTW_MESG_ANTLIB_CONFIG_ID: {
		payload[0] = ch;
		err = antr_lib_config_get(&payload[1]);
		if (!err) {
			send_message(ANTW_MESG_ANTLIB_CONFIG_ID, payload,
				     ANTW_MESG_ANTLIB_CONFIG_SIZE);
		}
		break;
	}

	case ANTW_MESG_ACTIVE_SEARCH_SHARING_ID: {
		payload[0] = ch;
		err = antr_active_search_sharing_cycles_get(ch, &payload[1]);
		if (!err) {
			send_message(ANTW_MESG_ACTIVE_SEARCH_SHARING_ID, payload,
				     ANTW_MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE);
		}
		break;
	}

	case ANTW_MESG_EVENT_FILTER_CONFIG_ID: {
		payload[0] = ch;
		err = antr_event_filtering_get(&value16);
		if (!err) {
			payload[1] = (uint8_t)value16;
			payload[2] = (uint8_t)(value16 >> 8);
			send_message(ANTW_MESG_EVENT_FILTER_CONFIG_ID, payload,
				     ANTW_MESG_EVENT_FILTER_CONFIG_REQ_SIZE);
		}
		break;
	}

	case ANTW_MESG_CONFIG_ADV_BURST_ID: {
		/* body[0] is 0 for "what can you do" and 1 for "what are you
		 * set to", not a channel, and the two replies are different
		 * lengths.
		 */
#ifdef ANTW_MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE
		uint8_t reply_len =
			ch ? ANTW_MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE :
			     ANTW_MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE;
#else
		/*
		 * K1: unresolved, see protocol/ant_wire.yaml.
		 *
		 * The advanced-burst *capabilities* reply's LEN byte has no
		 * recorded value, and unlike the ids above it has no
		 * legitimate substitute either: it goes on the wire in a
		 * direction this dongle originates, so a guessed length is a
		 * wrong length in front of a host rather than a message we
		 * decline. The configuration form (index 1) is unaffected and
		 * still answers; the capabilities form is refused with
		 * ANTW_INVALID_MESSAGE until src/ant_radio_sdk_ant.c recovers
		 * the value.
		 */
		uint8_t reply_len =
			ch ? ANTW_MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE : 0;
#endif

		if (reply_len == 0) {
			err = ANTW_INVALID_MESSAGE;
			break;
		}

		err = antr_adv_burst_config_get(ch, payload);
		if (!err) {
			send_message(ANTW_MESG_CONFIG_ADV_BURST_ID, payload,
				     reply_len);
		}
		break;
	}

	case ANTW_MESG_SDU_SET_MASK_ID: {
		/* [mask_index, mask[8]]. The 8 mask bytes start one in: the reply is
		 * the same shape as the command that set them, and the size constant
		 * is ANTW_ANT_MAX_PAYLOAD_SIZE *plus* ANTW_CHANNEL_NUM_SIZE precisely
		 * because of that leading index byte. Writing the mask at payload[0]
		 * instead fills the index slot with mask[0], shifts every byte down
		 * one, and pads the end with a zero the host reads as mask[7].
		 */
		payload[0] = ch;
		err = antr_sdu_mask_get(ch, &payload[1]);
		if (!err) {
			send_message(ANTW_MESG_SDU_SET_MASK_ID, payload,
				     ANTW_ANT_MAX_PAYLOAD_SIZE +
				     ANTW_CHANNEL_NUM_SIZE);
		}
		break;
	}

	case ANTW_MESG_ENCRYPT_ENABLE_ID: {
		/* Same leading byte, same reason: the info type is echoed at
		 * payload[0] and the requested data follows it. Each reply size below
		 * counts that byte - 2 = type + 1 mode byte, 5 = type + a 4-byte
		 * crypto ID, 20 = type + 19 bytes of custom user data.
		 */
		payload[0] = ch;
		err = antr_crypto_info_get(ch, &payload[1]);
		if (!err) {
			uint8_t reply_len = 0;

			switch (ch) {
			case ANTW_ENCRYPTION_INFO_GET_SUPPORTED_MODE:
				reply_len = ANTW_MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE;
				break;
			case ANTW_ENCRYPTION_INFO_GET_CRYPTO_ID:
				reply_len = ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE;
				break;
			case ANTW_ENCRYPTION_INFO_GET_CUSTOM_USER_DATA:
				reply_len = ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE;
				break;
			default:
				err = ANTW_INVALID_MESSAGE;
				break;
			}

			if (!err) {
				send_message(ANTW_MESG_ENCRYPT_ENABLE_ID, payload,
					     reply_len);
			}
		}
		break;
	}

	default:
		send_response(ch, ANTW_MESG_REQUEST_ID, ANTW_INVALID_MESSAGE);
		return;
	}

	if (err) {
		LOG_WRN("Request 0x%02X failed: %d", req_id, err);
		send_response(ch, ANTW_MESG_REQUEST_ID, err);
	}
}

/* ── System reset ──────────────────────────────────────────────────────────── */

/* ANTW_MESG_SYSTEM_RESET_ID resets the ANT protocol stack, not the MCU: every
 * host library keeps using the same USB handle after sending reset, and
 * rebooting here would drop the device off the bus mid-session.
 */
static void handle_system_reset(void)
{
	atomic_set(&reset_in_progress, 1);

	(void)antr_stack_reset();

	/* The stack is back to its defaults and every channel is unassigned, so
	 * anything a previous session asked for is gone. Forget it here too,
	 * or a deliberately quiet setting would outlive the reset that a real
	 * stick clears it with.
	 */
	tx_power_reset();

	/* Let the resulting channel-closed events land and be discarded before
	 * the host is told the stack is up again. Real sticks are unresponsive
	 * for rather longer than this.
	 */
	k_sleep(K_MSEC(50));
	atomic_set(&reset_in_progress, 0);
}

/* ── Burst transmit ─────────────────────────────────────────────────────────── */

/*
 * antr_burst_tx() takes ownership of the buffer it is handed until the radio
 * has sent it, but `body` points into the parser's frame buffer, overwritten
 * by the next packet - so each block is copied somewhere that outlives the
 * call and held there. Advanced burst carries up to ANTW_ADV_BURST_BLOCK_MAX
 * (24) bytes per packet; plain burst carries 8.
 */

static uint8_t burst_block[ANTW_ADV_BURST_BLOCK_MAX];
static K_SEM_DEFINE(burst_block_free, 1, 1);

static void handle_burst(uint8_t msg_id, const uint8_t *body, uint8_t len)
{
	/* body: [seq << 5 | channel, data[8|16|24]], bit 7 marks the last
	 * packet of the transfer.
	 */
	uint8_t size = (len >= 1) ? (uint8_t)(len - 1) : 0;

	if (size < 8 || size > ANTW_ADV_BURST_BLOCK_MAX || (size % 8) != 0) {
		send_response(0, msg_id, ANTW_INVALID_MESSAGE);
		return;
	}

	uint8_t ch      = body[0] & ANTW_BURST_HEADER_CHANNEL_MASK;
	uint8_t seq     = (body[0] >> ANTW_BURST_HEADER_SEQ_SHIFT) &
			  ANTW_BURST_HEADER_SEQ_MASK;
	uint8_t segment = ANTR_BURST_SEGMENT_CONTINUE;

	if (seq == 0) {
		segment |= ANTR_BURST_SEGMENT_START;
	}
	if (body[0] & ANTW_BURST_HEADER_LAST) {
		segment |= ANTR_BURST_SEGMENT_END;
	}

	/* Waiting for the stack to release the previous block, rather than
	 * queueing, is what applies back-pressure to the host. The timeout only
	 * exists so a transfer that dies mid-flight cannot wedge this thread.
	 */
	if (k_sem_take(&burst_block_free, K_MSEC(1000)) != 0) {
		/* A counter and nothing more, deliberately. Shortening the
		 * stall does not help anyone: the 1000 ms IS the back-pressure,
		 * so a shorter one makes the host's transfer FAIL where it
		 * currently waits. Zwift uses acknowledged data rather than
		 * burst, so a non-zero reading here is a different host's
		 * problem and worth being able to see from one. */
		ant_health_note(ANT_HEALTH_BURST_STALL);
		send_response(ch, msg_id, ANTW_TRANSFER_IN_PROGRESS);
		return;
	}

	memcpy(burst_block, &body[1], size);

	antr_err_t err = antr_burst_tx(ch, size, burst_block, segment);

	if (err) {
		k_sem_give(&burst_block_free);
		send_response(ch, msg_id, err);
	}

	/* No per-packet acknowledgement on success: like a real stick, the host
	 * learns the outcome from EVENT_TRANSFER_TX_COMPLETED or _TX_FAILED
	 * once the whole transfer has finished.
	 */
}

/* ── Inbound dispatcher ─────────────────────────────────────────────────────── */

static void dispatch(uint8_t msg_id, const uint8_t *body, uint8_t len)
{
	/* Starts as a failure so a too-short body falls through every
	 * `if (len >= N)` below as ANTW_INVALID_MESSAGE, rather than silently
	 * acknowledging a truncated command that never ran.
	 */
	antr_err_t err = ANTW_INVALID_MESSAGE;
	/* body[0] is channel number (or network number for NETWORK_KEY) */
	uint8_t ch = (len > 0) ? body[0] : 0;

	switch (msg_id) {

	case ANTW_MESG_SYSTEM_RESET_ID:
		handle_system_reset();
		send_startup();
		return; /* no RESPONSE_EVENT for reset */

	case ANTW_MESG_NETWORK_KEY_ID:
		/* body: [net_num, key[8]] */
		if (len >= 9) {
			err = antr_network_address_set(body[0], &body[1]);
		}
		break;

	case ANTW_MESG_ASSIGN_CHANNEL_ID:
		/* body: [ch, type, net, (ext_assign optional)] */
		if (len >= 3) {
			uint8_t ext = (len >= 4) ? body[3] : 0x00;
			err = antr_channel_assign(body[0], body[1], body[2], ext);
			if (!err) {
				/* The channel exists now, so the power the host
				 * asked for earlier - or our boosted default,
				 * if it never asked - can finally be applied.
				 */
				tx_power_apply(body[0]);
			}
		}
		break;

	case ANTW_MESG_CHANNEL_ID_ID:
		/* body: [ch, dev_lsb, dev_msb, dev_type, trans_type] */
		if (len >= 5) {
			uint16_t dev = (uint16_t)body[1] |
				       ((uint16_t)body[2] << 8);
			err = antr_channel_id_set(body[0], dev, body[3], body[4]);
		}
		break;

	case ANTW_MESG_CHANNEL_RADIO_FREQ_ID:
		/* body: [ch, freq_offset_from_2400MHz] */
		if (len >= 2) {
			err = antr_channel_radio_freq_set(body[0], body[1]);
		}
		break;

	case ANTW_MESG_CHANNEL_MESG_PERIOD_ID:
		/* body: [ch, period_lsb, period_msb] (32 kHz counts) */
		if (len >= 3) {
			uint16_t period = (uint16_t)body[1] |
					  ((uint16_t)body[2] << 8);
			err = antr_channel_period_set(body[0], period);
		}
		break;

	case ANTW_MESG_CHANNEL_SEARCH_TIMEOUT_ID:
		/* body: [ch, timeout] (2.5 s increments) */
		if (len >= 2) {
			err = antr_channel_search_timeout_set(body[0], body[1]);
		}
		break;

	case ANTW_MESG_SET_LP_SEARCH_TIMEOUT_ID:
		/* body: [ch, timeout] */
		if (len >= 2) {
			err = antr_channel_low_priority_rx_search_timeout_set(
				body[0], body[1]);
		}
		break;

	case ANTW_MESG_CHANNEL_RADIO_TX_POWER_ID:
		/* body: [ch, tx_power] or [ch, tx_power, custom]
		 *
		 * The third byte is an additive extension: a two-byte message
		 * means what it always did, and the byte is only read when the
		 * level carries ANTW_RADIO_TX_POWER_LVL_CUSTOM. Without it, a
		 * custom-power request silently got register 0 (0 dBm on an
		 * nRF52840) - a failure invisible from the far end of the link,
		 * which is why tools/ant_sens.py checks against measured RSSI.
		 */
		if (len >= 2) {
			uint8_t level = tx_power_resolve(body[1]);
			uint8_t custom = (len >= 3) ? body[2] : 0U;

			if (body[0] < ARRAY_SIZE(tx_power)) {
				tx_power[body[0]] = level;
				tx_power_custom[body[0]] = custom;
			}
			/* Still call through for an out-of-range channel, so
			 * the stack returns the error a real stick would.
			 */
			err = antr_channel_radio_tx_power_set(body[0], level,
							      custom);
		}
		break;

	case ANTW_MESG_RADIO_TX_POWER_ID:
		/* body: [filler, tx_power]. Device-wide form (ANT_SetTransmitPower);
		 * 0x60 above is per-channel. The radio contract only exposes the
		 * per-channel setter, so fan it out - failures not propagated,
		 * since a host sets default power before assigning any channel.
		 */
		if (len >= 2) {
			uint8_t level = tx_power_resolve(body[1]);
			uint8_t custom = (len >= 3) ? body[2] : 0U;

			for (uint8_t i = 0; i < CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED;
			     i++) {
				tx_power[i] = level;
				tx_power_custom[i] = custom;
				(void)antr_channel_radio_tx_power_set(i, level,
								      custom);
			}
			err = 0;
		}
		break;

	case ANTW_MESG_OPEN_CHANNEL_ID:
		/* body: [ch] */
		if (len >= 1) {
			err = antr_channel_open(body[0]);
		}
		break;

	case ANTW_MESG_CLOSE_CHANNEL_ID:
		/* body: [ch] */
		if (len >= 1) {
			err = antr_channel_close(body[0]);
		}
		break;

	case ANTW_MESG_UNASSIGN_CHANNEL_ID:
		/* body: [ch] */
		if (len >= 1) {
			err = antr_channel_unassign(body[0]);
		}
		break;

	case ANTW_MESG_OPEN_RX_SCAN_MODE_ID: {
		/*
		 * body: [(synchronous channel packets only, optional)]. That flag
		 * restricts reporting to already-tracked periods; accepted and
		 * ignored here since reporting everything is a superset. len is
		 * not checked - nothing depends on it.
		 *
		 * Real scan mode takes over the whole radio, requiring every
		 * other channel closed first (else ANTW_CLOSE_ALL_CHANNELS).
		 * Channel 0 is assigned as a wildcard background-scan slave via
		 * MESG_ASSIGN_CHANNEL's extended byte (ANTW_EXT_PARAM_ALWAYS_
		 * SEARCH) - the same call ZwiftApp.exe resolves.
		 */
		uint8_t status;
		uint8_t i;

		for (i = 0; i < CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED; i++) {
			if (antr_channel_status_get(i, &status) ==
				    (antr_err_t)ANTW_RESPONSE_NO_ERROR &&
			    (status & 0x03u) >= ANTW_STATUS_SEARCHING_CHANNEL) {
				break;
			}
		}
		if (i < CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED) {
			send_response(0, msg_id, ANTW_CLOSE_ALL_CHANNELS);
			return;
		}

		err = (antr_channel_status_get(0, &status) ==
			       (antr_err_t)ANTW_RESPONSE_NO_ERROR &&
		       (status & 0x03u) == ANTW_STATUS_ASSIGNED_CHANNEL)
			      ? antr_channel_unassign(0)
			      : (antr_err_t)ANTW_RESPONSE_NO_ERROR;
		if (!err) {
			err = antr_channel_assign(0, ANTW_CHANNEL_TYPE_SLAVE, 0,
						  ANTW_EXT_PARAM_ALWAYS_SEARCH);
		}
		if (!err) {
			err = antr_channel_id_set(0, 0, 0, 0);
		}
		if (!err) {
			err = antr_channel_open(0);
		}
		send_response(0, msg_id, err ? err : ANTW_RESPONSE_NO_ERROR);
		return;
	}

	case ANTW_MESG_ANTLIB_CONFIG_ID:
		/* body: [filler, config_bits]. Zero means "clear everything",
		 * which is how hosts drop the extended-message flags between
		 * sessions; there is no separate clear message on the wire.
		 */
		if (len >= 2) {
			err = body[1] ?
			      antr_lib_config_set(body[1]) :
			      antr_lib_config_clear(ANTW_LIB_CONFIG_MASK_ALL);
		}
		break;

	case ANTW_MESG_RX_EXT_MESGS_ENABLE_ID:
		/* body: [filler, enable]. The older, narrower way of asking for
		 * the channel id on every received message. Hosts still send it
		 * instead of MESG_ANTLIB_CONFIG, and without it every broadcast
		 * arrives anonymous and no sensor can be identified.
		 */
		if (len >= 2) {
			err = body[1] ?
			      antr_lib_config_set(
				      ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID) :
			      antr_lib_config_clear(
				      ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID);
		}
		break;

	case ANTW_MESG_SEARCH_WAVEFORM_ID:
		/* body: [ch, waveform_lsb, waveform_msb] */
		if (len >= 3) {
			uint16_t waveform = (uint16_t)body[1] |
					    ((uint16_t)body[2] << 8);
			err = antr_search_waveform_set(body[0], waveform);
		}
		break;

	case ANTW_MESG_PROX_SEARCH_CONFIG_ID:
		/* body: [ch, threshold, (custom threshold optional)] */
		if (len >= 2) {
			uint8_t custom = (len >= 3) ? body[2] : 0;

			err = antr_prox_search_set(body[0], body[1], custom);
		}
		break;

	case ANTW_MESG_SET_SEARCH_CH_PRIORITY_ID:
		/* body: [ch, priority] */
		if (len >= 2) {
			err = antr_search_channel_priority_set(body[0], body[1]);
		}
		break;

	case ANTW_MESG_ACTIVE_SEARCH_SHARING_ID:
		/* body: [ch, cycles] */
		if (len >= 2) {
			err = antr_active_search_sharing_cycles_set(body[0],
								    body[1]);
		}
		break;

	case ANTW_MESG_AUTO_FREQ_CONFIG_ID:
		/* body: [ch, freq0, freq1, freq2] */
		if (len >= 4) {
			err = antr_auto_freq_hop_table_set(body[0], body[1],
							   body[2], body[3]);
		}
		break;

#ifdef ANTW_MESG_CHANNEL_CRC_MODE_ID
	case ANTW_MESG_CHANNEL_CRC_MODE_ID:
		/* body: [filler, mode] */
		if (len >= 2) {
			err = antr_channel_radio_crc_mode_set(body[0], body[1]);
		}
		break;
#else
	/*
	 * K1: unresolved, see protocol/ant_wire.yaml. See the matching note in
	 * handle_request() - the id is a Nordic extension with no recorded
	 * value, so the write is left unimplemented and the message is answered
	 * ANTW_INVALID_MESSAGE by the default arm below.
	 */
#endif

	case ANTW_MESG_ID_LIST_ADD_ID:
		/* body: [ch, dev_lsb, dev_msb, dev_type, trans_type, index] */
		if (len >= 6) {
			err = antr_id_list_add(body[0], &body[1], body[5]);
		}
		break;

	case ANTW_MESG_ID_LIST_CONFIG_ID:
		/* body: [ch, list_size, exclude_flag] */
		if (len >= 3) {
			err = antr_id_list_config(body[0], body[1], body[2]);
		}
		break;

	case ANTW_MESG_EVENT_FILTER_CONFIG_ID:
		/* body: [filter_lsb, filter_msb] */
		if (len >= 2) {
			uint16_t filter = (uint16_t)body[0] |
					  ((uint16_t)body[1] << 8);
			err = antr_event_filtering_set(filter);
		}
		break;

	case ANTW_MESG_CONFIG_ADV_BURST_ID:
		/* body: [filler, config[8..11]]. The filler byte occupies the channel
		 * slot every message has and is not part of the configuration, so the
		 * buffer antr_adv_burst_config_set() documents starts at body[1]:
		 * [enable, rf payload size, required modes, 0, 0, optional modes, 0, 0]
		 * with an optional [stall lsb, stall msb, retry extension].
		 *
		 * Without this, ANTW_MESG_ADV_BURST_DATA_ID is accepted but advanced
		 * burst is never on, so nothing ever leaves at more than 8 bytes a
		 * packet.
		 */
		if (len >= 9 && len <= 12) {
			err = antr_adv_burst_config_set(&body[1],
							(uint8_t)(len - 1));
		}
		break;

	case ANTW_MESG_SDU_CONFIG_ID:
		/* body: [ch, mask_config]. Binds one of the masks below to a channel;
		 * ANTW_INVALID_SDU_MASK (0xFF) turns selective updates back off.
		 */
		if (len >= 2) {
			err = antr_sdu_mask_config(body[0], body[1]);
		}
		break;

	case ANTW_MESG_SDU_SET_MASK_ID:
		/* body: [mask_index, mask[8]]. body[0] is a mask number, not a channel
		 * - masks are defined here and attached to channels by MESG_SDU_CONFIG.
		 */
		if (len >= 9) {
			err = antr_sdu_mask_set(body[0], &body[1]);
		}
		break;

#ifdef CONFIG_ANT_DONGLE_ENCRYPTION
	/* The three writes that can put a channel into AES-CTR mode. Compiled out
	 * by default since no host on this platform sends them. Matching reads
	 * live in handle_request() and are always present.
	 */
	case ANTW_MESG_ENCRYPT_ENABLE_ID:
		/* body: [ch, mode, key_num, decimation_rate] */
		if (len >= 4) {
			err = antr_crypto_channel_enable(body[0], body[1], body[2],
							 body[3]);
		}
		break;

	case ANTW_MESG_SET_ENCRYPT_KEY_ID:
		/* body: [key_num, key[16]]. No wire constant names the key length;
		 * that it is 128 bits is stated only in the prose of
		 * antr_crypto_key_set(), which takes a bare pointer.
		 */
		if (len >= 1 + ANTW_ENCRYPTION_KEY_SIZE) {
			err = antr_crypto_key_set(body[0], &body[1]);
		}
		break;

	case ANTW_MESG_SET_ENCRYPT_INFO_ID:
		/* body: [info_type, data]. How much data depends on the type, and the
		 * length has to be checked against it here: antr_crypto_info_set()
		 * reads a fixed count per type with no size to bound it, so a
		 * truncated command would otherwise hand the radio whatever follows
		 * in the parser's frame buffer.
		 */
		if (len >= 2) {
			uint8_t want = 0;

			switch (body[0]) {
			case ANTW_ENCRYPTION_INFO_SET_CRYPTO_ID:
				want = ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE;
				break;
			case ANTW_ENCRYPTION_INFO_SET_CUSTOM_USER_DATA:
				want = ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE;
				break;
			/* ANTW_ENCRYPTION_INFO_SET_RNG_SEED is deliberately absent.
			 * It is documented as platform specific with no size defined
			 * for it anywhere, and the radio does not take its randomness
			 * from the host regardless - a backend seeds itself at init.
			 * Refusing beats guessing a length.
			 */
			default:
				break;
			}

			if (want && len >= want) {
				err = antr_crypto_info_set(body[0], &body[1]);
			}
		}
		break;
#endif /* CONFIG_ANT_DONGLE_ENCRYPTION */

#ifdef CONFIG_RADIANT_SEC_HOST_MESSAGES
	/*
	 * The RadiANT payload transforms: 0xF1 configure, 0xF2 key, 0xF3 epoch.
	 * 0xF4 is read-only (handle_request()); 0xF5 is pairing, with X25519.
	 * Unlike the encryption block above, the declarations are under the
	 * symbol too - these have exactly one implementation, not a contract
	 * every backend owes.
	 */
	case ANTW_MESG_RADIANT_SEC_CONFIG_ID:
		/* body: [ch, switches, W, page_lo, page_hi] */
		if (len >= 5) {
			err = antr_sec_config(body[0], body[1], body[2],
					      body[3], body[4]);
		}
		break;

	case ANTW_MESG_RADIANT_SET_KEY_ID:
		/* body: [ch, key_bits, key[16]].
		 *
		 * process_byte() wipes its frame buffer after this dispatch
		 * returns - see the note there, including what that wipe does
		 * not cover.
		 */
		if (len >= 18) {
			err = antr_sec_key_set(body[0], body[1], &body[2]);
		}
		break;

	case ANTW_MESG_RADIANT_EPOCH_ID:
		/* body: [ch, flags, epoch u32 LE, us_into_epoch u64 LE] */
		if (len >= 14) {
			uint32_t epoch;
			uint64_t us;
			int      i;

			epoch = (uint32_t)body[2] | ((uint32_t)body[3] << 8) |
				((uint32_t)body[4] << 16) |
				((uint32_t)body[5] << 24);

			us = 0u;
			for (i = 7; i >= 0; i--) {
				us = (us << 8) | (uint64_t)body[6 + i];
			}

			err = antr_sec_epoch_set(body[0], body[1], epoch, us);
		}
		break;

#ifdef CONFIG_RADIANT_SEC_PAIRING_X25519
	case ANTW_MESG_RADIANT_PAIRING_ID: {
		/* body: [ch, subcmd, arg...]. Reply is a 0xF5 of its own, not a
		 * response code, since three of the four sub-commands reply. */
		uint8_t reply[2 + 32];
		uint8_t reply_len = 0u;

		if (len < 2) {
			break;
		}
		err = antr_sec_pair(body[0], body[1],
				    (len > 2) ? &body[2] : NULL,
				    (uint8_t)(len - 2), reply,
				    (uint8_t)sizeof(reply), &reply_len);
		if (!err && reply_len > 0u) {
			send_message(ANTW_MESG_RADIANT_PAIRING_ID, reply,
				     reply_len);
		}
		/*
		 * The scalar just crossed the frame buffer. process_byte()
		 * wipes it after this dispatch for the same reason it wipes a
		 * 0xF2, and with the same honest limit: the USB ring and the
		 * transport buffer beneath it are not reachable from here.
		 */
		memset(reply, 0, sizeof(reply));
		break;
	}
#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */
#endif /* CONFIG_RADIANT_SEC_HOST_MESSAGES */

#ifdef ANTW_MESG_ECS_ENABLE_ID
	case ANTW_MESG_ECS_ENABLE_ID:
		/* body: [filler, enable] */
		if (len >= 2) {
			err = antr_enhanced_channel_spacing_enable(body[1]);
		}
		break;
#else
	/*
	 * K1: unresolved, see protocol/ant_wire.yaml. Enhanced channel spacing
	 * is a Nordic extension whose message id appears nowhere in this
	 * repository and is not in Rev 5.1, so there is nothing to dispatch on.
	 * antr_enhanced_channel_spacing_enable() stays in the contract and
	 * regains its caller when src/ant_radio_sdk_ant.c recovers the id.
	 */
#endif

#ifdef ANTW_MESG_PENDING_TRANSMIT_CLEAR_ID
	case ANTW_MESG_PENDING_TRANSMIT_CLEAR_ID:
		/* body: [ch] */
		if (len >= 1) {
			uint8_t cleared;

			err = antr_pending_transmit_clear(body[0], &cleared);
		}
		break;
#else
	/*
	 * K1: unresolved, see protocol/ant_wire.yaml. Same id as the read arm
	 * in handle_request(); neither is dispatchable until it resolves.
	 */
#endif

	case ANTW_MESG_REQUEST_ID:
		handle_request(body, len);
		return; /* handle_request sends its own response */

	case ANTW_MESG_BROADCAST_DATA_ID:
		/* body: [ch, data[8]] */
		if (len >= 9) {
			err = antr_broadcast_message_tx(body[0], 8, &body[1]);
		}
		break;

	case ANTW_MESG_ACKNOWLEDGED_DATA_ID:
		/* body: [ch, data[8]] */
		if (len >= 9) {
			err = antr_acknowledge_message_tx(body[0], 8, &body[1]);
		}
		break;

	case ANTW_MESG_BURST_DATA_ID:
	case ANTW_MESG_ADV_BURST_DATA_ID:
		handle_burst(msg_id, body, len);
		return; /* handle_burst answers only on failure */

	default:
		LOG_WRN("Unknown msg_id 0x%02X len=%d", msg_id, len);
		send_response(ch, msg_id, ANTW_INVALID_MESSAGE);
		return;
	}

	send_response(ch, msg_id, err ? err : ANTW_RESPONSE_NO_ERROR);
}

/* ── Frame parser state machine ─────────────────────────────────────────────── */

enum parser_state {
	S_SYNC,
	S_LEN,
	S_ID,
	S_BODY,
	S_CSUM,
};

static void process_byte(uint8_t b)
{
	static enum parser_state state = S_SYNC;
	static uint8_t  msg_len;
	static uint8_t  msg_id;
	/* Sized from the largest value the LEN byte may carry, which counts the
	 * leading channel byte as well as the payload. An advanced-burst data
	 * message alone reaches 25, so a buffer sized from the longest message
	 * a *host* sends would fail handle_burst()'s size % 8 check and reject
	 * every 24-byte burst packet without a word.
	 */
	static uint8_t  msg_body[ANTW_MAX_SIZE_VALUE]; /* channel + payload */
	static uint8_t  body_idx;
	static uint8_t  running_xor;

	switch (state) {
	case S_SYNC:
		if (b == ANTW_SYNC_TX) {
			/* Seed with SYNC, not 0: it is part of the checksum. */
			running_xor = b;
			state = S_LEN;
		} else {
			/* A byte that was not SYNC while hunting for a frame
			 * start - noise, or the tail of a frame this parser
			 * lost sync on. */
			ant_health_note(ANT_HEALTH_HOST_DESYNC);
		}
		break;

	case S_LEN:
		msg_len = b;
		running_xor ^= b;
		body_idx = 0;
		state = S_ID;
		break;

	case S_ID:
		msg_id = b;
		running_xor ^= b;
		state = (msg_len == 0) ? S_CSUM : S_BODY;
		break;

	case S_BODY:
		if (body_idx < sizeof(msg_body)) {
			msg_body[body_idx] = b;
		}
		body_idx++;
		running_xor ^= b;
		if (body_idx >= msg_len) {
			state = S_CSUM;
		}
		break;

	case S_CSUM:
		if (b == running_xor) {
			dispatch(msg_id, msg_body,
				 MIN(msg_len, (uint8_t)sizeof(msg_body)));
#ifdef CONFIG_RADIANT_SEC_HOST_MESSAGES
			if (msg_id == ANTW_MESG_RADIANT_SET_KEY_ID
#ifdef CONFIG_RADIANT_SEC_PAIRING_X25519
			    /* 0xF5 sub-command 0x02 carries the host's private
			     * X25519 scalar, wiped on the same grounds as a root
			     * key. Checked by message id alone, not sub-command:
			     * a wipe that had to parse the body first would be
			     * one parsing bug away from keeping a private key. */
			    || msg_id == ANTW_MESG_RADIANT_PAIRING_ID
#endif
			) {
				/*
				 * A root key just crossed this static, long-lived
				 * buffer, which would otherwise hold it until a
				 * later message overwrote it - indefinitely on a
				 * quiet link.
				 *
				 * DOES NOT COVER: the same bytes passed through
				 * the USB ring buffer and transport buffer beneath
				 * it, neither reachable or wiped from here, and
				 * were in host memory before that. This narrows
				 * the exposure window, it does not close it.
				 *
				 * Unconditional: a key rejected for bad length was
				 * still a key.
				 */
				memset(msg_body, 0, sizeof(msg_body));
			}
#endif
		} else {
			LOG_WRN("Checksum err: got 0x%02X exp 0x%02X", b,
				running_xor);
			ant_health_note(ANT_HEALTH_HOST_CSUM);
		}
		state = S_SYNC;
		break;
	}
}

/* ── Bridge thread ─────────────────────────────────────────────────────────── */

static void bridge_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	LOG_INF("ANT serial bridge thread started");

	while (1) {
		/* Block until the USB OUT callback signals new data */
		k_sem_take(&ant_rx_sem, K_FOREVER);

		/* Drain the ring buffer completely before sleeping again */
		uint8_t b;
		while (ring_buf_get(&ant_rx_ring_buf, &b, 1) == 1) {
			process_byte(b);
		}

		usb_ant_resume_rx();
	}
}

/* ── Radio event handler (outbound path, chip → host) ───────────────────────── */

/*
 * The one definition of antr_on_message() in the image; the backend calls it,
 * nothing registers it. The struct it takes is wire-shaped on purpose - three
 * fields, no unions - since this function's whole job is wrapping those bytes
 * in a frame.
 *
 * Runs on the backend's own thread/work queue/callback context, never
 * re-entrantly, never before antr_init() has returned. Touches only
 * statically-initialised state, so it does not depend on the bridge thread
 * existing - keep it that way: antr_init() runs before
 * ant_serial_bridge_init().
 */
#if defined(CONFIG_ANT_DONGLE_RX_TAP)
/*
 * The RadiANT bridge's ingest point. Declared here rather than in a header
 * because that is the whole hook: one declaration and one call, both compiled
 * out entirely in any image that does not set the symbol - which is every
 * apps/dongle build, so that image stays byte-identical and the `zero-cost` CI
 * job keeps its meaning.
 *
 * Resolved at link time, exactly like antr_on_message() itself and for the
 * same reason: there is one definition per image (apps/dongle_thread/src/
 * rx_tap.c) and nothing to register, so there is nothing here that can fail.
 * It reads a copy and returns; what goes out the transport below is unchanged.
 */
void ant_dongle_rx_tap(const struct antr_msg *msg);
#endif

void antr_on_message(const struct antr_msg *msg)
{
#if defined(CONFIG_ANT_DONGLE_RX_TAP)
	ant_dongle_rx_tap(msg);
#endif

	/*
	 * The RGB activity indicator's whole ingest: one atomic increment, and
	 * only for the data-bearing message IDs. A macro expanding to nothing
	 * when the symbol is off, so the stock image is unchanged.
	 *
	 * Not folded into ant_dongle_rx_tap() above because that hook is
	 * resolved at link time with one definition per image, already owned by
	 * apps/dongle_thread/src/rx_tap.c - two features cannot share it.
	 */
	ant_rgb_led_note_msg(msg->id);

	/*
	 * Wire format: [0xA4][LEN][ID][channel+payload...][XOR checksum]
	 * Total size  = LEN + 4 bytes.
	 */
	uint8_t msg_len = msg->len;
	uint8_t msg_id  = msg->id;
	const uint8_t *msg_data = msg->data;

	if (atomic_get(&reset_in_progress)) {
		return;
	}

	/* A channel event arrives as [channel, MESG_EVENT_ID, code]. Three of
	 * them mean the burst handler has finished with the block it was given,
	 * so the next packet from the host may overwrite it.
	 */
	if (msg_id == ANTW_MESG_RESPONSE_EVENT_ID && msg_len >= 3 &&
	    msg_data[1] == ANTW_MESG_EVENT_ID) {
		switch (msg_data[2]) {
		case ANTW_EVENT_TRANSFER_NEXT_DATA_BLOCK:
			k_sem_give(&burst_block_free);
			/* Internal flow control for the buffer handover above.
			 * A real stick frames bursts itself and never puts this
			 * on the wire, so neither do we.
			 */
			return;
		case ANTW_EVENT_TRANSFER_TX_COMPLETED:
		case ANTW_EVENT_TRANSFER_TX_FAILED:
			k_sem_give(&burst_block_free);
			break;
		default:
			break;
		}
	}

	/* Guard against oversized messages */
	if (msg_len > ANTW_MAX_SIZE_VALUE) {
		LOG_WRN("Oversized ANT event: id=0x%02X len=%d", msg_id, msg_len);
		return;
	}

	/* Sized like send_message(), from ANTW_MAX_FRAME_SIZE - never
	 * ANTW_MESG_MAX_SIZE, which is one byte short since it doesn't count
	 * the channel byte the LEN ceiling does.
	 */
	uint8_t buf[ANTW_MAX_FRAME_SIZE];
	uint8_t xor = ANTW_SYNC_TX;

	buf[0] = ANTW_SYNC_TX;
	buf[1] = msg_len;   xor ^= msg_len;
	buf[2] = msg_id;    xor ^= msg_id;

	for (uint8_t i = 0; i < msg_len; i++) {
		buf[3 + i] = msg_data[i];
		xor ^= msg_data[i];
	}
	buf[3 + msg_len] = xor;

	int ret = usb_ant_send_async(buf, (size_t)(4 + msg_len));
	if (ret) {
		LOG_WRN("Dropped ANT event 0x%02X due to USB TX backlog: %d",
			msg_id, ret);
	}
}

/* ── Public init ────────────────────────────────────────────────────────────── */

int ant_serial_bridge_init(void)
{
	tx_power_reset();

	/* No callback registration: antr_on_message() above is resolved at link
	 * time, so there is nothing here that can fail and nothing to report.
	 */
	k_thread_create(&bridge_thread_data, bridge_stack,
			K_THREAD_STACK_SIZEOF(bridge_stack),
			bridge_thread_fn, NULL, NULL, NULL,
			BRIDGE_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&bridge_thread_data, "ant_bridge");

	return 0;
}
