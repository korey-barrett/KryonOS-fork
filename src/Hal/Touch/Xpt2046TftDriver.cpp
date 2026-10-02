#include "Hal/Touch/Xpt2046TftDriver.h"

#include "Hal/Display/KryonDisplay.h"
#include "Hal/Touch/TouchConfig.h"

#include <Arduino.h>
#include <TFT_eSPI.h>

#if KRYONOS_TOUCH_HAS_TFT_TOUCH

void Xpt2046TftDriver::begin(KryonDisplay* display) {
    // This driver is TFT_eSPI's own touch path, so it needs the TFT_eSPI instance behind the
    // display. A backend without one leaves tft_ null and every call below returns false.
    tft_ = display ? display->nativeTft() : nullptr;
    if (TOUCH_CS >= 0) {
        pinMode(TOUCH_CS, OUTPUT);
        digitalWrite(TOUCH_CS, HIGH);
    }
}

bool Xpt2046TftDriver::getTouchRaw(uint16_t* x, uint16_t* y) {
    if (!tft_) return false;
    return tft_->getTouchRaw(x, y);
}

bool Xpt2046TftDriver::getTouch(uint16_t* x, uint16_t* y, uint16_t threshold) {
    if (!tft_) return false;
    // TFT_eSPI applies its own calibration and returns pixels already clipped to the panel, so
    // there is nothing to map here.
    return tft_->getTouch(x, y, threshold);
}

void Xpt2046TftDriver::setCalibration(const uint16_t* parameters) {
    if (tft_) {
        // TFT_eSPI's setTouch takes a non-const pointer but only reads it; the cast is safe.
        tft_->setTouch(const_cast<uint16_t*>(parameters));
    }
}

void Xpt2046TftDriver::calibrate(uint16_t* parameters, uint32_t color_fg, uint32_t color_bg,
                                 uint8_t size) {
    if (!tft_ || !parameters) return;
    tft_->calibrateTouch(parameters, color_fg, color_bg, size);
    setCalibration(parameters);
}

#endif // KRYONOS_TOUCH_HAS_TFT_TOUCH

// Boards without TOUCH_CS still reference this class from the factory's choice list, so the vtable
// and constructor need real symbols here too. Unreachable: the factory only builds this driver
// when TOUCH_CS is defined.
#if !KRYONOS_TOUCH_HAS_TFT_TOUCH

void Xpt2046TftDriver::begin(KryonDisplay* display) { (void)display; }
bool Xpt2046TftDriver::getTouchRaw(uint16_t* x, uint16_t* y) { (void)x; (void)y; return false; }
bool Xpt2046TftDriver::getTouch(uint16_t* x, uint16_t* y, uint16_t threshold) {
    (void)x; (void)y; (void)threshold;
    return false;
}
void Xpt2046TftDriver::setCalibration(const uint16_t* parameters) { (void)parameters; }
void Xpt2046TftDriver::calibrate(uint16_t* parameters, uint32_t color_fg, uint32_t color_bg,
                                 uint8_t size) {
    (void)parameters; (void)color_fg; (void)color_bg; (void)size;
}

#endif // !KRYONOS_TOUCH_HAS_TFT_TOUCH
