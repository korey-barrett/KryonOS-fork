#ifndef ESP32S31_BOARD_CONFIG_H
#define ESP32S31_BOARD_CONFIG_H

// Default board profile for the ESP32-S31 chip type.
//
// This is a STARTING POINT, not a specific product: it assumes a generic SPI TFT (ILI9341) and an
// XPT2046 resistive touch controller. Every value here is an #ifndef default, so the environment's
// build_flags — or your own board_configs/<name>.ini — always wins. Copy this directory to create a
// named board; see Documentation/Display_Touch_Architecture.md.
//
// NOTE ON PINS: the S31 exposes ~60 GPIOs and its strapping/USB/JTAG assignments differ from the
// ESP32 and ESP32-S3. The numbers below are placeholders mirroring the S3 default — they are only
// correct for whatever panel you actually wire up. Set TFT_*/TOUCH_* in the environment's
// build_flags to match your hardware; do not trust these defaults.
//
// Guarded on the same positive macro as BoardConfig.cpp, so header and implementation are always
// included or excluded together. Never use an inverse ("none of the others") guard here.
#if defined(TARGET_ESP32S31_DEFAULT)

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
#define TFT_BL 21
#endif

// --- Touch pins (XPT2046, bit-banged by TouchDriver) ------------------------------------------
#ifndef TOUCH_CS
#define TOUCH_CS 7
#endif
#ifndef TOUCH_CLK
#define TOUCH_CLK 4
#endif
#ifndef TOUCH_DIN
#define TOUCH_DIN 5
#endif
#ifndef TOUCH_DO
#define TOUCH_DO 6
#endif
#ifndef TOUCH_IRQ
#define TOUCH_IRQ 14
#endif

#endif // TARGET_ESP32S31_DEFAULT

#endif // ESP32S31_BOARD_CONFIG_H
