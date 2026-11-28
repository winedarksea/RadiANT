/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_ble_ftms.h - the Bluetooth SIG Fitness Machine Service (FTMS 1.0)
 * core, shared by every fitness-machine reference design: a treadmill
 * (apps/treadmill) today, an indoor bike next (CONFIG_RADIANT_BLE_FTMS).
 *
 * ─── What is common, and what is the machine's ─────────────────────────────
 *
 * COMMON, here: the service skeleton; the Fitness Machine Feature read; the
 * Control Point's framing, its Request Control requirement and its two ATT
 * errors (0xFE Procedure Already In Progress, a Core Specification
 * Supplement ATT application error, while a procedure is pending or
 * its indication is outstanding, 0xFD CCC Improperly Configured without an
 * indication subscription, §4.16.3); Training Status; the Fitness Machine
 * Status builder and its notification to EVERY subscribed client (the
 * initiating one included); the Supported Range reads; the advertising
 * Service Data; the FE-C/FTMS control arbiter hook-up.
 *
 * THE MACHINE'S, through struct radiant_ble_ftms_machine: its data record
 * (Treadmill Data, Indoor Bike Data - encode_data), which ranges it has
 * (caps), what each target op does to its hardware (ops), whether a motion
 * command is authorised (auth), its session (session/epoch) and what a Reset
 * zeroes in its FTMS view (reset_view). The GATT table carries the data and
 * range characteristics of the machine type chosen in Kconfig
 * (RADIANT_BLE_FTMS_TREADMILL or _INDOOR_BIKE).
 *
 * ─── The Control Point as a pure function ──────────────────────────────────
 *
 *   radiant_ble_ftms_cp_decide(request, session, caps, owner, auth)
 *       -> decision { result, action, target }
 *   radiant_ble_ftms_cp_execute(decision, ops)
 *       -> outcome { result, grant, release, reset_view, status }
 *
 * Both kernel-free (radiant_ble_ftms_core.c), compiled unmodified by
 * radiant/tests/ftms and apps/treadmill/tests/pages. The GATT glue
 * (radiant_ble_ftms.c) only moves bytes and runs the outcome.
 *
 * ─── What a result means ───────────────────────────────────────────────────
 *
 * 0x01 Success means the machine's driver ACCEPTED the request (its hook
 * returned 0). Training Status and the 0x02/0x03/0x04 machine-status
 * notifications follow the machine's own session reports
 * (radiant_ble_ftms_session_changed()), never the command: a machine that
 * accepts Start and never starts stays Idle, and clients see that. Target
 * changes (0x05..0x08, 0x12, 0x13) and Reset (0x01) are notified on
 * acceptance, because they are about the setpoint, not the motion.
 *
 * ─── Choices where FTMS 1.0 is not explicit ────────────────────────────────
 *
 *   - Parameter lengths must be exact; a short or long parameter is 0x03 -
 *     including a write longer than the glue's buffer, which is answered with
 *     a Response Code rather than an ATT error.
 *   - Stop and Pause always reach the machine, in every state: a client whose
 *     Start is still spinning the belt up must be able to stop it even though
 *     the session has not reported RUNNING yet.
 *   - Start while RUNNING is 0x04 (Table 4.24's "not possible in the current
 *     state") - except on a PASSIVE machine (caps.passive, an indoor bike),
 *     whose session starts when the rider pedals: there Start while RUNNING
 *     is 0x01 and does nothing, because the client's goal is already met;
 *     state"); Start while PAUSED is a resume.
 *   - Start/Resume and Stop/Pause are mandatory op codes (Table 4.15), so a
 *     machine whose workout hook answers -ENOTSUP gets 0x04, never 0x02.
 *   - Every procedure but Request Control, Stop and Pause included, needs
 *     control (§4.16.2). A stop is always available at the machine itself.
 *   - A successful Reset sets Training Status to Idle (§4.16.2); the machine's
 *     reset_done hook returns its own session to idle with it.
 *   - The Fitness Machine Status goes to every subscriber, the initiating
 *     client included (§4.17's "other clients" read as "also"); 0xFF goes to
 *     the client that lost control only.
 *   - A request queued before the machine's epoch moved (a local stop, a
 *     safety-key event) and that would actuate is answered 0x04 without
 *     reaching the machine. Stop/Pause and Request Control are not refused
 *     for that.
 */
#ifndef RADIANT_BLE_FTMS_H_
#define RADIANT_BLE_FTMS_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ── Table 4.15: Control Point op codes ─────────────────────────────────── */
#define RADIANT_BLE_FTMS_OP_REQUEST_CONTROL  0x00u
#define RADIANT_BLE_FTMS_OP_RESET            0x01u
#define RADIANT_BLE_FTMS_OP_SET_SPEED        0x02u /* UINT16, 0.01 km/h */
#define RADIANT_BLE_FTMS_OP_SET_INCLINE      0x03u /* SINT16, 0.1 % */
#define RADIANT_BLE_FTMS_OP_SET_RESISTANCE   0x04u /* UINT8, 0.1 */
#define RADIANT_BLE_FTMS_OP_SET_POWER        0x05u /* SINT16, W */
#define RADIANT_BLE_FTMS_OP_START_RESUME     0x07u
#define RADIANT_BLE_FTMS_OP_STOP_PAUSE       0x08u /* UINT8: 0x01 stop, 0x02 pause */
#define RADIANT_BLE_FTMS_OP_SET_BIKE_SIM     0x11u /* wind, grade, crr, cw */
#define RADIANT_BLE_FTMS_OP_SET_WHEEL_CIRC   0x12u /* UINT16, 0.1 mm */
#define RADIANT_BLE_FTMS_OP_RESPONSE_CODE    0x80u

#define RADIANT_BLE_FTMS_PARAM_STOP  0x01u
#define RADIANT_BLE_FTMS_PARAM_PAUSE 0x02u

/* ── Table 4.24: result codes ───────────────────────────────────────────── */
#define RADIANT_BLE_FTMS_RESULT_SUCCESS               0x01u
#define RADIANT_BLE_FTMS_RESULT_OP_NOT_SUPPORTED      0x02u
#define RADIANT_BLE_FTMS_RESULT_INVALID_PARAM         0x03u
#define RADIANT_BLE_FTMS_RESULT_OPERATION_FAILED      0x04u
#define RADIANT_BLE_FTMS_RESULT_CONTROL_NOT_PERMITTED 0x05u

/* ── Table 4.26: Fitness Machine Status op codes ────────────────────────── */
#define RADIANT_BLE_FTMS_STATUS_RESET               0x01u
#define RADIANT_BLE_FTMS_STATUS_STOPPED_OR_PAUSED   0x02u /* + 0x01 stop / 0x02 pause */
#define RADIANT_BLE_FTMS_STATUS_STOPPED_SAFETY_KEY  0x03u
#define RADIANT_BLE_FTMS_STATUS_STARTED_OR_RESUMED  0x04u
#define RADIANT_BLE_FTMS_STATUS_TARGET_SPEED        0x05u
#define RADIANT_BLE_FTMS_STATUS_TARGET_INCLINE      0x06u
#define RADIANT_BLE_FTMS_STATUS_TARGET_RESISTANCE   0x07u
#define RADIANT_BLE_FTMS_STATUS_TARGET_POWER        0x08u
#define RADIANT_BLE_FTMS_STATUS_BIKE_SIM            0x12u
#define RADIANT_BLE_FTMS_STATUS_WHEEL_CIRC          0x13u
#define RADIANT_BLE_FTMS_STATUS_CONTROL_LOST        0xFFu

/* ── Table 4.13: Training Status values used here ───────────────────────── */
#define RADIANT_BLE_FTMS_TRAINING_IDLE         0x01u
#define RADIANT_BLE_FTMS_TRAINING_MANUAL_MODE  0x0Du /* "Quick Start" */
#define RADIANT_BLE_FTMS_TRAINING_POST_WORKOUT 0x0Fu

/* ── Table 4.3: Fitness Machine Features (first UINT32) ─────────────────── */
#define RADIANT_BLE_FTMS_FMF_AVERAGE_SPEED    (1u << 0)
#define RADIANT_BLE_FTMS_FMF_CADENCE          (1u << 1)
#define RADIANT_BLE_FTMS_FMF_TOTAL_DISTANCE   (1u << 2)
#define RADIANT_BLE_FTMS_FMF_INCLINATION      (1u << 3)
#define RADIANT_BLE_FTMS_FMF_ELEVATION_GAIN   (1u << 4)
#define RADIANT_BLE_FTMS_FMF_PACE             (1u << 5)
#define RADIANT_BLE_FTMS_FMF_RESISTANCE_LEVEL (1u << 7)
#define RADIANT_BLE_FTMS_FMF_EXPENDED_ENERGY  (1u << 9)
#define RADIANT_BLE_FTMS_FMF_HEART_RATE       (1u << 10)
#define RADIANT_BLE_FTMS_FMF_METABOLIC_EQUIV  (1u << 11)
#define RADIANT_BLE_FTMS_FMF_ELAPSED_TIME     (1u << 12)
#define RADIANT_BLE_FTMS_FMF_REMAINING_TIME   (1u << 13)
#define RADIANT_BLE_FTMS_FMF_POWER            (1u << 14)
#define RADIANT_BLE_FTMS_FMF_FORCE_ON_BELT    (1u << 15)

/* ── Table 4.4: Target Setting Features (second UINT32) ─────────────────── */
#define RADIANT_BLE_FTMS_TSF_SPEED        (1u << 0)
#define RADIANT_BLE_FTMS_TSF_INCLINATION  (1u << 1)
#define RADIANT_BLE_FTMS_TSF_RESISTANCE   (1u << 2)
#define RADIANT_BLE_FTMS_TSF_POWER        (1u << 3)
#define RADIANT_BLE_FTMS_TSF_BIKE_SIM     (1u << 13)
#define RADIANT_BLE_FTMS_TSF_WHEEL_CIRC   (1u << 14)

/* ── Table 3.1: advertising Service Data ────────────────────────────────── */
#define RADIANT_BLE_FTMS_SD_FLAG_AVAILABLE    0x01u
#define RADIANT_BLE_FTMS_TYPE_TREADMILL       (1u << 0)
#define RADIANT_BLE_FTMS_TYPE_CROSS_TRAINER   (1u << 1)
#define RADIANT_BLE_FTMS_TYPE_STEP_CLIMBER    (1u << 2)
#define RADIANT_BLE_FTMS_TYPE_STAIR_CLIMBER   (1u << 3)
#define RADIANT_BLE_FTMS_TYPE_ROWER           (1u << 4)
#define RADIANT_BLE_FTMS_TYPE_INDOOR_BIKE     (1u << 5)
/* UUID16 (2) + Flags (1) + Fitness Machine Type (2). */
#define RADIANT_BLE_FTMS_SVC_DATA_LEN 5u

/* The longest Fitness Machine Status (op + the 6-byte bike sim block). */
#define RADIANT_BLE_FTMS_STATUS_MAX 7u

/* ── The machine's session, as the core sees it ─────────────────────────── */

enum radiant_ble_ftms_run {
	RADIANT_BLE_FTMS_RUN_IDLE = 0,
	RADIANT_BLE_FTMS_RUN_RUNNING,
	RADIANT_BLE_FTMS_RUN_PAUSED,
	RADIANT_BLE_FTMS_RUN_STOPPED,
	RADIANT_BLE_FTMS_RUN_SAFETY_STOP,
};

struct radiant_ble_ftms_session {
	enum radiant_ble_ftms_run run;
	uint32_t epoch; /* bumps on every local stop / safety event */
};

/* ── Capabilities ───────────────────────────────────────────────────────── */

/* A Supported Range: min and max in the target's own unit (UINT16 speed,
 * SINT16 incline/resistance/power) and the minimum increment. `valid` false
 * means "no range": the core does not range-check and the read is all zero. */
struct radiant_ble_ftms_range {
	int32_t min;
	int32_t max;
	uint16_t step;
	bool valid;
};

struct radiant_ble_ftms_caps {
	uint32_t features; /* FMF bits */
	uint32_t targets;  /* TSF bits: which target op codes are honoured */
	struct radiant_ble_ftms_range speed;      /* 0.01 km/h */
	struct radiant_ble_ftms_range incline;    /* 0.1 % */
	struct radiant_ble_ftms_range resistance; /* 0.1, unitless */
	struct radiant_ble_ftms_range power;      /* W */
	/* A passive machine (an indoor bike): nothing moves unless the user
	 * moves it, and its session may start without a Start. Start/Resume
	 * while RUNNING is then a 0x01 no-op instead of 0x04. false (the
	 * zero value) for a treadmill. */
	bool passive;
};

/* ── A request, decided ─────────────────────────────────────────────────── */

/* The requesting client's relation to FTMS control (§4.16.2). */
enum radiant_ble_ftms_owner {
	RADIANT_BLE_FTMS_OWNER_NONE = 0, /* nobody has control */
	RADIANT_BLE_FTMS_OWNER_SELF,     /* this client has it */
	RADIANT_BLE_FTMS_OWNER_OTHER,    /* another client has it */
};

/* A parsed target op. Units are the wire's. */
struct radiant_ble_ftms_target {
	uint8_t op;
	union {
		uint16_t speed_centi_kph;
		int16_t incline_deci;
		uint8_t resistance_deci;
		int16_t power_w;
		struct {
			int16_t wind_mm_s;   /* 0.001 m/s */
			int16_t grade_centi; /* 0.01 % */
			uint8_t crr_e4;      /* 0.0001 */
			uint8_t cw_centi;    /* 0.01 kg/m */
		} sim;
		uint16_t wheel_deci_mm;
	} v;
};

/* What a motion check is asked about: every request that could actuate the
 * machine - a target op, Start/Resume, or Reset's non-stop half (op 0x01). */
struct radiant_ble_ftms_motion {
	uint8_t op;
	bool resume;  /* 0x07 from PAUSED */
	bool stale;   /* received under an epoch that has since moved */
	struct radiant_ble_ftms_target target; /* target ops only */
};

enum radiant_ble_ftms_verdict {
	RADIANT_BLE_FTMS_PERMIT = 0,
	RADIANT_BLE_FTMS_DENY,  /* answered 0x05 Control Not Permitted */
	RADIANT_BLE_FTMS_STALE, /* answered 0x04 Operation Failed */
};

/*
 * The machine's motion authorisation. NULL `check` means: a stale request is
 * STALE, everything else PERMIT - right for a passive machine (an indoor bike
 * cannot start anything by itself). A treadmill routes this to its one motion
 * rule; see apps/treadmill/src/treadmill_session.h.
 */
struct radiant_ble_ftms_auth {
	enum radiant_ble_ftms_verdict (*check)(const struct radiant_ble_ftms_motion *m,
					       const struct radiant_ble_ftms_session *s,
					       void *ctx);
	void *ctx;
};

struct radiant_ble_ftms_cp_request {
	const uint8_t *data; /* op code + parameter, as written */
	uint16_t len;
	uint32_t epoch;      /* the machine's epoch when the write was received */
};

enum radiant_ble_ftms_action {
	RADIANT_BLE_FTMS_ACT_NONE = 0, /* answer `result`, do nothing */
	RADIANT_BLE_FTMS_ACT_GRANT,    /* Request Control granted */
	RADIANT_BLE_FTMS_ACT_RESET,
	RADIANT_BLE_FTMS_ACT_SET_TARGET,
	RADIANT_BLE_FTMS_ACT_START,
	RADIANT_BLE_FTMS_ACT_RESUME,
	RADIANT_BLE_FTMS_ACT_STOP,
	RADIANT_BLE_FTMS_ACT_PAUSE,
};

struct radiant_ble_ftms_cp_decision {
	uint8_t op;
	/* The answer when action is NONE; otherwise execute decides it. */
	uint8_t result;
	enum radiant_ble_ftms_action action;
	struct radiant_ble_ftms_target target; /* SET_TARGET */
	/* RESET: may the non-stop half (e.g. a treadmill's incline to 0) run?
	 * The stop half always runs. */
	bool reset_motion_ok;
	/* The machine's epoch when the request was received, handed to every
	 * hook so its own re-check compares against receipt, not "now". */
	uint32_t epoch;
};

/*
 * THE DECISION. Pure: no kernel, no stack, no side effect beyond calling
 * auth->check. In order:
 *
 *   1. Request Control: 0x05 if another client holds control, else GRANT.
 *   2. Anything else without control (owner != SELF): 0x05 (§4.16.2).
 *   3. Op codes this machine does not honour (a target op whose TSF bit is
 *      clear, or one not in Table 4.15's set handled here): 0x02.
 *   4. Wrong parameter length, a Stop/Pause parameter other than 1 or 2, a
 *      target outside caps' valid range: 0x03.
 *   5. Start while RUNNING: 0x04, or a 0x01 no-op if caps->passive.
 *   6. Motion check for target ops and Start/Resume: DENY 0x05, STALE 0x04.
 *      Reset asks for its non-stop half and records the verdict.
 *   7. Otherwise the action, for execute.
 */
struct radiant_ble_ftms_cp_decision
radiant_ble_ftms_cp_decide(const struct radiant_ble_ftms_cp_request *req,
			   const struct radiant_ble_ftms_session *session,
			   const struct radiant_ble_ftms_caps *caps,
			   enum radiant_ble_ftms_owner owner,
			   const struct radiant_ble_ftms_auth *auth);

/* ── Running a decision against the machine ─────────────────────────────── */

enum radiant_ble_ftms_workout {
	RADIANT_BLE_FTMS_WORKOUT_START = 0,
	RADIANT_BLE_FTMS_WORKOUT_RESUME,
	RADIANT_BLE_FTMS_WORKOUT_PAUSE,
	RADIANT_BLE_FTMS_WORKOUT_STOP,
};

/*
 * The machine's actuators. Each answers 0 (accepted), -ENOTSUP (0x02 for a
 * target op; 0x04 for the mandatory workout ops), -EINVAL/-ERANGE (0x03) or any
 * other negative errno (0x04). A NULL hook is -ENOTSUP. `epoch` is the
 * machine's epoch at receipt of the request.
 *
 *   set_target  a target op the decision has range-checked and authorised.
 *               The hook MAY rewrite *t to the value it actually applied
 *               (e.g. a clamped simulation grade): the status notification
 *               that follows a success carries *t as the hook left it, so
 *               subscribers hear what the machine did, not what was asked.
 *   workout     Start/Resume (authorised) and Stop/Pause (never gated).
 *   reset       §4.16.2 Reset. `motion_ok` false: perform only the stop half
 *               (e.g. a treadmill's speed 0) - the core answers 0x04 whatever
 *               the hook returns. Return 0 only if everything it attempted
 *               was accepted.
 */
struct radiant_ble_ftms_ops {
	int (*set_target)(struct radiant_ble_ftms_target *t, uint32_t epoch, void *ctx);
	int (*workout)(enum radiant_ble_ftms_workout w, uint32_t epoch, void *ctx);
	int (*reset)(bool motion_ok, uint32_t epoch, void *ctx);
	void *ctx;
};

struct radiant_ble_ftms_cp_outcome {
	uint8_t op;
	uint8_t result;
	bool grant;      /* make this client the controller, claim the token */
	bool release;    /* revoke control and release the token (Reset) */
	bool reset_view; /* zero the FTMS view's accumulators (Reset) */
	uint8_t status[RADIANT_BLE_FTMS_STATUS_MAX]; /* to notify, if status_len */
	uint8_t status_len;
};

struct radiant_ble_ftms_cp_outcome
radiant_ble_ftms_cp_execute(const struct radiant_ble_ftms_cp_decision *d,
			    const struct radiant_ble_ftms_ops *ops);

/* An actuator's answer as a result code (see radiant_ble_ftms_ops). */
uint8_t radiant_ble_ftms_result_for(int rc);

/* ── Builders ───────────────────────────────────────────────────────────── */

/* Training Status for a run state: IDLE and SAFETY_STOP Idle, RUNNING and
 * PAUSED Manual Mode (Quick Start), STOPPED Post-Workout. */
uint8_t radiant_ble_ftms_training_status(enum radiant_ble_ftms_run run);

/* The machine-status notification a session transition owes, or 0:
 * -> RUNNING 0x04; RUNNING -> PAUSED 0x02/0x02; RUNNING|PAUSED -> STOPPED|IDLE
 * 0x02/0x01; -> SAFETY_STOP 0x03. out must hold 2 bytes. */
size_t radiant_ble_ftms_session_status(enum radiant_ble_ftms_run prev,
				       enum radiant_ble_ftms_run next, uint8_t *out);

/* Fitness Machine Feature: FMF then TSF, little-endian. out holds 8. */
size_t radiant_ble_ftms_feature_encode(const struct radiant_ble_ftms_caps *caps,
				       uint8_t *out);

/* A Supported Range characteristic: min, max (16-bit two's complement), step.
 * out holds 6. */
size_t radiant_ble_ftms_range_encode(const struct radiant_ble_ftms_range *r,
				     uint8_t *out);

/* The advertising Service Data element's payload: 0x1826, flags, machine type.
 * Returns RADIANT_BLE_FTMS_SVC_DATA_LEN, or 0 if cap is too small. */
size_t radiant_ble_ftms_service_data(uint8_t *out, size_t cap, bool available,
				     uint16_t machine_type);

/* ── GATT glue (radiant_ble_ftms.c) ─────────────────────────────────────── */

struct radiant_ble_ftms_machine {
	struct radiant_ble_ftms_caps caps;
	uint16_t machine_type; /* RADIANT_BLE_FTMS_TYPE_* for Service Data */
	struct radiant_ble_ftms_ops ops;
	struct radiant_ble_ftms_auth auth;
	/* Encode the data record (Treadmill Data, Indoor Bike Data) for one
	 * notification of at most `cap` bytes. Length or negative errno. */
	int (*encode_data)(uint8_t *buf, size_t cap, void *ctx);
	/* The machine's session now. System workqueue. */
	void (*session)(struct radiant_ble_ftms_session *out, void *ctx);
	/* The session epoch now. CALLED FROM THE BT RX THREAD at receipt of
	 * every Control Point write: must be safe from any thread. */
	uint32_t (*epoch)(void *ctx);
	/* A Reset succeeded: zero what the FTMS data record reports as
	 * accumulated (elapsed time, distance...) and return the machine's
	 * session to idle. The core sets Training Status to Idle itself.
	 * System workqueue. */
	void (*reset_done)(void *ctx);
	void *ctx;
};

/*
 * Register the machine. Once, before advertising starts; -EALREADY after the
 * first. The struct must outlive the program.
 */
int radiant_ble_ftms_register(const struct radiant_ble_ftms_machine *m);

/* Notify the data record to every subscribed client, each at its own ATT MTU.
 * System workqueue; the caller paces it (FTMS: about once a second). */
void radiant_ble_ftms_notify_data(void);

/* The machine's session moved: update and notify Training Status, and notify
 * the machine status the transition owes, to every subscriber. System
 * workqueue. */
void radiant_ble_ftms_session_changed(enum radiant_ble_ftms_run prev,
				      enum radiant_ble_ftms_run next);

/* Take control from the FTMS controller, if any: 0xFF Control Permission Lost
 * to that client, and the arbiter token released. System workqueue. */
void radiant_ble_ftms_revoke_control(void);

/* Notify a Fitness Machine Status to every subscriber - for a change that did
 * not come through the Control Point (e.g. 0x06 Target Incline Changed when an
 * FE-C controller moved the target). System workqueue. */
void radiant_ble_ftms_notify_status(const uint8_t *msg, size_t len);

/* True while some client holds FTMS control. */
bool radiant_ble_ftms_has_controller(void);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_BLE_FTMS_H_ */
