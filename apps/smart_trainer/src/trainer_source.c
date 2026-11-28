/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_source.c - the registry behind trainer_source.h, and the thread hop:
 * a driver reports from any thread, and the equipment state is workqueue-only,
 * so reports and events are copied under a spinlock and applied by a work
 * item. The latest report wins; events queue (eight deep) and are never
 * coalesced, because a stop must not be lost behind a start.
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "trainer_node.h"
#include "trainer_source.h"

LOG_MODULE_DECLARE(smart_trainer, CONFIG_SMART_TRAINER_LOG_LEVEL);

#define EVT_QUEUE_DEPTH 8u

static const struct trainer_source_ops *source;

static struct k_spinlock lock;
static struct trainer_report pending_report;
static bool have_report;
static uint8_t evt_q[EVT_QUEUE_DEPTH];
static uint8_t evt_head;
static uint8_t evt_tail;

static void source_work_fn(struct k_work *w)
{
	ARG_UNUSED(w);

	for (;;) {
		k_spinlock_key_t key = k_spin_lock(&lock);
		struct trainer_report r;
		bool rep = false;
		bool evt = false;
		uint8_t e = 0u;

		/* Events first, in order: a report that arrived after an event
		 * is applied after it. */
		if (evt_tail != evt_head) {
			e = evt_q[evt_tail];
			evt_tail = (uint8_t)((evt_tail + 1u) % EVT_QUEUE_DEPTH);
			evt = true;
		} else if (have_report) {
			r = pending_report;
			have_report = false;
			rep = true;
		}
		k_spin_unlock(&lock, key);

		if (evt) {
			trainer_node_event((enum trainer_source_event)e);
		} else if (rep) {
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

int trainer_source_report(const struct trainer_report *r)
{
	k_spinlock_key_t key;

	if (r == NULL) {
		return -EINVAL;
	}
	key = k_spin_lock(&lock);
	pending_report = *r;
	have_report = true;
	k_spin_unlock(&lock, key);
	(void)k_work_submit(&source_work);
	return 0;
}

int trainer_source_report_event(enum trainer_source_event evt)
{
	k_spinlock_key_t key;
	uint8_t next;
	int rc = 0;

	key = k_spin_lock(&lock);
	next = (uint8_t)((evt_head + 1u) % EVT_QUEUE_DEPTH);
	if (next == evt_tail) {
		rc = -ENOMEM;
	} else {
		evt_q[evt_head] = (uint8_t)evt;
		evt_head = next;
	}
	k_spin_unlock(&lock, key);
	if (rc != 0) {
		LOG_ERR("trainer event %d dropped: queue full", (int)evt);
		return rc;
	}
	(void)k_work_submit(&source_work);
	return 0;
}
