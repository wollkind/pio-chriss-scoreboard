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

- **Scoreboard:** X11 4×6 font (`Font4x6`, 5 px letters), `ROW_H` 6.
  - Player rows at y = i × 6 (rows 0–53), in the web page's order.
  - Dotted divider at `DIVIDER_Y` 55.
  - Total line top at `TOTAL_Y` 57.
- **Player row:**
  - x 0–1: position bar
  - label from x 3, shortened by `abbreviate()` (`src/abbrev.h`) until its ink ends `LABEL_GAP` px before the football space (live game) or the points
  - football `drawBall()` 5×3 at `pts_x - BALL_GAP - BALL_W`, row offset +1: brown with a white lace pixel, red in the red zone. Drawn only while the team has the ball, but its space is reserved for the whole live game (`teamPossession()`).
  - points right-aligned to the ink edge at x 63
- **Total line (`drawTotalLine()`):**
  - total of `PTS_OK` players right-aligned, gold
  - left: `orderedGames()` (live, final, pre), rotated every `SCORES_ROTATE_MS`. `drawGameScore()` picks the first form that fits before the total: `AWY 17 HOM 10` (4×6), `AWY17 HOM10` (4×6), then the same two in TomThumb. Upcoming games use `AWY - HOM 1:00P`, then `AWY - HOM`.
- **Points format:** `formatPoints()` switches to whole numbers at ≥ 99.95 or ≤ −9.95, so values stay within 4 characters.
- **Order:** the roster order from the web page (↑/↓). `SORT_BY_POINTS 1` sorts by points instead.
- **Fonts:** `tools/bdf2gfx.py` converts `tools/fonts/4x6.bdf` and `5x7.bdf` (public domain, from olikraus/u8g2 at d6c8499) into `src/fonts/*.h`.
- **Previews:** `tools/render_preview.py` draws `docs/panel-preview.png` and `docs/update-preview.png`.

## Events (celebration and update screens)

- **Detection (`fetchTask`):** when both the old and new state are `PTS_OK` and the points went up by any amount, or down by at least `EVENT_MIN_PTS` (1.0), an `Event` is queued (`queueEvent()`, ring buffer of `MAX_EVENTS` 6 under `g_lock`; extras are dropped).
- **Description (`describeChange()`):** from the differences of the `STAT_KEYS` fields kept per player. Priority: TDs, field goal (distance = `fgm_yds` delta / `fgm` delta), defense INT / fumble recovery / sack, `INTERCEPTED`, `FUMBLE LOST`, the largest yardage (`RUSH FOR`, `CATCH FOR`, `PASS FOR`), `EXTRA POINT`, else `POINTS UP` / `POINTS DOWN`.
  - Stat names seen in real week 2 replies: `pass_yd`, `pass_td`, `rush_yd`, `rec`, `rec_yd`, `rec_td`, `fgm`, `fgm_yds`, `xpm`, `sack`, `td` (DEF).
  - Not seen, unverified: `pass_int`, `rush_td`, `fum_lost`, `int`, `fum_rec`.
- **Play kind:** `describeChange()` also sets `Event.play` (`Play` in `celebrate.h`: RUSH, PASS, CATCH, DEFENSE, KICK, OTHER) and `Event.touchdown`. TD → its kind + touchdown; FG and XP → KICK; INT, fumble recovery, sack → DEFENSE; yardage → RUSH / CATCH / PASS; `INTERCEPTED`, `FUMBLE LOST`, `POINTS UP/DOWN` → OTHER.
- **Celebrations (`celebrate.cpp`):** rush (scrolling side-view field, runner hops a diving defender, `RUSH!`), pass/catch (spiral QB→receiver, sparks, `COMPLETE!` / `CAUGHT IT!`), defense (hit, shake, loose ball, D + fence), kick (behind the kicker, ball through the uprights, `GOOD!`), other (fireworks). `celebrationMs()`: `CELEBRATE_MS` 3000, `TOUCHDOWN_MS` 3500.
- **Touchdowns**, one scene per kind, on `drawFieldCam()` (side-view field with a camera, a theme-striped end zone, goal line and post):
  - `drawRushTD()`: sprint, end zone scrolls in, dive over the goal line, spike, `TOUCHDOWN!` marquee
  - `drawPassTD()`: the player is the QB; the bomb leaves the top of the screen, its path turns into a rainbow arc, `DIME!`
  - `drawCatchTD()`: toe-tap catch, leap into the stands (`drawStands()`), `SIX!`
  - `drawDefTD()`: jumped route, return to the end zone on the left, `TO THE` `HOUSE!`
  - `drawTouchdown()` (`TOUCH` `DOWN!` over fireworks) remains for a TD of any other kind (none is produced today).
- **Screens (`drawEventScreens()`, loop):**
  - Gains: `drawCelebration()` for `celebrationMs()`, then `drawUpdate()` for `UPDATE_MS` 3500.
  - Losses: `drawUpdate()` only.
  - Then the next queued event, or the scoreboard.
- **`drawUpdate()` layout:**
  - position-colour frame
  - name, uppercase, 5×7 at size 2. `drawCenteredFit()` falls back to size 1, then 4×6 with `abbreviate()`.
  - action, 5×7, split at the space nearest the middle when too wide
  - delta, size 2, green or red
  - `NOW 25.1` in 4×6
- **Test:** `POST /api/test?kind=N` (one web page button per celebration) queues a made-up event from `SAMPLES` in `handleTest()` for the first player.

## Games (ESPN)

- `fetchScoreboard()` reads the whole reply (~200 KB, no Content-Length, HTTP/1.0) into a 512 KB PSRAM buffer, then parses it through an ArduinoJson filter (`getJson(..., large = true)`).
  - v0.2–v0.5 parsed straight off the TLS stream; on the board this failed with −2 on every round (seen 2026-09-27 on v0.5: `status` −2, `last_ok_s` −1, no games). Buffered, it parses: 16 games, possession correct.
  - `/api/config` reports `espn_status`, `json_err` (last parse error text) and `games`.
- Games are stored in `g_games`. The panel uses them for possession (`teamPossession()`) and the rotating scores on the total line (`orderedGames()`). `scoreLine()` builds the game text for `/api/config` (`game` field), which the web page shows.
- Possession: `situation.possession` is an ESPN team ID (string), matched to the competitor's `team.id`. Verified against live games on 2026-09-27: the parse, run on the computer with the same filter, gave the right team with the ball and red zone for all 9 live games. `STATUS_HALFTIME` is still unseen.
- Nesting: the reply is 15 levels deep. ArduinoJson's default limit (10) applies to filtered-out parts too, so `getJson()` passes `NestingLimit(JSON_NESTING_LIMIT)` (32). Without it, v0.2 and v0.3 failed every scoreboard fetch with −2.
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
| `pio run -e esp32s3` (v0.5) | builds, no warnings in project files. RAM 13.3 %, flash 12.5 % |
| `pio run -e esp32s3-ota -t upload --upload-port 127.0.0.1` | builds, runs espota with `.pio/build/esp32s3-ota/firmware.bin` (no board, so no response) |
| Picker page in headless Chromium, against a mock `/api/config` and Sleeper replies saved with curl on 2026-09-27 | search, add 8, 9th refused, save POST body correct, reload uses the cached list (0 Sleeper requests) |
| Sleeper endpoints with curl, 2026-09-27 (week 3) | state, player lists, per-player stats, `null` for a player with no game yet, DEF stats by team ID. CORS header present |
| Panel layout | rendered offline from `glcdfont.c` (`docs/panel-preview.png`); fits 64 px |
| On hardware (v0.1) | works (owner, 2026-09-27) |
| On hardware (v0.5) | running (owner, 2026-09-27); player points work; NFL scores and possession were missing (ESPN parse −2) |
| On hardware (v0.6), OTA 2026-09-27 | ESPN fetch 200, 16 games, possession shown in `/api/config`. Celebrations **not yet watched** |
| Live-game update latency | **not measured** |

## History

- **v0.1:** first pass. Player picker page, per-player weekly points, sorted rows, change flash, OTA. Tested on hardware: works.
- **v0.2:**
  - 9 players
  - Name size setting (Large, Medium, Narrow), with labels fitted to the width
  - Game score line with a possession marker, from ESPN
  - Pages when the players don't fit on one screen
  - Startup animation
  - OTA: `ota` target on the main environment instead of a separate environment, no modem sleep, no espota `--debug`
- **v0.3:** labels too wide for the row are shortened from the middle (`abbrev.h`) instead of cut off at the end
- **v0.3.1:** fix: ESPN scoreboard parse failed with TooDeep; JSON nesting limit raised to 32
- **v0.4:** all 9 players on one screen (7 px rows). Score lines and pages removed from the panel; possession is a brown football before the points. Default name size Medium.
- **v0.4.1:** panel order is the order set on the web page (↑ and ↓ buttons) instead of by points.
- **v0.4.2:** OTA is a plain `[env:esp32s3-ota]` again (`upload_protocol = espota`). The custom `ota` target from v0.2 (`scripts/ota.py`) was removed: the owner could not get it to run.
- **v0.5:** 5 px font, 10 lines (9 players + total line with rotating NFL scores). Celebration and update screens on points changes. Test button. Name-size setting removed.
- **v0.6:** fix: ESPN reply buffered in PSRAM before parsing (stream parse failed on the board, so no scores or possession). Celebration per kind of play, separate touchdown screen (3–3.5 s). Every gain celebrates. One test button per celebration.
- **v0.6.1:** a touchdown scene per kind (rush, pass, catch, defense). "Run" renamed "Rush". Upcoming games `AWY - HOM` instead of `AWY@HOM`.
