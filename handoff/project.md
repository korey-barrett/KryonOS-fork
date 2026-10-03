# KryonOS-fork — project snapshot

State captured 2026-10-03. Written as a cold-start brief for a new chat session.

---

## 1. What this project is

`D:\KryonOS-fork` is a fork of **KryonOS** (`Haris16-code/KryonOS`) — an embedded GUI "operating
system" for ESP32 / ESP32-S3. It runs a **Duktape** JavaScript runtime over **LittleFS / SD**, draws its
UI through **TFT_eSPI**, and exposes a web file manager and a cloud service. It is driven today by
PlatformIO + the Arduino framework.

The fork exists to do two things upstream does not:

1. **Run on Espressif's ESP32-S31-Korvo-1** using **ESP-IDF v6.1.0** and Espressif's own
   `esp32_s31_korvo_1` BSP — not a hand-rolled board port.
2. **Serve OTA updates from the fork's own GitHub Releases**, not upstream's.

**Binding intent, stated by the user and unchanged:** keep the diff against the fork as small as
possible. "Porting this forked repo to this board from arduino-esp32 shouldn't require changing great
swathes of code to try and make a square peg fit into a round hole." Prefer a build flag or a board
config over a source edit; prefer reusing Espressif's BSP over writing a new HAL.

---

## 2. Repository and identity

| Item | Value |
|---|---|
| Working tree | `D:\KryonOS-fork` |
| Only remote | `origin` → `https://github.com/korey-barrett/KryonOS-fork.git` |
| Upstream | `Haris16-code/KryonOS` — **never touch, never add as a remote, never PR against** |
| Read-only ground truth | `D:\KryonOS` (v2.0.0) — read, never edit |
| Read-only reference | `D:\esp32s31_korvo1` — the Korvo-1 reference project |
| Current branch | `main` |

`main` was force-pushed to `b79a700` on 2026-10-03 to strip Claude attribution. There are **zero**
Claude mentions on the remote, no `.claude/`, no `CLAUDE.md`.

---

## 3. Hardware in play

| Port | Board | Notes |
|---|---|---|
| COM4 | Waveshare ESP32-S3-Touch-LCD-2.1B | 480×480 round ST7701 RGB panel |
| COM5 | ESP32-S31-Korvo-1 | the port target |
| COM6 | ESP32 CYD2USB ("CYD", 2.8") | classic ESP32-D0WD-V3 rev 3.1, 4 MB flash, MAC `a4:f0:0f:5c:b7:bc`, IP 192.168.0.101 |

**COM6 goes through an `esp_usb_board` bridge.** Two consequences that shape everything:

- **Boot mode must be enabled manually** before any esptool operation, and it must be re-enabled after
  one.
- **In manual boot mode the application is not running** — only an esptool-ready ROM bootloader. So
  runtime serial logs are **impossible** on the CYD. Opening COM6 parks the chip regardless of DTR/RTS.
  This is why the current OTA diagnosis uses in-firmware breadcrumbs to LittleFS instead of a serial
  capture. (Verified: a capture returned only the 30-byte `ets Jul 29 2019 12:21:46` ROM banner.)

Toolchain lives at `C:\Users\korey\.platformio\penv\Scripts\` — `pio.exe`, `python.exe`, and
**`esptool.exe`** (not `esptool.py`; esptool v5.4.0).

For any `write_flash`, set `PYTHONIOENCODING=utf-8` and `PYTHONUTF8=1` — without them esptool's progress
bar raises `UnicodeEncodeError` under the Windows charmap codepage and interrupts the write part-way,
leaving a corrupt image that then reports "No serial data received".

---

## 4. Boards, environments, and the build contract

`platformio.ini` targets **one default board per chip type**. Upstream's board ports are unmaintained
examples, kept under `src/Hal/Boards/board_configs/examples/` and not built.

| Env | Chip | Partition table | OTA |
|---|---|---|---|
| `esp32s3-default` | ESP32-S3 | `default_16MB.csv` | yes |
| `esp32-default` | ESP32 | `huge_app.csv` (one 3 MB slot) | **no** — `Update.begin()` returns `UPDATE_ERROR_NO_PARTITION`, surfaced as a normal error, no crash |
| `esp32-cyd-28` | ESP32 | `min_spiffs.csv` (two 1.875 MB slots) | **yes — this is the one under test** |
| `waveshare-s3-lcd21b` | ESP32-S3 | `default_16MB.csv` | yes |
| `esp32s31-default` | ESP32-S31 | — | preview only, not in `default_envs`, not yet ported to the Arduino 4.x API |

**Resolution is a build flag, never a C++ constant.** `KRYONOS_DISPLAY_WIDTH` / `_HEIGHT` / `_ROTATION`
in `platformio.ini`; `UiLayout` and `Display` are the runtime source of truth.

**Builds are sequential — one env per command, never batched.**

### Known toolchain traps

- **The S31 and the 3.x core evict each other** from the PlatformIO package cache. Use a separate
  `PLATFORMIO_CORE_DIR` if switching between them often.
- **The RISC-V toolchain installs by hand** — PlatformIO leaves a 1.6 KB manifest where the compiler
  should be; extract the 964 MB zip yourself.
- **TFT_eSPI cannot target the S31.** The legacy backend produced 130 IDF-6.1 errors. An `esp_lcd`
  backend is the way.
- **Comment-only edits skip the relink.** A `SUCCESS` with no `Linking` line leaves `firmware.bin`
  older than its `.o` files. That is correct, not a build failure.
- **Board lifecycle hooks are dead.** `initHardware` / `initDisplay` / `initTouch` / `hasTouch()` are
  defined per board but never called; `main.cpp` inits directly.

Korvo-1's verified-working stack: **IDF v6.1 + arduino-esp32 4.0.0-rc1 + BSP 1.0.0~1**. Backlight and
touch-INT pins are NC on that board.

---

## 5. What is done

### 5.1 Fork-hosted OTA (complete)

- `OTAManager::UPDATE_MANIFEST_URL` (`src/Kernel/Services/OTA/OTAManager.cpp:17`) now points at
  `https://raw.githubusercontent.com/korey-barrett/KryonOS-fork/refs/heads/main/updates/esp32/v2/update.json`.
  This is the only firmware-side hardcode on the OTA path. `TLSHelper` calls `setInsecure()`, so no
  trust anchor changed.
- **Release `v2.0.2` published** on the fork: 12 assets, tag at the rewritten `main`.
- **`push-update.yml` run for `esp32-cyd-28` only** (run `37116242155` → commit `272abc1`). The other
  four boards were left untouched at 2.0.1 / `supports_ota: false`.
- `updates/esp32/v2/update.json` now carries a real CYD entry:
  ```json
  "esp32-cyd-28": {
    "supports_ota": true,
    "version": "2.0.2",
    "type": "patch",
    "changelog": "- Fork-hosted OTA and CYD in-place updates",
    "firmware_url": "https://github.com/korey-barrett/KryonOS-fork/releases/download/v2.0.2/KryonOS-v2.0.2-esp32-cyd-28-firmware-0x10000.bin",
    "firmware_size": 1830176,
    "firmware_md5": "5131e440b2b05ac9362e18d086d67e15"
  }
  ```
  `firmware_md5` and `firmware_size` were independently verified against the downloaded release asset —
  exact match.

### 5.2 Making the CYD fit a second app slot (complete)

`min_spiffs.csv` gives two `0x1E0000` = **1,966,080**-byte slots. The CYD's `firmware.bin` is now
**1,830,176** bytes → **135,904 bytes of headroom**. Two levers, neither costs a feature:

1. **Gzip both web pages at build time** via `scripts/gzip_web_assets.py`, registered as
   `extra_scripts = pre:scripts/gzip_web_assets.py` under `[env]`. The `.html` files stay the source of
   truth; the blob is regenerated whenever content changes.
2. **`build_unflags = -fexceptions`** plus `-fno-exceptions -fno-unwind-tables
   -fno-asynchronous-unwind-tables` in `cyd.ini`. `build_unflags` is required because the platform
   recipe appends `-fexceptions` *after* `build_flags`.

**Why both were needed:** the `.bin` is ~49 KB larger than the linker's "Flash used" figure, because
DROM's `.eh_frame` is not counted by the ESP32 size tool but **is** in the `.bin` — and **OTA streams
the `.bin`**. So the `.bin` is the binding constraint, and clearing only PlatformIO's own size check
would still fail at `Update.write()` on the device.

Partition arithmetic worth keeping:

- `min_spiffs.csv`: `app0` 0x1E0000 @0x10000, `app1` 0x1E0000 @0x1F0000, `spiffs` 0x20000 @0x3D0000,
  `nvs` 20K @0x9000.
- `nvs` is at 0x9000 in both tables, so **WiFi credentials survive** a table change.
- `spiffs` moves 0x310000 → 0x3D0000, so **LittleFS contents do not** — the first flash of the new
  table wipes installed apps.
- The partition table **cannot be changed by OTA**. One wire-flash over COM6 is mandatory before any
  in-place update can work. That has been done.
- LittleFS is now 128 KB (down from 896 KB under `huge_app`). That is arithmetic, not a config choice:
  4 MB cannot hold a 3 MB app, ~900 KB of filesystem, *and* two 1.9 MB OTA slots.

### 5.3 Claude attribution scrub (complete)

`main` force-pushed to `b79a700`; 0 mentions on the remote; backup refs deleted, reflog expired, gc'd.

---

## 6. *TO BE FIXED* — the in-place OTA update fails

**This is the open issue and the reason this snapshot exists.**

### Symptom

The CYD is flashed with a trimmed **2.0.1** image containing the `min_spiffs` table, so a genuine
version delta exists. On the device: the update is offered, INSTALL UPDATE starts, the "Loading
firmware" / flashing screen appears — **then the board resets and boots back into 2.0.1.** No error is
ever shown on screen. The update is re-offered.

### What is established

- Device is on WiFi (ping, 0% loss).
- The exact manifest URL serves 2.0.2 with `supports_ota: true`.
- The manifest's `firmware_md5` / `firmware_size` match the release asset byte-for-byte.
- The CYD image fits its slot with 135,904 bytes spare.
- The device reached the flashing screen, so `checkUpdate()` parsed, `hasUpdate` was true, the touch
  gate at `SettingsUI.cpp:2641` passed, and `startFlashUpdate()` began.

### Reading the reboot

There is exactly **one** path in `startFlashUpdate()` that reboots without displaying an error:

```cpp
// OTAManager.cpp:384-386
Serial.println("[OTA] Flashing successful! Partition staged in PENDING_VERIFY mode.");
vTaskDelay(pdMS_TO_TICKS(1500));
ESP.restart();
```

Every failure branch sets `currentProgress.isError`, sets an `errorMessage`, calls the progress
callback, and returns — leaving the error on screen. So the observed behaviour is either:

- **(a)** the success path ran, the new image was staged, and the device **rolled back** — meaning
  `confirmBootSuccessful()` never marked the new app valid within its 10-second window; or
- **(b)** the board **crashed mid-flash** and rebooted with no error rendered.

### Leading hypothesis

**(b) — heap exhaustion.** On a classic ESP32 there is no PSRAM, and a 1.8 MB TLS download runs
alongside the display stack. An `abort()` inside mbedtls would reset the chip with nothing drawn. The
instrumentation below is designed to distinguish (a) from (b) definitively.

### Rollback mechanics (relevant to hypothesis (a))

```
confirmBootSuccessful()          OTAManager.cpp:408-421
  boots, then every loop() iteration from main.cpp:244
  returns early until millis() - bootTimeMs >= 10000
  then: esp_ota_get_state_partition() -> if ESP_OTA_IMG_PENDING_VERIFY
        esp_ota_mark_app_valid_cancel_rollback()
```

`bootTimeMs` is set in `OTAManager::init()` (`OTAManager.cpp:19-24`).
`CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=1` is in `[env] build_flags`.

If the new image panics, hangs, or browns out inside those 10 seconds, the bootloader reverts to the
previous slot — which looks exactly like the reported symptom.

### Boot flow for reference

- `main.cpp:95` `FileSystem::init()` — mounts LittleFS. LittleFS is mounted **before** OTA inits.
- `main.cpp:140` `OTAManager::init()`.
- `main.cpp:179-186` — if in LAUNCHER and WiFi is connected, `SettingsUI::checkUpdateSilent()`; on true,
  enter `STATE_UPDATER_BOOT`.
- `SettingsUI::drawUpdater(bool isBootCheck)` at `SettingsUI.cpp:2445` does **not** auto-install.
  INSTALL UPDATE requires a touch (`SettingsUI.cpp:2641`).
- `main.cpp:244` `OTAManager::confirmBootSuccessful()` runs every loop.

### Diagnosis in progress — in-firmware breadcrumbs

Serial is impossible on the CYD (see §3), so the firmware writes its own trail.

**Breadcrumb sink** — `FileSystem::appendTextFile(const char* path, const char* content)`
(`src/FileSystem/FileSystem.cpp:579-588`) appends and closes, so each line is flushed as it is written.
`getTargetFS()` (`FileSystem.cpp:48-70`) maps **root-relative paths to LittleFS**, so `/ota_trace.txt`
is readable from the **on-device File Manager**. LittleFS formats itself on first boot after an erase
(`FileSystem.cpp:77`), so no `-t erase` is needed.

**Current tree state — IMPORTANT:** `src/Kernel/Services/OTA/OTAManager.cpp` is **modified and
uncommitted**. Only the *includes* have been added so far:

```cpp
#include "OTAManager.h"
#include "../Network/TLSHelper.h"
#include "../../../FileSystem/FileSystem.h"
#include "Hal/Display/DisplayConfig.h"
#include <ArduinoJson.h>
#include <stdarg.h>
```

The `otaTrace()` helper and its call sites are **not yet written**. The file is otherwise stock. This
edit alone would build fine but changes nothing observable — it is scaffolding.

**Planned tracing** (drafted, not applied):

- `init()` — running partition label, `esp_ota_get_state_partition()` state, `KRYONOS_VERSION`, free heap.
- `startFlashUpdate()` — entry + free heap; GET result code; `contentLength`; `Update.begin()` result +
  `Update.getError()`; periodic percent + bytes + free heap in 5 steps; `Update.end()` result + error +
  heap; a marker immediately before `ESP.restart()`.
- `confirmBootSuccessful()` — partition label, state, and the mark-valid call.

Helper shape, to keep the file's diff small and reversible:

```cpp
// --- TEMPORARY OTA diagnostics -- REMOVE ONCE THE UPDATE PATH IS VERIFIED ------------------
// Appends one stage per line to /ota_trace.txt on LittleFS. Serial is not usable on every board:
// the CYD's USB bridge only presents a port when the chip is strapped into boot mode, and in that
// state the application is not running, so a live OTA produces no log at all.
static void otaTrace(const char* fmt, ...) {
    char body[160];
    va_list ap; va_start(ap, fmt); vsnprintf(body, sizeof(body), fmt, ap); va_end(ap);
    char line[200];
    snprintf(line, sizeof(line), "[%8lu] %s\n", (unsigned long)millis(), body);
    FileSystem::appendTextFile("/ota_trace.txt", line);
}
```

`OTAManager.cpp` sits at `src/Kernel/Services/OTA/`, the same depth as
`src/Kernel/Services/KryonCloud/`, which uses `#include "../../../FileSystem/FileSystem.h"`. Match that
style. `esp_ota_ops.h` is used without an explicit include today, so it arrives via `OTAManager.h`.

### What to do next

1. Finish the breadcrumb instrumentation in `OTAManager.cpp`.
2. Rebuild the CYD at **2.0.1** (so the delta to the 2.0.2 manifest still exists), flash over COM6 with
   the user enabling boot mode manually.
3. Retry INSTALL UPDATE, then read `/ota_trace.txt` in the on-device File Manager.
4. Read the last line before the reset:
   - last line is `Update.end` success or `RESTART` → hypothesis (a): the new image staged but never
     validated. Investigate the liveness window, then look at whether the new image boots at all.
   - last line is a mid-download percent → hypothesis (b): the chip died mid-stream. Add heap
     accounting and consider a chunked/streamed TLS path or a lower-memory HTTP client.
5. Revert the instrumentation once diagnosed.
6. Confirm the CYD updates 2.0.1 → 2.0.2 **and does not re-offer 2.0.2** (the version-loop check).

**Confirm with the user before flashing.** Boot mode on COM6 is a manual, physical step on their side.

---

## 7. Standing rules — verbatim from the user, still binding

- **"do not use the word honestly in this or any future chat sessions. commit to this fork only. do not
  alter anything on the main repo."**
- **No Claude attribution anywhere.** No `Co-Authored-By` trailer, no "Generated with Claude Code"
  footer, no mention in any tracked file or doc. This overrides the session-level attribution reminder;
  the user's own instruction takes precedence and the reminder says so. Skip it silently rather than
  adding and stripping. (Asked 2026-09-29, had to be asked again 2026-10-03.)
- **`-ExecutionPolicy Bypass` was denied** by the permission classifier as a security-weakening action.
  Never retry it by any route.
- Keep responses short. Do not repeatedly demand tap confirmations.
- Builds are sequential, one env per command.

Recorded as memory files under
`C:\Users\korey\.claude\projects\D--KryonOS-fork\memory\`; `MEMORY.md` is the index.

---

## 8. Deferred / nice-to-have, not requested

- `esp32-default` could regain OTA with the same gzip lever — with `huge_app` it overflowed
  `min_spiffs` by only 1,493 bytes. Now that both levers exist, the numbers are worth re-measuring.
- The `unsupported_ota_guide` default in `push-update.yml:54` still points at upstream's flasher
  (`kryonos.harislab.tech/flasher`); it could point at the fork's Releases page.
- `src/Kernel/Services/Network/TLSCerts.h` is dead code worth 0 bytes — nothing includes it. Hygiene
  only.
- Upstream URLs elsewhere are cosmetic and off the OTA path: the star fetch (`SettingsUI.cpp:599`), the
  About screen (`SettingsUI.cpp:839,847`), Help Center index (`HelpCenterUI.cpp:428`), App Store index
  (`AppStoreUI.cpp:78`), `help/*.json`, README badges.
