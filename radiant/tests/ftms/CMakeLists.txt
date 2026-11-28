# SPDX-License-Identifier: Apache-2.0
#
# radiant/tests/ftms - the kernel-free halves of the shared fitness-machine
# control layer, on native_sim with no Bluetooth stack: the FE-C / FTMS control
# arbiter (radiant_ctrl_arbiter.c) and the FTMS core (radiant_ble_ftms_core.c:
# the Control Point decision table, procedure execution against fake machine
# hooks, the Fitness Machine Status builder, Training Status and the
# advertising Service Data).
#
# Both source files are compiled unmodified, exactly as radiant/CMakeLists.txt
# compiles them into an image. The GATT glue (radiant_ble_ftms.c) needs a
# Bluetooth host and is covered by the build rows and the bench.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radiant_ftms_tests)

target_sources(app PRIVATE
  src/test_ctrl_arbiter.c
  src/test_ftms_core.c
  ../../src/ble/radiant_ctrl_arbiter.c
  ../../src/ble/radiant_ble_ftms_core.c
)
target_include_directories(app PRIVATE ../../include)
