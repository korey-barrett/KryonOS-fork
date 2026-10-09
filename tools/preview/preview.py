"""Render KryonOS on-device screen geometry at any resolution — without flashing hardware.

Produces a self-contained HTML page with one labelled SVG per resolution showing the regions
UiLayout derives: frame, header, list rows, footer buttons, scrollbar, notification card, app-exit
button and the on-screen keyboard. Use it to sanity-check a new panel size before building.

Usage:
  python tools/preview/preview.py --all
  python tools/preview/preview.py --board esp32s3-default
  python tools/preview/preview.py --width 480 --height 320 --rotation 1
  python tools/preview/preview.py --all --out tools/preview/out

Boards and their resolutions are read from platformio.ini and board_configs/*.ini, so the preview
reflects exactly the numbers a firmware build would use.
"""

from __future__ import annotations

import argparse
import html
import sys
from pathlib import Path
from typing import Dict, List, Optional, Tuple

sys.path.insert(0, str(Path(__file__).resolve().parent))

import layout_model as lm  # noqa: E402

REPO_ROOT = Path(__file__).resolve().parents[2]

# A spread of resolutions worth eyeballing, beyond the real boards.
TEST_RESOLUTIONS: List[Tuple[int, int, int]] = [
    (240, 320, 0),   # legacy portrait
    (320, 240, 1),   # legacy landscape
    (240, 135, 1),   # Cardputer
    (320, 480, 0),   # 3.5" panels (ILI9488/ST7796)
    (480, 320, 1),   # 4" landscape
    (800, 480, 1),   # 5" RGB / high-res
]


def _rect(x, y, w, h, stroke, fill="none", dash=None, sw=1.0) -> str:
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return (f'<rect x="{x}" y="{y}" width="{w}" height="{h}" fill="{fill}" '
            f'stroke="{stroke}" stroke-width="{sw}"{d} />')


def _text(x, y, s, color="#333", size=9, anchor="start") -> str:
    return (f'<text x="{x}" y="{y}" fill="{color}" font-size="{size}" '
            f'font-family="monospace" text-anchor="{anchor}">{html.escape(s)}</text>')


def render_svg(m: Dict, title: str) -> str:
    w, h = m["w"], m["h"]
    pad = 28
    parts: List[str] = []

    # frame / header / list / footer
    parts.append(_rect(*m["frame"], "#888", sw=1.5))
    parts.append(_rect(*m["header"], "#22a06b"))
    parts.append(_text(m["header"][0] + 4, m["headerTextY"] + 3, "header", "#0a6b45"))
    parts.append(_rect(*m["list"], "#2f6fd0"))
    parts.append(_text(m["list"][0] + 4, m["list"][1] + 12, "list", "#2f6fd0"))

    # list rows
    for i in range(m["itemsPerPage"]):
        rx, ry, rw, rh = lm.list_row_rect(m, i)
        if ry + rh > m["list"][1] + m["list"][3]:
            break
        parts.append(_rect(rx, ry, rw, rh, "#9dbdf0", dash="3 3", sw=0.6))

    # footer + its three buttons
    parts.append(_rect(*m["footer"], "#d08a00"))
    for which, label in ((lm.FOOTER_UP, "UP"), (lm.FOOTER_SEL, "SEL"), (lm.FOOTER_DN, "DN")):
        bx, by, bw, bh = lm.footer_button(m, which)
        parts.append(_rect(bx, by, bw, bh, "#d08a00", dash="2 2", sw=0.6))
        parts.append(_text(lm.footer_button_center_x(m, which), m["footerTextY"] + 3,
                           label, "#8a5a00", anchor="middle"))

    # The App Store's four-zone footer (BACK/UP/SEL/DN) shares this strip. Its zone edges are drawn
    # as dividers only, so they can be compared against the three-button split without burying it.
    fy, fh = m["footer"][1], m["footer"][3]
    for slot in (lm.SLOT_UP, lm.SLOT_SEL, lm.SLOT_DN):
        sx = lm.footer_slot(m, slot)[0]
        parts.append(f'<line x1="{sx}" y1="{fy}" x2="{sx}" y2="{fy + fh}" '
                     f'stroke="#c060a0" stroke-width="1" stroke-dasharray="2 3" />')

    # scrollbar
    parts.append(_rect(m["scrollX"], m["list"][1], m["scrollW"], m["list"][3], "#666", fill="#666"))

    # app exit button
    parts.append(_rect(*m["appExitButton"], "#cc3333", fill="#fdd"))
    parts.append(_text(m["appExitButton"][0] + 20, m["appExitButton"][1] + 19, "X",
                       "#cc3333", anchor="middle"))

    # notification card (at rest)
    cx, cy = m["cardX"], m["restingY"]
    parts.append(f'<rect x="{cx}" y="{cy}" width="{m["cardW"]}" height="{m["cardH"]}" '
                 f'rx="{m["cardR"]}" fill="#fff6d5" stroke="#b58900" />')
    parts.append(_text(cx + 6, cy + 14, "notification card", "#8a6a00"))

    # keyboard
    parts.append(_rect(*m["kbTextBox"], "#7a3fb0"))
    parts.append(_rect(*m["kbButtonRow"], "#7a3fb0", dash="2 2", sw=0.6))
    for i in range(m["kbButtonCount"]):
        parts.append(_rect(i * m["kbButtonW"], m["kbButtonRow"][1], m["kbButtonW"],
                           m["kbButtonRow"][3], "#7a3fb0", dash="2 2", sw=0.5))
    for gy in range(m["kbRows"]):
        for gx in range(m["kbCols"]):
            parts.append(_rect(gx * m["kbKeyW"], m["kbGridTop"] + gy * m["kbKeyH"],
                               m["kbKeyW"], m["kbKeyH"], "#c3a6e0", sw=0.4))

    # canvas bounds
    parts.append(_rect(0, 0, w, h, "#000", sw=1.2))

    return (f'<figure><figcaption>{html.escape(title)} — {w}x{h}</figcaption>'
            f'<svg viewBox="-2 -2 {w + 4} {h + 4}" width="{w}" height="{h}" '
            f'style="max-width:100%;height:auto;background:#fff">'
            + "".join(parts) + "</svg></figure>")


def render_page(screens: List[Tuple[str, Dict]]) -> str:
    body = "\n".join(render_svg(m, t) for t, m in screens)
    return f"""<!doctype html>
<html lang="en"><head><meta charset="utf-8">
<title>KryonOS layout preview</title>
<style>
  body {{ font-family: system-ui, sans-serif; margin: 24px; background:#f6f7f9; color:#222; }}
  h1 {{ font-size: 20px; }}
  .grid {{ display:flex; flex-wrap:wrap; gap:24px; align-items:flex-start; }}
  figure {{ margin:0; background:#fff; padding:12px; border:1px solid #ddd; border-radius:8px; }}
  figcaption {{ font-size:12px; font-weight:600; margin-bottom:8px; }}
  .legend {{ font-size:12px; color:#555; margin:12px 0 24px; }}
</style></head>
<body>
<h1>KryonOS layout preview</h1>
<p class="legend">frame (grey) · header (green) · list + rows (blue) · footer + UP/SEL/DN (amber) ·
App Store BACK/UP/SEL/DN zone edges (pink, dashed) · scrollbar (dark) · app-exit (red) ·
notification card (yellow) · keyboard (purple).</p>
<div class="grid">
{body}
</div>
</body></html>
"""


def _kb(name: str) -> Dict:
    """The env's keyboard grid shape, or None for the shared 12x4 default.

    A resolution alone does not imply the grid -- the Waveshare declares 6x6 (see KB_SHAPE in
    layout_model.py, mirrored from KRYONOS_KB_* in UiLayout.h). Rendering it with the shared grid
    would draw keys the board does not have, in the one place the keyboard is easiest to eyeball.
    """
    return lm.KB_SHAPE.get(name)


def build_screens(args) -> List[Tuple[str, Dict]]:
    screens: List[Tuple[str, Dict]] = []

    if args.width and args.height:
        screens.append((f"custom (rotation {args.rotation})",
                        lm.compute(args.width, args.height)))
        return screens

    boards = lm.discover_boards(REPO_ROOT)

    if args.board:
        if args.board not in boards:
            raise SystemExit(f"unknown board '{args.board}'. Known: {', '.join(sorted(boards))}")
        w, h, rot = boards[args.board]
        screens.append((f"{args.board} (rotation {rot})", lm.compute(w, h, _kb(args.board))))
        return screens

    # default / --all: every discovered board, then the test strip.
    for name in sorted(boards):
        w, h, rot = boards[name]
        screens.append((f"{name} (rotation {rot})", lm.compute(w, h, _kb(name))))
    if args.all:
        for (w, h, rot) in TEST_RESOLUTIONS:
            screens.append((f"test {w}x{h} (rotation {rot})", lm.compute(w, h)))
    return screens


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--board", help="environment name to render (e.g. esp32s3-default)")
    ap.add_argument("--width", type=int, help="custom logical width")
    ap.add_argument("--height", type=int, help="custom logical height")
    ap.add_argument("--rotation", type=int, default=0, help="rotation label for custom sizes")
    ap.add_argument("--all", action="store_true", help="also render the standard test resolutions")
    ap.add_argument("--out", default=str(REPO_ROOT / "tools" / "preview" / "out"),
                    help="output directory (default tools/preview/out)")
    args = ap.parse_args()

    if bool(args.width) ^ bool(args.height):
        raise SystemExit("--width and --height must be given together")

    screens = build_screens(args)
    out_dir = Path(args.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    out_file = out_dir / "layout.html"
    out_file.write_text(render_page(screens), encoding="utf-8")
    print(f"wrote {out_file} ({len(screens)} screen(s))")
    for title, m in screens:
        print(f"  {title}: {m['w']}x{m['h']}  list rows/page={m['itemsPerPage']}  "
              f"key={m['kbKeyW']}x{m['kbKeyH']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
