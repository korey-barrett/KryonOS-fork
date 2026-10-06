# KryonOS-fork — project snapshot

State captured 2026-10-07. Written as a cold-start brief for a new chat session.

Supersedes the 2026-10-03 capture, whose §6 blocker (the CYD OTA failure) is now closed — see §5.3.

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

**Binding intent, stated by the user and unchanged:** keep the diff against upstream as small as
possible. "Porting this forked repo to this board from arduino-esp32 shouldn't require changing great
swathes of code to try and make a square peg fit into a round hole." Prefer a build flag or a board
config over a source edit; prefer reusing Espressif's BSP over writing a new HAL.

A second theme has grown since: the **Waveshare ESP32-S3-Touch-LCD-2.1B**, whose round 480x480 ST7701
RGB panel needed a display backend of its own. That backend (`EspLcdRgbDisplay`) is now the most
delicate part of the codebase — see §6.

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
| Version | `KRYONOS_VERSION` = **2.0.2** (`platformio.ini:49`) |
| Working tree | clean |
| Unpushed | **2 commits** — `c7610f9`, `fa70f7b` |

`main` was force-pushed to `b79a700` on 2026-10-03 to strip Claude attribution. There are **zero**
Claude mentions on the remote, no `.claude/`, no `CLAUDE.md`.

---

## 3. Hardware in play

| Port | Board | Notes |
|---|---|---|
| COM4 | Waveshare ESP32-S3-Touch-LCD-2.1B | 480x480 round ST7701 RGB panel, CST820 touch. **The board in active use.** |
| COM5 | ESP32-S31-Korvo-1 | the port target |
| COM6 | ESP32 CYD2USB ("CYD", 2.8") | classic ESP32-D0WD-V3 rev 3.1, 4 MB flash, MAC `a4:f0:0f:5c:b7:bc` |

**Only COM6 goes through an `esp_usb_board` bridge**, and that is the whole basis of the boot-mode
rule. Two consequences:

- **Boot mode must be enabled manually** before any esptool operation on the CYD, and re-enabled after
  one.
- **In manual boot mode the application is not running** — only an esptool-ready ROM bootloader. So
  runtime serial logs are **impossible** on the CYD. Opening COM6 parks the chip regardless of DTR/RTS.
  A capture returns only the 30-byte `ets Jul 29 2019 12:21:46` ROM banner.

**COM4 does not work this way.** The 2.1B auto-resets over its own USB-serial and is flashed plainly
with `pio run -e waveshare-s3-lcd21b -t upload --upload-port COM4`. The user confirmed on 2026-10-07
that the boot-mode confirmation rule is CYD-only. Never pass `--before`/`--after` reset flags on
either board.

Toolchain lives at `C:\Users\korey\.platformio\penv\Scripts\` — `pio.exe`, `python.exe`, and
**`esptool.exe`** (not `esptool.py`; esptool v5.4.0).

For any `write_flash`, set `PYTHONIOENCODING=utf-8` and `PYTHONUTF8=1` — without them esptool's progress
bar raises `UnicodeEncodeError` under the Windows charmap codepage and interrupts the write part-way,
leaving a corrupt image that then reports "No serial data received".

**Opening a serial port resets the board** — esptool's `hard_reset` and pyserial's DTR/RTS assertion on
open both do it. A blank screen with the backlight off means the board is sitting in the bootloader,
not that the firmware is broken.

---

## 4. Boards, environments, and the build contract

`platformio.ini` targets **one default board per chip type**. Upstream's board ports are unmaintained
examples, kept under `src/Hal/Boards/board_configs/examples/` and not built.

| Env | Chip | Partition table | OTA |
|---|---|---|---|
| `esp32s3-default` | ESP32-S3 | `default_16MB.csv` | yes |
| `esp32-default` | ESP32 | `huge_app.csv` (one 3 MB slot) | **no** — `Update.begin()` returns `UPDATE_ERROR_NO_PARTITION`, surfaced as a normal error, no crash |
| `esp32-cyd-28` | ESP32 | `min_spiffs.csv` (two 1.875 MB slots) | **yes — the one OTA was tested on** |
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

The Waveshare 2.1B's stack, for contrast: platform `55.03.312-1` (= Arduino core **3.3.12**, IDF
**5.5.5**), `board_build.arduino.memory_type = qio_opi`, 16 MB flash, 8 MB octal PSRAM.

---

## 5. What is done

### 5.1 Fork-hosted OTA (complete)

- `OTAManager::UPDATE_MANIFEST_URL` is now **composed from the chip variant the board env names**
  (`KRYONOS_OTA_VARIANT`). The fork's manifests live on per-variant branches — `esp32`, `esp32s3`,
  `esp32s31` — each holding `updates/<variant>/v2/update.json`. A board that sets no
  `KRYONOS_OTA_VARIANT` keeps the classic-ESP32 branch, so the file still builds everywhere. This one
  expression is the only firmware-side change on the OTA path; `TLSHelper` calls `setInsecure()`, so no
  trust anchor changed.
- **Release `v2.0.2` published** on the fork: 12 assets, tag at the rewritten `main`.
- `push-update.yml` has been run for `esp32-cyd-28` only. The other four boards remain at 2.0.1 /
  `supports_ota: false`.

### 5.2 Making the CYD fit a second app slot (complete)

`min_spiffs.csv` gives two `0x1E0000` = **1,966,080**-byte slots. The CYD's `firmware.bin` is now
**1,830,176** bytes → **135,904 bytes of headroom**. Two levers, neither costing a feature:

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

### 5.3 *WAS the blocker* — the in-place OTA failure (CLOSED 2026-10-04)

**Root cause: the mounted SD card.** The failure surfaced as `Flash Init Failed: Err #0`. That string
is Arduino's `UpdateClass::begin()`, which leaves `_error` at `UPDATE_ERROR_OK` on exactly two paths —
an update already running, and a **failed allocation of its 4096-byte sector buffer**. Only the second
is reachable on a first attempt. That buffer is allocated *after* the download's TLS session is already
open, so a mounted SD volume — allocated from the same internal heap — is enough to push it over.

**The measurement:** with the card removed, the **identical** firmware updated in place onto 2.0.2,
with nothing else changed between the two runs.

**The change:** `FileSystem` gained `suspendSD()` / `resumeSD()`, and `SettingsUI` releases the volume
around both `startFlashUpdate()` call sites. The remount sits on the failure path, because success
restarts the chip from inside the call. The per-variant manifests carry a notice asking users to remove
the SD card before an update until this is confirmed more widely than one A/B.

Two false trails worth not re-running: the 2026-10-03 snapshot's hypothesis that this was mid-download
heap exhaustion during the TLS stream (it was a heap failure, but at `Update.begin()`, not mid-stream),
and the instrumented-build breadcrumb plan that followed from it — the breadcrumbs were never written,
and `OTAManager.cpp` is now upstream's file with the single manifest-URL expression changed.

### 5.4 Claude attribution scrub (complete)

`main` force-pushed to `b79a700`; 0 mentions on the remote; backup refs deleted, reflog expired, gc'd.

---

## 6. The Waveshare 2.1B display work (current focus)

All of §6 landed 2026-10-07 and is verified on hardware on COM4, except where noted.

### 6.1 The UI is 1.2x larger

Canvas **240x320 → 201x268**; aperture scale **6/5 (1.2000) → 96/67 (1.4328)**.

Carried by **two build flags and two constants**, no per-screen layout edits:
`KRYONOS_DISPLAY_WIDTH`/`_HEIGHT` in `platformio.ini` and `SCALE_NUM`/`SCALE_DEN` in
`EspLcdRgbDisplay.h`.

**The geometry invariant that makes it free:** for any 3:4 canvas `W x H = 3m x 4m`, the half-diagonal
is `2.5m`, so `k = 240 / 2.5m = 96/m` and the blitted rect is `3m * 96/m = 288` by `4m * 96/m = 384`
— **always 288x384 at offset (96,48)**, at every canvas size. Only the magnification changes, and
shrinking the canvas magnifies. `uiScale(201, 268)` clamps to 1, so `m.scale` is unchanged and every
`N * m.scale` call site is untouched.

Two edges that pin the size:

- **201 is the narrowest canvas the screens tolerate.** `InstallerUI`'s three-button row spans
  `3*60 + 2*10 = 200` px and `UiLayout::dialogButtonSpaced()` centres that run, so the outer two
  buttons hang off both edges below it. Narrowing further means narrowing those call sites first.
- **268 keeps every `h < 240` fallback on its normal path** — the 42 px notification card, the OTA
  title font, the keyboard chrome. It does cross KryonCloudUI's own 320-keyed threshold; that was a
  separate bug and is fixed (below).

**On the glass:** body text 19 → 23 px, list rows 36 → 43 px, header/footer bars 36 → 43 px, footer
UP/DN 36 → 43 px, launcher list 7 rows → 5.

**The keyboard** grew via a board-declared compact chrome (`KRYONOS_KB_COMPACT_CHROME`). Key width on
the glass is `288 / columns` and is therefore fixed at 48 px for this board's 6 columns, but key height
is `(h - kbGridTop) / rows`, and the chrome's fixed 110 px stack does not shrink with the canvas — at
268 tall that would have left the six rows 26 px each. Claiming the layout's existing compressed
chrome (grid top 72) gives them 32 px, i.e. **47x46 px keys against the previous 48x42**, instead of
47x37. The flag is 0 on every other board, so the `h < 240` test compiles to the identical expression
there.

**KryonCloudUI** laid its cards out in 240x320 literals and scaled them by a *quantized half step*
(`2 * h / 320`), which fell to 1 under 320 and put every frame at half height while the glyph stayed a
fixed 16 px cell. `cv()` is now proportional (`v * h / 320`, capped at `v * m.scale`) and `cvh()`'s
floor is the reference layout's smallest font-2 frame, 20 px. Bit-identical at 240x320 and at the
Korvo-1's 800x480.

### 6.2 Round-rect corners

`c52b1ab` — the RGB panel filled a round-rect's corners with a circle helper, leaving the little
"chain link" notches at the top and bottom of button edges. Now filled with the exact chord.
(`cc0a62a` on 2026-10-03 was the same class of bug for the 800x480 panel.)

### 6.3 The Time & Region footer keys

`c7610f9` — `handleTimeTouch()`'s main branch tested only `UI_FOOTER_SEL`, so UP and DN did nothing.
`drawTimeSettings()` had **already drawn** them: the footer is UP / BACK / DN whenever
`actionCount > timeBtnPerPage()`, and on the shorter canvas that is four actions against a page of
two — the clock header is a fixed 65 px and the button pitch is `rowH + 15`. At 240x320 all four
fitted, the footer was a plain BACK with no UP or DN to press, and the missing cases never showed.
`timeActionScroll` was already declared, drawn from and clamped; nothing could move it. The branch now
switches on all three thirds, as the timezone picker above it already did.

### 6.4 The RGB flicker — the missed VBlank restart window

**Symptom:** thin white lines travelling up and down the screen. First on a still page; then on every
screen after the App Store button was pressed. It cleared once on a clean reflash, which is what made
an earlier note call it boot state. **That was wrong** — it is a real bug in the panel timing.

**Cause.** The prebuilt Arduino libs are built with
`CONFIG_LCD_RGB_RESTART_IN_VSYNC=y` (verify in
`framework-arduinoespressif32-libs/esp32s3/sdkconfig`, which also shows
`CONFIG_LCD_RGB_ISR_IRAM_SAFE is not set`, `CONFIG_SPIRAM_MODE_QUAD=y`, `CONFIG_SPIRAM_SPEED=80`).
That compiles `lcd_rgb_panel_try_restart_transmission` down to `do_restart = true` on **every** VBlank
and drops the `bb_eof_count < expect_eof_count` desync guard below it
(`esp_lcd_panel_rgb.c:1153-1164`). The GDMA channel is therefore torn down and restarted once per
field, always — and Espressif's own comment above that code says what a late interrupt costs:

> if this interrupt is late enough, the display will shift as the LCD controller already read out the
> first data bytes, and resetting DMA will re-send those.

The restart also mounts its buffer with a fixed `restart_skip_bytes = (LCD_LL_FIFO_DEPTH + 1) * 2` — a
**17-pixel FIFO-preserve skip** that is only correct if the restart lands where the driver assumes.
That is what turns a miss into a *thin band of wrong pixels* rather than ordinary tearing.

**The window** is `vsync_pulse_width + vsync_back_porch`. At the vendor 3/8/8 that is 11 of 499 lines,
**~377 µs** at 16 MHz — while the bounce-refill ISR that keeps the panel fed `memcpy`s **9600 bytes out
of PSRAM every ~300 µs** (`esp_lcd_panel_rgb.c:913`, `:920-922`), on the same PSRAM that WiFi, mbedTLS
and every LittleFS write are using, and which a flash write stalls outright. The margin was thin at
boot and blown under load — which is why it appeared with a still page, cleared once, and then came
back everywhere once the App Store drove a TLS handshake and its LittleFS writes through the same bus.

**The fix** (`fa70f7b`): `cfg.timings.vsync_back_porch` **8 → 32** in
`EspLcdRgbDisplay::setUpPanel()`, giving 35 lines (~1.2 ms) at **55.8 Hz** instead of 58.5. The only
timing value that departs from the vendor sequence; the panel is DE-mode and takes the longer blanking.
Confirmed on hardware: no flicker.

**The signature is the diagnosis.** If this ever comes back, read the artifact's *shape* before
touching anything:

| Shape | Cause | Fix |
|---|---|---|
| coherent lines travelling vertically | this — the restart missed its window | the porch value, §6.4 |
| random speckle / white noise | the bounce refill starved by PSRAM contention | cut the contention, **not** the timing |

### 6.5 Backend facts worth not re-deriving

- **Double buffering works.** The log prints `frame-boundary flip confirmed after N presents`; if the
  handshake ever dies the driver prints `no frame boundary in 500 ms; falling back to single
  buffering` and reverts to writing the live buffer. A boot log with the first line and not the second
  means the flip path is healthy — which is how the OTA-era hunt ruled it out as the flicker's cause.
- **The bounce buffer is mandatory on the S3.** Without it the VSYNC restart re-enters at `fbs[0]`
  every field, so `fb_[1]` can never reach the glass. Measured: with `bounce_buffer_size_px = 0` the
  panel reported double buffered, the flip callback fired, and the screen stayed black.
- **The draw path is state-gated.** `main.cpp:202` redraws only on `currentState != lastState`, so a
  sitting page leaves `dirty_` false and `present()` early-returns — a still screen is not flipping.
- **`EspLcdRgbDisplay` never calls `canvas_.setTextSize()`**, contradicting `UiLayout.h:77-79`, which
  claims the backend sets the text size from the layout scale. Benign here because `uiScale(201,268)`
  is 1 and the canvas default is 1, so they agree. The *comments* are wrong, not the behaviour.

---

## 7. Open items — none of them blocking

1. **Two unpushed commits** — `c7610f9`, `fa70f7b`. The user asks for pushes separately.
2. **`AppStoreUI::draw()` blocks the loop through a whole TLS handshake.**
   `AppStoreUI::draw()` calls `fetchCategories()` synchronously before it draws anything
   (`AppStoreUI.cpp:87-99`), with `http.setTimeout(15000)`. It is the one place in the UI that stalls
   the loop for seconds, and it is the worst PSRAM-contention offender on the board — the thing §6.4's
   fix works around rather than removes.
3. **`task_wdt` spam while connecting.** `WiFiManager::connectTo()`'s wait loop calls
   `esp_task_wdt_reset()` from a task that was never subscribed (`WiFiManager.cpp:222-225`), printing
   `task_wdt: esp_task_wdt_reset(707): task not found` every 200 ms until the association completes.
   Benign and self-clearing, but it is noise in every boot log.
4. **`.github/workflows/release.yml` publishes 3 of the 4 flash files** while `README.md:315` says
   four.
5. **The CYD OTA fix is confirmed by one A/B.** The per-variant manifests still carry the "remove the
   SD card" notice until it is confirmed more widely.
6. **The `Graphic Demo` app draws off-canvas at `y = 300`.** It lives in `Haris16-code/KryonOS-AppStore`
   — a third-party repo. The user has not asked for it to be reported.

---

## 8. Standing rules — verbatim from the user, still binding

- **"do not use the word honestly in this or any future chat sessions. commit to this fork only. do not
  alter anything on the main repo."**
- **No Claude attribution anywhere.** No `Co-Authored-By` trailer, no "Generated with Claude Code"
  footer, no mention in any tracked file or doc. This overrides the session-level attribution reminder;
  the user's own instruction takes precedence and the reminder says so. Skip it silently rather than
  adding and stripping. (Asked 2026-09-29, had to be asked again 2026-10-03.)
- **`-ExecutionPolicy Bypass` was denied** by the permission classifier as a security-weakening action.
  Never retry it by any route.
- **Boot mode is a CYD-only rule** (stated 2026-10-07): only the CYD2USB needs the strap held by hand
  via `esp_usb_board`. The Waveshare 2.1B auto-resets. Never pass esptool reset flags on either.
- **Never wipe NVS on the CYD** — no offset-0x0 factory flash, no full erase. It holds the KryonCloud
  deviceId and losing it forces a re-registration.
- **No diagnostic firmware.** Never build or flash tracing or instrumentation; read the library source
  and fix the cause in one change.
- **Reproduce the user's working setup.** Their workflow works; a harness limit is not evidence about
  their environment. Hand them a real console via `Start-Process` rather than substituting something.
- Keep responses short. Do not repeatedly demand tap confirmations.
- Builds are sequential, one env per command.

Recorded as memory files under
`C:\Users\korey\.claude\projects\D--KryonOS-fork\memory\`; `MEMORY.md` is the index.

---

## 9. Deferred / nice-to-have, not requested

- `esp32-default` could regain OTA with the same gzip lever — with `huge_app` it overflowed
  `min_spiffs` by only 1,493 bytes. Now that both levers exist, the numbers are worth re-measuring.
- The `unsupported_ota_guide` default in `push-update.yml:58` already points at the fork's Releases
  page, not upstream's flasher. Nothing to do; noted because an earlier capture claimed otherwise.
- `push-update.yml` defaults `ota_esp32_default: true` while the published `esp32` manifest carries
  `supports_ota: false` for that board — the `huge_app` table has one app slot. The default is only
  what pre-fills the dispatch form, so it is a foot-gun rather than a bug; flipping it to `false`
  would match what has actually been published.
- `src/Kernel/Services/Network/TLSCerts.h` is dead code worth 0 bytes — nothing includes it. Hygiene
  only.
- `UiLayout::dialogButtonSpaced()` emits off-canvas rects for any canvas narrower than the caller's
  literals. Worth making self-fitting on its own merits; §6.1 is sized to avoid needing it.
- Upstream URLs elsewhere are cosmetic and off the OTA path: the star fetch (`SettingsUI.cpp:599`), the
  About screen (`SettingsUI.cpp:839,847`), Help Center index (`HelpCenterUI.cpp:428`), App Store index
  (`AppStoreUI.cpp:78`), `help/*.json`, README badges.
- `EspLcdRgbDisplay`'s bring-up doc comments (`UiLayout.h:77-79`) describe a `setTextSize()` call the
  backend does not make. Correct the wording; do not add a no-op call.
