// Renders the panel's animations on a desktop, from the firmware's own drawing code.
//
// src/main.cpp is compiled here with the stand-in headers in tools/sim/include (no WiFi, no
// panel), and its static functions are driven directly: a made-up roster and made-up NFL games,
// then the same test events the web page's buttons send (handleTest), a loss, the idle scoreboard
// and the startup animation. Every 50 ms frame (the firmware's FRAME_MS) of each scene is written
// as raw RGB565 to <outdir>/<scene>.rgb565; tools/sim/make_gifs.py turns them into GIFs.
//
// Build and run with tools/sim/make_gifs.py.

#include "../../src/main.cpp"

#include <string>
#include <vector>

HardwareSerial Serial;
WiFiClass WiFi;
MDNSClass MDNS;
ArduinoOTAClass ArduinoOTA;

static uint32_t g_sim_ms = 10000;
uint32_t millis() { return g_sim_ms; }

struct SamplePlayer {
  const char *id, *label, *pos, *team;
  float pts;
};

// The lineup from the README's example, with made-up points.
static const SamplePlayer LINEUP[] = {
    {"12545", "Shough", "QB", "NO", 19.8f},   {"8138", "Cook", "RB", "BUF", 18.3f},
    {"9221", "Gibbs", "RB", "DET", 25.1f},    {"9488", "Smith-Njigba", "WR", "SEA", 22.8f},
    {"13346", "Boston", "WR", "CLE", 11.3f},  {"9487", "Washington", "WR", "JAX", 16.3f},
    {"5001", "Schultz", "TE", "HOU", 12.1f},  {"5844", "Hockenson", "TE", "MIN", 8.7f},
    {"TEN", "Titans", "DEF", "TEN", 6.8f},
};

static Game makeGame(const char *away, int as, const char *home, int hs, GameState state, const char *poss = "",
                     bool red_zone = false, const char *kickoff = "")
{
  Game g = {};
  copyStr(g.away, sizeof(g.away), away);
  copyStr(g.home, sizeof(g.home), home);
  g.away_score = as;
  g.home_score = hs;
  g.state = state;
  g.period = state == GAME_LIVE ? 3 : 4;
  copyStr(g.clock, sizeof(g.clock), "8:14");
  copyStr(g.kickoff, sizeof(g.kickoff), kickoff);
  copyStr(g.poss, sizeof(g.poss), poss);
  g.red_zone = red_zone;
  return g;
}

static void resetBoard()
{
  g_lock = xSemaphoreCreateMutex();
  if (!canvas) {
    canvas = new GFXcanvas16(PANEL_W, PANEL_H);
    canvas->setTextWrap(false);
  }
  g_count = 0;
  for (const SamplePlayer &s : LINEUP) {
    Player p = {};
    copyStr(p.id, sizeof(p.id), s.id);
    copyStr(p.label, sizeof(p.label), s.label);
    copyStr(p.pos, sizeof(p.pos), s.pos);
    copyStr(p.team, sizeof(p.team), s.team);
    p.pts = s.pts;
    p.state = PTS_OK;
    g_players[g_count++] = p;
  }
  const Game games[] = {
      makeGame("NO", 17, "ATL", 10, GAME_LIVE, "NO", true),   makeGame("LAC", 7, "BUF", 21, GAME_LIVE, "LAC"),
      makeGame("NYJ", 3, "DET", 24, GAME_LIVE, "DET"),        makeGame("SEA", 14, "WAS", 14, GAME_LIVE, "WAS"),
      makeGame("NE", 10, "JAX", 13, GAME_LIVE, "JAX"),        makeGame("HOU", 20, "IND", 17, GAME_FINAL),
      makeGame("CLE", 13, "CAR", 16, GAME_FINAL),             makeGame("MIN", 0, "TB", 0, GAME_PRE, "", false, "4:05P"),
      makeGame("TEN", 0, "NYG", 0, GAME_PRE, "", false, "4:25P"),
  };
  g_game_count = 0;
  for (const Game &g : games) {
    g_games[g_game_count++] = g;
  }
  g_have_games = true;
  g_event_head = g_event_count = 0;
  g_showing = false;
  g_status = HTTP_CODE_OK;
  g_last_ok_ms = g_sim_ms;
}

static bool busy()
{
  if (g_showing || g_event_count) {
    return true;
  }
  for (int i = 0; i < g_count; ++i) {
    if (g_players[i].rolling) {
      return true;
    }
  }
  return false;
}

// One 50 ms frame of loop()'s screen choice: queued events first, otherwise the scoreboard.
static void frame(FILE *f)
{
  const uint32_t now = g_sim_ms;
  g_last_ok_ms = now;   // keep the data fresh (no grey)
  if (!drawEventScreens(now)) {
    drawScreen(now);
  }
  fwrite(canvas->getBuffer(), sizeof(uint16_t), PANEL_W * PANEL_H, f);
  g_sim_ms += FRAME_MS;
}

static FILE *openScene(const std::string &dir, const char *name)
{
  const std::string path = dir + "/" + name + ".rgb565";
  FILE *f = fopen(path.c_str(), "wb");
  if (!f) {
    perror(path.c_str());
    exit(1);
  }
  return f;
}

// Scoreboard for `lead_ms`, then `trigger`, then frames until everything settles, plus `tail_ms`.
template <typename Trigger>
static void scene(const std::string &dir, const char *name, uint32_t lead_ms, Trigger trigger, uint32_t tail_ms = 1500)
{
  resetBoard();
  FILE *f = openScene(dir, name);
  for (uint32_t t = 0; t < lead_ms; t += FRAME_MS) {
    frame(f);
  }
  trigger();
  for (int guard = 0; busy() && guard < 2000; ++guard) {
    frame(f);
  }
  for (uint32_t t = 0; t < tail_ms; t += FRAME_MS) {
    frame(f);
  }
  fclose(f);
  printf("%s\n", name);
}

// The web page's test button for sample `kind`, with `who` (a LINEUP index) as the first player.
static void testEvent(int kind, int who)
{
  std::swap(g_players[0], g_players[who]);
  server.args["kind"] = std::to_string(kind);
  handleTest();
}

int main(int argc, char **argv)
{
  const std::string dir = argc > 1 ? argv[1] : ".";

  // Startup animation (off by default in the firmware: STARTUP_ANIMATION 0).
  {
    resetBoard();
    FILE *f = openScene(dir, "startup");
    for (uint32_t t = 0; drawStartup(*canvas, t); t += FRAME_MS) {
      fwrite(canvas->getBuffer(), sizeof(uint16_t), PANEL_W * PANEL_H, f);
    }
    fclose(f);
    printf("startup\n");
  }

  // Idle scoreboard: the total line cycles through the games (live, final, upcoming).
  scene(dir, "scoreboard", SCORES_ROTATE_MS * 9, [] {}, 0);

  // One scene per test button, each with a player of the matching position.
  struct Pick {
    const char *name;
    int kind, who;
  };
  const Pick picks[] = {
      {"rush", 0, 2},         {"catch", 1, 3},       {"pass", 2, 0},        {"sack", 3, 8},
      {"field-goal", 4, 8},   {"rushing-td", 5, 2},  {"receiving-td", 6, 5}, {"td-pass", 7, 0},
      {"defensive-td", 8, 8}, {"points-up", 9, 6},   {"small-gain", 10, 1},
  };
  for (const Pick &p : picks) {
    if (!strcmp(p.name, "field-goal")) {
      // A kicker for the field goal: turn the defense slot into one.
      scene(dir, p.name, 1000, [&] {
        copyStr(g_players[8].label, sizeof(g_players[8].label), "Aubrey");
        copyStr(g_players[8].pos, sizeof(g_players[8].pos), "K");
        copyStr(g_players[8].team, sizeof(g_players[8].team), "DAL");
        testEvent(p.kind, p.who);
      });
    } else {
      scene(dir, p.name, 1000, [&] { testEvent(p.kind, p.who); });
    }
  }

  // A loss: no celebration, a red update screen, the points roll down in red.
  scene(dir, "loss", 1000, [] {
    Player &p = g_players[4];
    Event e = {};
    copyStr(e.id, sizeof(e.id), p.id);
    copyStr(e.label, sizeof(e.label), p.label);
    copyStr(e.pos, sizeof(e.pos), p.pos);
    copyStr(e.action, sizeof(e.action), "FUMBLE LOST");
    e.delta = -2.0f;
    e.pts = p.pts;
    e.play = PLAY_OTHER;
    p.roll_from = p.pts + 2.0f;
    p.rolling = true;
    p.roll_ms = 0;
    p.changed_ms = millis();
    p.dropped = true;
    queueEvent(e);
  });
  return 0;
}
