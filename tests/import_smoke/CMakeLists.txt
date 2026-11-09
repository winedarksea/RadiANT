# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.20.0)

# The real proof radiant/ is importable: this application is built with
# RADIANT_MODULE_DIR pointing at a radiant/ checkout and NOTHING else from
# this repository on any include path. CI proves it for real by extracting
# radiant/ alone via `git archive` into a temp directory and pointing this
# variable there - see .github/workflows/ci.yml's import-smoke job. A
# reach-through this application or the module itself made past radiant/'s
# own boundary would fail to configure or fail to link in that mode, where a
# normal in-repo build (this file's own default below) would still succeed
# by accident, hiding the violation.
#
# No default that guesses a path outside this directory tree, on purpose:
# defaulting to "../../radiant" would make a normal in-repo build silently
# depend on the surrounding checkout's layout, which is exactly the kind of
# implicit dependency this test exists to catch. The default below is the
# ONE exception, and it stays inside what a plain `git clone` of this
# repository gives - the module two directories up - so `west build
# tests/import_smoke` works with no flags for a contributor working in the
# full tree, while CI's isolated run always passes the variable explicitly.
if(NOT DEFINED RADIANT_MODULE_DIR)
  set(RADIANT_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../radiant" CACHE PATH
      "Path to a radiant/ checkout (the directory holding zephyr/module.yml)")
endif()

if(NOT EXISTS "${RADIANT_MODULE_DIR}/zephyr/module.yml")
  message(FATAL_ERROR
    "tests/import_smoke: no radiant module at '${RADIANT_MODULE_DIR}' "
    "(no zephyr/module.yml there). Pass -DRADIANT_MODULE_DIR=<path> to a "
    "radiant/ checkout.")
endif()

list(APPEND ZEPHYR_EXTRA_MODULES "${RADIANT_MODULE_DIR}")

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(radiant_import_smoke)

target_sources(app PRIVATE src/main.c)

# The sample bus and HR decoder come from CONFIG_RADIANT_BRIDGE=y in prj.conf.
