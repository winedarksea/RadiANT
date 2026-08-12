# SPDX-License-Identifier: Apache-2.0
#
# The radiant_core unit-test application.
#
# This is a standalone Zephyr application, deliberately not a subdirectory of
# the dongle build: it never includes the top-level CMakeLists.txt, so it has
# no sdk-ant dependency of any kind and the CI job that runs it needs no
# secret. That is the whole point - native_sim does not build on Windows (no
# host C compiler, no QEMU), so every C assertion in this project executes
# only in CI on Linux, and it has to execute on forks and on every pull
# request or the parallel core work has no verification at all.

cmake_minimum_required(VERSION 3.20.0)

# radiant_core is consumed here exactly as any third party would consume it:
# as a Zephyr module, before find_package(Zephyr), with CONFIG_RADIANT_CORE=y
# in prj.conf doing the selecting. This application therefore contains no
# application code of its own beyond the suites and the mock radio - which is
# what earns the module claim. It also means the module owns its source list
# and this file does not duplicate it; the duplicate that used to live here
# was one line per core module and a suite that silently stopped linking the
# module it tests still passes.
list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/..")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radiant_core_tests)

# ── Adding a test suite ─────────────────────────────────────────────────────
#
# Drop a .c file into src/ that declares its own ZTEST_SUITE(). Nothing else.
# The glob is CONFIGURE_DEPENDS, and twister configures from scratch on every
# run anyway, so a new file is picked up without editing this file. This is
# structured for a fan-out of parallel agents each owning one core module and
# one suite file: an explicit source list would make this file contested, and
# a contested file serialises the fan-out it exists to enable.
file(GLOB suite_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
target_sources(app PRIVATE ${suite_sources})

# ── The mock radio ──────────────────────────────────────────────────────────
#
# radiant_core/tests/fake_radio.c implements
# radiant_core/include/radiant_core/radiant_radio_hal.h against no hardware,
# and is what makes six of the seven core agents able to work in parallel
# without a board. It is the HAL implementation for this image, which is why
# prj.conf selects CONFIG_RADIANT_CORE_BACKEND_NONE: the module compiles no
# backend of its own and this file is the one definition of the eight entry
# points.
target_sources(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fake_radio.c")

# ── The mock NVM ────────────────────────────────────────────────────────────
#
# The same arrangement one layer up: fake_nvm.c implements src/node/node_nvm.h
# against RAM, so src/node/node_ident.c is compiled unmodified and the two
# things a real backend cannot be asked to do on demand - a power cycle and a
# write that fails - are callable from a suite.
#
# src/node/node_nvm_settings.c is deliberately NOT built here. It is the
# shipping backend (the settings subsystem over NVS) and it needs a storage
# partition this test application does not have; linking both would be a
# duplicate-symbol error, which is the same guard CONFIG_RADIANT_CORE_BACKEND_
# NONE provides for the radio.
target_sources(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fake_nvm.c")

# ── src/profiles ────────────────────────────────────────────────────────────
#
# The RadiANT telemetry envelope (device type 0x60): docs/radiant-telemetry.md
# in code. Compiled into THIS APPLICATION rather than into the radiant_core
# module, and the directory boundary is the reason, not convenience:
# docs/decisions/0002-clean-room-policy.md draws its read-scope line at a path.
# radiant_core/** is strict clean-room; src/profiles/ is the pragmatic side,
# where an implementer may read a page-layout table. A profile encoder that
# linked into the link layer would erase that boundary, so it does not.
#
# (Phase E itself read no gated document at all - it is derived entirely from
# docs/radiant-telemetry.md, which is this project's own specification. The
# separation is structural, so that the next profile author inherits it.)
#
# Listed explicitly rather than globbed: unlike src/ above, this is a library
# and not a suite drop box, and a new file here is a deliberate act.
# profile_handoff.c and profile_schedule.c are the two files here that include a
# radiant_core header: the page codecs are pure, but applying a handoff means
# driving a channel and announcing a clock accuracy means widening one, and a
# format with no consumer would not have been worth specifying. The dependency
# runs application -> core, which is the direction the module boundary already
# permits; nothing in radiant_core knows either file exists.
target_sources(app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_bits.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_telemetry.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_sched.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_handoff.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_schedule.c"
  # The ANT+ compatibility profiles (compat-C5): heart rate 0x78 and bicycle
  # power 0x0B, the common pages they share, and the compat client that inserts
  # the RadiANT beacon and attestation pages through profile_sched.c's seam.
  #
  # profile_compat.c is the THIRD file here that includes a radiant_core header,
  # and it is the only one of the four that does. That is the boundary the phase
  # exists to draw: profile_hr.c and profile_power.c reach no radiant_core
  # header even transitively, so "the profile modules never call radiant_sec" is
  # a property of the include graph rather than a rule somebody remembers.
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_common.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_compat.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_hr.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_power.c"
  # Layer D (compat-C7): the over-air enrolment window. The FOURTH file here
  # that includes a radiant_core header, and the second that has to - adding a
  # keyholder is a key operation. It names no page number and no frame index,
  # so the boundary profile_compat.c draws is not widened by it.
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_enrol.c"
  # Layer C (compat-C8): the private-mode switch. The FIFTH file here that
  # includes a radiant_core header and the third that has to - deriving the
  # locator needs K_id and verifying a command needs K_cmd, and both live where
  # the root key does. It names no page number of its own: the announcement is
  # two frames of the beacon page's set, which profile_compat.h already owns.
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_private.c"
  # Adaptive frequency (RF-7): choosing a quieter RF index from the
  # channel-quality map, and announcing the move on page 0x13 before it happens.
  # The SIXTH file here that includes a radiant_core header and the fourth that
  # has to - it reads radiant_chanmap.h, which is the map ADR 0012 selects from,
  # and radiant_radio_hal.h for the index range. It reaches no scheduler and no
  # search header on purpose: "discovery never moves" is a property of the
  # include graph here, not a rule somebody remembers.
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_freq.c"
  # Response slots and the reliable-command state machine (RF-8): section 9's
  # idempotency rule, the inline tag at both widths, and the downlink window
  # profile_schedule.c has been announcing since RF-5a. The SEVENTH file here
  # that includes a radiant_core header and the fifth that has to - verifying a
  # command needs a key, and arming a listen slot needs the scheduler. It adds
  # no slot kind to radiant_sched.c and reaches no search header: a response
  # slot is an ordinary bounded receive window, which is the claim
  # test_command.c exists to keep honest.
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles/profile_command.c"
)
target_include_directories(app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/profiles")

# ── src/node ────────────────────────────────────────────────────────────────
#
# The hostless node's identity, epoch and pairing scalar (compat-C6):
# docs/decisions/0009-hostless-node-identity.md in code.
#
# Outside radiant_core for the reason the ADR gives in its decision 7, not for
# convenience: radiant_core is TOLD an epoch and refuses one that has not
# advanced, and putting the persistence inside it would hand every consumer of
# the module a flash dependency and a storage backend to serve one class of
# node. So the dependency runs application -> core here as well, and nothing in
# radiant_core knows this directory exists. There is no new core entry point for
# a node-derived epoch: node_ident.c calls radiant_sec_set_epoch(), which is the
# same function the host adapter calls with a host-supplied value.
target_sources(app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/node/node_ident.c"
)
target_include_directories(app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src/node")

# This directory, so a suite writes #include "fake_radio.h" rather than
# #include "../fake_radio.h". Suites live in src/ and the mock does not, so
# without this every suite author gets the relative path wrong exactly once.
#
# Nothing else is needed: the module's own zephyr_include_directories() puts
# both radiant_core/include and ../src (for src/ant_radio.h and the generated
# src/ant_wire.h, which radiant_event.c reads) on the search path for this
# target too.
target_include_directories(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
