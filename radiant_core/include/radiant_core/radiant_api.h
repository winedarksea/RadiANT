/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_api.h - what radiant_core presents to the rest of the image, and the two
 * seams it closes that no single module could close for itself.
 *
 * Provenance: clean-room. Written from src/ant_radio.h and
 * docs/sdk-ant-contract.md (the antr_* contract this module implements), and
 * from the six radiant_core module headers it composes. Nothing here derives from
 * sdk-ant, from libant.a, from disassembly of any binary, or from any
 * adopter-gated ANT+ device profile document.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * ---------------------------------------------------------------------------
 * There is almost nothing in this header, and that is the point
 * ---------------------------------------------------------------------------
 * radiant_api.c's real interface is src/ant_radio.h: it defines the fifty antr_*
 * entry points the serial bridge calls, and it defines the handful of symbols
 * the core modules deliberately left undefined so that a build which forgot one
 * would fail at the link with a name rather than ship a silent data race:
 *
 *   radiant_channel_event_out()   radiant_channel.c raises channel events through it;
 *                             radiant_event.c spells the same thing
 *                             radiant_event_post_channel_event(). One adapter.
 *   radiant_event_crit_enter()    the ring's critical section - irq_lock() here,
 *   radiant_event_crit_exit()     because the producer is a radio ISR and a mutex
 *                             would be wrong.
 *   radiant_event_wakeup()        makes the drain run. A semaphore give, O(1) and
 *                             ISR-safe, and it does not call back into
 *                             radiant_event.
 *
 * None of those belong in a public header - they are resolved at link time by
 * design - so what is left here is the little that another translation unit in
 * this image might legitimately want: the event thread's identity for a
 * diagnostic line, and the counters that say whether the integration is doing
 * anything.
 *
 * The one hard rule this file exists to write down: radiant_event_drain() is
 * THREAD CONTEXT ONLY and must never run inside an antr_* call the bridge made,
 * because docs/sdk-ant-contract.md forbids re-entering antr_on_message() from
 * inside a call the bridge is still executing. radiant_api.c therefore owns a
 * thread whose whole job is to take the semaphore, drain, and run housekeeping.
 */

#ifndef RADIANT_API_H_
#define RADIANT_API_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How often the event thread wakes when nothing has posted an event.
 *
 * radiant_sched.c owns no timer: it arms for absolute instants and lets the backend
 * hold them, so a request that lost an arm race with RADIANT_RADIO_EBUSY or was
 * posted before the radio was enabled (RADIANT_RADIO_ESTATE) has nothing scheduled
 * to retry it. radiant_sched_tick() is the only recovery path, and this is what
 * drives it. radiant_channel_tick() - search deadlines - and radiant_search_tick() -
 * the seen cache and its own deadlines - ride along on the same wake.
 *
 * 50 ms is well inside the shortest channel period this stack will schedule
 * (RADIANT_CHANNEL_PERIOD_MIN_COUNTS is 8.3 ms, but no ANT+ profile is faster than
 * the 249.7 ms 4.005 Hz slot), so a pending request can stall for at most one
 * of these rather than indefinitely. It is a recovery interval, not a
 * scheduling tick: nothing on the fast path waits for it.
 */
#define RADIANT_API_HOUSEKEEP_MS 50u

/*
 * How long a transfer may sit non-idle with nothing armed before housekeeping
 * terminates it, and how long an inbound transfer may go without a packet
 * before it is treated as abandoned. One constant because both answer the same
 * question - "the peer or the radio stopped, and no completion is coming".
 *
 * WHAT THIS RECOVERS FROM, because it is not a theoretical hardening. A
 * transfer wedged in TX_REPLY refuses every subsequent inbound data packet
 * (radiant_ack.c checks for IDLE), and api_pump_locked() then stops posting the
 * channel's own broadcast slot too. The only route back to IDLE was
 * api_feed_xfer_terminal(), reachable only from api_sched_done() and only when
 * nothing is armed - so if nothing is armed, no done() ever arrives and one
 * lost reply completion killed the channel for the life of the process,
 * silently. This is what turns that into a half-second outage.
 *
 * 500 ms is two 249.7 ms channel periods: comfortably longer than any radio
 * operation this stack arms and than any gap between packets of one inbound
 * burst (~3.1 ms measured), and short enough that a Zwift session recovers
 * within one screen refresh. WAIT_BLOCK is deliberately NOT watched - that
 * state is waiting on the host, which has its own much longer timeout, and
 * terminating it here would break the B1-B5 buffer-ownership contract.
 */
#define RADIANT_API_XFER_WATCHDOG_MS 500u

/*
 * Counters. Not decoration - each one distinguishes two outcomes that look the
 * same from outside, and they are what a bring-up session is read on when the
 * radio backend under radiant_core cannot yet put anything on the air.
 */
struct radiant_api_stats {
	uint32_t drains;          /* radiant_event_drain() calls that delivered */
	uint32_t delivered;       /* messages handed to antr_on_message() */
	uint32_t housekeeps;      /* thread wakes that ran the tick trio */
	uint32_t pumps;           /* scheduling passes posted from thread ctx */
	uint32_t rx_posted;       /* received data messages queued for the host */
	uint32_t rx_dropped;      /* received frames the event queue refused */
	/*
	 * Scheduler completions delivered here, of every kind. The two outcomes
	 * it separates are "the window ran" and "the window ran and its
	 * completion was swallowed on the way back" - which is not a theoretical
	 * pair: posting a master's turnaround from inside its own transmit
	 * callback reused a slot the scheduler still held in flight, and every
	 * listening master's transmit lost its done() to that, silently, four
	 * times a second. Count the operations that terminate at the HAL and
	 * compare.
	 */
	uint32_t sched_dones;
	uint32_t sched_missed;    /* RADIANT_SCHED_DONE_MISSED reports */
	uint32_t sched_failed;    /* RADIANT_SCHED_DONE_FAILED reports */
	uint32_t slots_missed;    /* windows that ran and heard nothing */
	uint32_t acquired;        /* search acquisitions accepted onto a channel */
	uint32_t id_list_rejects; /* acquisitions refused by a device ID list */
	uint32_t key_rejects;     /* network keys not in the key -> address table */
	/*
	 * Transfers the housekeeping watchdog had to terminate because the
	 * engine was left non-idle with nothing pending and no completion
	 * coming. Non-zero means a real fault happened AND was recovered from;
	 * it must never be the normal case, which is why it is counted rather
	 * than merely logged.
	 */
	uint32_t xfer_watchdogs;
	/*
	 * Frames that failed their CRC on a tracked window and were recovered
	 * by flipping one bit back (CONFIG_RADIANT_CORE_CRC_REPAIR).
	 *
	 * COUNTED SEPARATELY AND NEVER FOLDED INTO A CLEAN RECEIVE, which is
	 * the point of having it. A repaired frame is a frame that arrived
	 * damaged, and roughly one in 585 unrepairable frames will have hit a
	 * valid syndrome by chance and been "repaired" into something still
	 * wrong. At full signal this must be zero; if it is not, the link is
	 * marginal and any A/B reading loss alone would show an improvement
	 * that is really a different measurement.
	 */
	uint32_t crc_repaired;
	/*
	 * Tracked-window CRC failures that no single flipped bit explains. The
	 * pair is what says whether the feature is earning anything: repaired
	 * against unrepairable is the shape of the error distribution at
	 * whatever signal level the bench is running at.
	 */
	uint32_t crc_unrepairable;
	/*
	 * Repairs that produced a syndrome-valid frame which the receiver then
	 * refuted on evidence outside the CRC: a control byte that has never
	 * been on the air, or a transmission type that is not this channel's.
	 *
	 * These are caught mis-repairs, so a non-zero value here is the feature
	 * working rather than failing - and the ratio against crc_repaired is
	 * the only direct measurement anyone gets of how often a repair lands
	 * on the wrong bit. Whatever fraction slips past both checks lands in
	 * the payload, where nothing at this layer can judge it.
	 */
	uint32_t crc_repair_refuted;
	/*
	 * Repairs refuted by radiant_profile_sanity.c
	 * (CONFIG_RADIANT_CORE_PROFILE_SANITY): a syndrome-valid,
	 * control-byte-valid, transmission-type-valid repair whose payload
	 * is still a physically impossible reading - 4000 W, a 300 bpm
	 * heart rate.
	 *
	 * Counted apart from crc_repair_refuted, not folded into it, because
	 * the two checks catch mis-repairs by different evidence (frame
	 * structure the receiver already knew, versus what the payload
	 * bytes mean) and an A/B distinguishing them tells you which
	 * refutation is actually earning its keep.
	 */
	uint32_t crc_repair_implausible;
};

const struct radiant_api_stats *radiant_api_stats_get(void);

/* True once antr_init() has returned 0. The bridge has no business asking, but
 * a diagnostic line does, and so does a test double. */
bool radiant_api_ready(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_API_H_ */
