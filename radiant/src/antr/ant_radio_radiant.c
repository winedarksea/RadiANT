/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ant_radio_radiant.c - integration layer: src/ant_radio.h's antr_* entry points
 * on top of the radiant modules, plus the seams none of them own.
 *
 * Clean-room: nothing here derives from sdk-ant, libant.a, disassembly, or ANT+
 * profile documents. See docs/decisions/0002-clean-room-policy.md.
 *
 * This file owns the key -> address table, the SDU masks, the advanced burst
 * config, encryption slots, per-channel device ID lists, one broadcast payload
 * per channel, the event thread, and the surrogate operation ids that attribute
 * terminal events to channels. Everything else forwards to the channel, sched,
 * search, event and transfer modules.
 *
 * Posting is not committing: radiant_sched_request_rx()/_tx() change the plan
 * and radiant_sched_tick() acts on it. Thread context posts everything and ticks
 * once at the end of pump(), so overlapping windows can merge. Callbacks never
 * tick. radiant_sched.c has no timer, so RADIANT_API_HOUSEKEEP_MS bounds how long
 * a request refused with EBUSY or ESTATE waits for a retry.
 *
 * radiant_event_drain() is thread context only and is never re-entered, so it
 * never runs inside an antr_* call (antr_stack_reset() flushes instead). It runs
 * on this file's event thread, woken by radiant_event_wakeup().
 *
 * Permanent limitation: key -> address. The derivation is not public, so the
 * one-entry table below (the ANT+ key) is the whole of it; any other key gets
 * ANTW_INVALID_PARAMETER_PROVIDED. Every ANT+ profile uses that key. For A/B
 * work, use the ANT+ key too, since libant.a computes the address.
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#if defined(CONFIG_HWINFO)
#include <zephyr/drivers/hwinfo.h>
#endif

#include <radiant/ant_wire.h>
#include <radiant/ant_health.h>
#include <radiant/ant_radio.h>

#include "ant_radio_radiant.h"
#include "ant_radio_radiant_priv.h"
#include <radiant/radiant_channel.h>
#include <radiant/radiant_crc_repair.h>
#include <radiant/radiant_diag.h>
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

/* Uses CONFIG_RADIANT_LOG_LEVEL so this module can be raised when debugging. */
LOG_MODULE_REGISTER(radiant_api, CONFIG_RADIANT_LOG_LEVEL);

/*
 * RADIANT_TRANSFER_SEG_* equal ANTR_BURST_SEGMENT_* so antr_burst_tx() forwards
 * the byte unchanged. A wrong END value marks every burst's last block as a
 * middle block, so each transfer times out at 1000 ms.
 */
BUILD_ASSERT(RADIANT_TRANSFER_SEG_CONTINUE == ANTR_BURST_SEGMENT_CONTINUE,
	     "burst segment drift: CONTINUE");
BUILD_ASSERT(RADIANT_TRANSFER_SEG_START == ANTR_BURST_SEGMENT_START,
	     "burst segment drift: START");
BUILD_ASSERT(RADIANT_TRANSFER_SEG_END == ANTR_BURST_SEGMENT_END,
	     "burst segment drift: END");

/* The channel ceiling must agree everywhere it sizes an array. The wire limit
 * is the burst header's low five bits. */
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

/* Four SDU masks, matching src/ant_radio_stub.c and tools/ant_features.py. */
#define API_SDU_MASKS 4u

/* Four encryption key slots, as in the stub, so A/B diffs do not flag it. */
#define API_CRYPTO_KEYS 4u
#define API_CRYPTO_ID_SIZE                                                    \
	(ANTW_MESG_CONFIG_ENCRYPT_REQ_CONFIG_ID_SIZE - ANTW_CHANNEL_NUM_SIZE)

/* Device IDs per inclusion/exclusion list: four, what a retail stick
 * accepts. Beyond that: ANTW_INVALID_LIST_ID. */
#define API_ID_LIST_MAX 4u
#define API_ID_LIST_BYTES 4u /* [devnum_lo][devnum_hi][dtype][ttype] */

/*
 * Half-width of a tracked channel's receive window (us). Only the ceiling and the
 * fallback when the estimator has nothing to say; radiant_channel_guard_us() is
 * the adaptive value in use.
 */
#define API_SLOT_GUARD_US RADIANT_CHANNEL_GUARD_MAX_US

/*
 * The guard's invariants: the ceiling must cover worst-case drift over every miss
 * up to RX_FAIL_TO_SEARCH, and the adaptive floor must cover one drift period.
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

/* Extended assignment bit for background scanning (ANT Message Protocol
 * and Usage Rev 5.1). Hand-defined: not in the generated ant_wire.h. */
#define API_EXT_ASSIGN_BACKGROUND_SCAN 0x01u

/* Noise-floor log interval: 60 s gives enough windows for a 10th percentile. */
#define API_NOISE_REPORT_US 60000000u

/*
 * Receive-path rate log interval: short enough to give a rate over a bench run.
 * Not shorter, since LOG_MODE_DEFERRED consoles drop other messages when a chatty
 * line fills the buffer. Lower it by hand when chasing a bench problem.
 */
#define API_RXPATH_REPORT_US 30000000u

#if defined(CONFIG_RADIANT_CRC_REPAIR)
/* CRC-repair counter interval: 10 s, so every sensitivity rung shows a line. */
#define API_CRC_REPORT_US 10000000u
#endif

/*
 * Event thread: 1280 B covers radiant_event_drain()'s frame and pump()'s
 * arithmetic. Priority 6 is below the bridge's parser thread (5), and the order
 * is the point: a higher-priority event thread could preempt the parser between
 * dispatching a command and sending its response, so the event would reach the
 * host first. Keep it greater than BRIDGE_PRIORITY (src/ant_serial_bridge.c);
 * the BUILD_ASSERT below enforces it.
 */
/*
 * Stack sizing. Security transforms add 256 B for radiant_sec_pump()'s CMAC
 * frame. Deliberately generous: an overrun here shows as a wrong answer, not a
 * fault. An arbitrated backend adds 1024 B: the pump's call chain continues
 * through the MPSL gate, and 1280 B overflowed on channel open.
 */
#if defined(CONFIG_RADIANT_BACKEND_NRF_GATE_MPSL)
#define API_EVENT_STACK_GATE 1024
#elif defined(CONFIG_RADIANT_BACKEND_CC26XX)
/* CC26x2's RF_postCmd() path is deeper still (queue walk, power
 * negotiation), so the same stack size applies. */
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

/* What this channel's scheduler slot holds, so a completion can be attributed to its requester. */
enum api_slot_kind {
	API_SLOT_NONE = 0,
	API_SLOT_TRACK_RX,  /* a tracked channel's predicted receive window */
	API_SLOT_MASTER_TX, /* a master's own slot */
	API_SLOT_MASTER_RX, /* the turnaround a master listens in after its slot */
	API_SLOT_SEARCH     /* the shared wildcard sweep, carried on this slot */
};

struct api_chan {
	/* Surrogate operation id for radiant_channel.c. radiant_sched.c merges
	 * channels with no id of its own, so this file mints a non-zero token per
	 * accepted post, used only for attribution. */
	uint32_t op;

	uint8_t slot_kind;
	/* Set when anything arrived in the window now ending. */
	bool slot_heard;

	/* The HAL keeps this filter array until the terminal event, so it
	 * cannot be a stack local. */
	struct radiant_rx_filter filter;

	/* Likewise the transmit body: backends DMA straight out of it. */
	uint8_t tx_body[RADIANT_FRAME_BODY_MAX];
	uint8_t tx_body_len;

	/* Broadcast payload: queued by antr_broadcast_message_tx(), and also
	 * the acknowledgement payload. */
	uint8_t bcast[ANTW_ANT_MAX_PAYLOAD_SIZE];
	bool    bcast_valid;
	bool    bcast_pending;
	/* Host write generation. bcast_gen_tx is the generation the frame in
	 * flight carries. A completion clears bcast_pending only if no newer write
	 * landed since, or that write is lost. */
	uint8_t bcast_gen;
	uint8_t bcast_gen_tx;
	/* Last data packet this master sent, as plaintext. Rev 5.1 p.109: the
	 * next broadcast repeats it after a transfer, until the host writes a
	 * broadcast (which clears last_tx_newer). */
	uint8_t last_tx[RADIANT_TRANSFER_PKT_BYTES];
	bool    last_tx_newer;
	/* A slave keeps receiving until a real master slot opens the exchange. */
	bool ack_pending;
	uint8_t ack_data[RADIANT_TRANSFER_PKT_BYTES];
	radiant_time_t ack_deadline;

#if defined(CONFIG_RADIANT_SEC)
	/* The current slot's ciphertext, sent as the ack payload so host
	 * plaintext never goes on the air. bcast is never modified: its readback
	 * must return what was written. */
	uint8_t sec_pay[ANTW_ANT_MAX_PAYLOAD_SIZE];
	bool    sec_pay_valid;
#endif

	/* One word per entry, wire order [devnum_lsb, devnum_msb, device_type,
	 * trans_type], so antr_id_list_add() can replace an entry with one store. */
	uint32_t id_list[API_ID_LIST_MAX];
	uint8_t id_list_size;
	bool    id_list_exclude;

	uint8_t crypto_mode;

	/* Next host-facing burst sequence number: 0 = no transfer in progress,
	 * 1-3 = mid-transfer. Acknowledged data is a one-packet burst ending LAST. */
	uint8_t in_pkts;

	/* Master turnaround, deferred from api_sched_tx() to api_sched_done().
	 * Posting it from the TX callback reuses the slot still in flight and
	 * swallows that TX's own done(). */
	bool           turn_pending;

	/* A tracked RX window is armed and the channel owes its next one on
	 * completion. Bounds the re-post to depth one (see api_sched_done()). Also
	 * set for a master's next TX slot after MISSED/DENIED MASTER_TX. */
	bool           track_repost;
	radiant_time_t turn_t_sync;

	/* Watchdog state for api_housekeep(): xfer_stuck_since is when the engine
	 * was first seen busy with nothing armed; in_last is the last inbound
	 * packet. Each has a validity flag, since zero is a valid time. */
	bool           xfer_stuck_valid;
	radiant_time_t xfer_stuck_since;
	bool           in_last_valid;
	radiant_time_t in_last;
};

static struct api_chan     api_ch[API_CHANNELS];
static struct radiant_transfer api_xfer[API_CHANNELS];
static struct radiant_search   api_search;

static uint8_t api_net_addr[API_NETWORKS][RADIANT_NET_ADDR_LEN];

/* Which api_net_addr[] rows a host installed. An all-zero row looks like
 * a real address, and a channel tracked on one hears nothing, so assign to
 * an unset network is refused. Only the ANT+ key is accepted. */
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
/* Network-0 key install deferred while a sweep chunk is live; applied by
 * the next pump with no carrier. */
static bool     api_search_reinit_pending;
static bool     api_inited;

/* Single-bit CRC repair on this build and backend. Resolved once in
 * antr_init() by api_crc_repair_setup(). api_post_track_rx() asks the backend
 * for CRC failures; api_sched_rx() acts on them. */
static bool api_crc_repair_on;

/* Repaired frames are also checked by radiant_profile_sanity.c. Only true
 * when api_crc_repair_on is. */
static bool api_profile_sanity_on;

static struct radiant_api_stats api_stats;

/*
 * Serialises thread-context scheduling passes (the parser and event threads).
 * Thread context only, around bounded work; never around a drain. Radio-callback
 * passes are serialised in radiant_sched.c instead (in_pass, interrupts off).
 */
K_MUTEX_DEFINE(api_lock);

#if defined(CONFIG_RADIANT_SEC_LOCK_CHECK)
uint32_t antr_api_lock_violations;

bool antr_api_lock_held(void)
{
	/* k_current_get() in an ISR is the interrupted thread, which would pass
	 * exactly the dangerous case, so an ISR never holds the lock. */
	return !k_is_in_isr() && api_lock.lock_count > 0u &&
	       api_lock.owner == k_current_get();
}

/* The port's half of radiant_sec's lock check (radiant_sec.h). radiant_sec
 * is not re-entrant, so reaching it without api_lock is a bug whether or not
 * a race happens to land. */
void radiant_sec_port_lock_check(const char *who)
{
	if (!antr_api_lock_held()) {
		antr_api_lock_violations++;
		__ASSERT(false, "radiant_sec: %s without api_lock", who);
	}
	ARG_UNUSED(who);
}
#endif

/* Given once per queued message (possibly from the radio ISR), and on each housekeeping timeout. */
static K_SEM_DEFINE(api_event_sem, 0, 1);

/* Created in antr_init(), not K_THREAD_DEFINE: the drain calls
 * antr_on_message(), which must not run before antr_init() returns. */
K_THREAD_STACK_DEFINE(api_event_stack, API_EVENT_STACK_SIZE);
static struct k_thread api_event_thread_data;

static void api_event_thread(void *a, void *b, void *c);
static void api_pump_locked(void);

/* Port hooks that radiant_event.c declares but leaves undefined, so a
 * build that omits one fails to link. */

unsigned int radiant_event_crit_enter(void)
{
	/* irq_lock(), not a mutex: the producer may be a radio ISR and must not block. */
	return irq_lock();
}

void radiant_event_crit_exit(unsigned int key)
{
	irq_unlock(key);
}

#if defined(CONFIG_RADIANT_SWI)
/* Runs at ordinary interrupt priority from the trampoline, which is what
 * makes it safe to call from the wakeup. */
static void event_wakeup_swi(uint32_t bits)
{
	ARG_UNUSED(bits);
	k_sem_give(&api_event_sem);
}
#endif

void radiant_event_wakeup(void)
{
	/* O(1), ISR-safe, never calls back into radiant_event. The limit-1
	 * semaphore collapses a burst of arrivals into one wake; the drain empties
	 * the queue anyway. */
#if defined(CONFIG_RADIANT_SWI)
	/*
	 * Under the MPSL gate this runs from MPSL's zero-latency signal callback, above
	 * every irq_lock(). Giving the semaphore there wakes a thread whose timeout is
	 * pending, and the kernel's timeout list is then touched from a context that
	 * cannot hold its lock (bus faults, reboot loops). Pending the SWI defers the
	 * give. See docs/p4-coexistence-bugs.md.
	 */
	radiant_swi_pend(RADIANT_SWI_EVENT_WAKEUP);
#else
	/* Ordinary radio interrupt here, so a direct semaphore give is legal and cheaper. */
	k_sem_give(&api_event_sem);
#endif
}

/*
 * Port hook: radiant_event.c calls radiant_on_message(), which forwards field for
 * field to antr_on_message(). The bridge stays the single definition of
 * antr_on_message() for every backend; the two message structs are layout-identical.
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

/* Adapter between radiant_channel.c and radiant_event.c, which name the
 * same operation differently. event_code is an ANTW_EVENT_* byte in both.
 * May run in a radio callback, so it must not block. */
void radiant_channel_event_out(uint8_t channel, uint8_t event_code)
{
#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
	/* A peer lost mid-pairing raises only RX_FAIL_GO_TO_SEARCH, so the
	 * half-finished exchange is freed here. Otherwise the device-wide key
	 * exchange would deadlock every channel (docs/radiant-security.md 8.1). */
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

/*
 * A host-originated transfer has taken this channel's slot, so the binding (usually
 * a queued MASTER_TX) no longer describes the slot. Clear only the kind: `op`
 * stays, so a close during the transfer still completes on the engine's terminal.
 * Otherwise the transfer's done() is taken for a missed MASTER_TX and the clock
 * advances an extra period. Called only from the submit that starts a transfer.
 */
static void api_xfer_superseded(uint8_t ch)
{
	api_ch[ch].slot_kind = (uint8_t)API_SLOT_NONE;
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

/* Maps radiant_transfer return codes to wire responses (mapping per radiant_transfer.h). */
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
		 * FAILED: a denied ack reply missed the peer's receive window. The engine
		 * still opens the next window inside an inbound burst; a denied continuation
		 * ends it (RX_FAILED). The originator's burst retry runs only on TIMEOUT,
		 * never on a denial. The denial is counted in api_stats.sched_denied from
		 * `why` at the top of api_sched_done(), so this lossy mapping loses nothing.
		 */
		return RADIANT_RADIO_STATUS_FAILED;
	case RADIANT_SCHED_DONE_MISSED:
	case RADIANT_SCHED_DONE_FAILED:
	default:
		return RADIANT_RADIO_STATUS_FAILED;
	}
}

/*
 * Device ID inclusion/exclusion list, enforced through cbs.permit (api_search_permit()):
 * at steer time, and at offer time before the frame's ACQUIRE claim, so a refused
 * device neither costs a wildcard channel nor drops this channel from the search.
 * A stored but unused list would be worse than a refused one, so it is enforced.
 *
 * Runs in the radio interrupt and in thread context: read-only and bounded. The
 * list may be rewritten while open, so on an MPSL build a zero-latency interrupt
 * can preempt a writer. Each field is read once, and writers order their stores so
 * any prefix is the old list, switched off, or the new one.
 */
static bool api_id_list_permits(uint8_t ch, const struct radiant_channel_id *id)
{
	const struct api_chan *c = &api_ch[ch];
	uint8_t n = *(const volatile uint8_t *)&c->id_list_size;
	uint8_t i;
	bool    hit = false;

	if (n == 0u) {
		return true;
	}
	if (n > API_ID_LIST_MAX) {
		n = API_ID_LIST_MAX;
	}

	for (i = 0u; i < n; i++) {
		uint32_t w = *(const volatile uint32_t *)&c->id_list[i];
		const uint8_t *e = (const uint8_t *)&w;
		uint16_t dn = (uint16_t)((uint16_t)e[0] | ((uint16_t)e[1] << 8));

		/* Zero is the wildcard in each field, as with a channel ID. */
		if (dn != 0u && dn != id->device_number) {
			continue;
		}
		if (e[2] != 0u &&
		    (uint8_t)(e[2] & RADIANT_DEVICE_TYPE_MASK) !=
			    (uint8_t)(id->device_type & RADIANT_DEVICE_TYPE_MASK)) {
			continue;
		}
		if (e[3] != 0u && e[3] != id->trans_type) {
			continue;
		}
		hit = true;
		break;
	}

	return *(const volatile bool *)&c->id_list_exclude ? !hit : hit;
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
 * Where radiant_sec_pump() delivers plaintext. A direct call that overrides a weak
 * definition: a registered ops struct would be a .data relocation the linker
 * cannot discard, and the unit-test app has no radiant_api.c. Event-thread context.
 *
 * The verdict does not reach the wire, so the data path is the same whatever the
 * channel asked for. It is reported per channel via 0xF4 and per window via 0xF8.
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
	/* Data after CHANNEL_CLOSED is a protocol violation, so it is dropped. */
	if (!radiant_channel_is_open(ch)) {
		return;
	}

	memset(&hal, 0, sizeof(hal));
	hal.status = RADIANT_RADIO_STATUS_OK;
	hal.t_sync = (radiant_time_t)t_sync;
	/* Carried from the radio's ADDRESS-event capture, not re-read when the pump runs. */
	hal.t_sync_exact = true;

	api_post_rx(ch, (uint8_t)ANTW_MESG_BROADCAST_DATA_ID, &id, &hal, pay,
		    len, 0u, false);
}

/*
 * One window's verdict, MESG_RADIANT_SEC_VERDICT (0xF8):
 * [ch, verdict, window_start u16 LE, W, seq u16 LE]. Sent only when the channel set
 * RADIANT_SEC_SW_VERDICT_EVENT in 0xF1, and only from an X_AUTH window close.
 * Event-thread context under api_lock. It posts to the same ring as the data, so
 * it stays behind the data it judges. A full ring drops it (counted in
 * verdict_events_dropped); the host sees the gap as a hole in seq and treats
 * unverified packets as such.
 */
void radiant_sec_window_verdict(uint8_t ch, uint16_t window_start,
				enum radiant_sec_verdict verdict, uint8_t w,
				uint8_t switches, uint16_t seq)
{
	uint8_t body[7];

	if ((switches & RADIANT_SEC_SW_VERDICT_EVENT) == 0u) {
		return;
	}
	/* As for data: nothing after CHANNEL_CLOSED. */
	if (!api_ch_valid(ch) || !radiant_channel_is_open(ch)) {
		return;
	}

	body[0] = ch;
	body[1] = (uint8_t)verdict;
	body[2] = (uint8_t)window_start;
	body[3] = (uint8_t)(window_start >> 8);
	body[4] = w;
	body[5] = (uint8_t)seq;
	body[6] = (uint8_t)(seq >> 8);
	if (radiant_event_post_raw((uint8_t)ANTW_MESG_RADIANT_SEC_VERDICT_ID,
				   body, (uint8_t)sizeof(body)) !=
	    RADIANT_EVENT_OK) {
		radiant_sec_stat_verdict_dropped(ch);
	}
}
#endif /* CONFIG_RADIANT_SEC */

/*
 * Transfer-engine callbacks. They may run in radio interrupt context (the engine
 * re-arms from its own completions), so each one only posts to the ISR-safe event ring.
 */

static void api_xfer_done(void *ctx, const struct radiant_transfer_result *r)
{
	uint8_t ch = (uint8_t)(uintptr_t)ctx;

	if (!api_ch_valid(ch) || r == NULL) {
		return;
	}
	if (r->ev == RADIANT_TRANSFER_EV_TX_FAILED) {
		switch (r->fail) {
		case RADIANT_TRANSFER_FAIL_NO_ACK: api_stats.xfer_no_ack++; break;
		case RADIANT_TRANSFER_FAIL_BAD_ACK: api_stats.xfer_bad_ack++; break;
		case RADIANT_TRANSFER_FAIL_RADIO: api_stats.xfer_radio_failed++; break;
		default: break;
		}
		LOG_DBG("ch=%u transfer failed reason=%u", ch, (unsigned)r->fail);
	}
	/* Completion of a transfer owes the channel its next tracking window. */
	if (r->ev != RADIANT_TRANSFER_EV_NEXT_BLOCK) {
		api_ch[ch].track_repost = true;
	}

	/*
	 * Buffer contract: the bridge holds one 24-byte block behind a binary semaphore,
	 * released only here. A missed NEXT_DATA_BLOCK stalls a burst 1000 ms per packet.
	 * The engine guarantees one done() per block.
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

	/* 0 on a transfer's first packet, then 1,2,3,1,2,3..., as handle_burst() decodes. */
	seq = api_ch[ch].in_pkts;

	/* One packet ending LAST is acknowledged data; longer runs are bursts.
	 * The air cannot tell them apart, so it is decided here. */
	if (last && api_ch[ch].in_pkts == 0u) {
		msg_id = (uint8_t)ANTW_MESG_ACKNOWLEDGED_DATA_ID;
	} else {
		msg_id = (uint8_t)ANTW_MESG_BURST_DATA_ID;
	}

	/* Wraps 3 -> 1, never to 0: 0 means a new transfer, so a wrapped
	 * counter would mislabel a LAST. */
	api_ch[ch].in_pkts = last ? 0u : (uint8_t)((seq == 3u) ? 1u : (seq + 1u));
	/* Feeds the abandoned-burst watchdog (api_watchdogs_locked()). */
	api_ch[ch].in_last = radiant_radio_now();
	api_ch[ch].in_last_valid = true;

	/* rx_data carries no clock reading, so t_sync_exact is false and the
	 * 0xE0 timestamp is suppressed rather than faking a radio time. */
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
	 * An unwritten broadcast buffer is valid: it reads as eight zero bytes. Refusing
	 * it meant a slave could never acknowledge anything, since no host writes
	 * broadcast data to a listen-only channel. The engine then returned ESTATE and
	 * dropped every acknowledged packet silently. The acknowledgement still carries
	 * the channel's own buffer, whatever it holds (api_post_master_tx() sends zeros
	 * for the same reason).
	 */
	if (!api_ch[ch].bcast_valid) {
		api_stats.acks_from_unwritten_buffer++;
	}

#if defined(CONFIG_RADIANT_SEC)
	/* Secured channel: ack with the ciphertext already built for this
	 * counter. Same nonce and plaintext, so no nonce reuse, and never the
	 * host's plaintext. A keyed master with nothing built - its last slot
	 * withheld (pending with no epoch, past the ceiling, spent) - does not
	 * acknowledge: the plaintext buffer would be the fail-open path. A
	 * slave never builds sec_pay; its ack carries its own buffer, armed or
	 * pending alike (TODO security item 11). */
	if (radiant_sec_channel_is_secured(ch)) {
		if (api_ch[ch].sec_pay_valid) {
			memcpy(out, api_ch[ch].sec_pay,
			       RADIANT_TRANSFER_PKT_BYTES);
			return true;
		}
		if (radiant_channel_is_master(ch)) {
			return false;
		}
	}
#endif

	memcpy(out, api_ch[ch].bcast, RADIANT_TRANSFER_PKT_BYTES);
	return true;
}

#if defined(CONFIG_RADIANT_ACK_FAULTS)
/*
 * ACK fault instrument forwarding (CONFIG_RADIANT_ACK_FAULTS, test images only).
 * Kept out of api_ch[], which is cleared on unassign, so the hook survives it.
 * Set before the channel opens (see antr_reply_plan_hook_set()).
 */
BUILD_ASSERT((int)ANTR_REPLY_ACK == (int)RADIANT_REPLY_ACK &&
	     (int)ANTR_REPLY_MUTE == (int)RADIANT_REPLY_MUTE &&
	     (int)ANTR_REPLY_DEAF == (int)RADIANT_REPLY_DEAF,
	     "antr_reply_action must mirror radiant_reply_action");

static struct {
	antr_reply_plan_fn fn;
	void              *ctx;
} api_reply_hook[API_CHANNELS];

static void api_xfer_reply_plan(void *ctx, const struct radiant_frame *f,
				radiant_time_t t_sync,
				struct radiant_reply_plan *out)
{
	uint8_t                ch = (uint8_t)(uintptr_t)ctx;
	antr_reply_plan_fn     fn;
	struct antr_reply_plan plan = { .action = ANTR_REPLY_ACK, .offset_us = 0 };

	if (!api_ch_valid(ch)) {
		return;
	}
	fn = api_reply_hook[ch].fn;
	if (fn == NULL) {
		return;
	}
	fn(api_reply_hook[ch].ctx, ch, f->payload, (uint64_t)t_sync, &plan);
	out->action = (enum radiant_reply_action)plan.action;
	out->offset_us = plan.offset_us;
}

antr_err_t antr_reply_plan_hook_set(uint8_t channel, antr_reply_plan_fn fn,
				    void *ctx)
{
	if (!api_ch_valid(channel)) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	api_reply_hook[channel].fn = NULL;
	api_reply_hook[channel].ctx = ctx;
	api_reply_hook[channel].fn = fn;
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_reply_stats_get(uint8_t channel, struct antr_reply_stats *out)
{
	const struct radiant_transfer_stats *s;

	if (!api_ch_valid(channel) || out == NULL) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	s = radiant_transfer_stats(&api_xfer[channel]);
	out->acks_sent = s->acks_sent;
	out->too_early = s->reply_plan_too_early;
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}
#endif /* CONFIG_RADIANT_ACK_FAULTS */

static radiant_time_t api_xfer_t_sync_for(uint8_t ch);

/* Retry point for a slave's burst packet 0: the master's next reply window. */
static radiant_time_t api_xfer_retry_t_sync(void *ctx)
{
	uint8_t ch = (uint8_t)(uintptr_t)ctx;

	if (!api_ch_valid(ch)) {
		return RADIANT_TIME_NEVER;
	}
	return api_xfer_t_sync_for(ch);
}

/*
 * Ends an unfinished inbound burst: EVENT_TRANSFER_RX_FAILED (0x04) to the host, and
 * numbering restarts at 0 so a later acknowledged message is labelled as such. Every
 * path that ends a burst comes here: engine end, close, drop to search, and the
 * abandoned-burst watchdog. Radio interrupt or thread context; O(1).
 */
static void api_inbound_failed(uint8_t ch)
{
	api_ch[ch].in_pkts = 0u;
	api_ch[ch].in_last_valid = false;
	api_stats.rx_bursts_failed++;
	(void)radiant_event_post_channel_event(
		ch, (uint8_t)ANTW_EVENT_TRANSFER_RX_FAILED);
}

/* The engine ended an inbound burst before its LAST was acknowledged. */
static void api_xfer_rx_failed(void *ctx)
{
	uint8_t ch = (uint8_t)(uintptr_t)ctx;

	if (!api_ch_valid(ch)) {
		return;
	}
	api_inbound_failed(ch);
}

/*
 * The engine is idle after an inbound burst. The channel was owed no tracked work
 * during the burst (the pump skips a busy engine), so its next window is armed now,
 * from this terminal. Its slot clock may be stale, since continuation packets never
 * re-anchor it. Step it past the instants already gone, charging nothing: posting
 * them would count a miss for each and could send the slave to search.
 */
static void api_xfer_inbound_ended(void *ctx)
{
	uint8_t ch = (uint8_t)(uintptr_t)ctx;
	const struct radiant_radio_caps *caps = radiant_radio_caps_get();
	radiant_time_t earliest;

	if (!api_ch_valid(ch)) {
		return;
	}
	earliest = radiant_radio_now() +
		   (radiant_time_t)radiant_channel_guard_us(ch) +
		   (radiant_time_t)((caps != NULL) ? caps->min_arm_lead_us : 0u);
	radiant_channel_catch_up(ch, earliest);
	/* Stepping widens the guard, which can open the window before the
	 * instant just checked. Recheck with the wide guard so the first window
	 * after the burst stays reachable. */
	earliest = radiant_radio_now() +
		   (radiant_time_t)radiant_channel_guard_us(ch) +
		   (radiant_time_t)((caps != NULL) ? caps->min_arm_lead_us : 0u);
	radiant_channel_catch_up(ch, earliest);
	api_ch[ch].track_repost = true;
}

static const struct radiant_transfer_ops api_xfer_ops = {
	.done = api_xfer_done,
	.rx_data = api_xfer_rx_data,
	.broadcast_payload = api_xfer_broadcast,
	.retry_t_sync = api_xfer_retry_t_sync,
	.rx_failed = api_xfer_rx_failed,
	.inbound_ended = api_xfer_inbound_ended,
#if defined(CONFIG_RADIANT_ACK_FAULTS)
	.reply_plan = api_xfer_reply_plan,
#endif
};

/* Configures one channel's engine at open, not assign: it reads the ID,
 * network address, RF index and power at init, and the host may still be
 * setting them. */
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
	/* No safe default for the slot bit (0x82 vs 0x8A): wrong either way and
	 * invisible without a sniffer. Channel type decides it. */
	cfg.slot_opener = radiant_channel_is_master(ch);
	cfg.rx_only = radiant_channel_type_get(ch) ==
		      (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY;
	cfg.channel = ch;
	/* Kconfig's 0 means no repeats; the engine's 0 means its default. */
	cfg.in_ack_repeats = (CONFIG_RADIANT_INBOUND_ACK_REPEATS == 0) ?
				     (uint8_t)RADIANT_TRANSFER_IN_ACK_REPEATS_NONE :
				     (uint8_t)CONFIG_RADIANT_INBOUND_ACK_REPEATS;
	cfg.ops = &api_xfer_ops;
	cfg.ctx = (void *)(uintptr_t)ch;

	return radiant_transfer_init(&api_xfer[ch], &cfg);
}

/* Acquisition resolves wildcards after open() copied the ID. Refresh only
 * an idle engine, and never discard an owned burst block. */
static void api_xfer_refresh_id(uint8_t ch)
{
	struct radiant_channel_id id;
	struct radiant_transfer *t = &api_xfer[ch];
	uint8_t addr[RADIANT_RADIO_ADDR_MAX];
	int n;

	if (!radiant_transfer_is_idle(t) || !t->ready ||
	    radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return;
	}
	n = radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING, t->cfg.net_addr,
			       &id, addr, sizeof(addr));
	if (n > 0) {
		t->cfg.id = id;
		memcpy(t->filter.addr, addr, (size_t)n);
		t->filter.addr_len = (uint8_t)n;
	}
}

/* Sends the pending command from the master's frame that opens this slave's reply
 * slot, not from the window's terminal (a merged window can leave less than an arm
 * lead before the slot). Posting aborts the window, as the engine's own ACK reply
 * does; the transmit's completion retires the receive's op token. Radio interrupt context.
 *
 * Never on a secured channel: its MAC verdict lags a window, so the opener is not
 * authenticated in time. antr_acknowledge_message_tx() keeps secured channels off
 * this path; the check here covers a channel secured after acceptance. */
static void api_ack_pending_send(uint8_t ch, radiant_time_t t_sync)
{
	struct api_chan *c = &api_ch[ch];

	if (radiant_sec_channel_is_secured(ch) ||
	    radiant_channel_state_get(ch) != RADIANT_CH_STATE_TRACKING ||
	    !radiant_transfer_is_idle(&api_xfer[ch]) ||
	    radiant_radio_now() >= c->ack_deadline) {
		return; /* the watchdog owns an expired command's failure */
	}
	api_xfer_refresh_id(ch);
	c->ack_pending = false;
	if (radiant_transfer_ack_data(&api_xfer[ch], c->ack_data,
				      RADIANT_TRANSFER_PKT_BYTES,
				      t_sync + RADIANT_TRANSFER_SLOT_REPLY_US) !=
	    RADIANT_TRANSFER_OK) {
		api_stats.xfer_radio_failed++;
		(void)radiant_event_post_transfer_tx_failed(ch);
	}
}

/*
 * Slave broadcast to the master: control byte 0x02 (RADIANT_CTRL_SLAVE_BROADCAST),
 * sent RADIANT_TRANSFER_SLOT_REPLY_US after the master frame, once per host write
 * (as a Garmin USB-m slave does). Called from a master frame's receive callback,
 * after api_ack_pending_send(), which takes priority. Bidirectional slaves only; a
 * secured channel keeps the write pending. A refused post leaves bcast_pending set,
 * so the next master frame retries. Completion is api_sched_tx().
 */
static void api_post_slave_bcast(uint8_t ch, radiant_time_t t_sync)
{
	struct radiant_ctrl_fields fields;
	struct radiant_channel_id  id;
	struct radiant_frame       f;
	struct radiant_frame_wire  w;
	struct radiant_sched_tx    req;
	uint8_t                    pay[RADIANT_TRANSFER_PKT_BYTES];
	unsigned int               key;

	if (!api_ch[ch].bcast_pending || api_ch[ch].ack_pending ||
	    radiant_channel_type_get(ch) != (uint8_t)ANTW_CHANNEL_TYPE_SLAVE ||
	    radiant_sec_channel_is_secured(ch) ||
	    radiant_channel_state_get(ch) != RADIANT_CH_STATE_TRACKING ||
	    !radiant_transfer_is_idle(&api_xfer[ch])) {
		return;
	}
	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return;
	}

	key = radiant_event_crit_enter();
	memcpy(pay, api_ch[ch].bcast, sizeof(pay));
	api_ch[ch].bcast_gen_tx = api_ch[ch].bcast_gen;
	radiant_event_crit_exit(key);

	/* 0x02, all flags clear: radiant_frame_make() refuses control bytes
	 * outside the captured table, and this one is in it. */
	memset(&fields, 0, sizeof(fields));
	if (radiant_frame_make(&f, &id, &fields, pay,
			       RADIANT_FRAME_PAYLOAD_STD) != RADIANT_FRAME_OK ||
	    radiant_frame_encode(RADIANT_FRAME_CFG_TRACKING, api_net_addr_for(ch),
				 &f, &w) != RADIANT_FRAME_OK ||
	    (size_t)w.body_len > sizeof(api_ch[ch].tx_body) ||
	    w.addr_len > sizeof(req.addr)) {
		return;
	}
	memcpy(api_ch[ch].tx_body, w.body, w.body_len);
	api_ch[ch].tx_body_len = w.body_len;

	memset(&req, 0, sizeof(req));
	req.fmt = radiant_frame_format(RADIANT_FRAME_CFG_TRACKING);
	req.rf_index = api_rf_of(ch);
	req.power = api_power_of(ch);
	memcpy(req.addr, w.addr, w.addr_len);
	req.addr_len = w.addr_len;
	req.body = api_ch[ch].tx_body;
	req.body_len = api_ch[ch].tx_body_len;
	req.t_sync_at = t_sync + (radiant_time_t)RADIANT_TRANSFER_SLOT_REPLY_US;
	req.follow_on_us = 0u;

	if (radiant_sched_request_tx(ch, &req) == RADIANT_RADIO_OK_RC) {
		api_stats.slave_bcast_posted++;
	}
}

/* Ends an accepted command that was never sent, with its one terminal
 * event. Every path that takes the channel off air must come here before
 * CHANNEL_CLOSED, or unassign drops the command with no outcome. */
static void api_ack_pending_fail(uint8_t ch)
{
	unsigned int key = radiant_event_crit_enter();

	if (api_ch[ch].ack_pending) {
		api_ch[ch].ack_pending = false;
		(void)radiant_event_post_transfer_tx_failed(ch);
	}
	radiant_event_crit_exit(key);
}

/* ---------------------------------------------------------------------------
 * The search
 * ---------------------------------------------------------------------------
 */

static void api_search_cfg_build(struct radiant_search_cfg *cfg)
{
	radiant_search_cfg_default(cfg);
	uint8_t net = 0u;

	/* Sweep on network 0's address, where nearly every host installs the
	 * ANT+ key. If only another network has a key, sweep the lowest such
	 * network instead of the all-zero address. With no key anywhere it stays
	 * on network 0. */
	if (!api_net_valid[0]) {
		for (uint8_t n = 1u; n < API_NETWORKS; n++) {
			if (api_net_valid[n]) {
				net = n;
				break;
			}
		}
	}
	memcpy(cfg->net_addr, api_net_addr[net], RADIANT_NET_ADDR_LEN);
}

#if defined(CONFIG_RADIANT_SEARCH_COVERAGE)
uint32_t radiant_search_sets_uncovered_get(void)
{
	const struct radiant_search_stats *st = radiant_search_get_stats(&api_search);

	return (st != NULL) ? st->sets_advanced_uncovered : 0u;
}
#endif

#if defined(CONFIG_RADIANT_SEARCH_PHASE_DIAG)
/* ADR 0018 bench diagnostic for tools/ant_disc_phase.py: each frame's
 * phase against channel 0's tracked slot (heart-rate period). Filled in the
 * radio interrupt, logged from api_housekeep(). */
#define PHASE_DIAG_PERIOD_US 246277u
#define PHASE_DIAG_RING      32u
#define PHASE_DIAG_BIN_US    10000u

struct phase_diag_frame {
	uint16_t dev;
	uint8_t  type;
	uint8_t  tt;
	uint8_t  set;
	uint8_t  nact;
	uint8_t  nmatch;
	uint8_t  ch0_state;
	int8_t   claim;
	uint32_t ph;
};

static struct phase_diag_frame phase_diag_ring[PHASE_DIAG_RING];
static volatile uint32_t phase_diag_wr;
static volatile uint32_t phase_diag_rd;
static uint32_t phase_diag_lost;
static uint32_t phase_diag_ok;
static uint32_t phase_diag_ab;
static uint32_t phase_diag_other;
static uint32_t phase_diag_bins;
static uint64_t phase_diag_us;

void radiant_search_phase_diag_frame(const struct radiant_channel_id *id,
				     radiant_time_t t_sync, int claim,
				     uint8_t nact, uint8_t nmatch, uint8_t set)
{
	radiant_time_t ref = radiant_channel_next_slot(0u);
	struct phase_diag_frame *e;

	if (phase_diag_wr - phase_diag_rd >= PHASE_DIAG_RING) {
		phase_diag_lost++;
		return;
	}
	e = &phase_diag_ring[phase_diag_wr % PHASE_DIAG_RING];
	e->dev = id->device_number;
	e->type = id->device_type;
	e->tt = id->trans_type;
	e->set = set;
	e->nact = nact;
	e->nmatch = nmatch;
	e->claim = (int8_t)claim;
	e->ch0_state = (uint8_t)radiant_channel_state_get(0u);
	e->ph = (ref == RADIANT_TIME_NEVER || ref == 0u)
			? 0xFFFFFFFFu
			: (uint32_t)((t_sync + PHASE_DIAG_PERIOD_US * 64u - ref) %
				     PHASE_DIAG_PERIOD_US);
	phase_diag_wr++;
}

/* Tallies a finished scan chunk into 10 ms bins of the period. Call before
 * radiant_search_on_done() clears the bounds. */
static void phase_diag_chunk(enum radiant_sched_done why, radiant_time_t now)
{
	radiant_time_t o = api_search.t_open;
	radiant_time_t c = api_search.t_close;
	radiant_time_t ref = radiant_channel_next_slot(0u);

	if (why == RADIANT_SCHED_DONE_OK) {
		phase_diag_ok++;
	} else if (why == RADIANT_SCHED_DONE_ABORTED) {
		phase_diag_ab++;
		c = now;
	} else {
		phase_diag_other++;
		c = o;
	}
	if (c > o && api_search.op != 0u) {
		phase_diag_us += (uint64_t)(c - o);
		if (ref != RADIANT_TIME_NEVER && ref != 0u) {
			for (radiant_time_t t = o; t < c; t += PHASE_DIAG_BIN_US) {
				radiant_time_t ph = (t + PHASE_DIAG_PERIOD_US * 64u - ref) %
						    PHASE_DIAG_PERIOD_US;

				phase_diag_bins |= 1u << (ph / PHASE_DIAG_BIN_US);
			}
		}
	}
}

/* Log what the interrupt collected. Called from api_housekeep(). */
static void phase_diag_housekeep(radiant_time_t now)
{
	static radiant_time_t last;
	const struct radiant_search_stats *ss = radiant_search_get_stats(&api_search);

	while (phase_diag_rd != phase_diag_wr) {
		const struct phase_diag_frame *e =
			&phase_diag_ring[phase_diag_rd % PHASE_DIAG_RING];

		LOG_INF("TDIAG frame dev=%u type=0x%02x tt=%u ph=%u set=%u claim=%d "
			"nact=%u nmatch=%u ch0=%u",
			e->dev, e->type, e->tt, (unsigned int)e->ph, e->set, e->claim,
			e->nact, e->nmatch, e->ch0_state);
		phase_diag_rd++;
	}
	if (now - last >= 5000000u) {
		last = now;
		LOG_INF("TEMPDIAG chunks: ok=%u aborted=%u other=%u credited_ms=%u "
			"phase_bins=0x%08x sets=%u ok_frames=%u acq=%u idrej=%u lost=%u",
			(unsigned int)phase_diag_ok, (unsigned int)phase_diag_ab,
			(unsigned int)phase_diag_other,
			(unsigned int)(phase_diag_us / 1000u),
			(unsigned int)phase_diag_bins, (unsigned int)ss->sets_advanced,
			(unsigned int)ss->frames_ok, (unsigned int)ss->acquired,
			(unsigned int)api_stats.id_list_rejects,
			(unsigned int)phase_diag_lost);
		phase_diag_bins = 0u;
	}
}
#endif /* CONFIG_RADIANT_SEARCH_PHASE_DIAG */

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

	/* Backstop only: api_search_permit() refuses the device before it is
	 * offered. Reaching here means that wiring was lost; the frame is dropped. */
	if (!api_id_list_permits(channel, &r->id)) {
		api_stats.id_list_rejects++;
		return;
	}

	/* A scan never leaves its search. Query the mode before anything below
	 * changes the channel state. */
	is_scan = radiant_search_chan_mode(&api_search, channel) ==
		  RADIANT_SEARCH_MODE_SCAN;

	if (!is_scan) {
		radiant_channel_on_acquired(channel, &r->id, r->t_sync);
		api_xfer_refresh_id(channel);
		api_stats.acquired++;
	}

	/* The acquiring frame is the first broadcast, so it goes to the host now
	 * rather than a period later. The HAL event is built from the search
	 * result. */
	memset(&hal, 0, sizeof(hal));
	hal.status = RADIANT_RADIO_STATUS_OK;
	hal.t_sync = r->t_sync;
	hal.t_sync_exact = r->t_sync_exact;
	hal.has_rssi = r->has_rssi;
	hal.rssi_dbm = r->rssi_dbm;

	api_post_rx(channel, (uint8_t)ANTW_MESG_BROADCAST_DATA_ID, &r->id, &hal,
		    r->frame.payload, r->frame.payload_len, 0u, false);

	/* ACQUIRE leaves the search; a scan stays SEARCHING, so radiant_search_end() is skipped. */
	if (radiant_channel_state_get(channel) == RADIANT_CH_STATE_TRACKING) {
		(void)radiant_search_end(&api_search, channel);
	}
}

/* Search timeout is raised by radiant_channel.c, which orders
 * RX_SEARCH_TIMEOUT before CHANNEL_CLOSED. Callers pass
 * RADIANT_SEARCH_TIMEOUT_NONE, so this never fires. */
static void api_search_timeout(uint8_t channel, void *user)
{
	(void)channel;
	(void)user;
}

/* May this channel acquire this device? Asked by radiant_search before it
 * spends anything on the answer. See api_id_list_permits(). */
static bool api_search_permit(uint8_t channel, const struct radiant_channel_id *id,
			      void *user)
{
	(void)user;
	if (!api_ch_valid(channel) || id == NULL) {
		return false;
	}
	return api_id_list_permits(channel, id);
}

static const struct radiant_search_cbs api_search_cbs = {
	.acquired = api_search_acquired,
	.timeout = api_search_timeout,
	.permit = api_search_permit,
};

/* Does the channel's network have a key installed? */
static bool api_net_keyed(uint8_t ch)
{
	uint8_t net = radiant_channel_network_get(ch);

	if (net >= API_NETWORKS) {
		net = 0u;
	}
	return api_net_valid[net];
}

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

	/*
	 * A channel on a keyed network declares its RF, and the sweep follows it. A
	 * keyless channel declares nothing: the sweep hears only the ANT+ address, so
	 * its RF says nothing about where ANT+ devices are. Declaring it would drag
	 * the sweep off the frequency they are on.
	 */
	if (api_net_keyed(ch)) {
		(void)radiant_search_begin_rf(&api_search, ch, mode, &want, now,
					      RADIANT_SEARCH_TIMEOUT_NONE,
					      api_rf_of(ch));
	} else {
		(void)radiant_search_begin(&api_search, ch, mode, &want, now,
					   RADIANT_SEARCH_TIMEOUT_NONE);
	}
}

/* ---------------------------------------------------------------------------
 * Posting requests to the scheduler. Thread context, under api_lock.
 * ---------------------------------------------------------------------------
 */

/*
 * True while a radiant_sched_*() call that may complete synchronously is in
 * progress. A refused arm completes inline, so posting from that completion would
 * recurse without bound against a backend that refuses every arm. A refused arm
 * leaves the retry to the next (rate-limited) pump.
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
		return;
	}

	/* One sweep at a time: radiant_search.c holds one window in flight,
	 * which keeps its filters[] pointer valid for the HAL. */
	if (api_search_slot != RADIANT_SCHED_CH_NONE) {
		return;
	}

	/*
	 * One sweep serves every searching channel, riding the first one whose slot is
	 * free: a busy slot is skipped, not waited on. any_searching lets the 0xF6
	 * SWEEP_BUSY note tell "none searching" from "one searching but busy".
	 */
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
		/* Only the BUSY half is noted: the idle half would climb forever and
		 * saturate the reply. See ANT_HEALTH_SWEEP_BUSY. */
		if (any_searching) {
			ant_health_note(ANT_HEALTH_SWEEP_BUSY);
		}
		return;
	}

	if (radiant_search_window(&api_search,
			      now + (radiant_time_t)caps->min_arm_lead_us,
			      &w) != RADIANT_SEARCH_OK) {
		return;
	}

	/*
	 * The sweep is a continuous background scan (t_close = RADIANT_TIME_NEVER), not
	 * a bounded window. A bounded window holds the radio for its whole dwell, so a
	 * tracked slave's window inside it is never armed and expires MISSED. The scan
	 * is capped by the scheduler's limit and pauses for later bounded requests.
	 * Do not compute a shorter limit here: a refused bounded post is a lost pass,
	 * while a declined scan chunk is free and reconsidered next pass.
	 *
	 * chunk_us is the dwell still owed to the current set: a ceiling, not a period.
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
	/* Never STOP_ON_FIRST here: the HAL forbids it with more than one
	 * filter, and several masters may transmit in one dwell. */
	req.stop_on_first = false;

	if (radiant_sched_request_rx(ch, &req) != RADIANT_RADIO_OK_RC) {
		radiant_search_arm_failed(&api_search);
		return;
	}

	api_search_slot = ch;
	api_bind_accepted(ch, API_SLOT_SEARCH);
	/* Not radiant_search_armed() here: the scheduler decides the chunk
	 * bounds, so api_sched_armed() credits the dwell once per granted chunk. */
}

/*
 * Follow-on reserve: air an arbitrated backend must hold past an operation's end
 * for the reply the core may arm from its completion (struct radiant_rx_req::
 * follow_on_us). Zero cost on a backend that owns the radio outright. Derived
 * here, the only file holding both the transfer turnaround constants and the slot kinds.
 */

/*
 * One ANT+ frame at 1 Mbit/s: 1 preamble + 5 address + 10 body + 2 CRC = 18 B =
 * 144 us, rounded up to 150. A compile-time budget for the BUILD_ASSERTs below;
 * rounding errs toward over-reserving.
 */
#define API_FRAME_AIRTIME_US 150u

/*
 * A command can arrive after this RX is armed, so reserve the whole possible slave
 * data and ACK exchange up front. An empty window returns the unused air. This is
 * a grant budget, not a wider receive window.
 */
#define API_FOLLOW_ON_TRACK_US (RADIANT_TRANSFER_SLOT_REPLY_US + \
	RADIANT_TRANSFER_REPLY_US + RADIANT_TRANSFER_ACK_GUARD_US + API_FRAME_AIRTIME_US)

/* Separate from MERGE_SPAN, which caps the receive union. The slave data
 * and ACK exchange needs 4150 us past the latest receive sync; the 4200 us
 * ceiling stops a timing change from silently growing every grant. Check
 * before casting, since uint16_t truncation would hide overflow. Merged
 * requests take MAX of the members' reserves, never SUM. */
BUILD_ASSERT(API_FOLLOW_ON_TRACK_US <= 4200u,
	     "tracked follow-on exceeds the reviewed 4.2 ms grant budget");
BUILD_ASSERT(API_FOLLOW_ON_TRACK_US <= UINT16_MAX,
	     "tracked follow-on does not fit the HAL field");
/* A master's slot-opening 0x8A is acknowledged on the slot-reply
 * turnaround (radiant_ack.c), the latest reply, so the reserve must reach
 * its end too. */
BUILD_ASSERT(RADIANT_TRANSFER_OPENER_REPLY_US + API_FRAME_AIRTIME_US <=
		     API_FOLLOW_ON_TRACK_US,
	     "the tracked reserve does not cover an opener's acknowledgement");

/*
 * The turnaround's own reserve, from its t_close: a slave's acknowledged data
 * at the window's late edge is answered REPLY_US later, plus that reply's air.
 */
#define API_FOLLOW_ON_MASTER_RX_US                                       \
	((uint16_t)(RADIANT_TRANSFER_REPLY_US +                          \
		    RADIANT_TRANSFER_ACK_GUARD_US + API_FRAME_AIRTIME_US))

/*
 * A listening master's reserve covers its slave's reply (RADIANT_TRANSFER_SLOT_REPLY_US
 * after its own t_sync) and the turnaround's own reserve. An arbitrated backend serves
 * the turnaround from this grant only if everything it reserves fits inside it.
 */
#define API_FOLLOW_ON_MASTER_TX_US                                       \
	((uint16_t)(RADIANT_TRANSFER_SLOT_REPLY_US +                     \
		    RADIANT_TRANSFER_ACK_GUARD_US + API_FRAME_AIRTIME_US + \
		    API_FOLLOW_ON_MASTER_RX_US))

BUILD_ASSERT(RADIANT_TRANSFER_SLOT_REPLY_US + RADIANT_TRANSFER_ACK_GUARD_US +
		     API_FRAME_AIRTIME_US + RADIANT_TRANSFER_REPLY_US +
		     RADIANT_TRANSFER_ACK_GUARD_US + API_FRAME_AIRTIME_US <=
		     UINT16_MAX,
	     "the master follow-on does not fit the HAL field");

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
	/* Per-channel window from the guard estimate; reverts to the wide
	 * default when not tracking. */
	guard = (radiant_time_t)radiant_channel_guard_us(ch);
	req.t_open = (t > guard) ? (t - guard) : 0u;
	req.t_close = t + guard;
	/* Request CRC failures only when repair uses them, so the noise floor
	 * does not cross the HAL. */
	req.report_crc_fail = api_crc_repair_on;
	/* A request, not a guarantee: the scheduler drops it if the window
	 * merges. Worth asking, since it frees the slot early on a single-channel
	 * window. */
	req.stop_on_first = true;
	/* Reserved unconditionally: any frame here may become a transmit 1.56 ms
	 * later, depending on a control byte not yet read. An empty window gives
	 * it back. SLAVE_RX_ONLY never transmits, so it reserves nothing. */
	req.follow_on_us =
		(radiant_channel_type_get(ch) ==
		 (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY) ?
			0u : API_FOLLOW_ON_TRACK_US;

	if (radiant_sched_request_rx(ch, &req) != RADIANT_RADIO_OK_RC) {
		api_bind_rejected(ch);
		return;
	}
	api_bind_accepted(ch, API_SLOT_TRACK_RX);
}

/*
 * A master listens for slave replies (acknowledged data, burst starts) in its slot
 * turnaround. Without this window a master never hears a reply, and EVENT_TX looks
 * the same either way. Window: RADIANT_TRANSFER_SLOT_REPLY_US (2190 us) after the
 * master's t_sync, +/- RADIANT_TRANSFER_ACK_GUARD_US. Same on-air address as the
 * master's TX. Never STOP_ON_FIRST, since several slaves may answer. Costs ~500 us
 * of receive per period, unconditionally, since a slave may reply in any slot.
 */
static void api_post_master_rx(uint8_t ch, radiant_time_t t_sync)
{
	struct radiant_channel_id id;
	struct radiant_sched_rx   req;
	int                       n;

	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		RADIANT_DIAG_INC(mrx_skipped);
		return;
	}

	n = radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING, api_net_addr_for(ch), &id,
			   api_ch[ch].filter.addr,
			   sizeof(api_ch[ch].filter.addr));
	if (n < 0) {
		RADIANT_DIAG_INC(mrx_skipped);
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
	 * The same reserve the tracked window takes, for the same reason. A slave's
	 * acknowledged-data packet arrives in this window and its reply goes out
	 * RADIANT_TRANSFER_REPLY_US later. Without follow_on_us an arbitrated backend
	 * grants air only to t_close, so the reply would never leave. This is a
	 * separate constant from API_FOLLOW_ON_TRACK_US: the reply is due REPLY_US after
	 * the data packet's t_sync, which is at most this window's late edge. The
	 * transmit's reserve (API_FOLLOW_ON_MASTER_TX_US) must cover this one too.
	 */
	req.follow_on_us = API_FOLLOW_ON_MASTER_RX_US;

	if (radiant_sched_request_rx(ch, &req) != RADIANT_RADIO_OK_RC) {
		RADIANT_DIAG_INC(mrx_refused);
		api_bind_rejected(ch);
		return;
	}
	RADIANT_DIAG_INC(mrx_posted);
	api_bind_accepted(ch, API_SLOT_MASTER_RX);
}

/* Bit 4 is the master bit, but MASTER_TX_ONLY (0x50) does not listen, so test the type. */
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
	uint8_t                    pay[RADIANT_TRANSFER_PKT_BYTES];
	radiant_time_t             t = radiant_channel_next_slot(ch);

	if (t == RADIANT_TIME_NEVER) {
		return;
	}
#if defined(CONFIG_RADIANT_SEC)
	/*
	 * A slot the transform withheld (pending, past the epoch ceiling, spent)
	 * was never requested, so nothing completed it and the clock did not
	 * move: t is then a slot already gone. Requested as it stands, the
	 * scheduler refuses it, the MISSED terminal advances one period, and a
	 * secured master is re-posted only by the next pump, so the master
	 * caught up one period per pump and stayed silent meanwhile. Skip the
	 * slots that have passed here instead. The pump runs at least every
	 * RADIANT_API_HOUSEKEEP_MS, so this is one or two periods at most.
	 */
	{
		radiant_time_t now = radiant_radio_now();

		while (t != RADIANT_TIME_NEVER && t <= now) {
			(void)radiant_channel_on_slot_missed(ch, now);
			t = radiant_channel_next_slot(ch);
		}
		if (t == RADIANT_TIME_NEVER) {
			return;
		}
	}
#endif
	if (radiant_channel_id_get(ch, &id) != RADIANT_CH_OK) {
		return;
	}

	/*
	 * A master transmits from its first slot with or without host data. Refusing
	 * until written would deadlock hosts, which pace writes off EVENT_TX, so an
	 * unwritten buffer goes out as zeros.
	 */

	/* 0x0A: slot opener, all other flags clear. Built from fields so
	 * radiant_frame_make() refuses other combinations. */
	memset(&fields, 0, sizeof(fields));
	fields.slot_opener = true;

	/* The host's buffer, or the last transfer packet if one went out since
	 * the host last wrote (see last_tx). Copied under the crit section: the
	 * TX callback writes last_tx from radio context. */
	{
		unsigned int key = radiant_event_crit_enter();

		memcpy(pay, api_ch[ch].last_tx_newer ? api_ch[ch].last_tx :
			    api_ch[ch].bcast, sizeof(pay));
		api_ch[ch].bcast_gen_tx = api_ch[ch].bcast_gen;
		radiant_event_crit_exit(key);
	}

#if defined(CONFIG_RADIANT_SEC)
	/*
	 * TX hook, under api_lock, just before the frame is built (the TX body is DMA'd
	 * when armed, so this is the latest correct point). Transform into a copy:
	 * bcast's readback must stay the host's plaintext, and must not be re-encrypted
	 * on each retransmission.
	 */
	memcpy(api_ch[ch].sec_pay, pay, sizeof(api_ch[ch].sec_pay));
	api_ch[ch].sec_pay_valid = false;
	/* The counter derives from the slot instant t, so a re-post of the same
	 * slot gets the same bytes. */
	if (radiant_sec_tx_transform(ch, api_ch[ch].sec_pay,
				     (uint8_t)sizeof(api_ch[ch].sec_pay),
				     (uint64_t)t) != RADIANT_SEC_OK) {
		/* Transmit nothing rather than send in the clear: a channel the
		 * host believes is protected must not fail open. */
		return;
	}
	api_ch[ch].sec_pay_valid = radiant_sec_channel_is_secured(ch);

	if (radiant_frame_make(&f, &id, &fields, api_ch[ch].sec_pay,
			   RADIANT_FRAME_PAYLOAD_STD) != RADIANT_FRAME_OK) {
		return;
	}
#else
	if (radiant_frame_make(&f, &id, &fields, pay,
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

	/* Per-channel buffer: the HAL DMAs from it until the completion callback. */
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
	/* Only a listening master needs the reserve: MASTER_TX_ONLY arms no turnaround. */
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

	/* Apply a deferred network-0 key once no sweep chunk is in flight. The
	 * re-init orphans nothing with no carrier; dropped members rejoin below. */
	if (api_search_reinit_pending &&
	    api_search_slot == RADIANT_SCHED_CH_NONE) {
		struct radiant_search_cfg scfg;

		api_search_cfg_build(&scfg);
		(void)radiant_search_init(&api_search, &scfg, &api_search_cbs,
				      NULL);
		api_search_reinit_pending = false;
	}

	for (ch = 0u; ch < API_CHANNELS; ch++) {
		enum radiant_channel_state st = radiant_channel_state_get(ch);
		bool searching = radiant_search_is_searching(&api_search, ch);

		/* Keep the sweep's membership in step with the state machine. A
		 * channel that fell back to SEARCHING rejoins here, steered at once
		 * and then revisited with backoff (RADIANT_SEARCH_REVISIT_FIRST). */
		if (st == RADIANT_CH_STATE_SEARCHING) {
			if (!searching) {
				api_search_begin(ch, now);
			}
		} else if (searching) {
			/* api_search_slot is left alone: the window in flight keeps
			 * serving other searching channels until its own completion
			 * releases it. Clearing it here would start a second live sweep. */
			(void)radiant_search_end(&api_search, ch);
		}

		if (radiant_sched_pending(ch)) {
			continue;
		}
		/* The transfer engine arms through the same slot, with the tighter
		 * deadline (1.55 ms against a 250 ms period). */
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

	/* The one commit. An arm refused here completes synchronously; see api_arming. */
	api_arming = true;
	(void)radiant_sched_tick();
	api_arming = false;
	api_stats.pumps++;
}

/* ---------------------------------------------------------------------------
 * The scheduler's calls back into us. Usually radio interrupt context;
 * synchronous arm failures/cancellation can also complete in thread context
 * under the scheduler critical section (see api_sched_done).
 * ---------------------------------------------------------------------------
 */

/* Resolves the CRC-repair and profile-sanity switches once, at antr_init(). */
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
		/* Single-bit errors sharing a syndrome should not occur for this
		 * CRC on a frame this short. If they do, repair is unsafe. */
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

	/* Matched address bytes never reach RAM, so the address half is
 * regenerated from the ID this window filtered on. Status OK implies a
 * verified CRC. */
	memset(&w, 0, sizeof(w));
	n = radiant_frame_addr(RADIANT_FRAME_CFG_TRACKING, api_net_addr_for(ch), &id,
			   w.addr, sizeof(w.addr));
	if (n < 0) {
		return false;
	}
	w.addr_len = (uint8_t)n;
	memcpy(w.body, evt->body, evt->body_len);
	w.body_len = evt->body_len;

	/* Repair runs here, where address and body are both present (the CRC covers both). */
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
			/* A mis-repair in the control byte is usually rejected here by an
			 * existing check. Counted separately. */
			api_stats.crc_repair_refuted++;
		}
		return false;
	}

	/* Repaired frames are broadcasts only: acknowledged or burst data would
	 * be ACKed on air from a possible mis-repair. A repaired frame never
	 * makes this node transmit. */
	if (crc_failed &&
	    radiant_frame_msg_type(f.ctrl_byte) != RADIANT_MSG_BROADCAST) {
		api_stats.crc_repair_refuted++;
		return false;
	}

	/*
	 * Byte 0 of a tracked body is the transmission type, which the address match
	 * cannot prove. For repaired frames only: a mismatch means a mis-repair. On a
	 * clean frame it is a fact about the air, not evidence of a bad repair.
	 */
	if (crc_failed && id.trans_type != 0u &&
	    f.payload_len > 0u && w.body[0] != id.trans_type) {
		api_stats.crc_repair_refuted++;
		LOG_DBG("crc repair ch=%u refuted: ttype %u != %u", (unsigned)ch,
			(unsigned)w.body[0], (unsigned)id.trans_type);
		return false;
	}

	/*
	 * The only check reading the payload: radiant_profile_sanity.c rejects power
	 * or heart rate that is physically impossible. Repaired frames only; a clean
	 * implausible frame is a fact about the sensor and is never dropped.
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
		/* Counted here, not at the repair: this means repaired AND delivered. */
		api_stats.crc_repaired++;
	}

	/*
	 * A slave re-anchors its slot clock on the frame heard. A master must not: it
	 * owns the slot grid, and the frame it hears is a slave's reply 2.19 ms into
	 * the period, so re-anchoring would drag its phase forward each exchange.
	 */
	if (!radiant_channel_is_master(ch)) {
		radiant_channel_on_slot(ch, evt->t_sync);
		radiant_search_seen_note_rf(&api_search, &id, evt->t_sync,
					    api_rf_of(ch));
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
		 * RX policy guard: classification only, no crypto, so it is safe in ISR context.
		 * XOR, CMAC and verdict run later in radiant_sec_pump(), which keeps ordering.
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
		/* After the policy guard, so a frame it defers or drops never
		 * opens a pending command's slot. */
		if (!crc_failed && api_ch[ch].ack_pending &&
		    !radiant_channel_is_master(ch)) {
			api_ack_pending_send(ch, evt->t_sync);
		} else if (!crc_failed && api_ch[ch].bcast_pending &&
			   !radiant_channel_is_master(ch)) {
			api_post_slave_bcast(ch, evt->t_sync);
		}
		api_post_rx(ch, (uint8_t)ANTW_MESG_BROADCAST_DATA_ID, &id, evt,
			    f.payload, f.payload_len, 0u, false);
		break;
	case RADIANT_MSG_BURST_DATA:
	case RADIANT_MSG_BURST_LAST:
#if defined(CONFIG_RADIANT_SEC)
		/*
		 * D15: this switch also decodes acknowledged data and bursts. Without this
		 * check an injector could send a secured-range page as acknowledged data
		 * and skip the MAC. Secured-range pages are dropped here.
		 */
		if (radiant_sec_rx_ingest(ch, f.payload, f.payload_len, false,
					  (uint64_t)evt->t_sync) ==
		    RADIANT_SEC_RX_DROP) {
			LOG_DBG("sec ch=%u dropped non-broadcast page 0x%02X",
				(unsigned)ch, (unsigned)f.payload[0]);
			break;
		}
#endif
		/* radiant_transfer_on_data() arms the ack within 1.55 ms, before the
		 * payload goes up. The host message is posted from api_xfer_rx_data(). */
		/* Kept out of the LOG_DBG argument list: side effects there vanish
		 * in release builds. */
		api_stats.rx_on_data++;
		rc = radiant_transfer_on_data(&api_xfer[ch], &f, evt->t_sync);
		LOG_DBG("on_data rc=%d", rc);
		break;
	case RADIANT_MSG_BURST_ACK:
	case RADIANT_MSG_TRANSFER_ACK:
	case RADIANT_MSG_UNKNOWN:
	default:
		/* An acknowledgement with no transfer behind it, or an unmeasured
		 * flag combination. Not counted in 0xF6: nothing the host awaited
		 * was lost. */
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
	if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_MASTER_RX &&
	    evt->status == RADIANT_RADIO_STATUS_OK) {
		RADIANT_DIAG_INC(mrx_frames);
	}

	if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_SEARCH) {
		/* Use the indexed form: the scheduler has already remapped a merged
		 * window's filter_index to this channel's filters. */
		radiant_search_on_rx_indexed(&api_search, evt, filter_index);
		return;
	}

	if (evt->status != RADIANT_RADIO_STATUS_OK) {
		/*
		 * CRC failures are noise floor (~1.4/s in a quiet room). On a tracked window
		 * with repair on, a matched address means this is the tracked sensor, likely
		 * one bit off, so the checks below mirror the repair module's.
		 */
		if (!api_crc_repair_on || !evt->has_crc_rx) {
			return;
		}
		if (radiant_channel_op_owner(api_ch[ch].op) != (int)ch) {
			/* Same condition as the ownership check below, without LOG_DBG:
			 * this runs on CRC_FAIL events. */
			ant_health_note(ANT_HEALTH_RX_NOT_OWNER);
			return;
		}
		if (!radiant_transfer_is_idle(&api_xfer[ch])) {
			/* A transfer in flight is arming an ack within a 1.55 ms
			 * turnaround, so it is not offered repaired frames. */
			return;
		}
		if (api_tracked_frame(ch, evt, true)) {
			/* The slot was heard; counting it as a miss would send a working
			 * channel back to search. */
			api_ch[ch].slot_heard = true;
		}
		return;
	}

	/*
	 * Ownership: the event must belong to the request the channel still owns.
	 * evt->op is the HAL's id; radiant_channel.c holds the surrogate minted here.
	 * The transfer engine posts its own requests, and scheduler routing (not the
	 * retired tracking token) establishes ownership of the ACK window.
	 */
	if (radiant_transfer_state(&api_xfer[ch]) == RADIANT_TRANSFER_STATE_WAIT_ACK) {
		api_stats.rx_to_xfer++;
		(void)radiant_transfer_on_rx_event(&api_xfer[ch], evt);
		return;
	}
	/* An inbound burst's continuation window. Its packets must never reach
	 * api_tracked_frame(): that re-anchors a slave's slot clock, and each
	 * burst packet would walk it by 3.11 ms. The one frame handed back is a
	 * broadcast, which ends the burst. */
	if (radiant_transfer_state(&api_xfer[ch]) == RADIANT_TRANSFER_STATE_WAIT_NEXT) {
		api_stats.rx_to_xfer++;
		if (!radiant_transfer_on_rx_event(&api_xfer[ch], evt)) {
			(void)api_tracked_frame(ch, evt, false);
		}
		return;
	}
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
		 * api_tracked_frame(), so a wedged engine looks like silence. */
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
		/* EVENT_TRANSFER_TX_START (0x0A): the first packet of a multi-packet
		 * burst. Not raised for acknowledged data, a one-packet burst or a
		 * retry. */
		if (evt->status == RADIANT_RADIO_STATUS_OK &&
		    radiant_transfer_cur_starts_burst(&api_xfer[ch])) {
			(void)radiant_event_post_channel_event(
				ch, (uint8_t)ANTW_EVENT_TRANSFER_TX_START);
		}
		if (radiant_channel_is_master(ch) &&
		    radiant_transfer_state(&api_xfer[ch]) == RADIANT_TRANSFER_STATE_TX_DATA &&
		    evt->status == RADIANT_RADIO_STATUS_OK) {
			const uint8_t *sent =
				radiant_transfer_cur_payload(&api_xfer[ch]);

			/* Only packet 0's first attempt occupies the master's slot.
			 * Re-anchoring on continuation packets or retries would walk the
			 * grid by the burst length. */
			if (radiant_transfer_cur_is_slot(&api_xfer[ch])) {
				radiant_channel_on_slot(ch, evt->t_sync);
			}
			if (sent != NULL) {
				memcpy(api_ch[ch].last_tx, sent,
				       RADIANT_TRANSFER_PKT_BYTES);
				api_ch[ch].last_tx_newer = true;
			}
		}
		radiant_transfer_on_tx_event(&api_xfer[ch], evt);
		return;
	}

	/* A slave's broadcast went out in its master's reply window. Not a slot
	 * of this channel's own (the master's frame already anchored the clock),
	 * so no radiant_channel_on_slot(); the next tracked window is re-posted
	 * from api_sched_done(). */
	if (!radiant_channel_is_master(ch)) {
		if (evt->status == RADIANT_RADIO_STATUS_OK) {
			/* A write that landed after the snapshot is still
			 * pending: it goes out on the next master frame. */
			if (api_ch[ch].bcast_gen_tx == api_ch[ch].bcast_gen) {
				api_ch[ch].bcast_pending = false;
			}
			api_stats.slave_bcast_sent++;
			(void)radiant_event_post_channel_event(ch,
							       (uint8_t)ANTW_EVENT_TX);
		}
		api_ch[ch].track_repost = true;
		return;
	}

	/* Our own broadcast went out. EVENT_TX is the outcome; broadcast is
	 * unacknowledged by design so there's no failure event. */
	api_ch[ch].slot_heard = true;
	if (api_ch[ch].bcast_gen_tx == api_ch[ch].bcast_gen) {
		api_ch[ch].bcast_pending = false;
	}
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
	case RADIANT_TRANSFER_STATE_WAIT_ACK:
	case RADIANT_TRANSFER_STATE_WAIT_NEXT: {
		struct radiant_rx_event e;

		memset(&e, 0, sizeof(e));
		e.op = RADIANT_TRANSFER_OP_EXTERNAL;
		/* An empty window is TIMEOUT: the engine retries a burst packet (Rev
		 * 5.1 sec 5.4.3) and otherwise fails with FAIL_NO_ACK. In WAIT_NEXT,
		 * TIMEOUT repeats our acknowledgement, or ends the burst after LAST;
		 * any other status ends it. */
		e.status = st;
		e.t_sync = radiant_radio_now();
		(void)radiant_transfer_on_rx_event(&api_xfer[ch], &e);
		break;
	}
	case RADIANT_TRANSFER_STATE_IDLE:
	case RADIANT_TRANSFER_STATE_WAIT_BLOCK:
	default:
		break;
	}
}

/*
 * A window was armed with the bounds it really got. Only the sweep uses this: the
 * scheduler arms scan chunks from pending work this layer cannot see, so the
 * proposed bounds would overcount.
 */
static void api_sched_armed(uint8_t ch, radiant_time_t t_open, radiant_time_t t_close,
			void *user)
{
	(void)user;
	if (!api_ch_valid(ch)) {
		return;
	}
	/*
	 * A tracked window that really got armed owes its successor. Set here, from a
	 * terminal rather than a post, which keeps it bounded. A window that never
	 * armed reaches api_sched_done() as MISSED or DENIED and sets the flag there.
	 */
	if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_TRACK_RX) {
		api_ch[ch].track_repost = true;
	}
	if (api_search_slot != ch ||
	    api_ch[ch].slot_kind != (uint8_t)API_SLOT_SEARCH) {
		return;
	}
	/* RADIANT_SEARCH_OP_EXTERNAL: the scheduler merges channels, so there
	 * is no HAL op id; the module trusts our routing. */
	radiant_search_armed(&api_search, RADIANT_SEARCH_OP_EXTERNAL, t_open, t_close);
}

/* Can run in thread context: pass() may report a missed arm synchronously,
 * under the scheduler critical section. A pending slave command is sent
 * from the frame (api_ack_pending_send()), not from here. */
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
	if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_MASTER_RX) {
		if (why == RADIANT_SCHED_DONE_OK) {
			RADIANT_DIAG_INC(mrx_done_ok);
		} else if (why == RADIANT_SCHED_DONE_MISSED) {
			RADIANT_DIAG_INC(mrx_done_missed);
		} else {
			RADIANT_DIAG_INC(mrx_done_other);
		}
	}

	if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_SEARCH) {
		/*
		 * A scan chunk ended. OK credits the whole chunk; ABORTED (a nearer deadline
		 * took the radio) credits the time actually listened; MISSED, FAILED and
		 * DENIED credit nothing. DENIED credits zero on purpose: under an arbiter the
		 * sweep is the elastic consumer (ADR 0013), and crediting unopened chunks would
		 * advance the address set on a fraction of its real dwell.
		 */
#if defined(CONFIG_RADIANT_SEARCH_PHASE_DIAG)
		phase_diag_chunk(why, now);
#endif
		radiant_search_on_done(&api_search, why == RADIANT_SCHED_DONE_OK,
				   why == RADIANT_SCHED_DONE_ABORTED, now);
		/*
		 * A denied chunk gives zero dwell, but a channel's ACQUIRE deadline keeps
		 * running, so each denial extends it by one housekeeping interval. The
		 * deadline that fires is the channel layer's (radiant_channel_tick()).
		 */
		if (why == RADIANT_SCHED_DONE_DENIED) {
			radiant_search_note_denied(&api_search,
						RADIANT_API_HOUSEKEEP_MS * 1000u);
			radiant_channel_search_note_denied(
				now, RADIANT_API_HOUSEKEEP_MS * 1000u);
		}
		/*
		 * A continuous request outlives its chunks, so the slot is kept across them.
		 * Dropping it at each chunk end would round-trip every chunk through the
		 * event thread and slow the sweep. Drop it only when the set finishes, since
		 * the filters describe one address set. MISSED and FAILED drop the slot and
		 * let the rate-limited pump decide.
		 */
		if (api_search_slot == ch) {
			/*
			 * DENIED counts as "ran" here, unlike the dwell credit: the scheduler may
			 * keep the request and retry on its own way out. Its EDENIED path ends the
			 * pass on a kept request, so a retry costs one refused arm per pass.
			 */
			bool ran = (why == RADIANT_SCHED_DONE_OK) ||
				   (why == RADIANT_SCHED_DONE_ABORTED) ||
				   (why == RADIANT_SCHED_DONE_DENIED);

			/* The channel state, not only search membership: the
			 * pump syncs membership one pass late, and a channel the
			 * tick has just closed must not get a search op re-bound
			 * after on_terminal() below has raised CHANNEL_CLOSED. */
			if (ran && !radiant_search_set_complete(&api_search) &&
			    radiant_search_is_searching(&api_search, ch) &&
			    radiant_channel_state_get(ch) ==
				    RADIANT_CH_STATE_SEARCHING) {
				/* Re-bound below after the clear: without slot_kind SEARCH the
				 * sweep would stop advancing. */
				keep_search = true;

				/* Lower the ceiling to the remaining budget, or the set's last
				 * chunk overruns it. */
				(void)radiant_sched_rechunk(
					ch,
					radiant_search_dwell_remaining(&api_search));
			} else {
				/* Cancel is silent, so no second done() arrives; the pump posts
				 * the next request. */
				(void)radiant_sched_cancel(ch);
				api_search_slot = RADIANT_SCHED_CH_NONE;
			}
		}
	} else if (!radiant_transfer_is_idle(&api_xfer[ch]) &&
		   !radiant_sched_pending(ch)) {
		/* If the engine has not re-posted, this completion ends its
		 * operation. If it has, the slot holds the new request. */
		api_feed_xfer_terminal(ch, st);
	} else if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_MASTER_RX) {
		/* A master's turnaround closing needs no action: the clock is already
		 * anchored by its transmit, and an empty turnaround is normal, so
		 * there is no RX_FAIL. */
	} else if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_MASTER_TX &&
		   !api_ch[ch].slot_heard) {
		/*
		 * A master's slot that did not go out: the clock must move. This trusts that
		 * the binding still describes this request, which is why
		 * api_xfer_superseded() clears the kind when a host transfer takes the slot.
		 * The clock advances from the missed slot, not from now, and a DENIED
		 * transmit gets the same advance, with its own counter.
		 */
		if (why == RADIANT_SCHED_DONE_DENIED) {
			api_stats.slots_denied++;
			(void)radiant_channel_on_slot_denied(ch, now);
		} else {
			api_stats.slots_missed++;
			(void)radiant_channel_on_slot_missed(ch, now);
		}
		/*
		 * The master owes itself the next slot now, not after the 50 ms pump. The
		 * re-post is bounded: the clock advanced a full period past the dead slot,
		 * so the next slot is strictly later and cannot complete immediately.
		 * The flag routes it through the re-post block below, which checks state.
		 */
		api_ch[ch].track_repost = true;
	} else if (api_ch[ch].slot_kind == (uint8_t)API_SLOT_TRACK_RX &&
		   !api_ch[ch].slot_heard) {
		/*
		 * A predicted window ran and heard nothing. The clock advances from the missed
		 * slot, and after eight misses the channel returns to search
		 * (RX_FAIL_GO_TO_SEARCH).
		 * A DENIED window did not run: it charges the guard, not the miss counter, and
		 * raises no RX_FAIL, since a denial says nothing about the sensor.
		 */
		if (why == RADIANT_SCHED_DONE_DENIED) {
			api_stats.slots_denied++;
			(void)radiant_channel_on_slot_denied(ch, now);
		} else {
			api_stats.slots_missed++;
			if (!radiant_channel_on_slot_missed(ch, now)) {
				(void)radiant_event_post_channel_event(
					ch, (uint8_t)ANTW_EVENT_RX_FAIL);
			} else if (api_ch[ch].in_pkts != 0u) {
				/* Back to search with an inbound burst
				 * half-delivered: it cannot finish now. */
				api_inbound_failed(ch);
			}
		}
		/*
		 * The channel owes itself the next window now. A MISSED or DENIED window may
		 * never have armed, so nothing else re-posts it. The re-post is bounded: the
		 * clock advanced a full period, so the next window is strictly later. The
		 * flag routes it through the repost block below, which checks state.
		 */
		api_ch[ch].track_repost = true;
	}

	op = api_ch[ch].op;
	api_ch[ch].op = 0u;
	api_ch[ch].slot_kind = (uint8_t)API_SLOT_NONE;

	/* Every terminal reaches here, even with no channel: the HAL always
	 * delivers a cancelled operation's terminal. */
	(void)radiant_channel_on_terminal(op, st, now);

	/* Re-bind the sweep with a fresh op id: the scheduler arms the next
	 * chunk on the way out, and that arm needs slot_kind SEARCH. */
	if (keep_search) {
		api_bind_accepted(ch, API_SLOT_SEARCH);
	}

	/*
	 * A listening master's turnaround is posted here, not from api_sched_tx() (which
	 * would reuse the slot still in flight). The flag is cleared before the post and
	 * set only by a transmit that went out, so a refused arm cannot loop. The
	 * TRACKING check runs after the terminal, so a channel just closed gets no turnaround.
	 */
	if (api_ch[ch].turn_pending) {
		radiant_time_t t_sync = api_ch[ch].turn_t_sync;

		api_ch[ch].turn_pending = false;
		if (radiant_channel_state_get(ch) == RADIANT_CH_STATE_TRACKING) {
			api_post_master_rx(ch, t_sync);
		} else {
			RADIANT_DIAG_INC(turn_dropped);
		}
	}

	/*
	 * Keeps a background scan bounded while anything is tracked. The scheduler caps a
	 * scan chunk only for work it can already see, so the tracked window is re-posted
	 * here rather than a moment later from the pump: otherwise a scan is armed with no
	 * end and then preempted straight away. Re-post in both directions, in the same
	 * order as api_pump_locked(), so the two posts never disagree.
	 */
	if (api_ch[ch].track_repost) {
		api_ch[ch].track_repost = false;
		if (radiant_channel_state_get(ch) == RADIANT_CH_STATE_TRACKING &&
		    !radiant_sched_pending(ch) &&
		    radiant_transfer_is_idle(&api_xfer[ch])) {
			if (radiant_channel_is_master(ch)) {
				/* A secured master is not re-posted from this callback: the
				 * transform runs the shared AES key schedule, so the pump posts
				 * it in thread context under api_lock. */
				if (!radiant_sec_channel_is_secured(ch)) {
					api_post_master_tx(ch);
				}
			} else {
				api_post_track_rx(ch);
			}
		}
	}

	/*
	 * Post nothing else from here: against a backend that refuses every arm, posting
	 * from a radio callback is an unbounded loop. A kept sweep request on DENIED does
	 * not wake the event thread either, or a refused arm would spin the pump forever.
	 * A denied master TX is the exception, since its request was consumed.
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

/* Is this a state the radio must finish? Not WAIT_BLOCK: it waits on the
 * host, and terminating it would release a block the bridge still owns. */
static bool api_xfer_awaits_radio(uint8_t ch)
{
	switch (radiant_transfer_state(&api_xfer[ch])) {
	case RADIANT_TRANSFER_STATE_TX_DATA:
	case RADIANT_TRANSFER_STATE_TX_REPLY:
	case RADIANT_TRANSFER_STATE_WAIT_ACK:
	case RADIANT_TRANSFER_STATE_WAIT_NEXT:
	case RADIANT_TRANSFER_STATE_ABORTING:
		return true;
	case RADIANT_TRANSFER_STATE_IDLE:
	case RADIANT_TRANSFER_STATE_WAIT_BLOCK:
	default:
		return false;
	}
}

/*
 * Recovery watchdogs, run under api_lock. They should never fire on a healthy link.
 * They cover silent, permanent failures: a transfer with no completion coming, or an
 * acknowledgement that never got its opener.
 */
static void api_watchdogs_locked(radiant_time_t now)
{
	const radiant_time_t limit =
		(radiant_time_t)RADIANT_API_XFER_WATCHDOG_MS * 1000u;
	uint8_t ch;

	for (ch = 0u; ch < API_CHANNELS; ch++) {
		struct api_chan *c = &api_ch[ch];
		unsigned int key = radiant_event_crit_enter();

		if (c->ack_pending && now >= c->ack_deadline) {
			api_stats.xfer_no_opener++;
			api_ack_pending_fail(ch);
		}
		radiant_event_crit_exit(key);
		/* Backstop: every close path fails the command before closing;
		 * one that does not must still not leave it for unassign. */
		if (!radiant_channel_is_open(ch)) {
			api_ack_pending_fail(ch);
		}

		/* Non-idle with nothing armed means no done() is coming. */
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

		/* in_pkts clears only on LAST, so a burst that lost its LAST would
		 * mislabel later messages. Backstop for bursts the engine did not
		 * follow; skipped while the engine is busy. */
		if (c->in_pkts != 0u && c->in_last_valid &&
		    (now - c->in_last) >= limit &&
		    radiant_transfer_is_idle(&api_xfer[ch])) {
			LOG_DBG("ch=%u inbound transfer abandoned at seq %u",
				(unsigned)ch, (unsigned)c->in_pkts);
			/* Re-check under the crit section: the radio callback also resets it. */
			key = radiant_event_crit_enter();
			if (c->in_pkts != 0u) {
				api_inbound_failed(ch);
			}
			radiant_event_crit_exit(key);
		}
	}
}

static void api_housekeep(void)
{
	radiant_time_t now;

	k_mutex_lock(&api_lock, K_FOREVER);
	now = radiant_radio_now();

	/* A pending slave command fails before a timed-out search closes its
	 * channel, or unassign would wipe it with no outcome. */
	for (uint8_t ch = 0u; ch < API_CHANNELS; ch++) {
		if (radiant_channel_search_deadline(ch) <= now) {
			api_ack_pending_fail(ch);
		}
	}

	/* A SEARCHING channel past its deadline raises RX_SEARCH_TIMEOUT, then closes. */
	(void)radiant_channel_tick(now);

	/* Seen-cache expiry. The timeout half never fires; see api_search_timeout(). */
	radiant_search_tick(&api_search, now);

	/* Before the pump, so a transfer released here gets its slot re-posted in the same pass. */
	api_watchdogs_locked(now);

#if defined(CONFIG_RADIANT_SEC_PAIRING_X25519)
	/* The pairing release that radiant_sec_pair_on_search() cannot do from
	 * the radio callback. */
	radiant_sec_pair_service();
#endif
	api_pump_locked();
	api_stats.housekeeps++;

	/* Noise floor per frequency, once per interval, at INF so it shows on a
	 * bench log. Cleared after reporting, so each line covers its interval. */
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

	/* Receive-path counters, one line per interval, only when they moved.
	 * See struct radiant_api_stats::rx_sched_events. */
	{
		static radiant_time_t rxpath_last;
		static uint32_t       rxpath_seen;

		if (now - rxpath_last >= API_RXPATH_REPORT_US &&
		    api_stats.rx_sched_events != rxpath_seen) {
			/* Reported as a delta over the interval, since the counters are
			 * cumulative since boot. */
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
			LOG_INF("xfer failures: no_opener=%u no_ack=%u bad_ack=%u radio=%u",
				(unsigned int)api_stats.xfer_no_opener,
				(unsigned int)api_stats.xfer_no_ack,
				(unsigned int)api_stats.xfer_bad_ack,
				(unsigned int)api_stats.xfer_radio_failed);
			rxpath_last = now;
			rxpath_seen = api_stats.rx_sched_events;
		}
	}

#if defined(CONFIG_RADIANT_SEARCH_PHASE_DIAG)
	phase_diag_housekeep(now);
#endif

#if defined(CONFIG_RADIANT_CRC_REPAIR)
	/*
	 * CRC-repair counters, logged on a UART. Cumulative, not cleared per interval:
	 * a sensitivity run reads a rate as a delta across the run. A null result is
	 * only readable if the counters show repair engaged.
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

	k_mutex_unlock(&api_lock);
}

static void api_event_thread(void *a, void *b, void *c)
{
	(void)a;
	(void)b;
	(void)c;

	for (;;) {
		uint32_t n;

		/* The timeout drives housekeeping: radiant_sched.c has no timer of its own. */
		(void)k_sem_take(&api_event_sem, K_MSEC(RADIANT_API_HOUSEKEEP_MS));

		/* RX crypto runs under api_lock before the drain, so packets are in
		 * the ring when the drain delivers them. The lock keeps unassign from
		 * wiping state mid-pump. */
#if defined(CONFIG_RADIANT_SEC)
		k_mutex_lock(&api_lock, K_FOREVER);
		radiant_sec_pump();
		k_mutex_unlock(&api_lock);
#endif

		/* Drain outside api_lock: the drain calls antr_on_message(), and
		 * holding a lock across that can deadlock. */
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
	api_search_reinit_pending = false;
}

antr_err_t antr_init(void)
{
	struct radiant_search_cfg scfg;
	int                   rc;

	/* Taken although no other thread exists yet: radiant_sec_reset() asserts the lock. */
	k_mutex_lock(&api_lock, K_FOREVER);
	api_reset_state();
	k_mutex_unlock(&api_lock);
	memset(&api_stats, 0, sizeof(api_stats));

	/* Order matters: the event ring must exist before the radio is enabled,
	 * since an earlier post returns ESTATE. */
	radiant_channel_init();
	radiant_event_init();

	/* The scheduler owns radio events; the radio lifecycle stays in this file. */
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
	 * The adaptive guard needs a hardware t_sync. Without one, pin the floor at the
	 * ceiling and switch the mechanism off. Otherwise the floor is the core minimum
	 * plus two ticks of the backend's timebase.
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
		/* ENOTSUP: the backend advertises a wildcard device-number filter
		 * that the rx filter cannot express. Fail rather than fake it. */
		LOG_ERR("radiant_search_init: %d", rc);
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	api_inited = true;
#if defined(CONFIG_RADIANT_SWI)
	/* Claim the SWI trampoline before the event thread exists: pends are
	 * dropped until it is claimed. Failure is fatal, since every wakeup goes
	 * through it. */
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

	/* Abort the radio before resetting channels: the channel layer cannot
	 * abort an operation itself. */
	radiant_sched_reset();
	(void)radiant_radio_abort();

	for (i = 0u; i < API_CHANNELS; i++) {
		(void)radiant_transfer_abort(&api_xfer[i]);
		(void)radiant_search_end(&api_search, i);
	}

	radiant_channel_reset_all();

	/* Discard, do not drain: draining would re-enter antr_on_message(),
	 * which docs/sdk-ant-contract.md forbids. Pending ACK commands end
	 * without a terminal event, since the single-outcome guarantee does not
	 * span a reset. */
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
	 * An in-range network with no installed key is accepted with a warning, not
	 * refused: a refused assign leaves a host with no recovery path, and it can
	 * spin on close forever. A background scan on a keyless network still reports
	 * ANT+ devices, since the sweep uses the lowest keyed network's address. Only a
	 * channel that tracks on the zero address hears nothing. Prefer a warning the
	 * maintainer can read over an error the host cannot act on. Out-of-range network
	 * numbers are still refused by radiant_channel_assign().
	 */
	if (network > 0u && network < API_NETWORKS && !api_net_valid[network]) {
		LOG_WRN("ch%u assigned to network %u, which has no address "
			"installed - a background scan still works (the sweep "
			"uses the lowest keyed network), but tracking on it will hear nothing",
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
		if (api_search_slot == channel) {
			/* Same carrier release as antr_channel_close(): the
			 * cancel below is silent, so nothing else would. */
			radiant_search_on_done(&api_search, false, false, 0u);
			api_search_slot = RADIANT_SCHED_CH_NONE;
		}
		(void)radiant_sched_cancel(channel);
		/* Backstop: the wipe below must not be a command's silent end. */
		api_ack_pending_fail(channel);
		/* Keys and derived values go with the channel, or they'd stay
		 * installed on a number the host is free to reassign. */
		radiant_sec_channel_release(channel);
		memset(&api_ch[channel], 0, sizeof(api_ch[channel]));
		memset(&api_xfer[channel], 0, sizeof(api_xfer[channel]));
	}
	k_mutex_unlock(&api_lock);

	return rc;
}

/*
 * Warns once per open if a master's on-air address has a long alternating run,
 * which nRF receivers can miss (ADR 0019). Nothing is refused.
 */
static void api_warn_slip_prone_id(uint8_t ch)
{
	struct radiant_channel_id id;
	uint8_t run;

	if (!radiant_channel_is_master(ch) ||
	    radiant_channel_id_get(ch, &id) != RADIANT_CH_OK ||
	    id.device_number == 0u) {
		return;
	}
	run = radiant_channel_id_alt_run(api_net_addr_for(ch), id.device_number,
					 id.device_type);
	if (run >= RADIANT_ADDR_ALT_RUN_AVOID) {
		LOG_WRN("ch %u: device #%u type 0x%02x has a %u-bit alternating "
			"run in its address; nRF-based receivers can lose up to "
			"~5%% of its frames (ADR 0019). Prefer another device "
			"number",
			(unsigned int)ch, (unsigned int)id.device_number,
			(unsigned int)id.device_type, (unsigned int)run);
	}
}

/* Warns once per open for a slave off RF 57. On a keyed network the sweep
 * retunes to follow it; on a keyless one it stays put. */
static void api_warn_off_57_slave(uint8_t ch)
{
	uint8_t rf = api_rf_of(ch);

	if (radiant_channel_is_master(ch) || rf == RADIANT_RF_INDEX_ANT_PLUS) {
		return;
	}
	if (api_net_keyed(ch)) {
		LOG_WRN("ch %u: slave opened on RF %u (not %u): the discovery "
			"sweep retunes to follow it, and alternates passes "
			"when channels disagree",
			(unsigned int)ch, (unsigned int)rf,
			(unsigned int)RADIANT_RF_INDEX_ANT_PLUS);
	} else {
		LOG_WRN("ch %u: slave opened on RF %u (not %u) on a network with "
			"no key: the discovery sweep stays on RF %u and this "
			"channel takes what it hears there",
			(unsigned int)ch, (unsigned int)rf,
			(unsigned int)RADIANT_RF_INDEX_ANT_PLUS,
			(unsigned int)RADIANT_RF_INDEX_ANT_PLUS);
	}
}

antr_err_t antr_channel_open_with_offset(uint8_t channel, uint16_t offset)
{
	antr_err_t rc;

	k_mutex_lock(&api_lock, K_FOREVER);
	rc = radiant_channel_open(channel, offset, radiant_radio_now());
	if (rc == RADIANT_CH_OK) {
		int trc = api_xfer_setup(channel);

		/* A secured channel's receive windows start afresh, so one left
		 * pending before a close can't be judged by tag bytes heard
		 * after it (an alias 256 counters on). Nothing for any other
		 * channel: no context, an immediate return. */
		radiant_sec_rx_restart(channel);

		api_warn_slip_prone_id(channel);
		api_warn_off_57_slave(channel);

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

	/* An open inbound burst fails before CHANNEL_CLOSED, so the host hears
	 * EVENT_TRANSFER_RX_FAILED first. */
	if (api_ch_valid(channel) &&
	    (radiant_channel_state_get(channel) == RADIANT_CH_STATE_SEARCHING ||
	     radiant_channel_state_get(channel) == RADIANT_CH_STATE_TRACKING)) {
		enum radiant_transfer_state xs =
			radiant_transfer_state(&api_xfer[channel]);

		if (xs == RADIANT_TRANSFER_STATE_TX_REPLY ||
		    xs == RADIANT_TRANSFER_STATE_WAIT_NEXT) {
			(void)radiant_transfer_abort(&api_xfer[channel]);
		}
		if (api_ch[channel].in_pkts != 0u) {
			unsigned int key = radiant_event_crit_enter();

			if (api_ch[channel].in_pkts != 0u) {
				api_inbound_failed(channel);
			}
			radiant_event_crit_exit(key);
		}
	}

	rc = radiant_channel_close(channel, radiant_radio_now());
	if (rc == RADIANT_CH_OK) {
		uint32_t op = api_ch[channel].op;

		/* Also drops this channel's packets queued for the pump, so none
		 * opens a window after the close. */
		radiant_sec_rx_restart(channel);

		(void)radiant_transfer_abort(&api_xfer[channel]);
		api_ack_pending_fail(channel);
		(void)radiant_search_end(&api_search, channel);
		if (api_search_slot == channel) {
			/* This channel carried the sweep and its request is cancelled
			 * silently. Credit nothing, so the same set is retried. */
			radiant_search_on_done(&api_search, false, false, 0u);
			api_search_slot = RADIANT_SCHED_CH_NONE;
		}

		/* Cancel is silent, so no done() arrives; the terminal below raises
		 * ANTW_EVENT_CHANNEL_CLOSED exactly once. */
		(void)radiant_sched_cancel(channel);
		/* Finish an ABORTING transfer now, so its TX_FAILED is posted before
		 * the host can unassign. An armed request already finished in the
		 * abort, so this never double-reports. */
		if (radiant_transfer_state(&api_xfer[channel]) ==
		    RADIANT_TRANSFER_STATE_ABORTING) {
			api_feed_xfer_terminal(channel, RADIANT_RADIO_STATUS_ABORTED);
		}
		api_ch[channel].op = 0u;
		api_ch[channel].slot_kind = (uint8_t)API_SLOT_NONE;
		/* Per-channel state that must not outlive the channel: a
		 * pending turnaround, a half-received burst, both watchdogs. */
		api_ch[channel].turn_pending = false;
		api_ch[channel].track_repost = false;
		api_ch[channel].in_pkts = 0u;
		api_ch[channel].in_last_valid = false;
		api_ch[channel].xfer_stuck_valid = false;
		(void)radiant_channel_on_terminal(op, RADIANT_RADIO_STATUS_ABORTED,
					      radiant_radio_now());

		api_pump_locked();

#if defined(CONFIG_RADIANT_CRC_REPAIR)
		/* CRC-repair counters at close, so the last seconds before a close
		 * are logged. Cumulative across channels: read as a delta. */
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
	antr_err_t rc;

	k_mutex_lock(&api_lock, K_FOREVER);
	rc = radiant_channel_tx_power_set(channel, level, custom);
	/* The engine reads power at open; a change must reach its data and ack packets as well. */
	if (rc == RADIANT_CH_OK && api_xfer[channel].ready) {
		(void)radiant_transfer_set_power(&api_xfer[channel],
						 api_power_of(channel));
	}
	k_mutex_unlock(&api_lock);

	return rc;
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
	/* Device-wide and unmodeled: only enable == 0 (the default) is honest to accept. */
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
	/* No open-channel refusal: see antr_id_list_config(). */
	if (index >= API_ID_LIST_MAX) {
		return (antr_err_t)ANTW_INVALID_LIST_ID;
	}

	/* One aligned word store, so an interrupt preempting this sees the old
	 * entry or the new one. */
	uint32_t w;

	BUILD_ASSERT(API_ID_LIST_BYTES == sizeof(w));
	memcpy(&w, device_id, sizeof(w));
	k_mutex_lock(&api_lock, K_FOREVER);
	*(volatile uint32_t *)&api_ch[channel].id_list[index] = w;
	k_mutex_unlock(&api_lock);
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

antr_err_t antr_id_list_config(uint8_t channel, uint8_t size, uint8_t inc_exc)
{
	struct api_chan *c;
	bool exclude = (inc_exc != 0u);

	if (!api_ch_valid(channel)) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (size > API_ID_LIST_MAX) {
		return (antr_err_t)ANTW_INVALID_LIST_ID;
	}

	/*
	 * Accepted on an open channel: the Android ANT stack rewrites its exclusion
	 * list while searching, and a retail stick accepts this (Rev 5.1 sec
	 * 9.5.2.10/12 sets no channel-state condition). The list is consulted only at
	 * steer, offer and acquire time, so a change leaves a tracking channel alone.
	 *
	 * Store order matters for api_id_list_permits() in an interrupt: when the
	 * include/exclude sense flips, the list is switched off first and back on last,
	 * so no reader pairs old entries with the new sense. Off admits anything for
	 * those few instructions; api_search_acquired() re-checks against the result.
	 */
	c = &api_ch[channel];
	k_mutex_lock(&api_lock, K_FOREVER);
	if (c->id_list_exclude != exclude) {
		*(volatile uint8_t *)&c->id_list_size = 0u;
		compiler_barrier();
		*(volatile bool *)&c->id_list_exclude = exclude;
		compiler_barrier();
	}
	*(volatile uint8_t *)&c->id_list_size = size;
	k_mutex_unlock(&api_lock);
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

	/* Only the published ANT+ key is accepted; anything else is
	 * ANTW_INVALID_PARAMETER_PROVIDED (see the file header). */
	if (memcmp(key, api_ant_plus_key, sizeof(api_ant_plus_key)) != 0) {
		api_stats.key_rejects++;
		ant_health_note(ANT_HEALTH_KEY_REJECT);
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	/* Only a network's first valid key changes anything: it moves the
	 * sweep's address and turns that network's channels from keyless to
	 * keyed. */
	bool newly = !api_net_valid[network];

	api_net_addr[network][0] = RADIANT_NET_ADDR_ANT_PLUS_0;
	api_net_addr[network][1] = RADIANT_NET_ADDR_ANT_PLUS_1;
	/* The only place this is ever set. Everything above this line can fail,
	 * and each failure path returns without reaching here. */
	api_net_valid[network] = true;

	/* The sweep's address is fixed at radiant_search_init(), so a newly keyed
	 * network re-initialises it. Never under a live carrier, which would
	 * orphan that request: the re-init waits for the next pump with no
	 * carrier. */
	if (newly) {
		if (api_search_slot == RADIANT_SCHED_CH_NONE) {
			struct radiant_search_cfg scfg;

			api_search_cfg_build(&scfg);
			(void)radiant_search_init(&api_search, &scfg,
					      &api_search_cbs, NULL);
			api_search_reinit_pending = false;
			api_pump_locked();
		} else {
			api_search_reinit_pending = true;
		}
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
	/* Size 8 minimum, 11 maximum. Reject rather than pad, so a bridge bug is visible. */
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

	/* Two lengths: the caller sizes its buffer from request_type alone, so
	 * the wrong one shifts fields. */
	if (request_type != 0u) {
		memcpy(config, &api_adv_burst[1], sizeof(api_adv_burst) - 1u);
	} else {
		memset(config, 0,
		       ANTW_MESG_CONFIG_ADV_BURST_REQ_CAPABILITIES_SIZE);
		config[0] = (uint8_t)ANTW_ADV_BURST_MODES_SIZE_24_BYTES;
		/* Supported modes: none. Nothing hops, so advertising hopping would
		 * mislead a host. */
		config[1] = 0u;
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
	/* A bad channel is INVALID_MESSAGE, a bad mask
	 * INVALID_PARAMETER_PROVIDED. The ack-data bit is masked off first. */
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
	/* Compare the whole type nibble: masking only the master bit would read
	 * MASTER_TX_ONLY (0x50) as SLAVE_RX_ONLY (0x40). */
	if ((uint8_t)(radiant_channel_type_get(channel) &
		      (uint8_t)RADIANT_CH_STATUS_TYPE_MASK) ==
	    (uint8_t)ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY) {
		return (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE;
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	/*
	 * Refused only while a transfer this host started owns the channel. A write
	 * during TX_REPLY (acknowledging a peer) is accepted: the reply copied its
	 * payload when the frame arrived, and this write goes out on the next slot.
	 * Under the crit section, since the radio IRQ reads bcast.
	 */
	unsigned int key = radiant_event_crit_enter();
	enum radiant_transfer_state st = radiant_transfer_state(&api_xfer[channel]);

	/* WAIT_NEXT is treated like TX_REPLY: each new packet's acknowledgement
	 * copies bcast afresh. */
	if (st != RADIANT_TRANSFER_STATE_IDLE &&
	    st != RADIANT_TRANSFER_STATE_TX_REPLY &&
	    st != RADIANT_TRANSFER_STATE_WAIT_NEXT) {
		radiant_event_crit_exit(key);
		k_mutex_unlock(&api_lock);
		return (antr_err_t)ANTW_TRANSFER_IN_PROGRESS;
	}

	/* Copied before return. One payload per slot: a second write before the
	 * slot fires replaces the first. */
	memcpy(api_ch[channel].bcast, data, ANTW_ANT_MAX_PAYLOAD_SIZE);
	api_ch[channel].bcast_valid = true;
	api_ch[channel].bcast_pending = true;
	api_ch[channel].bcast_gen++;
	/* A host write supersedes the repeat of the last transfer packet. */
	api_ch[channel].last_tx_newer = false;
	radiant_event_crit_exit(key);

	api_pump_locked();
	k_mutex_unlock(&api_lock);

	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

/*
 * Transmit instant of an originated transfer, resolved from the channel's own grid.
 * radiant_transfer_submit() takes a slave's reply instant as the master's t_sync plus
 * RADIANT_TRANSFER_SLOT_REPLY_US, and a master's own slot instant. Passing
 * RADIANT_TIME_NEVER put the packet at an arbitrary phase of the period, where a
 * peer listening for a few hundred microseconds almost never hears it. The cost
 * is up to one channel period of latency; an acknowledged transfer is a slotted
 * exchange, so it cannot be sent early.
 */
static radiant_time_t api_xfer_t_sync_for(uint8_t ch)
{
	const struct radiant_radio_caps *caps = radiant_radio_caps_get();
	radiant_time_t                   slot = radiant_channel_next_slot(ch);
	radiant_time_t                   earliest;
	uint16_t                         period_counts = 0u;
	uint32_t                         lead;

	/* No grid to sit on (not on air, or a master that has not transmitted
	 * yet): send as soon as the backend can. */
	if (slot == RADIANT_TIME_NEVER) {
		return RADIANT_TIME_NEVER;
	}

	/* A slave replies in its master's reply window: the master's expected
	 * t_sync plus the 2190 us turnaround. */
	if (!radiant_channel_is_master(ch)) {
		slot += (radiant_time_t)RADIANT_TRANSFER_SLOT_REPLY_US;
	}

	/*
	 * Far enough out to arm: radiant_transfer_submit() does not clamp an explicit
	 * instant, and a refused one reaches the host as TRANSFER_IN_ERROR. Slipping a
	 * period keeps the same phase, so it costs only latency.
	 */
	lead = (caps != NULL) ? (uint32_t)caps->min_arm_lead_us : 0u;
	earliest = radiant_radio_now() + (radiant_time_t)lead +
		   (radiant_time_t)RADIANT_TRANSFER_ARM_SLACK_US;

	if (slot < earliest &&
	    radiant_channel_period_get(ch, &period_counts) == RADIANT_CH_OK &&
	    period_counts != 0u) {
		radiant_time_t period = radiant_channel_counts_to_us(period_counts);
		unsigned int   guard;

		/* A loop, since one period may not clear the arm lead at a short
		 * period. Bounded: radiant_time_t wraps, so the comparison is not a
		 * true ordering near the wrap. */
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
	if (radiant_channel_type_get(channel) == ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY ||
	    radiant_channel_type_get(channel) == ANTW_CHANNEL_TYPE_MASTER_TX_ONLY) {
		return (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE;
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	unsigned int key = radiant_event_crit_enter();
	if (api_ch[channel].ack_pending ||
	    !radiant_transfer_is_idle(&api_xfer[channel])) {
		radiant_event_crit_exit(key);
		k_mutex_unlock(&api_lock);
		return (antr_err_t)ANTW_TRANSFER_IN_PROGRESS;
	}
	api_xfer_refresh_id(channel);
	/* A secured channel takes the projected slot below, never a received
	 * frame (see api_ack_pending_send()). */
	if (!radiant_channel_is_master(channel) && api_xfer[channel].ready &&
	    !radiant_sec_channel_is_secured(channel)) {
		uint16_t counts = 0u;
		radiant_time_t wait = RADIANT_API_XFER_WATCHDOG_MS * 1000u;

		(void)radiant_channel_period_get(channel, &counts);
		radiant_time_t periods = RADIANT_API_ACK_OPENER_PERIODS *
					 radiant_channel_counts_to_us(counts);
		if (periods > wait) {
			wait = periods;
		}
		memcpy(api_ch[channel].ack_data, data, size);
		api_ch[channel].ack_deadline = radiant_radio_now() + wait;
		api_ch[channel].ack_pending = true;
		radiant_event_crit_exit(key);
		k_mutex_unlock(&api_lock);
		return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
	}
	rc = radiant_transfer_ack_data(&api_xfer[channel], data, size,
				   api_xfer_t_sync_for(channel));
	if (rc == RADIANT_TRANSFER_OK) {
		api_xfer_superseded(channel);
	}
	radiant_event_crit_exit(key);
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
	bool       was_idle;

	if (!api_ch_valid(channel) || data == NULL) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (size != 8u && size != 16u && size != 24u) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	/* Refuse blocks over 8 bytes until advanced burst is enabled
	 * (api_adv_burst[0]): accepting and never sending is worse. */
	if (size > 8u && api_adv_burst[0] == 0u) {
		return (antr_err_t)ANTW_INVALID_MESSAGE;
	}
	if (!radiant_channel_is_open(channel)) {
		return (antr_err_t)ANTW_CHANNEL_NOT_OPENED;
	}
	/* As antr_acknowledge_message_tx(): SLAVE_RX_ONLY never transmits;
	 * MASTER_TX_ONLY never listens for the acknowledgement. */
	if (radiant_channel_type_get(channel) == ANTW_CHANNEL_TYPE_SLAVE_RX_ONLY ||
	    radiant_channel_type_get(channel) == ANTW_CHANNEL_TYPE_MASTER_TX_ONLY) {
		return (antr_err_t)ANTW_CHANNEL_IN_WRONG_STATE;
	}

	k_mutex_lock(&api_lock, K_FOREVER);
	/*
	 * The segment byte is forwarded untouched. On a non-zero return the engine owns
	 * nothing and raises no event for this block (B2), and the bridge has already
	 * released its semaphore, so it must not be released again (B5). Only the START
	 * block is slot-aligned. Later packets are placed by radiant_burst.c from the
	 * preceding acknowledgement, a tighter grid that must not be overridden.
	 */
	unsigned int key = radiant_event_crit_enter();
	if (api_ch[channel].ack_pending) {
		radiant_event_crit_exit(key);
		k_mutex_unlock(&api_lock);
		return (antr_err_t)ANTW_TRANSFER_IN_PROGRESS;
	}
	api_xfer_refresh_id(channel);
	was_idle = radiant_transfer_is_idle(&api_xfer[channel]);
	rc = radiant_transfer_burst_block(&api_xfer[channel], data, (uint8_t)size,
				      segment,
				      (segment & ANTR_BURST_SEGMENT_START) != 0u
					      ? api_xfer_t_sync_for(channel)
					      : RADIANT_TIME_NEVER);
	if (rc == RADIANT_TRANSFER_OK && was_idle) {
		api_xfer_superseded(channel);
	}
	radiant_event_crit_exit(key);
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

	/* API_CHANNELS matches the radiant modules' sizes (asserted at the top)
	 * and the protocol ceiling (five bits in the burst header). */
	capabilities[ANTW_CAPABILITIES_OFFSET_MAX_CHANNELS] = API_CHANNELS;
	capabilities[ANTW_CAPABILITIES_OFFSET_MAX_NETWORKS] = API_NETWORKS;
	capabilities[ANTW_CAPABILITIES_OFFSET_STANDARD_OPTIONS] = 0x00u;

	/*
	 * The serial-number bit is set only when antr_serial_number_get() answers, so the
	 * bit and the request cannot disagree. This byte differs from the sdk_ant
	 * reference (0xb2) by exactly 0x08, on purpose.
	 */
	uint32_t serial;
	uint8_t serial_bit =
		(antr_serial_number_get(&serial) == ANTW_RESPONSE_NO_ERROR) ?
			(uint8_t)ANTW_CAPABILITIES_SERIAL_NUMBER_ENABLED : 0u;

	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS] =
		(uint8_t)(serial_bit |
			  ANTW_CAPABILITIES_NETWORK_ENABLED |
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
			  /* Honoured: radiant_event.c filters at delivery. */
			  ANTW_CAPABILITIES_EVENT_FILTERING_ENABLED);
	/*
	 * Search sharing (0x10), selective data update (0x40) and encryption (0x80)
	 * are not advertised: the first two are stored but not applied, and the
	 * third is refused by antr_crypto_channel_enable(). Their commands are still
	 * accepted, since a host with no recovery path is worse off than one whose
	 * setting quietly does nothing.
	 */

	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_4] = 0x00u;

	/* antr_channel_open_with_offset() is the real entry point, so the
	 * offset really is honoured. */
	capabilities[ANTW_CAPABILITIES_OFFSET_ADVANCED_OPTIONS_5] =
		(uint8_t)ANTW_CAPABILITIES_CHANNEL_START_OFFSET_ENABLED;

	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}

/* Identifies this build, not an AVN build (the stub reports "STUB0.01B00"). */
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

antr_err_t antr_serial_number_get(uint32_t *serial)
{
	if (serial == NULL) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}
#if defined(CONFIG_HWINFO)
	/* Last four bytes of the hwinfo device ID, big-endian. On nRF this
	 * matches the last eight hex digits of the USB iSerial. */
	uint8_t id[8];
	ssize_t n = hwinfo_get_device_id(id, sizeof(id));

	if (n >= 4) {
		*serial = sys_get_be32(&id[n - 4]);
		return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
	}
#endif
	return (antr_err_t)ANTW_INVALID_MESSAGE;
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
 * Encryption stubs: reads succeed, writes are held, and encrypted modes are refused.
 * ANT+'s AES-CTR is malleable and unauthenticated; see docs/radiant-security.md.
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

	/* No length parameter: the bridge bounds the read per type. */
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

	/* Write only what the type covers: the caller's buffer is sized for the
	 * largest reply, so zeroing it all could overrun. */
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

#if defined(CONFIG_RADIANT_DIAG_COUNTERS)
/* One 0xF7 page: see radiant_diag.h and ADR 0023 for the layout. */
antr_err_t antr_diag_page(uint8_t page, uint8_t *out)
{
	uint32_t v[RADIANT_DIAG_FIELDS];
	struct radiant_radio_diag rd;
	uint8_t backend;
	const struct radiant_diag_core *dc = radiant_diag_core_get();

	if (out == NULL || page >= RADIANT_DIAG_PAGES) {
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	/* Every page names its backend, so this runs for each page. */
	for (size_t i = 0; i < ARRAY_SIZE(rd.v); i++) {
		rd.v[i] = RADIANT_DIAG_NOT_MEASURED;
	}
	backend = radiant_radio_diag_get(&rd);

	switch (page) {
	case RADIANT_DIAG_PAGE_API:
		v[0] = api_stats.sched_dones;
		v[1] = api_stats.sched_missed;
		v[2] = api_stats.sched_failed;
		v[3] = api_stats.sched_denied;
		v[4] = api_stats.slots_missed;
		v[5] = api_stats.slots_denied;
		v[6] = api_stats.rx_sched_events;
		v[7] = api_stats.xfer_no_ack;
		break;
	case RADIANT_DIAG_PAGE_MASTER_RX:
		v[0] = dc->mrx_posted;
		v[1] = dc->mrx_refused;
		v[2] = dc->mrx_skipped;
		v[3] = dc->turn_dropped;
		v[4] = dc->mrx_done_ok;
		v[5] = dc->mrx_done_missed;
		v[6] = dc->mrx_done_other;
		v[7] = dc->mrx_frames;
		break;
	case RADIANT_DIAG_PAGE_TRANSFER: {
		const struct radiant_sched_stats *ss = radiant_sched_stats_get();

		v[0] = dc->cont_submits;
		v[1] = dc->cont_late;
		v[2] = dc->turn_seen ? dc->turn_min_us : RADIANT_DIAG_NOT_MEASURED;
		v[3] = dc->turn_seen ? dc->turn_max_us : RADIANT_DIAG_NOT_MEASURED;
		v[4] = dc->tx_expired;
		v[5] = dc->margin_seen ? (uint32_t)dc->tx_margin_min_us
				       : RADIANT_DIAG_NOT_MEASURED;
		v[6] = ss->arm_rejected;
		v[7] = ss->missed;
		break;
	}
	case RADIANT_DIAG_PAGE_BACKEND_A:
	case RADIANT_DIAG_PAGE_BACKEND_B: {
		size_t base = (page == RADIANT_DIAG_PAGE_BACKEND_A)
				      ? 0u : RADIANT_DIAG_FIELDS;

		memcpy(v, &rd.v[base], sizeof(v));
		break;
	}
	case RADIANT_DIAG_PAGE_HANDOFF:
		memcpy(v, &rd.v[RADIANT_RADIO_DIAG_HANDOFF_BASE], 3u * sizeof(v[0]));
		v[RADIANT_DIAG_HANDOFF_TRK_OPEN_LATE] = dc->trk_open_late;
		v[RADIANT_DIAG_HANDOFF_TRK_OPEN_LATE_MAX_US] = dc->trk_open_late_max_us;
		v[RADIANT_DIAG_HANDOFF_TRK_MISSED_BEHIND_SCAN] = dc->trk_missed_behind_scan;
		v[6] = RADIANT_DIAG_NOT_MEASURED; /* reserved, see radiant_diag.h */
		v[7] = RADIANT_DIAG_NOT_MEASURED;
		break;
	case RADIANT_DIAG_PAGE_SEARCH_A:
	case RADIANT_DIAG_PAGE_SEARCH_B: {
		const struct radiant_search_stats *st =
			radiant_search_get_stats(&api_search);

		if (page == RADIANT_DIAG_PAGE_SEARCH_A) {
			v[0] = st->windows;
			v[1] = st->sets_advanced;
			v[2] = st->sweeps;
			v[3] = st->frames_ok;
			v[4] = st->crc_fail;
			v[5] = st->acquired;
			v[6] = st->scan_reports;
			v[7] = st->timeouts;
		} else {
			v[0] = st->cache_steers;
			v[1] = st->cache_acquires;
			v[2] = st->permit_rejected;
			v[3] = st->rssi_rejected;
			v[4] = st->decode_failed;
			v[5] = st->bad_filter_index;
			v[6] = st->late_events;
#if defined(CONFIG_RADIANT_SEARCH_COVERAGE)
			v[7] = st->sets_advanced_uncovered;
#else
			v[7] = RADIANT_DIAG_NOT_MEASURED;
#endif
		}
		break;
	}
	default:
		return (antr_err_t)ANTW_INVALID_PARAMETER_PROVIDED;
	}

	out[0] = page;
	out[1] = RADIANT_DIAG_VERSION;
	out[2] = 0u;
	out[3] = backend;
	for (size_t i = 0; i < RADIANT_DIAG_FIELDS; i++) {
		if (v[i] == RADIANT_DIAG_NOT_MEASURED) {
			out[2] |= RADIANT_DIAG_FLAG_UNMEASURED;
		}
		sys_put_le32(v[i], &out[4u + 4u * i]);
	}
	return (antr_err_t)ANTW_RESPONSE_NO_ERROR;
}
#endif /* CONFIG_RADIANT_DIAG_COUNTERS */

bool radiant_api_ready(void)
{
	return api_inited;
}
