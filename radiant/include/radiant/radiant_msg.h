/* SPDX-License-Identifier: Apache-2.0 */
/*
 * radiant_msg.h - the flat, wire-shaped message radiant_event.c assembles and
 * hands to whatever sits outside the module, plus the wire-response-code
 * typedef a handful of radiant entry points return.
 *
 * WHY THIS EXISTS, SEPARATELY FROM radiant_event.h: radiant_event.h used to
 * reach outside the module - `#include "ant_radio.h"` via an extra
 * zephyr_include_directories(../src) - for exactly the three names declared
 * here. That is a self-containment violation for a Zephyr module meant to be
 * dropped into a project with nothing else from this repository on its
 * include path (docs/decisions/0002-clean-room-policy.md and the module's own
 * README). This header is the fix: struct radiant_msg, radiant_msg_err_t and
 * radiant_on_message() are now defined inside radiant, and it is
 * src/ant_radio.h - the application layer - that adapts them onto struct
 * antr_msg / antr_err_t / antr_on_message(), not the other way around.
 *
 * src/ant_radio.h's antr_msg and antr_err_t are unchanged and are the ones a
 * host-facing backend (sdk_ant, stub) still uses; struct radiant_msg here is
 * layout-identical to struct antr_msg on purpose (uint8_t id; uint8_t len;
 * const uint8_t *data;) so the application-side adapter that forwards
 * radiant_on_message() onto antr_on_message() is a field-for-field copy, not
 * a reinterpretation.
 *
 * Provenance: clean-room, from the same sources radiant_event.h names -
 * docs/sdk-ant-contract.md for the event path and radiant/include's own
 * HAL contract. See docs/decisions/0002-clean-room-policy.md.
 */

#ifndef RADIANT_RADIANT_MSG_H_
#define RADIANT_RADIANT_MSG_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A wire response/error code: 0 (RADIANT_WIRE_RESPONSE_NO_ERROR) or a
 * RADIANT_WIRE_* channel response code, verbatim - the same convention
 * antr_err_t documents in src/ant_radio.h, and value-compatible with it by
 * construction, since both ultimately name the same generated constants (see
 * radiant/include/radiant/radiant_wire.h and src/ant_wire.h's
 * aliases of it).
 */
typedef uint8_t radiant_msg_err_t;

/*
 * One ANT message travelling from the radio to whatever sits outside this
 * module. Deliberately wire-shaped rather than semantically decomposed - see
 * struct antr_msg in src/ant_radio.h, which this mirrors field for field.
 */
struct radiant_msg {
	/* ANT message ID - RADIANT_WIRE_MESG_BROADCAST_DATA_ID,
	 * _RESPONSE_EVENT_ID, _STARTUP_MESG_ID etc. Goes on the wire
	 * unchanged.
	 */
	uint8_t id;

	/*
	 * Valid bytes at `data`, becoming the frame's LEN byte - so it counts
	 * the channel byte plus payload. Must be <= RADIANT_WIRE_MAX_SIZE_VALUE
	 * (38, not the 20 derived elsewhere from inbound-only messages: an
	 * advanced-burst message reaches 25).
	 */
	uint8_t len;

	/*
	 * The message body, exactly as it goes on the wire. data[0] is the
	 * channel number for channel-scoped messages, else the first payload
	 * byte.
	 *
	 * LIFETIME: valid only for the duration of the radiant_on_message()
	 * call - the receiver copies what it needs before returning.
	 *
	 * Never NULL - a bodyless message has len == 0 and data still valid.
	 */
	const uint8_t *data;
};

/*
 * Called by radiant_event.c for every message the host should see.
 *
 * IMPLEMENTED OUTSIDE THE MODULE, NOT BY radiant_event.c - no registration
 * call, the symbol is resolved at link time. The application-side antr_*
 * adapter (src/ant_radio_radiant.c, the file radiant_api.c became once it
 * moved out of this module) is the one translation unit that defines it,
 * forwarding onto antr_on_message() - the same function src/ant_radio.h's
 * other two backends (sdk_ant, stub) already deliver events through, so
 * src/ant_serial_bridge.c stays the single implementation of antr_on_message()
 * regardless of which radio backend is compiled in.
 *
 * Contract on the caller (radiant_event.c): see radiant_event.h's own header
 * comment for the full event-path reasoning (the ISR-safe queue, the
 * drain-thread assembly, the RX-timestamp-from-t_sync-only rule). The
 * contract on antr_on_message() in src/ant_radio.h - not called re-entrantly,
 * must not block, etc. - applies transitively through the forwarder.
 */
void radiant_on_message(const struct radiant_msg *msg);

#ifdef __cplusplus
}
#endif

#endif /* RADIANT_RADIANT_MSG_H_ */
