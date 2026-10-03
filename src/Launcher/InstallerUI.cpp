#include "InstallerUI.h"
#include "../FileSystem/FileSystem.h"
#include "../Kernel/Core/HarixKernel.h"
#include "LauncherUI.h"
#include "../UI/UiLayout.h"
#include <ArduinoJson.h>

// External state variable
extern int currentState;

// Current screen metrics. See Documentation/Display_Touch_Architecture.md.
static inline const UiMetrics& M() { return UiLayout::current(); }

KryonDisplay *InstallerUI::tftInstance = nullptr;
FileEntry InstallerUI::files[200];
int InstallerUI::fileCount = 0;
String InstallerUI::currentPath = "/";
String InstallerUI::autoInstallPath = "";
int InstallerUI::scrollOffset = 0;
int InstallerUI::selectedIndex = 0;
String InstallerUI::selectedFile = "";
bool InstallerUI::showActionDialog = false;
String InstallerUI::displayNames[200];
bool InstallerUI::isAppPackage[200];

static int installState = 0; // 0=None, 1=OverwritePrompt, 2=Result, 3=AppInfo, 4=Installing, 5=PermissionReview
static bool installResultOk = false;
static bool installSyntaxError = false;
static bool installApiError = false;
static bool installNoMetadata = false;
String syntaxErrorMessage = "";
static AppMetadata currentAppMeta;
static bool isUpdatingApp = false;

static void saveAppPermissions(const String& pkg, const std::vector<String>& perms) {
    if (pkg.length() == 0 || perms.empty()) return;
    String path = "/local/system/app_permissions.json";
    JsonDocument doc;
    if (FileSystem::exists(path.c_str())) {
        String existing = FileSystem::readTextFile(path.c_str());
        deserializeJson(doc, existing);
    }
    JsonArray arr = doc[pkg].to<JsonArray>();
    for (const auto& p : perms) {
        arr.add(p);
    }
    String out;
    serializeJson(doc, out);
    FileSystem::writeTextFile(path.c_str(), out.c_str());
}

// Static pointer for progress callback
static KryonDisplay* progressTft = nullptr;

static bool isVersionGreater(const String& newVer, const String& oldVer) {
    int newParts[3] = {0, 0, 0};
    int oldParts[3] = {0, 0, 0};
    
    auto parseVer = [](const String& v, int* parts) {
        int partIdx = 0;
        int startIdx = 0;
        while (partIdx < 3 && startIdx < (int)v.length()) {
            int dotIdx = v.indexOf('.', startIdx);
            if (dotIdx == -1) {
                parts[partIdx] = v.substring(startIdx).toInt();
                break;
            }
            parts[partIdx] = v.substring(startIdx, dotIdx).toInt();
            startIdx = dotIdx + 1;
            partIdx++;
        }
    };
    
    parseVer(newVer, newParts);
    parseVer(oldVer, oldParts);
    
    if (newParts[0] > oldParts[0]) return true;
    if (newParts[0] < oldParts[0]) return false;
    
    if (newParts[1] > oldParts[1]) return true;
    if (newParts[1] < oldParts[1]) return false;
    
    if (newParts[2] > oldParts[2]) return true;
    return false;
}

void InstallerUI::init(KryonDisplay *tft) {
    tftInstance = tft;
    progressTft = tft;
}

// ============================================================
// App Metadata Parsing
// ============================================================

AppMetadata InstallerUI::parseAppJson(const String& folderPath) {
    AppMetadata meta;
    meta.valid = false;
    meta.api = 0;
    meta.folderPath = folderPath;
    
    String jsonPath = folderPath;
    if (!jsonPath.endsWith("/")) jsonPath += "/";
    jsonPath += "app.json";
    
    if (!FileSystem::exists(jsonPath.c_str())) {
        return meta;
    }
    
    String content = FileSystem::readTextFile(jsonPath.c_str());
    if (content.length() == 0) {
        return meta;
    }
    
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, content);
    if (!err) {
        meta.name = doc["name"] | "";
        meta.packageName = doc["packageName"] | (doc["package"] | "");
        meta.version = doc["version"] | "1.0.0";
        meta.author = doc["author"] | "Unknown";
        meta.type = doc["type"] | "App";
        meta.category = doc["category"] | "Utility";
        meta.description = doc["description"] | "";
        meta.changelog = doc["changelog"] | "";
        meta.api = doc["api"] | 1;

        JsonArray perms = doc["permissions"].as<JsonArray>();
        if (!perms.isNull()) {
            for (JsonVariant p : perms) {
                String pStr = p.as<String>();
                if (pStr.length() > 0) {
                    meta.permissions.push_back(pStr);
                }
            }
        }
    } else {
        meta.name = FileSystem::parseJsonValue(content, "name");
        meta.packageName = FileSystem::parseJsonValue(content, "packageName");
        meta.version = FileSystem::parseJsonValue(content, "version");
        meta.author = FileSystem::parseJsonValue(content, "author");
        meta.type = FileSystem::parseJsonValue(content, "type");
        meta.category = FileSystem::parseJsonValue(content, "category");
        meta.description = FileSystem::parseJsonValue(content, "description");
        meta.changelog = FileSystem::parseJsonValue(content, "changelog");
        meta.api = FileSystem::parseJsonValue(content, "api").toInt();
    }
    
    if (meta.name.length() > 0) {
        meta.valid = true;
    }
    
    return meta;
}

// ============================================================
// Scanning
// ============================================================

static bool needsRescan = true;
static String lastScannedPath = "";

void InstallerUI::scanSD() {
    if (!needsRescan && currentPath == lastScannedPath) return;
    
    needsRescan = false;
    lastScannedPath = currentPath;
    if (currentPath == "/") {
        fileCount = 3;
        files[0].name = "SD Card";
        files[0].path = "/sd/";
        files[0].isDir = true;
        files[1].name = "Internal Storage";
        files[1].path = "/local/";
        files[1].isDir = true;
        files[2].name = "Help / Guide";
        files[2].path = "/help/";
        files[2].isDir = true;
        
        for (int i = 0; i < 3; i++) {
            displayNames[i] = files[i].name;
            isAppPackage[i] = false;
        }
    } else {
        fileCount = FileSystem::listDirectory(currentPath.c_str(), files, 200);
        
        if (fileCount > 0) {
            const UiMetrics& m = M();
            const int16_t s = (int16_t)m.scale;
            tftInstance->fillScreen(TFT_BLACK);
            tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("Loading App Details...", m.centerX,
                                    (int16_t)(m.centerY - 20 * s), m.fontBody);
            // The shared progress track, so the fill below lands inside the outline on any panel.
            const UiRect bar = m.progressBar;
            tftInstance->drawRect(bar.x, bar.y, bar.w, bar.h, TFT_WHITE);
            // The directory walk below reads every app.json, which on SD is slow enough to see; the
            // main loop is sitting inside this call. See MyKeyboard::getString().
            tftInstance->present();
        }

        // Check each directory for app.json
        for (int i = 0; i < fileCount; i++) {
            if (fileCount > 0) {
                const UiRect bar = M().progressBar;
                int progressWidth = map(i, 0, fileCount, 0, bar.w - 4);
                tftInstance->fillRect((int16_t)(bar.x + 2), (int16_t)(bar.y + 2), progressWidth,
                                      (int16_t)(bar.h - 4), TFT_GREEN);
            }
            
            isAppPackage[i] = false;
            displayNames[i] = files[i].name;
            
            if (files[i].isDir) {
                String appJsonPath = files[i].path;
                if (!appJsonPath.endsWith("/")) appJsonPath += "/";
                appJsonPath += "app.json";
                
                if (FileSystem::exists(appJsonPath.c_str())) {
                    // It's an app package! Read the name and type from app.json
                    String jsonContent = FileSystem::readTextFile(appJsonPath.c_str());
                    String appName = FileSystem::parseJsonValue(jsonContent, "name");
                    String appType = FileSystem::parseJsonValue(jsonContent, "type");
                    if (appType.length() == 0) appType = "App";
                    if (appName.length() > 0) {
                        displayNames[i] = "[" + appType + "] " + appName;
                        isAppPackage[i] = true;
                    }
                }
            }
        }
        
        // Sort everything by Type (App -> Dir -> File) then alphabetically
        for (int i = 0; i < fileCount - 1; i++) {
            for (int j = i + 1; j < fileCount; j++) {
                int scoreI = isAppPackage[i] ? 0 : (files[i].isDir ? 1 : 2);
                int scoreJ = isAppPackage[j] ? 0 : (files[j].isDir ? 1 : 2);
                
                bool doSwap = false;
                if (scoreI > scoreJ) {
                    doSwap = true;
                } else if (scoreI == scoreJ) {
                    String nameI = files[i].name;
                    nameI.toLowerCase();
                    String nameJ = files[j].name;
                    nameJ.toLowerCase();
                    if (nameI.compareTo(nameJ) > 0) {
                        doSwap = true;
                    }
                }
                
                if (doSwap) {
                    // Swap files
                    FileEntry tempFile = files[i];
                    files[i] = files[j];
                    files[j] = tempFile;
                    
                    // Swap displayNames
                    String tempName = displayNames[i];
                    displayNames[i] = displayNames[j];
                    displayNames[j] = tempName;
                    
                    // Swap isAppPackage
                    bool tempApp = isAppPackage[i];
                    isAppPackage[i] = isAppPackage[j];
                    isAppPackage[j] = tempApp;
                }
            }
        }
    }
    
    // Reset selection state if we hit bounds
    bool hasUp = (currentPath != "/");
    if (selectedIndex >= fileCount + (hasUp ? 1 : 0)) {
        selectedIndex = 0;
        scrollOffset = 0;
    }
}

// ============================================================
// Drawing
// ============================================================

void InstallerUI::draw() {
    if (!tftInstance) return;
    
    if (autoInstallPath.length() > 0) {
        selectedFile = autoInstallPath;
        if (!selectedFile.endsWith("/")) selectedFile += "/";
        currentAppMeta = parseAppJson(selectedFile);
        
        bool defaultSD = FileSystem::exists("/local/config_install_sd.txt");
        if (defaultSD && !FileSystem::exists("/sd/")) defaultSD = false;
        String destBase = defaultSD ? "/sd/apps/" : "/local/apps/";
        String destFolder = destBase + currentAppMeta.packageName + "/";
        
        isUpdatingApp = false;
        installSyntaxError = false;
        
        if (FileSystem::exists(destFolder.c_str())) {
            String installedJsonPath = destFolder + "app.json";
            if (FileSystem::exists(installedJsonPath.c_str())) {
                String installedJsonContent = FileSystem::readTextFile(installedJsonPath.c_str());
                String installedAuthor = FileSystem::parseJsonValue(installedJsonContent, "author");
                String installedVersion = FileSystem::parseJsonValue(installedJsonContent, "version");
                
                if (installedAuthor != currentAppMeta.author) {
                    installSyntaxError = true;
                    syntaxErrorMessage = "Author conflict!\nInstalled: " + installedAuthor + "\nNew: " + currentAppMeta.author;
                    installResultOk = false;
                    installState = 2;
                } else if (isVersionGreater(currentAppMeta.version, installedVersion)) {
                    isUpdatingApp = true;
                }
            }
        }
        
        if (!installSyntaxError) {
            installState = 3;
        }
        
        showActionDialog = true;
        autoInstallPath = "";
    }
    
    if (showActionDialog) {
        drawActionDialog();
        return;
    }

    scanSD();
    
    if (currentPath == "/help/") {
        drawHelp();
    } else {
        drawFileList();
    }
}

void InstallerUI::drawFileList() {
    const UiMetrics& m = M();

    // Draw the main border
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);

    // Header Bar
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_GREEN);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    String headerText = "App Installer";
    if (currentPath.startsWith("/sd")) {
        headerText = "App Installer   /sdcard";
    } else if (currentPath.startsWith("/local")) {
        headerText = "App Installer   /internal-storage";
    }

    tftInstance->drawString(headerText, m.header.cx(), m.headerTextY, m.fontBody);

    // Clear only the list area
    tftInstance->fillRect(m.list.x, m.list.y, m.list.w, m.list.h, TFT_BLACK);

    const int itemsPerPage = m.itemsPerPage;
    bool hasUp = (currentPath != "/");
    int totalItems = fileCount + (hasUp ? 1 : 0);

    if (totalItems == 0) {
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->setTextDatum(TC_DATUM);
        tftInstance->drawString("Folder is empty", m.centerX, m.listMessageY, m.fontBody);
    } else {
        for (int i = 0; i < itemsPerPage; i++) {
            int listIndex = scrollOffset + i;
            if (listIndex >= totalItems) break;

            const UiRect row  = m.listRowRect(i);
            const UiRect fill = m.listRowFillRect(i);

            String displayName = "";
            bool isDirectory = false;

            if (hasUp && listIndex == 0) {
                displayName = "[..] UP";
                isDirectory = true;
            } else {
                int fileIdx = listIndex - (hasUp ? 1 : 0);
                isDirectory = files[fileIdx].isDir;

                if (isAppPackage[fileIdx]) {
                    // Show as app package with app name
                    displayName = displayNames[fileIdx];
                } else if (isDirectory && currentPath != "/") {
                    displayName = "[D] " + displayNames[fileIdx];
                } else {
                    displayName = displayNames[fileIdx];
                }
            }

            if (listIndex == selectedIndex) {
                // Highlighted Item
                tftInstance->fillRect(fill.x, fill.y, fill.w, fill.h, TFT_WHITE);
                tftInstance->setTextColor(TFT_BLACK, TFT_WHITE);
                tftInstance->setTextDatum(ML_DATUM);
                tftInstance->drawString(("> " + displayName).c_str(),
                                        (int16_t)(row.x + m.rowTextPadX), m.listRowTextY(i), m.fontBody);
            } else {
                // Normal Item
                uint16_t textColor = TFT_WHITE;
                if (hasUp && listIndex != 0) {
                    int fileIdx = listIndex - (hasUp ? 1 : 0);
                    if (isAppPackage[fileIdx]) textColor = TFT_GREEN;
                }
                tftInstance->setTextColor(textColor, TFT_BLACK);
                tftInstance->setTextDatum(ML_DATUM);
                tftInstance->drawString(("  " + displayName).c_str(),
                                        (int16_t)(row.x + m.rowTextPadX), m.listRowTextY(i), m.fontBody);
            }
        }
    }

    // Touch Footer
    // (We intentionally do NOT clear the footer to prevent blinking on scroll)
    tftInstance->drawRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_WHITE);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);

    // Seven evenly spaced footer slots: ESC | UP | SEL | DN at eighths of the width.
    const int16_t e = m.w / 8; // 30 at 240x320
    const int16_t q = m.w / 4; // 60 at 240x320
    tftInstance->drawString("ESC", m.footerButtonCenterX(UI_FOOTER_UP), m.footerTextY, m.fontBody);
    tftInstance->drawString("|",   q,                                  m.footerTextY, m.fontBody);
    tftInstance->drawString("UP",  (int16_t)(3 * e),                   m.footerTextY, m.fontBody);
    tftInstance->drawString("|",   m.centerX,                          m.footerTextY, m.fontBody);
    tftInstance->drawString("SEL", (int16_t)(5 * e),                   m.footerTextY, m.fontBody);
    tftInstance->drawString("|",   (int16_t)(3 * q),                   m.footerTextY, m.fontBody);
    tftInstance->drawString("DN",  m.footerButtonCenterX(UI_FOOTER_DN), m.footerTextY, m.fontBody);
}

// ============================================================
// Action Dialog Drawing
// ============================================================

void InstallerUI::drawActionDialog() {
    const UiMetrics& m = M();
    // These dialogs are laid out in 240x320 pixels throughout -- panel heights, offsets inside the
    // panel, button sizes. `s` is the text scale (UiLayout), so multiplying every one of them puts
    // the frames around the text instead of the text over the frames. At 240x320 s is 1 and every
    // number below reproduces the historical literal exactly.
    const int16_t s = (int16_t)m.scale;
    const int16_t linePitch = (int16_t)(16 * s);   // one body glyph plus its leading

    tftInstance->fillScreen(TFT_BLACK);

    if (installState == 1) { // Overwrite Prompt
        const UiRect panel  = m.dialogPanel((int16_t)(160 * s));
        const int16_t btnH  = (int16_t)(30 * s);
        const int16_t btnY  = (int16_t)(panel.y + 100 * s);
        const UiRect yesBtn = m.dialogButtonSpaced(btnY, btnH, 0, 2, (int16_t)(70 * s), (int16_t)(40 * s));
        const UiRect noBtn  = m.dialogButtonSpaced(btnY, btnH, 1, 2, (int16_t)(70 * s), (int16_t)(40 * s));

        tftInstance->fillRoundRect(panel.x, panel.y, panel.w, panel.h, (int32_t)(8 * s), TFT_DARKGREY);
        tftInstance->setTextColor(TFT_YELLOW, TFT_DARKGREY);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("App Exists!", panel.cx(), (int16_t)(panel.y + 30 * s), 4);
        tftInstance->setTextColor(TFT_WHITE, TFT_DARKGREY);
        tftInstance->drawString("Overwrite?", panel.cx(), (int16_t)(panel.y + 60 * s), 2);

        tftInstance->fillRoundRect(yesBtn.x, yesBtn.y, yesBtn.w, yesBtn.h, (int32_t)(4 * s), TFT_GREEN);
        tftInstance->setTextColor(TFT_BLACK, TFT_GREEN);
        tftInstance->drawString("Yes", yesBtn.cx(), yesBtn.cy(), 2);

        tftInstance->fillRoundRect(noBtn.x, noBtn.y, noBtn.w, noBtn.h, (int32_t)(4 * s), TFT_RED);
        tftInstance->setTextColor(TFT_WHITE, TFT_RED);
        tftInstance->drawString("No", noBtn.cx(), noBtn.cy(), 2);
        return;
    } else if (installState == 2) { // Result
        const UiRect panel = m.dialogPanel((int16_t)(200 * s));
        const UiRect okBtn = m.dialogButtonSpaced((int16_t)(panel.bottom() - 40 * s), (int16_t)(30 * s),
                                                  0, 1, (int16_t)(70 * s), 0);

        tftInstance->fillRoundRect(panel.x, panel.y, panel.w, panel.h, (int32_t)(8 * s), TFT_DARKGREY);
        tftInstance->setTextColor(TFT_WHITE, TFT_DARKGREY);
        tftInstance->setTextDatum(MC_DATUM);
        if (installResultOk) {
            tftInstance->setTextColor(TFT_GREEN, TFT_DARKGREY);
            tftInstance->drawString("Installed!", panel.cx(), (int16_t)(panel.y + 40 * s), 4);
            tftInstance->setTextColor(TFT_WHITE, TFT_DARKGREY);
            tftInstance->drawString(currentAppMeta.name, panel.cx(), (int16_t)(panel.y + 70 * s), 2);
            tftInstance->drawString("v" + currentAppMeta.version, panel.cx(), (int16_t)(panel.y + 90 * s), 2);
        } else {
            tftInstance->setTextColor(TFT_RED, TFT_DARKGREY);
            if (installNoMetadata) {
                tftInstance->drawString("No Metadata!", panel.cx(), (int16_t)(panel.y + 30 * s), 4);
                tftInstance->setTextColor(TFT_WHITE, TFT_DARKGREY);
                tftInstance->drawString("Folder missing app.json", panel.cx(), (int16_t)(panel.y + 65 * s), 2);
                tftInstance->drawString("Cannot install.", panel.cx(), (int16_t)(panel.y + 85 * s), 2);
            } else if (installApiError) {
                tftInstance->drawString("API Error!", panel.cx(), (int16_t)(panel.y + 30 * s), 4);
                tftInstance->setTextColor(TFT_WHITE, TFT_DARKGREY);
                tftInstance->drawString("App requires API: " + String(currentAppMeta.api), panel.cx(), (int16_t)(panel.y + 65 * s), 2);
                tftInstance->drawString("OS has API: " + String(KRYONOS_API_LEVEL), panel.cx(), (int16_t)(panel.y + 85 * s), 2);
                tftInstance->drawString("Update KryonOS!", panel.cx(), (int16_t)(panel.y + 110 * s), 2);
            } else if (installSyntaxError) {
                tftInstance->drawString("Syntax Error!", panel.cx(), (int16_t)(panel.y + 30 * s), 4);

                tftInstance->setTextColor(TFT_WHITE, TFT_DARKGREY);
                tftInstance->setTextDatum(TC_DATUM);
                // Font 1 has an 8px cell and a 10px line before the scale, and 30 characters was
                // 180px of the 220px panel — so both the pitch and the 30-character cut-off follow
                // the scale, and the line count is whatever still clears the OK button.
                const int16_t monoPitch = (int16_t)(10 * s);
                const int maxMonoChars  = 30 * s;
                int startIdx = 0;
                int yPos = (int16_t)(panel.y + 55 * s);
                int lineCount = 0;
                const int maxMonoLines = (int)((okBtn.y - yPos) / monoPitch);
                while (startIdx < (int)syntaxErrorMessage.length() && lineCount < 4 && lineCount < maxMonoLines) {
                    int nextNewline = syntaxErrorMessage.indexOf('\n', startIdx);
                    if (nextNewline == -1) nextNewline = syntaxErrorMessage.length();
                    String line = syntaxErrorMessage.substring(startIdx, nextNewline);
                    if ((int)line.length() > maxMonoChars) line = line.substring(0, maxMonoChars - 3) + "...";
                    tftInstance->drawString(line, panel.cx(), yPos, 1);
                    yPos += monoPitch;
                    startIdx = nextNewline + 1;
                    lineCount++;
                }
                tftInstance->setTextDatum(MC_DATUM);
            } else {
                tftInstance->drawString("Failed!", panel.cx(), (int16_t)(panel.y + 60 * s), 4);
            }
        }

        tftInstance->fillRoundRect(okBtn.x, okBtn.y, okBtn.w, okBtn.h, (int32_t)(4 * s), TFT_BLUE);
        tftInstance->setTextColor(TFT_WHITE, TFT_BLUE);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("OK", okBtn.cx(), okBtn.cy(), 2);
        return;
    } else if (installState == 3) { // App Info Dialog (before install)
        const UiRect panel  = m.dialogPanel((int16_t)(240 * s));
        const int16_t inset = (int16_t)(panel.x + 15 * s);   // 25 at 240x320
        const int16_t btnH  = (int16_t)(30 * s);
        const int16_t btnY  = (int16_t)(panel.bottom() - 40 * s);
        const UiRect installBtn = m.dialogButtonSpaced(btnY, btnH, 0, 2, (int16_t)(80 * s), (int16_t)(30 * s));
        const UiRect cancelBtn  = m.dialogButtonSpaced(btnY, btnH, 1, 2, (int16_t)(80 * s), (int16_t)(30 * s));

        tftInstance->fillRoundRect(panel.x, panel.y, panel.w, panel.h, (int32_t)(8 * s), TFT_DARKGREY);
        tftInstance->setTextColor(TFT_GREEN, TFT_DARKGREY);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString(currentAppMeta.name, panel.cx(), (int16_t)(panel.y + 25 * s), 4);

        tftInstance->setTextColor(TFT_WHITE, TFT_DARKGREY);
        tftInstance->setTextDatum(TL_DATUM);
        int y = (int16_t)(panel.y + 50 * s);
        tftInstance->drawString("Version: " + currentAppMeta.version, inset, y, 2); y += linePitch;
        tftInstance->drawString("Author:  " + currentAppMeta.author, inset, y, 2); y += linePitch;
        tftInstance->drawString("Type:    " + currentAppMeta.type, inset, y, 2); y += linePitch;
        tftInstance->drawString("Category: " + currentAppMeta.category, inset, y, 2); y += (int16_t)(20 * s);

        // Changelog or Description
        tftInstance->setTextColor(TFT_LIGHTGREY, TFT_DARKGREY);
        String desc = "";

        if (isUpdatingApp && currentAppMeta.changelog.length() > 0) {
            tftInstance->setTextColor(TFT_YELLOW, TFT_DARKGREY);
            tftInstance->drawString("What's New:", inset, y, 2); y += linePitch;
            tftInstance->setTextColor(TFT_LIGHTGREY, TFT_DARKGREY);
            desc = currentAppMeta.changelog;
        } else {
            desc = currentAppMeta.description;
        }

        if (desc.length() > 0) {
            // Three lines is what the panel had room for at 240x320; at a larger text scale the
            // same panel holds fewer, and asking for three would run them under the buttons. The
            // wrap width is a count of characters, so it comes from the panel width and the glyph
            // width together -- 25 at 240x320, where the historical 28 already ran past the edge.
            const int maxChars = (panel.w - 20 * s) / (8 * s);
            int maxLines = (btnY - y) / linePitch;
            if (maxLines > 3) maxLines = 3;
            int startIdx = 0;
            int lineCount = 0;
            while (startIdx < (int)desc.length() && lineCount < maxLines) {
                int endIdx = startIdx + maxChars;
                if (endIdx >= (int)desc.length()) endIdx = desc.length();
                else {
                    // Try to break at a space
                    int spaceIdx = desc.lastIndexOf(' ', endIdx);
                    if (spaceIdx > startIdx) endIdx = spaceIdx;
                }
                tftInstance->drawString(desc.substring(startIdx, endIdx), inset, y, 2);
                y += linePitch;
                startIdx = endIdx;
                if (startIdx < (int)desc.length() && desc[startIdx] == ' ') startIdx++;
                lineCount++;
            }
        }

        // Install and Cancel buttons
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->fillRoundRect(installBtn.x, installBtn.y, installBtn.w, installBtn.h, (int32_t)(4 * s), TFT_GREEN);
        tftInstance->setTextColor(TFT_BLACK, TFT_GREEN);
        tftInstance->drawString(isUpdatingApp ? "Update" : "Install",
                                installBtn.cx(), installBtn.cy(), 2);

        tftInstance->fillRoundRect(cancelBtn.x, cancelBtn.y, cancelBtn.w, cancelBtn.h, (int32_t)(4 * s), TFT_RED);
        tftInstance->setTextColor(TFT_WHITE, TFT_RED);
        tftInstance->drawString("Cancel", cancelBtn.cx(), cancelBtn.cy(), 2);
        return;
    } else if (installState == 5) { // Permission Review Dialog (Native C++)
        const UiRect panel = m.dialogPanel((int16_t)(240 * s));
        const int16_t btnH = (int16_t)(30 * s);
        const int16_t btnY = (int16_t)(panel.bottom() - 40 * s);
        const UiRect grantBtn = m.dialogButtonSpaced(btnY, btnH, 0, 2, (int16_t)(95 * s), (int16_t)(10 * s));
        const UiRect denyBtn  = m.dialogButtonSpaced(btnY, btnH, 1, 2, (int16_t)(95 * s), (int16_t)(10 * s));

        tftInstance->fillRoundRect(panel.x, panel.y, panel.w, panel.h, (int32_t)(8 * s), TFT_DARKGREY);
        tftInstance->setTextColor(TFT_GOLD, TFT_DARKGREY);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("App Permissions", panel.cx(), (int16_t)(panel.y + 20 * s), 4);

        tftInstance->setTextColor(TFT_WHITE, TFT_DARKGREY);
        tftInstance->setTextDatum(TL_DATUM);
        int y = (int16_t)(panel.y + 48 * s);
        tftInstance->drawString("Requires access to:", (int16_t)(panel.x + 10 * s), y, 2); y += (int16_t)(18 * s);

        // As many permission rows as clear the buttons, up to the five the list can hold.
        const int maxPermRows = (btnY - y) / (int16_t)(18 * s);

        for (size_t i = 0; i < currentAppMeta.permissions.size() && i < 5 && (int)i < maxPermRows; i++) {
            String p = currentAppMeta.permissions[i];
            String desc = "• " + p;
            if (p == "network") desc = "• Network & Cloud APIs";
            else if (p == "storage") desc = "• Local Sandbox Storage";
            else if (p == "gpio") desc = "• Hardware GPIO Pins";
            else if (p == "i2c") desc = "• Hardware I2C Master";
            else if (p == "pwm") desc = "• Hardware PWM & Audio";
            else if (p == "ai") desc = "• KryonAI Engine";

            tftInstance->setTextColor(TFT_CYAN, TFT_DARKGREY);
            tftInstance->drawString(desc, (int16_t)(panel.x + 12 * s), y, 2);
            y += (int16_t)(18 * s);
        }

        // Grant & Install / Deny buttons
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->fillRoundRect(grantBtn.x, grantBtn.y, grantBtn.w, grantBtn.h, (int32_t)(4 * s), TFT_GREEN);
        tftInstance->setTextColor(TFT_BLACK, TFT_GREEN);
        tftInstance->drawString("Grant", grantBtn.cx(), grantBtn.cy(), 2);

        tftInstance->fillRoundRect(denyBtn.x, denyBtn.y, denyBtn.w, denyBtn.h, (int32_t)(4 * s), TFT_RED);
        tftInstance->setTextColor(TFT_WHITE, TFT_RED);
        tftInstance->drawString("Deny", denyBtn.cx(), denyBtn.cy(), 2);
        return;
    }

    // Default Action Dialog (for regular files - non-app folders)
    const UiRect panel = m.dialogPanelTop((int16_t)(160 * s));
    const int16_t btnY = (int16_t)(panel.y + 80 * s);   // 120 at 240x320
    const int16_t btnH = (int16_t)(30 * s);
    const int16_t btnW = (int16_t)(60 * s);
    const int16_t btnGap = (int16_t)(10 * s);
    tftInstance->fillRoundRect(panel.x, panel.y, panel.w, panel.h, (int32_t)(8 * s), TFT_DARKGREY);
    tftInstance->setTextColor(TFT_WHITE, TFT_DARKGREY);
    tftInstance->setTextDatum(MC_DATUM);

    String filename = selectedFile.substring(selectedFile.lastIndexOf('/') + 1);
    tftInstance->drawString(filename, panel.cx(), (int16_t)(panel.y + 20 * s), 2);

    bool isJS = filename.endsWith(".js");

    // Run Button (only show if it's a JS file)
    if (isJS) {
        const UiRect runBtn = m.dialogButtonSpaced(btnY, btnH, 0, 3, btnW, btnGap);
        tftInstance->fillRoundRect(runBtn.x, runBtn.y, runBtn.w, runBtn.h, (int32_t)(4 * s), TFT_GREEN);
        tftInstance->setTextColor(TFT_BLACK, TFT_GREEN);
        tftInstance->drawString("Run", runBtn.cx(), runBtn.cy(), 2);
    }

    // Install Button
    const UiRect instBtn = m.dialogButtonSpaced(btnY, btnH, 1, 3, btnW, btnGap);
    tftInstance->fillRoundRect(instBtn.x, instBtn.y, instBtn.w, instBtn.h, (int32_t)(4 * s), TFT_BLUE);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLUE);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Install", instBtn.cx(), instBtn.cy(), 2);

    // Cancel Button
    const UiRect cancelBtn = m.dialogButtonSpaced(btnY, btnH, 2, 3, btnW, btnGap);
    tftInstance->fillRoundRect(cancelBtn.x, cancelBtn.y, cancelBtn.w, cancelBtn.h, (int32_t)(4 * s), TFT_RED);
    tftInstance->setTextColor(TFT_WHITE, TFT_RED);
    tftInstance->drawString("Cancel", cancelBtn.cx(), cancelBtn.cy(), 2);
}

// ============================================================
// Progress Animation
// ============================================================

static void installProgressCallback(int current, int total) {
    if (!progressTft) return;

    // Every position here was a 240x320 literal -- the 180x20 bar, the panel's 200 height, the
    // offsets that place the two readouts under it. They are all scaled from the metrics now, and
    // the bar itself is the shared progress track rather than a fourth copy of its arithmetic.
    const UiMetrics& m = M();
    const int16_t    s = (int16_t)m.scale;
    const UiRect     bar = m.progressBar;
    const UiRect     panel = m.dialogPanel((int16_t)(200 * s)); // (10,60,220,200) at scale 1

    int fillWidth = (current * bar.w) / total;

    // Draw progress bar outline (only first time)
    if (current == 1) {
        progressTft->fillRoundRect(panel.x, panel.y, panel.w, panel.h, (int32_t)(8 * s), TFT_DARKGREY);
        progressTft->setTextColor(TFT_GREEN, TFT_DARKGREY);
        progressTft->setTextDatum(MC_DATUM);
        progressTft->drawString("Installing...", m.centerX, (int16_t)(panel.y + 40 * s), 4);
        progressTft->drawRoundRect((int16_t)(bar.x - 2 * s), (int16_t)(bar.y - 2 * s),
                                   (int16_t)(bar.w + 4 * s), (int16_t)(bar.h + 4 * s),
                                   (int32_t)(3 * s), TFT_WHITE);
    }

    // Fill progress bar
    progressTft->fillRect(bar.x, bar.y, fillWidth, bar.h, TFT_GREEN);

    // Draw percentage text
    int pct = (current * 100) / total;
    progressTft->fillRect((int16_t)(m.centerX - 30 * s), (int16_t)(m.centerY + 30 * s),
                          (int16_t)(60 * s), (int16_t)(20 * s), TFT_DARKGREY);
    progressTft->setTextColor(TFT_WHITE, TFT_DARKGREY);
    progressTft->setTextDatum(MC_DATUM);
    progressTft->drawString(String(pct) + "%", m.centerX, (int16_t)(m.centerY + 40 * s), m.fontBody);

    // Draw file count
    progressTft->fillRect((int16_t)(m.centerX - 60 * s), (int16_t)(m.centerY + 50 * s),
                          (int16_t)(120 * s), (int16_t)(20 * s), TFT_DARKGREY);
    progressTft->drawString(String(current) + " / " + String(total) + " files", m.centerX,
                            (int16_t)(m.centerY + 60 * s), m.fontBody);

    // Flush each step. performInstall() copies and flashes between callbacks and does not return to
    // the main loop until the install is over, so nothing else presents this screen -- without this
    // the bar sat still and then the finished screen appeared all at once. See MyKeyboard::getString().
    progressTft->present();
}

void InstallerUI::drawInstallProgress(int current, int total) {
    installProgressCallback(current, total);
}

// ============================================================
// Install Logic
// ============================================================

void InstallerUI::performInstall(const String& srcFolder, const String& appName, bool overwrite) {
    bool defaultSD = FileSystem::exists("/local/config_install_sd.txt");
    if (defaultSD && !FileSystem::exists("/sd/")) defaultSD = false;
    
    String destBase = defaultSD ? "/sd/apps/" : "/local/apps/";
    String destFolder = destBase + appName + "/";
    
    if (overwrite) {
        // Delete existing app folder files first
        FileEntry existingFiles[50];
        int existingCount = FileSystem::listDirectory(destFolder.c_str(), existingFiles, 50);
        for (int i = 0; i < existingCount; i++) {
            if (!existingFiles[i].isDir) {
                FileSystem::deleteFile(existingFiles[i].path.c_str());
            }
        }
    }
    
    // Ensure apps directory exists
    FileSystem::mkdir(destBase.c_str());
    
    // Show installing screen
    tftInstance->fillScreen(TFT_BLACK);
    
    // Copy entire folder with progress callback
    installResultOk = FileSystem::copyDirectory(srcFolder.c_str(), destFolder.c_str(), installProgressCallback);
    
    // Cleanup AppStore temporary download
    if (srcFolder.indexOf("tmp_download") != -1) {
        FileSystem::deleteFile((srcFolder + "app.json").c_str());
        FileSystem::deleteFile((srcFolder + "main.js").c_str());
        FileSystem::rmdir(srcFolder.c_str());
    }
    
    needsRescan = true; // Refresh list after install
    LauncherUI::requestRescan(); // Tell KryonOS Home to refresh its cache
    
    delay(300); // Brief pause so user sees 100%
    
    installState = 2; // Show result
    drawActionDialog();
}

// ============================================================
// Help Screen
// ============================================================

void InstallerUI::drawHelp() {
    const UiMetrics& m = M();

    tftInstance->fillScreen(TFT_BLACK);
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);

    // Header Bar
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_GREEN);
    tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Installer Help", m.header.cx(), m.headerTextY, m.fontBody);

    // Help Text
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(TL_DATUM);
    // A heading is followed by a 2px gap; the lines within a block are one body cell apart. Both
    // follow the text scale, or at 2x the lines would sit on top of each other.
    const int16_t s = (int16_t)m.scale;
    const int16_t headPitch = (int16_t)(18 * s);
    const int16_t bodyPitch = (int16_t)(16 * s);
    int y = m.list.y;

    tftInstance->drawString("How to Install Apps:", m.list.x, y, m.fontBody); y += headPitch;
    tftInstance->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tftInstance->drawString("1. Put app folder on SD.", m.list.x, y, m.fontBody); y += bodyPitch;
    tftInstance->drawString("2. Folder needs app.json", m.list.x, y, m.fontBody); y += bodyPitch;
    tftInstance->drawString("   and main.js inside.", m.list.x, y, m.fontBody); y += bodyPitch;
    tftInstance->drawString("3. Tap [APP] to install.", m.list.x, y, m.fontBody); y += bodyPitch;
    tftInstance->drawString("4. App appears in Home.", m.list.x, y, m.fontBody); y += (int16_t)(20 * s);

    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->drawString("How to Update Apps:", m.list.x, y, m.fontBody); y += headPitch;
    tftInstance->setTextColor(TFT_LIGHTGREY, TFT_BLACK);
    tftInstance->drawString("1. Copy updated folder.", m.list.x, y, m.fontBody); y += bodyPitch;
    tftInstance->drawString("2. Install and overwrite.", m.list.x, y, m.fontBody);

    // Back Button Footer
    tftInstance->drawRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_WHITE);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("BACK", m.centerX, m.footerTextY, m.fontBody);
}

// ============================================================
// Touch Handling
// ============================================================

void InstallerUI::handleTouch(uint16_t x, uint16_t y) {
    const UiMetrics& m = M();

    if (currentPath == "/help/") {
        if (y >= m.footer.y) { // BACK button
            currentPath = "/";
            draw();
        }
        return;
    }

    if (showActionDialog) {
        String filename = selectedFile.substring(selectedFile.lastIndexOf('/') + 1);
        bool isJS = filename.endsWith(".js");

        // Every rect below is the one drawActionDialog painted, from the same numbers.
        const int16_t s = (int16_t)m.scale;

        if (installState == 1) { // Overwrite Prompt
            const UiRect panel  = m.dialogPanel((int16_t)(160 * s));
            const int16_t btnH  = (int16_t)(30 * s);
            const int16_t btnY  = (int16_t)(panel.y + 100 * s);
            const UiRect yesBtn = m.dialogButtonSpaced(btnY, btnH, 0, 2, (int16_t)(70 * s), (int16_t)(40 * s));
            const UiRect noBtn  = m.dialogButtonSpaced(btnY, btnH, 1, 2, (int16_t)(70 * s), (int16_t)(40 * s));
            if (yesBtn.contains((int16_t)x, (int16_t)y)) { // Yes - overwrite
                installSyntaxError = false;
                installApiError = false;
                installNoMetadata = false;
                performInstall(currentAppMeta.folderPath, currentAppMeta.packageName, true);
            } else if (noBtn.contains((int16_t)x, (int16_t)y)) { // No
                installState = 0;
                showActionDialog = false;
                tftInstance->fillScreen(TFT_BLACK);
                if (selectedFile.indexOf("tmp_download") != -1) {
                    FileSystem::deleteFile((selectedFile + "app.json").c_str());
                    FileSystem::deleteFile((selectedFile + "main.js").c_str());
                    FileSystem::rmdir(selectedFile.c_str());
                    extern int currentState;
                    currentState = 13;
                } else {
                    drawFileList();
                }
            }
            return;
        } else if (installState == 2) { // Result
            const UiRect panel = m.dialogPanel((int16_t)(200 * s));
            const UiRect okBtn = m.dialogButtonSpaced((int16_t)(panel.bottom() - 40 * s), (int16_t)(30 * s),
                                                      0, 1, (int16_t)(70 * s), 0);
            if (okBtn.contains((int16_t)x, (int16_t)y)) { // OK
                installState = 0;
                showActionDialog = false;
                tftInstance->fillScreen(TFT_BLACK);
                if (selectedFile.indexOf("tmp_download") != -1) {
                    extern int currentState;
                    currentState = 13;
                } else {
                    drawFileList();
                }
            }
            return;
        } else if (installState == 3) { // App Info dialog
            const UiRect panel = m.dialogPanel((int16_t)(240 * s));
            const int16_t btnH = (int16_t)(30 * s);
            const int16_t btnY = (int16_t)(panel.bottom() - 40 * s);
            const UiRect installBtn = m.dialogButtonSpaced(btnY, btnH, 0, 2, (int16_t)(80 * s), (int16_t)(30 * s));
            const UiRect cancelBtn  = m.dialogButtonSpaced(btnY, btnH, 1, 2, (int16_t)(80 * s), (int16_t)(30 * s));
            if (installBtn.contains((int16_t)x, (int16_t)y)) { // Install clicked
                bool defaultSD = FileSystem::exists("/local/config_install_sd.txt");
                if (defaultSD && !FileSystem::exists("/sd/")) defaultSD = false;
                String destBase = defaultSD ? "/sd/apps/" : "/local/apps/";
                String destFolder = destBase + currentAppMeta.packageName + "/";
                
                if (isUpdatingApp) {
                    installSyntaxError = false;
                    installApiError = false;
                    installNoMetadata = false;
                    performInstall(currentAppMeta.folderPath, currentAppMeta.packageName, true);
                } else if (FileSystem::exists(destFolder.c_str())) {
                    installState = 1; // Ask overwrite
                    drawActionDialog();
                } else {
                    performInstall(currentAppMeta.folderPath, currentAppMeta.packageName, false);
                }
            } else if (cancelBtn.contains((int16_t)x, (int16_t)y)) { // Cancel clicked
                installState = 0;
                showActionDialog = false;
                tftInstance->fillScreen(TFT_BLACK);
                if (selectedFile.indexOf("tmp_download") != -1) {
                    FileSystem::deleteFile((selectedFile + "app.json").c_str());
                    FileSystem::deleteFile((selectedFile + "main.js").c_str());
                    FileSystem::rmdir(selectedFile.c_str());
                    extern int currentState;
                    currentState = 13; // Return to App Store instead of staying in Installer
                } else {
                    drawFileList();
                }
            }
            return;
        } else if (installState == 5) { // Permission Review Dialog Touches
            const UiRect panel = m.dialogPanel((int16_t)(240 * s));
            const int16_t btnH = (int16_t)(30 * s);
            const int16_t btnY = (int16_t)(panel.bottom() - 40 * s);
            const UiRect grantBtn = m.dialogButtonSpaced(btnY, btnH, 0, 2, (int16_t)(95 * s), (int16_t)(10 * s));
            const UiRect denyBtn  = m.dialogButtonSpaced(btnY, btnH, 1, 2, (int16_t)(95 * s), (int16_t)(10 * s));
            if (grantBtn.contains((int16_t)x, (int16_t)y)) { // Grant clicked
                saveAppPermissions(currentAppMeta.packageName, currentAppMeta.permissions);

                bool defaultSD = FileSystem::exists("/local/config_install_sd.txt");
                if (defaultSD && !FileSystem::exists("/sd/")) defaultSD = false;
                String destBase = defaultSD ? "/sd/apps/" : "/local/apps/";
                String destFolder = destBase + currentAppMeta.packageName + "/";
                
                if (isUpdatingApp) {
                    installSyntaxError = false;
                    installApiError = false;
                    installNoMetadata = false;
                    performInstall(currentAppMeta.folderPath, currentAppMeta.packageName, true);
                } else if (FileSystem::exists(destFolder.c_str())) {
                    installState = 1; // Ask overwrite
                    drawActionDialog();
                } else {
                    performInstall(currentAppMeta.folderPath, currentAppMeta.packageName, false);
                }
            } else if (denyBtn.contains((int16_t)x, (int16_t)y)) { // Deny clicked
                installState = 0;
                showActionDialog = false;
                tftInstance->fillScreen(TFT_BLACK);
                if (selectedFile.indexOf("tmp_download") != -1) {
                    FileSystem::deleteFile((selectedFile + "app.json").c_str());
                    FileSystem::deleteFile((selectedFile + "main.js").c_str());
                    FileSystem::rmdir(selectedFile.c_str());
                    extern int currentState;
                    currentState = 13;
                } else {
                    drawFileList();
                }
            }
            return;
        }

        // Default Action Dialog Touches (for regular files)
        const UiRect filePanel = m.dialogPanelTop((int16_t)(160 * s));
        const int16_t fileBtnY = (int16_t)(filePanel.y + 80 * s);   // 120 at 240x320
        const int16_t fileBtnH = (int16_t)(30 * s);
        const int16_t fileBtnW = (int16_t)(60 * s);
        const int16_t fileBtnGap = (int16_t)(10 * s);
        const UiRect runBtn    = m.dialogButtonSpaced(fileBtnY, fileBtnH, 0, 3, fileBtnW, fileBtnGap);
        const UiRect instBtn   = m.dialogButtonSpaced(fileBtnY, fileBtnH, 1, 3, fileBtnW, fileBtnGap);
        const UiRect cancelBtn = m.dialogButtonSpaced(fileBtnY, fileBtnH, 2, 3, fileBtnW, fileBtnGap);

        // Run clicked (only if JS)
        if (isJS && runBtn.contains((int16_t)x, (int16_t)y)) {
            Serial.println("Running from SD: " + selectedFile);
            
            extern int currentState;
            currentState = 2; // STATE_RUN_APP
            showActionDialog = false;
            
            tftInstance->fillScreen(TFT_BLACK);
            tftInstance->setTextDatum(TL_DATUM);
            
            HarixKernel::runFile(selectedFile.c_str());

            const UiRect& ex = m.appExitButton;
            tftInstance->fillRoundRect(ex.x, ex.y, ex.w, ex.h, 5, TFT_RED);
            tftInstance->setTextColor(TFT_WHITE, TFT_RED);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("X", ex.cx(), ex.cy(), m.fontBody);
        }
        // Install clicked (legacy single-file install)
        else if (instBtn.contains((int16_t)x, (int16_t)y)) {
            bool defaultSD = FileSystem::exists("/local/config_install_sd.txt");
            if (defaultSD && !FileSystem::exists("/sd/")) defaultSD = false;
            String dest = defaultSD ? "/sd/apps/" + filename : "/local/apps/" + filename;
            String otherDest = defaultSD ? "/local/apps/" + filename : "/sd/apps/" + filename;
            
            if (FileSystem::exists(dest.c_str()) || FileSystem::exists(otherDest.c_str())) {
                installState = 1; // Overwrite prompt
                drawActionDialog();
            } else {
                installSyntaxError = false;
                installResultOk = true;
                if (isJS) {
                    String content = FileSystem::readTextFile(selectedFile.c_str());
                    extern String syntaxErrorMessage;
                    syntaxErrorMessage = HarixKernel::checkSyntax(content.c_str());
                    if (syntaxErrorMessage.length() > 0) {
                        installResultOk = false;
                        installSyntaxError = true;
                    }
                }
                if (installResultOk) {
                    if (defaultSD) FileSystem::mkdir("/sd/apps/");
                    installResultOk = FileSystem::copyFile(selectedFile.c_str(), dest.c_str());
                }
                installState = 2; // Result
                drawActionDialog();
            }
        }
        // Cancel clicked
        else if (cancelBtn.contains((int16_t)x, (int16_t)y)) {
            installState = 0;
            showActionDialog = false;
            tftInstance->fillScreen(TFT_BLACK);
            drawFileList();
        }
        return;
    }

    bool hasUp = (currentPath != "/");
    int totalItems = fileCount + (hasUp ? 1 : 0);

    // Helper lambda-like function to handle item selection
    auto selectItem = [&](int fileIdx) {
        if (files[fileIdx].isDir) {
            if (isAppPackage[fileIdx]) {
                // This is an app package - show app info dialog
                currentAppMeta = parseAppJson(files[fileIdx].path);
                
                installSyntaxError = false;
                installApiError = false;
                installNoMetadata = false;
                
                if (!currentAppMeta.valid) {
                    // No valid metadata
                    installNoMetadata = true;
                    installResultOk = false;
                    installState = 2;
                    showActionDialog = true;
                    drawActionDialog();
                    return;
                }
                
                // Check API level
                if (currentAppMeta.api > KRYONOS_API_LEVEL) {
                    installApiError = true;
                    installResultOk = false;
                    installState = 2;
                    showActionDialog = true;
                    drawActionDialog();
                    return;
                }
                
                // Check syntax of main.js
                String mainJsPath = currentAppMeta.folderPath;
                if (!mainJsPath.endsWith("/")) mainJsPath += "/";
                mainJsPath += "main.js";
                
                if (FileSystem::exists(mainJsPath.c_str())) {
                    String content = FileSystem::readTextFile(mainJsPath.c_str());
                    syntaxErrorMessage = HarixKernel::checkSyntax(content.c_str());
                    if (syntaxErrorMessage.length() > 0) {
                        installSyntaxError = true;
                        installResultOk = false;
                        installState = 2;
                        showActionDialog = true;
                        drawActionDialog();
                        return;
                    }
                }
                
                // Validate packageName
                String pkg = currentAppMeta.packageName;
                bool validPkg = true;
                if (pkg.length() == 0 || pkg.indexOf(' ') != -1 || pkg.indexOf('.') == -1) validPkg = false;
                for (int c = 0; c < pkg.length(); c++) {
                    if (isUpperCase(pkg[c])) validPkg = false;
                }
                
                if (!validPkg) {
                    installSyntaxError = true;
                    syntaxErrorMessage = "Invalid packageName!\nMust be lowercase,\nno spaces, dot-separated.";
                    installResultOk = false;
                    installState = 2;
                    showActionDialog = true;
                    drawActionDialog();
                    return;
                }

                // Check for updates and conflicts
                bool defaultSD = FileSystem::exists("/local/config_install_sd.txt");
                if (defaultSD && !FileSystem::exists("/sd/")) defaultSD = false;
                String destBase = defaultSD ? "/sd/apps/" : "/local/apps/";
                String destFolder = destBase + currentAppMeta.packageName + "/";
                
                isUpdatingApp = false;
                
                if (FileSystem::exists(destFolder.c_str())) {
                    String installedJsonPath = destFolder + "app.json";
                    if (FileSystem::exists(installedJsonPath.c_str())) {
                        String installedJsonContent = FileSystem::readTextFile(installedJsonPath.c_str());
                        String installedAuthor = FileSystem::parseJsonValue(installedJsonContent, "author");
                        String installedVersion = FileSystem::parseJsonValue(installedJsonContent, "version");
                        
                        if (installedAuthor != currentAppMeta.author) {
                            installSyntaxError = true;
                            syntaxErrorMessage = "Author conflict!\nInstalled: " + installedAuthor + "\nNew: " + currentAppMeta.author;
                            installResultOk = false;
                            installState = 2;
                            showActionDialog = true;
                            drawActionDialog();
                            return;
                        }
                        
                        if (isVersionGreater(currentAppMeta.version, installedVersion)) {
                            isUpdatingApp = true;
                        }
                    }
                }
                
                // Show app info dialog
                installState = 3;
                showActionDialog = true;
                drawActionDialog();
            } else {
                // Regular directory - navigate into it
                currentPath = files[fileIdx].path;
                if (!currentPath.endsWith("/")) currentPath += "/";
                selectedIndex = 0;
                scrollOffset = 0;
                draw();
            }
        } else {
            // Regular file
            selectedFile = files[fileIdx].path;
            showActionDialog = true;
            installState = 0;
            drawActionDialog();
        }
    };

    // Direct Touch Selection (Single Tap)
    const int rowIndex = m.listRowFromY((int16_t)y);
    if (rowIndex >= 0) {
        int clickedItem = scrollOffset + rowIndex;
        if (clickedItem < totalItems) {
            selectedIndex = clickedItem;

            if (hasUp && selectedIndex == 0) {
                int lastSlash = currentPath.lastIndexOf('/', currentPath.length() - 2);
                if (lastSlash >= 0) {
                    currentPath = currentPath.substring(0, lastSlash + 1);
                } else {
                    currentPath = "/";
                }
                selectedIndex = 0;
                scrollOffset = 0;
                draw();
            } else {
                int fileIdx = selectedIndex - (hasUp ? 1 : 0);
                selectItem(fileIdx);
            }
        }
        return;
    }

    // Footer Buttons: four zones split at quarter-widths (ESC / UP / SEL / DN).
    if (m.inFooter((int16_t)y)) {
        if (x < m.w / 4) { // ESC (BACK)
            currentState = 0;
            needsRescan = true;
            return;
        } else if (x < m.w / 2) { // UP
            if (selectedIndex > 0) {
                selectedIndex--;
                if (selectedIndex < scrollOffset) scrollOffset--;
                draw();
            }
        } else if (x < (int16_t)(3 * (m.w / 4))) { // SEL
            if (hasUp && selectedIndex == 0) {
                int lastSlash = currentPath.lastIndexOf('/', currentPath.length() - 2);
                if (lastSlash >= 0) {
                    currentPath = currentPath.substring(0, lastSlash + 1);
                } else {
                    currentPath = "/";
                }
                selectedIndex = 0;
                scrollOffset = 0;
                draw();
            } else {
                int fileIdx = selectedIndex - (hasUp ? 1 : 0);
                selectItem(fileIdx);
            }
        } else { // DN
            if (selectedIndex < totalItems - 1) {
                selectedIndex++;
                if (selectedIndex >= scrollOffset + m.itemsPerPage) scrollOffset++;
                draw();
            }
        }
        return;
    }

    // Quick jump back to launcher if pressing header
    if (y < m.header.bottom() + 4) {
        currentState = 0; // Back to launcher
        needsRescan = true;
        return;
    }
}
