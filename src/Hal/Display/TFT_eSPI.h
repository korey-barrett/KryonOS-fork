#ifndef KRYON_TFT_ESPI_SHIM_H
#define KRYON_TFT_ESPI_SHIM_H

/**
 * A drop-in replacement for <TFT_eSPI.h> on the ESP32-S31.
 *
 * WHY THIS EXISTS
 *   Twenty-one files in src/ include <TFT_eSPI.h>. Fourteen of them -- the launcher, settings, the
 *   keyboard, the JS bindings, main.cpp -- do it for nothing but the TFT_* colour macros and the
 *   *_DATUM constants, because the include used to be the cheap way to get them. The other seven
 *   genuinely need TFT_eSPI and belong to boards that have it.
 *
 *   This file is on the S31 build's include path and nowhere else, so those fourteen files compile
 *   unmodified: they still say `#include <TFT_eSPI.h>` and get this. Rewriting their includes, or
 *   seeding a project-owned colours header through forty files, would have been a far larger change
 *   for zero behavioural difference -- and the whole point of this port is that it stays a port.
 *
 * WHAT IT IS NOT
 *   It is not TFT_eSPI and must not grow into it. There is no display bus here, no pin handling and
 *   no SPI: on this board the panel is an RGB parallel panel driven by esp_lcd through Espressif's
 *   BSP, and `tft` is a KryonDisplay. The only two types below exist so that code written against
 *   TFT_eSPI's *shapes* -- `tft->nativeTft()` returning a TFT_eSPI*, `new TFT_eSprite(native)` --
 *   still compiles and still works, backed by the KryonSprite that does the real drawing.
 *
 *   Boards that use TFT_eSPI never see this header, so there is no risk of it shadowing the real
 *   library for them.
 */

#include <stdint.h>

#include "KryonSprite.h"

// --- Colours -------------------------------------------------------------------------------------
// The same RGB565 values TFT_eSPI defines, so a colour is the same colour on every board. Only the
// names KryonOS actually uses are here; adding more is a one-line change, but an unused macro is a
// claim this shim is more complete than it is.
#define TFT_BLACK       0x0000
#define TFT_NAVY        0x000F
#define TFT_DARKGREY    0x7BEF
#define TFT_BLUE        0x001F
#define TFT_GREEN       0x07E0
#define TFT_CYAN        0x07FF
#define TFT_RED         0xF800
#define TFT_MAGENTA     0xF81F
#define TFT_YELLOW      0xFFE0
#define TFT_WHITE       0xFFFF
#define TFT_ORANGE      0xFDA0
#define TFT_LIGHTGREY   0xD69A
#define TFT_PURPLE      0x780F
#define TFT_GOLD        0xFEA0

// --- Text datums ---------------------------------------------------------------------------------
// Values match TFT_eSPI's, and kryon_text uses the same numbering, so a datum passed to one backend
// means the same thing on another.
#define TL_DATUM     0
#define TC_DATUM     1
#define TR_DATUM     2
#define ML_DATUM     3
#define CL_DATUM     3  // TFT_eSPI alias
#define MC_DATUM     4
#define CC_DATUM     4  // TFT_eSPI alias
#define MR_DATUM     5
#define CR_DATUM     5  // TFT_eSPI alias
#define BL_DATUM     6
#define BC_DATUM     7
#define BR_DATUM     8
#define L_BASELINE   9
#define C_BASELINE  10
#define R_BASELINE  11

/**
 * Stands in for TFT_eSPI itself.
 *
 * KryonDisplay::nativeTft() returns one of these, and non-null is the signal the JS bindings use to
 * decide whether sprites are available at all -- so it cannot simply be a null-returning stub. It
 * carries the display it stands for, which is what lets a sprite pushed onto it reach the panel.
 */
class TFT_eSPI {
public:
    TFT_eSPI() = default;
    explicit TFT_eSPI(KryonDisplay* display) : display_(display) {}

    /** The KryonDisplay this stands in for, or nullptr if it was default-constructed. */
    KryonDisplay* display() const { return display_; }
    void setDisplay(KryonDisplay* display) { display_ = display; }

private:
    KryonDisplay* display_ = nullptr;
};

/**
 * The sprite, with TFT_eSPI's name and constructor shape.
 *
 * `new TFT_eSprite(native)` is how the JS bindings allocate one, so the constructor takes the
 * TFT_eSPI* and resolves it to the display to push onto. An unknown or null parent leaves the target
 * unset, and pushSprite() then does nothing rather than crashing -- the same degradation as the old
 * "no nativeTft, no sprites" path, but with the sprite still usable as a scratch surface.
 */
class TFT_eSprite : public KryonSprite {
public:
    TFT_eSprite() = default;
    explicit TFT_eSprite(TFT_eSPI* parent) { setTarget(parent ? parent->display() : nullptr); }
};

#endif // KRYON_TFT_ESPI_SHIM_H
