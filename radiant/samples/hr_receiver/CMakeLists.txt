# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# This sample lives inside the module, so it finds the module by relative path.
# In your own project, point this at wherever RadiANT's radiant/ directory is.
list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../..")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hr_receiver)

target_sources(app PRIVATE src/main.c)
