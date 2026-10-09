#include "Hal/Touch/CapacitiveTouchDriver.h"

#include "Hal/Display/Display.h"
// Display.h deliberately does not pull in the backend interface (ITouchDriver.h forward-declares
// KryonDisplay so the resistive drivers can take a pointer without one), but toCanvas() below CALLS
// panelToCanvas() on it, which needs the complete type.
#include "Hal/Display/KryonDisplay.h"
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

// Map a controller-native coordinate onto the live canvas.
//
// On most boards the controller's grid IS the canvas and this is a clamp: a value past the edge is
// pulled back rather than allowed to produce an out-of-bounds tap. But on a board whose canvas is
// scaled into a larger panel -- the round Waveshare 2.1B, where a 240x320 canvas is blitted 6/5 into
// a 480x480 circle -- the controller reports PANEL pixels, so a clamp alone would put every tap up
// to 2x off. Those backends own the inverse via KryonDisplay::panelToCanvas, and this asks them for
// it rather than keeping a second copy of the scale and offset that can drift from the blit's.
static bool toCanvas(KryonDisplay* display, uint16_t nativeX, uint16_t nativeY, uint16_t* x,
                     uint16_t* y) {
    const int16_t w = Display::width();
    const int16_t h = Display::height();
    if (w <= 0 || h <= 0) return false;

    int32_t cx = nativeX;
    int32_t cy = nativeY;

    // A tap outside the drawn area is a REJECTION, not a clamp. Clamping it would turn the black
    // ring around the canvas into a band of edge taps -- exactly the failure the aperture exists to
    // fix -- because the bezel's pixels are the ones a finger misses by the most.
    if (display && !display->panelToCanvas(nativeX, nativeY, &cx, &cy)) return false;

    *x = (cx >= w) ? static_cast<uint16_t>(w - 1) : static_cast<uint16_t>(cx < 0 ? 0 : cx);
    *y = (cy >= h) ? static_cast<uint16_t>(h - 1) : static_cast<uint16_t>(cy < 0 ? 0 : cy);
    return true;
}

// --- Shared lifecycle --------------------------------------------------------------------------

void I2cTouchDriver::begin(KryonDisplay* display) {
    // Kept rather than discarded: toCanvas() asks it to invert the blit's transform. Null is
    // tolerated -- the clamp above is then the whole mapping, which is correct for a 1:1 backend.
    display_ = display;

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
    return toCanvas(display_, nativeX, nativeY, x, y);
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
    // Same repeated-START requirement as the GT1151 below: this is the other Goodix part in the
    // file, it had the identical writeRaw/readRaw defect, and neither driver had been run against a
    // panel before -- see the note on the capacitive set. Fixed here rather than left as a known
    // copy of the same bug.
    std::vector<uint8_t> data;
    if (!I2CEngine::readRegBytes16(address_, reg, length, data) || data.size() < length) return false;
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
    if (id < 0 || id == 0xFF) return false;

    // Stop the controller dropping into its low-power state between touches, which it otherwise does
    // and in which it stops acknowledging I2C.
    //
    // The reference port for this board does this at init and we did not
    // (X:\KryonOS .../waveshare_2_1/cst820.cpp begin(): writeReg(REG_DIS_AUTOSLEEP, 0x01)). Note what
    // that port says about it: its reads are DELIBERATELY ungated on INT, because "the I2C traffic is
    // itself the wake-up" -- so a sleeping part is woken by the very read that would find it asleep,
    // and this write is not by itself the explanation for dead touch. It is here because the vendor
    // sets it, and a part that never sleeps has fewer ways to be missed.
    //
    // Failure is ignored: a controller that refuses the write still answers reads, and probe() runs
    // on a retry loop where a per-attempt line would flood.
    if (!I2CEngine::writeReg(address_, 0xFE, 0x01)) {
        static bool reported = false;
        if (!reported) {
            reported = true;
            Serial.println("[TOUCH] cst816 auto-sleep-off write (0xFE) refused; reads are ungated, harmless");
        }
    }
    return true;
}

bool Cst816Driver::readPoint(uint16_t* x, uint16_t* y) {
    // ONE burst from 0x02, and this is correctness rather than an optimisation.
    //
    // The five fields are a single snapshot of one touch sample, so reading them as five separate
    // transactions lets the coordinates TEAR: the controller can finish a finger while the CPU is
    // between reads, and X then comes from one sample and Y from the next. On this board that
    // surfaced as raw reads like (1792, 0) -- off a 480 px panel entirely -- which the aperture guard
    // correctly rejected, so the symptom was touch that was simply dead rather than visibly wrong.
    //
    // Both references agree, which is what makes this the bug rather than a guess: the port this
    // board was written against reads the payload in one burst for exactly this reason
    // (X:\KryonOS .../waveshare_2_1/cst820.cpp: "the 5-byte payload is read in one burst so the
    // coordinates cannot tear between two I2C transactions"), and the vendor driver it was itself
    // written against -- esp_lcd_touch_cst816s -- reads data_t through a single rx_param call.
    // readRegBytes is the same repeated-start handshake they use, not a STOP followed by a read.
    //
    //   b[0] finger count   reg 0x02
    //   b[1] X high nibble  reg 0x03
    //   b[2] X low byte     reg 0x04
    //   b[3] Y high nibble  reg 0x05
    //   b[4] Y low byte     reg 0x06
    std::vector<uint8_t> b;
    if (!I2CEngine::readRegBytes(address_, 0x02, 5, b) || b.size() < 5) return false;
    if ((b[0] & 0x0F) == 0) return false;

    const uint16_t rx = (uint16_t)(((b[1] & 0x0F) << 8) | b[2]);
    const uint16_t ry = (uint16_t)(((b[3] & 0x0F) << 8) | b[4]);

    // The first few touches report their raw coordinates, so a controller that is reporting
    // something other than panel pixels says so in the boot log instead of presenting as dead
    // touch. Capped, so it cannot flood at touch rate. Eight is the reference port's number.
    if (touchesLogged_ < 8) {
        touchesLogged_++;
        Serial.printf("[TOUCH] cst816 raw=(%u,%u) num=%u\n", (unsigned)rx, (unsigned)ry,
                      (unsigned)b[0]);
    }

    *x = rx;
    *y = ry;
    return true;
}

// --- GT1151 --------------------------------------------------------------------------------------
//
// The protocol is esp_lcd_touch_gt1151's, ported to this file's I2C helpers. See Gt1151Driver in the
// header for the three ways it differs from the GT911 sitting above it -- the missing ready bit, the
// frame checksum, and the 8-byte point stride.

bool Gt1151Driver::writeReg8(uint16_t reg, uint8_t value) {
    uint8_t frame[3] = {(uint8_t)(reg >> 8), (uint8_t)(reg & 0xFF), value};
    return I2CEngine::writeRaw(address_, frame, sizeof(frame));
}

bool Gt1151Driver::readReg16(uint16_t reg, uint8_t* out, size_t length) {
    // Repeated START, via I2CEngine::readRegBytes16. The writeRaw/readRaw pair this replaces ended
    // the register write with a STOP, and a Goodix part then answers from wherever its pointer was
    // left rather than from the register asked for -- which is why the probe "succeeded" on bytes
    // that were never the product id, with dead touch behind a driver reporting itself ready.
    std::vector<uint8_t> data;
    if (!I2CEngine::readRegBytes16(address_, reg, length, data) || data.size() < length) return false;
    for (size_t i = 0; i < length; i++) out[i] = data[i];
    return true;
}

bool Gt1151Driver::probe() {
    if (!I2CEngine::ping(address_)) return false;

    // 0x8140 is the product id. Reading it is what distinguishes a controller that is really there
    // from one that merely holds the bus -- so this checks the CONTENT, which the first version did
    // not: a bare "did the read return bytes" is true on an idle bus too.
    //
    // The three tests are esp_lcd_touch_gt1151's own (touch_gt1151_read_product_id): the 11-byte
    // block sums to non-zero, the first three bytes are alphanumeric, and the sensor-id byte is not
    // the 0xFF an unpopulated bus reads back as.
    uint8_t buf[11] = {0};
    if (!readReg16(0x8140, buf, sizeof(buf))) return false;

    uint8_t checksum = 0;
    for (size_t i = 0; i < sizeof(buf); i++) checksum = (uint8_t)(checksum + buf[i]);
    if (checksum == 0) return false;
    if (buf[10] == 0xFF) return false;

    auto alnum = [](uint8_t c) {
        return (c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z');
    };
    if (!alnum(buf[0]) || !alnum(buf[1]) || !alnum(buf[2])) return false;

    return true;
}

bool Gt1151Driver::readPoint(uint16_t* x, uint16_t* y) {
    // 0x814E is the report register: one status byte in, the whole frame readable from the same
    // address. The vendor reads it twice -- once for the count, once for the frame.
    uint8_t status = 0;
    if (!readReg16(0x814E, &status, 1)) return false;

    const int points = status & 0x0F;

    // MAX_TOUCH_NUM in the vendor driver is 10; anything larger is a garbled read rather than a
    // ten-fingered report. The register is also a latch, so it is cleared on every scan -- including
    // the paths that report nothing, or the controller stops publishing new frames.
    if (points == 0 || points > 10) {
        writeReg8(0x814E, 0);
        return false;
    }

    // status + one 8-byte record + the 2-byte checksum, matching DATA_BUFF_LEN(1).
    uint8_t buf[11] = {0};
    if (!readReg16(0x814E, buf, sizeof(buf))) return false;

    // Clear only after the frame is captured, as the vendor does: writing 0 first would discard the
    // report this scan is about to read.
    writeReg8(0x814E, 0);

    // Every byte of the frame, status included, sums to zero.
    uint8_t sum = 0;
    for (size_t i = 0; i < sizeof(buf); i++) sum = (uint8_t)(sum + buf[i]);
    if (sum != 0) {
        if (!checksumWarned_) {
            checksumWarned_ = true;
            Serial.printf("[TOUCH] gt1151 frame checksum failed (sum 0x%02X); frame ignored. Every "
                          "frame failing is an address or wiring fault, not a panel one.\n",
                          sum);
        }
        return false;
    }

    // The record begins at offset 1: byte 0 is the id nibble and four reserved bits, then X and Y
    // little-endian, then strength, then a spare. X and Y are native panel pixels -- the controller is
    // configured with the panel's own size -- so neither is masked, unlike the GT911's 12-bit fields.
    *x = (uint16_t)(buf[1] | ((uint16_t)buf[2] << 8));
    *y = (uint16_t)(buf[3] | ((uint16_t)buf[4] << 8));
    return true;
}
