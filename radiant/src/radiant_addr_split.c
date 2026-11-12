/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_addr_split.c - see radiant_addr_split.h.
 *
 * Provenance: clean-room. See the header.
 */

#include <stdbool.h>
#include <string.h>

#include <radiant/radiant_frame.h>

#include "radiant_addr_split.h"

int radiant_addr_split_match(const struct radiant_rx_filter *filters,
			     uint8_t n, uint8_t hw_len, uint8_t sw_len,
			     uint8_t hint, const uint8_t *head)
{
	if (filters == NULL || head == NULL || n == 0u || hint >= n ||
	    hw_len == 0u ||
	    (uint32_t)hw_len + (uint32_t)sw_len > RADIANT_RADIO_ADDR_MAX) {
		return -1;
	}

	for (uint8_t i = 0; i < n; i++) {
		const struct radiant_rx_filter *f = &filters[i];

		if ((uint32_t)f->addr_len != (uint32_t)hw_len + sw_len) {
			continue;
		}
		/* Same hardware address as the one the RADIO matched? */
		if (memcmp(f->addr, filters[hint].addr, hw_len) != 0) {
			continue;
		}
		/* A wild_last filter (ADR 0022) accepts any final byte, so only
		 * the software bytes before it are compared. It needs at least
		 * that final byte left to software; rx_hw_len() guarantees it. */
		if (f->wild_last) {
			if (sw_len != 0u &&
			    memcmp(&f->addr[hw_len], head, sw_len - 1u) == 0) {
				return (int)i;
			}
			continue;
		}
		if (sw_len == 0u || memcmp(&f->addr[hw_len], head, sw_len) == 0) {
			return (int)i;
		}
	}
	return -1;
}

/* Bit i of a byte string in on-air order (byte 0 first, MSb first). */
static inline uint8_t bit_at(const uint8_t *s, uint32_t i)
{
	return (uint8_t)((s[i >> 3] >> (7u - (i & 7u))) & 1u);
}

uint8_t radiant_addr_split_rx_hw_len(const struct radiant_rx_filter *filters,
				     uint8_t n, uint8_t addr_len, bool phy_1m,
				     uint8_t hw_len_cfg, uint8_t guard_run)
{
	uint8_t hw = radiant_addr_split_hw_len(addr_len, phy_1m, hw_len_cfg);

	/* Wildcard search (ADR 0022): the last byte must reach software, so
	 * the hardware matches everything before it - [A6 C5] on the 3-byte
	 * search address. Never fewer than 2: the RADIO needs a base byte and
	 * a prefix byte. Only on 1 M, where the bit stream is unchanged by
	 * moving the match boundary. */
	if (filters != NULL && phy_1m && addr_len >= 3u) {
		for (uint8_t i = 0; i < n; i++) {
			if (filters[i].addr_len == addr_len &&
			    filters[i].wild_last) {
				return (hw < addr_len) ? hw
						       : (uint8_t)(addr_len - 1u);
			}
		}
	}

	if (guard_run == 0u || filters == NULL || hw <= 2u ||
	    radiant_addr_split_hw_len(addr_len, phy_1m, 2u) != 2u) {
		return hw;
	}
	for (uint8_t i = 0; i < n; i++) {
		if (filters[i].addr_len == addr_len &&
		    radiant_addr_split_alt_run(filters[i].addr, addr_len) >=
			    guard_run) {
			return 2u;
		}
	}
	return hw;
}

int radiant_addr_split_repair(const struct radiant_rx_filter *filters,
			      uint8_t n, uint8_t hw_len, uint8_t sw_len,
			      uint8_t hint, const uint8_t *payload,
			      uint8_t body_len, uint32_t rxcrc,
			      uint16_t crc_init, uint8_t *body_out)
{
	const uint32_t L = (uint32_t)sw_len * 8u;
	uint8_t body[RADIANT_RADIO_BODY_MAX];
	uint16_t crc_rx;

	if (filters == NULL || payload == NULL || body_out == NULL ||
	    n == 0u || hint >= n || hw_len == 0u || sw_len == 0u ||
	    body_len == 0u || body_len > RADIANT_RADIO_BODY_MAX ||
	    (uint32_t)hw_len + (uint32_t)sw_len > RADIANT_RADIO_ADDR_MAX) {
		return -1;
	}

	/*
	 * The reconstruction is the same for every candidate: received bits
	 * from L-1 on are the original body and CRC (the bit at L-1 is the
	 * original's L, and so on), the last original CRC bit being RXCRC's
	 * bit 1 and RXCRC's bit 0 whatever followed the frame.
	 */
	for (uint8_t k = 0; k < body_len; k++) {
		body[k] = (uint8_t)((payload[sw_len - 1u + k] << 7) |
				    (payload[sw_len + k] >> 1));
	}
	crc_rx = (uint16_t)(((uint32_t)(payload[sw_len + body_len - 1u] & 1u)
			     << 15) |
			    ((rxcrc & 0xFFFFu) >> 1));

	for (uint8_t i = 0; i < n; i++) {
		const struct radiant_rx_filter *f = &filters[i];
		const uint8_t *exp;
		uint32_t m = 0;
		bool ok = true;

		/* A wild filter has no expected final byte to compare a slipped
		 * one against, so it is never repaired here. */
		if (f->wild_last ||
		    (uint32_t)f->addr_len != (uint32_t)hw_len + sw_len ||
		    memcmp(f->addr, filters[hint].addr, hw_len) != 0) {
			continue;
		}
		exp = &f->addr[hw_len];

		/* First received address bit that disagrees: the deletion is at
		 * or before it. None means this was not a slip in the address. */
		while (m < L && bit_at(payload, m) == bit_at(exp, m)) {
			m++;
		}
		if (m == L) {
			continue;
		}
		/* Everything after it must be the expected bits one place
		 * early - a single deletion, nothing else. */
		for (uint32_t j = m; j + 1u < L; j++) {
			if (bit_at(payload, j) != bit_at(exp, j + 1u)) {
				ok = false;
				break;
			}
		}
		if (!ok) {
			continue;
		}
		if (radiant_crc16(radiant_crc16(crc_init, f->addr, f->addr_len),
				  body, body_len) != crc_rx) {
			continue;
		}
		memcpy(body_out, body, body_len);
		return (int)i;
	}
	return -1;
}
