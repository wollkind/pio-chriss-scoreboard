# pio-chriss-scoreboard

A live fantasy football scoreboard for a 64×64 HUB75 LED panel on the **Waveshare ESP32-S3-RGB-Matrix** board. It shows up to 8 chosen players and their fantasy points for the current NFL week. The players are picked on a web page served by the board.

![Panel layout preview](docs/panel-preview.png)

*Layout preview rendered with the panel's font; not a photo.*

## What it shows

- **Rows:** one 8-pixel row per player, up to 8, sorted by points (highest first). Players with no stats yet go last.
- **Each row:**
  - a 2 px position bar: QB red, RB green, WR blue, TE orange, K purple, DEF grey
  - a label of up to 6 characters, editable on the web page
  - this week's points, right-aligned
- **Points:**
  - `-` in grey: no stats this week yet (game not started, or bye)
  - green for 8 s after a change
  - all grey when no fetch has succeeded for 5 min
- **Status screens:**
  - "SET WIFI": `src/secrets.h` is missing
  - "WIFI..": connecting
  - "PICK PLAYERS" with the board's IP: no players chosen yet
- **Red pixel, bottom-right:** the last fetch failed. The web page shows the error code.

## Setup

1. Copy `src/secrets.example.h` to `src/secrets.h` (git-ignored) and fill in the WiFi SSID and password. Optionally set `OTA_PASSWORD`.
2. Build and flash over USB:
   ```
   pio run -e esp32s3 -t upload
   ```
3. Open `http://scoreboard.local/` on a phone on the same WiFi, or use the IP shown on the panel.
4. Search for players, add up to 8, pick the scoring format (PPR, half PPR, standard), and press **Save to panel**.

The roster, scoring format and brightness are stored in NVS and survive reboots.

## How the picker works

- **Page source:** the board serves the page (`src/web_page.h`). Nothing needs to be installed on the phone.
- **Player list:** the page's JavaScript downloads Sleeper's player list directly from `api.sleeper.app`. Sleeper sends `Access-Control-Allow-Origin: *`, so the browser allows the request. The board never downloads the list.
- **Size:** six position-filtered requests (QB, RB, WR, TE, K, DEF; `active=true`), about 4 MB uncompressed in total. The page keeps only rostered players, about 900 of them.
- **Cache:** the trimmed list is stored in the browser's `localStorage` for 24 hours. Sleeper asks that the player list be fetched at most once a day.
- **What reaches the board:** only `{id, label, pos, team}` for each chosen player, via `POST /api/config`.
- **Internet:** the phone needs internet access, because the list comes from Sleeper and not from the board.

## Data source: Sleeper

Sleeper needs no API key and no account. Its documentation (docs.sleeper.com) says the API is free for non-commercial use, with a suggested ceiling of 1000 calls per minute.

| Call | Endpoint | Documented? | Size |
|---|---|---|---|
| Season/week | `https://api.sleeper.app/v1/state/nfl` | yes | ~200 B |
| Player list (browser only) | `https://api.sleeper.app/v1/players/nfl?position=QB&active=true` | yes | 5 KB (DEF) to 1.7 MB (WR) |
| One player's week | `https://api.sleeper.com/stats/nfl/player/<id>?season_type=regular&season=2026&week=3` | **no** | ~0.7–1.2 KB |

The stats endpoint is the one the Sleeper app itself uses. It is not in Sleeper's public documentation, so it can change without notice.

- **Reply:** the player's stat line for that week, or the literal `null` when there are no stats yet.
- **Points field:** the stat line includes Sleeper's computed fantasy points, so the firmware does no scoring maths. It reads one of these fields:
  - `pts_ppr`
  - `pts_half_ppr`
  - `pts_std`
- **Example (trimmed):**
  ```json
  {"week":3,"season":"2026","player_id":"11370","team":"GB","opponent":"ATL",
   "stats":{"pts_ppr":1.4,"pts_half_ppr":0.9,"pts_std":0.4,"rec":1.0,"rec_yd":4.0, ...},
   "player":{"first_name":"Chris","last_name":"Brooks","position":"RB", ...}}
  ```
- **Parsing:** ArduinoJson parses with a filter, so only `stats.<scoring field>` is kept.

**Player IDs.** Sleeper's `player_id` is a string. It is digits for players (`"4046"` is Patrick Mahomes) and the team abbreviation for team defenses (`"GB"`). The same ID works in every Sleeper endpoint. The player list also carries `espn_id`, `yahoo_id`, `gsis_id` (NFL) and `sportradar_id`, for cross-referencing other sources.

**Timeliness (unverified).** Sleeper's CDN caches the weekly stats reply for 4 s (`cache-control: s-maxage=4`), and the player-list reply for 600 s. How soon points move after a play has not yet been measured during a live game. The firmware polls once a minute (`POLL_MS`).

## Load on the ESP32-S3

Small. Each poll round is:
- one HTTPS GET per player, at most 8, each about 1 KB
- parsed with a filter, so the JSON document holds a single number

That runs in a background task on core 0, once a minute. Each request opens a new TLS connection (HTTP/1.0, see below), which takes on the order of a second, so a full round takes a few seconds. The display and web server stay responsive on core 1 during a round.

Build output: RAM 13.1 % (43 KB static), flash 12.1 % (508 KB of the 4 MB app slot).

## Web API

| Method | Path | Body / reply |
|---|---|---|
| GET | `/` | the picker page |
| GET | `/api/config` | `{scoring, brightness, players:[{id,label,pos,team,pts,state}], season, season_type, week, status, last_ok_s}` |
| POST | `/api/config` | `{scoring, brightness, players:[{id,label,pos,team}]}`, at most 8 players; replies like GET |

`state`: 0 not fetched yet, 1 no stats this week, 2 points valid. `status`: the last HTTP code, or −1 begin failed, −2 JSON parse error, −3 unexpected state reply.

## Updating over WiFi (OTA)

- **Setup:** set `OTA_PASSWORD` in `src/secrets.h`, and put the same value in the `SCOREBOARD_OTA_PASSWORD` environment variable.
- **Upload:**
  ```
  pio run -e esp32s3-ota -t upload
  ```
- **Details:** the mechanism and troubleshooting are the same as in infopanel64's `OTA.md`.

## Repository layout

| Path | What |
|---|---|
| `src/main.cpp` | the application: display, Sleeper fetch task, web server, OTA |
| `src/web_page.h` | the player picker page (HTML + JavaScript, served from flash) |
| `src/ESP32-HUB75-*`, `src/platforms/` | HUB75 panel driver vendored from Waveshare's Arduino examples (copied from infopanel64) |
| `src/secrets.example.h` | template for `src/secrets.h` |
| `partitions_32MB.csv` | flash layout (two 4 MB app slots) |
| `CLAUDE.md` | engineering notes and status |

## Credits and licences

- **HUB75 driver:** [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) by mrcodetastic, vendored from Waveshare's examples.
- **Board configuration:** Waveshare's [ESP32-S3-RGB-Matrix](https://www.waveshare.com/esp32-s3-rgb-matrix.htm) examples (Apache-2.0), via infopanel64.
- **Data:** [Sleeper](https://sleeper.com/), under its API terms (non-commercial use).
