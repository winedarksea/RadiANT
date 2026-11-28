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

/* The equipment state, for trainer_ble.c. Workqueue only. */
struct trainer_equip *trainer_node_equip(void);

/* The session epoch, from any thread (the BT RX thread reads it). */
uint32_t trainer_node_epoch(void);

/* A session transition, so main.c's FE-C pages and FTMS agree. Workqueue. */
void trainer_node_session_changed(enum trainer_run prev, enum trainer_run next);

#endif /* TRAINER_NODE_H_ */
