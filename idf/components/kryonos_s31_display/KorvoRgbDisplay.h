#ifndef KORVO_RGB_DISPLAY_H
#define KORVO_RGB_DISPLAY_H

/**
 * The KryonDisplay backend for the ESP32-S31-Korvo-1.
 *
 * WHY THIS IS SHORT
 *   Espressif's BSP owns this board's hardware. bsp_display_new_with_handles() configures the 16-bit
 *   RGB565 parallel bus, the timings, the pin map, the PSRAM frame buffers and the bounce buffer, and
 *   hands back an esp_lcd_panel_handle_t. Nothing about the panel is re-derived here, so -- unlike the
 *   Waveshare backend, which had to reverse-engineer an ST7701 init table and a GPIO expander -- this
 *   file has no register map, no init table, no expander and no pin list. That is the whole point of
 *   using the BSP.
 *
 * HOW IT DRAWS
 *   The BSP gives a panel handle and a blit, not a rasterizer: LVGL is not something you can call
 *   per-primitive. So drawing goes to a KryonSprite -- the component's TFT_eSprite replacement, which
 *   is a RAM framebuffer plus the ported TFT_eSPI text engine -- and present() copies the finished
 *   frame into the panel's own frame buffer and hands that buffer back to the driver.
 *
 *   The canvas is exactly the panel: 800x480, and panelToCanvas() is identity. The Waveshare's round
 *   aperture and 6/5 upscale have no counterpart here because this panel is a plain rectangle.
 *
 * NO DOUBLE-BUFFER HANDSHAKE
 *   The Waveshare backend needed an ISR callback, a swap-pending flag and a stall watchdog, because
 *   that driver anchors the scanout to framebuffer 0 on every VSYNC (an ESP32-S3-only restart-link
 *   workaround). The S31's RGB driver does not: it takes the frame buffer you hand it, sets
 *   cur_fb_index, and -- in bounce-buffer mode -- lets bb_fb_index re-latch at the next frame wrap.
 *   So present() alternates the two buffers and the flip is a frame boundary late, tear-free, with no
 *   callback and no watchdog to get wrong.
 *
 * WHAT IT DOES NOT DO
 *   setRotation() other than 0. The RGB scanout's orientation is fixed by the panel's wiring; a
 *   quarter-turn request would relabel the axes and then clip an 800x480 canvas into a 480x800 hole.
 *   It warns once and stays at 0 rather than drawing a broken layout quietly.
 *
 *   Backlight brightness. BSP_LCD_BACKLIGHT is GPIO_NUM_NC -- this board's backlight is hardwired on,
 *   so the BSP's own on/off/brightness calls return ESP_ERR_NOT_SUPPORTED by design. setBacklight()
 *   records the value and says so once; it is not a fault and not something to work around.
 */

#include <TFT_eSPI.h> // the component's shim, not the real library -- see that header

#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

#include "Hal/Display/KryonDisplay.h"

class KorvoRgbDisplay : public KryonDisplay {
public:
    // The panel's own size. Passed in rather than compiled in so the backend can be constructed for a
    // different panel without editing it -- the board file is where the numbers belong.
    KorvoRgbDisplay(int16_t width, int16_t height);
    ~KorvoRgbDisplay() override;

    // --- Lifecycle ---
    void init(uint8_t tc = 0) override;
    void setRotation(uint8_t rotation) override;
    uint8_t getRotation() override;
    int16_t width() override;
    int16_t height() override;

    // --- Filled and outlined shapes ---
    void fillScreen(uint32_t color) override;
    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) override;
    void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) override;
    void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                       uint32_t color) override;
    void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                       uint32_t color) override;
    void drawLine(int32_t xs, int32_t ys, int32_t xe, int32_t ye, uint32_t color) override;
    void drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t color) override;
    void drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t color) override;
    void drawPixel(int32_t x, int32_t y, uint32_t color) override;
    void drawCircle(int32_t x, int32_t y, int32_t r, uint32_t color) override;
    void fillCircle(int32_t x, int32_t y, int32_t r, uint32_t color) override;
    void drawTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                      uint32_t color) override;
    void fillTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                      uint32_t color) override;
    void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* data) override;

    // --- Text ---
    void setTextColor(uint16_t color) override;
    void setTextColor(uint16_t fgcolor, uint16_t bgcolor, bool bgfill = false) override;
    void setTextDatum(uint8_t datum) override;
    void setTextSize(uint8_t size) override;
    void drawString(const char* string, int32_t x, int32_t y) override;
    void drawString(const char* string, int32_t x, int32_t y, uint8_t font) override;
    void drawString(const String& string, int32_t x, int32_t y) override;
    void drawString(const String& string, int32_t x, int32_t y, uint8_t font) override;
    int16_t textWidth(const char* string) override;
    int16_t textWidth(const char* string, uint8_t font) override;
    int16_t textWidth(const String& string) override;
    int16_t textWidth(const String& string, uint8_t font) override;

    // --- Colour and bus helpers ---
    uint16_t color565(uint8_t red, uint8_t green, uint8_t blue) override;
    void setSwapBytes(bool swap) override;
    void startWrite() override;
    void endWrite() override;

    // --- Backend introspection ---
    const char* backendName() const override { return "korvo_rgb"; }

    /**
     * The shim instance, so the sprite consumers (NotificationManager, the JS createSprite and 3D
     * bindings) can allocate a TFT_eSprite. Non-null here, unlike the Waveshare backend, because the
     * sprite it returns is backed by KryonSprite rather than by an unconfigured bus.
     */
    TFT_eSPI* nativeTft() override { return &native_; }

    // --- Frame presentation ---
    /** Copies the canvas into the frame buffer the scanout is not reading, then flips to it. */
    void present() override;

    /** True once the panel exists and the canvas allocation succeeded. */
    bool ready() const;

    /** Recorded and logged once; this board's backlight has no brightness control and is always on. */
    void setBacklight(uint8_t brightness);

    /** One-line reason the last failed bring-up step gave up, or "" when nothing failed. */
    const char* lastError() const { return lastError_; }

private:
    // The shim, declared first so it is constructed before the sprite that points at it.
    TFT_eSPI native_;
    TFT_eSprite canvas_{&native_};

    esp_lcd_panel_handle_t panel_ = nullptr;

    // The panel's two frame buffers, fetched once at bring-up. present() alternates between them; the
    // driver recognises a pointer that is one of its own and adopts it without copying.
    void* fb_[2] = {nullptr, nullptr};

    int16_t nativeW_ = 0;
    int16_t nativeH_ = 0;
    int16_t w_ = 0;
    int16_t h_ = 0;
    uint8_t rotation_ = 0;

    bool canvasReady_ = false;
    bool dirty_ = false;
    bool rotationWarned_ = false;
    bool backlightWarned_ = false;
    // Index of the frame buffer present() writes into next. Starts at 1 because the driver brings the
    // scanout up on framebuffer 0, so the first present has to use the other one.
    uint8_t nextFb_ = 1;
    const char* lastError_ = "";
};

#endif // KORVO_RGB_DISPLAY_H
