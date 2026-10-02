#ifndef ESP_LCD_RGB_DISPLAY_H
#define ESP_LCD_RGB_DISPLAY_H

/**
 * A KryonDisplay backend for an RGB565 PARALLEL panel driven by ESP-IDF's esp_lcd_panel_rgb.
 *
 * WHY THIS EXISTS
 *   TFT_eSPI drives SPI and 8-bit parallel panels only. A board whose panel is on a 16-bit RGB
 *   parallel bus (16 data lines plus DE/PCLK/VSYNC/HSYNC, with a 3-wire bus used for init commands
 *   alone) cannot be driven by TFT_eSPI at all, so this backend talks to esp_lcd instead.
 *
 * WHY IT STILL COMPILES TFT_eSPI
 *   The entire UI draws text through TFT_eSPI's fonts and lays out with TFT_eSPI's textWidth(). A
 *   hand-written rasterizer (see RamFramebufferDisplay) has no font, so it would silently drop every
 *   label. This backend therefore keeps TFT_eSPI as a pure SOFTWARE RASTERIZER: drawings land in a
 *   full-screen 16bpp TFT_eSprite in PSRAM, and present() blits that sprite to the panel. The glyphs,
 *   the metrics and the anti-aliasing are then pixel-identical to the SPI boards, so UiLayout's
 *   geometry stays correct here for free.
 *
 *   The TFT_eSPI instance this sprite is built against is a "phantom": it exists only to satisfy the
 *   sprite's constructor, is never init()ed, and owns no pins. The environment that selects this
 *   backend must therefore define no TFT_* / TOUCH_* pin macros -- that is also what keeps the I2C
 *   bus (used by the panel's IO expander and, later, its touch controller) free of a pin collision,
 *   because I2CEngine refuses to open a bus on a pin any of those macros claims.
 *
 * BYTE ORDER
 *   TFT_eSprite stores every 16bpp pixel byte-swapped (Sprite.cpp: drawPixel/fillRect/
 *   drawFastHLine/drawFastVLine/pushColor all write `(color >> 8) | (color << 8)`), because it was
 *   designed to be pushed to an SPI panel. The RGB panel path is the other way round: ESP-IDF's RGB
 *   driver exposes no byte-order knob for the framebuffer, and every RGB panel configuration in the
 *   wild hands it native little-endian RGB565 (the vendor code for this very board sets
 *   `swap_bytes = 0` for its RGB path and `1` for its SPI path). present() therefore swaps the
 *   sprite's words into a staging buffer on the way out. Flip KRYONOS_RGB_BLIT_SWAP to 0 if a future
 *   IDF revision starts swapping for you; the boot log prints which mode is active.
 *
 * WHAT IT DOES NOT DO
 *   Sprites for JS / notifications. nativeTft() stays nullptr, so NotificationManager and the JS
 *   createSprite binding degrade exactly as documented for a non-TFT_eSPI backend. That needs a
 *   backend-neutral KryonSprite, which is not part of this pass.
 *
 *   This backend has never run on hardware. Every bring-up failure path logs a distinct one-line
 *   reason and leaves the backlight off rather than hanging, so a dark panel can be localised from
 *   serial alone.
 */

// ONLY the chips whose ESP-IDF ships the LCD_CAM RGB peripheral driver can compile this backend --
// ESP32-S3 (and P4) have esp_lcd_panel_rgb.h; the classic ESP32 does not. On every other target this
// header expands to nothing, which is exactly what a board file guarded by its TARGET_* macro does.
// That guard is load-bearing, not tidiness: PlatformIO compiles every src/*.cpp for EVERY
// environment, so an unguarded include here breaks `pio run -e esp32-default` outright.
//
// The detection matches src/Hal/I2C/I2CEngine.cpp: the sdkconfig macro where sdkconfig.h is already
// in scope, and the board-manifest macro otherwise (it arrives as a -D, so it does not depend on
// include order).
#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)

#include <TFT_eSPI.h> // TFT_eSprite — used here purely as a rasterizer, see below

#include <esp_lcd_panel_ops.h>
#include <esp_lcd_panel_rgb.h>

#include "Hal/Display/KryonDisplay.h"

class EspLcdRgbDisplay : public KryonDisplay {
public:
    EspLcdRgbDisplay(int16_t nativeWidth, int16_t nativeHeight, int backlightPin);
    ~EspLcdRgbDisplay() override;

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
    const char* backendName() const override { return "esp_lcd_rgb"; }
    // nativeTft() is deliberately left as the nullptr default: the phantom TFT_eSPI owns no pins and
    // was never init()ed, so handing it to sprite consumers would push pixels at an unconfigured bus.

    // --- Frame presentation ---
    void present() override;

    /** True once the panel exists and the sprite allocation succeeded. */
    bool ready() const;

    /** 0 = off, 255 = full. Drives the panel's backlight GPIO over LEDC. */
    void setBacklight(uint8_t brightness);

    /** One-line reason the last failed bring-up step gave up, or "" when nothing failed. */
    const char* lastError() const { return lastError_; }

private:
    // Bring-up stages. Each returns false and sets lastError_ on failure.
    bool setUpCanvas();
    bool setUpExpander();
    bool sendInitTable();
    bool setUpPanel();

    // One full-panel frame: optional byte swap into staging_, then esp_lcd_panel_draw_bitmap.
    //
    // This is deliberately the same route this board's shipped firmware takes: its LVGL port calls
    // draw_bitmap(panel, 0, 0, 480, 480, buffer) with two framebuffers and a bounce buffer, and the
    // driver handles the copy and the PSRAM cache maintenance. Writing into the framebuffer directly
    // instead would mean owning esp_cache_msync here, which is only necessary for code that fills
    // the framebuffer itself.
    void blit();

    // Sprite-native rasterizer helpers, mirroring RamFramebufferDisplay's algorithms.
    void hLine(int32_t x, int32_t y, int32_t w, uint16_t color);
    void vLine(int32_t x, int32_t y, int32_t h, uint16_t color);
    void putPixel(int32_t x, int32_t y, uint16_t color);
    void circleHelper(int32_t x0, int32_t y0, int32_t r, uint8_t corners, int32_t delta,
                      uint16_t color, bool filled);

    // The phantom. Declared first so it is constructed before the sprite that points at it.
    TFT_eSPI phantom_;
    TFT_eSprite canvas_{&phantom_};

    esp_lcd_panel_handle_t panel_ = nullptr;
    uint16_t* staging_ = nullptr; // byte-swap destination for present()

    int backlightPin_ = -1;
    int16_t nativeW_ = 0;
    int16_t nativeH_ = 0;
    int16_t w_ = 0;
    int16_t h_ = 0;
    uint8_t rotation_ = 0;
    uint8_t brightness_ = 0;

    bool dirty_ = false;
    bool failed_ = false;
    bool canvasReady_ = false;
    bool backlightAttached_ = false;
    const char* lastError_ = "";
    uint32_t lastPresentMs_ = 0;
};

#endif // RGB-capable targets only (see the note above the includes)

#endif // ESP_LCD_RGB_DISPLAY_H
