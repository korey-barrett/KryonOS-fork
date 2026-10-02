#include "Hal/Touch/CapacitiveTouchDriver.h"

#include "Hal/Display/Display.h"
#include "Hal/I2C/I2CEngine.h"

#if defined(TARGET_WAVESHARE_S3_LCD21B)
#include "Hal/I2C/Tca9554.h"
#endif

#include <Arduino.h>

// --- Optional wiring, resolved once -----------------------------------------------------------
// None of these are required. Without SDA/SCL the driver stays absent and reports no touch, which
// is what keeps an unconfigured board from reading a floating bus as a permanent press.

static void i2cTouchReset() {
#ifdef KRYONOS_TOUCH_RST_PIN
    pinMode(KRYONOS_TOUCH_RST_PIN, OUTPUT);
    digitalWrite(KRYONOS_TOUCH_RST_PIN, LOW);
    delay(10);
    digitalWrite(KRYONOS_TOUCH_RST_PIN, HIGH);
    delay(50);
#endif
}

// Clamp a controller-native coordinate into the live canvas. Capacitive panels report in their own
// native grid, which is usually but not always the canvas size; a value past the edge is clamped
// rather than allowed to produce an out-of-bounds tap.
static bool toCanvas(uint16_t nativeX, uint16_t nativeY, uint16_t* x, uint16_t* y) {
    const int16_t w = Display::width();
    const int16_t h = Display::height();
    if (w <= 0 || h <= 0) return false;
    *x = (nativeX >= (uint16_t)w) ? (uint16_t)(w - 1) : nativeX;
    *y = (nativeY >= (uint16_t)h) ? (uint16_t)(h - 1) : nativeY;
    return true;
}

// --- Shared lifecycle --------------------------------------------------------------------------

void I2cTouchDriver::begin(KryonDisplay* display) {
    (void)display; // capacitive panels report absolute pixels; nothing to draw or delegate

#if defined(KRYONOS_TOUCH_I2C_SDA) && defined(KRYONOS_TOUCH_I2C_SCL)
#ifdef KRYONOS_TOUCH_I2C_ADDR
    address_ = KRYONOS_TOUCH_I2C_ADDR;
#endif
    // The GPIO reset path. A controller whose reset hangs off an I/O expander instead has no
    // KRYONOS_TOUCH_RST_PIN and is handled by the board block below; this stays a no-op there.
    i2cTouchReset();

    if (!I2CEngine::isInitialized()) {
        I2CEngine::begin(KRYONOS_TOUCH_I2C_SDA, KRYONOS_TOUCH_I2C_SCL);
    }

#if defined(TARGET_WAVESHARE_S3_LCD21B)
    // This board's TP_RST is not a SoC pin, so the reset above cannot reach it -- it is EXIO2 on the
    // TCA9554 expander, which is also why this environment defines no TOUCH_RST_PIN macro (that macro
    // would drive a pin nothing is connected to).
    //
    // The pulse has to happen HERE, immediately before the probe, and not during display bring-up.
    // The CST820 drops into a low-power state between touches and stops acknowledging I2C while it is
    // there -- the reference port for this board measured 0/20 ACKs asleep against 20/20 awake, with
    // the expander on the same bus answering perfectly throughout. By the time this runs, a web
    // server and the whole UI have come up, so the controller has long since gone to sleep and a
    // pulse issued minutes earlier during panel bring-up is worthless: the probe below then fails on
    // a controller that is present, wired and entirely healthy.
    //
    // This runs twice per boot -- once from main.cpp's TouchDriver::init(), again from
    // TouchCalibrator::init() -- which is harmless and useful: the second pulse also recovers a
    // controller that went to sleep while the UI was being constructed. The pulse costs 100 ms, twice.
    if (!boardExpander().begin(KRYONOS_TOUCH_I2C_SDA, KRYONOS_TOUCH_I2C_SCL) ||
        !boardExpander().pulseTouchReset()) {
        Serial.printf("[TOUCH] TCA9554 TP_RST pulse failed (%s); probing anyway\n",
                      boardExpander().lastError());
    }
#endif

    // Retry, rather than one shot: the controller needs a moment to come up after that pulse, and a
    // lone read landing inside the wake-up window fails on a healthy part. The reference needs five
    // attempts spaced 20 ms, and the second call is the harder one -- the RGB panel is already
    // scanning and the radio is up, so the bus is busier than it was at the first.
    bool up = false;
    for (int attempt = 0; attempt < 5 && !up; attempt++) {
        if (attempt) delay(20);
        up = probe();
    }

    // A failed re-probe must never disarm a controller that already answered. begin() runs twice per
    // boot, and an earlier success is proof the part is present and wired; clearing present_ here
    // would turn one flaky read into touch that is dead for the rest of the session.
    if (up) {
        present_ = true;
    } else if (present_) {
        Serial.printf("[TOUCH] %s re-probe failed; keeping the earlier successful init\n", name_);
    } else {
        present_ = false;
    }

    Serial.printf("[TOUCH] %s at 0x%02X: %s\n", name_, address_, present_ ? "ready" : "not found");
#else
    present_ = false;
    Serial.printf("[TOUCH] %s not wired (KRYONOS_TOUCH_I2C_SDA/SCL unset)\n", name_);
#endif
}

bool I2cTouchDriver::getTouchRaw(uint16_t* x, uint16_t* y) {
    if (!present_ || !x || !y) return false;
    return readPoint(x, y);
}

bool I2cTouchDriver::getTouch(uint16_t* x, uint16_t* y, uint16_t threshold) {
    (void)threshold;
    uint16_t nativeX = 0, nativeY = 0;
    if (!getTouchRaw(&nativeX, &nativeY)) return false;
    return toCanvas(nativeX, nativeY, x, y);
}

void I2cTouchDriver::calibrate(uint16_t* parameters, uint32_t color_fg, uint32_t color_bg,
                               uint8_t size) {
    // Absolute-position controller: no corner-touch calibration exists. Leave `parameters` alone
    // so a caller that writes it to /touch_cal_p.bin cannot persist a bogus tuple.
    (void)parameters; (void)color_fg; (void)color_bg; (void)size;
}

// --- FT6236 ------------------------------------------------------------------------------------

bool Ft6236Driver::probe() {
    // Reading TD_STATUS proves the controller is present and not merely ACKing its address.
    return I2CEngine::ping(address_) && I2CEngine::readReg(address_, 0x02) >= 0;
}

bool Ft6236Driver::readPoint(uint16_t* x, uint16_t* y) {
    const int touches = I2CEngine::readReg(address_, 0x02);
    if (touches <= 0 || (touches & 0x0F) == 0) return false;

    const int xh = I2CEngine::readReg(address_, 0x03);
    const int xl = I2CEngine::readReg(address_, 0x04);
    const int yh = I2CEngine::readReg(address_, 0x05);
    const int yl = I2CEngine::readReg(address_, 0x06);
    if (xh < 0 || xl < 0 || yh < 0 || yl < 0) return false;

    *x = (uint16_t)(((xh & 0x0F) << 8) | xl);
    *y = (uint16_t)(((yh & 0x0F) << 8) | yl);
    return true;
}

// --- GT911 -------------------------------------------------------------------------------------
// 16-bit register addresses, so the address is written as a raw 2-byte big-endian prefix and the
// payload read straight off the bus (I2CEngine's register helpers take an 8-bit register).

bool Gt911Driver::writeReg8(uint16_t reg, uint8_t value) {
    uint8_t frame[3] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), value };
    return I2CEngine::writeRaw(address_, frame, sizeof(frame));
}

bool Gt911Driver::readReg16(uint16_t reg, uint8_t* out, size_t length) {
    uint8_t addr[2] = { (uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF) };
    if (!I2CEngine::writeRaw(address_, addr, sizeof(addr))) return false;
    std::vector<uint8_t> data;
    if (!I2CEngine::readRaw(address_, length, data) || data.size() < length) return false;
    for (size_t i = 0; i < length; i++) out[i] = data[i];
    return true;
}

bool Gt911Driver::probe() {
    if (!I2CEngine::ping(address_)) return false;
    uint8_t product[4] = {0, 0, 0, 0};
    return readReg16(0x8140, product, sizeof(product));
}

bool Gt911Driver::readPoint(uint16_t* x, uint16_t* y) {
    uint8_t status = 0;
    if (!readReg16(0x814E, &status, 1)) return false;

    const bool ready = (status & 0x80) != 0;
    const int points = status & 0x0F;

    // The status register is write-1-to-clear and must be cleared every scan, ready or not,
    // otherwise the controller stops reporting new points.
    writeReg8(0x814E, 0);

    if (!ready || points == 0) return false;

    uint8_t p[4] = {0, 0, 0, 0};
    if (!readReg16(0x814F, p, sizeof(p))) return false;

    *x = (uint16_t)((p[0] | ((uint16_t)p[1] << 8)) & 0x0FFF);
    *y = (uint16_t)((p[2] | ((uint16_t)p[3] << 8)) & 0x0FFF);
    return true;
}

// --- CST816 ------------------------------------------------------------------------------------

bool Cst816Driver::probe() {
    // Chip ID lives at 0xA7 on the CST816; anything that answers with a plausible non-0xFF byte is
    // treated as present, since the family varies between S and T parts.
    if (!I2CEngine::ping(address_)) return false;
    const int id = I2CEngine::readReg(address_, 0xA7);
    return id >= 0 && id != 0xFF;
}

bool Cst816Driver::readPoint(uint16_t* x, uint16_t* y) {
    const int count = I2CEngine::readReg(address_, 0x02);
    if (count <= 0 || (count & 0x0F) == 0) return false;

    const int xh = I2CEngine::readReg(address_, 0x03);
    const int xl = I2CEngine::readReg(address_, 0x04);
    const int yh = I2CEngine::readReg(address_, 0x05);
    const int yl = I2CEngine::readReg(address_, 0x06);
    if (xh < 0 || xl < 0 || yh < 0 || yl < 0) return false;

    *x = (uint16_t)(((xh & 0x0F) << 8) | xl);
    *y = (uint16_t)(((yh & 0x0F) << 8) | yl);
    return true;
}
