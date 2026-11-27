# SPDX-License-Identifier: Apache-2.0
#
# radiant/tests/ble - the kernel-free decisions of the shared BLE layer and
# the product identity record, on native_sim with no Bluetooth stack:
# advertising restart and backoff, the connection-parameter rule, the
# per-unit name, the pairing window, pages 80/81 and the DIS strings from one
# record, the strict-identity checks and the identity-fallback decision.
#
# The two source files are compiled unmodified, exactly as radiant/CMakeLists.txt
# compiles them into an image with CONFIG_RADIANT_PRODUCT / CONFIG_RADIANT_BLE.
# The Zephyr glue (radiant_ble_adv.c and friends) needs a Bluetooth host and
# is covered by the build rows and the bench, not here.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radiant_ble_tests)

target_sources(app PRIVATE
  src/test_ble_policy.c
  src/test_product.c
  ../../src/ble/radiant_ble_policy.c
  ../../src/ble/radiant_product.c
  ../../src/profiles/radiant_profile_common.c
)
target_include_directories(app PRIVATE ../../include)
