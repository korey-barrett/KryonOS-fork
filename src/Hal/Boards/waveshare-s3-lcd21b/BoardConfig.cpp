// Board implementation for the Waveshare ESP32-S3-Touch-LCD-2.1B.
//
// Board selection is COMPILE-TIME via a positive TARGET_* macro. This file defines the global `tft`
// symbol and the HAL functions declared in ../Board.h, so it MUST be mutually exclusive with every
// other board implementation — otherwise the linker sees duplicate symbols. It is therefore keyed on
// the positive TARGET_WAVESHARE_S3_LCD21B macro rather than an inverse "none of the others" test:
// an inverse test silently breaks the moment a new TARGET_* board is added and forgotten here.
//
// This board is the first to use a non-TFT_eSPI panel driver: the ST7701 here is RGB565 parallel, so
// TFT_eSPI cannot drive it at all. See src/Hal/Display/EspLcdRgbDisplay.h and
// Documentation/Display_Touch_Architecture.md.
#if defined(TARGET_WAVESHARE_S3_LCD21B)

#include "../Board.h"
#include "Hal/Display/EspLcdRgbDisplay.h"
#include "BoardConfig.h"
#include "Settings/TouchDriver.h"
#include <SD_MMC.h>
#include "FileSystem/FileSystem.h"

// The panel's backlight is on GPIO6, driven by the backend over LEDC.
static constexpr int kBacklightPin = 6;

// Unlike the chip defaults, this board has exactly ONE possible backend, so there is no #if/#else
// choosing between implementations here. The TFT_eSPI this environment compiles is a rasterizer, not
// a panel driver: it is never init()ed and owns no pins, so a TftEspiDisplay built on it would push
// pixels at an unconfigured bus. The check below keeps the environment and the code in agreement --
// KRYONOS_DISPLAY_BACKEND stays the single source of truth, and an environment that forgets the flag
// fails the build instead of silently mis-building.
#if KRYONOS_DISPLAY_BACKEND != KRYONOS_BACKEND_RGB
#error "waveshare-s3-lcd21b needs -D KRYONOS_DISPLAY_BACKEND=KRYONOS_BACKEND_RGB (see platformio.ini)"
#endif

// Global display instance. Unlike every other board, this one is NOT a TftEspiDisplay: the backend
// owns the RGB panel, the ST7701 init sequence, the TCA9554 expander and the backlight.
//
// The two size pairs are different arguments because they are different things: BOARD_PANEL_* is the
// scan size the ST7701's timings come from, KRYONOS_DISPLAY_* is the logical canvas the UI draws to
// and is scaled up into that panel. Passing the canvas as the panel size would rewrite the timings.
static EspLcdRgbDisplay s_display(BOARD_PANEL_W, BOARD_PANEL_H, KRYONOS_DISPLAY_WIDTH,
                                  KRYONOS_DISPLAY_HEIGHT, kBacklightPin);
KryonDisplay& tft = s_display;

// Capabilities
// Touch is live: the panel carries a CST820 on the shared I2C bus, driven by the CST816 driver (see
// CapacitiveTouchDriver.h for why the two parts share a register map). Nothing in src/ actually reads
// this function today -- the real switch is the KRYONOS_TOUCH_* set in platformio.ini -- so this is
// the board contract and the docs telling the truth rather than a gate that enables input.
bool hasTouch(void) { return true; }
bool hasKeyboard(void) { return false; }
bool hasBattery(void) { return false; }

// Touch Interface. TouchDriver resolves to the CST816 driver here because platformio.ini names it and
// gives it the bus pins; the controller's reset is pulsed by that driver, immediately before it
// probes (see CapacitiveTouchDriver.cpp -- a pulse during display bring-up is too early to matter).
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
    // The CST820 is capacitive: it needs no calibration, and there is nothing to load. Keep the
    // function so the Board.h contract is satisfied without pretending a calibration exists.
}

void setBacklight(uint8_t brightness) {
    s_display.setBacklight(brightness);
}

void setRGBLED(uint8_t, uint8_t, uint8_t, bool) {}

// Hardware Lifecycle
void initHardware(void) {
    Serial.println("[Board Waveshare S3 LCD2.1B] Initializing Hardware...");
    // Nothing to do: the panel, expander, backlight and I2C bus are all brought up by the display
    // backend's init(), which Display::begin() calls. Deliberately no TFT_BL pinMode here — a
    // phantom TFT_eSPI is compiled on this env, but it is never init()ed and owns no pins.
    Serial.println("[Board Waveshare S3 LCD2.1B] Hardware Initialized.");
}

void initDisplay(void) {
    Serial.println("[Board Waveshare S3 LCD2.1B] Initializing Display...");
    tft.init();
    tft.setRotation(KRYONOS_DISPLAY_ROTATION);
    tft.fillScreen(TFT_BLACK);
    tft.present();
}

// Board.h declares this and every board defines it, but nothing on the live boot path calls it --
// main.cpp does Display::begin() then TouchDriver::init(&tft) directly, and the latter is what
// actually brings touch up (including the TP_RST pulse, which lives in the driver). Kept so the
// Board.h contract stays satisfied, and so it would still be correct if the lifecycle were ever
// wired up; it deliberately does NOT pulse the reset, because a pulse here would never run and
// implying touch depends on this function is how a future reader gets misled.
void initTouch(void) {
    Serial.println("[Board Waveshare S3 LCD2.1B] Initializing TouchDriver...");
    TouchDriver::init(&tft);
}

// SD Card -- SDMMC 1-bit on CLK=GPIO2, CMD=GPIO1, D0=GPIO42.
//
// GPIO1/GPIO2 are shared with the ST7701's 3-wire command channel, which the display backend
// bit-bangs during panel bring-up and then hands back. That is why this must run after the panel is
// up -- and FileSystem::init(), which calls this, is several steps below Display::begin() in
// main.cpp. Mounting before that would fight the command bus; mounting during it would corrupt the
// init table.
//
// The slot wires only DAT0, so it is 1-bit and cannot be driven by the SPI `SD` class (there is no
// chip-select line). Every failure path returns nullptr instead of aborting: this board is meant to
// stay reachable with no card in the slot.
fs::FS* initSD(void) {
    if (SD_MMC.cardType() != CARD_NONE) return &SD_MMC; // already mounted

    SD_MMC.setPins(BOARD_SD_CLK_PIN, BOARD_SD_CMD_PIN, BOARD_SD_D0_PIN);

    // mode1bit = true (only DAT0 is wired). format_if_mount_failed stays false: a failed mount must
    // never reformat a card that may hold the user's data.
    if (!SD_MMC.begin("/sd", true)) {
        Serial.println("[Board Waveshare S3 LCD2.1B] SD mount failed (card inserted? FAT32?)");
        return nullptr;
    }
    if (SD_MMC.cardType() == CARD_NONE) {
        Serial.println("[Board Waveshare S3 LCD2.1B] SD mounted but no card detected");
        SD_MMC.end();
        return nullptr;
    }

    Serial.printf("[Board Waveshare S3 LCD2.1B] SD mounted: %llu MB\n",
                  static_cast<unsigned long long>(SD_MMC.cardSize() / (1024 * 1024)));
    return &SD_MMC;
}

void deinitSD(void) {
    if (SD_MMC.cardType() != CARD_NONE) SD_MMC.end();
}

uint64_t getSDTotalBytes(void) {
    return SD_MMC.cardType() == CARD_NONE ? 0 : SD_MMC.totalBytes();
}

uint64_t getSDUsedBytes(void) {
    return SD_MMC.cardType() == CARD_NONE ? 0 : SD_MMC.usedBytes();
}

bool isSDMounted(void) { return SD_MMC.cardType() != CARD_NONE; }

// Keyboard Dummies
BoardKey getKeyInput(void) { return BOARD_KEY_NONE; }
void updateModifiers(BoardKey) {}
void clearModifiers(void) {}
char keyToChar(BoardKey) { return '\0'; }
bool isShiftActive(void) { return false; }
bool isFnActive(void) { return false; }
float getBatteryVoltage(void) { return 0.0f; }
int getBatteryPercent(void) { return 0; }

#endif // TARGET_WAVESHARE_S3_LCD21B
