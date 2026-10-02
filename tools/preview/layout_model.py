"""Host-side mirror of src/UI/UiLayout.cpp.

The firmware derives every on-device screen region from the canvas size in ``UiLayout::compute``.
This module re-implements that derivation with the *same integer arithmetic* so layouts can be
previewed and golden-tested at arbitrary resolutions without flashing hardware.

If you change UiLayout.cpp, change this file too — test_layout.py pins both to the historical
240x320 constants and will fail loudly on drift.
"""

from __future__ import annotations

import re
from pathlib import Path
from typing import Dict, Tuple

Rect = Tuple[int, int, int, int]

# Footer button slots (mirrors UiFooterButton in UiLayout.h).
FOOTER_UP = 0
FOOTER_SEL = 1
FOOTER_DN = 2

# Four-zone footer slots (mirrors UiFooterSlot in UiLayout.h) — the App Store's BACK/UP/SEL/DN bar.
SLOT_BACK = 0
SLOT_UP = 1
SLOT_SEL = 2
SLOT_DN = 3


def compute(w: int, h: int) -> Dict:
    """Return the layout metrics dict for a w x h logical canvas (post-rotation)."""
    m: Dict = {"w": w, "h": h, "landscape": w > h}
    mind = min(w, h)

    inset = mind // 80
    inset = max(2, min(8, inset))
    m["inset"] = inset

    m["frame"] = (inset, inset, w - 2 * inset, h - 2 * inset)
    m["header"] = (inset + 3, inset + 3, w - 2 * (inset + 3), 30)
    m["headerTextY"] = m["header"][1] + 15

    m["footer"] = (inset + 2, h - 35, w - 2 * (inset + 2), 30)
    m["footerTextY"] = m["footer"][1] + 15

    list_h = m["footer"][1] - (inset + 42) - 10
    if list_h < 0:
        list_h = 0
    m["list"] = (inset + 7, inset + 42, w - 2 * (inset + 7), list_h)

    # Row height is 30 on a normal panel; a short one shrinks toward a floor so at least ~4 rows fit.
    m["rowH"] = 30
    if list_h < 4 * 30:
        m["rowH"] = max(14, list_h // 4)
    m["rowFillH"] = m["rowH"] - 5
    m["rowTextPadX"] = inset + 2
    ipp = list_h // m["rowH"] if m["rowH"] > 0 else 0
    m["itemsPerPage"] = max(1, ipp)

    m["scrollX"] = w - 8
    m["scrollW"] = 3
    m["scrollThumbMin"] = 20

    m["centerX"] = w // 2
    m["centerY"] = h // 2

    m["appExitButton"] = (w - 40, 0, 40, 30)
    m["progressBar"] = (w // 8, m["centerY"], w - w // 4, 20)
    m["listMessageY"] = m["list"][1] + 55

    m["dialogButtonRowY"] = h - 90
    m["dialogButtonGap"] = w // 8

    m["fontSmall"] = 1
    m["fontBody"] = 2 if h >= 200 else 1
    m["fontHeader"] = 4 if mind >= 300 else 2

    m["cardX"] = 8
    m["cardW"] = w - 16
    m["cardH"] = 42 if h >= 240 else 30
    m["cardR"] = 6
    m["restingY"] = 8
    m["hiddenY"] = -(m["cardH"] + 2)
    m["shadowW"] = w
    m["shadowH"] = m["cardH"] + 22

    m["kbPromptX"] = 5
    m["kbButtonCount"] = 5
    m["kbCols"] = 12
    m["kbRows"] = 4
    m["kbKeyW"] = max(1, w // m["kbCols"])
    m["kbButtonW"] = w // m["kbButtonCount"]

    # A short panel compresses the chrome so the key grid keeps a usable height (see UiLayout.cpp).
    if h < 240:
        m["kbPromptY"] = 4
        m["kbTextBox"] = (5, 20, w - 10, 20)
        m["kbButtonRow"] = (0, 44, w, 24)
        m["kbGridTop"] = 72
    else:
        m["kbPromptY"] = 10
        m["kbTextBox"] = (5, 30, w - 10, 30)
        m["kbButtonRow"] = (0, 70, w, 30)
        m["kbGridTop"] = 110

    # Paging: hold out for a legible key height rather than shrinking keys to nothing.
    kb_min_key_h = 16 if m["fontBody"] >= 2 else 12
    kb_pager_h = m["fontBody"] * 10 + 2
    rows = (h - m["kbGridTop"]) // kb_min_key_h
    m["kbPagerH"] = 0
    if rows < m["kbRows"]:
        reserved = (h - m["kbGridTop"] - kb_pager_h) // kb_min_key_h
        if reserved < m["kbRows"]:
            rows = reserved
            m["kbPagerH"] = kb_pager_h
    rows = max(1, min(m["kbRows"], rows))
    m["kbRowsPerPage"] = rows
    m["kbPages"] = (m["kbRows"] + rows - 1) // rows
    m["kbKeyH"] = max(1, (h - m["kbGridTop"] - m["kbPagerH"]) // rows)

    if m["kbPagerH"] > 0:
        m["kbPagerStrip"] = (0, m["kbGridTop"] + rows * m["kbKeyH"], w, m["kbPagerH"])
    else:
        m["kbPagerStrip"] = (0, 0, 0, 0)

    return m


def _rect_cx(r: Rect) -> int:
    return r[0] + r[2] // 2


def list_row_rect(m: Dict, visible_index: int) -> Rect:
    x, y, w, _ = m["list"]
    return (x, y + visible_index * m["rowH"], w, m["rowH"])


def list_row_fill_rect(m: Dict, visible_index: int) -> Rect:
    x, y, w, _ = m["list"]
    return (x, y + visible_index * m["rowH"], w, m["rowFillH"])


def list_row_text_y(m: Dict, visible_index: int) -> int:
    """ML_DATUM baseline inside a row — row.y + 12 for the historical 30px row."""
    y = m["list"][1]
    return y + visible_index * m["rowH"] + (m["rowH"] - 6) // 2


def dialog_button_spaced(m: Dict, y: int, h: int, index: int, count: int, button_w: int,
                         gap: int) -> Rect:
    if count < 1 or index < 0 or index >= count:
        return (0, 0, 0, 0)
    total = count * button_w + (count - 1) * gap
    x0 = (m["w"] - total) // 2
    return (x0 + index * (button_w + gap), y, button_w, h)


def dialog_button(m: Dict, y: int, h: int, index: int, count: int, button_w: int) -> Rect:
    """`count` buttons of width button_w, separated by dialogButtonGap, centred on the canvas."""
    return dialog_button_spaced(m, y, h, index, count, button_w, m["dialogButtonGap"])


def dialog_panel(m: Dict, height: int) -> Rect:
    """A centred modal panel of `height` px, capped to the room above the footer."""
    height = max(1, height)
    bottom = m["footer"][1] - 15
    if height > bottom:
        height = bottom
    y = m["centerY"] - height // 2
    if y + height > bottom:
        y = bottom - height
    if y < 0:
        y = 0
    return (m["list"][0], y, m["list"][2], height)


def dialog_panel_top(m: Dict, height: int) -> Rect:
    """A modal panel anchored just below the header rather than centred (the Installer's
    file-action chooser). Same cap as dialog_panel — the room above the footer."""
    height = max(1, height)
    bottom = m["footer"][1] - 15
    if height > bottom:
        height = bottom
    y = m["header"][1] + m["header"][3] + 4
    if y + height > bottom:
        y = bottom - height
    if y < 0:
        y = 0
    return (m["list"][0], y, m["list"][2], height)


def kb_key_rect(m: Dict, row: int, col: int, page: int) -> Rect:
    """Key cell for an absolute grid row/col on `page`; empty when that row is not on the page."""
    if row < 0 or row >= m["kbRows"] or col < 0 or col >= m["kbCols"]:
        return (0, 0, 0, 0)
    local = row - page * m["kbRowsPerPage"]
    if local < 0 or local >= m["kbRowsPerPage"]:
        return (0, 0, 0, 0)
    return (col * m["kbKeyW"], m["kbGridTop"] + local * m["kbKeyH"], m["kbKeyW"], m["kbKeyH"])


def kb_row_from_y(m: Dict, y: int, page: int) -> int:
    """Absolute grid row under y on `page`, or -1 when y is outside the key grid."""
    if m["kbKeyH"] <= 0 or y < m["kbGridTop"]:
        return -1
    local = (y - m["kbGridTop"]) // m["kbKeyH"]
    if local < 0 or local >= m["kbRowsPerPage"]:
        return -1
    row = page * m["kbRowsPerPage"] + local
    return row if row < m["kbRows"] else -1


def kb_pager_prev(m: Dict) -> Rect:
    if m["kbPagerH"] <= 0:
        return (0, 0, 0, 0)
    x, y, w, h = m["kbPagerStrip"]
    return (x, y, w // 3, h)


def kb_pager_next(m: Dict) -> Rect:
    if m["kbPagerH"] <= 0:
        return (0, 0, 0, 0)
    x, y, w, h = m["kbPagerStrip"]
    third = w // 3
    return (x + w - third, y, third, h)


def in_list(m: Dict, y: int) -> bool:
    _, ly, _, lh = m["list"]
    return ly <= y < ly + lh


def list_row_from_y(m: Dict, y: int) -> int:
    if not in_list(m, y):
        return -1
    idx = (y - m["list"][1]) // m["rowH"]
    return max(0, min(m["itemsPerPage"] - 1, idx))


def in_footer(m: Dict, y: int) -> bool:
    _, fy, _, fh = m["footer"]
    return fy <= y <= fy + fh


def footer_button(m: Dict, which: int) -> Rect:
    fy = m["footer"][1]
    fh = m["footer"][3]
    q = m["w"] // 4
    if which == FOOTER_UP:
        return (0, fy, q, fh)
    if which == FOOTER_SEL:
        return (q, fy, m["w"] // 2, fh)
    if which == FOOTER_DN:
        return (3 * q, fy, m["w"] - 3 * q, fh)
    return (0, 0, 0, 0)


def footer_button_center_x(m: Dict, which: int) -> int:
    return _rect_cx(footer_button(m, which))


def footer_button_from_x(m: Dict, x: int) -> int:
    if x < m["w"] // 4:
        return FOOTER_UP
    if x < 3 * (m["w"] // 4):
        return FOOTER_SEL
    return FOOTER_DN


def _slot_edge(w: int, numerator: int) -> int:
    """Zone edge scaled from the historical 240px layout (mirrors slotEdge in UiLayout.cpp)."""
    return w * numerator // 240


def footer_slot(m: Dict, which: int) -> Rect:
    w, fy, fh = m["w"], m["footer"][1], m["footer"][3]
    b1, b2, b3 = _slot_edge(w, 70), _slot_edge(w, 130), _slot_edge(w, 200)
    if which == SLOT_BACK:
        return (0, fy, b1, fh)
    if which == SLOT_UP:
        return (b1, fy, b2 - b1, fh)
    if which == SLOT_SEL:
        return (b2, fy, b3 - b2, fh)
    if which == SLOT_DN:
        return (b3, fy, w - b3, fh)
    return (0, 0, 0, 0)


def footer_slot_center_x(m: Dict, which: int) -> int:
    return _rect_cx(footer_slot(m, which))


def footer_slot_from_x(m: Dict, x: int) -> int:
    w = m["w"]
    if x < _slot_edge(w, 70):
        return SLOT_BACK
    if x < _slot_edge(w, 130):
        return SLOT_UP
    if x < _slot_edge(w, 200):
        return SLOT_SEL
    return SLOT_DN


# ---------------------------------------------------------------------------------------------
# Board discovery: read resolution/rotation from the PlatformIO envs so the preview reflects the
# exact same numbers a firmware build would use.
# ---------------------------------------------------------------------------------------------

_DEFAULT_RES = (240, 320, 0)

_SECTION_RE = re.compile(r"^\[env:([^\]]+)\]\s*$", re.MULTILINE)


def _extract_macros(section_text: str) -> Dict[str, int]:
    """Pull `-D KEY=VALUE` integer defines out of a section's build_flags text."""
    out: Dict[str, int] = {}
    for key, val in re.findall(r"-D\s+([A-Za-z0-9_]+)\s*=\s*(\d+)\b", section_text):
        out[key] = int(val)
    return out


def _resolve(macros: Dict[str, int]) -> Tuple[int, int, int]:
    """Return (width, height, rotation) with the same fallback chain as DisplayConfig.h."""
    def pick(*names):
        for n in names:
            if n in macros and macros[n] > 0:
                return macros[n]
        return None

    w = pick("KRYONOS_DISPLAY_WIDTH", "DISP_HOR_RES") or _DEFAULT_RES[0]
    h = pick("KRYONOS_DISPLAY_HEIGHT", "DISP_VER_RES") or _DEFAULT_RES[1]
    rot = macros.get("KRYONOS_DISPLAY_ROTATION", _DEFAULT_RES[2])
    return w, h, rot


def discover_boards(repo_root: Path) -> Dict[str, Tuple[int, int, int]]:
    """Map env name -> (width, height, rotation) across platformio.ini and board_configs/*.ini."""
    boards: Dict[str, Tuple[int, int, int]] = {}
    files = [repo_root / "platformio.ini"]
    files += sorted((repo_root / "src" / "Hal" / "Boards" / "board_configs").glob("*.ini"))
    for path in files:
        if not path.exists():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        matches = list(_SECTION_RE.finditer(text))
        for i, mt in enumerate(matches):
            start = mt.end()
            end = matches[i + 1].start() if i + 1 < len(matches) else len(text)
            boards[mt.group(1)] = _resolve(_extract_macros(text[start:end]))
    return boards
