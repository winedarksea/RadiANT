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
)

# The antr_* adapter. Kconfig-gated because it is the one file here that
# includes an application header; see RADIANT_CORE_ANTR_API.
zephyr_library_sources_ifdef(CONFIG_RADIANT_CORE_ANTR_API src/radiant_api.c)

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
    "radiant_core: the nrf backend is RECEIVE ONLY. radiant_radio_tx() returns "
    "ENOTSUP because struct radiant_tx_req carries no on-air address - see the "
    "header comment in src/radiant_radio_nrf.c. This image will hear real "
    "sensors and will not answer one, so acknowledged data, burst and any "
    "master channel cannot work.")
elseif(CONFIG_RADIANT_CORE_BACKEND_MPSL)
  message(FATAL_ERROR
    "radiant_core: CONFIG_RADIANT_CORE_BACKEND_MPSL is selected but "
    "src/radiant_radio_mpsl.c does not exist - the MPSL-arbitrated backend is "
    "Phase 6b. See RADIANT_CORE_BACKEND_NRF's message for the alternatives.")
endif()
