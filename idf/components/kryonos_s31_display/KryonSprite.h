#ifndef KRYON_SPRITE_H
#define KRYON_SPRITE_H

#include <stdint.h>

#include "KryonText.h"
#include "Hal/Display/KryonDisplay.h"

class RamFramebufferDisplay;

/**
 * A drawing surface in RAM, with the same call surface as the panel, that can be pushed to it.
 *
 * WHAT THIS IS FOR
 *   Two parts of KryonOS draw into an off-screen buffer rather than straight at the panel: the JS
 *   `createSprite` binding, and NotificationManager's card and shadow sprites. Both were written
 *   against TFT_eSprite, which cannot exist on the ESP32-S31 because TFT_eSPI cannot compile for the
 *   chip at all. This is the replacement.
 *
 * WHY IT DOES NOT REIMPLEMENT THE RASTERIZER
 *   Every geometric primitive here already exists, tested, in RamFramebufferDisplay -- and that class
 *   is itself the TFT_eSPI-compatible set (the circle helper, the corner bit order, the triangle
 *   scanlines). So this class owns a RamFramebufferDisplay and forwards geometry to it, and adds only
 *   the two things that class lacks: a real font, and lifecycle.
 *
 *   The consequence worth knowing: a sprite and a RamFramebufferDisplay backend draw identically,
 *   because they are the same code.
 *
 * COLOUR DEPTH
 *   setColorDepth() is accepted and remembered, but the buffer is always 16bpp. The 8bpp path exists
 *   in the JS binding only as a fallback for when a 16bpp allocation fails, and with 16 MB of PSRAM
 *   the first attempt does not fail; an 8bpp palette path would be dead code that still had to be
 *   kept correct. Recorded rather than silently ignored, so a future low-memory board can see the
 *   request was made.
 */
class KryonSprite : public kryon_text::Surface {
public:
    KryonSprite() = default;
    ~KryonSprite() override;

    KryonSprite(const KryonSprite&) = delete;
    KryonSprite& operator=(const KryonSprite&) = delete;

    // --- Lifecycle (TFT_eSprite's names) ---------------------------------------------------------
    /** Allocates the buffer. Returns it, or nullptr if the allocation failed. */
    void* createSprite(int16_t w, int16_t h);
    void deleteSprite();
    /** Accepted and recorded; the buffer stays 16bpp -- see COLOUR DEPTH above. */
    void setColorDepth(int8_t bits);
    int8_t getColorDepth() const { return depth_; }
    bool created() const { return canvas_ != nullptr; }

    int16_t width() const { return w_; }
    int16_t height() const { return h_; }

    /**
     * Rotates the canvas under TFT_eSprite's name.
     *
     * The buffer is w*h either way, so -- as in RamFramebufferDisplay -- a quarter turn only changes
     * which way the sprite is measured. It does not move pixels, and a sprite created for one
     * orientation should not be rotated after drawing into it.
     */
    void setRotation(uint8_t rotation);

    /** The target pushSprite() blits onto. Set by the backend that owns the sprite. */
    void setTarget(KryonDisplay* target) { target_ = target; }

    /** Blit this sprite onto its target with its top-left at (x, y). */
    void pushSprite(int32_t x, int32_t y);
    /** Blit onto an explicit display, ignoring the target. */
    void pushSprite(KryonDisplay* display, int32_t x, int32_t y);

    // --- kryon_text::Surface ---------------------------------------------------------------------
    void drawPixel(int32_t x, int32_t y, uint16_t color) override;
    void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) override;

    // --- The rest of the panel's call surface, forwarded -----------------------------------------
    void fillScreen(uint16_t color);
    void fillSprite(uint16_t color);
    void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color);
    void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint16_t color);
    void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius, uint16_t color);
    void drawLine(int32_t xs, int32_t ys, int32_t xe, int32_t ye, uint16_t color);
    void drawFastVLine(int32_t x, int32_t y, int32_t h, uint16_t color);
    void drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color);
    void drawCircle(int32_t x, int32_t y, int32_t r, uint16_t color);
    void fillCircle(int32_t x, int32_t y, int32_t r, uint16_t color);
    void drawTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                      uint16_t color);
    void fillTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                      uint16_t color);
    void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* data);

    // --- Text ------------------------------------------------------------------------------------
    void setTextColor(uint16_t color);
    void setTextColor(uint16_t fgcolor, uint16_t bgcolor, bool bgfill = false);
    void setTextDatum(uint8_t datum) { datum_ = datum; }
    void setTextSize(uint8_t size) { size_ = size ? size : 1; }
    void drawString(const char* string, int32_t x, int32_t y);
    void drawString(const char* string, int32_t x, int32_t y, uint8_t font);
    int16_t textWidth(const char* string);
    int16_t textWidth(const char* string, uint8_t font);
    /** The font drawString() uses when none is passed -- TFT_eSPI's `textfont`, default 1. */
    void setTextFont(uint8_t font) { lastFont_ = font; }
    uint8_t textFont() const { return lastFont_; }

    // --- Misc, for call-site compatibility -------------------------------------------------------
    static uint16_t color565(uint8_t red, uint8_t green, uint8_t blue) {
        return (uint16_t)(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3));
    }
    void setSwapBytes(bool) {}
    void startWrite() {}
    void endWrite() {}

    /** Row-major RGB565 frame, or nullptr when not created. */
    uint16_t* pixels();

    /**
     * The same buffer under TFT_eSprite's name. NotificationManager and the JS 3D-sprite binding both
     * read a sprite's pixels back through getPointer(), so the name has to exist even though the
     * buffer here is native-endian rather than the byte-swapped one a real TFT_eSprite would hand out.
     */
    void* getPointer() { return pixels(); }

private:
    RamFramebufferDisplay* canvas_ = nullptr;  // owned
    KryonDisplay* target_ = nullptr;           // not owned
    int16_t w_ = 0;
    int16_t h_ = 0;
    int8_t depth_ = 16;
    uint16_t textFg_ = 0xFFFF;
    uint16_t textBg_ = 0x0000;
    bool textBgFill_ = false;
    uint8_t datum_ = 0;  // TL_DATUM
    uint8_t size_ = 1;
    uint8_t lastFont_ = 1;
};

#endif // KRYON_SPRITE_H
