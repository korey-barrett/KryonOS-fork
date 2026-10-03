// Ported from TFT_eSPI 2.5.43 (TFT_eSPI.cpp) -- see KryonText.h for why this is a port rather than a
// rewrite, and for the two deliberate deviations (no viewport, font 2 clamped to its real width).
//
// The font tables are vendored verbatim under ./fonts and are #included rather than compiled
// separately: glcdfont.c declares its table `static`, and TFT_eSPI itself #includes all three for the
// same reason. On ESP32 targets PROGMEM is empty and rodata is directly addressable, so TFT_eSPI's
// pgm_read_byte/pgm_read_dword indirection is dropped -- it would be a no-op, and indexing the tables
// directly is what makes the pointer tables below readable.

#include "KryonText.h"

#include <string.h>

#ifndef PROGMEM
#define PROGMEM
#endif

#include "fonts/glcdfont.c"
#include "fonts/Font16.c"
#include "fonts/Font32rle.c"

// Font 2 / font 4 table metadata. These constants live in the vendored .h files, which are not used
// here because they reach for the .c through TFT_eSPI's own directory layout; the numbers are copied
// from Font16.h / Font32rle.h and the names kept so they can be grepped against upstream.
static const int kChrHgtF16 = 16;    // chr_hgt_f16
static const int kBaselineF16 = 13;  // baseline_f16
static const int kChrHgtF32 = 26;    // chr_hgt_f32
static const int kBaselineF32 = 19;  // baseline_f32

namespace kryon_text {

bool fontSupported(uint8_t font) { return font == 1 || font == 2 || font == 4; }

int16_t fontHeight(uint8_t font, uint8_t size) {
    switch (font) {
        case 2: return (int16_t)(kChrHgtF16 * size);
        case 4: return (int16_t)(kChrHgtF32 * size);
        case 1: return (int16_t)(8 * size);
        default: return 0;
    }
}

int16_t textWidth(const char* string, uint8_t font, uint8_t size) {
    if (!string) return 0;

    int32_t strWidth = 0;

    if (font > 1 && font < 9) {
        // TFT_eSPI indexes the width table by (uniCode - 32) with the subtraction folded into the
        // base pointer. Character codes outside 32..127 -- including the terminating NUL, which is
        // why this loop is a plain walk -- use the space width (entry 0), matching TFT_eSPI's
        // "set illegal character = space width".
        const unsigned char* widthTable =
            (font == 2) ? widtbl_f16 : (font == 4) ? widtbl_f32 : nullptr;
        if (!widthTable) return 0;
        for (const char* p = string; *p; p++) {
            uint16_t uniCode = (uint8_t)*p;
            strWidth += (uniCode > 31 && uniCode < 128) ? widthTable[uniCode - 32] : widthTable[0];
        }
    } else if (font == 1) {
        strWidth = (int32_t)strlen(string) * 6;
    } else {
        return 0;
    }

    return (int16_t)(strWidth * size);
}

// --- Font 1: the 5x7 GLCD font, drawn into a 6x8 cell ------------------------------------------
static void drawCharGlcd(Surface& s, uint16_t c, int32_t x, int32_t y, uint16_t fg, uint16_t bg,
                         uint8_t size) {
    if (c > 255) return;
    // TFT_eSPI's _cp437 defaults to 0, which shifts codes above 175 by one. Kept for fidelity; no
    // KryonOS string reaches that range, but a mismatch here would show up as one wrong glyph.
    if (c > 175) c++;

    bool fillbg = (bg != fg);

    // Six columns; the sixth is the inter-character gap and is always blank. Each column byte holds
    // one bit per row with the top row in bit 0.
    for (int i = 0; i < 6; i++) {
        uint8_t line = (i == 5) ? 0x00 : font[(c * 5) + i];
        for (int j = 0; j < 8; j++) {
            if (line & 0x01) {
                if (size == 1) s.drawPixel(x + i, y + j, fg);
                else s.fillRect(x + i * size, y + j * size, size, size, fg);
            } else if (fillbg) {
                if (size == 1) s.drawPixel(x + i, y + j, bg);
                else s.fillRect(x + i * size, y + j * size, size, size, bg);
            }
            line >>= 1;
        }
    }
}

// --- Font 2: 16 px, one byte per row segment, MSB leftmost -------------------------------------
static void drawCharF16(Surface& s, uint16_t c, int32_t x, int32_t y, uint16_t fg, uint16_t bg,
                        uint8_t size) {
    const int idx = c - 32;
    const unsigned char* data = chrtbl_f16[idx];
    const int width = widtbl_f16[idx];
    const int bytesPerRow = (width + 6) / 8;  // TFT_eSPI's padding rule, off-by-one and all

    // Clamped to `width` rather than the padded byte span. TFT_eSPI's fast path does exactly this
    // (setWindow + a `pX` counter that stops at width); its slow path draws the padding instead, but
    // the padding bits are blank in this font, so the two agree on every glyph.
    const int drawW = (width < bytesPerRow * 8) ? width : bytesPerRow * 8;

    for (int i = 0; i < kChrHgtF16; i++) {
        const int32_t ty = y + i * size;
        if (fg != bg) s.fillRect(x, ty, width * size, size, bg);
        const unsigned char* row = data + (size_t)bytesPerRow * i;
        for (int j = 0; j < drawW; j++) {
            uint8_t bits = row[j >> 3];
            if (bits & (0x80 >> (j & 7))) {
                if (size == 1) s.drawPixel(x + j, ty, fg);
                else s.fillRect(x + j * size, ty, size, size, fg);
            }
        }
    }
}

// --- Font 4: 8-bit run-length encoded, 26 px tall ----------------------------------------------
static void drawCharF32Rle(Surface& s, uint16_t c, int32_t x, int32_t y, uint16_t fg, uint16_t bg,
                           uint8_t size) {
    const int idx = c - 32;
    const unsigned char* data = chrtbl_f32[idx];
    const int width = widtbl_f32[idx];
    const int32_t total = (int32_t)width * kChrHgtF32;

    // Each byte is a run: bit 7 selects the colour, the low 7 bits are (length - 1).
    int32_t pc = 0;
    uint16_t pcol = 0;
    bool pf = true;

    while (pc < total) {
        uint8_t line = *data++;
        if (line & 0x80) {
            pcol = fg;
            line &= 0x7F;
            pf = true;
        } else {
            pcol = bg;
            if (fg == bg) pf = false;  // transparent background
        }
        int32_t run = (int32_t)line + 1;

        int32_t px = pc % width;
        int32_t py = pc / width;
        int32_t tx = x + size * px;
        int32_t ty = y + size * py;
        pc += run;

        int32_t pl = 0;
        while (run--) {
            pl++;
            if ((px + pl) >= width) {  // run crosses into the next row
                if (pf) s.fillRect(tx, ty, pl * size, size, pcol);
                pl = 0;
                px = 0;
                tx = x;
                py++;
                ty += size;
            }
        }
        if (pl && pf) s.fillRect(tx, ty, pl * size, size, pcol);
    }
}

int16_t drawChar(Surface& surface, uint16_t uniCode, int32_t x, int32_t y, uint8_t font, uint16_t fg,
                 uint16_t bg, uint8_t size) {
    if (!uniCode) return 0;
    if (size == 0) size = 1;

    if (font == 1) {
        drawCharGlcd(surface, uniCode, x, y, fg, bg, size);
        return (int16_t)(6 * size);
    }

    // Characters outside printable ASCII render nothing, exactly as TFT_eSPI does with these fonts.
    if (font != 2 && font != 4) return 0;
    if (uniCode < 32 || uniCode > 127) return 0;

    const int width = (font == 2) ? widtbl_f16[uniCode - 32] : widtbl_f32[uniCode - 32];

    if (font == 2) drawCharF16(surface, uniCode, x, y, fg, bg, size);
    else drawCharF32Rle(surface, uniCode, x, y, fg, bg, size);

    return (int16_t)(width * size);
}

int16_t drawString(Surface& surface, const char* string, int32_t poX, int32_t poY, uint8_t font,
                   uint16_t fg, uint16_t bg, uint8_t size, uint8_t datum) {
    if (!string) return 0;
    if (size == 0) size = 1;
    if (!fontSupported(font)) return 0;

    const int16_t cwidth = textWidth(string, font, size);

    // TFT_eSPI: font 1 is measured as 8 px per line whatever the size; the other fonts report their
    // own height. The baseline is 0 for font 1 and the font's own for the rest.
    int16_t cheight = fontHeight(font, size);
    int16_t baseline = 0;
    if (font == 2) baseline = (int16_t)(kBaselineF16 * size);
    else if (font == 4) baseline = (int16_t)(kBaselineF32 * size);

    // The datum shift, ported verbatim (TFT_eSPI::drawString). This is applied once, here --
    // drawChar does not re-apply it.
    switch (datum) {
        case TC_DATUM: poX -= cwidth / 2; break;
        case TR_DATUM: poX -= cwidth; break;
        case ML_DATUM: poY -= cheight / 2; break;
        case MC_DATUM: poX -= cwidth / 2; poY -= cheight / 2; break;
        case MR_DATUM: poX -= cwidth; poY -= cheight / 2; break;
        case BL_DATUM: poY -= cheight; break;
        case BC_DATUM: poX -= cwidth / 2; poY -= cheight; break;
        case BR_DATUM: poX -= cwidth; poY -= cheight; break;
        case L_BASELINE: poY -= baseline; break;
        case C_BASELINE: poX -= cwidth / 2; poY -= baseline; break;
        case R_BASELINE: poX -= cwidth; poY -= baseline; break;
        default: break;  // TL_DATUM
    }

    int16_t sumX = 0;
    for (const char* p = string; *p; p++) {
        uint16_t uniCode = (uint8_t)*p;
        sumX += drawChar(surface, uniCode, poX + sumX, poY, font, fg, bg, size);
    }
    return sumX;
}

} // namespace kryon_text
