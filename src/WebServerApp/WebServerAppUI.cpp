#include "WebServerAppUI.h"
#include "../WebManager/WebManager.h"
#include "../FileSystem/FileSystem.h"
#include "../Keyboard/MyKeyboard.h"
#include "../UI/UiLayout.h"

KryonDisplay *WebServerAppUI::tftInstance = nullptr;

// Shared rects so draw() and handleTouch() can never disagree. At 240x320 these reproduce the
// historical literals: Set User (12,182,104,32), Set Pass (124,182,104,32), toggle (45,230,150,36).
namespace {
struct WebAppRects {
    UiRect setUser;
    UiRect setPass;
    UiRect toggle;
};

WebAppRects webAppRects(const UiMetrics& m) {
    WebAppRects r{};
    const int16_t s = (int16_t)m.scale;   // 1 at 240x320, so the historical literals are reproduced
    const int16_t bx = (int16_t)(m.list.x + 2 * s);
    const int16_t bw = (int16_t)((m.list.w - 12 * s) / 2);
    const int16_t toggleY = (int16_t)(m.footer.y - 55 * s);
    const int16_t rowY = (int16_t)(toggleY - 48 * s);

    // 32 and 36 are the 240x320 button heights: one body glyph plus padding, and the taller
    // toggle. Both follow the text scale so the labels stay inside their rounded rect.
    r.setUser = { bx, rowY, bw, (int16_t)(32 * s) };
    r.setPass = { (int16_t)(bx + bw + 8 * s), rowY, bw, (int16_t)(32 * s) };

    const int16_t tw = (int16_t)(m.w * 5 / 8);
    r.toggle = { (int16_t)(m.centerX - tw / 2), toggleY, tw, (int16_t)(36 * s) };
    return r;
}
} // namespace

void WebServerAppUI::init(KryonDisplay *tft) {
    tftInstance = tft;
}

void WebServerAppUI::draw() {
    if (!tftInstance) return;
    
    const UiMetrics& m = UiLayout::current();
    const WebAppRects r = webAppRects(m);

    tftInstance->fillScreen(TFT_BLACK);

    // Draw the main border
    tftInstance->drawRoundRect(m.frame.x, m.frame.y, m.frame.w, m.frame.h, 5, TFT_WHITE);

    // Header Bar
    tftInstance->fillRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_BLACK);
    tftInstance->drawRoundRect(m.header.x, m.header.y, m.header.w, m.header.h, 5, TFT_CYAN);
    tftInstance->setTextColor(TFT_CYAN, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("Web Server", m.header.cx(), m.headerTextY, m.fontBody);

    bool wifiDisabled = FileSystem::exists("/local/nowifi.txt");
    bool isConnected = WebManager::isActive();
    bool isRunning = WebManager::isServerRunning();

    tftInstance->setTextDatum(TL_DATUM);

    // Three lines of info, one body glyph plus leading each. The block used to run to six lines by
    // spelling out a "Credentials:" heading and one line per credential; this panel is only 1.5x
    // taller than the reference while the text is 2x, so the heading went and the credentials share
    // a line. What is shown follows what the state is for: the URL while the server is up, the
    // credentials while it is down and you would be typing them in.
    const int16_t s = (int16_t)m.scale;
    const int16_t linePitch = (int16_t)(20 * s);

    int16_t y = (int16_t)(m.header.bottom() + 8 * s);

    String user = WebManager::getAdminUsername();
    String pass = WebManager::getAdminPassword();

    if (wifiDisabled) {
        tftInstance->setTextColor(TFT_RED, TFT_BLACK);
        tftInstance->drawString("WiFi is DISABLED", m.list.x + 5 * s, y, m.fontBody);
        y += linePitch;
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->drawString("Please turn on WiFi", m.list.x + 5 * s, y, m.fontBody);
        y += linePitch;
        tftInstance->drawString("in Settings > WiFi.", m.list.x + 5 * s, y, m.fontBody);
    } else if (!isConnected) {
        tftInstance->setTextColor(TFT_YELLOW, TFT_BLACK);
        tftInstance->drawString("WiFi DISCONNECTED", m.list.x + 5 * s, y, m.fontBody);
        y += linePitch;
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->drawString("Connect to a network", m.list.x + 5 * s, y, m.fontBody);
        y += linePitch;
        tftInstance->drawString("to start Web Server.", m.list.x + 5 * s, y, m.fontBody);
    } else if (!isRunning) {
        String ip = WebManager::getIPAddress();
        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->drawString("Status:", m.list.x + 5 * s, y, m.fontBody);
        tftInstance->setTextColor(TFT_ORANGE, TFT_BLACK);
        tftInstance->drawString("STOPPED", m.list.x + 65 * s, y, m.fontBody);
        y += linePitch;

        tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
        tftInstance->drawString("IP:", m.list.x + 5 * s, y, m.fontBody);
        tftInstance->setTextColor(TFT_CYAN, TFT_BLACK);
        tftInstance->drawString(ip, m.list.x + 35 * s, y, m.fontBody);
        y += linePitch;

        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->drawString("User: " + user + "   Pass: " + pass, m.list.x + 5 * s, y, m.fontBody);
    } else {
        String ip = WebManager::getIPAddress();

        tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
        tftInstance->drawString("Status:", m.list.x + 5 * s, y, m.fontBody);
        tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
        tftInstance->drawString("RUNNING", m.list.x + 65 * s, y, m.fontBody);
        y += linePitch;

        tftInstance->setTextColor(TFT_GREEN, TFT_BLACK);
        tftInstance->drawString("IP:", m.list.x + 5 * s, y, m.fontBody);
        tftInstance->setTextColor(TFT_CYAN, TFT_BLACK);
        tftInstance->drawString(ip, m.list.x + 35 * s, y, m.fontBody);
        y += linePitch;

        tftInstance->setTextColor(TFT_CYAN, TFT_BLACK);
        tftInstance->drawString("http://" + ip, m.list.x + 5 * s, y, m.fontBody);
    }

    // Change User / Change Pass buttons
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->fillRoundRect(r.setUser.x, r.setUser.y, r.setUser.w, r.setUser.h, 5, 0x18C3 /* Dark Blue-Grey */);
    tftInstance->drawRoundRect(r.setUser.x, r.setUser.y, r.setUser.w, r.setUser.h, 5, TFT_CYAN);
    tftInstance->setTextColor(TFT_WHITE, 0x18C3);
    tftInstance->drawString("Set User", r.setUser.cx(), r.setUser.cy(), m.fontBody);

    tftInstance->fillRoundRect(r.setPass.x, r.setPass.y, r.setPass.w, r.setPass.h, 5, 0x18C3 /* Dark Blue-Grey */);
    tftInstance->drawRoundRect(r.setPass.x, r.setPass.y, r.setPass.w, r.setPass.h, 5, TFT_CYAN);
    tftInstance->setTextColor(TFT_WHITE, 0x18C3);
    tftInstance->drawString("Set Pass", r.setPass.cx(), r.setPass.cy(), m.fontBody);

    // Toggle Button (bottom area, above footer)
    const UiRect& tg = r.toggle;
    if (!isRunning || !isConnected || wifiDisabled) {
        uint16_t btnColor = (isConnected && !wifiDisabled) ? 0x03E0 /* Dark Green */ : TFT_DARKGREY;
        tftInstance->fillRoundRect(tg.x, tg.y, tg.w, tg.h, 6, btnColor);
        tftInstance->drawRoundRect(tg.x, tg.y, tg.w, tg.h, 6, TFT_WHITE);
        tftInstance->setTextColor(TFT_WHITE, btnColor);
        tftInstance->drawString("Turn ON", tg.cx(), tg.cy(), m.fontBody);
    } else {
        tftInstance->fillRoundRect(tg.x, tg.y, tg.w, tg.h, 6, TFT_RED);
        tftInstance->drawRoundRect(tg.x, tg.y, tg.w, tg.h, 6, TFT_WHITE);
        tftInstance->setTextColor(TFT_WHITE, TFT_RED);
        tftInstance->drawString("Turn OFF", tg.cx(), tg.cy(), m.fontBody);
    }

    // Touch Footer
    tftInstance->drawRoundRect(m.footer.x, m.footer.y, m.footer.w, m.footer.h, 5, TFT_WHITE);
    tftInstance->setTextColor(TFT_WHITE, TFT_BLACK);
    tftInstance->setTextDatum(MC_DATUM);
    tftInstance->drawString("EXIT", m.footerButtonCenterX(UI_FOOTER_SEL), m.footerTextY, m.fontBody);
}

void WebServerAppUI::handleTouch(uint16_t x, uint16_t y) {
    extern int currentState;
    const UiMetrics& m = UiLayout::current();
    const WebAppRects r = webAppRects(m);

    // Set User Button
    if (r.setUser.contains((int16_t)x, (int16_t)y)) {
        String curUser = WebManager::getAdminUsername();
        String newUser = MyKeyboard::getString(curUser, "Enter Web Username:", 24);
        newUser.trim();
        if (newUser.length() > 0) {
            WebManager::setAdminCredentials(newUser, WebManager::getAdminPassword());
        }
        draw();
        return;
    }

    // Set Pass Button
    if (r.setPass.contains((int16_t)x, (int16_t)y)) {
        String curPass = WebManager::getAdminPassword();
        String newPass = MyKeyboard::getString(curPass, "Enter Web Password:", 32);
        newPass.trim();
        if (newPass.length() > 0) {
            WebManager::setAdminCredentials(WebManager::getAdminUsername(), newPass);
        }
        draw();
        return;
    }

    // Toggle Button
    if (r.toggle.contains((int16_t)x, (int16_t)y)) {
        if (!WebManager::isActive()) {
            return; // Cannot turn on without WiFi connection
        }

        if (WebManager::isServerRunning()) {
            WebManager::stopServer();
            FileSystem::deleteFile("/local/web_on.txt");
        } else {
            FileSystem::writeTextFile("/local/web_on.txt", "1");
            WebManager::startServer();
        }
        
        draw();
        return;
    }

    // Bottom Nav: EXIT
    if (m.inFooter((int16_t)y)) {
        if (m.footerButtonFromX((int16_t)x) == UI_FOOTER_SEL) {
            currentState = 0; // STATE_LAUNCHER
        }
    }
}


