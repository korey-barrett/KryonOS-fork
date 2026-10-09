#ifndef KRYONOS_TOUCH_CONFIG_H
#define KRYONOS_TOUCH_CONFIG_H

// ---------------------------------------------------------------------------------------------
// Touch configuration — pin spelling, driver selection, and the raw-count constants.
//
// Intentionally Arduino-free (only the preprocessor, plus <stdint.h> for the constants), so this
// header can be included from board files and from host-side tooling without dragging in TFT_eSPI
// or Wire. The driver implementations are the only files that need the real hardware headers.
//
// SELECTION
//   KRYONOS_TOUCH_DRIVER is a string build flag naming the driver explicitly:
//       -D KRYONOS_TOUCH_DRIVER=\"ft6236\"
//   Left unset it is "auto", and the factory (TouchDriverFactory.cpp) reproduces the historical
//   compile-time ladder: bit-banged XPT2046 when all four pins exist, else TFT_eSPI's own touch
//   (when TOUCH_CS is defined), else a null driver that reports no touch. That keeps every board
//   that existed before this seam behaving identically without setting the flag at all.
//
//   Known names: auto, xpt2046_bitbang, xpt2046_tft, ft6236, gt911, gt1151, cst816, null.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#ifndef KRYONOS_TOUCH_DRIVER
#define KRYONOS_TOUCH_DRIVER "auto"
#endif

// --- Pin normalization ------------------------------------------------------------------------
// Boards spell the XPT2046 pins two different ways: the S3/ESP32 defaults define
// TOUCH_CS/TOUCH_CLK/TOUCH_DIN/TOUCH_DO (or their *_PIN forms); the CYD and T-HMI reference
// snippets define TOUCHSCREEN_CS/SCLK/MOSI/MISO_PIN instead. Both spellings resolve here, once,
// so the driver guards and the implementations cannot drift apart. The TOUCH_* spelling is tested
// first, so boards that use it resolve exactly as before.
//
// T_DIN is the pin the controller is written on (MOSI); T_DO is the pin it drives back (MISO).
#if defined(TOUCH_CS_PIN)
#define T_CS_PIN TOUCH_CS_PIN
#elif defined(TOUCH_CS)
#define T_CS_PIN TOUCH_CS
#elif defined(TOUCHSCREEN_CS_PIN)
#define T_CS_PIN TOUCHSCREEN_CS_PIN
#endif

#if defined(TOUCH_CLK_PIN)
#define T_CLK_PIN TOUCH_CLK_PIN
#elif defined(TOUCH_CLK)
#define T_CLK_PIN TOUCH_CLK
#elif defined(TOUCHSCREEN_SCLK_PIN)
#define T_CLK_PIN TOUCHSCREEN_SCLK_PIN
#endif

#if defined(TOUCH_DIN_PIN)
#define T_DIN_PIN TOUCH_DIN_PIN
#elif defined(TOUCH_DIN)
#define T_DIN_PIN TOUCH_DIN
#elif defined(TOUCHSCREEN_MOSI_PIN)
#define T_DIN_PIN TOUCHSCREEN_MOSI_PIN
#endif

#if defined(TOUCH_DO_PIN)
#define T_DO_PIN TOUCH_DO_PIN
#elif defined(TOUCH_DO)
#define T_DO_PIN TOUCH_DO
#elif defined(TOUCHSCREEN_MISO_PIN)
#define T_DO_PIN TOUCHSCREEN_MISO_PIN
#endif

#if defined(TOUCH_IRQ_PIN)
#define T_IRQ_PIN TOUCH_IRQ_PIN
#elif defined(TOUCH_IRQ)
#define T_IRQ_PIN TOUCH_IRQ
#endif
// TOUCHSCREEN_IRQ_PIN is deliberately NOT mapped onto T_IRQ_PIN. The CYD's XPT2046 PENIRQ is not
// wired out (cyd/BoardConfig.h defaults it to 36, which is unconnected on the ESP32-2432S028R).
// Mapping it would make getTouchRaw() read a permanently-released touch and disable input
// entirely. The bit-bang path detects contact by pressure (Z1/Z2) instead.

// --- What the pins imply (for the "auto" choice) -----------------------------------------------
#if defined(T_CLK_PIN) && defined(T_DIN_PIN) && defined(T_DO_PIN) && defined(T_CS_PIN)
#define KRYONOS_TOUCH_HAS_BITBANG_PINS 1
#else
#define KRYONOS_TOUCH_HAS_BITBANG_PINS 0
#endif

#if defined(TOUCH_CS)
#define KRYONOS_TOUCH_HAS_TFT_TOUCH 1
#else
#define KRYONOS_TOUCH_HAS_TFT_TOUCH 0
#endif

namespace kryon_touch {

// --- Raw XPT2046 counts ------------------------------------------------------------------------
// The panel's usable ADC window. Values outside the validity window are noise from a released or
// mis-sampled touch and are rejected before mapping.
inline constexpr uint16_t kRawValidMin = 30;
inline constexpr uint16_t kRawValidMax = 4060;

// The window the *uncalibrated* fallback maps from. Wider than the validity window on purpose:
// the historical default stretched 300..3800 across the full canvas.
inline constexpr uint16_t kRawDefaultMin = 300;
inline constexpr uint16_t kRawDefaultMax = 3800;

// Pressure gate for the bit-bang path: Z1 below this, or the computed Z below kMinPressure, is a
// release.
inline constexpr uint16_t kMinZ1 = 60;
inline constexpr int16_t  kMinPressure = 150;

} // namespace kryon_touch

#endif // KRYONOS_TOUCH_CONFIG_H
