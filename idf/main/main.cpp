// The Korvo-1 ENTRY POINT.
//
// By default this file defines almost nothing: the OS's setup()/loop() come from ../src/main.cpp,
// which kryonos_app compiles, and the Arduino core's generated app_main calls them. That is the
// normal build.
//
// Everything below is the M2/M3 BRING-UP DIAGNOSTIC, compiled only when the project is configured
// with -DKRYONOS_KORVO_BRINGUP=1. It is kept because it is the instrument every hardware question on
// this board has been answered with -- the panel pattern separates "the panel is dead" from "the
// rasterizer is wrong", the font metrics catch a text-engine regression that a screenshot would not
// explain, and the touch log is what proved the controller works. When something on the S31 looks
// wrong, this is the build that tells you which layer is at fault.
//
// M3: display and touch on the ESP32-S31-Korvo-1, both through KryonOS's own seams.
//
// M1 proved the Arduino core and the BSP come up and that the panel and touch both work. M2 added the
// thing the BSP cannot supply: a KryonDisplay backend. This adds the touch driver, and -- more to the
// point -- gives it one that goes through the same ITouchDriver seam every other board uses, so the
// kernel, keyboard, JS bindings and settings screens need no S31-specific branch. The driver is
// EspLcdTouchDriver: an adapter over the handle bsp_touch_new() returns, not a register implementation.
//
// Still no KryonOS sources: main.cpp, the rasterizer and the text engine are the whole program. M4
// wires the real UI in behind a board config.
//
// WHAT TO LOOK FOR ON THE PANEL
//   Eight colour bars across the top; a white circle, a rounded rect and a filled triangle below
//   them; and three lines of text drawn with fonts 1, 2 and 4 at three different datums. The text is
//   the part that matters most: the glyphs come from TFT_eSPI's own vendored font tables through a
//   ported renderer, so they should be pixel-identical to the esp32s3 board's.
//
//   Text metrics are printed as well, because UiLayout's geometry is built from textWidth() -- a
//   mismatch there would misplace every label on every screen in a way no screenshot would explain.
//
// WHAT TO LOOK FOR IN THE LOG
//   VERIFIED on hardware: taps at all four corners and the centre each report where they were aimed
//   (26,2 / 793,23 / 785,477 / 41,469 / 379,274), raw and canvas agreeing throughout because this
//   backend draws 1:1. Contacts are logged per position rather than per sample -- see the note in
//   loop() for why that distinction cost a day of chasing a fault that was not there.

#if KRYONOS_KORVO_BRINGUP

#include <Arduino.h>

#include "KorvoRgbDisplay.h"
#include "Hal/Touch/EspLcdTouchDriver.h"

#include <esp_heap_caps.h>

#include "bsp/display.h"

static KorvoRgbDisplay display(BSP_LCD_H_RES, BSP_LCD_V_RES);
static EspLcdTouchDriver touchDriver;

static void drawTestPattern() {
    const uint16_t bars[] = {TFT_RED,  TFT_GREEN,   TFT_BLUE, TFT_YELLOW,
                             TFT_CYAN, TFT_MAGENTA, TFT_WHITE, TFT_DARKGREY};
    const int barW = display.width() / 8;
    for (int i = 0; i < 8; i++) {
        display.fillRect(i * barW, 0, barW, 40, bars[i]);
    }

    display.drawCircle(120, 200, 60, TFT_WHITE);
    display.fillCircle(120, 200, 30, TFT_ORANGE);
    display.drawRoundRect(300, 140, 220, 120, 18, TFT_CYAN);
    display.fillRoundRect(310, 150, 200, 100, 14, TFT_NAVY);
    display.fillTriangle(640, 260, 560, 140, 720, 140, TFT_GOLD);
    display.drawTriangle(640, 300, 560, 180, 720, 180, TFT_LIGHTGREY);

    // One line per font, so a wrong glyph or a wrong baseline shows up as a wrong line rather than
    // as an odd-looking screen. Fonts 1/2/4 are the only three ids KryonOS ever asks for.
    display.setTextColor(TFT_WHITE, TFT_BLACK);
    display.setTextDatum(TL_DATUM);
    display.drawString("font 1  GLCD 5x7  ABCabc0123", 20, 400, 1);
    display.drawString("font 2  16px  ABCabc0123", 20, 414, 2);
    display.drawString("font 4  26px RLE  ABCabc0123", 20, 436, 4);

    // The datum switch, one string at one font, so the six placements can be compared directly.
    display.setTextColor(TFT_YELLOW, TFT_BLACK);
    display.setTextDatum(TL_DATUM);
    display.drawString("TL", 480, 300, 2);
    display.setTextDatum(TC_DATUM);
    display.drawString("TC", 560, 300, 2);
    display.setTextDatum(TR_DATUM);
    display.drawString("TR", 640, 300, 2);
    display.setTextDatum(ML_DATUM);
    display.drawString("ML", 480, 330, 2);
    display.setTextDatum(MC_DATUM);
    display.drawString("MC", 560, 330, 2);
    display.setTextDatum(MR_DATUM);
    display.drawString("MR", 640, 330, 2);

    display.setTextDatum(TL_DATUM);
    display.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    display.drawString("KryonOS / ESP32-S31-Korvo-1 / esp_lcd RGB565 / KryonSprite rasterizer", 20,
                       460, 1);
}

static void reportMetrics() {
    // textWidth is what UiLayout positions everything with, so print it rather than inferring it from
    // the picture. The same strings measured on esp32s3 should give the same numbers.
    const char* sample = "KryonOS 0123 ABCabc";
    Serial.printf("[Display:korvo] textWidth(\"%s\"): font1 %d  font2 %d  font4 %d\n", sample,
                  display.textWidth(sample, 1), display.textWidth(sample, 2),
                  display.textWidth(sample, 4));
    Serial.printf("[Display:korvo] backend %s  canvas %dx%d  rotation %u\n", display.backendName(),
                  display.width(), display.height(), display.getRotation());
}

void setup() {
    Serial.begin(115200);
    delay(1200);

    Serial.println();
    Serial.println("======================================================");
    Serial.println("  KryonOS / ESP32-S31-Korvo-1 / M3 display + touch");
    Serial.println("======================================================");
    Serial.printf("  chip %s  cores %d  flash %u MB  psram %u KB\n", ESP.getChipModel(),
                  ESP.getChipCores(), (unsigned)(ESP.getFlashChipSize() / (1024 * 1024)),
                  (unsigned)(ESP.getPsramSize() / 1024));

    display.init(0);
    if (!display.ready()) {
        Serial.printf("[Display:korvo] not ready: %s -- the panel would stay blank.\n",
                      display.lastError());
        return;
    }

    display.setRotation(0); // this panel is fixed landscape; see KorvoRgbDisplay::setRotation
    reportMetrics();

    display.fillScreen(TFT_BLACK);
    drawTestPattern();
    display.present();
    Serial.println("[Display:korvo] first frame presented");

    // M3: the touch driver through the seam. Same call the Waveshare, CYD and T-HMI boards make.
    touchDriver.begin(&display);

    // A calibration screen is skipped by the OS for every absolute-position controller, so this
    // should be false. If it is ever true, the OS would ask the user to touch four corners that this
    // panel does not need -- worth saying out loud rather than discovering later.
    Serial.printf("[touch] needsCalibration %s  resistive %s\n",
                  touchDriver.needsCalibration() ? "yes" : "no",
                  touchDriver.isResistive() ? "yes" : "no");
}

void loop() {
    static uint32_t lastBeat = 0;
    static uint32_t beats = 0;
    static uint32_t polls = 0;
    static uint32_t transitions = 0;
    static uint32_t touches = 0;
    static uint32_t lastPrint = 0;
    static uint16_t lastCx = 0;
    static uint16_t lastCy = 0;
    static bool down = false;
    static bool pressShown = false;

    const uint32_t now = millis();

    display.present(); // cheap when nothing was drawn: present() returns early on the dirty flag

    // Polled, not interrupt-driven: BSP_LCD_TOUCH_INT is GPIO_NUM_NC on this board, so there is no
    // interrupt line to wait on. getTouch() is the call the rest of the OS makes -- it returns canvas
    // pixels, which is what a hit test needs.
    uint16_t cx = 0, cy = 0;
    const bool nowDown = touchDriver.getTouch(&cx, &cy, 0);
    polls++;
    if (nowDown != down) {
        transitions++; // a press or a release edge; the beat line reports the running count
        down = nowDown;
    }
    if (nowDown) {
        touches++;

        // Logged per CONTACT POSITION, never capped by a count. Both earlier attempts at this failed
        // the same way: a cap on printed contacts let a held finger fill the budget in the first
        // second, so every LATER tap went unlogged and working hardware read as a sensor stuck on one
        // coordinate. Throttling by time instead means a long press costs a few lines and a new
        // position always prints.
        //
        // Both numbers come from the SAME sample: lastRaw*() is what getTouch() just read, not a
        // second I2C transaction, which would pair a live canvas point with a later reading.
        if ((!pressShown || cx != lastCx || cy != lastCy) && now - lastPrint >= 200) {
            lastPrint = now;
            pressShown = true;
            Serial.printf("[touch] panel %u,%u -> canvas %u,%u\n",
                          (unsigned)touchDriver.lastRawX(), (unsigned)touchDriver.lastRawY(),
                          (unsigned)cx, (unsigned)cy);
        }
        lastCx = cx;
        lastCy = cy;
    } else if (pressShown && now - lastPrint >= 200) {
        // Only after a press was actually shown, so a controller that flickers contact/release at the
        // poll rate cannot bury the log in release lines.
        lastPrint = now;
        pressShown = false;
        Serial.println("[touch] released");
    }

    if (now - lastBeat >= 2000) {
        lastBeat = now;
        beats++;
        Serial.printf("[beat %u] heap %u  psram-free %u  polls %u  edges %u  touches %u  up %us\n",
                      (unsigned)beats, (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getFreePsram(),
                      (unsigned)polls, (unsigned)transitions, (unsigned)touches,
                      (unsigned)(now / 1000));
    }

    // A working getTouch() blocks on I2C and yields, but a failed bring-up makes every call return
    // immediately -- and then this loop spins hard enough to starve IDLE1 and trip the task watchdog,
    // filling the log with register dumps that hide the actual problem. One tick per iteration costs
    // nothing next to a 115200-baud poll and keeps the failure readable.
    delay(1);
}

#endif // KRYONOS_KORVO_BRINGUP

// Without the diagnostic there is nothing to put here, and that is the point: the OS's own
// setup()/loop() are in kryonos_app, and the Arduino core's app_main finds them at link time. This
// translation unit exists only to give the main component something to compile.
