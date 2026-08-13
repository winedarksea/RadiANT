/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_api.h - what radiant_core presents to the rest of the image.
 *
 * Provenance: clean-room, from src/ant_radio.h, docs/sdk-ant-contract.md and
 * the six radiant_core module headers it composes.
 * See docs/decisions/0002-clean-room-policy.md.
 *
 * radiant_api.c's real interface is src/ant_radio.h (the antr_* entry points),
 * plus a few symbols the core modules leave undefined on purpose so a build
 * that forgets one fails at link time: radiant_channel_event_out() (adapts to
 * radiant_event_post_channel_event()), radiant_event_crit_enter/exit()
 * (irq_lock(), since the producer is a radio ISR and a mutex would be wrong),
 * and radiant_event_wakeup() (semaphore give, O(1), ISR-safe). None belong in
 * a public header; what's left here is the event thread's identity and the
 * integration counters.
 *
 * Hard rule: radiant_event_drain() is THREAD CONTEXT ONLY and must never run
 * inside an antr_* call the bridge made (docs/sdk-ant-contract.md forbids
 * re-entering antr_on_message()). radiant_api.c owns a thread that takes the
 * semaphore, drains, and runs housekeeping.
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
 * radiant_sched.c arms for absolute instants only, so a request that lost an
 * arm race (RADIANT_RADIO_EBUSY/ESTATE) has nothing scheduled to retry it;
 * radiant_sched_tick() is the recovery path, driven from here (channel and
 * search ticks ride along on the same wake). 50 ms is well inside the
 * shortest schedulable period (8.3 ms floor, 249.7 ms real ANT+ slot), so a
 * stalled request waits at most one of these. Recovery interval, not a
 * scheduling tick.
 */
#define RADIANT_API_HOUSEKEEP_MS 50u

/*
 * How long a transfer may sit non-idle with nothing armed, or an inbound
 * transfer go without a packet, before housekeeping terminates it: "the peer
 * or the radio stopped, and no completion is coming".
 *
 * Real recovery, not hardening: a transfer wedged in TX_REPLY with no op
 * armed had no route back to IDLE (api_feed_xfer_terminal() is reachable only
 * from api_sched_done(), which needs an armed op) and silently killed the
 * channel for the process lifetime. This is what bounds that to 500 ms.
 *
 * 500 ms is two channel periods: longer than any op this stack arms or any
 * gap in one inbound burst (~3.1 ms measured), short enough to recover within
 * a screen refresh. WAIT_BLOCK is deliberately not watched - it waits on the
 * host's own longer timeout, and terminating it here would break the B1-B5
 * buffer-ownership contract.
 */
#define RADIANT_API_XFER_WATCHDOG_MS 500u

/* Counters. Each distinguishes two outcomes that look the same from outside. */
struct radiant_api_stats {
	uint32_t drains;          /* radiant_event_drain() calls that delivered */
	uint32_t delivered;       /* messages handed to antr_on_message() */
	uint32_t housekeeps;      /* thread wakes that ran the tick trio */
	uint32_t pumps;           /* scheduling passes posted from thread ctx */
	uint32_t rx_posted;       /* received data messages queued for the host */
	uint32_t rx_dropped;      /* received frames the event queue refused */
	/*
	 * Scheduler completions of every kind. Separates "the window ran" from
	 * "it ran and its completion was swallowed" - posting a master's
	 * turnaround from inside its own TX callback once reused a slot the
	 * scheduler still held in flight, silently losing done() four times a
	 * second. Count against HAL-terminated operations to catch that.
	 */
	uint32_t sched_dones;
	uint32_t sched_missed;    /* RADIANT_SCHED_DONE_MISSED reports */
	uint32_t sched_failed;    /* RADIANT_SCHED_DONE_FAILED reports */
	/*
	 * RADIANT_SCHED_DONE_DENIED reports: the radio lent to another protocol
	 * stack. Zero on a backend that owns the radio. Kept separate from
	 * sched_failed for attribution: downstream a denial is reported to the
	 * transfer engine as a plain failure, so this counter is the only way
	 * to tell "air was bad" from "arbiter took the slot".
	 */
	uint32_t sched_denied;
	uint32_t slots_missed;    /* windows that ran and heard nothing */
	/* Tracked or master slots lost to the arbiter. Against slots_missed:
	 * loss that is ours versus loss that is the air's. */
	uint32_t slots_denied;
	uint32_t acquired;        /* search acquisitions accepted onto a channel */
	uint32_t id_list_rejects; /* acquisitions refused by a device ID list */
	uint32_t key_rejects;     /* network keys not in the key -> address table */
	/* Transfers the watchdog terminated (non-idle, nothing armed, no
	 * completion coming). Must never be the normal case. */
	uint32_t xfer_watchdogs;
	/*
	 * CRC failures on a tracked window recovered by flipping one bit back
	 * (CONFIG_RADIANT_CORE_CRC_REPAIR). Kept apart from a clean receive:
	 * about 1 in 585 unrepairable frames hits a valid syndrome by chance
	 * and gets "repaired" wrong, so at full signal this must be zero.
	 */
	uint32_t crc_repaired;
	/* Tracked-window CRC failures no single flipped bit explains. Against
	 * crc_repaired, shows the error distribution at the bench's signal level. */
	uint32_t crc_unrepairable;
	/*
	 * Repairs whose syndrome-valid result was refuted on evidence outside
	 * the CRC (a control byte never seen on air, or wrong transmission
	 * type) - caught mis-repairs, so non-zero here is the feature working.
	 */
	uint32_t crc_repair_refuted;
	/*
	 * Repairs refuted by radiant_profile_sanity.c
	 * (CONFIG_RADIANT_CORE_PROFILE_SANITY): passed the CRC/control/type
	 * checks but the payload is a physically impossible reading. Kept
	 * separate from crc_repair_refuted since the two catch mis-repairs on
	 * different evidence (frame structure vs. payload meaning).
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
