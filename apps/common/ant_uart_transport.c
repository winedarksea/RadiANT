/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ANT serial transport over a plain UART.
 *
 * For parts with no USB device peripheral - the nRF54L05/L10/L15 have no USB
 * controller in silicon, so the nRF54L15 DK cannot enumerate as a dongle
 * regardless of software. Worth building for anyway: it's where the ANT
 * stack, MPSL and clock setup get exercised end to end on nRF54L hardware.
 *
 * Less of a compromise than it sounds: ANT's serial protocol is a UART
 * protocol, and a USB ANT stick is a UART-to-USB bridge with the UART hidden
 * inside, so the 0xA4-framed messages here are byte-for-byte what the USB
 * build puts on its bulk endpoints.
 *
 * One real difference: USB bulk endpoints NAK, giving genuine backpressure;
 * a UART without RTS/CTS cannot, so bytes that arrive with no room are
 * dropped and counted rather than corrupting a frame boundary - the checksum
 * on the following frame is what tells the host. Enable hardware flow
 * control in the board devicetree if the pins are wired.
 */

#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

#include "ant_transport.h"

LOG_MODULE_REGISTER(ant_uart, LOG_LEVEL_INF);

/* The board overlay names which UART carries ANT. Must not be the console -
 * the two would interleave into one unparseable stream. On the nRF54L15 DK
 * the console is uart20 (VCOM0) and this is uart30 (VCOM1), both arriving
 * over the same debugger cable as separate COM ports.
 */
#define ANT_UART_NODE DT_ALIAS(ant_uart)

BUILD_ASSERT(DT_NODE_HAS_STATUS(ANT_UART_NODE, okay),
	     "No ant-uart alias. The UART transport needs a board overlay "
	     "naming a UART that is not the console.");

/* Two records of one number; this assert is what stops them disagreeing. The
 * devicetree's current-speed CONFIGURES the UART; CONFIG_ANT_DONGLE_UART_BAUD
 * RECORDS the rate where a script can read it. A mismatch is invisible on
 * the wire - framing errors that look exactly like a hung image or wrong COM
 * port - which is the failure this catches.
 */
BUILD_ASSERT(DT_PROP(ANT_UART_NODE, current_speed) == CONFIG_ANT_DONGLE_UART_BAUD,
	     "The ant-uart node's current-speed and CONFIG_ANT_DONGLE_UART_BAUD "
	     "disagree. They describe the same UART; fix the board overlay or "
	     "the Kconfig default so both name the rate the host will use.");

static const struct device *const ant_uart_dev = DEVICE_DT_GET(ANT_UART_NODE);

#define ANT_TX_FRAME_MAX 64
#define ANT_TX_QUEUE_DEPTH 32
#define ANT_TX_STACK_SIZE 1024
#define ANT_TX_PRIORITY 5

#define ANT_RX_BUF_BYTES 512
#define ANT_UART_TX_BUF_BYTES 512

static uint8_t rx_buf_backing[ANT_RX_BUF_BYTES];
struct ring_buf ant_rx_ring_buf;
K_SEM_DEFINE(ant_rx_sem, 0, 32);

static uint8_t tx_buf_backing[ANT_UART_TX_BUF_BYTES];
static struct ring_buf ant_uart_tx_ring;

struct ant_tx_frame {
	uint16_t len;
	uint8_t data[ANT_TX_FRAME_MAX];
};

K_MSGQ_DEFINE(ant_tx_msgq, sizeof(struct ant_tx_frame), ANT_TX_QUEUE_DEPTH, 4);
K_THREAD_STACK_DEFINE(ant_tx_stack, ANT_TX_STACK_SIZE);
static struct k_thread ant_tx_thread_data;
static bool tx_thread_started;

static K_MUTEX_DEFINE(tx_mutex);
static K_SEM_DEFINE(tx_space_sem, 0, 1);
static volatile bool transport_up;
static volatile uint32_t rx_dropped;

/* ── UART interrupt handler ────────────────────────────────────────────────── */

static void ant_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t chunk[32];
			int n = uart_fifo_read(dev, chunk, sizeof(chunk));

			if (n > 0) {
				uint32_t put = ring_buf_put(&ant_rx_ring_buf,
							    chunk, (uint32_t)n);

				if (put < (uint32_t)n) {
					rx_dropped += (uint32_t)n - put;
				}

				if (put > 0) {
					k_sem_give(&ant_rx_sem);
				}
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *data;
			uint32_t claim = ring_buf_get_claim(&ant_uart_tx_ring,
							    &data, 64);

			if (claim == 0) {
				ring_buf_get_finish(&ant_uart_tx_ring, 0);
				uart_irq_tx_disable(dev);
			} else {
				int sent = uart_fifo_fill(dev, data, (int)claim);

				ring_buf_get_finish(&ant_uart_tx_ring,
						    sent > 0 ? (uint32_t)sent : 0);
				k_sem_give(&tx_space_sem);
			}
		}
	}
}

/* ── Transmit ──────────────────────────────────────────────────────────────── */

static int ant_uart_write(const uint8_t *buf, size_t len)
{
	size_t off = 0;

	if (!transport_up) {
		return -ENODEV;
	}

	k_mutex_lock(&tx_mutex, K_FOREVER);

	while (off < len) {
		uint32_t put = ring_buf_put(&ant_uart_tx_ring, buf + off,
					    (uint32_t)(len - off));

		off += put;

		if (put > 0) {
			uart_irq_tx_enable(ant_uart_dev);
		}

		if (off < len) {
			/* Ring buffer full - wait for the ISR to drain some.
			 * Bounded, so a wedged UART cannot park the bridge
			 * thread indefinitely.
			 */
			k_sem_reset(&tx_space_sem);
			uart_irq_tx_enable(ant_uart_dev);

			if (k_sem_take(&tx_space_sem, K_MSEC(1000)) != 0) {
				k_mutex_unlock(&tx_mutex);
				LOG_WRN("UART TX stalled, dropped %zu bytes",
					len - off);
				return -ETIMEDOUT;
			}
		}
	}

	k_mutex_unlock(&tx_mutex);

	return 0;
}

static void ant_tx_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct ant_tx_frame frame;

	while (1) {
		k_msgq_get(&ant_tx_msgq, &frame, K_FOREVER);

		if (!transport_up) {
			continue;
		}

		int ret = ant_uart_write(frame.data, frame.len);

		if (ret) {
			LOG_WRN("UART TX failed: %d", ret);
		}
	}
}

/* ── Public API ────────────────────────────────────────────────────────────── */

void usb_ant_class_init(void)
{
	ring_buf_init(&ant_rx_ring_buf, ANT_RX_BUF_BYTES, rx_buf_backing);
	ring_buf_init(&ant_uart_tx_ring, ANT_UART_TX_BUF_BYTES, tx_buf_backing);

	if (!tx_thread_started) {
		k_thread_create(&ant_tx_thread_data, ant_tx_stack,
				K_THREAD_STACK_SIZEOF(ant_tx_stack),
				ant_tx_thread_fn, NULL, NULL, NULL,
				ANT_TX_PRIORITY, 0, K_NO_WAIT);
		k_thread_name_set(&ant_tx_thread_data, "ant_uart_tx");
		tx_thread_started = true;
	}
}

int ant_transport_enable(void)
{
	int ret;

	if (!device_is_ready(ant_uart_dev)) {
		LOG_ERR("ANT UART not ready");
		return -ENODEV;
	}

	/* Refuse to open a UART whose buffers were never allocated.
	 *
	 * This is not defensive padding - it is the check for a failure that
	 * has actually happened here, and that cost a bench session precisely
	 * because nothing detected it. Both ring buffers are file-scope, so
	 * they start as zeroed BSS: skipping usb_ant_class_init() leaves them
	 * as perfectly well-formed ring buffers of size ZERO rather than as
	 * anything that would fault. ring_buf_put() then accepts nothing, so
	 * received bytes are dropped and transmitted bytes time out - and the
	 * transport reports itself up, because from in here it IS up.
	 *
	 * A silent dead UART is the single most expensive symptom this
	 * codebase can produce, because it is what a wrong pin map, a wrong
	 * baud, wrong wiring, a crashed thread and a missing init call all
	 * look like from the host. Naming this one here removes it from that
	 * list for good.
	 */
	if (ant_rx_ring_buf.size == 0 || ant_uart_tx_ring.size == 0) {
		LOG_ERR("ANT UART buffers not allocated - usb_ant_class_init() "
			"was not called before ant_transport_enable(). The port "
			"would open and then silently drop every byte in both "
			"directions.");
		return -EINVAL;
	}

	ret = uart_irq_callback_user_data_set(ant_uart_dev, ant_uart_isr, NULL);
	if (ret) {
		LOG_ERR("Failed to set UART callback: %d", ret);
		return ret;
	}

	uart_irq_rx_enable(ant_uart_dev);
	transport_up = true;

	LOG_INF("ANT serial on %s", ant_uart_dev->name);

	return 0;
}

int usb_ant_send(const uint8_t *buf, size_t len)
{
	if (len > ANT_TX_FRAME_MAX) {
		return -EMSGSIZE;
	}

	return ant_uart_write(buf, len);
}

int usb_ant_send_async(const uint8_t *buf, size_t len)
{
	struct ant_tx_frame frame;

	if (!transport_up) {
		return -ENODEV;
	}

	if (len > sizeof(frame.data)) {
		return -EMSGSIZE;
	}

	frame.len = (uint16_t)len;
	memcpy(frame.data, buf, len);

	return k_msgq_put(&ant_tx_msgq, &frame, K_NO_WAIT);
}

void usb_ant_resume_rx(void)
{
	/* Nothing to re-arm: RX interrupts stay enabled for the life of the
	 * transport, because disabling them would not stop the host sending -
	 * it would only move where the bytes are lost. See the file header.
	 */
	if (rx_dropped != 0) {
		LOG_WRN("Dropped %u RX bytes (ring buffer full)", rx_dropped);
		rx_dropped = 0;
	}
}
