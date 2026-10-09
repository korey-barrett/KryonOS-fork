# The ESP32-S31-Korvo-1 display stack for the IDF build.
#
# WHAT THIS COMPONENT IS NOW
#   A compile shim, not a home for code. Every source it builds lives in src/, because the PlatformIO
#   S31 environment needs the same files and there must be one copy of each. The component exists only
#   so the IDF build compiles them with the right IDF dependencies -- esp_lcd, esp_psram, esp_mm --
#   which the PlatformIO build gets from the Arduino core instead.
#
#   The backend (src/Hal/Display/KorvoRgbDisplay.cpp) used to live here and used to call Espressif's
#   BSP. It is now BSP-free: the panel's timings, pin map and frame buffer counts are vendored into it
#   from espressif__esp32_s31_korvo_1, because that component is an IDF-only dependency and this board
#   also has to build under PlatformIO/arduino-esp32. That is why espressif__esp32_s31_korvo_1 is no
#   longer REQUIRED here -- kryonos_s31_touch still needs it for the touch controller.
#
# WHY THE RASTERIZER MAY LIVE IN src/ WITHOUT COSTING THE OTHER BOARDS
#   src/ is compiled for every PlatformIO environment, and the font tables are 86 KB of source --
#   enough to matter on the CYD, which §5.2 left with 135,904 bytes of headroom. So KryonText.cpp,
#   KryonSprite.cpp and KorvoRgbDisplay.cpp guard their bodies on KRYONOS_KRYON_SPRITE and compile to
#   nothing elsewhere. That flag is set only by an environment with no real TFT_eSPI to draw text with.
#   The IDF build defines it too, since it likewise has no TFT_eSPI for this target.
#
# arduino-esp32 is required for Serial, String and ps_malloc, all of which the shared src/ code uses.

# Three levels up: idf/components/kryonos_s31_display -> idf/components -> idf -> <repo>, whose src/
# is where the shared display code lives.
set(KRYONOS_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src")

idf_component_register(
    SRCS
        "${KRYONOS_SRC_DIR}/Hal/Display/KorvoRgbDisplay.cpp"
        "${KRYONOS_SRC_DIR}/Hal/Display/KryonText.cpp"
        "${KRYONOS_SRC_DIR}/Hal/Display/KryonSprite.cpp"
        "${KRYONOS_SRC_DIR}/Hal/Display/RamFramebufferDisplay.cpp"
    INCLUDE_DIRS
        "${KRYONOS_SRC_DIR}"
        # For <TFT_eSPI.h>, which resolves to the shim beside the rasterizer rather than to the real
        # library, and for "KorvoRgbDisplay.h" / "KryonSprite.h".
        "${KRYONOS_SRC_DIR}/Hal/Display"
    REQUIRES
        arduino-esp32
        esp_psram
        esp_lcd
        # For esp_cache_msync, which present() calls to write the CPU's cached drawing back to the
        # PSRAM the RGB panel's DMA scans. Needed explicitly: esp_lcd and esp_psram both list esp_mm
        # as a PRIV_REQUIRES, and private requires do not propagate their include directories here.
        esp_mm
)
