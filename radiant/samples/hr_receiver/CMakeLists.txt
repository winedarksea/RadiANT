# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# This sample lives inside the module, so it finds the module by relative path.
# That keeps it building in a workspace that does not list RadiANT as a west
# project. Under west, where the repository root's zephyr/module.yml already
# provides the module, this is harmless: both entries are named radiant and
# name the same directory. In your own project, a west project entry is enough;
# without west, point this line at wherever radiant/ is.
list(APPEND ZEPHYR_EXTRA_MODULES "${CMAKE_CURRENT_SOURCE_DIR}/../..")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(hr_receiver)

target_sources(app PRIVATE src/main.c)
