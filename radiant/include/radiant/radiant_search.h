/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_search.h - wildcard search: the sweep, the seen cache, and scan mode.
 *
 * Provenance: clean-room, from docs/ant-radio-link.md (search packet config,
 * BALEN < 4 packing rule, 32-set sweep, refutation of preamble-as-address),
 * docs/spike-a-results.md and docs/spike-b-results.md (3-byte matcher noise
 * floor, RXMATCH recovery of devnum_lo, eight logical addresses in one
 * window), radiant_radio_hal.h, radiant_frame.h, and the free ANT Message
 * Protocol and Usage Rev 5.1 (D00000652) for the wildcard channel-ID
 * convention and the 25 s default timeout. Nothing here derives from sdk-ant,
 * libant.a, or any ANT+ device profile document. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ANT asks a slave to find "any device of type 0x0B", which no radio here can
 * match in hardware: the shortest matchable address is 3 bytes (third byte
 * unavoidably devnum_lo) and RAIL matches at most two sync words at once. So
 * wildcard search is core policy, not a HAL feature: enumerate
 * caps.max_filters concrete addresses per window and sweep enough sets to
 * cover all 256 values of devnum_lo - 8 filters/32 sets on nRF, 2/128 on
 * EFR32/RAIL, same code. radiant_search_filters_per_window()/_sets() are
 * public so a test can assert both, since a hardcoded 8 is otherwise
 * invisible until a second backend exists.
 *
 * ---------------------------------------------------------------------------
 * Four things this module gets right
 * ---------------------------------------------------------------------------
 * 1. CRC is the gate, match count is noise. Spike B measured ~1.4 CRC
 *    failures/sec on a quiet bench with 8 filters armed and the transmitter
 *    off, all ~70 dB weaker than the real signal - a CRC_FAIL event never
 *    contributes to acquisition, only to a noise-floor count.
 * 2. devnum_lo comes from the filter index, never the body: the matched
 *    address byte is consumed by hardware and never reaches RAM (Spike B put
 *    the real device in slot 5, decoys elsewhere, and RXMATCH read 5 on all
 *    750 CRC-valid frames). A search frame's body has no devnum_lo in it.
 * 3. One sweep serves every searching channel: all ANT+ traffic is on RF 57,
 *    so one window's result is offered in software to every ACQUIRE/SCAN
 *    channel - without this, 8 simultaneous searches take ~64 s not ~8 s.
 * 4. A 16-entry, 60 s recently-seen cache: a channel searching for something
 *    the cache holds tries that set first, cutting re-acquisition from an
 *    average half-sweep (~4 s) to about one dwell (~250 ms).
 *
 * ---------------------------------------------------------------------------
 * Dwell = one channel period
 * ---------------------------------------------------------------------------
 * A shorter dwell doesn't raise per-transmission acquisition probability
 * (still 1/n_sets); a dwell equal to the channel period guarantees the sensor
 * transmits at least once while its set is selected, making acquisition
 * certain within one sweep rather than merely likely (32 x 260 ms = 8.3 s
 * worst case on nRF, ~4 s average, well inside ANT's 25 s timeout).
 *
 * Dwell is accounted as accumulated listening time on the current set, not as
 * one window, because the scheduler may cut a search window short for a
 * tracked channel - crediting only actual listening time keeps "certain
 * within one sweep" true under fragmentation.
 *
 * ---------------------------------------------------------------------------
 * This module never touches the radio
 * ---------------------------------------------------------------------------
 * radiant_sched.c is the sole arming authority (the HAL permits one operation
 * in flight); radiant_search.c is pure policy - it produces a window request,
 * is told what was armed, and consumes events. Its only HAL call is
 * radiant_radio_caps_get(), once, in radiant_search_init(). That's what lets
 * the scheduler merge/delay/truncate/refuse a window without this module
 * knowing, and why a search suite needs no scheduler.
 *
 * Consequence: no open-ended window. RADIANT_TIME_NEVER isn't a valid window
 * edge, so background scan mode is the same bounded sweep with no timeout and
 * no channel ever leaving - designed in, not bolted on.
 */

#ifndef RADIANT_SEARCH_H_
#define RADIANT_SEARCH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant/radiant_frame.h>
#include <radiant/radiant_radio_hal.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Return codes - our own, freestanding (must compile with no Zephyr present),
 * because the tests that matter most assert *which* rejection happened.
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
 * Sizes - all state is caller-provided (struct radiant_search) with fixed
 * arrays; the module allocates nothing.
 * ---------------------------------------------------------------------------
 */

/* Channels that may be searching at once. 32 is the project ceiling (serial
 * protocol's burst header, low five bits), sized in from the start. */
#ifndef RADIANT_SEARCH_MAX_CHANNELS
#define RADIANT_SEARCH_MAX_CHANNELS 32
#endif

/*
 * Storage for one window's filters. Eight is the nRF ceiling (one base
 * address plus eight prefixes), a STORAGE bound only - the sweep width is
 * caps.max_filters clamped to this. Nothing here computes from the literal 8.
 */
#ifndef RADIANT_SEARCH_MAX_FILTERS
#define RADIANT_SEARCH_MAX_FILTERS 8
#endif

/* Recently-seen ring. 16 entries covers a dense bench or crowded gym rack. */
#ifndef RADIANT_SEARCH_SEEN_ENTRIES
#define RADIANT_SEARCH_SEEN_ENTRIES 16
#endif

/* Values of devnum_lo the sweep must cover - one byte, named for clarity. */
#define RADIANT_SEARCH_LO_VALUES 256u

/* Lifetime of a seen entry. 60 s covers a sensor dropped behind a rider's
 * body without steering the sweep at a device that has left the room. */
#define RADIANT_SEARCH_SEEN_LIFETIME_US 60000000u

/* One ANT+ channel period (8182/32768 s = 249.7 ms) plus guards. Not a
 * tunable: see the dwell discussion above - shortening it loses the
 * within-one-sweep guarantee for no gain. */
#define RADIANT_SEARCH_DWELL_DEFAULT_US 260000u

/*
 * THE RESIDUE THAT ENDS A SET. A set with less than this still owed is
 * FINISHED, or the sweep can freeze a second way: without it, a dwindling
 * remainder (e.g. 260 ms budget minus two ~94 ms chunks) gets armed as a
 * chunk of a few hundred microseconds - too short to hear a 4 Hz master and,
 * on an arbitrated backend, likely refused outright, crediting zero. The set
 * then sits fractionally short forever, since only a new set resets the
 * ceiling.
 *
 * MEASURED on the nRF54L15 beside a 1 s BLE advertiser: chunks collapsed to
 * 3378 us, 11339 of them in 55 s, sets_advanced stuck at 1, frames_ok 0 - vs.
 * immediate acquisition with the advertiser off. MPSL was granting 97.7% of
 * what was asked; the starvation was self-inflicted. Same freeze as the
 * dwell-is-a-budget note below, reached by a different route.
 *
 * 5 ms is a choice: comfortably longer than one chunk's arm/teardown cost,
 * and only 2% of the budget.
 */
#define RADIANT_SEARCH_DWELL_EPSILON_US 5000u

/*
 * THE DWELL IS A BUDGET, NOT A SINGLE WINDOW - load-bearing, since
 * radiant_search_on_done() credits time rather than counting windows.
 *
 * A tracked slave demands the radio every 249.7 ms, so a 260 ms search window
 * can never reach its close and gets preempted (RADIANT_SCHED_DONE_ABORTED).
 * Until 2026-08-10 an abort credited nothing, so set_dwell_us never filled and
 * THE SWEEP FROZE on whichever set it was on - every device outside that set
 * was undiscoverable while anything was tracking, i.e. every real session.
 * MEASURED: a -25 dBm transmitter in set 2 was invisible to a 60 s wildcard
 * scan while one in set 1 was found every time (Zwift saw one sensor only).
 * Fix: credit the time actually listened to.
 *
 * CHUNKING THE DWELL WAS TRIED AND REJECTED: capping windows at a fraction of
 * the dwell unfreezes the sweep too, but a short window only catches a 4 Hz
 * sensor when it happens to align, silently degrading "certain within one
 * sweep" to "likely" for no gain. Seven tests in test_search.c encode that
 * guarantee and fail if the cap comes back.
 */

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
 * ANT's wildcard convention: zero means "any" in each field [rev5.1]. A pure
 * wildcard search is all three zero; pairing to a specific strap is
 * device_number set with the other two zero.
 *
 * device_type is compared with the pairing bit masked off, since that bit is
 * a property of the transmission, not the device type, and a search must not
 * fail to match a sensor that's actively pairing. The raw byte still reaches
 * the caller via radiant_search_result.id.device_type.
 */
struct radiant_search_id_filter {
	uint16_t device_number; /* 0 = any */
	uint8_t  device_type;   /* 0 = any; compared without the pairing bit */
	uint8_t  trans_type;    /* 0 = any */
};

enum radiant_search_mode {
	/*
	 * The ANT default. The first matching frame is offered to the channel,
	 * which then LEAVES the search and joins the scheduler's tracked set.
	 * May call radiant_search_begin() again on loss - fast via the seen
	 * cache.
	 */
	RADIANT_SEARCH_MODE_ACQUIRE = 0,
	/*
	 * Background scan. Every matching frame is reported, the channel never
	 * leaves. libant.a advertised this mode and never implemented it;
	 * radiant does. Also what the telemetry envelope's sparse nodes
	 * require: a node that transmits on change rather than periodically
	 * can't be tracked with a predicted window, only heard by scanning.
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
 * Delivery. Both callbacks run under the radio-interrupt callback contract
 * from radiant_radio_hal.h (no blocking, no mutex, no retaining past return,
 * no proportional work) - acquired() genuinely runs there via
 * radiant_search_on_rx(); timeout() is thread context via
 * radiant_search_tick() but follows the same rules so the two need no
 * separate reasoning.
 *
 * result is valid only for the duration of the call.
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
	 * Reject frames weaker than this, for ANT's proximity pairing ("this
	 * strap, not the one on the next bike"). 0 disables it (default).
	 * A threshold on a CRC-VALID frame only - must never substitute for the
	 * CRC gate, since a 3-byte matcher's noise sits 70 dB down and an RSSI
	 * floor would hide it rather than reject it cleanly.
	 */
	int8_t min_rssi_dbm;

	/*
	 * Ask the backend for CRC_FAIL events too. Off by default (they arrive
	 * >1/sec on a search window); turn on for RX_FAIL accounting and for
	 * the test proving noise never acquires.
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
	 * Points INTO the struct radiant_search that produced it (HAL requires
	 * filters stay valid until the terminal event); invalidated by the next
	 * radiant_search_window() call, and only one window is ever in flight.
	 */
	const struct radiant_rx_filter *filters;
	uint8_t                     n_filters;

	radiant_time_t t_open;
	radiant_time_t t_close;

	/*
	 * RADIANT_RX_REPORT_CRC_FAIL if cfg.report_crc_fail. Never
	 * RADIANT_RX_STOP_ON_FIRST - the HAL forbids it on a search window
	 * since several masters may transmit inside one, and stopping at the
	 * first would cost every other searching channel the rest of the dwell.
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
	/* Pair each set's two devnum_lo values as k and k ^ 0xFF rather than as
	 * consecutive integers, because the backend's matcher cannot separate
	 * two addresses one bit apart. Set from caps.min_filter_hamming_bits;
	 * see set_filter_lo() in radiant_search.c. */
	bool     spread_pairs;

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
 * Zero the instance and compute the sweep geometry from
 * radiant_radio_caps_get(). cbs may be NULL, or either function in it, for a
 * caller that only reads counters.
 *
 * Caps are read ONCE (the HAL says the pointer is static for the program's
 * lifetime and capabilities don't change at runtime); a test wanting RAIL
 * geometry calls fake_radio_caps_preset_rail() before this, not after.
 *
 * RADIANT_SEARCH_ENOTSUP if caps.filter_wildcard_dev is set: that flag would
 * collapse the sweep to one window, but struct radiant_rx_filter has no field
 * to ask for a wildcard, so failing loudly is correct - the fix is a HAL
 * change. No planned backend sets it.
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

/*
 * Has the selected set had its whole dwell? Pure read, safe from a radio
 * callback - the arming authority calls it on every chunk completion to
 * decide whether the current request is still right to re-arm as-is (false)
 * or a new set must come from radiant_search_window() (true, thread context
 * only).
 *
 * MEASURED cost of getting this wrong (answering via a round trip through the
 * caller's pump instead): on the nRF54L15 with one 4 Hz tracked channel, 7.6
 * windows/s against 38.5 housekeeping passes, sets advancing at 2.5/s instead
 * of 3.85/s, full sweep 12.8 s instead of 8.3 s - radio idle, sweep waiting to
 * be told to use it.
 */
bool radiant_search_set_complete(const struct radiant_search *s);

/*
 * Dwell still owed to the selected set, in microseconds. 0 when it is finished.
 *
 * The companion to radiant_search_set_complete(): having decided to keep a
 * continuous request armed, the arming authority must also tell the scheduler
 * how much of it is left, or the next chunk is bounded by a ceiling that was
 * computed when the set was fresh and the set overshoots its budget. Pure read,
 * safe from a radio callback.
 */
uint32_t radiant_search_dwell_remaining(const struct radiant_search *s);

/* ---------------------------------------------------------------------------
 * Channels joining and leaving the search
 * ---------------------------------------------------------------------------
 */

/*
 * Put a channel into the search. timeout_us is RADIANT_SEARCH_TIMEOUT_NONE
 * for no expiry, or a duration from now; the deadline is examined only by
 * radiant_search_tick().
 *
 * If the seen cache holds a device matching want, the set containing its
 * devnum_lo is queued ahead of the round-robin cursor - the whole mechanism
 * of fast re-acquisition, so a channel losing a sensor should just call this
 * again.
 *
 * RADIANT_SEARCH_EINVAL for a null filter or unknown mode;
 * RADIANT_SEARCH_ENOSPC for an out-of-range channel.
 *
 * Re-issuing for a channel already searching REPLACES its filter, mode and
 * deadline rather than failing - what a host changing its mind looks like,
 * and the same path a re-SEARCHING state machine takes.
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

/*
 * Which mode an active channel is searching in. The caller needs this to
 * decide whether a match should convert the channel to tracking (ACQUIRE) or
 * leave it alone (SCAN, which never leaves) - see the acquired callback's
 * contract above. RADIANT_SEARCH_MODE_ACQUIRE if the channel is not active;
 * a caller asking this outside its own acquired callback for that channel is
 * asking about a channel that may already have left, and ACQUIRE is the mode
 * that does not keep searching, which is the safe default to fail toward.
 */
enum radiant_search_mode radiant_search_chan_mode(const struct radiant_search *s,
						 uint8_t channel);

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
 * The window in flight has finished. Credits the time actually spent listening
 * on the current set, which is what the dwell is accounted in.
 *
 * Three outcomes, not interchangeable:
 *   ran_to_close       reached its close - credit the whole span.
 *   opened, cut short  listened t_open..ended_at, then lost the radio -
 *                      credit up to ended_at.
 *   never opened       credit NOTHING (never armed/refused).
 *
 * The middle case is the one that matters - it used to credit zero, which
 * under a tracked channel (every search window cut short) meant the dwell
 * never filled and the sweep never advanced (see the dwell-is-a-budget
 * comment above).
 *
 * ended_at is ignored except in the middle case, and clamped to
 * [t_open, t_close] so a slightly-late clock read cannot over-credit.
 *
 * Called for you by radiant_search_on_rx() on a terminal event; call
 * directly if the scheduler's completion isn't a HAL event.
 */
void radiant_search_on_done(struct radiant_search *s, bool ran_to_close,
			bool opened, radiant_time_t ended_at);

/*
 * Expire seen entries and fire the timeout callback for any channel past its
 * deadline. Thread context. The scheduler should call it whenever it services
 * the search and at least once per dwell; nothing breaks if it is late, the
 * timeout simply fires late.
 */
void radiant_search_tick(struct radiant_search *s, radiant_time_t now);

/*
 * A search window was refused by an arbiter rather than by the air -
 * radiant_search_on_done()'s "never opened" case, credited zero dwell.
 * ch->deadline is a WALL-CLOCK bound though, and keeps counting down
 * regardless of why the window never opened.
 *
 * Left alone, an ACQUIRE search with a finite timeout expires on schedule
 * even when nearly every window was denied, conflating "how long have we
 * been trying" with "how long were we refused". MEASURED beside a busy BLE
 * advertiser: a channel armed 13 times, denied 12, aborted once, then simply
 * stopped - not because the device was absent, but because arbiter-owned
 * wall-clock time had been charged against a listening-time budget.
 *
 * Fix, same shape as radiant_channel.c's guard: push every ACTIVE channel's
 * finite deadline out by window_us (one denied window is time every
 * searching channel lost, not just the one carrying the sweep). The caller
 * sizes window_us to what the refused attempt cost in wall clock
 * (radiant_api.c uses the housekeeping interval driving the retry).
 * RADIANT_TIME_NEVER is untouched - no ceiling to protect.
 */
void radiant_search_note_denied(struct radiant_search *s, uint32_t window_us);

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
