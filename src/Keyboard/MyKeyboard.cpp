#include "MyKeyboard.h"
#include "../Settings/TouchDriver.h"
#include "../UI/UiLayout.h"

KryonDisplay *MyKeyboard::tftInstance = nullptr;

// Screen metrics. At 240x320 the keyboard metrics reproduce the historical literals exactly:
// text box (5,30,230,30), buttons at y70..100, key grid from y110. The grid's column and row counts
// come from the board (KRYONOS_KB_* in UiLayout.h), so the shared board gets 12x4 keys of 20x52 and
// the Waveshare's capacitive panel gets 6x6 of 40x35. A short panel compresses that chrome and, if
// even then the keys would be too small to read, shows the grid a page of rows at a time (see the
// keyboard block in UiLayout::compute).
static inline const UiMetrics& M() { return UiLayout::current(); }

// The key table. One shape on every board -- [charPage][row][col][unshifted, shifted] -- so the grid
// loop and the touch handler have a single indexing path instead of one per board. A board with a
// single character set declares KRYONOS_KB_CHAR_PAGES 1 and its page index is always 0.
//
// The dimensions are the SAME macros UiLayout sizes the grid from, so the table cannot end up a
// different shape from the rectangles it is drawn into.
static const int kw = KRYONOS_KB_COLS; // keyboard width  (columns)
static const int kh = KRYONOS_KB_ROWS; // keyboard height (rows)
char qwerty_keyset[KRYONOS_KB_CHAR_PAGES][kh][kw][2] = {
#if KRYONOS_KB_CHAR_PAGES > 1
    // Page 0 -- letters, then digits. 26 + 10 fills 6x6 exactly with nothing to spare, which also
    // means a WiFi password, the common case, never needs a page switch.
    {
        {{'q', 'Q'}, {'w', 'W'}, {'e', 'E'}, {'r', 'R'}, {'t', 'T'}, {'y', 'Y'}},
        {{'u', 'U'}, {'i', 'I'}, {'o', 'O'}, {'p', 'P'}, {'a', 'A'}, {'s', 'S'}},
        {{'d', 'D'}, {'f', 'F'}, {'g', 'G'}, {'h', 'H'}, {'j', 'J'}, {'k', 'K'}},
        {{'l', 'L'}, {'m', 'M'}, {'n', 'N'}, {'b', 'B'}, {'v', 'V'}, {'c', 'C'}},
        {{'x', 'X'}, {'z', 'Z'}, {'1', '1'}, {'2', '2'}, {'3', '3'}, {'4', '4'}},
        {{'5', '5'}, {'6', '6'}, {'7', '7'}, {'8', '8'}, {'9', '9'}, {'0', '0'}},
    },
    // Page 1 -- symbols. The four blank cells at the end are deliberate: the ASCII symbol set is 32
    // characters and the grid holds 36. They must draw as empty keys and type NOTHING -- appending a
    // NUL to a password is a real failure, not a cosmetic one.
    {
        {{'!', '!'}, {'@', '@'}, {'#', '#'}, {'$', '$'}, {'%', '%'}, {'^', '^'}},
        {{'&', '&'}, {'*', '*'}, {'(', '('}, {')', ')'}, {'-', '-'}, {'_', '_'}},
        {{'=', '='}, {'+', '+'}, {'[', '['}, {']', ']'}, {'{', '{'}, {'}', '}'}},
        {{';', ';'}, {':', ':'}, {'\'', '\''}, {'"', '"'}, {',', ','}, {'.', '.'}},
        {{'<', '<'}, {'>', '>'}, {'/', '/'}, {'?', '?'}, {'\\', '\\'}, {'|', '|'}},
        {{'~', '~'}, {'`', '`'}, {'\0', '\0'}, {'\0', '\0'}, {'\0', '\0'}, {'\0', '\0'}},
    },
#else
    {
        {{'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'}, {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'}, {'-', '_'}, {'=', '+'}},
        {{'q', 'Q'}, {'w', 'W'}, {'e', 'E'}, {'r', 'R'}, {'t', 'T'}, {'y', 'Y'}, {'u', 'U'}, {'i', 'I'}, {'o', 'O'}, {'p', 'P'}, {'[', '{'}, {']', '}'}},
        {{'a', 'A'}, {'s', 'S'}, {'d', 'D'}, {'f', 'F'}, {'g', 'G'}, {'h', 'H'}, {'j', 'J'}, {'k', 'K'}, {'l', 'L'}, {';', ':'}, {'\'', '"'}, {'\\', '|'}},
        {{'\\', '|'}, {'z', 'Z'}, {'x', 'X'}, {'c', 'C'}, {'v', 'V'}, {'b', 'B'}, {'n', 'N'}, {'m', 'M'}, {',', '<'}, {'.', '>'}, {'/', '?'}, {' ', ' '}}
    },
#endif
};

void MyKeyboard::init(KryonDisplay *tft) {
    tftInstance = tft;
}

String MyKeyboard::getString(String initialText, String promptMsg, int maxLen) {
    if (!tftInstance) return "";
    (void)maxLen; // the text box clips by width; there is no length cap to enforce here

    String currentText = initialText;
    bool caps = false;
    bool done = false;
    int page = 0;     // row pager: which page of ROWS the grid is showing (always 0 when they all fit)
    int charPage = 0; // character set: letters+digits, or symbols (always 0 on a single-page board)

    // Draw initial state.
    //
    // present() is not optional here. This function blocks, and the thing that normally flushes a
    // frame is the main loop -- the same loop that is now sitting inside this call. On a
    // write-through backend present() is a no-op and the draws above are already on the panel; on a
    // canvas backend it is the only thing that copies the canvas to the panel, so without it the
    // keyboard is invisible and the whole screen looks frozen.
    drawKeyboard(currentText, promptMsg, caps, -1, -1, page, charPage);
    tftInstance->present();

    while (!done) {
        uint16_t x, y;
        if (TouchDriver::getTouch(&x, &y)) {
            handleTouch(x, y, currentText, caps, done, page, charPage);
            if (!done) {
                drawKeyboard(currentText, promptMsg, caps, -1, -1, page, charPage);
                tftInstance->present();
            }

            // Wait for the finger to lift before accepting another key.
            //
            // A fixed debounce delay is not enough here, and this is the difference between a tap
            // typing one character and typing two. The main loop is edge-triggered -- it acts on a
            // press only at the rising edge (see the wasTouched flag in main.cpp) -- but this
            // function reads the panel directly, and on an absolute-position controller a contact
            // reads as down for as long as it lasts. On the S31 the gap between one read and the
            // next is not the 200ms delay but that delay plus the canvas blit inside present()
            // (768KB over the RGB bus), which together outlast an ordinary tap: the tail of the
            // press is then read as a second press and types the character again. Requiring the
            // release is what actually makes one tap one key, on every backend and at any frame
            // cost. Bounded so a panel stuck reporting contact cannot wedge the keyboard.
            const uint32_t pressStartedAt = millis();
            uint16_t rx, ry;
            while (TouchDriver::getTouch(&rx, &ry) && (uint32_t)(millis() - pressStartedAt) < 1000) {
                delay(20);
            }
        }
        delay(10);
    }

    // Drain touch buffer & wait for physical touch release so touches do not bleed through
    uint16_t rx, ry;
    while (TouchDriver::getTouch(&rx, &ry)) {
        delay(20);
    }
    delay(80);

    return currentText;
}

void MyKeyboard::drawKeyboard(String currentText, String promptMsg, bool caps, int selectedX, int selectedY, int page, int charPage) {
    const UiMetrics& m = M();
    (void)selectedX; (void)selectedY; // reserved: highlight a key under a stylus, unused so far

    tftInstance->fillScreen(TFT_BLACK);

    // Prompt
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(TL_DATUM);
    tftInstance->drawString(promptMsg, m.kbPromptX, m.kbPromptY, m.fontBody);

    // Text box
    tftInstance->drawRect(m.kbTextBox.x, m.kbTextBox.y, m.kbTextBox.w, m.kbTextBox.h, TFT_GREEN);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->drawString(currentText + "_", (int16_t)(m.kbTextBox.x + 5 * m.scale),
                            (int16_t)(m.kbTextBox.y + 8 * m.scale), m.fontBody);

    // Top Row Buttons. The count and the width come from the metrics, so the drawn cells and the
    // hit zones in handleTouch() are the same rectangles by construction.
    //
    // The label sets differ because they have to: a 6-button row is 240/6 = 40px wide, which will
    // not hold "SPACE" (~45px in font 2, so it would run over its neighbour), hence "SPC". "CAPS"
    // fits at ~38px. On those boards the CAPS button shows its state in COLOUR rather than in its
    // label, since the SYM/ABC button already owns the label-as-state slot.
#if KRYONOS_KB_CHAR_PAGES > 1
    const char* btns[] = {"OK", "CAPS", "DEL", "SPC", charPage == 0 ? "SYM" : "ABC", "ESC"};
#else
    const char* btns[] = {"OK", caps ? "abc" : "ABC", "DEL", "SPACE", "ESC"};
#endif
    static_assert(static_cast<int>(sizeof(btns) / sizeof(btns[0])) == KRYONOS_KB_BUTTONS,
                  "the button label row must match KRYONOS_KB_BUTTONS");
    for (int i = 0; i < m.kbButtonCount; i++) {
        tftInstance->drawRect(i * m.kbButtonW, m.kbButtonRow.y, m.kbButtonW, m.kbButtonRow.h, TFT_GREEN);
#if KRYONOS_KB_CHAR_PAGES > 1
        tftInstance->setTextColor((i == 1 && caps) ? TFT_GREEN : TFT_WHITE, TFT_BLACK);
#else
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
#endif
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString(btns[i], i * m.kbButtonW + m.kbButtonW / 2, m.kbButtonRow.cy(), m.fontBody);
    }

    // Keyboard Grid -- only the rows on the current ROW page have a cell, so an off-page row is
    // skipped. kbRowsPerPage == kbRows on every board whose grid fits, which is where kbPages == 1
    // and this loop walks all of it.
    for (int row = 0; row < kh; row++) {
        for (int col = 0; col < kw; col++) {
            const UiRect key = m.kbKeyRect(row, col, page);
            if (key.w == 0) continue;

            tftInstance->drawRect(key.x, key.y, key.w, key.h, TFT_DARKGREY);

            // A blank cell (symbols page only) draws as an empty key rather than printing a NUL.
            char c = qwerty_keyset[charPage][row][col][caps ? 1 : 0];
            if (c) {
                tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
                tftInstance->setTextDatum(MC_DATUM);
                tftInstance->drawString(String(c), key.cx(), key.cy(), m.fontBody);
            }
        }
    }

    // Pager strip -- only drawn when the panel is too short for all rows at a legible size. This is
    // row paging, not the character page: see the note on kbCharPages in UiLayout.h.
    if (m.kbPagerH > 0) {
        const UiRect prev = m.kbPagerPrev();
        const UiRect next = m.kbPagerNext();
        tftInstance->drawRect(m.kbPagerStrip.x, m.kbPagerStrip.y, m.kbPagerStrip.w, m.kbPagerStrip.h,
                              TFT_GREEN);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->drawString("PREV", prev.cx(), prev.cy(), m.fontBody);
        tftInstance->drawString("NEXT", next.cx(), next.cy(), m.fontBody);
        tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
        tftInstance->drawString(String(page + 1) + "/" + String(m.kbPages), m.kbPagerStrip.cx(),
                                m.kbPagerStrip.cy(), m.fontBody);
    }
}

void MyKeyboard::handleTouch(uint16_t x, uint16_t y, String &currentText, bool &caps, bool &done, int &page, int &charPage) {
    const UiMetrics& m = M();

    // Pager strip -- checked first because it sits below the grid's last row.
    if (m.kbPagerH > 0 && m.kbPagerStrip.contains((int16_t)x, (int16_t)y)) {
        if (m.kbPagerPrev().contains((int16_t)x, (int16_t)y)) {
            page = (page + m.kbPages - 1) % m.kbPages;
        } else if (m.kbPagerNext().contains((int16_t)x, (int16_t)y)) {
            page = (page + 1) % m.kbPages;
        }
        return;
    }

    // Top Row Buttons -- same cells the drawing loop used, walked by index so the two cannot drift.
    if (m.kbButtonRow.contains((int16_t)x, (int16_t)y)) {
        const int index = (int)x / m.kbButtonW;
        if (index < 0 || index >= m.kbButtonCount) return;

        // Index order matches the label row above: OK, CAPS, DEL, then the two board-specific
        // entries, then ESC.
#if KRYONOS_KB_CHAR_PAGES > 1
        switch (index) {
            case 0: done = true; break;                                              // OK
            case 1: caps = !caps; break;                                             // CAPS
            case 2: if (currentText.length() > 0) currentText.remove(currentText.length() - 1); break; // DEL
            case 3: currentText += " "; break;                                       // SPC
            case 4: charPage = (charPage + 1) % KRYONOS_KB_CHAR_PAGES; break;         // SYM / ABC
            default: currentText = ""; done = true; break;                           // ESC
        }
#else
        switch (index) {
            case 0: done = true; break;                                              // OK
            case 1: caps = !caps; break;                                             // CAPS
            case 2: if (currentText.length() > 0) currentText.remove(currentText.length() - 1); break; // DEL
            case 3: currentText += " "; break;                                       // SPACE
            default: currentText = ""; done = true; break;                           // ESC
        }
#endif
        return;
    }

    // Keyboard Grid
    const int row = m.kbRowFromY((int16_t)y, page);
    if (row >= 0) {
        const int col = ((int)x) / m.kbKeyW;
        if (col >= 0 && col < kw) {
            // A blank cell ('\0', symbols page only) types nothing -- without this check it would
            // append a NUL to the password.
            const char c = qwerty_keyset[charPage][row][col][caps ? 1 : 0];
            if (c) currentText += c;
        }
    }
}
