# The Korvo-1 touch path: one adapter over the BSP's esp_lcd_touch handle.
#
# The driver itself lives in src/Hal/Touch/EspLcdTouchDriver.cpp, next to the other touch drivers and
# behind the ITouchDriver seam -- it is KryonOS's code and belongs with its siblings. What keeps it out
# of the PlatformIO builds is not where the file is but what compiles it: the seven PlatformIO
# environments compile src/ themselves and never see this component, and the driver's guard macro is
# defined only here. So the file is inert unless this component builds it.
#
# KRYONOS_TOUCH_USE_ESP_LCD is PUBLIC because the touch driver factory in src/ has to guard its own
# reference to the driver with the same macro, and it will be compiled by a component that depends on
# this one. Defining it privately would leave that reference uncompilable in exactly one build.

set(KRYONOS_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/../../../src")

idf_component_register(
    SRCS
        "${KRYONOS_SRC_DIR}/Hal/Touch/EspLcdTouchDriver.cpp"
    INCLUDE_DIRS
        "${KRYONOS_SRC_DIR}"
    REQUIRES
        arduino-esp32
        espressif__esp32_s31_korvo_1
        espressif__esp_lcd_touch
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC KRYONOS_TOUCH_USE_ESP_LCD=1)
