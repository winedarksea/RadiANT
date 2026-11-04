/* SPDX-License-Identifier: Apache-2.0 */

/*
 * ant_serial_bridge.h - what the host has done to the stack, for firmware that
 * shares the stack with it.
 *
 * A host SYSTEM_RESET (Zwift sends one at startup) runs antr_stack_reset(),
 * which unassigns EVERY channel and clears every network key - including the
 * channels and key a dongle-side user such as apps/dongle_thread's self
 * channels set up for itself. That user cannot see the reset happen (it polls
 * channel status from its own thread), and it must not overwrite a key the
 * host chose. Both facts live here because this file is the host-command
 * path: they are true of the host whatever backend is underneath.
 */

#ifndef ANT_SERIAL_BRIDGE_H_
#define ANT_SERIAL_BRIDGE_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Networks whose host key writes are tracked. A write to a higher network
 * number is still forwarded to the stack; it is just not recorded. */
#define ANT_BRIDGE_HOST_NETWORKS 8u

struct ant_bridge_host_state {
	/* Host SYSTEM_RESETs since boot. A change means every channel and key
	 * was wiped since the last look. */
	uint32_t reset_gen;
	/* Host writes of this network's key since boot that the stack accepted.
	 * A change means the host replaced whatever key was there. */
	uint32_t key_writes;
	/* The host wrote this network's key since the last reset, and key[] is
	 * the last one it wrote. */
	bool     key_written;
	uint8_t  key[8];
};

int ant_serial_bridge_init(void);

/* A consistent snapshot for one network. Safe from any thread. A network at or
 * above ANT_BRIDGE_HOST_NETWORKS reads as never written. */
void ant_serial_bridge_host_state(uint8_t net, struct ant_bridge_host_state *out);

#ifdef __cplusplus
}
#endif

#endif /* ANT_SERIAL_BRIDGE_H_ */
