// See EspLcdTouchDriver.h. Under PlatformIO this file expands to an empty translation unit.

#include "Hal/Touch/EspLcdTouchDriver.h"

#if defined(KRYONOS_TOUCH_USE_ESP_LCD)

#include <Arduino.h>

#include "bsp/touch.h"

// ITouchDriver.h forward-declares KryonDisplay so a driver can take a pointer without one, but
// getTouch() below CALLS width(), height() and panelToCanvas() on it, which needs the complete type.
// Same include and same reason as CapacitiveTouchDriver.cpp.
#include "Hal/Display/KryonDisplay.h"

namespace {
// See the retry in begin(): the controller's reset is shared with the panel's, so the first product-id
// read can land before the GT1151 has finished coming up. Three attempts is enough to cover that
// window without turning a genuinely dead controller into a long boot.
constexpr int kBringUpAttempts = 3;
constexpr uint32_t kBringUpRetryMs = 120;

// How long a contact may go without a fresh report before it is treated as released. Deliberately
// measured against the controller's own report rate, not against the poll rate: see getTouchRaw().
constexpr uint32_t kHoldMs = 80;
} // namespace

void EspLcdTouchDriver::begin(KryonDisplay* display) {
    // Kept rather than discarded: getTouch() asks it to invert the blit's transform, so a backend
    // that scales its canvas into a larger panel gets touch that follows the blit. Null is tolerated
    // -- the clamp in getTouch() is then the whole mapping, which is correct for a 1:1 backend.
    display_ = display;

    if (touch_) return; // idempotent: init() may run more than once per boot

    // bsp_touch_new() does the bring-up: it creates (or joins) the I2C master bus and constructs the
    // right esp_lcd_touch part driver. On this board that is the GT1151 family driver, and the fitted
    // part reports itself as GT1158_000101.
    //
    // Retried, because it does NOT reset the controller: it passes rst_gpio_num = GPIO_NUM_NC with
    // the BSP's own comment "usually shared with LCD reset". On this board it evidently is, so the
    // panel init that ran moments earlier is what pulses that shared line -- and the GT1151 is still
    // bringing its firmware up when the driver reads its product id. That read then fails with
    // ESP_ERR_INVALID_RESPONSE on roughly one boot in several. It is a bring-up race, not a fault, so
    // give the controller time and ask again rather than reporting a working panel as dead.
    esp_err_t err = ESP_FAIL;
    for (int attempt = 1; attempt <= kBringUpAttempts; attempt++) {
        err = bsp_touch_new(nullptr, &touch_);
        if (err == ESP_OK && touch_) break;

        touch_ = nullptr;
        if (attempt == kBringUpAttempts) {
            Serial.printf("[TOUCH] %s bring-up failed after %d attempts: %s\n", name(), attempt,
                          esp_err_to_name(err));
            return;
        }
        Serial.printf("[TOUCH] %s bring-up attempt %d failed: %s -- retrying in %ums\n", name(),
                      attempt, esp_err_to_name(err), (unsigned)kBringUpRetryMs);
        delay(kBringUpRetryMs);
    }

    Serial.printf("[TOUCH] %s ready (polled -- this board wires no touch interrupt line)\n", name());
}

bool EspLcdTouchDriver::getTouchRaw(uint16_t* x, uint16_t* y) {
    if (!touch_ || !x || !y) return false;

    // read_data() pulls the controller's report over I2C; get_data() decodes it. Both are needed and
    // they are deliberately separate calls in esp_lcd_touch -- a caller that only wants the touch
    // count can stop after the first. This is the same call this driver has always made, kept exactly
    // as it was: the contact path is the one already verified against all four corners and the centre.
    if (esp_lcd_touch_read_data(touch_) != ESP_OK) return false;

    esp_lcd_touch_point_data_t points[1] = {};
    uint8_t count = 0;
    if (esp_lcd_touch_get_data(touch_, points, &count, 1) != ESP_OK) return false;

    if (count > 0) {
        *x = points[0].x;
        *y = points[0].y;

        // Kept so a caller can report the panel and canvas numbers of ONE sample. Reading the
        // controller again to get the raw pair would sample a second moment, and the two lines would
        // then describe different touches -- which is exactly how a released 0,0 got printed beside a
        // live canvas point.
        lastRawX_ = *x;
        lastRawY_ = *y;
        lastReportMs_ = millis();
        down_ = true;
        return true;
    }

    // No points in this report -- which is NOT the same as a lifted finger.
    //
    // esp_lcd_touch_gt1151_read_data() masks the status byte with 0x0f before testing it, so "the
    // controller has not finished a scan since you last asked" and "the finger is up" arrive here
    // identically, as zero. get_xy() then zeroes tp->data.points after every copy, so nothing carries
    // over. This loop polls far faster than the controller reports (measured: 500 polls a second),
    // which means most polls land in the gap between two reports -- and treating each of those as a
    // release is what made a finger held still alternate contact/release several times a second, i.e.
    // a button that fires a click every frame.
    //
    // A gap therefore only ends the contact once it has lasted longer than the controller's own report
    // interval by a comfortable margin. A real tap still releases well inside the time the OS allows.
    if (down_) {
        if (millis() - lastReportMs_ < kHoldMs) {
            *x = lastRawX_;
            *y = lastRawY_;
            return true;
        }
        down_ = false;
    }
    return false;
}

bool EspLcdTouchDriver::getTouch(uint16_t* x, uint16_t* y, uint16_t threshold) {
    (void)threshold; // a pressure threshold is an XPT2046 concept; absolute-position parts ignore it

    uint16_t panelX = 0, panelY = 0;
    if (!getTouchRaw(&panelX, &panelY)) return false;

    if (!display_) return false;
    const int16_t w = display_->width();
    const int16_t h = display_->height();
    if (w <= 0 || h <= 0) return false;

    // The backend owns the panel-to-canvas transform, so ask it rather than re-deriving one here --
    // the same rule CapacitiveTouchDriver::toCanvas() applies, and the reason neither driver keeps a
    // copy of a scale or offset that could drift from the blit's.
    int32_t cx = panelX;
    int32_t cy = panelY;
    if (!display_->panelToCanvas(panelX, panelY, &cx, &cy)) return false;

    // A tap past the edge is pulled back rather than dropped: it is a finger at the border, not a
    // different point, and the controller can report one pixel outside on a fast swipe.
    if (cx < 0) cx = 0;
    if (cx >= w) cx = w - 1;
    if (cy < 0) cy = 0;
    if (cy >= h) cy = h - 1;

    *x = static_cast<uint16_t>(cx);
    *y = static_cast<uint16_t>(cy);
    return true;
}

void EspLcdTouchDriver::calibrate(uint16_t* parameters, uint32_t color_fg, uint32_t color_bg,
                                  uint8_t size) {
    // No corner-touch calibration exists for an absolute-position controller, so `parameters` is left
    // exactly as it was found -- a caller that then writes it to /touch_cal_p.bin cannot persist a
    // tuple this driver never produced. needsCalibration() is false, so the OS does not ask anyway.
    (void)parameters;
    (void)color_fg;
    (void)color_bg;
    (void)size;
}

#endif // KRYONOS_TOUCH_USE_ESP_LCD
