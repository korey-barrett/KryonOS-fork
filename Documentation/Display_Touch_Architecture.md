# Display & Touch Driver Architecture

This document explains **exactly where and how KryonOS decides which display and touch hardware it
drives**, because that decision cascades into everything downstream: the resolution the UI lays itself
out at, the coordinates touch events are mapped to, and what a new board must provide.

If you are adding a board with a different panel or touch controller, read this first.

## Active targets

This fork carries **one default board per chip type**. Each is a bring-up starting point (generic SPI
panel + XPT2046 touch), not a specific product — copy it and give it your own name for real hardware.

| Environment | Chip | Flash/PSRAM | Core / IDF | In `default_envs` | App partition |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `esp32s3-default` | ESP32-S3 (Xtensa LX7) | 16 MB / 8 MB | Arduino 3.3.12 / IDF 5.5.5 | yes | 6.5 MB — ample headroom |
| `esp32-default` | ESP32 (Xtensa LX6) | 4 MB / none | Arduino 3.3.12 / IDF 5.5.5 | yes | 1.96 MB — **1,493 bytes free**, see below |
| `esp32s31-default` | ESP32-S31 (RISC-V) | 16 MB / 16 MB | Arduino 4.0.0-RC1 / IDF 6.1 | **no — preview** | 16 MB table (`default_16MB.csv`) |
| `waveshare-s3-lcd21b` | ESP32-S3 (Xtensa LX7) | 16 MB / 8 MB | Arduino 3.3.12 / IDF 5.5.5 | **no — one product** | 16 MB table (`default_16MB.csv`) |

`waveshare-s3-lcd21b` is the first board whose panel is not on SPI at all — see §2.5.

`esp32-default` originally used `min_spiffs.csv` — the largest app partition a 4 MB ESP32 offers while
keeping two OTA slots — and the firmware filled 99.9% of it (**1,964,587 of 1,966,080 bytes, leaving
1,493 bytes**). That margin was the binding constraint on this target: any further work on the shared
UI costs bytes here first, so changes that built fine on the S3 could still overflow the ESP32.

It now uses **`huge_app.csv`**: a 3 MB app partition (up from 1.875 MB) and an 896 KB LittleFS area,
at the cost of the second OTA slot. On a 4 MB ESP32 you cannot have a large app *and* two OTA slots,
and the app space is what was blocking work.

**Note this trades away OTA on this env.** `Update.begin()` finds no update partition and fails with
`UPDATE_ERROR_NO_PARTITION`, which `OTAManager` reports as an ordinary error (no crash), and
`confirmBootSuccessful()` still marks the running app valid. The S3 default keeps full OTA.

`max_app_4MB.csv` is *not* an alternative here: it has no filesystem partition at all, so the
LittleFS mount (apps, `/system/`, touch calibration) would have nowhere to live.

**ESP32-S31 is on a pre-release toolchain, and is not built by default.** The S31 (dual-core RISC-V,
Wi-Fi 6, RGB/I8080 LCD, 14 capacitive touch channels) needs **ESP-IDF 6.1**; arduino-esp32 reaches
that only on its **4.x line**, which is not released yet. The environment therefore pins the
pioarduino **pre-release** platform `61.04.00-RC1` (Arduino 4.0.0-RC1 / IDF 6.1) — the only published
spec that ships an `esp32s31` toolchain, the matching `framework-arduinoespressif32-libs`, and a board
JSON (`esp32-s31-coreboard-1`). PlatformIO resolves `platform` per environment, so the S31's IDF 6.1
core and the other two targets' IDF 5.5.5 core coexist in one project.

It is deliberately **not** in `default_envs`: the OS is written against the Arduino 3.x API, and the
Arduino 4.x / IDF 6.1 port is still to do. Build it explicitly with `pio run -e esp32s31-default`,
and add it to `default_envs` once it compiles. See §5 for the toolchain details.

**The env does not build yet, and the blocker is TFT_eSPI, not the OS.** A full run
(2026-10-03) compiled the entire framework — `Network`, `AsyncTCP`, `Hash`, the Arduino `WebServer`,
`WiFi` — and then died inside TFT_eSPI with **130 errors, all in its legacy
`Processors/TFT_eSPI_ESP32.{h,c}` backend**; nothing in `src/` was ever reached. That is the Phase 5
case: the S31's SPI/GPIO peripherals are not the ones TFT_eSPI register-pokes, so the S31 needs an
`esp_lcd` backend (§5.2).

---

## 1. The selection chain at a glance

Board selection is **compile-time only**. There is no runtime board manager and no probing. The chain is:

```
platformio.ini ([platformio] extra_configs)
        │  pulls in board_configs/<board>.ini  and/or declares the env inline
        ▼
[env:<board>] build_flags
        │  -D TARGET_<BOARD>=1                     ← selects the C++ board implementation
        │  -D USER_SETUP_LOADED=1                  ← tells TFT_eSPI to ignore its own User_Setup.h
        │  -D <CONTROLLER>_DRIVER=1                ← selects the TFT_eSPI display driver
        │  -D TFT_*, SPI_*, USE_HSPI_PORT ...      ← display pins / bus
        │  -D KRYONOS_DISPLAY_WIDTH/HEIGHT/ROTATION ← logical resolution + orientation
        │  -D TOUCHSCREEN_* / TOUCH_* pins         ← touch wiring (two accepted spellings)
        │  -D KRYONOS_TOUCH_DRIVER=\"<name>\"      ← optional; "auto" derives it from the pins
        ▼
src/Hal/Boards/<board>/BoardConfig.cpp
        │  guarded by #if defined(TARGET_<BOARD>)
        │  defines the global `TFT_eSPI tft` and the HAL functions declared in Board.h
        ▼
src/main.cpp  setup()
        │  Display::begin();  TouchDriver::init(&tft);
        ▼
UI + touch mapping
```

Two facts about this chain matter more than the rest:

1. **TFT_eSPI is configured purely by `-D` build flags.** There is no runtime API to pick a display
   controller — the controller macro (`ILI9341_DRIVER`, `ST7789_DRIVER`, …) is compiled in. Changing
   the panel is a build-flag change, not a code change.
2. **Board code is selected by a positive `TARGET_*` macro.** Every board implementation must be
   mutually exclusive with every other, because each defines the global `tft` symbol and the same HAL
   functions.

---

## 2. Display: how the driver is chosen

### 2.1 Build flags (the real driver selection)

Each environment's `build_flags` configure TFT_eSPI. The active ESP32-S3 default sets:

```ini
-D USER_SETUP_LOADED=1        ; required — TFT_eSPI uses these flags instead of its User_Setup.h
-D ILI9341_DRIVER=1           ; the display controller driver
-D TFT_MISO=13
-D TFT_MOSI=11
-D TFT_SCLK=12
-D TFT_CS=10
-D TFT_DC=9
-D TFT_RST=8
-D TFT_BL=21
-D TFT_BACKLIGHT_ON=HIGH
-D SPI_FREQUENCY=40000000
-D SPI_READ_FREQUENCY=20000000
-D SPI_TOUCH_FREQUENCY=2500000
-D USE_HSPI_PORT=1
```

The three environments in `platformio.ini` (`esp32s3-default`, `esp32-default`, `esp32s31-default`)
are the only *active* places these flags live. The three reference snippets under
`src/Hal/Boards/board_configs/examples/` (`cyd.ini`, `t_hmi.ini`, `cardputer.ini`) show other
panel/touch combinations but are deliberately not in `extra_configs`, so they are never built.

### 2.2 Board profile headers

`src/Hal/Boards/<board>/BoardConfig.h` is the C++ side of the profile: it is guarded by
`#if defined(TARGET_<BOARD>)` and uses `#ifndef` defaults so the `-D` flags always win. It declares:

- `KRYONOS_DISPLAY_WIDTH` / `KRYONOS_DISPLAY_HEIGHT` / `KRYONOS_DISPLAY_ROTATION` — the **logical**
  canvas the UI draws to (see §4).
- `DISP_HOR_RES` / `DISP_VER_RES` — **deprecated aliases**, still honoured for one release.
- Backlight, touch, SD and any board-specific pins.

### 2.3 The global `tft` symbol

`src/Hal/Boards/Board.h` declares `extern KryonDisplay& tft;` — a reference to the abstract surface in
`src/Hal/Display/KryonDisplay.h`, not to TFT_eSPI. Every UI file holds a `KryonDisplay*`, so the panel
driver behind it is a build-flag choice. The board file owns the concrete object and binds the
reference:

```cpp
#if KRYONOS_DISPLAY_BACKEND == KRYONOS_BACKEND_RAM
static RamFramebufferDisplay s_display(KRYONOS_DISPLAY_WIDTH, KRYONOS_DISPLAY_HEIGHT);
#else
static TftEspiDisplay s_display;
#endif
KryonDisplay& tft = s_display;
```

`KRYONOS_DISPLAY_BACKEND` (defined in `DisplayConfig.h`, default `KRYONOS_BACKEND_TFT_ESPI`) must be
compile-time because it decides the static type of the object — unlike `KRYONOS_TOUCH_DRIVER`, which
is a string compared at runtime. `TftEspiDisplay` derives from both `TFT_eSPI` and `KryonDisplay`, so
one object is simultaneously the driver and the interface; `KryonDisplay::nativeTft()` returns it for
the code that genuinely needs TFT_eSPI (sprite allocation, `Xpt2046TftDriver`), and returns `nullptr`
on a backend that has none.

Two things still reach for TFT_eSPI and are deliberately left that way for now: every UI file keeps
`#include <TFT_eSPI.h>` for the `TFT_*` colour and `*_DATUM` macros, and notification/JS sprites are
still `TFT_eSprite`, which needs a `TFT_eSPI*` — hence `nativeTft()` and the graceful no-op when it is
null. Decoupling the macros and adding a backend-neutral `KryonSprite` are follow-ups.

Exactly one board implementation defines the object. There is one default implementation **per chip
type**, each behind a positive guard set by its environment in `platformio.ini`:

| Environment | Guard | Implementation |
| :--- | :--- | :--- |
| `esp32s3-default` | `TARGET_ESP32S3_DEFAULT` | `src/Hal/Boards/esp32s3/BoardConfig.cpp` |
| `esp32-default` | `TARGET_ESP32_DEFAULT` | `src/Hal/Boards/esp32/BoardConfig.cpp` |
| `waveshare-s3-lcd21b` | `TARGET_WAVESHARE_S3_LCD21B` | `src/Hal/Boards/waveshare-s3-lcd21b/BoardConfig.cpp` |

> **Trap:** these files used to be guarded by an *inverse* condition
> (`#if !defined(TARGET_CARDPUTER) && !defined(TARGET_CYD) && !defined(TARGET_T_HMI)`). Adding a new
> board without editing that guard caused the new board **and** the default implementation to compile,
> producing duplicate `tft` / HAL symbols at link time. It is now a positive guard. **Every new board
> must use a positive `TARGET_<BOARD>` guard, and no two environments may define the same one.**

### 2.4 Dead vs live lifecycle

`Board.h` declares `initHardware()`, `initDisplay()` and `initTouch()`, and every board file defines
them — but **nothing calls them today**. The live boot path is in `src/main.cpp`:

```cpp
Display::begin();        // tft.init() + setRotation(KRYONOS_DISPLAY_ROTATION) + metric check
TouchDriver::init(&tft);
```

This is why the Cardputer's intended `setRotation(1)` (landscape) has never taken effect. The
resolution/rotation work moves this into a single `Display::begin()` so rotation comes from the board
profile.

### 2.5 A non-TFT_eSPI backend: the RGB parallel panel

`esp32s3-default` and `esp32-default` both drive SPI panels through TFT_eSPI, but TFT_eSPI cannot
drive every panel. The Waveshare ESP32-S3-Touch-LCD-2.1B carries a **480×480 ST7701 on a 16-bit RGB
parallel bus** (16 data lines plus DE/PCLK/VSYNC/HSYNC), with a 3-wire SPI-like bus used *only* for
init commands. There is no SPI pixel path, so the only route is ESP-IDF's `esp_lcd_panel_rgb` — which
means a `KryonDisplay` backend that is not a TFT_eSPI adapter:
`src/Hal/Display/EspLcdRgbDisplay.{h,cpp}`, selected by `KRYONOS_BACKEND_RGB` (value 3 in
`DisplayConfig.h`).

The design problem is text. Every UI label is drawn through TFT_eSPI's fonts, and TFT_eSPI's own
rasterizer is non-virtual, so a backend that does not use TFT_eSPI loses every glyph and every
`textWidth()` metric that `UiLayout` depends on. (`RamFramebufferDisplay` is exactly that: a correct
reference surface with no font.) The RGB backend therefore keeps **TFT_eSPI as a pure software
rasterizer**: it draws into a full-screen 16bpp `TFT_eSprite` in PSRAM and blits that sprite to the
panel, so the glyphs, metrics and anti-aliasing are pixel-identical to the SPI boards.

Three details are worth knowing before touching it:

- **The sprite is the rasterizer's target, not the panel.** `TFT_eSprite` derives from `TFT_eSPI`, so
  it inherits `drawString`/`textWidth`/`setTextDatum`/`color565`, and its virtual `drawPixel` /
  `drawChar` / `pushColor` land in the sprite buffer. The *composite* shapes are the trap: TFT_eSPI's
  `drawRect`, `fillRoundRect`, `fillCircle`, `fillTriangle`, `fillScreen` and friends are **not**
  virtual and not overridden by `TFT_eSprite`, so calling them on a sprite reaches the TFT_eSPI
  implementations and would push pixels at an unconfigured bus. The backend implements those itself on
  top of the sprite's primitives.
- **The sprite's buffer is byte-swapped, the panel's is not.** `TFT_eSprite` writes every 16bpp pixel
  as `(color >> 8) | (color << 8)` because it was built to feed an SPI panel. The RGB framebuffer is
  native little-endian, and `esp_lcd_panel_rgb` exposes no byte-order knob, so `present()` swaps on
  the way out and `pushImage` swaps on the way in (`canvas_.setSwapBytes(true)`), keeping
  `KryonDisplay`'s colour domain native everywhere.
- **The phantom TFT_eSPI.** The sprite's constructor needs a `TFT_eSPI*`, so the backend owns a
  `TFT_eSPI` instance that is never `init()`ed and owns no pins. Its environment therefore defines
  `USER_SETUP_LOADED`, a placeholder driver macro and the `LOAD_*` fonts — **and no pin macros at
  all**. That last part is load-bearing twice over: `I2CEngine::begin()` refuses a bus whose pins
  collide with any `TFT_*`/`TOUCH_*` macro, and this board's expander and touch controller share I²C on
  GPIO15/GPIO7. The phantom's own `TFT_WIDTH`/`TFT_HEIGHT` are not set from the environment: the
  driver's own defines set them unconditionally, so a `-D` would lose the race and only earn a
  redefinition warning. They never matter here, because the canvas is created at the panel's native
  size and every geometry call this backend makes goes to the sprite or to a method it implements
  itself.

The blit itself is the route this board's own shipped firmware takes: `esp_lcd_panel_draw_bitmap()`
over the full frame, with two framebuffers in PSRAM and a bounce buffer. Writing into the framebuffer
directly instead would mean owning `esp_cache_msync()` here, which is only needed by code that fills
the framebuffer itself.

> **Trap: PlatformIO compiles every `src/*.cpp` for every environment.** A backend that includes a
> header only some targets ship therefore breaks the *other* environments at compile time, not at link
> time. `EspLcdRgbDisplay.{h,cpp}` include `<esp_lcd_panel_rgb.h>`, which exists for the ESP32-S3 and
> P4 but not for the classic ESP32, so both files are wrapped in
> `#if defined(CONFIG_IDF_TARGET_ESP32S3) || defined(ARDUINO_ESP32S3_DEV)` and expand to an empty
> translation unit elsewhere — the same way a board file compiles to nothing when its `TARGET_*` guard
> is false. Any new backend must carry an equivalent guard around its target-specific includes.

### 2.6 The TCA9554 expander is its own module

`src/Hal/I2C/Tca9554.{h,cpp}` owns the board's I²C GPIO expander, because the display is no longer its
only user. Three signals this board needs are not on the SoC and all three live on that chip: the
ST7701's reset (`LCD_RST`), the touch controller's reset (`TP_RST`), and the chip select of the 3-wire
bus the panel takes its init sequence over. The micro-SD socket's chip select is on it too, and is
left alone.

The reason it is a **class** and not a byte of constants is the output latch. The chip has exactly one
(register `0x01`), and it now has two independent callers: the display backend asserts the command-bus
chip select while it clocks out the init table, and the touch path pulses `TP_RST` before every probe.
A caller that recomputes the whole byte from its own idea of the other bits clobbers the other's pin —
a literal `0x0B` written for the chip select drops a `TP_RST` the touch path had just released. So
every write goes through the object's shadow of the latch, and this class is the only sanctioned
writer of that register.

`boardExpander()` returns the one shared instance. It is a **function-local static**, not a
namespace-scope object, and that is deliberate: a global with a constructor emits an `.init_array`
entry, which the SDK's linker script keeps, so the object would be linked into every environment
whether or not it uses the chip. With the function-local static, an environment that never calls
`boardExpander()` drops the whole translation unit under `--gc-sections`.

The module carries the board's pin assignments (bit 0 `LCD_RST`, 1 `TP_RST`, 2 `LCD_CS`, 3 `SD_CS`,
7 buzzer; the wiki's `EXIO<n>` is bit `n-1`) and its masks, so they are defined once. `EspLcdRgbDisplay`
now holds none of them — `setUpExpander()` calls `boardExpander().begin(15, 7)` and `pulseLcdReset()`,
and the init table goes out under `boardExpander().setCommandCsAsserted(...)`.

---

## 3. Touch: how the driver is chosen

### 3.1 Pin normalization

Touch pin macros are spelled two ways across the repo. `src/Hal/Touch/TouchConfig.h` resolves both
into one canonical set, once, so the driver guards and the implementations cannot drift apart
(`src/Settings/TouchDriver.h` includes that header, so code that used to reach `T_CS_PIN` through the
facade still can):

| Canonical | Accepted spellings (first match wins) |
| :--- | :--- |
| `T_CS_PIN` | `TOUCH_CS_PIN`, `TOUCH_CS`, `TOUCHSCREEN_CS_PIN` |
| `T_CLK_PIN` | `TOUCH_CLK_PIN`, `TOUCH_CLK`, `TOUCHSCREEN_SCLK_PIN` |
| `T_DIN_PIN` | `TOUCH_DIN_PIN`, `TOUCH_DIN`, `TOUCHSCREEN_MOSI_PIN` |
| `T_DO_PIN` | `TOUCH_DO_PIN`, `TOUCH_DO`, `TOUCHSCREEN_MISO_PIN` |
| `T_IRQ_PIN` | `TOUCH_IRQ_PIN`, `TOUCH_IRQ` |

`TOUCHSCREEN_IRQ_PIN` is deliberately **not** mapped onto `T_IRQ_PIN`: the CYD's XPT2046 PENIRQ is not
wired out, so mapping it would make the driver read a permanently-released touch and disable input.

### 3.2 Which implementation compiles

`src/Settings/TouchDriver.cpp` is a **facade**. Its public API (`init`, `getTouch`, `getTouchRaw`,
`calibrateTouch`, `setTouch`) is unchanged from before the driver seam existed, so the ~30 call sites
in the kernel, keyboard, JS bindings and settings screens never had to move. Each call forwards to
whichever `ITouchDriver` the factory selected.

The drivers live in `src/Hal/Touch/`:

| File | Role |
| :--- | :--- |
| `ITouchDriver.h` | The interface every controller implements. |
| `TouchConfig.h` | Pin normalization (`T_*_PIN`) and driver selection. Arduino-free. |
| `TouchCalibration.h` | The 5-word tuple and the raw→pixel mapping. Arduino-free, header-inline. |
| `TouchDriverFactory.{h,cpp}` | The one place a driver is chosen. |
| `Xpt2046BitbangDriver.{h,cpp}` | Four-GPIO bit-banged XPT2046 (the historical implementation). |
| `Xpt2046TftDriver.{h,cpp}` | XPT2046 via TFT_eSPI's own touch path. |
| `NullTouchDriver.h` | No panel; reports no touch. |
| `CapacitiveTouchDriver.{h,cpp}` | FT6236 / GT911 / CST816 over I²C. The CST816 path is selected by the Waveshare 2.1B, whose CST820 answers to the same map; none of the three has been run against a real panel yet. |

Selection is the string build flag `KRYONOS_TOUCH_DRIVER`. Left unset it is `auto`, which reproduces
the old compile-time ladder exactly — so every pre-existing board keeps the driver it already used
without setting anything:

| Condition | Driver |
| :--- | :--- |
| all four of `T_CLK`/`T_DIN`/`T_DO`/`T_CS` defined | `xpt2046_bitbang` |
| else `TOUCH_CS` defined | `xpt2046_tft` |
| else | `null` |

Naming a driver explicitly overrides that, e.g. `-D KRYONOS_TOUCH_DRIVER=\"ft6236\"`. Naming one the
board has no pins for (or an unknown name) falls back to `null` with a warning rather than reading
unconnected pins, so a typo cannot turn into phantom touches. Capacitive controllers additionally
need `KRYONOS_TOUCH_I2C_SDA` / `_SCL`; without the bus pins the driver stays absent and reports no
touch.

`KRYONOS_TOUCH_RST_PIN` is for a reset line wired to a **SoC GPIO** — it is driven directly. A
controller whose reset hangs off an I/O expander instead leaves it undefined and lets the board pulse
through the expander: the Waveshare 2.1B's `TP_RST` is EXIO2 on the TCA9554, so defining the macro
there would drive a pin nothing is connected to. Its environment therefore names the bus pins only.

The capacitive drivers are complete register-level implementations, but **none of them has been
validated against a real panel yet.** They compile, they probe the bus, and they report "no touch"
cleanly when nothing answers — selecting one cannot break a board that lacks the hardware. Treat their
register maps as documentation to check against your panel's datasheet.

### 3.2a The touch reset, and why it is pulsed late

`I2cTouchDriver::begin()` resets the controller **immediately before it probes** rather than during
display bring-up, and that ordering is load-bearing for the Waveshare 2.1B. Its CST820 drops into a
low-power state between touches and stops acknowledging I²C while it is there — measured on the
reference port for this board as 0/20 ACKs asleep against 20/20 awake, with the expander on the same
bus answering throughout. By the time touch init runs, the web server and the UI have come up, so a
pulse issued during panel bring-up has long expired and the probe fails on a controller that is
present, wired and healthy.

Three consequences worth keeping in mind when editing that function:

- The pulse must not move earlier, and a **second** pulse is not redundant. `begin()` runs twice per
  boot — `main.cpp`'s `TouchDriver::init()`, then `TouchCalibrator::init()` — and the second one also
  recovers a controller that fell asleep while the UI was being constructed.
- The probe **retries** (five attempts, 20 ms apart). One read can land inside the controller's
  wake-up window, and the second `begin()` runs on a busier bus than the first.
- A failed re-probe must **never** clear a success. An earlier answer is proof the part is present, so
  a flaky second read would otherwise turn into touch that is dead for the rest of the session.

Where the reset lives is board-specific: the driver reaches it through `boardExpander()` under a
`TARGET_WAVESHARE_S3_LCD21B` guard, mirroring how the shipped v2.0.0 port gates its board hook. The
expander itself is `src/Hal/I2C/Tca9554.{h,cpp}`, described in §2.6.

### 3.3 Mapping raw → pixels, and calibration

`TouchDriver::getTouch()` converts controller counts to screen pixels, asking `Display::width()` /
`height()` for the live canvas rather than assuming 240×320. Calibration is persisted to LittleFS at
`/touch_cal_p.bin` as a 10-byte tuple `[x0, dx, y0, dy, flags]` (TFT_eSPI layout; `flags` bits are
rotate `0x01`, invert_x `0x02`, invert_y `0x04`). It survives reboot and is re-applied on boot; the
serial `cal` command erases and re-runs calibration.

Because calibration is stored in **raw controller counts**, it is independent of the display
resolution — changing `KRYONOS_DISPLAY_WIDTH/HEIGHT` does **not** invalidate it. Changing
`KRYONOS_DISPLAY_ROTATION` **does** change how it must be interpreted, so **recalibrate after a
rotation change**.

`needsCalibration()` is the gate the OS reads before offering the calibration screen. It is `true`
for the resistive drivers and `false` for every capacitive one (they report absolute coordinates)
and for `null`. When it is `false`, `main.cpp` goes straight to the launcher, `/touch_cal_p.bin` is
never consulted, and `TouchCalibrator::runCalibration()` returns immediately — so a capacitive board
is never shown four corner arrows it has no way to complete.

---

## 4. Resolution & rotation contract

The logical canvas the UI draws to is `KRYONOS_DISPLAY_WIDTH × KRYONOS_DISPLAY_HEIGHT`, and
`KRYONOS_DISPLAY_ROTATION` is the value passed to `tft.setRotation()`.

| Macro | Meaning |
| :--- | :--- |
| `KRYONOS_DISPLAY_WIDTH` | Logical width **after** rotation. Defaults from `DISP_HOR_RES`, else 240. |
| `KRYONOS_DISPLAY_HEIGHT` | Logical height **after** rotation. Defaults from `DISP_VER_RES`, else 320. |
| `KRYONOS_DISPLAY_ROTATION` | `0`–`3`, passed to `tft.setRotation()`. Default 0. |
| `TFT_WIDTH` / `TFT_HEIGHT` | TFT_eSPI's **native panel** dimensions. Set only when the driver default is wrong (e.g. Cardputer `135×240`). |

The board profile is responsible for making these agree: after `tft.setRotation(KRYONOS_DISPLAY_ROTATION)`,
`tft.width()` must equal `KRYONOS_DISPLAY_WIDTH` (and likewise for height). `Display::begin()` snaps
`tft.width()/height()` into runtime metrics and logs a warning when the panel disagrees with the flags.

Everything downstream — UI layout, touch mapping, JS `System.screenWidth()/screenHeight()`, 3D
transform defaults, BMP clipping — reads the runtime metrics (`Display::width()/height()`), never the
macros and never `tft.width()` directly.

---

## 5. Toolchain: the pinned platform

Each environment pins the pioarduino distribution of the Espressif 32 platform by URL, and the pin
differs per chip because the S31 needs a newer IDF than the other two:

| Env | Pinned platform | Resolves to |
| :--- | :--- | :--- |
| `esp32s3-default`, `esp32-default` | `.../releases/download/55.03.312-1/platform-espressif32.zip` | Arduino 3.3.12 / IDF 5.5.5 (stable) |
| `esp32s31-default` | `.../releases/download/61.04.00-RC1/platform-espressif32.zip` | Arduino 4.0.0-RC1 / IDF 6.1 (**pre-release**) |

```
platform = https://github.com/pioarduino/platform-espressif32/releases/download/55.03.312-1/platform-espressif32.zip
```

**Both cores share one slot in the PlatformIO package cache.** The framework and its libs are
declared by package *name* (`framework-arduinoespressif32`, `framework-arduinoespressif32-libs`) with
the release URL as the version, so installing one core version replaces the other in
`~/.platformio/packages/` — the S31 build deletes the 3.3.12 framework and libs, and the next
`esp32s3-default` build downloads them again (`esp32-core-*.tar.xz` ~43 MB + `*-libs.tar.xz` ~387 MB
each way). If you switch between the two cores often, give the S31 its own cache:

```powershell
$env:PLATFORMIO_CORE_DIR = "$PWD\.pio-s31"   # separate platforms/packages/toolchains
pio run -e esp32s31-default
```

The 3.x pin resolves to **Arduino core 3.3.12 / ESP-IDF 5.5**. Do not change it back to a bare
`platform = espressif32`: PlatformIO resolves that name to whatever happens to be installed under
`~/.platformio/platforms/espressif32` on the machine, so whether the project builds depends on local
state rather than on the repo. (Here, a pioarduino install sat under exactly that name and silently
selected core 3.x while the code assumed core 2.x.)

Three spots were written against Arduino core 2.x and had to be adjusted for core 3.x. Keep them in
mind for any new code that touches WiFi or crypto:

| Area | Core 2.x | Core 3.x (current) |
| :--- | :--- | :--- |
| MD5 | `mbedtls_md5_starts_ret(&ctx)` | the `_ret` forms were removed in mbedTLS 3; use `mbedtls_md5_starts(&ctx)` (see `FileSystem::getFileMD5`) |
| WiFi types | `WiFiClientSecure.h` pulled in `WiFi.h` transitively | include `<WiFi.h>` explicitly for `WiFi`, `WL_CONNECTED`, `WiFiClient` |
| HTTP streams | `HTTPClient::getStreamPtr()` returns `WiFiClient*` | it returns `NetworkClient*` — use `auto*` |

The project's own `Runtime/NetworkClient.*` was also renamed to `Runtime/KryonHttpClient.*`: core 3.x
added a global `NetworkClient` class (the base of `WiFiClientSecure`), and with `src/` on the include
path the project header shadowed the framework's, producing a redefinition error. Do not reintroduce
a file or class named `NetworkClient`.

### 5.1 Installing the RISC-V toolchain by hand

`esp32s31-default` will fail with `'riscv32-esp-elf-g++' is not recognized` on a machine that has
never built it, **even though the platform, framework and libs all resolve correctly**. The pioarduino
registry publishes ~1.6 KB *manifests* (`package.json` + `tools.json` + `.piopm`) rather than payloads,
and PlatformIO's stage-2 per-OS archive download does not run for `toolchain-riscv32-esp` — so
`~/.platformio/packages/toolchain-riscv32-esp/` ends up 13 KB with no `bin/`.

To fix it, download `riscv32-esp-elf-15.2.0_20251204-x86_64-w64-mingw32.zip` (963,974,138 bytes,
sha256 `c61488aa15f49146aae918267110f775a52c3cef3844cbf261f475ef97523c3d`) and extract it into that
package directory **with the single container directory `riscv32-esp-elf/` stripped**, so `bin/` lands
at the package root (this mirrors the manifest's `strip_container_dirs: 1`). Confirm with
`riscv32-esp-elf-g++.exe --version` → `crosstool-NG esp-15.2.0_20251204`. `pio pkg install -g -p <url>`
is **not** the fix — `-p` means *platform*, and it fails with `MissingPackageManifestError`.

### 5.2 TFT_eSPI does not support the ESP32-S31

Nothing at any TFT_eSPI version drives the S31, and no fork adds it:

- **V2.5.43** (the newest published release, and what the PlatformIO registry resolves) fails to
  compile for S31 with **130 errors**, every one of them in
  `Processors/TFT_eSPI_ESP32.h` / `.c`:

  | Count | Error | Cause on S31 |
  | --- | --- | --- |
  | 67 | `'VSPI' was not declared` | `#define SPI_PORT VSPI` — no `VSPI` global |
  | 44 | `no match for 'operator=' … 'gpio_out_w1ts_reg_t' and 'int'` | `GPIO.out_w1ts = (1 << pin)` — the S31 makes these a bitfield *struct*, not an integer |
  | 12 | `invalid operands of types 'void' and 'long unsigned int'` | `READ_PERI_REG` / `SET_PERI_REG_MASK` now yield `void` |
  | 6 | `'SPI_MOSI_DLEN_REG' was not declared` | renamed (compiler suggests `SPI_MS_DLEN_REG`) |
  | 1 | `'VSPI_HOST' was not declared` | same root cause as `VSPI` |

- **master** (HEAD `16e37595040e`, 19 commits ahead) changes none of this: `Processors/` is the
  identical set of eight backends, `TFT_eSPI.h` still branches only to S3 and C3, and
  `TFT_eSPI_ESP32.c` uses the same `CONFIG_IDF_TARGET_ESP32` guards. The 19 commits are DMA fixes for
  C3/S3, STM32L4xx DMA, a CYD setup and docs.
- **Upstream** has the same wall for the P4 ([issue #3719](https://github.com/Bodmer/TFT_eSPI/issues/3719),
  open since 2025-04-29, unresolved).

TFT_eSPI special-cases only S3 and C3; every other part falls through to the classic `SPI_*_REG`
register-poking path, which the S31's new SPI/GPIO peripherals no longer expose. Do not plan on
updating TFT_eSPI — the S31's `framework-arduinoespressif32-libs` ships `include/esp_lcd`, so the path
is a `KryonDisplay` backend built on `esp_lcd` (Phase 5, §7).

---

## 6. Adding a new board

1. **Environment.** Add an `[env:<board>]` block to `platformio.ini` (or a `board_configs/<board>.ini`
   snippet registered under `[platformio] extra_configs`), mirroring an existing one: platform, board,
   framework, filesystem, partitions, `lib_deps`, and `build_flags` containing `${env.build_flags}`,
   `-D CORE_DEBUG_LEVEL=0`, `-D TARGET_<BOARD>=1`, `-D USER_SETUP_LOADED=1`, the `<CONTROLLER>_DRIVER=1`
   macro, all `TFT_*` / `SPI_*` flags, the `KRYONOS_DISPLAY_*` flags, and the touch pins. A panel that
   is not an XPT2046 also needs `-D KRYONOS_TOUCH_DRIVER=\"<name>\"` and, for a capacitive one, the
   `KRYONOS_TOUCH_I2C_SDA` / `_SCL` bus pins (§3.2).

   **If the panel is not on SPI**, do not follow that flag list. An RGB parallel panel cannot be
   driven by TFT_eSPI at all, so it needs `-D KRYONOS_DISPLAY_BACKEND=KRYONOS_BACKEND_RGB` (§2.5) and
   TFT_eSPI compiled as a rasterizer only: `USER_SETUP_LOADED`, a placeholder driver macro and the
   `LOAD_*` fonts — and **no pin macros**, nor `TFT_WIDTH`/`TFT_HEIGHT` (the driver's own defines win).
   Omitting every pin macro is
   not a shortcut: `I2CEngine::begin()` rejects a bus whose pins collide with one, which would take
   out the panel's own expander and touch controller. `[env:waveshare-s3-lcd21b]` in `platformio.ini`
   is the worked example.
2. **Profile header/impl.** Add `src/Hal/Boards/<board>/BoardConfig.h` and `.cpp`, both guarded by
   `#if defined(TARGET_<BOARD>)`, implementing the `Board.h` surface (global `tft`, capabilities, touch,
   backlight, SD, keyboard/battery). Provide `#ifndef` defaults for the resolution macros. The fastest
   start is to copy `src/Hal/Boards/esp32s3/` (SPI panel + XPT2046 touch).
3. **No shared guards.** A `TARGET_*` macro must be defined by exactly one environment, and exactly
   one board implementation may be active — otherwise the global `tft` and HAL functions are defined
   twice and the link fails.
4. **Register in CI/OTA.** A board is registered in three places and a board missing from any one of
   them silently breaks OTA for that board:
   - the `matrix.board` list and the bootloader-offset `case` in `.github/workflows/release.yml`;
   - the `target_board` choices, the `ota_*` toggles and the `ota_flags` / `all_boards` maps in
     `.github/workflows/push-update.yml`;
   - the `"boards"` keys in `updates/esp32/v2/update.json`.

   The string must be spelled identically in all three, because the firmware reports itself under
   exactly one name: `KRYONOS_BOARD_ID` in `src/Hal/Display/DisplayConfig.h`, returned verbatim by
   `OTAManager::getBoardTargetName()` and used as the manifest lookup key. There is deliberately no
   second copy of the name in the OTA code — a parallel `TARGET_*` ladder in `OTAManager` is how the
   manifest and the build matrix drifted apart before, leaving a board whose manifest entry no device
   ever asked for. When adding the manifest entry, seed it with `"supports_ota": false` and a `guide`
   until a release exists that actually carries binaries named for the new environment; `push-update.yml`
   promotes the board to `true` and fills in `firmware_url` / `firmware_size` / `firmware_md5` from the
   downloaded assets on its next run. Leaving an entry with `supports_ota: true` and no matching release
   asset produces a `firmware_url` that 404s.
5. **Docs.** Add the board to the table in `Documentation/Hardware_Architecture.md` and `README.md`.

### Per-controller notes

Some TFT_eSPI controllers need extra flags beyond the driver macro: `TFT_WIDTH`/`TFT_HEIGHT` when the
driver default is wrong, `CGRAM_OFFSET`, `TFT_RGB_ORDER`, and `TFT_INVERSION_ON`/`_OFF`. Verify rotation
`0`–`3` on real hardware — the rotation that yields the intended orientation differs between controller
families (ILI9341 vs ST7789).

---

## 7. Roadmap

This document describes the target architecture. The migration is staged so the reference boards stay
pixel-identical at 240×320 at each step:

- **Phase 0** — this document + the positive per-board guard fix + pinning the platform (§5) and
  porting the three Arduino-core-3.x incompatibilities.
- **Phase 1** — `KRYONOS_DISPLAY_*` macros and a `Display` metrics accessor as the single runtime source
  of truth; touch mapping and JS bindings stop hard-coding 240/320.
- **Phase 2** — a shared `UiLayout` metric system replacing the hard-coded 240×320 UI literals, plus a
  Python/HTML preview tool to render any resolution without flashing.
- **Phase 3** — per-screen redesign where scaling is not enough (Settings, keyboard, pagination, cloud nav).
  In progress, one screen at a time:
  - **3a — Launcher + App Store** (done). `UiLayout` gained `rowTextPadX`, `listRowTextY()`, `progressBar`,
    `listMessageY`, `dialogButtonRowY` / `dialogButtonGap` and `dialogButton()`, and `rowH` now shrinks on
    a short panel (240×135 goes from **1 row per page to 3**, with `fontBody` dropping to 1 so the text
    fits). Launcher's fixed SYSTEM section is named (`ITEM_SYSTEM_HEADER` / `ITEM_APPS_HEADER` /
    `ITEM_FIRST_APP`) instead of the bare `7` and `6` that were spread through the draw and the two
    navigation paths, and the row text / dialog buttons in both screens now come from the same helpers
    the touch handlers hit-test against.
  - **3b — Installer + Help Center** (done). `UiLayout` gained `dialogPanel()` / `dialogPanelTop()` (a
    modal panel is now anchored to the panel and clamped above the footer rather than placed at absolute
    coordinates) and `dialogButtonSpaced()`, which takes the explicit gap the Installer's dialogs need
    (40 and 10) where the App Store and Settings use the default 30. Every Installer dialog — overwrite,
    result, install/cancel, permission grant/deny, and the file-action chooser — now derives its panel
    and buttons from those helpers, and the touch handlers hit-test the same rects instead of repeating
    the draw literals. Help Center shed the last three local conventions: its row text now uses
    `listRowTextY()` (its old `row.y + 4` with `TL_DATUM` was the same pixels for font 2, but only by
    coincidence), its four-zone footer uses `footerSlot*` like the App Store instead of its own
    `footer.x + q/2 + k*q` spacing, and the four copy-pasted marquee loops collapsed into one
    `marqueeSlice()` helper with widths derived from `header.w` / `list.w`. Its viewer scroll bands were
    absolute (`y < 100`, `180 < y < 270`) and the four-zone footer was hit-tested at 70/160 while the
    labels were drawn at 40/120/200 — on a short panel the scroll-down band fell off the bottom of the
    screen entirely and text could only scroll one way; the bands are now thirds of the list area and
    the footer zones are the thirds the labels are drawn in.
  - **3c — Settings** (done). `SettingsUI.cpp` was the largest hard-coded surface (~2,100 lines). Every
    sub-screen now derives its geometry from a file-local anonymous namespace of rect helpers so the
    `draw*` and its matching `handle*Touch` walk the *same* rects rather than repeating literals: the
    saved-network cards, the WiFi status card and action list, the Manage Apps rows and action sheet,
    the Permissions cards with their pager, the Time & Region buttons and manual-time spinners, the
    About Device cards, the WiFi scanner's card stack, and the OTA progress / error / updater screens.
    Two conventions changed with them. Fixed button stacks (WiFi actions, Manage Apps, the scanner's
    page list) became scrollable lists driven by a `clampScrollWindow`-ed offset, because a stack sized
    for 320px simply falls off a 135px panel; the footer grows UP / DN labels only when the list
    really overflows. And the bespoke `fillTriangle` scroll affordances and `y >= 278 && y <= 312`
    touch bands gave way to the shared scrollbar (`scrollX` / `scrollW` / `scrollThumbMin`) and rect
    `.contains()` tests. This also fixed a real defect: `savedNetScroll` was declared but never
    assigned, so a fifth saved network was unreachable. Two one-off inconsistencies were folded away
    in passing — the OTA dismiss chip was 5px wider on the no-WiFi path than on the main path, and the
    WiFi scanner's SAVED / OPEN / SECURE tags were placed at three different x values rather than
    right-aligned in the card.
  - **3d — Keyboard** (done). The 12×4 QWERTY grid was already reading `kb*` metrics, but on a short
    panel those metrics themselves were unusable: 135px minus the 110px of chrome left a **6px key**.
    `UiLayout` now compresses the keyboard chrome when `h < 240` (prompt at y4, a 20px text box at y20,
    a 24px button row at y44, grid from y72) so 240×135 gets 15px keys under `fontBody` 1, and it
    holds out for a legible key height in general: if all four rows will not fit at 16px (12px on a
    small-font panel), the grid shows `kbRowsPerPage` rows at a time with a `kbPagerStrip` beneath it
    (PREV / page count / NEXT) — so 240×100 pages four times instead of drawing 7px keys. `kbKeyRect()`
    and `kbRowFromY()` are the shared draw/hit-test pair, and `MyKeyboard` no longer re-derives grid
    coordinates in its touch handler. On any panel tall enough for the whole keyset the pager strip is
    an empty rectangle and nothing about the keyboard changes.
  - **3e — KryonCloud** (done). `KryonCloudUI.cpp` had 62 literal coordinate sites across nine
    sub-screens. It now carries a file-local anonymous namespace of ~30 rect helpers — nav and tabs
    (`cloudNavH`, `cloudTabRect`, `cloudTabFromX`), cards and modals (`cloudCard`, `cloudModal`,
    `cloudModalInset`), splitters (`cloudSplit`, `cloudSplitInset`, `cloudModalSplit`, and
    `cloudWeighted`, which reproduces the AI screen's uneven 84/48/40/40 action bar from weights
    `{21,12,10,10}`), quota tracks (`cloudTrack`, `cloudTrackInnerW`, `cloudBarColor` — the amber/red
    thresholds are now `innerW * 3/4` and `innerW * 9/10` rather than the 154/185 that only made sense
    on a 206px bar), and the AI response window (`cloudAIPromptCard`, `cloudAIButtonY`,
    `cloudAIResponseCard`, `cloudAIResponseWindow`, `cloudAiVisibleLines`). Every sub-screen's
    `handle*Touch` hits the identical rects, so the old duplicated bands (`y >= 244 && y <= 278`,
    `y >= 58 && y <= 84`) are gone, as are the `if (y < 56)` nav checks, which are now
    `y < cloudNavH(m)`. The one place the streaming callbacks used to repeat a literal — the response
    window they repainted eight times — now shares one `repaint()` lambda with the draw path, so the
    clip and the scroll cap cannot drift from what was drawn. Two behaviours changed deliberately:
    the AI panel's scroll band is the drawn window rather than the enclosing card (the card is 6px
    taller), and the Storage file list's tap band is `cloudListBottom(m)` rather than a flat y285,
    which for the no-toast case now matches the fourth card's bottom edge exactly.
- **Phase 4** — an `ITouchDriver` interface behind the existing `TouchDriver` facade, adding capacitive
  controllers (FT6236 / GT911 / CST816) alongside XPT2046. **Done.** `TouchDriver` is now a pure
  forwarder over a factory-selected `ITouchDriver` (`src/Hal/Touch/`), the raw→pixel mapping moved to
  the Arduino-free `TouchCalibration.h` and reads `Display::*` instead of a hard-coded 240×320, and
  `KRYONOS_TOUCH_DRIVER` selects a driver by name with `auto` preserving the old ladder bit for bit.
  `needsCalibration()` is the new gate: a capacitive or absent panel skips the calibration screen and
  never consults `/touch_cal_p.bin`. The three capacitive drivers are implemented at register level
  but have no panel to be tested against.
- **Phase 5** — a `KryonDisplay` backend seam so panels TFT_eSPI cannot drive (RGB parallel, OLED,
  e-paper) can be added without rewriting the UI. **Done** for the interface and the two backends:
  `KryonDisplay.h` is the abstract surface, `TftEspiDisplay` the default adapter (it derives from both
  `TFT_eSPI` and `KryonDisplay`, so the concrete object keeps the whole TFT_eSPI API), and
  `RamFramebufferDisplay` a reference backend sharing no code with TFT_eSPI. The global `tft` is now a
  `KryonDisplay&` and every call site holds a `KryonDisplay*`; both `esp32s3-default` and
  `esp32-default` build, and the S3 target also builds with
  `-D KRYONOS_DISPLAY_BACKEND=KRYONOS_BACKEND_RAM`. Two caveats: the RAM rasterizer compiles and binds
  but has never been run, and it embeds no font, so it draws geometry only. `KryonSprite` (a
  backend-neutral sprite) and removing the remaining `TFT_*` macro dependency are outstanding.
- **Phase 6** — new-board recipe hardening, CI build matrix, and a gate that fails on reintroduced
  hard-coded dimensions. The CI/OTA registration half is done: `OTAManager::getBoardTargetName()` now
  returns `KRYONOS_BOARD_ID` directly instead of a `TARGET_*` ladder, `release.yml` derives the
  bootloader offset from the board name (`0x1000` for the classic ESP32, `0x0000` for the S3), and the
  manifest's `"boards"` keys, `push-update.yml`'s board list and the release matrix all name the three
  defaults. A PR-triggered `.github/workflows/build.yml` is still outstanding.
