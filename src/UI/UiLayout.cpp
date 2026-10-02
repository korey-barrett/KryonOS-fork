#include "UiLayout.h"

#include "Hal/Display/Display.h"
#include "Hal/Display/DisplayConfig.h"

// ---------------------------------------------------------------------------------------------
// Derivation. Every constant below was chosen so that compute(240, 320) reproduces the historical
// hard-coded layout *exactly* (see the guarantee in UiLayout.h and tools/preview/test_layout.py).
// Integer arithmetic is deliberate: the preview model mirrors it bit-for-bit.
// ---------------------------------------------------------------------------------------------
UiMetrics UiLayout::compute(int16_t w, int16_t h) {
    UiMetrics m = {};
    m.w = w;
    m.h = h;
    m.landscape = (w > h);

    const int16_t mind = (w < h) ? w : h;

    // Screen-edge inset: 3 at 240x320, never below 2 or above 8.
    int16_t inset = mind / 80;
    if (inset < 2) inset = 2;
    if (inset > 8) inset = 8;
    m.inset = (uint8_t)inset;

    m.frame  = { inset, inset, (int16_t)(w - 2 * inset), (int16_t)(h - 2 * inset) };
    m.header = { (int16_t)(inset + 3), (int16_t)(inset + 3), (int16_t)(w - 2 * (inset + 3)), 30 };
    m.headerTextY = (int16_t)(m.header.y + 15);

    m.footer      = { (int16_t)(inset + 2), (int16_t)(h - 35), (int16_t)(w - 2 * (inset + 2)), 30 };
    m.footerTextY = (int16_t)(m.footer.y + 15);

    m.list = { (int16_t)(inset + 7), (int16_t)(inset + 42), (int16_t)(w - 2 * (inset + 7)),
               (int16_t)(m.footer.y - (inset + 42) - 10) };
    if (m.list.h < 0) m.list.h = 0;

    // Row height is 30 on a normal panel. A short one cannot afford that — 240x135 would show a
    // single row — so shrink toward a floor, keeping at least ~4 rows on screen. rowFillH stays 5px
    // shorter than the row so the highlight keeps its inset.
    m.rowH = 30;
    if (m.list.h < 4 * 30) {
        m.rowH = (int16_t)(m.list.h / 4);
        if (m.rowH < 14) m.rowH = 14;
    }
    m.rowFillH    = (int16_t)(m.rowH - 5);
    m.rowTextPadX = (int16_t)(inset + 2);

    m.itemsPerPage = (m.rowH > 0) ? (int16_t)(m.list.h / m.rowH) : 0;
    if (m.itemsPerPage < 1) m.itemsPerPage = 1;

    m.scrollX        = (int16_t)(w - 8);
    m.scrollW        = 3;
    m.scrollThumbMin = 20;

    m.centerX = (int16_t)(w / 2);
    m.centerY = (int16_t)(h / 2);

    m.appExitButton = { (int16_t)(w - 40), 0, 40, 30 };
    m.progressBar   = { (int16_t)(w / 8), m.centerY, (int16_t)(w - w / 4), 20 };
    // The historical y=100 sits 55px below list.y (45) on a 240x320 canvas. Anchoring it to the
    // list instead of the screen keeps it inside the list on a short panel, where y=100 would land
    // on the footer.
    m.listMessageY  = (int16_t)(m.list.y + 55);

    m.dialogButtonRowY = (int16_t)(h - 90);
    m.dialogButtonGap  = (int16_t)(w / 8);

    m.fontSmall  = 1;
    // Body text must fit inside a row: a short panel drops to the 8px cell, because the larger
    // font would not fit a 14px row. The header keeps its own size — it has a full-width 30px bar.
    m.fontBody   = (h >= 200) ? 2 : 1;
    m.fontHeader = (mind >= 300) ? 4 : 2;

    // Notification card: 8px gutters, 42px tall on normal panels, thinner on short ones.
    m.cardX     = 8;
    m.cardW     = (int16_t)(w - 16);
    m.cardH     = (h >= 240) ? 42 : 30;
    m.cardR     = 6;
    m.restingY  = 8;
    m.hiddenY   = (int16_t)(-(m.cardH + 2));
    m.shadowW   = w;
    m.shadowH   = (int16_t)(m.cardH + 22);

    // On-screen keyboard (12x4 QWERTY grid under a 5-button row).
    m.kbPromptX     = 5;
    m.kbButtonCount = 5;
    m.kbCols        = 12;
    m.kbRows        = 4;
    m.kbKeyW        = (int16_t)(w / m.kbCols);
    m.kbButtonW     = (int16_t)(w / m.kbButtonCount);

    // A full-height panel keeps the historical chrome: prompt at y10, a 30px text box at y30, the
    // 5-button row at y70, and the grid from y110. A short panel cannot afford that — 135px minus
    // 110px of chrome would leave 6px keys — so the chrome compresses and the grid takes the rest.
    if (h < 240) {
        m.kbPromptY   = 4;
        m.kbTextBox   = { 5, 20, (int16_t)(w - 10), 20 };
        m.kbButtonRow = { 0, 44, w, 24 };
        m.kbGridTop   = 72;
    } else {
        m.kbPromptY   = 10;
        m.kbTextBox   = { 5, 30, (int16_t)(w - 10), 30 };
        m.kbButtonRow = { 0, 70, w, 30 };
        m.kbGridTop   = 110;
    }

    // Hold out for a legible key height rather than shrinking the keys to nothing; if holding out
    // costs a row, the pager strip pays for it. Only a panel shorter than ~120px ever pages.
    const int16_t kbMinKeyH = (m.fontBody >= 2) ? 16 : 12;
    const int16_t kbPagerH  = (int16_t)(m.fontBody * 10 + 2);
    int rows = (h - m.kbGridTop) / kbMinKeyH;
    m.kbPagerH = 0;
    if (rows < m.kbRows) {
        const int reserved = (h - m.kbGridTop - kbPagerH) / kbMinKeyH;
        if (reserved < m.kbRows) { rows = reserved; m.kbPagerH = kbPagerH; }
    }
    if (rows < 1) rows = 1;
    if (rows > m.kbRows) rows = m.kbRows;
    m.kbRowsPerPage = (int16_t)rows;
    m.kbPages       = (int16_t)((m.kbRows + rows - 1) / rows);
    m.kbKeyH        = (int16_t)((h - m.kbGridTop - m.kbPagerH) / rows);
    if (m.kbKeyH < 1) m.kbKeyH = 1;
    if (m.kbKeyW < 1) m.kbKeyW = 1;

    if (m.kbPagerH > 0) {
        m.kbPagerStrip = { 0, (int16_t)(m.kbGridTop + rows * m.kbKeyH), w, m.kbPagerH };
    } else {
        m.kbPagerStrip = { 0, 0, 0, 0 };
    }

    return m;
}

// --- list helpers ----------------------------------------------------------------------------
UiRect UiMetrics::listRowRect(int visibleIndex) const {
    return { list.x, (int16_t)(list.y + visibleIndex * rowH), list.w, rowH };
}

UiRect UiMetrics::listRowFillRect(int visibleIndex) const {
    return { list.x, (int16_t)(list.y + visibleIndex * rowH), list.w, rowFillH };
}

int16_t UiMetrics::listRowTextY(int visibleIndex) const {
    // Historical: row.y + 12 for a 30px row — centred, less half a descender allowance.
    return (int16_t)(list.y + visibleIndex * rowH + (rowH - 6) / 2);
}

int UiMetrics::listRowFromY(int16_t y) const {
    if (!inList(y)) return -1;
    int idx = (y - list.y) / rowH;
    if (idx < 0) idx = 0;
    if (idx >= itemsPerPage) idx = itemsPerPage - 1;
    return idx;
}

bool UiMetrics::inList(int16_t y) const {
    return y >= list.y && y < list.y + list.h;
}

bool UiMetrics::inFooter(int16_t y) const {
    // Inclusive of the bottom edge, matching the historical `y >= 285 && y <= 315`.
    return y >= footer.y && y <= footer.y + footer.h;
}

// --- dialog helpers ----------------------------------------------------------------------------
UiRect UiMetrics::dialogPanel(int16_t height) const {
    if (height < 1) height = 1;

    // The panel is capped to the room above the footer, so a dialog can never be asked for more
    // space than the panel has. At 240x320 that ceiling is 270px and every historical height
    // (160 / 200 / 240) passes through untouched.
    const int16_t bottom = (int16_t)(footer.y - 15);
    if (height > bottom) height = bottom;

    int16_t y = (int16_t)(centerY - height / 2);
    if (y + height > bottom) y = (int16_t)(bottom - height);
    if (y < 0) y = 0;
    return { list.x, y, list.w, height };
}

UiRect UiMetrics::dialogPanelTop(int16_t height) const {
    if (height < 1) height = 1;
    const int16_t bottom = (int16_t)(footer.y - 15);
    if (height > bottom) height = bottom;
    int16_t y = (int16_t)(header.bottom() + 4);
    if (y + height > bottom) y = (int16_t)(bottom - height);
    if (y < 0) y = 0;
    return { list.x, y, list.w, height };
}

UiRect UiMetrics::dialogButtonSpaced(int16_t y, int16_t h, int index, int count, int16_t buttonW,
                                     int16_t gap) const {
    if (count < 1 || index < 0 || index >= count) return { 0, 0, 0, 0 };
    const int16_t total = (int16_t)(count * buttonW + (count - 1) * gap);
    const int16_t x0    = (int16_t)((w - total) / 2);
    return { (int16_t)(x0 + index * (buttonW + gap)), y, buttonW, h };
}

UiRect UiMetrics::dialogButton(int16_t y, int16_t h, int index, int count, int16_t buttonW) const {
    return dialogButtonSpaced(y, h, index, count, buttonW, dialogButtonGap);
}

// --- footer helpers --------------------------------------------------------------------------
UiRect UiMetrics::footerButton(int which) const {
    const int16_t q = (int16_t)(w / 4);
    switch (which) {
        case UI_FOOTER_UP:  return { 0, footer.y, q, footer.h };
        case UI_FOOTER_SEL: return { q, footer.y, (int16_t)(w / 2), footer.h };
        case UI_FOOTER_DN:  return { (int16_t)(3 * q), footer.y, (int16_t)(w - 3 * q), footer.h };
        default:            return { 0, 0, 0, 0 };
    }
}

int16_t UiMetrics::footerButtonCenterX(int which) const {
    return footerButton(which).cx();
}

int UiMetrics::footerButtonFromX(int16_t x) const {
    if (x < (int16_t)(w / 4))      return UI_FOOTER_UP;
    if (x < (int16_t)(3 * (w / 4))) return UI_FOOTER_SEL;
    return UI_FOOTER_DN;
}

// --- four-zone footer (BACK / UP / SEL / DN) ---------------------------------------------------
// The zone edges scale with the width, so at 240 they land on the historical 70 / 130 / 200 and the
// label centres come out as 35 / 100 / 165 / 220 — exactly where the App Store used to hard-code
// them. Drawing and hit-testing both read these boundaries, so they cannot drift apart.
namespace {
int16_t slotEdge(int16_t w, int numerator) {
    return (int16_t)((int32_t)w * numerator / 240);
}
} // namespace

UiRect UiMetrics::footerSlot(int which) const {
    const int16_t b1 = slotEdge(w, 70);
    const int16_t b2 = slotEdge(w, 130);
    const int16_t b3 = slotEdge(w, 200);
    switch (which) {
        case UI_SLOT_BACK: return { 0,  footer.y, b1, footer.h };
        case UI_SLOT_UP:   return { b1, footer.y, (int16_t)(b2 - b1), footer.h };
        case UI_SLOT_SEL:  return { b2, footer.y, (int16_t)(b3 - b2), footer.h };
        case UI_SLOT_DN:   return { b3, footer.y, (int16_t)(w - b3), footer.h };
        default:           return { 0, 0, 0, 0 };
    }
}

int16_t UiMetrics::footerSlotCenterX(int which) const {
    return footerSlot(which).cx();
}

int UiMetrics::footerSlotFromX(int16_t x) const {
    if (x < slotEdge(w, 70))  return UI_SLOT_BACK;
    if (x < slotEdge(w, 130)) return UI_SLOT_UP;
    if (x < slotEdge(w, 200)) return UI_SLOT_SEL;
    return UI_SLOT_DN;
}

// --- keyboard helpers --------------------------------------------------------------------------
UiRect UiMetrics::kbKeyRect(int row, int col, int page) const {
    if (row < 0 || row >= kbRows || col < 0 || col >= kbCols) return { 0, 0, 0, 0 };
    const int local = row - page * kbRowsPerPage;
    if (local < 0 || local >= kbRowsPerPage) return { 0, 0, 0, 0 };
    return { (int16_t)(col * kbKeyW), (int16_t)(kbGridTop + local * kbKeyH), kbKeyW, kbKeyH };
}

int UiMetrics::kbRowFromY(int16_t y, int page) const {
    if (kbKeyH <= 0 || y < kbGridTop) return -1;
    const int local = (y - kbGridTop) / kbKeyH;
    if (local < 0 || local >= kbRowsPerPage) return -1;
    const int row = page * kbRowsPerPage + local;
    return (row < kbRows) ? row : -1;
}

UiRect UiMetrics::kbPagerPrev() const {
    if (kbPagerH <= 0) return { 0, 0, 0, 0 };
    return { kbPagerStrip.x, kbPagerStrip.y, (int16_t)(kbPagerStrip.w / 3), kbPagerStrip.h };
}

UiRect UiMetrics::kbPagerNext() const {
    if (kbPagerH <= 0) return { 0, 0, 0, 0 };
    const int16_t third = (int16_t)(kbPagerStrip.w / 3);
    return { (int16_t)(kbPagerStrip.right() - third), kbPagerStrip.y, third, kbPagerStrip.h };
}

// --- binding ---------------------------------------------------------------------------------
namespace {
UiMetrics g_current;
bool      g_bound = false;
}

void UiLayout::begin(int16_t w, int16_t h) {
    g_current = compute(w, h);
    g_bound = true;
}

void UiLayout::begin() {
    begin(Display::width(), Display::height());
}

const UiMetrics& UiLayout::current() {
    if (!g_bound) {
        begin(Display::width(), Display::height());
    }
    return g_current;
}
