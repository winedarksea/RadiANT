/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fakes/mpsl_timeslot.h - the timeslot API, as a header a test can drive.
 *
 * Provenance: original clean-room work, written against nrfxlib's own
 * mpsl_timeslot.h (v3.4.0). The type layouts, the signal numbers, the action
 * numbers and the published limits are reproduced EXACTLY, because every one of
 * them is load-bearing in the file under test:
 *
 *   - the signal numbers are what on_signal() switches on, and the trace this
 *     file's header reads bugs out of is printed as those numbers;
 *   - the action numbers are what the trace's low nibble is;
 *   - MPSL_TIMESLOT_LENGTH_MAX_US, EXTENSION_TIME_MIN_US and
 *     EXTENSION_MARGIN_MIN_US are read by four BUILD_ASSERTs and by the length
 *     arithmetic in gate_acquire(), so a fake with rounder numbers would move
 *     the very boundaries the arithmetic exists to respect.
 *
 * A fake that invented its own would not be testing this gate. Nothing here is
 * copied from MPSL's implementation - there is none; the three entry points are
 * implemented in fake_mpsl.c.
 */

#ifndef RADIANT_CORE_TESTS_GATE_FAKE_MPSL_TIMESLOT_H_
#define RADIANT_CORE_TESTS_GATE_FAKE_MPSL_TIMESLOT_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MPSL_TIMESLOT_LENGTH_MIN_US                    (100UL)
#define MPSL_TIMESLOT_LENGTH_MAX_US                    (100000UL)
#define MPSL_TIMESLOT_DISTANCE_MAX_US                  (256000000UL - 1UL)
#define MPSL_TIMESLOT_EARLIEST_TIMEOUT_MAX_US          (256000000UL - 1UL)
#define MPSL_TIMESLOT_START_JITTER_US                  (1UL)
#define MPSL_TIMESLOT_EXTENSION_TIME_MIN_US            (200UL)
#define MPSL_TIMESLOT_EXTENSION_PROCESSING_TIME_MAX_US (25UL)
#define MPSL_TIMESLOT_EXTENSION_MARGIN_MIN_US          (87UL)

typedef uint8_t mpsl_timeslot_session_id_t;

enum MPSL_TIMESLOT_SIGNAL {
	MPSL_TIMESLOT_SIGNAL_START            = 0,
	MPSL_TIMESLOT_SIGNAL_TIMER0           = 1,
	MPSL_TIMESLOT_SIGNAL_RADIO            = 2,
	MPSL_TIMESLOT_SIGNAL_EXTEND_FAILED    = 3,
	MPSL_TIMESLOT_SIGNAL_EXTEND_SUCCEEDED = 4,
	/* 5 and above are delivered from mpsl_low_priority_process(), where
	 * returning anything but ACTION_NONE asserts. fake_mpsl.c enforces
	 * exactly that. */
	MPSL_TIMESLOT_SIGNAL_BLOCKED          = 5,
	MPSL_TIMESLOT_SIGNAL_CANCELLED        = 6,
	MPSL_TIMESLOT_SIGNAL_SESSION_IDLE     = 7,
	MPSL_TIMESLOT_SIGNAL_INVALID_RETURN   = 8,
	MPSL_TIMESLOT_SIGNAL_SESSION_CLOSED   = 9,
	MPSL_TIMESLOT_SIGNAL_OVERSTAYED       = 10,
};

enum MPSL_TIMESLOT_SIGNAL_ACTION {
	MPSL_TIMESLOT_SIGNAL_ACTION_NONE    = 0,
	MPSL_TIMESLOT_SIGNAL_ACTION_EXTEND  = 1,
	MPSL_TIMESLOT_SIGNAL_ACTION_END     = 2,
	MPSL_TIMESLOT_SIGNAL_ACTION_REQUEST = 3,
};

enum MPSL_TIMESLOT_HFCLK_CFG {
	MPSL_TIMESLOT_HFCLK_CFG_XTAL_GUARANTEED = 0,
	MPSL_TIMESLOT_HFCLK_CFG_NO_GUARANTEE    = 1,
};

enum MPSL_TIMESLOT_PRIORITY {
	MPSL_TIMESLOT_PRIORITY_HIGH   = 0,
	MPSL_TIMESLOT_PRIORITY_NORMAL = 1,
};

enum MPSL_TIMESLOT_REQUEST_TYPE {
	MPSL_TIMESLOT_REQ_TYPE_EARLIEST = 0,
	MPSL_TIMESLOT_REQ_TYPE_NORMAL   = 1,
};

typedef struct {
	uint8_t  hfclk;
	uint8_t  priority;
	uint32_t length_us;
	uint32_t timeout_us;
} mpsl_timeslot_request_earliest_t;

typedef struct {
	uint8_t  hfclk;
	uint8_t  priority;
	uint32_t distance_us;
	uint32_t length_us;
} mpsl_timeslot_request_normal_t;

typedef struct {
	uint8_t request_type;
	union {
		mpsl_timeslot_request_earliest_t earliest;
		mpsl_timeslot_request_normal_t   normal;
	} params;
} mpsl_timeslot_request_t;

typedef struct {
	uint8_t callback_action;
	union {
		struct {
			mpsl_timeslot_request_t *p_next;
		} request;
		struct {
			uint32_t length_us;
		} extend;
	} params;
} mpsl_timeslot_signal_return_param_t;

typedef mpsl_timeslot_signal_return_param_t *(*mpsl_timeslot_callback_t)(
	mpsl_timeslot_session_id_t session_id, uint32_t signal);

int32_t mpsl_timeslot_session_open(
	mpsl_timeslot_callback_t    mpsl_timeslot_signal_callback,
	mpsl_timeslot_session_id_t *p_session_id);
int32_t mpsl_timeslot_session_close(mpsl_timeslot_session_id_t session_id);
int32_t mpsl_timeslot_request(mpsl_timeslot_session_id_t      session_id,
			      mpsl_timeslot_request_t const *p_request);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_CORE_TESTS_GATE_FAKE_MPSL_TIMESLOT_H_ */
