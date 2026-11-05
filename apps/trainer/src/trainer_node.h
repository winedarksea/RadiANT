/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_node.h - what main.c and trainer_shell.c say to each other.
 */

#ifndef TRAINER_NODE_H_
#define TRAINER_NODE_H_

#include <stdint.h>

struct shell;

/* The simulated rider, from the `rider` shell command. Stored and picked up
 * by the next model tick on the workqueue, so the shell thread never touches
 * the model. A negative argument leaves that half unchanged. */
void trainer_node_set_rider(int32_t power_w, int32_t cadence_rpm);

/* One status block: mode, road, rider, and what the model last produced. */
void trainer_node_print(const struct shell *sh);

/* Register the ACK fault hook on the FE-C channel. Call before the channel
 * opens. Returns 0 or -EIO. */
int trainer_ackfault_attach(uint8_t channel);

#endif /* TRAINER_NODE_H_ */
