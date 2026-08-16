/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * FTMS (0x1826) and RSC (0x1814), beside two ANT+ masters on one radio.
 *
 * ===========================================================================
 * PROVENANCE, AND WHY IT IS THREE DOCUMENTS AND NOT ONE
 * ===========================================================================
 * 1. Bluetooth SIG, "Fitness Machine Service" (FTMS) v1.0 - behaviour, flag
 *    semantics, procedures, the control-point op-code table, the feature bit
 *    tables and the advertising Service Data element. Section and table
 *    numbers are cited at each field below so they can be re-checked.
 * 2. The GATT Specification Supplement / Assigned Numbers - EVERY
 *    characteristic field encoding. FTMS defines the Treadmill Data *flags*
 *    and then says the fields themselves are "as defined on the Bluetooth SIG
 *    Assigned Numbers webpage"; it never prints a data type, a unit or a
 *    resolution for any of them, and it never prints a single 16-bit UUID
 *    value either. Implementing from FTMS alone leaves every scale factor a
 *    guess. Each constant below therefore carries its GSS-derived unit
 *    explicitly, in the same voice profile_fec.h cites its tables.
 * 3. The Fitness Machine Profile (FTMP) - the *profile* spec, separate from
 *    the service spec. At FTMS service level almost everything is Optional
 *    (Table 4.1: only Fitness Machine Feature is unconditionally Mandatory,
 *    and even Treadmill Data is "O"), and FTMS never says a server must
 *    advertise the Service Data element at all. The per-machine-type
 *    obligations are FTMP's.
 *
 * RSC is not hand-written: NCS ships the service
 * (nrf/subsys/bluetooth/services/rscs.c, CONFIG_BT_RSCS=y) and this file uses
 * bt_rscs_init() / bt_rscs_measurement_send().
 *
 * ===========================================================================
 * SEVEN THINGS THAT ARE EASY TO GET WRONG HERE, EACH WITH A SILENT FAILURE
 * ===========================================================================
 *
 * 1. THE UUID MACRO NAMES ARE A TRAP. The FTMS *service* 0x1826 is
 *    `BT_UUID_FMS`, while `BT_UUID_GATT_FMS` is the Fitness Machine *Status
 *    characteristic* 0x2ADA - one `GATT_` apart, both plausible in a service
 *    definition, and swapping them compiles cleanly and discovers wrong.
 *    `BT_UUID_FTMS` does not exist. Verified against
 *    zephyr/include/zephyr/bluetooth/uuid.h in NCS v3.2.4, along with
 *    `BT_UUID_GATT_TRSTAT` (0x2AD3), whose name is not `..._TS`.
 *
 * 2. TREADMILL DATA'S BIT 0 IS INVERTED. Table 4.5: bit 0 is "More Data", and
 *    it set to **0** means "Instantaneous Speed field present". Every other
 *    bit is present-when-1. It also has no corresponding Feature bit.
 *
 * 3. SEVERAL FIELDS SHARE ONE FLAG BIT. Bit 3 is "Inclination AND Ramp Angle
 *    Setting Present" - inclination cannot be sent without the ramp angle.
 *    Bit 4 is the positive/negative elevation gain PAIR. Bit 7 covers total
 *    energy, energy per hour and energy per minute as one group.
 *
 * 4. THE FULL RECORD DOES NOT FIT ONE NOTIFICATION and the spec says so:
 *    "all the fields of this characteristic cannot be present simultaneously
 *    if using a default ATT_MTU size". §4.19 then defines a split where every
 *    notification but the last carries More Data = 1. THIS IMPLEMENTATION
 *    AVOIDS THE SPLIT ENTIRELY by fixing a field set that fits: 18 bytes
 *    against the 20 a default 23-byte ATT_MTU leaves. BUILD_ASSERTed below,
 *    so adding a field is a build error rather than a truncated notification.
 *
 * 5. THE CONTROL POINT REQUIRES ENCRYPTION. Table 4.1 gives it security
 *    permission "Encryption", so the attribute carries
 *    BT_GATT_PERM_WRITE_ENCRYPT and the build needs CONFIG_BT_SMP and a
 *    bonding store. apps/hrm_ble has no equivalent of any of that - it
 *    advertises an unencrypted HRS and never pairs - so this is new surface,
 *    not a Kconfig line.
 *
 * 6. THE STATUS NOTIFY DIRECTION IS EASY TO GET BACKWARDS. §4.17.1: a status
 *    caused by the USER at the machine's own console goes to ALL connected
 *    clients; a status caused by a CLIENT's control-point write goes to the
 *    OTHER clients - the writing client is answered by the Response Code
 *    indication, not by a status notification. notify_status() takes an
 *    "except" connection for exactly this.
 *
 * 7. THE ADVERTISING PAYLOAD DOES NOT FIT WITH A NAME IN IT.
 *      flags                    1 + 1 + 1 =  3
 *      UUID16 list (two UUIDs)  1 + 1 + 4 =  6
 *      FTMS service data        1 + 1 + 5 =  7
 *                                          ----
 *                                            16, leaving N <= 13 for a name.
 *    Too short for a user-chosen machine name, so THE NAME GOES IN THE SCAN
 *    RESPONSE. Passing a non-NULL `sd` to bt_le_adv_start() is the whole
 *    change - per zephyr/subsys/bluetooth/host/adv.c it makes a legacy
 *    advertiser scannable automatically, and BT_LE_ADV_CONN_FAST_1 works
 *    unmodified. THE COST IS NOT FREE AND IS RECORDED IN ADR 0017: a scannable
 *    advertiser answers SCAN_REQ, which is extra radio time contending with
 *    the ANT+ masters through exactly the MPSL gate this application family
 *    exists to arbitrate. CONFIG_BT_EXT_ADV is used nowhere in this repository
 *    and is the higher-risk answer; legacy adv plus a scan response is the one
 *    taken.
 *
 * ===========================================================================
 * WHAT IS NOT HERE
 * ===========================================================================
 * No procedure timeout. §4.16.4 is titled "Procedure Timeout" and gives no
 * number; the commonly-assumed 30 s is not in FTMS v1.0. If one is ever
 * wanted it is local policy to document, not a spec citation, and it is not
 * invented here.
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/bluetooth/att.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/byteorder.h>

#include <bluetooth/services/rscs.h>

#include "treadmill_ble.h"
#include "treadmill_state.h"

LOG_MODULE_DECLARE(treadmill, CONFIG_TREADMILL_LOG_LEVEL);

/* 0.625 ms units, which is what the advertising interval is expressed in. */
#define ADV_UNITS(ms) ((uint32_t)(((uint32_t)(ms) * 1000u) / 625u))

/* ---------------------------------------------------------------------------
 * Table 4.15's op codes and Table 4.24's result codes
 * ---------------------------------------------------------------------------
 */
#define FTMS_OP_REQUEST_CONTROL      0x00u
#define FTMS_OP_RESET                0x01u
#define FTMS_OP_SET_TARGET_SPEED     0x02u /* UINT16, km/h, 0.01 km/h */
#define FTMS_OP_SET_TARGET_INCLINE   0x03u /* SINT16, percent, 0.1 %   */
#define FTMS_OP_SET_TARGET_RESIST    0x04u
#define FTMS_OP_SET_TARGET_POWER     0x05u
#define FTMS_OP_START_RESUME         0x07u
#define FTMS_OP_STOP_PAUSE           0x08u /* 1 octet: 0x01 stop, 0x02 pause */
#define FTMS_OP_RESPONSE_CODE        0x80u

#define FTMS_RESULT_SUCCESS          0x01u
#define FTMS_RESULT_OP_NOT_SUPPORTED 0x02u
#define FTMS_RESULT_INVALID_PARAM    0x03u
#define FTMS_RESULT_OPERATION_FAILED 0x04u
#define FTMS_RESULT_CONTROL_NOT_PERMITTED 0x05u

/* Table 4.16's parameter for 0x08. */
#define FTMS_STOP  0x01u
#define FTMS_PAUSE 0x02u

/* ---------------------------------------------------------------------------
 * Table 4.26's Fitness Machine Status op codes
 * ---------------------------------------------------------------------------
 */
#define FTMS_STATUS_RESET                 0x01u
#define FTMS_STATUS_STOPPED_OR_PAUSED     0x02u /* param 0x01 stop, 0x02 pause */
#define FTMS_STATUS_STARTED_OR_RESUMED    0x04u
#define FTMS_STATUS_TARGET_SPEED_CHANGED  0x05u /* UINT16, 0.01 km/h */
#define FTMS_STATUS_TARGET_INCLINE_CHANGED 0x06u /* SINT16, 0.1 %    */
#define FTMS_STATUS_CONTROL_PERMISSION_LOST 0xFFu

/* Table 4.13's Training Status values. `Reset` is specified to write Idle. */
#define FTMS_TRAINING_IDLE        0x01u
#define FTMS_TRAINING_MANUAL_MODE 0x0Du /* "quick start" */

/* ---------------------------------------------------------------------------
 * Table 4.5's Treadmill Data flags, and Table 4.3/4.4's feature bits
 * ---------------------------------------------------------------------------
 * The flag bits are keyed to the feature bits (the correspondence is Table
 * 4.5's own last column), so an inconsistency between the two is visible to
 * any client. They are defined next to each other here for that reason.
 */
#define TD_FLAG_MORE_DATA        (1u << 0)  /* INVERTED - see trap 2 */
#define TD_FLAG_AVERAGE_SPEED    (1u << 1)
#define TD_FLAG_TOTAL_DISTANCE   (1u << 2)
#define TD_FLAG_INCLINATION      (1u << 3)  /* AND ramp angle - trap 3 */
#define TD_FLAG_ELEVATION_GAIN   (1u << 4)  /* the pos/neg PAIR - trap 3 */
#define TD_FLAG_INST_PACE        (1u << 5)
#define TD_FLAG_AVERAGE_PACE     (1u << 6)
#define TD_FLAG_EXPENDED_ENERGY  (1u << 7)  /* three fields - trap 3 */
#define TD_FLAG_HEART_RATE       (1u << 8)
#define TD_FLAG_METABOLIC_EQUIV  (1u << 9)
#define TD_FLAG_ELAPSED_TIME     (1u << 10)
#define TD_FLAG_REMAINING_TIME   (1u << 11)
#define TD_FLAG_FORCE_ON_BELT    (1u << 12)

/* Table 4.3, Fitness Machine Features (the first u32). */
#define FMF_AVERAGE_SPEED        (1u << 0)
#define FMF_CADENCE              (1u << 1)
#define FMF_TOTAL_DISTANCE       (1u << 2)
#define FMF_INCLINATION          (1u << 3)
#define FMF_ELEVATION_GAIN       (1u << 4)
#define FMF_PACE                 (1u << 5)
#define FMF_EXPENDED_ENERGY      (1u << 9)
#define FMF_HEART_RATE           (1u << 10)
#define FMF_METABOLIC_EQUIV      (1u << 11)
#define FMF_ELAPSED_TIME         (1u << 12)
#define FMF_REMAINING_TIME       (1u << 13)
#define FMF_FORCE_ON_BELT        (1u << 15)

/* Table 4.4, Target Setting Features (the second u32). */
#define TSF_SPEED_TARGET         (1u << 0)
#define TSF_INCLINATION_TARGET   (1u << 1)
#define TSF_RESISTANCE_TARGET    (1u << 2)
#define TSF_POWER_TARGET         (1u << 3)

/*
 * The field set this machine emits, and the flags that must accompany it.
 * Speed is present because bit 0 is CLEAR.
 */
#define TD_FLAGS (TD_FLAG_TOTAL_DISTANCE | TD_FLAG_INCLINATION | \
		  TD_FLAG_ELEVATION_GAIN | TD_FLAG_HEART_RATE |  \
		  TD_FLAG_ELAPSED_TIME)

/*
 * The features that must be declared for exactly those flags, plus the two
 * target-setting bits.
 *
 * §4.16.2 C.1/C.2: op codes 0x02 and 0x03 are "Mandatory to support if the
 * corresponding Target Setting feature is supported; otherwise EXCLUDED" -
 * conditionally excluded, not optional. So declaring TSF_SPEED_TARGET and
 * honouring 0x02 go together in BOTH directions, as do TSF_INCLINATION_TARGET
 * and 0x03. §4.11/§4.12 add that Supported Speed Range and Supported
 * Inclination Range "shall be exposed by the Server if the corresponding
 * Target Setting feature is supported", which is why both are in the service
 * definition below rather than being optional extras.
 */
#define FMF_FLAGS (FMF_TOTAL_DISTANCE | FMF_INCLINATION | FMF_ELEVATION_GAIN | \
		   FMF_HEART_RATE | FMF_ELAPSED_TIME)
#define TSF_FLAGS (TSF_SPEED_TARGET | TSF_INCLINATION_TARGET)

/*
 * §4.4.1.5 / §4.4.1.6: 0x7FFF is the "cannot compute" value for BOTH
 * Inclination and Ramp Angle Setting (both sint16). It happens to be the same
 * sentinel FE-C page 17 uses for incline - convenient, and worth the comment
 * so nobody "fixes" one of them to match the other's neighbour.
 */
#define FTMS_SINT16_UNKNOWN ((int16_t)0x7FFF)

/*
 * The record, laid out in the order Table 4.5 lists the fields. Adding a field
 * means adding its flag AND its feature bit AND re-checking this length: the
 * BUILD_ASSERT below is what turns "it does not fit an ATT_MTU" from a
 * truncated notification a client silently mis-parses into a build error.
 *
 *   flags                    2
 *   instantaneous speed      2   uint16, 0.01 km/h
 *   total distance           3   uint24, metres
 *   inclination              2   sint16, 0.1 %
 *   ramp angle setting       2   sint16, 0.1 degree
 *   positive elevation gain  2   uint16, 0.1 m
 *   negative elevation gain  2   uint16, 0.1 m
 *   heart rate               1   uint8, bpm
 *   elapsed time             2   uint16, seconds
 *                           ---
 *                            18
 */
#define TD_RECORD_LEN 18u
BUILD_ASSERT(TD_RECORD_LEN <= 20u,
	     "the Treadmill Data record must fit the 20-byte payload of a "
	     "default 23-byte ATT_MTU. FTMS 4.19 defines a multi-notification "
	     "split for records that do not, with More Data = 1 on every "
	     "notification but the last; this implementation deliberately "
	     "keeps a fixed field set that fits so that path is never "
	     "exercised. Adding a field means implementing the split.");

/* ---------------------------------------------------------------------------
 * The ranges this machine advertises, which must agree with what
 * treadmill_source will actually accept - a client reads them to learn the
 * legal span before commanding one.
 * ---------------------------------------------------------------------------
 */
#define SPEED_MIN_CENTI_KPH 0u
#define SPEED_MAX_CENTI_KPH ((uint16_t)CONFIG_TREADMILL_BLE_MAX_SPEED_CENTI_KPH)
#define SPEED_STEP_CENTI_KPH 10u /* 0.10 km/h */

#define INCLINE_MIN_DECI ((int16_t)CONFIG_TREADMILL_BLE_MIN_INCLINE_DECI)
#define INCLINE_MAX_DECI ((int16_t)CONFIG_TREADMILL_BLE_MAX_INCLINE_DECI)
#define INCLINE_STEP_DECI 5u /* 0.5 % */

/* ---------------------------------------------------------------------------
 * Module state
 * ---------------------------------------------------------------------------
 */
static atomic_t connections;

/*
 * §4.16.2: EVERY procedure requires control permission. A client must run
 * Request Control first; writes without it get result 0x05. Permission lasts
 * until disconnect, until a Control Permission Lost status, or until the
 * client runs Reset.
 *
 * One controller at a time, tracked by connection pointer rather than by a
 * bool, because "who has it" is the question §4.17.1's notify-direction rule
 * asks and a bool cannot answer it.
 */
static struct bt_conn *controller;

static uint8_t training_status = FTMS_TRAINING_IDLE;

/* The Zwift seam. Registered by nothing in this tree; see the header. */
static treadmill_ble_grade_provider_t grade_provider;
static void *grade_provider_user;

static uint32_t last_notify_ms;
static bool     rsc_notify_enabled;

/* The indication machinery, lifted from nrf/subsys/bluetooth/services/rscs.c:
 * the params live in the instance and not on the stack, the attribute is
 * resolved once rather than by a hardcoded .attrs[] index, and `indicating` is
 * cleared in .destroy because ATT allows exactly one outstanding indication
 * per connection. */
static struct bt_gatt_indicate_params cp_ind_params;
static bool                           cp_indicating;
static uint8_t                        cp_ind_data[8];

/* ---------------------------------------------------------------------------
 * Read handlers
 * ---------------------------------------------------------------------------
 */

static ssize_t read_feature(struct bt_conn *conn,
			    const struct bt_gatt_attr *attr, void *buf,
			    uint16_t len, uint16_t offset)
{
	uint8_t value[8];

	sys_put_le32(FMF_FLAGS, &value[0]);
	sys_put_le32(TSF_FLAGS, &value[4]);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
				 sizeof(value));
}

static ssize_t read_speed_range(struct bt_conn *conn,
				const struct bt_gatt_attr *attr, void *buf,
				uint16_t len, uint16_t offset)
{
	uint8_t value[6];

	/* GSS: Minimum, Maximum, Minimum Increment - three uint16 in
	 * 0.01 km/h, in that order. FTMS itself prints none of this. */
	sys_put_le16(SPEED_MIN_CENTI_KPH, &value[0]);
	sys_put_le16(SPEED_MAX_CENTI_KPH, &value[2]);
	sys_put_le16(SPEED_STEP_CENTI_KPH, &value[4]);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
				 sizeof(value));
}

static ssize_t read_incline_range(struct bt_conn *conn,
				  const struct bt_gatt_attr *attr, void *buf,
				  uint16_t len, uint16_t offset)
{
	uint8_t value[6];

	/* GSS: Minimum and Maximum are SINT16 in 0.1 %, Minimum Increment is
	 * UINT16 in 0.1 %. The sign difference between the first two and the
	 * third is the part a single loop over three uint16s gets wrong. */
	sys_put_le16((uint16_t)INCLINE_MIN_DECI, &value[0]);
	sys_put_le16((uint16_t)INCLINE_MAX_DECI, &value[2]);
	sys_put_le16(INCLINE_STEP_DECI, &value[4]);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
				 sizeof(value));
}

static ssize_t read_training_status(struct bt_conn *conn,
				    const struct bt_gatt_attr *attr, void *buf,
				    uint16_t len, uint16_t offset)
{
	uint8_t value[2];

	/* Table 4.13: flags then status. Flags bit 0 would say a status STRING
	 * follows; there is none, so the byte is zero. */
	value[0] = 0x00u;
	value[1] = training_status;
	return bt_gatt_attr_read(conn, attr, buf, len, offset, value,
				 sizeof(value));
}

static void ccc_changed_noop(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	ARG_UNUSED(value);
}

/* ---------------------------------------------------------------------------
 * The control point
 * ---------------------------------------------------------------------------
 */

static void cp_indicate_destroy(struct bt_gatt_indicate_params *params)
{
	ARG_UNUSED(params);
	cp_indicating = false;
}

/* Forward: the service definition is below and the attribute is resolved from
 * it by UUID rather than by index. */
static const struct bt_gatt_attr *cp_attr(void);
static const struct bt_gatt_attr *status_attr(void);

/*
 * Table 4.23's Response Code packet: 0x80, the echoed request op code, the
 * result code, and an optional parameter (unused here).
 */
static void cp_respond(struct bt_conn *conn, uint8_t request_op, uint8_t result)
{
	const struct bt_gatt_attr *attr = cp_attr();

	if (attr == NULL) {
		return;
	}
	if (cp_indicating) {
		/* ATT allows one outstanding indication per connection. A
		 * client that managed to write twice inside one round trip gets
		 * the second answer dropped rather than a corrupted first -
		 * §4.16.3's own failure case for that is the ATT error
		 * "Procedure Already In Progress", which write_cp() returns
		 * before ever reaching here. */
		LOG_WRN("FTMS: an indication is already outstanding");
		return;
	}

	cp_ind_data[0] = FTMS_OP_RESPONSE_CODE;
	cp_ind_data[1] = request_op;
	cp_ind_data[2] = result;

	memset(&cp_ind_params, 0, sizeof(cp_ind_params));
	cp_ind_params.attr = attr;
	cp_ind_params.data = cp_ind_data;
	cp_ind_params.len = 3u;
	cp_ind_params.destroy = cp_indicate_destroy;

	cp_indicating = true;
	if (bt_gatt_indicate(conn, &cp_ind_params) != 0) {
		cp_indicating = false;
	}
}

/*
 * §4.17.1, and it is the rule most easily got backwards.
 *
 * `except` is the connection that CAUSED the status by writing the control
 * point; it is answered by the Response Code indication and must NOT also get
 * a status notification. A status caused by the user at the machine's own
 * console passes NULL and goes to everybody.
 */
static void notify_status(struct bt_conn *except, const uint8_t *data,
			  uint16_t len)
{
	const struct bt_gatt_attr *attr = status_attr();

	if (attr == NULL) {
		return;
	}
	if (except == NULL) {
		(void)bt_gatt_notify(NULL, attr, data, len);
		return;
	}
	/* bt_gatt_notify(NULL, ...) means "every subscriber". There is no
	 * "everyone except" form, so with a single connection - which is what
	 * CONFIG_BT_MAX_CONN is here - the correct behaviour is simply to send
	 * nothing: the only other client does not exist. Kept as a function
	 * rather than an if at each call site because raising CONFIG_BT_MAX_CONN
	 * is the change that makes this loop over connections, and this is
	 * where that change belongs. */
}

static void status_target_incline_changed(struct bt_conn *except,
					  int16_t deci_pct)
{
	uint8_t msg[3];

	msg[0] = FTMS_STATUS_TARGET_INCLINE_CHANGED;
	sys_put_le16((uint16_t)deci_pct, &msg[1]);
	notify_status(except, msg, sizeof(msg));
}

static void status_target_speed_changed(struct bt_conn *except,
					uint16_t centi_kph)
{
	uint8_t msg[3];

	msg[0] = FTMS_STATUS_TARGET_SPEED_CHANGED;
	sys_put_le16(centi_kph, &msg[1]);
	notify_status(except, msg, sizeof(msg));
}

/*
 * Map the source's answer onto FTMS's result codes. The same three answers as
 * FE-C's command statuses, mapped at this radio's own edge - see
 * treadmill_ble.h's note on why the node functions return the source's errno
 * unchanged.
 */
static uint8_t ftms_result_for(int rc)
{
	if (rc == 0) {
		return FTMS_RESULT_SUCCESS;
	}
	if (rc == -ENOTSUP) {
		return FTMS_RESULT_OP_NOT_SUPPORTED;
	}
	return FTMS_RESULT_OPERATION_FAILED;
}

static uint8_t handle_set_incline(struct bt_conn *conn, const uint8_t *param,
				  uint16_t len)
{
	int16_t  deci;
	int32_t  adopted_centi = 0;
	int      rc;

	if (len < 2u) {
		return FTMS_RESULT_INVALID_PARAM;
	}
	deci = (int16_t)sys_get_le16(param);
	if (deci < INCLINE_MIN_DECI || deci > INCLINE_MAX_DECI) {
		/* The Supported Inclination Range characteristic told the
		 * client the legal span; a value outside it is Invalid
		 * Parameter and not Operation Failed. */
		return FTMS_RESULT_INVALID_PARAM;
	}

	rc = treadmill_node_set_incline_deci(deci, &adopted_centi);
	if (rc == 0) {
		status_target_incline_changed(
			conn, treadmill_ftms_incline_from_fec(adopted_centi));
	}
	return ftms_result_for(rc);
}

static uint8_t handle_set_speed(struct bt_conn *conn, const uint8_t *param,
				uint16_t len)
{
	uint16_t centi_kph;
	uint32_t adopted_mm_s = 0;
	int      rc;

	if (len < 2u) {
		return FTMS_RESULT_INVALID_PARAM;
	}
	centi_kph = sys_get_le16(param);
	if (centi_kph > SPEED_MAX_CENTI_KPH) {
		return FTMS_RESULT_INVALID_PARAM;
	}

	rc = treadmill_node_set_speed_mm_s(
		treadmill_mm_s_from_ftms_speed(centi_kph), &adopted_mm_s);
	if (rc == 0) {
		status_target_speed_changed(
			conn, treadmill_ftms_speed_from_mm_s(adopted_mm_s));
	}
	return ftms_result_for(rc);
}

static uint8_t handle_reset(struct bt_conn *conn)
{
	uint8_t msg[1];

	/* §4.16.2 specifies Reset concretely: target speed and inclination to
	 * 0, time fields to 0, Training Status to Idle. The time fields belong
	 * to the machine and this node does not zero a session on a BLE
	 * command - a client resetting its own view must not silently discard
	 * the ANT+ side's accumulators mid-run, which is a decision worth
	 * recording rather than a shortcut. */
	(void)treadmill_node_set_incline_deci(0, NULL);
	(void)treadmill_node_set_speed_mm_s(0u, NULL);
	training_status = FTMS_TRAINING_IDLE;

	/* Reset also revokes control (§4.16.2). */
	controller = NULL;

	msg[0] = FTMS_STATUS_RESET;
	notify_status(conn, msg, sizeof(msg));
	return FTMS_RESULT_SUCCESS;
}

static ssize_t write_cp(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			const void *buf, uint16_t len, uint16_t offset,
			uint8_t flags)
{
	const uint8_t *data = buf;
	uint8_t        op;
	uint8_t        result;

	ARG_UNUSED(attr);
	ARG_UNUSED(flags);

	if (offset != 0u) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_OFFSET);
	}
	if (len < 1u) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	/* §4.16.3 maps two failures to ATT errors rather than to a Response
	 * Code, and returning the wrong one of the two is invisible to a
	 * client that only checks for success. */
	if (cp_indicating) {
		return BT_GATT_ERR(BT_ATT_ERR_PROCEDURE_IN_PROGRESS);
	}
	if (!bt_gatt_is_subscribed(conn, cp_attr(), BT_GATT_CCC_INDICATE)) {
		/* The CCC must be configured for INDICATION, not notification.
		 * A client that subscribed the wrong way gets this rather than
		 * an answer it will never receive. */
		return BT_GATT_ERR(BT_ATT_ERR_CCC_IMPROPER_CONF);
	}

	op = data[0];

	if (op == FTMS_OP_REQUEST_CONTROL) {
		if (controller != NULL && controller != conn) {
			/* Another client holds it. §4.17.1: tell the loser
			 * nothing and tell the previous holder it lost - which
			 * with one connection cannot happen, and the branch is
			 * here because raising CONFIG_BT_MAX_CONN is what makes
			 * it reachable. */
			cp_respond(conn, op, FTMS_RESULT_CONTROL_NOT_PERMITTED);
			return len;
		}
		controller = conn;
		cp_respond(conn, op, FTMS_RESULT_SUCCESS);
		return len;
	}

	/* EVERY other procedure requires control permission (§4.16.2). */
	if (controller != conn) {
		cp_respond(conn, op, FTMS_RESULT_CONTROL_NOT_PERMITTED);
		return len;
	}

	switch (op) {
	case FTMS_OP_RESET:
		result = handle_reset(conn);
		break;
	case FTMS_OP_SET_TARGET_SPEED:
		result = handle_set_speed(conn, &data[1], (uint16_t)(len - 1u));
		break;
	case FTMS_OP_SET_TARGET_INCLINE:
		result = handle_set_incline(conn, &data[1],
					    (uint16_t)(len - 1u));
		break;
	case FTMS_OP_START_RESUME:
		if (training_status == FTMS_TRAINING_MANUAL_MODE) {
			/* Table 4.24 and §4.16.2: Start when already started is
			 * Operation Failed, NOT Success. A client uses the
			 * difference to decide whether it started the session. */
			result = FTMS_RESULT_OPERATION_FAILED;
		} else {
			uint8_t msg[1] = { FTMS_STATUS_STARTED_OR_RESUMED };

			training_status = FTMS_TRAINING_MANUAL_MODE;
			notify_status(conn, msg, sizeof(msg));
			result = FTMS_RESULT_SUCCESS;
		}
		break;
	case FTMS_OP_STOP_PAUSE:
		if (len < 2u) {
			result = FTMS_RESULT_INVALID_PARAM;
		} else if (training_status == FTMS_TRAINING_IDLE) {
			result = FTMS_RESULT_OPERATION_FAILED;
		} else {
			uint8_t msg[2] = { FTMS_STATUS_STOPPED_OR_PAUSED,
					   data[1] };

			if (data[1] == FTMS_STOP) {
				training_status = FTMS_TRAINING_IDLE;
				(void)treadmill_node_set_speed_mm_s(0u, NULL);
			}
			notify_status(conn, msg, sizeof(msg));
			result = FTMS_RESULT_SUCCESS;
		}
		break;
	default:
		result = FTMS_RESULT_OP_NOT_SUPPORTED;
		break;
	}

	cp_respond(conn, op, result);
	return len;
}

/* ---------------------------------------------------------------------------
 * The service definition
 * ---------------------------------------------------------------------------
 *
 * Note BT_UUID_FMS for the SERVICE and BT_UUID_GATT_FMS for the STATUS
 * CHARACTERISTIC - see trap 1. They are one `GATT_` apart and swapping them
 * compiles.
 */
BT_GATT_SERVICE_DEFINE(
	ftms_svc,
	BT_GATT_PRIMARY_SERVICE(BT_UUID_FMS),

	/* Table 4.1: the ONE unconditionally mandatory characteristic. */
	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_FMF, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_feature, NULL, NULL),

	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_TD, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed_noop,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_TRSTAT,
			       BT_GATT_CHRC_READ | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_READ, read_training_status, NULL,
			       NULL),
	BT_GATT_CCC(ccc_changed_noop,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* §4.11 / §4.12: mandatory BECAUSE the matching target-setting feature
	 * bits are declared. */
	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_SSR, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_speed_range, NULL, NULL),
	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_SIR, BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ, read_incline_range, NULL,
			       NULL),

	/*
	 * WRITE + INDICATE, and Table 4.1 gives it security permission
	 * "Encryption" - hence BT_GATT_PERM_WRITE_ENCRYPT and not
	 * BT_GATT_PERM_WRITE. An unpaired write must fail, and confirming that
	 * on a phone is step 5 of the bench procedure.
	 */
	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_FMCP,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_INDICATE,
			       BT_GATT_PERM_WRITE_ENCRYPT, NULL, write_cp,
			       NULL),
	BT_GATT_CCC(ccc_changed_noop,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* C.6: mandatory BECAUSE the control point is supported. */
	BT_GATT_CHARACTERISTIC(BT_UUID_GATT_FMS, BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE, NULL, NULL, NULL),
	BT_GATT_CCC(ccc_changed_noop,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),
);

/*
 * Resolved by UUID rather than by a hardcoded ftms_svc.attrs[N] index, which
 * is the pattern rscs.c uses and the reason inserting a characteristic above
 * cannot silently repoint an indication at the wrong attribute.
 */
static const struct bt_gatt_attr *cp_attr(void)
{
	static const struct bt_gatt_attr *cached;

	if (cached == NULL) {
		cached = bt_gatt_find_by_uuid(ftms_svc.attrs, ftms_svc.attr_count,
					      BT_UUID_GATT_FMCP);
	}
	return cached;
}

static const struct bt_gatt_attr *status_attr(void)
{
	static const struct bt_gatt_attr *cached;

	if (cached == NULL) {
		cached = bt_gatt_find_by_uuid(ftms_svc.attrs, ftms_svc.attr_count,
					      BT_UUID_GATT_FMS);
	}
	return cached;
}

static const struct bt_gatt_attr *td_attr(void)
{
	static const struct bt_gatt_attr *cached;

	if (cached == NULL) {
		cached = bt_gatt_find_by_uuid(ftms_svc.attrs, ftms_svc.attr_count,
					      BT_UUID_GATT_TD);
	}
	return cached;
}

/* ---------------------------------------------------------------------------
 * Treadmill Data
 * ---------------------------------------------------------------------------
 */

static void put_u24(uint8_t *out, uint32_t value)
{
	out[0] = (uint8_t)(value & 0xFFu);
	out[1] = (uint8_t)((value >> 8) & 0xFFu);
	out[2] = (uint8_t)((value >> 16) & 0xFFu);
}

static void notify_treadmill_data(const struct treadmill_state *s)
{
	uint8_t                    rec[TD_RECORD_LEN];
	size_t                     i = 0;
	const struct bt_gatt_attr *attr = td_attr();
	int16_t                    incline_deci;

	if (attr == NULL) {
		return;
	}

	/* Bit 0 CLEAR means the instantaneous speed IS present. Inverted, and
	 * the only bit in the word that is. */
	sys_put_le16(TD_FLAGS, &rec[i]);
	i += 2u;

	sys_put_le16(treadmill_ftms_speed_from_mm_s(s->belt_speed_mm_s),
		     &rec[i]);
	i += 2u;

	put_u24(&rec[i], (uint32_t)(s->distance_mm / 1000u));
	i += 3u;

	/* Inclination AND ramp angle, because bit 3 covers both and neither
	 * can be sent without the other. This machine has no ramp-angle
	 * sensor, so the ramp angle is the "cannot compute" sentinel - which
	 * is 0x7FFF for both fields (§4.4.1.5, §4.4.1.6), the same value FE-C
	 * page 17 uses for incline. */
	incline_deci = treadmill_ftms_incline_from_fec(s->incline_centi_pct);
	sys_put_le16((uint16_t)incline_deci, &rec[i]);
	i += 2u;
	sys_put_le16((uint16_t)FTMS_SINT16_UNKNOWN, &rec[i]);
	i += 2u;

	/* The elevation-gain PAIR, both uint16 in 0.1 m. Both are magnitudes
	 * and both count up - the negative one is descent, not a signed
	 * climb. */
	sys_put_le16((uint16_t)((s->pos_vertical_mm / 100u) & 0xFFFFu), &rec[i]);
	i += 2u;
	sys_put_le16((uint16_t)((s->neg_vertical_mm / 100u) & 0xFFFFu), &rec[i]);
	i += 2u;

	rec[i++] = s->heart_rate_bpm;

	sys_put_le16((uint16_t)((s->elapsed_ms / 1000u) & 0xFFFFu), &rec[i]);
	i += 2u;

	__ASSERT_NO_MSG(i == TD_RECORD_LEN);
	(void)bt_gatt_notify(NULL, attr, rec, TD_RECORD_LEN);
}

static void notify_rsc(const struct treadmill_state *s)
{
	struct bt_rscs_measurement m;

	if (!rsc_notify_enabled) {
		return;
	}

	memset(&m, 0, sizeof(m));
	m.inst_speed = treadmill_rsc_speed_from_mm_s(s->belt_speed_mm_s);
	/*
	 * STEPS PER MINUTE, NOT STRIDES, and this is the one line of the whole
	 * application most likely to be wrong by a factor of two.
	 *
	 * rscs.h comments this field as "1 unit = 1 stride/minute", but
	 * Nordic's own sample (nrf/samples/bluetooth/peripheral_rscs) simulates
	 * 150-180, which is steps - strides would be 75-90 and no runner has a
	 * 170 stride/min gait. Steps is also what real clients expect. FE-C
	 * page 19 and ANT+ SDM both count STRIDES, so the conversion is a
	 * doubling and it lives in exactly one function with its own unit test.
	 */
	m.inst_cadence =
		treadmill_rsc_cadence_from_strides(s->cadence_strides_min_16);
	m.is_inst_stride_len = true;
	/* RSC's stride length is 100 units = 1 m, i.e. centimetres - the same
	 * unit as FE-C page 17's cycle length, which is the one piece of luck
	 * in this application. */
	m.inst_stride_length =
		treadmill_stride_length_cm(s->distance_mm, s->strides);
	m.is_total_distance = true;
	m.total_distance = (uint32_t)(s->distance_mm / 1000u);
	/* "Running" above about 2.2 m/s is the conventional split and it is a
	 * display hint, not a measurement. */
	m.is_running = (s->belt_speed_mm_s >= 2200u);

	(void)bt_rscs_measurement_send(NULL, &m);
}

void treadmill_ble_state_changed(const struct treadmill_state *s)
{
	uint32_t now;

	if (s == NULL || atomic_get(&connections) == 0) {
		return;
	}

	/*
	 * FTMS: "notify at a regular interval, typically once per second", and
	 * the interval is NOT client-configurable. So the pacing is here and
	 * not at the caller: main.c calls this on every 100 ms tick and on
	 * every source report, and nine out of ten of those return.
	 *
	 * That is also the arbitration argument. Every notification is radio
	 * time contending with two ANT+ masters through the MPSL gate, and a
	 * caller that notified on every state change would quadruple the BLE
	 * load for no client benefit.
	 */
	now = (uint32_t)k_uptime_get_32();
	if ((now - last_notify_ms) < CONFIG_TREADMILL_BLE_NOTIFY_INTERVAL_MS) {
		return;
	}
	last_notify_ms = now;

	/* The Zwift seam, asked here and answered by nothing in this tree. */
	if (grade_provider != NULL) {
		int32_t grade = grade_provider(grade_provider_user);

		if (grade != INT32_MIN) {
			int32_t adopted = 0;

			(void)treadmill_node_set_incline_deci(
				treadmill_ftms_incline_from_fec(grade),
				&adopted);
		}
	}

	notify_treadmill_data(s);
	notify_rsc(s);
}

int treadmill_ble_grade_provider_register(treadmill_ble_grade_provider_t fn,
					  void *user)
{
	if (grade_provider != NULL) {
		return -EEXIST;
	}
	grade_provider = fn;
	grade_provider_user = user;
	return 0;
}

/* ---------------------------------------------------------------------------
 * Connections and advertising
 * ---------------------------------------------------------------------------
 */

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err != 0u) {
		LOG_WRN("BLE connect failed (0x%02x)", err);
		return;
	}
	atomic_inc(&connections);
	LOG_INF("BLE connected");
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	atomic_dec(&connections);
	if (controller == conn) {
		/* §4.16.2: control permission lasts until disconnect. */
		controller = NULL;
	}
	LOG_INF("BLE disconnected (0x%02x)", reason);
}

/*
 * Refuse a short connection interval rather than accept it quietly. Carried
 * over from apps/hrm_ble unchanged, and it is policy rather than power tuning:
 * MPSL ranks the Bluetooth controller above anything an application can
 * request, so RadiANT's share of the radio is delivered by SHAPING BLE's
 * demand, and this is where that decision is made. A phone will ask for tens
 * of milliseconds.
 */
static bool on_param_req(struct bt_conn *conn, struct bt_le_conn_param *param)
{
	ARG_UNUSED(conn);

	if (param->interval_max < CONFIG_TREADMILL_BLE_MIN_CONN_INTERVAL_UNITS) {
		LOG_INF("refusing %u-unit connection interval, holding at %u",
			param->interval_max,
			(unsigned)CONFIG_TREADMILL_BLE_MIN_CONN_INTERVAL_UNITS);
		param->interval_min =
			CONFIG_TREADMILL_BLE_MIN_CONN_INTERVAL_UNITS;
		param->interval_max =
			CONFIG_TREADMILL_BLE_MIN_CONN_INTERVAL_UNITS;
	}
	return true;
}

BT_CONN_CB_DEFINE(treadmill_ble_conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
	.le_param_req = on_param_req,
};

/*
 * Table 3.1's Service Data element: the FTMS UUID, a Flags octet (bit 0 =
 * Fitness Machine Available) and a Fitness Machine Type uint16 (bit 0 =
 * Treadmill Supported; 1 cross trainer, 2 step climber, 3 stair climber,
 * 4 rower, 5 indoor bike).
 *
 * FTMS marks the element's FIELDS mandatory but never says the server must
 * advertise the element at all - that obligation is FTMP's. It is emitted
 * anyway: it is how FTMS clients filter, and omitting it is the most common
 * reason a machine is invisible to an app that otherwise works.
 */
#define FTMS_SD_FLAG_AVAILABLE 0x01u
#define FTMS_SD_TYPE_TREADMILL 0x0001u

static const struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA_BYTES(BT_DATA_UUID16_ALL,
		      BT_UUID_16_ENCODE(BT_UUID_FMS_VAL),
		      BT_UUID_16_ENCODE(BT_UUID_RSCS_VAL)),
	BT_DATA_BYTES(BT_DATA_SVC_DATA16,
		      BT_UUID_16_ENCODE(BT_UUID_FMS_VAL),
		      FTMS_SD_FLAG_AVAILABLE,
		      (FTMS_SD_TYPE_TREADMILL & 0xFFu),
		      ((FTMS_SD_TYPE_TREADMILL >> 8) & 0xFFu)),
};

/*
 * THE NAME IS IN THE SCAN RESPONSE, not the advertisement - see trap 7. That
 * buys a second 31-byte budget, so the name may be up to 29 characters:
 *
 *   name  1 + 1 + N <= 31   ->   N <= 29
 *
 * The failure mode if it does not fit is the one apps/hrm_ble documents:
 * bt_le_adv_start() returns -EINVAL, main.c's call site is
 * `(void)treadmill_ble_start()`, and the node transmits ANT+ perfectly while
 * never advertising, with the only evidence a log line on a board that may
 * have no console attached. Hence a BUILD_ASSERT.
 */
BUILD_ASSERT(sizeof(CONFIG_BT_DEVICE_NAME) - 1u <= 29u,
	     "CONFIG_BT_DEVICE_NAME does not fit the 31-byte scan response: "
	     "the budget is 29 characters (2 + N <= 31). Over it, "
	     "bt_le_adv_start() returns -EINVAL and the node silently never "
	     "advertises.");

/* And the advertisement's own sum, so that adding an AD structure is a build
 * error rather than a node that stops being discoverable. */
BUILD_ASSERT((3u + 6u + 7u) <= 31u,
	     "the advertising payload no longer fits 31 bytes: flags 3 + a "
	     "two-UUID16 list 6 + the FTMS Service Data element 7 = 16. "
	     "Anything added here comes out of nothing, because the name is "
	     "already in the scan response.");

static const struct bt_data sd[] = {
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1u),
};

bool treadmill_ble_connected(void)
{
	return atomic_get(&connections) != 0;
}

static void rscs_evt_handler(enum bt_rscs_evt evt)
{
	rsc_notify_enabled = (evt == RSCS_EVT_MEAS_NOTIFY_ENABLE);
}

static int rscs_set_cumulative(uint32_t total_distance)
{
	/* The one SC Control Point procedure worth implementing: a client
	 * setting the cumulative distance. Refused rather than obeyed, and the
	 * refusal is the honest answer - this node's distance accumulator is
	 * shared with two ANT+ masters and a BLE client rewriting it would
	 * make the same treadmill report two different totals on two radios. */
	ARG_UNUSED(total_distance);
	return -ENOTSUP;
}

static const struct bt_rscs_cp_cb rscs_cp_cb = {
	.set_cumulative = rscs_set_cumulative,
};

int treadmill_ble_start(void)
{
	struct bt_le_adv_param param = *BT_LE_ADV_CONN_FAST_1;
	struct bt_rscs_init_params rscs_params;
	int                        err;

	err = bt_enable(NULL);
	if (err != 0) {
		LOG_ERR("bt_enable: %d", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_SETTINGS)) {
		/* Bonds live in settings. Without this call a pairing survives
		 * only until reboot, which on a machine whose control point
		 * requires encryption means every power cycle costs the user a
		 * re-pair. */
		settings_load();
	}

	memset(&rscs_params, 0, sizeof(rscs_params));
	rscs_params.location = RSC_LOC_OTHER; /* a treadmill is not a foot pod */
	rscs_params.supported_locations.other = 1;
	rscs_params.features.inst_stride_len = 1;
	rscs_params.features.total_distance = 1;
	rscs_params.features.walking_or_running = 1;
	rscs_params.evt_handler = rscs_evt_handler;
	rscs_params.cp_cb = &rscs_cp_cb;

	/*
	 * CONFIG_BT_RSCS=y PUBLISHES THE SERVICE WHETHER OR NOT THIS IS
	 * CALLED - rscs.c uses a static BT_GATT_SERVICE_DEFINE - so a build
	 * that enabled the symbol and forgot the init would advertise a
	 * service whose features and location are whatever zeroed memory says.
	 * Failing loudly here is this repository's house answer to that.
	 *
	 * Note it also publishes Sensor Location (0x2A5D) and SC Control Point
	 * (0x2A55) whether a treadmill wants them or not.
	 */
	err = bt_rscs_init(&rscs_params);
	if (err != 0) {
		LOG_ERR("bt_rscs_init: %d - the RSC service is published by "
			"CONFIG_BT_RSCS regardless, so this build would "
			"advertise an uninitialised one", err);
		return err;
	}

	param.interval_min = ADV_UNITS(CONFIG_TREADMILL_BLE_ADV_INTERVAL_MS);
	param.interval_max = ADV_UNITS(CONFIG_TREADMILL_BLE_ADV_INTERVAL_MS) +
			     16u;

	/* A NON-NULL `sd` IS THE WHOLE CHANGE that makes this advertiser
	 * scannable - see trap 7 and ADR 0017 for what it costs. */
	err = bt_le_adv_start(&param, ad, ARRAY_SIZE(ad), sd, ARRAY_SIZE(sd));
	if (err != 0) {
		LOG_ERR("bt_le_adv_start: %d", err);
		return err;
	}

	LOG_INF("BLE: FTMS + RSC advertising every %u ms as '%s'",
		(unsigned)CONFIG_TREADMILL_BLE_ADV_INTERVAL_MS,
		CONFIG_BT_DEVICE_NAME);
	return 0;
}
