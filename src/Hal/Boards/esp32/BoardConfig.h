#ifndef ESP32_BOARD_CONFIG_H
#define ESP32_BOARD_CONFIG_H

// Default board profile for the plain ESP32 (Xtensa LX6) chip type.
//
// This is a STARTING POINT, not a specific product: it assumes a generic SPI TFT (ILI9341) and an
// XPT2046 resistive touch controller. Every value here is an #ifndef default, so the environment's
// build_flags — or your own board_configs/<name>.ini — always wins. Copy this directory to create a
// named board; see Documentation/Display_Touch_Architecture.md.
//
// Guarded on the same positive macro as BoardConfig.cpp, so header and implementation are always
// included or excluded together. Never use an inverse ("none of the others") guard here.
#if defined(TARGET_ESP32_DEFAULT)

#include <Arduino.h>

// --- Display resolution: the LOGICAL canvas, measured AFTER rotation -------------------------
// These are only FALLBACKS. The live values are the -D KRYONOS_DISPLAY_* build flags in the
// environment (platformio.ini); change the panel size there, not here. The legacy DISP_HOR_RES /
// DISP_VER_RES spellings are still honoured by DisplayConfig.h.
#ifndef KRYONOS_DISPLAY_WIDTH
#define KRYONOS_DISPLAY_WIDTH 240
#endif
#ifndef KRYONOS_DISPLAY_HEIGHT
#define KRYONOS_DISPLAY_HEIGHT 320
#endif
#ifndef KRYONOS_DISPLAY_ROTATION
#define KRYONOS_DISPLAY_ROTATION 0
#endif

// --- Backlight ---
#ifndef TFT_BL
#define TFT_BL 4
#endif

// --- Touch pins (XPT2046, bit-banged by TouchDriver) ------------------------------------------
#ifndef TOUCH_CLK
#define TOUCH_CLK 14
#endif
#ifndef TOUCH_DIN
#define TOUCH_DIN 13
#endif
#ifndef TOUCH_DO
#define TOUCH_DO 12
#endif
#ifndef TOUCH_CS
#define TOUCH_CS 15
#endif
#ifndef TOUCH_IRQ
#define TOUCH_IRQ 27
#endif

#endif // TARGET_ESP32_DEFAULT

#endif // ESP32_BOARD_CONFIG_H
