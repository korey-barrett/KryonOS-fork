# Reference board snippets

These are **examples, not active build targets.** They are deliberately absent from
`[platformio] extra_configs` in `platformio.ini`, so PlatformIO never builds them.

They are kept because each one shows a complete, working recipe for a *different* panel/touch
combination, which is the fastest way to work out what your own board needs:

| Snippet | Display | Touch | Resolution |
| :--- | :--- | :--- | :--- |
| `cyd.ini` | ILI9341 SPI (`ILI9341_2_DRIVER`) | XPT2046 via `TOUCHSCREEN_*` pins | 240×320 |
| `t_hmi.ini` | ST7789 8-bit parallel (`TFT_PARALLEL_8_BIT`) | XPT2046 | 240×320 |
| `cardputer.ini` | ST7789V2 SPI, native panel 135×240 | none (keyboard) | 240×135, rotation 1 |

The matching C++ board implementations live in `src/Hal/Boards/{cyd,t_hmi,cardputer}/`. They are
compiled only when their `TARGET_*` macro is defined, so they cost nothing while unused.

## Using one

1. Copy the snippet next to this file's parent, e.g. `src/Hal/Boards/board_configs/<yourboard>.ini`.
2. Register it in `platformio.ini`:
   ```ini
   [platformio]
   extra_configs =
       src/Hal/Boards/board_configs/<yourboard>.ini
   ```
3. Give it its own `-D TARGET_<YOURBOARD>=1` and add the matching
   `src/Hal/Boards/<yourboard>/BoardConfig.{h,cpp}` pair (copy a default board and edit it).
4. Set `KRYONOS_DISPLAY_WIDTH` / `_HEIGHT` / `_ROTATION` to your panel's **post-rotation** logical
   size, and check the boot log for the `[Display] WARNING` line if the panel disagrees.

See `Documentation/Display_Touch_Architecture.md` for the full contract and the per-controller
flags (`TFT_WIDTH`/`TFT_HEIGHT`, `CGRAM_OFFSET`, `TFT_RGB_ORDER`, `TFT_INVERSION_*`).
