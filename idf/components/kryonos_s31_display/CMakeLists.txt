# The ESP32-S31-Korvo-1 display stack: a KryonDisplay backend over Espressif's BSP, the rasterizer it
# draws into, and the TFT_eSPI text engine that rasterizer needs.
#
# WHY THIS IS A COMPONENT AND NOT FILES IN src/
#   Everything here is S31-only. PlatformIO compiles every src/*.cpp for all seven of its
#   environments, so a file under src/ would have to be guarded by a TARGET_* macro and would still
#   be parsed by builds that will never use it -- which is what the existing RGB backend has to do.
#   Keeping the S31 hardware layer in the IDF tree means the PlatformIO environments do not see it at
#   all, and the "nothing that works today may break" rule is satisfied by construction rather than by
#   getting a guard right.
#
# RamFramebufferDisplay IS built from src/, and deliberately so: it is the existing, tested rasterizer,
# and KryonSprite composes it rather than duplicating it. Compiling the original file here -- not a
# copy -- is what keeps a sprite and a RAM backend identical by construction.
#
# arduino-esp32 is required for Serial, String and ps_malloc, all of which the shared src/ code uses.

# Three levels up: idf/components/kryonos_s31_display -> idf/components -> idf -> <repo>, whose src/
# is where the shared display code lives.
set(KRYONOS_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src")

idf_component_register(
    SRCS
        "KryonText.cpp"
        "KryonSprite.cpp"
        "KorvoRgbDisplay.cpp"
        "${KRYONOS_SRC_DIR}/Hal/Display/RamFramebufferDisplay.cpp"
    INCLUDE_DIRS
        "."
        "${KRYONOS_SRC_DIR}"
    REQUIRES
        arduino-esp32
        esp_psram
        esp_lcd
        # For esp_cache_msync, which present() calls to write the CPU's cached drawing back to the
        # PSRAM the RGB panel's DMA scans. Needed explicitly: esp_lcd and esp_psram both list esp_mm
        # as a PRIV_REQUIRES, and private requires do not propagate their include directories here.
        esp_mm
        espressif__esp32_s31_korvo_1
)
