cmake_minimum_required(VERSION 3.20.0)

# Include sdk-ant as an external module
set(ZEPHYR_EXTRA_MODULES ${ZEPHYR_EXTRA_MODULES} /Users/colincatlin/Documents-NoCloud/sdk-ant)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(simple_ant_dongle)

# Include additional ANT SDK directories
include_directories(/Users/colincatlin/Documents-NoCloud/sdk-ant/include)

# NORDIC SDK APP START
target_sources(app PRIVATE
  src/main.c
)
# NORDIC SDK APP END
zephyr_library_include_directories(.)
