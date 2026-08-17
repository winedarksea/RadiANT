/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_frame.h - the ANT on-air frame: CRC, address packing, encode, decode.
 *
 * Provenance: clean-room, from docs/ant-radio-link.md (frame layout, CRC
 * parameters, the two packet configurations, address-packing rules),
 * docs/spike-a-results.md, docs/spike-b-results.md and
 * docs/spike-b-part2-results.md (supersedes part 1 on the control byte; the
 * six-field model below), with public nRF52840/nRF54L15 product
 * specifications behind the reasoning about why the two configurations
 * differ. See docs/decisions/0002-clean-room-policy.md.
 *
 * The layer with no radio in it. Everything here is a pure function of
 * bytes: given a channel ID and payload, what goes on the air; given what
 * came off the air, what channel ID and payload that was; and the CRC that
 * decides whether the second question was worth asking. The only module
 * fully testable in CI with no board.
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
 * that: an ANT master re-broadcasts its last payload, so the same eight
 * payload bytes appear one slot apart with this byte changing 0xAA -> 0x0A,
 * which a length field cannot do. Part 2 decomposed it into six independent
 * fields and killed the length reading outright - see below.
 *
 * The preamble is a PHY property (the radio derives it from the first
 * address bit, no software involved) and not this module's business;
 * everything from the network address onward is.
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
 * same CRC (the 15-byte coverage invariant, asserted both directions in
 * radiant/tests/src/test_frame.c). The split matters because a matched
 * address byte never reaches RAM: in tracking the channel ID is proven by
 * the match, in search it's in the buffer. Search exists because the nRF's
 * fixed S0|LENGTH|S1 layout has no slot for a length field ahead of three
 * address bytes, so it runs static-length - a backend fact that's still the
 * reason this module has to express both layouts and convert between them.
 *
 * BOTH formats are static-length now, a Spike B consequence, not a tidy-up.
 * Parsing byte 3 as a hardware length field (nRF PCNF0.LFLEN=8) reads an
 * acknowledged frame's 0xAA as LENGTH=170, overruns MAXLEN, and silently
 * drops every acknowledged and burst frame; transmit would try to send
 * 170 bytes. So both radiant_pkt_formats below are RADIANT_LEN_FIXED
 * (PCNF0=0/STATLEN), byte 3 a plain body byte software writes and reads.
 * Part 2 confirmed this further: 0x0A's low five bits read 10, 0xA2's read
 * 2, same 8-byte payload - no length field can parse both.
 *
 * The one byte of a search frame in neither place is devnum_lo, consumed by
 * the matcher; the core recovers it from which filter matched
 * (radiant_rx_event.filter_index). A caller decoding search fills
 * wire.addr from its own filter table - this module never guesses it.
 *
 * ---------------------------------------------------------------------------
 * No register semantics
 * ---------------------------------------------------------------------------
 * Registers belong to a backend. This header speaks frames, channel IDs,
 * buffers and on-air byte order. radiant_addr_pack_leading() returns a
 * 32-bit word because every planned backend wants one, but it's named for
 * what it is - leading on-air address bytes packed in transmission order -
 * not for the register it happens to land in on one part.
 */

#ifndef RADIANT_FRAME_H_
#define RADIANT_FRAME_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant/radiant_radio_hal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Return codes
 *
 * Distinct from the HAL's and from each other, since the malformed-input
 * tests need more than "it failed" to assert on. Negative, same reason as
 * the HAL's: a call returns either an error or a small non-negative length.
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
 * Two bytes, the only key-dependent thing in the frame. ANT+ network key
 * B9 A5 21 FB BD 72 C3 45 corresponds to A6 C5; the key->address function is
 * unknown outside Garmin, not needed, and not to be fitted from samples
 * (docs/ant-radio-link.md). ant_net.c holds the one-entry table; every
 * function here takes the address as an argument rather than assuming ANT+.
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
 * Width 16, polynomial 0x1021 (normal/unreflected), init 0xFFFF, no
 * reflection, no final XOR. Covers the on-air address bytes as well as the
 * body (15 bytes standard) - a property most sync-word matchers lack, and
 * what decides whether a given radio can do this in hardware at all.
 * ---------------------------------------------------------------------------
 */
#define RADIANT_CRC_POLY 0x1021u
#define RADIANT_CRC_INIT 0xFFFFu

/*
 * The state of the CRC register after it has consumed the ANT+ network
 * address A6 C5 from init 0xFFFF.
 *
 * Not a curiosity: CCITT-FALSE chains, so a radio whose CRC engine can't
 * cover its own sync word can still produce ANT's exact CRC by folding this
 * constant in as the initial value and covering only the remaining 13
 * bytes - what makes hardware CRC reachable on EFR32/RAIL (docs/backends.md).
 * Holds only while the covered prefix is constant; a device-number-bearing
 * sync word can't be folded this way.
 */
#define RADIANT_CRC_AFTER_ANT_PLUS_NET 0x233Eu

/*
 * Run the CRC over len bytes, starting from seed. seed is a parameter (not
 * a constant) so the chaining above is expressible; pass RADIANT_CRC_INIT
 * for a whole frame.
 *
 * Two ways to check a received frame, catching different mistakes - assert
 * both: compute over the 15 covered bytes and compare to the received 2, or
 * compute over all 17 and check for zero (works because no final XOR/
 * reflection means a correct CRC drives the register to zero).
 */
uint16_t radiant_crc16(uint16_t seed, const uint8_t *data, size_t len);

/* ---------------------------------------------------------------------------
 * Bit order, and the address packing every backend needs
 *
 * Address bits go out LSB-first regardless of the frame's other endianness.
 * Spike A settled this: bit-reversed packing caught every frame sent, all
 * seven other permutations of bit/byte order and prefix position heard
 * literally nothing. A wrong answer here produces silence, not a weak link -
 * worth knowing when a backend port comes up dead.
 *
 * Kept here rather than in a backend since they're pure functions of the
 * on-air address every backend needs, and two implementations is one too many.
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
 * Two rules folded into one expression: the first byte on air sits in the
 * lowest occupied byte (each byte bit-reversed), and with fewer than four
 * leading bytes the used bytes are the HIGH ones - a short address is
 * truncated from the least-significant byte, so this ordering keeps exactly
 * the bytes that must survive. Collapses to the four-byte case at
 * n_leading == 4.
 *
 * The naive reading (keep the low bytes) is not merely suboptimal: Spike A
 * measured it producing 9-20 address matches per 60s with ZERO CRC-valid
 * frames at -54..-101 dBm, against 24 CRC-valid frames at -17 dBm for the
 * correct packing - a matcher firing on noise, ~70 dB apart.
 *
 * Returns 0 for an addr_len outside 2..RADIANT_FRAME_ADDR_MAX, which is also
 * a legitimate all-zero-address value; callers validate the length
 * themselves rather than testing the result.
 */
uint32_t radiant_addr_pack_leading(const uint8_t *addr, uint8_t addr_len);

/* The last byte of an on-air address, bit-reversed. Transmitted last; on the
 * nRF24-descended parts it's the byte distinguishing one logical address
 * from another while leading bytes are shared, which the eight-filter
 * wildcard sweep in radiant_search.c exploits. Returns 0 for an invalid
 * addr_len, same terms as above. */
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
	/*
	 * 4-byte address, 4-byte header, LENGTH-FROM-BODY, on the coded PHY.
	 * The only configuration here that is not ANT and the only one that is
	 * not eight payload bytes. See "The long-range configuration" below;
	 * ADR 0007 is the decision.
	 */
	RADIANT_FRAME_CFG_LR = 2,
	RADIANT_FRAME_CFG_COUNT
};

#define RADIANT_FRAME_ADDR_LEN_TRACKING 5u
#define RADIANT_FRAME_ADDR_LEN_SEARCH   3u
#define RADIANT_FRAME_ADDR_LEN_LR       4u
#define RADIANT_FRAME_ADDR_MAX          5u

/* Body bytes ahead of the payload: [ttype][ctrl] in tracking,
 * [dnhi][dtype][ttype][ctrl] in search, [len][dtype][ttype][ctrl] on
 * long range. */
#define RADIANT_FRAME_HDR_LEN_TRACKING  2u
#define RADIANT_FRAME_HDR_LEN_SEARCH    4u
#define RADIANT_FRAME_HDR_LEN_LR        4u

/*
 * Payload bytes. 8 is every frame ever measured (Spike A's 2,164, Spike B
 * part 1's 750, part 2's 2,354). Part 2's advanced burst sent 24-byte
 * blocks but the stack fragmented them into three 8-byte on-air packets
 * (sequence bit alternating per packet, not per block); no other payload
 * size has ever been on this bench's air.
 *
 * 24 is therefore only a buffer ceiling, not a prediction about an
 * advanced-burst control byte - that prediction rested on the low bits
 * being a length, which part 2 falsified.
 */
#define RADIANT_FRAME_PAYLOAD_STD 8u
#define RADIANT_FRAME_PAYLOAD_MAX 24u

/* Longest body either ANT configuration can produce: search header plus the
 * largest payload. Stays inside the HAL's advisory RADIANT_RADIO_BODY_MAX. */
#define RADIANT_FRAME_BODY_MAX (RADIANT_FRAME_HDR_LEN_SEARCH + RADIANT_FRAME_PAYLOAD_MAX)

#define RADIANT_FRAME_CRC_BYTES 2u

/* ---------------------------------------------------------------------------
 * The long-range configuration - ADR 0007
 *
 * The first format here with a real length field, and the first that is not
 * ANT - both facts are the same decision.
 *
 * A length is allowed here despite ADR 0005 Axis 3 withdrawing one, because
 * Axis 3 asked whether a length could be INFERRED from an ANT frame (no:
 * byte 3 reads 10 on a broadcast and 2 on an in-slot frame for the same
 * eight payload bytes). This length byte is not inferred - it's written by
 * this project, at an offset this project chose, on a PHY a stock ANT
 * receiver cannot demodulate at all. The isolation is PHYSICAL: a stock
 * receiver on A6 C5 at 1M doesn't fail to parse a coded frame, it doesn't
 * see one - which is why ADR 0007's rule is "length extension is permitted
 * exactly where the PHY already makes us invisible", not a general licence.
 *
 *   address 4 bytes  [A6 C5 dnlo dnhi]        - BALEN=3 plus one prefix byte
 *   body           [len][dtype][ttype][ctrl][payload ...]
 *
 * Four address bytes, not five, and device type moves into the body. Four
 * is BALEN=3 plus a prefix on the nRF matcher - one more than search's
 * three, so fewer false triggers (search's matcher sees 19-27 noise
 * matches per 15s) - and it's what the coded PHY's fixed 32-bit access
 * address is anyway. Device type can't stay in a four-byte address, so it
 * moves to the body front, same move search makes with its shorter address.
 *
 * The length byte counts everything after itself, the nRF RADIO's own
 * convention (PCNF0.LFLEN=8, S0LEN=S1LEN=0), needing no backend
 * translation: body_len = body[0] + 1, which is len_bias's first use.
 * ---------------------------------------------------------------------------
 */

/* body[0] is the length; body_len = body[0] + RADIANT_FRAME_LR_LEN_BIAS. */
#define RADIANT_FRAME_LR_LEN_OFFSET 0u
#define RADIANT_FRAME_LR_LEN_BIAS   1

/*
 * The longest payload a long-range frame may carry - a BUFFER bound, not a
 * licence to fill it.
 *
 * At S=8, a full 40-byte body is ~3.2 ms (1.3% duty at 4 Hz, 0.16% at
 * 0.5 Hz). What actually limits battery spend is the duty rule - a frame
 * must stay under 25% of its channel period - enforced by
 * profile_sched_duty_check() at the encoder, not by any constant here. That
 * binds only a node with both a long frame and a fast period: costs a
 * 0.5 Hz asset tag nothing, refuses a 4 Hz node wanting a 40-byte frame.
 */
#define RADIANT_FRAME_PAYLOAD_LR_MAX 36u

/*
 * The long-range format's max_body_len, and the number RADIANT_RADIO_BODY_MAX
 * was raised from 32 to accommodate.
 */
#define RADIANT_FRAME_LR_BODY_MAX \
	(RADIANT_FRAME_HDR_LEN_LR + RADIANT_FRAME_PAYLOAD_LR_MAX)

/*
 * Compose a long-range body. Returns the body length written, or
 * RADIANT_FRAME_EINVAL / RADIANT_FRAME_ETRUNC.
 *
 * Deliberately not radiant_frame_encode(): struct radiant_frame's 24-byte
 * payload buffer and eleven-value control byte are ANT facts, and routing a
 * 36-byte RadiANT-authored payload through it would mean widening an ANT
 * structure and loosening checks that keep other control bytes off the air,
 * just so two unrelated formats could share one function. These take the
 * channel ID and bytes directly instead.
 *
 * Control byte passed through with only bits 2:0 checked, same terms as
 * radiant_frame_encode(): this is a RadiANT-authored format, the caller
 * above decides what the byte means.
 */
int radiant_frame_lr_body(const struct radiant_channel_id *id, uint8_t ctrl_byte,
			  const uint8_t *payload, uint8_t payload_len,
			  uint8_t *out, size_t out_len);

/*
 * The inverse, over a body as it came off the air. `payload` points INTO
 * `body` (no copy), valid as long as body is.
 *
 * The length byte is checked against the delivered body_len rather than
 * trusted - a mis-programmed MAXLEN or a corrupted length byte must not
 * become a read past a DMA buffer. RADIANT_FRAME_ETRUNC when they disagree.
 */
int radiant_frame_lr_parse(const uint8_t *body, uint8_t body_len,
			   struct radiant_channel_id *id, uint8_t *ctrl_byte,
			   const uint8_t **payload, uint8_t *payload_len);

/*
 * Airtime of one frame in this configuration, in microseconds, for a given
 * payload length. Exact integer arithmetic (see radiant_frame.c for the
 * derivation and FEC-block table). Lives here since it's a property of the
 * format/PHY, both stated here, and neither the duty bound nor the
 * scheduler should need to know the backend. Returns 0 for an unknown
 * configuration or over-long payload.
 */
uint32_t radiant_frame_airtime_us(enum radiant_frame_cfg cfg, uint8_t payload_len);

/* ---------------------------------------------------------------------------
 * The control byte - byte 3 of the body, and what Spike B is about
 *
 * SIX INDEPENDENT FIELDS, not a type field and a length. Measured across
 * four runs and 2,354 CRC-valid frames on 2026-08-09 with three radios on
 * the air at once (docs/spike-b-part2-results.md), consistent with all 750
 * frames of part 1:
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
 * THE BURST SEQUENCE IS ONE BIT, SEQUENCES 2..7 DO NOT EXIST: a 17-packet
 * and a 51-packet burst captured end to end (sniffer ring-drop counter zero
 * throughout) showed bit 4 alternating 0,1,0,1... on every on-air packet
 * while bits 7:6 held still, across nineteen bursts of 1-51 packets with
 * zero disagreements against packet index. No field left could hold a
 * number larger than one.
 *
 * Bit 5 moves once, on the final packet of every burst - "last" doing
 * exactly what it says (82 92 82 92 ... then A2 on the last packet of a
 * 17-packet burst, archive/captures/radio/2026-08-09-spike-b2-runA-burst-seq.log).
 *
 * THE LOW BITS ARE NOT A LENGTH: 0x0A's bits 4:0 read 10, 0xA2's read 2, for
 * the same eight-byte payload - no length field reads both. The two bits
 * that moved are the slot bit and the sequence bit, both accounted for
 * above; part 1's inference (ShockBurst length in 4:0, a 3-bit type/seq
 * field in 7:5) is withdrawn, not requalified.
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

/* Slot openers - part 1's three values, the only ones a master was ever
 * seen to send. Run C isolated the difference: same hardware, stack, script
 * and payload, only the channel role changed, and 0x82 -> 0x8A, 0xA2 -> 0xAA. */
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
 * A field model can express 256 values, eleven were measured. Every
 * function below encodes from the fields and checks against the measured
 * eleven.
 *
 * `[inferred]`, not a finding:
 *   - BIT 3 AS "SLOT OPENER": only measured that it's not master-vs-slave
 *     (master's broadcast carries it set, its ack 1.6ms later carries it
 *     clear). Falsifiable by a scriptable ANT master, which sim/ is not.
 *   - BIT 4 IN AN ACKNOWLEDGEMENT as "the sequence bit I expect next" -
 *     arithmetic measured on 165 pairs, the reading is not.
 *   - BIT 3 OF AN ACKNOWLEDGEMENT TO A SLOT OPENER, taken as clear, so
 *     0x8A -> 0xD2 and 0xAA -> 0xF2. Added 2026-08-17; until then
 *     radiant_ctrl_reply_for() returned 0 for both, which did not leave a
 *     gap open, it closed the path: no RadiANT acknowledger would answer a
 *     master-originated acknowledged transfer at all. See the table note in
 *     radiant_frame.c for the three lines of evidence and how to falsify it.
 *
 * NEVER OBSERVED, so nothing here encodes it: any payload other than 8
 * bytes; bit 5 set on a broadcast (or any b7=0 value except 0x0A); what a
 * data sender does when its ack goes missing (the receiver retransmits 21
 * times at 3143us and gives up, but no ack was ever actually lost in a
 * completed transfer).
 *
 * RADIANT_FRAME_LEN_ADV_BURST used to predict 0x1A (then 0x9A) for a
 * 24-byte advanced-burst frame; both premises assumed bits 4:0 were a
 * length, which is falsified. The only 0x1A ever seen was twice as a
 * CRC-FAILED frame (one bit from 0x0A) - a bit error, not a real value.
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
 * Which message a control byte describes, decoded FROM THE FIELDS rather
 * than matched against literals.
 *
 * RADIANT_MSG_ACKNOWLEDGED does not appear - on air, acknowledged data is
 * byte-for-byte a one-packet burst ("sequence 0, last"), so a receiver
 * can't and shouldn't tell them apart; the distinction is serial-layer
 * only, which is why radiant_ack.c and radiant_burst.c share one encoder.
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

/* True only for the eleven values measured on air - the guard that keeps
 * the field model from inventing the other 245. radiant_frame_make() refuses
 * anything it rejects; radiant_frame_encode() still carries any
 * low-bits-legal byte set by hand, so a bench experiment can try a
 * candidate encoding without editing this module. */
bool radiant_ctrl_observed(uint8_t ctrl);

/*
 * The acknowledgement a receiver puts on the air for a data packet: bit 6
 * set, bit 5 echoed, bit 4 complemented, everything else unchanged.
 * Measured on 165 adjacent CRC-valid data/ack pairs across runs 0, A and B,
 * no exceptions - 82->D2, 92->C2, A2->F2, B2->E2. (165, not 168: three
 * pairs had a CRC-failed ack and don't count as CRC-valid.)
 *
 * The slot openers 0x8A and 0xAA answer 0xD2 and 0xF2 - the same replies as
 * their in-slot twins 0x82 and 0xA2, i.e. bit 3 clear. `[inferred]`: no ack
 * of a slot-opening data packet was ever captured, so this is the reading
 * that fits (no ack with bit 3 set exists in any capture, and an ack is sent
 * inside the slot the data opened, so it cannot be an opener). It is the
 * difference between a master-originated acknowledged transfer working and
 * never completing; radiant_frame.c holds the full argument.
 *
 * Returns 0 - never a legal control byte - for anything else.
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

/* Bytes the CRC covers: address plus body. 15 for a standard frame in
 * *both* configurations (5+10 and 3+12) - worth asserting in CI so a future
 * format that breaks the equality doesn't announce itself on the air instead. */
int radiant_frame_covered_len(enum radiant_frame_cfg cfg, uint8_t payload_len);

/*
 * The packet format a HAL arm call needs for each configuration, for the
 * standard 8-byte payload. Static/const since a backend may have to
 * precompile the formats it can express (RAIL).
 *
 * Both are RADIANT_LEN_FIXED - tracking 10 body bytes, search 12 - a Spike
 * B consequence: length-from-body (nRF PCNF0.LFLEN=8) reads an
 * acknowledged frame's 0xAA as a 170-byte length; 0x0A/0xA2's low five
 * bits read 10/2 for the same 8-byte payload, so there's no length field
 * to parse in either configuration. Fixed length (PCNF0=0/STATLEN) was
 * measured byte-identical on air, and puts byte 3 where software reads
 * and chooses it, as radiant_ack.c and radiant_burst.c need.
 *
 * Cost: a payload other than 8 bytes isn't receivable under either ANT
 * format (nothing has ever sent one). RADIANT_FRAME_CFG_LR is the
 * exception - RADIANT_LEN_FROM_BODY, 4-byte address, 0..PAYLOAD_LR_MAX,
 * RADIANT_PHY_LR_CODED - and a backend without the coded PHY refuses it
 * with the existing RADIANT_RADIO_ENOTSUP rule. Returns NULL for an
 * unknown configuration.
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
	 * Byte 3 exactly as it appears on air. A stated field, not
	 * derived from payload_len - Spike B confirmed none of its bits
	 * depend on payload length. Only cross-check is bits 2:0 == 010;
	 * use the radiant_ctrl_* accessors for the five flags above it.
	 */
	uint8_t ctrl_byte;
	uint8_t payload_len;
	uint8_t payload[RADIANT_FRAME_PAYLOAD_MAX];
};

/*
 * One frame split the way a radio wants it: what the hardware matches, what
 * it DMAs, and what its CRC engine produces. addr[0] is the first byte on
 * air; no bit/byte reversal or register layout here, same as
 * struct radiant_rx_filter.
 *
 * crc is the value as a number; on air it goes out MSB first, which
 * radiant_frame_to_bytes() does and radiant_frame_from_bytes() undoes.
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
 * combination never on the air (radiant_ctrl_observed()).
 *
 * Fields are an argument rather than a default because there's no safe
 * default: a caller meaning "acknowledged data" who got a broadcast would
 * set no trainer resistance and report no error.
 *
 * Refusing an unmeasured combination here, while radiant_frame_encode()
 * carries one set by hand, is deliberate: a field model can name 256
 * bytes, eleven have been transmitted, and this is where that's enforced.
 */
int radiant_frame_make(struct radiant_frame *f, const struct radiant_channel_id *id,
		   const struct radiant_ctrl_fields *ctrl, const uint8_t *payload,
		   uint8_t payload_len);

/* The on-air address for a channel ID in the given configuration, first
 * byte first. Returns the address length, or RADIANT_FRAME_EINVAL. In
 * search the address stops after devnum_lo, the rest moves into the body -
 * the whole difference between the two configurations. */
int radiant_frame_addr(enum radiant_frame_cfg cfg, const uint8_t net_addr[RADIANT_NET_ADDR_LEN],
		   const struct radiant_channel_id *id, uint8_t *out,
		   size_t out_len);

/*
 * Encode. Fills out->addr, out->body and out->crc; fully overwritten on
 * success, untouched on failure.
 *
 * RADIANT_FRAME_ECTRL if bits 2:0 of in->ctrl_byte are not 010. The five
 * flags above them are copied through untouched, including combinations
 * never measured - see the comment on struct radiant_frame.
 */
int radiant_frame_encode(enum radiant_frame_cfg cfg, const uint8_t net_addr[RADIANT_NET_ADDR_LEN],
		     const struct radiant_frame *in, struct radiant_frame_wire *out);

/* The CRC of a wire frame: address then body, seeded with
 * RADIANT_CRC_INIT. Does not look at wire->crc. Returns 0 for a null or
 * malformed argument, indistinguishable from a legitimate zero CRC - use
 * radiant_frame_crc_ok() to ask whether a frame verifies. */
uint16_t radiant_frame_crc(const struct radiant_frame_wire *w);

/* True only if the frame is well formed and its CRC matches. */
bool radiant_frame_crc_ok(const struct radiant_frame_wire *w);

/*
 * The CRC has already been verified elsewhere - typically a hardware CRC
 * engine, so wire->crc holds nothing useful.
 *
 * Opt-*out* rather than opt-in on purpose: a caller that forgets it gets a
 * loud RADIANT_FRAME_ECRC on a fine frame; the other polarity would
 * silently accept corruption. Only one of those failure modes is discoverable.
 */
#define RADIANT_FRAME_TRUSTED_CRC (1u << 0)

/*
 * Decode. out is written only on success, so a rejected frame can't leave
 * a half-populated structure for a caller that ignored the return value.
 *
 * A CRC failure is RADIANT_FRAME_ECRC and nothing else: never repaired,
 * never guessed, never reported as good with a flag to remember to check.
 *
 * Decode DISPATCHES on the control byte rather than validating against
 * 0x0A - rejecting everything but broadcast would drop the acknowledged
 * frames trainer control is made of. out->ctrl_byte holds whatever was on
 * the air; radiant_frame_msg_type() names it, or doesn't.
 *
 * The one thing validated is bits 2:0 == 010 (RADIANT_FRAME_ECTRL). An
 * earlier version cross-checked bits 4:0 against payload length instead,
 * which rejected every valid slave frame (0xA2's low five bits read 2, no
 * length check expecting 10 accepts it) - bits 4:0 are the slot bit, the
 * sequence bit and 010, none of them a length.
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
