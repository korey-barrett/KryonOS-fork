# Changelog

All notable changes to KryonOS are documented in this file.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.0.0/), and this project adheres to [Semantic Versioning](https://semver.org/).

---

## Categories Legend
- **Added**: New APIs, hardware modules, board targets, or user-facing features.
- **Changed**: Modifications to existing APIs, core behavior, or configurations.
- **Deprecated**: Features or APIs that still work but shouldn't be used for new code.
- **Removed**: Features or APIs that have been completely removed.
- **Fixed**: Bug fixes, stability improvements, or resolved hardware conflicts.
- **Breaking**: Incompatible architectural changes requiring modifications in existing apps or configurations.

---

## [Unreleased]

### Changed
- **The Waveshare 2.1B's UI is 1.2x larger.** Its canvas moves from 240x320 to **201x268** and its
  aperture scale from 6/5 to **96/67**, so the same 288x384 rect lands on the glass with everything
  inside it magnified: body text 19 px -> 23 px, list rows 36 px -> 43 px, header and footer bars 36 px
  -> 43 px tall, footer UP/DN 36 px -> 43 px tall, and the launcher list falls from 7 rows to 5.
  - Two build flags plus two constants carry the enlargement — `KRYONOS_DISPLAY_WIDTH`/`_HEIGHT` in
    `platformio.ini` and `SCALE_NUM`/`SCALE_DEN` in `EspLcdRgbDisplay.h`. **No screen's layout code is
    touched by that**, because a 3:4 canvas puts the blitted rect on 288x384 at *every* size and
    `uiScale(201, 268)` is still 1. A smaller canvas magnifies the UI rather than shrinking it. The
    shorter canvas does break one screen that sizes itself off a 320 px height, though — see
    KryonCloudUI under Fixed below.
  - 201 is the narrowest canvas the screens tolerate: `InstallerUI`'s three-button row spans
    `3*60 + 2*10 = 200` px and `UiLayout::dialogButtonSpaced()` centres that run, so the outer two
    buttons hang off both edges below it. Narrowing further means narrowing those call sites first.
  - 268 keeps every `h < 240` fallback on its normal path — the 42 px notification card, the OTA title
    font and the keyboard chrome. It does cross KryonCloudUI's own threshold, which keys off 320.
- **The on-screen keyboard keys grow with it, via a board-declared compact chrome**
  (`KRYONOS_KB_COMPACT_CHROME`). Key width on the glass is `288 / columns` and is therefore fixed at
  48 px for this board's 6 columns, but key height is `(h - kbGridTop) / rows`, and the chrome's
  fixed 110 px stack does not shrink with the canvas — at 268 tall it would have left the six rows
  26 px each. Claiming the layout's existing compressed chrome (grid top 72) gives them 32 px, i.e.
  **47x46 px keys against the previous 48x42**, instead of 47x37. The flag is 0 on every other board,
  so the `h < 240` test compiles to the identical expression there.

### Fixed
- **The KryonCloud screens no longer squeeze their text out of its frames on a canvas shorter than
  320 px.** `KryonCloudUI` lays its cards out in 240x320 literals and scales them to the canvas, but
  the scale was a *quantized half step* — `2 * h / 320`, which is 2 at 240x320 and 3 at 800x480 — so
  for any canvas under 320 it fell to 1 and put every frame at **half** height, while the glyph it
  holds stayed a fixed 16 px cell. `cvh()`'s floor, which exists to prevent exactly that, was
  `2 * m.scale` (2 px) rather than a glyph, so it never caught it. On the 201x268 canvas that left a
  22 px pill 11 px tall and a 20 px toast 10 px tall around 16 px type — read on the panel as lines
  piled on top of each other. `cv()` is now proportional (`v * h / 320`, capped at `v * m.scale`) and
  `cvh()`'s floor is the reference layout's own smallest font-2 frame, 20 px. Both are bit-identical
  at 240x320 and at the Korvo-1's 800x480 (`v` and `v * 3/2` respectively), so nothing but a sub-320
  canvas changes.
- **Three right-hand columns in the KryonCloud message, file and stream cards are anchored to the
  card's right edge** instead of its left. They hold fixed-width font-2 strings, so the room they
  need does not change when the card does, and a bare `card.x + 162` ran off a 187 px card that is
  224 px at the reference width. The offsets are unchanged there.
- **`tools/preview/preview.py` rendered every board with the shared 12x4 keyboard grid**, including
  the Waveshare, whose grid is 6x6 — so the one place the keyboard is easiest to eyeball drew keys
  the board does not have. It now passes the env's `KB_SHAPE`, as `layout_model.compute()`'s own
  contract requires.
- **`test_layout.py`'s board list was stale**: it still asserted `esp32-cyd-28` was an unbuilt
  example, but `board_configs/cyd.ini` had been promoted beside `platformio.ini` so the CYD's OTA
  slots could be built from the repo, making it a real env. The assertion, and the test run as a
  whole, now pass.
- **Every filled round rect on the Waveshare 2.1B gets its right-hand corners back.** `cc0a62a`
  fixed `fillRoundRect()`'s corner fill in `RamFramebufferDisplay` — `circleHelper()`'s filled
  branch anchors each chord on the circle's centre column, which is correct for a whole circle but
  not for a quadrant, so the outer half of each right-hand corner square went unpainted — but
  `EspLcdRgbDisplay`, written later for this board, copied the old construction. `drawRoundRect()`
  paints the correct arc on top, so it reads as two short marks at the top and bottom right of
  every filled round rect: a hollow link on each keypad key, a notch in the launcher's red exit
  button. The corners are now filled row by row with the exact chord, as the other backend does.
  This is the artifact reported against the Korvo-1, on the one backend that never got that fix.
- **The Time & Region screen's UP and DN footer keys work.** Its main branch handled only
  `UI_FOOTER_SEL`, so the two keys the page draws for itself did nothing. `drawTimeSettings()`
  renders UP / BACK / DN whenever `actionCount > timeBtnPerPage()`, and on the 201x268 canvas that
  is four actions against a page of two — while at 240x320 all four fitted, the footer was a plain
  BACK, and the missing cases never showed. `timeActionScroll` was already declared, drawn from and
  clamped; nothing could move it. The branch now handles all three thirds, as the timezone picker
  beside it already did.
- **The Waveshare 2.1B's RGB scanout no longer flickers.** Thin white lines travelled up and down the
  screen — at first on a still page, then on every screen once the App Store had been opened — and a
  clean reflash seemed to clear them, which is why they were first read as boot state rather than a
  timing bug. They are neither. The prebuilt Arduino libs are built with
  `CONFIG_LCD_RGB_RESTART_IN_VSYNC=y`, which compiles `lcd_rgb_panel_try_restart_transmission` down to
  an unconditional `do_restart = true` on **every** VBlank and drops the `bb_eof_count <
  expect_eof_count` desync guard below it (`esp_lcd_panel_rgb.c:1153`), so the GDMA channel is torn
  down and restarted once per field, always. Espressif's own comment on that path says a late
  interrupt makes the display "shift as the LCD controller already read out the first data bytes, and
  resetting DMA will re-send those" — and the restart re-mounts its buffer with a fixed 17-pixel
  `restart_skip_bytes` that is only correct if it lands where the driver assumes. That is what turns
  a miss into a thin band of wrong pixels rather than ordinary tearing.
  - The window is `vsync_pulse_width + vsync_back_porch`. At the vendor 3/8/8 that is 11 of 499 lines,
    **~377 us** at 16 MHz, while the bounce-refill ISR that keeps the panel fed `memcpy`s 9600 bytes
    out of PSRAM every ~300 us (`esp_lcd_panel_rgb.c:913`) — the same PSRAM WiFi, mbedTLS and every
    LittleFS write are using, and which a flash write stalls outright. The margin was thin at boot and
    gone under load, which is why opening the App Store made it permanent.
  - `vsync_back_porch` is now **32**, giving 35 lines (~1.2 ms) at 55.8 Hz instead of 58.5. It is the
    one timing value that departs from the vendor sequence; the panel is DE-mode and takes the longer
    blanking. Confirmed on hardware.

---

## [2.0.2] - 2026-10-03

### Added
- **Multi-Board Display Abstraction (`KryonDisplay`)**:
  - The UI now draws through an abstract `KryonDisplay` reference instead of a TFT_eSPI object, so the panel driver is a build-flag choice (`KRYONOS_DISPLAY_BACKEND`). `tft` is now `extern KryonDisplay&`.
  - `TftEspiDisplay` is the default adapter; it derives from both TFT_eSPI and `KryonDisplay`, so the concrete object keeps the whole TFT_eSPI API (sprites, DMA, `readRect`) while satisfying the interface.
  - `RamFramebufferDisplay` is a reference software rasterizer sharing no code with TFT_eSPI, which is what proves the seam. It embeds no font, so text is measured but not painted.
- **RGB Parallel Display Backend for the Waveshare ESP32-S3-Touch-LCD-2.1B** (`KRYONOS_BACKEND_RGB`):
  - Drives a 480x480 ST7701 on a 16-bit RGB565 parallel bus — a panel TFT_eSPI cannot drive — keeping TFT_eSPI compiled as a pure software rasterizer into a full-screen PSRAM sprite that is blitted through `esp_lcd_panel_draw_bitmap`. This keeps the glyphs, metrics and `UiLayout` geometry pixel-identical to the SPI boards.
  - The panel is a **circle**, so the logical canvas (240x320) is scaled 6/5 into a 288x384 rect whose corners land exactly on the bezel; `BOARD_PANEL_W`/`_H` in the board header holds the scan size the ST7701 timings come from.
  - The TCA9554 I2C expander is extracted into `src/Hal/I2C/Tca9554` and shared by the display and touch paths, with a write shadow that only advances on ACK.
  - **Preview:** this backend has never run on hardware. Its pins, timings, init table and expander masks were checked field-for-field against the vendor's `board_devices.yaml` and the Waveshare wiki, which agree independently.
- **Pluggable Touch Drivers (`ITouchDriver`)**:
  - The single touch class becomes a facade over `Xpt2046BitbangDriver`, `Xpt2046TftDriver`, `Ft6236Driver`, `Gt911Driver`, `Cst816Driver` and `NullTouchDriver`, selected by `KRYONOS_TOUCH_DRIVER` (unset is `auto`, which reproduces the previous compile-time ladder exactly). Naming a driver whose pins a board does not have falls back to null rather than reading unconnected pins.
  - `TouchCalibration.h` holds the calibration tuple and the raw-to-pixel math in an Arduino-free header, so the firmware and the off-device preview tool share one mapping.
  - `needsCalibration()` gates the calibration screen, so an absolute-position or absent panel skips it and `/touch_cal_p.bin` is never consulted.
  - The capacitive drivers are register-level and **untested against hardware**. The Waveshare's CST820 answers to the CST816S register map, so it binds `Cst816Driver` unchanged — but that panel has not been run either.
- **Arbitrary-Resolution Canvas and `UiLayout`**:
  - Panel size and orientation are build-time configuration (`KRYONOS_DISPLAY_WIDTH` / `_HEIGHT` / `_ROTATION`, with `DISP_HOR_RES` / `DISP_VER_RES` honoured as aliases) rather than a constant; `Display` is the runtime source of truth, snapshotted after rotation.
  - `UiLayout::compute(w, h)` returns the frame, header, list, footer, scrollbar, dialog, keyboard and notification metrics every screen draws from. `compute(240, 320)` reproduces the historical geometry exactly, and golden tests pin it.
  - Every screen — Launcher, App Store, Installer, Help Center, Settings, keyboard, notifications, Web Server, KryonCloud and the boot path — derives both its draw rects and its touch hit rects from those metrics, so a tap hits what was drawn at any resolution.
  - `uiScale(w, h)` = `min(w, h) / 240` clamped to `[1, 3]` scales the whole layout to a larger panel, and `KRYONOS_KB_*` lets a board declare its own keyboard grid shape (key width is fixed by the column count, so finger-sized keys need fewer, wider columns rather than scaling).
  - `tools/preview` renders the same layout model off-device to HTML/SVG.
- **ESP32-S31-Korvo-1 Board Profile**:
  - The board builds and runs on the **upstream stack — PlatformIO with arduino-esp32** — with no IDF build system and no BSP. TFT_eSPI cannot compile for this chip at all, so the 800x480 RGB panel is driven through `esp_lcd` by `KorvoRgbDisplay`, with Espressif's verified pin and timing values vendored into it from `esp32_s31_korvo_1` and marked at their source rather than the BSP being linked.
  - Touch is the GT1151 — the part reports itself as a GT1158 — brought up at register level over I²C on GPIO 0/1. Two of the board's pins are NC, and both change a decision: the backlight pin is `GPIO_NUM_NC`, so `setBacklight()` records the request and reports that it is always on rather than pretending (a dark panel is never misread as a dim one), and the touch interrupt is `GPIO_NUM_NC`, so the driver polls.
  - The IDF build still exists and still uses the BSP. `src/` carries the board for both, so the two build systems cannot disagree about what the board is.
- **ESP-IDF v6.1 Build for the ESP32-S31**:
  - The S31 is only reachable from arduino-esp32 4.0.0-rc1, which is IDF 6.x, which the stable pioarduino `espressif32` platform does not carry — so the board also gets a CMake project of its own under `idf/`. `platformio.ini` and its other environments are untouched and still build.
  - `kryonos_app` is the only place the fork's `src/` tree is listed, so the IDF and PlatformIO builds cannot disagree about what the project is. `kryonos_s31_display` supplies the rasterizer the BSP does not (it hands back a panel handle and `esp_lcd_panel_draw_bitmap` and stops there).
  - AsyncTCP, ESPAsyncWebServer and WebSockets are vendored as IDF components at the same versions the PlatformIO environments resolve, so the two build systems cannot drift onto different sources of the same library.
  - Font data is vendored verbatim from TFT_eSPI so the Korvo-1 renders text pixel-identical to the other boards and `textWidth()` reports the same numbers.
- **Fork-Hosted OTA**:
  - The firmware's OTA manifest URL now points at the fork's per-variant branches (`esp32`, `esp32s3`, `esp32s31`) instead of upstream, and release **v2.0.2** is published from the fork. Previously every board in the fork's manifest was `supports_ota: false`, so no in-place update path existed anywhere.
- **CYD In-Place OTA**:
  - `esp32-cyd-28` moves to `min_spiffs.csv` — two 1,966,080-byte app slots — and becomes the only 4 MB target here that updates over the air. The app fits via two levers, neither sufficient alone: both embedded web pages are gzipped at build time by `scripts/gzip_web_assets.py`, and the never-taken DWARF unwind tables are stripped. `firmware.bin` lands at 1,830,176 bytes, 135,904 under the slot, with no feature gated off.
- **Off-Device Layout Preview (`tools/preview`)**:
  - Renders the `UiLayout` model to HTML/SVG without hardware, with golden tests pinning 240x320 to the legacy values.

### Changed
- **Cryptography moved to PSA Crypto**: MD5, SHA-256, SHA-512, HMAC-SHA256 and AES-CBC now go through PSA, so there is one implementation across IDF 5.x and 6.x rather than a per-core split. mbedTLS 4.x (IDF 6.1) moved the per-algorithm headers under its private directory. AES-CBC uses the multipart API with an explicit `psa_cipher_set_iv()`, because the one-shot would generate its own random IV and prepend it, leaving the IV stored in the JSON envelope unused and every decrypt failing. Every failure path returns empty rather than a partial result, and the cloud download's checksum fails **closed**.
- **One default board per chip type** (`esp32-default`, `esp32s3-default`, `esp32s31-default`). Upstream's CYD / T-HMI / Cardputer ports move to `src/Hal/Boards/board_configs/examples/` as unbuilt reference snippets. Board guards are now positive `TARGET_*` macros.
- **SD access routes to the volume the card is actually mounted on** rather than always the SPI `SD` object, which a board with an SDMMC slot never mounts — its paths previously "worked" by failing quietly, and the app scan reported an empty card on a card that was present.
- **The SD pin map is keyed off the board, not the chip.** `CONFIG_IDF_TARGET_ESP32S3` is defined for every ESP32-S3 build, so the generic map attached FSPI to the Waveshare's LCD VSYNC / DE / PCLK pins and tore down the RGB timing moments after the panel came up.
- **Wi-Fi scans are capped at 15 seconds** instead of Arduino's 60-second `_scanTimeout` default. `smartAutoConnect()` scans synchronously and is called from both `WiFiManager::init()` and the Settings touch handler, so the default was the difference between a few seconds and a pinned UI with no way out but reset.
- **The board id has one source of truth** (`KRYONOS_BOARD_ID`) across the firmware, CI and the OTA manifest, which previously named boards that no longer exist.

### Fixed
- **Frames drawn inside blocking calls now reach the panel.** `main.cpp::loop()` was the only thing that flushed a frame, so anything drawn by a callee that then blocked — a load stage, scan, fetch, install or OTA download — arrived only as whatever cache lines happened to be evicted, showing as horizontal streaks over the text.
- **Filled round rectangles no longer lose their right-hand corners in the RAM framebuffer backend.** `fillRoundRect()` filled its corner squares through `circleHelper(filled=true)`, which anchors every chord on the circle's centre column; that is right for a whole circle but not for a quadrant, so the outer half of each corner went unpainted.
- **Taps land where they were aimed on a scaled panel.** Once the canvas is scaled into the aperture, raw panel coordinates are no longer canvas coordinates and every tap landed at 5/6 of its target, drifting further from the centre — easy to misread as a calibration fault, since the centre of the screen still worked.
- **The Korvo-1's RGB panel rasterizes into the panel's own frame buffer** instead of a bounce buffer. In bounce mode the driver tracks its place in the frame in a software counter that only advances when a refill interrupt beats the DMA; miss that deadline once — a flash write or a WiFi burst is enough — and every later slice is offset permanently, silently, and differently on every boot. Refresh-auto mode restarts the DMA at row 0 each frame, so the scanout position lives in hardware and a stall can corrupt at most one frame. It also removes a second full-size copy of the canvas, at the cost of an `esp_cache_msync` in `present()`.
- **Frames grow with the text on larger panels**, and the whole layout scales with the panel rather than only its proportions.
- **OTA on the CYD no longer fails with `Flash Init Failed: Err #0`.** Arduino's `UpdateClass::begin()` allocates a 4096-byte sector buffer after the download's TLS session is already open, and reports a failed allocation as `Err #0`, which leaves `_error` at `UPDATE_ERROR_OK`. A mounted SD volume is allocated from the same internal heap and is enough to push it over. `FileSystem::suspendSD()` / `resumeSD()` release the volume around the flash; **until that is confirmed on hardware, removing the SD card before an update stays the reliable workaround.**

### Breaking
- **The CYD's partition table changes** from `huge_app.csv` to `min_spiffs.csv`. Two one-time consequences follow, both because OTA cannot deliver a partition table: LittleFS drops from 896 KB to 128 KB, and the first wire flash orphans its contents — including `/local/web_on.txt`, so the web server toggle resets itself. The CYD must be flashed over USB once before in-place updates work at all. `nvs` stays at `0x9000` in both tables, so WiFi credentials and KryonCloud pairing survive.

---

## [2.0.1] - 2026-10-01

### Added
- **Dynamic TLS Build Epoch Time Sync**:
  - Implemented compile-time build epoch baseline (`getBuildEpoch()` from `__DATE__`/`__TIME__`) setting the system clock on boot if uninitialized, safely satisfying `notBefore` certificate validation.
  - Added non-blocking SNTP synchronization (`configTime()`) in background upon Wi-Fi connection.
- **Settings Permissions Manager**:
  - Added dedicated **Permissions Manager** in Settings with paginated app list, permission status badges, individual **`[ Revoke ]`** buttons, and global **`[ Reset All ]`** confirmation dialog.
- **On-Demand Runtime Storage Permissions & Cross-Storage Sandboxing**:
  - Replaced pre-install permission barriers with just-in-time on-demand runtime security: Apps install directly with zero friction.
  - Symmetrical bidirectional sandboxing: Apps running from LittleFS (`/local`) accessing SD (`/sd`) or external paths, and apps running from SD (`/sd`) accessing LittleFS (`/local`) or external paths trigger the native interactive modal (`Allow Once`, `Always Allow`, or `Deny`).
  - Access to internal files within the app's own directory is always granted without prompts.
- **Compact Settings UI Layout**:
  - Reorganized Settings Menu with 7 crisp `200x30` px buttons (`WiFi Options`, `Touch Calibrator`, `Manage Apps`, `Permissions Manager`, `Time & Region`, `About Device`, `System Updates`).
- **Web Manager Session Security, Cookie Auth & Rate Limiting**:
  - Replaced basic auth prompts with a modern glassmorphic web login interface, session token management (epoch time expiry), and `SameSite=Strict` HTTP cookie authentication.
  - Added persistent **"Remember Me"** login toggle extending session validity (10-year cookie expiration) until the user explicitly logs out.
  - Brute-force rate limiting: 30-second lockout after 5 consecutive failed login attempts.
  - Dedicated `/api/login` and `/api/logout` endpoints.
- **Recursive Folder & Drag-and-Drop Uploads**:
  - Added full drag-and-drop file and folder upload engine with client-side recursive directory extraction (`webkitGetAsEntry` / `FileSystemDirectoryReader`) and seamless parent folder creation (`ensureParentDirectories()`).
  - Added dedicated `Upload Folder` button with `webkitdirectory` support.
- **Comprehensive Online Help Center & Automated Workflow**:
  - Structured 8 detailed categories covering Setup, Wi-Fi, Web Server, App Store, Permissions, KryonCloud & AI, Settings, and FAQs.
- **On-Device Web Server Customization**:
  - Added `[ Set User ]` and `[ Set Pass ]` on-device keyboard controls in `WebServerAppUI` allowing users to change web credentials on the go.
  - Renamed UI to "Web Server".
- **Hardware-Protected WiFi Credential Storage (AES-256-CBC)**:
  - Network credentials are now encrypted on flash at `/local/system/wifi_credentials.enc` using hardware AES-256-CBC with PKCS#7 padding.
  - Unique per-device key generated using ESP32 Hardware True Random Number Generator (`esp_fill_random`) on initial boot and isolated in dedicated NVS partition (`kryon_sec`).
  - Seamless automatic boot migration: Legacy cleartext `known_networks.json` and `wifi.txt` files are decrypted/parsed, saved to encrypted storage, and securely purged from filesystem.
  - Encrypted cloud backup integration: Device backups seamlessly bundle and restore the encrypted credential vault.
- **TLS Certificate Validation & Mozilla CA Trust Bundle**:
  - Implemented centralized `TLSHelper` and `TLSCerts` trust store (`ISRG Root X1`, `DigiCert Global Root G2`, `Amazon Root CA 1`, and ESP-IDF Mozilla CA bundle fallback).
  - Complete elimination of all 25 `setInsecure()` / `setTrustAnchors()` bypasses across the entire operating system codebase.
- **Web Manager HTTP Basic Authentication & Protected Path Guard**:
  - Authentication enforced across all Web Manager `/api/*` endpoints and web editor root (`/`).
  - Web Manager credentials persisted in isolated NVS (`kryon_web`) and viewable directly on the device screen when Web Server is active.
  - System path guard strictly blocking traversal (`..`, `\`) or access to `/system/`, `/local/system/`, `/sd/system/`, and internal security vaults via Web Manager.
  - Direct raw streaming `POST /api/save` endpoint preventing heap exhaustion / OOM when saving large files.
- **Cryptographic App Store Package Integrity Verification**:
  - App Store downloads now verify package integrity via SHA-256 streaming hash comparison before installing app code or metadata.
- **Scoped JavaScript Filesystem Sandboxing**:
  - JavaScript applications are restricted strictly to their package folder (`/local/apps/<pkg>/` or `/sd/apps/<pkg>/`) or explicitly configured data directory.
  - Hard sandbox with immediate rejection for path traversal (`..`, `\`), absolute escapes, or out-of-sandbox filesystem operations, preventing FreeRTOS watchdog freezes.
- **Privacy & Telemetry Hardening**:
  - Eliminated automatic boot and settings-open HTTP pings to GitHub API (`stargazers/count`).
  - Star count telemetry is strictly opt-in and cached locally under `/local/system/stars_cache.json`.

### Changed
- **Web Manager CORS Policy**: Removed wildcard `Access-Control-Allow-Origin: *` header to prevent Cross-Origin LAN exploitation.
- **File System Architecture**: Created reserved `/system` folder on LittleFS and SD partitions for OS-internal configuration and security vaults with system-level path protection.
- **App Store Status & Security Feedback**:
  - "Check for Updates" screen now displays `No Update found.` when all installed applications are on their latest version.
  - Package integrity verification now displays `Installation Blocked: Hash Not Found in store.` when a repository catalog entry is missing a SHA-256 hash.

### Fixed
- **JavaScript Engine RAM & Display Buffer Deallocation**:
  - Fixed heap retention and frame buffer memory leak on JavaScript app termination by explicitly deleting `tftSprite` and `sprite3D` buffers in `JSBindings::cleanup()`, immediately freeing up to 153.6 KB of RAM and resetting all display caches.
- **CVE Fixes / Security Vulnerability Resolution**:
  - Fixed plain-text credential disclosure via Web Manager download endpoint.
  - Fixed remote unauthenticated code execution and storage deletion via Web Manager.
  - Fixed TLS MITM vulnerability across KryonCloud, KryonAI, OTA Manager, and Network clients.
  - Fixed untrusted package execution in App Store by enforcing SHA-256 verification.
  - Fixed arbitrary filesystem read/write vulnerability in JavaScript Duktape runtime via hard sandboxing.
  - Fixed Web Manager file descriptor leak on client disconnections during streaming uploads.

---

## [2.0.0] - 2026-09-27

### Added
- **New Boards Added**:
  - Expanded device compatibility beyond the initial prototype with dedicated hardware abstraction profiles, display drivers, and pin mappings under `src/Hal/Boards/`.
  
  - **ESP32-S3 DevKitC-1 N16R8 (`esp32-s3-devkitc-1-n16r8`) [Fully Tested And Verified]**:
    - Full tier-one support as the default flagship development board.
    - 16MB Flash memory setup with dual-OTA support and custom partition layout.
    - 8MB Octal PSRAM (`qio_opi`) enabled for fluid UI rendering and expanded JavaScript heap runtime.
    - Native USB-CDC serial and hardware JTAG debugging support (`ARDUINO_USB_CDC_ON_BOOT=1`).

  - **M5Stack Cardputer v1.1 (`m5stack-cardputer`) [EXPERIMENTAL]**:
    - 56-Key physical matrix keyboard driver scanning via 74HC138 demultiplexer with complete Shift and Fn modifier layer decoding.
    - ST7789V2 SPI 240x135 landscape display integration.
    - Internal ADC-based battery voltage and charge percentage telemetry.
    - Dedicated 8MB dual-OTA partition scheme (`partitions_cardputer.csv`).

  - **LilyGO T-HMI (`lilygo-t-hmi`) [EXPERIMENTAL]**:
    - High-speed 8-bit parallel ST7789 display bus driver with hardware BGR color correction.
    - Integrated XPT2046 resistive touch controller with calibration routines.
    - 1-bit high-speed SD_MMC storage mount.
    - Onboard battery voltage telemetry.

  - **ESP32-CYD-28 / Cheap Yellow Display (`esp32-cyd-28`) [EXPERIMENTAL]**:
    - ILI9341 SPI display driver with dedicated touch SPI bus.
    - Built-in RGB notification LED and onboard LDR light sensor support.

  - > **Note on Experimental Boards:** Target boards marked as `[EXPERIMENTAL]` are implemented at the driver and HAL level but currently lack hands-on verification due to unavailable test hardware. The **ESP32-S3 DevKitC-1 N16R8** remains the sole fully verified, physical bench-tested device. Community members with these experimental boards are encouraged to test builds and submit calibration feedback.

  - **Support Hardware Acquisition & Testing**:
    - To help purchase these test boards, verify hardware in-person, and expand native device support, consider backing the project on [GitHub Sponsors](https://github.com/sponsors/Haris16-code). Sponsorships directly fund development hardware, display panels, and ongoing maintenance.
    - Offers one-time contributions, monthly recurring tiers, and custom support amounts.

  - **Modular Build Configurations**:
    - Board environments are decoupled into dedicated `.ini` files under `src/Hal/Boards/board_configs/` (`cardputer.ini`, `cyd.ini`, `t_hmi.ini`) and imported automatically into `platformio.ini` using `extra_configs`.
- **KryonOS Cloud Platform & Account Integration (`KryonCloud`)**:
  - Full native C++ system application `KryonCloud` located under launcher item `[ SYSTEM ]` apps.
  - **Fresh Server Synchronization on Open**: Automatically pings the server for live quotas, and KryonBeam inbox every time the app is launched.
  - **Standardized 5-Tab Navigation Layout**:
    1. **Overview**: Account profile, hardware handle (`@handle`), active WiFi status, quick summary cards, and unpair button.
    2. **KryonAI Studio**: Interactive AI with on-screen touch keyboard (`MyKeyboard`) prompt input, real-time SSE streaming, and strictly bounded word-wrapping preventing screen edge overflow. We also added new JS API's in this version so you can also use KryonAI in your JS Apps
    3. **KryonBeam**: Hardware-to-hardware mesh messaging with inbox threads, detail modal, on-screen keyboard composer, recipient handle/broadcast selector (`*`), message , and queue purge acknowledgment. You can also message in the Public group.
    4. **Cloud Storage**: File browser for `/cloud/shared` and `/cloud/device` scopes with SHA-256 verified downloading and background chunked uploading. Currently we add feature in Cloud Storage that you can take backup of your device and it will upload to cloud storage and you can able to restore it. Also you can use KryonCloud from the Kryon Account web dashboard.
    5. **Usage & Quotas Dashboard**: Dedicated telemetry dashboard with daily AI request progress bars, cloud storage allocation bars, 00:00 UTC quota reset countdown, and verified node health badge.
- **Automatic OTA (Over The Air) Updates**:
  - Devices with large flash memory (8MB+) can now update KryonOS completely over Wi-Fi.
  - **Full OTA Support**: Enabled for **ESP32-S3 DevKitC-1 (16MB)** and **M5Stack Cardputer (8MB)**.
  - **Model Matching**: Devices automatically fetch the exact update file made for their hardware.
  - **Anti-Brick Safety**: Automatically restores the previous version if a new update fails on first boot.
  - **Smaller Flash Boards**: Devices with 4MB flash safely need to guided USB updates.
  ---
  ## New Javascript API's We Add In this version
- **KryonOS Cloud AI JavaScript Engine (`Kryon.ai` / `System.ai` / `AI`)**:
  - `Kryon.ai.stream(options)`: Real-time Server-Sent Events (SSE) token-by-token streaming directly into UI without RAM memory exhaustion.
  - `Kryon.ai.ask(prompt, options, callback)`: One-shot LLM reasoning query with usage stats and error handling.
  - `Kryon.ai.extract(options, callback)`: Schema-constrained structured JSON extraction from messy logs and sensor feeds.
  - `Kryon.ai.vision(options, callback)`: Computer vision analysis of screen buffer snapshots or camera images.
  - `Kryon.ai.status()`: Real-time connectivity and daily credit quota monitoring.
- **FastMath Hardware Acceleration Engine (`FastMath` / `System.math`)**:
  - Direct hardware single-precision Floating Point Unit (FPU) math: `FastMath.sin()`, `FastMath.cos()`, `FastMath.tan()`, `FastMath.asin()`, `FastMath.acos()`, `FastMath.atan2()`, `FastMath.sqrt()`, `FastMath.hypot()`.
  - **360° Trigonometric LUT**: Pre-computed `FastMath.fastSin()` and `FastMath.fastCos()` for 60FPS animation loops.
  - **Hardware True Random Number Generator (TRNG)**: `FastMath.random()` and `FastMath.randomRange()` powered by ESP32 internal cryptographic hardware generator (`esp_random()`).
  - **3D Graphics & Matrix Batch Processing (300%–800% FPS Boost)**: Native C++ 4x4 matrix multiplications (`FastMath.mat4Multiply`), Euler rotations (`FastMath.mat4Rotate`), perspective projection (`FastMath.mat4Perspective`), and bulk vertex projection (`FastMath.transformVertices`).
  - **Vector & Array DSP Utilities**: Native `FastMath.vec2Distance`, `FastMath.vec3Distance`, `FastMath.dot2`, `FastMath.dot3`, `FastMath.cross3`, `FastMath.normalize2`, `FastMath.normalize3`, `FastMath.arraySum`, `FastMath.arrayMinMax`, and `FastMath.arrayDot`.
  - Mathematical constants: `FastMath.PI`, `FastMath.HALF_PI`, `FastMath.TWO_PI`, `FastMath.DEG_TO_RAD`, `FastMath.RAD_TO_DEG`.
- **Kryon3D Hardware-Accelerated 3D Engine (`Kryon3D` / `System.graphics3d`)**:
  - **Flicker-Free Double-Buffering**: Off-screen frame buffer management in PSRAM/SRAM with single-burst DMA SPI flush (`Kryon3D.begin`, `Kryon3D.clear`, `Kryon3D.render` / `Kryon3D.flush`, `Kryon3D.end`).
  - **Unified 2D + 3D Composite Rendering**: All standard `System` 2D graphics functions (`fillRect`, `drawString`, `drawLine`, `drawRoundRect`, `fillCircle`, `drawPixel`) automatically layer directly onto the active 3D double buffer, enabling 100% flicker-free game HUDs, weapons, and minimaps.
  - **3D Camera, Viewport, Projection & Lighting**: Configurable viewport sub-rects (`Kryon3D.setViewport`), camera eye/target, FOV, and directional light shading (`Kryon3D.setCamera`, `Kryon3D.setLight`, `Kryon3D.setFog`).
  - **Hardware 3D Primitives**: `Kryon3D.drawLine`, `Kryon3D.drawTriangle`, `Kryon3D.fillTriangle` (with automatic backface culling and flat directional shading), `Kryon3D.drawCube`, and `Kryon3D.drawBillboard`.
  - **Batch 3D Mesh Rendering**: Native `Kryon3D.drawMesh` capable of transforming, culling, shading, and rasterizing entire 3D polygon models in a single C++ call.
- **JavaScript Networking & REST API Engine (`Network`)**:
  - Full REST API support: `Network.get()`, `Network.post()`, `Network.put()`, `Network.delete()`, and unified `Network.request()`.
  - Configurable timeouts (`timeoutMs`) and FreeRTOS watchdog-safe non-blocking event pumping.
  - Streaming file downloads with live progress callbacks (`Network.downloadFile(url, dest, onProgress)`) in 1024-byte chunk buffers directly to LittleFS/SD.
  - Real-time diagnostic APIs: `Network.isConnected()`, `Network.hasInternet()`, `Network.getIP()`, `Network.getSSID()`, `Network.getRSSI()`, and `Network.showWiFiPrompt()`.
  - Seamless offline protection: returns `{ status: 0, error: "WiFi not connected" }` without freezing or crashing.
- **Hardware-Accelerated WebSocket Client Engine (`WebSocket`)**:
  - W3C-standard `WebSocket` class: `onopen`, `onmessage`, `onerror`, `onclose`, `send()`, `close()`.
  - Pure C++ native FreeRTOS 20-second ping/pong keep-alive heartbeat to prevent home NAT router dropouts.
  - Automatic event dispatching integrated into the JS event pump.
- **Android-Style Smart Multi-Network WiFi Manager**:
  - Multi-credential database (`known_networks.json`) with auto-migration from legacy `wifi.txt`.
  - Two-Phase Sequential Candidate Evaluation: sorts in-range APs by RSSI signal strength and performs post-association HTTP 204 WAN verification.
  - Background auto-reconnect on connection dropout and on-boot smart association.
  - Upgraded Settings WiFi UI with live connection card (RSSI bars, IP, Online/Offline badge), asynchronous network scanner with animated spinner, and Saved Networks management screen.
- **Embedded JavaScript Web Server Engine (`HttpServer`)**:
  - Express.js-style embedded HTTP server: `HttpServer.listen()`, `HttpServer.stop()`, `HttpServer.isRunning()`, `HttpServer.getPort()`, `HttpServer.getURL()`, `HttpServer.getStats()`, `HttpServer.poll()`, `HttpServer.reset()`.
  - Full routing & middleware engine: `HttpServer.on()`, `.get()`, `.post()`, `.put()`, `.delete()`, `.patch()`, `.options()`, `HttpServer.use()`, `HttpServer.notFound()`, and dynamic route parameter matching (`/user/:id`, `/files/*`).
  - Express-like `req` and `res` objects (`req.query`, `req.params`, `req.headers`, `req.body`, `req.json()`, `req.ip`, `res.status()`, `res.json()`, `res.html()`, `res.text()`, `res.send()`, `res.sendFile()`, `res.cors()`, `res.redirect()`).
  - Safe 1024-byte (1 KB) chunked static file streaming (`HttpServer.serveStatic`) from LittleFS / SD with automatic MIME type detection and TWDT watchdog safety.
  - Hardened embedded security: 16 KB body payload limit (`413 Payload Too Large`), directory traversal protection (`..`), 500 ms header timeout against Slowloris freezes, and explicit Duktape callback lifecycle management.
- **Hardware-Accelerated PWM Engine (`PWM` / `System.pwm`)**:
  - Bound directly to the ESP32 / ESP32-S3 LEDC hardware peripheral (8 channels on ESP32-S3, 16 on ESP32).
  - High-precision control APIs: `PWM.setup()`, `PWM.write()`, `PWM.setDuty()`, `PWM.setFrequency()`, `PWM.setTone()`, `PWM.stopTone()`, `PWM.setServo()`, `PWM.detach()`, `PWM.getChannel()`, `PWM.reset()`.
  - Non-blocking tone synthesis with automatic timed expiration and rollover-safe polling.
  - 50 Hz 14-bit microsecond pulse width calculations for RC servo motor positioning.
  - Multi-tier safeguards: Arduino ESP32 v2.x / v3.x cross-version macros, frequency vs resolution validation, and smart timer distribution.
- **Hardware TwoWire I2C Master Engine (`I2C` / `System.i2c`)**:
  - Full hardware I2C master peripheral control: `I2C.begin()`, `I2C.end()`, `I2C.scan()`, `I2C.ping()`, `I2C.readReg()`, `I2C.writeReg()`, `I2C.readReg16()`, `I2C.writeReg16()`, `I2C.readRegBytes()`, `I2C.write()`, `I2C.read()`, `I2C.reset()`.
  - Automatic 50ms hardware bus timeout protection preventing FreeRTOS watchdog freezes when slaves are disconnected.
  - Repeated-Start (`Wire.endTransmission(false)`) for atomic multi-byte register operations on sensors (MPU6050, BMP280, ADS1115).
  - 9-clock cycle bus recovery sequence on `I2C.reset()` to release unacknowledged lockups from wedged slave devices.
  - Automatic 32-byte chunking for large buffer transmissions (SSD1306 OLED displays, DACs).
- **Hardware Cryptographic Acceleration Engine (`Crypto` / `System.crypto`)**:
  - Direct ESP32 silicon hardware-accelerated SHA-256 (`Crypto.sha256()`) and SHA-512 (`Crypto.sha512()`) hash computation.
  - Hardware-assisted HMAC-SHA256 (`Crypto.hmacSha256()`) for API tokens, AWS signature v4, webhooks, and JWT validation.
  - Hardware AES-128 / AES-256 symmetric cipher blocks (`Crypto.aesEncrypt()`, `Crypto.aesDecrypt()`) with PKCS#7 block alignment and strict padding validation.
  - Cryptographically secure hardware True Random Number Generator (`Crypto.randomBytes()`) sampled directly from silicon RF noise.
- **Floating System Notifications (`System.notify` / `Notification`)**:
  - Non-blocking, fixed-size ring buffer queue in RAM (4 items max) preventing memory fragmentation.
  - Smooth cubic easing animation state machine (`SLIDE_IN` $\rightarrow$ `DISPLAYING` $\rightarrow$ `SLIDE_OUT` over $Y: -45 \rightarrow +10$).
  - Compositor overlay hook rendering glass card pill banners with typed status badges (info, warning, error, success) and optional PWM audio chimes.
  - JS management APIs: `System.notify(options)`, `System.notify.dismiss(id)`, `System.notify.clearAll()`.
- **Inter-App Communication & Intent Dispatcher (`System.ipc` / `IPC`)**:
  - Contextual application launching with JSON startup argument passing (`System.ipc.launch()`, `System.ipc.getLaunchArgs()`).
  - File extension association mapping (`fileAssociations: [".bmp", ".txt"]`) with automatic handler resolution (`System.ipc.openFile()`).
  - Companion launch access control (`allowCompanionLaunch` default `true`).
  - Runtime message mailbox system for inter-process communication (`System.ipc.send()`, `System.ipc.onMessage()`).

#### May be some new API's are miss here and these changelogs are only for the Reference Please Read the detailed API Docs here [Documentation/Kryon3D_Engine_Guide.md](Documentation/Kryon3D_Engine_Guide.md) 
  ---
- **Async Web Manager & Storage Dashboard Redesign**:
  - Embedded high-performance Web File Manager (`/`) served directly from flash PROGMEM with 0 bytes required on LittleFS/SD.
  - 100% inline embedded Lucide/Tabler SVG vector icons for drives, files, folders, and operations.
  - Real-time `/api/storage` telemetry reporting LittleFS total/used/free, SD mount status & total/used/free, dynamic SoC chip model, free RAM, and conditional PSRAM display.
  - In-browser VS Code-style code editor featuring synchronized line numbering gutter (1..N), Tab indentation, Ctrl+S save shortcut, cursor telemetry (`Ln X, Col Y`), and language identification.
  - Multi-theme selector (VS Code Dark `#1e1e1e`, Cyber Blue `#0f172a`, Clean Light `#f8fafc`) with client-side `localStorage` persistence.
  - Direct browser-to-GitHub live telemetry for stargazers count, forks, author info, and repository community modal with zero ESP32 network overhead.
  - Low-overhead 30-second background polling with `document.hidden` visibility checks and instant window focus refresh.
- **Settings UI "About Device" Community Card**:
  - Live GitHub stargazers count fetcher (`https://api.github.com/repos/Haris16-code/KryonOS/stargazers/count`).
  - Offline 5-tier star count rounding cache (`/local/system/stars_cache.json`) for instantaneous loading without internet delays.
  - Community repository links, author attribution (`Haris (@Haris16-code)`), and dynamic OS versioning.

### Changed
- **FileSystem Path Resolution**: Refactored `FileSystem::getTargetFS` to use non-static string references and uniform trailing slash normalization across all file, directory, and storage calls.
- **Documentation**: Updated `Documentation/JS_API_Guide.md` with complete reference documentation for `FastMath` (Section 6) and `Kryon3D` (Section 7), and created dedicated detailed [Documentation/Kryon3D_Engine_Guide.md](Documentation/Kryon3D_Engine_Guide.md) with 3D game tutorials and PSRAM memory recommendations.

### Deprecated
- *None in this release.*

### Removed
- *None in this release.*


### Breaking
- *None (All existing JavaScript apps and APIs remain 100% backward compatible).*

---

## [1.0.0] - Initial Release

### Added
- Core HarixOS / KryonOS JavaScript runtime based on Duktape engine.
- Direct-to-glass rendering pipeline with 16-bit RGB565 color support.
- GPIO control APIs (`System.gpio.pinMode`, `digitalWrite`, `digitalRead`, `analogRead`, `analogWrite`, `pulseIn`) And other API's for more info please refer this: https://github.com/Haris16-code/KryonOS/releases/tag/v1.0.0.
- Virtual File System (`FS`) supporting `/local` and `/sd` partitions.
- Full touch screen input polling and on-screen keyboard (`System.prompt`).
- App Store, Help Center, and System Settings user interfaces.
