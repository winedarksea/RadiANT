/* SPDX-License-Identifier: Apache-2.0 */
/*
 * binding_store.h - self-channel bindings that survive a reboot.
 *
 * radiant/src/bridge/radiant_binding.c is RAM only and stays storage-free;
 * this is the application half that keeps a binding on flash and puts it
 * back (docs/radiant-bridge.md section 5).
 *
 * WHAT IS KEPT. One record per paired channel ID, under the Zephyr settings
 * key "radiant/bind/<n>", n in [0, BINDING_STORE_SLOTS). Record v1, 35 bytes,
 * little-endian:
 *
 *   [0]      version, 1
 *   [1..2]   device number
 *   [3]      device type
 *   [4]      transmission type
 *   [5..12]  uuid (never 0)
 *   [13..28] label, NUL-padded, NUL-terminated within the 16 bytes
 *   [29..30] channel period, 32768 Hz counts (0 = never read)
 *   [31..34] bind sequence: larger = paired later
 *
 * The sequence is not in struct radiant_binding. It is what decides which
 * records get the self channels at boot when there are more records than
 * channels (the newest), the order a long press walks back through after a
 * reboot, and which record a ninth pairing overwrites (the oldest one that
 * is not bound now).
 *
 * WHEN IT WRITES. Only on a bind (self_channels.c try_bind(), inside a
 * button-opened pairing window) and on a user eviction (a short press freeing
 * a channel, or a long press). Never from traffic: nothing a sensor sends can
 * cause a flash write, which is the 11-22 security review's rule for rename
 * budgets applied here.
 *
 * WHAT A RELEASE DOES. When a host takes a self channel after its reset, the
 * binding is released (self_channels.c release_binding()) but its record is
 * KEPT, dormant: the next boot pins the sensor again. Only a user eviction
 * deletes a record.
 *
 * RE-PAIR. Pairing a channel ID that has a record reuses that record's uuid
 * (radiant_binding_restore()), so its MQTT topics and anything else keyed on
 * the uuid carry on. A record deleted by an eviction is gone, so pairing the
 * same sensor after an eviction mints a new uuid: eviction is the per-binding
 * reset.
 *
 * WITHOUT CONFIG_ANT_DONGLE_BINDING_STORE (no settings subsystem in the
 * image) the codec is still compiled, every I/O call is a no-op that keeps
 * nothing, and binding_store_on_evict() is still called on a user eviction.
 */

#ifndef BINDING_STORE_H_
#define BINDING_STORE_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <radiant/radiant_binding.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BINDING_RECORD_VERSION 1u
#define BINDING_RECORD_LEN     35u
#define BINDING_RECORD_LABEL   16u

/* One per binding table row: there is never a reason to remember more
 * channel IDs than the table could hold at once. */
#define BINDING_STORE_SLOTS RADIANT_BINDING_MAX

struct binding_record {
	uint16_t devnum;
	uint8_t  devtype;
	uint8_t  trans;
	uint64_t uuid;
	char     label[BINDING_RECORD_LABEL];
	uint16_t period;
	uint32_t seq;
};

/* ── The codec: pure, no Zephyr, no storage (binding_store_codec.c) ─────── */

/*
 * Encode `r` into `buf`. Returns BINDING_RECORD_LEN, or 0 if `buf_len` is
 * shorter than that or `r` would not decode (devnum 0, devtype 0, uuid 0).
 * A label longer than 15 characters is truncated so it stays terminated.
 */
size_t binding_record_encode(const struct binding_record *r, uint8_t *buf,
			     size_t buf_len);

/*
 * Decode a stored record. Returns 0 and fills *r, or:
 *   -EINVAL  wrong length, devnum 0, devtype 0, uuid 0, or a label with no
 *            NUL in its 16 bytes;
 *   -ENOTSUP a version other than 1.
 * On an error *r is zeroed. Never reads past `len`.
 */
int binding_record_decode(const uint8_t *buf, size_t len,
			  struct binding_record *r);

/* ── Settings I/O (binding_store.c) ─────────────────────────────────────── */

/*
 * At boot, after radiant_binding_init(): read every record, and restore the
 * `max` newest into the binding table with radiant_binding_restore(). Called
 * once, from bridge_pump.c's thread. Returns the number restored. The sources
 * are kept, oldest first, for binding_store_boot_sources().
 */
size_t binding_store_boot_restore(size_t max);

/*
 * The sources binding_store_boot_restore() put back, oldest pairing first,
 * up to `max` of them. Waits up to `timeout_ms` for that restore to have run
 * (the self-channel thread starts seconds later, so it normally does not
 * wait). Returns the count, 0 if the restore had not run in time.
 */
size_t binding_store_boot_sources(uint32_t *sources, size_t max,
				  uint32_t timeout_ms);

/* The record for this channel ID, if one is kept. */
bool binding_store_lookup(uint16_t devnum, uint8_t devtype, uint8_t trans,
			  struct binding_record *out);

/*
 * Save `b` as the newest record: in the slot that already holds its channel
 * ID, else a free one, else over the oldest record whose channel ID is not
 * bound now. Returns 0 or a negative errno (the binding itself is unaffected
 * either way: it just will not survive a reboot).
 */
int binding_store_save(const struct radiant_binding *b);

/*
 * A USER EVICTION: delete the record for `b`'s channel ID, then call
 * binding_store_on_evict(b->uuid). Called with the binding still in the table
 * (before radiant_binding_unbind()). Never called for a host-takes-channel
 * release, which keeps the record.
 */
void binding_store_evict(const struct radiant_binding *b);

/*
 * THE PER-UUID ERASE HOOK. Called by binding_store_evict() after the record
 * is deleted, on the self-channel thread (1536 B of stack: do not block, and
 * keep flash work short). A weak no-op here; anything that keeps data keyed
 * on a binding uuid (the HR personalisation profile, for one) defines it
 * strongly and erases that uuid's data. Only one strong definition can link,
 * so a second consumer must chain from the first.
 */
void binding_store_on_evict(uint64_t uuid);

#ifdef __cplusplus
}
#endif

#endif /* BINDING_STORE_H_ */
