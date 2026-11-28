/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_source.c - the registry behind trainer_source.h, and the thread hop:
 * a driver reports from any thread, and the equipment state is workqueue-only,
 * so reports and events go into trainer_inbox (under a spinlock) and a work
 * item applies them. trainer_inbox.h has the ordering rule: an event drops a
 * report still pending from before it.
 *
 * NOTHING IS DELIVERED BEFORE trainer_source_ready(). A button pressed or a
 * report pushed before main() has initialised the equipment state waits here
 * instead of landing in a struct that trainer_equip_init() then wipes.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "trainer_inbox.h"
#include "trainer_node.h"
#include "trainer_source.h"

LOG_MODULE_DECLARE(smart_trainer, CONFIG_SMART_TRAINER_LOG_LEVEL);

static const struct trainer_source_ops *source;

static struct k_spinlock lock;
static struct trainer_inbox inbox;
static bool ready;

static void source_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		struct trainer_report r;
		enum trainer_source_event e = TRAINER_EVT_STARTED;
		enum trainer_inbox_item item = ready ? trainer_inbox_pop(&inbox, &r, &e)
						     : TRAINER_INBOX_EMPTY;

		k_spin_unlock(&lock, key);

		if (item == TRAINER_INBOX_EVENT) {
			trainer_node_event(e);
		} else if (item == TRAINER_INBOX_REPORT) {
			trainer_node_report(&r);
		} else {
			break;
		}
	}
}

static K_WORK_DEFINE(source_work, source_work_fn);

int trainer_source_register(const struct trainer_source_ops *ops)
{
	if (ops == NULL) {
		return -EINVAL;
	}
	if (source != NULL) {
		return -EALREADY;
	}
	source = ops;
	return 0;
}

const struct trainer_source_ops *trainer_source_get(void)
{
	return source;
}

void trainer_source_ready(void)
{
	k_spinlock_key_t key = k_spin_lock(&lock);

	ready = true;
	k_spin_unlock(&lock, key);
	(void)k_work_submit(&source_work);
}

int trainer_source_report(const struct trainer_report *r)
{
	k_spinlock_key_t key;

	if (r == NULL) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	trainer_inbox_report(&inbox, r);
	k_spin_unlock(&lock, key);
	(void)k_work_submit(&source_work);
	return 0;
}

int trainer_source_report_event(enum trainer_source_event evt)
{
	k_spinlock_key_t key;
	int rc;

	key = k_spin_lock(&lock);
	rc = trainer_inbox_event(&inbox, evt);
	k_spin_unlock(&lock, key);
	if (rc != 0) {
		LOG_ERR("trainer event %d dropped: queue full", (int)evt);
		return rc;
	}
	(void)k_work_submit(&source_work);
	return 0;
}
