// The ESP32-S31-Korvo-1 display backend. See KorvoRgbDisplay.h for why the panel is configured here
// rather than by Espressif's BSP, why it is not in bounce buffer mode, and why present() is not an
// empty method.
//
// This file owns the panel's bring-up. Drawing goes straight into the panel's own frame buffer, so
// present() has no frame to hand over; what it does have to do is write the CPU's cached drawing back
// out to the PSRAM the DMA scans.

#include "KorvoRgbDisplay.h"

#include <Arduino.h>

#include <stdio.h>

#include <driver/gpio.h>
#include <esp_cache.h>
#include <esp_err.h>
#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

#include "UI/UiLayout.h" // uiScale(), so text is sized from the same number the layout is

// =================================================================================================
// The panel, vendored from Espressif's BSP
//
// Every value in this block is copied from espressif__esp32_s31_korvo_1 -- the managed component an
// earlier revision of this backend called into through bsp_display_new_with_handles(). It could not
// stay a dependency: the BSP is an ESP-IDF component, and this board builds under the upstream
// PlatformIO / arduino-esp32 stack, where no such component exists.
//
// So the numbers are the BSP's; keeping them correct is now ours. Sources are named at each one.
// =================================================================================================

namespace {

// From Espressif's board support package for this board -- the 16-bit RGB565 parallel bus.
constexpr int kRgbVsync = 45;
constexpr int kRgbHsync = 44;
constexpr int kRgbDe = 43;
constexpr int kRgbPclk = 40;
constexpr int kRgbDisp = 38;
constexpr int kRgbData[16] = {8,  9,  10, 11, 12, 13, 14, 15,
                              16, 17, 18, 19, 33, 34, 35, 36};

// The same package's panel timings. Note pclk_active_neg: this panel latches on the falling edge, and
// getting it wrong gives a picture that is present but shifted and washed out rather than absent.
constexpr int kPanelHRes = 800;
constexpr int kPanelVRes = 480;

// BSP src/bsp_display.c:79-118. num_fbs is the BSP's CONFIG_BSP_LCD_RGB_BUFFER_NUMS default of 1, and
// that is load-bearing here rather than incidental: the canvas IS the single frame buffer, so a second
// one would be memory nothing ever scans. bounce_buffer_size_px is left at 0 for the reason in the
// header -- the BSP reached the same value through its CONFIG_BSP_LCD_RGB_REFRESH_AUTO default.
void buildPanelConfig(esp_lcd_rgb_panel_config_t* cfg) {
    *cfg = {};

    cfg->clk_src = LCD_CLK_SRC_PLL160M;
    cfg->dma_burst_size = 64;
    cfg->data_width = 16;
    cfg->in_color_format = LCD_COLOR_FMT_RGB565;

    cfg->de_gpio_num = static_cast<gpio_num_t>(kRgbDe);
    cfg->pclk_gpio_num = static_cast<gpio_num_t>(kRgbPclk);
    cfg->vsync_gpio_num = static_cast<gpio_num_t>(kRgbVsync);
    cfg->hsync_gpio_num = static_cast<gpio_num_t>(kRgbHsync);
    cfg->disp_gpio_num = static_cast<gpio_num_t>(kRgbDisp);
    for (int i = 0; i < 16; i++) {
        cfg->data_gpio_nums[i] = static_cast<gpio_num_t>(kRgbData[i]);
    }

    cfg->timings.pclk_hz = 18 * 1000 * 1000;
    cfg->timings.h_res = kPanelHRes;
    cfg->timings.v_res = kPanelVRes;
    cfg->timings.hsync_pulse_width = 40;
    cfg->timings.hsync_back_porch = 40;
    cfg->timings.hsync_front_porch = 48;
    cfg->timings.vsync_pulse_width = 23;
    cfg->timings.vsync_back_porch = 32;
    cfg->timings.vsync_front_porch = 13;
    cfg->timings.flags.pclk_active_neg = true;

    cfg->flags.fb_in_psram = 1;
    cfg->num_fbs = 1;
    // cfg->bounce_buffer_size_px stays 0 -- see "WHY THE PANEL IS NOT IN BOUNCE BUFFER MODE".
}

} // namespace

// =================================================================================================
// Lifecycle
// =================================================================================================

KorvoRgbDisplay::KorvoRgbDisplay(int16_t width, int16_t height)
    : nativeW_(width), nativeH_(height), w_(width), h_(height) {
    // Tell the shim which display it stands for, and re-point the canvas. Both were missing, and
    // both matter.
    //
    // nativeTft() hands out &native_, and a sprite is built as `new TFT_eSprite(native)`, whose
    // constructor resolves its push target through native->display(). On a default-constructed shim
    // that is nullptr, KryonSprite::pushSprite() returns immediately on a null target, and so EVERY
    // pushSprite() became a silent no-op.
    //
    // That is what froze the Cube3d demo: the app draws entirely through a bound sprite, so it
    // rendered frame after frame into its buffer and nothing ever reached the panel -- the screen
    // kept showing the menu drawn directly at start. The serial log showed the 800x32 framebuffer
    // being created once per frame with no panic and no watchdog, which is exactly what a dropped
    // push looks like.
    //
    // canvas_ is constructed in the member-init list, i.e. BEFORE this body runs, so it captured the
    // same null target and is re-pointed here for the same reason. This also affects
    // NotificationManager's card and shadow sprites, not just JS apps.
    native_.setDisplay(this);
    canvas_.setTarget(this);
}

KorvoRgbDisplay::~KorvoRgbDisplay() {
    if (canvas_.created()) canvas_.deleteSprite();
    // Unlike the BSP-backed revision, this backend created the panel, so it deletes it. Nothing else
    // holds the handle.
    if (panel_) {
        esp_lcd_panel_del(panel_);
        panel_ = nullptr;
    }
}

bool KorvoRgbDisplay::ready() const { return panel_ != nullptr && canvasReady_; }

void KorvoRgbDisplay::init(uint8_t tc) {
    (void)tc;
    if (panel_) return; // idempotent: Display::begin() may run more than once

    Serial.printf("[Display:korvo] bringing up the %dx%d RGB panel directly through esp_lcd\n",
                  static_cast<int>(nativeW_), static_cast<int>(nativeH_));

    esp_lcd_rgb_panel_config_t panelCfg;
    buildPanelConfig(&panelCfg);

    esp_err_t err = esp_lcd_new_rgb_panel(&panelCfg, &panel_);
    if (err != ESP_OK || !panel_) {
        lastError_ = esp_err_to_name(err);
        Serial.printf("[Display:korvo] FAILED at panel bring-up: %s -- drawing disabled.\n",
                      lastError_);
        panel_ = nullptr;
        return;
    }

    // The BSP's init sequence, verbatim (src/bsp_display.c:120-125). invert_color(false) is not
    // cosmetic: the RGB565 path here is native end to end, and inverting would render every colour as
    // its complement.
    esp_lcd_panel_reset(panel_);
    esp_lcd_panel_init(panel_);
    esp_lcd_panel_invert_color(panel_, false);
    esp_lcd_panel_swap_xy(panel_, false);
    esp_lcd_panel_mirror(panel_, false, false);

    // The panel's own frame buffer, and the canvas IS that buffer -- see "DRAWING GOES STRAIGHT TO
    // THE PANEL" in the header. There is no second copy: num_fbs is 1 and this is the memory the
    // scanout reads.
    err = esp_lcd_rgb_panel_get_frame_buffer(panel_, 1, &fb_);
    if (err != ESP_OK || !fb_) {
        lastError_ = "the panel did not expose a frame buffer";
        Serial.printf("[Display:korvo] FAILED at frame buffer: %s -- drawing disabled.\n",
                      lastError_);
        esp_lcd_panel_del(panel_);
        panel_ = nullptr;
        return;
    }
    fbSize_ = static_cast<size_t>(nativeW_) * static_cast<size_t>(nativeH_) * 2;

    canvas_.setColorDepth(16);
    if (!canvas_.attachBuffer(static_cast<uint16_t*>(fb_), nativeW_, nativeH_)) {
        lastError_ = "the canvas could not be attached to the panel frame buffer";
        Serial.printf("[Display:korvo] FAILED at canvas: %s -- drawing disabled.\n", lastError_);
        esp_lcd_panel_del(panel_);
        panel_ = nullptr;
        return;
    }
    canvasReady_ = true;

    // Text scale. The canvas's own default is 1, which on this 800x480 panel renders the fonts at
    // their 240x320 size: 16px body text on a screen with three times the reference area, which is
    // both hard to read and too small to sit in the scaled-up rows and keys UiLayout now hands out.
    // setTextSize is TFT_eSPI's `textsize`, and KryonSprite feeds it into the draw path, the measure
    // path and the datum switch together -- so every screen that measures its text to lay out stays
    // correct, and no call site needs touching. js_setTextSize can still override it per app.
    const uint8_t textScale = static_cast<uint8_t>(uiScale(nativeW_, nativeH_));
    canvas_.setTextSize(textScale);

    // Black before the first real frame. The driver zeroed the frame buffer with calloc and flushed
    // it, so this is belt-and-braces rather than the only thing standing between the panel and
    // uninitialised PSRAM -- but it is also the first exercise of the flush in present(), so it is
    // worth doing here rather than assuming the path works later.
    canvas_.fillScreen(0x0000);
    flushToPanel();

    err = esp_lcd_panel_disp_on_off(panel_, true);
    if (err != ESP_OK) {
        Serial.printf("[Display:korvo] esp_lcd_panel_disp_on_off failed: %s\n", esp_err_to_name(err));
    }

    // The BSP's bsp_display_backlight_on() returned ESP_ERR_NOT_SUPPORTED on this board BY DESIGN:
    // BSP_LCD_BACKLIGHT is GPIO_NUM_NC, so there is no pin to drive and the backlight is hardwired on.
    // Vendoring the call would mean vendoring a guaranteed failure, so the fact is logged instead --
    // the bring-up sequence below still reports it in the same terms as every other backend.
    Serial.printf("[Display:korvo] ready: canvas is the panel frame buffer, %dx%d, UI scale %u, "
                  "backlight always-on (no control pin)\n",
                  static_cast<int>(w_), static_cast<int>(h_), static_cast<unsigned>(textScale));
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
    // all. Recording the request keeps the behaviour legible from a log instead of looking like a
    // silent failure.
    if (!backlightWarned_) {
        backlightWarned_ = true;
        Serial.printf("[Display:korvo] setBacklight(%u) is a no-op: this board's backlight is "
                      "hardwired on.\n",
                      static_cast<unsigned>(brightness));
    }
}

// =================================================================================================
// Frame presentation
// =================================================================================================

void KorvoRgbDisplay::present() { flushToPanel(); }

void KorvoRgbDisplay::flushToPanel() {
    // Write the CPU's cached drawing back to physical PSRAM, because the scanout does not go through
    // the CPU any more: the RGB panel's DMA reads the frame buffer straight out of PSRAM while every
    // draw KryonOS made is sitting in the write-back cache in front of it. Without this the DMA is
    // reading a stale picture. See "WHY present() IS NOT EMPTY" in the header.
    //
    // Unconditional, and cheap enough to be so. esp_cache_msync walks the range a cache line at a
    // time, but the write-back itself is driven by the dirty bits, so the ~6000 lines of a mostly
    // clean 800x480x2 buffer cost the walk and no bus traffic. Threading a dirty rectangle through
    // every primitive would save that walk and not the flush, which is not worth it.
    if (!fb_ || fbSize_ == 0) return;
    esp_cache_msync(fb_, fbSize_, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
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
    canvas_.fillScreen(static_cast<uint16_t>(color));}

void KorvoRgbDisplay::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!canvasReady_ || w <= 0 || h <= 0) return;
    canvas_.fillRect(x, y, w, h, static_cast<uint16_t>(color));}

void KorvoRgbDisplay::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!canvasReady_ || w <= 0 || h <= 0) return;
    canvas_.drawRect(x, y, w, h, static_cast<uint16_t>(color));}

void KorvoRgbDisplay::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                                    uint32_t color) {
    if (!canvasReady_ || w <= 0 || h <= 0) return;
    canvas_.drawRoundRect(x, y, w, h, radius, static_cast<uint16_t>(color));}

void KorvoRgbDisplay::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                                    uint32_t color) {
    if (!canvasReady_ || w <= 0 || h <= 0) return;
    canvas_.fillRoundRect(x, y, w, h, radius, static_cast<uint16_t>(color));}

void KorvoRgbDisplay::drawLine(int32_t xs, int32_t ys, int32_t xe, int32_t ye, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.drawLine(xs, ys, xe, ye, static_cast<uint16_t>(color));}

void KorvoRgbDisplay::drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t color) {
    if (!canvasReady_ || h <= 0) return;
    canvas_.drawFastVLine(x, y, h, static_cast<uint16_t>(color));}

void KorvoRgbDisplay::drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t color) {
    if (!canvasReady_ || w <= 0) return;
    canvas_.drawFastHLine(x, y, w, static_cast<uint16_t>(color));}

void KorvoRgbDisplay::drawPixel(int32_t x, int32_t y, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.drawPixel(x, y, static_cast<uint16_t>(color));}

void KorvoRgbDisplay::drawCircle(int32_t x, int32_t y, int32_t r, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.drawCircle(x, y, r, static_cast<uint16_t>(color));}

void KorvoRgbDisplay::fillCircle(int32_t x, int32_t y, int32_t r, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.fillCircle(x, y, r, static_cast<uint16_t>(color));}

void KorvoRgbDisplay::drawTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3,
                                   int32_t y3, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.drawTriangle(x1, y1, x2, y2, x3, y3, static_cast<uint16_t>(color));}

void KorvoRgbDisplay::fillTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3,
                                   int32_t y3, uint32_t color) {
    if (!canvasReady_) return;
    canvas_.fillTriangle(x1, y1, x2, y2, x3, y3, static_cast<uint16_t>(color));}

void KorvoRgbDisplay::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* data) {
    if (!canvasReady_ || !data || w <= 0 || h <= 0) return;
    // The canvas, the sprite buffers it is handed and the panel frame buffer all hold native
    // little-endian RGB565, so this is a straight copy with no byte swap anywhere in the pipeline.
    canvas_.pushImage(x, y, w, h, data);}

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
    canvas_.drawString(string, x, y);}

void KorvoRgbDisplay::drawString(const char* string, int32_t x, int32_t y, uint8_t font) {
    if (!canvasReady_ || !string) return;
    canvas_.drawString(string, x, y, font);}

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
