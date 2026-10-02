#include "Display.h"
#include "DisplayConfig.h"

#include <Arduino.h>
#include <TFT_eSPI.h>

#include "Hal/Boards/Board.h" // for the global `tft` instance

// Defaults come from the compile-time contract so early callers (before begin()) still get sane
// numbers. begin() overwrites them with what the panel actually reports.
int16_t Display::s_width    = KRYONOS_DISPLAY_WIDTH;
int16_t Display::s_height   = KRYONOS_DISPLAY_HEIGHT;
uint8_t Display::s_rotation = KRYONOS_DISPLAY_ROTATION;
bool    Display::s_bound    = false;

void Display::begin() {
    tft.init();
    tft.setRotation(KRYONOS_DISPLAY_ROTATION);

    s_rotation = KRYONOS_DISPLAY_ROTATION;
    s_width    = static_cast<int16_t>(tft.width());
    s_height   = static_cast<int16_t>(tft.height());
    s_bound    = true;

    Serial.printf("[Display] board=%s canvas=%dx%d rotation=%u (flags %dx%d)\n",
                  boardId(), s_width, s_height, s_rotation,
                  static_cast<int>(KRYONOS_DISPLAY_WIDTH),
                  static_cast<int>(KRYONOS_DISPLAY_HEIGHT));

    if (s_width != static_cast<int16_t>(KRYONOS_DISPLAY_WIDTH) ||
        s_height != static_cast<int16_t>(KRYONOS_DISPLAY_HEIGHT)) {
        Serial.printf("[Display] WARNING: panel reports %dx%d but KRYONOS_DISPLAY_WIDTH/HEIGHT are "
                      "%dx%d. Check TFT_WIDTH/TFT_HEIGHT and KRYONOS_DISPLAY_ROTATION for this "
                      "board.\n",
                      s_width, s_height,
                      static_cast<int>(KRYONOS_DISPLAY_WIDTH),
                      static_cast<int>(KRYONOS_DISPLAY_HEIGHT));
    }
}

bool Display::isBound() { return s_bound; }

int16_t Display::width()  { return s_width; }
int16_t Display::height() { return s_height; }
uint8_t Display::rotation() { return s_rotation; }

bool Display::isLandscape() { return s_width > s_height; }
int16_t Display::centerX()  { return static_cast<int16_t>(s_width / 2); }
int16_t Display::centerY()  { return static_cast<int16_t>(s_height / 2); }

const char* Display::boardId() { return KRYONOS_BOARD_ID; }

void Display::setRotation(uint8_t rotation) {
    s_rotation = rotation;
    tft.setRotation(rotation);
    s_width  = static_cast<int16_t>(tft.width());
    s_height = static_cast<int16_t>(tft.height());
}
