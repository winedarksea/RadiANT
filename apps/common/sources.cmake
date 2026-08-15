# SPDX-License-Identifier: Apache-2.0
#
# ant_app_common: the antr_* seam and everything downstream of it that every
# ANT-serial RadiANT application needs, unchanged, once the radio backend
# question (radiant module vs. sdk-ant vs. stub) is settled.
#
# include()d - NOT add_subdirectory()'d - from each consuming application's
# own CMakeLists.txt (apps/dongle, apps/dongle_thread, apps/hrm_ble), and
# every source below is added straight onto that application's own `app`
# target via target_sources_ifdef(), not collected into a library of its own.
#
# THAT IS NOT INCIDENTAL. zephyr_library()/zephyr_library_named() only
# register a library with the rest of the Zephyr build during Zephyr's own
# "kernel CMake mode" - the phase in which modules named in
# ZEPHYR_EXTRA_MODULES are processed, before an application's CMakeLists.txt
# ever runs. An application's own CMakeLists.txt runs in "application CMake
# mode" instead, and calling zephyr_library_named() from there (whether
# directly or via add_subdirectory()) prints a CMake warning - "will not be
# treated as a Zephyr library" - and silently produces a plain static archive
# that never gets the syscall linkage Zephyr gives its own libraries. The
# first casualty was usb_ant_class.c: it uses k_msgq_get()/_put()/_purge(),
# and the resulting image failed to link with "undefined reference to
# z_impl_k_msgq_get" - a syscall wrapper that exists in every ordinary Zephyr
# build and only goes missing when the calling library was built this way.
# include()ing this file into the caller's own scope and building straight
# into `app` sidesteps the whole distinction: `app` is always in application
# mode already, by definition.
#
# There is no single build gluing the applications together to link against:
# each is its own `west build`, its own project(), so "shared" here means
# "one file admins one list", not "one binary artifact reused across images".
#
# WHY THIS IS NOT PART OF THE radiant MODULE: every file here reaches an
# application header (src/ant_radio.h's antr_* declarations) or IS one - the
# module's own self-containment rule (radiant/CMakeLists.txt reaches no path
# outside itself) is exactly what this directory is on the other side of.
# ant_app_common is the seam; radiant is what it adapts onto.

# ant/ carries ant_radio.h and ant_wire.h - the antr_* contract and the wire
# constants every backend and the bridge itself need in scope. PUBLIC (via
# target_include_directories(app ...)) so a consuming application's own
# sources (radiant-adjacent test doubles, apps/dongle_ti's not-yet-written
# backend) can `#include "ant_radio.h"` too, and so files in this directory
# that spell the bare filename (no "ant/" prefix - see ant_serial_bridge.c)
# resolve without every file needing its own relative path back into the
# subdirectory.
target_include_directories(app PUBLIC
  "${CMAKE_CURRENT_LIST_DIR}"
  "${CMAKE_CURRENT_LIST_DIR}/ant"
)

# ── The antr_* adapter onto radiant ──────────────────────────────────────
#
# Unconditional, unlike everything below it: every consumer of this file that
# selects the radiant core backend (ANT_RADIO=core, or apps/hrm_ble's
# unconditional CONFIG_RADIANT=y) needs it, and CONFIG_RADIANT_ANTR_API
# defaults to y precisely so that is the common case. See
# apps/common/Kconfig.antr_api's own header for why the symbol exists at all
# rather than an unconditional target_sources() call.
target_sources_ifdef(CONFIG_RADIANT_ANTR_API app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/ant_radio_radiant.c")
target_sources_ifdef(CONFIG_RADIANT_SEC_HOST_MESSAGES app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/ant_sec_host.c")

# The other two antr_* implementations. Neither reaches the radiant module at
# all; CONFIG_ANT_DONGLE_RADIO_* is what CMake's ANT_RADIO axis
# (apps/common/ant_radio_backend.cmake) writes into the generated fragment,
# so these two and the two lines above are mutually exclusive by
# construction, not by convention.
target_sources_ifdef(CONFIG_ANT_DONGLE_RADIO_SDK_ANT app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/ant_radio_sdk_ant.c")
target_sources_ifdef(CONFIG_ANT_DONGLE_RADIO_STUB app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/ant_radio_stub.c")

# ── The serial bridge and its transports ─────────────────────────────────
#
# apps/hrm_ble never sets any CONFIG_ANT_DONGLE_TRANSPORT_* symbol - its
# Kconfig tree never sources the choice that defines them - so all four of
# these lines are silently inert there, and ant_serial_bridge.c along with
# every transport stays out of that image entirely, the same way it always
# has. The choice itself guarantees at most one of the three fires for any
# application that does select it.
target_sources_ifdef(CONFIG_ANT_DONGLE_TRANSPORT_USB_LEGACY app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/ant_serial_bridge.c" "${CMAKE_CURRENT_LIST_DIR}/usb_ant_class.c")
target_sources_ifdef(CONFIG_ANT_DONGLE_TRANSPORT_USB_NEXT app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/ant_serial_bridge.c" "${CMAKE_CURRENT_LIST_DIR}/usb_ant_class_next.c")
target_sources_ifdef(CONFIG_ANT_DONGLE_TRANSPORT_UART app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/ant_serial_bridge.c" "${CMAKE_CURRENT_LIST_DIR}/ant_uart_transport.c")

target_sources_ifdef(CONFIG_ANT_DONGLE_FLASH_LOG app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/diag_flash_log.c")

# ── The shared boot sequence ─────────────────────────────────────────────
#
# apps/dongle and apps/dongle_thread ship the same main(), so they share one
# file rather than two copies that must not diverge - see ant_dongle_main.h.
# CONFIG_ANT_DONGLE_MAIN comes from apps/common/Kconfig.dongle_main, which
# only those two applications rsource, so this line is inert in apps/hrm_ble
# and apps/dongle_ti exactly the way the transport lines above are.
target_sources_ifdef(CONFIG_ANT_DONGLE_MAIN app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/ant_dongle_main.c")

# ── The 0xF6 health counters ─────────────────────────────────────────────
#
# Off by default so the `zero-cost` CI job stays a real check - see
# apps/common/Kconfig.health. With the symbol off this file is not compiled
# and every ant_health_note() in the tree is a macro expanding to nothing, so
# there is no empty function to call and no argument to evaluate.
target_sources_ifdef(CONFIG_ANT_DONGLE_HEALTH_COUNTERS app PRIVATE
  "${CMAKE_CURRENT_LIST_DIR}/ant_health.c")
