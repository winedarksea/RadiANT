/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_api.c - the integration layer: src/ant_radio.h's fifty antr_* entry
 * points on top of the six radiant_core modules, and the seams none of them could
 * close for themselves.
 *
 * Provenance: clean-room. Written from src/ant_radio.h and
 * docs/sdk-ant-contract.md (the antr_* semantics, the permitted error codes,
 * the ordering and threading tables, the burst buffer-ownership obligations
 * B1-B5 and the permanent key -> address limitation), from src/ant_wire.h -
 * generated from protocol/ant_wire.yaml - for every wire constant, and from the
 * six radiant_core module headers this file composes. Nothing here derives from
 * sdk-ant, from libant.a, from disassembly of any binary, or from any
 * adopter-gated ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * INCLUDE ORDER IS LOAD-BEARING
 * ---------------------------------------------------------------------------
 * src/ant_wire.h must be in scope BEFORE radiant_channel.h. That header spells its
 * response and event codes as literals so that it compiles against nothing but
 * radiant_core/include, and guards a block of _Static_asserts on
 * `#ifdef ANTW_RESPONSE_NO_ERROR` that compare every one of them against the
 * generated wire header. This translation unit is the only one in the image
 * where both headers are present, so it is the only place those asserts can
 * run - and if the order were reversed they would silently compile out and the
 * check would look like it was happening when it was not.
 *
 * ---------------------------------------------------------------------------
 * WHAT THIS FILE OWNS, AND WHAT IT DELIBERATELY DOES NOT
 * ---------------------------------------------------------------------------
 * It owns only what no module owns:
 *
 *   - the key -> address table (there is no ant_net.c; see below),
 *   - the selective-data-update mask table, the advanced-burst configuration
 *     block and the encryption slots, none of which any module models,
 *   - the per-channel device ID list, because radiant_search.c turned out not to
 *     have one (see the report),
 *   - one broadcast payload per channel, which is both what
 *     antr_broadcast_message_tx() queues and what an acknowledgement carries,
 *   - the event thread, and the surrogate operation ids that let radiant_channel.c
 *     attribute a terminal event to a channel when the arming authority in
 *     between is radiant_sched.c rather than the HAL.
 *
 * Everything else is a forward. Channel state, every configuration byte and the
 * slot clock are radiant_channel.c's; the schedule is radiant_sched.c's; the sweep is
 * radiant_search.c's; the queue and the wire assembly are radiant_event.c's; the
 * acknowledged/burst engine is radiant_transfer's. Where a forward looks like it is
 * doing nothing, that is the shape working.
 *
 * ---------------------------------------------------------------------------
 * POSTING IS NOT COMMITTING
 * ---------------------------------------------------------------------------
 * radiant_sched_request_rx()/_tx() change the plan; radiant_sched_tick() acts on it.
 * From thread context this file therefore posts everything it knows about and
 * ticks ONCE, at the end of pump(). That is not ceremony: a scheduler that
 * armed on every post would commit the radio to the first channel's window
 * before the second channel - whose window overlaps it exactly - had said a
 * word, and the two would never share a window. Merged receive windows are the
 * highest-value item in the scheduler, so the discipline is worth a comment
 * every time it is relied on.
 *
 * From inside a callback nothing calls tick at all, because the scheduler runs
 * the commit itself on the way out. The one thing this file does do from a
 * callback is set a flag and give a semaphore; the transfer engine re-arms from
 * inside its own callbacks, which at a 1.55 ms turnaround is the only path that
 * meets the deadline.
 *
 * radiant_sched.c owns no timer. A request that lost an arm race (RADIANT_RADIO_EBUSY)
 * or was posted before the radio was enabled (RADIANT_RADIO_ESTATE) stays pending
 * with nothing scheduled to retry it, so RADIANT_API_HOUSEKEEP_MS exists to bound
 * how long that can last.
 *
 * ---------------------------------------------------------------------------
 * radiant_event_drain() IS THREAD CONTEXT ONLY
 * ---------------------------------------------------------------------------
 * docs/sdk-ant-contract.md forbids a backend calling antr_on_message()
 * recursively from inside a call the bridge made, and radiant_event.h says the
 * drain must not be re-entered. So the drain never happens inside an antr_*
 * function - not even antr_stack_reset(), which discards the queue rather than
 * draining it for exactly this reason. It happens on this file's own thread,
 * woken by radiant_event_wakeup().
 *
 * ---------------------------------------------------------------------------
 * THE PERMANENT LIMITATION: key -> address
 * ---------------------------------------------------------------------------
 * There is no ant_net.c and there never will be a key -> address function here.
 * The derivation is not public, and fitting it from samples is the activity
 * most likely to be characterised as reverse-engineering; the clean-room policy
 * rules it out. What replaces it is the one-entry table below, seeded with the
 * published ANT+ pair, and ANTW_INVALID_PARAMETER_PROVIDED for everything else.
 * That is the correct behaviour, not a to-do: every ANT+ profile, every fitness
 * app and every shipping sensor uses the one key that is in the table.
 *
 * The consequence for A/B work, restated because it is easy to trip over:
 * libant.a computes the address, so a comparison between backends must use the
 * ANT+ key or it is comparing a table against an algorithm.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* First, and see the note above. */
#include "ant_wire.h"
#include "ant_radio.h"

#include <radiant_core/radiant_api.h>
#include <radiant_core/radiant_channel.h>
#include <radiant_core/radiant_event.h>
#include <radiant_core/radiant_frame.h>
#include <radiant_core/radiant_radio_hal.h>
#include <radiant_core/radiant_sched.h>
#include <radiant_core/radiant_search.h>
#include <radiant_core/radiant_transfer.h>

/* CONFIG_RADIANT_CORE_LOG_LEVEL, not a hardcoded LOG_LEVEL_INF. A module that
 * ignores its own Kconfig log level cannot be turned up when it is the one
 * misbehaving, and every LOG_DBG in it is dead code that reads as live. */
LOG_MODULE_REGISTER(radiant_api, CONFIG_RADIANT_CORE_LOG_LEVEL);

/* ---------------------------------------------------------------------------
 * The agreement between src/ant_radio.h and radiant_transfer.h
 *
 * RADIANT_TRANSFER_SEG_* were chosen equal to ANTR_BURST_SEGMENT_* so that
 * antr_burst_tx() forwards the byte untouched instead of remapping it, and
 * radiant_transfer.h cannot include src/ant_radio.h to check that - it must compile
 * with no application header in scope. This translation unit has both, so the
 * check lives here, exactly as the sdk-ant shim carries the same three asserts
 * against sdk-ant's BURST_SEGMENT_*.
 *
 * IT HAS ALREADY BEEN WRONG ONCE. ANTR_BURST_SEGMENT_END was 0x04 - an
 * assumption about the bit pattern src/ant_serial_bridge.c assembles - until a
 * shim compared it with sdk-ant's 0x02. Forwarding the wrong value marks the
 * last block of every burst as a middle block, so TRANSFER_TX_COMPLETED never
 * arrives and the bridge's burst semaphore times out at 1000 ms per transfer:
 * a burst that should take two seconds takes ten minutes and nothing says why.
 * Two constants for one concept only stay equal if something checks.
 * ---------------------------------------------------------------------------
 */
BUILD_ASSERT(RADIANT_TRANSFER_SEG_CONTINUE == ANTR_BURST_SEGMENT_CONTINUE,
	     "burst segment drift: CONTINUE");
BUILD_ASSERT(RADIANT_TRANSFER_SEG_START == ANTR_BURST_SEGMENT_START,
	     "burst segment drift: START");
BUILD_ASSERT(RADIANT_TRANSFER_SEG_END == ANTR_BURST_SEGMENT_END,
	     "burst segment drift: END");

/* The channel ceiling has to agree in all three places that size an array from
 * it, and the wire's own limit is the burst header's low five bits. */
BUILD_ASSERT(RADIANT_CHANNEL_COUNT == RADIANT_SCHED_MAX_CHANNELS,
	     "channel count drift: radiant_channel vs radiant_sched");
BUILD_ASSERT(RADIANT_CHANNEL_COUNT <= (RADIANT_SEARCH_MAX_CHANNELS),
	     "channel count drift: radiant_channel vs radiant_search");
BUILD_ASSERT(RADIANT_CHANNEL_COUNT == (ANTW_BURST_HEADER_CHANNEL_MASK + 1),
	     "channel count drift: radiant_channel vs the burst header");

/* ---------------------------------------------------------------------------
 * Sizing
 * ---------------------------------------------------------------------------
 */

#define API_CHANNELS ((uint8_t)RADIANT_CHANNEL_COUNT)
#define API_NETWORKS ((uint8_t)RADIANT_CHANNEL_NETWORK_COUNT)

/* Selective-data-update masks. Four is what the reference behaviour in
 * src/ant_radio_stub.c:92 uses and what tools/ant_features.py round-trips
 * against; nothing in this project sends a fifth. */
#define API_SDU_MASKS 4u

/* Encryption key slots. Same reasoning: the count the stub answers with, so an
 * A/B diff of the two backends does not turn on it. */
#define API_CRYPTO_KEYS 4u
#define API_CRYPTO_ID_SIZE                                                    \
	(ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE - ANTW_CHANNEL_NUM_SIZE)

/*
 * Device IDs one channel's inclusion/exclusion list may hold.
 *
 * Four, because that is what a retail stick accepts and because the list is
 * consulted once per acquisition rather than once per frame, so a linear scan
 * of four is free. An index or a size beyond this is ANTW_INVALID_LIST_ID,
 * which is the code src/ant_radio.h reserves for exactly this.
 */
#define API_ID_LIST_MAX 4u
#define API_ID_LIST_BYTES 4u /* [devnum_lo][devnum_hi][dtype][ttype] */

/*
 * ANT's LF clock tolerance, worst case, restated from radiant_channel.h so the
 * BUILD_ASSERT below has a named number to check against rather than a bare
 * 25u a future edit could change without noticing what it was guarding.
 * +/-50 ppm at each end is +/-100 ppm relative; over one
 * RADIANT_CHANNEL_PERIOD_ANT_PLUS period (~249.7 ms) that is +/-25 us - a BUDGET,
 * not headroom: this bench's own master sits exactly on it, with nothing
 * spare between "typical" and "worst case" (docs/ant-radio-link.md,
 * docs/sdk-ant-comparison.md item 3).
 */
#define API_SLOT_DRIFT_WORST_US 25u

/*
 * Half-width of a tracked channel's receive window, in microseconds.
 *
 * This is a guard against OUR OWN drift and clock error, not against a sloppy
 * master - but "our own drift and clock error" is bounded by the same spec
 * figure, and it COMPOUNDS across consecutive missed slots:
 * radiant_channel_on_slot_missed() extrapolates one more period per miss with
 * no fresh sync, so by the time a window is armed after N consecutive misses
 * it must cover up to N * API_SLOT_DRIFT_WORST_US of additional worst-case
 * disagreement. The BUILD_ASSERT below is that check, made permanent: 400 us
 * covers RADIANT_CHANNEL_RX_FAIL_TO_SEARCH (8) consecutive misses at this
 * spec's worst case with margin to spare, and stays comfortably inside
 * RADIANT_SCHED_MERGE_SPAN_MAX_US, so two tracked channels asking for nearby
 * instants can still merge into one hardware window - which is the whole
 * return on the scheduler. radiant_sched.c subtracts caps.ramp_up_us and
 * caps.min_arm_lead_us itself; nothing backend-specific belongs here.
 */
#define API_SLOT_GUARD_US 400u

/*
 * The invariant the two comments above describe, checked rather than merely
 * claimed. If RADIANT_CHANNEL_RX_FAIL_TO_SEARCH grows, or the guard shrinks,
 * this is what catches a channel that could lose its window on the last
 * miss before falling back to search - silently, since a lost window at that
 * point looks identical to the master having gone quiet.
 */
BUILD_ASSERT(API_SLOT_GUARD_US >=
	     (uint32_t)RADIANT_CHANNEL_RX_FAIL_TO_SEARCH * API_SLOT_DRIFT_WORST_US,
	     "the tracked-channel guard no longer covers worst-case LF clock "
	     "drift across every consecutive miss up to RX_FAIL_TO_SEARCH");

/*
 * Extended assignment bit that puts a channel into background scanning mode.
 *
 * NOT IN src/ant_wire.h, and that is a gap rather than a choice: nothing in
 * protocol/ant_wire.yaml names the extended-assignment bits, because until now
 * nothing in the tree acted on one. 0x01 is the value the free ANT Message
 * Protocol and Usage Rev 5.1 gives for background scanning enable. It is
 * defined here with the prefix that says it is ours, so that a grep for
 * ANTW_EXT_ASSIGN_* does not find a constant that is not generated; the change
 * request against protocol/ant_wire.yaml is in this wave's report.
 */
#define API_EXT_ASSIGN_BACKGROUND_SCAN 0x01u

/*
 * The event thread. 1280 bytes covers radiant_event_drain()'s frame - one queued
 * slot plus the ten-byte extended tail - and the scheduling pass pump() runs
 * afterwards, which is arithmetic over the slot table and nothing deeper.
 *
 * PRIORITY 6 IS BELOW THE BRIDGE'S PARSER THREAD (5), AND THE INEQUALITY IS THE
 * WHOLE POINT. It used to be 4 - above it - on the reasoning that a drained
 * message should reach the writer before the parser takes the next host frame.
 * That reasoning is about two host frames; the ordering that actually matters is
 * between ONE host frame and its own reply, and above the parser it is wrong:
 *
 *   host --> MESG_CLOSE_CHANNEL (0x4C)
 *   dongle <-- EVENT_CHANNEL_CLOSED        <- unsolicited, and FIRST
 *   dongle <-- RESPONSE to 0x4C, code 0    <- the reply to the command
 *
 * That is a real capture from the bench, not a hypothetical. antr_channel_close()
 * runs on the parser thread and completes the close inline, so
 * radiant_channel_event_out() queues CHANNEL_CLOSED and radiant_event_wakeup()
 * makes this thread runnable - which, at priority 4, preempted the parser before
 * it had sent send_response(). Every host library sends a command and then reads
 * until it finds that command's response, discarding what it passes; ANT's own
 * documented close sequence is response first, event second. So the event was
 * eaten by the very loop waiting for the reply, the host never learned the
 * channel had closed, and the next antr_channel_unassign() was refused
 * CHANNEL_IN_WRONG_STATE - which is how one lost event ends a session.
 *
 * Below the parser, the ordering is structural rather than lucky: on one core a
 * lower-priority thread cannot run until the parser blocks, and the parser
 * blocks only at k_sem_take(&ant_rx_sem) once it has dispatched a frame AND
 * written its response. Everything that waits inside dispatch - handle_burst()'s
 * block semaphore, handle_reset()'s k_sleep() - yields properly, so nothing here
 * is starved by the change; it is merely no longer able to interpose itself
 * between a command and its reply.
 *
 * KEEP THIS NUMBER GREATER THAN BRIDGE_PRIORITY in src/ant_serial_bridge.c.
 * BUILD_ASSERT below is that requirement made permanent rather than remembered.
 */
#define API_EVENT_STACK_SIZE 1280
#define API_EVENT_PRIORITY   6

BUILD_ASSERT(API_EVENT_PRIORITY > ANTR_HOST_THREAD_PRIORITY,
	     "the radiant_core event thread must be LOWER priority than the "
	     "thread the bridge makes antr_* calls on, or an event raised "
	     "inside a command is delivered before that command's response");

/* ---------------------------------------------------------------------------
 * The key -> address table
 * ---------------------------------------------------------------------------
 */

static const uint8_t api_ant_plus_key[8] = {
	0xB9u, 0xA5u, 0x21u, 0xFBu, 0xBDu, 0x72u, 0xC3u, 0x45u
};

/* ---------------------------------------------------------------------------
 * State
 * ---------------------------------------------------------------------------
 */

/* What this channel's scheduler slot currently holds. The scheduler routes by
 * channel, so this is how a completion is attributed to the thing that asked
 * for it without keeping a second map. */
enum api_slot_kind {
	API_SLOT_NONE = 0,
	API_SLOT_TRACK_RX,  /* a tracked channel's predicted receive window */
	API_SLOT_MASTER_TX, /* a master's own slot */
	API_SLOT_MASTER_RX, /* the turnaround a master listens in after its slot */
	API_SLOT_SEARCH     /* the shared wildcard sweep, carried on this slot */
};

struct api_chan {
	/*
	 * The surrogate operation id bound into radiant_channel.c.
	 *
	 * radiant_channel.h asks for the id radiant_radio_tx()/radiant_radio_rx() returned,
	 * because a late terminal event has to be recognisable. With
	 * radiant_sched.c between the two there is no such id to hand over - the
	 * scheduler merges several channels' requests into one hardware
	 * operation and exposes only per-channel completions - so this file
	 * mints its own non-zero token per accepted post. It is opaque and
	 * used only for attribution, which is all radiant_channel.c does with it.
	 */
	uint32_t op;

	uint8_t slot_kind;
	/* Something arrived in the window that is now ending. What tells a
	 * closed-empty window apart from one that heard its master. */
	bool slot_heard;

	/* The HAL requires a filter array to stay valid until the terminal
	 * event, so it cannot be a stack local. */
	struct radiant_rx_filter filter;

	/* Likewise the transmit body: backends DMA straight out of it. */
	uint8_t tx_body[RADIANT_FRAME_BODY_MAX];
	uint8_t tx_body_len;

	/* The channel's broadcast payload. Both what antr_broadcast_message_tx()
	 * queues for the next slot and what an acknowledgement carries - every
	 * acknowledgement ever captured carried the acknowledger's own broadcast
	 * buffer (docs/spike-b-part2-results.md). */
	uint8_t bcast[ANTW_ANT_MAX_PAYLOAD_SIZE];
	bool    bcast_valid;
	bool    bcast_pending;

	uint8_t id_list[API_ID_LIST_MAX][API_ID_LIST_BYTES];
	uint8_t id_list_size;
	bool    id_list_exclude;

	uint8_t crypto_mode;

	/* On-air packets of the inbound transfer so far. The serial layer wants
	 * to know whether a completed inbound exchange was acknowledged data or
	 * a burst, and the air cannot tell it: acknowledged data is byte for
	 * byte a one-packet burst. One packet ending with LAST is the former;
	 * anything longer is the latter. */
	uint8_t in_pkts;

	/*
	 * The master turnaround, deferred from api_sched_tx() to api_sched_done().
	 *
	 * It used to be posted straight out of the transmit callback, and that
	 * destroyed its own transmit's completion: radiant_sched_request_rx()
	 * calls drop_slot() -> abort_armed() on a slot that is still in flight,
	 * clear_armed() then makes hal_tx()'s end_armed() a no-op, and no done()
	 * is ever delivered for a listening master's own slot. Consequences were
	 * a CLOSING channel that never finished closing if the close raced a
	 * master slot (radiant_channel_on_terminal() is the only route to
	 * chan_finish_close()), and radiant_radio_abort() called on the real
	 * RADIO from inside a completed transmit's own ISR four times a second.
	 *
	 * Deferring by one callback costs nothing that matters. Both callbacks
	 * are the same radio interrupt, the achieved t_sync - a hardware capture
	 * of RADIO's own ADDRESS event, not the requested instant - is carried
	 * here rather than recomputed, and radiant_sched.c still commits the
	 * post on the way out of the same dispatch. What it buys is that the
	 * scheduler has fully retired the transmit before its slot is reused.
	 */
	bool           turn_pending;
	radiant_time_t turn_t_sync;

	/*
	 * The watchdogs. See RADIANT_API_XFER_WATCHDOG_MS and api_housekeep().
	 *
	 * xfer_stuck_since is when this channel's transfer engine was first seen
	 * non-idle with nothing armed; in_last is when its last inbound packet
	 * arrived. Both carry a separate validity flag rather than using zero as
	 * a sentinel, because zero is a legal radiant_radio_now() reading and a
	 * test that starts the clock there would otherwise arm a watchdog against
	 * a transfer that had not started.
	 */
	bool           xfer_stuck_valid;
	radiant_time_t xfer_stuck_since;
	bool           in_last_valid;
	radiant_time_t in_last;
};

static struct api_chan     api_ch[API_CHANNELS];
static struct radiant_transfer api_xfer[API_CHANNELS];
static struct radiant_search   api_search;

static uint8_t api_net_addr[API_NETWORKS][RADIANT_NET_ADDR_LEN];

/* [enable, rf payload size, required modes, 0, 0, optional modes, 0, 0,
 *  stall count lsb, stall count msb, retry extension] */
static uint8_t api_adv_burst[11];

static uint8_t api_sdu_mask[API_SDU_MASKS][ANTW_ANT_MAX_PAYLOAD_SIZE];

static uint8_t api_crypto_id[API_CRYPTO_ID_SIZE];
static uint8_t api_crypto_user[ANTW_ENCRYPTION_USER_DATA_SIZE];
static uint8_t api_crypto_key[API_CRYPTO_KEYS][ANTW_ENCRYPTION_KEY_SIZE];

static uint32_t api_op_next = 1u;
static uint8_t  api_search_slot = RADIANT_SCHED_CH_NONE;
static bool     api_inited;

static struct radiant_api_stats api_stats;

/*
 * One mutex, held across every thread-context scheduling pass.
 *
 * The bridge's parser thread and this file's event thread both post requests
 * and both call radiant_sched_tick(); without this they would interleave inside a
 * module that was written for one caller at a time. It is taken only in thread
 * context and only around work that is bounded arithmetic - never around a
 * drain, and never anywhere an interrupt could want it.
 *
 * WHAT IT DOES NOT COVER, said plainly rather than left to be found: a radio
 * callback runs a scheduling pass of its own, in interrupt context, and cannot
 * take a mutex. This mutex therefore serialises the two THREADS that post here
 * and nothing else.
 *
 * The thread-against-interrupt half is answered where the shared state actually
 * lives - see "Thread against interrupt" in radiant_sched.c, which now saves
 * and restores `in_pass` rather than clearing it, and runs every write to the
 * slot table with interrupts off. That was the right place for it: an irq_lock()
 * here would have had to wrap real work in this file to cover state that is not
 * this file's.
 */
K_MUTEX_DEFINE(api_lock);

/* Given once per queued message, possibly from the radio ISR, and once per
 * housekeeping interval by the timeout on the take. */
static K_SEM_DEFINE(api_event_sem, 0, 1);

/*
 * Created in antr_init() rather than defined with K_THREAD_DEFINE, and for a
 * reason rather than a preference: the drain calls antr_on_message(), which
 * must never run before antr_init() has returned 0. A statically started thread
 * would be runnable from the moment the kernel came up, which is before
 * src/main.c has called us. It is the same shape src/ant_serial_bridge.c uses
 * for its own thread.
 */
K_THREAD_STACK_DEFINE(api_event_stack, API_EVENT_STACK_SIZE);
static struct k_thread api_event_thread_data;

static void api_event_thread(void *a, void *b, void *c);
static void api_pump_locked(void);

/* ---------------------------------------------------------------------------
 * The three port hooks radiant_event.c declares and deliberately does not define
 *
 * They are left undefined there so that a build which forgets one fails at the
 * link with a name rather than shipping a silent data race or a queue nothing
 * ever drains. radiant_event.c includes no Zephyr header - it compiles as a
 * freestanding translation unit - so it cannot reach any of this itself.
 * ---------------------------------------------------------------------------
 */

unsigned int radiant_event_crit_enter(void)
{
	/*
	 * irq_lock(), not a mutex. The producer may be a radio ISR and must not
	 * block; the section is a handful of instructions plus one copy, never
	 * calls out and never loops.
	 */
	return irq_lock();
}

void radiant_event_crit_exit(unsigned int key)
{
	irq_unlock(key);
}

void radiant_event_wakeup(void)
{
	/*
	 * O(1), ISR-safe, and it does not call back into radiant_event - the "queue
	 * it and return" rule applies to everything on this path. k_sem_give()
	 * on a semaphore with a limit of 1 collapses a burst of arrivals into
	 * one wake, which is what the drain wants anyway: it drains everything
	 * queued, not one message.
	 */
	k_sem_give(&api_event_sem);
}

/* ---------------------------------------------------------------------------
 * The one symbol radiant_channel.c expects radiant_event.c to define, and it does not
 *
 * radiant_channel.c raises channel events through radiant_channel_event_out();
 * radiant_event.c spells the same operation radiant_event_post_channel_event(). Neither
 * is wrong and neither module could have known about the other's spelling, so
 * the adapter lives here.
 *
 * event_code is an ANTW_EVENT_* wire byte either way, so there is no mapping to
 * do. It may be called from a radio callback - radiant_sched.c's context is a radio
 * callback whenever a search times out or a close completes - so it must not
 * block, and posting to the ring does not.
 * ---------------------------------------------------------------------------
 */
void radiant_channel_event_out(uint8_t channel, uint8_t event_code)
{
	(void)radiant_event_post_channel_event(channel, event_code);
}

/* ---------------------------------------------------------------------------
 * Small helpers
 * ---------------------------------------------------------------------------
 */

static bool api_ch_valid(uint8_t channel)
{
	return channel < API_CHANNELS;
}

static uint32_t api_next_op(void)
{
	/* Never zero: radiant_channel_op_owner() treats 0 as "no operation" so that
	 * an idle channel's cleared binding cannot match a real event. */
	api_op_next++;
	if (api_op_next == 0u) {
		api_op_next = 1u;
	}
	return api_op_next;
}

static void api_bind_accepted(uint8_t ch, enum api_slot_kind kind)
{
	api_ch[ch].op = api_next_op();
	api_ch[ch].slot_kind = (uint8_t)kind;
	api_ch[ch].slot_heard = false;
	radiant_channel_bind_op(ch, api_ch[ch].op);
}

static void api_bind_rejected(uint8_t ch)
{
	api_ch[ch].op = 0u;
	api_ch[ch].slot_kind = (uint8_t)API_SLOT_NONE;
	radiant_channel_bind_op(ch, 0u);
}

static const uint8_t *api_net_addr_for(uint8_t ch)
{
	uint8_t net = radiant_channel_network_get(ch);

	if (net >= API_NETWORKS) {
		net = 0u;
	}
	return api_net_addr[net];
}

static uint8_t api_rf_of(uint8_t ch)
{
	uint8_t rf = RADIANT_RF_INDEX_ANT_PLUS;

	(void)radiant_channel_rf_freq_get(ch, &rf);
	return rf;
}

/*
 * Levels 0..5 are -20, -12, -4, 0, +4 and +8 dBm. The HAL takes dBm and rounds
 * to the nearest setting the part has; the custom bit is the escape hatch for a
 * raw part-specific value, and a raw value is by definition not portable, which
 * is why nothing in the core sets one except a host that asked for it by name.
 */
static struct radiant_tx_power api_power_of(uint8_t ch)
{
	static const int8_t dbm[6] = { -20, -12, -4, 0, 4, 8 };
	struct radiant_tx_power p;
	uint8_t level = 3u;
	uint8_t custom = 0u;

	memset(&p, 0, sizeof(p));
	(void)radiant_channel_tx_power_get(ch, &level, &custom);

	if ((level & (uint8_t)ANTW_RADIO_TX_POWER_LVL_CUSTOM) != 0u) {
		p.use_raw = true;
		p.raw = custom;
		return p;
	}
	p.dbm = (level < 6u) ? dbm[level] : 0;
	return p;
}

/* radiant_transfer's return codes onto the wire, exactly as radiant_transfer.h states
 * the mapping. It is stated there and implemented here because this is the only
 * place in the tree that can hold both halves of it. */
static antr_err_t api_xfer_err(int rc)
{
	switch (rc) {
	case RADIANT_TRANSFER_OK:
		return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
	case RADIANT_TRANSFER_EBUSY:
		return (antr_err_t)ANTW_TRANSFER_IN_PROGRESS;
	case RADIANT_TRANSFER_ESTATE:
		return (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE;
	case RADIANT_TRANSFER_ESEQ:
		return (antr_err_t)ANTW_TRANSFER_SEQUENCE_NUMBER_ERROR;
	case RADIANT_TRANSFER_ENOTSUP:
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	case RADIANT_TRANSFER_EIO:
		return (antr_err_t)ANTW_TRANSFER_IN_ERROR;
	case RADIANT_TRANSFER_EINVAL:
	default:
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
}

static enum radiant_radio_status api_done_to_status(enum radiant_sched_done why)
{
	switch (why) {
	case RADIANT_SCHED_DONE_OK:
		return RADIANT_RADIO_STATUS_TIMEOUT; /* the window closed */
	case RADIANT_SCHED_DONE_ABORTED:
		return RADIANT_RADIO_STATUS_ABORTED;
	case RADIANT_SCHED_DONE_MISSED:
	case RADIANT_SCHED_DONE_FAILED:
	default:
		return RADIANT_RADIO_STATUS_FAILED;
	}
}

/* ---------------------------------------------------------------------------
 * The device ID list
 *
 * radiant_search.c owns the sweep, the seen cache and one struct
 * radiant_search_id_filter per channel - "what this channel is looking for" - but
 * it has no inclusion/exclusion LIST, so antr_id_list_add() and
 * antr_id_list_config() have nowhere in the modules to go. They are implemented
 * here, and implemented rather than stored-and-ignored: the list is consulted
 * at the one moment it means anything, which is when the sweep offers an
 * acquisition to a channel. A list that was accepted and never consulted would
 * be worse than one that was refused, because a host would believe it had
 * excluded a sensor.
 * ---------------------------------------------------------------------------
 */
static bool api_id_list_permits(uint8_t ch, const struct radiant_channel_id *id)
{
	const struct api_chan *c = &api_ch[ch];
	uint8_t i;
	bool    hit = false;

	if (c->id_list_size == 0u) {
		return true;
	}

	for (i = 0u; i < c->id_list_size; i++) {
		uint16_t dn = (uint16_t)((uint16_t)c->id_list[i][0] |
					 ((uint16_t)c->id_list[i][1] << 8));

		/* Zero is the wildcard in each field, on the same terms as a
		 * channel ID: a list entry naming only a device type excludes
		 * or admits every device of that type. */
		if (dn != 0u && dn != id->device_number) {
			continue;
		}
		if (c->id_list[i][2] != 0u &&
		    (uint8_t)(c->id_list[i][2] & RADIANT_DEVICE_TYPE_MASK) !=
			    (uint8_t)(id->device_type & RADIANT_DEVICE_TYPE_MASK)) {
			continue;
		}
		if (c->id_list[i][3] != 0u &&
		    c->id_list[i][3] != id->trans_type) {
			continue;
		}
		hit = true;
		break;
	}

	return c->id_list_exclude ? !hit : hit;
}

/* ---------------------------------------------------------------------------
 * Handing a received frame to the host
 * ---------------------------------------------------------------------------
 */
static void api_post_rx(uint8_t ch, uint8_t msg_id,
			const struct radiant_channel_id *id,
			const struct radiant_rx_event *hal, const uint8_t *payload,
			uint8_t payload_len, uint8_t burst_seq, bool burst_last)
{
	struct radiant_event_rx r;

	memset(&r, 0, sizeof(r));
	r.hal = hal;
	r.msg_id = msg_id;
	r.channel = ch;
	r.burst_seq = burst_seq;
	r.burst_last = burst_last;
	r.id = *id;
	r.payload = payload;
	r.payload_len = payload_len;

	if (radiant_event_post_rx(&r) == RADIANT_EVENT_OK) {
		api_stats.rx_posted++;
	} else {
		api_stats.rx_dropped++;
	}
}

/* ---------------------------------------------------------------------------
 * The transfer engine's calls back into us
 *
 * ALL THREE MAY RUN IN RADIO INTERRUPT CONTEXT: the engine re-arms from inside
 * a completion callback because at a 1.55 ms turnaround that is the only path
 * that meets the deadline, and these are reached from there. Signalling an
 * ISR-safe object is the intended shape and posting to the event ring is
 * exactly that.
 * ---------------------------------------------------------------------------
 */

static void api_xfer_done(void *ctx, const struct radiant_transfer_result *r)
{
	uint8_t ch = (uint8_t)(uintptr_t)ctx;

	if (!api_ch_valid(ch) || r == NULL) {
		return;
	}

	/*
	 * These three codes are the buffer-ownership contract, B1-B5. The
	 * bridge holds ONE 24-byte block behind a binary semaphore and gives it
	 * back only here; miss NEXT_DATA_BLOCK once per accepted block and a
	 * host burst does not break, it stalls 1000 ms per packet and then
	 * reports a plausible-looking error. The engine guarantees exactly one
	 * done() carries any given block pointer, so this function's whole duty
	 * is to translate without adding or dropping one.
	 */
	switch (r->ev) {
	case RADIANT_TRANSFER_EV_NEXT_BLOCK:
		(void)radiant_event_post_transfer_next_block(ch);
		break;
	case RADIANT_TRANSFER_EV_TX_COMPLETED:
		(void)radiant_event_post_transfer_tx_completed(ch);
		break;
	case RADIANT_TRANSFER_EV_TX_FAILED:
	default:
		(void)radiant_event_post_transfer_tx_failed(ch);
		break;
	}
}

static void api_xfer_rx_data(void *ctx, const uint8_t *payload, uint8_t len,
			     bool last)
{
	uint8_t ch = (uint8_t)(uintptr_t)ctx;
	struct radiant_channel_id id;
	struct radiant_rx_event   hal;
	uint8_t               msg_id;
	uint8_t               seq;

	if (!api_ch_valid(ch) || payload == NULL) {
		return;
	}
	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return;
	}

	seq = (uint8_t)(api_ch[ch].in_pkts & (uint8_t)ANTW_BURST_HEADER_SEQ_MASK);

	/*
	 * One packet ending with LAST is acknowledged data; anything longer is
	 * a burst. The air cannot tell them apart - acknowledged data is byte
	 * for byte a one-packet burst, which is the whole finding of Spike B
	 * part 2 - so the distinction is made here, at the serial layer that
	 * is the only place it exists.
	 */
	if (last && api_ch[ch].in_pkts == 0u) {
		msg_id = (uint8_t)ANTW_MESG_ACKNOWLEDGED_DATA_ID;
	} else {
		msg_id = (uint8_t)ANTW_MESG_BURST_DATA_ID;
	}

	api_ch[ch].in_pkts = last ? 0u : (uint8_t)(api_ch[ch].in_pkts + 1u);
	/* The abandoned-burst watchdog's clock. LAST is the normal way in_pkts
	 * returns to zero; this is what covers the burst whose LAST never
	 * arrives. See api_watchdogs_locked(). */
	api_ch[ch].in_last = radiant_radio_now();
	api_ch[ch].in_last_valid = true;

	/*
	 * radiant_event_post_rx() needs the HAL event, because the CRC gate and the
	 * 0xE0 RX timestamp both come out of it and there must be nowhere else
	 * to put a clock reading. radiant_transfer_ops::rx_data does not carry one -
	 * see the report - so the event is synthesised with t_sync_exact false,
	 * which under the default timestamp policy suppresses the timestamp
	 * field entirely rather than reporting a thread-context reading as if
	 * it were a radio-clock one. Absence is the only way this wire format
	 * can say "I cannot give you the accurate number", and saying nothing
	 * is better than saying something wrong.
	 */
	memset(&hal, 0, sizeof(hal));
	hal.status = RADIANT_RADIO_STATUS_OK;
	hal.t_sync = radiant_radio_now();
	hal.t_sync_exact = false;

	api_post_rx(ch, msg_id, &id, &hal, payload, len, seq, last);
}

static bool api_xfer_broadcast(void *ctx, uint8_t out[RADIANT_TRANSFER_PKT_BYTES])
{
	uint8_t ch = (uint8_t)(uintptr_t)ctx;

	if (!api_ch_valid(ch) || !api_ch[ch].bcast_valid) {
		/*
		 * Every acknowledgement ever captured carried the
		 * acknowledger's own broadcast buffer. Whether one may carry
		 * anything else has never been measured, so a channel with
		 * nothing to broadcast refuses to acknowledge rather than
		 * sending something no measurement supports.
		 */
		return false;
	}

	memcpy(out, api_ch[ch].bcast, RADIANT_TRANSFER_PKT_BYTES);
	return true;
}

static const struct radiant_transfer_ops api_xfer_ops = {
	.done = api_xfer_done,
	.rx_data = api_xfer_rx_data,
	.broadcast_payload = api_xfer_broadcast,
};

/* Configure one channel's engine. Deferred to open, not assign: the engine
 * reads the channel ID, the network address, the RF index and the power at
 * init, and none of them is settled until the host has finished configuring. */
static int api_xfer_setup(uint8_t ch)
{
	struct radiant_transfer_cfg cfg;
	struct radiant_channel_id   id;

	memset(&cfg, 0, sizeof(cfg));
	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return RADIANT_TRANSFER_EINVAL;
	}

	cfg.id = id;
	memcpy(cfg.net_addr, api_net_addr_for(ch), RADIANT_NET_ADDR_LEN);
	cfg.rf_index = api_rf_of(ch);
	cfg.power = api_power_of(ch);
	/*
	 * There is no safe default for the slot bit and radiant_transfer.h refuses
	 * to invent one: a master that sent in-slot bytes would put 0x82 where
	 * 0x8A belongs, and a slave that sent slot-opening bytes would claim a
	 * slot it does not own. Both are one bit and neither is visible without
	 * a sniffer. The channel type is what decides it.
	 */
	cfg.slot_opener = radiant_channel_is_master(ch);
	cfg.channel = ch;
	cfg.ops = &api_xfer_ops;
	cfg.ctx = (void *)(uintptr_t)ch;

	return radiant_transfer_init(&api_xfer[ch], &cfg);
}

/* ---------------------------------------------------------------------------
 * The search
 * ---------------------------------------------------------------------------
 */

static void api_search_cfg_build(struct radiant_search_cfg *cfg)
{
	radiant_search_cfg_default(cfg);
	/*
	 * The sweep runs on network 0's address, which is where every host puts
	 * the ANT+ key. That the default is already A6 C5 is not a coincidence
	 * worth relying on, so it is copied rather than assumed - and because
	 * the key -> address table has exactly one entry, network 0 can only
	 * ever be A6 C5 or unset.
	 */
	memcpy(cfg->net_addr, api_net_addr[0], RADIANT_NET_ADDR_LEN);
}

/*
 * The sweep offered this channel a device.
 *
 * RUNS IN THE RADIO INTERRUPT. Three lines of adapter, plus the device ID list
 * that has nowhere else to live.
 */
static void api_search_acquired(uint8_t channel,
				const struct radiant_search_result *r, void *user)
{
	struct radiant_rx_event hal;

	(void)user;
	if (!api_ch_valid(channel) || r == NULL) {
		return;
	}

	if (!api_id_list_permits(channel, &r->id)) {
		api_stats.id_list_rejects++;
		return;
	}

	radiant_channel_on_acquired(channel, &r->id, r->t_sync);
	api_stats.acquired++;

	/*
	 * ANT delivers the first broadcast along with the channel ID on
	 * acquisition and a channel that had to ask for it again would waste a
	 * period, so the acquiring frame goes to the host here. The HAL event
	 * is synthesised from the search result because struct
	 * radiant_search_result carries the decoded frame and the timing but not
	 * the event it came from - see the report. t_sync and t_sync_exact are
	 * the search module's own, so the timestamp this produces is the real
	 * one whenever the backend could capture it.
	 */
	memset(&hal, 0, sizeof(hal));
	hal.status = RADIANT_RADIO_STATUS_OK;
	hal.t_sync = r->t_sync;
	hal.t_sync_exact = r->t_sync_exact;
	hal.has_rssi = r->has_rssi;
	hal.rssi_dbm = r->rssi_dbm;

	api_post_rx(channel, (uint8_t)ANTW_MESG_BROADCAST_DATA_ID, &r->id, &hal,
		    r->frame.payload, r->frame.payload_len, 0u, false);

	/*
	 * RADIANT_SEARCH_MODE_ACQUIRE says the channel leaves the search with an ID
	 * and belongs to the scheduler's tracked set from here on. Scan mode
	 * never leaves, and the state machine says which this is.
	 */
	if (radiant_channel_state_get(channel) == RADIANT_CH_STATE_TRACKING) {
		(void)radiant_search_end(&api_search, channel);
	}
}

/*
 * SEARCH TIMEOUT IS OWNED BY radiant_channel.c, NOT HERE.
 *
 * It is expressible in both modules - radiant_channel_search_deadline() and
 * radiant_search_begin(timeout_us) - and two owners of one deadline is how a
 * channel times out twice or not at all. radiant_channel.c wins because it already
 * has the high-priority/low-priority tick semantics the serial protocol
 * defines, because it is the module that has to raise RX_SEARCH_TIMEOUT and
 * then CHANNEL_CLOSED in that order, and because a search timeout that did not
 * close the channel would leave it un-unassignable for ever.
 *
 * So every radiant_search_begin() below passes RADIANT_SEARCH_TIMEOUT_NONE and this
 * callback cannot fire. It is registered anyway, because a NULL entry would
 * make "nothing calls it" indistinguishable from "nobody wired it up".
 */
static void api_search_timeout(uint8_t channel, void *user)
{
	(void)channel;
	(void)user;
}

static const struct radiant_search_cbs api_search_cbs = {
	.acquired = api_search_acquired,
	.timeout = api_search_timeout,
};

static void api_search_begin(uint8_t ch, radiant_time_t now)
{
	struct radiant_search_id_filter want;
	struct radiant_channel_id       id;
	enum radiant_search_mode        mode;

	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return;
	}

	memset(&want, 0, sizeof(want));
	want.device_number = id.device_number;
	want.device_type = id.device_type;
	want.trans_type = id.trans_type;

	mode = ((radiant_channel_ext_assign_get(ch) &
		 API_EXT_ASSIGN_BACKGROUND_SCAN) != 0u)
		       ? RADIANT_SEARCH_MODE_SCAN
		       : RADIANT_SEARCH_MODE_ACQUIRE;

	(void)radiant_search_begin(&api_search, ch, mode, &want, now,
			       RADIANT_SEARCH_TIMEOUT_NONE);
}

/* ---------------------------------------------------------------------------
 * Posting requests to the scheduler. Thread context, under api_lock.
 * ---------------------------------------------------------------------------
 */

/*
 * True while a radiant_sched_*() call that may invoke done() SYNCHRONOUSLY is
 * in progress.
 *
 * The scheduler completes a request from inside the arm call when the arm is
 * refused, and radiant_radio_hal.h is explicit that posting the next request
 * from a completion is the intended low-jitter path. Both are correct.
 * Together, against a backend that refuses EVERY arm, they are unbounded
 * mutual re-entry: post -> arm -> refused -> done() -> post -> ...
 *
 * api_search_slot was meant to be the one-sweep-at-a-time guard and cannot do
 * this job, because api_sched_done() CLEARS it - so the synchronous completion
 * destroys the very interlock that would have excluded it.
 *
 * Found on the bench, not by inspection: opening one wildcard channel wedged
 * the firmware completely, with both the nrf backend (every arm ETIME) and the
 * null backend (every arm ENOTSUP), and a halted CPU showed the stack walked
 * down to within 648 bytes of PSPLIM through api_post_search_window ->
 * radiant_search_window -> arm_rx_window -> clear_armed -> api_sched_done.
 *
 * A refused arm must leave the next attempt to the next pump, which is the
 * thing that has a rate limit.
 */
static bool api_arming;
static void api_post_search_window(radiant_time_t now)
{
	const struct radiant_radio_caps *caps = radiant_radio_caps_get();
	struct radiant_search_window     w;
	struct radiant_sched_rx          req;
	uint8_t                      ch;

	/*
	 * Not from inside an arm. See api_arming - this is the guard that stops
	 * a backend refusing every arm from recursing until the stack runs out.
	 */
	if (api_arming) {
		return;
	}

	/* One sweep at a time - radiant_search.c holds exactly one window in
	 * flight, which is what makes its filters[] pointer safe to hand
	 * straight to the HAL. */
	if (api_search_slot != RADIANT_SCHED_CH_NONE) {
		return;
	}

	/*
	 * ONE SWEEP SERVES EVERY SEARCHING CHANNEL - that is the point of
	 * radiant_search.c and the difference between eight simultaneous searches
	 * taking ~8 s and taking ~64 s. The scheduler's slots are per channel,
	 * so the sweep is carried on the lowest-numbered searching channel's
	 * slot and every channel in the search is served by it. The choice of
	 * carrier is arbitrary; that there is only one is not.
	 */
	for (ch = 0u; ch < API_CHANNELS; ch++) {
		if (radiant_search_is_searching(&api_search, ch)) {
			break;
		}
	}
	if (ch >= API_CHANNELS || radiant_sched_pending(ch)) {
		return;
	}

	if (radiant_search_window(&api_search,
			      now + (radiant_time_t)caps->min_arm_lead_us,
			      &w) != RADIANT_SEARCH_OK) {
		return;
	}

	/*
	 * THE SWEEP EATS TRACKED SLOTS, AND THIS IS NOT WHERE TO FIX IT.
	 *
	 * A sweep window is a BOUNDED receive that wants to start immediately,
	 * and radiant_sched.c's arm_next() runs "whatever starts first". A tracked
	 * slave's window opens up to a full period later, so the sweep wins the
	 * choice every time and is then armed for its whole remaining dwell with
	 * no limit - arm_next() truncates a bounded receive only to make room for
	 * a TRANSMIT, never for another receive. The tracked window falls inside
	 * the armed sweep, is never armed, and expires RADIANT_SCHED_DONE_MISSED.
	 * want_preempt() cannot rescue it: its "tracked work outranks searching"
	 * rule is gated on the armed operation being CONTINUOUS, and
	 * could_join_armed() refuses the merge because a tracking window's fmt is
	 * CFG_TRACKING against the sweep's CFG_SEARCH.
	 *
	 * MEASURED 2026-08-10, against a master at -25 dBm so signal level cannot
	 * be the explanation: 0.02 % loss with one channel tracking and nothing
	 * searching, 5.96 % with one channel also searching, RX_FAIL x693 in a
	 * 60 s eight-channel session.
	 *
	 * BOUNDING THE REQUEST HERE WAS TRIED AND REVERTED. Ending the window
	 * before the earliest TRACKING channel's next slot fixes the loss
	 * handsomely - RX_FAIL 590 -> 115 and RX_FAIL_GO_TO_SEARCH 68 -> 2 on the
	 * same board and sensors - and starves the sweep, because the earliest
	 * tracked slot is nearly always imminent, so almost every post is refused
	 * as too short and the sweep stops advancing. Two controlled runs, with
	 * masters at -25 dBm confirmed transmitting 81/81 and 26/26 when pinned:
	 * neither a set-9 nor a set-30 device was discovered in 90 s, where the
	 * unbounded sweep finds a set-9 device in 60 s. Trading discovery for
	 * loss is not a fix; a dongle that cannot see a sensor is worse than one
	 * that sees it unsteadily.
	 *
	 * The real fix is to post the sweep as a CONTINUOUS request
	 * (t_close = RADIANT_TIME_NEVER plus chunk_us). radiant_sched.c already
	 * implements exactly the semantics wanted and nothing else does: a
	 * continuous RX is skipped by abort_armed() so it pauses instead of
	 * losing its dwell, want_preempt() lets any bounded request displace it,
	 * and arm_next() gives it s_limit = t_back(committed, lead) so it fills
	 * the gap ahead of committed work rather than guessing at one. It needs
	 * slot-lifecycle work here, because the sweep must change filters every
	 * set while a continuous slot is designed to stay armed until cancelled.
	 */

	memset(&req, 0, sizeof(req));
	req.fmt = w.fmt;
	req.rf_index = w.rf_index;
	req.filters = w.filters;
	req.n_filters = w.n_filters;
	req.t_open = w.t_open;
	req.t_close = w.t_close;
	req.report_crc_fail = (w.flags & RADIANT_RX_REPORT_CRC_FAIL) != 0u;
	/* Never RADIANT_RX_STOP_ON_FIRST on a search window: the HAL forbids it on
	 * anything carrying more than one filter, and several masters may
	 * transmit inside one dwell. */
	req.stop_on_first = false;

	if (radiant_sched_request_rx(ch, &req) != RADIANT_RADIO_OK_RC) {
		radiant_search_arm_failed(&api_search);
		return;
	}

	api_search_slot = ch;
	api_bind_accepted(ch, API_SLOT_SEARCH);
	/*
	 * RADIANT_SEARCH_OP_EXTERNAL: the scheduler merges several channels'
	 * requests into one hardware operation and hands each channel back only
	 * its own events, so there is no HAL operation id to give. With this
	 * token the module stops filtering by op and trusts our routing, which
	 * is correct precisely because we are the thing that did the routing.
	 */
	radiant_search_armed(&api_search, RADIANT_SEARCH_OP_EXTERNAL, w.t_open,
			 w.t_close);
}

static void api_post_track_rx(uint8_t ch)
{
	struct radiant_channel_id id;
	struct radiant_sched_rx   req;
	radiant_time_t            t = radiant_channel_next_slot(ch);
	int                   n;

	if (t == RADIANT_TIME_NEVER) {
		return;
	}
	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return;
	}

	/* An on-air address match IS a channel-ID match, which is why no
	 * software comparison of the sender's ID follows one. */
	n = radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING, api_net_addr_for(ch), &id,
			   api_ch[ch].filter.addr,
			   sizeof(api_ch[ch].filter.addr));
	if (n < 0) {
		return;
	}
	api_ch[ch].filter.addr_len = (uint8_t)n;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = api_rf_of(ch);
	req.filters = &api_ch[ch].filter;
	req.n_filters = 1u;
	req.t_open = (t > (radiant_time_t)API_SLOT_GUARD_US)
			     ? (t - (radiant_time_t)API_SLOT_GUARD_US)
			     : 0u;
	req.t_close = t + (radiant_time_t)API_SLOT_GUARD_US;
	/*
	 * A request, not a guarantee: the HAL forbids STOP_ON_FIRST on a merged
	 * window and the scheduler drops it when this window turns out to carry
	 * more than one channel. Asking for it is still worth it - on a window
	 * that stays single it saves receive current and frees the operation
	 * slot early, which is what lets the next thing be armed from inside
	 * the same callback.
	 */
	req.stop_on_first = true;

	if (radiant_sched_request_rx(ch, &req) != RADIANT_RADIO_OK_RC) {
		api_bind_rejected(ch);
		return;
	}
	api_bind_accepted(ch, API_SLOT_TRACK_RX);
}

/*
 * A MASTER HAS TO LISTEN, AND UNTIL THIS EXISTED IT NEVER DID.
 *
 * api_pump_locked() gives a TRACKING master api_post_master_tx() and nothing
 * else, so the only radio operation a master ever owned was its own
 * transmission. That is a uni-directional master - ANT's MASTER_TX_ONLY - and it
 * is not what CHANNEL_TYPE_MASTER means. Everything a slave can send its master
 * arrives in the turnaround right after the master's own frame, so with no
 * window there the whole reverse direction is unreachable: acknowledged data
 * from a slave is never heard, no acknowledgement is ever returned, and a
 * slave-originated burst cannot start. The slave's stack reports it as a
 * transfer that stayed queued or failed, which reads as a link problem and is
 * not one - the master was not listening.
 *
 * It was invisible from the master's own side, because a master with nothing to
 * hear and a master that cannot hear look identical from the host: EVENT_TX four
 * times a second, exactly as they should be, in both cases.
 *
 * WHEN. RADIANT_TRANSFER_SLOT_REPLY_US - 2190 us from the master's broadcast
 * t_sync to a slave's reply t_sync - is measured, and radiant_transfer.h exports
 * it saying this is what needs it. The guard is that module's own
 * RADIANT_TRANSFER_ACK_GUARD_US, sized on the observed range rather than a
 * standard deviation, so the two edges of this window and the two edges of the
 * acknowledgement window the transfer engine opens are sized by one constant.
 *
 * WHAT. The same on-air address the master transmits: an ANT channel's address
 * is derived from the channel ID either way round, so a slave answering this
 * master sends to the address this master sends from, and the hardware match is
 * the identity check. Never STOP_ON_FIRST - see the note in api_sched_rx() on
 * what a merged window does to it - and here it would be wrong anyway on a
 * shared channel, where more than one slave may answer.
 *
 * COST. About 500 us of receive per 249.7 ms period, which is 0.2 % duty and
 * why it is unconditional rather than opened only when something is outstanding:
 * a slave may begin an exchange in any slot, and a master that listened only
 * when it expected to be spoken to could never hear the first packet.
 */
static void api_post_master_rx(uint8_t ch, radiant_time_t t_sync)
{
	struct radiant_channel_id id;
	struct radiant_sched_rx   req;
	int                       n;

	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return;
	}

	n = radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING, api_net_addr_for(ch), &id,
			   api_ch[ch].filter.addr,
			   sizeof(api_ch[ch].filter.addr));
	if (n < 0) {
		return;
	}
	api_ch[ch].filter.addr_len = (uint8_t)n;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = api_rf_of(ch);
	req.filters = &api_ch[ch].filter;
	req.n_filters = 1u;
	req.t_open = t_sync + (radiant_time_t)RADIANT_TRANSFER_SLOT_REPLY_US -
		     (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US;
	req.t_close = t_sync + (radiant_time_t)RADIANT_TRANSFER_SLOT_REPLY_US +
		      (radiant_time_t)RADIANT_TRANSFER_ACK_GUARD_US;
	req.stop_on_first = false;

	if (radiant_sched_request_rx(ch, &req) != RADIANT_RADIO_OK_RC) {
		api_bind_rejected(ch);
		return;
	}
	api_bind_accepted(ch, API_SLOT_MASTER_RX);
}

/*
 * Bit 4 of the channel type is the master bit, and 0x50 is MASTER_TX_ONLY - the
 * one master that is documented not to listen. Asking the type rather than
 * asking radiant_channel_is_master() is the whole difference between the two
 * master shapes ANT defines, and it is the only place in this file that cares.
 */
static bool api_master_listens(uint8_t ch)
{
	uint8_t type = radiant_channel_type_get(ch);

	return ((type & RADIANT_CH_TYPE_MASTER_BIT) != 0u) &&
	       (type != (uint8_t)ANTW_CHANNEL_TYPE_MASTER_TX_ONLY);
}

static void api_post_master_tx(uint8_t ch)
{
	struct radiant_ctrl_fields fields;
	struct radiant_channel_id  id;
	struct radiant_frame       f;
	struct radiant_frame_wire  w;
	struct radiant_sched_tx    req;
	radiant_time_t             t = radiant_channel_next_slot(ch);

	if (t == RADIANT_TIME_NEVER) {
		return;
	}
	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return;
	}

	/*
	 * A MASTER TRANSMITS FROM ITS FIRST SLOT, WITH OR WITHOUT HOST DATA.
	 *
	 * This used to return here unless bcast_valid, and that is a deadlock
	 * with every host library there is. The ANT pattern is that the host
	 * paces its writes off EVENT_TX: it opens the channel, waits to be told
	 * a slot went out, and writes the next payload in response. A master
	 * that refuses to transmit until the host has written raises no
	 * EVENT_TX, so the host waits for a slot that is waiting for the host.
	 * tools/ant_sim.py is the local statement of that contract - it reports
	 * "no EVENT_TX within two message periods" and falls back to guessing
	 * the pacing from the wall clock, which is exactly what it did here.
	 *
	 * So an unwritten buffer transmits as zeros. api_ch[].bcast is zeroed
	 * at init and by antr_channel_unassign(), so this needs no code of its
	 * own; what it needs is to be a stated decision rather than an
	 * accident, because the two OTHER users of that buffer must NOT do the
	 * same thing. api_xfer_broadcast() still refuses to acknowledge without
	 * a valid payload, and that refusal has a measurement behind it: every
	 * acknowledgement ever captured carried the acknowledger's own
	 * broadcast buffer, and nothing has ever been observed carrying zeros.
	 * A broadcast slot has no such constraint - it is unacknowledged, it is
	 * one page among four per second, and a receiver discards a page it
	 * does not recognise.
	 *
	 * NOT MEASURED on this bench: what a real ANT master puts on the air
	 * before its host writes anything. Zeros are what an unwritten buffer
	 * contains and what the ANT documentation's ordering implies; nothing
	 * here has captured one.
	 */

	/*
	 * 0x0A: not an exchange, not an acknowledgement, not last, sequence 0,
	 * and it opens the slot. That is the only encoding with bit 7 clear
	 * that has ever been on the air, and it is built from the fields rather
	 * than written as a literal so that radiant_frame_make() gets to refuse a
	 * combination nothing has ever transmitted.
	 */
	memset(&fields, 0, sizeof(fields));
	fields.slot_opener = true;

	if (radiant_frame_make(&f, &id, &fields, api_ch[ch].bcast,
			   RADIANT_FRAME_PAYLOAD_STD) != RADIANT_FRAME_OK) {
		return;
	}
	if (radiant_frame_encode(RADIANT_FRAME_CFG_TRACKING, api_net_addr_for(ch), &f,
			     &w) != RADIANT_FRAME_OK) {
		return;
	}
	if ((size_t)w.body_len > sizeof(api_ch[ch].tx_body)) {
		return;
	}

	/* Into the per-channel buffer, not a local: the HAL DMAs out of it and
	 * it must stay valid and unmodified until the completion callback. */
	memcpy(api_ch[ch].tx_body, w.body, w.body_len);
	api_ch[ch].tx_body_len = w.body_len;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = api_rf_of(ch);
	req.power = api_power_of(ch);
	/*
	 * The address radiant_frame_encode() just derived, not a second
	 * derivation of it. w.addr and w.body are the two halves of one frame
	 * and only one of them used to leave this function.
	 */
	if (w.addr_len > sizeof(req.addr)) {
		return;
	}
	memcpy(req.addr, w.addr, w.addr_len);
	req.addr_len = w.addr_len;
	req.body = api_ch[ch].tx_body;
	req.body_len = api_ch[ch].tx_body_len;
	req.t_sync_at = t;

	if (radiant_sched_request_tx(ch, &req) != RADIANT_RADIO_OK_RC) {
		api_bind_rejected(ch);
		return;
	}
	api_bind_accepted(ch, API_SLOT_MASTER_TX);
}

/*
 * One scheduling pass, from thread context. POST EVERYTHING, THEN TICK ONCE.
 *
 * Callers hold api_lock.
 */
static void api_pump_locked(void)
{
	radiant_time_t now;
	uint8_t    ch;

	if (!api_inited) {
		return;
	}

	now = radiant_radio_now();

	for (ch = 0u; ch < API_CHANNELS; ch++) {
		enum radiant_channel_state st = radiant_channel_state_get(ch);
		bool searching = radiant_search_is_searching(&api_search, ch);

		/* Keep the sweep's membership in step with the state machine.
		 * A channel that fell back to SEARCHING after eight missed
		 * slots rejoins here, and the seen cache is what makes that
		 * re-acquisition take about one dwell instead of half a sweep. */
		if (st == RADIANT_CH_STATE_SEARCHING) {
			if (!searching) {
				api_search_begin(ch, now);
			}
		} else if (searching) {
			/*
			 * Note what is NOT done here: if this channel was the
			 * one carrying the sweep, api_search_slot is left
			 * alone. radiant_search_end() deliberately does not touch
			 * the window in flight - only the scheduler knows what
			 * else wants the radio - so that window still runs and
			 * still serves every other searching channel, and its
			 * completion is what releases the carrier. Clearing the
			 * carrier here instead would let a second sweep be
			 * posted while the first was live, and the first one's
			 * done() would then credit a dwell to the second one's
			 * set.
			 */
			(void)radiant_search_end(&api_search, ch);
		}

		if (radiant_sched_pending(ch)) {
			continue;
		}
		/* The transfer engine arms through the same slot, and it has
		 * the tighter deadline: 1.55 ms against a 250 ms period. */
		if (!radiant_transfer_is_idle(&api_xfer[ch])) {
			continue;
		}

		if (st != RADIANT_CH_STATE_TRACKING) {
			continue;
		}
		if (radiant_channel_is_master(ch)) {
			api_post_master_tx(ch);
		} else {
			api_post_track_rx(ch);
		}
	}

	api_post_search_window(now);

	/* THE one commit. See api_arming: an arm refused in here completes
	 * synchronously, and nothing it calls may post again. */
	api_arming = true;
	(void)radiant_sched_tick();
	api_arming = false;
	api_stats.pumps++;
}

/* ---------------------------------------------------------------------------
 * The scheduler's calls back into us. Radio interrupt context.
 * ---------------------------------------------------------------------------
 */

/* Decode a frame that arrived on a tracked window and do whatever it is. */
static void api_tracked_frame(uint8_t ch, const struct radiant_rx_event *evt)
{
	struct radiant_channel_id id;
	struct radiant_frame_wire w;
	struct radiant_frame      f;
	enum radiant_msg_type     mt;
	int                   n;
	int                   rc;

	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return;
	}
	if (evt->body == NULL || evt->body_len == 0u ||
	    (size_t)evt->body_len > sizeof(w.body)) {
		return;
	}

	/*
	 * The matched address bytes never reach RAM - that is what a hardware
	 * address match means - so the address half of the wire frame is
	 * regenerated from the ID this window filtered on, which is exactly the
	 * identity the match proved. RADIANT_FRAME_TRUSTED_CRC is right for every
	 * delivered event: the HAL says a packet delivered with status OK has a
	 * verified CRC, and the received CRC bytes are not available here.
	 */
	memset(&w, 0, sizeof(w));
	n = radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING, api_net_addr_for(ch), &id,
			   w.addr, sizeof(w.addr));
	if (n < 0) {
		return;
	}
	w.addr_len = (uint8_t)n;
	memcpy(w.body, evt->body, evt->body_len);
	w.body_len = evt->body_len;

	if (radiant_frame_decode(RADIANT_FRAME_CFG_TRACKING, &w, RADIANT_FRAME_TRUSTED_CRC,
			     &f) != RADIANT_FRAME_OK) {
		return;
	}

	/*
	 * The slot clock is re-anchored on the frame that was actually on the
	 * air, which is what keeps the window centred on the master we hear
	 * rather than on the moment the host opened the channel.
	 *
	 * A MASTER MUST NOT DO EITHER OF THESE. It owns the slot grid rather than
	 * following one, and the frame it is hearing is a slave's reply 2.19 ms
	 * INTO its own period - so re-anchoring here would drag the master's
	 * phase forward by that much on every exchange and hand its own slaves a
	 * moving target. The seen cache is likewise about masters this device
	 * searched for and found, and a slave answering us is neither.
	 */
	if (!radiant_channel_is_master(ch)) {
		radiant_channel_on_slot(ch, evt->t_sync);
		radiant_search_seen_note(&api_search, &id, evt->t_sync);
	}

	mt = radiant_frame_msg_type(f.ctrl_byte);
	LOG_DBG("frame ch=%u kind=%u ctrl=0x%02X mt=%d xfer=%d reply=0x%02X bc=%u",
		(unsigned)ch, (unsigned)api_ch[ch].slot_kind,
		(unsigned)f.ctrl_byte, (int)mt,
		(int)radiant_transfer_state(&api_xfer[ch]),
		(unsigned)radiant_transfer_reply_ctrl(f.ctrl_byte),
		(unsigned)api_ch[ch].bcast_valid);
	switch (mt) {
	case RADIANT_MSG_BROADCAST:
		api_post_rx(ch, (uint8_t)ANTW_MESG_BROADCAST_DATA_ID, &id, evt,
			    f.payload, f.payload_len, 0u, false);
		break;
	case RADIANT_MSG_BURST_DATA:
	case RADIANT_MSG_BURST_LAST:
		/*
		 * The receive path that has to TRANSMIT. About 1.55 ms between
		 * this packet's t_sync and the instant the peer expects a
		 * reply, and radiant_transfer_on_data() arms the acknowledgement
		 * before it hands the payload up - which is why the host-facing
		 * message is posted from api_xfer_rx_data() rather than here.
		 */
		/* Not inside the LOG_DBG argument list. LOG_DBG compiles to
		 * nothing below its module's level, and an expression that only
		 * runs when logging is on is a release build with no
		 * acknowledged data in it. */
		rc = radiant_transfer_on_data(&api_xfer[ch], &f, evt->t_sync);
		LOG_DBG("on_data rc=%d", rc);
		break;
	case RADIANT_MSG_BURST_ACK:
	case RADIANT_MSG_TRANSFER_ACK:
	case RADIANT_MSG_UNKNOWN:
	default:
		/* An acknowledgement with no transfer of ours behind it, or a
		 * flag combination nothing has measured. The frame layer names
		 * it and declines to interpret it, and so does this. */
		break;
	}
}

static void api_sched_rx(uint8_t ch, uint8_t filter_index,
			 const struct radiant_rx_event *evt, void *user)
{
	(void)user;

	if (!api_ch_valid(ch) || evt == NULL) {
		return;
	}

	if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_SEARCH) {
		/*
		 * radiant_search_on_rx_indexed(), NEVER radiant_search_on_rx().
		 *
		 * A merged window's evt->filter_index indexes the MERGED array;
		 * the scheduler has already remapped it into the requesting
		 * channel's own filters and hands the remapped value over
		 * beside the event. Passing evt->filter_index to the plain form
		 * would recover a real device number that is not the one that
		 * transmitted - silently, and plausibly.
		 */
		radiant_search_on_rx_indexed(&api_search, evt, filter_index);
		return;
	}

	if (evt->status != RADIANT_RADIO_STATUS_OK) {
		/* CRC_FAIL. With eight filters armed the bench sees about 1.4 a
		 * second on a quiet room; it is the noise floor and never
		 * evidence of anything. */
		return;
	}

	/*
	 * Ownership, before acting on a non-terminal event.
	 *
	 * radiant_channel.h asks for radiant_channel_op_owner(evt->op). evt->op is the
	 * HAL's id and radiant_channel.c holds the surrogate this file minted, so
	 * the question is asked in the only form that means anything here: is
	 * the request this event belongs to still the one this channel owns?
	 * See the report - the substitution is forced by radiant_sched.c sitting
	 * between the HAL and radiant_channel.c, and it is checked rather than
	 * assumed for the same reason the original was.
	 */
	if (radiant_channel_op_owner(api_ch[ch].op) != (int)ch) {
		/*
		 * One of the two ways a frame reaches this file and produces no
		 * "frame ch=..." line at all. Without this, a channel that hears
		 * its peer perfectly and drops it here is indistinguishable from
		 * a window that was never armed, which is the difference between
		 * a scheduler bug and an ownership bug.
		 */
		LOG_DBG("rx drop ch=%u not-owner op=%d owner=%d", (unsigned)ch,
			(int)api_ch[ch].op,
			radiant_channel_op_owner(api_ch[ch].op));
		return;
	}

	api_ch[ch].slot_heard = true;

	if (!radiant_transfer_is_idle(&api_xfer[ch])) {
		/*
		 * The other way. A frame routed into a non-idle transfer never
		 * reaches api_tracked_frame(), so a wedged engine looks exactly
		 * like silence from up here - which is the whole of the "no line
		 * at all" row in the discriminator table.
		 */
		LOG_DBG("rx to xfer ch=%u xfer=%d", (unsigned)ch,
			(int)radiant_transfer_state(&api_xfer[ch]));
		radiant_transfer_on_rx_event(&api_xfer[ch], evt);
		return;
	}

	api_tracked_frame(ch, evt);
}

static void api_sched_tx(uint8_t ch, const struct radiant_tx_event *evt, void *user)
{
	(void)user;

	if (!api_ch_valid(ch) || evt == NULL) {
		return;
	}

	if (!radiant_transfer_is_idle(&api_xfer[ch])) {
		radiant_transfer_on_tx_event(&api_xfer[ch], evt);
		return;
	}

	/* Our own broadcast went out. EVENT_TX is the outcome and there is no
	 * failure event, because broadcast is unacknowledged by design and
	 * there is nothing to fail. */
	api_ch[ch].slot_heard = true;
	api_ch[ch].bcast_pending = false;
	radiant_channel_on_slot(ch, evt->t_sync);
	(void)radiant_event_post_channel_event(ch, (uint8_t)ANTW_EVENT_TX);

	/*
	 * The turnaround is REQUESTED here and POSTED from api_sched_done(), and
	 * the split is the whole of the H3 fix - see api_chan::turn_pending.
	 *
	 * Requested here because of the one number it needs: the window is placed
	 * against the t_sync the transmit actually achieved - a hardware capture
	 * of RADIO's own ADDRESS event - and not against the t_sync that was
	 * requested. A pump running in thread context a few milliseconds later
	 * has neither the instant nor the deadline; the reply is already 2.19 ms
	 * away when this callback runs.
	 *
	 * Posted from the completion because posting from HERE reuses a slot the
	 * scheduler still holds in flight, and that is what silently ate this
	 * transmit's own done(). The completion is the same interrupt, one
	 * callback later, and radiant_sched.c commits it on the way out of the
	 * same dispatch - so nothing here calls tick and no jitter is added.
	 *
	 * api_master_listens() is asked here rather than there only because it is
	 * the same question either way; the TRACKING check moves to the post,
	 * where it is asked AFTER radiant_channel_on_terminal() has had its say
	 * and is therefore the accurate one.
	 */
	if (api_master_listens(ch)) {
		api_ch[ch].turn_pending = true;
		api_ch[ch].turn_t_sync = evt->t_sync;
	}
}

/*
 * The scheduler consumed a request. Feed the transfer engine the terminal event
 * it is waiting for, because the scheduler swallowed the HAL's.
 */
static void api_feed_xfer_terminal(uint8_t ch, enum radiant_radio_status st)
{
	switch (radiant_transfer_state(&api_xfer[ch])) {
	case RADIANT_TRANSFER_STATE_TX_DATA:
	case RADIANT_TRANSFER_STATE_TX_REPLY:
	case RADIANT_TRANSFER_STATE_ABORTING: {
		struct radiant_tx_event e;

		memset(&e, 0, sizeof(e));
		e.op = RADIANT_TRANSFER_OP_EXTERNAL;
		e.status = st;
		e.t_sync = radiant_radio_now();
		e.t_sync_exact = false;
		radiant_transfer_on_tx_event(&api_xfer[ch], &e);
		break;
	}
	case RADIANT_TRANSFER_STATE_WAIT_ACK: {
		struct radiant_rx_event e;

		memset(&e, 0, sizeof(e));
		e.op = RADIANT_TRANSFER_OP_EXTERNAL;
		/*
		 * A window that closed normally with nothing in it is exactly
		 * RADIANT_RADIO_STATUS_TIMEOUT, which the engine reads as
		 * RADIANT_TRANSFER_FAIL_NO_ACK and does NOT retry - what a data
		 * sender does when an acknowledgement goes missing was never
		 * captured, and inventing a retry would put an unmeasured frame
		 * on the air at an unmeasured instant.
		 */
		e.status = st;
		e.t_sync = radiant_radio_now();
		radiant_transfer_on_rx_event(&api_xfer[ch], &e);
		break;
	}
	case RADIANT_TRANSFER_STATE_IDLE:
	case RADIANT_TRANSFER_STATE_WAIT_BLOCK:
	default:
		break;
	}
}

static void api_sched_done(uint8_t ch, enum radiant_sched_done why, void *user)
{
	enum radiant_radio_status st;
	radiant_time_t            now;
	uint32_t              op;

	(void)user;
	if (!api_ch_valid(ch)) {
		return;
	}

	now = radiant_radio_now();
	st = api_done_to_status(why);

	api_stats.sched_dones++;
	if (why == RADIANT_SCHED_DONE_MISSED) {
		api_stats.sched_missed++;
	} else if (why == RADIANT_SCHED_DONE_FAILED) {
		api_stats.sched_failed++;
	}

	if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_SEARCH) {
		/*
		 * DONE_OK credits the whole window. DONE_ABORTED means a nearer
		 * deadline - a tracked channel - took the radio partway through,
		 * so credit what was actually listened to up to now: that is the
		 * normal case whenever anything is tracking, and crediting it
		 * zero is what froze the sweep on one set and made every device
		 * outside that set undiscoverable. MISSED and FAILED never
		 * opened a window and credit nothing.
		 */
		radiant_search_on_done(&api_search, why == RADIANT_SCHED_DONE_OK,
				   why == RADIANT_SCHED_DONE_ABORTED, now);
		if (api_search_slot == ch) {
			api_search_slot = RADIANT_SCHED_CH_NONE;
		}
	} else if (!radiant_transfer_is_idle(&api_xfer[ch]) &&
		   !radiant_sched_pending(ch)) {
		/*
		 * The engine is mid-transfer and has not re-posted, so this
		 * really is the end of its operation. If it HAS re-posted -
		 * which is the normal path, because it arms the reply window
		 * from inside the transmit callback - the new request is what
		 * the slot holds and this completion belongs to the old one.
		 */
		api_feed_xfer_terminal(ch, st);
	} else if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_MASTER_RX) {
		/*
		 * A master's turnaround window closing, and there is deliberately
		 * NOTHING to do about it - which is why it needs its own arm of
		 * this chain rather than falling through to one of the two below.
		 *
		 * It must not touch the slot clock: the master's phase was
		 * anchored by its own completed transmit a moment ago, and both
		 * radiant_channel_on_slot_missed() and _on_slot() would move it.
		 * It must not raise ANTW_EVENT_RX_FAIL either: an empty
		 * turnaround is what four slots a second look like when no slave
		 * has anything to say, and reporting each one would flood a host
		 * with an error for the normal case.
		 */
	} else if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_MASTER_TX &&
		   !api_ch[ch].slot_heard) {
		/*
		 * A master's slot that did not go out. THE CLOCK MUST MOVE.
		 *
		 * t_next only advances on a completed transmit, so a slot lost
		 * to contention or armed too late leaves it in the past. This
		 * function then signals the event thread, the event thread's
		 * pump re-posts that same dead instant, the scheduler refuses
		 * it without ever reaching the backend, and the refusal
		 * completes synchronously back into here. The result is a hot
		 * loop with no fault, no log line and no host response - the
		 * dongle stops answering while looking perfectly alive. One
		 * missed transmit was enough.
		 *
		 * radiant_channel_on_slot_missed() advances by exactly one
		 * period from the slot that was missed rather than from now,
		 * which is what keeps a master's phase its own; it counts no
		 * misses and never sends a master to search.
		 */
		api_stats.slots_missed++;
		(void)radiant_channel_on_slot_missed(ch, now);
	} else if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_TRACK_RX &&
		   !api_ch[ch].slot_heard) {
		/*
		 * A predicted window that ran and heard nothing. The clock
		 * advances from the slot that was MISSED rather than from now,
		 * so scheduler latency cannot walk the phase away from the
		 * master one late notification at a time; after eight in a row
		 * the channel goes back to search and raises
		 * RX_FAIL_GO_TO_SEARCH itself.
		 */
		api_stats.slots_missed++;
		if (!radiant_channel_on_slot_missed(ch, now)) {
			(void)radiant_event_post_channel_event(
				ch, (uint8_t)ANTW_EVENT_RX_FAIL);
		}
	}

	op = api_ch[ch].op;
	api_ch[ch].op = 0u;
	api_ch[ch].slot_kind = (uint8_t)API_SLOT_NONE;

	/*
	 * EVERY terminal event, including one bound to no channel. Recognising
	 * the late event is the point: the HAL guarantees a cancelled
	 * operation's terminal still arrives, so a close that raced a window
	 * always produces one, and treating it as an anomaly would make every
	 * clean close look like a fault. A CLOSING channel completes here and
	 * raises ANTW_EVENT_CHANNEL_CLOSED.
	 */
	(void)radiant_channel_on_terminal(op, st, now);

	/*
	 * THE ONE EXCEPTION to the rule below, and it is bounded by construction.
	 *
	 * A listening master's turnaround has to be placed against the t_sync its
	 * own transmit achieved, which is a number only api_sched_tx() ever sees;
	 * it also has a 2.19 ms deadline, so it cannot wait for a pump. It is
	 * posted HERE rather than there because doing it there reuses a slot the
	 * scheduler still holds in flight and swallows this very completion - see
	 * api_chan::turn_pending.
	 *
	 * Why this is not the unbounded loop api_arming exists to stop: the flag
	 * is cleared BEFORE the post, and only api_sched_tx() ever sets it, on a
	 * transmit that actually went out. A refused arm re-enters this function
	 * with the flag already false and posts nothing. Depth one, always.
	 *
	 * The TRACKING check is here rather than in api_sched_tx() on purpose: it
	 * is asked after radiant_channel_on_terminal(), so a channel that just
	 * finished closing does not get a turnaround armed behind it.
	 */
	if (api_ch[ch].turn_pending) {
		radiant_time_t t_sync = api_ch[ch].turn_t_sync;

		api_ch[ch].turn_pending = false;
		if (radiant_channel_state_get(ch) == RADIANT_CH_STATE_TRACKING) {
			api_post_master_rx(ch, t_sync);
		}
	}

	/*
	 * Do NOT post anything else from here. This runs in a radio callback
	 * and the scheduler commits on the way out, so posting would be the
	 * intended low-jitter path - but against a backend that refuses every
	 * arm it is also an unbounded loop, and the transfer engine already
	 * covers the one deadline that genuinely cannot wait for a thread.
	 * Everything else is re-posted on the next pass.
	 */
	k_sem_give(&api_event_sem);
}

static const struct radiant_sched_cbs api_sched_cbs = {
	.rx = api_sched_rx,
	.tx = api_sched_tx,
	.done = api_sched_done,
};

/* ---------------------------------------------------------------------------
 * The event thread
 * ---------------------------------------------------------------------------
 */

/*
 * Is this transfer state one the RADIO is supposed to finish?
 *
 * WAIT_BLOCK is not: it is waiting for the host to hand over the next 24-byte
 * block, and the host's own stall timeout is a second per packet. Terminating
 * it here would fail live bursts and would release a block the bridge still
 * owns, which is exactly what B1-B5 forbid. IDLE is not, trivially.
 */
static bool api_xfer_awaits_radio(uint8_t ch)
{
	switch (radiant_transfer_state(&api_xfer[ch])) {
	case RADIANT_TRANSFER_STATE_TX_DATA:
	case RADIANT_TRANSFER_STATE_TX_REPLY:
	case RADIANT_TRANSFER_STATE_WAIT_ACK:
	case RADIANT_TRANSFER_STATE_ABORTING:
		return true;
	case RADIANT_TRANSFER_STATE_IDLE:
	case RADIANT_TRANSFER_STATE_WAIT_BLOCK:
	default:
		return false;
	}
}

/*
 * The two recovery watchdogs. Callers hold api_lock.
 *
 * Neither is a scheduling mechanism and neither should ever fire on a healthy
 * link - api_stats.xfer_watchdogs staying at zero across a bench run is part of
 * what "healthy" means. They exist because both failures they cover are
 * PERMANENT and SILENT: a transfer engine with no completion coming never
 * leaves its state, and radiant_ack.c then refuses every subsequent inbound
 * packet on that channel while api_pump_locked() stops posting its broadcast
 * slot - so the channel simply goes quiet with no event, no error and no log
 * line, for the life of the process.
 */
static void api_watchdogs_locked(radiant_time_t now)
{
	const radiant_time_t limit =
		(radiant_time_t)RADIANT_API_XFER_WATCHDOG_MS * 1000u;
	uint8_t ch;

	for (ch = 0u; ch < API_CHANNELS; ch++) {
		struct api_chan *c = &api_ch[ch];

		/*
		 * H2. Non-idle with nothing armed means no done() is coming,
		 * because api_feed_xfer_terminal() is only reachable from
		 * api_sched_done() and only a completion calls that.
		 */
		if (api_xfer_awaits_radio(ch) && !radiant_sched_pending(ch)) {
			if (!c->xfer_stuck_valid) {
				c->xfer_stuck_valid = true;
				c->xfer_stuck_since = now;
			} else if (now - c->xfer_stuck_since >= limit) {
				LOG_WRN("ch=%u transfer wedged in state %d, "
					"terminating", (unsigned)ch,
					(int)radiant_transfer_state(&api_xfer[ch]));
				api_stats.xfer_watchdogs++;
				c->xfer_stuck_valid = false;
				api_feed_xfer_terminal(
					ch, RADIANT_RADIO_STATUS_FAILED);
			}
		} else {
			c->xfer_stuck_valid = false;
		}

		/*
		 * in_pkts only ever cleared on a packet carrying LAST, so a
		 * multi-packet inbound burst whose final packet is missed left
		 * it non-zero for ever - and every later acknowledged message on
		 * that channel was then reported to the host as MESG_BURST_DATA
		 * with a sequence number counted from a burst that ended
		 * minutes ago. Nothing different goes on the air; it is a
		 * host-facing lie, which is worse rather than better.
		 */
		if (c->in_pkts != 0u && c->in_last_valid &&
		    (now - c->in_last) >= limit) {
			LOG_DBG("ch=%u inbound transfer abandoned after %u pkts",
				(unsigned)ch, (unsigned)c->in_pkts);
			c->in_pkts = 0u;
			c->in_last_valid = false;
		}
	}
}

static void api_housekeep(void)
{
	radiant_time_t now;

	k_mutex_lock(&api_lock, K_FOREVER);
	now = radiant_radio_now();

	/* Search deadlines. A SEARCHING channel past its deadline raises
	 * RX_SEARCH_TIMEOUT and then closes, both, in that order. */
	(void)radiant_channel_tick(now);

	/* Seen-cache expiry. The timeout half cannot fire - radiant_channel.c owns
	 * the deadline; see api_search_timeout(). */
	radiant_search_tick(&api_search, now);

	/* Before the pump, so a transfer this releases gets its channel's
	 * broadcast slot posted again on the same pass rather than one
	 * housekeeping interval later. */
	api_watchdogs_locked(now);

	api_pump_locked();
	api_stats.housekeeps++;
	k_mutex_unlock(&api_lock);
}

static void api_event_thread(void *a, void *b, void *c)
{
	(void)a;
	(void)b;
	(void)c;

	for (;;) {
		uint32_t n;

		/*
		 * The timeout is what drives housekeeping. radiant_sched.c owns no
		 * timer, so a request that lost an arm race has nothing else to
		 * retry it, and radiant_channel_tick() has no other caller.
		 */
		(void)k_sem_take(&api_event_sem, K_MSEC(RADIANT_API_HOUSEKEEP_MS));

		/*
		 * THREAD CONTEXT ONLY, and deliberately outside api_lock: the
		 * drain calls antr_on_message(), which is the bridge's, and
		 * holding a lock across a call into another subsystem is how a
		 * deadlock gets built. Nothing the drain touches is shared with
		 * the pump - radiant_event.c protects its own ring with
		 * radiant_event_crit_enter().
		 */
		n = radiant_event_drain(0u);
		if (n != 0u) {
			api_stats.drains++;
			api_stats.delivered += n;
		}

		api_housekeep();
	}
}

/* ---------------------------------------------------------------------------
 * Initialisation and stack lifecycle
 * ---------------------------------------------------------------------------
 */

static void api_reset_state(void)
{
	uint8_t i;

	memset(api_ch, 0, sizeof(api_ch));
	memset(api_net_addr, 0, sizeof(api_net_addr));
	memset(api_adv_burst, 0, sizeof(api_adv_burst));
	memset(api_sdu_mask, 0, sizeof(api_sdu_mask));
	memset(api_crypto_id, 0, sizeof(api_crypto_id));
	memset(api_crypto_user, 0, sizeof(api_crypto_user));
	memset(api_crypto_key, 0, sizeof(api_crypto_key));

	for (i = 0u; i < API_CHANNELS; i++) {
		memset(&api_xfer[i], 0, sizeof(api_xfer[i]));
	}

	api_search_slot = RADIANT_SCHED_CH_NONE;
}

antr_err_t antr_init(void)
{
	struct radiant_search_cfg scfg;
	int                   rc;

	api_reset_state();
	memset(&api_stats, 0, sizeof(api_stats));

	/*
	 * Order matters and each step is a precondition of the next.
	 *
	 * radiant_event_init() first, and before the radio is enabled: posting
	 * before it returns is RADIANT_EVENT_ESTATE, which mirrors the rule that
	 * antr_on_message() is never called before antr_init() has returned 0.
	 */
	radiant_channel_init();
	radiant_event_init();

	/* The scheduler owns radio EVENTS because it is the only thing that
	 * arms. It deliberately does not own the radio LIFECYCLE, which is
	 * this file's. */
	rc = radiant_sched_init(&api_sched_cbs, NULL);
	if (rc != RADIANT_RADIO_OK_RC) {
		LOG_ERR("radiant_sched_init: %d", rc);
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	rc = radiant_radio_init(radiant_sched_radio_cbs(), NULL);
	if (rc != RADIANT_RADIO_OK_RC) {
		LOG_ERR("radiant_radio_init: %d", rc);
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}
	rc = radiant_radio_enable();
	if (rc != RADIANT_RADIO_OK_RC) {
		LOG_ERR("radiant_radio_enable: %d", rc);
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	api_search_cfg_build(&scfg);
	rc = radiant_search_init(&api_search, &scfg, &api_search_cbs, NULL);
	if (rc != RADIANT_SEARCH_OK) {
		/* RADIANT_SEARCH_ENOTSUP means the backend advertises a wildcard
		 * device-number filter, which struct radiant_rx_filter has no field
		 * to ask for. Failing loudly is the correct response to a
		 * capability the contract cannot express. */
		LOG_ERR("radiant_search_init: %d", rc);
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	api_inited = true;
	(void)k_thread_create(&api_event_thread_data, api_event_stack,
			      K_THREAD_STACK_SIZEOF(api_event_stack),
			      api_event_thread, NULL, NULL, NULL,
			      API_EVENT_PRIORITY, 0, K_NO_WAIT);
	(void)k_thread_name_set(&api_event_thread_data, "radiant_event");

	LOG_INF("radiant_core up: %u channels, %u filters/window, %u search sets",
		(unsigned int)API_CHANNELS,
		(unsigned int)radiant_search_filters_per_window(&api_search),
		(unsigned int)radiant_search_sets(&api_search));

	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_stack_reset(void)
{
	struct radiant_search_cfg scfg;
	uint8_t               i;

	k_mutex_lock(&api_lock, K_FOREVER);

	/*
	 * radiant_radio_abort() BEFORE radiant_channel_reset_all(). radiant_channel.c has
	 * no way to abort an operation, so a reset that tore its state down
	 * first would leave the backend referring to a channel that no longer
	 * exists. radiant_sched_reset() is that abort - it retires the operation
	 * and then aborts it, which is the order the module's header insists on
	 * - and the extra call below makes the ordering visible at the one
	 * place a reader looks for it.
	 */
	radiant_sched_reset();
	(void)radiant_radio_abort();

	for (i = 0u; i < API_CHANNELS; i++) {
		(void)radiant_transfer_abort(&api_xfer[i]);
		(void)radiant_search_end(&api_search, i);
	}

	radiant_channel_reset_all();

	/*
	 * DISCARD the queue rather than draining it. A real stick emits nothing
	 * between the reset command and the startup message, the bridge
	 * discards inbound events for a short window afterwards for exactly
	 * that reason, and draining here would call antr_on_message()
	 * recursively from inside an antr_* call - which
	 * docs/sdk-ant-contract.md forbids outright. Channel-closed events
	 * raised by the reset are queued and delivered; that is the contract,
	 * and the bridge's discard window is what absorbs them.
	 */
	radiant_event_flush();

	api_reset_state();

	api_search_cfg_build(&scfg);
	(void)radiant_search_init(&api_search, &scfg, &api_search_cbs, NULL);

	k_mutex_unlock(&api_lock);

	/* "Returns 0. There is no failure case a host could act on." */
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

/* ---------------------------------------------------------------------------
 * Channel lifecycle
 * ---------------------------------------------------------------------------
 */

antr_err_t antr_channel_assign(uint8_t channel, uint8_t type, uint8_t network,
			       uint8_t ext_assign)
{
	antr_err_t rc;

	k_mutex_lock(&api_lock, K_FOREVER);
	rc = radiant_channel_assign(channel, type, network, ext_assign);
	if (rc == RADIANT_CH_OK) {
		/* Assignment resets every configuration byte to its default, so
		 * the bytes this file holds go with them: a channel reassigned
		 * to a different type must not inherit the last session's
		 * broadcast payload or device ID list. */
		memset(&api_ch[channel], 0, sizeof(api_ch[channel]));
	}
	k_mutex_unlock(&api_lock);

	return rc;
}

antr_err_t antr_channel_unassign(uint8_t channel)
{
	antr_err_t rc;

	k_mutex_lock(&api_lock, K_FOREVER);
	rc = radiant_channel_unassign(channel);
	if (rc == RADIANT_CH_OK) {
		(void)radiant_search_end(&api_search, channel);
		(void)radiant_sched_cancel(channel);
		memset(&api_ch[channel], 0, sizeof(api_ch[channel]));
		memset(&api_xfer[channel], 0, sizeof(api_xfer[channel]));
	}
	k_mutex_unlock(&api_lock);

	return rc;
}

antr_err_t antr_channel_open_with_offset(uint8_t channel, uint16_t offset)
{
	antr_err_t rc;

	k_mutex_lock(&api_lock, K_FOREVER);
	rc = radiant_channel_open(channel, offset, radiant_radio_now());
	if (rc == RADIANT_CH_OK) {
		int trc = api_xfer_setup(channel);

		if (trc != RADIANT_TRANSFER_OK) {
			/*
			 * radiant_transfer_init() refuses a backend that cannot
			 * turn a frame round inside RADIANT_TRANSFER_REPLY_US.
			 * Opening still succeeds - broadcast works on such a
			 * backend and a slave can still track - but
			 * acknowledged data and burst will answer
			 * CHANNEL_IN_WRONG_STATE, and the reason is worth a log
			 * line rather than a mystery months later.
			 */
			LOG_WRN("ch %u: no transfer engine (%d): acknowledged "
				"data and burst unavailable",
				(unsigned int)channel, trc);
		}
		api_pump_locked();
	}
	k_mutex_unlock(&api_lock);

	return rc;
}

antr_err_t antr_channel_close(uint8_t channel)
{
	antr_err_t rc;

	k_mutex_lock(&api_lock, K_FOREVER);
	rc = radiant_channel_close(channel, radiant_radio_now());
	if (rc == RADIANT_CH_OK) {
		uint32_t op = api_ch[channel].op;

		(void)radiant_transfer_abort(&api_xfer[channel]);
		(void)radiant_search_end(&api_search, channel);
		if (api_search_slot == channel) {
			/*
			 * This channel was carrying the sweep and its request
			 * is about to be cancelled silently, so nothing else
			 * will tell radiant_search.c that its window is over.
			 * Nothing is credited, so the same set is listened to
			 * again on the next pass rather than the sweep skipping
			 * a set every time a host closes a channel. The window
			 * is being cancelled rather than cut short by a nearer
			 * deadline, and this path has no end instant to credit
			 * against.
			 */
			radiant_search_on_done(&api_search, false, false, 0u);
			api_search_slot = RADIANT_SCHED_CH_NONE;
		}

		/*
		 * radiant_sched_cancel() is deliberately SILENT for the channel it
		 * cancels - the caller already knows - so no done() arrives and
		 * the terminal event radiant_channel.c is waiting on to complete
		 * the close would never come. Feeding it here is what raises
		 * ANTW_EVENT_CHANNEL_CLOSED exactly once per successful close;
		 * a host library that reopens on that event hangs without it.
		 */
		(void)radiant_sched_cancel(channel);
		api_ch[channel].op = 0u;
		api_ch[channel].slot_kind = (uint8_t)API_SLOT_NONE;
		/* Per-channel state that outlives a slot but must not outlive a
		 * channel: a turnaround requested by the last transmit before
		 * the close, a half-received inbound burst, and both watchdog
		 * clocks. Reopening must not inherit any of them. */
		api_ch[channel].turn_pending = false;
		api_ch[channel].in_pkts = 0u;
		api_ch[channel].in_last_valid = false;
		api_ch[channel].xfer_stuck_valid = false;
		(void)radiant_channel_on_terminal(op, RADIANT_RADIO_STATUS_ABORTED,
					      radiant_radio_now());

		api_pump_locked();
	}
	k_mutex_unlock(&api_lock);

	return rc;
}

/* ---------------------------------------------------------------------------
 * Channel configuration - forwards, and nothing else
 * ---------------------------------------------------------------------------
 */

antr_err_t antr_channel_id_set(uint8_t channel, uint16_t device_number,
			       uint8_t device_type, uint8_t trans_type)
{
	return radiant_channel_id_set(channel, device_number, device_type,
				  trans_type);
}

antr_err_t antr_channel_period_set(uint8_t channel, uint16_t period)
{
	return radiant_channel_period_set(channel, period);
}

antr_err_t antr_channel_radio_freq_set(uint8_t channel, uint8_t freq)
{
	return radiant_channel_rf_freq_set(channel, freq);
}

antr_err_t antr_channel_radio_tx_power_set(uint8_t channel, uint8_t level,
					   uint8_t custom)
{
	return radiant_channel_tx_power_set(channel, level, custom);
}

antr_err_t antr_channel_search_timeout_set(uint8_t channel, uint8_t timeout)
{
	return radiant_channel_search_timeout_set(channel, timeout);
}

antr_err_t antr_channel_low_priority_rx_search_timeout_set(uint8_t channel,
							   uint8_t timeout)
{
	return radiant_channel_lp_search_timeout_set(channel, timeout);
}

antr_err_t antr_channel_radio_crc_mode_set(uint8_t channel, uint8_t mode)
{
	return radiant_channel_crc_mode_set(channel, mode);
}

antr_err_t antr_auto_freq_hop_table_set(uint8_t channel, uint8_t freq0,
					uint8_t freq1, uint8_t freq2)
{
	return radiant_channel_freq_hop_table_set(channel, freq0, freq1, freq2);
}

antr_err_t antr_enhanced_channel_spacing_enable(uint8_t enable)
{
	/*
	 * A device-wide radio property, which is why it takes no channel
	 * number, and one no radiant_core module models. Accepting enable == 0 -
	 * the default state - is honest; accepting a non-zero value would be a
	 * host believing it configured a spacing it did not get.
	 */
	return (enable == 0u) ? (antr_err_t)ANTW_RESPONSE_NO_ERROR
			      : (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
}

/* ---------------------------------------------------------------------------
 * Search behaviour
 * ---------------------------------------------------------------------------
 */

antr_err_t antr_search_waveform_set(uint8_t channel, uint16_t waveform)
{
	return radiant_channel_search_waveform_set(channel, waveform);
}

antr_err_t antr_prox_search_set(uint8_t channel, uint8_t threshold,
				uint8_t custom)
{
	return radiant_channel_prox_search_set(channel, threshold, custom);
}

antr_err_t antr_search_channel_priority_set(uint8_t channel, uint8_t priority)
{
	return radiant_channel_search_priority_set(channel, priority);
}

antr_err_t antr_search_channel_priority_get(uint8_t channel, uint8_t *priority)
{
	return radiant_channel_search_priority_get(channel, priority);
}

antr_err_t antr_active_search_sharing_cycles_set(uint8_t channel,
						 uint8_t cycles)
{
	return radiant_channel_sharing_cycles_set(channel, cycles);
}

antr_err_t antr_active_search_sharing_cycles_get(uint8_t channel,
						 uint8_t *cycles)
{
	return radiant_channel_sharing_cycles_get(channel, cycles);
}

antr_err_t antr_id_list_add(uint8_t channel, const uint8_t *device_id,
			    uint8_t index)
{
	if (!api_ch_valid(channel)) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (device_id == NULL) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (radiant_channel_is_open(channel)) {
		return (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE;
	}
	if (index >= API_ID_LIST_MAX) {
		return (antr_err_t)ANTW_INVALID_LIST_ID;
	}

	/* Wire order: [devnum_lsb, devnum_msb, device_type, trans_type],
	 * copied before this returns. */
	memcpy(api_ch[channel].id_list[index], device_id, API_ID_LIST_BYTES);
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_id_list_config(uint8_t channel, uint8_t size, uint8_t inc_exc)
{
	if (!api_ch_valid(channel)) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (radiant_channel_is_open(channel)) {
		return (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE;
	}
	if (size > API_ID_LIST_MAX) {
		return (antr_err_t)ANTW_INVALID_LIST_ID;
	}

	api_ch[channel].id_list_size = size;
	api_ch[channel].id_list_exclude = (inc_exc != 0u);
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

/* ---------------------------------------------------------------------------
 * Networks and keys - the permanent limitation
 * ---------------------------------------------------------------------------
 */

antr_err_t antr_network_address_set(uint8_t network, const uint8_t *key)
{
	if (network >= API_NETWORKS) {
		return (antr_err_t)ANTW_INVALID_NETWORK_NUMBER;
	}
	if (key == NULL) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	/*
	 * The whole table. The key -> address function is not public and this
	 * project will not recover it - fitting it from samples is the activity
	 * most likely to be characterised as reverse-engineering, and the
	 * clean-room policy rules it out. So: the published ANT+ pair, and
	 * ANTW_INVALID_PARAMETER_PROVIDED for anything else.
	 *
	 * THIS IS THE CORRECT BEHAVIOUR, NOT A TO-DO. Every ANT+ profile, every
	 * fitness app and every shipping sensor uses this key, so the
	 * limitation costs nothing on the path anyone runs. The table may be
	 * extended by OBSERVING RF EMISSIONS from a shipping master, subject to
	 * the licence position being confirmed first - never by fitting the
	 * function.
	 */
	if (memcmp(key, api_ant_plus_key, sizeof(api_ant_plus_key)) != 0) {
		api_stats.key_rejects++;
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	api_net_addr[network][0] = RADIANT_NET_ADDR_ANT_PLUS_0;
	api_net_addr[network][1] = RADIANT_NET_ADDR_ANT_PLUS_1;

	/*
	 * The sweep's address is fixed when radiant_search_init() runs, and
	 * re-initialising the instance would drop every channel already in the
	 * search. Hosts install keys before they open channels, so the common
	 * case is free; a host that installed network 0's key with a search
	 * already running keeps the address the sweep started with, which - the
	 * table having exactly one entry - is the same address either way.
	 */
	if (network == 0u && radiant_search_n_searching(&api_search) == 0u) {
		struct radiant_search_cfg scfg;

		api_search_cfg_build(&scfg);
		(void)radiant_search_init(&api_search, &scfg, &api_search_cbs,
				      NULL);
		api_search_slot = RADIANT_SCHED_CH_NONE;
	}
	k_mutex_unlock(&api_lock);

	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

/* ---------------------------------------------------------------------------
 * Library configuration
 * ---------------------------------------------------------------------------
 */

antr_err_t antr_lib_config_set(uint8_t config)
{
	return radiant_event_lib_config_set(config);
}

antr_err_t antr_lib_config_clear(uint8_t config)
{
	return radiant_event_lib_config_clear(config);
}

antr_err_t antr_lib_config_get(uint8_t *config)
{
	return radiant_event_lib_config_get(config);
}

antr_err_t antr_event_filtering_set(uint16_t filter)
{
	return radiant_event_filter_set(filter);
}

antr_err_t antr_event_filtering_get(uint16_t *filter)
{
	return radiant_event_filter_get(filter);
}

antr_err_t antr_adv_burst_config_set(const uint8_t *config, uint8_t size)
{
	/* 8 is the required part and 11 the whole thing. Anything outside that
	 * is rejected rather than padded: accepting a short block would let a
	 * bridge bug that passed the wrong slice of the message look correct. */
	if (config == NULL || size < 8u || size > sizeof(api_adv_burst)) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	memset(api_adv_burst, 0, sizeof(api_adv_burst));
	memcpy(api_adv_burst, config, size);
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_adv_burst_config_get(uint8_t request_type, uint8_t *config)
{
	if (config == NULL) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	/* Two different lengths, and the caller's buffer is sized off
	 * request_type and nothing else - writing the wrong one is invisible
	 * until a host reads a field shifted by one. */
	if (request_type != 0u) {
		memcpy(config, &api_adv_burst[1], sizeof(api_adv_burst) - 1u);
	} else {
		memset(config, 0,
		       ANTW_MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE);
		config[0] = (uint8_t)ANTW_ADV_BURST_MODES_SIZE_24_BYTES;
		config[1] = (uint8_t)ANTW_ADV_BURST_MODES_FREQ_HOP;
	}
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_sdu_mask_set(uint8_t mask_index, const uint8_t *mask)
{
	if (mask_index >= API_SDU_MASKS || mask == NULL) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}
	memcpy(api_sdu_mask[mask_index], mask, ANTW_ANT_MAX_PAYLOAD_SIZE);
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_sdu_mask_get(uint8_t mask_index, uint8_t *mask)
{
	if (mask_index >= API_SDU_MASKS || mask == NULL) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}
	memcpy(mask, api_sdu_mask[mask_index], ANTW_ANT_MAX_PAYLOAD_SIZE);
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_sdu_mask_config(uint8_t channel, uint8_t mask_config)
{
	/*
	 * Two different codes, one per argument, and the distinction is
	 * observable from the host: an out-of-range channel is
	 * ANTW_INVALID_MESSAGE and an out-of-range mask is
	 * ANTW_INVALID_PARAMETER_PROVIDED. The acknowledged-data bit is masked
	 * off BEFORE the index is range-checked.
	 */
	if (!api_ch_valid(channel)) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (mask_config != (uint8_t)ANTW_INVALID_SDU_MASK &&
	    (uint8_t)(mask_config & (uint8_t)~ANTW_SDU_MASK_ACK_CONFIG_BIT) >=
		    API_SDU_MASKS) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	return radiant_channel_sdu_mask_config_set(channel, mask_config);
}

/* ---------------------------------------------------------------------------
 * Data transfer
 * ---------------------------------------------------------------------------
 */

antr_err_t antr_broadcast_message_tx(uint8_t channel, uint8_t size,
				     const uint8_t *data)
{
	if (!api_ch_valid(channel) || data == NULL ||
	    size != (uint8_t)ANTW_ANT_MAX_PAYLOAD_SIZE) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (!radiant_channel_is_open(channel)) {
		return (antr_err_t)ANTW_CHANNEL_NOT_OPENED;
	}
	/*
	 * A receive-only channel has no transmitter to queue for. The type
	 * lives in the top nibble, so the whole nibble is compared: masking off
	 * only the master bit would make MASTER_TX_ONLY (0x50) read as
	 * SLAVE_RX_ONLY (0x40) and refuse the one channel type that exists to
	 * transmit.
	 */
	if ((uint8_t)(radiant_channel_type_get(channel) &
		      (uint8_t)RADIANT_CH_STATUS_TYPE_MASK) ==
	    (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY) {
		return (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE;
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	if (!radiant_transfer_is_idle(&api_xfer[channel])) {
		k_mutex_unlock(&api_lock);
		return (antr_err_t)ANTW_TRANSFER_IN_PROGRESS;
	}

	/*
	 * Copied before this returns - unlike antr_burst_tx() there is no
	 * ownership transfer. One payload per slot: queueing a second before
	 * the slot fires REPLACES the first rather than sending both, which is
	 * what a master re-broadcasting its last page looks like from here.
	 */
	memcpy(api_ch[channel].bcast, data, ANTW_ANT_MAX_PAYLOAD_SIZE);
	api_ch[channel].bcast_valid = true;
	api_ch[channel].bcast_pending = true;

	api_pump_locked();
	k_mutex_unlock(&api_lock);

	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_acknowledge_message_tx(uint8_t channel, uint8_t size,
				       const uint8_t *data)
{
	antr_err_t err;
	int        rc;

	if (!api_ch_valid(channel) || data == NULL ||
	    size != (uint8_t)RADIANT_TRANSFER_PKT_BYTES) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (!radiant_channel_is_open(channel)) {
		return (antr_err_t)ANTW_CHANNEL_NOT_OPENED;
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	/*
	 * RADIANT_TIME_NEVER: "as soon as the backend can". The engine resolves it
	 * against caps.min_arm_lead_us itself, because a caller that meant "now"
	 * and got a literal instant already gone by would have its frame
	 * refused with RADIANT_RADIO_ETIME.
	 */
	rc = radiant_transfer_ack_data(&api_xfer[channel], data, size,
				   RADIANT_TIME_NEVER);
	err = api_xfer_err(rc);
	if (rc == RADIANT_TRANSFER_OK) {
		api_pump_locked();
	}
	k_mutex_unlock(&api_lock);

	return err;
}

antr_err_t antr_burst_tx(uint8_t channel, uint16_t size, uint8_t *data,
			 uint8_t segment)
{
	antr_err_t err;
	int        rc;

	if (!api_ch_valid(channel) || data == NULL) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (size != 8u && size != 16u && size != 24u) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	/*
	 * Until advanced burst is enabled, reject anything over 8. A dongle
	 * that takes 24-byte blocks and can never send one is worse than one
	 * that refuses them. api_adv_burst[0] is the enable byte.
	 */
	if (size > 8u && api_adv_burst[0] == 0u) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (!radiant_channel_is_open(channel)) {
		return (antr_err_t)ANTW_CHANNEL_NOT_OPENED;
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	/*
	 * The segment byte is FORWARDED UNTOUCHED. RADIANT_TRANSFER_SEG_* were
	 * chosen equal to ANTR_BURST_SEGMENT_* for exactly this, and the
	 * BUILD_ASSERTs at the top of this file are what keep the two equal.
	 *
	 * ON ANY NON-ZERO RETURN THE ENGINE OWNS NOTHING AND RAISES NO EVENT
	 * FOR THIS BLOCK (B2). The bridge releases its semaphore synchronously
	 * on a non-zero return, so raising one here as well would hand back a
	 * semaphore it holds for a different block and the next host packet
	 * would overwrite a buffer the radio is transmitting from (B5).
	 */
	rc = radiant_transfer_burst_block(&api_xfer[channel], data, (uint8_t)size,
				      segment, RADIANT_TIME_NEVER);
	err = api_xfer_err(rc);
	if (rc == RADIANT_TRANSFER_OK) {
		api_pump_locked();
	}
	k_mutex_unlock(&api_lock);

	return err;
}

antr_err_t antr_pending_transmit_get(uint8_t channel, uint8_t *pending)
{
	if (!api_ch_valid(channel) || pending == NULL) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	*pending = api_ch[channel].bcast_pending ? 1u : 0u;
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_pending_transmit_clear(uint8_t channel, uint8_t *success)
{
	if (!api_ch_valid(channel) || success == NULL) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}

	/* The outcome is reported through `success`, not through the return
	 * value: "there was nothing to clear" is not an error, and a host that
	 * asked still gets RESPONSE_NO_ERROR with a zero in the payload. */
	*success = api_ch[channel].bcast_pending ? 1u : 0u;
	api_ch[channel].bcast_pending = false;
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

/* ---------------------------------------------------------------------------
 * Status and queries
 * ---------------------------------------------------------------------------
 */

antr_err_t antr_capabilities_get(uint8_t *capabilities)
{
	if (capabilities == NULL) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	memset(capabilities, 0, ANTW_MESG_CAPABILITIES_SIZE);

	/*
	 * THIRTY-TWO CHANNELS, and that is not a flattering number - it is the
	 * count radiant_channel.c, radiant_sched.c and radiant_search.c are all sized for
	 * from their first line, asserted at the top of this file, and it is
	 * the serial protocol's own ceiling because the burst header carries
	 * the channel number in its low five bits. libant.a is built for eight
	 * here.
	 */
	capabilities[ANTW_CAPABILITIES_OFFSET_MAX_CHANNELS] = API_CHANNELS;
	capabilities[ANTW_CAPABILITIES_OFFSET_MAX_NETWORKS] = API_NETWORKS;
	capabilities[ANTW_CAPABILITIES_OFFSET_STANDARD_OPTIONS] = 0x00u;

	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS] =
		(uint8_t)(ANTW_CAPABILITIES_NETWORK_ENABLED |
			  ANTW_CAPABILITIES_SERIAL_NUMBER_ENABLED |
			  ANTW_CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED |
			  ANTW_CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED |
			  /* The device ID list really is honoured - it is
			   * consulted at acquisition, in api_id_list_permits() -
			   * so the bit is set. Advertising a list that was
			   * stored and never read would be worse than not
			   * having one. */
			  ANTW_CAPABILITIES_SEARCH_LIST_ENABLED);

	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_2] =
		(uint8_t)(ANTW_CAPABILITIES_EXT_MESSAGE_ENABLED |
			  /* BACKGROUND SCAN. radiant_search.c implements scan mode
			   * as a first-class part of the sweep; libant.a
			   * advertises the mode and never implemented it. This
			   * is one of the two capabilities a core build has and
			   * an sdk_ant build does not. */
			  ANTW_CAPABILITIES_SCAN_MODE_ENABLED |
			  ANTW_CAPABILITIES_EXT_ASSIGN_ENABLED);

	capabilities[ANTW_CAPABILITIES_OFFSET_MAX_SENSRCORE_CHANNELS] = 0x00u;

	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_3] =
		(uint8_t)(ANTW_CAPABILITIES_ADVANCED_BURST_ENABLED |
			  ANTW_CAPABILITIES_EVENT_FILTERING_ENABLED |
			  ANTW_CAPABILITIES_SEARCH_SHARING_ENABLED |
			  ANTW_CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED |
			  /* The bit describes what the radio layer can do
			   * rather than what this backend chose to implement,
			   * and it is what src/ant_radio_stub.c reports too, so
			   * an A/B diff does not turn on it. What radiant_core
			   * actually does with ANT+ AES-CTR is refuse it - see
			   * antr_crypto_channel_enable(). */
			  ANTW_CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED);

	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_4] = 0x00u;

	/* antr_channel_open_with_offset() is the real entry point here, not a
	 * convenience over one, so the offset really is honoured. */
	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_5] =
		(uint8_t)ANTW_CAPABILITIES_CHANNEL_START_OFFSET_ENABLED;

	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

/*
 * Identifiable from the host side with no debugger, and deliberately not a
 * string claiming to be an AVN build it is not. The stub reports "STUB0.01B00"
 * for the same reason.
 */
static const char api_version[] = "RADIANT0.01B00";
BUILD_ASSERT(sizeof(api_version) <= ANTW_MESG_VERSION_SIZE,
	     "version string does not fit MESG_VERSION_ID");

antr_err_t antr_version_get(uint8_t *version)
{
	if (version == NULL) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	memset(version, 0, ANTW_MESG_VERSION_SIZE);
	memcpy(version, api_version, sizeof(api_version));
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_status_get(uint8_t channel, uint8_t *status)
{
	return radiant_channel_status_get(channel, status);
}

antr_err_t antr_channel_id_get(uint8_t channel, uint16_t *device_number,
			       uint8_t *device_type, uint8_t *trans_type)
{
	struct radiant_channel_id id;
	antr_err_t            rc;

	if (device_number == NULL || device_type == NULL ||
	    trans_type == NULL) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}

	memset(&id, 0, sizeof(id));
	rc = radiant_channel_id_get(channel, &id);
	if (rc != RADIANT_CH_OK) {
		return rc;
	}

	*device_number = id.device_number;
	*device_type = id.device_type;
	*trans_type = id.trans_type;
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_channel_period_get(uint8_t channel, uint16_t *period)
{
	return radiant_channel_period_get(channel, period);
}

antr_err_t antr_channel_radio_freq_get(uint8_t channel, uint8_t *freq)
{
	return radiant_channel_rf_freq_get(channel, freq);
}

antr_err_t antr_channel_radio_crc_mode_get(uint8_t channel, uint8_t *mode)
{
	return radiant_channel_crc_mode_get(channel, mode);
}

/* ---------------------------------------------------------------------------
 * Encryption
 *
 * The four exist because a backend stands in for libant.a, which exports them
 * regardless of which the bridge chooses to call. What they do is the honest
 * implementation src/ant_radio.h asks for: accept the reads, hold the writes,
 * and REFUSE to put a channel into an encrypted mode. No Windows host can reach
 * ANT+'s AES-CTR at all (ANT_DLL.dll exports no encryption call), the mode is
 * malleable and unauthenticated, and it reserves RAM shared with the plain
 * channels. What replaces it is docs/radiant-security.md and is not part of
 * this contract.
 * ---------------------------------------------------------------------------
 */

antr_err_t antr_crypto_channel_enable(uint8_t channel, uint8_t enable,
				      uint8_t key_num, uint8_t decimation)
{
	(void)decimation;

	if (!api_ch_valid(channel) || key_num >= API_CRYPTO_KEYS) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}
	if (enable != 0u) {
		/* Not "unimplemented": declined. See the block comment. */
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	api_ch[channel].crypto_mode = 0u;
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_crypto_key_set(uint8_t key_num, const uint8_t *key)
{
	if (key_num >= API_CRYPTO_KEYS || key == NULL) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}
	memcpy(api_crypto_key[key_num], key, ANTW_ENCRYPTION_KEY_SIZE);
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_crypto_info_set(uint8_t type, const uint8_t *info)
{
	if (info == NULL) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	/* There is no length parameter, so the callee reads a fixed count per
	 * type and the CALLER bounds it. The bridge does exactly that. */
	switch (type) {
	case ANTW_ENCRYPTION_INFO_SET_CRYPTO_ID:
		memcpy(api_crypto_id, info, sizeof(api_crypto_id));
		break;
	case ANTW_ENCRYPTION_INFO_SET_CUSTOM_USER_DATA:
		memcpy(api_crypto_user, info, sizeof(api_crypto_user));
		break;
	case ANTW_ENCRYPTION_INFO_SET_RNG_SEED:
		/* Accepted and dropped. The bridge refuses to send this one
		 * because no size is defined for it anywhere. */
		break;
	default:
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_crypto_info_get(uint8_t type, uint8_t *info)
{
	if (info == NULL) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	/* Write exactly what the type accounts for and no more: the caller's
	 * buffer is sized for the largest reply INCLUDING the leading
	 * info-type byte the bridge supplies itself, so zeroing the whole thing
	 * overruns it. */
	switch (type) {
	case ANTW_ENCRYPTION_INFO_GET_SUPPORTED_MODE:
		info[0] = (uint8_t)ANTW_MAX_SUPPORTED_ENCRYPTION_MODE;
		break;
	case ANTW_ENCRYPTION_INFO_GET_CRYPTO_ID:
		memcpy(info, api_crypto_id, sizeof(api_crypto_id));
		break;
	case ANTW_ENCRYPTION_INFO_GET_CUSTOM_USER_DATA:
		memcpy(info, api_crypto_user, sizeof(api_crypto_user));
		break;
	default:
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

/* ---------------------------------------------------------------------------
 * Inspection
 * ---------------------------------------------------------------------------
 */

const struct radiant_api_stats *radiant_api_stats_get(void)
{
	return &api_stats;
}

bool radiant_api_ready(void)
{
	return api_inited;
}
