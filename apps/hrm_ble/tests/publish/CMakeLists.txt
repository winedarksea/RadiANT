# SPDX-License-Identifier: Apache-2.0
#
# The heart-rate node's publish path under test: the beat ring, the snapshot
# both radios read, and radiant's Heart Rate Measurement encoder.
#
# All three are compiled unmodified, exactly as the application and
# radiant/CMakeLists.txt compile them. None needs a radio, a Bluetooth host or
# a workqueue - the ring needs only <zephyr/sys/atomic.h>, the other two
# nothing at all - so the image is ztest plus these three files, and time is a
# variable the test advances.
#
# CI discovers it because .github/workflows/ci.yml's ztest job passes
# `-T app/apps/hrm_ble/tests`, which walks subdirectories.
#
# On Windows there is no native_sim (no host C compiler); use
# scripts\run_ztest_hw.ps1 -App apps\hrm_ble\tests\publish on a DK.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hrm_publish_test)

target_sources(app PRIVATE
  src/main.c
  ../../src/hrm_beat_ring.c
  ../../src/hrm_publish.c
  ../../../../radiant/src/ble/radiant_ble_hrs_encode.c
)
target_include_directories(app PRIVATE
  ../../src
  ../../../../radiant/include
)
