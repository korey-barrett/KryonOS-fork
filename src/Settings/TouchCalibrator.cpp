#include "TouchCalibrator.h"
#include "TouchDriver.h"
#include "../FileSystem/FileSystem.h"
#include "../UI/UiLayout.h"

extern int currentState;

TFT_eSPI *TouchCalibrator::tftInstance = nullptr;

void TouchCalibrator::init(TFT_eSPI *tft) {
    tftInstance = tft;
    TouchDriver::init(tft);
}

void TouchCalibrator::runCalibration() {
    if (!tftInstance) return;

    // Absolute-position drivers (capacitive panels) have no calibration to take, and a board with
    // no touch panel has nothing to press. Both must leave immediately rather than draw four
    // corner arrows that would spin forever waiting for a raw reading that never arrives.
    if (!TouchDriver::needsCalibration()) {
        Serial.printf("[TOUCH] '%s' needs no calibration; skipping.\n", TouchDriver::driverName());
        currentState = 0;
        return;
    }

    // Vertical positions stay proportional to the canvas (5/32, 5/16, 3/8 reproduce the original
    // 50 / 100 / 120 exactly at 240x320).
    const UiMetrics& m = UiLayout::current();

    tftInstance->fillScreen(TFT_BLACK);
    tftInstance->setTextDatum(TC_DATUM);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->drawString("Touch Calibration", m.centerX, (int16_t)(m.h * 5 / 32), m.fontHeader);
    tftInstance->drawString("Touch the arrows at the corners", m.centerX,
                            (int16_t)(m.h * 5 / 16), m.fontBody);

    uint16_t calData[5];
    TouchDriver::calibrateTouch(calData, TFT_RED, TFT_BLACK, 15);

    tftInstance->fillScreen(TFT_BLACK);
    tftInstance->drawString("Calibration Saved!", m.centerX, (int16_t)(m.h * 3 / 8), m.fontBody);

    FileSystem::writeCalData(calData);
    TouchDriver::setTouch(calData);

    delay(1000);
    // Transition back to Launcher
    currentState = 0; 
}

