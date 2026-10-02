#ifndef KRYONOS_XPT2046_BITBANG_DRIVER_H
#define KRYONOS_XPT2046_BITBANG_DRIVER_H

#include "Hal/Touch/ITouchDriver.h"
#include "Hal/Touch/TouchCalibration.h"

// XPT2046 read over four plain GPIOs, clocked by hand. This is the historical TouchDriver
// implementation, unchanged apart from living behind ITouchDriver and asking Display for the
// canvas instead of assuming 240x320.
//
// The bit-bang path is used when the board wires the controller to its own pins rather than to
// TFT_eSPI's. Contact is detected by pressure (Z1/Z2) rather than by PENIRQ, because several
// panels in this repo do not wire the IRQ line out at all (see TouchConfig.h).
class Xpt2046BitbangDriver : public ITouchDriver {
public:
    const char* name() const override { return "xpt2046-bitbang"; }

    void begin(KryonDisplay* display) override;
    bool getTouchRaw(uint16_t* x, uint16_t* y) override;
    bool getTouch(uint16_t* x, uint16_t* y, uint16_t threshold) override;
    void setCalibration(const uint16_t* parameters) override;
    void calibrate(uint16_t* parameters, uint32_t color_fg, uint32_t color_bg,
                   uint8_t size) override;
    bool needsCalibration() const override { return true; }

private:
    uint16_t transfer16(uint8_t cmd);

    KryonDisplay* display_ = nullptr;
    kryon_touch::CalibrationData cal_;
};

#endif // KRYONOS_XPT2046_BITBANG_DRIVER_H
