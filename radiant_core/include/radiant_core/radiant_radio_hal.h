/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_hal.h - the contract between radiant_core and a radio backend.
 *
 * Provenance: clean-room. Written from the free ANT Message Protocol and Usage
 * Rev 5.1 (D00000652), from public nRF52840/nRF54L and Silicon Labs RAIL
 * documentation, and from the frame facts recorded in docs/ant-radio-link.md.
 * Nothing here derives from sdk-ant, from libant.a, or from any adopter-gated
 * ANT+ device profile document.
 *
 * ---------------------------------------------------------------------------
 * What this file is
 * ---------------------------------------------------------------------------
 * radiant_core is a link layer. It decides *what* goes on the air and *when*. A
 * backend decides *how*: which peripheral, which registers, which DMA, which
 * interrupt. This header is the whole of the boundary between them, and it is
 * written so that a second backend on a completely different vendor's radio is
 * an addition rather than a redesign. Two are planned on nRF (direct-peripheral
 * and MPSL-timeslot) and one on EFR32/RAIL; a fourth, radiant_core/tests/fake_radio.c,
 * is what lets six core modules be developed in parallel with no hardware.
 *
 * Six rules give the boundary its shape. They are not style preferences; each
 * one is a specific portability failure that has been designed out.
 *
 *   1. No register semantics. Nothing in this file names a field of any radio
 *      peripheral. An operation is "put this frame on the air with its address
 *      ending at absolute time T" or "be able to hear a matching address
 *      between T0 and T1", and a completion callback says what happened. If a
 *      backend concept cannot be phrased that way it does not belong here.
 *
 *   2. Absolute 64-bit microsecond timestamps everywhere. Not ticks, not
 *      relative delays, not a per-backend resolution. RAIL is natively
 *      microsecond-based, the nRF backends run a 1 MHz timer, and the core's
 *      scheduler is easier to reason about and to unit-test in absolute time.
 *      Relative delays would push wrap and latency handling into the core,
 *      which is exactly where portability bugs hide.
 *
 *   3. Frequency is an index 0..124 meaning 2400 + N MHz, never hertz. That is
 *      how ANT itself expresses it, how RAIL's generated channel configuration
 *      expresses it, and it removes an entire class of unit mistakes.
 *
 *   4. Power is dBm, with a raw escape hatch. dBm is portable and is what the
 *      link budget is reasoned in; the raw field exists because every part has
 *      a small set of settings that do not land on integer dBm, and a bench
 *      sweep needs to reach them without a HAL change.
 *
 *   5. PHY is a compile-time backend property, selected - not configured - at
 *      run time. RAIL's PHY comes out of a generated blob and its parameters
 *      cannot be set at run time; a backend therefore advertises the PHYs it
 *      was built with (struct radiant_radio_caps) and rejects any other. Adding the
 *      Phase 7 long-range axis grows that list. It is not a redesign.
 *
 *   6. Radio configuration is per-operation, never global state. Every arm call
 *      carries its own struct radiant_pkt_format. The reason is concrete rather
 *      than aesthetic: tracking/TX and wildcard search need genuinely different
 *      packet configurations. Search matches a 3-byte on-air address
 *      [A6 C5 devnum_lo], which leaves three bytes ahead of the length field -
 *      and the nRF RADIO's fixed S0|LENGTH|S1 layout cannot place a length
 *      field there at all, so search must run with a static-length packet and
 *      recover the matched address byte from the match index instead. A HAL
 *      with one global configuration would have been wrong from its first line,
 *      and it would have been discovered on the bench rather than here.
 *
 * See docs/ant-radio-link.md for the on-air frame and for the concrete nRF
 * mapping. Nothing in that mapping appears in this header by design.
 */

#ifndef RADIANT_RADIO_HAL_H_
#define RADIANT_RADIO_HAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Return codes
 *
 * Deliberately our own rather than <errno.h>. Only EDOM, EILSEQ and ERANGE are
 * standard C; EBUSY and ETIMEDOUT are POSIX, and this header must compile
 * standalone against a freestanding toolchain and inside a host unit test with
 * no Zephyr present. The values are negative so a call can return either an
 * error or a small non-negative result without ambiguity.
 * ---------------------------------------------------------------------------
 */
#define RADIANT_RADIO_OK_RC        0
#define RADIANT_RADIO_EINVAL     (-1)  /* malformed request */
#define RADIANT_RADIO_ENOTSUP    (-2)  /* well-formed, but this backend cannot do it */
#define RADIANT_RADIO_EBUSY      (-3)  /* an operation is already armed or running */
#define RADIANT_RADIO_ETIME      (-4)  /* the requested instant is already unreachable */
#define RADIANT_RADIO_ESTATE     (-5)  /* called in the wrong lifecycle state */
#define RADIANT_RADIO_EIO        (-6)  /* the hardware did not do as it was told */

/* ---------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------------
 */

/*
 * Absolute microseconds on a single monotonic backend timebase, starting at an
 * unspecified origin at radiant_radio_init() and never going backwards. 64 bits is
 * 584,000 years; the core may assume it never wraps and may subtract two
 * timestamps freely.
 *
 * A backend whose hardware counter is narrower must extend it. This is not
 * hypothetical: RAIL_Time_t is 32-bit microseconds and wraps every ~71.6
 * minutes, so the EFR32 backend owns a wrap-extension counter. Putting that
 * duty in the backend rather than in the core is the point of the rule - a
 * dongle left plugged in over a weekend must not acquire a scheduling bug at
 * minute 72.
 */
typedef uint64_t radiant_time_t;

/* "No deadline" / "not known". Never a valid arm time. */
#define RADIANT_TIME_NEVER  ((radiant_time_t)UINT64_MAX)

/* ---------------------------------------------------------------------------
 * Frequency and power
 * ---------------------------------------------------------------------------
 */

/* RF index N means 2400 + N MHz. ANT+ lives on 57 (2457 MHz). */
#define RADIANT_RF_INDEX_MAX      124
#define RADIANT_RF_INDEX_ANT_PLUS 57

static inline uint16_t radiant_rf_index_to_mhz(uint8_t rf_index)
{
	return (uint16_t)(2400u + rf_index);
}

/*
 * Transmit power.
 *
 * dbm is the request. A backend rounds to the nearest setting it has and does
 * not fail for an unreachable value - the link budget does not care about
 * half a dB, and failing here would make a shared scheduler brittle.
 *
 * use_raw is the escape hatch for bench work and for parts whose settings do
 * not land on integer dBm. When it is set, dbm is ignored and raw is written
 * to whatever backend-specific setting corresponds. A raw value is by
 * definition not portable; nothing in the core may set it except code that
 * already knows which backend it is talking to (a bench sweep, a Kconfig
 * override). Everything else uses dbm.
 */
struct radiant_tx_power {
	int8_t   dbm;
	bool     use_raw;
	uint32_t raw;
};

/* ---------------------------------------------------------------------------
 * PHY
 *
 * Compile-time property of the backend build, selected per operation from the
 * set the backend advertises. RADIANT_PHY_1M_GFSK is the only one an ANT+
 * compatibility channel may ever use; the long-range entry is reserved for the
 * Phase 7 per-device-type opt-in and is listed here so that adding it later
 * touches a table rather than an interface.
 * ---------------------------------------------------------------------------
 */
enum radiant_phy {
	/* 1 Mbps GFSK, ANT-compatible deviation. The ANT+ PHY. */
	RADIANT_PHY_1M_GFSK = 0,
	/* Reserved: lower-rate GFSK for RadiANT-only device types (Phase 7).
	 * A backend that was not built with it must reject it, not approximate it.
	 */
	RADIANT_PHY_LR_GFSK = 1,
	RADIANT_PHY_COUNT
};

/* ---------------------------------------------------------------------------
 * CRC
 *
 * Expressed as a value, not as a register. CRC-16/CCITT-FALSE is
 * width 16, poly 0x1021, init 0xFFFF, no reflection, no final XOR, and it
 * covers the on-air address bytes as well as the body.
 *
 * Note "poly = 0x1021", not 0x11021: the implicit x^16 term is not part of the
 * value. One backend's register wants it included and translating is that
 * backend's job. A backend that has no hardware CRC engine able to express
 * this - covering the address is the usual sticking point, because most sync-
 * word matchers exclude the sync word from the CRC - computes it in software
 * and says so through caps.crc_in_hw. The contract is unchanged either way:
 * a delivered packet with status RADIANT_RADIO_STATUS_OK has a verified CRC.
 * ---------------------------------------------------------------------------
 */
struct radiant_crc_cfg {
	uint8_t  width_bits;   /* 0 disables the CRC entirely */
	uint32_t poly;         /* normal (unreflected) form, x^width term implicit */
	uint32_t init;
	uint32_t xor_out;
	bool     reflect_in;
	bool     reflect_out;
	bool     cover_addr;   /* CRC includes the on-air address bytes */
};

/* ---------------------------------------------------------------------------
 * Packet format
 * ---------------------------------------------------------------------------
 */

/* Longest on-air address any planned backend can match in hardware. */
#define RADIANT_RADIO_ADDR_MAX 5

/*
 * Advisory ceiling for statically sized body buffers in the core. The real
 * limit is caps.max_body_len. Sized for the largest frame currently foreseen:
 * an advanced-burst body in ANT's tracking geometry, which is transmission
 * type + control byte + 24 payload = 26. There is NO length byte anywhere in
 * an ANT frame - byte 3 is six independent flag fields
 * (docs/spike-b-part2-results.md) - and an earlier version of this line said
 * otherwise. No frame with a payload other than 8 bytes has ever been captured
 * on this bench, so 26 is a ceiling rather than a measurement.
 */
#define RADIANT_RADIO_BODY_MAX 32

enum radiant_len_mode {
	/* Every frame in this format has exactly fmt->body_len body bytes. */
	RADIANT_LEN_FIXED = 0,
	/*
	 * The body carries its own length at body[len_offset]:
	 *     body_len = body[len_offset] + len_bias
	 * len_bias exists because a format's declared length and a radio's idea
	 * of body length may count the CRC bytes and the header bytes ahead of
	 * the payload differently. Expressing the relation as an offset and a
	 * bias keeps the arithmetic in one place instead of once per backend.
	 *
	 * NO ANT FORMAT USES THIS MODE, and none ever can. The rationale here
	 * used to read "the ANT length byte counts the CRC bytes", which was
	 * `[inferred]` from broadcasts alone and is now falsified: byte 3 is a
	 * control byte whose low five bits read 10 on a broadcast and 2 on an
	 * in-slot frame, both carrying eight payload bytes
	 * (docs/spike-b-part2-results.md). radiant_frame_format() returns
	 * RADIANT_LEN_FIXED for both ANT configurations, and a backend that maps
	 * ANT tracking onto this mode - i.e. onto nRF PCNF0.LFLEN=8 - is a
	 * broadcast-only receiver that silently drops every acknowledged and
	 * burst frame.
	 *
	 * The mode stays because it is a real capability of real radios and
	 * some future format may want it. It is exercised by
	 * fmt_hal_len_from_body in radiant_core/tests/src/test_fake_radio.c, which
	 * is labelled at length for exactly this reason.
	 */
	RADIANT_LEN_FROM_BODY = 1
};

/*
 * How one on-air frame is laid out, from the receiver's point of view:
 *
 *   [preamble] [address: addr_len bytes] [body: body_len bytes] [CRC]
 *
 * The preamble is entirely the backend's business - its length and value are
 * PHY properties, and on at least one part the radio derives the preamble
 * pattern from the first address bit with no software involvement.
 *
 * "Body" is every byte between the address and the CRC, opaque to the HAL. The
 * core hands over the exact body bytes for transmit and receives the exact body
 * bytes back. Whether a given backend implements the leading body bytes with a
 * dedicated header field, a static length, or a length-field decoder is an
 * implementation detail that must not change a single byte on the air.
 *
 * The core is expected to use a small set of static const formats - one for
 * tracking/TX, one for search, later one per extension - because a backend may
 * need to precompile them (RAIL derives frame-length handling from a generated
 * configuration, so the set of expressible formats is fixed when the image is
 * built). A backend that cannot express a format MUST fail the arm call with
 * RADIANT_RADIO_ENOTSUP. It must never transmit or receive something close.
 */
struct radiant_pkt_format {
	enum radiant_phy       phy;
	uint8_t            addr_len;     /* on-air address bytes, 2..RADIANT_RADIO_ADDR_MAX */
	enum radiant_len_mode  len_mode;
	uint8_t            body_len;     /* RADIANT_LEN_FIXED only */
	uint8_t            len_offset;   /* RADIANT_LEN_FROM_BODY only */
	int8_t             len_bias;     /* RADIANT_LEN_FROM_BODY only */
	uint8_t            max_body_len; /* RX bound; reject anything longer */
	struct radiant_crc_cfg crc;
};

/* ---------------------------------------------------------------------------
 * Receive filters, and why the core does the sweeping
 * ---------------------------------------------------------------------------
 */

/*
 * One address the receiver should accept during a window.
 *
 * addr[0] is the first byte on the air. There is no bit- or byte-reversal here
 * and no register layout: the core states the on-air byte order and the backend
 * does whatever its hardware needs to produce it. (On at least one part the
 * address is emitted least-significant-bit first irrespective of the endianness
 * setting that governs the rest of the frame, so every address byte has to be
 * bit-reversed on the way into the peripheral. That is exactly the sort of fact
 * that must live in one backend and not in the link layer.)
 *
 * There is no wildcard field. This is deliberate. ANT wildcard search wants
 * "any device number", and no planned backend can express that: the shortest
 * on-air address the nRF RADIO will match is 3 bytes, so the third matched byte
 * is unavoidably the low byte of the device number, and RAIL matches at most
 * two sync words at a time. So wildcard search is *core-level policy driven by
 * a capability query*, not a HAL feature: with caps.filter_wildcard_dev false
 * and caps.max_filters == 8, radiant_search.c enumerates eight concrete addresses
 * per window and sweeps 32 sets to cover all 256 values of that byte. With
 * max_filters == 2 the same policy code produces 128 sets, or chooses a
 * different strategy, without a HAL change. If some future backend can truly
 * wildcard the device-number byte it sets filter_wildcard_dev and the policy
 * collapses to one window.
 *
 * THE PARAGRAPH ABOVE DESCRIBES THE 3-BYTE SEARCH FORMAT AND ONLY IT. It used
 * to be the whole of what this preamble said about filters, and read as a
 * general statement it is wrong in a way that costs windows: on the 5-byte
 * tracking format the device number lives inside the part of the address the
 * nRF calls the BASE, and a window can carry at most two distinct bases. Eight
 * filters is eight *addresses*; it is not eight *device numbers*. See
 * caps.max_addr_groups, which is the number that governs the tracking case, and
 * ADR 0005's "32 sensors do not cost 32 windows" claim, which is true only when
 * the scheduler reads that number rather than this one.
 *
 * Putting the sweep in the core rather than in the HAL is what keeps the sweep
 * *shared*: one sweep serves every channel in SEARCHING at once. A per-backend
 * sweep could not do that, and eight simultaneous searches would take ~64 s
 * instead of ~8 s.
 */
struct radiant_rx_filter {
	uint8_t addr[RADIANT_RADIO_ADDR_MAX];
	uint8_t addr_len;   /* must equal fmt->addr_len */
};

/* ---------------------------------------------------------------------------
 * t_sync - the subtlest thing in this file
 * ---------------------------------------------------------------------------
 *
 * DEFINITION. t_sync is the absolute time, in this HAL's microsecond timebase,
 * at which the *last bit of the on-air address was at the antenna*. Not when
 * the interrupt fired, not the end of the packet, not when a callback ran.
 * Every backend applies its own correction for demodulator group delay, filter
 * latency and event-capture offset so that all of them report the same instant
 * for the same physical event.
 *
 * WHY THAT INSTANT. It is the earliest point at which the frame is identified,
 * it is equally definable on transmit and on receive, and it is independent of
 * payload length and of PHY. Anchoring on the end of the packet instead would
 * make the reference move whenever a frame's length changed, which is precisely
 * what the extension axes do.
 *
 * SYMMETRY ON TRANSMIT. A transmit request names the t_sync the frame should
 * have (radiant_tx_req.t_sync_at), and the backend works backwards through its own
 * ramp-up, preamble and address airtime to decide when to actually start. A
 * master's period is then exact by construction, and a slave's acknowledged
 * reply is expressed as an offset from the master's t_sync, with the turnaround
 * budget checkable against caps.rx_to_tx_us. If TX were anchored on "start
 * transmitting" instead, every timing constant in the core would silently carry
 * one backend's ramp-up time.
 *
 * SYMMETRY ON RECEIVE. An RX window is likewise expressed in t_sync terms: the
 * receiver must be able to detect any frame whose t_sync falls within
 * [t_open, t_close]. The backend therefore has to be in receive early enough to
 * have heard the whole preamble and address of a frame that completes exactly
 * at t_open. Defining the window on "when RX is enabled" instead would make the
 * effective window edge depend on ramp-up and preamble length - the same silent
 * per-backend bias, in the other direction.
 *
 * CALIBRATION. The correction constant is measured per backend with a wired
 * two-board trigger: one board pulses a GPIO from its own address-sent event,
 * the other captures that pulse on the same timer it timestamps t_sync with,
 * and the difference minus the known cable and air delay is the constant. It is
 * a measurement, not a datasheet number, and it is re-measured for every new
 * part and every PHY. Each backend records its measured value and the date in
 * a comment next to the constant.
 *
 * THE FAILURE MODE, WHICH IS SILENT. The channel state machine predicts the
 * next master transmission from previous t_sync values and centres the next RX
 * window on the prediction. A *constant* t_sync error cancels out of the
 * period estimate - so the drift PLL still locks and nothing looks wrong - but
 * it shifts every window by that amount, eating the guard on one side and
 * enlarging it on the other. The window keeps catching packets until the offset
 * plus jitter reaches the edge, at which point yield falls. There is no error
 * code, no CRC failure, no log line: swapping backends simply changes RX yield
 * by a few tenths of a percent, at the same order as the bench's characterised
 * ~0.4% collision floor, which is precisely the range in which a real
 * regression is easiest to mistake for noise. This is the reason t_sync has a
 * written contract, a per-backend measured constant, and a paragraph of prose.
 *
 * THE OTHER CONSUMER. Extended message lib config 0xE0 reports channel ID,
 * RSSI and an RX timestamp to the host. That timestamp MUST be derived from
 * t_sync and MUST NOT come from a host-side or thread-context clock. It is what
 * makes the timing figure read 0.009 ms on the radio clock rather than the
 * ~2.6 ms a host clock sees through USB jitter, and it is what the A/B `timing`
 * gate is read against. It is also the whole basis of the sub-millisecond
 * multi-sensor fusion claim. A backend with caps.has_sync_timestamp false must
 * still fill t_sync on a best-effort basis and MUST clear t_sync_exact on the
 * event, so that radiant_event.c can degrade the 0xE0 timestamp's advertised
 * accuracy instead of quietly reporting a worse number as if it were the good
 * one.
 */

/* ---------------------------------------------------------------------------
 * Operations
 * ---------------------------------------------------------------------------
 */

/*
 * Transmit one frame.
 *
 * body must remain valid and unmodified from the arm call until the completion
 * callback - backends DMA straight out of it - and must live in memory the
 * radio's DMA can reach (RAM, not flash, on every planned part).
 *
 * addr is NOT part of body and never has been. body is the bytes between the
 * address and the CRC, in both directions, and the receive side has always said
 * so explicitly through struct radiant_rx_filter. This request said nothing,
 * and the first version of this structure genuinely had no address field at
 * all - see the note under "Symmetry" below, which is kept because the reason
 * it survived review is more useful than the fix.
 */
struct radiant_tx_req {
	const struct radiant_pkt_format *fmt;
	uint8_t                      rf_index;
	struct radiant_tx_power          power;
	/*
	 * The on-air address, first byte on the air first - the same statement
	 * struct radiant_rx_filter makes, on the same terms: no bit- or
	 * byte-reversal, no register layout, the backend does whatever its
	 * hardware needs to produce these bytes in this order.
	 *
	 * SYMMETRY. An RX request names the address it will match. A TX request
	 * must name the address it will emit, because there is nowhere else for
	 * a backend to get one. That reads as obvious and it was still missing
	 * for the whole of the mock-only period, for a reason worth recording:
	 * radiant_core/tests/fake_radio.c records a transmit request and replays
	 * it, so a missing field is simply a field that is neither written nor
	 * read, and every test above the HAL asserts on body bytes. It took the
	 * first real backend to make it a question, because on the nRF RADIO the
	 * transmit address is TXADDRESS - an index into BASE/PREFIX registers
	 * that the PREVIOUS operation left loaded. A transmit with no address in
	 * its request does not fail; it inherits the last receive's device
	 * number and puts a well-formed frame addressed to the wrong sensor on
	 * the air. That is worse than a dropped frame, because another device
	 * may accept it.
	 *
	 * Inline rather than a pointer, unlike body: five bytes go into
	 * registers rather than into DMA, so there is no reason to make the
	 * caller guarantee a lifetime it could get wrong.
	 */
	uint8_t                      addr[RADIANT_RADIO_ADDR_MAX];
	uint8_t                      addr_len;   /* must equal fmt->addr_len */
	const uint8_t               *body;
	uint8_t                      body_len;
	/* Requested t_sync of the frame. Must be at least caps.min_arm_lead_us
	 * in the future; otherwise the call fails RADIANT_RADIO_ETIME. A backend
	 * never silently transmits late - a late master frame is worse than no
	 * frame, because it lands in the next slot.
	 */
	radiant_time_t                   t_sync_at;
};

/* Close the window as soon as one frame is accepted. Saves receive current on
 * a tracked single-master window; must NOT be set on a merged window or a
 * search window, where more than one master may transmit inside one window.
 */
#define RADIANT_RX_STOP_ON_FIRST  (1u << 0)
/* Deliver RADIANT_RADIO_STATUS_CRC_FAIL events too. Off by default: the core only
 * needs them for the RX_FAIL accounting that makes "unexplained loss" a
 * meaningful gate, and for bench diagnostics.
 */
#define RADIANT_RX_REPORT_CRC_FAIL (1u << 1)

/*
 * Open one receive window matching up to n_filters addresses.
 *
 * MULTIPLE FILTERS IN ONE WINDOW IS THE POINT. All ANT+ traffic is on RF 57,
 * so tracked channels whose predicted windows overlap can be merged by the
 * scheduler into a single hardware window carrying one filter per channel.
 * That is the highest-value item in the scheduler: with merged windows a slave
 * never misses one tracked sensor because it was listening for another, so
 * slave-side inter-channel collisions drop to zero, and 32 tracked sensors do
 * not cost 32 windows. The HAL's job is only to make the merge expressible;
 * deciding what to merge is radiant_sched.c's.
 *
 * A window may therefore deliver several packets: each match produces one
 * rx_event, and the operation ends with exactly one terminal event
 * (RADIANT_RADIO_STATUS_TIMEOUT if the window closed, RADIANT_RADIO_STATUS_ABORTED if
 * it was cancelled). With RADIANT_RX_STOP_ON_FIRST the first accepted frame's event
 * is itself the terminal one.
 */
struct radiant_rx_req {
	const struct radiant_pkt_format *fmt;
	uint8_t                      rf_index;
	const struct radiant_rx_filter  *filters;   /* n_filters entries, core-owned,
						 * must stay valid until the
						 * terminal event */
	uint8_t                      n_filters; /* 1..caps.max_filters */
	radiant_time_t                   t_open;    /* earliest acceptable t_sync */
	radiant_time_t                   t_close;   /* latest acceptable t_sync */
	uint32_t                     flags;
};

/* ---------------------------------------------------------------------------
 * Events
 * ---------------------------------------------------------------------------
 */

enum radiant_radio_status {
	RADIANT_RADIO_STATUS_OK = 0,       /* frame received, CRC verified */
	RADIANT_RADIO_STATUS_CRC_FAIL,     /* address matched, CRC did not */
	RADIANT_RADIO_STATUS_TIMEOUT,      /* window closed */
	RADIANT_RADIO_STATUS_ABORTED,      /* radiant_radio_abort() or a lifecycle call */
	RADIANT_RADIO_STATUS_FAILED        /* the backend could not complete it */
};

struct radiant_rx_event {
	uint32_t              op;      /* the id the arm call returned */
	enum radiant_radio_status status;
	/* Valid when status is OK or CRC_FAIL. See the t_sync contract above. */
	radiant_time_t            t_sync;
	bool                  t_sync_exact;
	/*
	 * Which filter matched, indexing radiant_rx_req.filters.
	 *
	 * This is how the core recovers information the payload does not carry.
	 * In wildcard search the third on-air address byte is devnum_lo, so the
	 * matched filter index *is* the identity of that byte - the core maps
	 * index back to the address it supplied and reads devnum_lo out of it.
	 * In a merged tracked window the index says which channel the frame
	 * belongs to before a single payload byte has been parsed. Backends
	 * must report it for every OK and CRC_FAIL event; a backend whose
	 * hardware matched fewer bytes than the filter and completed the match
	 * in software reports the index of the filter that fully matched.
	 */
	uint8_t               filter_index;
	/* Backend-owned, valid only for the duration of this callback. Copy
	 * anything that must outlive it. */
	const uint8_t        *body;
	uint8_t               body_len;
	bool                  has_rssi;   /* mirrors caps.has_rssi */
	int8_t                rssi_dbm;

	/*
	 * The CRC as it arrived on the air, NOT as recomputed over `body`.
	 *
	 * Valid only on RADIANT_RADIO_STATUS_CRC_FAIL, and only on a backend
	 * whose caps.has_rx_crc is true. Meaningless - and left zero, with
	 * has_crc_rx false - on every other event, including OK: a frame that
	 * passed already has a CRC the core can recompute for itself.
	 *
	 * WHY IT IS HERE AT ALL. `body` carries the bytes between address and
	 * CRC and the CRC bytes are not among them, so a core that wants to know
	 * HOW a frame failed cannot find out: the difference between the CRC
	 * received and the CRC computed is the error syndrome, and without the
	 * first half there is no syndrome. That difference is what
	 * radiant_crc_repair.c turns a single flipped bit into a good packet
	 * with, which is worth 1-3 dB of effective sensitivity for a table and
	 * nothing on the air.
	 *
	 * uint32_t rather than uint16_t because it is the CRC register's value:
	 * the nRF RADIO's RXCRC field is 24 bits wide and a backend should not
	 * have to narrow it on the way out. Only as many low bits as the
	 * configured struct radiant_crc_cfg produces are meaningful.
	 */
	bool                  has_crc_rx;
	uint32_t              crc_rx;

	/*
	 * RSSI measured INSIDE THIS WINDOW WITH NO PACKET PRESENT.
	 *
	 * Valid only on a terminal RADIANT_RADIO_STATUS_TIMEOUT event for a
	 * window that delivered no frame at all, which is exactly the
	 * population that is the noise floor: the radio listening to a
	 * frequency at a moment when, by observation, nothing was transmitting
	 * on it. A backend that cannot take such a sample leaves has_noise
	 * false, and that is not a degraded mode - it is a backend that has
	 * nothing to say.
	 *
	 * NOT rssi_dbm under another name, and the difference is the whole
	 * point. rssi_dbm is the level of a packet that arrived, so it measures
	 * a transmitter; this measures the absence of one. A window that
	 * received something must never report both, because the frame's own
	 * energy is in the number.
	 *
	 * On the same scale as rssi_dbm - same corrections, same reference - so
	 * the two can be subtracted to get a margin. A backend that applied a
	 * temperature or errata correction to one and not the other would
	 * produce a margin figure that is wrong by the correction and looks
	 * entirely plausible.
	 */
	bool                  has_noise;
	int8_t                noise_dbm;
};

struct radiant_tx_event {
	uint32_t              op;
	enum radiant_radio_status status;
	/* Actual t_sync of the frame that went out, for closing the loop on
	 * master slot phase. Equals the requested value on every backend that
	 * can schedule exactly; a backend that cannot must report what it did. */
	radiant_time_t            t_sync;
	bool                  t_sync_exact;
};

/*
 * Callback and threading contract.
 *
 * CONTEXT. Callbacks run in the backend's radio interrupt context, at the
 * highest priority the backend uses. They are not a work queue and not a
 * thread. This is not an implementation detail that might change: re-arming
 * the next operation from inside the completion callback is the low-jitter
 * path, and it is the only one that reliably meets an acknowledged-data
 * turnaround.
 *
 * A callback MAY:
 *   - read the event (only for the duration of the call),
 *   - call radiant_radio_tx(), radiant_radio_rx(), radiant_radio_abort(), radiant_radio_now()
 *     and radiant_radio_caps_get(),
 *   - signal an ISR-safe synchronisation object to wake a thread.
 *
 * A callback MUST NOT:
 *   - block, sleep, take a mutex, or wait on anything,
 *   - retain event->body past the return (it is a backend DMA buffer and is
 *     reused immediately),
 *   - call radiant_radio_init(), radiant_radio_enable() or radiant_radio_disable(),
 *   - do work proportional to anything - queue it and return.
 *
 * ORDERING. Callbacks never nest and never run concurrently with each other.
 * Every accepted arm call produces exactly one terminal event; a receive window
 * may additionally produce zero or more non-terminal RADIANT_RADIO_STATUS_OK /
 * CRC_FAIL events before it. Events are delivered in time order.
 *
 * RE-ENTRANCY FROM THREADS. Arm calls made from thread context race with the
 * callback that may be about to consume the operation slot. The backend
 * resolves that race internally and returns RADIANT_RADIO_EBUSY rather than
 * corrupting state; the core must handle EBUSY rather than assume it cannot
 * happen.
 */
struct radiant_radio_cbs {
	void (*rx)(const struct radiant_rx_event *evt, void *user);
	void (*tx)(const struct radiant_tx_event *evt, void *user);
};

/* ---------------------------------------------------------------------------
 * Capabilities
 *
 * The query that makes portability structural instead of conditional. Core
 * policy reads these; core policy never tests for a backend by name, and there
 * is no #ifdef on a part number anywhere above this line.
 * ---------------------------------------------------------------------------
 */
struct radiant_radio_caps {
	/* Human-readable, for logs and for the A/B report header. */
	const char *name;

	/* Addresses matchable in one receive window. 8 on nRF (one base plus
	 * eight prefixes); 2 on RAIL (two runtime sync words). This single
	 * number sets both the wildcard-search sweep length and how many tracked
	 * channels can share a merged window. */
	uint8_t max_filters;

	/*
	 * Distinct values of addr[0 .. addr_len-2] matchable in one window -
	 * "address groups". THIS IS NOT max_filters, AND ASSUMING IT IS IS A
	 * REAL BUG WITH A QUIET SIGNATURE.
	 *
	 * The nRF matcher has eight logical addresses and they are not eight
	 * independent ones: logical 0 is BASE0 + AP0 and logical 1..7 are BASE1
	 * + AP1..AP7. So a window carries at most TWO DISTINCT BASES, and seven
	 * of the eight filters must share one. max_addr_groups is 2 there.
	 *
	 * That falls out exactly right for search and exactly wrong for
	 * tracking:
	 *
	 *   SEARCH, 3-byte [A6 C5 devnum_lo]: the group is [A6 C5], shared by
	 *   every filter, and the prefix is devnum_lo. Eight filters, one
	 *   group, no constraint hit.
	 *
	 *   TRACKING, 5-byte [A6 C5 dnl dnh dtype]: the group is
	 *   [A6 C5 dnl dnh] and differs per sensor, because the device number
	 *   is inside it. Two tracked channels share a window only if they have
	 *   the same device number, which is to say never - so a merged
	 *   tracking window holds TWO channels on nRF, not eight.
	 *
	 * Where every filter carries a full independent address - RAIL's two
	 * sync words - this equals max_filters and the constraint never binds.
	 *
	 * A backend that leaves this 0 is read as "no group constraint" for
	 * backward compatibility, and the scheduler treats it as max_filters.
	 * A backend whose hardware does have the constraint and does not say so
	 * gets a stream of RADIANT_RADIO_ENOTSUP from arm_rx() instead, which is
	 * safe and is diagnosable only by reading the backend - hence the
	 * field.
	 */
	uint8_t max_addr_groups;

	/* True only if the backend can match "any value" in the device-number
	 * bytes with a single filter. False on every planned backend; see
	 * struct radiant_rx_filter for what the core does about it. */
	bool filter_wildcard_dev;

	/* Longest address the hardware matcher itself handles. Informational:
	 * semantics do not change if it is shorter than a requested filter,
	 * because the backend completes the match in software before delivering
	 * - but a shorter hardware match means more spurious wakeups and more
	 * receive current, which the scheduler may reasonably care about. */
	uint8_t addr_len_hw_max;

	/* Largest body (bytes between address and CRC) in either direction. */
	uint8_t max_body_len;

	/* PHYs this build supports, most-preferred first. n_phys is always >= 1
	 * and phys[0] is RADIANT_PHY_1M_GFSK on any backend claiming ANT+
	 * compatibility. The Phase 7 long-range axis appends to this list. */
	const enum radiant_phy *phys;
	uint8_t             n_phys;
	/* Cost of switching between two of the above between operations. Zero
	 * where a PHY switch is free; non-zero where it means reloading a
	 * generated configuration, which the scheduler must budget for. */
	uint16_t phy_switch_us;

	/* Transmitter ramp-up, measured antenna-referenced. */
	uint16_t ramp_up_us;
	/* Turnaround, receive-end to transmit-start and back. The
	 * acknowledged-data reply budget is checked against rx_to_tx_us. */
	uint16_t rx_to_tx_us;
	uint16_t tx_to_rx_us;

	/* Minimum lead time between radiant_radio_now() and the earliest instant an
	 * arm call can still honour: software setup, DMA, clock start and
	 * ramp-up. Arming inside this window fails RADIANT_RADIO_ETIME rather than
	 * running late. The scheduler's slack budget is built on this number. */
	uint16_t min_arm_lead_us;

	/* Granularity of the underlying timebase, in nanoseconds. 1000 on a
	 * 1 MHz timer and on RAIL. Timestamps are still microseconds; this says
	 * how much of the last digit to believe. */
	uint16_t time_resolution_ns;

	/* True if t_sync comes from a hardware capture of the address event
	 * rather than from an inference. When false, events carry
	 * t_sync_exact = false and the 0xE0 RX timestamp must be advertised as
	 * approximate. */
	bool has_sync_timestamp;

	/* True if rx_event.rssi_dbm is populated. */
	bool has_rssi;

	/*
	 * True if rx_event.crc_rx is populated on a CRC_FAIL event - that is,
	 * if the hardware keeps the CRC it received rather than only a
	 * pass/fail bit.
	 *
	 * A backend that verifies in software has it trivially; one whose
	 * engine exposes a received-CRC register has it; one that reports only
	 * a status bit does not, and says so. The core's single-bit repair is
	 * arithmetic on a syndrome and simply does not run without this - it
	 * cannot be approximated, and a backend that left this true while
	 * reporting a zero would manufacture repairs out of nothing.
	 */
	bool has_rx_crc;

	/* True if the CRC in struct radiant_crc_cfg is computed by hardware. False
	 * means the backend verifies in software after reception: identical
	 * semantics, but more CPU per frame and no hardware suppression of
	 * noise-triggered matches. Present because it changes the cost model of
	 * a wide search window, not because it changes the contract. */
	bool crc_in_hw;

	/* Inclusive transmit power range in dBm, for clamping and for the
	 * bench sweep's bounds. */
	int8_t tx_power_min_dbm;
	int8_t tx_power_max_dbm;
};

/* ---------------------------------------------------------------------------
 * API
 *
 * One radio, one instance: these are plain functions, not a vtable with an
 * instance pointer. Every planned target has exactly one radio, a vtable would
 * cost an indirect call in the interrupt path for no present benefit, and
 * radiant_core/tests/fake_radio.c substitutes at link time - which is also what
 * makes the mock cheap.
 *
 * Lifecycle: init -> enable -> (arm/callback)* -> disable. Arm calls outside
 * the enabled state return RADIANT_RADIO_ESTATE.
 * ---------------------------------------------------------------------------
 */

/* Static for the lifetime of the program, never NULL, callable before init. */
const struct radiant_radio_caps *radiant_radio_caps_get(void);

/* Claim the peripherals and start the timebase. cbs must outlive the backend.
 * Does not put the radio on the air. */
int radiant_radio_init(const struct radiant_radio_cbs *cbs, void *user);

/* Power up: clocks, calibration, whatever the part needs before an arm call
 * can meet min_arm_lead_us. Idempotent. */
int radiant_radio_enable(void);

/* Abort anything in flight, deliver its terminal event, and power down.
 * Timestamps remain monotonic across a disable/enable pair. */
int radiant_radio_disable(void);

/* Current time on the HAL timebase. Callable from any context including a
 * callback, and cheap enough to be called in one. */
radiant_time_t radiant_radio_now(void);

/*
 * Arm a transmit or a receive. At most one operation exists at a time: a second
 * arm call while one is armed or running returns RADIANT_RADIO_EBUSY. Chaining is
 * done by arming the next operation from the completion callback, which is why
 * the callback contract permits exactly that.
 *
 * On success *op receives a monotonically increasing, non-zero identifier that
 * every event for this operation carries. It exists so that a late event from a
 * cancelled operation is recognisable rather than merely surprising - a class
 * of bug that is otherwise very hard to see in a scheduler trace.
 */
int radiant_radio_tx(const struct radiant_tx_req *req, uint32_t *op);
int radiant_radio_rx(const struct radiant_rx_req *req, uint32_t *op);

/*
 * Abort the current operation. Its terminal event is still delivered, with
 * status RADIANT_RADIO_STATUS_ABORTED, before this function's caller can observe
 * the slot as free - so the core never has to reason about an operation that
 * ended without an event. Returns RADIANT_RADIO_OK_RC when there was nothing to do.
 */
int radiant_radio_abort(void);

/*
 * Deliberately absent, so that their absence is a decision rather than an
 * oversight:
 *   - continuous-wave / test-mode output. Not in v1 (dropped along with the
 *     rest of the CW test API); a bench build can add a backend-private entry
 *     point without touching this contract.
 *   - encryption offload. RadiANT's switches operate on payload bytes above
 *     this layer, so a crypto peripheral is not a radio capability.
 *   - any notion of channel, network, device number or ANT message. This file
 *     knows about addresses, bodies and time. Everything ANT-shaped lives above
 *     it, which is what lets fake_radio.c be a few hundred lines.
 */

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RADIO_HAL_H_ */
