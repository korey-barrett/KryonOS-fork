// Default-board implementation for the plain ESP32 (Xtensa LX6) chip type.
//
// Board selection is COMPILE-TIME via a positive TARGET_* macro. This file defines the global `tft`
// symbol and the HAL functions declared in ../Board.h, so it MUST be mutually exclusive with every
// other board implementation — otherwise the linker sees duplicate symbols. It is therefore keyed
// on the positive TARGET_ESP32_DEFAULT macro rather than an inverse "none of the others" test:
// an inverse test silently breaks the moment a new TARGET_* board is added and forgotten here.
#if defined(TARGET_ESP32_DEFAULT)

#include "../Board.h"
#include "BoardConfig.h"
#include "Settings/TouchDriver.h"
#include <SPI.h>
#include <SD.h>
#include "FileSystem/FileSystem.h"

// Global display instance. TFT_eSPI is configured entirely by the environment's -D build flags
// (USER_SETUP_LOADED=1 + <CONTROLLER>_DRIVER=1 + pins), never from a User_Setup.h.
TFT_eSPI tft = TFT_eSPI();

// --- Capabilities ---
bool hasTouch(void) { return true; }
bool hasKeyboard(void) { return false; }
bool hasBattery(void) { return false; }

// --- Touch (routed through the TouchDriver facade so calibration and rotation are shared) ---
bool isTouched(void) {
    uint16_t x, y;
    return TouchDriver::getTouch(&x, &y);
}

bool getTouch(uint16_t *x, uint16_t *y) {
    return TouchDriver::getTouch(x, y);
}

bool getTouchRaw(uint16_t *x, uint16_t *y) {
    return TouchDriver::getTouchRaw(x, y);
}

void loadTouchCalibration(void) {
    uint16_t calData[4];
    if (FileSystem::readCalData(calData)) {
        TouchDriver::setTouch(calData);
    }
}

// --- Hardware controls ---
void setBacklight(uint8_t brightness) {
#if defined(TFT_BL)
    analogWrite(TFT_BL, brightness);
#else
    (void)brightness;
#endif
}

void setRGBLED(uint8_t, uint8_t, uint8_t, bool) {}

// --- Hardware lifecycle ---
void initHardware(void) {
    Serial.println("[Board ESP32] Initializing Hardware...");
#if defined(TFT_BL)
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
#endif
    Serial.println("[Board ESP32] Hardware Initialized.");
}

void initDisplay(void) {
    Serial.println("[Board ESP32] Initializing Display...");
    tft.init();
    tft.setRotation(KRYONOS_DISPLAY_ROTATION);
    tft.fillScreen(TFT_BLACK);
}

void initTouch(void) {
    Serial.println("[Board ESP32] Initializing TouchDriver...");
    TouchDriver::init(&tft);
}

// --- SD card ---
fs::FS* initSD(void) {
    if (FileSystem::isSDMounted()) {
        return &SD;
    }
    if (FileSystem::mountSD()) {
        return &SD;
    }
    return nullptr;
}

void deinitSD(void) {
    FileSystem::unmountSD();
}

uint64_t getSDTotalBytes(void) {
    if (!FileSystem::isSDMounted()) return 0;
    return SD.totalBytes();
}

uint64_t getSDUsedBytes(void) {
    if (!FileSystem::isSDMounted()) return 0;
    return SD.usedBytes();
}

bool isSDMounted(void) {
    return FileSystem::isSDMounted();
}

// --- Keyboard & battery dummies for a touch-only generic board ---
BoardKey getKeyInput(void) { return BOARD_KEY_NONE; }
void updateModifiers(BoardKey) {}
void clearModifiers(void) {}
char keyToChar(BoardKey) { return '\0'; }
bool isShiftActive(void) { return false; }
bool isFnActive(void) { return false; }
float getBatteryVoltage(void) { return 0.0f; }
int getBatteryPercent(void) { return 0; }

#endif // TARGET_ESP32_DEFAULT
