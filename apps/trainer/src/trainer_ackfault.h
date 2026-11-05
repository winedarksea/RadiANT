/* SPDX-License-Identifier: Apache-2.0 */
/*
 * trainer_ackfault.h - what the simulated trainer does with each acknowledged
 * command: acknowledge it (late, early, jittered), apply it and lose the ACK
 * (MUTE), or never hear it (DEAF).
 *
 * KERNEL-FREE, like trainer_model.h, so tests/model can draw ten thousand
 * decisions from a fixed seed and check the statistics. The Zephyr half - the
 * radio hook, the ground-truth ring and the shell - is trainer_shell.c.
 *
 * ONE DECISION PER RECEIVED ACKNOWLEDGED PACKET, IN THIS ORDER, and the order
 * is part of the model:
 *
 *   1. BUSY. Within busy_ms of the last command this trainer applied, MUTE.
 *      A busy MUTE does not extend the window - only an applied command
 *      starts one, and a busy one was applied.
 *   2. BURST. A two-state Gilbert chain steps once per packet: GOOD goes BAD
 *      with p_bad, BAD goes GOOD with p_good (both per-mille). In BAD the
 *      packet gets bad_action (MUTE or DEAF). Mean burst length is
 *      1000 / p_good packets.
 *   3. DEAF with deaf_pm per-mille.
 *   4. MUTE with mute_pm per-mille.
 *   5. Otherwise ACK at the normal instant + offset_us + a uniform draw in
 *      [-jitter_us, +jitter_us].
 *
 * A non-busy ACK or MUTE starts the busy window, because both mean the
 * command was applied. DEAF does not.
 *
 * The generator is xorshift32 from `seed`, re-seeded on every
 * trainer_ackfault_init()/_reset(), so a run is reproducible from its
 * parameters alone. Seed 0 is a xorshift fixed point and is replaced.
 */

#ifndef TRAINER_ACKFAULT_H_
#define TRAINER_ACKFAULT_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Values match antr_reply_action, which trainer_shell.c asserts. */
enum trainer_action {
	TRAINER_ACTION_ACK = 0,
	TRAINER_ACTION_MUTE,
	TRAINER_ACTION_DEAF
};

/* Which rule decided, for the ground-truth record. */
enum trainer_cause {
	TRAINER_CAUSE_NONE = 0, /* a plain ACK */
	TRAINER_CAUSE_BUSY,
	TRAINER_CAUSE_BURST,
	TRAINER_CAUSE_DEAF,
	TRAINER_CAUSE_MUTE
};

#define TRAINER_ACKFAULT_SEED_DEFAULT 0x2545F491u

struct trainer_ackfault_params {
	int32_t  offset_us;
	uint32_t jitter_us;
	uint16_t mute_pm;
	uint16_t deaf_pm;
	uint16_t p_bad_pm;
	uint16_t p_good_pm;
	uint8_t  bad_action; /* TRAINER_ACTION_MUTE or _DEAF */
	uint32_t busy_ms;
	uint32_t seed;
};

struct trainer_ackfault {
	struct trainer_ackfault_params p;

	uint32_t rng;
	bool     burst_bad;
	bool     busy_valid;
	uint64_t busy_until_us;

	/* Decisions so far, by action and by cause. */
	uint32_t n;
	uint32_t n_action[3];
	uint32_t n_cause[5];
};

struct trainer_ackfault_decision {
	uint8_t action; /* enum trainer_action */
	uint8_t cause;  /* enum trainer_cause */
	int32_t offset_us;
};

/* The "ideal" parameters: every packet acknowledged on time. */
void trainer_ackfault_params_ideal(struct trainer_ackfault_params *p);

/* Named presets: ideal, late300, late-edge, jitter, lossy25, wahoo. Returns 0,
 * or -1 for an unknown name (and leaves *p untouched). `wahoo` is a
 * placeholder equal to `ideal` until the sniffer calibration fits it. */
int trainer_ackfault_preset(const char *name, struct trainer_ackfault_params *p);

/* Install parameters and reset every piece of state, the generator included. */
void trainer_ackfault_init(struct trainer_ackfault *af,
			   const struct trainer_ackfault_params *p);

/* Same parameters, fresh state - what makes two runs comparable. */
void trainer_ackfault_reset(struct trainer_ackfault *af);

/* One decision for a packet received at `t_us` (any monotonic microsecond
 * clock; the radio's t_sync in the application). */
void trainer_ackfault_decide(struct trainer_ackfault *af, uint64_t t_us,
			     struct trainer_ackfault_decision *out);

/* Set one parameter from its shell spelling: offset_us, jitter_us, mute_pm,
 * deaf_pm, p_bad, p_good, bad_action (mute|deaf), busy_ms, seed. Returns 0,
 * or -1 for an unknown key, an unparsable value or one out of range (*p is
 * then untouched). Does not reset state; the caller decides. */
int trainer_ackfault_set(struct trainer_ackfault_params *p, const char *key,
			 const char *value);

const char *trainer_ackfault_action_name(uint8_t action);
const char *trainer_ackfault_cause_name(uint8_t cause);

#ifdef __cplusplus
}
#endif

#endif /* TRAINER_ACKFAULT_H_ */
