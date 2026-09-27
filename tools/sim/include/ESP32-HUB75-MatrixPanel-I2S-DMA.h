#pragma once
#include <Adafruit_GFX.h>
struct HUB75_I2S_CFG {
  enum Driver { SHIFTREG, FM6126A };
  struct { int e; } gpio;
  bool clkphase;
  Driver driver;
  HUB75_I2S_CFG(int, int, int) {}
};
struct MatrixPanel_I2S_DMA {
  explicit MatrixPanel_I2S_DMA(const HUB75_I2S_CFG &) {}
  bool begin() { return true; }
  void clearScreen() {}
  void setBrightness8(int) {}
  void drawPixel(int, int, uint16_t) {}
};
