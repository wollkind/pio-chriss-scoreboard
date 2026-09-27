// pio-chriss-scoreboard: live fantasy football points on the Waveshare ESP32-S3-RGB-Matrix with a
// 64x64 HUB75 panel.
//
// Up to 9 players on one screen, in the order set on the web page. Each player gets a row: a
// position-coloured bar, a label and this week's fantasy points, with a small football before the
// points while the player's team has the ball. A player's points turn green for a few seconds when
// they change.
//
// Players are picked on a web page served by the board (http://scoreboard.local/). The page's
// JavaScript downloads Sleeper's player list itself, so the board never handles that 5 MB file; it
// only stores the chosen IDs and labels (NVS, survives reboots).
//
// Data, fetched by a background task on core 0 every POLL_MS:
//   - Points: Sleeper (no API key). /v1/state/nfl gives the season and week, then each player's
//     stats for that week (api.sleeper.com/stats/nfl/player/<id>, about 1 KB each).
//   - Possession: ESPN's public scoreboard (one ~270 KB reply for the whole week, parsed as a
//     stream through a filter, so only a few hundred bytes are kept). The game scores it also
//     carries are shown on the web page, not the panel.
//
// Boot plays a startup animation (startup.cpp) while WiFi connects.
//
// Display setup (HUB75 config, off-screen canvas, changed-pixel push) follows infopanel64.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <ArduinoOTA.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <Adafruit_GFX.h>
#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <algorithm>
#include <cmath>
#include "abbrev.h"
#include "fonts/Font5x7.h"
#include "fonts/FontSqueezed7.h"
#include "startup.h"
#include "web_page.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""   // empty: anyone on the network can flash the panel; set one in secrets.h
#endif

// ---- Hardware ----------------------------------------------------------------------
#define PANEL_W            64
#define PANEL_H            64
#define ROTATION           0       // quarter turns clockwise, applied in present()

// ---- Behaviour ---------------------------------------------------------------------
#define FRAME_MS           50      // ~20 fps; only changed pixels reach the panel
#define DEFAULT_BRIGHTNESS 60      // 0-255, changeable on the web page
#define MAX_PLAYERS        9
#define LABEL_X            3       // after the 2 px position bar
#define LABEL_GAP          2       // minimum blank columns between the label and the points
#define MIN_ROW_H          7       // 9 rows x 7 px = 63; row 63 stays free for the error pixel
#define BALL_W             5       // possession football, 5x3
#define BALL_GAP           2       // blank columns between the football and the points
#define SORT_BY_POINTS     0       // 0: the order set on the web page (arrows); 1: highest points first
#define FLASH_MS           8000    // points that changed are drawn green this long
#define STARTUP_ANIMATION  1       // 0: skip the startup animation
#define HOSTNAME           "scoreboard"

// ---- Data ----------------------------------------------------------------------------
#define POLL_MS            (30UL * 1000)        // one round of fetches (players + scoreboard)
#define STATE_REFRESH_MS   (30UL * 60 * 1000)   // season/week check
#define STALE_MS           (5UL * 60 * 1000)    // no successful round for this long: points grey
#define HTTP_TIMEOUT_MS    10000
#define MAX_GAMES          16
#define JSON_NESTING_LIMIT 32      // ESPN's scoreboard is 15 levels deep (2026-09-27)

static_assert(PANEL_W == PANEL_H, "quarter-turn rotation needs a square panel");

enum PointsState : int8_t {
  PTS_UNKNOWN = 0,   // not fetched yet
  PTS_NO_GAME = 1,   // Sleeper returned null: no stats this week yet (game not started, or bye)
  PTS_OK = 2,
};

struct Player {
  char id[12];       // Sleeper player_id: digits for players, team abbreviation for defenses
  char label[16];    // shown on the panel, cut to the width that fits
  char pos[4];
  char team[4];
  float pts;
  PointsState state;
  uint32_t changed_ms;   // millis() of the last points change, for the flash
};

enum GameState : uint8_t { GAME_PRE, GAME_LIVE, GAME_FINAL };

struct Game {
  char home[4];
  char away[4];
  int16_t home_score;
  int16_t away_score;
  GameState state;
  bool halftime;
  uint8_t period;
  char clock[6];      // "4:12"
  char kickoff[8];    // "1:00P", from ESPN's shortDetail
  char poss[4];       // abbreviation of the team with the ball; empty when unknown or not live
  bool red_zone;
};

// Name sizes offered on the web page. The label and points share a baseline; `height` is the row.
struct RowFont {
  const GFXfont *label;    // nullptr: Adafruit GFX's built-in 6x8 font
  const GFXfont *points;
  int8_t baseline;         // cursor y offset from the row top (0 for the built-in font)
  int8_t height;
};
static const RowFont ROW_FONTS[] = {
    {nullptr, nullptr, 0, 8},                                   // 0 Large: about 6 letters
    {&Font5x7, &Font5x7, Font5x7_ASCENT, 7},                    // 1 Medium: about 7-8 letters
    {&FontSqueezed7, &Font5x7, FontSqueezed7_ASCENT, 8},        // 2 Narrow: about 8-9 letters
};
static constexpr int NUM_ROW_FONTS = sizeof(ROW_FONTS) / sizeof(ROW_FONTS[0]);

static MatrixPanel_I2S_DMA *display = nullptr;
static GFXcanvas16 *canvas = nullptr;
static uint16_t g_shown[PANEL_W * PANEL_H];    // what the panel currently shows

static WebServer server(80);
static Preferences prefs;

// Everything below is shared by loop() (core 1) and the fetch task (core 0): hold g_lock.
static SemaphoreHandle_t g_lock = nullptr;
static Player g_players[MAX_PLAYERS];
static int g_count = 0;
static uint32_t g_generation = 0;             // bumped on every roster change
static char g_scoring[12] = "pts_ppr";        // pts_ppr, pts_half_ppr or pts_std
static int g_brightness = DEFAULT_BRIGHTNESS;
static int g_font = 1;                        // index into ROW_FONTS; Medium fits 9 rows cleanly
static char g_season[8] = "";
static char g_season_type[12] = "regular";
static int g_week = 0;
static Game g_games[MAX_GAMES];
static int g_game_count = 0;
static bool g_have_games = false;
static uint32_t g_last_ok_ms = 0;             // end of the last round with no errors
static int g_status = 0;                      // last HTTP code, or negative for local errors

static TaskHandle_t g_fetch_task = nullptr;
static uint32_t g_startup_ms = 0;
static bool g_startup_running = STARTUP_ANIMATION;

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static const uint16_t COLOR_TEXT    = rgb565(235, 235, 235);
static const uint16_t COLOR_POINTS  = rgb565(255, 215, 140);
static const uint16_t COLOR_FLASH   = rgb565(80, 255, 120);
static const uint16_t COLOR_DIM     = rgb565(80, 80, 80);
static const uint16_t COLOR_WARN    = rgb565(255, 140, 0);
static const uint16_t COLOR_INFO    = rgb565(120, 200, 240);
static const uint16_t COLOR_BALL    = rgb565(200, 105, 35);    // leather brown
static const uint16_t COLOR_LACE    = rgb565(255, 255, 255);
static const uint16_t COLOR_RED_ZONE = rgb565(255, 40, 40);

static uint16_t positionColor(const char *pos)
{
  if (!strcmp(pos, "QB"))  return rgb565(255, 70, 70);
  if (!strcmp(pos, "RB"))  return rgb565(60, 220, 90);
  if (!strcmp(pos, "WR"))  return rgb565(70, 150, 255);
  if (!strcmp(pos, "TE"))  return rgb565(255, 160, 40);
  if (!strcmp(pos, "K"))   return rgb565(200, 110, 255);
  if (!strcmp(pos, "DEF")) return rgb565(160, 160, 160);
  return COLOR_DIM;
}

static void copyStr(char *dst, size_t size, const char *src)
{
  strlcpy(dst, src ? src : "", size);
}

// ---- Games -----------------------------------------------------------------------------

// Caller holds g_lock. The player's team is matched against both sides of each game.
static const Game *findGame(const char *team)
{
  for (int i = 0; i < g_game_count; ++i) {
    if (!strcmp(g_games[i].home, team) || !strcmp(g_games[i].away, team)) {
      return &g_games[i];
    }
  }
  return nullptr;
}

// The game line for one player's team, shown on the web page (the panel only shows possession).
// `alt` picks the clock instead of the opponent for live games.
struct ScoreLine {
  bool has_ball;
  bool red_zone;
  char score[10];        // "17-10", own team first
  char info[12];         // "@MIA", "Q3 4:12", "HALF", "F", "BYE", "@MIA 1:00P"
};

static bool scoreLine(const char *team, bool alt, ScoreLine &out)
{
  out = {};
  if (!g_have_games || !team[0]) {
    return false;
  }
  const Game *game = findGame(team);
  if (!game) {
    copyStr(out.info, sizeof(out.info), "BYE");
    return true;
  }
  const bool home = !strcmp(game->home, team);
  const char *opponent = home ? game->away : game->home;
  char versus[8];
  snprintf(versus, sizeof(versus), "%s%s", home ? "v" : "@", opponent);

  if (game->state == GAME_PRE) {
    snprintf(out.info, sizeof(out.info), "%s %s", versus, game->kickoff);
    return true;
  }
  const int own = home ? game->home_score : game->away_score;
  const int other = home ? game->away_score : game->home_score;
  snprintf(out.score, sizeof(out.score), "%d-%d", own, other);
  if (game->state == GAME_FINAL) {
    snprintf(out.info, sizeof(out.info), "%s F", versus);
    return true;
  }
  out.has_ball = game->poss[0] && !strcmp(game->poss, team);
  out.red_zone = out.has_ball && game->red_zone;
  if (game->halftime) {
    copyStr(out.info, sizeof(out.info), alt ? "HALF" : versus);
  } else if (alt) {
    if (game->period > 4) {
      snprintf(out.info, sizeof(out.info), "OT %s", game->clock);
    } else {
      snprintf(out.info, sizeof(out.info), "Q%d %s", game->period, game->clock);
    }
  } else {
    copyStr(out.info, sizeof(out.info), versus);
  }
  return true;
}

// ---- Roster storage ----------------------------------------------------------------

static bool validId(const char *id)
{
  const size_t n = strlen(id);
  if (n == 0 || n >= sizeof(Player::id)) {
    return false;
  }
  for (size_t i = 0; i < n; ++i) {
    if (!isalnum(static_cast<unsigned char>(id[i]))) {
      return false;   // it goes into a URL path
    }
  }
  return true;
}

// Applies {"scoring", "brightness", "font", "players": [{id, label, pos, team}, ...]}.
// Caller holds g_lock. Returns false (and changes nothing) on bad input.
static bool applyConfig(JsonDocument &doc)
{
  JsonArray list = doc["players"].as<JsonArray>();
  if (list.isNull() || list.size() > MAX_PLAYERS) {
    return false;
  }
  Player next[MAX_PLAYERS] = {};
  int n = 0;
  for (JsonObject p : list) {
    Player &q = next[n];
    copyStr(q.id, sizeof(q.id), p["id"] | "");
    if (!validId(q.id)) {
      return false;
    }
    copyStr(q.label, sizeof(q.label), p["label"] | q.id);
    copyStr(q.pos, sizeof(q.pos), p["pos"] | "");
    copyStr(q.team, sizeof(q.team), p["team"] | "");
    // Keep the points of players who were already on the roster, so a save doesn't blank the panel.
    for (int i = 0; i < g_count; ++i) {
      if (!strcmp(g_players[i].id, q.id)) {
        q.pts = g_players[i].pts;
        q.state = g_players[i].state;
      }
    }
    ++n;
  }

  const char *scoring = doc["scoring"] | g_scoring;
  if (!strcmp(scoring, "pts_ppr") || !strcmp(scoring, "pts_half_ppr") || !strcmp(scoring, "pts_std")) {
    if (strcmp(scoring, g_scoring) != 0) {
      for (int i = 0; i < n; ++i) {
        next[i].state = PTS_UNKNOWN;   // different scoring: points are refetched
      }
    }
    copyStr(g_scoring, sizeof(g_scoring), scoring);
  }
  g_brightness = std::min(255, std::max(1, doc["brightness"] | g_brightness));
  g_font = std::min(NUM_ROW_FONTS - 1, std::max(0, doc["font"] | g_font));

  memcpy(g_players, next, sizeof(g_players));
  g_count = n;
  ++g_generation;
  return true;
}

// Caller holds g_lock.
static void configToJson(JsonDocument &doc, bool with_points)
{
  doc["scoring"] = g_scoring;
  doc["brightness"] = g_brightness;
  doc["font"] = g_font;
  JsonArray list = doc["players"].to<JsonArray>();
  for (int i = 0; i < g_count; ++i) {
    const Player &p = g_players[i];
    JsonObject o = list.add<JsonObject>();
    o["id"] = p.id;
    o["label"] = p.label;
    o["pos"] = p.pos;
    o["team"] = p.team;
    if (with_points) {
      if (p.state == PTS_OK) {
        o["pts"] = p.pts;
      } else {
        o["pts"] = nullptr;
      }
      o["state"] = static_cast<int>(p.state);
      ScoreLine line;
      if (scoreLine(p.team, false, line)) {
        ScoreLine clock;
        scoreLine(p.team, true, clock);
        char text[48];
        snprintf(text, sizeof(text), "%s%s%s%s%s%s", line.has_ball ? "(ball) " : "", line.score,
                 line.score[0] ? " " : "", line.info,
                 strcmp(clock.info, line.info) ? " \xC2\xB7 " : "", strcmp(clock.info, line.info) ? clock.info : "");
        o["game"] = text;
      }
    }
  }
}

static void saveConfig()
{
  JsonDocument doc;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  configToJson(doc, false);
  xSemaphoreGive(g_lock);
  String out;
  serializeJson(doc, out);
  prefs.putString("config", out);
}

static void loadConfig()
{
  const String saved = prefs.getString("config", "");
  if (saved.isEmpty()) {
    return;
  }
  JsonDocument doc;
  if (deserializeJson(doc, saved)) {
    return;
  }
  xSemaphoreTake(g_lock, portMAX_DELAY);
  applyConfig(doc);
  xSemaphoreGive(g_lock);
}

// ---- Fetching --------------------------------------------------------------------------

// GET url into doc (optionally through a filter). With `stream`, the body is parsed straight off
// the connection instead of being buffered first (for the large ESPN reply). Returns the HTTP code,
// or -1 begin failed, -2 JSON parse error.
static int getJson(const char *url, JsonDocument &doc, JsonDocument *filter, bool stream = false)
{
  WiFiClientSecure client;
  client.setInsecure();   // public read-only data; skips shipping a CA bundle
  HTTPClient http;
  // HTTP/1.0: plain body and the server closes (see the infopanel64 v4.1 note on chunked replies
  // timing out in HTTPClient). It also makes the stream parse below see only the JSON.
  http.useHTTP10(true);
  http.setConnectTimeout(HTTP_TIMEOUT_MS);
  if (!http.begin(client, url)) {
    return -1;
  }
  http.setTimeout(HTTP_TIMEOUT_MS);
  client.setTimeout(HTTP_TIMEOUT_MS);
  const int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return code;
  }
  // ESPN's scoreboard nests 15 levels deep. ArduinoJson's default limit (10) also applies to the
  // parts a filter skips, so without a higher limit the parse fails with TooDeep.
  const auto nesting = DeserializationOption::NestingLimit(JSON_NESTING_LIMIT);
  DeserializationError err;
  if (stream) {
    err = filter ? deserializeJson(doc, http.getStream(), DeserializationOption::Filter(*filter), nesting)
                 : deserializeJson(doc, http.getStream(), nesting);
    http.end();
  } else {
    const String body = http.getString();
    http.end();
    err = filter ? deserializeJson(doc, body, DeserializationOption::Filter(*filter), nesting)
                 : deserializeJson(doc, body, nesting);
  }
  return err ? -2 : code;
}

// /v1/state/nfl: {"week":3,"season":"2026","season_type":"regular","display_week":3,...}
static bool fetchState()
{
  JsonDocument doc;
  const int code = getJson("https://api.sleeper.app/v1/state/nfl", doc, nullptr);
  if (code != HTTP_CODE_OK) {
    g_status = code;
    return false;
  }
  const char *season = doc["season"] | "";
  const char *type = doc["season_type"] | "regular";
  const int week = doc["week"] | 0;
  if (!season[0] || week <= 0) {
    g_status = -3;
    return false;
  }
  xSemaphoreTake(g_lock, portMAX_DELAY);
  copyStr(g_season, sizeof(g_season), season);
  // Outside the season the state reports "off"; the stats endpoint needs a real type.
  copyStr(g_season_type, sizeof(g_season_type), strcmp(type, "off") ? type : "regular");
  g_week = week;
  xSemaphoreGive(g_lock);
  return true;
}

// One player's points for the current week. The reply is the stat line, or the literal null when
// the player has no stats that week yet.
static bool fetchPlayer(const char *id, const char *scoring, const char *season, const char *type,
                        int week, float &pts, PointsState &state)
{
  char url[192];
  snprintf(url, sizeof(url),
           "https://api.sleeper.com/stats/nfl/player/%s?season_type=%s&season=%s&week=%d",
           id, type, season, week);
  JsonDocument filter;
  filter["stats"][scoring] = true;
  JsonDocument doc;
  const int code = getJson(url, doc, &filter);
  if (code != HTTP_CODE_OK) {
    g_status = code;
    return false;
  }
  if (doc.isNull() || doc["stats"].isNull()) {
    state = PTS_NO_GAME;
    pts = 0.0f;
    return true;
  }
  state = PTS_OK;
  pts = doc["stats"][scoring] | 0.0f;   // stats present but no points field: 0
  return true;
}

// ESPN abbreviations match Sleeper's except Washington.
static void toSleeperTeam(char *dst, size_t size, const char *espn)
{
  copyStr(dst, size, strcmp(espn, "WSH") ? espn : "WAS");
}

// "9/27 - 1:00 PM EDT" -> "1:00P". Falls back to the first characters of the text.
static void parseKickoff(char *dst, size_t size, const char *detail)
{
  const char *dash = strstr(detail, " - ");
  if (dash) {
    int hour = 0, minute = 0;
    char ampm[3] = "";
    if (sscanf(dash + 3, "%d:%d %2s", &hour, &minute, ampm) == 3) {
      snprintf(dst, size, "%d:%02d%c", hour, minute, ampm[0]);
      return;
    }
  }
  copyStr(dst, size, detail);
}

// ESPN scoreboard for the current week. Only these fields survive the filter:
//   events[].competitions[0].competitors[].{homeAway, score, team.{id, abbreviation}}
//   events[].competitions[0].status.{period, displayClock, type.{name, state, shortDetail}}
//   events[].competitions[0].situation.{possession, isRedZone}   (live games only)
static bool fetchScoreboard()
{
  JsonDocument filter;
  JsonObject comp = filter["events"][0]["competitions"][0].to<JsonObject>();
  JsonObject team = comp["competitors"][0].to<JsonObject>();
  team["homeAway"] = true;
  team["score"] = true;
  team["team"]["id"] = true;
  team["team"]["abbreviation"] = true;
  comp["status"]["period"] = true;
  comp["status"]["displayClock"] = true;
  comp["status"]["type"]["name"] = true;
  comp["status"]["type"]["state"] = true;
  comp["status"]["type"]["shortDetail"] = true;
  comp["situation"]["possession"] = true;
  comp["situation"]["isRedZone"] = true;

  JsonDocument doc;
  const int code = getJson("https://site.api.espn.com/apis/site/v2/sports/football/nfl/scoreboard",
                           doc, &filter, true);
  if (code != HTTP_CODE_OK) {
    g_status = code;
    return false;
  }

  Game games[MAX_GAMES] = {};
  int n = 0;
  for (JsonObject event : doc["events"].as<JsonArray>()) {
    if (n >= MAX_GAMES) {
      break;
    }
    JsonObject c = event["competitions"][0];
    Game &game = games[n];
    const char *poss_id = c["situation"]["possession"] | "";
    for (JsonObject side : c["competitors"].as<JsonArray>()) {
      const bool home = !strcmp(side["homeAway"] | "", "home");
      char abbr[4];
      toSleeperTeam(abbr, sizeof(abbr), side["team"]["abbreviation"] | "");
      const int score = atoi(side["score"] | "0");
      copyStr(home ? game.home : game.away, 4, abbr);
      (home ? game.home_score : game.away_score) = score;
      if (poss_id[0] && !strcmp(side["team"]["id"] | "", poss_id)) {
        copyStr(game.poss, sizeof(game.poss), abbr);
      }
    }
    JsonObject status = c["status"];
    const char *state = status["type"]["state"] | "pre";
    game.state = !strcmp(state, "in") ? GAME_LIVE : (!strcmp(state, "post") ? GAME_FINAL : GAME_PRE);
    game.halftime = !strcmp(status["type"]["name"] | "", "STATUS_HALFTIME");
    game.period = status["period"] | 0;
    copyStr(game.clock, sizeof(game.clock), status["displayClock"] | "");
    parseKickoff(game.kickoff, sizeof(game.kickoff), status["type"]["shortDetail"] | "");
    game.red_zone = c["situation"]["isRedZone"] | false;
    if (game.home[0] && game.away[0]) {
      ++n;
    }
  }

  xSemaphoreTake(g_lock, portMAX_DELAY);
  memcpy(g_games, games, sizeof(g_games));
  g_game_count = n;
  g_have_games = true;
  xSemaphoreGive(g_lock);
  return true;
}

static void fetchTask(void *)
{
  uint32_t last_state_ms = 0;
  bool have_state = false;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      vTaskDelay(pdMS_TO_TICKS(2000));
      continue;
    }
    if (!have_state || millis() - last_state_ms > STATE_REFRESH_MS) {
      if (fetchState()) {
        have_state = true;
        last_state_ms = millis();
      } else if (!have_state) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        continue;
      }
    }

    // Snapshot what to fetch, then fetch without holding the lock.
    Player roster[MAX_PLAYERS];
    char scoring[12], season[8], type[12];
    int count, week;
    uint32_t generation;
    xSemaphoreTake(g_lock, portMAX_DELAY);
    memcpy(roster, g_players, sizeof(roster));
    count = g_count;
    generation = g_generation;
    copyStr(scoring, sizeof(scoring), g_scoring);
    copyStr(season, sizeof(season), g_season);
    copyStr(type, sizeof(type), g_season_type);
    week = g_week;
    xSemaphoreGive(g_lock);

    bool all_ok = count == 0 || fetchScoreboard();
    for (int i = 0; i < count; ++i) {
      float pts = 0.0f;
      PointsState state = PTS_UNKNOWN;
      if (!fetchPlayer(roster[i].id, scoring, season, type, week, pts, state)) {
        all_ok = false;
        continue;
      }
      xSemaphoreTake(g_lock, portMAX_DELAY);
      if (g_generation == generation) {
        Player &p = g_players[i];
        if (p.state == PTS_OK && state == PTS_OK && fabsf(p.pts - pts) > 0.001f) {
          p.changed_ms = millis();
        }
        p.pts = pts;
        p.state = state;
      }
      xSemaphoreGive(g_lock);
    }
    if (all_ok) {
      g_status = HTTP_CODE_OK;
      g_last_ok_ms = millis();
    }

    // Wait for the next round; a roster change on the web page wakes the task early.
    ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(POLL_MS));
  }
}

// ---- Web page ------------------------------------------------------------------------

static void handleIndex()
{
  server.send_P(200, "text/html", INDEX_HTML);
}

static void handleGetConfig()
{
  JsonDocument doc;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  configToJson(doc, true);
  doc["season"] = g_season;
  doc["season_type"] = g_season_type;
  doc["week"] = g_week;
  doc["status"] = g_status;
  doc["last_ok_s"] = g_last_ok_ms ? static_cast<int>((millis() - g_last_ok_ms) / 1000) : -1;
  xSemaphoreGive(g_lock);
  String out;
  serializeJson(doc, out);
  server.send(200, "application/json", out);
}

static void handlePostConfig()
{
  JsonDocument doc;
  if (deserializeJson(doc, server.arg("plain"))) {
    server.send(400, "text/plain", "bad JSON");
    return;
  }
  xSemaphoreTake(g_lock, portMAX_DELAY);
  const bool ok = applyConfig(doc);
  xSemaphoreGive(g_lock);
  if (!ok) {
    server.send(400, "text/plain", "at most 9 players, each with an alphanumeric id");
    return;
  }
  saveConfig();
  if (g_fetch_task) {
    xTaskNotifyGive(g_fetch_task);
  }
  handleGetConfig();
}

// ---- Display -------------------------------------------------------------------------

static void drawTextCentered(const char *text, int y, uint16_t color)
{
  int16_t bx, by;
  uint16_t bw, bh;
  canvas->setFont(nullptr);
  canvas->getTextBounds(text, 0, y, &bx, &by, &bw, &bh);
  canvas->setTextColor(color);
  canvas->setCursor((PANEL_W - static_cast<int>(bw)) / 2, y);
  canvas->print(text);
}

// Width in pixels from the cursor to the right edge of the ink, in the canvas's current font.
static int inkWidth(const char *text, const GFXfont *font)
{
  if (!text[0]) {
    return 0;
  }
  int16_t bx, by;
  uint16_t bw, bh;
  canvas->getTextBounds(text, 0, 32, &bx, &by, &bw, &bh);
  if (!font) {
    return static_cast<int>(bw) - 1;   // the built-in font's last column is blank spacing
  }
  return bw ? bx + static_cast<int>(bw) : 0;
}

static void formatPoints(char *out, size_t size, float pts)
{
  // Four characters fit after the label: "23.4", "-2.0"; wider values drop the decimal.
  if (pts >= 99.95f || pts <= -9.95f) {
    snprintf(out, size, "%d", static_cast<int>(lroundf(pts)));
  } else {
    snprintf(out, size, "%.1f", pts);
  }
}

static void drawStatus(const char *line1, uint16_t c1, const char *line2, uint16_t c2)
{
  drawTextCentered(line1, 20, c1);
  if (line2) {
    drawTextCentered(line2, 34, c2);
  }
}

// Possession marker: a football, 5x3, brown with a white lace (red inside the opponent's 20).
//   .###.
//   ##-##    (- = lace)
//   .###.
static void drawBall(int x, int y, bool red_zone)
{
  const uint16_t c = red_zone ? COLOR_RED_ZONE : COLOR_BALL;
  canvas->drawFastHLine(x + 1, y, 3, c);
  canvas->drawFastHLine(x, y + 1, 5, c);
  canvas->drawPixel(x + 2, y + 1, COLOR_LACE);
  canvas->drawFastHLine(x + 1, y + 2, 3, c);
}

// Possession state of a team's game. Caller holds g_lock.
static void teamPossession(const char *team, bool &live, bool &has_ball, bool &red_zone)
{
  const Game *game = team[0] ? findGame(team) : nullptr;
  live = game && game->state == GAME_LIVE;
  has_ball = live && game->poss[0] && !strcmp(game->poss, team);
  red_zone = has_ball && game->red_zone;
}

// One player row, `row_h` pixels tall starting at y.
static void drawPlayer(const Player &p, int y, int row_h, const RowFont &rf, bool stale, uint32_t now)
{
  canvas->fillRect(0, y, 2, row_h - 1, positionColor(p.pos));

  // Points, right-aligned to the panel edge.
  char pts[8];
  uint16_t color = COLOR_POINTS;
  if (p.state == PTS_OK) {
    formatPoints(pts, sizeof(pts), p.pts);
    if (p.changed_ms && now - p.changed_ms < FLASH_MS) {
      color = COLOR_FLASH;
    }
  } else {
    snprintf(pts, sizeof(pts), "-");   // no stats yet this week (or not fetched yet)
    color = COLOR_DIM;
  }
  if (stale && p.state != PTS_UNKNOWN) {
    color = COLOR_DIM;
  }
  canvas->setFont(rf.points);
  const int pts_x = PANEL_W - inkWidth(pts, rf.points);
  canvas->setTextColor(color);
  canvas->setCursor(pts_x, y + rf.baseline);
  canvas->print(pts);

  // Football before the points while the team has the ball. Its space is kept for the whole game,
  // so the label doesn't change length every time possession changes.
  bool live, has_ball, red_zone;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  teamPossession(p.team, live, has_ball, red_zone);
  xSemaphoreGive(g_lock);
  const int ball_x = pts_x - BALL_GAP - BALL_W;
  if (has_ball) {
    drawBall(ball_x, y + 2, red_zone);
  }

  // Label: shortened from the middle until it fits (abbrev.h).
  canvas->setFont(rf.label);
  char label[sizeof(Player::label)];
  copyStr(label, sizeof(label), p.label);
  const int right = live ? ball_x : pts_x;
  abbreviate(label, right - LABEL_GAP - LABEL_X, [&rf](const char *text) { return inkWidth(text, rf.label); });
  canvas->setTextColor(COLOR_TEXT);
  canvas->setCursor(LABEL_X, y + rf.baseline);
  canvas->print(label);
}

static void drawScreen(uint32_t now)
{
  canvas->fillScreen(0);
  canvas->setFont(nullptr);
  canvas->setTextSize(1);

  if (strlen(WIFI_SSID) == 0) {
    drawStatus("SET WIFI", COLOR_WARN, "secrets.h", COLOR_DIM);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    drawStatus("WIFI..", COLOR_DIM, nullptr, 0);
    return;
  }

  Player rows[MAX_PLAYERS];
  int count, font;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  memcpy(rows, g_players, sizeof(rows));
  count = g_count;
  font = g_font;
  xSemaphoreGive(g_lock);

  if (count == 0) {
    // Show where the web page is: the IP always works, scoreboard.local depends on the phone.
    char ip[16];
    snprintf(ip, sizeof(ip), "%s", WiFi.localIP().toString().c_str());
    const char *dot = strchr(strchr(strchr(ip, '.') + 1, '.') + 1, '.');   // last octet group
    char head[16];
    snprintf(head, sizeof(head), "%.*s", static_cast<int>(dot - ip), ip);
    drawTextCentered("PICK", 4, COLOR_WARN);
    drawTextCentered("PLAYERS", 14, COLOR_WARN);
    drawTextCentered("http://", 30, COLOR_DIM);
    drawTextCentered(head, 40, COLOR_INFO);
    drawTextCentered(dot, 50, COLOR_INFO);
    return;
  }

  if (SORT_BY_POINTS) {
    // Stable, so equal scores keep the chosen order; players without stats go last.
    std::stable_sort(rows, rows + count, [](const Player &a, const Player &b) {
      const float pa = a.state == PTS_OK ? a.pts : -1000.0f;
      const float pb = b.state == PTS_OK ? b.pts : -1000.0f;
      return pa > pb;
    });
  }

  // Everyone on one screen: rows of 64 / count px, at least MIN_ROW_H (9 players: 7 px) and at
  // most the font's height plus a blank line. When a row is shorter than the font, descenders
  // (g, j, p, q, y) that would reach into the next row are cleared before that row is drawn.
  const RowFont &rf = ROW_FONTS[font];
  const int row_h = std::min(rf.height + 1, std::max(MIN_ROW_H, PANEL_H / count));
  const bool stale = g_last_ok_ms == 0 || now - g_last_ok_ms > STALE_MS;
  for (int i = 0; i < count; ++i) {
    const int y = i * row_h;
    drawPlayer(rows[i], y, row_h, rf, stale, now);
    if (row_h < rf.height) {
      canvas->fillRect(0, y + row_h, PANEL_W, rf.height - row_h, 0);
    }
  }
  canvas->setFont(nullptr);

  // Fetch problem: one red pixel in the bottom-right corner.
  if (g_status != 0 && g_status != HTTP_CODE_OK) {
    canvas->drawPixel(PANEL_W - 1, PANEL_H - 1, rgb565(255, 0, 0));
  }
}

// Push the frame to the panel turned ROTATION quarter turns clockwise, sending only pixels that
// differ from what the panel already shows.
static void present()
{
  const uint16_t *buf = canvas->getBuffer();
  for (int y = 0; y < PANEL_H; ++y) {
    for (int x = 0; x < PANEL_W; ++x) {
      int px = x;
      int py = y;
      switch (ROTATION) {
        case 1:
          px = PANEL_W - 1 - y;
          py = x;
          break;
        case 2:
          px = PANEL_W - 1 - x;
          py = PANEL_H - 1 - y;
          break;
        case 3:
          px = y;
          py = PANEL_H - 1 - x;
          break;
        default:
          break;
      }
      const uint16_t color = buf[y * PANEL_W + x];
      const int i = py * PANEL_W + px;
      if (color != g_shown[i]) {
        g_shown[i] = color;
        display->drawPixel(px, py, color);
      }
    }
  }
}

static void updateBrightness()
{
  static int current = -1;
  const int target = g_brightness;
  if (target != current) {
    display->setBrightness8(target);
    current = target;
  }
}

// ---- Network services ------------------------------------------------------------------

static void drawOtaScreen(const char *title, int percent, uint16_t color)
{
  canvas->fillScreen(0);
  drawTextCentered(title, 16, color);
  if (percent >= 0) {
    canvas->drawRect(6, 30, PANEL_W - 12, 7, COLOR_DIM);
    canvas->fillRect(8, 32, (PANEL_W - 16) * percent / 100, 3, COLOR_INFO);
  }
  present();
}

// Started the first time WiFi connects. ArduinoOTA also starts mDNS with HOSTNAME, so the web page
// is advertised on the same responder.
static void serviceNetwork()
{
  static bool started = false;
  if (!started) {
    if (strlen(WIFI_SSID) == 0 || WiFi.status() != WL_CONNECTED) {
      return;
    }
    ArduinoOTA.setHostname(HOSTNAME);
    if (strlen(OTA_PASSWORD) > 0) {
      ArduinoOTA.setPassword(OTA_PASSWORD);
    }
    ArduinoOTA.onStart([]() {
      if (g_fetch_task) {
        vTaskSuspend(g_fetch_task);
      }
      drawOtaScreen("UPDATING", 0, COLOR_INFO);
    });
    ArduinoOTA.onProgress([](unsigned int done, unsigned int total) {
      static int last_percent = -1;
      const int percent = total ? static_cast<int>(done * 100ULL / total) : 0;
      if (percent != last_percent) {
        last_percent = percent;
        drawOtaScreen("UPDATING", percent, COLOR_INFO);
      }
    });
    ArduinoOTA.onEnd([]() { drawOtaScreen("REBOOTING", 100, COLOR_INFO); });
    ArduinoOTA.onError([](ota_error_t error) {
      char text[16];
      snprintf(text, sizeof(text), "OTA ERR %d", static_cast<int>(error));
      drawOtaScreen(text, -1, COLOR_WARN);
      delay(2000);
      if (g_fetch_task) {
        vTaskResume(g_fetch_task);
      }
    });
    ArduinoOTA.begin();

    server.on("/", HTTP_GET, handleIndex);
    server.on("/api/config", HTTP_GET, handleGetConfig);
    server.on("/api/config", HTTP_POST, handlePostConfig);
    server.onNotFound([]() { server.send(404, "text/plain", "not found"); });
    server.begin();
    MDNS.addService("http", "tcp", 80);
    started = true;
  }
  ArduinoOTA.handle();
  server.handleClient();
}

void setup()
{
  Serial.begin(115200);
  Serial.setTxTimeoutMs(0);       // never stall when nothing is reading the port

  g_lock = xSemaphoreCreateMutex();
  prefs.begin("scoreboard", false);
  loadConfig();

  HUB75_I2S_CFG mxconfig(PANEL_W, PANEL_H, 1);
  mxconfig.gpio.e = 9;
  mxconfig.clkphase = false;
  mxconfig.driver = HUB75_I2S_CFG::FM6126A;
  display = new MatrixPanel_I2S_DMA(mxconfig);
  display->begin();
  display->clearScreen();
  updateBrightness();

  canvas = new GFXcanvas16(PANEL_W, PANEL_H);
  canvas->setTextWrap(false);

  if (strlen(WIFI_SSID) > 0) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    // No modem sleep: with it, every reply waits for the access point's next beacon, which made
    // OTA (1 KB per round trip) crawl.
    WiFi.setSleep(false);
    WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
    xTaskCreatePinnedToCore(fetchTask, "fetch", 12288, nullptr, 1, &g_fetch_task, 0);
  }
  g_startup_ms = millis();
}

void loop()
{
  const uint32_t now = millis();

  serviceNetwork();
  updateBrightness();
  if (g_startup_running) {
    g_startup_running = drawStartup(*canvas, now - g_startup_ms);
  }
  if (!g_startup_running) {
    drawScreen(now);
  }
  present();

  const uint32_t spent = millis() - now;
  delay(spent < FRAME_MS ? FRAME_MS - spent : 1);
}
