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
#include "ant_health.h"

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

/* ── The CC13xx/CC26xx UART: hardware FIFO on ───────────────────────────────
 *
 * Zephyr's uart_cc13xx_cc26xx driver turns the UART's 32-byte FIFOs OFF
 * ("character mode": a 1-byte holding register each way). At 57600 baud a
 * byte lands every 174 us, so with the FIFO off any span of more than about
 * one byte time in which this ISR cannot run overruns the receiver and
 * silently loses a host byte. Under the C2 load (9 tracked channels and
 * Zwift's 4.5 s scan reopen) that happened every few tens of seconds
 * (archive/benchmarks/2026-11-16-ti-cmd-stall/): the UART's overrun flag
 * counted one overrun per lost frame, 0xF6 host_csum and host_desync moved
 * with it, and the host saw commands that were never answered.
 *
 * One lost byte costs more than one command. Lose a LEN byte and the message
 * id is read as the length; lose a short message's last byte and the NEXT
 * frame's SYNC is taken as its checksum. Either way the parser swallows the
 * host's following command too - on the bench, the scan's UNASSIGN and then
 * its ASSIGN, which the host resent and had refused with code 21 because the
 * UNASSIGN never ran. process_byte()'s S_LEN bound in ant_serial_bridge.c
 * limits that second-order damage; this removes the first-order loss.
 *
 * With the FIFO on, the receive interrupt fires at 8 bytes (or on the receive
 * timeout, 32 bit periods after the last byte, for a shorter frame), and the
 * ISR can be held off for 24 byte times - about 4 ms - before a byte is lost.
 *
 * Why this ISR reads the hardware rather than the driver's helpers: the
 * driver's irq_is_pending() looks at the TX and RX interrupts only. A frame
 * shorter than the RX trigger level raises only the receive TIMEOUT
 * interrupt; a loop gated on is_pending() would never read it, never clear
 * it, and re-enter forever. Draining the FIFO until it is empty clears both.
 *
 * TX needs no workaround, but is tested directly too so the ISR does not
 * depend on a helper whose meaning has changed between releases:
 * irq_tx_ready() is the masked TX interrupt status in NCS v3.4.0's driver
 * and was UARTSpaceAvail() in v3.2.4 (!UARTBusy() is irq_tx_complete()).
 * "TX interrupt unmasked and space in the FIFO" is right under either.
 *
 * The driver's configure() disables the FIFO again, and its PM resume path
 * calls configure(); the BUILD_ASSERT below keeps PM out of these images
 * rather than re-enabling the FIFO from a PM hook.
 */
#if DT_NODE_HAS_COMPAT(ANT_UART_NODE, ti_cc13xx_cc26xx_uart)
#include <driverlib/uart.h>
#define ANT_UART_TI_FIFO 1
#define ANT_UART_BASE ((uint32_t)DT_REG_ADDR(ANT_UART_NODE))
/* configure(), re-run by the driver on every PM wake from standby, turns the
 * FIFO back off - and nothing here would notice, so the 1-byte receiver and
 * its silent overruns would quietly return. */
BUILD_ASSERT(!IS_ENABLED(CONFIG_PM) && !IS_ENABLED(CONFIG_PM_DEVICE),
	     "The ANT UART's hardware FIFO is enabled once, at transport "
	     "enable; the cc13xx_cc26xx driver disables it again on PM "
	     "resume. Re-enable it from a PM hook before allowing PM.");
#endif

/* ── UART interrupt handler ────────────────────────────────────────────────── */

/* 0xF6 [32..33]: receiver overruns, read through the driver's err_check(),
 * which on both UARTs this transport serves is one status-register read and
 * clear (cc13xx_cc26xx: RSR, cleared through ECR; nRF UARTE: ERRORSRC).
 * Called at the top of every ISR pass, so an overrun is counted on the pass
 * after the byte it cost. Off with the health counters, so the shipping
 * image does not change. */
#if defined(CONFIG_ANT_DONGLE_HEALTH_COUNTERS)
static bool rx_overrun_measured;

static void note_rx_errors(const struct device *dev)
{
	if (rx_overrun_measured) {
		int err = uart_err_check(dev);

		if (err > 0 && (err & UART_ERROR_OVERRUN) != 0) {
			ant_health_note(ANT_HEALTH_UART_RX_OVERRUN);
		}
	}
}

/* At enable: a driver without err_check() answers -ENOSYS, and its field
 * stays "not measured" rather than reading as zero overruns. The probe also
 * clears anything latched before the transport was up. */
static void rx_overrun_probe(const struct device *dev)
{
	if (uart_err_check(dev) != -ENOSYS) {
		rx_overrun_measured = true;
		ant_health_measured(ANT_HEALTH_UART_RX_OVERRUN);
	}
}
#else
#define note_rx_errors(dev)   ((void)0)
#define rx_overrun_probe(dev) ((void)0)
#endif

/* One FIFO read into the RX ring. Returns the number of bytes read. */
static int rx_service(const struct device *dev)
{
	uint8_t chunk[32];
	int n = uart_fifo_read(dev, chunk, sizeof(chunk));

	if (n > 0) {
		uint32_t put = ring_buf_put(&ant_rx_ring_buf, chunk, (uint32_t)n);

		if (put < (uint32_t)n) {
			rx_dropped += (uint32_t)n - put;
		}

		if (put > 0) {
			k_sem_give(&ant_rx_sem);
		}
	}

	return n;
}

/* Refill the TX FIFO from the ring, or turn the TX interrupt off once the ring
 * is empty. */
static void tx_service(const struct device *dev)
{
	uint8_t *data;
	uint32_t claim = ring_buf_get_claim(&ant_uart_tx_ring, &data, 64);

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

#if defined(ANT_UART_TI_FIFO)

static void ant_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	for (;;) {
		bool worked = false;

		note_rx_errors(dev);

		/* Until the FIFO is empty: clears both RX and RX timeout. */
		while (UARTCharsAvail(ANT_UART_BASE)) {
			if (rx_service(dev) <= 0) {
				break;
			}
			worked = true;
		}

		/* The TX interrupt is unmasked exactly while the ring may hold
		 * data (ant_uart_write() unmasks after every put; tx_service()
		 * masks it on an empty ring). A fill that reaches the trigger
		 * level clears the TX interrupt; the loop stops at a full
		 * FIFO. */
		if ((HWREG(ANT_UART_BASE + UART_O_IMSC) & UART_INT_TX) &&
		    UARTSpaceAvail(ANT_UART_BASE)) {
			tx_service(dev);
			worked = true;
		}

		if (!worked) {
			break;
		}
	}
}

static void ant_uart_hw_fifo_enable(void)
{
	/* RX at 1/4 full (8 bytes): prompt for a burst, 24 bytes of slack
	 * after it. TX at 1/2: refill with 16 bytes still queued. */
	UARTFIFOLevelSet(ANT_UART_BASE, UART_FIFO_TX4_8, UART_FIFO_RX2_8);
	UARTFIFOEnable(ANT_UART_BASE);
	UARTIntClear(ANT_UART_BASE, UART_INT_RT);
	UARTIntEnable(ANT_UART_BASE, UART_INT_RT);
}

#else /* !ANT_UART_TI_FIFO */

static void ant_uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		note_rx_errors(dev);

		if (uart_irq_rx_ready(dev)) {
			(void)rx_service(dev);
		}

		if (uart_irq_tx_ready(dev)) {
			tx_service(dev);
		}
	}
}

static inline void ant_uart_hw_fifo_enable(void)
{
}

#endif /* ANT_UART_TI_FIFO */

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

	/* Before RX is unmasked; see the CC13xx/CC26xx note above. */
	ant_uart_hw_fifo_enable();
	rx_overrun_probe(ant_uart_dev);
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

	/* Same accounting as both USB transports, so 0xF6 reads alike on all. */
	int rc = k_msgq_put(&ant_tx_msgq, &frame, K_NO_WAIT);

	if (rc != 0) {
		ant_health_note(ANT_HEALTH_TX_DROP);
	} else {
		ant_health_depth(ANT_HEALTH_DEPTH_TX,
				 k_msgq_num_used_get(&ant_tx_msgq));
	}
	return rc;
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

uint32_t ant_transport_session(uint32_t *rx_boundary)
{
	/* Constant: a UART has no bus reset and no SET_CONFIGURATION, so
	 * there is no session boundary for the bridge to see. A host that
	 * reopens the port mid-frame is resynchronised the way it always was,
	 * by the parser hunting for the next SYNC.
	 */
	*rx_boundary = 0;
	return 0;
}
