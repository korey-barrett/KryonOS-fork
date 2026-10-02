#ifndef KRYON_DISPLAY_H
#define KRYON_DISPLAY_H

#include <stddef.h>
#include <stdint.h>

class TFT_eSPI;
class String;

/**
 * The drawing surface the KryonOS UI is written against.
 *
 * Everything that paints -- the launcher, settings, keyboard, notifications, the JS bindings --
 * talks to this interface and never to TFT_eSPI directly. A backend implements it and the global
 * `tft` reference is bound to that instance, so swapping the panel driver (or running with no
 * panel at all) is a build-flag decision rather than a source change.
 *
 * The method names and signatures deliberately mirror TFT_eSPI: TftEspiDisplay therefore satisfies
 * the interface by inheritance alone, and the existing call sites read exactly as they did.
 *
 * This header includes no backend and only forward-declares `String`, so a backend that is not
 * TFT_eSPI can implement it without pulling TFT_eSPI into its build.
 */
class KryonDisplay {
public:
    virtual ~KryonDisplay() {}

    // --- Lifecycle -------------------------------------------------------------------------
    virtual void init(uint8_t tc = 0) = 0;
    virtual void setRotation(uint8_t rotation) = 0;
    virtual uint8_t getRotation() = 0;

    /** Panel dimensions after setRotation(); the source of truth for layout. */
    virtual int16_t width() = 0;
    virtual int16_t height() = 0;

    // --- Filled and outlined shapes ---------------------------------------------------------
    virtual void fillScreen(uint32_t color) = 0;
    virtual void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) = 0;
    virtual void drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) = 0;
    virtual void drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                               uint32_t color) = 0;
    virtual void fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                               uint32_t color) = 0;
    virtual void drawLine(int32_t xs, int32_t ys, int32_t xe, int32_t ye, uint32_t color) = 0;
    virtual void drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t color) = 0;
    virtual void drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t color) = 0;
    virtual void drawPixel(int32_t x, int32_t y, uint32_t color) = 0;
    virtual void drawCircle(int32_t x, int32_t y, int32_t r, uint32_t color) = 0;
    virtual void fillCircle(int32_t x, int32_t y, int32_t r, uint32_t color) = 0;
    virtual void drawTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                              uint32_t color) = 0;
    virtual void fillTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                              uint32_t color) = 0;
    virtual void pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* data) = 0;

    // --- Text -------------------------------------------------------------------------------
    virtual void setTextColor(uint16_t color) = 0;
    virtual void setTextColor(uint16_t fgcolor, uint16_t bgcolor, bool bgfill = false) = 0;
    virtual void setTextDatum(uint8_t datum) = 0;
    virtual void setTextSize(uint8_t size) = 0;
    virtual void drawString(const char* string, int32_t x, int32_t y) = 0;
    virtual void drawString(const char* string, int32_t x, int32_t y, uint8_t font) = 0;
    virtual void drawString(const String& string, int32_t x, int32_t y) = 0;
    virtual void drawString(const String& string, int32_t x, int32_t y, uint8_t font) = 0;
    virtual int16_t textWidth(const char* string) = 0;
    virtual int16_t textWidth(const char* string, uint8_t font) = 0;
    virtual int16_t textWidth(const String& string) = 0;
    virtual int16_t textWidth(const String& string, uint8_t font) = 0;

    // --- Colour and bus helpers -------------------------------------------------------------
    virtual uint16_t color565(uint8_t red, uint8_t green, uint8_t blue) = 0;
    virtual void setSwapBytes(bool swap) = 0;
    virtual void startWrite() = 0;
    virtual void endWrite() = 0;

    // --- Backend introspection --------------------------------------------------------------
    /** Short identifier for diagnostics and the boot banner ("tft_espi", "ram", ...). */
    virtual const char* backendName() const = 0;

    /**
     * The underlying TFT_eSPI instance, or nullptr on a backend that is not TFT_eSPI. It exists so
     * sprite allocation can keep using TFT_eSprite where that is available; new code should prefer
     * a KryonSprite, which any backend can supply.
     */
    virtual TFT_eSPI* nativeTft() { return nullptr; }
};

#endif // KRYON_DISPLAY_H
