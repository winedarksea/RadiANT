/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ant_radio_radiant.c (was radiant/src/radiant_api.c, before radiant
 * stopped reaching outside itself for src/ant_radio.h) - integration layer:
 * src/ant_radio.h's antr_* entry points on top of the six radiant
 * modules, plus the seams none of them own.
 *
 * Provenance: clean-room, from src/ant_radio.h, docs/sdk-ant-contract.md,
 * src/ant_wire.h (generated from protocol/ant_wire.yaml), and the radiant
 * module headers. Nothing here derives from sdk-ant, libant.a, disassembly, or
 * any ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * Living outside radiant now, not inside it: this is the one file that
 * was always shaped by the seam above the module rather than by the module
 * itself - see CONFIG_RADIANT_ANTR_API in src/Kconfig.antr_api for why.
 * radiant_channel.h's own wire-code cross-check no longer depends on include
 * order the way it used to: it includes radiant's own radiant_wire.h
 * unconditionally now, rather than gating on whichever translation unit
 * happened to bring src/ant_wire.h into scope first (this one, always).
 *
 * WHAT THIS FILE OWNS: the key -> address table (no ant_net.c; see below), the
 * SDU mask table, the advanced-burst config block and encryption slots (no
 * module models these), the per-channel device ID list (radiant_search.c has
 * none), one broadcast payload per channel, the event thread, and the
 * surrogate operation ids that let radiant_channel.c attribute a terminal
 * event to a channel when radiant_sched.c sits between it and the HAL.
 * Everything else is a forward to channel/sched/search/event/transfer.
 *
 * POSTING IS NOT COMMITTING: radiant_sched_request_rx()/_tx() change the plan;
 * radiant_sched_tick() acts on it. Thread context posts everything then ticks
 * ONCE at the end of pump() - arming on every post would let the first
 * channel's window commit before an overlapping second channel had a say, so
 * merged receive windows would never merge. Callbacks never call tick; the
 * scheduler commits on its own way out. radiant_sched.c owns no timer, so
 * RADIANT_API_HOUSEKEEP_MS bounds how long a request that lost an arm race
 * (EBUSY) or predates radio-enable (ESTATE) can sit unretried.
 *
 * radiant_event_drain() IS THREAD CONTEXT ONLY: the drain must never be
 * re-entered (docs/sdk-ant-contract.md forbids antr_on_message() recursing
 * from inside a bridge call), so it never runs inside an antr_* function - not
 * even antr_stack_reset(), which flushes instead of draining. It runs on this
 * file's own thread, woken by radiant_event_wakeup().
 *
 * THE PERMANENT LIMITATION: key -> address. There is no ant_net.c and never
 * will be - the derivation is not public and fitting it from samples is
 * reverse-engineering, which the clean-room policy rules out. The one-entry
 * table below (the published ANT+ pair) is the whole of it; anything else is
 * ANTW_INVALID_PARAMETER_PROVIDED. Every ANT+ profile and shipping sensor uses
 * that one key, so this costs nothing in practice. For A/B work: libant.a
 * computes the address, so a comparison must use the ANT+ key or it compares a
 * table against an algorithm.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

/* First, and see the note above. */
#include "ant_wire.h"
#include "ant_health.h"
#include "ant_radio.h"

#include "ant_radio_radiant.h"
#include <radiant/radiant_channel.h>
#include <radiant/radiant_crc_repair.h>
#include <radiant/radiant_event.h>
#include <radiant/radiant_frame.h>
#include <radiant/radiant_noise.h>
#include <radiant/radiant_profile_sanity.h>
#include <radiant/radiant_radio_hal.h>
#include <radiant/radiant_sched.h>
#if defined(CONFIG_RADIANT_SWI)
#include <radiant/radiant_swi.h>
#endif
#include <radiant/radiant_search.h>
#include <radiant/radiant_sec.h>
#include <radiant/radiant_transfer.h>

/* CONFIG_RADIANT_LOG_LEVEL, not a hardcoded LOG_LEVEL_INF, so this module
 * can be turned up when it is the one misbehaving. */
LOG_MODULE_REGISTER(radiant_api, CONFIG_RADIANT_LOG_LEVEL);

/*
 * RADIANT_TRANSFER_SEG_* were chosen equal to ANTR_BURST_SEGMENT_* so
 * antr_burst_tx() forwards the byte untouched; radiant_transfer.h can't check
 * this itself (compiles with no app header in scope), so the assert lives
 * here, the one TU with both headers.
 *
 * Already wrong once: ANTR_BURST_SEGMENT_END was assumed 0x04 until compared
 * against sdk-ant's 0x02. Forwarding the wrong value marks every burst's last
 * block as a middle block, so TRANSFER_TX_COMPLETED never arrives and each
 * transfer times out at 1000 ms - a two-second burst takes ten minutes.
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

/* Selective-data-update masks: four, matching src/ant_radio_stub.c and what
 * tools/ant_features.py round-trips against. */
#define API_SDU_MASKS 4u

/* Encryption key slots: same count as the stub, so an A/B diff doesn't flag it. */
#define API_CRYPTO_KEYS 4u
#define API_CRYPTO_ID_SIZE                                                    \
	(ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE - ANTW_CHANNEL_NUM_SIZE)

/* Device IDs one channel's inclusion/exclusion list may hold: four, what a
 * retail stick accepts; consulted once per acquisition so a linear scan is
 * free. Beyond this is ANTW_INVALID_LIST_ID. */
#define API_ID_LIST_MAX 4u
#define API_ID_LIST_BYTES 4u /* [devnum_lo][devnum_hi][dtype][ttype] */

/*
 * Half-width of a tracked channel's receive window, in microseconds.
 *
 * NO LONGER THE NUMBER USED - radiant_channel_guard_us() is (adaptive; see its
 * declaration). This constant is the spec's worst case over eight misses and
 * survives only as the ceiling and the answer when the estimator has nothing
 * to say (not tracking, just acquired, just sent back to search).
 */
#define API_SLOT_GUARD_US RADIANT_CHANNEL_GUARD_MAX_US

/*
 * Two invariants the guard rests on, checked not just claimed: (1) if
 * RX_FAIL_TO_SEARCH grows or the ceiling shrinks, a channel could silently
 * lose its window on the last miss before falling back to search; (2) the
 * adaptive guard's floor must cover one period of worst-case drift, or the
 * narrow path's per-miss charging leaves it weaker than the wide guard it
 * replaced.
 */
BUILD_ASSERT(RADIANT_CHANNEL_GUARD_MAX_US >=
	     (uint32_t)RADIANT_CHANNEL_RX_FAIL_TO_SEARCH *
		     RADIANT_CHANNEL_DRIFT_WORST_US,
	     "the tracked-channel guard ceiling no longer covers worst-case LF "
	     "clock drift across every consecutive miss up to RX_FAIL_TO_SEARCH");
BUILD_ASSERT(RADIANT_CHANNEL_GUARD_MIN_US >= RADIANT_CHANNEL_DRIFT_WORST_US,
	     "the adaptive guard's floor no longer covers one period of "
	     "worst-case LF clock drift, so a narrowed window is weaker than "
	     "the wide one it replaced");

/*
 * Extended assignment bit for background scanning. Not in src/ant_wire.h -
 * protocol/ant_wire.yaml never named the extended-assignment bits, since
 * nothing acted on one until now. 0x01 is ANT Message Protocol and Usage
 * Rev 5.1's value for background scanning enable; kept here with an
 * unmistakable prefix rather than faked as generated.
 */
#define API_EXT_ASSIGN_BACKGROUND_SCAN 0x01u

/* Noise-floor log interval: 60 s is enough windows for a 10th percentile to
 * mean something, short enough to watch live while testing a USB port. */
#define API_NOISE_REPORT_US 60000000u

/*
 * Receive-path log interval. Shorter than the noise line's sixty seconds
 * because this one exists to give a RATE across a bench run that lasts tens of
 * seconds, and at 60 s a whole run produces a single line. Not shorter than
 * this: apps with a console run LOG_MODE_DEFERRED over a small buffer, and a
 * chatty periodic line there discards other messages rather than its own (see
 * the dropped boot banner in docs/treadmill-reference-design.md). Lower it to
 * ~2 s by hand when actually chasing something on a bench board.
 */
#define API_RXPATH_REPORT_US 30000000u

#if defined(CONFIG_RADIANT_CRC_REPAIR)
/* CRC-repair counter log interval. 10 s rather than the noise line's 60 s: the
 * S3 sensitivity ladder holds each rung for well under a minute, and a counter
 * that reports once per 60 s can produce a whole rung with no line in it - which
 * reads identically to a rung where repair never engaged. */
#define API_CRC_REPORT_US 10000000u
#endif

/*
 * The event thread. 1280 B covers radiant_event_drain()'s frame plus pump()'s
 * slot-table arithmetic.
 *
 * PRIORITY 6, BELOW THE BRIDGE'S PARSER THREAD (5) - THE INEQUALITY IS THE
 * POINT. At priority 4 (above the parser) a captured bench sequence showed
 * EVENT_CHANNEL_CLOSED delivered before the RESPONSE to the close command that
 * caused it: antr_channel_close() completes inline on the parser thread, and
 * this thread, higher priority, preempted it before send_response() ran. Host
 * libraries read until they see their command's response and discard what
 * they pass, so the event was silently eaten, and the next unassign was
 * refused CHANNEL_IN_WRONG_STATE. Below the parser the ordering is structural:
 * a lower-priority thread can't run until the parser blocks on
 * k_sem_take(&ant_rx_sem), which only happens after it has dispatched and
 * replied. KEEP THIS GREATER THAN BRIDGE_PRIORITY (src/ant_serial_bridge.c) -
 * the BUILD_ASSERT below enforces it.
 */
/*
 * +256 B when security transforms are compiled in: radiant_sec_pump()'s
 * deepest frame is a CMAC final (two 16-byte subkeys, block, tag under a
 * 16-byte nonce). Generous rather than exact - on this platform a stack
 * overrun surfaces as a wrong answer, not a fault, which is the worst failure
 * mode for a path deciding packet authenticity.
 */
/*
 * +1024 B more under an arbitrated backend: the pump's deepest call
 * (post -> radiant_sched_tick() -> arm_rx_window() -> radiant_radio_rx())
 * continues into gate_acquire() and MPSL under the MPSL gate, versus a few
 * register writes on the direct backend. Measured: at 1280 the board faulted
 * "Stack overflow on CPU 0" in thread `radiant_event` on channel open (looked
 * like a wedge, was a fast reboot loop, since deferred logging needs a live
 * thread to flush). CONFIG_THREAD_NAME + CONFIG_LOG_MODE_IMMEDIATE is what
 * named the faulting thread.
 */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
#define API_EVENT_STACK_GATE 1024
#elif defined(CONFIG_RADIANT_BACKEND_CC26XX)
/*
 * Same reason, second vendor: this thread's arm call costs whatever the
 * backend's arm costs, and CC26x2's RF_postCmd() (SimpleLink driver call,
 * queue walk, power-state negotiation, cross-processor doorbell) is far
 * deeper than the nRF's register writes. Measured the same way: first window
 * on this backend reset inside the completion path; the faulting thread
 * pointer (0x200025e8, "(unknown)" for lack of CONFIG_THREAD_NAME) resolved
 * via `nm -n -S zephyr.elf` to api_event_thread_data.
 */
#define API_EVENT_STACK_GATE 1024
#else
#define API_EVENT_STACK_GATE 0
#endif

#if defined(CONFIG_RADIANT_SEC)
#define API_EVENT_STACK_SIZE (1280 + 256 + API_EVENT_STACK_GATE)
#else
#define API_EVENT_STACK_SIZE (1280 + API_EVENT_STACK_GATE)
#endif
#define API_EVENT_PRIORITY   6

BUILD_ASSERT(API_EVENT_PRIORITY > ANTR_HOST_THREAD_PRIORITY,
	     "the radiant event thread must be LOWER priority than the "
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
	 * The surrogate operation id bound into radiant_channel.c. It wants the
	 * id radiant_radio_tx()/rx() returned so a late terminal event is
	 * recognisable, but radiant_sched.c merges channels into one hardware
	 * operation with no such id to hand over - so this file mints its own
	 * non-zero token per accepted post, opaque, used only for attribution.
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

	/* The channel's broadcast payload: what antr_broadcast_message_tx()
	 * queues, and what an acknowledgement carries - every acknowledgement
	 * ever captured carried the acknowledger's own broadcast buffer
	 * (docs/spike-b-part2-results.md). */
	uint8_t bcast[ANTW_ANT_MAX_PAYLOAD_SIZE];
	bool    bcast_valid;
	bool    bcast_pending;

#if defined(CONFIG_RADIANT_SEC)
	/*
	 * The current slot's transformed payload. Fixes a plaintext leak:
	 * api_xfer_broadcast() would otherwise send `bcast` (host plaintext) as
	 * an acknowledgement, handing a listener the plaintext of ciphertext it
	 * just heard. Returning the ciphertext already built for this counter
	 * fixes it with no second nonce. `bcast` itself is never mutated -
	 * antr_broadcast_message_tx()'s readback must return what was written.
	 */
	uint8_t sec_pay[ANTW_ANT_MAX_PAYLOAD_SIZE];
	bool    sec_pay_valid;
#endif

	uint8_t id_list[API_ID_LIST_MAX][API_ID_LIST_BYTES];
	uint8_t id_list_size;
	bool    id_list_exclude;

	uint8_t crypto_mode;

	/* On-air packets of the inbound transfer so far. Acknowledged data is
	 * byte-for-byte a one-packet burst, so this is how the serial layer
	 * tells them apart: one packet ending LAST is the former, anything
	 * longer is the latter. */
	uint8_t in_pkts;

	/*
	 * The master turnaround, deferred from api_sched_tx() to
	 * api_sched_done(). Posting it straight from the TX callback used to
	 * reuse the slot the scheduler still held in flight, which silently ate
	 * that same transmit's own done() (radiant_sched_request_rx() aborts the
	 * still-armed slot, making the TX's end_armed() a no-op) - leaving
	 * CLOSING channels stuck and radiant_radio_abort() firing on a live TX
	 * ISR 4x/s. Deferring one callback costs nothing: same interrupt, t_sync
	 * (the hardware ADDRESS-event capture) carried rather than recomputed,
	 * and radiant_sched.c still commits on the way out of the same dispatch.
	 */
	bool           turn_pending;

	/* A tracked RX window is armed, so this channel owes the scheduler its
	 * next one the moment it completes. Set at arm, cleared before the post,
	 * bounding the re-post to depth one against a backend that refuses every
	 * arm (see api_sched_done()). Named for its first use; it now also
	 * carries a master's next TRANSMIT slot after a MISSED/DENIED
	 * MASTER_TX, which is the same fact about the same slot in the other
	 * direction. */
	bool           track_repost;
	radiant_time_t turn_t_sync;

	/*
	 * Watchdog state (RADIANT_API_XFER_WATCHDOG_MS, api_housekeep()).
	 * xfer_stuck_since: when the transfer engine was first seen non-idle
	 * with nothing armed. in_last: last inbound packet's arrival. Both carry
	 * a separate validity flag since zero is a legal radiant_radio_now()
	 * reading.
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

/* Which entries of api_net_addr[] were actually installed by a host.
 *
 * An all-zero row is indistinguishable from a real address by inspection, and
 * that ambiguity was a silent bug rather than a theoretical one. Zwift sets
 * three network keys at startup and cycles channel 0 across networks 0, 1 and
 * 2 (measured - archive/host-api/2026-08-10-zwift-session-decoded.txt). Only
 * the ANT+ key is accepted here, so networks 1 and 2 kept all-zero addresses
 * while remaining perfectly *in range*: api_net_addr_for() only clamps
 * out-of-range numbers, so an assign to network 1 succeeded, the channel was
 * admitted to the sweep (which sweeps api_net_addr[0] unconditionally),
 * acquired, then tracked on a zero address and heard nothing at all. Two
 * thirds of the host's discovery attempts went into that hole with no error
 * anywhere - which is what made ANT+ discovery look roughly three times
 * slower than it is.
 *
 * A refusal at assign time is the same class of change as the nRF54L
 * synthesized-clock BUILD_ASSERT: convert a silence into a diagnosable error,
 * because the silence gets debugged forever and the error gets read once.
 */
static bool api_net_valid[API_NETWORKS];

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

/*
 * Whether this build repairs single-bit CRC errors, and whether the backend
 * supports it. Resolved once by api_crc_repair_setup() at antr_init() (all
 * three conditions - Kconfig, syndrome table validity, backend CRC support -
 * are fixed for the image's life). Read in two places that must agree:
 * api_post_track_rx() asks the backend to deliver CRC failures, api_sched_rx()
 * acts on them.
 */
static bool api_crc_repair_on;

/* Whether a repaired frame is also checked against radiant_profile_sanity.c
 * before delivery. Resolved alongside api_crc_repair_on; can only be true
 * when that is, since Kconfig makes PROFILE_SANITY depend on CRC_REPAIR. */
static bool api_profile_sanity_on;

static struct radiant_api_stats api_stats;

/*
 * One mutex, held across every thread-context scheduling pass. The bridge's
 * parser thread and this file's event thread both post requests and call
 * radiant_sched_tick(); without this they'd interleave inside a module built
 * for one caller at a time. Taken only in thread context, around bounded
 * arithmetic - never around a drain, never where an interrupt could want it.
 *
 * Does NOT cover a radio callback's own scheduling pass (interrupt context,
 * can't take a mutex) - that serialisation lives in radiant_sched.c, which
 * saves/restores `in_pass` and runs slot-table writes with interrupts off.
 */
K_MUTEX_DEFINE(api_lock);

/* Given once per queued message (possibly from the radio ISR), and once per
 * housekeeping interval by the take's timeout. */
static K_SEM_DEFINE(api_event_sem, 0, 1);

/*
 * Created in antr_init() rather than K_THREAD_DEFINE: the drain calls
 * antr_on_message(), which must never run before antr_init() returns 0, and a
 * statically started thread would be runnable before src/main.c calls us.
 * Same shape src/ant_serial_bridge.c uses for its own thread.
 */
K_THREAD_STACK_DEFINE(api_event_stack, API_EVENT_STACK_SIZE);
static struct k_thread api_event_thread_data;

static void api_event_thread(void *a, void *b, void *c);
static void api_pump_locked(void);

/*
 * The three port hooks radiant_event.c declares but deliberately leaves
 * undefined, so a build that forgets one fails at link time rather than
 * shipping a silent data race. radiant_event.c is freestanding (no Zephyr
 * header) and cannot reach any of this itself.
 */

unsigned int radiant_event_crit_enter(void)
{
	/* irq_lock(), not a mutex: the producer may be a radio ISR and must not
	 * block; the section is a few instructions plus one copy. */
	return irq_lock();
}

void radiant_event_crit_exit(unsigned int key)
{
	irq_unlock(key);
}

#if defined(CONFIG_RADIANT_SWI)
/* Runs from the trampoline's handler at an ordinary interrupt priority, which
 * is the whole point - see the wakeup below. */
static void event_wakeup_swi(uint32_t bits)
{
	ARG_UNUSED(bits);
	k_sem_give(&api_event_sem);
}
#endif

void radiant_event_wakeup(void)
{
	/* O(1), ISR-safe, never calls back into radiant_event. Semaphore limit
	 * of 1 collapses a burst of arrivals into one wake, which is fine since
	 * the drain empties the whole queue anyway. */
#if defined(CONFIG_RADIANT_SWI)
	/*
	 * BUG 27, AND THIS IS THE CALL THAT MATTERED.
	 *
	 * "ISR-safe" above is true of an ordinary ISR and false of a
	 * zero-latency one. Under the MPSL gate this function is reached from
	 * MPSL's timeslot signal callback -
	 *
	 *   SIGNAL_RADIO -> radiant_nrf_gate_on_radio_irq() -> deliver_terminal()
	 *     -> radiant_event_post_rx() -> here
	 *
	 * - and MPSL connects that vector with IRQ_ZERO_LATENCY, which runs
	 * above every irq_lock() and every kernel spinlock. The event thread
	 * below waits on this semaphore WITH A TIMEOUT (see api_event_thread's
	 * k_sem_take(&api_event_sem, K_MSEC(RADIANT_API_HOUSEKEEP_MS))), so
	 * giving it wakes a thread that has a timeout pending, and that runs
	 * z_abort_timeout() on the kernel's timeout list from a context which
	 * cannot hold its lock. Measured: sys_dlist_remove/sys_dlist_insert bus
	 * faults with BFAR 0, "Fault during interrupt handling", and on a build
	 * with the SoftDevice Controller a RESET_CPU_LOCKUP reboot loop.
	 *
	 * Fixing the gate's own kernel calls and not this one was measured to
	 * make the fault rate WORSE, because this is the frequent caller - one
	 * per received packet - and the gate's are not. So the pend goes here
	 * first. See docs/p4-zli-kernel-calls.md.
	 */
	radiant_swi_pend(RADIANT_SWI_EVENT_WAKEUP);
#else
	/* No MPSL gate, no zero-latency vector in the path: the direct backend's
	 * radio interrupt is an ordinary one, where this is legal and cheaper. */
	k_sem_give(&api_event_sem);
#endif
}

/*
 * The fourth port hook, and the one that used to not need one at all:
 * radiant_event.c called antr_on_message() directly until radiant
 * stopped reaching outside itself for src/ant_radio.h. Now it calls
 * radiant_on_message() (radiant/include/radiant/radiant_msg.h,
 * module-owned), and this is the one-line forward onto the real thing -
 * a field-for-field copy since struct radiant_msg and struct antr_msg are
 * layout-identical by construction. src/ant_serial_bridge.c stays the single
 * definition of antr_on_message() regardless of which radio backend is
 * compiled in, which is the point: the sdk_ant and stub backends call it
 * directly, and this file is the only thing that knows the core backend
 * reaches it through an extra hop.
 */
void radiant_on_message(const struct radiant_msg *msg)
{
	struct antr_msg m = {
		.id = msg->id,
		.len = msg->len,
		.data = msg->data,
	};

	antr_on_message(&m);
}

/*
 * radiant_channel.c raises events through radiant_channel_event_out();
 * radiant_event.c spells the same operation radiant_event_post_channel_event().
 * The adapter lives here since neither module knows the other's name for it.
 * event_code is an ANTW_EVENT_* wire byte either way, so no mapping is
 * needed. May run in a radio callback (search timeout, close completing), so
 * it must not block - posting to the ring doesn't.
 */
void radiant_channel_event_out(uint8_t channel, uint8_t event_code)
{
#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
	/*
	 * Release-on-loss-of-tracking: a channel that starts pairing and then
	 * loses its peer produces no completion or failure event -
	 * RX_FAIL_GO_TO_SEARCH is the only transition that fires, so it's the
	 * only place to free a half-finished exchange. Needed now, while
	 * pairing state is per-channel: sdk-ant serialises key exchange
	 * device-wide, so without this hook a lost peer would deadlock
	 * negotiation for every channel, permanently (docs/radiant-security.md
	 * 8.1).
	 */
	if (event_code == (uint8_t)ANTW_EVENT_RX_FAIL_GO_TO_SEARCH) {
		radiant_sec_pair_on_search(channel);
	}
#endif
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
	/* Never zero: radiant_channel_op_owner() treats 0 as "no operation". */
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

/* Levels 0..5 are -20, -12, -4, 0, +4, +8 dBm. The HAL rounds dBm to the
 * nearest setting; the custom bit is a raw part-specific escape hatch, set
 * only when a host asks for it by name. */
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

/* radiant_transfer's return codes onto the wire, per the mapping
 * radiant_transfer.h states - implemented here, the only place holding both
 * halves. */
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
	case RADIANT_SCHED_DONE_DENIED:
		/*
		 * FAILED is the only answer the consumers (transfer engine,
		 * radiant_channel_on_terminal()) can act on: a denied ack reply
		 * missed the peer's receive window, and radiant_transfer.h's
		 * retransmit path is deliberately unwired, so the transfer must
		 * end. The denial itself is not lost - api_stats.sched_denied is
		 * incremented from `why` at the top of api_sched_done(), so this
		 * lossy mapping is compensated exactly once, there.
		 */
		return RADIANT_RADIO_STATUS_FAILED;
	case RADIANT_SCHED_DONE_MISSED:
	case RADIANT_SCHED_DONE_FAILED:
	default:
		return RADIANT_RADIO_STATUS_FAILED;
	}
}

/*
 * The device ID list. radiant_search.c has no inclusion/exclusion list (only
 * "what this channel is looking for"), so antr_id_list_add()/_config() are
 * implemented here, and actually consulted - at acquisition - rather than
 * stored and ignored, since an accepted-but-unused list is worse than a
 * refused one.
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

		/* Zero is the wildcard in each field, as with a channel ID. */
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

#if defined(CONFIG_RADIANT_SEC)
/*
 * Where radiant_sec_pump() puts a packet once it is plaintext. A direct call
 * displacing a weak definition, not a registered callback: an ops struct
 * would be a .data relocation the linker can't garbage-collect, defeating the
 * zero-cost size gate; it also lets radiant_sec.c link into the unit-test app,
 * which has no radiant_api.c.
 *
 * Event-thread context - posting to the same ISR-safe ring the decode path
 * uses is the easy direction.
 *
 * The verdict does not reach the wire here; it's recorded per channel and
 * reported through 0xF4, so unverified data can never be silently read as
 * verified.
 */
void radiant_sec_deliver(uint8_t ch, const uint8_t *pay, uint8_t len,
			 enum radiant_sec_verdict verdict, uint64_t t_sync)
{
	struct radiant_channel_id id;
	struct radiant_rx_event   hal;

	ARG_UNUSED(verdict);

	if (!api_ch_valid(ch) || pay == NULL) {
		return;
	}
	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return;
	}

	memset(&hal, 0, sizeof(hal));
	hal.status = RADIANT_RADIO_STATUS_OK;
	hal.t_sync = (radiant_time_t)t_sync;
	/* Carried through the queue rather than re-read, so the host sees the
	 * radio's ADDRESS-event capture, not when the pump got to it. */
	hal.t_sync_exact = true;

	api_post_rx(ch, (uint8_t)ANTW_MESG_BROADCAST_DATA_ID, &id, &hal, pay,
		    len, 0u, false);
}
#endif /* CONFIG_RADIANT_SEC */

/*
 * The transfer engine's calls back into us. All three may run in radio
 * interrupt context - the engine re-arms from inside its own completion
 * callbacks (1.55 ms turnaround leaves no other path) - so posting to the
 * event ring, ISR-safe, is the right shape here too.
 */

static void api_xfer_done(void *ctx, const struct radiant_transfer_result *r)
{
	uint8_t ch = (uint8_t)(uintptr_t)ctx;

	if (!api_ch_valid(ch) || r == NULL) {
		return;
	}

	/*
	 * The buffer-ownership contract, B1-B5: the bridge holds one 24-byte
	 * block behind a binary semaphore, released only here. Miss
	 * NEXT_DATA_BLOCK once and a burst stalls 1000 ms per packet instead of
	 * breaking outright. The engine guarantees exactly one done() per block
	 * pointer; this just translates it.
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

	api_stats.rx_data_up++;

	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return;
	}

	seq = (uint8_t)(api_ch[ch].in_pkts & (uint8_t)ANTW_BURST_HEADER_SEQ_MASK);

	/* One packet ending LAST is acknowledged data; anything longer is a
	 * burst - the air can't tell them apart (Spike B part 2), so the
	 * distinction is made here. */
	if (last && api_ch[ch].in_pkts == 0u) {
		msg_id = (uint8_t)ANTW_MESG_ACKNOWLEDGED_DATA_ID;
	} else {
		msg_id = (uint8_t)ANTW_MESG_BURST_DATA_ID;
	}

	api_ch[ch].in_pkts = last ? 0u : (uint8_t)(api_ch[ch].in_pkts + 1u);
	/* Abandoned-burst watchdog clock - covers the burst whose LAST never
	 * arrives. See api_watchdogs_locked(). */
	api_ch[ch].in_last = radiant_radio_now();
	api_ch[ch].in_last_valid = true;

	/*
	 * radiant_transfer_ops::rx_data carries no clock reading, so the HAL
	 * event is synthesised with t_sync_exact false - which suppresses the
	 * 0xE0 timestamp field rather than reporting a thread-context reading
	 * as a radio-clock one.
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

	if (!api_ch_valid(ch)) {
		return false;
	}

	/*
	 * AN UNWRITTEN BUFFER IS NOT A REASON TO REFUSE, and treating it as one
	 * meant A SLAVE COULD NEVER ACKNOWLEDGE ANYTHING.
	 *
	 * The rule this enforced - "every acknowledgement ever captured carried
	 * the acknowledger's own broadcast buffer" - is a statement about
	 * CONTENT, and it is still obeyed below: the buffer is what goes out.
	 * What was wrong was reading it as a precondition. api_ch[].bcast is
	 * zeroed at init and at unassign, so an unwritten buffer is a valid
	 * eight zero bytes, not an absence.
	 *
	 * The consequence of refusing was total and silent, and it is the
	 * second half of why apps/treadmill's control loop never worked:
	 *
	 *   - a SLAVE has no reason to ever hold a broadcast buffer. No host
	 *     writes broadcast data to a channel it opened to listen on, so
	 *     bcast_valid was false for the whole life of the channel, so
	 *     radiant_transfer_on_data() got ESTATE, so the slave never
	 *     acknowledged, AND never passed the data up either (the refusal
	 *     returns before ops->rx_data). The host saw nothing at all.
	 *   - a MASTER was refused until its host had written a page, so an
	 *     acknowledged command arriving before the first broadcast was
	 *     dropped the same way.
	 *
	 * Measured 2026-08-17: a slave with a broadcast staged by hand
	 * acknowledged normally and reported every packet; the identical slave
	 * without one reported zero, with the master 150 of whose broadcasts it
	 * was receiving perfectly. api_post_master_tx() already transmits an
	 * unwritten buffer as zeros for the mirror-image reason (a master that
	 * will not transmit until written deadlocks every host library), so
	 * refusing here was also the odd one out.
	 */
	if (!api_ch[ch].bcast_valid) {
		api_stats.acks_from_unwritten_buffer++;
	}

#if defined(CONFIG_RADIANT_SEC)
	/*
	 * The plaintext leak, fixed in two lines: this used to memcpy the
	 * host's plaintext onto the air as the ack payload, which under X_CONF
	 * hands a listener the plaintext of ciphertext it just heard. The
	 * ciphertext already built for the current counter is free and correct:
	 * same nonce, same plaintext, identical ciphertext, no reuse.
	 */
	if (radiant_sec_channel_is_secured(ch) && api_ch[ch].sec_pay_valid) {
		memcpy(out, api_ch[ch].sec_pay, RADIANT_TRANSFER_PKT_BYTES);
		return true;
	}
#endif

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
	/* No safe default for the slot bit: wrong either way (0x82 vs 0x8A, or
	 * claiming a slot not owned) and invisible without a sniffer. Channel
	 * type decides it. */
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
	/* The sweep runs on network 0's address, where every host puts the
	 * ANT+ key; copied rather than assumed. */
	memcpy(cfg->net_addr, api_net_addr[0], RADIANT_NET_ADDR_LEN);
}

/* The sweep offered this channel a device. Runs in the radio interrupt. */
static void api_search_acquired(uint8_t channel,
				const struct radiant_search_result *r, void *user)
{
	struct radiant_rx_event hal;
	bool                 is_scan;

	(void)user;
	if (!api_ch_valid(channel) || r == NULL) {
		return;
	}

	if (!api_id_list_permits(channel, &r->id)) {
		api_stats.id_list_rejects++;
		return;
	}

	/*
	 * SCAN mode never leaves - the point of a background scan. Converting
	 * to tracking unconditionally here (as this used to do) ended the
	 * search on the first device answered, since radiant_channel_on_acquired()
	 * always moves the channel to TRACKING and the state check below then
	 * treats that as done. Query the mode before anything below changes it.
	 */
	is_scan = radiant_search_chan_mode(&api_search, channel) ==
		  RADIANT_SEARCH_MODE_SCAN;

	if (!is_scan) {
		radiant_channel_on_acquired(channel, &r->id, r->t_sync);
		api_stats.acquired++;
	}

	/*
	 * ANT delivers the first broadcast with the channel ID on acquisition,
	 * so the acquiring frame goes to the host here rather than wasting a
	 * period asking again. The HAL event is synthesised from the search
	 * result, which carries the decoded frame and timing but not the event.
	 */
	memset(&hal, 0, sizeof(hal));
	hal.status = RADIANT_RADIO_STATUS_OK;
	hal.t_sync = r->t_sync;
	hal.t_sync_exact = r->t_sync_exact;
	hal.has_rssi = r->has_rssi;
	hal.rssi_dbm = r->rssi_dbm;

	api_post_rx(channel, (uint8_t)ANTW_MESG_BROADCAST_DATA_ID, &r->id, &hal,
		    r->frame.payload, r->frame.payload_len, 0u, false);

	/* ACQUIRE leaves the search and joins the tracked set. Scan mode never
	 * leaves - is_scan skipped the state change above, so this stays
	 * SEARCHING and radiant_search_end() is not called. */
	if (radiant_channel_state_get(channel) == RADIANT_CH_STATE_TRACKING) {
		(void)radiant_search_end(&api_search, channel);
	}
}

/*
 * Search timeout is owned by radiant_channel.c, not here: it has the
 * high/low-priority tick semantics the serial protocol defines and must raise
 * RX_SEARCH_TIMEOUT then CHANNEL_CLOSED in order. So every radiant_search_begin()
 * below passes RADIANT_SEARCH_TIMEOUT_NONE and this callback never fires - it's
 * registered anyway so a NULL entry doesn't hide whether it was wired up.
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

#ifdef CONFIG_RADIANT_SWEEP_DEBUG
/*
 * The bounded-denial escape's counter, on the SWEEP line and nowhere else.
 * `struct radiant_search_stats` only carries the field where the escape is
 * compiled in (single-protocol images cannot be denied the air at all), so the
 * two symbols are independent and this reads 0 rather than failing to build.
 *
 * NOT in the 0xF6 health reply, deliberately: a nonzero esc= says the sweep
 * was refused long enough to have frozen AND DID NOT. That is the recovery,
 * not the loss - the loss is already visible to the host as slow discovery,
 * and a host acting on the recovery counter would act on good news.
 */
#if defined(CONFIG_RADIANT_SEARCH_DENIAL_ESCAPE)
#define API_DBG_DENIAL_ESCAPES(ss) ((unsigned int)(ss)->denial_escapes)
#else
#define API_DBG_DENIAL_ESCAPES(ss) ((void)(ss), 0u)
#endif

/*
 * ---------------------------------------------------------------------------
 * THE SWEEP ACCOUNTING IDENTITY
 * ---------------------------------------------------------------------------
 *
 * The SWEEP counters (declared with api_sched_armed(), further down) rule
 * things out. Four of them have now ruled out preemption, tracked channels
 * taking the radio, arm failures and crediting, and roughly 35 % of the
 * sweep's wall clock is still unexplained - because a fifth counter of the
 * same kind rules out a fifth thing and leaves the remainder unexplained
 * again.
 *
 * These are different: they are an IDENTITY. Every microsecond between one
 * search chunk's terminal and the next one opening falls into exactly one of
 * four intervals, and
 *
 *     unacct = elapsed - (scan_armed + overrun + gap_arm + gap_pump +
 *                         gap_post + track_armed)
 *
 * must come out at zero. `unacct` reaching zero IS the exit criterion for the
 * investigation - not a number getting smaller, a number reaching zero. Until
 * it does, the model of where the time goes is incomplete, and this line says
 * so rather than letting a plausible story stand.
 *
 * The two paths out of a chunk are distinguished because they are different
 * mechanisms, not different amounts:
 *
 *   KEPT (rechunk). The scheduler re-arms inside the terminal event. The gap
 *   is arm lead + ramp + commit ordering, and should be about
 *   caps.min_arm_lead_us. -> gap_arm
 *
 *   DROPPED. api_sched_done() cleared the slot, so only
 *   api_post_search_window() can re-post, and it is reachable only from
 *   api_housekeep() via api_event_thread()'s k_sem_take(..., K_MSEC(50)). On
 *   quiet air there is no RX event to wake it, so the wait is the timeout:
 *   uniform 0-50 ms. PREDICTION: mean gap_pump ~= 25 000 us, once per SET. If
 *   it is not, the mechanism above is wrong and one run says so - which is the
 *   point of predicting it in the source rather than after the fact.
 *   -> gap_pump, then gap_post for the posting-to-opening remainder.
 *
 * overrun is signed on purpose: large and positive means the HAL holds past
 * t_close, so the dwell credit is honest while the clock is not, and that is a
 * different bug from a gap.
 *
 * Declared here, above api_post_search_window(), because that function is the
 * one that closes gap_pump and it is defined before the rest of the SWEEP
 * counters are.
 */
static radiant_time_t dbg_last_close;      /* t_close actually granted */
static radiant_time_t dbg_last_done_now;   /* now at a search terminal */
static radiant_time_t dbg_slot_dropped_at; /* when api_search_slot was cleared */
static radiant_time_t dbg_pump_post_at;    /* when the pump posted */
static radiant_time_t dbg_epoch;           /* first search arm, for elapsed */

/* Which of the two paths the last terminal took, so the next arm knows which
 * interval it closes. Not derivable at arm time - the scheduler does not tell
 * this layer whether it kept the request. */
static bool dbg_kept;
static bool dbg_awaiting_post;

static uint32_t dbg_gap_arm_us, dbg_gap_arm_n;
static uint32_t dbg_gap_pump_us, dbg_gap_pump_n;
static uint32_t dbg_gap_post_us, dbg_gap_post_n;
static int64_t  dbg_overrun_us;
static uint32_t dbg_overrun_n;

/*
 * WHY THE PUMP DID NOT POST, one counter per early return.
 *
 * The first run of the identity above measured gap_pump at 1.716 s per set -
 * 86 % of the sweep's whole wall clock, and 69x the 25 ms the housekeeping
 * tick can explain. Housekeeping itself ran 31 times a second throughout, so
 * the pump was called about 54 times inside each of those gaps and declined to
 * post on every one of them. Which of its four early returns fired is the
 * whole remaining question, and it is not answerable from any counter that
 * existed.
 *
 * `arming` is the recursion guard, `slot` is a sweep already in flight,
 * `nochan` is no searching channel free to carry one, `window` is
 * radiant_search.c refusing. `ok` is a post.
 */
static uint32_t dbg_pump_arming, dbg_pump_slot, dbg_pump_nochan;
static uint32_t dbg_pump_window, dbg_pump_ok;

/*
 * `nochan` split, because the two halves mean opposite things.
 *
 * NONE SEARCHING is the sweep having nothing to do: every channel has acquired
 * a device and stopped looking. That is correct - a dongle with all eight
 * channels tracking is not failing to discover, it has discovered - and the
 * time is not a defect, however large it is.
 *
 * BUSY is a channel that IS still searching and whose slot the scheduler is
 * holding for something else. That time is a real stall: there is discovery
 * work outstanding and the sweep cannot get on the air to do it.
 *
 * Without the split, `nochan` cannot tell "working as designed" from "the
 * defect" - and on a one-channel rig the first dominates so completely that a
 * reader would conclude the second.
 */
static uint32_t dbg_pump_none_searching, dbg_pump_busy;

/*
 * THE IN-FLIGHT CHUNK, AND WHY unacct WAS NOT ACTUALLY ZERO.
 *
 * dbg_scan_us credits a chunk's WHOLE armed length at arm time, because that is
 * where the granted bounds are known. The 1 s report then samples at an
 * arbitrary instant, which is usually part-way through a 260 ms chunk - so
 * `accounted` includes airtime that has not elapsed yet, and unacct comes out
 * NEGATIVE by however much of the chunk is still in the future. One sample
 * later the chunk has completed and the same arithmetic reads POSITIVE.
 *
 * That is exactly what the first run showed: unacct alternating between about
 * -625 000 and +526 000 us against a 125 s elapsed, sign flipping with the
 * sample phase. It looked like +-0.5 % of unexplained time and was not - it was
 * one chunk of double-counting, appearing and disappearing.
 *
 * The exit criterion for this investigation is unacct reaching ZERO, not
 * reaching small, so this is worth removing rather than describing. Recording
 * the in-flight chunk's scheduled close lets the report subtract the part of it
 * that has not happened yet, which makes the identity exact at ANY sampling
 * instant instead of only at a chunk boundary.
 *
 * Tracked windows have the same shape and are ignored on purpose: one is 640 us
 * against a 260 ms scan chunk, so the correction would be a four-hundredth of
 * the one that matters and would cost a second pair of variables to carry.
 */
static bool           dbg_inflight;
static radiant_time_t dbg_inflight_close;
#endif

/*
 * True while a radiant_sched_*() call that may invoke done() synchronously is
 * in progress. A refused arm completes inline, and posting the next request
 * from a completion is normally the intended low-jitter path - but against a
 * backend refusing every arm the two together are unbounded recursion:
 * post -> arm -> refused -> done() -> post -> ... api_search_slot can't guard
 * this itself since api_sched_done() clears it before the recursion.
 *
 * Found on the bench: opening one wildcard channel wedged the firmware on
 * both the nrf backend (every arm ETIME) and the null backend (every arm
 * ENOTSUP), stack walked to within 648 B of PSPLIM. A refused arm must leave
 * the retry to the next (rate-limited) pump.
 */
static bool api_arming;
static void api_post_search_window(radiant_time_t now)
{
	const struct radiant_radio_caps *caps = radiant_radio_caps_get();
	struct radiant_search_window     w;
	struct radiant_sched_rx          req;
	uint8_t                      ch;

	/* Not from inside an arm - see api_arming above. */
	if (api_arming) {
#ifdef CONFIG_RADIANT_SWEEP_DEBUG
		dbg_pump_arming++;
#endif
		return;
	}

	/* One sweep at a time - radiant_search.c holds exactly one window in
	 * flight, which is what makes its filters[] pointer safe to hand
	 * straight to the HAL. */
	if (api_search_slot != RADIANT_SCHED_CH_NONE) {
#ifdef CONFIG_RADIANT_SWEEP_DEBUG
		dbg_pump_slot++;
#endif
		return;
	}

	/*
	 * One sweep serves every searching channel - the difference between
	 * eight simultaneous searches taking ~8 s and ~64 s. The sweep rides a
	 * searching channel's slot; any one will do, but it must actually be
	 * free. Stopping at the first searching channel and giving up if its
	 * slot was merely pending (busy for reasons unrelated to the sweep)
	 * used to stall the whole sweep - skip a busy one and keep looking.
	 */
	/* Unconditional, not under SWEEP_DEBUG: both that log line and the
	 * 0xF6 health reply need it, and the two symbols are independent. With
	 * neither compiled in it is a set-but-unused bool the optimiser
	 * deletes. */
	bool any_searching = false;

	for (ch = 0u; ch < API_CHANNELS; ch++) {
		if (!radiant_search_is_searching(&api_search, ch)) {
			continue;
		}
		any_searching = true;
		if (!radiant_sched_pending(ch)) {
			break;
		}
	}
	if (ch >= API_CHANNELS) {
#ifdef CONFIG_RADIANT_SWEEP_DEBUG
		dbg_pump_nochan++;
		if (any_searching) {
			dbg_pump_busy++;
		} else {
			dbg_pump_none_searching++;
		}
#endif
		/* Independent of SWEEP_DEBUG: a shipping image has no business
		 * carrying the once-a-second SWEEP log line, and this is the
		 * half of that line a host still needs.
		 *
		 * Only the BUSY half. The idle half climbs forever on a dongle
		 * with nothing open and would saturate the reply in about half
		 * an hour - see ANT_HEALTH_SWEEP_BUSY. */
		if (any_searching) {
			ant_health_note(ANT_HEALTH_SWEEP_BUSY);
		}
		return;
	}

	if (radiant_search_window(&api_search,
			      now + (radiant_time_t)caps->min_arm_lead_us,
			      &w) != RADIANT_SEARCH_OK) {
#ifdef CONFIG_RADIANT_SWEEP_DEBUG
		dbg_pump_window++;
#endif
		return;
	}

	/*
	 * The sweep is posted as a background scan (t_close = RADIANT_TIME_NEVER,
	 * radiant_sched.c's continuous form), not a bounded window - this is the
	 * fix for tracked-channel loss.
	 *
	 * A bounded sweep window always won arm_next()'s "whatever starts
	 * first" and then held the radio for its whole dwell (arm_next() only
	 * truncates a bounded RX for a TX, never for another RX), so a tracked
	 * slave's window fell inside it, was never armed, and expired MISSED -
	 * neither want_preempt() nor could_join_armed() could rescue it (the
	 * fmt mismatch blocks merging). Measured against a -25 dBm master:
	 * 0.02% loss tracking alone, 5.96% with a sweep also running, RX_FAIL
	 * x693 in a 60 s eight-channel session.
	 *
	 * The continuous form fixes it because arm_next() already caps a scan
	 * at s_limit = t_back(committed, lead) and want_preempt()/abort_armed()
	 * let a later bounded request displace a running chunk without
	 * consuming it, so the scan pauses and resumes in the gaps.
	 *
	 * Tried and rejected: computing the limit here from the earliest
	 * tracked slot. Fixed the loss (RX_FAIL 590->115) but starved the
	 * sweep - the earliest slot is nearly always imminent, so almost every
	 * post was refused as too short and no device was found in 90 s. A
	 * refused bounded post is a lost pass; a chunk the scheduler declines
	 * to arm is free and reconsidered next pass. Only the scheduler can see
	 * the transmits, so only it can make that call.
	 *
	 * chunk_us is the dwell still owed to the current SET (a ceiling, not a
	 * period) - the gap is usually the real limit, and passing the whole
	 * dwell keeps an idle dongle arming one full-period window as before.
	 */
	memset(&req, 0, sizeof(req));
	req.fmt = w.fmt;
	req.rf_index = w.rf_index;
	req.filters = w.filters;
	req.n_filters = w.n_filters;
	req.t_open = w.t_open;
	req.t_close = RADIANT_TIME_NEVER;
	req.chunk_us = (uint32_t)(w.t_close - w.t_open);
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
#ifdef CONFIG_RADIANT_SWEEP_DEBUG
	/* The posting pass closes gap_pump and opens gap_post. Stamped after
	 * the request is accepted, not before: a refused post returned above
	 * and left the slot dropped, so charging it here would credit the pump
	 * with work it did not do. */
	dbg_pump_post_at = now;
	dbg_pump_ok++;
	if (dbg_slot_dropped_at != 0u && now > dbg_slot_dropped_at) {
		dbg_gap_pump_us += (uint32_t)(now - dbg_slot_dropped_at);
		dbg_gap_pump_n++;
		dbg_slot_dropped_at = 0u;
	}
	dbg_awaiting_post = true;
#endif
	/* radiant_search_armed() is NOT called here: a scan's chunk bounds are
	 * decided by the scheduler from work this layer can't see, so crediting
	 * dwell here would be a guess. api_sched_armed() does it instead, once
	 * per chunk, with the shape actually granted. */
}

/*
 * The follow-on reserve: how much air an arbitrated backend must hold past an
 * operation's end for the reply the core may arm from its completion. See
 * struct radiant_rx_req::follow_on_us. Zero cost on a backend that owns the
 * radio outright.
 *
 * Derived here, the only file holding both radiant_transfer.h's turnaround
 * constants and radiant_sched.h's slot kinds - the backend can't compute it
 * without reading ANT semantics out of a format pointer, which
 * radiant_radio_hal.h forbids.
 */

/*
 * One ANT+ frame on the air at 1 Mbit/s: 1 preamble + 5 address + 10 body +
 * 2 CRC = 18 B = 144 us, rounded up to 150. A compile-time budget rather than
 * radiant_frame_airtime_us() (exact but a function), needed for the
 * BUILD_ASSERT below; rounding errs toward over-reserving.
 */
#define API_FRAME_AIRTIME_US 150u

/*
 * A tracked window's reserve: ack turnaround + window guard + reply airtime,
 * ~1960 us - within 2% of RADIANT_SCHED_MERGE_SPAN_MAX_US (2000), not a
 * coincidence: both bound how far apart two tracked channels can be and still
 * share a window, so the merge span is reused rather than risking drift.
 */
#define API_FOLLOW_ON_TRACK_US ((uint16_t)RADIANT_SCHED_MERGE_SPAN_MAX_US)

_Static_assert(RADIANT_TRANSFER_REPLY_US + RADIANT_TRANSFER_ACK_GUARD_US +
			       API_FRAME_AIRTIME_US <=
		       RADIANT_SCHED_MERGE_SPAN_MAX_US,
	       "the tracked follow-on reserve no longer fits inside the merge "
	       "span, so the two have stopped being the same quantity and the "
	       "reserve needs its own constant");

/*
 * A listening master's reserve: its slave's reply lands
 * RADIANT_TRANSFER_SLOT_REPLY_US after its own t_sync, and api_post_master_rx()
 * arms that turnaround from inside this transmit's completion.
 */
#define API_FOLLOW_ON_MASTER_TX_US                                       \
	((uint16_t)(RADIANT_TRANSFER_SLOT_REPLY_US +                     \
		    RADIANT_TRANSFER_ACK_GUARD_US + API_FRAME_AIRTIME_US))

static void api_post_track_rx(uint8_t ch)
{
	struct radiant_channel_id id;
	struct radiant_sched_rx   req;
	radiant_time_t            t = radiant_channel_next_slot(ch);
	radiant_time_t            guard;
	int                   n;

	if (t == RADIANT_TIME_NEVER) {
		return;
	}
	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return;
	}

	/* An on-air address match IS a channel-ID match. */
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
	/* Per channel/slot, not a fixed constant: a measured master gets a
	 * window sized to that measurement, one that's just acquired or missing
	 * slots gets the wide default back automatically. Never above the old
	 * constant. */
	guard = (radiant_time_t)radiant_channel_guard_us(ch);
	req.t_open = (t > guard) ? (t - guard) : 0u;
	req.t_close = t + guard;
	/* Ask for CRC failures only when something uses them. Off by default: a
	 * quiet room's ~1.4/s noise never crosses the HAL boundary. */
	req.report_crc_fail = api_crc_repair_on;
	/* A request, not a guarantee - HAL forbids STOP_ON_FIRST on a merged
	 * window and the scheduler drops it if this one ends up carrying more
	 * than one channel. Worth asking: saves receive current and frees the
	 * slot early on a window that stays single. */
	req.stop_on_first = true;
	/* Unconditional: any frame here may become a transmit 1.56 ms later
	 * (api_sched_rx() -> api_tracked_frame() -> radiant_transfer_on_data()),
	 * depending on a control byte not yet read. Reserve is given back the
	 * moment an empty window closes. */
	req.follow_on_us = API_FOLLOW_ON_TRACK_US;

	if (radiant_sched_request_rx(ch, &req) != RADIANT_RADIO_OK_RC) {
		api_bind_rejected(ch);
		return;
	}
	api_bind_accepted(ch, API_SLOT_TRACK_RX);
}

/*
 * A master has to listen, and until this existed it never did:
 * api_pump_locked() gave a TRACKING master only api_post_master_tx(), so it
 * never heard a slave's reply - no acknowledged data, no burst start,
 * indistinguishable from a link problem, and invisible from the host since
 * EVENT_TX fires the same either way.
 *
 * WHEN: RADIANT_TRANSFER_SLOT_REPLY_US (2190 us, measured) from the master's
 * t_sync to a slave's reply t_sync, guarded by RADIANT_TRANSFER_ACK_GUARD_US -
 * the same constant the transfer engine's own ack window uses.
 * WHAT: the same on-air address the master transmits, so a reply hardware-
 * matches by construction. Never STOP_ON_FIRST - a shared channel may have
 * more than one slave answering.
 * COST: ~500 us receive per 249.7 ms period (0.2% duty), unconditional
 * because a slave may start an exchange in any slot.
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
	/*
	 * THE SAME RESERVE THE TRACKED WINDOW TAKES, and for the identical
	 * reason - this was missing until 2026-08-17 and it is the master half
	 * of the acknowledged-data path.
	 *
	 * A slave's acknowledged-data packet arrives in THIS window and becomes
	 * a transmit RADIANT_TRANSFER_REPLY_US later (api_sched_rx() ->
	 * api_tracked_frame() -> radiant_transfer_on_data(), which arms the ack
	 * before returning). Without follow_on_us the arbiter is told the air is
	 * wanted only until t_close, so on an MPSL build the reply is asked for
	 * outside the granted timeslot and never leaves: the slave has the
	 * packet delivered, the master has the data, and the originator still
	 * reports FAIL_NO_ACK. On a backend that owns the radio outright the
	 * field is ignored by contract, so this costs nothing there.
	 *
	 * API_FOLLOW_ON_TRACK_US (2000 us) rather than a tighter number derived
	 * from t_close: the reply is due REPLY_US after the DATA packet's t_sync,
	 * which can be anywhere in this window, so the reserve has to cover the
	 * late edge. 1560 - 250 + 150 = 1460 us is the requirement; reusing the
	 * tracked constant covers it and keeps one number for one meaning.
	 */
	req.follow_on_us = API_FOLLOW_ON_TRACK_US;

	if (radiant_sched_request_rx(ch, &req) != RADIANT_RADIO_OK_RC) {
		api_bind_rejected(ch);
		return;
	}
	api_bind_accepted(ch, API_SLOT_MASTER_RX);
}

/* Bit 4 of the channel type is the master bit; 0x50 is MASTER_TX_ONLY, the
 * one master documented not to listen - hence asking the type rather than
 * radiant_channel_is_master(). */
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
	 * A master transmits from its first slot with or without host data.
	 * Refusing to transmit until bcast_valid deadlocks every host library:
	 * hosts pace writes off EVENT_TX, so a master that won't transmit until
	 * written raises no EVENT_TX to write in response to. So an unwritten
	 * buffer transmits as zeros (api_ch[].bcast is already zeroed at init
	 * and unassign). api_xfer_broadcast() still refuses to acknowledge
	 * without a valid payload - unlike a broadcast slot, every captured
	 * acknowledgement carried real data, never zeros.
	 */

	/* 0x0A: not exchange, not ack, not last, seq 0, slot opener - the only
	 * encoding with bit 7 clear ever seen on the air. Built from fields so
	 * radiant_frame_make() can refuse any other combination. */
	memset(&fields, 0, sizeof(fields));
	fields.slot_opener = true;

#if defined(CONFIG_RADIANT_SEC)
	/*
	 * The TX hook: thread context, under api_lock, immediately before the
	 * frame is built - the latest correct moment, since the TX body is
	 * DMA'd and armed early. Transforms into a copy, never into
	 * api_ch[ch].bcast, whose readback must stay the host's plaintext and
	 * must not be re-encrypted on every retransmission.
	 */
	memcpy(api_ch[ch].sec_pay, api_ch[ch].bcast, sizeof(api_ch[ch].sec_pay));
	api_ch[ch].sec_pay_valid = false;
	if (radiant_sec_tx_transform(ch, api_ch[ch].sec_pay,
				     (uint8_t)sizeof(api_ch[ch].sec_pay)) !=
	    RADIANT_SEC_OK) {
		/* A channel configured for something the backend cannot do
		 * transmits nothing rather than transmitting it in the clear.
		 * Failing open here would be a channel the host believes is
		 * protected and is not. */
		return;
	}
	api_ch[ch].sec_pay_valid = radiant_sec_channel_is_secured(ch);

	if (radiant_frame_make(&f, &id, &fields, api_ch[ch].sec_pay,
			   RADIANT_FRAME_PAYLOAD_STD) != RADIANT_FRAME_OK) {
		return;
	}
#else
	if (radiant_frame_make(&f, &id, &fields, api_ch[ch].bcast,
			   RADIANT_FRAME_PAYLOAD_STD) != RADIANT_FRAME_OK) {
		return;
	}
#endif
	if (radiant_frame_encode(RADIANT_FRAME_CFG_TRACKING, api_net_addr_for(ch), &f,
			     &w) != RADIANT_FRAME_OK) {
		return;
	}
	if ((size_t)w.body_len > sizeof(api_ch[ch].tx_body)) {
		return;
	}

	/* Per-channel buffer, not a local: the HAL DMAs out of it until the
	 * completion callback. */
	memcpy(api_ch[ch].tx_body, w.body, w.body_len);
	api_ch[ch].tx_body_len = w.body_len;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = api_rf_of(ch);
	req.power = api_power_of(ch);
	/* The address radiant_frame_encode() just derived, not re-derived. */
	if (w.addr_len > sizeof(req.addr)) {
		return;
	}
	memcpy(req.addr, w.addr, w.addr_len);
	req.addr_len = w.addr_len;
	req.body = api_ch[ch].tx_body;
	req.body_len = api_ch[ch].tx_body_len;
	req.t_sync_at = t;
	/* Only a listening master needs the reserve - a MASTER_TX_ONLY channel
	 * arms no turnaround, so reserving for it would charge air for a window
	 * never opened. */
	req.follow_on_us = api_master_listens(ch) ? API_FOLLOW_ON_MASTER_TX_US
						  : 0u;

	if (radiant_sched_request_tx(ch, &req) != RADIANT_RADIO_OK_RC) {
		api_bind_rejected(ch);
		return;
	}
	api_bind_accepted(ch, API_SLOT_MASTER_TX);
}

/* One scheduling pass, from thread context: post everything, then tick once.
 * Callers hold api_lock. */
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
		 * slots rejoins here; the seen cache makes re-acquisition take
		 * about one dwell instead of half a sweep. */
		if (st == RADIANT_CH_STATE_SEARCHING) {
			if (!searching) {
				api_search_begin(ch, now);
			}
		} else if (searching) {
			/* If this channel carried the sweep, api_search_slot
			 * is left alone - radiant_search_end() doesn't touch
			 * the window in flight, so it keeps serving other
			 * searching channels until its own completion releases
			 * the carrier. Clearing it here would let a second
			 * sweep start while the first is still live. */
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

	/* The one commit. See api_arming: an arm refused here completes
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
static void api_crc_repair_setup(void)
{
	const struct radiant_radio_caps *caps = radiant_radio_caps_get();

	api_crc_repair_on = false;

#if defined(CONFIG_RADIANT_CRC_REPAIR)
	if (caps == NULL || !caps->has_rx_crc) {
		LOG_WRN("CRC repair is enabled but this backend reports no "
			"received CRC; the feature is inert");
		return;
	}
	if (!radiant_crc_repair_init()) {
		/* Two single-bit errors sharing a syndrome shouldn't happen
		 * for CRC-16/CCITT over a frame this short; if it does, the
		 * table is no longer a bijection and repairing would be a
		 * coin toss. */
		LOG_ERR("CRC repair table is not a bijection; repair disabled");
		return;
	}
	api_crc_repair_on = true;
	LOG_INF("CRC repair: on, tracked windows only");

#if defined(CONFIG_RADIANT_PROFILE_SANITY)
	api_profile_sanity_on = true;
	LOG_INF("CRC repair: profile sanity on (bpwr>%uW, hr>%ubpm dropped)",
		(unsigned)RADIANT_PROFILE_SANITY_BPWR_MAX_WATTS,
		(unsigned)RADIANT_PROFILE_SANITY_HR_MAX_BPM);
#endif
#else
	(void)caps;
#endif
}

static bool api_tracked_frame(uint8_t ch, const struct radiant_rx_event *evt,
			      bool crc_failed)
{
	struct radiant_channel_id id;
	struct radiant_frame_wire w;
	struct radiant_frame      f;
	enum radiant_msg_type     mt;
	int                   n;
	int                   rc;

	api_stats.rx_tracked_frames++;

	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return false;
	}
	if (evt->body == NULL || evt->body_len == 0u ||
	    (size_t)evt->body_len > sizeof(w.body)) {
		return false;
	}

	/* Matched address bytes never reach RAM (that's what a hardware address
	 * match means), so the wire frame's address half is regenerated from
	 * the ID this window filtered on. RADIANT_FRAME_TRUSTED_CRC is correct
	 * for every delivered event - status OK implies a verified CRC. */
	memset(&w, 0, sizeof(w));
	n = radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING, api_net_addr_for(ch), &id,
			   w.addr, sizeof(w.addr));
	if (n < 0) {
		return false;
	}
	w.addr_len = (uint8_t)n;
	memcpy(w.body, evt->body, evt->body_len);
	w.body_len = evt->body_len;

	/* Repair happens here, where address and body exist side by side (the
	 * CRC covers both). radiant_crc_repair() re-checks the configuration
	 * itself too; see its header for why. */
	if (crc_failed) {
		rc = radiant_crc_repair(RADIANT_FRAME_CFG_TRACKING, w.addr,
					w.addr_len, w.body, w.body_len,
					evt->crc_rx);
		if (rc != RADIANT_CRC_REPAIR_BODY &&
		    rc != RADIANT_CRC_REPAIR_IN_CRC) {
			api_stats.crc_unrepairable++;
			LOG_DBG("crc repair ch=%u rc=%d", (unsigned)ch, rc);
			return false;
		}
	}

	if (radiant_frame_decode(RADIANT_FRAME_CFG_TRACKING, &w, RADIANT_FRAME_TRUSTED_CRC,
			     &f) != RADIANT_FRAME_OK) {
		if (crc_failed) {
			/* Free refutation, worth counting separately: a
			 * mis-repair landing in the control byte (8 of 80 body
			 * bits, only 11 of 256 values ever seen) is rejected
			 * here ~96% of the time by a check that already
			 * existed for other reasons. */
			api_stats.crc_repair_refuted++;
		}
		return false;
	}

	/*
	 * Second free refutation, one the codec can't make: byte 0 of a tracked
	 * body is the transmission type, which the address match can't prove
	 * (it's in the body, not the matched address). A repaired frame whose
	 * trans_type disagrees with the channel's landed on the wrong bit.
	 * Applied to repaired frames only - a clean frame disagreeing here is a
	 * fact about the air (shared channel, guessed trans_type), not evidence
	 * of a bad repair.
	 */
	if (crc_failed && id.trans_type != 0u &&
	    f.payload_len > 0u && w.body[0] != id.trans_type) {
		api_stats.crc_repair_refuted++;
		LOG_DBG("crc repair ch=%u refuted: ttype %u != %u", (unsigned)ch,
			(unsigned)w.body[0], (unsigned)id.trans_type);
		return false;
	}

	/*
	 * Third refutation, the only one reading the payload: radiant_profile_sanity.c
	 * flags power >3000 W or heart rate >240 bpm as physically impossible
	 * for the two device types where a single frame decides it. crc_failed-
	 * gated for the same reason as the two checks above - a clean frame
	 * this implausible is a fact about the sensor, never dropped.
	 */
	if (crc_failed && api_profile_sanity_on &&
	    radiant_profile_sanity_implausible(id.device_type, f.payload,
						f.payload_len)) {
		api_stats.crc_repair_implausible++;
		LOG_DBG("crc repair ch=%u refuted: implausible payload for "
			"device type %u", (unsigned)ch,
			(unsigned)(id.device_type & RADIANT_DEVICE_TYPE_MASK));
		return false;
	}

	if (crc_failed) {
		/* Counted here, not at the repair, so this means "repaired AND
		 * delivered" - never confused with a clean frame. */
		api_stats.crc_repaired++;
	}

	/*
	 * Slot clock re-anchored on the frame actually heard, centring the
	 * window on the master rather than the moment the channel opened.
	 * A master must not do either of these: it owns the slot grid, and the
	 * frame it hears is a slave's reply 2.19 ms into its own period, so
	 * re-anchoring would drag its phase forward every exchange. The seen
	 * cache is likewise about masters found by searching, not slaves.
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
#if defined(CONFIG_RADIANT_SEC)
		/*
		 * The RX policy guard: ISR context, classification only, zero
		 * crypto - decides whether this packet belongs to the
		 * transform path and if so queues eight bytes. XOR, CMAC and
		 * verdict happen later in radiant_sec_pump(), which posts the
		 * plaintext via radiant_sec_deliver() so ordering is preserved.
		 */
		{
			int sec = radiant_sec_rx_ingest(ch, f.payload,
							f.payload_len, true,
							(uint64_t)evt->t_sync);

			if (sec != RADIANT_SEC_RX_PLAIN) {
				break;
			}
		}
#endif
		api_post_rx(ch, (uint8_t)ANTW_MESG_BROADCAST_DATA_ID, &id, evt,
			    f.payload, f.payload_len, 0u, false);
		break;
	case RADIANT_MSG_BURST_DATA:
	case RADIANT_MSG_BURST_LAST:
#if defined(CONFIG_RADIANT_SEC)
		/*
		 * D15. X_AUTH governs what a secured channel initiates, but
		 * this switch also decodes acknowledged-data/burst, so without
		 * this an injector could send a secured-range page as
		 * acknowledged data and skip the MAC entirely. broadcast=false
		 * routes a secured-range page to the drop arm; anything else
		 * is PLAIN and proceeds untouched.
		 */
		if (radiant_sec_rx_ingest(ch, f.payload, f.payload_len, false,
					  (uint64_t)evt->t_sync) ==
		    RADIANT_SEC_RX_DROP) {
			LOG_DBG("sec ch=%u dropped non-broadcast page 0x%02X",
				(unsigned)ch, (unsigned)f.payload[0]);
			break;
		}
#endif
		/* The receive path that has to transmit: ~1.55 ms until the
		 * peer expects a reply, so radiant_transfer_on_data() arms the
		 * ack before handing the payload up - the host-facing message
		 * is posted from api_xfer_rx_data() instead of here. */
		/* Not inside the LOG_DBG argument list: an expression that only
		 * runs when logging is on would be dead in a release build. */
		api_stats.rx_on_data++;
		rc = radiant_transfer_on_data(&api_xfer[ch], &f, evt->t_sync);
		LOG_DBG("on_data rc=%d", rc);
		break;
	case RADIANT_MSG_BURST_ACK:
	case RADIANT_MSG_TRANSFER_ACK:
	case RADIANT_MSG_UNKNOWN:
	default:
		/* An acknowledgement with no transfer of ours behind it, or a
		 * flag combination nothing has measured. The frame layer names
		 * it and declines to interpret it, and so does this.
		 *
		 * DELIBERATELY NO 0xF6 COUNTER HERE. This is not host-bound
		 * loss - nothing the host was waiting for went missing - so
		 * it is outside 0xF6's charter (see ant_health.h). */
		break;
	}

	return true;
}

static void api_sched_rx(uint8_t ch, uint8_t filter_index,
			 const struct radiant_rx_event *evt, void *user)
{
	(void)user;

	if (!api_ch_valid(ch) || evt == NULL) {
		return;
	}

	api_stats.rx_sched_events++;

	if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_SEARCH) {
		/* radiant_search_on_rx_indexed(), never the plain form: a merged
		 * window's evt->filter_index indexes the merged array, and the
		 * scheduler has already remapped it to this channel's filters. */
		radiant_search_on_rx_indexed(&api_search, evt, filter_index);
		return;
	}

	if (evt->status != RADIANT_RADIO_STATUS_OK) {
		/*
		 * CRC_FAIL: with eight filters armed, ~1.4/s in a quiet room
		 * is just noise floor - except on a tracked window with
		 * repair on, where a matched 5-byte address means this IS the
		 * tracked sensor, likely one flipped bit from good. The three
		 * checks below mirror what the repair module would apply.
		 */
		if (!api_crc_repair_on || !evt->has_crc_rx) {
			return;
		}
		if (radiant_channel_op_owner(api_ch[ch].op) != (int)ch) {
			/* The CRC-repair path's silent twin of the ownership
			 * check further down - same condition, no LOG_DBG
			 * here since this runs on a CRC_FAIL event rather
			 * than a good one. */
			ant_health_note(ANT_HEALTH_RX_NOT_OWNER);
			return;
		}
		if (!radiant_transfer_is_idle(&api_xfer[ch])) {
			/* Scope decision: a transfer in flight is arming an ack
			 * inside a 1.55 ms turnaround, so it isn't offered
			 * repaired frames - a burst already retries anyway. */
			return;
		}
		if (api_tracked_frame(ch, evt, true)) {
			/* The slot WAS heard - counting it as a miss would send
			 * a working channel back to search. */
			api_ch[ch].slot_heard = true;
		}
		return;
	}

	/*
	 * Ownership check before acting on a non-terminal event: is the
	 * request this event belongs to still the one this channel owns?
	 * evt->op is the HAL's id; radiant_channel.c holds the surrogate this
	 * file minted, since radiant_sched.c sits between the HAL and it.
	 */
	if (radiant_channel_op_owner(api_ch[ch].op) != (int)ch) {
		/* Without this, a channel that heard its peer but is dropped
		 * here looks identical to a window never armed. */
		LOG_DBG("rx drop ch=%u not-owner op=%d owner=%d", (unsigned)ch,
			(int)api_ch[ch].op,
			radiant_channel_op_owner(api_ch[ch].op));
		ant_health_note(ANT_HEALTH_RX_NOT_OWNER);
		return;
	}

	api_ch[ch].slot_heard = true;

	if (!radiant_transfer_is_idle(&api_xfer[ch])) {
		/* A frame routed to a non-idle transfer never reaches
		 * api_tracked_frame(), so a wedged engine looks like silence
		 * from up here too. */
		LOG_DBG("rx to xfer ch=%u xfer=%d", (unsigned)ch,
			(int)radiant_transfer_state(&api_xfer[ch]));
		api_stats.rx_to_xfer++;
		radiant_transfer_on_rx_event(&api_xfer[ch], evt);
		return;
	}

	(void)api_tracked_frame(ch, evt, false);
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

	/* Our own broadcast went out. EVENT_TX is the outcome; broadcast is
	 * unacknowledged by design so there's no failure event. */
	api_ch[ch].slot_heard = true;
	api_ch[ch].bcast_pending = false;
	radiant_channel_on_slot(ch, evt->t_sync);
	(void)radiant_event_post_channel_event(ch, (uint8_t)ANTW_EVENT_TX);

	/*
	 * The turnaround is requested here but posted from api_sched_done()
	 * (see api_chan::turn_pending) - the H3 fix. Requested here because
	 * this is the only place with the achieved t_sync (hardware ADDRESS-
	 * event capture, needed since the reply is only 2.19 ms away). Posted
	 * from the completion, not here, because posting here would reuse the
	 * slot the scheduler still holds in flight and silently eat this same
	 * transmit's done(); the completion is the same interrupt, one callback
	 * later, with no extra tick or jitter. api_master_listens() is asked
	 * here for the same answer either way, but the TRACKING check moves to
	 * the post, after radiant_channel_on_terminal() has run.
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
		/* A window closing empty is RADIANT_RADIO_STATUS_TIMEOUT, which
		 * the engine reads as FAIL_NO_ACK and does not retry - inventing
		 * a retry would put an unmeasured frame on the air at an
		 * unmeasured instant. */
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

/*
 * A window carrying this channel has been armed, with the bounds it really
 * got. Only the sweep cares: its dwell is accounted in listening time, and
 * the scheduler arms scan chunks based on pending work (transmits included)
 * this layer can't see, so crediting the bounds api_post_search_window()
 * merely proposed would overcount.
 */
#ifdef CONFIG_RADIANT_SWEEP_DEBUG
/* Armed length by slot kind - showed tracked windows cost 640 us each
 * (2.6 ms/s), ruling them out as the cause of a starved sweep. */
static uint32_t dbg_scan_us, dbg_scan_n, dbg_track_us, dbg_track_n;
/* How scan chunks end, which decides how much dwell they credit. */
static uint32_t dbg_end_ok, dbg_end_abort, dbg_end_missed, dbg_end_failed;
/* Chunks the arbiter refused - separate from `failed` since this is the
 * number the "sweep is the elastic consumer" claim (ADR 0013) is read against. */
static uint32_t dbg_end_denied;

#endif

static void api_sched_armed(uint8_t ch, radiant_time_t t_open, radiant_time_t t_close,
			void *user)
{
	(void)user;
	if (!api_ch_valid(ch)) {
		return;
	}
#ifdef CONFIG_RADIANT_SWEEP_DEBUG
	/* TEMPORARY, for the search-vs-track contention hunt: one line per armed
	 * receive, so the interleave can be read directly instead of inferred
	 * from per-second totals. */
	LOG_INF("ARM k=%u ch=%u open=%d len=%d",
		(unsigned int)api_ch[ch].slot_kind, (unsigned int)ch,
		(int)(int64_t)(t_open - radiant_radio_now()),
		(t_close == RADIANT_TIME_NEVER)
			? -1
			: (int)(int64_t)(t_close - t_open));

	if (t_close != RADIANT_TIME_NEVER && t_close > t_open) {
		uint32_t len = (uint32_t)(t_close - t_open);

		if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_SEARCH) {
			dbg_scan_us += len;
			dbg_scan_n++;
		} else if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_TRACK_RX) {
			dbg_track_us += len;
			dbg_track_n++;
		}
	}

	/* The identity's arm side. Only the search slot participates: the
	 * question is where the SWEEP's wall clock goes. */
	if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_SEARCH) {
		if (dbg_epoch == 0u) {
			dbg_epoch = t_open;
		}
		dbg_last_close = t_close;

		/* The chunk just credited to dbg_scan_us has not been listened
		 * to yet. Remember when it is due to finish so the report can
		 * take back whatever part of it is still in the future. */
		if (t_close != RADIANT_TIME_NEVER && t_close > t_open) {
			dbg_inflight = true;
			dbg_inflight_close = t_close;
		}

		if (dbg_kept && dbg_last_done_now != 0u &&
		    t_open > dbg_last_done_now) {
			dbg_gap_arm_us += (uint32_t)(t_open - dbg_last_done_now);
			dbg_gap_arm_n++;
		} else if (dbg_awaiting_post && dbg_pump_post_at != 0u &&
			   t_open > dbg_pump_post_at) {
			dbg_gap_post_us += (uint32_t)(t_open - dbg_pump_post_at);
			dbg_gap_post_n++;
		}
		dbg_kept = false;
		dbg_awaiting_post = false;
	}
#endif
	/*
	 * A tracked window that really got armed owes its successor. Recorded
	 * here rather than at post time so a refused arm - which never reaches
	 * this callback - is not credited a successor from the arm side.
	 *
	 * IT IS NO LONGER THE ONLY PLACE THAT SETS THIS. A window that never
	 * armed reaches api_sched_done() as MISSED or DENIED and sets the flag
	 * there instead, because "the arm was refused" and "the channel has
	 * stopped asking for windows" are different facts and only the first is
	 * true - see that branch for why waiting for the 50 ms pump was a ~2 s
	 * dropout. What this site still buys is that the flag is set from a
	 * TERMINAL rather than from a post, which is what makes it bounded.
	 */
	if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_TRACK_RX) {
		api_ch[ch].track_repost = true;
	}
	if (api_search_slot != ch ||
	    api_ch[ch].slot_kind != (uint8_t)API_SLOT_SEARCH) {
		return;
	}
	/* RADIANT_SEARCH_OP_EXTERNAL: the scheduler merges channels into one
	 * hardware operation, so there's no HAL op id to give; this token tells
	 * the module to trust our own routing instead. */
	radiant_search_armed(&api_search, RADIANT_SEARCH_OP_EXTERNAL, t_open, t_close);
}

static void api_sched_done(uint8_t ch, enum radiant_sched_done why, void *user)
{
	enum radiant_radio_status st;
	radiant_time_t            now;
	uint32_t              op;
	bool                  keep_search = false;

	(void)user;
	if (!api_ch_valid(ch)) {
		return;
	}

	now = radiant_radio_now();
	st = api_done_to_status(why);

#ifdef CONFIG_RADIANT_SWEEP_DEBUG
	/* TEMPORARY, pairs with the ARM line above. */
	LOG_INF("END k=%u ch=%u why=%u heard=%u",
		(unsigned int)api_ch[ch].slot_kind, (unsigned int)ch,
		(unsigned int)why, (unsigned int)api_ch[ch].slot_heard);
#endif

#ifdef CONFIG_RADIANT_SWEEP_DEBUG
	/* Taken at the top, before any of the work below can move the clock:
	 * this instant is one end of both the arm gap and the overrun, and a
	 * terminal that spent time in radiant_search_on_done() before being
	 * stamped would charge that time to the wrong interval. */
	if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_SEARCH) {
		dbg_last_done_now = now;
		/* The chunk is over, however it ended: nothing of it is still
		 * in the future, so the report's correction stops applying. */
		dbg_inflight = false;
		if (dbg_last_close != 0u && dbg_last_close != RADIANT_TIME_NEVER) {
			dbg_overrun_us += (int64_t)now - (int64_t)dbg_last_close;
			dbg_overrun_n++;
		}
	}
#endif

	api_stats.sched_dones++;
	if (why == RADIANT_SCHED_DONE_MISSED) {
		api_stats.sched_missed++;
		ant_health_note(ANT_HEALTH_SCHED_MISSED);
	} else if (why == RADIANT_SCHED_DONE_FAILED) {
		api_stats.sched_failed++;
		ant_health_note(ANT_HEALTH_SCHED_FAILED);
	} else if (why == RADIANT_SCHED_DONE_DENIED) {
		ant_health_note(ANT_HEALTH_SCHED_DENIED);
		/* Counted from `why`: api_done_to_status() folds a denial into
		 * FAILED for the transfer engine, so this is the one point the
		 * distinction survives. */
		api_stats.sched_denied++;
	}

	if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_SEARCH) {
		/*
		 * One scan chunk has ended. OK credits the whole chunk.
		 * ABORTED means a nearer deadline (tracked channel) took the
		 * radio partway through, so credit what was actually listened
		 * to - crediting zero here used to freeze the sweep on one set.
		 * MISSED/FAILED/DENIED never opened, credit nothing.
		 *
		 * DENIED CREDITS ZERO ON PURPOSE. Under an arbiter the sweep is
		 * the elastic consumer (ADR 0013) and gets refused hundreds of
		 * times a sweep; crediting a chunk that never opened would
		 * advance the address set on a fraction of its real dwell.
		 */
		radiant_search_on_done(&api_search, why == RADIANT_SCHED_DONE_OK,
				   why == RADIANT_SCHED_DONE_ABORTED, now);
		/*
		 * A denied chunk credited zero dwell above (correct), but a
		 * channel under a finite ACQUIRE timeout has a wall-clock
		 * deadline that keeps counting regardless. Measured: a channel
		 * armed 13 times, denied 12, simply stopped - not because the
		 * device was absent but because refusals ate its listening
		 * budget. RADIANT_API_HOUSEKEEP_MS (the actual retry pacing,
		 * see api_event_thread()) is the right size for the extension.
		 */
		if (why == RADIANT_SCHED_DONE_DENIED) {
			radiant_search_note_denied(&api_search,
						RADIANT_API_HOUSEKEEP_MS * 1000u);
		}
#ifdef CONFIG_RADIANT_SWEEP_DEBUG
		switch (why) {
		case RADIANT_SCHED_DONE_OK:
			dbg_end_ok++;
			break;
		case RADIANT_SCHED_DONE_ABORTED:
			dbg_end_abort++;
			break;
		case RADIANT_SCHED_DONE_MISSED:
			dbg_end_missed++;
			break;
		case RADIANT_SCHED_DONE_DENIED:
			dbg_end_denied++;
			break;
		default:
			dbg_end_failed++;
			break;
		}
#endif

		/*
		 * A continuous request outlives its chunks (lets a displaced
		 * scan resume), so the slot is still here - whether to keep it
		 * is the whole of the sweep's throughput.
		 *
		 * Must drop when the SET finishes - the filters describe one
		 * address set, only radiant_search_window() picks the next.
		 * Must NOT drop merely because a chunk ended - that used to
		 * round-trip every chunk through the event thread (most of
		 * them, since a tracked channel ends one 4x/s). Measured on
		 * nRF54L15, one channel tracking at 4 Hz: 38.5 housekeeping
		 * passes/s but only 7.6 search windows, sets advancing at
		 * 2.5/s vs nominal 3.85/s, full sweep 12.8 s instead of 8.3 s,
		 * with preempt=0 missed=0 - nothing competing for the radio,
		 * just idle waiting on the pump.
		 *
		 * Kept only when the chunk really ran - same bound as
		 * turn_pending/track_repost, guarding the same unbounded-loop
		 * risk as api_arming above. MISSED/FAILED drop the slot and
		 * let the rate-limited pump decide.
		 */
		if (api_search_slot == ch) {
			/*
			 * DENIED counts as "ran" here, unlike the dwell credit
			 * above: it asks whether the scheduler may keep the
			 * request and arm the next chunk on its own way out,
			 * vs. being cancelled and re-posted by the pump.
			 * radiant_sched.c's EDENIED path ends the pass on a
			 * kept continuous request, so retry costs one refused
			 * arm per pass, not a loop. Excluding DENIED here would
			 * send every denied chunk through the same round trip
			 * that measured 12.8 s vs 8.3 s per sweep - the
			 * difference between surviving contention and
			 * stalling under it.
			 */
			bool ran = (why == RADIANT_SCHED_DONE_OK) ||
				   (why == RADIANT_SCHED_DONE_ABORTED) ||
				   (why == RADIANT_SCHED_DONE_DENIED);

			if (ran && !radiant_search_set_complete(&api_search) &&
			    radiant_search_is_searching(&api_search, ch)) {
				/* Re-bound below after the unconditional clear -
				 * without slot_kind SEARCH the next armed()
				 * wouldn't reach radiant_search_armed() and the
				 * sweep would stop advancing. */
				keep_search = true;
#ifdef CONFIG_RADIANT_SWEEP_DEBUG
				/* The kept path: the scheduler re-arms from
				 * inside this event, so the next arm closes
				 * gap_arm rather than gap_post. */
				dbg_kept = true;
#endif

				/*
				 * Ceiling comes down with the budget: leaving
				 * chunk_us at the whole original dwell lets
				 * the set's last chunk overrun it. Measured:
				 * 379 ms listened on a 260 ms budget, 46%
				 * slower, invisible to any counter since every
				 * chunk still ends DONE_OK honestly.
				 */
				(void)radiant_sched_rechunk(
					ch,
					radiant_search_dwell_remaining(&api_search));
			} else {
				/* Cancel is silent for this channel, so no
				 * second done() arrives; the pump posts
				 * whatever the sweep should hear next. */
				(void)radiant_sched_cancel(ch);
				api_search_slot = RADIANT_SCHED_CH_NONE;
#ifdef CONFIG_RADIANT_SWEEP_DEBUG
				/* The dropped path. From here only
				 * api_post_search_window() can re-post, and it
				 * is reachable only from api_housekeep(). This
				 * is the start of gap_pump - the interval the
				 * 50 ms tick paces. */
				dbg_slot_dropped_at = now;
#endif
			}
		}
	} else if (!radiant_transfer_is_idle(&api_xfer[ch]) &&
		   !radiant_sched_pending(ch)) {
		/* The engine hasn't re-posted, so this really is its
		 * operation ending. If it HAS re-posted (the normal path,
		 * arming the reply from inside the TX callback), the slot
		 * holds the new request and this completion belongs to the
		 * old one. */
		api_feed_xfer_terminal(ch, st);
	} else if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_MASTER_RX) {
		/*
		 * A master's turnaround window closing, deliberately nothing
		 * to do: must not touch the slot clock (already anchored by
		 * its own transmit) and must not raise RX_FAIL (an empty
		 * turnaround is the normal case 4x/s).
		 */
	} else if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_MASTER_TX &&
		   !api_ch[ch].slot_heard) {
		/*
		 * A master's slot that didn't go out - the clock must move.
		 * t_next only advances on a completed transmit, so leaving it
		 * in the past would make the pump re-post the same dead
		 * instant forever, refused synchronously each time: a hot
		 * loop with no fault and no host response.
		 *
		 * radiant_channel_on_slot_missed() advances by one period from
		 * the missed slot, not from now, keeping the master's phase
		 * its own; it counts no misses and never sends a master to
		 * search.
		 *
		 * Keyed on slot_kind and !slot_heard, not on `why` - a DENIED
		 * master TX lands here too and gets its clock advanced the
		 * same way (test_denied_master_tx_does_not_wedge asserts this
		 * directly). radiant_channel_on_slot_denied() does the same
		 * arithmetic for the honest counter.
		 */
		if (why == RADIANT_SCHED_DONE_DENIED) {
			api_stats.slots_denied++;
			(void)radiant_channel_on_slot_denied(ch, now);
		} else {
			api_stats.slots_missed++;
			(void)radiant_channel_on_slot_missed(ch, now);
		}
		/*
		 * AND THE MASTER OWES ITSELF THE NEXT SLOT, immediately - the
		 * transmit-side twin of the tracked re-post below.
		 *
		 * Same hole, other direction: a MASTER_TX that never went out
		 * left nothing posted, so the next slot came from the
		 * RADIANT_API_HOUSEKEEP_MS pump (50 ms) rather than from here.
		 * At the ANT+ period that is a fifth of a period of pure
		 * scheduling latency added to a slot that was already lost, on
		 * the leg this dongle exists to keep short - and once the pump
		 * is late enough that arm_next() has committed the radio
		 * elsewhere, the slot the clock advance just computed is missed
		 * in its turn.
		 *
		 * BOUNDEDNESS, confirmed rather than assumed, and it transfers
		 * verbatim from the tracked case: this branch is reached only
		 * after one of the two clock advances immediately above, each
		 * of which moves the master's t_next a FULL PERIOD past the
		 * slot that died - and both advance from the missed slot, not
		 * from `now`, so the advance cannot be swallowed by scheduler
		 * latency. The re-posted slot is therefore strictly later in
		 * time than the one it replaces, cannot produce another
		 * immediate terminal, and the sequence advances rather than
		 * spinning. api_arming still guards the other shape (a refused
		 * arm completing inline), which is the one that can loop.
		 *
		 * The flag, not a direct api_post_master_tx() call, so this
		 * goes through the single re-post block below with its
		 * TRACKING / !pending / transfer-idle guards.
		 */
		api_ch[ch].track_repost = true;
	} else if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_TRACK_RX &&
		   !api_ch[ch].slot_heard) {
		/*
		 * A predicted window that ran and heard nothing. Clock
		 * advances from the missed slot, not now, so scheduler latency
		 * can't walk the phase away; after eight misses the channel
		 * goes back to search and raises RX_FAIL_GO_TO_SEARCH itself.
		 *
		 * A DENIED window didn't run, so it advances through
		 * radiant_channel_on_slot_denied() instead, which charges the
		 * guard not the miss counter - contention in the other stack
		 * must not drop a live sensor back to SEARCHING. No
		 * RX_FAIL event either: that event says "nothing was there",
		 * a denial says something about us, and a host (Zwift) acting
		 * on it would tear down a channel that was never in trouble.
		 * api_stats.sched_denied/slots_denied make the denial visible
		 * instead.
		 */
		if (why == RADIANT_SCHED_DONE_DENIED) {
			api_stats.slots_denied++;
			(void)radiant_channel_on_slot_denied(ch, now);
		} else {
			api_stats.slots_missed++;
			if (!radiant_channel_on_slot_missed(ch, now)) {
				(void)radiant_event_post_channel_event(
					ch, (uint8_t)ANTW_EVENT_RX_FAIL);
			}
		}
		/*
		 * AND THE CHANNEL OWES ITSELF THE NEXT WINDOW, immediately.
		 *
		 * api_sched_armed() sets track_repost only for a window that
		 * really got armed, which is right for every terminal that
		 * follows an arm - but a window reported MISSED or DENIED may
		 * never have been armed at all, and then nothing here re-posted
		 * anything. The channel waited for the RADIANT_API_HOUSEKEEP_MS
		 * pump (50 ms), by which time arm_next() has committed the radio
		 * to something up to a quarter of a second out and the window
		 * this clock advance just computed is missed in its turn. Eight
		 * of those is RX_FAIL_GO_TO_SEARCH: a ~2 s dropout that looks
		 * exactly like an RF problem and is not one.
		 *
		 * BOUNDEDNESS, which this file demands for every post reachable
		 * from a callback: unlike a refused arm, a MISSED or DENIED
		 * terminal ALWAYS follows one of the clock advances just above,
		 * each of which moves the slot a full period past the window
		 * that died. So the re-posted window is strictly later than the
		 * one it replaces and cannot produce another immediate terminal;
		 * the sequence advances in time rather than spinning. api_arming
		 * still guards the refusal path, which is the shape that can.
		 *
		 * The flag rather than a direct api_post_track_rx() call so this
		 * goes through the single repost block below with its
		 * TRACKING / !pending / transfer-idle guards - a channel that
		 * this same callback is about to close or hand to the transfer
		 * engine must not acquire a window behind it. (That block used
		 * to carry a !master guard too; it now makes the same
		 * master/slave split api_pump_locked() does, since the missed
		 * MASTER_TX branch above sets this flag as well.)
		 */
		api_ch[ch].track_repost = true;
	}

	op = api_ch[ch].op;
	api_ch[ch].op = 0u;
	api_ch[ch].slot_kind = (uint8_t)API_SLOT_NONE;

	/*
	 * Every terminal event, including one bound to no channel: the HAL
	 * guarantees a cancelled operation's terminal still arrives, so a close
	 * that raced a window always produces one. A CLOSING channel completes
	 * here and raises ANTW_EVENT_CHANNEL_CLOSED.
	 */
	(void)radiant_channel_on_terminal(op, st, now);

	/*
	 * If the sweep's request is still in the scheduler's slot and its set
	 * isn't finished, re-bind it: the scheduler arms the next chunk on the
	 * way out of this same callback, and that arm needs slot_kind SEARCH to
	 * credit the dwell. A fresh op id, since each chunk is a separate
	 * operation and the terminal above already retired the previous one.
	 */
	if (keep_search) {
		api_bind_accepted(ch, API_SLOT_SEARCH);
	}

	/*
	 * Exception #1, bounded by construction: a listening master's
	 * turnaround needs the t_sync its own transmit achieved (only
	 * api_sched_tx() sees it) and has a 2.19 ms deadline, so it's posted
	 * here rather than from api_sched_tx() itself, which would reuse a slot
	 * still in flight and swallow this same completion (api_chan::turn_pending).
	 * Not an api_arming-style unbounded loop: the flag is cleared before the
	 * post and only set by a transmit that actually went out, so a refused
	 * arm re-enters with it already false. The TRACKING check runs after
	 * radiant_channel_on_terminal() so a channel that just closed doesn't get
	 * a turnaround armed behind it.
	 */
	if (api_ch[ch].turn_pending) {
		radiant_time_t t_sync = api_ch[ch].turn_t_sync;

		api_ch[ch].turn_pending = false;
		if (radiant_channel_state_get(ch) == RADIANT_CH_STATE_TRACKING) {
			api_post_master_rx(ch, t_sync);
		}
	}

	/*
	 * Exception #2, what keeps a background scan alive while anything is
	 * tracked. arm_next() caps a scan chunk at t_back(committed, lead), but
	 * only for work the scheduler can already see; left to the pump, a
	 * tracked slave's next window is posted a moment later from the event
	 * thread, and in that gap the scan gets armed with no end, then
	 * want_preempt() tears it straight back down when the tracked window
	 * arrives. Measured on nRF54L15, one channel tracking at 3 Hz: 109 scan
	 * chunks armed unbounded vs 6 bounded, sets_advanced down from ~4/s to
	 * 0.4/s, frames_ok frozen for 14 s - matches a Zwift capture where ch0
	 * went silent for 40 s.
	 *
	 * Rejected fix: capping chunk_us in api_post_search_window() against
	 * the soonest tracked slot - shortens the dwell even when correctly
	 * bounded already, and fails test_the_scan_keeps_finding_devices_while_
	 * another_channel_tracks (a 12 s device went unreported). The defect is
	 * a missing fact, not an over-long chunk.
	 *
	 * Bounded the same way as turn_pending: set only by api_sched_armed() on
	 * a window that really armed, or by a MISSED/DENIED terminal that has
	 * just advanced the slot clock a full period; cleared before the post.
	 *
	 * BOTH DIRECTIONS NOW. The flag started life as the tracked slave's
	 * re-post and the guard read !master, which silently made the transmit
	 * side wait on the 50 ms pump. The two-way split below is
	 * api_pump_locked()'s, spelled the same way and in the same order
	 * (master -> api_post_master_tx(), slave -> api_post_track_rx()), so a
	 * terminal's re-post and the pump's post can never disagree about what
	 * a channel should be doing next. Nothing else changed: track_repost is
	 * still only ever set on an API_SLOT_TRACK_RX or API_SLOT_MASTER_TX
	 * slot, and those two kinds are exactly the two arms of this if.
	 */
	if (api_ch[ch].track_repost) {
		api_ch[ch].track_repost = false;
		if (radiant_channel_state_get(ch) == RADIANT_CH_STATE_TRACKING &&
		    !radiant_sched_pending(ch) &&
		    radiant_transfer_is_idle(&api_xfer[ch])) {
			if (radiant_channel_is_master(ch)) {
				api_post_master_tx(ch);
			} else {
				api_post_track_rx(ch);
			}
		}
	}

	/*
	 * Do NOT post anything else from here: against a backend that refuses
	 * every arm, posting from a radio callback is an unbounded loop.
	 * Everything but the transfer engine's one hard deadline waits for the
	 * next pass.
	 */

	/*
	 * A denial does not wake the event thread, or the whole dongle spins.
	 * Every other completion gives the thread something to do; a denial
	 * doesn't, and the refused request is deliberately still in the
	 * scheduler's slot (`ran` above). Waking the thread would just pump,
	 * post nothing, tick, get refused, and complete back into here.
	 * Measured against an arbiter refusing every arm: 510,000 housekeeping
	 * passes and 510,000 denials in 19 s, with the logging ring dropping
	 * the very diagnostics that would explain why. api_arming doesn't cover
	 * this shape (post -> arm -> refuse -> WAKE -> pump, not -> POST), so
	 * the bound is here instead; housekeeping's own cadence still retries a
	 * few times a second.
	 *
	 * Only when the slot was kept (keep_search): skipping the wake for
	 * every denial regressed test_a_denied_master_transmit_does_not_wedge -
	 * a denied master TX has its request consumed and needs the pump to
	 * re-post it, so it still needs the wake.
	 */
	if (why == RADIANT_SCHED_DONE_DENIED && keep_search) {
		return;
	}

	k_sem_give(&api_event_sem);
}

static const struct radiant_sched_cbs api_sched_cbs = {
	.rx = api_sched_rx,
	.tx = api_sched_tx,
	.armed = api_sched_armed,
	.done = api_sched_done,
};

/* ---------------------------------------------------------------------------
 * The event thread
 * ---------------------------------------------------------------------------
 */

/* Is this transfer state one the radio is supposed to finish? WAIT_BLOCK is
 * not - it's waiting on the host, and terminating it here would release a
 * block the bridge still owns (forbidden by B1-B5). IDLE is not, trivially. */
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
 * The two recovery watchdogs. Callers hold api_lock. Neither should ever fire
 * on a healthy link; they exist because the failures they cover are permanent
 * and silent - a transfer engine with no completion coming never leaves its
 * state, and the channel just goes quiet with no event, error, or log line.
 */
static void api_watchdogs_locked(radiant_time_t now)
{
	const radiant_time_t limit =
		(radiant_time_t)RADIANT_API_XFER_WATCHDOG_MS * 1000u;
	uint8_t ch;

	for (ch = 0u; ch < API_CHANNELS; ch++) {
		struct api_chan *c = &api_ch[ch];

		/* H2. Non-idle with nothing armed means no done() is coming -
		 * api_feed_xfer_terminal() is only reachable from api_sched_done(). */
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

		/* in_pkts only clears on LAST, so a burst whose final packet is
		 * missed leaves it non-zero forever, mislabeling later
		 * acknowledged messages as burst data. */
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

	/*
	 * The noise floor, once per interval, per frequency. USB 3.0 broadband
	 * noise desensing the receiver 10-20 dB is otherwise indistinguishable
	 * from a flat sensor battery; this line makes it a measurement. INF not
	 * DBG so it's readable on an ordinary bench log. Cleared after
	 * reporting so each line describes its own interval.
	 */
	{
		static radiant_time_t noise_last;
		uint8_t slot;

		if (now - noise_last >= API_NOISE_REPORT_US) {
			noise_last = now;
			for (slot = 0u; slot < RADIANT_NOISE_SLOTS; slot++) {
				struct radiant_noise_report r;

				if (!radiant_noise_get(slot, &r)) {
					continue;
				}
				LOG_INF("noise rf=%u n=%u floor=%d busy=%d "
					"min=%d max=%d clip=%u/%u lost=%u",
					(unsigned int)r.rf_index,
					(unsigned int)r.samples, (int)r.floor_dbm,
					(int)r.busy_dbm, (int)r.min_dbm,
					(int)r.max_dbm,
					(unsigned int)r.below_range,
					(unsigned int)r.above_range,
					(unsigned int)radiant_noise_unslotted());
				radiant_noise_clear(slot);
			}
		}
	}

	/*
	 * THE RECEIVE PATH, one line per interval, and only when it moved - a
	 * quiet dongle must not fill the console with zeros. Reports the four
	 * stage counters plus what reached the host, so the hop that multiplies
	 * is the one whose successor is larger than it. See the comment on
	 * struct radiant_api_stats::rx_sched_events.
	 */
	{
		static radiant_time_t rxpath_last;
		static uint32_t       rxpath_seen;

		if (now - rxpath_last >= API_RXPATH_REPORT_US &&
		    api_stats.rx_sched_events != rxpath_seen) {
			/*
			 * Reported as a DELTA over a measured interval, not as a
			 * running total: the counters are cumulative since boot,
			 * and a total spanning several bench runs reads as one
			 * enormous flood no matter how quiet the current run is.
			 * That very confusion cost a reading here already.
			 */
			uint32_t dt_ms = (uint32_t)((now - rxpath_last) / 1000u);

			LOG_INF("rxpath/%ums sched=%u tracked=%u on_data=%u "
				"up=%u posted=%u | xfer=%u dropped=%u tot=%u",
				(unsigned int)dt_ms,
				(unsigned int)(api_stats.rx_sched_events - rxpath_seen),
				(unsigned int)api_stats.rx_tracked_frames,
				(unsigned int)api_stats.rx_on_data,
				(unsigned int)api_stats.rx_data_up,
				(unsigned int)api_stats.rx_posted,
				(unsigned int)api_stats.rx_to_xfer,
				(unsigned int)api_stats.rx_dropped,
				(unsigned int)api_stats.rx_sched_events);
			rxpath_last = now;
			rxpath_seen = api_stats.rx_sched_events;
		}
	}

#if defined(CONFIG_RADIANT_CRC_REPAIR)
	/*
	 * W5/F2: the CRC-repair counters, on a UART a person can read.
	 *
	 * This is a hard prerequisite of the S3 sensitivity A/B rather than a
	 * convenience. That sitting compares the knee of a dB ladder with
	 * repair on against the knee with it off, and its stated decision rule
	 * demands `crc_repaired` be climbing AT the knee before a difference in
	 * knees may be attributed to repair. Without a read path the honest
	 * outcome of a null result is unreadable: "repair did nothing" and
	 * "repair never engaged, so the run measured nothing" produce the same
	 * two numbers, and only the first of those is a result.
	 *
	 * Cumulative, not per-interval - unlike the noise line above, which
	 * clears. A rate is what matters here, and the sitting takes it as a
	 * delta across a run, so clearing would fight the tool that reads it.
	 *
	 * Compiled out entirely with the feature, so the default image carries
	 * neither the timestamp nor the call.
	 */
	{
		static radiant_time_t crc_last;

		if ((radiant_time_t)(now - crc_last) >= API_CRC_REPORT_US) {
			crc_last = now;
			LOG_INF("crc repaired=%u unrepairable=%u refuted=%u "
				"implausible=%u",
				(unsigned int)api_stats.crc_repaired,
				(unsigned int)api_stats.crc_unrepairable,
				(unsigned int)api_stats.crc_repair_refuted,
				(unsigned int)api_stats.crc_repair_implausible);
		}
	}
#endif /* CONFIG_RADIANT_CRC_REPAIR */

#ifdef CONFIG_RADIANT_SWEEP_DEBUG
	/* Sweep rate is invisible from the host; both discovery defects fixed
	 * in this file were found with these counters. */
	{
		static radiant_time_t dbg_last;

		if ((radiant_time_t)(now - dbg_last) > 1000000u) {
			const struct radiant_search_stats *ss =
				radiant_search_get_stats(&api_search);
			const struct radiant_sched_stats *ds =
				radiant_sched_stats_get();

			/*
			 * The identity. Everything on the right is a measured
			 * interval; unacct is what none of them claimed, and it
			 * is the only number on this line that is supposed to
			 * be zero. A negative unacct means double-counting -
			 * most likely a tracked window armed inside a scan
			 * chunk, which is legal and would mean track_us must
			 * come out of the sum rather than into it.
			 */
			int64_t elapsed = (dbg_epoch != 0u)
						  ? (int64_t)(now - dbg_epoch)
						  : 0;
			/*
			 * track_us IS DELIBERATELY NOT IN THIS SUM, and that is
			 * a measurement rather than a preference.
			 *
			 * A tracked window PREEMPTS a running scan chunk - that
			 * is the whole reason the sweep is posted as a
			 * continuous background scan rather than a bounded
			 * window. So a tracked window is armed INSIDE the span
			 * a scan chunk has already been credited for, and
			 * adding it counts the same microseconds twice.
			 *
			 * Measured: with it in the sum, unacct settled at a
			 * smooth -3200 us per second of elapsed - always
			 * negative, which is the signature of double counting
			 * rather than of missing time. 3200 us/s against a
			 * tracked window of ~640 us arriving at 4 Hz (2560
			 * us/s) is the same quantity. Taking it out is what
			 * makes the residual zero.
			 *
			 * The counter itself stays on the SWEEP line: knowing
			 * tracked windows cost 640 us each is what ruled them
			 * out as the cause of a starved sweep in the first
			 * place. It is not part of the identity, that is all.
			 */
			int64_t accounted = (int64_t)dbg_scan_us +
					    (int64_t)dbg_gap_arm_us +
					    (int64_t)dbg_gap_pump_us +
					    (int64_t)dbg_gap_post_us +
					    dbg_overrun_us;
			int64_t unacct;

			/* Take back the part of the in-flight chunk that has
			 * not elapsed yet - see dbg_inflight. Without this the
			 * residual alternates sign with the sample phase and
			 * never settles at zero, which is the one thing this
			 * whole line exists to show. */
			if (dbg_inflight && dbg_inflight_close > now) {
				accounted -= (int64_t)(dbg_inflight_close - now);
			}

			/*
			 * And add the pump gap that is STILL RUNNING. gap_pump
			 * is credited in one lump when the pump finally posts,
			 * so between the slot being dropped and that post -
			 * which measured 1.7 s, longer than the report interval
			 * - elapsed runs ahead of everything claiming it, and
			 * unacct saws up to a full second and back.
			 *
			 * That sawtooth is the mirror image of the in-flight
			 * chunk above: one is time credited before it happened,
			 * the other is time that happened before it was
			 * credited. Both have to be corrected or the residual
			 * is dominated by which side of a boundary the 1 s
			 * report landed on, and the real remainder cannot be
			 * seen at all.
			 */
			if (dbg_slot_dropped_at != 0u && now > dbg_slot_dropped_at) {
				accounted += (int64_t)(now - dbg_slot_dropped_at);
			}
			unacct = elapsed - accounted;

			dbg_last = now;
			LOG_INF("SWEEP set=%u steer=%u dwell=%u/%u adv=%u sweeps=%u "
				"win=%u ok=%u searching=%u carrier=%d | chunks=%u "
				"preempt=%u missed=%u ebusy=%u rej=%u | pumps=%u "
				"hk=%u | scan_us=%u/%u track_us=%u/%u | "
				"end ok=%u abrt=%u miss=%u fail=%u deny=%u "
				"replan=%u",
				(unsigned int)api_search.cur_set,
				(unsigned int)api_search.n_steer,
				(unsigned int)api_search.set_dwell_us,
				(unsigned int)api_search.cfg.dwell_us,
				(unsigned int)ss->sets_advanced,
				(unsigned int)ss->sweeps,
				(unsigned int)ss->windows,
				(unsigned int)ss->frames_ok,
				(unsigned int)radiant_search_n_searching(&api_search),
				(int)api_search_slot,
				(unsigned int)ds->scan_chunks,
				(unsigned int)ds->preempted,
				(unsigned int)ds->missed,
				(unsigned int)ds->arm_ebusy,
				(unsigned int)ds->arm_rejected,
				(unsigned int)api_stats.pumps,
				(unsigned int)api_stats.housekeeps,
				(unsigned int)dbg_scan_us,
				(unsigned int)dbg_scan_n,
				(unsigned int)dbg_track_us,
				(unsigned int)dbg_track_n,
				(unsigned int)dbg_end_ok,
				(unsigned int)dbg_end_abort,
				(unsigned int)dbg_end_missed,
				(unsigned int)dbg_end_failed,
				(unsigned int)dbg_end_denied,
				(unsigned int)ds->replans);

			/*
			 * A second line rather than a longer first one: the
			 * SWEEP line is already at the edge of what a 128-byte
			 * deferred log message can carry, and a truncated
			 * identity is worse than none - it reads as a number.
			 */
			LOG_INF("SWEEP why arm=%u slot=%u nochan=%u"
				"(idle=%u busy=%u) win=%u ok=%u esc=%u",
				(unsigned int)dbg_pump_arming,
				(unsigned int)dbg_pump_slot,
				(unsigned int)dbg_pump_nochan,
				(unsigned int)dbg_pump_none_searching,
				(unsigned int)dbg_pump_busy,
				(unsigned int)dbg_pump_window,
				(unsigned int)dbg_pump_ok,
				API_DBG_DENIAL_ESCAPES(ss));
			LOG_INF("SWEEP gap arm=%u/%u pump=%u/%u post=%u/%u "
				"over=%lld/%u | elapsed=%lld acct=%lld "
				"unacct=%lld",
				(unsigned int)dbg_gap_arm_us,
				(unsigned int)dbg_gap_arm_n,
				(unsigned int)dbg_gap_pump_us,
				(unsigned int)dbg_gap_pump_n,
				(unsigned int)dbg_gap_post_us,
				(unsigned int)dbg_gap_post_n,
				(long long)dbg_overrun_us,
				(unsigned int)dbg_overrun_n,
				(long long)elapsed, (long long)accounted,
				(long long)unacct);
		}
	}
#endif

	k_mutex_unlock(&api_lock);
}

static void api_event_thread(void *a, void *b, void *c)
{
	(void)a;
	(void)b;
	(void)c;

	for (;;) {
		uint32_t n;

		/* Timeout drives housekeeping: radiant_sched.c owns no timer, so
		 * a request that lost an arm race has nothing else to retry it. */
		(void)k_sem_take(&api_event_sem, K_MSEC(RADIANT_API_HOUSEKEEP_MS));

		/*
		 * All RX crypto, run before the drain: the pump turns queued
		 * ciphertext into posted plaintext, so a packet is in the ring
		 * before the drain delivers it. Outside api_lock, like the
		 * drain - radiant_sec_deliver() posts to the event ring, which
		 * radiant_event.c protects with its own critical section.
		 */
		radiant_sec_pump();

		/* Thread context only, deliberately outside api_lock: the drain
		 * calls antr_on_message() (the bridge's), and holding a lock
		 * across that is how a deadlock gets built. */
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
	memset(api_net_valid, 0, sizeof(api_net_valid));
	memset(api_adv_burst, 0, sizeof(api_adv_burst));
	memset(api_sdu_mask, 0, sizeof(api_sdu_mask));
	memset(api_crypto_id, 0, sizeof(api_crypto_id));
	memset(api_crypto_user, 0, sizeof(api_crypto_user));
	memset(api_crypto_key, 0, sizeof(api_crypto_key));

	for (i = 0u; i < API_CHANNELS; i++) {
		memset(&api_xfer[i], 0, sizeof(api_xfer[i]));
	}

	/* Keys are write-only and do not survive a reset, so epoch monotonicity
	 * can only be enforced within one power cycle. */
	radiant_sec_reset();
#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
	/* And every half-finished exchange, so no scalar is left in RAM behind
	 * an unreachable state. */
	radiant_sec_pair_reset();
#endif

	api_search_slot = RADIANT_SCHED_CH_NONE;
}

antr_err_t antr_init(void)
{
	struct radiant_search_cfg scfg;
	int                   rc;

	api_reset_state();
	memset(&api_stats, 0, sizeof(api_stats));

	/*
	 * Order matters; each step preconditions the next. radiant_event_init()
	 * first, before the radio is enabled: posting before it returns is
	 * RADIANT_EVENT_ESTATE, mirroring antr_on_message() never firing before
	 * antr_init() returns 0.
	 */
	radiant_channel_init();
	radiant_event_init();

	/* The scheduler owns radio events (it's the only thing that arms), not
	 * the radio lifecycle, which stays this file's. */
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

	/*
	 * The adaptive guard is only as good as its t_sync. When the HAL says
	 * that's an inference rather than a hardware capture, the floor is
	 * pinned at the ceiling and the mechanism switches itself off - no
	 * narrowed window sized from a number that doesn't mean what it says.
	 * Otherwise the floor is the core minimum, raised by two ticks of the
	 * backend's own timebase granularity.
	 */
	{
		const struct radiant_radio_caps *caps = radiant_radio_caps_get();
		uint32_t floor_us = RADIANT_CHANNEL_GUARD_MIN_US;

		if (caps == NULL || !caps->has_sync_timestamp) {
			floor_us = RADIANT_CHANNEL_GUARD_MAX_US;
		} else {
			floor_us += 2u * (((uint32_t)caps->time_resolution_ns +
					   999u) / 1000u);
		}
		radiant_channel_guard_floor_set((uint16_t)floor_us);
		LOG_DBG("tracked-window guard: floor %u us, ceiling %u us",
			(unsigned int)floor_us,
			(unsigned int)RADIANT_CHANNEL_GUARD_MAX_US);
	}

	api_crc_repair_setup();

	api_search_cfg_build(&scfg);
	rc = radiant_search_init(&api_search, &scfg, &api_search_cbs, NULL);
	if (rc != RADIANT_SEARCH_OK) {
		/* RADIANT_SEARCH_ENOTSUP: the backend advertises a wildcard
		 * device-number filter that struct radiant_rx_filter has no
		 * field for. Fail loudly rather than fake the capability. */
		LOG_ERR("radiant_search_init: %d", rc);
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	api_inited = true;
#if defined(CONFIG_RADIANT_SWI)
	/*
	 * Claim the trampoline before the event thread exists, and so before
	 * anything can post: radiant_swi_pend() drops silently until the line is
	 * claimed, and that window must not overlap a live receiver. Failure is
	 * fatal to arbitrated operation rather than cosmetic - every wakeup on
	 * the completion path goes through it - so it is reported, not ignored.
	 */
	{
		int swi_rc = radiant_swi_register(RADIANT_SWI_EVENT_WAKEUP,
						  event_wakeup_swi);

		if (swi_rc != 0) {
			LOG_ERR("radiant_swi_register: %d", swi_rc);
			return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
		}
	}
#endif
	(void)k_thread_create(&api_event_thread_data, api_event_stack,
			      K_THREAD_STACK_SIZEOF(api_event_stack),
			      api_event_thread, NULL, NULL, NULL,
			      API_EVENT_PRIORITY, 0, K_NO_WAIT);
	(void)k_thread_name_set(&api_event_thread_data, "radiant_event");

	LOG_INF("radiant up: %u channels, %u filters/window, %u search sets",
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

	/* radiant_radio_abort() before radiant_channel_reset_all(): radiant_channel.c
	 * can't abort an operation itself, so tearing its state down first
	 * would leave the backend pointing at a channel that no longer exists. */
	radiant_sched_reset();
	(void)radiant_radio_abort();

	for (i = 0u; i < API_CHANNELS; i++) {
		(void)radiant_transfer_abort(&api_xfer[i]);
		(void)radiant_search_end(&api_search, i);
	}

	radiant_channel_reset_all();

	/* Discard the queue rather than draining it: draining here would call
	 * antr_on_message() recursively from inside an antr_* call, which
	 * docs/sdk-ant-contract.md forbids. The bridge's own discard window
	 * absorbs any channel-closed events the reset raised. */
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

	/*
	 * An in-range network whose address was never installed is WARNED
	 * ABOUT AND ACCEPTED. It used to be refused with
	 * ANTW_INVALID_NETWORK_NUMBER, and that refusal - correct on its face -
	 * was a worse bug than the silence it replaced.
	 *
	 * Measured on real hardware against real Zwift, 2026-08-18
	 * (captures/zwift-20260818-144612.pcap, decoded with
	 * tools/decode_pcap.py). Zwift installs three network keys at startup,
	 * only the ANT+ one is accepted here, and it then cycles its wildcard
	 * scan channel across networks 0, 1 and 2 every ~4-5 s:
	 *
	 *     t=21.0  ch0 background-scan assigned on network 0, opens, works
	 *     t=24.7  power meter #20329 found and tracked on ch1
	 *     t=26.0  Zwift closes and unassigns ch0 cleanly
	 *     t=26.05 assign ch0 on NETWORK 1 -> ANTW_INVALID_NETWORK_NUMBER
	 *     t=30+   CLOSE_CHANNEL 00 x5385 at ~40/s for the remaining 127 s,
	 *             each answered CHANNEL_IN_WRONG_STATE, and no further
	 *             ASSIGN or OPEN for the rest of the session
	 *
	 * Zwift has no recovery path for a failed assign: it spins on close
	 * forever and never rebuilds the scan channel, so the session ends with
	 * exactly one ANT+ device ever discovered and every other sensor
	 * invisible. That is the "it only finds one device / the strap only
	 * shows up over Bluetooth" report.
	 *
	 * Accepting is also the honest answer, not merely the tolerable one.
	 * The sweep in radiant_search.c matches on api_net_addr[0]
	 * unconditionally, so a background scan assigned to a keyless network
	 * still reports ANT+ devices normally - Zwift's network 1 and 2 cycles
	 * are productive, not wasted. Only a channel that leaves scan mode and
	 * TRACKS pays for the zero address, by hearing nothing; Zwift assigns
	 * its tracking channels to network 0, so it never does.
	 *
	 * The diagnostic that motivated the refusal is kept - LOG_WRN here plus
	 * api_stats.key_rejects / ANT_HEALTH_KEY_REJECT at the point the key is
	 * turned away. Both are visible to us without being fatal to the host,
	 * which is the property the refusal lacked.
	 *
	 * RULE, learned the expensive way: returning a correct error to a host
	 * that has no recovery path for it is a regression. Prefer a warning we
	 * can read over an error the host cannot act on.
	 *
	 * The bounds test stays with radiant_channel_assign(), which already
	 * answers it: an out-of-range network number is a different mistake and
	 * is still refused there.
	 */
	if (network > 0u && network < API_NETWORKS && !api_net_valid[network]) {
		LOG_WRN("ch%u assigned to network %u, which has no address "
			"installed - a background scan still works (the sweep "
			"uses network 0), but tracking on it will hear nothing",
			channel, network);
	}

	rc = radiant_channel_assign(channel, type, network, ext_assign);
	if (rc == RADIANT_CH_OK) {
		/* Assignment resets every config byte to default, so this
		 * file's own bytes (broadcast payload, ID list) go with them. */
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
		/* Keys and derived values go with the channel, or they'd stay
		 * installed on a number the host is free to reassign. */
		radiant_sec_channel_release(channel);
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
			/* radiant_transfer_init() refuses a backend too slow for
			 * RADIANT_TRANSFER_REPLY_US. Open still succeeds
			 * (broadcast and tracking still work), but acknowledged
			 * data/burst will answer CHANNEL_IN_WRONG_STATE. */
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
			/* This channel carried the sweep and its request is
			 * about to be cancelled silently, so nothing else tells
			 * radiant_search.c the window is over. Credit nothing,
			 * so the same set is retried rather than skipped. */
			radiant_search_on_done(&api_search, false, false, 0u);
			api_search_slot = RADIANT_SCHED_CH_NONE;
		}

		/* radiant_sched_cancel() is silent for the channel it cancels,
		 * so no done() arrives; feeding the terminal here is what
		 * raises ANTW_EVENT_CHANNEL_CLOSED exactly once. */
		(void)radiant_sched_cancel(channel);
		api_ch[channel].op = 0u;
		api_ch[channel].slot_kind = (uint8_t)API_SLOT_NONE;
		/* Per-channel state that must not outlive the channel: a
		 * pending turnaround, a half-received burst, both watchdogs. */
		api_ch[channel].turn_pending = false;
		api_ch[channel].in_pkts = 0u;
		api_ch[channel].in_last_valid = false;
		api_ch[channel].xfer_stuck_valid = false;
		(void)radiant_channel_on_terminal(op, RADIANT_RADIO_STATUS_ABORTED,
					      radiant_radio_now());

		api_pump_locked();

#if defined(CONFIG_RADIANT_CRC_REPAIR)
		/*
		 * W5/F2's on-close half. The periodic line in api_housekeep()
		 * can miss the end of a short rung by up to its interval, and
		 * the last few seconds before a close are exactly the ones an
		 * A/B rung is made of. Cumulative across all channels - the
		 * stats struct has no per-channel breakdown - so read it as a
		 * delta against the previous line, not as this channel's total.
		 */
		LOG_INF("crc at close ch=%u repaired=%u unrepairable=%u "
			"refuted=%u implausible=%u",
			(unsigned int)channel,
			(unsigned int)api_stats.crc_repaired,
			(unsigned int)api_stats.crc_unrepairable,
			(unsigned int)api_stats.crc_repair_refuted,
			(unsigned int)api_stats.crc_repair_implausible);
#endif /* CONFIG_RADIANT_CRC_REPAIR */
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
	/* Device-wide, unmodeled by any module. Accepting enable == 0 (the
	 * default state) is honest; accepting non-zero would be a host
	 * believing it configured a spacing it did not get. */
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
	 * The whole table: the published ANT+ pair, and
	 * ANTW_INVALID_PARAMETER_PROVIDED for anything else - not a to-do, see
	 * the file header. May be extended by observing RF emissions from a
	 * shipping master (licence position permitting), never by fitting the
	 * function.
	 */
	if (memcmp(key, api_ant_plus_key, sizeof(api_ant_plus_key)) != 0) {
		api_stats.key_rejects++;
		ant_health_note(ANT_HEALTH_KEY_REJECT);
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	api_net_addr[network][0] = RADIANT_NET_ADDR_ANT_PLUS_0;
	api_net_addr[network][1] = RADIANT_NET_ADDR_ANT_PLUS_1;
	/* The only place this is ever set. Everything above this line can fail,
	 * and each failure path returns without reaching here. */
	api_net_valid[network] = true;

	/* The sweep's address is fixed at radiant_search_init(); re-initialising
	 * would drop every channel already in the search. Hosts install keys
	 * before opening channels, so this is normally free. */
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
	/* 8 is required, 11 is the whole thing; rejected rather than padded so
	 * a bridge bug passing the wrong slice doesn't look correct. */
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

	/* Two different lengths; the caller's buffer is sized off request_type
	 * alone, so writing the wrong one shifts a field by one, invisibly. */
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
	/* Two codes, one per argument, observable from the host: an
	 * out-of-range channel is INVALID_MESSAGE, an out-of-range mask is
	 * INVALID_PARAMETER_PROVIDED. Ack-data bit masked off before the check. */
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
	/* A receive-only channel has no transmitter to queue for. Compare the
	 * whole top nibble - masking only the master bit would make
	 * MASTER_TX_ONLY (0x50) read as SLAVE_RX_ONLY (0x40). */
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

	/* Copied before this returns - unlike antr_burst_tx(), no ownership
	 * transfer. One payload per slot: queueing a second before the slot
	 * fires replaces the first. */
	memcpy(api_ch[channel].bcast, data, ANTW_ANT_MAX_PAYLOAD_SIZE);
	api_ch[channel].bcast_valid = true;
	api_ch[channel].bcast_pending = true;

	api_pump_locked();
	k_mutex_unlock(&api_lock);

	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

/*
 * THE TRANSMIT INSTANT OF AN ORIGINATED TRANSFER, and the two-session story of
 * getting it wrong. Written out because the first correction was reverted on a
 * bad measurement and the next reader needs to know why it is back.
 *
 * radiant_transfer.h states the contract at radiant_transfer_submit():
 *
 *   "A slave answering its master passes master_t_sync +
 *    RADIANT_TRANSFER_SLOT_REPLY_US; a master passes its own slot instant."
 *
 * Both call sites below passed RADIANT_TIME_NEVER, which radiant_burst.c
 * resolves to `radiant_radio_now() + min_lead_us + ARM_SLACK_US`: whenever the
 * host happened to ask, bearing no relation to the channel's grid. An ANT peer
 * only listens in a window a few hundred microseconds wide, so a packet placed
 * at an arbitrary phase of a 250 ms period is heard essentially never.
 *
 * THE FIRST ATTEMPT WAS REVERTED ON A MISREAD MEASUREMENT (2026-08-17,
 * docs/treadmill-reference-design.md SS7.7). An A/B of exactly this change
 * appeared to show the packet "arriving at the full channel rate in both
 * arms", concluding the transmit instant was irrelevant and the fault lay in
 * the return acknowledgement. The instrument was counting the receiving
 * master's OWN slots alongside inbound data, and ~219 in 55 s is 4 Hz - the
 * channel period, not a delivery rate. Re-run with the two counters separated:
 *
 *   master originates 20 acknowledged packets over 40 s, 160 of its own slots
 *   the slave hears 150 broadcasts from it  -> the link is fine
 *   the slave hears 0 acknowledged packets  -> it is not a lost ACK
 *   the sender fails 0-16 ms after queueing -> it transmitted, then heard
 *                                              nothing in its reply window
 *
 * A lost acknowledgement and an unheard data packet look identical from the
 * originator (both are FAIL_NO_ACK). They are only distinguishable from the
 * far end, which is why every earlier session went round this loop.
 *
 * So the instant is resolved from the channel's own grid. The cost the revert
 * was worried about - "up to one channel period of latency" - is real and is
 * the correct price: an acknowledged transfer is a slotted exchange, and there
 * is no such thing as sending one early.
 */
static radiant_time_t api_xfer_t_sync_for(uint8_t ch)
{
	const struct radiant_radio_caps *caps = radiant_radio_caps_get();
	radiant_time_t                   slot = radiant_channel_next_slot(ch);
	radiant_time_t                   earliest;
	uint16_t                         period_counts = 0u;
	uint32_t                         lead;

	/*
	 * No grid to sit on - a channel that is not on air, or a master that
	 * has not transmitted yet. RADIANT_TIME_NEVER keeps the old
	 * "as soon as the backend can" behaviour, which is the honest answer
	 * when there is no slot to align to rather than a silent failure.
	 */
	if (slot == RADIANT_TIME_NEVER) {
		return RADIANT_TIME_NEVER;
	}

	/*
	 * A slave transmits in the reply window its master opens, not on the
	 * master's slot itself. radiant_channel_next_slot() gives a slave the
	 * opening edge of its next receive window, i.e. the master's expected
	 * t_sync, so the reply is that plus the measured 2190 us turnaround.
	 */
	if (!radiant_channel_is_master(ch)) {
		slot += (radiant_time_t)RADIANT_TRANSFER_SLOT_REPLY_US;
	}

	/*
	 * And it has to be far enough out to arm. The next slot can be
	 * microseconds away when the host writes just after one, and
	 * radiant_transfer_submit() does NOT clamp an explicitly requested
	 * instant for a starting transfer - the scheduler would refuse it with
	 * ETIME, which reaches the host as TRANSFER_IN_ERROR on a channel that
	 * is working perfectly. One period later is the same phase, so slipping
	 * costs a period and changes nothing else.
	 */
	lead = (caps != NULL) ? (uint32_t)caps->min_arm_lead_us : 0u;
	earliest = radiant_radio_now() + (radiant_time_t)lead +
		   (radiant_time_t)RADIANT_TRANSFER_ARM_SLACK_US;

	if (slot < earliest &&
	    radiant_channel_period_get(ch, &period_counts) == RADIANT_CH_OK &&
	    period_counts != 0u) {
		radiant_time_t period = radiant_channel_counts_to_us(period_counts);
		unsigned int   guard;

		/*
		 * A loop, not one addition: at a short period (4 Hz is the slow
		 * case, some profiles run at 32 Hz) one period may still not
		 * clear the arm lead.
		 *
		 * Explicitly bounded, because radiant_time_t is a wrapping
		 * microsecond counter and `slot < earliest` is therefore not a
		 * true ordering across the wrap - near it, the comparison can
		 * stay true for thousands of additions. Sixteen periods is far
		 * more than the two or three this can legitimately need, and
		 * falling out of the loop leaves an instant the scheduler will
		 * refuse honestly rather than one this function span for.
		 */
		for (guard = 0u; guard < 16u && slot < earliest; guard++) {
			slot += period;
		}
	}

	return slot;
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
	rc = radiant_transfer_ack_data(&api_xfer[channel], data, size,
				   api_xfer_t_sync_for(channel));
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
	/* Until advanced burst is enabled, reject anything over 8 - a dongle
	 * that accepts 24-byte blocks but can never send one is worse than one
	 * that refuses them. api_adv_burst[0] is the enable byte. */
	if (size > 8u && api_adv_burst[0] == 0u) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (!radiant_channel_is_open(channel)) {
		return (antr_err_t)ANTW_CHANNEL_NOT_OPENED;
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	/*
	 * Segment byte forwarded untouched (the BUILD_ASSERTs at the top keep
	 * RADIANT_TRANSFER_SEG_* == ANTR_BURST_SEGMENT_*). On any non-zero
	 * return the engine owns nothing and raises no event for this block
	 * (B2) - the bridge already released its semaphore synchronously, so
	 * doing it again here would let the next host packet overwrite a
	 * buffer the radio is still transmitting from (B5).
	 *
	 * The START block is slot-aligned for the same reason as
	 * antr_acknowledge_message_tx() above; see the note there. Only the
	 * START block - once a burst is running, every later packet is placed
	 * by radiant_burst.c from the acknowledgement that preceded it
	 * (ack t_sync + NEXT_PACKET_US), which is a tighter grid than this one
	 * and must not be overridden. radiant_transfer_submit() ignores the
	 * instant for a continuation anyway, but passing a slot here would be
	 * misleading to read.
	 */
	rc = radiant_transfer_burst_block(&api_xfer[channel], data, (uint8_t)size,
				      segment,
				      (segment & ANTR_BURST_SEGMENT_START) != 0u
					      ? api_xfer_t_sync_for(channel)
					      : RADIANT_TIME_NEVER);
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

	/* Outcome reported through `success`, not the return value: "nothing to
	 * clear" is not an error. */
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

	/* API_CHANNELS is what radiant_channel.c/radiant_sched.c/radiant_search.c are
	 * all sized for (asserted at the top of this file) and the protocol's
	 * own ceiling, since the burst header's channel field is five bits. */
	capabilities[ANTW_CAPABILITIES_OFFSET_MAX_CHANNELS] = API_CHANNELS;
	capabilities[ANTW_CAPABILITIES_OFFSET_MAX_NETWORKS] = API_NETWORKS;
	capabilities[ANTW_CAPABILITIES_OFFSET_STANDARD_OPTIONS] = 0x00u;

	/*
	 * ANTW_CAPABILITIES_SERIAL_NUMBER_ENABLED is DELIBERATELY NOT SET, and
	 * removing it is a bug fix rather than a reduction.
	 *
	 * The bit means "the device has a readable serial number", and
	 * MESG_GET_SERIAL_NUM (0x61) is not bridged - it is answered
	 * ANTW_INVALID_MESSAGE, pinned that way for every backend in
	 * archive/captures/serial/*.antser (case 61-get-serial-num/valid, reply
	 * a40340006128ae). Advertising a capability that answers INVALID_MESSAGE
	 * is the exact trap docs/gotchas.md names, and it is not hypothetical
	 * here: a live capture of real Zwift shows it requesting 0x61 during
	 * startup and being refused (captures/zwift-20260818-144612.pcap,
	 * `a4024d00618a` at t=0, answered `a40340004d2882`).
	 *
	 * The sdk_ant backend - the reference this one is A/B'd against - does
	 * NOT set this bit either: its recorded capabilities advanced-options
	 * byte is 0xb2 against radiant's 0xba, and 0x08 is the whole difference.
	 * So this was radiant claiming something the stack it emulates never
	 * claimed.
	 *
	 * Setting it back requires implementing 0x61 first, and that needs the
	 * reply's LEN byte, which is recorded NOWHERE in this tree (K1: the
	 * yaml's payload_len 0 describes the request). Guessing a length for a
	 * message the dongle originates is the thing ant_serial_bridge.c's
	 * advanced-burst arm already refuses to do.
	 */
	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS] =
		(uint8_t)(ANTW_CAPABILITIES_NETWORK_ENABLED |
			  ANTW_CAPABILITIES_PER_CHANNEL_TX_POWER_ENABLED |
			  ANTW_CAPABILITIES_LOW_PRIORITY_SEARCH_ENABLED |
			  /* The device ID list really is honoured - consulted
			   * at acquisition in api_id_list_permits(). */
			  ANTW_CAPABILITIES_SEARCH_LIST_ENABLED);

	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_2] =
		(uint8_t)(ANTW_CAPABILITIES_EXT_MESSAGE_ENABLED |
			  /* Background scan: radiant_search.c implements it as a
			   * first-class part of the sweep. */
			  ANTW_CAPABILITIES_SCAN_MODE_ENABLED |
			  ANTW_CAPABILITIES_EXT_ASSIGN_ENABLED);

	capabilities[ANTW_CAPABILITIES_OFFSET_MAX_SENSRCORE_CHANNELS] = 0x00u;

	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_3] =
		(uint8_t)(ANTW_CAPABILITIES_ADVANCED_BURST_ENABLED |
			  ANTW_CAPABILITIES_EVENT_FILTERING_ENABLED |
			  ANTW_CAPABILITIES_SEARCH_SHARING_ENABLED |
			  ANTW_CAPABILITIES_SELECTIVE_DATA_UPDATE_ENABLED |
			  /* Describes what the radio layer can do, same as
			   * src/ant_radio_stub.c reports, so an A/B diff won't
			   * flag it. radiant actually refuses AES-CTR - see
			   * antr_crypto_channel_enable(). */
			  ANTW_CAPABILITIES_ENCRYPTED_CHANNEL_ENABLED);

	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_4] = 0x00u;

	/* antr_channel_open_with_offset() is the real entry point, so the
	 * offset really is honoured. */
	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_5] =
		(uint8_t)ANTW_CAPABILITIES_CHANNEL_START_OFFSET_ENABLED;

	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

/* Identifiable from the host side with no debugger; deliberately not a string
 * claiming to be an AVN build it is not (the stub reports "STUB0.01B00"). */
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

/*
 * Encryption: these four exist because a backend stands in for libant.a,
 * which exports them regardless. They accept reads, hold writes, and refuse
 * to put a channel into an encrypted mode - ANT+'s AES-CTR is malleable and
 * unauthenticated. What replaces it is docs/radiant-security.md, outside this
 * contract.
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

	/* No length parameter: the callee reads a fixed count per type and the
	 * caller (the bridge) bounds it. */
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

	/* Write exactly what the type accounts for: the caller's buffer is
	 * sized for the largest reply including the bridge's own leading
	 * info-type byte, so zeroing the whole thing would overrun it. */
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
