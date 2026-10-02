#ifndef KRYONOS_DISPLAY_H
#define KRYONOS_DISPLAY_H

// ---------------------------------------------------------------------------------------------
// Display — the single RUNTIME source of truth for screen geometry.
//
// The UI, touch mapping, JS bindings and graphics helpers must read these accessors rather than the
// KRYONOS_DISPLAY_* macros or tft.width()/tft.height() directly, so that every consumer agrees on
// one set of numbers at runtime (after rotation), and so a non-TFT_eSPI backend can supply them
// later without touching call sites.
//
// Before begin() runs, the accessors return the compile-time KRYONOS_DISPLAY_* values, so code that
// queries geometry very early still gets sane numbers.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>
#include "Hal/Display/DisplayConfig.h"

class Display {
public:
    // Initialises the panel and snapshots the real geometry. Idempotent.
    static void begin();

    // True once begin() has bound the metrics to a real panel.
    static bool isBound();

    // Logical geometry AFTER rotation (equals tft.width()/height() once bound).
    static int16_t width();
    static int16_t height();
    static uint8_t rotation();

    // Convenience helpers.
    static bool isLandscape();
    static int16_t centerX();
    static int16_t centerY();

    // Compile-time board id, for logs.
    static const char* boardId();

    // Explicit rotation change (rare; most boards set KRYONOS_DISPLAY_ROTATION instead).
    static void setRotation(uint8_t rotation);

private:
    static int16_t s_width;
    static int16_t s_height;
    static uint8_t s_rotation;
    static bool    s_bound;
};

#endif // KRYONOS_DISPLAY_H
