/* SPDX-License-Identifier: Apache-2.0 */
/*
 * binding_store_codec.c - the binding record's byte layout (binding_store.h).
 *
 * No Zephyr and no storage call, so apps/dongle_thread/tests/binding_store
 * can link it alone on native_sim, the same split as
 * src/matter/bridge_storage_codec.cpp.
 */

#include <errno.h>
#include <string.h>

#include "binding_store.h"

#define OFF_VERSION 0u
#define OFF_DEVNUM  1u
#define OFF_DEVTYPE 3u
#define OFF_TRANS   4u
#define OFF_UUID    5u
#define OFF_LABEL   13u
#define OFF_PERIOD  29u
#define OFF_SEQ     31u

_Static_assert(OFF_SEQ + 4u == BINDING_RECORD_LEN, "record layout");
_Static_assert(OFF_PERIOD - OFF_LABEL == BINDING_RECORD_LABEL, "label width");

static void put_le(uint8_t *p, uint64_t v, size_t n)
{
	for (size_t i = 0u; i < n; i++) {
		p[i] = (uint8_t)(v >> (8u * i));
	}
}

static uint64_t get_le(const uint8_t *p, size_t n)
{
	uint64_t v = 0u;

	for (size_t i = 0u; i < n; i++) {
		v |= (uint64_t)p[i] << (8u * i);
	}
	return v;
}

static bool identity_valid(uint16_t devnum, uint8_t devtype, uint64_t uuid)
{
	/* 0 is the wildcard in both ID fields: a channel whose ID still reads
	 * 0 has not resolved a device, and try_bind() never binds one. */
	return devnum != 0u && devtype != 0u && uuid != 0u;
}

size_t binding_record_encode(const struct binding_record *r, uint8_t *buf,
			     size_t buf_len)
{
	size_t label_len;

	if (r == NULL || buf == NULL || buf_len < BINDING_RECORD_LEN ||
	    !identity_valid(r->devnum, r->devtype, r->uuid)) {
		return 0u;
	}

	memset(buf, 0, BINDING_RECORD_LEN);
	buf[OFF_VERSION] = (uint8_t)BINDING_RECORD_VERSION;
	put_le(&buf[OFF_DEVNUM], r->devnum, 2u);
	buf[OFF_DEVTYPE] = r->devtype;
	buf[OFF_TRANS] = r->trans;
	put_le(&buf[OFF_UUID], r->uuid, 8u);
	/* Bounded by the source array as well, so an unterminated label in
	 * `r` cannot be read past; the last byte stays NUL either way. */
	for (label_len = 0u; label_len < BINDING_RECORD_LABEL - 1u &&
			     r->label[label_len] != '\0';
	     label_len++) {
	}
	memcpy(&buf[OFF_LABEL], r->label, label_len);
	put_le(&buf[OFF_PERIOD], r->period, 2u);
	put_le(&buf[OFF_SEQ], r->seq, 4u);
	return BINDING_RECORD_LEN;
}

int binding_record_decode(const uint8_t *buf, size_t len,
			  struct binding_record *r)
{
	if (r == NULL) {
		return -EINVAL;
	}
	memset(r, 0, sizeof(*r));
	if (buf == NULL || len < 1u) {
		return -EINVAL;
	}
	/* The version first, so a later, longer layout reads as "not mine"
	 * rather than as a corrupt v1. */
	if (buf[OFF_VERSION] != BINDING_RECORD_VERSION) {
		return -ENOTSUP;
	}
	if (len != BINDING_RECORD_LEN) {
		return -EINVAL;
	}
	if (memchr(&buf[OFF_LABEL], '\0', BINDING_RECORD_LABEL) == NULL) {
		return -EINVAL;
	}

	r->devnum = (uint16_t)get_le(&buf[OFF_DEVNUM], 2u);
	r->devtype = buf[OFF_DEVTYPE];
	r->trans = buf[OFF_TRANS];
	r->uuid = get_le(&buf[OFF_UUID], 8u);
	memcpy(r->label, &buf[OFF_LABEL], BINDING_RECORD_LABEL);
	r->period = (uint16_t)get_le(&buf[OFF_PERIOD], 2u);
	r->seq = (uint32_t)get_le(&buf[OFF_SEQ], 4u);

	if (!identity_valid(r->devnum, r->devtype, r->uuid)) {
		memset(r, 0, sizeof(*r));
		return -EINVAL;
	}
	return 0;
}
