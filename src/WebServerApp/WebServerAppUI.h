#pragma once

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "Hal/Display/KryonDisplay.h"
class WebServerAppUI {
private:
    static KryonDisplay *tftInstance;

public:
    static void init(KryonDisplay *tft);
    static void draw();
    static void handleTouch(uint16_t x, uint16_t y);
};
