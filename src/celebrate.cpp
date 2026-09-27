#include "celebrate.h"

#include <cmath>

namespace {

constexpr int W = 64;
constexpr int H = 64;
constexpr float TWO_PI_F = 6.2831853f;
constexpr int BURSTS = 4;
constexpr int SPARKS = 36;          // per burst
constexpr float ROCKET_S = 0.35f;   // rise time before a burst
constexpr float BURST_S = 1.0f;     // spark life after a burst

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

}  // namespace

void drawCelebration(GFXcanvas16 &g, uint32_t t_ms, uint32_t seed, uint16_t theme)
{
  g.fillScreen(0);
  const float t = t_ms / 1000.0f;
  float flash = 0;

  for (int b = 0; b < BURSTS; ++b) {
    const uint32_t id = seed * 131 + b * 17;
    const float start = b * 0.22f;                       // bursts staggered so the last ends at ~2 s
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
      flash = fmaxf(flash, 1 - age / 0.08f);
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

  // Brief white wash over the screen at each burst.
  if (flash > 0) {
    uint16_t *buf = g.getBuffer();
    const int add = static_cast<int>(flash * 60);
    for (int i = 0; i < W * H; ++i) {
      int r, gg, bb;
      unpack565(buf[i], r, gg, bb);
      buf[i] = rgb565(r + add, gg + add, bb + add);
    }
  }
}
