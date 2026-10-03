# Font data — vendored from TFT_eSPI

These three files are **data**, not code, and are copied **verbatim** from TFT_eSPI 2.5.43
(`.pio/libdeps/esp32s3-default/TFT_eSPI/Fonts/`). They are byte-identical to the originals; nothing in
this directory has been edited. The `Font16.h` / `Font32rle.h` companions were dropped because they
`#include <Fonts/Font16.c>` through TFT_eSPI's own directory layout; the tables are declared directly
in `KryonText.cpp` instead.

| File | Font id | Contents |
| :--- | :--- | :--- |
| `glcdfont.c` | 1 | Adafruit_GFX 5x7 GLCD font, 5 bytes per glyph, 96 glyphs |
| `Font16.c` | 2 | 16 px variable-width font; `chrtbl_f16[96]` pointers + `widtbl_f16[96]` |
| `Font32rle.c` | 4 | 26 px font, 8-bit run-length encoded |

## Why these and not something new

KryonOS's entire UI draws through TFT_eSPI's numeric font ids, and the only three it ever passes are
**1, 2 and 4** — `UiLayout` picks `fontSmall=1`, `fontBody=2`, `fontHeader=4`. On the boards that use
TFT_eSPI these come from these exact tables, and `UiLayout` lays the whole UI out against the advance
widths they produce.

The ESP32-S31 cannot use TFT_eSPI at all — its processor layer does not compile for the chip (see
`Documentation/Display_Touch_Architecture.md` §5.2). But the *fonts* are ordinary C arrays with no
dependence on that layer. Reusing them means the Korvo-1 renders text that is pixel-identical to the
other boards and `textWidth()` reports the same numbers, so the layout needs no legibility pass at
all — which is the whole reason this is worth doing rather than designing a new font.

## Licence

The font data carries the same terms as TFT_eSPI itself, which is derived from the MIT-licensed
`Adafruit_ILI9341` library. TFT_eSPI's `license.txt` is reproduced in full at
`idf/components/kryonos_s31_display/fonts/TFT_eSPI_license.txt`. The 5x7 GLCD font originates from
Adafruit_GFX and is likewise permissively licensed. Attribution is preserved here rather than in each
file's header, since the files are unmodified.
