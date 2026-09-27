#pragma once

#include <Adafruit_GFX.h>

// What kind of play earned the points; picks the celebration.
enum Play : uint8_t {
  PLAY_OTHER,     // fireworks
  PLAY_RUSH,      // ball carrier sprinting down the field, "RUSH!"
  PLAY_PASS,      // spiral from the QB to a receiver, "COMPLETE!"
  PLAY_CATCH,     // same throw, "CAUGHT IT!"
  PLAY_DEFENSE,   // hit, ball knocked loose, D + fence
  PLAY_KICK,      // ball through the uprights, "GOOD!"
};

// Full-screen celebration played before the update screen when a player gains points.
// Touchdowns get their own, longer scene per kind of play:
//   rush     sprint, dive over the goal line, spike, "TOUCHDOWN!" scrolling across the sky
//   pass     the QB's bomb leaves the screen and drops into the end zone, rainbow arc, "DIME!"
//   catch    toe-tap catch in the end zone, leap into the stands, "SIX!"
//   defense  jumped route, return the length of the field, "TO THE HOUSE!"
constexpr uint32_t CELEBRATE_MS = 3000;
constexpr uint32_t TOUCHDOWN_MS = 3500;

inline uint32_t celebrationMs(bool touchdown)
{
  return touchdown ? TOUCHDOWN_MS : CELEBRATE_MS;
}

// Draws frame t (ms since start) into the 64x64 canvas. `seed` varies random details from one
// event to the next; `theme` is the position colour (RGB565).
void drawCelebration(GFXcanvas16 &g, uint32_t t, uint32_t seed, uint16_t theme, Play play, bool touchdown);
