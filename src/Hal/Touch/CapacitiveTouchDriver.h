#ifndef KRYONOS_CAPACITIVE_TOUCH_DRIVER_H
#define KRYONOS_CAPACITIVE_TOUCH_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include "Hal/Touch/ITouchDriver.h"

// ---------------------------------------------------------------------------------------------
// Capacitive (I2C) touch controllers.
//
// STATUS: complete register-level implementations. The CST816 path is what the Waveshare 2.1B
// selects (its CST820 answers to the same map — see the note on Cst816Driver below), and none of
// these has been run against a real panel yet. They compile, they probe the bus, and they report
// "no touch" cleanly when the controller does not answer, so selecting one cannot break a board that
// lacks the hardware. Treat the register maps as documentation to verify against your panel's
// datasheet + the vendor's own driver, not as proven code.
//
// Unlike the resistive path, these report absolute pixel coordinates, so they need no calibration:
// needsCalibration() is false, the OS skips the calibration screen, and /touch_cal_p.bin is unused.
//
// WIRING (all optional, from build flags)
//   KRYONOS_TOUCH_I2C_SDA / KRYONOS_TOUCH_I2C_SCL  — bus pins; without them begin() is a no-op and
//                                                    the driver stays absent.
//   KRYONOS_TOUCH_I2C_ADDR                         — override the default 7-bit address.
//   KRYONOS_TOUCH_RST_PIN / KRYONOS_TOUCH_IRQ_PIN  — reset line and (unused) interrupt line. The
//                                                    reset macro drives a SoC GPIO directly, so a
//                                                    controller whose reset hangs off an I/O
//                                                    expander leaves it undefined and is pulsed by
//                                                    the board instead (the Waveshare 2.1B).
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

    // The display this driver was initialised against. The panel-to-canvas mapping is the BACKEND's
    // to own -- a backend that scales its canvas into a larger panel inverts that same transform
    // here, so the touch path and the blit cannot describe different rectangles. Keeping it means
    // this file holds no copy of the scale and offset to drift from the blit's.
    KryonDisplay* display_ = nullptr;

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
//
// A CST820 answers to the same address with the same map -- 0x02 burst, 0xA7 chip ID, high nibble
// masked, and it ignores the auto-sleep register some drivers write at 0xFE -- so the Waveshare
// ESP32-S3-Touch-LCD-2.1B's touch controller binds here unchanged despite the different part number.
class Cst816Driver : public I2cTouchDriver {
public:
    Cst816Driver() : I2cTouchDriver(0x15) { setName("cst816"); }

protected:
    bool probe() override;
    bool readPoint(uint16_t* x, uint16_t* y) override;

    // How many touches have logged their raw coordinates -- see readPoint(). Capped there, because
    // this runs at touch rate and only the first few readings carry information.
    uint8_t touchesLogged_ = 0;
};

// GT1151 (Goodix, 7-bit address 0x14) -- the touch controller fitted to the ESP32-S31-Korvo-1.
//
// 16-bit register addresses like the GT911 above, but it is NOT the same protocol. Three differences
// matter, and each one silently yields "no touch" if carried over from the GT911:
//
//   * Status 0x814E carries the touch count in its LOW NIBBLE. There is no ready bit -- the GT911's
//     0x80 test finds nothing here, because GT1151 reports readiness through the count alone.
//   * Every frame ends with a 2-byte checksum, and ALL bytes read -- status included -- must sum to
//     zero. The GT911 has no such field.
//   * A point record is 8 bytes, not the GT911's 7: a nibble of id plus four reserved bits, then X,
//     Y, strength, then a spare. The stride is what differs; the X/Y offsets happen to line up.
//
// The protocol below follows espressif/esp_lcd_touch_gt1151 (esp_lcd_touch_gt1151.c: READ_XY_REG,
// DATA_BUFF_LEN, the checksum loop) rather than being inferred from the GT911's map. That component
// is what brought this controller up before, through the BSP -- this is the same sequence without
// the IDF dependency.
class Gt1151Driver : public I2cTouchDriver {
public:
    Gt1151Driver() : I2cTouchDriver(0x14) { setName("gt1151"); }

protected:
    bool probe() override;
    bool readPoint(uint16_t* x, uint16_t* y) override;

private:
    bool readReg16(uint16_t reg, uint8_t* out, size_t length);
    bool writeReg8(uint16_t reg, uint8_t value);

    // One line, once. A checksum that fails on every frame is an address or wiring problem rather than
    // a panel one, and saying so beats a controller that looks present but never reports a touch.
    bool checksumWarned_ = false;

    // Raw-coordinate self-report, as Cst816Driver carries -- the first few DISTINCT contacts. Printing
    // on change rather than on a plain count is the lesson the IDF bring-up diagnostic wrote down
    // (idf/main/main.cpp): under a count alone a held finger fills the budget in the first second,
    // every later tap goes unlogged, and working hardware reads as a sensor stuck on one coordinate.
    uint8_t touchesLogged_ = 0;
    uint16_t lastLoggedX_ = 0xFFFF;
    uint16_t lastLoggedY_ = 0xFFFF;
};

#endif // KRYONOS_CAPACITIVE_TOUCH_DRIVER_H
