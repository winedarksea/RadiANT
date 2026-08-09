/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_frame.h - the ANT on-air frame: CRC, address packing, encode, decode.
 *
 * Provenance: clean-room. Written from docs/ant-radio-link.md (the frame
 * layout, the CRC parameters, the two packet configurations and the two
 * address-packing rules), docs/spike-a-results.md, docs/spike-b-results.md and
 * docs/spike-b-part2-results.md - which supersedes part 1 on the control byte
 * and is where the six-field model below comes from -
 * (the measurements those rest on, 2026-08-09, nRF54L15 DK), with public
 * nRF52840/nRF54L15 product specifications behind the reasoning about *why*
 * the two configurations differ. Nothing here derives from sdk-ant, from
 * libant.a, from disassembly
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
 * Spike A on 2,164 real frames, by Spike B part 1 on 750 more and by part 2 on
 * 2,354 more):
 *
 *       1        2         2        1       1       1        8         2
 *  +--------+---------+---------+-------+-------+--------+---------+--------+
 *  |preamble| net addr| dev num | dtype | ttype |  ctrl  | payload |  CRC   |
 *  |  0x55  |  A6 C5  | lo  hi  |       |       |6 fields| d0..d7  |        |
 *  +--------+---------+---------+-------+-------+--------+---------+--------+
 *           |<----------------- 15 bytes CRC coverage ------------>|
 *           |<------------------------ 17 bytes ------------------------->|
 *
 * Byte 3 of the body is a CONTROL byte, not a length. Spike B part 1 settled
 * that: an ANT master re-broadcasts its last payload, so the same eight payload
 * bytes appear one slot apart with this byte changing 0xAA -> 0x0A. A length
 * field cannot do that. Part 2 then decomposed the byte into six independent
 * fields and killed the last of the length reading outright - see the control
 * byte section below.
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
 *   tracking/TX   address A6 C5 dnlo dnhi dtype     body [ttype][ctrl][d0..d7]
 *                 5 bytes                           10 bytes
 *   search        address A6 C5 dnlo                body [dnhi][dtype][ttype]
 *                 3 bytes                                [ctrl][d0..d7]
 *                                                   12 bytes
 *
 * Concatenated, those are byte-for-byte the same 15 bytes and therefore the
 * same CRC - which is the 15-byte coverage invariant, and is asserted in
 * radiant_core/tests/src/test_frame.c in both directions. The split matters
 * because a matched address byte never reaches RAM: in tracking the channel ID
 * is proven by the match and cannot be read back, while in search it is in the
 * buffer and can be. Search exists at all because the nRF's fixed
 * S0|LENGTH|S1 layout has no slot for a length field with three bytes ahead of
 * it, so search runs static-length; that is a backend fact, but it is the
 * reason this module has to express both layouts and convert between them.
 *
 * BOTH formats are static-length now, and that is a Spike B consequence rather
 * than a tidy-up. Parsing byte 3 as a hardware length field (nRF
 * PCNF0.LFLEN=8) reads an acknowledged frame's 0xAA as LENGTH=170, overruns
 * MAXLEN and discards it as a CRC error - a receiver that hears every
 * broadcast perfectly and silently drops every acknowledged and burst frame.
 * On transmit the same field would try to send 170 bytes. So both
 * radiant_pkt_formats below are RADIANT_LEN_FIXED, which is the nRF's PCNF0=0 /
 * STATLEN form, and byte 3 is a body byte that software writes and reads.
 *
 * Part 2 strengthened that call rather than weakening it. 0x0A's low five bits
 * read 10 and 0xA2's read 2, and both frames carry eight payload bytes: no
 * hardware length field can parse a byte that says 10 and 2 for the same
 * payload, in any configuration, on any part.
 *
 * The one byte of a search frame that is in neither place is devnum_lo: it is
 * consumed by the matcher, and the core recovers it from which filter matched
 * (see radiant_rx_event.filter_index in radiant_radio_hal.h). So a caller decoding a
 * search frame fills wire.addr from its own filter table before calling here.
 * This module never guesses it.
 *
 * ---------------------------------------------------------------------------
 * No register semantics
 * ---------------------------------------------------------------------------
 * Registers belong to a backend. This header speaks frames, channel IDs,
 * buffers and on-air byte order. radiant_addr_pack_leading() does return a 32-bit
 * word, because every planned backend wants one, but it is named for what it
 * is - the leading on-air address bytes packed in transmission order - and not
 * for the register it happens to land in on one part.
 */

#ifndef RADIANT_FRAME_H_
#define RADIANT_FRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant_core/radiant_radio_hal.h>

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
#define RADIANT_FRAME_OK        0
#define RADIANT_FRAME_EINVAL  (-1)  /* null pointer, unknown configuration, or a
				 * length no configuration can express */
#define RADIANT_FRAME_ECTRL   (-2)  /* bits 2:0 of the on-air control byte are not
				 * 010. That is the whole invariant the byte
				 * has: it held on all 750 frames of Spike B
				 * part 1 and all 2,354 of part 2, and no other
				 * bit of the byte is ever an error - the five
				 * flags above it are dispatch, not validation.
				 * See radiant_frame_msg_type() */
#define RADIANT_FRAME_ECRC    (-3)  /* the CRC did not verify. Reported, never
				 * repaired; the output is left untouched */
#define RADIANT_FRAME_ETRUNC  (-4)  /* the buffer is shorter than the frame it
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
#define RADIANT_NET_ADDR_LEN 2

#define RADIANT_NET_ADDR_ANT_PLUS_0 0xA6u  /* first byte on air */
#define RADIANT_NET_ADDR_ANT_PLUS_1 0xC5u

/* The same two bytes as an array, for passing to the functions below. */
extern const uint8_t radiant_net_addr_ant_plus[RADIANT_NET_ADDR_LEN];

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
#define RADIANT_CRC_POLY 0x1021u
#define RADIANT_CRC_INIT 0xFFFFu

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
#define RADIANT_CRC_AFTER_ANT_PLUS_NET 0x233Eu

/*
 * Run the CRC over len bytes, starting from seed.
 *
 * seed is a parameter rather than a constant precisely so the chaining above
 * is expressible. Pass RADIANT_CRC_INIT for a whole frame.
 *
 * Two ways to check a received frame, and they catch different mistakes, so
 * assert both:
 *   - compute over the 15 covered bytes and compare against the received 2, or
 *   - compute over all 17 and check the result is zero.
 * The second works because there is no final XOR and no reflection, so
 * appending a correct CRC drives the register to zero. Both held on every one
 * of Spike A's 2,164 CRC-valid frames.
 */
uint16_t radiant_crc16(uint16_t seed, const uint8_t *data, size_t len);

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
uint8_t radiant_rev8(uint8_t b);

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
 * Returns 0 for an addr_len outside 2..RADIANT_FRAME_ADDR_MAX, which is also a
 * legitimate value for an all-zero address; callers that care validate the
 * length themselves rather than testing the result.
 */
uint32_t radiant_addr_pack_leading(const uint8_t *addr, uint8_t addr_len);

/*
 * The last byte of an on-air address, bit-reversed. It is transmitted last,
 * and on the nRF24-descended parts it is the byte that distinguishes one
 * logical address from another while the leading bytes are shared - which is
 * exactly what the eight-filter wildcard sweep in radiant_search.c exploits.
 * Returns 0 for an invalid addr_len, on the same terms as above.
 */
uint8_t radiant_addr_pack_trailing(const uint8_t *addr, uint8_t addr_len);

/* ---------------------------------------------------------------------------
 * Channel ID
 * ---------------------------------------------------------------------------
 */

/* The four bytes that follow the network address are exactly ANT's channel ID,
 * which is why an on-air address match *is* a channel-ID match and no software
 * comparison is needed in the common case. */
struct radiant_channel_id {
	uint16_t device_number;
	uint8_t  device_type;   /* MSB is the pairing bit, so types are 1..127 */
	uint8_t  trans_type;
};

#define RADIANT_DEVICE_TYPE_PAIRING_BIT 0x80u
#define RADIANT_DEVICE_TYPE_MASK        0x7Fu

/* ---------------------------------------------------------------------------
 * Geometry
 * ---------------------------------------------------------------------------
 */

enum radiant_frame_cfg {
	/* 5-byte address, 2-byte header. What a tracked channel and every
	 * transmitter use. */
	RADIANT_FRAME_CFG_TRACKING = 0,
	/* 3-byte address, 4-byte header. What wildcard search uses, because
	 * three matched bytes is the shortest address the hardware will take
	 * and the channel ID then has to be readable out of RAM. */
	RADIANT_FRAME_CFG_SEARCH = 1,
	RADIANT_FRAME_CFG_COUNT
};

#define RADIANT_FRAME_ADDR_LEN_TRACKING 5u
#define RADIANT_FRAME_ADDR_LEN_SEARCH   3u
#define RADIANT_FRAME_ADDR_MAX          5u

/* Body bytes ahead of the payload: [ttype][ctrl] in tracking,
 * [dnhi][dtype][ttype][ctrl] in search. */
#define RADIANT_FRAME_HDR_LEN_TRACKING  2u
#define RADIANT_FRAME_HDR_LEN_SEARCH    4u

/*
 * Payload bytes. 8 is every frame anyone has ever measured - Spike A's 2,164,
 * Spike B part 1's 750 and part 2's 2,354 alike. Part 2 enabled advanced burst
 * a second time, sent 24-byte blocks, and watched the stack fragment them into
 * three 8-byte on-air packets; the sequence bit alternated per on-air packet,
 * not per host block. No frame with a payload other than eight bytes has ever
 * been on this bench's air.
 *
 * 24 is therefore a buffer ceiling and nothing more. It is NOT a prediction
 * about an advanced-burst frame's control byte: that prediction rested on the
 * low bits being a length, and part 2 falsified the premise, so there is no
 * encoding here to predict with.
 */
#define RADIANT_FRAME_PAYLOAD_STD 8u
#define RADIANT_FRAME_PAYLOAD_MAX 24u

/* Longest body either configuration can produce: search header plus the
 * largest payload. Stays inside the HAL's advisory RADIANT_RADIO_BODY_MAX. */
#define RADIANT_FRAME_BODY_MAX (RADIANT_FRAME_HDR_LEN_SEARCH + RADIANT_FRAME_PAYLOAD_MAX)

#define RADIANT_FRAME_CRC_BYTES 2u

/* ---------------------------------------------------------------------------
 * The control byte - byte 3 of the body, and what Spike B is about
 *
 * SIX INDEPENDENT FIELDS, not a type field and a length. Measured across four
 * runs and 2,354 CRC-valid frames on 2026-08-09 with three radios on the air at
 * once (docs/spike-b-part2-results.md,
 * archive/captures/radio/2026-08-09-spike-b2-*.log), and consistent with all
 * 750 frames of part 1:
 *
 *      bit    7      6      5      4      3     2 1 0
 *           +------+------+------+------+------+-------+
 *           | xchg | ack  | last | seq  | slot | 0 1 0 |
 *           +------+------+------+------+------+-------+
 *
 *   b7 exchange   0 = plain broadcast, 1 = part of an acknowledged exchange
 *                 (acknowledged data or burst, either direction)
 *   b6 ack        0 = this is the data packet, 1 = this is the ACKNOWLEDGEMENT
 *   b5 last       1 = last packet of the transfer; echoed by its ack
 *   b4 seq        a ONE-BIT alternating sequence number
 *   b3 slot       1 = this frame opens the channel slot `[inferred]`
 *   b2:0          always 010. Meaning unknown; it is not a length
 *
 * THE BURST SEQUENCE IS ONE BIT AND SEQUENCES 2..7 DO NOT EXIST. That is a
 * positive statement, not an absence: a 17-packet and a 51-packet burst were
 * captured end to end with the sniffer's ring-drop counter at zero throughout,
 * and bit 4 alternated 0,1,0,1,... across every on-air packet while bits 7:5
 * held still. Nineteen bursts of 1, 2, 3, 6, 9, 17, 27 and 51 packets, zero
 * frames where the sequence bit disagreed with the packet index. There is no
 * field left in the frame that could hold a number larger than one.
 *
 * THE LOW BITS ARE NOT A LENGTH, and that is now measured rather than argued.
 * 0x0A has bits 4:0 = 01010 = 10 and 0xA2 has 00010 = 2, and both carry eight
 * payload bytes. No length field reads 10 and 2 for the same length. The two
 * bits that moved are the slot bit and the sequence bit, both accounted for
 * above. Part 1's inference - that bits 4:0 remain a ShockBurst length and that
 * bits 7:5 are a three-bit type-and-sequence field - is withdrawn, not
 * requalified.
 *
 * The eleven values that have been on the air, and nothing else has:
 * ---------------------------------------------------------------------------
 */

/* Field masks. Use these; do not hand-mask this byte anywhere in the tree,
 * because hand-masking is how bit 3 gets forgotten. */
#define RADIANT_CTRL_EXCHANGE  0x80u
#define RADIANT_CTRL_ACK       0x40u
#define RADIANT_CTRL_LAST      0x20u
#define RADIANT_CTRL_SEQ       0x10u
#define RADIANT_CTRL_SLOT      0x08u
#define RADIANT_CTRL_LOW_MASK  0x07u
#define RADIANT_CTRL_LOW_VALUE 0x02u

/*
 * Slot openers - part 1's three values, and the only three a master was ever
 * seen to send. Run C is the control that names the difference: the same
 * Feather, the same stack, the same driver script and the same payload bytes,
 * with only the channel role changed, moved 0x82 -> 0x8A and 0xA2 -> 0xAA.
 */
#define RADIANT_CTRL_BROADCAST         0x0Au /* the ONLY encoding with b7 = 0 */
#define RADIANT_CTRL_BURST_OPEN_SEQ0   0x8Au /* burst packet, seq 0, not last */
#define RADIANT_CTRL_ACK_DATA_OPEN     0xAAu /* acknowledged data == a one-packet
					  * burst: seq 0, LAST. Part 1 saw this
					  * and refused to call it burst-last;
					  * it is burst-last */

/* In-slot data packets, from the acknowledged exchange's originator. */
#define RADIANT_CTRL_BURST_SEQ0        0x82u
#define RADIANT_CTRL_BURST_SEQ1        0x92u
#define RADIANT_CTRL_BURST_LAST_SEQ0   0xA2u /* also: in-slot acknowledged data */
#define RADIANT_CTRL_BURST_LAST_SEQ1   0xB2u

/* In-slot acknowledgements, from the receiver of the exchange. */
#define RADIANT_CTRL_ACK_SEQ0          0xC2u /* acks a non-final packet */
#define RADIANT_CTRL_ACK_SEQ1          0xD2u
#define RADIANT_CTRL_ACK_LAST_SEQ0     0xE2u /* acks the final packet - transfer
					  * complete */
#define RADIANT_CTRL_ACK_LAST_SEQ1     0xF2u

/*
 * ---------------------------------------------------------------------------
 * THE REMAINING GAPS, stated so that nothing is built over them by accident
 * ---------------------------------------------------------------------------
 * A field model can express 256 values. Eleven were measured. Every function
 * below encodes from the fields and then checks the result against the
 * measured eleven, because the difference between "the model permits it" and
 * "the air has carried it" is the whole value of two spikes.
 *
 * `[inferred]`, and named so it is not mistaken for a finding:
 *
 *   - BIT 3 AS "SLOT OPENER". What is measured is only that it is not
 *     master-versus-slave: the master's own broadcast carries it set and the
 *     master's acknowledgement 1.6 ms later in the same slot carries it clear.
 *     Falsify it by getting a MASTER to send a multi-packet burst to a real
 *     slave - under this reading packet 0 carries bit 3 and packets 1..N do
 *     not. That needs a scriptable ANT master, which sim/ is not.
 *   - THE READING OF BIT 4 IN AN ACKNOWLEDGEMENT as "the sequence bit I expect
 *     next". The arithmetic is measured on 168 pairs; the reading is not.
 *
 * NEVER OBSERVED, so nothing here encodes it:
 *
 *   - any payload other than 8 bytes, so bits 2:0 = 010 has no measured
 *     meaning, only a disproved one;
 *   - bit 5 set on a broadcast, and indeed any b7 = 0 value except 0x0A: bits
 *     7:5 never took 001, 010 or 011;
 *   - an acknowledgement of a slot-opening data packet, which is why
 *     radiant_ctrl_reply_for() below refuses to invent one;
 *   - what the DATA SENDER does when an acknowledgement goes missing. The
 *     receiver's behaviour is measured - it retransmits its acknowledgement 21
 *     times at 3143 us and gives up - but no acknowledgement went missing in a
 *     completed transfer.
 *
 * RADIANT_FRAME_LEN_ADV_BURST used to live here, predicting 0x1A for a 24-byte
 * advanced-burst frame, and a later pass restated it as 0x9A. Both are gone,
 * not requalified: the restatement assumed bits 4:0 were a length, and that
 * premise is falsified. The only 0x1A ever seen on this bench appeared twice
 * as a CRC-FAILED frame - one bit away from 0x0A - which is a bit error, not
 * an advanced burst. A wrong constant left in a header with a comment
 * explaining it is wrong is how it gets used.
 */

/*
 * The five flags, unpacked. This is what callers work with; the byte itself is
 * only for the air and for a log line.
 */
struct radiant_ctrl_fields {
	bool exchange;    /* bit 7 */
	bool ack;         /* bit 6 */
	bool last;        /* bit 5 */
	bool seq;         /* bit 4 - one bit, and there is no second one */
	bool slot_opener; /* bit 3 */
};

/*
 * Which message a control byte describes, decoded FROM THE FIELDS rather than
 * matched against a list of literals.
 *
 * RADIANT_MSG_ACKNOWLEDGED does not appear, and its absence is the finding: on air,
 * acknowledged data is byte-for-byte a one-packet burst - "sequence 0, last" -
 * so a receiver dispatching on this byte cannot tell them apart and must not
 * pretend to. The distinction between them is a serial-layer distinction only.
 * radiant_ack.c and radiant_burst.c should therefore share one encoder.
 */
enum radiant_msg_type {
	/* b2:0 != 010, or a flag combination never measured: b7 = 0 with any
	 * of b6/b5 set. Never an error here - the frame layer reports it and
	 * the layer above decides. */
	RADIANT_MSG_UNKNOWN = 0,
	RADIANT_MSG_BROADCAST,     /* b7=0                     */
	RADIANT_MSG_BURST_DATA,    /* b7=1 b6=0 b5=0           */
	RADIANT_MSG_BURST_LAST,    /* b7=1 b6=0 b5=1 - also what acknowledged data
				* is on the air */
	RADIANT_MSG_BURST_ACK,     /* b7=1 b6=1 b5=0           */
	RADIANT_MSG_TRANSFER_ACK,  /* b7=1 b6=1 b5=1 - the transfer is complete */
};

/* --- Field accessors. Named so that every read of this byte is greppable. -- */

/* The one invariant the byte has. Everything else is dispatch. */
static inline bool radiant_ctrl_low_ok(uint8_t ctrl)
{
	return (uint8_t)(ctrl & RADIANT_CTRL_LOW_MASK) == RADIANT_CTRL_LOW_VALUE;
}

static inline bool radiant_ctrl_is_exchange(uint8_t ctrl)
{
	return (ctrl & RADIANT_CTRL_EXCHANGE) != 0u;
}

static inline bool radiant_ctrl_is_ack(uint8_t ctrl)
{
	return (ctrl & RADIANT_CTRL_ACK) != 0u;
}

static inline bool radiant_ctrl_is_last(uint8_t ctrl)
{
	return (ctrl & RADIANT_CTRL_LAST) != 0u;
}

/* The sequence bit, as 0 or 1. It is one bit; there is no wider form. */
static inline uint8_t radiant_ctrl_seq(uint8_t ctrl)
{
	return (uint8_t)((ctrl & RADIANT_CTRL_SEQ) != 0u);
}

static inline bool radiant_ctrl_is_slot_opener(uint8_t ctrl)
{
	return (ctrl & RADIANT_CTRL_SLOT) != 0u;
}

/* Pack the five flags into the on-air byte, with 010 in bits 2:0. This always
 * produces a syntactically legal control byte; whether it is one that has ever
 * been transmitted is radiant_ctrl_observed()'s question. */
uint8_t radiant_ctrl_encode(const struct radiant_ctrl_fields *f);

/* Unpack. RADIANT_FRAME_ECTRL, and out untouched, if bits 2:0 are not 010. */
int radiant_ctrl_decode(uint8_t ctrl, struct radiant_ctrl_fields *out);

/*
 * True only for the eleven values measured on air. This is the guard that keeps
 * the field model from quietly inventing the other 245: radiant_frame_make()
 * refuses anything it rejects, while radiant_frame_encode() still carries any
 * low-bits-legal byte a caller sets by hand, so a bench experiment can put a
 * candidate encoding on the air without editing this module.
 */
bool radiant_ctrl_observed(uint8_t ctrl);

/*
 * The acknowledgement a receiver puts on the air for a data packet: bit 6 set,
 * bit 5 echoed, bit 4 complemented, everything else unchanged. Measured on 168
 * adjacent CRC-valid data/acknowledgement pairs across runs 0, A and B with no
 * exceptions - 82 -> D2, 92 -> C2, A2 -> F2, B2 -> E2.
 *
 * Returns 0 - never a legal control byte, since bits 2:0 must be 010 - for
 * anything else, INCLUDING the slot openers 0x8A and 0xAA. No acknowledgement
 * of a slot-opening data packet has ever been captured, so there is no measured
 * answer for what its bit 3 would be, and this function does not guess one.
 */
uint8_t radiant_ctrl_reply_for(uint8_t data_ctrl);

/* Which message a control byte says this is. Dispatch, never validation: an
 * unmeasured combination is RADIANT_MSG_UNKNOWN and the frame still decodes. */
enum radiant_msg_type radiant_frame_msg_type(uint8_t ctrl_byte);

/* Geometry queries. Each returns a length, or RADIANT_FRAME_EINVAL for an unknown
 * configuration or a payload no configuration can express. */
int radiant_frame_addr_len(enum radiant_frame_cfg cfg);
int radiant_frame_hdr_len(enum radiant_frame_cfg cfg);
int radiant_frame_body_len(enum radiant_frame_cfg cfg, uint8_t payload_len);

/*
 * Bytes the CRC covers: address plus body. It is 15 for a standard frame in
 * *both* configurations - 5 + 10 and 3 + 12 - and that is the invariant worth
 * asserting in CI, because the two configurations put genuinely different
 * bytes on the hardware matcher and a future format that quietly breaks the
 * equality would otherwise announce itself on the air instead.
 */
int radiant_frame_covered_len(enum radiant_frame_cfg cfg, uint8_t payload_len);

/*
 * The packet format a HAL arm call needs for each configuration, for the
 * standard 8-byte payload. Static and const, because a backend may have to
 * precompile the set of formats it can express (RAIL derives frame-length
 * handling from a generated configuration).
 *
 * Both are RADIANT_LEN_FIXED - tracking at 10 body bytes, search at 12 - and that
 * is the Spike B consequence, not a simplification. A length-from-body format
 * maps onto nRF PCNF0.LFLEN=8, which reads an acknowledged frame's 0xAA as a
 * 170-byte length and discards it; the same field on transmit would try to
 * send 170 bytes. Part 2 closed the argument for good: 0x0A's low five bits
 * read 10 and 0xA2's read 2 for the same 8-byte payload, so there is no length
 * field to parse in any configuration. Fixed length is the PCNF0=0 / STATLEN
 * form, it was measured byte-identical on air to the CRCINC form, and it puts
 * byte 3 where software can both read it and choose it - which is exactly what
 * radiant_ack.c and radiant_burst.c need.
 *
 * The cost is honest and is the same one search already carried: a payload
 * other than 8 bytes is not receivable under either format and would need one
 * of its own. Nothing has ever put such a frame on the air. Returns NULL for
 * an unknown configuration.
 */
const struct radiant_pkt_format *radiant_frame_format(enum radiant_frame_cfg cfg);

/* ---------------------------------------------------------------------------
 * The frame, decoded and on the wire
 * ---------------------------------------------------------------------------
 */

/* A frame as the layers above think of it. */
struct radiant_frame {
	struct radiant_channel_id id;
	/*
	 * Byte 3 exactly as it appears on air.
	 *
	 * It is a stated field, not derived from payload_len, and the two
	 * writebacks that followed Spike B are the return on that. Part 1
	 * renamed it from len_byte and broke every caller in one edit; part 2
	 * then showed that NONE of its bits depend on the payload length, so
	 * there was never anything here to derive.
	 *
	 * The only cross-check is bits 2:0 == 010. Use the radiant_ctrl_* accessors
	 * to read the five flags above it; this module carries them and names
	 * them, and interprets none of them.
	 */
	uint8_t ctrl_byte;
	uint8_t payload_len;
	uint8_t payload[RADIANT_FRAME_PAYLOAD_MAX];
};

/*
 * One frame split the way a radio wants it: what the hardware matches, what it
 * DMAs, and what its CRC engine produces. addr[0] is the first byte on air;
 * there is no bit- or byte-reversal in here and no register layout, exactly as
 * in struct radiant_rx_filter.
 *
 * crc is the value as a number. On air it goes out most significant byte
 * first, which radiant_frame_to_bytes() does and radiant_frame_from_bytes() undoes.
 */
struct radiant_frame_wire {
	uint8_t  addr[RADIANT_FRAME_ADDR_MAX];
	uint8_t  addr_len;
	uint8_t  body[RADIANT_FRAME_BODY_MAX];
	uint8_t  body_len;
	uint16_t crc;
};

/*
 * Fill in a frame from the five control-byte fields. Returns RADIANT_FRAME_OK, or
 * RADIANT_FRAME_EINVAL for a null argument, an over-long payload, or a field
 * combination that has never been on the air (radiant_ctrl_observed()).
 *
 * The fields are an argument rather than a default because there is no safe
 * default: a caller that meant "acknowledged data" and got a broadcast would
 * set no trainer resistance and report no error, and a caller that forgot the
 * slot bit would put an in-slot byte in a slot-opening frame.
 *
 * Refusing an unmeasured combination here, while radiant_frame_encode() carries
 * one set by hand, is deliberate. A field model can name 256 bytes; eleven have
 * been transmitted. This is where the difference is enforced.
 */
int radiant_frame_make(struct radiant_frame *f, const struct radiant_channel_id *id,
		   const struct radiant_ctrl_fields *ctrl, const uint8_t *payload,
		   uint8_t payload_len);

/*
 * The on-air address for a channel ID in the given configuration, first byte
 * first. Returns the address length, or RADIANT_FRAME_EINVAL.
 *
 * In search the address stops after devnum_lo: the rest of the channel ID
 * moves into the body. That is the whole difference between the two.
 */
int radiant_frame_addr(enum radiant_frame_cfg cfg, const uint8_t net_addr[RADIANT_NET_ADDR_LEN],
		   const struct radiant_channel_id *id, uint8_t *out,
		   size_t out_len);

/*
 * Encode. Fills out->addr, out->body and out->crc; out is fully overwritten on
 * success and untouched on failure.
 *
 * RADIANT_FRAME_ECTRL if bits 2:0 of in->ctrl_byte are not 010. The five flags
 * above them are copied to the air untouched, including combinations nothing
 * has ever measured - see the comment on struct radiant_frame.
 */
int radiant_frame_encode(enum radiant_frame_cfg cfg, const uint8_t net_addr[RADIANT_NET_ADDR_LEN],
		     const struct radiant_frame *in, struct radiant_frame_wire *out);

/*
 * The CRC of a wire frame: address then body, seeded with RADIANT_CRC_INIT. Does
 * not look at wire->crc. Returns 0 for a null or malformed argument, which is
 * indistinguishable from a legitimate zero CRC - use radiant_frame_crc_ok() to ask
 * whether a frame verifies.
 */
uint16_t radiant_frame_crc(const struct radiant_frame_wire *w);

/* True only if the frame is well formed and its CRC matches. */
bool radiant_frame_crc_ok(const struct radiant_frame_wire *w);

/*
 * The CRC has already been verified by something other than this module -
 * typically a hardware CRC engine, in which case wire->crc holds nothing
 * useful because the received CRC bytes never reach the core.
 *
 * The flag is opt-*out* rather than opt-in on purpose. A caller that forgets
 * it gets a loud RADIANT_FRAME_ECRC on a frame that was actually fine; a caller
 * that forgot the other polarity would silently accept corruption. Only one of
 * those two failure modes is discoverable.
 */
#define RADIANT_FRAME_TRUSTED_CRC (1u << 0)

/*
 * Decode. out is written only on success, so a rejected frame cannot leave a
 * half-populated structure behind for a caller that ignored the return value.
 *
 * A CRC failure is RADIANT_FRAME_ECRC and nothing else: this module never repairs,
 * never guesses, and never reports a corrupt frame as good with a flag the
 * caller has to remember to read.
 *
 * Decode DISPATCHES on the control byte rather than validating it against
 * 0x0A. A tracking channel sees eleven values today and may one day see more;
 * a receiver that rejected everything but broadcast would drop exactly the
 * acknowledged frames trainer control is made of. out->ctrl_byte holds
 * whatever was on the air and radiant_frame_msg_type() names it, or does not.
 *
 * The one thing that IS validated is bits 2:0 == 010, which is RADIANT_FRAME_ECTRL.
 * The version of this module written against part 1 cross-checked bits 4:0
 * against the payload length instead, and that rejected every valid slave
 * frame: 0xA2's low five bits are 00010 = 2, and no length check expecting 10
 * accepts it. Bits 4:0 are the slot bit, the sequence bit and 010 - three
 * things, none of them a length.
 */
int radiant_frame_decode(enum radiant_frame_cfg cfg, const struct radiant_frame_wire *in,
		     uint32_t flags, struct radiant_frame *out);

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
 * the number of bytes written, or RADIANT_FRAME_EINVAL / RADIANT_FRAME_ETRUNC if out
 * is too small. */
int radiant_frame_to_bytes(const struct radiant_frame_wire *w, uint8_t *out,
		       size_t out_len);

/*
 * Parse those bytes back.
 *
 * THE FRAME LENGTH COMES FROM cfg, NOT FROM THE CONTROL BYTE. There is no
 * length anywhere in an ANT frame: both configurations are static, at the
 * standard 8-byte payload, which is the only payload any ANT frame on this
 * bench has ever carried across three spikes and 5,268 CRC-valid frames. So a
 * frame is 17 bytes and a buffer shorter than that is RADIANT_FRAME_ETRUNC.
 *
 * Earlier versions of this function took the length from bits 4:0 of the
 * control byte. That is the software half of the PCNF0.LFLEN=8 mistake wearing
 * a mask: for a broadcast the five bits happen to read 10 and it works, and for
 * a slave's 0xA2 they read 2 and every valid frame is rejected. Bits 2:0 are
 * checked, and only bits 2:0; RADIANT_FRAME_ECTRL if they are not 010.
 *
 * Trailing bytes beyond the frame are ignored; the number consumed is the
 * return value.
 *
 * This does not verify the CRC - it only puts the received value in w->crc.
 * Pass the result to radiant_frame_decode() without RADIANT_FRAME_TRUSTED_CRC to check
 * it, which is the path every test and every capture reader should take.
 */
int radiant_frame_from_bytes(enum radiant_frame_cfg cfg, const uint8_t *buf, size_t len,
			 struct radiant_frame_wire *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_FRAME_H_ */
