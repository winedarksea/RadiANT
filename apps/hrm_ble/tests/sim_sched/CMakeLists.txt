# SPDX-License-Identifier: Apache-2.0
#
# The simulated source's beat schedule, under test.
#
# ── Why this application is three lines and no module ──────────────────────
#
# The whole point of src/hrm_sim_sched.c being separate from src/hrm_sim.c is
# that it is a PURE FUNCTION: no globals, no kernel calls, no clock, no radio.
# So this image needs none of radiant, none of the profiles, no HAL and no
# backend - it compiles one production .c file beside one test file, and 36 000
# virtual ticks run in microseconds.
#
# That cheapness is the argument. docs/hrm-reference-design.md's production
# checklist lists three drift checks at three costs; this is the free one, the
# one that runs on every CI push, and the one that DID NOT EXIST when this
# application shipped a strap notifying 67 times a minute while claiming 72.
#
# CI discovers it because .github/workflows/ci.yml's ztest job passes
# `-T app/apps/hrm_ble/tests` alongside `-T app/radiant/tests`. It is not under
# radiant/tests because it tests an application, not the module, and
# radiant/tests/CMakeLists.txt's glob deliberately does not reach out of the
# module.
#
# On Windows there is no native_sim (no host C compiler); use
# scripts\run_ztest_hw.ps1 -App apps\hrm_ble\tests\sim_sched on a DK.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hrm_sim_sched_test)

target_sources(app PRIVATE
  src/main.c
  ../../src/hrm_sim_sched.c
)
target_include_directories(app PRIVATE ../../src)
