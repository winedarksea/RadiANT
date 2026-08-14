/* SPDX-License-Identifier: Apache-2.0 */

/*
 * Transport contract between the ANT serial bridge and whatever carries its
 * bytes to a host.
 *
 * The bridge does not care what the wire is. It needs a byte sink, a byte
 * source, and a way to say "I have drained my input, you may take more".
 * Three backends implement this, selected by CONFIG_ANT_DONGLE_TRANSPORT_*:
 *
 *   usb_ant_class.c        legacy USB device stack. The proven one - this is
 *                          what enumerates as 0FCF:1009 and what Zwift has
 *                          been verified against. Default wherever it builds.
 *   usb_ant_class_next.c   the new USB device stack (USBD/UDC). Same wire
 *                          appearance, different plumbing. Mandatory on any
 *                          nRF54 part: their USB controller is a DesignWare
 *                          DWC2, whose only Zephyr driver lives under
 *                          drivers/usb/udc/, so there is no legacy path to
 *                          fall back to.
 *   ant_uart_transport.c   raw UART. For parts with no USB device peripheral
 *                          at all - the nRF54L15 among them. ANT's serial
 *                          protocol is a UART protocol to begin with; a USB
 *                          stick is a UART bridge in a plastic shell, so this
 *                          is the same protocol minus the shell.
 *
 * The names below are the ones usb_ant_class.c already exported, kept exactly
 * as they were so that file did not have to be touched to gain a sibling.
 */

#ifndef ANT_TRANSPORT_H_
#define ANT_TRANSPORT_H_

#include <stddef.h>
#include <stdint.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

/* Host -> device bytes land here; the bridge thread drains them. */
extern struct ring_buf ant_rx_ring_buf;
extern struct k_sem ant_rx_sem;

/** Set up buffers, threads and descriptors. Does not touch the bus. */
void usb_ant_class_init(void);

/**
 * Bring the transport up: enumerate, or open the UART.
 *
 * Split from usb_ant_class_init() because ANT/MPSL must own clock startup
 * first - see the ordering in main(). Returns 0 or a negative errno.
 */
int ant_transport_enable(void);

/** Send one ANT serial frame, blocking until it is on the wire. */
int usb_ant_send(const uint8_t *buf, size_t len);

/**
 * Queue one ANT serial frame for a writer thread.
 *
 * For callers that must not block - specifically ant_evt_handler(), which runs
 * on sdk-ant's work queue and whose stack is not ours to spend.
 */
int usb_ant_send_async(const uint8_t *buf, size_t len);

/**
 * Re-arm reception after the bridge has made room in ant_rx_ring_buf.
 *
 * A transport that stopped reading because the ring buffer was full has no
 * other way to learn that it has drained.
 */
void usb_ant_resume_rx(void);

#endif /* ANT_TRANSPORT_H_ */
