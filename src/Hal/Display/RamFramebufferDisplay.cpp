#include "Hal/Display/RamFramebufferDisplay.h"

#include <Arduino.h>

static_assert(!__is_abstract(RamFramebufferDisplay),
              "RamFramebufferDisplay must implement every KryonDisplay pure virtual");

RamFramebufferDisplay::RamFramebufferDisplay(int16_t nativeWidth, int16_t nativeHeight)
    : nativeW_(nativeWidth), nativeH_(nativeHeight), w_(nativeWidth), h_(nativeHeight) {}

RamFramebufferDisplay::~RamFramebufferDisplay() {
    if (buffer_) {
        free(buffer_);
        buffer_ = nullptr;
    }
}

// --- Lifecycle ----------------------------------------------------------------------------------

void RamFramebufferDisplay::init(uint8_t tc) {
    (void)tc;
    if (buffer_) return;

    size_t bytes = static_cast<size_t>(nativeW_) * static_cast<size_t>(nativeH_) * sizeof(uint16_t);
    // PSRAM first: at 240x320 this is 150 KB and at 320x480 it is 300 KB, neither of which fits in
    // the internal heap alongside the rest of the OS.
    buffer_ = static_cast<uint16_t*>(ps_malloc(bytes));
    if (!buffer_) buffer_ = static_cast<uint16_t*>(malloc(bytes));

    if (!buffer_) {
        Serial.printf("[Display:ram] could not allocate %u bytes for a %dx%d framebuffer; "
                      "drawing is disabled.\n",
                      static_cast<unsigned>(bytes), static_cast<int>(nativeW_),
                      static_cast<int>(nativeH_));
        return;
    }

    fillScreen(0x0000);
    Serial.printf("[Display:ram] %dx%d framebuffer ready (%u bytes)\n", static_cast<int>(w_),
                  static_cast<int>(h_), static_cast<unsigned>(bytes));
}

void RamFramebufferDisplay::setRotation(uint8_t rotation) {
    rotation_ = rotation & 0x03;
    // There is no panel to send a MADCTL to, so rotation only decides which way the canvas is
    // measured -- it must match how the panel would have been oriented for the layout to agree.
    bool landscape = (rotation_ & 1) != 0;
    w_ = landscape ? nativeH_ : nativeW_;
    h_ = landscape ? nativeW_ : nativeH_;
}

uint8_t RamFramebufferDisplay::getRotation() { return rotation_; }
int16_t RamFramebufferDisplay::width() { return w_; }
int16_t RamFramebufferDisplay::height() { return h_; }

// --- Pixel plumbing -----------------------------------------------------------------------------

void RamFramebufferDisplay::putPixel(int32_t x, int32_t y, uint16_t color) {
    if (!buffer_ || x < 0 || y < 0 || x >= w_ || y >= h_) return;
    buffer_[static_cast<size_t>(y) * static_cast<size_t>(w_) + static_cast<size_t>(x)] = color;
}

void RamFramebufferDisplay::hLine(int32_t x, int32_t y, int32_t w, uint16_t color) {
    if (!buffer_ || w <= 0 || y < 0 || y >= h_) return;
    if (x < 0) { w += x; x = 0; }
    if (x + w > w_) w = w_ - x;
    if (w <= 0) return;
    uint16_t* row = buffer_ + static_cast<size_t>(y) * static_cast<size_t>(w_) + x;
    for (int32_t i = 0; i < w; i++) row[i] = color;
}

void RamFramebufferDisplay::vLine(int32_t x, int32_t y, int32_t h, uint16_t color) {
    if (!buffer_ || h <= 0 || x < 0 || x >= w_) return;
    if (y < 0) { h += y; y = 0; }
    if (y + h > h_) h = h_ - y;
    for (int32_t i = 0; i < h; i++) putPixel(x, y + i, color);
}

// --- Filled and outlined shapes -------------------------------------------------------------------

void RamFramebufferDisplay::fillScreen(uint32_t color) {
    fillRect(0, 0, w_, h_, color);
}

void RamFramebufferDisplay::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    if (!buffer_ || w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > w_) w = w_ - x;
    if (y + h > h_) h = h_ - y;
    for (int32_t row = 0; row < h; row++) hLine(x, y + row, w, static_cast<uint16_t>(color));
}

void RamFramebufferDisplay::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint32_t color) {
    uint16_t c = static_cast<uint16_t>(color);
    hLine(x, y, w, c);
    hLine(x, y + h - 1, w, c);
    vLine(x, y, h, c);
    vLine(x + w - 1, y, h, c);
}

void RamFramebufferDisplay::drawFastHLine(int32_t x, int32_t y, int32_t w, uint32_t color) {
    hLine(x, y, w, static_cast<uint16_t>(color));
}

void RamFramebufferDisplay::drawFastVLine(int32_t x, int32_t y, int32_t h, uint32_t color) {
    vLine(x, y, h, static_cast<uint16_t>(color));
}

void RamFramebufferDisplay::drawPixel(int32_t x, int32_t y, uint32_t color) {
    putPixel(x, y, static_cast<uint16_t>(color));
}

void RamFramebufferDisplay::drawLine(int32_t xs, int32_t ys, int32_t xe, int32_t ye,
                                     uint32_t color) {
    uint16_t c = static_cast<uint16_t>(color);
    int32_t dx = abs(xe - xs), sx = xs < xe ? 1 : -1;
    int32_t dy = -abs(ye - ys), sy = ys < ye ? 1 : -1;
    int32_t err = dx + dy;
    while (true) {
        putPixel(xs, ys, c);
        if (xs == xe && ys == ye) break;
        int32_t e2 = 2 * err;
        if (e2 >= dy) { err += dy; xs += sx; }
        if (e2 <= dx) { err += dx; ys += sy; }
    }
}

// Rounds one or more corners of a rectangle. `corners` uses TFT_eSPI's corner bit order so the
// callers below read the same way as their TFT_eSPI counterparts.
void RamFramebufferDisplay::circleHelper(int32_t x0, int32_t y0, int32_t r, uint8_t corners,
                                         int32_t delta, uint16_t color, bool filled) {
    int32_t f = 1 - r, ddF_x = 1, ddF_y = -2 * r, x = 0, y = r;
    while (x < y) {
        if (f >= 0) { y--; ddF_y += 2; f += ddF_y; }
        x++;
        ddF_x += 2;
        f += ddF_x;
        if (filled) {
            if (corners & 0x4) { hLine(x0 - x, y0 + y, 2 * x + 1 + delta, color); }
            if (corners & 0x2) { hLine(x0 - x, y0 - y, 2 * x + 1 + delta, color); }
            if (corners & 0x8) { hLine(x0 - y, y0 + x, 2 * y + 1 + delta, color); }
            if (corners & 0x1) { hLine(x0 - y, y0 - x, 2 * y + 1 + delta, color); }
        } else {
            if (corners & 0x4) { putPixel(x0 + x, y0 + y, color); putPixel(x0 + y, y0 + x, color); }
            if (corners & 0x2) { putPixel(x0 + x, y0 - y, color); putPixel(x0 + y, y0 - x, color); }
            if (corners & 0x8) { putPixel(x0 - x, y0 + y, color); putPixel(x0 - y, y0 + x, color); }
            if (corners & 0x1) { putPixel(x0 - x, y0 - y, color); putPixel(x0 - y, y0 - x, color); }
        }
    }
}

void RamFramebufferDisplay::drawCircle(int32_t x, int32_t y, int32_t r, uint32_t color) {
    circleHelper(x, y, r, 0xF, 0, static_cast<uint16_t>(color), false);
}

void RamFramebufferDisplay::fillCircle(int32_t x, int32_t y, int32_t r, uint32_t color) {
    uint16_t c = static_cast<uint16_t>(color);
    vLine(x, y - r, 2 * r + 1, c);
    circleHelper(x, y, r, 0x1 | 0x2 | 0x4 | 0x8, 0, c, true);
}

void RamFramebufferDisplay::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h,
                                          int32_t radius, uint32_t color) {
    uint16_t c = static_cast<uint16_t>(color);
    if (radius <= 0) { drawRect(x, y, w, h, color); return; }
    if (radius > (w < h ? w : h) / 2) radius = (w < h ? w : h) / 2;

    hLine(x + radius, y, w - 2 * radius, c);
    hLine(x + radius, y + h - 1, w - 2 * radius, c);
    vLine(x, y + radius, h - 2 * radius, c);
    vLine(x + w - 1, y + radius, h - 2 * radius, c);

    circleHelper(x + radius, y + radius, radius, 0x1, 0, c, false);
    circleHelper(x + w - radius - 1, y + radius, radius, 0x2, 0, c, false);
    circleHelper(x + w - radius - 1, y + h - radius - 1, radius, 0x4, 0, c, false);
    circleHelper(x + radius, y + h - radius - 1, radius, 0x8, 0, c, false);
}

void RamFramebufferDisplay::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h,
                                          int32_t radius, uint32_t color) {
    uint16_t c = static_cast<uint16_t>(color);
    if (radius <= 0) { fillRect(x, y, w, h, color); return; }
    if (radius > (w < h ? w : h) / 2) radius = (w < h ? w : h) / 2;

    fillRect(x + radius, y, w - 2 * radius, h, color);
    fillRect(x, y + radius, radius, h - 2 * radius, color);
    fillRect(x + w - radius, y + radius, radius, h - 2 * radius, color);

    circleHelper(x + radius, y + radius, radius, 0x1, 0, c, true);
    circleHelper(x + w - radius - 1, y + radius, radius, 0x2, 0, c, true);
    circleHelper(x + w - radius - 1, y + h - radius - 1, radius, 0x4, 0, c, true);
    circleHelper(x + radius, y + h - radius - 1, radius, 0x8, 0, c, true);
}

void RamFramebufferDisplay::drawTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                         int32_t x3, int32_t y3, uint32_t color) {
    drawLine(x1, y1, x2, y2, color);
    drawLine(x2, y2, x3, y3, color);
    drawLine(x3, y3, x1, y1, color);
}

void RamFramebufferDisplay::fillTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2,
                                         int32_t x3, int32_t y3, uint32_t color) {
    uint16_t c = static_cast<uint16_t>(color);
    // Order the vertices by y, then walk scanlines and span between the two edges.
    if (y1 > y2) { int32_t t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }
    if (y2 > y3) { int32_t t = y2; y2 = y3; y3 = t; t = x2; x2 = x3; x3 = t; }
    if (y1 > y2) { int32_t t = y1; y1 = y2; y2 = t; t = x1; x1 = x2; x2 = t; }

    int32_t total = y3 - y1;
    if (total == 0) return;
    for (int32_t y = y1; y <= y3; y++) {
        int32_t xa = x1 + (int32_t)((int64_t)(x3 - x1) * (y - y1) / total);
        int32_t xb = (y < y2) ? (y2 == y1 ? x2 : x1 + (int32_t)((int64_t)(x2 - x1) * (y - y1) / (y2 - y1)))
                              : (y3 == y2 ? x2 : x2 + (int32_t)((int64_t)(x3 - x2) * (y - y2) / (y3 - y2)));
        if (xa > xb) { int32_t t = xa; xa = xb; xb = t; }
        hLine(xa, y, xb - xa + 1, c);
    }
}

void RamFramebufferDisplay::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* data) {
    if (!buffer_ || !data) return;
    for (int32_t row = 0; row < h; row++) {
        int32_t dy = y + row;
        if (dy < 0 || dy >= h_) continue;
        for (int32_t col = 0; col < w; col++) {
            putPixel(x + col, dy, data[static_cast<size_t>(row) * static_cast<size_t>(w) + col]);
        }
    }
}

// --- Text ---------------------------------------------------------------------------------------

void RamFramebufferDisplay::setTextColor(uint16_t color) { textColor_ = color; }

void RamFramebufferDisplay::setTextColor(uint16_t fgcolor, uint16_t bgcolor, bool bgfill) {
    (void)bgfill;
    textColor_ = fgcolor;
    textBg_ = bgcolor;
}

void RamFramebufferDisplay::setTextDatum(uint8_t datum) { textDatum_ = datum; }
void RamFramebufferDisplay::setTextSize(uint8_t size) { textSize_ = size ? size : 1; }

// No font is embedded in this backend, so nothing is painted. The calls are accepted rather than
// ignored silently so a board can be brought up and the geometry checked before text matters.
void RamFramebufferDisplay::drawString(const char* string, int32_t x, int32_t y) {
    (void)string; (void)x; (void)y;
}

void RamFramebufferDisplay::drawString(const char* string, int32_t x, int32_t y, uint8_t font) {
    (void)string; (void)x; (void)y; (void)font;
}

void RamFramebufferDisplay::drawString(const String& string, int32_t x, int32_t y) {
    (void)string; (void)x; (void)y;
}

void RamFramebufferDisplay::drawString(const String& string, int32_t x, int32_t y, uint8_t font) {
    (void)string; (void)x; (void)y; (void)font;
}

// Rough advance widths of TFT_eSPI's fonts 1 (GLCD), 2 and 4, so text that is laid out but not
// painted still occupies a plausible amount of room during bring-up.
int16_t RamFramebufferDisplay::glyphAdvance(uint8_t font) {
    switch (font) {
        case 2:  return 8;
        case 4:  return 13;
        case 6:  return 8;
        case 7:  return 12;
        case 8:  return 16;
        default: return 6;
    }
}

int16_t RamFramebufferDisplay::textWidth(const char* string) {
    return textWidth(string, lastFont_);
}

int16_t RamFramebufferDisplay::textWidth(const char* string, uint8_t font) {
    if (!string) return 0;
    lastFont_ = font;
    return static_cast<int16_t>(strlen(string) * glyphAdvance(font) * textSize_);
}

int16_t RamFramebufferDisplay::textWidth(const String& string) {
    return textWidth(string.c_str(), lastFont_);
}

int16_t RamFramebufferDisplay::textWidth(const String& string, uint8_t font) {
    return textWidth(string.c_str(), font);
}

// --- Colour and bus helpers ---------------------------------------------------------------------

uint16_t RamFramebufferDisplay::color565(uint8_t red, uint8_t green, uint8_t blue) {
    return static_cast<uint16_t>(((red & 0xF8) << 8) | ((green & 0xFC) << 3) | (blue >> 3));
}

void RamFramebufferDisplay::setSwapBytes(bool swap) { (void)swap; }
void RamFramebufferDisplay::startWrite() {}
void RamFramebufferDisplay::endWrite() {}
