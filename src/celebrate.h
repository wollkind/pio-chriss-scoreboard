#pragma once

#include <Adafruit_GFX.h>

// Full-screen celebration, CELEBRATE_MS long: fireworks bursting in the player's position colour
// and rainbow sparks, with a white flash at each burst. Played before the update screen when a
// player gains points.
constexpr uint32_t CELEBRATE_MS = 2000;

// Draws frame t (ms since start) into the 64x64 canvas. `seed` varies the burst positions from one
// event to the next; `theme` is the position colour (RGB565).
void drawCelebration(GFXcanvas16 &g, uint32_t t, uint32_t seed, uint16_t theme);
