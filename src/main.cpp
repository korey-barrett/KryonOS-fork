#include <Arduino.h>
#include <SPI.h>
#include <TFT_eSPI.h>
#include <WiFi.h>
#include "FileSystem/FileSystem.h"
#include "Launcher/LauncherUI.h"
#include "Settings/SettingsUI.h"
#include "Launcher/InstallerUI.h"
#include "Settings/TouchCalibrator.h"
#include "Keyboard/MyKeyboard.h"
#include "WebManager/WebManager.h"
#include "Runtime/JSBindings.h"
#include "Kernel/Core/HarixKernel.h"
#include "WebServerApp/WebServerAppUI.h"
#include "Kernel/TimeManager.h"
#include "Kernel/WiFiManager.h"
#include "Launcher/AppStoreUI.h"
#include "Launcher/HelpCenterUI.h"
#include "Settings/TouchDriver.h"
#include "Kernel/Services/NotificationManager.h"
#include "Kernel/Services/KryonCloud/KryonCloudManager.h"
#include "Kernel/Services/KryonCloud/KryonCloudUI.h"
#include "Kernel/Services/OTA/OTAManager.h"
#include "Hal/Boards/Board.h"
#include "Hal/Display/Display.h"
#include "UI/UiLayout.h"

// Define states
#define STATE_LAUNCHER 0
#define STATE_SETTINGS 1
#define STATE_RUN_APP 2
#define STATE_INSTALLER 3
#define STATE_CALIBRATOR 4
#define STATE_WEB_APP 5
#define STATE_SETTINGS_WIFI 6
#define STATE_SETTINGS_ABOUT 7
#define STATE_SETTINGS_APPS 8
#define STATE_SETTINGS_TIME 9
#define STATE_SETTINGS_TIME_MANUAL 10
#define STATE_UPDATER_BOOT 11
#define STATE_UPDATER_MANUAL 12
#define STATE_APP_STORE 13
#define STATE_HELP_CENTER 14
#define STATE_SETTINGS_WIFI_SAVED 15
#define STATE_KRYON_CLOUD 16
#define STATE_SETTINGS_PERMISSIONS 17

int currentState = STATE_LAUNCHER;

// Shorthand for the current screen metrics. UiLayout::current() lazily derives from Display, so
// this is safe even before UiLayout::begin() runs. See Documentation/Display_Touch_Architecture.md.
static inline const UiMetrics& M() { return UiLayout::current(); }

void setup() {
    Serial.begin(115200);
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
    Serial0.begin(115200);
#endif
    delay(1000);
    Serial.println("\n--- KryonOS Booting ---");
#if defined(ARDUINO_USB_CDC_ON_BOOT) && (ARDUINO_USB_CDC_ON_BOOT == 1)
    Serial0.println("\n--- KryonOS Booting (UART0) ---");
#endif

#if defined(BOARD_HAS_PSRAM)
    if (psramFound()) {
        Serial.printf("[PSRAM] Octal PSRAM Initialized: %u MB Total (%u MB Free)\n", 
                      ESP.getPsramSize() / (1024 * 1024), 
                      ESP.getFreePsram() / (1024 * 1024));
    } else {
        Serial.println("[PSRAM] PSRAM not found or not enabled.");
    }
#endif

    // Init Display & Touch Driver. Display::begin() applies the board profile's rotation so the
    // logical canvas matches KRYONOS_DISPLAY_*. See Documentation/Display_Touch_Architecture.md.
    Display::begin();
    UiLayout::begin();
    TouchDriver::init(&tft);
    Serial.printf("[TOUCH] Driver: %s | canvas %dx%d\n", TouchDriver::driverName(),
                  Display::width(), Display::height());

    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.setTextDatum(MC_DATUM);
    tft.drawString("Booting KryonOS...", Display::centerX(), Display::centerY(), M().fontBody);
    // Every message below is drawn and then waited on -- a file system mount, a WiFi association, the
    // app scan -- and each of those blocks. The main loop, which is the only other caller of
    // present(), has not started yet, so anything not flushed here is overwritten in the cache before
    // it ever reaches the panel. That is why the boot text looked absent on this board when it was
    // there all along. See the note in MyKeyboard::getString().
    tft.present();

    // Initialize File Systems (LittleFS & SD)
    if (!FileSystem::init()) {
        Serial.println("File System Warning: One or more FS failed to mount.");
        tft.drawString("FS Mount Warning!", Display::centerX(), Display::centerY() + 20, M().fontBody);
        tft.present();
        delay(1000);
    }
    
    // Initialize Time Manager
    TimeManager::init();
    
    // Initialize Web Manager (Only if not disabled)
    if (!FileSystem::exists("/local/nowifi.txt")) {
        tft.fillScreen(TFT_BLACK);
        tft.drawString("Connecting WiFi...", Display::centerX(), Display::centerY(), M().fontBody);
        tft.present();
        Serial.println("DEBUG: Starting WebManager...");
        if (WebManager::init()) {
            tft.drawString("WiFi Connected!", Display::centerX(), Display::centerY() - 20, M().fontBody);
            tft.drawString(WebManager::getIPAddress(), Display::centerX(), Display::centerY() + 20, M().fontBody);
            tft.present();
            delay(2000);
        }
        Serial.println("DEBUG: WebManager initialized.");
    } else {
        Serial.println("DEBUG: WebManager Disabled by user (RAM Mode).");
    }
    Serial.printf("DEBUG: Free heap before Kernel: %u\n", ESP.getFreeHeap());

    // Initialize JS Runtime
    Serial.println("DEBUG: Starting HarixKernel...");
    HarixKernel::init(&tft);
    Serial.println("HarixKernel initialized successfully.");
    Serial.printf("DEBUG: Free heap after Kernel: %u\n", ESP.getFreeHeap());

    // Init UI Components
    LauncherUI::init(&tft);
    SettingsUI::init(&tft);
    InstallerUI::init(&tft);
    TouchCalibrator::init(&tft);
    MyKeyboard::init(&tft);
    WebServerAppUI::init(&tft);
    AppStoreUI::init(&tft);
    HelpCenterUI::init(&tft);
    KryonCloudManager::init();
    KryonCloudUI::init(&tft);
    OTAManager::init();

    // Initial App Scan (with loading bar outline)
    Serial.println("DEBUG: Scanning Local Apps...");
    tft.fillScreen(TFT_BLACK);
    tft.drawString("Loading Apps...", Display::centerX(), Display::centerY(), M().fontBody);
    // Loading bar outline: 18px side gutters, 38px below centre (18,198,204,14 at 240x320). The
    // gutters, the offset and the height are all 240x320 pixels, so they follow the text scale --
    // the fill LauncherUI::scanLocalApps() draws into this rect uses the same numbers.
    const int16_t bootScale = (int16_t)M().scale;
    tft.drawRect(18 * bootScale, (int16_t)(Display::centerY() + 38 * bootScale),
                 (int16_t)(Display::width() - 36 * bootScale), (int16_t)(14 * bootScale), TFT_WHITE);
    tft.present();
    LauncherUI::scanLocalApps();
    LauncherUI::needsRescan = false;
    Serial.println("DEBUG: Local Apps Scanned.");

    // Attempt to read touch calibration. A driver that reports absolute coordinates (any
    // capacitive panel, or a board with no touch at all) has nothing to calibrate, so it goes
    // straight to the launcher and /touch_cal_p.bin is simply not consulted.
    Serial.println("DEBUG: Reading CalData...");
    if (!TouchDriver::needsCalibration()) {
        Serial.printf("Touch driver '%s' does not need calibration. Skipping calibrator.\n",
                      TouchDriver::driverName());
        currentState = STATE_LAUNCHER;
    } else {
        uint16_t calData[5];
        if (FileSystem::readCalData(calData)) {
            Serial.printf("Calibration data found: [%u, %u, %u, %u, %u]\n",
                          calData[0], calData[1], calData[2], calData[3], calData[4]);
            TouchDriver::setTouch(calData);
            currentState = STATE_LAUNCHER;
        } else {
            Serial.println("No valid calibration data. Entering touch calibrator.");
            currentState = STATE_CALIBRATOR;
        }
    }
    
    // Check for updates & send zero-auth telemetry ping on boot
    if (currentState == STATE_LAUNCHER && WiFi.status() == WL_CONNECTED) {
        Serial.println("DEBUG: Checking for updates...");
        if (SettingsUI::checkUpdateSilent()) {
            currentState = STATE_UPDATER_BOOT;
        }
        // Send initial dual-mode telemetry ping with dynamic version
        KryonCloudManager::sendBootTelemetry();
    }
    
    Serial.println("DEBUG: Setup complete, entering loop!");
    
    // Initial draw
    if (currentState == STATE_LAUNCHER) {
        LauncherUI::draw();
    } else if (currentState == STATE_UPDATER_BOOT) {
        SettingsUI::drawUpdater(true);
    }
}

int lastState = -1; // To trigger draws on state change

void loop() {

    if (currentState != lastState) {
        if (currentState != STATE_RUN_APP) {
            tft.fillScreen(TFT_BLACK); // Completely wipe screen when changing states!
        }
        int oldState = currentState;
        if (currentState == STATE_LAUNCHER) LauncherUI::draw();
        else if (currentState == STATE_SETTINGS) SettingsUI::draw();
        else if (currentState == STATE_INSTALLER) InstallerUI::draw();
        else if (currentState == STATE_CALIBRATOR) TouchCalibrator::runCalibration();
        else if (currentState == STATE_WEB_APP) WebServerAppUI::draw();
        else if (currentState == STATE_SETTINGS_ABOUT) SettingsUI::drawAbout();
        else if (currentState == STATE_SETTINGS_WIFI) SettingsUI::drawWiFi();
        else if (currentState == STATE_SETTINGS_WIFI_SAVED) SettingsUI::drawSavedNetworks();
        else if (currentState == STATE_SETTINGS_APPS) SettingsUI::drawApps();
        else if (currentState == STATE_SETTINGS_PERMISSIONS) SettingsUI::drawPermissions();
        else if (currentState == STATE_SETTINGS_TIME) SettingsUI::drawTimeSettings();
        else if (currentState == STATE_SETTINGS_TIME_MANUAL) SettingsUI::drawTimeManual();
        else if (currentState == STATE_UPDATER_BOOT) SettingsUI::drawUpdater(true);
        else if (currentState == STATE_UPDATER_MANUAL) SettingsUI::drawUpdater(false);
        else if (currentState == STATE_APP_STORE) AppStoreUI::draw();
        else if (currentState == STATE_HELP_CENTER) HelpCenterUI::draw();
        else if (currentState == STATE_KRYON_CLOUD) {
            KryonCloudUI::resetToHome();
            KryonCloudUI::draw();
        }
        
        // If state changed during drawing, don't set lastState to oldState
        if (currentState == oldState) {
            lastState = currentState;
        } else {
            lastState = -1; // Force next iteration to draw the new state
        }
    }

    if (currentState == STATE_HELP_CENTER) {
        HelpCenterUI::update();
    }
    if (currentState == STATE_KRYON_CLOUD) {
        KryonCloudUI::loop();
    }

    // Verify and confirm stable boot after 10 seconds of runtime
    OTAManager::confirmBootSuccessful();

    // Basic Touch handling loop
    uint16_t x, y;
    bool touched = TouchDriver::getTouch(&x, &y);
    
    static unsigned long lastTouchTime = 0;
    static bool wasTouched = false;

    if (touched) {
        bool processNow = false;
        
        if (!wasTouched) {
            processNow = true;
            lastTouchTime = millis();
        } else {
            // If held down for 300ms, start fast repeat
            if (millis() - lastTouchTime > 300) {
                // Only fast repeat inside the footer strip (UP/DN/SEL buttons).
                if (M().inFooter(y)) {
                    processNow = true;
                    lastTouchTime = millis() - 250; // repeat every 50ms
                }
            }
        }
        
        if (processNow) {
            if (currentState == STATE_LAUNCHER) {
                LauncherUI::handleTouch(x, y);
            } else if (currentState == STATE_SETTINGS) {
                SettingsUI::handleTouch(x, y);
            } else if (currentState == STATE_INSTALLER) {
                InstallerUI::handleTouch(x, y);
            } else if (currentState == STATE_WEB_APP) {
                WebServerAppUI::handleTouch(x, y);
            } else if (currentState == STATE_SETTINGS_ABOUT) {
                SettingsUI::handleAboutTouch(x, y);
            } else if (currentState == STATE_SETTINGS_WIFI) {
                SettingsUI::handleWiFiTouch(x, y);
            } else if (currentState == STATE_SETTINGS_WIFI_SAVED) {
                SettingsUI::handleSavedNetworksTouch(x, y);
            } else if (currentState == STATE_SETTINGS_APPS) {
                SettingsUI::handleAppsTouch(x, y);
            } else if (currentState == STATE_SETTINGS_PERMISSIONS) {
                SettingsUI::handlePermissionsTouch(x, y);
            } else if (currentState == STATE_SETTINGS_TIME) {
                SettingsUI::handleTimeTouch(x, y);
            } else if (currentState == STATE_SETTINGS_TIME_MANUAL) {
                SettingsUI::handleTimeManualTouch(x, y);
            } else if (currentState == STATE_UPDATER_BOOT || currentState == STATE_UPDATER_MANUAL) {
                SettingsUI::handleUpdaterTouch(x, y);
            } else if (currentState == STATE_APP_STORE) {
                AppStoreUI::handleTouch(x, y);
            } else if (currentState == STATE_HELP_CENTER) {
                HelpCenterUI::handleTouch(x, y);
            } else if (currentState == STATE_KRYON_CLOUD) {
                KryonCloudUI::handleTouch(x, y);
            } else if (currentState == STATE_RUN_APP) {
                // Check if user touched the top-right "X" button. The 10px of extra vertical
                // tolerance matches the historical `y <= 40` test at 240x320.
                const UiRect& ex = M().appExitButton;
                if (x >= ex.x && y <= ex.h + 10) {
                    currentState = STATE_LAUNCHER; // Exit app
                }
            }
        }
        wasTouched = true;
    } else {
        wasTouched = false;
    }

    // Check for Serial Monitor commands (e.g. typing 'cal' triggers fresh calibration)
    if (Serial.available()) {
        String cmd = Serial.readStringUntil('\n');
        cmd.trim();
        if (cmd.equalsIgnoreCase("cal") || cmd.equalsIgnoreCase("calibrate") || cmd.equalsIgnoreCase("recal")) {
            Serial.println("[COMMAND] Erasing calibration and starting Touch Calibrator...");
            LittleFS.remove("/touch_cal_p.bin");
            currentState = STATE_CALIBRATOR;
            lastState = -1;
        } else if (cmd.equalsIgnoreCase("reboot") || cmd.equalsIgnoreCase("restart")) {
            Serial.println("[COMMAND] Rebooting system...");
            delay(100);
            ESP.restart();
        } else if (cmd.equalsIgnoreCase("info")) {
            Serial.printf("[INFO] Free Heap: %u bytes, Free PSRAM: %u bytes / %u total\n",
                          ESP.getFreeHeap(), ESP.getFreePsram(), ESP.getPsramSize());
        } else if (cmd.equalsIgnoreCase("layout")) {
            // Dump the live layout so it can be diffed against tools/preview/layout_model.py.
            const UiMetrics& m = M();
            Serial.printf("[LAYOUT] board=%s canvas=%dx%d rotation=%d landscape=%d\n",
                          Display::boardId(), Display::width(), Display::height(),
                          Display::rotation(), m.landscape ? 1 : 0);
            Serial.printf("[LAYOUT] macros=%dx%d rot=%d panel=%dx%d\n",
                          KRYONOS_DISPLAY_WIDTH, KRYONOS_DISPLAY_HEIGHT, KRYONOS_DISPLAY_ROTATION,
                          tft.width(), tft.height());
            Serial.printf("[LAYOUT] inset=%u frame=%d,%d,%d,%d header=%d,%d,%d,%d headerTextY=%d\n",
                          m.inset, m.frame.x, m.frame.y, m.frame.w, m.frame.h,
                          m.header.x, m.header.y, m.header.w, m.header.h, m.headerTextY);
            Serial.printf("[LAYOUT] list=%d,%d,%d,%d footer=%d,%d,%d,%d footerTextY=%d\n",
                          m.list.x, m.list.y, m.list.w, m.list.h,
                          m.footer.x, m.footer.y, m.footer.w, m.footer.h, m.footerTextY);
            Serial.printf("[LAYOUT] rowH=%d rowFillH=%d itemsPerPage=%d scrollX=%d,%d thumbMin=%d\n",
                          m.rowH, m.rowFillH, m.itemsPerPage, m.scrollX, m.scrollW, m.scrollThumbMin);
            Serial.printf("[LAYOUT] center=%d,%d appExit=%d,%d,%d,%d fonts=%d/%d/%d\n",
                          m.centerX, m.centerY, m.appExitButton.x, m.appExitButton.y,
                          m.appExitButton.w, m.appExitButton.h,
                          m.fontSmall, m.fontBody, m.fontHeader);
            Serial.printf("[LAYOUT] card=%d,%d %dx%d r=%d restingY=%d hiddenY=%d shadow=%dx%d\n",
                          m.cardX, m.restingY, m.cardW, m.cardH, m.cardR, m.restingY, m.hiddenY,
                          m.shadowW, m.shadowH);
            Serial.printf("[LAYOUT] kb textBox=%d,%d,%d,%d buttonRow=%d,%d,%d,%d buttonW=%d "
                          "gridTop=%d keys=%dx%d %dx%d\n",
                          m.kbTextBox.x, m.kbTextBox.y, m.kbTextBox.w, m.kbTextBox.h,
                          m.kbButtonRow.x, m.kbButtonRow.y, m.kbButtonRow.w, m.kbButtonRow.h,
                          m.kbButtonW, m.kbGridTop, m.kbKeyW, m.kbKeyH, m.kbCols, m.kbRows);
            const char* fname[3] = { "UP", "SEL", "DN" };
            for (int i = UI_FOOTER_UP; i <= UI_FOOTER_DN; i++) {
                UiRect r = m.footerButton(i);
                Serial.printf("[LAYOUT] footer[%s]=%d,%d,%d,%d centerX=%d\n",
                              fname[i], r.x, r.y, r.w, r.h, m.footerButtonCenterX(i));
            }
        } else if (cmd.equalsIgnoreCase("help")) {
            Serial.println("--- KryonOS Serial Commands ---");
            Serial.println("  cal / calibrate : Launch fresh touch calibration");
            Serial.println("  info            : View memory & system diagnostics");
            Serial.println("  layout          : Dump live screen metrics (see tools/preview/)");
            Serial.println("  reboot          : Reboot the ESP32");
        }
    }

    // Background WiFi monitoring and auto-reconnect
    WiFiManager::backgroundLoop();

    // Floating Notification compositor overlay
    NotificationManager::updateAndRender(&tft);

    // Flush any pending frame. A no-op on the TFT_eSPI backend, which writes straight to the panel;
    // on the RGB backend the panel's DMA reads the frame buffer out of physical PSRAM, so this is
    // what writes the CPU's cached drawing back to it.
    tft.present();

    // Yield to let ESP32 handle background tasks
    delay(10);
}
