/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The FTMS core's kernel-free half (radiant_ble_ftms_core.c): the Control
 * Point decision table (opcode x state x caps x owner x auth), execution
 * against fake machine hooks, and the byte builders.
 *
 * The decision table is checked two ways: a sweep over every combination
 * asserting the rules that must hold everywhere (no control -> 0x05; a denied
 * motion -> 0x05; Stop/Pause always reach the machine), and explicit rows for
 * each op code's own answers.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/ztest.h>

#include <radiant/radiant_ble_ftms.h>

/* ── Fixtures ───────────────────────────────────────────────────────────── */

static enum radiant_ble_ftms_verdict verdict;
static int auth_calls;
static struct radiant_ble_ftms_motion last_motion;

static enum radiant_ble_ftms_verdict fake_check(const struct radiant_ble_ftms_motion *m,
						const struct radiant_ble_ftms_session *s,
						void *ctx)
{
	ARG_UNUSED(s);
	ARG_UNUSED(ctx);
	auth_calls++;
	last_motion = *m;
	return verdict;
}

static const struct radiant_ble_ftms_auth auth = { .check = fake_check };

static struct radiant_ble_ftms_caps treadmill_caps(void)
{
	struct radiant_ble_ftms_caps c = {
		.features = RADIANT_BLE_FTMS_FMF_TOTAL_DISTANCE,
		.targets = RADIANT_BLE_FTMS_TSF_SPEED | RADIANT_BLE_FTMS_TSF_INCLINATION,
		.speed = { .min = 0, .max = 2000, .step = 10, .valid = true },
		.incline = { .min = -30, .max = 150, .step = 5, .valid = true },
	};

	return c;
}

static struct radiant_ble_ftms_caps bike_caps(void)
{
	struct radiant_ble_ftms_caps c = {
		.targets = RADIANT_BLE_FTMS_TSF_RESISTANCE | RADIANT_BLE_FTMS_TSF_POWER |
			   RADIANT_BLE_FTMS_TSF_BIKE_SIM | RADIANT_BLE_FTMS_TSF_WHEEL_CIRC,
		.resistance = { .min = 0, .max = 200, .step = 10, .valid = true },
		.power = { .min = 0, .max = 2000, .step = 1, .valid = true },
	};

	return c;
}

static struct radiant_ble_ftms_cp_decision decide(const uint8_t *data, uint16_t len,
						  enum radiant_ble_ftms_run run,
						  const struct radiant_ble_ftms_caps *caps,
						  enum radiant_ble_ftms_owner owner)
{
	struct radiant_ble_ftms_cp_request req = { .data = data, .len = len, .epoch = 3u };
	struct radiant_ble_ftms_session s = { .run = run, .epoch = 3u };

	return radiant_ble_ftms_cp_decide(&req, &s, caps, owner, &auth);
}

static void core_reset(void *fixture)
{
	ARG_UNUSED(fixture);
	verdict = RADIANT_BLE_FTMS_PERMIT;
	auth_calls = 0;
	memset(&last_motion, 0, sizeof(last_motion));
}

/* Every request this table drives, each well formed. */
static const struct {
	uint8_t data[7];
	uint8_t len;
	bool motion; /* consults the motion check */
} requests[] = {
	{ { 0x00 }, 1, false },                         /* Request Control */
	{ { 0x01 }, 1, false },                         /* Reset (asks, never refused) */
	{ { 0x02, 0xE8, 0x03 }, 3, true },              /* speed 10.00 km/h */
	{ { 0x03, 0x32, 0x00 }, 3, true },              /* incline 5.0 % */
	{ { 0x04, 0x0A }, 2, true },                    /* resistance 1.0 */
	{ { 0x05, 0xC8, 0x00 }, 3, true },              /* power 200 W */
	{ { 0x07 }, 1, true },                          /* Start/Resume */
	{ { 0x08, 0x01 }, 2, false },                   /* Stop */
	{ { 0x08, 0x02 }, 2, false },                   /* Pause */
	{ { 0x11, 0, 0, 0x64, 0, 0x28, 0x33 }, 7, true }, /* bike sim */
	{ { 0x12, 0x10, 0x52 }, 3, true },              /* wheel */
	{ { 0x09 }, 1, false },                         /* not handled here */
};

static const enum radiant_ble_ftms_run runs[] = {
	RADIANT_BLE_FTMS_RUN_IDLE, RADIANT_BLE_FTMS_RUN_RUNNING, RADIANT_BLE_FTMS_RUN_PAUSED,
	RADIANT_BLE_FTMS_RUN_STOPPED, RADIANT_BLE_FTMS_RUN_SAFETY_STOP,
};

static const enum radiant_ble_ftms_owner owners[] = {
	RADIANT_BLE_FTMS_OWNER_NONE, RADIANT_BLE_FTMS_OWNER_SELF, RADIANT_BLE_FTMS_OWNER_OTHER,
};

static const enum radiant_ble_ftms_verdict verdicts[] = {
	RADIANT_BLE_FTMS_PERMIT, RADIANT_BLE_FTMS_DENY, RADIANT_BLE_FTMS_STALE,
};

/* ── The sweep ──────────────────────────────────────────────────────────── */

ZTEST(radiant_ftms_core, test_decision_table_sweep)
{
	struct radiant_ble_ftms_caps capsets[2] = { treadmill_caps(), bike_caps() };

	for (size_t r = 0; r < ARRAY_SIZE(requests); r++) {
		for (size_t c = 0; c < ARRAY_SIZE(capsets); c++) {
			for (size_t ru = 0; ru < ARRAY_SIZE(runs); ru++) {
				for (size_t o = 0; o < ARRAY_SIZE(owners); o++) {
					for (size_t v = 0; v < ARRAY_SIZE(verdicts); v++) {
						uint8_t op = requests[r].data[0];
						struct radiant_ble_ftms_cp_decision d;

						core_reset(NULL);
						verdict = verdicts[v];
						d = decide(requests[r].data, requests[r].len, runs[ru],
							   &capsets[c], owners[o]);
						zassert_equal(d.op, op, NULL);

						if (op == 0x00) {
							if (owners[o] == RADIANT_BLE_FTMS_OWNER_OTHER) {
								zassert_equal(d.result, 0x05, NULL);
								zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_NONE, NULL);
							} else {
								zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_GRANT, NULL);
							}
							zassert_equal(auth_calls, 0, NULL);
							continue;
						}

						/* RULE: nothing but Request Control without control. */
						if (owners[o] != RADIANT_BLE_FTMS_OWNER_SELF) {
							zassert_equal(d.result, 0x05, "op 0x%02x", op);
							zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_NONE, NULL);
							zassert_equal(auth_calls, 0,
								      "no motion check without control");
							continue;
						}

						/* RULE: Stop and Pause always reach the machine,
						 * and are not swapped. */
						if (op == 0x08) {
							zassert_equal(d.action,
								      (requests[r].data[1] == 0x01)
									      ? RADIANT_BLE_FTMS_ACT_STOP
									      : RADIANT_BLE_FTMS_ACT_PAUSE,
								      "run %u verdict %u", (unsigned)runs[ru],
								      (unsigned)verdicts[v]);
							zassert_equal(auth_calls, 0, "stopping is never asked");
							continue;
						}

						/* RULE: Reset always runs its stop half. */
						if (op == 0x01) {
							zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_RESET, NULL);
							zassert_equal(d.reset_motion_ok,
								      verdicts[v] == RADIANT_BLE_FTMS_PERMIT, NULL);
							continue;
						}

						if (op == 0x09) {
							zassert_equal(d.result, 0x02, NULL);
							continue;
						}

						if (op == 0x07 && runs[ru] == RADIANT_BLE_FTMS_RUN_RUNNING) {
							zassert_equal(d.result, 0x04, "Start when started");
							zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_NONE, NULL);
							continue;
						}

						/* Target ops the caps do not declare: 0x02, unasked. */
						if (op != 0x07) {
							uint32_t tsf =
								(op == 0x02) ? RADIANT_BLE_FTMS_TSF_SPEED :
								(op == 0x03) ? RADIANT_BLE_FTMS_TSF_INCLINATION :
								(op == 0x04) ? RADIANT_BLE_FTMS_TSF_RESISTANCE :
								(op == 0x05) ? RADIANT_BLE_FTMS_TSF_POWER :
								(op == 0x11) ? RADIANT_BLE_FTMS_TSF_BIKE_SIM :
									       RADIANT_BLE_FTMS_TSF_WHEEL_CIRC;

							if ((capsets[c].targets & tsf) == 0u) {
								zassert_equal(d.result, 0x02, "op 0x%02x", op);
								zassert_equal(auth_calls, 0, NULL);
								continue;
							}
						}

						/* RULE: the motion verdict decides the rest. */
						zassert_true(requests[r].motion, NULL);
						zassert_equal(auth_calls, 1, "op 0x%02x", op);
						switch (verdicts[v]) {
						case RADIANT_BLE_FTMS_PERMIT:
							/* The EXACT action: a START/RESUME swap
							 * would be caught here. */
							zassert_equal(d.action,
								      (op != 0x07) ? RADIANT_BLE_FTMS_ACT_SET_TARGET :
								      (runs[ru] == RADIANT_BLE_FTMS_RUN_PAUSED)
									      ? RADIANT_BLE_FTMS_ACT_RESUME
									      : RADIANT_BLE_FTMS_ACT_START,
								      "op 0x%02x run %u", op, (unsigned)runs[ru]);
							zassert_equal(d.epoch, 3u, "the receipt epoch rides along");
							break;
						case RADIANT_BLE_FTMS_DENY:
							zassert_equal(d.result, 0x05, "op 0x%02x", op);
							zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_NONE, NULL);
							break;
						default:
							zassert_equal(d.result, 0x04, "op 0x%02x", op);
							zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_NONE, NULL);
							break;
						}
					}
				}
			}
		}
	}
}

/* ── Explicit rows ──────────────────────────────────────────────────────── */

ZTEST(radiant_ftms_core, test_request_control_with_a_parameter_is_invalid)
{
	static const uint8_t req[] = { 0x00, 0x01 };
	struct radiant_ble_ftms_caps caps = treadmill_caps();
	struct radiant_ble_ftms_cp_decision d =
		decide(req, sizeof(req), RADIANT_BLE_FTMS_RUN_IDLE, &caps,
		       RADIANT_BLE_FTMS_OWNER_NONE);

	zassert_equal(d.result, 0x03, NULL);
	zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_NONE, NULL);
}

ZTEST(radiant_ftms_core, test_parameter_lengths_must_be_exact)
{
	static const uint8_t short_speed[] = { 0x02, 0xE8 };
	static const uint8_t long_speed[] = { 0x02, 0xE8, 0x03, 0x00 };
	static const uint8_t bare_stop[] = { 0x08 };
	static const uint8_t bad_stop[] = { 0x08, 0x03 };
	static const uint8_t reset_param[] = { 0x01, 0x00 };
	struct radiant_ble_ftms_caps caps = treadmill_caps();

	zassert_equal(decide(short_speed, sizeof(short_speed), RADIANT_BLE_FTMS_RUN_RUNNING,
			     &caps, RADIANT_BLE_FTMS_OWNER_SELF).result, 0x03, NULL);
	zassert_equal(decide(long_speed, sizeof(long_speed), RADIANT_BLE_FTMS_RUN_RUNNING,
			     &caps, RADIANT_BLE_FTMS_OWNER_SELF).result, 0x03, NULL);
	zassert_equal(decide(bare_stop, sizeof(bare_stop), RADIANT_BLE_FTMS_RUN_RUNNING,
			     &caps, RADIANT_BLE_FTMS_OWNER_SELF).result, 0x03, NULL);
	zassert_equal(decide(bad_stop, sizeof(bad_stop), RADIANT_BLE_FTMS_RUN_RUNNING,
			     &caps, RADIANT_BLE_FTMS_OWNER_SELF).result, 0x03, NULL);
	zassert_equal(decide(reset_param, sizeof(reset_param), RADIANT_BLE_FTMS_RUN_RUNNING,
			     &caps, RADIANT_BLE_FTMS_OWNER_SELF).result, 0x03, NULL);
	zassert_equal(auth_calls, 0, "a malformed request is never authorised");
}

ZTEST(radiant_ftms_core, test_out_of_range_targets_are_invalid_and_unasked)
{
	static const uint8_t fast[] = { 0x02, 0xD1, 0x07 };    /* 2001 > 2000 */
	static const uint8_t steep[] = { 0x03, 0x97, 0x00 };   /* 151 > 150 */
	static const uint8_t low[] = { 0x03, 0xE1, 0xFF };     /* -31 < -30 */
	static const uint8_t edge[] = { 0x03, 0xE2, 0xFF };    /* -30, legal */
	static const uint8_t power[] = { 0x05, 0xD1, 0x07 };   /* 2001 W */
	struct radiant_ble_ftms_caps tm = treadmill_caps();
	struct radiant_ble_ftms_caps bike = bike_caps();
	struct radiant_ble_ftms_cp_decision d;

	zassert_equal(decide(fast, 3, RADIANT_BLE_FTMS_RUN_RUNNING, &tm,
			     RADIANT_BLE_FTMS_OWNER_SELF).result, 0x03, NULL);
	zassert_equal(decide(steep, 3, RADIANT_BLE_FTMS_RUN_RUNNING, &tm,
			     RADIANT_BLE_FTMS_OWNER_SELF).result, 0x03, NULL);
	zassert_equal(decide(low, 3, RADIANT_BLE_FTMS_RUN_RUNNING, &tm,
			     RADIANT_BLE_FTMS_OWNER_SELF).result, 0x03, NULL);
	zassert_equal(decide(power, 3, RADIANT_BLE_FTMS_RUN_RUNNING, &bike,
			     RADIANT_BLE_FTMS_OWNER_SELF).result, 0x03, NULL);
	zassert_equal(auth_calls, 0, NULL);

	d = decide(edge, 3, RADIANT_BLE_FTMS_RUN_RUNNING, &tm, RADIANT_BLE_FTMS_OWNER_SELF);
	zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_SET_TARGET, NULL);
	zassert_equal(d.target.v.incline_deci, -30, NULL);
	zassert_equal(last_motion.target.v.incline_deci, -30,
		      "the motion check sees the parsed value");
}

ZTEST(radiant_ftms_core, test_start_from_pause_is_a_resume)
{
	static const uint8_t start[] = { 0x07 };
	struct radiant_ble_ftms_caps caps = treadmill_caps();
	struct radiant_ble_ftms_cp_decision d;

	d = decide(start, 1, RADIANT_BLE_FTMS_RUN_PAUSED, &caps, RADIANT_BLE_FTMS_OWNER_SELF);
	zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_RESUME, NULL);
	zassert_true(last_motion.resume, NULL);

	d = decide(start, 1, RADIANT_BLE_FTMS_RUN_IDLE, &caps, RADIANT_BLE_FTMS_OWNER_SELF);
	zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_START, NULL);
	zassert_false(last_motion.resume, NULL);
}

ZTEST(radiant_ftms_core, test_a_stale_epoch_reaches_the_check)
{
	static const uint8_t speed[] = { 0x02, 0xE8, 0x03 };
	struct radiant_ble_ftms_caps caps = treadmill_caps();
	struct radiant_ble_ftms_cp_request req = { .data = speed, .len = 3, .epoch = 1u };
	struct radiant_ble_ftms_session s = { .run = RADIANT_BLE_FTMS_RUN_RUNNING, .epoch = 2u };
	struct radiant_ble_ftms_cp_decision d;

	(void)radiant_ble_ftms_cp_decide(&req, &s, &caps, RADIANT_BLE_FTMS_OWNER_SELF, &auth);
	zassert_true(last_motion.stale, NULL);

	/* With no machine check at all (a passive machine), stale is still
	 * refused 0x04 and a fresh request permitted. */
	d = radiant_ble_ftms_cp_decide(&req, &s, &caps, RADIANT_BLE_FTMS_OWNER_SELF, NULL);
	zassert_equal(d.result, 0x04, NULL);
	zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_NONE, NULL);
	req.epoch = 2u;
	d = radiant_ble_ftms_cp_decide(&req, &s, &caps, RADIANT_BLE_FTMS_OWNER_SELF, NULL);
	zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_SET_TARGET, NULL);
}

ZTEST(radiant_ftms_core, test_bike_target_ops_parse)
{
	static const uint8_t sim[] = { 0x11, 0x2C, 0x01, 0x9C, 0xFF, 0x28, 0x33 };
	static const uint8_t wheel[] = { 0x12, 0x10, 0x52 };
	static const uint8_t res[] = { 0x04, 0x0A };
	struct radiant_ble_ftms_caps caps = bike_caps();
	struct radiant_ble_ftms_cp_decision d;

	d = decide(sim, sizeof(sim), RADIANT_BLE_FTMS_RUN_RUNNING, &caps,
		   RADIANT_BLE_FTMS_OWNER_SELF);
	zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_SET_TARGET, NULL);
	zassert_equal(d.target.v.sim.wind_mm_s, 300, NULL);
	zassert_equal(d.target.v.sim.grade_centi, -100, NULL);
	zassert_equal(d.target.v.sim.crr_e4, 0x28, NULL);
	zassert_equal(d.target.v.sim.cw_centi, 0x33, NULL);

	d = decide(wheel, 3, RADIANT_BLE_FTMS_RUN_IDLE, &caps, RADIANT_BLE_FTMS_OWNER_SELF);
	zassert_equal(d.target.v.wheel_deci_mm, 0x5210, NULL);

	d = decide(res, 2, RADIANT_BLE_FTMS_RUN_IDLE, &caps, RADIANT_BLE_FTMS_OWNER_SELF);
	zassert_equal(d.target.v.resistance_deci, 10, NULL);
}

/* ── Execution ──────────────────────────────────────────────────────────── */

static int set_target_rc, workout_rc, reset_rc;
static int set_target_calls, workout_calls, reset_calls;
static enum radiant_ble_ftms_workout last_workout;
static bool last_reset_motion_ok;

static uint32_t last_epoch;

/* When set, the hook reports this grade as the one it applied. */
static bool clamp_grade_to_set;
static int16_t clamp_grade_to;

static int fake_set_target(struct radiant_ble_ftms_target *t, uint32_t epoch, void *ctx)
{
	ARG_UNUSED(ctx);
	last_epoch = epoch;
	set_target_calls++;
	if (clamp_grade_to_set && t->op == RADIANT_BLE_FTMS_OP_SET_BIKE_SIM) {
		t->v.sim.grade_centi = clamp_grade_to;
	}
	return set_target_rc;
}

static int fake_workout(enum radiant_ble_ftms_workout w, uint32_t epoch, void *ctx)
{
	ARG_UNUSED(ctx);
	last_epoch = epoch;
	workout_calls++;
	last_workout = w;
	return workout_rc;
}

static int fake_reset(bool motion_ok, uint32_t epoch, void *ctx)
{
	ARG_UNUSED(ctx);
	last_epoch = epoch;
	reset_calls++;
	last_reset_motion_ok = motion_ok;
	return reset_rc;
}

static const struct radiant_ble_ftms_ops ops = {
	.set_target = fake_set_target, .workout = fake_workout, .reset = fake_reset,
};

static void exec_reset(void)
{
	set_target_rc = workout_rc = reset_rc = 0;
	set_target_calls = workout_calls = reset_calls = 0;
}

ZTEST(radiant_ftms_core, test_hooks_get_the_receipt_epoch)
{
	struct radiant_ble_ftms_cp_decision d = {
		.op = 0x02, .action = RADIANT_BLE_FTMS_ACT_SET_TARGET, .epoch = 41u,
		.target = { .op = 0x02 },
	};

	exec_reset();
	(void)radiant_ble_ftms_cp_execute(&d, &ops);
	zassert_equal(last_epoch, 41u, NULL);
	d.action = RADIANT_BLE_FTMS_ACT_STOP;
	d.epoch = 42u;
	(void)radiant_ble_ftms_cp_execute(&d, &ops);
	zassert_equal(last_epoch, 42u, NULL);
	d.action = RADIANT_BLE_FTMS_ACT_RESET;
	d.epoch = 43u;
	(void)radiant_ble_ftms_cp_execute(&d, &ops);
	zassert_equal(last_epoch, 43u, NULL);
}

ZTEST(radiant_ftms_core, test_an_oversize_write_is_invalid_parameter)
{
	/* The glue queues an over-long write with its true length; the exact
	 * length rule answers it 0x03 in a Response Code. */
	static const uint8_t big[12] = { 0x02, 0xE8, 0x03 };
	static const uint8_t big_rc[12] = { 0x00 };
	static const uint8_t big_unknown[12] = { 0x09 };
	struct radiant_ble_ftms_caps caps = treadmill_caps();

	zassert_equal(decide(big, sizeof(big), RADIANT_BLE_FTMS_RUN_RUNNING, &caps,
			     RADIANT_BLE_FTMS_OWNER_SELF).result, 0x03, NULL);
	zassert_equal(decide(big_rc, sizeof(big_rc), RADIANT_BLE_FTMS_RUN_RUNNING, &caps,
			     RADIANT_BLE_FTMS_OWNER_NONE).result, 0x03, NULL);
	zassert_equal(decide(big_unknown, sizeof(big_unknown), RADIANT_BLE_FTMS_RUN_RUNNING,
			     &caps, RADIANT_BLE_FTMS_OWNER_SELF).result, 0x02, NULL);
	zassert_equal(auth_calls, 0, NULL);
}

ZTEST(radiant_ftms_core, test_success_means_the_driver_accepted)
{
	struct radiant_ble_ftms_cp_decision d = {
		.op = 0x03, .action = RADIANT_BLE_FTMS_ACT_SET_TARGET,
		.target = { .op = 0x03, .v.incline_deci = 25 },
	};
	struct radiant_ble_ftms_cp_outcome o;
	static const int rcs[] = { 0, -ENOTSUP, -ERANGE, -EINVAL, -EIO, -EACCES, -ENODEV };
	static const uint8_t want[] = { 0x01, 0x02, 0x03, 0x03, 0x04, 0x04, 0x04 };

	for (size_t i = 0; i < ARRAY_SIZE(rcs); i++) {
		exec_reset();
		set_target_rc = rcs[i];
		o = radiant_ble_ftms_cp_execute(&d, &ops);
		zassert_equal(o.result, want[i], "rc %d", rcs[i]);
		zassert_equal(set_target_calls, 1, NULL);
		/* Target Inclination Changed only on acceptance. */
		zassert_equal(o.status_len, (rcs[i] == 0) ? 3u : 0u, NULL);
		if (rcs[i] == 0) {
			zassert_equal(o.status[0], 0x06, NULL);
		}
	}
}

ZTEST(radiant_ftms_core, test_target_status_bytes)
{
	struct radiant_ble_ftms_cp_decision d = {
		.op = 0x02, .action = RADIANT_BLE_FTMS_ACT_SET_TARGET,
		.target = { .op = 0x02, .v.speed_centi_kph = 1000 },
	};
	struct radiant_ble_ftms_cp_outcome o;

	exec_reset();
	o = radiant_ble_ftms_cp_execute(&d, &ops);
	zassert_equal(o.status_len, 3u, NULL);
	zassert_equal(o.status[0], 0x05, NULL);
	zassert_equal(o.status[1], 0xE8, NULL);
	zassert_equal(o.status[2], 0x03, NULL);

	d.op = d.target.op = 0x11;
	d.target.v.sim.wind_mm_s = -2;
	d.target.v.sim.grade_centi = 150;
	d.target.v.sim.crr_e4 = 40;
	d.target.v.sim.cw_centi = 51;
	o = radiant_ble_ftms_cp_execute(&d, &ops);
	zassert_equal(o.status_len, 7u, NULL);
	zassert_equal(o.status[0], 0x12, NULL);
	zassert_equal(o.status[1], 0xFE, NULL);
	zassert_equal(o.status[2], 0xFF, NULL);
	zassert_equal(o.status[3], 150, NULL);
	zassert_equal(o.status[5], 40, NULL);
	zassert_equal(o.status[6], 51, NULL);
}

ZTEST(radiant_ftms_core, test_workout_actions_and_their_absence)
{
	static const enum radiant_ble_ftms_action acts[] = {
		RADIANT_BLE_FTMS_ACT_START, RADIANT_BLE_FTMS_ACT_RESUME,
		RADIANT_BLE_FTMS_ACT_PAUSE, RADIANT_BLE_FTMS_ACT_STOP,
	};
	static const enum radiant_ble_ftms_workout ws[] = {
		RADIANT_BLE_FTMS_WORKOUT_START, RADIANT_BLE_FTMS_WORKOUT_RESUME,
		RADIANT_BLE_FTMS_WORKOUT_PAUSE, RADIANT_BLE_FTMS_WORKOUT_STOP,
	};
	struct radiant_ble_ftms_ops none = { 0 };

	for (size_t i = 0; i < ARRAY_SIZE(acts); i++) {
		struct radiant_ble_ftms_cp_decision d = { .op = 0x07, .action = acts[i] };
		struct radiant_ble_ftms_cp_outcome o;

		exec_reset();
		o = radiant_ble_ftms_cp_execute(&d, &ops);
		zassert_equal(o.result, 0x01, NULL);
		zassert_equal(last_workout, ws[i], NULL);
		/* No status: the session reports drive 0x02/0x04. */
		zassert_equal(o.status_len, 0u, NULL);

		o = radiant_ble_ftms_cp_execute(&d, &none);
		zassert_equal(o.result, 0x04,
			      "a mandatory op code with no hook fails; never 0x02");

		exec_reset();
		workout_rc = -ENOTSUP;
		o = radiant_ble_ftms_cp_execute(&d, &ops);
		zassert_equal(o.result, 0x04, NULL);
	}
}

ZTEST(radiant_ftms_core, test_reset_releases_only_when_whole)
{
	struct radiant_ble_ftms_cp_decision d = {
		.op = 0x01, .action = RADIANT_BLE_FTMS_ACT_RESET, .reset_motion_ok = true,
	};
	struct radiant_ble_ftms_cp_outcome o;

	exec_reset();
	o = radiant_ble_ftms_cp_execute(&d, &ops);
	zassert_equal(o.result, 0x01, NULL);
	zassert_true(o.release, NULL);
	zassert_true(o.reset_view, NULL);
	zassert_equal(o.status_len, 1u, NULL);
	zassert_equal(o.status[0], 0x01, NULL);

	/* The machine refused part of it. */
	exec_reset();
	reset_rc = -EIO;
	o = radiant_ble_ftms_cp_execute(&d, &ops);
	zassert_equal(o.result, 0x04, NULL);
	zassert_false(o.release, NULL);
	zassert_false(o.reset_view, NULL);
	zassert_equal(o.status_len, 0u, NULL);

	/* The non-stop half was denied: the stop half still runs (the hook is
	 * called, told so), and the answer is 0x04 even though it succeeded. */
	exec_reset();
	d.reset_motion_ok = false;
	o = radiant_ble_ftms_cp_execute(&d, &ops);
	zassert_equal(reset_calls, 1, NULL);
	zassert_false(last_reset_motion_ok, NULL);
	zassert_equal(o.result, 0x04, NULL);
	zassert_false(o.release, NULL);
	zassert_equal(o.status_len, 0u, NULL);
}

ZTEST(radiant_ftms_core, test_grant_and_refusals_touch_nothing)
{
	struct radiant_ble_ftms_cp_decision grant = { .op = 0x00,
						      .action = RADIANT_BLE_FTMS_ACT_GRANT };
	struct radiant_ble_ftms_cp_decision refused = { .op = 0x02, .result = 0x05 };
	struct radiant_ble_ftms_cp_outcome o;

	exec_reset();
	o = radiant_ble_ftms_cp_execute(&grant, &ops);
	zassert_true(o.grant, NULL);
	zassert_equal(o.result, 0x01, NULL);

	o = radiant_ble_ftms_cp_execute(&refused, &ops);
	zassert_equal(o.result, 0x05, NULL);
	zassert_false(o.grant, NULL);
	zassert_equal(set_target_calls + workout_calls + reset_calls, 0, NULL);
}

/* ── Builders ───────────────────────────────────────────────────────────── */

ZTEST(radiant_ftms_core, test_training_status_from_the_session)
{
	zassert_equal(radiant_ble_ftms_training_status(RADIANT_BLE_FTMS_RUN_IDLE), 0x01, NULL);
	zassert_equal(radiant_ble_ftms_training_status(RADIANT_BLE_FTMS_RUN_RUNNING), 0x0D, NULL);
	zassert_equal(radiant_ble_ftms_training_status(RADIANT_BLE_FTMS_RUN_PAUSED), 0x0D, NULL);
	zassert_equal(radiant_ble_ftms_training_status(RADIANT_BLE_FTMS_RUN_STOPPED), 0x0F, NULL);
	zassert_equal(radiant_ble_ftms_training_status(RADIANT_BLE_FTMS_RUN_SAFETY_STOP), 0x01,
		      NULL);
}

ZTEST(radiant_ftms_core, test_machine_status_for_each_transition)
{
	uint8_t m[2];

	zassert_equal(radiant_ble_ftms_session_status(RADIANT_BLE_FTMS_RUN_IDLE,
						      RADIANT_BLE_FTMS_RUN_RUNNING, m), 1u, NULL);
	zassert_equal(m[0], 0x04, NULL);
	zassert_equal(radiant_ble_ftms_session_status(RADIANT_BLE_FTMS_RUN_PAUSED,
						      RADIANT_BLE_FTMS_RUN_RUNNING, m), 1u, NULL);
	zassert_equal(m[0], 0x04, "resumed");

	zassert_equal(radiant_ble_ftms_session_status(RADIANT_BLE_FTMS_RUN_RUNNING,
						      RADIANT_BLE_FTMS_RUN_PAUSED, m), 2u, NULL);
	zassert_equal(m[0], 0x02, NULL);
	zassert_equal(m[1], 0x02, NULL);

	zassert_equal(radiant_ble_ftms_session_status(RADIANT_BLE_FTMS_RUN_RUNNING,
						      RADIANT_BLE_FTMS_RUN_STOPPED, m), 2u, NULL);
	zassert_equal(m[1], 0x01, NULL);
	zassert_equal(radiant_ble_ftms_session_status(RADIANT_BLE_FTMS_RUN_PAUSED,
						      RADIANT_BLE_FTMS_RUN_IDLE, m), 2u, NULL);
	zassert_equal(m[1], 0x01, NULL);

	zassert_equal(radiant_ble_ftms_session_status(RADIANT_BLE_FTMS_RUN_RUNNING,
						      RADIANT_BLE_FTMS_RUN_SAFETY_STOP, m), 1u, NULL);
	zassert_equal(m[0], 0x03, NULL);
	zassert_equal(radiant_ble_ftms_session_status(RADIANT_BLE_FTMS_RUN_IDLE,
						      RADIANT_BLE_FTMS_RUN_SAFETY_STOP, m), 1u, NULL);

	/* Nothing owed. */
	zassert_equal(radiant_ble_ftms_session_status(RADIANT_BLE_FTMS_RUN_STOPPED,
						      RADIANT_BLE_FTMS_RUN_IDLE, m), 0u, NULL);
	zassert_equal(radiant_ble_ftms_session_status(RADIANT_BLE_FTMS_RUN_SAFETY_STOP,
						      RADIANT_BLE_FTMS_RUN_IDLE, m), 0u, NULL);
	zassert_equal(radiant_ble_ftms_session_status(RADIANT_BLE_FTMS_RUN_RUNNING,
						      RADIANT_BLE_FTMS_RUN_RUNNING, m), 0u, NULL);
}

ZTEST(radiant_ftms_core, test_feature_range_and_service_data_bytes)
{
	struct radiant_ble_ftms_caps caps = treadmill_caps();
	struct radiant_ble_ftms_range none = { 0 };
	uint8_t f[8], r[6], sd[8];

	zassert_equal(radiant_ble_ftms_feature_encode(&caps, f), 8u, NULL);
	zassert_equal(f[0], 0x04, NULL);
	zassert_equal(f[4], 0x03, "speed + inclination targets");

	zassert_equal(radiant_ble_ftms_range_encode(&caps.incline, r), 6u, NULL);
	zassert_equal(r[0], 0xE2, NULL);
	zassert_equal(r[1], 0xFF, "SINT16 -30");
	zassert_equal(r[2], 150, NULL);
	zassert_equal(r[4], 5, NULL);
	(void)radiant_ble_ftms_range_encode(&none, r);
	zassert_equal(r[0] | r[1] | r[2] | r[3] | r[4] | r[5], 0, NULL);

	zassert_equal(radiant_ble_ftms_service_data(sd, sizeof(sd), true,
						    RADIANT_BLE_FTMS_TYPE_TREADMILL), 5u, NULL);
	zassert_equal(sd[0], 0x26, NULL);
	zassert_equal(sd[1], 0x18, NULL);
	zassert_equal(sd[2], 0x01, NULL);
	zassert_equal(sd[3], 0x01, NULL);
	zassert_equal(sd[4], 0x00, NULL);
	(void)radiant_ble_ftms_service_data(sd, sizeof(sd), true,
					    RADIANT_BLE_FTMS_TYPE_INDOOR_BIKE);
	zassert_equal(sd[3], 0x20, "indoor bike is bit 5");
	zassert_equal(radiant_ble_ftms_service_data(sd, 4, true, 1), 0u, NULL);
}

ZTEST(radiant_ftms_core, test_status_carries_the_value_the_hook_applied)
{
	struct radiant_ble_ftms_cp_decision d = {
		.op = 0x11, .action = RADIANT_BLE_FTMS_ACT_SET_TARGET,
		.target = { .op = 0x11, .v.sim = { .grade_centi = 30000, .crr_e4 = 40,
						    .cw_centi = 51 } },
	};
	struct radiant_ble_ftms_cp_outcome o;

	exec_reset();
	clamp_grade_to_set = true;
	clamp_grade_to = 20000; /* the machine clamped +300 % to +200 % */
	o = radiant_ble_ftms_cp_execute(&d, &ops);
	clamp_grade_to_set = false;
	zassert_equal(o.result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(o.status[0], 0x12);
	zassert_equal(o.status[3] | (o.status[4] << 8), 20000, "the applied grade");
	zassert_equal(d.target.v.sim.grade_centi, 30000, "the decision is not modified");
}

ZTEST(radiant_ftms_core, test_start_while_running_is_a_no_op_on_a_passive_machine)
{
	static const uint8_t start[] = { 0x07 };
	struct radiant_ble_ftms_caps bike = bike_caps();
	struct radiant_ble_ftms_caps tm = treadmill_caps();
	struct radiant_ble_ftms_cp_decision d;
	struct radiant_ble_ftms_cp_outcome o;

	bike.passive = true;
	d = decide(start, 1u, RADIANT_BLE_FTMS_RUN_RUNNING, &bike, RADIANT_BLE_FTMS_OWNER_SELF);
	zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_NONE);
	zassert_equal(d.result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	exec_reset();
	o = radiant_ble_ftms_cp_execute(&d, &ops);
	zassert_equal(o.result, RADIANT_BLE_FTMS_RESULT_SUCCESS);
	zassert_equal(workout_calls, 0, "a no-op reaches no hook");
	zassert_equal(o.status_len, 0u);

	/* Still subject to control, and not RUNNING behaves as before. */
	d = decide(start, 1u, RADIANT_BLE_FTMS_RUN_RUNNING, &bike, RADIANT_BLE_FTMS_OWNER_NONE);
	zassert_equal(d.result, RADIANT_BLE_FTMS_RESULT_CONTROL_NOT_PERMITTED);
	d = decide(start, 1u, RADIANT_BLE_FTMS_RUN_IDLE, &bike, RADIANT_BLE_FTMS_OWNER_SELF);
	zassert_equal(d.action, RADIANT_BLE_FTMS_ACT_START);

	/* A treadmill (passive false) is unchanged: 0x04. */
	d = decide(start, 1u, RADIANT_BLE_FTMS_RUN_RUNNING, &tm, RADIANT_BLE_FTMS_OWNER_SELF);
	zassert_equal(d.result, RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED);
}

ZTEST_SUITE(radiant_ftms_core, NULL, NULL, core_reset, NULL, NULL);
