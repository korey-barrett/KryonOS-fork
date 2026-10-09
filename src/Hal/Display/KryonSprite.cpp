// Compiled only where a backend has no real TFT_eSPI -- see the note at the top of KryonText.cpp.
#if defined(KRYONOS_KRYON_SPRITE)

#include "KryonSprite.h"

#include "Hal/Display/RamFramebufferDisplay.h"

KryonSprite::~KryonSprite() { deleteSprite(); }

void* KryonSprite::createSprite(int16_t w, int16_t h) {
    if (canvas_) deleteSprite();
    if (w <= 0 || h <= 0) return nullptr;

    // RamFramebufferDisplay is constructed at its final size rather than resized: its init() does a
    // single allocation, and a resize would either need a second entry point on that class (which is
    // shared with the existing RAM backend and stays untouched) or a reallocation here that would
    // leave the size and the buffer able to disagree.
    canvas_ = new RamFramebufferDisplay(w, h);
    if (!canvas_) return nullptr;

    canvas_->init();
    if (!canvas_->ready()) {
        delete canvas_;
        canvas_ = nullptr;
        return nullptr;
    }

    w_ = w;
    h_ = h;
    return pixels();
}

void* KryonSprite::attachBuffer(uint16_t* buffer, int16_t w, int16_t h) {
    if (canvas_) deleteSprite();
    if (!buffer || w <= 0 || h <= 0) return nullptr;

    canvas_ = new RamFramebufferDisplay(w, h);
    if (!canvas_) return nullptr;

    // Attach BEFORE init(): init() allocates only when it finds no buffer, so handing it one first
    // is what keeps it from making a second full-size copy alongside the panel's framebuffer.
    canvas_->attachBuffer(buffer);
    canvas_->init();
    if (!canvas_->ready()) {
        delete canvas_;
        canvas_ = nullptr;
        return nullptr;
    }

    w_ = w;
    h_ = h;
    return pixels();
}

void KryonSprite::deleteSprite() {
    delete canvas_;
    canvas_ = nullptr;
    w_ = 0;
    h_ = 0;
}

void KryonSprite::setColorDepth(int8_t bits) {
    // See COLOUR DEPTH in the header: recorded, never acted on. Anything unexpected falls back to 16
    // rather than being stored, so getColorDepth() never reports a depth the buffer does not have.
    depth_ = (bits == 8) ? 8 : 16;
}

void KryonSprite::setRotation(uint8_t rotation) {
    if (canvas_) canvas_->setRotation(rotation);
    // The canvas owns the measurement, so take the size back from it rather than recomputing the
    // swap here. A sprite that was never created keeps the size it was created for, which is 0.
    w_ = canvas_ ? canvas_->width() : w_;
    h_ = canvas_ ? canvas_->height() : h_;
}

uint16_t* KryonSprite::pixels() {
    // RamFramebufferDisplay::pixels() is const because the RAM backend is only ever read back for
    // tests and frame dumps. A sprite is written to in place by pushSprite and by the JS bindings, so
    // the constness is dropped here rather than widened on the shared class.
    return canvas_ ? const_cast<uint16_t*>(canvas_->pixels()) : nullptr;
}

// --- Presentation -------------------------------------------------------------------------------

void KryonSprite::pushSprite(KryonDisplay* display, int32_t x, int32_t y) {
    if (!display || !canvas_) return;
    display->pushImage(x, y, w_, h_, pixels());
}

void KryonSprite::pushSprite(int32_t x, int32_t y) { pushSprite(target_, x, y); }

// --- kryon_text::Surface ------------------------------------------------------------------------

void KryonSprite::drawPixel(int32_t x, int32_t y, uint16_t color) {
    if (canvas_) canvas_->drawPixel(x, y, color);
}

void KryonSprite::fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
    if (canvas_) canvas_->fillRect(x, y, w, h, color);
}

// --- Geometry, forwarded ------------------------------------------------------------------------

void KryonSprite::fillScreen(uint16_t color) {
    if (canvas_) canvas_->fillScreen(color);
}

void KryonSprite::fillSprite(uint16_t color) { fillScreen(color); }

void KryonSprite::drawRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) {
    if (canvas_) canvas_->drawRect(x, y, w, h, color);
}

void KryonSprite::drawRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                                uint16_t color) {
    if (canvas_) canvas_->drawRoundRect(x, y, w, h, radius, color);
}

void KryonSprite::fillRoundRect(int32_t x, int32_t y, int32_t w, int32_t h, int32_t radius,
                                uint16_t color) {
    if (canvas_) canvas_->fillRoundRect(x, y, w, h, radius, color);
}

void KryonSprite::drawLine(int32_t xs, int32_t ys, int32_t xe, int32_t ye, uint16_t color) {
    if (canvas_) canvas_->drawLine(xs, ys, xe, ye, color);
}

void KryonSprite::drawFastVLine(int32_t x, int32_t y, int32_t h, uint16_t color) {
    if (canvas_) canvas_->drawFastVLine(x, y, h, color);
}

void KryonSprite::drawFastHLine(int32_t x, int32_t y, int32_t w, uint16_t color) {
    if (canvas_) canvas_->drawFastHLine(x, y, w, color);
}

void KryonSprite::drawCircle(int32_t x, int32_t y, int32_t r, uint16_t color) {
    if (canvas_) canvas_->drawCircle(x, y, r, color);
}

void KryonSprite::fillCircle(int32_t x, int32_t y, int32_t r, uint16_t color) {
    if (canvas_) canvas_->fillCircle(x, y, r, color);
}

void KryonSprite::drawTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                               uint16_t color) {
    if (canvas_) canvas_->drawTriangle(x1, y1, x2, y2, x3, y3, color);
}

void KryonSprite::fillTriangle(int32_t x1, int32_t y1, int32_t x2, int32_t y2, int32_t x3, int32_t y3,
                               uint16_t color) {
    if (canvas_) canvas_->fillTriangle(x1, y1, x2, y2, x3, y3, color);
}

void KryonSprite::pushImage(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t* data) {
    if (canvas_) canvas_->pushImage(x, y, w, h, data);
}

// --- Text ---------------------------------------------------------------------------------------

void KryonSprite::setTextColor(uint16_t color) {
    textFg_ = color;
    textBg_ = color;  // transparent: TFT_eSPI treats fg == bg as "do not fill"
    textBgFill_ = false;
}

void KryonSprite::setTextColor(uint16_t fgcolor, uint16_t bgcolor, bool bgfill) {
    textFg_ = fgcolor;
    textBg_ = bgcolor;
    textBgFill_ = bgfill;
}

void KryonSprite::drawString(const char* string, int32_t x, int32_t y) {
    drawString(string, x, y, lastFont_);
}

void KryonSprite::drawString(const char* string, int32_t x, int32_t y, uint8_t font) {
    if (!canvas_) return;
    lastFont_ = font;
    // bgfill is not threaded through: KryonText fills the glyph cell whenever fg != bg, which is
    // TFT_eSPI's own rule -- the bgfill flag only ever decided whether a *transparent* background was
    // filled, and KryonOS never asks for that.
    kryon_text::drawString(*this, string, x, y, font, textFg_, textBg_, size_, datum_);
}

int16_t KryonSprite::textWidth(const char* string) { return textWidth(string, lastFont_); }

int16_t KryonSprite::textWidth(const char* string, uint8_t font) {
    return kryon_text::textWidth(string, font, size_);
}

#endif // KRYONOS_KRYON_SPRITE
