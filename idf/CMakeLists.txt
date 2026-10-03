cmake_minimum_required(VERSION 3.16)

# ESP-IDF project for the ESP32-S31-Korvo-1.
#
# This is a SECOND build system alongside the fork's original PlatformIO one, not a replacement:
# platformio.ini and everything it drives are untouched. Only the S31 is built from here.
#
# The KryonOS sources stay where PlatformIO expects them (../src). Components under ./components
# read from that tree; nothing here writes to it.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

if(NOT DEFINED ENV{IDF_PATH})
    message(FATAL_ERROR
        "IDF_PATH is not set. Run an ESP-IDF export script first, e.g.\n"
        "  C:\\esp\\v6.1\\esp-idf\\export.ps1")
endif()

project(kryonos)

# Which program this board builds. Both are real, and both are useful.
#
#   OFF (default)  the operating system: the fork's own setup()/loop() from ../src/main.cpp. This is
#                  what the port is for.
#   ON             the M2/M3 bring-up diagnostic -- eight colour bars, the primitive set, three fonts
#                  and three datums, plus polled touch logging and a heartbeat. It is how the panel,
#                  the rasterizer, the text metrics and the touch driver were each verified on real
#                  silicon, and it is the thing to come back to when a display or touch fault needs
#                  to be told apart from an OS fault.
#
# The switch exists because the two programs define the same symbols -- setup() and loop() -- so
# exactly one of them can be compiled. ../src/main.cpp is therefore excluded from the app component
# when this is ON, which is the only thing the flag does.
#
# Set it with: idf.py -DKRYONOS_KORVO_BRINGUP=1 build
option(KRYONOS_KORVO_BRINGUP "Build the M2/M3 display+touch diagnostic instead of the OS" OFF)

if(KRYONOS_KORVO_BRINGUP)
    # A cache variable, not a plain one: components are configured after this file, and only cache
    # variables are reliably visible to them.
    set(KRYONOS_KORVO_BRINGUP "${KRYONOS_KORVO_BRINGUP}" CACHE STRING "" FORCE)
    message(STATUS "Korvo-1: building the bring-up diagnostic, not the OS")
else()
    message(STATUS "Korvo-1: building KryonOS")
endif()

# Third-party components in arduino-esp32's dependency tree do not compile clean under GCC 15, which
# IDF 6.x runs with default warnings promoted to errors. The breakage is not ours and the sources are
# not ours to patch, so the warnings are demoted here rather than fixed.
#
# Each flag is deliberately narrow, one warning at a time, rather than a blanket -Wno-error. A blanket
# opt-out would also disarm -Werror for KryonOS's own sources, and that check is worth more than the
# convenience -- especially through the rest of this port, where the compiler catching a real mistake
# is one of the few cheap signals available.
#
# esp-modbus (reached via arduino-esp32) trips this one:
#   port_tcp_utils.c:685: strncpy(temp_str, *dns_name, strlen(*dns_name))
# The reference project for this board suppresses the same warning the same way. Add further flags
# here, individually, as the build surfaces them.
idf_build_set_property(COMPILE_OPTIONS "-Wno-error=stringop-truncation" APPEND)
