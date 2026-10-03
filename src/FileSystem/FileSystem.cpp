#include "FileSystem.h"
#include "Hal/Boards/Board.h"
// mbedTLS 4.x (ESP-IDF v6.1) no longer publishes md5.h -- it moved under the driver's private/
// directory -- so MD5 comes from PSA Crypto. PSA has always had it; the header was simply the only
// thing that vanished.
#include <psa/crypto.h>

// These boards' cards are on SD_MMC, not the SPI `SD` class the shared code below names. Their
// BoardConfig.cpp files are separate translation units, so the object has to be declared here too.
#if defined(TARGET_WAVESHARE_S3_LCD21B) || defined(TARGET_ESP32S31_KORVO1)
#include <SD_MMC.h>
#endif

static SPIClass *sdSPI = nullptr;
static bool sdMounted = false;

// The filesystem the card is ACTUALLY mounted on, and what every "/sd/..." path must address.
// On the remaining boards that is the SPI `SD` object; the Waveshare's slot is SDMMC 1-bit and the
// Korvo-1's is SDMMC 4-bit, so on both of those it is SD_MMC. Routing paths through a literal `SD`
// would silently address an unmounted filesystem -- writes would appear to succeed and go nowhere.
static fs::FS *sdFS = nullptr;

// Capacity and unmount are not on fs::FS: that class exposes only the operations a filesystem can
// be asked generically (open, exists, remove, rename, mkdir, rmdir), while totalBytes/usedBytes/end
// live on the concrete fs::SDFS and fs::SDMMCFS classes, which share no base beyond it. So sdFS
// routes paths and these three route the volume.
#if defined(TARGET_WAVESHARE_S3_LCD21B) || defined(TARGET_ESP32S31_KORVO1)
static inline uint64_t sdTotalBytes() { return SD_MMC.totalBytes(); }
static inline uint64_t sdUsedBytes()  { return SD_MMC.usedBytes(); }
static inline void     sdEnd()        { SD_MMC.end(); }
#else
static inline uint64_t sdTotalBytes() { return SD.totalBytes(); }
static inline uint64_t sdUsedBytes()  { return SD.usedBytes(); }
static inline void     sdEnd()        { SD.end(); }
#endif

bool FileSystem::isSDMounted() {
    return sdMounted;
}

fs::FS* FileSystem::sdVolume() {
    return sdFS ? sdFS : static_cast<fs::FS*>(&SD);
}

// Resolves a virtual path like "/local/apps/foo" or "/sd/data" into a
// target filesystem pointer and relative path within that FS.
// The returned relPath String has trailing slashes stripped.
static fs::FS* getTargetFS(const char* path, String& relPath) {
    if (path == nullptr) return nullptr;
    fs::FS* targetFS = nullptr;
    if (strncmp(path, "/sd", 3) == 0 && (path[3] == '/' || path[3] == '\0')) {
        if (!sdMounted) return nullptr;
        relPath = (path[3] == '\0') ? "/" : (path + 3);
        targetFS = sdFS;
    } else if (strncmp(path, "/local", 6) == 0 && (path[6] == '/' || path[6] == '\0')) {
        relPath = (path[6] == '\0') ? "/" : (path + 6);
        targetFS = &LittleFS;
    } else if (strncmp(path, "/littlefs", 9) == 0 && (path[9] == '/' || path[9] == '\0')) {
        relPath = (path[9] == '\0') ? "/" : (path + 9);
        targetFS = &LittleFS;
    } else {
        // Default to LittleFS for root-relative paths e.g. /apps or /tmp_download
        relPath = path;
        targetFS = &LittleFS;
    }
    // Strip trailing slashes (but keep root "/")
    while (relPath.length() > 1 && relPath.endsWith("/")) {
        relPath = relPath.substring(0, relPath.length() - 1);
    }
    return targetFS;
}

bool FileSystem::init() {
    bool success = true;
    
    // Initialize LittleFS (format on first boot after flash erase)
    if (!LittleFS.begin(true)) {
        Serial.println("LittleFS: First mount failed, formatting...");
        if (LittleFS.format()) {
            Serial.println("LittleFS: Format successful, retrying mount...");
            if (!LittleFS.begin(true)) {
                Serial.println("LittleFS: Mount still failed after format!");
                success = false;
            } else {
                Serial.println("LittleFS: Mount Successful after format.");
            }
        } else {
            Serial.println("LittleFS: Format failed!");
            success = false;
        }
    } else {
        Serial.println("LittleFS Mount Successful");
    }

    // Create required directories if LittleFS is mounted
    if (success) {
        if (!LittleFS.exists("/apps")) LittleFS.mkdir("/apps");
        if (!LittleFS.exists("/system")) LittleFS.mkdir("/system");
        if (!LittleFS.exists("/tmp_download")) LittleFS.mkdir("/tmp_download");
        Serial.println("LittleFS directories verified.");
        migrateSystemFiles();
    }

    // Initialize dedicated SPI bus for SD Card at runtime
    //
    // NOTE: this is a BOARD pin map, so it is keyed off the board's own TARGET_* macro and never off
    // a chip macro. CONFIG_IDF_TARGET_ESP32S3 is defined for every ESP32-S3 build, so keying off it
    // selected this generic map on the Waveshare 2.1B as well -- and pins 39/40/41 are that board's
    // LCD VSYNC, DE and PCLK. Attaching FSPI to them tears down the RGB timing a few lines after the
    // panel came up, which reads on hardware as "backlight on, nothing on screen". The board layer
    // has its own initSD() for that reason; this macro is what let it be dead code.
    //
    // (Also never key this off ARDUINO_USB_CDC_ON_BOOT: the Arduino core always defines that macro --
    // as 0 when USB CDC is off (cores/esp32/HardwareSerial.h) -- so a bare defined() would compile
    // this map into classic-ESP32 builds.)
#if defined(TARGET_ESP32S3_DEFAULT)
    pinMode(42, OUTPUT);
    digitalWrite(42, HIGH);    // De-select SD card CS during startup
    pinMode(39, INPUT_PULLUP); // Enable internal pull-up on MISO/DO for reliable card response
    pinMode(40, OUTPUT);
    digitalWrite(40, HIGH);    // MOSI default high
    pinMode(41, OUTPUT);
    digitalWrite(41, HIGH);    // SCK default high

    // Attempt 1: Host Controller FSPI (SPI2)
    if (sdSPI) { delete sdSPI; sdSPI = nullptr; }
    sdSPI = new SPIClass(FSPI);
    sdSPI->begin(41, 39, 40, -1); // IMPORTANT: SS must be -1 so software GPIO CS (42) works cleanly

    // Physical Layer Spec: Clock 80+ dummy pulses with CS HIGH to wake card into SPI mode
    digitalWrite(42, HIGH);
    for (int i = 0; i < 16; i++) {
        sdSPI->transfer(0xFF);
    }
    delay(10);

    // Try standard frequencies (4MHz -> 1MHz -> 400kHz initialization speed)
    sdMounted = (SD.begin(42, *sdSPI, 4000000, "/sd", 5, false) || 
                 SD.begin(42, *sdSPI, 1000000, "/sd", 5, false) || 
                 SD.begin(42, *sdSPI, 400000, "/sd", 5, false));

    // Attempt 2: Fallback to Host Controller HSPI (SPI3) if FSPI was busy/unavailable
    if (!sdMounted) {
        SD.end();
        delete sdSPI;
        sdSPI = new SPIClass(HSPI);
        sdSPI->begin(41, 39, 40, -1);
        digitalWrite(42, HIGH);
        for (int i = 0; i < 16; i++) {
            sdSPI->transfer(0xFF);
        }
        delay(10);
        sdMounted = (SD.begin(42, *sdSPI, 4000000, "/sd", 5, false) || 
                     SD.begin(42, *sdSPI, 1000000, "/sd", 5, false) || 
                     SD.begin(42, *sdSPI, 400000, "/sd", 5, false));
    }

    if (sdMounted) {
        uint8_t cardType = SD.cardType();
        if (cardType != CARD_NONE) {
            uint64_t cardSize = SD.cardSize() / (1024 * 1024);
            Serial.printf("SD Card Mount Successful (%llu MB, Type: %d)\n", cardSize, cardType);
        } else {
            sdMounted = false;
            SD.end();
            Serial.println("SD Card: Card reported CARD_NONE");
        }
    } else {
        Serial.println("SD Card: Mount failed (Verify 5V power, FAT32 format, & wiring: CS=42, MOSI=40, MISO=39, SCK=41)");
    }
#elif defined(TARGET_CYD)
    // CYD has its own micro-SD slot on a dedicated VSPI bus. Delegate to the board layer so the pin
    // map lives in one place (cyd/BoardConfig.cpp). Do not fold this into the #else below: that one
    // is the generic ESP32 map (esp32-default), whose SCK/MOSI 14/13 and CS 15 are this board's TFT
    // pins.
    sdMounted = (initSD() != nullptr);
#elif defined(TARGET_WAVESHARE_S3_LCD21B)
    // MUST NOT fall into the #else below: that map drives GPIO15, which on this board is the I2C
    // SDA line shared by the TCA9554 expander and the touch controller. This board's slot is SDMMC
    // 1-bit and shares GPIO1/GPIO2 with the panel's 3-wire command bus, so mounting belongs after
    // display init -- which is where this call sits, several steps after Display::begin(). Delegate
    // to the board layer, which owns those pins.
    sdMounted = (initSD() != nullptr);
#elif defined(TARGET_ESP32S31_KORVO1)
    // Same trap as the Waveshare line above, for the same reason and one more: the #else map would
    // drive GPIO15, and on this board that is the I2C line the BSP's touch controller sits on --
    // so falling through would not merely fail to find a card, it would talk over the touch bus.
    // The slot is 4-bit SDMMC on the BSP's own pins (CLK 24, CMD 25, D0-D3 20-23) behind an enable
    // line on GPIO39, none of which the generic map knows about. The board layer owns all of it.
    sdMounted = (initSD() != nullptr);
#else
    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);
    pinMode(26, INPUT_PULLUP);

    if (!sdSPI) sdSPI = new SPIClass(HSPI);
    sdSPI->begin(14, 26, 13, -1);
    if (SD.begin(15, *sdSPI, 4000000) || SD.begin(15, *sdSPI, 1000000)) {
        sdMounted = true;
        Serial.println("SD Card Mount Successful");
    } else {
        sdMounted = false;
        Serial.println("SD Card Mount Failed");
    }
#endif

    // The volume "/sd/..." paths address. Set once here rather than in each arm above, so an arm
    // that grows a new failure path cannot forget it.
#if defined(TARGET_WAVESHARE_S3_LCD21B) || defined(TARGET_ESP32S31_KORVO1)
    sdFS = sdMounted ? static_cast<fs::FS*>(&SD_MMC) : nullptr;
#else
    sdFS = sdMounted ? static_cast<fs::FS*>(&SD) : nullptr;
#endif

    return success;
}

File FileSystem::openFile(const char* path, const char* mode) {
    if (path == nullptr) return File();
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return File();
    return targetFS->open(relPath.c_str(), mode);
}

String FileSystem::readTextFile(const char* path) {
    if (path == nullptr) return "";
    
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return "";
    
    File file = targetFS->open(relPath.c_str());
    if (!file || file.isDirectory()) {
        return "";
    }
    
    size_t size = file.size();
    if (size == 0) {
        file.close();
        return "";
    }
    
    // Pre-allocate String to prevent massive heap fragmentation
    String content;
    if (!content.reserve(size)) {
        Serial.println("Memory allocation failed for reading file.");
        file.close();
        return "";
    }
    
    // Read in 512-byte chunks
    uint8_t buffer[512];
    while (file.available()) {
        size_t bytesRead = file.read(buffer, sizeof(buffer));
        for (size_t i = 0; i < bytesRead; i++) {
            content += (char)buffer[i];
        }
    }
    
    file.close();
    return content;
}

bool FileSystem::writeTextFile(const char* path, const char* content) {
    if (path == nullptr || content == nullptr) return false;
    
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return false;
    
    File file = targetFS->open(relPath.c_str(), FILE_WRITE);
    if (!file) {
        return false;
    }
    
    if (file.print(content)) {
        file.close();
        return true;
    } else {
        file.close();
        return false;
    }
}

bool FileSystem::exists(const char* path) {
    if (path == nullptr) return false;
    
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return false;
    
    return targetFS->exists(relPath.c_str());
}

bool FileSystem::deleteFile(const char* path) {
    if (path == nullptr) return false;
    
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return false;
    
    if (relPath.length() == 0 || relPath == "/") return false;
    return targetFS->remove(relPath.c_str());
}

bool FileSystem::formatLittleFS() {
    Serial.println("Formatting LittleFS...");
    return LittleFS.format();
}

int FileSystem::listDir(const char* dirPath, String* resultFiles, int maxFiles) {
    if (dirPath == nullptr || resultFiles == nullptr) return 0;
    
    String relativePath;
    fs::FS* targetFS = getTargetFS(dirPath, relativePath);
    if (!targetFS) return 0;
    
    File dir = targetFS->open(relativePath.c_str());
    if (!dir || !dir.isDirectory()) {
        return 0;
    }

    int count = 0;
    File file = dir.openNextFile();
    while (file && count < maxFiles) {
        String name = file.name();
        if (name.endsWith("/")) name = name.substring(0, name.length() - 1);
        int lastSlash = name.lastIndexOf('/');
        if (lastSlash >= 0) name = name.substring(lastSlash + 1);
        
        // Return full absolute path e.g. /local/apps/file.js or /sd/apps/file.js
        String fullPath = String(dirPath);
        if (!fullPath.endsWith("/")) fullPath += "/";
        fullPath += name;
        resultFiles[count++] = fullPath;
        
        file = dir.openNextFile();
    }
    return count;
}

int FileSystem::listDirectory(const char* dirPath, FileEntry* entries, int maxEntries) {
    if (dirPath == nullptr || entries == nullptr) return 0;
    
    String relativePath;
    fs::FS* targetFS = getTargetFS(dirPath, relativePath);
    if (!targetFS) return 0;
    
    File dir = targetFS->open(relativePath.c_str());
    if (!dir || !dir.isDirectory()) {
        return 0;
    }

    int count = 0;
    File file = dir.openNextFile();
    while (file && count < maxEntries) {
        String baseName = file.name();
        if (baseName.endsWith("/")) baseName = baseName.substring(0, baseName.length() - 1);
        int lastSlash = baseName.lastIndexOf('/');
        if (lastSlash >= 0) baseName = baseName.substring(lastSlash + 1);
        
        entries[count].name = baseName;
        
        String fullPath = String(dirPath);
        if (!fullPath.endsWith("/")) fullPath += "/";
        fullPath += baseName;
        
        entries[count].path = fullPath;
        entries[count].isDir = file.isDirectory();
        
        count++;
        file = dir.openNextFile();
    }
    return count;
}

bool FileSystem::readCalData(uint16_t* calData) {
    if (!LittleFS.exists("/touch_cal_p.bin")) return false;
    File f = LittleFS.open("/touch_cal_p.bin", FILE_READ);
    if (!f) return false;
    if (f.read((uint8_t*)calData, 10) == 10) {
        f.close();
        // Validate calibration parameters:
        // In TFT_eSPI, calData format is: [x0, deltaX, y0, deltaY, flags]
        // deltaX (calData[1]) and deltaY (calData[3]) must be reasonably large (typically 1500-3800)
        // If they are < 500 or > 4096, or if all are 0, the data is invalid/corrupt
        if (calData[1] < 500 || calData[1] > 4096 || calData[3] < 500 || calData[3] > 4096) {
            Serial.printf("DEBUG: Corrupt touch calibration detected [%u, %u, %u, %u, %u], deleting...\n",
                          calData[0], calData[1], calData[2], calData[3], calData[4]);
            LittleFS.remove("/touch_cal_p.bin");
            return false;
        }
        return true;
    }
    f.close();
    return false;
}

bool FileSystem::writeCalData(uint16_t* calData) {
    File f = LittleFS.open("/touch_cal_p.bin", FILE_WRITE);
    if (!f) return false;
    f.write((uint8_t*)calData, 10);
    f.close();
    return true;
}

bool FileSystem::copyFile(const char* srcPath, const char* dstPath) {
    if (srcPath == nullptr || dstPath == nullptr) return false;
    
    String srcRel;
    fs::FS* srcFS = getTargetFS(srcPath, srcRel);
    if (!srcFS) return false;
    
    String dstRel;
    fs::FS* dstFS = getTargetFS(dstPath, dstRel);
    if (!dstFS) return false;

    File srcFile = srcFS->open(srcRel.c_str(), FILE_READ);
    if (!srcFile || srcFile.isDirectory()) return false;
    
    File dstFile = dstFS->open(dstRel.c_str(), FILE_WRITE);
    if (!dstFile) {
        srcFile.close();
        return false;
    }
    
    size_t n;
    uint8_t buf[512];
    while ((n = srcFile.read(buf, sizeof(buf))) > 0) {
        dstFile.write(buf, n);
    }
    
    srcFile.close();
    dstFile.close();
    return true;
}

int FileSystem::countFilesInDir(const char* dirPath) {
    String relPath;
    fs::FS* targetFS = getTargetFS(dirPath, relPath);
    if (!targetFS) return 0;
    File dir = targetFS->open(relPath.c_str());
    if (!dir || !dir.isDirectory()) return 0;
    int count = 0;
    File f = dir.openNextFile();
    while (f) {
        if (!f.isDirectory()) {
            count++;
        } else {
            String subPath = String(dirPath);
            if (!subPath.endsWith("/")) subPath += "/";
            subPath += f.name();
            count += countFilesInDir(subPath.c_str());
        }
        f = dir.openNextFile();
    }
    return count;
}

bool FileSystem::copyDirectory(const char* srcDir, const char* destDir, void (*progressCb)(int current, int total)) {
    if (!srcDir || !destDir) return false;
    mkdir(destDir);
    
    String relPath;
    fs::FS* srcFS = getTargetFS(srcDir, relPath);
    if (!srcFS) return false;
    
    File dir = srcFS->open(relPath.c_str());
    if (!dir || !dir.isDirectory()) return false;
    
    static int copiedFiles = 0;
    static int totalFiles = 0;
    static bool isTopLevel = true;
    
    if (isTopLevel) {
        copiedFiles = 0;
        totalFiles = countFilesInDir(srcDir);
        if (totalFiles == 0) totalFiles = 1;
        isTopLevel = false;
    }
    
    File file = dir.openNextFile();
    while (file) {
        String fileName = file.name();
        if (fileName.endsWith("/")) fileName = fileName.substring(0, fileName.length() - 1);
        int lastSlash = fileName.lastIndexOf('/');
        if (lastSlash >= 0) fileName = fileName.substring(lastSlash + 1);
        
        String srcFilePath = String(srcDir);
        if (!srcFilePath.endsWith("/")) srcFilePath += "/";
        srcFilePath += fileName;
        
        String dstFilePath = String(destDir);
        if (!dstFilePath.endsWith("/")) dstFilePath += "/";
        dstFilePath += fileName;
        
        if (file.isDirectory()) {
            bool wasTopLevel = isTopLevel;
            isTopLevel = false;
            copyDirectory(srcFilePath.c_str(), dstFilePath.c_str(), progressCb);
            isTopLevel = wasTopLevel;
        } else {
            copyFile(srcFilePath.c_str(), dstFilePath.c_str());
            copiedFiles++;
            if (progressCb) progressCb(copiedFiles, totalFiles);
            yield();
        }
        file = dir.openNextFile();
    }
    
    isTopLevel = true;
    return true;
}

String FileSystem::parseJsonValue(const String& json, const char* key) {
    String searchKey = String("\"" ) + key + "\"";
    int keyIdx = json.indexOf(searchKey);
    if (keyIdx == -1) return "";
    
    int colonIdx = json.indexOf(':', keyIdx + searchKey.length());
    if (colonIdx == -1) return "";
    
    int valStart = colonIdx + 1;
    while (valStart < (int)json.length() && (json[valStart] == ' ' || json[valStart] == '\t')) valStart++;
    
    if (valStart >= (int)json.length()) return "";
    
    if (json[valStart] == '"') {
        int valEnd = json.indexOf('"', valStart + 1);
        if (valEnd == -1) return "";
        return json.substring(valStart + 1, valEnd);
    } else {
        int valEnd = valStart;
        while (valEnd < (int)json.length() && json[valEnd] != ',' && json[valEnd] != '}' && json[valEnd] != '\n') valEnd++;
        String val = json.substring(valStart, valEnd);
        val.trim();
        return val;
    }
}

bool FileSystem::mkdir(const char* path) {
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return false;
    Serial.printf("FS::mkdir relPath='%s'\n", relPath.c_str());
    return targetFS->mkdir(relPath.c_str());
}

bool FileSystem::rmdir(const char* path) {
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return false;
    return targetFS->rmdir(relPath.c_str());
}

bool FileSystem::isDirectory(const char* path) {
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return false;
    File file = targetFS->open(relPath.c_str());
    if (!file) return false;
    bool isDir = file.isDirectory();
    file.close();
    return isDir;
}

bool FileSystem::isFile(const char* path) {
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return false;
    File file = targetFS->open(relPath.c_str());
    if (!file) return false;
    bool isF = !file.isDirectory();
    file.close();
    return isF;
}

bool FileSystem::appendTextFile(const char* path, const char* content) {
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return false;
    File file = targetFS->open(relPath.c_str(), FILE_APPEND);
    if (!file) return false;
    bool res = file.print(content);
    file.close();
    return res;
}

bool FileSystem::renameFile(const char* pathFrom, const char* pathTo) {
    String relPathFrom;
    fs::FS* targetFSFrom = getTargetFS(pathFrom, relPathFrom);
    String relPathTo;
    fs::FS* targetFSTo = getTargetFS(pathTo, relPathTo);
    
    if (!targetFSFrom || !targetFSTo || targetFSFrom != targetFSTo) return false;
    
    return targetFSFrom->rename(relPathFrom.c_str(), relPathTo.c_str());
}

size_t FileSystem::getFileSize(const char* path) {
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return 0;
    File file = targetFS->open(relPath.c_str());
    if (!file) return 0;
    size_t size = file.size();
    file.close();
    return size;
}

time_t FileSystem::getLastModified(const char* path) {
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return 0;
    File file = targetFS->open(relPath.c_str());
    if (!file) return 0;
    time_t mod = file.getLastWrite();
    file.close();
    return mod;
}

size_t FileSystem::getTotalSpace(const char* drive) {
    if (strncmp(drive, "/sd", 3) == 0) return sdMounted ? sdTotalBytes() : 0;
    if (strncmp(drive, "/local", 6) == 0) return LittleFS.totalBytes();
    return 0;
}

size_t FileSystem::getUsedSpace(const char* drive) {
    if (strncmp(drive, "/sd", 3) == 0) return sdMounted ? sdUsedBytes() : 0;
    if (strncmp(drive, "/local", 6) == 0) return LittleFS.usedBytes();
    return 0;
}

size_t FileSystem::getFreeSpace(const char* drive) {
    size_t total = getTotalSpace(drive);
    size_t used = getUsedSpace(drive);
    return total > used ? (total - used) : 0;
}

String FileSystem::getFileMD5(const char* path) {
    String relPath;
    fs::FS* targetFS = getTargetFS(path, relPath);
    if (!targetFS) return "";
    File file = targetFS->open(relPath.c_str(), FILE_READ);
    if (!file || file.isDirectory()) return "";

    // Streaming PSA MD5, for the same reason sha256File() streams: the file is read in 512-byte
    // chunks and never held in memory.
    if (psa_crypto_init() != PSA_SUCCESS) {
        file.close();
        return "";
    }

    psa_hash_operation_t ctx = PSA_HASH_OPERATION_INIT;
    if (psa_hash_setup(&ctx, PSA_ALG_MD5) != PSA_SUCCESS) {
        file.close();
        return "";
    }

    uint8_t buffer[512];
    size_t len;
    while ((len = file.read(buffer, sizeof(buffer))) > 0) {
        if (psa_hash_update(&ctx, buffer, len) != PSA_SUCCESS) {
            psa_hash_abort(&ctx);
            file.close();
            return "";
        }
    }
    file.close();

    uint8_t hash[16];
    size_t hashLen = 0;
    if (psa_hash_finish(&ctx, hash, sizeof(hash), &hashLen) != PSA_SUCCESS) {
        psa_hash_abort(&ctx);
        return "";
    }

    String hexHash = "";
    for (int i = 0; i < 16; i++) {
        char buf[3];
        sprintf(buf, "%02x", hash[i]);
        hexHash += buf;
    }
    return hexHash;
}

bool FileSystem::mountSD() {
    // Board pin map, not chip: see the note in init() for what these pins are on the Waveshare 2.1B.
#if defined(TARGET_ESP32S3_DEFAULT)
    pinMode(42, OUTPUT);
    digitalWrite(42, HIGH);
    pinMode(39, INPUT_PULLUP);
    pinMode(40, OUTPUT);
    digitalWrite(40, HIGH);
    pinMode(41, OUTPUT);
    digitalWrite(41, HIGH);

    if (!sdSPI) {
        sdSPI = new SPIClass(FSPI);
        sdSPI->begin(41, 39, 40, -1);
    }
    digitalWrite(42, HIGH);
    for (int i = 0; i < 16; i++) {
        sdSPI->transfer(0xFF);
    }
    delay(10);
    sdMounted = (SD.begin(42, *sdSPI, 4000000, "/sd", 5, false) || 
                 SD.begin(42, *sdSPI, 1000000, "/sd", 5, false) || 
                 SD.begin(42, *sdSPI, 400000, "/sd", 5, false));
#elif defined(TARGET_CYD)
    sdMounted = (initSD() != nullptr);
#elif defined(TARGET_WAVESHARE_S3_LCD21B) || defined(TARGET_ESP32S31_KORVO1)
    // See the note in the first SD branch: the generic map below drives GPIO15, which is the
    // Waveshare's I2C SDA and the Korvo-1's touch bus. Delegate to the board layer instead -- which
    // also matters on the Korvo-1 for a second reason: its slot is 4-bit SDMMC behind an enable line
    // on GPIO39, and no part of the SPI map below applies to it.
    sdMounted = (initSD() != nullptr);
#else
    if (!sdSPI) return false;
    pinMode(15, OUTPUT);
    digitalWrite(15, HIGH);
    pinMode(26, INPUT_PULLUP);
    sdMounted = (SD.begin(15, *sdSPI, 4000000) || SD.begin(15, *sdSPI, 1000000));
#endif

    // Same as init(): the volume "/sd/..." paths address, set here so every arm above is covered.
#if defined(TARGET_WAVESHARE_S3_LCD21B) || defined(TARGET_ESP32S31_KORVO1)
    sdFS = sdMounted ? static_cast<fs::FS*>(&SD_MMC) : nullptr;
#else
    sdFS = sdMounted ? static_cast<fs::FS*>(&SD) : nullptr;
#endif

    return sdMounted;
}

void FileSystem::unmountSD() {
    if (sdFS) sdEnd();
    sdFS = nullptr;
    sdMounted = false;
}

bool FileSystem::formatSD() {
    return false; // Not natively supported on standard Arduino core without custom FAT commands
}

bool FileSystem::isSystemPath(const char* path) {
    if (!path || path[0] == '\0') return false;
    String p = String(path);
    p.trim();
    if (p.indexOf("..") >= 0 || p.indexOf("\\") >= 0) return true; // Block traversal attempts
    while (p.indexOf("//") >= 0) p.replace("//", "/");

    // Block system directory targets
    if (p.startsWith("/local/system") || p.startsWith("/sd/system") ||
        p.startsWith("/littlefs/system") || p.startsWith("local/system") ||
        p.startsWith("sd/system") || p.startsWith("littlefs/system") ||
        p.startsWith("/system") || p == "system") {
        return true;
    }

    // Block sensitive files and legacy credential paths
    if (p == "/local/known_networks.json" || p == "/known_networks.json" ||
        p == "local/known_networks.json" || p == "known_networks.json" ||
        p == "/littlefs/known_networks.json" || p == "/littlefs/local/known_networks.json" ||
        p.endsWith("/wifi_credentials.enc") || p.endsWith("/app_permissions.json")) {
        return true;
    }

    return false;
}

void FileSystem::migrateSystemFiles() {
    // Migrate legacy stars_cache.json if present
    const char* legacyStars[] = { "/local/stars_cache.json", "/stars_cache.json" };
    for (const char* oldPath : legacyStars) {
        if (FileSystem::exists(oldPath)) {
            if (!FileSystem::exists("/local/system/stars_cache.json")) {
                FileSystem::copyFile(oldPath, "/local/system/stars_cache.json");
                Serial.println("[FileSystem] Migrated stars_cache.json to /local/system/");
            }
            FileSystem::deleteFile(oldPath);
        }
    }
}

