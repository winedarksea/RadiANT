/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Receive an ANT+ heart-rate broadcast and print it.
 *
 * Opens one receive channel that accepts any heart-rate sensor. The first one
 * heard is acquired, and its device number and heart rate are printed about
 * once a second.
 */

#include <zephyr/kernel.h>

#include <radiant/ant_radio.h>

#define HR_CHANNEL     0u
#define HR_NETWORK     0u
#define HR_DEVICE_TYPE 0x78u
#define HR_PERIOD      8070u /* 32768 / 8070 = 4.06 messages a second */
#define HR_RF_FREQ     57u   /* 2457 MHz */

/* The ANT+ network key. Without it a channel hears nothing. */
static const uint8_t ant_plus_key[8] = {
	0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45
};

/* Extended data follows the channel byte and eight payload bytes. */
#define EXT_FLAGS_OFFSET 9u
#define EXT_FLAG_DEVICE  0x80u

/*
 * Called by RadiANT for every message. Implement exactly one of these in your
 * application. It runs on RadiANT's event thread and must not block.
 */
void antr_on_message(const struct antr_msg *msg)
{
	static uint8_t count;
	uint16_t device = 0u;

	if ((msg->id != ANTW_MESG_BROADCAST_DATA_ID &&
	     msg->id != ANTW_MESG_EXT_BROADCAST_DATA_ID) ||
	    msg->len < EXT_FLAGS_OFFSET || msg->data[0] != HR_CHANNEL) {
		return;
	}

	if (msg->len >= EXT_FLAGS_OFFSET + 3u &&
	    (msg->data[EXT_FLAGS_OFFSET] & EXT_FLAG_DEVICE) != 0u) {
		device = msg->data[EXT_FLAGS_OFFSET + 1u] |
			 ((uint16_t)msg->data[EXT_FLAGS_OFFSET + 2u] << 8);
	}

	if ((count++ % 4u) == 0u) {
		/* Payload byte 7 is the computed heart rate on every HR page.
		 * 0 means the sensor has no reading yet. */
		printk("device %u: %u bpm\n", device, msg->data[1u + 7u]);
	}
}

#define TRY(call)                                                        \
	do {                                                             \
		antr_err_t rc = (call);                                  \
		if (rc != ANTW_RESPONSE_NO_ERROR) {                      \
			printk("%s failed: 0x%02x\n", #call, rc);        \
			return 0;                                        \
		}                                                        \
	} while (0)

int main(void)
{
	TRY(antr_init());
	TRY(antr_network_address_set(HR_NETWORK, ant_plus_key));

	TRY(antr_channel_assign(HR_CHANNEL, ANTW_CHANNEL_TYPE_SLAVE, HR_NETWORK, 0u));
	/* Device number 0 and transmission type 0 mean "any". */
	TRY(antr_channel_id_set(HR_CHANNEL, 0u, HR_DEVICE_TYPE, 0u));
	TRY(antr_channel_period_set(HR_CHANNEL, HR_PERIOD));
	TRY(antr_channel_radio_freq_set(HR_CHANNEL, HR_RF_FREQ));
	/* 0xFF searches until a sensor is found. */
	TRY(antr_channel_search_timeout_set(HR_CHANNEL, 0xFFu));
	/* Ask for the sender's device number on every message. */
	TRY(antr_lib_config_set(ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID));
	TRY(antr_channel_open(HR_CHANNEL));

	printk("Searching for a heart-rate sensor...\n");
	return 0;
}
