/* SPDX-License-Identifier: Apache-2.0 */

/*
 * USB ANT vendor class — bulk-only interface enumerating as VID 0x0FCF / PID 0x1009
 * (ANT USB-m Stick).  Zwift and openant find the dongle by scanning for these IDs.
 *
 * Architecture:
 *   EP_OUT (0x01)  host → device: Zephyr USB stack calls ep_out_cb, data goes into
 *                  ant_rx_ring_buf and ant_rx_sem is signalled for the bridge thread.
 *   EP_IN  (0x81)  device → host: usb_ant_send() does a synchronous transfer.
 */

#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/bos.h>
#include <zephyr/usb/msos_desc.h>
#include <usb_descriptor.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>

#include "ant_transport.h"

LOG_MODULE_REGISTER(usb_ant, LOG_LEVEL_INF);

/* Endpoint addresses */
#define ANT_EP_OUT_IDX  0
#define ANT_EP_IN_IDX   1
#define ANT_EP_MPS  64
/* Largest ANT serial frame is 42 bytes (sync+size+id+38 body max+checksum);
 * 64 keeps it to one bulk packet with room to spare. Not just a buffer bound
 * either: usb_ant_send_async() puts a whole struct ant_tx_frame on the
 * stack, and its caller runs on sdk-ant's 1 KB work queue stack, which is
 * not ours to spend - a larger value here was ~262 bytes of someone else's
 * stack, with CONFIG_HW_STACK_PROTECTION off to catch it.
 */
#define ANT_TX_FRAME_MAX 64
#define ANT_TX_QUEUE_DEPTH 32
#define ANT_TX_STACK_SIZE 2048
#define ANT_TX_PRIORITY 5

/* Ring buffer backing store for bulk-OUT data */
#define ANT_RX_BUF_BYTES 512
static uint8_t rx_buf_backing[ANT_RX_BUF_BYTES];
struct ring_buf ant_rx_ring_buf;       /* extern in ant_serial_bridge.c */
K_SEM_DEFINE(ant_rx_sem, 0, 32);      /* extern in ant_serial_bridge.c */

struct ant_tx_frame {
	uint16_t len;
	uint8_t data[ANT_TX_FRAME_MAX];
};

K_MSGQ_DEFINE(ant_tx_msgq, sizeof(struct ant_tx_frame), ANT_TX_QUEUE_DEPTH, 4);
K_THREAD_STACK_DEFINE(ant_tx_stack, ANT_TX_STACK_SIZE);
static struct k_thread ant_tx_thread_data;

static volatile bool configured;
static volatile bool was_configured;
static volatile bool rx_armed;
static volatile bool rx_backpressured;
static uint8_t ep_out_buf[ANT_EP_MPS];
static K_MUTEX_DEFINE(tx_mutex);
static struct usb_ep_cfg_data ant_ep_data[];
static bool tx_thread_started;

#if defined(CONFIG_ANT_DONGLE_MSOS_DESCRIPTORS)

/* Windows uses these descriptors to auto-bind WinUSB for the ANT interface.
 * A retail ANT+ stick has none of this - see the Kconfig help for why that
 * matters, and why this is off by default.
 */
#define ANT_MSOSV2_VENDOR_CODE 0x20
#define ANT_MSOSV1_VENDOR_CODE 0x03
#define ANT_MSOS_COMPAT_ID_WINUSB \
	'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00

/* Random stable GUID {8FCE9F2D-A2E1-4A20-A8CC-3C7C9DA6A51A} */
#define ANT_DEVICE_INTERFACE_GUID \
	'{', 0x00, '8', 0x00, 'F', 0x00, 'C', 0x00, 'E', 0x00, '9', 0x00, \
	'F', 0x00, '2', 0x00, 'D', 0x00, '-', 0x00, 'A', 0x00, '2', 0x00, \
	'E', 0x00, '1', 0x00, '-', 0x00, '4', 0x00, 'A', 0x00, '2', 0x00, \
	'0', 0x00, '-', 0x00, 'A', 0x00, '8', 0x00, 'C', 0x00, 'C', 0x00, \
	'-', 0x00, '3', 0x00, 'C', 0x00, '7', 0x00, 'C', 0x00, '9', 0x00, \
	'D', 0x00, 'A', 0x00, '6', 0x00, 'A', 0x00, '5', 0x00, '1', 0x00, \
	'A', 0x00, '}', 0x00, 0x00, 0x00, 0x00, 0x00

#define ANT_MSOS_STRING_LENGTH 18

static struct ant_msosv2_descriptor {
	struct msosv2_descriptor_set_header header;
	struct msosv2_compatible_id compatible_id;
	struct msosv2_guids_property interface_guids;
} __packed ant_msosv2_descriptor = {
	.header = {
		.wLength = sizeof(struct msosv2_descriptor_set_header),
		.wDescriptorType = MS_OS_20_SET_HEADER_DESCRIPTOR,
		.dwWindowsVersion = 0x06030000,
		.wTotalLength = sizeof(struct ant_msosv2_descriptor),
	},
	.compatible_id = {
		.wLength = sizeof(struct msosv2_compatible_id),
		.wDescriptorType = MS_OS_20_FEATURE_COMPATIBLE_ID,
		.CompatibleID = { ANT_MSOS_COMPAT_ID_WINUSB },
	},
	.interface_guids = {
		.wLength = sizeof(struct msosv2_guids_property),
		.wDescriptorType = MS_OS_20_FEATURE_REG_PROPERTY,
		.wPropertyDataType = MS_OS_20_PROPERTY_DATA_REG_MULTI_SZ,
		.wPropertyNameLength = 42,
		.PropertyName = { DEVICE_INTERFACE_GUIDS_PROPERTY_NAME },
		.wPropertyDataLength = 80,
		.bPropertyData = { ANT_DEVICE_INTERFACE_GUID },
	},
};

static struct ant_msos1_string_desc {
	uint8_t bLength;
	uint8_t bDescriptorType;
	uint8_t bString[ANT_MSOS_STRING_LENGTH];
} __packed ant_msos1_string_descriptor = {
	.bLength = ANT_MSOS_STRING_LENGTH,
	.bDescriptorType = USB_DESC_STRING,
	.bString = {
		'M', 0x00, 'S', 0x00, 'F', 0x00, 'T', 0x00,
		'1', 0x00, '0', 0x00, '0', 0x00,
		ANT_MSOSV1_VENDOR_CODE,
		0x00,
	},
};

static uint8_t ant_msos1_compatid_descriptor[] = {
	0x28, 0x00, 0x00, 0x00,
	0x00, 0x01,
	0x04, 0x00,
	0x01,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00,
	0x01,
	'W', 'I', 'N', 'U', 'S', 'B', 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

USB_DEVICE_BOS_DESC_DEFINE_CAP struct usb_bos_ant_msosv2_desc {
	struct usb_bos_platform_descriptor platform;
	struct usb_bos_capability_msos cap;
} __packed bos_cap_ant_msosv2 = {
	.platform = {
		.bLength = sizeof(struct usb_bos_platform_descriptor) +
			   sizeof(struct usb_bos_capability_msos),
		.bDescriptorType = USB_DESC_DEVICE_CAPABILITY,
		.bDevCapabilityType = USB_BOS_CAPABILITY_PLATFORM,
		.bReserved = 0,
		.PlatformCapabilityUUID = {
			0xDF, 0x60, 0xDD, 0xD8,
			0x89, 0x45,
			0xC7, 0x4C,
			0x9C, 0xD2,
			0x65, 0x9D, 0x9E, 0x64, 0x8A, 0x9F,
		},
	},
	.cap = {
		.dwWindowsVersion = sys_cpu_to_le32(0x06030000),
		.wMSOSDescriptorSetTotalLength =
			sys_cpu_to_le16(sizeof(ant_msosv2_descriptor)),
		.bMS_VendorCode = ANT_MSOSV2_VENDOR_CODE,
		.bAltEnumCode = 0x00,
	},
};

#endif /* CONFIG_ANT_DONGLE_MSOS_DESCRIPTORS */

/* ── USB descriptor ────────────────────────────────────────────────────────── */

struct usb_ant_descriptor {
	struct usb_if_descriptor if0;
	struct usb_ep_descriptor ep_out;
	struct usb_ep_descriptor ep_in;
} __packed;

USBD_CLASS_DESCR_DEFINE(primary, 0) struct usb_ant_descriptor ant_desc = {
	.if0 = {
		.bLength          = sizeof(struct usb_if_descriptor),
		.bDescriptorType  = USB_DESC_INTERFACE,
		.bInterfaceNumber = 0,           /* updated by ant_iface_config */
		.bAlternateSetting = 0,
		.bNumEndpoints    = 2,
		.bInterfaceClass  = USB_BCC_VENDOR,   /* 0xFF */
		.bInterfaceSubClass = 0x00,
		.bInterfaceProtocol = 0x00,
		.iInterface       = 0,
	},
	/* AUTO_EP_*, not the literal 0x01/0x81 the ANTUSB-2 uses: the stack
	 * pairs each endpoint descriptor with its usb_ep_cfg_data entry by
	 * matching addresses, then rewrites both - a descriptor already
	 * carrying the final address matches nothing and usb_enable() fails
	 * silently. Allocation still lands on 0x01/0x81 in practice.
	 */
	.ep_out = {
		.bLength          = sizeof(struct usb_ep_descriptor),
		.bDescriptorType  = USB_DESC_ENDPOINT,
		.bEndpointAddress = AUTO_EP_OUT,
		.bmAttributes     = USB_DC_EP_BULK,
		.wMaxPacketSize   = sys_cpu_to_le16(ANT_EP_MPS),
		.bInterval        = 0,
	},
	.ep_in = {
		.bLength          = sizeof(struct usb_ep_descriptor),
		.bDescriptorType  = USB_DESC_ENDPOINT,
		.bEndpointAddress = AUTO_EP_IN,
		.bmAttributes     = USB_DC_EP_BULK,
		.wMaxPacketSize   = sys_cpu_to_le16(ANT_EP_MPS),
		.bInterval        = 0,
	},
};

/* ── Bulk-OUT receive path ─────────────────────────────────────────────────── */

static void ep_out_cb(uint8_t ep, int tsize, void *priv);

#if defined(CONFIG_ANT_DONGLE_MSOS_DESCRIPTORS)

static int ant_custom_handle_req(struct usb_setup_packet *setup,
				       int32_t *len,
				       uint8_t **data)
{
	if (usb_reqtype_is_to_device(setup)) {
		return -ENOTSUP;
	}

	if (USB_GET_DESCRIPTOR_TYPE(setup->wValue) == USB_DESC_STRING &&
	    USB_GET_DESCRIPTOR_INDEX(setup->wValue) == 0xEE) {
		*data = (uint8_t *)&ant_msos1_string_descriptor;
		*len = sizeof(ant_msos1_string_descriptor);
		return 0;
	}

	return -EINVAL;
}

static int ant_vendor_handle_req(struct usb_setup_packet *setup,
				      int32_t *len,
				      uint8_t **data)
{
	if (usb_reqtype_is_to_device(setup)) {
		return -ENOTSUP;
	}

	if (setup->bRequest == ANT_MSOSV2_VENDOR_CODE &&
	    setup->wIndex == MS_OS_20_DESCRIPTOR_INDEX) {
		*data = (uint8_t *)&ant_msosv2_descriptor;
		*len = sizeof(ant_msosv2_descriptor);
		return 0;
	}

	if (setup->bRequest == ANT_MSOSV1_VENDOR_CODE &&
	    setup->wIndex == 0x04) {
		*data = ant_msos1_compatid_descriptor;
		*len = sizeof(ant_msos1_compatid_descriptor);
		return 0;
	}

	return -ENOTSUP;
}

#define ANT_CUSTOM_HANDLER ant_custom_handle_req
#define ANT_VENDOR_HANDLER ant_vendor_handle_req

#else

/* No 0xEE string descriptor and no vendor requests: a retail stick answers
 * neither, and answering them is what makes Windows bind WinUSB.
 */
#define ANT_CUSTOM_HANDLER NULL
#define ANT_VENDOR_HANDLER NULL

#endif /* CONFIG_ANT_DONGLE_MSOS_DESCRIPTORS */

static int usb_ant_write(const uint8_t *buf, size_t len)
{
	if (!configured) {
		return -ENODEV;
	}

	k_mutex_lock(&tx_mutex, K_FOREVER);
	int ret = usb_transfer_sync(ant_ep_data[ANT_EP_IN_IDX].ep_addr,
				    (uint8_t *)buf, len,
				    USB_TRANS_WRITE);
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

static void arm_rx(void)
{
	if (!configured || rx_armed) {
		return;
	}

	if (ring_buf_space_get(&ant_rx_ring_buf) < sizeof(ep_out_buf)) {
		rx_backpressured = true;
		return;
	}

	int ret = usb_transfer(ant_ep_data[ANT_EP_OUT_IDX].ep_addr,
			       ep_out_buf, sizeof(ep_out_buf),
			       USB_TRANS_READ, ep_out_cb, NULL);
	if (ret == 0) {
		rx_armed = true;
		rx_backpressured = false;
	} else {
		LOG_WRN("Failed to arm USB RX: %d", ret);
	}
}

static void ep_out_cb(uint8_t ep, int tsize, void *priv)
{
	ARG_UNUSED(ep);
	ARG_UNUSED(priv);
	rx_armed = false;

	if (tsize > 0) {
		if (ring_buf_space_get(&ant_rx_ring_buf) < (uint32_t)tsize) {
			rx_backpressured = true;
			LOG_WRN("RX buffer full, delaying re-arm");
		} else {
			ring_buf_put(&ant_rx_ring_buf,
				     ep_out_buf, (uint32_t)tsize);
			k_sem_give(&ant_rx_sem);
		}
	}

	if (configured) {
		arm_rx();
	}
}

/* ── USB device status callback ────────────────────────────────────────────── */

static void ant_status_cb(struct usb_cfg_data *cfg,
			  enum usb_dc_status_code status,
			  const uint8_t *param)
{
	ARG_UNUSED(cfg);
	ARG_UNUSED(param);

	switch (status) {
	case USB_DC_CONFIGURED:
		configured = true;
		was_configured = true;
		rx_armed = false;
		rx_backpressured = false;
		arm_rx();
		LOG_INF("USB configured");
		break;
	case USB_DC_RESUME:
		/* A host does not repeat SET_CONFIGURATION after a bus resume -
		 * the device stays configured across a suspend from its point of
		 * view, so USB_DC_CONFIGURED never comes again. Without this the
		 * suspend below is one-way: usb_ant_write() answers every command
		 * with -ENODEV and arm_rx() never re-arms EP_OUT, so the dongle
		 * enumerates, binds its driver and then replies to nothing until
		 * it is physically replugged. The trigger is a laptop sleep/wake.
		 */
		if (was_configured) {
			configured = true;
			rx_armed = false;
			rx_backpressured = false;
			arm_rx();
			LOG_INF("USB resumed");
		}
		break;
	case USB_DC_DISCONNECTED:
		was_configured = false;
		/* usb_device.c cancels in-flight transfers itself on DISCONNECTED
		 * and RESET, but not on SUSPEND - hence the explicit call there.
		 */
		configured = false;
		rx_armed = false;
		rx_backpressured = false;
		k_msgq_purge(&ant_tx_msgq);
		break;
	case USB_DC_SUSPEND:
		configured = false;
		rx_armed = false;
		rx_backpressured = false;
		k_msgq_purge(&ant_tx_msgq);
		/* usb_ant_write() holds tx_mutex across a usb_transfer_sync() that
		 * waits K_FOREVER. A suspend landing mid-transfer would otherwise
		 * park the bridge thread - and the TX thread behind it - for the
		 * whole duration of the host's sleep, with the mutex held.
		 */
		usb_cancel_transfers();
		break;
	default:
		break;
	}
}

/* ── Interface config callback (assigns interface number) ─────────────────── */

static void ant_iface_config(struct usb_desc_header *head, uint8_t bInterfaceNumber)
{
	ARG_UNUSED(head);
	ant_desc.if0.bInterfaceNumber = bInterfaceNumber;
#if defined(CONFIG_ANT_DONGLE_MSOS_DESCRIPTORS)
	/* The MS OS 1.0 compatible ID section names the interface it applies to. */
	ant_msos1_compatid_descriptor[16] = bInterfaceNumber;
#endif
}

/* ── Endpoint table ────────────────────────────────────────────────────────── */

static struct usb_ep_cfg_data ant_ep_data[] = {
	{ .ep_cb = usb_transfer_ep_callback, .ep_addr = AUTO_EP_OUT },
	{ .ep_cb = usb_transfer_ep_callback, .ep_addr = AUTO_EP_IN  },
};

/* ── Config data registration ──────────────────────────────────────────────── */

USBD_DEFINE_CFG_DATA(ant_usb_cfg) = {
	.usb_device_description = NULL,
	.interface_descriptor   = &ant_desc.if0,
	.interface_config       = ant_iface_config,
	.cb_usb_status          = ant_status_cb,
	.interface = {
		.class_handler  = NULL,
		.vendor_handler = ANT_VENDOR_HANDLER,
		.custom_handler = ANT_CUSTOM_HANDLER,
	},
	.num_endpoints = ARRAY_SIZE(ant_ep_data),
	.endpoint      = ant_ep_data,
};

/* ── Public API ────────────────────────────────────────────────────────────── */

void usb_ant_class_init(void)
{
	ring_buf_init(&ant_rx_ring_buf, ANT_RX_BUF_BYTES, rx_buf_backing);
	rx_armed = false;
	rx_backpressured = false;
#if defined(CONFIG_ANT_DONGLE_MSOS_DESCRIPTORS)
	usb_bos_register_cap((void *)&bos_cap_ant_msosv2);
#endif

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
#if CONFIG_ANT_DONGLE_BCD_DEVICE != 0
	/* Bring-up knob: usb_get_device_descriptor() is legacy-stack API, kept
	 * here (not main()) so main() stays stack-agnostic. Zephyr pins
	 * bcdDevice identically across rebuilds, and Windows keys its cached
	 * MS OS descriptor verdict on VID+PID+bcdDevice - bumping it gives
	 * Windows a device with no cached verdict. Must run before
	 * usb_enable(), which publishes the descriptor.
	 */
	struct usb_device_descriptor *dev_desc =
		(struct usb_device_descriptor *)usb_get_device_descriptor();

	dev_desc->bcdDevice = sys_cpu_to_le16(CONFIG_ANT_DONGLE_BCD_DEVICE);
	LOG_INF("bcdDevice overridden to 0x%04X",
		(unsigned int)CONFIG_ANT_DONGLE_BCD_DEVICE);
#endif

	return usb_enable(NULL);
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
