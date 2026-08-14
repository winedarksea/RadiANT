/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_channel.h - the per-channel state machine, its configuration, and the
 * slot clock every other core module reads.
 *
 * Provenance: clean-room, from docs/sdk-ant-contract.md and src/ant_radio.h
 * (semantics, ordering, permitted error codes), the ANT Message Protocol and
 * Usage Rev 5.1 (D00000652) for the channel-status byte layout and the
 * 32768 Hz period unit, and bench measurements in docs/ant-radio-link.md and
 * docs/spike-b-part2-results.md (open-to-first-transmission delay, slot
 * period/jitter). See docs/decisions/0002-clean-room-policy.md.
 *
 * This layer has no radio and no scheduler in it. It owns: each of the 32
 * channels' state and every legal/illegal transition (with the wire response
 * code for each refusal); every per-channel configuration byte; the
 * channel's *slot clock* (next slot time, search-gives-up time); and which
 * radio operation id is attributed to a channel, which is the whole defence
 * against a cancelled operation's terminal event arriving late.
 *
 * It calls no HAL function - arming the radio is radiant_sched.c's job, so
 * windows from different channels can be merged (the scheduler's
 * highest-value optimization; see struct radiant_rx_req in
 * radiant_radio_hal.h). This module *decides*, radiant_sched.c *acts*; the
 * seam is the radiant_channel_on_*()/radiant_channel_next_* functions at the
 * bottom of this file. Consequently it's testable with no radio at all
 * (radiant_core/tests/src/test_channel.c still drives fake_radio to prove the
 * scheduler accepts these decisions, but every state assertion is pure).
 *
 * 32 channels because that's the serial protocol's own ceiling (burst header
 * channel field is 5 bits), baked in from the start since a channel-count
 * assumption is expensive to retrofit into a scheduler. Flat array of 32, no
 * allocation.
 *
 * Every function radiant_api.c forwards returns radiant_channel_err_t, a
 * uint8_t whose value IS the wire byte (src/ant_radio.h's convention - an
 * unrecognised code hangs a host library). Values are spelled as literals
 * here rather than pulled from radiant_wire.h so this header reads standalone
 * without a reader having to cross-reference the generated one; the
 * _Static_assert block at the bottom checks the two spellings agree,
 * unconditionally - radiant_wire.h is module-owned, so unlike the src/ant_wire.h
 * this used to (indirectly) depend on, it costs this header nothing to include
 * outright rather than gate the check on some other translation unit having
 * already pulled the generated names into scope first.
 */

#ifndef RADIANT_CHANNEL_H_
#define RADIANT_CHANNEL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant_core/radiant_frame.h>     /* struct radiant_channel_id, and the frame geometry */
#include <radiant_core/radiant_radio_hal.h> /* radiant_time_t, enum radiant_radio_status */
#include <radiant_core/radiant_wire.h>      /* RADIANT_WIRE_* - only for the _Static_assert block below */

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Sizing
 * ---------------------------------------------------------------------------
 */

/* The serial protocol's ceiling: the burst header's low five bits. */
#define RADIANT_CHANNEL_COUNT 32u

/*
 * Networks, indexing the keys ant_net.c holds. Three is what every host
 * expects; a host that configures network 1 and gets INVALID_NETWORK_NUMBER
 * concludes the dongle is broken rather than frugal. ant_net.c owns the
 * keys, this module owns only the range check (antr_channel_assign() must
 * refuse a bad network before any key lookup).
 */
#define RADIANT_CHANNEL_NETWORK_COUNT 3u

/* ---------------------------------------------------------------------------
 * Response codes - the wire bytes, checked against radiant_wire.h at the
 * bottom of this file.
 * ---------------------------------------------------------------------------
 */
typedef uint8_t radiant_channel_err_t;

#define RADIANT_CH_OK                 0x00u /* RADIANT_WIRE_RESPONSE_NO_ERROR */
#define RADIANT_CH_ERR_WRONG_STATE    0x15u /* RADIANT_WIRE_CHANNEL_IN_WRONG_STATE */
#define RADIANT_CH_ERR_NOT_OPENED     0x16u /* RADIANT_WIRE_CHANNEL_NOT_OPENED */
#define RADIANT_CH_ERR_ID_NOT_SET     0x18u /* RADIANT_WIRE_CHANNEL_ID_NOT_SET */
#define RADIANT_CH_ERR_INVALID_MESSAGE 0x28u /* RADIANT_WIRE_INVALID_MESSAGE */
#define RADIANT_CH_ERR_INVALID_NETWORK 0x29u /* RADIANT_WIRE_INVALID_NETWORK_NUMBER */
#define RADIANT_CH_ERR_INVALID_PARAM  0x33u /* RADIANT_WIRE_INVALID_PARAMETER_PROVIDED */

/*
 * Two host-observable distinctions, not interchangeable (tools/ant_conformance.py
 * byte-diffs them): channel NUMBER out of range is INVALID_MESSAGE, a bad
 * parameter value is INVALID_PARAM; WRONG_STATE is a channel in the wrong
 * state, NOT_OPENED is a data-transfer call on a channel not on air.
 */

/* ---------------------------------------------------------------------------
 * Channel events this module raises: RADIANT_WIRE_EVENT_* codes, sent to the host
 * through radiant_event.c. Spelled as literals for the same reason as the
 * response codes above, checked in the same _Static_assert block.
 * ---------------------------------------------------------------------------
 */
#define RADIANT_CH_EVENT_RX_SEARCH_TIMEOUT   0x01u /* RADIANT_WIRE_EVENT_RX_SEARCH_TIMEOUT */
#define RADIANT_CH_EVENT_CHANNEL_CLOSED      0x07u /* RADIANT_WIRE_EVENT_CHANNEL_CLOSED */
#define RADIANT_CH_EVENT_RX_FAIL_GO_TO_SEARCH 0x08u /* RADIANT_WIRE_EVENT_RX_FAIL_GO_TO_SEARCH */

/* ---------------------------------------------------------------------------
 * Channel status byte, and channel types
 * ---------------------------------------------------------------------------
 */

/* Low two bits of a MESG_CHANNEL_STATUS reply. */
#define RADIANT_CH_STATUS_UNASSIGNED 0x00u /* RADIANT_WIRE_STATUS_UNASSIGNED_CHANNEL */
#define RADIANT_CH_STATUS_ASSIGNED   0x01u /* RADIANT_WIRE_STATUS_ASSIGNED_CHANNEL */
#define RADIANT_CH_STATUS_SEARCHING  0x02u /* RADIANT_WIRE_STATUS_SEARCHING_CHANNEL */
#define RADIANT_CH_STATUS_TRACKING   0x03u /* RADIANT_WIRE_STATUS_TRACKING_CHANNEL */
#define RADIANT_CH_STATUS_STATE_MASK 0x03u /* RADIANT_WIRE_STATUS_CHANNEL_STATE_MASK */

/* Rest of the byte, per Rev 5.1 9.5.7.1: bits 3:2 network, bits 7:4 channel
 * type. Type values already live in the top nibble (SLAVE 0x00 ..
 * MASTER_TX_ONLY 0x50) so it's copied, not shifted - a mistake here is
 * invisible until a host reads channel type zero on a live master. */
#define RADIANT_CH_STATUS_NETWORK_SHIFT 2u
#define RADIANT_CH_STATUS_NETWORK_MASK  0x0Cu
#define RADIANT_CH_STATUS_TYPE_MASK     0xF0u

/*
 * Bit 4 is the master bit: set for the three master types, clear for the
 * three slave types. The only bit of the type byte this module interprets -
 * it decides transmitter vs. search on open.
 *
 * An UNRECOGNISED type byte is accepted, deliberately: antr_channel_assign()
 * has no permitted error code left to reject a type with (only NO_ERROR,
 * INVALID_MESSAGE, WRONG_STATE, INVALID_NETWORK_NUMBER), so inventing one
 * would be wire-visible. See docs/sdk-ant-contract.md.
 */
#define RADIANT_CH_TYPE_MASTER_BIT 0x10u /* RADIANT_WIRE_CHANNEL_TYPE_MASTER */

/* ---------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------------
 */

/*
 * Period is in 32768 Hz counts, the radio's own timebase - a backend that
 * rounded to milliseconds would walk a slave's window out of the master's
 * slot within a minute.
 *
 * 8182 counts is the ANT+ 4.005 Hz default, matching the bench measurement
 * (249,696.4 us mean over 744 intervals, docs/ant-radio-link.md). NOT 8192:
 * that's 250,000 us exactly, 305 us off what a real ANT+ master transmits
 * at, though src/ant_radio.h's comment calls it "the 4 Hz most profiles use".
 */
#define RADIANT_CHANNEL_PERIOD_HZ         32768u
#define RADIANT_CHANNEL_PERIOD_ANT_PLUS   8182u  /* 4.005 Hz, measured */

/*
 * The schedulable range. Top is what a uint16_t holds (2.0 s). The floor is
 * measured, not chosen: a slave answers 2.19 ms after the master's address,
 * an acknowledged exchange runs ~1.55 ms each way, a burst sustains one
 * packet per ~3.11 ms (docs/ant-radio-link.md) - so under ~8 ms cannot fit
 * one slot's exchange. 273 counts is 8.331 ms.
 *
 * radiant_sched.c may raise this floor at init via
 * radiant_channel_period_floor_set() once it knows caps.min_arm_lead_us; this
 * constant is only the protocol's bound, not the backend's.
 */
#define RADIANT_CHANNEL_PERIOD_MIN_COUNTS 273u
#define RADIANT_CHANNEL_PERIOD_MAX_COUNTS 65535u

/* Search timeouts are in 2.5 s increments. 0 disables that priority's search;
 * 255 means "search forever" and is the one value that is not a duration. */
#define RADIANT_CHANNEL_SEARCH_TICK_US    2500000u
#define RADIANT_CHANNEL_SEARCH_INFINITE   255u

/* ANT's default high-priority search timeout is 10 ticks = 25 s. The wildcard
 * sweep's worst case is 8.3 s, average ~4 s (docs/ant-radio-link.md), so a
 * full sweep fits with room to spare - radiant_search.c can size its sweep
 * without negotiating with this module. */
#define RADIANT_CHANNEL_SEARCH_TIMEOUT_DEFAULT 10u

/* Default low-priority timeout: off. Low-priority search yields the radio to
 * any other channel, so it can run indefinitely without starving a live
 * session - but a channel that silently never times out is a host bug that
 * never surfaces, so it is opt-in. */
#define RADIANT_CHANNEL_LP_SEARCH_TIMEOUT_DEFAULT 0u

/*
 * Consecutive missed slots before a TRACKING channel gives up and searches
 * again, raising RADIANT_CH_EVENT_RX_FAIL_GO_TO_SEARCH.
 *
 * `[inferred]` - ANT's own threshold has not been measured (needs a master
 * that can be silenced mid-session). 8 slots is 2.0 s at the ANT+ period:
 * long enough that the ~0.4% per-slot collision floor can't plausibly
 * produce 8 in a row, short enough a rider notices re-acquisition rather
 * than dropout. radiant_sched.c may override per build.
 */
#define RADIANT_CHANNEL_RX_FAIL_TO_SEARCH 8u

/*
 * Consecutive DENIED slots (radio lent to another stack, never ran) before a
 * TRACKING channel gives up and promotes into the miss path above.
 *
 * A bound exists even though a denial is not evidence about the peer,
 * because the guard is finite: each denial still advances the slot clock by
 * one period with no fresh sync, charged by radiant_channel_guard_us()
 * exactly as a miss is. Past GUARD_MAX_US / DRIFT_WORST_US periods the guard
 * has hit its ceiling and provably no longer covers the disagreement - the
 * channel is listening in the wrong place, which RX_FAIL_GO_TO_SEARCH exists
 * to catch. (Same quantity as radiant_channel.c's second _Static_assert.)
 *
 * 16 slots is ~4 s. Under a working arbiter this never fires - tracked
 * windows get an inviolate reservation - so 16 consecutive denials means the
 * arbiter is broken; this counter against `missed` says which it was.
 */
#define RADIANT_CHANNEL_DENY_TO_SEARCH \
	(RADIANT_CHANNEL_GUARD_MAX_US / RADIANT_CHANNEL_DRIFT_WORST_US)

/* ---------------------------------------------------------------------------
 * The tracked-window guard
 *
 * How wide a slave opens its receive window either side of the t_sync it
 * predicts. It used to be one constant, sized from the spec's worst case, and
 * these constants plus radiant_channel_guard_us() are what replaced it. The
 * reasoning is in the doc comment on that function; the numbers are here so a
 * build can see them and the _Static_asserts in radiant_api.c can check them.
 * ---------------------------------------------------------------------------
 */

/*
 * ANT's LF clock tolerance, worst case, as one period's worth of microseconds.
 *
 * +/-50 ppm at each end (any ANT datasheet) is +/-100 ppm relative; over one
 * ~249.7 ms period that's +/-25 us that period alone could add, with no
 * accumulated drift from a prior miss.
 *
 * A budget, not headroom - a spec-limit master sits exactly here. Used as the
 * guard's per-miss term below: an extrapolated period's real error is
 * unmeasured, so it's charged at the spec bound rather than at what this
 * master has actually been doing.
 */
#define RADIANT_CHANNEL_DRIFT_WORST_US 25u

/* Widest the guard ever gets, and its value whenever the estimator has
 * nothing to say: RX_FAIL_TO_SEARCH * DRIFT_WORST_US, with margin. This was
 * the whole guard before the estimator existed. */
#define RADIANT_CHANNEL_GUARD_MAX_US 400u

/*
 * Narrowest the guard ever gets, before radiant_channel_guard_floor_set()
 * raises it for a backend that needs more.
 *
 * Deliberately far above the measured residual (9 us on this bench) - not a
 * tight fit, a refusal to fit tightly. A too-narrow guard fails silently
 * (yield drops near the collision floor, no error code), and nothing on this
 * bench distinguishes the two, so the floor is set where it doesn't need to.
 * Lower it only against a Phase 0 sensitivity ladder showing the cost.
 */
#define RADIANT_CHANNEL_GUARD_MIN_US 100u

/* How many measured residuals the guard is sized at: an ordinary excursion
 * to several times the running average still stays inside the window. */
#define RADIANT_CHANNEL_GUARD_K 4u

/*
 * Consecutive clean slots before the estimator is allowed to narrow anything.
 * An unlocked estimator must never narrow - at acquisition the residual has
 * zero measurements, so a guard from no evidence is a guard from luck. 8 is
 * one EWMA time constant at the 1/8 weight used below, so the average is
 * genuinely an average by the time it's trusted.
 */
#define RADIANT_CHANNEL_GUARD_LOCK_SLOTS 8u

/*
 * The clock accuracy every constant above silently assumes, in ppm.
 * DRIFT_WORST_US is +/-50 ppm at each end over one ANT+ period; naming the 50
 * makes it arithmetic rather than prose, since radiant_channel_clock_accuracy_set()
 * lets a master announce a worse one without the receiver's own budget going away.
 */
#define RADIANT_CHANNEL_CLOCK_PPM_ANT 50u

/*
 * Widest announcement taken at face value. Above 1000 ppm the master's clock
 * couldn't hold a channel at all, so it's a typo or an attacker rather than
 * a real bound - clamped rather than rejected, since the widest window this
 * core builds is still a better answer than the narrowest.
 */
#define RADIANT_CHANNEL_CLOCK_PPM_MAX 1000u

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------
 */

/*
 * Five states, not the four the wire names.
 *
 *   UNASSIGNED --assign--> ASSIGNED --open--> SEARCHING --acquire--> TRACKING
 *        ^                    |  ^                |                     |
 *        +----unassign--------+  +----------------+---------------------+
 *                                          via CLOSING
 *
 * "CONFIGURED" is deliberately not a state: the ID-not-set flag every other
 * config call ignores would have produced a transition table with a dozen
 * entries all meaning "still ASSIGNED", so it's just a flag on ASSIGNED.
 *
 * CLOSING is the state the wire doesn't name but the contract requires ("not
 * closed when this returns, it is closing", RADIANT_WIRE_EVENT_CHANNEL_CLOSED tells
 * the host when done). A channel with an operation in flight can't become
 * ASSIGNED the instant close() returns - the terminal event hasn't arrived,
 * and unassigning underneath it would release state the backend still
 * refers to - so close() parks it here until the terminal event completes
 * the transition.
 *
 * radiant_channel_status_get() reports CLOSING as whatever it was closing
 * FROM (SEARCHING/TRACKING), not ASSIGNED - reporting ASSIGNED early would
 * let a host try to unassign a channel that radiant_channel_unassign() would
 * then refuse.
 */
enum radiant_channel_state {
	RADIANT_CH_STATE_UNASSIGNED = 0,
	RADIANT_CH_STATE_ASSIGNED,
	RADIANT_CH_STATE_SEARCHING,
	RADIANT_CH_STATE_TRACKING,
	RADIANT_CH_STATE_CLOSING,
	RADIANT_CH_STATE_COUNT
};

/* No stored "why am I closing" field: each reason's event is raised the
 * moment it's known (search timeout raises RX_SEARCH_TIMEOUT immediately;
 * only the trailing CHANNEL_CLOSED is deferred), so there's nothing left
 * for a stored reason to select by the time the close completes. */

/* ---------------------------------------------------------------------------
 * The event sink - implemented elsewhere, resolved at link time
 * ---------------------------------------------------------------------------
 */

/*
 * One channel event, on its way to the host.
 *
 * Implemented by radiant_event.c, not by radiant_channel.c (a test double in
 * radiant_core/tests/src/test_channel.c otherwise) - resolved at link time,
 * the same inversion src/ant_radio.h uses for antr_on_message(). No
 * registration pointer needed since there's never more than one consumer.
 *
 * @param channel     0 .. RADIANT_CHANNEL_COUNT-1.
 * @param event_code  An RADIANT_CH_EVENT_* value (an RADIANT_WIRE_EVENT_* wire byte).
 *
 * Called from whatever context provoked the transition - the bridge's parser
 * thread, or radiant_sched.c's context which may be a radio callback - so the
 * implementation must not block and must not call back into radiant_channel.
 */
void radiant_channel_event_out(uint8_t channel, uint8_t event_code);

/* ---------------------------------------------------------------------------
 * Module lifecycle
 * ---------------------------------------------------------------------------
 */

/* Every channel to UNASSIGNED, every configuration byte to its default, the
 * slot clock stopped. Raises no events - there is nobody to tell yet, and
 * antr_init() has not returned. */
void radiant_channel_init(void);

/*
 * antr_stack_reset(): every channel closed and unassigned, synchronously.
 *
 * Channel-closed events raised as a side effect ARE delivered - the contract
 * requires it, and the bridge discards them for a short window afterwards
 * (a real stick emits nothing between reset and the startup message).
 *
 * Any operation in flight is orphaned rather than waited on - this module
 * can't abort one, so radiant_sched.c must call radiant_radio_abort() first
 * and feed the terminal event in afterwards; it arrives at a channel with no
 * binding and is counted as stale, same path as any late event.
 */
void radiant_channel_reset_all(void);

/* Raise the schedulable period floor, in 32768 Hz counts. For radiant_sched.c at
 * init, once it has read caps.min_arm_lead_us. Values below
 * RADIANT_CHANNEL_PERIOD_MIN_COUNTS are ignored: the protocol floor is a floor. */
void radiant_channel_period_floor_set(uint16_t counts);

/* ---------------------------------------------------------------------------
 * Lifecycle - what radiant_api.c forwards from the four antr_channel_* calls
 * ---------------------------------------------------------------------------
 */

/*
 * antr_channel_assign(). The channel must be UNASSIGNED.
 *
 * Resets every configuration byte to default, including the channel ID - a
 * reassigned channel must not inherit the last session's device number, or a
 * host that opens without setting an ID would silently track the wrong
 * sensor instead of getting RADIANT_CH_ERR_ID_NOT_SET.
 *
 * Returns RADIANT_CH_OK, RADIANT_CH_ERR_INVALID_MESSAGE (channel out of range),
 * RADIANT_CH_ERR_WRONG_STATE (already assigned) or RADIANT_CH_ERR_INVALID_NETWORK.
 */
radiant_channel_err_t radiant_channel_assign(uint8_t channel, uint8_t type,
				     uint8_t network, uint8_t ext_assign);

/*
 * antr_channel_unassign(). The channel must be ASSIGNED and closed.
 *
 * Returns RADIANT_CH_OK, RADIANT_CH_ERR_INVALID_MESSAGE or RADIANT_CH_ERR_WRONG_STATE
 * (never assigned, still open, or still closing). Refuses rather than
 * closing it first, which would swallow the CHANNEL_CLOSED event a host
 * may be waiting on.
 */
radiant_channel_err_t radiant_channel_unassign(uint8_t channel);

/*
 * antr_channel_open_with_offset(). The channel must be ASSIGNED with its ID
 * set.
 *
 * @param offset  The one-shot start offset, in the same 32768 Hz counts the
 *                period uses.
 * @param now     radiant_radio_now(). Passed in since this module calls no
 *                HAL function and a test must open a channel at a chosen instant.
 *
 * A MASTER's first transmission is scheduled one FULL CHANNEL PERIOD after
 * this call, plus `offset` - measured, not assumed: 8 of 8 runs showed one
 * full period from MESG_OPEN_CHANNEL to first EVENT_TX (docs/ant-radio-link.md).
 * The offset is applied on top of that period, not instead of it - offset 0
 * does not mean "transmit now", which the measurement falsifies.
 *
 * A SLAVE enters SEARCHING with its first receive window at now + offset and
 * search deadline beyond that - no period wait, it has nothing to align to yet.
 *
 * Returns RADIANT_CH_OK, RADIANT_CH_ERR_INVALID_MESSAGE (channel out of range),
 * RADIANT_CH_ERR_WRONG_STATE (unassigned, or already open or closing) or
 * RADIANT_CH_ERR_ID_NOT_SET.
 */
radiant_channel_err_t radiant_channel_open(uint8_t channel, uint16_t offset,
				   radiant_time_t now);

/*
 * antr_channel_close(). The channel must be on air.
 *
 * If no radio operation is bound, the close completes here and
 * CHANNEL_CLOSED is raised before this returns. If one is bound, the channel
 * enters CLOSING and the event is raised when radiant_channel_on_terminal()
 * delivers that operation's terminal event - which WILL arrive, aborted or
 * not, since the HAL guarantees exactly one terminal event per accepted
 * operation even for a cancelled one. Either way, raised exactly once.
 *
 * Returns RADIANT_CH_OK, RADIANT_CH_ERR_INVALID_MESSAGE or RADIANT_CH_ERR_WRONG_STATE
 * (not open). A second call returns WRONG_STATE rather than a second event.
 */
radiant_channel_err_t radiant_channel_close(uint8_t channel, radiant_time_t now);

/* ---------------------------------------------------------------------------
 * Configuration - legal only on an ASSIGNED, closed channel unless said
 * otherwise
 *
 * Per docs/sdk-ant-contract.md: ID, period, RF frequency, CRC mode and
 * frequency-hop table all retune the radio, so they require a closed channel.
 * Search and power settings retune nothing and need only an assigned channel.
 * ---------------------------------------------------------------------------
 */

/* antr_channel_id_set(). Zero is the wildcard on a slave, in all three fields.
 * Returns OK, INVALID_MESSAGE, or WRONG_STATE (unassigned or open). */
radiant_channel_err_t radiant_channel_id_set(uint8_t channel, uint16_t device_number,
				     uint8_t device_type, uint8_t trans_type);

/*
 * antr_channel_id_get(). On a TRACKING slave reports the ID actually
 * acquired (wildcards resolved), otherwise what was configured. The
 * acquired ID is kept separately and discarded when the channel closes, so
 * a reopened channel searches again rather than silently narrowing to last
 * session's sensor.
 *
 * Returns OK or INVALID_MESSAGE. Legal on an unassigned channel - the answer
 * is the all-zero ID, not an error.
 */
radiant_channel_err_t radiant_channel_id_get(uint8_t channel,
				     struct radiant_channel_id *out);

/* antr_channel_period_set(), in 32768 Hz counts.
 * Returns OK, INVALID_MESSAGE, WRONG_STATE, or INVALID_PARAM (outside
 * [floor, RADIANT_CHANNEL_PERIOD_MAX_COUNTS]). */
radiant_channel_err_t radiant_channel_period_set(uint8_t channel, uint16_t period);
radiant_channel_err_t radiant_channel_period_get(uint8_t channel, uint16_t *out);

/* antr_channel_radio_freq_set(): an index, 0..124, meaning 2400+N MHz.
 * Returns OK, INVALID_MESSAGE, WRONG_STATE, or INVALID_PARAM (> 124). */
radiant_channel_err_t radiant_channel_rf_freq_set(uint8_t channel, uint8_t freq);
radiant_channel_err_t radiant_channel_rf_freq_get(uint8_t channel, uint8_t *out);

/*
 * antr_channel_radio_tx_power_set(). Requires an ASSIGNED channel, legal
 * while open - power is the one radio property that doesn't need to leave
 * the air to change.
 *
 * Bit 7 of `level` (0x80) means `custom` names a raw part-specific value.
 * Levels 0..5 are -20,-12,-4,0,+4,+8 dBm; level 5 exists only on
 * nRF52820/52833/52840 and is accepted here regardless, since whether the
 * part can produce it is checked by radiant_sched.c against
 * caps.tx_power_max_dbm when it arms.
 *
 * Returns OK, INVALID_MESSAGE, WRONG_STATE (unassigned) or INVALID_PARAM
 * (a level above 5 with the custom bit clear).
 */
radiant_channel_err_t radiant_channel_tx_power_set(uint8_t channel, uint8_t level,
					   uint8_t custom);
radiant_channel_err_t radiant_channel_tx_power_get(uint8_t channel, uint8_t *level,
					   uint8_t *custom);

/* antr_channel_search_timeout_set() / _low_priority_rx_search_timeout_set(),
 * both in 2.5 s increments, both requiring only an assigned channel. Changing
 * either on a SEARCHING channel re-derives the deadline from the time the
 * search began, so a host that extends a timeout mid-search gets the extension
 * rather than a restart.
 * Returns OK, INVALID_MESSAGE or WRONG_STATE (unassigned). */
radiant_channel_err_t radiant_channel_search_timeout_set(uint8_t channel,
						 uint8_t timeout);
radiant_channel_err_t radiant_channel_lp_search_timeout_set(uint8_t channel,
						    uint8_t timeout);

/*
 * antr_channel_radio_crc_mode_set(). Opaque byte, validated against this
 * module's supported set - exactly {RADIANT_CH_CRC_MODE_DEFAULT} - since a
 * backend that silently accepted a shorter CRC would give a host a link
 * that works and framing that doesn't. Returns OK, INVALID_MESSAGE,
 * WRONG_STATE or INVALID_PARAM.
 */
#define RADIANT_CH_CRC_MODE_DEFAULT 0x00u
radiant_channel_err_t radiant_channel_crc_mode_set(uint8_t channel, uint8_t mode);
radiant_channel_err_t radiant_channel_crc_mode_get(uint8_t channel, uint8_t *out);

/* antr_auto_freq_hop_table_set(). Three 2400 MHz offsets, only meaningful on a
 * channel assigned with the frequency-agility extended flag - which ANT+
 * profiles forbid, so nothing in a standard session reaches it.
 * Returns OK, INVALID_MESSAGE, WRONG_STATE or INVALID_PARAM (any offset >124). */
radiant_channel_err_t radiant_channel_freq_hop_table_set(uint8_t channel, uint8_t freq0,
						 uint8_t freq1, uint8_t freq2);
radiant_channel_err_t radiant_channel_freq_hop_table_get(uint8_t channel,
						 uint8_t out[3]);

/* ---------------------------------------------------------------------------
 * Search configuration held per channel
 *
 * radiant_search.c owns the sweep, seen-cache and scan mode, but not the
 * per-channel bytes the host writes - those share this module's state
 * machine and "assigned channel" precondition, and are read back through
 * the getters below.
 * ---------------------------------------------------------------------------
 */

/*
 * antr_search_waveform_set(). Stored and never read - this backend runs one
 * search geometry. sdk-ant names exactly two legal values (316 default,
 * 97 fast) and forbids custom ones, so only those two are accepted rather
 * than storing an arbitrary uint16_t that could never mean anything. See
 * docs/sdk-ant-comparison.md item 2.
 * Returns OK, INVALID_MESSAGE, WRONG_STATE (unassigned) or INVALID_PARAM
 * (neither DEFAULT nor FAST).
 */
#define RADIANT_CH_SEARCH_WAVEFORM_DEFAULT 316u
#define RADIANT_CH_SEARCH_WAVEFORM_FAST     97u
radiant_channel_err_t radiant_channel_search_waveform_set(uint8_t channel,
						  uint16_t waveform);
radiant_channel_err_t radiant_channel_search_waveform_get(uint8_t channel,
						  uint16_t *out);

/*
 * antr_prox_search_set(). PERMANENT LIMITATION, not a gap: nothing wires a
 * channel's prox_threshold to radiant_search.c's RSSI gate, and the
 * capability bit is not advertised. Storing a non-zero threshold that
 * nothing enforces is the trap docs/gotchas.md warns against - a host would
 * believe distant sensors are filtered when none are. So only 0 (off) is
 * accepted; any non-zero threshold is refused rather than silently dropped.
 * See docs/sdk-ant-comparison.md item 2.
 * Returns OK, INVALID_MESSAGE, WRONG_STATE or INVALID_PARAM (threshold > 10,
 * or any non-zero threshold).
 */
#define RADIANT_CH_PROX_THRESHOLD_MAX 10u
radiant_channel_err_t radiant_channel_prox_search_set(uint8_t channel,
					      uint8_t threshold,
					      uint8_t custom);
radiant_channel_err_t radiant_channel_prox_search_get(uint8_t channel,
					      uint8_t *threshold,
					      uint8_t *custom);

/* antr_search_channel_priority_set()/_get(). 0 low, 1 high.
 * Returns OK, INVALID_MESSAGE or WRONG_STATE (unassigned) on the setter; the
 * getter returns OK or INVALID_MESSAGE, per the contract's asymmetry. */
radiant_channel_err_t radiant_channel_search_priority_set(uint8_t channel,
						  uint8_t priority);
radiant_channel_err_t radiant_channel_search_priority_get(uint8_t channel,
						  uint8_t *out);

/* antr_active_search_sharing_cycles_set()/_get(). Advisory for a backend free
 * to merge receive windows, but it must still round-trip because
 * tools/ant_features.py checks that it does. */
radiant_channel_err_t radiant_channel_sharing_cycles_set(uint8_t channel,
						 uint8_t cycles);
radiant_channel_err_t radiant_channel_sharing_cycles_get(uint8_t channel,
						 uint8_t *out);

/*
 * antr_sdu_mask_config(): the per-channel half of selective data update.
 * Only the channel range is checked; the byte is stored verbatim, and the
 * mask-index INVALID_PARAM check belongs to whoever owns the mask table.
 * Returns OK or INVALID_MESSAGE.
 */
#define RADIANT_CH_SDU_MASK_OFF 0xFFu /* RADIANT_WIRE_INVALID_SDU_MASK */
radiant_channel_err_t radiant_channel_sdu_mask_config_set(uint8_t channel,
						  uint8_t mask_config);
radiant_channel_err_t radiant_channel_sdu_mask_config_get(uint8_t channel,
						  uint8_t *out);

/* ---------------------------------------------------------------------------
 * Status
 * ---------------------------------------------------------------------------
 */

/*
 * antr_channel_status_get(). Legal on an unassigned channel - that's the
 * answer, not an error, which is why every host opens a session by reading
 * it. Byte layout: state bits 1:0, network bits 3:2, channel type bits 7:4.
 *
 * Returns OK or INVALID_MESSAGE.
 */
radiant_channel_err_t radiant_channel_status_get(uint8_t channel, uint8_t *out);

/* The state itself, for the other core modules. Out-of-range answers
 * RADIANT_CH_STATE_UNASSIGNED, so a caller that forgot to range-check gets the
 * least dangerous answer rather than a read past the array. */
enum radiant_channel_state radiant_channel_state_get(uint8_t channel);

/* True for the three master channel types. False for an unassigned channel. */
bool radiant_channel_is_master(uint8_t channel);

/* True while the channel is on air: SEARCHING, TRACKING or CLOSING. This is
 * the predicate behind RADIANT_CH_ERR_NOT_OPENED, and it deliberately includes
 * CLOSING - a data-transfer call on a channel whose close has not completed is
 * refused for wanting the radio, not for wanting the wrong state. */
bool radiant_channel_is_open(uint8_t channel);

/* The channel type and the extended assignment flags as assigned. Zero on an
 * unassigned channel. */
uint8_t radiant_channel_type_get(uint8_t channel);
uint8_t radiant_channel_ext_assign_get(uint8_t channel);
uint8_t radiant_channel_network_get(uint8_t channel);

/* ---------------------------------------------------------------------------
 * The scheduler seam
 *
 * Everything below is called by radiant_sched.c and by nothing else. None of it
 * returns a wire code, because none of it is reachable from a host message.
 * ---------------------------------------------------------------------------
 */

/*
 * When this channel next wants the radio: t_sync of its next slot for a
 * master, opening edge of its next receive window for a slave.
 * RADIANT_TIME_NEVER when the channel is not on air.
 *
 * radiant_sched.c subtracts caps.ramp_up_us and caps.min_arm_lead_us
 * (backend properties, not this module's business). Window width either
 * side of this instant is radiant_channel_guard_us() below.
 *
 * The spec figure is +/-25 us PER PERIOD, a budget not headroom
 * (RADIANT_CHANNEL_DRIFT_WORST_US), and it compounds: each miss extrapolates
 * t_next by one more period with no fresh sync, so N consecutive misses
 * carry up to N*25 us of added worst-case disagreement. Checked by the two
 * BUILD_ASSERTs in radiant_api.c (ceiling and floor).
 */
radiant_time_t radiant_channel_next_slot(uint8_t channel);

/*
 * How far either side of that instant this channel's receive window should
 * reach, in microseconds. Never zero; never above RADIANT_CHANNEL_GUARD_MAX_US
 * unless the master has announced a clock needing more (see below).
 *
 * Not a constant because the spec-worst-case-over-8-misses figure is 400 us,
 * but the bench measures a 9 us residual - so tracked slots were buying 800 us
 * of receive (contention, current draw, matcher noise) to cover ~9 us of real
 * error. Instead the channel keeps an EWMA of |t_sync - predicted| over clean
 * slots, and the guard is:
 *
 *     clamp(floor, K * average + miss_count * DRIFT_WORST, GUARD_MAX)
 *
 * First term is what this master has been measured doing, with margin.
 * Second is what an unmeasured extrapolated period could be doing, charged
 * at the spec bound. A well-behaved link runs narrow and widens automatically
 * as it loses slots.
 *
 * GUARD_MAX_US unconditionally whenever the estimator has nothing to say
 * (not TRACKING, just acquired, or after RX_FAIL_GO_TO_SEARCH) - narrowing on
 * no evidence is the one thing this must never do; the failure mode has no
 * error code.
 *
 * Everything above assumes ANT's +/-50 ppm bound, which a crystal-less
 * coin-cell node's RC oscillator (+/-250-500 ppm) blows through - at a 2 s
 * period that's 600 us of disagreement against a 400 us ceiling, so the
 * channel is lost silently and the estimator never gets a clean slot to
 * lock on. radiant_channel_clock_accuracy_set() lets such a master announce
 * its own ppm, which scales both terms (per-miss charge and ceiling) in
 * proportion, and raises the floor to cover one such period.
 *
 * AN ANNOUNCEMENT NEVER NARROWS ANYTHING - only the measured residual
 * estimator does, since a claim is not a measurement. A build with no
 * announcement is byte-for-byte the guard computed before the field existed.
 *
 * No period (rate) estimator, deliberately: radiant_channel_on_slot()
 * re-anchors on every received frame, so a period correction would only
 * affect MISSED slots, where the guard already charges the safer spec-bound
 * estimate; and acting on the slot clock instead of the window risks walking
 * off the master silently over minutes. struct chan's 72-byte/32-channel
 * budget also doesn't have room for one. If radiant_channel_residual_us()
 * ever reads tens of microseconds on a link that isn't losing slots, that's
 * the signal this needs revisiting.
 *
 * EWMA rather than a Kalman filter: a filter's variance would let the guard
 * use mean + k*sigma instead of k*mean, but at the measured 9 us residual the
 * 100 us floor dominates the clamp regardless, so a better estimate changes
 * nothing today - and it costs multiplies/division with no float support and
 * the same tight per-channel budget. Revisit only if a sensitivity ladder
 * ever justifies dropping GUARD_MIN_US near k*avg; the next cheap step then
 * is a decaying maximum (the guard needs the worst residual, not the mean),
 * not a filter.
 */
uint32_t radiant_channel_guard_us(uint8_t channel);

/*
 * Raise the floor of the above, in microseconds. Values below
 * RADIANT_CHANNEL_GUARD_MIN_US are ignored - the floor is a floor.
 *
 * Called once at init by whoever holds backend capabilities: a backend with
 * caps.has_sync_timestamp == false is reporting an inferred t_sync, not a
 * captured one, and an estimator fed that must not narrow anything - passing
 * RADIANT_CHANNEL_GUARD_MAX_US pins the guard wide, switching the mechanism
 * off with no separate flag needed.
 */
void radiant_channel_guard_floor_set(uint16_t us);

/*
 * The clock accuracy this channel's MASTER has announced, in ppm, or 0 for
 * nothing announced. Per channel, not global - a fact about one master. Set
 * from device type 0x60's schedule block or a sync-handoff page; cleared by
 * assign/unassign so a reassigned channel doesn't inherit the old clock.
 *
 * 0 means "nothing announced", not "perfect": a silent node gets exactly the
 * guard this core computed before this field existed. Values above
 * RADIANT_CHANNEL_CLOCK_PPM_MAX are clamped. An announcement may only widen
 * the window - see radiant_channel_guard_us().
 */
void radiant_channel_clock_accuracy_set(uint8_t channel, uint16_t ppm);

/* What was announced, or 0. Diagnostic, and the seam a test asserts against. */
uint16_t radiant_channel_clock_accuracy_get(uint8_t channel);

/*
 * The running average above, in microseconds, or 0 if the estimator hasn't
 * locked. Diagnostic only - nothing in the core branches on it, but it's the
 * first thing to read if a narrowed window is suspected of costing packets.
 */
uint16_t radiant_channel_residual_us(uint8_t channel);

/* The absolute instant this channel's search gives up, or RADIANT_TIME_NEVER for
 * an infinite timeout or a channel that is not searching. */
radiant_time_t radiant_channel_search_deadline(uint8_t channel);

/* The earliest next_slot across all channels, and which channel it belongs to.
 * *channel_out is untouched when the answer is RADIANT_TIME_NEVER. */
radiant_time_t radiant_channel_earliest_slot(uint8_t *channel_out);

/*
 * Attribute a radio operation to a channel.
 *
 * `op` is the non-zero id radiant_radio_tx()/rx() returned. Binding it makes
 * a late terminal event recognisable: the HAL guarantees a cancelled
 * operation's terminal event still arrives, so an unbound op is the normal
 * outcome of a close that raced a window, not an anomaly.
 *
 * op == 0 clears the binding. Rebinding a channel that already has a
 * different op is a scheduler bug; the new binding wins and
 * radiant_channel_stale_op_count() is NOT incremented, since that counter
 * means "event for an operation nobody owns" and conflating the two would
 * hide the scheduler bug.
 */
void radiant_channel_bind_op(uint8_t channel, uint32_t op);

/* Which channel owns `op`, or -1. Never matches op == 0. */
int radiant_channel_op_owner(uint32_t op);

/*
 * A frame belonging to this channel was transmitted or received at t_sync.
 * Advances the slot clock to t_sync + one period and clears the missed-slot
 * counter - the only place the slot clock is re-anchored, keeping a slave's
 * window centred on the master it actually hears.
 *
 * Ignored unless TRACKING. A SEARCHING channel that hears something is an
 * acquisition (radiant_channel_on_acquired()); a CLOSING channel must not
 * have its clock re-anchored on the way out.
 */
void radiant_channel_on_slot(uint8_t channel, radiant_time_t t_sync);

/*
 * A SEARCHING slave acquired a master. Resolves the wildcards, moves the
 * channel to TRACKING, anchors the slot clock on t_sync and clears the search
 * deadline. Ignored unless the channel is SEARCHING.
 */
void radiant_channel_on_acquired(uint8_t channel, const struct radiant_channel_id *id,
			     radiant_time_t t_sync);

/*
 * A slot this channel expected produced nothing. Advances the slot clock by
 * one period from the expected instant - NOT from `now`, since a miss must
 * not shift the tracked phase - and counts it. After
 * RADIANT_CHANNEL_RX_FAIL_TO_SEARCH consecutive misses the channel returns
 * to SEARCHING and raises RX_FAIL_GO_TO_SEARCH. Returns true if that happened.
 */
bool radiant_channel_on_slot_missed(uint8_t channel, radiant_time_t now);

/*
 * A slot this channel expected was never put on the air because an
 * arbitrated backend lent the radio to another stack. See
 * RADIANT_RADIO_STATUS_DENIED / RADIANT_RADIO_EDENIED in radiant_radio_hal.h.
 *
 * Same clock arithmetic as a miss - t_next advances one period from the lost
 * slot, guard widens by DRIFT_WORST_US, since a period elapsed with no fresh
 * sync regardless of why. Deliberately a separate counter though: this is
 * not evidence the sensor stopped, it's evidence about us, so counting it as
 * a miss would drop the channel to SEARCHING visibly on the wire after eight
 * busy moments in the other stack, which the denial signal exists to
 * prevent - callers must not post RX_FAIL for it either. Bounded by
 * RADIANT_CHANNEL_DENY_TO_SEARCH, past which the guard has hit its ceiling
 * and this promotes into the miss path.
 *
 * Returns true if the channel went back to SEARCHING.
 */
bool radiant_channel_on_slot_denied(uint8_t channel, radiant_time_t now);

/* Consecutive denials on this channel since the last slot it heard. Zero on a
 * backend that owns the radio; readable so that a bench run can attribute a
 * widened guard to the arbiter rather than to the master's clock. */
uint8_t radiant_channel_denied_count(uint8_t channel);

/*
 * The terminal event of a radio operation. Call for EVERY terminal event,
 * including one whose op is bound to no channel - recognising the late
 * event is the point.
 *
 * A CLOSING channel completes its close here and raises CHANNEL_CLOSED.
 * Every other state just drops the binding; choosing the next window is
 * radiant_sched.c's business.
 *
 * Returns the channel the event was attributed to, or -1 if the op was stale.
 */
int radiant_channel_on_terminal(uint32_t op, enum radiant_radio_status status,
			    radiant_time_t now);

/*
 * Deadlines: run whenever the scheduler wakes and at least once per shortest
 * channel period.
 *
 * A SEARCHING channel past its deadline raises RX_SEARCH_TIMEOUT then closes,
 * raising CHANNEL_CLOSED too - both, in that order, since a search timeout
 * that doesn't close the channel leaves it un-unassignable forever. A
 * channel with an operation still bound goes to CLOSING instead and
 * finishes when the terminal event lands.
 *
 * Returns the number of channels that timed out.
 */
uint32_t radiant_channel_tick(radiant_time_t now);

/* ---------------------------------------------------------------------------
 * Counters, for tests and for a diagnostic line
 * ---------------------------------------------------------------------------
 */

/* Terminal or data events that arrived carrying an operation id no channel
 * owns. Expected to be non-zero on a busy link; a test asserts it goes up by
 * exactly one when it injects a late event. */
uint32_t radiant_channel_stale_op_count(void);

/* Channels closed by a search timeout since init. */
uint32_t radiant_channel_search_timeout_count(void);

/* ---------------------------------------------------------------------------
 * Unit conversion, exposed because radiant_sched.c and the tests both need it and
 * two implementations of it is one more than the number that can be right.
 * ---------------------------------------------------------------------------
 */

/*
 * 32768 Hz counts to microseconds, rounded to nearest.
 *
 *     us = counts * 1000000 / 32768 = counts * 15625 / 512
 *
 * Exact for any multiple of 512 counts and within half a microsecond
 * otherwise. 8182 counts -> 249695 us; 8192 -> 250000.
 */
static inline radiant_time_t radiant_channel_counts_to_us(uint32_t counts)
{
	return (((radiant_time_t)counts * 15625u) + 256u) / 512u;
}

/* Search timeout ticks (2.5 s each) to microseconds. Not defined for
 * RADIANT_CHANNEL_SEARCH_INFINITE - callers test for that first. */
static inline radiant_time_t radiant_channel_search_ticks_to_us(uint8_t ticks)
{
	return (radiant_time_t)ticks * RADIANT_CHANNEL_SEARCH_TICK_US;
}

/* ---------------------------------------------------------------------------
 * The agreement with radiant_wire.h
 *
 * Unconditional, unlike the app-side src/ant_wire.h cross-check this replaced
 * (which only fired in the one translation unit - radiant_api.c - that
 * happened to have included that header first). radiant_wire.h is
 * module-owned, so every translation unit that includes this header gets it
 * for free, and the literals above can never drift from the generated values
 * without a build failure naming exactly which one.
 * ---------------------------------------------------------------------------
 */
_Static_assert(RADIANT_CH_OK == RADIANT_WIRE_RESPONSE_NO_ERROR, "wire code drift: OK");
_Static_assert(RADIANT_CH_ERR_WRONG_STATE == RADIANT_WIRE_CHANNEL_IN_WRONG_STATE,
	       "wire code drift: CHANNEL_IN_WRONG_STATE");
_Static_assert(RADIANT_CH_ERR_NOT_OPENED == RADIANT_WIRE_CHANNEL_NOT_OPENED,
	       "wire code drift: CHANNEL_NOT_OPENED");
_Static_assert(RADIANT_CH_ERR_ID_NOT_SET == RADIANT_WIRE_CHANNEL_ID_NOT_SET,
	       "wire code drift: CHANNEL_ID_NOT_SET");
_Static_assert(RADIANT_CH_ERR_INVALID_MESSAGE == RADIANT_WIRE_INVALID_MESSAGE,
	       "wire code drift: INVALID_MESSAGE");
_Static_assert(RADIANT_CH_ERR_INVALID_NETWORK == RADIANT_WIRE_INVALID_NETWORK_NUMBER,
	       "wire code drift: INVALID_NETWORK_NUMBER");
_Static_assert(RADIANT_CH_ERR_INVALID_PARAM == RADIANT_WIRE_INVALID_PARAMETER_PROVIDED,
	       "wire code drift: INVALID_PARAMETER_PROVIDED");
_Static_assert(RADIANT_CH_EVENT_RX_SEARCH_TIMEOUT == RADIANT_WIRE_EVENT_RX_SEARCH_TIMEOUT,
	       "wire code drift: EVENT_RX_SEARCH_TIMEOUT");
_Static_assert(RADIANT_CH_EVENT_CHANNEL_CLOSED == RADIANT_WIRE_EVENT_CHANNEL_CLOSED,
	       "wire code drift: EVENT_CHANNEL_CLOSED");
_Static_assert(RADIANT_CH_EVENT_RX_FAIL_GO_TO_SEARCH ==
		       RADIANT_WIRE_EVENT_RX_FAIL_GO_TO_SEARCH,
	       "wire code drift: EVENT_RX_FAIL_GO_TO_SEARCH");
_Static_assert(RADIANT_CH_STATUS_UNASSIGNED == RADIANT_WIRE_STATUS_UNASSIGNED_CHANNEL,
	       "wire code drift: STATUS_UNASSIGNED_CHANNEL");
_Static_assert(RADIANT_CH_STATUS_ASSIGNED == RADIANT_WIRE_STATUS_ASSIGNED_CHANNEL,
	       "wire code drift: STATUS_ASSIGNED_CHANNEL");
_Static_assert(RADIANT_CH_STATUS_SEARCHING == RADIANT_WIRE_STATUS_SEARCHING_CHANNEL,
	       "wire code drift: STATUS_SEARCHING_CHANNEL");
_Static_assert(RADIANT_CH_STATUS_TRACKING == RADIANT_WIRE_STATUS_TRACKING_CHANNEL,
	       "wire code drift: STATUS_TRACKING_CHANNEL");
_Static_assert(RADIANT_CH_STATUS_STATE_MASK == RADIANT_WIRE_STATUS_CHANNEL_STATE_MASK,
	       "wire code drift: STATUS_CHANNEL_STATE_MASK");
_Static_assert(RADIANT_CH_TYPE_MASTER_BIT == RADIANT_WIRE_CHANNEL_TYPE_MASTER,
	       "wire code drift: CHANNEL_TYPE_MASTER");

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_CHANNEL_H_ */
