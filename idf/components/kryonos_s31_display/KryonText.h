#ifndef KRYON_TEXT_H
#define KRYON_TEXT_H

#include <stdint.h>

/**
 * Text rendering for backends that cannot use TFT_eSPI, but must produce the same pixels.
 *
 * WHY THIS IS A PORT AND NOT A REWRITE
 *   The KryonOS UI lays itself out with TFT_eSPI's textWidth() and positions every label with
 *   TFT_eSPI's text datums. Re-deriving that geometry from scratch would mean re-deriving the datum
 *   switch, the per-font baseline, the byte-padded advance of font 2 and the RLE run model -- and any
 *   one of them being subtly wrong misplaces *every* string on *every* screen, which is far harder to
 *   debug than an obviously broken renderer. So the algorithms below are ported from TFT_eSPI 2.5.43
 *   (TFT_eSPI.cpp: drawChar, drawString, textWidth, fontHeight) with one change: the calls that wrote
 *   to a TFT_eSPI bus -- drawPixel, fillRect, setWindow/tft_Write_16, pushBlock -- are replaced by two
 *   virtuals on a surface. Nothing else moved.
 *
 *   The viewport (_vpX/_vpY/_vpW/_vpH) and the CP437 toggle are TFT_eSPI features KryonOS never uses;
 *   their clipping checks are omitted, and the CP437 behaviour that does apply to the default
 *   (_cp437 == 0, so character codes above 175 shift by one) is kept.
 *
 * FONT IDS
 *   Only 1, 2 and 4 exist here, because those are the only ids KryonOS ever passes (UiLayout picks
 *   fontSmall=1, fontBody=2, fontHeader=4) and the only three font tables vendored. Asking for any
 *   other id renders nothing and measures zero -- TFT_eSPI would do the same with those fonts
 *   unloaded, so the failure mode matches.
 */

namespace kryon_text {

/** The two operations the ported glyph code needs from whatever it is drawing into. */
class Surface {
public:
    virtual ~Surface() {}
    virtual void drawPixel(int32_t x, int32_t y, uint16_t color) = 0;
    virtual void fillRect(int32_t x, int32_t y, int32_t w, int32_t h, uint16_t color) = 0;
};

/** Datum constants, matching TFT_eSPI's values. Re-exported by the TFT_eSPI.h shim. */
enum : uint8_t {
    TL_DATUM = 0,
    TC_DATUM = 1,
    TR_DATUM = 2,
    ML_DATUM = 3,
    MC_DATUM = 4,
    MR_DATUM = 5,
    BL_DATUM = 6,
    BC_DATUM = 7,
    BR_DATUM = 8,
    L_BASELINE = 9,
    C_BASELINE = 10,
    R_BASELINE = 11,
};

/** True for a font id this implementation has data for. */
bool fontSupported(uint8_t font);

/** Glyph cell height for a font, in pixels, before textSize. TFT_eSPI::fontHeight. */
int16_t fontHeight(uint8_t font, uint8_t size);

/**
 * Rendered width of a string at a font and size -- TFT_eSPI::textWidth.
 *
 * Font 1 sums a flat 6 px per character; fonts 2 and 4 sum the per-glyph widths from their width
 * tables, with unprintable characters counted as a space. drawChar() advances by exactly the same
 * numbers, so textWidth() is a true predictor of where a string ends.
 */
int16_t textWidth(const char* string, uint8_t font, uint8_t size);

/**
 * Draw one character and return the advance -- TFT_eSPI::drawChar.
 *
 * `bg != fg` fills each glyph cell with `bg` first, which is how the opaque text mode works.
 */
int16_t drawChar(Surface& surface, uint16_t uniCode, int32_t x, int32_t y, uint8_t font,
                 uint16_t fg, uint16_t bg, uint8_t size);

/**
 * Draw a string with datum alignment and return the total advance -- TFT_eSPI::drawString.
 *
 * The datum is applied to the start coordinate once, here; drawChar does not re-apply it. `length` is
 * taken from the string itself.
 */
int16_t drawString(Surface& surface, const char* string, int32_t poX, int32_t poY, uint8_t font,
                   uint16_t fg, uint16_t bg, uint8_t size, uint8_t datum);

} // namespace kryon_text

#endif // KRYON_TEXT_H
