/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_inbox.h - what a driver pushed (trainer_source_report() and
 * trainer_source_report_event()) waiting for the workqueue. Kernel-free; the
 * caller (trainer_source.c) holds a spinlock around every call.
 *
 * THE ORDERING RULE. Reports coalesce (the latest wins); events queue in
 * order. An event DROPS a report still pending from before it: that report
 * describes the trainer before the event, and applied after it, it would
 * undo it - a pedalling report taken just before an FTMS Pause would restart
 * the session the Pause just ended. A report pushed after the event is kept
 * and is applied after it.
 */
#ifndef TRAINER_INBOX_H_
#define TRAINER_INBOX_H_

#include <stdbool.h>
#include <stdint.h>

#include "trainer_source.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TRAINER_INBOX_EVENTS 8u

struct trainer_inbox {
	struct trainer_report rep;
	bool    have_rep;
	uint8_t q[TRAINER_INBOX_EVENTS];
	uint8_t head;
	uint8_t tail;
};

enum trainer_inbox_item {
	TRAINER_INBOX_EMPTY = 0,
	TRAINER_INBOX_EVENT,
	TRAINER_INBOX_REPORT,
};

void trainer_inbox_init(struct trainer_inbox *ib);

/* Store a report (replacing a pending one). */
void trainer_inbox_report(struct trainer_inbox *ib, const struct trainer_report *r);

/* Queue an event and drop any report pending from before it. 0, or -ENOMEM
 * when the queue is full (the event is lost and the report kept). */
int trainer_inbox_event(struct trainer_inbox *ib, enum trainer_source_event evt);

/* The next item: events first, in order, then the report. */
enum trainer_inbox_item trainer_inbox_pop(struct trainer_inbox *ib, struct trainer_report *rep,
					  enum trainer_source_event *evt);

#ifdef __cplusplus
}
#endif

#endif /* TRAINER_INBOX_H_ */
