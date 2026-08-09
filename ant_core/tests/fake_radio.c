/*
 * SPDX-License-Identifier: Apache-2.0
 *
 * Provenance: original work, written from the plan's HAL sketch alone. No
 * sdk-ant source, header or binary was consulted, and nothing here derives
 * from disassembly of libant.a.
 *
 * PLACEHOLDER - this file is keystone K5 and is not written yet.
 *
 * The real fake_radio.c implements ant_core/include/ant_radio_hal.h (keystone
 * K3) against no hardware: a virtual clock the test advances by hand, a queue
 * of frames to hand back on the next arm-for-RX, and a record of every
 * arm/transmit call with its absolute microsecond deadline. That is what lets
 * six of the seven core agents - frame, sched, channel, search, events,
 * transfer - develop and verify with no board, which is the entire reason the
 * core fan-out can run in parallel.
 *
 * It exists now, empty, so the slot is in the build before its owner arrives:
 * ant_core/tests/CMakeLists.txt already compiles it, so K5 replaces the body
 * of this file and touches nothing else.
 *
 * The HAL header does not exist yet, so there is nothing to implement against
 * and nothing to include. An empty translation unit is not valid C, hence the
 * declaration below; it is removed wholesale when the real implementation
 * lands.
 */

extern int ant_core_fake_radio_placeholder;
