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
 *   [SYNC=0xA4] [LEN] [ID] [payload: LEN bytes] [XOR checksum of LEN..last]
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

#include "ant_interface.h"
#include "ant_parameters.h"
#include "ant_host_init.h"

LOG_MODULE_REGISTER(ant_bridge, LOG_LEVEL_INF);

/* ── External symbols from usb_ant_class.c ─────────────────────────────────── */

extern struct ring_buf ant_rx_ring_buf;
extern struct k_sem    ant_rx_sem;
int usb_ant_send(const uint8_t *buf, size_t len);
int usb_ant_send_async(const uint8_t *buf, size_t len);
void usb_ant_resume_rx(void);

/* ── Bridge thread ─────────────────────────────────────────────────────────── */

#define BRIDGE_STACK_SIZE  2048
#define BRIDGE_PRIORITY    5

K_THREAD_STACK_DEFINE(bridge_stack, BRIDGE_STACK_SIZE);
static struct k_thread bridge_thread_data;

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
	buf[6] = buf[1] ^ buf[2] ^ buf[3] ^ buf[4] ^ buf[5];
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
	buf[4] = buf[1] ^ buf[2] ^ buf[3];
	usb_ant_send(buf, sizeof(buf));
}

static void send_message(uint8_t msg_id, const uint8_t *payload, uint8_t len)
{
	uint8_t buf[MESG_MAX_SIZE_VALUE + 4];
	uint8_t xor = len ^ msg_id;

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
		err = ant_sdu_mask_get(ch, payload);
		if (!err) {
			send_message(MESG_SDU_SET_MASK_ID, payload,
				     MESG_ANT_MAX_PAYLOAD_SIZE +
				     MESG_CHANNEL_NUM_SIZE);
		}
		break;
	}

	case MESG_ENCRYPT_ENABLE_ID: {
		err = ant_crypto_info_get(ch, payload);
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

/* ── Inbound dispatcher ─────────────────────────────────────────────────────── */

static void dispatch(uint8_t msg_id, const uint8_t *body, uint8_t len)
{
	ant_err_t err = 0;
	/* body[0] is channel number (or network number for NETWORK_KEY) */
	uint8_t ch = (len > 0) ? body[0] : 0;

	switch (msg_id) {

	case MESG_SYSTEM_RESET_ID:
		send_startup();
		k_sleep(K_MSEC(20));
		sys_reboot(SYS_REBOOT_WARM);
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
			running_xor = 0;
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

	/* Guard against oversized messages */
	if (msg_len > MESG_MAX_SIZE_VALUE) {
		LOG_WRN("Oversized ANT event: id=0x%02X len=%d", msg_id, msg_len);
		return;
	}

	uint8_t buf[MESG_MAX_SIZE];
	uint8_t xor = 0;

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
