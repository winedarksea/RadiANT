/* SPDX-License-Identifier: Apache-2.0 */
/*
 * gate_probe.h - reading the gate's own counters from a test.
 *
 * The counters and flags exposed here are `static` inside
 * radiant_radio_nrf_gate_mpsl.c and must stay that way - read on a running
 * dongle only via the 1 Hz gate_dump_fn() line; an exported symbol would
 * invite something to branch on one.
 *
 * So the file under test is compiled into gate_under_test.c by #include,
 * and these accessors are the only things that leave that translation
 * unit. Nothing in the production file changes for the tests - a
 * #ifdef CONFIG_ZTEST anywhere in it would mean the shipping build takes a
 * different path than the tested one.
 */

#ifndef RADIANT_TESTS_GATE_PROBE_H_
#define RADIANT_TESTS_GATE_PROBE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gate_probe {
	/* stats */
	uint32_t granted;         /* SIGNAL_START count: granted timeslots */
	uint32_t grant_end_calls; /* radiant_nrf_gate_on_grant_end() calls */
	uint32_t release_disarm;
	uint32_t idle_disarm;
	uint32_t late_disarm;     /* must be zero on a healthy bench */
	uint32_t placed;
	uint32_t blocked;
	uint32_t cancelled;
	uint32_t refused_eagain;
	uint32_t extends_ok;
	uint32_t extends_failed;
	uint32_t sent_after_grant;
	uint32_t sent_blocked;
	uint32_t deny_suppressed;
	uint32_t overstayed;
	uint32_t invalid_return;
	uint32_t unknown_signal;
	/* A request MPSL answered from inside mpsl_timeslot_request(). Bug 15. */
	uint32_t answered_inline;
	/* Acquires refused because MPSL still owed an answer - the wedge's
	 * own counter: once it's the only thing moving, the radio is dead. */
	uint32_t den_owed;

	/* g */
	bool hw_held;
	bool granted_flag;
	bool pending;
	bool release_wanted;
	bool ended;
	bool bootstrapping;
	bool anchor_valid;
	bool mpsl_owes;
	uint32_t granted_len_us;
	uint32_t want_len_us;
};

void gate_probe_read(struct gate_probe *out);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_TESTS_GATE_PROBE_H_ */
