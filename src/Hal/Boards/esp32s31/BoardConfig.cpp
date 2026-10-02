// Default-board implementation for the ESP32-S31 chip type.
//
// Board selection is COMPILE-TIME via a positive TARGET_* macro. This file defines the global `tft`
// symbol and the HAL functions declared in ../Board.h, so it MUST be mutually exclusive with every
// other board implementation — otherwise the linker sees duplicate symbols. It is therefore keyed
// on the positive TARGET_ESP32S31_DEFAULT macro rather than an inverse "none of the others" test:
// an inverse test silently breaks the moment a new TARGET_* board is added and forgotten here.
//
// The S31 is a RISC-V chip brought up on arduino-esp32 4.x (ESP-IDF 6.1), which is why this
// environment pins a different pioarduino platform from the ESP32/ESP32-S3 defaults. See
// Documentation/Display_Touch_Architecture.md §5.
#if defined(TARGET_ESP32S31_DEFAULT)

#include "../Board.h"
#include "Hal/Display/RamFramebufferDisplay.h"
#include "Hal/Display/TftEspiDisplay.h"
#include "BoardConfig.h"
#include "Settings/TouchDriver.h"
#include <SPI.h>
#include <SD.h>
#include "FileSystem/FileSystem.h"

// Global display instance. TFT_eSPI is configured entirely by the environment's -D build flags
// (USER_SETUP_LOADED=1 + <CONTROLLER>_DRIVER=1 + pins), never from a User_Setup.h.
// The display backend for this board: the concrete object lives here, and `tft` is the
// KryonDisplay reference the rest of the OS draws through. Which backend is chosen comes
// from KRYONOS_DISPLAY_BACKEND (src/Hal/Display/DisplayConfig.h), defaulting to TFT_eSPI.
#if KRYONOS_DISPLAY_BACKEND == KRYONOS_BACKEND_RAM
static RamFramebufferDisplay s_display(KRYONOS_DISPLAY_WIDTH, KRYONOS_DISPLAY_HEIGHT);
#else
static TftEspiDisplay s_display;
#endif
KryonDisplay& tft = s_display;

// Capabilities
bool hasTouch(void) { return true; }
bool hasKeyboard(void) { return false; }
bool hasBattery(void) { return false; }

// Touch Interface
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

void setBacklight(uint8_t brightness) {
#if defined(TFT_BL)
    analogWrite(TFT_BL, brightness);
#endif
}

void setRGBLED(uint8_t, uint8_t, uint8_t, bool) {}

// Hardware Lifecycle
void initHardware(void) {
    Serial.println("[Board ESP32-S31] Initializing Hardware...");
#if defined(TFT_BL)
    pinMode(TFT_BL, OUTPUT);
    digitalWrite(TFT_BL, HIGH);
#endif
    Serial.println("[Board ESP32-S31] Hardware Initialized.");
}

void initDisplay(void) {
    Serial.println("[Board ESP32-S31] Initializing Display...");
    tft.init();
    tft.setRotation(KRYONOS_DISPLAY_ROTATION);
    tft.fillScreen(TFT_BLACK);
}

void initTouch(void) {
    Serial.println("[Board ESP32-S31] Initializing TouchDriver...");
    TouchDriver::init(&tft);
}

// SD Card
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

// Keyboard Dummies
BoardKey getKeyInput(void) { return BOARD_KEY_NONE; }
void updateModifiers(BoardKey) {}
void clearModifiers(void) {}
char keyToChar(BoardKey) { return '\0'; }
bool isShiftActive(void) { return false; }
bool isFnActive(void) { return false; }
float getBatteryVoltage(void) { return 0.0f; }
int getBatteryPercent(void) { return 0; }

#endif // TARGET_ESP32S31_DEFAULT
