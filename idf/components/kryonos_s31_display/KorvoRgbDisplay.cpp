// The ESP32-S31-Korvo-1 display backend. See KorvoRgbDisplay.h for why it is this short: Espressif's
// BSP owns the panel, the bus, the timings and the frame buffers, and this file owns only the two
// things a BSP cannot give -- a rasterizer call surface, and a way to put a finished frame on screen.

#include "KorvoRgbDisplay.h"

#include <Arduino.h>

#include <stdio.h>
#include <string.h>

#include <esp_err.h>

#include "bsp/display.h"

// =================================================================================================
// Lifecycle
// =================================================================================================

KorvoRgbDisplay::KorvoRgbDisplay(int16_t width, int16_t height)
    : nativeW_(width), nativeH_(height), w_(width), h_(height) {}

KorvoRgbDisplay::~KorvoRgbDisplay() {
    if (canvas_.created()) canvas_.deleteSprite();
    // The panel itself belongs to the BSP (bsp_display_delete), which the BSP's own teardown runs.
    // Deleting it here would free a handle another owner is still holding.
    panel_ = nullptr;
}

bool KorvoRgbDisplay::ready() const { return panel_ != nullptr && canvasReady_; }

void KorvoRgbDisplay::init(uint8_t tc) {
    (void)tc;
    if (panel_) return; // idempotent: Display::begin() may run more than once

    Serial.printf("[Display:korvo] bringing up the %dx%d RGB panel through the Korvo-1 BSP\n",
                  static_cast<int>(nativeW_), static_cast<int>(nativeH_));

    const bsp_display_config_t bspCfg = {};
    bsp_lcd_handles_t handles = {};
    esp_err_t err = bsp_display_new_with_handles(&bspCfg, &handles);
    if (err != ESP_OK || !handles.panel) {
        lastError_ = esp_err_to_name(err);
        Serial.printf("[Display:korvo] FAILED at panel bring-up: %s -- drawing disabled.\n",
                      lastError_);
        return;
    }
    panel_ = handles.panel;

    // BOTH of the BSP's frame buffers, by pointer. Presenting is then picking one of these rather
    // than handing the driver a staging buffer to copy into -- rgb_panel_draw_bitmap recognises an
    // address inside its own frame buffer, adopts it and copies nothing.
    err = esp_lcd_rgb_panel_get_frame_buffer(panel_, 2, &fb_[0], &fb_[1]);
    if (err != ESP_OK || !fb_[0] || !fb_[1]) {
        lastError_ = "the panel does not expose two frame buffers (is BSP_LCD_RGB_BUFFER_NUMS 2?)";
        Serial.printf("[Display:korvo] FAILED at frame buffers: %s -- drawing disabled.\n",
                      lastError_);
        panel_ = nullptr;
        return;
    }

    canvas_.setColorDepth(16);
    if (!canvas_.createSprite(nativeW_, nativeH_)) {
        lastError_ = "16bpp canvas allocation failed";
        Serial.printf("[Display:korvo] FAILED at canvas: %s -- drawing disabled.\n", lastError_);
        panel_ = nullptr;
        return;
    }
    canvasReady_ = true;

    // Black, and black in both frame buffers, before the display is switched on. The driver's
    // scanout is already running against freshly allocated PSRAM at this point, so filling only the
    // canvas would leave the other buffer full of noise to appear on the first flip. The display is
    // still off (the BSP leaves the DISP GPIO inactive), so none of this is visible.
    canvas_.fillScreen(0x0000);
    const size_t fbBytes = static_cast<size_t>(nativeW_) * static_cast<size_t>(nativeH_) *
                           sizeof(uint16_t);
    for (size_t i = 0; i < 2; i++) {
        memcpy(fb_[i], canvas_.pixels(), fbBytes);
    }
    // The scanout came up on buffer 0, so the first present must write the other one -- see nextFb_.
    nextFb_ = 1;

    err = esp_lcd_panel_disp_on_off(panel_, true);
    if (err != ESP_OK) {
        Serial.printf("[Display:korvo] esp_lcd_panel_disp_on_off failed: %s\n", esp_err_to_name(err));
    }

    // Returns ESP_ERR_NOT_SUPPORTED on this board BY DESIGN: BSP_LCD_BACKLIGHT is GPIO_NUM_NC, so
    // there is no pin to drive and the backlight is hardwired on. Calling it anyway keeps the
    // bring-up sequence identical to every other backend, and the result is logged rather than
    // checked -- a "failure" here would be a false alarm.
    const esp_err_t blErr = bsp_display_backlight_on();
    Serial.printf("[Display:korvo] ready: canvas %dx%d, 2 frame buffers, backlight %s (%s)\n",
                  static_cast<int>(w_), static_cast<int>(h_),
                  blErr == ESP_OK ? "on" : "always-on (no control)",
                  esp_err_to_name(blErr));
}

void KorvoRgbDisplay::setRotation(uint8_t rotation) {
    const uint8_t requested = rotation & 0x03;
    // A quarter turn (1 or 3) swaps the axes, which for this backend would relabel an 800x480 canvas
    // as 480x800 and then clip it into the frame buffer. The RGB scanout's orientation is fixed by
    // how the panel is wired, so there is nothing to turn: say so once and stay put.
    if (requested != 0 && !rotationWarned_) {
        rotationWarned_ = true;
        Serial.printf("[Display:korvo] rotation %u requested, but this panel is fixed %dx%d "
                      "landscape; staying at 0. Set KRYONOS_DISPLAY_ROTATION=0 for this board.\n",
                      static_cast<unsigned>(requested), static_cast<int>(nativeW_),
                      static_cast<int>(nativeH_));
    }
    rotation_ = 0;
    canvas_.setRotation(0);
    w_ = canvas_.created() ? canvas_.width() : nativeW_;
    h_ = canvas_.created() ? canvas_.height() : nativeH_;
}

uint8_t KorvoRgbDisplay::getRotation() { return rotation_; }
int16_t KorvoRgbDisplay::width() { return w_; }
int16_t KorvoRgbDisplay::height() { return h_; }

void KorvoRgbDisplay::setBacklight(uint8_t brightness) {
    // No-op with a reason, once. The board has no backlight pin, so brightness cannot be changed at
    // all -- the BSP's own brightness and on/off calls all return ESP_ERR_NOT_SUPPORTED. Recording
    // the request keeps the behaviour legible from a log instead of looking like a silent failure.
    if (!backlightWarned_) {
        backlightWarned_ = true;
        Serial.printf("[Display:korvo] setBacklight(%u) is a no-op: this board's backlight is "
                      "hardwired on (BSP_LCD_BACKLIGHT is GPIO_NUM_NC).\n",
                      static_cast<unsigned>(brightness));
    }
}

// =================================================================================================
// Frame presentation
// =================================================================================================

void KorvoRgbDisplay::present() {
    if (!panel_ || !canvasReady_ || !fb_[0] || !dirty_) return;

    uint16_t* dst = static_cast<uint16_t*>(fb_[nextFb_]);
    const uint16_t* src = canvas_.pixels();
    if (!dst || !src) return;

    const size_t fbBytes = static_cast<size_t>(nativeW_) * static_cast<size_t>(nativeH_) *
                           sizeof(uint16_t);
    memcpy(dst, src, fbBytes);

    // Handing the driver a pointer that IS one of its own frame buffers is what makes it adopt this
    // one: rgb_panel_draw_bitmap finds the address inside fbs[], sets cur_fb_index and copies
    // nothing. In bounce-buffer mode -- which is how this board is configured -- the scanout keeps
    // reading the previous buffer until bb_fb_index re-latches at the frame wrap, so the switch
    // lands on a boundary rather than mid-frame. That is the entire anti-tearing story here; the
    // Waveshare's ISR handshake is not needed because this driver does not anchor the scanout.
    //
    // No cache maintenance on our side: with a bounce buffer the driver reads the frame buffer with
    // the CPU (coherent by construction), and its own write-back is what feeds the DMA.
    esp_lcd_panel_draw_bitmap(panel_, 0, 0, w_, h_, fb_[nextFb_]);
    nextFb_ = static_cast<uint8_t>(nextFb_ ^ 1u);

    dirty_ = false;
}

// =================================================================================================
// Shapes
//
// Forwarded straight to the canvas. KryonSprite is not TFT_eSprite: its composites (round rects,
// circles, triangles) are real implementations, forwarded in turn to RamFramebufferDisplay, which is
// the same geometry the TFT_eSPI boards use. So unlike the Waveshare backend there is nothing to
// rebuild here on top of drawPixel/hLine/vLine -- the calls just go through.
// =================================================================================================

void KorvoRgbDisplay::fillScreen(uint32_t color) {
    if (!canvasReady_) return;
    canvas_.fillScreen(static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!canvasReady_ || w <= 0 || h <= 0) return;
    canvas_.fillRect(x, y, w, h, static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!canvasReady_ || w <= 0 || h <= 0) return;
    canvas_.drawRect(x, y, w, h, static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                                    uint32_t color) {
    if (!canvasReady_ || w <= 0 || h <= 0) return;
    canvas_.drawRoundRect(x, y, w, h, radius, static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                                    uint32_t color) {
    if (!canvasReady_ || w <= 0 || h <= 0) return;
    canvas_.fillRoundRect(x, y, w, h, radius, static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::drawLine(int32_t xs, int32_t ys, int32_t xe, int32_t ye, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.drawLine(xs, ys, xe, ye, static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t color) {
    if (!canvasReady_ || h <= 0) return;
    canvas_.drawFastVLine(x, y, h, static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t color) {
    if (!canvasReady_ || w <= 0) return;
    canvas_.drawFastHLine(x, y, w, static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::drawPixel(int32_t x, int32_t y, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.drawPixel(x, y, static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::drawCircle(int32_t x, int32_t y, int32_t r, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.drawCircle(x, y, r, static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::fillCircle(int32_t x, int32_t y, int32_t r, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.fillCircle(x, y, r, static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::drawTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3,
                                   int32_t y3, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.drawTriangle(x1, y1, x2, y2, x3, y3, static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::fillTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3,
                                   int32_t y3, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.fillTriangle(x1, y1, x2, y2, x3, y3, static_cast<uint16_t>(color));
    dirty_ = true;
}

void KorvoRgbDisplay::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* data) {
    if (!canvasReady_ || !data || w <= 0 || h <= 0) return;
    // The canvas, the sprite buffers it is handed and the panel frame buffer all hold native
    // little-endian RGB565, so this is a straight copy with no byte swap anywhere in the pipeline.
    canvas_.pushImage(x, y, w, h, data);
    dirty_ = true;
}

// =================================================================================================
// Text -- the canvas owns the ported TFT_eSPI font engine, so glyphs and metrics here are the same
// ones the S3 and classic-ESP32 boards produce.
// =================================================================================================

void KorvoRgbDisplay::setTextColor(uint16_t color) { canvas_.setTextColor(color); }

void KorvoRgbDisplay::setTextColor(uint16_t fgcolor, uint16_t bgcolor, bool bgfill) {
    canvas_.setTextColor(fgcolor, bgcolor, bgfill);
}

void KorvoRgbDisplay::setTextDatum(uint8_t datum) { canvas_.setTextDatum(datum); }
void KorvoRgbDisplay::setTextSize(uint8_t size) { canvas_.setTextSize(size); }

void KorvoRgbDisplay::drawString(const char* string, int32_t x, int32_t y) {
    if (!canvasReady_ || !string) return;
    canvas_.drawString(string, x, y);
    dirty_ = true;
}

void KorvoRgbDisplay::drawString(const char* string, int32_t x, int32_t y, uint8_t font) {
    if (!canvasReady_ || !string) return;
    canvas_.drawString(string, x, y, font);
    dirty_ = true;
}

void KorvoRgbDisplay::drawString(const String& string, int32_t x, int32_t y) {
    drawString(string.c_str(), x, y);
}

void KorvoRgbDisplay::drawString(const String& string, int32_t x, int32_t y, uint8_t font) {
    drawString(string.c_str(), x, y, font);
}

// textWidth is pure font-table arithmetic and touches no buffer, so it is answered even when the
// canvas failed. UiLayout keeps laying out from real metrics instead of collapsing to zeros, which
// is the difference between a readable log and a mystery.
int16_t KorvoRgbDisplay::textWidth(const char* string) {
    return string ? canvas_.textWidth(string) : 0;
}

int16_t KorvoRgbDisplay::textWidth(const char* string, uint8_t font) {
    return string ? canvas_.textWidth(string, font) : 0;
}

int16_t KorvoRgbDisplay::textWidth(const String& string) { return canvas_.textWidth(string.c_str()); }

int16_t KorvoRgbDisplay::textWidth(const String& string, uint8_t font) {
    return canvas_.textWidth(string.c_str(), font);
}

// =================================================================================================
// Colour and bus helpers
// =================================================================================================

uint16_t KorvoRgbDisplay::color565(uint8_t red, uint8_t green, uint8_t blue) {
    return KryonSprite::color565(red, green, blue);
}

void KorvoRgbDisplay::setSwapBytes(bool swap) {
    // Nothing to do. TFT_eSprite's byte swap exists because it was built to feed an SPI panel; the
    // RGB path here is native end to end. Accepted so a caller written against TFT_eSPI compiles.
    (void)swap;
}

void KorvoRgbDisplay::startWrite() {}
void KorvoRgbDisplay::endWrite() {}
