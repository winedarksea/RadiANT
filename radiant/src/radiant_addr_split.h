/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_addr_split.h - finish an address match in software.
 *
 * Provenance: clean-room. Written from radiant_radio_hal.h (struct
 * radiant_rx_filter, the t_sync definition) and this project's own bench
 * measurements (docs/decisions/0019-tracking-matches-a-short-address.md).
 * Nothing derives from sdk-ant or any ANT+ document.
 *
 * WHY. The nRF backend can match fewer address bytes in hardware than the
 * format puts on the air, and receive the rest as the first payload bytes.
 * ADR 0019 does that for the TRACKING addresses the demodulator slips on (a
 * long alternating run, e.g. #54100: every RadiANT receiver missed ~2 % of
 * frames that a retail stick heard). The bytes the hardware no longer matches
 * are matched here instead, removed from the body, and their airtime added
 * back to t_sync. A frame whose slip landed in them is repaired here too.
 *
 * Pure functions over plain data, no Zephyr or register access, so the suite
 * in radiant/tests/src/test_addr_split.c runs them on any board without the
 * nRF backend in the image.
 */

#ifndef RADIANT_ADDR_SPLIT_H_
#define RADIANT_ADDR_SPLIT_H_

#include <stdbool.h>
#include <stdint.h>

#include <radiant/radiant_radio_hal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Airtime of one address byte on the 1 Mbit/s PHY, microseconds. */
#define RADIANT_ADDR_SPLIT_US_PER_BYTE 8u

/*
 * How many of a receive format's address bytes the hardware matches.
 *
 * Only the full-length (5-byte, TRACKING) address on the 1 M PHY is ever
 * shortened. `hw_len_cfg` is the configured hardware length (2..5); anything
 * shorter than the format, or not 5 bytes, is matched in full as before.
 * The search format (3 bytes) is "permanent" by design and is not touched.
 */
static inline uint8_t radiant_addr_split_hw_len(uint8_t addr_len, bool phy_1m,
						 uint8_t hw_len_cfg)
{
	if (!phy_1m || addr_len != RADIANT_RADIO_ADDR_MAX ||
	    hw_len_cfg < 2u || hw_len_cfg >= addr_len) {
		return addr_len;
	}
	return hw_len_cfg;
}

/*
 * The full-address filter a received frame belongs to, or -1 for a frame that
 * matched the hardware's short address but is somebody else's (a "foreign"
 * frame: same leading bytes, different device number or type).
 *
 *   filters, n   the window's filters, each RADIANT_RADIO_ADDR_MAX wide
 *   hw_len       bytes the hardware matched (addr[0 .. hw_len))
 *   sw_len       bytes left to match here (addr[hw_len .. hw_len + sw_len))
 *   hint         the filter whose hardware address the RADIO reported a
 *                match on (RXMATCH, mapped back through the backend's slot
 *                table). Any filter with the same leading hw_len bytes is a
 *                candidate: with a short match several filters share one
 *                hardware address.
 *   head         the first sw_len bytes the RADIO delivered as payload
 *
 * Returns the lowest matching index, so the answer does not depend on which
 * of several identical hardware addresses RXMATCH happened to name.
 */
int radiant_addr_split_match(const struct radiant_rx_filter *filters,
			     uint8_t n, uint8_t hw_len, uint8_t sw_len,
			     uint8_t hint, const uint8_t *head);

/*
 * Longest run of alternating bits (0101... or 1010...) in an address, in
 * on-air order: byte 0 first, each byte MSb first. A single bit counts as a
 * run of 1; an empty or NULL address returns 0.
 *
 * WHY THIS NUMBER. The nRF RADIO in Nrf_1Mbit drops exactly one bit when a
 * long alternating run sits inside the address it is matching, or within a
 * few bits after its end (ADR 0019; archive/benchmarks/2026-11-07-devnum-rx-
 * loss). Inside the address the frame is never seen at all. The run length is
 * what predicts it: 9 or more lost 1-5 % on every nRF part tested, 8 lost at
 * most 0.2 %, 7 and below nothing measurable.
 */
uint8_t radiant_addr_split_alt_run(const uint8_t *addr, uint8_t len);

/*
 * How many address bytes the hardware matches for one receive window, given
 * all of its filters.
 *
 *   hw_len_cfg   the fixed length radiant_addr_split_hw_len() applies (5 =
 *                match everything, the stock behaviour)
 *   guard_run    0, or the alternating-run length at which an address is "at
 *                risk": if ANY 5-byte filter on a 1 M window has a run of at
 *                least this, the window matches only 2 bytes, which puts
 *                every device-number bit outside the hardware match - a slip
 *                there is then a delivered frame radiant_addr_split_repair()
 *                can fix rather than a frame never seen.
 *
 * An address with no such run keeps the full hardware match. That is
 * deliberate: a 2-byte match also matches noise (~2^-16 per bit) and costs a
 * frame whenever the false match lands just before the real one - measured
 * ~0.4 % on a clean number - so it is only worth paying where it buys back a
 * slip.
 */
uint8_t radiant_addr_split_rx_hw_len(const struct radiant_rx_filter *filters,
				     uint8_t n, uint8_t addr_len, bool phy_1m,
				     uint8_t hw_len_cfg, uint8_t guard_run);

/*
 * Recover a frame the RADIO delivered with exactly one bit deleted inside its
 * software-matched address bytes (the slip described above), or return -1.
 *
 *   filters..hint  as radiant_addr_split_match()
 *   payload        what the RADIO stored: sw_len address bytes then body_len
 *                  body bytes, on-air order, MSb first
 *   rxcrc          the RADIO's RXCRC: the 16 bits that followed the payload,
 *                  first-received bit in bit 15
 *   crc_init       the format's CRC seed (CRC-16, poly 0x1021, over the whole
 *                  address and the body)
 *   body_out       body_len bytes, written only on success
 *
 * A deletion at bit j of the software bytes leaves every later bit one place
 * early, with one bit of whatever followed the frame appended. So the frame is
 * recoverable from what arrived: the address is the candidate filter's own,
 * and the body and CRC are the received bits from (sw_len*8 - 1) on. It is
 * accepted only if (a) the received address bits are exactly the filter's
 * with one bit deleted - not equal to it, and not anything else - and (b) the
 * CRC over the reconstructed frame verifies. That is one CRC trial per
 * candidate filter, and only on frames that already failed the plain compare.
 *
 * Returns the lowest such filter index. A deletion in the body, an insertion,
 * or two slips are not repaired.
 */
int radiant_addr_split_repair(const struct radiant_rx_filter *filters,
			      uint8_t n, uint8_t hw_len, uint8_t sw_len,
			      uint8_t hint, const uint8_t *payload,
			      uint8_t body_len, uint32_t rxcrc,
			      uint16_t crc_init, uint8_t *body_out);

/* The body the core is owed: the delivered payload with the software-matched
 * address bytes removed. */
static inline const uint8_t *radiant_addr_split_body(const uint8_t *payload,
						     uint8_t sw_len)
{
	return payload + sw_len;
}

/*
 * What to add to a t_sync captured at the hardware's ADDRESS event. The HAL
 * defines t_sync as the instant the LAST bit of the on-air address was at the
 * antenna; the hardware now signals sw_len bytes before that.
 */
static inline uint32_t radiant_addr_split_t_sync_us(uint8_t sw_len)
{
	return (uint32_t)sw_len * RADIANT_ADDR_SPLIT_US_PER_BYTE;
}

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_ADDR_SPLIT_H_ */
