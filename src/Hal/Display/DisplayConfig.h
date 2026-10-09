#ifndef KRYONOS_DISPLAY_CONFIG_H
#define KRYONOS_DISPLAY_CONFIG_H

// ---------------------------------------------------------------------------------------------
// Display configuration — the single compile-time contract for the logical canvas.
//
// This header is intentionally Arduino-free (it includes only <stdint.h>) so that host-side tools
// (tools/preview) and any future native simulator can include it and reason about the exact same
// dimensions the firmware is built with.
//
// Contract:
//   * KRYONOS_DISPLAY_WIDTH / _HEIGHT are the LOGICAL canvas, measured AFTER rotation — i.e. they
//     must equal tft.width() / tft.height() once tft.setRotation(KRYONOS_DISPLAY_ROTATION) has run.
//   * KRYONOS_DISPLAY_ROTATION is the value passed to tft.setRotation() (0-3).
//   * TFT_WIDTH / TFT_HEIGHT (TFT_eSPI) are the NATIVE panel dimensions and are set only when the
//     driver default is wrong (e.g. Cardputer 135x240).
//
// A board sets these from its environment's build_flags. The legacy DISP_HOR_RES / DISP_VER_RES
// spellings are honoured as aliases for one release so existing board files keep working.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

// --- Logical canvas width --------------------------------------------------------------------
#ifndef KRYONOS_DISPLAY_WIDTH
  #if defined(DISP_HOR_RES) && (DISP_HOR_RES > 0)
    #define KRYONOS_DISPLAY_WIDTH DISP_HOR_RES
  #else
    #define KRYONOS_DISPLAY_WIDTH 240
  #endif
#endif

// --- Logical canvas height -------------------------------------------------------------------
#ifndef KRYONOS_DISPLAY_HEIGHT
  #if defined(DISP_VER_RES) && (DISP_VER_RES > 0)
    #define KRYONOS_DISPLAY_HEIGHT DISP_VER_RES
  #else
    #define KRYONOS_DISPLAY_HEIGHT 320
  #endif
#endif

// --- Rotation (0-3, passed to tft.setRotation) -----------------------------------------------
#ifndef KRYONOS_DISPLAY_ROTATION
  #define KRYONOS_DISPLAY_ROTATION 0
#endif

// --- Human-readable board id used in logs ----------------------------------------------------
// One default board per chip type; the TARGET_CYD / TARGET_T_HMI / TARGET_CARDPUTER entries are the
// reference boards kept under src/Hal/Boards/<name>/ as templates (not built by default).
// TARGET_ESP32S31_DEFAULT pairs with the S31 preview environment (Arduino 4.x / IDF 6.1) — see
// Documentation/Display_Touch_Architecture.md.
#ifndef KRYONOS_BOARD_ID
  #if defined(TARGET_ESP32S3_DEFAULT)
    #define KRYONOS_BOARD_ID "esp32s3-default"
  #elif defined(TARGET_ESP32_DEFAULT)
    #define KRYONOS_BOARD_ID "esp32-default"
  #elif defined(TARGET_ESP32S31_DEFAULT)
    #define KRYONOS_BOARD_ID "esp32s31-default"
  #elif defined(TARGET_WAVESHARE_S3_LCD21B)
    #define KRYONOS_BOARD_ID "waveshare-s3-lcd21b"
  #elif defined(TARGET_ESP32S31_KORVO1)
    #define KRYONOS_BOARD_ID "esp32s31-korvo1"
  #elif defined(TARGET_CYD)
    #define KRYONOS_BOARD_ID "esp32-cyd-28"
  #elif defined(TARGET_T_HMI)
    #define KRYONOS_BOARD_ID "lilygo-t-hmi"
  #elif defined(TARGET_CARDPUTER)
    #define KRYONOS_BOARD_ID "m5stack-cardputer"
  #else
    #define KRYONOS_BOARD_ID "unknown"
  #endif
#endif

// --- Display backend --------------------------------------------------------------------------
// Which KryonDisplay implementation the board file instantiates. This is compile-time on purpose:
// it decides the static type of the display object, so it cannot be a string compared at runtime
// the way KRYONOS_TOUCH_DRIVER is. Set it from the environment, e.g.
//     -D KRYONOS_DISPLAY_BACKEND=KRYONOS_BACKEND_RAM
// See Documentation/Display_Touch_Architecture.md §2 (and §2.5 for the RGB backend, whose files
// compile only on targets that ship ESP-IDF's RGB panel driver).
#define KRYONOS_BACKEND_TFT_ESPI 1 // src/Hal/Display/TftEspiDisplay.h -- the real panel driver
#define KRYONOS_BACKEND_RAM      2 // src/Hal/Display/RamFramebufferDisplay.h -- RAM framebuffer
#define KRYONOS_BACKEND_RGB      3 // src/Hal/Display/{EspLcdRgbDisplay,KorvoRgbDisplay}.h -- RGB parallel panel (RGB-capable targets: S3, S31, P4)
#ifndef KRYONOS_DISPLAY_BACKEND
  #define KRYONOS_DISPLAY_BACKEND KRYONOS_BACKEND_TFT_ESPI
#endif

namespace kryon_display {

// Compile-time sanity: a zero or negative dimension is always a configuration mistake.
static_assert(KRYONOS_DISPLAY_WIDTH  > 0, "KRYONOS_DISPLAY_WIDTH must be > 0");
static_assert(KRYONOS_DISPLAY_HEIGHT > 0, "KRYONOS_DISPLAY_HEIGHT must be > 0");

inline constexpr int16_t width()  { return static_cast<int16_t>(KRYONOS_DISPLAY_WIDTH); }
inline constexpr int16_t height() { return static_cast<int16_t>(KRYONOS_DISPLAY_HEIGHT); }
inline constexpr uint8_t rotation() { return static_cast<uint8_t>(KRYONOS_DISPLAY_ROTATION); }
inline constexpr bool isLandscape() { return KRYONOS_DISPLAY_WIDTH > KRYONOS_DISPLAY_HEIGHT; }
inline constexpr int16_t centerX() { return static_cast<int16_t>(KRYONOS_DISPLAY_WIDTH / 2); }
inline constexpr int16_t centerY() { return static_cast<int16_t>(KRYONOS_DISPLAY_HEIGHT / 2); }

} // namespace kryon_display

#endif // KRYONOS_DISPLAY_CONFIG_H
