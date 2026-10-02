#ifndef KRYONOS_XPT2046_TFT_DRIVER_H
#define KRYONOS_XPT2046_TFT_DRIVER_H

#include "Hal/Touch/ITouchDriver.h"

class TFT_eSPI;

// XPT2046 driven by TFT_eSPI itself, for boards that wire the controller onto the display's SPI
// bus and define TOUCH_CS. TFT_eSPI owns the chip select, the SPI clock and the mapping, so this
// driver is a thin pass-through — its job is to present the same ITouchDriver surface as the
// bit-bang path, not to reimplement anything.
class Xpt2046TftDriver : public ITouchDriver {
public:
    const char* name() const override { return "xpt2046-tft"; }

    void begin(KryonDisplay* display) override;
    bool getTouchRaw(uint16_t* x, uint16_t* y) override;
    bool getTouch(uint16_t* x, uint16_t* y, uint16_t threshold) override;
    void setCalibration(const uint16_t* parameters) override;
    void calibrate(uint16_t* parameters, uint32_t color_fg, uint32_t color_bg,
                   uint8_t size) override;
    bool needsCalibration() const override { return true; }

private:
    TFT_eSPI* tft_ = nullptr;
};

#endif // KRYONOS_XPT2046_TFT_DRIVER_H
