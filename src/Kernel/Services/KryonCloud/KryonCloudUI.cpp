#include "KryonCloudUI.h"
#include "../../../FileSystem/FileSystem.h"
#include "../../Core/HarixKernel.h"
#include "../../../Keyboard/MyKeyboard.h"
#include "../../../UI/UiLayout.h"

// Current screen metrics. See Documentation/Display_Touch_Architecture.md.
static inline const UiMetrics& M() { return UiLayout::current(); }

extern int currentState;

KryonDisplay *KryonCloudUI::tftInstance = nullptr;
CloudUISubState KryonCloudUI::currentSubState = CLOUD_STATE_OVERVIEW;
int KryonCloudUI::selectedIndex = 0;
int KryonCloudUI::scrollOffset = 0;
unsigned long KryonCloudUI::lastWaitPollTime = 0;

String KryonCloudUI::activePairingCode = "";
String KryonCloudUI::activePairingId = "";
String KryonCloudUI::activeChallenge = "";
int KryonCloudUI::pairingExpiresIn = 1200;
bool KryonCloudUI::isWaitingForClaim = false;
unsigned long KryonCloudUI::pairingStartTime = 0;

std::vector<CloudFileItem> KryonCloudUI::cachedSharedFiles;
std::vector<CloudFileItem> KryonCloudUI::cachedDeviceFiles;
bool KryonCloudUI::manifestLoaded = false;
bool KryonCloudUI::storageScopeDevice = false;
String KryonCloudUI::storageStatusToast = "";
unsigned long KryonCloudUI::storageStatusToastTime = 0;

std::vector<BeamMessage> KryonCloudUI::cachedBeamMessages;
std::vector<BeamMessage> KryonCloudUI::cachedPublicMessages;
bool KryonCloudUI::beamInboxLoaded = false;
bool KryonCloudUI::beamPublicLoaded = false;
bool KryonCloudUI::beamPublicScope = false;
int KryonCloudUI::beamViewingIndex = -1;
bool KryonCloudUI::beamComposing = false;
String KryonCloudUI::beamTargetHandle = "";
int KryonCloudUI::beamMsgTypeIndex = 0;
String KryonCloudUI::beamContent = "Hello from KryonOS Node!";
int KryonCloudUI::beamPublicPage = 1;
String KryonCloudUI::beamStatusToast = "";
unsigned long KryonCloudUI::beamStatusToastTime = 0;

String KryonCloudUI::aiConsolePrompt = "Explain ESP32 dual-core FreeRTOS";
String KryonCloudUI::aiConsoleResponse = "Ready. Tap 'Ask Stream' or tap the prompt to edit.";
bool KryonCloudUI::aiStreamingActive = false;
int KryonCloudUI::aiRemainingTokens = 100;
int KryonCloudUI::aiScrollOffset = 0;
CloudBanStatus KryonCloudUI::cachedBanStatus;

// ============================================================================
// HIGH-CONTRAST VIBRANT PALETTE (ZERO DULL / MUDDY COLORS, ZERO CYAN)
// ============================================================================
#define CLOUD_BG          0x0000        // Pure deep obsidian black
#define CLOUD_NAV_BG      0x10A2        // Sleek elevated dark navy bar
#define CLOUD_CARD_BG     0x18E4        // Sleek modern elevated card background
#define CLOUD_CARD_BORDER 0x39E8        // Crisp visible border
#define CLOUD_ACCENT_BLUE 0x229F        // Vibrant Electric Royal Blue
#define CLOUD_PILL_INACT  0x2125        // Clean dark pill
#define CLOUD_TRACK_BG    0x2104        // Clean dark progress track background
#define CLOUD_BAR_AI      0xFDA0        // Vivid Amber / Gold for AI tokens
#define CLOUD_BAR_STR     0x07E0        // Vivid Neon Green for Storage (TFT_GREEN)
#define CLOUD_TEXT_MUTED  0xC618        // Clean readable silver

namespace {

// ---------------------------------------------------------------------------------------------
// Layout helpers. Every rect is derived from UiLayout metrics so a draw* and its matching
// handle*Touch walk the same rectangle, and so the cloud screens follow the panel like the rest of
// the OS. Each one reproduces its historical 240x320 coordinates exactly.
// ---------------------------------------------------------------------------------------------

// --- top nav: a title row over a 5-tab row ---
int16_t cloudTitleTop(const UiMetrics& m) { return (m.h < 240) ? 2 : 4; }               // 4
int16_t cloudTabTop(const UiMetrics& m)   { return (int16_t)(cloudTitleTop(m) + 26); }  // 30
int16_t cloudTabH()                       { return 22; }
int16_t cloudNavH(const UiMetrics& m)     { return (int16_t)(cloudTabTop(m) + cloudTabH() + 4); } // 56
int16_t cloudTitleY(const UiMetrics& m)   { return (int16_t)(cloudTitleTop(m) + 10); }  // 14

// The connection dot that heads the title row, and the title text that follows it.
int16_t cloudDotX(const UiMetrics& m)   { return (int16_t)(m.inset + 9); }               // 12
int16_t cloudTitleX(const UiMetrics& m) { return (int16_t)(cloudDotX(m) + 10); }         // 22

// The body area under the nav's hairline rule; every sub-screen repaints it before its own content.
UiRect cloudBody(const UiMetrics& m) {
    const int16_t top = (int16_t)(cloudNavH(m) + 1);
    return { 0, top, m.w, (int16_t)(m.h - top) };
}

// Where the cloud screens put their first card / row: 62 for cards, 60 for the button rows.
int16_t cloudTopY(const UiMetrics& m) { return (int16_t)(cloudBody(m).y + 5); }   // 62
int16_t cloudRowTop(const UiMetrics& m) { return (int16_t)(cloudBody(m).y + 3); } // 60

UiRect cloudExitButton(const UiMetrics& m) {
    return { (int16_t)(m.w - 32), cloudTitleTop(m), 26, 20 };                     // (208,4,26,20)
}

UiRect cloudTabRect(const UiMetrics& m, int index) {
    const int16_t gap  = 2;
    const int16_t tabW = (int16_t)((m.w - 2 * m.inset - 4 * gap) / 5);            // 45
    return { (int16_t)(m.inset + index * (tabW + gap)), cloudTabTop(m), tabW, cloudTabH() };
}

int cloudTabFromX(const UiMetrics& m, int16_t x) {
    const int16_t gap  = 2;
    const int16_t tabW = (int16_t)((m.w - 2 * m.inset - 4 * gap) / 5);
    const int idx = (x - m.inset) / (tabW + gap);
    return (idx >= 0 && idx < 5) ? idx : -1;
}

// --- cards ---
int16_t cloudCardX(const UiMetrics& m) { return (int16_t)(m.list.x - 2); }        // 8
int16_t cloudCardW(const UiMetrics& m) { return (int16_t)(m.list.w + 4); }        // 224

// A full-width card (or list row — the Beam and Storage rows are cards too).
UiRect cloudCard(const UiMetrics& m, int16_t y, int16_t h) {
    return { cloudCardX(m), y, cloudCardW(m), h };
}

// A modal panel: list width, not card width. (10,y,220,h).
UiRect cloudModal(const UiMetrics& m, int16_t y, int16_t h) {
    return { m.list.x, y, m.list.w, h };
}

// As cloudModal but inset by `pad` on each side, for the content boxes inside a panel.
UiRect cloudModalInset(const UiMetrics& m, int16_t y, int16_t h, int16_t pad) {
    return { (int16_t)(m.list.x + pad), y, (int16_t)(m.list.w - 2 * pad), h };
}

// `count` equal columns spanning the card width, separated by `gap`.
UiRect cloudSplit(const UiMetrics& m, int16_t y, int16_t h, int16_t gap, int index, int count) {
    const int16_t w = (int16_t)((cloudCardW(m) - gap * (count - 1)) / count);
    return { (int16_t)(cloudCardX(m) + index * (w + gap)), y, w, h };
}

// As cloudSplit but inset by `pad` on each side of the card.
UiRect cloudSplitInset(const UiMetrics& m, int16_t y, int16_t h, int16_t pad, int16_t gap,
                       int index, int count) {
    const int16_t total = (int16_t)(cloudCardW(m) - 2 * pad);
    const int16_t w = (int16_t)((total - gap * (count - 1)) / count);
    return { (int16_t)(cloudCardX(m) + pad + index * (w + gap)), y, w, h };
}

// As cloudSplitInset but measured against a modal panel rather than a card.
UiRect cloudModalSplit(const UiMetrics& m, int16_t y, int16_t h, int16_t pad, int16_t gap,
                       int index, int count) {
    const int16_t total = (int16_t)(m.list.w - 2 * pad);
    const int16_t w = (int16_t)((total - gap * (count - 1)) / count);
    return { (int16_t)(m.list.x + pad + index * (w + gap)), y, w, h };
}

// A quota progress track: 8px inset inside a card, with a 1px inner fill.
UiRect cloudTrack(const UiMetrics& m, int16_t y, int16_t h) {
    return cloudSplitInset(m, y, h, 8, 0, 0, 1);                                  // (16,y,208,h)
}
int16_t cloudTrackInnerW(const UiMetrics& m) { return (int16_t)(cloudTrack(m, 0, 0).w - 2); } // 206

// Unequal action row spanning the card width (the AI screen's Ask/Clear/UP/DN bar).
UiRect cloudWeighted(const UiMetrics& m, int16_t y, int16_t h, int16_t gap,
                     const int* weights, int count, int index) {
    int total = 0;
    for (int i = 0; i < count; i++) total += weights[i];
    if (total <= 0) return { 0, 0, 0, 0 };
    const int16_t avail = (int16_t)(cloudCardW(m) - gap * (count - 1));
    int16_t x = cloudCardX(m);
    for (int i = 0; i < count; i++) {
        const int16_t w = (int16_t)(avail * weights[i] / total);
        if (i == index) return { x, y, w, h };
        x = (int16_t)(x + w + gap);
    }
    return { 0, 0, 0, 0 };
}

// The #public stream's pager bar, pinned above the node-status line.
int16_t cloudPagerY(const UiMetrics& m) { return (int16_t)(m.footer.y - 33); }    // 252
UiRect cloudPagerButton(const UiMetrics& m, int index) {
    const int16_t w = 64;
    if (index == 0) return { cloudCardX(m), cloudPagerY(m), w, 22 };              // (8,252,64,22)
    return { (int16_t)(cloudCardX(m) + cloudCardW(m) - w), cloudPagerY(m), w, 22 }; // (168,252,64,22)
}

// The mailbox / manifest erase limit and the "checking..." line — both centred on the panel.
int16_t cloudListBottom(const UiMetrics& m) { return (int16_t)(m.footer.y - 25); } // 260

// Where a Beam / Storage list begins: four pixels under the toast when one is shown, otherwise
// two pixels below where the toast would have been. `toastDY` is the toast's offset from rowTop.
int16_t cloudListTop(const UiMetrics& m, int16_t toastDY, bool toastShown) {
    const UiRect toast = cloudCard(m, (int16_t)(cloudRowTop(m) + toastDY), 20);
    return toastShown ? (int16_t)(toast.bottom() + 4) : (int16_t)(toast.y + 2);
}

// --- the AI screen's card stack (prompt card, action bar, response window) ---
UiRect cloudAIPromptCard(const UiMetrics& m) { return cloudCard(m, cloudTopY(m), 40); }  // (8,62,224,40)
int16_t cloudAIButtonY(const UiMetrics& m) { return (int16_t)(cloudAIPromptCard(m).bottom() + 4); } // 106
UiRect cloudAIResponseCard(const UiMetrics& m) {
    return cloudCard(m, (int16_t)(cloudAIButtonY(m) + 26 + 4), 146);              // (8,136,224,146)
}
// The inner window the streaming callback repaints over and over.
UiRect cloudAIResponseWindow(const UiMetrics& m) {
    const UiRect c = cloudAIResponseCard(m);
    return { (int16_t)(c.x + 2), (int16_t)(c.y + 16), (int16_t)(c.w - 4), (int16_t)(c.h - 20) };
}
// Font-2 is a 16px cell, so the window scrolls once the response passes this many lines.
int cloudAiVisibleLines(const UiMetrics& m) {
    const UiRect w = cloudAIResponseWindow(m);
    return (w.h - 6) / 16;                                                        // 7
}

// A quota bar is green while there is plenty left, amber past three quarters used and red past
// nine tenths — the historical 154 / 185 thresholds on a 206px inner track.
uint16_t cloudBarColor(int width, int innerW) {
    if (width > innerW * 9 / 10) return TFT_RED;
    if (width > innerW * 3 / 4) return TFT_ORANGE;
    return 0x07E0;
}

} // namespace


// ============================================================================
// LOADING SCREEN WITH ANIMATED PROGRESS BAR
// ============================================================================
void KryonCloudUI::showLoadingScreen(const String& status, int progressPct) {
    if (!tftInstance) return;
    const UiMetrics& m = M();

    // The whole loading column is centred on the panel.
    const int16_t titleY  = (int16_t)(m.centerY - 75);
    const int16_t subY    = (int16_t)(m.centerY - 45);
    const UiRect  statusArea = { m.list.x, (int16_t)(m.centerY - 10), m.list.w, 24 };
    const int16_t statusY = statusArea.cy();
    const UiRect  track   = { (int16_t)(m.list.x + 10), (int16_t)(m.centerY + 24),
                              (int16_t)(m.list.w - 20), 16 };
    const UiRect  pctArea = { (int16_t)(m.centerX - 30), (int16_t)(m.centerY + 48), 60, 20 };

    if (progressPct <= 20) {
        tftInstance->fillScreen(CLOUD_BG);

        // Title
        tftInstance->setTextColor(TFT_WHITE, CLOUD_BG);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("KryonCloud", m.centerX, titleY, 4);

        tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_BG);
        tftInstance->drawString("Connecting to Platform...", m.centerX, subY, 2);
    }

    // Status Message Area
    tftInstance->fillRect(statusArea.x, statusArea.y, statusArea.w, statusArea.h, CLOUD_BG);
    tftInstance->setTextColor(TFT_YELLOW, CLOUD_BG);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString(status.c_str(), statusArea.cx(), statusY, 2);

    // Progress Bar Track
    tftInstance->drawRoundRect(track.x, track.y, track.w, track.h, 4, CLOUD_CARD_BORDER);
    const UiRect fill = { (int16_t)(track.x + 2), (int16_t)(track.y + 2),
                          (int16_t)(track.w - 4), (int16_t)(track.h - 4) };
    tftInstance->fillRect(fill.x, fill.y, fill.w, fill.h, CLOUD_TRACK_BG);

    int barW = (fill.w * progressPct) / 100;
    if (barW < 0) barW = 0;
    if (barW > fill.w) barW = fill.w;

    if (barW > 0) {
        tftInstance->fillRoundRect(fill.x, fill.y, barW, fill.h, 3, CLOUD_ACCENT_BLUE);
    }

    // Percentage
    tftInstance->fillRect(pctArea.x, pctArea.y, pctArea.w, pctArea.h, CLOUD_BG);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_BG);
    tftInstance->drawString((String(progressPct) + "%").c_str(), pctArea.cx(), pctArea.cy(), 2);

    // Flush here rather than at each of the twenty-odd call sites: every one of them draws a stage and
    // then sits on a network call, and this is the only thing that draws those stages. Without it the
    // cloud screens were whichever cache lines happened to be evicted while the request was in
    // flight. See the note in MyKeyboard::getString().
    tftInstance->present();
}

void KryonCloudUI::init(KryonDisplay *tft) {
    tftInstance = tft;
}

void KryonCloudUI::resetToHome() {
    selectedIndex = 0;
    scrollOffset = 0;
    manifestLoaded = false;
    beamInboxLoaded = false;
    beamViewingIndex = -1;
    beamComposing = false;
    aiStreamingActive = false;

    // Show immediate loading animation before blocking network sync
    showLoadingScreen("Connecting to KryonCloud...", 15);

    if (KryonCloudManager::isPaired()) {
        currentSubState = CLOUD_STATE_OVERVIEW;
        showLoadingScreen("Checking Account Status...", 25);
        KryonCloudManager::checkBanStatus(cachedBanStatus);
        if (cachedBanStatus.isBanned) {
            currentSubState = CLOUD_STATE_BAN_STATUS;
            showLoadingScreen("Device Suspended", 100);
            delay(200);
            draw();
            return;
        }

        showLoadingScreen("Syncing Profile...", 50);
        KryonCloudManager::syncHeartbeat();
        if (cachedBanStatus.isBanned) {
            currentSubState = CLOUD_STATE_BAN_STATUS;
            showLoadingScreen("Device Suspended", 100);
            delay(200);
            draw();
            return;
        }

        showLoadingScreen("Fetching Limits...", 80);
        KryonCloudManager::fetchAccountLimits();
        if (cachedBanStatus.isBanned) {
            currentSubState = CLOUD_STATE_BAN_STATUS;
            showLoadingScreen("Device Suspended", 100);
            delay(200);
            draw();
            return;
        }

        showLoadingScreen("Ready!", 100);
    } else {
        currentSubState = CLOUD_STATE_PAIRING;
        activePairingCode = "";
        isWaitingForClaim = false;
        showLoadingScreen("Opening Pairing Wizard...", 85);
        showLoadingScreen("Ready!", 100);
    }
}

void KryonCloudUI::draw() {
    if (!tftInstance) return;

    if (cachedBanStatus.isBanned) {
        currentSubState = CLOUD_STATE_BAN_STATUS;
        drawBanStatusScreen();
        return;
    }

    if (!KryonCloudManager::isPaired() && currentSubState != CLOUD_STATE_PAIRING) {
        currentSubState = CLOUD_STATE_PAIRING;
    }

    if (currentSubState == CLOUD_STATE_PAIRING) {
        drawPairingScreen();
        return;
    }

    // Top Navigation & Header for Paired Views
    drawTopNav();

    switch (currentSubState) {
        case CLOUD_STATE_OVERVIEW:
            drawOverviewScreen();
            break;
        case CLOUD_STATE_KRYON_AI:
            drawKryonAIScreen();
            break;
        case CLOUD_STATE_KRYON_BEAM:
            drawBeamScreen();
            break;
        case CLOUD_STATE_STORAGE:
            drawStorageScreen();
            break;
        case CLOUD_STATE_LIMITS:
            drawLimitsScreen();
            break;
        case CLOUD_STATE_BAN_STATUS:
            drawBanStatusScreen();
            break;
        default:
            break;
    }
}

// ============================================================================
// TOP NAVIGATION BAR (Dynamic title + 5 Subscreen Tabs)
// 1. Home (KryonCloud) 2. AI (KryonAI) 3. Beam (KryonBeam) 4. Drive (KryonDrive) 5. Limits (Usage Limits)
// ============================================================================
void KryonCloudUI::drawTopNav() {
    const UiMetrics& m = M();

    // Header Bar
    const int16_t navH   = cloudNavH(m);
    const int16_t titleY = cloudTitleY(m);
    tftInstance->fillRect(0, 0, m.w, navH, CLOUD_NAV_BG);
    tftInstance->drawFastHLine(0, navH, m.w, CLOUD_CARD_BORDER);

    // Title & Status Dot
    bool online = KryonCloudManager::isConnected();
    tftInstance->fillCircle(cloudDotX(m), titleY, 4, online ? TFT_GREEN : TFT_RED);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_NAV_BG);
    tftInstance->setTextDatum(ML_DATUM);

    // Dynamic Title per Subscreen
    const char* subTitles[] = {
        "KryonCloud",     // CLOUD_STATE_PAIRING
        "KryonCloud",     // CLOUD_STATE_OVERVIEW
        "KryonAI",        // CLOUD_STATE_KRYON_AI
        "KryonBeam",      // CLOUD_STATE_KRYON_BEAM
        "KryonDrive",     // CLOUD_STATE_STORAGE
        "Usage Limits",   // CLOUD_STATE_LIMITS
        "Account Status"  // CLOUD_STATE_BAN_STATUS
    };
    const char* activeTitle = subTitles[currentSubState];
    tftInstance->drawString(activeTitle, cloudTitleX(m), titleY, m.fontBody);

    // Top-Right Exit Button [X]
    const UiRect exitBtn = cloudExitButton(m);
    tftInstance->fillRoundRect(exitBtn.x, exitBtn.y, exitBtn.w, exitBtn.h, 4, 0x8800);
    tftInstance->setTextColor(TFT_WHITE, 0x8800);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("X", exitBtn.cx(), exitBtn.cy(), m.fontBody);

    // 5 Tab Buttons, spread across the frame with a 2px gap
    const char* tabNames[] = { "Home", "AI", "Beam", "Drive", "Limits" };
    CloudUISubState tabStates[] = {
        CLOUD_STATE_OVERVIEW,
        CLOUD_STATE_KRYON_AI,
        CLOUD_STATE_KRYON_BEAM,
        CLOUD_STATE_STORAGE,
        CLOUD_STATE_LIMITS
    };

    for (int i = 0; i < 5; i++) {
        const UiRect tab = cloudTabRect(m, i);
        bool active = (currentSubState == tabStates[i]);

        if (active) {
            tftInstance->fillRoundRect(tab.x, tab.y, tab.w, tab.h, 4, CLOUD_ACCENT_BLUE);
            tftInstance->setTextColor(TFT_WHITE, CLOUD_ACCENT_BLUE);
        } else {
            tftInstance->fillRoundRect(tab.x, tab.y, tab.w, tab.h, 4, CLOUD_PILL_INACT);
            tftInstance->drawRoundRect(tab.x, tab.y, tab.w, tab.h, 4, 0x31A7);
            tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_PILL_INACT);
        }
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString(tabNames[i], tab.cx(), tab.cy(), m.fontBody);
    }
}

void KryonCloudUI::handleTopNavTouch(uint16_t x, uint16_t y) {
    const UiMetrics& m = M();

    // Close button
    if (cloudExitButton(m).contains((int16_t)x, (int16_t)y)) {
        currentState = 0; // STATE_LAUNCHER
        return;
    }

    // Tab switching — the same zones drawTopNav draws the pills in.
    if (y >= cloudTabTop(m) && y < cloudTabTop(m) + cloudTabH()) {
        const int tabIdx = cloudTabFromX(m, (int16_t)x);
        if (tabIdx >= 0) {
            CloudUISubState tabStates[] = {
                CLOUD_STATE_OVERVIEW,
                CLOUD_STATE_KRYON_AI,
                CLOUD_STATE_KRYON_BEAM,
                CLOUD_STATE_STORAGE,
                CLOUD_STATE_LIMITS
            };
            if (currentSubState != tabStates[tabIdx]) {
                currentSubState = tabStates[tabIdx];
                beamViewingIndex = -1;
                beamComposing = false;
                draw();
            }
        }
    }
}

// ============================================================================
// 1. PAIRING WIZARD SCREEN
// ============================================================================
void KryonCloudUI::startPairingInit() {
    if (activePairingCode.length() > 0) return;
    const UiMetrics& m = M();

    const UiRect panel = m.dialogPanel(160);
    tftInstance->fillRect(panel.x, panel.y, panel.w, panel.h, CLOUD_BG);
    tftInstance->setTextColor(TFT_YELLOW, CLOUD_BG);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Contacting Cloud Hub...", m.centerX, (int16_t)(panel.cy() - 10), 2);
    tftInstance->present(); // every draw-then-request pair below flushes for this reason: the request
                            // blocks, and the main loop is inside the handler that started it

    bool ok = KryonCloudManager::initPairingSession(activePairingCode, activePairingId,
                                                    activeChallenge, pairingExpiresIn);
    if (ok) {
        isWaitingForClaim = true;
        pairingStartTime = millis();
        drawPairingScreen();
    } else {
        tftInstance->fillRect(panel.x, panel.y, panel.w, panel.h, CLOUD_BG);
        tftInstance->setTextColor(TFT_RED, CLOUD_BG);
        tftInstance->drawString("Pairing Init Failed", m.centerX, (int16_t)(panel.cy() - 20), 2);
        tftInstance->drawString("Check WiFi Connection", m.centerX, (int16_t)(panel.cy() + 10), 2);
    }
}

void KryonCloudUI::drawPairingScreen() {
    const UiMetrics& m = M();
    tftInstance->fillScreen(CLOUD_BG);

    // Header Frame
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, CLOUD_CARD_BORDER);
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, CLOUD_NAV_BG);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, CLOUD_ACCENT_BLUE);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_NAV_BG);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("KryonCloud Link", m.header.cx(), m.headerTextY, m.fontBody);

    if (activePairingCode.length() == 0) {
        startPairingInit();
        return;
    }

    // The wizard's text stack hangs off the header so it follows the panel.
    const int16_t top = m.header.bottom();
    tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_BG);
    tftInstance->setTextDatum(TC_DATUM);
    tftInstance->drawString("Open your browser & visit:", m.centerX, (int16_t)(top + 12), 2);

    tftInstance->setTextColor(TFT_YELLOW, CLOUD_BG);
    tftInstance->drawString("kryonos.harislab.tech", m.centerX, (int16_t)(top + 32), 2);

    tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_BG);
    tftInstance->drawString("Enter this Pairing Code:", m.centerX, (int16_t)(top + 59), 2);

    // Large Code Box
    const UiRect codeBox = cloudSplitInset(m, (int16_t)(top + 84), 55, 7, 0, 0, 1);
    tftInstance->fillRoundRect(codeBox.x, codeBox.y, codeBox.w, codeBox.h, 8, CLOUD_CARD_BG);
    tftInstance->drawRoundRect(codeBox.x, codeBox.y, codeBox.w, codeBox.h, 8, CLOUD_ACCENT_BLUE);
    tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
    tftInstance->setTextDatum(MC_DATUM);
    // +1: the 55px box has an odd centre, and the code sat one pixel below it.
    tftInstance->drawString(activePairingCode.c_str(), codeBox.cx(), (int16_t)(codeBox.cy() + 1), 4);

    tftInstance->setTextColor(TFT_GREEN, CLOUD_BG);
    tftInstance->drawString("Waiting for user claim...", m.centerX, (int16_t)(codeBox.bottom() + 20), 2);

    // Action Buttons
    const int16_t btnY = (int16_t)(m.footer.y - 45);
    const UiRect newCode = cloudSplitInset(m, btnY, 35, 7, 10, 0, 2);
    const UiRect cancel  = cloudSplitInset(m, btnY, 35, 7, 10, 1, 2);

    tftInstance->fillRoundRect(newCode.x, newCode.y, newCode.w, newCode.h, 5, CLOUD_PILL_INACT);
    tftInstance->drawRoundRect(newCode.x, newCode.y, newCode.w, newCode.h, 5, CLOUD_CARD_BORDER);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_PILL_INACT);
    tftInstance->drawString("NEW CODE", newCode.cx(), newCode.cy(), 2);

    tftInstance->fillRoundRect(cancel.x, cancel.y, cancel.w, cancel.h, 5, 0x9000);
    tftInstance->setTextColor(TFT_WHITE, 0x9000);
    tftInstance->drawString("CANCEL", cancel.cx(), cancel.cy(), 2);
}

void KryonCloudUI::handlePairingTouch(uint16_t x, uint16_t y) {
    const UiMetrics& m = M();
    const int16_t btnY = (int16_t)(m.footer.y - 45);
    const UiRect newCode = cloudSplitInset(m, btnY, 35, 7, 10, 0, 2);
    const UiRect cancel  = cloudSplitInset(m, btnY, 35, 7, 10, 1, 2);

    if (newCode.contains((int16_t)x, (int16_t)y)) {
        activePairingCode = "";
        startPairingInit();
    } else if (cancel.contains((int16_t)x, (int16_t)y)) {
        currentState = 0; // STATE_LAUNCHER
    }
}

// ============================================================================
// 2. OVERVIEW SCREEN (HOME)
// ============================================================================
void KryonCloudUI::drawOverviewScreen() {
    const UiMetrics& m = M();
    const UiRect body = cloudBody(m);
    tftInstance->fillRect(body.x, body.y, body.w, body.h, CLOUD_BG);

    // Account Profile Card
    const UiRect profile = cloudCard(m, cloudTopY(m), 72);
    tftInstance->fillRoundRect(profile.x, profile.y, profile.w, profile.h, 6, CLOUD_CARD_BG);
    tftInstance->drawRoundRect(profile.x, profile.y, profile.w, profile.h, 6, CLOUD_CARD_BORDER);

    const int16_t padX = (int16_t)(profile.x + 8);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_CARD_BG);
    tftInstance->setTextDatum(TL_DATUM);
    tftInstance->drawString(KryonCloudManager::getUserName().c_str(), padX, (int16_t)(profile.y + 6), 2);

    tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_CARD_BG);
    tftInstance->drawString(KryonCloudManager::getAccountEmail().c_str(), padX, (int16_t)(profile.y + 26), 2);

    tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
    tftInstance->drawString(("Handle: " + KryonCloudManager::getBeamHandle()).c_str(), padX,
                            (int16_t)(profile.y + 46), 2);

    // Live Quotas Quick Summary Card
    const CloudLimits& lim = KryonCloudManager::getLimits();
    const UiRect quota = cloudCard(m, (int16_t)(profile.bottom() + 6), 96);
    tftInstance->fillRoundRect(quota.x, quota.y, quota.w, quota.h, 6, CLOUD_CARD_BG);
    tftInstance->drawRoundRect(quota.x, quota.y, quota.w, quota.h, 6, CLOUD_CARD_BORDER);

    const int16_t innerW = cloudTrackInnerW(m);

    tftInstance->setTextColor(TFT_WHITE, CLOUD_CARD_BG);
    tftInstance->setTextDatum(TL_DATUM);
    tftInstance->drawString("Daily AI Quota:", padX, (int16_t)(quota.y + 6), 2);
    tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
    tftInstance->drawString((String(lim.dailyAiRemaining) + " left").c_str(),
                            (int16_t)(quota.x + 147), (int16_t)(quota.y + 6), 2);

    // AI Progress Bar (Green when plenty available, Orange/Red only when depleted)
    const UiRect aiTrack = cloudTrack(m, (int16_t)(quota.y + 26), 8);
    tftInstance->drawRoundRect(aiTrack.x, aiTrack.y, aiTrack.w, aiTrack.h, 3, CLOUD_CARD_BORDER);
    tftInstance->fillRect((int16_t)(aiTrack.x + 1), (int16_t)(aiTrack.y + 1), innerW,
                          (int16_t)(aiTrack.h - 2), CLOUD_TRACK_BG);
    int aiW = (lim.dailyAiLimit > 0) ? (lim.dailyAiUsed * innerW) / lim.dailyAiLimit : 0;
    if (aiW > innerW) aiW = innerW;
    if (aiW > 0) {
        tftInstance->fillRoundRect((int16_t)(aiTrack.x + 1), (int16_t)(aiTrack.y + 1), aiW,
                                   (int16_t)(aiTrack.h - 2), 2, cloudBarColor(aiW, innerW));
    }

    // Cloud Storage Summary
    tftInstance->setTextColor(TFT_WHITE, CLOUD_CARD_BG);
    tftInstance->drawString("Cloud Storage:", padX, (int16_t)(quota.y + 42), 2);
    tftInstance->setTextColor(TFT_GREEN, CLOUD_CARD_BG);
    tftInstance->drawString((String(lim.storageRemainingBytes / 1024) + " KB free").c_str(),
                            (int16_t)(quota.x + 127), (int16_t)(quota.y + 42), 2);

    // Storage Progress Bar
    const UiRect strTrack = cloudTrack(m, (int16_t)(quota.y + 62), 8);
    tftInstance->drawRoundRect(strTrack.x, strTrack.y, strTrack.w, strTrack.h, 3, CLOUD_CARD_BORDER);
    tftInstance->fillRect((int16_t)(strTrack.x + 1), (int16_t)(strTrack.y + 1), innerW,
                          (int16_t)(strTrack.h - 2), CLOUD_TRACK_BG);
    int strW = (lim.storageQuotaBytes > 0) ? (lim.storageUsedBytes * innerW) / lim.storageQuotaBytes : 0;
    if (strW > innerW) strW = innerW;
    if (strW > 0) {
        tftInstance->fillRoundRect((int16_t)(strTrack.x + 1), (int16_t)(strTrack.y + 1), strW,
                                   (int16_t)(strTrack.h - 2), 2, cloudBarColor(strW, innerW));
    }

    tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_CARD_BG);
    tftInstance->drawString("Node Status: Online & Verified", padX, (int16_t)(quota.y + 78), 1);

    // Action Buttons
    const int16_t rowY = (int16_t)(m.footer.y - 41);
    const UiRect sync   = cloudSplit(m, rowY, 34, 8, 0, 2);
    const UiRect unpair = cloudSplit(m, rowY, 34, 8, 1, 2);

    tftInstance->fillRoundRect(sync.x, sync.y, sync.w, sync.h, 5, CLOUD_ACCENT_BLUE);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_ACCENT_BLUE);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Sync Fresh", sync.cx(), sync.cy(), 2);

    tftInstance->fillRoundRect(unpair.x, unpair.y, unpair.w, unpair.h, 5, 0x9000);
    tftInstance->setTextColor(TFT_WHITE, 0x9000);
    tftInstance->drawString("Unpair Board", unpair.cx(), unpair.cy(), 2);
}

void KryonCloudUI::handleOverviewTouch(uint16_t x, uint16_t y) {
    const UiMetrics& m = M();
    if (y < cloudNavH(m)) {
        handleTopNavTouch(x, y);
        return;
    }

    const int16_t rowY = (int16_t)(m.footer.y - 41);
    const UiRect sync   = cloudSplit(m, rowY, 34, 8, 0, 2);
    const UiRect unpair = cloudSplit(m, rowY, 34, 8, 1, 2);

    if (sync.contains((int16_t)x, (int16_t)y)) {
        // Sync Fresh Data
        tftInstance->fillRect(sync.x, sync.y, sync.w, sync.h, 0x0215);
        tftInstance->setTextColor(TFT_YELLOW, 0x0215);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("Syncing...", sync.cx(), sync.cy(), 2);
        tftInstance->present();
        KryonCloudManager::syncAllFreshData();
        drawOverviewScreen();
    } else if (unpair.contains((int16_t)x, (int16_t)y)) {
        // Unpair Device
        KryonCloudManager::unpair();
        resetToHome();
        draw();
    }
}

// ============================================================================
// ============================================================================
// 3. KRYONAI SCREEN (Interactive Studio with Keyboard & Smooth Scrolling)
// ============================================================================
void KryonCloudUI::drawKryonAIScreen() {
    const UiMetrics& m = M();
    const UiRect body = cloudBody(m);
    tftInstance->fillRect(body.x, body.y, body.w, body.h, CLOUD_BG);

    // Prompt Card (Tap to edit)
    const UiRect prompt = cloudAIPromptCard(m);
    tftInstance->fillRoundRect(prompt.x, prompt.y, prompt.w, prompt.h, 5, CLOUD_CARD_BG);
    tftInstance->drawRoundRect(prompt.x, prompt.y, prompt.w, prompt.h, 5, CLOUD_ACCENT_BLUE);

    const int16_t padX = (int16_t)(prompt.x + 6);
    tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
    tftInstance->setTextDatum(TL_DATUM);
    tftInstance->drawString("Prompt (Tap to type):", padX, (int16_t)(prompt.y + 4), 1);

    tftInstance->setTextColor(TFT_WHITE, CLOUD_CARD_BG);
    String dispPrompt = aiConsolePrompt;
    if (dispPrompt.length() > 28) dispPrompt = dispPrompt.substring(0, 25) + "...";
    tftInstance->drawString(dispPrompt.c_str(), padX, (int16_t)(prompt.y + 18), 2);

    // Action Buttons — the four columns keep their historical 84/48/40/40 widths.
    static const int kAiBtnW[4] = { 21, 12, 10, 10 };
    const int16_t barY = cloudAIButtonY(m);
    const UiRect ask   = cloudWeighted(m, barY, 26, 4, kAiBtnW, 4, 0);
    const UiRect clear = cloudWeighted(m, barY, 26, 4, kAiBtnW, 4, 1);
    const UiRect up    = cloudWeighted(m, barY, 26, 4, kAiBtnW, 4, 2);
    const UiRect dn    = cloudWeighted(m, barY, 26, 4, kAiBtnW, 4, 3);

    tftInstance->fillRoundRect(ask.x, ask.y, ask.w, ask.h, 4, CLOUD_ACCENT_BLUE);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_ACCENT_BLUE);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Ask AI", ask.cx(), ask.cy(), 2);

    tftInstance->fillRoundRect(clear.x, clear.y, clear.w, clear.h, 4, 0x6000);
    tftInstance->setTextColor(TFT_WHITE, 0x6000);
    tftInstance->drawString("Clear", clear.cx(), clear.cy(), 2);

    tftInstance->fillRoundRect(up.x, up.y, up.w, up.h, 4, CLOUD_PILL_INACT);
    tftInstance->drawRoundRect(up.x, up.y, up.w, up.h, 4, CLOUD_CARD_BORDER);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_PILL_INACT);
    tftInstance->drawString("UP", up.cx(), up.cy(), 2);

    tftInstance->fillRoundRect(dn.x, dn.y, dn.w, dn.h, 4, CLOUD_PILL_INACT);
    tftInstance->drawRoundRect(dn.x, dn.y, dn.w, dn.h, 4, CLOUD_CARD_BORDER);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_PILL_INACT);
    tftInstance->drawString("DN", dn.cx(), dn.cy(), 2);

    // Response Window (Strictly bounded)
    const UiRect resp = cloudAIResponseCard(m);
    tftInstance->fillRoundRect(resp.x, resp.y, resp.w, resp.h, 5, CLOUD_CARD_BG);
    tftInstance->drawRoundRect(resp.x, resp.y, resp.w, resp.h, 5, CLOUD_CARD_BORDER);

    tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
    tftInstance->setTextDatum(TL_DATUM);
    tftInstance->drawString("AI Response:", padX, (int16_t)(resp.y + 4), 1);

    // Render bounded text with scrolling
    const UiRect win = cloudAIResponseWindow(m);
    drawWrappedText(aiConsoleResponse, (int16_t)(win.x + 4), (int16_t)(win.y + 2),
                    (int16_t)(win.w - 8), (int16_t)(win.h - 6), TFT_WHITE, 2, aiScrollOffset);

    // Footer info
    const CloudLimits& lim = KryonCloudManager::getLimits();
    tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_BG);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString(("Daily AI Remaining: " + String(lim.dailyAiRemaining) + " / " + String(lim.dailyAiLimit)).c_str(), m.centerX, (int16_t)(m.footer.y + 13), 1);
}

void KryonCloudUI::handleKryonAITouch(uint16_t x, uint16_t y) {
    const UiMetrics& m = M();
    if (y < cloudNavH(m)) {
        handleTopNavTouch(x, y);
        return;
    }

    // The response window's text box, its line cap and its clip — shared by the draw above and
    // every streaming callback below, so a repaint can never drift from what was drawn.
    const UiRect win = cloudAIResponseWindow(m);
    const int16_t textX = (int16_t)(win.x + 4);
    const int16_t textY = (int16_t)(win.y + 2);
    const int16_t textW = (int16_t)(win.w - 8);
    const int16_t textH = (int16_t)(win.h - 6);
    const int visLines  = cloudAiVisibleLines(m);

    auto repaint = [&](int scroll) {
        tftInstance->fillRect(win.x, win.y, win.w, win.h, CLOUD_CARD_BG);
        drawWrappedText(aiConsoleResponse, textX, textY, textW, textH, TFT_WHITE, 2, scroll);
        // Flush per token: the streaming callback runs this from inside KryonCloudAI::stream(), which
        // does not return until the answer is finished -- so this is the only chance the growing reply
        // gets to reach the panel before it is complete. See MyKeyboard::getString().
        tftInstance->present();
    };
    auto scrollBy = [&](int delta) {
        const int totalL = getTextLineCount(aiConsoleResponse, textW, 2);
        int want = aiScrollOffset + delta;
        if (want > totalL - visLines) want = totalL - visLines;
        if (want < 0) want = 0;
        aiScrollOffset = want;
        repaint(aiScrollOffset);
    };

    const UiRect prompt = cloudAIPromptCard(m);

    // Tap prompt box -> Open Keyboard
    if (prompt.contains((int16_t)x, (int16_t)y)) {
        String input = MyKeyboard::getString(aiConsolePrompt, "KryonAI Prompt:", 160);
        if (input.length() > 0) {
            aiConsolePrompt = input;
        }
        draw();
        return;
    }

    // Action Buttons
    static const int kAiBtnW[4] = { 21, 12, 10, 10 };
    const int16_t barY = cloudAIButtonY(m);
    const UiRect ask   = cloudWeighted(m, barY, 26, 4, kAiBtnW, 4, 0);
    const UiRect clear = cloudWeighted(m, barY, 26, 4, kAiBtnW, 4, 1);
    const UiRect up    = cloudWeighted(m, barY, 26, 4, kAiBtnW, 4, 2);
    const UiRect dn    = cloudWeighted(m, barY, 26, 4, kAiBtnW, 4, 3);

    if (ask.contains((int16_t)x, (int16_t)y)) {
        // Ask AI Button
        if (!KryonCloudManager::isConnected()) {
            aiConsoleResponse = "WiFi not connected! Connect in Settings.";
            drawKryonAIScreen();
            return;
        }
        if (!KryonCloudManager::isPaired()) {
            aiConsoleResponse = "Device not paired! Pair in Overview tab first.";
            drawKryonAIScreen();
            return;
        }

        aiConsoleResponse = "";
        aiScrollOffset = 0;
        tftInstance->fillRect(win.x, win.y, win.w, win.h, CLOUD_CARD_BG);
        tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
        tftInstance->setTextDatum(TL_DATUM);
        tftInstance->drawString("Connecting & Streaming...", textX, textY, 2);
        tftInstance->present();

        aiStreamingActive = true;
        KryonCloudAI::stream(
            aiConsolePrompt,
            [&](const String& token) {
                if (tftInstance) {
                    aiConsoleResponse += token;
                    const int totalL = getTextLineCount(aiConsoleResponse, textW, 2);
                    aiScrollOffset = (totalL > visLines) ? (totalL - visLines) : 0;
                    repaint(aiScrollOffset);
                }
            },
            [&](const String& fullText, const AiUsageStats& usage) {
                aiConsoleResponse = fullText;
                aiStreamingActive = false;
                const int totalL = getTextLineCount(aiConsoleResponse, textW, 2);
                aiScrollOffset = (totalL > visLines) ? (totalL - visLines) : 0;
                if (tftInstance) {
                    repaint(aiScrollOffset);
                }
            },
            [&](const String& err) {
                aiConsoleResponse = "Error: " + err;
                aiStreamingActive = false;
                if (tftInstance) {
                    repaint(0);
                }
            }
        );
        drawKryonAIScreen();
        return;
    } else if (clear.contains((int16_t)x, (int16_t)y)) {
        // Clear
        aiConsoleResponse = "Ready. Tap 'Ask AI' or tap the prompt to edit.";
        aiScrollOffset = 0;
        drawKryonAIScreen();
        return;
    } else if (up.contains((int16_t)x, (int16_t)y)) {
        // UP button
        if (aiScrollOffset > 0) scrollBy(-2);
        return;
    } else if (dn.contains((int16_t)x, (int16_t)y)) {
        // DN button
        if (aiScrollOffset < getTextLineCount(aiConsoleResponse, textW, 2) - visLines) scrollBy(2);
        return;
    }

    // Tap in the response window to scroll smoothly
    if (win.contains((int16_t)x, (int16_t)y)) {
        if (y < win.cy()) {
            // Tap top half -> Scroll UP
            if (aiScrollOffset > 0) scrollBy(-2);
        } else {
            // Tap bottom half -> Scroll DOWN
            if (aiScrollOffset < getTextLineCount(aiConsoleResponse, textW, 2) - visLines) scrollBy(2);
        }
    }
}

// ============================================================================
// 4. KRYONBEAM MESSENGER SCREEN (Direct Mailbox + Paginated #public Stream)
// ============================================================================
void KryonCloudUI::drawBeamScreen() {
    const UiMetrics& m = M();
    const UiRect body = cloudBody(m);
    tftInstance->fillRect(body.x, body.y, body.w, body.h, CLOUD_BG);

    if (beamViewingIndex >= 0) {
        drawBeamDetailModal();
        return;
    }

    if (beamComposing) {
        drawBeamComposeModal();
        return;
    }

    // Check toast expiration
    if (beamStatusToast.length() > 0 && millis() - beamStatusToastTime > 3500) {
        beamStatusToast = "";
    }

    const int16_t rowTop = cloudRowTop(m);

    // Row 1: Scope Switcher (Mailbox vs #public Stream)
    const UiRect mailbox  = cloudSplit(m, rowTop, 24, 4, 0, 2);
    const UiRect pubScope = cloudSplit(m, rowTop, 24, 4, 1, 2);

    tftInstance->fillRoundRect(mailbox.x, mailbox.y, mailbox.w, mailbox.h, 4,
                               (!beamPublicScope) ? CLOUD_ACCENT_BLUE : CLOUD_PILL_INACT);
    if (beamPublicScope) tftInstance->drawRoundRect(mailbox.x, mailbox.y, mailbox.w, mailbox.h, 4, CLOUD_CARD_BORDER);
    tftInstance->setTextColor(TFT_WHITE, (!beamPublicScope) ? CLOUD_ACCENT_BLUE : CLOUD_PILL_INACT);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Direct Mailbox", mailbox.cx(), mailbox.cy(), 2);

    tftInstance->fillRoundRect(pubScope.x, pubScope.y, pubScope.w, pubScope.h, 4,
                               beamPublicScope ? CLOUD_ACCENT_BLUE : CLOUD_PILL_INACT);
    if (!beamPublicScope) tftInstance->drawRoundRect(pubScope.x, pubScope.y, pubScope.w, pubScope.h, 4, CLOUD_CARD_BORDER);
    tftInstance->setTextColor(TFT_WHITE, beamPublicScope ? CLOUD_ACCENT_BLUE : CLOUD_PILL_INACT);
    tftInstance->drawString("#public Stream", pubScope.cx(), pubScope.cy(), 2);

    // Row 2: Action Buttons
    const UiRect compose = cloudSplit(m, (int16_t)(rowTop + 28), 22, 4, 0, 3);
    const UiRect refresh = cloudSplit(m, (int16_t)(rowTop + 28), 22, 4, 1, 3);
    const UiRect third   = cloudSplit(m, (int16_t)(rowTop + 28), 22, 4, 2, 3);

    tftInstance->fillRoundRect(compose.x, compose.y, compose.w, compose.h, 4, CLOUD_ACCENT_BLUE);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_ACCENT_BLUE);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Compose", compose.cx(), compose.cy(), 2);

    tftInstance->fillRoundRect(refresh.x, refresh.y, refresh.w, refresh.h, 4, CLOUD_PILL_INACT);
    tftInstance->drawRoundRect(refresh.x, refresh.y, refresh.w, refresh.h, 4, CLOUD_CARD_BORDER);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_PILL_INACT);
    tftInstance->drawString("Refresh", refresh.cx(), refresh.cy(), 2);

    const uint16_t thirdBg = beamPublicScope ? 0x03E0 : 0x6000;
    tftInstance->fillRoundRect(third.x, third.y, third.w, third.h, 4, thirdBg);
    tftInstance->setTextColor(TFT_WHITE, thirdBg);
    tftInstance->drawString(beamPublicScope ? "Broadcast" : "Clear All", third.cx(), third.cy(), 2);

    // Status Toast Banner (e.g. "Message Sent Successfully!")
    const bool hasToast = (beamStatusToast.length() > 0);
    const int16_t listY = cloudListTop(m, 52, hasToast);
    if (hasToast) {
        const bool isErr = beamStatusToast.indexOf("Failed") >= 0;
        const uint16_t toastBg = isErr ? TFT_RED : 0x03E0;
        const UiRect toast = cloudCard(m, (int16_t)(rowTop + 52), 20);
        tftInstance->fillRoundRect(toast.x, toast.y, toast.w, toast.h, 4, toastBg);
        tftInstance->setTextColor(TFT_WHITE, toastBg);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString(beamStatusToast.c_str(), m.centerX, toast.cy(), 2);
    }

    // Messages Area — the same layout serves both scopes, only the list and the badge differ.
    if (!beamPublicScope) {
        if (!beamInboxLoaded) {
            tftInstance->setTextColor(TFT_YELLOW, CLOUD_BG);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("Checking Mailbox...", m.centerX, (int16_t)(m.centerY + 20), 2);
            tftInstance->present();
            beamInboxLoaded = KryonCloudManager::pollBeamInbox(cachedBeamMessages, 2);
            tftInstance->fillRect(0, listY, m.w, (int16_t)(cloudListBottom(m) - listY), CLOUD_BG);
        }

        if (cachedBeamMessages.empty()) {
            tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_BG);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("No Direct Messages", m.centerX, (int16_t)(m.centerY + 15), 2);
            tftInstance->drawString("Tap 'Compose' to send @handle", m.centerX, (int16_t)(m.centerY + 38), 2);
        } else {
            int16_t y = listY;
            for (size_t i = 0; i < 3 && i < cachedBeamMessages.size(); i++) {
                const UiRect card = cloudCard(m, y, 40);
                tftInstance->fillRoundRect(card.x, card.y, card.w, card.h, 4, CLOUD_CARD_BG);
                tftInstance->drawRoundRect(card.x, card.y, card.w, card.h, 4, CLOUD_CARD_BORDER);

                tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
                tftInstance->setTextDatum(TL_DATUM);
                tftInstance->drawString(cachedBeamMessages[i].senderHandle.c_str(),
                                        (int16_t)(card.x + 6), (int16_t)(card.y + 4), 2);

                uint16_t badgeColor = TFT_WHITE;
                if (cachedBeamMessages[i].msgType == "ALERT") badgeColor = TFT_RED;
                else if (cachedBeamMessages[i].msgType == "TELEMETRY") badgeColor = TFT_GREEN;
                else if (cachedBeamMessages[i].msgType == "COMMAND") badgeColor = TFT_ORANGE;

                tftInstance->setTextColor(badgeColor, CLOUD_CARD_BG);
                tftInstance->drawString(("[" + cachedBeamMessages[i].msgType + "]").c_str(),
                                        (int16_t)(card.x + 142), (int16_t)(card.y + 4), 2);

                tftInstance->setTextColor(TFT_WHITE, CLOUD_CARD_BG);
                String snippet = cachedBeamMessages[i].content;
                if (snippet.length() > 25) snippet = snippet.substring(0, 22) + "...";
                tftInstance->drawString(snippet.c_str(), (int16_t)(card.x + 6), (int16_t)(card.y + 21), 2);

                y = (int16_t)(y + card.h + 4);
            }
        }
    } else {
        // #public Stream (Paginated in safe 3-message chunks)
        if (!beamPublicLoaded) {
            tftInstance->setTextColor(TFT_YELLOW, CLOUD_BG);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("Loading #public channel...", m.centerX, (int16_t)(m.centerY + 20), 2);
            tftInstance->present();
            beamPublicLoaded = KryonCloudManager::pollPublicBeamMessages(cachedPublicMessages, "public", 3, beamPublicPage);
            tftInstance->fillRect(0, listY, m.w, (int16_t)(cloudListBottom(m) - listY), CLOUD_BG);
        }

        if (cachedPublicMessages.empty()) {
            tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_BG);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("No Messages on this page", m.centerX, (int16_t)(m.centerY + 15), 2);
            tftInstance->drawString("Tap 'Broadcast' to post", m.centerX, (int16_t)(m.centerY + 38), 2);
        } else {
            int16_t y = listY;
            for (size_t i = 0; i < 3 && i < cachedPublicMessages.size(); i++) {
                const UiRect card = cloudCard(m, y, 40);
                tftInstance->fillRoundRect(card.x, card.y, card.w, card.h, 4, CLOUD_CARD_BG);
                tftInstance->drawRoundRect(card.x, card.y, card.w, card.h, 4, CLOUD_CARD_BORDER);

                tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
                tftInstance->setTextDatum(TL_DATUM);
                tftInstance->drawString(cachedPublicMessages[i].senderHandle.c_str(),
                                        (int16_t)(card.x + 6), (int16_t)(card.y + 4), 2);

                tftInstance->setTextColor(CLOUD_BAR_STR, CLOUD_CARD_BG);
                tftInstance->drawString("#public", (int16_t)(card.x + 152), (int16_t)(card.y + 4), 2);

                tftInstance->setTextColor(TFT_WHITE, CLOUD_CARD_BG);
                String snippet = cachedPublicMessages[i].content;
                if (snippet.length() > 25) snippet = snippet.substring(0, 22) + "...";
                tftInstance->drawString(snippet.c_str(), (int16_t)(card.x + 6), (int16_t)(card.y + 21), 2);

                y = (int16_t)(y + card.h + 4);
            }
        }

        // Pagination Bar
        const bool hasPrev = (beamPublicPage > 1);
        const bool hasNext = (cachedPublicMessages.size() == 3);
        const UiRect prev = cloudPagerButton(m, 0);
        const UiRect next = cloudPagerButton(m, 1);

        tftInstance->fillRoundRect(prev.x, prev.y, prev.w, prev.h, 3, hasPrev ? CLOUD_ACCENT_BLUE : CLOUD_PILL_INACT);
        if (!hasPrev) tftInstance->drawRoundRect(prev.x, prev.y, prev.w, prev.h, 3, CLOUD_CARD_BORDER);
        tftInstance->setTextColor(hasPrev ? TFT_WHITE : CLOUD_TEXT_MUTED, hasPrev ? CLOUD_ACCENT_BLUE : CLOUD_PILL_INACT);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("< Prev", prev.cx(), prev.cy(), 2);

        tftInstance->setTextColor(TFT_YELLOW, CLOUD_BG);
        tftInstance->drawString(("Page " + String(beamPublicPage)).c_str(), m.centerX, prev.cy(), 2);

        tftInstance->fillRoundRect(next.x, next.y, next.w, next.h, 3, hasNext ? CLOUD_ACCENT_BLUE : CLOUD_PILL_INACT);
        if (!hasNext) tftInstance->drawRoundRect(next.x, next.y, next.w, next.h, 3, CLOUD_CARD_BORDER);
        tftInstance->setTextColor(hasNext ? TFT_WHITE : CLOUD_TEXT_MUTED, hasNext ? CLOUD_ACCENT_BLUE : CLOUD_PILL_INACT);
        tftInstance->drawString("Next >", next.cx(), next.cy(), 2);
    }

    tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_BG);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString(("Node: " + KryonCloudManager::getBeamHandle() + (beamPublicScope ? " | #public" : " | Mailbox")).c_str(), m.centerX, (int16_t)(m.footer.y + 19), 1);
}

void KryonCloudUI::drawBeamDetailModal() {
    const UiMetrics& m = M();
    const auto& msgList = beamPublicScope ? cachedPublicMessages : cachedBeamMessages;
    if (beamViewingIndex < 0 || (size_t)beamViewingIndex >= msgList.size()) return;
    const auto& msg = msgList[beamViewingIndex];

    const UiRect modal = cloudModal(m, cloudRowTop(m), 235);
    tftInstance->fillRoundRect(modal.x, modal.y, modal.w, modal.h, 6, CLOUD_CARD_BG);
    tftInstance->drawRoundRect(modal.x, modal.y, modal.w, modal.h, 6, CLOUD_ACCENT_BLUE);

    tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
    tftInstance->setTextDatum(TL_DATUM);
    tftInstance->drawString(("From: " + msg.senderHandle).c_str(),
                            (int16_t)(modal.x + 8), (int16_t)(modal.y + 8), 2);

    tftInstance->setTextColor(TFT_WHITE, CLOUD_CARD_BG);
    tftInstance->drawString(beamPublicScope ? "Channel: #public" : ("Type: [" + msg.msgType + "]").c_str(),
                            (int16_t)(modal.x + 8), (int16_t)(modal.y + 28), 2);

    // Message Body (Safely bounded)
    const UiRect box = cloudModalInset(m, (int16_t)(modal.y + 50), 130, 6);
    tftInstance->fillRoundRect(box.x, box.y, box.w, box.h, 4, CLOUD_BG);
    tftInstance->drawRoundRect(box.x, box.y, box.w, box.h, 4, CLOUD_CARD_BORDER);
    drawWrappedText(msg.content, (int16_t)(box.x + 6), (int16_t)(box.y + 6),
                    (int16_t)(box.w - 12), (int16_t)(box.h - 12), TFT_WHITE, 2);

    // Close Button
    const UiRect close = cloudModalInset(m, (int16_t)(modal.y + 190), 34, 6);
    tftInstance->fillRoundRect(close.x, close.y, close.w, close.h, 4, CLOUD_ACCENT_BLUE);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_ACCENT_BLUE);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("CLOSE MESSAGE", close.cx(), close.cy(), 2);
}

void KryonCloudUI::drawBeamComposeModal() {
    const UiMetrics& m = M();
    const UiRect modal = cloudModal(m, cloudRowTop(m), 240);
    tftInstance->fillRoundRect(modal.x, modal.y, modal.w, modal.h, 6, CLOUD_CARD_BG);
    tftInstance->drawRoundRect(modal.x, modal.y, modal.w, modal.h, 6, CLOUD_ACCENT_BLUE);

    tftInstance->setTextColor(TFT_WHITE, CLOUD_CARD_BG);
    tftInstance->setTextDatum(TC_DATUM);
    tftInstance->drawString(beamPublicScope ? "Public Broadcast" : "Direct Message",
                            m.centerX, (int16_t)(modal.y + 8), 2);

    // Recipient Box
    tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_CARD_BG);
    tftInstance->setTextDatum(TL_DATUM);
    tftInstance->drawString(beamPublicScope ? "Channel:" : "Recipient (@handle):",
                            (int16_t)(modal.x + 6), (int16_t)(modal.y + 30), 1);

    const UiRect target = cloudModalInset(m, (int16_t)(modal.y + 44), 26, 6);
    tftInstance->fillRoundRect(target.x, target.y, target.w, target.h, 3, CLOUD_BG);
    tftInstance->drawRoundRect(target.x, target.y, target.w, target.h, 3, CLOUD_CARD_BORDER);
    tftInstance->setTextColor(TFT_YELLOW, CLOUD_BG);
    if (beamPublicScope) {
        tftInstance->drawString("#public (Global Channel)", (int16_t)(target.x + 6),
                                (int16_t)(target.y + 6), 2);
    } else {
        String dispTarget = (beamTargetHandle.length() > 0) ? beamTargetHandle : "Tap to enter @handle";
        tftInstance->drawString(dispTarget.c_str(), (int16_t)(target.x + 6), (int16_t)(target.y + 6), 2);
    }

    // Content Box (Taller and cleaner without type selector)
    tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_CARD_BG);
    tftInstance->drawString("Message Body (Tap to type):", (int16_t)(modal.x + 6),
                            (int16_t)(modal.y + 78), 1);

    const UiRect bodyBox = cloudModalInset(m, (int16_t)(modal.y + 92), 66, 6);
    tftInstance->fillRoundRect(bodyBox.x, bodyBox.y, bodyBox.w, bodyBox.h, 3, CLOUD_BG);
    tftInstance->drawRoundRect(bodyBox.x, bodyBox.y, bodyBox.w, bodyBox.h, 3, CLOUD_CARD_BORDER);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_BG);
    String dispBody = (beamContent.length() > 0) ? beamContent : "Tap to type message...";
    drawWrappedText(dispBody, (int16_t)(bodyBox.x + 6), (int16_t)(bodyBox.y + 6),
                    (int16_t)(bodyBox.w - 12), (int16_t)(bodyBox.h - 12), TFT_WHITE, 2);

    // Send & Cancel
    const UiRect send   = cloudModalSplit(m, (int16_t)(modal.y + 170), 32, 6, 12, 0, 2);
    const UiRect cancel = cloudModalSplit(m, (int16_t)(modal.y + 170), 32, 6, 12, 1, 2);

    tftInstance->fillRoundRect(send.x, send.y, send.w, send.h, 4, CLOUD_ACCENT_BLUE);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_ACCENT_BLUE);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("SEND", send.cx(), send.cy(), 2);

    tftInstance->fillRoundRect(cancel.x, cancel.y, cancel.w, cancel.h, 4, 0x6000);
    tftInstance->setTextColor(TFT_WHITE, 0x6000);
    tftInstance->drawString("CANCEL", cancel.cx(), cancel.cy(), 2);
}

void KryonCloudUI::handleBeamTouch(uint16_t x, uint16_t y) {
    const UiMetrics& m = M();
    if (y < cloudNavH(m)) {
        handleTopNavTouch(x, y);
        return;
    }

    const int16_t rowTop = cloudRowTop(m);

    if (beamViewingIndex >= 0) {
        const UiRect modal = cloudModal(m, rowTop, 235);
        const UiRect close = cloudModalInset(m, (int16_t)(modal.y + 190), 34, 6);
        if (close.contains((int16_t)x, (int16_t)y)) {
            beamViewingIndex = -1;
            drawBeamScreen();
        }
        return;
    }

    if (beamComposing) {
        const UiRect modal   = cloudModal(m, rowTop, 240);
        const UiRect target  = cloudModalInset(m, (int16_t)(modal.y + 44), 26, 6);
        const UiRect bodyBox = cloudModalInset(m, (int16_t)(modal.y + 92), 66, 6);
        const UiRect send    = cloudModalSplit(m, (int16_t)(modal.y + 170), 32, 6, 12, 0, 2);
        const UiRect cancel  = cloudModalSplit(m, (int16_t)(modal.y + 170), 32, 6, 12, 1, 2);

        // Target handle tap (Direct messaging only)
        if (target.contains((int16_t)x, (int16_t)y) && !beamPublicScope) {
            String res = MyKeyboard::getString(beamTargetHandle, "Recipient (@handle):", 32);
            drawTopNav();
            const UiRect body = cloudBody(m);
            tftInstance->fillRect(body.x, body.y, body.w, body.h, CLOUD_BG);
            if (res.length() > 0) beamTargetHandle = res;
            drawBeamComposeModal();
            return;
        }

        // Body tap
        if (bodyBox.contains((int16_t)x, (int16_t)y)) {
            String res = MyKeyboard::getString(beamContent, "Message Body:", 120);
            drawTopNav();
            const UiRect body = cloudBody(m);
            tftInstance->fillRect(body.x, body.y, body.w, body.h, CLOUD_BG);
            if (res.length() > 0) beamContent = res;
            drawBeamComposeModal();
            return;
        }

        // Action Buttons (Send / Cancel)
        if (send.contains((int16_t)x, (int16_t)y)) {
            // Send Message - always TEXT
            bool ok = false;
            if (beamPublicScope || beamTargetHandle == "#public" || beamTargetHandle.equalsIgnoreCase("public")) {
                ok = KryonCloudManager::broadcastPublicBeam(beamContent, "public");
                beamStatusToast = ok ? "Broadcast Posted to #public!" : "Broadcast Failed!";
            } else {
                String target2 = (beamTargetHandle.length() > 0) ? beamTargetHandle : "@node";
                ok = KryonCloudManager::sendBeamMessage(target2, beamContent, "TEXT");
                beamStatusToast = ok ? "Message Sent to " + target2 + "!" : "Message Send Failed!";
            }
            beamStatusToastTime = millis();
            beamComposing = false;
            beamInboxLoaded = false;
            beamPublicLoaded = false;
            drawBeamScreen();
        } else if (cancel.contains((int16_t)x, (int16_t)y)) {
            beamComposing = false;
            drawBeamScreen();
        }
        return;
    }

    // Row 1: Scope Switcher touches
    const UiRect mailbox  = cloudSplit(m, rowTop, 24, 4, 0, 2);
    const UiRect pubScope = cloudSplit(m, rowTop, 24, 4, 1, 2);
    if (mailbox.contains((int16_t)x, (int16_t)y)) {
        if (beamPublicScope) {
            beamPublicScope = false;
            beamInboxLoaded = false;
            drawBeamScreen();
        }
        return;
    } else if (pubScope.contains((int16_t)x, (int16_t)y)) {
        if (!beamPublicScope) {
            beamPublicScope = true;
            beamPublicLoaded = false;
            beamPublicPage = 1;
            drawBeamScreen();
        }
        return;
    }

    // Row 2: Action Bar Buttons
    const UiRect compose = cloudSplit(m, (int16_t)(rowTop + 28), 22, 4, 0, 3);
    const UiRect refresh = cloudSplit(m, (int16_t)(rowTop + 28), 22, 4, 1, 3);
    const UiRect third   = cloudSplit(m, (int16_t)(rowTop + 28), 22, 4, 2, 3);

    if (compose.contains((int16_t)x, (int16_t)y)) {
        // Compose
        beamComposing = true;
        if (beamPublicScope) {
            beamTargetHandle = "#public";
        } else if (beamTargetHandle == "#public" || beamTargetHandle == "*") {
            beamTargetHandle = "";
        }
        drawBeamComposeModal();
        return;
    } else if (refresh.contains((int16_t)x, (int16_t)y)) {
        // Refresh
        if (beamPublicScope) beamPublicLoaded = false;
        else beamInboxLoaded = false;
        drawBeamScreen();
        return;
    } else if (third.contains((int16_t)x, (int16_t)y)) {
        if (!beamPublicScope) {
            // Clear All / Ack
            std::vector<String> ackIds;
            for (const auto& msg : cachedBeamMessages) ackIds.push_back(msg.id);
            if (!ackIds.empty()) {
                KryonCloudManager::acknowledgeBeamMessages(ackIds);
                cachedBeamMessages.clear();
            }
            drawBeamScreen();
        } else {
            // Broadcast directly to #public
            beamTargetHandle = "#public";
            beamComposing = true;
            drawBeamComposeModal();
        }
        return;
    }

    // Pagination Row touches for #public Stream
    if (beamPublicScope) {
        const UiRect prev = cloudPagerButton(m, 0);
        const UiRect next = cloudPagerButton(m, 1);
        if (prev.contains((int16_t)x, (int16_t)y)) {
            if (beamPublicPage > 1) {
                beamPublicPage--;
                beamPublicLoaded = false;
                drawBeamScreen();
            }
            return;
        } else if (next.contains((int16_t)x, (int16_t)y)) {
            if (cachedPublicMessages.size() == 3) {
                beamPublicPage++;
                beamPublicLoaded = false;
                drawBeamScreen();
            }
            return;
        }
    }

    // Tap on a message card
    const int16_t startY = cloudListTop(m, 52, beamStatusToast.length() > 0);
    const int16_t limitY = cloudListBottom(m);
    if (y >= startY && y <= limitY) {
        const int idx = (y - startY) / 44;
        const auto& list = beamPublicScope ? cachedPublicMessages : cachedBeamMessages;
        if (idx >= 0 && (size_t)idx < list.size() && idx < 3) {
            beamViewingIndex = idx;
            drawBeamDetailModal();
        }
    }
}

// ============================================================================
// 5. CLOUD STORAGE EXPLORER SCREEN (With Device Config Backup & Restore)
// ============================================================================
void KryonCloudUI::drawStorageScreen() {
    const UiMetrics& m = M();
    const UiRect body = cloudBody(m);
    tftInstance->fillRect(body.x, body.y, body.w, body.h, CLOUD_BG);

    // Check toast expiration
    if (storageStatusToast.length() > 0 && millis() - storageStatusToastTime > 3500) {
        storageStatusToast = "";
    }

    const int16_t rowTop = cloudRowTop(m);

    // Row 1: Scope Switcher (Shared vs Device)
    const UiRect shared = cloudSplit(m, rowTop, 22, 4, 0, 2);
    const UiRect device = cloudSplit(m, rowTop, 22, 4, 1, 2);

    tftInstance->fillRoundRect(shared.x, shared.y, shared.w, shared.h, 4, storageScopeDevice ? CLOUD_PILL_INACT : CLOUD_ACCENT_BLUE);
    if (storageScopeDevice) tftInstance->drawRoundRect(shared.x, shared.y, shared.w, shared.h, 4, CLOUD_CARD_BORDER);
    tftInstance->setTextColor(TFT_WHITE, storageScopeDevice ? CLOUD_PILL_INACT : CLOUD_ACCENT_BLUE);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Shared (/cloud)", shared.cx(), shared.cy(), 2);

    tftInstance->fillRoundRect(device.x, device.y, device.w, device.h, 4, storageScopeDevice ? CLOUD_ACCENT_BLUE : CLOUD_PILL_INACT);
    if (!storageScopeDevice) tftInstance->drawRoundRect(device.x, device.y, device.w, device.h, 4, CLOUD_CARD_BORDER);
    tftInstance->setTextColor(TFT_WHITE, storageScopeDevice ? CLOUD_ACCENT_BLUE : CLOUD_PILL_INACT);
    tftInstance->drawString("Device (/device)", device.cx(), device.cy(), 2);

    // Row 2: Backup & Restore Action Buttons
    const UiRect backup  = cloudSplit(m, (int16_t)(rowTop + 26), 22, 4, 0, 2);
    const UiRect restore = cloudSplit(m, (int16_t)(rowTop + 26), 22, 4, 1, 2);

    tftInstance->fillRoundRect(backup.x, backup.y, backup.w, backup.h, 4, CLOUD_ACCENT_BLUE);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_ACCENT_BLUE);
    tftInstance->drawString("Create Backup", backup.cx(), backup.cy(), 2);

    tftInstance->fillRoundRect(restore.x, restore.y, restore.w, restore.h, 4, 0x03E0);
    tftInstance->setTextColor(TFT_WHITE, 0x03E0);
    tftInstance->drawString("Restore Backup", restore.cx(), restore.cy(), 2);

    // Status Toast Banner
    const bool hasToast = (storageStatusToast.length() > 0);
    const int16_t listY = cloudListTop(m, 50, hasToast);
    if (hasToast) {
        const bool isErr = (storageStatusToast.indexOf("Failed") >= 0 || storageStatusToast.indexOf("No ") >= 0);
        const uint16_t toastBg = isErr ? TFT_RED : 0x03E0;
        const UiRect toast = cloudCard(m, (int16_t)(rowTop + 50), 20);
        tftInstance->fillRoundRect(toast.x, toast.y, toast.w, toast.h, 4, toastBg);
        tftInstance->setTextColor(TFT_WHITE, toastBg);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString(storageStatusToast.c_str(), m.centerX, toast.cy(), 2);
    }

    if (!manifestLoaded) {
        tftInstance->setTextColor(TFT_YELLOW, CLOUD_BG);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("Fetching Manifest...", m.centerX, (int16_t)(m.centerY + 20), 2);
        tftInstance->present();
        manifestLoaded = KryonCloudManager::fetchStorageManifest(cachedSharedFiles, cachedDeviceFiles);
        tftInstance->fillRect(0, listY, m.w, (int16_t)(cloudListBottom(m) - listY), CLOUD_BG);
    }

    const auto& list = storageScopeDevice ? cachedDeviceFiles : cachedSharedFiles;

    if (list.empty()) {
        tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_BG);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("No Files in this scope", m.centerX, (int16_t)(m.centerY + 10), 2);
        tftInstance->drawString(storageScopeDevice ? "Tap 'Create Backup' to sync" : "Upload via Web Server", m.centerX, (int16_t)(m.centerY + 35), 2);
    } else {
        int16_t y = listY;
        for (size_t i = 0; i < 4 && i < list.size(); i++) {
            const UiRect card = cloudCard(m, y, 34);
            tftInstance->fillRoundRect(card.x, card.y, card.w, card.h, 4, CLOUD_CARD_BG);
            tftInstance->drawRoundRect(card.x, card.y, card.w, card.h, 4, CLOUD_CARD_BORDER);

            tftInstance->setTextColor(TFT_WHITE, CLOUD_CARD_BG);
            tftInstance->setTextDatum(TL_DATUM);
            String fn = list[i].filename;
            if (fn.length() > 20) fn = fn.substring(0, 17) + "...";
            tftInstance->drawString(fn.c_str(), (int16_t)(card.x + 6), (int16_t)(card.y + 8), 2);

            tftInstance->setTextColor(TFT_GREEN, CLOUD_CARD_BG);
            tftInstance->drawString((String(list[i].fileSize / 1024) + " KB").c_str(),
                                    (int16_t)(card.x + 162), (int16_t)(card.y + 8), 2);

            y = (int16_t)(y + card.h + 4);
        }
    }

    // Storage status footer
    const CloudLimits& lim = KryonCloudManager::getLimits();
    tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_BG);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString(("Storage: " + String(lim.storageUsedBytes / 1024) + " KB / " + String(lim.storageQuotaMb) + " MB").c_str(), m.centerX, (int16_t)(m.footer.y + 17), 1);
}

void KryonCloudUI::handleStorageTouch(uint16_t x, uint16_t y) {
    const UiMetrics& m = M();
    if (y < cloudNavH(m)) {
        handleTopNavTouch(x, y);
        return;
    }

    const int16_t rowTop = cloudRowTop(m);

    // Scope Switcher
    const UiRect shared = cloudSplit(m, rowTop, 22, 4, 0, 2);
    const UiRect device = cloudSplit(m, rowTop, 22, 4, 1, 2);
    if (shared.contains((int16_t)x, (int16_t)y)) {
        if (storageScopeDevice) {
            storageScopeDevice = false;
            drawStorageScreen();
        }
        return;
    } else if (device.contains((int16_t)x, (int16_t)y)) {
        if (!storageScopeDevice) {
            storageScopeDevice = true;
            drawStorageScreen();
        }
        return;
    }

    // Action Buttons (Create Backup & Restore Backup)
    const UiRect backup  = cloudSplit(m, (int16_t)(rowTop + 26), 22, 4, 0, 2);
    const UiRect restore = cloudSplit(m, (int16_t)(rowTop + 26), 22, 4, 1, 2);
    if (backup.contains((int16_t)x, (int16_t)y) || restore.contains((int16_t)x, (int16_t)y)) {
        // A progress card sits over the list while the transfer runs, then the screen redraws.
        const UiRect prog = cloudSplitInset(m, 130, 65, 8, 0, 0, 1);
        tftInstance->fillRoundRect(prog.x, prog.y, prog.w, prog.h, 5, CLOUD_CARD_BG);
        tftInstance->drawRoundRect(prog.x, prog.y, prog.w, prog.h, 5, CLOUD_ACCENT_BLUE);
        tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
        tftInstance->setTextDatum(MC_DATUM);

        bool ok = false;
        if (backup.contains((int16_t)x, (int16_t)y)) {
            tftInstance->drawString("Backing Up Configs...", m.centerX, prog.cy(), 2);
            tftInstance->present();
            ok = KryonCloudManager::createDeviceBackup();
            storageStatusToast = ok ? "Backup Saved to Cloud!" : "Backup Failed / No Files";
            storageStatusToastTime = millis();
            manifestLoaded = false;
        } else {
            tftInstance->drawString("Restoring Backup...", m.centerX, prog.cy(), 2);
            tftInstance->present();
            ok = KryonCloudManager::restoreDeviceBackup();
            storageStatusToast = ok ? "Backup Restored to Disk!" : "No Backup Found / Error";
            storageStatusToastTime = millis();
        }
        drawStorageScreen();
        return;
    }

    // File item click -> Download
    const int16_t startY = cloudListTop(m, 50, storageStatusToast.length() > 0);
    if (y >= startY && y <= cloudListBottom(m)) {
        const int idx = (y - startY) / 38;
        const auto& list = storageScopeDevice ? cachedDeviceFiles : cachedSharedFiles;
        if (idx >= 0 && idx < (int)list.size()) {
            const UiRect card = cloudCard(m, (int16_t)(startY + idx * 38), 34);
            tftInstance->fillRect(card.x, card.y, card.w, card.h, CLOUD_ACCENT_BLUE);
            tftInstance->setTextColor(TFT_WHITE, CLOUD_ACCENT_BLUE);
            tftInstance->setTextDatum(MC_DATUM);
            tftInstance->drawString("Downloading...", card.cx(), card.cy(), 2);
            tftInstance->present();

            String localDest = "/local/cloud_" + list[idx].filename;
            KryonCloudManager::downloadCloudFile(list[idx].path, localDest, list[idx].scope);
            drawStorageScreen();
        }
    }
}

// ============================================================================
// 6. USAGE & QUOTAS SCREEN
// ============================================================================
void KryonCloudUI::drawLimitsScreen() {
    const UiMetrics& m = M();
    const UiRect body = cloudBody(m);
    tftInstance->fillRect(body.x, body.y, body.w, body.h, CLOUD_BG);

    const CloudLimits& lim = KryonCloudManager::getLimits();
    const int16_t innerW = cloudTrackInnerW(m);

    const UiRect aiCard = cloudCard(m, cloudTopY(m), 68);
    const UiRect strCard = cloudCard(m, (int16_t)(aiCard.bottom() + 6), 68);
    const UiRect rstCard = cloudCard(m, (int16_t)(strCard.bottom() + 6), 44);
    const UiRect refresh = cloudCard(m, (int16_t)(rstCard.bottom() + 6), 32);

    // AI Quota Card
    tftInstance->fillRoundRect(aiCard.x, aiCard.y, aiCard.w, aiCard.h, 5, CLOUD_CARD_BG);
    tftInstance->drawRoundRect(aiCard.x, aiCard.y, aiCard.w, aiCard.h, 5, CLOUD_CARD_BORDER);

    tftInstance->setTextColor(TFT_WHITE, CLOUD_CARD_BG);
    tftInstance->setTextDatum(TL_DATUM);
    tftInstance->drawString("Daily AI Limit", (int16_t)(aiCard.x + 8), (int16_t)(aiCard.y + 6), 2);

    tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
    tftInstance->drawString((String(lim.dailyAiUsed) + " / " + String(lim.dailyAiLimit) + " used").c_str(),
                            m.centerX, (int16_t)(aiCard.y + 6), 2);

    int aiPct = (lim.dailyAiLimit > 0) ? (lim.dailyAiUsed * innerW) / lim.dailyAiLimit : 0;
    if (aiPct > innerW) aiPct = innerW;

    const UiRect aiTrack = cloudTrack(m, (int16_t)(aiCard.y + 30), 10);
    tftInstance->drawRoundRect(aiTrack.x, aiTrack.y, aiTrack.w, aiTrack.h, 3, CLOUD_CARD_BORDER);
    tftInstance->fillRect((int16_t)(aiTrack.x + 1), (int16_t)(aiTrack.y + 1), innerW,
                          (int16_t)(aiTrack.h - 2), CLOUD_TRACK_BG);
    if (aiPct > 0) {
        tftInstance->fillRoundRect((int16_t)(aiTrack.x + 1), (int16_t)(aiTrack.y + 1), aiPct,
                                   (int16_t)(aiTrack.h - 2), 2, cloudBarColor(aiPct, innerW));
    }

    tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_CARD_BG);
    tftInstance->drawString(("Remaining Today: " + String(lim.dailyAiRemaining) + " requests").c_str(),
                            (int16_t)(aiCard.x + 8), (int16_t)(aiCard.y + 48), 1);

    // Storage Quota Card
    tftInstance->fillRoundRect(strCard.x, strCard.y, strCard.w, strCard.h, 5, CLOUD_CARD_BG);
    tftInstance->drawRoundRect(strCard.x, strCard.y, strCard.w, strCard.h, 5, CLOUD_CARD_BORDER);

    tftInstance->setTextColor(TFT_WHITE, CLOUD_CARD_BG);
    tftInstance->drawString("Cloud Storage", (int16_t)(strCard.x + 8), (int16_t)(strCard.y + 6), 2);

    tftInstance->setTextColor(TFT_GREEN, CLOUD_CARD_BG);
    tftInstance->drawString((String(lim.storageUsedBytes / 1024) + " KB / " + String(lim.storageQuotaMb) + " MB").c_str(),
                            (int16_t)(m.centerX - 10), (int16_t)(strCard.y + 6), 2);

    int strPct = (lim.storageQuotaBytes > 0) ? (lim.storageUsedBytes * innerW) / lim.storageQuotaBytes : 0;
    if (strPct > innerW) strPct = innerW;

    const UiRect strTrack = cloudTrack(m, (int16_t)(strCard.y + 30), 10);
    tftInstance->drawRoundRect(strTrack.x, strTrack.y, strTrack.w, strTrack.h, 3, CLOUD_CARD_BORDER);
    tftInstance->fillRect((int16_t)(strTrack.x + 1), (int16_t)(strTrack.y + 1), innerW,
                          (int16_t)(strTrack.h - 2), CLOUD_TRACK_BG);
    if (strPct > 0) {
        tftInstance->fillRoundRect((int16_t)(strTrack.x + 1), (int16_t)(strTrack.y + 1), strPct,
                                   (int16_t)(strTrack.h - 2), 2, cloudBarColor(strPct, innerW));
    }

    tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_CARD_BG);
    tftInstance->drawString(("Free Space: " + String(lim.storageRemainingBytes / 1024) + " KB remaining").c_str(),
                            (int16_t)(strCard.x + 8), (int16_t)(strCard.y + 48), 1);

    // Reset Schedule & Health Card
    tftInstance->fillRoundRect(rstCard.x, rstCard.y, rstCard.w, rstCard.h, 5, CLOUD_CARD_BG);
    tftInstance->drawRoundRect(rstCard.x, rstCard.y, rstCard.w, rstCard.h, 5, CLOUD_CARD_BORDER);

    tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
    tftInstance->drawString("Quota Reset:", (int16_t)(rstCard.x + 8), (int16_t)(rstCard.y + 6), 1);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_CARD_BG);
    tftInstance->drawString("Daily at 00:00 UTC", (int16_t)(m.centerX - 30), (int16_t)(rstCard.y + 6), 1);

    tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
    tftInstance->drawString("Account Tier:", (int16_t)(rstCard.x + 8), (int16_t)(rstCard.y + 24), 1);
    tftInstance->setTextColor(TFT_GREEN, CLOUD_CARD_BG);
    tftInstance->drawString("Verified Hardware Node", (int16_t)(m.centerX - 30), (int16_t)(rstCard.y + 24), 1);

    // Refresh Quota Button
    tftInstance->fillRoundRect(refresh.x, refresh.y, refresh.w, refresh.h, 5, CLOUD_ACCENT_BLUE);
    tftInstance->setTextColor(TFT_WHITE, CLOUD_ACCENT_BLUE);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("REFRESH QUOTAS", refresh.cx(), refresh.cy(), 2);

    tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_BG);
    tftInstance->drawString("KryonCloud Account Gateway", m.centerX, (int16_t)(m.footer.y + 19), 1);
}

void KryonCloudUI::handleLimitsTouch(uint16_t x, uint16_t y) {
    const UiMetrics& m = M();
    if (y < cloudNavH(m)) {
        handleTopNavTouch(x, y);
        return;
    }

    const UiRect aiCard = cloudCard(m, cloudTopY(m), 68);
    const UiRect strCard = cloudCard(m, (int16_t)(aiCard.bottom() + 6), 68);
    const UiRect rstCard = cloudCard(m, (int16_t)(strCard.bottom() + 6), 44);
    const UiRect refresh = cloudCard(m, (int16_t)(rstCard.bottom() + 6), 32);

    if (refresh.contains((int16_t)x, (int16_t)y)) {
        KryonCloudManager::fetchAccountLimits();
        drawLimitsScreen();
    }
}

// ============================================================================
// 7. BAN / APPEAL STATUS SCREEN
// ============================================================================
void KryonCloudUI::drawBanStatusScreen() {
    const UiMetrics& m = M();
    tftInstance->fillScreen(CLOUD_BG);

    // Frame & Header
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, CLOUD_CARD_BORDER);
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, CLOUD_NAV_BG);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, cachedBanStatus.isBanned ? TFT_RED : CLOUD_ACCENT_BLUE);
    tftInstance->setTextColor(cachedBanStatus.isBanned ? TFT_RED : TFT_WHITE, CLOUD_NAV_BG);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString(cachedBanStatus.isBanned ? "Account / Device Ban Notice" : "Security & Ban Status", m.header.cx(), m.headerTextY, m.fontBody);

    if (cachedBanStatus.isBanned) {
        const bool isDeviceBan = cachedBanStatus.deviceBanned && !cachedBanStatus.accountBanned;

        // Card container
        const UiRect card = cloudModal(m, (int16_t)(m.header.bottom() + 6), 226);
        tftInstance->fillRoundRect(card.x, card.y, card.w, card.h, 6, CLOUD_CARD_BG);
        tftInstance->drawRoundRect(card.x, card.y, card.w, card.h, 6, TFT_RED);

        tftInstance->setTextColor(TFT_RED, CLOUD_CARD_BG);
        tftInstance->setTextDatum(TC_DATUM);
        tftInstance->drawString(isDeviceBan ? "DEVICE SUSPENDED" : "ACCOUNT SUSPENDED", m.centerX, (int16_t)(card.y + 6), 2);

        // Ban Reason
        tftInstance->setTextColor(TFT_YELLOW, CLOUD_CARD_BG);
        tftInstance->setTextDatum(TL_DATUM);
        tftInstance->drawString("Reason:", (int16_t)(card.x + 6), (int16_t)(card.y + 30), 1);
        String reasonStr = (cachedBanStatus.banReason.length() > 0) ? cachedBanStatus.banReason : "Terms of Service Violation";
        drawWrappedText(reasonStr, (int16_t)(card.x + 6), (int16_t)(card.y + 44),
                        (int16_t)(card.w - 12), 32, TFT_WHITE, 1);

        // Instruction Box
        const UiRect box = cloudModalInset(m, (int16_t)(card.y + 80), 140, 4);
        tftInstance->fillRoundRect(box.x, box.y, box.w, box.h, 4, CLOUD_BG);
        tftInstance->drawRoundRect(box.x, box.y, box.w, box.h, 4, CLOUD_CARD_BORDER);

        const int16_t tx = (int16_t)(box.x + 4);
        tftInstance->setTextColor(TFT_YELLOW, CLOUD_BG);
        tftInstance->setTextDatum(TL_DATUM);
        tftInstance->drawString("To submit an appeal:", tx, (int16_t)(box.y + 6), 1);

        tftInstance->setTextColor(TFT_WHITE, CLOUD_BG);
        tftInstance->drawString("Please Login to your Kryon Account here:", tx, (int16_t)(box.y + 22), 1);

        tftInstance->setTextColor(CLOUD_BAR_STR, CLOUD_BG);
        tftInstance->drawString("https://kryonos.harislab.tech/login", tx, (int16_t)(box.y + 38), 1);

        tftInstance->setTextColor(TFT_WHITE, CLOUD_BG);
        if (isDeviceBan) {
            tftInstance->drawString("go to connected device,", tx, (int16_t)(box.y + 58), 1);
            tftInstance->drawString("select this device and", tx, (int16_t)(box.y + 72), 1);
            tftInstance->drawString("submit appeal.", tx, (int16_t)(box.y + 86), 1);
        } else {
            tftInstance->drawString("go to your account and", tx, (int16_t)(box.y + 58), 1);
            tftInstance->drawString("then submit appeal.", tx, (int16_t)(box.y + 72), 1);
        }

        tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_BG);
        tftInstance->drawString(("Appeal Status: " + (cachedBanStatus.appealStatus.length() > 0 ? cachedBanStatus.appealStatus : "NONE")).c_str(), tx, (int16_t)(box.y + 118), 1);

        // Exit button
        const UiRect exit = cloudModal(m, (int16_t)(m.footer.y - 11), 36);
        tftInstance->fillRoundRect(exit.x, exit.y, exit.w, exit.h, 5, CLOUD_ACCENT_BLUE);
        tftInstance->setTextColor(TFT_WHITE, CLOUD_ACCENT_BLUE);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("EXIT TO LAUNCHER", exit.cx(), exit.cy(), 2);
    } else {
        tftInstance->setTextColor(TFT_GREEN, CLOUD_BG);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("STATUS: ACTIVE / GOOD", m.centerX, (int16_t)(m.centerY - 60), 2);
        tftInstance->setTextColor(CLOUD_TEXT_MUTED, CLOUD_BG);
        tftInstance->drawString("No suspensions or bans.", m.centerX, (int16_t)(m.centerY - 30), 1);

        const UiRect back = cloudSplitInset(m, (int16_t)(m.footer.y - 45), 38, 12, 0, 0, 1);
        tftInstance->fillRoundRect(back.x, back.y, back.w, back.h, 5, CLOUD_ACCENT_BLUE);
        tftInstance->setTextColor(TFT_WHITE, CLOUD_ACCENT_BLUE);
        tftInstance->setTextDatum(MC_DATUM);
        tftInstance->drawString("< BACK TO OVERVIEW", back.cx(), back.cy(), 2);
    }
}

void KryonCloudUI::handleBanStatusTouch(uint16_t x, uint16_t y) {
    const UiMetrics& m = M();
    if (cachedBanStatus.isBanned) {
        const UiRect exit = cloudModal(m, (int16_t)(m.footer.y - 11), 36);
        if (exit.contains((int16_t)x, (int16_t)y)) {
            currentState = 0; // STATE_LAUNCHER
        }
    } else {
        const UiRect back = cloudSplitInset(m, (int16_t)(m.footer.y - 45), 38, 12, 0, 0, 1);
        if (back.contains((int16_t)x, (int16_t)y)) {
            currentSubState = CLOUD_STATE_OVERVIEW;
            draw();
        }
    }
}

// ============================================================================
// TEXT WRAPPING, SCROLLING & SCREEN BOUNDS PROTECTION HELPERS
// ============================================================================
int KryonCloudUI::getTextLineCount(const String& text, int maxW, uint8_t font) {
    if (text.length() == 0) return 0;
    int charWidth = (font == 1) ? 6 : 8;
    int maxCharsPerLine = maxW / charWidth;
    int count = 0;
    int currentLineLen = 0;
    String word = "";

    for (size_t i = 0; i <= text.length(); i++) {
        char c = (i < text.length()) ? text[i] : ' ';

        if (c == '\n' || c == ' ' || i == text.length()) {
            if (currentLineLen + word.length() > maxCharsPerLine) {
                count++;
                currentLineLen = 0;
            }
            if (currentLineLen > 0) currentLineLen++;
            currentLineLen += word.length();
            word = "";

            if (c == '\n') {
                count++;
                currentLineLen = 0;
            }
        } else {
            word += c;
        }
    }
    if (currentLineLen > 0) count++;
    return count;
}

void KryonCloudUI::drawWrappedText(const String& text, int x, int y, int maxW, int maxH, uint16_t color, uint8_t font, int scrollLine) {
    if (!tftInstance || text.length() == 0) return;

    tftInstance->setTextColor(color);
    tftInstance->setTextDatum(TL_DATUM);

    int lineHeight = (font == 1) ? 10 : 16;
    int charWidth = (font == 1) ? 6 : 8;
    int maxCharsPerLine = maxW / charWidth;
    int maxVisibleLines = maxH / lineHeight;

    std::vector<String> lines;
    String currentLine = "";
    String word = "";

    for (size_t i = 0; i <= text.length(); i++) {
        char c = (i < text.length()) ? text[i] : ' ';

        if (c == '\n' || c == ' ' || i == text.length()) {
            if (currentLine.length() + word.length() > maxCharsPerLine) {
                lines.push_back(currentLine);
                currentLine = "";
            }
            if (currentLine.length() > 0) currentLine += " ";
            currentLine += word;
            word = "";

            if (c == '\n') {
                lines.push_back(currentLine);
                currentLine = "";
            }
        } else {
            word += c;
        }
    }
    if (currentLine.length() > 0) {
        lines.push_back(currentLine);
    }

    if (lines.empty()) return;

    int totalLines = lines.size();
    if (scrollLine < 0) scrollLine = 0;
    if (scrollLine > totalLines - maxVisibleLines && totalLines > maxVisibleLines) {
        scrollLine = totalLines - maxVisibleLines;
    } else if (totalLines <= maxVisibleLines) {
        scrollLine = 0;
    }

    int curY = y;
    for (int i = scrollLine; i < totalLines && (i - scrollLine) < maxVisibleLines; i++) {
        tftInstance->drawString(lines[i], x, curY, font);
        curY += lineHeight;
    }
}

// ============================================================================
// MAIN LOOP & TOUCH DISPATCHER
// ============================================================================
void KryonCloudUI::loop() {
    if (currentSubState == CLOUD_STATE_PAIRING && isWaitingForClaim && activePairingCode.length() > 0) {
        if (millis() - lastWaitPollTime > 3000) {
            lastWaitPollTime = millis();
            bool claimSuccess = KryonCloudManager::pollPairingWait(activePairingCode, 5000);
            if (claimSuccess) {
                isWaitingForClaim = false;
                activePairingCode = "";
                currentSubState = CLOUD_STATE_OVERVIEW;
                draw();
            }
        }
    }
}

void KryonCloudUI::handleTouch(uint16_t x, uint16_t y) {
    if (cachedBanStatus.isBanned) {
        handleBanStatusTouch(x, y);
        return;
    }

    switch (currentSubState) {
        case CLOUD_STATE_PAIRING:
            handlePairingTouch(x, y);
            break;
        case CLOUD_STATE_OVERVIEW:
            handleOverviewTouch(x, y);
            break;
        case CLOUD_STATE_KRYON_AI:
            handleKryonAITouch(x, y);
            break;
        case CLOUD_STATE_KRYON_BEAM:
            handleBeamTouch(x, y);
            break;
        case CLOUD_STATE_STORAGE:
            handleStorageTouch(x, y);
            break;
        case CLOUD_STATE_LIMITS:
            handleLimitsTouch(x, y);
            break;
        case CLOUD_STATE_BAN_STATUS:
            handleBanStatusTouch(x, y);
            break;
    }
}
