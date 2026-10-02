#include "HelpCenterUI.h"
#include "../FileSystem/FileSystem.h"
#include "../UI/UiLayout.h"
#include <WiFi.h>
#include <HTTPClient.h>
#include <LittleFS.h>

// Current screen metrics. See Documentation/Display_Touch_Architecture.md.
static inline const UiMetrics& M() { return UiLayout::current(); }

// --- marquee ---------------------------------------------------------------------------------
// Help titles and topic names can be wider than the box that holds them. Rather than clipping, the
// UI scrolls the text through its box. That loop used to be copy-pasted four times with three
// different hard-coded widths (200 to trigger, 210 in the header, 190 in a row); these helpers fold
// it into one place and take the widths as arguments so they follow the panel.
//
// The historical 240x320 numbers are reproduced as *insets* on the containing box, which is what
// makes them scale: header 228-28 = 200 (trigger) and 228-18 = 210 (window), list 220-30 = 190.
namespace {
const int MARQUEE_GAP = 6;   // spaces appended so a repeat is visibly separated from its predecessor

String marqueeSlice(KryonDisplay* tft, const String& text, int pos, int maxWidth, uint8_t font) {
    String scrollText = text + "      ";
    const int len = scrollText.length();
    String out = "";
    for (int i = 0; i < len; i++) {
        const char c = scrollText.charAt((pos + i) % len);
        if (tft->textWidth(out + c, font) > maxWidth) break;
        out += c;
    }
    return out;
}

// --- viewer footer ---------------------------------------------------------------------------
// The viewer draws BACK / UP / DN at the centre of each third of the width — 40 / 120 / 200 at
// 240x320. Hit-testing the same thirds is what makes a tap land on the label it looks like it
// should; the historical hit zones (70 / 160) did not line up with the drawn labels at all.
inline int16_t thirdCenterX(const UiMetrics& m, int which) {
    return (int16_t)((int32_t)m.w * (2 * which + 1) / 6);
}

inline int viewerZoneFromX(const UiMetrics& m, int16_t x) {
    if (x < (int16_t)(m.w / 3))               return 0;   // BACK
    if (x < (int16_t)(2 * (m.w / 3)))         return 1;   // UP
    return 2;                                             // DN
}
} // namespace

KryonDisplay* HelpCenterUI::tftInstance = nullptr;

int HelpCenterUI::uiState = 0;
int HelpCenterUI::selectedIndex = 0;
int HelpCenterUI::scrollOffset = 0;
int HelpCenterUI::listCount = 0;
String HelpCenterUI::listItems[25];
String HelpCenterUI::listUrls[25];
int HelpCenterUI::selectedCategoryIndex = 0;

String HelpCenterUI::currentViewerTitle = "";
String HelpCenterUI::currentViewerContent = "";
int HelpCenterUI::viewerScrollOffset = 0;
String HelpCenterUI::dialogMessage = "";
int HelpCenterUI::titleScrollPos = 0;
unsigned long HelpCenterUI::lastTitleScrollTime = 0;

String HelpCenterUI::currentCategoryName = "";
int HelpCenterUI::listScrollPos = 0;
unsigned long HelpCenterUI::lastListScrollTime = 0;

// --- Offline Data ---
const char* offCats[] = {"Getting Started", "Basic Navigation", "Connectivity", "Troubleshooting"};
const int offCatCount = 4;

const char* offTopics0[] = {"What is KryonOS", "First setup guide", "System vs User apps"};
const char* offContent0[] = {
    "KryonOS is a fast, lightweight operating system built specifically for ESP32. It features an onboard app store, JavaScript app execution from SD, and a smooth UI interface.",
    "To get started, go to Settings -> WiFi to connect your device. Make sure a FAT32 formatted SD card is inserted if you plan to install new user apps.",
    "System apps (like Settings, Launcher) run deeply integrated in C++ for maximum speed. User apps run in KryonOS JavaScript Runtime from the SD card or local memory."
};

const char* offTopics1[] = {"Home screen overview", "Opening & closing apps"};
const char* offContent1[] = {
    "The Home screen lists system settings at the top and your installed user apps below. Use physical hardware buttons or touch controls to navigate up and down.",
    "Tap an app name to open it. To close any running user app, simply tap the red X button in the top right corner of the screen to return to the launcher."
};

const char* offTopics2[] = {"How to connect WiFi"};
const char* offContent2[] = {
    "Go to Settings -> WiFi. The device will scan networks. Tap an available network, enter the password using the on-screen keyboard, and press Connect. The device will reboot to apply."
};

const char* offTopics3[] = {"Black screen & Crash"};
const char* offContent3[] = {
    "If you experience a black screen or ESP crash when trying to run JS apps, please turn off WiFi. This will free up the RAM and allow your app to work fine."
};


void HelpCenterUI::init(KryonDisplay *tft) {
    tftInstance = tft;
}

void HelpCenterUI::draw() {
    if (!tftInstance) return;
    
    if (uiState == 0) drawMainMenu();
    else if (uiState == 1) drawList("Offline Categories");
    else if (uiState == 2) drawList(offCats[selectedCategoryIndex]);
    else if (uiState == 3) drawList("Online Categories");
    else if (uiState == 4) drawList(currentCategoryName);
    else if (uiState == 5) drawViewer();
    else if (uiState == 6) drawDialog();
}

void HelpCenterUI::drawMainMenu() {
    const UiMetrics& m = M();
    const UiRect offlineBtn = { (int16_t)(m.list.x + 10), (int16_t)(m.list.y + 35), (int16_t)(m.list.w - 20), 40 };
    const UiRect onlineBtn  = { (int16_t)(m.list.x + 10), (int16_t)(m.list.y + 95), (int16_t)(m.list.w - 20), 40 };
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_GREEN);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Help Center", m.header.cx(), m.headerTextY, m.fontBody);

    tftInstance->fillRect(m.list.x, m.list.y, m.list.w, m.list.h, TFT_BLACK);

    // Offline Button
    if (selectedIndex == 0) {
        tftInstance->fillRoundRect(offlineBtn.x, offlineBtn.y, offlineBtn.w, offlineBtn.h, 5, TFT_WHITE);
        tftInstance->setTextColor(TFT_BLACK, TFT_WHITE);
    } else {
        tftInstance->drawRoundRect(offlineBtn.x, offlineBtn.y, offlineBtn.w, offlineBtn.h, 5, TFT_WHITE);
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    }
    tftInstance->drawString("Offline Help Center", offlineBtn.cx(), offlineBtn.cy(), m.fontBody);

    // Online Button
    if (selectedIndex == 1) {
        tftInstance->fillRoundRect(onlineBtn.x, onlineBtn.y, onlineBtn.w, onlineBtn.h, 5, TFT_WHITE);
        tftInstance->setTextColor(TFT_BLACK, TFT_WHITE);
    } else {
        tftInstance->drawRoundRect(onlineBtn.x, onlineBtn.y, onlineBtn.w, onlineBtn.h, 5, TFT_WHITE);
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    }
    tftInstance->drawString("Online Help Center", onlineBtn.cx(), onlineBtn.cy(), m.fontBody);

    // Footer
    tftInstance->fillRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_WHITE);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("BACK", m.centerX, m.footerTextY, m.fontBody);
}

void HelpCenterUI::drawList(const String& title) {
    const UiMetrics& m = M();
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_GREEN);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);

    const int16_t hdrTrigger = (int16_t)(m.header.w - 28);   // 200 at 240x320
    const int16_t hdrWindow  = (int16_t)(m.header.w - 18);   // 210
    if (tftInstance->textWidth(title, m.fontBody) > hdrTrigger) {
        tftInstance->setTextDatum(ML_DATUM);
        tftInstance->drawString(marqueeSlice(tftInstance, title, titleScrollPos, hdrWindow, m.fontBody),
                                (int16_t)(m.header.x + 4), m.headerTextY, m.fontBody);
    } else {
        tftInstance->drawString(title, m.header.cx(), m.headerTextY, m.fontBody);
    }

    tftInstance->fillRect(m.list.x, m.list.y, m.list.w, m.list.h, TFT_BLACK);

    int itemsPerPage = m.itemsPerPage;
    const int16_t rowWindow = (int16_t)(m.list.w - 30);      // 190
    tftInstance->setTextDatum(ML_DATUM);                     // shared row baseline (see listRowTextY)

    for (int i=0; i<itemsPerPage; i++) {
        int idx = scrollOffset + i;
        if (idx >= listCount) break;

        const int16_t textX = (int16_t)(m.list.x + m.rowTextPadX);
        const int16_t textY = m.listRowTextY(i);
        if (idx == selectedIndex) {
            const UiRect fill = m.listRowFillRect(i);
            tftInstance->fillRect(fill.x, fill.y, fill.w, fill.h, TFT_WHITE);
            tftInstance->setTextColor(TFT_BLACK, TFT_WHITE);

            if (tftInstance->textWidth(listItems[idx], m.fontBody) > rowWindow) {
                tftInstance->drawString("> " + marqueeSlice(tftInstance, listItems[idx], listScrollPos,
                                                            rowWindow, m.fontBody),
                                        textX, textY, m.fontBody);
            } else {
                tftInstance->drawString("> " + listItems[idx], textX, textY, m.fontBody);
            }
        } else {
            tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
            tftInstance->drawString("  " + listItems[idx], textX, textY, m.fontBody);
        }
    }

    // Scrollbar
    if (listCount > itemsPerPage) {
        int thumbH = max((int)m.scrollThumbMin, (m.list.h * itemsPerPage) / listCount);
        int thumbY = m.list.y + (scrollOffset * (m.list.h - thumbH)) / (listCount - itemsPerPage);
        tftInstance->fillRect(m.scrollX, m.list.y, m.scrollW, m.list.h, TFT_DARKGREY);
        tftInstance->fillRect(m.scrollX, thumbY, m.scrollW, thumbH, TFT_WHITE);
    }

    tftInstance->fillRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_WHITE);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    // Shared four-zone footer (BACK / UP / SEL / DN) — the same helper the App Store uses, so the
    // labels sit at the centre of the zone that actually responds to a tap.
    tftInstance->drawString("BACK", m.footerSlotCenterX(UI_SLOT_BACK), m.footerTextY, m.fontBody);
    tftInstance->drawString("UP",   m.footerSlotCenterX(UI_SLOT_UP),   m.footerTextY, m.fontBody);
    tftInstance->drawString("SEL",  m.footerSlotCenterX(UI_SLOT_SEL),  m.footerTextY, m.fontBody);
    tftInstance->drawString("DN",   m.footerSlotCenterX(UI_SLOT_DN),   m.footerTextY, m.fontBody);
}

void HelpCenterUI::drawViewer() {
    const UiMetrics& m = M();
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_GREEN);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);

    const int16_t hdrTrigger = (int16_t)(m.header.w - 28);   // 200 at 240x320
    const int16_t hdrWindow  = (int16_t)(m.header.w - 18);   // 210
    if (tftInstance->textWidth(currentViewerTitle, m.fontBody) > hdrTrigger) {
        tftInstance->setTextDatum(ML_DATUM);
        tftInstance->drawString(marqueeSlice(tftInstance, currentViewerTitle, titleScrollPos,
                                             hdrWindow, m.fontBody),
                                (int16_t)(m.header.x + 4), m.headerTextY, m.fontBody);
    } else {
        tftInstance->drawString(currentViewerTitle, m.header.cx(), m.headerTextY, m.fontBody);
    }

    tftInstance->fillRect(m.list.x, m.list.y, m.list.w, m.list.h, TFT_BLACK);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(TL_DATUM);

    int yPos = m.list.y;
    int currentLine = 0;
    const int visibleLines = (m.list.h > 0) ? (m.list.h / 16) : 1; // 14 lines at 240x320

    String content = currentViewerContent;
    while(content.length() > 0) {
        int splitIdx = 22; // Max chars per line
        if(content.length() <= 22) splitIdx = content.length();
        else {
            int spaceIdx = content.lastIndexOf(' ', 22);
            if(spaceIdx > 0) splitIdx = spaceIdx;
        }

        int nlIdx = content.indexOf('\n');
        if(nlIdx >= 0 && nlIdx < splitIdx) {
            splitIdx = nlIdx;
        }

        if (currentLine >= viewerScrollOffset && currentLine < viewerScrollOffset + visibleLines) {
            tftInstance->drawString(content.substring(0, splitIdx), m.list.x + 2, yPos, m.fontBody);
            yPos += 16;
        }

        content = content.substring(splitIdx);
        if(content.startsWith("\n") || content.startsWith(" ")) {
            content = content.substring(1);
        }
        currentLine++;
    }

    // Up/Down Indicators
    if (viewerScrollOffset > 0) {
        tftInstance->fillTriangle(m.list.right() - 10, m.list.y + 5, m.list.right(), m.list.y + 15, m.list.right() - 20, m.list.y + 15, TFT_WHITE);
    }
    if (currentLine > viewerScrollOffset + visibleLines) {
        tftInstance->fillTriangle(m.list.right() - 10, m.list.bottom() - 10, m.list.right() - 20, m.list.bottom() - 20, m.list.right(), m.list.bottom() - 20, TFT_WHITE);
    }

    tftInstance->fillRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_WHITE);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    // BACK / UP / DN at the centre of each third (40 / 120 / 200 at 240x320) — see viewerZoneFromX.
    tftInstance->drawString("BACK", thirdCenterX(m, 0), m.footerTextY, m.fontBody);
    tftInstance->drawString("UP",   thirdCenterX(m, 1), m.footerTextY, m.fontBody);
    tftInstance->drawString("DN",   thirdCenterX(m, 2), m.footerTextY, m.fontBody);
}

void HelpCenterUI::drawDialog() {
    const UiMetrics& m = M();
    tftInstance->fillScreen(TFT_BLACK);
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_RED);
    tftInstance->setTextColor(TFT_RED, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Error", m.header.cx(), m.headerTextY, m.fontBody);

    const UiRect ok = m.dialogButton((int16_t)(m.dialogButtonRowY - 10), 30, 0, 1, 70);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->drawString(dialogMessage, m.centerX, m.centerY - 20, m.fontBody);

    tftInstance->drawRoundRect(ok.x, ok.y, ok.w, ok.h, 5, TFT_WHITE);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->drawString("OK", ok.cx(), ok.cy(), m.fontBody);
}

// --- Loaders ---
void HelpCenterUI::loadOfflineCategories() {
    listCount = offCatCount;
    for(int i=0; i<listCount; i++) listItems[i] = offCats[i];
    selectedIndex = 0;
    scrollOffset = 0;
    uiState = 1;
    draw();
}

void HelpCenterUI::loadOfflineTopics(int catIdx) {
    if (catIdx == 0) { listCount = 3; for(int i=0; i<3; i++) listItems[i] = offTopics0[i]; }
    else if (catIdx == 1) { listCount = 2; for(int i=0; i<2; i++) listItems[i] = offTopics1[i]; }
    else if (catIdx == 2) { listCount = 1; for(int i=0; i<1; i++) listItems[i] = offTopics2[i]; }
    else if (catIdx == 3) { listCount = 1; for(int i=0; i<1; i++) listItems[i] = offTopics3[i]; }
    selectedIndex = 0;
    scrollOffset = 0;
    uiState = 2;
    draw();
}

void HelpCenterUI::loadOfflineContent(int catIdx, int topicIdx) {
    if (catIdx == 0) { currentViewerTitle = offTopics0[topicIdx]; currentViewerContent = offContent0[topicIdx]; }
    else if (catIdx == 1) { currentViewerTitle = offTopics1[topicIdx]; currentViewerContent = offContent1[topicIdx]; }
    else if (catIdx == 2) { currentViewerTitle = offTopics2[topicIdx]; currentViewerContent = offContent2[topicIdx]; }
    else if (catIdx == 3) { currentViewerTitle = offTopics3[topicIdx]; currentViewerContent = offContent3[topicIdx]; }
    viewerScrollOffset = 0;
    titleScrollPos = 0;
    lastTitleScrollTime = millis();
    uiState = 5;
    draw();
}

bool HelpCenterUI::downloadFile(const String& url, const String& destPath, const String& loadingMsg) {
    if (WiFi.status() != WL_CONNECTED) {
        dialogMessage = "Please turn on WiFi first!";
        return false;
    }

    const UiMetrics& m = M();
    HTTPClient http;
    http.begin(url);

    tftInstance->fillScreen(TFT_BLACK);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString(loadingMsg, m.centerX, m.centerY - 20, m.fontBody);
    const UiRect bar = { (int16_t)(m.centerX - (m.w - 60) / 2), m.centerY, (int16_t)(m.w - 60), 20 };
    tftInstance->drawRect(bar.x, bar.y, bar.w, bar.h, TFT_WHITE);
    
    int httpCode = http.GET();
    if (httpCode > 0 && httpCode == HTTP_CODE_OK) {
        int totalLen = http.getSize();
        int downloaded = 0;
        
        // Ensure parent directory exists before writing
        int lastSlash = destPath.lastIndexOf('/');
        if (lastSlash > 0) {
            String parentDir = destPath.substring(0, lastSlash);
            if (!LittleFS.exists(parentDir.c_str())) {
                LittleFS.mkdir(parentDir.c_str());
            }
        }
        
        File file = LittleFS.open(destPath, FILE_WRITE);
        if (!file) {
            dialogMessage = "FS Write: " + destPath;
            http.end();
            return false;
        }
        
        WiFiClient *stream = http.getStreamPtr();
        uint8_t buff[512] = { 0 };
        while ((http.connected() || stream->available() > 0) && (totalLen == -1 || downloaded < totalLen)) {
            size_t size = stream->available();
            if (size) {
                int toRead = size > sizeof(buff) ? sizeof(buff) : size;
                int readLen = stream->readBytes(buff, toRead);
                if (readLen > 0) {
                    file.write(buff, readLen);
                    downloaded += readLen;
                    
                    if (totalLen > 0) {
                        int progressWidth = map(downloaded, 0, totalLen, 0, bar.w - 4);
                        tftInstance->fillRect(bar.x + 2, bar.y + 2, progressWidth, bar.h - 4, TFT_GREEN);
                    }
                }
            } else {
                delay(1);
            }
        }
        file.close();
        http.end();
        return true;
    }
    dialogMessage = "Error HTTP " + String(httpCode);
    http.end();
    return false;
}

bool HelpCenterUI::fetchOnlineCategories() {
    String tmpPath = "/tmp_download/h_idx.json";
    if (!FileSystem::exists("/local/tmp_download/")) FileSystem::mkdir("/local/tmp_download/");
    
    if (!downloadFile("https://raw.githubusercontent.com/Haris16-code/KryonOS/refs/heads/main/help/index.json", tmpPath, "Fetching Index...")) {
        return false;
    }
    
    File file = LittleFS.open(tmpPath, "r");
    if (!file) { dialogMessage = "Failed to open index"; return false; }
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    LittleFS.remove(tmpPath);
    
    if (err && err != DeserializationError::IncompleteInput) { dialogMessage = "Parse error!"; return false; }
    
    listCount = 0;
    JsonArray cats = doc["categories"];
    for (JsonObject cat : cats) {
        if (listCount >= 25) break;
        listItems[listCount] = cat["name"].as<String>();
        listUrls[listCount] = cat["url"].as<String>();
        listCount++;
    }
    
    selectedIndex = 0;
    scrollOffset = 0;
    titleScrollPos = 0;
    listScrollPos = 0;
    lastTitleScrollTime = millis();
    lastListScrollTime = millis();
    uiState = 3;
    draw();
    return true;
}

bool HelpCenterUI::fetchOnlineTopics(const String& url) {
    String tmpPath = "/tmp_download/h_cat.json";
    if (!downloadFile(url, tmpPath, "Loading Topics...")) return false;
    
    File file = LittleFS.open(tmpPath, "r");
    if (!file) { dialogMessage = "Failed to open cat"; return false; }
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    LittleFS.remove(tmpPath);
    
    if (err && err != DeserializationError::IncompleteInput) { dialogMessage = "Parse error!"; return false; }
    
    listCount = 0;
    JsonArray arts = doc["articles"];
    for (JsonObject art : arts) {
        if (listCount >= 25) break;
        listItems[listCount] = art["title"].as<String>();
        listUrls[listCount] = art["url"].as<String>();
        listCount++;
    }
    
    selectedIndex = 0;
    scrollOffset = 0;
    titleScrollPos = 0;
    listScrollPos = 0;
    lastTitleScrollTime = millis();
    lastListScrollTime = millis();
    uiState = 4;
    draw();
    return true;
}

bool HelpCenterUI::fetchOnlineContent(const String& url, const String& title) {
    String tmpPath = "/tmp_download/h_art.json";
    if (!downloadFile(url, tmpPath, "Loading Article...")) return false;
    
    File file = LittleFS.open(tmpPath, "r");
    if (!file) { dialogMessage = "Failed to open art"; return false; }
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, file);
    file.close();
    LittleFS.remove(tmpPath);
    
    if (err && err != DeserializationError::IncompleteInput) { dialogMessage = "Parse error!"; return false; }
    
    currentViewerTitle = title;
    currentViewerContent = doc["content"] | "No content found.";
    viewerScrollOffset = 0;
    titleScrollPos = 0;
    lastTitleScrollTime = millis();
    uiState = 5;
    draw();
    return true;
}

void HelpCenterUI::handleTouch(uint16_t x, uint16_t y) {
    extern int currentState;
    const UiMetrics& m = M();

    if (uiState == 0) { // Main Menu
        const UiRect offlineBtn = { (int16_t)(m.list.x + 10), (int16_t)(m.list.y + 35), (int16_t)(m.list.w - 20), 40 };
        const UiRect onlineBtn  = { (int16_t)(m.list.x + 10), (int16_t)(m.list.y + 95), (int16_t)(m.list.w - 20), 40 };
        if (offlineBtn.contains((int16_t)x, (int16_t)y)) {
            selectedIndex = 0; draw();
            loadOfflineCategories();
        } else if (onlineBtn.contains((int16_t)x, (int16_t)y)) {
            selectedIndex = 1; draw();
            if(!fetchOnlineCategories()) {
                uiState = 6; draw();
            }
        } else if (m.inFooter((int16_t)y)) {
            currentState = 0; // Launcher
        }
    }
    else if (uiState == 1 || uiState == 2 || uiState == 3 || uiState == 4) { // Lists
        const int rowIndex = m.listRowFromY((int16_t)y);
        if (rowIndex >= 0) {
            int clickedAbs = scrollOffset + rowIndex;
            if (clickedAbs < listCount) {
                if (selectedIndex != clickedAbs) {
                    selectedIndex = clickedAbs;
                    listScrollPos = 0;
                    lastListScrollTime = millis();
                    draw(); // Highlight
                }

                if (uiState == 1) { // Off Cats -> Off Topics
                    selectedCategoryIndex = selectedIndex;
                    loadOfflineTopics(selectedCategoryIndex);
                } else if (uiState == 2) { // Off Topics -> Viewer
                    loadOfflineContent(selectedCategoryIndex, selectedIndex);
                } else if (uiState == 3) { // On Cats -> On Topics
                    currentCategoryName = listItems[selectedIndex];
                    if(!fetchOnlineTopics(listUrls[selectedIndex])) { uiState = 6; draw(); }
                } else if (uiState == 4) { // On Topics -> Viewer
                    if(!fetchOnlineContent(listUrls[selectedIndex], listItems[selectedIndex])) { uiState = 6; draw(); }
                }
            }
        }
        else if (m.inFooter((int16_t)y)) {
            // Same zones the labels are drawn in (footerSlotCenterX), so the two cannot drift.
            switch (m.footerSlotFromX((int16_t)x)) {
            case UI_SLOT_BACK:
                if (uiState == 1 || uiState == 3) { uiState = 0; selectedIndex = 0; draw(); }
                else if (uiState == 2) { loadOfflineCategories(); }
                else if (uiState == 4) { if(!fetchOnlineCategories()) { uiState = 6; draw(); } }
                break;
            case UI_SLOT_UP:
                if (selectedIndex > 0) {
                    selectedIndex--;
                    if (selectedIndex < scrollOffset) scrollOffset = selectedIndex;
                    listScrollPos = 0;
                    lastListScrollTime = millis();
                    draw();
                }
                break;
            case UI_SLOT_SEL:
                if (uiState == 1) { selectedCategoryIndex = selectedIndex; loadOfflineTopics(selectedCategoryIndex); }
                else if (uiState == 2) { loadOfflineContent(selectedCategoryIndex, selectedIndex); }
                else if (uiState == 3) { currentCategoryName = listItems[selectedIndex]; if(!fetchOnlineTopics(listUrls[selectedIndex])) { uiState = 6; draw(); } }
                else if (uiState == 4) { if(!fetchOnlineContent(listUrls[selectedIndex], listItems[selectedIndex])) { uiState = 6; draw(); } }
                break;
            default: // UI_SLOT_DN
                if (selectedIndex < listCount - 1) {
                    selectedIndex++;
                    if (selectedIndex >= scrollOffset + m.itemsPerPage) scrollOffset = selectedIndex - (m.itemsPerPage - 1);
                    listScrollPos = 0;
                    lastListScrollTime = millis();
                    draw();
                }
                break;
            }
        }
    }
    else if (uiState == 5) { // Viewer
        // The list area is split into thirds: the top third scrolls back, the bottom third forward,
        // the middle stays inert so a tap does not scroll by accident. The historical bands were
        // absolute (y<100 / 180<y<270), which on a short panel covered the whole list — the lower
        // zone fell off the screen entirely and text could only ever scroll one way.
        const int16_t third = (int16_t)(m.list.h / 3);
        if (m.inFooter((int16_t)y)) {
            // The footer is below the list, so it has to be tested first: its y would otherwise
            // satisfy "past the last third" and be swallowed by the scroll-down zone.
            switch (viewerZoneFromX(m, (int16_t)x)) {
            case 1: // UP
                if (viewerScrollOffset > 0) { viewerScrollOffset--; draw(); }
                break;
            case 2: // DN
                viewerScrollOffset++; draw();
                break;
            default: // BACK
                if (listUrls[0].length() > 0) { uiState = 4; draw(); }
                else { loadOfflineTopics(selectedCategoryIndex); }
                break;
            }
        } else if (y < m.list.y + third) { // Scroll Up
            if (viewerScrollOffset > 0) { viewerScrollOffset--; draw(); }
        } else if (y >= m.list.y + 2 * third) { // Scroll Down
            viewerScrollOffset++; draw();
        }
    }
    else if (uiState == 6) { // Dialog
        const UiRect ok = m.dialogButton((int16_t)(m.dialogButtonRowY - 10), 30, 0, 1, 70);
        if (ok.contains((int16_t)x, (int16_t)y)) {
            uiState = 0;
            draw();
        }
    }
}

void HelpCenterUI::update() {
    const UiMetrics& m = M();
    if (uiState == 5 && tftInstance) {
        if (tftInstance->textWidth(currentViewerTitle, m.fontBody) > (int16_t)(m.header.w - 28)) {
            unsigned long waitTime = (titleScrollPos == 0) ? 1500 : 350;
            if (millis() - lastTitleScrollTime > waitTime) {
                lastTitleScrollTime = millis();
                int scrollTextLen = currentViewerTitle.length() + MARQUEE_GAP;
                titleScrollPos = (titleScrollPos + 1) % scrollTextLen;

                tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
                tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_GREEN);
                tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
                tftInstance->setTextDatum(ML_DATUM);

                tftInstance->drawString(
                    marqueeSlice(tftInstance, currentViewerTitle, titleScrollPos,
                                 (int16_t)(m.header.w - 18), m.fontBody),
                    (int16_t)(m.header.x + 4), m.headerTextY, m.fontBody);
            }
        }
    } else if (uiState >= 1 && uiState <= 4 && tftInstance) {
        String headerTitle = "";
        if (uiState == 1) headerTitle = "Offline Categories";
        else if (uiState == 2) headerTitle = offCats[selectedCategoryIndex];
        else if (uiState == 3) headerTitle = "Online Categories";
        else if (uiState == 4) headerTitle = currentCategoryName;

        if (tftInstance->textWidth(headerTitle, m.fontBody) > (int16_t)(m.header.w - 28)) {
            unsigned long waitTime = (titleScrollPos == 0) ? 1500 : 350;
            if (millis() - lastTitleScrollTime > waitTime) {
                lastTitleScrollTime = millis();
                int scrollTextLen = headerTitle.length() + MARQUEE_GAP;
                titleScrollPos = (titleScrollPos + 1) % scrollTextLen;

                tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
                tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_GREEN);
                tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
                tftInstance->setTextDatum(ML_DATUM);

                tftInstance->drawString(
                    marqueeSlice(tftInstance, headerTitle, titleScrollPos,
                                 (int16_t)(m.header.w - 18), m.fontBody),
                    (int16_t)(m.header.x + 4), m.headerTextY, m.fontBody);
            }
        }

        if (listCount > 0 && selectedIndex >= 0 && selectedIndex < listCount) {
            const int16_t rowWindow = (int16_t)(m.list.w - 30);
            if (tftInstance->textWidth(listItems[selectedIndex], m.fontBody) > rowWindow) {
                unsigned long waitTime = (listScrollPos == 0) ? 1500 : 300;
                if (millis() - lastListScrollTime > waitTime) {
                    lastListScrollTime = millis();
                    int scrollTextLen = listItems[selectedIndex].length() + MARQUEE_GAP;
                    listScrollPos = (listScrollPos + 1) % scrollTextLen;

                    const int vis = selectedIndex - scrollOffset;
                    const int yPos = m.list.y + (vis * m.rowH);
                    if (yPos >= m.list.y && yPos < m.list.y + m.itemsPerPage * m.rowH) {
                        const UiRect fill = m.listRowFillRect(vis);
                        tftInstance->fillRect(fill.x, fill.y, fill.w, fill.h, TFT_WHITE);
                        tftInstance->setTextColor(TFT_BLACK, TFT_WHITE);
                        tftInstance->setTextDatum(ML_DATUM);

                        tftInstance->drawString(
                            "> " + marqueeSlice(tftInstance, listItems[selectedIndex], listScrollPos,
                                                rowWindow, m.fontBody),
                            (int16_t)(fill.x + m.rowTextPadX), m.listRowTextY(vis), m.fontBody);
                    }
                }
            }
        }
    }
}
