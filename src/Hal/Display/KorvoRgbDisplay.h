#ifndef KORVO_RGB_DISPLAY_H
#define KORVO_RGB_DISPLAY_H

/**
 * The KryonDisplay backend for the ESP32-S31-Korvo-1.
 *
 * THE PANEL IS CONFIGURED HERE, NOT BY A BSP
 *   An earlier revision let Espressif's BSP own this board -- bsp_display_new_with_handles() supplied
 *   the bus, the timings, the pin map and the frame buffers. That is not available on the path this
 *   fork ships: the BSP is an ESP-IDF managed component, and this board has to build under the
 *   upstream PlatformIO / arduino-esp32 stack, where no such component exists. So the values the BSP
 *   used are vendored into the .cpp (copied from espressif__esp32_s31_korvo_1 -- bsp_display.c and
 *   esp32_s31_korvo_1.h, each marked at its definition) and the panel is created directly with
 *   esp_lcd_new_rgb_panel().
 *
 *   The consequence worth remembering: those numbers are now ours to keep. If the panel misbehaves,
 *   the timings and pin map in the .cpp are what to check against the BSP or the schematic, because
 *   nothing else supplies them any more.
 *
 * DRAWING GOES STRAIGHT TO THE PANEL
 *   esp_lcd gives a panel and a blit, not a rasterizer. So drawing goes to a KryonSprite -- KryonOS's
 *   TFT_eSprite replacement, a RAM framebuffer plus the ported TFT_eSPI text engine -- and that
 *   sprite is attached to the panel's OWN frame buffer. Every primitive therefore lands in the memory
 *   the scanout reads, with no second copy. This is the same shape as TftEspiDisplay, whose present()
 *   is likewise thin because TFT_eSPI writes through to the glass.
 *
 * WHY THE PANEL IS NOT IN BOUNCE BUFFER MODE
 *   The displaced picture this board used to show -- the top of the page reappearing at the bottom, by
 *   an amount that changed between boots -- was the RGB driver's bounce buffer losing its place.
 *
 *   In bounce mode the driver streams the frame buffer to the panel through two small internal
 *   buffers, and tracks how far through the frame it has got in a SOFTWARE counter (bounce_pos_px,
 *   esp_lcd_panel_rgb.c) that only advances when its refill interrupt manages to copy the next slice
 *   out of PSRAM before the DMA has finished sending the previous one. Miss that deadline once -- and
 *   a flash write, a WiFi burst or any cache-off window is enough -- and every later slice is offset
 *   by a whole bounce buffer. PERMANENTLY, because nothing on this chip corrects it: the desync
 *   recovery path (lcd_rgb_panel_try_restart_transmission, and the public esp_lcd_rgb_panel_restart)
 *   is guarded by RGB_LCD_NEEDS_SEPARATE_RESTART_LINK, which that file defines ONLY under
 *   CONFIG_IDF_TARGET_ESP32S3, so on the S31 it is not compiled and the public call returns
 *   ESP_ERR_NOT_SUPPORTED. The "LCD underrun" it would have logged on a missed refill is missing too,
 *   because LCD_LL_EVENT_UNDERRUN is not defined for this SoC and the check compiles out. Silent and
 *   permanent is exactly what was observed, including the amount varying from boot to boot.
 *
 *   So the counter is gone: with bounce_buffer_size_px left at 0 the DMA link is built with
 *   mark_final = GDMA_FINAL_LINK_TO_DEFAULT (esp_lcd_panel_rgb.c:1225), which restarts the transfer
 *   at row 0 of the frame buffer at the end of EVERY frame. The scanout position now lives in the DMA
 *   hardware and is re-established 35 times a second, so a stall can corrupt a frame but can never
 *   accumulate into a displacement. The BSP reached the same place through its
 *   CONFIG_BSP_LCD_RGB_REFRESH_AUTO default, which is why its bounce-buffer branch is not vendored.
 *
 *   The cost is that a redraw is visible as it happens, because the panel is reading the same memory
 *   being written. That is the trade the whole design makes: the other KryonOS boards hide it behind
 *   the panel's own GRAM, and this panel has none.
 *
 * WHY present() IS NOT EMPTY
 *   Because the scanout no longer passes through the CPU. The panel's DMA reads the frame buffer out
 *   of PHYSICAL PSRAM, while KryonOS's draws land in the write-back cache in front of it. Until those
 *   dirty lines are written back, the DMA is reading a stale picture -- so present() flushes them.
 *
 *   This is the same esp_cache_msync the RGB driver makes for itself when a draw buffer is found to
 *   belong to the frame buffer (esp_lcd_panel_rgb.c:767). KryonOS never calls
 *   esp_lcd_panel_draw_bitmap, because the canvas IS the frame buffer, so it makes the call itself.
 *   Bounce mode hid this by accident: its refill interrupt read the frame buffer through the CPU, so
 *   it saw cached writes for free.
 *
 *   The canvas is exactly the panel: 800x480, and panelToCanvas() is identity. The Waveshare's round
 *   aperture and 6/5 upscale have no counterpart here because this panel is a plain rectangle.
 *
 * WHAT IT DOES NOT DO
 *   setRotation() other than 0. The RGB scanout's orientation is fixed by the panel's wiring; a
 *   quarter-turn request would relabel the axes and then clip an 800x480 canvas into a 480x800 hole.
 *   It warns once and stays at 0 rather than drawing a broken layout quietly.
 *
 *   Backlight brightness. This board's backlight is hardwired on and there is no pin to drive, so
 *   setBacklight() records the value and says so once. It is not a fault and not something to work
 *   around.
 */

// The whole unit is S31-only, and src/ is compiled for every environment -- so the guard comes BEFORE
// the includes, not after them. esp_lcd_panel_rgb.h does not exist for the classic ESP32 at all, and
// without this ordering the six other environments would fail on a header for a peripheral their
// silicon does not have -- and would resolve <TFT_eSPI.h> against the real library rather than the
// shim, because that is what is on their include path.
#if defined(KRYONOS_KRYON_SPRITE)

#include <TFT_eSPI.h> // KryonOS's shim, not the real library -- see that header

#include <stddef.h>

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
    /** Writes the CPU's cached drawing out to the frame buffer the DMA scans. See "WHY present() IS
     *  NOT EMPTY" above. */
    void present() override;

    /** True once the panel exists and the canvas allocation succeeded. */
    bool ready() const;

    /** Recorded and logged once; this board's backlight has no brightness control and is always on. */
    void setBacklight(uint8_t brightness);

    /** One-line reason the last failed bring-up step gave up, or "" when nothing failed. */
    const char* lastError() const { return lastError_; }

private:
    /** The cache flush present() performs, also used once during bring-up. See its definition. */
    void flushToPanel();

    // The shim, declared first so it is constructed before the sprite that points at it.
    TFT_eSPI native_;
    TFT_eSprite canvas_{&native_};

    esp_lcd_panel_handle_t panel_ = nullptr;
    // The panel's frame buffer, which the canvas is attached to, and its size in bytes for the flush
    // in present(). Held separately from the canvas so present() does not have to go through the
    // sprite to find out what to write back.
    void* fb_ = nullptr;
    size_t fbSize_ = 0;

    int16_t nativeW_ = 0;
    int16_t nativeH_ = 0;
    int16_t w_ = 0;
    int16_t h_ = 0;
    uint8_t rotation_ = 0;

    bool canvasReady_ = false;
    bool rotationWarned_ = false;
    bool backlightWarned_ = false;
    const char* lastError_ = "";
};

#endif // KRYONOS_KRYON_SPRITE

#endif // KORVO_RGB_DISPLAY_H
