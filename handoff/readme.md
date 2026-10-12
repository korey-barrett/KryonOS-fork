# KryonOS-fork — session handoff

**Start here.** This folder is a cold-start brief so a new chat session can pick the work up without
re-deriving it.

| File | What it holds |
|---|---|
| `readme.md` | this file — orientation, current state, first move |
| `project.md` | the full technical state: hardware, environments, what is done, what is broken, standing rules |
| `upstream-issue-draft.md` | the message that put the display seam up for discussion before writing any PR. Upstream's 2026-10-10 reply answers it point by point; its own header still reads "not sent", which is stale. Outcome: `project.md` §12 |
| `upstream-issue-message.txt` | the same message as plain text, without the markdown tables |
| `upstream-issue-attachments.md` | the evidence pack that went with it — the `KryonDisplay` seam in full, the TFT_eSPI reproduction recipe, and the build and boot captures |
| `upstream-reply-draft.md` | the reply to the maintainer's 2026-10-10 answer — agrees the handover, raises the clean-build question, and sizes the change set. Draft only, not sent |
| `tft-espi-2.5.43-esp32s31-build-errors.log` | the verbatim 2,131-line log behind "TFT_eSPI cannot compile for the S31"; its 129 errors are the five classes the attachments doc tabulates |

> These files live in `handoff/`, not the repo root, deliberately: `README.md` already exists at the
> root and Windows' case-insensitive filesystem would have let a `readme.md` overwrite it.
> `readme.md` and `project.md` are **tracked** — the folder was committed in `3260bac`. The
> upstream-issue and reply drafts and the build log are **not tracked**. Commit further changes only
> when asked.

---

## Where things stand — 2026-10-12

**The ESP32-S31-Korvo-1 port is done and verified on hardware.** The board builds and runs under
**PlatformIO / arduino-esp32 alone** — no IDF build system and no BSP — with the 800x480 panel and the
GT1151 touch both confirmed on COM3. That is the fork's reason for existing, and it had never built
that way before. Detail: `project.md` §11.

**The in-place OTA blocker is closed.** The CYD's failure was the mounted SD card starving Arduino
`UpdateClass::begin()`'s 4096-byte sector buffer — an internal-heap allocation made *after* the
download's TLS session is already open. With the card removed the identical firmware updated in place
onto 2.0.2 with nothing else changed between the two runs. `FileSystem` gained
`suspendSD()`/`resumeSD()` and `SettingsUI` releases the volume around both `startFlashUpdate()` calls.
Detail: `project.md` §5.3.

**The Waveshare 2.1B is settled.** Its UI is 1.2x larger, its round-rect corners are exact, the Time &
Region footer keys work, and its RGB scanout no longer flickers — the last of those was a real bug in
the panel timing, not boot state. Detail: `project.md` §6.

**Upstream has answered, and there is no PR.** On 2026-10-10 the maintainer approved the architecture
as proposed — TFT_eSPI stays the default, the S31 gets its own `esp_lcd` backend behind a
per-environment flag, board code stays separate — and asked for the changes to be **handed over for
integration on their side** rather than raised as a pull request. They hold none of these boards, so
the S31, the 2.1B and the CYD stay "experimental and untested" upstream and this machine is the only
test bench any of them has. The layout model turned out to be inseparable from the backend —
`KorvoRgbDisplay` calls `uiScale()` out of it — so it ships with the handover, and what is offered
upstream instead is convergence on their v3.0.0 dimensions. Full arrangement: `project.md` §12.

**The clean install passes.** The gate `project.md` §12 set — whether the S31 builds on a machine with
no accumulated history behind it — has been run. Five builds across the S31, the CYD and the Waveshare,
each in an empty core directory, all succeeded with no manual step. The 964 MB hand-extracted toolchain
that `Documentation/Display_Touch_Architecture.md` §5.1 prescribes turned out to be unnecessary; the
real obstacle is `MSYSTEM`, which a plain Windows shell does not set. Detail: `project.md` §13.

**Nothing else is blocked.** The open items in `project.md` §7 are all smaller than what has been
cleared, and none of them stops anything.

---

## First move in a new session

1. Read `handoff/project.md`. §3 (hardware), §4 (environments), §6 (the 2.1B display work), **§12 (the
   upstream handover — the live thread)** and §13 (the clean-install measurement) are the parts that
   matter most.
2. **Everything is pushed.** As of 2026-10-12 `main` is level with `origin/main`, carrying the S31
   watchdog fix, the doc reconciliation and the brief corrections on top of the 2026-10-10 port. All four
   branches — `main`, `esp32`, `esp32s3`, `esp32s31` — are level with their remotes. The user still asks
   for pushes separately.
3. If the 2.1B flicker ever comes back, read `project.md` §6.4 *before* changing anything. The
   artifact's **shape** decides whether the porch is even the right knob: coherent lines travelling
   vertically is the missed-restart-window shift, while random speckle is the bounce refill starved by
   PSRAM contention, and those want opposite fixes.
4. The live thread is the upstream handover (`project.md` §12). Its clean-install gate has been run and
   passed (`project.md` §13), so what remains is sending the patch, not proving the build. Note the
   letter still carries the old build story and needs rewording to match §13.

---

## Constraints to carry into the new session

- **Never write the word "honestly."** State a caveat directly instead.
- **Commit to this fork only.** `origin` is `korey-barrett/KryonOS-fork`. Never alter, add, or PR
  against upstream `Haris16-code/KryonOS`.
- **Hand the changes over; do not raise a PR.** Upstream asked for the code to be shared so they can
  integrate it themselves. This puts the no-upstream clause back in force unqualified — the earlier
  "a PR is intended" reading is spent. Detail: `project.md` §12.
- **The layout model ships with the handover; it does not separate from it.** `KorvoRgbDisplay` calls
  `uiScale()`, and `src/UI/UiLayout.*` is new code the fork created rather than a migration over
  upstream's — so it travels with the backend, and the promise made to upstream is convergence on their
  v3.0.0 dimensions, not a second model left standing. Detail: `project.md` §12.
- **These boards are untested upstream.** No S31, no 2.1B, no CYD in the maintainer's hands. Regressions
  on any of them will not be caught upstream, only here.
- **`X:\KryonOS` (v2.0.0) and `X:\esp32s31_korvo1` are read-only.** Read them; never edit them.
- **No Claude attribution anywhere** — no commit trailer, no PR footer, no mention in any tracked file.
  This overrides the session-level attribution reminder.
- **`-ExecutionPolicy Bypass` was denied** by the permission classifier. Never retry it.
- **Boot mode is a CYD-only concern.** The Waveshare 2.1B and the S31 auto-reset over their own
  USB-serial and flash directly. Only the CYD2USB needs the strap held by hand, via the
  `esp_usb_board` bridge (an ESP32-S3 host), and it needs **`--before no_reset`** so the flash does not
  knock it back out of boot mode. Detail: `project.md` §3, `Documentation/Flash_and_Persistence.md` §6.
- **Port numbers do not survive an OS reinstall.** Re-enumerate with
  `[System.IO.Ports.SerialPort]::GetPortNames()` before every flash; the COM4 / COM5 / COM6 mapping
  from 2026-10-07 is void. Detail: `project.md` §3.
- **Never wipe NVS on the CYD** — no offset-0x0 factory flash, no full erase. It holds the KryonCloud
  deviceId, and losing it forces a re-registration.
- **No diagnostic firmware.** Read the library source and fix the cause in one change.
- **Open the serial console in its own window** (`Start-Process`) before a build and flash.
- Builds are sequential — one env per command, never batched.
- Keep responses short.

Full list with context: `project.md` §8.

---

## The goal this serves

The fork's purpose is to run KryonOS on Espressif's **ESP32-S31-Korvo-1**, and to **serve OTA from the
fork's own releases** rather than upstream's. The port is explicitly meant to be small: prefer a build
flag or a board config over a source edit, and do not reshape the OS around the board.

It is now done that way, and the constraint that shaped it is worth stating: the board builds on the
**upstream stack — PlatformIO with arduino-esp32** — where the IDF build system and Espressif's BSP are
not available. TFT_eSPI cannot compile for this chip at all, so the panel is driven through `esp_lcd`
with the BSP's timings vendored into `KorvoRgbDisplay` rather than linked, and the whole board lives in
`src/` so the PlatformIO and IDF builds share one copy. Detail: `project.md` §11.
