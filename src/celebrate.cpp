#include "celebrate.h"

#include <cmath>
#include <cstring>
#include <utility>
#include "fonts/Font5x7.h"

namespace {

constexpr int W = 64;
constexpr int H = 64;
constexpr float TWO_PI_F = 6.2831853f;
constexpr int SPARKS = 36;          // per burst
constexpr float ROCKET_S = 0.35f;   // rise time before a burst
constexpr float BURST_S = 1.0f;     // spark life after a burst
constexpr int GROUND_Y = 44;        // top of the grass in the side views
constexpr int FEET_Y = 58;          // where the players stand in the side views

inline int clamp255(float v)
{
  return v < 0 ? 0 : (v > 255 ? 255 : static_cast<int>(v));
}

inline uint16_t rgb565(int r, int g, int b)
{
  return ((clamp255(r) & 0xF8) << 8) | ((clamp255(g) & 0xFC) << 3) | (clamp255(b) >> 3);
}

inline void unpack565(uint16_t c, int &r, int &g, int &b)
{
  r = (c >> 8) & 0xF8;
  g = (c >> 3) & 0xFC;
  b = (c << 3) & 0xF8;
}

inline uint16_t scale565(uint16_t c, float k)
{
  int r, g, b;
  unpack565(c, r, g, b);
  return rgb565(static_cast<int>(r * k), static_cast<int>(g * k), static_cast<int>(b * k));
}

const uint16_t WHITE = rgb565(255, 255, 255);
const uint16_t BALL = rgb565(200, 105, 35);
const uint16_t PANTS = rgb565(210, 210, 210);
const uint16_t GRASS = rgb565(20, 90, 30);
const uint16_t GRASS_DARK = rgb565(14, 70, 22);
const uint16_t LINE = rgb565(150, 170, 150);
const uint16_t POST = rgb565(255, 215, 0);
const uint16_t OPPONENT = rgb565(120, 120, 130);

uint16_t hsv(float h, float v)
{
  h -= floorf(h);
  const float x = h * 6.0f;
  const int i = static_cast<int>(x);
  const float f = x - i;
  float r = 0, g = 0, b = 0;
  switch (i % 6) {
    case 0: r = 1; g = f; break;
    case 1: r = 1 - f; g = 1; break;
    case 2: g = 1; b = f; break;
    case 3: g = 1 - f; b = 1; break;
    case 4: r = f; b = 1; break;
    default: r = 1; b = 1 - f; break;
  }
  return rgb565(static_cast<int>(r * v * 255), static_cast<int>(g * v * 255), static_cast<int>(b * v * 255));
}

inline uint32_t hash(uint32_t x)
{
  x ^= x >> 16;
  x *= 0x7FEB352D;
  x ^= x >> 15;
  x *= 0x846CA68B;
  x ^= x >> 16;
  return x;
}

inline float hashf(uint32_t x) { return (hash(x) & 0xFFFF) / 65535.0f; }

inline float clamp01(float v) { return v < 0 ? 0 : (v > 1 ? 1 : v); }
inline float easeOut(float s) { s = clamp01(s); return 1 - (1 - s) * (1 - s); }
inline int ri(float v) { return static_cast<int>(lroundf(v)); }

// Adds colour to a pixel (sparks brighten whatever is under them).
void addPixel(GFXcanvas16 &g, int x, int y, uint16_t c, float k)
{
  if (x < 0 || x >= W || y < 0 || y >= H || k <= 0) {
    return;
  }
  uint16_t *buf = g.getBuffer();
  int r0, g0, b0, r1, g1, b1;
  unpack565(buf[y * W + x], r0, g0, b0);
  unpack565(c, r1, g1, b1);
  buf[y * W + x] = rgb565(r0 + static_cast<int>(r1 * k), g0 + static_cast<int>(g1 * k), b0 + static_cast<int>(b1 * k));
}

// Lightens the whole screen (flash at a burst, an impact or a catch).
void wash(GFXcanvas16 &g, float flash)
{
  if (flash <= 0) {
    return;
  }
  uint16_t *buf = g.getBuffer();
  const int add = static_cast<int>(flash * 90);
  for (int i = 0; i < W * H; ++i) {
    int r, gg, bb;
    unpack565(buf[i], r, gg, bb);
    buf[i] = rgb565(r + add, gg + add, bb + add);
  }
}

// Moves the whole frame by (dx, dy), filling with black (screen shake).
void shift(GFXcanvas16 &g, int dx, int dy)
{
  uint16_t *buf = g.getBuffer();
  if (dy > 0) {
    for (int y = H - 1; y >= 0; --y) {
      if (y - dy >= 0) memcpy(buf + y * W, buf + (y - dy) * W, W * 2);
      else memset(buf + y * W, 0, W * 2);
    }
  } else if (dy < 0) {
    for (int y = 0; y < H; ++y) {
      if (y - dy < H) memcpy(buf + y * W, buf + (y - dy) * W, W * 2);
      else memset(buf + y * W, 0, W * 2);
    }
  }
  if (dx > 0) {
    for (int y = 0; y < H; ++y) {
      memmove(buf + y * W + dx, buf + y * W, (W - dx) * 2);
      memset(buf + y * W, 0, dx * 2);
    }
  } else if (dx < 0) {
    for (int y = 0; y < H; ++y) {
      memmove(buf + y * W, buf + y * W - dx, (W + dx) * 2);
      memset(buf + y * W + W + dx, 0, -dx * 2);
    }
  }
}

// ---- Text -------------------------------------------------------------------------------

int textWidth(const char *text, int size)
{
  const int n = static_cast<int>(strlen(text));
  return n ? n * 5 * size - size : 0;   // 5 px cells, last column blank
}

void drawText(GFXcanvas16 &g, const char *text, int x, int base, int size, uint16_t c)
{
  g.setFont(&Font5x7);
  g.setTextSize(size);
  g.setTextColor(c);
  g.setCursor(x, base);
  g.print(text);
}

void drawCentered(GFXcanvas16 &g, const char *text, int base, int size, uint16_t c)
{
  drawText(g, text, (W - textWidth(text, size)) / 2, base, size, c);
}

// One colour per letter, the hues moving along the word.
void drawRainbow(GFXcanvas16 &g, const char *text, int x, int base, int size, float hue)
{
  char one[2] = {0, 0};
  for (int i = 0; text[i]; ++i) {
    one[0] = text[i];
    drawText(g, one, x + i * 5 * size, base, size, hsv(hue + i * 0.12f, 1));
  }
}

// ---- Sprites ----------------------------------------------------------------------------

// Football, about 5x3 at scale 1. `tumble` turns it end over end.
void drawBall(GFXcanvas16 &g, float x, float y, float scale, bool tumble)
{
  float rx = 2.5f * scale, ry = 1.5f * scale;
  if (tumble) {
    std::swap(rx, ry);
  }
  const int cx = ri(x), cy = ri(y);
  if (scale <= 1.2f) {
    if (tumble) {
      g.drawFastVLine(cx, cy - 2, 5, BALL);
      g.drawFastVLine(cx - 1, cy - 1, 3, BALL);
      g.drawFastVLine(cx + 1, cy - 1, 3, BALL);
      g.drawPixel(cx, cy, WHITE);
    } else {
      g.drawFastHLine(cx - 2, cy, 5, BALL);
      g.drawFastHLine(cx - 1, cy - 1, 3, BALL);
      g.drawFastHLine(cx - 1, cy + 1, 3, BALL);
      g.drawPixel(cx, cy, WHITE);
    }
    return;
  }
  // Filled ellipse (Adafruit GFX has none).
  for (int dy = -ri(ry); dy <= ri(ry); ++dy) {
    const float k = 1 - (dy * dy) / (ry * ry);
    if (k < 0) continue;
    const int half = ri(rx * sqrtf(k));
    g.drawFastHLine(cx - half, cy + dy, 2 * half + 1, BALL);
  }
  // Laces.
  if (tumble) {
    g.drawFastVLine(cx, cy - ri(ry * 0.5f), ri(ry) + 1, WHITE);
  } else {
    g.drawFastHLine(cx - ri(rx * 0.5f), cy, ri(rx) + 1, WHITE);
  }
}

// Side-view player, about 16 px tall, feet at (x, feet). `dir` +1 faces right. `phase` drives the
// stride; `arms_up` raises both arms; `ball` puts the football under the front arm.
void drawFigure(GFXcanvas16 &g, float xf, float feetf, float phase, uint16_t jersey, int dir, bool arms_up, bool ball)
{
  const int x = ri(xf), feet = ri(feetf);
  const int hip = feet - 6;
  const float a = sinf(phase) * 0.8f;
  // Legs: one swings forward while the other swings back.
  g.drawLine(x, hip, x + dir * ri(sinf(a) * 4), feet, PANTS);
  g.drawLine(x, hip, x - dir * ri(sinf(a) * 4), feet, PANTS);
  // Body, leaning into the run.
  g.fillRect(x - 1, hip - 5, 3, 5, jersey);
  g.drawPixel(x + dir * 2, hip - 4, jersey);
  // Helmet with a facemask.
  g.fillCircle(x + dir, hip - 8, 2, jersey);
  g.drawPixel(x + dir * 3, hip - 8, PANTS);
  // Arms.
  if (arms_up) {
    g.drawLine(x - 1, hip - 5, x - 3, hip - 11, jersey);
    g.drawLine(x + 1, hip - 5, x + 3, hip - 11, jersey);
  } else {
    g.drawLine(x, hip - 4, x - dir * ri(sinf(a) * 3), hip - 1, jersey);
  }
  if (ball) {
    g.drawFastHLine(x + dir * 2 - 1, hip - 3, 3, BALL);
    g.drawPixel(x + dir * 2, hip - 4, BALL);
  }
}

// Side-view field: grass with yard lines scrolling left by `scroll` px.
void drawSideField(GFXcanvas16 &g, float scroll)
{
  for (int y = GROUND_Y; y < H; ++y) {
    g.drawFastHLine(0, y, W, ((y - GROUND_Y) / 4) & 1 ? GRASS_DARK : GRASS);
  }
  const int off = static_cast<int>(scroll) % 20;
  for (int x = -off; x < W; x += 20) {
    // Lines lean a little, as if seen from the stands.
    g.drawLine(x + 4, GROUND_Y, x, H - 1, LINE);
  }
}

// ---- Fireworks (other plays, and behind the touchdown text) -----------------------------

// Returns the flash strength of the bursts at this moment.
float fireworks(GFXcanvas16 &g, float t, uint32_t seed, uint16_t theme, int bursts, float spacing)
{
  float flash = 0;
  for (int b = 0; b < bursts; ++b) {
    const uint32_t id = seed * 131 + b * 17;
    const float start = b * spacing;
    const float bx = 10 + hashf(id + 1) * 44;            // burst point
    const float by = 10 + hashf(id + 2) * 26;
    const float lt = t - start;
    if (lt < 0) {
      continue;
    }
    if (lt < ROCKET_S) {
      // Rocket climbing from the bottom edge, with a short fading trail.
      const float s = lt / ROCKET_S;
      const float y = H - 1 - (H - 1 - by) * (1 - (1 - s) * (1 - s));
      for (int k = 0; k < 4; ++k) {
        addPixel(g, static_cast<int>(bx), static_cast<int>(y) + k, rgb565(255, 220, 150), 1.0f - k * 0.25f);
      }
      continue;
    }
    const float age = lt - ROCKET_S;
    if (age > BURST_S) {
      continue;
    }
    if (age < 0.08f) {
      flash = fmaxf(flash, (1 - age / 0.08f) * 0.66f);
    }
    // Sparks: half in the position colour, half rainbow; they slow down, fall and fade.
    const float fade = 1 - age / BURST_S;
    for (int s = 0; s < SPARKS; ++s) {
      const float angle = (s + hashf(id * 7 + s)) / SPARKS * TWO_PI_F;
      const float speed = 18 + 16 * hashf(id * 11 + s);
      const float dist = speed * (1 - expf(-3 * age)) / 3;   // decelerating
      const float x = bx + cosf(angle) * dist;
      const float y = by + sinf(angle) * dist + 14 * age * age;
      const uint16_t c = (s & 1) ? theme : hsv(hashf(id * 13 + s), 1);
      addPixel(g, static_cast<int>(x), static_cast<int>(y), c, fade);
      // A dimmer trailing pixel toward the centre.
      addPixel(g, static_cast<int>(bx + cosf(angle) * dist * 0.8f), static_cast<int>(by + sinf(angle) * dist * 0.8f + 14 * age * age),
               c, fade * 0.4f);
    }
  }
  return flash;
}

// A ring of sparks thrown out from one point (catch, impact, ball exploding).
void sparkBurst(GFXcanvas16 &g, float cx, float cy, float age, uint32_t seed, uint16_t theme, int count, float life)
{
  if (age < 0 || age > life) {
    return;
  }
  const float fade = 1 - age / life;
  for (int s = 0; s < count; ++s) {
    const float angle = (s + hashf(seed + s)) / count * TWO_PI_F;
    const float speed = 20 + 20 * hashf(seed * 3 + s);
    const float dist = speed * (1 - expf(-3 * age)) / 3;
    const float x = cx + cosf(angle) * dist;
    const float y = cy + sinf(angle) * dist + 10 * age * age;
    addPixel(g, ri(x), ri(y), (s & 1) ? theme : hsv(hashf(seed * 5 + s), 1), fade);
  }
}

// Confetti falling from the top, only over empty pixels.
void confetti(GFXcanvas16 &g, float t, uint32_t seed, uint16_t theme)
{
  uint16_t *buf = g.getBuffer();
  for (int i = 0; i < 40; ++i) {
    const float x0 = hashf(seed + i * 3) * W;
    const float speed = 18 + 14 * hashf(seed + i * 5);
    const float y = -4 + fmodf(t * speed + hashf(seed + i * 7) * 70, 70);
    const int x = ri(x0 + 2 * sinf(t * 4 + i));
    const int yy = ri(y);
    if (x < 0 || x >= W || yy < 0 || yy >= H || buf[yy * W + x]) {
      continue;
    }
    buf[yy * W + x] = (i % 3 == 0) ? theme : hsv(hashf(seed + i), 1);
  }
}

// ---- Plays ------------------------------------------------------------------------------

// Rush: the ball carrier sprints down the field, hops a diving defender, and runs off the right edge.
void drawRush(GFXcanvas16 &g, float t, uint32_t seed, uint16_t theme)
{
  const float scroll = t * 70;
  drawSideField(g, scroll);

  // Speed streaks in the air behind the runner.
  for (int i = 0; i < 7; ++i) {
    const int y = 22 + ri(hashf(seed + i) * 32);
    const float len = 6 + 8 * hashf(seed + i * 9);
    const float x = W - fmodf(t * (110 + 40 * hashf(seed + i * 5)) + hashf(seed + i * 3) * 80, W + 30);
    g.drawFastHLine(ri(x), y, ri(len), scale565(WHITE, 0.35f));
  }

  // Runner: in from the left, holds the middle while the field streams past, then leaves.
  float x;
  if (t < 0.5f) {
    x = -6 + 30 * easeOut(t / 0.5f);
  } else if (t < 2.3f) {
    x = 24 + 2 * sinf(t * 3);
  } else {
    x = 24 + 2 * sinf(2.3f * 3) + (t - 2.3f) * (t - 2.3f) * 160;
  }
  // A grey defender comes the other way; the runner hops over his dive.
  const float dx = 96 - (t - 0.6f) * 90;
  float hop = 0;
  if (fabsf(dx - x) < 8) {
    hop = 5 * cosf((dx - x) / 8 * 1.5708f);
  }
  if (t > 0.6f && dx > -10) {
    const bool diving = dx - x < 10;
    if (diving) {
      // Laid out flat, arms reaching.
      g.fillRect(ri(dx) - 4, FEET_Y - 3, 9, 3, OPPONENT);
      g.fillCircle(ri(dx) - 6, FEET_Y - 2, 2, OPPONENT);
    } else {
      drawFigure(g, dx, FEET_Y, t * 16, OPPONENT, -1, false, false);
    }
  }
  // Dust kicked up behind the runner.
  for (int i = 0; i < 6; ++i) {
    const float age = fmodf(t + i * 0.1f, 0.6f);
    const float px = x - 3 - age * 30;
    const float py = FEET_Y - age * 8;
    addPixel(g, ri(px), ri(py), rgb565(160, 130, 90), 0.8f * (1 - age / 0.6f));
  }
  drawFigure(g, x, FEET_Y - hop, t * 18, theme, 1, false, true);

  // "RUSH!" drops in and bounces.
  if (t > 0.25f) {
    const float s = (t - 0.25f) / 0.45f;
    const float base = s < 1 ? -4 + 24 * s * s : 20 - 4 * fabsf(sinf((t - 0.7f) * 9)) * expf(-(t - 0.7f) * 4);
    drawCentered(g, "RUSH!", ri(base), 2, theme);
  }
}

// The quarterback throws a spiral across the screen; the receiver runs under it and catches it.
void drawPass(GFXcanvas16 &g, float t, uint32_t seed, uint16_t theme, bool catcher)
{
  drawSideField(g, 0);
  const float release = 0.45f, flight = 1.2f, caught = release + flight;
  const float x0 = 11, y0 = FEET_Y - 17;
  const float rcv_x = t < caught ? 66 - 14 * easeOut(t / caught) : 52 + (t > caught + 0.35f ? (t - caught - 0.35f) * (t - caught - 0.35f) * 90 : 0);
  const float x1 = 52, y1 = FEET_Y - 14;

  // QB: arm cocked back, then follows through.
  drawFigure(g, 10, FEET_Y, 0, catcher ? OPPONENT : theme, 1, false, false);
  const uint16_t qb = catcher ? OPPONENT : theme;
  if (t < release) {
    const float w = t / release;
    g.drawLine(10, FEET_Y - 10, ri(10 - 3 + w), FEET_Y - 15, qb);
    drawBall(g, 10 - 3 + w, FEET_Y - 16, 1, false);
  } else {
    g.drawLine(10, FEET_Y - 10, 13, FEET_Y - 13, qb);
  }

  // The spiral and its dotted trail.
  auto path = [&](float s, float &x, float &y) {
    x = x0 + (x1 - x0) * s;
    y = y0 + (y1 - y0) * s - 4 * 30 * s * (1 - s);
  };
  if (t >= release && t < caught) {
    const float s = (t - release) / flight;
    for (int k = 1; k <= 10; ++k) {
      const float sk = s - k * 0.035f;
      if (sk <= 0) break;
      float tx, ty;
      path(sk, tx, ty);
      addPixel(g, ri(tx), ri(ty), WHITE, 0.6f * (1 - k / 11.0f));
    }
    float bx, by;
    path(s, bx, by);
    drawBall(g, bx, by, 1, false);
    // Spiral: the lace pixel flickers.
    if (static_cast<int>(t * 30) & 1) {
      g.drawPixel(ri(bx) + 1, ri(by), WHITE);
    }
  }

  // Receiver: arms up as the ball arrives, then away with it.
  const bool reaching = t > caught - 0.4f && t < caught + 0.2f;
  drawFigure(g, rcv_x, FEET_Y, t * 16, catcher ? theme : OPPONENT, t < caught ? -1 : 1, reaching, t >= caught);

  // Catch: spark burst, flash, and the word.
  float flash = 0;
  if (t >= caught) {
    const float age = t - caught;
    sparkBurst(g, x1, y1, age, seed, theme, 28, 0.9f);
    flash = age < 0.1f ? 0.7f * (1 - age / 0.1f) : 0;
    const float pop = age < 0.2f ? age / 0.2f : 1;
    const char *word = catcher ? "CAUGHT IT!" : "COMPLETE!";
    drawCentered(g, word, ri(12 - 4 * (1 - pop)), 1, hsv(t * 0.8f, 1));
  }
  wash(g, flash);
}

// A hit: defender and ball carrier collide, the ball pops loose, then the D-fence chant.
void drawDefense(GFXcanvas16 &g, float t, uint32_t seed, uint16_t theme)
{
  drawSideField(g, 0);
  const float hit = 0.7f;
  const float cx = 33, cy = FEET_Y - 9;

  if (t < hit) {
    const float s = t / hit;
    drawFigure(g, 70 - 34 * s, FEET_Y, t * 16, OPPONENT, -1, false, true);
    drawFigure(g, -6 + 36 * s * s, FEET_Y, t * 18, theme, 1, false, false);
  } else {
    const float age = t - hit;
    // Carrier knocked flat.
    g.fillRect(34, FEET_Y - 3, 10, 3, OPPONENT);
    g.fillCircle(46, FEET_Y - 2, 2, OPPONENT);
    // Defender standing over him, arms up and bouncing.
    const float bounce = age > 0.4f ? fabsf(sinf(age * 7)) * 3 : 0;
    drawFigure(g, 28, FEET_Y - bounce, 0, theme, 1, age > 0.4f, false);
    // Loose ball popping up and bouncing away.
    const float vx = 22, vy = -55, grav = 140;
    float ba = age, by = cy + vy * ba + 0.5f * grav * ba * ba, bx = cx + vx * ba;
    const float land = -2 * vy / grav;   // back at cy
    if (ba > land) {
      const float b2 = ba - land;
      bx = cx + vx * land + vx * 0.6f * b2;
      by = cy + vy * 0.35f * b2 + 0.5f * grav * b2 * b2;   // smaller second bounce
      if (by > cy) by = cy;
    }
    drawBall(g, bx, by, 1, static_cast<int>(age * 12) & 1);
    // Star of impact rays and a shockwave ring.
    if (age < 0.5f) {
      const float f = 1 - age / 0.5f;
      for (int r = 0; r < 12; ++r) {
        const float a = r / 12.0f * TWO_PI_F + hashf(seed + r) * 0.3f;
        const float len = 4 + age * 40 * (0.6f + 0.4f * hashf(seed * 3 + r));
        for (float d = 3; d < len; d += 1) {
          addPixel(g, ri(cx + cosf(a) * d), ri(cy + sinf(a) * d), (r & 1) ? theme : WHITE, f * (1 - d / (len + 1)));
        }
      }
    }
    if (age < 0.6f) {
      const int rad = ri(age * 70);
      g.drawCircle(ri(cx), ri(cy), rad, scale565(theme, 1 - age / 0.6f));
    }
    // D + fence, flashing to the chant once the dust settles.
    if (age > 0.3f) {
      const float c = age - 0.3f;
      const bool beat = static_cast<int>(c / 0.35f) & 1;
      const float pop = c < 0.15f ? c / 0.15f : 1;
      const int top = 4 + ri(8 * (1 - pop));
      drawText(g, "D", 8, top + 18, 3, beat ? WHITE : theme);
      // Picket fence.
      const uint16_t fc = beat ? theme : WHITE;
      for (int p = 0; p < 6; ++p) {
        const int px = 28 + p * 5;
        g.fillRect(px, top + 4, 3, 16, fc);
        g.drawPixel(px + 1, top + 3, fc);
      }
      g.drawFastHLine(26, top + 8, 32, fc);
      g.drawFastHLine(26, top + 15, 32, fc);
    }
    // Screen shake right after the hit.
    if (age < 0.45f) {
      const float amp = 3 * (1 - age / 0.45f);
      const uint32_t f = static_cast<uint32_t>(age * 40);
      shift(g, ri((hashf(seed + f) * 2 - 1) * amp), ri((hashf(seed + f + 99) * 2 - 1) * amp));
    }
    if (age < 0.08f) {
      wash(g, 1 - age / 0.08f);
    }
  }
}

// Behind the kicker: the ball flies away through the uprights, then "GOOD!".
void drawKick(GFXcanvas16 &g, float t, uint32_t seed, uint16_t theme)
{
  // Sky and perspective field.
  const int horizon = 26;
  for (int y = 0; y < horizon; ++y) {
    g.drawFastHLine(0, y, W, rgb565(0, 0, 10 + y));
  }
  for (int y = horizon; y < H; ++y) {
    const float depth = (y - horizon) / static_cast<float>(H - horizon);
    const int band = static_cast<int>(sqrtf(depth) * 8);
    g.drawFastHLine(0, y, W, (band & 1) ? GRASS_DARK : GRASS);
  }
  g.drawLine(26, horizon, 0, 50, LINE);
  g.drawLine(38, horizon, 63, 50, LINE);

  // Uprights, lit when the kick is good.
  const float fly = 1.4f;
  const bool good = t > fly * 0.9f;
  const uint16_t post = good && (static_cast<int>(t * 8) & 1) ? WHITE : POST;
  auto posts = [&]() {
    g.drawFastVLine(32, 16, horizon - 16 + 1, post);
    g.drawFastHLine(23, 16, 19, post);
    g.drawFastVLine(23, 1, 16, post);
    g.drawFastVLine(41, 1, 16, post);
  };

  const float s = clamp01(t / fly);
  const float bx = 32 + 3 * sinf(s * 3.1416f) * (hashf(seed) - 0.5f);
  const float by = 60 - 54 * (1 - (1 - s) * (1 - s));
  const float scale = 3.2f - 2.4f * s;
  const bool tumble = static_cast<int>(t * 14) & 1;
  const bool behind = s > 0.85f;   // past the crossbar: drawn behind the posts
  if (t < fly) {
    if (behind) {
      drawBall(g, bx, by, scale, tumble);
      posts();
    } else {
      posts();
      drawBall(g, bx, by, scale, tumble);
    }
  } else {
    posts();
  }

  if (good) {
    const float c = t - fly * 0.9f;
    confetti(g, c, seed, theme);
    const float pop = c < 0.2f ? c / 0.2f : 1;
    drawRainbow(g, "GOOD!", (W - textWidth("GOOD!", 2)) / 2, ri(52 + 10 * (1 - pop)), 2, t * 0.7f);
    wash(g, c < 0.1f ? 0.6f * (1 - c / 0.1f) : 0);
  }
}

// ---- Touchdowns, one scene per kind ------------------------------------------------------

constexpr float PI_F = 3.1415927f;

// Side-view field seen from a camera at world x `cam`, with the end zone past world x `goal`
// (to the right when `dir` is +1, to the left when -1): striped in the theme colour, with the
// goal line and the goal post.
void drawFieldCam(GFXcanvas16 &g, float cam, float goal, int dir, uint16_t theme)
{
  const uint16_t zone_a = scale565(theme, 0.45f), zone_b = scale565(theme, 0.22f);
  uint16_t *buf = g.getBuffer();
  for (int y = GROUND_Y; y < H; ++y) {
    const uint16_t grass = ((y - GROUND_Y) / 4) & 1 ? GRASS_DARK : GRASS;
    const float lean = 4.0f * (H - 1 - y) / (H - 1 - GROUND_Y);   // same lean as the yard lines
    for (int x = 0; x < W; ++x) {
      const float wx = x + cam - lean;
      const bool zone = dir > 0 ? wx > goal : wx < goal;
      const int stripe = ((static_cast<int>(floorf(x + cam)) + y) % 8 + 8) % 8;
      buf[y * W + x] = zone ? (stripe < 4 ? zone_a : zone_b) : grass;
    }
  }
  for (float wx = ceilf(cam / 20) * 20; wx < cam + W + 4; wx += 20) {
    if (dir > 0 ? wx < goal : wx > goal) {
      const int sx = ri(wx - cam);
      g.drawLine(sx + 4, GROUND_Y, sx, H - 1, LINE);
    }
  }
  const int gx = ri(goal - cam);
  g.drawLine(gx + 4, GROUND_Y, gx, H - 1, WHITE);
  // Goal post at the back of the end zone.
  const int px = ri(goal + dir * 30 - cam);
  if (px > -4 && px < W + 4) {
    g.drawFastVLine(px, 30, GROUND_Y + 4 - 30, POST);
    g.drawFastHLine(px - 3, 30, 7, POST);
    g.drawFastVLine(px - 3, 12, 18, POST);
    g.drawFastVLine(px + 3, 12, 18, POST);
  }
}

// Player laid out horizontally (diving, or tackled), centre (x, y), head toward `dir`.
void drawDive(GFXcanvas16 &g, float xf, float yf, uint16_t jersey, int dir, bool ball)
{
  const int x = ri(xf), y = ri(yf);
  g.drawLine(x - dir * 4, y, x - dir * 9, y + 1, PANTS);
  g.drawLine(x - dir * 4, y - 1, x - dir * 8, y - 2, PANTS);
  g.fillRect(x - 4, y - 2, 9, 3, jersey);
  g.fillCircle(x + dir * 6, y - 1, 2, jersey);
  g.drawPixel(x + dir * 8, y - 1, PANTS);
  if (ball) {
    g.drawLine(x + dir * 4, y - 2, x + dir * 9, y - 4, jersey);
    drawBall(g, x + dir * 11, y - 4, 1, false);
  }
}

// Rushing TD: a sprint down the sideline, the end zone scrolls in, a dive over the goal line, a
// spike, and "TOUCHDOWN!" scrolling across the sky.
void drawRushTD(GFXcanvas16 &g, float t, uint32_t seed, uint16_t theme)
{
  const float cam = fminf(t, 1.5f) * 80;
  drawFieldCam(g, cam, 160, 1, theme);
  float flash = 0;
  if (t > 2.3f) {
    flash = fireworks(g, t - 2.3f, seed, theme, 4, 0.25f);
  }

  // Grey defender chasing, then diving short.
  const float dx = 4 + (t > 1.5f ? (fminf(t, 1.7f) - 1.5f) * 80 : 0);
  if (t < 1.7f) {
    drawFigure(g, dx, FEET_Y, t * 17, OPPONENT, 1, false, false);
  } else {
    drawDive(g, dx + 6, FEET_Y - 2, OPPONENT, 1, false);
  }

  // Runner.
  if (t < 1.625f) {
    const float x = t < 1.5f ? 20 : 20 + (t - 1.5f) * 80;
    drawFigure(g, x, FEET_Y, t * 18, theme, 1, false, true);
  } else if (t < 1.95f) {
    const float s = (t - 1.625f) / 0.325f;
    drawDive(g, 30 + 22 * s, FEET_Y - 3 - 7 * sinf(s * PI_F), theme, 1, true);
  } else if (t < 2.15f) {
    drawDive(g, 52, FEET_Y - 2, theme, 1, true);
  } else {
    const float bounce = t > 2.45f ? fabsf(sinf((t - 2.45f) * 8)) * 3 : 0;
    drawFigure(g, 50, FEET_Y - bounce, 0, theme, 1, t > 2.3f, false);
    if (t < 2.3f) {
      // Ball held high, then slammed down.
      g.drawLine(51, FEET_Y - 10, 53, FEET_Y - 16, theme);
      drawBall(g, 53, FEET_Y - 17, 1, false);
    } else {
      const float a = t - 2.3f;
      const float by = FEET_Y - 1 - 70 * a + 50 * a * a;
      drawBall(g, 55 + a * 6, fminf(by, FEET_Y - 1), 1, static_cast<int>(a * 14) & 1);
      if (a < 0.1f) {
        flash = fmaxf(flash, 0.6f * (1 - a / 0.1f));
      }
    }
  }

  if (t > 2.25f) {
    drawRainbow(g, "TOUCHDOWN!", ri(W - (t - 2.25f) * 120), 24, 2, t * 0.9f);
  }
  wash(g, flash);
}

// Passing TD: the quarterback (this player) launches a bomb that leaves the top of the screen
// and drops into the end zone. The ball's path stays behind as a rainbow, "DIME!".
void drawPassTD(GFXcanvas16 &g, float t, uint32_t seed, uint16_t theme)
{
  drawFieldCam(g, 0, 40, 1, theme);
  const float release = 0.4f, flight = 1.3f, caught = release + flight;
  const float x0 = 11, y0 = FEET_Y - 17, x1 = 52, y1 = FEET_Y - 14;
  auto path = [&](float s, float &x, float &y) {
    x = x0 + (x1 - x0) * s;
    y = y0 + (y1 - y0) * s - 4 * 48 * s * (1 - s);
  };

  float flash = 0;
  if (t > caught + 0.3f) {
    flash = fireworks(g, t - caught - 0.3f, seed, theme, 4, 0.3f) * 0.6f;
  }

  // The arc: a faint trail in flight, a shimmering rainbow once caught.
  const float s_now = t < release ? 0 : clamp01((t - release) / flight);
  for (float s = 0; s < s_now; s += 0.01f) {
    float x, y;
    path(s, x, y);
    if (t < caught) {
      if (s > s_now - 0.3f) addPixel(g, ri(x), ri(y), WHITE, 0.5f * (1 - (s_now - s) / 0.3f));
    } else {
      g.drawPixel(ri(x), ri(y), hsv(s * 1.5f - t * 0.8f, 0.9f));
    }
  }
  if (t >= release && t < caught) {
    float bx, by;
    path(s_now, bx, by);
    drawBall(g, bx, by, 1, false);
    if (static_cast<int>(t * 30) & 1) g.drawPixel(ri(bx) + 1, ri(by), WHITE);
  }

  // QB: arm back with the ball, follow-through, then arms up and jumping.
  const float jump = t > caught ? fabsf(sinf((t - caught) * 7)) * 4 : 0;
  drawFigure(g, 9, FEET_Y - jump, 0, theme, 1, t > caught, false);
  if (t < release) {
    g.drawLine(9, FEET_Y - 10, 6, FEET_Y - 15, theme);
    drawBall(g, 6, FEET_Y - 16, 1, false);
  } else if (t <= caught) {
    g.drawLine(9, FEET_Y - 10, 12, FEET_Y - 13, theme);
  }

  // Receiver running under it in the end zone.
  const float rx = t < caught ? 24 + 28 * easeOut(t / caught) : 52;
  drawFigure(g, rx, FEET_Y, t < caught ? t * 16 : 0, OPPONENT, 1, t > caught - 0.35f, t >= caught);

  if (t >= caught) {
    const float a = t - caught;
    sparkBurst(g, x1, y1, a, seed, theme, 28, 0.9f);
    flash = fmaxf(flash, a < 0.1f ? 0.7f * (1 - a / 0.1f) : 0);
    const float pop = a < 0.2f ? a / 0.2f : 1;
    drawRainbow(g, "DIME!", (W - textWidth("DIME!", 2)) / 2, ri(32 + 8 * (1 - pop)), 2, t);
  }
  wash(g, flash);
}

// The crowd behind the end zone: fans flicker faster as they get excited.
void drawStands(GFXcanvas16 &g, float t, uint32_t seed, float excitement)
{
  const uint32_t frame = static_cast<uint32_t>(t * (3 + 14 * excitement));
  for (int y = 0; y < 10; ++y) {
    for (int x = 0; x < W; ++x) {
      const uint32_t id = seed + x * 7 + y * 131;
      const bool moving = hashf(id) < excitement;
      if (hashf(id * 3 + (moving ? frame : 0)) < 0.55f) {
        g.drawPixel(x, y, hsv(hashf(id * 5), 0.35f + 0.35f * hashf(id * 9 + frame)));
      }
    }
  }
  g.fillRect(0, 10, W, 2, rgb565(70, 70, 80));
}

// Receiving TD: a toe-tap catch in the end zone, then a leap into the stands; the crowd goes
// wild, "SIX!".
void drawCatchTD(GFXcanvas16 &g, float t, uint32_t seed, uint16_t theme)
{
  drawFieldCam(g, 0, 4, 1, theme);
  const float caught = 1.2f;
  drawStands(g, t, seed, t < caught ? 0.15f : (t < 2.2f ? 0.6f : 1.0f));
  float flash = 0;

  // Throw coming in from off screen.
  if (t > 0.2f && t < caught) {
    const float s = (t - 0.2f) / (caught - 0.2f);
    const float bx = -8 + 44 * s;
    const float by = 20 + (FEET_Y - 14 - 20) * s - 4 * 26 * s * (1 - s);
    drawBall(g, bx, by, 1, false);
    addPixel(g, ri(bx - 3), ri(by + 1), WHITE, 0.4f);
  }

  float x, feet = FEET_Y;
  bool arms_up = false, ball = true;
  if (t < caught) {
    x = 10 + 26 * easeOut(t / caught);
    arms_up = t > caught - 0.35f;
    ball = false;
  } else if (t < 1.7f) {
    x = 36 + 14 * (t - caught) / 0.5f;
  } else if (t < 2.2f) {
    const float s = (t - 1.7f) / 0.5f;
    x = 50 + 4 * s;
    feet = FEET_Y - (FEET_Y - 20) * easeOut(s);
    arms_up = true;
  } else {
    x = 54;
    feet = 20 + sinf(t * 9);
    arms_up = true;
  }
  drawFigure(g, x, feet, t * 16, theme, 1, arms_up, ball && !arms_up);

  if (t >= caught) {
    const float a = t - caught;
    sparkBurst(g, 36, FEET_Y - 14, a, seed, theme, 24, 0.8f);
    flash = a < 0.1f ? 0.6f * (1 - a / 0.1f) : 0;
  }
  if (t > 2.2f) {
    const float a = t - 2.2f;
    confetti(g, a, seed, theme);
    const float pop = a < 0.2f ? a / 0.2f : 1;
    drawRainbow(g, "SIX!", 4, ri(38 - 6 * (1 - pop)), 2, t);
  }
  wash(g, flash);
}

// Defensive TD: the defender jumps a throw, turns, and returns it the length of the field into
// the end zone on the left, "TO THE HOUSE!".
void drawDefTD(GFXcanvas16 &g, float t, uint32_t seed, uint16_t theme)
{
  const float grab = 0.65f;
  const float cam = t < grab ? 0 : -fminf(t - grab, 1.5f) * 90;
  drawFieldCam(g, cam, -83, -1, theme);
  float flash = 0;
  if (t > 2.1f) {
    flash = fireworks(g, t - 2.1f, seed, theme, 4, 0.3f) * 0.6f;
  }

  // Grey quarterback throwing to the left; he is left behind as the camera follows the return.
  const float qx = 58 - cam;
  if (qx < W + 6) {
    drawFigure(g, qx, FEET_Y, 0, OPPONENT, -1, false, false);
    g.drawLine(ri(qx), FEET_Y - 10, ri(qx) + (t < 0.15f ? 3 : -3), FEET_Y - 14, OPPONENT);
  }
  if (t > 0.15f && t < grab) {
    const float s = (t - 0.15f) / (grab - 0.15f);
    drawBall(g, 56 - 26 * s, FEET_Y - 16 + 2 * s - 4 * 6 * s * (1 - s), 1, false);
  }
  // A grey chaser falling behind.
  if (t > grab) {
    drawFigure(g, 44 + (t - grab) * 20, FEET_Y, t * 16, OPPONENT, -1, false, false);
  }

  // Defender: jumps the route, snatches it, runs it back, then holds the ball up.
  if (t < grab) {
    const float lift = t > 0.45f ? 4 * sinf((t - 0.45f) / 0.2f * PI_F) : 0;
    drawFigure(g, 18 + 12 * easeOut(t / grab), FEET_Y - lift, t * 18, theme, 1, t > 0.45f, false);
  } else {
    const float a = t - grab;
    sparkBurst(g, 30, FEET_Y - 16, a, seed, theme, 24, 0.7f);
    flash = fmaxf(flash, a < 0.1f ? 0.7f * (1 - a / 0.1f) : 0);
    float x = 30;
    if (t > 2.15f) {
      x = fmaxf(16, 30 - (t - 2.15f) * 60);
    }
    if (t < 2.45f) {
      // High-stepping as he crosses the goal line.
      const float step = t > 1.9f ? fabsf(sinf(t * 12)) * 2 : 0;
      drawFigure(g, x, FEET_Y - step, t * 18, theme, -1, false, true);
    } else {
      drawFigure(g, x, FEET_Y, 0, theme, -1, true, false);
      drawBall(g, x - 3, FEET_Y - 21, 1, static_cast<int>(t * 6) & 1);
    }
  }

  if (t > 2.0f) {
    const float a = t - 2.0f;
    const float pop = a < 0.15f ? a / 0.15f : 1;
    const bool beat = static_cast<int>(a / 0.3f) & 1;
    drawCentered(g, "TO THE", ri(16 - 6 * (1 - pop)), 2, beat ? WHITE : theme);
    drawCentered(g, "HOUSE!", ri(32 - 6 * (1 - pop)), 2, beat ? theme : WHITE);
  }
  wash(g, flash);
}

// Touchdown of no known kind: end-zone stripes, the ball spirals in and explodes, "TOUCH" and
// "DOWN!" slam together, fireworks behind.
void drawTouchdown(GFXcanvas16 &g, float t, uint32_t seed, uint16_t theme, Play play)
{
  const uint16_t stripe = scale565(theme, 0.22f);
  const int scroll = static_cast<int>(t * 14);
  uint16_t *buf = g.getBuffer();
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      if (((x + y + scroll) % 12) < 5) buf[y * W + x] = stripe;
    }
  }

  const float boom = 0.55f;
  float flash = 0;
  if (t < boom) {
    // Ball spiralling in from the corner, growing.
    const float s = t / boom;
    const float ang = s * 5;
    const float r = (1 - s) * 22;
    drawBall(g, 32 - (1 - s) * 30 + cosf(ang) * r * 0.3f, 32 + (1 - s) * 30 + sinf(ang) * r * 0.3f, 1 + 2 * s,
             static_cast<int>(t * 16) & 1);
  } else {
    const float age = t - boom;
    flash = fmaxf(flash, age < 0.12f ? 1 - age / 0.12f : 0);
    sparkBurst(g, 32, 32, age, seed * 7 + 1, theme, 40, 1.1f);
    flash = fmaxf(flash, fireworks(g, age - 0.3f, seed, theme, 8, 0.3f));

    // TOUCH from the left, DOWN! from the right, meeting in the middle.
    const float in = easeOut(age / 0.3f);
    const int w = textWidth("TOUCH", 2);
    const int home = (W - w) / 2;
    const int xt = ri(-w + (home + w) * in);
    const int xd = ri(W + (home - W) * in);
    const float hue = t * 0.9f;
    const int jolt = age > 0.3f && age < 0.45f ? ri(2 * sinf(age * 90)) : 0;
    if (age < 0.3f) {
      drawText(g, "TOUCH", xt, 22, 2, WHITE);
      drawText(g, "DOWN!", xd, 40, 2, WHITE);
    } else {
      drawRainbow(g, "TOUCH", xt + jolt, 22, 2, hue);
      drawRainbow(g, "DOWN!", xd - jolt, 40, 2, hue + 0.5f);
    }
    if (age > 0.45f) {
      const char *kind = play == PLAY_RUSH ? "RUSHING" : play == PLAY_PASS ? "PASSING"
                       : play == PLAY_CATCH ? "RECEIVING" : play == PLAY_DEFENSE ? "DEFENSE" : "";
      if (kind[0] && (static_cast<int>(age * 4) & 1 || age > 1.5f)) {
        drawCentered(g, kind, 56, 1, WHITE);
      }
    }
  }
  wash(g, flash);
}

}  // namespace

void drawCelebration(GFXcanvas16 &g, uint32_t t_ms, uint32_t seed, uint16_t theme, Play play, bool touchdown)
{
  g.fillScreen(0);
  const float t = t_ms / 1000.0f;
  if (touchdown) {
    switch (play) {
      case PLAY_RUSH: drawRushTD(g, t, seed, theme); break;
      case PLAY_PASS: drawPassTD(g, t, seed, theme); break;
      case PLAY_CATCH: drawCatchTD(g, t, seed, theme); break;
      case PLAY_DEFENSE: drawDefTD(g, t, seed, theme); break;
      default: drawTouchdown(g, t, seed, theme, play); break;
    }
  } else {
    switch (play) {
      case PLAY_RUSH: drawRush(g, t, seed, theme); break;
      case PLAY_PASS: drawPass(g, t, seed, theme, false); break;
      case PLAY_CATCH: drawPass(g, t, seed, theme, true); break;
      case PLAY_DEFENSE: drawDefense(g, t, seed, theme); break;
      case PLAY_KICK: drawKick(g, t, seed, theme); break;
      default: wash(g, fireworks(g, t, seed, theme, 6, 0.3f)); break;
    }
  }
  g.setTextSize(1);
}
