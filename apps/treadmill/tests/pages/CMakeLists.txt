# SPDX-License-Identifier: Apache-2.0
#
# The treadmill's state module and every unit conversion it owns, under test.
#
# ── Why this application is three lines and no module ───────────────────────
#
# The whole point of src/treadmill_state.c including no Zephyr header, taking
# no lock and reading no clock is that it can be driven by a counted loop: it
# is TOLD how much time passed. So this image needs none of radiant, none of
# the profiles, no HAL and no backend - it compiles one production .c file
# beside one test file, and a simulated hour of running takes microseconds.
#
# That cheapness is the argument, and it is apps/hrm_ble/tests/sim_sched's
# argument repeated because it was right there. The properties this covers are
# the ones that cannot be seen from a receiver:
#
#   * the strides-to-STEPS doubling, which is the single most likely factor-of-
#     two error in this application (three outputs, two units);
#   * the FE-C 0.01 % / FTMS 0.1 % incline scaling, in both directions;
#   * the accumulator carries - a tick's worth of rounding is nothing and an
#     hour's worth of it is a visible error, which is exactly the class of bug
#     a bench run reads as a calibration problem;
#   * the vertical-distance divisor, where dropping two zeroes makes a 1 %
#     incline climb like a 100 % one and nothing downstream would question it.
#
# CI discovers it because .github/workflows/build.yml's ztest job passes
# `-T app/apps/treadmill/tests` alongside `-T app/radiant/tests` and
# `-T app/apps/hrm_ble/tests`.
#
# On Windows there is no native_sim (no host C compiler); use
# scripts\run_ztest_hw.ps1 -App apps\treadmill\tests\pages on a DK.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(treadmill_pages_test)

target_sources(app PRIVATE
  src/main.c
  ../../src/treadmill_state.c
  # The control-owner token. Kernel-free for the same reason treadmill_state.c
  # is, and that is not a stylistic echo - it is what makes the policy testable
  # at all. There is no ztest anywhere for src/treadmill_ble.c, so arbitration
  # implemented inside that file would have been arbitration nothing could
  # reach; it lives in its own module so this image can drive it.
  ../../src/treadmill_control.c
)
target_include_directories(app PRIVATE ../../src)
