#pragma once

#include <Adafruit_GFX.h>

// Startup animation, about 9 s, drawn while WiFi connects:
//   1. Kick: a spinning football flies from the bottom-left through the goalposts.
//   2. Pinwheel: a rainbow pinwheel spins open where the ball went, drifts to the centre and spins
//      faster with the arms curling, then dissolves into confetti from the rim inward.
//   3. Message: "GOOD / AFTERNOON / CHAMPIONS / !!!" appears line by line, with a shimmer sweeping
//      across the text and confetti still falling behind it.
// Draws frame t (ms since start) into the 64x64 canvas; returns false once the animation is over.
bool drawStartup(GFXcanvas16 &g, uint32_t t);
