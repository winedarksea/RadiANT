/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_radio_hal.h - the contract between radiant and a radio backend.
 *
 * Provenance: clean-room, from the free ANT Message Protocol and Usage Rev 5.1
 * (D00000652), public nRF52840/nRF54L and Silicon Labs RAIL documentation, and
 * docs/ant-radio-link.md. Nothing here derives from sdk-ant, libant.a, or any
 * ANT+ device profile document.
 *
 * radiant decides *what* goes on the air and *when*; a backend decides
 * *how*. Two backends are planned on nRF (direct-peripheral and MPSL-timeslot),
 * one on EFR32/RAIL, and radiant/tests/fake_radio.c is the mock.
 *
 * Design rules, each closing a specific portability failure:
 *   1. No register semantics - operations are phrased as "on the air by time T"
 *      / "listen between T0 and T1", never as peripheral fields.
 *   2. Absolute 64-bit microsecond timestamps everywhere, not ticks or relative
 *      delays - keeps wrap/latency handling out of the core.
 *   3. Frequency is an index 0..124 meaning 2400 + N MHz, never hertz.
 *   4. Power is dBm, with a raw escape hatch for settings that don't land on
 *      integer dBm and for bench sweeps.
 *   5. PHY is a compile-time backend property selected per operation, not
 *      configured at run time - RAIL's PHY comes from a generated blob.
 *   6. Radio configuration is per-operation (struct radiant_pkt_format), never
 *      global state: tracking/TX and wildcard search need different packet
 *      configs, and the nRF RADIO's fixed S0|LENGTH|S1 layout can't place a
 *      length field where search's 3-byte address needs one, so search runs
 *      static-length and recovers the address byte from the match index.
 *
 * See docs/ant-radio-link.md for the on-air frame and the concrete nRF mapping;
 * none of that mapping appears in this header by design.
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
/*
 * RADIANT_RADIO_EDENIED - request well formed, instant reachable, but an
 * arbitrated backend (MPSL beside BLE/802.15.4, RAIL's mp scheduler) has no
 * air to give. A backend that owns the radio never returns it.
 *
 * Not ETIME: ETIME means the instant itself is unreachable, and radiant_sched.c
 * reports that as MISSED; under an arbiter the instant IS reachable, just not
 * granted, which is a different defect and needs its own code.
 *
 * Synchronous rather than event-only because denial can happen with no time to
 * recover: radiant_transfer.c arms an acknowledged-data reply from inside the
 * RX callback, 1.56 ms before it must be on air. A denial discovered after the
 * request was placed and never granted arrives later as
 * RADIANT_RADIO_STATUS_DENIED on the terminal event; both are the same fact at
 * different times and get the same accounting.
 */
#define RADIANT_RADIO_EDENIED    (-7)

/* ---------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------------
 */

/*
 * Absolute microseconds on a single monotonic backend timebase, origin
 * unspecified at radiant_radio_init(), never going backwards. 64 bits never
 * wraps in practice, so the core may subtract two timestamps freely.
 *
 * A backend with a narrower hardware counter must extend it itself:
 * RAIL_Time_t is 32-bit microseconds and wraps every ~71.6 minutes, so the
 * EFR32 backend owns a wrap-extension counter rather than pushing that into
 * the core.
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
 * Transmit power. dbm is the request; a backend rounds to its nearest setting
 * rather than failing on an unreachable value.
 *
 * use_raw is the escape hatch for bench work and parts whose settings don't
 * land on integer dBm: dbm is then ignored and raw is written directly. A raw
 * value is backend-specific, so only code that already knows the backend (a
 * bench sweep, a Kconfig override) may set it.
 */
struct radiant_tx_power {
	int8_t   dbm;
	bool     use_raw;
	uint32_t raw;
};

/* ---------------------------------------------------------------------------
 * PHY - compile-time backend property, selected per operation from the set
 * the backend advertises. RADIANT_PHY_1M_GFSK is the only PHY an ANT+
 * compatibility channel may use; the long-range entry is ADR 0007's
 * per-device-type opt-in.
 * ---------------------------------------------------------------------------
 */
enum radiant_phy {
	/* 1 Mbps GFSK, ANT-compatible deviation. The ANT+ PHY. */
	RADIANT_PHY_1M_GFSK = 0,
	/*
	 * LE Coded PHY at S=8: 125 kbit/s, hardware FEC/pattern mapping,
	 * ~8 dB over 1 M. A backend not built with it must reject rather than
	 * approximate it - a 1 M receiver cannot demodulate a coded frame at
	 * all, which is the physical invisibility ADR 0007's length extension
	 * relies on. (Renamed from RADIANT_PHY_LR_GFSK, which mis-described the
	 * modulation; numeric value kept unchanged since it's never been
	 * stored or on the air.)
	 */
	RADIANT_PHY_LR_CODED = 1,
	RADIANT_PHY_COUNT
};

/* ---------------------------------------------------------------------------
 * CRC - expressed as a value, not a register. ANT's is CRC-16/CCITT-FALSE:
 * width 16, poly 0x1021 (x^16 term implicit, not included), init 0xFFFF, no
 * reflection, no final XOR, covering the on-air address bytes plus the body.
 * A backend whose engine can't cover the address (the usual sync-word-matcher
 * limitation) computes it in software and reports so via caps.crc_in_hw; a
 * delivered RADIANT_RADIO_STATUS_OK packet always has a verified CRC either way.
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
 * Advisory ceiling for statically sized body buffers in the core; the real
 * limit is caps.max_body_len.
 *
 * 40, not 32: ADR 0007's long-range format has a real length field and a
 * 4-byte header + 36 payload bytes, the first format to carry other than an
 * 8-byte ANT payload. Both 1 M ANT formats stay static-length at 10/12 bytes -
 * nothing ANT-shaped got longer, this constant grew only for the new
 * RadiANT-only format. The real duty-cycle bound is ADR 0007's 25%-of-period
 * rule, enforced by profile_sched_duty_check(); this is a buffer ceiling, not
 * a licence.
 */
#define RADIANT_RADIO_BODY_MAX 40

enum radiant_len_mode {
	/* Every frame in this format has exactly fmt->body_len body bytes. */
	RADIANT_LEN_FIXED = 0,
	/*
	 * body_len = body[len_offset] + len_bias. len_bias exists because a
	 * format's declared length and a radio's idea of body length may count
	 * header/CRC bytes differently.
	 *
	 * NO ANT FORMAT USES THIS MODE. Byte 3 of an ANT frame is a control
	 * byte (low five bits: 10 on broadcast, 2 in-slot, both 8 payload
	 * bytes), not a length byte (docs/spike-b-part2-results.md); a backend
	 * that maps ANT tracking onto this mode (nRF PCNF0.LFLEN=8) silently
	 * drops every acknowledged/burst frame. RADIANT_FRAME_CFG_LR (ADR 0007)
	 * is the one format that legitimately uses it: its length byte is
	 * written by this project on a PHY no stock receiver can demodulate, so
	 * there's nothing to infer and no ANT frame to misread.
	 */
	RADIANT_LEN_FROM_BODY = 1
};

/*
 * One on-air frame, from the receiver's point of view:
 *
 *   [preamble] [address: addr_len bytes] [body: body_len bytes] [CRC]
 *
 * Preamble is entirely the backend's business (PHY property). "Body" is every
 * byte between address and CRC, opaque to the HAL; how a backend implements it
 * internally must not change a byte on the air.
 *
 * The core uses a small set of static const formats (tracking/TX, search, ...)
 * because RAIL precompiles frame-length handling from a generated config, so
 * the expressible set is fixed at build time. A backend that cannot express a
 * format MUST fail the arm call with RADIANT_RADIO_ENOTSUP, never approximate.
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
 * One address the receiver should accept during a window. addr[0] is the
 * first byte on the air - no bit/byte-reversal, no register layout; the
 * backend does whatever its hardware needs (e.g. one part emits addresses
 * LSB-first regardless of frame endianness, so it bit-reverses internally).
 *
 * No wildcard field, deliberately: no planned backend can match "any device
 * number" in hardware, so wildcard search is core-level policy
 * (radiant_search.c) driven by caps.filter_wildcard_dev/max_filters, not a HAL
 * feature - e.g. 8 filters sweeps 32 sets, 2 filters sweeps 128.
 *
 * NOTE: that policy is about the 3-byte SEARCH format only. On the 5-byte
 * tracking format the device number is inside the nRF BASE, and a window
 * holds at most two distinct bases (caps.max_addr_groups) - 8 filters there is
 * 8 *addresses*, not 8 *device numbers*.
 *
 * The sweep lives in the core rather than per-backend so it stays *shared*:
 * one sweep serves every SEARCHING channel at once instead of ~8x slower.
 */
struct radiant_rx_filter {
	uint8_t addr[RADIANT_RADIO_ADDR_MAX];
	uint8_t addr_len;   /* must equal fmt->addr_len */
};

/* ---------------------------------------------------------------------------
 * t_sync
 * ---------------------------------------------------------------------------
 * DEFINITION: the absolute time the *last bit of the on-air address was at the
 * antenna* - not interrupt time, not packet end, not callback time. Every
 * backend corrects for its own demodulator/filter/capture latency so all
 * backends report the same instant for the same physical event. Chosen because
 * it's the earliest identifying instant, symmetric between TX and RX, and
 * independent of payload length/PHY (unlike packet-end, which moves with the
 * extension axes).
 *
 * TX: radiant_tx_req.t_sync_at is the target instant; the backend works
 * backwards through its own ramp-up/preamble/address airtime to decide when to
 * actually start, so a master's period is exact by construction and a
 * turnaround reply is a clean offset from the peer's t_sync.
 *
 * RX: a window [t_open, t_close] must catch any frame whose t_sync falls
 * inside it, so the backend must be listening early enough to have heard the
 * whole preamble+address of a frame completing exactly at t_open.
 *
 * CALIBRATION: each backend's correction constant is measured with a wired
 * two-board GPIO trigger against its own timestamp, per part and per PHY, and
 * recorded next to the constant with its date.
 *
 * FAILURE MODE IS SILENT: a *constant* t_sync error cancels out of the drift
 * PLL's period estimate (so it still locks) but shifts every window, eating
 * guard on one side - yield drops by a few tenths of a percent with no error
 * code, in the same range as the bench's ~0.4% collision floor, easy to
 * mistake for noise.
 *
 * OTHER CONSUMER: extended message 0xE0's RX timestamp MUST come from t_sync,
 * never a host/thread clock - it's the basis of the sub-ms multi-sensor fusion
 * claim. A backend with caps.has_sync_timestamp false must still fill t_sync
 * best-effort and clear t_sync_exact, so radiant_event.c can degrade the
 * advertised accuracy instead of silently reporting a worse number as good.
 */

/* ---------------------------------------------------------------------------
 * Operations
 * ---------------------------------------------------------------------------
 */

/*
 * Transmit one frame. body must remain valid and unmodified from the arm call
 * until the completion callback (backends DMA from it directly) and must live
 * in DMA-reachable RAM, not flash.
 *
 * addr is not part of body - body is only the bytes between address and CRC.
 */
struct radiant_tx_req {
	const struct radiant_pkt_format *fmt;
	uint8_t                      rf_index;
	struct radiant_tx_power          power;
	/*
	 * The on-air address, first byte first - same terms as
	 * struct radiant_rx_filter. Required: on the nRF RADIO, TXADDRESS
	 * indexes BASE/PREFIX registers left loaded by the PREVIOUS operation,
	 * so an unspecified address doesn't fail - it silently transmits under
	 * the last RX's device number, addressed to the wrong sensor.
	 *
	 * Inline rather than a pointer, unlike body: five bytes go into
	 * registers, not DMA, so there's no lifetime for the caller to get
	 * wrong.
	 */
	uint8_t                      addr[RADIANT_RADIO_ADDR_MAX];
	uint8_t                      addr_len;   /* must equal fmt->addr_len */
	const uint8_t               *body;
	uint8_t                      body_len;
	/* Requested t_sync of the frame. Must be at least caps.min_arm_lead_us
	 * in the future or the call fails RADIANT_RADIO_ETIME - a backend never
	 * transmits late, since a late master frame lands in the next slot.
	 */
	radiant_time_t                   t_sync_at;

	/*
	 * The core may arm one follow-on operation whose t_sync is no later
	 * than this many microseconds after t_sync_at. 0 = none. Ignored by a
	 * backend that owns the radio; exists for an arbitrated backend that
	 * must reserve the reply's air in the same ask as this frame's, because
	 * MPSL cannot issue a new request inside a granted timeslot without
	 * closing it and re-asking ~1.4 ms later - with no retry, a reply that
	 * misses that gap is simply lost.
	 *
	 * Supplied by the core, not inferred by the backend, because a backend
	 * must know nothing about ANT (rule 6) - inferring turnaround from
	 * `fmt` would be reading ANT semantics out of a pointer.
	 *
	 * A reservation, not a promise: the core often arms nothing. It costs
	 * an arbitrated backend scheduling pressure, not air time, provided the
	 * backend releases the reservation the moment it knows no follow-on is
	 * coming - required, not optional.
	 */
	uint16_t                     follow_on_us;
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
 * Multiple filters in one window is the point: since all ANT+ traffic is on
 * RF 57, radiant_sched.c can merge overlapping tracked channels' windows into
 * one hardware window carrying one filter per channel, so slave-side
 * inter-channel collisions drop to zero. The HAL only makes the merge
 * expressible; deciding what to merge is radiant_sched.c's job.
 *
 * Each match produces one rx_event; the operation ends with exactly one
 * terminal event (TIMEOUT if closed, ABORTED if cancelled). With
 * RADIANT_RX_STOP_ON_FIRST the first accepted frame's event is the terminal one.
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
	/*
	 * As struct radiant_tx_req::follow_on_us, measured from t_close.
	 *
	 * Unconditional on a tracked window, not conservatism: any tracked
	 * frame can become a transmit 1.56 ms later depending on a payload byte
	 * that doesn't exist yet when this request is built, so there's nothing
	 * to condition on. Zero on a search window, an ED sweep, or a master's
	 * turnaround - none of those can be followed by a transmit at a fixed
	 * instant.
	 */
	uint16_t                     follow_on_us;
};

/* ---------------------------------------------------------------------------
 * Energy detect
 * ---------------------------------------------------------------------------
 * Not the noise floor: rx_event.noise_dbm is an opportunistic sample from a
 * window the core opens anyway; this deliberately points the receiver at an
 * otherwise-unused frequency to measure it. Goes through the HAL rather than a
 * backend-private entry point because it costs radio time and must be
 * scheduled by radiant_sched.c like everything else.
 *
 * Shaped like a receive window deliberately: one arm call names an RF-index
 * range and a per-index dwell, walks it in order, delivers one OK event per
 * index and ends with one terminal event (TIMEOUT/ABORTED/FAILED) - same
 * "events then one terminal" contract the scheduler already knows.
 *
 * Reports min and mean per index rather than a percentile, because a backend
 * samples in an ISR with a fixed budget and cannot keep raw samples; the
 * min/mean gap shows how bursty that index was. Sample count is reported
 * because it varies with what a backend's hardware could take in the dwell.
 */

/*
 * Measure the band on rf_index_lo .. rf_index_hi, inclusive, ascending.
 *
 * dwell_us is an upper bound per index, not a duration to fill - a backend
 * that has taken every sample it can should leave early, but must never
 * exceed it, since the scheduler sized the gap from
 * (rf_index_hi - rf_index_lo + 1) * dwell_us with something committed after.
 *
 * t_start follows the same rule as radiant_rx_req.t_open: at least
 * caps.min_arm_lead_us in the future, or RADIANT_RADIO_ETIME.
 */
struct radiant_ed_req {
	uint8_t  rf_index_lo;   /* 0 .. RADIANT_RF_INDEX_MAX */
	uint8_t  rf_index_hi;   /* >= rf_index_lo, <= RADIANT_RF_INDEX_MAX */
	uint32_t dwell_us;      /* per index; must be non-zero */
	radiant_time_t t_start;
};

/* struct radiant_ed_event is defined with the other event types below, because
 * it needs enum radiant_radio_status. */

/* ---------------------------------------------------------------------------
 * Events
 * ---------------------------------------------------------------------------
 */

enum radiant_radio_status {
	RADIANT_RADIO_STATUS_OK = 0,       /* frame received, CRC verified */
	RADIANT_RADIO_STATUS_CRC_FAIL,     /* address matched, CRC did not */
	RADIANT_RADIO_STATUS_TIMEOUT,      /* window closed */
	RADIANT_RADIO_STATUS_ABORTED,      /* radiant_radio_abort() or a lifecycle call */
	RADIANT_RADIO_STATUS_FAILED,       /* the backend could not complete it */
	/*
	 * The operation was accepted and then never got the air (arbitrated
	 * backend only; see RADIANT_RADIO_EDENIED for the synchronous form).
	 * Says nothing about the peer, so radiant_channel.c widens its guard but
	 * does not raise RX_FAIL. Appended rather than inserted in "logical"
	 * order so existing bench/fake_radio traces keep their meaning.
	 */
	RADIANT_RADIO_STATUS_DENIED
};

struct radiant_rx_event {
	uint32_t              op;      /* the id the arm call returned */
	enum radiant_radio_status status;
	/* Valid when status is OK or CRC_FAIL. See the t_sync contract above. */
	radiant_time_t            t_sync;
	bool                  t_sync_exact;
	/*
	 * Which filter matched, indexing radiant_rx_req.filters. How the core
	 * recovers info the payload doesn't carry: in wildcard search the
	 * matched index recovers devnum_lo (the third address byte); in a
	 * merged window it says which channel the frame belongs to before any
	 * payload is parsed. Required on every OK and CRC_FAIL event.
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
	 * Valid only on CRC_FAIL with caps.has_rx_crc true; zero/has_crc_rx
	 * false otherwise, including OK (a passed frame's CRC is recomputable).
	 *
	 * `body` excludes the CRC bytes, so this is the only way to see the
	 * error syndrome (received CRC vs. recomputed) - what
	 * radiant_crc_repair.c uses to fix a single flipped bit, worth 1-3 dB
	 * of effective sensitivity.
	 *
	 * uint32_t because it's the raw register value (nRF RXCRC is 24 bits);
	 * only the low bits struct radiant_crc_cfg produces are meaningful.
	 */
	bool                  has_crc_rx;
	uint32_t              crc_rx;

	/*
	 * RSSI measured inside this window with no packet present - the noise
	 * floor. Valid only on a terminal TIMEOUT event for a window that
	 * delivered no frame. A backend that can't take such a sample leaves
	 * has_noise false.
	 *
	 * Not rssi_dbm under another name: rssi_dbm measures an arrived
	 * packet's transmitter, this measures the absence of one, and a window
	 * that received something must never report both. Same scale/reference
	 * as rssi_dbm so the two can be subtracted for a margin - a backend
	 * that corrected one and not the other would silently corrupt that
	 * margin.
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

/* One energy-detect index, or the end of a sweep. See "Energy detect" above. */
struct radiant_ed_event {
	uint32_t              op;
	enum radiant_radio_status status;

	/*
	 * Everything below is valid ONLY on RADIANT_RADIO_STATUS_OK, which is
	 * the per-index measurement event. The terminal event carries no
	 * measurement and leaves them zero - on the same terms, and for the
	 * same reason, as radiant_rx_event's frame fields on a TIMEOUT.
	 */
	uint8_t  rf_index;
	/* Quietest and average sample over this index's dwell, in dBm, on the
	 * SAME SCALE as rx_event.rssi_dbm and rx_event.noise_dbm - same
	 * corrections, same reference. A backend that applied an errata or
	 * temperature correction to one and not the others would produce a map
	 * that cannot be compared against the noise histogram, which is the one
	 * cross-check the map has. */
	int8_t   min_dbm;
	int8_t   mean_dbm;
	/* How many RSSI samples that dwell actually produced. Never zero on an
	 * OK event: a backend with nothing to report delivers no event for that
	 * index rather than an event claiming a measurement it did not make. */
	uint16_t samples;
};

/*
 * Callback and threading contract.
 *
 * Callbacks run in the backend's radio interrupt context, at its highest
 * priority - not a work queue, not a thread. This is load-bearing: re-arming
 * the next operation from inside the completion callback is the only path
 * that reliably meets an acknowledged-data turnaround.
 *
 * ONE AMENDMENT: a terminal RADIANT_RADIO_STATUS_DENIED event may instead be
 * delivered from a cooperative thread, because MPSL delivers its BLOCKED/
 * CANCELLED signals from mpsl_low_priority_process() and returning anything
 * but "no action" there would assert inside MPSL. Everything else about the
 * contract (no blocking, etc.) is unchanged for such an event - a consumer
 * must not rely on interrupt context for mutual exclusion anyway, since
 * radiant_event.c's critical section is taken by the poster regardless of
 * context. A backend that CAN deliver DENIED from the radio interrupt should;
 * this only permits a context, it doesn't prefer one.
 *
 * A callback MAY:
 *   - read the event (only for the duration of the call),
 *   - call radiant_radio_tx(), radiant_radio_rx(), radiant_radio_ed(),
 *     radiant_radio_abort(), radiant_radio_now() and radiant_radio_caps_get(),
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
	/*
	 * Energy detect. May be NULL, and is on any core built without
	 * CONFIG_RADIANT_ED_SCAN - a backend that finds it NULL must not
	 * call it, and must still refuse radiant_radio_ed() rather than arm an
	 * operation whose events have nowhere to go.
	 */
	void (*ed)(const struct radiant_ed_event *evt, void *user);
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
	 * "address groups". NOT max_filters, and assuming it is a real bug with
	 * a quiet signature: the nRF matcher's 8 logical addresses are logical
	 * 0 = BASE0+AP0 and 1..7 = BASE1+AP1..7, so a window carries at most
	 * TWO distinct bases (max_addr_groups = 2). That's fine for search
	 * (3-byte [A6 C5 devnum_lo]: one shared group, 8 filters, no
	 * constraint) but means a merged 5-byte tracking window ([A6 C5 dnl
	 * dnh] differs per sensor) holds only TWO channels on nRF, not eight.
	 * Equals max_filters on RAIL, where every filter is independent.
	 *
	 * 0 means "no constraint" (scheduler treats it as max_filters) for
	 * backward compatibility; a backend with the constraint that doesn't
	 * report it just gets a stream of RADIANT_RADIO_ENOTSUP instead.
	 */
	uint8_t max_addr_groups;

	/*
	 * Minimum Hamming distance, in bits, that the hardware-matched parts of
	 * two simultaneously-armed filters must differ by before the matcher
	 * can tell them apart. 0 or 1 means no constraint.
	 *
	 * Real, not theoretical: a sync-word correlator scores a sliding window
	 * against a template, so two templates one bit apart can score equally
	 * and the part may fire on neither. MEASURED on a CC26x2 at -47 dBm,
	 * 4.0049 fps: 1-bit apart received 0 of ~30 frames, 2-bit 3, 4-bit 18,
	 * 8-bit 20 (single sync word: 21-24). It's a deaf link, not a weak one,
	 * with no error at any layer - ordinary timeout, scheduler re-arms.
	 *
	 * radiant_search's default sweep hit this directly: pairing devnum_lo
	 * as consecutive integers (2k, 2k+1) put a 1-bit-apart pair in every
	 * set. Invisible on nRF, whose matcher has no such property.
	 *
	 * 0 keeps the consecutive pairing, so existing backends are unaffected.
	 */
	uint8_t min_filter_hamming_bits;

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
	 * compatibility. RADIANT_PHY_LR_CODED appends to this list. */
	const enum radiant_phy *phys;
	uint8_t             n_phys;
	/*
	 * Cost of switching between two of the above between operations. Zero
	 * where free; non-zero where it means reloading a generated config.
	 *
	 * A SCHEDULING cost, not an airtime one: radiant_sched.c spends it as
	 * extra arm lead only on the first operation after a PHY change, never
	 * on one that stays on the current PHY. Kept separate from
	 * min_arm_lead_us so a two-PHY build isn't charged this on every
	 * window, only the handful of times a second it actually switches.
	 */
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

	/*
	 * The same lead, for an operation that follows one already in flight -
	 * a reply, a turnaround, the next burst packet. Equal to min_arm_lead_us
	 * on a backend that owns the radio (may leave this 0 to say so); the
	 * two come apart only under an arbiter:
	 *
	 *   min_arm_lead_us          covers PLACING A RESERVATION that may be
	 *                            refused - on MPSL, ~1.7 ms measured grant
	 *                            latency (mostly HFXO startup) plus margin.
	 *   min_arm_lead_in_grant_us covers only programming the peripheral
	 *                            inside air ALREADY granted - no ask of
	 *                            the arbiter, just hardware setup/ramp-up.
	 *
	 * Load-bearing: radiant_burst.c refuses acknowledged data unless lead +
	 * ACK_GUARD_US fits inside REPLY_US (1310 us; an arbiter needs 2500).
	 * Checked against min_arm_lead_us that would wrongly disable ERG mode on
	 * a combined build - the reply doesn't need a fresh reservation because
	 * radiant_rx_req.follow_on_us already reserved its air. Consumers pick
	 * by whether an operation is already in flight: radiant_sched.c's
	 * arming pass (new work) uses min_arm_lead_us; radiant_burst.c and
	 * radiant_transfer.c (arming from inside a completion callback) use
	 * this one.
	 */
	uint16_t min_arm_lead_in_grant_us;

	/*
	 * How much longer than the FRAME ITSELF this backend keeps the receiver
	 * on after a window's t_close, in microseconds. Zero on a backend whose
	 * window really does end with the last frame it could still be hearing.
	 *
	 * Why any of this exists: t_close bounds a frame's t_SYNC, and t_sync is
	 * the end of the address, so every receive window occupies the radio
	 * past t_close by at least the body and CRC still to arrive. That part
	 * is airtime and radiant_sched.c computes it itself from the format
	 * (radiant_frame_tail_us()). THIS field is only the surplus a particular
	 * backend adds on top - e.g. a receiver whose end trigger stops sync
	 * SEARCH rather than reception needs run-up at the late edge or it
	 * misses exactly the frames the window was placed for.
	 *
	 * radiant_sched.c charges both to whatever fills a gap in front of a
	 * committed operation. Leaving the surplus unaccounted does not cost the
	 * gap filler, it costs the COMMITTED window - it is armed inside its own
	 * lead and hears nothing - which is worth every other packet on the
	 * tracked channel and is invisible until something else is searching.
	 *
	 * Not folded into min_arm_lead_us: that is what every operation pays to
	 * be armed, this is what ONE operation costs the one after it, and an
	 * energy-detect chunk pays neither.
	 */
	uint16_t rx_close_hold_us;

	/* Granularity of the underlying timebase, in nanoseconds. 1000 on a
	 * 1 MHz timer and on RAIL. Timestamps are still microseconds; this says
	 * how much of the last digit to believe. */
	uint16_t time_resolution_ns;

	/*
	 * Longest single receive or ED operation this backend can arm, in
	 * microseconds. 0 = unbounded (a backend that owns the radio).
	 *
	 * A capability, not a policy knob: on an arbitrated backend it's a hard
	 * API ceiling (MPSL_TIMESLOT_LENGTH_MAX_US = 100000), so a longer
	 * window cannot be requested at all. It must bound the REQUEST rather
	 * than have the backend silently run a shorter window: radiant_sched.c
	 * reports the bounds it asked for, and radiant_search.c credits the
	 * full dwell whenever a window ran to its requested close - a silently
	 * truncated grant would credit listening time that never happened.
	 *
	 * Not a substitute for yielding early: a backend that runs out of
	 * granted air mid-window ends it ABORTED, credited as time actually
	 * listened to. Denial, elasticity, and this ceiling are three separate
	 * signals. Transmits are unaffected - a transmit is an instant.
	 */
	uint32_t max_window_us;

	/* True if t_sync comes from a hardware capture of the address event
	 * rather than from an inference. When false, events carry
	 * t_sync_exact = false and the 0xE0 RX timestamp must be advertised as
	 * approximate. */
	bool has_sync_timestamp;

	/* True if rx_event.rssi_dbm is populated. */
	bool has_rssi;

	/*
	 * True if radiant_radio_ed() is implemented. False is ordinary, not
	 * degraded: such a backend just returns RADIANT_RADIO_ENOTSUP. Read by
	 * radiant_sched.c before it ever posts an ED chunk, so a backend
	 * without it costs one test rather than a stream of refusals.
	 */
	bool has_ed_scan;

	/* True if rx_event.crc_rx is populated on CRC_FAIL - the hardware kept
	 * the received CRC rather than only a pass/fail bit. The core's
	 * single-bit repair is arithmetic on that syndrome and simply doesn't
	 * run without it; a backend reporting true with a zero would
	 * manufacture repairs out of nothing. */
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
 * radiant/tests/fake_radio.c substitutes at link time - which is also what
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
 * Arm a transmit or a receive. At most one operation exists at a time: a
 * second arm call while one is armed or running returns RADIANT_RADIO_EBUSY.
 * Chain by arming the next operation from the completion callback.
 *
 * On success *op receives a monotonically increasing, non-zero id that every
 * event for this operation carries, so a late event from a cancelled
 * operation is recognisable rather than merely surprising.
 */
int radiant_radio_tx(const struct radiant_tx_req *req, uint32_t *op);
int radiant_radio_rx(const struct radiant_rx_req *req, uint32_t *op);

/*
 * Arm an energy-detect sweep. Occupies the single operation slot exactly as a
 * transmit or a receive does - there is one radio, and measuring the band is
 * one of the things it can be doing instead of listening to a sensor.
 *
 * RADIANT_RADIO_ENOTSUP where caps.has_ed_scan is false, or where the
 * installed callback table has no ed entry. RADIANT_RADIO_EINVAL for a range
 * running backwards, an index above RADIANT_RF_INDEX_MAX, or a zero dwell.
 * Otherwise EBUSY, ESTATE, ETIME on the same terms as the other two arm calls.
 */
int radiant_radio_ed(const struct radiant_ed_req *req, uint32_t *op);

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
