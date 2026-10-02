#ifndef TOUCH_DRIVER_H
#define TOUCH_DRIVER_H

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "Hal/Display/KryonDisplay.h"
#include "Hal/Touch/TouchConfig.h"
#include "Hal/Touch/TouchDriverFactory.h"
#include "Hal/Display/DisplayConfig.h"

// ---------------------------------------------------------------------------------------------
// TouchDriver — the facade the rest of the OS calls.
//
// The public API here is deliberately unchanged from before the driver seam existed: the kernel,
// keyboard, JS bindings and settings screens all still call TouchDriver::getTouch() and friends.
// What changed is what answers: the calls now forward to whichever ITouchDriver the factory picked
// (see Hal/Touch/TouchDriverFactory.h), so a capacitive panel or a different resistive wiring is a
// build-flag change rather than a rewrite.
//
// Everything here is static and thin on purpose — there is exactly one touch controller per board.
// ---------------------------------------------------------------------------------------------

class TouchDriver {
public:
    // Binds the driver to the display instance and brings up its pins/bus. Idempotent.
    static void init(KryonDisplay *tft);

    // Calibrated pixel coordinates in the live logical canvas (see Display). False when there is
    // no touch. `threshold` is the XPT2046 pressure threshold; absolute-position drivers ignore it.
    static bool getTouch(uint16_t *x, uint16_t *y, uint16_t threshold = 600);

    // Raw controller counts, pre-calibration. For the calibration routine and diagnostics only.
    static bool getTouchRaw(uint16_t *x, uint16_t *y);

    // Interactive corner-touch calibration. A driver that reports absolute coordinates returns
    // without touching `parameters`; check needsCalibration() before offering the screen.
    static void calibrateTouch(uint16_t *parameters, uint32_t color_fg, uint32_t color_bg,
                               uint8_t size = 15);

    // Install a calibration tuple (the 5-word /touch_cal_p.bin layout).
    static void setTouch(uint16_t *parameters);

    // False for absolute-position (capacitive) drivers and for a board with no touch panel, so the
    // OS can skip the calibration screen instead of showing four arrows nothing can press.
    static bool needsCalibration();

    // Short stable driver name for boot logs ("xpt2046-bitbang", "ft6236", "none").
    static const char *driverName();
};

#endif // TOUCH_DRIVER_H
