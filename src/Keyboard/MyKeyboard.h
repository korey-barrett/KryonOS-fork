#ifndef MY_KEYBOARD_H
#define MY_KEYBOARD_H

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "Hal/Display/KryonDisplay.h"
class MyKeyboard {
public:
    static void init(KryonDisplay *tft);
    static String getString(String initialText, String promptMsg, int maxLen = 30);

private:
    static KryonDisplay *tftInstance;
    // `page` is the ROW pager (which page of rows the grid shows, from the metrics); `charPage` is
    // the CHARACTER set (letters+digits / symbols, from the key table). They are separate inputs
    // because a board uses one or the other, and merging them into one variable is how a short
    // panel with a symbols page would silently start drawing the wrong rows.
    static void drawKeyboard(String currentText, String promptMsg, bool caps, int selectedX, int selectedY, int page, int charPage);
    static void handleTouch(uint16_t x, uint16_t y, String &currentText, bool &caps, bool &done, int &page, int &charPage);
};

#endif // MY_KEYBOARD_H
