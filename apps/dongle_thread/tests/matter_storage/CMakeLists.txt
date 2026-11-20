# SPDX-License-Identifier: Apache-2.0
#
# The Matter bridge's stored-record decoder (security review 2026-11-20
# item 2), under test.
#
# One production file beside one test file: bridge_storage_codec.cpp includes
# no CHIP, no Zephyr and no persistent-storage header, which is the reason it
# was split out of the vendored bridge_storage_manager.cpp. The CHIP stack does
# not build on native_sim, so the decoder is the part that can be tested.
#
# On Windows there is no native_sim; use
#   scripts\run_ztest_hw.ps1 -App apps\dongle_thread\tests\matter_storage
# on a DK.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(matter_storage_test)

target_sources(app PRIVATE
  src/main.cpp
  ../../src/matter/bridge_storage_codec.cpp
)
target_include_directories(app PRIVATE
  ../../src/matter
)
