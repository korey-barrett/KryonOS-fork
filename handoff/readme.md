# KryonOS-fork — session handoff

**Start here.** This folder is a cold-start brief so a new chat session can pick the work up without
re-deriving it.

| File | What it holds |
|---|---|
| `readme.md` | this file — orientation, current state, first move |
| `project.md` | the full technical state: hardware, environments, what is done, what is broken, standing rules |

> These files live in `handoff/`, not the repo root, deliberately: `README.md` already exists at the
> root and Windows' case-insensitive filesystem would have let a `readme.md` overwrite it. Both are
> **tracked** — the folder was committed in `3260bac`, and this capture is committed too. Commit
> further changes only when asked.

---

## Where things stand — 2026-10-10

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

**The upstream PR is held**, pending a re-test of the S31 on a clean install. `project.md` §8 records
that decision.

**Nothing else is blocked.** The open items in `project.md` §7 are all smaller than what has been
cleared, and none of them stops anything.

---

## First move in a new session

1. Read `handoff/project.md`. §3 (hardware), §4 (environments) and §6 (the 2.1B display work) are the
   parts that matter most.
2. **Nothing is unpushed.** `c7610f9` and `fa70f7b` went up on 2026-10-10 along with the S31 port, and
   `main` and `esp32s31` are both level with their remotes. The user still asks for pushes separately.
3. If the 2.1B flicker ever comes back, read `project.md` §6.4 *before* changing anything. The
   artifact's **shape** decides whether the porch is even the right knob: coherent lines travelling
   vertically is the missed-restart-window shift, while random speckle is the bounce refill starved by
   PSRAM contention, and those want opposite fixes.

---

## Constraints to carry into the new session

- **Never write the word "honestly."** State a caveat directly instead.
- **Commit to this fork only.** `origin` is `korey-barrett/KryonOS-fork`. Never alter, add, or PR
  against upstream `Haris16-code/KryonOS`.
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
