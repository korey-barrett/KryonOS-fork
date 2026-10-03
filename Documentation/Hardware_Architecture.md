# KryonOS Hardware Architecture & Multi-Board HAL

KryonOS features a modular **Hardware Abstraction Layer (HAL)** designed to run seamlessly across various ESP32 and ESP32-S3 hardware devices.

All board-specific pinouts, bus initializations, display controllers, and input mechanisms are encapsulated inside `src/Hal/Boards/`.

> **Adding a board, or changing the display resolution?** Read
> [Display & Touch Driver Architecture](Display_Touch_Architecture.md) first. It documents the exact
> selection chain (build flags → `TARGET_*` macro → the global `tft` symbol → touch driver), the
> `KRYONOS_DISPLAY_*` resolution/rotation contract, and the new-board recipe.

> **About to reflash a board that is already set up?** Read
> [Flash Layout, Reflashing, and What Survives](Flash_and_Persistence.md) first. Two things cost you
> the settings on the device, and neither is a normal upload: writing a factory image at offset `0x0`
> (which covers the `nvs` partition at `0x9000` and takes the KryonCloud `deviceId` with it), and
> changing `board_build.partitions` (which moves the filesystem and formats it). That page gives the
> offsets per board and lists exactly what has to be re-entered after each case.

---

## 1. Supported Board Targets

This fork ships **one default board per chip type**. Each default is a bring-up starting point — a
generic SPI panel and an XPT2046 touch controller — not a specific product. Copy one and give it your
own `TARGET_*` name for real hardware (see `Display_Touch_Architecture.md` §6).

| Board Target | Environment Name | MCU | Flash / PSRAM | Display | Input Device | App partition |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **ESP32-S3 default** *(primary)* | `esp32s3-default` | ESP32-S3 (Xtensa LX7) | 16MB / 8MB OPI | generic ILI9341 SPI, 240x320 | XPT2046 Touch | 6.5 MB |
| **ESP32 default** | `esp32-default` | ESP32 (Xtensa LX6) | 4MB / None | generic ILI9341 SPI, 240x320 | XPT2046 Touch | 3 MB (`huge_app.csv`) — **no OTA** |
| **ESP32-CYD-28** *(one product)* | `esp32-cyd-28` | ESP32 (Xtensa LX6) | 4MB / None | ILI9341 SPI, 240x320 | XPT2046 Touch (bit-banged) | 1.875 MB ×2 slots (`min_spiffs.csv`) |
| **ESP32-S31** *(preview)* | `esp32s31-default` | ESP32-S31 (RISC-V) | 16MB / 16MB OPI | generic ILI9341 SPI, 240x320 | XPT2046 Touch | 16 MB table |
| **Waveshare ESP32-S3-Touch-LCD-2.1B** *(preview)* | `waveshare-s3-lcd21b` | ESP32-S3 (Xtensa LX7) | 16MB / 8MB OPI | ST7701 480x480 **RGB parallel** | CST820 capacitive touch (I2C) | 16 MB table |

The Waveshare row is the one target that is a specific product rather than a generic bring-up board, and
the first whose panel is not on SPI at all: an RGB parallel bus has no SPI pixel path, so TFT_eSPI
cannot drive it and the display comes up through `esp_lcd_panel_rgb` instead
(`src/Hal/Display/EspLcdRgbDisplay.cpp`; see `Display_Touch_Architecture.md` §2.5). It is **not** in
`default_envs` — build it with `pio run -e waveshare-s3-lcd21b`. Its touch controller is a CST820 on
the shared I2C bus, bound to the CST816 driver from `platformio.ini`, and the board reports
`hasTouch() == true`. Note that nothing in `src/` reads that function — the real switch is the
`KRYONOS_TOUCH_*` build flags.

The CYD row is the second specific product and the only 4 MB target that can update itself in place:
`min_spiffs.csv` splits the flash into two 1.875 MB app slots, and the app measures ~1.83 MB. The
price is the filesystem — `/apps` on LittleFS drops to 128 KB, where `esp32-default` keeps 896 KB —
because 4 MB cannot hold a 3 MB app, ~900 KB of files and two 1.9 MB slots at once. It is **not** in
`default_envs`; the environment lives in `src/Hal/Boards/board_configs/cyd.ini`, registered through
`[platformio] extra_configs`, so build it with `pio run -e esp32-cyd-28`. Its touch is the XPT2046
bit-banged on four dedicated pins (PENIRQ is unconnected on this board, so the driver reads pressure).

The S31 requires ESP-IDF v6.1, which only arduino-esp32 **4.x** reaches, so `esp32s31-default` pins the
pioarduino **pre-release** platform `61.04.00-RC1` (Arduino 4.0.0-RC1 / IDF 6.1) instead of the stable
`55.03.312-1` the other two use — PlatformIO resolves `platform` per environment, so both cores coexist.
It is **not** in `default_envs`: the OS still targets the Arduino 3.x API, so build it explicitly with
`pio run -e esp32s31-default`, and expect to port code before it compiles. As of 2026-10-03 the env
does not link: the framework's own libraries all compile, then **TFT_eSPI fails with 130 IDF-6.1
errors** in its legacy ESP32 backend — so the S31 needs an `esp_lcd` display backend, not a newer
TFT_eSPI. See [Display & Touch Driver Architecture](Display_Touch_Architecture.md) §5.1–5.2.

The S31 pins in the environment are placeholders mirroring the S3 default — set `TFT_*`/`TOUCH_*` to
your actual wiring.

### 1.1 Reference boards (not built)

The upstream board ports are kept as inert templates — their implementations compile only when their
`TARGET_*` macro is defined, and their environment snippets live in
`src/Hal/Boards/board_configs/examples/`, which is not in `[platformio] extra_configs`:

| Reference | Snippet | MCU | Display | Input |
| :--- | :--- | :--- | :--- | :--- |
| M5Stack Cardputer v1.1 | `examples/cardputer.ini` | ESP32-S3 (Stamp-S3) | ST7789V2 240x135 SPI | 56-key matrix keyboard |
| LilyGO T-HMI | `examples/t_hmi.ini` | ESP32-S3 | ST7789 240x320 8-bit parallel | XPT2046 Touch |
| ESP32-CYD-28 | `examples/cyd.ini` | ESP32 | ILI9341 240x320 SPI | XPT2046 Touch |

---

## 2. Directory Structure

```
src/Hal/
├── Boards/
│   ├── Board.h                         <-- Central HAL interface (extern TFT_eSPI tft, ...)
│   ├── esp32s3/                        <-- ESP32-S3 default board (TARGET_ESP32S3_DEFAULT)
│   │   ├── BoardConfig.h
│   │   └── BoardConfig.cpp
│   ├── esp32/                          <-- ESP32 default board (TARGET_ESP32_DEFAULT)
│   │   ├── BoardConfig.h
│   │   └── BoardConfig.cpp
│   ├── esp32s31/                       <-- ESP32-S31 default board (TARGET_ESP32S31_DEFAULT)
│   ├── waveshare-s3-lcd21b/            <-- Waveshare ESP32-S3-Touch-LCD-2.1B (TARGET_WAVESHARE_S3_LCD21B)
│   │   ├── BoardConfig.h
│   │   └── BoardConfig.cpp
│   ├── board_configs/
│   │   ├── cyd.ini                     <-- ACTIVE: env esp32-cyd-28 (listed in extra_configs)
│   │   └── examples/                   <-- NOT built; reference snippets + README
│   │       ├── cardputer.ini
│   │       ├── cyd.ini                 <-- the pre-registration copy of the above
│   │       └── t_hmi.ini
│   ├── cardputer/                      <-- Cardputer reference implementation (unbuilt)
│   ├── cyd/                            <-- CYD implementation (TARGET_CYD)
│   └── t_hmi/                          <-- T-HMI reference implementation (unbuilt)
├── Display/                            <-- DisplayConfig.h + Display metrics accessor
├── Crypto/  I2C/  PWM/                 <-- Peripherals
```

---

## 3. Input Abstraction Model

KryonOS abstracts input across touch-driven devices and keyboard-driven devices:

```cpp
#include "Hal/Boards/Board.h"

// Capability queries
if (hasTouch()) {
    uint16_t x, y;
    if (getTouch(&x, &y)) {
        // Handle touch interaction
    }
}

if (hasKeyboard()) {
    BoardKey key = getKeyInput();
    if (key != BOARD_KEY_NONE) {
        char ch = keyToChar(key);
        // Handle physical keypress or navigation arrow
    }
}
```

### Cardputer Keyboard Modifier Layers:
- **`Fn` + `;` / `.` / `,` / `/`**: Arrow Up / Down / Left / Right
- **`Fn` + `` ` ``**: Real `ESC`
- **`Fn` + `Backspace`**: Real `DELETE`
- **`Shift`**: Toggles uppercase / special punctuation symbols

---

## 4. How to Build for Different Boards

`pio run` with no `-e` builds both defaults (`default_envs` in `platformio.ini`).

### ESP32-S3 default (primary):
```powershell
pio run -e esp32s3-default
```

### ESP32 default:
```powershell
pio run -e esp32-default
```

### ESP32-S31 (preview — Arduino 4.x / IDF 6.1, not built by `pio run`):
```powershell
pio run -e esp32s31-default
```

### Waveshare ESP32-S3-Touch-LCD-2.1B (preview — RGB parallel panel, not built by `pio run`):
```powershell
pio run -e waveshare-s3-lcd21b
```
Unlike every other target, this panel is not on SPI, so the `TFT_*` pin flags do not apply to it. Its
`build_flags` define no pins at all and select `KRYONOS_BACKEND_RGB`; the panel's pins, init sequence,
I²C expander and backlight are all owned by `EspLcdRgbDisplay`. See
`Display_Touch_Architecture.md` §2.5 before editing that block.

### A different panel size — no code changes:
Edit the `-D KRYONOS_DISPLAY_WIDTH` / `_HEIGHT` / `_ROTATION` lines under the environment in
`platformio.ini` to match your panel's post-rotation canvas, then rebuild. For example `320x480`,
landscape (`480x320`):

```ini
-D KRYONOS_DISPLAY_WIDTH=480
-D KRYONOS_DISPLAY_HEIGHT=320
-D KRYONOS_DISPLAY_ROTATION=1
```

Set the controller in the same block (`-D ILI9341_DRIVER=1`, `-D ST7789_DRIVER=1`, …) and the
`TFT_*` pins. If the canvas and the panel disagree, the boot log prints a `[Display] WARNING` line
naming both sizes — that is the signal to fix `TFT_WIDTH`/`TFT_HEIGHT` or the rotation.

**The one exception is a panel larger than its canvas.** `KRYONOS_DISPLAY_*` is the canvas the UI is
laid out for, and on most boards that IS the panel. On the Waveshare 2.1B it deliberately is not:
the panel is a 480x480 circle, so a 480x480 canvas loses its corners to the bezel, and the canvas is
kept at 240x320 and scaled 6/5 into a 288x384 rect centred in the panel. There the panel's own size
is a board constant (`BOARD_PANEL_W`/`BOARD_PANEL_H` in `src/Hal/Boards/waveshare-s3-lcd21b/BoardConfig.h`)
because the RGB timings are written from it, and it is not editable as a build flag. Changing
`KRYONOS_DISPLAY_*` there resizes the canvas and the aperture rect with it — the panel keeps
scanning 480x480. See `Display_Touch_Architecture.md` §2.5.

The reference boards (`m5stack-cardputer`, `lilygo-t-hmi`, `esp32-cyd-28`) are no longer active
environments; their snippets and implementations are kept as unbuilt examples (§1.1).
