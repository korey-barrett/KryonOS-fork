# Flash Layout, Reflashing, and What Survives

Everything you type into KryonOS — WiFi credentials, your KryonCloud pairing, the web file
manager's password, touch calibration — lives outside the app slot, in one of two places. Which
flash you perform decides whether those survive it. This page lists both, board by board, and says
exactly what has to be re-entered after each kind of flash.

The short version:

| Flash kind | NVS | LittleFS (`/local`) | What you re-enter |
| :--- | :--- | :--- | :--- |
| `pio run -e <env> -t upload`, or any OTA update | survives | survives | **nothing** |
| Factory image written at offset `0x0` | **erased** | survives | KryonCloud pairing, web password is regenerated |
| Partition table change (new `board_build.partitions`) | survives | **erased** | WiFi SSID + password, apps, touch calibration |
| `pio run -t erase` (full chip) | **erased** | **erased** | everything above |

---

## 1. Where each setting actually lives

**NVS** — the `nvs` partition at `0x9000`, 20 KB. Four namespaces:

| Namespace | Contents | Written by |
| :--- | :--- | :--- |
| `kryon_auth` | `token`, `userName`, `beamHandle`, `email`, **`deviceId`**, `deviceName`, `userId` | `KryonCloudManager.cpp:99-121` |
| `kryon_web` | `user` (default `admin`), `pass` — the file manager's login | `WebManager.cpp:27-45` |
| `kryon_sec` | `enc_key` — the 32-byte device encryption key, generated from the hardware TRNG on first use | `CryptoEngine.cpp:343-359` |
| `kryon_ota` | `auto_update` | `OTAManager.cpp:49-60` |

**LittleFS** — the `spiffs` partition, reached through the `/local` virtual path
(`FileSystem.cpp:55`). Contains:

| Path | Contents |
| :--- | :--- |
| `/local/system/wifi_credentials.enc` | saved WiFi networks, encrypted with `kryon_sec`'s key (`WiFiManager.cpp:13`) |
| `/local/system/stars_cache.json` | star count cache (`SettingsUI.cpp:557`) |
| `/local/apps/` | installed apps (`FileSystem.cpp:97`) |
| `/local/touch_cal_p.bin` | touch calibration (`FileSystem.cpp:379`) |
| `/local/nowifi.txt` | marker that WiFi is switched off (`WiFiManager.cpp:45`) |

On a board with an SD card mounted, the WiFi file may instead be at
`/sd/system/wifi_credentials.enc` (`WiFiManager.cpp:14`), in which case it survives a LittleFS
format. `loadKnownNetworks()` reads whichever exists.

> **The two are coupled.** The WiFi file is encrypted with the key stored in NVS `kryon_sec`. Wiping
> NVS alone makes a surviving `wifi_credentials.enc` undecryptable; wiping LittleFS alone loses the
> file the key belongs to. Either one means re-entering WiFi — a fact worth knowing before assuming a
> reflash "only" cost you the pairing.

---

## 2. Flash layout per board

The `nvs` and `otadata` partitions are at the same offsets in every table in this repo, so an app
flash never lands on them.

| Environment | Partition table | `nvs` | `otadata` | Apps | Filesystem | `coredump` |
| :--- | :--- | :--- | :--- | :--- | :--- | :--- |
| `esp32s3-default` | `default_16MB.csv` | `0x9000` / 20 KB | `0xE000` / 8 KB | `0x10000` + `0x650000`, 6.25 MB each | `spiffs` `0xC90000` / 3.375 MB | `0xFF0000` / 64 KB |
| `waveshare-s3-lcd21b` | `default_16MB.csv` | `0x9000` / 20 KB | `0xE000` / 8 KB | `0x10000` + `0x650000`, 6.25 MB each | `spiffs` `0xC90000` / 3.375 MB | `0xFF0000` / 64 KB |
| `esp32-default` | `huge_app.csv` | `0x9000` / 20 KB | `0xE000` / 8 KB | `0x10000`, 3 MB — **no second slot, no OTA** | `spiffs` `0x310000` / 896 KB | `0x3F0000` / 64 KB |
| `esp32-cyd-28` | `min_spiffs.csv` | `0x9000` / 20 KB | `0xE000` / 8 KB | `0x10000` + `0x1F0000`, 1.875 MB each | `spiffs` `0x3D0000` / 128 KB | `0x3F0000` / 64 KB |
| `esp32s31-default` | `default_16MB.csv` (from the board JSON) | `0x9000` / 20 KB | `0xE000` / 8 KB | `0x10000` + `0x650000` | `spiffs` `0xC90000` | `0xFF0000` / 64 KB |
| `esp32s31-korvo1` | `default_16MB.csv` (from the board JSON) | `0x9000` / 20 KB | `0xE000` / 8 KB | `0x10000` + `0x650000`, 6.25 MB each | `spiffs` `0xC90000` | `0xFF0000` / 64 KB |

The S31 row's app and filesystem sizes come from the board's own `default_16MB.csv` in the
`61.04.00-RC1` platform. Only the offsets are fixed by the bootloader; if you change tables, read the
actual numbers out of the CSV you selected rather than trusting a table on a page — including this
one.

`esp32-default` is the only environment with no second app slot, so `Update.begin()` there fails with
`UPDATE_ERROR_NO_PARTITION` and no in-place update is possible at all (`platformio.ini:142-150`).

---

## 3. What each kind of flash writes

### 3.1 Normal upload — nothing is lost

`pio run -e <env> -t upload` (and the VS Code upload button) writes four regions:

```
0x001000  bootloader.bin
0x008000  partitions.bin
0x00E000  boot_app0.bin
0x010000  firmware.bin
```

esptool erases only the sectors it writes, so the erase windows are `0x1000`-`0x6FFF`,
`0x8000`-`0x8FFF`, `0xE000`-`0xFFFF` and the app slot. **`nvs` at `0x9000`-`0xDFFF` is inside none of
them**, and neither is the filesystem partition. Nothing has to be re-entered.

An OTA update is the same case: `Update.write()` streams into the app slot only
(`OTAManager.cpp:342`), so a device that updates itself keeps its pairing and its WiFi.

### 3.2 A factory image written at offset `0x0` — NVS is erased

A `firmware.factory.bin` is a concatenated image: bootloader, partition table, `boot_app0` and the
app, meant to be written in one go at offset `0x0`. Written there, it spans `0x0` through the end of
the app slot — for `huge_app` that is `0x0`-`0x30FFFF`, for `min_spiffs` `0x0`-`0x1CEFFF` — and the
`nvs` partition at `0x9000` sits squarely inside that range.

Results, in order of how much they annoy you:

- **KryonCloud pairing is gone.** `deviceId` and `token` were in `kryon_auth`. The device now
  registers with KryonCloud as a **new board**, which means pairing again *and* deleting the old
  board entry from your account — the old one is a ghost that will never come back.
- **The web file-manager password is regenerated**, not restored. With `kryon_web` cleared,
  `loadWebCredentials()` sees an empty `pass` and writes a fresh random one
  (`WebManager.cpp:32-38`); the username falls back to `admin`. Read the new password off the device.
- **The device encryption key is replaced.** `getDeviceKey()` finds no `enc_key` and generates a new
  one from the TRNG (`CryptoEngine.cpp:348-353`). Any `wifi_credentials.enc` still on LittleFS was
  encrypted to the old key and can no longer be decrypted, so **WiFi must be re-entered too**, even
  though the file itself was never touched.
- **The auto-update flag resets** to off (`kryon_ota`).

LittleFS itself survives this flash, because the filesystem partition starts above the app slot and
lies outside the erase window. Your apps, star cache and touch calibration are still there — the
WiFi loss here is the key mismatch above, not a formatted filesystem.

### 3.3 Changing the partition table — LittleFS is erased

`nvs` and `otadata` stay at `0x9000` and `0xE000` in every table here, so **NVS survives a table
change**: your KryonCloud pairing and web credentials are unaffected.

The filesystem partition, however, moves — `spiffs` goes from `0x310000` under `huge_app` to
`0x3D0000` under `min_spiffs` on the CYD, for example. The old data is orphaned at its old offset and
the new partition is blank, so `LittleFS.begin(true)` formats it on the next boot
(`FileSystem.cpp:77`). That takes the WiFi credentials, `/local/apps`, the star cache and the touch
calibration with it.

A partition table cannot be delivered by OTA, so this is always a one-time wire flash. It has to be
done once per table change, not once per update.

### 3.4 `pio run -t erase` — everything

A full-chip erase writes over `nvs`, `otadata`, both app slots and the filesystem. It is the
superset of 3.2 and 3.3: re-enter WiFi, re-pair KryonCloud, look up the new web password, and
re-calibrate touch.

---

## 4. What to re-enter, by scenario

| Scenario | WiFi | KryonCloud | Web password | Touch calibration | Apps |
| :--- | :--- | :--- | :--- | :--- | :--- |
| `-t upload` / OTA | — | — | — | — | — |
| Factory image at `0x0` | **yes** (key changed) | **yes** (re-pair, delete old board) | new one generated | — | — |
| Partition table change | **yes** (file formatted) | — | — | **yes** | **yes** |
| `-t erase` | **yes** | **yes** | new one generated | **yes** | **yes** |

---

## 5. Keeping your settings through a full flash

If a full flash is genuinely unavoidable — a table change, or a device that will not boot — snapshot
the two regions first and put them back afterwards. Both are plain `read-flash` / `write-flash`
operations, and neither needs a rebuild:

```
# Snapshot (nvs first, then the filesystem at whatever offset your table gives it)
esptool.py --port COM6 read-flash 0x009000 0x005000 nvs_backup.bin
esptool.py --port COM6 read-flash 0x3D0000 0x020000 littlefs_backup.bin   # min_spiffs, CYD

# Flash whatever you need to flash, then restore
esptool.py --port COM6 write-flash 0x009000 nvs_backup.bin
esptool.py --port COM6 write-flash 0x3D0000 littlefs_backup.bin
```

Two habits that avoid the problem entirely:

- **Flash through `pio run -e <env> -t upload`, never a factory image at `0x0`.** On the CYD this is
  the difference between keeping your pairing and registering a new board.
- **Treat `board_build.partitions` as a one-way door.** Changing it costs the filesystem and is the
  only thing besides a full erase that does. Decide the table before you hand a device to anyone.

---

*See also: [Hardware Architecture](Hardware_Architecture.md) §1 for the board table, and
[Display & Touch Architecture](Display_Touch_Architecture.md) for how a board is selected at build
time.*
