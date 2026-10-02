#ifndef KRYONOS_UI_LAYOUT_H
#define KRYONOS_UI_LAYOUT_H

// ---------------------------------------------------------------------------------------------
// UiLayout — one source of truth for on-device screen geometry.
//
// Every screen used to hard-code 240x320 coordinates and duplicate them again in its touch
// handler. UiLayout derives the regions from the actual canvas size so the same code adapts to any
// resolution, and so drawing and hit-testing can share the exact same rectangles.
//
// This header is intentionally Arduino-free (only <stdint.h>) so the host-side preview tool
// (tools/preview/layout_model.py mirrors these formulas) and any native simulator can use it.
//
// GUARANTEE: compute(240, 320) reproduces the historical constants exactly
//   frame (3,3,234,314) · header (6,6,228,30) · list (10,45,220,230) · footer (5,285,230,30)
//   center (120,160) · rowH 30 · rowFillH 25 · rowTextPadX 5 · listRowTextY row.y+12
//   itemsPerPage 7 · scrollbar x 232 · app-exit (200,0,40,30)
//   progress bar (30,160,180,20) · dialog row y 230 · dialog gap 30
//   notification card (8,·,224,42, r6) shadow 240x64 hiddenY -44 · keyboard grid 12x4 @ y110
// Two footer conventions live here, both pinned to their historical 240x320 geometry:
//   footerButton* — even thirds (UP/SEL/DN), used by the Launcher, Installer, Settings, WebServer
//   footerSlot*   — uneven 70/130/200 zones (BACK/UP/SEL/DN), used by the App Store; label centres
//                   land on 35/100/165/220
// tools/preview/test_layout.py asserts all of this against layout_model.py.
// ---------------------------------------------------------------------------------------------

#include <stdint.h>

struct UiRect {
    int16_t x, y, w, h;

    int16_t right()  const { return (int16_t)(x + w); }
    int16_t bottom() const { return (int16_t)(y + h); }
    int16_t cx()     const { return (int16_t)(x + w / 2); }
    int16_t cy()     const { return (int16_t)(y + h / 2); }

    bool contains(int16_t px, int16_t py) const {
        return px >= x && px < x + w && py >= y && py < y + h;
    }
};

// Footer button slots (shared by every screen that draws the UP / SEL / DN bar).
enum UiFooterButton { UI_FOOTER_UP = 0, UI_FOOTER_SEL = 1, UI_FOOTER_DN = 2, UI_FOOTER_NONE = -1 };

// Footer slots for the four-zone BACK / UP / SEL / DN bar (App Store). Unlike UiFooterButton these
// are *not* even thirds: the zones keep the historical 70 / 130 / 200 boundaries, so the drawn
// labels and the touch zones are derived from one place instead of being spelled out twice.
enum UiFooterSlot { UI_SLOT_BACK = 0, UI_SLOT_UP = 1, UI_SLOT_SEL = 2, UI_SLOT_DN = 3, UI_SLOT_NONE = -1 };

struct UiMetrics {
    int16_t w, h;
    bool    landscape;
    uint8_t inset;

    UiRect  frame;
    UiRect  header;
    int16_t headerTextY;

    UiRect  list;
    int16_t rowH;          // vertical step between rows (shrinks on short panels)
    int16_t rowFillH;      // height of a highlighted row fill
    int16_t rowTextPadX;   // left inset for a row's label
    int16_t itemsPerPage;

    UiRect  footer;
    int16_t footerTextY;

    int16_t scrollX, scrollW, scrollThumbMin;

    int16_t centerX, centerY;

    UiRect  appExitButton; // the red "X" an app draws top-right
    UiRect  progressBar;   // centred 3/4-width progress track
    int16_t listMessageY;  // the "no items" / loading line inside the list area

    // Dialog button row (App Store / Installer / Settings modals), anchored 90px above the bottom
    // edge — y=230 on a 320-tall screen. A dialog that needs a second row uses dialogButtonRowY - 10.
    int16_t dialogButtonRowY;
    int16_t dialogButtonGap;

    uint8_t fontSmall, fontBody, fontHeader;

    // Notification card.
    int16_t cardX, cardW, cardH, cardR, restingY, hiddenY, shadowW, shadowH;

    // On-screen keyboard.
    int16_t kbPromptX, kbPromptY;
    UiRect  kbTextBox;
    UiRect  kbButtonRow;
    int16_t kbButtonCount, kbButtonW;
    int16_t kbGridTop;
    int16_t kbCols, kbRows, kbKeyW, kbKeyH;

    // Paging. A very short panel cannot fit all kbRows at a legible key height, so the grid shows
    // kbRowsPerPage rows at a time and a pager strip appears beneath it. When the panel is tall
    // enough (all of the reference resolutions) kbRowsPerPage == kbRows, kbPages == 1, kbPagerH is
    // 0 and kbPagerStrip is empty, so no paging UI is drawn at all.
    int16_t kbRowsPerPage, kbPages, kbPagerH;
    UiRect  kbPagerStrip;

    // Key cell for an absolute grid row (0..kbRows-1) and column on the given page. Empty when the
    // row is not on that page — so draw and hit-test can both walk it without a bounds dance.
    UiRect  kbKeyRect(int row, int col, int page) const;
    // Absolute grid row under a touch y on the given page, or -1 if outside the key grid.
    int     kbRowFromY(int16_t y, int page) const;
    // The pager's two hit zones (empty rectangles when nothing is paged).
    UiRect  kbPagerPrev() const;
    UiRect  kbPagerNext() const;

    // --- list helpers (use these in BOTH draw() and handleTouch()) ---
    UiRect  listRowRect(int visibleIndex) const;      // full row cell (rowH tall)
    UiRect  listRowFillRect(int visibleIndex) const;  // highlighted fill (rowFillH tall)
    int16_t listRowTextY(int visibleIndex) const;     // ML_DATUM baseline inside a row
    int     listRowFromY(int16_t y) const;            // visible index, or -1 if outside the list
    bool    inList(int16_t y) const;
    bool    inFooter(int16_t y) const;

    // --- dialog helpers ---
    // A modal panel: the same x/width as the list, `height` tall, vertically centred and then
    // clamped into the safe area between the header and the footer. dialogPanel(160) is
    // (10, 80, 220, 160) at 240x320; dialogPanel(240) clamps to (10, 30, 220, 240).
    UiRect  dialogPanel(int16_t height) const;

    // A modal panel anchored just below the header instead of centred — the Installer's file-action
    // chooser. dialogPanelTop(160) is (10, 40, 220, 160) at 240x320.
    UiRect  dialogPanelTop(int16_t height) const;

    // `count` buttons of width buttonW, separated by dialogButtonGap, centred on the canvas:
    // dialogButton(rowY, 30, 0, 2, 80) == {25, rowY, 80, 30} on a 240px-wide screen.
    UiRect  dialogButton(int16_t y, int16_t h, int index, int count, int16_t buttonW) const;

    // As dialogButton, but with an explicit gap — the Installer's dialogs use 40 and 10 where the
    // App Store and Settings use the default 30.
    UiRect  dialogButtonSpaced(int16_t y, int16_t h, int index, int count, int16_t buttonW,
                               int16_t gap) const;

    // --- footer helpers ---
    UiRect  footerButton(int which) const;            // UI_FOOTER_*; thirds of the screen width
    int16_t footerButtonCenterX(int which) const;
    int     footerButtonFromX(int16_t x) const;       // UI_FOOTER_* or UI_FOOTER_NONE

    // --- four-zone footer helpers (UI_SLOT_*; BACK / UP / SEL / DN) ---
    UiRect  footerSlot(int which) const;
    int16_t footerSlotCenterX(int which) const;       // where the label is drawn
    int     footerSlotFromX(int16_t x) const;         // UI_SLOT_* or UI_SLOT_NONE
};

namespace UiLayout {

// Pure: derives metrics for an arbitrary canvas. Safe to call from host code.
UiMetrics compute(int16_t w, int16_t h);

// Binds the metrics for the running device. begin(w,h) is explicit; begin() uses the Display
// geometry (see Hal/Display). Screens read current().
void begin(int16_t w, int16_t h);
void begin();
const UiMetrics& current();

} // namespace UiLayout

#endif // KRYONOS_UI_LAYOUT_H
