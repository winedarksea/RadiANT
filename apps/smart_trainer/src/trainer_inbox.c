/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_inbox.c - see trainer_inbox.h. No Zephyr header.
 */

#include <errno.h>
#include <string.h>

#include "trainer_inbox.h"

void trainer_inbox_init(struct trainer_inbox *ib)
{
	memset(ib, 0, sizeof(*ib));
}

void trainer_inbox_report(struct trainer_inbox *ib, const struct trainer_report *r)
{
	ib->rep = *r;
	ib->have_rep = true;
}

int trainer_inbox_event(struct trainer_inbox *ib, enum trainer_source_event evt)
{
	uint8_t next = (uint8_t)((ib->head + 1u) % TRAINER_INBOX_EVENTS);

	if (next == ib->tail) {
		return -ENOMEM;
	}
	ib->q[ib->head] = (uint8_t)evt;
	ib->head = next;
	/* The pending report predates the event: applied after it, it would
	 * undo it. */
	ib->have_rep = false;
	return 0;
}

enum trainer_inbox_item trainer_inbox_pop(struct trainer_inbox *ib, struct trainer_report *rep,
					  enum trainer_source_event *evt)
{
	if (ib->tail != ib->head) {
		*evt = (enum trainer_source_event)ib->q[ib->tail];
		ib->tail = (uint8_t)((ib->tail + 1u) % TRAINER_INBOX_EVENTS);
		return TRAINER_INBOX_EVENT;
	}
	if (ib->have_rep) {
		*rep = ib->rep;
		ib->have_rep = false;
		return TRAINER_INBOX_REPORT;
	}
	return TRAINER_INBOX_EMPTY;
}
