"""Golden tests for the UiLayout derivation.

Guarantees that layout_model.py (and therefore, by the documented contract, UiLayout.cpp)
reproduces the historical 240x320 geometry exactly, and that common alternative resolutions
produce sane, non-degenerate regions.

Run:  python tools/preview/test_layout.py
"""

from __future__ import annotations

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))

import layout_model as lm  # noqa: E402

FAILURES = []


def check(cond: bool, msg: str) -> None:
    if cond:
        print(f"  ok   {msg}")
    else:
        print(f"  FAIL {msg}")
        FAILURES.append(msg)


def check_eq(actual, expected, msg: str) -> None:
    check(actual == expected, f"{msg} (got {actual!r}, want {expected!r})")


def test_legacy_240x320() -> None:
    print("legacy 240x320 (must match the pre-refactor constants):")
    m = lm.compute(240, 320)
    check_eq(m["frame"], (3, 3, 234, 314), "frame")
    check_eq(m["header"], (6, 6, 228, 30), "header")
    check_eq(m["headerTextY"], 21, "headerTextY")
    check_eq(m["list"], (10, 45, 220, 230), "list")
    check_eq(m["footer"], (5, 285, 230, 30), "footer")
    check_eq(m["footerTextY"], 300, "footerTextY")
    check_eq(m["scrollX"], 232, "scrollbar x")
    check_eq(m["scrollW"], 3, "scrollbar width")
    check_eq(m["centerX"], 120, "centerX")
    check_eq(m["centerY"], 160, "centerY")
    check_eq(m["rowH"], 30, "rowH")
    check_eq(m["rowFillH"], 25, "rowFillH")
    check_eq(m["rowTextPadX"], 5, "row text pad x")
    check_eq(lm.list_row_text_y(m, 0), 57, "row 0 text baseline (was row.y + 12)")
    check_eq(lm.list_row_text_y(m, 1), 87, "row 1 text baseline")
    check_eq(m["itemsPerPage"], 7, "itemsPerPage")
    check_eq(m["appExitButton"], (200, 0, 40, 30), "app exit button")
    check_eq(m["progressBar"], (30, 160, 180, 20), "progress bar")
    check_eq(m["listMessageY"], 100, "empty-state message y (was a literal 100)")
    check_eq(m["dialogButtonRowY"], 230, "dialog button row y")
    check_eq(m["dialogButtonGap"], 30, "dialog button gap")
    check_eq(lm.dialog_button(m, 230, 30, 0, 2, 80), (25, 230, 80, 30), "2-button dialog, left")
    check_eq(lm.dialog_button(m, 230, 30, 1, 2, 80), (135, 230, 80, 30), "2-button dialog, right")
    check_eq(lm.dialog_button(m, 220, 30, 0, 1, 70), (85, 220, 70, 30), "single centred dialog button")
    # Installer dialogs: panels reproduce their historical rects, and the two non-default gaps hold.
    check_eq(lm.dialog_panel(m, 160), (10, 80, 220, 160), "dialog panel h=160 (App Exists)")
    check_eq(lm.dialog_panel(m, 200), (10, 60, 220, 200), "dialog panel h=200")
    check_eq(lm.dialog_panel(m, 240), (10, 30, 220, 240), "dialog panel h=240 (clamped up)")
    # The Installer's file-action chooser: anchored below the header, not centred.
    check_eq(lm.dialog_panel_top(m, 160), (10, 40, 220, 160), "top-anchored dialog panel h=160")
    check_eq(lm.dialog_button_spaced(m, 180, 30, 0, 2, 70, 40), (30, 180, 70, 30), "Yes/No left (gap 40)")
    check_eq(lm.dialog_button_spaced(m, 180, 30, 1, 2, 70, 40), (140, 180, 70, 30), "Yes/No right")
    check_eq(lm.dialog_button_spaced(m, 230, 30, 0, 2, 95, 10), (20, 230, 95, 30), "Grant/Deny left (gap 10)")
    check_eq(lm.dialog_button_spaced(m, 230, 30, 1, 2, 95, 10), (125, 230, 95, 30), "Grant/Deny right")
    check_eq(m["cardX"], 8, "card x")
    check_eq(m["cardW"], 224, "card width")
    check_eq(m["cardH"], 42, "card height")
    check_eq(m["cardR"], 6, "card radius")
    check_eq(m["hiddenY"], -44, "card hiddenY")
    check_eq(m["shadowW"], 240, "shadow width")
    check_eq(m["shadowH"], 64, "shadow height")
    check_eq(m["kbButtonW"], 48, "kb button width (240/5)")
    check_eq(m["kbKeyW"], 20, "kb key width (240/12)")
    check_eq(m["kbKeyH"], 52, "kb key height ((320-110)/4)")
    # The full-height keyboard keeps the historical chrome and never pages.
    check_eq(m["kbPromptY"], 10, "kb prompt y")
    check_eq(m["kbTextBox"], (5, 30, 230, 30), "kb text box")
    check_eq(m["kbButtonRow"], (0, 70, 240, 30), "kb button row")
    check_eq(m["kbGridTop"], 110, "kb grid top")
    check_eq(m["kbRowsPerPage"], 4, "all four key rows fit")
    check_eq(m["kbPages"], 1, "one page")
    check_eq(m["kbPagerH"], 0, "no pager strip")
    check_eq(m["kbPagerStrip"], (0, 0, 0, 0), "pager strip is empty")
    check_eq(lm.kb_key_rect(m, 0, 0, 0), (0, 110, 20, 52), "key (0,0)")
    check_eq(lm.kb_key_rect(m, 3, 11, 0), (220, 266, 20, 52), "key (3,11) bottom-right")
    check_eq(lm.kb_key_rect(m, 3, 11, 1), (0, 0, 0, 0), "row 3 is not on page 1")
    check_eq(lm.kb_row_from_y(m, 110, 0), 0, "y=110 -> row 0")
    check_eq(lm.kb_row_from_y(m, 266, 0), 3, "y=266 -> row 3")
    check_eq(lm.kb_row_from_y(m, 109, 0), -1, "above the grid -> -1")
    check_eq(lm.kb_row_from_y(m, 318, 0), -1, "below the last row -> -1")
    check_eq(m["fontHeader"], 2, "header font (240x320 uses font 2)")


def test_footer_buttons_240() -> None:
    print("footer buttons at 240 wide (UP<60 / SEL 60..180 / DN>=180):")
    m = lm.compute(240, 320)
    check_eq(lm.footer_button(m, lm.FOOTER_UP), (0, 285, 60, 30), "UP rect")
    check_eq(lm.footer_button(m, lm.FOOTER_SEL), (60, 285, 120, 30), "SEL rect")
    check_eq(lm.footer_button(m, lm.FOOTER_DN), (180, 285, 60, 30), "DN rect")
    check_eq(lm.footer_button_center_x(m, lm.FOOTER_UP), 30, "UP label x")
    check_eq(lm.footer_button_center_x(m, lm.FOOTER_SEL), 120, "SEL label x")
    check_eq(lm.footer_button_center_x(m, lm.FOOTER_DN), 210, "DN label x")
    check_eq(lm.footer_button_from_x(m, 10), lm.FOOTER_UP, "tap x=10 -> UP")
    check_eq(lm.footer_button_from_x(m, 120), lm.FOOTER_SEL, "tap x=120 -> SEL")
    check_eq(lm.footer_button_from_x(m, 230), lm.FOOTER_DN, "tap x=230 -> DN")


def test_footer_slots_240() -> None:
    print("four-zone footer slots at 240 wide (App Store BACK/UP/SEL/DN, legacy 35/100/165/220):")
    m = lm.compute(240, 320)
    check_eq(lm.footer_slot(m, lm.SLOT_BACK), (0, 285, 70, 30), "BACK zone")
    check_eq(lm.footer_slot(m, lm.SLOT_UP), (70, 285, 60, 30), "UP zone")
    check_eq(lm.footer_slot(m, lm.SLOT_SEL), (130, 285, 70, 30), "SEL zone")
    check_eq(lm.footer_slot(m, lm.SLOT_DN), (200, 285, 40, 30), "DN zone")
    # The drawn labels must land on the pre-refactor hard-coded x positions...
    check_eq(lm.footer_slot_center_x(m, lm.SLOT_BACK), 35, "BACK label x (was 35)")
    check_eq(lm.footer_slot_center_x(m, lm.SLOT_UP), 100, "UP label x (was 100)")
    check_eq(lm.footer_slot_center_x(m, lm.SLOT_SEL), 165, "SEL label x (was 165)")
    check_eq(lm.footer_slot_center_x(m, lm.SLOT_DN), 220, "DN label x (was 220)")
    # ...and the dividers on the pre-refactor zone edges.
    check_eq(lm.footer_slot(m, lm.SLOT_UP)[0], 70, "divider at 70 (was 70)")
    check_eq(lm.footer_slot(m, lm.SLOT_SEL)[0], 130, "divider at 130 (was 130)")
    check_eq(lm.footer_slot(m, lm.SLOT_DN)[0], 200, "divider at 200 (was 200)")
    # Tapping a label centre must select that very slot — the draw/hit-test coupling.
    for name, slot in (("BACK", lm.SLOT_BACK), ("UP", lm.SLOT_UP),
                       ("SEL", lm.SLOT_SEL), ("DN", lm.SLOT_DN)):
        cx = lm.footer_slot_center_x(m, slot)
        check_eq(lm.footer_slot_from_x(m, cx), slot, f"tap on {name} label -> {name}")


def test_list_rows_240() -> None:
    print("list row hit-testing at 240x320:")
    m = lm.compute(240, 320)
    check_eq(lm.list_row_from_y(m, 45), 0, "y=45 -> row 0")
    check_eq(lm.list_row_from_y(m, 74), 0, "y=74 -> row 0")
    check_eq(lm.list_row_from_y(m, 75), 1, "y=75 -> row 1")
    check_eq(lm.list_row_from_y(m, 270), 6, "y=270 clamps to last visible row")
    check_eq(lm.list_row_from_y(m, 10), -1, "y=10 is outside the list")
    check(lm.in_footer(m, 300), "y=300 is in the footer")
    check(not lm.in_footer(m, 100), "y=100 is not in the footer")


def test_short_panel_240x135() -> None:
    print("240x135 short panel: rows shrink so a list is still usable:")
    m = lm.compute(240, 135)
    check_eq(m["list"], (9, 44, 222, 46), "list region")
    check_eq(m["rowH"], 14, "row height shrinks to the 14px floor")
    check_eq(m["rowFillH"], 9, "row fill height")
    check_eq(m["rowTextPadX"], 4, "row text pad x follows the inset")
    check(m["itemsPerPage"] >= 3, f"at least 3 rows per page (got {m['itemsPerPage']})")
    check_eq(m["fontBody"], 1, "body font drops to 1 so text fits a 14px row")
    check_eq(lm.list_row_text_y(m, 0), 48, "row 0 baseline is centred in the 14px row")
    # Dialog buttons used to sit at a hard-coded y=230, which is off a 135px screen entirely.
    check(m["dialogButtonRowY"] + 30 <= 135, "dialog button row stays on-screen")
    check_eq(lm.dialog_button(m, m["dialogButtonRowY"], 30, 0, 2, 80), (25, 45, 80, 30),
             "2-button dialog stays centred (left)")
    check_eq(lm.dialog_button(m, m["dialogButtonRowY"], 30, 1, 2, 80), (135, 45, 80, 30),
             "2-button dialog stays centred (right)")
    # The keyboard compresses its chrome instead of leaving 6px keys under the old 110px grid top.
    check_eq(m["kbGridTop"], 72, "keyboard grid starts higher")
    check_eq(m["kbKeyH"], 15, "keys stay a legible 15px with font 1")
    check_eq(m["kbRowsPerPage"], 4, "all four rows still fit at 135px")
    check_eq(m["kbPagerH"], 0, "no pager needed at 135px")
    check(m["kbButtonRow"][1] + m["kbButtonRow"][3] <= m["kbGridTop"],
          "the button row does not overlap the key grid")


def test_keyboard_paging_240x100() -> None:
    print("240x100 keyboard pages rather than shrinking keys below legibility:")
    m = lm.compute(240, 100)
    check(m["kbKeyH"] >= 12, f"keys stay >= 12px (got {m['kbKeyH']})")
    check(m["kbPages"] > 1, f"the grid pages (got {m['kbPages']})")
    check(m["kbRowsPerPage"] < 4, "fewer than four rows are shown at once")
    check_eq(m["kbRowsPerPage"] * m["kbPages"] >= 4, True, "every row is reachable")
    strip = m["kbPagerStrip"]
    check_eq(strip[1] + strip[3], 100, "the pager strip reaches the bottom edge")
    check_eq(strip[1], m["kbGridTop"] + m["kbRowsPerPage"] * m["kbKeyH"],
             "the pager strip sits directly under the visible rows")
    # PREV / NEXT tile the strip's outer thirds and must select their own page.
    check_eq(lm.kb_pager_prev(m), (0, strip[1], 80, strip[3]), "PREV zone")
    check_eq(lm.kb_pager_next(m), (160, strip[1], 80, strip[3]), "NEXT zone")
    # A row on page 0 is not on page 1, and vice versa.
    rows = m["kbRowsPerPage"]
    check_eq(lm.kb_key_rect(m, 0, 0, 1), (0, 0, 0, 0), "page-0 row is empty on page 1")
    check(lm.kb_key_rect(m, rows, 0, 1) != (0, 0, 0, 0), "page-1's first row has a cell")


def test_waveshare_keyboard_201x268() -> None:
    """The Waveshare 2.1B declares its own keyboard grid on its own 201x268 canvas."""
    print("waveshare-s3-lcd21b keyboard (board-declared 6x6 grid on a 201x268 canvas):")
    shape = lm.KB_SHAPE["waveshare-s3-lcd21b"]
    m = lm.compute(201, 268, shape)

    check_eq((m["kbCols"], m["kbRows"]), (6, 6), "grid is 6x6")
    check_eq((m["kbButtonCount"], m["kbButtonW"]), (6, 33), "six 33px top buttons")
    check_eq((m["kbKeyW"], m["kbKeyH"]), (33, 32), "keys are 33x32")
    check_eq(m["kbCharPages"], 2, "two character pages (letters+digits, then symbols)")
    # The whole grid fits, so the ROW pager is inert and no strip is drawn -- the SYM button owns
    # page switching on this board, not the pager.
    check_eq(m["kbRowsPerPage"], 6, "all six rows fit")
    check_eq(m["kbPages"], 1, "row pager is inert")
    check_eq(m["kbPagerH"], 0, "no pager strip")
    # This board claims the compact chrome (KRYONOS_KB_COMPACT_CHROME). It is not cosmetic: at 268
    # tall the full 110px chrome would leave the six rows 26px each, where the compact 72px grid top
    # gives 32px. Everything from here down is the compact branch, not the reference layout.
    check_eq((m["kbPromptY"], m["kbTextBox"], m["kbButtonRow"], m["kbGridTop"]),
             (4, (5, 20, 191, 20), (0, 44, 201, 24), 72), "compact chrome")
    check(m["kbButtonRow"][1] + m["kbButtonRow"][3] <= m["kbGridTop"],
          "the button row does not overlap the key grid")
    # The grid reaches both canvas edges exactly: no clipped column, no unreachable row.
    check_eq(lm.kb_key_rect(m, 0, 0, 0), (0, 72, 33, 32), "key (0,0)")
    check_eq(lm.kb_key_rect(m, 5, 5, 0), (165, 232, 33, 32), "key (5,5) bottom-right")
    check_eq(lm.kb_row_from_y(m, 72, 0), 0, "y=72 -> row 0")
    check_eq(lm.kb_row_from_y(m, 263, 0), 5, "y=263 -> row 5")
    check_eq(lm.kb_row_from_y(m, 71, 0), -1, "above the grid -> -1")
    check_eq(lm.kb_row_from_y(m, 267, 0), -1, "below the last row -> -1")
    # The point of the change: the failing axis is wider than the shared grid's.
    shared = lm.compute(240, 320)
    check_eq(shared["kbKeyW"], 20, "shared 12x4 key width is still 20")
    check(m["kbKeyW"] > shared["kbKeyW"], f"keys are wider (got {m['kbKeyW']})")


def test_waveshare_canvas_geometry() -> None:
    """The board's canvas size is load-bearing twice over; pin what it is chosen for.

    Mirrors platformio.ini's canvas note and the aperture block in EspLcdRgbDisplay.h.
    """
    print("waveshare-s3-lcd21b canvas geometry (201x268 -> 288x384 at 96/67):")
    m = lm.compute(201, 268, lm.KB_SHAPE["waveshare-s3-lcd21b"])
    # 3:4 is what keeps the inscribed rect on 288x384 for ANY canvas size, which is what lets the
    # canvas shrink to magnify rather than to shrink the picture.
    check_eq(201 * 4, 268 * 3, "the canvas is exactly 3:4")
    # The aperture the UI is actually magnified by, and the rect it lands on.
    check_eq((201 * 96 + 66) // 67, 288, "canvas width blows up to the 288px aperture")
    check_eq((268 * 96 + 66) // 67, 384, "canvas height blows up to the 384px aperture")
    # Corners exactly on the 480px bezel: 144^2 + 192^2 == 240^2. Off by a pixel either way is a black
    # ring or a clipped corner, so this is asserted rather than eyeballed.
    check_eq(144 ** 2 + 192 ** 2, 240 ** 2, "the aperture's corners land on the bezel")
    # The widest fixed row the UI draws is InstallerUI's 3 x 60px buttons with 2 x 10px gaps;
    # dialogButtonSpaced() centres them and pushes the outer two off the canvas below 200.
    b = lm.dialog_button_spaced(m, m["dialogButtonRowY"], 30, 0, 3, 60, 10)
    check_eq(b, (0, 178, 60, 30), "the widest dialog row starts flush at x=0, not negative")
    check_eq(b[0] >= 0, True, "dialog buttons do not overflow the canvas")
    # And 268 >= 240 keeps every h < 240 fallback on its normal path -- the 30px notification card,
    # the OTA title font, KryonCloudUI's vertical rhythm and the keyboard chrome.
    check(m["h"] >= 240, f"the canvas keeps the full-height layout paths (got h={m['h']})")
    check_eq(m["cardH"], 42, "notification card keeps its 42px height")


def test_other_resolutions_sane() -> None:
    print("alternative resolutions stay non-degenerate:")
    for (w, h) in [(240, 135), (320, 480), (480, 320), (800, 480)]:
        m = lm.compute(w, h)
        regions = ["frame", "header", "list", "footer"]
        ok = True
        for name in regions:
            x, y, rw, rh = m[name]
            if rw <= 0 or rh <= 0 or x < 0 or y < 0 or x + rw > w or y + rh > h:
                ok = False
                print(f"      {name} out of bounds at {w}x{h}: {m[name]}")
        check(ok, f"{w}x{h}: all regions positive and within the canvas")
        check(m["itemsPerPage"] >= 1, f"{w}x{h}: itemsPerPage >= 1 (got {m['itemsPerPage']})")
        check(m["kbKeyW"] >= 1 and m["kbKeyH"] >= 1, f"{w}x{h}: keyboard keys > 0")
        # The four footer zones must tile the full width with no gap or overlap.
        zones = [lm.footer_slot(m, s) for s in
                 (lm.SLOT_BACK, lm.SLOT_UP, lm.SLOT_SEL, lm.SLOT_DN)]
        tiled = zones[0][0] == 0 and zones[-1][0] + zones[-1][2] == w
        for a, b in zip(zones, zones[1:]):
            tiled = tiled and a[0] + a[2] == b[0]
        check(tiled, f"{w}x{h}: footer slots tile the width with no gap")
        check(all(z[2] > 0 for z in zones), f"{w}x{h}: every footer slot has a positive width")


def test_degenerate_canvas() -> None:
    print("degenerate canvas degrades gracefully (no crash, list clamped to 0):")
    m = lm.compute(128, 64)
    check_eq(m["list"][3], 0, "list height clamps to 0 rather than going negative")
    check(m["itemsPerPage"] >= 1, "itemsPerPage stays >= 1")
    check(m["kbKeyW"] >= 1 and m["kbKeyH"] >= 1, "keyboard keys stay > 0")


def test_board_discovery() -> None:
    print("board discovery from PlatformIO envs:")
    repo = Path(__file__).resolve().parents[2]
    boards = lm.discover_boards(repo)
    if not boards:
        check(False, "discovered at least one env")
        return
    check(True, f"discovered {len(boards)} envs: {', '.join(sorted(boards))}")
    # One default board per chip type; all three set the canvas explicitly to 240x320.
    for env in ("esp32-default", "esp32s3-default", "esp32s31-default"):
        check_eq(boards.get(env), (240, 320, 0), f"{env} -> 240x320 rotation 0")
    # The one product board so far, and the only non-SPI panel: an RGB panel whose CANVAS is
    # 201x268, blitted 96/67 into the 288x384 rect inscribed in the 480x480 round bezel (hence
    # 201x268 here, not the panel's own size). See test_waveshare_canvas_geometry for what that
    # pair is chosen for.
    check_eq(boards.get("waveshare-s3-lcd21b"), (201, 268, 0),
             "waveshare-s3-lcd21b -> 201x268 rotation 0")
    # A retune of that canvas must trip one of these rather than silently laying dialog buttons off
    # the canvas or switching on the h < 240 fallbacks.
    ws = boards.get("waveshare-s3-lcd21b")
    if ws:
        w, h, _ = ws
        check(3 * 60 + 2 * 10 <= w,
              f"canvas fits the widest dialog row (3*60+2*10 <= w, got w={w})")
        check(h >= 240, f"canvas keeps the full-height layout paths (h >= 240, got h={h})")
        check(w * 4 == h * 3, f"canvas is 3:4 so the aperture stays 288x384 (got {w}x{h})")
    # The CYD is a build target, not an example: board_configs/cyd.ini sits beside platformio.ini
    # rather than under examples/, and the OTA work promoted it there so its two app slots and the
    # KRYONOS_VERSION manifest could be built from the repo. Same 240x320 canvas as the chip defaults.
    check_eq(boards.get("esp32-cyd-28"), (240, 320, 0), "esp32-cyd-28 -> 240x320 rotation 0")
    # These really are examples -- their env blocks live under board_configs/examples/, which is not
    # globbed, so they must NOT be discovered.
    for env in ("m5stack-cardputer", "lilygo-t-hmi", "esp32-s3-devkitc-1-n16r8",
                "esp32doit-devkit-v1"):
        check(env not in boards, f"{env} is not an active env (kept as an example)")


def main() -> int:
    test_legacy_240x320()
    test_footer_buttons_240()
    test_footer_slots_240()
    test_list_rows_240()
    test_short_panel_240x135()
    test_keyboard_paging_240x100()
    test_waveshare_keyboard_201x268()
    test_waveshare_canvas_geometry()
    test_other_resolutions_sane()
    test_degenerate_canvas()
    test_board_discovery()
    print()
    if FAILURES:
        print(f"{len(FAILURES)} check(s) FAILED")
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
