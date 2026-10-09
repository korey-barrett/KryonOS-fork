// This backend IS TFT_eSPI -- see the note in TftEspiDisplay.h. It is compiled out wherever the
// real library is not built (KRYONOS_KRYON_SPRITE), which on the ESP32-S31 is everywhere.
#if !defined(KRYONOS_KRYON_SPRITE)

#include "Hal/Display/TftEspiDisplay.h"

// An abstract adapter would only be diagnosed where something constructs one, which is the board
// files -- far from here. Fail right here instead, the moment an interface method loses its
// override. __is_abstract is a compiler builtin, so this costs no include.
static_assert(!__is_abstract(TftEspiDisplay),
              "TftEspiDisplay must implement every KryonDisplay pure virtual");

// Every method is an explicit, qualified forward to the TFT_eSPI implementation. The qualification
// matters: without it the derived declaration would call itself.

// --- Lifecycle ---------------------------------------------------------------------------------
void TftEspiDisplay::init(uint8_t tc) { TFT_eSPI::init(tc); }
void TftEspiDisplay::setRotation(uint8_t rotation) { TFT_eSPI::setRotation(rotation); }
uint8_t TftEspiDisplay::getRotation() { return TFT_eSPI::getRotation(); }
int16_t TftEspiDisplay::width() { return TFT_eSPI::width(); }
int16_t TftEspiDisplay::height() { return TFT_eSPI::height(); }

// --- Filled and outlined shapes ------------------------------------------------------------------
void TftEspiDisplay::fillScreen(uint32_t color) { TFT_eSPI::fillScreen(color); }

void TftEspiDisplay::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    TFT_eSPI::fillRect(x, y, w, h, color);
}

void TftEspiDisplay::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    TFT_eSPI::drawRect(x, y, w, h, color);
}

void TftEspiDisplay::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                                   uint32_t color) {
    TFT_eSPI::drawRoundRect(x, y, w, h, radius, color);
}

void TftEspiDisplay::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                                   uint32_t color) {
    TFT_eSPI::fillRoundRect(x, y, w, h, radius, color);
}

void TftEspiDisplay::drawLine(int32_t xs, int32_t ys, int32_t xe, int32_t ye, uint32_t color) {
    TFT_eSPI::drawLine(xs, ys, xe, ye, color);
}

void TftEspiDisplay::drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t color) {
    TFT_eSPI::drawFastVLine(x, y, h, color);
}

void TftEspiDisplay::drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t color) {
    TFT_eSPI::drawFastHLine(x, y, w, color);
}

void TftEspiDisplay::drawPixel(int32_t x, int32_t y, uint32_t color) {
    TFT_eSPI::drawPixel(x, y, color);
}

void TftEspiDisplay::drawCircle(int32_t x, int32_t y, int32_t r, uint32_t color) {
    TFT_eSPI::drawCircle(x, y, r, color);
}

void TftEspiDisplay::fillCircle(int32_t x, int32_t y, int32_t r, uint32_t color) {
    TFT_eSPI::fillCircle(x, y, r, color);
}

void TftEspiDisplay::drawTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3,
                                  int32_t y3, uint32_t color) {
    TFT_eSPI::drawTriangle(x1, y1, x2, y2, x3, y3, color);
}

void TftEspiDisplay::fillTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3,
                                  int32_t y3, uint32_t color) {
    TFT_eSPI::fillTriangle(x1, y1, x2, y2, x3, y3, color);
}

void TftEspiDisplay::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* data) {
    TFT_eSPI::pushImage(x, y, w, h, data);
}

// --- Text ---------------------------------------------------------------------------------------
void TftEspiDisplay::setTextColor(uint16_t color) { TFT_eSPI::setTextColor(color); }

void TftEspiDisplay::setTextColor(uint16_t fgcolor, uint16_t bgcolor, bool bgfill) {
    TFT_eSPI::setTextColor(fgcolor, bgcolor, bgfill);
}

void TftEspiDisplay::setTextDatum(uint8_t datum) { TFT_eSPI::setTextDatum(datum); }
void TftEspiDisplay::setTextSize(uint8_t size) { TFT_eSPI::setTextSize(size); }

void TftEspiDisplay::drawString(const char* string, int32_t x, int32_t y) {
    TFT_eSPI::drawString(string, x, y);
}

void TftEspiDisplay::drawString(const char* string, int32_t x, int32_t y, uint8_t font) {
    TFT_eSPI::drawString(string, x, y, font);
}

void TftEspiDisplay::drawString(const String& string, int32_t x, int32_t y) {
    TFT_eSPI::drawString(string, x, y);
}

void TftEspiDisplay::drawString(const String& string, int32_t x, int32_t y, uint8_t font) {
    TFT_eSPI::drawString(string, x, y, font);
}

int16_t TftEspiDisplay::textWidth(const char* string) { return TFT_eSPI::textWidth(string); }

int16_t TftEspiDisplay::textWidth(const char* string, uint8_t font) {
    return TFT_eSPI::textWidth(string, font);
}

int16_t TftEspiDisplay::textWidth(const String& string) { return TFT_eSPI::textWidth(string); }

int16_t TftEspiDisplay::textWidth(const String& string, uint8_t font) {
    return TFT_eSPI::textWidth(string, font);
}

// --- Colour and bus helpers ---------------------------------------------------------------------
uint16_t TftEspiDisplay::color565(uint8_t red, uint8_t green, uint8_t blue) {
    return TFT_eSPI::color565(red, green, blue);
}

void TftEspiDisplay::setSwapBytes(bool swap) { TFT_eSPI::setSwapBytes(swap); }
void TftEspiDisplay::startWrite() { TFT_eSPI::startWrite(); }
void TftEspiDisplay::endWrite() { TFT_eSPI::endWrite(); }

#endif // !KRYONOS_KRYON_SPRITE
