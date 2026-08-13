/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Radio contract between the ANT serial bridge and whatever implements the
 * ANT link layer underneath it. Three backends, selected by
 * CONFIG_ANT_DONGLE_RADIO_*:
 *
 *   ant_radio_sdk_ant.c   thin forwarders onto Nordic's prebuilt libant.a,
 *                         plus a BUILD_ASSERT block checking every constant
 *                         and signature here against sdk-ant. Proven backend
 *                         (Zwift-verified) and the only one where the
 *                         asserts can run.
 *   ant_radio_stub.c      accepts config and discards it; getters return
 *                         synthetic data. Nothing transmits. Builds with no
 *                         sdk-ant present, proving this seam holds standalone.
 *   radiant_core/         the clean-room rebuild; this file is its spec.
 *
 * Everything here is prefixed antr_ because sdk-ant's error macros are
 * computed expressions, not literals - an unprefixed macro of the same name
 * would be a hard redefinition error, so ant_wire.h and ant_parameters.h
 * could never share a translation unit and ant_radio_sdk_ant.c could not
 * assert one against the other.
 *
 * Two deliberate departures from sdk-ant's model, documented in place below:
 *   - The event path is inverted: the backend calls antr_on_message() (a flat
 *     struct we define) instead of sdk-ant's callback over a nested union.
 *   - antr_channel_open() is a convenience over
 *     antr_channel_open_with_offset(), the real entry point, because sdk-ant
 *     makes the plain form a macro and a backend needs exactly one function.
 *
 * 50 backend functions below; 49 are called directly by ant_serial_bridge.c
 * or main(), the 50th (antr_channel_open_with_offset()) via its wrapper.
 * Call sites for all of them are in docs/sdk-ant-contract.md.
 */

#ifndef ANT_RADIO_H_
#define ANT_RADIO_H_

#include <stddef.h>
#include <stdint.h>

/*
 * ANTW_* names below are the ANT serial protocol's own response and event
 * codes, generated into src/ant_wire.h from protocol/ant_wire.yaml. Pulled in
 * here so a caller acting on a return value has the codes in scope.
 */
#include "ant_wire.h"

/* ── Error convention ──────────────────────────────────────────────────────── */

/*
 * Every function returns a response code that *is* the byte the serial
 * protocol puts on the wire: 0 is ANTW_RESPONSE_NO_ERROR, else an ANTW_*
 * channel response code, forwarded to the host unchanged.
 *
 * sdk-ant's ant_err_t instead carries a wide NRF error offset; narrowing that
 * to a wire byte happens once in ant_radio_sdk_ant.c rather than at three
 * bridge call sites. A clean-room backend must return wire codes directly and
 * invent no error space of its own - an unrecognised code makes a host
 * library hang waiting for a reply it can parse.
 *
 * Per-function return lists are the permitted set, not a promise of which
 * case produces which code - that is pinned against sdk-ant byte-for-byte by
 * tools/ant_conformance.py.
 */
typedef uint8_t antr_err_t;

/* ── Inbound events: the message the backend hands up ──────────────────────── */

/*
 * One ANT message travelling from the radio to the host. Deliberately
 * wire-shaped rather than semantically decomposed - the bridge's whole job is
 * wrapping it in [0xA4][len][id][data...][xor], and an extra field would just
 * be one more thing that could disagree with the bytes. No unions: `id`
 * alone distinguishes broadcast/acknowledged/burst/response/startup.
 */
struct antr_msg {
	/* ANT message ID - ANTW_MESG_BROADCAST_DATA_ID, _RESPONSE_EVENT_ID,
	 * _STARTUP_MESG_ID etc. Goes on the wire unchanged.
	 */
	uint8_t id;

	/*
	 * Valid bytes at `data`, becoming the frame's LEN byte - so it counts
	 * the channel byte plus payload. Must be <= ANTW_MAX_SIZE_VALUE (38,
	 * not the 20 derived elsewhere from inbound-only messages: an
	 * advanced-burst message reaches 25). Exceeding it drops the message
	 * with a warning rather than overrunning the frame buffer.
	 */
	uint8_t len;

	/*
	 * The message body, exactly as it goes on the wire. data[0] is the
	 * channel number for channel-scoped messages, else the first payload
	 * byte.
	 *
	 * LIFETIME: valid only for the duration of the antr_on_message() call;
	 * the bridge copies what it needs before returning, so a backend must
	 * not hand up a DMA pointer the radio could rewrite mid-call.
	 *
	 * Never NULL - a bodyless message has len == 0 and data still valid.
	 */
	const uint8_t *data;
};

/*
 * Called by the backend for every message the host should see.
 *
 * IMPLEMENTED BY THE BRIDGE, NOT BY THE BACKEND - no registration call, the
 * symbol is resolved at link time, exactly one translation unit defines it
 * (src/ant_serial_bridge.c, or a test double in radiant_core/tests/).
 *
 * Contract on the caller:
 *   1. Not before antr_init() has returned 0.
 *   2. `msg` and `msg->data` non-NULL and readable until the call returns.
 *   3. Never called re-entrantly - the backend serialises its own event
 *      delivery; the bridge relies on this and does not lock around the
 *      burst semaphore.
 *   4. May run on the backend's own thread/work queue/callback context, but
 *      must not block.
 *   5. Three ANTW_EVENT_* codes carry buffer-ownership meaning as well as
 *      status - see the burst contract below.
 *   6. An event caused by an antr_* call must not reach the host before that
 *      call's own response - see ANTR_HOST_THREAD_PRIORITY.
 */
void antr_on_message(const struct antr_msg *msg);

/*
 * Zephyr thread priority the bridge makes antr_* calls on; a backend's own
 * event-delivery thread must sit below it (numerically higher).
 *
 * Why: the bridge sends a command's RESPONSE_EVENT reply after the antr_*
 * call returns. A backend thread able to preempt this one would put an event
 * the command caused onto the wire before the reply to the command itself -
 * and a host library, which reads until it finds its own response and
 * discards what it passes, never sees that event at all. Measured on the
 * bench: with the event thread one priority above this one, closing a
 * channel delivered EVENT_CHANNEL_CLOSED before the close response, the host
 * missed it, and the following unassign was refused CHANNEL_IN_WRONG_STATE.
 *
 * A backend delivering events from an ISR or work queue instead of a thread
 * has the same obligation, met its own way.
 */
#define ANTR_HOST_THREAD_PRIORITY 5

/* ── Burst buffer ownership: the contract that costs a second per packet ───── */

/*
 * antr_burst_tx() is the only function here that takes ownership of a
 * caller's buffer. Getting the handback wrong does not fail loudly - it just
 * makes the dongle slow.
 *
 * The bridge holds one static 24-byte block behind a binary semaphore: each
 * incoming burst packet takes the semaphore, is copied into the block, and
 * the block is handed to antr_burst_tx(). The semaphore is given back only
 * on ANTW_EVENT_TRANSFER_NEXT_DATA_BLOCK, _TX_COMPLETED, _TX_FAILED, or
 * synchronously if antr_burst_tx() itself returns an error.
 *
 * Contract a backend must satisfy:
 *   B1. Return 0 -> backend owns `data` until it raises one of the three
 *       events above; not valid to assume afterwards.
 *   B2. Return non-zero -> backend owns nothing, must not touch `data` or
 *       raise any of the three events for it.
 *   B3. NEXT_DATA_BLOCK exactly once per accepted block that is not the
 *       transfer's last.
 *   B4. The last block (ANTR_BURST_SEGMENT_END) is released by
 *       TX_COMPLETED/TX_FAILED instead, exactly once - never also
 *       NEXT_DATA_BLOCK.
 *   B5. Never raise NEXT_DATA_BLOCK for a block that was never accepted -
 *       it frees a semaphore the bridge still holds for a different block,
 *       letting the next host packet overwrite an in-flight buffer.
 *
 * Missing B3: the bridge's k_sem_take() waits its full 1000 ms then answers
 * ANTW_TRANSFER_IN_PROGRESS - a burst stalls a second per packet with a
 * plausible-looking error instead of failing outright, which is why this is
 * unit-tested (release-event count == accepted-block count) rather than
 * caught by inspection; radiant_core/tests owns it, on Linux CI since
 * native_sim does not build on Windows.
 *
 * NEXT_DATA_BLOCK is internal flow control and never reaches the host - a
 * real ANT stick frames bursts itself and never puts it on the wire.
 */

/*
 * Segment flags for antr_burst_tx(). A block first and last of its transfer
 * carries both bits.
 *
 * API values, not wire values - the bridge derives them from the burst
 * sequence number, so these are ours to choose. Chosen to match sdk-ant's
 * BURST_SEGMENT_CONTINUE/_START/_END (BUILD_ASSERTed in
 * ant_radio_sdk_ant.c), which is what lets that shim forward the byte
 * untouched. END was wrongly 0x04 before that assert existed - it would have
 * marked every burst's last block as a middle block, so TX_COMPLETED never
 * arrives and the burst semaphore times out at 1000 ms per transfer.
 */
#define ANTR_BURST_SEGMENT_CONTINUE  0x00  /* a middle block */
#define ANTR_BURST_SEGMENT_START     0x01  /* first block of the transfer */
#define ANTR_BURST_SEGMENT_END       0x02  /* last block of the transfer */

/* The largest block antr_burst_tx() will ever be handed is
 * ANTW_ADV_BURST_BLOCK_MAX, from src/ant_wire.h. Advanced burst carries 8, 16
 * or 24 bytes per packet; plain burst carries 8. A backend must accept those
 * three and reject everything else with ANTW_INVALID_MESSAGE.
 *
 * Deliberately not redefined here under an ANTR_ name. Two constants for one
 * buffer length is how a reply gets framed a byte short, and this one is
 * already generated from protocol/ant_wire.yaml.
 */

/* ── Initialisation and stack lifecycle ────────────────────────────────────── */

/**
 * Bring the radio stack up.
 *
 * Called once from main(), before the transport is enabled, because ANT (and
 * MPSL under it) must own high-frequency clock startup before USB asks for the
 * HFXO. Getting that order wrong does not fail cleanly - USB either hangs or
 * fails to enumerate.
 *
 * A backend allocates its channels, arms its clocks and does nothing on air.
 * No channel exists yet, so nothing can be transmitted or received until the
 * host has assigned and opened one.
 *
 * antr_on_message() must not be called before this returns 0.
 *
 * Returns 0, or ANTW_INVALID_PARAMETER_PROVIDED if the build's channel or
 * network sizing cannot be satisfied. A backend that cannot start has no
 * useful wire code to report - main() logs the value and refuses to continue.
 */
antr_err_t antr_init(void);

/**
 * Reset the protocol stack to its power-on defaults.
 *
 * This resets the *stack*, not the MCU. Every host library (openant, Zwift,
 * Golden Cheetah) opens the device, sends a reset, and then keeps using the
 * same USB handle - so rebooting here drops the device off the bus and the
 * host's next transfer fails on a stale pipe, which is exactly what a session
 * start looks like.
 *
 * Every channel is closed and unassigned, every network key is forgotten, and
 * library configuration returns to zero. Channel-closed events raised as a
 * side effect are still delivered through antr_on_message(); the bridge
 * discards them for a short window afterwards, because a real stick emits
 * nothing between the reset command and the startup message.
 *
 * Synchronous: when it returns, the stack is usable again.
 *
 * Returns 0. There is no failure case a host could act on.
 */
antr_err_t antr_stack_reset(void);

/* ── Channel lifecycle ─────────────────────────────────────────────────────── */

/**
 * Create a channel: bind a channel number to a type and a network.
 *
 * @param channel     0 .. (channel count - 1).
 * @param type        ANTW_CHANNEL_TYPE_* - slave/master, bidirectional,
 *                    shared, receive-only, transmit-only.
 * @param network     Network number, indexing the keys set by
 *                    antr_network_address_set().
 * @param ext_assign  Extended assignment flags (background scanning,
 *                    frequency agility, fast-start search, asynchronous
 *                    transmission). Zero when the host sent the three-byte
 *                    form of the message, which most do.
 *
 * The channel must be unassigned. Assignment does not put anything on air -
 * antr_channel_open_with_offset() does that.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel number out of range),
 * ANTW_CHANNEL_IN_WRONG_STATE (already assigned) or
 * ANTW_INVALID_NETWORK_NUMBER.
 */
antr_err_t antr_channel_assign(uint8_t channel, uint8_t type, uint8_t network,
			       uint8_t ext_assign);

/**
 * Destroy a channel and release everything it held.
 *
 * The channel must be closed first. Unassigning an open channel is a host bug
 * and is reported rather than silently closing it, because the host would
 * otherwise miss the channel-closed event it is waiting for.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range) or
 * ANTW_CHANNEL_IN_WRONG_STATE (still open, or never assigned).
 */
antr_err_t antr_channel_unassign(uint8_t channel);

/**
 * Put an assigned channel on air, optionally delaying the first slot.
 *
 * This is the real entry point; antr_channel_open() below forwards to it with
 * an offset of zero. The split exists because sdk-ant exposes the plain form
 * as a macro rather than a function, so a backend that implements only
 * antr_channel_open() would leave the shim with nothing to forward to.
 *
 * @param channel  An assigned channel.
 * @param offset   Delay before the channel's first slot, in the same 32768 Hz
 *                 counts antr_channel_period_set() uses. Zero means "as soon
 *                 as possible". A master uses this to phase its slot away from
 *                 another channel's; it is a one-shot offset applied to the
 *                 first slot only, after which the channel period governs.
 *
 *                 The unit is worth stating because a uint16_t of microseconds
 *                 would top out at 65 ms - less than a single 4 Hz period - so
 *                 it could not phase a channel across its own period and
 *                 therefore cannot be what this field is. In 32768 Hz counts
 *                 the range is 2 s, which covers every period ANT allows.
 *
 * A slave enters search; a master starts transmitting. Either way the outcome
 * arrives later as an event, not as a return value: the search result is an
 * ANTW_EVENT_RX_SEARCH_TIMEOUT or the first data message.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range),
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned, or already open) or
 * ANTW_CHANNEL_ID_NOT_SET (assigned but antr_channel_id_set() was never
 * called).
 */
antr_err_t antr_channel_open_with_offset(uint8_t channel, uint16_t offset);

/**
 * Open a channel with no start offset.
 *
 * A convenience for the overwhelmingly common case, and the only form the
 * serial protocol's MESG_OPEN_CHANNEL carries. Deliberately a static inline
 * rather than a macro: it type-checks its argument and cannot double-evaluate
 * it. A backend implements antr_channel_open_with_offset() and gets this for
 * free.
 */
static inline antr_err_t antr_channel_open(uint8_t channel)
{
	return antr_channel_open_with_offset(channel, 0);
}

/**
 * Take a channel off air, keeping its assignment and configuration.
 *
 * Asynchronous in the sense that matters: the channel is not closed when this
 * returns, it is closing. The host learns it is done from
 * ANTW_EVENT_CHANNEL_CLOSED, and a backend must raise that event exactly once
 * per successful close - a host library that reopens on the event will hang
 * without it.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range) or
 * ANTW_CHANNEL_IN_WRONG_STATE (not open).
 */
antr_err_t antr_channel_close(uint8_t channel);

/* ── Channel configuration ─────────────────────────────────────────────────── */

/**
 * Set the channel ID - what a master transmits as, or what a slave searches for.
 *
 * @param channel       An assigned channel.
 * @param device_number 0 is the wildcard on a slave: match any device number.
 * @param device_type   Low 7 bits are the device type (0x78 heart rate, 0x0B
 *                      power, 0x11 fitness equipment); the MSB is the pairing
 *                      bit. 0 is the wildcard on a slave.
 * @param trans_type    Transmission type. 0 is the wildcard on a slave.
 *
 * Must be called before the channel is opened; sdk-ant and every retail stick
 * reject it on an open channel rather than retuning underneath the host.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range) or
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned or open).
 */
antr_err_t antr_channel_id_set(uint8_t channel, uint16_t device_number,
			       uint8_t device_type, uint8_t trans_type);

/**
 * Set the channel period in 32768 Hz counts.
 *
 * 8192 is the 4 Hz that most ANT+ profiles use. The units are the radio's own
 * timebase, not milliseconds, and the value is what makes a slave's receive
 * window land where the master's slot is - a backend that rounds it to a
 * millisecond tick will drift out of the window within a minute.
 *
 * Must be called before the channel is opened.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range),
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned or open) or
 * ANTW_INVALID_PARAMETER_PROVIDED (period outside what the backend can
 * schedule).
 */
antr_err_t antr_channel_period_set(uint8_t channel, uint16_t period);

/**
 * Set the channel's RF frequency as an offset from 2400 MHz.
 *
 * 0 .. 124, so 2400 .. 2524 MHz. 57 is the ANT+ frequency and is what every
 * profile uses; 66 is the ANT+ pairing/ANT-FS frequency. The value is an index,
 * never a frequency in Hz - that is a deliberate portability choice, because a
 * backend on another part translates an index far more cheaply than it parses
 * a frequency.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range),
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned or open) or
 * ANTW_INVALID_PARAMETER_PROVIDED (offset > 124).
 */
antr_err_t antr_channel_radio_freq_set(uint8_t channel, uint8_t freq);

/**
 * Set transmit power for one channel.
 *
 * @param channel    An assigned channel. This is the constraint that shapes
 *                   the bridge: the host sets power before assigning anything,
 *                   so the bridge holds the requested level and reapplies it
 *                   after each successful assign. A backend does not need to
 *                   replicate that - it may reject an unassigned channel.
 * @param level      ANTW_RADIO_TX_POWER_LVL_0 .. _4 (-20, -12, -4, 0, +4 dBm),
 *                   or ANTW_RADIO_TX_POWER_LVL_5 (+8 dBm) where the part has
 *                   it - nRF52820, nRF52833 and nRF52840 only. Bit 7
 *                   (ANTW_RADIO_TX_POWER_LVL_CUSTOM) means `custom` names a
 *                   raw register value instead.
 * @param custom     Raw part-specific power value; ignored unless bit 7 of
 *                   `level` is set.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range),
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned) or ANTW_INVALID_PARAMETER_PROVIDED
 * (level the part cannot produce).
 */
antr_err_t antr_channel_radio_tx_power_set(uint8_t channel, uint8_t level,
					   uint8_t custom);

/**
 * Set the high-priority search timeout, in 2.5 second increments.
 *
 * 0 disables high-priority search entirely; 255 means "search forever". The
 * ANT default is 10 (25 s). High-priority search blocks other channels'
 * traffic while it runs, which is what the low-priority variant below exists
 * to avoid.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range) or
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned).
 */
antr_err_t antr_channel_search_timeout_set(uint8_t channel, uint8_t timeout);

/**
 * Set the low-priority search timeout, in 2.5 second increments.
 *
 * Low-priority search yields to any other channel that wants the radio, so it
 * can run indefinitely without starving a live session. 0 disables it; 255 is
 * infinite. Runs after the high-priority timeout expires.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range) or
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned).
 */
antr_err_t antr_channel_low_priority_rx_search_timeout_set(uint8_t channel,
							   uint8_t timeout);

/**
 * Select the CRC mode for a channel.
 *
 * @param channel  An assigned channel.
 * @param mode     The on-air CRC width. src/ant_wire.h names no constants for
 *                 the values - only ANTW_MESG_CHANNEL_CRC_MODE_SIZE for the
 *                 message - because nothing in this project sends anything but
 *                 the default. A backend must therefore treat the byte as
 *                 opaque and validate it against its own supported set.
 *
 * Changing this breaks interoperability with anything using the default, so it
 * exists for proprietary links rather than for ANT+ traffic. A backend that
 * supports only the default CRC must reject every other value rather than
 * accepting and ignoring it: a host that believes it configured a shorter CRC
 * and did not will see the link work and its own framing fail.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range),
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned or open) or
 * ANTW_INVALID_PARAMETER_PROVIDED (mode not supported).
 */
antr_err_t antr_channel_radio_crc_mode_set(uint8_t channel, uint8_t mode);

/**
 * Program the three-frequency table a channel hops through under frequency
 * agility.
 *
 * Only meaningful on a channel assigned with the frequency-agility extended
 * flag. Each parameter is a 2400 MHz offset, same units as
 * antr_channel_radio_freq_set(). ANT+ profiles forbid frequency agility, so
 * nothing in a standard session reaches this.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range),
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned or open) or
 * ANTW_INVALID_PARAMETER_PROVIDED (any offset > 124).
 */
antr_err_t antr_auto_freq_hop_table_set(uint8_t channel, uint8_t freq0,
					uint8_t freq1, uint8_t freq2);

/**
 * Enable or disable enhanced channel spacing, device-wide.
 *
 * @param enable  Non-zero to enable.
 *
 * A device-wide radio property, not a per-channel one, which is why it takes
 * no channel number. A backend that does not implement it should still accept
 * enable == 0 (the default state) and may reject a non-zero value with
 * ANTW_INVALID_PARAMETER_PROVIDED.
 *
 * Returns 0 or ANTW_INVALID_PARAMETER_PROVIDED.
 */
antr_err_t antr_enhanced_channel_spacing_enable(uint8_t enable);

/* ── Data transfer ─────────────────────────────────────────────────────────── */

/**
 * Queue an 8-byte broadcast payload for the channel's next slot.
 *
 * @param channel  An open channel.
 * @param size     Payload length. 8 for standard broadcast.
 * @param data     Payload. Copied before this returns - unlike antr_burst_tx(),
 *                 there is no ownership transfer and the caller may reuse the
 *                 buffer immediately.
 *
 * Broadcast is unacknowledged by design: the outcome is ANTW_EVENT_TX when the
 * slot fires, and there is no failure event because there is nothing to fail.
 * One payload per slot - queueing a second before the slot fires replaces the
 * first rather than sending both.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range, or bad size),
 * ANTW_CHANNEL_NOT_OPENED, ANTW_CHANNEL_IN_WRONG_STATE (a receive-only
 * channel) or ANTW_TRANSFER_IN_PROGRESS (a burst owns the channel).
 */
antr_err_t antr_broadcast_message_tx(uint8_t channel, uint8_t size,
				     const uint8_t *data);

/**
 * Queue an 8-byte acknowledged payload.
 *
 * Same shape as broadcast, different guarantee: the receiver replies and the
 * outcome arrives as ANTW_EVENT_TRANSFER_TX_COMPLETED or
 * ANTW_EVENT_TRANSFER_TX_FAILED. A backend must raise exactly one of those two
 * per accepted message - this is how Zwift learns a trainer took a resistance
 * change, so a missing event is a stuck ERG session, not a cosmetic gap.
 *
 * @param data  Copied before this returns.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range, or bad size),
 * ANTW_CHANNEL_NOT_OPENED, ANTW_CHANNEL_IN_WRONG_STATE or
 * ANTW_TRANSFER_IN_PROGRESS (a previous acknowledged message or burst has not
 * finished).
 */
antr_err_t antr_acknowledge_message_tx(uint8_t channel, uint8_t size,
				       const uint8_t *data);

/**
 * Hand one block of a burst transfer to the radio.
 *
 * @param channel  An open channel.
 * @param size     8, 16 or 24. Advanced burst is what makes 16 and 24 legal,
 *                 and it must have been enabled by antr_adv_burst_config_set()
 *                 first.
 * @param data     The block. OWNERSHIP TRANSFERS on a zero return - read the
 *                 buffer-ownership contract above in full before implementing
 *                 this. The buffer is mutable because a backend may need to
 *                 transform it in place; it must not be read after the
 *                 releasing event.
 * @param segment  ANTR_BURST_SEGMENT_* flags marking the first and last block.
 *
 * Deliberately named for what it does rather than mirroring sdk-ant's
 * ant_burst_handler_request(); the mapping is recorded in
 * docs/sdk-ant-contract.md.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range, or a size that is not
 * 8/16/24), ANTW_CHANNEL_NOT_OPENED, ANTW_TRANSFER_IN_PROGRESS (a different
 * transfer owns the channel, or this transfer has not consumed the previous
 * block), ANTW_TRANSFER_SEQUENCE_NUMBER_ERROR (a segment flag that does not
 * follow from the previous block) or ANTW_TRANSFER_IN_ERROR.
 *
 * On any non-zero return the backend owns nothing and must raise no event for
 * this block.
 */
antr_err_t antr_burst_tx(uint8_t channel, uint16_t size, uint8_t *data,
			 uint8_t segment);

/**
 * Read whether a channel has a message queued but not yet transmitted.
 *
 * @param channel  An assigned channel.
 * @param pending  Out: non-zero if something is queued.
 *
 * Named _get, unlike sdk-ant's bare ant_pending_transmit(), because it sits
 * next to antr_pending_transmit_clear() and a verb-less name next to a verb is
 * how a getter gets called for its side effects. Along with antr_burst_tx()
 * this is one of only two names here that is not a straight ant_ -> antr_
 * prefix swap; the mapping table in docs/sdk-ant-contract.md records both, and
 * every other name is mechanical precisely so that an omission in Wave 2 shows
 * up as a link error rather than as a silent behaviour change.
 *
 * Returns 0 or ANTW_INVALID_MESSAGE (channel out of range).
 */
antr_err_t antr_pending_transmit_get(uint8_t channel, uint8_t *pending);

/**
 * Discard a channel's queued-but-unsent message.
 *
 * @param channel  An assigned channel.
 * @param success  Out: non-zero if something was actually discarded.
 *
 * Note that the outcome is reported through `success`, not through the return
 * value: "there was nothing to clear" is not an error, and a host that asked
 * still gets a RESPONSE_NO_ERROR with a zero in the payload.
 *
 * Returns 0 or ANTW_INVALID_MESSAGE (channel out of range).
 */
antr_err_t antr_pending_transmit_clear(uint8_t channel, uint8_t *success);

/* ── Search behaviour ──────────────────────────────────────────────────────── */

/**
 * Set the search waveform - how wide a slave's receive window opens.
 *
 * @param channel   An assigned channel.
 * @param waveform  Window width in the radio's own units. 316 is the ANT
 *                  default; a smaller value narrows the window, finds sensors
 *                  more slowly and costs less power.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range) or
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned).
 */
antr_err_t antr_search_waveform_set(uint8_t channel, uint16_t waveform);

/**
 * Set the proximity search threshold - refuse to acquire a sensor that is too
 * far away.
 *
 * @param channel    An assigned channel.
 * @param threshold  0 disables proximity search; 1..10 are increasing RSSI
 *                   thresholds. This is what a head unit uses to pair with the
 *                   strap on your chest rather than the one on the next bike.
 * @param custom     A raw RSSI threshold in dBm, used when the backend
 *                   supports it; 0 otherwise. Optional in the serial message,
 *                   so the bridge passes 0 when the host sent the short form.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range),
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned) or ANTW_INVALID_PARAMETER_PROVIDED
 * (threshold > 10).
 */
antr_err_t antr_prox_search_set(uint8_t channel, uint8_t threshold,
				uint8_t custom);

/**
 * Set whether a channel searches at high or low priority.
 *
 * @param channel   An assigned channel.
 * @param priority  0 low, 1 high.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range) or
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned).
 */
antr_err_t antr_search_channel_priority_set(uint8_t channel, uint8_t priority);

/**
 * Read back the search priority set above.
 *
 * @param priority  Out: 0 or 1.
 *
 * Returns 0 or ANTW_INVALID_MESSAGE (channel out of range).
 */
antr_err_t antr_search_channel_priority_get(uint8_t channel, uint8_t *priority);

/**
 * Set how many channel periods a shared search cycle spends on this channel.
 *
 * @param channel  An assigned channel.
 * @param cycles   Number of periods, 0 for the backend default.
 *
 * This is the knob that makes several channels searching at once tolerable:
 * without it, eight simultaneous searches serialise. A backend free to merge
 * receive windows (all ANT+ traffic sits on one frequency, so one window with
 * several filters catches any of them) may treat this as advisory - but it
 * must still accept and read back the value, because tools/ant_features.py
 * round-trips it.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range) or
 * ANTW_CHANNEL_IN_WRONG_STATE (unassigned).
 */
antr_err_t antr_active_search_sharing_cycles_set(uint8_t channel,
						 uint8_t cycles);

/**
 * Read back the search-sharing cycle count.
 *
 * @param cycles  Out.
 *
 * Returns 0 or ANTW_INVALID_MESSAGE (channel out of range).
 */
antr_err_t antr_active_search_sharing_cycles_get(uint8_t channel,
						 uint8_t *cycles);

/**
 * Add one device ID to a channel's inclusion/exclusion list.
 *
 * @param channel    An assigned, closed channel.
 * @param device_id  4 bytes, in wire order:
 *                   [devnum_lsb, devnum_msb, device_type, trans_type].
 *                   Copied before this returns.
 * @param index      Slot in the list.
 *
 * The list is only consulted once antr_id_list_config() has given it a size.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range),
 * ANTW_CHANNEL_IN_WRONG_STATE (open) or ANTW_INVALID_LIST_ID (index beyond
 * what the backend supports).
 */
antr_err_t antr_id_list_add(uint8_t channel, const uint8_t *device_id,
			    uint8_t index);

/**
 * Activate a channel's device ID list.
 *
 * @param channel   An assigned, closed channel.
 * @param size      Number of entries to honour; 0 turns the list off.
 * @param inc_exc   0 for an inclusion list (acquire only these), 1 for an
 *                  exclusion list (acquire anything but these).
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range),
 * ANTW_CHANNEL_IN_WRONG_STATE (open) or ANTW_INVALID_LIST_ID (size beyond what
 * the backend supports).
 */
antr_err_t antr_id_list_config(uint8_t channel, uint8_t size,
			       uint8_t inc_exc);

/* ── Networks and keys ─────────────────────────────────────────────────────── */

/**
 * Install a network key.
 *
 * @param network  Network number, 0 .. (network count - 1). 0 is where every
 *                 host puts the ANT+ key.
 * @param key      8 bytes. Copied before this returns.
 *
 * PERMANENT LIMITATION, not a to-do. The on-air network address is derived
 * from the key by a non-public function this project will not attempt to
 * recover (fitting it from samples would be reverse-engineering). A
 * clean-room backend holds a small key -> address table, seeded with the
 * published ANT+ pair (B9A521FBBD72C345 -> A6 C5), and returns
 * ANTW_INVALID_PARAMETER_PROVIDED for any other key. This costs nothing on
 * the path anyone actually runs, since every ANT+ profile uses that one key.
 *
 * sdk-ant has no such limitation - libant.a computes the address - so A/B
 * comparisons between backends must use the ANT+ key, not an arbitrary one.
 *
 * Returns 0, ANTW_INVALID_NETWORK_NUMBER, or ANTW_INVALID_PARAMETER_PROVIDED
 * for a key whose network address the backend cannot derive.
 */
antr_err_t antr_network_address_set(uint8_t network, const uint8_t *key);

/* ── Library configuration ─────────────────────────────────────────────────── */

/**
 * Turn on library configuration bits.
 *
 * @param config  ANTW_LIB_CONFIG_* bits, OR'd. The three that matter are
 *                ANTW_LIB_CONFIG_MESG_OUT_INC_DEVICE_ID,
 *                ANTW_LIB_CONFIG_MESG_OUT_INC_RSSI and
 *                ANTW_LIB_CONFIG_MESG_OUT_INC_TIME_STAMP on every received
 *                message; the tools request all three at once
 *                (ANTW_LIB_CONFIG_ALL_EXT_FIELDS, 0xE0) rather than the
 *                device-ID-only 0x80.
 *
 * The RX timestamp must come from the backend's own capture of packet
 * arrival, not a host-side clock reading (0.009 ms vs ~2.6 ms of USB jitter).
 * A backend filling it from a software timer passes every functional test
 * while quietly destroying the one measurement this field exists for.
 *
 * Bits are additive: setting one leaves the others alone.
 *
 * Returns 0 or ANTW_INVALID_PARAMETER_PROVIDED (a bit the backend does not
 * implement).
 */
antr_err_t antr_lib_config_set(uint8_t config);

/**
 * Turn off library configuration bits.
 *
 * @param config  Bits to clear. ANTW_LIB_CONFIG_MASK_ALL clears everything,
 *                which is how the bridge implements the host's "set config to
 *                zero" - there is no separate clear message on the wire.
 *
 * Returns 0 or ANTW_INVALID_PARAMETER_PROVIDED.
 */
antr_err_t antr_lib_config_clear(uint8_t config);

/**
 * Read the current library configuration bits.
 *
 * @param config  Out: one byte of ANTW_LIB_CONFIG_* flags.
 *
 * Returns 0.
 */
antr_err_t antr_lib_config_get(uint8_t *config);

/**
 * Set the event filter - which channel events are suppressed before they reach
 * antr_on_message().
 *
 * @param filter  16-bit mask, one bit per channel event code. A set bit
 *                suppresses that event. src/ant_wire.h names no constants for
 *                the individual bits - only ANTW_MESG_EVENT_FILTER_CONFIG_ID
 *                and its reply size - because the bridge passes the mask
 *                through whole.
 *
 * Exists to keep a busy multi-channel session from drowning the host in
 * ANTW_EVENT_RX_FAIL. A backend that does not filter must still store and
 * return the value, because tools/ant_features.py round-trips it - but it must
 * not pretend to filter, because loss accounting depends on the RX_FAIL events
 * actually arriving.
 *
 * Returns 0 or ANTW_INVALID_PARAMETER_PROVIDED.
 */
antr_err_t antr_event_filtering_set(uint16_t filter);

/**
 * Read the event filter mask.
 *
 * @param filter  Out.
 *
 * Returns 0.
 */
antr_err_t antr_event_filtering_get(uint16_t *filter);

/**
 * Configure advanced burst.
 *
 * @param config  The configuration block, without the leading filler byte the
 *                serial message carries:
 *                [enable, rf payload size, required modes, 0, 0,
 *                 optional modes, 0, 0] and optionally
 *                [stall count lsb, stall count msb, retry extension].
 *                Copied before this returns.
 * @param size    8 for the required part, up to 11 with the optional tail.
 *                Anything outside 8..11 is rejected - accepting a short block
 *                would let a bridge bug that passes the wrong slice of the
 *                message look correct here.
 *
 * Until this enables advanced burst, antr_burst_tx() must reject sizes above 8:
 * a dongle that takes 24-byte blocks and can never send one is worse than one
 * that refuses them.
 *
 * Returns 0 or ANTW_INVALID_PARAMETER_PROVIDED (size out of range, or a mode
 * the backend does not implement).
 */
antr_err_t antr_adv_burst_config_set(const uint8_t *config, uint8_t size);

/**
 * Read advanced burst capabilities or current configuration.
 *
 * @param request_type  0 asks what the backend can do; non-zero asks what is
 *                      currently configured.
 * @param config        Out. For request_type 0 the backend writes the
 *                      capabilities block - supported RF payload sizes
 *                      (ANTW_ADV_BURST_MODES_SIZE_*), then supported optional
 *                      modes (ANTW_ADV_BURST_MODES_FREQ_HOP). For non-zero it
 *                      writes ANTW_MESG_CONFIG_ADV_BURST_REQ_CONFIG_SIZE (10)
 *                      bytes: the block passed to the setter, minus its
 *                      leading enable byte.
 *
 * Neither form includes the leading byte the serial reply carries - the bridge
 * supplies that. Writing the wrong one of the two lengths here is invisible
 * until a host reads a field shifted by one, so a backend must size its write
 * off `request_type` and nothing else.
 *
 * Returns 0.
 */
antr_err_t antr_adv_burst_config_get(uint8_t request_type, uint8_t *config);

/**
 * Define a selective-data-update mask.
 *
 * @param mask_index  Which mask, 0 .. (mask count - 1). A mask number, not a
 *                    channel - masks are defined here and attached to channels
 *                    by antr_sdu_mask_config().
 * @param mask        8 bytes, one bit per payload bit. Copied before this
 *                    returns.
 *
 * Returns 0 or ANTW_INVALID_PARAMETER_PROVIDED (mask_index out of range).
 */
antr_err_t antr_sdu_mask_set(uint8_t mask_index, const uint8_t *mask);

/**
 * Read back a selective-data-update mask.
 *
 * @param mask  Out: 8 bytes.
 *
 * Returns 0 or ANTW_INVALID_PARAMETER_PROVIDED (mask_index out of range).
 */
antr_err_t antr_sdu_mask_get(uint8_t mask_index, uint8_t *mask);

/**
 * Bind a selective-data-update mask to a channel.
 *
 * @param channel      An assigned channel.
 * @param mask_config  A mask index, optionally OR'd with the acknowledged-data
 *                     bit that applies the mask to acknowledged data as well.
 *                     ANTW_INVALID_SDU_MASK (0xFF) turns selective updates
 *                     back off for the channel. A backend must mask that bit
 *                     off before range-checking the index, which is what
 *                     src/ant_radio_stub.c:586-587 does.
 *
 * Note the two different error codes: an out-of-range channel is
 * ANTW_INVALID_MESSAGE and an out-of-range mask is
 * ANTW_INVALID_PARAMETER_PROVIDED. The distinction is observable from the host
 * and tools/ant_conformance.py diffs it.
 *
 * Returns 0, ANTW_INVALID_MESSAGE or ANTW_INVALID_PARAMETER_PROVIDED.
 */
antr_err_t antr_sdu_mask_config(uint8_t channel, uint8_t mask_config);

/* ── Status and queries ────────────────────────────────────────────────────── */

/**
 * Report what the radio can do.
 *
 * @param capabilities  Out: ANTW_MESG_CAPABILITIES_SIZE bytes (9), in wire
 *                      order - max channels, max networks, standard options,
 *                      advanced options, advanced options 2, max SensRcore
 *                      channels, advanced options 3, 4 and 5.
 *
 * The bits describe what the radio layer can do, not what the bridge chose to
 * expose. Reporting an ANTUSB-m's capabilities and then declining a message is
 * closer to the device being impersonated than trimming the bits to match; the
 * reasoning is in README.md and must not be quietly reversed by a rebuild.
 *
 * Returns 0.
 */
antr_err_t antr_capabilities_get(uint8_t *capabilities);

/**
 * Report the stack version string.
 *
 * @param version  Out: ANTW_MESG_VERSION_SIZE bytes, NUL-padded.
 *
 * A backend should make its string identifiable - the stub reports
 * "STUB0.01B00" precisely so a stub image can be recognised from the host side
 * without a debugger. A clean-room backend should do the same rather than
 * claiming to be an AVN build it is not.
 *
 * Returns 0.
 */
antr_err_t antr_version_get(uint8_t *version);

/**
 * Read a channel's state.
 *
 * @param channel  Any channel number.
 * @param status   Out: ANTW_STATUS_UNASSIGNED_CHANNEL, _ASSIGNED_CHANNEL,
 *                 _SEARCHING_CHANNEL or _TRACKING_CHANNEL in the low two bits,
 *                 with the channel type in the upper bits.
 *
 * Legal on an unassigned channel - that is the answer, not an error.
 *
 * Returns 0 or ANTW_INVALID_MESSAGE (channel out of range).
 */
antr_err_t antr_channel_status_get(uint8_t channel, uint8_t *status);

/**
 * Read a channel's ID.
 *
 * On a tracking slave this returns the ID of the device actually acquired,
 * with the wildcards resolved - which is how a host learns which sensor it
 * found. On a master it returns what was configured.
 *
 * @param device_number  Out.
 * @param device_type    Out.
 * @param trans_type     Out.
 *
 * Returns 0 or ANTW_INVALID_MESSAGE (channel out of range).
 */
antr_err_t antr_channel_id_get(uint8_t channel, uint16_t *device_number,
			       uint8_t *device_type, uint8_t *trans_type);

/**
 * Read a channel's period in 32768 Hz counts.
 *
 * @param period  Out.
 *
 * Returns 0 or ANTW_INVALID_MESSAGE (channel out of range).
 */
antr_err_t antr_channel_period_get(uint8_t channel, uint16_t *period);

/**
 * Read a channel's RF frequency offset from 2400 MHz.
 *
 * @param freq  Out: 0 .. 124.
 *
 * Returns 0 or ANTW_INVALID_MESSAGE (channel out of range).
 */
antr_err_t antr_channel_radio_freq_get(uint8_t channel, uint8_t *freq);

/**
 * Read a channel's CRC mode.
 *
 * @param mode  Out: the same opaque width byte antr_channel_radio_crc_mode_set()
 *              takes.
 *
 * Returns 0 or ANTW_INVALID_MESSAGE (channel out of range).
 */
antr_err_t antr_channel_radio_crc_mode_get(uint8_t channel, uint8_t *mode);

/* ── Encryption (ANT+ AES-CTR) ─────────────────────────────────────────────── */

/*
 * Declared unconditionally, whatever CONFIG_ANT_DONGLE_ENCRYPTION says, since
 * a backend stands in for libant.a which exports them regardless. The bridge
 * compiles the three write paths out by default; the read path is always
 * present, and antr_capabilities_get() advertises the encrypted-channel bit
 * either way.
 *
 * A clean-room backend need not implement ANT+'s own AES-CTR - no Windows
 * host can reach it, it is malleable and unauthenticated. The honest
 * implementation accepts the reads and refuses antr_crypto_channel_enable()
 * for any non-zero mode with ANTW_INVALID_PARAMETER_PROVIDED. Its
 * replacement is a separate, optional design (docs/radiant-security.md), not
 * part of this contract.
 */

/**
 * Put a channel into (or out of) encrypted mode.
 *
 * @param channel     An assigned channel.
 * @param enable      0 disables; non-zero selects an encryption mode.
 * @param key_num     Which key installed by antr_crypto_key_set().
 * @param decimation  Rate at which encryption-info pages are interleaved.
 *
 * Returns 0, ANTW_INVALID_PARAMETER_PROVIDED (channel, key number or mode out
 * of range, or encryption not implemented) or ANTW_CHANNEL_IN_WRONG_STATE.
 */
antr_err_t antr_crypto_channel_enable(uint8_t channel, uint8_t enable,
				      uint8_t key_num, uint8_t decimation);

/**
 * Install a 128-bit encryption key.
 *
 * @param key_num  Which key slot.
 * @param key      16 bytes. Copied before this returns. The length is not
 *                 expressed in the signature and is not carried by the serial
 *                 message either - it is fixed at 128 bits by the protocol and
 *                 stated here because there is nowhere else to state it.
 *
 * Returns 0 or ANTW_INVALID_PARAMETER_PROVIDED (key_num out of range).
 */
antr_err_t antr_crypto_key_set(uint8_t key_num, const uint8_t *key);

/**
 * Write one piece of encryption configuration.
 *
 * @param type  ANTW_ENCRYPTION_INFO_SET_CRYPTO_ID (4 bytes of `info`),
 *              ANTW_ENCRYPTION_INFO_SET_CUSTOM_USER_DATA (19 bytes) or
 *              ANTW_ENCRYPTION_INFO_SET_RNG_SEED.
 * @param info  Copied before this returns. There is no length parameter, so
 *              the callee reads a fixed count per type and the *caller* must
 *              bound it. The bridge does exactly that, and refuses the RNG
 *              seed entirely because no size is defined for it anywhere - a
 *              backend must be able to assume the caller checked.
 *
 * Returns 0 or ANTW_INVALID_PARAMETER_PROVIDED (unknown type).
 */
antr_err_t antr_crypto_info_set(uint8_t type, const uint8_t *info);

/**
 * Read one piece of encryption configuration.
 *
 * @param type  ANTW_ENCRYPTION_INFO_GET_SUPPORTED_MODE (writes 1 byte),
 *              ANTW_ENCRYPTION_INFO_GET_CRYPTO_ID (4 bytes) or
 *              ANTW_ENCRYPTION_INFO_GET_CUSTOM_USER_DATA (19 bytes).
 * @param info  Out. Write exactly what the type accounts for and no more:
 *              the caller's buffer is sized for the largest reply *including*
 *              the leading info-type byte that the bridge supplies itself, so
 *              zeroing the whole thing overruns it.
 *
 * Returns 0 or ANTW_INVALID_PARAMETER_PROVIDED (unknown type).
 */
antr_err_t antr_crypto_info_get(uint8_t type, uint8_t *info);

/* ── RadiANT payload security (X_AUTH, X_CONF) ─────────────────────────────── */

#if defined(CONFIG_RADIANT_SEC_HOST_MESSAGES)

/*
 * Messages 0xF1-0xF4. NOT ANT protocol - ours, answered by no ANT device, and
 * described in docs/radiant-security.md.
 *
 * Declared inside the #ifdef (unlike the encryption block above) because
 * these four are implemented by exactly one backend, not every backend
 * standing in for libant.a; the bridge's dispatch is under the same symbol so
 * the two halves appear and disappear together.
 *
 * ORDERING: a channel needs its ID before its key (the device number is
 * bound into the KDF) and its period before its epoch (the period turns a
 * phase into a packet index). Both refused with ANTW_CHANNEL_IN_WRONG_STATE.
 */

/**
 * Configure the transforms on one channel. Message 0xF1.
 *
 * @param switches  Bit 0 X_CONF, bit 1 X_AUTH, bit 2 drop an unverified window
 *                  instead of delivering it, bit 3 descriptor confidentiality
 *                  (refused in v1 - the descriptor has no counter and therefore
 *                  no nonce). Bits 7..4 reserved, must be 0. The two transforms
 *                  are independent, not a ladder.
 * @param w         MAC window, the literal 2, 4 or 8.
 * @param page_lo   Secured page range, both bounded to 0x01..0x1F so the
 * @param page_hi   descriptor and the ANT+ common pages stay in the clear
 *                  mechanically rather than by memory.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel out of range),
 * ANTW_INVALID_PARAMETER_PROVIDED or ANTW_CHANNEL_IN_WRONG_STATE (no key yet).
 */
antr_err_t antr_sec_config(uint8_t channel, uint8_t switches, uint8_t w,
			   uint8_t page_lo, uint8_t page_hi);

/**
 * Install the one 16-byte root key for a channel. Message 0xF2.
 *
 * @param bits  Key length in bits. 128 and nothing else in v1.
 * @param key   Copied before this returns.
 *
 * WRITE ONLY. There is no read arm anywhere and MESG_REQUEST for 0xF2 answers
 * ANTW_INVALID_MESSAGE rather than a key. The caller should wipe its own copy of
 * the message body afterwards; that is worth doing and is not sufficient, since
 * the bytes also passed through the USB ring and the transport buffer beneath
 * it, neither of which is reachable from here.
 *
 * Returns 0, ANTW_INVALID_MESSAGE, ANTW_INVALID_PARAMETER_PROVIDED (unsupported
 * key length) or ANTW_CHANNEL_IN_WRONG_STATE (no channel ID, or a wildcard one).
 */
antr_err_t antr_sec_key_set(uint8_t channel, uint8_t bits, const uint8_t *key);

/**
 * Set the epoch and its time anchor. Message 0xF3.
 *
 * @param flags          Bit 0: the epoch is coarse real time rather than a bare
 *                       ordinal. Other bits reserved, must be 0.
 * @param epoch          Refused if less than or equal to the current one, and
 *                       refused near 0xFFFFFFFF so a counter wrap has headroom.
 * @param us_into_epoch  The phase a receiver derives the packet counter from,
 *                       rather than from arrival history.
 *
 * No transform enables until this has been called after a reset: a reboot that
 * restarted the counter under an unchanged epoch would be a two-time pad for
 * X_CONF and a full session replay against X_AUTH.
 *
 * Returns 0, ANTW_INVALID_MESSAGE, ANTW_INVALID_PARAMETER_PROVIDED or
 * ANTW_CHANNEL_IN_WRONG_STATE (no key, or no channel period).
 */
antr_err_t antr_sec_epoch_set(uint8_t channel, uint8_t flags, uint32_t epoch,
			      uint64_t us_into_epoch);

/**
 * Read a channel's security state. Message 0xF4, via MESG_REQUEST.
 *
 * @param out      23 bytes, laid out as ANTW_MESG_RADIANT_SEC_STATUS_ID
 *                 documents. Counters are u16 LE and saturate rather than wrap.
 * @param out_len  Capacity of `out`; must be at least 23.
 *
 * An unkeyed channel answers with zeros and a CLEAR verdict rather than an
 * error: "no security on this channel" is a state a host must be able to read
 * without treating an ordinary channel as a fault.
 *
 * This is what makes deliver-as-unverified auditable. A host that ignores the
 * per-message verdict flag can still see that unverified windows happened, and
 * "deliver but mark" only means something if unverified cannot be silently
 * treated as verified.
 *
 * Returns 0 or ANTW_INVALID_MESSAGE (channel out of range, or buffer too small).
 */
antr_err_t antr_sec_status_get(uint8_t channel, uint8_t *out, uint8_t out_len);

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)

/**
 * Drive the pairing exchange. Message 0xF5.
 *
 * @param subcmd     ANTW_RADIANT_PAIR_LEAVE / _ENTER / _SCALAR / _EXCHANGE.
 * @param arg        The sub-command's argument: a timeout byte for _ENTER, 32
 *                   bytes for _SCALAR and _EXCHANGE, nothing for _LEAVE.
 * @param reply      Out. Always leads with [channel, subcmd] so a host reading
 *                   a stream can tell which sub-command answered - a public key
 *                   and a fingerprint are both "bytes after a channel byte"
 *                   otherwise. _SCALAR appends the 32-byte local public key;
 *                   _EXCHANGE appends the six-digit fingerprint as a u24 LE.
 * @param reply_cap  Capacity of `reply`. 34 covers every sub-command.
 * @param reply_len  Out: bytes written.
 *
 * PAIRING HAPPENS IN THE CLEAR. An active attacker present during the exchange
 * can be the man in the middle, and the exchange succeeds anyway - that is what
 * an anonymous key agreement is, not a defect. The fingerprint is the whole
 * mitigation and it only works if a human compares it against the other end's.
 * Out-of-band pairing (a QR code, a typed key) is the recommended path for
 * anything that matters.
 *
 * Returns 0, ANTW_INVALID_MESSAGE (channel or lengths), ANTW_INVALID_PARAMETER_
 * PROVIDED (unknown sub-command, or a small-order peer key) or
 * ANTW_CHANNEL_IN_WRONG_STATE.
 */
antr_err_t antr_sec_pair(uint8_t channel, uint8_t subcmd, const uint8_t *arg,
			 uint8_t arg_len, uint8_t *reply, uint8_t reply_cap,
			 uint8_t *reply_len);

#endif /* CONFIG_RADIANT_SEC_PAIRING_X25519 */

#endif /* CONFIG_RADIANT_SEC_HOST_MESSAGES */

#endif /* ANT_RADIO_H_ */
