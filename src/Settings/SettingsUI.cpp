#include "SettingsUI.h"
#include "TouchDriver.h"
#include <SD.h>
#include <LittleFS.h>
#include "../FileSystem/FileSystem.h"
#include "../Kernel/TimeManager.h"
#include "../Keyboard/MyKeyboard.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <esp_task_wdt.h>
#include "../Kernel/WiFiManager.h"
#include "../Kernel/Services/OTA/OTAManager.h"
#include "../Kernel/Services/Network/TLSHelper.h"
#include "../Runtime/JSBindings.h"
#include "../UI/UiLayout.h"

// Current screen metrics. See Documentation/Display_Touch_Architecture.md. This file has a dozen
// sub-screens that each repeat the same frame/header/footer geometry; M() keeps them consistent.
static inline const UiMetrics& M() { return UiLayout::current(); }

KryonDisplay *SettingsUI::tftInstance = nullptr;
bool SettingsUI::otaErrorShown = false;
bool showResetDialog = false;

void SettingsUI::init(KryonDisplay *tft) {
    tftInstance = tft;
}

// --- shared sub-screen chrome ------------------------------------------------------------------
// Every Settings sub-screen paints the same three pieces: a rounded frame, a titled header bar, and
// a footer holding one centred action. They were copy-pasted into each of the dozen screens; drawing
// them from one place keeps the screens aligned and leaves each one spelling out only what is
// actually different about it.
static void drawSettingsBackdrop(KryonDisplay* t, uint16_t borderColor = TFT_GREEN) {
    const UiMetrics& m = M();
    t->fillScreen(TFT_BLACK);
    t->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);
    t->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
    t->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, borderColor);
}

static void drawSettingsFrame(KryonDisplay* t, const char* title,
                              uint16_t borderColor = TFT_GREEN, uint16_t titleColor = TFT_GREEN) {
    const UiMetrics& m = M();
    drawSettingsBackdrop(t, borderColor);
    t->setTextColor(titleColor, TFT_BLACK);
    t->setTextDatum(MC_DATUM);
    t->drawString(title, m.header.cx(), m.headerTextY, m.fontBody);
}

static void drawSettingsFooter(KryonDisplay* t, const char* label) {
    const UiMetrics& m = M();
    t->drawRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_WHITE);
    t->setTextColor(TFT_WHITE, TFT_BLACK);
    t->setTextDatum(MC_DATUM);
    t->drawString(label, m.footerButtonCenterX(UI_FOOTER_SEL), m.footerTextY, m.fontBody);
}

// Same footer, but for the screens whose body is a scrollable list: the UP and DN thirds move the
// selection. Without them a touch-only device could never reach an item that fell off a short panel.
static void drawSettingsFooterScroll(KryonDisplay* t, const char* label) {
    const UiMetrics& m = M();
    t->drawRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_WHITE);
    t->setTextColor(TFT_WHITE, TFT_BLACK);
    t->setTextDatum(MC_DATUM);
    t->drawString("UP",  m.footerButtonCenterX(UI_FOOTER_UP),  m.footerTextY, m.fontBody);
    t->drawString(label, m.footerButtonCenterX(UI_FOOTER_SEL), m.footerTextY, m.fontBody);
    t->drawString("DN",  m.footerButtonCenterX(UI_FOOTER_DN),  m.footerTextY, m.fontBody);
}

// Scroll the window so `selected` is visible, clamped to the ends of the list. Shared by every
// Settings screen that shows a list, so UP / DN and the draw agree on what is on screen.
static int clampScroll(int selected, int scroll, int count, int perPage) {
    if (perPage < 1) return 0;
    if (selected < scroll)               scroll = selected;
    if (selected >= scroll + perPage)    scroll = selected - perPage + 1;
    if (scroll > count - perPage)        scroll = count - perPage;
    if (scroll < 0)                      scroll = 0;
    return scroll;
}

// Same window bounds, for lists that scroll without a cursor (the saved-network cards): there is no
// selection to follow, so the window just has to stay inside [0, count - perPage].
static int clampScrollWindow(int scroll, int count, int perPage) {
    return clampScroll(scroll, scroll, count, perPage);
}

String formatBytes(uint64_t bytes) {
    if (bytes < 1024) return String((uint32_t)bytes) + " B";
    else if (bytes < (1024 * 1024)) return String((uint32_t)(bytes / 1024)) + " KB";
    else if (bytes < (1024 * 1024 * 1024)) return String((uint32_t)(bytes / (1024 * 1024))) + " MB";
    else return String((uint32_t)(bytes / (1024 * 1024 * 1024))) + " GB";
}

// ----------------------------------------------------
// MAIN SETTINGS MENU
// ----------------------------------------------------

// The menu is a table rather than a run of near-identical draw calls, so the entry a tap resolves to
// and the entry that gets painted come from the same row of data. `state` is the launcher state the
// entry switches to.
namespace {
struct SettingsMenuItem {
    const char* label;
    uint16_t    bg;
    uint16_t    fg;
    int         state;
};

const SettingsMenuItem kSettingsMenu[] = {
    { "WiFi Options",        TFT_BLUE,     TFT_WHITE, 6  }, // STATE_SETTINGS_WIFI
    { "Touch Calibrator",    TFT_ORANGE,   TFT_WHITE, 4  }, // STATE_CALIBRATOR
    { "Manage Apps",         TFT_PURPLE,   TFT_WHITE, 8  }, // STATE_SETTINGS_APPS
    { "Permissions Manager", 0x03E0,       TFT_WHITE, 17 }, // STATE_SETTINGS_PERMISSIONS
    { "Time & Region",       TFT_CYAN,     TFT_BLACK, 9  }, // STATE_SETTINGS_TIME
    { "About Device",        TFT_DARKGREY, TFT_WHITE, 7  }, // STATE_SETTINGS_ABOUT
    { "System Updates",      TFT_RED,      TFT_WHITE, 12 }, // STATE_UPDATER_MANUAL
};
const int kSettingsMenuCount = (int)(sizeof(kSettingsMenu) / sizeof(kSettingsMenu[0]));

int settingsMenuSelected = 0;
int settingsMenuScroll   = 0;
} // namespace

void SettingsUI::draw() {
    if (!tftInstance) return;
    const UiMetrics& m = M();

    drawSettingsFrame(tftInstance, "Settings Menu");

    // Seven entries over 30px rows need 210px; they all fit at 240x320, but a short panel shows
    // fewer and has to scroll. The list grid is the same one Launcher / App Store / Help Center use.
    const int perPage = m.itemsPerPage;
    settingsMenuScroll = clampScroll(settingsMenuSelected, settingsMenuScroll,
                                     kSettingsMenuCount, perPage);

    for (int i = 0; i < perPage; i++) {
        const int idx = settingsMenuScroll + i;
        if (idx >= kSettingsMenuCount) break;

        const UiRect fill = m.listRowFillRect(i);
        tftInstance->fillRoundRect(fill.x, fill.y, fill.w, fill.h, 4, kSettingsMenu[idx].bg);
        if (idx == settingsMenuSelected) {
            // The entries are already colour-coded, so selection reads as a white outline rather
            // than an inverted fill.
            tftInstance->drawRoundRect(fill.x, fill.y, fill.w, fill.h, 4, TFT_WHITE);
        }
        tftInstance->setTextColor(kSettingsMenu[idx].fg, kSettingsMenu[idx].bg);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString(kSettingsMenu[idx].label, fill.cx(), m.listRowTextY(i), m.fontBody);
    }

    if (kSettingsMenuCount > perPage) {
        const int thumbH = max((int)m.scrollThumbMin, (m.list.h * perPage) / kSettingsMenuCount);
        const int thumbY = m.list.y + (settingsMenuScroll * (m.list.h - thumbH)) /
                                         (kSettingsMenuCount - perPage);
        tftInstance->fillRect(m.scrollX, m.list.y, m.scrollW, m.list.h, TFT_DARKGREY);
        tftInstance->fillRect(m.scrollX, thumbY, m.scrollW, thumbH, TFT_WHITE);
    }

    drawSettingsFooterScroll(tftInstance, "EXIT");
}

void SettingsUI::handleTouch(uint16_t x, uint16_t y) {
    extern int currentState;
    const UiMetrics& m = M();

    // A tap activates the entry directly — the highlight exists for the UP / DN keys, not as a
    // select-then-confirm step.
    const int rowIndex = m.listRowFromY((int16_t)y);
    if (rowIndex >= 0 && x >= m.list.x && x < m.list.x + m.list.w) {
        const int idx = settingsMenuScroll + rowIndex;
        if (idx < kSettingsMenuCount) {
            settingsMenuSelected = idx;
            currentState = kSettingsMenu[idx].state;
            return;
        }
    }

    if (!m.inFooter((int16_t)y)) return;
    switch (m.footerButtonFromX((int16_t)x)) {
    case UI_FOOTER_UP:
        if (settingsMenuSelected > 0) { settingsMenuSelected--; draw(); }
        break;
    case UI_FOOTER_DN:
        if (settingsMenuSelected < kSettingsMenuCount - 1) { settingsMenuSelected++; draw(); }
        break;
    default: // UI_FOOTER_SEL — EXIT
        currentState = 0; // STATE_LAUNCHER
        break;
    }
}

// ----------------------------------------------------
// WIFI OPTIONS MENU
// ----------------------------------------------------

// ----------------------------------------------------
// WIFI OPTIONS
// ----------------------------------------------------

static int wifiActionScroll = 0;

// Line step for a body-text block: one and a half rows, so a card of N lines scales with the panel.
// Shared by the WiFi status card and the Permissions empty-state card.
static int16_t bodyLineStep(const UiMetrics& m) { return (int16_t)(m.rowH * 2 / 3); }

namespace {
int16_t wifiCardH(const UiMetrics& m) { return (int16_t)(3 * bodyLineStep(m) + 18); }

UiRect wifiStatusCard(const UiMetrics& m) {
    return { m.list.x, (int16_t)(m.header.y + m.header.h + 4), m.list.w, wifiCardH(m) };
}

// The action buttons fill whatever is left between the card and the footer.
int16_t wifiActionTop(const UiMetrics& m) {
    const UiRect card = wifiStatusCard(m);
    return (int16_t)(card.y + card.h + 6);
}

int16_t wifiActionH(const UiMetrics& m)     { return (int16_t)(m.rowH + 4); }
int16_t wifiActionPitch(const UiMetrics& m) { return (int16_t)(wifiActionH(m) + 6); }

int wifiActionsPerPage(const UiMetrics& m) {
    const int pitch = wifiActionPitch(m);
    if (pitch <= 0) return 1;
    const int avail = (m.footer.y - 7) - wifiActionTop(m);
    const int n = (avail - wifiActionH(m)) / pitch + 1;
    return (n < 1) ? 1 : n;
}

UiRect wifiActionButton(const UiMetrics& m, int visibleIndex) {
    return { (int16_t)(m.list.x + 2),
             (int16_t)(wifiActionTop(m) + visibleIndex * wifiActionPitch(m)),
             (int16_t)(m.list.w - 4), wifiActionH(m) };
}
} // namespace

void SettingsUI::drawWiFi() {
    const UiMetrics& m = M();
    drawSettingsFrame(tftInstance, "WiFi Options");

    const bool enabled   = WiFiManager::isEnabled();
    const bool connected = WiFiManager::isConnected();
    const bool online    = WiFiManager::hasInternet();

    // Live status card
    const UiRect card  = wifiStatusCard(m);
    const int16_t step = bodyLineStep(m);
    tftInstance->fillRoundRect(card.x, card.y, card.w, card.h, 6, 0x10A2); // Dark navy
    tftInstance->drawRoundRect(card.x, card.y, card.w, card.h, 6, TFT_CYAN);

    const int16_t textX  = (int16_t)(card.x + 8);
    const int16_t line1Y = (int16_t)(card.y + 8);
    const int16_t line2Y = (int16_t)(line1Y + step);
    const int16_t line3Y = (int16_t)(line2Y + step);

    tftInstance->setTextDatum(TL_DATUM);
    if (!enabled) {
        tftInstance->setTextColor(TFT_DARKGREY, 0x10A2);
        tftInstance->drawString("Status: DISABLED", textX, line1Y, m.fontBody);
        tftInstance->drawString("WiFi radio is turned off", textX, line2Y, m.fontBody);
        tftInstance->drawString("to conserve battery/RAM.", textX, line3Y, m.fontBody);
    } else if (!connected) {
        tftInstance->setTextColor(TFT_RED, 0x10A2);
        tftInstance->drawString("Status: DISCONNECTED", textX, line1Y, m.fontBody);
        tftInstance->setTextColor(TFT_WHITE, 0x10A2);
        tftInstance->drawString("No network connected", textX, line2Y, m.fontBody);
        tftInstance->drawString("Scan to find networks", textX, line3Y, m.fontBody);
    } else {
        tftInstance->setTextColor(online ? TFT_GREEN : TFT_ORANGE, 0x10A2);
        tftInstance->drawString(online ? "Status: ONLINE" : "Status: LOCAL ONLY", textX, line1Y, m.fontBody);

        // Signal bars, pinned to the right edge of the card
        const int bars = WiFiManager::getSignalBars();
        const int sx = (int)(card.right() - 35);
        const int sy = (int)(card.y + 22);
        for (int b = 1; b <= 4; b++) {
            const uint16_t bColor = (b <= bars) ? (online ? TFT_GREEN : TFT_ORANGE) : TFT_DARKGREY;
            tftInstance->fillRect(sx + (b - 1) * 6, sy - (b * 3), 4, b * 3, bColor);
        }

        tftInstance->setTextColor(TFT_WHITE, 0x10A2);
        String ssid = WiFiManager::getSSID();
        if (ssid.length() > 16) ssid = ssid.substring(0, 14) + "..";
        tftInstance->drawString("SSID: " + ssid, textX, line2Y, m.fontBody);
        tftInstance->drawString("IP:   " + WiFiManager::getIP(), textX, line3Y, m.fontBody);
    }

    // The toggle is always available; the other three actions only exist while the radio is on.
    struct WifiAction { String label; uint16_t bg; };
    WifiAction actions[4];
    int actionCount = 0;
    actions[actionCount++] = { enabled ? String("WiFi: ON (Tap to Disable)")
                                       : String("WiFi: OFF (Tap to Enable)"),
                               (uint16_t)(enabled ? TFT_BLUE : TFT_DARKGREY) };
    if (enabled) {
        actions[actionCount++] = { String("Scan Nearby Networks"), (uint16_t)TFT_PURPLE };
        actions[actionCount++] = { "Saved Networks (" + String((int)WiFiManager::getSavedNetworks().size()) + ")",
                                   (uint16_t)0x03E0 /* Forest Green */ };
        actions[actionCount++] = { String("Web Server"), (uint16_t)TFT_ORANGE };
    }

    const int perPage = wifiActionsPerPage(m);
    wifiActionScroll = clampScrollWindow(wifiActionScroll, actionCount, perPage);

    for (int i = 0; i < perPage; i++) {
        const int idx = wifiActionScroll + i;
        if (idx >= actionCount) break;
        const UiRect btn = wifiActionButton(m, i);
        tftInstance->fillRoundRect(btn.x, btn.y, btn.w, btn.h, 5, actions[idx].bg);
        tftInstance->setTextColor(TFT_WHITE, actions[idx].bg);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString(actions[idx].label, btn.cx(), btn.cy(), m.fontBody);
    }

    if (actionCount > perPage) {
        const int thumbH = max((int)m.scrollThumbMin, (m.list.h * perPage) / actionCount);
        const int thumbY = m.list.y + (wifiActionScroll * (m.list.h - thumbH)) / (actionCount - perPage);
        tftInstance->fillRect(m.scrollX, m.list.y, m.scrollW, m.list.h, TFT_DARKGREY);
        tftInstance->fillRect(m.scrollX, thumbY, m.scrollW, thumbH, TFT_WHITE);
    }

    if (actionCount > perPage) drawSettingsFooterScroll(tftInstance, "BACK");
    else                       drawSettingsFooter(tftInstance, "BACK");
}

void SettingsUI::handleWiFiTouch(uint16_t x, uint16_t y) {
    extern int currentState;
    const UiMetrics& m = M();

    const bool enabled = WiFiManager::isEnabled();
    const int  actionCount = enabled ? 4 : 1;
    const int  perPage = wifiActionsPerPage(m);

    // Walk the same rects drawWiFi() laid out — only the visible buttons can be hit.
    for (int i = 0; i < perPage; i++) {
        const int idx = wifiActionScroll + i;
        if (idx >= actionCount) break;
        const UiRect btn = wifiActionButton(m, i);
        if (!btn.contains((int16_t)x, (int16_t)y)) continue;

        if (idx == 0) {
            // WiFi ON/OFF toggle
            WiFiManager::setEnabled(!enabled);
            if (!enabled) {
                // Turning ON -> if there are no saved networks, scan immediately
                if (WiFiManager::getSavedNetworks().empty()) {
                    scanAndConnectWiFi();
                    return;
                }
                WiFiManager::smartAutoConnect();
            }
            drawWiFi();
            return;
        }
        if (idx == 1) { scanAndConnectWiFi(); return; }
        if (idx == 2) { currentState = 15; /* STATE_SETTINGS_WIFI_SAVED */ drawSavedNetworks(); return; }
        currentState = 5; // STATE_WEB_APP
        return;
    }

    if (!m.inFooter((int16_t)y)) return;
    switch (m.footerButtonFromX((int16_t)x)) {
    case UI_FOOTER_UP:
        if (wifiActionScroll > 0) { wifiActionScroll--; drawWiFi(); }
        break;
    case UI_FOOTER_DN:
        if (wifiActionScroll < actionCount - perPage) { wifiActionScroll++; drawWiFi(); }
        break;
    default: // UI_FOOTER_SEL — BACK
        currentState = 1; // STATE_SETTINGS
        break;
    }
}

// ----------------------------------------------------
// SAVED NETWORKS MANAGEMENT MENU
// ----------------------------------------------------

static int savedNetScroll = 0;

// The saved-network list is a stack of cards rather than single-line rows: each card carries the
// SSID, a status badge and a Forget button. Card height is derived from the row height so the list
// still degrades sensibly on a short panel.
namespace {
int16_t savedCardH(const UiMetrics& m) { return (int16_t)(m.rowH + 22); }
int16_t savedCardPitch(const UiMetrics& m) { return (int16_t)(savedCardH(m) + 6); }

int16_t savedCardY(const UiMetrics& m, int visibleIndex) {
    return (int16_t)(m.list.y + visibleIndex * savedCardPitch(m));
}

// As many cards as fit: the last one needs (n-1) pitches plus its own height, not n pitches.
int savedCardsPerPage(const UiMetrics& m) {
    const int pitch = savedCardPitch(m);
    if (pitch <= 0) return 1;
    const int n = (m.list.h - savedCardH(m)) / pitch + 1;
    return (n < 1) ? 1 : n;
}

UiRect savedForgetButton(const UiMetrics& m, int16_t cardY) {
    return { (int16_t)(m.list.x + m.list.w - 75), (int16_t)(cardY + 10), 65, 32 };
}
} // namespace

void SettingsUI::drawSavedNetworks() {
    const UiMetrics& m = M();
    auto saved = WiFiManager::getSavedNetworks();
    const int count = (int)saved.size();

    drawSettingsFrame(tftInstance, "Saved Networks");

    const int perPage = savedCardsPerPage(m);
    if (count == 0) {
        tftInstance->setTextColor(TFT_DARKGREY, TFT_BLACK);
        tftInstance->setTextDatum(MC_DATUM);
        const int16_t line1 = (int16_t)(m.list.y + m.list.h / 2 - 12);
        tftInstance->drawString("No saved networks.", m.centerX, line1, m.fontBody);
        tftInstance->drawString("Scan and connect to add!", m.centerX, (int16_t)(line1 + 25), m.fontBody);
    } else {
        savedNetScroll = clampScrollWindow(savedNetScroll, count, perPage);
        String curSSID = WiFiManager::getSSID();
        const int16_t cardH = savedCardH(m);

        for (int i = 0; i < perPage; i++) {
            const int idx = savedNetScroll + i;
            if (idx >= count) break;

            const auto& net = saved[idx];
            bool isCurrent = WiFiManager::isConnected() && net.ssid.equalsIgnoreCase(curSSID);
            const int16_t cardY = savedCardY(m, i);
            const uint16_t cardBg = isCurrent ? 0x02E0 : 0x18C3;

            tftInstance->fillRoundRect(m.list.x, cardY, m.list.w, cardH, 5, cardBg);
            tftInstance->drawRoundRect(m.list.x, cardY, m.list.w, cardH, 5,
                                       isCurrent ? TFT_GREEN : TFT_WHITE);

            tftInstance->setTextDatum(TL_DATUM);
            tftInstance->setTextColor(TFT_WHITE, cardBg);
            String displaySSID = net.ssid;
            if (displaySSID.length() > 14) displaySSID = displaySSID.substring(0, 12) + "..";
            tftInstance->drawString(displaySSID, (int16_t)(m.list.x + 8), (int16_t)(cardY + 8), m.fontBody);

            if (isCurrent) {
                tftInstance->setTextColor(TFT_GREEN, cardBg);
                tftInstance->drawString("Connected", (int16_t)(m.list.x + 8), (int16_t)(cardY + 30), m.fontBody);
            } else {
                tftInstance->setTextColor(TFT_CYAN, cardBg);
                tftInstance->drawString("Tap to Connect", (int16_t)(m.list.x + 8), (int16_t)(cardY + 30), m.fontBody);
            }

            const UiRect forget = savedForgetButton(m, cardY);
            tftInstance->fillRoundRect(forget.x, forget.y, forget.w, forget.h, 4, TFT_RED);
            tftInstance->setTextColor(TFT_WHITE, TFT_RED);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("Forget", forget.cx(), forget.cy(), m.fontBody);
        }

        if (count > perPage) {
            const int thumbH = max((int)m.scrollThumbMin, (m.list.h * perPage) / count);
            const int thumbY = m.list.y + (savedNetScroll * (m.list.h - thumbH)) / (count - perPage);
            tftInstance->fillRect(m.scrollX, m.list.y, m.scrollW, m.list.h, TFT_DARKGREY);
            tftInstance->fillRect(m.scrollX, thumbY, m.scrollW, thumbH, TFT_WHITE);
        }
    }

    // UP / DN only appear when there is actually something off-screen to scroll to.
    if (count > perPage) drawSettingsFooterScroll(tftInstance, "BACK");
    else                 drawSettingsFooter(tftInstance, "BACK");
}

void SettingsUI::handleSavedNetworksTouch(uint16_t x, uint16_t y) {
    extern int currentState;
    const UiMetrics& m = M();
    auto saved = WiFiManager::getSavedNetworks();
    const int count = (int)saved.size();
    const int perPage = savedCardsPerPage(m);

    for (int i = 0; i < perPage && count > 0; i++) {
        const int idx = savedNetScroll + i;
        if (idx >= count) break;

        const int16_t cardY = savedCardY(m, i);
        const UiRect forget = savedForgetButton(m, cardY);

        // Forget button (checked before the card body, which it sits on top of)
        if (forget.contains((int16_t)x, (int16_t)y)) {
            String toForget = saved[idx].ssid;
            WiFiManager::forgetNetwork(toForget);
            if (savedNetScroll >= count - 1) savedNetScroll = max(0, savedNetScroll - 1);

            tftInstance->fillScreen(TFT_BLACK);
            tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("Network Forgot!", m.centerX, m.centerY, m.fontBody);
            delay(800);
            drawSavedNetworks();
            return;
        }

        // Card body (excluding the Forget button) connects.
        const UiRect body = { m.list.x, cardY, (int16_t)(forget.x - m.list.x), savedCardH(m) };
        if (body.contains((int16_t)x, (int16_t)y)) {
            String toConnect = saved[idx].ssid;
            String pass = saved[idx].password;

            tftInstance->fillScreen(TFT_BLACK);
            tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("Connecting to", m.centerX, (int16_t)(m.list.y + m.list.h / 2 - 12), m.fontBody);
            tftInstance->drawString(toConnect + "...", m.centerX, (int16_t)(m.list.y + m.list.h / 2 + 13), m.fontBody);
            // The association below blocks for up to ten seconds, and this runs inside the main loop's
            // touch handling, so the loop cannot present this screen. See MyKeyboard::getString().
            tftInstance->present();

            bool success = WiFiManager::connectTo(toConnect, pass, 10000);
            tftInstance->fillScreen(TFT_BLACK);
            tftInstance->setTextColor(success ? TFT_GREEN : TFT_RED, TFT_BLACK);
            tftInstance->drawString(success ? "Connected!" : "Connection Failed", m.centerX, m.centerY, m.fontBody);
            delay(1000);
            drawSavedNetworks();
            return;
        }
    }

    if (!m.inFooter((int16_t)y)) return;
    switch (m.footerButtonFromX((int16_t)x)) {
    case UI_FOOTER_UP:
        if (savedNetScroll > 0) { savedNetScroll--; drawSavedNetworks(); }
        break;
    case UI_FOOTER_DN:
        if (savedNetScroll < count - perPage) { savedNetScroll++; drawSavedNetworks(); }
        break;
    default: // UI_FOOTER_SEL — BACK
        currentState = 6; // STATE_SETTINGS_WIFI
        drawWiFi();
        break;
    }
}

// ----------------------------------------------------
// ABOUT DEVICE MENU & GITHUB COMMUNITY
// ----------------------------------------------------

static int s_cachedGitHubStars = -1;
static bool s_lastFetchLive = false;
static unsigned long s_lastFetchTime = 0;

static int loadCachedStars() {
    if (s_cachedGitHubStars > 0) return s_cachedGitHubStars;
    if (LittleFS.exists("/local/system/stars_cache.json")) {
        File f = LittleFS.open("/local/system/stars_cache.json", "r");
        if (f) {
            JsonDocument doc;
            DeserializationError err = deserializeJson(doc, f);
            f.close();
            if (!err) {
                if (doc.containsKey("count")) {
                    s_cachedGitHubStars = doc["count"].as<int>();
                } else if (doc.containsKey("stargazers_count")) {
                    s_cachedGitHubStars = doc["stargazers_count"].as<int>();
                }
            }
        }
    }
    if (s_cachedGitHubStars <= 0) {
        s_cachedGitHubStars = 85; // Default fallback baseline
    }
    return s_cachedGitHubStars;
}

static void saveCachedStars(int stars) {
    if (stars <= 0) return;
    s_cachedGitHubStars = stars;
    if (!LittleFS.exists("/local/system")) {
        LittleFS.mkdir("/local/system");
    }
    File f = LittleFS.open("/local/system/stars_cache.json", FILE_WRITE);
    if (f) {
        JsonDocument doc;
        doc["count"] = stars;
        serializeJson(doc, f);
        f.close();
    }
}

static bool fetchGitHubStarsLive() {
    if (WiFi.status() != WL_CONNECTED) {
        s_lastFetchLive = false;
        return false;
    }

    String starsUrl = "https://api.github.com/repos/Haris16-code/KryonOS/stargazers/count";
    WiFiClientSecure client;
    TLSHelper::configureTLS(client, starsUrl);
    client.setTimeout(2500);

    HTTPClient http;
    http.setTimeout(2500);

    if (!http.begin(client, starsUrl)) {
        s_lastFetchLive = false;
        return false;
    }

    http.setUserAgent("KryonOS-Device");
    http.addHeader("Accept", "application/vnd.github.v3+json");

    int httpCode = http.GET();
    if (httpCode == HTTP_CODE_OK) {
        String payload = http.getString();
        http.end();

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, payload);
        if (!err) {
            int stars = -1;
            if (doc.containsKey("count")) {
                stars = doc["count"].as<int>();
            } else if (doc.containsKey("stargazers_count")) {
                stars = doc["stargazers_count"].as<int>();
            }
            if (stars >= 0) {
                saveCachedStars(stars);
                s_lastFetchLive = true;
                s_lastFetchTime = millis();
                return true;
            }
        }
    } else {
        http.end();
    }

    s_lastFetchLive = false;
    return false;
}

static void drawMiniStar(KryonDisplay *tft, int cx, int cy, uint16_t color) {
    if (!tft) return;
    tft->fillTriangle(cx, cy - 5, cx - 2, cy + 3, cx + 2, cy + 3, color);
    tft->fillTriangle(cx - 5, cy - 2, cx + 5, cy - 2, cx, cy + 3, color);
    tft->fillTriangle(cx - 3, cy + 2, cx + 3, cy + 2, cx, cy - 4, color);
}

static void drawMiniHeart(KryonDisplay *tft, int cx, int cy, uint16_t color) {
    if (!tft) return;
    tft->fillCircle(cx - 2, cy - 2, 2, color);
    tft->fillCircle(cx + 2, cy - 2, 2, color);
    tft->fillTriangle(cx - 4, cy - 1, cx + 4, cy - 1, cx, cy + 4, color);
}

static void drawMiniSparkle(KryonDisplay *tft, int cx, int cy, uint16_t color) {
    if (!tft) return;
    tft->fillTriangle(cx, cy - 4, cx - 3, cy, cx + 3, cy, color);
    tft->fillTriangle(cx, cy + 4, cx - 3, cy, cx + 3, cy, color);
    tft->drawPixel(cx, cy, TFT_WHITE);
}

namespace {
// About Device is two stacked cards: hardware/storage, then community. The first is sized by its
// four rows; the second fills whatever is left above the Reset button.
int16_t aboutCard1H(const UiMetrics& m) { return (int16_t)(6 + 4 * bodyLineStep(m) + 6); }

UiRect aboutCard1(const UiMetrics& m) {
    return { m.list.x, m.header.bottom(), m.list.w, aboutCard1H(m) };
}

UiRect aboutResetButton(const UiMetrics& m) {
    const int16_t w = (int16_t)min((int)160, (int)(m.list.w - 60));
    return { (int16_t)(m.centerX - w / 2), (int16_t)(m.footer.y - 37), w, 28 };
}

UiRect aboutCard2(const UiMetrics& m) {
    const UiRect c1 = aboutCard1(m);
    const int16_t bottom = (int16_t)(aboutResetButton(m).y - 8);
    int16_t h = (int16_t)(bottom - (c1.bottom() + 6));
    if (h < 1) h = 1;
    return { m.list.x, (int16_t)(c1.bottom() + 6), m.list.w, h };
}
} // namespace

void SettingsUI::drawAboutLoading(int percent, const String& statusText) {
    if (!tftInstance) return;

    const UiMetrics& m = M();
    drawSettingsFrame(tftInstance, "About Device");

    // Loading Card
    const UiRect card = { m.list.x, (int16_t)(m.list.y + 40), m.list.w, (int16_t)(m.list.h - 100) };
    tftInstance->fillRoundRect(card.x, card.y, card.w, card.h, 6, 0x10A2); // Dark cyber navy
    tftInstance->drawRoundRect(card.x, card.y, card.w, card.h, 6, TFT_CYAN);

    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->setTextColor(TFT_WHITE, 0x10A2);
    tftInstance->drawString("Loading System Info...", card.cx(), (int16_t)(card.y + card.h / 5),
                            m.fontBody);

    // Progress Bar Outline — inset 14px from each side of the card
    const int16_t barX = (int16_t)(card.x + 14);
    const int16_t barY = (int16_t)(card.y + card.h * 2 / 5);
    const int16_t barW = (int16_t)(card.w - 28);
    const int16_t barH = 16;
    tftInstance->drawRoundRect(barX, barY, barW, barH, 4, TFT_WHITE);
    tftInstance->fillRect((int16_t)(barX + 2), (int16_t)(barY + 2),
                          (int16_t)(barW - 4), (int16_t)(barH - 4), TFT_BLACK);

    // Filled Bar
    const int16_t fillW = (int16_t)((percent * (barW - 4)) / 100);
    if (fillW > 0) {
        tftInstance->fillRect((int16_t)(barX + 2), (int16_t)(barY + 2), fillW,
                              (int16_t)(barH - 4), TFT_GREEN);
    }

    tftInstance->setTextColor(TFT_YELLOW, 0x10A2);
    tftInstance->drawString(statusText.c_str(), card.cx(), (int16_t)(card.y + card.h * 7 / 10),
                            m.fontBody);

    tftInstance->setTextColor(TFT_DARKGREY, 0x10A2);
    tftInstance->drawString(String(percent) + "%", card.cx(), (int16_t)(card.y + card.h * 17 / 20),
                            m.fontBody);

    // Flush, because every caller of this blocks. drawAbout() draws the 25% stage, then fetches from
    // GitHub for seconds, then draws 60% and 90% -- and the main loop cannot present any of them,
    // because it is sitting inside drawAbout(). Without this the only frame that ever reached the
    // panel was the finished one, and the stages in between showed up as whatever the cache happened
    // to evict mid-fetch. See the note in MyKeyboard::getString().
    tftInstance->present();
}

void SettingsUI::drawAbout() {
    if (!tftInstance) return;

    const UiMetrics& m = M();

    // Show initial loading stage
    drawAboutLoading(25, "Reading Hardware & Storage...");

    // Trigger auto-fetch if connected and stale
    if (WiFi.status() == WL_CONNECTED && (!s_lastFetchLive || millis() - s_lastFetchTime > 30000)) {
        drawAboutLoading(60, "Loading...");
        fetchGitHubStarsLive();
        drawAboutLoading(90, "Finalizing System Info...");
    } else {
        drawAboutLoading(70, "Loading Cached Telemetry...");
        loadCachedStars();
        drawAboutLoading(95, "Finalizing System Info...");
    }

    drawSettingsFrame(tftInstance, "About Device");

    // Get Storage Info
    uint64_t fsTotal = LittleFS.totalBytes();
    uint64_t fsUsed = LittleFS.usedBytes();
    uint64_t fsFree = fsTotal - fsUsed;

    uint64_t sdTotal = FileSystem::isSDMounted() ? SD.totalBytes() : 0;
    uint64_t sdUsed = FileSystem::isSDMounted() ? SD.usedBytes() : 0;
    uint64_t sdFree = sdTotal - sdUsed;

    const int16_t step = bodyLineStep(m);

    // 1. Hardware & System Card
    const UiRect c1 = aboutCard1(m);
    tftInstance->fillRoundRect(c1.x, c1.y, c1.w, c1.h, 6, 0x10A2); // Dark cyber navy
    tftInstance->drawRoundRect(c1.x, c1.y, c1.w, c1.h, 6, 0x2945);

    const int16_t c1LabelX = (int16_t)(c1.x + 8);
    int16_t rowY = (int16_t)(c1.y + 6);

    tftInstance->setTextDatum(TL_DATUM);
    tftInstance->setTextColor(TFT_GREEN, 0x10A2);
    tftInstance->drawString(String("KryonOS v") + KRYONOS_VERSION, c1LabelX, rowY, m.fontBody);

    rowY += step;
    tftInstance->setTextColor(TFT_CYAN, 0x10A2);
    tftInstance->drawString("Flash:", c1LabelX, rowY, m.fontBody);
    tftInstance->setTextColor(TFT_WHITE, 0x10A2);
    tftInstance->drawString(formatBytes(fsTotal) + " (" + formatBytes(fsFree) + " free)",
                            (int16_t)(c1.x + 52), rowY, m.fontBody);

    rowY += step;
    tftInstance->setTextColor(TFT_ORANGE, 0x10A2);
    tftInstance->drawString("SD:", c1LabelX, rowY, m.fontBody);
    tftInstance->setTextColor(sdTotal > 0 ? TFT_WHITE : TFT_RED, 0x10A2);
    String sdStr = (sdTotal > 0) ? (formatBytes(sdTotal) + " (" + formatBytes(sdFree) + " free)")
                                 : "Not mounted";
    tftInstance->drawString(sdStr, (int16_t)(c1.x + 36), rowY, m.fontBody);

    rowY += step;
    tftInstance->setTextColor(TFT_MAGENTA, 0x10A2);
    tftInstance->drawString("RAM:", c1LabelX, rowY, m.fontBody);
    tftInstance->setTextColor(TFT_WHITE, 0x10A2);
    String ramStr = String(ESP.getFreeHeap() / 1024) + " KB";
#if defined(BOARD_HAS_PSRAM)
    if (psramFound()) {
        ramStr += " | PS: " + formatBytes(ESP.getFreePsram());
    }
#endif
    tftInstance->drawString(ramStr, (int16_t)(c1.x + 48), rowY, m.fontBody);

    // 2. Community & Project Card
    const UiRect c2 = aboutCard2(m);
    tftInstance->fillRoundRect(c2.x, c2.y, c2.w, c2.h, 6, 0x10A2);
    tftInstance->drawRoundRect(c2.x, c2.y, c2.w, c2.h, 6, 0x05BF);

    const int16_t iconX = (int16_t)(c2.x + 12);
    const int16_t c2LabelX = (int16_t)(c2.x + 22);
    const int16_t c2RowPitch = (int16_t)(step + 2);
    int16_t c2Row = (int16_t)(c2.y + 6);

    tftInstance->setTextColor(TFT_CYAN, 0x10A2);
    tftInstance->drawString("Community & Project", c2LabelX, c2Row, m.fontBody);

    // Row 1: GitHub Stars
    c2Row += c2RowPitch;
    drawMiniStar(tftInstance, iconX, (int16_t)(c2Row + 8), TFT_YELLOW);
    tftInstance->setTextColor(TFT_YELLOW, 0x10A2);
    tftInstance->drawString("Stars:", c2LabelX, c2Row, m.fontBody);
    int starsCount = loadCachedStars();
    int roundedTier = (starsCount / 5) * 5;
    String starsStr = s_lastFetchLive ? (String(starsCount) + " (Live)")
                                      : (String(roundedTier) + "+ stars");
    tftInstance->setTextColor(s_lastFetchLive ? TFT_GREEN : 0xFEA0, 0x10A2);
    tftInstance->drawString(starsStr, (int16_t)(c2.x + 70), c2Row, m.fontBody);

    // Row 2: Community URL
    c2Row += c2RowPitch;
    drawMiniHeart(tftInstance, iconX, (int16_t)(c2Row + 8), 0xF81F);
    tftInstance->setTextColor(0xF81F, 0x10A2);
    tftInstance->drawString("Repo:", c2LabelX, c2Row, m.fontBody);
    tftInstance->setTextColor(TFT_WHITE, 0x10A2);
    tftInstance->drawString("Haris16-code/KryonOS", (int16_t)(c2.x + 64), c2Row, m.fontBody);

    // Row 3: Author
    c2Row += c2RowPitch;
    drawMiniSparkle(tftInstance, iconX, (int16_t)(c2Row + 8), TFT_CYAN);
    tftInstance->setTextColor(TFT_CYAN, 0x10A2);
    tftInstance->drawString("Author:", c2LabelX, c2Row, m.fontBody);
    tftInstance->setTextColor(TFT_WHITE, 0x10A2);
    tftInstance->drawString("Haris (@Haris16-code)", (int16_t)(c2.x + 74), c2Row, m.fontBody);

    // Reset Apps Button
    const UiRect reset = aboutResetButton(m);
    tftInstance->fillRoundRect(reset.x, reset.y, reset.w, reset.h, 4, TFT_RED);
    tftInstance->setTextColor(TFT_WHITE, TFT_RED);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Reset App Data", reset.cx(), reset.cy(), m.fontBody);

    extern bool showResetDialog;
    if (showResetDialog) {
        const UiRect panel = m.dialogPanel(160);
        tftInstance->fillRoundRect(panel.x, panel.y, panel.w, panel.h, 8, TFT_DARKGREY);
        tftInstance->drawRoundRect(panel.x, panel.y, panel.w, panel.h, 8, TFT_RED);

        tftInstance->setTextColor(TFT_YELLOW, TFT_DARKGREY);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("WARNING!", panel.cx(), (int16_t)(panel.y + 30), m.fontHeader);

        tftInstance->setTextColor(TFT_WHITE, TFT_DARKGREY);
        tftInstance->drawString("Format LittleFS &", panel.cx(), (int16_t)(panel.y + 60), m.fontBody);
        tftInstance->drawString("Delete all Apps?", panel.cx(), (int16_t)(panel.y + 80), m.fontBody);

        const int16_t rowY2 = (int16_t)(panel.y + 110);
        const UiRect yes = m.dialogButtonSpaced(rowY2, 30, 0, 2, 70, 40);
        const UiRect no  = m.dialogButtonSpaced(rowY2, 30, 1, 2, 70, 40);

        tftInstance->fillRoundRect(yes.x, yes.y, yes.w, yes.h, 4, TFT_RED);
        tftInstance->setTextColor(TFT_WHITE, TFT_RED);
        tftInstance->drawString("Yes", yes.cx(), yes.cy(), m.fontBody);

        tftInstance->fillRoundRect(no.x, no.y, no.w, no.h, 4, TFT_GREEN);
        tftInstance->setTextColor(TFT_BLACK, TFT_GREEN);
        tftInstance->drawString("No", no.cx(), no.cy(), m.fontBody);
    }

    drawSettingsFooter(tftInstance, "BACK");
}

void SettingsUI::handleAboutTouch(uint16_t x, uint16_t y) {
    extern int currentState;
    extern bool showResetDialog;
    const UiMetrics& m = M();

    if (showResetDialog) {
        const int16_t rowY = (int16_t)(m.dialogPanel(160).y + 110);
        const UiRect yes = m.dialogButtonSpaced(rowY, 30, 0, 2, 70, 40);
        const UiRect no  = m.dialogButtonSpaced(rowY, 30, 1, 2, 70, 40);

        if (yes.contains((int16_t)x, (int16_t)y)) {
            tftInstance->fillScreen(TFT_BLACK);
            tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("Formatting...", m.centerX, m.centerY, m.fontHeader);
            tftInstance->present(); // the format below writes the whole flash partition

            FileSystem::formatLittleFS();

            tftInstance->drawString("Rebooting...", m.centerX, (int16_t)(m.centerY + 40), m.fontHeader);
            tftInstance->present();
            delay(1000);
            ESP.restart();
        } else if (no.contains((int16_t)x, (int16_t)y)) {
            showResetDialog = false;
            drawAbout();
        }
        return;
    }

    // Community Card Touched (Tap to refresh Live Stars)
    if (aboutCard2(m).contains((int16_t)x, (int16_t)y)) {
        if (WiFi.status() == WL_CONNECTED) {
            const UiRect c2 = aboutCard2(m);
            const int16_t rowY = (int16_t)(c2.y + 6 + (bodyLineStep(m) + 2));
            tftInstance->setTextColor(TFT_YELLOW, 0x10A2);
            tftInstance->setTextDatum(TL_DATUM);
            tftInstance->drawString("Fetching...", (int16_t)(c2.x + 70), rowY, m.fontBody);
            tftInstance->present(); // the fetch below blocks; the main loop is inside this handler
            fetchGitHubStarsLive();
            drawAbout();
        }
        return;
    }

    // Reset Button Touched
    if (aboutResetButton(m).contains((int16_t)x, (int16_t)y)) {
        showResetDialog = true;
        drawAbout();
        return;
    }

    // Bottom Nav: BACK
    if (m.inFooter((int16_t)y)) {
        if (m.footerButtonFromX((int16_t)x) == UI_FOOTER_SEL) {
            currentState = 1; // STATE_SETTINGS
        }
    }
}

// ----------------------------------------------------
// MANAGE APPS MENU
// ----------------------------------------------------

static FileEntry appEntries[50];
static int totalApps = -1;
static int appScroll = 0;
static int appSelected = -1;
static bool appMenuOpen = false;
static bool defaultInstallSD = false; // Loaded lazily

static void loadAppInstallPreference() {
    if (FileSystem::exists("/local/config_install_sd.txt")) {
        defaultInstallSD = true;
    } else {
        defaultInstallSD = false;
    }
}

static void saveAppInstallPreference() {
    if (defaultInstallSD) {
        FileSystem::writeTextFile("/local/config_install_sd.txt", "1");
    } else {
        FileSystem::deleteFile("/local/config_install_sd.txt");
    }
}

namespace {
// The Apps screen is a location toggle pinned under the header, then a list of installed apps.
UiRect appsToggleRect(const UiMetrics& m) {
    return { m.list.x, (int16_t)(m.header.y + m.header.h + 4), m.list.w, m.rowH };
}

int16_t appsListTop(const UiMetrics& m) { return (int16_t)(appsToggleRect(m).bottom() + 10); }
int16_t appsRowH(const UiMetrics& m)     { return m.rowH; }
int16_t appsRowPitch(const UiMetrics& m) { return (int16_t)(m.rowH + 5); }

// The rows run right down to the footer rather than stopping short of it, which is how the
// historical layout fitted six apps on a 320px-tall screen.
int appsPerPage(const UiMetrics& m) {
    const int pitch = appsRowPitch(m);
    if (pitch <= 0) return 1;
    const int avail = m.footer.y - appsListTop(m);
    const int n = (avail - appsRowH(m)) / pitch + 1;
    return (n < 1) ? 1 : n;
}

UiRect appsRowRect(const UiMetrics& m, int visibleIndex) {
    return { m.list.x, (int16_t)(appsListTop(m) + visibleIndex * appsRowPitch(m)),
             m.list.w, appsRowH(m) };
}

// The per-app action sheet. Narrower than a dialog panel, and tall enough for a title plus three
// buttons; the button height follows the row height so it still fits a short panel.
UiRect appsMenuPanel(const UiMetrics& m) {
    const int16_t w = (int16_t)(m.list.w - 20);
    const int16_t h = (int16_t)(m.rowH * 5);
    const int16_t x = (int16_t)(m.centerX - w / 2);
    int16_t y = appsListTop(m);
    const int16_t bottom = (int16_t)(m.footer.y - 15);
    if (y + h > bottom) y = (int16_t)(bottom - h);
    if (y < 0) y = 0;
    return { x, y, w, h };
}

UiRect appsMenuButton(const UiMetrics& m, int index) {
    const UiRect panel = appsMenuPanel(m);
    const int16_t gap = (int16_t)(m.rowH / 3);
    return { (int16_t)(panel.x + 10),
             (int16_t)(panel.y + m.rowH + index * (m.rowH + gap)),
             (int16_t)(panel.w - 20), m.rowH };
}
} // namespace

void SettingsUI::drawApps() {
    if (!tftInstance) return;

    const UiMetrics& m = M();
    drawSettingsFrame(tftInstance, "Manage Apps");

    // Default Install Location Toggle Button
    if (totalApps == -1) loadAppInstallPreference();

    const UiRect toggle = appsToggleRect(m);
    tftInstance->fillRoundRect(toggle.x, toggle.y, toggle.w, toggle.h, 4, TFT_DARKGREY);
    tftInstance->setTextColor(TFT_WHITE, TFT_DARKGREY);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString(defaultInstallSD ? "Default Install: SD" : "Default Install: LFS",
                            toggle.cx(), toggle.cy(), m.fontBody);

    // Load Apps
    if (totalApps == -1) {
        totalApps = 0;
        int c1 = FileSystem::listDirectory("/local/apps/", appEntries, 25);
        totalApps += c1;

        // Also list /sd/apps/
        int c2 = FileSystem::listDirectory("/sd/apps/", appEntries + totalApps, 25);
        totalApps += c2;
    }

    const int perPage = appsPerPage(m);

    for (int i = 0; i < perPage; i++) {
        const int listIndex = appScroll + i;
        if (listIndex >= totalApps) break;

        FileEntry entry = appEntries[listIndex];
        const UiRect row = appsRowRect(m, i);
        const bool selected = (listIndex == appSelected);

        tftInstance->fillRect(row.x, row.y, row.w, row.h, selected ? TFT_BLUE : TFT_BLACK);

        tftInstance->setTextColor(TFT_WHITE);
        // Show Name
        String displayName = entry.name;
        if (entry.isDir) {
            String appJsonPath = entry.path;
            if (!appJsonPath.endsWith("/")) appJsonPath += "/";
            appJsonPath += "app.json";
            if (FileSystem::exists(appJsonPath.c_str())) {
                String jsonContent = FileSystem::readTextFile(appJsonPath.c_str());
                String parsedName = FileSystem::parseJsonValue(jsonContent, "name");
                if (parsedName.length() > 0) displayName = parsedName;
            }
        }
        tftInstance->setTextDatum(ML_DATUM);
        tftInstance->drawString(displayName, (int16_t)(row.x + m.rowTextPadX), row.cy(), m.fontBody);

        // Show Drive Marker
        tftInstance->setTextColor(TFT_YELLOW);
        tftInstance->drawString(entry.path.startsWith("/sd") ? "[SD]" : "[LFS]",
                                (int16_t)(row.right() - 40), row.cy(), m.fontBody);
    }

    // Scrollbar, spanning the same band as the rows
    if (totalApps > perPage) {
        const int trackTop = appsListTop(m);
        const int trackH = m.footer.y - trackTop;
        const int thumbH = max((int)m.scrollThumbMin, (trackH * perPage) / totalApps);
        const int thumbY = trackTop + (appScroll * (trackH - thumbH)) / (totalApps - perPage);
        tftInstance->fillRect(m.scrollX, trackTop, m.scrollW, trackH, TFT_DARKGREY);
        tftInstance->fillRect(m.scrollX, thumbY, m.scrollW, thumbH, TFT_WHITE);
    }

    // Footer
    if (totalApps > perPage) drawSettingsFooterScroll(tftInstance, "BACK");
    else                     drawSettingsFooter(tftInstance, "BACK");

    // Draw Pop-Up Menu
    if (appMenuOpen && appSelected != -1) {
        FileEntry sel = appEntries[appSelected];
        const bool isSD = sel.path.startsWith("/sd");
        const UiRect panel = appsMenuPanel(m);

        tftInstance->fillRoundRect(panel.x, panel.y, panel.w, panel.h, 5, TFT_DARKGREY);
        tftInstance->drawRoundRect(panel.x, panel.y, panel.w, panel.h, 5, TFT_WHITE);

        tftInstance->setTextColor(TFT_YELLOW, TFT_DARKGREY);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("App Actions", panel.cx(), (int16_t)(panel.y + 15), m.fontBody);

        const UiRect uninstall = appsMenuButton(m, 0);
        tftInstance->fillRoundRect(uninstall.x, uninstall.y, uninstall.w, uninstall.h, 4, TFT_RED);
        tftInstance->setTextColor(TFT_WHITE, TFT_RED);
        tftInstance->drawString("Uninstall", uninstall.cx(), uninstall.cy(), m.fontBody);

        const UiRect move = appsMenuButton(m, 1);
        tftInstance->fillRoundRect(move.x, move.y, move.w, move.h, 4, TFT_ORANGE);
        tftInstance->setTextColor(TFT_BLACK, TFT_ORANGE);
        tftInstance->drawString(isSD ? "Move to LFS" : "Move to SD", move.cx(), move.cy(), m.fontBody);

        const UiRect cancel = appsMenuButton(m, 2);
        tftInstance->fillRoundRect(cancel.x, cancel.y, cancel.w, cancel.h, 4, TFT_BLACK);
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->drawString("Cancel", cancel.cx(), cancel.cy(), m.fontBody);
    }
}

void SettingsUI::handleAppsTouch(uint16_t x, uint16_t y) {
    extern int currentState;
    const UiMetrics& m = M();

    if (appMenuOpen) {
        FileEntry sel = appEntries[appSelected];
        const bool isSD = sel.path.startsWith("/sd");

        if (appsMenuButton(m, 0).contains((int16_t)x, (int16_t)y)) {
            // UNINSTALL
            if (sel.isDir) {
                FileEntry existingFiles[50];
                int existingCount = FileSystem::listDirectory(sel.path.c_str(), existingFiles, 50);
                for (int i = 0; i < existingCount; i++) {
                    if (!existingFiles[i].isDir) {
                        FileSystem::deleteFile(existingFiles[i].path.c_str());
                    }
                }
                FileSystem::rmdir(sel.path.c_str());
            } else {
                FileSystem::deleteFile(sel.path.c_str());
            }
            totalApps = -1; // Refresh list
            appMenuOpen = false;
            appSelected = -1;
            drawApps();
        } else if (appsMenuButton(m, 1).contains((int16_t)x, (int16_t)y)) {
            // MOVE
            String destDir = isSD ? "/local/apps/" : "/sd/apps/";
            FileSystem::mkdir(destDir.c_str()); // Ensure dir exists
            String destPath = destDir + sel.name;

            // Transient "Moving..." pill
            const UiRect pill = { (int16_t)(m.centerX - (m.list.w - 60) / 2),
                                  (int16_t)(m.centerY - (m.rowH + 10) / 2),
                                  (int16_t)(m.list.w - 60), (int16_t)(m.rowH + 10) };
            tftInstance->fillRoundRect(pill.x, pill.y, pill.w, pill.h, 5, TFT_BLACK);
            tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("Moving...", pill.cx(), pill.cy(), m.fontBody);
            tftInstance->present(); // the copy below runs to completion before anything else can flush

            if (sel.isDir) {
                if (FileSystem::copyDirectory(sel.path.c_str(), destPath.c_str())) {
                    FileEntry existingFiles[50];
                    int existingCount = FileSystem::listDirectory(sel.path.c_str(), existingFiles, 50);
                    for (int i = 0; i < existingCount; i++) {
                        if (!existingFiles[i].isDir) {
                            FileSystem::deleteFile(existingFiles[i].path.c_str());
                        }
                    }
                    FileSystem::rmdir(sel.path.c_str());
                }
            } else {
                if (FileSystem::copyFile(sel.path.c_str(), destPath.c_str())) {
                    FileSystem::deleteFile(sel.path.c_str());
                }
            }

            totalApps = -1; // Refresh list
            appMenuOpen = false;
            appSelected = -1;
            drawApps();
        } else if (appsMenuButton(m, 2).contains((int16_t)x, (int16_t)y)) {
            // CANCEL
            appMenuOpen = false;
            drawApps();
        }
        return;
    }

    // Default Install Toggle
    if (appsToggleRect(m).contains((int16_t)x, (int16_t)y)) {
        defaultInstallSD = !defaultInstallSD;
        saveAppInstallPreference();
        drawApps();
        return;
    }

    // List Selection — walk the same rows drawApps() laid out
    const int perPage = appsPerPage(m);
    for (int i = 0; i < perPage; i++) {
        if (appScroll + i >= totalApps) break;
        if (!appsRowRect(m, i).contains((int16_t)x, (int16_t)y)) continue;
        appSelected = appScroll + i;
        appMenuOpen = true;
        drawApps();
        return;
    }

    // Bottom Nav
    if (!m.inFooter((int16_t)y)) return;
    switch (m.footerButtonFromX((int16_t)x)) {
    case UI_FOOTER_UP:
        if (appScroll > 0) { appScroll--; drawApps(); }
        break;
    case UI_FOOTER_DN:
        if (appScroll + perPage < totalApps) { appScroll++; drawApps(); }
        break;
    default: // UI_FOOTER_SEL — BACK
        totalApps = -1; // Reset state for next visit
        appScroll = 0;
        appSelected = -1;
        appMenuOpen = false;
        currentState = 1; // STATE_SETTINGS
        break;
    }
}

// ----------------------------------------------------
// PERMISSIONS MANAGER
// ----------------------------------------------------

struct PermAppEntry {
    String packageName;
    String appName;
    String storageDrive;
    std::vector<String> permissions;
};

static int s_permPage = 0;
static bool s_permResetConfirm = false;
static std::vector<PermAppEntry> s_permApps;

static void loadPermissionsData() {
    s_permApps.clear();
    String path = "/local/system/app_permissions.json";
    if (!FileSystem::exists(path.c_str())) return;
    
    String content = FileSystem::readTextFile(path.c_str());
    if (content.length() == 0) return;

    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, content);
    if (err) return;

    JsonObject root = doc.as<JsonObject>();
    for (JsonPair kv : root) {
        String pkg = String(kv.key().c_str());
        JsonArray arr = kv.value().as<JsonArray>();
        if (arr.isNull() || arr.size() == 0) continue;

        PermAppEntry entry;
        entry.packageName = pkg;
        entry.appName = pkg;
        entry.storageDrive = "[LFS]";

        for (JsonVariant v : arr) {
            entry.permissions.push_back(v.as<String>());
        }

        // Try reading display name from app.json in LittleFS or SD
        String lfsJson = "/local/apps/" + pkg + "/app.json";
        String sdJson = "/sd/apps/" + pkg + "/app.json";
        if (FileSystem::exists(lfsJson.c_str())) {
            String jc = FileSystem::readTextFile(lfsJson.c_str());
            String n = FileSystem::parseJsonValue(jc, "name");
            if (n.length() > 0) entry.appName = n;
            entry.storageDrive = "[LFS]";
        } else if (FileSystem::exists(sdJson.c_str())) {
            String jc = FileSystem::readTextFile(sdJson.c_str());
            String n = FileSystem::parseJsonValue(jc, "name");
            if (n.length() > 0) entry.appName = n;
            entry.storageDrive = "[SD]";
        }

        s_permApps.push_back(entry);
    }
}

namespace {
int16_t permCardH(const UiMetrics& m)     { return (int16_t)(m.rowH + 32); }
int16_t permCardPitch(const UiMetrics& m) { return (int16_t)(m.rowH + 38); }
int16_t permCardsTop(const UiMetrics& m)  { return (int16_t)(m.header.bottom() + 6); }

// "Reset All" shares the header row with the left-aligned title, so it is pinned to the right edge.
UiRect permResetAllButton(const UiMetrics& m) {
    const int16_t w = (int16_t)min((int)88, (int)(m.header.w / 2));
    return { (int16_t)(m.header.right() - 6 - w), (int16_t)(m.header.y + 3), w,
             (int16_t)(m.header.h - 6) };
}

int16_t permPagerH(const UiMetrics& m) { return (int16_t)min((int)28, (int)m.rowH); }
int16_t permPagerY(const UiMetrics& m) { return (int16_t)(m.footer.y - m.rowH - 7); }

UiRect permPrevButton(const UiMetrics& m) {
    return { m.list.x, permPagerY(m), (int16_t)min((int)60, (int)(m.list.w / 3)), permPagerH(m) };
}

UiRect permNextButton(const UiMetrics& m) {
    const int16_t w = (int16_t)min((int)60, (int)(m.list.w / 3));
    return { (int16_t)(m.list.right() - w), permPagerY(m), w, permPagerH(m) };
}

// How many app cards fit. The pager band is only reserved once the list is known to paginate, hence
// the parameter: the caller does one pass without the reservation and a second pass with it.
int permCardsPerPage(const UiMetrics& m, bool reservePager) {
    const int pitch = permCardPitch(m);
    if (pitch <= 0) return 1;
    const int bottom = reservePager ? (permPagerY(m) - 6) : (m.footer.y - 7);
    const int n = (bottom - permCardsTop(m) - permCardH(m)) / pitch + 1;
    return (n < 1) ? 1 : n;
}

UiRect permCardRect(const UiMetrics& m, int visibleIndex) {
    return { m.list.x, (int16_t)(permCardsTop(m) + visibleIndex * permCardPitch(m)),
             m.list.w, permCardH(m) };
}

UiRect permRevokeButton(const UiMetrics& m, int visibleIndex) {
    const int16_t w = (int16_t)min((int)75, (int)(m.list.w / 2));
    return { (int16_t)(m.list.right() - 10 - w),
             (int16_t)(permCardRect(m, visibleIndex).y + max(8, (int)m.rowH * 3 / 4)),
             w, (int16_t)max(14, (int)m.rowH) };
}

// Resolve the card count and page count together: reserving the pager band can only shrink the
// cards-per-page figure, so a second pass settles it.
void permResolvePages(const UiMetrics& m, int appCount, int& itemsPerPage, int& totalPages) {
    itemsPerPage = permCardsPerPage(m, false);
    totalPages = (appCount + itemsPerPage - 1) / itemsPerPage;
    if (totalPages > 1) {
        itemsPerPage = permCardsPerPage(m, true);
        totalPages = (appCount + itemsPerPage - 1) / itemsPerPage;
    }
}
} // namespace

void SettingsUI::drawPermissions() {
    if (!tftInstance) return;

    loadPermissionsData();

    const UiMetrics& m = M();

    // The title is left-aligned here because a Reset All button shares the header row.
    // ML_DATUM at headerTextY puts the same pixels as the old TL_DATUM at y=13 (font 2 is 16px tall),
    // but keeps the baseline centred if the header height ever changes.
    drawSettingsBackdrop(tftInstance);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(ML_DATUM);
    tftInstance->drawString("Permissions", (int16_t)(m.header.x + 8), m.headerTextY, m.fontBody);

    // Reset All Button in header
    if (!s_permApps.empty()) {
        const UiRect reset = permResetAllButton(m);
        tftInstance->fillRoundRect(reset.x, reset.y, reset.w, reset.h, 4, TFT_RED);
        tftInstance->setTextColor(TFT_WHITE, TFT_RED);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("Reset All", reset.cx(), reset.cy(), m.fontBody);
    }

    if (s_permResetConfirm) {
        // Confirmation Dialog
        const UiRect panel = m.dialogPanel(160);
        tftInstance->fillRoundRect(panel.x, panel.y, panel.w, panel.h, 8, TFT_DARKGREY);
        tftInstance->drawRoundRect(panel.x, panel.y, panel.w, panel.h, 8, TFT_WHITE);

        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->setTextColor(TFT_GOLD, TFT_DARKGREY);
        tftInstance->drawString("Reset Permissions?", panel.cx(), (int16_t)(panel.y + 18), m.fontBody);

        tftInstance->setTextColor(TFT_WHITE, TFT_DARKGREY);
        tftInstance->drawString("Revoke and clear all", panel.cx(), (int16_t)(panel.y + 45), m.fontBody);
        tftInstance->drawString("granted app permissions?", panel.cx(), (int16_t)(panel.y + 65), m.fontBody);

        // Confirm / Cancel, centred under the text
        const int16_t rowY = (int16_t)(panel.y + 100);
        const UiRect confirm = m.dialogButtonSpaced(rowY, 34, 0, 2, 90, 10);
        const UiRect cancel  = m.dialogButtonSpaced(rowY, 34, 1, 2, 90, 10);

        tftInstance->fillRoundRect(confirm.x, confirm.y, confirm.w, confirm.h, 4, TFT_RED);
        tftInstance->setTextColor(TFT_WHITE, TFT_RED);
        tftInstance->drawString("Confirm", confirm.cx(), confirm.cy(), m.fontBody);

        tftInstance->fillRoundRect(cancel.x, cancel.y, cancel.w, cancel.h, 4, TFT_NAVY);
        tftInstance->setTextColor(TFT_WHITE, TFT_NAVY);
        tftInstance->drawString("Cancel", cancel.cx(), cancel.cy(), m.fontBody);

        drawSettingsFooter(tftInstance, "BACK");
        return;
    }

    if (s_permApps.empty()) {
        const UiRect card = { m.list.x, (int16_t)(m.list.y + 10), m.list.w,
                              (int16_t)(m.list.h - 40) };
        tftInstance->fillRoundRect(card.x, card.y, card.w, card.h, 6, 0x10A2); // Dark navy
        tftInstance->drawRoundRect(card.x, card.y, card.w, card.h, 6, TFT_CYAN);

        // A title and two two-line paragraphs, evenly pitched off the row height.
        const int16_t lh = (int16_t)(bodyLineStep(m) + 5);
        int16_t y = (int16_t)(card.y + 30);

        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->setTextColor(TFT_GOLD, 0x10A2);
        tftInstance->drawString("No Granted Permissions", card.cx(), y, m.fontBody);
        tftInstance->setTextColor(TFT_WHITE, 0x10A2);
        y += lh + 10;
        tftInstance->drawString("Apps access internal folders", card.cx(), y, m.fontBody);
        y += lh;
        tftInstance->drawString("without permission.", card.cx(), y, m.fontBody);
        y += lh + 10;
        tftInstance->drawString("External storage requests", card.cx(), y, m.fontBody);
        y += lh;
        tftInstance->drawString("will prompt on demand.", card.cx(), y, m.fontBody);
    } else {
        int itemsPerPage = 0, totalPages = 0;
        permResolvePages(m, (int)s_permApps.size(), itemsPerPage, totalPages);
        if (s_permPage >= totalPages) s_permPage = totalPages - 1;
        if (s_permPage < 0) s_permPage = 0;

        const int startIdx = s_permPage * itemsPerPage;

        for (int i = 0; i < itemsPerPage && (startIdx + i) < (int)s_permApps.size(); i++) {
            const PermAppEntry& entry = s_permApps[startIdx + i];
            const UiRect card = permCardRect(m, i);

            // Card background
            tftInstance->fillRoundRect(card.x, card.y, card.w, card.h, 5, 0x18C3);
            tftInstance->drawRoundRect(card.x, card.y, card.w, card.h, 5, 0x2945);

            // App Name & Package
            tftInstance->setTextDatum(TL_DATUM);
            tftInstance->setTextColor(TFT_WHITE, 0x18C3);
            String title = entry.appName;
            if (title.length() > 16) title = title.substring(0, 14) + "..";
            tftInstance->drawString(title, (int16_t)(card.x + 6), (int16_t)(card.y + 6), m.fontBody);

            tftInstance->setTextColor(TFT_DARKGREY, 0x18C3);
            String sub = entry.packageName;
            if (sub.length() > 20) sub = sub.substring(0, 18) + "..";
            tftInstance->drawString(sub, (int16_t)(card.x + 6),
                                    (int16_t)(card.y + m.rowH * 4 / 5), m.fontSmall);

            // Permission Badge
            const int16_t badgeW = (int16_t)min((int)110, (int)(m.list.w / 2));
            const int16_t badgeH = (int16_t)max(10, (int)m.rowH * 3 / 5);
            const int16_t badgeY = (int16_t)(card.y + m.rowH + 8);
            tftInstance->fillRoundRect((int16_t)(card.x + 6), badgeY, badgeW, badgeH, 3, 0x03E0);
            tftInstance->setTextColor(TFT_WHITE, 0x03E0);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("Storage: GRANTED", (int16_t)(card.x + 6 + badgeW / 2),
                                    (int16_t)(badgeY + badgeH / 2), m.fontSmall);

            // Revoke Button
            const UiRect revoke = permRevokeButton(m, i);
            tftInstance->fillRoundRect(revoke.x, revoke.y, revoke.w, revoke.h, 4, TFT_RED);
            tftInstance->setTextColor(TFT_WHITE, TFT_RED);
            tftInstance->drawString("Revoke", revoke.cx(), revoke.cy(), m.fontBody);
        }

        // Pagination Bar
        if (totalPages > 1) {
            const int16_t rowCY = permPrevButton(m).cy();
            tftInstance->setTextDatum(MC_DATUM);

            // Prev Button
            if (s_permPage > 0) {
                const UiRect prev = permPrevButton(m);
                tftInstance->fillRoundRect(prev.x, prev.y, prev.w, prev.h, 4, TFT_BLUE);
                tftInstance->setTextColor(TFT_WHITE, TFT_BLUE);
                tftInstance->drawString("< Prev", prev.cx(), prev.cy(), m.fontBody);
            }

            // Page Indicator
            tftInstance->setTextColor(TFT_YELLOW, TFT_BLACK);
            tftInstance->drawString(String(s_permPage + 1) + "/" + String(totalPages),
                                    m.centerX, rowCY, m.fontBody);

            // Next Button
            if (s_permPage < totalPages - 1) {
                const UiRect next = permNextButton(m);
                tftInstance->fillRoundRect(next.x, next.y, next.w, next.h, 4, TFT_BLUE);
                tftInstance->setTextColor(TFT_WHITE, TFT_BLUE);
                tftInstance->drawString("Next >", next.cx(), next.cy(), m.fontBody);
            }
        }
    }

    // Footer
    drawSettingsFooter(tftInstance, "BACK");
}

void SettingsUI::handlePermissionsTouch(uint16_t x, uint16_t y) {
    extern int currentState;
    const UiMetrics& m = M();

    if (s_permResetConfirm) {
        const int16_t rowY = (int16_t)(m.dialogPanel(160).y + 100);
        const UiRect confirm = m.dialogButtonSpaced(rowY, 34, 0, 2, 90, 10);
        const UiRect cancel  = m.dialogButtonSpaced(rowY, 34, 1, 2, 90, 10);

        if (confirm.contains((int16_t)x, (int16_t)y)) { // Confirm Reset
            FileSystem::deleteFile("/local/system/app_permissions.json");
            JSBindings::clearAllSessionPermissions();
            s_permResetConfirm = false;
            drawPermissions();
            return;
        }
        if (cancel.contains((int16_t)x, (int16_t)y)) { // Cancel
            s_permResetConfirm = false;
            drawPermissions();
            return;
        }
        if (m.inFooter((int16_t)y) && m.footerButtonFromX((int16_t)x) == UI_FOOTER_SEL) {
            s_permResetConfirm = false;
            currentState = 1; // Back to Settings Menu
        }
        return;
    }

    // Header: Reset All Button
    if (!s_permApps.empty() && permResetAllButton(m).contains((int16_t)x, (int16_t)y)) {
        s_permResetConfirm = true;
        drawPermissions();
        return;
    }

    // Revoke Buttons
    if (!s_permApps.empty()) {
        int itemsPerPage = 0, totalPages = 0;
        permResolvePages(m, (int)s_permApps.size(), itemsPerPage, totalPages);
        const int startIdx = s_permPage * itemsPerPage;

        for (int i = 0; i < itemsPerPage && (startIdx + i) < (int)s_permApps.size(); i++) {
            if (!permRevokeButton(m, i).contains((int16_t)x, (int16_t)y)) continue;

            String pkg = s_permApps[startIdx + i].packageName;

            // Remove from app_permissions.json
            String path = "/local/system/app_permissions.json";
            if (FileSystem::exists(path.c_str())) {
                String content = FileSystem::readTextFile(path.c_str());
                JsonDocument doc;
                deserializeJson(doc, content);
                doc.remove(pkg);
                String out;
                serializeJson(doc, out);
                FileSystem::writeTextFile(path.c_str(), out.c_str());
            }

            // Clear session cache
            JSBindings::revokeSessionPermission(pkg);

            drawPermissions();
            return;
        }

        // Pagination
        if (totalPages > 1) {
            if (s_permPage > 0 && permPrevButton(m).contains((int16_t)x, (int16_t)y)) {
                s_permPage--;
                drawPermissions();
                return;
            }
            if (s_permPage < totalPages - 1 && permNextButton(m).contains((int16_t)x, (int16_t)y)) {
                s_permPage++;
                drawPermissions();
                return;
            }
        }
    }

    // Bottom Nav: BACK
    if (m.inFooter((int16_t)y)) {
        if (m.footerButtonFromX((int16_t)x) == UI_FOOTER_SEL) {
            currentState = 1; // STATE_SETTINGS
        }
    }
}

// ----------------------------------------------------
// TIME & REGION MENU
// ----------------------------------------------------

static int tzScroll = 0;
static int timeActionScroll = 0;
static bool tzSelectMode = false;

struct TZEntry {
    const char* label;
    const char* value;
};

static TZEntry tzList[] = {
    {"UTC-12 Baker Is", "UTC12"},
    {"UTC-11 Midway", "UTC11"},
    {"UTC-10 Hawaii", "UTC10"},
    {"UTC-9 Alaska", "UTC9"},
    {"UTC-8 PST", "UTC8"},
    {"UTC-7 MST", "UTC7"},
    {"UTC-6 CST", "UTC6"},
    {"UTC-5 EST", "UTC5"},
    {"UTC-4 AST", "UTC4"},
    {"UTC-3 BRT", "UTC3"},
    {"UTC-2", "UTC2"},
    {"UTC-1 AZOT", "UTC1"},
    {"UTC+0 GMT", "UTC0"},
    {"UTC+1 CET", "UTC-1"},
    {"UTC+2 EET", "UTC-2"},
    {"UTC+3 MSK", "UTC-3"},
    {"UTC+4 GST", "UTC-4"},
    {"UTC+5 PKT", "UTC-5"},
    {"UTC+5:30 IST", "UTC-5:30"},
    {"UTC+6 BST", "UTC-6"},
    {"UTC+7 ICT", "UTC-7"},
    {"UTC+8 CST/AWST", "UTC-8"},
    {"UTC+9 JST", "UTC-9"},
    {"UTC+10 AEST", "UTC-10"},
    {"UTC+11 AEDT", "UTC-11"},
    {"UTC+12 NZST", "UTC-12"}
};
const int tzCount = sizeof(tzList) / sizeof(TZEntry);

namespace {
// The main Time screen: a fixed "current time" header, then a stack of option buttons.
const int16_t TIME_HEADER_H = 65;

int16_t timeBtnTop(const UiMetrics& m)   { return (int16_t)(m.list.y + TIME_HEADER_H); }
int16_t timeBtnH(const UiMetrics& m)     { return (int16_t)(m.rowH + 5); }
int16_t timeBtnPitch(const UiMetrics& m) { return (int16_t)(m.rowH + 15); }

int timeBtnPerPage(const UiMetrics& m) {
    const int pitch = timeBtnPitch(m);
    if (pitch <= 0) return 1;
    const int avail = m.footer.y - timeBtnTop(m);
    const int n = (avail - timeBtnH(m)) / pitch + 1;
    return (n < 1) ? 1 : n;
}

UiRect timeButtonRect(const UiMetrics& m, int visibleIndex) {
    return { (int16_t)(m.list.x + 10), (int16_t)(timeBtnTop(m) + visibleIndex * timeBtnPitch(m)),
             (int16_t)(m.list.w - 20), timeBtnH(m) };
}

// Timezone picker rows sit just below the "Select Timezone" heading.
int16_t tzRowsTop(const UiMetrics& m)  { return (int16_t)(m.list.y + 15); }
int16_t tzRowPitch(const UiMetrics& m) { return (int16_t)(m.rowH + 5); }

int tzRowsPerPage(const UiMetrics& m) {
    const int pitch = tzRowPitch(m);
    if (pitch <= 0) return 1;
    const int avail = m.footer.y - tzRowsTop(m);
    const int n = (avail - m.rowH) / pitch + 1;
    return (n < 1) ? 1 : n;
}

UiRect tzRowRect(const UiMetrics& m, int visibleIndex) {
    return { m.list.x, (int16_t)(tzRowsTop(m) + visibleIndex * tzRowPitch(m)), m.list.w, m.rowH };
}

// --- Manual-time spinners -------------------------------------------------------------------
// A spinner is an up triangle, a value box and a down triangle stacked in one rect.

int16_t spinnerSectionH(const UiMetrics& m) { return (int16_t)(2 * (m.rowH / 2) + m.rowH + 10); }
int16_t spinnerBoxY(const UiRect& r, const UiMetrics& m) { return (int16_t)(r.y + m.rowH / 2 + 5); }

// The two spinner rows, one section apart.
int16_t spinnerRowY(const UiMetrics& m, int rowIndex) {
    return (int16_t)(m.list.y + 15 + rowIndex * (spinnerSectionH(m) + m.rowH));
}

// Tapping the upper half of a spinner increments it, the lower half decrements — the split falls on
// the value box's centre line, so each half includes its own triangle.
UiRect spinnerUpZone(const UiRect& r, const UiMetrics& m) {
    const int16_t mid = (int16_t)(spinnerBoxY(r, m) + m.rowH / 2);
    return { r.x, r.y, r.w, (int16_t)(mid - r.y) };
}

UiRect spinnerDownZone(const UiRect& r, const UiMetrics& m) {
    const int16_t mid = (int16_t)(spinnerBoxY(r, m) + m.rowH / 2);
    return { r.x, mid, r.w, (int16_t)(r.y + spinnerSectionH(m) - mid) };
}

// Date row: day / month / year. The weights (out of 22) reproduce the historical 50 / 50 / 70 px
// widths and the two 20px separator gaps at a 240px screen width.
UiRect dateSpinnerRect(const UiMetrics& m, int index) {
    static const int kWeight[3] = { 5, 5, 7 };
    static const int kLead[3]   = { 0, 7, 14 }; // cumulative weight + gap before this spinner
    return { (int16_t)(m.list.x + (m.list.w * kLead[index]) / 22),
             spinnerRowY(m, 0),
             (int16_t)((m.list.w * kWeight[index]) / 22),
             spinnerSectionH(m) };
}

int16_t dateSeparatorX(const UiMetrics& m, int index) {
    return (int16_t)(dateSpinnerRect(m, index).right() + m.list.w / 22);
}

// Time row: hour : minute, centred on the canvas.
UiRect timeSpinnerRect(const UiMetrics& m, int index) {
    const int16_t w = (int16_t)(m.list.w * 3 / 11);
    const int16_t gap = (int16_t)(m.list.w * 2 / 11); // the colon slot
    const int16_t x = (index == 0) ? (int16_t)(m.centerX - gap / 2 - w)
                                   : (int16_t)(m.centerX + gap / 2);
    return { x, spinnerRowY(m, 1), w, spinnerSectionH(m) };
}
} // namespace

void SettingsUI::drawTimeSettings() {
    if (!tftInstance) return;
    const UiMetrics& m = M();
    drawSettingsFrame(tftInstance, "Time & Region", TFT_CYAN, TFT_CYAN);

    if (tzSelectMode) {
        // Draw TZ Selection Menu
        tftInstance->setTextColor(TFT_YELLOW, TFT_BLACK);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("Select Timezone", m.centerX, m.list.y, m.fontBody);

        const int perPage = tzRowsPerPage(m);
        tzScroll = clampScrollWindow(tzScroll, tzCount, perPage);

        for (int i = 0; i < perPage; i++) {
            const int listIndex = tzScroll + i;
            if (listIndex >= tzCount) break;

            const UiRect row = tzRowRect(m, i);
            const bool current = (String(tzList[listIndex].value) == TimeManager::currentTimezone);

            tftInstance->fillRect(row.x, row.y, row.w, row.h, current ? TFT_BLUE : TFT_BLACK);
            tftInstance->setTextColor(TFT_WHITE);
            tftInstance->setTextDatum(ML_DATUM);
            tftInstance->drawString(tzList[listIndex].label, (int16_t)(row.x + m.rowTextPadX),
                                    row.cy(), m.fontBody);
        }

        if (tzCount > perPage) {
            const int trackTop = tzRowsTop(m);
            const int trackH = m.footer.y - trackTop;
            const int thumbH = max((int)m.scrollThumbMin, (trackH * perPage) / tzCount);
            const int thumbY = trackTop + (tzScroll * (trackH - thumbH)) / (tzCount - perPage);
            tftInstance->fillRect(m.scrollX, trackTop, m.scrollW, trackH, TFT_DARKGREY);
            tftInstance->fillRect(m.scrollX, thumbY, m.scrollW, thumbH, TFT_WHITE);
        }

        if (tzCount > perPage) drawSettingsFooterScroll(tftInstance, "BACK");
        else                   drawSettingsFooter(tftInstance, "BACK");
        return;
    }

    // Current time header
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->drawString("Current Time:", m.centerX, (int16_t)(m.list.y + 5), m.fontBody);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->drawString(TimeManager::getFormattedTime(), m.centerX,
                            (int16_t)(m.list.y + 30), m.fontHeader);

    // Options
    struct TimeAction { String label; uint16_t bg; uint16_t fg; };
    TimeAction actions[4];
    int actionCount = 0;
    actions[actionCount++] = { TimeManager::ntpEnabled ? String("NTP Sync: ON") : String("NTP Sync: OFF"),
                               (uint16_t)TFT_DARKGREY,
                               (uint16_t)(TimeManager::ntpEnabled ? TFT_GREEN : TFT_RED) };
    actions[actionCount++] = { "Region: " + TimeManager::currentTimezone,
                               (uint16_t)TFT_BLUE, (uint16_t)TFT_WHITE };
    actions[actionCount++] = { TimeManager::use24hFormat ? String("Format: 24h") : String("Format: 12h"),
                               (uint16_t)TFT_ORANGE, (uint16_t)TFT_WHITE };
    if (!TimeManager::ntpEnabled) {
        actions[actionCount++] = { String("Set Manual Time"), (uint16_t)TFT_PURPLE, (uint16_t)TFT_WHITE };
    }

    const int perPage = timeBtnPerPage(m);
    timeActionScroll = clampScrollWindow(timeActionScroll, actionCount, perPage);

    for (int i = 0; i < perPage; i++) {
        const int idx = timeActionScroll + i;
        if (idx >= actionCount) break;
        const UiRect btn = timeButtonRect(m, i);
        tftInstance->fillRoundRect(btn.x, btn.y, btn.w, btn.h, 5, actions[idx].bg);
        tftInstance->setTextColor(actions[idx].fg, actions[idx].bg);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString(actions[idx].label, btn.cx(), btn.cy(), m.fontBody);
    }

    if (actionCount > perPage) {
        const int thumbH = max((int)m.scrollThumbMin, (m.list.h * perPage) / actionCount);
        const int thumbY = m.list.y + (timeActionScroll * (m.list.h - thumbH)) / (actionCount - perPage);
        tftInstance->fillRect(m.scrollX, m.list.y, m.scrollW, m.list.h, TFT_DARKGREY);
        tftInstance->fillRect(m.scrollX, thumbY, m.scrollW, thumbH, TFT_WHITE);
    }

    if (actionCount > perPage) drawSettingsFooterScroll(tftInstance, "BACK");
    else                       drawSettingsFooter(tftInstance, "BACK");
}

void SettingsUI::handleTimeTouch(uint16_t x, uint16_t y) {
    extern int currentState;
    const UiMetrics& m = M();

    if (tzSelectMode) {
        const int perPage = tzRowsPerPage(m);

        for (int i = 0; i < perPage; i++) {
            const int idx = tzScroll + i;
            if (idx >= tzCount) break;
            if (!tzRowRect(m, i).contains((int16_t)x, (int16_t)y)) continue;
            TimeManager::setTimezone(tzList[idx].value);
            tzSelectMode = false;
            drawTimeSettings();
            return;
        }

        if (!m.inFooter((int16_t)y)) return;
        switch (m.footerButtonFromX((int16_t)x)) {
        case UI_FOOTER_UP:
            if (tzScroll > 0) { tzScroll--; drawTimeSettings(); }
            break;
        case UI_FOOTER_DN:
            if (tzScroll < tzCount - perPage) { tzScroll++; drawTimeSettings(); }
            break;
        default: // UI_FOOTER_SEL — cancel the picker
            tzSelectMode = false;
            drawTimeSettings();
            break;
        }
        return;
    }

    const int actionCount = TimeManager::ntpEnabled ? 3 : 4;
    const int perPage = timeBtnPerPage(m);

    for (int i = 0; i < perPage; i++) {
        const int idx = timeActionScroll + i;
        if (idx >= actionCount) break;
        if (!timeButtonRect(m, i).contains((int16_t)x, (int16_t)y)) continue;

        switch (idx) {
        case 0: TimeManager::setNTPEnabled(!TimeManager::ntpEnabled);   drawTimeSettings(); return;
        case 1: tzSelectMode = true;                                    drawTimeSettings(); return;
        case 2: TimeManager::setTimeFormat(!TimeManager::use24hFormat); drawTimeSettings(); return;
        default: currentState = 10; return; // STATE_SETTINGS_TIME_MANUAL
        }
    }

    if (m.inFooter((int16_t)y) && m.footerButtonFromX((int16_t)x) == UI_FOOTER_SEL) {
        currentState = 1; // STATE_SETTINGS
    }
}

// ----------------------------------------------------
// MANUAL TIME MENU
// ----------------------------------------------------

static int mDay = 1, mMonth = 1, mYear = 2026, mHour = 12, mMinute = 0;
static bool loadedManual = false;

// An up triangle, a value box and a down triangle, all anchored to the spinner rect.
static void drawTimeSpinner(KryonDisplay* t, const UiMetrics& m, const UiRect& r, const String& val) {
    const int16_t triH = (int16_t)(m.rowH / 2);
    const int16_t boxY = spinnerBoxY(r, m);

    t->fillTriangle(r.cx(), r.y, (int16_t)(r.right() - 5), (int16_t)(r.y + triH),
                    (int16_t)(r.x + 5), (int16_t)(r.y + triH), TFT_GREEN);
    t->fillRoundRect(r.x, boxY, r.w, m.rowH, 4, TFT_DARKGREY);
    t->setTextColor(TFT_WHITE, TFT_DARKGREY);
    t->setTextDatum(MC_DATUM);
    t->drawString(val, r.cx(), (int16_t)(boxY + m.rowH / 2), m.fontBody);

    const int16_t downY = (int16_t)(boxY + m.rowH + 5);
    t->fillTriangle((int16_t)(r.x + 5), downY, (int16_t)(r.right() - 5), downY,
                    r.cx(), (int16_t)(downY + triH), TFT_RED);
}

void SettingsUI::drawTimeManual() {
    if (!tftInstance) return;

    if (!loadedManual) {
        mYear = TimeManager::getYear();
        mMonth = TimeManager::getMonth();
        mDay = TimeManager::getDay();

        time_t now; time(&now);
        struct tm tinfo; localtime_r(&now, &tinfo);
        mHour = tinfo.tm_hour;
        mMinute = tinfo.tm_min;
        loadedManual = true;
    }

    const UiMetrics& m = M();
    drawSettingsFrame(tftInstance, "Set Time", TFT_PURPLE, TFT_PURPLE);

    // Date Line
    drawTimeSpinner(tftInstance, m, dateSpinnerRect(m, 0), String(mDay));
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("/", dateSeparatorX(m, 0), dateSpinnerRect(m, 0).cy(), m.fontBody);
    drawTimeSpinner(tftInstance, m, dateSpinnerRect(m, 1), String(mMonth));
    tftInstance->drawString("/", dateSeparatorX(m, 1), dateSpinnerRect(m, 1).cy(), m.fontBody);
    drawTimeSpinner(tftInstance, m, dateSpinnerRect(m, 2), String(mYear));

    // Time Line
    drawTimeSpinner(tftInstance, m, timeSpinnerRect(m, 0), String(mHour));
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString(":", m.centerX, timeSpinnerRect(m, 0).cy(), m.fontHeader);
    char mBuf[8]; snprintf(mBuf, sizeof(mBuf), "%02d", mMinute);
    drawTimeSpinner(tftInstance, m, timeSpinnerRect(m, 1), String(mBuf));

    // Save Footer
    tftInstance->fillRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_GREEN);
    tftInstance->setTextColor(TFT_BLACK, TFT_GREEN);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("SAVE & BACK", m.footerButtonCenterX(UI_FOOTER_SEL),
                            m.footerTextY, m.fontBody);
}

void SettingsUI::handleTimeManualTouch(uint16_t x, uint16_t y) {
    extern int currentState;
    const UiMetrics& m = M();

    // Tapping the top half of a spinner increments it, the bottom half decrements it; both wrap.
    auto checkClick = [&](const UiRect& r, int& val, int minV, int maxV) {
        if (spinnerUpZone(r, m).contains((int16_t)x, (int16_t)y)) {
            if (++val > maxV) val = minV;
            drawTimeManual();
        } else if (spinnerDownZone(r, m).contains((int16_t)x, (int16_t)y)) {
            if (--val < minV) val = maxV;
            drawTimeManual();
        }
    };

    // Date Line
    checkClick(dateSpinnerRect(m, 0), mDay, 1, 31);
    checkClick(dateSpinnerRect(m, 1), mMonth, 1, 12);
    checkClick(dateSpinnerRect(m, 2), mYear, 2000, 2100);

    // Time Line
    checkClick(timeSpinnerRect(m, 0), mHour, 0, 23);
    checkClick(timeSpinnerRect(m, 1), mMinute, 0, 59);

    if (m.inFooter((int16_t)y)) {
        TimeManager::setManualTime(mYear, mMonth, mDay, mHour, mMinute);
        loadedManual = false;
        currentState = 9; // STATE_SETTINGS_TIME
    }
}

// ----------------------------------------------------
// WIFI SCANNER AND CONNECT UI
// ----------------------------------------------------

namespace {
// The scanner list: one card per access point, plus a Cancel / Next Page bar pinned above the footer.
int16_t scanCardH(const UiMetrics& m)     { return (int16_t)(m.rowH + 12); }
int16_t scanCardPitch(const UiMetrics& m) { return (int16_t)(m.rowH + 16); }
int16_t scanCardsTop(const UiMetrics& m)  { return (int16_t)(m.header.bottom() + 10); }

int scanCardsPerPage(const UiMetrics& m) {
    const int pitch = scanCardPitch(m);
    if (pitch <= 0) return 1;
    const int avail = m.footer.y - scanCardsTop(m);
    const int n = (avail - scanCardH(m)) / pitch + 1;
    return (n < 1) ? 1 : n;
}

UiRect scanCardRect(const UiMetrics& m, int visibleIndex) {
    return { m.list.x, (int16_t)(scanCardsTop(m) + visibleIndex * scanCardPitch(m)),
             m.list.w, scanCardH(m) };
}

UiRect scanCancelButton(const UiMetrics& m) {
    return { m.list.x, (int16_t)(m.footer.y - 7), (int16_t)min((int)100, (int)(m.list.w / 2)), 32 };
}

UiRect scanNextButton(const UiMetrics& m) {
    const int16_t w = (int16_t)min((int)100, (int)(m.list.w / 2));
    return { (int16_t)(m.list.right() - w), (int16_t)(m.footer.y - 7), w, 32 };
}
} // namespace

void SettingsUI::scanAndConnectWiFi() {
    const UiMetrics& m = M();
    drawSettingsFrame(tftInstance, "WiFi Scanner");

    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Scanning 2.4GHz Networks...", m.centerX, (int16_t)(m.centerY - 20),
                            m.fontBody);
    // This function blocks, so the main loop cannot flush the frame -- see the note in
    // MyKeyboard::getString(). Without this the scanner page is drawn into a canvas nobody copies to
    // the panel, and the screen keeps showing the WiFi menu while this runs.
    tftInstance->present();

    // Initialize WiFi in Station Mode and start async scan
    WiFi.mode(WIFI_STA);
    WiFi.disconnect();
    delay(50);
    WiFi.scanNetworks(true); // Async scan

    // Animated spinner while scanning
    //
    // Bounded, and that is the point. arduino's async scan reports WIFI_SCAN_RUNNING until its OWN
    // timeout, which defaults to 60 SECONDS (WiFiScanClass::_scanTimeout), so a scan that never
    // completes pins this loop -- and with it the whole UI, since this runs inside the main loop's
    // touch handling -- for a full minute with no way out but the reset button. Give up sooner; the
    // message after the loop distinguishes "timed out" from "found nothing".
    constexpr uint32_t kScanBudgetMs = 15000;
    const uint32_t scanStartedAt = millis();
    bool scanTimedOut = false;
    int spinAngle = 0;
    int16_t scanStatus = WIFI_SCAN_RUNNING;
    const int16_t spinY = (int16_t)(m.centerY + 30);
    const int16_t spinR = (int16_t)max(6, min(18, (int)(m.list.h / 12)));
    while ((scanStatus = WiFi.scanComplete()) == WIFI_SCAN_RUNNING) {
        // Draw spinning radar / circle
        tftInstance->drawCircle(m.centerX, spinY, spinR, TFT_DARKGREY);
        float rad = spinAngle * (PI / 180.0f);
        int px = m.centerX + (int)(cos(rad) * spinR);
        int py = spinY + (int)(sin(rad) * spinR);
        tftInstance->fillCircle(px, py, 4, TFT_CYAN);
        tftInstance->present(); // the spinner has to reach the panel too, or it is not a spinner
        delay(40);
        tftInstance->fillCircle(px, py, 4, TFT_BLACK); // clear dot
        spinAngle = (spinAngle + 30) % 360;
        esp_task_wdt_reset();
        if ((uint32_t)(millis() - scanStartedAt) > kScanBudgetMs) {
            scanTimedOut = true;
            break;
        }
    }

    int n = WiFi.scanComplete();
    // A scan that ended without a result is a timeout whichever way it ended: our own budget, or the
    // timeout inside WiFiScanClass that setScanTimeout() caps at 15s (which reports WIFI_SCAN_FAILED,
    // not WIFI_SCAN_RUNNING). Only a completed scan with no APs is genuinely "none found".
    if (n < 0) scanTimedOut = true;

    if (n <= 0) {
        tftInstance->fillScreen(TFT_BLACK);
        tftInstance->setTextColor(TFT_RED, TFT_BLACK);
        tftInstance->setTextDatum(MC_DATUM);
        // A scan that never finished is a different claim from an empty one: reporting "no networks
        // found" when none were ever looked for would send the wrong fix.
        tftInstance->drawString(scanTimedOut ? "Scan timed out." : "No networks found.",
                                m.centerX, m.centerY, m.fontBody);
        tftInstance->present();
        delay(1500);
        drawWiFi();
        return;
    }

    int currentPage = 0;
    const int networksPerPage = scanCardsPerPage(m);
    int totalPages = (n + networksPerPage - 1) / networksPerPage;
    auto savedNets = WiFiManager::getSavedNetworks();

    while (true) {
        drawSettingsFrame(tftInstance, ("Select Network (" + String(n) + ")").c_str());

        const int startIdx = currentPage * networksPerPage;
        int endIdx = startIdx + networksPerPage;
        if (endIdx > n) endIdx = n;

        for (int i = startIdx; i < endIdx; i++) {
            const UiRect card = scanCardRect(m, i - startIdx);
            String ssid = WiFi.SSID(i);
            int rssi = WiFi.RSSI(i);
            bool isSaved = false;
            for (const auto& s : savedNets) {
                if (s.ssid.equalsIgnoreCase(ssid)) { isSaved = true; break; }
            }

            // Draw card
            const uint16_t cardBg = isSaved ? 0x18C3 : TFT_DARKGREY;
            tftInstance->fillRoundRect(card.x, card.y, card.w, card.h, 5, cardBg);
            tftInstance->drawRoundRect(card.x, card.y, card.w, card.h, 5,
                                       isSaved ? TFT_CYAN : TFT_WHITE);

            String displaySSID = ssid;
            if (displaySSID.length() > 14) displaySSID = displaySSID.substring(0, 12) + "..";

            tftInstance->setTextColor(TFT_WHITE, cardBg);
            tftInstance->setTextDatum(TL_DATUM);
            tftInstance->drawString(displaySSID, (int16_t)(card.x + 8), (int16_t)(card.y + 6),
                                    m.fontBody);

            // Signal bars
            int bars = 1;
            if (rssi >= -55) bars = 4;
            else if (rssi >= -65) bars = 3;
            else if (rssi >= -75) bars = 2;

            const int sx = card.x + 135;
            const int sy = card.y + card.h - 20;
            for (int b = 1; b <= 4; b++) {
                const uint16_t bColor = (b <= bars) ? TFT_GREEN : 0x4208;
                tftInstance->fillRect(sx + (b - 1) * 4, sy - (b * 2), 3, b * 2, bColor);
            }

            // Saved / Open / Secure marker, right-aligned in the card
            tftInstance->setTextDatum(MR_DATUM);
            if (isSaved) {
                tftInstance->setTextColor(TFT_CYAN, cardBg);
                tftInstance->drawString("SAVED", (int16_t)(card.right() - 30),
                                        (int16_t)(card.y + 14), m.fontBody);
            } else if (WiFi.encryptionType(i) == WIFI_AUTH_OPEN) {
                tftInstance->setTextColor(TFT_GREEN, cardBg);
                tftInstance->drawString("OPEN", (int16_t)(card.right() - 30),
                                        (int16_t)(card.y + 14), m.fontBody);
            } else {
                tftInstance->setTextColor(TFT_RED, cardBg);
                tftInstance->drawString("SECURE", (int16_t)(card.right() - 30),
                                        (int16_t)(card.y + 14), m.fontBody);
            }
        }

        // Draw pagination or Cancel
        const UiRect cancel = scanCancelButton(m);
        tftInstance->fillRoundRect(cancel.x, cancel.y, cancel.w, cancel.h, 5, TFT_RED);
        tftInstance->setTextColor(TFT_WHITE, TFT_RED);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("Cancel", cancel.cx(), cancel.cy(), m.fontBody);

        if (totalPages > 1) {
            const UiRect next = scanNextButton(m);
            tftInstance->fillRoundRect(next.x, next.y, next.w, next.h, 5, TFT_BLUE);
            tftInstance->setTextColor(TFT_WHITE, TFT_BLUE);
            tftInstance->drawString("Next Page", next.cx(), next.cy(), m.fontBody);
        }
        // The page is complete; put it on the panel before waiting for a tap, or the wait is for a
        // tap on something the user cannot see.
        tftInstance->present();

        // Touch handling loop
        uint16_t tx = 0, ty = 0;
        bool touched = false;
        while (!touched) {
            if (TouchDriver::getTouch(&tx, &ty)) {
                while (TouchDriver::getTouch(&tx, &ty)) { delay(10); }
                touched = true;
            }
            delay(50);
            esp_task_wdt_reset();
        }

        // Check if Cancel tapped
        if (scanCancelButton(m).contains((int16_t)tx, (int16_t)ty)) {
            drawWiFi();
            return;
        }

        // Check if Next Page tapped
        if (totalPages > 1 && scanNextButton(m).contains((int16_t)tx, (int16_t)ty)) {
            currentPage++;
            if (currentPage >= totalPages) currentPage = 0;
            continue; // redraw
        }

        // Check if a network was tapped
        int tappedIndex = -1;
        for (int i = startIdx; i < endIdx; i++) {
            if (scanCardRect(m, i - startIdx).contains((int16_t)tx, (int16_t)ty)) {
                tappedIndex = i;
                break;
            }
        }

        if (tappedIndex != -1) {
            String selectedSSID = WiFi.SSID(tappedIndex);
            selectedSSID.trim();
            String password = "";

            // Check if already in saved networks
            for (const auto& s : savedNets) {
                if (s.ssid.equalsIgnoreCase(selectedSSID)) {
                    password = s.password;
                    break;
                }
            }

            if (password.length() == 0 && WiFi.encryptionType(tappedIndex) != WIFI_AUTH_OPEN) {
                // Ask for password
                String promptMsg = "Password for " + selectedSSID;
                password = MyKeyboard::getString("", promptMsg, 64);
                password.trim();
                if (password.length() == 0) {
                    continue; // Canceled typing password
                }
            }

            tftInstance->fillScreen(TFT_BLACK);
            tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("Connecting to", m.centerX, (int16_t)(m.centerY - 20), m.fontBody);
            tftInstance->drawString(selectedSSID + "...", m.centerX, (int16_t)(m.centerY + 5),
                                    m.fontBody);
            tftInstance->present();

            bool success = WiFiManager::connectTo(selectedSSID, password, 10000);

            tftInstance->fillScreen(TFT_BLACK);
            tftInstance->setTextColor(success ? TFT_GREEN : TFT_RED, TFT_BLACK);
            tftInstance->drawString(success ? "Connected Successfully!" : "Connection Failed!",
                                    m.centerX, m.centerY, m.fontBody);
            tftInstance->present();
            delay(1200);

            drawWiFi();
            return;
        }
    }
}

// ----------------------------------------------------
// SYSTEM UPDATER
// ----------------------------------------------------

static bool updaterHasUpdate = false;
static bool updaterFetchFailed = false;
static bool updaterIsFromBoot = false;
static String updaterVersion = "";
static String updaterApi = "";
static String updaterChangelog = "";
static String updaterGuide = "";
static String updaterType = "";

static bool isVerGreater(const String& newVer, const String& oldVer) {
    int newParts[3] = {0,0,0}, oldParts[3] = {0,0,0};
    auto parseV = [](const String& v, int* p) {
        int pt = 0, st = 0;
        while(pt<3 && st<(int)v.length()){
            int d = v.indexOf('.', st);
            if(d==-1) { p[pt] = v.substring(st).toInt(); break; }
            p[pt] = v.substring(st, d).toInt();
            st = d+1; pt++;
        }
    };
    parseV(newVer, newParts);
    parseV(oldVer, oldParts);
    if(newParts[0] > oldParts[0]) return true;
    if(newParts[0] < oldParts[0]) return false;
    if(newParts[1] > oldParts[1]) return true;
    if(newParts[1] < oldParts[1]) return false;
    if(newParts[2] > oldParts[2]) return true;
    return false;
}

bool SettingsUI::checkUpdateSilent() {
    return OTAManager::checkUpdate(false);
}

// The OTA screens draw their own 1px-inset cyan/red frame rather than the Settings chrome.
static UiRect otaPanel(const UiMetrics& m) {
    return { (int16_t)(m.frame.x + 1), (int16_t)(m.frame.y + 1),
             (int16_t)(m.frame.w - 2), (int16_t)(m.frame.h - 2) };
}

// The big centred readout: font 6 is 48px tall, which only fits a full-height panel.
static uint8_t otaBigFont(const UiMetrics& m)   { return (m.h >= 240) ? 6 : (uint8_t)m.fontHeader; }
static uint8_t otaTitleFont(const UiMetrics& m) { return (m.h >= 240) ? 4 : (uint8_t)m.fontHeader; }

static UiRect otaBarRect(const UiMetrics& m) {
    const int16_t h = (int16_t)max(8, (int)(m.rowH * 2 / 3));
    return { (int16_t)(m.list.x + 8), (int16_t)(m.centerY - 15), (int16_t)(m.list.w - 16), h };
}

static UiRect otaErrorHeader(const UiMetrics& m) {
    return { m.list.x, (int16_t)(m.header.y + 4), m.list.w, (int16_t)(m.header.h + 6) };
}

// The two stacked buttons at the foot of the error screen; the card above fills the gap between
// the header and the retry button.
static UiRect otaErrorBackButton(const UiMetrics& m) {
    return { (int16_t)(m.list.x + 5), (int16_t)(m.footer.y - 13), (int16_t)(m.list.w - 10),
             (int16_t)(m.header.h + 6) };
}

static UiRect otaErrorRetryButton(const UiMetrics& m) {
    const UiRect back = otaErrorBackButton(m);
    return { back.x, (int16_t)(back.y - 48), back.w, (int16_t)(back.h + 2) };
}

static UiRect otaErrorCard(const UiMetrics& m) {
    const int16_t top = (int16_t)(otaErrorHeader(m).bottom() + 10);
    return { m.list.x, top, m.list.w, (int16_t)(otaErrorRetryButton(m).y - 10 - top) };
}

// Shared by both updater states: the dismiss chip sits just above the footer, and the install chip
// stacks 44px above it.
static UiRect updaterDismissButton(const UiMetrics& m) {
    return { m.list.x, (int16_t)(m.footer.y - 7), m.list.w, 32 };
}

static UiRect updaterInstallButton(const UiMetrics& m) {
    return { m.list.x, (int16_t)(updaterDismissButton(m).y - 44), m.list.w, 36 };
}

void SettingsUI::drawOTAProgress(int percent, size_t currentBytes, size_t totalBytes, float speedKBs, const String& status) {
    if (!tftInstance) return;

    const UiMetrics& m = M();
    tftInstance->fillScreen(TFT_BLACK);
    const UiRect panel = otaPanel(m);
    tftInstance->drawRoundRect(panel.x, panel.y, panel.w, panel.h, 6, TFT_CYAN);

    tftInstance->setTextDatum(TC_DATUM);
    tftInstance->setTextColor(TFT_CYAN, TFT_BLACK);
    tftInstance->drawString("KRYONOS FIRMWARE OTA", m.centerX, (int16_t)(m.header.y + 10), m.fontBody);

    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->drawString("Downloading & Flashing...", m.centerX,
                            (int16_t)(m.header.bottom() + 6), m.fontBody);

    // Main Percentage
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->drawString(String(percent) + "%", m.centerX, m.listMessageY, otaBigFont(m));

    // Progress Bar Outline
    const UiRect bar = otaBarRect(m);
    tftInstance->drawRoundRect(bar.x, bar.y, bar.w, bar.h, 4, TFT_WHITE);
    tftInstance->fillRect(bar.x + 2, bar.y + 2, bar.w - 4, bar.h - 4, TFT_BLACK);

    // Filled bar
    const int fillW = (percent * (bar.w - 4)) / 100;
    if (fillW > 0) {
        const uint16_t barColor = (percent < 50) ? TFT_CYAN : TFT_GREEN;
        tftInstance->fillRect(bar.x + 2, bar.y + 2, fillW, bar.h - 4, barColor);
    }

    // Byte Counter
    const int16_t bytesY  = (int16_t)(bar.bottom() + 13);
    const int16_t statusY = (int16_t)(bytesY + 26);
    tftInstance->setTextDatum(TC_DATUM);
    tftInstance->setTextColor(TFT_YELLOW, TFT_BLACK);
    const float currMb = currentBytes / (1024.0f * 1024.0f);
    const float totMb = totalBytes / (1024.0f * 1024.0f);
    char buf[64];
    sprintf(buf, "%.2f MB / %.2f MB (%.1f kB/s)", currMb, totMb, speedKBs);
    tftInstance->drawString(buf, m.centerX, bytesY, m.fontBody);

    // Status Message
    tftInstance->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tftInstance->drawString(status.c_str(), m.centerX, statusY, m.fontBody);

    // Critical Safety Notice, pinned above the panel's lower edge
    tftInstance->setTextColor(TFT_RED, TFT_BLACK);
    tftInstance->drawString("DO NOT POWER OFF DEVICE", m.centerX,
                            (int16_t)(m.centerY + 85), m.fontBody);
    tftInstance->setTextColor(TFT_DARKGREY, TFT_BLACK);
    tftInstance->drawString("Anti-rollback protection active", m.centerX,
                            (int16_t)(m.centerY + 105), m.fontSmall);

    // Flush each step: the download and flash call this from inside their own loop, so the main loop
    // never gets a turn until the update is over. See the note in MyKeyboard::getString().
    tftInstance->present();
}

void SettingsUI::drawOTAError(const String& errorMsg) {
    otaErrorShown = true;
    if (!tftInstance) return;

    const UiMetrics& m = M();
    const UiRect panel = otaPanel(m);
    const UiRect head = otaErrorHeader(m);
    const UiRect card = otaErrorCard(m);
    const UiRect retry = otaErrorRetryButton(m);
    const UiRect back = otaErrorBackButton(m);

    tftInstance->fillScreen(TFT_BLACK);
    tftInstance->drawRoundRect(panel.x, panel.y, panel.w, panel.h, 6, TFT_RED);

    // Warning Header
    tftInstance->fillRoundRect(head.x, head.y, head.w, head.h, 5, TFT_RED);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->setTextColor(TFT_WHITE, TFT_RED);
    tftInstance->drawString("UPDATE FAILED", head.cx(), head.cy(), otaTitleFont(m));

    // Error Details Card
    tftInstance->fillRoundRect(card.x, card.y, card.w, card.h, 5, 0x1800); // Deep maroon
    tftInstance->drawRoundRect(card.x, card.y, card.w, card.h, 5, TFT_RED);

    tftInstance->setTextDatum(TC_DATUM);
    tftInstance->setTextColor(TFT_YELLOW, 0x1800);
    tftInstance->drawString("Error Reason:", card.cx(), (int16_t)(card.y + 12), m.fontBody);

    tftInstance->setTextColor(TFT_WHITE, 0x1800);
    // Wrap error string across lines, stopping short of the closing reassurance line
    const int16_t wrapBottom = (int16_t)(card.bottom() - 44);
    int y = card.y + 36;
    int start = 0;
    while (start < (int)errorMsg.length() && y < wrapBottom) {
        int lEnd = start + 26;
        if (lEnd >= (int)errorMsg.length()) lEnd = errorMsg.length();
        else {
            int space = errorMsg.lastIndexOf(' ', lEnd);
            if (space > start) lEnd = space;
        }
        tftInstance->drawString(errorMsg.substring(start, lEnd).c_str(), card.cx(), y, m.fontBody);
        y += 18;
        start = lEnd;
        if (start < (int)errorMsg.length() && errorMsg[start] == ' ') start++;
    }

    tftInstance->setTextColor(TFT_GREEN, 0x1800);
    tftInstance->drawString("Previous OS safe & intact.", card.cx(),
                            (int16_t)(card.bottom() - 29), m.fontBody);

    // Button 1: Retry
    tftInstance->fillRoundRect(retry.x, retry.y, retry.w, retry.h, 5, TFT_YELLOW);
    tftInstance->setTextColor(TFT_BLACK, TFT_YELLOW);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("RETRY UPDATE", retry.cx(), retry.cy(), m.fontBody);

    // Button 2: Back / Exit (outlined rather than filled)
    tftInstance->drawRoundRect(back.x, back.y, back.w, back.h, 5, TFT_WHITE);
    tftInstance->fillRect(back.x + 1, back.y + 1, back.w - 2, back.h - 2, TFT_BLACK);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("BACK TO SETTINGS", back.cx(), back.cy(), m.fontBody);
}

void SettingsUI::drawUpdater(bool isBootCheck) {
    const UiMetrics& m = M();
    const UiRect frame = m.frame;
    const UiRect dismiss = updaterDismissButton(m);
    const UiRect install = updaterInstallButton(m);

    updaterIsFromBoot = isBootCheck;
    otaErrorShown = false;
    tftInstance->fillScreen(TFT_BLACK);
    tftInstance->drawRoundRect(frame.x, frame.y, frame.w, frame.h, 5, TFT_WHITE);

    if (WiFi.status() != WL_CONNECTED) {
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->setTextColor(TFT_RED, TFT_BLACK);
        tftInstance->drawString("No WiFi Connection!", m.centerX, (int16_t)(m.centerY - 20), m.fontBody);
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->drawString("Please turn on WiFi", m.centerX, m.centerY, m.fontBody);
        tftInstance->drawString("first in Settings.", m.centerX, (int16_t)(m.centerY + 20), m.fontBody);

        tftInstance->drawRoundRect(dismiss.x, dismiss.y, dismiss.w, dismiss.h, 5, TFT_WHITE);
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString(isBootCheck ? "CLOSE" : "BACK", dismiss.cx(), dismiss.cy(),
                                m.fontBody);
        return;
    }

    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Checking for updates...", m.centerX, m.centerY, m.fontBody);
    tftInstance->present(); // the version check below blocks on the network

    bool hasUpdate = OTAManager::checkUpdate(isBootCheck);
    const OTAUpdateInfo& info = OTAManager::getUpdateInfo();

    if (isBootCheck && (!hasUpdate || info.fetchFailed)) {
        extern int currentState;
        currentState = 0; // STATE_LAUNCHER
        return;
    }

    tftInstance->fillScreen(TFT_BLACK);
    tftInstance->drawRoundRect(frame.x, frame.y, frame.w, frame.h, 5, TFT_WHITE);

    if (info.fetchFailed) {
        tftInstance->setTextColor(TFT_RED, TFT_BLACK);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("Failed to check", m.centerX, (int16_t)(m.centerY - 20), m.fontBody);
        tftInstance->drawString("for updates!", m.centerX, m.centerY, m.fontBody);
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->drawString("Check your connection", m.centerX, (int16_t)(m.centerY + 30),
                                m.fontBody);
    } else if (!hasUpdate) {
        tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("System is up to date!", m.centerX, (int16_t)(m.centerY - 15),
                                m.fontBody);
        tftInstance->setTextColor(TFT_CYAN, TFT_BLACK);
        tftInstance->drawString(String("Current: v") + KRYONOS_VERSION, m.centerX,
                                (int16_t)(m.centerY + 10), m.fontBody);
    } else {
        tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
        tftInstance->setTextDatum(TC_DATUM);
        tftInstance->drawString(info.updateType.c_str(), m.centerX,
                                (int16_t)(m.header.y + 4), m.fontBody);

        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->drawString(String("v") + KRYONOS_VERSION + " -> v" + info.version, m.centerX,
                                (int16_t)(m.header.y + 22), m.fontBody);

        // Text flows down from the header and must stop above the install chip. When a guide is
        // present the changelog gets the upper half and the guide the lower.
        const bool hasGuide = info.guide.length() > 0;
        const int16_t installTop = install.y;
        const int16_t maxChangelogY = (int16_t)(hasGuide ? installTop - 94 : installTop - 49);
        const int16_t maxGuideY = (int16_t)(installTop - 29);

        int y = m.header.bottom() + 12;
        tftInstance->setTextColor(TFT_YELLOW, TFT_BLACK);
        tftInstance->setTextDatum(TL_DATUM);
        tftInstance->drawString("What's New:", (int16_t)(m.list.x + 5), y, m.fontBody); y += 15;

        tftInstance->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
        int start = 0;
        while (start < (int)info.changelog.length() && y < maxChangelogY) {
            int nl = info.changelog.indexOf('\n', start);
            String line;
            if (nl == -1) { line = info.changelog.substring(start); start = info.changelog.length(); }
            else { line = info.changelog.substring(start, nl); start = nl + 1; }

            int lStart = 0;
            while (lStart < (int)line.length() && y < maxChangelogY) {
                int lEnd = lStart + 30;
                if (lEnd >= (int)line.length()) lEnd = line.length();
                else { int space = line.lastIndexOf(' ', lEnd); if (space > lStart) lEnd = space; }
                tftInstance->drawString(line.substring(lStart, lEnd).c_str(),
                                        (int16_t)(m.list.x + 5), y, m.fontBody);
                y += 14;
                lStart = lEnd;
                if (lStart < (int)line.length() && line[lStart] == ' ') lStart++;
            }
        }

        // Render Guide if present and non-empty
        if (hasGuide) {
            y += 4;
            tftInstance->setTextColor(TFT_YELLOW, TFT_BLACK);
            tftInstance->drawString("How to Install:", (int16_t)(m.list.x + 5), y, m.fontBody); y += 14;
            tftInstance->setTextColor(TFT_CYAN, TFT_BLACK);
            int gStart = 0;
            while (gStart < (int)info.guide.length() && y < maxGuideY) {
                int nl = info.guide.indexOf('\n', gStart);
                String line;
                if (nl == -1) { line = info.guide.substring(gStart); gStart = info.guide.length(); }
                else { line = info.guide.substring(gStart, nl); gStart = nl + 1; }

                int lStart = 0;
                while (lStart < (int)line.length() && y < maxGuideY) {
                    int lEnd = lStart + 30;
                    if (lEnd >= (int)line.length()) lEnd = line.length();
                    else { int space = line.lastIndexOf(' ', lEnd); if (space > lStart) lEnd = space; }
                    tftInstance->drawString(line.substring(lStart, lEnd).c_str(),
                                            (int16_t)(m.list.x + 5), y, m.fontBody);
                    y += 14;
                    lStart = lEnd;
                    if (lStart < (int)line.length() && line[lStart] == ' ') lStart++;
                }
            }
        }

        if (info.firmwareSize > 0) {
            y += 4;
            tftInstance->setTextColor(TFT_CYAN, TFT_BLACK);
            float szMb = info.firmwareSize / (1024.0f * 1024.0f);
            char szBuf[32];
            sprintf(szBuf, "Firmware Size: %.2f MB", szMb);
            tftInstance->drawString(szBuf, (int16_t)(m.list.x + 5), y, m.fontBody);
        }

        // INSTALL UPDATE Button (only if board supports OTA)
        if (info.supportsOta) {
            tftInstance->fillRoundRect(install.x, install.y, install.w, install.h, 5, TFT_GREEN);
            tftInstance->drawRoundRect(install.x, install.y, install.w, install.h, 5, TFT_WHITE);
            tftInstance->setTextColor(TFT_BLACK, TFT_GREEN);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("INSTALL UPDATE", install.cx(), install.cy(), m.fontBody);
        }
    }

    // Bottom Dismiss Button
    tftInstance->drawRoundRect(dismiss.x, dismiss.y, dismiss.w, dismiss.h, 5, TFT_WHITE);
    tftInstance->fillRect(dismiss.x + 1, dismiss.y + 1, dismiss.w - 2, dismiss.h - 2, TFT_BLACK);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString(isBootCheck ? "CLOSE" : "BACK", dismiss.cx(), dismiss.cy(), m.fontBody);
}

void SettingsUI::handleUpdaterTouch(uint16_t x, uint16_t y) {
    extern int currentState;

    const UiMetrics& m = M();
    const UiRect retry = otaErrorRetryButton(m);
    const UiRect back = otaErrorBackButton(m);
    const UiRect install = updaterInstallButton(m);
    const UiRect dismiss = updaterDismissButton(m);

    if (otaErrorShown) {
        // Retry button
        if (retry.contains((int16_t)x, (int16_t)y)) {
            otaErrorShown = false;
            const OTAUpdateInfo& info = OTAManager::getUpdateInfo();
            drawOTAProgress(0, 0, info.firmwareSize, 0, "Reconnecting...");
            bool success = OTAManager::startFlashUpdate([](const OTAProgress& p) {
                SettingsUI::drawOTAProgress(p.percent, p.downloadedBytes, p.totalBytes, p.speedKBs, p.statusMessage);
            });
            if (!success) {
                const OTAProgress& prog = OTAManager::getProgress();
                drawOTAError(prog.errorMessage);
            }
            return;
        }
        // Back to settings button
        if (back.contains((int16_t)x, (int16_t)y)) {
            otaErrorShown = false;
            currentState = updaterIsFromBoot ? 0 : 1;
            return;
        }
        return;
    }

    const OTAUpdateInfo& info = OTAManager::getUpdateInfo();

    // 1. "INSTALL UPDATE" Button (only if supports_ota is true)
    if (info.hasUpdate && info.supportsOta && install.contains((int16_t)x, (int16_t)y)) {
        drawOTAProgress(0, 0, info.firmwareSize, 0, "Initializing Flash Stream...");
        bool success = OTAManager::startFlashUpdate([](const OTAProgress& p) {
            SettingsUI::drawOTAProgress(p.percent, p.downloadedBytes, p.totalBytes, p.speedKBs, p.statusMessage);
        });
        if (!success) {
            const OTAProgress& prog = OTAManager::getProgress();
            drawOTAError(prog.errorMessage);
        }
        return;
    }

    // 2. "BACK / CLOSE" Button
    if (dismiss.contains((int16_t)x, (int16_t)y)) {
        if (updaterIsFromBoot) {
            currentState = 0; // STATE_LAUNCHER
        } else {
            currentState = 1; // STATE_SETTINGS
        }
    }
}

