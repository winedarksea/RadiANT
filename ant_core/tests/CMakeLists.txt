# SPDX-License-Identifier: Apache-2.0
#
# The ant_core unit-test application.
#
# This is a standalone Zephyr application, deliberately not a subdirectory of
# the dongle build: it never includes the top-level CMakeLists.txt, so it has
# no sdk-ant dependency of any kind and the CI job that runs it needs no
# secret. That is the whole point - native_sim does not build on Windows (no
# host C compiler, no QEMU), so every C assertion in this project executes
# only in CI on Linux, and it has to execute on forks and on every pull
# request or the parallel core work has no verification at all.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_core_tests)

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
# ant_core/tests/fake_radio.c implements ant_core/include/ant_radio_hal.h
# against no hardware, and is what makes six of the seven core agents able to
# work in parallel without a board. It is keystone K5 and is authored after
# K3 freezes the HAL header; the file present today is a placeholder that
# compiles to nothing so this slot exists before its owner arrives.
target_sources(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/fake_radio.c")

# ── The code under test ─────────────────────────────────────────────────────
#
# ant_core/include/ does not exist yet (keystone K3 authors it). CMake does
# not require an include directory to exist, so naming it now costs nothing
# and means the K3 header is on the search path the moment it lands.
target_include_directories(app PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/../include")

# Core module sources are added here as they are written, one line per module
# (ant_frame.c, ant_sched.c, ant_channel.c, ant_search.c, ant_event.c,
# ant_api.c, ant_ack.c, ant_burst.c). They are listed explicitly rather than
# globbed, because a suite that silently stops linking the module it tests
# still passes, and an explicit list makes that a merge conflict instead.
set(ant_core_sources)
if(ant_core_sources)
  target_sources(app PRIVATE ${ant_core_sources})
endif()
