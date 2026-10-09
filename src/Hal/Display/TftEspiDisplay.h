#ifndef TFT_ESPI_DISPLAY_H
#define TFT_ESPI_DISPLAY_H

// This backend IS TFT_eSPI -- it inherits it -- so it cannot exist on a target where the real library
// is not built, which is what KRYONOS_KRYON_SPRITE marks. On the ESP32-S31 the shim answers to
// <TFT_eSPI.h> instead, and every member this class forwards would simply be missing; the class is
// therefore compiled out there and the RGB backend takes its place.
#if !defined(KRYONOS_KRYON_SPRITE)

#include <TFT_eSPI.h>

#include "Hal/Display/KryonDisplay.h"

/**
 * The default KryonDisplay backend: the TFT_eSPI driver the OS has always used.
 *
 * It inherits TFT_eSPI rather than wrapping it, so the concrete object keeps the whole TFT_eSPI
 * API (sprites, DMA, readRect, the touch helpers) while also being a KryonDisplay for the
 * resolution-independent UI. The forwarding overrides below are spelled out even though the
 * inherited TFT_eSPI members already match the interface: they document the adapter and keep calls
 * through the concrete type unambiguous (two bases declaring the same name would otherwise make
 * every call ambiguous).
 */
class TftEspiDisplay : public TFT_eSPI, public KryonDisplay {
public:
    TftEspiDisplay() : TFT_eSPI() {}

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
    const char* backendName() const override { return "tft_espi"; }
    TFT_eSPI* nativeTft() override { return this; }
};

#endif // !KRYONOS_KRYON_SPRITE

#endif // TFT_ESPI_DISPLAY_H
