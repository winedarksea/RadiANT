cmake_minimum_required(VERSION 3.20.0)

# The sysbuild-cache reader and the RADIANT_BACKEND selection/assertion
# machinery are shared with every other RadiANT app (strap/ today; more as
# they land) rather than copy-pasted - see that file for the sysbuild
# mechanics this depends on.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/radiant_backend.cmake")

# ── Where sdk-ant is ────────────────────────────────────────────────────────
#
# Location of the sdk-ant Zephyr module. Override on the command line with
#   -DANT_MODULE_DIR=<path>
# It supplies both the ANT headers (include/, init/) and the prebuilt libant.a.
# Only the sdk_ant backend needs it; core and stub builds never look at it.
#
# The default is resolved rather than hardcoded, because sdk-ant is a private,
# non-redistributable checkout that lands wherever its owner put it:
#   1. $ENV{SDK_ANT_DIR} if the environment names one. This is the per-machine
#      setting - set it once in a shell profile and every build in the tree
#      picks it up, including sim/ and any out-of-tree consumer.
#   2. Otherwise a sibling checkout next to this repository. That is where
#      west.yml places it (<topdir>/sdk-ant, in the `ant` group) and where a
#      manual `git clone` alongside this repository naturally lands, so the
#      common case needs no configuration at all.
# Neither is a guess about a particular user's home directory.
radiant_sysbuild_value(ANT_MODULE_DIR ANT_MODULE_DIR_SYSBUILD)
if(ANT_MODULE_DIR_SYSBUILD)
  # FORCE, because sysbuild is the outer authority: on a re-configure our own
  # cache entry from the previous run would otherwise win over a changed -D.
  set(ANT_MODULE_DIR "${ANT_MODULE_DIR_SYSBUILD}" CACHE PATH
      "Path to the sdk-ant Zephyr module (the directory holding zephyr/module.yml)" FORCE)
else()
  if(DEFINED ENV{SDK_ANT_DIR})
    file(TO_CMAKE_PATH "$ENV{SDK_ANT_DIR}" ANT_MODULE_DIR_DEFAULT)
  else()
    set(ANT_MODULE_DIR_DEFAULT "${CMAKE_CURRENT_SOURCE_DIR}/../sdk-ant")
  endif()

  set(ANT_MODULE_DIR "${ANT_MODULE_DIR_DEFAULT}" CACHE PATH
      "Path to the sdk-ant Zephyr module (the directory holding zephyr/module.yml)")
endif()

if(EXISTS "${ANT_MODULE_DIR}/zephyr/module.yml")
  set(ANT_MODULE_PRESENT TRUE)
else()
  set(ANT_MODULE_PRESENT FALSE)
endif()

# ── Which radio backend ─────────────────────────────────────────────────────
#
# CMake decides, Kconfig mirrors. This is not a preference: appending to
# ZEPHYR_EXTRA_MODULES has to happen before find_package(Zephyr), therefore
# before Kconfig exists at all, so a Kconfig symbol cannot decide whether
# sdk-ant's Kconfig gets sourced. The decision is made here and written back
# out as CONFIG_ANT_DONGLE_RADIO_* below, so that .config records which
# backend was compiled and CI can assert on it.
#
# The default follows what is on disk, so that neither of the two normal
# situations needs a flag: with an sdk-ant checkout resolved, `sdk_ant` is the
# proven build and the one release artifacts come from; with no checkout - a
# fork, a fresh clone, a CI job with no secret - `core` is the only honest
# answer, and it fails at the link rather than at a path that was never the
# user's fault.
# radiant_core is appended unconditionally, and the asymmetry with sdk-ant is
# deliberate. sdk-ant is appended only for -DANT_RADIO=sdk_ant because it may
# not be on disk at all; radiant_core is in this repository and always is. So
# it is always a module and CONFIG_RADIANT_CORE gates it, which is both the
# ordinary Zephyr idiom and a better test: an sdk_ant build then asserts
# CONFIG_RADIANT_CORE=n positively, rather than proving nothing by the module's
# absence. radiant_core/CMakeLists.txt returns immediately when the symbol is
# n, so an sdk_ant build cannot reach a <radiant_core/...> header either.
#
# The HAL backend is resolved here too (radiant_select_backend(), from
# cmake/radiant_backend.cmake), even though it is only used below when
# ANT_RADIO=core: resolving it once, unconditionally, is simpler than
# threading it through both ANT_RADIO branches, and a stray RADIANT_BACKEND
# cache entry on an sdk_ant/stub build is harmless since nothing reads it.
radiant_select_backend("${CMAKE_CURRENT_SOURCE_DIR}/radiant_core" "null" RADIANT_BACKEND_CONF)

radiant_sysbuild_value(ANT_RADIO ANT_RADIO_SYSBUILD)
if(ANT_RADIO_SYSBUILD)
  set(ANT_RADIO "${ANT_RADIO_SYSBUILD}" CACHE STRING
      "ANT radio backend: sdk_ant, core or stub" FORCE)
else()
  if(ANT_MODULE_PRESENT)
    set(ANT_RADIO_DEFAULT "sdk_ant")
  else()
    set(ANT_RADIO_DEFAULT "core")
  endif()
  set(ANT_RADIO "${ANT_RADIO_DEFAULT}" CACHE STRING
      "ANT radio backend: sdk_ant, core or stub")
endif()
set_property(CACHE ANT_RADIO PROPERTY STRINGS sdk_ant core stub)

if(NOT ANT_RADIO MATCHES "^(sdk_ant|core|stub)$")
  message(FATAL_ERROR
    "ant_dongle: ANT_RADIO='${ANT_RADIO}' is not a radio backend.\n"
    "Valid values are:\n"
    "  sdk_ant  Nordic's prebuilt libant.a from sdk-ant, through the thin\n"
    "           forwarders in src/ant_radio_sdk_ant.c. Needs an sdk-ant\n"
    "           checkout; see ANT_MODULE_DIR.\n"
    "  core     the clean-room radiant_core stack. No sdk-ant of any kind.\n"
    "  stub     src/ant_radio_stub.c - accepts everything, transmits nothing.\n"
    "Pass one with -DANT_RADIO=<value>.")
endif()

if(ANT_RADIO STREQUAL "sdk_ant")
  if(ANT_MODULE_PRESENT)
    message(STATUS "ant_dongle: radio backend sdk_ant, module at ${ANT_MODULE_DIR}")
    list(APPEND ZEPHYR_EXTRA_MODULES "${ANT_MODULE_DIR}")
  else()
    message(FATAL_ERROR
      "ant_dongle: ANT_RADIO=sdk_ant but no sdk-ant module at "
      "'${ANT_MODULE_DIR}'.\n"
      "Either set the SDK_ANT_DIR environment variable to the sdk-ant "
      "checkout (the per-machine setting, picked up by this application and "
      "by sim/), or pass -DANT_MODULE_DIR=<path> for a single build. Both "
      "must point at the directory that contains zephyr/module.yml.\n"
      "With neither set, the default is a sibling checkout at "
      "'${CMAKE_CURRENT_SOURCE_DIR}/../sdk-ant'. In a west workspace, "
      "`west config manifest.group-filter +ant && west update` puts it "
      "there.\n"
      "If you do not have sdk-ant at all, that is a supported configuration: "
      "build with -DANT_RADIO=core or -DANT_RADIO=stub and nothing here will "
      "look for it.")
  endif()
else()
  # Nothing to do, and that is the point. The module is never appended, so
  # sdk-ant's Kconfig is never sourced, CONFIG_ANT never comes into existence,
  # and there is no CONFIG_ANT=n to set anywhere - a symbol that does not exist
  # cannot be assigned to, and Zephyr treats assigning to one as fatal.
  message(STATUS "ant_dongle: radio backend ${ANT_RADIO}, no sdk-ant module")
endif()

# ── Telling Kconfig what CMake decided ──────────────────────────────────────
#
# A generated fragment rather than a line in prj.conf, for two reasons. The
# backend is not known until the block above has run, and the CONFIG_ANT*
# settings name symbols that only exist when sdk-ant's Kconfig was sourced -
# stating them unconditionally in prj.conf would make every core or stub build
# fail on an assignment to an undefined symbol.
#
# It lands in the application binary directory because Zephyr globs *.conf
# there and merges the result *after* everything else, including
# -DEXTRA_CONF_FILE. That ordering is what makes this authoritative: the
# backend CMake actually compiled and the backend Kconfig records cannot
# disagree.
if(ANT_RADIO STREQUAL "sdk_ant")
  set(ANT_RADIO_CONF
"# Generated by CMakeLists.txt from -DANT_RADIO. Do not edit; do not commit.
CONFIG_ANT_DONGLE_RADIO_SDK_ANT=y

# ANT stack - 8 channels gives Zwift headroom for HRM + power + cadence + extras
CONFIG_ANT=y
CONFIG_ANT_EVALUATION_KEY=y
CONFIG_ANT_TOTAL_CHANNELS_ALLOCATED=8
CONFIG_ANT_BURST_QUEUE_SIZE=256
")
elseif(ANT_RADIO STREQUAL "core")
  # RADIANT_BACKEND and its Kconfig-fragment text (RADIANT_BACKEND_CONF) were
  # already resolved above by radiant_select_backend(), unconditionally, so
  # that both ANT_RADIO branches above it could stay untouched by the change.
  set(ANT_RADIO_CONF
"# Generated by CMakeLists.txt from -DANT_RADIO. Do not edit; do not commit.
CONFIG_ANT_DONGLE_RADIO_CORE=y

# radiant_core and its HAL backend, from -DRADIANT_BACKEND.
CONFIG_RADIANT_CORE=y
${RADIANT_BACKEND_CONF}
")
else()
  set(ANT_RADIO_CONF
"# Generated by CMakeLists.txt from -DANT_RADIO. Do not edit; do not commit.
CONFIG_ANT_DONGLE_RADIO_STUB=y
")
endif()

# Write only on a change. Zephyr adds every merged fragment to
# CMAKE_CONFIGURE_DEPENDS, so rewriting an identical file each configure would
# hand ninja a freshly-stamped dependency of the configure step it just ran.
set(ANT_RADIO_CONF_FILE "${CMAKE_CURRENT_BINARY_DIR}/ant_radio_backend.conf")
set(ANT_RADIO_CONF_EXISTING "")
if(EXISTS "${ANT_RADIO_CONF_FILE}")
  file(READ "${ANT_RADIO_CONF_FILE}" ANT_RADIO_CONF_EXISTING)
endif()
if(NOT ANT_RADIO_CONF_EXISTING STREQUAL ANT_RADIO_CONF)
  file(WRITE "${ANT_RADIO_CONF_FILE}" "${ANT_RADIO_CONF}")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_dongle)

# ── The backend actually compiled is the backend that was asked for ──────────
#
# See cmake/radiant_backend.cmake's radiant_assert_backend() for the full
# rationale and what it has caught before (twice, on the Nordic side). Only
# relevant when ANT_RADIO chose radiant_core in the first place.
if(ANT_RADIO STREQUAL "core")
  radiant_assert_backend("ant_dongle")
endif()

target_sources(app PRIVATE
  src/main.c
  src/ant_serial_bridge.c
)

# Exactly one transport, chosen by Kconfig. All three implement
# src/ant_transport.h, so the bridge and main() do not know which is present.
if(CONFIG_ANT_DONGLE_TRANSPORT_USB_LEGACY)
  target_sources(app PRIVATE src/usb_ant_class.c)
elseif(CONFIG_ANT_DONGLE_TRANSPORT_USB_NEXT)
  target_sources(app PRIVATE src/usb_ant_class_next.c)
elseif(CONFIG_ANT_DONGLE_TRANSPORT_UART)
  target_sources(app PRIVATE src/ant_uart_transport.c)
else()
  message(FATAL_ERROR "ant_dongle: no transport selected")
endif()

if(CONFIG_ANT_DONGLE_FLASH_LOG)
  target_sources(app PRIVATE src/diag_flash_log.c)
endif()

# The bench instrument for the coexistence gate: a BLE advertiser that competes
# for the radio, so the gate has a second stack that wants air rather than one
# that is merely linked in. See src/ble_coex_load.c.
if(CONFIG_ANT_DONGLE_BLE_COEX_LOAD)
  target_sources(app PRIVATE src/ble_coex_load.c)
endif()

# The same instrument for the other stack: the P4 gate's Thread load. Separate
# symbol and separate file rather than a mode of the BLE one, because the two
# are legal together - a build with both is the three-stack case - and because
# the failure they guard against differs in kind: a linked-but-asleep controller
# versus an attached-but-silent sleepy child. See src/thread_coex_load.c.
if(CONFIG_ANT_DONGLE_THREAD_COEX_LOAD)
  target_sources(app PRIVATE src/thread_coex_load.c)
endif()

# ── The hostless node's identity (compat-C6) ────────────────────────────────
#
# docs/decisions/0009-hostless-node-identity.md. Default off and off in every
# dongle build: a dongle's epoch authority is the PC behind it, over 0xF3, and
# its pairing scalar arrives over 0xF5 - it wants none of this. The symbol and
# the wiring live here so that a node application built from this tree has a
# home for them, and so the storage backend is selected the same way every other
# backend in this project is.
#
# The counters' storage is a seam (radiant_core/src/node/node_nvm.h) with no
# default implementation, which is why the backend is a separate condition
# rather than being folded into the one above: a build that selects the
# identity and no backend must fail at the link, not fall back to RAM.
#
# radiant_core/src/node, not src/node: the files moved into the module
# directory (radiant_core reaches no path outside itself, and this pair
# always did), but the decision to compile them belongs to this application,
# same as before the move - nothing in radiant_core/CMakeLists.txt reads
# these two symbols.
if(CONFIG_RADIANT_NODE_IDENT)
  target_sources(app PRIVATE radiant_core/src/node/node_ident.c)
  target_include_directories(app PRIVATE radiant_core/src/node)
endif()
if(CONFIG_RADIANT_NODE_NVM_SETTINGS)
  target_sources(app PRIVATE radiant_core/src/node/node_nvm_settings.c)
endif()

# Exactly one radio backend, chosen by ANT_RADIO above. All three implement
# src/ant_radio.h, so the bridge and main() do not know which is present -
# the same shape as the transport choice, deliberately.
#
# Each is guarded on existing, and a missing one is a warning rather than a
# hard error, because the three implementations arrive at different times: the
# stub and the sdk-ant shim in this same wave, radiant_core in Phase 5. A build
# whose backend has not been written yet must still configure and still reach
# the link, because "it fails with 42 undefined antr_* symbols" is a useful
# answer and "CMake cannot find a source file" is not - it hides every other
# problem behind the first missing file.
if(ANT_RADIO STREQUAL "sdk_ant")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ant_radio_sdk_ant.c")
    target_sources(app PRIVATE src/ant_radio_sdk_ant.c)
  else()
    message(WARNING
      "ant_dongle: ANT_RADIO=sdk_ant but src/ant_radio_sdk_ant.c does not "
      "exist. libant.a is linked, but nothing forwards src/ant_radio.h onto "
      "it, so the link will fail with undefined antr_* references.")
  endif()
elseif(ANT_RADIO STREQUAL "stub")
  if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/src/ant_radio_stub.c")
    target_sources(app PRIVATE src/ant_radio_stub.c)
  else()
    message(WARNING
      "ant_dongle: ANT_RADIO=stub but src/ant_radio_stub.c does not exist. "
      "The link will fail with undefined antr_* references.")
  endif()
else()
  # radiant_core is a Zephyr module appended to ZEPHYR_EXTRA_MODULES above and
  # switched on by CONFIG_RADIANT_CORE in the generated fragment; it owns its
  # own source list, its own Kconfig-selected HAL backend, and must stay
  # buildable outside this application (radiant_core/tests builds it with no
  # dongle in sight).
  #
  # What the module does NOT own any more is the antr_* adapter that sits
  # between it and this application: src/ant_radio_radiant.c (radiant_api.c,
  # before radiant_core stopped reaching outside itself) and
  # src/ant_sec_host.c (radiant_sec_host.c) are application sources now, gated
  # by CONFIG_RADIANT_CORE_ANTR_API / CONFIG_RADIANT_SEC_HOST_MESSAGES from
  # src/Kconfig.antr_api - the same symbols, the same default-on behaviour,
  # just declared where the files they gate actually live.
  target_sources_ifdef(CONFIG_RADIANT_CORE_ANTR_API app PRIVATE
    src/ant_radio_radiant.c)
  target_sources_ifdef(CONFIG_RADIANT_SEC_HOST_MESSAGES app PRIVATE
    src/ant_sec_host.c)
  message(STATUS "ant_dongle: radiant_core module supplies the antr_* backend")
endif()
