# KryonOS Hardware Architecture & Multi-Board HAL

KryonOS features a modular **Hardware Abstraction Layer (HAL)** designed to run seamlessly across various ESP32 and ESP32-S3 hardware devices.

All board-specific pinouts, bus initializations, display controllers, and input mechanisms are encapsulated inside `src/Hal/Boards/`.

> **Adding a board, or changing the display resolution?** Read
> [Display & Touch Driver Architecture](Display_Touch_Architecture.md) first. It documents the exact
> selection chain (build flags → `TARGET_*` macro → the global `tft` symbol → touch driver), the
> `KRYONOS_DISPLAY_*` resolution/rotation contract, and the new-board recipe.

---

## 1. Supported Board Targets

This fork ships **one default board per chip type**. Each default is a bring-up starting point — a
generic SPI panel and an XPT2046 touch controller — not a specific product. Copy one and give it your
own `TARGET_*` name for real hardware (see `Display_Touch_Architecture.md` §6).

| Board Target | Environment Name | MCU | Flash / PSRAM | Display | Input Device | App partition |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| **ESP32-S3 default** *(primary)* | `esp32s3-default` | ESP32-S3 (Xtensa LX7) | 16MB / 8MB OPI | generic ILI9341 SPI, 240x320 | XPT2046 Touch | 6.5 MB |
| **ESP32 default** | `esp32-default` | ESP32 (Xtensa LX6) | 4MB / None | generic ILI9341 SPI, 240x320 | XPT2046 Touch | 3 MB (`huge_app.csv`) — **no OTA** |
| **ESP32-S31** *(preview)* | `esp32s31-default` | ESP32-S31 (RISC-V) | 16MB / 16MB OPI | generic ILI9341 SPI, 240x320 | XPT2046 Touch | 16 MB table |

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
│   ├── board_configs/
│   │   └── examples/                   <-- NOT built; reference snippets + README
│   │       ├── cardputer.ini
│   │       ├── cyd.ini
│   │       └── t_hmi.ini
│   ├── cardputer/                      <-- Cardputer reference implementation (unbuilt)
│   ├── cyd/                            <-- CYD reference implementation (unbuilt)
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

The reference boards (`m5stack-cardputer`, `lilygo-t-hmi`, `esp32-cyd-28`) are no longer active
environments; their snippets and implementations are kept as unbuilt examples (§1.1).
