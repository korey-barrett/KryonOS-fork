#include "LauncherUI.h"
#include "../Kernel/Core/HarixKernel.h"
#include "../FileSystem/FileSystem.h"
#include "../Kernel/Services/IPCManager.h"
#include "../UI/UiLayout.h"
#include <ArduinoJson.h>

extern int currentState;

// Current screen metrics. See Documentation/Display_Touch_Architecture.md.
static inline const UiMetrics& M() { return UiLayout::current(); }

// The home list is a fixed SYSTEM section (a header, five built-in entries, then an APPS header)
// followed by the scanned user apps. Every offset that used to be a bare `7` or `6` is derived from
// these, so adding or removing a built-in entry cannot silently desynchronise the app index.
static const int ITEM_SYSTEM_HEADER = 0;
static const int ITEM_APPS_HEADER   = 6;
static const int ITEM_FIRST_APP     = ITEM_APPS_HEADER + 1;  // 7
static const int FIXED_ITEM_COUNT   = ITEM_FIRST_APP;        // 7 rows before the first user app

// Header rows are section labels: they are drawn, but never selected, so navigation steps over them.
static inline bool isHeaderItem(int item) {
    return item == ITEM_SYSTEM_HEADER || item == ITEM_APPS_HEADER;
}

TFT_eSPI *LauncherUI::tftInstance = nullptr;
String LauncherUI::appPaths[50];
String LauncherUI::appNames[50];
bool   LauncherUI::appIsFolder[50];
int LauncherUI::appCount = 0;
int LauncherUI::selectedIndex = 1;
int LauncherUI::scrollOffset = 0;
bool LauncherUI::needsRescan = true;

void LauncherUI::requestRescan() {
    needsRescan = true;
}

void LauncherUI::init(TFT_eSPI *tft) {
    tftInstance = tft;
}

void LauncherUI::scanLocalApps() {
    appCount = 0;
    IPCManager::clearRegistry();
    Serial.println("=== scanLocalApps START ===");
    
    // Scan both /local/apps/ and /sd/apps/ for folder-based apps and legacy .js files
    const char* appDirs[] = { "/local/apps/", "/sd/apps/" };
    
    for (int d = 0; d < 2; d++) {
        Serial.printf("Checking dir: %s\n", appDirs[d]);
        if (!FileSystem::exists(appDirs[d])) {
            Serial.printf("  -> does NOT exist, skipping\n");
            continue;
        }
        Serial.printf("  -> exists!\n");
        
        FileEntry entries[50];
        int count = FileSystem::listDirectory(appDirs[d], entries, 50);
        Serial.printf("  -> listDirectory returned %d entries\n", count);
        
        for (int i = 0; i < count && appCount < 50; i++) {
            Serial.printf("  Entry[%d]: name='%s' path='%s' isDir=%d\n", 
                          i, entries[i].name.c_str(), entries[i].path.c_str(), entries[i].isDir);
            
            // Draw loading bar (inside the outline main.cpp draws: 20px gutters, 40px below centre)
            if (tftInstance) {
                const UiMetrics& m = M();
                const int16_t barW = (int16_t)(m.w - 40);
                tftInstance->fillRect(20, m.centerY + 40, (i * barW) / count, 10, TFT_GREEN);
            }
            
            // 1. Check if it's an app package (has app.json)
            String appJsonPath = entries[i].path;
            if (!appJsonPath.endsWith("/")) appJsonPath += "/";
            appJsonPath += "app.json";
            
            Serial.printf("  Checking app.json at: '%s'\n", appJsonPath.c_str());
            
            if (FileSystem::exists(appJsonPath.c_str())) {
                Serial.printf("  -> app.json EXISTS\n");
                String jsonContent = FileSystem::readTextFile(appJsonPath.c_str());
                Serial.printf("  -> json content length: %d\n", jsonContent.length());
                
                JsonDocument doc;
                DeserializationError jsonErr = deserializeJson(doc, jsonContent);
                String name = "";
                String pkgName = "";
                String category = "Utility";
                std::vector<String> fileAssocs;
                bool allowCompanion = true;

                if (!jsonErr) {
                    name = doc["name"] | "";
                    pkgName = doc["packageName"] | "";
                    category = doc["category"] | "Utility";
                    
                    if (doc["fileAssociations"].is<JsonArray>()) {
                        for (JsonVariant v : doc["fileAssociations"].as<JsonArray>()) {
                            fileAssocs.push_back(v.as<String>());
                        }
                    }
                    if (doc.containsKey("allowCompanionLaunch")) {
                        allowCompanion = doc["allowCompanionLaunch"].as<bool>();
                    }
                } else {
                    name = FileSystem::parseJsonValue(jsonContent, "name");
                    pkgName = FileSystem::parseJsonValue(jsonContent, "packageName");
                    category = FileSystem::parseJsonValue(jsonContent, "category");
                    if (category.length() == 0) category = "Utility";
                }

                Serial.printf("  -> parsed name: '%s', pkg: '%s', cat: '%s'\n", name.c_str(), pkgName.c_str(), category.c_str());
                
                if (name.length() > 0) {
                    // Check if this app is already in our list (avoid duplicates from SD + local)
                    bool duplicate = false;
                    for (int j = 0; j < appCount; j++) {
                        if (appNames[j] == name) { duplicate = true; break; }
                    }
                    if (!duplicate) {
                        appPaths[appCount] = entries[i].path;
                        appNames[appCount] = name;
                        appIsFolder[appCount] = true;
                        appCount++;
                        Serial.printf("  -> ADDED app #%d: '%s'\n", appCount, name.c_str());
                    }

                    // Register in IPCManager
                    String targetId = (pkgName.length() > 0) ? pkgName : name;
                    IPCManager::registerApp(targetId, entries[i].path, name, category, fileAssocs, allowCompanion);
                }
            } else {
                Serial.printf("  -> app.json does NOT exist\n");
                // 2. Legacy .js file support
                String fname = entries[i].name;
                if (fname.endsWith(".js")) {
                    bool duplicate = false;
                    for (int j = 0; j < appCount; j++) {
                        if (appNames[j] == fname) { duplicate = true; break; }
                    }
                    if (!duplicate) {
                        appPaths[appCount] = entries[i].path;
                        appNames[appCount] = fname;
                        appIsFolder[appCount] = false;
                        appCount++;
                        Serial.printf("  -> ADDED legacy app #%d: '%s'\n", appCount, fname.c_str());
                    }
                }
            }
        }
    }
    Serial.printf("=== scanLocalApps END: appCount=%d ===\n", appCount);
}

void LauncherUI::draw() {
    if (!tftInstance) return;
    
    const UiMetrics& m = M();

    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK); // Header bg
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_GREEN);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("KryonOS Home", m.header.cx(), m.headerTextY, m.fontBody);
    
    // Clear only the list area to prevent full screen flicker
    tftInstance->fillRect(m.list.x, m.list.y, m.list.w, m.list.h, TFT_BLACK);

    if (needsRescan) {
        scanLocalApps();
        needsRescan = false;
        // Re-clear after scanning as the loading bar might have been drawn
        tftInstance->fillRect(m.list.x, m.list.y, m.list.w, m.list.h, TFT_BLACK);
    }

    const int totalItems = appCount + FIXED_ITEM_COUNT;
    const int itemsPerPage = m.itemsPerPage;

    for (int i = 0; i < itemsPerPage; i++) {
        const int listIndex = scrollOffset + i;
        if (listIndex >= totalItems) break;

        const UiRect row  = m.listRowRect(i);
        const UiRect fill = m.listRowFillRect(i);
        const int16_t textX = (int16_t)(row.x + m.rowTextPadX);
        const int16_t textY = m.listRowTextY(i);

        String itemName = "";
        bool isHeader = false;

        if (listIndex == ITEM_SYSTEM_HEADER) { itemName = "[ SYSTEM ]"; isHeader = true; }
        else if (listIndex == 1) itemName = "App Store";
        else if (listIndex == 2) itemName = "App Installer";
        else if (listIndex == 3) itemName = "Kryon Cloud";
        else if (listIndex == 4) itemName = "Settings";
        else if (listIndex == 5) itemName = "Help Center";
        else if (listIndex == ITEM_APPS_HEADER) { itemName = "[ APPS ]"; isHeader = true; }
        else {
            int appIdx = listIndex - ITEM_FIRST_APP;
            itemName = appNames[appIdx];
        }

        if (isHeader) {
            tftInstance->fillRect(fill.x, fill.y, fill.w, fill.h, TFT_BLACK);
            tftInstance->setTextColor(TFT_DARKGREY, TFT_BLACK);
            tftInstance->setTextDatum(ML_DATUM);
            tftInstance->drawString(itemName.c_str(), textX, textY, m.fontBody);
        } else if (listIndex == selectedIndex) {
            // Highlighted Item
            tftInstance->fillRect(fill.x, fill.y, fill.w, fill.h, TFT_WHITE);
            tftInstance->setTextColor(TFT_BLACK, TFT_WHITE);
            tftInstance->setTextDatum(ML_DATUM);
            tftInstance->drawString(("> " + itemName).c_str(), textX, textY, m.fontBody);
        } else {
            // Normal Item
            tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
            tftInstance->setTextDatum(ML_DATUM);
            tftInstance->drawString(("  " + itemName).c_str(), textX, textY, m.fontBody);
        }

    }

    // Draw Scrollbar
    if (totalItems > itemsPerPage) {
        const int sbX = m.scrollX;
        const int sbY = m.list.y;
        const int sbHeight = m.list.h;
        int thumbHeight = (sbHeight * itemsPerPage) / totalItems;
        if (thumbHeight < m.scrollThumbMin) thumbHeight = m.scrollThumbMin;
        const int maxThumbY = sbHeight - thumbHeight;
        const int thumbY = sbY + (scrollOffset * maxThumbY) / (totalItems - itemsPerPage);

        tftInstance->fillRect(sbX, sbY, m.scrollW, sbHeight, TFT_DARKGREY);
        tftInstance->fillRect(sbX, thumbY, m.scrollW, thumbHeight, TFT_WHITE);
    }

    // Touch Footer
    tftInstance->drawRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_WHITE);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    const int16_t q = m.w / 4;
    tftInstance->drawString("UP",  m.footerButtonCenterX(UI_FOOTER_UP),  m.footerTextY, m.fontBody);
    tftInstance->drawString("|",   q,                                   m.footerTextY, m.fontBody);
    tftInstance->drawString("SEL", m.footerButtonCenterX(UI_FOOTER_SEL), m.footerTextY, m.fontBody);
    tftInstance->drawString("|",   (int16_t)(3 * q),                    m.footerTextY, m.fontBody);
    tftInstance->drawString("DN",  m.footerButtonCenterX(UI_FOOTER_DN),  m.footerTextY, m.fontBody);
}

static void runApp(TFT_eSPI* tft, const String& path, bool isFolder) {
    extern int currentState;
    currentState = 2; // STATE_RUN_APP
    
    String currentPath = path;
    bool currentIsFolder = isFolder;

    while (true) {
        tft->fillScreen(TFT_BLACK);
        tft->setTextDatum(TL_DATUM);
        
        String filePath;
        if (currentIsFolder) {
            filePath = currentPath;
            if (!filePath.endsWith("/")) filePath += "/";
            filePath += "main.js";
        } else {
            filePath = currentPath;
        }
        
        // Find appId for this path
        String appId = "";
        for (const auto& app : IPCManager::getRegisteredApps()) {
            if (app.appPath == currentPath || currentPath.startsWith(app.appPath)) {
                appId = app.appId;
                break;
            }
        }
        IPCManager::setCurrentAppId(appId);
        
        HarixKernel::runFile(filePath.c_str());
        
        if (IPCManager::hasPendingLaunch()) {
            currentPath = IPCManager::getPendingAppPath();
            currentIsFolder = true; // Target packages are app directories
            IPCManager::clearPendingLaunch();
            continue;
        }
        break;
    }
    
    // Draw exit button
    const UiRect& ex = M().appExitButton;
    tft->fillRoundRect(ex.x, ex.y, ex.w, ex.h, 5, TFT_RED);
    tft->setTextColor(TFT_WHITE, TFT_RED);
    tft->setTextDatum(MC_DATUM);
    tft->drawString("X", ex.cx(), ex.cy(), M().fontBody);
}

void LauncherUI::handleTouch(uint16_t x, uint16_t y) {
    extern int currentState;
    const UiMetrics& m = M();

    const int totalItems = appCount + FIXED_ITEM_COUNT;
    const int itemsPerPage = m.itemsPerPage;

    // Check list item touch first
    const int clickedRelativeIndex = m.listRowFromY((int16_t)y);
    if (clickedRelativeIndex >= 0) {
        const int clickedAbsoluteIndex = scrollOffset + clickedRelativeIndex;


        if (clickedAbsoluteIndex < totalItems && !isHeaderItem(clickedAbsoluteIndex)) {
            selectedIndex = clickedAbsoluteIndex;
            draw(); // Highlight the item
            
            // Execute it
            if (selectedIndex == 1) {
                currentState = 13; // STATE_APP_STORE
            } else if (selectedIndex == 2) {
                currentState = 3; // STATE_INSTALLER
            } else if (selectedIndex == 3) {
                currentState = 16; // STATE_KRYON_CLOUD (3rd item)
            } else if (selectedIndex == 4) {
                currentState = 1; // STATE_SETTINGS
            } else if (selectedIndex == 5) {
                currentState = 14; // STATE_HELP_CENTER
            } else if (selectedIndex >= ITEM_FIRST_APP) {
                int appIndex = selectedIndex - ITEM_FIRST_APP;
                runApp(tftInstance, appPaths[appIndex], appIsFolder[appIndex]);
            }
        }
        return;
    }

    if (m.inFooter((int16_t)y)) {
        // Footer Buttons
        const int btn = m.footerButtonFromX((int16_t)x);
        if (btn == UI_FOOTER_UP) { // UP
            if (selectedIndex > 1) {
                selectedIndex--;
                if (isHeaderItem(selectedIndex)) selectedIndex--;

                if (selectedIndex < scrollOffset) {
                    scrollOffset = selectedIndex;
                }

                if (selectedIndex == 1) scrollOffset = 0;
                // Landing on the first app keeps its [ APPS ] header on screen.
                if (selectedIndex == ITEM_FIRST_APP && scrollOffset > ITEM_APPS_HEADER) {
                    scrollOffset = ITEM_APPS_HEADER;
                }

                draw();
            } else if (scrollOffset > 0) {
                scrollOffset = 0;
                draw();
            }
        } else if (btn == UI_FOOTER_SEL) { // SEL
            if (selectedIndex == 1) {
                currentState = 13; // STATE_APP_STORE
            } else if (selectedIndex == 2) {
                currentState = 3; // STATE_INSTALLER
            } else if (selectedIndex == 3) {
                currentState = 16; // STATE_KRYON_CLOUD (3rd item)
            } else if (selectedIndex == 4) {
                currentState = 1; // STATE_SETTINGS
            } else if (selectedIndex == 5) {
                currentState = 14; // STATE_HELP_CENTER
            } else if (selectedIndex >= ITEM_FIRST_APP) {
                int appIndex = selectedIndex - ITEM_FIRST_APP;
                runApp(tftInstance, appPaths[appIndex], appIsFolder[appIndex]);
            }
        } else if (btn == UI_FOOTER_DN) { // DN
            if (selectedIndex < totalItems - 1) {
                selectedIndex++;
                if (isHeaderItem(selectedIndex)) selectedIndex++;
                if (selectedIndex >= scrollOffset + itemsPerPage) {
                    scrollOffset = selectedIndex - (itemsPerPage - 1);
                }
                draw();
            }
        }
        return;
    }
    
    // Quick jump to installer if pressing header
    if (y < m.header.bottom() + 4) {
        currentState = 3;
        return;
    }
}
