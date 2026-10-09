// Board implementation for Espressif's ESP32-S31-Korvo-1.
//
// Board selection is COMPILE-TIME via a positive TARGET_* macro. This file defines the global `tft`
// symbol and the HAL functions declared in ../Board.h, so it MUST be mutually exclusive with every
// other board implementation -- otherwise the linker sees duplicate symbols. It is therefore keyed on
// the positive TARGET_ESP32S31_KORVO1 macro rather than an inverse "none of the others" test: an
// inverse test silently breaks the moment a new TARGET_* board is added and forgotten here.
//
// `[env:esp32s31-korvo1]` defines that macro, so under PlatformIO this is the *active* board
// implementation for that one environment; the other environments compile it to nothing and are
// untouched. The IDF build defines it too (see idf/main/CMakeLists.txt).
//
// Like the Waveshare board, this one has exactly ONE possible backend, and it is not a TftEspiDisplay:
// TFT_eSPI cannot drive an RGB parallel panel at all, and on this target it is a rasterizer-only shim
// that is never init()ed and owns no pins. The check below keeps the environment and the code in
// agreement, so a build that forgets the flag fails instead of silently pushing pixels at an
// unconfigured bus.
#if defined(TARGET_ESP32S31_KORVO1)

#include "../Board.h"
#include "BoardConfig.h"

#include "Hal/Display/KryonDisplay.h"
#include "KorvoRgbDisplay.h" // src/Hal/Display -- the RGB backend. Configures the panel itself through
                             // esp_lcd rather than through Espressif's BSP, so the board can also
                             // build under PlatformIO, where no BSP component exists.

#include "Settings/TouchDriver.h"
#include "FileSystem/FileSystem.h"

// The card is mounted with Arduino's SDMMC filesystem rather than the BSP's VFS mount -- see initSD().
#include <SD_MMC.h>

// The board's pin facts, vendored from Espressif's BSP (esp32_s31_korvo_1.h) so this file carries no
// IDF-only dependency -- the same reason KorvoRgbDisplay vendors the panel's timings. See initSD() for
// why the enable line is load-bearing.
namespace {
constexpr int kSdClk = 24;     // BSP_SD_CLK
constexpr int kSdCmd = 25;     // BSP_SD_CMD
constexpr int kSdD0 = 20;      // BSP_SD_D0
constexpr int kSdD1 = 21;      // BSP_SD_D1
constexpr int kSdD2 = 22;      // BSP_SD_D2
constexpr int kSdD3 = 23;      // BSP_SD_D3
constexpr int kSdEnable = 39;  // BSP_SD_EN -- the slot's power/enable line
} // namespace

#if KRYONOS_DISPLAY_BACKEND != KRYONOS_BACKEND_RGB
#error "esp32s31-korvo1 needs -D KRYONOS_DISPLAY_BACKEND=KRYONOS_BACKEND_RGB"
#endif

// Global display instance. The backend owns the RGB panel: it configures it through esp_lcd directly
// (the panel values are vendored into KorvoRgbDisplay from the BSP), then attaches its canvas to the
// panel's own frame buffer so every primitive lands in the memory the scanout reads.
//
// The size is passed as the canvas size and NOT as a separate panel size, because on this board they
// are the same thing -- the canvas is 1:1 with the panel and panelToCanvas() is the identity. The
// Waveshare port passes two pairs only because its round aperture makes them genuinely different.
static KorvoRgbDisplay s_display(KRYONOS_DISPLAY_WIDTH, KRYONOS_DISPLAY_HEIGHT);
KryonDisplay& tft = s_display;

// Capabilities.
bool hasTouch(void) { return true; }
bool hasKeyboard(void) { return false; }
bool hasBattery(void) { return false; }

// Touch Interface. The environment names the driver (-D KRYONOS_TOUCH_DRIVER="gt1151"), so the
// register map lives in Gt1151Driver rather than behind a BSP call. This board's touch interrupt is
// not connected (the BSP reports it as GPIO_NUM_NC), so the driver polls: there is no line to wait on
// and nothing to miss.
bool isTouched(void) {
    uint16_t x, y;
    return TouchDriver::getTouch(&x, &y);
}

bool getTouch(uint16_t *x, uint16_t *y) { return TouchDriver::getTouch(x, y); }

bool getTouchRaw(uint16_t *x, uint16_t *y) { return TouchDriver::getTouchRaw(x, y); }

void loadTouchCalibration(void) {
    // The GT1158 reports absolute positions, so there is nothing to calibrate and nothing to load.
    // Kept so the Board.h contract is satisfied without pretending a calibration file exists.
}

void setBacklight(uint8_t brightness) {
    // Delegated to the backend even though the answer is always ESP_ERR_NOT_SUPPORTED on this board:
    // the backlight is hardwired on (BSP_LCD_BACKLIGHT is GPIO_NUM_NC). Going through the backend means
    // the one place that knows that is also the one place that logs it, rather than every caller
    // inventing its own no-op.
    s_display.setBacklight(brightness);
}

void setRGBLED(uint8_t, uint8_t, uint8_t, bool) {
    // The BSP carries an led_indicator dependency for the board's LED, but nothing in KryonOS drives an
    // LED today and wiring one here would be the first use of a component we otherwise never call.
}

// Hardware Lifecycle.
void initHardware(void) {
    Serial.println("[Board Korvo-1] Initializing Hardware...");
    // Nothing to do: the panel, backlight and touch controller are all brought up by the BSP, reached
    // through the display backend's init() and the touch driver's begin(), which Display::begin() and
    // TouchDriver::init() call. Deliberately no pinMode on a backlight pin -- there is no such pin.
    Serial.println("[Board Korvo-1] Hardware Initialized.");
}

void initDisplay(void) {
    Serial.println("[Board Korvo-1] Initializing Display...");
    tft.init();
    tft.setRotation(KRYONOS_DISPLAY_ROTATION);
    tft.fillScreen(TFT_BLACK);
    tft.present();
}

// Board.h declares this and every board defines it, but nothing on the live boot path calls it -- see
// the note in the Waveshare board file: main.cpp does Display::begin() then TouchDriver::init(&tft)
// directly. Kept so the contract stays satisfied and so it would still be correct if the lifecycle were
// ever wired up.
void initTouch(void) {
    Serial.println("[Board Korvo-1] Initializing TouchDriver...");
    TouchDriver::init(&tft);
}

// SD Card -- 4-bit SDMMC on the board's own pins, mounted as an Arduino fs::FS.
//
// WHY THE MOUNT IS ARDUINO'S
//   The BSP can mount this card, and what it hands back is an esp_vfs_fat_sdmmc_mount registration --
//   a path under the VFS. Board.h's contract is fs::FS*, and the file browser, app loader and installer
//   are all written against that type. Adopting the VFS mount would mean rewriting the storage layer
//   against raw fopen paths, which is exactly the kind of change this port exists to avoid. So the pins
//   are vendored above and the mount itself is Arduino's, which yields a real fs::FS the rest of the OS
//   already understands.
//
// THE ENABLE PIN IS THE PART THAT WOULD SILENTLY FAIL
//   The slot has a power/enable line on GPIO39 that has to be driven before the card answers. Left
//   alone, SD_MMC.begin() finds no card and reports a mount failure -- which reads like a card problem
//   rather than a missing enable.
//
// NO CARD-DETECT PIN, so there is no way to tell "empty slot" from "bad card". Every failure returns
// nullptr rather than aborting, the same shape the Waveshare board uses -- this board must stay
// reachable with no card in the slot.
fs::FS* initSD(void) {
    if (SD_MMC.cardType() != CARD_NONE) return &SD_MMC; // already mounted

    // Driven on every attempt rather than once: re-asserting it is free, and a mount can fail for
    // reasons other than the enable line, so a retry should not depend on remembering whether an
    // earlier call already ran.
    pinMode(kSdEnable, OUTPUT);
    digitalWrite(kSdEnable, LOW);

    // 4-bit: the slot wires D0-D3, unlike the Waveshare board's 1-bit slot, which is why all six pins
    // are named here and mode1bit is false below.
    SD_MMC.setPins(kSdClk, kSdCmd, kSdD0, kSdD1, kSdD2, kSdD3);

    // format_if_mount_failed stays false on purpose: a failed mount must never reformat a card that may
    // hold the user's data -- and this board cannot even tell "no card" from "unreadable card", which is
    // the worst possible situation in which to decide a card needs erasing.
    if (!SD_MMC.begin("/sd", false, false)) {
        Serial.println("[Board Korvo-1] SD mount failed (card inserted? FAT32?)");
        return nullptr;
    }
    if (SD_MMC.cardType() == CARD_NONE) {
        Serial.println("[Board Korvo-1] SD mounted but no card detected");
        SD_MMC.end();
        return nullptr;
    }

    Serial.printf("[Board Korvo-1] SD mounted: %llu MB\n",
                  static_cast<unsigned long long>(SD_MMC.cardSize() / (1024 * 1024)));
    return &SD_MMC;
}

void deinitSD(void) {
    if (SD_MMC.cardType() != CARD_NONE) SD_MMC.end();
}

uint64_t getSDTotalBytes(void) { return SD_MMC.cardType() == CARD_NONE ? 0 : SD_MMC.totalBytes(); }

uint64_t getSDUsedBytes(void) { return SD_MMC.cardType() == CARD_NONE ? 0 : SD_MMC.usedBytes(); }

bool isSDMounted(void) { return SD_MMC.cardType() != CARD_NONE; }

// Keyboard Dummies.
BoardKey getKeyInput(void) { return BOARD_KEY_NONE; }
void updateModifiers(BoardKey) {}
void clearModifiers(void) {}
char keyToChar(BoardKey) { return '\0'; }
bool isShiftActive(void) { return false; }
bool isFnActive(void) { return false; }
float getBatteryVoltage(void) { return 0.0f; }
int getBatteryPercent(void) { return 0; }

#endif // TARGET_ESP32S31_KORVO1
