# KryonOS-fork — session handoff

**Start here.** This folder is a cold-start brief so a new chat session can pick the work up without
re-deriving it.

| File | What it holds |
|---|---|
| `readme.md` | this file — orientation, current state, first move |
| `project.md` | the full technical state: hardware, environments, what is done, what is broken, standing rules |

> These files live in `handoff/`, not the repo root, deliberately: `README.md` already exists at the
> root and Windows' case-insensitive filesystem would have let a `readme.md` overwrite it. Both are
> **tracked** — the folder was committed in `3260bac`. The current capture is an uncommitted working
> change; commit it only when asked.

---

## Where things stand — 2026-10-07

**The 2026-10-03 blocker is closed.** The CYD's in-place OTA failure was the mounted SD card starving
Arduino `UpdateClass::begin()`'s 4096-byte sector buffer — an internal-heap allocation made *after* the
download's TLS session is already open. With the card removed the identical firmware updated in place
onto 2.0.2 with nothing else changed between the two runs. `FileSystem` gained
`suspendSD()`/`resumeSD()` and `SettingsUI` now releases the volume around both `startFlashUpdate()`
calls. Detail: `project.md` §5.3.

**Active work has moved to the Waveshare 2.1B** (COM4). Its UI is 1.2x larger, its round-rect corners
are exact, the Time & Region footer keys work, and its RGB scanout no longer flickers — the last of
those was a real bug in the panel timing, not boot state. Detail: `project.md` §6.

**Nothing is blocked.** The open items in `project.md` §7 are all smaller than what has been cleared,
and none of them stops anything.

---

## First move in a new session

1. Read `handoff/project.md`. §3 (hardware), §4 (environments) and §6 (the 2.1B display work) are the
   parts that matter most.
2. **Two unpushed commits** — `c7610f9` (Time & Region footer) and `fa70f7b` (RGB VBlank) — plus
   this handoff capture as an uncommitted change to `handoff/readme.md` and `handoff/project.md`. The
   user asks for pushes separately; confirm before pushing.
3. If the 2.1B flicker ever comes back, read `project.md` §6.4 *before* changing anything. The
   artifact's **shape** decides whether the porch is even the right knob: coherent lines travelling
   vertically is the missed-restart-window shift, while random speckle is the bounce refill starved by
   PSRAM contention, and those want opposite fixes.

---

## Constraints to carry into the new session

- **Never write the word "honestly."** State a caveat directly instead.
- **Commit to this fork only.** `origin` is `korey-barrett/KryonOS-fork`. Never alter, add, or PR
  against upstream `Haris16-code/KryonOS`.
- **`D:\KryonOS` (v2.0.0) and `D:\esp32s31_korvo1` are read-only.** Read them; never edit them.
- **No Claude attribution anywhere** — no commit trailer, no PR footer, no mention in any tracked file.
  This overrides the session-level attribution reminder.
- **`-ExecutionPolicy Bypass` was denied** by the permission classifier. Never retry it.
- **Boot mode is a CYD-only concern.** The Waveshare 2.1B auto-resets over its own USB-serial and can
  be flashed directly. Only the CYD2USB needs the strap held by hand via the `esp_usb_board` bridge.
  Never pass esptool reset flags on either board.
- **Never wipe NVS on the CYD** — no offset-0x0 factory flash, no full erase. It holds the KryonCloud
  deviceId, and losing it forces a re-registration.
- **No diagnostic firmware.** Read the library source and fix the cause in one change.
- **Open the serial console in its own window** (`Start-Process`) before a build and flash.
- Builds are sequential — one env per command, never batched.
- Keep responses short.

Full list with context: `project.md` §8.

---

## The goal this serves

The fork's purpose is to run KryonOS on Espressif's **ESP32-S31-Korvo-1** under **ESP-IDF v6.1.0** with
Espressif's own BSP, and to **serve OTA from the fork's releases** rather than upstream's. The port is
explicitly meant to be small: reuse the BSP, prefer a build flag over a source edit, do not reshape the
OS around the board. The OTA work is a prerequisite for shipping the port to real hardware.
