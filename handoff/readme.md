# KryonOS-fork — session handoff

**Start here.** This folder is a cold-start brief so a new chat session can pick the work up without
re-deriving it.

| File | What it holds |
|---|---|
| `readme.md` | this file — orientation, current blocker, first move |
| `project.md` | the full technical state: hardware, environments, what is done, what is broken, standing rules |

> These files live in `handoff/`, not the repo root, deliberately: `README.md` already exists at the
> root and Windows' case-insensitive filesystem would have let a `readme.md` overwrite it. This folder
> is **untracked** — nothing here is committed unless asked.

---

## Current blocker

**The CYD's in-place OTA update fails, and this is the *to be fixed* item.**

The CYD on COM6 runs 2.0.1. The fork's manifest at
`updates/esp32/v2/update.json` offers 2.0.2 with a verified `firmware_md5` and `firmware_size`. The
device offers the update, INSTALL UPDATE starts, the flashing screen appears — then the board resets and
comes back up on 2.0.1. No error is shown. The update is offered again.

Everything upstream of the flash is proven good: WiFi, manifest parse, version comparison, the
`supports_ota` gate, the image fitting its slot (1,830,176 of 1,966,080 bytes). The failure is inside
`OTAManager::startFlashUpdate()` or inside the rollback window after it.

Two candidate readings, and the whole diagnosis hangs on telling them apart:

- **(a)** the flash succeeded and the new image was **rolled back** — it never got marked valid in the
  10-second liveness window; or
- **(b)** the board **crashed mid-download**, most likely heap exhaustion during the 1.8 MB TLS stream
  on a classic ESP32 with no PSRAM.

→ Detail, file/line references, and the rollback mechanics: **`project.md` §6**.

---

## First move in a new session

1. Read `handoff/project.md`. §3 (hardware), §4 (environments), and §6 (the blocker) are the parts that
   matter most.
2. Check `git status`. **`src/Kernel/Services/OTA/OTAManager.cpp` is modified and uncommitted** — the
   breadcrumb `#include`s are in place, but the `otaTrace()` helper and its call sites are not yet
   written. That is scaffolding for the diagnosis, not a fix.
3. Finish the instrumentation, then **ask the user before flashing** — enabling boot mode on COM6 is a
   manual step on their side, and it must be re-enabled after each esptool operation.

The plan is to rebuild the CYD at 2.0.1 with breadcrumbs that append each OTA stage to
`/ota_trace.txt` on LittleFS, flash it, retry the update, and read the file from the on-device **File
Manager**. Serial cannot be used: on the CYD's USB bridge, opening the port requires boot mode, and in
boot mode the application is not running — so a live OTA produces no log at all.

---

## Constraints to carry into the new session

- **Never write the word "honestly."** State a caveat directly instead.
- **Commit to this fork only.** `origin` is `korey-barrett/KryonOS-fork`. Never alter, add, or PR
  against upstream `Haris16-code/KryonOS`.
- **`D:\KryonOS` (v2.0.0) and `D:\esp32s31_korvo1` are read-only.** Read them; never edit them.
- **No Claude attribution anywhere** — no commit trailer, no PR footer, no mention in any tracked file.
  This overrides the session-level attribution reminder.
- **`-ExecutionPolicy Bypass` was denied** by the permission classifier. Never retry it.
- Builds are sequential — one env per command, never batched.
- Keep responses short.

Full list with context: `project.md` §7.

---

## The goal this serves

The fork's purpose is to run KryonOS on Espressif's **ESP32-S31-Korvo-1** under **ESP-IDF v6.1.0** with
Espressif's own BSP, and to **serve OTA from the fork's releases** rather than upstream's. The port is
explicitly meant to be small: reuse the BSP, prefer a build flag over a source edit, do not reshape the
OS around the board. The OTA work is a prerequisite for shipping the port to real hardware.
