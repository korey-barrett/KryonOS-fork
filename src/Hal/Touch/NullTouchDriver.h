#ifndef KRYONOS_NULL_TOUCH_DRIVER_H
#define KRYONOS_NULL_TOUCH_DRIVER_H

#include "Hal/Touch/ITouchDriver.h"

// A board with no touch panel at all. Reports "no touch" forever so the OS runs headless instead
// of reading unconnected pins as a permanently-pressed finger, which is what the unguarded pin
// reads used to do on boards without a controller.
class NullTouchDriver : public ITouchDriver {
public:
    const char* name() const override { return "none"; }

    void begin(KryonDisplay* display) override { (void)display; }
    bool getTouchRaw(uint16_t* x, uint16_t* y) override { (void)x; (void)y; return false; }
    bool getTouch(uint16_t* x, uint16_t* y, uint16_t threshold) override {
        (void)x; (void)y; (void)threshold;
        return false;
    }
    void setCalibration(const uint16_t* parameters) override { (void)parameters; }
    void calibrate(uint16_t* parameters, uint32_t color_fg, uint32_t color_bg,
                   uint8_t size) override {
        (void)parameters; (void)color_fg; (void)color_bg; (void)size;
    }
    // No touch, so there is nothing to calibrate and the calibration screen must be skipped.
    bool needsCalibration() const override { return false; }
    bool isResistive() const override { return false; }
};

#endif // KRYONOS_NULL_TOUCH_DRIVER_H
