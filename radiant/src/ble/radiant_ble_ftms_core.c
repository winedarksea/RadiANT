/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_ftms_core.c - the kernel-free half of the FTMS core: the Control
 * Point decision, its execution against the machine's hooks, and the byte
 * builders. See <radiant/radiant_ble_ftms.h>. No Zephyr header, no Bluetooth
 * host: radiant/tests/ftms and apps/treadmill/tests/pages compile this file
 * unmodified on native_sim.
 */

#include <errno.h>
#include <string.h>

#include <radiant/radiant_ble_ftms.h>

#define FTMS_UUID16 0x1826u

static void put_le16(uint8_t *out, uint16_t v)
{
	out[0] = (uint8_t)(v & 0xFFu);
	out[1] = (uint8_t)(v >> 8);
}

static uint16_t get_le16(const uint8_t *in)
{
	return (uint16_t)((uint16_t)in[0] | ((uint16_t)in[1] << 8));
}

static void put_le32(uint8_t *out, uint32_t v)
{
	put_le16(&out[0], (uint16_t)(v & 0xFFFFu));
	put_le16(&out[2], (uint16_t)(v >> 16));
}

uint8_t radiant_ble_ftms_result_for(int rc)
{
	if (rc == 0) {
		return RADIANT_BLE_FTMS_RESULT_SUCCESS;
	}
	if (rc == -ENOTSUP) {
		return RADIANT_BLE_FTMS_RESULT_OP_NOT_SUPPORTED;
	}
	if (rc == -EINVAL || rc == -ERANGE) {
		return RADIANT_BLE_FTMS_RESULT_INVALID_PARAM;
	}
	return RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED;
}

/* The target ops this core parses: their TSF bit and parameter length. */
static bool target_op_info(uint8_t op, uint32_t *tsf, uint16_t *plen)
{
	switch (op) {
	case RADIANT_BLE_FTMS_OP_SET_SPEED:
		*tsf = RADIANT_BLE_FTMS_TSF_SPEED;
		*plen = 2u;
		return true;
	case RADIANT_BLE_FTMS_OP_SET_INCLINE:
		*tsf = RADIANT_BLE_FTMS_TSF_INCLINATION;
		*plen = 2u;
		return true;
	case RADIANT_BLE_FTMS_OP_SET_RESISTANCE:
		*tsf = RADIANT_BLE_FTMS_TSF_RESISTANCE;
		*plen = 1u;
		return true;
	case RADIANT_BLE_FTMS_OP_SET_POWER:
		*tsf = RADIANT_BLE_FTMS_TSF_POWER;
		*plen = 2u;
		return true;
	case RADIANT_BLE_FTMS_OP_SET_BIKE_SIM:
		*tsf = RADIANT_BLE_FTMS_TSF_BIKE_SIM;
		*plen = 6u;
		return true;
	case RADIANT_BLE_FTMS_OP_SET_WHEEL_CIRC:
		*tsf = RADIANT_BLE_FTMS_TSF_WHEEL_CIRC;
		*plen = 2u;
		return true;
	default:
		return false;
	}
}

static bool in_range(const struct radiant_ble_ftms_range *r, int32_t v)
{
	return !r->valid || (v >= r->min && v <= r->max);
}

/* Parse a target op's parameter (length already checked) and range-check it
 * against the machine's Supported Range, where it has one. */
static bool parse_target(uint8_t op, const uint8_t *p,
			 const struct radiant_ble_ftms_caps *caps,
			 struct radiant_ble_ftms_target *t)
{
	memset(t, 0, sizeof(*t));
	t->op = op;

	switch (op) {
	case RADIANT_BLE_FTMS_OP_SET_SPEED:
		t->v.speed_centi_kph = get_le16(p);
		return in_range(&caps->speed, t->v.speed_centi_kph);
	case RADIANT_BLE_FTMS_OP_SET_INCLINE:
		t->v.incline_deci = (int16_t)get_le16(p);
		return in_range(&caps->incline, t->v.incline_deci);
	case RADIANT_BLE_FTMS_OP_SET_RESISTANCE:
		t->v.resistance_deci = p[0];
		return in_range(&caps->resistance, t->v.resistance_deci);
	case RADIANT_BLE_FTMS_OP_SET_POWER:
		t->v.power_w = (int16_t)get_le16(p);
		return in_range(&caps->power, t->v.power_w);
	case RADIANT_BLE_FTMS_OP_SET_BIKE_SIM:
		/* No Supported Range covers these; the machine's hook answers
		 * -EINVAL/-ERANGE (0x03) for a value it cannot take. */
		t->v.sim.wind_mm_s = (int16_t)get_le16(&p[0]);
		t->v.sim.grade_centi = (int16_t)get_le16(&p[2]);
		t->v.sim.crr_e4 = p[4];
		t->v.sim.cw_centi = p[5];
		return true;
	case RADIANT_BLE_FTMS_OP_SET_WHEEL_CIRC:
		t->v.wheel_deci_mm = get_le16(p);
		return true;
	default:
		return false;
	}
}

static enum radiant_ble_ftms_verdict
check_motion(const struct radiant_ble_ftms_auth *auth,
	     const struct radiant_ble_ftms_motion *m,
	     const struct radiant_ble_ftms_session *s)
{
	if (auth == NULL || auth->check == NULL) {
		return m->stale ? RADIANT_BLE_FTMS_STALE : RADIANT_BLE_FTMS_PERMIT;
	}
	return auth->check(m, s, auth->ctx);
}

static uint8_t verdict_result(enum radiant_ble_ftms_verdict v)
{
	return (v == RADIANT_BLE_FTMS_STALE) ? RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED
					     : RADIANT_BLE_FTMS_RESULT_CONTROL_NOT_PERMITTED;
}

struct radiant_ble_ftms_cp_decision
radiant_ble_ftms_cp_decide(const struct radiant_ble_ftms_cp_request *req,
			   const struct radiant_ble_ftms_session *session,
			   const struct radiant_ble_ftms_caps *caps,
			   enum radiant_ble_ftms_owner owner,
			   const struct radiant_ble_ftms_auth *auth)
{
	struct radiant_ble_ftms_cp_decision d = {
		.result = RADIANT_BLE_FTMS_RESULT_OP_NOT_SUPPORTED,
		.action = RADIANT_BLE_FTMS_ACT_NONE,
	};
	struct radiant_ble_ftms_motion m = { 0 };
	enum radiant_ble_ftms_verdict v;
	uint32_t tsf;
	uint16_t plen;
	uint8_t op;

	if (req == NULL || req->data == NULL || req->len < 1u || session == NULL ||
	    caps == NULL) {
		d.result = RADIANT_BLE_FTMS_RESULT_INVALID_PARAM;
		return d;
	}

	op = req->data[0];
	d.op = op;
	d.epoch = req->epoch;
	m.op = op;
	m.stale = (req->epoch != session->epoch);

	/* 1. Request Control. */
	if (op == RADIANT_BLE_FTMS_OP_REQUEST_CONTROL) {
		if (owner == RADIANT_BLE_FTMS_OWNER_OTHER) {
			d.result = RADIANT_BLE_FTMS_RESULT_CONTROL_NOT_PERMITTED;
		} else if (req->len != 1u) {
			d.result = RADIANT_BLE_FTMS_RESULT_INVALID_PARAM;
		} else {
			d.action = RADIANT_BLE_FTMS_ACT_GRANT;
		}
		return d;
	}

	/* 2. Every other procedure requires control (§4.16.2). */
	if (owner != RADIANT_BLE_FTMS_OWNER_SELF) {
		d.result = RADIANT_BLE_FTMS_RESULT_CONTROL_NOT_PERMITTED;
		return d;
	}

	if (target_op_info(op, &tsf, &plen)) {
		/* 3. Honoured only if declared (§4.16.2: conditionally
		 * EXCLUDED otherwise, so the two go together). */
		if ((caps->targets & tsf) == 0u) {
			d.result = RADIANT_BLE_FTMS_RESULT_OP_NOT_SUPPORTED;
			return d;
		}
		/* 4. */
		if (req->len != (uint16_t)(1u + plen) ||
		    !parse_target(op, &req->data[1], caps, &d.target)) {
			d.result = RADIANT_BLE_FTMS_RESULT_INVALID_PARAM;
			return d;
		}
		/* 6. */
		m.target = d.target;
		v = check_motion(auth, &m, session);
		if (v != RADIANT_BLE_FTMS_PERMIT) {
			d.result = verdict_result(v);
			return d;
		}
		d.action = RADIANT_BLE_FTMS_ACT_SET_TARGET;
		return d;
	}

	switch (op) {
	case RADIANT_BLE_FTMS_OP_RESET:
		if (req->len != 1u) {
			d.result = RADIANT_BLE_FTMS_RESULT_INVALID_PARAM;
			return d;
		}
		/* The stop half always runs; the rest only if authorised. */
		d.reset_motion_ok = (check_motion(auth, &m, session) == RADIANT_BLE_FTMS_PERMIT);
		d.action = RADIANT_BLE_FTMS_ACT_RESET;
		return d;

	case RADIANT_BLE_FTMS_OP_START_RESUME:
		if (req->len != 1u) {
			d.result = RADIANT_BLE_FTMS_RESULT_INVALID_PARAM;
			return d;
		}
		/* 5. Table 4.24: Start when already started is Operation
		 * Failed, not Success - a client uses the difference to decide
		 * whether it started the session. */
		if (session->run == RADIANT_BLE_FTMS_RUN_RUNNING) {
			/* A passive machine's session starts when the user
			 * moves it, so "already started" is the request met. */
			d.result = caps->passive ? RADIANT_BLE_FTMS_RESULT_SUCCESS
						 : RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED;
			return d;
		}
		m.resume = (session->run == RADIANT_BLE_FTMS_RUN_PAUSED);
		v = check_motion(auth, &m, session);
		if (v != RADIANT_BLE_FTMS_PERMIT) {
			d.result = verdict_result(v);
			return d;
		}
		d.action = m.resume ? RADIANT_BLE_FTMS_ACT_RESUME : RADIANT_BLE_FTMS_ACT_START;
		return d;

	case RADIANT_BLE_FTMS_OP_STOP_PAUSE:
		if (req->len != 2u || (req->data[1] != RADIANT_BLE_FTMS_PARAM_STOP &&
				       req->data[1] != RADIANT_BLE_FTMS_PARAM_PAUSE)) {
			d.result = RADIANT_BLE_FTMS_RESULT_INVALID_PARAM;
			return d;
		}
		/* Never gated, never refused for the state or the epoch:
		 * stopping always reaches the machine. */
		d.action = (req->data[1] == RADIANT_BLE_FTMS_PARAM_STOP)
				   ? RADIANT_BLE_FTMS_ACT_STOP
				   : RADIANT_BLE_FTMS_ACT_PAUSE;
		return d;

	default:
		d.result = RADIANT_BLE_FTMS_RESULT_OP_NOT_SUPPORTED;
		return d;
	}
}

/* The Fitness Machine Status a successful target op owes (Table 4.26). */
static uint8_t target_status(const struct radiant_ble_ftms_target *t, uint8_t *out)
{
	switch (t->op) {
	case RADIANT_BLE_FTMS_OP_SET_SPEED:
		out[0] = RADIANT_BLE_FTMS_STATUS_TARGET_SPEED;
		put_le16(&out[1], t->v.speed_centi_kph);
		return 3u;
	case RADIANT_BLE_FTMS_OP_SET_INCLINE:
		out[0] = RADIANT_BLE_FTMS_STATUS_TARGET_INCLINE;
		put_le16(&out[1], (uint16_t)t->v.incline_deci);
		return 3u;
	case RADIANT_BLE_FTMS_OP_SET_RESISTANCE:
		out[0] = RADIANT_BLE_FTMS_STATUS_TARGET_RESISTANCE;
		out[1] = t->v.resistance_deci;
		return 2u;
	case RADIANT_BLE_FTMS_OP_SET_POWER:
		out[0] = RADIANT_BLE_FTMS_STATUS_TARGET_POWER;
		put_le16(&out[1], (uint16_t)t->v.power_w);
		return 3u;
	case RADIANT_BLE_FTMS_OP_SET_BIKE_SIM:
		out[0] = RADIANT_BLE_FTMS_STATUS_BIKE_SIM;
		put_le16(&out[1], (uint16_t)t->v.sim.wind_mm_s);
		put_le16(&out[3], (uint16_t)t->v.sim.grade_centi);
		out[5] = t->v.sim.crr_e4;
		out[6] = t->v.sim.cw_centi;
		return 7u;
	case RADIANT_BLE_FTMS_OP_SET_WHEEL_CIRC:
		out[0] = RADIANT_BLE_FTMS_STATUS_WHEEL_CIRC;
		put_le16(&out[1], t->v.wheel_deci_mm);
		return 3u;
	default:
		return 0u;
	}
}

/* Start/Resume and Stop/Pause are mandatory (Table 4.15): a machine that
 * cannot do one fails the procedure (0x04) rather than calling a mandatory op
 * code unsupported. */
static uint8_t workout(const struct radiant_ble_ftms_ops *ops, enum radiant_ble_ftms_workout w,
		       uint32_t epoch)
{
	int rc = (ops != NULL && ops->workout != NULL) ? ops->workout(w, epoch, ops->ctx)
						      : -ENOTSUP;

	return (rc == -ENOTSUP) ? RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED
				: radiant_ble_ftms_result_for(rc);
}

struct radiant_ble_ftms_cp_outcome
radiant_ble_ftms_cp_execute(const struct radiant_ble_ftms_cp_decision *d,
			    const struct radiant_ble_ftms_ops *ops)
{
	struct radiant_ble_ftms_cp_outcome o = { .op = d->op, .result = d->result };
	int rc;

	switch (d->action) {
	case RADIANT_BLE_FTMS_ACT_NONE:
		break;

	case RADIANT_BLE_FTMS_ACT_GRANT:
		o.result = RADIANT_BLE_FTMS_RESULT_SUCCESS;
		o.grant = true;
		break;

	case RADIANT_BLE_FTMS_ACT_RESET:
		rc = (ops != NULL && ops->reset != NULL)
			     ? ops->reset(d->reset_motion_ok, d->epoch, ops->ctx)
			     : -ENOTSUP;
		if (!d->reset_motion_ok) {
			/* The stop half ran (or was attempted); the procedure as
			 * a whole did not happen. */
			o.result = RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED;
			break;
		}
		o.result = (rc == 0) ? RADIANT_BLE_FTMS_RESULT_SUCCESS
				     : RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED;
		if (rc == 0) {
			/* §4.16.2: Reset revokes control, and the FTMS view's
			 * accumulated fields go to zero. */
			o.release = true;
			o.reset_view = true;
			o.status[0] = RADIANT_BLE_FTMS_STATUS_RESET;
			o.status_len = 1u;
		}
		break;

	case RADIANT_BLE_FTMS_ACT_SET_TARGET: {
		/* The hook may rewrite the copy to what it applied. */
		struct radiant_ble_ftms_target applied = d->target;

		rc = (ops != NULL && ops->set_target != NULL)
			     ? ops->set_target(&applied, d->epoch, ops->ctx)
			     : -ENOTSUP;
		o.result = radiant_ble_ftms_result_for(rc);
		if (rc == 0) {
			o.status_len = target_status(&applied, o.status);
		}
		break;
	}

	case RADIANT_BLE_FTMS_ACT_START:
		o.result = workout(ops, RADIANT_BLE_FTMS_WORKOUT_START, d->epoch);
		break;
	case RADIANT_BLE_FTMS_ACT_RESUME:
		o.result = workout(ops, RADIANT_BLE_FTMS_WORKOUT_RESUME, d->epoch);
		break;
	case RADIANT_BLE_FTMS_ACT_PAUSE:
		o.result = workout(ops, RADIANT_BLE_FTMS_WORKOUT_PAUSE, d->epoch);
		break;
	case RADIANT_BLE_FTMS_ACT_STOP:
		o.result = workout(ops, RADIANT_BLE_FTMS_WORKOUT_STOP, d->epoch);
		break;
	}

	return o;
}

uint8_t radiant_ble_ftms_training_status(enum radiant_ble_ftms_run run)
{
	switch (run) {
	case RADIANT_BLE_FTMS_RUN_RUNNING:
	case RADIANT_BLE_FTMS_RUN_PAUSED:
		return RADIANT_BLE_FTMS_TRAINING_MANUAL_MODE;
	case RADIANT_BLE_FTMS_RUN_STOPPED:
		return RADIANT_BLE_FTMS_TRAINING_POST_WORKOUT;
	default:
		return RADIANT_BLE_FTMS_TRAINING_IDLE;
	}
}

size_t radiant_ble_ftms_session_status(enum radiant_ble_ftms_run prev,
				       enum radiant_ble_ftms_run next, uint8_t *out)
{
	bool was_moving = (prev == RADIANT_BLE_FTMS_RUN_RUNNING) ||
			  (prev == RADIANT_BLE_FTMS_RUN_PAUSED);

	if (prev == next) {
		return 0u;
	}
	switch (next) {
	case RADIANT_BLE_FTMS_RUN_SAFETY_STOP:
		out[0] = RADIANT_BLE_FTMS_STATUS_STOPPED_SAFETY_KEY;
		return 1u;
	case RADIANT_BLE_FTMS_RUN_RUNNING:
		out[0] = RADIANT_BLE_FTMS_STATUS_STARTED_OR_RESUMED;
		return 1u;
	case RADIANT_BLE_FTMS_RUN_PAUSED:
		if (prev != RADIANT_BLE_FTMS_RUN_RUNNING) {
			return 0u;
		}
		out[0] = RADIANT_BLE_FTMS_STATUS_STOPPED_OR_PAUSED;
		out[1] = RADIANT_BLE_FTMS_PARAM_PAUSE;
		return 2u;
	case RADIANT_BLE_FTMS_RUN_STOPPED:
	case RADIANT_BLE_FTMS_RUN_IDLE:
		if (!was_moving) {
			return 0u;
		}
		out[0] = RADIANT_BLE_FTMS_STATUS_STOPPED_OR_PAUSED;
		out[1] = RADIANT_BLE_FTMS_PARAM_STOP;
		return 2u;
	default:
		return 0u;
	}
}

size_t radiant_ble_ftms_feature_encode(const struct radiant_ble_ftms_caps *caps,
				       uint8_t *out)
{
	put_le32(&out[0], caps->features);
	put_le32(&out[4], caps->targets);
	return 8u;
}

size_t radiant_ble_ftms_range_encode(const struct radiant_ble_ftms_range *r,
				     uint8_t *out)
{
	if (!r->valid) {
		memset(out, 0, 6u);
		return 6u;
	}
	/* GSS: Minimum and Maximum are UINT16 (speed) or SINT16 (incline,
	 * resistance, power) - the same 16 bits either way - and the Minimum
	 * Increment is UINT16. */
	put_le16(&out[0], (uint16_t)r->min);
	put_le16(&out[2], (uint16_t)r->max);
	put_le16(&out[4], r->step);
	return 6u;
}

size_t radiant_ble_ftms_service_data(uint8_t *out, size_t cap, bool available,
				     uint16_t machine_type)
{
	if (out == NULL || cap < RADIANT_BLE_FTMS_SVC_DATA_LEN) {
		return 0u;
	}
	put_le16(&out[0], FTMS_UUID16);
	out[2] = available ? RADIANT_BLE_FTMS_SD_FLAG_AVAILABLE : 0u;
	put_le16(&out[3], machine_type);
	return RADIANT_BLE_FTMS_SVC_DATA_LEN;
}
