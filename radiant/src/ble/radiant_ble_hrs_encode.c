/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_hrs_encode.c - the Heart Rate Measurement encoder, a pure
 * function: no kernel, no Bluetooth host. Compiled into an image by
 * CONFIG_RADIANT_BLE_HRS and, unmodified, by apps/hrm_ble/tests/publish on
 * native_sim. See <radiant/radiant_ble_hrs.h>.
 */

#include <errno.h>

#include <radiant/radiant_ble_hrs.h>

static void put_le16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v & 0xFFu);
	p[1] = (uint8_t)(v >> 8);
}

int radiant_ble_hrs_encode(const struct radiant_ble_hrs_meas *m, uint8_t *buf,
			   size_t cap, size_t *rr_written)
{
	uint8_t flags = 0u;
	size_t len;
	size_t room;
	size_t n;

	if (rr_written != NULL) {
		*rr_written = 0u;
	}
	if (m == NULL || buf == NULL || (m->rr == NULL && m->rr_count > 0u)) {
		return -EINVAL;
	}

	/* Bit 0, the rate format: UINT8 whenever the rate fits it. HRS lets
	 * a sensor use UINT16 always, but the short form is what every
	 * collector expects and leaves room for one more RR-Interval. */
	len = (m->bpm > 0xFFu) ? 3u : 2u;
	if (cap < len) {
		return -ENOMEM;
	}
	if (m->bpm > 0xFFu) {
		flags |= RADIANT_BLE_HRS_FLAG_UINT16;
		put_le16(&buf[1], m->bpm);
	} else {
		buf[1] = (uint8_t)m->bpm;
	}

	/* Bits 1-2: support is the capability, status the live state. With
	 * support off both bits are 0 ("not supported"); status alone is
	 * never sent. */
	if (m->contact_supported) {
		flags |= RADIANT_BLE_HRS_FLAG_CONTACT_SUPP;
		if (m->contact_detected) {
			flags |= RADIANT_BLE_HRS_FLAG_CONTACT;
		}
	}

	/* Bit 3, Energy Expended: never. No Control Point either. */

	/* Bit 4: as many RR-Intervals as fit, the OLDEST first. HRS 1.0
	 * 3.1.1.5: intervals that do not fit are sent in the next
	 * notification, so the caller keeps the rest pending. */
	room = (cap - len) / 2u;
	n = (m->rr_count < room) ? m->rr_count : room;
	if (n > 0u) {
		flags |= RADIANT_BLE_HRS_FLAG_RR;
		for (size_t i = 0u; i < n; i++) {
			put_le16(&buf[len], m->rr[i]);
			len += 2u;
		}
	}

	buf[0] = flags;
	if (rr_written != NULL) {
		*rr_written = n;
	}
	return (int)len;
}
