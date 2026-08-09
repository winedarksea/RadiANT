/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_search.h - wildcard search: the sweep, the seen cache, and scan mode.
 *
 * Provenance: clean-room. Written from docs/ant-radio-link.md (the search
 * packet configuration, the BALEN < 4 packing rule, the 32-set sweep and the
 * refutation of preamble-as-address), docs/spike-a-results.md and
 * docs/spike-b-results.md (the measurements those rest on: the noise floor of a
 * 3-byte matcher, RXMATCH recovery of devnum_lo, eight logical addresses in one
 * window), from radiant_core/include/radiant_core/radiant_radio_hal.h and radiant_core/include/radiant_core/radiant_frame.h,
 * and from the free ANT Message Protocol and Usage Rev 5.1 (D00000652) for the
 * wildcard channel-ID convention and the 25 s default search timeout. Nothing
 * here derives from sdk-ant, from libant.a, from disassembly of any binary, or
 * from any adopter-gated ANT+ device profile document. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this module is
 * ---------------------------------------------------------------------------
 * ANT asks a slave to find "any device of type 0x0B". No radio in view can do
 * that. The nRF's shortest matchable on-air address is three bytes, so the
 * third matched byte is unavoidably devnum_lo, and RAIL matches at most two
 * sync words at a time. Wildcard search is therefore not a HAL feature but
 * *core policy driven by a capability query*: enumerate caps.max_filters
 * concrete addresses per window and sweep enough sets to cover all 256 values
 * of that byte.
 *
 * On nRF that is 8 filters and 32 sets. On EFR32/RAIL it is 2 filters and 128
 * sets, from the same code. THE TWO NUMBERS ARE ARITHMETIC, NOT CONSTANTS.
 * radiant_search_filters_per_window() and radiant_search_sets() are public so a test
 * can assert them at both presets, because a hardcoded 8 is otherwise
 * invisible until somebody builds the second backend.
 *
 * ---------------------------------------------------------------------------
 * Four things this module gets right that a naive one does not
 * ---------------------------------------------------------------------------
 *
 * 1. CRC IS THE GATE, MATCH COUNT IS NOISE. A 3-byte address matcher fires on
 *    noise several times a second on a quiet bench; with eight filters armed
 *    Spike B measured 19, 22 and 27 CRC failures per 15 s window with the
 *    transmitter switched off - of order 1.4 per second. Every one was rejected
 *    by the CRC and none decoded to a plausible channel ID, and the RSSI column
 *    separated them from the real transmitter by about 70 dB. So an
 *    RADIANT_RADIO_STATUS_CRC_FAIL event NEVER contributes to an acquisition here.
 *    It is counted, and counting it is the whole of its use: a configuration
 *    whose only output is CRC errors has found the noise floor, not a sensor.
 *
 * 2. devnum_lo COMES FROM THE FILTER INDEX, NEVER FROM THE BODY. The matched
 *    address byte is consumed by the hardware and never reaches RAM. The core
 *    recovers it by indexing its OWN filter table with radiant_rx_event.filter_index
 *    (Spike B put the real device in slot 5 and decoys in the other seven, and
 *    RXMATCH read 5 on all 750 CRC-valid frames - slot 5 rather than slot 0
 *    precisely so a register stuck at zero could not have passed). A search
 *    frame's body is [devnum_hi][dtype][ttype][ctrl][d0..d7]: devnum_lo is not
 *    in it and cannot be read out of it.
 *
 * 3. ONE SWEEP SERVES EVERY SEARCHING CHANNEL. All ANT+ traffic is on RF 57,
 *    network 0, so a single window with every filter armed catches any matching
 *    packet, and the resulting channel ID is offered *in software* to every
 *    channel in RADIANT_SEARCH_MODE_ACQUIRE or _SCAN. Without this, eight
 *    simultaneous searches take ~64 s instead of ~8 s and tools/ant_session.py
 *    fails outright. It is not an optimisation; it is the difference between
 *    working and not.
 *
 * 4. A RECENTLY-SEEN CACHE. 16 entries, 60 s. When a channel starts searching
 *    for something the cache has seen, the set holding that devnum_lo is tried
 *    first, so a dropped sensor is re-acquired in about one dwell (~250 ms)
 *    rather than an average half-sweep (~4 s). Dropout recovery is the thing a
 *    rider actually notices, and it is thirty lines.
 *
 * ---------------------------------------------------------------------------
 * Dwell, and why it is one channel period
 * ---------------------------------------------------------------------------
 * A shorter dwell does not raise the per-transmission acquisition probability:
 * that is 1/n_sets either way, because a short window simply misses more
 * transmissions of the set it is on. A dwell equal to the channel period
 * *guarantees* the sensor transmits at least once while its set is selected, so
 * acquisition is certain within one full sweep rather than merely likely. On
 * nRF that is 32 x 260 ms = 8.3 s worst case, ~4 s average, comfortably inside
 * ANT's 25 s default search timeout.
 *
 * The dwell is accounted as *accumulated listening time on the current set*,
 * not as one window. That matters because the scheduler owns the radio and may
 * legitimately cut a search window short to service a tracked channel; crediting
 * only what was actually listened to keeps the "certain within one sweep"
 * guarantee under fragmentation instead of quietly degrading it to "likely".
 *
 * ---------------------------------------------------------------------------
 * This module never touches the radio
 * ---------------------------------------------------------------------------
 * The HAL permits exactly one operation in flight, so there is exactly one
 * arming authority, and it is radiant_sched.c. radiant_search.c is pure policy: it
 * produces a window request, is told what was armed, and consumes the events.
 * The only HAL function it calls is radiant_radio_caps_get(), once, in
 * radiant_search_init(). That is what lets the scheduler merge, delay, truncate or
 * refuse a search window without this module knowing or caring, and it is why
 * a search suite needs no scheduler.
 *
 * A consequence worth stating: there is no open-ended window. RADIANT_TIME_NEVER is
 * not a valid window edge, so background scan mode is not "arm once and leave
 * it"; it is the same bounded sweep with no timeout and no channel ever leaving
 * it. Scan mode was designed in rather than bolted on for exactly this reason.
 */

#ifndef RADIANT_SEARCH_H_
#define RADIANT_SEARCH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_radio_hal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Return codes
 *
 * Our own, for the reason the HAL's and radiant_frame.h's are their own: this
 * header must compile standalone against a freestanding toolchain and inside a
 * host unit test with no Zephyr present, and the tests that matter most are the
 * ones asserting *which* rejection happened.
 * ---------------------------------------------------------------------------
 */
#define RADIANT_SEARCH_OK        0
#define RADIANT_SEARCH_EINVAL  (-1)  /* null pointer or an out-of-range argument */
#define RADIANT_SEARCH_ENOSPC  (-2)  /* no free searching-channel slot */
#define RADIANT_SEARCH_ESTATE  (-3)  /* not initialised, or a window is already
				  * armed and has not terminated */
#define RADIANT_SEARCH_ENOENT  (-4)  /* nothing is searching, so no window is
				  * wanted. Not an error - the scheduler asks
				  * this question on every pass */
#define RADIANT_SEARCH_ENOTSUP (-5)  /* the backend advertises something this
				  * policy cannot express; see
				  * radiant_search_init() */

/* ---------------------------------------------------------------------------
 * Sizes
 *
 * All state is caller-provided (struct radiant_search below) and every array in it
 * is fixed. The module allocates nothing: on a dongle there is no allocator
 * worth trusting in an interrupt path, and in CI a leak checker that has
 * nothing to find is one fewer source of noise.
 * ---------------------------------------------------------------------------
 */

/* Channels that may be searching at once. 32 is the project ceiling - the
 * serial protocol's burst header uses the low five bits for the channel
 * number - and search is sized for it from the first line rather than
 * retrofitted, because a channel-count assumption threaded through a module is
 * far more expensive to remove later than to size correctly now. */
#ifndef RADIANT_SEARCH_MAX_CHANNELS
#define RADIANT_SEARCH_MAX_CHANNELS 32
#endif

/*
 * Storage for one window's filters. Eight is the nRF ceiling (one base address
 * plus eight prefixes) and therefore the largest any planned backend needs.
 *
 * This is a STORAGE bound, not the sweep width. The sweep width is
 * caps.max_filters, clamped to this; a backend advertising more would be
 * honoured up to this number and the sweep would simply be longer. Nothing in
 * this module computes anything from the literal 8.
 */
#ifndef RADIANT_SEARCH_MAX_FILTERS
#define RADIANT_SEARCH_MAX_FILTERS 8
#endif

/* Recently-seen ring. 16 entries covers a dense bench or a crowded gym rack
 * without making the linear scan through it interesting. */
#ifndef RADIANT_SEARCH_SEEN_ENTRIES
#define RADIANT_SEARCH_SEEN_ENTRIES 16
#endif

/* Values of devnum_lo the sweep must cover. It is the size of one byte, and it
 * is written as a name so that the arithmetic below reads as coverage rather
 * than as a magic 256. */
#define RADIANT_SEARCH_LO_VALUES 256u

/* Lifetime of a seen entry. 60 s is long enough to cover a sensor that dropped
 * out behind a rider's body and short enough that a stale entry does not steer
 * the sweep at a device that has left the room. */
#define RADIANT_SEARCH_SEEN_LIFETIME_US 60000000u

/* One ANT+ channel period (8182/32768 s = 249.7 ms) plus guards. See the dwell
 * discussion in the header comment: this is not a tuning parameter with a range
 * of reasonable values, it is "one channel period", and shortening it buys
 * nothing while losing the within-one-sweep guarantee. */
#define RADIANT_SEARCH_DWELL_DEFAULT_US 260000u

/* ANT's default search timeout: 10 counts of 2.5 s [rev5.1]. */
#define RADIANT_SEARCH_TIMEOUT_DEFAULT_US 25000000u

/* Passed as timeout_us to radiant_search_begin() for a search that never expires.
 * What background scan mode uses, and what ANT calls an infinite search. */
#define RADIANT_SEARCH_TIMEOUT_NONE 0u

/* ---------------------------------------------------------------------------
 * What a channel is looking for
 * ---------------------------------------------------------------------------
 */

/*
 * ANT's wildcard convention: zero means "any" in each of the three fields
 * [rev5.1]. A pure wildcard search is therefore all three zero, and pairing to
 * a specific strap is device_number set with the other two zero.
 *
 * device_type is compared with the pairing bit masked off, because the pairing
 * bit is a property of the *transmission* rather than of the device type and a
 * search that failed to match a sensor in pairing mode would fail at exactly
 * the moment it is most wanted. The bit is not discarded - the raw on-air byte
 * reaches the caller in radiant_search_result.id.device_type, so a channel that
 * wants to know whether the sensor was pairing can see it.
 */
struct radiant_search_id_filter {
	uint16_t device_number; /* 0 = any */
	uint8_t  device_type;   /* 0 = any; compared without the pairing bit */
	uint8_t  trans_type;    /* 0 = any */
};

enum radiant_search_mode {
	/*
	 * The ANT default. The first matching frame is offered to the channel
	 * and the channel then LEAVES the search - it has a channel ID now and
	 * belongs to the scheduler's tracked set. It may call
	 * radiant_search_begin() again if it loses the sensor, which is the path
	 * the seen cache makes fast.
	 */
	RADIANT_SEARCH_MODE_ACQUIRE = 0,
	/*
	 * Background scan. Every matching frame is reported, the channel never
	 * leaves, and the sweep runs until something ends it. libant.a
	 * advertises this mode and never implemented it; radiant_core implements it,
	 * and the sweep is the natural shape for it because a scanning receiver
	 * wants to hear *everything* rather than to lock onto one master.
	 *
	 * Scan mode is also what the telemetry envelope's sparse nodes require:
	 * a node that transmits on change rather than periodically cannot be
	 * tracked with a predicted window, and only a scanning receiver hears
	 * it. That dependency runs in this direction and is worth knowing when
	 * changing anything here.
	 */
	RADIANT_SEARCH_MODE_SCAN = 1
};

/* ---------------------------------------------------------------------------
 * What comes back
 * ---------------------------------------------------------------------------
 */

struct radiant_search_result {
	/* device_number's low byte comes from the filter index and its high
	 * byte from the body; device_type carries the pairing bit exactly as it
	 * was on the air. */
	struct radiant_channel_id id;

	/* The whole acquiring frame, control byte and payload included. ANT
	 * delivers the first broadcast along with the channel ID on
	 * acquisition, and a channel that had to ask for it again would waste a
	 * period. */
	struct radiant_frame frame;

	radiant_time_t t_sync;
	bool       t_sync_exact;
	bool       has_rssi;
	int8_t     rssi_dbm;

	/* Which sweep set caught it and which filter within that set. Together
	 * they are the whole provenance of devnum_lo, which is why they are
	 * reported rather than kept private. */
	uint16_t set_index;
	uint8_t  filter_index;

	/* True if the window that caught it had been steered by the seen
	 * cache. The figure that says whether the cache is earning its 16
	 * entries. */
	bool from_cache;
};

/*
 * Delivery.
 *
 * BOTH CALLBACKS RUN IN THE RADIO INTERRUPT. acquired() is called from
 * radiant_search_on_rx(), which the scheduler calls from the HAL's rx callback, and
 * it therefore inherits every MUST NOT in the callback contract in
 * radiant_radio_hal.h: it must not block, must not take a mutex, must not retain
 * anything past its return, and must not do work proportional to anything.
 * Queue it and return. timeout() is called from radiant_search_tick() and is
 * thread context, but writing it to the same rules costs nothing and means the
 * two do not have to be reasoned about separately.
 *
 * result is valid only for the duration of the call. Copy what must outlive it.
 */
struct radiant_search_cbs {
	void (*acquired)(uint8_t channel, const struct radiant_search_result *r,
			 void *user);
	void (*timeout)(uint8_t channel, void *user);
};

/* ---------------------------------------------------------------------------
 * Configuration
 * ---------------------------------------------------------------------------
 */
struct radiant_search_cfg {
	/* The two network-address bytes, first on air first. ant_net.c owns the
	 * key -> address table; this module takes the answer as an argument so
	 * the pairing is written down in exactly one place. */
	uint8_t net_addr[RADIANT_NET_ADDR_LEN];

	/* 2400 + N MHz. ANT+ is RADIANT_RF_INDEX_ANT_PLUS, and that every channel
	 * shares it is precisely what makes one sweep serve all of them. */
	uint8_t rf_index;

	/* Listening time per sweep set. See the header comment. */
	uint32_t dwell_us;

	/* Seen-entry lifetime. */
	uint32_t seen_lifetime_us;

	/*
	 * Reject frames weaker than this, for ANT's proximity pairing - "pair
	 * with the strap I am holding, not the one on the next bike".
	 * 0 disables it, which is the default and the only setting a normal
	 * search should use.
	 *
	 * This is a threshold on a CRC-VALID frame. It is not, and must never
	 * become, a substitute for the CRC gate: the noise triggers a 3-byte
	 * matcher produces sit 70 dB down, so an RSSI floor would filter most
	 * of them and thereby hide the ones it did not.
	 */
	int8_t min_rssi_dbm;

	/*
	 * Ask the backend for RADIANT_RADIO_STATUS_CRC_FAIL events too. Off by
	 * default: the core does not need them to work, and on a search window
	 * they arrive at over one a second. Turn them on for the RX_FAIL
	 * accounting that makes "unexplained loss" a meaningful gate, and for
	 * the test that proves noise never acquires - which is the single most
	 * important test in this module and cannot be written without them.
	 */
	bool report_crc_fail;
};

/* ANT+ defaults: the A6 C5 network, RF 57, one channel period of dwell, a 60 s
 * cache, no RSSI floor, CRC failures not reported. Start from this and change
 * what you mean to change. */
void radiant_search_cfg_default(struct radiant_search_cfg *cfg);

/* ---------------------------------------------------------------------------
 * The window this module wants the scheduler to arm
 * ---------------------------------------------------------------------------
 */
struct radiant_search_window {
	const struct radiant_pkt_format *fmt;
	uint8_t                      rf_index;

	/*
	 * Points INTO the struct radiant_search that produced it, which is what the
	 * HAL requires: filters must stay valid until the terminal event. It is
	 * invalidated by the next radiant_search_window() call on the same
	 * instance, and there can only be one window in flight, so that is not
	 * a constraint anyone can trip over accidentally.
	 */
	const struct radiant_rx_filter *filters;
	uint8_t                     n_filters;

	radiant_time_t t_open;
	radiant_time_t t_close;

	/*
	 * RADIANT_RX_REPORT_CRC_FAIL if cfg.report_crc_fail. Never
	 * RADIANT_RX_STOP_ON_FIRST: the HAL forbids it on a search window and the
	 * reason is this module's whole point - several masters may transmit
	 * inside one window, and stopping at the first would hand one channel
	 * an acquisition while silently costing every other searching channel
	 * the rest of the dwell.
	 */
	uint32_t flags;

	uint16_t set_index;
	bool     from_cache;
};

/* ---------------------------------------------------------------------------
 * Counters
 *
 * These exist to be asserted on. crc_fail next to frames_ok is the pair that
 * says whether a configuration is hearing a transmitter or hearing the noise
 * floor, and it is the pair Spike A and Spike B were read on.
 * ---------------------------------------------------------------------------
 */
struct radiant_search_stats {
	uint32_t windows;         /* windows handed to the scheduler */
	uint32_t sets_advanced;   /* dwell completed, moved to the next set */
	uint32_t sweeps;          /* round-robin cursor wrapped */
	uint32_t frames_ok;       /* CRC-valid frames decoded */
	uint32_t crc_fail;        /* address matched, CRC did not: the noise */
	uint32_t rssi_rejected;   /* CRC-valid but below cfg.min_rssi_dbm */
	uint32_t decode_failed;   /* CRC-valid and still not an ANT frame */
	uint32_t bad_filter_index;/* index outside the filters we supplied - a
				   * backend bug, and one worth seeing */
	uint32_t late_events;     /* an event for an operation we no longer own */
	uint32_t acquired;        /* channels that left the search with an ID */
	uint32_t scan_reports;    /* frames delivered to scanning channels */
	uint32_t timeouts;        /* channels that gave up */
	/* Windows ordered ahead of the round-robin cursor, either by the seen
	 * cache or by a channel that named its device number outright. Both are
	 * the same mechanism and both are the reason re-acquisition is fast, so
	 * they are counted together. */
	uint32_t cache_steers;
	uint32_t cache_acquires;  /* acquisitions on one of those windows */
};

/* ---------------------------------------------------------------------------
 * State
 *
 * Public so the caller can place it. Read the counters through
 * radiant_search_stats(); everything else in here is private and may be
 * rearranged.
 * ---------------------------------------------------------------------------
 */

struct radiant_search_chan {
	bool                        active;
	enum radiant_search_mode        mode;
	struct radiant_search_id_filter want;
	radiant_time_t                  deadline; /* RADIANT_TIME_NEVER = no timeout */
};

struct radiant_search_seen {
	bool                  valid;
	struct radiant_channel_id id;
	radiant_time_t            t_seen;
};

struct radiant_search {
	bool                         inited;
	struct radiant_search_cfg        cfg;
	struct radiant_search_cbs        cbs;
	void                        *user;
	const struct radiant_pkt_format *fmt;

	/* Sweep geometry, computed once from caps. */
	uint8_t  n_filters;
	uint16_t n_sets;

	/* The set currently selected, and the filters that express it. */
	struct radiant_rx_filter filters[RADIANT_SEARCH_MAX_FILTERS];
	uint8_t              filter_lo[RADIANT_SEARCH_MAX_FILTERS];
	uint8_t              cur_n_filters;
	uint16_t             cur_set;
	bool                 cur_from_cache;
	bool                 set_valid;
	uint32_t             set_dwell_us; /* listening credited to cur_set */

	/* The window in flight, if any. */
	uint32_t   op;
	radiant_time_t t_open;
	radiant_time_t t_close;

	/* Round-robin cursor, and the cache's queue-jumpers ahead of it. */
	uint16_t next_set;
	uint16_t steer[RADIANT_SEARCH_SEEN_ENTRIES];
	uint8_t  n_steer;

	struct radiant_search_chan chans[RADIANT_SEARCH_MAX_CHANNELS];
	struct radiant_search_seen seen[RADIANT_SEARCH_SEEN_ENTRIES];
	uint8_t                seen_next;

	struct radiant_search_stats stats;
};

/* ---------------------------------------------------------------------------
 * Lifecycle
 * ---------------------------------------------------------------------------
 */

/*
 * Zero the instance and compute the sweep geometry from radiant_radio_caps_get().
 *
 * cbs may be NULL, and either function in it may be NULL, for a caller that
 * only reads the counters.
 *
 * Caps are read ONCE. The HAL says the caps pointer is static for the lifetime
 * of the program and no backend changes its own capabilities at run time, so a
 * snapshot is honest; a test that wants the RAIL geometry calls
 * fake_radio_caps_preset_rail() before this, not after.
 *
 * RADIANT_SEARCH_ENOTSUP if caps.filter_wildcard_dev is set. That flag says the
 * backend can match "any device number" in one filter, which would collapse the
 * sweep to a single window - but struct radiant_rx_filter has no field in which to
 * ASK for a wildcard, so there is nothing this module could arm. Failing loudly
 * is the correct response to a capability the contract cannot express: the fix
 * is a HAL change, and a policy module quietly ignoring the flag would hide the
 * need for it. No planned backend sets it.
 */
int radiant_search_init(struct radiant_search *s, const struct radiant_search_cfg *cfg,
		    const struct radiant_search_cbs *cbs, void *user);

/* Filters per window: caps.max_filters, clamped to RADIANT_SEARCH_MAX_FILTERS. */
uint8_t radiant_search_filters_per_window(const struct radiant_search *s);

/* Sets needed to cover all 256 values of devnum_lo. 32 at eight filters, 128
 * at two. Assert this at both, or a hardcoded 8 will never be found. */
uint16_t radiant_search_sets(const struct radiant_search *s);

/* Worst-case time to cover every device number: sets x dwell. 8.3 s on nRF,
 * 33 s on RAIL - which is outside ANT's 25 s default timeout, and is a fact a
 * RAIL backend has to answer for rather than a bug in this module. */
uint64_t radiant_search_sweep_us(const struct radiant_search *s);

/* ---------------------------------------------------------------------------
 * Channels joining and leaving the search
 * ---------------------------------------------------------------------------
 */

/*
 * Put a channel into the search.
 *
 * timeout_us is RADIANT_SEARCH_TIMEOUT_NONE for no expiry, or a duration from now;
 * RADIANT_SEARCH_TIMEOUT_DEFAULT_US is the 25 s the protocol defaults to. The
 * deadline is only examined by radiant_search_tick().
 *
 * If the seen cache holds a device matching want, the set containing its
 * devnum_lo is queued ahead of the round-robin cursor. That is the whole
 * mechanism of fast re-acquisition, and it is why a channel that loses a sensor
 * should call this again rather than the caller inventing a retry path.
 *
 * RADIANT_SEARCH_EINVAL for a null filter or an unknown mode; RADIANT_SEARCH_ENOSPC for
 * a channel number this build was not sized for (see RADIANT_SEARCH_MAX_CHANNELS).
 *
 * Re-issuing for a channel already searching REPLACES its filter, mode and
 * deadline in place rather than failing. That is what a host changing its mind
 * looks like, and a channel state machine re-entering SEARCHING after losing a
 * sensor takes the same path - which is exactly the case the cache steering
 * below is for.
 */
int radiant_search_begin(struct radiant_search *s, uint8_t channel,
		     enum radiant_search_mode mode,
		     const struct radiant_search_id_filter *want, radiant_time_t now,
		     uint32_t timeout_us);

/* Take a channel out of the search. Idempotent; RADIANT_SEARCH_ENOENT if it was not
 * in one. Does not touch the window in flight - the scheduler decides whether
 * to let it run out or abort it, because only the scheduler knows what else
 * wants the radio. */
int radiant_search_end(struct radiant_search *s, uint8_t channel);

bool    radiant_search_is_searching(const struct radiant_search *s, uint8_t channel);
uint8_t radiant_search_n_searching(const struct radiant_search *s);

/* ---------------------------------------------------------------------------
 * The pump - what the scheduler calls
 * ---------------------------------------------------------------------------
 */

/*
 * Ask for the next search window.
 *
 * t_earliest is the earliest instant the caller can actually BE RECEIVING by -
 * the scheduler has already added caps.min_arm_lead_us and whatever else it
 * knows about. This module has no business knowing the arming lead of a backend
 * it never calls, and a window whose t_open the HAL rejects with
 * RADIANT_RADIO_ETIME would be this module's fault under any other convention.
 *
 * Returns RADIANT_SEARCH_OK and fills out, or RADIANT_SEARCH_ENOENT if nothing is
 * searching, or RADIANT_SEARCH_ESTATE if a window is already in flight.
 *
 * The set advances only when the dwell has been fully credited, so calling this
 * again after a truncated window continues the SAME set for the remaining time
 * rather than skipping ahead. t_close is t_earliest plus the remaining dwell.
 */
int radiant_search_window(struct radiant_search *s, radiant_time_t t_earliest,
		      struct radiant_search_window *out);

/*
 * Passed as op to radiant_search_armed() by an arming authority that does not
 * expose a HAL operation id - which radiant_sched.c does not, because it merges
 * several channels' requests into one hardware operation and hands each
 * channel back only its own events. With this token the module stops filtering
 * events by op and trusts the caller's routing, which is correct precisely
 * because the caller is the thing that did the routing.
 */
#define RADIANT_SEARCH_OP_EXTERNAL ((uint32_t)0xFFFFFFFFu)

/*
 * The window was armed: op is what radiant_radio_rx() returned, or
 * RADIANT_SEARCH_OP_EXTERNAL, and t_open / t_close are what was actually asked for,
 * which need not be what radiant_search_window() proposed. The dwell is credited
 * from these, not from the proposal.
 *
 * A caller running the sweep as one continuous scheduler request that re-arms
 * in bounded chunks calls this once per chunk with that chunk's bounds. This
 * module's unit of accounting is a dwell, not an operation.
 */
void radiant_search_armed(struct radiant_search *s, uint32_t op, radiant_time_t t_open,
		      radiant_time_t t_close);

/* The scheduler could not arm it after all - EBUSY, ETIME, or it changed its
 * mind. Nothing is credited and the same set is offered again. */
void radiant_search_arm_failed(struct radiant_search *s);

/* True if evt->op belongs to the window this module has in flight. How the
 * scheduler routes an event without keeping its own map. */
bool radiant_search_owns_op(const struct radiant_search *s, uint32_t op);

/*
 * One radio event. RUNS IN THE RADIO INTERRUPT; see struct radiant_search_cbs.
 *
 * Events for any other operation are counted as late and ignored, which is what
 * the op id exists for: on real hardware a frame already in the pipeline when
 * abort() ran arrives after the core moved on.
 *
 * RADIANT_RADIO_STATUS_CRC_FAIL is counted and DISCARDED. It is never evidence of
 * anything but the noise floor.
 */
void radiant_search_on_rx(struct radiant_search *s, const struct radiant_rx_event *evt);

/*
 * The same, with the filter index supplied separately.
 *
 * This exists for one specific and load-bearing reason. A scheduler that merges
 * requests into one hardware window must remap the HAL's filter_index - which
 * indexes the MERGED array - back into the index of the requesting channel's
 * own filters, and it hands that remapped value over beside the event rather
 * than rewriting a const event in an interrupt. Passing evt->filter_index to a
 * merged window would recover the wrong devnum_lo, silently and plausibly: the
 * acquired device number would be a real one, just not the one that
 * transmitted. Use this from radiant_sched.c; use radiant_search_on_rx() when the
 * module has the radio to itself.
 */
void radiant_search_on_rx_indexed(struct radiant_search *s, const struct radiant_rx_event *evt,
			      uint8_t filter_index);

/*
 * The window in flight has finished. ran_to_close credits the dwell; anything
 * else - aborted, failed, taken back by the scheduler - credits nothing, so the
 * same set is listened to again. Called for you by radiant_search_on_rx() on a
 * terminal event; called directly by a scheduler whose completion notification
 * is not a HAL event.
 */
void radiant_search_on_done(struct radiant_search *s, bool ran_to_close);

/*
 * Expire seen entries and fire the timeout callback for any channel past its
 * deadline. Thread context. The scheduler should call it whenever it services
 * the search and at least once per dwell; nothing breaks if it is late, the
 * timeout simply fires late.
 */
void radiant_search_tick(struct radiant_search *s, radiant_time_t now);

/* ---------------------------------------------------------------------------
 * The seen cache, exposed
 *
 * Public because the interesting assertions are about it directly - that it
 * steers, that it expires at 60 s, that a stale entry does not survive - and
 * inferring all of that from acquisition latency alone would make the test both
 * slower and weaker.
 * ---------------------------------------------------------------------------
 */

/* Record a device. Called internally for every CRC-valid frame; public so
 * radiant_channel.c can also note a sensor it is tracking, which is what makes the
 * cache useful at the moment that sensor drops. */
void radiant_search_seen_note(struct radiant_search *s, const struct radiant_channel_id *id,
			  radiant_time_t now);

/* The most recently seen unexpired device matching want, if any. */
bool radiant_search_seen_lookup(const struct radiant_search *s,
			    const struct radiant_search_id_filter *want,
			    radiant_time_t now, struct radiant_channel_id *out);

/* Unexpired entries at time now. */
uint8_t radiant_search_seen_count(const struct radiant_search *s, radiant_time_t now);

/* The set that would catch a given device number, i.e. the one holding its low
 * byte. RADIANT_SEARCH_SETS_NONE if the instance is not initialised. */
#define RADIANT_SEARCH_SETS_NONE ((uint16_t)0xFFFFu)
uint16_t radiant_search_set_for_devnum(const struct radiant_search *s, uint16_t devnum);

const struct radiant_search_stats *radiant_search_get_stats(const struct radiant_search *s);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_SEARCH_H_ */
