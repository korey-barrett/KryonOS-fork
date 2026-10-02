#ifndef KRYONOS_CAPACITIVE_TOUCH_DRIVER_H
#define KRYONOS_CAPACITIVE_TOUCH_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include "Hal/Touch/ITouchDriver.h"

// ---------------------------------------------------------------------------------------------
// Capacitive (I2C) touch controllers.
//
// STATUS: these are complete register-level implementations, but they have NOT been validated
// against real panels — no board in this repo carries one yet. They compile, they probe the bus,
// and they report "no touch" cleanly when the controller does not answer, so selecting one cannot
// break a board that lacks the hardware. Treat the register maps as documentation to verify
// against your panel's datasheet + the vendor's own driver, not as proven code.
//
// Unlike the resistive path, these report absolute pixel coordinates, so they need no calibration:
// needsCalibration() is false, the OS skips the calibration screen, and /touch_cal_p.bin is unused.
//
// WIRING (all optional, from build flags)
//   KRYONOS_TOUCH_I2C_SDA / KRYONOS_TOUCH_I2C_SCL  — bus pins; without them begin() is a no-op and
//                                                    the driver stays absent.
//   KRYONOS_TOUCH_I2C_ADDR                         — override the default 7-bit address.
//   KRYONOS_TOUCH_RST_PIN / KRYONOS_TOUCH_IRQ_PIN  — reset line and (unused) interrupt line.
// ---------------------------------------------------------------------------------------------

class I2cTouchDriver : public ITouchDriver {
public:
    I2cTouchDriver(uint8_t defaultAddress) : address_(defaultAddress) {}

    const char* name() const override { return name_; }

    void begin(KryonDisplay* display) override;
    bool getTouchRaw(uint16_t* x, uint16_t* y) override;
    bool getTouch(uint16_t* x, uint16_t* y, uint16_t threshold) override;
    void setCalibration(const uint16_t* parameters) override { (void)parameters; }
    void calibrate(uint16_t* parameters, uint32_t color_fg, uint32_t color_bg,
                   uint8_t size) override;
    // Absolute coordinates: nothing to calibrate.
    bool needsCalibration() const override { return false; }
    bool isResistive() const override { return false; }

protected:
    // Bring the controller out of reset and configure it. Return false if it does not answer.
    virtual bool probe() = 0;

    // Read one touch point in controller-native coordinates. Return false when no finger is down.
    virtual bool readPoint(uint16_t* x, uint16_t* y) = 0;

    void setName(const char* name) { name_ = name; }

    uint8_t address_;
    bool present_ = false;

private:
    const char* name_ = "i2c-touch";
};

// FT6x36 family (FT6236 / FT6336), 7-bit address 0x38. Register map: TD_STATUS 0x02, then the
// first point as XH/XL/YH/YL at 0x03..0x06 (12-bit X/Y in the high nibble of each high byte).
class Ft6236Driver : public I2cTouchDriver {
public:
    Ft6236Driver() : I2cTouchDriver(0x38) { setName("ft6236"); }

protected:
    bool probe() override;
    bool readPoint(uint16_t* x, uint16_t* y) override;
};

// GT911, 7-bit address 0x5D (0x14 when the INT level at reset selects the alternate address).
// This controller uses 16-bit register addresses: status at 0x814E, the first point at 0x814F.
class Gt911Driver : public I2cTouchDriver {
public:
    Gt911Driver() : I2cTouchDriver(0x5D) { setName("gt911"); }

protected:
    bool probe() override;
    bool readPoint(uint16_t* x, uint16_t* y) override;

private:
    bool readReg16(uint16_t reg, uint8_t* out, size_t length);
    bool writeReg8(uint16_t reg, uint8_t value);
};

// CST816S/CST816T, 7-bit address 0x15. Register map: 0x01 gesture, 0x02 finger count,
// 0x03 XH / 0x04 XL / 0x05 YH / 0x06 YL (low nibble of the high byte holds bits 8-11).
class Cst816Driver : public I2cTouchDriver {
public:
    Cst816Driver() : I2cTouchDriver(0x15) { setName("cst816"); }

protected:
    bool probe() override;
    bool readPoint(uint16_t* x, uint16_t* y) override;
};

#endif // KRYONOS_CAPACITIVE_TOUCH_DRIVER_H
