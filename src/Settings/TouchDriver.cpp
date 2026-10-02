#include "TouchDriver.h"

#include "Hal/Display/Display.h"

// The facade owns no state of its own: the driver singleton holds the calibration and the pins,
// and Display holds the canvas. Every call is a forward.
//
// The four XPT2046 pins (and the CYD's TOUCHSCREEN_* spelling) are normalized in
// Hal/Touch/TouchConfig.h, which this header pulls in, so anything that used to reach T_CS_PIN
// through here still can.

void TouchDriver::init(KryonDisplay *tft) {
    touchDriver().begin(tft);
}

bool TouchDriver::getTouch(uint16_t *x, uint16_t *y, uint16_t threshold) {
    return touchDriver().getTouch(x, y, threshold);
}

bool TouchDriver::getTouchRaw(uint16_t *x, uint16_t *y) {
    return touchDriver().getTouchRaw(x, y);
}

void TouchDriver::calibrateTouch(uint16_t *parameters, uint32_t color_fg, uint32_t color_bg,
                                 uint8_t size) {
    touchDriver().calibrate(parameters, color_fg, color_bg, size);
}

void TouchDriver::setTouch(uint16_t *parameters) {
    touchDriver().setCalibration(parameters);
}

bool TouchDriver::needsCalibration() {
    return touchDriver().needsCalibration();
}

const char *TouchDriver::driverName() {
    return touchDriver().name();
}
