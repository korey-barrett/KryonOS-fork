#include "WebManager.h"
#include <WiFi.h>
#include <SD.h>
#include <LittleFS.h>
#include <ESPAsyncWebServer.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include "../FileSystem/FileSystem.h"
#include "web_assets_gz.h"
#include "../Kernel/TimeManager.h"
#include "../Kernel/WiFiManager.h"

static AsyncWebServer *server = nullptr;
static String s_adminUser = "admin";
static String s_adminPass = "";

struct SessionEntry {
    char token[33];
    time_t expiresAt;
    bool active;
};

static SessionEntry s_sessions[3] = {0};
static int s_failedAttempts = 0;
static time_t s_lockoutUntil = 0;

static void loadWebCredentials() {
    Preferences prefs;
    if (prefs.begin("kryon_web", false)) {
        s_adminUser = prefs.getString("user", "admin");
        s_adminPass = prefs.getString("pass", "");
        if (s_adminPass.length() == 0) {
            uint32_t r = esp_random();
            char buf[12];
            snprintf(buf, sizeof(buf), "%08lx", (unsigned long)r);
            s_adminPass = String(buf);
            prefs.putString("pass", s_adminPass);
        }
        prefs.end();
    } else {
        s_adminUser = "admin";
        s_adminPass = "admin123";
    }
}

String WebManager::getAdminUsername() {
    if (s_adminPass.length() == 0) loadWebCredentials();
    return s_adminUser;
}

String WebManager::getAdminPassword() {
    if (s_adminPass.length() == 0) loadWebCredentials();
    return s_adminPass;
}

void WebManager::setAdminCredentials(const String& username, const String& password) {
    s_adminUser = username;
    s_adminPass = password;
    Preferences prefs;
    if (prefs.begin("kryon_web", false)) {
        prefs.putString("user", s_adminUser);
        prefs.putString("pass", s_adminPass);
        prefs.end();
    }
}

static String generateSessionToken() {
    uint32_t r1 = esp_random();
    uint32_t r2 = esp_random();
    uint32_t r3 = esp_random();
    uint32_t r4 = esp_random();
    char buf[33];
    snprintf(buf, sizeof(buf), "%08lx%08lx%08lx%08lx", 
             (unsigned long)r1, (unsigned long)r2, (unsigned long)r3, (unsigned long)r4);
    return String(buf);
}

static String createSession(bool remember = false) {
    time_t now = time(nullptr);
    int slot = 0;
    time_t oldest = now + 999999999ULL;
    for (int i = 0; i < 3; i++) {
        if (!s_sessions[i].active || s_sessions[i].expiresAt < now) {
            slot = i;
            break;
        }
        if (s_sessions[i].expiresAt < oldest) {
            oldest = s_sessions[i].expiresAt;
            slot = i;
        }
    }
    String tok = generateSessionToken();
    strncpy(s_sessions[slot].token, tok.c_str(), 32);
    s_sessions[slot].token[32] = '\0';
    s_sessions[slot].expiresAt = remember ? (now + 315360000ULL) : (now + 86400); // 10 years if remember me, else 24 hours
    s_sessions[slot].active = true;
    return tok;
}

static bool isValidSession(const String& tok) {
    if (tok.length() != 32) return false;
    time_t now = time(nullptr);
    for (int i = 0; i < 3; i++) {
        if (s_sessions[i].active && s_sessions[i].expiresAt >= now) {
            if (strncmp(s_sessions[i].token, tok.c_str(), 32) == 0) {
                return true;
            }
        }
    }
    return false;
}

static void invalidateSession(const String& tok) {
    for (int i = 0; i < 3; i++) {
        if (s_sessions[i].active && strncmp(s_sessions[i].token, tok.c_str(), 32) == 0) {
            s_sessions[i].active = false;
        }
    }
}

// Helper to extract session token from Cookie header
static String extractCookieToken(AsyncWebServerRequest *request) {
    if (request->hasHeader("Cookie")) {
        String cookieHeader = request->getHeader("Cookie")->value();
        int idx = cookieHeader.indexOf("kryon_session=");
        if (idx >= 0) {
            int start = idx + 14;
            int end = cookieHeader.indexOf(';', start);
            if (end == -1) end = cookieHeader.length();
            String tok = cookieHeader.substring(start, end);
            tok.trim();
            return tok;
        }
    }
    return "";
}

// Helper to check if request is authenticated
static bool isAuthenticated(AsyncWebServerRequest *request) {
    // 1. Check Session Cookie
    String cookieTok = extractCookieToken(request);
    if (cookieTok.length() > 0 && isValidSession(cookieTok)) {
        return true;
    }

    // 2. Check Authorization Bearer Token
    if (request->hasHeader("Authorization")) {
        String authHeader = request->getHeader("Authorization")->value();
        if (authHeader.startsWith("Bearer ")) {
            String bearerTok = authHeader.substring(7);
            bearerTok.trim();
            if (isValidSession(bearerTok)) return true;
        }
    }

    // 3. Check HTTP Basic Auth
    String u = WebManager::getAdminUsername();
    String p = WebManager::getAdminPassword();
    if (request->authenticate(u.c_str(), p.c_str())) {
        return true;
    }

    return false;
}

// Helper to strictly enforce Authentication on API endpoints (returns 403 Forbidden on failure)
static bool checkAuth(AsyncWebServerRequest *request) {
    if (isAuthenticated(request)) {
        return true;
    }
    AsyncWebServerResponse *res = request->beginResponse(403, "application/json", "{\"error\":\"Forbidden: Authentication required\"}");
    res->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    res->addHeader("Pragma", "no-cache");
    res->addHeader("Expires", "0");
    request->send(res);
    return false;
}

// Helper to sanitize path strings (reject traversal .. and \)
static String normalizePath(const String& path) {
    if (path.indexOf("..") >= 0 || path.indexOf("\\") >= 0) return "__BLOCKED__";
    String p = path;
    while (p.indexOf("//") >= 0) p.replace("//", "/");
    return p;
}

// Helper to check if a path is protected from web access
static bool isProtectedPath(const String& path) {
    if (path == "__BLOCKED__" || path.indexOf("..") >= 0 || path.indexOf("\\") >= 0) return true;
    if (path.indexOf("/system/") >= 0 || path.indexOf("/system") == 0 || 
        path.startsWith("/local/system") || path.startsWith("/sd/system") ||
        path.indexOf("wifi_credentials") >= 0 || path.indexOf("known_networks") >= 0 ||
        path.indexOf("app_permissions") >= 0) {
        return true;
    }
    return false;
}

// Helper to get FS based on path
static fs::FS* getFSFromPath(String& path) {
    if (path.startsWith("/sd")) {
        if (!FileSystem::isSDMounted()) return nullptr;
        path = path.substring(3);
        if (path == "") path = "/";
        return FileSystem::sdVolume();
    } else if (path.startsWith("/littlefs") || path.startsWith("/local")) {
        if (path.startsWith("/littlefs")) {
            path = path.substring(9);
        } else if (path.startsWith("/local")) {
            path = path.substring(6);
        }
        if (path == "") path = "/";
        return &LittleFS;
    }
    return nullptr;
}

// Helper to ensure each parent directory level exists sequentially in LittleFS/SD
static void ensureParentDirectories(fs::FS* fs, const String& path) {
    if (!fs) return;
    int pos = 0;
    while ((pos = path.indexOf('/', pos + 1)) > 0) {
        String dirPath = path.substring(0, pos);
        if (dirPath.length() > 0 && !fs->exists(dirPath)) {
            fs->mkdir(dirPath);
        }
    }
}

bool WebManager::startServer() {
    if (server != nullptr) {
        return true;
    }

    if (!WiFiManager::isConnected()) {
        Serial.println("[WebManager] Cannot start Async Web Server: WiFi is not connected.");
        return false;
    }

    loadWebCredentials();
    server = new AsyncWebServer(80);

    // Root page - serves file manager HTML UI only when authenticated; otherwise serves clean standalone login gateway with strict anti-caching
    server->on("/", HTTP_GET, [](AsyncWebServerRequest *request){
        AsyncWebServerResponse *response;
        if (isAuthenticated(request)) {
            response = request->beginResponse(200, "text/html",
                                              filemanager_html_gz,
                                              filemanager_html_gz_len);
        } else {
            response = request->beginResponse(200, "text/html",
                                              login_html_gz,
                                              login_html_gz_len);
        }
        // Both blobs are gzip streams generated at build time from the .html sources (see
        // scripts/gzip_web_assets.py). The raw literals no longer exist, so there is nothing to
        // fall back to: the body is always gzip. Content-Length is the compressed size, which is
        // what the ESPAsyncWebServer uint8_t*/size_t overload sets.
        response->addHeader("Content-Encoding", "gzip");
        response->addHeader("Vary", "Accept-Encoding");
        response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        response->addHeader("Pragma", "no-cache");
        response->addHeader("Expires", "0");
        request->send(response);
    });

    // Session Login Endpoint (Rate-Limited & Sanitized)
    server->on("/api/login", HTTP_POST, [](AsyncWebServerRequest *request){}, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        time_t now = time(nullptr);
        if (now < s_lockoutUntil) {
            int remaining = (int)(s_lockoutUntil - now);
            if (remaining <= 0) remaining = 30;
            AsyncWebServerResponse *res = request->beginResponse(429, "application/json", "{\"error\":\"Too many failed login attempts\",\"retry_after\":" + String(remaining) + "}");
            res->addHeader("Retry-After", String(remaining));
            res->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
            res->addHeader("Pragma", "no-cache");
            res->addHeader("Expires", "0");
            request->send(res);
            return;
        }

        JsonDocument doc;
        DeserializationError err = deserializeJson(doc, data, len);
        if (err) {
            AsyncWebServerResponse *res = request->beginResponse(400, "application/json", "{\"error\":\"Invalid JSON format\"}");
            res->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
            request->send(res);
            return;
        }

        String user = doc["username"] | "";
        String pass = doc["password"] | "";
        bool remember = doc["rememberMe"] | false;

        if (user == WebManager::getAdminUsername() && pass == WebManager::getAdminPassword()) {
            s_failedAttempts = 0;
            String tok = createSession(remember);
            uint32_t maxAge = remember ? 315360000 : 86400;

            AsyncWebServerResponse *response = request->beginResponse(200, "application/json", "{\"status\":\"ok\",\"token\":\"" + tok + "\"}");
            response->addHeader("Set-Cookie", "kryon_session=" + tok + "; HttpOnly; SameSite=Strict; Path=/; Max-Age=" + String(maxAge));
            response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
            response->addHeader("Pragma", "no-cache");
            response->addHeader("Expires", "0");
            request->send(response);
        } else {
            s_failedAttempts++;
            if (s_failedAttempts >= 5) {
                s_lockoutUntil = now + 30; // 30 second cooldown
                AsyncWebServerResponse *res = request->beginResponse(429, "application/json", "{\"error\":\"Too many failed login attempts\",\"retry_after\":30}");
                res->addHeader("Retry-After", "30");
                res->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
                res->addHeader("Pragma", "no-cache");
                res->addHeader("Expires", "0");
                request->send(res);
                return;
            }
            AsyncWebServerResponse *res = request->beginResponse(401, "application/json", "{\"error\":\"Invalid username or password\",\"attempts_left\":" + String(5 - s_failedAttempts) + "}");
            res->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
            res->addHeader("Pragma", "no-cache");
            res->addHeader("Expires", "0");
            request->send(res);
        }
    });

    // Session Logout Endpoint
    server->on("/api/logout", HTTP_POST, [](AsyncWebServerRequest *request){
        String cookieTok = extractCookieToken(request);
        if (cookieTok.length() > 0) {
            invalidateSession(cookieTok);
        }
        AsyncWebServerResponse *response = request->beginResponse(200, "application/json", "{\"status\":\"logged_out\"}");
        response->addHeader("Set-Cookie", "kryon_session=; HttpOnly; SameSite=Strict; Path=/; Max-Age=0");
        response->addHeader("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        response->addHeader("Pragma", "no-cache");
        response->addHeader("Expires", "0");
        request->send(response);
    });

    server->on("/api/storage", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!checkAuth(request)) return;

        JsonDocument doc;
        
        // LittleFS
        uint64_t lfsTotal = LittleFS.totalBytes();
        uint64_t lfsUsed = LittleFS.usedBytes();
        uint64_t lfsFree = (lfsTotal > lfsUsed) ? (lfsTotal - lfsUsed) : 0;
        JsonObject lfs = doc["littlefs"].to<JsonObject>();
        lfs["mounted"] = true;
        lfs["total"] = lfsTotal;
        lfs["used"] = lfsUsed;
        lfs["free"] = lfsFree;

        // SD Card
        bool sdMounted = FileSystem::isSDMounted();
        JsonObject sd = doc["sd"].to<JsonObject>();
        sd["mounted"] = sdMounted;
        if (sdMounted) {
            uint64_t sdTotal = SD.totalBytes();
            uint64_t sdUsed = SD.usedBytes();
            uint64_t sdFree = (sdTotal > sdUsed) ? (sdTotal - sdUsed) : 0;
            sd["total"] = sdTotal;
            sd["used"] = sdUsed;
            sd["free"] = sdFree;
        } else {
            sd["total"] = 0;
            sd["used"] = 0;
            sd["free"] = 0;
        }

        // System specs
        JsonObject sys = doc["system"].to<JsonObject>();
        sys["heapFree"] = ESP.getFreeHeap();
        sys["heapTotal"] = ESP.getHeapSize();
#if defined(BOARD_HAS_PSRAM)
        bool psramOk = psramFound();
        sys["hasPsram"] = psramOk;
        sys["psramFree"] = psramOk ? ESP.getFreePsram() : 0;
        sys["psramTotal"] = psramOk ? ESP.getPsramSize() : 0;
#else
        sys["hasPsram"] = false;
        sys["psramFree"] = 0;
        sys["psramTotal"] = 0;
#endif
        sys["version"] = KRYONOS_VERSION;
        sys["chip"] = ESP.getChipModel();

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    server->on("/api/list", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!checkAuth(request)) return;

        if (!request->hasParam("dir")) {
            request->send(400, "text/plain", "Missing dir parameter");
            return;
        }
        String origPath = normalizePath(request->getParam("dir")->value());
        if (isProtectedPath(origPath)) {
            request->send(403, "text/plain", "Access Denied: System Path Protected");
            return;
        }

        String path = origPath;
        fs::FS* fs = getFSFromPath(path);
        if (!fs) {
            request->send(400, "text/plain", "Invalid storage");
            return;
        }

        File dir = fs->open(path);
        if (!dir || !dir.isDirectory()) {
            request->send(404, "text/plain", "Not a directory");
            return;
        }

        JsonDocument doc;
        JsonArray array = doc.to<JsonArray>();

        File file = dir.openNextFile();
        while (file) {
            String name = String(file.name());
            if (name != "system" && !name.startsWith("system/") && name != "wifi_credentials.enc" && name != "app_permissions.json") {
                JsonObject item = array.add<JsonObject>();
                item["name"] = name;
                item["type"] = file.isDirectory() ? "dir" : "file";
                item["size"] = file.size();
            }
            file.close();
            file = dir.openNextFile();
        }
        dir.close();

        String response;
        serializeJson(doc, response);
        request->send(200, "application/json", response);
    });

    server->on("/api/edit", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!checkAuth(request)) return;

        if (!request->hasParam("path")) {
            request->send(400, "text/plain", "Missing path");
            return;
        }
        String origPath = normalizePath(request->getParam("path")->value());
        if (isProtectedPath(origPath)) {
            request->send(403, "text/plain", "Access Denied: Protected File");
            return;
        }

        String path = origPath;
        fs::FS* fs = getFSFromPath(path);
        if (!fs || !fs->exists(path)) {
            request->send(404, "text/plain", "File not found");
            return;
        }
        request->send(*fs, path, "text/plain");
    });

    server->on("/api/edit", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!checkAuth(request)) return;

        if (!request->hasParam("path", true) || !request->hasParam("content", true)) {
            request->send(400, "text/plain", "Missing parameters");
            return;
        }
        String origPath = normalizePath(request->getParam("path", true)->value());
        if (isProtectedPath(origPath)) {
            request->send(403, "text/plain", "Access Denied: Protected File");
            return;
        }

        String path = origPath;
        String content = request->getParam("content", true)->value();
        fs::FS* fs = getFSFromPath(path);
        if (!fs) {
            request->send(400, "text/plain", "Invalid storage");
            return;
        }

        File f = fs->open(path, FILE_WRITE);
        if (f) {
            f.print(content);
            f.close();
            request->send(200, "text/plain", "OK");
        } else {
            request->send(500, "text/plain", "Failed to write file");
        }
    });

    // Direct raw streaming save endpoint (prevents OOM on large files)
    server->on("/api/save", HTTP_POST, [](AsyncWebServerRequest *request){
        if (request->_tempFile) {
            request->_tempFile.close();
        }
        request->send(200, "text/plain", "OK");
    }, NULL, [](AsyncWebServerRequest *request, uint8_t *data, size_t len, size_t index, size_t total) {
        if (!checkAuth(request)) return;
        if (!request->hasParam("path")) return;
        
        String origPath = normalizePath(request->getParam("path")->value());
        if (isProtectedPath(origPath)) return;
        
        String path = origPath;
        fs::FS* fs = getFSFromPath(path);
        if (!fs) return;
        
        if (index == 0) {
            ensureParentDirectories(fs, path);
            request->_tempFile = fs->open(path, FILE_WRITE);
        }
        
        if (request->_tempFile) {
            if (len > 0) {
                request->_tempFile.write(data, len);
            }
            if (index + len >= total) {
                request->_tempFile.close();
            }
        }
    });

    server->on("/api/download", HTTP_GET, [](AsyncWebServerRequest *request){
        if (!checkAuth(request)) return;

        if (!request->hasParam("path")) {
            request->send(400, "text/plain", "Missing path");
            return;
        }
        String origPath = normalizePath(request->getParam("path")->value());
        if (isProtectedPath(origPath)) {
            request->send(403, "text/plain", "Access Denied: Protected File");
            return;
        }

        String path = origPath;
        fs::FS* fs = getFSFromPath(path);
        if (!fs || !fs->exists(path)) {
            request->send(404, "text/plain", "File not found");
            return;
        }
        AsyncWebServerResponse *response = request->beginResponse(*fs, path, "application/octet-stream", true);
        request->send(response);
    });

    server->on("/api/delete", HTTP_DELETE, [](AsyncWebServerRequest *request){
        if (!checkAuth(request)) return;

        if (!request->hasParam("path", true)) {
            request->send(400, "text/plain", "Missing path");
            return;
        }
        String origPath = normalizePath(request->getParam("path", true)->value());
        if (isProtectedPath(origPath)) {
            request->send(403, "text/plain", "Access Denied: Protected File");
            return;
        }

        String path = origPath;
        fs::FS* fs = getFSFromPath(path);
        if (!fs) {
            request->send(400, "text/plain", "Invalid storage");
            return;
        }

        File f = fs->open(path);
        bool isDir = false;
        if (f) {
            isDir = f.isDirectory();
            f.close();
        }

        if (isDir) {
            fs->rmdir(path);
        } else {
            fs->remove(path);
        }
        request->send(200, "text/plain", "OK");
    });

    server->on("/api/create", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!checkAuth(request)) return;

        if (!request->hasParam("path", true) || !request->hasParam("type", true)) {
            request->send(400, "text/plain", "Missing parameters");
            return;
        }
        String origPath = normalizePath(request->getParam("path", true)->value());
        if (isProtectedPath(origPath)) {
            request->send(403, "text/plain", "Access Denied: Protected File");
            return;
        }

        String path = origPath;
        String type = request->getParam("type", true)->value();
        fs::FS* fs = getFSFromPath(path);
        if (!fs) {
            request->send(400, "text/plain", "Invalid storage");
            return;
        }

        if (type == "folder") {
            ensureParentDirectories(fs, path);
            fs->mkdir(path);
        } else {
            ensureParentDirectories(fs, path);
            File f = fs->open(path, FILE_WRITE);
            if (f) f.close();
        }
        request->send(200, "text/plain", "OK");
    });

    server->on("/api/rename", HTTP_POST, [](AsyncWebServerRequest *request){
        if (!checkAuth(request)) return;

        if (!request->hasParam("oldPath", true) || !request->hasParam("newPath", true)) {
            request->send(400, "text/plain", "Missing parameters");
            return;
        }
        String oldPath = normalizePath(request->getParam("oldPath", true)->value());
        String newPath = normalizePath(request->getParam("newPath", true)->value());
        
        if (isProtectedPath(oldPath) || isProtectedPath(newPath)) {
            request->send(403, "text/plain", "Access Denied: Protected File");
            return;
        }

        String oldFsPath = oldPath;
        String newFsPath = newPath;
        fs::FS* fs1 = getFSFromPath(oldFsPath);
        fs::FS* fs2 = getFSFromPath(newFsPath);
        
        if (fs1 != fs2 || !fs1) {
            request->send(400, "text/plain", "Cannot rename across different storages or invalid");
            return;
        }

        ensureParentDirectories(fs1, newFsPath);
        if (fs1->rename(oldFsPath, newFsPath)) {
            request->send(200, "text/plain", "OK");
        } else {
            request->send(500, "text/plain", "Rename failed");
        }
    });

    // Handle file & folder uploads with sequential LittleFS directory creation
    server->on("/api/upload", HTTP_POST, [](AsyncWebServerRequest *request){
        if (request->_tempFile) {
            request->_tempFile.close();
        }
        request->send(200, "text/plain", "Upload Complete");
    }, [](AsyncWebServerRequest *request, String filename, size_t index, uint8_t *data, size_t len, bool final) {
        if (!checkAuth(request)) return;

        String path = normalizePath(filename); 
        if (isProtectedPath(path)) return;
        
        // Intercept app uploads and respect default installation location
        if (path.startsWith("/local/apps/") || path.startsWith("/sd/apps/")) {
            bool defaultSD = FileSystem::exists("/local/config_install_sd.txt");
            int appsIndex = path.indexOf("/apps/");
            String relativePath = path.substring(appsIndex + 6);
            
            if (defaultSD && FileSystem::exists("/sd/")) {
                path = "/sd/apps/" + relativePath;
            } else {
                path = "/local/apps/" + relativePath;
            }
        }
        
        fs::FS* fs = getFSFromPath(path);
        if (!fs) return;

        if (!index) {
            ensureParentDirectories(fs, path);
            request->_tempFile = fs->open(path, FILE_WRITE);
        }
        if (request->_tempFile) {
            if (len) {
                request->_tempFile.write(data, len);
            }
            if (final) {
                request->_tempFile.close();
            }
        }
    });

    server->begin();
    Serial.println("[WebManager] Async Web Server successfully listening on port 80 (Authenticated)");
    return true;
}

void WebManager::stopServer() {
    if (server) {
        server->end();
        delete server;
        server = nullptr;
        Serial.println("[WebManager] Async Web Server stopped.");
    }
}

bool WebManager::isServerRunning() {
    return server != nullptr;
}

bool WebManager::init() {
    WiFiManager::init();

    if (WiFiManager::isConnected()) {
        Serial.println("[WebManager] WiFi connected via WiFiManager!");
        Serial.print("[WebManager] IP Address: ");
        Serial.println(WiFiManager::getIP());

        // Sync NTP Time
        TimeManager::syncNTP();

        // Check if Web Server is enabled by user
        if (FileSystem::exists("/local/web_on.txt")) {
            startServer();
        } else {
            Serial.println("[WebManager] Web Server is disabled by default (web_on.txt not set).");
        }
        return true;
    } else {
        Serial.println("[WebManager] WiFi is not connected at boot.");
        return false;
    }
}

bool WebManager::isActive() {
    return WiFiManager::isConnected();
}

String WebManager::getIPAddress() {
    return WiFiManager::getIP();
}
