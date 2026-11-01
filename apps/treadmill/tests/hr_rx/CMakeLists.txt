# SPDX-License-Identifier: Apache-2.0
#
# The treadmill's ANT+ heart-rate slave (src/treadmill_hr_rx.c), against a
# stub antr_* layer.
#
# WHY A STUB AND NOT THE REAL STACK. The defect this image was added for
# (review 2026-10-31, B2-3) is an ORDERING one: the HR slave reopened its
# channel when RX_SEARCH_TIMEOUT arrived, before the CHANNEL_CLOSED that
# follows it, and on the real stack that open is refused with WRONG_STATE
# whenever a scan window is still bound to the channel. A stub whose open
# answers WRONG_STATE until it has been told the channel closed is the
# smallest thing that states that contract - and it is what lets the case run
# in microseconds with no radio, no scheduler and no fake HAL.
#
# CI discovers it because .github/workflows/ci.yml's ztest job passes
# `-T app/apps/treadmill/tests`, which walks subdirectories.
#
# On Windows there is no native_sim; use
#   scripts\run_ztest_hw.ps1 -App apps\treadmill\tests\hr_rx
# on a DK.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(treadmill_hr_rx_test)

target_sources(app PRIVATE
  src/main.c
  ../../src/treadmill_hr_rx.c
)
target_include_directories(app PRIVATE
  ../../src
  ../../../common/ant
  ../../../../radiant/include
  ../../../../radiant/src/profiles
)
