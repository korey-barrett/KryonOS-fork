#ifndef RAM_FRAMEBUFFER_DISPLAY_H
#define RAM_FRAMEBUFFER_DISPLAY_H

#include "Hal/Display/KryonDisplay.h"

/**
 * A reference KryonDisplay backend that owns a plain RGB565 framebuffer in RAM and draws into it
 * with no display controller attached.
 *
 * It exists to prove the seam: it is not a TFT_eSPI, it shares no code with TftEspiDisplay, and the
 * UI cannot tell the difference. Select it with -D KRYONOS_DISPLAY_BACKEND=KRYONOS_BACKEND_RAM.
 *
 * WHAT IT DOES NOT DO
 *   Text. There is no font in this backend, so drawString() draws nothing and textWidth() returns
 *   an estimate (6/8/13 px per glyph for font 1/2/4, scaled by setTextSize). Everything geometric
 *   -- rectangles, round rectangles, lines, circles, triangles, pushImage -- is implemented.
 *
 * The rasterizer below is compile-tested on every build but has never been run on hardware or
 * diffed against a TFT_eSPI frame, so treat its output as unverified until you compare it.
 *
 * MEMORY
 *   The buffer is width * height * 2 bytes: 150 KB at 240x320, 300 KB at 320x480. init() prefers
 *   PSRAM and falls back to the internal heap; if neither has room the buffer stays null and every
 *   draw becomes a no-op (the boot log says so). Call pixels() to read the frame back -- that is
 *   what a host-side PPM dump or the layout preview would consume.
 */
class RamFramebufferDisplay : public KryonDisplay {
public:
    RamFramebufferDisplay(int16_t nativeWidth, int16_t nativeHeight);
    ~RamFramebufferDisplay() override;

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

    // --- Text (see "WHAT IT DOES NOT DO" above) ---
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
    const char* backendName() const override { return "ram"; }

    /** True once init() has a buffer to draw into. */
    bool ready() const { return buffer_ != nullptr; }

    /** Row-major RGB565 frame, width() * height() entries, or nullptr when not ready. */
    const uint16_t* pixels() const { return buffer_; }

    /**
     * Draw into a caller-owned buffer of nativeW_ * nativeH_ RGB565 pixels instead of allocating
     * one, and stop owning the storage. Call before init(), which then finds the buffer already
     * present and does nothing.
     *
     * This exists so a backend whose panel already owns a frame buffer can rasterize straight into
     * it rather than keeping a second full-size copy of the frame in RAM and memcpy'ing between
     * them. Nothing on the existing RAM backend calls it, so its behaviour is unchanged.
     */
    void attachBuffer(uint16_t* buffer);

private:
    void putPixel(int32_t x, int32_t y, uint16_t color);
    void hLine(int32_t x, int32_t y, int32_t w, uint16_t color);
    void vLine(int32_t x, int32_t y, int32_t h, uint16_t color);
    void circleHelper(int32_t x0, int32_t y0, int32_t r, uint8_t corners, int32_t delta,
                      uint16_t color, bool filled);
    static int16_t glyphAdvance(uint8_t font);

    uint16_t* buffer_ = nullptr;
    bool ownsBuffer_ = true;
    int16_t nativeW_ = 0;
    int16_t nativeH_ = 0;
    int16_t w_ = 0;
    int16_t h_ = 0;
    uint8_t rotation_ = 0;
    uint16_t textColor_ = 0xFFFF;
    uint16_t textBg_ = 0x0000;
    uint8_t textSize_ = 1;
    uint8_t textDatum_ = 0;
    uint8_t lastFont_ = 1;
};

#endif // RAM_FRAMEBUFFER_DISPLAY_H
