# SPDX-License-Identifier: Apache-2.0
#
# The self-channel binding record's codec (src/binding_store.h), under test.
#
# One production file beside one test file: binding_store_codec.c calls no
# Zephyr and no settings API, which is the reason it is split from
# binding_store.c. The settings I/O needs a flash backend and the self-channel
# thread; the byte layout is the part that can be tested here.
#
# On Windows there is no native_sim; use
#   scripts\run_ztest_hw.ps1 -App apps\dongle_thread\tests\binding_store
# on a DK.

cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(binding_store_test)

target_sources(app PRIVATE
  src/main.c
  ../../src/binding_store_codec.c
)
target_include_directories(app PRIVATE
  ../../src
  ../../../../radiant/include
)
