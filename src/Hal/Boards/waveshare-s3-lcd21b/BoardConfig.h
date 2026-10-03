#ifndef WAVESHARE_S3_LCD21B_BOARD_CONFIG_H
#define WAVESHARE_S3_LCD21B_BOARD_CONFIG_H

// Board profile for the Waveshare ESP32-S3-Touch-LCD-2.1B: an ESP32-S3 with a 480x480 ST7701 panel
// on an RGB565 PARALLEL bus, plus a CST820 capacitive touch controller.
//
// This board is deliberately unlike the three chip defaults. It does NOT drive its panel with
// TFT_eSPI -- an RGB parallel panel has no SPI pixel path, so TFT_eSPI cannot drive it at all.
// It selects the KRYONOS_BACKEND_RGB backend (src/Hal/Display/EspLcdRgbDisplay.h), which brings the
// panel up through ESP-IDF's esp_lcd_panel_rgb and rasterizes the UI with TFT_eSPI into a PSRAM
// sprite. See Documentation/Display_Touch_Architecture.md.
//
// Guarded on the same positive macro as BoardConfig.cpp, so header and implementation are always
// included or excluded together. Never use an inverse ("none of the others") guard here.
#if defined(TARGET_WAVESHARE_S3_LCD21B)

#include <Arduino.h>

// --- Panel geometry: a HARDWARE FACT, not a UI resolution -------------------------------------
// The ST7701 scans out 480x480 and its timing registers are written from these two numbers. They are
// deliberately NOT KRYONOS_DISPLAY_*: that pair is the logical canvas the UI draws to, and the two
// are different on this board because the panel is round. A 480x480 canvas is cut by the bezel --
// the footer's UP and DN thirds sit outside the glass -- so the canvas is smaller and is scaled up
// into the largest rect that fits the circle. See the round-aperture block in
// Hal/Display/EspLcdRgbDisplay.h; the rect there is derived from the canvas, not from this pair.
//
// Collapsing these back into one value is the specific mistake that reconfigured the panel timings
// instead of upscaling, so keep them separate.
#define BOARD_PANEL_W 480
#define BOARD_PANEL_H 480

// --- Display resolution: the LOGICAL canvas, measured AFTER rotation -------------------------
// These are only FALLBACKS; the live values are the -D KRYONOS_DISPLAY_* build flags in
// platformio.ini. 240x320 is the canvas the UI was laid out for, and at that size the 6/5 aperture
// scale lands its corners exactly on the 480 px bezel (sqrt(144^2 + 192^2) = 240).
#ifndef KRYONOS_DISPLAY_WIDTH
#define KRYONOS_DISPLAY_WIDTH 240
#endif
#ifndef KRYONOS_DISPLAY_HEIGHT
#define KRYONOS_DISPLAY_HEIGHT 320
#endif
#ifndef KRYONOS_DISPLAY_ROTATION
#define KRYONOS_DISPLAY_ROTATION 0
#endif

// --- Backlight --------------------------------------------------------------------------------
// GPIO6, driven by LEDC from EspLcdRgbDisplay (25 kHz / 10-bit, matching the panel's own driver), so
// it needs no pin macro here. BoardConfig.cpp passes it to the backend's constructor.

// --- Touch ------------------------------------------------------------------------------------
// Still intentionally NO TOUCH_* macros, and that is not a leftover. The XPT2046 quartet has no
// meaning here, and worse, I2CEngine::begin() refuses to open a bus on any pin a TFT_* or TOUCH_*
// macro claims -- so a stray TOUCH_DIN=15 would silently kill the very I2C bus the touch controller
// and the expander both live on.
//
// Touch is selected the supported way instead, from platformio.ini:
//   -D KRYONOS_TOUCH_DRIVER=\"cst816\" -D KRYONOS_TOUCH_I2C_SDA=15 -D KRYONOS_TOUCH_I2C_SCL=7
// This board's controller is a CST820, which answers to the CST816S register map; see
// Hal/Touch/CapacitiveTouchDriver.h.
//
// There is deliberately NO TOUCH_RST_PIN either: the controller's reset is EXIO2 on the TCA9554
// expander, not a SoC pin, so the macro would drive a pin nothing is connected to. The driver pulses
// it through boardExpander() immediately before each probe, which is the only moment that works.

// --- SD card: SDMMC 1-bit ---------------------------------------------------------------------
// CLK/CMD are GPIO2/GPIO1 -- the same pair the ST7701's 3-wire command bus uses (SDA=1, SCL=2).
// That bus is bit-banged only while the panel's init table goes out, and the backend releases both
// pins when it finishes, so the card must be mounted AFTER Display::begin(). main.cpp's ordering
// already gives it that: FileSystem::init() runs several steps later.
//
// Only DAT0 is wired, hence 1-bit. The slot is NOT on SPI -- there is no separate chip-select line
// -- so the SPI `SD` class cannot drive it. It mounts as SD_MMC, and FileSystem routes "/sd/..."
// paths through whichever object the board actually mounted (see sdFS in FileSystem.cpp).
#define BOARD_SD_CLK_PIN 2
#define BOARD_SD_CMD_PIN 1
#define BOARD_SD_D0_PIN  42

// --- Panel wiring (documentation; the backend holds the real values) --------------------------
//   ST7701 RGB data[16] = {5,45,48,47,21,14,13,12,11,10,9,46,3,8,18,17}
//   DE=40  PCLK=41  VSYNC=39  HSYNC=38   backlight=6
//   Command bus (init only): SCL=2 SDA=1, CS on TCA9554 bit 2
//   TCA9554 at 0x20: LCD_RST=bit0  TP_RST=bit1  SPI_CS=bit2  SD_CS=bit3  buzzer=bit7
//   I2C for expander + touch: SDA=15 SCL=7
//   Touch INT=16 (unused: the CST816 driver reads ungated, which is what wakes a sleeping part)
//   Bit assignments live in Hal/I2C/Tca9554.h, which is the one place they are defined.

#endif // TARGET_WAVESHARE_S3_LCD21B

#endif // WAVESHARE_S3_LCD21B_BOARD_CONFIG_H
