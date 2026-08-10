/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_channel.h - the per-channel state machine, its configuration, and the
 * slot clock every other core module reads.
 *
 * Provenance: clean-room. Written from docs/sdk-ant-contract.md and
 * src/ant_radio.h (the semantics, the ordering constraints and the permitted
 * error codes each entry point may return), from the free ANT Message Protocol
 * and Usage Rev 5.1 (D00000652) for the channel-status byte layout and the
 * 32768 Hz period unit, and from the bench measurements in
 * docs/ant-radio-link.md and docs/spike-b-part2-results.md (the master's
 * open-to-first-transmission delay, the slot period and its jitter). Nothing
 * here derives from sdk-ant, from libant.a, from disassembly of any binary, or
 * from any adopter-gated ANT+ device profile document. See
 * docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * What this module is, and what it deliberately is not
 * ---------------------------------------------------------------------------
 * This is the layer with no radio in it and no scheduler in it. It owns:
 *
 *   - what state each of the 32 channels is in, and every legal and illegal
 *     transition between those states, with the exact wire response code for
 *     each refusal;
 *   - every per-channel configuration byte the host can write or read back;
 *   - the channel's *slot clock*: the absolute time of the next slot this
 *     channel wants the radio for, and the absolute time its search gives up;
 *   - which radio operation id is currently attributed to this channel, which
 *     is the whole of the defence against a cancelled operation's terminal
 *     event arriving after the core moved on.
 *
 * It calls no HAL function. Not one. Arming the radio is radiant_sched.c's job,
 * and the split is not tidiness: a channel that armed its own windows could
 * not be merged with another channel's window, and merged windows are the
 * single highest-value item in the scheduler (see struct radiant_rx_req in
 * radiant_radio_hal.h). So this module *decides* and radiant_sched.c *acts*, and the
 * seam between them is the handful of radiant_channel_on_*() / radiant_channel_next_*
 * functions at the bottom of this file.
 *
 * The practical consequence, which is worth knowing before reading
 * radiant_core/tests/src/test_channel.c: this module is testable with no radio at
 * all. The suite still drives fake_radio, because it must show that the
 * decisions this module hands the scheduler are ones a real backend accepts -
 * that a master's first transmit really is armable one period out, that a
 * close with an operation in flight really does survive the terminal event
 * that follows the abort - but every assertion about state is a pure-function
 * assertion.
 *
 * ---------------------------------------------------------------------------
 * Thirty-two channels, and why the number is here rather than in a Kconfig
 * ---------------------------------------------------------------------------
 * 32 is the serial protocol's own ceiling: the burst header carries the
 * channel number in the low five bits of the channel byte, so 32 is the
 * largest count that can be addressed at all. The plan requires it baked in
 * from the first line rather than retrofitted, because a channel-count
 * assumption threaded through a scheduler is far more expensive to remove
 * later than to size correctly now. Everything below is a flat array of 32;
 * there is no allocation anywhere in this module.
 *
 * ---------------------------------------------------------------------------
 * The error codes are wire bytes
 * ---------------------------------------------------------------------------
 * Every function here that radiant_api.c forwards returns radiant_channel_err_t, which
 * is a uint8_t whose value IS the byte the serial protocol puts on the wire.
 * That is src/ant_radio.h's convention and it is not negotiable: a code the
 * host does not recognise is worse than a wrong-but-valid one, because a host
 * library typically hangs waiting for a reply it can parse.
 *
 * The values are spelled as literals here rather than taken from
 * src/ant_wire.h, because this header must compile against nothing but
 * radiant_core/include - the gate is
 *
 *   arm-zephyr-eabi-gcc -c -std=c11 -Wall -Wextra -Werror \
 *       -I radiant_core/include -I radiant_core/tests -fsyntax-only \
 *       radiant_core/src/radiant_channel.c
 *
 * and pulling src/ant_wire.h in would make the link layer depend on the serial
 * layer's generated header for no gain. Two spellings of one constant is
 * exactly how a reply gets framed wrong, so the divergence is checked instead
 * of trusted: the _Static_assert block at the bottom of this file fires in any
 * translation unit that has already included src/ant_wire.h. radiant_api.c
 * includes both and must include ant_wire.h first; that is the check, and it
 * runs in the firmware build where both headers are present.
 */

#ifndef RADIANT_CHANNEL_H_
#define RADIANT_CHANNEL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant_core/radiant_frame.h>     /* struct radiant_channel_id, and the frame geometry */
#include <radiant_core/radiant_radio_hal.h> /* radiant_time_t, enum radiant_radio_status */

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
 * expects to find and what the ANT+ key at network 0 needs one of; the other
 * two exist because a host that configures network 1 and gets
 * INVALID_NETWORK_NUMBER concludes the dongle is broken rather than that it is
 * frugal. ant_net.c owns the keys; this module owns only the range check,
 * because antr_channel_assign() has to refuse a bad network before any key
 * lookup happens.
 */
#define RADIANT_CHANNEL_NETWORK_COUNT 3u

/* ---------------------------------------------------------------------------
 * Response codes - the wire bytes, checked against src/ant_wire.h at the
 * bottom of this file wherever that header is in scope.
 * ---------------------------------------------------------------------------
 */
typedef uint8_t radiant_channel_err_t;

#define RADIANT_CH_OK                 0x00u /* ANTW_RESPONSE_NO_ERROR */
#define RADIANT_CH_ERR_WRONG_STATE    0x15u /* ANTW_CHANNEL_IN_WRONG_STATE */
#define RADIANT_CH_ERR_NOT_OPENED     0x16u /* ANTW_CHANNEL_NOT_OPENED */
#define RADIANT_CH_ERR_ID_NOT_SET     0x18u /* ANTW_CHANNEL_ID_NOT_SET */
#define RADIANT_CH_ERR_INVALID_MESSAGE 0x28u /* ANTW_INVALID_MESSAGE */
#define RADIANT_CH_ERR_INVALID_NETWORK 0x29u /* ANTW_INVALID_NETWORK_NUMBER */
#define RADIANT_CH_ERR_INVALID_PARAM  0x33u /* ANTW_INVALID_PARAMETER_PROVIDED */

/*
 * The two distinctions docs/sdk-ant-contract.md calls out as load-bearing and
 * host-observable, restated here because this is where they are produced:
 *
 *   - a channel NUMBER out of range is RADIANT_CH_ERR_INVALID_MESSAGE; a bad
 *     parameter value is RADIANT_CH_ERR_INVALID_PARAM;
 *   - RADIANT_CH_ERR_WRONG_STATE is a channel that exists in the wrong state;
 *     RADIANT_CH_ERR_NOT_OPENED is a data-transfer call on a channel that is not
 *     on air. They are not interchangeable, and tools/ant_conformance.py
 *     byte-diffs them.
 */

/* ---------------------------------------------------------------------------
 * Channel events this module raises
 *
 * These are ANTW_EVENT_* codes and go to the host through radiant_event.c. They
 * are listed here rather than taken from src/ant_wire.h for the same reason
 * the response codes are, and are asserted against it in the same block.
 * ---------------------------------------------------------------------------
 */
#define RADIANT_CH_EVENT_RX_SEARCH_TIMEOUT   0x01u /* ANTW_EVENT_RX_SEARCH_TIMEOUT */
#define RADIANT_CH_EVENT_CHANNEL_CLOSED      0x07u /* ANTW_EVENT_CHANNEL_CLOSED */
#define RADIANT_CH_EVENT_RX_FAIL_GO_TO_SEARCH 0x08u /* ANTW_EVENT_RX_FAIL_GO_TO_SEARCH */

/* ---------------------------------------------------------------------------
 * Channel status byte, and channel types
 * ---------------------------------------------------------------------------
 */

/* Low two bits of a MESG_CHANNEL_STATUS reply. */
#define RADIANT_CH_STATUS_UNASSIGNED 0x00u /* ANTW_STATUS_UNASSIGNED_CHANNEL */
#define RADIANT_CH_STATUS_ASSIGNED   0x01u /* ANTW_STATUS_ASSIGNED_CHANNEL */
#define RADIANT_CH_STATUS_SEARCHING  0x02u /* ANTW_STATUS_SEARCHING_CHANNEL */
#define RADIANT_CH_STATUS_TRACKING   0x03u /* ANTW_STATUS_TRACKING_CHANNEL */
#define RADIANT_CH_STATUS_STATE_MASK 0x03u /* ANTW_STATUS_CHANNEL_STATE_MASK */

/*
 * The rest of the byte, per Rev 5.1 section 9.5.7.1: bits 2:3 are the network
 * number and bits 4:7 are the channel type. The type values already live in
 * the top nibble (SLAVE 0x00 .. MASTER_TX_ONLY 0x50), so the type is copied
 * across rather than shifted, and getting that wrong is invisible until a host
 * reads a channel type of zero on a live master.
 */
#define RADIANT_CH_STATUS_NETWORK_SHIFT 2u
#define RADIANT_CH_STATUS_NETWORK_MASK  0x0Cu
#define RADIANT_CH_STATUS_TYPE_MASK     0xF0u

/*
 * Bit 4 of the channel type is the master bit, and it is set in exactly the
 * three master types (MASTER 0x10, SHARED_MASTER 0x30, MASTER_TX_ONLY 0x50)
 * and clear in exactly the three slave types (SLAVE 0x00, SHARED_SLAVE 0x20,
 * SLAVE_RX_ONLY 0x40). That is the only bit of the type byte this module
 * interprets: it decides whether opening the channel starts a transmitter or
 * a search, and nothing else here cares.
 *
 * An UNRECOGNISED type byte is accepted rather than refused, and that is a
 * deliberate reading of the contract rather than laxity: antr_channel_assign()
 * may return only NO_ERROR, INVALID_MESSAGE (channel out of range),
 * CHANNEL_IN_WRONG_STATE and INVALID_NETWORK_NUMBER, so there is no permitted
 * code left with which to reject a type. Inventing one would be a wire-visible
 * defect. See the report against docs/sdk-ant-contract.md.
 */
#define RADIANT_CH_TYPE_MASTER_BIT 0x10u /* ANTW_CHANNEL_TYPE_MASTER */

/* ---------------------------------------------------------------------------
 * Time
 * ---------------------------------------------------------------------------
 */

/*
 * The channel period is in 32768 Hz counts, which is the radio's own timebase
 * and not a millisecond tick - a backend that rounded it to milliseconds would
 * walk a slave's receive window out of the master's slot within a minute.
 *
 * 8182 counts is the ANT+ 4.005 Hz default and is what the bench measured:
 * 249,694.8 us nominal against a measured mean of 249,696.4 us over 744
 * intervals, sd 5.4-6.5 us (docs/ant-radio-link.md). Note that this is NOT
 * 8192: src/ant_radio.h's comment calls 8192 "the 4 Hz that most ANT+ profiles
 * use", and 8192 counts is 250,000 us exactly, ten counts and 305 us away from
 * what a real ANT+ master transmits at. Both numbers appear in the tree; only
 * one has been on the air here.
 */
#define RADIANT_CHANNEL_PERIOD_HZ         32768u
#define RADIANT_CHANNEL_PERIOD_ANT_PLUS   8182u  /* 4.005 Hz, measured */

/*
 * The schedulable range.
 *
 * A period of zero is not a period, and the top of the range is simply what a
 * uint16_t holds (2.0 s). The floor is derived from the bench rather than
 * chosen: a slave answers its master 2.19 ms after the master's address, an
 * acknowledged exchange runs at ~1.55 ms each way, and a burst sustains one
 * packet per ~3.11 ms (docs/ant-radio-link.md, part 2 turnarounds). A period
 * shorter than about 8 ms therefore cannot contain one slot's exchange, so
 * accepting it would mean promising a schedule the radio can never keep. 273
 * counts is 8.331 ms.
 *
 * radiant_sched.c may raise the floor at init through
 * radiant_channel_period_floor_set() once it has read caps.min_arm_lead_us from
 * the backend, because the real bound is a backend property and this one is
 * only the protocol's.
 */
#define RADIANT_CHANNEL_PERIOD_MIN_COUNTS 273u
#define RADIANT_CHANNEL_PERIOD_MAX_COUNTS 65535u

/* Search timeouts are in 2.5 s increments. 0 disables that priority's search;
 * 255 means "search forever" and is the one value that is not a duration. */
#define RADIANT_CHANNEL_SEARCH_TICK_US    2500000u
#define RADIANT_CHANNEL_SEARCH_INFINITE   255u

/* ANT's default high-priority search timeout is 10 ticks = 25 s. The wildcard
 * sweep's worst case is 8.3 s and its average about 4 s
 * (docs/ant-radio-link.md), so a full sweep fits inside the default with room
 * to spare - which is why radiant_search.c may size its sweep without negotiating
 * with this module. */
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
 * `[inferred]`. Nothing on this bench has measured ANT's own threshold - doing
 * so needs a master that can be silenced mid-session - so this is a chosen
 * value, not a recovered one. Eight slots is 2.0 s at the ANT+ period, which
 * is long enough that the characterised ~0.4% per-slot collision floor
 * (docs/ant-bench-loss-floor, docs/testing.md) cannot plausibly produce eight
 * in a row, and short enough that a rider notices the re-acquisition rather
 * than the dropout. radiant_sched.c may override it per build if a measurement
 * ever supersedes the guess.
 */
#define RADIANT_CHANNEL_RX_FAIL_TO_SEARCH 8u

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
 * +/-50 ppm at each end (sdk-ant's compatibility.rst, and not proprietary to
 * it - any ANT datasheet states the same figure) is +/-100 ppm relative; over
 * one RADIANT_CHANNEL_PERIOD_ANT_PLUS period (~249.7 ms) that is +/-25 us THAT
 * PERIOD ALONE could add, with no accumulated drift from a previous miss.
 *
 * A BUDGET, NOT HEADROOM. A master built to the tolerance limit sits exactly on
 * this figure. It is the per-miss term of the guard below for that reason: an
 * extrapolated period is one whose real error is unmeasured, so it is charged
 * at the spec bound rather than at what this master has been doing.
 */
#define RADIANT_CHANNEL_DRIFT_WORST_US 25u

/*
 * The widest the guard ever gets, and the value it takes whenever the estimator
 * has nothing to say: RADIANT_CHANNEL_RX_FAIL_TO_SEARCH * the figure above, with
 * margin. This was the whole guard before the estimator existed.
 */
#define RADIANT_CHANNEL_GUARD_MAX_US 400u

/*
 * The narrowest the guard ever gets, before radiant_channel_guard_floor_set()
 * raises it for a backend that needs more.
 *
 * DELIBERATELY FAR ABOVE WHAT THE MEASUREMENT JUSTIFIES. The measured residual
 * on this bench is 0.009 ms - nine microseconds - so eleven times that is not a
 * tight fit, it is a refusal to fit tightly. The failure mode of a guard that
 * is too narrow is the silent one radiant_radio_hal.h documents at length: yield
 * falls at the same order as the collision floor and no error code is raised
 * anywhere. There is no measurement on this bench that would tell the two
 * apart, so the floor is set where it does not need one.
 *
 * Lower it only against a Phase 0 sensitivity ladder that shows what it costs.
 */
#define RADIANT_CHANNEL_GUARD_MIN_US 100u

/*
 * How many measured residuals the guard is sized at. Four, so an ordinary
 * excursion to several times the running average is still comfortably inside
 * the window before the floor is even reached.
 */
#define RADIANT_CHANNEL_GUARD_K 4u

/*
 * Consecutive clean slots before the estimator is allowed to narrow anything.
 *
 * An unlocked estimator must never narrow a window: at acquisition the residual
 * has been measured zero times, and a guard sized from no evidence is a guard
 * sized from luck. Eight is one EWMA time constant at the 1/8 weight used
 * below, so by the time it is trusted the average is genuinely an average.
 */
#define RADIANT_CHANNEL_GUARD_LOCK_SLOTS 8u

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------
 */

/*
 * The five states, and why there are five rather than the four the wire names.
 *
 *   UNASSIGNED --assign--> ASSIGNED --open--> SEARCHING --acquire--> TRACKING
 *        ^                    |  ^                |                     |
 *        +----unassign--------+  +----------------+---------------------+
 *                                          via CLOSING
 *
 * "CONFIGURED" is deliberately NOT a state. src/ant_radio.h requires
 * antr_channel_open_with_offset() to answer ANTW_CHANNEL_ID_NOT_SET when the
 * channel ID was never set, which reads like a state, but every other
 * configuration call is legal both before and after the ID is written and the
 * wire has no status value for it. It is a flag on an ASSIGNED channel, and
 * modelling it as a state would have produced a transition table with a dozen
 * entries that all mean "still ASSIGNED".
 *
 * CLOSING is the state the wire does not name and the contract requires:
 * "the channel is not closed when this returns, it is closing", and the host
 * learns it is done from ANTW_EVENT_CHANNEL_CLOSED. A channel with a radio
 * operation in flight cannot become ASSIGNED the instant close() returns,
 * because the operation's terminal event has not arrived yet and unassigning
 * underneath it would release state the backend still refers to. So close()
 * parks the channel here, and the terminal event completes the transition.
 *
 * radiant_channel_status_get() reports CLOSING as whatever it was closing FROM -
 * SEARCHING or TRACKING - not as ASSIGNED. Reporting ASSIGNED early would tell
 * a host it may unassign a channel that radiant_channel_unassign() will then
 * refuse, and a status byte that disagrees with the next call's return code is
 * worse than one that lags by a millisecond.
 */
enum radiant_channel_state {
	RADIANT_CH_STATE_UNASSIGNED = 0,
	RADIANT_CH_STATE_ASSIGNED,
	RADIANT_CH_STATE_SEARCHING,
	RADIANT_CH_STATE_TRACKING,
	RADIANT_CH_STATE_CLOSING,
	RADIANT_CH_STATE_COUNT
};

/*
 * There is no stored "why am I closing" field, and that is worth stating
 * because the obvious design has one. The events that distinguish the three
 * reasons are all raised at the moment the reason is known - a search timeout
 * raises RADIANT_CH_EVENT_RX_SEARCH_TIMEOUT immediately and only the trailing
 * RADIANT_CH_EVENT_CHANNEL_CLOSED is deferred - so by the time the terminal event
 * completes the close there is nothing left for the reason to select.
 */

/* ---------------------------------------------------------------------------
 * The event sink - implemented elsewhere, resolved at link time
 * ---------------------------------------------------------------------------
 */

/*
 * One channel event, on its way to the host.
 *
 * IMPLEMENTED BY radiant_event.c, NOT BY radiant_channel.c, and by a test double in
 * radiant_core/tests/src/test_channel.c. This mirrors the inversion src/ant_radio.h
 * already uses for antr_on_message(): there is no registration call, the
 * symbol is resolved at link time, and exactly one translation unit in the
 * image defines it. A registration pointer would buy nothing here - there is
 * never more than one consumer - and would cost a null check on every event.
 *
 * @param channel     0 .. RADIANT_CHANNEL_COUNT-1.
 * @param event_code  An RADIANT_CH_EVENT_* value, which is an ANTW_EVENT_* wire
 *                    byte. radiant_event.c wraps it in a channel-response message.
 *
 * Called from whatever context provoked the transition: the bridge's parser
 * thread for a host-initiated close, and radiant_sched.c's context - which may be
 * a radio callback - for a search timeout or a completed close. The
 * implementation must therefore obey the tighter of the two contracts and not
 * block. It must not call back into radiant_channel.
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
 * says so, and the bridge discards them for a short window afterwards because
 * a real stick emits nothing between the reset command and the startup
 * message. Suppressing them here instead would make the backend's behaviour
 * depend on a bridge-side timing window it cannot see.
 *
 * Any operation in flight is orphaned rather than waited on: this module has
 * no way to abort one, so radiant_sched.c must call radiant_radio_abort() before this
 * and feed the terminal event in afterwards. A terminal event for an orphaned
 * op arrives at a channel with no binding and is counted as stale, which is
 * exactly the same path a late event takes.
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
 * Assignment puts nothing on air and resets every configuration byte to its
 * default, including clearing the channel ID: a channel reassigned to a
 * different type must not inherit the last session's device number, because a
 * host that assigns and opens without setting an ID would then silently track
 * the wrong sensor instead of getting RADIANT_CH_ERR_ID_NOT_SET.
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
 * (never assigned, still open, or still closing). Closing it first would
 * swallow the ANTW_EVENT_CHANNEL_CLOSED the host is waiting on, which is why
 * this refuses rather than obliges.
 */
radiant_channel_err_t radiant_channel_unassign(uint8_t channel);

/*
 * antr_channel_open_with_offset(). The channel must be ASSIGNED with its ID
 * set.
 *
 * @param offset  The one-shot start offset, in the same 32768 Hz counts the
 *                period uses.
 * @param now     radiant_radio_now(). Passed in rather than read here, because
 *                this module calls no HAL function and because a test must be
 *                able to open a channel at a chosen instant.
 *
 * A MASTER's first transmission is scheduled one FULL CHANNEL PERIOD after
 * this call, plus `offset`. That is measured, not assumed: time from
 * MESG_OPEN_CHANNEL to the first EVENT_TX was one full period in 8 of 8 runs
 * (docs/ant-radio-link.md, "Master open-time"). A master does not transmit
 * when you open it. Reproducing that is this function's job, and a host that
 * expects data immediately after opening a master is going to wait a slot.
 *
 * The offset is applied ON TOP of that period rather than instead of it, which
 * is a reading of the contract and not a quotation from it - see the report.
 * The alternative reading makes offset 0 mean "transmit now", which the
 * measurement falsifies.
 *
 * A SLAVE enters SEARCHING with its first receive window at now + offset and
 * its search deadline at the high-priority timeout beyond that. There is no
 * period wait: a searching slave has nothing to align to yet.
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
 * If no radio operation is bound to the channel the close completes here and
 * RADIANT_CH_EVENT_CHANNEL_CLOSED is raised before this returns. If one is bound,
 * the channel enters RADIANT_CH_STATE_CLOSING and the event is raised when
 * radiant_channel_on_terminal() delivers that operation's terminal event - which
 * WILL arrive, aborted or not, because the HAL guarantees exactly one terminal
 * event per accepted operation and delivers it even for a cancelled one.
 *
 * Either way the event is raised exactly once per successful close. A host
 * library that reopens on the event hangs without it.
 *
 * Returns RADIANT_CH_OK, RADIANT_CH_ERR_INVALID_MESSAGE or RADIANT_CH_ERR_WRONG_STATE
 * (not open). Calling it twice returns RADIANT_CH_ERR_WRONG_STATE the second time
 * rather than raising a second event.
 */
radiant_channel_err_t radiant_channel_close(uint8_t channel, radiant_time_t now);

/* ---------------------------------------------------------------------------
 * Configuration - legal only on an ASSIGNED, closed channel unless said
 * otherwise
 *
 * The rule and its reason, from docs/sdk-ant-contract.md: the ID, the period,
 * the RF frequency, the CRC mode and the frequency-hop table all retune the
 * radio, and retuning underneath a live host session is worse than refusing.
 * The search and power settings do not retune anything, so they need only an
 * assigned channel.
 * ---------------------------------------------------------------------------
 */

/* antr_channel_id_set(). Zero is the wildcard on a slave, in all three fields.
 * Returns OK, INVALID_MESSAGE, or WRONG_STATE (unassigned or open). */
radiant_channel_err_t radiant_channel_id_set(uint8_t channel, uint16_t device_number,
				     uint8_t device_type, uint8_t trans_type);

/*
 * antr_channel_id_get(). On a TRACKING slave this reports the ID actually
 * acquired, with the wildcards resolved, which is how a host learns which
 * sensor it found; otherwise it reports what was configured.
 *
 * The acquired ID is kept separately from the configured one and is discarded
 * when the channel closes. A channel reopened after a wildcard search must
 * search again rather than silently narrow to last session's sensor, and the
 * host can still read the ID it found for as long as the channel is up.
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
 * antr_channel_radio_tx_power_set(). Requires an ASSIGNED channel and is legal
 * while open - power is the one radio property that does not move the channel
 * off the air to change.
 *
 * Bit 7 of `level` (0x80) means `custom` names a raw part-specific value
 * instead. Levels 0..5 are -20, -12, -4, 0, +4 and +8 dBm; level 5 exists only
 * on the nRF52820/52833/52840, and this module accepts it because whether the
 * part can produce it is a backend question that radiant_sched.c answers against
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
 * antr_channel_radio_crc_mode_set(). The byte is opaque - src/ant_wire.h names
 * no constants for it, because nothing in this project sends anything but the
 * default - so it is validated against this module's supported set, which is
 * exactly {RADIANT_CH_CRC_MODE_DEFAULT}. A backend that accepted and ignored a
 * shorter CRC would give a host a link that works and framing that does not.
 * Returns OK, INVALID_MESSAGE, WRONG_STATE or INVALID_PARAM.
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
 * radiant_search.c owns the sweep, the seen-cache and scan mode; it does not own
 * the per-channel bytes the host writes, because those share this module's
 * state machine and its "assigned channel" precondition. It reads them back
 * through the getters below.
 * ---------------------------------------------------------------------------
 */

/*
 * antr_search_waveform_set(). Stored and never read - radiant_search.c has no
 * duty-cycle switch, and this backend runs one search geometry. Nothing here
 * refuses the setting outright, because sdk-ant names exactly two legal
 * values for it - 316, the default window width, and 97, the fast one - and
 * says "Do not use custom values." A two-value enum is a cheap contract to
 * keep even while ignoring what it selects: accept the two real values,
 * refuse everything else, rather than storing an arbitrary uint16_t that
 * could never have meant anything. See docs/sdk-ant-comparison.md item 2.
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
 * antr_prox_search_set(). PERMANENT LIMITATION, not a gap: nothing reads
 * prox_threshold back - radiant_search.c's own RSSI gate is cfg.min_rssi_dbm,
 * and nothing wires a channel's threshold to it - and the capability bit
 * ANTW_CAPABILITIES_PROX_SEARCH_ENABLED is not advertised by
 * antr_capabilities_get(). Storing a non-zero threshold that nothing enforces
 * is the trap docs/gotchas.md warns against: a host that asked for proximity
 * pairing would believe distant sensors are being filtered when none are. So
 * only 0 (off) is accepted - it asks for nothing this stack does not already
 * do - and any non-zero threshold is refused rather than silently dropped.
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
 * antr_sdu_mask_config(): the per-channel half of selective data update. Only
 * the channel range is checked here and the byte is stored verbatim; the
 * mask-index range check that produces INVALID_PARAM belongs to whoever owns
 * the mask table, because that is where the mask count lives.
 * Returns OK or INVALID_MESSAGE.
 */
#define RADIANT_CH_SDU_MASK_OFF 0xFFu /* ANTW_INVALID_SDU_MASK */
radiant_channel_err_t radiant_channel_sdu_mask_config_set(uint8_t channel,
						  uint8_t mask_config);
radiant_channel_err_t radiant_channel_sdu_mask_config_get(uint8_t channel,
						  uint8_t *out);

/* ---------------------------------------------------------------------------
 * Status
 * ---------------------------------------------------------------------------
 */

/*
 * antr_channel_status_get(). Legal on an unassigned channel - that is the
 * answer, not an error - which is why every host opens a session by reading
 * it. Byte layout: state in bits 1:0, network in bits 3:2, channel type in
 * bits 7:4.
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
 * When this channel next wants the radio: the t_sync of its next slot for a
 * master, the opening edge of its next receive window for a slave.
 * RADIANT_TIME_NEVER when the channel is not on air.
 *
 * radiant_sched.c subtracts caps.ramp_up_us and caps.min_arm_lead_us; those are
 * backend properties and none of them belong here. How far either side of this
 * instant the window reaches is radiant_channel_guard_us() below, and the
 * argument for that number - including why it is no longer the constant this
 * paragraph used to describe - is on its declaration.
 *
 * THE SPEC NUMBER IS +/-25 US, AND IT IS A BUDGET, NOT HEADROOM.
 * RADIANT_CHANNEL_DRIFT_WORST_US carries it; consecutive misses compound it,
 * because radiant_channel_on_slot_missed() extrapolates t_next by one more
 * period per miss without a fresh sync, so N consecutive misses carry up to
 * N * 25 us of additional worst-case disagreement by the time a receive window
 * is next armed against this t_sync. The two BUILD_ASSERTs in radiant_api.c
 * turn that into a checked invariant instead of a claim - one for the ceiling
 * the estimator falls back to, one for the floor it may narrow to.
 */
radiant_time_t radiant_channel_next_slot(uint8_t channel);

/*
 * How far either side of that instant this channel's receive window should
 * reach, in microseconds. Never zero; never above RADIANT_CHANNEL_GUARD_MAX_US.
 *
 * WHY THIS IS NOT A CONSTANT ANY MORE. The paragraph above sizes the guard from
 * the spec's worst case, compounded over eight misses, and arrives at 400 us.
 * The residual this bench actually measures - t_sync minus the predicted
 * instant, on the radio's own clock - is 9 us. Every tracked slot was therefore
 * buying 800 us of receive to cover about nine microseconds of real error.
 *
 * That is not free. Window occupancy is what turns into contention between
 * tracked channels, into aborted scan chunks, and into receive current for
 * anything running this core on a battery; a window forty times wider than it
 * needs to be also arms the address matcher against forty times as much noise.
 *
 * So: measure the master rather than assume the spec bound. The channel keeps
 * an exponential average of |t_sync - predicted| across clean consecutive
 * slots, and the guard is
 *
 *     clamp(floor,
 *           K * average + miss_count * DRIFT_WORST,
 *           GUARD_MAX)
 *
 * Two terms, for two different things. The first is what this master has been
 * measured doing, times a safety factor. The second is what an extrapolated
 * period could be doing and has not been measured doing - after a miss the
 * clock has run one more period with no fresh sync, so that period is charged
 * at the spec bound. Together they mean a well-behaved link runs on a narrow
 * window and reopens the old wide one automatically as it starts to lose slots.
 *
 * RADIANT_CHANNEL_GUARD_MAX_US, unconditionally, whenever the estimator has
 * nothing to say: on a channel that is not TRACKING, immediately after
 * acquisition, and after RX_FAIL_GO_TO_SEARCH. Narrowing on no evidence is the
 * one thing this must never do, because the failure mode has no error code.
 *
 * ONE ESTIMATOR, NOT TWO, AND THE SECOND ONE'S ABSENCE IS A DECISION.
 *
 * The obvious companion to this is an estimate of the master's PERIOD - a
 * fixed-point ppm correction, averaged the same way - so that t_next could be
 * extrapolated at the master's real rate instead of the nominal one. It is not
 * here, and it was left out rather than forgotten:
 *
 *   - It has no consumer. radiant_channel_on_slot() re-anchors on every frame
 *     that arrives, so a period correction changes nothing at all except across
 *     MISSED slots - and the guard already charges every extrapolated period at
 *     the spec's worst case, which is strictly safer than charging it at an
 *     estimate.
 *   - Where it would act, it would act on the slot clock rather than on the
 *     window width. That is a behaviour change with exactly the silent failure
 *     mode this whole feature is written around: a period estimate that is
 *     slightly wrong walks the window off the master over minutes, with no
 *     error code anywhere, and the only instrument that could grade it is the
 *     Phase 0 sensitivity ladder, which has not been run yet.
 *   - struct chan is under a checked 72-byte budget at 32 copies. Carrying a
 *     second estimator that nothing reads spends that budget on nothing.
 *
 * If the residual EWMA ever stops converging on a real master - if
 * radiant_channel_residual_us() reads tens of microseconds on a link that is
 * not losing slots - that is the signal that this decision needs revisiting,
 * because a large steady residual is a period error by another name.
 *
 * WHY AN EWMA AND NOT A KALMAN FILTER. A Kalman filter is the right tool when
 * you want the optimal estimate of a state with known dynamics, and it would
 * genuinely be better here in one respect: it carries a variance, so the guard
 * could be sized as mean + k*sigma rather than as k*mean, which is what a
 * window that must cover nearly every residual actually wants.
 *
 * It buys nothing today, and that is measurable rather than an opinion. The
 * measured residual is 9 us; k*avg is 36 us; the floor is 100 us. THE CLAMP
 * DECIDES THE ANSWER, so a better estimate of the 36 produces exactly the same
 * window. Against that, a filter needs multiplies and a division in the radio
 * event path, in a module with no float and a checked 72-byte-per-channel
 * budget at 32 copies.
 *
 * The condition that changes the answer is concrete: if a Phase 0 sensitivity
 * ladder ever justifies dropping RADIANT_CHANNEL_GUARD_MIN_US to the same order
 * as k*avg, the estimator stops being masked and its quality starts to matter.
 * At that point the cheap step is not a Kalman filter but a decaying MAXIMUM
 * rather than a mean - the guard needs the worst residual, not the typical one,
 * and a decaying max estimates that directly at the same cost as this. A
 * two-state (phase, period) filter is the step after that, and it is the same
 * behaviour change - and the same silent failure mode - as the period estimator
 * described above.
 */
uint32_t radiant_channel_guard_us(uint8_t channel);

/*
 * Raise the floor of the above, in microseconds. Values below
 * RADIANT_CHANNEL_GUARD_MIN_US are ignored; the floor is a floor, and a caller
 * is not allowed to talk the core into a narrower window than its own minimum.
 *
 * Called once at init by whoever holds the backend capabilities, in the same
 * shape and for the same reason as radiant_channel_period_floor_set(): a
 * backend whose t_sync is inferred rather than captured
 * (caps.has_sync_timestamp == false) is not reporting the master's clock at all,
 * it is reporting its own inference, and an estimator fed with that must not be
 * allowed to narrow anything. Passing RADIANT_CHANNEL_GUARD_MAX_US pins the
 * guard wide and switches the whole mechanism off, which is the correct
 * behaviour for such a backend and needs no separate flag.
 */
void radiant_channel_guard_floor_set(uint16_t us);

/*
 * The running average above, in microseconds, or 0 on a channel whose estimator
 * has not locked. Diagnostic only - nothing in the core branches on it.
 *
 * It exists because the Phase 1 A/B needs the win to be visible as something
 * other than "loss did not get worse": this is the number that says how much of
 * the old 400 us was ever justified, and it is the first thing to read if a
 * narrowed window is ever suspected of costing packets.
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
 * `op` is the non-zero id radiant_radio_tx()/radiant_radio_rx() returned. Binding it
 * here is what makes a late terminal event recognisable: the HAL guarantees a
 * cancelled operation's terminal event still arrives, so an unbound op is not
 * an anomaly to log, it is the normal outcome of a close that raced a window.
 *
 * Passing op == 0 clears the binding. Binding an op to a channel that already
 * has a different one is a scheduler bug; the new binding wins and the counter
 * radiant_channel_stale_op_count() is NOT incremented, because that counter means
 * "an event arrived for an operation nobody owns" and conflating the two would
 * hide the scheduler bug behind an expected number.
 */
void radiant_channel_bind_op(uint8_t channel, uint32_t op);

/* Which channel owns `op`, or -1. Never matches op == 0. */
int radiant_channel_op_owner(uint32_t op);

/*
 * A frame belonging to this channel was transmitted or received at t_sync.
 *
 * Advances the slot clock to t_sync + one period and clears the missed-slot
 * counter. This is the only place the slot clock is re-anchored, which is what
 * keeps a slave's window centred on the master it actually hears rather than
 * on the time the host happened to open the channel.
 *
 * Ignored unless the channel is TRACKING. A SEARCHING channel that hears
 * something is an acquisition, not a slot, and goes through
 * radiant_channel_on_acquired(); a CLOSING channel is leaving and must not have
 * its clock re-anchored on the way out.
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
 * A slot this channel expected produced nothing.
 *
 * Advances the slot clock by one period from the expected instant - NOT from
 * `now`, because a missed slot must not shift the phase the channel is
 * tracking - and counts the miss. After RADIANT_CHANNEL_RX_FAIL_TO_SEARCH
 * consecutive misses a TRACKING channel returns to SEARCHING and
 * RADIANT_CH_EVENT_RX_FAIL_GO_TO_SEARCH is raised. Returns true if that happened.
 */
bool radiant_channel_on_slot_missed(uint8_t channel, radiant_time_t now);

/*
 * The terminal event of a radio operation.
 *
 * Call this for EVERY terminal event, including one whose op is bound to no
 * channel: recognising the late event is the point, and the count is what a
 * test asserts on.
 *
 * A CLOSING channel completes its close here and raises
 * RADIANT_CH_EVENT_CHANNEL_CLOSED. Every other state simply drops the binding -
 * the window closed, and choosing the next one is radiant_sched.c's business.
 *
 * Returns the channel the event was attributed to, or -1 if the op was stale.
 */
int radiant_channel_on_terminal(uint32_t op, enum radiant_radio_status status,
			    radiant_time_t now);

/*
 * Deadlines: run this whenever the scheduler wakes and at least once per
 * shortest channel period.
 *
 * A SEARCHING channel whose deadline has passed raises
 * RADIANT_CH_EVENT_RX_SEARCH_TIMEOUT and then closes, raising
 * RADIANT_CH_EVENT_CHANNEL_CLOSED as well - both, in that order, because that is
 * what a host library waits for and a search timeout that does not close the
 * channel leaves it un-unassignable forever.
 *
 * A channel with an operation still bound goes to CLOSING instead and finishes
 * when the terminal event lands, exactly as a host-initiated close does.
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
 * The agreement with src/ant_wire.h
 *
 * These fire only in a translation unit that included src/ant_wire.h BEFORE
 * this header - radiant_api.c, in the firmware build, which is required to. That
 * is deliberate: the check runs everywhere both headers exist, and this header
 * still compiles standalone against nothing but radiant_core/include, which is
 * what the module's own gate needs.
 * ---------------------------------------------------------------------------
 */
#ifdef ANTW_RESPONSE_NO_ERROR
_Static_assert(RADIANT_CH_OK == ANTW_RESPONSE_NO_ERROR, "wire code drift: OK");
_Static_assert(RADIANT_CH_ERR_WRONG_STATE == ANTW_CHANNEL_IN_WRONG_STATE,
	       "wire code drift: CHANNEL_IN_WRONG_STATE");
_Static_assert(RADIANT_CH_ERR_NOT_OPENED == ANTW_CHANNEL_NOT_OPENED,
	       "wire code drift: CHANNEL_NOT_OPENED");
_Static_assert(RADIANT_CH_ERR_ID_NOT_SET == ANTW_CHANNEL_ID_NOT_SET,
	       "wire code drift: CHANNEL_ID_NOT_SET");
_Static_assert(RADIANT_CH_ERR_INVALID_MESSAGE == ANTW_INVALID_MESSAGE,
	       "wire code drift: INVALID_MESSAGE");
_Static_assert(RADIANT_CH_ERR_INVALID_NETWORK == ANTW_INVALID_NETWORK_NUMBER,
	       "wire code drift: INVALID_NETWORK_NUMBER");
_Static_assert(RADIANT_CH_ERR_INVALID_PARAM == ANTW_INVALID_PARAMETER_PROVIDED,
	       "wire code drift: INVALID_PARAMETER_PROVIDED");
_Static_assert(RADIANT_CH_EVENT_RX_SEARCH_TIMEOUT == ANTW_EVENT_RX_SEARCH_TIMEOUT,
	       "wire code drift: EVENT_RX_SEARCH_TIMEOUT");
_Static_assert(RADIANT_CH_EVENT_CHANNEL_CLOSED == ANTW_EVENT_CHANNEL_CLOSED,
	       "wire code drift: EVENT_CHANNEL_CLOSED");
_Static_assert(RADIANT_CH_EVENT_RX_FAIL_GO_TO_SEARCH ==
		       ANTW_EVENT_RX_FAIL_GO_TO_SEARCH,
	       "wire code drift: EVENT_RX_FAIL_GO_TO_SEARCH");
_Static_assert(RADIANT_CH_STATUS_UNASSIGNED == ANTW_STATUS_UNASSIGNED_CHANNEL,
	       "wire code drift: STATUS_UNASSIGNED_CHANNEL");
_Static_assert(RADIANT_CH_STATUS_ASSIGNED == ANTW_STATUS_ASSIGNED_CHANNEL,
	       "wire code drift: STATUS_ASSIGNED_CHANNEL");
_Static_assert(RADIANT_CH_STATUS_SEARCHING == ANTW_STATUS_SEARCHING_CHANNEL,
	       "wire code drift: STATUS_SEARCHING_CHANNEL");
_Static_assert(RADIANT_CH_STATUS_TRACKING == ANTW_STATUS_TRACKING_CHANNEL,
	       "wire code drift: STATUS_TRACKING_CHANNEL");
_Static_assert(RADIANT_CH_STATUS_STATE_MASK == ANTW_STATUS_CHANNEL_STATE_MASK,
	       "wire code drift: STATUS_CHANNEL_STATE_MASK");
_Static_assert(RADIANT_CH_TYPE_MASTER_BIT == ANTW_CHANNEL_TYPE_MASTER,
	       "wire code drift: CHANNEL_TYPE_MASTER");
#endif /* ANTW_RESPONSE_NO_ERROR */

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_CHANNEL_H_ */
