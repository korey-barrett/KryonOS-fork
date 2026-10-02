#include "MyKeyboard.h"
#include "../Settings/TouchDriver.h"
#include "../UI/UiLayout.h"

KryonDisplay *MyKeyboard::tftInstance = nullptr;

// Screen metrics. At 240x320 the keyboard metrics reproduce the historical literals exactly:
// text box (5,30,230,30), 5 buttons 48px wide at y70..100, 12x4 key grid from y110, keys 20x52.
// A short panel compresses that chrome and, if even then the keys would be too small to read,
// shows the grid a page of rows at a time (see the keyboard block in UiLayout::compute).
static inline const UiMetrics& M() { return UiLayout::current(); }

const int kw = 12; // keyboard width
const int kh = 4;  // keyboard height
char qwerty_keyset[kh][kw][2] = {
    {{'1', '!'}, {'2', '@'}, {'3', '#'}, {'4', '$'}, {'5', '%'}, {'6', '^'}, {'7', '&'}, {'8', '*'}, {'9', '('}, {'0', ')'}, {'-', '_'}, {'=', '+'}},
    {{'q', 'Q'}, {'w', 'W'}, {'e', 'E'}, {'r', 'R'}, {'t', 'T'}, {'y', 'Y'}, {'u', 'U'}, {'i', 'I'}, {'o', 'O'}, {'p', 'P'}, {'[', '{'}, {']', '}'}},
    {{'a', 'A'}, {'s', 'S'}, {'d', 'D'}, {'f', 'F'}, {'g', 'G'}, {'h', 'H'}, {'j', 'J'}, {'k', 'K'}, {'l', 'L'}, {';', ':'}, {'\'', '"'}, {'\\', '|'}},
    {{'\\', '|'}, {'z', 'Z'}, {'x', 'X'}, {'c', 'C'}, {'v', 'V'}, {'b', 'B'}, {'n', 'N'}, {'m', 'M'}, {',', '<'}, {'.', '>'}, {'/', '?'}, {' ', ' '}}
};

void MyKeyboard::init(KryonDisplay *tft) {
    tftInstance = tft;
}

String MyKeyboard::getString(String initialText, String promptMsg, int maxLen) {
    if (!tftInstance) return "";

    String currentText = initialText;
    bool caps = false;
    bool done = false;
    int page = 0;

    // Draw initial state
    drawKeyboard(currentText, promptMsg, caps, -1, -1, page);

    while (!done) {
        uint16_t x, y;
        if (TouchDriver::getTouch(&x, &y)) {
            handleTouch(x, y, currentText, caps, done, page);
            if (!done) {
                drawKeyboard(currentText, promptMsg, caps, -1, -1, page);
            }
            delay(200); // Debounce
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

void MyKeyboard::drawKeyboard(String currentText, String promptMsg, bool caps, int selectedX, int selectedY, int page) {
    const UiMetrics& m = M();

    tftInstance->fillScreen(TFT_BLACK);

    // Prompt
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(TL_DATUM);
    tftInstance->drawString(promptMsg, m.kbPromptX, m.kbPromptY, m.fontBody);

    // Text box
    tftInstance->drawRect(m.kbTextBox.x, m.kbTextBox.y, m.kbTextBox.w, m.kbTextBox.h, TFT_GREEN);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->drawString(currentText + "_", m.kbTextBox.x + 5, m.kbTextBox.y + 8, m.fontBody);

    // Top Row Buttons (OK, CAPS, DEL, SPACE, ESC)
    const int btnW = m.kbButtonW;
    const char* btns[] = {"OK", caps ? "abc" : "ABC", "DEL", "SPACE", "ESC"};
    for (int i=0; i<m.kbButtonCount; i++) {
        tftInstance->drawRect(i * btnW, m.kbButtonRow.y, btnW, m.kbButtonRow.h, TFT_GREEN);
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString(btns[i], i * btnW + btnW/2, m.kbButtonRow.cy(), m.fontBody);
    }

    // Keyboard Grid — only the rows on the current page have a cell, so an off-page row is skipped.
    for (int row = 0; row < kh; row++) {
        for (int col = 0; col < kw; col++) {
            const UiRect key = m.kbKeyRect(row, col, page);
            if (key.w == 0) continue;

            tftInstance->drawRect(key.x, key.y, key.w, key.h, TFT_DARKGREY);

            char c = caps ? qwerty_keyset[row][col][1] : qwerty_keyset[row][col][0];
            tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString(String(c), key.cx(), key.cy(), m.fontBody);
        }
    }

    // Pager strip — only drawn when the panel is too short for all four rows at a legible size.
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

void MyKeyboard::handleTouch(uint16_t x, uint16_t y, String &currentText, bool &caps, bool &done, int &page) {
    const UiMetrics& m = M();

    // Pager strip — checked first because it sits below the grid's last row.
    if (m.kbPagerH > 0 && m.kbPagerStrip.contains((int16_t)x, (int16_t)y)) {
        if (m.kbPagerPrev().contains((int16_t)x, (int16_t)y)) {
            page = (page + m.kbPages - 1) % m.kbPages;
        } else if (m.kbPagerNext().contains((int16_t)x, (int16_t)y)) {
            page = (page + 1) % m.kbPages;
        }
        return;
    }

    // Top Row Buttons
    if (m.kbButtonRow.contains((int16_t)x, (int16_t)y)) {
        const int btnW = m.kbButtonW;
        if (x < btnW * 1) {
            done = true; // OK
        } else if (x < btnW * 2) {
            caps = !caps; // CAPS
        } else if (x < btnW * 3) {
            if (currentText.length() > 0) currentText.remove(currentText.length() - 1); // DEL
        } else if (x < btnW * 4) {
            currentText += " "; // SPACE
        } else {
            currentText = ""; // ESC
            done = true;
        }
        return;
    }

    // Keyboard Grid
    const int row = m.kbRowFromY((int16_t)y, page);
    if (row >= 0) {
        const int col = ((int)x) / m.kbKeyW;
        if (col >= 0 && col < kw) {
            char c = caps ? qwerty_keyset[row][col][1] : qwerty_keyset[row][col][0];
            currentText += c;
        }
    }
}
