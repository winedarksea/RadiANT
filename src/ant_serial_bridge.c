/*
 * ANT Serial Bridge — translates the ANT USB serial protocol (0xA4-framed
 * messages) to/from the on-chip ANT stack API (ant_interface.h).
 *
 * Inbound (host → chip):
 *   Parser thread drains ant_rx_ring_buf byte by byte through a state machine,
 *   validates the XOR checksum, and calls the matching ant_interface.h function.
 *   Each command gets a MESG_RESPONSE_EVENT_ID (0x40) acknowledgement.
 *
 * Outbound (chip → host):
 *   ant_serial_bridge_init() registers ant_evt_handler() and starts the parser
 *   thread. The handler wraps every ANT stack event in a serial frame and
 *   pushes it to usb_ant_send().
 *
 * Wire frame format (ANT Serial Protocol):
 *   [SYNC=0xA4] [LEN] [ID] [payload: LEN bytes] [XOR checksum]
 *
 * The checksum is the XOR of every preceding byte *including the SYNC byte*.
 * Leaving SYNC out yields a value that differs by exactly 0xA4, so every frame
 * a real host sends fails validation and is dropped without a word, and every
 * frame we send is rejected the same way at the other end. Two implementations
 * that both omit it agree with each other perfectly and with nobody else -
 * which is how this survived a passing test suite.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

#include "ant_transport.h"

#include "ant_interface.h"
#include "ant_parameters.h"
/* ant_host_init.h is the nRF5340 dual-core cpuapp header (CONFIG_ANT_NP_HOST,
 * which depends on SOC_NRF5340_CPUAPP). Single-core targets such as the
 * nRF52840 use ant_init.h. Both declare ant_init()/ant_cb_register(), so
 * picking the wrong one compiles and then misbehaves.
 */
#if defined(CONFIG_ANT_NP_HOST)
#include "ant_host_init.h"
#else
#include "ant_init.h"
#endif

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
#define BRIDGE_PRIORITY    5

K_THREAD_STACK_DEFINE(bridge_stack, BRIDGE_STACK_SIZE);
static struct k_thread bridge_thread_data;

/* Set while a system reset is tearing channels down, so the resulting
 * EVENT_CHANNEL_CLOSED storm is not forwarded to the host. A real stick emits
 * nothing between the reset command and the startup message.
 */
static atomic_t reset_in_progress;

/* ── Frame helpers ─────────────────────────────────────────────────────────── */

/* Build and transmit a MESG_RESPONSE_EVENT_ID (0x40) reply. */
static void send_response(uint8_t ch, uint8_t msg_id, uint8_t code)
{
	uint8_t buf[7];
	buf[0] = MESG_TX_SYNC;          /* 0xA4 */
	buf[1] = MESG_RESPONSE_EVENT_SIZE; /* 3 */
	buf[2] = MESG_RESPONSE_EVENT_ID;   /* 0x40 */
	buf[3] = ch;
	buf[4] = msg_id;
	buf[5] = code;
	buf[6] = buf[0] ^ buf[1] ^ buf[2] ^ buf[3] ^ buf[4] ^ buf[5];
	usb_ant_send(buf, sizeof(buf));
}

/* Startup message sent after ant_stack_reset(). */
static void send_startup(void)
{
	uint8_t buf[5];
	buf[0] = MESG_TX_SYNC;
	buf[1] = MESG_STARTUP_MESG_SIZE;  /* 1 */
	buf[2] = MESG_STARTUP_MESG_ID;    /* 0x6F */
	buf[3] = 0x00;                     /* reset by command */
	buf[4] = buf[0] ^ buf[1] ^ buf[2] ^ buf[3];
	usb_ant_send(buf, sizeof(buf));
}

static void send_message(uint8_t msg_id, const uint8_t *payload, uint8_t len)
{
	uint8_t buf[MESG_MAX_SIZE_VALUE + 4];
	uint8_t xor = MESG_TX_SYNC ^ len ^ msg_id;

	buf[0] = MESG_TX_SYNC;
	buf[1] = len;
	buf[2] = msg_id;

	for (uint8_t index = 0; index < len; index++) {
		buf[3 + index] = payload[index];
		xor ^= payload[index];
	}

	buf[3 + len] = xor;
	usb_ant_send(buf, len + 4U);
}

/* ── Request sub-handler (opcode 0x4D) ─────────────────────────────────────── */

static void handle_request(const uint8_t *body, uint8_t len)
{
	ant_err_t err = 0;
	uint8_t payload[MESG_MAX_SIZE_VALUE] = {0};
	uint16_t value16;

	if (len < 2) {
		send_response(0, MESG_REQUEST_ID, INVALID_MESSAGE);
		return;
	}
	uint8_t ch     = body[0];
	uint8_t req_id = body[1];

	switch (req_id) {
	case MESG_CAPABILITIES_ID: {
		err = ant_capabilities_get(payload);
		if (!err) {
			send_message(MESG_CAPABILITIES_ID, payload,
				     MESG_CAPABILITIES_SIZE);
		}
		break;
	}

	case MESG_VERSION_ID: {
		err = ant_version_get(payload);
		if (!err) {
			send_message(MESG_VERSION_ID, payload, MESG_VERSION_SIZE);
		}
		break;
	}

	case MESG_CHANNEL_STATUS_ID: {
		payload[0] = ch;
		err = ant_channel_status_get(ch, &payload[1]);
		if (!err) {
			send_message(MESG_CHANNEL_STATUS_ID, payload,
				     MESG_CHANNEL_STATUS_SIZE);
		}
		break;
	}

	case MESG_CHANNEL_ID_ID: {
		payload[0] = ch;
		err = ant_channel_id_get(ch, &value16, &payload[3], &payload[4]);
		if (!err) {
			payload[1] = (uint8_t)value16;
			payload[2] = (uint8_t)(value16 >> 8);
			send_message(MESG_CHANNEL_ID_ID, payload,
				     MESG_CHANNEL_ID_SIZE);
		}
		break;
	}

	case MESG_CHANNEL_MESG_PERIOD_ID: {
		payload[0] = ch;
		err = ant_channel_period_get(ch, &value16);
		if (!err) {
			payload[1] = (uint8_t)value16;
			payload[2] = (uint8_t)(value16 >> 8);
			send_message(MESG_CHANNEL_MESG_PERIOD_ID, payload,
				     MESG_CHANNEL_MESG_PERIOD_SIZE);
		}
		break;
	}

	case MESG_CHANNEL_RADIO_FREQ_ID: {
		payload[0] = ch;
		err = ant_channel_radio_freq_get(ch, &payload[1]);
		if (!err) {
			send_message(MESG_CHANNEL_RADIO_FREQ_ID, payload,
				     MESG_CHANNEL_RADIO_FREQ_SIZE);
		}
		break;
	}

	case MESG_CHANNEL_CRC_MODE_ID: {
		payload[0] = ch;
		err = ant_channel_radio_crc_mode_get(ch, &payload[1]);
		if (!err) {
			send_message(MESG_CHANNEL_CRC_MODE_ID, payload,
				     MESG_CHANNEL_CRC_MODE_SIZE);
		}
		break;
	}

	case MESG_SET_SEARCH_CH_PRIORITY_ID: {
		payload[0] = ch;
		err = ant_search_channel_priority_get(ch, &payload[1]);
		if (!err) {
			send_message(MESG_SET_SEARCH_CH_PRIORITY_ID, payload,
				     MESG_SET_SEARCH_CH_PRIORITY_SIZE);
		}
		break;
	}

	case MESG_PENDING_TRANSMIT_CLEAR_ID: {
		payload[0] = ch;
		err = ant_pending_transmit(ch, &payload[1]);
		if (!err) {
			send_message(MESG_PENDING_TRANSMIT_CLEAR_ID, payload,
				     MESG_PENDING_TRANSMIT_GET_SIZE);
		}
		break;
	}

	case MESG_ANTLIB_CONFIG_ID: {
		payload[0] = ch;
		err = ant_lib_config_get(&payload[1]);
		if (!err) {
			send_message(MESG_ANTLIB_CONFIG_ID, payload,
				     MESG_ANTLIB_CONFIG_SIZE);
		}
		break;
	}

	case MESG_ACTIVE_SEARCH_SHARING_ID: {
		payload[0] = ch;
		err = ant_active_search_sharing_cycles_get(ch, &payload[1]);
		if (!err) {
			send_message(MESG_ACTIVE_SEARCH_SHARING_ID, payload,
				     MESG_ACTIVE_SEARCH_SHARING_REQ_SIZE);
		}
		break;
	}

	case MESG_EVENT_FILTER_CONFIG_ID: {
		payload[0] = ch;
		err = ant_event_filtering_get(&value16);
		if (!err) {
			payload[1] = (uint8_t)value16;
			payload[2] = (uint8_t)(value16 >> 8);
			send_message(MESG_EVENT_FILTER_CONFIG_ID, payload,
				     MESG_EVENT_FILTER_CONFIG_REQ_SIZE);
		}
		break;
	}

	case MESG_CONFIG_ADV_BURST_ID: {
		err = ant_adv_burst_config_get(ch, payload);
		if (!err) {
			send_message(MESG_CONFIG_ADV_BURST_ID, payload,
				     ch ? MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE :
				     MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE);
		}
		break;
	}

	case MESG_SDU_SET_MASK_ID: {
		/* [mask_index, mask[8]]. The 8 mask bytes start one in: the reply is
		 * the same shape as the command that set them, and the size constant
		 * is MESG_ANT_MAX_PAYLOAD_SIZE *plus* MESG_CHANNEL_NUM_SIZE precisely
		 * because of that leading index byte. Writing the mask at payload[0]
		 * instead fills the index slot with mask[0], shifts every byte down
		 * one, and pads the end with a zero the host reads as mask[7].
		 */
		payload[0] = ch;
		err = ant_sdu_mask_get(ch, &payload[1]);
		if (!err) {
			send_message(MESG_SDU_SET_MASK_ID, payload,
				     MESG_ANT_MAX_PAYLOAD_SIZE +
				     MESG_CHANNEL_NUM_SIZE);
		}
		break;
	}

	case MESG_ENCRYPT_ENABLE_ID: {
		/* Same leading byte, same reason: the info type is echoed at
		 * payload[0] and the requested data follows it. Each reply size below
		 * counts that byte - 2 = type + 1 mode byte, 5 = type + a 4-byte
		 * crypto ID, 20 = type + 19 bytes of custom user data.
		 */
		payload[0] = ch;
		err = ant_crypto_info_get(ch, &payload[1]);
		if (!err) {
			uint8_t reply_len = 0;

			switch (ch) {
			case ENCRYPTION_INFO_GET_SUPPORTED_MODE:
				reply_len = MESG_CONFIG_ENCRYPT_REQ_CAPABILITIES_SIZE;
				break;
			case ENCRYPTION_INFO_GET_CRYPTO_ID:
				reply_len = MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE;
				break;
			case ENCRYPTION_INFO_GET_CUSTOM_USER_DATA:
				reply_len = MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE;
				break;
			default:
				err = INVALID_MESSAGE;
				break;
			}

			if (!err) {
				send_message(MESG_ENCRYPT_ENABLE_ID, payload, reply_len);
			}
		}
		break;
	}

	default:
		send_response(ch, MESG_REQUEST_ID, INVALID_MESSAGE);
		return;
	}

	if (err) {
		LOG_WRN("Request 0x%02X failed: %d", req_id, err);
		send_response(ch, MESG_REQUEST_ID, (uint8_t)err);
	}
}

/* ── System reset ──────────────────────────────────────────────────────────── */

/*
 * MESG_SYSTEM_RESET resets the ANT protocol stack, not the MCU.
 *
 * Every host library (openant, Zwift, Golden Cheetah) opens the device, sends
 * a reset, and then keeps using the same USB handle. Rebooting here drops the
 * device off the bus, and the host's next transfer fails on a stale pipe -
 * which is exactly what a session start looks like, so nothing works.
 */
static void handle_system_reset(void)
{
	atomic_set(&reset_in_progress, 1);

	(void)ant_stack_reset();

	/* Let the resulting channel-closed events land and be discarded before
	 * the host is told the stack is up again. Real sticks are unresponsive
	 * for rather longer than this.
	 */
	k_sleep(K_MSEC(50));
	atomic_set(&reset_in_progress, 0);
}

/* ── Burst transmit ─────────────────────────────────────────────────────────── */

/*
 * The serial protocol streams a burst as a run of packets, but
 * ant_burst_handler_request() takes ownership of the buffer it is handed and
 * keeps it until the radio has sent it. `body` points into the parser's frame
 * buffer, which the very next packet overwrites, so each block has to be
 * copied somewhere that outlives the call - and then held there.
 *
 * Advanced burst carries up to 24 bytes per packet; plain burst carries 8.
 */
#define BURST_BLOCK_MAX 24

static uint8_t burst_block[BURST_BLOCK_MAX];
static K_SEM_DEFINE(burst_block_free, 1, 1);

static void handle_burst(uint8_t msg_id, const uint8_t *body, uint8_t len)
{
	/* body: [seq << 5 | channel, data[8|16|24]], bit 7 marks the last
	 * packet of the transfer.
	 */
	uint8_t size = (len >= 1) ? (uint8_t)(len - 1) : 0;

	if (size < 8 || size > BURST_BLOCK_MAX || (size % 8) != 0) {
		send_response(0, msg_id, INVALID_MESSAGE);
		return;
	}

	uint8_t ch      = body[0] & 0x1F;
	uint8_t seq     = (body[0] >> 5) & 0x03;
	uint8_t segment = BURST_SEGMENT_CONTINUE;

	if (seq == 0) {
		segment |= BURST_SEGMENT_START;
	}
	if (body[0] & 0x80) {
		segment |= BURST_SEGMENT_END;
	}

	/* Waiting for the stack to release the previous block, rather than
	 * queueing, is what applies back-pressure to the host. The timeout only
	 * exists so a transfer that dies mid-flight cannot wedge this thread.
	 */
	if (k_sem_take(&burst_block_free, K_MSEC(1000)) != 0) {
		send_response(ch, msg_id, TRANSFER_IN_PROGRESS);
		return;
	}

	memcpy(burst_block, &body[1], size);

	ant_err_t err = ant_burst_handler_request(ch, size, burst_block, segment);

	if (err) {
		k_sem_give(&burst_block_free);
		send_response(ch, msg_id, (uint8_t)err);
	}

	/* No per-packet acknowledgement on success: like a real stick, the host
	 * learns the outcome from EVENT_TRANSFER_TX_COMPLETED or _TX_FAILED
	 * once the whole transfer has finished.
	 */
}

/* ── Inbound dispatcher ─────────────────────────────────────────────────────── */

static void dispatch(uint8_t msg_id, const uint8_t *body, uint8_t len)
{
	/* Starts as a failure so a command whose body is too short falls through
	 * every `if (len >= N)` below and is reported as INVALID_MESSAGE. Starting
	 * at 0 instead acknowledges a truncated command with RESPONSE_NO_ERROR
	 * without ever having run it, and the host has no way to tell.
	 */
	ant_err_t err = INVALID_MESSAGE;
	/* body[0] is channel number (or network number for NETWORK_KEY) */
	uint8_t ch = (len > 0) ? body[0] : 0;

	switch (msg_id) {

	case MESG_SYSTEM_RESET_ID:
		handle_system_reset();
		send_startup();
		return; /* no RESPONSE_EVENT for reset */

	case MESG_NETWORK_KEY_ID:
		/* body: [net_num, key[8]] */
		if (len >= 9) {
			err = ant_network_address_set(body[0], &body[1]);
		}
		break;

	case MESG_ASSIGN_CHANNEL_ID:
		/* body: [ch, type, net, (ext_assign optional)] */
		if (len >= 3) {
			uint8_t ext = (len >= 4) ? body[3] : 0x00;
			err = ant_channel_assign(body[0], body[1], body[2], ext);
		}
		break;

	case MESG_CHANNEL_ID_ID:
		/* body: [ch, dev_lsb, dev_msb, dev_type, trans_type] */
		if (len >= 5) {
			uint16_t dev = (uint16_t)body[1] |
				       ((uint16_t)body[2] << 8);
			err = ant_channel_id_set(body[0], dev, body[3], body[4]);
		}
		break;

	case MESG_CHANNEL_RADIO_FREQ_ID:
		/* body: [ch, freq_offset_from_2400MHz] */
		if (len >= 2) {
			err = ant_channel_radio_freq_set(body[0], body[1]);
		}
		break;

	case MESG_CHANNEL_MESG_PERIOD_ID:
		/* body: [ch, period_lsb, period_msb] (32 kHz counts) */
		if (len >= 3) {
			uint16_t period = (uint16_t)body[1] |
					  ((uint16_t)body[2] << 8);
			err = ant_channel_period_set(body[0], period);
		}
		break;

	case MESG_CHANNEL_SEARCH_TIMEOUT_ID:
		/* body: [ch, timeout] (2.5 s increments) */
		if (len >= 2) {
			err = ant_channel_search_timeout_set(body[0], body[1]);
		}
		break;

	case MESG_SET_LP_SEARCH_TIMEOUT_ID:
		/* body: [ch, timeout] */
		if (len >= 2) {
			err = ant_channel_low_priority_rx_search_timeout_set(
				body[0], body[1]);
		}
		break;

	case MESG_CHANNEL_RADIO_TX_POWER_ID:
		/* body: [ch, tx_power] */
		if (len >= 2) {
			err = ant_channel_radio_tx_power_set(body[0], body[1], 0);
		}
		break;

	case MESG_RADIO_TX_POWER_ID:
		/* body: [filler, tx_power]. The device-wide form, which every ANT
		 * stick answers and which is what ANT_SetTransmitPower() sends;
		 * 0x60 above is the per-channel one. sdk-ant exposes only the
		 * per-channel setter, so fan it out.
		 *
		 * Per-channel failures are deliberately not propagated: a host
		 * sets the default power before assigning any channel, and
		 * refusing the message because channel 7 is unassigned would fail
		 * a command a real stick accepts.
		 */
		if (len >= 2) {
			for (uint8_t i = 0; i < CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED;
			     i++) {
				(void)ant_channel_radio_tx_power_set(i, body[1], 0);
			}
			err = 0;
		}
		break;

	case MESG_OPEN_CHANNEL_ID:
		/* body: [ch] */
		if (len >= 1) {
			err = ant_channel_open(body[0]);
		}
		break;

	case MESG_CLOSE_CHANNEL_ID:
		/* body: [ch] */
		if (len >= 1) {
			err = ant_channel_close(body[0]);
		}
		break;

	case MESG_UNASSIGN_CHANNEL_ID:
		/* body: [ch] */
		if (len >= 1) {
			err = ant_channel_unassign(body[0]);
		}
		break;

	case MESG_ANTLIB_CONFIG_ID:
		/* body: [filler, config_bits]. Zero means "clear everything",
		 * which is how hosts drop the extended-message flags between
		 * sessions; there is no separate clear message on the wire.
		 */
		if (len >= 2) {
			err = body[1] ?
			      ant_lib_config_set(body[1]) :
			      ant_lib_config_clear(ANT_LIB_CONFIG_MASK_ALL);
		}
		break;

	case MESG_RX_EXT_MESGS_ENABLE_ID:
		/* body: [filler, enable]. The older, narrower way of asking for
		 * the channel id on every received message. Hosts still send it
		 * instead of MESG_ANTLIB_CONFIG, and without it every broadcast
		 * arrives anonymous and no sensor can be identified.
		 */
		if (len >= 2) {
			err = body[1] ?
			      ant_lib_config_set(
				      ANT_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID) :
			      ant_lib_config_clear(
				      ANT_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID);
		}
		break;

	case MESG_SEARCH_WAVEFORM_ID:
		/* body: [ch, waveform_lsb, waveform_msb] */
		if (len >= 3) {
			uint16_t waveform = (uint16_t)body[1] |
					    ((uint16_t)body[2] << 8);
			err = ant_search_waveform_set(body[0], waveform);
		}
		break;

	case MESG_PROX_SEARCH_CONFIG_ID:
		/* body: [ch, threshold, (custom threshold optional)] */
		if (len >= 2) {
			uint8_t custom = (len >= 3) ? body[2] : 0;

			err = ant_prox_search_set(body[0], body[1], custom);
		}
		break;

	case MESG_SET_SEARCH_CH_PRIORITY_ID:
		/* body: [ch, priority] */
		if (len >= 2) {
			err = ant_search_channel_priority_set(body[0], body[1]);
		}
		break;

	case MESG_ACTIVE_SEARCH_SHARING_ID:
		/* body: [ch, cycles] */
		if (len >= 2) {
			err = ant_active_search_sharing_cycles_set(body[0],
								   body[1]);
		}
		break;

	case MESG_AUTO_FREQ_CONFIG_ID:
		/* body: [ch, freq0, freq1, freq2] */
		if (len >= 4) {
			err = ant_auto_freq_hop_table_set(body[0], body[1],
							  body[2], body[3]);
		}
		break;

	case MESG_CHANNEL_CRC_MODE_ID:
		/* body: [filler, mode] */
		if (len >= 2) {
			err = ant_channel_radio_crc_mode_set(body[0], body[1]);
		}
		break;

	case MESG_ID_LIST_ADD_ID:
		/* body: [ch, dev_lsb, dev_msb, dev_type, trans_type, index] */
		if (len >= 6) {
			err = ant_id_list_add(body[0], (uint8_t *)&body[1],
					      body[5]);
		}
		break;

	case MESG_ID_LIST_CONFIG_ID:
		/* body: [ch, list_size, exclude_flag] */
		if (len >= 3) {
			err = ant_id_list_config(body[0], body[1], body[2]);
		}
		break;

	case MESG_EVENT_FILTER_CONFIG_ID:
		/* body: [filter_lsb, filter_msb] */
		if (len >= 2) {
			uint16_t filter = (uint16_t)body[0] |
					  ((uint16_t)body[1] << 8);
			err = ant_event_filtering_set(filter);
		}
		break;

	case MESG_CONFIG_ADV_BURST_ID:
		/* body: [filler, config[8..11]]. The filler byte occupies the channel
		 * slot every message has and is not part of the configuration, so the
		 * buffer ant_adv_burst_config_set() documents starts at body[1]:
		 * [enable, rf payload size, required modes, 0, 0, optional modes, 0, 0]
		 * with an optional [stall lsb, stall msb, retry extension].
		 *
		 * Without this, MESG_ADV_BURST_DATA_ID is accepted but advanced burst
		 * is never on, so nothing ever leaves at more than 8 bytes a packet.
		 */
		if (len >= 9 && len <= 12) {
			err = ant_adv_burst_config_set((uint8_t *)&body[1],
						       (uint8_t)(len - 1));
		}
		break;

	case MESG_SDU_CONFIG_ID:
		/* body: [ch, mask_config]. Binds one of the masks below to a channel;
		 * INVALID_SDU_MASK (0xFF) turns selective updates back off.
		 */
		if (len >= 2) {
			err = ant_sdu_mask_config(body[0], body[1]);
		}
		break;

	case MESG_SDU_SET_MASK_ID:
		/* body: [mask_index, mask[8]]. body[0] is a mask number, not a channel
		 * - masks are defined here and attached to channels by MESG_SDU_CONFIG.
		 */
		if (len >= 9) {
			err = ant_sdu_mask_set(body[0], (uint8_t *)&body[1]);
		}
		break;

#ifdef CONFIG_ANT_DONGLE_ENCRYPTION
	/* The three writes that can put a channel into AES-CTR mode. Compiled out
	 * by default: no host on this platform can send them, and a mode nothing
	 * asks for should not be reachable on the path Zwift runs. See the Kconfig.
	 * The matching reads live in handle_request() and are always present.
	 */
	case MESG_ENCRYPT_ENABLE_ID:
		/* body: [ch, mode, key_num, decimation_rate] */
		if (len >= 4) {
			err = ant_crypto_channel_enable(body[0], body[1], body[2],
							body[3]);
		}
		break;

	case MESG_SET_ENCRYPT_KEY_ID:
		/* body: [key_num, key[16]]. sdk-ant names no constant for the key
		 * length; that it is 128 bits is stated only in the prose of
		 * ant_crypto_key_set(), which takes a bare pointer.
		 */
		if (len >= 1 + 16) {
			err = ant_crypto_key_set(body[0], (uint8_t *)&body[1]);
		}
		break;

	case MESG_SET_ENCRYPT_INFO_ID:
		/* body: [info_type, data]. How much data depends on the type, and the
		 * length has to be checked against it here: ant_crypto_info_set() reads
		 * a fixed count per type with no size to bound it, so a truncated
		 * command would otherwise hand the stack whatever follows in the
		 * parser's frame buffer.
		 */
		if (len >= 2) {
			uint8_t want = 0;

			switch (body[0]) {
			case ENCRYPTION_INFO_SET_CRYPTO_ID:
				want = MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE;
				break;
			case ENCRYPTION_INFO_SET_CUSTOM_USER_DATA:
				want = MESG_CONFIG_ENCRYPT_REQ_CONFIG_USER_DATA_SIZE;
				break;
			/* ENCRYPTION_INFO_SET_RNG_SEED is deliberately absent. sdk-ant
			 * documents it as "platform specific" and defines no size for
			 * it anywhere, and the stack does not take its randomness from
			 * the host regardless - ant_stack_funcs_register() hands it an
			 * fpRANDGet callback at init. Refusing beats guessing a length.
			 */
			default:
				break;
			}

			if (want && len >= want) {
				err = ant_crypto_info_set(body[0],
							  (uint8_t *)&body[1]);
			}
		}
		break;
#endif /* CONFIG_ANT_DONGLE_ENCRYPTION */

	case MESG_ECS_ENABLE_ID:
		/* body: [filler, enable] */
		if (len >= 2) {
			err = ant_enhanced_channel_spacing_enable(body[1]);
		}
		break;

	case MESG_PENDING_TRANSMIT_CLEAR_ID:
		/* body: [ch] */
		if (len >= 1) {
			uint8_t cleared;

			err = ant_pending_transmit_clear(body[0], &cleared);
		}
		break;

	case MESG_REQUEST_ID:
		handle_request(body, len);
		return; /* handle_request sends its own response */

	case MESG_BROADCAST_DATA_ID:
		/* body: [ch, data[8]] */
		if (len >= 9) {
			err = ant_broadcast_message_tx(body[0], 8,
						       (uint8_t *)&body[1]);
		}
		break;

	case MESG_ACKNOWLEDGED_DATA_ID:
		/* body: [ch, data[8]] */
		if (len >= 9) {
			err = ant_acknowledge_message_tx(body[0], 8,
							 (uint8_t *)&body[1]);
		}
		break;

	case MESG_BURST_DATA_ID:
	case MESG_ADV_BURST_DATA_ID:
		handle_burst(msg_id, body, len);
		return; /* handle_burst answers only on failure */

	default:
		LOG_WRN("Unknown msg_id 0x%02X len=%d", msg_id, len);
		send_response(ch, msg_id, INVALID_MESSAGE);
		return;
	}

	send_response(ch, msg_id,
		      err ? (uint8_t)err : RESPONSE_NO_ERROR);
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
	static uint8_t  msg_body[MESG_MAX_SIZE_VALUE]; /* channel + payload */
	static uint8_t  body_idx;
	static uint8_t  running_xor;

	switch (state) {
	case S_SYNC:
		if (b == MESG_TX_SYNC) {
			/* Seed with SYNC, not 0: it is part of the checksum. */
			running_xor = b;
			state = S_LEN;
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
		} else {
			LOG_WRN("Checksum err: got 0x%02X exp 0x%02X", b,
				running_xor);
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

/* ── ANT event handler (outbound path, chip → host) ─────────────────────────── */

void ant_evt_handler(ant_evt_t *p_ant_evt)
{
	/*
	 * p_ant_evt->message layout:
	 *   ANT_MESSAGE_ucSize       = len of (channel byte + payload)
	 *   ANT_MESSAGE_ucMesgID     = message ID
	 *   ANT_MESSAGE_aucMesgData  = [channel, payload...]
	 *
	 * Wire format: [0xA4][LEN][ID][channel+payload...][XOR checksum]
	 * Total size  = LEN + 4 bytes.
	 */
	uint8_t msg_len = p_ant_evt->message.ANT_MESSAGE_ucSize;
	uint8_t msg_id  = p_ant_evt->message.ANT_MESSAGE_ucMesgID;
	const uint8_t *msg_data = p_ant_evt->message.ANT_MESSAGE_aucMesgData;

	if (atomic_get(&reset_in_progress)) {
		return;
	}

	/* A channel event arrives as [channel, MESG_EVENT_ID, code]. Three of
	 * them mean the burst handler has finished with the block it was given,
	 * so the next packet from the host may overwrite it.
	 */
	if (msg_id == MESG_RESPONSE_EVENT_ID && msg_len >= 3 &&
	    msg_data[1] == MESG_EVENT_ID) {
		switch (msg_data[2]) {
		case EVENT_TRANSFER_NEXT_DATA_BLOCK:
			k_sem_give(&burst_block_free);
			/* Internal flow control for the buffer handover above.
			 * A real stick frames bursts itself and never puts this
			 * on the wire, so neither do we.
			 */
			return;
		case EVENT_TRANSFER_TX_COMPLETED:
		case EVENT_TRANSFER_TX_FAILED:
			k_sem_give(&burst_block_free);
			break;
		default:
			break;
		}
	}

	/* Guard against oversized messages */
	if (msg_len > MESG_MAX_SIZE_VALUE) {
		LOG_WRN("Oversized ANT event: id=0x%02X len=%d", msg_id, msg_len);
		return;
	}

	/* Sized like send_message(), not MESG_MAX_SIZE. MESG_MAX_SIZE counts a
	 * frame around MESG_MAX_DATA_SIZE, but the size byte is allowed to reach
	 * MESG_MAX_SIZE_VALUE, which is one larger - it also counts the channel
	 * byte. A full extended-data message therefore writes the checksum one
	 * past the end and hands usb_ant_send_async() a length one over the
	 * buffer, from the ANT work-queue thread.
	 */
	uint8_t buf[MESG_MAX_SIZE_VALUE + 4];
	uint8_t xor = MESG_TX_SYNC;

	buf[0] = MESG_TX_SYNC;
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
	ant_err_t err = ant_cb_register(ant_evt_handler);

	if (err) {
		return err;
	}

	k_thread_create(&bridge_thread_data, bridge_stack,
			K_THREAD_STACK_SIZEOF(bridge_stack),
			bridge_thread_fn, NULL, NULL, NULL,
			BRIDGE_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&bridge_thread_data, "ant_bridge");

	return 0;
}
