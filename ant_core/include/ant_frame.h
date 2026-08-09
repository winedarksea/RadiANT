/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ant_frame.h - the ANT on-air frame: CRC, address packing, encode, decode.
 *
 * Provenance: clean-room. Written from docs/ant-radio-link.md (the frame
 * layout, the CRC parameters, the two packet configurations and the two
 * address-packing rules) and docs/spike-a-results.md (the measurements those
 * rest on, 2026-08-09, nRF54L15 DK), with public nRF52840/nRF54L15 product
 * specifications behind the reasoning about *why* the two configurations
 * differ. Nothing here derives from sdk-ant, from libant.a, from disassembly
 * of any binary, or from any adopter-gated ANT+ device profile document; no
 * expression from rtl_433 was read or transliterated - only the facts already
 * recorded in docs/ant-radio-link.md. See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this file is
 * ---------------------------------------------------------------------------
 * The layer with no radio in it. Everything here is a pure function of bytes:
 * given a channel ID and a payload, what goes on the air; given what came off
 * the air, what channel ID and payload that was; and the CRC that decides
 * whether the second question was worth asking. That is why it is the first
 * module written and the only one that is fully testable in CI with no board.
 *
 * The frame, every byte of it measured (docs/ant-radio-link.md, confirmed by
 * Spike A on 2,164 real frames):
 *
 *       1        2         2        1       1       1        8         2
 *  +--------+---------+---------+-------+-------+--------+---------+--------+
 *  |preamble| net addr| dev num | dtype | ttype | length | payload |  CRC   |
 *  |  0x55  |  A6 C5  | lo  hi  |       |       |  0x0A  | d0..d7  |        |
 *  +--------+---------+---------+-------+-------+--------+---------+--------+
 *           |<----------------- 15 bytes CRC coverage ------------>|
 *           |<------------------------ 17 bytes ------------------------->|
 *
 * The preamble is not this module's business: it is a PHY property, and on the
 * parts in view the radio derives it from the first address bit with no
 * software involvement. Everything from the network address onwards is.
 *
 * ---------------------------------------------------------------------------
 * Two configurations, not one - and the split is not where a reader expects
 * ---------------------------------------------------------------------------
 * The same 15 covered bytes are divided differently depending on how many of
 * them the receiver matched in hardware:
 *
 *   tracking/TX   address A6 C5 dnlo dnhi dtype     body [ttype][len][d0..d7]
 *                 5 bytes                           10 bytes
 *   search        address A6 C5 dnlo                body [dnhi][dtype][ttype]
 *                 3 bytes                                [len][d0..d7]
 *                                                   12 bytes
 *
 * Concatenated, those are byte-for-byte the same 15 bytes and therefore the
 * same CRC - which is the 15-byte coverage invariant, and is asserted in
 * ant_core/tests/src/test_frame.c in both directions. The split matters
 * because a matched address byte never reaches RAM: in tracking the channel ID
 * is proven by the match and cannot be read back, while in search it is in the
 * buffer and can be. Search exists at all because the nRF's fixed
 * S0|LENGTH|S1 layout has no slot for a length field with three bytes ahead of
 * it, so search runs static-length; that is a backend fact, but it is the
 * reason this module has to express both layouts and convert between them.
 *
 * The one byte of a search frame that is in neither place is devnum_lo: it is
 * consumed by the matcher, and the core recovers it from which filter matched
 * (see ant_rx_event.filter_index in ant_radio_hal.h). So a caller decoding a
 * search frame fills wire.addr from its own filter table before calling here.
 * This module never guesses it.
 *
 * ---------------------------------------------------------------------------
 * No register semantics
 * ---------------------------------------------------------------------------
 * Registers belong to a backend. This header speaks frames, channel IDs,
 * buffers and on-air byte order. ant_addr_pack_leading() does return a 32-bit
 * word, because every planned backend wants one, but it is named for what it
 * is - the leading on-air address bytes packed in transmission order - and not
 * for the register it happens to land in on one part.
 */

#ifndef ANT_FRAME_H_
#define ANT_FRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ant_radio_hal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Return codes
 *
 * Distinct from the HAL's, and distinct from each other, because the tests
 * that matter most here are the malformed-input ones and "it failed" is not a
 * result worth asserting on. Negative for the same reason the HAL's are: a
 * call can return either an error or a small non-negative length.
 * ---------------------------------------------------------------------------
 */
#define ANT_FRAME_OK        0
#define ANT_FRAME_EINVAL  (-1)  /* null pointer, unknown configuration, or a
				 * length no configuration can express */
#define ANT_FRAME_ELEN    (-2)  /* the on-air length byte disagrees with the
				 * number of payload bytes actually present */
#define ANT_FRAME_ECRC    (-3)  /* the CRC did not verify. Reported, never
				 * repaired; the output is left untouched */
#define ANT_FRAME_ETRUNC  (-4)  /* the buffer is shorter than the frame it
				 * claims to hold */

/* ---------------------------------------------------------------------------
 * The network address
 *
 * Two bytes, and the only key-dependent thing in the whole frame. The ANT+
 * network key B9 A5 21 FB BD 72 C3 45 corresponds to A6 C5; the key -> address
 * function is unknown outside Garmin, is not needed, and is not to be fitted
 * from samples (docs/ant-radio-link.md). ant_net.c holds the one-entry table.
 * Every function here takes the address as an argument rather than assuming
 * ANT+, so that table is the only place the pairing is written down.
 * ---------------------------------------------------------------------------
 */
#define ANT_NET_ADDR_LEN 2

#define ANT_NET_ADDR_ANT_PLUS_0 0xA6u  /* first byte on air */
#define ANT_NET_ADDR_ANT_PLUS_1 0xC5u

/* The same two bytes as an array, for passing to the functions below. */
extern const uint8_t ant_net_addr_ant_plus[ANT_NET_ADDR_LEN];

/* ---------------------------------------------------------------------------
 * CRC-16/CCITT-FALSE
 *
 * Width 16, polynomial 0x1021 in normal (unreflected) form, init 0xFFFF, no
 * reflection either way, no final XOR. It covers the on-air address bytes as
 * well as the body - 15 bytes for a standard frame - which is the property
 * most sync-word matchers do not have and which therefore decides whether a
 * given radio can do this in hardware at all.
 * ---------------------------------------------------------------------------
 */
#define ANT_CRC_POLY 0x1021u
#define ANT_CRC_INIT 0xFFFFu

/*
 * The state of the CRC register after it has consumed the ANT+ network address
 * A6 C5 from init 0xFFFF.
 *
 * This is not a curiosity. CCITT-FALSE chains, so a radio whose CRC engine
 * cannot be made to cover its own sync word still produces ANT's exact CRC by
 * folding this constant in as the initial value and covering only the
 * remaining 13 bytes. That is what makes hardware CRC reachable on EFR32/RAIL
 * (docs/backends.md), and it holds only while the covered prefix is constant -
 * a sync word carrying the device number varies per channel and cannot be
 * folded. Spike A used it on real silicon in its phase E, which failed for an
 * unrelated reason, so the confirmation behind it is arithmetic.
 */
#define ANT_CRC_AFTER_ANT_PLUS_NET 0x233Eu

/*
 * Run the CRC over len bytes, starting from seed.
 *
 * seed is a parameter rather than a constant precisely so the chaining above
 * is expressible. Pass ANT_CRC_INIT for a whole frame.
 *
 * Two ways to check a received frame, and they catch different mistakes, so
 * assert both:
 *   - compute over the 15 covered bytes and compare against the received 2, or
 *   - compute over all 17 and check the result is zero.
 * The second works because there is no final XOR and no reflection, so
 * appending a correct CRC drives the register to zero. Both held on every one
 * of Spike A's 2,164 CRC-valid frames.
 */
uint16_t ant_crc16(uint16_t seed, const uint8_t *data, size_t len);

/* ---------------------------------------------------------------------------
 * Bit order, and the address packing every backend needs
 *
 * Address bits go out least-significant-bit first, whatever the endianness
 * setting that governs the rest of the frame says. Spike A settled this: the
 * bit-reversed packing caught every frame the transmitter sent, and the other
 * seven permutations of bit order, byte order and prefix position heard
 * literally nothing - not degraded reception, zero address matches. A wrong
 * answer here does not produce a weak link, it produces silence, which is
 * worth knowing when a backend port comes up dead.
 *
 * These live here rather than in a backend because they are pure functions of
 * the on-air address, every backend needs them, and two implementations of
 * this arithmetic is one more than the number that can be right.
 * ---------------------------------------------------------------------------
 */

/* Reverse the bits of one byte. rev8(0xA6) = 0x65, rev8(0xC5) = 0xA3. */
uint8_t ant_rev8(uint8_t b);

/*
 * The leading (addr_len - 1) bytes of an on-air address, bit-reversed and
 * packed into one 32-bit word in transmission order.
 *
 *     word = (lsb-first packing of the leading bytes) << (8 * (4 - n_leading))
 *
 * Two separate rules are folded into that one expression, and the folding is
 * the point:
 *
 *   - the first byte on air sits in the *lowest* occupied byte of the word,
 *     and every byte is bit-reversed on the way in;
 *   - when fewer than four leading bytes are used, the used bytes are the
 *     *high* ones. A short address is truncated from the least significant
 *     byte, and under the order above the bytes truncation would discard are
 *     exactly the ones that must survive.
 *
 * Written as a shift it collapses to the four-byte case at n_leading == 4, so
 * a backend has one expression rather than two. The naive reading - keep the
 * low bytes - is not merely suboptimal: Spike A measured it producing 9 to 20
 * address matches per 60 s window with *zero* CRC-valid frames at -54 to
 * -101 dBm, against 24 CRC-valid frames at -17 dBm for the correct packing.
 * That is a matcher firing on noise, and the RSSI column separates the two by
 * about 70 dB. It is asserted against in test_frame.c by name.
 *
 * Returns 0 for an addr_len outside 2..ANT_FRAME_ADDR_MAX, which is also a
 * legitimate value for an all-zero address; callers that care validate the
 * length themselves rather than testing the result.
 */
uint32_t ant_addr_pack_leading(const uint8_t *addr, uint8_t addr_len);

/*
 * The last byte of an on-air address, bit-reversed. It is transmitted last,
 * and on the nRF24-descended parts it is the byte that distinguishes one
 * logical address from another while the leading bytes are shared - which is
 * exactly what the eight-filter wildcard sweep in ant_search.c exploits.
 * Returns 0 for an invalid addr_len, on the same terms as above.
 */
uint8_t ant_addr_pack_trailing(const uint8_t *addr, uint8_t addr_len);

/* ---------------------------------------------------------------------------
 * Channel ID
 * ---------------------------------------------------------------------------
 */

/* The four bytes that follow the network address are exactly ANT's channel ID,
 * which is why an on-air address match *is* a channel-ID match and no software
 * comparison is needed in the common case. */
struct ant_channel_id {
	uint16_t device_number;
	uint8_t  device_type;   /* MSB is the pairing bit, so types are 1..127 */
	uint8_t  trans_type;
};

#define ANT_DEVICE_TYPE_PAIRING_BIT 0x80u
#define ANT_DEVICE_TYPE_MASK        0x7Fu

/* ---------------------------------------------------------------------------
 * Geometry
 * ---------------------------------------------------------------------------
 */

enum ant_frame_cfg {
	/* 5-byte address, 2-byte header. What a tracked channel and every
	 * transmitter use. */
	ANT_FRAME_CFG_TRACKING = 0,
	/* 3-byte address, 4-byte header. What wildcard search uses, because
	 * three matched bytes is the shortest address the hardware will take
	 * and the channel ID then has to be readable out of RAM. */
	ANT_FRAME_CFG_SEARCH = 1,
	ANT_FRAME_CFG_COUNT
};

#define ANT_FRAME_ADDR_LEN_TRACKING 5u
#define ANT_FRAME_ADDR_LEN_SEARCH   3u
#define ANT_FRAME_ADDR_MAX          5u

/* Body bytes ahead of the payload: [ttype][len] in tracking,
 * [dnhi][dtype][ttype][len] in search. */
#define ANT_FRAME_HDR_LEN_TRACKING  2u
#define ANT_FRAME_HDR_LEN_SEARCH    4u

/* Payload bytes. 8 is every frame anyone has ever measured; 24 is the
 * advanced-burst frame this module is sized for but which has never been seen
 * on air (see the length byte, below). */
#define ANT_FRAME_PAYLOAD_STD 8u
#define ANT_FRAME_PAYLOAD_MAX 24u

/* Longest body either configuration can produce: search header plus the
 * largest payload. Stays inside the HAL's advisory ANT_RADIO_BODY_MAX. */
#define ANT_FRAME_BODY_MAX (ANT_FRAME_HDR_LEN_SEARCH + ANT_FRAME_PAYLOAD_MAX)

#define ANT_FRAME_CRC_BYTES 2u

/* The whole reason the length byte reads 10 rather than 8. */
#define ANT_FRAME_LEN_BROADCAST 0x0Au

/*
 * The prediction, and it is written here so that it is falsifiable rather than
 * merely believed: an advanced-burst frame carrying 24 payload bytes should
 * show 0x1A. That has never been observed on air. Spike B is what settles it,
 * and it may instead show that this byte is ANT's control byte - encoding
 * broadcast against acknowledged against burst sequence - which would look
 * identical in every frame anyone has captured, because a passive ANT+ sniffer
 * sees nothing but sensor broadcasts. If that is the answer, this macro and
 * ant_frame_len_byte() are where it lands.
 */
#define ANT_FRAME_LEN_ADV_BURST 0x1Au

/*
 * The on-air length byte for a given payload: ShockBurst semantics, in which
 * the length counts the two CRC bytes. Ten, not eight.
 *
 * A pleasant consequence worth naming, because it makes one backend simpler:
 * in the tracking configuration the two header bytes ahead of the payload and
 * the two CRC bytes cancel exactly, so body_len == len_byte for every payload
 * length. A radio that derives body length from a field in the body needs a
 * bias of zero. In search they do not cancel - four header bytes against two
 * CRC bytes - which is one more reason search runs static-length.
 */
static inline uint8_t ant_frame_len_byte(uint8_t payload_len)
{
	return (uint8_t)(payload_len + ANT_FRAME_CRC_BYTES);
}

/* Geometry queries. Each returns a length, or ANT_FRAME_EINVAL for an unknown
 * configuration or a payload no configuration can express. */
int ant_frame_addr_len(enum ant_frame_cfg cfg);
int ant_frame_hdr_len(enum ant_frame_cfg cfg);
int ant_frame_body_len(enum ant_frame_cfg cfg, uint8_t payload_len);

/*
 * Bytes the CRC covers: address plus body. It is 15 for a standard frame in
 * *both* configurations - 5 + 10 and 3 + 12 - and that is the invariant worth
 * asserting in CI, because the two configurations put genuinely different
 * bytes on the hardware matcher and a future format that quietly breaks the
 * equality would otherwise announce itself on the air instead.
 */
int ant_frame_covered_len(enum ant_frame_cfg cfg, uint8_t payload_len);

/*
 * The packet format a HAL arm call needs for each configuration, for the
 * standard 8-byte payload. Static and const, because a backend may have to
 * precompile the set of formats it can express (RAIL derives frame-length
 * handling from a generated configuration).
 *
 * Tracking is length-from-body and therefore accepts any payload up to the
 * maximum; search is fixed at 12 body bytes, which is what the hardware does
 * and is an honest limitation rather than an oversight - a longer frame is not
 * receivable in search and would need its own format. Returns NULL for an
 * unknown configuration.
 */
const struct ant_pkt_format *ant_frame_format(enum ant_frame_cfg cfg);

/* ---------------------------------------------------------------------------
 * The frame, decoded and on the wire
 * ---------------------------------------------------------------------------
 */

/* A frame as the layers above think of it. */
struct ant_frame {
	struct ant_channel_id id;
	/*
	 * The length byte exactly as it appears on air, stated separately from
	 * payload_len rather than derived from it, and cross-checked on every
	 * call. That is deliberate: it is the field Spike B may reveal to be a
	 * control byte rather than a length, and keeping it explicit means the
	 * day that happens, callers that assumed otherwise fail loudly here
	 * instead of putting a plausible wrong byte on the air. Use
	 * ant_frame_make() and the question does not arise.
	 */
	uint8_t len_byte;
	uint8_t payload_len;
	uint8_t payload[ANT_FRAME_PAYLOAD_MAX];
};

/*
 * One frame split the way a radio wants it: what the hardware matches, what it
 * DMAs, and what its CRC engine produces. addr[0] is the first byte on air;
 * there is no bit- or byte-reversal in here and no register layout, exactly as
 * in struct ant_rx_filter.
 *
 * crc is the value as a number. On air it goes out most significant byte
 * first, which ant_frame_to_bytes() does and ant_frame_from_bytes() undoes.
 */
struct ant_frame_wire {
	uint8_t  addr[ANT_FRAME_ADDR_MAX];
	uint8_t  addr_len;
	uint8_t  body[ANT_FRAME_BODY_MAX];
	uint8_t  body_len;
	uint16_t crc;
};

/* Fill in a frame with a consistent length byte. Returns ANT_FRAME_OK, or
 * ANT_FRAME_EINVAL for a null argument or an over-long payload. */
int ant_frame_make(struct ant_frame *f, const struct ant_channel_id *id,
		   const uint8_t *payload, uint8_t payload_len);

/*
 * The on-air address for a channel ID in the given configuration, first byte
 * first. Returns the address length, or ANT_FRAME_EINVAL.
 *
 * In search the address stops after devnum_lo: the rest of the channel ID
 * moves into the body. That is the whole difference between the two.
 */
int ant_frame_addr(enum ant_frame_cfg cfg, const uint8_t net_addr[ANT_NET_ADDR_LEN],
		   const struct ant_channel_id *id, uint8_t *out,
		   size_t out_len);

/*
 * Encode. Fills out->addr, out->body and out->crc; out is fully overwritten on
 * success and untouched on failure.
 *
 * ANT_FRAME_ELEN if in->len_byte does not match in->payload_len - see the
 * comment on struct ant_frame.
 */
int ant_frame_encode(enum ant_frame_cfg cfg, const uint8_t net_addr[ANT_NET_ADDR_LEN],
		     const struct ant_frame *in, struct ant_frame_wire *out);

/*
 * The CRC of a wire frame: address then body, seeded with ANT_CRC_INIT. Does
 * not look at wire->crc. Returns 0 for a null or malformed argument, which is
 * indistinguishable from a legitimate zero CRC - use ant_frame_crc_ok() to ask
 * whether a frame verifies.
 */
uint16_t ant_frame_crc(const struct ant_frame_wire *w);

/* True only if the frame is well formed and its CRC matches. */
bool ant_frame_crc_ok(const struct ant_frame_wire *w);

/*
 * The CRC has already been verified by something other than this module -
 * typically a hardware CRC engine, in which case wire->crc holds nothing
 * useful because the received CRC bytes never reach the core.
 *
 * The flag is opt-*out* rather than opt-in on purpose. A caller that forgets
 * it gets a loud ANT_FRAME_ECRC on a frame that was actually fine; a caller
 * that forgot the other polarity would silently accept corruption. Only one of
 * those two failure modes is discoverable.
 */
#define ANT_FRAME_TRUSTED_CRC (1u << 0)

/*
 * Decode. out is written only on success, so a rejected frame cannot leave a
 * half-populated structure behind for a caller that ignored the return value.
 *
 * A CRC failure is ANT_FRAME_ECRC and nothing else: this module never repairs,
 * never guesses, and never reports a corrupt frame as good with a flag the
 * caller has to remember to read.
 */
int ant_frame_decode(enum ant_frame_cfg cfg, const struct ant_frame_wire *in,
		     uint32_t flags, struct ant_frame *out);

/* ---------------------------------------------------------------------------
 * The flat form
 *
 * The 17 contiguous bytes after the preamble, which is what a capture file, a
 * conformance vector, a software-CRC backend and a promiscuous sniffer all
 * want. Nothing on the nRF path uses it - there the address is in registers
 * and the CRC is in its own register - which is exactly why it is worth having
 * a second, independent expression of the same layout.
 * ---------------------------------------------------------------------------
 */

/* Write address, body and CRC (most significant byte first) into out. Returns
 * the number of bytes written, or ANT_FRAME_EINVAL / ANT_FRAME_ETRUNC if out
 * is too small. */
int ant_frame_to_bytes(const struct ant_frame_wire *w, uint8_t *out,
		       size_t out_len);

/*
 * Parse those bytes back. The frame length is taken from the on-air length
 * byte, so a buffer that is too short for what the length byte claims is
 * ANT_FRAME_ETRUNC rather than a silently short frame. Trailing bytes beyond
 * the frame are ignored; the number consumed is the return value.
 *
 * This does not verify the CRC - it only puts the received value in w->crc.
 * Pass the result to ant_frame_decode() without ANT_FRAME_TRUSTED_CRC to check
 * it, which is the path every test and every capture reader should take.
 */
int ant_frame_from_bytes(enum ant_frame_cfg cfg, const uint8_t *buf, size_t len,
			 struct ant_frame_wire *out);

#ifdef __cplusplus
}
#endif

#endif /* ANT_FRAME_H_ */
