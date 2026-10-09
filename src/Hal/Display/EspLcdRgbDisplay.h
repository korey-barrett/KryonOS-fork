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
 *   16bpp TFT_eSprite in PSRAM -- the logical canvas, which is smaller than the panel because the
 *   panel is round (see THE ROUND APERTURE below) -- and present() blits that sprite to the panel.
 *   The glyphs, the metrics and the anti-aliasing are then pixel-identical to the SPI boards, so
 *   UiLayout's geometry stays correct here for free.
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
 *   `swap_bytes = 0` for its RGB path and `1` for its SPI path). The blit therefore swaps the
 *   sprite's words on the way into the framebuffer. Flip KRYONOS_RGB_BLIT_SWAP to 0 if a future IDF
 *   revision starts swapping for you; the boot log prints which mode is active.
 *
 * THE ROUND APERTURE
 *   This board's panel is a 480x480 circle, not a square. A 480x480 canvas is therefore cut by the
 *   bezel: the footer bar's UP and DN thirds, and both header corners, sit outside the glass and
 *   cannot be touched at all. So the logical canvas is smaller than the panel and is scaled into the
 *   largest rect that fits the circle -- see the aperture block on the class below.
 *
 * WHAT IT DOES NOT DO
 *   Sprites for JS / notifications. nativeTft() stays nullptr, so NotificationManager and the JS
 *   createSprite binding degrade exactly as documented for a non-TFT_eSPI backend. That needs a
 *   backend-neutral KryonSprite, which is not part of this pass.
 *
 *   Every bring-up failure path logs a distinct one-line reason and leaves the backlight off rather
 *   than hanging, so a dark panel can be localised from serial alone.
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
    // nativeWidth/nativeHeight are the PANEL's scan size and drive the RGB timings, the framebuffer
    // and the aperture rect's centring. logicalWidth/logicalHeight are the canvas the UI draws to --
    // the size of the sprite, and what width()/height() report. On this board the two differ because
    // the panel is round and the canvas is scaled up into it (see the aperture block below).
    //
    // They are deliberately separate arguments: passing the logical size as the native size (as this
    // backend used to, when the two were the same value) reconfigures the panel timings and shrinks
    // the sprite instead of upscaling, which is a broken panel rather than a smaller canvas.
    EspLcdRgbDisplay(int16_t nativeWidth, int16_t nativeHeight, int16_t logicalWidth,
                     int16_t logicalHeight, int backlightPin);
    ~EspLcdRgbDisplay() override;

    // --- The round aperture -------------------------------------------------------------------
    //
    // This panel is a 480 px circle, so the logical canvas has to be a rectangle that fits inside
    // it. The blit upscales the canvas by SCALE_NUM/SCALE_DEN into a rect centred on the panel, and
    // the touch path inverts exactly that transform to get canvas pixels back.
    //
    // The ratio is set by the canvas so that the inscribed rect always lands its four corners
    // EXACTLY on the bezel -- any scale smaller leaves a black ring, any larger is cut by the round
    // edge, and the cut goes fast: push past it and the corners leave the glass first, which is what
    // takes the footer's UP and DN thirds with them.
    //
    // The canvas is 3:4, so it is parametrised as W x H = 3m x 4m and its half-diagonal is 2.5m. The
    // corners land on the circle when 2.5m * NUM/DEN = 240, which for NUM/DEN = 96/67 gives m = 67:
    // a 201x268 canvas with a half-diagonal of sqrt(100.5^2 + 134^2) = 167.5 px, growing to
    // 167.5 * 96/67 = 240 px -- the bezel radius. That rect is 201 * 96/67 = 288 by 268 * 96/67 =
    // 384, i.e. the same 288x384 as before.
    //
    // THAT is the invariant worth keeping in mind when retuning: for ANY 3:4 canvas the blit is
    // exactly 288x384 and only the magnification k = 96/m changes. A smaller canvas therefore does
    // not shrink the picture, it enlarges it -- 240x320 at 6/5 and 201x268 at 96/67 put the same
    // 288x384 rect on the glass at 1.2x and 1.433x. What does NOT move with k is anything sized as a
    // fraction of the rect: keyboard key width stays blitW/kbCols, so at the board's 6 columns it is
    // 48 px on the glass at every canvas size. See the canvas note in platformio.ini.
    static constexpr int16_t SCALE_NUM = 96;
    static constexpr int16_t SCALE_DEN = 67;

    // Forward edge: the first PANEL offset that displays canvas pixel c. This is the CEILING of
    // c*SCALE_NUM/SCALE_DEN, not the floor -- the floor is the natural thing to write and it is wrong
    // for most c, naming a panel pixel that displays c-1.
    static int32_t canvasToPanelEdge(int32_t c) {
        return (c * SCALE_NUM + SCALE_DEN - 1) / SCALE_DEN;
    }

    // Inverse: the canvas pixel a panel offset inside the rect displays. The blit drives itself from
    // this function, so forward and inverse are one expression and cannot drift apart.
    static int32_t apertureOffsetToCanvas(int32_t p) { return p * SCALE_DEN / SCALE_NUM; }

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

    /**
     * Invert the blit's scale and centring, so a touch on the panel comes back in canvas pixels.
     *
     * Bounds are checked in rect space, BEFORE the divide. Integer division truncates toward zero,
     * so a negative offset -- a touch left of or above the rect -- would divide to 0 and be read as a
     * hit on the canvas edge. Taps outside the rect return false and are logged once, because a
     * controller reporting something other than panel pixels should say so on the first flash rather
     * than present as touch that is simply dead.
     */
    bool panelToCanvas(int32_t px, int32_t py, int32_t* cx, int32_t* cy) const override;

    /** The blitted rect, in panel pixels. The boot log prints these; tests can assert them. */
    int16_t apertureWidth() const { return blitW_; }
    int16_t apertureHeight() const { return blitH_; }
    int16_t apertureOffsetX() const { return offsetX_; }
    int16_t apertureOffsetY() const { return offsetY_; }

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

    // One frame: upscale the canvas into the given panel framebuffer through the aperture rect.
    //
    // The caller picks the buffer, and that choice is the whole anti-tearing story: with two
    // framebuffers (see setUpPanel) present() always draws into the one the scanout is NOT reading,
    // then asks the driver to adopt it at the next frame boundary. Drawing into the live buffer is
    // what produced the thin line sweeping the screen, and with a single framebuffer there is no way
    // to avoid it -- the write and the scan race on the same memory.
    //
    // Driving the sprite's pixels in directly, rather than handing draw_bitmap a staging buffer to
    // copy, also halves the per-frame work: the aperture rect is 288x384 = 110,592 pixels, against
    // the 230,400 a full-panel copy would touch. The cost of owning the framebuffer is owning its
    // cache maintenance, which is what the esp_cache_msync at the end of the blit is for.
    void blitInto(uint16_t* dst);

    // Blacken BOTH framebuffers, once, at bring-up. Everything outside the aperture rect is then
    // never written again, so a seam landing there has identical pixels on both sides and cannot
    // show -- in either buffer the scanout can reach.
    void clearFrameBuffer();

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

    // The panel's two framebuffers, taken once at bring-up and never re-fetched. Presenting is
    // picking one of these pointers, not copying pixels into the driver -- see blitInto.
    void* fb_[2] = {nullptr, nullptr};

    int backlightPin_ = -1;
    int16_t nativeW_ = 0; // the panel: sprite, RGB timings and framebuffer
    int16_t nativeH_ = 0;
    int16_t logicalW_ = 0; // the canvas the UI draws to, before rotation
    int16_t logicalH_ = 0;
    int16_t w_ = 0; // the canvas after rotation -- what width()/height() report
    int16_t h_ = 0;
    int16_t blitW_ = 0; // the aperture rect inside the panel
    int16_t blitH_ = 0;
    int16_t offsetX_ = 0;
    int16_t offsetY_ = 0;
    uint8_t rotation_ = 0;
    uint8_t brightness_ = 0;

    bool dirty_ = false;
    bool failed_ = false;
    bool canvasReady_ = false;
    bool backlightAttached_ = false;
    // True while present() flips between fb_[0] and fb_[1]. Cleared only by the stall watchdog in
    // present(), which drops back to the single-buffer write this backend used before -- slower to
    // look at, but never a frozen screen.
    bool doubleBuffered_ = false;
    // Set once the first frame-boundary flip has been observed and logged -- see present().
    bool flipLogged_ = false;
    // When the present in flight asked for its flip. Only meaningful while the ISR-side
    // s_swapPending is set, which is what makes it safe to compare without initialisation games.
    uint32_t swapAskedMs_ = 0;
    // panelToCanvas rejects a tap outside the rect; the first rejection is logged with its raw
    // coordinates and the rest are silent, since this runs per touch and would otherwise flood.
    // Mutable because panelToCanvas is const -- the flag is bookkeeping about logging, not state.
    mutable bool loggedReject_ = false;
    const char* lastError_ = "";
    uint32_t lastPresentMs_ = 0;
};

#endif // RGB-capable targets only (see the note above the includes)

#endif // ESP_LCD_RGB_DISPLAY_H
