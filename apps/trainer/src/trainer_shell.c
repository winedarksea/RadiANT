/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_shell.c - the fault model's Zephyr half: the radio hook, the
 * ground-truth ring and the shell.
 *
 * GROUND TRUTH IS KEPT AT THE FAR END, which is the whole point. An originator
 * cannot tell "my command never arrived" from "it arrived and the reply was
 * lost" - both are TRANSFER_TX_FAILED, and two whole sessions were once spent
 * inside that ambiguity. So every acknowledged packet this trainer receives
 * gets one record here, written in the radio interrupt at the moment of the
 * decision: when it arrived, what it carried, what was decided, by which rule,
 * at what offset, and - one packet later - whether the ACK actually completed
 * on the air (radiant's own acks_sent counter, not this file's intention).
 *
 * `ackfault dump` prints the ring as one line per record, prefixed `AF `, for
 * tools/fec_ack_sweep.py to join against the originator's outcomes by the
 * payload bytes (the sweep makes every command's grade unique).
 */

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#include "ant_radio.h"
#include "trainer_ackfault.h"
#include "trainer_node.h"

BUILD_ASSERT((int)TRAINER_ACTION_ACK == (int)ANTR_REPLY_ACK &&
	     (int)TRAINER_ACTION_MUTE == (int)ANTR_REPLY_MUTE &&
	     (int)TRAINER_ACTION_DEAF == (int)ANTR_REPLY_DEAF,
	     "trainer_action must mirror antr_reply_action");

struct af_rec {
	uint32_t seq;
	uint32_t t_sync_us; /* the radio clock's low 32 bits */
	uint32_t acks_before;
	int32_t  offset_us;
	uint8_t  payload[8];
	uint8_t  action;
	uint8_t  cause;
	/* -1 pending (an ACK whose completion is not known yet), 0 no ACK left
	 * the antenna, 1 it did. Never 1 for MUTE or DEAF. */
	int8_t   tx_done;
};

static struct af_rec ring[CONFIG_TRAINER_ACKFAULT_RING];
static uint32_t      ring_head;  /* next slot to write */
static uint32_t      ring_count; /* valid records, <= ring size */
static uint32_t      ring_lost;  /* overwritten before a dump read them */
static uint32_t      next_seq;

static struct trainer_ackfault af;
static uint8_t                 af_channel;

/* The hook runs in the radio ISR and the shell in a thread; this covers the
 * fault state and the ring, held for a copy and one decision. */
static struct k_spinlock af_lock;

static struct af_rec *newest_locked(void)
{
	if (ring_count == 0u) {
		return NULL;
	}
	return &ring[(ring_head + ARRAY_SIZE(ring) - 1u) % ARRAY_SIZE(ring)];
}

/* An ACK record's outcome is settled once acks_sent has had a chance to move:
 * by the next packet (250 ms later at the least) or by the dump. */
static void settle_locked(struct af_rec *r, uint32_t acks_now)
{
	if (r != NULL && r->tx_done < 0) {
		r->tx_done = (acks_now != r->acks_before) ? 1 : 0;
	}
}

/* RADIO INTERRUPT CONTEXT. One decision, one record, O(1). */
static void reply_plan(void *ctx, uint8_t channel, const uint8_t *payload,
		       uint64_t t_sync_us, struct antr_reply_plan *out)
{
	struct antr_reply_stats          st = { 0 };
	struct trainer_ackfault_decision d;
	struct af_rec                   *r;
	k_spinlock_key_t                 key;

	ARG_UNUSED(ctx);

	(void)antr_reply_stats_get(channel, &st);

	key = k_spin_lock(&af_lock);

	settle_locked(newest_locked(), st.acks_sent);
	trainer_ackfault_decide(&af, t_sync_us, &d);

	r = &ring[ring_head];
	r->seq = next_seq++;
	r->t_sync_us = (uint32_t)t_sync_us;
	r->acks_before = st.acks_sent;
	r->offset_us = d.offset_us;
	memcpy(r->payload, payload, sizeof(r->payload));
	r->action = d.action;
	r->cause = d.cause;
	r->tx_done = (d.action == TRAINER_ACTION_ACK) ? -1 : 0;

	ring_head = (ring_head + 1u) % ARRAY_SIZE(ring);
	if (ring_count < ARRAY_SIZE(ring)) {
		ring_count++;
	} else {
		ring_lost++;
	}

	k_spin_unlock(&af_lock, key);

	out->action = (enum antr_reply_action)d.action;
	out->offset_us = d.offset_us;
}

int trainer_ackfault_attach(uint8_t channel)
{
	struct trainer_ackfault_params p;

	trainer_ackfault_params_ideal(&p);
	trainer_ackfault_init(&af, &p);
	af_channel = channel;

	return (antr_reply_plan_hook_set(channel, reply_plan, NULL) ==
		ANTW_RESPONSE_NO_ERROR)
		       ? 0
		       : -EIO;
}

/* ── The shell ──────────────────────────────────────────────────────────── */

static void print_params(const struct shell *sh,
			 const struct trainer_ackfault_params *p)
{
	shell_print(sh,
		    "offset_us=%d jitter_us=%u mute_pm=%u deaf_pm=%u p_bad=%u "
		    "p_good=%u bad_action=%s busy_ms=%u seed=0x%08x",
		    (int)p->offset_us, (unsigned)p->jitter_us,
		    (unsigned)p->mute_pm, (unsigned)p->deaf_pm,
		    (unsigned)p->p_bad_pm, (unsigned)p->p_good_pm,
		    trainer_ackfault_action_name(p->bad_action),
		    (unsigned)p->busy_ms, (unsigned)p->seed);
}

static int cmd_show(const struct shell *sh, size_t argc, char **argv)
{
	struct trainer_ackfault_params p;
	k_spinlock_key_t               key;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	key = k_spin_lock(&af_lock);
	p = af.p;
	k_spin_unlock(&af_lock, key);

	print_params(sh, &p);
	return 0;
}

/* `ackfault set k=v [k=v ...]`. All or nothing: one bad pair and nothing
 * changes, so a typo cannot leave half a configuration installed. Decision
 * state is NOT reset - run `ackfault reset` for a reproducible start. */
static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	struct trainer_ackfault_params p;
	k_spinlock_key_t               key;

	key = k_spin_lock(&af_lock);
	p = af.p;
	k_spin_unlock(&af_lock, key);

	for (size_t i = 1; i < argc; i++) {
		char  buf[48];
		char *eq;

		if (strlen(argv[i]) >= sizeof(buf)) {
			shell_error(sh, "too long: %s", argv[i]);
			return -EINVAL;
		}
		strcpy(buf, argv[i]);
		eq = strchr(buf, '=');
		if (eq == NULL) {
			shell_error(sh, "expected key=value: %s", argv[i]);
			return -EINVAL;
		}
		*eq = '\0';
		if (trainer_ackfault_set(&p, buf, eq + 1) != 0) {
			shell_error(sh, "bad parameter: %s", argv[i]);
			return -EINVAL;
		}
	}

	key = k_spin_lock(&af_lock);
	af.p = p;
	k_spin_unlock(&af_lock, key);

	print_params(sh, &p);
	return 0;
}

static int cmd_preset(const struct shell *sh, size_t argc, char **argv)
{
	struct trainer_ackfault_params p;
	k_spinlock_key_t               key;

	ARG_UNUSED(argc);

	if (trainer_ackfault_preset(argv[1], &p) != 0) {
		shell_error(sh, "unknown preset '%s' (ideal late300 late-edge "
			    "jitter lossy25 wahoo)", argv[1]);
		return -EINVAL;
	}

	key = k_spin_lock(&af_lock);
	trainer_ackfault_init(&af, &p);
	k_spin_unlock(&af_lock, key);

	if (strcmp(argv[1], "wahoo") == 0) {
		shell_warn(sh, "wahoo is a PLACEHOLDER equal to ideal until the "
			   "sniffer calibration fits it");
	}
	print_params(sh, &p);
	return 0;
}

static int cmd_stats(const struct shell *sh, size_t argc, char **argv)
{
	struct antr_reply_stats st = { 0 };
	struct trainer_ackfault a;
	uint32_t                count, lost;
	k_spinlock_key_t        key;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	(void)antr_reply_stats_get(af_channel, &st);

	key = k_spin_lock(&af_lock);
	a = af;
	count = ring_count;
	lost = ring_lost;
	k_spin_unlock(&af_lock, key);

	shell_print(sh,
		    "acked_frames=%u ack=%u mute=%u deaf=%u "
		    "cause_busy=%u cause_burst=%u cause_deaf=%u cause_mute=%u "
		    "acks_sent=%u too_early=%u ring=%u lost=%u",
		    (unsigned)a.n, (unsigned)a.n_action[TRAINER_ACTION_ACK],
		    (unsigned)a.n_action[TRAINER_ACTION_MUTE],
		    (unsigned)a.n_action[TRAINER_ACTION_DEAF],
		    (unsigned)a.n_cause[TRAINER_CAUSE_BUSY],
		    (unsigned)a.n_cause[TRAINER_CAUSE_BURST],
		    (unsigned)a.n_cause[TRAINER_CAUSE_DEAF],
		    (unsigned)a.n_cause[TRAINER_CAUSE_MUTE],
		    (unsigned)st.acks_sent, (unsigned)st.too_early,
		    (unsigned)count, (unsigned)lost);
	return 0;
}

/* Machine-readable: AF-BEGIN, one AF line per record oldest first, AF-END.
 * Records are copied out one at a time under the lock, so the radio is never
 * held off for the length of a UART print. */
static int cmd_dump(const struct shell *sh, size_t argc, char **argv)
{
	struct antr_reply_stats st = { 0 };
	uint32_t                count, lost, first;
	k_spinlock_key_t        key;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	(void)antr_reply_stats_get(af_channel, &st);

	key = k_spin_lock(&af_lock);
	settle_locked(newest_locked(), st.acks_sent);
	count = ring_count;
	lost = ring_lost;
	first = (ring_head + ARRAY_SIZE(ring) - count) % ARRAY_SIZE(ring);
	k_spin_unlock(&af_lock, key);

	shell_print(sh, "AF-BEGIN n=%u lost=%u acks_sent=%u too_early=%u",
		    (unsigned)count, (unsigned)lost, (unsigned)st.acks_sent,
		    (unsigned)st.too_early);

	for (uint32_t i = 0; i < count; i++) {
		struct af_rec r;

		key = k_spin_lock(&af_lock);
		r = ring[(first + i) % ARRAY_SIZE(ring)];
		k_spin_unlock(&af_lock, key);

		shell_print(sh,
			    "AF seq=%u t=%u p=%02x%02x%02x%02x%02x%02x%02x%02x "
			    "act=%s cause=%s off=%d tx=%d",
			    (unsigned)r.seq, (unsigned)r.t_sync_us, r.payload[0],
			    r.payload[1], r.payload[2], r.payload[3],
			    r.payload[4], r.payload[5], r.payload[6],
			    r.payload[7], trainer_ackfault_action_name(r.action),
			    trainer_ackfault_cause_name(r.cause),
			    (int)r.offset_us, (int)r.tx_done);
	}

	shell_print(sh, "AF-END");
	return 0;
}

/* Clear the ring and restart the decision state from the seed. The
 * parameters stay. */
static int cmd_reset(const struct shell *sh, size_t argc, char **argv)
{
	k_spinlock_key_t key;

	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	key = k_spin_lock(&af_lock);
	trainer_ackfault_reset(&af);
	ring_head = 0u;
	ring_count = 0u;
	ring_lost = 0u;
	next_seq = 0u;
	k_spin_unlock(&af_lock, key);

	shell_print(sh, "ackfault reset");
	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_ackfault,
	SHELL_CMD_ARG(show, NULL, "print the fault parameters", cmd_show, 1, 0),
	SHELL_CMD_ARG(set, NULL,
		      "k=v ... (offset_us jitter_us mute_pm deaf_pm p_bad "
		      "p_good bad_action busy_ms seed)",
		      cmd_set, 2, 9),
	SHELL_CMD_ARG(preset, NULL,
		      "<ideal|late300|late-edge|jitter|lossy25|wahoo>",
		      cmd_preset, 2, 0),
	SHELL_CMD_ARG(stats, NULL, "decision and on-air counters", cmd_stats,
		      1, 0),
	SHELL_CMD_ARG(dump, NULL, "the ground-truth ring, machine-readable",
		      cmd_dump, 1, 0),
	SHELL_CMD_ARG(reset, NULL, "clear the ring, re-seed", cmd_reset, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(ackfault, &sub_ackfault,
		   "programmable ACK faults (test instrument)", NULL);

/* `rider [power=W] [cadence=RPM]`. Either may be left out; with neither,
 * just print the status block. */
static int cmd_rider(const struct shell *sh, size_t argc, char **argv)
{
	long power = -1;
	long cadence = -1;

	for (size_t i = 1; i < argc; i++) {
		char *end = NULL;

		if (strncmp(argv[i], "power=", 6) == 0) {
			power = strtol(argv[i] + 6, &end, 0);
			if (*end != '\0' || power < 0 || power > 4094) {
				shell_error(sh, "power is 0..4094 W");
				return -EINVAL;
			}
		} else if (strncmp(argv[i], "cadence=", 8) == 0) {
			cadence = strtol(argv[i] + 8, &end, 0);
			if (*end != '\0' || cadence < 0 || cadence > 254) {
				shell_error(sh, "cadence is 0..254 rpm");
				return -EINVAL;
			}
		} else {
			shell_error(sh, "expected power=W or cadence=RPM");
			return -EINVAL;
		}
	}

	if (power >= 0 || cadence >= 0) {
		trainer_node_set_rider((int32_t)power, (int32_t)cadence);
	}
	trainer_node_print(sh);
	return 0;
}

SHELL_CMD_ARG_REGISTER(rider, NULL,
		       "rider [power=W] [cadence=RPM] - the simulated rider",
		       cmd_rider, 1, 2);
