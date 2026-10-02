#include "Hal/Touch/Xpt2046BitbangDriver.h"

#include "Hal/Display/Display.h"
#include "Hal/Touch/TouchConfig.h"

#include <Arduino.h>
#include <TFT_eSPI.h>

// This translation unit is the only one that touches the XPT2046 GPIOs, so every T_*_PIN use is
// guarded here rather than leaking into the facade or the factory.
#if KRYONOS_TOUCH_HAS_BITBANG_PINS

static inline void touchBitbangDelay() {
    delayMicroseconds(2);
}

uint16_t Xpt2046BitbangDriver::transfer16(uint8_t cmd) {
    digitalWrite(T_CS_PIN, LOW);
    touchBitbangDelay();

    // 1. Send 8-bit command
    for (int i = 7; i >= 0; i--) {
        digitalWrite(T_DIN_PIN, (cmd >> i) & 0x01);
        touchBitbangDelay();
        digitalWrite(T_CLK_PIN, HIGH);
        touchBitbangDelay();
        digitalWrite(T_CLK_PIN, LOW);
        touchBitbangDelay();
    }

    // 2. BUSY clock cycle
    digitalWrite(T_CLK_PIN, HIGH);
    touchBitbangDelay();
    digitalWrite(T_CLK_PIN, LOW);
    touchBitbangDelay();

    // 3. Read 12-bit result (MSB first)
    uint16_t data = 0;
    for (int i = 11; i >= 0; i--) {
        digitalWrite(T_CLK_PIN, HIGH);
        touchBitbangDelay();
        if (digitalRead(T_DO_PIN)) {
            data |= (1 << i);
        }
        digitalWrite(T_CLK_PIN, LOW);
        touchBitbangDelay();
    }

    // 4. Dummy clocks to finish 24-bit cycle & restore PENIRQ
    for (int i = 0; i < 3; i++) {
        digitalWrite(T_CLK_PIN, HIGH);
        touchBitbangDelay();
        digitalWrite(T_CLK_PIN, LOW);
        touchBitbangDelay();
    }

    digitalWrite(T_CS_PIN, HIGH);
    return data;
}

void Xpt2046BitbangDriver::begin(TFT_eSPI* tft) {
    tft_ = tft;

    pinMode(T_CS_PIN, OUTPUT);
    digitalWrite(T_CS_PIN, HIGH);
    pinMode(T_CLK_PIN, OUTPUT);
    digitalWrite(T_CLK_PIN, LOW);
    pinMode(T_DIN_PIN, OUTPUT);
    digitalWrite(T_DIN_PIN, LOW);
    pinMode(T_DO_PIN, INPUT);

#if defined(T_IRQ_PIN) && (T_IRQ_PIN >= 0)
    pinMode(T_IRQ_PIN, INPUT_PULLUP);
#endif
}

bool Xpt2046BitbangDriver::getTouchRaw(uint16_t* x, uint16_t* y) {
    // Check IRQ pin if available
#if defined(T_IRQ_PIN) && (T_IRQ_PIN >= 0)
    if (digitalRead(T_IRQ_PIN) == HIGH) {
        return false; // Not touched (IRQ is active LOW)
    }
#endif

    // Read Z1 & Z2 pressure
    uint16_t z1 = transfer16(0xB0); // Z1
    uint16_t z2 = transfer16(0xC0); // Z2
    int16_t z = (int16_t)z1 + 4095 - (int16_t)z2;
    if (z < kryon_touch::kMinPressure || z1 < kryon_touch::kMinZ1) {
        return false; // Pressure too low
    }

    // Over-sample for stable reading
    uint32_t rx = 0, ry = 0;
    const int SAMPLES = 4;
    for (int i = 0; i < SAMPLES; i++) {
        rx += transfer16(0xD0); // X-position (12-bit)
        ry += transfer16(0x90); // Y-position (12-bit)
    }
    *x = rx / SAMPLES;
    *y = ry / SAMPLES;

    if (*x < kryon_touch::kRawValidMin || *x > kryon_touch::kRawValidMax ||
        *y < kryon_touch::kRawValidMin || *y > kryon_touch::kRawValidMax) {
        return false;
    }
    return true;
}

bool Xpt2046BitbangDriver::getTouch(uint16_t* x, uint16_t* y, uint16_t threshold) {
    (void)threshold; // Pressure is evaluated inside getTouchRaw on this path.

    uint16_t rawX = 0, rawY = 0;
    if (!getTouchRaw(&rawX, &rawY)) {
        return false;
    }

    // The canvas is read at runtime (post-rotation) rather than hard-coded, so a board at any
    // resolution maps correctly.
    return kryon_touch::mapRawToPixels(cal_, rawX, rawY,
                                       Display::width(), Display::height(), x, y);
}

void Xpt2046BitbangDriver::setCalibration(const uint16_t* parameters) {
    cal_.set(parameters);
}

static void drawCornerArrow(TFT_eSPI* tft, uint8_t corner, uint8_t size, uint32_t color,
                            int16_t width, int16_t height) {
    if (!tft) return;
    switch (corner) {
        case 0: // Top-Left
            tft->drawLine(0, 0, 0, size, color);
            tft->drawLine(0, 0, size, 0, color);
            tft->drawLine(0, 0, size, size, color);
            break;
        case 1: // Bottom-Left
            tft->drawLine(0, height - size - 1, 0, height - 1, color);
            tft->drawLine(0, height - 1, size, height - 1, color);
            tft->drawLine(size, height - size - 1, 0, height - 1, color);
            break;
        case 2: // Top-Right
            tft->drawLine(width - size - 1, 0, width - 1, 0, color);
            tft->drawLine(width - size - 1, size, width - 1, 0, color);
            tft->drawLine(width - 1, size, width - 1, 0, color);
            break;
        case 3: // Bottom-Right
            tft->drawLine(width - size - 1, height - size - 1, width - 1, height - 1, color);
            tft->drawLine(width - 1, height - 1 - size, width - 1, height - 1, color);
            tft->drawLine(width - 1 - size, height - 1, width - 1, height - 1, color);
            break;
    }
}

void Xpt2046BitbangDriver::calibrate(uint16_t* parameters, uint32_t color_fg, uint32_t color_bg,
                                     uint8_t size) {
    if (!tft_ || !parameters) return;

    tft_->fillScreen(color_bg);
    tft_->setTextColor(color_fg, color_bg);
    tft_->setTextDatum(MC_DATUM);
    tft_->drawString("Touch the arrows", Display::centerX(), Display::centerY(), 2);

    int16_t values[] = {0, 0, 0, 0, 0, 0, 0, 0};
    uint16_t x_tmp, y_tmp;
    int16_t _width = tft_->width();
    int16_t _height = tft_->height();

    for (uint8_t i = 0; i < 4; i++) {
        // Clear all 4 arrow positions
        tft_->fillRect(0, 0, size + 1, size + 1, color_bg);
        tft_->fillRect(0, _height - size - 1, size + 1, size + 1, color_bg);
        tft_->fillRect(_width - size - 1, 0, size + 1, size + 1, color_bg);
        tft_->fillRect(_width - size - 1, _height - size - 1, size + 1, size + 1, color_bg);

        // Draw active arrow in RED (color_fg)
        drawCornerArrow(tft_, i, size, color_fg, _width, _height);

        // Wait for user to touch the arrow
        while (!getTouchRaw(&x_tmp, &y_tmp)) {
            delay(10);
        }

        // Visual feedback: change arrow color to GREEN while pressed!
        drawCornerArrow(tft_, i, size, TFT_GREEN, _width, _height);

        // Accumulate 8 samples while touching
        for (uint8_t j = 0; j < 8; j++) {
            while (!getTouchRaw(&x_tmp, &y_tmp)) {
                delay(5);
            }
            values[i * 2] += x_tmp;
            values[i * 2 + 1] += y_tmp;
            delay(10);
        }
        values[i * 2] /= 8;
        values[i * 2 + 1] /= 8;

        // Wait for touch release
        while (getTouchRaw(&x_tmp, &y_tmp)) {
            delay(10);
        }

        // Clear arrow area
        tft_->fillRect(0, 0, size + 1, size + 1, color_bg);
        tft_->fillRect(0, _height - size - 1, size + 1, size + 1, color_bg);
        tft_->fillRect(_width - size - 1, 0, size + 1, size + 1, color_bg);
        tft_->fillRect(_width - size - 1, _height - size - 1, size + 1, size + 1, color_bg);

        delay(300); // Debounce between corners
    }

    // Calculate calibration parameters matching TFT_eSPI algorithm
    bool rotate = false;
    uint16_t x0, x1, y0, y1;
    if (abs(values[0] - values[2]) > abs(values[1] - values[3])) {
        rotate = true;
        x0 = (values[1] + values[3]) / 2;
        x1 = (values[5] + values[7]) / 2;
        y0 = (values[0] + values[4]) / 2;
        y1 = (values[2] + values[6]) / 2;
    } else {
        x0 = (values[0] + values[2]) / 2;
        x1 = (values[4] + values[6]) / 2;
        y0 = (values[1] + values[5]) / 2;
        y1 = (values[3] + values[7]) / 2;
    }

    bool invert_x = false;
    if (x0 > x1) {
        uint16_t tmp = x0;
        x0 = x1;
        x1 = tmp;
        invert_x = true;
    }

    bool invert_y = false;
    if (y0 > y1) {
        uint16_t tmp = y0;
        y0 = y1;
        y1 = tmp;
        invert_y = true;
    }

    x1 -= x0;
    y1 -= y0;

    if (x0 == 0) x0 = 1;
    if (x1 == 0) x1 = 1;
    if (y0 == 0) y0 = 1;
    if (y1 == 0) y1 = 1;

    parameters[0] = x0;
    parameters[1] = x1;
    parameters[2] = y0;
    parameters[3] = y1;
    parameters[4] = (rotate ? 1 : 0) | (invert_x ? 2 : 0) | (invert_y ? 4 : 0);

    setCalibration(parameters);
}

#endif // KRYONOS_TOUCH_HAS_BITBANG_PINS

// Boards without the four pins still reference this class (the factory names it as one of the
// choices), so the vtable and the constructor need real symbols. These stubs are unreachable:
// the factory only builds this driver when the pins exist.
#if !KRYONOS_TOUCH_HAS_BITBANG_PINS

uint16_t Xpt2046BitbangDriver::transfer16(uint8_t cmd) { (void)cmd; return 0; }
void Xpt2046BitbangDriver::begin(TFT_eSPI* tft) { tft_ = tft; }
bool Xpt2046BitbangDriver::getTouchRaw(uint16_t* x, uint16_t* y) { (void)x; (void)y; return false; }
bool Xpt2046BitbangDriver::getTouch(uint16_t* x, uint16_t* y, uint16_t threshold) {
    (void)x; (void)y; (void)threshold;
    return false;
}
void Xpt2046BitbangDriver::setCalibration(const uint16_t* parameters) { cal_.set(parameters); }
void Xpt2046BitbangDriver::calibrate(uint16_t* parameters, uint32_t color_fg, uint32_t color_bg,
                                     uint8_t size) {
    (void)parameters; (void)color_fg; (void)color_bg; (void)size;
}

#endif // !KRYONOS_TOUCH_HAS_BITBANG_PINS
