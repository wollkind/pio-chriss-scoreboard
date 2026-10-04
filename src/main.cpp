// pio-chriss-scoreboard: live fantasy football points on the Waveshare ESP32-S3-RGB-Matrix with a
// 64x64 HUB75 panel.
//
// Scoreboard: 10 lines of 5 px text (X11 4x6 font). Up to 9 player rows in the order set on the
// web page: a position-coloured bar, a label, a small football while the player's team has the
// ball, and this week's fantasy points. Under a divider, the total line: rotating NFL scores on the
// left, the total of all players' points on the right.
//
// When a player gains points, or loses EVENT_MIN_PTS or more, the scoreboard gives way to
// full-screen screens, then returns: a gain plays a celebration for the kind of play (run, pass,
// catch, defense, kick; touchdowns get their own, celebrate.cpp) and then the update screen
// ("GIBBS / RUSH FOR 30 YDS / +3.0"); a loss shows only the update screen, in red. The play is
// worked out from the change in the player's stats since the previous fetch.
//
// Players are picked on a web page served by the board (http://scoreboard.local/). The page's
// JavaScript downloads Sleeper's player list itself, so the board never handles that 5 MB file; it
// only stores the chosen IDs and labels (NVS, survives reboots).
//
// Data, fetched by a background task on core 0 every POLL_MS:
//   - Points: Sleeper (no API key). /v1/state/nfl gives the season and week, then each player's
//     stats for that week (api.sleeper.com/stats/nfl/player/<id>, about 1 KB each).
//   - Game scores and possession: ESPN's public scoreboard (one ~270 KB reply for the whole week,
//     parsed as a stream through a filter, so only a few hundred bytes are kept).
//
// Boot plays a startup animation (startup.cpp) while WiFi connects.
//
// Display setup (HUB75 config, off-screen canvas, changed-pixel push) follows infopanel64.

#include <Arduino.h>
#include <WiFi.h>
#include <WiFiMulti.h>
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
#include <Fonts/TomThumb.h>
#include "celebrate.h"
#include "fonts/Font4x6.h"
#include "fonts/Font5x7.h"
#include "startup.h"
#include "web_page.h"

#if __has_include("secrets.h")
#include "secrets.h"
#else
#include "secrets.example.h"
#endif
#ifndef WIFI_NETWORKS
#define WIFI_NETWORKS { { WIFI_SSID, WIFI_PASSWORD } }   // older secrets.h: a single network
#endif
#ifndef OTA_PASSWORD
#define OTA_PASSWORD ""   // empty: anyone on the network can flash the panel; set one in secrets.h
#endif

struct WifiNetwork {
  const char *ssid;
  const char *password;
};
static const WifiNetwork WIFI_LIST[] = WIFI_NETWORKS;
static WiFiMulti g_wifi;   // joins the strongest network from WIFI_LIST

static bool wifiConfigured()
{
  for (const WifiNetwork &n : WIFI_LIST) {
    if (n.ssid && n.ssid[0]) {
      return true;
    }
  }
  return false;
}

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
#define ROW_H              6       // 5 px letters + 1 px gap; 9 player rows = rows 0-53
#define DIVIDER_Y          55
#define TOTAL_Y            57      // top of the total line (letters on rows 57-61)
#define SCORES_ROTATE_MS   3000    // each NFL game on the total line shows this long
#define BALL_W             5       // possession football, 5x3
#define BALL_GAP           2       // blank columns between the football and the points
#define SORT_BY_POINTS     0       // 0: the order set on the web page (arrows); 1: highest points first
#define FLASH_MS           8000    // points that changed are drawn green (gain) or red (loss) this long
#define STARTUP_ANIMATION  1       // 0: skip the startup animation
#define EVENT_MIN_PTS      1.0f    // a points loss at least this big gets the update screen; any gain does
#define UPDATE_MS          3500    // how long the update screen shows
#define SHINE_BLINK_MS     140     // name shine before an update: the row blinks inverted, this long on / off
#define SHINE_BLINKS       3       // number of blinks
#define SHINE_BLINK_TOTAL_MS (2 * SHINE_BLINKS * SHINE_BLINK_MS)
#define SHINE_STEP_MS      55      // then a wave: delay from one letter to the next
#define SHINE_HALF_MS      140     // each letter brightens this long, then fades this long
#define SHINE_TAIL_MS      200     // pause on the scoreboard after the shine
#define RAINBOW_STEP_MS    120     // gain under EVENT_MIN_PTS: name lights letter by letter, this far apart
#define RAINBOW_HOLD_MS    1100    // each letter cycles through the rainbow this long
#define RAINBOW_FADE_MS    700     // then fades back to the normal text colour
#define RAINBOW_CYCLE_MS   1800    // one full trip round the colour wheel
#define ROLL_MIN_MS        700     // points roll like an odometer through every tenth: shortest roll
#define ROLL_STEP_MS       40      // plus this much per tenth
#define ROLL_MAX_MS        2500    // longest roll
#define MAX_EVENTS         6       // queued update screens; later ones are dropped when full
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

// Sleeper stat fields kept per player, to describe what happened when the points change.
enum Stat : uint8_t {
  ST_PASS_YD, ST_PASS_TD, ST_PASS_INT, ST_RUSH_YD, ST_RUSH_TD, ST_REC, ST_REC_YD, ST_REC_TD,
  ST_FUM_LOST, ST_FGM, ST_FGM_YDS, ST_XPM, ST_SACK, ST_INT, ST_DEF_TD, ST_FUM_REC, ST_COUNT
};
static const char *const STAT_KEYS[ST_COUNT] = {
    "pass_yd", "pass_td", "pass_int", "rush_yd", "rush_td", "rec", "rec_yd", "rec_td",
    "fum_lost", "fgm", "fgm_yds", "xpm", "sack", "int", "td", "fum_rec"};

struct Player {
  char id[12];       // Sleeper player_id: digits for players, team abbreviation for defenses
  char label[16];    // shown on the panel, shortened to the width that fits
  char pos[4];
  char team[4];
  float pts;
  PointsState state;
  uint32_t changed_ms;   // millis() of the last points change, for the flash
  bool dropped;          // the last points change was a loss: flash red
  bool rolling;          // the points are rolling (or waiting to roll) from roll_from to pts
  float roll_from;       // points shown when the change arrived
  uint32_t roll_ms;      // millis() the roll started on the scoreboard; 0: not started yet
  float stats[ST_COUNT];
};

// One change worth the full-screen treatment.
struct Event {
  char id[12];       // Player::id, for the name shine on the scoreboard
  char label[16];
  char pos[4];
  char action[24];   // "RUSH FOR 30 YDS", "RECEIVING TD", ...
  float delta;       // points gained (negative: lost)
  float pts;         // the player's points afterwards
  Play play;         // picks the celebration
  bool touchdown;
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
static char g_season[8] = "";
static char g_season_type[12] = "regular";
static int g_week = 0;
static Game g_games[MAX_GAMES];
static int g_game_count = 0;
static bool g_have_games = false;
static uint32_t g_last_ok_ms = 0;             // end of the last round with no errors
static int g_status = 0;                      // last HTTP code, or negative for local errors
static int g_espn_status = 0;                 // last ESPN scoreboard result
static char g_json_err[24] = "";              // last JSON parse error

static TaskHandle_t g_fetch_task = nullptr;
static uint32_t g_startup_ms = 0;
static bool g_startup_running = STARTUP_ANIMATION;

// Update screens waiting to be shown (ring buffer, guarded by g_lock), and the one showing.
static Event g_events[MAX_EVENTS];
static int g_event_head = 0;
static int g_event_count = 0;
static Event g_current;
static bool g_showing = false;
static uint32_t g_show_ms = 0;
static uint32_t g_event_seq = 0;
static const char *g_shine_id = nullptr;   // player whose name shines, while drawScreen() runs for the shine
static uint32_t g_shine_t = 0;             // ms into the shine
static bool g_shine_rainbow = false;       // small gain: rainbow letter sequence instead of blink + wave

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static const uint16_t COLOR_TEXT    = rgb565(235, 235, 235);
static const uint16_t COLOR_SHINE_DIM = rgb565(45, 45, 45);   // name letters outside the shine wave
static const uint16_t COLOR_SHINE   = rgb565(255, 245, 170);   // peak of the shine
static const uint16_t COLOR_POINTS  = rgb565(255, 215, 140);
static const uint16_t COLOR_FLASH   = rgb565(80, 255, 120);
static const uint16_t COLOR_DIM     = rgb565(80, 80, 80);
static const uint16_t COLOR_WARN    = rgb565(255, 140, 0);
static const uint16_t COLOR_INFO    = rgb565(120, 200, 240);
static const uint16_t COLOR_BALL    = rgb565(200, 105, 35);    // leather brown
static const uint16_t COLOR_LACE    = rgb565(255, 255, 255);
static const uint16_t COLOR_RED_ZONE = rgb565(255, 40, 40);
static const uint16_t COLOR_TOTAL   = rgb565(255, 190, 60);
static const uint16_t COLOR_DIVIDER = rgb565(90, 70, 130);

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

// Applies {"scoring", "brightness", "players": [{id, label, pos, team}, ...]}.
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
        memcpy(q.stats, g_players[i].stats, sizeof(q.stats));
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

// GET url into doc (optionally through a filter). With `large`, the body is read into a PSRAM
// buffer first (for the ~200 KB ESPN reply). Returns the HTTP code, or -1 begin failed, -2 JSON
// parse error, -4 out of memory, -5 body cut short. The parse error text goes to g_json_err.
static int getJson(const char *url, JsonDocument &doc, JsonDocument *filter, bool large = false)
{
  WiFiClientSecure client;
  client.setInsecure();   // public read-only data; skips shipping a CA bundle
  HTTPClient http;
  // HTTP/1.0: plain body and the server closes (see the infopanel64 v4.1 note on chunked replies
  // timing out in HTTPClient).
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
  if (large) {
    // Parsing straight off the TLS stream failed on the board with -2 every round (v0.5,
    // 2026-09-27), so the whole body is read first. ESPN sends no Content-Length: read until the
    // server closes and nothing is left.
    size_t cap = 512 * 1024, len = 0;
    char *buf = static_cast<char *>(ps_malloc(cap));
    if (!buf) {
      http.end();
      return -4;
    }
    WiFiClient *s = http.getStreamPtr();
    uint32_t last = millis();
    while (millis() - last < HTTP_TIMEOUT_MS) {
      const int avail = s->available();
      if (avail > 0) {
        if (len + avail >= cap) {
          free(buf);
          http.end();
          return -4;
        }
        len += s->read(reinterpret_cast<uint8_t *>(buf) + len, avail);
        last = millis();
      } else if (!s->connected()) {
        break;
      } else {
        vTaskDelay(pdMS_TO_TICKS(5));
      }
    }
    http.end();
    if (len == 0) {
      free(buf);
      return -5;
    }
    err = filter ? deserializeJson(doc, buf, len, DeserializationOption::Filter(*filter), nesting)
                 : deserializeJson(doc, buf, len, nesting);
    free(buf);
  } else {
    const String body = http.getString();
    http.end();
    err = filter ? deserializeJson(doc, body, DeserializationOption::Filter(*filter), nesting)
                 : deserializeJson(doc, body, nesting);
  }
  if (err) {
    copyStr(g_json_err, sizeof(g_json_err), err.c_str());
    return -2;
  }
  return code;
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
                        int week, float &pts, PointsState &state, float *stats)
{
  char url[192];
  snprintf(url, sizeof(url),
           "https://api.sleeper.com/stats/nfl/player/%s?season_type=%s&season=%s&week=%d",
           id, type, season, week);
  JsonDocument filter;
  filter["stats"][scoring] = true;
  for (const char *key : STAT_KEYS) {
    filter["stats"][key] = true;
  }
  JsonDocument doc;
  const int code = getJson(url, doc, &filter);
  if (code != HTTP_CODE_OK) {
    g_status = code;
    return false;
  }
  for (int k = 0; k < ST_COUNT; ++k) {
    stats[k] = doc["stats"][STAT_KEYS[k]] | 0.0f;
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
  g_espn_status = code;
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

// What happened between two stat lines, most notable first. Several plays can fall between two
// fetches; yardage is then the total of all of them.
static void describeChange(const float *before, const float *after, float delta, Event &e)
{
  char *out = e.action;
  const size_t size = sizeof(e.action);
  e.play = PLAY_OTHER;
  e.touchdown = false;
  float d[ST_COUNT];
  for (int k = 0; k < ST_COUNT; ++k) {
    d[k] = after[k] - before[k];
  }
  const int rush = static_cast<int>(lroundf(d[ST_RUSH_YD]));
  const int rec = static_cast<int>(lroundf(d[ST_REC_YD]));
  const int pass = static_cast<int>(lroundf(d[ST_PASS_YD]));
  if (d[ST_DEF_TD] > 0) {
    snprintf(out, size, "DEFENSIVE TD");
    e.play = PLAY_DEFENSE;
    e.touchdown = true;
  } else if (d[ST_RUSH_TD] > 0) {
    snprintf(out, size, "RUSHING TD");
    e.play = PLAY_RUSH;
    e.touchdown = true;
  } else if (d[ST_REC_TD] > 0) {
    snprintf(out, size, "RECEIVING TD");
    e.play = PLAY_CATCH;
    e.touchdown = true;
  } else if (d[ST_PASS_TD] > 0) {
    snprintf(out, size, "TD PASS");
    e.play = PLAY_PASS;
    e.touchdown = true;
  } else if (d[ST_FGM] > 0) {
    const int yds = static_cast<int>(lroundf(d[ST_FGM_YDS] / d[ST_FGM]));
    if (yds > 0) {
      snprintf(out, size, "%d YD FIELD GOAL", yds);
    } else {
      snprintf(out, size, "FIELD GOAL");
    }
    e.play = PLAY_KICK;
  } else if (d[ST_INT] > 0) {
    snprintf(out, size, "INTERCEPTION");
    e.play = PLAY_DEFENSE;
  } else if (d[ST_FUM_REC] > 0) {
    snprintf(out, size, "FUMBLE RECOVERY");
    e.play = PLAY_DEFENSE;
  } else if (d[ST_SACK] > 0) {
    snprintf(out, size, "SACK");
    e.play = PLAY_DEFENSE;
  } else if (d[ST_PASS_INT] > 0) {
    snprintf(out, size, "INTERCEPTED");
  } else if (d[ST_FUM_LOST] > 0) {
    snprintf(out, size, "FUMBLE LOST");
  } else if (rush != 0 && abs(rush) >= abs(rec) && abs(rush) >= abs(pass)) {
    snprintf(out, size, "RUSH FOR %d YDS", rush);
    e.play = PLAY_RUSH;
  } else if (rec != 0 && abs(rec) >= abs(pass)) {
    snprintf(out, size, "CATCH FOR %d YDS", rec);
    e.play = PLAY_CATCH;
  } else if (pass != 0) {
    snprintf(out, size, "PASS FOR %d YDS", pass);
    e.play = PLAY_PASS;
  } else if (d[ST_XPM] > 0) {
    snprintf(out, size, "EXTRA POINT");
    e.play = PLAY_KICK;
  } else {
    snprintf(out, size, delta > 0 ? "POINTS UP" : "POINTS DOWN");
  }
}

// Caller holds g_lock.
static void queueEvent(const Event &e)
{
  if (g_event_count >= MAX_EVENTS) {
    return;
  }
  g_events[(g_event_head + g_event_count) % MAX_EVENTS] = e;
  ++g_event_count;
}

// Points roll: every tenth between roll_from and pts passes like a wheel, fast at first and
// slowing to a stop.
static uint32_t rollDuration(const Player &p)
{
  const long steps = labs(lroundf(p.pts * 10) - lroundf(p.roll_from * 10));
  return std::min<uint32_t>(ROLL_MAX_MS, ROLL_MIN_MS + steps * ROLL_STEP_MS);
}

// Position of the wheel at `now`, in tenths of a point (fractional between two values).
static float rollShown(const Player &p, uint32_t now)
{
  const float from = lroundf(p.roll_from * 10), to = lroundf(p.pts * 10);
  const float u = std::min(1.0f, static_cast<float>(now - p.roll_ms) / rollDuration(p));
  const float left = 1 - u;
  return from + (to - from) * (1 - left * left * left);   // cubic ease-out
}

static void fetchTask(void *)
{
  uint32_t last_state_ms = 0;
  bool have_state = false;
  for (;;) {
    if (WiFi.status() != WL_CONNECTED) {
      g_wifi.run();   // scans and joins the strongest known network; blocks up to 5 s
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
      float stats[ST_COUNT] = {};
      if (!fetchPlayer(roster[i].id, scoring, season, type, week, pts, state, stats)) {
        all_ok = false;
        continue;
      }
      xSemaphoreTake(g_lock, portMAX_DELAY);
      if (g_generation == generation) {
        Player &p = g_players[i];
        if (p.state == PTS_OK && state == PTS_OK && fabsf(p.pts - pts) > 0.001f) {
          p.changed_ms = millis();
          p.dropped = pts < p.pts;
          // The roll starts from what the panel shows now (mid-roll, or still waiting to roll).
          if (!p.rolling) {
            p.roll_from = p.pts;
          } else if (p.roll_ms) {
            p.roll_from = rollShown(p, millis()) / 10.0f;
          }
          p.rolling = true;
          p.roll_ms = 0;
          // Gains of EVENT_MIN_PTS or more celebrate; smaller gains only get the rainbow name.
          // Only losses of EVENT_MIN_PTS or more interrupt the scoreboard.
          if (pts - p.pts > 0.001f || p.pts - pts >= EVENT_MIN_PTS - 0.001f) {
            Event e = {};
            copyStr(e.id, sizeof(e.id), p.id);
            copyStr(e.label, sizeof(e.label), p.label);
            copyStr(e.pos, sizeof(e.pos), p.pos);
            e.delta = pts - p.pts;
            e.pts = pts;
            describeChange(p.stats, stats, e.delta, e);
            queueEvent(e);
          }
        }
        p.pts = pts;
        p.state = state;
        memcpy(p.stats, stats, sizeof(p.stats));
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
  doc["espn_status"] = g_espn_status;
  doc["json_err"] = g_json_err;
  doc["games"] = g_game_count;
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

// POST /api/test?kind=N: queue a made-up event for the first player, to see the screens. N indexes
// SAMPLES (the web page has one button each); without it, each press shows the next one.
static void handleTest()
{
  struct Sample {
    const char *action;
    float delta;
    Play play;
    bool touchdown;
  };
  static const Sample SAMPLES[] = {
      {"RUSH FOR 30 YDS", 3.0f, PLAY_RUSH, false},
      {"CATCH FOR 25 YDS", 3.5f, PLAY_CATCH, false},
      {"PASS FOR 40 YDS", 1.6f, PLAY_PASS, false},
      {"SACK", 1.0f, PLAY_DEFENSE, false},
      {"45 YD FIELD GOAL", 4.0f, PLAY_KICK, false},
      {"RUSHING TD", 6.0f, PLAY_RUSH, true},
      {"RECEIVING TD", 6.0f, PLAY_CATCH, true},
      {"TD PASS", 4.0f, PLAY_PASS, true},
      {"DEFENSIVE TD", 6.0f, PLAY_DEFENSE, true},
      {"POINTS UP", 1.0f, PLAY_OTHER, false},
      {"POINTS UP", 0.4f, PLAY_OTHER, false},   // under a full point: rainbow name only
  };
  constexpr int COUNT = sizeof(SAMPLES) / sizeof(SAMPLES[0]);
  static int next = 0;
  if (server.hasArg("kind")) {
    const int kind = server.arg("kind").toInt();
    if (kind < 0 || kind >= COUNT) {
      server.send(400, "text/plain", "kind out of range");
      return;
    }
    next = kind;
  }
  const Sample &sample = SAMPLES[next];
  next = (next + 1) % COUNT;

  Event e = {};
  xSemaphoreTake(g_lock, portMAX_DELAY);
  if (g_count > 0) {
    copyStr(e.id, sizeof(e.id), g_players[0].id);
    copyStr(e.label, sizeof(e.label), g_players[0].label);
    copyStr(e.pos, sizeof(e.pos), g_players[0].pos);
    e.pts = (g_players[0].state == PTS_OK ? g_players[0].pts : 0.0f) + sample.delta;
    // Roll the real points up (or down) to themselves by the sample's delta, to show the wheel.
    Player &p = g_players[0];
    if (p.state == PTS_OK) {
      p.roll_from = p.pts - sample.delta;
      p.rolling = true;
      p.roll_ms = 0;
      p.changed_ms = millis();
      p.dropped = sample.delta < 0;
    }
  } else {
    copyStr(e.label, sizeof(e.label), "Test");
    copyStr(e.pos, sizeof(e.pos), "RB");
    e.pts = sample.delta;
  }
  copyStr(e.action, sizeof(e.action), sample.action);
  e.delta = sample.delta;
  e.play = sample.play;
  e.touchdown = sample.touchdown;
  queueEvent(e);
  xSemaphoreGive(g_lock);
  server.send(200, "text/plain", "queued");
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

// Possession marker: a football, 5x3. Hollow brown outside the opponent's 20, solid red with a
// white lace inside it.
//   .###.      .###.
//   #...#      ##-##    (- = lace)
//   .###.      .###.
static void drawBall(int x, int y, bool red_zone)
{
  const uint16_t c = red_zone ? COLOR_RED_ZONE : COLOR_BALL;
  canvas->drawFastHLine(x + 1, y, 3, c);
  if (red_zone) {
    canvas->drawFastHLine(x, y + 1, 5, c);
    canvas->drawPixel(x + 2, y + 1, COLOR_LACE);
  } else {
    canvas->drawPixel(x, y + 1, c);
    canvas->drawPixel(x + 4, y + 1, c);
  }
  canvas->drawFastHLine(x + 1, y + 2, 3, c);
}

// State of a team's game: live, over, and possession. Caller holds g_lock.
static void teamPossession(const char *team, bool &live, bool &final, bool &has_ball, bool &red_zone)
{
  const Game *game = team[0] ? findGame(team) : nullptr;
  live = game && game->state == GAME_LIVE;
  final = game && game->state == GAME_FINAL;
  has_ball = live && game->poss[0] && !strcmp(game->poss, team);
  red_zone = has_ball && game->red_zone;
}

// Fully saturated colour for hue h (wraps at 1).
static uint16_t rainbow565(float h)
{
  h -= floorf(h);
  const float x = h * 6.0f;
  const int i = static_cast<int>(x);
  const uint8_t f = static_cast<uint8_t>((x - i) * 255);
  switch (i % 6) {
    case 0: return rgb565(255, f, 0);
    case 1: return rgb565(255 - f, 255, 0);
    case 2: return rgb565(0, 255, f);
    case 3: return rgb565(0, 255 - f, 255);
    case 4: return rgb565(f, 0, 255);
    default: return rgb565(255, 0, 255 - f);
  }
}

static uint32_t rainbowMs(const char *label)
{
  return strlen(label) * RAINBOW_STEP_MS + RAINBOW_HOLD_MS + RAINBOW_FADE_MS + SHINE_TAIL_MS;
}

// Mix of two colours, f = 0 (a) to 255 (b).
static uint16_t blend565(uint16_t a, uint16_t b, int f)
{
  const int r = ((a >> 11) * (255 - f) + (b >> 11) * f) / 255;
  const int g = (((a >> 5) & 0x3F) * (255 - f) + ((b >> 5) & 0x3F) * f) / 255;
  const int bl = ((a & 0x1F) * (255 - f) + (b & 0x1F) * f) / 255;
  return (r << 11) | (g << 5) | bl;
}

// One Font4x6 character with its top at y + dy, only the rows from clip_top to clip_bottom.
static void drawGlyphClipped(char ch, int x, int y, int dy, int clip_top, int clip_bottom, uint16_t color)
{
  const GFXfont &font = Font4x6;
  if (ch < font.first || ch > font.last) {
    return;
  }
  const GFXglyph &g = font.glyph[ch - font.first];
  const uint8_t *bits = font.bitmap + g.bitmapOffset;
  int bit = 0;
  for (int row = 0; row < g.height; ++row) {
    const int py = y + Font4x6_ASCENT + g.yOffset + row + dy;
    for (int col = 0; col < g.width; ++col, ++bit) {
      if ((bits[bit >> 3] & (0x80 >> (bit & 7))) && py >= clip_top && py <= clip_bottom) {
        canvas->drawPixel(x + g.xOffset + col, py, color);
      }
    }
  }
}

// Points mid-roll in the row at y: characters that differ between `from` and `to` are on a wheel,
// `off` pixels turned. Rolling up (dir > 0) the new digit comes in from below; down, from above.
static void drawRoll(const char *from, const char *to, int dir, int off, int y, uint16_t color)
{
  // Right-align both, padded to the same length.
  char a[8], b[8];
  const int len = std::max(strlen(from), strlen(to));
  snprintf(a, sizeof(a), "%*s", len, from);
  snprintf(b, sizeof(b), "%*s", len, to);
  const int x0 = PANEL_W - inkWidth(a[0] == ' ' ? b : a, &Font4x6);
  const int advance = Font4x6.glyph[0].xAdvance;
  const int top = y, bottom = y + ROW_H - 2;   // the 5 letter rows
  for (int i = 0; i < len; ++i) {
    const int x = x0 + i * advance;
    if (a[i] == b[i]) {
      drawGlyphClipped(a[i], x, y, 0, top, bottom, color);
    } else {
      drawGlyphClipped(a[i], x, y, -dir * off, top, bottom, color);
      drawGlyphClipped(b[i], x, y, dir * (ROW_H - off), top, bottom, color);
    }
  }
}

// One player row, ROW_H pixels tall starting at y.
static void drawPlayer(const Player &p, int y, bool stale, uint32_t now)
{
  // Shine, part 1: the whole row blinks inverted (gold bar, black text).
  const bool shine = g_shine_id && !strcmp(p.id, g_shine_id);
  const bool blink_on = shine && !g_shine_rainbow && g_shine_t < SHINE_BLINK_TOTAL_MS && (g_shine_t / SHINE_BLINK_MS) % 2 == 0;
  if (blink_on) {
    canvas->fillRect(0, y, PANEL_W, ROW_H - 1, COLOR_SHINE);
  }

  // Game state; players whose game is over are drawn dimmed.
  bool live, final, has_ball, red_zone;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  teamPossession(p.team, live, final, has_ball, red_zone);
  xSemaphoreGive(g_lock);
  const uint16_t text = final ? COLOR_DIM : COLOR_TEXT;

  canvas->fillRect(0, y, 2, ROW_H - 1, final ? blend565(positionColor(p.pos), 0, 150) : positionColor(p.pos));
  canvas->setFont(&Font4x6);
  const int base = y + Font4x6_ASCENT;

  // Points, right-aligned to the panel edge. While rolling, `pts` is the value leaving and `next`
  // the one coming in, `roll_off` pixels (0 to ROW_H) along.
  char pts[8], next[8] = "";
  int roll_off = 0, roll_dir = 0;
  uint16_t color = final ? COLOR_DIM : COLOR_POINTS;
  if (p.state == PTS_OK && p.rolling && p.roll_ms) {
    const float pos = rollShown(p, now);
    const int to = lroundf(p.pts * 10);
    roll_dir = to > pos ? 1 : -1;
    int shown = roll_dir > 0 ? static_cast<int>(floorf(pos)) : static_cast<int>(ceilf(pos));
    roll_off = static_cast<int>(lroundf(fabsf(pos - shown) * ROW_H));
    if (roll_off >= ROW_H) {
      shown += roll_dir;
      roll_off = 0;
    }
    formatPoints(pts, sizeof(pts), shown / 10.0f);
    if (roll_off) {
      formatPoints(next, sizeof(next), (shown + roll_dir) / 10.0f);
    }
  } else if (p.state == PTS_OK) {
    formatPoints(pts, sizeof(pts), p.pts);
  }
  if (p.state == PTS_OK) {
    if (p.changed_ms && now - p.changed_ms < FLASH_MS) {
      color = p.dropped ? COLOR_RED_ZONE : COLOR_FLASH;
    }
  } else {
    snprintf(pts, sizeof(pts), "-");   // no stats yet this week (or not fetched yet)
    color = COLOR_DIM;
  }
  if (stale && p.state != PTS_UNKNOWN) {
    color = COLOR_DIM;
  }
  if (blink_on) {
    color = 0;
  }
  const int pts_x = PANEL_W - std::max(inkWidth(pts, &Font4x6), inkWidth(next, &Font4x6));
  if (roll_off) {
    drawRoll(pts, next, roll_dir, roll_off, y, color);
  } else {
    canvas->setTextColor(color);
    canvas->setCursor(pts_x, base);
    canvas->print(pts);
  }

  // Football before the points while the team has the ball. Its space is kept for the whole game,
  // so the label doesn't change length every time possession changes.
  const int ball_x = pts_x - BALL_GAP - BALL_W;
  if (has_ball) {
    drawBall(ball_x, y + 1, red_zone);
  }

  // Label: shortened from the middle until it fits (abbrev.h).
  char label[sizeof(Player::label)];
  copyStr(label, sizeof(label), p.label);
  const int right = live ? ball_x : pts_x;
  abbreviate(label, right - LABEL_GAP - LABEL_X, [](const char *text) { return inkWidth(text, &Font4x6); });
  canvas->setCursor(LABEL_X, base);
  if (blink_on) {
    canvas->setTextColor(0);
    canvas->print(label);
  } else if (shine && g_shine_rainbow) {
    // Small gain: letters light one by one, each cycling through the rainbow, then fade to normal.
    const int t = static_cast<int>(g_shine_t);
    for (int i = 0; label[i]; ++i) {
      const int d = t - i * RAINBOW_STEP_MS;   // < 0: not lit yet
      uint16_t c = COLOR_SHINE_DIM;
      if (d >= RAINBOW_HOLD_MS + RAINBOW_FADE_MS) {
        c = text;
      } else if (d >= 0) {
        const uint16_t hue = rainbow565(i * 0.09f + static_cast<float>(t) / RAINBOW_CYCLE_MS);
        c = d < RAINBOW_HOLD_MS ? hue : blend565(hue, text, (d - RAINBOW_HOLD_MS) * 255 / RAINBOW_FADE_MS);
      }
      canvas->setTextColor(c);
      canvas->print(label[i]);
    }
  } else if (shine && g_shine_t < SHINE_BLINK_TOTAL_MS) {
    canvas->setTextColor(COLOR_SHINE);   // blink off phase: bright text on black
    canvas->print(label);
  } else if (shine) {
    // Shine, part 2: a wave runs left to right. Each letter waits dimmed, brightens as the wave
    // reaches it, then fades back to the normal text colour.
    const int wave_t = static_cast<int>(g_shine_t) - SHINE_BLINK_TOTAL_MS;
    for (int i = 0; label[i]; ++i) {
      const int d = wave_t - SHINE_HALF_MS - i * SHINE_STEP_MS;   // < 0: wave not here yet
      uint16_t c = d >= SHINE_HALF_MS ? text : COLOR_SHINE_DIM;
      if (d > -SHINE_HALF_MS && d <= 0) {
        c = blend565(COLOR_SHINE_DIM, COLOR_SHINE, 255 + d * 255 / SHINE_HALF_MS);
      } else if (d > 0 && d < SHINE_HALF_MS) {
        c = blend565(COLOR_SHINE, text, d * 255 / SHINE_HALF_MS);
      }
      canvas->setTextColor(c);
      canvas->print(label[i]);
    }
  } else {
    canvas->setTextColor(text);
    canvas->print(label);
  }
}

// Games in the order the total line rotates through them: live, then final, then not started.
// Caller holds g_lock.
static int orderedGames(const Game **out)
{
  int n = 0;
  for (GameState want : {GAME_LIVE, GAME_FINAL, GAME_PRE}) {
    for (int i = 0; i < g_game_count; ++i) {
      if (g_games[i].state == want) {
        out[n++] = &g_games[i];
      }
    }
  }
  return n;
}

// Left part of the total line: one NFL game, away team first, in the widest form that fits.
static void drawGameScore(const Game &game, int max_w, int base)
{
  char forms[4][24];
  if (game.state == GAME_PRE) {
    snprintf(forms[0], sizeof(forms[0]), "%s - %s %s", game.away, game.home, game.kickoff);
    snprintf(forms[1], sizeof(forms[1]), "%s - %s", game.away, game.home);
    copyStr(forms[2], sizeof(forms[2]), forms[0]);
    copyStr(forms[3], sizeof(forms[3]), forms[1]);
  } else {
    snprintf(forms[0], sizeof(forms[0]), "%s %d %s %d", game.away, game.away_score, game.home, game.home_score);
    snprintf(forms[1], sizeof(forms[1]), "%s%d %s%d", game.away, game.away_score, game.home, game.home_score);
    copyStr(forms[2], sizeof(forms[2]), forms[0]);
    copyStr(forms[3], sizeof(forms[3]), forms[1]);
  }
  // Forms 0-1 in the row font, 2-3 in the narrower TomThumb (same 5 px height).
  const GFXfont *fonts[4] = {&Font4x6, &Font4x6, &TomThumb, &TomThumb};
  int pick = 3;
  for (int i = 0; i < 4; ++i) {
    canvas->setFont(fonts[i]);
    if (inkWidth(forms[i], fonts[i]) <= max_w) {
      pick = i;
      break;
    }
  }
  canvas->setFont(fonts[pick]);
  const uint16_t color = game.state == GAME_LIVE ? COLOR_TEXT : (game.state == GAME_FINAL ? COLOR_DIM : COLOR_INFO);
  canvas->setTextColor(color);
  canvas->setCursor(0, base);
  canvas->print(forms[pick]);
}

// Divider, then the total line: rotating NFL scores on the left, total points on the right.
static void drawTotalLine(const Player *rows, int count, uint32_t now)
{
  for (int x = 0; x < PANEL_W; ++x) {
    canvas->drawPixel(x, DIVIDER_Y, (x & 1) ? COLOR_DIVIDER : 0);
  }
  float total = 0;
  bool any = false;
  for (int i = 0; i < count; ++i) {
    if (rows[i].state == PTS_OK) {
      total += rows[i].pts;
      any = true;
    }
  }
  char text[12];
  snprintf(text, sizeof(text), any ? "%.1f" : "-", total);
  const int base = TOTAL_Y + Font4x6_ASCENT;
  canvas->setFont(&Font4x6);
  const int total_x = PANEL_W - inkWidth(text, &Font4x6);
  canvas->setTextColor(any ? COLOR_TOTAL : COLOR_DIM);
  canvas->setCursor(total_x, base);
  canvas->print(text);

  const Game *games[MAX_GAMES];
  xSemaphoreTake(g_lock, portMAX_DELAY);
  const int n = orderedGames(games);
  if (n > 0) {
    const Game game = *games[(now / SCORES_ROTATE_MS) % n];
    xSemaphoreGive(g_lock);
    drawGameScore(game, total_x - 3, base);
  } else {
    xSemaphoreGive(g_lock);
  }
}

static void drawScreen(uint32_t now)
{
  canvas->fillScreen(0);
  canvas->setFont(nullptr);
  canvas->setTextSize(1);

  if (!wifiConfigured()) {
    drawStatus("SET WIFI", COLOR_WARN, "secrets.h", COLOR_DIM);
    return;
  }
  if (WiFi.status() != WL_CONNECTED) {
    drawStatus("WIFI..", COLOR_DIM, nullptr, 0);
    return;
  }

  Player rows[MAX_PLAYERS];
  int count;
  xSemaphoreTake(g_lock, portMAX_DELAY);
  // Rolls start once the scoreboard is on screen, and end when the wheel stops.
  for (int i = 0; i < g_count; ++i) {
    Player &p = g_players[i];
    if (p.rolling && !p.roll_ms) {
      p.roll_ms = now ? now : 1;
    } else if (p.rolling && now - p.roll_ms >= rollDuration(p)) {
      p.rolling = false;
    }
  }
  memcpy(rows, g_players, sizeof(rows));
  count = g_count;
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

  const bool stale = g_last_ok_ms == 0 || now - g_last_ok_ms > STALE_MS;
  for (int i = 0; i < count; ++i) {
    drawPlayer(rows[i], i * ROW_H, stale, now);
  }
  drawTotalLine(rows, count, now);
  canvas->setFont(nullptr);

  // Fetch problem: one red pixel in the bottom-right corner.
  if (g_status != 0 && g_status != HTTP_CODE_OK) {
    canvas->drawPixel(PANEL_W - 1, PANEL_H - 1, rgb565(255, 0, 0));
  }
}

// Draws `text` centred at baseline y in `font` at `size`, shrinking to the 4x6 font and then
// shortening it if it is too wide.
static void drawCenteredFit(const char *text, int base, const GFXfont *font, uint8_t size, uint16_t color)
{
  char buf[32];
  copyStr(buf, sizeof(buf), text);
  canvas->setFont(font);
  canvas->setTextSize(size);
  if (inkWidth(buf, font) > PANEL_W) {
    font = &Font5x7;
    size = 1;
    canvas->setFont(font);
    canvas->setTextSize(size);
    if (inkWidth(buf, font) > PANEL_W) {
      font = &Font4x6;
      canvas->setFont(font);
      abbreviate(buf, PANEL_W, [font](const char *s) { return inkWidth(s, font); });
    }
  }
  const int w = inkWidth(buf, font);
  canvas->setTextColor(color);
  canvas->setCursor((PANEL_W - w) / 2, base);
  canvas->print(buf);
  canvas->setTextSize(1);
}

// Full-screen update: name, what happened, points gained, new total for the player.
static void drawUpdate(const Event &e, uint32_t t)
{
  canvas->fillScreen(0);
  const uint16_t theme = positionColor(e.pos);
  // Position-coloured frame that fades in.
  if (t > 100) {
    canvas->drawRect(0, 0, PANEL_W, PANEL_H, theme);
  }

  char name[16];
  copyStr(name, sizeof(name), e.label);
  for (char *c = name; *c; ++c) {
    *c = static_cast<char>(toupper(static_cast<unsigned char>(*c)));
  }
  drawCenteredFit(name, 4 + 2 * Font5x7_ASCENT, &Font5x7, 2, COLOR_TEXT);

  // The action, split over two lines when it has several words and doesn't fit on one.
  canvas->setFont(&Font5x7);
  canvas->setTextSize(1);
  const char *space = strchr(e.action, ' ');
  if (inkWidth(e.action, &Font5x7) <= PANEL_W - 2 || !space) {
    drawCenteredFit(e.action, 26 + Font5x7_ASCENT, &Font5x7, 1, theme);
  } else {
    // Break at the space nearest the middle.
    const char *best = space;
    const int len = static_cast<int>(strlen(e.action));
    for (const char *s = space; s; s = strchr(s + 1, ' ')) {
      if (abs((s - e.action) - len / 2) < abs((best - e.action) - len / 2)) {
        best = s;
      }
    }
    char first[24];
    snprintf(first, sizeof(first), "%.*s", static_cast<int>(best - e.action), e.action);
    drawCenteredFit(first, 22 + Font5x7_ASCENT, &Font5x7, 1, theme);
    drawCenteredFit(best + 1, 30 + Font5x7_ASCENT, &Font5x7, 1, theme);
  }

  char delta[12];
  snprintf(delta, sizeof(delta), "%+.1f", e.delta);
  drawCenteredFit(delta, 40 + 2 * Font5x7_ASCENT, &Font5x7, 2, e.delta >= 0 ? COLOR_FLASH : COLOR_RED_ZONE);

  char now_pts[16];
  snprintf(now_pts, sizeof(now_pts), "NOW %.1f", e.pts);
  drawCenteredFit(now_pts, 56 + Font4x6_ASCENT, &Font4x6, 1, COLOR_POINTS);
  canvas->setFont(nullptr);
}

// Which screen to draw: the queued events take over the scoreboard until they are all shown.
// Returns false when the scoreboard should be drawn.
static bool drawEventScreens(uint32_t now)
{
  if (!g_showing) {
    xSemaphoreTake(g_lock, portMAX_DELAY);
    if (g_event_count > 0) {
      g_current = g_events[g_event_head];
      g_event_head = (g_event_head + 1) % MAX_EVENTS;
      --g_event_count;
      g_showing = true;
      g_show_ms = now;
      ++g_event_seq;
    }
    xSemaphoreGive(g_lock);
    if (!g_showing) {
      return false;
    }
  }
  uint32_t t = now - g_show_ms;
  // A gain under a full point only gets the rainbow name on the scoreboard.
  const bool small = g_current.delta > 0 && g_current.delta < EVENT_MIN_PTS - 0.001f;
  // First the player's name shines on the scoreboard.
  const uint32_t shine = small ? rainbowMs(g_current.label)
                               : SHINE_BLINK_TOTAL_MS + strlen(g_current.label) * SHINE_STEP_MS +
                                     2 * SHINE_HALF_MS + SHINE_TAIL_MS;
  if (t < shine) {
    g_shine_id = g_current.id;
    g_shine_t = t;
    g_shine_rainbow = small;
    drawScreen(now);
    g_shine_id = nullptr;
    g_shine_rainbow = false;
    return true;
  }
  if (small) {
    g_showing = false;
    return drawEventScreens(now);
  }
  t -= shine;
  // Losses skip the party.
  const uint32_t celebrate = g_current.delta > 0 ? celebrationMs(g_current.touchdown) : 0;
  if (t < celebrate) {
    drawCelebration(*canvas, t, g_event_seq, positionColor(g_current.pos), g_current.play, g_current.touchdown);
    return true;
  }
  if (t < celebrate + UPDATE_MS) {
    drawUpdate(g_current, t - celebrate);
    return true;
  }
  g_showing = false;
  return drawEventScreens(now);   // next queued event, or back to the scoreboard
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
    if (!wifiConfigured() || WiFi.status() != WL_CONNECTED) {
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
    server.on("/api/test", HTTP_POST, handleTest);
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

  if (wifiConfigured()) {
    WiFi.mode(WIFI_STA);
    WiFi.setHostname(HOSTNAME);
    WiFi.setAutoReconnect(true);
    // No modem sleep: with it, every reply waits for the access point's next beacon, which made
    // OTA (1 KB per round trip) crawl.
    WiFi.setSleep(false);
    for (const WifiNetwork &n : WIFI_LIST) {
      if (n.ssid && n.ssid[0]) {
        g_wifi.addAP(n.ssid, n.password);
      }
    }
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
  if (!g_startup_running && !drawEventScreens(now)) {
    drawScreen(now);
  }
  present();

  const uint32_t spent = millis() - now;
  delay(spent < FRAME_MS ? FRAME_MS - spent : 1);
}
