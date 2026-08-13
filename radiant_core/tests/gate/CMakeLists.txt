# SPDX-License-Identifier: Apache-2.0
#
# The MPSL gate unit-test application.
#
# ── Why this is a third application ──────────────────────────────────────────
#
# radiant_core/src/radiant_radio_nrf_gate_mpsl.c had no test coverage of any
# kind, and it is the one file in the project whose failure mode is PERMANENT
# AND SILENT: a skipped radiant_nrf_gate_on_grant_end() leaves the 802.15.4
# driver's write-once SUBSCRIBE_RXEN swapped out for the rest of the boot.
#
# It cannot be a suite in ../src. That application selects
# CONFIG_RADIANT_CORE_BACKEND_NONE and compiles ../fake_radio.c as the HAL, and
# the gate is part of the NRF backend - so putting it there would mean either
# building the whole nRF backend (which defines the same eight HAL entry points
# fake_radio.c does, a duplicate-symbol link error) or compiling the gate beside
# a backend it is a seam inside of. This application does neither: it compiles
# the gate against fakes for MPSL and for the five radiant_nrf_gate_* callbacks,
# so the gate is the only production code in the image.
#
# The parent's glob does not reach in here and this one does not reach out
# there, which is what keeps the three link images separate. CI's
# `twister -T app/radiant_core/tests` walks subdirectories, so testcase.yaml
# here is picked up with no workflow edit, and scripts\run_ztest_hw.ps1 derives
# its build directory from the application's leaf name, so
# `-App radiant_core\tests\gate` needs no script edit either.

cmake_minimum_required(VERSION 3.20.0)

# Two levels up: the module root is radiant_core/, this is radiant_core/tests/gate.
list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../..")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radiant_core_gate_tests)

# Same glob rule as the other two applications: a new suite is a new file in
# src/ and this file does not change.
file(GLOB suite_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/*.c")
target_sources(app PRIVATE ${suite_sources})

# ── The file under test ─────────────────────────────────────────────────────
#
# NOT radiant_radio_nrf_gate_mpsl.c directly. gate_under_test.c #includes it, so
# that the suite can read the static counters the invariant is carried by -
# g.hw_held and stats.late_disarm are the whole of A1 and A2 and neither has any
# other observable. See the header of gate_under_test.c for why that is better
# than an accessor behind #ifdef CONFIG_ZTEST in the production file.
#
# It must stay the ONLY translation unit that includes it, or the statics exist
# twice and the suites silently test two different gates.
target_sources(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/gate_under_test.c")

# The arbiter and the backend, both fake. See fake_mpsl.h.
target_sources(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fake_mpsl.c")

# The mock radio, shared with the other two applications rather than copied. It
# is the HAL implementation for this image, and here it serves one further
# purpose: radiant_radio_now() is the gate's timebase and radiant_radio_abort()
# is what SIGNAL_EXTEND_FAILED calls, so the mock's counters are how the suite
# proves that path aborted the armed operation rather than only claiming to.
target_sources(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../fake_radio.c")

# ── The fake headers, and the one thing to know about them ──────────────────
#
# fakes/ is listed FIRST and it SHADOWS three system headers for this target:
# <mpsl_timeslot.h>, <mpsl_hwres.h>, <nrf_errno.h> and <hal/nrf_timer.h>. That
# is deliberate and it is contained: CMake places a target's own include
# directories ahead of those it inherits from linked libraries, and the
# radiant_core module is a separate CMake target that never sees this directory.
# Nothing outside this application compiles against these headers.
#
# The shadow of hal/nrf_timer.h is the one that earns its keep: MPSL_TIMER0
# becomes a register model in RAM whose counter a test can move, which is what
# makes BUG 9's race - the counter overtaking the compare between the write and
# the re-read - a two-line setup instead of a coin toss.
target_include_directories(app BEFORE PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/fakes")

# This directory for fake_mpsl.h / gate_probe.h, and the parent for
# fake_radio.h - the same spelling the other two applications' suites use.
#
# ../../src is radiant_core/src, for radiant_radio_nrf_gate.h. The module's own
# zephyr_include_directories() puts radiant_core/include and the APPLICATION's
# src/ on the path, not its own - the gate interface is internal to the module
# by design (see its header: no MPSL type or symbol appears above the backend),
# so a consumer would have no business including it and this application is not
# a consumer.
target_include_directories(app PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}"
  "${CMAKE_CURRENT_SOURCE_DIR}/.."
  "${CMAKE_CURRENT_SOURCE_DIR}/../../src")
