#include "AppStoreUI.h"
#include "../FileSystem/FileSystem.h"
#include "../Hal/Crypto/CryptoEngine.h"
#include "../Kernel/Services/Network/TLSHelper.h"
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <LittleFS.h>
#include <SD.h>
#include "InstallerUI.h"
#include "../UI/UiLayout.h"

// Current screen metrics. See Documentation/Display_Touch_Architecture.md.
static inline const UiMetrics& M() { return UiLayout::current(); }

// The App Store's four-zone footer (BACK / UP / SEL / DN). Labels are drawn on the centre of the
// very zone that handleTouch() tests, so the two can never drift apart. At 240x320 this lands on
// the historical x positions 35 / 100 / 165 / 220 with dividers at 70 / 130 / 200.
static void drawStoreFooter(KryonDisplay* tft, const UiMetrics& m) {
    tft->drawRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_WHITE);
    tft->setTextColor(TFT_WHITE, TFT_BLACK);
    tft->setTextDatum(MC_DATUM);
    tft->drawString("BACK", m.footerSlotCenterX(UI_SLOT_BACK), m.footerTextY, m.fontBody);
    tft->drawString("|",    m.footerSlot(UI_SLOT_UP).x,          m.footerTextY, m.fontBody);
    tft->drawString("UP",   m.footerSlotCenterX(UI_SLOT_UP),     m.footerTextY, m.fontBody);
    tft->drawString("|",    m.footerSlot(UI_SLOT_SEL).x,         m.footerTextY, m.fontBody);
    tft->drawString("SEL",  m.footerSlotCenterX(UI_SLOT_SEL),    m.footerTextY, m.fontBody);
    tft->drawString("|",    m.footerSlot(UI_SLOT_DN).x,          m.footerTextY, m.fontBody);
    tft->drawString("DN",   m.footerSlotCenterX(UI_SLOT_DN),     m.footerTextY, m.fontBody);
}

// One list row — "> name" when selected, "  name" otherwise. The fill rect, the text inset and the
// baseline all come from UiLayout, the same values handleTouch() hit-tests against.
static void drawAppStoreRow(KryonDisplay* tft, const UiMetrics& m, int visibleIndex,
                            const String& name, bool selected) {
    const UiRect   row   = m.listRowRect(visibleIndex);
    const int16_t  textX = (int16_t)(row.x + m.rowTextPadX);
    const int16_t  textY = m.listRowTextY(visibleIndex);

    if (selected) {
        const UiRect fill = m.listRowFillRect(visibleIndex);
        tft->fillRect(fill.x, fill.y, fill.w, fill.h, TFT_WHITE);
        tft->setTextColor(TFT_BLACK, TFT_WHITE);
        tft->setTextDatum(ML_DATUM);
        tft->drawString(("> " + name).c_str(), textX, textY, m.fontBody);
    } else {
        tft->setTextColor(TFT_WHITE, TFT_BLACK);
        tft->setTextDatum(ML_DATUM);
        tft->drawString(("  " + name).c_str(), textX, textY, m.fontBody);
    }
}

extern int currentState;

KryonDisplay *AppStoreUI::tftInstance = nullptr;

int AppStoreUI::storeState = 0;
bool AppStoreUI::isUpdateMode = false;
int AppStoreUI::selectedIndex = 0;
int AppStoreUI::scrollOffset = 0;

String AppStoreUI::categoryNames[20];
String AppStoreUI::categoryUrls[20];
int AppStoreUI::categoryCount = 0;

AppStoreItem AppStoreUI::currentApps[50];
int AppStoreUI::currentAppCount = 0;
String AppStoreUI::currentCategoryName = "";
int AppStoreUI::selectedAppIndex = -1;

AppStoreItem AppStoreUI::updateApps[50];
int AppStoreUI::updateAppCount = 0;

String AppStoreUI::dialogMessage = "";
bool AppStoreUI::downloadInProgress = false;

const char* INDEX_URL = "https://raw.githubusercontent.com/Haris16-code/KryonOS-AppStore/refs/heads/main/index.json";

void AppStoreUI::init(KryonDisplay *tft) {
    tftInstance = tft;
}

// ============================================================
// Core Draw Router
// ============================================================
void AppStoreUI::draw() {
    if (!tftInstance) return;
    
    if (storeState == 0) {
        if (categoryCount == 0) {
            bool success = fetchCategories();
            if (!success) {
                storeState = 4;
                drawDialog();
                return;
            }
        }
        drawCategories();
    } else if (storeState == 1) {
        drawAppList();
    } else if (storeState == 2) {
        drawAppInfo();
    } else if (storeState == 3 || storeState == 4) {
        drawDialog();
    }
}

// ============================================================
// Network Fetching
// ============================================================
bool AppStoreUI::downloadFile(const String& url, const String& destPath, const String& loadingMsg) {
    const UiMetrics& m = M();
    if (WiFi.status() != WL_CONNECTED) {
        dialogMessage = "Please turn on WiFi first\nto access the app store.";
        return false;
    }
    
    HTTPClient http;
    http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
    http.setTimeout(15000);
    http.setReuse(false);
    http.setUserAgent(String("KryonOS/") + KRYONOS_VERSION);

    WiFiClientSecure secureClient;
    WiFiClient plainClient;

    if (url.startsWith("https://")) {
        TLSHelper::configureTLS(secureClient, url);
        if (!http.begin(secureClient, url)) {
            dialogMessage = "SSL Connect Failed";
            return false;
        }
    } else {
        if (!http.begin(plainClient, url)) {
            dialogMessage = "HTTP Connect Failed";
            return false;
        }
    }
    
    // Draw initial progress UI
    tftInstance->fillScreen(TFT_BLACK);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString(loadingMsg, m.centerX, (int16_t)(m.progressBar.y - 20), m.fontBody);
    tftInstance->drawRect(m.progressBar.x, m.progressBar.y, m.progressBar.w, m.progressBar.h, TFT_WHITE);
    
    int httpCode = http.GET();
    if (httpCode > 0 && httpCode == HTTP_CODE_OK) {
        int totalLen = http.getSize();
        int downloaded = 0;
        
        WiFiClient *stream = http.getStreamPtr();
        fs::FS* targetFS = &LittleFS;
        String relPath = destPath;
        if (destPath.startsWith("/sd/")) {
            targetFS = FileSystem::sdVolume();
            relPath = destPath.substring(3);
        } else if (destPath.startsWith("/local/")) {
            targetFS = &LittleFS;
            relPath = destPath.substring(6);
        }
        
        // Ensure parent directory exists before writing
        int lastSlash = relPath.lastIndexOf('/');
        if (lastSlash > 0) {
            String parentDir = relPath.substring(0, lastSlash);
            if (!targetFS->exists(parentDir.c_str())) {
                targetFS->mkdir(parentDir.c_str());
            }
        }
        
        File file = targetFS->open(relPath, FILE_WRITE);
        if (!file) {
            dialogMessage = "Error: FS Write " + String(relPath);
            http.end();
            return false;
        }
        
        uint8_t buff[512] = { 0 };
        int len;
        
        while (http.connected() && (totalLen == -1 || downloaded < totalLen)) {
            size_t size = stream->available();
            if (size) {
                int readLen = stream->readBytes(buff, ((size > sizeof(buff)) ? sizeof(buff) : size));
                if (readLen > 0) {
                    file.write(buff, readLen);
                    downloaded += readLen;
                    
                    // Update Progress Bar
                    if (totalLen > 0) {
                        int progressWidth = map(downloaded, 0, totalLen, 0, 176);
                        tftInstance->fillRect(32, 162, progressWidth, 16, TFT_GREEN);
                    }
                }
            } else {
                delay(1);
            }
        }
        file.close();
        http.end();
        return true;
    } else {
        dialogMessage = "Error HTTP " + String(httpCode);
        http.end();
        return false;
    }
}

bool AppStoreUI::fetchCategories() {
    String tmpPath = "/tmp_index.json";
    if (!downloadFile(INDEX_URL, tmpPath, "Fetching App Store...")) {
        return false;
    }
    
    File file = LittleFS.open(tmpPath, "r");
    if (!file) {
        dialogMessage = "Failed to open index";
        return false;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    LittleFS.remove(tmpPath);
    
    if (error) {
        dialogMessage = "JSON Parse Failed";
        return false;
    }
    
    JsonObject categories = doc["categories"];
    categoryCount = 0;
    
    // Add Check for Updates category
    categoryNames[categoryCount] = "[ Check For Apps Update ]";
    categoryUrls[categoryCount] = "UPDATE_ACTION";
    categoryCount++;
    
    for (JsonPair kv : categories) {
        if (categoryCount >= 20) break;
        categoryNames[categoryCount] = kv.key().c_str();
        categoryUrls[categoryCount] = kv.value().as<String>();
        categoryCount++;
    }
    
    return true;
}

bool AppStoreUI::fetchCategoryApps(const String& url) {
    if (url == "UPDATE_ACTION") return checkUpdates();
    
    String tmpPath = "/tmp_category.json";
    if (!downloadFile(url, tmpPath, "Loading " + currentCategoryName + "...")) {
        return false;
    }
    
    File file = LittleFS.open(tmpPath, "r");
    if (!file) {
        dialogMessage = "Failed to open category";
        return false;
    }
    
    JsonDocument doc;
    DeserializationError error = deserializeJson(doc, file);
    file.close();
    LittleFS.remove(tmpPath);
    
    if (error) {
        dialogMessage = "Category Parse Failed";
        return false;
    }
    
    JsonObject apps = doc["apps"];
    currentAppCount = 0;
    
    for (JsonPair kv : apps) {
        if (currentAppCount >= 50) break;
        String id = kv.key().c_str();
        JsonObject appData = kv.value().as<JsonObject>();
        
        // Filter out apps that require a newer OS
        int requiredApi = appData["api"] | 1;
        if (requiredApi > KRYONOS_API_LEVEL) continue;
        
        currentApps[currentAppCount].id = id;
        currentApps[currentAppCount].metaUrl = appData["meta"].as<String>();
        currentApps[currentAppCount].metaSha256 = appData["meta_sha256"] | (appData["metaSha256"] | "");
        currentApps[currentAppCount].appUrl = appData["app"].as<String>();
        currentApps[currentAppCount].appSha256 = appData["app_sha256"] | (appData["appSha256"] | "");
        
        // Default placeholders before fetching meta
        String displayName = id;
        if (displayName.length() > 0) {
            displayName.setCharAt(0, toupper(displayName[0]));
        }
        currentApps[currentAppCount].name = displayName; 
        currentApps[currentAppCount].description = "Select to fetch details";
        currentApps[currentAppCount].author = "Unknown";
        currentApps[currentAppCount].version = "1.0.0";
        
        currentAppCount++;
    }
    
    // Sort apps alphabetically by name (case-insensitive)
    for (int i = 0; i < currentAppCount - 1; i++) {
        for (int j = 0; j < currentAppCount - i - 1; j++) {
            String name1 = currentApps[j].name; name1.toLowerCase();
            String name2 = currentApps[j + 1].name; name2.toLowerCase();
            if (name1.compareTo(name2) > 0) {
                AppStoreItem temp = currentApps[j];
                currentApps[j] = currentApps[j + 1];
                currentApps[j + 1] = temp;
            }
        }
    }
    
    return true;
}

int AppStoreUI::compareVersions(const String& v1, const String& v2) {
    int p1 = 0, p2 = 0;
    while(p1 < v1.length() || p2 < v2.length()) {
        int n1 = 0, n2 = 0;
        while(p1 < v1.length() && v1[p1] != '.') n1 = n1 * 10 + (v1[p1++] - '0');
        while(p2 < v2.length() && v2[p2] != '.') n2 = n2 * 10 + (v2[p2++] - '0');
        if (n1 > n2) return 1;
        if (n1 < n2) return -1;
        p1++; p2++;
    }
    return 0;
}

bool AppStoreUI::checkUpdates() {
    updateAppCount = 0;
    isUpdateMode = true;
    
    if (WiFi.status() != WL_CONNECTED) {
        dialogMessage = "Please turn on WiFi first\nto check for updates.";
        return false;
    }
    
    for (int i=0; i<2; i++) {
        if (i == 0 && !FileSystem::isSDMounted()) continue;
        fs::FS* targetFS = (i == 0) ? FileSystem::sdVolume() : (fs::FS*)&LittleFS;
        if (!targetFS->exists("/apps")) continue;
        
        File root = targetFS->open("/apps");
        if (!root || !root.isDirectory()) continue;
        
        File appDir = root.openNextFile();
        while (appDir) {
            if (appDir.isDirectory()) {
                String appJsonPath = "/apps/";
                String dName = appDir.name();
                if (dName.lastIndexOf('/') >= 0) dName = dName.substring(dName.lastIndexOf('/') + 1);
                appJsonPath += dName + "/app.json";
                if (targetFS->exists(appJsonPath)) {
                    File jsonFile = targetFS->open(appJsonPath, "r");
                    if (jsonFile) {
                        JsonDocument doc;
                        if (!deserializeJson(doc, jsonFile)) {
                            String metaUrl = doc["metaUrl"].as<String>();
                            String localVer = doc["version"].as<String>();
                            String pkgName = doc["packageName"].as<String>();
                            String name = doc["name"].as<String>();
                            
                            if (metaUrl.length() > 0 && updateAppCount < 50) {
                                String tmpPath = "/tmp_update.json";
                                if (downloadFile(metaUrl, tmpPath, "Checking " + name + "...")) {
                                    File remoteJson = LittleFS.open(tmpPath, "r");
                                    if (remoteJson) {
                                        JsonDocument rdoc;
                                        if (!deserializeJson(rdoc, remoteJson)) {
                                            String remoteVer = rdoc["version"].as<String>();
                                            int remoteApi = rdoc["api"] | 1;
                                            
                                            if (compareVersions(remoteVer, localVer) > 0) {
                                                String baseUrl = metaUrl;
                                                int lastSlash = baseUrl.lastIndexOf('/');
                                                if (lastSlash > 0) baseUrl = baseUrl.substring(0, lastSlash + 1);
                                                
                                                updateApps[updateAppCount].id = pkgName;
                                                updateApps[updateAppCount].name = rdoc["name"] | name;
                                                updateApps[updateAppCount].version = remoteVer;
                                                updateApps[updateAppCount].author = rdoc["author"] | "Unknown";
                                                String changelog = rdoc["changelog"] | "";
                                                if (changelog.length() > 0) {
                                                    updateApps[updateAppCount].description = changelog;
                                                } else {
                                                    updateApps[updateAppCount].description = "Update available!";
                                                }
                                                updateApps[updateAppCount].metaUrl = metaUrl;
                                                updateApps[updateAppCount].appUrl = baseUrl + "main.js";
                                                updateAppCount++;
                                            }
                                        }
                                        remoteJson.close();
                                    }
                                    LittleFS.remove(tmpPath);
                                }
                            }
                        }
                        jsonFile.close();
                    }
                }
            }
            appDir = root.openNextFile();
        }
    }
    
    // Copy to currentApps so the UI uses it
    currentAppCount = updateAppCount;
    for (int i=0; i<updateAppCount; i++) {
        currentApps[i] = updateApps[i];
    }
    
    if (currentAppCount == 0) {
        currentCategoryName = "All Apps are up to date";
        return true;
    } else {
        currentCategoryName = "Update Available";
    }
    
    // Sort apps alphabetically by name (case-insensitive)
    for (int i = 0; i < currentAppCount - 1; i++) {
        for (int j = 0; j < currentAppCount - i - 1; j++) {
            String name1 = currentApps[j].name; name1.toLowerCase();
            String name2 = currentApps[j + 1].name; name2.toLowerCase();
            if (name1.compareTo(name2) > 0) {
                AppStoreItem temp = currentApps[j];
                currentApps[j] = currentApps[j + 1];
                currentApps[j + 1] = temp;
            }
        }
    }
    
    return true;
}

void AppStoreUI::performInstall(int appIdx) {
    AppStoreItem& app = currentApps[appIdx];
    
    String destFolder = "/local/tmp_download/";
    if (FileSystem::exists(destFolder.c_str())) {
        FileSystem::deleteFile((destFolder + "app.json").c_str());
        FileSystem::deleteFile((destFolder + "main.js").c_str());
        FileSystem::rmdir(destFolder.c_str());
    }
    FileSystem::mkdir(destFolder.c_str());
    
    bool metaOk = downloadFile(app.metaUrl, destFolder + "app.json", "Downloading Meta...");
    if (!metaOk) return;

    // Mandatory SHA-256 presence check: reject unsigned/unhashed packages
    if (app.appSha256.length() == 0 || app.metaSha256.length() == 0) {
        FileSystem::deleteFile((destFolder + "app.json").c_str());
        FileSystem::rmdir(destFolder.c_str());
        dialogMessage = "Installation Blocked:\nHash Not Found in store.";
        storeState = 4;
        drawDialog();
        return;
    }

    // Verify SHA-256 integrity of app.json
    String metaHash = CryptoEngine::sha256File((destFolder + "app.json").c_str());
    if (!metaHash.equalsIgnoreCase(app.metaSha256)) {
        FileSystem::deleteFile((destFolder + "app.json").c_str());
        FileSystem::rmdir(destFolder.c_str());
        dialogMessage = "Installation Failed:\nIntegrity Check Mismatch\non app metadata.";
        storeState = 4;
        drawDialog();
        return;
    }
    
    bool appOk = downloadFile(app.appUrl, destFolder + "main.js", "Downloading App...");
    if (!appOk) {
        FileSystem::deleteFile((destFolder + "app.json").c_str());
        FileSystem::deleteFile((destFolder + "main.js").c_str());
        FileSystem::rmdir(destFolder.c_str());
        return;
    }

    // Verify SHA-256 integrity of main.js
    String appHash = CryptoEngine::sha256File((destFolder + "main.js").c_str());
    if (!appHash.equalsIgnoreCase(app.appSha256)) {
        FileSystem::deleteFile((destFolder + "app.json").c_str());
        FileSystem::deleteFile((destFolder + "main.js").c_str());
        FileSystem::rmdir(destFolder.c_str());
        dialogMessage = "Installation Failed:\nIntegrity Check Mismatch\non application payload.";
        storeState = 4;
        drawDialog();
        return;
    }
    
    InstallerUI::autoInstallPath = destFolder;
    currentState = 3; // STATE_INSTALLER
    storeState = 0;   // Reset AppStoreUI state
}

// ============================================================
// UI Draw Methods
// ============================================================
void AppStoreUI::drawCategories() {
    const UiMetrics& m = M();
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_GREEN);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("App Store", m.header.cx(), m.headerTextY, m.fontBody);
    
    tftInstance->fillRect(m.list.x, m.list.y, m.list.w, m.list.h, TFT_BLACK);

    const int itemsPerPage = m.itemsPerPage;
    int totalItems = categoryCount;
    
    if (totalItems == 0) {
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->drawString("No categories found.", m.centerX, m.listMessageY, m.fontBody);
    } else {
        for (int i = 0; i < itemsPerPage; i++) {
            int listIndex = scrollOffset + i;
            if (listIndex >= totalItems) break;

            drawAppStoreRow(tftInstance, m, i, categoryNames[listIndex], listIndex == selectedIndex);
        }
    }
    
    // Footer
    drawStoreFooter(tftInstance, m);
}

void AppStoreUI::drawAppList() {
    const UiMetrics& m = M();
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_GREEN);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString(currentCategoryName, m.header.cx(), m.headerTextY, m.fontBody);
    
    tftInstance->fillRect(m.list.x, m.list.y, m.list.w, m.list.h, TFT_BLACK);

    const int itemsPerPage = m.itemsPerPage;
    int totalItems = currentAppCount;
    
    if (totalItems == 0) {
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        if (isUpdateMode || currentCategoryName.indexOf("Update") != -1 || currentCategoryName.indexOf("up to date") != -1) {
            tftInstance->drawString("No Update found.", m.centerX, m.listMessageY, m.fontBody);
        } else {
            tftInstance->drawString("No apps found.", m.centerX, m.listMessageY, m.fontBody);
        }
    } else {
        for (int i = 0; i < itemsPerPage; i++) {
            int listIndex = scrollOffset + i;
            if (listIndex >= totalItems) break;

            drawAppStoreRow(tftInstance, m, i, currentApps[listIndex].name,
                            listIndex == selectedIndex);
        }
    }
    
    // Footer
    drawStoreFooter(tftInstance, m);
}

void AppStoreUI::drawAppInfo() {
    const UiMetrics& m = M();
    tftInstance->fillScreen(TFT_BLACK);
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);
    
    AppStoreItem& app = currentApps[selectedAppIndex];
    
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_GREEN);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("App Details", m.header.cx(), m.headerTextY, m.fontBody);
    
    int y = m.list.y;
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(TL_DATUM);

    tftInstance->drawString("Name:", m.list.x, y, m.fontBody); y += 18;
    tftInstance->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tftInstance->drawString(app.name, m.list.x, y, m.fontBody); y += 22;

    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->drawString("Author:", m.list.x, y, m.fontBody); y += 18;
    tftInstance->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tftInstance->drawString(app.author, m.list.x, y, m.fontBody); y += 22;

    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->drawString("Version:", m.list.x, y, m.fontBody); y += 18;
    tftInstance->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tftInstance->drawString(app.version, m.list.x, y, m.fontBody); y += 22;

    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    if (isUpdateMode) {
        tftInstance->drawString("What's New:", m.list.x, y, m.fontBody); y += 18;
    } else {
        tftInstance->drawString("Description:", m.list.x, y, m.fontBody); y += 18;
    }
    tftInstance->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    
    String desc = app.description;
    while(desc.length() > 0) {
        int splitIdx = 25;
        if(desc.length() <= 25) splitIdx = desc.length();
        else {
            int spaceIdx = desc.lastIndexOf(' ', 25);
            if(spaceIdx > 0) splitIdx = spaceIdx;
        }
        tftInstance->drawString(desc.substring(0, splitIdx), m.list.x, y, m.fontBody);
        desc = desc.substring(splitIdx);
        desc.trim();
        y += 15;
    }
    
    // Action Buttons
    const UiRect downloadBtn = m.dialogButton(m.dialogButtonRowY, 30, 0, 2, 80);
    const UiRect cancelBtn   = m.dialogButton(m.dialogButtonRowY, 30, 1, 2, 80);
    tftInstance->fillRoundRect(downloadBtn.x, downloadBtn.y, downloadBtn.w, downloadBtn.h, 5, TFT_GREEN);
    tftInstance->setTextColor(TFT_BLACK, TFT_GREEN);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString(isUpdateMode ? "UPDATE" : "DOWNLOAD",
                            downloadBtn.cx(), downloadBtn.cy(), m.fontBody);

    tftInstance->drawRoundRect(cancelBtn.x, cancelBtn.y, cancelBtn.w, cancelBtn.h, 5, TFT_WHITE);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->drawString("CANCEL", cancelBtn.cx(), cancelBtn.cy(), m.fontBody);
}

void AppStoreUI::drawDialog() {
    const UiMetrics& m = M();
    tftInstance->fillScreen(TFT_BLACK);
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);
    
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_GREEN);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Notice", m.header.cx(), m.headerTextY, m.fontBody);
    
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    
    // Split dialogMessage by '\n' and word-wrap lines if wider than 24 chars
    std::vector<String> lines;
    int start = 0;
    while (start < (int)dialogMessage.length()) {
        int nextNl = dialogMessage.indexOf('\n', start);
        String seg = (nextNl >= 0) ? dialogMessage.substring(start, nextNl) : dialogMessage.substring(start);
        start = (nextNl >= 0) ? nextNl + 1 : dialogMessage.length();
        seg.trim();
        
        while (seg.length() > 0) {
            if (seg.length() <= 24) {
                lines.push_back(seg);
                break;
            }
            int splitIdx = 24;
            int spaceIdx = seg.lastIndexOf(' ', 24);
            if (spaceIdx > 0) splitIdx = spaceIdx;
            lines.push_back(seg.substring(0, splitIdx));
            seg = seg.substring(splitIdx);
            seg.trim();
        }
    }
    
    int numLines = lines.size();
    if (numLines == 0) numLines = 1;
    int startY = (m.centerY - 25) - ((numLines - 1) * 11);
    if (startY < 50) startY = 50;

    for (size_t i = 0; i < lines.size(); i++) {
        tftInstance->drawString(lines[i], m.centerX, startY + (i * 22), m.fontBody);
    }

    const UiRect okBtn = m.dialogButton((int16_t)(m.dialogButtonRowY - 10), 30, 0, 1, 70);
    tftInstance->drawRoundRect(okBtn.x, okBtn.y, okBtn.w, okBtn.h, 5, TFT_WHITE);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("OK", okBtn.cx(), okBtn.cy(), m.fontBody);
}

// ============================================================
// Touch Handler
// ============================================================
void AppStoreUI::handleTouch(uint16_t x, uint16_t y) {
    const UiMetrics& m = M();
    if (storeState == 0) { // Categories
        const int rowIndex = m.listRowFromY((int16_t)y);
        if (rowIndex >= 0) {
            int clickedAbs = scrollOffset + rowIndex;
            if (clickedAbs < categoryCount) {
                selectedIndex = clickedAbs;
                currentCategoryName = categoryNames[clickedAbs];
                isUpdateMode = (categoryUrls[clickedAbs] == "UPDATE_ACTION");
                
                bool ok = fetchCategoryApps(categoryUrls[clickedAbs]);
                if (!ok) {
                    storeState = 4;
                    drawDialog();
                } else {
                    storeState = 1;
                    selectedIndex = 0;
                    scrollOffset = 0;
                    draw();
                }
            }
            return;
        }
        
        if (m.inFooter((int16_t)y)) {
            const int slot = m.footerSlotFromX((int16_t)x);
            if (slot == UI_SLOT_BACK) { // BACK
                currentState = 0;
                categoryCount = 0; // force refetch next time
            } else if (slot == UI_SLOT_UP) { // UP
                if (selectedIndex > 0) {
                    selectedIndex--;
                    if (selectedIndex < scrollOffset) scrollOffset--;
                    draw();
                }
            } else if (slot == UI_SLOT_SEL) { // SEL
                currentCategoryName = categoryNames[selectedIndex];
                isUpdateMode = (categoryUrls[selectedIndex] == "UPDATE_ACTION");
                
                bool ok = fetchCategoryApps(categoryUrls[selectedIndex]);
                if (!ok) {
                    storeState = 4;
                    drawDialog();
                } else {
                    storeState = 1;
                    selectedIndex = 0;
                    scrollOffset = 0;
                    draw();
                }
            } else if (slot == UI_SLOT_DN) { // DN
                if (selectedIndex < categoryCount - 1) {
                    selectedIndex++;
                    if (selectedIndex >= scrollOffset + m.itemsPerPage) scrollOffset++;
                    draw();
                }
            }
        }
    } else if (storeState == 1) { // App List
        const int rowIndex = m.listRowFromY((int16_t)y);
        if (rowIndex >= 0) {
            int clickedAbs = scrollOffset + rowIndex;
            if (clickedAbs < currentAppCount) {
                selectedIndex = clickedAbs;
                selectedAppIndex = clickedAbs;
                
                // Fetch Meta for details
                String tmpPath = "/tmp_meta.json";
                if (downloadFile(currentApps[selectedAppIndex].metaUrl, tmpPath, "Loading details...")) {
                    File file = LittleFS.open(tmpPath, "r");
                    if (file) {
                        JsonDocument doc;
                        if (!deserializeJson(doc, file)) {
                            int appApi = doc["api"] | 1;
                            if (appApi > KRYONOS_API_LEVEL) {
                                file.close();
                                LittleFS.remove(tmpPath);
                                if (isUpdateMode) {
                                    dialogMessage = "API " + String(appApi) + " needed to update.\nPlease update OS first!";
                                } else {
                                    dialogMessage = "This App Requires KryonOS API " + String(appApi) + "\nPlease update OS!";
                                }
                                storeState = 4;
                                drawDialog();
                                return;
                            }
                            currentApps[selectedAppIndex].name = doc["name"] | currentApps[selectedAppIndex].id;
                            currentApps[selectedAppIndex].description = doc["description"] | "No description.";
                            currentApps[selectedAppIndex].author = doc["author"] | "Unknown";
                            currentApps[selectedAppIndex].version = doc["version"] | "1.0.0";
                            
                            // Check if installed and if this is an update
                            isUpdateMode = false;
                            String pkgName = doc["packageName"] | currentApps[selectedAppIndex].id;
                            for (int fsIdx=0; fsIdx<2; fsIdx++) {
                                String localPath = (fsIdx == 0 ? "/sd/apps/" : "/local/apps/") + pkgName + "/app.json";
                                if (FileSystem::exists(localPath.c_str())) {
                                    String localJson = FileSystem::readTextFile(localPath.c_str());
                                    if (localJson.length() > 0) {
                                        String localVer = FileSystem::parseJsonValue(localJson, "version");
                                        if (compareVersions(currentApps[selectedAppIndex].version, localVer) > 0) {
                                            isUpdateMode = true;
                                        }
                                    }
                                }
                            }
                            
                            if (isUpdateMode) {
                                String changelog = doc["changelog"] | "";
                                if (changelog.length() > 0) {
                                    currentApps[selectedAppIndex].description = changelog;
                                } else {
                                    currentApps[selectedAppIndex].description = "Update available!";
                                }
                            }
                        }
                        file.close();
                        LittleFS.remove(tmpPath);
                    }
                }
                
                storeState = 2;
                draw();
            }
            return;
        }
        
        if (m.inFooter((int16_t)y)) {
            const int slot = m.footerSlotFromX((int16_t)x);
            if (slot == UI_SLOT_BACK) { // BACK
                storeState = 0;
                selectedIndex = 0;
                scrollOffset = 0;
                draw();
            } else if (slot == UI_SLOT_UP) { // UP
                if (selectedIndex > 0) {
                    selectedIndex--;
                    if (selectedIndex < scrollOffset) scrollOffset--;
                    draw();
                }
            } else if (slot == UI_SLOT_SEL) { // SEL
                selectedAppIndex = selectedIndex;
                
                // Fetch Meta for details
                String tmpPath = "/tmp_meta.json";
                if (downloadFile(currentApps[selectedAppIndex].metaUrl, tmpPath, "Loading details...")) {
                    File file = LittleFS.open(tmpPath, "r");
                    if (file) {
                        JsonDocument doc;
                        if (!deserializeJson(doc, file)) {
                            int appApi = doc["api"] | 1;
                            if (appApi > KRYONOS_API_LEVEL) {
                                file.close();
                                LittleFS.remove(tmpPath);
                                if (isUpdateMode) {
                                    dialogMessage = "API " + String(appApi) + " needed to update.\nPlease update OS first!";
                                } else {
                                    dialogMessage = "This App Requires KryonOS API " + String(appApi) + "\nPlease update OS!";
                                }
                                storeState = 4;
                                drawDialog();
                                return;
                            }
                            currentApps[selectedAppIndex].name = doc["name"] | currentApps[selectedAppIndex].id;
                            currentApps[selectedAppIndex].description = doc["description"] | "No description.";
                            currentApps[selectedAppIndex].author = doc["author"] | "Unknown";
                            currentApps[selectedAppIndex].version = doc["version"] | "1.0.0";
                            
                            // Check if installed and if this is an update
                            isUpdateMode = false;
                            String pkgName = doc["packageName"] | currentApps[selectedAppIndex].id;
                            for (int fsIdx=0; fsIdx<2; fsIdx++) {
                                String localPath = (fsIdx == 0 ? "/sd/apps/" : "/local/apps/") + pkgName + "/app.json";
                                if (FileSystem::exists(localPath.c_str())) {
                                    String localJson = FileSystem::readTextFile(localPath.c_str());
                                    if (localJson.length() > 0) {
                                        String localVer = FileSystem::parseJsonValue(localJson, "version");
                                        if (compareVersions(currentApps[selectedAppIndex].version, localVer) > 0) {
                                            isUpdateMode = true;
                                        }
                                    }
                                }
                            }
                            
                            if (isUpdateMode) {
                                String changelog = doc["changelog"] | "";
                                if (changelog.length() > 0) {
                                    currentApps[selectedAppIndex].description = changelog;
                                } else {
                                    currentApps[selectedAppIndex].description = "Update available!";
                                }
                            }
                        }
                        file.close();
                        LittleFS.remove(tmpPath);
                    }
                }
                
                storeState = 2;
                draw();
            } else if (slot == UI_SLOT_DN) { // DN
                if (selectedIndex < currentAppCount - 1) {
                    selectedIndex++;
                    if (selectedIndex >= scrollOffset + m.itemsPerPage) scrollOffset++;
                    draw();
                }
            }
        }
    } else if (storeState == 2) { // App Info
        const UiRect installBtn = m.dialogButton(m.dialogButtonRowY, 30, 0, 2, 80);
        const UiRect cancelBtn  = m.dialogButton(m.dialogButtonRowY, 30, 1, 2, 80);
        if (installBtn.contains((int16_t)x, (int16_t)y)) { // INSTALL
            performInstall(selectedAppIndex);
            draw();
        } else if (cancelBtn.contains((int16_t)x, (int16_t)y)) { // CANCEL
            storeState = 1;
            draw();
        }
    } else if (storeState == 3 || storeState == 4) { // Dialog
        const UiRect okBtn = m.dialogButton((int16_t)(m.dialogButtonRowY - 10), 30, 0, 1, 70);
        if (okBtn.contains((int16_t)x, (int16_t)y)) {
            if (categoryCount == 0) {
                extern int currentState;
                currentState = 0; // Back to Launcher
            } else {
                storeState = 0; // return to categories on dialog close
                draw();
            }
        }
    }
}
