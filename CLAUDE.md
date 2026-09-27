# pio-chriss-scoreboard: live fantasy points on a 64×64 panel

User-facing description, data format and web API: `README.md`. This file holds the engineering notes.

## Hardware

- **Board:** Waveshare ESP32-S3-RGB-Matrix (ESP32-S3-N32R16) with a 64×64 HUB75 panel. Library entry: `wollkind/hw-docs`, `boards/waveshare-esp32-s3-rgb-matrix`.
- **Display setup** is copied from `wollkind/infopanel64`, verified on this hardware:
  - `mxconfig.gpio.e = 9`, `clkphase = false`, `driver = FM6126A`
  - the vendored driver in `src/`, with `build_src_filter` excluding `platforms/`
  - a `GFXcanvas16` frame, pushed to the panel by `present()`, which sends only changed pixels
- **Not used:** mics, IMU, RTC, SHTC3, speaker. No auto-rotation; `ROTATION` is a fixed define.

## Structure

- **Core 1, `loop()` every `FRAME_MS` (50 ms):**
  - `serviceNetwork()`: ArduinoOTA, `WebServer`
  - `drawScreen()`, `present()`
- **Core 0, `fetchTask`:**
  - `/v1/state/nfl` at start and every 30 min
  - then one stats GET per player
  - then waits `POLL_MS` (60 s), or until `xTaskNotifyGive` from a POST
- **Shared state** (`g_players`, `g_count`, `g_scoring`, season/week) is guarded by the `g_lock` mutex.
  - The task snapshots the roster, fetches without the lock, and writes back only if `g_generation` hasn't changed. A save during a round therefore discards that round's results.
- **Persistence:** the config is stored as JSON in NVS (`Preferences`, namespace `scoreboard`, key `config`).
- **mDNS:** `ArduinoOTA.begin()` starts mDNS with `HOSTNAME`. `MDNS.addService("http", ...)` is called after it, on the same responder.

## Layout

- **Rows:** 8 rows of `ROW_H` 8 px, using the default 6×8 GFX font.
  - x 0–1: position bar, 7 px tall
  - label from x 3: `LABEL_CHARS` 6 characters, ending at x 38
  - points right-aligned to x 63: up to 4 characters, starting at x 41
- **Points format:** `formatPoints()` switches to whole numbers at ≥ 99.95 or ≤ −9.95, so values stay within 4 characters.
- **Sorting:** `SORT_BY_POINTS` 1 sorts with `std::stable_sort`. Players without stats sort last.

## HTTP lessons carried over from infopanel64

- `http.useHTTP10(true)`: a plain body and a closed connection, instead of chunked keep-alive. The chunked replies caused `-11` read timeouts there.
- Timeouts are set after `begin()`, plus `client.setTimeout()`.
- `setInsecure()`: the data is public and read-only. Pin a CA if that changes.

## Verification status

| Check | Result |
|---|---|
| `pio run -e esp32s3` | builds, no warnings in project files. RAM 13.1 %, flash 12.1 % |
| Picker page in headless Chromium, against a mock `/api/config` and Sleeper replies saved with curl on 2026-09-27 | search, add 8, 9th refused, save POST body correct, reload uses the cached list (0 Sleeper requests) |
| Sleeper endpoints with curl, 2026-09-27 (week 3) | state, player lists, per-player stats, `null` for a player with no game yet, DEF stats by team ID. CORS header present |
| Panel layout | rendered offline from `glcdfont.c` (`docs/panel-preview.png`); fits 64 px |
| On hardware | **not yet flashed** |
| Live-game update latency | **not measured** |

## History

- **v0.1:** first pass. Player picker page, per-player weekly points, sorted rows, change flash, OTA.
