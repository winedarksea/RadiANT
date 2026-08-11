# SPDX-License-Identifier: Apache-2.0
#
# radiant_core - the clean-room ANT-compatible link layer, as a Zephyr module.
#
# ── Why this file is a module and not a subdirectory ────────────────────────
#
# It used to be pulled in with add_subdirectory(radiant_core) from the
# application's own CMakeLists.txt, and listed its sources with
# target_sources(app ...). That worked, but it ruled out the library form: the
# supported way to say "this is a library" is zephyr_library(), and
# add_subdirectory() from an application puts this file in Zephyr's
# *application* mode, where it prints
#
#   zephyr_library() or zephyr_library_named() called in Zephyr CMake
#   application mode. `radiant_core` will not be treated as a Zephyr library.
#
# and then compiles nothing at all. The build still reached the link and still
# failed on the same undefined antr_* references, so the symptom was identical
# to a correct build - the only way to notice was that no object file existed.
# A library form that silently drops every source it was given is exactly the
# kind of green light that means nothing, and it is recorded here because this
# file is the resolution of it, not because the trap is still live.
#
# The resolution is module shape: zephyr/module.yml plus an entry in
# ZEPHYR_EXTRA_MODULES. That is how every protocol stack in this ecosystem
# ships - OpenThread, LoRaMac-node, the Wi-Fi supplicant, and sdk-ant itself,
# the thing being replaced. It also means the source list lives here and only
# here: the unit-test application used to keep a duplicate copy of it, and a
# duplicate that silently stops growing is a suite that stops testing the
# module it names.
#
# The application always appends this module and CONFIG_RADIANT_CORE gates it,
# rather than appending it only for -DANT_RADIO=core. An sdk_ant build can then
# assert CONFIG_RADIANT_CORE=n positively; the module's mere absence proves
# nothing. The guard below is what makes that gate real - with it, an sdk_ant
# build cannot reach a <radiant_core/...> header even by accident.

if(NOT CONFIG_RADIANT_CORE)
  return()
endif()

# include/ carries the namespaced <radiant_core/...> headers.
#
# ../src is here for src/radiant_event.c and src/radiant_api.c alone. Their
# *output* format is struct antr_msg and antr_on_message(), which live in
# src/ant_radio.h and pull in src/ant_wire.h - the generated header every
# ANTW_* value in radiant_core comes from. Both are plain headers with no
# application dependency, so this costs the build nothing. Note the asymmetry
# deliberately: radiant_event.c is the only *module* allowed to know the wire
# format, because it is the only one that produces it. Everything below it
# speaks frames and channels.
zephyr_include_directories(include ${CMAKE_CURRENT_SOURCE_DIR}/../src)

zephyr_library()
zephyr_library_sources(
  src/radiant_frame.c
  src/radiant_sched.c
  src/radiant_channel.c
  src/radiant_search.c
  src/radiant_event.c
  src/radiant_ack.c
  src/radiant_burst.c
  src/radiant_noise.c
)

# Single-bit CRC repair. Compiled unconditionally rather than behind its own
# Kconfig symbol, and the ~1 KB it costs is worth what it buys: the unit-test
# application can exercise the arithmetic whether or not a given build turns
# the feature on, so the syndrome table's duplicate check runs in CI on every
# build instead of only on the ones that opted in. CONFIG_RADIANT_CORE_CRC_REPAIR
# gates the CALL SITE in src/radiant_api.c, which is where the false-accept
# trade-off is actually taken.
zephyr_library_sources(src/radiant_crc_repair.c)

# The third refutation for a repaired frame: is the payload a physically
# possible reading, for the handful of device types where that is decidable
# from one frame. Compiled unconditionally for the same reason as
# radiant_crc_repair.c above; CONFIG_RADIANT_CORE_PROFILE_SANITY gates the
# call site in src/radiant_api.c.
zephyr_library_sources(src/radiant_profile_sanity.c)

# The antr_* adapter. Kconfig-gated because it is the one file here that
# includes an application header; see RADIANT_CORE_ANTR_API.
zephyr_library_sources_ifdef(CONFIG_RADIANT_CORE_ANTR_API src/radiant_api.c)

# ── Security ────────────────────────────────────────────────────────────────
#
# Gated, unlike radiant_crc_repair.c above, and the difference is deliberate.
# CRC repair is compiled unconditionally so CI exercises its arithmetic on
# every build; security is not, because the governing constraint here is that
# almost nobody turns it on and it must cost those users LITERALLY ZERO - which
# the `zero-cost` CI job asserts section by section. A file compiled "so the
# tests can reach it" would fail that job on the day it landed.
#
# The unit-test application reaches the primitives by selecting
# CONFIG_RADIANT_SEC=y in its own prj.conf, which is the same door a real
# application uses.
zephyr_library_sources_ifdef(CONFIG_RADIANT_SEC src/radiant_sec_aes.c)
zephyr_library_sources_ifdef(CONFIG_RADIANT_SEC src/radiant_sec.c)

# ── The radio backend ───────────────────────────────────────────────────────
#
# One of these, or none, and never two - see the choice in Kconfig. The two
# unwritten ones are a configure-time error naming their phase, because loud
# beats a link error whose cause is three layers away.
if(CONFIG_RADIANT_CORE_BACKEND_NULL)
  zephyr_library_sources(src/radiant_radio_null.c)
elseif(CONFIG_RADIANT_CORE_BACKEND_NRF)
  zephyr_library_sources(src/radiant_radio_nrf.c)
  message(WARNING
    "radiant_core: the nrf backend transmits and receives. Its ramp-up and "
    "turnaround figures are datasheet values, and the two t_sync calibration "
    "constants are SEEDED from Zephyr's per-SoC BLE chain delays rather than "
    "measured on this bench - a defensible prior at a different deviation, not "
    "the wired two-board trigger the HAL contract asks for. Every register "
    "fact is measured on nRF54L15 only. Expect timing to be approximately "
    "calibrated rather than exact - see the 'what is measured and what is not' "
    "header in src/radiant_radio_nrf.c before reading any A/B timing result "
    "off this build.")
elseif(CONFIG_RADIANT_CORE_BACKEND_MPSL)
  message(FATAL_ERROR
    "radiant_core: CONFIG_RADIANT_CORE_BACKEND_MPSL is selected but "
    "src/radiant_radio_mpsl.c does not exist - the MPSL-arbitrated backend is "
    "Phase 6b. See RADIANT_CORE_BACKEND_NRF's message for the alternatives.")
endif()
