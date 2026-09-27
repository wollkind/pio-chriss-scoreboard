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
  - then the ESPN scoreboard, then one stats GET per player
  - then waits `POLL_MS` (30 s), or until `xTaskNotifyGive` from a POST
- **Shared state** (`g_players`, `g_count`, `g_scoring`, season/week) is guarded by the `g_lock` mutex.
  - The task snapshots the roster, fetches without the lock, and writes back only if `g_generation` hasn't changed. A save during a round therefore discards that round's results.
- **Persistence:** the config is stored as JSON in NVS (`Preferences`, namespace `scoreboard`, key `config`).
- **mDNS:** `ArduinoOTA.begin()` starts mDNS with `HOSTNAME`. `MDNS.addService("http", ...)` is called after it, on the same responder.

## Layout

- **Blocks:** one per player. A name row of `RowFont::height`, then a `SCORE_H` 5 px score line in TomThumb.
  - x 0–1: position bar
  - label from x 3, cut character by character until its ink ends `LABEL_GAP` px before the points
  - points right-aligned to the ink edge at x 63
  - score line: ball marker at x 3–6 (column always reserved), text from x 8. The longest line, `17-10 Q3 4:12`, is 13 × 4 px and ends at x 59.
- **Name fonts (`ROW_FONTS`, index `g_font` from the web page):**
  - 0 Large: built-in 6×8, block 13 px, 4 per page
  - 1 Medium: X11 5×7 for label and points, block 12 px, 5 per page
  - 2 Narrow: u8g2 squeezed regular 7 for the label, 5×7 for the points, block 13 px, 4 per page
  - Label and points share one baseline: `RowFont::baseline`, 0 for the built-in font, which draws from the top.
- **Pages:** blocks that fit above the page-dot row (row 63), split evenly: `per_page = ceil(count / pages)`. The page is picked from `millis() / PAGE_MS`.
- **Points format:** `formatPoints()` switches to whole numbers at ≥ 99.95 or ≤ −9.95, so values stay within 4 characters.
- **Sorting:** `SORT_BY_POINTS` 1 sorts with `std::stable_sort`. Players without stats sort last.
- **Fonts:** `tools/bdf2gfx.py` converts the BDF files in `tools/fonts/` (public domain, from olikraus/u8g2 at d6c8499) into `src/fonts/*.h`. Rerun it to change fonts.

## Games (ESPN)

- `fetchScoreboard()` streams the reply through an ArduinoJson filter straight off the connection (`http.getStream()`, HTTP/1.0 so there is no chunk framing).
- Games are stored in `g_games`. `scoreLine()` builds the text for a team; the panel and `/api/config` (`game` field) both use it.
- Possession: `situation.possession` is an ESPN team ID, matched to the competitor's `team.id`. **Unverified against a live game.**
- `WSH` is converted to Sleeper's `WAS`.

## Startup animation (`startup.cpp`)

About 9 s, drawn in `loop()` while WiFi connects. The web server and OTA keep running during it.
- **Kick** (1.8 s): the ball follows a parabola and clears the crossbar between the uprights.
- **Pinwheel** (3 s): opens at the ball, drifts to the centre and accelerates. From 60 % it dissolves: pixels break off from the rim inward, and one in five flies outward under gravity.
- **Message** (4.3 s): the lines drop in one after another. A diagonal shimmer brightens the text, and confetti falls only on empty pixels.
- **Not seen on hardware yet.**

## HTTP lessons carried over from infopanel64

- `http.useHTTP10(true)`: a plain body and a closed connection, instead of chunked keep-alive. The chunked replies caused `-11` read timeouts there.
- Timeouts are set after `begin()`, plus `client.setTimeout()`.
- `setInsecure()`: the data is public and read-only. Pin a CA if that changes.

## Verification status

| Check | Result |
|---|---|
| `pio run -e esp32s3` | builds, no warnings in project files. RAM 13.1 %, flash 12.3 % |
| `pio run -e esp32s3 -t ota` against 127.0.0.1 | reuses the existing build, calls espota with the built `firmware.bin` (no board, so no response) |
| Picker page in headless Chromium, against a mock `/api/config` and Sleeper replies saved with curl on 2026-09-27 | search, add 8, 9th refused, save POST body correct, reload uses the cached list (0 Sleeper requests) |
| Sleeper endpoints with curl, 2026-09-27 (week 3) | state, player lists, per-player stats, `null` for a player with no game yet, DEF stats by team ID. CORS header present |
| Panel layout | rendered offline from `glcdfont.c` (`docs/panel-preview.png`); fits 64 px |
| On hardware (v0.1) | works (owner, 2026-09-27) |
| On hardware (v0.2) | **not yet flashed**: 9 players, fonts, score lines, pages, startup animation, OTA speed |
| Live-game update latency | **not measured** |

## History

- **v0.1:** first pass. Player picker page, per-player weekly points, sorted rows, change flash, OTA. Tested on hardware: works.
- **v0.2:**
  - 9 players
  - Name size setting (Large, Medium, Narrow), with labels cut to the width that fits
  - Game score line with a possession marker, from ESPN
  - Pages when the players don't fit on one screen
  - Startup animation
  - OTA: `ota` target on the main environment instead of a separate environment, no modem sleep, no espota `--debug`
