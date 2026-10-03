#ifndef FILE_SYSTEM_H
#define FILE_SYSTEM_H

#include <Arduino.h>
#include <FS.h>
#include <LittleFS.h>
#include <SPI.h>
#include <SD.h>

struct FileEntry {
    String name;
    String path;
    bool isDir;
};

class FileSystem {
public:
    static bool init();
    static File openFile(const char* path, const char* mode = "r");
    static String readTextFile(const char* path);
    static bool writeTextFile(const char* path, const char* content);
    static bool exists(const char* path);
    static int listDir(const char* dirPath, String* resultFiles, int maxFiles);
    static int listDirectory(const char* dirPath, FileEntry* entries, int maxEntries);
    static bool copyFile(const char* srcPath, const char* dstPath);
    static bool copyDirectory(const char* srcDir, const char* destDir, void (*progressCb)(int current, int total) = nullptr);
    static int countFilesInDir(const char* dirPath);
    static String parseJsonValue(const String& json, const char* key);
    static bool deleteFile(const char* path);
    static bool formatLittleFS();
    static bool readCalData(uint16_t* calData);
    static bool writeCalData(uint16_t* calData);
    
    // Directory Operations
    static bool mkdir(const char* path);
    static bool rmdir(const char* path);
    static bool isDirectory(const char* path);
    static bool isFile(const char* path);
    
    // Advanced File Operations
    static bool appendTextFile(const char* path, const char* content);
    static bool renameFile(const char* pathFrom, const char* pathTo);
    static size_t getFileSize(const char* path);
    static time_t getLastModified(const char* path);
    
    // Metrics
    static size_t getTotalSpace(const char* drive);
    static size_t getUsedSpace(const char* drive);
    static size_t getFreeSpace(const char* drive);
    
    // Security & Protected Paths
    static bool isSystemPath(const char* path);
    static void migrateSystemFiles();
    
    // Cryptography
    static String getFileMD5(const char* path);
    
    // Mounting/Formatting
    static bool isSDMounted();

    // The filesystem an "/sd/..." path must be opened on: the SPI `SD` object on most boards, but
    // SD_MMC on a board whose slot is SDMMC (the Waveshare 2.1B). Code that hardcodes `&SD` there
    // opens an empty, never-mounted filesystem, so those paths appear to succeed and go nowhere.
    // Falls back to `&SD` when nothing is mounted, which fails to open exactly as a literal `&SD`
    // would -- so a caller that does not check isSDMounted() first still cannot null-deref.
    static fs::FS* sdVolume();
    static bool mountSD();
    static void unmountSD();
    static bool formatSD();

    // Drops the SD volume for the duration of an in-place flash update and puts it back afterwards.
    // A mounted FATFS volume is not free: the VFS entry, the FATFS object and the card's file
    // allocations all come out of the same internal heap the updater needs. Arduino's
    // UpdateClass::begin() allocates its 4096-byte sector buffer at a point where the download's
    // TLS session is already up, and reports a failed allocation as "Err #0" -- that path leaves
    // _error at UPDATE_ERROR_OK, so the message names no cause. Without the card mounted that
    // allocation succeeds; with it mounted it does not. The volume is not needed while flash is
    // being written, so it is released rather than left to compete for the heap.
    static void suspendSD();
    static void resumeSD();
};

#endif // FILE_SYSTEM_H
