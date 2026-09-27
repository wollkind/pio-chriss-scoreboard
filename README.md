# pio-chriss-scoreboard

A live fantasy football scoreboard for a 64×64 HUB75 LED panel on the **Waveshare ESP32-S3-RGB-Matrix** board. It shows up to 9 chosen players, their fantasy points for the current NFL week, and the live score of each player's game. The players are picked on a web page served by the board.

![Panel layout preview](docs/panel-preview.png)

*One page at the Medium name size, drawn offline with the panel's fonts from made-up data (`tools/render_preview.py`); not a photo.*

Plain-language instructions for guests (finding the board, picking players): [`GUIDE.md`](GUIDE.md).

## Status (2026-09-27)

| Version | What | On hardware |
|---|---|---|
| v0.1 | picker page, weekly points, 8 rows | **works** (tested by the owner) |
| v0.2 | 9 players, pages, game score lines with possession, name sizes, startup animation, faster OTA and `ota` target | not yet flashed |
| v0.3 (current `main`) | long labels shortened from the middle | not yet flashed |

Open points:
- **Possession marker:** ESPN's `situation` fields were not in the reply when this was written (no game live). Their names are from the endpoint's commonly published format, not from a live reply.
- **OTA flashing speed:** flashing firmware over WiFi was very slow on v0.1. The fix (WiFi modem sleep off) is part of the firmware on the board, so it only helps once v0.2 or later is running: the OTA flash that installs v0.2 over v0.1 is still slow (or flash that one over USB). The speed of later OTA flashes has not been measured.
- **Data delay:** how soon points and scores on the panel change after a play has not been measured.

## What it shows

- **Startup:** a football is kicked through the goalposts, a rainbow pinwheel spins up and dissolves into confetti, and the panel says "GOOD AFTERNOON CHAMPIONS !!!" (about 9 s, while WiFi connects). `STARTUP_ANIMATION 0` skips it.
- **Players:** up to 9, sorted by points (highest first). Players with no stats yet go last.
- **Each player** is a block:
  - a position bar on the left: QB red, RB green, WR blue, TE orange, K purple, DEF grey
  - the label (editable on the web page). A label too wide for the row is shortened from the middle, so it stays readable: doubled letters, then vowels, then other letters go, keeping the first and last letters (`Washington` → `Washngtn` → `Wshngtn`, `Hockenson` → `Hocknsn`). See `src/abbrev.h`.
  - this week's points, right-aligned
  - underneath, in a tiny font, the player's game:

    | Game state | Shown |
    |---|---|
    | not started | `@MIA 1:00P` (`v` for a home game, `@` for away) |
    | live | `17-10 @MIA`, alternating every 3 s with `17-10 Q3 4:12` (or `HALF`, `OT`) |
    | live, player's team has the ball | a small yellow football before the score; red inside the opponent's 20 |
    | final | `35-14 @GB F` |
    | no game this week | `BYE` |

    The score is the player's team first, green when leading, red when trailing.
- **Name size**, chosen on the web page:

  | Setting | Font | Letters that fit (typical) | Players per page |
  |---|---|---|---|
  | Large | built-in 6×8 | about 6 | 4 |
  | Medium | X11 5×7 | about 7–8 | 5 |
  | Narrow | u8g2 "squeezed" 7 px, proportional | about 8–9 | 4 |

- **Pages:** when the players don't fit on one screen, they are split evenly over pages that switch every 7 s (9 players on Medium: 5 + 4). Dots on the bottom row show the page.
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
   - The board needs a WiFi network with internet access: it fetches points and scores itself.
   - There is no setup screen and no access-point mode. Changing networks means editing `secrets.h` and reflashing.
   - A phone hotspot works. The board then uses the phone's cellular data, roughly 30 MB per hour, almost all of it the ESPN scoreboard. Anyone using the picker page has to join the same hotspot.
2. Build and flash over USB:
   ```
   pio run -e esp32s3 -t upload
   ```
3. Open `http://scoreboard.local/` on a phone on the same WiFi, or use the IP shown on the panel. The panel shows its IP only while no players are chosen.
4. Search for players, add up to 9, pick the name size and the scoring format (PPR, half PPR, standard), and press **Save to panel**.

The roster, scoring format, name size and brightness are stored in NVS and survive reboots.

### Entering a lineup in one step

From a computer on the same network, one request replaces the whole roster. Scoring, brightness and name size stay as they are. IDs are Sleeper `player_id`s; team defenses use the team code.

```sh
curl -X POST http://scoreboard.local/api/config -H "Content-Type: application/json" \
  -d '{"players":[{"id":"12545","label":"Shough","pos":"QB","team":"NO"},{"id":"TEN","label":"Titans","pos":"DEF","team":"TEN"}]}'
```

PowerShell: `Invoke-RestMethod -Uri http://scoreboard.local/api/config -Method Post -ContentType application/json -Body '<same JSON>'`.

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

**Game scores: ESPN.** `https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard` returns every game of the current week in one reply: teams, scores, status, quarter and clock. During a live game it also has `situation.possession` (the ID of the team with the ball) and `situation.isRedZone`. The endpoint is public, needs no key, and is not officially documented by ESPN.
- **Team codes:** ESPN's match Sleeper's except Washington (`WSH` at ESPN, `WAS` at Sleeper). The firmware converts it.
- **Unverified:** the `situation` fields were not present in the reply checked on 2026-09-27, because no game was live at the time. Their names follow the commonly published format of this endpoint.

**Player IDs.** Sleeper's `player_id` is a string. It is digits for players (`"4046"` is Patrick Mahomes) and the team abbreviation for team defenses (`"GB"`). The same ID works in every Sleeper endpoint. The player list also carries `espn_id`, `yahoo_id`, `gsis_id` (NFL) and `sportradar_id`, for cross-referencing other sources.

**Timeliness (unverified).** Sleeper's CDN caches the weekly stats reply for 4 s (`cache-control: s-maxage=4`), and the player-list reply for 600 s. How soon points move after a play has not yet been measured during a live game. The firmware polls every 30 s (`POLL_MS`).

## Load on the ESP32-S3

Small. Each poll round is:
- one HTTPS GET per player, at most 9, each about 1 KB, parsed with a filter so the JSON document holds a single number
- one HTTPS GET of ESPN's scoreboard, about 270 KB, parsed as a stream straight off the connection through a filter, so it is never held in memory whole

That runs in a background task on core 0, every 30 s. Each request opens a new TLS connection (HTTP/1.0, see below), which takes on the order of a second, so a full round takes a few seconds. The display and web server stay responsive on core 1 during a round.

Build output: RAM 13.1 % (43 KB static), flash 12.3 % (518 KB of the 4 MB app slot).

## Web API

| Method | Path | Body / reply |
|---|---|---|
| GET | `/` | the picker page |
| GET | `/api/config` | `{scoring, brightness, font, players:[{id,label,pos,team,pts,state,game}], season, season_type, week, status, last_ok_s}` |
| POST | `/api/config` | `{scoring, brightness, font, players:[{id,label,pos,team}]}`, at most 9 players; replies like GET |

`state`: 0 not fetched yet, 1 no stats this week, 2 points valid. `status`: the last HTTP code, or −1 begin failed, −2 JSON parse error, −3 unexpected state reply.

## Updating over WiFi (OTA)

- **Setup:** set `OTA_PASSWORD` in `src/secrets.h`, and put the same value in the `SCOREBOARD_OTA_PASSWORD` environment variable.
- **Upload:**
  ```
  pio run -e esp32s3 -t ota
  ```
  This builds only if something changed, then sends the same `firmware.bin` a USB upload would use. The `ota` target comes from `scripts/ota.py`. Earlier versions had a separate `esp32s3-ota` environment, and PlatformIO builds every environment in its own folder, so the first OTA upload recompiled everything.
- **Address:** `scoreboard.local` by default. If that doesn't resolve, set `SCOREBOARD_HOST` to the panel's IP.
- **Speed:** the firmware turns off WiFi modem sleep (`WiFi.setSleep(false)`). espota sends 1 KB at a time and waits for the board to answer each block. With modem sleep on, each answer can wait for the access point's next beacon (typically about 100 ms), so a 520 KB image took minutes. The upload also runs without espota's `--debug`, which PlatformIO's built-in OTA upload adds and which printed a "Chunk response" line for every block. The improved speed has not been measured on hardware yet.

## Repository layout

| Path | What |
|---|---|
| `src/main.cpp` | the application: display, Sleeper and ESPN fetch task, web server, OTA |
| `src/web_page.h` | the player picker page (HTML + JavaScript, served from flash) |
| `src/abbrev.h` | shortens labels from the middle to fit the row |
| `src/startup.cpp`, `src/startup.h` | startup animation |
| `src/fonts/` | Medium and Narrow fonts as Adafruit GFX headers, generated by `tools/bdf2gfx.py` from `tools/fonts/*.bdf` |
| `scripts/ota.py` | PlatformIO `ota` target |
| `tools/render_preview.py` | redraws `docs/panel-preview.png` |
| `src/ESP32-HUB75-*`, `src/platforms/` | HUB75 panel driver vendored from Waveshare's Arduino examples (copied from infopanel64) |
| `src/secrets.example.h` | template for `src/secrets.h` |
| `partitions_32MB.csv` | flash layout (two 4 MB app slots) |
| `GUIDE.md` | step-by-step guide for guests: opening the page and picking players |
| `CLAUDE.md` | engineering notes and status |

## Credits and licences

- **HUB75 driver:** [ESP32-HUB75-MatrixPanel-DMA](https://github.com/mrcodetastic/ESP32-HUB75-MatrixPanel-DMA) by mrcodetastic, vendored from Waveshare's examples.
- **Board configuration:** Waveshare's [ESP32-S3-RGB-Matrix](https://www.waveshare.com/esp32-s3-rgb-matrix.htm) examples (Apache-2.0), via infopanel64.
- **Data:** [Sleeper](https://sleeper.com/), under its API terms (non-commercial use); game scores from ESPN's public scoreboard endpoint.
- **Fonts:** X11 misc-fixed 5×7 (public domain) and u8g2 "squeezed" regular 7 (public domain), both taken as BDF from [olikraus/u8g2](https://github.com/olikraus/u8g2). TomThumb ships with Adafruit GFX.
