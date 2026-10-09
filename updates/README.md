# Remove the SD card before an in-place (OTA) update

This is the `esp32s3` branch of [korey-barrett/KryonOS-fork](https://github.com/korey-barrett/KryonOS-fork).
It serves this chip variant's update manifest at `updates/esp32s3/v2/update.json`, which is where
a device on this branch reads it from.

## The workaround

**Remove the SD card from the board before starting an update.**

A mounted SD volume is allocated out of the same internal heap the updater needs. Arduino's
`UpdateClass::begin()` allocates a 4096-byte sector buffer at a point where the download's TLS
session to the firmware server is already open. When that allocation fails the updater reports it
as `Err #0`, because that path leaves `_error` at `UPDATE_ERROR_OK` -- so the device shows

```
Flash Init Failed: Err #0
```

and nothing in the message says why. The update cannot start.

Measured on `esp32-cyd-28` (classic ESP32, 4 MB flash, `min_spiffs`), KryonOS 2.0.1:

| SD card | Result |
| --- | --- |
| inserted | `Update.begin()` fails: `Flash Init Failed: Err #0` |
| removed | the same firmware updates in place and boots onto 2.0.2 |

Nothing else changed between those two runs.

## This may not be specific to this fork

`src/Kernel/Services/OTA/OTAManager.cpp` on this fork is upstream's file with a single expression
changed -- the manifest URL. The allocation that fails is in the Arduino core, and the ordering
that places it after the TLS session is upstream's. The same failure may therefore occur in the
upstream repository. That has not been tested here.

## Status

**Remove the SD card from the board before starting an update.** That is the solution, and it is what
this page exists to say. The fork's storage layer also releases the SD volume for the duration of a
flash update (`FileSystem::suspendSD()` / `resumeSD()`, called around `startFlashUpdate()` in
`src/Settings/SettingsUI.cpp`), but taking the card out first costs nothing and removes the variable.
