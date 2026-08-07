cmake_minimum_required(VERSION 3.20.0)

# Location of the sdk-ant Zephyr module. Override on the command line with
#   -DANT_MODULE_DIR=<path>
# It supplies both the ANT headers (include/, init/) and the prebuilt libant.a.
# Stage-1 stub builds still need the headers; only the radio is faked out.
set(ANT_MODULE_DIR "C:/Users/Colin/sdk-ant" CACHE PATH
    "Path to the sdk-ant Zephyr module (the directory holding zephyr/module.yml)")

if(EXISTS "${ANT_MODULE_DIR}/zephyr/module.yml")
  message(STATUS "ant_dongle: sdk-ant module at ${ANT_MODULE_DIR}")
  list(APPEND ZEPHYR_EXTRA_MODULES "${ANT_MODULE_DIR}")
else()
  message(FATAL_ERROR
    "ant_dongle: no sdk-ant module at '${ANT_MODULE_DIR}'.\n"
    "The ANT serial bridge compiles against sdk-ant's headers even in a stub "
    "build, so the checkout is required either way. Pass "
    "-DANT_MODULE_DIR=<path> pointing at the directory that contains "
    "zephyr/module.yml.")
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(ant_dongle)

target_sources(app PRIVATE
  src/main.c
  src/usb_ant_class.c
  src/ant_serial_bridge.c
)

if(CONFIG_ANT_DONGLE_FLASH_LOG)
  target_sources(app PRIVATE src/diag_flash_log.c)
endif()

if(CONFIG_ANT_DONGLE_RADIO_STUB)
  # With CONFIG_ANT=n the module's own CMakeLists adds neither its include
  # directories nor libant.a. The bridge still compiles against the real ANT
  # headers, so pull them in by hand and let src/ant_stub.c supply the entry
  # points libant.a would otherwise provide.
  target_include_directories(app PRIVATE
    "${ANT_MODULE_DIR}/include"
    "${ANT_MODULE_DIR}/init"
  )
  target_sources(app PRIVATE src/ant_stub.c)
endif()
