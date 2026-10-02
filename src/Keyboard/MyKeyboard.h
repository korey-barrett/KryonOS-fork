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
    static void drawKeyboard(String currentText, String promptMsg, bool caps, int selectedX, int selectedY, int page);
    static void handleTouch(uint16_t x, uint16_t y, String &currentText, bool &caps, bool &done, int &page);
};

#endif // MY_KEYBOARD_H
