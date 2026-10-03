# The KryonOS application itself, compiled for the ESP32-S31-Korvo-1 under ESP-IDF.
#
# This is the step that turns "a working display and a working touch driver" into "the operating
# system". Everything under src/ is the fork's own code and is compiled here unchanged wherever
# possible -- that is the whole point of the port, and the reason the Arduino core is pulled in as an
# IDF component rather than the sources being rewritten against raw IDF APIs.
#
# THREE FILES ARE DELIBERATELY NOT COMPILED HERE, for two different reasons
#   main.cpp            the fork's setup()/loop() entry -- compiled here, and it IS the OS's entry
#                       point on this board: the Arduino core generates app_main and calls exactly one
#                       setup()/loop() pair. It is dropped only under KRYONOS_KORVO_BRINGUP, where
#                       idf/main/main.cpp supplies the other pair. Never both at once.
#   Hal/Touch/EspLcdTouchDriver.cpp         already compiled by kryonos_s31_touch
#   Hal/Display/RamFramebufferDisplay.cpp   already compiled by kryonos_s31_display
# Those two are compiled by the components that own their include paths and their defines, so
# compiling them again here would be a duplicate-symbol error, not a second copy.
#   Hal/Display/TftEspiDisplay.cpp          NOT the backend this board uses.
# The last one is different in kind: it is the TFT_eSPI backend, and on this target TFT_eSPI cannot
# drive the panel at all (it is RGB parallel -- see Documentation/Display_Touch_Architecture.md §5.2).
# What the IDF tree provides under that name is a rasterizer-only shim with no panel driver, so this
# file cannot compile here and nothing would call it if it did: KRYONOS_DISPLAY_BACKEND selects
# KorvoRgbDisplay. Its 29 errors are all the same missing-method error against that shim.
#
# Excluding it here rather than guarding the file itself keeps the fork's own sources byte-identical
# to what PlatformIO compiles: on the Waveshare environment a REAL TFT_eSPI is present and this file
# builds, and a guard that assumed otherwise would break that build to satisfy this one.
#
# The board files under src/Hal/Boards/ are all compiled, but each is guarded by its own positive
# TARGET_* macro and this build defines only TARGET_ESP32S31_KORVO1, so the other five expand to empty
# translation units. That is the same mechanism PlatformIO relies on.

set(KRYONOS_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src")

# No CONFIGURE_DEPENDS: ESP-IDF parses every component's CMakeLists once in CMake's script mode to
# collect requirements, and CONFIGURE_DEPENDS is invalid there -- it fails the configure outright rather
# than degrading. The cost is that adding a NEW source file to src/ needs a reconfigure (delete
# idf/build, or `idf.py reconfigure`); editing an existing one does not.
file(GLOB_RECURSE KRYONOS_APP_SRCS
     "${KRYONOS_SRC_DIR}/*.cpp"
     "${KRYONOS_SRC_DIR}/*.c")

list(REMOVE_ITEM KRYONOS_APP_SRCS
     "${KRYONOS_SRC_DIR}/Hal/Touch/EspLcdTouchDriver.cpp"
     "${KRYONOS_SRC_DIR}/Hal/Display/RamFramebufferDisplay.cpp"
     "${KRYONOS_SRC_DIR}/Hal/Display/TftEspiDisplay.cpp")

# The fork's setup()/loop() live in src/main.cpp, and they are the OS's entry point on this board.
# They are only dropped when the bring-up diagnostic is selected, because the two programs would
# otherwise both define setup() and loop() and the linker would see two of each. See the option in
# idf/CMakeLists.txt for why the diagnostic is kept at all.
if(KRYONOS_KORVO_BRINGUP)
    list(REMOVE_ITEM KRYONOS_APP_SRCS "${KRYONOS_SRC_DIR}/main.cpp")
endif()

idf_component_register(
    SRCS ${KRYONOS_APP_SRCS}
    INCLUDE_DIRS
        "${KRYONOS_SRC_DIR}"
        "${KRYONOS_SRC_DIR}/Hal/Boards/esp32s31-korvo1"
    REQUIRES
        arduino-esp32
        esp_psram
        esp_lcd
        kryonos_s31_display
        kryonos_s31_touch
        espressif__esp32_s31_korvo_1
        espressif__esp_lcd_touch
        # esp_ota_ops.h, for the OTA manager. Part of IDF rather than a managed component, so this is
        # the whole fix for that include -- unlike the ArduinoJson and mbedtls problems, which are not.
        app_update

        # The fork's two Arduino-only web libraries. Both are vendored under idf/components/ because
        # neither is published to the ESP-IDF component registry -- see their CMakeLists for why they
        # are copies and what, if anything, was changed in them.
        kryonos_async_tcp
        kryonos_async_webserver
        kryonos_websockets
)

# The fork is built with -Werror=format by the IDF toolchain and without it by PlatformIO, so a handful
# of printf calls that have always been warnings there are hard errors here. They are real warnings --
# %u against a size_t or a uint32_t -- and they are all benign on this target, where those types are
# the same width.
#
# Relaxed as warnings for this component instead of editing the call sites, because the goal is to port
# the fork, not to lint it: 20-odd edits to the fork's sources would be churn that PlatformIO's next
# build would re-check for no benefit, and any of them would make this build's sources differ from the
# one the other seven environments compile. The warnings still print, so they stay visible.
#
# format-truncation is the TimeManager's snprintf'd timestamp; its buffer is sized for the format and
# the compiler is being conservative about an int that cannot exceed four digits here.
#
# misleading-indentation is JSBindings::js_color(), which clamps three channels with two statements
# per line (`if (r < 0) r = 0; if (r > 255) r = 255;`). It reads exactly as it behaves; the warning
# is about the shape of the line, not the logic.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -Wno-error=format
    -Wno-error=format-truncation
    -Wno-error=misleading-indentation
)

# Which board this build is, and the display contract that goes with it. These are PRIVATE: they
# describe how THIS component's sources are compiled, and leaking them would let another component
# believe it is the Korvo-1 board.
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    TARGET_ESP32S31_KORVO1=1
    KRYONOS_DISPLAY_BACKEND=KRYONOS_BACKEND_RGB
    KRYONOS_DISPLAY_WIDTH=800
    KRYONOS_DISPLAY_HEIGHT=480
    KRYONOS_DISPLAY_ROTATION=0

    # The OS's own version, which on the PlatformIO side is supplied as a build flag rather than
    # declared in any header (platformio.ini, KRYONOS_VERSION and KRYONOS_API_LEVEL). Nothing in
    # src/ defines them, so a build that omits them here fails at every use site -- the installer,
    # the OTA manager and the app store all print them. Kept numerically identical to the
    # platformio.ini values, because the OTA manifest compares them: a Korvo-1 that reports a
    # different API level than the manifest advertises would find no updates.
    KRYONOS_VERSION=\"2.0.1\"
    KRYONOS_API_LEVEL=2
)
