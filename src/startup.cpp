#include "startup.h"

#include <cmath>
#include <cstring>

namespace {

constexpr int W = 64;
constexpr int H = 64;
constexpr float TWO_PI_F = 6.2831853f;

// Phase lengths, ms.
constexpr uint32_t KICK_MS = 1800;
constexpr uint32_t PINWHEEL_MS = 3000;
constexpr uint32_t MESSAGE_MS = 4300;
constexpr uint32_t PINWHEEL_START = KICK_MS;
constexpr uint32_t MESSAGE_START = KICK_MS + PINWHEEL_MS;
constexpr uint32_t TOTAL_MS = MESSAGE_START + MESSAGE_MS;

// Goalposts (kick scene).
constexpr int POST_LEFT = 46;
constexpr int POST_RIGHT = 62;
constexpr int CROSSBAR_Y = 30;
constexpr int UPRIGHT_TOP = 4;
constexpr int GRASS_Y = 58;

// Ball path: x runs linearly, y is a parabola; it clears the crossbar between the uprights.
constexpr float BALL_X0 = 4.0f, BALL_X1 = 58.0f;
constexpr float BALL_Y0 = 56.0f, BALL_Y1 = 17.0f;

inline uint16_t rgb565(int r, int g, int b)
{
  r = r < 0 ? 0 : (r > 255 ? 255 : r);
  g = g < 0 ? 0 : (g > 255 ? 255 : g);
  b = b < 0 ? 0 : (b > 255 ? 255 : b);
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

inline void unpack565(uint16_t c, int &r, int &g, int &b)
{
  r = (c >> 8) & 0xF8;
  g = (c >> 3) & 0xFC;
  b = (c << 3) & 0xF8;
}

// h in turns (any value, wraps), v 0..1, full saturation.
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

inline float easeOut(float s) { return 1 - (1 - s) * (1 - s); }

void ballAt(float s, float &x, float &y)
{
  x = BALL_X0 + (BALL_X1 - BALL_X0) * s;
  // y = y0 + (y1 - y0) * s - lift * s * (1 - s): rises fast, flattens out by the posts.
  y = BALL_Y0 + (BALL_Y1 - BALL_Y0) * s - 30.0f * s * (1 - s);
}

// A football: a rotated ellipse with a white lace stripe.
void drawBall(GFXcanvas16 &g, float cx, float cy, float angle, float a, float b, float bright)
{
  const float c = cosf(angle), s = sinf(angle);
  const int r = static_cast<int>(ceilf(a)) + 1;
  for (int y = static_cast<int>(cy) - r; y <= static_cast<int>(cy) + r; ++y) {
    for (int x = static_cast<int>(cx) - r; x <= static_cast<int>(cx) + r; ++x) {
      const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      const float u = dx * c + dy * s;
      const float v = -dx * s + dy * c;
      if ((u * u) / (a * a) + (v * v) / (b * b) > 1.0f) {
        continue;
      }
      const bool lace = fabsf(v) < 0.6f && fabsf(u) < a * 0.5f;
      if (lace) {
        g.drawPixel(x, y, rgb565(static_cast<int>(255 * bright), static_cast<int>(255 * bright), static_cast<int>(255 * bright)));
      } else {
        g.drawPixel(x, y, rgb565(static_cast<int>(185 * bright), static_cast<int>(95 * bright), static_cast<int>(40 * bright)));
      }
    }
  }
}

void drawField(GFXcanvas16 &g, float fade)
{
  for (int y = GRASS_Y; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const bool stripe = ((x / 8) & 1) == 0;
      g.drawPixel(x, y, stripe ? rgb565(0, static_cast<int>(120 * fade), 0) : rgb565(0, static_cast<int>(80 * fade), 0));
    }
  }
  const uint16_t post = rgb565(static_cast<int>(255 * fade), static_cast<int>(215 * fade), 0);
  g.drawFastVLine((POST_LEFT + POST_RIGHT) / 2, CROSSBAR_Y, GRASS_Y - CROSSBAR_Y, post);
  g.drawFastHLine(POST_LEFT, CROSSBAR_Y, POST_RIGHT - POST_LEFT + 1, post);
  g.drawFastVLine(POST_LEFT, UPRIGHT_TOP, CROSSBAR_Y - UPRIGHT_TOP, post);
  g.drawFastVLine(POST_RIGHT, UPRIGHT_TOP, CROSSBAR_Y - UPRIGHT_TOP, post);
}

void drawKick(GFXcanvas16 &g, uint32_t t)
{
  drawField(g, 1.0f);
  const float s = clamp01(t / static_cast<float>(KICK_MS));
  // Fading trail of earlier positions.
  for (int k = 4; k >= 1; --k) {
    const float sk = s - k * 0.035f;
    if (sk <= 0) {
      continue;
    }
    float x, y;
    ballAt(sk, x, y);
    const int v = 90 - k * 18;
    g.drawPixel(static_cast<int>(x), static_cast<int>(y), rgb565(v, v / 2, v / 4));
  }
  float x, y;
  ballAt(s, x, y);
  // The ball shrinks a little as it flies away.
  drawBall(g, x, y, s * 3.5f * TWO_PI_F, 4.2f - 1.4f * s, 2.7f - 0.8f * s, 1.0f);
}

// Rainbow pinwheel: 8 arms whose hue follows the angle, rotating and curling (twist) over time.
// Colour of the pinwheel at pixel offset (dx, dy) from its centre, or 0 outside `radius`.
uint16_t pinwheelColor(float dx, float dy, float radius, float spin, float twist, float hue_shift)
{
  constexpr int ARMS = 8;
  const float r = sqrtf(dx * dx + dy * dy);
  if (r > radius) {
    return 0;
  }
  const float a = atan2f(dy, dx) + spin - r * twist;
  const float turns = a / TWO_PI_F;
  const float arm = turns * ARMS - floorf(turns * ARMS);   // 0..1 across one arm
  // Each arm is a wedge that is brightest along its leading edge.
  const float v = 0.25f + 0.75f * (1 - arm);
  const float edge = clamp01((radius - r) / 3.0f);          // soft rim
  return hsv(floorf(turns * ARMS) / ARMS + hue_shift, v * edge);
}

// Open (first 20 %), spin up (to 60 %), then dissolve: pixels break off from the rim inward and
// fly out and down as confetti, most of them vanishing, until only scattered specks are left.
void drawPinwheel(GFXcanvas16 &g, uint32_t t)
{
  constexpr float OPEN_END = 0.2f;
  constexpr float DISSOLVE_START = 0.6f;
  constexpr float MAX_RADIUS = 48.0f;
  const float s = t / static_cast<float>(PINWHEEL_MS);
  float bx, by;
  ballAt(1.0f, bx, by);
  const float move = easeOut(clamp01(s / 0.3f));   // drift from the ball to the centre
  const float cx = bx + (W / 2.0f - bx) * move;
  const float cy = by + (H / 2.0f - by) * move;
  const float radius = s < OPEN_END ? MAX_RADIUS * easeOut(s / OPEN_END) : MAX_RADIUS;

  // Rotation accelerates, and the arms curl more as it speeds up.
  auto spinAt = [](float u) { return u * u * 5.0f * TWO_PI_F; };
  auto twistAt = [](float u) { return 0.02f + 0.10f * u; };

  if (s < DISSOLVE_START) {
    const float spin = spinAt(s), twist = twistAt(s), hue = s * 1.5f;
    for (int y = 0; y < H; ++y) {
      for (int x = 0; x < W; ++x) {
        const uint16_t c = pinwheelColor(x + 0.5f - cx, y + 0.5f - cy, radius, spin, twist, hue);
        if (c) {
          g.drawPixel(x, y, c);
        }
      }
    }
    if (radius > 3) {
      g.fillCircle(static_cast<int>(cx), static_cast<int>(cy), 2, rgb565(255, 255, 255));   // hub
    }
    return;
  }

  // Dissolve: d runs 0..1. The pattern keeps its colours from the moment the dissolve started.
  const float d = (s - DISSOLVE_START) / (1 - DISSOLVE_START);
  const float seconds = (1 - DISSOLVE_START) * PINWHEEL_MS / 1000.0f;
  const float spin = spinAt(DISSOLVE_START) + d * 2.0f, twist = twistAt(DISSOLVE_START), hue = DISSOLVE_START * 1.5f;
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      const float dx = x + 0.5f - cx, dy = y + 0.5f - cy;
      const uint16_t c = pinwheelColor(dx, dy, radius, spin, twist, hue);
      if (!c) {
        continue;
      }
      const uint32_t id = static_cast<uint32_t>(y * W + x);
      const float r = sqrtf(dx * dx + dy * dy);
      const float breaks = 0.5f * (1 - r / MAX_RADIUS) + 0.3f * hashf(id);   // rim first
      if (d < breaks) {
        g.drawPixel(x, y, c);
        continue;
      }
      if (hashf(id + 7777) > 0.2f) {
        continue;   // most pixels vanish; one in five becomes a flying speck
      }
      const float age = (d - breaks) * seconds;
      const float inv = r > 0.1f ? 1 / r : 0;
      const float speed = 12.0f + 26.0f * hashf(id + 999);
      const float px = x + dx * inv * speed * age;
      const float py = y + dy * inv * speed * age + 40.0f * age * age;   // gravity
      g.drawPixel(static_cast<int>(px), static_cast<int>(py), c);
    }
  }
}

struct Line {
  const char *text;
  int y;
  uint8_t size;
  uint32_t at;        // ms into the message phase
  uint8_t r, g, b;    // 0 = rainbow
};

const Line LINES[] = {
    {"GOOD", 3, 1, 0, 255, 200, 40},
    {"AFTERNOON", 15, 1, 450, 255, 120, 80},
    {"CHAMPIONS", 27, 1, 900, 220, 120, 255},
    {"!!!", 41, 2, 1350, 0, 0, 0},
};

void drawMessage(GFXcanvas16 &g, uint32_t t)
{
  g.setFont(nullptr);
  g.setTextWrap(false);
  for (const Line &line : LINES) {
    if (t < line.at) {
      continue;
    }
    // Each line drops into place over 150 ms.
    const float drop = clamp01((t - line.at) / 150.0f);
    const int y = line.y - static_cast<int>((1 - drop) * 6);
    const int width = static_cast<int>(strlen(line.text)) * 6 * line.size - line.size;
    const int x0 = (W - width) / 2;
    g.setTextSize(line.size);
    if (line.r || line.g || line.b) {
      g.setTextColor(rgb565(line.r, line.g, line.b));
      g.setCursor(x0, y);
      g.print(line.text);
    } else {
      // Rainbow, one hue per character, cycling.
      for (size_t i = 0; i < strlen(line.text); ++i) {
        g.setTextColor(hsv(i / 3.0f + t / 900.0f, 1.0f));
        g.setCursor(x0 + static_cast<int>(i) * 6 * line.size, y);
        g.print(line.text[i]);
      }
    }
  }
  g.setTextSize(1);

  // Shimmer: a diagonal band sweeping left to right every 1.4 s brightens the text toward white.
  const float band = fmodf(t / 1400.0f, 1.0f) * 110.0f - 25.0f;
  uint16_t *buf = g.getBuffer();
  for (int y = 0; y < H; ++y) {
    for (int x = 0; x < W; ++x) {
      uint16_t &c = buf[y * W + x];
      if (!c) {
        continue;
      }
      const float d = fabsf(x + y * 0.5f - band);
      if (d < 6.0f) {
        const float k = (1 - d / 6.0f) * 0.8f;
        int r, gg, b;
        unpack565(c, r, gg, b);
        c = rgb565(static_cast<int>(r + (255 - r) * k), static_cast<int>(gg + (255 - gg) * k),
                   static_cast<int>(b + (255 - b) * k));
      }
    }
  }

  // Confetti falling behind the text (only drawn on empty pixels).
  constexpr int CONFETTI = 44;
  for (int i = 0; i < CONFETTI; ++i) {
    const float speed = 14.0f + 22.0f * hashf(i * 3 + 1);           // px per second
    const float start = hashf(i * 3 + 2) * 70.0f;
    const float yf = fmodf(start + speed * t / 1000.0f, 72.0f) - 6.0f;
    const float xf = hashf(i * 3 + 3) * W + 2.0f * sinf(t / 300.0f + i);
    const int x = static_cast<int>(xf), y = static_cast<int>(yf);
    if (x < 0 || x >= W || y < 0 || y >= H) {
      continue;
    }
    uint16_t &c = buf[y * W + x];
    if (!c) {
      c = hsv(hashf(i * 7 + 5), 0.9f);
    }
  }
}

}  // namespace

bool drawStartup(GFXcanvas16 &g, uint32_t t)
{
  if (t >= TOTAL_MS) {
    return false;
  }
  g.fillScreen(0);
  if (t < PINWHEEL_START) {
    drawKick(g, t);
  } else if (t < MESSAGE_START) {
    drawPinwheel(g, t - PINWHEEL_START);
  } else {
    drawMessage(g, t - MESSAGE_START);
  }
  return true;
}
