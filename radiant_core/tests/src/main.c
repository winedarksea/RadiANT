/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original work. Nothing here derives from sdk-ant, from any
 * adopter-gated document, or from disassembly of libant.a.
 *
 * The harness suite for radiant_core.
 *
 * These two tests are not placeholders and they are not smoke tests for
 * ztest. They assert the two host properties that every later suite silently
 * depends on, and that are genuinely capable of differing between the
 * native_sim host and the nRF52840/nRF54L target the code actually runs on.
 * If either one is wrong, the frame, channel and scheduler suites still pass
 * while proving nothing - which is the failure mode a test harness must not
 * have, because it is indistinguishable from success.
 *
 * Core module suites go in sibling files under this directory, each with its
 * own ZTEST_SUITE(). Nothing needs to be added to CMakeLists.txt.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include <zephyr/toolchain.h>
#include <zephyr/ztest.h>

ZTEST_SUITE(host_abi, NULL, NULL, NULL, NULL, NULL);

/*
 * ANT is little-endian on the wire - a 16-bit device number goes out low byte
 * first - and the encode/decode paths will be tested by comparing a struct's
 * bytes in memory against a captured frame. That comparison is only
 * meaningful if the test host orders bytes the way the target does.
 */
ZTEST(host_abi, test_host_is_little_endian)
{
	const uint16_t devnum = 0x1234U;
	static const uint8_t on_the_wire[] = { 0x34U, 0x12U };
	uint8_t bytes[sizeof(devnum)];

	zassert_equal(sizeof(uint8_t), 1U, "uint8_t is not one byte");
	zassert_equal(sizeof(devnum), sizeof(on_the_wire),
		      "uint16_t is not two bytes");

	memcpy(bytes, &devnum, sizeof(devnum));

	zassert_mem_equal(bytes, on_the_wire, sizeof(on_the_wire),
			  "host byte order differs from the target's; "
			  "byte-level frame tests on this host prove nothing");
}

/*
 * The frame layer describes on-air and host-serial frames as packed structs
 * overlaid on a byte buffer. A toolchain that ignored __packed would insert
 * padding, every field would read back correctly through the struct, and
 * every test would pass against a layout that no real receiver would accept.
 * So assert the attribute has an effect before relying on it.
 */
struct wire_layout_probe {
	uint8_t sync;
	uint16_t devnum;
	uint8_t checksum;
} __packed;

ZTEST(host_abi, test_packed_structs_have_no_padding)
{
	zassert_equal(sizeof(struct wire_layout_probe), 4U,
		      "__packed had no effect: struct is %zu bytes, expected 4",
		      sizeof(struct wire_layout_probe));
	zassert_equal(offsetof(struct wire_layout_probe, devnum), 1U,
		      "__packed struct field was aligned up");
	zassert_equal(offsetof(struct wire_layout_probe, checksum), 3U,
		      "__packed struct field was aligned up");
}
