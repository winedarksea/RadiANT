/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Broadcast a simulated ANT+ heart rate of 120 bpm.
 *
 * Any ANT+ compatible receiver will see this as a heart-rate strap, including
 * a second board running the hr_receiver sample.
 */

#include <zephyr/kernel.h>

#include <radiant/ant_radio.h>

#define HR_CHANNEL     0u
#define HR_NETWORK     0u
#define HR_DEVICE_TYPE 0x78u
#define HR_PERIOD      8070u /* 32768 / 8070 = 4.06 messages a second */
#define HR_RF_FREQ     57u   /* 2457 MHz */

/* Pick your own, from 1 to 65535. Two sensors should not share a number. */
#define DEVICE_NUMBER  12345u
#define TX_TYPE        5u

#define SIM_BPM        120u
#define SIM_BEAT_MS    (60000u / SIM_BPM)

/* The ANT+ network key. Without it nothing can hear this channel. */
static const uint8_t ant_plus_key[8] = {
	0xB9, 0xA5, 0x21, 0xFB, 0xBD, 0x72, 0xC3, 0x45
};

static void send_next_page(void)
{
	uint32_t beats = k_uptime_get_32() / SIM_BEAT_MS;
	/* The time of the last beat, in 1/1024 s. */
	uint16_t event_time = (uint16_t)(beats * SIM_BEAT_MS * 1024u / 1000u);
	/* HR data page 0. */
	uint8_t page[8] = {
		0x00u, 0xFFu, 0xFFu, 0xFFu,
		(uint8_t)event_time, (uint8_t)(event_time >> 8),
		(uint8_t)beats, SIM_BPM,
	};

	(void)antr_broadcast_message_tx(HR_CHANNEL, sizeof(page), page);
}

/*
 * Called by RadiANT for every message. EVENT_TX means a page has just gone out,
 * which is the moment to supply the next one.
 */
void antr_on_message(const struct antr_msg *msg)
{
	if (msg->id == ANTW_MESG_RESPONSE_EVENT_ID &&
	    msg->len >= ANTW_MESG_RESPONSE_EVENT_SIZE &&
	    msg->data[0] == HR_CHANNEL && msg->data[2] == ANTW_EVENT_TX) {
		send_next_page();
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

	TRY(antr_channel_assign(HR_CHANNEL, ANTW_CHANNEL_TYPE_MASTER, HR_NETWORK, 0u));
	TRY(antr_channel_id_set(HR_CHANNEL, DEVICE_NUMBER, HR_DEVICE_TYPE, TX_TYPE));
	TRY(antr_channel_period_set(HR_CHANNEL, HR_PERIOD));
	TRY(antr_channel_radio_freq_set(HR_CHANNEL, HR_RF_FREQ));
	/* The first message goes out empty; EVENT_TX after it starts the data. */
	TRY(antr_channel_open(HR_CHANNEL));

	printk("Transmitting as device %u\n", DEVICE_NUMBER);
	return 0;
}
