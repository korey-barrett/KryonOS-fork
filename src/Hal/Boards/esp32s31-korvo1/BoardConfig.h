#ifndef ESP32S31_KORVO1_BOARD_CONFIG_H
#define ESP32S31_KORVO1_BOARD_CONFIG_H

// Board profile for Espressif's ESP32-S31-Korvo-1 development board.
//
// This is a NAMED board, not a chip default: it describes one specific piece of hardware, whose panel,
// touch controller, pin map and timings all come from Espressif's own BSP (espressif__esp32_s31_korvo_1)
// rather than from numbers chosen here. That is the point of the port -- the constants that are easy to
// get wrong are the vendor's, and this file only records the ones that are properties of the board
// rather than of the panel: the logical canvas the UI draws to, and the board's identity in logs.
//
// WHY THERE ARE NO TFT_* PIN MACROS HERE
//   Every other board in this directory names a backlight pin and a bus wiring, because every other
//   board has to drive its own panel. This one does not: the BSP owns the RGB bus, the panel init
//   sequence and the backlight (which on this board is hardwired on -- BSP_LCD_BACKLIGHT is GPIO_NUM_NC
//   and the BSP's own brightness call returns ESP_ERR_NOT_SUPPORTED by design). A pin macro here would
//   be a second, staler copy of a fact the BSP already states.
//
// Guarded on the same positive macro as BoardConfig.cpp, so header and implementation are always
// included or excluded together. Never use an inverse ("none of the others") guard here: that silently
// breaks the moment another board is added and forgotten.
#if defined(TARGET_ESP32S31_KORVO1)

#include <Arduino.h>

// --- Display resolution: the LOGICAL canvas, measured AFTER rotation -------------------------
// The panel is 800x480 and this backend draws 1:1 into it -- panelToCanvas() is the identity, unlike
// the Waveshare port whose round aperture needed a transform. So the canvas is the panel, and these
// two numbers are the same ones the BSP reports as BSP_LCD_H_RES / BSP_LCD_V_RES.
#ifndef KRYONOS_DISPLAY_WIDTH
#define KRYONOS_DISPLAY_WIDTH 800
#endif
#ifndef KRYONOS_DISPLAY_HEIGHT
#define KRYONOS_DISPLAY_HEIGHT 480
#endif

// The panel's own timings fix it in landscape; there is no portrait mode to rotate into, which is why
// KorvoRgbDisplay::setRotation() refuses anything but 0 rather than pretending.
#ifndef KRYONOS_DISPLAY_ROTATION
#define KRYONOS_DISPLAY_ROTATION 0
#endif

#endif // TARGET_ESP32S31_KORVO1

#endif // ESP32S31_KORVO1_BOARD_CONFIG_H
