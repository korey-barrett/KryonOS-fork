# M3: the display backend plus the touch adapter, on top of the Arduino core and the BSP.
#
# kryonos_s31_display is where the board's display stack lives -- the backend, the sprite rasterizer
# and the ported TFT_eSPI text engine. kryonos_s31_touch is the one adapter that turns the BSP's
# esp_lcd_touch handle into an ITouchDriver. Both are IDF components rather than files under src/ so
# that the PlatformIO environments never compile any of it; see their CMakeLists for the reasoning.
#
# esp_psram is named explicitly because PSRAM is what the 800x480 canvas is allocated from:
# arduino-esp32 does not re-export it. esp_lcd is named for esp_lcd_panel_ops.h. esp_lcd_touch is
# named because main.cpp includes EspLcdTouchDriver.h, which pulls esp_lcd_touch.h in behind its
# guard -- naming it here keeps that include resolvable without relying on a transitive one.
idf_component_register(SRCS "main.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES arduino-esp32 esp_psram esp_lcd
                                kryonos_s31_display
                                kryonos_s31_touch
                                kryonos_app
                                espressif__esp32_s31_korvo_1
                                espressif__esp_lcd_touch)

# Which of the two programs main.cpp contains; see the option in ../CMakeLists.txt. Defined as 0
# rather than left undefined so the file's `#if KRYONOS_KORVO_BRINGUP` is a real boolean test with no
# -Wundef warning, and so a stale cache cannot leave it half-set.
if(KRYONOS_KORVO_BRINGUP)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KRYONOS_KORVO_BRINGUP=1)
else()
    target_compile_definitions(${COMPONENT_LIB} PRIVATE KRYONOS_KORVO_BRINGUP=0)
endif()
