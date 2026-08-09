/* SPDX-License-Identifier: Apache-2.0 */

/*
 * USB ANT vendor class on the *new* USB device stack (USBD/UDC).
 *
 * Same device on the wire as usb_ant_class.c: VID 0x0FCF / PID 0x1009, one
 * vendor-specific interface, two bulk endpoints at 0x01 (OUT) and 0x81 (IN).
 * A host cannot tell which of the two files produced the descriptors, which is
 * the point - the legacy build is the one verified against Zwift, and this one
 * has to be swappable for it without renegotiating that.
 *
 * Why it exists at all: every nRF54 part that has a USB device controller has
 * a DesignWare DWC2, and Zephyr's only DWC2 driver is drivers/usb/udc/udc_dwc2.c.
 * There is no usb_dc_dwc2.c. So the legacy stack is not "older but available"
 * on those parts, it is absent, and this file is the price of admission.
 *
 * The two stacks differ in ways that matter here:
 *
 *   Transfers.  Legacy had usb_transfer_sync(). USBD is asynchronous only:
 *               allocate a net_buf from the UDC pool, enqueue it, and get a
 *               completion callback. usb_ant_send() rebuilds the synchronous
 *               shape on top with a semaphore, because the bridge is written
 *               against a blocking send.
 *   Buffers.    Buffers come from a shared UDC pool (CONFIG_UDC_BUF_COUNT,
 *               CONFIG_UDC_BUF_POOL_SIZE) rather than from static per-class
 *               arrays, so exhausting it is a new failure mode. Both are set
 *               in prj.conf.
 *   State.      Configured/deconfigured arrives through the class .enable and
 *               .disable callbacks; suspend/resume arrives through the device
 *               message callback. Legacy funnelled all of it through one
 *               status callback.
 *
 * The suspend/resume handling below is deliberately a transcription of the
 * legacy file's, including the reason it is shaped that way - a host does not
 * repeat SET_CONFIGURATION after a bus resume, so nothing re-arms the OUT
 * endpoint unless resume does it explicitly. That bug cost a physical replug
 * after every laptop sleep and it is not worth rediscovering on a new stack.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/usb/usbd.h>
#include <zephyr/usb/usb_ch9.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/logging/log.h>

#include "ant_transport.h"

LOG_MODULE_REGISTER(usb_ant, LOG_LEVEL_INF);

#define ANT_EP_MPS_FS 64
/* High speed bulk endpoints must use 512. Only reachable on an HS controller
 * (nRF54LM20A and nRF54H20); the nRF52840 is full speed and never sees this.
 * The largest ANT frame is 42 bytes either way - the packet size is the bus's
 * business, not the protocol's.
 */
#define ANT_EP_MPS_HS 512

/* See the legacy file: this bounds a struct on the caller's stack, and one of
 * those callers runs on sdk-ant's 1 KB work queue.
 */
#define ANT_TX_FRAME_MAX 64
#define ANT_TX_QUEUE_DEPTH 32
#define ANT_TX_STACK_SIZE 2048
#define ANT_TX_PRIORITY 5

/* How long usb_ant_send() waits for a bulk IN to complete before giving up.
 * A host that has stopped polling is indistinguishable from one that is slow,
 * so this must be finite or the bridge thread parks forever.
 */
#define ANT_TX_TIMEOUT K_MSEC(1000)

#define ANT_RX_BUF_BYTES 512
static uint8_t rx_buf_backing[ANT_RX_BUF_BYTES];
struct ring_buf ant_rx_ring_buf;
K_SEM_DEFINE(ant_rx_sem, 0, 32);

struct ant_tx_frame {
	uint16_t len;
	uint8_t data[ANT_TX_FRAME_MAX];
};

K_MSGQ_DEFINE(ant_tx_msgq, sizeof(struct ant_tx_frame), ANT_TX_QUEUE_DEPTH, 4);
K_THREAD_STACK_DEFINE(ant_tx_stack, ANT_TX_STACK_SIZE);
static struct k_thread ant_tx_thread_data;
static bool tx_thread_started;

static volatile bool configured;
static volatile bool was_configured;
static volatile bool rx_armed;
static volatile bool rx_backpressured;

static K_MUTEX_DEFINE(tx_mutex);
static K_SEM_DEFINE(tx_done_sem, 0, 1);
static volatile int tx_result;

/* ── Descriptors ───────────────────────────────────────────────────────────── */

struct ant_desc {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor if0_out_ep;
	struct usb_ep_descriptor if0_in_ep;
	struct usb_ep_descriptor if0_hs_out_ep;
	struct usb_ep_descriptor if0_hs_in_ep;
	struct usb_desc_header nil_desc;
};

static struct ant_desc ant_desc = {
	.if0 = {
		.bLength = sizeof(struct usb_if_descriptor),
		.bDescriptorType = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,
		.bAlternateSetting = 0,
		.bNumEndpoints = 2,
		.bInterfaceClass = USB_BCC_VENDOR,
		.bInterfaceSubClass = 0,
		.bInterfaceProtocol = 0,
		.iInterface = 0,
	},
	/* Unlike the legacy stack there is no AUTO_EP_* indirection: USBD reads
	 * the address written here and assigns it if the controller can, so the
	 * literal 0x01/0x81 a retail stick uses can just be stated.
	 */
	.if0_out_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x01,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(ANT_EP_MPS_FS),
		.bInterval = 0x00,
	},
	.if0_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(ANT_EP_MPS_FS),
		.bInterval = 0x00,
	},
	.if0_hs_out_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x01,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(ANT_EP_MPS_HS),
		.bInterval = 0x00,
	},
	.if0_hs_in_ep = {
		.bLength = sizeof(struct usb_ep_descriptor),
		.bDescriptorType = USB_DESC_ENDPOINT,
		.bEndpointAddress = 0x81,
		.bmAttributes = USB_EP_TYPE_BULK,
		.wMaxPacketSize = sys_cpu_to_le16(ANT_EP_MPS_HS),
		.bInterval = 0x00,
	},
	.nil_desc = {
		.bLength = 0,
		.bDescriptorType = 0,
	},
};

static const struct usb_desc_header *ant_fs_desc[] = {
	(struct usb_desc_header *)&ant_desc.if0,
	(struct usb_desc_header *)&ant_desc.if0_out_ep,
	(struct usb_desc_header *)&ant_desc.if0_in_ep,
	(struct usb_desc_header *)&ant_desc.nil_desc,
};

static const struct usb_desc_header *ant_hs_desc[] = {
	(struct usb_desc_header *)&ant_desc.if0,
	(struct usb_desc_header *)&ant_desc.if0_hs_out_ep,
	(struct usb_desc_header *)&ant_desc.if0_hs_in_ep,
	(struct usb_desc_header *)&ant_desc.nil_desc,
};

/* ── Endpoint address lookup ───────────────────────────────────────────────── */

static struct usbd_class_data *ant_c_data;

static uint8_t ant_ep_out(struct usbd_class_data *const c_data)
{
	struct usbd_context *uds_ctx = usbd_class_get_ctx(c_data);

	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_bus_speed(uds_ctx) == USBD_SPEED_HS) {
		return ant_desc.if0_hs_out_ep.bEndpointAddress;
	}

	return ant_desc.if0_out_ep.bEndpointAddress;
}

static uint8_t ant_ep_in(struct usbd_class_data *const c_data)
{
	struct usbd_context *uds_ctx = usbd_class_get_ctx(c_data);

	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_bus_speed(uds_ctx) == USBD_SPEED_HS) {
		return ant_desc.if0_hs_in_ep.bEndpointAddress;
	}

	return ant_desc.if0_in_ep.bEndpointAddress;
}

static size_t ant_ep_mps(struct usbd_class_data *const c_data)
{
	struct usbd_context *uds_ctx = usbd_class_get_ctx(c_data);

	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_bus_speed(uds_ctx) == USBD_SPEED_HS) {
		return ANT_EP_MPS_HS;
	}

	return ANT_EP_MPS_FS;
}

/* ── Bulk-OUT receive path ─────────────────────────────────────────────────── */

static void arm_rx(void)
{
	struct net_buf *buf;
	int ret;

	if (!configured || rx_armed || ant_c_data == NULL) {
		return;
	}

	/* Refuse to take data we have nowhere to put. The endpoint then NAKs,
	 * the host retries, and the bridge calls usb_ant_resume_rx() once it
	 * has drained. Dropping instead would corrupt the frame stream.
	 */
	if (ring_buf_space_get(&ant_rx_ring_buf) < ant_ep_mps(ant_c_data)) {
		rx_backpressured = true;
		return;
	}

	buf = usbd_ep_buf_alloc(ant_c_data, ant_ep_out(ant_c_data),
				ant_ep_mps(ant_c_data));
	if (buf == NULL) {
		LOG_WRN("No UDC buffer for RX");
		rx_backpressured = true;
		return;
	}

	ret = usbd_ep_enqueue(ant_c_data, buf);
	if (ret) {
		LOG_WRN("Failed to arm USB RX: %d", ret);
		net_buf_unref(buf);
		return;
	}

	rx_armed = true;
	rx_backpressured = false;
}

/* ── Class callbacks ───────────────────────────────────────────────────────── */

static int ant_request(struct usbd_class_data *const c_data,
		       struct net_buf *const buf, const int err)
{
	struct udc_buf_info *bi = (struct udc_buf_info *)net_buf_user_data(buf);

	if (bi->ep == ant_ep_out(c_data)) {
		rx_armed = false;

		if (err == 0 && buf->len > 0) {
			if (ring_buf_space_get(&ant_rx_ring_buf) < buf->len) {
				rx_backpressured = true;
				LOG_WRN("RX buffer full, delaying re-arm");
			} else {
				ring_buf_put(&ant_rx_ring_buf, buf->data,
					     buf->len);
				k_sem_give(&ant_rx_sem);
			}
		}

		net_buf_unref(buf);

		if (configured) {
			arm_rx();
		}

		return 0;
	}

	if (bi->ep == ant_ep_in(c_data)) {
		net_buf_unref(buf);
		tx_result = err;
		k_sem_give(&tx_done_sem);
		return 0;
	}

	net_buf_unref(buf);

	return 0;
}

static void ant_class_enable(struct usbd_class_data *const c_data)
{
	ant_c_data = c_data;
	configured = true;
	was_configured = true;
	rx_armed = false;
	rx_backpressured = false;
	arm_rx();
	LOG_INF("USB configured");
}

static void ant_class_disable(struct usbd_class_data *const c_data)
{
	ARG_UNUSED(c_data);

	configured = false;
	was_configured = false;
	rx_armed = false;
	rx_backpressured = false;
	k_msgq_purge(&ant_tx_msgq);
	LOG_INF("USB deconfigured");
}

static void ant_update(struct usbd_class_data *const c_data,
		       uint8_t iface, uint8_t alternate)
{
	ARG_UNUSED(c_data);
	LOG_DBG("iface %u alternate %u", iface, alternate);
}

static void *ant_get_desc(struct usbd_class_data *const c_data,
			  const enum usbd_speed speed)
{
	ARG_UNUSED(c_data);

	if (USBD_SUPPORTS_HIGH_SPEED && speed == USBD_SPEED_HS) {
		return ant_hs_desc;
	}

	return ant_fs_desc;
}

static int ant_class_init(struct usbd_class_data *const c_data)
{
	ant_c_data = c_data;

	return 0;
}

static struct usbd_class_api ant_api = {
	.update = ant_update,
	.request = ant_request,
	.get_desc = ant_get_desc,
	.enable = ant_class_enable,
	.disable = ant_class_disable,
	.init = ant_class_init,
};

USBD_DEFINE_CLASS(ant_usb, &ant_api, NULL, NULL);

/* ── Device context ────────────────────────────────────────────────────────── */

/* CONFIG_USB_DEVICE_* only exist when the legacy stack is enabled - they live
 * inside `if USB_DEVICE_STACK` in subsys/usb/device/Kconfig. The identity is
 * therefore held in application symbols, which the legacy build feeds into the
 * stack's own and this one reads directly. One source, two consumers.
 */
USBD_DEVICE_DEFINE(ant_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   CONFIG_ANT_DONGLE_USB_VID, CONFIG_ANT_DONGLE_USB_PID);

USBD_DESC_LANG_DEFINE(ant_lang);
USBD_DESC_MANUFACTURER_DEFINE(ant_mfr, CONFIG_ANT_DONGLE_USB_MANUFACTURER);
USBD_DESC_PRODUCT_DEFINE(ant_product, CONFIG_ANT_DONGLE_USB_PRODUCT);
USBD_DESC_SERIAL_NUMBER_DEFINE(ant_sn);

/* Bus powered, no remote wakeup, 100 mA - the profile of the stick this
 * impersonates. bMaxPower is in 2 mA units.
 */
USBD_CONFIGURATION_DEFINE(ant_fs_config, 0, 50, NULL);
USBD_CONFIGURATION_DEFINE(ant_hs_config, 0, 50, NULL);

static void ant_msg_cb(struct usbd_context *const ctx,
		       const struct usbd_msg *const msg)
{
	ARG_UNUSED(ctx);

	switch (msg->type) {
	case USBD_MSG_RESUME:
		/* SET_CONFIGURATION is not repeated after a bus resume, so
		 * .enable will not fire again and nothing else would re-arm
		 * EP_OUT. Without this the dongle enumerates, binds, and then
		 * answers nothing until it is physically replugged - triggered
		 * by any host sleep/wake.
		 */
		if (was_configured && !configured) {
			configured = true;
			rx_armed = false;
			rx_backpressured = false;
			arm_rx();
			LOG_INF("USB resumed");
		}
		break;
	case USBD_MSG_SUSPEND:
		configured = false;
		rx_armed = false;
		rx_backpressured = false;
		k_msgq_purge(&ant_tx_msgq);
		/* Release anyone blocked in usb_ant_send(): the completion
		 * that would have woken them is not coming while suspended.
		 */
		tx_result = -ESHUTDOWN;
		k_sem_give(&tx_done_sem);
		break;
	case USBD_MSG_RESET:
		configured = false;
		rx_armed = false;
		rx_backpressured = false;
		k_msgq_purge(&ant_tx_msgq);
		break;
	case USBD_MSG_VBUS_REMOVED:
		configured = false;
		was_configured = false;
		rx_armed = false;
		rx_backpressured = false;
		k_msgq_purge(&ant_tx_msgq);
		break;
	default:
		LOG_DBG("USBD message: %s", usbd_msg_type_string(msg->type));
		break;
	}
}

/* ── Transmit ──────────────────────────────────────────────────────────────── */

static int usb_ant_write(const uint8_t *buf, size_t len)
{
	struct net_buf *nbuf;
	int ret;

	if (!configured || ant_c_data == NULL) {
		return -ENODEV;
	}

	k_mutex_lock(&tx_mutex, K_FOREVER);

	nbuf = usbd_ep_buf_alloc(ant_c_data, ant_ep_in(ant_c_data), len);
	if (nbuf == NULL) {
		k_mutex_unlock(&tx_mutex);
		return -ENOMEM;
	}

	net_buf_add_mem(nbuf, buf, len);

	/* Drop any completion left over from a previous timeout or a suspend,
	 * so the wait below cannot be satisfied by a stale give.
	 */
	k_sem_reset(&tx_done_sem);
	tx_result = 0;

	ret = usbd_ep_enqueue(ant_c_data, nbuf);
	if (ret) {
		net_buf_unref(nbuf);
		k_mutex_unlock(&tx_mutex);
		return ret;
	}

	if (k_sem_take(&tx_done_sem, ANT_TX_TIMEOUT) != 0) {
		/* The buffer is still owned by the controller. Dequeuing gives
		 * it back with -ECONNABORTED rather than leaking it out of the
		 * UDC pool, which is shared and small.
		 */
		LOG_WRN("USB TX timeout");
		(void)usbd_ep_dequeue(usbd_class_get_ctx(ant_c_data),
				      ant_ep_in(ant_c_data));
		k_mutex_unlock(&tx_mutex);
		return -ETIMEDOUT;
	}

	ret = tx_result;
	k_mutex_unlock(&tx_mutex);

	return ret;
}

static void ant_tx_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	struct ant_tx_frame frame;

	while (1) {
		k_msgq_get(&ant_tx_msgq, &frame, K_FOREVER);

		if (!configured) {
			continue;
		}

		int ret = usb_ant_write(frame.data, frame.len);

		if (ret) {
			LOG_WRN("USB TX failed: %d", ret);
		}
	}
}

/* ── Public API ────────────────────────────────────────────────────────────── */

void usb_ant_class_init(void)
{
	ring_buf_init(&ant_rx_ring_buf, ANT_RX_BUF_BYTES, rx_buf_backing);
	rx_armed = false;
	rx_backpressured = false;

	if (!tx_thread_started) {
		k_thread_create(&ant_tx_thread_data, ant_tx_stack,
				K_THREAD_STACK_SIZEOF(ant_tx_stack),
				ant_tx_thread_fn, NULL, NULL, NULL,
				ANT_TX_PRIORITY, 0, K_NO_WAIT);
		k_thread_name_set(&ant_tx_thread_data, "ant_usb_tx");
		tx_thread_started = true;
	}
}

int ant_transport_enable(void)
{
	int err;

	err = usbd_add_descriptor(&ant_usbd, &ant_lang);
	if (err) {
		LOG_ERR("Failed to add language descriptor: %d", err);
		return err;
	}

	err = usbd_add_descriptor(&ant_usbd, &ant_mfr);
	if (err) {
		LOG_ERR("Failed to add manufacturer descriptor: %d", err);
		return err;
	}

	err = usbd_add_descriptor(&ant_usbd, &ant_product);
	if (err) {
		LOG_ERR("Failed to add product descriptor: %d", err);
		return err;
	}

	/* Same substitution the legacy stack did: the literal in Kconfig is a
	 * placeholder whose length is the setting, and HWINFO's device ID is
	 * what a host actually reads back.
	 */
	err = usbd_add_descriptor(&ant_usbd, &ant_sn);
	if (err) {
		LOG_ERR("Failed to add serial number descriptor: %d", err);
		return err;
	}

	if (USBD_SUPPORTS_HIGH_SPEED &&
	    usbd_caps_speed(&ant_usbd) == USBD_SPEED_HS) {
		err = usbd_add_configuration(&ant_usbd, USBD_SPEED_HS,
					     &ant_hs_config);
		if (err) {
			LOG_ERR("Failed to add HS configuration: %d", err);
			return err;
		}

		err = usbd_register_class(&ant_usbd, "ant_usb",
					  USBD_SPEED_HS, 1);
		if (err) {
			LOG_ERR("Failed to register HS class: %d", err);
			return err;
		}

		/* A retail stick reports class at the interface, not the
		 * device. USBD_DEVICE_DEFINE defaults to the IAD triple
		 * (0xEF/0x02/0x01), which this device must not claim - it has
		 * one interface and no association.
		 */
		usbd_device_set_code_triple(&ant_usbd, USBD_SPEED_HS, 0, 0, 0);
	}

	err = usbd_add_configuration(&ant_usbd, USBD_SPEED_FS, &ant_fs_config);
	if (err) {
		LOG_ERR("Failed to add FS configuration: %d", err);
		return err;
	}

	err = usbd_register_class(&ant_usbd, "ant_usb", USBD_SPEED_FS, 1);
	if (err) {
		LOG_ERR("Failed to register FS class: %d", err);
		return err;
	}

	usbd_device_set_code_triple(&ant_usbd, USBD_SPEED_FS, 0, 0, 0);

#if CONFIG_ANT_DONGLE_BCD_DEVICE != 0
	/* Windows caches its driver verdict against VID+PID+bcdDevice, so
	 * pinning this keeps the hardware ID stable across SDK bumps.
	 */
	err = usbd_device_set_bcd_device(&ant_usbd, CONFIG_ANT_DONGLE_BCD_DEVICE);
	if (err) {
		LOG_ERR("Failed to set bcdDevice: %d", err);
		return err;
	}
#endif

	err = usbd_msg_register_cb(&ant_usbd, ant_msg_cb);
	if (err) {
		LOG_ERR("Failed to register message callback: %d", err);
		return err;
	}

	err = usbd_init(&ant_usbd);
	if (err) {
		LOG_ERR("usbd_init failed: %d", err);
		return err;
	}

	err = usbd_enable(&ant_usbd);
	if (err) {
		LOG_ERR("usbd_enable failed: %d", err);
		return err;
	}

	return 0;
}

int usb_ant_send(const uint8_t *buf, size_t len)
{
	if (len > ANT_TX_FRAME_MAX) {
		return -EMSGSIZE;
	}

	return usb_ant_write(buf, len);
}

int usb_ant_send_async(const uint8_t *buf, size_t len)
{
	struct ant_tx_frame frame;

	if (!configured) {
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
	if (configured && rx_backpressured) {
		arm_rx();
	}
}
