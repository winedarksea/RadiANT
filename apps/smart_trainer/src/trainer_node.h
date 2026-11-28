/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_node.h - what main.c offers the rest of the application. Internal.
 */
#ifndef TRAINER_NODE_H_
#define TRAINER_NODE_H_

#include <stdint.h>

#include "trainer_equip.h"
#include "trainer_source.h"

/* A driver report / event, delivered ON THE SYSTEM WORKQUEUE by
 * trainer_source.c. */
void trainer_node_report(const struct trainer_report *r);
void trainer_node_event(enum trainer_source_event evt);

/* main() calls this once the equipment state exists: trainer_source.c holds
 * every report and event until then. */
void trainer_source_ready(void);

/* The equipment state, for trainer_ble.c. Workqueue only. */
struct trainer_equip *trainer_node_equip(void);

/* The session epoch, from any thread (the BT RX thread reads it). */
uint32_t trainer_node_epoch(void);

/* FTMS Reset succeeded (the core's reset_done hook). Workqueue. */
void trainer_node_ftms_reset_done(void);

#endif /* TRAINER_NODE_H_ */
