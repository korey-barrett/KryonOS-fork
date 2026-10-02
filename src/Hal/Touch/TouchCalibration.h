#ifndef KRYONOS_TOUCH_CALIBRATION_H
#define KRYONOS_TOUCH_CALIBRATION_H

// ---------------------------------------------------------------------------------------------
// Calibration state and the raw-count -> pixel mapping, shared by the XPT2046 drivers.
//
// Arduino-free and header-inline on purpose: tools/preview and any future host simulator can use
// the exact same mapping the firmware runs, so a layout preview and a real panel cannot disagree
// about where a tap lands.
//
// The 5-word tuple is the historical /touch_cal_p.bin layout:  [x0, dx, y0, dy, flags].
// `x0`/`y0` are the raw counts at the canvas origin, `dx`/`dy` the raw count span across the
// canvas, and `flags` bit 0 = axes swapped, bit 1 = X inverted, bit 2 = Y inverted. The format is
// unchanged by this refactor: raw counts do not depend on the logical resolution, so changing
// KRYONOS_DISPLAY_* does not invalidate an existing calibration file.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

#include "TouchConfig.h"

namespace kryon_touch {

struct CalibrationData {
    uint16_t words[5] = {0, 0, 0, 0, 0};
    bool present = false;

    void set(const uint16_t* parameters) {
        if (!parameters) return;
        for (int i = 0; i < 5; i++) words[i] = parameters[i];
        present = true;
    }

    // A tuple with a zero span maps everything to the origin, so treat it as absent and fall back
    // to the uncalibrated window rather than reporting a dead screen.
    bool usable() const { return present && words[1] != 0 && words[3] != 0; }

    uint16_t flags() const { return words[4]; }
    bool rotated()  const { return (flags() & 0x01) != 0; }
    bool invertedX() const { return (flags() & 0x02) != 0; }
    bool invertedY() const { return (flags() & 0x04) != 0; }
};

// Arduino's map(), spelled out so this header stays self-contained. Truncating integer division,
// identical to the built-in for the ranges used here.
inline long scaleToRange(long value, long inMin, long inMax, long outMin, long outMax) {
    if (inMax == inMin) return outMin;
    return (value - inMin) * (outMax - outMin) / (inMax - inMin) + outMin;
}

inline uint16_t clampToCanvas(long value, int16_t extent) {
    if (value < 0) return 0;
    if (value >= extent) return (uint16_t)(extent - 1);
    return (uint16_t)value;
}

// Uncalibrated fallback: stretch the wide default window across the canvas. Used until the first
// calibration is written, and whenever a stored tuple is unusable.
inline void mapRawDefault(uint16_t rawX, uint16_t rawY, int16_t w, int16_t h,
                          uint16_t* outX, uint16_t* outY) {
    *outX = clampToCanvas(scaleToRange(rawX, kRawDefaultMin, kRawDefaultMax, 0, w), w);
    *outY = clampToCanvas(scaleToRange(rawY, kRawDefaultMin, kRawDefaultMax, 0, h), h);
}

// Calibrated mapping. False when the result falls outside the canvas, which is how a stray raw
// reading during calibration or a panel glitch is rejected instead of turning into a tap at (0,0).
inline bool mapRawCalibrated(const CalibrationData& cal, uint16_t rawX, uint16_t rawY,
                             int16_t w, int16_t h, uint16_t* outX, uint16_t* outY) {
    const uint16_t x0 = cal.words[0];
    const uint16_t dx = cal.words[1];
    const uint16_t y0 = cal.words[2];
    const uint16_t dy = cal.words[3];

    int32_t xx, yy;
    if (!cal.rotated()) {
        xx = (int32_t)((int32_t)(rawX - x0) * w) / (int32_t)dx;
        yy = (int32_t)((int32_t)(rawY - y0) * h) / (int32_t)dy;
    } else {
        xx = (int32_t)((int32_t)(rawY - x0) * w) / (int32_t)dx;
        yy = (int32_t)((int32_t)(rawX - y0) * h) / (int32_t)dy;
    }

    if (cal.invertedX()) xx = w - xx;
    if (cal.invertedY()) yy = h - yy;

    if (xx < 0 || xx >= w || yy < 0 || yy >= h) return false;

    *outX = (uint16_t)xx;
    *outY = (uint16_t)yy;
    return true;
}

// The two above, chosen by whether a usable calibration exists.
inline bool mapRawToPixels(const CalibrationData& cal, uint16_t rawX, uint16_t rawY,
                           int16_t w, int16_t h, uint16_t* outX, uint16_t* outY) {
    if (!cal.usable()) {
        mapRawDefault(rawX, rawY, w, h, outX, outY);
        return true;
    }
    return mapRawCalibrated(cal, rawX, rawY, w, h, outX, outY);
}

} // namespace kryon_touch

#endif // KRYONOS_TOUCH_CALIBRATION_H
